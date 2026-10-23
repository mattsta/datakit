#ifdef DATAKIT_TEST
#include "fastmutex.h"

#include "ctest.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define FM_THREADS 8
#define FM_ITERS 100000

typedef struct fmWorkerArg {
    fastMutex *m;
    uint64_t *counter;
} fmWorkerArg;

static void *fmWorker(void *arg) {
    fmWorkerArg *a = (fmWorkerArg *)arg;
    for (int i = 0; i < FM_ITERS; i++) {
        fast_mutex_lock(a->m);
        (*a->counter)++;
        fastMutexUnlock(a->m);
    }
    return NULL;
}

/* Block in fast_mutex_lock until the holder lets go */
typedef struct fmBlocked {
    fastMutex *m;
    _Atomic bool acquired;
} fmBlocked;

static void *fmBlockedLocker(void *arg) {
    fmBlocked *b = arg;
    fast_mutex_lock(b->m);
    /* @sync: relaxed owner */
    atomic_store_explicit(&b->acquired, true, memory_order_relaxed);
    fastMutexUnlock(b->m);
    return NULL;
}

/* Wait on a condition until 'ready' */
typedef struct fmCondWaiter {
    fastMutex *m;
    fastCond *c;
    bool ready;    /* guarded by m */
    bool sawReady; /* written by the waiter, read after join */
    /* Set while holding m just before the first wait.  CondWait queues
     * the waiter before it releases m, so a thread that sees this set
     * and then takes m knows the waiter is queued. */
    _Atomic bool entered;
} fmCondWaiter;

static void *fmCondWaiterThread(void *arg) {
    fmCondWaiter *w = arg;
    fast_mutex_lock(w->m);
    /* @sync: release fmTest.entered */
    atomic_store_explicit(&w->entered, true, memory_order_release);
    while (!w->ready) {
        fastMutexCondWait(w->c, w->m);
    }
    w->sawReady = w->ready && fastMutexIsLocked(w->m);
    fastMutexUnlock(w->m);
    return NULL;
}

/* Poll (1 ms steps, up to 10 s) until 'done' reports true */
static bool fmPollUntil(bool (*done)(void *), void *arg) {
    for (int i = 0; i < 10000; i++) {
        if (done(arg)) {
            return true;
        }
        usleep(1000);
    }
    return false;
}

static bool fmHasWaiter(void *arg) {
    const fastMutex *m = arg;
    /* @sync: relaxed hint */
    return (atomic_load_explicit(&m->word, memory_order_relaxed) &
            ~(uint64_t)3) != 0;
}

static bool fmCondEntered(void *arg) {
    fmCondWaiter *w = arg;
    /* @sync: acquire fmTest.entered */
    return atomic_load_explicit(&w->entered, memory_order_acquire);
}

/* Lock m, then wait on c with a deadline */
typedef struct fmTimedWaiter {
    fastMutex *m;
    fastCond *c;
    struct timespec deadline;
    int ret; /* read after join */
} fmTimedWaiter;

static void *fmTimedWaiterThread(void *arg) {
    fmTimedWaiter *w = arg;
    fast_mutex_lock(w->m);
    w->ret = fastMutexCondTimedWait(w->c, w->m, &w->deadline);
    fastMutexUnlock(w->m);
    return NULL;
}

/* Take and drop m: once a waiter has been seen `entered`, this returns
 * only after that waiter is queued. */
static void fmSettle(fastMutex *m) {
    fast_mutex_lock(m);
    fastMutexUnlock(m);
}

static bool fmCondHasWaiter(void *arg) {
    fastCond *c = arg;
    pthread_mutex_lock(&c->m);
    const bool queued = c->w != NULL;
    pthread_mutex_unlock(&c->m);
    return queued;
}

int fastmutexTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    TEST("fastMutex: a queued waiter is woken by the slow unlock") {
        /* The holder keeps the lock until the other thread has queued a
         * waiter in the lock word, so the unlock must take the slow path
         * and wake it */
        fastMutex m;
        fastMutexInit(&m);
        fmBlocked b = {.m = &m};
        fast_mutex_lock(&m);
        pthread_t th;
        pthread_create(&th, NULL, fmBlockedLocker, &b);
        if (!fmPollUntil(fmHasWaiter, &m)) {
            ERRR("the blocked locker never queued");
        }
        /* @sync: relaxed owner */
        const bool earlyAcquire =
            atomic_load_explicit(&b.acquired, memory_order_relaxed);
        fastMutexUnlock(&m);
        pthread_join(th, NULL);
        /* @sync: relaxed init */
        if (earlyAcquire ||
            !atomic_load_explicit(&b.acquired, memory_order_relaxed) ||
            fastMutexIsLocked(&m)) {
            ERRR("the queued waiter did not get the lock exactly once");
        }
    }

    TEST("fastMutex: condition wait, signal and a timed-out wait") {
        fastMutex m;
        fastCond c;
        fastMutexInit(&m);
        fastMutexCondInit(&c, NULL);
        fmCondWaiter w = {.m = &m, .c = &c};
        pthread_t th;
        pthread_create(&th, NULL, fmCondWaiterThread, &w);
        if (!fmPollUntil(fmCondHasWaiter, &c)) {
            ERRR("the condition waiter never queued");
        }
        fast_mutex_lock(&m);
        w.ready = true;
        fastMutexCondSignal(&c);
        fastMutexUnlock(&m);
        pthread_join(th, NULL);
        if (!w.sawReady) {
            ERRR("the waiter did not wake holding the mutex with ready set");
        }

        /* A deadline already past: ETIMEDOUT, the mutex held again and
         * no waiter left queued; a signal with no waiter is a no-op */
        struct timespec past = {.tv_sec = 1, .tv_nsec = 0};
        fast_mutex_lock(&m);
        const int timed = fastMutexCondTimedWait(&c, &m, &past);
        const bool relocked = fastMutexIsLocked(&m);
        fastMutexUnlock(&m);
        fastMutexCondSignal(&c);
        if (timed != ETIMEDOUT || !relocked || fmCondHasWaiter(&c)) {
            ERR("timed wait returned %d, relocked %d", timed, relocked);
        }
        pthread_mutex_destroy(&c.m);
    }

    TEST("fastMutex: a timed-out head keeps the condition queue intact") {
        /* A waits with a deadline at the head of the queue, B queues
         * behind it, A times out and unlinks itself; then C queues behind
         * B.  The head carries the queue's tail, so C's enqueue needs B to
         * have inherited it.  Both B and C must then wake on a signal. */
        fastMutex m;
        fastCond c;
        fastMutexInit(&m);
        fastMutexCondInit(&c, NULL);
        bool behind = false;
        for (int attempt = 0; attempt < 3 && !behind; attempt++) {
            fmTimedWaiter a = {.m = &m, .c = &c};
            clock_gettime(CLOCK_REALTIME, &a.deadline);
            a.deadline.tv_nsec += 300 * 1000 * 1000;
            if (a.deadline.tv_nsec >= 1000 * 1000 * 1000) {
                a.deadline.tv_sec++;
                a.deadline.tv_nsec -= 1000 * 1000 * 1000;
            }
            fmCondWaiter b = {.m = &m, .c = &c};
            fmCondWaiter cw = {.m = &m, .c = &c};
            pthread_t ta;
            pthread_t tb;
            pthread_t tc;
            pthread_create(&ta, NULL, fmTimedWaiterThread, &a);
            if (!fmPollUntil(fmCondHasWaiter, &c)) {
                ERRR("the timed waiter never queued");
            }
            pthread_create(&tb, NULL, fmCondWaiterThread, &b);
            if (!fmPollUntil(fmCondEntered, &b)) {
                ERRR("the second waiter never started waiting");
            }
            fmSettle(&m);
            /* B is queued behind A; it counts only if A had not yet
             * reached its deadline (A unlinks itself after it). */
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            behind = now.tv_sec < a.deadline.tv_sec ||
                     (now.tv_sec == a.deadline.tv_sec &&
                      now.tv_nsec < a.deadline.tv_nsec);
            pthread_join(ta, NULL);
            pthread_create(&tc, NULL, fmCondWaiterThread, &cw);
            if (!fmPollUntil(fmCondEntered, &cw)) {
                ERRR("the third waiter never started waiting");
            }
            fmSettle(&m);
            fast_mutex_lock(&m);
            b.ready = true;
            cw.ready = true;
            fastMutexCondSignal(&c);
            fastMutexCondSignal(&c);
            fastMutexUnlock(&m);
            pthread_join(tb, NULL);
            pthread_join(tc, NULL);
            if (a.ret != ETIMEDOUT || !b.sawReady || !cw.sawReady ||
                fmCondHasWaiter(&c)) {
                ERR("timed %d, woke B %d, woke C %d, queue left %d", a.ret,
                    b.sawReady, cw.sawReady, fmCondHasWaiter(&c));
            }
        }
        if (!behind) {
            ERRR("B never queued behind A before A's deadline");
        }
        pthread_mutex_destroy(&c.m);
    }

    TEST("fastMutex: trylock semantics (true on acquire, false when held)") {
        fastMutex m;
        fastMutexInit(&m);
        assert(!fastMutexIsLocked(&m));
        /* trylock on a free lock acquires it and returns true */
        assert(fastMutexTryLock(&m) == true);
        assert(fastMutexIsLocked(&m));
        /* trylock on an already-held lock returns false and does not acquire */
        assert(fastMutexTryLock(&m) == false);
        assert(fastMutexIsLocked(&m));
        fastMutexUnlock(&m);
        assert(!fastMutexIsLocked(&m));
        /* re-acquire after unlock works */
        assert(fastMutexTryLock(&m) == true);
        fastMutexUnlock(&m);
        assert(!fastMutexIsLocked(&m));
    }

    TEST("fastMutex: plain lock / unlock") {
        fastMutex m;
        fastMutexInit(&m);
        fast_mutex_lock(&m);
        assert(fastMutexIsLocked(&m));
        fastMutexUnlock(&m);
        assert(!fastMutexIsLocked(&m));
    }

    TEST("fastMutex: mutual exclusion under contention") {
        /* Exercises the slow-path waiter queue (fastMutexLockSlow/UnlockSlow):
         * if mutual exclusion holds, the shared counter equals the exact total
         * number of increments with no lost updates. */
        fastMutex m;
        fastMutexInit(&m);
        uint64_t counter = 0;
        fmWorkerArg arg = {&m, &counter};
        pthread_t th[FM_THREADS];
        for (int i = 0; i < FM_THREADS; i++) {
            assert(pthread_create(&th[i], NULL, fmWorker, &arg) == 0);
        }
        for (int i = 0; i < FM_THREADS; i++) {
            pthread_join(th[i], NULL);
        }
        assert(counter == (uint64_t)FM_THREADS * (uint64_t)FM_ITERS);
        assert(!fastMutexIsLocked(&m));
    }

    TEST_FINAL_RESULT;
}
#endif
