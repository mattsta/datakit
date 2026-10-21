/* ====================================================================
 * Segment Tree Full Tier Implementation Template (2-TIER SYSTEM)
 * ====================================================================
 *
 * Full tier: separately allocated nodes, uint64_t indexes, unlimited
 * growth (capacity overflow is refused).  Converts from the Small tier by
 * copying its nodes (same layout).  Shares the algorithms and arithmetic
 * of segmentCoreAlgo.h.  The struct's 'lazy' member is not used (range
 * updates assign leaves and refresh their ancestors) and stays NULL.
 */

#ifndef SEGMENT_SUFFIX
#error "SEGMENT_SUFFIX must be defined before including segmentCoreFullImpl.h"
#endif

#ifndef SEGMENT_VALUE_TYPE
#error                                                                         \
    "SEGMENT_VALUE_TYPE must be defined before including segmentCoreFullImpl.h"
#endif

#ifndef SEGMENT_INDEX_TYPE_FULL
#define SEGMENT_INDEX_TYPE_FULL uint64_t
#endif

/* The Small tier's index type is part of the shared define set */
#ifndef SEGMENT_INDEX_TYPE_SMALL
#define SEGMENT_INDEX_TYPE_SMALL uint32_t
#endif

#ifndef SEGMENT_IS_SIGNED
#define SEGMENT_IS_SIGNED 1
#endif

#ifndef SEGMENT_IS_FLOATING
#define SEGMENT_IS_FLOATING 0
#endif

#ifndef SEGMENT_IMPL_SCOPE
#define SEGMENT_IMPL_SCOPE
#endif

#include "segmentCoreAlgo.h"

#define SEGMENT_FULL_T_ SEGMENT_FULL_TYPENAME(SEGMENT_SUFFIX)
#define SEGMENT_FULL_FN_(name) SEGMENT_NAME(SEGMENT_SUFFIX, name)
#define SEGMENT_FULL_LEAVES_(seg) ((uint64_t)(seg)->capacity / 2)

/* ====================================================================
 * FULL TIER IMPLEMENTATIONS
 * ==================================================================== */

/* Create new full segment tree */
SEGMENT_IMPL_SCOPE SEGMENT_FULL_T_ *SEGMENT_FULL_FN_(FullNew)(segmentOp op) {
    /* @own: caller */
    SEGMENT_FULL_T_ *seg = zcalloc(1, sizeof(SEGMENT_FULL_T_));
    seg->count = 0;
    seg->capacity = 0;
    seg->maxCapacity = (SEGMENT_INDEX_TYPE_FULL)-1;
    seg->operation = op;
    seg->tree = NULL;
    seg->lazy = NULL;
    return seg;
}

/* Create from array, O(n) */
SEGMENT_IMPL_SCOPE SEGMENT_FULL_T_ *
SEGMENT_FULL_FN_(FullNewFromArray)(const SEGMENT_VALUE_TYPE *values,
                                   SEGMENT_INDEX_TYPE_FULL count,
                                   segmentOp op) {
    SEGMENT_FULL_T_ *seg = SEGMENT_FULL_FN_(FullNew)(op);
    if (!values || count == 0) {
        return seg;
    }

    const uint64_t leaves = SEGMENT_FULL_FN_(LeavesFor_)(0, count - 1);
    seg->count = count;
    seg->capacity = 2 * leaves;
    /* @own: container */
    seg->tree = zmalloc(2 * leaves * sizeof(SEGMENT_VALUE_TYPE));
    memcpy(&seg->tree[leaves], values, count * sizeof(SEGMENT_VALUE_TYPE));
    const SEGMENT_VALUE_TYPE identity = SEGMENT_FULL_FN_(Identity_)(op);
    for (uint64_t i = count; i < leaves; i++) {
        seg->tree[leaves + i] = identity;
    }

    SEGMENT_FULL_FN_(Rebuild_)(seg->tree, leaves, op);
    return seg;
}

/* Convert from the Small tier: the same layout, so copy the nodes */
SEGMENT_IMPL_SCOPE SEGMENT_FULL_T_ *SEGMENT_FULL_FN_(FullFromSmall)(
    SEGMENT_SMALL_TYPENAME(SEGMENT_SUFFIX) * smallTier) {
    if (!smallTier) {
        return SEGMENT_FULL_FN_(FullNew)(SEGMENT_OP_SUM);
    }

    SEGMENT_FULL_T_ *seg = SEGMENT_FULL_FN_(FullNew)(smallTier->operation);
    if (smallTier->capacity == 0) {
        return seg;
    }

    seg->count = smallTier->count;
    seg->capacity = smallTier->capacity;
    /* @own: container */
    seg->tree = zmalloc(seg->capacity * sizeof(SEGMENT_VALUE_TYPE));
    memcpy(seg->tree, smallTier->tree,
           seg->capacity * sizeof(SEGMENT_VALUE_TYPE));
    return seg;
}

/* Free full segment tree */
SEGMENT_IMPL_SCOPE void SEGMENT_FULL_FN_(FullFree)(SEGMENT_FULL_T_ *seg) {
    if (seg) {
        zfree(seg->tree);
        zfree(seg->lazy);
        zfree(seg);
    }
}

/* Assign element idx.  An index past the leaves doubles them (amortized
 * O(1) per appended element; refused when the capacity would overflow);
 * within them it only sets the leaf.  OOM is fail-fast. */
SEGMENT_IMPL_SCOPE SEGMENT_FULL_T_ *
SEGMENT_FULL_FN_(FullUpdate)(SEGMENT_FULL_T_ *seg, SEGMENT_INDEX_TYPE_FULL idx,
                             SEGMENT_VALUE_TYPE value, bool *success) {
    if (!seg) {
        if (success) {
            *success = false;
        }

        return NULL;
    }

    uint64_t leaves = SEGMENT_FULL_LEAVES_(seg);
    if (idx >= leaves) {
        if (idx > seg->maxCapacity / 4) {
            if (success) {
                *success = false;
            }

            return seg;
        }

        const uint64_t newLeaves = SEGMENT_FULL_FN_(LeavesFor_)(leaves, idx);
        /* @own: container */
        seg->tree =
            zrealloc(seg->tree, 2 * newLeaves * sizeof(SEGMENT_VALUE_TYPE));
        SEGMENT_FULL_FN_(Regrow_)(seg->tree, leaves, newLeaves, seg->operation);
        seg->capacity = 2 * newLeaves;
        leaves = newLeaves;
    }

    if (idx >= seg->count) {
        seg->count = idx + 1;
    }

    seg->tree[leaves + idx] = value;
    SEGMENT_FULL_FN_(Refresh_)(seg->tree, leaves, idx, idx, seg->operation);
    if (success) {
        *success = true;
    }

    return seg;
}

/* Fold [left, right]; the identity when the range is not in the tree */
SEGMENT_IMPL_SCOPE SEGMENT_VALUE_TYPE SEGMENT_FULL_FN_(FullQuery)(
    const SEGMENT_FULL_T_ *seg, SEGMENT_INDEX_TYPE_FULL left,
    SEGMENT_INDEX_TYPE_FULL right) {
    if (!seg) {
        return (SEGMENT_VALUE_TYPE)0;
    }

    if (left > right || right >= seg->count) {
        return SEGMENT_FULL_FN_(Identity_)(seg->operation);
    }

    return SEGMENT_FULL_FN_(Query_)(seg->tree, SEGMENT_FULL_LEAVES_(seg), left,
                                    right, seg->operation);
}

/* Get single element; the identity past the count */
SEGMENT_IMPL_SCOPE SEGMENT_VALUE_TYPE SEGMENT_FULL_FN_(FullGet)(
    const SEGMENT_FULL_T_ *seg, SEGMENT_INDEX_TYPE_FULL idx) {
    if (!seg) {
        return (SEGMENT_VALUE_TYPE)0;
    }

    if (idx >= seg->count) {
        return SEGMENT_FULL_FN_(Identity_)(seg->operation);
    }

    return seg->tree[SEGMENT_FULL_LEAVES_(seg) + idx];
}

/* Get count */
SEGMENT_IMPL_SCOPE SEGMENT_INDEX_TYPE_FULL
SEGMENT_FULL_FN_(FullCount)(const SEGMENT_FULL_T_ *seg) {
    return seg ? seg->count : 0;
}

/* Get bytes */
SEGMENT_IMPL_SCOPE size_t
SEGMENT_FULL_FN_(FullBytes)(const SEGMENT_FULL_T_ *seg) {
    if (!seg) {
        return 0;
    }

    return sizeof(SEGMENT_FULL_T_) + seg->capacity * sizeof(SEGMENT_VALUE_TYPE);
}

/* Assign 'value' to every existing element in [left, right]; never
 * extends the tree.  O(k + log n). */
SEGMENT_IMPL_SCOPE void SEGMENT_FULL_FN_(FullRangeUpdate)(
    SEGMENT_FULL_T_ *seg, SEGMENT_INDEX_TYPE_FULL left,
    SEGMENT_INDEX_TYPE_FULL right, SEGMENT_VALUE_TYPE value) {
    if (!seg || seg->count == 0 || left > right || left >= seg->count) {
        return;
    }

    if (right >= seg->count) {
        right = seg->count - 1;
    }

    const uint64_t leaves = SEGMENT_FULL_LEAVES_(seg);
    for (uint64_t i = left; i <= right; i++) {
        seg->tree[leaves + i] = value;
    }

    SEGMENT_FULL_FN_(Refresh_)(seg->tree, leaves, left, right, seg->operation);
}

#ifdef DATAKIT_TEST
SEGMENT_IMPL_SCOPE void SEGMENT_FULL_FN_(FullRepr)(const SEGMENT_FULL_T_ *seg) {
    if (!seg) {
        printf("SegmentFull: (nil)\n");
        return;
    }

    const char *opName = (seg->operation == SEGMENT_OP_MIN   ? "MIN"
                          : seg->operation == SEGMENT_OP_MAX ? "MAX"
                                                             : "SUM");

    printf("SegmentFull [op=%s, count=%" PRIu64 ", capacity=%" PRIu64
           ", bytes=%zu]\n",
           opName, (uint64_t)seg->count, (uint64_t)seg->capacity,
           SEGMENT_FULL_FN_(FullBytes)(seg));
}
#endif

#undef SEGMENT_FULL_T_
#undef SEGMENT_FULL_FN_
#undef SEGMENT_FULL_LEAVES_

/* Clean up */
#undef SEGMENT_SUFFIX
#undef SEGMENT_VALUE_TYPE
#undef SEGMENT_INDEX_TYPE_FULL
#undef SEGMENT_INDEX_TYPE_SMALL
#undef SEGMENT_TYPE_MIN
#undef SEGMENT_TYPE_MAX
#undef SEGMENT_IS_SIGNED
#undef SEGMENT_IS_FLOATING
#undef SEGMENT_IMPL_SCOPE
