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
 *       printf("Timer %llu fired!\n", id);
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
 * Timer processing is NOT automatic. You MUST call timerWheelProcessTimerEvents()
 * periodically in your event loop. This function:
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
 *       uint64_t startAfterMicroseconds,   // Delay before first fire
 *       uint64_t repeatEveryMicroseconds,  // 0 = one-shot, >0 = repeating
 *       timerWheelCallback *cb,            // Your callback function
 *       void *clientData                   // Passed to callback
 *   );
 *
 * Timer types:
 *   - One-shot:   timerWheelRegister(tw, 5000, 0, cb, data);     // Fire once
 *   - Repeating:  timerWheelRegister(tw, 1000, 1000, cb, data);  // Fire every 1ms
 *   - Immediate:  timerWheelRegister(tw, 0, 0, cb, data);        // Fire on next process
 *   - Delayed repeat: timerWheelRegister(tw, 5000, 1000, cb, data); // Start in 5ms, then every 1ms
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
 *   - Query timer count and statistics
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
 * Note: Unregistration is O(1) - cancelled timers are tracked in a set and
 * skipped during processing, not immediately removed from wheel slots.
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
 *   - Cancel: O(1)
 *   - Tick:   O(1) amortized (with occasional cascading)
 *   - Memory: O(n) where n = number of active timers
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
#include "flex.h"
#include "multimap.h"
#include <stdint.h>

typedef struct timerWheel timerWheel;

typedef uint64_t timerWheelId;
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
timerWheelId timerWheelRegister(timerWheel *tw, uint64_t startAfterMicroseconds,
                                 uint64_t repeatEveryMicroseconds,
                                 timerWheelCallback *cb, void *clientData);

/**
 * Unregister a timer by ID.
 * Timer will not fire after this call.
 * Safe to call from within a timer callback.
 *
 * @return true on success
 */
bool timerWheelUnregister(timerWheel *tw, timerWheelId id);

/**
 * Stop all timers.
 */
bool timerWheelStopAll(timerWheel *tw);

/**
 * Get the count of scheduled timers (approximate, excludes cancelled).
 */
size_t timerWheelCount(const timerWheel *tw);

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
void timerWheelAdvanceTime(timerWheel *tw, uint64_t microseconds);

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
    size_t totalRegistrations;  /* Total timers registered since creation/reset */
    size_t totalCancellations;  /* Total timers cancelled via unregister */
    size_t totalExpirations;    /* Total timer callbacks executed */
    size_t totalCascades;       /* Timer migrations between wheel levels */
    size_t overflowCount;       /* Current timers in overflow (>18.6h) */
    size_t memoryBytes;         /* Current memory usage in bytes */
} timerWheelStats;

/**
 * Get current statistics snapshot.
 *
 * Populates stats structure with current counters and memory usage.
 * Memory calculation includes all wheel slots, overflow, and metadata.
 *
 * @param tw    Timer wheel instance
 * @param stats Output structure to populate
 */
void timerWheelGetStats(const timerWheel *tw, timerWheelStats *stats);

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
#endif
