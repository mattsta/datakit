#pragma once

#include <stdint.h>

#include "multiTimer.h"
#include "multimap.h"

typedef enum multiTimerContext {
    MULTI_TIMER_CONTEXT_USER = 1,
    MULTI_TIMER_CONTEXT_TIMER,
} multiTimerContext;

struct multiTimer {
    multimap *scheduled;         /* active timers */
    multimap *pendingScheduling; /* to-be-active timers */
    multimap *stopEvents;        /* instead of deleting timers, we mark them to
                                  * just not run later. */
    multiTimerId nextTimerId;
    multiTimerSystemMonotonicUs initialStartTime;
    multiTimerId stopLowest; /* shortcut endpoints for invalid timer checking */
    multiTimerId stopHighest;
    multiTimerContext context;          /* USER, TIMER */
    bool timersInclusiveOfTimerRuntime; /* true == exact intervals;
                                         * false == repeat intervals
                                         * happen *after* timers run. */
};
