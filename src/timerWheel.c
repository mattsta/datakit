/**
 * timerWheel - High-Performance Hierarchical Timing Wheel Implementation
 *
 * Uses datakit's flex for memory-efficient slot storage with excellent
 * cache locality for frequently accessed near-term timers.
 */

#include "timerWheel.h"
#include "timerWheelInternal.h"

#include "databox.h"
#include "intsetU32.h"
#include "timeUtil.h"

#include <string.h>

/* ====================================================================
 * Wheel Configuration
 * ==================================================================== */

/* Wheel 0: Fine granularity (1ms resolution) */
#define WHEEL0_BITS 8
#define WHEEL0_SIZE (1 << WHEEL0_BITS) /* 256 slots */
#define WHEEL0_MASK (WHEEL0_SIZE - 1)
#define WHEEL0_RESOLUTION_US 1000ULL                        /* 1ms per slot */
#define WHEEL0_SPAN_US (WHEEL0_SIZE * WHEEL0_RESOLUTION_US) /* 256ms */

/* Wheel 1: Medium granularity */
#define WHEEL1_BITS 6
#define WHEEL1_SIZE (1 << WHEEL1_BITS) /* 64 slots */
#define WHEEL1_MASK (WHEEL1_SIZE - 1)
#define WHEEL1_RESOLUTION_US WHEEL0_SPAN_US                 /* 256ms per slot */
#define WHEEL1_SPAN_US (WHEEL1_SIZE * WHEEL1_RESOLUTION_US) /* ~16.4s */

/* Wheel 2: Coarse granularity */
#define WHEEL2_BITS 6
#define WHEEL2_SIZE (1 << WHEEL2_BITS) /* 64 slots */
#define WHEEL2_MASK (WHEEL2_SIZE - 1)
#define WHEEL2_RESOLUTION_US WHEEL1_SPAN_US /* ~16.4s per slot */
#define WHEEL2_SPAN_US (WHEEL2_SIZE * WHEEL2_RESOLUTION_US) /* ~17.5min */

/* Wheel 3: Very coarse granularity */
#define WHEEL3_BITS 6
#define WHEEL3_SIZE (1 << WHEEL3_BITS) /* 64 slots */
#define WHEEL3_MASK (WHEEL3_SIZE - 1)
#define WHEEL3_RESOLUTION_US WHEEL2_SPAN_US /* ~17.5min per slot */
#define WHEEL3_SPAN_US (WHEEL3_SIZE * WHEEL3_RESOLUTION_US) /* ~18.6h */

#define NUM_WHEELS 4
#define ELEMENTS_PER_TIMER 5

/* Total wheel coverage in microseconds (~18.6 hours) */
#define MAX_WHEEL_COVERAGE_US WHEEL3_SPAN_US

/* ====================================================================
 * Internal Data Structures
 * ==================================================================== */

struct timerWheel {
    /* Current time tracking */
    uint64_t currentTimeUs;    /* Current wheel time (adjusted) */
    uint64_t initialStartTime; /* Real monotonic start time */

    /* Current slot indices for each wheel */
    uint32_t slotIndex[NUM_WHEELS];

    /* Wheel slots - each slot is a flex containing timer entries */
    flex *wheel0[WHEEL0_SIZE];
    flex *wheel1[WHEEL1_SIZE];
    flex *wheel2[WHEEL2_SIZE];
    flex *wheel3[WHEEL3_SIZE];

    /* Overflow for very long timers (> 18.6 hours) */
    multimap *overflow;

    /* Cancelled timer tracking using intset for O(log n) lookup */
    intsetU32 *cancelledTimers;
    timerWheelId cancelLowest;
    timerWheelId cancelHighest;

    /* Pending timers (scheduled from within callbacks) */
    flex *pendingTimers;

    /* Timer ID generation */
    timerWheelId nextTimerId;

    /* Context tracking (0 = user context, 1 = timer callback context) */
    int context;

    /* Statistics */
    timerWheelStats stats;

    /* Total timer count (approximate) */
    size_t timerCount;

    /* Cached next expiry (for fast lookup) */
    uint64_t cachedNextExpiry;
    bool nextExpiryCacheValid;
};

/* ====================================================================
 * Utility Functions
 * ==================================================================== */

static inline uint64_t adjustedNowUs(const timerWheel *tw) {
    return timeUtilMonotonicUs() - tw->initialStartTime;
}

static inline uint64_t adjustedToAbsolute(const timerWheel *tw,
                                          uint64_t adjusted) {
    return adjusted + tw->initialStartTime;
}

/* Determine which wheel a timer belongs to based on delay */
static inline int32_t getWheelLevel(uint64_t delay) {
    if (delay < WHEEL0_SPAN_US) {
        return 0;
    } else if (delay < WHEEL1_SPAN_US) {
        return 1;
    } else if (delay < WHEEL2_SPAN_US) {
        return 2;
    } else if (delay < WHEEL3_SPAN_US) {
        return 3;
    } else {
        return -1; /* Overflow */
    }
}

/* Get slot index within a wheel for a given absolute time */
static inline uint32_t getSlotIndex(timerWheel *tw, int32_t level,
                                    uint64_t expireTimeUs) {
    uint64_t timeDiff = expireTimeUs - tw->currentTimeUs;

    switch (level) {
    case 0:
        return (tw->slotIndex[0] + (timeDiff / WHEEL0_RESOLUTION_US)) &
               WHEEL0_MASK;
    case 1:
        return (tw->slotIndex[1] + (timeDiff / WHEEL1_RESOLUTION_US)) &
               WHEEL1_MASK;
    case 2:
        return (tw->slotIndex[2] + (timeDiff / WHEEL2_RESOLUTION_US)) &
               WHEEL2_MASK;
    case 3:
        return (tw->slotIndex[3] + (timeDiff / WHEEL3_RESOLUTION_US)) &
               WHEEL3_MASK;
    default:
        return 0;
    }
}

/* Get pointer to slot flex for a wheel level and index */
static inline flex **getSlot(timerWheel *tw, int32_t level, uint32_t idx) {
    switch (level) {
    case 0:
        return &tw->wheel0[idx];
    case 1:
        return &tw->wheel1[idx];
    case 2:
        return &tw->wheel2[idx];
    case 3:
        return &tw->wheel3[idx];
    default:
        return NULL;
    }
}

/* Get wheel size for a level */
static inline uint32_t getWheelSize(int32_t level) {
    switch (level) {
    case 0:
        return WHEEL0_SIZE;
    case 1:
        return WHEEL1_SIZE;
    case 2:
        return WHEEL2_SIZE;
    case 3:
        return WHEEL3_SIZE;
    default:
        return 0;
    }
}

/* ====================================================================
 * Timer Entry Operations
 * ==================================================================== */

#define boxUnsigned64(us) {.type = DATABOX_UNSIGNED_64, .data.u64 = (us)}
#define boxPtr(ptr)                                                            \
    {.type = DATABOX_UNSIGNED_64, .data.u64 = ((uintptr_t)(ptr))}

/* Insert a timer entry into a slot's flex */
static void insertTimerIntoSlot(flex **slot, uint64_t expireTimeUs,
                                timerWheelCallback *cb, void *clientData,
                                timerWheelId id, uint64_t repeatIntervalUs) {
    if (*slot == NULL) {
        *slot = flexNew();
    }

    const databox expireBox = boxUnsigned64(expireTimeUs);
    const databox cbBox = boxPtr(cb);
    const databox dataBox = boxPtr(clientData);
    const databox idBox = boxUnsigned64(id);
    const databox repeatBox = boxUnsigned64(repeatIntervalUs);

    /* Append all 5 elements to end of flex (unsorted within slot) */
    flexPushByType(slot, &expireBox, FLEX_ENDPOINT_TAIL);
    flexPushByType(slot, &cbBox, FLEX_ENDPOINT_TAIL);
    flexPushByType(slot, &dataBox, FLEX_ENDPOINT_TAIL);
    flexPushByType(slot, &idBox, FLEX_ENDPOINT_TAIL);
    flexPushByType(slot, &repeatBox, FLEX_ENDPOINT_TAIL);
}

/* Insert timer into overflow multimap (sorted by expiry time) */
static void insertTimerIntoOverflow(timerWheel *tw, uint64_t expireTimeUs,
                                    timerWheelCallback *cb, void *clientData,
                                    timerWheelId id,
                                    uint64_t repeatIntervalUs) {
    const databox expireBox = boxUnsigned64(expireTimeUs);
    const databox cbBox = boxPtr(cb);
    const databox dataBox = boxPtr(clientData);
    const databox idBox = boxUnsigned64(id);
    const databox repeatBox = boxUnsigned64(repeatIntervalUs);

    const databox *entry[ELEMENTS_PER_TIMER] = {&expireBox, &cbBox, &dataBox,
                                                &idBox, &repeatBox};

    multimapInsert(&tw->overflow, entry);
    tw->stats.overflowCount++;
}

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

bool timerWheelInit(timerWheel *tw) {
    memset(tw, 0, sizeof(*tw));

    tw->context = TIMER_WHEEL_CONTEXT_USER;
    tw->initialStartTime = timeUtilMonotonicUs();
    tw->currentTimeUs = 0;

    /* Initialize all wheel slots to NULL (allocated lazily) */
    memset(tw->wheel0, 0, sizeof(tw->wheel0));
    memset(tw->wheel1, 0, sizeof(tw->wheel1));
    memset(tw->wheel2, 0, sizeof(tw->wheel2));
    memset(tw->wheel3, 0, sizeof(tw->wheel3));

    /* Initialize overflow multimap for very long timers */
    tw->overflow = multimapNew(ELEMENTS_PER_TIMER);
    if (!tw->overflow) {
        return false;
    }

    /* Initialize cancelled timer set */
    tw->cancelledTimers = intsetU32New();
    if (!tw->cancelledTimers) {
        multimapFree(tw->overflow);
        return false;
    }

    /* Initialize pending timers flex */
    tw->pendingTimers = flexNew();
    if (!tw->pendingTimers) {
        multimapFree(tw->overflow);
        intsetU32Free(tw->cancelledTimers);
        return false;
    }

    return true;
}

void timerWheelDeinit(timerWheel *tw) {
    if (!tw) {
        return;
    }

    /* Free all wheel slots */
    for (uint32_t i = 0; i < WHEEL0_SIZE; i++) {
        if (tw->wheel0[i]) {
            flexFree(tw->wheel0[i]);
        }
    }
    for (uint32_t i = 0; i < WHEEL1_SIZE; i++) {
        if (tw->wheel1[i]) {
            flexFree(tw->wheel1[i]);
        }
    }
    for (uint32_t i = 0; i < WHEEL2_SIZE; i++) {
        if (tw->wheel2[i]) {
            flexFree(tw->wheel2[i]);
        }
    }
    for (uint32_t i = 0; i < WHEEL3_SIZE; i++) {
        if (tw->wheel3[i]) {
            flexFree(tw->wheel3[i]);
        }
    }

    multimapFree(tw->overflow);
    intsetU32Free(tw->cancelledTimers);
    flexFree(tw->pendingTimers);

    memset(tw, 0, sizeof(*tw));
}

timerWheel *timerWheelNew(void) {
    timerWheel *tw = zcalloc(1, sizeof(*tw));
    if (!tw) {
        return NULL;
    }

    if (!timerWheelInit(tw)) {
        zfree(tw);
        return NULL;
    }

    return tw;
}

void timerWheelFree(timerWheel *tw) {
    if (tw) {
        timerWheelDeinit(tw);
        zfree(tw);
    }
}

/* ====================================================================
 * Timer Management
 * ==================================================================== */

timerWheelId timerWheelRegister(timerWheel *tw, uint64_t startAfterMicroseconds,
                                uint64_t repeatEveryMicroseconds,
                                timerWheelCallback *cb, void *clientData) {
    timerWheelId id = ++tw->nextTimerId;
    uint64_t now = adjustedNowUs(tw);
    uint64_t expireTimeUs = now + startAfterMicroseconds;

    tw->stats.totalRegistrations++;
    tw->timerCount++;
    tw->nextExpiryCacheValid = false;

    /* If we're in a timer callback, defer to pending */
    if (tw->context == TIMER_WHEEL_CONTEXT_TIMER) {
        insertTimerIntoSlot(&tw->pendingTimers, expireTimeUs, cb, clientData,
                            id, repeatEveryMicroseconds);
        return id;
    }

    /* Zero-delay timers go to pending to ensure they fire on next process */
    if (startAfterMicroseconds == 0) {
        insertTimerIntoSlot(&tw->pendingTimers, expireTimeUs, cb, clientData,
                            id, repeatEveryMicroseconds);
        return id;
    }

    /* Determine wheel level based on delay */
    uint64_t delay = startAfterMicroseconds;
    int32_t level = getWheelLevel(delay);

    if (level >= 0) {
        /* Insert into appropriate wheel slot */
        uint32_t slotIdx = getSlotIndex(tw, level, expireTimeUs);
        flex **slot = getSlot(tw, level, slotIdx);
        insertTimerIntoSlot(slot, expireTimeUs, cb, clientData, id,
                            repeatEveryMicroseconds);
    } else {
        /* Very long timer - use overflow */
        insertTimerIntoOverflow(tw, expireTimeUs, cb, clientData, id,
                                repeatEveryMicroseconds);
    }

    return id;
}

bool timerWheelUnregister(timerWheel *tw, timerWheelId id) {
    if (id == 0 || id > tw->nextTimerId) {
        return false;
    }

    /* Add to cancelled set */
    bool success = intsetU32Add(&tw->cancelledTimers, (uint32_t)id);

    if (success) {
        tw->stats.totalCancellations++;

        /* Update bounds for efficient range checking */
        if (intsetU32Count(tw->cancelledTimers) == 1) {
            tw->cancelLowest = id;
            tw->cancelHighest = id;
        } else {
            if (id < tw->cancelLowest) {
                tw->cancelLowest = id;
            }
            if (id > tw->cancelHighest) {
                tw->cancelHighest = id;
            }
        }
    }

    return true;
}

bool timerWheelStopAll(timerWheel *tw) {
    for (timerWheelId i = 1; i <= tw->nextTimerId; i++) {
        timerWheelUnregister(tw, i);
    }
    return true;
}

size_t timerWheelCount(const timerWheel *tw) {
    size_t cancelled = intsetU32Count(tw->cancelledTimers);
    return tw->timerCount > cancelled ? tw->timerCount - cancelled : 0;
}

/* ====================================================================
 * Timer Cancellation Check
 * ==================================================================== */

static bool isTimerCancelled(const timerWheel *tw, timerWheelId id) {
    if (intsetU32Count(tw->cancelledTimers) == 0) {
        return false;
    }

    /* Quick bounds check */
    if (id < tw->cancelLowest || id > tw->cancelHighest) {
        return false;
    }

    return intsetU32Exists(tw->cancelledTimers, (uint32_t)id);
}

static void removeCancelledTimer(timerWheel *tw, timerWheelId id) {
    bool success = intsetU32Remove(&tw->cancelledTimers, (uint32_t)id);

    size_t count = intsetU32Count(tw->cancelledTimers);
    if (success && count > 0) {
        /* Update bounds - get new min/max from intset */
        uint32_t newLow, newHigh;
        intsetU32Get(tw->cancelledTimers, 0, &newLow);
        intsetU32Get(tw->cancelledTimers, count - 1, &newHigh);
        tw->cancelLowest = newLow;
        tw->cancelHighest = newHigh;
    } else if (count == 0) {
        tw->cancelLowest = 0;
        tw->cancelHighest = 0;
    }
}

/* ====================================================================
 * Timer Execution
 * ==================================================================== */

/* Process all timers in a slot's flex */
static void processSlot(timerWheel *tw, flex **slot, uint64_t currentTime) {
    if (*slot == NULL || flexCount(*slot) == 0) {
        return;
    }

    const flex *f = *slot;
    flexEntry *fe = flexHead(f);
    size_t count = flexCount(f) / ELEMENTS_PER_TIMER;

    /* Process all timer entries in this slot */
    for (size_t i = 0; i < count; i++) {
        databox boxes[ELEMENTS_PER_TIMER];
        flexEntry *current = fe;

        /* Read all 5 elements of this timer entry */
        for (int j = 0; j < ELEMENTS_PER_TIMER; j++) {
            flexGetByType(fe, &boxes[j]);
            fe = flexNext(f, fe);
        }

        uint64_t expireTimeUs = boxes[0].data.u64;
        timerWheelCallback *cb =
            (timerWheelCallback *)(uintptr_t)boxes[1].data.u64;
        void *clientData = (void *)(uintptr_t)boxes[2].data.u64;
        timerWheelId id = boxes[3].data.u64;
        uint64_t repeatIntervalUs = boxes[4].data.u64;

        /* Check if timer should fire now */
        if (expireTimeUs <= currentTime) {
            /* Check if cancelled */
            if (isTimerCancelled(tw, id)) {
                removeCancelledTimer(tw, id);
                tw->timerCount--;
                continue;
            }

            /* Execute callback */
            tw->context = TIMER_WHEEL_CONTEXT_TIMER;
            bool reschedule = cb(tw, id, clientData);
            tw->context = TIMER_WHEEL_CONTEXT_USER;

            tw->stats.totalExpirations++;
            tw->timerCount--;

            /* Handle rescheduling */
            if (reschedule && repeatIntervalUs > 0) {
                /* Schedule relative to wheel's current position (after this
                 * slot), NOT the final target time. This ensures repeating
                 * timers fire at each interval as the wheel advances, rather
                 * than jumping to the final target and missing intermediate
                 * fires. */
                uint64_t slotEndTime = tw->currentTimeUs + WHEEL0_RESOLUTION_US;
                uint64_t newExpireTime = slotEndTime + repeatIntervalUs;

                /* Sub-resolution timers go to pending for prompt fire */
                if (repeatIntervalUs < WHEEL0_RESOLUTION_US) {
                    insertTimerIntoSlot(&tw->pendingTimers, newExpireTime, cb,
                                        clientData, id, repeatIntervalUs);
                    tw->timerCount++;
                } else {
                    /* Place in appropriate wheel slot relative to where
                     * we ARE now, not where we'll END UP */
                    uint64_t delay = repeatIntervalUs;
                    int32_t level = getWheelLevel(delay);

                    if (level >= 0) {
                        uint32_t slotIdx =
                            getSlotIndex(tw, level, newExpireTime);
                        flex **newSlot = getSlot(tw, level, slotIdx);
                        insertTimerIntoSlot(newSlot, newExpireTime, cb,
                                            clientData, id, repeatIntervalUs);
                        tw->timerCount++;
                    } else {
                        insertTimerIntoOverflow(tw, newExpireTime, cb,
                                                clientData, id,
                                                repeatIntervalUs);
                        tw->timerCount++;
                    }
                }
            }
        } else {
            /* Timer not ready yet - re-insert into appropriate location */
            uint64_t delay = expireTimeUs - currentTime;

            /* Sub-resolution delays stay in pending for prompt firing */
            if (delay < WHEEL0_RESOLUTION_US) {
                insertTimerIntoSlot(&tw->pendingTimers, expireTimeUs, cb,
                                    clientData, id, repeatIntervalUs);
            } else {
                int32_t level = getWheelLevel(delay);

                if (level >= 0) {
                    uint32_t slotIdx = getSlotIndex(tw, level, expireTimeUs);
                    flex **newSlot = getSlot(tw, level, slotIdx);
                    if (newSlot != slot) {
                        insertTimerIntoSlot(newSlot, expireTimeUs, cb,
                                            clientData, id, repeatIntervalUs);
                    } else {
                        /* Same slot - will be processed on next tick */
                        insertTimerIntoSlot(&tw->pendingTimers, expireTimeUs,
                                            cb, clientData, id,
                                            repeatIntervalUs);
                    }
                } else {
                    insertTimerIntoOverflow(tw, expireTimeUs, cb, clientData,
                                            id, repeatIntervalUs);
                }
            }
        }

        (void)current; /* Silence unused warning */
    }

    /* Clear the processed slot */
    flexFree(*slot);
    *slot = NULL;
}

/* Cascade timers from higher wheel to lower wheels */
static void cascadeWheel(timerWheel *tw, int32_t level) {
    if (level <= 0 || level >= NUM_WHEELS) {
        return;
    }

    uint32_t slotIdx = tw->slotIndex[level];
    flex **slot = getSlot(tw, level, slotIdx);

    if (*slot == NULL || flexCount(*slot) == 0) {
        return;
    }

    tw->stats.totalCascades++;

    const flex *f = *slot;
    flexEntry *fe = flexHead(f);
    size_t count = flexCount(f) / ELEMENTS_PER_TIMER;

    uint64_t currentTime = tw->currentTimeUs;

    /* Move all timers to appropriate lower-level slots */
    for (size_t i = 0; i < count; i++) {
        databox boxes[ELEMENTS_PER_TIMER];

        for (int j = 0; j < ELEMENTS_PER_TIMER; j++) {
            flexGetByType(fe, &boxes[j]);
            fe = flexNext(f, fe);
        }

        uint64_t expireTimeUs = boxes[0].data.u64;
        timerWheelCallback *cb =
            (timerWheelCallback *)(uintptr_t)boxes[1].data.u64;
        void *clientData = (void *)(uintptr_t)boxes[2].data.u64;
        timerWheelId id = boxes[3].data.u64;
        uint64_t repeatIntervalUs = boxes[4].data.u64;

        /* Skip cancelled timers */
        if (isTimerCancelled(tw, id)) {
            removeCancelledTimer(tw, id);
            tw->timerCount--;
            continue;
        }

        /* Determine new wheel level */
        uint64_t delay =
            expireTimeUs > currentTime ? expireTimeUs - currentTime : 0;

        /* Sub-resolution delays go to pending for prompt firing */
        if (delay < WHEEL0_RESOLUTION_US) {
            insertTimerIntoSlot(&tw->pendingTimers, expireTimeUs, cb,
                                clientData, id, repeatIntervalUs);
        } else {
            int32_t newLevel = getWheelLevel(delay);

            if (newLevel >= 0 && newLevel < level) {
                uint32_t newSlotIdx = getSlotIndex(tw, newLevel, expireTimeUs);
                flex **newSlot = getSlot(tw, newLevel, newSlotIdx);
                insertTimerIntoSlot(newSlot, expireTimeUs, cb, clientData, id,
                                    repeatIntervalUs);
            } else if (newLevel == -1) {
                /* Shouldn't happen during cascade, but handle gracefully */
                insertTimerIntoOverflow(tw, expireTimeUs, cb, clientData, id,
                                        repeatIntervalUs);
            } else {
                /* Keep in same level (edge case) */
                insertTimerIntoSlot(&tw->pendingTimers, expireTimeUs, cb,
                                    clientData, id, repeatIntervalUs);
            }
        }
    }

    /* Clear cascaded slot */
    flexFree(*slot);
    *slot = NULL;
}

/* Process overflow timers that are now within wheel range */
static void processOverflow(timerWheel *tw, uint64_t currentTime) {
    if (multimapCount(tw->overflow) == 0) {
        return;
    }

    /* Get first timer from overflow */
    databox boxes[ELEMENTS_PER_TIMER];
    databox *boxPtrs[ELEMENTS_PER_TIMER];
    for (int i = 0; i < ELEMENTS_PER_TIMER; i++) {
        boxPtrs[i] = &boxes[i];
    }

    while (multimapCount(tw->overflow) > 0) {
        multimapFirst(tw->overflow, boxPtrs);

        uint64_t expireTimeUs = boxes[0].data.u64;

        /* Check if this timer is now within wheel range */
        if (expireTimeUs <= currentTime + MAX_WHEEL_COVERAGE_US) {
            timerWheelCallback *cb = (timerWheelCallback *)boxes[1].data.ptr;
            void *clientData = boxes[2].data.ptr;
            timerWheelId id = boxes[3].data.u64;
            uint64_t repeatIntervalUs = boxes[4].data.u64;

            /* Remove from overflow using full-width delete */
            multimapDeleteFullWidth(&tw->overflow, (const databox **)boxPtrs);
            tw->stats.overflowCount--;

            /* Skip if cancelled */
            if (isTimerCancelled(tw, id)) {
                removeCancelledTimer(tw, id);
                tw->timerCount--;
                continue;
            }

            /* Insert into appropriate location */
            uint64_t delay =
                expireTimeUs > currentTime ? expireTimeUs - currentTime : 0;

            /* Sub-resolution delays go to pending for prompt firing */
            if (delay < WHEEL0_RESOLUTION_US) {
                insertTimerIntoSlot(&tw->pendingTimers, expireTimeUs, cb,
                                    clientData, id, repeatIntervalUs);
            } else {
                int32_t level = getWheelLevel(delay);

                if (level >= 0) {
                    uint32_t slotIdx = getSlotIndex(tw, level, expireTimeUs);
                    flex **slot = getSlot(tw, level, slotIdx);
                    insertTimerIntoSlot(slot, expireTimeUs, cb, clientData, id,
                                        repeatIntervalUs);
                } else {
                    /* Shouldn't happen - timer is within range but level is -1
                     */
                    insertTimerIntoSlot(&tw->pendingTimers, expireTimeUs, cb,
                                        clientData, id, repeatIntervalUs);
                }
            }
        } else {
            /* No more overflow timers within range */
            break;
        }
    }
}

/* Process pending timers scheduled during callbacks */
static void processPending(timerWheel *tw) {
    if (flexCount(tw->pendingTimers) == 0) {
        return;
    }

    flex *pending = tw->pendingTimers;
    tw->pendingTimers = flexNew();

    flexEntry *fe = flexHead(pending);
    size_t count = flexCount(pending) / ELEMENTS_PER_TIMER;

    uint64_t now = adjustedNowUs(tw);

    for (size_t i = 0; i < count; i++) {
        databox boxes[ELEMENTS_PER_TIMER];

        for (int j = 0; j < ELEMENTS_PER_TIMER; j++) {
            flexGetByType(fe, &boxes[j]);
            fe = flexNext(pending, fe);
        }

        uint64_t expireTimeUs = boxes[0].data.u64;
        timerWheelCallback *cb =
            (timerWheelCallback *)(uintptr_t)boxes[1].data.u64;
        void *clientData = (void *)(uintptr_t)boxes[2].data.u64;
        timerWheelId id = boxes[3].data.u64;
        uint64_t repeatIntervalUs = boxes[4].data.u64;

        /* Check if timer should fire now (including zero-delay timers) */
        if (expireTimeUs <= now) {
            /* Skip if cancelled */
            if (isTimerCancelled(tw, id)) {
                removeCancelledTimer(tw, id);
                tw->timerCount--;
                continue;
            }

            /* Execute callback */
            tw->context = TIMER_WHEEL_CONTEXT_TIMER;
            bool reschedule = cb(tw, id, clientData);
            tw->context = TIMER_WHEEL_CONTEXT_USER;

            tw->stats.totalExpirations++;
            tw->timerCount--;

            /* Handle rescheduling */
            if (reschedule && repeatIntervalUs > 0) {
                /* Schedule relative to current time (now). processPending
                 * runs after the wheel has advanced to `now`, so we use
                 * `now` directly (unlike processSlot which uses wheel pos). */
                uint64_t newExpireTime = now + repeatIntervalUs;

                /* Sub-resolution timers go to pending for prompt fire */
                if (repeatIntervalUs < WHEEL0_RESOLUTION_US) {
                    insertTimerIntoSlot(&tw->pendingTimers, newExpireTime, cb,
                                        clientData, id, repeatIntervalUs);
                    tw->timerCount++;
                } else {
                    uint64_t delay = repeatIntervalUs;
                    int32_t level = getWheelLevel(delay);

                    if (level >= 0) {
                        uint32_t slotIdx =
                            getSlotIndex(tw, level, newExpireTime);
                        flex **slot = getSlot(tw, level, slotIdx);
                        insertTimerIntoSlot(slot, newExpireTime, cb, clientData,
                                            id, repeatIntervalUs);
                        tw->timerCount++;
                    } else {
                        insertTimerIntoOverflow(tw, newExpireTime, cb,
                                                clientData, id,
                                                repeatIntervalUs);
                        tw->timerCount++;
                    }
                }
            }
        } else {
            /* Timer not due yet - insert into appropriate location */
            uint64_t delay = expireTimeUs - now;

            /* Sub-resolution timers stay in pending for prompt firing */
            if (delay < WHEEL0_RESOLUTION_US) {
                insertTimerIntoSlot(&tw->pendingTimers, expireTimeUs, cb,
                                    clientData, id, repeatIntervalUs);
            } else {
                int32_t level = getWheelLevel(delay);

                if (level >= 0) {
                    uint32_t slotIdx = getSlotIndex(tw, level, expireTimeUs);
                    flex **slot = getSlot(tw, level, slotIdx);
                    insertTimerIntoSlot(slot, expireTimeUs, cb, clientData, id,
                                        repeatIntervalUs);
                } else {
                    insertTimerIntoOverflow(tw, expireTimeUs, cb, clientData,
                                            id, repeatIntervalUs);
                }
            }
        }
    }

    flexFree(pending);
}

/* ====================================================================
 * Timer Processing
 * ==================================================================== */

void timerWheelProcessTimerEvents(timerWheel *tw) {
    uint64_t now = adjustedNowUs(tw);
    tw->nextExpiryCacheValid = false;

    /* Process overflow timers that may have come into range */
    processOverflow(tw, now);

    /* Advance wheel0 to current time, cascading as needed */
    while (tw->currentTimeUs < now) {
        /* Process current wheel0 slot */
        uint32_t slot0Idx = tw->slotIndex[0];

        /* Prefetch next slot for better cache performance */
        uint32_t nextSlot0Idx = (slot0Idx + 1) & WHEEL0_MASK;
        if (tw->wheel0[nextSlot0Idx]) {
            __builtin_prefetch(tw->wheel0[nextSlot0Idx], 0, 1);
        }

        processSlot(tw, &tw->wheel0[slot0Idx], now);

        /* Advance wheel0 */
        tw->slotIndex[0] = nextSlot0Idx;
        tw->currentTimeUs += WHEEL0_RESOLUTION_US;

        /* Check for wheel wrap and cascade */
        if (nextSlot0Idx == 0) {
            /* Wheel 0 wrapped - advance wheel 1 */
            uint32_t nextSlot1Idx = (tw->slotIndex[1] + 1) & WHEEL1_MASK;

            /* Cascade timers from wheel 1 to wheel 0 */
            cascadeWheel(tw, 1);
            tw->slotIndex[1] = nextSlot1Idx;

            if (nextSlot1Idx == 0) {
                /* Wheel 1 wrapped - advance wheel 2 */
                uint32_t nextSlot2Idx = (tw->slotIndex[2] + 1) & WHEEL2_MASK;
                cascadeWheel(tw, 2);
                tw->slotIndex[2] = nextSlot2Idx;

                if (nextSlot2Idx == 0) {
                    /* Wheel 2 wrapped - advance wheel 3 */
                    uint32_t nextSlot3Idx =
                        (tw->slotIndex[3] + 1) & WHEEL3_MASK;
                    cascadeWheel(tw, 3);
                    tw->slotIndex[3] = nextSlot3Idx;
                }
            }
        }
    }

    /* Process any timers scheduled during callbacks */
    processPending(tw);
}

void timerWheelAdvanceTime(timerWheel *tw, uint64_t microseconds) {
    /* For testing: manually advance time */
    tw->initialStartTime -= microseconds;
    timerWheelProcessTimerEvents(tw);
}

/* ====================================================================
 * Timer Queries
 * ==================================================================== */

timerWheelSystemMonotonicUs timerWheelNextTimerEventStartUs(timerWheel *tw) {
    if (tw->nextExpiryCacheValid) {
        return (timerWheelSystemMonotonicUs)adjustedToAbsolute(
            tw, tw->cachedNextExpiry);
    }

    uint64_t earliest = UINT64_MAX;

    /* Check pending timers first - they fire immediately */
    if (tw->pendingTimers && flexCount(tw->pendingTimers) > 0) {
        databox box;
        flexGetByType(flexHead(tw->pendingTimers), &box);
        earliest = box.data.u64;
    }

    /* Check wheel 0 first (most likely to have nearest timer) */
    for (uint32_t i = 0; i < WHEEL0_SIZE; i++) {
        uint32_t idx = (tw->slotIndex[0] + i) & WHEEL0_MASK;
        if (tw->wheel0[idx] && flexCount(tw->wheel0[idx]) > 0) {
            /* Get first timer in this slot */
            databox box;
            flexGetByType(flexHead(tw->wheel0[idx]), &box);
            if (box.data.u64 < earliest) {
                earliest = box.data.u64;
            }
            break; /* Found nearest in wheel 0 */
        }
    }

    /* If nothing in wheel 0, check higher wheels */
    if (earliest == UINT64_MAX) {
        for (int32_t level = 1; level < NUM_WHEELS; level++) {
            uint32_t size = getWheelSize(level);
            for (uint32_t i = 0; i < size; i++) {
                uint32_t idx = (tw->slotIndex[level] + i) & (size - 1);
                flex **slot = getSlot(tw, level, idx);
                if (*slot && flexCount(*slot) > 0) {
                    databox box;
                    flexGetByType(flexHead(*slot), &box);
                    if (box.data.u64 < earliest) {
                        earliest = box.data.u64;
                    }
                    goto foundInWheel;
                }
            }
        }
    }

foundInWheel:
    /* Check overflow */
    if (multimapCount(tw->overflow) > 0) {
        databox box;
        databox *boxPtr = &box;
        multimapFirst(tw->overflow, &boxPtr);
        if (box.data.u64 < earliest) {
            earliest = box.data.u64;
        }
    }

    if (earliest == UINT64_MAX) {
        return 0;
    }

    tw->cachedNextExpiry = earliest;
    tw->nextExpiryCacheValid = true;

    return (timerWheelSystemMonotonicUs)adjustedToAbsolute(tw, earliest);
}

timerWheelUs timerWheelNextTimerEventOffsetFromNowUs(timerWheel *tw) {
    timerWheelSystemMonotonicUs next = timerWheelNextTimerEventStartUs(tw);
    if (next == 0) {
        return 0;
    }
    return next - (timerWheelSystemMonotonicUs)timeUtilMonotonicUs();
}

/* ====================================================================
 * Statistics
 * ==================================================================== */

void timerWheelGetStats(const timerWheel *tw, timerWheelStats *stats) {
    *stats = tw->stats;

    /* Calculate memory usage */
    stats->memoryBytes = sizeof(timerWheel);

    for (uint32_t i = 0; i < WHEEL0_SIZE; i++) {
        if (tw->wheel0[i]) {
            stats->memoryBytes += flexBytes(tw->wheel0[i]);
        }
    }
    for (uint32_t i = 0; i < WHEEL1_SIZE; i++) {
        if (tw->wheel1[i]) {
            stats->memoryBytes += flexBytes(tw->wheel1[i]);
        }
    }
    for (uint32_t i = 0; i < WHEEL2_SIZE; i++) {
        if (tw->wheel2[i]) {
            stats->memoryBytes += flexBytes(tw->wheel2[i]);
        }
    }
    for (uint32_t i = 0; i < WHEEL3_SIZE; i++) {
        if (tw->wheel3[i]) {
            stats->memoryBytes += flexBytes(tw->wheel3[i]);
        }
    }

    stats->memoryBytes += multimapBytes(tw->overflow);
    stats->memoryBytes += intsetU32Bytes(tw->cancelledTimers);
    stats->memoryBytes += flexBytes(tw->pendingTimers);

    stats->overflowCount = multimapCount(tw->overflow);
}

void timerWheelResetStats(timerWheel *tw) {
    tw->stats.totalRegistrations = 0;
    tw->stats.totalCancellations = 0;
    tw->stats.totalExpirations = 0;
    tw->stats.totalCascades = 0;
    /* Don't reset overflowCount - it's a current state, not a counter */
}
