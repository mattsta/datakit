#pragma once

/**
 * multiTimer - Compact Sorted-Map Timer Scheduler
 *
 * Timers live in a multimap sorted by expiry, so the next-event query
 * is a first-entry read and storage stays maximally compact (~50-60
 * bytes per timer, no preallocated wheel structure). Registration and
 * cancellation are O(log n); firing walks the due prefix in order.
 * For large, churn-heavy timer populations where per-op latency
 * matters more than memory, see timerWheel instead.
 *
 * Processing is NOT automatic: call multiTimerProcessTimerEvents()
 * periodically (or sleep until multiTimerNextTimerEventOffsetFromNowUs
 * and then call it). Timers that expire between calls fire on the next
 * call; none are lost.
 *
 * Callback contract: bool cb(multiTimer *t, multiTimerId id, void *d).
 * For repeating timers (repeatEveryMicroseconds > 0) the return value
 * decides whether the timer reschedules; for one-shot timers it is
 * ignored. Inside a callback it is safe to register timers (deferred
 * until processing completes), unregister any timer including self,
 * and call multiTimerStopAll (later registrations survive). Calling
 * multiTimerProcessTimerEvents from inside a callback is a safe no-op,
 * reported as DK_DIAG_TIMER_REENTERED (dkDiag.h).
 *
 * Cancellation is a tombstone: the id is recorded in O(log n) and the
 * timer's storage is reclaimed when processing next encounters it (or
 * when the timer set drains empty). Unregistering an already-fired id
 * is harmless and count-exact: readers (multiTimerCount, GetStats,
 * end of each processing batch) lazily reconcile stop accounting
 * against stored entries — an O(n) walk that runs at most once per
 * cancellation batch, so counts stay exact at amortized O(1). Reads
 * from inside a callback skip the reconcile (the batch's bulk delete
 * hasn't run yet) and may transiently over-report stopPending until
 * the batch ends.
 */

#include "datakit.h"
#include <stdint.h>

/* @handle: fixed: its address holds from creation to free */
typedef struct multiTimer multiTimer;

typedef uint64_t multiTimerId;
typedef uint64_t multiTimerDurationUs;
typedef int64_t multiTimerUs;
typedef int64_t multiTimerSystemMonotonicUs;

typedef bool multiTimerCallback(multiTimer *t, multiTimerId id,
                                void *clientData);

/* Init / Cleanup */
[[nodiscard]] multiTimer *multiTimerNew(void);
void multiTimerFree(multiTimer *t);
bool multiTimerInit(multiTimer *t);
void multiTimerDeinit(multiTimer *t);

/* Count of live timers (registered, not yet fired or stopped).
 * Reconciles stale stop accounting first when needed (see header). */
size_t multiTimerCount(multiTimer *t);

/* Export our timer calculator */
multiTimerSystemMonotonicUs multiTimerGetNs(void);
multiTimerSystemMonotonicUs multiTimerGetUs(void);

/* Timer processing */
void multiTimerProcessTimerEvents(multiTimer *t);

/* The time of the earliest timer that will fire (a stopped timer still
 * stored is skipped).  Returns 0 when none will. */
multiTimerSystemMonotonicUs multiTimerNextTimerEventStartUs(multiTimer *t);

/* Microseconds until the next timer event (negative if overdue).
 * Returns 0 when no timers are scheduled. */
multiTimerUs multiTimerNextTimerEventOffsetFromNowUs(multiTimer *t);

/* Timers */
multiTimerId multiTimerRegister(multiTimer *t,
                                multiTimerDurationUs startAfterMicroseconds,
                                multiTimerDurationUs repeatEveryMicroseconds,
                                multiTimerCallback *cb, void *clientData);

/* Stop a timer by id.
 * Returns true if the id was ever issued by this timer, else false. */
bool multiTimerUnregister(multiTimer *t, multiTimerId id);

/* Stop all currently registered timers.
 * Safe from within a callback; timers registered afterward survive. */
bool multiTimerStopAll(multiTimer *t);

/* Live-state snapshot, computed on demand (nothing is stored). */
typedef struct multiTimerStats {
    size_t activeCount;    /* live timers (excludes stop-marked) */
    size_t scheduledCount; /* entries currently in the scheduled map */
    size_t pendingCount;   /* entries deferred from inside callbacks */
    size_t stopPending;    /* stop-marked entries awaiting lazy discard */
    size_t memoryBytes;    /* bytes across all three maps + struct */
} multiTimerStats;

void multiTimerGetStats(multiTimer *t, multiTimerStats *stats);

#ifdef DATAKIT_TEST
int multiTimerTest(int argc, char *argv[]);
#endif
