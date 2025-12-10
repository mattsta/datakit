/* Fenwick Tree - U64 Dispatcher (2-TIER SYSTEM)
 * Auto-generated tier management and public API.
 * DO NOT EDIT MANUALLY.
 *
 * Architecture: Small → Full (single transition)
 * Benefit: 50% fewer transitions vs 3-tier system
 */

#include "fenwickU64.h"
#include "../datakit.h"

/* Create new fenwickU64 - starts as small tier */
void *fenwickU64New(void) {
    fenwickU64Small *small = fenwickU64SmallNew();
    return FENWICK_U64_TAG(small, FENWICK_U64_TYPE_SMALL);
}

/* Free fenwickU64 based on tier */
void fenwickU64Free(void *fw) {
    if (!fw) {
        return;
    }

    fenwickU64Type type = FENWICK_U64_TYPE(fw);
    void *ptr = FENWICK_U64_UNTAG(fw);

    switch (type) {
    case FENWICK_U64_TYPE_SMALL:
        fenwickU64SmallFree((fenwickU64Small *)ptr);
        break;
    case FENWICK_U64_TYPE_FULL:
        fenwickU64FullFree((fenwickU64Full *)ptr);
        break;
    }
}

/* Upgrade small to full (single transition in 2-tier system) */
static void *fenwickU64UpgradeSmallToFull(fenwickU64Small *small) {
    fenwickU64Full *full = fenwickU64FullFromSmall(small);
    fenwickU64SmallFree(small);
    return FENWICK_U64_TAG(full, FENWICK_U64_TYPE_FULL);
}

/* Update with tier transitions (2-TIER) */
bool fenwickU64Update(void **fw, size_t idx, uint64_t delta) {
    if (!fw) {
        return false;
    }

    if (!*fw) {
        *fw = fenwickU64New();
    }

    fenwickU64Type type = FENWICK_U64_TYPE(*fw);
    void *ptr = FENWICK_U64_UNTAG(*fw);

    switch (type) {
    case FENWICK_U64_TYPE_SMALL: {
        fenwickU64Small *small = (fenwickU64Small *)ptr;

        /* Check if we need to upgrade to Full (check BEFORE update) */
        if (fenwickU64SmallShouldUpgrade(small) ||
            idx >= fenwickU64SmallCount(small) + 1000 ||
            idx * sizeof(uint64_t) > 128 * 1024) {
            *fw = fenwickU64UpgradeSmallToFull(small);
            return fenwickU64Update(fw, idx, delta);
        }

        bool success = false;
        fenwickU64Small *newSmall =
            fenwickU64SmallUpdate(small, (uint32_t)idx, delta, &success);
        *fw = FENWICK_U64_TAG(newSmall, FENWICK_U64_TYPE_SMALL);
        return success;
    }

    case FENWICK_U64_TYPE_FULL: {
        fenwickU64Full *full = (fenwickU64Full *)ptr;

        /* No further upgrades in 2-tier system */
        bool success = false;
        fenwickU64Full *newFull =
            fenwickU64FullUpdate(full, (uint64_t)idx, delta, &success);
        *fw = FENWICK_U64_TAG(newFull, FENWICK_U64_TYPE_FULL);
        return success;
    }
    }

    return false;
}

/* Query prefix sum */
uint64_t fenwickU64Query(const void *fw, size_t idx) {
    if (!fw) {
        return (uint64_t)0;
    }

    fenwickU64Type type = FENWICK_U64_TYPE(fw);
    void *ptr = FENWICK_U64_UNTAG(fw);

    switch (type) {
    case FENWICK_U64_TYPE_SMALL:
        return fenwickU64SmallQuery((const fenwickU64Small *)ptr,
                                    (uint32_t)idx);
    case FENWICK_U64_TYPE_FULL:
        return fenwickU64FullQuery((const fenwickU64Full *)ptr, (uint64_t)idx);
    }

    return (uint64_t)0;
}

/* Range query */
uint64_t fenwickU64RangeQuery(const void *fw, size_t left, size_t right) {
    if (!fw) {
        return (uint64_t)0;
    }

    fenwickU64Type type = FENWICK_U64_TYPE(fw);
    void *ptr = FENWICK_U64_UNTAG(fw);

    switch (type) {
    case FENWICK_U64_TYPE_SMALL:
        return fenwickU64SmallRangeQuery((const fenwickU64Small *)ptr,
                                         (uint32_t)left, (uint32_t)right);
    case FENWICK_U64_TYPE_FULL:
        return fenwickU64FullRangeQuery((const fenwickU64Full *)ptr,
                                        (uint64_t)left, (uint64_t)right);
    }

    return (uint64_t)0;
}

/* Get single element */
uint64_t fenwickU64Get(const void *fw, size_t idx) {
    if (!fw) {
        return (uint64_t)0;
    }

    fenwickU64Type type = FENWICK_U64_TYPE(fw);
    void *ptr = FENWICK_U64_UNTAG(fw);

    switch (type) {
    case FENWICK_U64_TYPE_SMALL:
        return fenwickU64SmallGet((const fenwickU64Small *)ptr, (uint32_t)idx);
    case FENWICK_U64_TYPE_FULL:
        return fenwickU64FullGet((const fenwickU64Full *)ptr, (uint64_t)idx);
    }

    return (uint64_t)0;
}

/* Set single element */
bool fenwickU64Set(void **fw, size_t idx, uint64_t value) {
    if (!fw) {
        return false;
    }

    if (!*fw) {
        *fw = fenwickU64New();
    }

    fenwickU64Type type = FENWICK_U64_TYPE(*fw);
    void *ptr = FENWICK_U64_UNTAG(*fw);

    switch (type) {
    case FENWICK_U64_TYPE_SMALL: {
        fenwickU64Small *small = (fenwickU64Small *)ptr;

        if (fenwickU64SmallShouldUpgrade(small)) {
            *fw = fenwickU64UpgradeSmallToFull(small);
            return fenwickU64Set(fw, idx, value);
        }

        bool success = false;
        fenwickU64Small *newSmall =
            fenwickU64SmallSet(small, (uint32_t)idx, value, &success);
        *fw = FENWICK_U64_TAG(newSmall, FENWICK_U64_TYPE_SMALL);
        return success;
    }

    case FENWICK_U64_TYPE_FULL: {
        fenwickU64Full *full = (fenwickU64Full *)ptr;

        bool success = false;
        fenwickU64Full *newFull =
            fenwickU64FullSet(full, (uint64_t)idx, value, &success);
        *fw = FENWICK_U64_TAG(newFull, FENWICK_U64_TYPE_FULL);
        return success;
    }
    }

    return false;
}

/* Get count */
size_t fenwickU64Count(const void *fw) {
    if (!fw) {
        return 0;
    }

    fenwickU64Type type = FENWICK_U64_TYPE(fw);
    void *ptr = FENWICK_U64_UNTAG(fw);

    switch (type) {
    case FENWICK_U64_TYPE_SMALL:
        return fenwickU64SmallCount((const fenwickU64Small *)ptr);
    case FENWICK_U64_TYPE_FULL:
        return fenwickU64FullCount((const fenwickU64Full *)ptr);
    }

    return 0;
}

/* Get bytes */
size_t fenwickU64Bytes(const void *fw) {
    if (!fw) {
        return 0;
    }

    fenwickU64Type type = FENWICK_U64_TYPE(fw);
    void *ptr = FENWICK_U64_UNTAG(fw);

    switch (type) {
    case FENWICK_U64_TYPE_SMALL:
        return fenwickU64SmallBytes((const fenwickU64Small *)ptr);
    case FENWICK_U64_TYPE_FULL:
        return fenwickU64FullBytes((const fenwickU64Full *)ptr);
    }

    return 0;
}

/* Lower bound */
size_t fenwickU64LowerBound(const void *fw, uint64_t target) {
    if (!fw) {
        return SIZE_MAX;
    }

    fenwickU64Type type = FENWICK_U64_TYPE(fw);
    void *ptr = FENWICK_U64_UNTAG(fw);

    switch (type) {
    case FENWICK_U64_TYPE_SMALL: {
        uint32_t result =
            fenwickU64SmallLowerBound((const fenwickU64Small *)ptr, target);
        return (result == (uint32_t)-1) ? SIZE_MAX : result;
    }
    case FENWICK_U64_TYPE_FULL: {
        uint64_t result =
            fenwickU64FullLowerBound((const fenwickU64Full *)ptr, target);
        return (result == (uint64_t)-1) ? SIZE_MAX : result;
    }
    }

    return SIZE_MAX;
}

/* Clear */
void fenwickU64Clear(void *fw) {
    if (!fw) {
        return;
    }

    fenwickU64Type type = FENWICK_U64_TYPE(fw);
    void *ptr = FENWICK_U64_UNTAG(fw);

    switch (type) {
    case FENWICK_U64_TYPE_SMALL:
        fenwickU64SmallClear((fenwickU64Small *)ptr);
        break;
    case FENWICK_U64_TYPE_FULL:
        fenwickU64FullClear((fenwickU64Full *)ptr);
        break;
    }
}

#ifdef DATAKIT_TEST
/* Debug representation */
void fenwickU64Repr(const void *fw) {
    if (!fw) {
        printf("fenwickU64: (nil)\n");
        return;
    }

    fenwickU64Type type = FENWICK_U64_TYPE(fw);
    void *ptr = FENWICK_U64_UNTAG(fw);

    const char *tierName = "UNKNOWN";
    switch (type) {
    case FENWICK_U64_TYPE_SMALL:
        tierName = "SMALL";
        fenwickU64SmallRepr((const fenwickU64Small *)ptr);
        break;
    case FENWICK_U64_TYPE_FULL:
        tierName = "FULL";
        fenwickU64FullRepr((const fenwickU64Full *)ptr);
        break;
    }

    printf("  Tier: %s, Count: %zu, Bytes: %zu\n", tierName,
           fenwickU64Count(fw), fenwickU64Bytes(fw));
}
#endif
