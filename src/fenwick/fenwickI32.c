/* Fenwick Tree - I32 Dispatcher (2-TIER SYSTEM)
 * Auto-generated tier management and public API.
 * DO NOT EDIT MANUALLY.
 *
 * Architecture: Small → Full (single transition)
 * Benefit: 50% fewer transitions vs 3-tier system
 */

#include "fenwickI32.h"
#include "../datakit.h"

/* Create new fenwickI32 - starts as small tier */
void *fenwickI32New(void) {
    fenwickI32Small *small = fenwickI32SmallNew();
    return FENWICK_I32_TAG(small, FENWICK_I32_TYPE_SMALL);
}

/* Free fenwickI32 based on tier */
void fenwickI32Free(void *fw) {
    if (!fw) {
        return;
    }

    fenwickI32Type type = FENWICK_I32_TYPE(fw);
    void *ptr = FENWICK_I32_UNTAG(fw);

    switch (type) {
    case FENWICK_I32_TYPE_SMALL:
        fenwickI32SmallFree((fenwickI32Small *)ptr);
        break;
    case FENWICK_I32_TYPE_FULL:
        fenwickI32FullFree((fenwickI32Full *)ptr);
        break;
    }
}

/* Upgrade small to full (single transition in 2-tier system) */
static void *fenwickI32UpgradeSmallToFull(fenwickI32Small *small) {
    fenwickI32Full *full = fenwickI32FullFromSmall(small);
    fenwickI32SmallFree(small);
    return FENWICK_I32_TAG(full, FENWICK_I32_TYPE_FULL);
}

/* Update with tier transitions (2-TIER) */
bool fenwickI32Update(void **fw, size_t idx, int32_t delta) {
    if (!fw) {
        return false;
    }

    if (!*fw) {
        *fw = fenwickI32New();
    }

    fenwickI32Type type = FENWICK_I32_TYPE(*fw);
    void *ptr = FENWICK_I32_UNTAG(*fw);

    switch (type) {
    case FENWICK_I32_TYPE_SMALL: {
        fenwickI32Small *small = (fenwickI32Small *)ptr;

        /* Check if we need to upgrade to Full (check BEFORE update) */
        if (fenwickI32SmallShouldUpgrade(small) ||
            idx >= fenwickI32SmallCount(small) + 1000 ||
            idx * sizeof(int32_t) > 128 * 1024) {
            *fw = fenwickI32UpgradeSmallToFull(small);
            return fenwickI32Update(fw, idx, delta);
        }

        bool success = false;
        fenwickI32Small *newSmall =
            fenwickI32SmallUpdate(small, (uint32_t)idx, delta, &success);
        *fw = FENWICK_I32_TAG(newSmall, FENWICK_I32_TYPE_SMALL);
        return success;
    }

    case FENWICK_I32_TYPE_FULL: {
        fenwickI32Full *full = (fenwickI32Full *)ptr;

        /* No further upgrades in 2-tier system */
        bool success = false;
        fenwickI32Full *newFull =
            fenwickI32FullUpdate(full, (uint64_t)idx, delta, &success);
        *fw = FENWICK_I32_TAG(newFull, FENWICK_I32_TYPE_FULL);
        return success;
    }
    }

    return false;
}

/* Query prefix sum */
int32_t fenwickI32Query(const void *fw, size_t idx) {
    if (!fw) {
        return (int32_t)0;
    }

    fenwickI32Type type = FENWICK_I32_TYPE(fw);
    void *ptr = FENWICK_I32_UNTAG(fw);

    switch (type) {
    case FENWICK_I32_TYPE_SMALL:
        return fenwickI32SmallQuery((const fenwickI32Small *)ptr,
                                    (uint32_t)idx);
    case FENWICK_I32_TYPE_FULL:
        return fenwickI32FullQuery((const fenwickI32Full *)ptr, (uint64_t)idx);
    }

    return (int32_t)0;
}

/* Range query */
int32_t fenwickI32RangeQuery(const void *fw, size_t left, size_t right) {
    if (!fw) {
        return (int32_t)0;
    }

    fenwickI32Type type = FENWICK_I32_TYPE(fw);
    void *ptr = FENWICK_I32_UNTAG(fw);

    switch (type) {
    case FENWICK_I32_TYPE_SMALL:
        return fenwickI32SmallRangeQuery((const fenwickI32Small *)ptr,
                                         (uint32_t)left, (uint32_t)right);
    case FENWICK_I32_TYPE_FULL:
        return fenwickI32FullRangeQuery((const fenwickI32Full *)ptr,
                                        (uint64_t)left, (uint64_t)right);
    }

    return (int32_t)0;
}

/* Get single element */
int32_t fenwickI32Get(const void *fw, size_t idx) {
    if (!fw) {
        return (int32_t)0;
    }

    fenwickI32Type type = FENWICK_I32_TYPE(fw);
    void *ptr = FENWICK_I32_UNTAG(fw);

    switch (type) {
    case FENWICK_I32_TYPE_SMALL:
        return fenwickI32SmallGet((const fenwickI32Small *)ptr, (uint32_t)idx);
    case FENWICK_I32_TYPE_FULL:
        return fenwickI32FullGet((const fenwickI32Full *)ptr, (uint64_t)idx);
    }

    return (int32_t)0;
}

/* Set single element */
bool fenwickI32Set(void **fw, size_t idx, int32_t value) {
    if (!fw) {
        return false;
    }

    if (!*fw) {
        *fw = fenwickI32New();
    }

    fenwickI32Type type = FENWICK_I32_TYPE(*fw);
    void *ptr = FENWICK_I32_UNTAG(*fw);

    switch (type) {
    case FENWICK_I32_TYPE_SMALL: {
        fenwickI32Small *small = (fenwickI32Small *)ptr;

        if (fenwickI32SmallShouldUpgrade(small)) {
            *fw = fenwickI32UpgradeSmallToFull(small);
            return fenwickI32Set(fw, idx, value);
        }

        bool success = false;
        fenwickI32Small *newSmall =
            fenwickI32SmallSet(small, (uint32_t)idx, value, &success);
        *fw = FENWICK_I32_TAG(newSmall, FENWICK_I32_TYPE_SMALL);
        return success;
    }

    case FENWICK_I32_TYPE_FULL: {
        fenwickI32Full *full = (fenwickI32Full *)ptr;

        bool success = false;
        fenwickI32Full *newFull =
            fenwickI32FullSet(full, (uint64_t)idx, value, &success);
        *fw = FENWICK_I32_TAG(newFull, FENWICK_I32_TYPE_FULL);
        return success;
    }
    }

    return false;
}

/* Get count */
size_t fenwickI32Count(const void *fw) {
    if (!fw) {
        return 0;
    }

    fenwickI32Type type = FENWICK_I32_TYPE(fw);
    void *ptr = FENWICK_I32_UNTAG(fw);

    switch (type) {
    case FENWICK_I32_TYPE_SMALL:
        return fenwickI32SmallCount((const fenwickI32Small *)ptr);
    case FENWICK_I32_TYPE_FULL:
        return fenwickI32FullCount((const fenwickI32Full *)ptr);
    }

    return 0;
}

/* Get bytes */
size_t fenwickI32Bytes(const void *fw) {
    if (!fw) {
        return 0;
    }

    fenwickI32Type type = FENWICK_I32_TYPE(fw);
    void *ptr = FENWICK_I32_UNTAG(fw);

    switch (type) {
    case FENWICK_I32_TYPE_SMALL:
        return fenwickI32SmallBytes((const fenwickI32Small *)ptr);
    case FENWICK_I32_TYPE_FULL:
        return fenwickI32FullBytes((const fenwickI32Full *)ptr);
    }

    return 0;
}

/* Lower bound */
size_t fenwickI32LowerBound(const void *fw, int32_t target) {
    if (!fw) {
        return SIZE_MAX;
    }

    fenwickI32Type type = FENWICK_I32_TYPE(fw);
    void *ptr = FENWICK_I32_UNTAG(fw);

    switch (type) {
    case FENWICK_I32_TYPE_SMALL: {
        uint32_t result =
            fenwickI32SmallLowerBound((const fenwickI32Small *)ptr, target);
        return (result == (uint32_t)-1) ? SIZE_MAX : result;
    }
    case FENWICK_I32_TYPE_FULL: {
        uint64_t result =
            fenwickI32FullLowerBound((const fenwickI32Full *)ptr, target);
        return (result == (uint64_t)-1) ? SIZE_MAX : result;
    }
    }

    return SIZE_MAX;
}

/* Clear */
void fenwickI32Clear(void *fw) {
    if (!fw) {
        return;
    }

    fenwickI32Type type = FENWICK_I32_TYPE(fw);
    void *ptr = FENWICK_I32_UNTAG(fw);

    switch (type) {
    case FENWICK_I32_TYPE_SMALL:
        fenwickI32SmallClear((fenwickI32Small *)ptr);
        break;
    case FENWICK_I32_TYPE_FULL:
        fenwickI32FullClear((fenwickI32Full *)ptr);
        break;
    }
}

#ifdef DATAKIT_TEST
/* Debug representation */
void fenwickI32Repr(const void *fw) {
    if (!fw) {
        printf("fenwickI32: (nil)\n");
        return;
    }

    fenwickI32Type type = FENWICK_I32_TYPE(fw);
    void *ptr = FENWICK_I32_UNTAG(fw);

    const char *tierName = "UNKNOWN";
    switch (type) {
    case FENWICK_I32_TYPE_SMALL:
        tierName = "SMALL";
        fenwickI32SmallRepr((const fenwickI32Small *)ptr);
        break;
    case FENWICK_I32_TYPE_FULL:
        tierName = "FULL";
        fenwickI32FullRepr((const fenwickI32Full *)ptr);
        break;
    }

    printf("  Tier: %s, Count: %zu, Bytes: %zu\n", tierName,
           fenwickI32Count(fw), fenwickI32Bytes(fw));
}
#endif
