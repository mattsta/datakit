/* multiTimer Tests */
#include "multiTimer.h"
#include "multiTimerInternal.h"
#include "timeUtil.h"

#include <inttypes.h>
#include <string.h>
#include <unistd.h>

#ifdef DATAKIT_TEST
#include "cleaner.h"
#include "ctest.h"
#include "dkDiag.h"

#define DOUBLE_NEWLINE 1
#include "perf.h"

/* ================================================================
 * Test State and Helpers
 * ================================================================ */

/* Callback counter state */
typedef struct testCallbackState {
    int32_t callCount;
    multiTimerId lastId;
    bool shouldReschedule;
} testCallbackState;

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleanerName(testCallbackState *, zfree, States);
defcleaner(multiTimer *, multiTimerFree);

/* Simple callback that counts invocations */
static bool testCountingCallback(multiTimer *, multiTimerId id,
                                 void *clientData) {
    testCallbackState *state = clientData;
    state->callCount++;
    state->lastId = id;
    return state->shouldReschedule;
}

/* Callback that creates a new timer during execution */
static bool testNestedTimerCallback(multiTimer *t, multiTimerId,
                                    void *clientData) {
    testCallbackState *state = clientData;
    state->callCount++;

    /* Create a new timer from within a timer callback */
    if (state->callCount == 1) {
        multiTimerRegister(t, 1000, 0, testCountingCallback, clientData);
    }

    return false;
}

/* Callback that unregisters itself */
static bool testSelfUnregisterCallback(multiTimer *t, multiTimerId id,
                                       void *clientData) {
    testCallbackState *state = clientData;
    state->callCount++;
    multiTimerUnregister(t, id);
    return true; /* Request reschedule but we unregistered */
}

/* Callback that unregisters itself and does not request repeat */
static bool testSelfUnregisterNoRepeatCallback(multiTimer *t, multiTimerId id,
                                               void *clientData) {
    testCallbackState *state = clientData;
    state->callCount++;
    multiTimerUnregister(t, id);
    return false;
}

/* Callback that unregisters a different (possibly already-fired) id */
typedef struct testUnregisterOtherState {
    multiTimerId target;
    int callCount;
} testUnregisterOtherState;

static bool testUnregisterOtherCallback(multiTimer *t, multiTimerId,
                                        void *clientData) {
    testUnregisterOtherState *state = clientData;
    state->callCount++;
    multiTimerUnregister(t, state->target);
    return false;
}

/* Callback that stops all timers */
static bool testStopAllCallback(multiTimer *t, multiTimerId, void *clientData) {
    testCallbackState *state = clientData;
    state->callCount++;
    multiTimerStopAll(t);
    return true; /* reschedule request must be ignored after StopAll */
}

/* Callback that tries to re-enter timer processing */
static bool testReentrantProcessCallback(multiTimer *t, multiTimerId,
                                         void *clientData) {
    testCallbackState *state = clientData;
    state->callCount++;
    multiTimerProcessTimerEvents(t); /* must be a safe no-op */
    return false;
}

/* Helper to sleep for microseconds (approximate) */
static void sleepUs(uint64_t us) {
    usleep((useconds_t)us);
}

int multiTimerTest(int, char *[]) {
    int32_t err = 0;

    /* ================================================================
     * Basic Initialization Tests
     * ================================================================ */

    TEST("multiTimer: create and free") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        if (!t) {
            ERR("Failed to create multiTimer");
        }
    }

    TEST("multiTimer: init and deinit on stack") {
        multiTimer t;
        if (!multiTimerInit(&t)) {
            ERR("Failed to init multiTimer");
        } else {
            if (multiTimerCount(&t) != 0) {
                ERR("Expected 0 timers, got %zu", multiTimerCount(&t));
            }
            multiTimerDeinit(&t);
        }
    }

    TEST("multiTimer: free NULL safety") {
        multiTimerFree(NULL); /* Should not crash */
    }

    /* ================================================================
     * Timer Registration Tests
     * ================================================================ */

    TEST("multiTimer: register single timer") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState state = {};

        multiTimerId id =
            multiTimerRegister(t, 1000, 0, testCountingCallback, &state);
        if (id == 0) {
            ERR("Timer ID should not be 0");
        }

        if (multiTimerCount(t) != 1) {
            ERR("Expected 1 timer, got %zu", multiTimerCount(t));
        }
    }

    TEST("multiTimer: register multiple timers") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState state = {};

        multiTimerId ids[5];
        for (int32_t i = 0; i < 5; i++) {
            ids[i] = multiTimerRegister(t, (uint64_t)(i + 1) * 1000, 0,
                                        testCountingCallback, &state);
        }

        /* Verify all IDs are unique and sequential */
        for (int32_t i = 0; i < 5; i++) {
            if (ids[i] != (multiTimerId)(i + 1)) {
                ERR("Timer %d has unexpected ID %" PRIu64, i, ids[i]);
            }
        }

        if (multiTimerCount(t) != 5) {
            ERR("Expected 5 timers, got %zu", multiTimerCount(t));
        }
    }

    /* ================================================================
     * Timer Execution Tests
     * ================================================================ */

    TEST("multiTimer: timer fires after delay") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        /* Register timer to fire in 5ms */
        multiTimerRegister(t, 5000, 0, testCountingCallback, &state);

        /* Process immediately - should not fire */
        multiTimerProcessTimerEvents(t);
        if (state.callCount != 0) {
            ERR("Timer fired too early, callCount=%d", state.callCount);
        }

        /* Wait for timer to expire */
        sleepUs(10000);

        /* Process - timer should fire */
        multiTimerProcessTimerEvents(t);
        if (state.callCount != 1) {
            ERR("Timer did not fire, callCount=%d", state.callCount);
        }

        /* Timer was one-shot, should be removed */
        if (multiTimerCount(t) != 0) {
            ERR("Timer should be removed, count=%zu", multiTimerCount(t));
        }
    }

    TEST("multiTimer: repeating timer") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = true};

        /* Register timer to fire every 5ms, starting in 5ms */
        multiTimerRegister(t, 5000, 5000, testCountingCallback, &state);

        /* Wait and process multiple times */
        for (int32_t i = 0; i < 3; i++) {
            sleepUs(7000);
            multiTimerProcessTimerEvents(t);
        }

        if (state.callCount < 3) {
            ERR("Repeating timer fired only %d times, expected >= 3",
                state.callCount);
        }

        /* Timer should still be scheduled */
        if (multiTimerCount(t) == 0) {
            ERR("Repeating timer was incorrectly removed");
        }
    }

    TEST("multiTimer: timer ordering (multiple timers fire in order)") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState states[3] = {{0}, {0}, {0}};

        /* Register timers in reverse order of when they should fire */
        multiTimerRegister(t, 15000, 0, testCountingCallback,
                           &states[2]); /* ID 1, fires 3rd */
        multiTimerRegister(t, 5000, 0, testCountingCallback,
                           &states[0]); /* ID 2, fires 1st */
        multiTimerRegister(t, 10000, 0, testCountingCallback,
                           &states[1]); /* ID 3, fires 2nd */

        /* Wait for all to expire */
        sleepUs(20000);
        multiTimerProcessTimerEvents(t);

        /* All timers should have fired once */
        if (states[0].callCount != 1 || states[1].callCount != 1 ||
            states[2].callCount != 1) {
            ERR("Not all timers fired: %d, %d, %d (expected 1, 1, 1)",
                states[0].callCount, states[1].callCount, states[2].callCount);
        }

        /* Verify IDs were captured correctly */
        if (states[0].lastId != 2 || states[1].lastId != 3 ||
            states[2].lastId != 1) {
            ERR("Wrong IDs: %" PRIu64 ", %" PRIu64 ", %" PRIu64
                " (expected 2, 3, 1)",
                states[0].lastId, states[1].lastId, states[2].lastId);
        }
    }

    /* ================================================================
     * Timer Unregistration Tests
     * ================================================================ */

    TEST("multiTimer: unregister timer before it fires") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        multiTimerId id =
            multiTimerRegister(t, 100000, 0, testCountingCallback, &state);

        /* Unregister before it fires */
        multiTimerUnregister(t, id);

        /* Wait and process */
        sleepUs(150000);
        multiTimerProcessTimerEvents(t);

        /* Timer should NOT have fired */
        if (state.callCount != 0) {
            ERR("Unregistered timer fired, callCount=%d", state.callCount);
        }
    }

    TEST("multiTimer: unregister multiple timers") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState states[5] = {{0}, {0}, {0}, {0}, {0}};

        multiTimerId ids[5];
        for (int32_t i = 0; i < 5; i++) {
            states[i].shouldReschedule = false;
            ids[i] = multiTimerRegister(t, 50000, 0, testCountingCallback,
                                        &states[i]);
        }

        /* Unregister even-indexed timers (IDs 1, 3, 5) */
        multiTimerUnregister(t, ids[0]);
        multiTimerUnregister(t, ids[2]);
        multiTimerUnregister(t, ids[4]);

        /* Wait and process */
        sleepUs(60000);
        multiTimerProcessTimerEvents(t);

        /* Only odd-indexed timers should have fired */
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

    TEST("multiTimer: stopAll") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState states[10];
        memset(states, 0, sizeof(states));

        for (int32_t i = 0; i < 10; i++) {
            states[i].shouldReschedule = false;
            multiTimerRegister(t, 50000, 0, testCountingCallback, &states[i]);
        }

        /* Stop all timers */
        multiTimerStopAll(t);

        /* Wait and process */
        sleepUs(60000);
        multiTimerProcessTimerEvents(t);

        /* No timers should have fired */
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

    TEST("multiTimer: register timer from within callback") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        /* Register timer that creates another timer */
        multiTimerRegister(t, 5000, 0, testNestedTimerCallback, &state);

        /* First timer fires and creates second timer */
        sleepUs(10000);
        multiTimerProcessTimerEvents(t);

        if (state.callCount != 1) {
            ERR("First timer did not fire, callCount=%d", state.callCount);
        }

        /* Second timer (created by first) should be scheduled */
        if (multiTimerCount(t) == 0) {
            ERR("Nested timer was not scheduled");
        }

        /* Wait for nested timer */
        sleepUs(5000);
        multiTimerProcessTimerEvents(t);

        if (state.callCount != 2) {
            ERR("Nested timer did not fire, callCount=%d", state.callCount);
        }
    }

    TEST("multiTimer: self-unregister from callback") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = true};

        /* Register repeating timer that unregisters itself */
        multiTimerRegister(t, 5000, 5000, testSelfUnregisterCallback, &state);

        sleepUs(10000);
        multiTimerProcessTimerEvents(t);

        if (state.callCount != 1) {
            ERR("Timer did not fire once, callCount=%d", state.callCount);
        }

        /* Wait more - timer should not fire again */
        sleepUs(20000);
        multiTimerProcessTimerEvents(t);

        if (state.callCount != 1) {
            ERR("Self-unregistered timer fired again, callCount=%d",
                state.callCount);
        }
    }

    /* ================================================================
     * Next Timer Event Tests
     * ================================================================ */

    TEST("multiTimer: nextTimerEventStartUs with no timers") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();

        multiTimerSystemMonotonicUs next = multiTimerNextTimerEventStartUs(t);
        if (next != 0) {
            ERR("Expected 0 for empty timer, got %" PRId64, next);
        }
    }

    TEST("multiTimer: nextTimerEventStartUs returns correct time") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState state = {};

        const uint64_t delayUs = 100000; /* 100ms */
        const int64_t before = (int64_t)timeUtilMonotonicUs();

        multiTimerRegister(t, delayUs, 0, testCountingCallback, &state);

        const multiTimerSystemMonotonicUs next =
            multiTimerNextTimerEventStartUs(t);
        const int64_t after = (int64_t)timeUtilMonotonicUs();

        /* The event is delayUs after a clock read between the two reads
         * (with 1ms of rounding): bracketing checks units and offset
         * without depending on how long a loaded machine takes */
        const int64_t earliest = before + (int64_t)delayUs - 1000;
        const int64_t latest = after + (int64_t)delayUs + 1000;
        if (next < earliest || next > latest) {
            ERR("nextTimerEventStartUs %" PRId64 " outside [%" PRId64
                ", %" PRId64 "]",
                (int64_t)next, earliest, latest);
        }
    }

    TEST("multiTimer: nextTimerEventOffsetFromNowUs") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState state = {};

        uint64_t delayUs = 50000; /* 50ms */
        multiTimerRegister(t, delayUs, 0, testCountingCallback, &state);

        multiTimerUs offset = multiTimerNextTimerEventOffsetFromNowUs(t);

        /* Offset should be close to delayUs (slightly less due to elapsed time)
         */
        if (offset < 0 || offset > (multiTimerUs)delayUs + 5000) {
            ERR("nextTimerEventOffsetFromNowUs returned %" PRId64
                ", expected ~%" PRIu64,
                offset, delayUs);
        }
    }

    /* ================================================================
     * Timer Utility Functions Tests
     * ================================================================ */

    TEST("multiTimer: getUs returns reasonable value") {
        multiTimerSystemMonotonicUs t1 = multiTimerGetUs();
        sleepUs(1000);
        multiTimerSystemMonotonicUs t2 = multiTimerGetUs();

        if (t2 <= t1) {
            ERR("Time did not advance: %" PRId64 " -> %" PRId64, t1, t2);
        }

        int64_t elapsed = t2 - t1;
        /* The sleep lasts at least 1ms, so a microsecond clock advances at
         * least ~1000.  The ceiling only catches a wrong unit; how much
         * longer the sleep runs depends on machine load, not on the clock. */
        if (elapsed < 500 || elapsed > 1'000'000) {
            ERR("Elapsed time %" PRId64 "us unexpected for 1ms sleep", elapsed);
        }
    }

    TEST("multiTimer: getNs returns reasonable value") {
        multiTimerSystemMonotonicUs t1 = multiTimerGetNs();
        sleepUs(1000);
        multiTimerSystemMonotonicUs t2 = multiTimerGetNs();

        if (t2 <= t1) {
            ERR("Time did not advance: %" PRId64 " -> %" PRId64, t1, t2);
        }

        int64_t elapsed = t2 - t1;
        /* The sleep lasts at least 1ms, so a nanosecond clock advances at
         * least ~1000000.  The ceiling only catches a wrong unit; how much
         * longer the sleep runs depends on machine load, not on the clock. */
        if (elapsed < 500000 || elapsed > 1'000'000'000) {
            ERR("Elapsed time %" PRId64 "ns unexpected for 1ms sleep", elapsed);
        }
    }

    /* ================================================================
     * Edge Cases and Stress Tests
     * ================================================================ */

    TEST("multiTimer: many timers") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        const int32_t numTimers = 1000;
        int32_t totalCallCount = 0;

        /* Use array of state to count total firings */
        cleanerName(zfree, States) testCallbackState *states =
            zcalloc(numTimers, sizeof(testCallbackState));

        for (int32_t i = 0; i < numTimers; i++) {
            states[i].shouldReschedule = false;
            /* All fire at approximately the same time */
            multiTimerRegister(t, 10000 + (uint64_t)i, 0, testCountingCallback,
                               &states[i]);
        }

        if (multiTimerCount(t) != (size_t)numTimers) {
            ERR("Expected %d timers, got %zu", numTimers, multiTimerCount(t));
        }

        /* Wait and process */
        sleepUs(20000);
        multiTimerProcessTimerEvents(t);

        for (int32_t i = 0; i < numTimers; i++) {
            totalCallCount += states[i].callCount;
        }

        if (totalCallCount != numTimers) {
            ERR("Expected %d firings, got %d", numTimers, totalCallCount);
        }
    }

    TEST("multiTimer: timer with zero delay fires immediately") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        /* Register with 0 delay */
        multiTimerRegister(t, 0, 0, testCountingCallback, &state);

        /* Process immediately */
        multiTimerProcessTimerEvents(t);

        if (state.callCount != 1) {
            ERR("Zero-delay timer did not fire, callCount=%d", state.callCount);
        }
    }

    TEST("multiTimer: unregister non-existent timer (no crash)") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();

        /* Unregister timer ID that doesn't exist */
        multiTimerUnregister(t, 9999);

        /* Should not crash, process should work */
        multiTimerProcessTimerEvents(t);
    }

    TEST("multiTimer: callback returns false stops repeating") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        /* Register repeating timer, but callback returns false */
        multiTimerRegister(t, 5000, 5000, testCountingCallback, &state);

        sleepUs(10000);
        multiTimerProcessTimerEvents(t);

        if (state.callCount != 1) {
            ERR("Timer did not fire once, callCount=%d", state.callCount);
        }

        /* Timer should not repeat since callback returned false */
        sleepUs(15000);
        multiTimerProcessTimerEvents(t);

        if (state.callCount != 1) {
            ERR("Timer repeated despite returning false, callCount=%d",
                state.callCount);
        }
    }

    /* ================================================================
     * Regression Tests
     * ================================================================ */

    TEST("multiTimer: stack init is fully deterministic") {
        /* multiTimerInit must not inherit stack garbage: poison the
         * struct first and verify ids start at 1 */
        multiTimer t;
        memset(&t, 0xA5, sizeof(t));

        if (!multiTimerInit(&t)) {
            ERR("Failed to init multiTimer");
        } else {
            testCallbackState state = {};
            multiTimerId id =
                multiTimerRegister(&t, 1000, 0, testCountingCallback, &state);
            if (id != 1) {
                ERR("First id should be 1, got %" PRIu64
                    " (stack garbage leaked into nextTimerId)",
                    id);
            }
            multiTimerDeinit(&t);
        }
    }

    TEST("multiTimer: offset with no timers returns 0") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();

        multiTimerUs offset = multiTimerNextTimerEventOffsetFromNowUs(t);
        if (offset != 0) {
            ERR("Expected 0 offset for empty timer, got %" PRId64
                " (event loops would spin on a huge negative)",
                offset);
        }
    }

    TEST("multiTimer: unregister validates ids") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState state = {};

        if (multiTimerUnregister(t, 0)) {
            ERR("Unregister of id 0 should return false");
        }
        if (multiTimerUnregister(t, 9999)) {
            ERR("Unregister of never-issued id should return false");
        }

        /* A rejected id must not plant a stop entry that could block a
         * future timer */
        multiTimerStats stats;
        multiTimerGetStats(t, &stats);
        if (stats.stopPending != 0) {
            ERR("Invalid unregister planted a stop entry, stopPending=%zu",
                stats.stopPending);
        }

        multiTimerId id =
            multiTimerRegister(t, 1000, 0, testCountingCallback, &state);
        if (!multiTimerUnregister(t, id)) {
            ERR("Unregister of live id should return true");
        }
        if (!multiTimerUnregister(t, id)) {
            ERR("Double unregister should still return true");
        }

        multiTimerGetStats(t, &stats);
        if (stats.stopPending != 1) {
            ERR("Double unregister duplicated stop entry, stopPending=%zu",
                stats.stopPending);
        }
    }

    TEST("multiTimer: unregister after fire keeps count sane") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        multiTimerId id =
            multiTimerRegister(t, 1000, 0, testCountingCallback, &state);
        sleepUs(3000);
        multiTimerProcessTimerEvents(t);
        if (state.callCount != 1) {
            ERR("Timer did not fire, callCount=%d", state.callCount);
        }

        /* Stopping an already-fired id is a harmless no-op; the stale
         * entry purges on the next drained process pass */
        multiTimerUnregister(t, id);
        multiTimerProcessTimerEvents(t);

        multiTimerStats stats;
        multiTimerGetStats(t, &stats);
        if (stats.stopPending != 0) {
            ERR("Stale stop entry lingered, stopPending=%zu",
                stats.stopPending);
        }

        multiTimerRegister(t, 1000, 0, testCountingCallback, &state);
        if (multiTimerCount(t) != 1) {
            ERR("Expected 1 timer, got %zu", multiTimerCount(t));
        }
    }

    TEST("multiTimer: unregister of fired id is exact with live timers") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        multiTimerId a =
            multiTimerRegister(t, 1000, 0, testCountingCallback, &state);
        multiTimerRegister(t, 60000000, 0, testCountingCallback, &state);
        sleepUs(3000);
        multiTimerProcessTimerEvents(t);
        if (state.callCount != 1) {
            ERR("Near timer did not fire, callCount=%d", state.callCount);
        }

        /* The far timer keeps storage nonempty, so no drain purge can
         * mask a stale stop entry: the unregister itself must detect
         * the id is gone */
        if (!multiTimerUnregister(t, a)) {
            ERR("Unregister of fired id should return true");
        }
        if (multiTimerCount(t) != 1) {
            ERR("Fired-id unregister skewed count, got %zu",
                multiTimerCount(t));
        }

        multiTimerStats stats;
        multiTimerGetStats(t, &stats);
        if (stats.stopPending != 0) {
            ERR("Fired-id unregister planted a stop entry, stopPending=%zu",
                stats.stopPending);
        }
    }

    TEST("multiTimer: mid-callback unregister of fired id reconciles") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        multiTimerId a =
            multiTimerRegister(t, 1000, 0, testCountingCallback, &state);
        multiTimerRegister(t, 60000000, 0, testCountingCallback, &state);
        sleepUs(3000);
        multiTimerProcessTimerEvents(t);
        if (state.callCount != 1) {
            ERR("Near timer did not fire, callCount=%d", state.callCount);
        }

        /* A later callback unregisters the long-dead id a; the
         * end-of-batch reconcile must drop the stale stop entry */
        testUnregisterOtherState unregState = {.target = a, .callCount = 0};
        multiTimerRegister(t, 1000, 0, testUnregisterOtherCallback,
                           &unregState);
        sleepUs(3000);
        multiTimerProcessTimerEvents(t);
        if (unregState.callCount != 1) {
            ERR("Unregistering callback did not fire, callCount=%d",
                unregState.callCount);
        }

        if (multiTimerCount(t) != 1) {
            ERR("Stale mid-callback stop skewed count, got %zu",
                multiTimerCount(t));
        }

        multiTimerStats stats;
        multiTimerGetStats(t, &stats);
        if (stats.stopPending != 0) {
            ERR("Stale mid-callback stop entry lingered, stopPending=%zu",
                stats.stopPending);
        }
    }

    TEST("multiTimer: mid-callback unregister of live timer still stops it") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState victim = {.callCount = 0, .shouldReschedule = false};

        /* Victim fires later than the unregistering callback */
        multiTimerId victimId =
            multiTimerRegister(t, 100000, 0, testCountingCallback, &victim);
        testUnregisterOtherState unregState = {.target = victimId,
                                               .callCount = 0};
        multiTimerRegister(t, 1000, 0, testUnregisterOtherCallback,
                           &unregState);
        sleepUs(3000);
        multiTimerProcessTimerEvents(t);
        if (unregState.callCount != 1) {
            ERR("Unregistering callback did not fire, callCount=%d",
                unregState.callCount);
        }

        /* The victim's stop entry matches a live stored timer, so the
         * reconcile must keep it: count excludes the victim */
        if (multiTimerCount(t) != 0) {
            ERR("Live-victim stop lost by reconcile, count=%zu",
                multiTimerCount(t));
        }

        sleepUs(150000);
        multiTimerProcessTimerEvents(t);
        if (victim.callCount != 0) {
            ERR("Cancelled victim fired anyway, callCount=%d",
                victim.callCount);
        }
        if (multiTimerCount(t) != 0) {
            ERR("Expected empty timer set, got %zu", multiTimerCount(t));
        }
    }

    TEST("multiTimer: self-unregister without repeat leaves no stop entry") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState state = {.callCount = 0};

        multiTimerRegister(t, 1000, 0, testSelfUnregisterNoRepeatCallback,
                           &state);
        sleepUs(3000);
        multiTimerProcessTimerEvents(t);
        if (state.callCount != 1) {
            ERR("Timer did not fire, callCount=%d", state.callCount);
        }

        multiTimerStats stats;
        multiTimerGetStats(t, &stats);
        if (stats.stopPending != 0) {
            ERR("Self-unregister leaked a stop entry, stopPending=%zu",
                stats.stopPending);
        }
    }

    TEST("multiTimer: stopAll then register new timers") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState old = {.callCount = 0};
        testCallbackState fresh = {.callCount = 0};

        for (int32_t i = 0; i < 5; i++) {
            multiTimerRegister(t, 50000, 0, testCountingCallback, &old);
        }

        multiTimerStopAll(t);
        if (multiTimerCount(t) != 0) {
            ERR("Expected 0 timers after StopAll, got %zu", multiTimerCount(t));
        }

        multiTimerRegister(t, 5000, 0, testCountingCallback, &fresh);
        if (multiTimerCount(t) != 1) {
            ERR("Expected 1 timer after re-register, got %zu",
                multiTimerCount(t));
        }

        sleepUs(60000);
        multiTimerProcessTimerEvents(t);
        if (old.callCount != 0) {
            ERR("Stopped timers fired, callCount=%d", old.callCount);
        }
        if (fresh.callCount != 1) {
            ERR("New timer did not fire, callCount=%d", fresh.callCount);
        }
    }

    TEST("multiTimer: stopAll from within callback") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState stopper = {.callCount = 0};
        testCallbackState other = {.callCount = 0, .shouldReschedule = true};
        testCallbackState fresh = {.callCount = 0};

        multiTimerRegister(t, 5000, 5000, testStopAllCallback, &stopper);
        multiTimerRegister(t, 50000, 5000, testCountingCallback, &other);

        sleepUs(10000);
        multiTimerProcessTimerEvents(t);
        if (stopper.callCount != 1) {
            ERR("Stopper did not fire, callCount=%d", stopper.callCount);
        }

        sleepUs(50000);
        multiTimerProcessTimerEvents(t);
        if (stopper.callCount != 1) {
            ERR("Stopper rescheduled despite StopAll, callCount=%d",
                stopper.callCount);
        }
        if (other.callCount != 0) {
            ERR("Other timer fired despite StopAll, callCount=%d",
                other.callCount);
        }
        if (multiTimerCount(t) != 0) {
            ERR("Expected 0 timers after StopAll, got %zu", multiTimerCount(t));
        }

        multiTimerRegister(t, 1000, 0, testCountingCallback, &fresh);
        sleepUs(3000);
        multiTimerProcessTimerEvents(t);
        if (fresh.callCount != 1) {
            ERR("Post-StopAll timer did not fire, callCount=%d",
                fresh.callCount);
        }
    }

    TEST("multiTimer: process from within callback is a safe no-op") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState reentrant = {.callCount = 0};
        testCallbackState later = {.callCount = 0};

        multiTimerRegister(t, 1000, 0, testReentrantProcessCallback,
                           &reentrant);
        multiTimerRegister(t, 2000, 0, testCountingCallback, &later);

        sleepUs(5000);
        const uint64_t diagBefore = dkDiagCount();
        multiTimerProcessTimerEvents(t);

        if (reentrant.callCount != 1) {
            ERR("Reentrant callback fired %d times, expected 1",
                reentrant.callCount);
        }
        /* The ignored inner call is reported, once */
        dkDiagnostic d;
        if (dkDiagCount() != diagBefore + 1 || !dkDiagLast(&d) ||
            d.code != DK_DIAG_TIMER_REENTERED) {
            ERR("a re-entered ProcessTimerEvents must report "
                "TIMER_REENTERED once");
        }
        if (later.callCount != 1) {
            ERR("Second timer fired %d times, expected 1", later.callCount);
        }
    }

    TEST("multiTimer: count includes callback-registered timers") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        multiTimerRegister(t, 1000, 0, testNestedTimerCallback, &state);
        sleepUs(3000);
        multiTimerProcessTimerEvents(t);

        /* Nested registration happened inside the callback; count must
         * see it immediately after processing */
        if (multiTimerCount(t) != 1) {
            ERR("Expected 1 timer (nested), got %zu", multiTimerCount(t));
        }
    }

    TEST("multiTimer: PERF stopAll at scale is O(1)") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};
        const size_t numTimers = 50000;

        /* Fire-and-forget a large population first so nextTimerId is
         * large, then register a small live set: the old
         * per-id-tombstone StopAll degraded on nextTimerId, not on the
         * live count */
        for (size_t i = 0; i < numTimers; i++) {
            multiTimerRegister(t, 0, 0, testCountingCallback, &state);
        }
        multiTimerProcessTimerEvents(t);

        for (size_t i = 0; i < 10; i++) {
            multiTimerRegister(t, 1'000'000, 0, testCountingCallback, &state);
        }

        const uint64_t start = timeUtilMonotonicNs();
        multiTimerStopAll(t);
        const uint64_t elapsed = timeUtilMonotonicNs() - start;

        printf("    StopAll with %zu lifetime ids: %" PRIu64 "ns\n",
               numTimers + 10, elapsed);

        if (multiTimerCount(t) != 0) {
            ERR("Expected 0 timers after StopAll, got %zu", multiTimerCount(t));
        }

        multiTimerStats stats;
        multiTimerGetStats(t, &stats);
        if (stats.stopPending != 0) {
            ERR("StopAll materialized stop entries, stopPending=%zu",
                stats.stopPending);
        }
    }

    /* ================================================================
     * Performance Tests
     * ================================================================ */

    TEST("multiTimer: registration performance") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState state = {};
        const size_t numOps = 100000;

        PERF_TIMERS_SETUP;

        for (size_t i = 0; i < numOps; i++) {
            multiTimerRegister(t, 1'000'000 + i, 0, testCountingCallback,
                               &state);
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(numOps, "timer registrations");

        printf("    Registered %zu timers\n", multiTimerCount(t));
    }

    TEST("multiTimer: unregistration performance") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState state = {};
        const size_t numOps = 10000;

        /* First register all timers */
        for (size_t i = 0; i < numOps; i++) {
            multiTimerRegister(t, 1'000'000 + i, 0, testCountingCallback,
                               &state);
        }

        PERF_TIMERS_SETUP;

        /* Unregister all timers */
        for (size_t i = 1; i <= numOps; i++) {
            multiTimerUnregister(t, (multiTimerId)i);
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(numOps, "timer unregistrations");
    }

    TEST("multiTimer: nextTimerEvent lookup performance") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState state = {};
        const size_t numTimers = 10000;
        const size_t numLookups = 100000;

        /* Register many timers */
        for (size_t i = 0; i < numTimers; i++) {
            multiTimerRegister(t, 1'000'000 + i * 100, 0, testCountingCallback,
                               &state);
        }

        PERF_TIMERS_SETUP;

        [[maybe_unused]] volatile multiTimerSystemMonotonicUs next = 0;
        for (size_t i = 0; i < numLookups; i++) {
            next = multiTimerNextTimerEventStartUs(t);
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(numLookups, "nextTimerEvent lookups");
    }

    TEST("multiTimer: process performance with many expired timers") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        const size_t numTimers = 10000;
        int32_t totalFired = 0;

        cleanerName(zfree, States) testCallbackState *states =
            zcalloc(numTimers, sizeof(testCallbackState));

        /* Register many timers that expire immediately */
        for (size_t i = 0; i < numTimers; i++) {
            states[i].shouldReschedule = false;
            multiTimerRegister(t, 0, 0, testCountingCallback, &states[i]);
        }

        PERF_TIMERS_SETUP;

        multiTimerProcessTimerEvents(t);

        PERF_TIMERS_FINISH_PRINT_RESULTS(numTimers, "timer executions");

        for (size_t i = 0; i < numTimers; i++) {
            totalFired += states[i].callCount;
        }

        if ((size_t)totalFired != numTimers) {
            ERR("Expected %zu firings, got %d", numTimers, totalFired);
        }
    }

    /* ================================================================
     * Timer Mode Tests
     * ================================================================ */

    TEST("multiTimer: timersInclusiveOfTimerRuntime mode") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        t->timersInclusiveOfTimerRuntime = true;

        testCallbackState state = {.callCount = 0, .shouldReschedule = true};

        /* Register timer with 10ms repeat */
        multiTimerRegister(t, 10000, 10000, testCountingCallback, &state);

        /* Simulate slow processing by sleeping inside fire loop */
        sleepUs(15000);
        multiTimerProcessTimerEvents(t);

        /* In inclusive mode, next timer should fire at originalTime +
         * 2*interval, not at now + interval */
        sleepUs(15000);
        multiTimerProcessTimerEvents(t);

        /* Should have fired at least twice */
        if (state.callCount < 2) {
            ERR("Expected at least 2 firings in inclusive mode, got %d",
                state.callCount);
        }
    }

    /* ================================================================
     * Stop Events Range Tracking Tests
     * ================================================================ */

    TEST("multiTimer: stopLowest/stopHighest tracking") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        /* Register 10 timers */
        for (int32_t i = 0; i < 10; i++) {
            multiTimerRegister(t, 100000, 0, testCountingCallback, &state);
        }

        /* Unregister timers 3, 5, 7 */
        multiTimerUnregister(t, 3);
        multiTimerUnregister(t, 5);
        multiTimerUnregister(t, 7);

        /* Verify bounds are tracked correctly */
        if (t->stopLowest != 3) {
            ERR("stopLowest should be 3, got %" PRIu64, t->stopLowest);
        }
        if (t->stopHighest != 7) {
            ERR("stopHighest should be 7, got %" PRIu64, t->stopHighest);
        }

        /* Wait and process - unregistered timers should not fire */
        sleepUs(110000);
        multiTimerProcessTimerEvents(t);

        /* 7 timers should have fired (10 - 3 unregistered) */
        if (state.callCount != 7) {
            ERR("Expected 7 firings, got %d", state.callCount);
        }
    }

    TEST("multiTimer: stopEvents bounds reset after all processed") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = false};

        /* Register 3 timers */
        multiTimerId id1 =
            multiTimerRegister(t, 50000, 0, testCountingCallback, &state);
        multiTimerId id2 =
            multiTimerRegister(t, 50000, 0, testCountingCallback, &state);
        multiTimerId id3 =
            multiTimerRegister(t, 50000, 0, testCountingCallback, &state);

        /* Unregister all */
        multiTimerUnregister(t, id1);
        multiTimerUnregister(t, id2);
        multiTimerUnregister(t, id3);

        /* Process - all stop events should be consumed */
        sleepUs(60000);
        multiTimerProcessTimerEvents(t);

        /* Bounds should be reset */
        if (t->stopLowest != 0 || t->stopHighest != 0) {
            ERR("stopLowest/stopHighest not reset: %" PRIu64 "/%" PRIu64,
                t->stopLowest, t->stopHighest);
        }

        /* No timers should have fired */
        if (state.callCount != 0) {
            ERR("Timers fired despite being unregistered, count=%d",
                state.callCount);
        }
    }

    /* ================================================================
     * High-Scale Performance Tests
     * ================================================================ */

    TEST("multiTimer: high-scale million timer registration") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState state = {};
        const size_t numTimers = 1'000'000;

        printf("    Registering %zu timers...\n", numTimers);

        PERF_TIMERS_SETUP;

        for (size_t i = 0; i < numTimers; i++) {
            /* Spread timers across 1 hour window */
            multiTimerRegister(t, (i % 3600000) * 1000, 0, testCountingCallback,
                               &state);
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(numTimers,
                                         "million timer registrations");

        size_t count = multiTimerCount(t);
        printf("    Total registered: %zu timers\n", count);

        if (count != numTimers) {
            ERR("Expected %zu timers, got %zu", numTimers, count);
        }

        /* Test lookup performance at scale */
        PERF_TIMERS_SETUP;

        [[maybe_unused]] volatile multiTimerSystemMonotonicUs next = 0;
        for (size_t i = 0; i < 100000; i++) {
            next = multiTimerNextTimerEventStartUs(t);
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(100000, "lookups with 1M timers");
    }

    TEST("multiTimer: high-scale batch expiration") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        const size_t numTimers = 100000;
        int32_t totalFired = 0;

        cleanerName(zfree, States) testCallbackState *states =
            zcalloc(numTimers, sizeof(testCallbackState));

        /* All timers expire immediately */
        for (size_t i = 0; i < numTimers; i++) {
            states[i].shouldReschedule = false;
            multiTimerRegister(t, 0, 0, testCountingCallback, &states[i]);
        }

        printf("    Processing %zu expired timers...\n", numTimers);

        PERF_TIMERS_SETUP;

        multiTimerProcessTimerEvents(t);

        PERF_TIMERS_FINISH_PRINT_RESULTS(numTimers, "batch expirations");

        for (size_t i = 0; i < numTimers; i++) {
            totalFired += states[i].callCount;
        }

        printf("    Total fired: %d\n", totalFired);

        if ((size_t)totalFired != numTimers) {
            ERR("Expected %zu firings, got %d", numTimers, totalFired);
        }
    }

    TEST("multiTimer: high-scale mixed operations simulation") {
        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();
        testCallbackState state = {.callCount = 0, .shouldReschedule = true};
        const size_t warmupTimers = 100000;
        const size_t ops = 50000;

        /* Warmup: register many timers spread across time */
        for (size_t i = 0; i < warmupTimers; i++) {
            multiTimerRegister(t, (i % 1000) * 1000 + 1'000'000, 0,
                               testCountingCallback, &state);
        }

        printf("    Simulating %zu mixed ops with %zu existing timers...\n",
               ops, warmupTimers);

        PERF_TIMERS_SETUP;

        /* Mixed operations: register, unregister, process */
        for (size_t i = 0; i < ops; i++) {
            /* Register new timer */
            multiTimerId id = multiTimerRegister(t, 1000 + (i % 10000), 0,
                                                 testCountingCallback, &state);

            /* Occasionally unregister */
            if (i % 3 == 0) {
                multiTimerUnregister(t, id);
            }

            /* Occasionally process */
            if (i % 100 == 0) {
                multiTimerProcessTimerEvents(t);
            }

            /* Always check next event */
            (void)multiTimerNextTimerEventStartUs(t);
        }

        PERF_TIMERS_FINISH_PRINT_RESULTS(ops, "mixed operations");

        printf("    Final timer count: %zu\n", multiTimerCount(t));
    }

    TEST("multiTimer: memory efficiency analysis") {
        const size_t numTimers = 100000;
        testCallbackState state = {};

        cleaner(multiTimerFree) multiTimer *t = multiTimerNew();

        /* Measure memory before */
        size_t memBefore = multimapBytes(t->scheduled);

        for (size_t i = 0; i < numTimers; i++) {
            multiTimerRegister(t, i * 1000, 0, testCountingCallback, &state);
        }

        size_t memAfter = multimapBytes(t->scheduled);
        double bytesPerTimer = (double)(memAfter - memBefore) / numTimers;

        printf("    Memory for %zu timers: %zu bytes (%.2f MB)\n", numTimers,
               memAfter, (double)memAfter / (1024 * 1024));
        printf("    Bytes per timer: %.2f\n", bytesPerTimer);
        printf("    Theoretical minimum (5 uint64s): %zu bytes\n",
               5 * sizeof(uint64_t));

        /* Each timer entry has 5 elements (runAt, callback, clientData, id,
         * repeat) Minimum would be 5 * 8 = 40 bytes per entry With multimap
         * overhead, expect ~50-100 bytes per entry */
        if (bytesPerTimer > 150) {
            ERR("Memory usage too high: %.2f bytes/timer (expected < 150)",
                bytesPerTimer);
        }
    }

    TEST_FINAL_RESULT;
}
#endif
