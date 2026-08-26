/**
 * timerWheel - High-Performance Hierarchical Timing Wheel Implementation
 *
 * Uses datakit's flex for memory-efficient slot storage with excellent
 * cache locality for frequently accessed near-term timers.
 */

#include "timerWheel.h"
#include "timerWheelInternal.h"

#include "databox.h"
#include "flex.h"
#include "intset.h"
#include "multimap.h"
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

/* getWheelLevel() result for delays beyond all wheel spans */
#define WHEEL_LEVEL_OVERFLOW (-1)

/* Total wheel coverage in microseconds (~18.6 hours) */
#define MAX_WHEEL_COVERAGE_US WHEEL3_SPAN_US

/* ====================================================================
 * Internal Data Structures
 * ==================================================================== */

/* Fields are ordered by access temperature so the hot state is
 * contiguous at the front (the first 64 bytes are the process-loop
 * core; the next line holds the register/fire scalars) instead of
 * scattered behind 3.5KB of slot-pointer arrays. Order WITHIN a line
 * is chosen for readability - it has no cache effect. */
struct timerWheel {
    /* ---- Process-loop core: exactly one cache line ---- */

    timerWheelAdjustedUs currentTimeUs; /* Current wheel time */
    uint64_t initialStartTime;          /* Real monotonic start time */

    /* Current slot indices for each wheel.
     * Invariant: slotIndex[k] is a pure function of the tick count
     * (currentTimeUs / WHEEL0_RESOLUTION_US) — the wheels advance as a
     * base-(256,64,64,64) odometer over ticks. */
    uint32_t slotIndex[NUM_WHEELS];

    /* One bit per wheel0 slot; set iff the slot flex is allocated.
     * Lets the catch-up loop hop empty spans in O(1) instead of
     * ticking through them 1ms at a time after idle gaps. */
    uint64_t wheel0Occupied[WHEEL0_SIZE / 64];

    /* ---- Touched by every register and every fire ---- */

    /* Timer ID generation */
    timerWheelId nextTimerId;

    /* Timer entries physically present in slots/pending/overflow,
     * including cancelled entries not yet discarded */
    size_t timerCount;

    /* Of timerCount, how many are cancelled and awaiting lazy discard.
     * Cancelling an already-fired id overcounts here (deflating
     * timerWheelCount) until the wheel next drains empty or
     * timerWheelReconcileCancellations restores the exact value; there
     * is no id index, so Unregister cannot detect the case inline. */
    size_t cancelledPending;

    /* Cached next expiry (for fast lookup) */
    timerWheelAdjustedUs cachedNextExpiry;

    /* Pending timers (scheduled from within callbacks) */
    flex *pendingTimers;

    timerWheelContext context;
    bool nextExpiryCacheValid;

    /* Statistics (the leading counters are hot-path writes) */
    timerWheelStats stats;

    /* ---- Warm: cancellation lookup state ---- */

    /* Cancellation is a tombstone scheme: cancelled ids sit in an
     * intset (full 64-bit ids; a narrower set would alias ids) until
     * processing encounters the timer's storage and discards it.
     * Ids at or below cancelledUpTo are cancelled wholesale (StopAll)
     * without materializing set entries — ids are monotonic, so the
     * watermark never has to be rolled back. */
    intset *cancelledTimers;
    timerWheelId cancelledUpTo;
    timerWheelId cancelLowest;
    timerWheelId cancelHighest;

    /* Set on every individual unregister (the id may already be gone);
     * cleared when a reconcile or drain proves the accounting exact.
     * Gates the O(n) exact walk in GetStats so it runs at most once
     * per cancellation batch. */
    bool cancelsMaybeStale;

    /* Overflow for very long timers (> 18.6 hours) */
    multimap *overflow;

    /* ---- Bulky: slot pointer arrays (indexed access; these always
     * occupy their own cache lines regardless of placement) ---- */

    flex *wheel0[WHEEL0_SIZE];
    flex *wheel1[WHEEL1_SIZE];
    flex *wheel2[WHEEL2_SIZE];
    flex *wheel3[WHEEL3_SIZE];
};

/* ====================================================================
 * Utility Functions
 * ==================================================================== */

static inline timerWheelAdjustedUs adjustedNowUs(const timerWheel *tw) {
    return timeUtilMonotonicUs() - tw->initialStartTime;
}

/* Expiry for a delay from `base`, saturating instead of wrapping: an
 * absurd duration becomes "effectively never" (parks in overflow)
 * rather than silently firing immediately. */
static inline timerWheelAdjustedUs expiryAfter(timerWheelAdjustedUs base,
                                               timerWheelDurationUs delay) {
    const timerWheelAdjustedUs expire = base + delay;
    return expire >= base ? expire : UINT64_MAX;
}

static inline uint64_t adjustedToAbsolute(const timerWheel *tw,
                                          timerWheelAdjustedUs adjusted) {
    return adjusted + tw->initialStartTime;
}

/* Determine which wheel a timer belongs to based on delay */
static inline int32_t getWheelLevel(timerWheelDurationUs delay) {
    if (delay < WHEEL0_SPAN_US) {
        return 0;
    } else if (delay < WHEEL1_SPAN_US) {
        return 1;
    } else if (delay < WHEEL2_SPAN_US) {
        return 2;
    } else if (delay < WHEEL3_SPAN_US) {
        return 3;
    } else {
        return WHEEL_LEVEL_OVERFLOW;
    }
}

/* Get slot index within a wheel for a given expiry time */
static inline uint32_t getSlotIndex(const timerWheel *tw, int32_t level,
                                    timerWheelAdjustedUs expireTimeUs) {
    /* An already-due expiry maps to the next slot the wheel processes
     * rather than underflowing into an arbitrary slot */
    const timerWheelDurationUs timeDiff =
        expireTimeUs > tw->currentTimeUs ? expireTimeUs - tw->currentTimeUs : 0;

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

/* A timer entry decoded from (or destined for) its 5-element
 * flex/multimap wire form */
typedef struct timerWheelEntry {
    timerWheelAdjustedUs expireTimeUs;
    timerWheelCallback *cb;
    void *clientData;
    timerWheelId id;
    timerWheelDurationUs repeatIntervalUs;
} timerWheelEntry;

/* Decode the 5-element timer entry at fe; returns the position after
 * it (NULL when this was the flex's final entry) */
static flexEntry *readTimerEntry(const flex *f, flexEntry *fe,
                                 timerWheelEntry *e) {
    databox boxes[ELEMENTS_PER_TIMER];

    for (int j = 0; j < ELEMENTS_PER_TIMER; j++) {
        flexGetByType(fe, &boxes[j]);
        fe = flexNext(f, fe);
    }

    e->expireTimeUs = boxes[0].data.u64;
    e->cb = (timerWheelCallback *)databoxRetrievePtrUnsigned(&boxes[1]);
    e->clientData = databoxRetrievePtrUnsigned(&boxes[2]);
    e->id = boxes[3].data.u64;
    e->repeatIntervalUs = boxes[4].data.u64;
    return fe;
}

/* Insert a timer entry into a slot's flex */
static void insertTimerIntoSlot(flex **slot, const timerWheelEntry *e) {
    if (*slot == NULL) {
        *slot = flexNew();
    }

    const databox expireBox = databoxNewUnsigned(e->expireTimeUs);
    const databox cbBox = databoxNewPtrAsUnsigned(e->cb);
    const databox dataBox = databoxNewPtrAsUnsigned(e->clientData);
    const databox idBox = databoxNewUnsigned(e->id);
    const databox repeatBox = databoxNewUnsigned(e->repeatIntervalUs);

    /* Append all 5 elements to end of flex (unsorted within slot) */
    flexPushByType(slot, &expireBox, FLEX_ENDPOINT_TAIL);
    flexPushByType(slot, &cbBox, FLEX_ENDPOINT_TAIL);
    flexPushByType(slot, &dataBox, FLEX_ENDPOINT_TAIL);
    flexPushByType(slot, &idBox, FLEX_ENDPOINT_TAIL);
    flexPushByType(slot, &repeatBox, FLEX_ENDPOINT_TAIL);
}

/* Insert timer into overflow multimap (sorted by expiry time) */
static void insertTimerIntoOverflow(timerWheel *tw, const timerWheelEntry *e) {
    const databox expireBox = databoxNewUnsigned(e->expireTimeUs);
    const databox cbBox = databoxNewPtrAsUnsigned(e->cb);
    const databox dataBox = databoxNewPtrAsUnsigned(e->clientData);
    const databox idBox = databoxNewUnsigned(e->id);
    const databox repeatBox = databoxNewUnsigned(e->repeatIntervalUs);

    const databox *entry[ELEMENTS_PER_TIMER] = {&expireBox, &cbBox, &dataBox,
                                                &idBox, &repeatBox};

    multimapInsert(&tw->overflow, entry);
}

static inline void wheel0OccupancySet(timerWheel *tw, uint32_t idx) {
    tw->wheel0Occupied[idx >> 6] |= 1ULL << (idx & 63);
}

static inline void wheel0OccupancyClear(timerWheel *tw, uint32_t idx) {
    tw->wheel0Occupied[idx >> 6] &= ~(1ULL << (idx & 63));
}

/* Ring distance from `from` to the nearest occupied wheel0 slot
 * (0 when `from` itself is occupied); WHEEL0_SIZE when wheel0 is
 * entirely empty. At most five word probes. */
static uint32_t wheel0OccupiedDistance(const timerWheel *tw, uint32_t from) {
    const uint32_t word = from >> 6;
    const uint32_t bit = from & 63;

    uint64_t w = tw->wheel0Occupied[word] >> bit;
    if (w) {
        return (uint32_t)__builtin_ctzll(w);
    }

    uint32_t dist = 64 - bit;
    for (uint32_t k = 1; k < WHEEL0_SIZE / 64; k++) {
        w = tw->wheel0Occupied[(word + k) & ((WHEEL0_SIZE / 64) - 1)];
        if (w) {
            return dist + (uint32_t)__builtin_ctzll(w);
        }
        dist += 64;
    }

    /* Wrap back into the low bits of the starting word */
    w = bit ? (tw->wheel0Occupied[word] & ((1ULL << bit) - 1)) : 0;
    if (w) {
        return dist + (uint32_t)__builtin_ctzll(w);
    }

    return WHEEL0_SIZE;
}

/* All wheel-slot inserts funnel through here so the wheel0 occupancy
 * bitmap can never go stale-clear (which would delay a timer by a
 * full revolution) */
static void insertTimerIntoWheelSlot(timerWheel *tw, int32_t level,
                                     uint32_t slotIdx,
                                     const timerWheelEntry *e) {
    insertTimerIntoSlot(getSlot(tw, level, slotIdx), e);
    if (level == 0) {
        wheel0OccupancySet(tw, slotIdx);
    }
}

/* Place a timer entry by how far away its expiry is: sub-resolution
 * delays go to pending (the wheel cannot represent them), in-span
 * delays go to the proper wheel slot, and beyond-span delays go to
 * overflow. Placement only - the caller owns timerCount accounting. */
static void scheduleTimerEntry(timerWheel *tw, const timerWheelEntry *e,
                               timerWheelDurationUs delay) {
    if (delay < WHEEL0_RESOLUTION_US) {
        insertTimerIntoSlot(&tw->pendingTimers, e);
        return;
    }

    const int32_t level = getWheelLevel(delay);

    if (level >= 0) {
        const uint32_t slotIdx = getSlotIndex(tw, level, e->expireTimeUs);
        insertTimerIntoWheelSlot(tw, level, slotIdx, e);
    } else {
        insertTimerIntoOverflow(tw, e);
    }
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
    tw->cancelledTimers = intsetNew();
    if (!tw->cancelledTimers) {
        multimapFree(tw->overflow);
        return false;
    }

    /* Initialize pending timers flex */
    tw->pendingTimers = flexNew();
    if (!tw->pendingTimers) {
        multimapFree(tw->overflow);
        intsetFree(tw->cancelledTimers);
        return false;
    }

    return true;
}

/* Free every wheel slot flex, leaving the slot pointers NULL */
static void freeAllWheelSlots(timerWheel *tw) {
    for (uint32_t i = 0; i < WHEEL0_SIZE; i++) {
        if (tw->wheel0[i]) {
            flexFree(tw->wheel0[i]);
            tw->wheel0[i] = NULL;
        }
    }
    for (uint32_t i = 0; i < WHEEL1_SIZE; i++) {
        if (tw->wheel1[i]) {
            flexFree(tw->wheel1[i]);
            tw->wheel1[i] = NULL;
        }
    }
    for (uint32_t i = 0; i < WHEEL2_SIZE; i++) {
        if (tw->wheel2[i]) {
            flexFree(tw->wheel2[i]);
            tw->wheel2[i] = NULL;
        }
    }
    for (uint32_t i = 0; i < WHEEL3_SIZE; i++) {
        if (tw->wheel3[i]) {
            flexFree(tw->wheel3[i]);
            tw->wheel3[i] = NULL;
        }
    }
}

void timerWheelDeinit(timerWheel *tw) {
    if (!tw) {
        return;
    }

    freeAllWheelSlots(tw);

    multimapFree(tw->overflow);
    intsetFree(tw->cancelledTimers);
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

timerWheelId timerWheelRegister(timerWheel *tw,
                                timerWheelDurationUs startAfterMicroseconds,
                                timerWheelDurationUs repeatEveryMicroseconds,
                                timerWheelCallback *cb, void *clientData) {
    const timerWheelId id = ++tw->nextTimerId;
    const timerWheelAdjustedUs now = adjustedNowUs(tw);
    const timerWheelAdjustedUs expireTimeUs =
        expiryAfter(now, startAfterMicroseconds);

    tw->stats.totalRegistrations++;
    tw->timerCount++;

    /* A new timer can only move the next expiry earlier, so fold it
     * into the cached minimum instead of invalidating: register-heavy
     * workloads keep O(1) next-event queries. */
    if (tw->nextExpiryCacheValid && expireTimeUs < tw->cachedNextExpiry) {
        tw->cachedNextExpiry = expireTimeUs;
    }

    const timerWheelEntry e = {.expireTimeUs = expireTimeUs,
                               .cb = cb,
                               .clientData = clientData,
                               .id = id,
                               .repeatIntervalUs = repeatEveryMicroseconds};

    /* Registrations made inside a callback go to pending regardless of
     * delay: the wheel structures may be mid-iteration. */
    if (tw->context == TIMER_WHEEL_CONTEXT_TIMER) {
        insertTimerIntoSlot(&tw->pendingTimers, &e);
        return id;
    }

    scheduleTimerEntry(tw, &e, startAfterMicroseconds);
    return id;
}

bool timerWheelUnregister(timerWheel *tw, timerWheelId id) {
    if (id == 0 || id > tw->nextTimerId) {
        return false;
    }

    /* Already cancelled wholesale by a StopAll watermark */
    if (id <= tw->cancelledUpTo) {
        return true;
    }

    bool success = false;
    intsetAdd(&tw->cancelledTimers, (int64_t)id, &success);

    if (success) {
        tw->stats.totalCancellations++;
        tw->cancelledPending++;
        tw->cancelsMaybeStale = true;

        /* Update bounds for efficient range checking */
        if (intsetCount(tw->cancelledTimers) == 1) {
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
    tw->stats.totalCancellations += timerWheelCount(tw);
    tw->nextExpiryCacheValid = false;

    /* Ids are monotonic, so "stop everything registered so far" is one
     * watermark: any id at or below it is cancelled. Individual
     * tombstones are all at or below the watermark too, so the set can
     * be dropped wholesale. */
    tw->cancelledUpTo = tw->nextTimerId;
    if (intsetCount(tw->cancelledTimers) > 0) {
        intsetFree(tw->cancelledTimers);
        tw->cancelledTimers = intsetNew();
    }
    tw->cancelLowest = 0;
    tw->cancelHighest = 0;

    if (tw->context == TIMER_WHEEL_CONTEXT_TIMER) {
        /* Mid-callback the wheel structures are being iterated, so
         * leave the entries in place; the watermark makes processing
         * discard each one lazily on encounter. */
        tw->cancelledPending = tw->timerCount;
        return true;
    }

    freeAllWheelSlots(tw);
    memset(tw->wheel0Occupied, 0, sizeof(tw->wheel0Occupied));
    multimapReset(tw->overflow);
    flexReset(&tw->pendingTimers);
    tw->timerCount = 0;
    tw->cancelledPending = 0;
    tw->cancelsMaybeStale = false;
    return true;
}

size_t timerWheelCount(const timerWheel *tw) {
    return tw->timerCount > tw->cancelledPending
               ? tw->timerCount - tw->cancelledPending
               : 0;
}

static bool isTimerCancelled(const timerWheel *tw, timerWheelId id);

/* Count stored entries whose id is tombstoned, collecting the
 * individually-tombstoned ids (those above the watermark) into
 * `liveSetOut`. O(n) over all stored timers - repair path only, never
 * called during timer processing (processSlot iterates a detached
 * slot this walk cannot see). */
static size_t countLiveCancellations(const timerWheel *tw,
                                     intset **liveSetOut) {
    size_t live = 0;

    flex *const *wheels[4] = {tw->wheel0, tw->wheel1, tw->wheel2, tw->wheel3};
    const size_t wheelSizes[4] = {WHEEL0_SIZE, WHEEL1_SIZE, WHEEL2_SIZE,
                                  WHEEL3_SIZE};

    for (size_t w = 0; w < 4; w++) {
        for (size_t i = 0; i < wheelSizes[w]; i++) {
            const flex *f = wheels[w][i];
            if (!f) {
                continue;
            }

            for (flexEntry *fe = flexHeadIfAny(f); fe;) {
                timerWheelEntry e;
                fe = readTimerEntry(f, fe, &e);
                if (isTimerCancelled(tw, e.id)) {
                    live++;
                    if (e.id > tw->cancelledUpTo) {
                        bool success = false;
                        intsetAdd(liveSetOut, (int64_t)e.id, &success);
                    }
                }
            }
        }
    }

    for (flexEntry *fe = flexHeadIfAny(tw->pendingTimers); fe;) {
        timerWheelEntry e;
        fe = readTimerEntry(tw->pendingTimers, fe, &e);
        if (isTimerCancelled(tw, e.id)) {
            live++;
            if (e.id > tw->cancelledUpTo) {
                bool success = false;
                intsetAdd(liveSetOut, (int64_t)e.id, &success);
            }
        }
    }

    multimapIterator iter;
    multimapIteratorInit(tw->overflow, &iter, true);
    databox boxes[ELEMENTS_PER_TIMER];
    while (multimapIteratorNextInto(&iter, boxes, ELEMENTS_PER_TIMER)) {
        const timerWheelId id = boxes[3].data.u64;
        if (isTimerCancelled(tw, id)) {
            live++;
            if (id > tw->cancelledUpTo) {
                bool success = false;
                intsetAdd(liveSetOut, (int64_t)id, &success);
            }
        }
    }

    return live;
}

bool timerWheelReconcileCancellations(timerWheel *tw) {
    /* Mid-callback the slot being processed is detached from the
     * wheel: its still-live cancelled entries would be misread as
     * orphans, and dropping their tombstones would let them fire. */
    if (tw->context == TIMER_WHEEL_CONTEXT_TIMER) {
        return false;
    }

    tw->cancelsMaybeStale = false;

    if (tw->cancelledPending == 0) {
        return true;
    }

    intset *liveSet = intsetNew();
    const size_t live = countLiveCancellations(tw, &liveSet);

    intsetFree(tw->cancelledTimers);
    tw->cancelledTimers = liveSet;
    tw->cancelledPending = live;

    int64_t bound;
    if (intsetMin(liveSet, &bound)) {
        tw->cancelLowest = (timerWheelId)bound;
        intsetMax(liveSet, &bound);
        tw->cancelHighest = (timerWheelId)bound;
    } else {
        tw->cancelLowest = 0;
        tw->cancelHighest = 0;
    }

    return true;
}

/* ====================================================================
 * Timer Cancellation Check
 * ==================================================================== */

/* Only meaningful for ids whose timer is (or was just) stored in the
 * wheel: any such cancelled timer keeps cancelledPending nonzero, so
 * the single field read up front answers the common no-cancellations
 * case without touching the watermark or the intset. */
static bool isTimerCancelled(const timerWheel *tw, timerWheelId id) {
    if (tw->cancelledPending == 0) {
        return false;
    }

    if (id <= tw->cancelledUpTo) {
        return true;
    }

    if (intsetCount(tw->cancelledTimers) == 0) {
        return false;
    }

    /* Quick bounds check */
    if (id < tw->cancelLowest || id > tw->cancelHighest) {
        return false;
    }

    return intsetFind(tw->cancelledTimers, (int64_t)id);
}

/* A cancelled timer's storage was just discarded during processing:
 * retire its tombstone. The caller adjusts timerCount (encountering an
 * entry in storage decrements it; a post-fire self-cancel already did).
 * Bounds are left wide after removal — tightening them costs an O(n)
 * intset scan and stale-wide bounds only cost a spurious intsetFind. */
static void dropCancellation(timerWheel *tw, timerWheelId id) {
    if (id > tw->cancelledUpTo) {
        bool success = false;
        intsetRemove(&tw->cancelledTimers, (int64_t)id, &success);

        if (intsetCount(tw->cancelledTimers) == 0) {
            tw->cancelLowest = 0;
            tw->cancelHighest = 0;
        }
    }

    if (tw->cancelledPending > 0) {
        tw->cancelledPending--;
    }
}

/* ====================================================================
 * Timer Execution
 * ==================================================================== */

/* Process all timers in a slot's flex */
static void processSlot(timerWheel *tw, flex **slot,
                        timerWheelAdjustedUs currentTime) {
    if (*slot == NULL || flexCount(*slot) == 0) {
        return;
    }

    /* Detach the slot before iterating: re-inserts below may target
     * this very slot index (e.g. a repeating timer whose next expiry
     * lands exactly one full wheel0 revolution away), and appending to
     * the flex being walked would invalidate the iteration pointers —
     * then freeing "the slot" at the end would drop the new entry. */
    const flex *f = *slot;
    *slot = NULL;

    /* Process all timer entries in this slot */
    for (flexEntry *fe = flexHeadIfAny(f); fe;) {
        timerWheelEntry e;
        fe = readTimerEntry(f, fe, &e);

        /* Check if timer should fire now */
        if (e.expireTimeUs <= currentTime) {
            /* Check if cancelled (cancelledPending gates the check:
             * any stored cancelled entry implies it is nonzero) */
            if (isTimerCancelled(tw, e.id)) {
                dropCancellation(tw, e.id);
                tw->timerCount--;
                continue;
            }

            /* Execute callback */
            tw->context = TIMER_WHEEL_CONTEXT_TIMER;
            const bool reschedule = e.cb(tw, e.id, e.clientData);
            tw->context = TIMER_WHEEL_CONTEXT_USER;

            tw->stats.totalExpirations++;
            tw->timerCount--;

            if (isTimerCancelled(tw, e.id)) {
                /* Cancelled during its own callback: retire the
                 * tombstone now whether or not it asked to repeat */
                dropCancellation(tw, e.id);
            } else if (reschedule && e.repeatIntervalUs > 0) {
                /* Schedule relative to wheel's current position (after this
                 * slot), NOT the final target time. This ensures repeating
                 * timers fire at each interval as the wheel advances, rather
                 * than jumping to the final target and missing intermediate
                 * fires. */
                e.expireTimeUs =
                    expiryAfter(tw->currentTimeUs + WHEEL0_RESOLUTION_US,
                                e.repeatIntervalUs);
                scheduleTimerEntry(tw, &e, e.repeatIntervalUs);
                tw->timerCount++;
            }
        } else {
            /* Timer not ready yet - re-insert into appropriate location */
            scheduleTimerEntry(tw, &e, e.expireTimeUs - currentTime);
        }
    }

    flexFree((flex *)f);
}

/* Cascade timers from higher wheel to lower wheels */
static void cascadeWheel(timerWheel *tw, int32_t level) {
    if (level <= 0 || level >= NUM_WHEELS) {
        return;
    }

    flex **slot = getSlot(tw, level, tw->slotIndex[level]);

    if (*slot == NULL || flexCount(*slot) == 0) {
        return;
    }

    tw->stats.totalCascades++;

    /* Detach before iterating so re-inserts can never alias the flex
     * being walked (see processSlot) */
    const flex *f = *slot;
    *slot = NULL;

    const timerWheelAdjustedUs currentTime = tw->currentTimeUs;

    /* Move all timers to appropriate lower-level slots */
    for (flexEntry *fe = flexHeadIfAny(f); fe;) {
        timerWheelEntry e;
        fe = readTimerEntry(f, fe, &e);

        /* Skip cancelled timers */
        if (isTimerCancelled(tw, e.id)) {
            dropCancellation(tw, e.id);
            tw->timerCount--;
            continue;
        }

        /* Determine new wheel level */
        const timerWheelDurationUs delay =
            e.expireTimeUs > currentTime ? e.expireTimeUs - currentTime : 0;

        /* Sub-resolution delays go to pending for prompt firing */
        if (delay < WHEEL0_RESOLUTION_US) {
            insertTimerIntoSlot(&tw->pendingTimers, &e);
        } else {
            const int32_t newLevel = getWheelLevel(delay);

            if (newLevel >= 0 && newLevel < level) {
                const uint32_t newSlotIdx =
                    getSlotIndex(tw, newLevel, e.expireTimeUs);
                insertTimerIntoWheelSlot(tw, newLevel, newSlotIdx, &e);
            } else if (newLevel == WHEEL_LEVEL_OVERFLOW) {
                /* Shouldn't happen during cascade, but handle gracefully */
                insertTimerIntoOverflow(tw, &e);
            } else {
                /* Cascades must make downward progress; a timer that
                 * maps back to this level or higher parks in pending
                 * and is re-placed when processPending runs at the end
                 * of this same processing pass */
                insertTimerIntoSlot(&tw->pendingTimers, &e);
            }
        }
    }

    flexFree((flex *)f);
}

/* Process overflow timers that are now within wheel range */
static void processOverflow(timerWheel *tw, timerWheelAdjustedUs currentTime) {
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

        const timerWheelAdjustedUs expireTimeUs = boxes[0].data.u64;

        /* Strictly within wheel range: a delay of exactly the full
         * wheel span still maps to overflow, so pulling it out here
         * would put it straight back (and loop forever) */
        if (expireTimeUs < currentTime + MAX_WHEEL_COVERAGE_US) {
            const timerWheelEntry e = {
                .expireTimeUs = expireTimeUs,
                .cb =
                    (timerWheelCallback *)databoxRetrievePtrUnsigned(&boxes[1]),
                .clientData = databoxRetrievePtrUnsigned(&boxes[2]),
                .id = boxes[3].data.u64,
                .repeatIntervalUs = boxes[4].data.u64};

            /* Remove from overflow using full-width delete */
            multimapDeleteFullWidth(&tw->overflow, (const databox **)boxPtrs);

            /* Skip if cancelled */
            if (isTimerCancelled(tw, e.id)) {
                dropCancellation(tw, e.id);
                tw->timerCount--;
                continue;
            }

            const timerWheelDurationUs delay =
                expireTimeUs > currentTime ? expireTimeUs - currentTime : 0;
            scheduleTimerEntry(tw, &e, delay);
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

    const timerWheelAdjustedUs now = adjustedNowUs(tw);

    for (flexEntry *fe = flexHeadIfAny(pending); fe;) {
        timerWheelEntry e;
        fe = readTimerEntry(pending, fe, &e);

        /* Check if timer should fire now (including zero-delay timers) */
        if (e.expireTimeUs <= now) {
            /* Skip if cancelled */
            if (isTimerCancelled(tw, e.id)) {
                dropCancellation(tw, e.id);
                tw->timerCount--;
                continue;
            }

            /* Execute callback */
            tw->context = TIMER_WHEEL_CONTEXT_TIMER;
            const bool reschedule = e.cb(tw, e.id, e.clientData);
            tw->context = TIMER_WHEEL_CONTEXT_USER;

            tw->stats.totalExpirations++;
            tw->timerCount--;

            if (isTimerCancelled(tw, e.id)) {
                /* Cancelled during its own callback: retire the
                 * tombstone now whether or not it asked to repeat */
                dropCancellation(tw, e.id);
            } else if (reschedule && e.repeatIntervalUs > 0) {
                /* Schedule relative to current time (now). processPending
                 * runs after the wheel has advanced to `now`, so we use
                 * `now` directly (unlike processSlot which uses wheel pos). */
                e.expireTimeUs = expiryAfter(now, e.repeatIntervalUs);
                scheduleTimerEntry(tw, &e, e.repeatIntervalUs);
                tw->timerCount++;
            }
        } else {
            /* Timer not due yet - insert into appropriate location */
            scheduleTimerEntry(tw, &e, e.expireTimeUs - now);
        }
    }

    flexFree(pending);
}

/* ====================================================================
 * Timer Processing
 * ==================================================================== */

/* A wheel0 wrap just happened: advance the upper wheels, then cascade
 * each slot being ENTERED, top-down. Entry-cascade (rather than
 * cascading a slot when leaving it) is what makes higher-wheel timers
 * fire within one wheel0 slot of their scheduled time instead of up to
 * a full higher-wheel slot span late. Top-down order lets a timer
 * dropped by wheel N land in wheel N-1's future slots before wheel N-1
 * itself cascades. */
static void advanceUpperWheels(timerWheel *tw) {
    tw->slotIndex[1] = (tw->slotIndex[1] + 1) & WHEEL1_MASK;

    if (tw->slotIndex[1] == 0) {
        tw->slotIndex[2] = (tw->slotIndex[2] + 1) & WHEEL2_MASK;

        if (tw->slotIndex[2] == 0) {
            tw->slotIndex[3] = (tw->slotIndex[3] + 1) & WHEEL3_MASK;
            cascadeWheel(tw, 3);
        }

        cascadeWheel(tw, 2);
    }

    cascadeWheel(tw, 1);
}

void timerWheelProcessTimerEvents(timerWheel *tw) {
    /* Callbacks must not re-enter processing: the slot flex being
     * iterated would be processed (and freed) twice. Deferred work is
     * picked up by the in-progress outer call. */
    if (tw->context == TIMER_WHEEL_CONTEXT_TIMER) {
        return;
    }

    const timerWheelAdjustedUs now = adjustedNowUs(tw);
    tw->nextExpiryCacheValid = false;

    /* Process overflow timers that may have come into range */
    processOverflow(tw, now);

    if (tw->timerCount == 0) {
        /* No entries anywhere, so every remaining tombstone is stale
         * (a cancel of an already-fired id) - purge them */
        if (intsetCount(tw->cancelledTimers) > 0) {
            intsetFree(tw->cancelledTimers);
            tw->cancelledTimers = intsetNew();
            tw->cancelLowest = 0;
            tw->cancelHighest = 0;
        }
        tw->cancelledPending = 0;
        tw->cancelsMaybeStale = false;

        /* Empty wheel fast-forward: nothing can fire or cascade, so
         * jump straight to `now` instead of ticking 1ms at a time.
         * Slot indices are the tick-count odometer (see struct). */
        if (tw->currentTimeUs < now) {
            const uint64_t ticks =
                (now + WHEEL0_RESOLUTION_US - 1) / WHEEL0_RESOLUTION_US;
            tw->currentTimeUs = ticks * WHEEL0_RESOLUTION_US;
            tw->slotIndex[0] = ticks & WHEEL0_MASK;
            tw->slotIndex[1] = (ticks >> WHEEL0_BITS) & WHEEL1_MASK;
            tw->slotIndex[2] =
                (ticks >> (WHEEL0_BITS + WHEEL1_BITS)) & WHEEL2_MASK;
            tw->slotIndex[3] =
                (ticks >> (WHEEL0_BITS + WHEEL1_BITS + WHEEL2_BITS)) &
                WHEEL3_MASK;
        }

        return;
    }

    /* Advance wheel0 to current time, cascading as needed */
    while (tw->currentTimeUs < now) {
        const uint32_t slot0Idx = tw->slotIndex[0];

        if (tw->wheel0[slot0Idx] == NULL) {
            /* Empty slot: hop straight to the next occupied slot, the
             * wrap boundary (cascade point), or `now` - whichever is
             * nearest - instead of ticking through the empty span. */
            const uint64_t remainingTicks =
                (now - tw->currentTimeUs + WHEEL0_RESOLUTION_US - 1) /
                WHEEL0_RESOLUTION_US;
            const uint32_t tillWrap = WHEEL0_SIZE - slot0Idx;
            const uint32_t tillOccupied = wheel0OccupiedDistance(tw, slot0Idx);

            uint64_t hop = remainingTicks;
            if (tillWrap < hop) {
                hop = tillWrap;
            }
            if (tillOccupied < hop) {
                hop = tillOccupied;
            }
            if (hop == 0) {
                /* Unreachable while the occupancy invariant holds (this
                 * slot is NULL, so its bit is clear and tillOccupied is
                 * at least 1); guarantee forward progress regardless
                 * rather than risk an infinite loop */
                hop = 1;
            }

            if (hop == tillOccupied) {
                __builtin_prefetch(
                    tw->wheel0[(slot0Idx + (uint32_t)hop) & WHEEL0_MASK], 0, 1);
            }

            tw->currentTimeUs += hop * WHEEL0_RESOLUTION_US;
            tw->slotIndex[0] = (slot0Idx + (uint32_t)hop) & WHEEL0_MASK;
            if (slot0Idx + hop == WHEEL0_SIZE) {
                advanceUpperWheels(tw);
            }
            continue;
        }

        /* Prefetch next slot for better cache performance */
        const uint32_t nextSlot0Idx = (slot0Idx + 1) & WHEEL0_MASK;
        if (tw->wheel0[nextSlot0Idx]) {
            __builtin_prefetch(tw->wheel0[nextSlot0Idx], 0, 1);
        }

        /* Clear occupancy before processing: processSlot may
         * re-populate this very slot index, re-setting the bit */
        wheel0OccupancyClear(tw, slot0Idx);
        processSlot(tw, &tw->wheel0[slot0Idx], now);

        /* Advance wheel0 */
        tw->slotIndex[0] = nextSlot0Idx;
        tw->currentTimeUs += WHEEL0_RESOLUTION_US;

        if (nextSlot0Idx == 0) {
            advanceUpperWheels(tw);
        }
    }

    /* Process any timers scheduled during callbacks */
    processPending(tw);
}

void timerWheelAdvanceTime(timerWheel *tw, timerWheelDurationUs microseconds) {
    /* For testing: manually advance time */
    tw->initialStartTime -= microseconds;
    timerWheelProcessTimerEvents(tw);
}

/* ====================================================================
 * Timer Queries
 * ==================================================================== */

/* Earliest expiry among all timer entries in a slot flex. Slots are
 * append-only (registration is the latency-critical path and firing
 * visits every entry regardless of order), so this query-side scan is
 * where the unsortedness is paid for - and it runs at most once per
 * process cycle thanks to the next-expiry cache. */
static timerWheelAdjustedUs flexMinExpiry(const flex *f) {
    timerWheelAdjustedUs earliest = UINT64_MAX;

    for (flexEntry *fe = flexHeadIfAny(f); fe;) {
        databox box;
        flexGetByType(fe, &box);
        if (box.data.u64 < earliest) {
            earliest = box.data.u64;
        }

        for (int j = 0; j < ELEMENTS_PER_TIMER && fe; j++) {
            fe = flexNext(f, fe);
        }
    }

    return earliest;
}

timerWheelSystemMonotonicUs timerWheelNextTimerEventStartUs(timerWheel *tw) {
    if (tw->nextExpiryCacheValid) {
        return (timerWheelSystemMonotonicUs)adjustedToAbsolute(
            tw, tw->cachedNextExpiry);
    }

    timerWheelAdjustedUs earliest = UINT64_MAX;

    /* Pending is unsorted and may hold anything from overdue timers to
     * far-future ones registered inside callbacks - scan all of it */
    if (tw->pendingTimers && flexCount(tw->pendingTimers) > 0) {
        earliest = flexMinExpiry(tw->pendingTimers);
    }

    /* Check wheel 0 first (most likely to have nearest timer). The
     * first occupied slot in ring order is the nearest slot. */
    const uint32_t w0Dist = wheel0OccupiedDistance(tw, tw->slotIndex[0]);
    if (w0Dist < WHEEL0_SIZE) {
        const uint32_t idx = (tw->slotIndex[0] + w0Dist) & WHEEL0_MASK;
        const timerWheelAdjustedUs slotMin = flexMinExpiry(tw->wheel0[idx]);
        if (slotMin < earliest) {
            earliest = slotMin;
        }
    }

    /* Each higher wheel contributes its nearest occupied slot: level
     * order says nothing about time order, and a nearer timer here
     * must not be masked by pending/wheel0 having some (later) entry */
    for (int32_t level = 1; level < NUM_WHEELS; level++) {
        const uint32_t size = getWheelSize(level);
        for (uint32_t i = 0; i < size; i++) {
            const uint32_t idx = (tw->slotIndex[level] + i) & (size - 1);
            flex **slot = getSlot(tw, level, idx);
            if (*slot && flexCount(*slot) > 0) {
                const timerWheelAdjustedUs slotMin = flexMinExpiry(*slot);
                if (slotMin < earliest) {
                    earliest = slotMin;
                }
                break;
            }
        }
    }

    /* Check overflow (sorted by expiry - first entry is earliest).
     * First fills ALL elementsPerEntry slots, so the out array must be
     * full width even though only the expiry is needed. */
    if (multimapCount(tw->overflow) > 0) {
        databox boxes[ELEMENTS_PER_TIMER];
        multimapFirstInto(tw->overflow, boxes, ELEMENTS_PER_TIMER);
        if (boxes[0].data.u64 < earliest) {
            earliest = boxes[0].data.u64;
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

void timerWheelGetStats(timerWheel *tw, timerWheelStats *stats) {
    /* Individual unregisters may name already-fired ids; reconcile
     * once so the reported (and stored) accounting is exact. The flag
     * keeps this O(n) walk to at most once per cancellation batch.
     * Mid-callback the walk is unsafe (detached slot) - the counter is
     * reported as-is and the next out-of-callback stats call heals. */
    if (tw->cancelsMaybeStale && tw->context != TIMER_WHEEL_CONTEXT_TIMER) {
        timerWheelReconcileCancellations(tw);
    }

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
    stats->memoryBytes += intsetBytes(tw->cancelledTimers);
    stats->memoryBytes += flexBytes(tw->pendingTimers);

    stats->overflowCount = multimapCount(tw->overflow);
    stats->pendingCount = flexCount(tw->pendingTimers) / ELEMENTS_PER_TIMER;
    stats->cancelledPending = tw->cancelledPending;
}

void timerWheelResetStats(timerWheel *tw) {
    tw->stats.totalRegistrations = 0;
    tw->stats.totalCancellations = 0;
    tw->stats.totalExpirations = 0;
    tw->stats.totalCascades = 0;
    /* overflowCount / pendingCount / cancelledPending / memoryBytes are
     * current state computed by timerWheelGetStats, not counters */
}
