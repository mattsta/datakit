/* ====================================================================
 * Segment Tree Small Tier Implementation Template (2-TIER SYSTEM)
 * ====================================================================
 *
 * Small tier: Eager updates, cache-friendly contiguous allocation
 * Supports: MIN, MAX, SUM operations
 */

#ifndef SEGMENT_SUFFIX
#error "SEGMENT_SUFFIX must be defined before including segmentCoreImpl.h"
#endif

#ifndef SEGMENT_VALUE_TYPE
#error "SEGMENT_VALUE_TYPE must be defined before including segmentCoreImpl.h"
#endif

/* Set defaults */
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

/* ====================================================================
 * IDENTITY AND COMBINE HELPERS
 * ==================================================================== */

/* Get identity value for operation */
SEGMENT_IMPL_SCOPE SEGMENT_VALUE_TYPE SEGMENT_NAME(SEGMENT_SUFFIX,
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

/* Combine two values based on operation */
SEGMENT_IMPL_SCOPE SEGMENT_VALUE_TYPE SEGMENT_NAME(SEGMENT_SUFFIX, Combine)(
    SEGMENT_VALUE_TYPE a, SEGMENT_VALUE_TYPE b, segmentOp op) {
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
 * SMALL TIER IMPLEMENTATIONS
 * ==================================================================== */

/* Create new small segment tree */
SEGMENT_IMPL_SCOPE SEGMENT_SMALL_TYPENAME(SEGMENT_SUFFIX) *
    SEGMENT_NAME(SEGMENT_SUFFIX, SmallNew)(segmentOp op) {
    SEGMENT_SMALL_TYPENAME(SEGMENT_SUFFIX) *seg =
        zcalloc(1, sizeof(SEGMENT_SMALL_TYPENAME(SEGMENT_SUFFIX)));
    seg->count = 0;
    seg->capacity = 0;
    seg->operation = op;
    return seg;
}

/* Build tree from array */
static void SEGMENT_NAME(SEGMENT_SUFFIX, SmallBuild)(
    SEGMENT_SMALL_TYPENAME(SEGMENT_SUFFIX) * seg,
    const SEGMENT_VALUE_TYPE *values, SEGMENT_INDEX_TYPE_SMALL n) {
    /* Copy leaf values to second half of tree array */
    SEGMENT_INDEX_TYPE_SMALL offset = seg->capacity / 2;
    for (SEGMENT_INDEX_TYPE_SMALL i = 0; i < n; i++) {
        seg->tree[offset + i] = values[i];
    }

    /* Fill remaining leaves with identity */
    SEGMENT_VALUE_TYPE identity =
        SEGMENT_NAME(SEGMENT_SUFFIX, Identity)(seg->operation);
    for (SEGMENT_INDEX_TYPE_SMALL i = n; i < offset; i++) {
        seg->tree[offset + i] = identity;
    }

    /* Build internal nodes from bottom up */
    for (int32_t i = (int32_t)offset - 1; i > 0; i--) {
        seg->tree[i] = SEGMENT_NAME(SEGMENT_SUFFIX, Combine)(
            seg->tree[segmentLeft(i)], seg->tree[segmentRight(i)],
            seg->operation);
    }
}

/* Create from array */
SEGMENT_IMPL_SCOPE SEGMENT_SMALL_TYPENAME(SEGMENT_SUFFIX) *
    SEGMENT_NAME(SEGMENT_SUFFIX,
                 SmallNewFromArray)(const SEGMENT_VALUE_TYPE *values,
                                    SEGMENT_INDEX_TYPE_SMALL count,
                                    segmentOp op) {
    if (!values || count == 0) {
        return SEGMENT_NAME(SEGMENT_SUFFIX, SmallNew)(op);
    }

    /* Capacity: power of 2 >= count, then double for full binary tree */
    SEGMENT_INDEX_TYPE_SMALL capacity = 1;
    while (capacity < count) {
        capacity <<= 1;
    }
    capacity <<= 1; /* Double for complete tree */

    size_t size = sizeof(SEGMENT_SMALL_TYPENAME(SEGMENT_SUFFIX)) +
                  capacity * sizeof(SEGMENT_VALUE_TYPE);
    SEGMENT_SMALL_TYPENAME(SEGMENT_SUFFIX) *seg = zcalloc(1, size);
    seg->count = count;
    seg->capacity = capacity;
    seg->operation = op;

    SEGMENT_NAME(SEGMENT_SUFFIX, SmallBuild)(seg, values, count);

    return seg;
}

/* Free small segment tree */
SEGMENT_IMPL_SCOPE void
SEGMENT_NAME(SEGMENT_SUFFIX,
             SmallFree)(SEGMENT_SMALL_TYPENAME(SEGMENT_SUFFIX) * seg) {
    if (seg) {
        zfree(seg);
    }
}

/* Update single element */
SEGMENT_IMPL_SCOPE SEGMENT_SMALL_TYPENAME(SEGMENT_SUFFIX) *
    SEGMENT_NAME(SEGMENT_SUFFIX,
                 SmallUpdate)(SEGMENT_SMALL_TYPENAME(SEGMENT_SUFFIX) * seg,
                              SEGMENT_INDEX_TYPE_SMALL idx,
                              SEGMENT_VALUE_TYPE value, bool *success) {
    if (!seg) {
        if (success) {
            *success = false;
        }
        return NULL;
    }

    /* Ensure capacity */
    if (idx >= seg->count) {
        SEGMENT_INDEX_TYPE_SMALL newCount = idx + 1;
        SEGMENT_INDEX_TYPE_SMALL newCapacity = 1;
        while (newCapacity < newCount) {
            newCapacity <<= 1;
        }
        newCapacity <<= 1; /* Double for full tree */

        size_t newSize = sizeof(SEGMENT_SMALL_TYPENAME(SEGMENT_SUFFIX)) +
                         newCapacity * sizeof(SEGMENT_VALUE_TYPE);

        SEGMENT_SMALL_TYPENAME(SEGMENT_SUFFIX) *newSeg = zrealloc(seg, newSize);
        if (!newSeg) {
            if (success) {
                *success = false;
            }
            return seg;
        }

        /* Initialize new space */
        SEGMENT_VALUE_TYPE identity =
            SEGMENT_NAME(SEGMENT_SUFFIX, Identity)(newSeg->operation);
        SEGMENT_INDEX_TYPE_SMALL oldCapacity = newSeg->capacity;
        newSeg->count = newCount;
        newSeg->capacity = newCapacity;

        /* Zero new tree nodes */
        memset(&newSeg->tree[oldCapacity], 0,
               (newCapacity - oldCapacity) * sizeof(SEGMENT_VALUE_TYPE));

        /* Rebuild tree with new capacity */
        SEGMENT_INDEX_TYPE_SMALL offset = newCapacity / 2;
        for (SEGMENT_INDEX_TYPE_SMALL i = 0; i < newCount; i++) {
            /* Get old value if it exists, or set new value at idx */
            if (i < idx && oldCapacity > 0 && i < (oldCapacity / 2)) {
                newSeg->tree[offset + i] = newSeg->tree[(oldCapacity / 2) + i];
            } else if (i == idx) {
                newSeg->tree[offset + i] = value;
            } else {
                newSeg->tree[offset + i] = identity;
            }
        }

        /* Fill rest of leaves with identity */
        for (SEGMENT_INDEX_TYPE_SMALL i = newCount; i < (newCapacity / 2);
             i++) {
            newSeg->tree[offset + i] = identity;
        }

        /* Rebuild internal nodes from bottom up */
        for (int32_t i = (int32_t)offset - 1; i > 0; i--) {
            newSeg->tree[i] = SEGMENT_NAME(SEGMENT_SUFFIX, Combine)(
                newSeg->tree[segmentLeft(i)], newSeg->tree[segmentRight(i)],
                newSeg->operation);
        }

        seg = newSeg;
    } else {
        /* Update existing element in-place */
        SEGMENT_INDEX_TYPE_SMALL pos = (seg->capacity / 2) + idx;
        seg->tree[pos] = value;

        /* Propagate change up the tree (stop at root which is index 1) */
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

/* Query range [left, right] - recursive helper */
static SEGMENT_VALUE_TYPE SEGMENT_NAME(SEGMENT_SUFFIX, SmallQueryRecursive)(
    const SEGMENT_SMALL_TYPENAME(SEGMENT_SUFFIX) * seg,
    SEGMENT_INDEX_TYPE_SMALL node, SEGMENT_INDEX_TYPE_SMALL nodeLeft,
    SEGMENT_INDEX_TYPE_SMALL nodeRight, SEGMENT_INDEX_TYPE_SMALL queryLeft,
    SEGMENT_INDEX_TYPE_SMALL queryRight) {
    /* No overlap */
    if (queryRight < nodeLeft || queryLeft > nodeRight) {
        return SEGMENT_NAME(SEGMENT_SUFFIX, Identity)(seg->operation);
    }

    /* Complete overlap */
    if (queryLeft <= nodeLeft && nodeRight <= queryRight) {
        return seg->tree[node];
    }

    /* Partial overlap - recurse */
    SEGMENT_INDEX_TYPE_SMALL mid = (nodeLeft + nodeRight) / 2;
    SEGMENT_VALUE_TYPE leftVal =
        SEGMENT_NAME(SEGMENT_SUFFIX, SmallQueryRecursive)(
            seg, segmentLeft(node), nodeLeft, mid, queryLeft, queryRight);
    SEGMENT_VALUE_TYPE rightVal =
        SEGMENT_NAME(SEGMENT_SUFFIX, SmallQueryRecursive)(
            seg, segmentRight(node), mid + 1, nodeRight, queryLeft, queryRight);

    return SEGMENT_NAME(SEGMENT_SUFFIX, Combine)(leftVal, rightVal,
                                                 seg->operation);
}

/* Query range */
SEGMENT_IMPL_SCOPE SEGMENT_VALUE_TYPE SEGMENT_NAME(SEGMENT_SUFFIX, SmallQuery)(
    const SEGMENT_SMALL_TYPENAME(SEGMENT_SUFFIX) * seg,
    SEGMENT_INDEX_TYPE_SMALL left, SEGMENT_INDEX_TYPE_SMALL right) {
    if (!seg || seg->count == 0 || left > right || right >= seg->count) {
        return SEGMENT_NAME(SEGMENT_SUFFIX, Identity)(seg->operation);
    }

    SEGMENT_INDEX_TYPE_SMALL n = seg->capacity / 2;
    return SEGMENT_NAME(SEGMENT_SUFFIX, SmallQueryRecursive)(seg, 1, 0, n - 1,
                                                             left, right);
}

/* Get single element */
SEGMENT_IMPL_SCOPE SEGMENT_VALUE_TYPE SEGMENT_NAME(SEGMENT_SUFFIX, SmallGet)(
    const SEGMENT_SMALL_TYPENAME(SEGMENT_SUFFIX) * seg,
    SEGMENT_INDEX_TYPE_SMALL idx) {
    if (!seg || idx >= seg->count) {
        return SEGMENT_NAME(SEGMENT_SUFFIX, Identity)(seg->operation);
    }

    return seg->tree[seg->capacity / 2 + idx];
}

/* Get count */
SEGMENT_IMPL_SCOPE SEGMENT_INDEX_TYPE_SMALL
SEGMENT_NAME(SEGMENT_SUFFIX,
             SmallCount)(const SEGMENT_SMALL_TYPENAME(SEGMENT_SUFFIX) * seg) {
    return seg ? seg->count : 0;
}

/* Get bytes */
SEGMENT_IMPL_SCOPE size_t SEGMENT_NAME(SEGMENT_SUFFIX, SmallBytes)(
    const SEGMENT_SMALL_TYPENAME(SEGMENT_SUFFIX) * seg) {
    if (!seg) {
        return 0;
    }
    return sizeof(SEGMENT_SMALL_TYPENAME(SEGMENT_SUFFIX)) +
           seg->capacity * sizeof(SEGMENT_VALUE_TYPE);
}

/* Check if should upgrade */
SEGMENT_IMPL_SCOPE bool SEGMENT_NAME(SEGMENT_SUFFIX, SmallShouldUpgrade)(
    const SEGMENT_SMALL_TYPENAME(SEGMENT_SUFFIX) * seg) {
    if (!seg) {
        return false;
    }

    if (seg->count > SEGMENT_SMALL_MAX_COUNT) {
        return true;
    }

    if (SEGMENT_NAME(SEGMENT_SUFFIX, SmallBytes)(seg) >
        SEGMENT_SMALL_MAX_BYTES) {
        return true;
    }

    return false;
}

#ifdef DATAKIT_TEST
SEGMENT_IMPL_SCOPE void
SEGMENT_NAME(SEGMENT_SUFFIX,
             SmallRepr)(const SEGMENT_SMALL_TYPENAME(SEGMENT_SUFFIX) * seg) {
    if (!seg) {
        printf("SegmentSmall: (nil)\n");
        return;
    }

    const char *opName = (seg->operation == SEGMENT_OP_MIN   ? "MIN"
                          : seg->operation == SEGMENT_OP_MAX ? "MAX"
                                                             : "SUM");

    printf("SegmentSmall [op=%s, count=%u, capacity=%u, bytes=%zu]\n", opName,
           (unsigned)seg->count, (unsigned)seg->capacity,
           SEGMENT_NAME(SEGMENT_SUFFIX, SmallBytes)(seg));
}
#endif

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
