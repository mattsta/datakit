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
 * Lifetime (drain-then-free): when inflightGateAwaitZeroMs returns IDLE,
 * every Exit counted before that zero has finished touching the gate, so
 * the caller may free the gate's storage at once, provided no thread can
 * still call Enter on it (for example, its owner is already unreachable
 * to new callers). On EXPIRED, or while another thread may still Enter,
 * the gate must stay allocated.
 *
 * Cost: Enter is one atomic RMW. Exit is a load and a CAS, retried only
 * when another Enter or Exit changed the count in between; it takes the
 * gate mutex only when it would bring the count to zero while a waiter
 * is registered. The RMWs are seq_cst, which compiles to the same
 * instruction as acq_rel on x86-64 and AArch64. Every Enter and Exit
 * modifies the one count word, so scopes that enter and exit at the same
 * time on different cores serialize on its cache line, as the read side
 * of a reader-writer lock does: the cost of a scope grows with the
 * number of threads using the gate at once.
 *
 * The count word holds the depth in its low 31 bits and
 * INFLIGHT_GATE_ARMED, set while a waiter is registered, in bit 31. At
 * most 2^31 - 1 scopes may be inside at once.
 *
 * No lost wakeup: a waiter sets ARMED with an RMW while it holds the
 * gate mutex and decides from the count that RMW returned or that it
 * loads later under the mutex. Every access involved is to the one count
 * word, so its modification order decides. An exit ordered before the
 * ARMED RMW has already decremented, and the waiter reads its result. An
 * exit ordered after it finds ARMED in the value its CAS compares
 * against; if it would reach zero, it does not CAS but takes the mutex,
 * decrements and broadcasts while holding it. The waiter holds the mutex
 * from its count read until pthread_cond_wait releases it, so the
 * broadcast cannot fall between that read and the park.
 *
 * Why IDLE permits a free: the waiter returns IDLE only after reading a
 * depth of zero.
 *   - An exit that decremented by CAS is done; the CAS was its last
 *     access to the gate.
 *   - An exit that decremented under the mutex did so inside a critical
 *     section, and the waiter read the zero while holding the mutex, so
 *     that critical section precedes the waiter's: its broadcast and
 *     unlock are complete. (The waiter returns without the mutex only
 *     when the gate was never constructed; then no exit has taken the
 *     mutex, because ARMED is set only after construction.)
 *   - An exit that has seen ARMED but not yet taken the mutex has not
 *     decremented, so the depth cannot read zero.
 * Waiters touch the gate only before they return.
 *
 * Initialisation: INFLIGHT_GATE_INIT is a constant initialiser. The
 * mutex and condition variable are created on the first wait that finds
 * the gate busy (never on the Enter/Exit path) and are not destroyed.
 * A waiter publishes `ready` before it sets ARMED, so an exiter that
 * sees ARMED finds them constructed.
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

/* Bit 31 of `count`: at least one waiter is registered. The low bits are
 * the depth. */
#define INFLIGHT_GATE_ARMED (UINT32_C(1) << 31)
#define INFLIGHT_GATE_DEPTH_MASK (INFLIGHT_GATE_ARMED - 1u)

typedef struct inflightGate {
    /* Depth plus INFLIGHT_GATE_ARMED. */
    _Atomic uint32_t count;
    /* Registered waiters; changed only under `mu`. The first sets ARMED
     * and the last clears it. */
    _Atomic uint32_t waiters;
    /* Set once the mutex and condvar below are constructed. */
    _Atomic bool ready;
    pthread_mutex_t mu;
    pthread_cond_t cv;
} inflightGate;

#define INFLIGHT_GATE_INIT {.count = 0, .waiters = 0, .ready = false}

const char *inflightGateVerdictName(inflightGateVerdict v);

/* Broadcast to every parked waiter under the gate mutex, constructing the
 * mutex and condvar first if needed. Waiters re-check the count, so a
 * wake while the gate is busy is harmless. Exit does not use it; see
 * inflightGateExitArmed. */
void inflightGateWakeAllWaiters(inflightGate *g);

/* Slow path of Exit: the exit that would bring the depth to zero while a
 * waiter is registered. Decrements under the gate mutex and broadcasts if
 * the depth reached zero. Returns the depth before the decrement. Out of
 * line so the inline Exit stays a load, a CAS and one
 * predicted-not-taken branch. */
uint32_t inflightGateExitArmed(inflightGate *g);

/* Park until the gate reads zero or `budgetMs` elapses. */
inflightGateVerdict inflightGateAwaitZeroMs(inflightGate *g, uint64_t budgetMs);

static inline void inflightGateEnter(inflightGate *g) {
    atomic_fetch_add_explicit(&g->count, 1, memory_order_seq_cst);
}

/* Exit, returning the depth before the decrement (1 means this call
 * reached zero). Use the returned value rather than a later load: it
 * identifies the modification that reached zero, which a later RMW can
 * synchronize with. The relaxed load only seeds the CAS, which compares
 * against the current value. */
static inline uint32_t inflightGateExitPrev(inflightGate *g) {
    uint32_t cur = atomic_load_explicit(&g->count, memory_order_relaxed);
    for (;;) {
        if (cur == (INFLIGHT_GATE_ARMED | 1u)) {
            return inflightGateExitArmed(g);
        }
        if (atomic_compare_exchange_weak_explicit(&g->count, &cur, cur - 1u,
                                                  memory_order_seq_cst,
                                                  memory_order_relaxed)) {
            return cur & INFLIGHT_GATE_DEPTH_MASK;
        }
    }
}

static inline void inflightGateExit(inflightGate *g) {
    (void)inflightGateExitPrev(g);
}

/* Acquire load of the depth. It may return a value older than the
 * latest in the count's modification order; use inflightGateSampleLatest
 * when that matters. */
static inline uint32_t inflightGateDepth(const inflightGate *g) {
    return atomic_load_explicit(&g->count, memory_order_acquire) &
           INFLIGHT_GATE_DEPTH_MASK;
}

/* Read the latest depth with an RMW (add 0). Unlike Depth, an RMW always
 * reads the last value in the count's modification order, and being
 * seq_cst it is also a release that later Enters read from. A zero here
 * means every earlier scope has exited and every later scope entered
 * after this read. */
static inline uint32_t inflightGateSampleLatest(inflightGate *g) {
    return atomic_fetch_add_explicit(&g->count, 0, memory_order_seq_cst) &
           INFLIGHT_GATE_DEPTH_MASK;
}
