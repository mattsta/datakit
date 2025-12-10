/* Fenwick Tree - I16 Dispatcher (2-TIER SYSTEM)
 * Auto-generated tier management and public API.
 * DO NOT EDIT MANUALLY.
 *
 * Architecture: Small → Full (single transition)
 * Benefit: 50% fewer transitions vs 3-tier system
 */

#include "fenwickI16.h"
#include "../datakit.h"

/* Create new fenwickI16 - starts as small tier */
void *fenwickI16New(void) {
    fenwickI16Small *small = fenwickI16SmallNew();
    return FENWICK_I16_TAG(small, FENWICK_I16_TYPE_SMALL);
}

/* Free fenwickI16 based on tier */
void fenwickI16Free(void *fw) {
    if (!fw) {
        return;
    }

    fenwickI16Type type = FENWICK_I16_TYPE(fw);
    void *ptr = FENWICK_I16_UNTAG(fw);

    switch (type) {
    case FENWICK_I16_TYPE_SMALL:
        fenwickI16SmallFree((fenwickI16Small *)ptr);
        break;
    case FENWICK_I16_TYPE_FULL:
        fenwickI16FullFree((fenwickI16Full *)ptr);
        break;
    }
}

/* Upgrade small to full (single transition in 2-tier system) */
static void *fenwickI16UpgradeSmallToFull(fenwickI16Small *small) {
    fenwickI16Full *full = fenwickI16FullFromSmall(small);
    fenwickI16SmallFree(small);
    return FENWICK_I16_TAG(full, FENWICK_I16_TYPE_FULL);
}

/* Update with tier transitions (2-TIER) */
bool fenwickI16Update(void **fw, size_t idx, int16_t delta) {
    if (!fw) {
        return false;
    }

    if (!*fw) {
        *fw = fenwickI16New();
    }

    fenwickI16Type type = FENWICK_I16_TYPE(*fw);
    void *ptr = FENWICK_I16_UNTAG(*fw);

    switch (type) {
    case FENWICK_I16_TYPE_SMALL: {
        fenwickI16Small *small = (fenwickI16Small *)ptr;

        /* Check if we need to upgrade to Full (check BEFORE update) */
        if (fenwickI16SmallShouldUpgrade(small) ||
            idx >= fenwickI16SmallCount(small) + 1000 ||
            idx * sizeof(int16_t) > 128 * 1024) {
            *fw = fenwickI16UpgradeSmallToFull(small);
            return fenwickI16Update(fw, idx, delta);
        }

        bool success = false;
        fenwickI16Small *newSmall =
            fenwickI16SmallUpdate(small, (uint32_t)idx, delta, &success);
        *fw = FENWICK_I16_TAG(newSmall, FENWICK_I16_TYPE_SMALL);
        return success;
    }

    case FENWICK_I16_TYPE_FULL: {
        fenwickI16Full *full = (fenwickI16Full *)ptr;

        /* No further upgrades in 2-tier system */
        bool success = false;
        fenwickI16Full *newFull =
            fenwickI16FullUpdate(full, (uint64_t)idx, delta, &success);
        *fw = FENWICK_I16_TAG(newFull, FENWICK_I16_TYPE_FULL);
        return success;
    }
    }

    return false;
}

/* Query prefix sum */
int16_t fenwickI16Query(const void *fw, size_t idx) {
    if (!fw) {
        return (int16_t)0;
    }

    fenwickI16Type type = FENWICK_I16_TYPE(fw);
    void *ptr = FENWICK_I16_UNTAG(fw);

    switch (type) {
    case FENWICK_I16_TYPE_SMALL:
        return fenwickI16SmallQuery((const fenwickI16Small *)ptr,
                                    (uint32_t)idx);
    case FENWICK_I16_TYPE_FULL:
        return fenwickI16FullQuery((const fenwickI16Full *)ptr, (uint64_t)idx);
    }

    return (int16_t)0;
}

/* Range query */
int16_t fenwickI16RangeQuery(const void *fw, size_t left, size_t right) {
    if (!fw) {
        return (int16_t)0;
    }

    fenwickI16Type type = FENWICK_I16_TYPE(fw);
    void *ptr = FENWICK_I16_UNTAG(fw);

    switch (type) {
    case FENWICK_I16_TYPE_SMALL:
        return fenwickI16SmallRangeQuery((const fenwickI16Small *)ptr,
                                         (uint32_t)left, (uint32_t)right);
    case FENWICK_I16_TYPE_FULL:
        return fenwickI16FullRangeQuery((const fenwickI16Full *)ptr,
                                        (uint64_t)left, (uint64_t)right);
    }

    return (int16_t)0;
}

/* Get single element */
int16_t fenwickI16Get(const void *fw, size_t idx) {
    if (!fw) {
        return (int16_t)0;
    }

    fenwickI16Type type = FENWICK_I16_TYPE(fw);
    void *ptr = FENWICK_I16_UNTAG(fw);

    switch (type) {
    case FENWICK_I16_TYPE_SMALL:
        return fenwickI16SmallGet((const fenwickI16Small *)ptr, (uint32_t)idx);
    case FENWICK_I16_TYPE_FULL:
        return fenwickI16FullGet((const fenwickI16Full *)ptr, (uint64_t)idx);
    }

    return (int16_t)0;
}

/* Set single element */
bool fenwickI16Set(void **fw, size_t idx, int16_t value) {
    if (!fw) {
        return false;
    }

    if (!*fw) {
        *fw = fenwickI16New();
    }

    fenwickI16Type type = FENWICK_I16_TYPE(*fw);
    void *ptr = FENWICK_I16_UNTAG(*fw);

    switch (type) {
    case FENWICK_I16_TYPE_SMALL: {
        fenwickI16Small *small = (fenwickI16Small *)ptr;

        if (fenwickI16SmallShouldUpgrade(small)) {
            *fw = fenwickI16UpgradeSmallToFull(small);
            return fenwickI16Set(fw, idx, value);
        }

        bool success = false;
        fenwickI16Small *newSmall =
            fenwickI16SmallSet(small, (uint32_t)idx, value, &success);
        *fw = FENWICK_I16_TAG(newSmall, FENWICK_I16_TYPE_SMALL);
        return success;
    }

    case FENWICK_I16_TYPE_FULL: {
        fenwickI16Full *full = (fenwickI16Full *)ptr;

        bool success = false;
        fenwickI16Full *newFull =
            fenwickI16FullSet(full, (uint64_t)idx, value, &success);
        *fw = FENWICK_I16_TAG(newFull, FENWICK_I16_TYPE_FULL);
        return success;
    }
    }

    return false;
}

/* Get count */
size_t fenwickI16Count(const void *fw) {
    if (!fw) {
        return 0;
    }

    fenwickI16Type type = FENWICK_I16_TYPE(fw);
    void *ptr = FENWICK_I16_UNTAG(fw);

    switch (type) {
    case FENWICK_I16_TYPE_SMALL:
        return fenwickI16SmallCount((const fenwickI16Small *)ptr);
    case FENWICK_I16_TYPE_FULL:
        return fenwickI16FullCount((const fenwickI16Full *)ptr);
    }

    return 0;
}

/* Get bytes */
size_t fenwickI16Bytes(const void *fw) {
    if (!fw) {
        return 0;
    }

    fenwickI16Type type = FENWICK_I16_TYPE(fw);
    void *ptr = FENWICK_I16_UNTAG(fw);

    switch (type) {
    case FENWICK_I16_TYPE_SMALL:
        return fenwickI16SmallBytes((const fenwickI16Small *)ptr);
    case FENWICK_I16_TYPE_FULL:
        return fenwickI16FullBytes((const fenwickI16Full *)ptr);
    }

    return 0;
}

/* Lower bound */
size_t fenwickI16LowerBound(const void *fw, int16_t target) {
    if (!fw) {
        return SIZE_MAX;
    }

    fenwickI16Type type = FENWICK_I16_TYPE(fw);
    void *ptr = FENWICK_I16_UNTAG(fw);

    switch (type) {
    case FENWICK_I16_TYPE_SMALL: {
        uint32_t result =
            fenwickI16SmallLowerBound((const fenwickI16Small *)ptr, target);
        return (result == (uint32_t)-1) ? SIZE_MAX : result;
    }
    case FENWICK_I16_TYPE_FULL: {
        uint64_t result =
            fenwickI16FullLowerBound((const fenwickI16Full *)ptr, target);
        return (result == (uint64_t)-1) ? SIZE_MAX : result;
    }
    }

    return SIZE_MAX;
}

/* Clear */
void fenwickI16Clear(void *fw) {
    if (!fw) {
        return;
    }

    fenwickI16Type type = FENWICK_I16_TYPE(fw);
    void *ptr = FENWICK_I16_UNTAG(fw);

    switch (type) {
    case FENWICK_I16_TYPE_SMALL:
        fenwickI16SmallClear((fenwickI16Small *)ptr);
        break;
    case FENWICK_I16_TYPE_FULL:
        fenwickI16FullClear((fenwickI16Full *)ptr);
        break;
    }
}

#ifdef DATAKIT_TEST
/* Debug representation */
void fenwickI16Repr(const void *fw) {
    if (!fw) {
        printf("fenwickI16: (nil)\n");
        return;
    }

    fenwickI16Type type = FENWICK_I16_TYPE(fw);
    void *ptr = FENWICK_I16_UNTAG(fw);

    const char *tierName = "UNKNOWN";
    switch (type) {
    case FENWICK_I16_TYPE_SMALL:
        tierName = "SMALL";
        fenwickI16SmallRepr((const fenwickI16Small *)ptr);
        break;
    case FENWICK_I16_TYPE_FULL:
        tierName = "FULL";
        fenwickI16FullRepr((const fenwickI16Full *)ptr);
        break;
    }

    printf("  Tier: %s, Count: %zu, Bytes: %zu\n", tierName,
           fenwickI16Count(fw), fenwickI16Bytes(fw));
}
#endif
