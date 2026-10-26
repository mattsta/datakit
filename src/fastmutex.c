#include "fastmutex.h"

/* Adapted from:
 * https://github.com/jemalloc/jemalloc/pull/1516
 */

#include <errno.h>  /* ETIMEDOUT */
#include <unistd.h> /* usleep */

struct fastWaiter {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool waiting;
    fastWaiter *next;
    fastWaiter *tail;
};

/* A waiter's address shares the mutex word with the flag bits */
static_assert(alignof(fastWaiter) > FAST_MUTEX_FLAGS,
              "waiter addresses leave the flag bits clear");

static void fast_waiter_init(fastWaiter *w) {
    pthread_mutex_init(&w->lock, nullptr);
    pthread_cond_init(&w->cond, nullptr);
    w->waiting = true;
    w->next = nullptr;
    w->tail = nullptr;
}

static void fast_waiter_destroy(fastWaiter *w) {
    // cppcheck-suppress intToPointerCast - intentional poison value to detect
    // use-after-destroy
    w->next = (void *)1;
    // cppcheck-suppress intToPointerCast - intentional poison value to detect
    // use-after-destroy
    w->tail = (void *)1;
    pthread_mutex_destroy(&w->lock);
    pthread_cond_destroy(&w->cond);
}

void fastMutexLockSlow(fastMutex *m) {
    uint64_t sleeptime = 1;
    while (true) {
        /* @sync: relaxed seed */
        uint64_t prev = atomic_load_explicit(&m->word, memory_order_relaxed);
        if (!(prev & FAST_MUTEX_HELD) &&
            /* @sync: acquire fastmutex.lock */
            atomic_compare_exchange_weak_explicit(
                &m->word, &prev, prev | FAST_MUTEX_HELD, memory_order_acquire,
                memory_order_relaxed)) {
            return;
        }

        while (fastMutexIsLocked(m)) {
            if (sleeptime++ > 40) {
                usleep(100);
            }

            if (sleeptime > 100) {
                break;
            }
        }

        /* @sync: relaxed seed */
        prev = atomic_load_explicit(&m->word, memory_order_relaxed);
        if (!(prev & FAST_MUTEX_HELD)) {
            continue;
        }

        if (prev & FAST_MUTEX_LIST_LOCK) {
            continue;
        }

        uint64_t expected = prev | FAST_MUTEX_LIST_LOCK;
        /* The list nodes the list lock guards were last written by the
         * previous holder before its release store. */
        /* @sync: acquire fastmutex.queue */
        if (atomic_compare_exchange_weak_explicit(&m->word, &prev, expected,
                                                  memory_order_acquire,
                                                  memory_order_relaxed)) {
            // Locked for access
            fastWaiter waiter;
            fast_waiter_init(&waiter);

            fastWaiter *w = (fastWaiter *)(prev & ~FAST_MUTEX_FLAGS);
            if (w) {
                fastWaiter *last = w->tail;
                last->next = &waiter;
                w->tail = &waiter;
            } else {
                w = &waiter;
                waiter.tail = &waiter;
            }

            /* Drops the list lock; the mutex stays held. */
            /* @sync: release fastmutex.queue */
            atomic_store_explicit(&m->word, (uint64_t)w | FAST_MUTEX_HELD,
                                  memory_order_release);
            // sleep wait
            pthread_mutex_lock(&waiter.lock);
            while (waiter.waiting) {
                pthread_cond_wait(&waiter.cond, &waiter.lock);
            }

            pthread_mutex_unlock(&waiter.lock);
            fast_waiter_destroy(&waiter);
        }
    }
}

void fastMutexUnlockSlow(fastMutex *m) {
    /* @sync: relaxed seed */
    uint64_t w = atomic_load_explicit(&m->word, memory_order_relaxed);
    assert(w & FAST_MUTEX_HELD);
    while (true) {
        while (w & FAST_MUTEX_LIST_LOCK) {
            /* @sync: relaxed seed */
            w = atomic_load_explicit(&m->word, memory_order_relaxed);
        }

        assert(!(w & FAST_MUTEX_LIST_LOCK));
        /* @sync: acquire fastmutex.queue */
        if (atomic_compare_exchange_weak_explicit(
                &m->word, &w, w | FAST_MUTEX_LIST_LOCK, memory_order_acquire,
                memory_order_relaxed)) {
            break;
        }
    }

    assert(w & FAST_MUTEX_HELD);
    fastWaiter *waiter = (fastWaiter *)(w & ~FAST_MUTEX_FLAGS);
    assert(waiter);
    fastWaiter *next_waiter = waiter->next;
    if (next_waiter) {
        next_waiter->tail = waiter->tail;
    } else {
        assert(waiter->tail == waiter);
    }

    // Note that someone else can still lock/unlock here, so this has to
    // cas.  We have the list locked though, so the list cannot change
    // release lock, release queue lock.  This store is the unlock: it must
    // be a release so the critical section's writes happen-before the next
    // owner's acquire (a relaxed store let them be seen late on weakly
    // ordered CPUs).
    /* @sync: release fastmutex.lock; release fastmutex.queue */
    atomic_store_explicit(&m->word, (uint64_t)next_waiter,
                          memory_order_release);
    pthread_mutex_lock(&waiter->lock);
    waiter->waiting = false;
    pthread_cond_signal(&waiter->cond);
    pthread_mutex_unlock(&waiter->lock);
}

int fastMutexCondWait(fastCond *c, fastMutex *m) {
    return fastMutexCondTimedWait(c, m, nullptr);
}

int fastMutexCondTimedWait(fastCond *c, fastMutex *m, struct timespec *ts) {
    int ret = 0;
    fastWaiter waiter;
    fast_waiter_init(&waiter);

    pthread_mutex_lock(&c->m);
    if (!c->w) {
        // cppcheck-suppress autoVariables
        c->w = &waiter; /* Safe: waiter removed from queue before return */
        waiter.tail = &waiter;
    } else {
        fastWaiter *last = c->w->tail;
        last->next = &waiter;
        c->w->tail = &waiter;
    }

    pthread_mutex_unlock(&c->m);

    fastMutexUnlock(m);

    pthread_mutex_lock(&waiter.lock);
    while (waiter.waiting && ret != ETIMEDOUT) {
        if (ts) {
            ret = pthread_cond_timedwait(&waiter.cond, &waiter.lock, ts);
        } else {
            pthread_cond_wait(&waiter.cond, &waiter.lock);
        }
    }

    pthread_mutex_unlock(&waiter.lock);

    // Try to remove us from queue if we timedout.
    bool removefail = false;
    if (ret == ETIMEDOUT) {
        pthread_mutex_lock(&c->m);
        fastWaiter **last = &c->w;
        fastWaiter *lasttail = c->w;
        fastWaiter *head = c->w;
        while (head && head != &waiter) {
            lasttail = head;
            last = &head->next;
            head = head->next;
        }

        if (head) {
            assert(head == &waiter);
            *last = waiter.next;
            /* Only the head carries `tail`: a removed head hands it to
             * its successor, and a removed last node moves it back. */
            if (last == &c->w && waiter.next) {
                waiter.next->tail = waiter.tail;
            } else if (c->w && waiter.next == nullptr) {
                c->w->tail = lasttail;
            }
        } else {
            removefail = true;
        }

        pthread_mutex_unlock(&c->m);
    }

    if (removefail) {
        ret = 0; // Couldn't find our node, wait to be notified.
        pthread_mutex_lock(&waiter.lock);
        while (waiter.waiting) {
            pthread_cond_wait(&waiter.cond, &waiter.lock);
        }

        pthread_mutex_unlock(&waiter.lock);
    }

    fast_waiter_destroy(&waiter);
    fast_mutex_lock(m);
    return ret;
}

void fastMutexCondSignal(fastCond *c) {
    fastWaiter *waiter;

    pthread_mutex_lock(&c->m);
    waiter = c->w;
    if (waiter) {
        if (waiter->next) {
            waiter->next->tail = waiter->tail;
        }

        c->w = waiter->next;
        if (c->w) {
            assert(c->w->tail == waiter->tail);
        }
    }

    pthread_mutex_unlock(&c->m);

    if (!waiter) {
        return;
    }

    pthread_mutex_lock(&waiter->lock);
    waiter->waiting = false;
    pthread_cond_signal(&waiter->cond);
    pthread_mutex_unlock(&waiter->lock);
}
