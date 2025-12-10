/* Segment Tree - I64 Dispatcher (2-TIER SYSTEM)
 * Auto-generated tier management and public API.
 */

#include "segmentI64.h"
#include "../datakit.h"

/* Create new segmentI64 */
void *segmentI64New(segmentOp op) {
    segmentI64Small *small = segmentI64SmallNew(op);
    return SEGMENT_I64_TAG(small, SEGMENT_I64_TYPE_SMALL);
}

/* Free segmentI64 */
void segmentI64Free(void *seg) {
    if (!seg) {
        return;
    }

    segmentI64Type type = SEGMENT_I64_TYPE(seg);
    void *ptr = SEGMENT_I64_UNTAG(seg);

    switch (type) {
    case SEGMENT_I64_TYPE_SMALL:
        segmentI64SmallFree((segmentI64Small *)ptr);
        break;
    case SEGMENT_I64_TYPE_FULL:
        segmentI64FullFree((segmentI64Full *)ptr);
        break;
    }
}

/* Upgrade Small to Full */
static void *segmentI64UpgradeSmallToFull(segmentI64Small *small) {
    segmentI64Full *full = segmentI64FullFromSmall(small);
    segmentI64SmallFree(small);
    return SEGMENT_I64_TAG(full, SEGMENT_I64_TYPE_FULL);
}

/* Update with tier transitions */
void segmentI64Update(void **seg, size_t idx, int64_t value) {
    if (!seg) {
        return;
    }

    if (!*seg) {
        *seg = segmentI64New(SEGMENT_OP_SUM);
    }

    segmentI64Type type = SEGMENT_I64_TYPE(*seg);
    void *ptr = SEGMENT_I64_UNTAG(*seg);

    switch (type) {
    case SEGMENT_I64_TYPE_SMALL: {
        segmentI64Small *small = (segmentI64Small *)ptr;

        /* Check upgrade */
        if (segmentI64SmallShouldUpgrade(small) ||
            idx >= segmentI64SmallCount(small) + 1000) {
            *seg = segmentI64UpgradeSmallToFull(small);
            segmentI64Update(seg, idx, value);
            return;
        }

        bool success;
        segmentI64Small *newSmall =
            segmentI64SmallUpdate(small, (uint32_t)idx, value, &success);
        *seg = SEGMENT_I64_TAG(newSmall, SEGMENT_I64_TYPE_SMALL);
        break;
    }

    case SEGMENT_I64_TYPE_FULL: {
        segmentI64Full *full = (segmentI64Full *)ptr;

        bool success;
        segmentI64Full *newFull =
            segmentI64FullUpdate(full, (uint64_t)idx, value, &success);
        *seg = SEGMENT_I64_TAG(newFull, SEGMENT_I64_TYPE_FULL);
        break;
    }
    }
}

/* Query range */
int64_t segmentI64Query(const void *seg, size_t left, size_t right) {
    if (!seg) {
        return (int64_t)0;
    }

    segmentI64Type type = SEGMENT_I64_TYPE(seg);
    void *ptr = SEGMENT_I64_UNTAG(seg);

    switch (type) {
    case SEGMENT_I64_TYPE_SMALL:
        return segmentI64SmallQuery((const segmentI64Small *)ptr,
                                    (uint32_t)left, (uint32_t)right);
    case SEGMENT_I64_TYPE_FULL:
        return segmentI64FullQuery((const segmentI64Full *)ptr, (uint64_t)left,
                                   (uint64_t)right);
    }

    return (int64_t)0;
}

/* Get single element */
int64_t segmentI64Get(const void *seg, size_t idx) {
    if (!seg) {
        return (int64_t)0;
    }

    segmentI64Type type = SEGMENT_I64_TYPE(seg);
    void *ptr = SEGMENT_I64_UNTAG(seg);

    switch (type) {
    case SEGMENT_I64_TYPE_SMALL:
        return segmentI64SmallGet((const segmentI64Small *)ptr, (uint32_t)idx);
    case SEGMENT_I64_TYPE_FULL:
        return segmentI64FullGet((const segmentI64Full *)ptr, (uint64_t)idx);
    }

    return (int64_t)0;
}

/* Get count */
size_t segmentI64Count(const void *seg) {
    if (!seg) {
        return 0;
    }

    segmentI64Type type = SEGMENT_I64_TYPE(seg);
    void *ptr = SEGMENT_I64_UNTAG(seg);

    switch (type) {
    case SEGMENT_I64_TYPE_SMALL:
        return segmentI64SmallCount((const segmentI64Small *)ptr);
    case SEGMENT_I64_TYPE_FULL:
        return segmentI64FullCount((const segmentI64Full *)ptr);
    }

    return 0;
}

/* Get bytes */
size_t segmentI64Bytes(const void *seg) {
    if (!seg) {
        return 0;
    }

    segmentI64Type type = SEGMENT_I64_TYPE(seg);
    void *ptr = SEGMENT_I64_UNTAG(seg);

    switch (type) {
    case SEGMENT_I64_TYPE_SMALL:
        return segmentI64SmallBytes((const segmentI64Small *)ptr);
    case SEGMENT_I64_TYPE_FULL:
        return segmentI64FullBytes((const segmentI64Full *)ptr);
    }

    return 0;
}

/* Range update (Full tier only) */
void segmentI64RangeUpdate(void *seg, size_t left, size_t right,
                           int64_t value) {
    if (!seg) {
        return;
    }

    segmentI64Type type = SEGMENT_I64_TYPE(seg);
    void *ptr = SEGMENT_I64_UNTAG(seg);

    if (type == SEGMENT_I64_TYPE_FULL) {
        segmentI64FullRangeUpdate((segmentI64Full *)ptr, (uint64_t)left,
                                  (uint64_t)right, value);
    }
    /* Small tier: implement point-by-point update */
    else if (type == SEGMENT_I64_TYPE_SMALL) {
        for (size_t i = left; i <= right; i++) {
            segmentI64Update(&seg, i, value);
        }
    }
}

#ifdef DATAKIT_TEST
void segmentI64Repr(const void *seg) {
    if (!seg) {
        printf("segmentI64: (nil)\n");
        return;
    }

    segmentI64Type type = SEGMENT_I64_TYPE(seg);
    void *ptr = SEGMENT_I64_UNTAG(seg);

    const char *tierName = type == SEGMENT_I64_TYPE_SMALL ? "SMALL" : "FULL";
    printf("segmentI64 [tier=%s, count=%zu, bytes=%zu]\n", tierName,
           segmentI64Count(seg), segmentI64Bytes(seg));

    if (type == SEGMENT_I64_TYPE_SMALL) {
        segmentI64SmallRepr((const segmentI64Small *)ptr);
    } else {
        segmentI64FullRepr((const segmentI64Full *)ptr);
    }
}
#endif
