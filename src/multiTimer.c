#include "multiTimer.h"
#include "multiTimerInternal.h"

#include "timeUtil.h"

typedef uint64_t multiTimerAdjustedUs;

/* Time event structure */
/* timer events are a 5-element map entry */
/* This struct is __documentation only__,
 * it's not actually used anywhere, but this *is* the
 * layout of our 5-tuple multimap entries. */
typedef struct multiTimerEvent {
    uint64_t runAtMicroseconds;
    multiTimerCallback *cb;
    void *clientData;
    multiTimerId id;
    uint64_t repeatIntervalMicroseconds;
} multiTimerEvent;

bool multiTimerInit(multiTimer *t) {
    t->context = MULTI_TIMER_CONTEXT_USER;
    t->timersInclusiveOfTimerRuntime = false;

    t->scheduled = multimapNew(5);
    t->pendingScheduling = multimapNew(5);
    t->stopEvents = multimapNew(1);

    if (!t->scheduled || !t->pendingScheduling || !t->stopEvents) {
        multiTimerDeinit(t);
        return false;
    }

    /* TODO: if we have no active timers, we can re-base our start time
     *       during timer maintenance to a newer time so our timer offsets
     *       are smaller as they march ever onward into the future */
    t->initialStartTime = timeUtilMonotonicUs();
    return true;
}

void multiTimerDeinit(multiTimer *t) {
    if (t) {
        multimapFree(t->scheduled);
        multimapFree(t->pendingScheduling);
        multimapFree(t->stopEvents);
        *t = (multiTimer){0};
    }
}

size_t multitimerCount(multiTimer *t) {
    return multimapCount(t->scheduled);
}

void multiTimerFree(multiTimer *t) {
    if (t) {
        multiTimerDeinit(t);
        zfree(t);
    }
}

multiTimer *multiTimerNew(void) {
    multiTimer *t = zcalloc(1, sizeof(*t));

    if (!multiTimerInit(t)) {
        multiTimerFree(t);
        return NULL;
    }

    return t;
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

#define boxUnsigned64(us)                                                      \
    { .type = DATABOX_UNSIGNED_64, .data.u64 = (us) }
#define boxPtr(ptr)                                                            \
    { .type = DATABOX_UNSIGNED_64, .data.u64 = ((uintptr_t)ptr) }
multiTimerId multiTimerRegister(multiTimer *t, uint64_t startAfterMicroseconds,
                                uint64_t repeatEveryMicroseconds,
                                multiTimerCallback *cb, void *clientData) {
    multiTimerId id = ++t->nextTimerId;

    const multiTimerAdjustedUs startAtMicroseconds =
        adjustedNowUs(t) + startAfterMicroseconds;
    const databox startAt = boxUnsigned64(startAtMicroseconds);
    const databox callback = boxPtr(cb);
    const databox privdata = boxPtr(clientData);
    const databox tid = boxUnsigned64(id);
    const databox repeat = boxUnsigned64(repeatEveryMicroseconds);

    const databox *timerEntry[5] = {&startAt, &callback, &privdata, &tid,
                                    &repeat};

    if (t->context == MULTI_TIMER_CONTEXT_TIMER) {
        /* We can't modify timers while we're executing a timer,
         * so creating a timer while in a timer means we
         * insert into the to-schedule-timers map. */
        multimapInsert(&t->pendingScheduling, timerEntry);
    } else {
        multimapInsert(&t->scheduled, timerEntry);
    }

    return id;
}

bool multiTimerUnregister(multiTimer *t, multiTimerId id) {
    const databox stop = boxUnsigned64(id);
    const databox *stopTimer[1] = {&stop};

    multimapInsert(&t->stopEvents, stopTimer);

    return true;
}

bool multiTimerStopAll(multiTimer *t) {
    for (size_t i = 1; i < t->nextTimerId; i++) {
        multiTimerUnregister(t, i);
    }

    return true;
}

static bool timerCopyRunner(void *userData, const databox *elements[]) {
    multiTimer *t = userData;
    multimapInsert(&t->scheduled, elements);
    return true;
}

static bool checkTimerExceptions(multiTimer *t, const databox *timerId) {
    const multiTimerId tid = timerId->data.u64;

    /* If 'timerId' is in the range of timers requested for deletion,
     * then delete timer from timer deletion map and update endpoints
     * for deleted timer ranges. */
    if (tid <= t->stopHighest && tid >= t->stopLowest) {
        if (multimapExists(t->stopEvents, timerId)) {
            databox high;
            databox *highs[1] = {&high};

            databox low;
            databox *lows[1] = {&low};

            /* Remove from times to stop since we're not running it. */
            multimapDelete(&t->stopEvents, timerId);

            /* New Highest */
            multimapLast(t->stopEvents, highs);

            /* New Lowest */
            multimapFirst(t->stopEvents, lows);

            t->stopHighest = high.data.u64;
            t->stopLowest = low.data.u64;
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
        multimapReset(t->scheduled);
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

            multimapReset(t->pendingScheduling);
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

    const databox *runAt = elements[0];
    const databox *callback = elements[1];
    const databox *callbackState = elements[2];
    const databox *localTimerId = elements[3];
    const databox *repeatInterval = elements[4];

    const multiTimerAdjustedUs runAtUs = runAt->data.u64;
    const uint64_t repeatIntervalUs = repeatInterval->data.u64;

    multiTimerCallback *timerCallback =
        (multiTimerCallback *)callback->data.ptr;
    void *timerCallbackState = callbackState->data.ptr;
    const multiTimerId tid = localTimerId->data.u64;

    /* If this timer is within the range of potential timers to stop,
     * check if we really should ignore this timer */
    if (checkTimerExceptions(t, localTimerId)) {
        /* Skip this timer, but continue processing future timers */
        return true;
    }

    const bool reschedule = timerCallback(t, tid, timerCallbackState);

    if (reschedule) {
        /* If timer event has a repeat interval, copy timer event into
         * the future. */
        if (repeatIntervalUs) {
            /* If 'timersInclusiveOfTimerRuntime', then we just boost the
             * current run offset of timer by 'repeatIntervalUs';
             * else, calculate new timer start based on now()+repeat. */

            /* TODO: if this gets too far behind, what do we do?
             * If computer sleeps for an hour, do we end up running a million
             * additions to catch up to current time? */
            /* We could actually have three run modes:
             *   - add repeat to currently scheduled time
             *   - add repeat to intitial run time of this timer run
             *   - add repeat to time after timer event finished. */
            const multiTimerAdjustedUs newStartBaseUs =
                t->timersInclusiveOfTimerRuntime ? runAtUs : adjustedNowUs(t);

            /* Calculate new scheduled start time... */
            const databox newStart =
                boxUnsigned64(newStartBaseUs + repeatIntervalUs);

            const databox *scheduleTimer[5] = {
                &newStart, elements[1], elements[2], elements[3], elements[4]};

            multimapInsert(&t->pendingScheduling, scheduleTimer);
        }
    }

    /* Continue processing future timers */
    return true;
}

/* Callback time events */
void multiTimerProcessTimerEvents(multiTimer *t) {
    multiTimerAdjustedUs now = adjustedNowUs(t);

    const multimapPredicate predicateTimer = {
        .condition = MULTIMAP_CONDITION_LESS_THAN_EQUAL,
        .compareAgainst = boxUnsigned64(now)};

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
}

multiTimerSystemMonotonicUs multiTimerNextTimerEventStartUs(multiTimer *t) {
    if (multimapCount(t->scheduled) == 0) {
        return 0;
    }

    databox a = {{0}}, b, c, d, e;
    databox *elements[5] = {&a, &b, &c, &d, &e};
    multimapFirst(t->scheduled, elements);
    return adjustedToNative(t, a.data.u64);
}

multiTimerUs multiTimerNextTimerEventOffsetFromNowUs(multiTimer *t) {
    return multiTimerNextTimerEventStartUs(t) - timeUtilMonotonicUs();
}
