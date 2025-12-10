/* Fenwick Tree - U16 Dispatcher (2-TIER SYSTEM)
 * Auto-generated tier management and public API.
 * DO NOT EDIT MANUALLY.
 *
 * Architecture: Small → Full (single transition)
 * Benefit: 50% fewer transitions vs 3-tier system
 */

#include "fenwickU16.h"
#include "../datakit.h"

/* Create new fenwickU16 - starts as small tier */
void *fenwickU16New(void) {
    fenwickU16Small *small = fenwickU16SmallNew();
    return FENWICK_U16_TAG(small, FENWICK_U16_TYPE_SMALL);
}

/* Free fenwickU16 based on tier */
void fenwickU16Free(void *fw) {
    if (!fw) {
        return;
    }

    fenwickU16Type type = FENWICK_U16_TYPE(fw);
    void *ptr = FENWICK_U16_UNTAG(fw);

    switch (type) {
    case FENWICK_U16_TYPE_SMALL:
        fenwickU16SmallFree((fenwickU16Small *)ptr);
        break;
    case FENWICK_U16_TYPE_FULL:
        fenwickU16FullFree((fenwickU16Full *)ptr);
        break;
    }
}

/* Upgrade small to full (single transition in 2-tier system) */
static void *fenwickU16UpgradeSmallToFull(fenwickU16Small *small) {
    fenwickU16Full *full = fenwickU16FullFromSmall(small);
    fenwickU16SmallFree(small);
    return FENWICK_U16_TAG(full, FENWICK_U16_TYPE_FULL);
}

/* Update with tier transitions (2-TIER) */
bool fenwickU16Update(void **fw, size_t idx, uint16_t delta) {
    if (!fw) {
        return false;
    }

    if (!*fw) {
        *fw = fenwickU16New();
    }

    fenwickU16Type type = FENWICK_U16_TYPE(*fw);
    void *ptr = FENWICK_U16_UNTAG(*fw);

    switch (type) {
    case FENWICK_U16_TYPE_SMALL: {
        fenwickU16Small *small = (fenwickU16Small *)ptr;

        /* Check if we need to upgrade to Full (check BEFORE update) */
        if (fenwickU16SmallShouldUpgrade(small) ||
            idx >= fenwickU16SmallCount(small) + 1000 ||
            idx * sizeof(uint16_t) > 128 * 1024) {
            *fw = fenwickU16UpgradeSmallToFull(small);
            return fenwickU16Update(fw, idx, delta);
        }

        bool success = false;
        fenwickU16Small *newSmall =
            fenwickU16SmallUpdate(small, (uint32_t)idx, delta, &success);
        *fw = FENWICK_U16_TAG(newSmall, FENWICK_U16_TYPE_SMALL);
        return success;
    }

    case FENWICK_U16_TYPE_FULL: {
        fenwickU16Full *full = (fenwickU16Full *)ptr;

        /* No further upgrades in 2-tier system */
        bool success = false;
        fenwickU16Full *newFull =
            fenwickU16FullUpdate(full, (uint64_t)idx, delta, &success);
        *fw = FENWICK_U16_TAG(newFull, FENWICK_U16_TYPE_FULL);
        return success;
    }
    }

    return false;
}

/* Query prefix sum */
uint16_t fenwickU16Query(const void *fw, size_t idx) {
    if (!fw) {
        return (uint16_t)0;
    }

    fenwickU16Type type = FENWICK_U16_TYPE(fw);
    void *ptr = FENWICK_U16_UNTAG(fw);

    switch (type) {
    case FENWICK_U16_TYPE_SMALL:
        return fenwickU16SmallQuery((const fenwickU16Small *)ptr,
                                    (uint32_t)idx);
    case FENWICK_U16_TYPE_FULL:
        return fenwickU16FullQuery((const fenwickU16Full *)ptr, (uint64_t)idx);
    }

    return (uint16_t)0;
}

/* Range query */
uint16_t fenwickU16RangeQuery(const void *fw, size_t left, size_t right) {
    if (!fw) {
        return (uint16_t)0;
    }

    fenwickU16Type type = FENWICK_U16_TYPE(fw);
    void *ptr = FENWICK_U16_UNTAG(fw);

    switch (type) {
    case FENWICK_U16_TYPE_SMALL:
        return fenwickU16SmallRangeQuery((const fenwickU16Small *)ptr,
                                         (uint32_t)left, (uint32_t)right);
    case FENWICK_U16_TYPE_FULL:
        return fenwickU16FullRangeQuery((const fenwickU16Full *)ptr,
                                        (uint64_t)left, (uint64_t)right);
    }

    return (uint16_t)0;
}

/* Get single element */
uint16_t fenwickU16Get(const void *fw, size_t idx) {
    if (!fw) {
        return (uint16_t)0;
    }

    fenwickU16Type type = FENWICK_U16_TYPE(fw);
    void *ptr = FENWICK_U16_UNTAG(fw);

    switch (type) {
    case FENWICK_U16_TYPE_SMALL:
        return fenwickU16SmallGet((const fenwickU16Small *)ptr, (uint32_t)idx);
    case FENWICK_U16_TYPE_FULL:
        return fenwickU16FullGet((const fenwickU16Full *)ptr, (uint64_t)idx);
    }

    return (uint16_t)0;
}

/* Set single element */
bool fenwickU16Set(void **fw, size_t idx, uint16_t value) {
    if (!fw) {
        return false;
    }

    if (!*fw) {
        *fw = fenwickU16New();
    }

    fenwickU16Type type = FENWICK_U16_TYPE(*fw);
    void *ptr = FENWICK_U16_UNTAG(*fw);

    switch (type) {
    case FENWICK_U16_TYPE_SMALL: {
        fenwickU16Small *small = (fenwickU16Small *)ptr;

        if (fenwickU16SmallShouldUpgrade(small)) {
            *fw = fenwickU16UpgradeSmallToFull(small);
            return fenwickU16Set(fw, idx, value);
        }

        bool success = false;
        fenwickU16Small *newSmall =
            fenwickU16SmallSet(small, (uint32_t)idx, value, &success);
        *fw = FENWICK_U16_TAG(newSmall, FENWICK_U16_TYPE_SMALL);
        return success;
    }

    case FENWICK_U16_TYPE_FULL: {
        fenwickU16Full *full = (fenwickU16Full *)ptr;

        bool success = false;
        fenwickU16Full *newFull =
            fenwickU16FullSet(full, (uint64_t)idx, value, &success);
        *fw = FENWICK_U16_TAG(newFull, FENWICK_U16_TYPE_FULL);
        return success;
    }
    }

    return false;
}

/* Get count */
size_t fenwickU16Count(const void *fw) {
    if (!fw) {
        return 0;
    }

    fenwickU16Type type = FENWICK_U16_TYPE(fw);
    void *ptr = FENWICK_U16_UNTAG(fw);

    switch (type) {
    case FENWICK_U16_TYPE_SMALL:
        return fenwickU16SmallCount((const fenwickU16Small *)ptr);
    case FENWICK_U16_TYPE_FULL:
        return fenwickU16FullCount((const fenwickU16Full *)ptr);
    }

    return 0;
}

/* Get bytes */
size_t fenwickU16Bytes(const void *fw) {
    if (!fw) {
        return 0;
    }

    fenwickU16Type type = FENWICK_U16_TYPE(fw);
    void *ptr = FENWICK_U16_UNTAG(fw);

    switch (type) {
    case FENWICK_U16_TYPE_SMALL:
        return fenwickU16SmallBytes((const fenwickU16Small *)ptr);
    case FENWICK_U16_TYPE_FULL:
        return fenwickU16FullBytes((const fenwickU16Full *)ptr);
    }

    return 0;
}

/* Lower bound */
size_t fenwickU16LowerBound(const void *fw, uint16_t target) {
    if (!fw) {
        return SIZE_MAX;
    }

    fenwickU16Type type = FENWICK_U16_TYPE(fw);
    void *ptr = FENWICK_U16_UNTAG(fw);

    switch (type) {
    case FENWICK_U16_TYPE_SMALL: {
        uint32_t result =
            fenwickU16SmallLowerBound((const fenwickU16Small *)ptr, target);
        return (result == (uint32_t)-1) ? SIZE_MAX : result;
    }
    case FENWICK_U16_TYPE_FULL: {
        uint64_t result =
            fenwickU16FullLowerBound((const fenwickU16Full *)ptr, target);
        return (result == (uint64_t)-1) ? SIZE_MAX : result;
    }
    }

    return SIZE_MAX;
}

/* Clear */
void fenwickU16Clear(void *fw) {
    if (!fw) {
        return;
    }

    fenwickU16Type type = FENWICK_U16_TYPE(fw);
    void *ptr = FENWICK_U16_UNTAG(fw);

    switch (type) {
    case FENWICK_U16_TYPE_SMALL:
        fenwickU16SmallClear((fenwickU16Small *)ptr);
        break;
    case FENWICK_U16_TYPE_FULL:
        fenwickU16FullClear((fenwickU16Full *)ptr);
        break;
    }
}

#ifdef DATAKIT_TEST
/* Debug representation */
void fenwickU16Repr(const void *fw) {
    if (!fw) {
        printf("fenwickU16: (nil)\n");
        return;
    }

    fenwickU16Type type = FENWICK_U16_TYPE(fw);
    void *ptr = FENWICK_U16_UNTAG(fw);

    const char *tierName = "UNKNOWN";
    switch (type) {
    case FENWICK_U16_TYPE_SMALL:
        tierName = "SMALL";
        fenwickU16SmallRepr((const fenwickU16Small *)ptr);
        break;
    case FENWICK_U16_TYPE_FULL:
        tierName = "FULL";
        fenwickU16FullRepr((const fenwickU16Full *)ptr);
        break;
    }

    printf("  Tier: %s, Count: %zu, Bytes: %zu\n", tierName,
           fenwickU16Count(fw), fenwickU16Bytes(fw));
}
#endif
