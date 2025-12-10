/* Fenwick Tree - U32 Dispatcher (2-TIER SYSTEM)
 * Auto-generated tier management and public API.
 * DO NOT EDIT MANUALLY.
 *
 * Architecture: Small → Full (single transition)
 * Benefit: 50% fewer transitions vs 3-tier system
 */

#include "fenwickU32.h"
#include "../datakit.h"

/* Create new fenwickU32 - starts as small tier */
void *fenwickU32New(void) {
    fenwickU32Small *small = fenwickU32SmallNew();
    return FENWICK_U32_TAG(small, FENWICK_U32_TYPE_SMALL);
}

/* Free fenwickU32 based on tier */
void fenwickU32Free(void *fw) {
    if (!fw) {
        return;
    }

    fenwickU32Type type = FENWICK_U32_TYPE(fw);
    void *ptr = FENWICK_U32_UNTAG(fw);

    switch (type) {
    case FENWICK_U32_TYPE_SMALL:
        fenwickU32SmallFree((fenwickU32Small *)ptr);
        break;
    case FENWICK_U32_TYPE_FULL:
        fenwickU32FullFree((fenwickU32Full *)ptr);
        break;
    }
}

/* Upgrade small to full (single transition in 2-tier system) */
static void *fenwickU32UpgradeSmallToFull(fenwickU32Small *small) {
    fenwickU32Full *full = fenwickU32FullFromSmall(small);
    fenwickU32SmallFree(small);
    return FENWICK_U32_TAG(full, FENWICK_U32_TYPE_FULL);
}

/* Update with tier transitions (2-TIER) */
bool fenwickU32Update(void **fw, size_t idx, uint32_t delta) {
    if (!fw) {
        return false;
    }

    if (!*fw) {
        *fw = fenwickU32New();
    }

    fenwickU32Type type = FENWICK_U32_TYPE(*fw);
    void *ptr = FENWICK_U32_UNTAG(*fw);

    switch (type) {
    case FENWICK_U32_TYPE_SMALL: {
        fenwickU32Small *small = (fenwickU32Small *)ptr;

        /* Check if we need to upgrade to Full (check BEFORE update) */
        if (fenwickU32SmallShouldUpgrade(small) ||
            idx >= fenwickU32SmallCount(small) + 1000 ||
            idx * sizeof(uint32_t) > 128 * 1024) {
            *fw = fenwickU32UpgradeSmallToFull(small);
            return fenwickU32Update(fw, idx, delta);
        }

        bool success = false;
        fenwickU32Small *newSmall =
            fenwickU32SmallUpdate(small, (uint32_t)idx, delta, &success);
        *fw = FENWICK_U32_TAG(newSmall, FENWICK_U32_TYPE_SMALL);
        return success;
    }

    case FENWICK_U32_TYPE_FULL: {
        fenwickU32Full *full = (fenwickU32Full *)ptr;

        /* No further upgrades in 2-tier system */
        bool success = false;
        fenwickU32Full *newFull =
            fenwickU32FullUpdate(full, (uint64_t)idx, delta, &success);
        *fw = FENWICK_U32_TAG(newFull, FENWICK_U32_TYPE_FULL);
        return success;
    }
    }

    return false;
}

/* Query prefix sum */
uint32_t fenwickU32Query(const void *fw, size_t idx) {
    if (!fw) {
        return (uint32_t)0;
    }

    fenwickU32Type type = FENWICK_U32_TYPE(fw);
    void *ptr = FENWICK_U32_UNTAG(fw);

    switch (type) {
    case FENWICK_U32_TYPE_SMALL:
        return fenwickU32SmallQuery((const fenwickU32Small *)ptr,
                                    (uint32_t)idx);
    case FENWICK_U32_TYPE_FULL:
        return fenwickU32FullQuery((const fenwickU32Full *)ptr, (uint64_t)idx);
    }

    return (uint32_t)0;
}

/* Range query */
uint32_t fenwickU32RangeQuery(const void *fw, size_t left, size_t right) {
    if (!fw) {
        return (uint32_t)0;
    }

    fenwickU32Type type = FENWICK_U32_TYPE(fw);
    void *ptr = FENWICK_U32_UNTAG(fw);

    switch (type) {
    case FENWICK_U32_TYPE_SMALL:
        return fenwickU32SmallRangeQuery((const fenwickU32Small *)ptr,
                                         (uint32_t)left, (uint32_t)right);
    case FENWICK_U32_TYPE_FULL:
        return fenwickU32FullRangeQuery((const fenwickU32Full *)ptr,
                                        (uint64_t)left, (uint64_t)right);
    }

    return (uint32_t)0;
}

/* Get single element */
uint32_t fenwickU32Get(const void *fw, size_t idx) {
    if (!fw) {
        return (uint32_t)0;
    }

    fenwickU32Type type = FENWICK_U32_TYPE(fw);
    void *ptr = FENWICK_U32_UNTAG(fw);

    switch (type) {
    case FENWICK_U32_TYPE_SMALL:
        return fenwickU32SmallGet((const fenwickU32Small *)ptr, (uint32_t)idx);
    case FENWICK_U32_TYPE_FULL:
        return fenwickU32FullGet((const fenwickU32Full *)ptr, (uint64_t)idx);
    }

    return (uint32_t)0;
}

/* Set single element */
bool fenwickU32Set(void **fw, size_t idx, uint32_t value) {
    if (!fw) {
        return false;
    }

    if (!*fw) {
        *fw = fenwickU32New();
    }

    fenwickU32Type type = FENWICK_U32_TYPE(*fw);
    void *ptr = FENWICK_U32_UNTAG(*fw);

    switch (type) {
    case FENWICK_U32_TYPE_SMALL: {
        fenwickU32Small *small = (fenwickU32Small *)ptr;

        if (fenwickU32SmallShouldUpgrade(small)) {
            *fw = fenwickU32UpgradeSmallToFull(small);
            return fenwickU32Set(fw, idx, value);
        }

        bool success = false;
        fenwickU32Small *newSmall =
            fenwickU32SmallSet(small, (uint32_t)idx, value, &success);
        *fw = FENWICK_U32_TAG(newSmall, FENWICK_U32_TYPE_SMALL);
        return success;
    }

    case FENWICK_U32_TYPE_FULL: {
        fenwickU32Full *full = (fenwickU32Full *)ptr;

        bool success = false;
        fenwickU32Full *newFull =
            fenwickU32FullSet(full, (uint64_t)idx, value, &success);
        *fw = FENWICK_U32_TAG(newFull, FENWICK_U32_TYPE_FULL);
        return success;
    }
    }

    return false;
}

/* Get count */
size_t fenwickU32Count(const void *fw) {
    if (!fw) {
        return 0;
    }

    fenwickU32Type type = FENWICK_U32_TYPE(fw);
    void *ptr = FENWICK_U32_UNTAG(fw);

    switch (type) {
    case FENWICK_U32_TYPE_SMALL:
        return fenwickU32SmallCount((const fenwickU32Small *)ptr);
    case FENWICK_U32_TYPE_FULL:
        return fenwickU32FullCount((const fenwickU32Full *)ptr);
    }

    return 0;
}

/* Get bytes */
size_t fenwickU32Bytes(const void *fw) {
    if (!fw) {
        return 0;
    }

    fenwickU32Type type = FENWICK_U32_TYPE(fw);
    void *ptr = FENWICK_U32_UNTAG(fw);

    switch (type) {
    case FENWICK_U32_TYPE_SMALL:
        return fenwickU32SmallBytes((const fenwickU32Small *)ptr);
    case FENWICK_U32_TYPE_FULL:
        return fenwickU32FullBytes((const fenwickU32Full *)ptr);
    }

    return 0;
}

/* Lower bound */
size_t fenwickU32LowerBound(const void *fw, uint32_t target) {
    if (!fw) {
        return SIZE_MAX;
    }

    fenwickU32Type type = FENWICK_U32_TYPE(fw);
    void *ptr = FENWICK_U32_UNTAG(fw);

    switch (type) {
    case FENWICK_U32_TYPE_SMALL: {
        uint32_t result =
            fenwickU32SmallLowerBound((const fenwickU32Small *)ptr, target);
        return (result == (uint32_t)-1) ? SIZE_MAX : result;
    }
    case FENWICK_U32_TYPE_FULL: {
        uint64_t result =
            fenwickU32FullLowerBound((const fenwickU32Full *)ptr, target);
        return (result == (uint64_t)-1) ? SIZE_MAX : result;
    }
    }

    return SIZE_MAX;
}

/* Clear */
void fenwickU32Clear(void *fw) {
    if (!fw) {
        return;
    }

    fenwickU32Type type = FENWICK_U32_TYPE(fw);
    void *ptr = FENWICK_U32_UNTAG(fw);

    switch (type) {
    case FENWICK_U32_TYPE_SMALL:
        fenwickU32SmallClear((fenwickU32Small *)ptr);
        break;
    case FENWICK_U32_TYPE_FULL:
        fenwickU32FullClear((fenwickU32Full *)ptr);
        break;
    }
}

#ifdef DATAKIT_TEST
/* Debug representation */
void fenwickU32Repr(const void *fw) {
    if (!fw) {
        printf("fenwickU32: (nil)\n");
        return;
    }

    fenwickU32Type type = FENWICK_U32_TYPE(fw);
    void *ptr = FENWICK_U32_UNTAG(fw);

    const char *tierName = "UNKNOWN";
    switch (type) {
    case FENWICK_U32_TYPE_SMALL:
        tierName = "SMALL";
        fenwickU32SmallRepr((const fenwickU32Small *)ptr);
        break;
    case FENWICK_U32_TYPE_FULL:
        tierName = "FULL";
        fenwickU32FullRepr((const fenwickU32Full *)ptr);
        break;
    }

    printf("  Tier: %s, Count: %zu, Bytes: %zu\n", tierName,
           fenwickU32Count(fw), fenwickU32Bytes(fw));
}
#endif
