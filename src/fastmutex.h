#pragma once

/* Adatped from:
 * https://github.com/jemalloc/jemalloc/pull/1516
 */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

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
    if (!atomic_compare_exchange_weak_explicit(
            &m->word, &prev, 1, memory_order_acquire, memory_order_relaxed)) {
        fastMutexLockSlow(m);
    }
}

static inline void fastMutexUnlock(fastMutex *m) {
    uint64_t prev = 1;
    if (!atomic_compare_exchange_strong_explicit(
            &m->word, &prev, 0, memory_order_release, memory_order_relaxed)) {
        assert((prev & 1) == 1);
        fastMutexUnlockSlow(m);
    }
}

static inline bool fastMutexTryLock(fastMutex *m) {
    uint64_t prev = 0;
    if (!atomic_compare_exchange_weak_explicit(
            &m->word, &prev, 1, memory_order_acquire, memory_order_relaxed)) {
        return true;
    }

    return false;
}

static inline bool fastMutexIsLocked(fastMutex *m) {
    return (atomic_load_explicit(&m->word, memory_order_relaxed) & 1) != 0;
}

static inline void fastMutexInit(fastMutex *m) {
    atomic_store_explicit(&m->word, 0, memory_order_relaxed);
}

static inline int fastMutexCondInit(fastCond *c, void *unused) {
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
