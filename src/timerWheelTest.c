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
#include "ctest.h"

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
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SANITIZER_BUILD 1
#endif

#ifdef SANITIZER_BUILD
/* Reduced scales for sanitizer builds (~100x smaller for large tests) */
#define BENCH_SCALE_1K 100
#define BENCH_SCALE_10K 1000
#define BENCH_SCALE_50K 5000
#define BENCH_SCALE_100K 10000
#define BENCH_SCALE_500K 50000
#define BENCH_SCALE_1M 100000
#define BENCH_QUERY_COUNT 10000
#define BENCH_CHURN_BASE 1000
#define BENCH_CHURN_ITERS 10000
#else
/* Full scales for regular builds */
#define BENCH_SCALE_1K 1000
#define BENCH_SCALE_10K 10000
#define BENCH_SCALE_50K 50000
#define BENCH_SCALE_100K 100000
#define BENCH_SCALE_500K 500000
#define BENCH_SCALE_1M 1000000
#define BENCH_QUERY_COUNT 100000
#define BENCH_CHURN_BASE 10000
#define BENCH_CHURN_ITERS 100000
#endif

/* ====================================================================
 * Test State and Helpers
 * ==================================================================== */

typedef struct testCallbackState {
    int32_t callCount;
    timerWheelId lastId;
    bool shouldReschedule;
} testCallbackState;

static bool testCountingCallback(timerWheel *tw, timerWheelId id,
                                 void *clientData) {
    (void)tw;
    testCallbackState *state = clientData;
    state->callCount++;
    state->lastId = id;
    return state->shouldReschedule;
}

static bool testNestedTimerCallback(timerWheel *tw, timerWheelId id,
                                    void *clientData) {
    (void)id;
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

/* Equivalent callbacks for multiTimer comparison */
static bool mtCountingCallback(multiTimer *t, multiTimerId id,
                               void *clientData) {
    (void)t;
    testCallbackState *state = clientData;
    state->callCount++;
    state->lastId = id;
    return state->shouldReschedule;
}

int timerWheelTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int32_t err = 0;

    /* ================================================================
     * Basic Lifecycle Tests
     * ================================================================ */

    TEST("timerWheel: create and free") {
        timerWheel *tw = timerWheelNew();
        if (!tw) {
            ERRR("Failed to create timerWheel");
        } else {
            timerWheelFree(tw);
        }
    }

    TEST("timerWheel: free NULL safety") {
        timerWheelFree(NULL);
    }

    /* ================================================================
     * Timer Registration Tests
     * ================================================================ */

    TEST("timerWheel: register single timer") {
        timerWheel *tw = timerWheelNew();
        testCallbackState state = {0};

        timerWheelId id =
            timerWheelRegister(tw, 1000, 0, testCountingCallback, &state);
        if (id == 0) {
            ERRR("Timer ID should not be 0");
        }

        if (timerWheelCount(tw) != 1) {
            ERR("Expected 1 timer, got %zu", timerWheelCount(tw));
        }

        timerWheelFree(tw);
    }

    TEST("timerWheel: register multiple timers") {
        timerWheel *tw = timerWheelNew();
        testCallbackState state = {0};

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

        timerWheelFree(tw);
    }

    /* ================================================================
     * Timer Execution Tests
     * ================================================================ */

    TEST("timerWheel: timer fires after delay") {
        timerWheel *tw = timerWheelNew();
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

        timerWheelFree(tw);
    }

    TEST("timerWheel: repeating timer") {
        timerWheel *tw = timerWheelNew();
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
            ERRR("Repeating timer was incorrectly removed");
        }

        timerWheelFree(tw);
    }

    TEST("timerWheel: timer ordering") {
        timerWheel *tw = timerWheelNew();
        testCallbackState states[3] = {{0}, {0}, {0}};

        timerWheelRegister(tw, 15000, 0, testCountingCallback, &states[2]);
        timerWheelRegister(tw, 5000, 0, testCountingCallback, &states[0]);
        timerWheelRegister(tw, 10000, 0, testCountingCallback, &states[1]);

        timerWheelAdvanceTime(tw, 20000);

        if (states[0].callCount != 1 || states[1].callCount != 1 ||
            states[2].callCount != 1) {
            ERR("Not all timers fired: %d, %d, %d", states[0].callCount,
                states[1].callCount, states[2].callCount);
        }

        timerWheelFree(tw);
    }

    /* ================================================================
     * Timer Unregistration Tests
     * ================================================================ */

    TEST("timerWheel: unregister timer before fire") {
        timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        timerWheelId id =
            timerWheelRegister(tw, 100000, 0, testCountingCallback, &state);
        timerWheelUnregister(tw, id);

        timerWheelAdvanceTime(tw, 150000);

        if (state.callCount != 0) {
            ERR("Unregistered timer fired, callCount=%d", state.callCount);
        }

        timerWheelFree(tw);
    }

    TEST("timerWheel: unregister multiple timers") {
        timerWheel *tw = timerWheelNew();
        testCallbackState states[5] = {{0}, {0}, {0}, {0}, {0}};

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

        timerWheelFree(tw);
    }

    TEST("timerWheel: stopAll") {
        timerWheel *tw = timerWheelNew();
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

        timerWheelFree(tw);
    }

    /* ================================================================
     * Nested Timer Operations Tests
     * ================================================================ */

    TEST("timerWheel: register timer from within callback") {
        timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        timerWheelRegister(tw, 5000, 0, testNestedTimerCallback, &state);

        timerWheelAdvanceTime(tw, 10000);

        if (state.callCount != 1) {
            ERR("First timer did not fire, callCount=%d", state.callCount);
        }

        if (timerWheelCount(tw) == 0) {
            ERRR("Nested timer was not scheduled");
        }

        timerWheelAdvanceTime(tw, 5000);

        if (state.callCount != 2) {
            ERR("Nested timer did not fire, callCount=%d", state.callCount);
        }

        timerWheelFree(tw);
    }

    TEST("timerWheel: self-unregister from callback") {
        timerWheel *tw = timerWheelNew();
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

        timerWheelFree(tw);
    }

    /* ================================================================
     * Next Timer Event Tests
     * ================================================================ */

    TEST("timerWheel: nextTimerEventStartUs with no timers") {
        timerWheel *tw = timerWheelNew();

        timerWheelSystemMonotonicUs next = timerWheelNextTimerEventStartUs(tw);
        if (next != 0) {
            ERR("Expected 0 for empty timer, got %" PRId64, next);
        }

        timerWheelFree(tw);
    }

    TEST("timerWheel: nextTimerEventStartUs returns correct time") {
        timerWheel *tw = timerWheelNew();
        testCallbackState state = {0};

        uint64_t delayUs = 100000;
        uint64_t beforeRegister = (uint64_t)timeUtilMonotonicUs();

        timerWheelRegister(tw, delayUs, 0, testCountingCallback, &state);

        timerWheelSystemMonotonicUs next = timerWheelNextTimerEventStartUs(tw);

        int64_t expected = (int64_t)(beforeRegister + delayUs);
        int64_t diff = next - expected;

        if (diff < -5000 || diff > 5000) {
            ERR("nextTimerEventStartUs off by %" PRId64 "us", diff);
        }

        timerWheelFree(tw);
    }

    /* ================================================================
     * Edge Cases
     * ================================================================ */

    TEST("timerWheel: many timers") {
        timerWheel *tw = timerWheelNew();
        const int32_t numTimers = 1000;
        int32_t totalCallCount = 0;

        testCallbackState *states =
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

        zfree(states);
        timerWheelFree(tw);
    }

    TEST("timerWheel: zero delay fires immediately") {
        timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        timerWheelRegister(tw, 0, 0, testCountingCallback, &state);
        timerWheelProcessTimerEvents(tw);

        if (state.callCount != 1) {
            ERR("Zero-delay timer did not fire, callCount=%d", state.callCount);
        }

        timerWheelFree(tw);
    }

    TEST("timerWheel: callback returns false stops repeating") {
        timerWheel *tw = timerWheelNew();
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

        timerWheelFree(tw);
    }

    /* ================================================================
     * Edge Cases: Zero-Delay and Sub-Resolution Timers
     * ================================================================ */

    TEST("timerWheel: zero delay timer goes to pending queue") {
        timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        timerWheelId id =
            timerWheelRegister(tw, 0, 0, testCountingCallback, &state);
        if (id == 0) {
            ERRR("Timer ID should not be 0");
        }

        /* Timer should be in pending, not yet fired */
        if (state.callCount != 0) {
            ERR("Zero-delay timer should not fire until process, callCount=%d",
                state.callCount);
        }

        /* nextTimerEventStartUs should report the pending timer */
        timerWheelSystemMonotonicUs next = timerWheelNextTimerEventStartUs(tw);
        if (next == 0) {
            ERRR("nextTimerEventStartUs should return non-zero for pending "
                 "timer");
        }

        timerWheelProcessTimerEvents(tw);

        if (state.callCount != 1) {
            ERR("Zero-delay timer did not fire after process, callCount=%d",
                state.callCount);
        }

        timerWheelFree(tw);
    }

    TEST("timerWheel: sub-resolution repeating timer fires rapidly") {
        timerWheel *tw = timerWheelNew();
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

        timerWheelFree(tw);
    }

    TEST("timerWheel: 1μs repeating timer stays in pending queue") {
        timerWheel *tw = timerWheelNew();
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

        timerWheelFree(tw);
    }

    TEST("timerWheel: nextTimerEventStartUs with only pending timers") {
        timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        /* Register only zero-delay timers */
        timerWheelRegister(tw, 0, 0, testCountingCallback, &state);
        timerWheelRegister(tw, 0, 0, testCountingCallback, &state);
        timerWheelRegister(tw, 0, 0, testCountingCallback, &state);

        /* nextTimerEventStartUs should return non-zero */
        timerWheelSystemMonotonicUs next = timerWheelNextTimerEventStartUs(tw);
        if (next == 0) {
            ERRR("nextTimerEventStartUs should not be 0 with pending timers");
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

        timerWheelFree(tw);
    }

    TEST("timerWheel: mixed pending and wheel timers") {
        timerWheel *tw = timerWheelNew();
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
            ERRR("nextTimerEventStartUs should return non-zero");
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

        timerWheelFree(tw);
    }

    TEST("timerWheel: sub-resolution timer after wheel timer") {
        timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = true};

        /* First a normal delay, then sub-resolution repeat */
        timerWheelRegister(tw, 5000, 100, testCountingCallback, &state);

        /* Initially no fires */
        timerWheelProcessTimerEvents(tw);
        if (state.callCount != 0) {
            ERR("Timer should not fire before delay, callCount=%d",
                state.callCount);
        }

        /* After delay plus one resolution unit (wheel slot granularity),
         * timer should fire. Wheel resolution is 1000μs. */
        timerWheelAdvanceTime(tw, 6000);
        if (state.callCount != 1) {
            ERR("Timer should fire after delay, callCount=%d", state.callCount);
        }

        /* Now timer is rescheduled with 100μs sub-resolution repeat.
         * It's now in pending queue. Advance time to trigger subsequent fires
         */
        timerWheelAdvanceTime(tw, 100);
        if (state.callCount != 2) {
            ERR("Timer should fire after first sub-res advance, callCount=%d",
                state.callCount);
        }

        timerWheelAdvanceTime(tw, 100);
        if (state.callCount != 3) {
            ERR("Timer should fire after second sub-res advance, callCount=%d",
                state.callCount);
        }

        timerWheelAdvanceTime(tw, 100);
        if (state.callCount != 4) {
            ERR("Timer should fire after third sub-res advance, callCount=%d",
                state.callCount);
        }

        timerWheelFree(tw);
    }

    TEST("timerWheel: multiple sub-resolution timers") {
        timerWheel *tw = timerWheelNew();
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

        timerWheelFree(tw);
    }

    TEST("timerWheel: zero delay with repeat goes to pending then wheel") {
        timerWheel *tw = timerWheelNew();
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

        timerWheelFree(tw);
    }

    TEST("timerWheel: offset returns negative for overdue timer") {
        timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        /* Register a zero-delay timer */
        timerWheelRegister(tw, 0, 0, testCountingCallback, &state);

        /* Small delay to ensure timer is "overdue" */
        struct timespec ts = {0, 1000000}; /* 1ms */
        nanosleep(&ts, NULL);

        /* Offset should be negative or very small (timer is due) */
        timerWheelUs offset = timerWheelNextTimerEventOffsetFromNowUs(tw);
        if (offset > 5000) { /* Allow 5ms variance */
            ERR("Offset should be <= 0 for overdue timer, got %" PRId64,
                offset);
        }

        timerWheelFree(tw);
    }

    TEST("timerWheel: stress sub-resolution timers") {
        timerWheel *tw = timerWheelNew();
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
            ERRR("nextTimerEventStartUs returned 0 after stress test");
        }

        timerWheelFree(tw);
    }

    TEST("timerWheel: sub-resolution timer stops correctly") {
        timerWheel *tw = timerWheelNew();
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

        timerWheelFree(tw);
    }

    /* ================================================================
     * Statistics Tests
     * ================================================================ */

    TEST("timerWheel: statistics tracking") {
        timerWheel *tw = timerWheelNew();
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

        timerWheelFree(tw);
    }

    /* ================================================================
     * Performance Tests - timerWheel
     * ================================================================ */

    TEST("timerWheel: PERF registration performance") {
        timerWheel *tw = timerWheelNew();
        testCallbackState state = {0};
        const size_t numOps = BENCH_SCALE_100K;

        PERF_TIMERS_SETUP;

        for (size_t i = 0; i < numOps; i++) {
            timerWheelRegister(tw, 1000000 + i, 0, testCountingCallback,
                               &state);
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(numOps, "timerWheel registrations");

        printf("    Registered %zu timers\n", timerWheelCount(tw));

        timerWheelFree(tw);
    }

    TEST("timerWheel: PERF unregistration performance") {
        timerWheel *tw = timerWheelNew();
        testCallbackState state = {0};
        const size_t numOps = 10000;

        for (size_t i = 0; i < numOps; i++) {
            timerWheelRegister(tw, 1000000 + i, 0, testCountingCallback,
                               &state);
        }

        PERF_TIMERS_SETUP;

        for (size_t i = 1; i <= numOps; i++) {
            timerWheelUnregister(tw, (timerWheelId)i);
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(numOps, "timerWheel unregistrations");

        timerWheelFree(tw);
    }

    TEST("timerWheel: PERF batch expiration") {
        timerWheel *tw = timerWheelNew();
        const size_t numTimers = 10000;
        int32_t totalFired = 0;

        testCallbackState *states =
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

        zfree(states);
        timerWheelFree(tw);
    }

    /* ================================================================
     * Performance Comparison vs multiTimer
     * ================================================================ */

    TEST("COMPARISON: registration performance (timerWheel vs multiTimer)") {
        const size_t numOps = BENCH_SCALE_100K;
        testCallbackState state = {0};

        /* timerWheel */
        timerWheel *tw = timerWheelNew();
        uint64_t twStart = timeUtilMonotonicNs();

        for (size_t i = 0; i < numOps; i++) {
            timerWheelRegister(tw, 1000000 + (i % 100000), 0,
                               testCountingCallback, &state);
        }

        uint64_t twEnd = timeUtilMonotonicNs();
        double twNsPerOp = (double)(twEnd - twStart) / numOps;

        timerWheelFree(tw);

        /* multiTimer */
        multiTimer *mt = multiTimerNew();
        uint64_t mtStart = timeUtilMonotonicNs();

        for (size_t i = 0; i < numOps; i++) {
            multiTimerRegister(mt, 1000000 + (i % 100000), 0,
                               mtCountingCallback, &state);
        }

        uint64_t mtEnd = timeUtilMonotonicNs();
        double mtNsPerOp = (double)(mtEnd - mtStart) / numOps;

        multiTimerFree(mt);

        printf("    timerWheel:  %.1f ns/registration\n", twNsPerOp);
        printf("    multiTimer:  %.1f ns/registration\n", mtNsPerOp);
        printf("    Speedup:     %.2fx\n", mtNsPerOp / twNsPerOp);
    }

    TEST("COMPARISON: batch expiration (timerWheel vs multiTimer)") {
        const size_t numTimers = 50000;
        testCallbackState *states =
            zcalloc(numTimers, sizeof(testCallbackState));

        /* timerWheel */
        timerWheel *tw = timerWheelNew();
        for (size_t i = 0; i < numTimers; i++) {
            states[i].shouldReschedule = false;
            timerWheelRegister(tw, 0, 0, testCountingCallback, &states[i]);
        }

        uint64_t twStart = timeUtilMonotonicNs();
        timerWheelProcessTimerEvents(tw);
        uint64_t twEnd = timeUtilMonotonicNs();

        double twNsPerOp = (double)(twEnd - twStart) / numTimers;
        timerWheelFree(tw);

        /* Reset states */
        memset(states, 0, numTimers * sizeof(testCallbackState));

        /* multiTimer */
        multiTimer *mt = multiTimerNew();
        for (size_t i = 0; i < numTimers; i++) {
            states[i].shouldReschedule = false;
            multiTimerRegister(mt, 0, 0, mtCountingCallback, &states[i]);
        }

        uint64_t mtStart = timeUtilMonotonicNs();
        multiTimerProcessTimerEvents(mt);
        uint64_t mtEnd = timeUtilMonotonicNs();

        double mtNsPerOp = (double)(mtEnd - mtStart) / numTimers;
        multiTimerFree(mt);

        zfree(states);

        printf("    timerWheel:  %.1f ns/expiration\n", twNsPerOp);
        printf("    multiTimer:  %.1f ns/expiration\n", mtNsPerOp);
        printf("    Speedup:     %.2fx\n", mtNsPerOp / twNsPerOp);
    }

    TEST("COMPARISON: million timer registration") {
        const size_t numTimers = BENCH_SCALE_1M;
        testCallbackState state = {0};

        printf("    Registering %zu timers...\n", numTimers);

        /* timerWheel */
        timerWheel *tw = timerWheelNew();
        uint64_t twStart = timeUtilMonotonicNs();

        for (size_t i = 0; i < numTimers; i++) {
            timerWheelRegister(tw, (i % 3600000) * 1000, 0,
                               testCountingCallback, &state);
        }

        uint64_t twEnd = timeUtilMonotonicNs();
        double twMs = (double)(twEnd - twStart) / 1000000.0;

        timerWheelStats twStats;
        timerWheelGetStats(tw, &twStats);

        timerWheelFree(tw);

        /* multiTimer */
        multiTimer *mt = multiTimerNew();
        uint64_t mtStart = timeUtilMonotonicNs();

        for (size_t i = 0; i < numTimers; i++) {
            multiTimerRegister(mt, (i % 3600000) * 1000, 0, mtCountingCallback,
                               &state);
        }

        uint64_t mtEnd = timeUtilMonotonicNs();
        double mtMs = (double)(mtEnd - mtStart) / 1000000.0;
        multiTimerFree(mt);

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
        timerWheel *tw = timerWheelNew();
        for (size_t i = 0; i < warmupTimers; i++) {
            timerWheelRegister(tw, (i % 1000) * 1000 + 1000000, 0,
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
        timerWheelFree(tw);

        /* multiTimer */
        multiTimer *mt = multiTimerNew();
        for (size_t i = 0; i < warmupTimers; i++) {
            multiTimerRegister(mt, (i % 1000) * 1000 + 1000000, 0,
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
        multiTimerFree(mt);

        double twNsPerOp = (double)(twEnd - twStart) / ops;
        double mtNsPerOp = (double)(mtEnd - mtStart) / ops;

        printf("    timerWheel:  %.1f ns/mixed-op\n", twNsPerOp);
        printf("    multiTimer:  %.1f ns/mixed-op\n", mtNsPerOp);
        printf("    Speedup:     %.2fx\n", mtNsPerOp / twNsPerOp);
    }

    TEST("timerWheel: memory efficiency") {
        const size_t numTimers = BENCH_SCALE_100K;
        testCallbackState state = {0};

        timerWheel *tw = timerWheelNew();

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

        timerWheelFree(tw);
    }

    /* ================================================================
     * Wheel-Specific Tests
     * ================================================================ */

    TEST("timerWheel: timers across wheel levels") {
        timerWheel *tw = timerWheelNew();
        testCallbackState states[4] = {{0}, {0}, {0}, {0}};

        /* Wheel 0: < 256ms */
        timerWheelRegister(tw, 100000, 0, testCountingCallback, &states[0]);

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

        timerWheelFree(tw);
    }

    TEST("timerWheel: overflow bucket for very long timers") {
        timerWheel *tw = timerWheelNew();
        testCallbackState state = {0};

        /* Register timer for > 18.6 hours (should go to overflow) */
        uint64_t veryLongDelay = 24ULL * 60 * 60 * 1000000; /* 24 hours */
        timerWheelRegister(tw, veryLongDelay, 0, testCountingCallback, &state);

        timerWheelStats stats;
        timerWheelGetStats(tw, &stats);

        if (stats.overflowCount != 1) {
            ERR("Expected 1 overflow timer, got %zu", stats.overflowCount);
        }

        timerWheelFree(tw);
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
        testCallbackState state = {0};
        const size_t scales[] = {BENCH_SCALE_1K, BENCH_SCALE_10K,
                                 BENCH_SCALE_100K, BENCH_SCALE_500K};
        const size_t numScales = sizeof(scales) / sizeof(scales[0]);

        printf("    %-12s  %12s  %12s  %8s\n", "Count", "timerWheel",
               "multiTimer", "Speedup");
        printf("    %-12s  %12s  %12s  %8s\n", "-----", "----------",
               "----------", "-------");

        for (size_t s = 0; s < numScales; s++) {
            const size_t count = scales[s];

            /* timerWheel */
            timerWheel *tw = timerWheelNew();
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < count; i++) {
                timerWheelRegister(tw, 1000000 + (i % 100000), 0,
                                   testCountingCallback, &state);
            }
            PERF_TIMERS_FINISH;
            double twCycles =
                (double)(lps.global.tsc.stop - lps.global.tsc.start) / count;
            timerWheelFree(tw);

            /* multiTimer */
            multiTimer *mt = multiTimerNew();
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < count; i++) {
                multiTimerRegister(mt, 1000000 + (i % 100000), 0,
                                   mtCountingCallback, &state);
            }
            PERF_TIMERS_FINISH;
            double mtCycles =
                (double)(lps.global.tsc.stop - lps.global.tsc.start) / count;
            multiTimerFree(mt);

            printf("    %-12zu  %9.1f cy  %9.1f cy  %7.2fx\n", count, twCycles,
                   mtCycles, mtCycles / twCycles);
        }
        printf("\n");
    }

    TEST("BENCHMARK: unregistration throughput (ID lookup)") {
        printf("    Measuring unregistration (ID lookup) performance...\n");
        testCallbackState state = {0};
        const size_t scales[] = {BENCH_SCALE_1K, BENCH_SCALE_10K,
                                 BENCH_SCALE_50K};
        const size_t numScales = sizeof(scales) / sizeof(scales[0]);

        printf("    %-12s  %12s  %12s  %8s\n", "Count", "timerWheel",
               "multiTimer", "Speedup");
        printf("    %-12s  %12s  %12s  %8s\n", "-----", "----------",
               "----------", "-------");

        for (size_t s = 0; s < numScales; s++) {
            const size_t count = scales[s];

            /* timerWheel - register then unregister */
            timerWheel *tw = timerWheelNew();
            for (size_t i = 0; i < count; i++) {
                timerWheelRegister(tw, 1000000 + i, 0, testCountingCallback,
                                   &state);
            }

            PERF_TIMERS_SETUP;
            for (size_t i = 1; i <= count; i++) {
                timerWheelUnregister(tw, (timerWheelId)i);
            }
            PERF_TIMERS_FINISH;
            double twCycles =
                (double)(lps.global.tsc.stop - lps.global.tsc.start) / count;
            timerWheelFree(tw);

            /* multiTimer - register then unregister */
            multiTimer *mt = multiTimerNew();
            for (size_t i = 0; i < count; i++) {
                multiTimerRegister(mt, 1000000 + i, 0, mtCountingCallback,
                                   &state);
            }

            PERF_TIMERS_SETUP;
            for (size_t i = 1; i <= count; i++) {
                multiTimerUnregister(mt, (multiTimerId)i);
            }
            PERF_TIMERS_FINISH;
            double mtCycles =
                (double)(lps.global.tsc.stop - lps.global.tsc.start) / count;
            multiTimerFree(mt);

            printf("    %-12zu  %9.1f cy  %9.1f cy  %7.2fx\n", count, twCycles,
                   mtCycles, mtCycles / twCycles);
        }
        printf("\n");
    }

    TEST("BENCHMARK: expiration throughput (batch fire)") {
        printf("    Measuring batch timer expiration throughput...\n");
        const size_t scales[] = {BENCH_SCALE_1K, BENCH_SCALE_10K,
                                 BENCH_SCALE_50K, BENCH_SCALE_100K};
        const size_t numScales = sizeof(scales) / sizeof(scales[0]);

        printf("    %-12s  %12s  %12s  %8s\n", "Count", "timerWheel",
               "multiTimer", "Speedup");
        printf("    %-12s  %12s  %12s  %8s\n", "-----", "----------",
               "----------", "-------");

        for (size_t s = 0; s < numScales; s++) {
            const size_t count = scales[s];
            testCallbackState *states =
                zcalloc(count, sizeof(testCallbackState));

            /* timerWheel */
            timerWheel *tw = timerWheelNew();
            for (size_t i = 0; i < count; i++) {
                states[i].shouldReschedule = false;
                timerWheelRegister(tw, 0, 0, testCountingCallback, &states[i]);
            }

            PERF_TIMERS_SETUP;
            timerWheelProcessTimerEvents(tw);
            PERF_TIMERS_FINISH;
            double twCycles =
                (double)(lps.global.tsc.stop - lps.global.tsc.start) / count;
            timerWheelFree(tw);

            /* Reset states */
            memset(states, 0, count * sizeof(testCallbackState));

            /* multiTimer */
            multiTimer *mt = multiTimerNew();
            for (size_t i = 0; i < count; i++) {
                states[i].shouldReschedule = false;
                multiTimerRegister(mt, 0, 0, mtCountingCallback, &states[i]);
            }

            PERF_TIMERS_SETUP;
            multiTimerProcessTimerEvents(mt);
            PERF_TIMERS_FINISH;
            double mtCycles =
                (double)(lps.global.tsc.stop - lps.global.tsc.start) / count;
            multiTimerFree(mt);

            zfree(states);

            printf("    %-12zu  %9.1f cy  %9.1f cy  %7.2fx\n", count, twCycles,
                   mtCycles, mtCycles / twCycles);
        }
        printf("\n");
    }

    TEST("BENCHMARK: nextTimerEvent query latency") {
        printf("    Measuring next-timer-event query latency...\n");
        testCallbackState state = {0};
        const size_t timerCounts[] = {100, BENCH_SCALE_1K, BENCH_SCALE_10K,
                                      BENCH_SCALE_100K};
        const size_t numCounts = sizeof(timerCounts) / sizeof(timerCounts[0]);
        const size_t queries = BENCH_QUERY_COUNT;

        printf("    %-12s  %12s  %12s  %8s\n", "Timers", "timerWheel",
               "multiTimer", "Speedup");
        printf("    %-12s  %12s  %12s  %8s\n", "------", "----------",
               "----------", "-------");

        for (size_t c = 0; c < numCounts; c++) {
            const size_t count = timerCounts[c];

            /* timerWheel */
            timerWheel *tw = timerWheelNew();
            for (size_t i = 0; i < count; i++) {
                timerWheelRegister(tw, 1000000 + (i * 100), 0,
                                   testCountingCallback, &state);
            }

            volatile int64_t sink = 0;
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < queries; i++) {
                sink += timerWheelNextTimerEventStartUs(tw);
            }
            PERF_TIMERS_FINISH;
            double twCycles =
                (double)(lps.global.tsc.stop - lps.global.tsc.start) / queries;
            timerWheelFree(tw);
            (void)sink;

            /* multiTimer */
            multiTimer *mt = multiTimerNew();
            for (size_t i = 0; i < count; i++) {
                multiTimerRegister(mt, 1000000 + (i * 100), 0,
                                   mtCountingCallback, &state);
            }

            sink = 0;
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < queries; i++) {
                sink += multiTimerNextTimerEventStartUs(mt);
            }
            PERF_TIMERS_FINISH;
            double mtCycles =
                (double)(lps.global.tsc.stop - lps.global.tsc.start) / queries;
            multiTimerFree(mt);
            (void)sink;

            printf("    %-12zu  %9.1f cy  %9.1f cy  %7.2fx\n", count, twCycles,
                   mtCycles, mtCycles / twCycles);
        }
        printf("\n");
    }

    TEST("BENCHMARK: timer delay distribution impact") {
        printf("    Measuring performance across delay distributions...\n");
        testCallbackState state = {0};
        const size_t count = BENCH_SCALE_50K;

        struct {
            const char *name;
            uint64_t minUs;
            uint64_t maxUs;
        } distributions[] = {
            {"Uniform short", 1000, 10000},       /* 1-10ms */
            {"Uniform medium", 100000, 1000000},  /* 100ms-1s */
            {"Uniform long", 1000000, 60000000},  /* 1s-60s */
            {"Clustered", 5000, 5100},            /* 5ms ± 50μs */
            {"Wide spread", 1000, 3600000000ULL}, /* 1ms-1hr */
        };
        const size_t numDists =
            sizeof(distributions) / sizeof(distributions[0]);

        printf("    %-16s  %12s  %12s  %8s\n", "Distribution", "timerWheel",
               "multiTimer", "Speedup");
        printf("    %-16s  %12s  %12s  %8s\n", "------------", "----------",
               "----------", "-------");

        for (size_t d = 0; d < numDists; d++) {
            uint64_t minUs = distributions[d].minUs;
            uint64_t range = distributions[d].maxUs - minUs;

            /* timerWheel */
            timerWheel *tw = timerWheelNew();
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < count; i++) {
                uint64_t delay = minUs + (i * 7919) % (range + 1);
                timerWheelRegister(tw, delay, 0, testCountingCallback, &state);
            }
            PERF_TIMERS_FINISH;
            double twCycles =
                (double)(lps.global.tsc.stop - lps.global.tsc.start) / count;
            timerWheelFree(tw);

            /* multiTimer */
            multiTimer *mt = multiTimerNew();
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < count; i++) {
                uint64_t delay = minUs + (i * 7919) % (range + 1);
                multiTimerRegister(mt, delay, 0, mtCountingCallback, &state);
            }
            PERF_TIMERS_FINISH;
            double mtCycles =
                (double)(lps.global.tsc.stop - lps.global.tsc.start) / count;
            multiTimerFree(mt);

            printf("    %-16s  %9.1f cy  %9.1f cy  %7.2fx\n",
                   distributions[d].name, twCycles, mtCycles,
                   mtCycles / twCycles);
        }
        printf("\n");
    }

    TEST("BENCHMARK: register-then-cancel pattern") {
        printf("    Measuring register-then-immediate-cancel pattern...\n");
        testCallbackState state = {0};
        const size_t scales[] = {BENCH_SCALE_10K, BENCH_SCALE_50K,
                                 BENCH_SCALE_100K};
        const size_t numScales = sizeof(scales) / sizeof(scales[0]);

        printf("    %-12s  %12s  %12s  %8s\n", "Count", "timerWheel",
               "multiTimer", "Speedup");
        printf("    %-12s  %12s  %12s  %8s\n", "-----", "----------",
               "----------", "-------");

        for (size_t s = 0; s < numScales; s++) {
            const size_t count = scales[s];

            /* timerWheel - register then immediately cancel */
            timerWheel *tw = timerWheelNew();
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < count; i++) {
                timerWheelId id = timerWheelRegister(
                    tw, 1000000, 0, testCountingCallback, &state);
                timerWheelUnregister(tw, id);
            }
            PERF_TIMERS_FINISH;
            double twCycles =
                (double)(lps.global.tsc.stop - lps.global.tsc.start) / count;
            timerWheelFree(tw);

            /* multiTimer - register then immediately cancel */
            multiTimer *mt = multiTimerNew();
            PERF_TIMERS_SETUP;
            for (size_t i = 0; i < count; i++) {
                multiTimerId id = multiTimerRegister(
                    mt, 1000000, 0, mtCountingCallback, &state);
                multiTimerUnregister(mt, id);
            }
            PERF_TIMERS_FINISH;
            double mtCycles =
                (double)(lps.global.tsc.stop - lps.global.tsc.start) / count;
            multiTimerFree(mt);

            printf("    %-12zu  %9.1f cy  %9.1f cy  %7.2fx\n", count, twCycles,
                   mtCycles, mtCycles / twCycles);
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
        timerWheel *tw = timerWheelNew();
        for (size_t i = 0; i < baseTimers; i++) {
            timerWheelRegister(tw, 1000000 + (i * 100), 0, testCountingCallback,
                               &state);
        }

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            /* Register new timer */
            timerWheelId id = timerWheelRegister(tw, 500000 + (i % 500000), 0,
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
        double twCycles =
            (double)(lps.global.tsc.stop - lps.global.tsc.start) / iterations;
        double twUsPerIter =
            (double)(lps.global.us.stop - lps.global.us.start) / iterations;
        timerWheelFree(tw);

        /* multiTimer steady-state */
        multiTimer *mt = multiTimerNew();
        for (size_t i = 0; i < baseTimers; i++) {
            multiTimerRegister(mt, 1000000 + (i * 100), 0, mtCountingCallback,
                               &state);
        }

        PERF_TIMERS_SETUP;
        for (size_t i = 0; i < iterations; i++) {
            /* Register new timer */
            multiTimerId id = multiTimerRegister(mt, 500000 + (i % 500000), 0,
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
        double mtCycles =
            (double)(lps.global.tsc.stop - lps.global.tsc.start) / iterations;
        double mtUsPerIter =
            (double)(lps.global.us.stop - lps.global.us.start) / iterations;
        multiTimerFree(mt);

        printf("    timerWheel:  %.1f cycles/iter (%.3f us/iter)\n", twCycles,
               twUsPerIter);
        printf("    multiTimer:  %.1f cycles/iter (%.3f us/iter)\n", mtCycles,
               mtUsPerIter);
        printf("    Speedup:     %.2fx\n\n", mtCycles / twCycles);
    }

    TEST("BENCHMARK: timerWheel memory at scale") {
        printf("    timerWheel memory usage at scale...\n");
        testCallbackState state = {0};
        const size_t scales[] = {BENCH_SCALE_10K, BENCH_SCALE_100K,
                                 BENCH_SCALE_500K, BENCH_SCALE_1M};
        const size_t numScales = sizeof(scales) / sizeof(scales[0]);

        printf("    %-12s  %14s  %12s\n", "Timers", "Memory", "Bytes/Timer");
        printf("    %-12s  %14s  %12s\n", "------", "------", "-----------");

        for (size_t s = 0; s < numScales; s++) {
            const size_t count = scales[s];

            timerWheel *tw = timerWheelNew();
            for (size_t i = 0; i < count; i++) {
                timerWheelRegister(tw, i * 1000, 0, testCountingCallback,
                                   &state);
            }
            timerWheelStats twStats;
            timerWheelGetStats(tw, &twStats);
            timerWheelFree(tw);

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
     * Wheel 1 slot 0 cascades at 256ms, slot 1 at 512ms, slot 2 at 768ms.
     * Cascade counter only increments when there ARE timers in the slot.
     *
     * Timer wheel trades timing precision for O(1) operations. Timers in
     * higher wheels fire at cascade boundaries, not exact scheduled times.
     * ================================================================ */

    printf("\n=== Multi-Level Cascade Tests ===\n\n");

    TEST("CASCADE: wheel 0 timers fire accurately") {
        /* Wheel 0 timers should fire at their scheduled time */
        timerWheel *tw = timerWheelNew();
        testCallbackState state = {0};

        /* Register timer for 100ms - stays in wheel 0 */
        timerWheelRegister(tw, 100000, 0, testCountingCallback, &state);

        timerWheelAdvanceTime(tw, 95000);
        if (state.callCount != 0) {
            ERR("Timer fired too early at 95ms, count=%d", state.callCount);
        }

        timerWheelAdvanceTime(tw, 10000); /* Now at 105ms */
        if (state.callCount != 1) {
            ERR("Timer should fire at 100ms, count=%d", state.callCount);
        }

        timerWheelFree(tw);
    }

    TEST("CASCADE: wheel 1 timer fires at cascade boundary") {
        /* A 300ms timer goes to wheel 1 slot 1, cascades at 512ms */
        timerWheel *tw = timerWheelNew();
        testCallbackState state = {0};

        /* Register 300ms timer - goes to wheel 1 slot 1 (300/256 = 1) */
        timerWheelRegister(tw, 300000, 0, testCountingCallback, &state);

        /* Advance to 300ms - timer should NOT fire yet (still in wheel 1) */
        timerWheelAdvanceTime(tw, 300000);
        if (state.callCount != 0) {
            ERR("Timer shouldn't fire at 300ms (still in wheel 1), count=%d",
                state.callCount);
        }

        /* Advance to 512ms - wheel 1 slot 1 cascades, timer fires (overdue) */
        timerWheelAdvanceTime(tw, 212000); /* Now at 512ms */
        if (state.callCount != 1) {
            ERR("Timer should fire at 512ms cascade, count=%d",
                state.callCount);
        }

        timerWheelFree(tw);
    }

    TEST("CASCADE: cascade counter only increments for non-empty slots") {
        timerWheel *tw = timerWheelNew();
        testCallbackState state = {0};

        /* Register 520ms timer - goes to wheel 1 slot 2 (520/256 = 2) */
        timerWheelRegister(tw, 520000, 0, testCountingCallback, &state);

        timerWheelStats stats;

        /* Advance to 256ms - slot 0 cascades (empty), counter should be 0 */
        timerWheelAdvanceTime(tw, 256000);
        timerWheelGetStats(tw, &stats);
        if (stats.totalCascades != 0) {
            ERR("Expected 0 cascades at 256ms (slot 0 empty), got %zu",
                stats.totalCascades);
        }

        /* Advance to 512ms - slot 1 cascades (empty), counter should be 0 */
        timerWheelAdvanceTime(tw, 256000);
        timerWheelGetStats(tw, &stats);
        if (stats.totalCascades != 0) {
            ERR("Expected 0 cascades at 512ms (slot 1 empty), got %zu",
                stats.totalCascades);
        }

        /* Timer should not have fired yet */
        if (state.callCount != 0) {
            ERR("Timer shouldn't fire before cascade, count=%d",
                state.callCount);
        }

        /* Advance to 768ms - slot 2 cascades (has timer!), counter = 1 */
        timerWheelAdvanceTime(tw, 256000);
        timerWheelGetStats(tw, &stats);
        if (stats.totalCascades != 1) {
            ERR("Expected 1 cascade at 768ms (slot 2 has timer), got %zu",
                stats.totalCascades);
        }

        /* Timer should have fired (overdue by 248ms) */
        if (state.callCount != 1) {
            ERR("Timer should fire when cascaded, count=%d", state.callCount);
        }

        timerWheelFree(tw);
    }

    TEST("CASCADE: multiple timers in same slot cascade together") {
        timerWheel *tw = timerWheelNew();
        testCallbackState states[3] = {0};

        /* All these go to wheel 1 slot 1 (since 260-400/256 = 1) */
        timerWheelRegister(tw, 260000, 0, testCountingCallback, &states[0]);
        timerWheelRegister(tw, 350000, 0, testCountingCallback, &states[1]);
        timerWheelRegister(tw, 400000, 0, testCountingCallback, &states[2]);

        /* Advance to 500ms - no cascade yet (slot 1 cascades at 512ms) */
        timerWheelAdvanceTime(tw, 500000);
        int firedBefore =
            states[0].callCount + states[1].callCount + states[2].callCount;
        if (firedBefore != 0) {
            ERR("Timers shouldn't fire before cascade, count=%d", firedBefore);
        }

        /* Advance to 520ms - slot 1 cascades, all 3 timers fire together */
        timerWheelAdvanceTime(tw, 20000);
        int firedAfter =
            states[0].callCount + states[1].callCount + states[2].callCount;
        if (firedAfter != 3) {
            ERR("All 3 timers should fire at cascade, count=%d", firedAfter);
        }

        timerWheelStats stats;
        timerWheelGetStats(tw, &stats);
        if (stats.totalCascades != 1) {
            ERR("Expected exactly 1 cascade event, got %zu",
                stats.totalCascades);
        }

        timerWheelFree(tw);
    }

    TEST(
        "CASCADE: timers inserted into wheel 0 after cascade fire accurately") {
        /* When a timer cascades to wheel 0, it should fire at its slot */
        timerWheel *tw = timerWheelNew();
        testCallbackState state = {0};

        /* 700ms timer goes to wheel 1 slot 2 (700/256 = 2), cascades at 768ms
         */
        timerWheelRegister(tw, 700000, 0, testCountingCallback, &state);

        /* Advance to 768ms - timer cascades to wheel 0 but not yet due */
        /* Wait, at 768ms the timer is overdue (700 < 768), so fires immediately
         */
        timerWheelAdvanceTime(tw, 768000);
        if (state.callCount != 1) {
            ERR("Timer should fire when cascaded (overdue), count=%d",
                state.callCount);
        }

        timerWheelFree(tw);
    }

    TEST("CASCADE: timer cascades to wheel 0 and fires at correct slot") {
        /* Test a timer that cascades to wheel 0 but isn't overdue yet */
        timerWheel *tw = timerWheelNew();
        testCallbackState state = {0};

        /* 520ms timer goes to wheel 1 slot 2 (520/256 = 2), cascades at 768ms
           But at 768ms, timer is overdue. Let's try a longer timer. */

        /* 780ms timer: slot = 780/256 = 3, cascades at 1024ms */
        timerWheelRegister(tw, 780000, 0, testCountingCallback, &state);

        /* Advance to 780ms - timer still in wheel 1 */
        timerWheelAdvanceTime(tw, 780000);
        if (state.callCount != 0) {
            ERR("Timer shouldn't fire before cascade at 1024ms, count=%d",
                state.callCount);
        }

        /* Advance to 1024ms - slot 3 cascades, timer is overdue, fires */
        timerWheelAdvanceTime(tw, 244000);
        if (state.callCount != 1) {
            ERR("Timer should fire at cascade, count=%d", state.callCount);
        }

        timerWheelFree(tw);
    }

    TEST("CASCADE: nested wheel 2 to wheel 1 to wheel 0") {
        /* Timer far enough to start in wheel 2, then cascade down */
        timerWheel *tw = timerWheelNew();
        testCallbackState state = {0};

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

        timerWheelFree(tw);
    }

    TEST("CASCADE: cancellation removes timer before cascade") {
        timerWheel *tw = timerWheelNew();
        testCallbackState states[3] = {0};

        /* All go to wheel 1 slot 1 */
        timerWheelId id1 =
            timerWheelRegister(tw, 300000, 0, testCountingCallback, &states[0]);
        timerWheelId id2 =
            timerWheelRegister(tw, 350000, 0, testCountingCallback, &states[1]);
        timerWheelId id3 =
            timerWheelRegister(tw, 400000, 0, testCountingCallback, &states[2]);

        /* Cancel middle timer before cascade */
        timerWheelUnregister(tw, id2);

        (void)id1;
        (void)id3;

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

        timerWheelFree(tw);
    }

    TEST("CASCADE: repeated timer reschedules after cascade fire") {
        timerWheel *tw = timerWheelNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = true};

        /* 300ms initial delay, 100ms repeat - starts in wheel 1
         * First fire at cascade (512ms), then reschedules every 100ms */
        timerWheelRegister(tw, 300000, 100000, testCountingCallback, &state);

        /* Advance to 520ms - fires at cascade (~512ms) */
        timerWheelAdvanceTime(tw, 520000);
        if (state.callCount < 1) {
            ERR("Timer should fire after cascade, count=%d", state.callCount);
        }

        int countAfterCascade = state.callCount;

        /* Advance another 500ms (to 1020ms) - should fire ~5 more times */
        timerWheelAdvanceTime(tw, 500000);

        /* With 100ms repeat, expect 4-6 more fires in 500ms */
        int additionalFires = state.callCount - countAfterCascade;
        if (additionalFires < 4 || additionalFires > 6) {
            ERR("Expected 4-6 additional fires with 100ms repeat, got %d",
                additionalFires);
        }

        printf("    Timer fired %d times total (%d after cascade)\n",
               state.callCount, additionalFires);

        timerWheelFree(tw);
    }

    printf("=== Multi-Level Cascade Tests Complete ===\n\n");

    TEST_FINAL_RESULT;
}
#endif
