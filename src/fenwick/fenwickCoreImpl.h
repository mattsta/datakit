/* ====================================================================
 * Fenwick Tree Core Implementation (2-TIER SYSTEM): Small tier
 * ====================================================================
 *
 * Included once per value type to generate that type's Small tier.
 *
 * Architecture: Small (one contiguous allocation) → Full (separately
 * allocated cells, uint64_t indexes).  Both tiers share the algorithms
 * and arithmetic of fenwickCoreAlgo.h.
 *
 * REQUIRED DEFINES (must be set before including this file):
 *   FENWICK_SUFFIX - Type suffix (I64, U32, Double, etc.)
 *   FENWICK_VALUE_TYPE - C type for values
 *   FENWICK_INDEX_TYPE_SMALL - Index type for Small tier (uint32_t)
 *   FENWICK_INDEX_TYPE_FULL - Index type for Full tier (uint64_t)
 *   FENWICK_SMALL_MAX_COUNT - Max count for Small tier before upgrade
 *   FENWICK_IS_SIGNED - 1 if signed, 0 if unsigned/float
 *   FENWICK_IS_FLOATING - 1 if floating-point, 0 if integer
 *   FENWICK_IMPL_SCOPE - Either 'static inline' or '' (empty for .c file)
 */

#ifndef FENWICK_SUFFIX
#error "FENWICK_SUFFIX must be defined before including fenwickCoreImpl.h"
#endif

#ifndef FENWICK_VALUE_TYPE
#error "FENWICK_VALUE_TYPE must be defined before including fenwickCoreImpl.h"
#endif

/* Set defaults for optional parameters */
#ifndef FENWICK_INDEX_TYPE_SMALL
#define FENWICK_INDEX_TYPE_SMALL uint32_t
#endif

#ifndef FENWICK_INDEX_TYPE_FULL
#define FENWICK_INDEX_TYPE_FULL uint64_t
#endif

#ifndef FENWICK_IS_SIGNED
#define FENWICK_IS_SIGNED 1
#endif

#ifndef FENWICK_IS_FLOATING
#define FENWICK_IS_FLOATING 0
#endif

#ifndef FENWICK_IMPL_SCOPE
#define FENWICK_IMPL_SCOPE
#endif

/* Calculate tier thresholds based on value size if not provided */
#ifndef FENWICK_SMALL_MAX_COUNT
#define FENWICK_SMALL_MAX_COUNT ((128 * 1024) / sizeof(FENWICK_VALUE_TYPE))
#endif

#ifndef FENWICK_SMALL_MAX_BYTES
#define FENWICK_SMALL_MAX_BYTES (128 * 1024)
#endif

#include "fenwickCoreAlgo.h"

#define FENWICK_SMALL_T_ FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX)
#define FENWICK_SMALL_FN_(name) FENWICK_NAME(FENWICK_SUFFIX, name)

/* ====================================================================
 * SMALL TIER IMPLEMENTATIONS
 * ==================================================================== */

/* Create new small Fenwick tree */
FENWICK_IMPL_SCOPE FENWICK_SMALL_T_ *FENWICK_SMALL_FN_(SmallNew)(void) {
    /* @own: caller */
    FENWICK_SMALL_T_ *fw = zcalloc(1, sizeof(FENWICK_SMALL_T_));
    fw->count = 0;
    fw->capacity = 0;
    return fw;
}

/* Create from array in O(n).  NULL values or count 0 give an empty tree;
 * a float tree refuses (NULL) an array holding a non-finite value. */
FENWICK_IMPL_SCOPE FENWICK_SMALL_T_ *
FENWICK_SMALL_FN_(SmallNewFromArray)(const FENWICK_VALUE_TYPE *values,
                                     FENWICK_INDEX_TYPE_SMALL count) {
    if (!values || count == 0) {
        return FENWICK_SMALL_FN_(SmallNew)();
    }

    for (FENWICK_INDEX_TYPE_SMALL i = 0; i < count; i++) {
        if (!FENWICK_SMALL_FN_(AlgoValid_)(values[i])) {
            return NULL;
        }
    }

    const uint64_t capacity = FENWICK_SMALL_FN_(AlgoCapacityFor_)(0, count - 1);
    /* @own: caller */
    FENWICK_SMALL_T_ *fw = zcalloc(
        1, sizeof(FENWICK_SMALL_T_) + capacity * sizeof(FENWICK_VALUE_TYPE));
    fw->count = count;
    fw->capacity = (FENWICK_INDEX_TYPE_SMALL)capacity;
    memcpy(fw->tree, values, count * sizeof(FENWICK_VALUE_TYPE));
    FENWICK_SMALL_FN_(AlgoBuild_)(fw->tree, capacity);
    return fw;
}

/* Free small Fenwick tree */
FENWICK_IMPL_SCOPE void FENWICK_SMALL_FN_(SmallFree)(FENWICK_SMALL_T_ *fw) {
    if (fw) {
        zfree(fw);
    }
}

/* Add 'delta' to element idx, growing (and possibly moving) the tree.
 * The caller has checked the new element.  OOM is fail-fast. */
static FENWICK_SMALL_T_ *
FENWICK_SMALL_FN_(SmallApply_)(FENWICK_SMALL_T_ *fw,
                               FENWICK_INDEX_TYPE_SMALL idx,
                               FENWICK_VALUE_TYPE delta) {
    if (idx >= fw->capacity) {
        const uint64_t oldCapacity = fw->capacity;
        const uint64_t newCapacity =
            FENWICK_SMALL_FN_(AlgoCapacityFor_)(oldCapacity, idx);
        /* @own: caller */
        fw = zrealloc(fw, sizeof(FENWICK_SMALL_T_) +
                              newCapacity * sizeof(FENWICK_VALUE_TYPE));
        memset(&fw->tree[oldCapacity], 0,
               (newCapacity - oldCapacity) * sizeof(FENWICK_VALUE_TYPE));
        FENWICK_SMALL_FN_(AlgoGrow_)(fw->tree, oldCapacity, newCapacity);
        fw->capacity = (FENWICK_INDEX_TYPE_SMALL)newCapacity;
    }

    if (idx >= fw->count) {
        fw->count = idx + 1;
    }

    FENWICK_SMALL_FN_(AlgoAddChain_)(fw->tree, fw->capacity, idx, delta);
    return fw;
}

/* Update: add delta to element at idx.  Refused (*success false, nothing
 * changed) when the new element would not fit the type, or for a float
 * tree when the delta or the new element is not finite. */
FENWICK_IMPL_SCOPE FENWICK_SMALL_T_ *
FENWICK_SMALL_FN_(SmallUpdate)(FENWICK_SMALL_T_ *fw,
                               FENWICK_INDEX_TYPE_SMALL idx,
                               FENWICK_VALUE_TYPE delta, bool *success) {
    FENWICK_VALUE_TYPE element;
    const bool ok = fw && FENWICK_SMALL_FN_(AlgoNewElement_)(
                              fw->tree, fw->count, idx, delta, &element);
    if (success) {
        *success = ok;
    }

    if (!ok) {
        return fw;
    }

    return FENWICK_SMALL_FN_(SmallApply_)(fw, idx, delta);
}

/* Query: compute prefix sum [0, idx] */
FENWICK_IMPL_SCOPE FENWICK_VALUE_TYPE FENWICK_SMALL_FN_(SmallQuery)(
    const FENWICK_SMALL_T_ *fw, FENWICK_INDEX_TYPE_SMALL idx) {
    if (!fw || idx >= fw->count) {
        return 0;
    }

    return FENWICK_SMALL_FN_(AlgoPrefix_)(fw->tree, idx);
}

/* Range query: sum [left, right] */
FENWICK_IMPL_SCOPE FENWICK_VALUE_TYPE FENWICK_SMALL_FN_(SmallRangeQuery)(
    const FENWICK_SMALL_T_ *fw, FENWICK_INDEX_TYPE_SMALL left,
    FENWICK_INDEX_TYPE_SMALL right) {
    if (!fw || left > right || right >= fw->count) {
        return 0;
    }

    const FENWICK_VALUE_TYPE hi =
        FENWICK_SMALL_FN_(AlgoPrefix_)(fw->tree, right);
    if (left == 0) {
        return hi;
    }

    return FENWICK_SMALL_FN_(AlgoSub_)(
        hi, FENWICK_SMALL_FN_(AlgoPrefix_)(fw->tree, left - 1));
}

/* Get single element value, in one O(log n) walk */
FENWICK_IMPL_SCOPE FENWICK_VALUE_TYPE FENWICK_SMALL_FN_(SmallGet)(
    const FENWICK_SMALL_T_ *fw, FENWICK_INDEX_TYPE_SMALL idx) {
    if (!fw || idx >= fw->count) {
        return 0;
    }

    return FENWICK_SMALL_FN_(AlgoElement_)(fw->tree, idx);
}

/* Set single element to exact value.  A float tree refuses a non-finite
 * value, or a change from the current element that is not finite. */
FENWICK_IMPL_SCOPE FENWICK_SMALL_T_ *
FENWICK_SMALL_FN_(SmallSet)(FENWICK_SMALL_T_ *fw, FENWICK_INDEX_TYPE_SMALL idx,
                            FENWICK_VALUE_TYPE value, bool *success) {
    bool ok = fw && FENWICK_SMALL_FN_(AlgoValid_)(value);
    FENWICK_VALUE_TYPE delta = 0;
    if (ok) {
        const FENWICK_VALUE_TYPE current =
            idx < fw->count ? FENWICK_SMALL_FN_(AlgoElement_)(fw->tree, idx)
                            : 0;
        delta = FENWICK_SMALL_FN_(AlgoSub_)(value, current);
        ok = FENWICK_SMALL_FN_(AlgoValid_)(delta);
    }

    if (success) {
        *success = ok;
    }

    if (!ok) {
        return fw;
    }

    return FENWICK_SMALL_FN_(SmallApply_)(fw, idx, delta);
}

/* Get element count */
FENWICK_IMPL_SCOPE FENWICK_INDEX_TYPE_SMALL
FENWICK_SMALL_FN_(SmallCount)(const FENWICK_SMALL_T_ *fw) {
    return fw ? fw->count : 0;
}

/* Get total bytes used */
FENWICK_IMPL_SCOPE size_t
FENWICK_SMALL_FN_(SmallBytes)(const FENWICK_SMALL_T_ *fw) {
    if (!fw) {
        return 0;
    }

    return sizeof(FENWICK_SMALL_T_) + fw->capacity * sizeof(FENWICK_VALUE_TYPE);
}

/* Check if should upgrade to the Full tier */
FENWICK_IMPL_SCOPE bool
FENWICK_SMALL_FN_(SmallShouldUpgrade)(const FENWICK_SMALL_T_ *fw) {
    if (!fw) {
        return false;
    }

    return fw->count > FENWICK_SMALL_MAX_COUNT ||
           FENWICK_SMALL_FN_(SmallBytes)(fw) > FENWICK_SMALL_MAX_BYTES;
}

/* Find smallest index with cumulative sum >= target (non-negative
 * elements); (index type)-1 when none */
FENWICK_IMPL_SCOPE FENWICK_INDEX_TYPE_SMALL FENWICK_SMALL_FN_(SmallLowerBound)(
    const FENWICK_SMALL_T_ *fw, FENWICK_VALUE_TYPE target) {
    if (!fw) {
        return (FENWICK_INDEX_TYPE_SMALL)-1;
    }

    const uint64_t pos =
        FENWICK_SMALL_FN_(AlgoLowerBound_)(fw->tree, fw->count, target);
    return pos == UINT64_MAX ? (FENWICK_INDEX_TYPE_SMALL)-1
                             : (FENWICK_INDEX_TYPE_SMALL)pos;
}

/* Clear all values to zero; count and capacity stay */
FENWICK_IMPL_SCOPE void FENWICK_SMALL_FN_(SmallClear)(FENWICK_SMALL_T_ *fw) {
    if (!fw) {
        return;
    }

    memset(fw->tree, 0, fw->capacity * sizeof(FENWICK_VALUE_TYPE));
}

#ifdef DATAKIT_TEST
/* Print debug representation */
FENWICK_IMPL_SCOPE void
FENWICK_SMALL_FN_(SmallRepr)(const FENWICK_SMALL_T_ *fw) {
    if (!fw) {
        printf("FenwickSmall: (nil)\n");
        return;
    }

    printf("FenwickSmall [count=%u, capacity=%u, bytes=%zu]\n",
           (unsigned)fw->count, (unsigned)fw->capacity,
           FENWICK_SMALL_FN_(SmallBytes)(fw));
}
#endif

#undef FENWICK_SMALL_T_
#undef FENWICK_SMALL_FN_

/* Clean up defines for next instantiation */
#undef FENWICK_SUFFIX
#undef FENWICK_VALUE_TYPE
#undef FENWICK_INDEX_TYPE_SMALL
#undef FENWICK_INDEX_TYPE_FULL
#undef FENWICK_SMALL_MAX_COUNT
#undef FENWICK_SMALL_MAX_BYTES
#undef FENWICK_IS_SIGNED
#undef FENWICK_IS_FLOATING
#undef FENWICK_IMPL_SCOPE
