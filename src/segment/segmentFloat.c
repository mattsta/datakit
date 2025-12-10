/* Segment Tree - Float Dispatcher (2-TIER SYSTEM)
 * Auto-generated tier management and public API.
 */

#include "segmentFloat.h"
#include "../datakit.h"

/* Create new segmentFloat */
void *segmentFloatNew(segmentOp op) {
    segmentFloatSmall *small = segmentFloatSmallNew(op);
    return SEGMENT_FLOAT_TAG(small, SEGMENT_FLOAT_TYPE_SMALL);
}

/* Free segmentFloat */
void segmentFloatFree(void *seg) {
    if (!seg) {
        return;
    }

    segmentFloatType type = SEGMENT_FLOAT_TYPE(seg);
    void *ptr = SEGMENT_FLOAT_UNTAG(seg);

    switch (type) {
    case SEGMENT_FLOAT_TYPE_SMALL:
        segmentFloatSmallFree((segmentFloatSmall *)ptr);
        break;
    case SEGMENT_FLOAT_TYPE_FULL:
        segmentFloatFullFree((segmentFloatFull *)ptr);
        break;
    }
}

/* Upgrade Small to Full */
static void *segmentFloatUpgradeSmallToFull(segmentFloatSmall *small) {
    segmentFloatFull *full = segmentFloatFullFromSmall(small);
    segmentFloatSmallFree(small);
    return SEGMENT_FLOAT_TAG(full, SEGMENT_FLOAT_TYPE_FULL);
}

/* Update with tier transitions */
void segmentFloatUpdate(void **seg, size_t idx, float value) {
    if (!seg) {
        return;
    }

    if (!*seg) {
        *seg = segmentFloatNew(SEGMENT_OP_SUM);
    }

    segmentFloatType type = SEGMENT_FLOAT_TYPE(*seg);
    void *ptr = SEGMENT_FLOAT_UNTAG(*seg);

    switch (type) {
    case SEGMENT_FLOAT_TYPE_SMALL: {
        segmentFloatSmall *small = (segmentFloatSmall *)ptr;

        /* Check upgrade */
        if (segmentFloatSmallShouldUpgrade(small) ||
            idx >= segmentFloatSmallCount(small) + 1000) {
            *seg = segmentFloatUpgradeSmallToFull(small);
            segmentFloatUpdate(seg, idx, value);
            return;
        }

        bool success;
        segmentFloatSmall *newSmall =
            segmentFloatSmallUpdate(small, (uint32_t)idx, value, &success);
        *seg = SEGMENT_FLOAT_TAG(newSmall, SEGMENT_FLOAT_TYPE_SMALL);
        break;
    }

    case SEGMENT_FLOAT_TYPE_FULL: {
        segmentFloatFull *full = (segmentFloatFull *)ptr;

        bool success;
        segmentFloatFull *newFull =
            segmentFloatFullUpdate(full, (uint64_t)idx, value, &success);
        *seg = SEGMENT_FLOAT_TAG(newFull, SEGMENT_FLOAT_TYPE_FULL);
        break;
    }
    }
}

/* Query range */
float segmentFloatQuery(const void *seg, size_t left, size_t right) {
    if (!seg) {
        return (float)0;
    }

    segmentFloatType type = SEGMENT_FLOAT_TYPE(seg);
    void *ptr = SEGMENT_FLOAT_UNTAG(seg);

    switch (type) {
    case SEGMENT_FLOAT_TYPE_SMALL:
        return segmentFloatSmallQuery((const segmentFloatSmall *)ptr,
                                      (uint32_t)left, (uint32_t)right);
    case SEGMENT_FLOAT_TYPE_FULL:
        return segmentFloatFullQuery((const segmentFloatFull *)ptr,
                                     (uint64_t)left, (uint64_t)right);
    }

    return (float)0;
}

/* Get single element */
float segmentFloatGet(const void *seg, size_t idx) {
    if (!seg) {
        return (float)0;
    }

    segmentFloatType type = SEGMENT_FLOAT_TYPE(seg);
    void *ptr = SEGMENT_FLOAT_UNTAG(seg);

    switch (type) {
    case SEGMENT_FLOAT_TYPE_SMALL:
        return segmentFloatSmallGet((const segmentFloatSmall *)ptr,
                                    (uint32_t)idx);
    case SEGMENT_FLOAT_TYPE_FULL:
        return segmentFloatFullGet((const segmentFloatFull *)ptr,
                                   (uint64_t)idx);
    }

    return (float)0;
}

/* Get count */
size_t segmentFloatCount(const void *seg) {
    if (!seg) {
        return 0;
    }

    segmentFloatType type = SEGMENT_FLOAT_TYPE(seg);
    void *ptr = SEGMENT_FLOAT_UNTAG(seg);

    switch (type) {
    case SEGMENT_FLOAT_TYPE_SMALL:
        return segmentFloatSmallCount((const segmentFloatSmall *)ptr);
    case SEGMENT_FLOAT_TYPE_FULL:
        return segmentFloatFullCount((const segmentFloatFull *)ptr);
    }

    return 0;
}

/* Get bytes */
size_t segmentFloatBytes(const void *seg) {
    if (!seg) {
        return 0;
    }

    segmentFloatType type = SEGMENT_FLOAT_TYPE(seg);
    void *ptr = SEGMENT_FLOAT_UNTAG(seg);

    switch (type) {
    case SEGMENT_FLOAT_TYPE_SMALL:
        return segmentFloatSmallBytes((const segmentFloatSmall *)ptr);
    case SEGMENT_FLOAT_TYPE_FULL:
        return segmentFloatFullBytes((const segmentFloatFull *)ptr);
    }

    return 0;
}

/* Range update (Full tier only) */
void segmentFloatRangeUpdate(void *seg, size_t left, size_t right,
                             float value) {
    if (!seg) {
        return;
    }

    segmentFloatType type = SEGMENT_FLOAT_TYPE(seg);
    void *ptr = SEGMENT_FLOAT_UNTAG(seg);

    if (type == SEGMENT_FLOAT_TYPE_FULL) {
        segmentFloatFullRangeUpdate((segmentFloatFull *)ptr, (uint64_t)left,
                                    (uint64_t)right, value);
    }
    /* Small tier: implement point-by-point update */
    else if (type == SEGMENT_FLOAT_TYPE_SMALL) {
        for (size_t i = left; i <= right; i++) {
            segmentFloatUpdate(&seg, i, value);
        }
    }
}

#ifdef DATAKIT_TEST
void segmentFloatRepr(const void *seg) {
    if (!seg) {
        printf("segmentFloat: (nil)\n");
        return;
    }

    segmentFloatType type = SEGMENT_FLOAT_TYPE(seg);
    void *ptr = SEGMENT_FLOAT_UNTAG(seg);

    const char *tierName = type == SEGMENT_FLOAT_TYPE_SMALL ? "SMALL" : "FULL";
    printf("segmentFloat [tier=%s, count=%zu, bytes=%zu]\n", tierName,
           segmentFloatCount(seg), segmentFloatBytes(seg));

    if (type == SEGMENT_FLOAT_TYPE_SMALL) {
        segmentFloatSmallRepr((const segmentFloatSmall *)ptr);
    } else {
        segmentFloatFullRepr((const segmentFloatFull *)ptr);
    }
}
#endif
