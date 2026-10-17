#pragma once

/**
 * timerWheel - High-Performance Hierarchical Timing Wheel
 *
 * A modern reimagining of hierarchical timing wheels using datakit's
 * high-performance memory primitives. Provides O(1) amortized insert
 * and O(1) amortized tick processing for timer management.
 *
 * ============================================================================
 * QUICK START
 * ============================================================================
 *
 *   // 1. Create timer wheel
 *   timerWheel *tw = timerWheelNew();
 *
 *   // 2. Define a callback
 *   bool myCallback(timerWheel *tw, timerWheelId id, void *data) {
 *       printf("Timer %" PRIu64 " fired!\n", id);
 *       return false;  // false = don't repeat, true = reschedule
 *   }
 *
 *   // 3. Register timers
 *   timerWheelId id1 = timerWheelRegister(tw, 5000, 0, myCallback, NULL);
 *   //                                        ^ 5ms delay, one-shot
 *
 *   timerWheelId id2 = timerWheelRegister(tw, 1000, 1000, myCallback, NULL);
 *   //                                        ^ 1ms delay, repeat every 1ms
 *
 *   // 4. Process timers in your event loop (REQUIRED - not automatic!)
 *   while (running) {
 *       timerWheelProcessTimerEvents(tw);  // Must call periodically!
 *       // ... do other work ...
 *       usleep(1000);  // ~1ms granularity recommended
 *   }
 *
 *   // 5. Cleanup
 *   timerWheelFree(tw);
 *
 * ============================================================================
 * TIMER PROCESSING - IMPORTANT!
 * ============================================================================
 *
 * Timer processing is NOT automatic. You MUST call
 * timerWheelProcessTimerEvents() periodically in your event loop. This
 * function:
 *   - Checks current wall-clock time
 *   - Fires all callbacks for timers that have expired
 *   - Handles timer cascading between wheel levels
 *   - Processes any timers scheduled from within callbacks
 *
 * Recommended call frequency: every 1ms for optimal timer resolution.
 * Less frequent calls still work but reduce timing precision.
 *
 * Event loop integration patterns:
 *
 *   // Pattern A: Fixed interval polling
 *   while (running) {
 *       timerWheelProcessTimerEvents(tw);
 *       usleep(1000);
 *   }
 *
 *   // Pattern B: Sleep until next timer (efficient for sparse timers)
 *   while (running) {
 *       timerWheelProcessTimerEvents(tw);
 *       int64_t waitUs = timerWheelNextTimerEventOffsetFromNowUs(tw);
 *       if (waitUs > 0) {
 *           usleep(waitUs < 10000 ? waitUs : 10000);  // cap at 10ms
 *       }
 *   }
 *
 *   // Pattern C: With select/poll/epoll
 *   while (running) {
 *       int64_t timeoutUs = timerWheelNextTimerEventOffsetFromNowUs(tw);
 *       struct timeval tv = {timeoutUs / 1000000, timeoutUs % 1000000};
 *       select(nfds, &readfds, NULL, NULL, timeoutUs > 0 ? &tv : NULL);
 *       timerWheelProcessTimerEvents(tw);
 *       // ... handle I/O ...
 *   }
 *
 * ============================================================================
 * ADDING TIMERS
 * ============================================================================
 *
 * Use timerWheelRegister() to add timers:
 *
 *   timerWheelId timerWheelRegister(
 *       timerWheel *tw,
 *       timerWheelDurationUs startAfterMicroseconds,  // Delay before first
 *       timerWheelDurationUs repeatEveryMicroseconds, // 0 = one-shot
 *       timerWheelCallback *cb,            // Your callback function
 *       void *clientData                   // Passed to callback
 *   );
 *
 * Timer types:
 *   - One-shot:   timerWheelRegister(tw, 5000, 0, cb, data);     // Fire once
 *   - Repeating:  timerWheelRegister(tw, 1000, 1000, cb, data);  // Fire every
 * 1ms
 *   - Immediate:  timerWheelRegister(tw, 0, 0, cb, data);        // Fire on
 * next process
 *   - Delayed repeat: timerWheelRegister(tw, 5000, 1000, cb, data); // Start in
 * 5ms, then every 1ms
 *
 * Returns: Timer ID (always > 0 on success), used for unregistration.
 *
 * ============================================================================
 * CALLBACK BEHAVIOR
 * ============================================================================
 *
 * Callback signature:
 *   bool myCallback(timerWheel *tw, timerWheelId id, void *clientData);
 *
 * Parameters:
 *   - tw:         Timer wheel instance (for registering new timers, etc.)
 *   - id:         ID of the timer that fired
 *   - clientData: User data passed during registration
 *
 * Return value (for repeating timers only):
 *   - true:  Reschedule timer for next interval
 *   - false: Stop timer, do not reschedule
 *
 * For one-shot timers (repeatInterval == 0), return value is ignored.
 *
 * Safe operations within callbacks:
 *   - Register new timers (deferred until callback completes)
 *   - Unregister other timers
 *   - Unregister self (timerWheelUnregister(tw, id))
 *   - Stop all timers (timerWheelStopAll; later registrations survive)
 *   - Query timer count and statistics
 *
 * Calling timerWheelProcessTimerEvents() or timerWheelAdvanceTime()
 * from inside a callback is a safe no-op for processing: the
 * in-progress outer call picks up any deferred work (AdvanceTime still
 * shifts the simulated clock; the shift takes effect on the next
 * processing pass).
 *
 * ============================================================================
 * CANCELING/UPDATING TIMERS
 * ============================================================================
 *
 * Cancel a timer:
 *   timerWheelUnregister(tw, timerId);
 *
 * Cancel all timers:
 *   timerWheelStopAll(tw);
 *
 * Update a timer (no direct API - cancel and re-register):
 *   timerWheelUnregister(tw, oldId);
 *   timerWheelId newId = timerWheelRegister(tw, newDelay, newRepeat, cb, data);
 *
 * Note: Unregistration is a tombstone - the id is recorded and the
 * timer's storage is skipped/reclaimed when processing next encounters
 * it (or when the wheel drains empty). Cancelling an id that already
 * fired never affects other timers, but the wheel has no id index, so
 * the stale tombstone makes timerWheelCount() under-report until the
 * wheel drains empty, timerWheelReconcileCancellations() runs, or
 * timerWheelGetStats() runs (it reconciles automatically, at most one
 * O(n) walk per cancellation batch, outside callbacks).
 *
 * ============================================================================
 * ARCHITECTURE
 * ============================================================================
 *
 * Four-level hierarchical wheel structure:
 *   Wheel 0: 256 slots × 1ms    = 256ms span   (fine granularity)
 *   Wheel 1:  64 slots × 256ms  = ~16 seconds  (medium granularity)
 *   Wheel 2:  64 slots × 16.4s  = ~17 minutes  (coarse granularity)
 *   Wheel 3:  64 slots × 17.5m  = ~18.6 hours  (very coarse)
 *   Overflow: Sorted multimap for timers > 18.6 hours
 *
 * Total slots: 448 (compact memory footprint)
 * Memory per timer: ~27 bytes
 *
 * Performance characteristics:
 *   - Insert: O(1) amortized
 *   - Cancel: O(log c) lookup in the tombstone set (c = outstanding
 *     cancellations; O(1) when none are outstanding)
 *   - Tick:   O(1) amortized (with occasional cascading); empty spans
 *     and idle gaps are skipped, not ticked through
 *   - Memory: O(n) where n = number of active timers
 *
 * Timing precision: higher-wheel slots cascade downward when the wheel
 * enters them, so timers at every level fire within one wheel-0 slot
 * (1ms) of their scheduled time - assuming processing is called at
 * least that often. Less frequent processing degrades precision to the
 * processing interval, never loses timers.
 *
 * ============================================================================
 * THREAD SAFETY
 * ============================================================================
 *
 * timerWheel is NOT thread-safe. All calls must be from the same thread,
 * or externally synchronized. Typical usage is single-threaded event loop.
 *
 * ============================================================================
 * TESTING/SIMULATION MODE
 * ============================================================================
 *
 * For deterministic testing without wall-clock dependency:
 *
 *   timerWheelAdvanceTime(tw, microseconds);
 *
 * This manually advances the timer wheel's internal clock and processes
 * any timers that would have fired. Useful for unit tests and simulations.
 */

#include "datakit.h"
#include <stdint.h>

typedef struct timerWheel timerWheel;

typedef uint64_t timerWheelId;
typedef uint64_t timerWheelDurationUs;
typedef int64_t timerWheelUs;
typedef int64_t timerWheelSystemMonotonicUs;

/**
 * Timer callback function type.
 *
 * Called when a timer expires. For repeating timers, return value controls
 * whether the timer is rescheduled:
 *   - Return true:  Reschedule for another interval
 *   - Return false: Cancel timer, do not reschedule
 *
 * For one-shot timers (repeatInterval == 0), return value is ignored.
 *
 * @param tw         Timer wheel instance (can register/unregister timers)
 * @param id         ID of the timer that fired
 * @param clientData User data passed during timerWheelRegister()
 * @return true to continue repeating, false to stop
 */
typedef bool timerWheelCallback(timerWheel *tw, timerWheelId id,
                                void *clientData);

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

/**
 * Create a new timer wheel instance.
 * Returns NULL on allocation failure.
 */
timerWheel *timerWheelNew(void);

/**
 * Free a timer wheel and all associated resources.
 * Safe to call with NULL.
 */
void timerWheelFree(timerWheel *tw);

/**
 * Initialize an existing timer wheel structure.
 * Returns true on success, false on allocation failure.
 */
bool timerWheelInit(timerWheel *tw);

/**
 * Deinitialize a timer wheel, freeing internal resources.
 */
void timerWheelDeinit(timerWheel *tw);

/* ====================================================================
 * Timer Management
 * ==================================================================== */

/**
 * Register a new timer.
 *
 * @param tw                    Timer wheel instance
 * @param startAfterMicroseconds Delay before first firing (0 = immediate)
 * @param repeatEveryMicroseconds Interval for repeating (0 = one-shot)
 * @param cb                    Callback function
 * @param clientData            User data passed to callback
 * @return Timer ID (never 0 on success)
 */
timerWheelId timerWheelRegister(timerWheel *tw,
                                timerWheelDurationUs startAfterMicroseconds,
                                timerWheelDurationUs repeatEveryMicroseconds,
                                timerWheelCallback *cb, void *clientData);

/**
 * Unregister a timer by ID.
 * Timer will not fire after this call.
 * Safe to call from within a timer callback.
 *
 * Cancellation is a tombstone: the timer's storage is reclaimed lazily
 * the next time processing encounters it (or when the wheel drains
 * empty). Unregistering an id that already fired never affects other
 * timers, but leaves a stale tombstone that makes timerWheelCount()
 * under-report until the wheel drains empty or
 * timerWheelReconcileCancellations() runs.
 *
 * @return true if the id was ever issued by this wheel, false otherwise
 */
bool timerWheelUnregister(timerWheel *tw, timerWheelId id);

/**
 * Stop all currently registered timers.
 * Safe to call from within a timer callback; timers registered after
 * this call (including later in the same callback) are unaffected.
 */
bool timerWheelStopAll(timerWheel *tw);

/**
 * Get the count of scheduled timers, excluding cancelled ones. O(1).
 * Exact unless ids that already fired were unregistered; call
 * timerWheelReconcileCancellations() to restore exactness after that.
 */
size_t timerWheelCount(const timerWheel *tw);

/**
 * Restore exact cancellation accounting: drops tombstones whose ids no
 * longer have stored timers (from unregistering already-fired ids), so
 * timerWheelCount() is exact again. O(n) over all stored timers.
 *
 * @return true on success; false when called from within a timer
 *         callback (storage cannot be walked safely mid-processing)
 */
bool timerWheelReconcileCancellations(timerWheel *tw);

/* ====================================================================
 * Timer Processing
 * ==================================================================== */

/**
 * Process all expired timers - MUST BE CALLED PERIODICALLY.
 *
 * This is the main "tick" function that drives the timer wheel. It:
 *   1. Checks current wall-clock time
 *   2. Advances internal wheel state
 *   3. Cascades timers from higher wheels as needed
 *   4. Executes callbacks for all expired timers
 *   5. Handles any timers registered during callbacks
 *
 * Call frequency recommendations:
 *   - Every 1ms:  Best timer resolution, higher CPU usage
 *   - Every 10ms: Good balance for most applications
 *   - On-demand:  Use timerWheelNextTimerEventOffsetFromNowUs() to sleep
 *
 * Timers that expire while this function is not being called will fire
 * immediately on the next call (no timers are lost).
 *
 * @param tw Timer wheel instance
 */
void timerWheelProcessTimerEvents(timerWheel *tw);

/**
 * Advance time by specified microseconds without wall-clock dependency.
 *
 * For deterministic testing and simulations. Advances the timer wheel's
 * internal clock by the specified amount and processes any timers that
 * would have fired during that interval.
 *
 * Example:
 *   timerWheelRegister(tw, 5000, 0, cb, data);  // Fire in 5ms
 *   timerWheelAdvanceTime(tw, 5000);            // Advance 5ms, timer fires
 *
 * @param tw           Timer wheel instance
 * @param microseconds Time to advance (in microseconds)
 */
void timerWheelAdvanceTime(timerWheel *tw, timerWheelDurationUs microseconds);

/* ====================================================================
 * Timer Queries
 * ==================================================================== */

/**
 * Get absolute monotonic time (us) of the next timer event.
 *
 * Useful for integrating with event loops that need absolute timestamps.
 * The returned value is in the same time base as timeUtilMonotonicUs().
 *
 * @param tw Timer wheel instance
 * @return Absolute time in microseconds, or 0 if no timers scheduled
 */
timerWheelSystemMonotonicUs timerWheelNextTimerEventStartUs(timerWheel *tw);

/**
 * Get microseconds until the next timer event fires.
 *
 * Useful for calculating sleep/poll timeout in event loops:
 *
 *   int64_t waitUs = timerWheelNextTimerEventOffsetFromNowUs(tw);
 *   if (waitUs > 0) {
 *       usleep(waitUs);
 *   }
 *   timerWheelProcessTimerEvents(tw);
 *
 * @param tw Timer wheel instance
 * @return Microseconds until next timer (negative if overdue, 0 if none)
 */
timerWheelUs timerWheelNextTimerEventOffsetFromNowUs(timerWheel *tw);

/* ====================================================================
 * Statistics
 * ==================================================================== */

/**
 * Statistics structure for performance monitoring and debugging.
 */
typedef struct timerWheelStats {
    size_t
        totalRegistrations; /* Total timers registered since creation/reset */
    size_t totalCancellations; /* Total timers cancelled via unregister */
    size_t totalExpirations;   /* Total timer callbacks executed */
    size_t totalCascades;      /* Timer migrations between wheel levels */
    size_t overflowCount;      /* Current timers in overflow (>18.6h) */
    size_t pendingCount;       /* Current timers in the pending queue (deferred
                                * from callbacks or sub-resolution delays) */
    size_t cancelledPending;   /* Current cancelled timers awaiting lazy
                                * discard (tombstone backlog) */
    size_t memoryBytes;        /* Current memory usage in bytes */
} timerWheelStats;

/**
 * Get current statistics snapshot.
 *
 * Populates stats structure with current counters and memory usage.
 * Memory calculation includes all wheel slots, overflow, and metadata.
 * When cancellations since the last reconcile might be stale (an
 * already-fired id was unregistered), this first runs the O(n)
 * timerWheelReconcileCancellations walk - at most once per
 * cancellation batch - so cancelledPending and timerWheelCount() are
 * exact afterward. Mid-callback the reconcile is skipped and raw
 * counters are reported.
 *
 * @param tw    Timer wheel instance
 * @param stats Output structure to populate
 */
void timerWheelGetStats(timerWheel *tw, timerWheelStats *stats);

/**
 * Reset statistics counters to zero.
 *
 * Resets registration, cancellation, expiration, and cascade counters.
 * Does not affect overflowCount (current state) or memoryBytes.
 *
 * @param tw Timer wheel instance
 */
void timerWheelResetStats(timerWheel *tw);

#ifdef DATAKIT_TEST
int timerWheelTest(int argc, char *argv[]);

/* Test hooks for the next-event differential test: drop the next-event
 * cache so the next query walks the wheel, and a brute-force answer to
 * the same query that decodes every entry (skipping tombstones) and
 * uses none of the bounds the real query relies on. */
void timerWheelTestInvalidateNextEventCache(timerWheel *tw);
timerWheelSystemMonotonicUs timerWheelTestNextEventOracle(const timerWheel *tw);
#endif
