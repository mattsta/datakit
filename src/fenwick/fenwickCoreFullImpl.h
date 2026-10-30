/* ====================================================================
 * Fenwick Tree Full Tier Implementation Template (2-TIER SYSTEM)
 * ====================================================================
 *
 * Full tier: {small_max}+ elements, unlimited growth (capacity overflow
 * is refused).  Converts from the Small tier by copying its cells.
 * Shares the algorithms and arithmetic of fenwickCoreAlgo.h.
 *
 * REQUIRED DEFINES: Same as fenwickCoreImpl.h
 */

#ifndef FENWICK_SUFFIX
#error "FENWICK_SUFFIX must be defined before including fenwickCoreFullImpl.h"
#endif

#ifndef FENWICK_VALUE_TYPE
#error                                                                         \
    "FENWICK_VALUE_TYPE must be defined before including fenwickCoreFullImpl.h"
#endif

#ifndef FENWICK_INDEX_TYPE_FULL
#define FENWICK_INDEX_TYPE_FULL uint64_t
#endif

/* The Small tier's index type is part of the shared define set; the Full
 * tier copies cells by count, so it only needs the name to exist */
#ifndef FENWICK_INDEX_TYPE_SMALL
#define FENWICK_INDEX_TYPE_SMALL uint32_t
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

#include "../dkDenseGrowth.h"
#include "fenwickCoreAlgo.h"

#define FENWICK_FULL_T_ FENWICK_FULL_TYPENAME(FENWICK_SUFFIX)
#define FENWICK_FULL_FN_(name) FENWICK_NAME(FENWICK_SUFFIX, name)

/* ====================================================================
 * FULL TIER IMPLEMENTATIONS
 * ==================================================================== */

/* Create new full Fenwick tree */
FENWICK_IMPL_SCOPE FENWICK_FULL_T_ *FENWICK_FULL_FN_(FullNew)(void) {
    /* @own: caller */
    FENWICK_FULL_T_ *fw = zcalloc(1, sizeof(FENWICK_FULL_T_));
    fw->count = 0;
    fw->capacity = 0;
    fw->maxCapacity = (FENWICK_INDEX_TYPE_FULL)-1;
    fw->tree = nullptr;
    return fw;
}

/* Create from array in O(n).  NULL values or count 0 give an empty tree;
 * a float tree refuses (NULL) an array holding a non-finite value. */
FENWICK_IMPL_SCOPE FENWICK_FULL_T_ *
FENWICK_FULL_FN_(FullNewFromArray)(const FENWICK_VALUE_TYPE *values,
                                   FENWICK_INDEX_TYPE_FULL count) {
    if (!values || count == 0) {
        return FENWICK_FULL_FN_(FullNew)();
    }

    for (FENWICK_INDEX_TYPE_FULL i = 0; i < count; i++) {
        if (!FENWICK_FULL_FN_(AlgoValid_)(values[i])) {
            return nullptr;
        }
    }

    FENWICK_FULL_T_ *fw = FENWICK_FULL_FN_(FullNew)();
    fw->count = count;
    fw->capacity = FENWICK_FULL_FN_(AlgoCapacityFor_)(0, count - 1);
    /* @own: container */
    fw->tree = zcalloc(fw->capacity, sizeof(FENWICK_VALUE_TYPE));
    memcpy(fw->tree, values, count * sizeof(FENWICK_VALUE_TYPE));
    FENWICK_FULL_FN_(AlgoBuild_)(fw->tree, fw->capacity);
    return fw;
}

/* Convert from the Small tier: the same capacity means the same cells */
FENWICK_IMPL_SCOPE FENWICK_FULL_T_ *FENWICK_FULL_FN_(FullFromSmall)(
    FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX) * small) {
    FENWICK_FULL_T_ *fw = FENWICK_FULL_FN_(FullNew)();
    if (!small || small->capacity == 0) {
        return fw;
    }

    fw->count = small->count;
    fw->capacity = small->capacity;
    /* @own: container */
    fw->tree = zmalloc(fw->capacity * sizeof(FENWICK_VALUE_TYPE));
    memcpy(fw->tree, small->tree, fw->capacity * sizeof(FENWICK_VALUE_TYPE));
    return fw;
}

/* Free full Fenwick tree */
FENWICK_IMPL_SCOPE void FENWICK_FULL_FN_(FullFree)(FENWICK_FULL_T_ *fw) {
    if (fw) {
        if (fw->tree) {
            zfree(fw->tree);
        }

        zfree(fw);
    }
}

/* Add 'delta' to element idx, growing the cells; false (nothing changed)
 * when the capacity idx needs would overflow.  The caller has checked
 * the new element.  OOM is fail-fast. */
static bool FENWICK_FULL_FN_(FullApply_)(FENWICK_FULL_T_ *fw,
                                         FENWICK_INDEX_TYPE_FULL idx,
                                         FENWICK_VALUE_TYPE delta) {
    if (idx >= fw->capacity) {
        if (idx > fw->maxCapacity / 2) {
            return false;
        }

        const uint64_t oldCapacity = fw->capacity;
        const uint64_t newCapacity =
            FENWICK_FULL_FN_(AlgoCapacityFor_)(oldCapacity, idx);
        if (!dkDenseGrowthAllowed(oldCapacity, newCapacity)) {
            return false;
        }
        /* @own: container */
        fw->tree = zrealloc(fw->tree, newCapacity * sizeof(FENWICK_VALUE_TYPE));
        memset(&fw->tree[oldCapacity], 0,
               (newCapacity - oldCapacity) * sizeof(FENWICK_VALUE_TYPE));
        FENWICK_FULL_FN_(AlgoGrow_)(fw->tree, oldCapacity, newCapacity);
        fw->capacity = newCapacity;
    }

    if (idx >= fw->count) {
        fw->count = idx + 1;
    }

    FENWICK_FULL_FN_(AlgoAddChain_)(fw->tree, fw->capacity, idx, delta);
    return true;
}

/* Update: add delta to element at idx.  Refused (*success false, nothing
 * changed) when the new element would not fit the type (floats: when the
 * delta or the new element is not finite) or the capacity would
 * overflow. */
FENWICK_IMPL_SCOPE FENWICK_FULL_T_ *
FENWICK_FULL_FN_(FullUpdate)(FENWICK_FULL_T_ *fw, FENWICK_INDEX_TYPE_FULL idx,
                             FENWICK_VALUE_TYPE delta, bool *success) {
    FENWICK_VALUE_TYPE element;
    const bool ok = fw &&
                    FENWICK_FULL_FN_(AlgoNewElement_)(fw->tree, fw->count, idx,
                                                      delta, &element) &&
                    FENWICK_FULL_FN_(FullApply_)(fw, idx, delta);
    if (success) {
        *success = ok;
    }

    return fw;
}

/* Query: prefix sum */
FENWICK_IMPL_SCOPE FENWICK_VALUE_TYPE FENWICK_FULL_FN_(FullQuery)(
    const FENWICK_FULL_T_ *fw, FENWICK_INDEX_TYPE_FULL idx) {
    if (!fw || idx >= fw->count) {
        return 0;
    }

    return FENWICK_FULL_FN_(AlgoPrefix_)(fw->tree, idx);
}

/* Range query */
FENWICK_IMPL_SCOPE FENWICK_VALUE_TYPE FENWICK_FULL_FN_(FullRangeQuery)(
    const FENWICK_FULL_T_ *fw, FENWICK_INDEX_TYPE_FULL left,
    FENWICK_INDEX_TYPE_FULL right) {
    if (!fw || left > right || right >= fw->count) {
        return 0;
    }

    const FENWICK_VALUE_TYPE hi =
        FENWICK_FULL_FN_(AlgoPrefix_)(fw->tree, right);
    if (left == 0) {
        return hi;
    }

    return FENWICK_FULL_FN_(AlgoSub_)(
        hi, FENWICK_FULL_FN_(AlgoPrefix_)(fw->tree, left - 1));
}

/* Get single element, in one O(log n) walk */
FENWICK_IMPL_SCOPE FENWICK_VALUE_TYPE FENWICK_FULL_FN_(FullGet)(
    const FENWICK_FULL_T_ *fw, FENWICK_INDEX_TYPE_FULL idx) {
    if (!fw || idx >= fw->count) {
        return 0;
    }

    return FENWICK_FULL_FN_(AlgoElement_)(fw->tree, idx);
}

/* Set single element.  A float tree refuses a non-finite value, or a
 * change from the current element that is not finite. */
FENWICK_IMPL_SCOPE FENWICK_FULL_T_ *
FENWICK_FULL_FN_(FullSet)(FENWICK_FULL_T_ *fw, FENWICK_INDEX_TYPE_FULL idx,
                          FENWICK_VALUE_TYPE value, bool *success) {
    bool ok = fw && FENWICK_FULL_FN_(AlgoValid_)(value);
    if (ok) {
        const FENWICK_VALUE_TYPE current =
            idx < fw->count ? FENWICK_FULL_FN_(AlgoElement_)(fw->tree, idx) : 0;
        const FENWICK_VALUE_TYPE delta =
            FENWICK_FULL_FN_(AlgoSub_)(value, current);
        ok = FENWICK_FULL_FN_(AlgoValid_)(delta) &&
             FENWICK_FULL_FN_(FullApply_)(fw, idx, delta);
    }

    if (success) {
        *success = ok;
    }

    return fw;
}

/* Get count */
FENWICK_IMPL_SCOPE FENWICK_INDEX_TYPE_FULL
FENWICK_FULL_FN_(FullCount)(const FENWICK_FULL_T_ *fw) {
    return fw ? fw->count : 0;
}

/* Get bytes */
FENWICK_IMPL_SCOPE size_t
FENWICK_FULL_FN_(FullBytes)(const FENWICK_FULL_T_ *fw) {
    if (!fw) {
        return 0;
    }

    return sizeof(FENWICK_FULL_T_) + fw->capacity * sizeof(FENWICK_VALUE_TYPE);
}

/* Lower bound (non-negative elements); (index type)-1 when none */
FENWICK_IMPL_SCOPE FENWICK_INDEX_TYPE_FULL FENWICK_FULL_FN_(FullLowerBound)(
    const FENWICK_FULL_T_ *fw, FENWICK_VALUE_TYPE target) {
    if (!fw) {
        return (FENWICK_INDEX_TYPE_FULL)-1;
    }

    const uint64_t pos =
        FENWICK_FULL_FN_(AlgoLowerBound_)(fw->tree, fw->count, target);
    return pos == UINT64_MAX ? (FENWICK_INDEX_TYPE_FULL)-1
                             : (FENWICK_INDEX_TYPE_FULL)pos;
}

/* Clear; count and capacity stay */
FENWICK_IMPL_SCOPE void FENWICK_FULL_FN_(FullClear)(FENWICK_FULL_T_ *fw) {
    if (!fw || !fw->tree) {
        return;
    }

    memset(fw->tree, 0, fw->capacity * sizeof(FENWICK_VALUE_TYPE));
}

#ifdef DATAKIT_TEST
FENWICK_IMPL_SCOPE void FENWICK_FULL_FN_(FullRepr)(const FENWICK_FULL_T_ *fw) {
    if (!fw) {
        printf("FenwickFull: (nil)\n");
        return;
    }

    printf("FenwickFull [count=%" PRIu64 ", capacity=%" PRIu64 ", bytes=%zu]\n",
           (uint64_t)fw->count, (uint64_t)fw->capacity,
           FENWICK_FULL_FN_(FullBytes)(fw));
}
#endif

#undef FENWICK_FULL_T_
#undef FENWICK_FULL_FN_

/* Clean up */
#undef FENWICK_SUFFIX
#undef FENWICK_VALUE_TYPE
#undef FENWICK_INDEX_TYPE_FULL
#undef FENWICK_INDEX_TYPE_SMALL
#undef FENWICK_IS_SIGNED
#undef FENWICK_IS_FLOATING
#undef FENWICK_IMPL_SCOPE
