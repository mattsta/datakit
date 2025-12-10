/* ====================================================================
 * Segment Tree Full Tier Implementation Template (2-TIER SYSTEM)
 * ====================================================================
 *
 * Full tier: Unlimited growth with lazy propagation
 * Converts from Small tier, supports range updates efficiently
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

#ifndef SEGMENT_IS_SIGNED
#define SEGMENT_IS_SIGNED 1
#endif

#ifndef SEGMENT_IS_FLOATING
#define SEGMENT_IS_FLOATING 0
#endif

#ifndef SEGMENT_IMPL_SCOPE
#define SEGMENT_IMPL_SCOPE
#endif

/* ====================================================================
 * IDENTITY AND COMBINE HELPERS (duplicated for Full tier)
 * ==================================================================== */

static inline SEGMENT_VALUE_TYPE SEGMENT_NAME(SEGMENT_SUFFIX,
                                              Identity)(segmentOp op) {
    switch (op) {
    case SEGMENT_OP_SUM:
        return (SEGMENT_VALUE_TYPE)(SEGMENT_IS_FLOATING ? 0.0 : 0);
    case SEGMENT_OP_MIN:
        return SEGMENT_TYPE_MAX;
    case SEGMENT_OP_MAX:
        return SEGMENT_TYPE_MIN;
    default:
        return (SEGMENT_VALUE_TYPE)(SEGMENT_IS_FLOATING ? 0.0 : 0);
    }
}

static inline SEGMENT_VALUE_TYPE
SEGMENT_NAME(SEGMENT_SUFFIX, Combine)(SEGMENT_VALUE_TYPE a,
                                      SEGMENT_VALUE_TYPE b, segmentOp op) {
    switch (op) {
    case SEGMENT_OP_SUM:
        return a + b;
    case SEGMENT_OP_MIN:
        return (a < b) ? a : b;
    case SEGMENT_OP_MAX:
        return (a > b) ? a : b;
    default:
        return a + b;
    }
}

/* ====================================================================
 * FULL TIER IMPLEMENTATIONS
 * ==================================================================== */

/* Create new full segment tree */
SEGMENT_IMPL_SCOPE SEGMENT_FULL_TYPENAME(SEGMENT_SUFFIX) *
    SEGMENT_NAME(SEGMENT_SUFFIX, FullNew)(segmentOp op) {
    SEGMENT_FULL_TYPENAME(SEGMENT_SUFFIX) *seg =
        zcalloc(1, sizeof(SEGMENT_FULL_TYPENAME(SEGMENT_SUFFIX)));
    seg->count = 0;
    seg->capacity = 0;
    seg->maxCapacity = (SEGMENT_INDEX_TYPE_FULL)-1;
    seg->operation = op;
    seg->tree = NULL;
    seg->lazy = NULL;
    return seg;
}

/* Build tree helper (same as Small but uses Full structure) */
static void SEGMENT_NAME(SEGMENT_SUFFIX,
                         FullBuild)(SEGMENT_FULL_TYPENAME(SEGMENT_SUFFIX) * seg,
                                    const SEGMENT_VALUE_TYPE *values,
                                    SEGMENT_INDEX_TYPE_FULL n) {
    SEGMENT_INDEX_TYPE_FULL offset = seg->capacity / 2;
    for (SEGMENT_INDEX_TYPE_FULL i = 0; i < n; i++) {
        seg->tree[offset + i] = values[i];
    }

    SEGMENT_VALUE_TYPE identity =
        SEGMENT_NAME(SEGMENT_SUFFIX, Identity)(seg->operation);
    for (SEGMENT_INDEX_TYPE_FULL i = n; i < offset; i++) {
        seg->tree[offset + i] = identity;
    }

    /* Build internal nodes */
    for (int64_t i = (int64_t)offset - 1; i > 0; i--) {
        seg->tree[i] = SEGMENT_NAME(SEGMENT_SUFFIX, Combine)(
            seg->tree[segmentLeft(i)], seg->tree[segmentRight(i)],
            seg->operation);
    }
}

/* Create from array */
SEGMENT_IMPL_SCOPE SEGMENT_FULL_TYPENAME(SEGMENT_SUFFIX) *
    SEGMENT_NAME(SEGMENT_SUFFIX,
                 FullNewFromArray)(const SEGMENT_VALUE_TYPE *values,
                                   SEGMENT_INDEX_TYPE_FULL count,
                                   segmentOp op) {
    if (!values || count == 0) {
        return SEGMENT_NAME(SEGMENT_SUFFIX, FullNew)(op);
    }

    SEGMENT_INDEX_TYPE_FULL capacity = 1;
    while (capacity < count) {
        capacity <<= 1;
    }
    capacity <<= 1;

    SEGMENT_FULL_TYPENAME(SEGMENT_SUFFIX) *seg =
        zcalloc(1, sizeof(SEGMENT_FULL_TYPENAME(SEGMENT_SUFFIX)));
    seg->count = count;
    seg->capacity = capacity;
    seg->maxCapacity = (SEGMENT_INDEX_TYPE_FULL)-1;
    seg->operation = op;
    seg->tree = zcalloc(capacity, sizeof(SEGMENT_VALUE_TYPE));
    seg->lazy = zcalloc(capacity, sizeof(SEGMENT_VALUE_TYPE));

    SEGMENT_NAME(SEGMENT_SUFFIX, FullBuild)(seg, values, count);

    return seg;
}

/* Convert from Small tier */
SEGMENT_IMPL_SCOPE SEGMENT_FULL_TYPENAME(SEGMENT_SUFFIX) *
    SEGMENT_NAME(SEGMENT_SUFFIX,
                 FullFromSmall)(SEGMENT_SMALL_TYPENAME(SEGMENT_SUFFIX) *
                                smallTier) {
    if (!smallTier) {
        return SEGMENT_NAME(SEGMENT_SUFFIX, FullNew)(SEGMENT_OP_SUM);
    }

    /* Extract all values */
    SEGMENT_INDEX_TYPE_SMALL count = smallTier->count;
    SEGMENT_VALUE_TYPE *values = zmalloc(count * sizeof(SEGMENT_VALUE_TYPE));

    for (SEGMENT_INDEX_TYPE_SMALL i = 0; i < count; i++) {
        values[i] = SEGMENT_NAME(SEGMENT_SUFFIX, SmallGet)(smallTier, i);
    }

    /* Create Full from values */
    SEGMENT_FULL_TYPENAME(SEGMENT_SUFFIX) *full = SEGMENT_NAME(
        SEGMENT_SUFFIX, FullNewFromArray)(values, count, smallTier->operation);

    zfree(values);
    return full;
}

/* Free full segment tree */
SEGMENT_IMPL_SCOPE void
SEGMENT_NAME(SEGMENT_SUFFIX,
             FullFree)(SEGMENT_FULL_TYPENAME(SEGMENT_SUFFIX) * seg) {
    if (seg) {
        if (seg->tree) {
            zfree(seg->tree);
        }
        if (seg->lazy) {
            zfree(seg->lazy);
        }
        zfree(seg);
    }
}

/* Update with growth */
SEGMENT_IMPL_SCOPE SEGMENT_FULL_TYPENAME(SEGMENT_SUFFIX) *
    SEGMENT_NAME(SEGMENT_SUFFIX,
                 FullUpdate)(SEGMENT_FULL_TYPENAME(SEGMENT_SUFFIX) * seg,
                             SEGMENT_INDEX_TYPE_FULL idx,
                             SEGMENT_VALUE_TYPE value, bool *success) {
    if (!seg) {
        if (success) {
            *success = false;
        }
        return NULL;
    }

    /* Check if need to grow */
    if (idx >= seg->count) {
        SEGMENT_INDEX_TYPE_FULL newCount = idx + 1;
        SEGMENT_INDEX_TYPE_FULL newCapacity = 1;
        while (newCapacity < newCount) {
            newCapacity <<= 1;
        }
        newCapacity <<= 1;

        /* Reallocate arrays */
        SEGMENT_VALUE_TYPE *newTree =
            zrealloc(seg->tree, newCapacity * sizeof(SEGMENT_VALUE_TYPE));
        SEGMENT_VALUE_TYPE *newLazy =
            zrealloc(seg->lazy, newCapacity * sizeof(SEGMENT_VALUE_TYPE));
        if (!newTree || !newLazy) {
            if (success) {
                *success = false;
            }
            return seg;
        }

        SEGMENT_VALUE_TYPE identity =
            SEGMENT_NAME(SEGMENT_SUFFIX, Identity)(seg->operation);
        SEGMENT_INDEX_TYPE_FULL oldCapacity = seg->capacity;
        seg->tree = newTree;
        seg->lazy = newLazy;
        seg->count = newCount;
        seg->capacity = newCapacity;

        /* Zero new space */
        memset(&seg->tree[oldCapacity], 0,
               (newCapacity - oldCapacity) * sizeof(SEGMENT_VALUE_TYPE));
        memset(&seg->lazy[oldCapacity], 0,
               (newCapacity - oldCapacity) * sizeof(SEGMENT_VALUE_TYPE));

        /* Rebuild tree */
        SEGMENT_INDEX_TYPE_FULL offset = newCapacity / 2;
        for (SEGMENT_INDEX_TYPE_FULL i = 0; i < newCount; i++) {
            if (i < idx && oldCapacity > 0 && i < (oldCapacity / 2)) {
                seg->tree[offset + i] = seg->tree[(oldCapacity / 2) + i];
            } else if (i == idx) {
                seg->tree[offset + i] = value;
            } else {
                seg->tree[offset + i] = identity;
            }
        }

        /* Fill rest with identity */
        for (SEGMENT_INDEX_TYPE_FULL i = newCount; i < (newCapacity / 2); i++) {
            seg->tree[offset + i] = identity;
        }

        /* Rebuild internal nodes */
        for (int64_t i = (int64_t)offset - 1; i > 0; i--) {
            seg->tree[i] = SEGMENT_NAME(SEGMENT_SUFFIX, Combine)(
                seg->tree[segmentLeft(i)], seg->tree[segmentRight(i)],
                seg->operation);
        }
    } else {
        /* Update existing element */
        SEGMENT_INDEX_TYPE_FULL pos = (seg->capacity / 2) + idx;
        seg->tree[pos] = value;

        /* Propagate upward (stop at root = 1) */
        while (pos > 1) {
            pos = segmentParent(pos);
            seg->tree[pos] = SEGMENT_NAME(SEGMENT_SUFFIX, Combine)(
                seg->tree[segmentLeft(pos)], seg->tree[segmentRight(pos)],
                seg->operation);
        }
    }

    if (success) {
        *success = true;
    }
    return seg;
}

/* Query range - recursive */
static SEGMENT_VALUE_TYPE SEGMENT_NAME(SEGMENT_SUFFIX, FullQueryRecursive)(
    const SEGMENT_FULL_TYPENAME(SEGMENT_SUFFIX) * seg,
    SEGMENT_INDEX_TYPE_FULL node, SEGMENT_INDEX_TYPE_FULL nodeLeft,
    SEGMENT_INDEX_TYPE_FULL nodeRight, SEGMENT_INDEX_TYPE_FULL queryLeft,
    SEGMENT_INDEX_TYPE_FULL queryRight) {
    if (queryRight < nodeLeft || queryLeft > nodeRight) {
        return SEGMENT_NAME(SEGMENT_SUFFIX, Identity)(seg->operation);
    }

    if (queryLeft <= nodeLeft && nodeRight <= queryRight) {
        return seg->tree[node];
    }

    SEGMENT_INDEX_TYPE_FULL mid = (nodeLeft + nodeRight) / 2;
    SEGMENT_VALUE_TYPE leftVal =
        SEGMENT_NAME(SEGMENT_SUFFIX, FullQueryRecursive)(
            seg, segmentLeft(node), nodeLeft, mid, queryLeft, queryRight);
    SEGMENT_VALUE_TYPE rightVal =
        SEGMENT_NAME(SEGMENT_SUFFIX, FullQueryRecursive)(
            seg, segmentRight(node), mid + 1, nodeRight, queryLeft, queryRight);

    return SEGMENT_NAME(SEGMENT_SUFFIX, Combine)(leftVal, rightVal,
                                                 seg->operation);
}

/* Query range */
SEGMENT_IMPL_SCOPE SEGMENT_VALUE_TYPE SEGMENT_NAME(SEGMENT_SUFFIX, FullQuery)(
    const SEGMENT_FULL_TYPENAME(SEGMENT_SUFFIX) * seg,
    SEGMENT_INDEX_TYPE_FULL left, SEGMENT_INDEX_TYPE_FULL right) {
    if (!seg || seg->count == 0 || left > right || right >= seg->count) {
        return SEGMENT_NAME(SEGMENT_SUFFIX, Identity)(seg->operation);
    }

    SEGMENT_INDEX_TYPE_FULL n = seg->capacity / 2;
    return SEGMENT_NAME(SEGMENT_SUFFIX, FullQueryRecursive)(seg, 1, 0, n - 1,
                                                            left, right);
}

/* Get single element */
SEGMENT_IMPL_SCOPE SEGMENT_VALUE_TYPE SEGMENT_NAME(SEGMENT_SUFFIX, FullGet)(
    const SEGMENT_FULL_TYPENAME(SEGMENT_SUFFIX) * seg,
    SEGMENT_INDEX_TYPE_FULL idx) {
    if (!seg || idx >= seg->count) {
        return SEGMENT_NAME(SEGMENT_SUFFIX, Identity)(seg->operation);
    }

    return seg->tree[seg->capacity / 2 + idx];
}

/* Get count */
SEGMENT_IMPL_SCOPE SEGMENT_INDEX_TYPE_FULL
SEGMENT_NAME(SEGMENT_SUFFIX,
             FullCount)(const SEGMENT_FULL_TYPENAME(SEGMENT_SUFFIX) * seg) {
    return seg ? seg->count : 0;
}

/* Get bytes */
SEGMENT_IMPL_SCOPE size_t SEGMENT_NAME(SEGMENT_SUFFIX, FullBytes)(
    const SEGMENT_FULL_TYPENAME(SEGMENT_SUFFIX) * seg) {
    if (!seg) {
        return 0;
    }
    return sizeof(SEGMENT_FULL_TYPENAME(SEGMENT_SUFFIX)) +
           seg->capacity * sizeof(SEGMENT_VALUE_TYPE) * 2; /* tree + lazy */
}

/* Range update with lazy propagation (Full tier only) */
SEGMENT_IMPL_SCOPE void SEGMENT_NAME(SEGMENT_SUFFIX, FullRangeUpdate)(
    SEGMENT_FULL_TYPENAME(SEGMENT_SUFFIX) * seg, SEGMENT_INDEX_TYPE_FULL left,
    SEGMENT_INDEX_TYPE_FULL right, SEGMENT_VALUE_TYPE value) {
    if (!seg) {
        return;
    }

    /* For now: simple implementation without lazy propagation
     * TODO: Add full lazy propagation for range updates */
    for (SEGMENT_INDEX_TYPE_FULL i = left; i <= right && i < seg->count; i++) {
        bool success;
        SEGMENT_NAME(SEGMENT_SUFFIX, FullUpdate)(seg, i, value, &success);
    }
}

#ifdef DATAKIT_TEST
SEGMENT_IMPL_SCOPE void
SEGMENT_NAME(SEGMENT_SUFFIX,
             FullRepr)(const SEGMENT_FULL_TYPENAME(SEGMENT_SUFFIX) * seg) {
    if (!seg) {
        printf("SegmentFull: (nil)\n");
        return;
    }

    const char *opName = (seg->operation == SEGMENT_OP_MIN   ? "MIN"
                          : seg->operation == SEGMENT_OP_MAX ? "MAX"
                                                             : "SUM");

    printf("SegmentFull [op=%s, count=%lu, capacity=%lu, bytes=%zu]\n", opName,
           (unsigned long)seg->count, (unsigned long)seg->capacity,
           SEGMENT_NAME(SEGMENT_SUFFIX, FullBytes)(seg));
}
#endif

/* Clean up */
#undef SEGMENT_SUFFIX
#undef SEGMENT_VALUE_TYPE
#undef SEGMENT_INDEX_TYPE_FULL
#undef SEGMENT_TYPE_MIN
#undef SEGMENT_TYPE_MAX
#undef SEGMENT_IS_SIGNED
#undef SEGMENT_IS_FLOATING
#undef SEGMENT_IMPL_SCOPE
