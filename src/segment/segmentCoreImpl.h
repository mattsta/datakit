/* ====================================================================
 * Segment Tree Small Tier Implementation Template (2-TIER SYSTEM)
 * ====================================================================
 *
 * Small tier: one contiguous allocation (header + tree nodes).  Shares the
 * algorithms and arithmetic of segmentCoreAlgo.h with the Full tier.
 */

#ifndef SEGMENT_SUFFIX
#error "SEGMENT_SUFFIX must be defined before including segmentCoreImpl.h"
#endif

#ifndef SEGMENT_VALUE_TYPE
#error "SEGMENT_VALUE_TYPE must be defined before including segmentCoreImpl.h"
#endif

#ifndef SEGMENT_INDEX_TYPE_SMALL
#define SEGMENT_INDEX_TYPE_SMALL uint32_t
#endif

#ifndef SEGMENT_INDEX_TYPE_FULL
#define SEGMENT_INDEX_TYPE_FULL uint64_t
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

#ifndef SEGMENT_SMALL_MAX_COUNT
#define SEGMENT_SMALL_MAX_COUNT                                                \
    ((256 * 1024) / (sizeof(SEGMENT_VALUE_TYPE) * 2))
#endif

#ifndef SEGMENT_SMALL_MAX_BYTES
#define SEGMENT_SMALL_MAX_BYTES (256 * 1024)
#endif

#include "segmentCoreAlgo.h"

#define SEGMENT_SMALL_T_ SEGMENT_SMALL_TYPENAME(SEGMENT_SUFFIX)
#define SEGMENT_SMALL_FN_(name) SEGMENT_NAME(SEGMENT_SUFFIX, name)

/* capacity is 2 * leaves (tree[0] unused, root at 1) */
#define SEGMENT_SMALL_LEAVES_(seg) ((uint64_t)(seg)->capacity / 2)

/* ====================================================================
 * SMALL TIER IMPLEMENTATIONS
 * ==================================================================== */

/* Create new small segment tree */
SEGMENT_IMPL_SCOPE SEGMENT_SMALL_T_ *SEGMENT_SMALL_FN_(SmallNew)(segmentOp op) {
    /* @own: caller */
    SEGMENT_SMALL_T_ *seg = zcalloc(1, sizeof(SEGMENT_SMALL_T_));
    seg->count = 0;
    seg->capacity = 0;
    seg->operation = op;
    return seg;
}

/* Create from array, O(n) */
SEGMENT_IMPL_SCOPE SEGMENT_SMALL_T_ *
SEGMENT_SMALL_FN_(SmallNewFromArray)(const SEGMENT_VALUE_TYPE *values,
                                     SEGMENT_INDEX_TYPE_SMALL count,
                                     segmentOp op) {
    if (!values || count == 0) {
        return SEGMENT_SMALL_FN_(SmallNew)(op);
    }

    const uint64_t slots = segmentTreeSize(count);
    const uint64_t leaves = slots / 2;
    /* @own: caller */
    SEGMENT_SMALL_T_ *seg = zcalloc(1, sizeof(SEGMENT_SMALL_T_) +
                                           slots * sizeof(SEGMENT_VALUE_TYPE));
    seg->count = count;
    seg->capacity = (SEGMENT_INDEX_TYPE_SMALL)slots;
    seg->operation = op;
    memcpy(&seg->tree[leaves], values, count * sizeof(SEGMENT_VALUE_TYPE));
    const SEGMENT_VALUE_TYPE identity = SEGMENT_SMALL_FN_(Identity_)(op);
    for (uint64_t i = count; i < leaves; i++) {
        seg->tree[leaves + i] = identity;
    }

    SEGMENT_SMALL_FN_(Rebuild_)(seg->tree, leaves, op);
    return seg;
}

/* Free small segment tree */
SEGMENT_IMPL_SCOPE void SEGMENT_SMALL_FN_(SmallFree)(SEGMENT_SMALL_T_ *seg) {
    if (seg) {
        zfree(seg);
    }
}

/* Assign element idx.  An index past the leaves doubles them (amortized
 * O(1) per appended element); within them it only sets the leaf, since
 * leaves past the count already hold the identity.  OOM is fail-fast. */
SEGMENT_IMPL_SCOPE SEGMENT_SMALL_T_ *
SEGMENT_SMALL_FN_(SmallUpdate)(SEGMENT_SMALL_T_ *seg,
                               SEGMENT_INDEX_TYPE_SMALL idx,
                               SEGMENT_VALUE_TYPE value, bool *success) {
    if (!seg) {
        if (success) {
            *success = false;
        }

        return nullptr;
    }

    uint64_t leaves = SEGMENT_SMALL_LEAVES_(seg);
    if (idx >= leaves) {
        const uint64_t newLeaves = SEGMENT_SMALL_FN_(LeavesFor_)(leaves, idx);
        /* @own: caller */
        seg = zrealloc(seg, sizeof(SEGMENT_SMALL_T_) +
                                2 * newLeaves * sizeof(SEGMENT_VALUE_TYPE));
        SEGMENT_SMALL_FN_(Regrow_)(seg->tree, leaves, newLeaves,
                                   seg->operation);
        seg->capacity = (SEGMENT_INDEX_TYPE_SMALL)(2 * newLeaves);
        leaves = newLeaves;
    }

    if (idx >= seg->count) {
        seg->count = idx + 1;
    }

    seg->tree[leaves + idx] = value;
    SEGMENT_SMALL_FN_(Refresh_)(seg->tree, leaves, idx, idx, seg->operation);
    if (success) {
        *success = true;
    }

    return seg;
}

/* Assign 'value' to every existing element in [left, right]; never
 * extends the tree.  O(k + log n). */
SEGMENT_IMPL_SCOPE void SEGMENT_SMALL_FN_(SmallRangeUpdate)(
    SEGMENT_SMALL_T_ *seg, SEGMENT_INDEX_TYPE_SMALL left,
    SEGMENT_INDEX_TYPE_SMALL right, SEGMENT_VALUE_TYPE value) {
    if (!seg || seg->count == 0 || left > right || left >= seg->count) {
        return;
    }

    if (right >= seg->count) {
        right = seg->count - 1;
    }

    const uint64_t leaves = SEGMENT_SMALL_LEAVES_(seg);
    for (uint64_t i = left; i <= right; i++) {
        seg->tree[leaves + i] = value;
    }

    SEGMENT_SMALL_FN_(Refresh_)(seg->tree, leaves, left, right, seg->operation);
}

/* Fold [left, right]; the identity when the range is not in the tree */
SEGMENT_IMPL_SCOPE SEGMENT_VALUE_TYPE SEGMENT_SMALL_FN_(SmallQuery)(
    const SEGMENT_SMALL_T_ *seg, SEGMENT_INDEX_TYPE_SMALL left,
    SEGMENT_INDEX_TYPE_SMALL right) {
    if (!seg) {
        return (SEGMENT_VALUE_TYPE)0;
    }

    if (left > right || right >= seg->count) {
        return SEGMENT_SMALL_FN_(Identity_)(seg->operation);
    }

    return SEGMENT_SMALL_FN_(Query_)(seg->tree, SEGMENT_SMALL_LEAVES_(seg),
                                     left, right, seg->operation);
}

/* Get single element; the identity past the count */
SEGMENT_IMPL_SCOPE SEGMENT_VALUE_TYPE SEGMENT_SMALL_FN_(SmallGet)(
    const SEGMENT_SMALL_T_ *seg, SEGMENT_INDEX_TYPE_SMALL idx) {
    if (!seg) {
        return (SEGMENT_VALUE_TYPE)0;
    }

    if (idx >= seg->count) {
        return SEGMENT_SMALL_FN_(Identity_)(seg->operation);
    }

    return seg->tree[SEGMENT_SMALL_LEAVES_(seg) + idx];
}

/* Get count */
SEGMENT_IMPL_SCOPE SEGMENT_INDEX_TYPE_SMALL
SEGMENT_SMALL_FN_(SmallCount)(const SEGMENT_SMALL_T_ *seg) {
    return seg ? seg->count : 0;
}

/* Get bytes */
SEGMENT_IMPL_SCOPE size_t
SEGMENT_SMALL_FN_(SmallBytes)(const SEGMENT_SMALL_T_ *seg) {
    if (!seg) {
        return 0;
    }

    return sizeof(SEGMENT_SMALL_T_) +
           seg->capacity * sizeof(SEGMENT_VALUE_TYPE);
}

/* Check if should upgrade */
SEGMENT_IMPL_SCOPE bool
SEGMENT_SMALL_FN_(SmallShouldUpgrade)(const SEGMENT_SMALL_T_ *seg) {
    if (!seg) {
        return false;
    }

    return seg->count > SEGMENT_SMALL_MAX_COUNT ||
           SEGMENT_SMALL_FN_(SmallBytes)(seg) > SEGMENT_SMALL_MAX_BYTES;
}

#ifdef DATAKIT_TEST
SEGMENT_IMPL_SCOPE void
SEGMENT_SMALL_FN_(SmallRepr)(const SEGMENT_SMALL_T_ *seg) {
    if (!seg) {
        printf("SegmentSmall: (nil)\n");
        return;
    }

    const char *opName = (seg->operation == SEGMENT_OP_MIN   ? "MIN"
                          : seg->operation == SEGMENT_OP_MAX ? "MAX"
                                                             : "SUM");

    printf("SegmentSmall [op=%s, count=%u, capacity=%u, bytes=%zu]\n", opName,
           (unsigned)seg->count, (unsigned)seg->capacity,
           SEGMENT_SMALL_FN_(SmallBytes)(seg));
}
#endif

#undef SEGMENT_SMALL_T_
#undef SEGMENT_SMALL_FN_
#undef SEGMENT_SMALL_LEAVES_

/* Clean up */
#undef SEGMENT_SUFFIX
#undef SEGMENT_VALUE_TYPE
#undef SEGMENT_INDEX_TYPE_SMALL
#undef SEGMENT_INDEX_TYPE_FULL
#undef SEGMENT_SMALL_MAX_COUNT
#undef SEGMENT_SMALL_MAX_BYTES
#undef SEGMENT_TYPE_MIN
#undef SEGMENT_TYPE_MAX
#undef SEGMENT_IS_SIGNED
#undef SEGMENT_IS_FLOATING
#undef SEGMENT_IMPL_SCOPE
