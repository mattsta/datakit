/* Fenwick Tree - Float Dispatcher (2-TIER SYSTEM)
 * Auto-generated tier management and public API.
 * DO NOT EDIT MANUALLY.
 *
 * Architecture: Small → Full (single transition)
 * Benefit: 50% fewer transitions vs 3-tier system
 */

#include "fenwickFloat.h"
#include "../datakit.h"

/* Create new fenwickFloat - starts as small tier */
void *fenwickFloatNew(void) {
    fenwickFloatSmall *small = fenwickFloatSmallNew();
    return FENWICK_FLOAT_TAG(small, FENWICK_FLOAT_TYPE_SMALL);
}

/* Free fenwickFloat based on tier */
void fenwickFloatFree(void *fw) {
    if (!fw) {
        return;
    }

    fenwickFloatType type = FENWICK_FLOAT_TYPE(fw);
    void *ptr = FENWICK_FLOAT_UNTAG(fw);

    switch (type) {
    case FENWICK_FLOAT_TYPE_SMALL:
        fenwickFloatSmallFree((fenwickFloatSmall *)ptr);
        break;
    case FENWICK_FLOAT_TYPE_FULL:
        fenwickFloatFullFree((fenwickFloatFull *)ptr);
        break;
    }
}

/* Upgrade small to full (single transition in 2-tier system) */
static void *fenwickFloatUpgradeSmallToFull(fenwickFloatSmall *small) {
    fenwickFloatFull *full = fenwickFloatFullFromSmall(small);
    fenwickFloatSmallFree(small);
    return FENWICK_FLOAT_TAG(full, FENWICK_FLOAT_TYPE_FULL);
}

/* Update with tier transitions (2-TIER) */
bool fenwickFloatUpdate(void **fw, size_t idx, float delta) {
    if (!fw) {
        return false;
    }

    if (!*fw) {
        *fw = fenwickFloatNew();
    }

    fenwickFloatType type = FENWICK_FLOAT_TYPE(*fw);
    void *ptr = FENWICK_FLOAT_UNTAG(*fw);

    switch (type) {
    case FENWICK_FLOAT_TYPE_SMALL: {
        fenwickFloatSmall *small = (fenwickFloatSmall *)ptr;

        /* Check if we need to upgrade to Full (check BEFORE update) */
        if (fenwickFloatSmallShouldUpgrade(small) ||
            idx >= fenwickFloatSmallCount(small) + 1000 ||
            idx * sizeof(float) > 128 * 1024) {
            *fw = fenwickFloatUpgradeSmallToFull(small);
            return fenwickFloatUpdate(fw, idx, delta);
        }

        bool success = false;
        fenwickFloatSmall *newSmall =
            fenwickFloatSmallUpdate(small, (uint32_t)idx, delta, &success);
        *fw = FENWICK_FLOAT_TAG(newSmall, FENWICK_FLOAT_TYPE_SMALL);
        return success;
    }

    case FENWICK_FLOAT_TYPE_FULL: {
        fenwickFloatFull *full = (fenwickFloatFull *)ptr;

        /* No further upgrades in 2-tier system */
        bool success = false;
        fenwickFloatFull *newFull =
            fenwickFloatFullUpdate(full, (uint64_t)idx, delta, &success);
        *fw = FENWICK_FLOAT_TAG(newFull, FENWICK_FLOAT_TYPE_FULL);
        return success;
    }
    }

    return false;
}

/* Query prefix sum */
float fenwickFloatQuery(const void *fw, size_t idx) {
    if (!fw) {
        return (float)0;
    }

    fenwickFloatType type = FENWICK_FLOAT_TYPE(fw);
    void *ptr = FENWICK_FLOAT_UNTAG(fw);

    switch (type) {
    case FENWICK_FLOAT_TYPE_SMALL:
        return fenwickFloatSmallQuery((const fenwickFloatSmall *)ptr,
                                      (uint32_t)idx);
    case FENWICK_FLOAT_TYPE_FULL:
        return fenwickFloatFullQuery((const fenwickFloatFull *)ptr,
                                     (uint64_t)idx);
    }

    return (float)0;
}

/* Range query */
float fenwickFloatRangeQuery(const void *fw, size_t left, size_t right) {
    if (!fw) {
        return (float)0;
    }

    fenwickFloatType type = FENWICK_FLOAT_TYPE(fw);
    void *ptr = FENWICK_FLOAT_UNTAG(fw);

    switch (type) {
    case FENWICK_FLOAT_TYPE_SMALL:
        return fenwickFloatSmallRangeQuery((const fenwickFloatSmall *)ptr,
                                           (uint32_t)left, (uint32_t)right);
    case FENWICK_FLOAT_TYPE_FULL:
        return fenwickFloatFullRangeQuery((const fenwickFloatFull *)ptr,
                                          (uint64_t)left, (uint64_t)right);
    }

    return (float)0;
}

/* Get single element */
float fenwickFloatGet(const void *fw, size_t idx) {
    if (!fw) {
        return (float)0;
    }

    fenwickFloatType type = FENWICK_FLOAT_TYPE(fw);
    void *ptr = FENWICK_FLOAT_UNTAG(fw);

    switch (type) {
    case FENWICK_FLOAT_TYPE_SMALL:
        return fenwickFloatSmallGet((const fenwickFloatSmall *)ptr,
                                    (uint32_t)idx);
    case FENWICK_FLOAT_TYPE_FULL:
        return fenwickFloatFullGet((const fenwickFloatFull *)ptr,
                                   (uint64_t)idx);
    }

    return (float)0;
}

/* Set single element */
bool fenwickFloatSet(void **fw, size_t idx, float value) {
    if (!fw) {
        return false;
    }

    if (!*fw) {
        *fw = fenwickFloatNew();
    }

    fenwickFloatType type = FENWICK_FLOAT_TYPE(*fw);
    void *ptr = FENWICK_FLOAT_UNTAG(*fw);

    switch (type) {
    case FENWICK_FLOAT_TYPE_SMALL: {
        fenwickFloatSmall *small = (fenwickFloatSmall *)ptr;

        if (fenwickFloatSmallShouldUpgrade(small)) {
            *fw = fenwickFloatUpgradeSmallToFull(small);
            return fenwickFloatSet(fw, idx, value);
        }

        bool success = false;
        fenwickFloatSmall *newSmall =
            fenwickFloatSmallSet(small, (uint32_t)idx, value, &success);
        *fw = FENWICK_FLOAT_TAG(newSmall, FENWICK_FLOAT_TYPE_SMALL);
        return success;
    }

    case FENWICK_FLOAT_TYPE_FULL: {
        fenwickFloatFull *full = (fenwickFloatFull *)ptr;

        bool success = false;
        fenwickFloatFull *newFull =
            fenwickFloatFullSet(full, (uint64_t)idx, value, &success);
        *fw = FENWICK_FLOAT_TAG(newFull, FENWICK_FLOAT_TYPE_FULL);
        return success;
    }
    }

    return false;
}

/* Get count */
size_t fenwickFloatCount(const void *fw) {
    if (!fw) {
        return 0;
    }

    fenwickFloatType type = FENWICK_FLOAT_TYPE(fw);
    void *ptr = FENWICK_FLOAT_UNTAG(fw);

    switch (type) {
    case FENWICK_FLOAT_TYPE_SMALL:
        return fenwickFloatSmallCount((const fenwickFloatSmall *)ptr);
    case FENWICK_FLOAT_TYPE_FULL:
        return fenwickFloatFullCount((const fenwickFloatFull *)ptr);
    }

    return 0;
}

/* Get bytes */
size_t fenwickFloatBytes(const void *fw) {
    if (!fw) {
        return 0;
    }

    fenwickFloatType type = FENWICK_FLOAT_TYPE(fw);
    void *ptr = FENWICK_FLOAT_UNTAG(fw);

    switch (type) {
    case FENWICK_FLOAT_TYPE_SMALL:
        return fenwickFloatSmallBytes((const fenwickFloatSmall *)ptr);
    case FENWICK_FLOAT_TYPE_FULL:
        return fenwickFloatFullBytes((const fenwickFloatFull *)ptr);
    }

    return 0;
}

/* Lower bound */
size_t fenwickFloatLowerBound(const void *fw, float target) {
    if (!fw) {
        return SIZE_MAX;
    }

    fenwickFloatType type = FENWICK_FLOAT_TYPE(fw);
    void *ptr = FENWICK_FLOAT_UNTAG(fw);

    switch (type) {
    case FENWICK_FLOAT_TYPE_SMALL: {
        uint32_t result =
            fenwickFloatSmallLowerBound((const fenwickFloatSmall *)ptr, target);
        return (result == (uint32_t)-1) ? SIZE_MAX : result;
    }
    case FENWICK_FLOAT_TYPE_FULL: {
        uint64_t result =
            fenwickFloatFullLowerBound((const fenwickFloatFull *)ptr, target);
        return (result == (uint64_t)-1) ? SIZE_MAX : result;
    }
    }

    return SIZE_MAX;
}

/* Clear */
void fenwickFloatClear(void *fw) {
    if (!fw) {
        return;
    }

    fenwickFloatType type = FENWICK_FLOAT_TYPE(fw);
    void *ptr = FENWICK_FLOAT_UNTAG(fw);

    switch (type) {
    case FENWICK_FLOAT_TYPE_SMALL:
        fenwickFloatSmallClear((fenwickFloatSmall *)ptr);
        break;
    case FENWICK_FLOAT_TYPE_FULL:
        fenwickFloatFullClear((fenwickFloatFull *)ptr);
        break;
    }
}

#ifdef DATAKIT_TEST
/* Debug representation */
void fenwickFloatRepr(const void *fw) {
    if (!fw) {
        printf("fenwickFloat: (nil)\n");
        return;
    }

    fenwickFloatType type = FENWICK_FLOAT_TYPE(fw);
    void *ptr = FENWICK_FLOAT_UNTAG(fw);

    const char *tierName = "UNKNOWN";
    switch (type) {
    case FENWICK_FLOAT_TYPE_SMALL:
        tierName = "SMALL";
        fenwickFloatSmallRepr((const fenwickFloatSmall *)ptr);
        break;
    case FENWICK_FLOAT_TYPE_FULL:
        tierName = "FULL";
        fenwickFloatFullRepr((const fenwickFloatFull *)ptr);
        break;
    }

    printf("  Tier: %s, Count: %zu, Bytes: %zu\n", tierName,
           fenwickFloatCount(fw), fenwickFloatBytes(fw));
}
#endif
