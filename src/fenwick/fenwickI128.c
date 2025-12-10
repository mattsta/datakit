/* Fenwick Tree - I128 Dispatcher (2-TIER SYSTEM)
 * Auto-generated tier management and public API.
 * DO NOT EDIT MANUALLY.
 *
 * Architecture: Small → Full (single transition)
 * Benefit: 50% fewer transitions vs 3-tier system
 */

#include "fenwickI128.h"
#include "../datakit.h"

#ifdef __SIZEOF_INT128__

/* Create new fenwickI128 - starts as small tier */
void *fenwickI128New(void) {
    fenwickI128Small *small = fenwickI128SmallNew();
    return FENWICK_I128_TAG(small, FENWICK_I128_TYPE_SMALL);
}

/* Free fenwickI128 based on tier */
void fenwickI128Free(void *fw) {
    if (!fw) {
        return;
    }

    fenwickI128Type type = FENWICK_I128_TYPE(fw);
    void *ptr = FENWICK_I128_UNTAG(fw);

    switch (type) {
    case FENWICK_I128_TYPE_SMALL:
        fenwickI128SmallFree((fenwickI128Small *)ptr);
        break;
    case FENWICK_I128_TYPE_FULL:
        fenwickI128FullFree((fenwickI128Full *)ptr);
        break;
    }
}

/* Upgrade small to full (single transition in 2-tier system) */
static void *fenwickI128UpgradeSmallToFull(fenwickI128Small *small) {
    fenwickI128Full *full = fenwickI128FullFromSmall(small);
    fenwickI128SmallFree(small);
    return FENWICK_I128_TAG(full, FENWICK_I128_TYPE_FULL);
}

/* Update with tier transitions (2-TIER) */
bool fenwickI128Update(void **fw, size_t idx, __int128_t delta) {
    if (!fw) {
        return false;
    }

    if (!*fw) {
        *fw = fenwickI128New();
    }

    fenwickI128Type type = FENWICK_I128_TYPE(*fw);
    void *ptr = FENWICK_I128_UNTAG(*fw);

    switch (type) {
    case FENWICK_I128_TYPE_SMALL: {
        fenwickI128Small *small = (fenwickI128Small *)ptr;

        /* Check if we need to upgrade to Full (check BEFORE update) */
        if (fenwickI128SmallShouldUpgrade(small) ||
            idx >= fenwickI128SmallCount(small) + 1000 ||
            idx * sizeof(__int128_t) > 128 * 1024) {
            *fw = fenwickI128UpgradeSmallToFull(small);
            return fenwickI128Update(fw, idx, delta);
        }

        bool success = false;
        fenwickI128Small *newSmall =
            fenwickI128SmallUpdate(small, (uint32_t)idx, delta, &success);
        *fw = FENWICK_I128_TAG(newSmall, FENWICK_I128_TYPE_SMALL);
        return success;
    }

    case FENWICK_I128_TYPE_FULL: {
        fenwickI128Full *full = (fenwickI128Full *)ptr;

        /* No further upgrades in 2-tier system */
        bool success = false;
        fenwickI128Full *newFull =
            fenwickI128FullUpdate(full, (uint64_t)idx, delta, &success);
        *fw = FENWICK_I128_TAG(newFull, FENWICK_I128_TYPE_FULL);
        return success;
    }
    }

    return false;
}

/* Query prefix sum */
__int128_t fenwickI128Query(const void *fw, size_t idx) {
    if (!fw) {
        return (__int128_t)0;
    }

    fenwickI128Type type = FENWICK_I128_TYPE(fw);
    void *ptr = FENWICK_I128_UNTAG(fw);

    switch (type) {
    case FENWICK_I128_TYPE_SMALL:
        return fenwickI128SmallQuery((const fenwickI128Small *)ptr,
                                     (uint32_t)idx);
    case FENWICK_I128_TYPE_FULL:
        return fenwickI128FullQuery((const fenwickI128Full *)ptr,
                                    (uint64_t)idx);
    }

    return (__int128_t)0;
}

/* Range query */
__int128_t fenwickI128RangeQuery(const void *fw, size_t left, size_t right) {
    if (!fw) {
        return (__int128_t)0;
    }

    fenwickI128Type type = FENWICK_I128_TYPE(fw);
    void *ptr = FENWICK_I128_UNTAG(fw);

    switch (type) {
    case FENWICK_I128_TYPE_SMALL:
        return fenwickI128SmallRangeQuery((const fenwickI128Small *)ptr,
                                          (uint32_t)left, (uint32_t)right);
    case FENWICK_I128_TYPE_FULL:
        return fenwickI128FullRangeQuery((const fenwickI128Full *)ptr,
                                         (uint64_t)left, (uint64_t)right);
    }

    return (__int128_t)0;
}

/* Get single element */
__int128_t fenwickI128Get(const void *fw, size_t idx) {
    if (!fw) {
        return (__int128_t)0;
    }

    fenwickI128Type type = FENWICK_I128_TYPE(fw);
    void *ptr = FENWICK_I128_UNTAG(fw);

    switch (type) {
    case FENWICK_I128_TYPE_SMALL:
        return fenwickI128SmallGet((const fenwickI128Small *)ptr,
                                   (uint32_t)idx);
    case FENWICK_I128_TYPE_FULL:
        return fenwickI128FullGet((const fenwickI128Full *)ptr, (uint64_t)idx);
    }

    return (__int128_t)0;
}

/* Set single element */
bool fenwickI128Set(void **fw, size_t idx, __int128_t value) {
    if (!fw) {
        return false;
    }

    if (!*fw) {
        *fw = fenwickI128New();
    }

    fenwickI128Type type = FENWICK_I128_TYPE(*fw);
    void *ptr = FENWICK_I128_UNTAG(*fw);

    switch (type) {
    case FENWICK_I128_TYPE_SMALL: {
        fenwickI128Small *small = (fenwickI128Small *)ptr;

        if (fenwickI128SmallShouldUpgrade(small)) {
            *fw = fenwickI128UpgradeSmallToFull(small);
            return fenwickI128Set(fw, idx, value);
        }

        bool success = false;
        fenwickI128Small *newSmall =
            fenwickI128SmallSet(small, (uint32_t)idx, value, &success);
        *fw = FENWICK_I128_TAG(newSmall, FENWICK_I128_TYPE_SMALL);
        return success;
    }

    case FENWICK_I128_TYPE_FULL: {
        fenwickI128Full *full = (fenwickI128Full *)ptr;

        bool success = false;
        fenwickI128Full *newFull =
            fenwickI128FullSet(full, (uint64_t)idx, value, &success);
        *fw = FENWICK_I128_TAG(newFull, FENWICK_I128_TYPE_FULL);
        return success;
    }
    }

    return false;
}

/* Get count */
size_t fenwickI128Count(const void *fw) {
    if (!fw) {
        return 0;
    }

    fenwickI128Type type = FENWICK_I128_TYPE(fw);
    void *ptr = FENWICK_I128_UNTAG(fw);

    switch (type) {
    case FENWICK_I128_TYPE_SMALL:
        return fenwickI128SmallCount((const fenwickI128Small *)ptr);
    case FENWICK_I128_TYPE_FULL:
        return fenwickI128FullCount((const fenwickI128Full *)ptr);
    }

    return 0;
}

/* Get bytes */
size_t fenwickI128Bytes(const void *fw) {
    if (!fw) {
        return 0;
    }

    fenwickI128Type type = FENWICK_I128_TYPE(fw);
    void *ptr = FENWICK_I128_UNTAG(fw);

    switch (type) {
    case FENWICK_I128_TYPE_SMALL:
        return fenwickI128SmallBytes((const fenwickI128Small *)ptr);
    case FENWICK_I128_TYPE_FULL:
        return fenwickI128FullBytes((const fenwickI128Full *)ptr);
    }

    return 0;
}

/* Lower bound */
size_t fenwickI128LowerBound(const void *fw, __int128_t target) {
    if (!fw) {
        return SIZE_MAX;
    }

    fenwickI128Type type = FENWICK_I128_TYPE(fw);
    void *ptr = FENWICK_I128_UNTAG(fw);

    switch (type) {
    case FENWICK_I128_TYPE_SMALL: {
        uint32_t result =
            fenwickI128SmallLowerBound((const fenwickI128Small *)ptr, target);
        return (result == (uint32_t)-1) ? SIZE_MAX : result;
    }
    case FENWICK_I128_TYPE_FULL: {
        uint64_t result =
            fenwickI128FullLowerBound((const fenwickI128Full *)ptr, target);
        return (result == (uint64_t)-1) ? SIZE_MAX : result;
    }
    }

    return SIZE_MAX;
}

/* Clear */
void fenwickI128Clear(void *fw) {
    if (!fw) {
        return;
    }

    fenwickI128Type type = FENWICK_I128_TYPE(fw);
    void *ptr = FENWICK_I128_UNTAG(fw);

    switch (type) {
    case FENWICK_I128_TYPE_SMALL:
        fenwickI128SmallClear((fenwickI128Small *)ptr);
        break;
    case FENWICK_I128_TYPE_FULL:
        fenwickI128FullClear((fenwickI128Full *)ptr);
        break;
    }
}

#ifdef DATAKIT_TEST
/* Debug representation */
void fenwickI128Repr(const void *fw) {
    if (!fw) {
        printf("fenwickI128: (nil)\n");
        return;
    }

    fenwickI128Type type = FENWICK_I128_TYPE(fw);
    void *ptr = FENWICK_I128_UNTAG(fw);

    const char *tierName = "UNKNOWN";
    switch (type) {
    case FENWICK_I128_TYPE_SMALL:
        tierName = "SMALL";
        fenwickI128SmallRepr((const fenwickI128Small *)ptr);
        break;
    case FENWICK_I128_TYPE_FULL:
        tierName = "FULL";
        fenwickI128FullRepr((const fenwickI128Full *)ptr);
        break;
    }

    printf("  Tier: %s, Count: %zu, Bytes: %zu\n", tierName,
           fenwickI128Count(fw), fenwickI128Bytes(fw));
}
#endif

#endif /* __SIZEOF_INT128__ */
