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
    inflightGateEnsureReady(g);

    /* Under the mutex, so the wake cannot fall between a waiter's count
     * read and its park. */
    pthread_mutex_lock(&g->mu);
    pthread_cond_broadcast(&g->cv);
    pthread_mutex_unlock(&g->mu);
}

uint32_t inflightGateExitArmed(inflightGate *g) {
    /* The waiter that set ARMED constructed the pair first. This thread
     * saw ARMED through a relaxed load, so it takes its own
     * happens-before edge to that construction here (an acquire load of
     * `ready`, or the init lock). */
    inflightGateEnsureReady(g);

    /* Decrement inside the critical section: a waiter that reads the
     * resulting zero holds the mutex, so this section, broadcast and
     * unlock included, is complete before the waiter can return. */
    pthread_mutex_lock(&g->mu);
    const uint32_t prev =
        atomic_fetch_sub_explicit(&g->count, 1, memory_order_seq_cst);
    if ((prev & INFLIGHT_GATE_DEPTH_MASK) == 1) {
        pthread_cond_broadcast(&g->cv);
    }

    pthread_mutex_unlock(&g->mu);
    return prev & INFLIGHT_GATE_DEPTH_MASK;
}

inflightGateVerdict inflightGateAwaitZeroMs(inflightGate *g,
                                            const uint64_t budgetMs) {
    /* Idle and never constructed: return without constructing it. No
     * exit can still be inside the mutex path, because one that was
     * would have decremented after seeing ARMED, which a waiter sets only
     * after constructing the pair, and the acquire load below that reads
     * zero would then see `ready` set. The count is loaded first for
     * that reason. */
    if (atomic_load_explicit(&g->count, memory_order_acquire) == 0 &&
        !atomic_load_explicit(&g->ready, memory_order_acquire)) {
        return INFLIGHT_GATE_IDLE;
    }

    inflightGateEnsureReady(g);

    const uint64_t startMs = timeUtilMonotonicMs();
    inflightGateVerdict verdict = INFLIGHT_GATE_EXPIRED;

    pthread_mutex_lock(&g->mu);
    /* Register, then arm with an RMW on the count word: from here on an
     * exit that would reach zero takes the mutex instead of its CAS. */
    atomic_fetch_add_explicit(&g->waiters, 1, memory_order_relaxed);
    uint32_t cur = atomic_fetch_or_explicit(&g->count, INFLIGHT_GATE_ARMED,
                                            memory_order_seq_cst);

    for (;;) {
        /* Read under the mutex; see the header for why a zero here
         * means no exit can touch the gate again. */
        if ((cur & INFLIGHT_GATE_DEPTH_MASK) == 0) {
            verdict = INFLIGHT_GATE_IDLE;
            break;
        }

        const uint64_t elapsedMs = timeUtilMonotonicMs() - startMs;
        if (elapsedMs >= budgetMs) {
            break;
        }

        (void)monotonicCondWaitMs(&g->cv, &g->mu, budgetMs - elapsedMs);
        cur = atomic_load_explicit(&g->count, memory_order_seq_cst);
    }

    /* The last waiter disarms, so exits go back to the CAS path. */
    if (atomic_fetch_sub_explicit(&g->waiters, 1, memory_order_relaxed) == 1) {
        atomic_fetch_and_explicit(&g->count, ~INFLIGHT_GATE_ARMED,
                                  memory_order_seq_cst);
    }

    pthread_mutex_unlock(&g->mu);
    return verdict;
}
