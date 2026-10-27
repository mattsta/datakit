/**
 * timerWheel Test Suite
 *
 * Comprehensive tests for correctness and performance benchmarks
 * comparing against multiTimer.
 */

#include "timerWheel.h"
#include "multiTimer.h"
#include "timeUtil.h"

#include <inttypes.h>
#include <string.h>
#include <time.h>

#ifdef DATAKIT_TEST
#include "cleaner.h"
#include "ctest.h"
#include "dkDiag.h"

#define DOUBLE_NEWLINE 1
#include "perf.h"

/* ====================================================================
 * Sanitizer Detection for Benchmark Scaling
 *
 * When running under sanitizers (ASan, MSan, etc.), operations are
 * significantly slower. We reduce benchmark scales to keep test times
 * reasonable while still verifying correctness.
 * ==================================================================== */
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer) ||     \
    __has_feature(thread_sanitizer)
#define SANITIZER_BUILD 1
#endif
#endif
#if !defined(SANITIZER_BUILD) &&                                               \
    (defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__))
#define SANITIZER_BUILD 1
#endif

#ifdef SANITIZER_BUILD
/* Reduced scales for sanitizer builds (~100x smaller for large tests) */
#define BENCH_SCALE_1K 100
#define BENCH_SCALE_10K 1000
#define BENCH_SCALE_50K 5000
#define BENCH_SCALE_100K 10000
#define BENCH_SCALE_500K 50000
#define BENCH_QUERY_COUNT 10000
#define BENCH_CHURN_BASE 1000
#define BENCH_CHURN_ITERS 10000
#else
/* Full scales for regular builds */
#define BENCH_SCALE_1K 1000
#define BENCH_SCALE_10K 10000
#define BENCH_SCALE_50K 50000
#define BENCH_SCALE_100K 100'000
#define BENCH_SCALE_500K 500'000
#define BENCH_QUERY_COUNT 100'000
#define BENCH_CHURN_BASE 10000
#define BENCH_CHURN_ITERS 100'000
#endif

/* ====================================================================
 * Test State and Helpers
 * ==================================================================== */

typedef struct testCallbackState {
    int32_t callCount;
    timerWheelId lastId;
    bool shouldReschedule;
} testCallbackState;

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleanerName(testCallbackState *, zfree, States);
defcleaner(timerWheel *, timerWheelFree);
defcleaner(multiTimer *, multiTimerFree);

/* True when the event time 'next' is 'delayUs' after a clock read taken
 * between 'before' and 'after', give or take one 1 ms wheel slot of
 * rounding.  Bracketing the calls checks the units and the offset without
 * depending on how long a loaded machine (a sanitizer, parallel suites)
 * takes between the reads, which a fixed tolerance around one read does. */
static bool twDueWithin_(const int64_t next, const int64_t before,
                         const int64_t after, const uint64_t delayUs) {
    const int64_t slotUs = 1000;
    return next >= before + (int64_t)delayUs - slotUs &&
           next <= after + (int64_t)delayUs + slotUs;
}

/* Counts, cancels itself, and asks to repeat anyway */
static bool testCancelSelfRepeating(timerWheel *tw, timerWheelId id,
                                    void *clientData) {
    testCallbackState *state = clientData;
    state->callCount++;
    timerWheelUnregister(tw, id);
    return true;
}

static bool testCountingCallback(timerWheel *, timerWheelId id,
                                 void *clientData) {
    testCallbackState *state = clientData;
    state->callCount++;
    state->lastId = id;
    return state->shouldReschedule;
}

/* Callback that tries to re-enter timer processing */
static bool testReentrantProcessCallback(timerWheel *tw, timerWheelId,
                                         void *clientData) {
    testCallbackState *state = clientData;
    state->callCount++;
    timerWheelProcessTimerEvents(tw); /* ignored, and reported */
    return false;
}

static bool testNestedTimerCallback(timerWheel *tw, timerWheelId,
                                    void *clientData) {
    testCallbackState *state = clientData;
    state->callCount++;

    if (state->callCount == 1) {
        timerWheelRegister(tw, 1000, 0, testCountingCallback, clientData);
    }

    return false;
}

static bool testSelfUnregisterCallback(timerWheel *tw, timerWheelId id,
                                       void *clientData) {
    testCallbackState *state = clientData;
    state->callCount++;
    timerWheelUnregister(tw, id);
    return true;
}

static bool testSelfUnregisterNoRepeatCallback(timerWheel *tw, timerWheelId id,
                                               void *clientData) {
    testCallbackState *state = clientData;
    state->callCount++;
    timerWheelUnregister(tw, id);
    return false;
}

static bool testStopAllCallback(timerWheel *tw, timerWheelId,
                                void *clientData) {
    testCallbackState *state = clientData;
    state->callCount++;
    timerWheelStopAll(tw);
    return true; /* reschedule request must be ignored after StopAll */
}

typedef struct testFarNearState {
    testCallbackState self;
    testCallbackState farTimer;
    testCallbackState nearTimer;
} testFarNearState;

static bool testRegisterFarThenNearCallback(timerWheel *tw, timerWheelId,
                                            void *clientData) {
    testFarNearState *s = clientData;
    s->self.callCount++;
    timerWheelRegister(tw, 10'000'000, 0, testCountingCallback, &s->farTimer);
    timerWheelRegister(tw, 2000, 0, testCountingCallback, &s->nearTimer);
    return false;
}

static bool testRegisterFarCallback(timerWheel *tw, timerWheelId,
                                    void *clientData) {
    timerWheelRegister(tw, 10'000'000, 0, testCountingCallback, clientData);
    return false;
}

/* The lagging-clock placement: inside a callback fired from a wheel-0
 * slot, `now` is moved past the wheel clock without processing (the
 * advance's own processing pass returns at once inside a callback), so
 * the two registrations are placed by processPending against a wheel
 * clock that lags `now` by lagUs.  A level chosen from the delay
 * measured from `now` with the slot offset measured from the wheel
 * clock wraps the far one into a slot nearer in ring order than the
 * near one's; placed by the wheel clock alone, the far one takes the
 * level its wheel-clock offset needs and nothing wraps. */
typedef struct testLaggingClockState {
    testCallbackState self;
    testCallbackState nearTimer;
    testCallbackState farTimer;
    uint64_t lagUs;
    uint64_t nearDelayUs;
    uint64_t farDelayUs;
    uint64_t registeredAtUs;
} testLaggingClockState;

static bool testLagThenRegisterCallback(timerWheel *tw, timerWheelId,
                                        void *clientData) {
    testLaggingClockState *s = clientData;
    s->self.callCount++;
    timerWheelAdvanceTime(tw, s->lagUs);
    s->registeredAtUs = timeUtilMonotonicUs();
    timerWheelRegister(tw, s->nearDelayUs, 0, testCountingCallback,
                       &s->nearTimer);
    timerWheelRegister(tw, s->farDelayUs, 0, testCountingCallback,
                       &s->farTimer);
    return false;
}

/* A periodic timer that cancels another timer on its first fire */
typedef struct testCancelOtherState {
    testCallbackState self;
    timerWheelId victim;
} testCancelOtherState;

static bool testCancelOtherCallback(timerWheel *tw, timerWheelId,
                                    void *clientData) {
    testCancelOtherState *s = clientData;
    s->self.callCount++;
    if (s->self.callCount == 1) {
        timerWheelUnregister(tw, s->victim);
    }
    return true;
}

/* Fires and repeats for the differential test; the answer, not the
 * callback, is what is checked. */
static bool testOracleCallback(timerWheel *, timerWheelId, void *) {
    return true;
}

/* Equivalent callbacks for multiTimer comparison */
static bool mtCountingCallback(multiTimer *, multiTimerId id,
                               void *clientData) {
    testCallbackState *state = clientData;
    state->callCount++;
    state->lastId = id;
    return state->shouldReschedule;
}

int timerWheelTest(int, char *[]) {
    int32_t err = 0;

    /* ================================================================
     * Basic Lifecycle Tests
     * ================================================================ */

    TEST("timerWheel: create and free") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        if (!tw) {
            ERR("Failed to create timerWheel");
        }
    }

    TEST("timerWheel: free NULL safety") {
        timerWheelFree(nullptr);
    }

    TEST("timerWheel: unregister after StopAll, StopAll after stale "
         "tombstones, a pending timer cancelling itself") {
        /* Ids StopAll covered unregister as done: true, and nothing more
         * is billed */
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {};
        const timerWheelId a =
            timerWheelRegister(tw, 1'000'000, 0, testCountingCallback, &state);
        timerWheelRegister(tw, 2'000'000, 0, testCountingCallback, &state);
        timerWheelStopAll(tw);
        timerWheelStats stats;
        timerWheelGetStats(tw, &stats);
        const size_t cancelledByStop = stats.totalCancellations;
        if (!timerWheelUnregister(tw, a) || timerWheelCount(tw) != 0 ||
            cancelledByStop != 2) {
            ERR("StopAll or a later unregister of its id miscounted");
        }
        timerWheelGetStats(tw, &stats);
        if (stats.totalCancellations != cancelledByStop) {
            ERR("unregistering a StopAll-covered id billed a cancellation");
        }

        /* An unregister of a fired id leaves a stale tombstone; StopAll
         * reconciles it first, so it bills exactly the one live timer */
        cleaner(timerWheelFree) timerWheel *tw2 = timerWheelNew();
        const timerWheelId fired =
            timerWheelRegister(tw2, 1000, 0, testCountingCallback, &state);
        timerWheelAdvanceTime(tw2, 5000);
        timerWheelUnregister(tw2, fired);
        timerWheelRegister(tw2, 1'000'000, 0, testCountingCallback, &state);
        timerWheelGetStats(tw2, &stats);
        const size_t beforeStop = stats.totalCancellations;
        timerWheelStopAll(tw2);
        timerWheelGetStats(tw2, &stats);
        if (stats.totalCancellations - beforeStop != 1 ||
            timerWheelCount(tw2) != 0) {
            ERR("StopAll billed %zu cancellations, want 1",
                stats.totalCancellations - beforeStop);
        }

        /* An immediate repeating timer that cancels itself in its own
         * callback fires once and is gone, though it asked to repeat */
        cleaner(timerWheelFree) timerWheel *tw3 = timerWheelNew();
        testCallbackState self = {};
        timerWheelRegister(tw3, 0, 1000, testCancelSelfRepeating, &self);
        timerWheelAdvanceTime(tw3, 10);
        timerWheelAdvanceTime(tw3, 10000);
        if (self.callCount != 1 || timerWheelCount(tw3) != 0) {
            ERR("a self-cancelling timer fired %d times, %zu left",
                self.callCount, timerWheelCount(tw3));
        }
    }

    TEST("timerWheel: ResetStats zeroes the counters, not the state") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        int counter = 0;
        const timerWheelId a = timerWheelRegister(
            tw, 1'000'000, 0, testCountingCallback, &counter);
        timerWheelRegister(tw, 2'000'000, 0, testCountingCallback, &counter);
        timerWheelUnregister(tw, a);
        timerWheelStats stats;
        timerWheelGetStats(tw, &stats);
        if (stats.totalRegistrations != 2 || stats.totalCancellations != 1) {
            ERR("before reset: %zu registrations, %zu cancellations",
                stats.totalRegistrations, stats.totalCancellations);
        }
        timerWheelResetStats(tw);
        timerWheelGetStats(tw, &stats);
        if (stats.totalRegistrations || stats.totalCancellations ||
            stats.totalExpirations || stats.totalCascades ||
            stats.memoryBytes == 0) {
            ERR("ResetStats left a counter or cleared the state");
        }
    }

    /* ================================================================
     * Timer Registration Tests
     * ================================================================ */

    TEST("timerWheel: register single timer") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {};

        timerWheelId id =
            timerWheelRegister(tw, 1000, 0, testCountingCallback, &state);
        if (id == 0) {
            ERR("Timer ID should not be 0");
        }

        if (timerWheelCount(tw) != 1) {
            ERR("Expected 1 timer, got %zu", timerWheelCount(tw));
        }
    }

    TEST("timerWheel: register multiple timers") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {};

        timerWheelId ids[5];
        for (int32_t i = 0; i < 5; i++) {
            ids[i] = timerWheelRegister(tw, (uint64_t)(i + 1) * 1000, 0,
                                        testCountingCallback, &state);
        }

        for (int32_t i = 0; i < 5; i++) {
            if (ids[i] != (timerWheelId)(i + 1)) {
                ERR("Timer %d has unexpected ID %" PRIu64, i, ids[i]);
            }
        }

        if (timerWheelCount(tw) != 5) {
            ERR("Expected 5 timers, got %zu", timerWheelCount(tw));
        }
    }

    /* ================================================================
     * Timer Execution Tests
     * ================================================================ */

    TEST("timerWheel: timer fires after delay") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        timerWheelRegister(tw, 5000, 0, testCountingCallback, &state);

        timerWheelProcessTimerEvents(tw);
        if (state.callCount != 0) {
            ERR("Timer fired too early, callCount=%d", state.callCount);
        }

        /* Use timerWheelAdvanceTime for deterministic testing */
        timerWheelAdvanceTime(tw, 10000);

        if (state.callCount != 1) {
            ERR("Timer did not fire, callCount=%d", state.callCount);
        }

        if (timerWheelCount(tw) != 0) {
            ERR("Timer should be removed, count=%zu", timerWheelCount(tw));
        }
    }

    TEST("timerWheel: process from within a callback is ignored and "
         "reported") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState reentrant = {};
        testCallbackState later = {};

        timerWheelRegister(tw, 1000, 0, testReentrantProcessCallback,
                           &reentrant);
        timerWheelRegister(tw, 2000, 0, testCountingCallback, &later);

        const uint64_t diagBefore = dkDiagCount();
        timerWheelAdvanceTime(tw, 10000);

        if (reentrant.callCount != 1 || later.callCount != 1) {
            ERR("the timers fired %d and %d times, expected once each",
                reentrant.callCount, later.callCount);
        }
        dkDiagnostic d;
        if (dkDiagCount() != diagBefore + 1 || !dkDiagLast(&d) ||
            d.code != DK_DIAG_TIMER_REENTERED) {
            ERR("a re-entered ProcessTimerEvents must report "
                "TIMER_REENTERED once");
        }
    }

    TEST("timerWheel: repeating timer") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = true};

        timerWheelRegister(tw, 5000, 5000, testCountingCallback, &state);

        for (int32_t i = 0; i < 3; i++) {
            timerWheelAdvanceTime(tw, 7000);
        }

        if (state.callCount < 3) {
            ERR("Repeating timer fired only %d times, expected >= 3",
                state.callCount);
        }

        if (timerWheelCount(tw) == 0) {
            ERR("Repeating timer was incorrectly removed");
        }
    }

    TEST("timerWheel: timer ordering") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState states[3] = {};

        timerWheelRegister(tw, 15000, 0, testCountingCallback, &states[2]);
        timerWheelRegister(tw, 5000, 0, testCountingCallback, &states[0]);
        timerWheelRegister(tw, 10000, 0, testCountingCallback, &states[1]);

        timerWheelAdvanceTime(tw, 20000);

        if (states[0].callCount != 1 || states[1].callCount != 1 ||
            states[2].callCount != 1) {
            ERR("Not all timers fired: %d, %d, %d", states[0].callCount,
                states[1].callCount, states[2].callCount);
        }
    }

    /* ================================================================
     * Timer Unregistration Tests
     * ================================================================ */

    TEST("timerWheel: unregister timer before fire") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        timerWheelId id =
            timerWheelRegister(tw, 100'000, 0, testCountingCallback, &state);
        timerWheelUnregister(tw, id);

        timerWheelAdvanceTime(tw, 150000);

        if (state.callCount != 0) {
            ERR("Unregistered timer fired, callCount=%d", state.callCount);
        }
    }

    TEST("timerWheel: unregister multiple timers") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState states[5] = {};

        timerWheelId ids[5];
        for (int32_t i = 0; i < 5; i++) {
            states[i].shouldReschedule = false;
            ids[i] = timerWheelRegister(tw, 50000, 0, testCountingCallback,
                                        &states[i]);
        }

        timerWheelUnregister(tw, ids[0]);
        timerWheelUnregister(tw, ids[2]);
        timerWheelUnregister(tw, ids[4]);

        timerWheelAdvanceTime(tw, 60000);

        if (states[0].callCount != 0 || states[2].callCount != 0 ||
            states[4].callCount != 0) {
            ERR("Unregistered timers fired: %d, %d, %d", states[0].callCount,
                states[2].callCount, states[4].callCount);
        }

        if (states[1].callCount != 1 || states[3].callCount != 1) {
            ERR("Registered timers did not fire: %d, %d", states[1].callCount,
                states[3].callCount);
        }
    }

    TEST("timerWheel: stopAll") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState states[10];
        memset(states, 0, sizeof(states));

        for (int32_t i = 0; i < 10; i++) {
            states[i].shouldReschedule = false;
            timerWheelRegister(tw, 50000, 0, testCountingCallback, &states[i]);
        }

        timerWheelStopAll(tw);

        timerWheelAdvanceTime(tw, 60000);

        int32_t totalFired = 0;
        for (int32_t i = 0; i < 10; i++) {
            totalFired += states[i].callCount;
        }

        if (totalFired != 0) {
            ERR("stopAll failed, %d timers fired", totalFired);
        }
    }

    /* ================================================================
     * Nested Timer Operations Tests
     * ================================================================ */

    TEST("timerWheel: register timer from within callback") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        timerWheelRegister(tw, 5000, 0, testNestedTimerCallback, &state);

        timerWheelAdvanceTime(tw, 10000);

        if (state.callCount != 1) {
            ERR("First timer did not fire, callCount=%d", state.callCount);
        }

        if (timerWheelCount(tw) == 0) {
            ERR("Nested timer was not scheduled");
        }

        timerWheelAdvanceTime(tw, 5000);

        if (state.callCount != 2) {
            ERR("Nested timer did not fire, callCount=%d", state.callCount);
        }
    }

    TEST("timerWheel: self-unregister from callback") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = true};

        timerWheelRegister(tw, 5000, 5000, testSelfUnregisterCallback, &state);

        timerWheelAdvanceTime(tw, 10000);

        if (state.callCount != 1) {
            ERR("Timer did not fire once, callCount=%d", state.callCount);
        }

        timerWheelAdvanceTime(tw, 20000);

        if (state.callCount != 1) {
            ERR("Self-unregistered timer fired again, callCount=%d",
                state.callCount);
        }
    }

    /* ================================================================
     * Next Timer Event Tests
     * ================================================================ */

    TEST("timerWheel: nextTimerEventStartUs with no timers") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();

        timerWheelSystemMonotonicUs next = timerWheelNextTimerEventStartUs(tw);
        if (next != 0) {
            ERR("Expected 0 for empty timer, got %" PRId64, next);
        }
    }

    TEST("timerWheel: nextTimerEventStartUs returns correct time") {
        const uint64_t delayUs = 100'000;
        const int64_t before = (int64_t)timeUtilMonotonicUs();
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {};

        timerWheelRegister(tw, delayUs, 0, testCountingCallback, &state);

        const timerWheelSystemMonotonicUs next =
            timerWheelNextTimerEventStartUs(tw);
        const int64_t after = (int64_t)timeUtilMonotonicUs();
        if (!twDueWithin_(next, before, after, delayUs)) {
            ERR("nextTimerEventStartUs %" PRId64 " is not %" PRIu64
                "us after a time in [%" PRId64 ", %" PRId64 "]",
                (int64_t)next, delayUs, before, after);
        }
    }

    /* ================================================================
     * Edge Cases
     * ================================================================ */

    TEST("timerWheel: many timers") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        const int32_t numTimers = 1000;
        int32_t totalCallCount = 0;

        cleanerName(zfree, States) testCallbackState *states =
            zcalloc(numTimers, sizeof(testCallbackState));

        for (int32_t i = 0; i < numTimers; i++) {
            states[i].shouldReschedule = false;
            timerWheelRegister(tw, 10000 + (uint64_t)i, 0, testCountingCallback,
                               &states[i]);
        }

        if (timerWheelCount(tw) != (size_t)numTimers) {
            ERR("Expected %d timers, got %zu", numTimers, timerWheelCount(tw));
        }

        timerWheelAdvanceTime(tw, 20000);

        for (int32_t i = 0; i < numTimers; i++) {
            totalCallCount += states[i].callCount;
        }

        if (totalCallCount != numTimers) {
            ERR("Expected %d firings, got %d", numTimers, totalCallCount);
        }
    }

    TEST("timerWheel: zero delay fires immediately") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        timerWheelRegister(tw, 0, 0, testCountingCallback, &state);
        timerWheelProcessTimerEvents(tw);

        if (state.callCount != 1) {
            ERR("Zero-delay timer did not fire, callCount=%d", state.callCount);
        }
    }

    TEST("timerWheel: callback returns false stops repeating") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        timerWheelRegister(tw, 5000, 5000, testCountingCallback, &state);

        timerWheelAdvanceTime(tw, 10000);

        if (state.callCount != 1) {
            ERR("Timer did not fire once, callCount=%d", state.callCount);
        }

        timerWheelAdvanceTime(tw, 15000);

        if (state.callCount != 1) {
            ERR("Timer repeated despite returning false, callCount=%d",
                state.callCount);
        }
    }

    /* ================================================================
     * Edge Cases: Zero-Delay and Sub-Resolution Timers
     * ================================================================ */

    TEST("timerWheel: zero delay timer goes to pending queue") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        timerWheelId id =
            timerWheelRegister(tw, 0, 0, testCountingCallback, &state);
        if (id == 0) {
            ERR("Timer ID should not be 0");
        }

        /* Timer should be in pending, not yet fired */
        if (state.callCount != 0) {
            ERR("Zero-delay timer should not fire until process, callCount=%d",
                state.callCount);
        }

        /* nextTimerEventStartUs should report the pending timer */
        timerWheelSystemMonotonicUs next = timerWheelNextTimerEventStartUs(tw);
        if (next == 0) {
            ERR("nextTimerEventStartUs should return non-zero for pending "
                "timer");
        }

        timerWheelProcessTimerEvents(tw);

        if (state.callCount != 1) {
            ERR("Zero-delay timer did not fire after process, callCount=%d",
                state.callCount);
        }
    }

    TEST("timerWheel: sub-resolution repeating timer fires rapidly") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = true};

        /* 100μs repeat interval (< 1000μs wheel resolution) */
        timerWheelRegister(tw, 0, 100, testCountingCallback, &state);

        /* First call should fire immediately (0 delay) */
        timerWheelProcessTimerEvents(tw);
        if (state.callCount != 1) {
            ERR("Initial fire failed, callCount=%d", state.callCount);
        }

        /* Advance time and process - timer should fire again after interval */
        for (int i = 1; i < 10; i++) {
            timerWheelAdvanceTime(tw, 100); /* Advance by repeat interval */
            if (state.callCount != i + 1) {
                ERR("Timer should fire after advance %d, callCount=%d "
                    "(expected %d)",
                    i, state.callCount, i + 1);
            }
        }

        if (state.callCount < 10) {
            ERR("Sub-resolution timer should fire 10 times, "
                "callCount=%d (expected >= 10)",
                state.callCount);
        }
    }

    TEST("timerWheel: 1μs repeating timer stays in pending queue") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = true};

        /* 1μs repeat interval (minimum sub-resolution) */
        timerWheelRegister(tw, 0, 1, testCountingCallback, &state);

        /* First fire (0 delay) */
        timerWheelProcessTimerEvents(tw);
        if (state.callCount != 1) {
            ERR("Initial fire failed, callCount=%d", state.callCount);
        }

        /* Process 4 more times with time advancing */
        for (int i = 1; i < 5; i++) {
            /* After each reschedule, nextTimerEventStartUs should return
             * non-zero because timer is in pending queue */
            timerWheelSystemMonotonicUs next =
                timerWheelNextTimerEventStartUs(tw);
            if (next == 0) {
                ERR("nextTimerEventStartUs returned 0 before advance %d, "
                    "callCount=%d",
                    i, state.callCount);
            }

            timerWheelAdvanceTime(tw, 1); /* Advance by 1μs */

            if (state.callCount != i + 1) {
                ERR("Timer should fire after advance %d, callCount=%d "
                    "(expected %d)",
                    i, state.callCount, i + 1);
            }
        }

        if (state.callCount < 5) {
            ERR("1μs repeating timer should fire 5 times, callCount=%d",
                state.callCount);
        }
    }

    TEST("timerWheel: nextTimerEventStartUs with only pending timers") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        /* Register only zero-delay timers */
        timerWheelRegister(tw, 0, 0, testCountingCallback, &state);
        timerWheelRegister(tw, 0, 0, testCountingCallback, &state);
        timerWheelRegister(tw, 0, 0, testCountingCallback, &state);

        /* nextTimerEventStartUs should return non-zero */
        timerWheelSystemMonotonicUs next = timerWheelNextTimerEventStartUs(tw);
        if (next == 0) {
            ERR("nextTimerEventStartUs should not be 0 with pending timers");
        }

        /* Offset should be <= 0 (timer is due now) */
        timerWheelUs offset = timerWheelNextTimerEventOffsetFromNowUs(tw);
        if (offset > 1000) { /* Allow small timing variance */
            ERR("Offset should be ~0 for pending timer, got %" PRId64, offset);
        }

        timerWheelProcessTimerEvents(tw);

        if (state.callCount != 3) {
            ERR("Expected 3 timers to fire, callCount=%d", state.callCount);
        }
    }

    TEST("timerWheel: mixed pending and wheel timers") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState pendingState = {.callCount = 0,
                                          .shouldReschedule = false};
        testCallbackState wheelState = {.callCount = 0,
                                        .shouldReschedule = false};

        /* Pending timer (0 delay) */
        timerWheelRegister(tw, 0, 0, testCountingCallback, &pendingState);

        /* Wheel timer (5ms delay) */
        timerWheelRegister(tw, 5000, 0, testCountingCallback, &wheelState);

        /* nextTimerEventStartUs should return the pending timer's time */
        timerWheelSystemMonotonicUs next = timerWheelNextTimerEventStartUs(tw);
        if (next == 0) {
            ERR("nextTimerEventStartUs should return non-zero");
        }

        timerWheelProcessTimerEvents(tw);

        if (pendingState.callCount != 1) {
            ERR("Pending timer should fire, callCount=%d",
                pendingState.callCount);
        }
        if (wheelState.callCount != 0) {
            ERR("Wheel timer should not fire yet, callCount=%d",
                wheelState.callCount);
        }

        /* Now advance time to fire wheel timer */
        timerWheelAdvanceTime(tw, 10000);

        if (wheelState.callCount != 1) {
            ERR("Wheel timer should fire after advance, callCount=%d",
                wheelState.callCount);
        }
    }

    TEST("timerWheel: sub-resolution timer after wheel timer") {
        /* A repeat shorter than the wheel resolution must fire on every
         * 100μs advance, whether its previous fire came from a wheel slot
         * or from the pending queue.  Which path fires depends on where
         * the real clock falls inside a tick, so run the scenario many
         * times to cover both phases.  AdvanceTime shifts the wheel's
         * epoch while the real clock keeps running, so elapsed real time
         * can add fires but never remove them: require at least one new
         * fire per step. */
        for (int trial = 0; trial < 64; trial++) {
            cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
            testCallbackState state = {.callCount = 0,
                                       .shouldReschedule = true};

            /* First a normal delay, then sub-resolution repeat */
            timerWheelRegister(tw, 5000, 100, testCountingCallback, &state);

            /* Initially no fires */
            timerWheelProcessTimerEvents(tw);
            if (state.callCount != 0) {
                ERR("Timer should not fire before delay, callCount=%d",
                    state.callCount);
            }

            /* After delay plus one resolution unit (wheel slot
             * granularity), timer should fire. Wheel resolution is
             * 1000μs. */
            timerWheelAdvanceTime(tw, 6000);
            if (state.callCount < 1) {
                ERR("Timer should fire after delay, callCount=%d",
                    state.callCount);
            }

            for (int step = 1; step <= 3; step++) {
                const int before = state.callCount;
                timerWheelAdvanceTime(tw, 100);
                if (state.callCount <= before) {
                    ERR("trial %d: timer should fire again after sub-res "
                        "advance %d, callCount=%d",
                        trial, step, state.callCount);
                }
            }
        }
    }

    TEST("timerWheel: multiple sub-resolution timers") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState states[3];
        memset(states, 0, sizeof(states));
        for (int i = 0; i < 3; i++) {
            states[i].shouldReschedule = true;
        }

        /* Register multiple sub-resolution repeating timers with different
         * intervals */
        timerWheelRegister(tw, 0, 100, testCountingCallback,
                           &states[0]); /* 100μs */
        timerWheelRegister(tw, 0, 200, testCountingCallback,
                           &states[1]); /* 200μs */
        timerWheelRegister(tw, 0, 500, testCountingCallback,
                           &states[2]); /* 500μs */

        /* First call fires all three (0 delay) */
        timerWheelProcessTimerEvents(tw);
        for (int i = 0; i < 3; i++) {
            if (states[i].callCount != 1) {
                ERR("Sub-res timer %d should fire initially, callCount=%d", i,
                    states[i].callCount);
            }
        }

        /* Advance time multiple times - each timer fires once per call
         * when its interval has elapsed */
        for (int i = 0; i < 10; i++) {
            timerWheelAdvanceTime(tw, 100); /* Advance 100μs per iteration */
        }

        /* After 1000μs total:
         * Timer 0 (100μs) should have fired ~10 additional times (1 + 10 = 11)
         * Timer 1 (200μs) should have fired ~5 additional times (1 + 5 = 6)
         * Timer 2 (500μs) should have fired ~2 additional times (1 + 2 = 3) */
        if (states[0].callCount < 8) {
            ERR("100μs timer should fire ~11 times in 1000μs, callCount=%d",
                states[0].callCount);
        }
        if (states[1].callCount < 4) {
            ERR("200μs timer should fire ~6 times in 1000μs, callCount=%d",
                states[1].callCount);
        }
        if (states[2].callCount < 2) {
            ERR("500μs timer should fire ~3 times in 1000μs, callCount=%d",
                states[2].callCount);
        }
    }

    TEST("timerWheel: zero delay with repeat goes to pending then wheel") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = true};

        /* Zero initial delay, normal repeat interval (>= resolution) */
        timerWheelRegister(tw, 0, 5000, testCountingCallback, &state);

        /* Should fire immediately from pending */
        timerWheelProcessTimerEvents(tw);
        if (state.callCount != 1) {
            ERR("First fire from pending failed, callCount=%d",
                state.callCount);
        }

        /* Should not fire again until time advances */
        timerWheelProcessTimerEvents(tw);
        if (state.callCount != 1) {
            ERR("Should not fire again without time advance, callCount=%d",
                state.callCount);
        }

        /* Advance time, should fire again */
        timerWheelAdvanceTime(tw, 10000);
        if (state.callCount != 2) {
            ERR("Second fire after advance failed, callCount=%d",
                state.callCount);
        }
    }

    TEST("timerWheel: offset returns negative for overdue timer") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        /* Register a zero-delay timer */
        timerWheelRegister(tw, 0, 0, testCountingCallback, &state);

        /* Small delay to ensure timer is "overdue" */
        struct timespec ts = {0, 1'000'000}; /* 1ms */
        nanosleep(&ts, nullptr);

        /* Offset should be negative or very small (timer is due) */
        timerWheelUs offset = timerWheelNextTimerEventOffsetFromNowUs(tw);
        if (offset > 5000) { /* Allow 5ms variance */
            ERR("Offset should be <= 0 for overdue timer, got %" PRId64,
                offset);
        }
    }

    TEST("timerWheel: stress sub-resolution timers") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = true};

        /* Single 1μs repeating timer */
        timerWheelRegister(tw, 0, 1, testCountingCallback, &state);

        /* First fire (0 delay) */
        timerWheelProcessTimerEvents(tw);
        if (state.callCount != 1) {
            ERR("Initial fire failed, callCount=%d", state.callCount);
        }

        /* Advance time to trigger subsequent fires */
        for (int i = 1; i < 100; i++) {
            timerWheelAdvanceTime(tw, 1); /* Advance 1μs per iteration */
        }

        if (state.callCount < 100) {
            ERR("Stress test: expected >= 100 fires, got %d", state.callCount);
        }

        /* Verify nextTimerEventStartUs still works */
        timerWheelSystemMonotonicUs next = timerWheelNextTimerEventStartUs(tw);
        if (next == 0) {
            ERR("nextTimerEventStartUs returned 0 after stress test");
        }
    }

    TEST("timerWheel: sub-resolution timer stops correctly") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = true};

        timerWheelId id =
            timerWheelRegister(tw, 0, 1, testCountingCallback, &state);

        /* Process a few times */
        timerWheelProcessTimerEvents(tw);
        timerWheelProcessTimerEvents(tw);
        int32_t countBefore = state.callCount;

        /* Unregister */
        timerWheelUnregister(tw, id);

        /* Process more - should not increment */
        timerWheelProcessTimerEvents(tw);
        timerWheelProcessTimerEvents(tw);

        if (state.callCount > countBefore) {
            ERR("Timer fired after unregister, before=%d after=%d", countBefore,
                state.callCount);
        }
    }

    /* ================================================================
     * Regression Tests
     * ================================================================ */

    TEST("timerWheel: repeat period of one full wheel0 revolution") {
        /* A 255ms repeat measured from the slot after its fire is one
         * full wheel0 revolution (256ms) from the wheel clock: placed
         * by the wheel clock it takes wheel 1, one slot on, never the
         * wheel0 slot index being processed (which was detached and
         * cleared). The timer must keep firing every revolution. */
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = true};

        timerWheelRegister(tw, 5000, 255000, testCountingCallback, &state);

        timerWheelAdvanceTime(tw, 6000);
        if (state.callCount != 1) {
            ERR("Timer did not fire initially, callCount=%d", state.callCount);
        }

        for (int i = 0; i < 4; i++) {
            timerWheelAdvanceTime(tw, 256000);
        }

        if (state.callCount < 4) {
            ERR("Timer lost after same-slot reschedule, callCount=%d",
                state.callCount);
        }

        if (timerWheelCount(tw) == 0) {
            ERR("Repeating timer was dropped from the wheel");
        }
    }

    TEST("timerWheel: stopAll from within callback") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState stopper = {.callCount = 0};
        testCallbackState other = {.callCount = 0, .shouldReschedule = true};
        testCallbackState fresh = {.callCount = 0};

        timerWheelRegister(tw, 5000, 5000, testStopAllCallback, &stopper);
        timerWheelRegister(tw, 10000, 5000, testCountingCallback, &other);

        timerWheelAdvanceTime(tw, 6000);
        if (stopper.callCount != 1) {
            ERR("Stopper did not fire, callCount=%d", stopper.callCount);
        }

        timerWheelAdvanceTime(tw, 30000);
        if (stopper.callCount != 1) {
            ERR("Stopper rescheduled despite StopAll, callCount=%d",
                stopper.callCount);
        }
        if (other.callCount != 0) {
            ERR("Other timer fired despite StopAll, callCount=%d",
                other.callCount);
        }
        if (timerWheelCount(tw) != 0) {
            ERR("Expected 0 timers after StopAll, got %zu",
                timerWheelCount(tw));
        }

        /* Timers registered after StopAll must be unaffected */
        timerWheelRegister(tw, 1000, 0, testCountingCallback, &fresh);
        timerWheelAdvanceTime(tw, 3000);
        if (fresh.callCount != 1) {
            ERR("Post-StopAll timer did not fire, callCount=%d",
                fresh.callCount);
        }
    }

    TEST("timerWheel: stopAll then register new timers") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState old = {.callCount = 0};
        testCallbackState fresh = {.callCount = 0};

        for (int32_t i = 0; i < 5; i++) {
            timerWheelRegister(tw, 50000, 0, testCountingCallback, &old);
        }

        timerWheelStopAll(tw);
        if (timerWheelCount(tw) != 0) {
            ERR("Expected 0 timers after StopAll, got %zu",
                timerWheelCount(tw));
        }

        timerWheelRegister(tw, 5000, 0, testCountingCallback, &fresh);
        if (timerWheelCount(tw) != 1) {
            ERR("Expected 1 timer after re-register, got %zu",
                timerWheelCount(tw));
        }

        timerWheelAdvanceTime(tw, 60000);
        if (old.callCount != 0) {
            ERR("Stopped timers fired, callCount=%d", old.callCount);
        }
        if (fresh.callCount != 1) {
            ERR("New timer did not fire, callCount=%d", fresh.callCount);
        }
    }

    TEST("timerWheel: nextTimerEvent scans all pending entries") {
        /* A callback fired from the pending queue registers a far
         * timer, then a near timer; both sit unsorted in pending until
         * the next process pass. The next-event query must report the
         * near one, not whichever entry happens to be first. */
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testFarNearState s;
        memset(&s, 0, sizeof(s));

        timerWheelRegister(tw, 0, 0, testRegisterFarThenNearCallback, &s);
        timerWheelProcessTimerEvents(tw);

        if (s.self.callCount != 1) {
            ERR("Registrar callback did not fire, callCount=%d",
                s.self.callCount);
        }

        timerWheelUs offset = timerWheelNextTimerEventOffsetFromNowUs(tw);
        if (offset > 1'000'000) {
            ERR("Query returned far timer (%" PRId64
                "us) instead of near (~2000us)",
                offset);
        }
    }

    TEST("timerWheel: sub-resolution initial delay fires promptly") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        /* Catch the wheel up first so its internal time is rounded up
         * past "now", then register a 500us timer into that gap */
        timerWheelProcessTimerEvents(tw);
        timerWheelRegister(tw, 500, 0, testCountingCallback, &state);

        timerWheelAdvanceTime(tw, 2000);
        if (state.callCount != 1) {
            ERR("Sub-resolution delay timer late or lost, callCount=%d",
                state.callCount);
        }
    }

    TEST("timerWheel: unregister after fire keeps count sane") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        timerWheelId id =
            timerWheelRegister(tw, 1000, 0, testCountingCallback, &state);
        timerWheelAdvanceTime(tw, 3000);
        if (state.callCount != 1) {
            ERR("Timer did not fire, callCount=%d", state.callCount);
        }

        /* Cancelling an id that already fired is a harmless no-op */
        if (!timerWheelUnregister(tw, id)) {
            ERR("Unregister of fired id should return true");
        }
        if (timerWheelCount(tw) != 0) {
            ERR("Expected 0 timers, got %zu", timerWheelCount(tw));
        }

        /* A process pass on the drained wheel purges the stale
         * tombstone so it cannot skew later counts */
        timerWheelProcessTimerEvents(tw);

        /* The stale tombstone must not affect later timers */
        timerWheelRegister(tw, 1000, 0, testCountingCallback, &state);
        if (timerWheelCount(tw) != 1) {
            ERR("Expected 1 timer, got %zu", timerWheelCount(tw));
        }

        timerWheelAdvanceTime(tw, 3000);
        if (state.callCount != 2) {
            ERR("Second timer did not fire, callCount=%d", state.callCount);
        }
        if (timerWheelCount(tw) != 0) {
            ERR("Expected 0 timers after fire, got %zu", timerWheelCount(tw));
        }
    }

    TEST("timerWheel: reconcile repairs count after unregistering fired id") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        timerWheelId a =
            timerWheelRegister(tw, 1000, 0, testCountingCallback, &state);
        timerWheelId far =
            timerWheelRegister(tw, 60000000, 0, testCountingCallback, &state);
        timerWheelAdvanceTime(tw, 3000);
        if (state.callCount != 1) {
            ERR("Near timer did not fire, callCount=%d", state.callCount);
        }

        /* Unregistering the fired id leaves a stale tombstone the
         * wheel cannot detect inline (no id index); stats must still
         * report the exact value and reconcile must repair count */
        if (!timerWheelUnregister(tw, a)) {
            ERR("Unregister of fired id should return true");
        }

        timerWheelStats stats;
        timerWheelGetStats(tw, &stats);
        if (stats.cancelledPending != 0) {
            ERR("Stats must report exact cancelledPending, got %zu",
                stats.cancelledPending);
        }

        if (!timerWheelReconcileCancellations(tw)) {
            ERR("Reconcile should succeed outside callbacks");
        }
        if (timerWheelCount(tw) != 1) {
            ERR("Count wrong after reconcile, got %zu", timerWheelCount(tw));
        }

        /* A tombstone matching a live timer must survive reconcile */
        timerWheelUnregister(tw, far);
        if (!timerWheelReconcileCancellations(tw)) {
            ERR("Reconcile should succeed outside callbacks");
        }
        if (timerWheelCount(tw) != 0) {
            ERR("Live cancellation lost by reconcile, count=%zu",
                timerWheelCount(tw));
        }
        timerWheelGetStats(tw, &stats);
        if (stats.cancelledPending != 1) {
            ERR("Expected 1 live cancellation, got %zu",
                stats.cancelledPending);
        }

        /* The cancelled far timer must be discarded, not fired */
        timerWheelAdvanceTime(tw, 70000000);
        if (state.callCount != 1) {
            ERR("Cancelled timer fired anyway, callCount=%d", state.callCount);
        }
        if (timerWheelCount(tw) != 0) {
            ERR("Expected empty wheel, got %zu", timerWheelCount(tw));
        }
    }

    TEST("timerWheel: overflow timers sharing an expiry are all kept") {
        /* A saturated delay gives every such timer the same expiry, as
         * two long timers registered in one microsecond do: the overflow
         * store must hold each of them, not keep only the last */
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {};
        timerWheelId ids[3];
        for (int i = 0; i < 3; i++) {
            ids[i] = timerWheelRegister(tw, UINT64_MAX, 0, testCountingCallback,
                                        &state);
        }
        timerWheelStats stats;
        timerWheelGetStats(tw, &stats);
        if (timerWheelCount(tw) != 3 || stats.overflowCount != 3) {
            ERR("count %zu, %zu in overflow, want 3 and 3", timerWheelCount(tw),
                stats.overflowCount);
        }
        /* Each one is still there to cancel */
        timerWheelUnregister(tw, ids[1]);
        if (!timerWheelReconcileCancellations(tw)) {
            ERR("reconcile failed outside a callback");
        }
        timerWheelGetStats(tw, &stats);
        if (timerWheelCount(tw) != 2 || stats.cancelledPending != 1) {
            ERR("after one cancel: count %zu (want 2), %zu live tombstones "
                "(want 1)",
                timerWheelCount(tw), stats.cancelledPending);
        }
    }

    TEST("timerWheel: reconcile keeps live cancellations in every store") {
        /* Timers in all three stores (pending: below the resolution;
         * wheel: one second; overflow: a day), one of each cancelled,
         * plus a fired id unregistered: reconcile drops only the fired
         * id's tombstone and keeps the three whose timers are stored */
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};
        const timerWheelId fired =
            timerWheelRegister(tw, 1000, 0, testCountingCallback, &state);
        timerWheelAdvanceTime(tw, 3000);

        const timerWheelDurationUs day = 24ULL * 3600 * 1'000'000;
        static const timerWheelDurationUs delays[3] = {0, 1'000'000, 0};
        timerWheelId ids[3][3];
        for (int store = 0; store < 3; store++) {
            for (int i = 0; i < 3; i++) {
                ids[store][i] =
                    timerWheelRegister(tw, store == 2 ? day : delays[store], 0,
                                       testCountingCallback, &state);
            }
        }
        timerWheelStats stats;
        timerWheelGetStats(tw, &stats);
        if (stats.pendingCount != 3 || stats.overflowCount != 3) {
            ERR("setup: %zu pending, %zu in overflow, want 3 and 3",
                stats.pendingCount, stats.overflowCount);
        }
        for (int store = 0; store < 3; store++) {
            timerWheelUnregister(tw, ids[store][1]);
        }
        timerWheelUnregister(tw, fired);

        if (!timerWheelReconcileCancellations(tw)) {
            ERR("reconcile failed outside a callback");
        }
        timerWheelGetStats(tw, &stats);
        if (timerWheelCount(tw) != 6 || stats.cancelledPending != 3) {
            ERR("after reconcile: count %zu (want 6), %zu live tombstones "
                "(want 3)",
                timerWheelCount(tw), stats.cancelledPending);
        }

        /* Everything left fires once; the cancelled ones never */
        timerWheelAdvanceTime(tw, day + 2'000'000);
        if (state.callCount != 1 + 6) {
            ERR("%d callbacks, want 7", state.callCount);
        }
    }

    TEST("timerWheel: single large advance with live distant timer") {
        /* Exercises empty-span hopping: the catch-up loop must cross
         * many wheel0 revolutions (with cascades) in one call and
         * still fire the timer exactly once */
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        timerWheelRegister(tw, 5000000, 0, testCountingCallback, &state);

        timerWheelAdvanceTime(tw, 5200000);
        if (state.callCount != 1) {
            ERR("Timer across idle gap fired %d times, expected 1",
                state.callCount);
        }
        if (timerWheelCount(tw) != 0) {
            ERR("Expected empty wheel, got %zu", timerWheelCount(tw));
        }

        /* Wheel positions must still be consistent for later timers */
        timerWheelRegister(tw, 3000, 0, testCountingCallback, &state);
        timerWheelAdvanceTime(tw, 5000);
        if (state.callCount != 2) {
            ERR("Post-hop timer fired %d times total, expected 2",
                state.callCount);
        }
    }

    TEST("timerWheel: nextTimerEvent sees higher-wheel timer past pending") {
        /* A far-future pending entry must not mask a nearer timer
         * sitting in wheel 1 */
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState farState = {.callCount = 0};
        testCallbackState nearState = {.callCount = 0};

        /* Near timer: 1s -> wheel 1 */
        timerWheelRegister(tw, 1'000'000, 0, testCountingCallback, &nearState);

        /* Far timer: registered from a callback, so it parks in the
         * pending queue (10s) until the next process pass */
        timerWheelRegister(tw, 0, 0, testRegisterFarCallback, &farState);
        timerWheelProcessTimerEvents(tw);

        timerWheelUs offset = timerWheelNextTimerEventOffsetFromNowUs(tw);
        if (offset > 2'000'000) {
            ERR("Query returned %" PRId64
                "us; pending far timer masked the ~1s wheel timer",
                offset);
        }
    }

    TEST("timerWheel: self-unregister without repeat leaves no tombstone") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0};

        /* One-shot timer that unregisters itself in its own callback:
         * the tombstone must be retired immediately, not linger */
        timerWheelRegister(tw, 1000, 0, testSelfUnregisterNoRepeatCallback,
                           &state);
        timerWheelAdvanceTime(tw, 3000);
        if (state.callCount != 1) {
            ERR("Timer did not fire, callCount=%d", state.callCount);
        }

        timerWheelStats stats;
        timerWheelGetStats(tw, &stats);
        if (stats.cancelledPending != 0) {
            ERR("Tombstone lingered after self-cancel, cancelledPending=%zu",
                stats.cancelledPending);
        }

        /* Count must be exact for a subsequent timer, with no drain
         * pass needed in between */
        timerWheelRegister(tw, 1000, 0, testCountingCallback, &state);
        if (timerWheelCount(tw) != 1) {
            ERR("Expected 1 timer, got %zu", timerWheelCount(tw));
        }
    }

    TEST("timerWheel: nextTimerEvent query with populated overflow") {
        /* The overflow map is 5 elements wide; the query must hand
         * multimapFirst a full-width out array (an undersized one gets
         * written past its end - this was a live stack corruption) */
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0};

        const uint64_t dayUs = 24ULL * 60 * 60 * 1'000'000;
        const uint64_t before = timeUtilMonotonicUs();
        timerWheelRegister(tw, dayUs, 0, testCountingCallback, &state);

        timerWheelSystemMonotonicUs next = timerWheelNextTimerEventStartUs(tw);
        const uint64_t after = timeUtilMonotonicUs();
        if (!twDueWithin_(next, (int64_t)before, (int64_t)after, dayUs)) {
            ERR("Overflow-timer query %" PRId64 " is not a day after the "
                "registration",
                (int64_t)next);
        }

        /* Overflow plus a nearer wheel timer: min must win */
        timerWheelRegister(tw, 5000, 0, testCountingCallback, &state);
        timerWheelUs offset = timerWheelNextTimerEventOffsetFromNowUs(tw);
        if (offset > 1'000'000) {
            ERR("Nearer wheel timer masked by overflow entry: %" PRId64 "us",
                offset);
        }
    }

    /* The next-event query returns the minimum expiry over every live
     * stored entry, and never 0 while one is stored.  An event loop that
     * sleeps without a timeout when the query answers 0 would otherwise
     * stop processing, and an answer later than the minimum would sleep
     * past a due timer. */

    TEST("timerWheel: tombstone-only nearest wheel-0 slot does not mask a "
         "later live timer") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState a = {};
        testCallbackState b = {};
        const uint64_t aDelayUs = 10000;
        const uint64_t bDelayUs = 100'000;

        const uint64_t before = timeUtilMonotonicUs();
        const timerWheelId aId =
            timerWheelRegister(tw, aDelayUs, 0, testCountingCallback, &a);
        timerWheelRegister(tw, bDelayUs, 0, testCountingCallback, &b);
        timerWheelUnregister(tw, aId);

        const timerWheelSystemMonotonicUs next =
            timerWheelNextTimerEventStartUs(tw);
        const uint64_t after = timeUtilMonotonicUs();
        if (next == 0) {
            ERR("A live timer was masked by a tombstone-only nearer slot: "
                "the query answered no timer");
        } else if (!twDueWithin_(next, (int64_t)before, (int64_t)after,
                                 bDelayUs)) {
            ERR("Query should name the live %" PRIu64 "us timer, got %" PRId64,
                bDelayUs, (int64_t)next);
        }

        timerWheelAdvanceTime(tw, bDelayUs + aDelayUs);
        if (a.callCount != 0 || b.callCount != 1) {
            ERR("Expected the cancelled timer silent and the live one fired "
                "once: %d, %d",
                a.callCount, b.callCount);
        }
    }

    TEST("timerWheel: cancelled higher-wheel slot does not mask a later "
         "one") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState q = {};
        testCallbackState p = {};
        const uint64_t qDelayUs = 300'000; /* wheel 1, slot +1 */
        const uint64_t pDelayUs = 600'000; /* wheel 1, slot +2 */

        const uint64_t before = timeUtilMonotonicUs();
        const timerWheelId qId =
            timerWheelRegister(tw, qDelayUs, 0, testCountingCallback, &q);
        timerWheelRegister(tw, pDelayUs, 0, testCountingCallback, &p);
        timerWheelUnregister(tw, qId);

        const timerWheelSystemMonotonicUs next =
            timerWheelNextTimerEventStartUs(tw);
        const uint64_t after = timeUtilMonotonicUs();
        if (next == 0) {
            ERR("A live wheel-1 timer was masked by a cancelled nearer "
                "wheel-1 slot: the query answered no timer");
        } else if (!twDueWithin_(next, (int64_t)before, (int64_t)after,
                                 pDelayUs)) {
            ERR("Query should name the live %" PRIu64 "us timer, got %" PRId64,
                pDelayUs, (int64_t)next);
        }

        timerWheelAdvanceTime(tw, pDelayUs + qDelayUs);
        if (q.callCount != 0 || p.callCount != 1) {
            ERR("Expected the cancelled timer silent and the live one fired "
                "once: %d, %d",
                q.callCount, p.callCount);
        }
    }

    TEST("timerWheel: wrapped wheel-0 placement does not delay a nearer "
         "timer") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testLaggingClockState s;
        memset(&s, 0, sizeof(s));
        s.lagUs = 200'000;
        s.nearDelayUs = 50000;
        s.farDelayUs = 100'000;

        /* Fired from a wheel-0 slot, so its registrations are placed by
         * processPending against the lagging wheel clock: the far
         * timer's wheel-clock offset (lag + 100ms) exceeds the 256-slot
         * wheel, so a level taken from its 100ms delay would wrap it
         * into a slot nearer in ring order than the near timer's
         * (lag + 50ms); from the wheel clock it takes wheel 1. */
        timerWheelRegister(tw, 1000, 0, testLagThenRegisterCallback, &s);
        timerWheelAdvanceTime(tw, 2000);
        if (s.self.callCount != 1) {
            ERR("Registrar callback did not fire, callCount=%d",
                s.self.callCount);
        }

        const timerWheelSystemMonotonicUs next =
            timerWheelNextTimerEventStartUs(tw);
        const uint64_t after = timeUtilMonotonicUs();
        if (next == 0 || !twDueWithin_(next, (int64_t)s.registeredAtUs,
                                       (int64_t)after, s.nearDelayUs)) {
            ERR("Query should name the near %" PRIu64
                "us timer, not the wrapped far one: got %" PRId64,
                s.nearDelayUs, (int64_t)next);
        }

        /* Both still fire, each at its own time */
        timerWheelAdvanceTime(tw, s.nearDelayUs + 10000);
        if (s.nearTimer.callCount != 1 || s.farTimer.callCount != 0) {
            ERR("At ~60ms only the near timer should have fired: %d, %d",
                s.nearTimer.callCount, s.farTimer.callCount);
        }
        timerWheelAdvanceTime(tw, s.farDelayUs);
        if (s.farTimer.callCount != 1) {
            ERR("Far timer did not fire, callCount=%d", s.farTimer.callCount);
        }
    }

    TEST("timerWheel: cancel of another timer inside a firing callback") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCancelOtherState p;
        memset(&p, 0, sizeof(p));
        testCallbackState q = {};
        const uint64_t periodUs = 20000;
        const uint64_t qDelayUs = 30000;
        const uint64_t stepUs = 10000;
        const uint64_t spanUs = 200'000;

        timerWheelRegister(tw, periodUs, periodUs, testCancelOtherCallback, &p);
        p.victim =
            timerWheelRegister(tw, qDelayUs, 0, testCountingCallback, &q);

        for (uint64_t advanced = 0; advanced < spanUs; advanced += stepUs) {
            timerWheelAdvanceTime(tw, stepUs);

            /* P is live after every pass, so the query never answers 0;
             * between P's first fire and Q's slot, Q's tombstone is the
             * nearest wheel-0 slot */
            const timerWheelSystemMonotonicUs next =
                timerWheelNextTimerEventStartUs(tw);
            if (next == 0) {
                ERR("Query answered no timer after %" PRIu64
                    "us with the periodic timer live",
                    advanced + stepUs);
                break;
            }
            const timerWheelUs offset =
                timerWheelNextTimerEventOffsetFromNowUs(tw);
            if (offset > (timerWheelUs)(periodUs + 2000)) {
                ERR("Query named %" PRId64 "us, beyond one period of the "
                    "periodic timer, after %" PRIu64 "us",
                    offset, advanced + stepUs);
            }
        }

        if (q.callCount != 0) {
            ERR("Cancelled timer fired, callCount=%d", q.callCount);
        }
        /* A repeat is re-placed one wheel-0 slot past its firing slot,
         * so the period as fired is periodUs plus one resolution */
        if (p.self.callCount < 8 || p.self.callCount > 10) {
            ERR("Periodic timer fired %d times over %" PRIu64
                "us, expected 8-10",
                p.self.callCount, spanUs);
        }
        if (timerWheelCount(tw) != 1) {
            ERR("Expected only the periodic timer live, count=%zu",
                timerWheelCount(tw));
        }
    }

    TEST("timerWheel: a phase-heavy wheel-1 placement does not hide a "
         "nearer timer in the next slot") {
        /* A slot is floor(cur / R) + floor((E - cur) / R), which is
         * floor(E / R) or one less, so a higher wheel's slot order is
         * expiry order only to within one slot.  With the wheel clock
         * 200ms into wheel 1's current slot, the 400ms timer X lands
         * one slot on while the earlier 520ms timer Y sits two slots
         * on: the first live slot alone names X. */
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState y = {};
        testCallbackState x = {};
        const uint64_t yDelayUs = 520000; /* wheel 1, slot +2 */
        const uint64_t phaseUs = 200'000;
        const uint64_t xDelayUs = 400'000; /* wheel 1, slot +1 from 200ms */
        const uint64_t marginUs = 20000;

        const uint64_t beforeY = timeUtilMonotonicUs();
        timerWheelRegister(tw, yDelayUs, 0, testCountingCallback, &y);
        const uint64_t afterY = timeUtilMonotonicUs();
        timerWheelAdvanceTime(tw, phaseUs);
        timerWheelRegister(tw, xDelayUs, 0, testCountingCallback, &x);

        /* The advance moved the wheel's time base back by phaseUs, so
         * Y's absolute due time is phaseUs earlier than registered */
        const timerWheelSystemMonotonicUs next =
            timerWheelNextTimerEventStartUs(tw);
        if (next == 0 || !twDueWithin_(next, (int64_t)beforeY, (int64_t)afterY,
                                       yDelayUs - phaseUs)) {
            ERR("Query should name the %" PRIu64
                "us timer in the next wheel-1 slot, not the %" PRIu64
                "us one: got %" PRId64,
                yDelayUs, xDelayUs, (int64_t)next);
        }

        /* Y fires before X, each at its own time */
        timerWheelAdvanceTime(tw, yDelayUs - phaseUs + marginUs);
        if (y.callCount != 1 || x.callCount != 0) {
            ERR("At ~%" PRIu64 "us only the %" PRIu64
                "us timer should have fired: %d, %d",
                yDelayUs + marginUs, yDelayUs, y.callCount, x.callCount);
        }
        timerWheelAdvanceTime(tw, phaseUs + xDelayUs - yDelayUs);
        if (x.callCount != 1) {
            ERR("The %" PRIu64 "us timer did not fire, callCount=%d", xDelayUs,
                x.callCount);
        }
    }

    TEST("timerWheel: a lagging-clock registration does not wrap past a "
         "nearer one in a higher wheel") {
        /* The lagging-clock registrar with a 10s lag: a level taken from
         * the delay measured from `now` (3s and 7s, both wheel 1) with
         * the slot offset measured from the wheel clock (13s and 17s)
         * wraps the 7s timer past wheel 1's 64 slots to a slot nearer
         * in ring order than the 3s timer's.  From the wheel clock the
         * 7s timer's 17s offset takes wheel 2. */
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testLaggingClockState s;
        memset(&s, 0, sizeof(s));
        s.lagUs = 10'000'000;
        s.nearDelayUs = 3000000;
        s.farDelayUs = 7000000;
        const uint64_t marginUs = 10000;

        timerWheelRegister(tw, 1000, 0, testLagThenRegisterCallback, &s);
        timerWheelAdvanceTime(tw, 2000);
        if (s.self.callCount != 1) {
            ERR("Registrar callback did not fire, callCount=%d",
                s.self.callCount);
        }

        const timerWheelSystemMonotonicUs next =
            timerWheelNextTimerEventStartUs(tw);
        const uint64_t after = timeUtilMonotonicUs();
        if (next == 0 || !twDueWithin_(next, (int64_t)s.registeredAtUs,
                                       (int64_t)after, s.nearDelayUs)) {
            ERR("Query should name the near %" PRIu64
                "us timer, not the far %" PRIu64
                "us one wrapped nearer in wheel 1: got %" PRId64,
                s.nearDelayUs, s.farDelayUs, (int64_t)next);
        }

        /* Both fire, each at its own time */
        timerWheelAdvanceTime(tw, s.nearDelayUs + marginUs);
        if (s.nearTimer.callCount != 1 || s.farTimer.callCount != 0) {
            ERR("At ~%" PRIu64 "us only the near timer should have "
                "fired: %d, %d",
                s.nearDelayUs + marginUs, s.nearTimer.callCount,
                s.farTimer.callCount);
        }
        timerWheelAdvanceTime(tw, s.farDelayUs - s.nearDelayUs);
        if (s.farTimer.callCount != 1) {
            ERR("Far timer did not fire, callCount=%d", s.farTimer.callCount);
        }
    }

    TEST("timerWheel: overflow repeating timer round-trips") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = true};

        /* 24h initial delay and repeat: lives in overflow (>18.6h),
         * fires, and must reschedule back into overflow */
        const uint64_t dayUs = 24ULL * 60 * 60 * 1'000'000;
        timerWheelRegister(tw, dayUs, dayUs, testCountingCallback, &state);

        timerWheelStats stats;
        timerWheelGetStats(tw, &stats);
        if (stats.overflowCount != 1) {
            ERR("Expected 1 overflow timer, got %zu", stats.overflowCount);
        }

        timerWheelAdvanceTime(tw, dayUs + 1'000'000);
        if (state.callCount != 1) {
            ERR("Overflow timer did not fire, callCount=%d", state.callCount);
        }

        timerWheelGetStats(tw, &stats);
        if (stats.overflowCount != 1) {
            ERR("Rescheduled timer should be back in overflow, got %zu",
                stats.overflowCount);
        }
        if (timerWheelCount(tw) != 1) {
            ERR("Expected 1 live timer, got %zu", timerWheelCount(tw));
        }
    }

    TEST("timerWheel: a cancelled overflow timer is not the next event") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {};
        const uint64_t hourUs = UINT64_C(60) * 60 * 1'000'000;

        /* A cancelled overflow entry stays stored until processing pulls
         * it into range, which is hours away. */
        const timerWheelId only = timerWheelRegister(
            tw, 20 * hourUs, 0, testCountingCallback, &state);
        timerWheelUnregister(tw, only);
        timerWheelProcessTimerEvents(tw);
        if (timerWheelCount(tw) != 0) {
            ERR("Expected no live timer, got %zu", timerWheelCount(tw));
        }
        if (timerWheelNextTimerEventStartUs(tw) != 0) {
            ERR("A wheel holding only a cancelled overflow timer must "
                "report no next event");
        }

        /* A cancelled overflow head must not hide the live one behind
         * it. */
        const uint64_t before = timeUtilMonotonicUs();
        const timerWheelId head = timerWheelRegister(
            tw, 20 * hourUs, 0, testCountingCallback, &state);
        timerWheelRegister(tw, 30 * hourUs, 0, testCountingCallback, &state);
        timerWheelUnregister(tw, head);
        timerWheelProcessTimerEvents(tw);
        const timerWheelSystemMonotonicUs next =
            timerWheelNextTimerEventStartUs(tw);
        const uint64_t after = timeUtilMonotonicUs();
        if (!twDueWithin_(next, (int64_t)before, (int64_t)after, 30 * hourUs)) {
            ERR("Query should name the live 30h overflow timer, got "
                "%" PRId64,
                (int64_t)next);
        }
    }

    TEST("timerWheel: next-event query matches a brute-force oracle") {
        /* Random register / cancel / advance mixes across pending, every
         * wheel level, and overflow.  After each step the query (with its
         * cache dropped, so it walks) must equal an oracle that decodes
         * every stored entry.  Before the cache is dropped, the cached
         * answer may only be early (a cancellation does not refresh it),
         * never late, and never "nothing" while a timer is live. */
#ifdef SANITIZER_BUILD
        const int rounds = 4;
        const int steps = 500;
#else
        const int rounds = 16;
        const int steps = 1500;
#endif
        uint64_t rng = UINT64_C(0x9E3779B97F4A7C15);
        uint64_t checks = 0;
        uint64_t mismatches = 0;
        uint64_t lateCached = 0;

        for (int round = 0; round < rounds; round++) {
            cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
            timerWheelId ids[512];
            size_t nids = 0;
            const bool fineSteps = (round & 1) != 0;

            for (int step = 0; step < steps; step++) {
                const uint64_t r = dkXorshift64Next(&rng) % 100;
                if (r < 40) {
                    uint64_t delay;
                    switch (dkXorshift64Next(&rng) % 5) {
                    case 0:
                        delay = dkXorshift64Next(&rng) % 3000;
                        break;
                    case 1:
                        delay = dkXorshift64Next(&rng) % 300'000;
                        break;
                    case 2:
                        delay = 200'000 + dkXorshift64Next(&rng) % 20000000;
                        break;
                    case 3:
                        delay = dkXorshift64Next(&rng) % UINT64_C(1200000000);
                        break;
                    default:
                        /* Beyond the wheels' ~18.6h coverage */
                        delay = UINT64_C(70000000000) +
                                dkXorshift64Next(&rng) % UINT64_C(10000000000);
                        break;
                    }
                    const uint64_t repeat =
                        (dkXorshift64Next(&rng) & 63) == 0
                            ? 50000 + dkXorshift64Next(&rng) % 500'000
                            : 0;
                    const timerWheelId id = timerWheelRegister(
                        tw, delay, repeat, testOracleCallback, nullptr);
                    if (nids < countof(ids)) {
                        ids[nids++] = id;
                    } else {
                        ids[dkXorshift64Next(&rng) % nids] = id;
                    }
                } else if (r < 55 && nids) {
                    timerWheelUnregister(tw,
                                         ids[dkXorshift64Next(&rng) % nids]);
                } else {
                    uint64_t advance;
                    if (fineSteps) {
                        advance = dkXorshift64Next(&rng) % 1000;
                    } else {
                        switch (dkXorshift64Next(&rng) % 4) {
                        case 0:
                            advance = dkXorshift64Next(&rng) % 1000;
                            break;
                        case 1:
                            advance = dkXorshift64Next(&rng) % 300'000;
                            break;
                        case 2:
                            advance = dkXorshift64Next(&rng) % 3000000;
                            break;
                        default:
                            advance =
                                dkXorshift64Next(&rng) % UINT64_C(400000000);
                            break;
                        }
                    }
                    timerWheelAdvanceTime(tw, advance);
                }

                const timerWheelSystemMonotonicUs want =
                    timerWheelTestNextEventOracle(tw);
                const timerWheelSystemMonotonicUs cached =
                    timerWheelNextTimerEventStartUs(tw);
                if (want != 0 && (cached == 0 || cached > want)) {
                    lateCached++;
                }

                timerWheelTestInvalidateNextEventCache(tw);
                const timerWheelSystemMonotonicUs got =
                    timerWheelNextTimerEventStartUs(tw);
                checks++;
                if (got != want) {
                    if (mismatches < 4) {
                        printf("    oracle mismatch round %d step %d: got "
                               "%" PRId64 " want %" PRId64 "\n",
                               round, step, got, want);
                    }
                    mismatches++;
                }
            }
        }

        if (mismatches != 0) {
            ERR("Next-event query disagreed with the oracle %" PRIu64
                " times in %" PRIu64 " checks",
                mismatches, checks);
        }
        if (lateCached != 0) {
            ERR("Cached next-event answer was later than the oracle %" PRIu64
                " times in %" PRIu64 " checks",
                lateCached, checks);
        }
    }

    /* ================================================================
     * Statistics Tests
     * ================================================================ */

    TEST("timerWheel: statistics tracking") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        for (int32_t i = 0; i < 100; i++) {
            timerWheelRegister(tw, 0, 0, testCountingCallback, &state);
        }

        timerWheelProcessTimerEvents(tw);

        timerWheelStats stats;
        timerWheelGetStats(tw, &stats);

        if (stats.totalRegistrations != 100) {
            ERR("Expected 100 registrations, got %zu",
                stats.totalRegistrations);
        }

        if (stats.totalExpirations != 100) {
            ERR("Expected 100 expirations, got %zu", stats.totalExpirations);
        }
    }

    /* ================================================================
     * Performance Tests - timerWheel
     * ================================================================ */

    TEST("timerWheel: PERF registration performance") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {};
        const size_t numOps = BENCH_SCALE_100K;

        PERF_TIMERS_SETUP;

        for (size_t i = 0; i < numOps; i++) {
            timerWheelRegister(tw, 1'000'000 + i, 0, testCountingCallback,
                               &state);
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(numOps, "timerWheel registrations");

        printf("    Registered %zu timers\n", timerWheelCount(tw));
    }

    TEST("timerWheel: PERF unregistration performance") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {};
        const size_t numOps = 10000;

        for (size_t i = 0; i < numOps; i++) {
            timerWheelRegister(tw, 1'000'000 + i, 0, testCountingCallback,
                               &state);
        }

        PERF_TIMERS_SETUP;

        for (size_t i = 1; i <= numOps; i++) {
            timerWheelUnregister(tw, (timerWheelId)i);
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(numOps, "timerWheel unregistrations");
    }

    TEST("timerWheel: PERF batch expiration") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        const size_t numTimers = 10000;
        int32_t totalFired = 0;

        cleanerName(zfree, States) testCallbackState *states =
            zcalloc(numTimers, sizeof(testCallbackState));

        for (size_t i = 0; i < numTimers; i++) {
            states[i].shouldReschedule = false;
            timerWheelRegister(tw, 0, 0, testCountingCallback, &states[i]);
        }

        PERF_TIMERS_SETUP;

        timerWheelProcessTimerEvents(tw);

        PERF_TIMERS_FINISH_PRINT_RESULTS(numTimers, "timerWheel expirations");

        for (size_t i = 0; i < numTimers; i++) {
            totalFired += states[i].callCount;
        }

        if ((size_t)totalFired != numTimers) {
            ERR("Expected %zu firings, got %d", numTimers, totalFired);
        }
    }

    /* ================================================================
     * Performance Comparison vs multiTimer
     * ================================================================ */

    TEST("COMPARISON: registration performance (timerWheel vs multiTimer)") {
        const size_t numOps = BENCH_SCALE_100K;
        testCallbackState state = {};

        /* timerWheel */
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        uint64_t twStart = timeUtilMonotonicNs();

        for (size_t i = 0; i < numOps; i++) {
            timerWheelRegister(tw, 1'000'000 + (i % 100'000), 0,
                               testCountingCallback, &state);
        }

        uint64_t twEnd = timeUtilMonotonicNs();
        double twNsPerOp = (double)(twEnd - twStart) / numOps;

        /* Freed before the multiTimer half so the two halves never
         * hold their timers at the same time; the cleaner skips NULL. */
        timerWheelFree(tw);
        tw = nullptr;

        /* multiTimer */
        cleaner(multiTimerFree) multiTimer *mt = multiTimerNew();
        uint64_t mtStart = timeUtilMonotonicNs();

        for (size_t i = 0; i < numOps; i++) {
            multiTimerRegister(mt, 1'000'000 + (i % 100'000), 0,
                               mtCountingCallback, &state);
        }

        uint64_t mtEnd = timeUtilMonotonicNs();
        double mtNsPerOp = (double)(mtEnd - mtStart) / numOps;

        printf("    timerWheel:  %.1f ns/registration\n", twNsPerOp);
        printf("    multiTimer:  %.1f ns/registration\n", mtNsPerOp);
        printf("    Speedup:     %.2fx\n", mtNsPerOp / twNsPerOp);
    }

    TEST("COMPARISON: batch expiration (timerWheel vs multiTimer)") {
        const size_t numTimers = 50000;
        cleanerName(zfree, States) testCallbackState *states =
            zcalloc(numTimers, sizeof(testCallbackState));

        /* timerWheel */
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        for (size_t i = 0; i < numTimers; i++) {
            states[i].shouldReschedule = false;
            timerWheelRegister(tw, 0, 0, testCountingCallback, &states[i]);
        }

        uint64_t twStart = timeUtilMonotonicNs();
        timerWheelProcessTimerEvents(tw);
        uint64_t twEnd = timeUtilMonotonicNs();

        double twNsPerOp = (double)(twEnd - twStart) / numTimers;
        /* Freed before the multiTimer half so the two halves never
         * hold their timers at the same time; the cleaner skips NULL. */
        timerWheelFree(tw);
        tw = nullptr;

        /* Reset states */
        memset(states, 0, numTimers * sizeof(testCallbackState));

        /* multiTimer */
        cleaner(multiTimerFree) multiTimer *mt = multiTimerNew();
        for (size_t i = 0; i < numTimers; i++) {
            states[i].shouldReschedule = false;
            multiTimerRegister(mt, 0, 0, mtCountingCallback, &states[i]);
        }

        uint64_t mtStart = timeUtilMonotonicNs();
        multiTimerProcessTimerEvents(mt);
        uint64_t mtEnd = timeUtilMonotonicNs();

        double mtNsPerOp = (double)(mtEnd - mtStart) / numTimers;

        printf("    timerWheel:  %.1f ns/expiration\n", twNsPerOp);
        printf("    multiTimer:  %.1f ns/expiration\n", mtNsPerOp);
        printf("    Speedup:     %.2fx\n", mtNsPerOp / twNsPerOp);
    }

    TEST("COMPARISON: bulk timer registration") {
        /* Smoke size (the printed timings are informational): timers 1 ms
         * apart crowd thousands into each higher-wheel slot, whose flex
         * grows by one realloc per element */
        const size_t numTimers = BENCH_SCALE_100K;
        testCallbackState state = {};

        printf("    Registering %zu timers...\n", numTimers);

        /* timerWheel */
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        uint64_t twStart = timeUtilMonotonicNs();

        for (size_t i = 0; i < numTimers; i++) {
            timerWheelRegister(tw, (i % 3'600'000) * 1000, 0,
                               testCountingCallback, &state);
        }

        uint64_t twEnd = timeUtilMonotonicNs();
        double twMs = (double)(twEnd - twStart) / 1'000'000.0;

        timerWheelStats twStats;
        timerWheelGetStats(tw, &twStats);

        /* Freed before the multiTimer half so the two halves never
         * hold their timers at the same time; the cleaner skips NULL. */
        timerWheelFree(tw);
        tw = nullptr;

        /* multiTimer */
        cleaner(multiTimerFree) multiTimer *mt = multiTimerNew();
        uint64_t mtStart = timeUtilMonotonicNs();

        for (size_t i = 0; i < numTimers; i++) {
            multiTimerRegister(mt, (i % 3'600'000) * 1000, 0,
                               mtCountingCallback, &state);
        }

        uint64_t mtEnd = timeUtilMonotonicNs();
        double mtMs = (double)(mtEnd - mtStart) / 1'000'000.0;

        printf("    timerWheel:  %.1f ms, %zu bytes\n", twMs,
               twStats.memoryBytes);
        printf("    multiTimer:  %.1f ms\n", mtMs);
        printf("    Time speedup: %.2fx\n", mtMs / twMs);
    }

    TEST("COMPARISON: mixed operations simulation") {
        const size_t warmupTimers = BENCH_SCALE_100K;
        const size_t ops = BENCH_SCALE_50K;
        testCallbackState state = {.callCount = 0, .shouldReschedule = true};

        printf("    Simulating %zu mixed ops with %zu existing timers...\n",
               ops, warmupTimers);

        /* timerWheel */
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        for (size_t i = 0; i < warmupTimers; i++) {
            timerWheelRegister(tw, (i % 1000) * 1000 + 1'000'000, 0,
                               testCountingCallback, &state);
        }

        uint64_t twStart = timeUtilMonotonicNs();

        for (size_t i = 0; i < ops; i++) {
            timerWheelId id = timerWheelRegister(tw, 1000 + (i % 10000), 0,
                                                 testCountingCallback, &state);
            if (i % 3 == 0) {
                timerWheelUnregister(tw, id);
            }
            if (i % 100 == 0) {
                timerWheelProcessTimerEvents(tw);
            }
            (void)timerWheelNextTimerEventStartUs(tw);
        }

        uint64_t twEnd = timeUtilMonotonicNs();
        /* Freed before the multiTimer half so the two halves never
         * hold their timers at the same time; the cleaner skips NULL. */
        timerWheelFree(tw);
        tw = nullptr;

        /* multiTimer */
        cleaner(multiTimerFree) multiTimer *mt = multiTimerNew();
        for (size_t i = 0; i < warmupTimers; i++) {
            multiTimerRegister(mt, (i % 1000) * 1000 + 1'000'000, 0,
                               mtCountingCallback, &state);
        }

        uint64_t mtStart = timeUtilMonotonicNs();

        for (size_t i = 0; i < ops; i++) {
            multiTimerId id = multiTimerRegister(mt, 1000 + (i % 10000), 0,
                                                 mtCountingCallback, &state);
            if (i % 3 == 0) {
                multiTimerUnregister(mt, id);
            }
            if (i % 100 == 0) {
                multiTimerProcessTimerEvents(mt);
            }
            (void)multiTimerNextTimerEventStartUs(mt);
        }

        uint64_t mtEnd = timeUtilMonotonicNs();

        double twNsPerOp = (double)(twEnd - twStart) / ops;
        double mtNsPerOp = (double)(mtEnd - mtStart) / ops;

        printf("    timerWheel:  %.1f ns/mixed-op\n", twNsPerOp);
        printf("    multiTimer:  %.1f ns/mixed-op\n", mtNsPerOp);
        printf("    Speedup:     %.2fx\n", mtNsPerOp / twNsPerOp);
    }

    TEST("timerWheel: memory efficiency") {
        const size_t numTimers = BENCH_SCALE_100K;
        testCallbackState state = {};

        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();

        for (size_t i = 0; i < numTimers; i++) {
            timerWheelRegister(tw, i * 1000, 0, testCountingCallback, &state);
        }

        timerWheelStats stats;
        timerWheelGetStats(tw, &stats);

        double bytesPerTimer = (double)stats.memoryBytes / numTimers;

        printf("    Memory for %zu timers: %zu bytes (%.2f MB)\n", numTimers,
               stats.memoryBytes, (double)stats.memoryBytes / (1024 * 1024));
        printf("    Bytes per timer: %.2f\n", bytesPerTimer);
        printf("    Overflow timers: %zu\n", stats.overflowCount);

        if (bytesPerTimer > 100) {
            ERR("Memory usage too high: %.2f bytes/timer", bytesPerTimer);
        }
    }

    /* ================================================================
     * Wheel-Specific Tests
     * ================================================================ */

    TEST("timerWheel: timers across wheel levels") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState states[4] = {};

        /* Wheel 0: < 256ms */
        timerWheelRegister(tw, 100'000, 0, testCountingCallback, &states[0]);

        /* Wheel 1: 256ms - 16s */
        timerWheelRegister(tw, 5000000, 0, testCountingCallback, &states[1]);

        /* Wheel 2: 16s - 17min */
        timerWheelRegister(tw, 60000000, 0, testCountingCallback, &states[2]);

        /* Wheel 3: 17min - 18h */
        timerWheelRegister(tw, 3600000000ULL, 0, testCountingCallback,
                           &states[3]);

        timerWheelStats stats;
        timerWheelGetStats(tw, &stats);

        if (timerWheelCount(tw) != 4) {
            ERR("Expected 4 timers across levels, got %zu",
                timerWheelCount(tw));
        }

        printf("    Timers spread across 4 wheel levels\n");
        printf("    Overflow count: %zu\n", stats.overflowCount);
    }

    TEST("timerWheel: overflow bucket for very long timers") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {};

        /* Register timer for > 18.6 hours (should go to overflow) */
        uint64_t veryLongDelay = 24ULL * 60 * 60 * 1'000'000; /* 24 hours */
        timerWheelRegister(tw, veryLongDelay, 0, testCountingCallback, &state);

        timerWheelStats stats;
        timerWheelGetStats(tw, &stats);

        if (stats.overflowCount != 1) {
            ERR("Expected 1 overflow timer, got %zu", stats.overflowCount);
        }
    }

    /* ================================================================
     * Detailed Performance Benchmark Suite (timerWheel vs multiTimer)
     *
     * Uses PERF_ macros for cycle-accurate measurements and provides
     * comprehensive throughput/latency comparisons across workloads.
     * ================================================================ */

    TEST("BENCHMARK: registration throughput scaling") {
        printf(
            "    Measuring registration throughput at different scales...\n");
        testCallbackState state = {};
        const size_t scales[] = {BENCH_SCALE_1K, BENCH_SCALE_10K,
                                 BENCH_SCALE_100K, BENCH_SCALE_500K};
        const size_t numScales = countof(scales);

        printf("    %-12s  %12s  %12s  %8s\n", "Count", "timerWheel",
               "multiTimer", "Speedup");
        printf("    %-12s  %12s  %12s  %8s\n", "-----", "----------",
               "----------", "-------");

        for (size_t s = 0; s < numScales; s++) {
            const size_t count = scales[s];

            /* timerWheel */
            cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < count; i++) {
                timerWheelRegister(tw, 1'000'000 + (i % 100'000), 0,
                                   testCountingCallback, &state);
            }
            PERF_TIMERS_FINISH;
            double twNs = (double)lps.global.ns.duration / count;
            /* Freed before the multiTimer half so the two halves never
             * hold their timers at the same time; the cleaner skips
             * NULL. */
            timerWheelFree(tw);
            tw = nullptr;

            /* multiTimer */
            cleaner(multiTimerFree) multiTimer *mt = multiTimerNew();
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < count; i++) {
                multiTimerRegister(mt, 1'000'000 + (i % 100'000), 0,
                                   mtCountingCallback, &state);
            }
            PERF_TIMERS_FINISH;
            double mtNs = (double)lps.global.ns.duration / count;

            printf("    %-12zu  %9.1f ns  %9.1f ns  %7.2fx\n", count, twNs,
                   mtNs, mtNs / twNs);
        }
        printf("\n");
    }

    TEST("BENCHMARK: unregistration throughput (ID lookup)") {
        printf("    Measuring unregistration (ID lookup) performance...\n");
        testCallbackState state = {};
        const size_t scales[] = {BENCH_SCALE_1K, BENCH_SCALE_10K,
                                 BENCH_SCALE_50K};
        const size_t numScales = countof(scales);

        printf("    %-12s  %12s  %12s  %8s\n", "Count", "timerWheel",
               "multiTimer", "Speedup");
        printf("    %-12s  %12s  %12s  %8s\n", "-----", "----------",
               "----------", "-------");

        for (size_t s = 0; s < numScales; s++) {
            const size_t count = scales[s];

            /* timerWheel - register then unregister */
            cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
            for (size_t i = 0; i < count; i++) {
                timerWheelRegister(tw, 1'000'000 + i, 0, testCountingCallback,
                                   &state);
            }

            PERF_TIMERS_SETUP;
            for (size_t i = 1; i <= count; i++) {
                timerWheelUnregister(tw, (timerWheelId)i);
            }
            PERF_TIMERS_FINISH;
            double twNs = (double)lps.global.ns.duration / count;
            /* Freed before the multiTimer half so the two halves never
             * hold their timers at the same time; the cleaner skips
             * NULL. */
            timerWheelFree(tw);
            tw = nullptr;

            /* multiTimer - register then unregister */
            cleaner(multiTimerFree) multiTimer *mt = multiTimerNew();
            for (size_t i = 0; i < count; i++) {
                multiTimerRegister(mt, 1'000'000 + i, 0, mtCountingCallback,
                                   &state);
            }

            PERF_TIMERS_SETUP;
            for (size_t i = 1; i <= count; i++) {
                multiTimerUnregister(mt, (multiTimerId)i);
            }
            PERF_TIMERS_FINISH;
            double mtNs = (double)lps.global.ns.duration / count;

            printf("    %-12zu  %9.1f ns  %9.1f ns  %7.2fx\n", count, twNs,
                   mtNs, mtNs / twNs);
        }
        printf("\n");
    }

    TEST("BENCHMARK: expiration throughput (batch fire)") {
        printf("    Measuring batch timer expiration throughput...\n");
        const size_t scales[] = {BENCH_SCALE_1K, BENCH_SCALE_10K,
                                 BENCH_SCALE_50K, BENCH_SCALE_100K};
        const size_t numScales = countof(scales);

        printf("    %-12s  %12s  %12s  %8s\n", "Count", "timerWheel",
               "multiTimer", "Speedup");
        printf("    %-12s  %12s  %12s  %8s\n", "-----", "----------",
               "----------", "-------");

        for (size_t s = 0; s < numScales; s++) {
            const size_t count = scales[s];
            cleanerName(zfree, States) testCallbackState *states =
                zcalloc(count, sizeof(testCallbackState));

            /* timerWheel */
            cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
            for (size_t i = 0; i < count; i++) {
                states[i].shouldReschedule = false;
                timerWheelRegister(tw, 0, 0, testCountingCallback, &states[i]);
            }

            PERF_TIMERS_SETUP;
            timerWheelProcessTimerEvents(tw);
            PERF_TIMERS_FINISH;
            double twNs = (double)lps.global.ns.duration / count;
            /* Freed before the multiTimer half so the two halves never
             * hold their timers at the same time; the cleaner skips
             * NULL. */
            timerWheelFree(tw);
            tw = nullptr;

            /* Reset states */
            memset(states, 0, count * sizeof(testCallbackState));

            /* multiTimer */
            cleaner(multiTimerFree) multiTimer *mt = multiTimerNew();
            for (size_t i = 0; i < count; i++) {
                states[i].shouldReschedule = false;
                multiTimerRegister(mt, 0, 0, mtCountingCallback, &states[i]);
            }

            PERF_TIMERS_SETUP;
            multiTimerProcessTimerEvents(mt);
            PERF_TIMERS_FINISH;
            double mtNs = (double)lps.global.ns.duration / count;

            printf("    %-12zu  %9.1f ns  %9.1f ns  %7.2fx\n", count, twNs,
                   mtNs, mtNs / twNs);
        }
        printf("\n");
    }

    TEST("BENCHMARK: nextTimerEvent query latency") {
        printf("    Measuring next-timer-event query latency...\n");
        testCallbackState state = {};
        const size_t timerCounts[] = {100, BENCH_SCALE_1K, BENCH_SCALE_10K,
                                      BENCH_SCALE_100K};
        const size_t numCounts = countof(timerCounts);
        const size_t queries = BENCH_QUERY_COUNT;

        printf("    %-12s  %12s  %12s  %8s\n", "Timers", "timerWheel",
               "multiTimer", "Speedup");
        printf("    %-12s  %12s  %12s  %8s\n", "------", "----------",
               "----------", "-------");

        for (size_t c = 0; c < numCounts; c++) {
            const size_t count = timerCounts[c];

            /* timerWheel */
            cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
            for (size_t i = 0; i < count; i++) {
                timerWheelRegister(tw, 1'000'000 + (i * 100), 0,
                                   testCountingCallback, &state);
            }

            [[maybe_unused]] volatile int64_t sink = 0;
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < queries; i++) {
                sink += timerWheelNextTimerEventStartUs(tw);
            }
            PERF_TIMERS_FINISH;
            double twNs = (double)lps.global.ns.duration / queries;
            /* Freed before the multiTimer half so the two halves never
             * hold their timers at the same time; the cleaner skips
             * NULL. */
            timerWheelFree(tw);
            tw = nullptr;

            /* multiTimer */
            cleaner(multiTimerFree) multiTimer *mt = multiTimerNew();
            for (size_t i = 0; i < count; i++) {
                multiTimerRegister(mt, 1'000'000 + (i * 100), 0,
                                   mtCountingCallback, &state);
            }

            sink = 0;
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < queries; i++) {
                sink += multiTimerNextTimerEventStartUs(mt);
            }
            PERF_TIMERS_FINISH;
            double mtNs = (double)lps.global.ns.duration / queries;

            printf("    %-12zu  %9.1f ns  %9.1f ns  %7.2fx\n", count, twNs,
                   mtNs, mtNs / twNs);
        }
        printf("\n");
    }

    TEST("BENCHMARK: process-then-query event loop") {
        /* The event-loop shape: process 1ms of time, then ask for the
         * next event.  Processing drops the next-event cache, so every
         * query here walks the wheel.  Repeating timers keep the
         * population steady: "near" delays are uniform in 1-255ms
         * (wheel 0), "idle" ones are 20-40s timeouts (wheels 1 and 2). */
        printf("    Measuring next-event queries after each 1ms pass...\n");
        const struct {
            size_t nearCount;
            size_t idleCount;
        } shapes[] = {{100, 0},
                      {BENCH_SCALE_1K, 0},
                      {BENCH_SCALE_10K, 0},
                      {BENCH_SCALE_1K, BENCH_SCALE_10K}};
        const int iterations = BENCH_QUERY_COUNT / 50;

        printf("    %-8s  %-8s  %14s  %14s\n", "Near", "Idle", "query ns/iter",
               "process ns/iter");
        uint64_t rng = 12345;
        for (size_t s = 0; s < countof(shapes); s++) {
            cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
            for (size_t i = 0; i < shapes[s].nearCount; i++) {
                const uint64_t d = 1000 + dkXorshift64Next(&rng) % 254000;
                timerWheelRegister(tw, d, d, testOracleCallback, nullptr);
            }
            for (size_t i = 0; i < shapes[s].idleCount; i++) {
                const uint64_t d = 20000000 + dkXorshift64Next(&rng) % 20000000;
                timerWheelRegister(tw, d, d, testOracleCallback, nullptr);
            }

            volatile int64_t sink = 0;
            uint64_t processNs = 0;
            uint64_t queryNs = 0;
            for (int i = 0; i < iterations; i++) {
                const uint64_t a = timeUtilMonotonicNs();
                timerWheelAdvanceTime(tw, 1000);
                const uint64_t b = timeUtilMonotonicNs();
                sink += timerWheelNextTimerEventStartUs(tw);
                const uint64_t c = timeUtilMonotonicNs();
                processNs += b - a;
                queryNs += c - b;
            }

            printf("    %-8zu  %-8zu  %14.0f  %14.0f\n", shapes[s].nearCount,
                   shapes[s].idleCount, (double)queryNs / iterations,
                   (double)processNs / iterations);
        }
        printf("\n");
    }

    TEST("BENCHMARK: timer delay distribution impact") {
        printf("    Measuring performance across delay distributions...\n");
        testCallbackState state = {};
        const size_t count = BENCH_SCALE_50K;

        struct {
            const char *name;
            uint64_t minUs;
            uint64_t maxUs;
        } distributions[] = {
            {"Uniform short", 1000, 10000},         /* 1-10ms */
            {"Uniform medium", 100'000, 1'000'000}, /* 100ms-1s */
            {"Uniform long", 1'000'000, 60000000},  /* 1s-60s */
            {"Clustered", 5000, 5100},              /* 5ms ± 50μs */
            {"Wide spread", 1000, 3600000000ULL},   /* 1ms-1hr */
        };
        const size_t numDists = countof(distributions);

        printf("    %-16s  %12s  %12s  %8s\n", "Distribution", "timerWheel",
               "multiTimer", "Speedup");
        printf("    %-16s  %12s  %12s  %8s\n", "------------", "----------",
               "----------", "-------");

        for (size_t d = 0; d < numDists; d++) {
            uint64_t minUs = distributions[d].minUs;
            uint64_t range = distributions[d].maxUs - minUs;

            /* timerWheel */
            cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < count; i++) {
                uint64_t delay = minUs + (i * 7919) % (range + 1);
                timerWheelRegister(tw, delay, 0, testCountingCallback, &state);
            }
            PERF_TIMERS_FINISH;
            double twNs = (double)lps.global.ns.duration / count;
            /* Freed before the multiTimer half so the two halves never
             * hold their timers at the same time; the cleaner skips
             * NULL. */
            timerWheelFree(tw);
            tw = nullptr;

            /* multiTimer */
            cleaner(multiTimerFree) multiTimer *mt = multiTimerNew();
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < count; i++) {
                uint64_t delay = minUs + (i * 7919) % (range + 1);
                multiTimerRegister(mt, delay, 0, mtCountingCallback, &state);
            }
            PERF_TIMERS_FINISH;
            double mtNs = (double)lps.global.ns.duration / count;

            printf("    %-16s  %9.1f ns  %9.1f ns  %7.2fx\n",
                   distributions[d].name, twNs, mtNs, mtNs / twNs);
        }
        printf("\n");
    }

    TEST("BENCHMARK: register-then-cancel pattern") {
        printf("    Measuring register-then-immediate-cancel pattern...\n");
        testCallbackState state = {};
        const size_t scales[] = {BENCH_SCALE_10K, BENCH_SCALE_50K,
                                 BENCH_SCALE_100K};
        const size_t numScales = countof(scales);

        printf("    %-12s  %12s  %12s  %8s\n", "Count", "timerWheel",
               "multiTimer", "Speedup");
        printf("    %-12s  %12s  %12s  %8s\n", "-----", "----------",
               "----------", "-------");

        for (size_t s = 0; s < numScales; s++) {
            const size_t count = scales[s];

            /* timerWheel - register then immediately cancel */
            cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < count; i++) {
                timerWheelId id = timerWheelRegister(
                    tw, 1'000'000, 0, testCountingCallback, &state);
                timerWheelUnregister(tw, id);
            }
            PERF_TIMERS_FINISH;
            double twNs = (double)lps.global.ns.duration / count;
            /* Freed before the multiTimer half so the two halves never
             * hold their timers at the same time; the cleaner skips
             * NULL. */
            timerWheelFree(tw);
            tw = nullptr;

            /* multiTimer - register then immediately cancel */
            cleaner(multiTimerFree) multiTimer *mt = multiTimerNew();
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < count; i++) {
                multiTimerId id = multiTimerRegister(
                    mt, 1'000'000, 0, mtCountingCallback, &state);
                multiTimerUnregister(mt, id);
            }
            PERF_TIMERS_FINISH;
            double mtNs = (double)lps.global.ns.duration / count;

            printf("    %-12zu  %9.1f ns  %9.1f ns  %7.2fx\n", count, twNs,
                   mtNs, mtNs / twNs);
        }
        printf("\n");
    }

    TEST("BENCHMARK: steady-state churn simulation") {
        printf("    Simulating steady-state timer churn...\n");
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};
        const size_t baseTimers = BENCH_CHURN_BASE;
        const size_t iterations = BENCH_CHURN_ITERS;

        printf("    %zu base timers, %zu churn iterations\n\n", baseTimers,
               iterations);

        /* timerWheel steady-state */
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        for (size_t i = 0; i < baseTimers; i++) {
            timerWheelRegister(tw, 1'000'000 + (i * 100), 0,
                               testCountingCallback, &state);
        }

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            /* Register new timer */
            timerWheelId id = timerWheelRegister(tw, 500'000 + (i % 500'000), 0,
                                                 testCountingCallback, &state);
            /* Cancel ~50% */
            if (i % 2 == 0) {
                timerWheelUnregister(tw, id);
            }
            /* Periodic query (no time advance - just measure churn overhead) */
            if (i % 100 == 0) {
                (void)timerWheelNextTimerEventStartUs(tw);
            }
        }
        PERF_TIMERS_FINISH;
        double twNs = (double)lps.global.ns.duration / iterations;
        /* Freed before the multiTimer half so the two halves never
         * hold their timers at the same time; the cleaner skips NULL. */
        timerWheelFree(tw);
        tw = nullptr;

        /* multiTimer steady-state */
        cleaner(multiTimerFree) multiTimer *mt = multiTimerNew();
        for (size_t i = 0; i < baseTimers; i++) {
            multiTimerRegister(mt, 1'000'000 + (i * 100), 0, mtCountingCallback,
                               &state);
        }

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            /* Register new timer */
            multiTimerId id = multiTimerRegister(mt, 500'000 + (i % 500'000), 0,
                                                 mtCountingCallback, &state);
            /* Cancel ~50% */
            if (i % 2 == 0) {
                multiTimerUnregister(mt, id);
            }
            /* Periodic query (no time advance - just measure churn overhead) */
            if (i % 100 == 0) {
                (void)multiTimerNextTimerEventStartUs(mt);
            }
        }
        PERF_TIMERS_FINISH;
        double mtNs = (double)lps.global.ns.duration / iterations;

        printf("    timerWheel:  %.1f ns/iter\n", twNs);
        printf("    multiTimer:  %.1f ns/iter\n", mtNs);
        printf("    Speedup:     %.2fx\n\n", mtNs / twNs);
    }

    TEST("BENCHMARK: timerWheel memory at scale") {
        printf("    timerWheel memory usage at scale...\n");
        testCallbackState state = {};
        /* Smoke size, as for bulk registration above */
        const size_t scales[] = {BENCH_SCALE_1K, BENCH_SCALE_10K,
                                 BENCH_SCALE_100K};
        const size_t numScales = countof(scales);

        printf("    %-12s  %14s  %12s\n", "Timers", "Memory", "Bytes/Timer");
        printf("    %-12s  %14s  %12s\n", "------", "------", "-----------");

        for (size_t s = 0; s < numScales; s++) {
            const size_t count = scales[s];

            cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
            for (size_t i = 0; i < count; i++) {
                timerWheelRegister(tw, i * 1000, 0, testCountingCallback,
                                   &state);
            }
            timerWheelStats twStats;
            timerWheelGetStats(tw, &twStats);

            printf("    %-12zu  %10zu B    %10.1f\n", count,
                   twStats.memoryBytes, (double)twStats.memoryBytes / count);
        }
        printf("\n");
    }

    TEST("BENCHMARK: summary") {
        printf("=== BENCHMARK SUMMARY ===\n");
        printf("timerWheel advantages:\n");
        printf("  - O(1) registration (amortized)\n");
        printf("  - O(1) next-timer query\n");
        printf("  - Efficient batch expiration\n");
        printf("  - Better cache locality for dense timers\n");
        printf("\nmultiTimer advantages:\n");
        printf("  - Lower overhead under debug/sanitizers\n");
        printf("  - More predictable per-operation cost\n");
        printf("\nRecommendation: Use timerWheel for production workloads\n");
        printf("                with -O2/-O3 optimization.\n\n");
    }

    /* ================================================================
     * MULTI-LEVEL CASCADE TESTS
     *
     * Timer wheel uses 4 wheels with cascading:
     * - Wheel 0: 256 slots × 1ms = 256ms span (timers < 256ms)
     * - Wheel 1: 64 slots × 256ms = ~16.4s span (timers 256ms - 16.4s)
     * - Wheel 2: 64 slots × 16.4s = ~17.5min span
     * - Wheel 3: 64 slots × 17.5min = ~18.6h span
     *
     * Key insight: slot = (currentIndex + delay/resolution)
     * A 300ms timer goes to wheel 1 slot 1 (since 300/256 = 1).
     * Each higher-wheel slot cascades downward when the wheel ENTERS
     * it (slot 1 at 256ms, slot 2 at 512ms, ...), so its timers land
     * in lower wheels before they are due and fire within one wheel0
     * slot (1ms) of their scheduled time.
     * Cascade counter only increments when there ARE timers in the slot.
     * ================================================================ */

    printf("\n=== Multi-Level Cascade Tests ===\n\n");

    TEST("CASCADE: wheel 0 timers fire accurately") {
        /* Wheel 0 timers should fire at their scheduled time */
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {};

        /* Register timer for 100ms - stays in wheel 0 */
        timerWheelRegister(tw, 100'000, 0, testCountingCallback, &state);

        timerWheelAdvanceTime(tw, 95000);
        if (state.callCount != 0) {
            ERR("Timer fired too early at 95ms, count=%d", state.callCount);
        }

        timerWheelAdvanceTime(tw, 10000); /* Now at 105ms */
        if (state.callCount != 1) {
            ERR("Timer should fire at 100ms, count=%d", state.callCount);
        }
    }

    TEST("CASCADE: wheel 1 timer fires on time after entry cascade") {
        /* A 300ms timer goes to wheel 1 slot 1; the slot cascades when
         * entered at 256ms, dropping the timer into wheel 0 where it
         * fires on schedule at ~300ms */
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {};

        /* Register 300ms timer - goes to wheel 1 slot 1 (300/256 = 1) */
        timerWheelRegister(tw, 300'000, 0, testCountingCallback, &state);

        /* Advance to 250ms - before the cascade, still in wheel 1 */
        timerWheelAdvanceTime(tw, 250000);
        if (state.callCount != 0) {
            ERR("Timer shouldn't fire at 250ms, count=%d", state.callCount);
        }

        /* Advance to 310ms - slot 1 cascaded at 256ms, timer fired at
         * its scheduled 300ms */
        timerWheelAdvanceTime(tw, 60000);
        if (state.callCount != 1) {
            ERR("Timer should fire on time at ~300ms, count=%d",
                state.callCount);
        }
    }

    TEST("CASCADE: cascade counter only increments for non-empty slots") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {};

        /* Register 520ms timer - goes to wheel 1 slot 2 (520/256 = 2) */
        timerWheelRegister(tw, 520000, 0, testCountingCallback, &state);

        timerWheelStats stats;

        /* Advance to 256ms - slot 1 entered (empty), counter stays 0 */
        timerWheelAdvanceTime(tw, 256000);
        timerWheelGetStats(tw, &stats);
        if (stats.totalCascades != 0) {
            ERR("Expected 0 cascades at 256ms (slot 1 empty), got %zu",
                stats.totalCascades);
        }

        /* Timer should not have fired yet */
        if (state.callCount != 0) {
            ERR("Timer shouldn't fire before its slot cascades, count=%d",
                state.callCount);
        }

        /* Advance to 512ms - slot 2 entered (has timer!), counter = 1;
         * the timer drops to wheel 0 but is not due until 520ms */
        timerWheelAdvanceTime(tw, 256000);
        timerWheelGetStats(tw, &stats);
        if (stats.totalCascades != 1) {
            ERR("Expected 1 cascade at 512ms (slot 2 has timer), got %zu",
                stats.totalCascades);
        }
        if (state.callCount != 0) {
            ERR("Timer shouldn't fire before 520ms, count=%d", state.callCount);
        }

        /* Advance to 530ms - timer fires at its scheduled 520ms */
        timerWheelAdvanceTime(tw, 18000);
        if (state.callCount != 1) {
            ERR("Timer should fire on time at ~520ms, count=%d",
                state.callCount);
        }
    }

    TEST("CASCADE: multiple timers in same slot cascade together") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState states[3] = {};

        /* All these go to wheel 1 slot 1 (since 260-400/256 = 1) */
        timerWheelRegister(tw, 260000, 0, testCountingCallback, &states[0]);
        timerWheelRegister(tw, 350000, 0, testCountingCallback, &states[1]);
        timerWheelRegister(tw, 400'000, 0, testCountingCallback, &states[2]);

        /* Advance to 250ms - slot 1 not yet entered, nothing fires */
        timerWheelAdvanceTime(tw, 250000);
        int firedBefore =
            states[0].callCount + states[1].callCount + states[2].callCount;
        if (firedBefore != 0) {
            ERR("Timers shouldn't fire before cascade, count=%d", firedBefore);
        }

        /* Advance to 310ms - slot 1 cascaded at 256ms as one event, and
         * each timer then fires at its OWN scheduled time: only the
         * 260ms timer is due so far */
        timerWheelAdvanceTime(tw, 60000);
        if (states[0].callCount != 1 || states[1].callCount != 0 ||
            states[2].callCount != 0) {
            ERR("Only the 260ms timer should have fired: %d, %d, %d",
                states[0].callCount, states[1].callCount, states[2].callCount);
        }

        /* Advance to 410ms - the 350ms and 400ms timers fire on time */
        timerWheelAdvanceTime(tw, 100'000);
        int firedAfter =
            states[0].callCount + states[1].callCount + states[2].callCount;
        if (firedAfter != 3) {
            ERR("All 3 timers should have fired by 410ms, count=%d",
                firedAfter);
        }

        timerWheelStats stats;
        timerWheelGetStats(tw, &stats);
        if (stats.totalCascades != 1) {
            ERR("Expected exactly 1 cascade event, got %zu",
                stats.totalCascades);
        }
    }

    TEST(
        "CASCADE: timers inserted into wheel 0 after cascade fire accurately") {
        /* When a timer cascades to wheel 0, it fires at its own slot */
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {};

        /* 700ms timer goes to wheel 1 slot 2 (700/256 = 2); slot 2 is
         * entered (cascaded) at 512ms, leaving 188ms in wheel 0 */
        timerWheelRegister(tw, 700000, 0, testCountingCallback, &state);

        /* Advance to 690ms - cascaded but not yet due */
        timerWheelAdvanceTime(tw, 690000);
        if (state.callCount != 0) {
            ERR("Timer shouldn't fire before 700ms, count=%d", state.callCount);
        }

        /* Advance to 720ms - fires at its scheduled 700ms */
        timerWheelAdvanceTime(tw, 30000);
        if (state.callCount != 1) {
            ERR("Timer should fire on time at ~700ms, count=%d",
                state.callCount);
        }
    }

    TEST("CASCADE: timer cascades to wheel 0 and fires at correct slot") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {};

        /* 780ms timer: wheel 1 slot 3 (780/256 = 3), entered at 768ms
         * with 12ms remaining in wheel 0 */
        timerWheelRegister(tw, 780000, 0, testCountingCallback, &state);

        /* Advance to 770ms - cascaded at 768ms, not yet due */
        timerWheelAdvanceTime(tw, 770000);
        if (state.callCount != 0) {
            ERR("Timer shouldn't fire before 780ms, count=%d", state.callCount);
        }

        /* Advance to 790ms - fires at its scheduled 780ms */
        timerWheelAdvanceTime(tw, 20000);
        if (state.callCount != 1) {
            ERR("Timer should fire on time at ~780ms, count=%d",
                state.callCount);
        }
    }

    TEST("CASCADE: nested wheel 2 to wheel 1 to wheel 0") {
        /* Timer far enough to start in wheel 2, then cascade down */
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {};

        /* 20s timer: wheel 1 span = 16.4s, so this goes to wheel 2 */
        /* Wheel 1 slot = 20000000 / (64*256000) = 1.2, so slot 1 of wheel 2 */
        timerWheelRegister(tw, 20000000, 0, testCountingCallback, &state);

        timerWheelStats stats;

        /* Verify timer is registered */
        if (timerWheelCount(tw) != 1) {
            ERR("Expected 1 timer, got %zu", timerWheelCount(tw));
        }

        /* Advance 16.4s (wheel 1 span) - wheel 1 wraps, wheel 2 slot 0 cascades
         */
        timerWheelAdvanceTime(tw, 16384000); /* 64 * 256ms = 16.384s */
        timerWheelGetStats(tw, &stats);

        /* Timer is in wheel 2 slot 1, slot 0 cascades (empty) */
        if (state.callCount != 0) {
            ERR("Timer shouldn't fire yet, count=%d", state.callCount);
        }

        /* Advance another 16.4s - wheel 2 slot 1 cascades */
        timerWheelAdvanceTime(tw, 16384000); /* Now at ~32.8s */
        timerWheelGetStats(tw, &stats);

        /* Timer should have cascaded from wheel 2 → wheel 1, then possibly
         * fired */
        /* At 32.8s, a 20s timer is overdue by 12.8s, should fire */
        if (state.callCount != 1) {
            ERR("Timer should fire after cascading from wheel 2, count=%d",
                state.callCount);
        }

        if (stats.totalCascades < 1) {
            ERR("Expected at least 1 cascade (from wheel 2), got %zu",
                stats.totalCascades);
        }

        printf("    20s timer fired after %zu cascades\n", stats.totalCascades);
    }

    TEST("CASCADE: cancellation removes timer before cascade") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState states[3] = {};

        /* All go to wheel 1 slot 1 */
        [[maybe_unused]] timerWheelId id1 = timerWheelRegister(
            tw, 300'000, 0, testCountingCallback, &states[0]);
        timerWheelId id2 =
            timerWheelRegister(tw, 350000, 0, testCountingCallback, &states[1]);
        [[maybe_unused]] timerWheelId id3 = timerWheelRegister(
            tw, 400'000, 0, testCountingCallback, &states[2]);

        /* Cancel middle timer before cascade */
        timerWheelUnregister(tw, id2);

        /* Advance past cascade point (512ms) */
        timerWheelAdvanceTime(tw, 520000);

        /* First and third should fire, second (cancelled) should not */
        if (states[0].callCount != 1) {
            ERR("First timer should fire, count=%d", states[0].callCount);
        }
        if (states[1].callCount != 0) {
            ERR("Cancelled timer should NOT fire, count=%d",
                states[1].callCount);
        }
        if (states[2].callCount != 1) {
            ERR("Third timer should fire, count=%d", states[2].callCount);
        }
    }

    TEST("CASCADE: repeated timer reschedules after cascade fire") {
        cleaner(timerWheelFree) timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = true};

        /* 300ms initial delay, 100ms repeat - starts in wheel 1
         * First fire at cascade (512ms), then reschedules every 100ms */
        timerWheelRegister(tw, 300'000, 100'000, testCountingCallback, &state);

        /* Advance to 520ms - fires at cascade (~512ms) */
        timerWheelAdvanceTime(tw, 520000);
        if (state.callCount < 1) {
            ERR("Timer should fire after cascade, count=%d", state.callCount);
        }

        int countAfterCascade = state.callCount;

        /* Advance another 500ms (to 1020ms) - should fire ~5 more times */
        timerWheelAdvanceTime(tw, 500'000);

        /* With 100ms repeat, expect 4-6 more fires in 500ms */
        int additionalFires = state.callCount - countAfterCascade;
        if (additionalFires < 4 || additionalFires > 6) {
            ERR("Expected 4-6 additional fires with 100ms repeat, got %d",
                additionalFires);
        }

        printf("    Timer fired %d times total (%d after cascade)\n",
               state.callCount, additionalFires);
    }

    printf("=== Multi-Level Cascade Tests Complete ===\n\n");

    TEST_FINAL_RESULT;
}
#endif
