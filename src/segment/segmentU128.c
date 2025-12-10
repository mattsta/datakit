/* Segment Tree - U128 Dispatcher (2-TIER SYSTEM)
 * Auto-generated tier management and public API.
 */

#include "segmentU128.h"
#include "../datakit.h"

#ifdef __SIZEOF_INT128__

/* Create new segmentU128 */
void *segmentU128New(segmentOp op) {
    segmentU128Small *small = segmentU128SmallNew(op);
    return SEGMENT_U128_TAG(small, SEGMENT_U128_TYPE_SMALL);
}

/* Free segmentU128 */
void segmentU128Free(void *seg) {
    if (!seg) {
        return;
    }

    segmentU128Type type = SEGMENT_U128_TYPE(seg);
    void *ptr = SEGMENT_U128_UNTAG(seg);

    switch (type) {
    case SEGMENT_U128_TYPE_SMALL:
        segmentU128SmallFree((segmentU128Small *)ptr);
        break;
    case SEGMENT_U128_TYPE_FULL:
        segmentU128FullFree((segmentU128Full *)ptr);
        break;
    }
}

/* Upgrade Small to Full */
static void *segmentU128UpgradeSmallToFull(segmentU128Small *small) {
    segmentU128Full *full = segmentU128FullFromSmall(small);
    segmentU128SmallFree(small);
    return SEGMENT_U128_TAG(full, SEGMENT_U128_TYPE_FULL);
}

/* Update with tier transitions */
void segmentU128Update(void **seg, size_t idx, __uint128_t value) {
    if (!seg) {
        return;
    }

    if (!*seg) {
        *seg = segmentU128New(SEGMENT_OP_SUM);
    }

    segmentU128Type type = SEGMENT_U128_TYPE(*seg);
    void *ptr = SEGMENT_U128_UNTAG(*seg);

    switch (type) {
    case SEGMENT_U128_TYPE_SMALL: {
        segmentU128Small *small = (segmentU128Small *)ptr;

        /* Check upgrade */
        if (segmentU128SmallShouldUpgrade(small) ||
            idx >= segmentU128SmallCount(small) + 1000) {
            *seg = segmentU128UpgradeSmallToFull(small);
            segmentU128Update(seg, idx, value);
            return;
        }

        bool success;
        segmentU128Small *newSmall =
            segmentU128SmallUpdate(small, (uint32_t)idx, value, &success);
        *seg = SEGMENT_U128_TAG(newSmall, SEGMENT_U128_TYPE_SMALL);
        break;
    }

    case SEGMENT_U128_TYPE_FULL: {
        segmentU128Full *full = (segmentU128Full *)ptr;

        bool success;
        segmentU128Full *newFull =
            segmentU128FullUpdate(full, (uint64_t)idx, value, &success);
        *seg = SEGMENT_U128_TAG(newFull, SEGMENT_U128_TYPE_FULL);
        break;
    }
    }
}

/* Query range */
__uint128_t segmentU128Query(const void *seg, size_t left, size_t right) {
    if (!seg) {
        return (__uint128_t)0;
    }

    segmentU128Type type = SEGMENT_U128_TYPE(seg);
    void *ptr = SEGMENT_U128_UNTAG(seg);

    switch (type) {
    case SEGMENT_U128_TYPE_SMALL:
        return segmentU128SmallQuery((const segmentU128Small *)ptr,
                                     (uint32_t)left, (uint32_t)right);
    case SEGMENT_U128_TYPE_FULL:
        return segmentU128FullQuery((const segmentU128Full *)ptr,
                                    (uint64_t)left, (uint64_t)right);
    }

    return (__uint128_t)0;
}

/* Get single element */
__uint128_t segmentU128Get(const void *seg, size_t idx) {
    if (!seg) {
        return (__uint128_t)0;
    }

    segmentU128Type type = SEGMENT_U128_TYPE(seg);
    void *ptr = SEGMENT_U128_UNTAG(seg);

    switch (type) {
    case SEGMENT_U128_TYPE_SMALL:
        return segmentU128SmallGet((const segmentU128Small *)ptr,
                                   (uint32_t)idx);
    case SEGMENT_U128_TYPE_FULL:
        return segmentU128FullGet((const segmentU128Full *)ptr, (uint64_t)idx);
    }

    return (__uint128_t)0;
}

/* Get count */
size_t segmentU128Count(const void *seg) {
    if (!seg) {
        return 0;
    }

    segmentU128Type type = SEGMENT_U128_TYPE(seg);
    void *ptr = SEGMENT_U128_UNTAG(seg);

    switch (type) {
    case SEGMENT_U128_TYPE_SMALL:
        return segmentU128SmallCount((const segmentU128Small *)ptr);
    case SEGMENT_U128_TYPE_FULL:
        return segmentU128FullCount((const segmentU128Full *)ptr);
    }

    return 0;
}

/* Get bytes */
size_t segmentU128Bytes(const void *seg) {
    if (!seg) {
        return 0;
    }

    segmentU128Type type = SEGMENT_U128_TYPE(seg);
    void *ptr = SEGMENT_U128_UNTAG(seg);

    switch (type) {
    case SEGMENT_U128_TYPE_SMALL:
        return segmentU128SmallBytes((const segmentU128Small *)ptr);
    case SEGMENT_U128_TYPE_FULL:
        return segmentU128FullBytes((const segmentU128Full *)ptr);
    }

    return 0;
}

/* Range update (Full tier only) */
void segmentU128RangeUpdate(void *seg, size_t left, size_t right,
                            __uint128_t value) {
    if (!seg) {
        return;
    }

    segmentU128Type type = SEGMENT_U128_TYPE(seg);
    void *ptr = SEGMENT_U128_UNTAG(seg);

    if (type == SEGMENT_U128_TYPE_FULL) {
        segmentU128FullRangeUpdate((segmentU128Full *)ptr, (uint64_t)left,
                                   (uint64_t)right, value);
    }
    /* Small tier: implement point-by-point update */
    else if (type == SEGMENT_U128_TYPE_SMALL) {
        for (size_t i = left; i <= right; i++) {
            segmentU128Update(&seg, i, value);
        }
    }
}

#ifdef DATAKIT_TEST
void segmentU128Repr(const void *seg) {
    if (!seg) {
        printf("segmentU128: (nil)\n");
        return;
    }

    segmentU128Type type = SEGMENT_U128_TYPE(seg);
    void *ptr = SEGMENT_U128_UNTAG(seg);

    const char *tierName = type == SEGMENT_U128_TYPE_SMALL ? "SMALL" : "FULL";
    printf("segmentU128 [tier=%s, count=%zu, bytes=%zu]\n", tierName,
           segmentU128Count(seg), segmentU128Bytes(seg));

    if (type == SEGMENT_U128_TYPE_SMALL) {
        segmentU128SmallRepr((const segmentU128Small *)ptr);
    } else {
        segmentU128FullRepr((const segmentU128Full *)ptr);
    }
}
#endif

#endif /* __SIZEOF_INT128__ */
