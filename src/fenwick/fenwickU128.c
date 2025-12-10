/* Fenwick Tree - U128 Dispatcher (2-TIER SYSTEM)
 * Auto-generated tier management and public API.
 * DO NOT EDIT MANUALLY.
 *
 * Architecture: Small → Full (single transition)
 * Benefit: 50% fewer transitions vs 3-tier system
 */

#include "fenwickU128.h"
#include "../datakit.h"

#ifdef __SIZEOF_INT128__

/* Create new fenwickU128 - starts as small tier */
void *fenwickU128New(void) {
    fenwickU128Small *small = fenwickU128SmallNew();
    return FENWICK_U128_TAG(small, FENWICK_U128_TYPE_SMALL);
}

/* Free fenwickU128 based on tier */
void fenwickU128Free(void *fw) {
    if (!fw) {
        return;
    }

    fenwickU128Type type = FENWICK_U128_TYPE(fw);
    void *ptr = FENWICK_U128_UNTAG(fw);

    switch (type) {
    case FENWICK_U128_TYPE_SMALL:
        fenwickU128SmallFree((fenwickU128Small *)ptr);
        break;
    case FENWICK_U128_TYPE_FULL:
        fenwickU128FullFree((fenwickU128Full *)ptr);
        break;
    }
}

/* Upgrade small to full (single transition in 2-tier system) */
static void *fenwickU128UpgradeSmallToFull(fenwickU128Small *small) {
    fenwickU128Full *full = fenwickU128FullFromSmall(small);
    fenwickU128SmallFree(small);
    return FENWICK_U128_TAG(full, FENWICK_U128_TYPE_FULL);
}

/* Update with tier transitions (2-TIER) */
bool fenwickU128Update(void **fw, size_t idx, __uint128_t delta) {
    if (!fw) {
        return false;
    }

    if (!*fw) {
        *fw = fenwickU128New();
    }

    fenwickU128Type type = FENWICK_U128_TYPE(*fw);
    void *ptr = FENWICK_U128_UNTAG(*fw);

    switch (type) {
    case FENWICK_U128_TYPE_SMALL: {
        fenwickU128Small *small = (fenwickU128Small *)ptr;

        /* Check if we need to upgrade to Full (check BEFORE update) */
        if (fenwickU128SmallShouldUpgrade(small) ||
            idx >= fenwickU128SmallCount(small) + 1000 ||
            idx * sizeof(__uint128_t) > 128 * 1024) {
            *fw = fenwickU128UpgradeSmallToFull(small);
            return fenwickU128Update(fw, idx, delta);
        }

        bool success = false;
        fenwickU128Small *newSmall =
            fenwickU128SmallUpdate(small, (uint32_t)idx, delta, &success);
        *fw = FENWICK_U128_TAG(newSmall, FENWICK_U128_TYPE_SMALL);
        return success;
    }

    case FENWICK_U128_TYPE_FULL: {
        fenwickU128Full *full = (fenwickU128Full *)ptr;

        /* No further upgrades in 2-tier system */
        bool success = false;
        fenwickU128Full *newFull =
            fenwickU128FullUpdate(full, (uint64_t)idx, delta, &success);
        *fw = FENWICK_U128_TAG(newFull, FENWICK_U128_TYPE_FULL);
        return success;
    }
    }

    return false;
}

/* Query prefix sum */
__uint128_t fenwickU128Query(const void *fw, size_t idx) {
    if (!fw) {
        return (__uint128_t)0;
    }

    fenwickU128Type type = FENWICK_U128_TYPE(fw);
    void *ptr = FENWICK_U128_UNTAG(fw);

    switch (type) {
    case FENWICK_U128_TYPE_SMALL:
        return fenwickU128SmallQuery((const fenwickU128Small *)ptr,
                                     (uint32_t)idx);
    case FENWICK_U128_TYPE_FULL:
        return fenwickU128FullQuery((const fenwickU128Full *)ptr,
                                    (uint64_t)idx);
    }

    return (__uint128_t)0;
}

/* Range query */
__uint128_t fenwickU128RangeQuery(const void *fw, size_t left, size_t right) {
    if (!fw) {
        return (__uint128_t)0;
    }

    fenwickU128Type type = FENWICK_U128_TYPE(fw);
    void *ptr = FENWICK_U128_UNTAG(fw);

    switch (type) {
    case FENWICK_U128_TYPE_SMALL:
        return fenwickU128SmallRangeQuery((const fenwickU128Small *)ptr,
                                          (uint32_t)left, (uint32_t)right);
    case FENWICK_U128_TYPE_FULL:
        return fenwickU128FullRangeQuery((const fenwickU128Full *)ptr,
                                         (uint64_t)left, (uint64_t)right);
    }

    return (__uint128_t)0;
}

/* Get single element */
__uint128_t fenwickU128Get(const void *fw, size_t idx) {
    if (!fw) {
        return (__uint128_t)0;
    }

    fenwickU128Type type = FENWICK_U128_TYPE(fw);
    void *ptr = FENWICK_U128_UNTAG(fw);

    switch (type) {
    case FENWICK_U128_TYPE_SMALL:
        return fenwickU128SmallGet((const fenwickU128Small *)ptr,
                                   (uint32_t)idx);
    case FENWICK_U128_TYPE_FULL:
        return fenwickU128FullGet((const fenwickU128Full *)ptr, (uint64_t)idx);
    }

    return (__uint128_t)0;
}

/* Set single element */
bool fenwickU128Set(void **fw, size_t idx, __uint128_t value) {
    if (!fw) {
        return false;
    }

    if (!*fw) {
        *fw = fenwickU128New();
    }

    fenwickU128Type type = FENWICK_U128_TYPE(*fw);
    void *ptr = FENWICK_U128_UNTAG(*fw);

    switch (type) {
    case FENWICK_U128_TYPE_SMALL: {
        fenwickU128Small *small = (fenwickU128Small *)ptr;

        if (fenwickU128SmallShouldUpgrade(small)) {
            *fw = fenwickU128UpgradeSmallToFull(small);
            return fenwickU128Set(fw, idx, value);
        }

        bool success = false;
        fenwickU128Small *newSmall =
            fenwickU128SmallSet(small, (uint32_t)idx, value, &success);
        *fw = FENWICK_U128_TAG(newSmall, FENWICK_U128_TYPE_SMALL);
        return success;
    }

    case FENWICK_U128_TYPE_FULL: {
        fenwickU128Full *full = (fenwickU128Full *)ptr;

        bool success = false;
        fenwickU128Full *newFull =
            fenwickU128FullSet(full, (uint64_t)idx, value, &success);
        *fw = FENWICK_U128_TAG(newFull, FENWICK_U128_TYPE_FULL);
        return success;
    }
    }

    return false;
}

/* Get count */
size_t fenwickU128Count(const void *fw) {
    if (!fw) {
        return 0;
    }

    fenwickU128Type type = FENWICK_U128_TYPE(fw);
    void *ptr = FENWICK_U128_UNTAG(fw);

    switch (type) {
    case FENWICK_U128_TYPE_SMALL:
        return fenwickU128SmallCount((const fenwickU128Small *)ptr);
    case FENWICK_U128_TYPE_FULL:
        return fenwickU128FullCount((const fenwickU128Full *)ptr);
    }

    return 0;
}

/* Get bytes */
size_t fenwickU128Bytes(const void *fw) {
    if (!fw) {
        return 0;
    }

    fenwickU128Type type = FENWICK_U128_TYPE(fw);
    void *ptr = FENWICK_U128_UNTAG(fw);

    switch (type) {
    case FENWICK_U128_TYPE_SMALL:
        return fenwickU128SmallBytes((const fenwickU128Small *)ptr);
    case FENWICK_U128_TYPE_FULL:
        return fenwickU128FullBytes((const fenwickU128Full *)ptr);
    }

    return 0;
}

/* Lower bound */
size_t fenwickU128LowerBound(const void *fw, __uint128_t target) {
    if (!fw) {
        return SIZE_MAX;
    }

    fenwickU128Type type = FENWICK_U128_TYPE(fw);
    void *ptr = FENWICK_U128_UNTAG(fw);

    switch (type) {
    case FENWICK_U128_TYPE_SMALL: {
        uint32_t result =
            fenwickU128SmallLowerBound((const fenwickU128Small *)ptr, target);
        return (result == (uint32_t)-1) ? SIZE_MAX : result;
    }
    case FENWICK_U128_TYPE_FULL: {
        uint64_t result =
            fenwickU128FullLowerBound((const fenwickU128Full *)ptr, target);
        return (result == (uint64_t)-1) ? SIZE_MAX : result;
    }
    }

    return SIZE_MAX;
}

/* Clear */
void fenwickU128Clear(void *fw) {
    if (!fw) {
        return;
    }

    fenwickU128Type type = FENWICK_U128_TYPE(fw);
    void *ptr = FENWICK_U128_UNTAG(fw);

    switch (type) {
    case FENWICK_U128_TYPE_SMALL:
        fenwickU128SmallClear((fenwickU128Small *)ptr);
        break;
    case FENWICK_U128_TYPE_FULL:
        fenwickU128FullClear((fenwickU128Full *)ptr);
        break;
    }
}

#ifdef DATAKIT_TEST
/* Debug representation */
void fenwickU128Repr(const void *fw) {
    if (!fw) {
        printf("fenwickU128: (nil)\n");
        return;
    }

    fenwickU128Type type = FENWICK_U128_TYPE(fw);
    void *ptr = FENWICK_U128_UNTAG(fw);

    const char *tierName = "UNKNOWN";
    switch (type) {
    case FENWICK_U128_TYPE_SMALL:
        tierName = "SMALL";
        fenwickU128SmallRepr((const fenwickU128Small *)ptr);
        break;
    case FENWICK_U128_TYPE_FULL:
        tierName = "FULL";
        fenwickU128FullRepr((const fenwickU128Full *)ptr);
        break;
    }

    printf("  Tier: %s, Count: %zu, Bytes: %zu\n", tierName,
           fenwickU128Count(fw), fenwickU128Bytes(fw));
}
#endif

#endif /* __SIZEOF_INT128__ */
