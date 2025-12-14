#include "fastmutex.h"

/* Adatped from:
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

static void fast_waiter_init(fastWaiter *w) {
    pthread_mutex_init(&w->lock, NULL);
    pthread_cond_init(&w->cond, NULL);
    w->waiting = true;
    w->next = NULL;
    w->tail = NULL;
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
        uint64_t prev = atomic_load_explicit(&m->word, memory_order_relaxed);
        if (((prev & 1) == 0) &&
            atomic_compare_exchange_weak_explicit(&m->word, &prev, prev | 1,
                                                  memory_order_acquire,
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

        prev = atomic_load_explicit(&m->word, memory_order_relaxed);
        if ((prev & 1) == 0) {
            continue;
        }

        if (prev & 2) {
            continue;
        }

        uint64_t expected = (uint64_t)prev | 2;
        if (atomic_compare_exchange_weak_explicit(&m->word, &prev, expected,
                                                  memory_order_acquire,
                                                  memory_order_relaxed)) {
            // Locked for access
            fastWaiter waiter;
            assert(((uintptr_t)&waiter & 0x3) == 0);
            fast_waiter_init(&waiter);

            fastWaiter *w = (fastWaiter *)(prev & ~(uint64_t)3);
            if (w) {
                fastWaiter *last = w->tail;
                last->next = &waiter;
                w->tail = &waiter;
            } else {
                w = &waiter;
                waiter.tail = &waiter;
            }

            atomic_store_explicit(&m->word, (uint64_t)w | 1,
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
    uint64_t w = atomic_load_explicit(&m->word, memory_order_relaxed);
    assert((w & 1) == 1);
    while (true) {
        while (w & 0x2) {
            w = atomic_load_explicit(&m->word, memory_order_relaxed);
        }

        assert((w & 0x2) == 0);
        if (atomic_compare_exchange_weak_explicit(&m->word, &w, w | 2,
                                                  memory_order_acquire,
                                                  memory_order_relaxed)) {
            break;
        }
    }

    assert((w & 1) == 1);
    w = w | 2;
    fastWaiter *waiter = (fastWaiter *)(w & ~(uint64_t)0x3);
    assert(waiter);
    fastWaiter *next_waiter = waiter->next;
    if (next_waiter) {
        next_waiter->tail = waiter->tail;
    } else {
        assert(waiter->tail == waiter);
    }

    // Note that someone else can still lock/unlock here, so this has to
    // cas.  We have the list locked though, so the list cannot change
    // release lock, release queue lock
    atomic_store_explicit(&m->word, (uint64_t)next_waiter,
                          memory_order_relaxed);
    pthread_mutex_lock(&waiter->lock);
    waiter->waiting = false;
    pthread_cond_signal(&waiter->cond);
    pthread_mutex_unlock(&waiter->lock);
}

int fastMutexCondWait(fastCond *c, fastMutex *m) {
    return fastMutexCondTimedWait(c, m, NULL);
}

int fastMutexCondTimedWait(fastCond *c, fastMutex *m, struct timespec *ts) {
    int ret = 0;
    fastWaiter waiter;
    assert(((uintptr_t)&waiter & 0x3) == 0);
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
            if (c->w && waiter.next == NULL) {
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
            assert(c->w->tail = waiter->tail);
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
