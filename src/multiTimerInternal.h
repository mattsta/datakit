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

    /* Ids at or below this are stopped wholesale (StopAll) without
     * materializing stopEvents entries; ids are monotonic so the
     * watermark never has to be rolled back. */
    multiTimerId stoppedUpTo;

    multiTimerId stopLowest; /* shortcut endpoints for invalid timer checking */
    multiTimerId stopHighest;

    /* Stored timer entries believed stopped and awaiting lazy discard */
    size_t stopPending;

    /* A planted stop can name an id whose entry is already gone (fired
     * earlier, or bulk-deleted at batch end), which would deflate
     * counts forever. Set on every plant; cleared when a reconcile
     * proves accounting exact again. Readers (count/stats/batch end)
     * reconcile lazily when this is set, so Unregister stays O(log n)
     * and the O(n) walk runs once per cancellation batch. */
    bool stopsMaybeStale;

    multiTimerContext context;          /* USER, TIMER */
    bool timersInclusiveOfTimerRuntime; /* true == exact intervals;
                                         * false == repeat intervals
                                         * happen *after* timers run. */
};
