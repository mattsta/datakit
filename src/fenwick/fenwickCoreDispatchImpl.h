/* ====================================================================
 * Fenwick Tree Dispatcher Template (2-TIER SYSTEM): the public API
 * ====================================================================
 *
 * Included once per value type (fenwick<Suffix>.c) to generate the public
 * functions declared in fenwick<Suffix>.h.  A tree is a tagged pointer to
 * its Small tier (one contiguous allocation) or, past the Small tier's
 * limits, its Full tier.
 *
 * REQUIRED DEFINES:
 *   FENWICK_SUFFIX     - Type suffix (I64, U32, Double, etc.)
 *   FENWICK_VALUE_TYPE - C type for values
 *   FENWICK_IS_FLOATING - 1 for float types, 0 for integers
 *
 * Contract (see fenwickCore.h, "VALUES"): Update and Set refuse an
 * element that would not fit the type (float trees: a non-finite delta,
 * value or element) and then change nothing (a NULL tree stays NULL);
 * Query, RangeQuery and Get past the count return zero. */

#include <math.h>

#define FENWICK_D_FN_(name) FENWICK_NAME(FENWICK_SUFFIX, name)
#define FENWICK_D_STR2_(x) #x
#define FENWICK_D_STR_(x) FENWICK_D_STR2_(x)
#define FENWICK_D_SMALL_ FENWICK_SMALL_TYPENAME(FENWICK_SUFFIX)
#define FENWICK_D_FULL_ FENWICK_FULL_TYPENAME(FENWICK_SUFFIX)

/* Every type tags Small as 1 and Full as 2 in the low two bits */
#define FENWICK_D_TAG_SMALL_ ((uintptr_t)1)
#define FENWICK_D_TAG_FULL_ ((uintptr_t)2)
#define FENWICK_D_TAG_MASK_ ((uintptr_t)3)
#define FENWICK_D_IS_SMALL_(fw)                                                \
    (((uintptr_t)(fw) & FENWICK_D_TAG_MASK_) == FENWICK_D_TAG_SMALL_)
#define FENWICK_D_UNTAG_(fw) ((void *)((uintptr_t)(fw) & ~FENWICK_D_TAG_MASK_))
#define FENWICK_D_TAG_(ptr, tag) ((void *)((uintptr_t)(ptr) | (tag)))

/* Create new tree - starts as small tier */
void *FENWICK_D_FN_(New)(void) {
    return FENWICK_D_TAG_(FENWICK_D_FN_(SmallNew)(), FENWICK_D_TAG_SMALL_);
}

void FENWICK_D_FN_(Free)(void *fw) {
    if (!fw) {
        return;
    }

    if (FENWICK_D_IS_SMALL_(fw)) {
        FENWICK_D_FN_(SmallFree)((FENWICK_D_SMALL_ *)FENWICK_D_UNTAG_(fw));
    } else {
        FENWICK_D_FN_(FullFree)((FENWICK_D_FULL_ *)FENWICK_D_UNTAG_(fw));
    }
}

/* A write at idx: moves a Small tree to the Full tier first when the
 * Small tier's limits would be passed or idx jumps far past the end, then
 * applies Update (isSet false) or Set.  A tree created here for a refused
 * write is freed again. */
static bool FENWICK_D_FN_(Write_)(void **fw, size_t idx, FENWICK_VALUE_TYPE v,
                                  bool isSet) {
    if (!fw) {
        return false;
    }

#if FENWICK_IS_FLOATING
    /* A non-finite delta or value is always refused: say so before
     * creating or upgrading anything */
    if (!isfinite(v)) {
        return false;
    }
#endif

    const bool created = !*fw;
    if (created) {
        *fw = FENWICK_D_FN_(New)();
    }

    if (FENWICK_D_IS_SMALL_(*fw)) {
        FENWICK_D_SMALL_ *small = FENWICK_D_UNTAG_(*fw);
        if (FENWICK_D_FN_(SmallShouldUpgrade)(small) ||
            idx >= (size_t)FENWICK_D_FN_(SmallCount)(small) + 1000 ||
            idx * sizeof(FENWICK_VALUE_TYPE) > 128 * 1024) {
            FENWICK_D_FULL_ *full = FENWICK_D_FN_(FullFromSmall)(small);
            FENWICK_D_FN_(SmallFree)(small);
            *fw = FENWICK_D_TAG_(full, FENWICK_D_TAG_FULL_);
        }
    }

    bool success = false;
    if (FENWICK_D_IS_SMALL_(*fw)) {
        /* idx < 128 KiB / sizeof(value) here, so it fits uint32_t */
        FENWICK_D_SMALL_ *small = FENWICK_D_UNTAG_(*fw);
        small =
            isSet
                ? FENWICK_D_FN_(SmallSet)(small, (uint32_t)idx, v, &success)
                : FENWICK_D_FN_(SmallUpdate)(small, (uint32_t)idx, v, &success);
        *fw = FENWICK_D_TAG_(small, FENWICK_D_TAG_SMALL_);
    } else {
        FENWICK_D_FULL_ *full = FENWICK_D_UNTAG_(*fw);
        if (isSet) {
            FENWICK_D_FN_(FullSet)(full, (uint64_t)idx, v, &success);
        } else {
            FENWICK_D_FN_(FullUpdate)(full, (uint64_t)idx, v, &success);
        }
    }

    if (!success && created) {
        FENWICK_D_FN_(Free)(*fw);
        *fw = NULL;
    }

    return success;
}

bool FENWICK_D_FN_(Update)(void **fw, size_t idx, FENWICK_VALUE_TYPE delta) {
    return FENWICK_D_FN_(Write_)(fw, idx, delta, false);
}

bool FENWICK_D_FN_(Set)(void **fw, size_t idx, FENWICK_VALUE_TYPE value) {
    return FENWICK_D_FN_(Write_)(fw, idx, value, true);
}

size_t FENWICK_D_FN_(Count)(const void *fw) {
    if (!fw) {
        return 0;
    }

    if (FENWICK_D_IS_SMALL_(fw)) {
        return FENWICK_D_FN_(SmallCount)(
            (const FENWICK_D_SMALL_ *)FENWICK_D_UNTAG_(fw));
    }

    return (size_t)FENWICK_D_FN_(FullCount)(
        (const FENWICK_D_FULL_ *)FENWICK_D_UNTAG_(fw));
}

/* Query prefix sum [0, idx] */
FENWICK_VALUE_TYPE FENWICK_D_FN_(Query)(const void *fw, size_t idx) {
    if (idx >= FENWICK_D_FN_(Count)(fw)) {
        return 0;
    }

    if (FENWICK_D_IS_SMALL_(fw)) {
        return FENWICK_D_FN_(SmallQuery)(
            (const FENWICK_D_SMALL_ *)FENWICK_D_UNTAG_(fw), (uint32_t)idx);
    }

    return FENWICK_D_FN_(FullQuery)(
        (const FENWICK_D_FULL_ *)FENWICK_D_UNTAG_(fw), (uint64_t)idx);
}

/* Range query [left, right] */
FENWICK_VALUE_TYPE FENWICK_D_FN_(RangeQuery)(const void *fw, size_t left,
                                             size_t right) {
    if (left > right || right >= FENWICK_D_FN_(Count)(fw)) {
        return 0;
    }

    if (FENWICK_D_IS_SMALL_(fw)) {
        return FENWICK_D_FN_(SmallRangeQuery)(
            (const FENWICK_D_SMALL_ *)FENWICK_D_UNTAG_(fw), (uint32_t)left,
            (uint32_t)right);
    }

    return FENWICK_D_FN_(FullRangeQuery)(
        (const FENWICK_D_FULL_ *)FENWICK_D_UNTAG_(fw), (uint64_t)left,
        (uint64_t)right);
}

/* Get single element */
FENWICK_VALUE_TYPE FENWICK_D_FN_(Get)(const void *fw, size_t idx) {
    if (idx >= FENWICK_D_FN_(Count)(fw)) {
        return 0;
    }

    if (FENWICK_D_IS_SMALL_(fw)) {
        return FENWICK_D_FN_(SmallGet)(
            (const FENWICK_D_SMALL_ *)FENWICK_D_UNTAG_(fw), (uint32_t)idx);
    }

    return FENWICK_D_FN_(FullGet)((const FENWICK_D_FULL_ *)FENWICK_D_UNTAG_(fw),
                                  (uint64_t)idx);
}

size_t FENWICK_D_FN_(Bytes)(const void *fw) {
    if (!fw) {
        return 0;
    }

    if (FENWICK_D_IS_SMALL_(fw)) {
        return FENWICK_D_FN_(SmallBytes)(
            (const FENWICK_D_SMALL_ *)FENWICK_D_UNTAG_(fw));
    }

    return FENWICK_D_FN_(FullBytes)(
        (const FENWICK_D_FULL_ *)FENWICK_D_UNTAG_(fw));
}

/* Smallest index whose prefix sum reaches 'target' (non-negative
 * elements); SIZE_MAX when none does */
size_t FENWICK_D_FN_(LowerBound)(const void *fw, FENWICK_VALUE_TYPE target) {
    if (!fw) {
        return SIZE_MAX;
    }

    if (FENWICK_D_IS_SMALL_(fw)) {
        const uint32_t pos = FENWICK_D_FN_(SmallLowerBound)(
            (const FENWICK_D_SMALL_ *)FENWICK_D_UNTAG_(fw), target);
        return pos == (uint32_t)-1 ? SIZE_MAX : (size_t)pos;
    }

    const uint64_t pos = FENWICK_D_FN_(FullLowerBound)(
        (const FENWICK_D_FULL_ *)FENWICK_D_UNTAG_(fw), target);
    return pos == (uint64_t)-1 ? SIZE_MAX : (size_t)pos;
}

void FENWICK_D_FN_(Clear)(void *fw) {
    if (!fw) {
        return;
    }

    if (FENWICK_D_IS_SMALL_(fw)) {
        FENWICK_D_FN_(SmallClear)((FENWICK_D_SMALL_ *)FENWICK_D_UNTAG_(fw));
    } else {
        FENWICK_D_FN_(FullClear)((FENWICK_D_FULL_ *)FENWICK_D_UNTAG_(fw));
    }
}

#ifdef DATAKIT_TEST
void FENWICK_D_FN_(Repr)(const void *fw) {
    if (!fw) {
        printf("fenwick" FENWICK_D_STR_(FENWICK_SUFFIX) ": (nil)\n");
        return;
    }

    const bool small = FENWICK_D_IS_SMALL_(fw);
    if (small) {
        FENWICK_D_FN_(SmallRepr)(
            (const FENWICK_D_SMALL_ *)FENWICK_D_UNTAG_(fw));
    } else {
        FENWICK_D_FN_(FullRepr)((const FENWICK_D_FULL_ *)FENWICK_D_UNTAG_(fw));
    }

    printf("  Tier: %s, Count: %zu, Bytes: %zu\n", small ? "SMALL" : "FULL",
           FENWICK_D_FN_(Count)(fw), FENWICK_D_FN_(Bytes)(fw));
}
#endif

#undef FENWICK_D_FN_
#undef FENWICK_D_STR2_
#undef FENWICK_D_STR_
#undef FENWICK_D_SMALL_
#undef FENWICK_D_FULL_
#undef FENWICK_D_TAG_SMALL_
#undef FENWICK_D_TAG_FULL_
#undef FENWICK_D_TAG_MASK_
#undef FENWICK_D_IS_SMALL_
#undef FENWICK_D_UNTAG_
#undef FENWICK_D_TAG_
#undef FENWICK_SUFFIX
#undef FENWICK_VALUE_TYPE
#undef FENWICK_IS_FLOATING
