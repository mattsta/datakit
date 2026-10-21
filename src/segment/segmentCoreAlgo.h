/* ====================================================================
 * Segment Tree Shared Algorithms (both tiers)
 * ====================================================================
 *
 * Included by segmentCoreImpl.h (Small tier) and segmentCoreFullImpl.h
 * (Full tier) with the instantiation's SEGMENT_SUFFIX,
 * SEGMENT_VALUE_TYPE, SEGMENT_TYPE_MIN, SEGMENT_TYPE_MAX and
 * SEGMENT_IS_FLOATING defined.  Every function is static to the including
 * file.
 *
 * Layout: 'leaves' (a power of two, or 0 for an empty tree) leaves at
 * tree[leaves, 2 * leaves), node i's children at 2i and 2i + 1, the root
 * at 1, tree[0] unused.  Leaves past the count hold the operation's
 * identity, so extending the count within the leaves only sets a leaf.
 *
 * Arithmetic: an integer SUM adds with wrapping (through
 * __builtin_add_overflow, never signed-overflow UB), so a sum is exact
 * whenever it fits the type and modulo 2^bits otherwise. */

#define SEGMENT_ALGO_(name) SEGMENT_NAME(SEGMENT_SUFFIX, name)

/* The operation's identity: 0 for SUM, the type's max for MIN, its min
 * for MAX */
static inline SEGMENT_VALUE_TYPE SEGMENT_ALGO_(Identity_)(segmentOp op) {
    switch (op) {
    case SEGMENT_OP_MIN:
        return SEGMENT_TYPE_MAX;
    case SEGMENT_OP_MAX:
        return SEGMENT_TYPE_MIN;
    case SEGMENT_OP_SUM:
    default:
        return (SEGMENT_VALUE_TYPE)0;
    }
}

static inline SEGMENT_VALUE_TYPE SEGMENT_ALGO_(Combine_)(SEGMENT_VALUE_TYPE a,
                                                         SEGMENT_VALUE_TYPE b,
                                                         segmentOp op) {
    switch (op) {
    case SEGMENT_OP_MIN:
        return a < b ? a : b;
    case SEGMENT_OP_MAX:
        return a > b ? a : b;
    case SEGMENT_OP_SUM:
    default: {
#if SEGMENT_IS_FLOATING
        return a + b;
#else
        SEGMENT_VALUE_TYPE r;
        (void)__builtin_add_overflow(a, b, &r); /* wraps by design */
        return r;
#endif
    }
    }
}

/* Leaves for a tree holding index 'idx': the smallest power of two, at
 * least 'current' (and at least 1), above idx */
static inline uint64_t SEGMENT_ALGO_(LeavesFor_)(uint64_t current,
                                                 uint64_t idx) {
    uint64_t leaves = current == 0 ? 1 : current;
    while (leaves <= idx) {
        leaves <<= 1;
    }

    return leaves;
}

/* Recompute every internal node from the leaves, O(leaves) */
static inline void SEGMENT_ALGO_(Rebuild_)(SEGMENT_VALUE_TYPE *tree,
                                           uint64_t leaves, segmentOp op) {
    for (uint64_t i = leaves - 1; i > 0; i--) {
        tree[i] = SEGMENT_ALGO_(Combine_)(tree[2 * i], tree[2 * i + 1], op);
    }
}

/* Recompute the ancestors of leaves [first, last] (leaf indexes, not
 * tree positions), O(last - first + log n) */
static inline void SEGMENT_ALGO_(Refresh_)(SEGMENT_VALUE_TYPE *tree,
                                           uint64_t leaves, uint64_t first,
                                           uint64_t last, segmentOp op) {
    uint64_t lo = (first + leaves) >> 1;
    uint64_t hi = (last + leaves) >> 1;
    while (lo > 0) {
        for (uint64_t p = lo; p <= hi; p++) {
            tree[p] = SEGMENT_ALGO_(Combine_)(tree[2 * p], tree[2 * p + 1], op);
        }

        lo >>= 1;
        hi >>= 1;
    }
}

/* Move the leaves of a tree just reallocated from 'oldLeaves' to
 * 'newLeaves' leaves (tree holds 2 * newLeaves slots) to their new
 * place, fill the new leaves with the identity and rebuild, O(newLeaves).
 * The new leaf region starts at or after the old tree's end, so the move
 * never overlaps. */
static inline void SEGMENT_ALGO_(Regrow_)(SEGMENT_VALUE_TYPE *tree,
                                          uint64_t oldLeaves,
                                          uint64_t newLeaves, segmentOp op) {
    if (oldLeaves > 0) {
        memcpy(&tree[newLeaves], &tree[oldLeaves],
               oldLeaves * sizeof(SEGMENT_VALUE_TYPE));
    }

    const SEGMENT_VALUE_TYPE identity = SEGMENT_ALGO_(Identity_)(op);
    for (uint64_t i = oldLeaves; i < newLeaves; i++) {
        tree[newLeaves + i] = identity;
    }

    SEGMENT_ALGO_(Rebuild_)(tree, newLeaves, op);
}

/* Fold leaves [left, right] (in range), O(log n).  SUM, MIN and MAX are
 * commutative, so the bottom-up walk may combine in any order. */
static inline SEGMENT_VALUE_TYPE
SEGMENT_ALGO_(Query_)(const SEGMENT_VALUE_TYPE *tree, uint64_t leaves,
                      uint64_t left, uint64_t right, segmentOp op) {
    SEGMENT_VALUE_TYPE result = SEGMENT_ALGO_(Identity_)(op);
    uint64_t lo = left + leaves;
    uint64_t hi = right + leaves + 1;
    while (lo < hi) {
        if (lo & 1) {
            result = SEGMENT_ALGO_(Combine_)(result, tree[lo++], op);
        }

        if (hi & 1) {
            result = SEGMENT_ALGO_(Combine_)(result, tree[--hi], op);
        }

        lo >>= 1;
        hi >>= 1;
    }

    return result;
}

#undef SEGMENT_ALGO_
