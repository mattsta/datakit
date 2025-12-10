/* Segment Tree - U64 Dispatcher (2-TIER SYSTEM)
 * Auto-generated tier management and public API.
 */

#include "segmentU64.h"
#include "../datakit.h"

/* Create new segmentU64 */
void *segmentU64New(segmentOp op) {
    segmentU64Small *small = segmentU64SmallNew(op);
    return SEGMENT_U64_TAG(small, SEGMENT_U64_TYPE_SMALL);
}

/* Free segmentU64 */
void segmentU64Free(void *seg) {
    if (!seg) {
        return;
    }

    segmentU64Type type = SEGMENT_U64_TYPE(seg);
    void *ptr = SEGMENT_U64_UNTAG(seg);

    switch (type) {
    case SEGMENT_U64_TYPE_SMALL:
        segmentU64SmallFree((segmentU64Small *)ptr);
        break;
    case SEGMENT_U64_TYPE_FULL:
        segmentU64FullFree((segmentU64Full *)ptr);
        break;
    }
}

/* Upgrade Small to Full */
static void *segmentU64UpgradeSmallToFull(segmentU64Small *small) {
    segmentU64Full *full = segmentU64FullFromSmall(small);
    segmentU64SmallFree(small);
    return SEGMENT_U64_TAG(full, SEGMENT_U64_TYPE_FULL);
}

/* Update with tier transitions */
void segmentU64Update(void **seg, size_t idx, uint64_t value) {
    if (!seg) {
        return;
    }

    if (!*seg) {
        *seg = segmentU64New(SEGMENT_OP_SUM);
    }

    segmentU64Type type = SEGMENT_U64_TYPE(*seg);
    void *ptr = SEGMENT_U64_UNTAG(*seg);

    switch (type) {
    case SEGMENT_U64_TYPE_SMALL: {
        segmentU64Small *small = (segmentU64Small *)ptr;

        /* Check upgrade */
        if (segmentU64SmallShouldUpgrade(small) ||
            idx >= segmentU64SmallCount(small) + 1000) {
            *seg = segmentU64UpgradeSmallToFull(small);
            segmentU64Update(seg, idx, value);
            return;
        }

        bool success;
        segmentU64Small *newSmall =
            segmentU64SmallUpdate(small, (uint32_t)idx, value, &success);
        *seg = SEGMENT_U64_TAG(newSmall, SEGMENT_U64_TYPE_SMALL);
        break;
    }

    case SEGMENT_U64_TYPE_FULL: {
        segmentU64Full *full = (segmentU64Full *)ptr;

        bool success;
        segmentU64Full *newFull =
            segmentU64FullUpdate(full, (uint64_t)idx, value, &success);
        *seg = SEGMENT_U64_TAG(newFull, SEGMENT_U64_TYPE_FULL);
        break;
    }
    }
}

/* Query range */
uint64_t segmentU64Query(const void *seg, size_t left, size_t right) {
    if (!seg) {
        return (uint64_t)0;
    }

    segmentU64Type type = SEGMENT_U64_TYPE(seg);
    void *ptr = SEGMENT_U64_UNTAG(seg);

    switch (type) {
    case SEGMENT_U64_TYPE_SMALL:
        return segmentU64SmallQuery((const segmentU64Small *)ptr,
                                    (uint32_t)left, (uint32_t)right);
    case SEGMENT_U64_TYPE_FULL:
        return segmentU64FullQuery((const segmentU64Full *)ptr, (uint64_t)left,
                                   (uint64_t)right);
    }

    return (uint64_t)0;
}

/* Get single element */
uint64_t segmentU64Get(const void *seg, size_t idx) {
    if (!seg) {
        return (uint64_t)0;
    }

    segmentU64Type type = SEGMENT_U64_TYPE(seg);
    void *ptr = SEGMENT_U64_UNTAG(seg);

    switch (type) {
    case SEGMENT_U64_TYPE_SMALL:
        return segmentU64SmallGet((const segmentU64Small *)ptr, (uint32_t)idx);
    case SEGMENT_U64_TYPE_FULL:
        return segmentU64FullGet((const segmentU64Full *)ptr, (uint64_t)idx);
    }

    return (uint64_t)0;
}

/* Get count */
size_t segmentU64Count(const void *seg) {
    if (!seg) {
        return 0;
    }

    segmentU64Type type = SEGMENT_U64_TYPE(seg);
    void *ptr = SEGMENT_U64_UNTAG(seg);

    switch (type) {
    case SEGMENT_U64_TYPE_SMALL:
        return segmentU64SmallCount((const segmentU64Small *)ptr);
    case SEGMENT_U64_TYPE_FULL:
        return segmentU64FullCount((const segmentU64Full *)ptr);
    }

    return 0;
}

/* Get bytes */
size_t segmentU64Bytes(const void *seg) {
    if (!seg) {
        return 0;
    }

    segmentU64Type type = SEGMENT_U64_TYPE(seg);
    void *ptr = SEGMENT_U64_UNTAG(seg);

    switch (type) {
    case SEGMENT_U64_TYPE_SMALL:
        return segmentU64SmallBytes((const segmentU64Small *)ptr);
    case SEGMENT_U64_TYPE_FULL:
        return segmentU64FullBytes((const segmentU64Full *)ptr);
    }

    return 0;
}

/* Range update (Full tier only) */
void segmentU64RangeUpdate(void *seg, size_t left, size_t right,
                           uint64_t value) {
    if (!seg) {
        return;
    }

    segmentU64Type type = SEGMENT_U64_TYPE(seg);
    void *ptr = SEGMENT_U64_UNTAG(seg);

    if (type == SEGMENT_U64_TYPE_FULL) {
        segmentU64FullRangeUpdate((segmentU64Full *)ptr, (uint64_t)left,
                                  (uint64_t)right, value);
    }
    /* Small tier: implement point-by-point update */
    else if (type == SEGMENT_U64_TYPE_SMALL) {
        for (size_t i = left; i <= right; i++) {
            segmentU64Update(&seg, i, value);
        }
    }
}

#ifdef DATAKIT_TEST
void segmentU64Repr(const void *seg) {
    if (!seg) {
        printf("segmentU64: (nil)\n");
        return;
    }

    segmentU64Type type = SEGMENT_U64_TYPE(seg);
    void *ptr = SEGMENT_U64_UNTAG(seg);

    const char *tierName = type == SEGMENT_U64_TYPE_SMALL ? "SMALL" : "FULL";
    printf("segmentU64 [tier=%s, count=%zu, bytes=%zu]\n", tierName,
           segmentU64Count(seg), segmentU64Bytes(seg));

    if (type == SEGMENT_U64_TYPE_SMALL) {
        segmentU64SmallRepr((const segmentU64Small *)ptr);
    } else {
        segmentU64FullRepr((const segmentU64Full *)ptr);
    }
}
#endif
