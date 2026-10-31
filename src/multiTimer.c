#include "multiTimer.h"
#include "multiTimerInternal.h"

#include "cleaner.h"
#include "dkDiag.h"
#include "timeUtil.h"

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleaner(multiTimer *, multiTimerFree);
defcleanerName(uint64_t *, zfree, U64);
defcleanerName(bool *, zfree, Bools);

typedef uint64_t multiTimerAdjustedUs;

/* A timer event decoded from (or destined for) its 5-element multimap
 * wire form. Entries sort by runAtMicroseconds (the first element), so
 * the next-due timer is always the map's first entry. */
typedef struct multiTimerEvent {
    multiTimerAdjustedUs runAtMicroseconds;
    multiTimerCallback *cb;
    void *clientData;
    multiTimerId id;
    multiTimerDurationUs repeatIntervalMicroseconds;
} multiTimerEvent;

static constexpr int ELEMENTS_PER_EVENT = 5;

bool multiTimerInit(multiTimer *t) {
    /* Full zero first: multiTimer is documented as stack-initializable,
     * so no field may inherit stack garbage (nextTimerId especially). */
    *t = (multiTimer){};

    t->context = MULTI_TIMER_CONTEXT_USER;
    t->timersInclusiveOfTimerRuntime = false;

    /* @own: container; free=multimapFree */
    t->scheduled = multimapNew(ELEMENTS_PER_EVENT);
    /* @own: container; free=multimapFree */
    t->pendingScheduling = multimapNew(ELEMENTS_PER_EVENT);
    /* @own: container; free=multimapFree */
    t->stopEvents = multimapNew(1);

    if (!t->scheduled || !t->pendingScheduling || !t->stopEvents) {
        multiTimerDeinit(t);
        return false;
    }

    /* Stored times are offsets from this base; processing moves it to the
     * present whenever no timer is stored */
    t->initialStartTime = timeUtilMonotonicUs();
    return true;
}

void multiTimerDeinit(multiTimer *t) {
    if (t) {
        multimapFree(t->scheduled);
        multimapFree(t->pendingScheduling);
        multimapFree(t->stopEvents);
        *t = (multiTimer){};
    }
}

static void reconcileStops(multiTimer *t);

size_t multiTimerCount(multiTimer *t) {
    /* Mid-callback the maps hold already-run entries until the batch's
     * bulk delete, so reconciling would misclassify; the batch end
     * reconciles instead and the count self-heals there */
    if (t->stopsMaybeStale && t->context == MULTI_TIMER_CONTEXT_USER) {
        reconcileStops(t);
    }

    const size_t present =
        multimapCount(t->scheduled) + multimapCount(t->pendingScheduling);
    return present > t->stopPending ? present - t->stopPending : 0;
}

void multiTimerFree(multiTimer *t) {
    if (t) {
        multiTimerDeinit(t);
        zfree(t);
    }
}

multiTimer *multiTimerNew(void) {
    cleaner(multiTimerFree) multiTimer *t = zcalloc(1, sizeof(*t));

    if (!multiTimerInit(t)) {
        return nullptr;
    }

    multiTimer *out = t;
    t = nullptr;
    return out;
}

static multiTimerSystemMonotonicUs
adjustedToNative(multiTimer *t, multiTimerAdjustedUs adjusted) {
    return t->initialStartTime + adjusted;
}

static multiTimerAdjustedUs
nativeToAdjusted(multiTimer *t, multiTimerSystemMonotonicUs native) {
    return native - t->initialStartTime;
}

static multiTimerAdjustedUs adjustedNowUs(multiTimer *t) {
    return nativeToAdjusted(t, timeUtilMonotonicUs());
}

/* Expiry for a delay from `base`, saturating instead of wrapping: an
 * absurd duration becomes "effectively never" rather than silently
 * firing immediately. */
static inline multiTimerAdjustedUs expiryAfter(multiTimerAdjustedUs base,
                                               multiTimerDurationUs delay) {
    const multiTimerAdjustedUs expire = base + delay;
    return expire >= base ? expire : UINT64_MAX;
}

/* Decode a stored 5-element entry (as handed to multimap runners) */
static void readTimerEvent(const databox *const elements[],
                           multiTimerEvent *e) {
    e->runAtMicroseconds = elements[0]->data.u64;
    e->cb = (multiTimerCallback *)databoxRetrievePtrUnsigned(elements[1]);
    e->clientData = databoxRetrievePtrUnsigned(elements[2]);
    e->id = elements[3]->data.u64;
    e->repeatIntervalMicroseconds = elements[4]->data.u64;
}

/* Box an event into wire form and insert it into `dest` (scheduled or
 * pendingScheduling) */
static void writeTimerEvent(multimap **dest, const multiTimerEvent *e) {
    const databox runAt = databoxNewUnsigned(e->runAtMicroseconds);
    const databox cb = databoxNewPtrAsUnsigned(e->cb);
    const databox clientData = databoxNewPtrAsUnsigned(e->clientData);
    const databox id = databoxNewUnsigned(e->id);
    const databox repeat = databoxNewUnsigned(e->repeatIntervalMicroseconds);

    const databox *entry[ELEMENTS_PER_EVENT] = {&runAt, &cb, &clientData, &id,
                                                &repeat};

    multimapInsertFullWidth(dest, entry);
}

multiTimerId multiTimerRegister(multiTimer *t,
                                multiTimerDurationUs startAfterMicroseconds,
                                multiTimerDurationUs repeatEveryMicroseconds,
                                multiTimerCallback *cb, void *clientData) {
    const multiTimerEvent e = {
        .runAtMicroseconds =
            expiryAfter(adjustedNowUs(t), startAfterMicroseconds),
        .cb = cb,
        .clientData = clientData,
        .id = ++t->nextTimerId,
        .repeatIntervalMicroseconds = repeatEveryMicroseconds};

    /* We can't modify timers while we're executing a timer, so
     * creating a timer while in a timer means we insert into the
     * to-schedule-timers map. */
    writeTimerEvent(t->context == MULTI_TIMER_CONTEXT_TIMER
                        ? &t->pendingScheduling
                        : &t->scheduled,
                    &e);

    return e.id;
}

/* Extend stopLowest/stopHighest for a stop entry just added */
static void stopBoundsExtend(multiTimer *t, multiTimerId id) {
    if (multimapCount(t->stopEvents) == 1) {
        /* First stop event - initialize both bounds */
        t->stopLowest = id;
        t->stopHighest = id;
    } else {
        /* Update bounds if new id extends the range */
        if (id < t->stopLowest) {
            t->stopLowest = id;
        }

        if (id > t->stopHighest) {
            t->stopHighest = id;
        }
    }
}

/* Recompute stopLowest/stopHighest after a stop entry was consumed
 * (multimapFirst/Last are cheap first/last-entry reads) */
static void stopBoundsAfterRemoval(multiTimer *t) {
    if (multimapCount(t->stopEvents) > 0) {
        databox high;
        databox low;

        multimapLastInto(t->stopEvents, &high, 1);
        multimapFirstInto(t->stopEvents, &low, 1);

        t->stopHighest = high.data.u64;
        t->stopLowest = low.data.u64;
    } else {
        /* All stop events processed - reset bounds */
        t->stopHighest = 0;
        t->stopLowest = 0;
    }
}

bool multiTimerUnregister(multiTimer *t, multiTimerId id) {
    if (id == 0 || id > t->nextTimerId) {
        return false;
    }

    /* Already stopped wholesale by a StopAll watermark */
    if (id <= t->stoppedUpTo) {
        return true;
    }

    const databox stop = databoxNewUnsigned(id);
    const databox *stopTimer[1] = {&stop};

    /* One search plants the stop entry; a double unregister finds it
     * there and plants no duplicate.  The id may already be gone (fired,
     * or dying in the current batch); readers reconcile lazily via
     * stopsMaybeStale, keeping this call O(log n) */
    if (!multimapInsertIfAbsent(&t->stopEvents, stopTimer)) {
        return true;
    }

    stopBoundsExtend(t, id);
    t->stopPending++;
    t->stopsMaybeStale = true;

    return true;
}

bool multiTimerStopAll(multiTimer *t) {
    /* Ids are monotonic, so "stop everything registered so far" is one
     * watermark: any id at or below it is stopped. Individual stop
     * entries are all at or below the watermark too, so the stop map
     * can be dropped wholesale. */
    t->stoppedUpTo = t->nextTimerId;
    if (multimapCount(t->stopEvents) > 0) {
        multimapReset(&t->stopEvents);
    }
    t->stopLowest = 0;
    t->stopHighest = 0;

    if (t->context == MULTI_TIMER_CONTEXT_TIMER) {
        /* Mid-callback the scheduled map is being iterated, so leave
         * the entries in place; the watermark makes processing discard
         * each one lazily on encounter. */
        t->stopPending =
            multimapCount(t->scheduled) + multimapCount(t->pendingScheduling);
        /* That counts entries this batch already ran, which wait for its
         * bulk delete and are never consumed as stops: the batch's end
         * recounts exactly */
        t->stopsMaybeStale = true;
        return true;
    }

    multimapReset(&t->scheduled);
    multimapReset(&t->pendingScheduling);
    t->stopPending = 0;
    return true;
}

static bool timerCopyRunner(void *userData, const databox *elements[]) {
    multiTimer *t = userData;
    multimapInsertFullWidth(&t->scheduled, elements);
    return true;
}

/* If timer `id` is stopped, consume its tombstone and report true; the
 * caller then discards the timer's storage. */
static bool checkTimerExceptions(multiTimer *t, multiTimerId id) {
    /* Quick check: nothing can be stopped while stopPending is zero */
    if (t->stopPending == 0) {
        return false;
    }

    /* Stopped wholesale by a StopAll watermark: no map entry to consume */
    if (id <= t->stoppedUpTo) {
        t->stopPending--;
        return true;
    }

    if (multimapCount(t->stopEvents) == 0) {
        return false;
    }

    /* If `id` is in the range of timers requested for deletion, then
     * delete timer from timer deletion map and update endpoints for
     * deleted timer ranges. */
    if (id <= t->stopHighest && id >= t->stopLowest) {
        const databox stop = databoxNewUnsigned(id);
        /* One search: the delete reports whether the id was there */
        if (multimapDelete(&t->stopEvents, &stop)) {
            /* Remove from times to stop since we're not running it. */
            t->stopPending--;

            stopBoundsAfterRemoval(t);
            return true;
        }
    }

    return false;
}

static bool cleanupTimersUpTo(multiTimer *t, size_t processedTimersCount,
                              const multimapPredicate *predicateDelete) {
    /* If we processed every existing timer, just remove all
     * currently existing timers. */
    if (processedTimersCount == multimapCount(t->scheduled)) {
        multimapReset(&t->scheduled);
    } else {
        /* else, we need to selectively remove only a subset of
         * currently active timers. */
        multimapDeleteByPredicate(&t->scheduled, predicateDelete);
    }

    return true;
}

static bool rescheduleTimers(multiTimer *t) {
    /* Check if any timer events re-scheduled new timers.
     * If so, we need to move timers from pending to scheduled. */
    if (multimapCount(t->pendingScheduling) > 0) {
        /* insert all from 'pendingScheduling' into 'scheduled' */
        if (multimapCount(t->scheduled) == 0) {
            /* We have no scheduled timers, so we can just swap
             * pending timers to be our new scheduled timers! */
            multimap *const tmp = t->scheduled;
            t->scheduled = t->pendingScheduling;
            t->pendingScheduling = tmp;
        } else {
            const multimapPredicate predicateAll = {.condition =
                                                        MULTIMAP_CONDITION_ALL};

            /* else, insert every pending timer to scheduled timers. */
            multimapProcessUntil(t->pendingScheduling, &predicateAll, true,
                                 timerCopyRunner, t);

            multimapReset(&t->pendingScheduling);
        }

        return true;
    }

    return false;
}

/* This is the callback passed to multimapProcessUntil() to run
 * for each currently valid timer expiration.
 *
 * The input of 'userData' is passed through from initial calls and
 * elements[] is the current multiTimer timer to run.
 *
 * If the timer callback returns true, the event is rescheduled
 * for its next duration.
 * If the timer callback returns false, the event is not scheduled again.
 * timerRunner() returns the reschedule status of the single
 * timer executed during this run. */
static bool timerRunner(void *userData, const databox *elements[]) {
    multiTimer *restrict t = userData;

    multiTimerEvent e;
    readTimerEvent(elements, &e);

    /* If this timer is within the range of potential timers to stop,
     * check if we really should ignore this timer */
    if (checkTimerExceptions(t, e.id)) {
        /* Skip this timer, but continue processing future timers */
        return true;
    }

    const bool reschedule = e.cb(t, e.id, e.clientData);

    /* Consume a stop requested during the timer's own callback whether
     * or not it asked to repeat - otherwise a self-unregister that
     * returns false leaks its stop entry forever. */
    if (checkTimerExceptions(t, e.id)) {
        return true;
    }

    /* If timer event has a repeat interval, copy timer event into
     * the future. */
    if (reschedule && e.repeatIntervalMicroseconds) {
        /* If 'timersInclusiveOfTimerRuntime', then we just boost the
         * current run offset of timer by its repeat interval; else,
         * calculate new timer start based on now()+repeat. */
        const multiTimerAdjustedUs newStartBaseUs =
            t->timersInclusiveOfTimerRuntime ? e.runAtMicroseconds
                                             : adjustedNowUs(t);

        e.runAtMicroseconds =
            expiryAfter(newStartBaseUs, e.repeatIntervalMicroseconds);
        writeTimerEvent(&t->pendingScheduling, &e);
    }

    /* Continue processing future timers */
    return true;
}

/* Drop stop entries whose ids no longer have stored timers and
 * recompute stopPending exactly: a stop of an already-gone id would
 * otherwise deflate multiTimerCount() forever. Runs only when
 * stopsMaybeStale is set, so the O(n) walk amortizes to once per
 * cancellation batch. */
static void reconcileStops(multiTimer *t) {
    t->stopsMaybeStale = false;

    if (t->stopPending == 0) {
        /* Every plant was consumed (e.g. self-unregisters); the
         * accounting is already exact */
        return;
    }

    const size_t stopCount = multimapCount(t->stopEvents);

    cleanerName(zfree, U64) uint64_t *stopIds = nullptr;
    cleanerName(zfree, Bools) bool *stopSeen = nullptr;
    if (stopCount) {
        stopIds = zmalloc(stopCount * sizeof(*stopIds));
        stopSeen = zcalloc(stopCount, sizeof(*stopSeen));

        multimapIterator iter;
        multimapIteratorInit(t->stopEvents, &iter, true);

        databox idBox;
        size_t i = 0;
        while (i < stopCount && multimapIteratorNextInto(&iter, &idBox, 1)) {
            stopIds[i++] = idBox.data.u64;
        }
    }

    /* One walk over stored timers: count watermark-stopped entries and
     * mark which individual stop entries still match a stored timer.
     * stopIds is ascending (stopEvents is a sorted map), so membership
     * is a binary search. pendingScheduling is empty at every current
     * call site (processing always drains it first) but is walked so
     * the reconcile stays correct if a future call site runs between
     * plant and drain. */
    size_t liveStops = 0;
    const multimap *maps[2] = {t->scheduled, t->pendingScheduling};
    for (size_t m = 0; m < 2; m++) {
        multimapIterator iter;
        multimapIteratorInit(maps[m], &iter, true);

        databox e[ELEMENTS_PER_EVENT];
        while (multimapIteratorNextInto(&iter, e, ELEMENTS_PER_EVENT)) {
            const multiTimerId id = e[3].data.u64;
            if (id <= t->stoppedUpTo) {
                liveStops++;
                continue;
            }

            size_t lo = 0;
            size_t hi = stopCount;
            while (lo < hi) {
                const size_t mid = (lo + hi) / 2;
                if (stopIds[mid] < id) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }

            if (lo < stopCount && stopIds[lo] == id && !stopSeen[lo]) {
                stopSeen[lo] = true;
                liveStops++;
            }
        }
    }

    for (size_t i = 0; i < stopCount; i++) {
        if (!stopSeen[i]) {
            const databox stop = databoxNewUnsigned(stopIds[i]);
            multimapDelete(&t->stopEvents, &stop);
        }
    }

    if (stopCount) {
        stopBoundsAfterRemoval(t);
    }

    t->stopPending = liveStops;
}

/* Callback time events */
void multiTimerProcessTimerEvents(multiTimer *t) {
    /* Callbacks must not re-enter processing: the scheduled map being
     * iterated would be walked (and reset) twice. Deferred work is
     * picked up by the in-progress outer call. */
    if (t->context == MULTI_TIMER_CONTEXT_TIMER) {
        DK_DIAG(DK_DIAG_TIMER_REENTERED,
                "called from a timer callback; the running call processes "
                "the rest");
        return;
    }

    multiTimerAdjustedUs now = adjustedNowUs(t);

    const multimapPredicate predicateTimer = {
        .condition = MULTIMAP_CONDITION_LESS_THAN_EQUAL,
        .compareAgainst = databoxNewUnsigned(now)};

    t->context = MULTI_TIMER_CONTEXT_TIMER;
    size_t processed = multimapProcessUntil(t->scheduled, &predicateTimer, true,
                                            timerRunner, t);
    t->context = MULTI_TIMER_CONTEXT_USER;

    if (processed) {
        /* delete processed timers but leave future timers */
        cleanupTimersUpTo(t, processed, &predicateTimer);
    }

    /* copy t->pendingScheduling to t->scheduled */
    rescheduleTimers(t);

    /* Once no timers are stored anywhere, every remaining stop entry
     * is stale (a stop of an already-fired id) - purge them - and no
     * stored time depends on the base: it moves to now, so the times the
     * next timers store (offsets from it, as varints) stay small however
     * long the process runs */
    if (multimapCount(t->scheduled) == 0 &&
        multimapCount(t->pendingScheduling) == 0) {
        if (multimapCount(t->stopEvents) > 0) {
            multimapReset(&t->stopEvents);
        }
        t->stopLowest = 0;
        t->stopHighest = 0;
        t->stopPending = 0;
        t->stopsMaybeStale = false;
        t->initialStartTime = timeUtilMonotonicUs();
    } else if (t->stopsMaybeStale) {
        reconcileStops(t);
    }
}

multiTimerSystemMonotonicUs multiTimerNextTimerEventStartUs(multiTimer *t) {
    if (multimapCount(t->scheduled) == 0) {
        return 0;
    }

    databox first[ELEMENTS_PER_EVENT];
    multimapFirstInto(t->scheduled, first, ELEMENTS_PER_EVENT);
    return adjustedToNative(t, first[0].data.u64);
}

multiTimerUs multiTimerNextTimerEventOffsetFromNowUs(multiTimer *t) {
    const multiTimerSystemMonotonicUs next = multiTimerNextTimerEventStartUs(t);
    if (next == 0) {
        return 0;
    }

    return next - timeUtilMonotonicUs();
}

void multiTimerGetStats(multiTimer *t, multiTimerStats *stats) {
    /* multiTimerCount reconciles stale stop accounting first (outside
     * callbacks), so stopPending is read after it */
    stats->activeCount = multiTimerCount(t);
    stats->scheduledCount = multimapCount(t->scheduled);
    stats->pendingCount = multimapCount(t->pendingScheduling);
    stats->stopPending = t->stopPending;
    stats->memoryBytes = sizeof(*t) + multimapBytes(t->scheduled) +
                         multimapBytes(t->pendingScheduling) +
                         multimapBytes(t->stopEvents);
}

multiTimerSystemMonotonicUs multiTimerGetNs(void) {
    return (multiTimerSystemMonotonicUs)timeUtilMonotonicNs();
}

multiTimerSystemMonotonicUs multiTimerGetUs(void) {
    return (multiTimerSystemMonotonicUs)timeUtilMonotonicUs();
}
