/* inflightGate - see the header for the contract and the lost-wakeup
 * argument this file implements. */

#include "inflightGate.h"

#include "monotonicCond.h"
#include "timeUtil.h"

static const char *const inflightGateVerdictNames[] = {
#define INFLIGHT_GATE_VERDICT_NAME(name, text) text,
    INFLIGHT_GATE_VERDICTS(INFLIGHT_GATE_VERDICT_NAME)
#undef INFLIGHT_GATE_VERDICT_NAME
};

const char *inflightGateVerdictName(const inflightGateVerdict v) {
    if ((unsigned)v >= INFLIGHT_GATE_VERDICT_COUNT) {
        return "unknown";
    }

    return inflightGateVerdictNames[v];
}

/* Guards first-use construction of every gate's mutex and condvar. A
 * condvar cannot be statically bound to CLOCK_MONOTONIC, so a statically
 * initialised gate builds the pair on its first wait; this lock is taken
 * at most once per gate after that. */
static pthread_mutex_t inflightGateInitLock = PTHREAD_MUTEX_INITIALIZER;

static void inflightGateEnsureReady(inflightGate *g) {
    if (atomic_load_explicit(&g->ready, memory_order_acquire)) {
        return;
    }

    pthread_mutex_lock(&inflightGateInitLock);
    if (!atomic_load_explicit(&g->ready, memory_order_relaxed)) {
        pthread_mutex_init(&g->mu, NULL);
        monotonicCondInit(&g->cv);
        atomic_store_explicit(&g->ready, true, memory_order_release);
    }

    pthread_mutex_unlock(&inflightGateInitLock);
}

void inflightGateWakeAllWaiters(inflightGate *g) {
    /* A registered waiter has already constructed the pair; the check
     * keeps this function safe to call on its own. */
    inflightGateEnsureReady(g);

    /* Broadcasting under the mutex closes the window between a waiter's
     * count read and its park: either this thread locks first and the
     * waiter then reads the zero count, or the waiter is already parked
     * and receives the broadcast. */
    pthread_mutex_lock(&g->mu);
    pthread_cond_broadcast(&g->cv);
    pthread_mutex_unlock(&g->mu);
}

inflightGateVerdict inflightGateAwaitZeroMs(inflightGate *g,
                                            const uint64_t budgetMs) {
    /* Already idle: return without constructing the gate. */
    if (atomic_load_explicit(&g->count, memory_order_acquire) == 0) {
        return INFLIGHT_GATE_IDLE;
    }

    inflightGateEnsureReady(g);

    const uint64_t startMs = timeUtilMonotonicMs();
    inflightGateVerdict verdict = INFLIGHT_GATE_EXPIRED;

    pthread_mutex_lock(&g->mu);
    /* Register before every count read, so an exiter that sees no waiter
     * has a decrement this thread's next count read observes. */
    atomic_fetch_add_explicit(&g->waiters, 1, memory_order_seq_cst);

    for (;;) {
        if (atomic_load_explicit(&g->count, memory_order_seq_cst) == 0) {
            verdict = INFLIGHT_GATE_IDLE;
            break;
        }

        const uint64_t elapsedMs = timeUtilMonotonicMs() - startMs;
        if (elapsedMs >= budgetMs) {
            break;
        }

        (void)monotonicCondWaitMs(&g->cv, &g->mu, budgetMs - elapsedMs);
    }

    atomic_fetch_sub_explicit(&g->waiters, 1, memory_order_seq_cst);
    pthread_mutex_unlock(&g->mu);
    return verdict;
}
