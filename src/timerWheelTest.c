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

#ifdef DATAKIT_TEST
#include "ctest.h"

#define DOUBLE_NEWLINE 1
#include "perf.h"

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
        const size_t numOps = 100000;

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
        const size_t numOps = 100000;
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
        const size_t numTimers = 1000000;
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
        const size_t warmupTimers = 100000;
        const size_t ops = 50000;
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
        const size_t numTimers = 100000;
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

    TEST_FINAL_RESULT;
}
#endif
