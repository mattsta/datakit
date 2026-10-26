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
    /* @sync: acquire gate.ready */
    if (atomic_load_explicit(&g->ready, memory_order_acquire)) {
        return;
    }

    pthread_mutex_lock(&inflightGateInitLock);
    /* @sync: relaxed owner */
    if (!atomic_load_explicit(&g->ready, memory_order_relaxed)) {
        pthread_mutex_init(&g->mu, nullptr);
        monotonicCondInit(&g->cv);
        /* @sync: release gate.ready */
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
    /* @sync: seq_cst gate.count */
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
    /* Idle and never constructed: return without constructing it.
     *
     * The count is sampled with an RMW, as inflightGateSampleLatest does,
     * because this IDLE lets the caller free what the gate guards.  The
     * caller unpublished it (or closed admission) before calling; a scope
     * runs "Enter, then load the published pointer".  A plain load here
     * would make that the store-buffering pattern, where C11 lets both
     * sides read the old value: IDLE while a scope holds the pointer.
     * With the RMW, an Enter ordered before it is counted (or has
     * exited), and an Enter ordered after it reads from its release
     * sequence, so that scope sees the unpublish.
     *
     * No exit can still be inside the mutex path: one that was would have
     * decremented after seeing ARMED, which a waiter sets only after
     * constructing the pair.  The RMW reads that decrement or later, so
     * it synchronizes with it, and the exit's own read of `ready` (set)
     * happens before the load below, which by coherence then reads set
     * too.  The count is sampled first for that reason; `ready` needs no
     * order of its own. */
    /* @sync: seq_cst gate.count */
    if (atomic_fetch_add_explicit(&g->count, 0, memory_order_seq_cst) == 0 &&
        /* @sync: relaxed hint */
        !atomic_load_explicit(&g->ready, memory_order_relaxed)) {
        return INFLIGHT_GATE_IDLE;
    }

    inflightGateEnsureReady(g);

    const uint64_t startMs = timeUtilMonotonicMs();
    inflightGateVerdict verdict = INFLIGHT_GATE_EXPIRED;

    pthread_mutex_lock(&g->mu);
    /* Register, then arm with an RMW on the count word: from here on an
     * exit that would reach zero takes the mutex instead of its CAS. */
    /* @sync: relaxed owner */
    atomic_fetch_add_explicit(&g->waiters, 1, memory_order_relaxed);
    /* @sync: seq_cst gate.count */
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
        /* @sync: seq_cst gate.count */
        cur = atomic_load_explicit(&g->count, memory_order_seq_cst);
    }

    /* The last waiter disarms, so exits go back to the CAS path. */
    /* @sync: relaxed owner */
    if (atomic_fetch_sub_explicit(&g->waiters, 1, memory_order_relaxed) == 1) {
        /* @sync: seq_cst gate.count */
        atomic_fetch_and_explicit(&g->count, ~INFLIGHT_GATE_ARMED,
                                  memory_order_seq_cst);
    }

    pthread_mutex_unlock(&g->mu);
    return verdict;
}
