/* Segment Tree - I32 Dispatcher (2-TIER SYSTEM)
 * Auto-generated tier management and public API.
 */

#include "segmentI32.h"
#include "../datakit.h"

/* Create new segmentI32 */
void *segmentI32New(segmentOp op) {
    segmentI32Small *small = segmentI32SmallNew(op);
    return SEGMENT_I32_TAG(small, SEGMENT_I32_TYPE_SMALL);
}

/* Free segmentI32 */
void segmentI32Free(void *seg) {
    if (!seg) {
        return;
    }

    segmentI32Type type = SEGMENT_I32_TYPE(seg);
    void *ptr = SEGMENT_I32_UNTAG(seg);

    switch (type) {
    case SEGMENT_I32_TYPE_SMALL:
        segmentI32SmallFree((segmentI32Small *)ptr);
        break;
    case SEGMENT_I32_TYPE_FULL:
        segmentI32FullFree((segmentI32Full *)ptr);
        break;
    }
}

/* Upgrade Small to Full */
static void *segmentI32UpgradeSmallToFull(segmentI32Small *small) {
    segmentI32Full *full = segmentI32FullFromSmall(small);
    segmentI32SmallFree(small);
    return SEGMENT_I32_TAG(full, SEGMENT_I32_TYPE_FULL);
}

/* Update with tier transitions */
void segmentI32Update(void **seg, size_t idx, int32_t value) {
    if (!seg) {
        return;
    }

    if (!*seg) {
        *seg = segmentI32New(SEGMENT_OP_SUM);
    }

    segmentI32Type type = SEGMENT_I32_TYPE(*seg);
    void *ptr = SEGMENT_I32_UNTAG(*seg);

    switch (type) {
    case SEGMENT_I32_TYPE_SMALL: {
        segmentI32Small *small = (segmentI32Small *)ptr;

        /* Check upgrade */
        if (segmentI32SmallShouldUpgrade(small) ||
            idx >= segmentI32SmallCount(small) + 1000) {
            *seg = segmentI32UpgradeSmallToFull(small);
            segmentI32Update(seg, idx, value);
            return;
        }

        bool success;
        segmentI32Small *newSmall =
            segmentI32SmallUpdate(small, (uint32_t)idx, value, &success);
        *seg = SEGMENT_I32_TAG(newSmall, SEGMENT_I32_TYPE_SMALL);
        break;
    }

    case SEGMENT_I32_TYPE_FULL: {
        segmentI32Full *full = (segmentI32Full *)ptr;

        bool success;
        segmentI32Full *newFull =
            segmentI32FullUpdate(full, (uint64_t)idx, value, &success);
        *seg = SEGMENT_I32_TAG(newFull, SEGMENT_I32_TYPE_FULL);
        break;
    }
    }
}

/* Query range */
int32_t segmentI32Query(const void *seg, size_t left, size_t right) {
    if (!seg) {
        return (int32_t)0;
    }

    segmentI32Type type = SEGMENT_I32_TYPE(seg);
    void *ptr = SEGMENT_I32_UNTAG(seg);

    switch (type) {
    case SEGMENT_I32_TYPE_SMALL:
        return segmentI32SmallQuery((const segmentI32Small *)ptr,
                                    (uint32_t)left, (uint32_t)right);
    case SEGMENT_I32_TYPE_FULL:
        return segmentI32FullQuery((const segmentI32Full *)ptr, (uint64_t)left,
                                   (uint64_t)right);
    }

    return (int32_t)0;
}

/* Get single element */
int32_t segmentI32Get(const void *seg, size_t idx) {
    if (!seg) {
        return (int32_t)0;
    }

    segmentI32Type type = SEGMENT_I32_TYPE(seg);
    void *ptr = SEGMENT_I32_UNTAG(seg);

    switch (type) {
    case SEGMENT_I32_TYPE_SMALL:
        return segmentI32SmallGet((const segmentI32Small *)ptr, (uint32_t)idx);
    case SEGMENT_I32_TYPE_FULL:
        return segmentI32FullGet((const segmentI32Full *)ptr, (uint64_t)idx);
    }

    return (int32_t)0;
}

/* Get count */
size_t segmentI32Count(const void *seg) {
    if (!seg) {
        return 0;
    }

    segmentI32Type type = SEGMENT_I32_TYPE(seg);
    void *ptr = SEGMENT_I32_UNTAG(seg);

    switch (type) {
    case SEGMENT_I32_TYPE_SMALL:
        return segmentI32SmallCount((const segmentI32Small *)ptr);
    case SEGMENT_I32_TYPE_FULL:
        return segmentI32FullCount((const segmentI32Full *)ptr);
    }

    return 0;
}

/* Get bytes */
size_t segmentI32Bytes(const void *seg) {
    if (!seg) {
        return 0;
    }

    segmentI32Type type = SEGMENT_I32_TYPE(seg);
    void *ptr = SEGMENT_I32_UNTAG(seg);

    switch (type) {
    case SEGMENT_I32_TYPE_SMALL:
        return segmentI32SmallBytes((const segmentI32Small *)ptr);
    case SEGMENT_I32_TYPE_FULL:
        return segmentI32FullBytes((const segmentI32Full *)ptr);
    }

    return 0;
}

/* Range update (Full tier only) */
void segmentI32RangeUpdate(void *seg, size_t left, size_t right,
                           int32_t value) {
    if (!seg) {
        return;
    }

    segmentI32Type type = SEGMENT_I32_TYPE(seg);
    void *ptr = SEGMENT_I32_UNTAG(seg);

    if (type == SEGMENT_I32_TYPE_FULL) {
        segmentI32FullRangeUpdate((segmentI32Full *)ptr, (uint64_t)left,
                                  (uint64_t)right, value);
    }
    /* Small tier: implement point-by-point update */
    else if (type == SEGMENT_I32_TYPE_SMALL) {
        for (size_t i = left; i <= right; i++) {
            segmentI32Update(&seg, i, value);
        }
    }
}

#ifdef DATAKIT_TEST
void segmentI32Repr(const void *seg) {
    if (!seg) {
        printf("segmentI32: (nil)\n");
        return;
    }

    segmentI32Type type = SEGMENT_I32_TYPE(seg);
    void *ptr = SEGMENT_I32_UNTAG(seg);

    const char *tierName = type == SEGMENT_I32_TYPE_SMALL ? "SMALL" : "FULL";
    printf("segmentI32 [tier=%s, count=%zu, bytes=%zu]\n", tierName,
           segmentI32Count(seg), segmentI32Bytes(seg));

    if (type == SEGMENT_I32_TYPE_SMALL) {
        segmentI32SmallRepr((const segmentI32Small *)ptr);
    } else {
        segmentI32FullRepr((const segmentI32Full *)ptr);
    }
}
#endif
