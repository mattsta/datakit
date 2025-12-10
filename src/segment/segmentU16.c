/* Segment Tree - U16 Dispatcher (2-TIER SYSTEM)
 * Auto-generated tier management and public API.
 */

#include "segmentU16.h"
#include "../datakit.h"

/* Create new segmentU16 */
void *segmentU16New(segmentOp op) {
    segmentU16Small *small = segmentU16SmallNew(op);
    return SEGMENT_U16_TAG(small, SEGMENT_U16_TYPE_SMALL);
}

/* Free segmentU16 */
void segmentU16Free(void *seg) {
    if (!seg) {
        return;
    }

    segmentU16Type type = SEGMENT_U16_TYPE(seg);
    void *ptr = SEGMENT_U16_UNTAG(seg);

    switch (type) {
    case SEGMENT_U16_TYPE_SMALL:
        segmentU16SmallFree((segmentU16Small *)ptr);
        break;
    case SEGMENT_U16_TYPE_FULL:
        segmentU16FullFree((segmentU16Full *)ptr);
        break;
    }
}

/* Upgrade Small to Full */
static void *segmentU16UpgradeSmallToFull(segmentU16Small *small) {
    segmentU16Full *full = segmentU16FullFromSmall(small);
    segmentU16SmallFree(small);
    return SEGMENT_U16_TAG(full, SEGMENT_U16_TYPE_FULL);
}

/* Update with tier transitions */
void segmentU16Update(void **seg, size_t idx, uint16_t value) {
    if (!seg) {
        return;
    }

    if (!*seg) {
        *seg = segmentU16New(SEGMENT_OP_SUM);
    }

    segmentU16Type type = SEGMENT_U16_TYPE(*seg);
    void *ptr = SEGMENT_U16_UNTAG(*seg);

    switch (type) {
    case SEGMENT_U16_TYPE_SMALL: {
        segmentU16Small *small = (segmentU16Small *)ptr;

        /* Check upgrade */
        if (segmentU16SmallShouldUpgrade(small) ||
            idx >= segmentU16SmallCount(small) + 1000) {
            *seg = segmentU16UpgradeSmallToFull(small);
            segmentU16Update(seg, idx, value);
            return;
        }

        bool success;
        segmentU16Small *newSmall =
            segmentU16SmallUpdate(small, (uint32_t)idx, value, &success);
        *seg = SEGMENT_U16_TAG(newSmall, SEGMENT_U16_TYPE_SMALL);
        break;
    }

    case SEGMENT_U16_TYPE_FULL: {
        segmentU16Full *full = (segmentU16Full *)ptr;

        bool success;
        segmentU16Full *newFull =
            segmentU16FullUpdate(full, (uint64_t)idx, value, &success);
        *seg = SEGMENT_U16_TAG(newFull, SEGMENT_U16_TYPE_FULL);
        break;
    }
    }
}

/* Query range */
uint16_t segmentU16Query(const void *seg, size_t left, size_t right) {
    if (!seg) {
        return (uint16_t)0;
    }

    segmentU16Type type = SEGMENT_U16_TYPE(seg);
    void *ptr = SEGMENT_U16_UNTAG(seg);

    switch (type) {
    case SEGMENT_U16_TYPE_SMALL:
        return segmentU16SmallQuery((const segmentU16Small *)ptr,
                                    (uint32_t)left, (uint32_t)right);
    case SEGMENT_U16_TYPE_FULL:
        return segmentU16FullQuery((const segmentU16Full *)ptr, (uint64_t)left,
                                   (uint64_t)right);
    }

    return (uint16_t)0;
}

/* Get single element */
uint16_t segmentU16Get(const void *seg, size_t idx) {
    if (!seg) {
        return (uint16_t)0;
    }

    segmentU16Type type = SEGMENT_U16_TYPE(seg);
    void *ptr = SEGMENT_U16_UNTAG(seg);

    switch (type) {
    case SEGMENT_U16_TYPE_SMALL:
        return segmentU16SmallGet((const segmentU16Small *)ptr, (uint32_t)idx);
    case SEGMENT_U16_TYPE_FULL:
        return segmentU16FullGet((const segmentU16Full *)ptr, (uint64_t)idx);
    }

    return (uint16_t)0;
}

/* Get count */
size_t segmentU16Count(const void *seg) {
    if (!seg) {
        return 0;
    }

    segmentU16Type type = SEGMENT_U16_TYPE(seg);
    void *ptr = SEGMENT_U16_UNTAG(seg);

    switch (type) {
    case SEGMENT_U16_TYPE_SMALL:
        return segmentU16SmallCount((const segmentU16Small *)ptr);
    case SEGMENT_U16_TYPE_FULL:
        return segmentU16FullCount((const segmentU16Full *)ptr);
    }

    return 0;
}

/* Get bytes */
size_t segmentU16Bytes(const void *seg) {
    if (!seg) {
        return 0;
    }

    segmentU16Type type = SEGMENT_U16_TYPE(seg);
    void *ptr = SEGMENT_U16_UNTAG(seg);

    switch (type) {
    case SEGMENT_U16_TYPE_SMALL:
        return segmentU16SmallBytes((const segmentU16Small *)ptr);
    case SEGMENT_U16_TYPE_FULL:
        return segmentU16FullBytes((const segmentU16Full *)ptr);
    }

    return 0;
}

/* Range update (Full tier only) */
void segmentU16RangeUpdate(void *seg, size_t left, size_t right,
                           uint16_t value) {
    if (!seg) {
        return;
    }

    segmentU16Type type = SEGMENT_U16_TYPE(seg);
    void *ptr = SEGMENT_U16_UNTAG(seg);

    if (type == SEGMENT_U16_TYPE_FULL) {
        segmentU16FullRangeUpdate((segmentU16Full *)ptr, (uint64_t)left,
                                  (uint64_t)right, value);
    }
    /* Small tier: implement point-by-point update */
    else if (type == SEGMENT_U16_TYPE_SMALL) {
        for (size_t i = left; i <= right; i++) {
            segmentU16Update(&seg, i, value);
        }
    }
}

#ifdef DATAKIT_TEST
void segmentU16Repr(const void *seg) {
    if (!seg) {
        printf("segmentU16: (nil)\n");
        return;
    }

    segmentU16Type type = SEGMENT_U16_TYPE(seg);
    void *ptr = SEGMENT_U16_UNTAG(seg);

    const char *tierName = type == SEGMENT_U16_TYPE_SMALL ? "SMALL" : "FULL";
    printf("segmentU16 [tier=%s, count=%zu, bytes=%zu]\n", tierName,
           segmentU16Count(seg), segmentU16Bytes(seg));

    if (type == SEGMENT_U16_TYPE_SMALL) {
        segmentU16SmallRepr((const segmentU16Small *)ptr);
    } else {
        segmentU16FullRepr((const segmentU16Full *)ptr);
    }
}
#endif
