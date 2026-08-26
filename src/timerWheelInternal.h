#pragma once

/**
 * timerWheel Internal Definitions
 *
 * Private header for internal access to timerWheel structure.
 * Only include this in timerWheel.c and test files.
 */

#include "timerWheel.h"

/* Whether the wheel is currently inside a user callback. Registrations
 * made during a callback are deferred to the pending queue so the slot
 * flex being iterated is never mutated mid-walk. */
typedef enum timerWheelContext {
    TIMER_WHEEL_CONTEXT_USER = 0,
    TIMER_WHEEL_CONTEXT_TIMER = 1
} timerWheelContext;

/* Microseconds relative to the wheel's start time (initialStartTime).
 * All expiry times stored inside slots/overflow use this timebase;
 * convert to system monotonic time only at the public API boundary. */
typedef uint64_t timerWheelAdjustedUs;
