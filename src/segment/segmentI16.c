/* Segment Tree - I16 Dispatcher (2-TIER SYSTEM)
 * Auto-generated tier management and public API.
 */

#include "segmentI16.h"
#include "../datakit.h"

/* Create new segmentI16 */
void *segmentI16New(segmentOp op) {
    segmentI16Small *small = segmentI16SmallNew(op);
    return SEGMENT_I16_TAG(small, SEGMENT_I16_TYPE_SMALL);
}

/* Free segmentI16 */
void segmentI16Free(void *seg) {
    if (!seg) {
        return;
    }

    segmentI16Type type = SEGMENT_I16_TYPE(seg);
    void *ptr = SEGMENT_I16_UNTAG(seg);

    switch (type) {
    case SEGMENT_I16_TYPE_SMALL:
        segmentI16SmallFree((segmentI16Small *)ptr);
        break;
    case SEGMENT_I16_TYPE_FULL:
        segmentI16FullFree((segmentI16Full *)ptr);
        break;
    }
}

/* Upgrade Small to Full */
static void *segmentI16UpgradeSmallToFull(segmentI16Small *small) {
    segmentI16Full *full = segmentI16FullFromSmall(small);
    segmentI16SmallFree(small);
    return SEGMENT_I16_TAG(full, SEGMENT_I16_TYPE_FULL);
}

/* Update with tier transitions */
void segmentI16Update(void **seg, size_t idx, int16_t value) {
    if (!seg) {
        return;
    }

    if (!*seg) {
        *seg = segmentI16New(SEGMENT_OP_SUM);
    }

    segmentI16Type type = SEGMENT_I16_TYPE(*seg);
    void *ptr = SEGMENT_I16_UNTAG(*seg);

    switch (type) {
    case SEGMENT_I16_TYPE_SMALL: {
        segmentI16Small *small = (segmentI16Small *)ptr;

        /* Check upgrade */
        if (segmentI16SmallShouldUpgrade(small) ||
            idx >= segmentI16SmallCount(small) + 1000) {
            *seg = segmentI16UpgradeSmallToFull(small);
            segmentI16Update(seg, idx, value);
            return;
        }

        bool success;
        segmentI16Small *newSmall =
            segmentI16SmallUpdate(small, (uint32_t)idx, value, &success);
        *seg = SEGMENT_I16_TAG(newSmall, SEGMENT_I16_TYPE_SMALL);
        break;
    }

    case SEGMENT_I16_TYPE_FULL: {
        segmentI16Full *full = (segmentI16Full *)ptr;

        bool success;
        segmentI16Full *newFull =
            segmentI16FullUpdate(full, (uint64_t)idx, value, &success);
        *seg = SEGMENT_I16_TAG(newFull, SEGMENT_I16_TYPE_FULL);
        break;
    }
    }
}

/* Query range */
int16_t segmentI16Query(const void *seg, size_t left, size_t right) {
    if (!seg) {
        return (int16_t)0;
    }

    segmentI16Type type = SEGMENT_I16_TYPE(seg);
    void *ptr = SEGMENT_I16_UNTAG(seg);

    switch (type) {
    case SEGMENT_I16_TYPE_SMALL:
        return segmentI16SmallQuery((const segmentI16Small *)ptr,
                                    (uint32_t)left, (uint32_t)right);
    case SEGMENT_I16_TYPE_FULL:
        return segmentI16FullQuery((const segmentI16Full *)ptr, (uint64_t)left,
                                   (uint64_t)right);
    }

    return (int16_t)0;
}

/* Get single element */
int16_t segmentI16Get(const void *seg, size_t idx) {
    if (!seg) {
        return (int16_t)0;
    }

    segmentI16Type type = SEGMENT_I16_TYPE(seg);
    void *ptr = SEGMENT_I16_UNTAG(seg);

    switch (type) {
    case SEGMENT_I16_TYPE_SMALL:
        return segmentI16SmallGet((const segmentI16Small *)ptr, (uint32_t)idx);
    case SEGMENT_I16_TYPE_FULL:
        return segmentI16FullGet((const segmentI16Full *)ptr, (uint64_t)idx);
    }

    return (int16_t)0;
}

/* Get count */
size_t segmentI16Count(const void *seg) {
    if (!seg) {
        return 0;
    }

    segmentI16Type type = SEGMENT_I16_TYPE(seg);
    void *ptr = SEGMENT_I16_UNTAG(seg);

    switch (type) {
    case SEGMENT_I16_TYPE_SMALL:
        return segmentI16SmallCount((const segmentI16Small *)ptr);
    case SEGMENT_I16_TYPE_FULL:
        return segmentI16FullCount((const segmentI16Full *)ptr);
    }

    return 0;
}

/* Get bytes */
size_t segmentI16Bytes(const void *seg) {
    if (!seg) {
        return 0;
    }

    segmentI16Type type = SEGMENT_I16_TYPE(seg);
    void *ptr = SEGMENT_I16_UNTAG(seg);

    switch (type) {
    case SEGMENT_I16_TYPE_SMALL:
        return segmentI16SmallBytes((const segmentI16Small *)ptr);
    case SEGMENT_I16_TYPE_FULL:
        return segmentI16FullBytes((const segmentI16Full *)ptr);
    }

    return 0;
}

/* Range update (Full tier only) */
void segmentI16RangeUpdate(void *seg, size_t left, size_t right,
                           int16_t value) {
    if (!seg) {
        return;
    }

    segmentI16Type type = SEGMENT_I16_TYPE(seg);
    void *ptr = SEGMENT_I16_UNTAG(seg);

    if (type == SEGMENT_I16_TYPE_FULL) {
        segmentI16FullRangeUpdate((segmentI16Full *)ptr, (uint64_t)left,
                                  (uint64_t)right, value);
    }
    /* Small tier: implement point-by-point update */
    else if (type == SEGMENT_I16_TYPE_SMALL) {
        for (size_t i = left; i <= right; i++) {
            segmentI16Update(&seg, i, value);
        }
    }
}

#ifdef DATAKIT_TEST
void segmentI16Repr(const void *seg) {
    if (!seg) {
        printf("segmentI16: (nil)\n");
        return;
    }

    segmentI16Type type = SEGMENT_I16_TYPE(seg);
    void *ptr = SEGMENT_I16_UNTAG(seg);

    const char *tierName = type == SEGMENT_I16_TYPE_SMALL ? "SMALL" : "FULL";
    printf("segmentI16 [tier=%s, count=%zu, bytes=%zu]\n", tierName,
           segmentI16Count(seg), segmentI16Bytes(seg));

    if (type == SEGMENT_I16_TYPE_SMALL) {
        segmentI16SmallRepr((const segmentI16Small *)ptr);
    } else {
        segmentI16FullRepr((const segmentI16Full *)ptr);
    }
}
#endif
