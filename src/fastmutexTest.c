#ifdef DATAKIT_TEST
#include "fastmutex.h"

#include "ctest.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

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

int fastmutexTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

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
