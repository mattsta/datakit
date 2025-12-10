/* Segment Tree - U32 Dispatcher (2-TIER SYSTEM)
 * Auto-generated tier management and public API.
 */

#include "segmentU32.h"
#include "../datakit.h"

/* Create new segmentU32 */
void *segmentU32New(segmentOp op) {
    segmentU32Small *small = segmentU32SmallNew(op);
    return SEGMENT_U32_TAG(small, SEGMENT_U32_TYPE_SMALL);
}

/* Free segmentU32 */
void segmentU32Free(void *seg) {
    if (!seg) {
        return;
    }

    segmentU32Type type = SEGMENT_U32_TYPE(seg);
    void *ptr = SEGMENT_U32_UNTAG(seg);

    switch (type) {
    case SEGMENT_U32_TYPE_SMALL:
        segmentU32SmallFree((segmentU32Small *)ptr);
        break;
    case SEGMENT_U32_TYPE_FULL:
        segmentU32FullFree((segmentU32Full *)ptr);
        break;
    }
}

/* Upgrade Small to Full */
static void *segmentU32UpgradeSmallToFull(segmentU32Small *small) {
    segmentU32Full *full = segmentU32FullFromSmall(small);
    segmentU32SmallFree(small);
    return SEGMENT_U32_TAG(full, SEGMENT_U32_TYPE_FULL);
}

/* Update with tier transitions */
void segmentU32Update(void **seg, size_t idx, uint32_t value) {
    if (!seg) {
        return;
    }

    if (!*seg) {
        *seg = segmentU32New(SEGMENT_OP_SUM);
    }

    segmentU32Type type = SEGMENT_U32_TYPE(*seg);
    void *ptr = SEGMENT_U32_UNTAG(*seg);

    switch (type) {
    case SEGMENT_U32_TYPE_SMALL: {
        segmentU32Small *small = (segmentU32Small *)ptr;

        /* Check upgrade */
        if (segmentU32SmallShouldUpgrade(small) ||
            idx >= segmentU32SmallCount(small) + 1000) {
            *seg = segmentU32UpgradeSmallToFull(small);
            segmentU32Update(seg, idx, value);
            return;
        }

        bool success;
        segmentU32Small *newSmall =
            segmentU32SmallUpdate(small, (uint32_t)idx, value, &success);
        *seg = SEGMENT_U32_TAG(newSmall, SEGMENT_U32_TYPE_SMALL);
        break;
    }

    case SEGMENT_U32_TYPE_FULL: {
        segmentU32Full *full = (segmentU32Full *)ptr;

        bool success;
        segmentU32Full *newFull =
            segmentU32FullUpdate(full, (uint64_t)idx, value, &success);
        *seg = SEGMENT_U32_TAG(newFull, SEGMENT_U32_TYPE_FULL);
        break;
    }
    }
}

/* Query range */
uint32_t segmentU32Query(const void *seg, size_t left, size_t right) {
    if (!seg) {
        return (uint32_t)0;
    }

    segmentU32Type type = SEGMENT_U32_TYPE(seg);
    void *ptr = SEGMENT_U32_UNTAG(seg);

    switch (type) {
    case SEGMENT_U32_TYPE_SMALL:
        return segmentU32SmallQuery((const segmentU32Small *)ptr,
                                    (uint32_t)left, (uint32_t)right);
    case SEGMENT_U32_TYPE_FULL:
        return segmentU32FullQuery((const segmentU32Full *)ptr, (uint64_t)left,
                                   (uint64_t)right);
    }

    return (uint32_t)0;
}

/* Get single element */
uint32_t segmentU32Get(const void *seg, size_t idx) {
    if (!seg) {
        return (uint32_t)0;
    }

    segmentU32Type type = SEGMENT_U32_TYPE(seg);
    void *ptr = SEGMENT_U32_UNTAG(seg);

    switch (type) {
    case SEGMENT_U32_TYPE_SMALL:
        return segmentU32SmallGet((const segmentU32Small *)ptr, (uint32_t)idx);
    case SEGMENT_U32_TYPE_FULL:
        return segmentU32FullGet((const segmentU32Full *)ptr, (uint64_t)idx);
    }

    return (uint32_t)0;
}

/* Get count */
size_t segmentU32Count(const void *seg) {
    if (!seg) {
        return 0;
    }

    segmentU32Type type = SEGMENT_U32_TYPE(seg);
    void *ptr = SEGMENT_U32_UNTAG(seg);

    switch (type) {
    case SEGMENT_U32_TYPE_SMALL:
        return segmentU32SmallCount((const segmentU32Small *)ptr);
    case SEGMENT_U32_TYPE_FULL:
        return segmentU32FullCount((const segmentU32Full *)ptr);
    }

    return 0;
}

/* Get bytes */
size_t segmentU32Bytes(const void *seg) {
    if (!seg) {
        return 0;
    }

    segmentU32Type type = SEGMENT_U32_TYPE(seg);
    void *ptr = SEGMENT_U32_UNTAG(seg);

    switch (type) {
    case SEGMENT_U32_TYPE_SMALL:
        return segmentU32SmallBytes((const segmentU32Small *)ptr);
    case SEGMENT_U32_TYPE_FULL:
        return segmentU32FullBytes((const segmentU32Full *)ptr);
    }

    return 0;
}

/* Range update (Full tier only) */
void segmentU32RangeUpdate(void *seg, size_t left, size_t right,
                           uint32_t value) {
    if (!seg) {
        return;
    }

    segmentU32Type type = SEGMENT_U32_TYPE(seg);
    void *ptr = SEGMENT_U32_UNTAG(seg);

    if (type == SEGMENT_U32_TYPE_FULL) {
        segmentU32FullRangeUpdate((segmentU32Full *)ptr, (uint64_t)left,
                                  (uint64_t)right, value);
    }
    /* Small tier: implement point-by-point update */
    else if (type == SEGMENT_U32_TYPE_SMALL) {
        for (size_t i = left; i <= right; i++) {
            segmentU32Update(&seg, i, value);
        }
    }
}

#ifdef DATAKIT_TEST
void segmentU32Repr(const void *seg) {
    if (!seg) {
        printf("segmentU32: (nil)\n");
        return;
    }

    segmentU32Type type = SEGMENT_U32_TYPE(seg);
    void *ptr = SEGMENT_U32_UNTAG(seg);

    const char *tierName = type == SEGMENT_U32_TYPE_SMALL ? "SMALL" : "FULL";
    printf("segmentU32 [tier=%s, count=%zu, bytes=%zu]\n", tierName,
           segmentU32Count(seg), segmentU32Bytes(seg));

    if (type == SEGMENT_U32_TYPE_SMALL) {
        segmentU32SmallRepr((const segmentU32Small *)ptr);
    } else {
        segmentU32FullRepr((const segmentU32Full *)ptr);
    }
}
#endif
