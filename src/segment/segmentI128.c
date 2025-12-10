/* Segment Tree - I128 Dispatcher (2-TIER SYSTEM)
 * Auto-generated tier management and public API.
 */

#include "segmentI128.h"
#include "../datakit.h"

#ifdef __SIZEOF_INT128__

/* Create new segmentI128 */
void *segmentI128New(segmentOp op) {
    segmentI128Small *small = segmentI128SmallNew(op);
    return SEGMENT_I128_TAG(small, SEGMENT_I128_TYPE_SMALL);
}

/* Free segmentI128 */
void segmentI128Free(void *seg) {
    if (!seg) {
        return;
    }

    segmentI128Type type = SEGMENT_I128_TYPE(seg);
    void *ptr = SEGMENT_I128_UNTAG(seg);

    switch (type) {
    case SEGMENT_I128_TYPE_SMALL:
        segmentI128SmallFree((segmentI128Small *)ptr);
        break;
    case SEGMENT_I128_TYPE_FULL:
        segmentI128FullFree((segmentI128Full *)ptr);
        break;
    }
}

/* Upgrade Small to Full */
static void *segmentI128UpgradeSmallToFull(segmentI128Small *small) {
    segmentI128Full *full = segmentI128FullFromSmall(small);
    segmentI128SmallFree(small);
    return SEGMENT_I128_TAG(full, SEGMENT_I128_TYPE_FULL);
}

/* Update with tier transitions */
void segmentI128Update(void **seg, size_t idx, __int128_t value) {
    if (!seg) {
        return;
    }

    if (!*seg) {
        *seg = segmentI128New(SEGMENT_OP_SUM);
    }

    segmentI128Type type = SEGMENT_I128_TYPE(*seg);
    void *ptr = SEGMENT_I128_UNTAG(*seg);

    switch (type) {
    case SEGMENT_I128_TYPE_SMALL: {
        segmentI128Small *small = (segmentI128Small *)ptr;

        /* Check upgrade */
        if (segmentI128SmallShouldUpgrade(small) ||
            idx >= segmentI128SmallCount(small) + 1000) {
            *seg = segmentI128UpgradeSmallToFull(small);
            segmentI128Update(seg, idx, value);
            return;
        }

        bool success;
        segmentI128Small *newSmall =
            segmentI128SmallUpdate(small, (uint32_t)idx, value, &success);
        *seg = SEGMENT_I128_TAG(newSmall, SEGMENT_I128_TYPE_SMALL);
        break;
    }

    case SEGMENT_I128_TYPE_FULL: {
        segmentI128Full *full = (segmentI128Full *)ptr;

        bool success;
        segmentI128Full *newFull =
            segmentI128FullUpdate(full, (uint64_t)idx, value, &success);
        *seg = SEGMENT_I128_TAG(newFull, SEGMENT_I128_TYPE_FULL);
        break;
    }
    }
}

/* Query range */
__int128_t segmentI128Query(const void *seg, size_t left, size_t right) {
    if (!seg) {
        return (__int128_t)0;
    }

    segmentI128Type type = SEGMENT_I128_TYPE(seg);
    void *ptr = SEGMENT_I128_UNTAG(seg);

    switch (type) {
    case SEGMENT_I128_TYPE_SMALL:
        return segmentI128SmallQuery((const segmentI128Small *)ptr,
                                     (uint32_t)left, (uint32_t)right);
    case SEGMENT_I128_TYPE_FULL:
        return segmentI128FullQuery((const segmentI128Full *)ptr,
                                    (uint64_t)left, (uint64_t)right);
    }

    return (__int128_t)0;
}

/* Get single element */
__int128_t segmentI128Get(const void *seg, size_t idx) {
    if (!seg) {
        return (__int128_t)0;
    }

    segmentI128Type type = SEGMENT_I128_TYPE(seg);
    void *ptr = SEGMENT_I128_UNTAG(seg);

    switch (type) {
    case SEGMENT_I128_TYPE_SMALL:
        return segmentI128SmallGet((const segmentI128Small *)ptr,
                                   (uint32_t)idx);
    case SEGMENT_I128_TYPE_FULL:
        return segmentI128FullGet((const segmentI128Full *)ptr, (uint64_t)idx);
    }

    return (__int128_t)0;
}

/* Get count */
size_t segmentI128Count(const void *seg) {
    if (!seg) {
        return 0;
    }

    segmentI128Type type = SEGMENT_I128_TYPE(seg);
    void *ptr = SEGMENT_I128_UNTAG(seg);

    switch (type) {
    case SEGMENT_I128_TYPE_SMALL:
        return segmentI128SmallCount((const segmentI128Small *)ptr);
    case SEGMENT_I128_TYPE_FULL:
        return segmentI128FullCount((const segmentI128Full *)ptr);
    }

    return 0;
}

/* Get bytes */
size_t segmentI128Bytes(const void *seg) {
    if (!seg) {
        return 0;
    }

    segmentI128Type type = SEGMENT_I128_TYPE(seg);
    void *ptr = SEGMENT_I128_UNTAG(seg);

    switch (type) {
    case SEGMENT_I128_TYPE_SMALL:
        return segmentI128SmallBytes((const segmentI128Small *)ptr);
    case SEGMENT_I128_TYPE_FULL:
        return segmentI128FullBytes((const segmentI128Full *)ptr);
    }

    return 0;
}

/* Range update (Full tier only) */
void segmentI128RangeUpdate(void *seg, size_t left, size_t right,
                            __int128_t value) {
    if (!seg) {
        return;
    }

    segmentI128Type type = SEGMENT_I128_TYPE(seg);
    void *ptr = SEGMENT_I128_UNTAG(seg);

    if (type == SEGMENT_I128_TYPE_FULL) {
        segmentI128FullRangeUpdate((segmentI128Full *)ptr, (uint64_t)left,
                                   (uint64_t)right, value);
    }
    /* Small tier: implement point-by-point update */
    else if (type == SEGMENT_I128_TYPE_SMALL) {
        for (size_t i = left; i <= right; i++) {
            segmentI128Update(&seg, i, value);
        }
    }
}

#ifdef DATAKIT_TEST
void segmentI128Repr(const void *seg) {
    if (!seg) {
        printf("segmentI128: (nil)\n");
        return;
    }

    segmentI128Type type = SEGMENT_I128_TYPE(seg);
    void *ptr = SEGMENT_I128_UNTAG(seg);

    const char *tierName = type == SEGMENT_I128_TYPE_SMALL ? "SMALL" : "FULL";
    printf("segmentI128 [tier=%s, count=%zu, bytes=%zu]\n", tierName,
           segmentI128Count(seg), segmentI128Bytes(seg));

    if (type == SEGMENT_I128_TYPE_SMALL) {
        segmentI128SmallRepr((const segmentI128Small *)ptr);
    } else {
        segmentI128FullRepr((const segmentI128Full *)ptr);
    }
}
#endif

#endif /* __SIZEOF_INT128__ */
