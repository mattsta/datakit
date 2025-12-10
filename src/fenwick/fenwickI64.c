/* Fenwick Tree - I64 Dispatcher (2-TIER SYSTEM)
 * Auto-generated tier management and public API.
 * DO NOT EDIT MANUALLY.
 *
 * Architecture: Small → Full (single transition)
 * Benefit: 50% fewer transitions vs 3-tier system
 */

#include "fenwickI64.h"
#include "../datakit.h"

/* Create new fenwickI64 - starts as small tier */
void *fenwickI64New(void) {
    fenwickI64Small *small = fenwickI64SmallNew();
    return FENWICK_I64_TAG(small, FENWICK_I64_TYPE_SMALL);
}

/* Free fenwickI64 based on tier */
void fenwickI64Free(void *fw) {
    if (!fw) {
        return;
    }

    fenwickI64Type type = FENWICK_I64_TYPE(fw);
    void *ptr = FENWICK_I64_UNTAG(fw);

    switch (type) {
    case FENWICK_I64_TYPE_SMALL:
        fenwickI64SmallFree((fenwickI64Small *)ptr);
        break;
    case FENWICK_I64_TYPE_FULL:
        fenwickI64FullFree((fenwickI64Full *)ptr);
        break;
    }
}

/* Upgrade small to full (single transition in 2-tier system) */
static void *fenwickI64UpgradeSmallToFull(fenwickI64Small *small) {
    fenwickI64Full *full = fenwickI64FullFromSmall(small);
    fenwickI64SmallFree(small);
    return FENWICK_I64_TAG(full, FENWICK_I64_TYPE_FULL);
}

/* Update with tier transitions (2-TIER) */
bool fenwickI64Update(void **fw, size_t idx, int64_t delta) {
    if (!fw) {
        return false;
    }

    if (!*fw) {
        *fw = fenwickI64New();
    }

    fenwickI64Type type = FENWICK_I64_TYPE(*fw);
    void *ptr = FENWICK_I64_UNTAG(*fw);

    switch (type) {
    case FENWICK_I64_TYPE_SMALL: {
        fenwickI64Small *small = (fenwickI64Small *)ptr;

        /* Check if we need to upgrade to Full (check BEFORE update) */
        if (fenwickI64SmallShouldUpgrade(small) ||
            idx >= fenwickI64SmallCount(small) + 1000 ||
            idx * sizeof(int64_t) > 128 * 1024) {
            *fw = fenwickI64UpgradeSmallToFull(small);
            return fenwickI64Update(fw, idx, delta);
        }

        bool success = false;
        fenwickI64Small *newSmall =
            fenwickI64SmallUpdate(small, (uint32_t)idx, delta, &success);
        *fw = FENWICK_I64_TAG(newSmall, FENWICK_I64_TYPE_SMALL);
        return success;
    }

    case FENWICK_I64_TYPE_FULL: {
        fenwickI64Full *full = (fenwickI64Full *)ptr;

        /* No further upgrades in 2-tier system */
        bool success = false;
        fenwickI64Full *newFull =
            fenwickI64FullUpdate(full, (uint64_t)idx, delta, &success);
        *fw = FENWICK_I64_TAG(newFull, FENWICK_I64_TYPE_FULL);
        return success;
    }
    }

    return false;
}

/* Query prefix sum */
int64_t fenwickI64Query(const void *fw, size_t idx) {
    if (!fw) {
        return (int64_t)0;
    }

    fenwickI64Type type = FENWICK_I64_TYPE(fw);
    void *ptr = FENWICK_I64_UNTAG(fw);

    switch (type) {
    case FENWICK_I64_TYPE_SMALL:
        return fenwickI64SmallQuery((const fenwickI64Small *)ptr,
                                    (uint32_t)idx);
    case FENWICK_I64_TYPE_FULL:
        return fenwickI64FullQuery((const fenwickI64Full *)ptr, (uint64_t)idx);
    }

    return (int64_t)0;
}

/* Range query */
int64_t fenwickI64RangeQuery(const void *fw, size_t left, size_t right) {
    if (!fw) {
        return (int64_t)0;
    }

    fenwickI64Type type = FENWICK_I64_TYPE(fw);
    void *ptr = FENWICK_I64_UNTAG(fw);

    switch (type) {
    case FENWICK_I64_TYPE_SMALL:
        return fenwickI64SmallRangeQuery((const fenwickI64Small *)ptr,
                                         (uint32_t)left, (uint32_t)right);
    case FENWICK_I64_TYPE_FULL:
        return fenwickI64FullRangeQuery((const fenwickI64Full *)ptr,
                                        (uint64_t)left, (uint64_t)right);
    }

    return (int64_t)0;
}

/* Get single element */
int64_t fenwickI64Get(const void *fw, size_t idx) {
    if (!fw) {
        return (int64_t)0;
    }

    fenwickI64Type type = FENWICK_I64_TYPE(fw);
    void *ptr = FENWICK_I64_UNTAG(fw);

    switch (type) {
    case FENWICK_I64_TYPE_SMALL:
        return fenwickI64SmallGet((const fenwickI64Small *)ptr, (uint32_t)idx);
    case FENWICK_I64_TYPE_FULL:
        return fenwickI64FullGet((const fenwickI64Full *)ptr, (uint64_t)idx);
    }

    return (int64_t)0;
}

/* Set single element */
bool fenwickI64Set(void **fw, size_t idx, int64_t value) {
    if (!fw) {
        return false;
    }

    if (!*fw) {
        *fw = fenwickI64New();
    }

    fenwickI64Type type = FENWICK_I64_TYPE(*fw);
    void *ptr = FENWICK_I64_UNTAG(*fw);

    switch (type) {
    case FENWICK_I64_TYPE_SMALL: {
        fenwickI64Small *small = (fenwickI64Small *)ptr;

        if (fenwickI64SmallShouldUpgrade(small)) {
            *fw = fenwickI64UpgradeSmallToFull(small);
            return fenwickI64Set(fw, idx, value);
        }

        bool success = false;
        fenwickI64Small *newSmall =
            fenwickI64SmallSet(small, (uint32_t)idx, value, &success);
        *fw = FENWICK_I64_TAG(newSmall, FENWICK_I64_TYPE_SMALL);
        return success;
    }

    case FENWICK_I64_TYPE_FULL: {
        fenwickI64Full *full = (fenwickI64Full *)ptr;

        bool success = false;
        fenwickI64Full *newFull =
            fenwickI64FullSet(full, (uint64_t)idx, value, &success);
        *fw = FENWICK_I64_TAG(newFull, FENWICK_I64_TYPE_FULL);
        return success;
    }
    }

    return false;
}

/* Get count */
size_t fenwickI64Count(const void *fw) {
    if (!fw) {
        return 0;
    }

    fenwickI64Type type = FENWICK_I64_TYPE(fw);
    void *ptr = FENWICK_I64_UNTAG(fw);

    switch (type) {
    case FENWICK_I64_TYPE_SMALL:
        return fenwickI64SmallCount((const fenwickI64Small *)ptr);
    case FENWICK_I64_TYPE_FULL:
        return fenwickI64FullCount((const fenwickI64Full *)ptr);
    }

    return 0;
}

/* Get bytes */
size_t fenwickI64Bytes(const void *fw) {
    if (!fw) {
        return 0;
    }

    fenwickI64Type type = FENWICK_I64_TYPE(fw);
    void *ptr = FENWICK_I64_UNTAG(fw);

    switch (type) {
    case FENWICK_I64_TYPE_SMALL:
        return fenwickI64SmallBytes((const fenwickI64Small *)ptr);
    case FENWICK_I64_TYPE_FULL:
        return fenwickI64FullBytes((const fenwickI64Full *)ptr);
    }

    return 0;
}

/* Lower bound */
size_t fenwickI64LowerBound(const void *fw, int64_t target) {
    if (!fw) {
        return SIZE_MAX;
    }

    fenwickI64Type type = FENWICK_I64_TYPE(fw);
    void *ptr = FENWICK_I64_UNTAG(fw);

    switch (type) {
    case FENWICK_I64_TYPE_SMALL: {
        uint32_t result =
            fenwickI64SmallLowerBound((const fenwickI64Small *)ptr, target);
        return (result == (uint32_t)-1) ? SIZE_MAX : result;
    }
    case FENWICK_I64_TYPE_FULL: {
        uint64_t result =
            fenwickI64FullLowerBound((const fenwickI64Full *)ptr, target);
        return (result == (uint64_t)-1) ? SIZE_MAX : result;
    }
    }

    return SIZE_MAX;
}

/* Clear */
void fenwickI64Clear(void *fw) {
    if (!fw) {
        return;
    }

    fenwickI64Type type = FENWICK_I64_TYPE(fw);
    void *ptr = FENWICK_I64_UNTAG(fw);

    switch (type) {
    case FENWICK_I64_TYPE_SMALL:
        fenwickI64SmallClear((fenwickI64Small *)ptr);
        break;
    case FENWICK_I64_TYPE_FULL:
        fenwickI64FullClear((fenwickI64Full *)ptr);
        break;
    }
}

#ifdef DATAKIT_TEST
/* Debug representation */
void fenwickI64Repr(const void *fw) {
    if (!fw) {
        printf("fenwickI64: (nil)\n");
        return;
    }

    fenwickI64Type type = FENWICK_I64_TYPE(fw);
    void *ptr = FENWICK_I64_UNTAG(fw);

    const char *tierName = "UNKNOWN";
    switch (type) {
    case FENWICK_I64_TYPE_SMALL:
        tierName = "SMALL";
        fenwickI64SmallRepr((const fenwickI64Small *)ptr);
        break;
    case FENWICK_I64_TYPE_FULL:
        tierName = "FULL";
        fenwickI64FullRepr((const fenwickI64Full *)ptr);
        break;
    }

    printf("  Tier: %s, Count: %zu, Bytes: %zu\n", tierName,
           fenwickI64Count(fw), fenwickI64Bytes(fw));
}
#endif
