#pragma once

/**
 * timerWheel Internal Definitions
 *
 * Private header for internal access to timerWheel structure.
 * Only include this in timerWheel.c and test files.
 */

#include "timerWheel.h"

/* Context values for tracking callback state */
#define TIMER_WHEEL_CONTEXT_USER  0
#define TIMER_WHEEL_CONTEXT_TIMER 1
