/* inflightGate - in-flight operation counter with a bounded wait for zero
 *
 * Callers bracket an operation with inflightGateEnter/inflightGateExit.
 * A shutdown path calls inflightGateAwaitZeroMs to park until every
 * operation has left or the budget expires (returns IDLE or EXPIRED).
 * On EXPIRED the caller decides what to do. inflightGateDepth is an
 * acquire load of the count, for diagnostics.
 *
 * The gate does not decide admission: a caller that refuses new work
 * during shutdown re-checks its own "open" flag after Enter and calls
 * Exit if it is closed.
 *
 * Cost: Enter is one atomic RMW. Exit is one atomic RMW plus, only on
 * the decrement that reaches zero, a load of `waiters` (on the same
 * cache line); it takes the gate mutex only when a waiter is
 * registered. The RMWs are seq_cst, which compiles to the same
 * instruction as acq_rel on x86-64 and AArch64.
 *
 * No lost wakeup: the waiter increments `waiters` and then loads
 * `count`; the exiter decrements `count` and then loads `waiters`. All
 * four accesses are seq_cst, so at least one side observes the other.
 * If the exiter sees a waiter, it broadcasts under the mutex the waiter
 * holds while checking `count`, so the wake cannot fall between that
 * check and the park. If it sees none, the waiter's load of `count`
 * already sees the decrement.
 *
 * Initialisation: INFLIGHT_GATE_INIT is a constant initialiser. The
 * mutex and condition variable are created on the first
 * inflightGateAwaitZeroMs (never on the Enter/Exit path) and are not
 * destroyed. A waiter publishes `ready` before it increments `waiters`,
 * so an exiter that sees a waiter always finds them constructed.
 */
#pragma once

#include "datakit.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* Verdicts, in one place: the enum, the count and the names all follow
 * this list. */
#define INFLIGHT_GATE_VERDICTS(X)                                              \
    X(IDLE, "idle")                                                            \
    X(EXPIRED, "expired")

typedef enum inflightGateVerdict {
#define INFLIGHT_GATE_VERDICT_ENUM(name, text) INFLIGHT_GATE_##name,
    INFLIGHT_GATE_VERDICTS(INFLIGHT_GATE_VERDICT_ENUM)
#undef INFLIGHT_GATE_VERDICT_ENUM
        INFLIGHT_GATE_VERDICT_COUNT
} inflightGateVerdict;

typedef struct inflightGate {
    /* Adjacent so Exit's load of `waiters` hits the line its RMW on
     * `count` just took. */
    _Atomic uint32_t count;
    _Atomic uint32_t waiters;
    /* Set once the mutex and condvar below are constructed. */
    _Atomic bool ready;
    pthread_mutex_t mu;
    pthread_cond_t cv;
} inflightGate;

#define INFLIGHT_GATE_INIT {.count = 0, .waiters = 0, .ready = false}

const char *inflightGateVerdictName(inflightGateVerdict v);

/* Slow path of Exit: the last exit with a waiter registered. Out of line
 * so the inline Exit stays a decrement plus one predicted-not-taken
 * branch. */
void inflightGateWakeAllWaiters(inflightGate *g);

/* Park until the gate reads zero or `budgetMs` elapses. */
inflightGateVerdict inflightGateAwaitZeroMs(inflightGate *g, uint64_t budgetMs);

static inline void inflightGateEnter(inflightGate *g) {
    atomic_fetch_add_explicit(&g->count, 1, memory_order_seq_cst);
}

/* Exit, returning the count before the decrement (1 means this call
 * reached zero). Use the returned value rather than a later load: it
 * identifies the modification that reached zero, which a later RMW can
 * synchronize with. */
static inline uint32_t inflightGateExitPrev(inflightGate *g) {
    const uint32_t prev =
        atomic_fetch_sub_explicit(&g->count, 1, memory_order_seq_cst);
    if (prev == 1 &&
        atomic_load_explicit(&g->waiters, memory_order_seq_cst) != 0) {
        inflightGateWakeAllWaiters(g);
    }
    return prev;
}

static inline void inflightGateExit(inflightGate *g) {
    (void)inflightGateExitPrev(g);
}

/* Acquire load of the count. It may return a value older than the
 * latest in the count's modification order; use inflightGateSampleLatest
 * when that matters. */
static inline uint32_t inflightGateDepth(const inflightGate *g) {
    return atomic_load_explicit(&g->count, memory_order_acquire);
}

/* Read the latest count with an RMW (add 0). Unlike Depth, an RMW always
 * reads the last value in the count's modification order, and being
 * seq_cst it is also a release that later Enters read from. A zero here
 * means every earlier scope has exited and every later scope entered
 * after this read. */
static inline uint32_t inflightGateSampleLatest(inflightGate *g) {
    return atomic_fetch_add_explicit(&g->count, 0, memory_order_seq_cst);
}
