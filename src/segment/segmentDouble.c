/* Segment Tree - Double Dispatcher (2-TIER SYSTEM)
 * Auto-generated tier management and public API.
 */

#include "segmentDouble.h"
#include "../datakit.h"

/* Create new segmentDouble */
void *segmentDoubleNew(segmentOp op) {
    segmentDoubleSmall *small = segmentDoubleSmallNew(op);
    return SEGMENT_DOUBLE_TAG(small, SEGMENT_DOUBLE_TYPE_SMALL);
}

/* Free segmentDouble */
void segmentDoubleFree(void *seg) {
    if (!seg) {
        return;
    }

    segmentDoubleType type = SEGMENT_DOUBLE_TYPE(seg);
    void *ptr = SEGMENT_DOUBLE_UNTAG(seg);

    switch (type) {
    case SEGMENT_DOUBLE_TYPE_SMALL:
        segmentDoubleSmallFree((segmentDoubleSmall *)ptr);
        break;
    case SEGMENT_DOUBLE_TYPE_FULL:
        segmentDoubleFullFree((segmentDoubleFull *)ptr);
        break;
    }
}

/* Upgrade Small to Full */
static void *segmentDoubleUpgradeSmallToFull(segmentDoubleSmall *small) {
    segmentDoubleFull *full = segmentDoubleFullFromSmall(small);
    segmentDoubleSmallFree(small);
    return SEGMENT_DOUBLE_TAG(full, SEGMENT_DOUBLE_TYPE_FULL);
}

/* Update with tier transitions */
void segmentDoubleUpdate(void **seg, size_t idx, double value) {
    if (!seg) {
        return;
    }

    if (!*seg) {
        *seg = segmentDoubleNew(SEGMENT_OP_SUM);
    }

    segmentDoubleType type = SEGMENT_DOUBLE_TYPE(*seg);
    void *ptr = SEGMENT_DOUBLE_UNTAG(*seg);

    switch (type) {
    case SEGMENT_DOUBLE_TYPE_SMALL: {
        segmentDoubleSmall *small = (segmentDoubleSmall *)ptr;

        /* Check upgrade */
        if (segmentDoubleSmallShouldUpgrade(small) ||
            idx >= segmentDoubleSmallCount(small) + 1000) {
            *seg = segmentDoubleUpgradeSmallToFull(small);
            segmentDoubleUpdate(seg, idx, value);
            return;
        }

        bool success;
        segmentDoubleSmall *newSmall =
            segmentDoubleSmallUpdate(small, (uint32_t)idx, value, &success);
        *seg = SEGMENT_DOUBLE_TAG(newSmall, SEGMENT_DOUBLE_TYPE_SMALL);
        break;
    }

    case SEGMENT_DOUBLE_TYPE_FULL: {
        segmentDoubleFull *full = (segmentDoubleFull *)ptr;

        bool success;
        segmentDoubleFull *newFull =
            segmentDoubleFullUpdate(full, (uint64_t)idx, value, &success);
        *seg = SEGMENT_DOUBLE_TAG(newFull, SEGMENT_DOUBLE_TYPE_FULL);
        break;
    }
    }
}

/* Query range */
double segmentDoubleQuery(const void *seg, size_t left, size_t right) {
    if (!seg) {
        return (double)0;
    }

    segmentDoubleType type = SEGMENT_DOUBLE_TYPE(seg);
    void *ptr = SEGMENT_DOUBLE_UNTAG(seg);

    switch (type) {
    case SEGMENT_DOUBLE_TYPE_SMALL:
        return segmentDoubleSmallQuery((const segmentDoubleSmall *)ptr,
                                       (uint32_t)left, (uint32_t)right);
    case SEGMENT_DOUBLE_TYPE_FULL:
        return segmentDoubleFullQuery((const segmentDoubleFull *)ptr,
                                      (uint64_t)left, (uint64_t)right);
    }

    return (double)0;
}

/* Get single element */
double segmentDoubleGet(const void *seg, size_t idx) {
    if (!seg) {
        return (double)0;
    }

    segmentDoubleType type = SEGMENT_DOUBLE_TYPE(seg);
    void *ptr = SEGMENT_DOUBLE_UNTAG(seg);

    switch (type) {
    case SEGMENT_DOUBLE_TYPE_SMALL:
        return segmentDoubleSmallGet((const segmentDoubleSmall *)ptr,
                                     (uint32_t)idx);
    case SEGMENT_DOUBLE_TYPE_FULL:
        return segmentDoubleFullGet((const segmentDoubleFull *)ptr,
                                    (uint64_t)idx);
    }

    return (double)0;
}

/* Get count */
size_t segmentDoubleCount(const void *seg) {
    if (!seg) {
        return 0;
    }

    segmentDoubleType type = SEGMENT_DOUBLE_TYPE(seg);
    void *ptr = SEGMENT_DOUBLE_UNTAG(seg);

    switch (type) {
    case SEGMENT_DOUBLE_TYPE_SMALL:
        return segmentDoubleSmallCount((const segmentDoubleSmall *)ptr);
    case SEGMENT_DOUBLE_TYPE_FULL:
        return segmentDoubleFullCount((const segmentDoubleFull *)ptr);
    }

    return 0;
}

/* Get bytes */
size_t segmentDoubleBytes(const void *seg) {
    if (!seg) {
        return 0;
    }

    segmentDoubleType type = SEGMENT_DOUBLE_TYPE(seg);
    void *ptr = SEGMENT_DOUBLE_UNTAG(seg);

    switch (type) {
    case SEGMENT_DOUBLE_TYPE_SMALL:
        return segmentDoubleSmallBytes((const segmentDoubleSmall *)ptr);
    case SEGMENT_DOUBLE_TYPE_FULL:
        return segmentDoubleFullBytes((const segmentDoubleFull *)ptr);
    }

    return 0;
}

/* Range update (Full tier only) */
void segmentDoubleRangeUpdate(void *seg, size_t left, size_t right,
                              double value) {
    if (!seg) {
        return;
    }

    segmentDoubleType type = SEGMENT_DOUBLE_TYPE(seg);
    void *ptr = SEGMENT_DOUBLE_UNTAG(seg);

    if (type == SEGMENT_DOUBLE_TYPE_FULL) {
        segmentDoubleFullRangeUpdate((segmentDoubleFull *)ptr, (uint64_t)left,
                                     (uint64_t)right, value);
    }
    /* Small tier: implement point-by-point update */
    else if (type == SEGMENT_DOUBLE_TYPE_SMALL) {
        for (size_t i = left; i <= right; i++) {
            segmentDoubleUpdate(&seg, i, value);
        }
    }
}

#ifdef DATAKIT_TEST
void segmentDoubleRepr(const void *seg) {
    if (!seg) {
        printf("segmentDouble: (nil)\n");
        return;
    }

    segmentDoubleType type = SEGMENT_DOUBLE_TYPE(seg);
    void *ptr = SEGMENT_DOUBLE_UNTAG(seg);

    const char *tierName = type == SEGMENT_DOUBLE_TYPE_SMALL ? "SMALL" : "FULL";
    printf("segmentDouble [tier=%s, count=%zu, bytes=%zu]\n", tierName,
           segmentDoubleCount(seg), segmentDoubleBytes(seg));

    if (type == SEGMENT_DOUBLE_TYPE_SMALL) {
        segmentDoubleSmallRepr((const segmentDoubleSmall *)ptr);
    } else {
        segmentDoubleFullRepr((const segmentDoubleFull *)ptr);
    }
}
#endif
