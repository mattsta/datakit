#pragma once

/* Adapted from:
 * https://github.com/jemalloc/jemalloc/pull/1516
 */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

/* The mutex word: two flag bits under the head of the waiter list */
static constexpr uint64_t FAST_MUTEX_HELD = 0b01;      /* the mutex is held */
static constexpr uint64_t FAST_MUTEX_LIST_LOCK = 0b10; /* waiter list locked */
static constexpr uint64_t FAST_MUTEX_FLAGS =
    FAST_MUTEX_HELD | FAST_MUTEX_LIST_LOCK;

typedef struct fastMutex {
    _Atomic uint64_t word;
} fastMutex;

typedef struct fastWaiter fastWaiter;

typedef struct fastCond {
    pthread_mutex_t m;
    fastWaiter *w;
} fastCond;

void fastMutexLockSlow(fastMutex *m);
void fastMutexUnlockSlow(fastMutex *m);

static inline void fast_mutex_lock(fastMutex *m) {
    uint64_t prev = 0;
    /* @sync: acquire fastmutex.lock */
    if (!atomic_compare_exchange_weak_explicit(&m->word, &prev, FAST_MUTEX_HELD,
                                               memory_order_acquire,
                                               memory_order_relaxed)) {
        fastMutexLockSlow(m);
    }
}

static inline void fastMutexUnlock(fastMutex *m) {
    uint64_t prev = FAST_MUTEX_HELD;
    /* @sync: release fastmutex.lock */
    if (!atomic_compare_exchange_strong_explicit(
            &m->word, &prev, 0, memory_order_release, memory_order_relaxed)) {
        assert(prev & FAST_MUTEX_HELD);
        fastMutexUnlockSlow(m);
    }
}

/* Try to acquire the lock without blocking.
 * Returns true if the lock was acquired, false if it was already held.
 * (Uses a strong CAS so a free lock is reliably acquired with no spurious
 * false negatives.) Only acquires when fully unlocked (word == 0), matching
 * the fast path of fast_mutex_lock(). */
static inline bool fastMutexTryLock(fastMutex *m) {
    uint64_t prev = 0;
    /* @sync: acquire fastmutex.lock */
    return atomic_compare_exchange_strong_explicit(
        &m->word, &prev, FAST_MUTEX_HELD, memory_order_acquire,
        memory_order_relaxed);
}

static inline bool fastMutexIsLocked(fastMutex *m) {
    /* A snapshot: the spin in fastMutexLockSlow re-decides with a CAS. */
    /* @sync: relaxed hint */
    return (atomic_load_explicit(&m->word, memory_order_relaxed) &
            FAST_MUTEX_HELD) != 0;
}

static inline void fastMutexInit(fastMutex *m) {
    /* @sync: relaxed init */
    atomic_store_explicit(&m->word, 0, memory_order_relaxed);
}

static inline int fastMutexCondInit(fastCond *c, void *) {
    int ret;
    ret = pthread_mutex_init(&c->m, NULL);
    if (ret) {
        return ret;
    }

    c->w = NULL;

    return 0;
}

int fastMutexCondWait(fastCond *c, fastMutex *m);
int fastMutexCondTimedWait(fastCond *c, fastMutex *m, struct timespec *ts);
void fastMutexCondSignal(fastCond *c);

#ifdef DATAKIT_TEST
int fastmutexTest(int argc, char *argv[]);
#endif
