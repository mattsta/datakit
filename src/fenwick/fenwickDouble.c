/* Fenwick Tree - Double Dispatcher (2-TIER SYSTEM)
 * Auto-generated tier management and public API.
 * DO NOT EDIT MANUALLY.
 *
 * Architecture: Small → Full (single transition)
 * Benefit: 50% fewer transitions vs 3-tier system
 */

#include "fenwickDouble.h"
#include "../datakit.h"

/* Create new fenwickDouble - starts as small tier */
void *fenwickDoubleNew(void) {
    fenwickDoubleSmall *small = fenwickDoubleSmallNew();
    return FENWICK_DOUBLE_TAG(small, FENWICK_DOUBLE_TYPE_SMALL);
}

/* Free fenwickDouble based on tier */
void fenwickDoubleFree(void *fw) {
    if (!fw) {
        return;
    }

    fenwickDoubleType type = FENWICK_DOUBLE_TYPE(fw);
    void *ptr = FENWICK_DOUBLE_UNTAG(fw);

    switch (type) {
    case FENWICK_DOUBLE_TYPE_SMALL:
        fenwickDoubleSmallFree((fenwickDoubleSmall *)ptr);
        break;
    case FENWICK_DOUBLE_TYPE_FULL:
        fenwickDoubleFullFree((fenwickDoubleFull *)ptr);
        break;
    }
}

/* Upgrade small to full (single transition in 2-tier system) */
static void *fenwickDoubleUpgradeSmallToFull(fenwickDoubleSmall *small) {
    fenwickDoubleFull *full = fenwickDoubleFullFromSmall(small);
    fenwickDoubleSmallFree(small);
    return FENWICK_DOUBLE_TAG(full, FENWICK_DOUBLE_TYPE_FULL);
}

/* Update with tier transitions (2-TIER) */
bool fenwickDoubleUpdate(void **fw, size_t idx, double delta) {
    if (!fw) {
        return false;
    }

    if (!*fw) {
        *fw = fenwickDoubleNew();
    }

    fenwickDoubleType type = FENWICK_DOUBLE_TYPE(*fw);
    void *ptr = FENWICK_DOUBLE_UNTAG(*fw);

    switch (type) {
    case FENWICK_DOUBLE_TYPE_SMALL: {
        fenwickDoubleSmall *small = (fenwickDoubleSmall *)ptr;

        /* Check if we need to upgrade to Full (check BEFORE update) */
        if (fenwickDoubleSmallShouldUpgrade(small) ||
            idx >= fenwickDoubleSmallCount(small) + 1000 ||
            idx * sizeof(double) > 128 * 1024) {
            *fw = fenwickDoubleUpgradeSmallToFull(small);
            return fenwickDoubleUpdate(fw, idx, delta);
        }

        bool success = false;
        fenwickDoubleSmall *newSmall =
            fenwickDoubleSmallUpdate(small, (uint32_t)idx, delta, &success);
        *fw = FENWICK_DOUBLE_TAG(newSmall, FENWICK_DOUBLE_TYPE_SMALL);
        return success;
    }

    case FENWICK_DOUBLE_TYPE_FULL: {
        fenwickDoubleFull *full = (fenwickDoubleFull *)ptr;

        /* No further upgrades in 2-tier system */
        bool success = false;
        fenwickDoubleFull *newFull =
            fenwickDoubleFullUpdate(full, (uint64_t)idx, delta, &success);
        *fw = FENWICK_DOUBLE_TAG(newFull, FENWICK_DOUBLE_TYPE_FULL);
        return success;
    }
    }

    return false;
}

/* Query prefix sum */
double fenwickDoubleQuery(const void *fw, size_t idx) {
    if (!fw) {
        return (double)0;
    }

    fenwickDoubleType type = FENWICK_DOUBLE_TYPE(fw);
    void *ptr = FENWICK_DOUBLE_UNTAG(fw);

    switch (type) {
    case FENWICK_DOUBLE_TYPE_SMALL:
        return fenwickDoubleSmallQuery((const fenwickDoubleSmall *)ptr,
                                       (uint32_t)idx);
    case FENWICK_DOUBLE_TYPE_FULL:
        return fenwickDoubleFullQuery((const fenwickDoubleFull *)ptr,
                                      (uint64_t)idx);
    }

    return (double)0;
}

/* Range query */
double fenwickDoubleRangeQuery(const void *fw, size_t left, size_t right) {
    if (!fw) {
        return (double)0;
    }

    fenwickDoubleType type = FENWICK_DOUBLE_TYPE(fw);
    void *ptr = FENWICK_DOUBLE_UNTAG(fw);

    switch (type) {
    case FENWICK_DOUBLE_TYPE_SMALL:
        return fenwickDoubleSmallRangeQuery((const fenwickDoubleSmall *)ptr,
                                            (uint32_t)left, (uint32_t)right);
    case FENWICK_DOUBLE_TYPE_FULL:
        return fenwickDoubleFullRangeQuery((const fenwickDoubleFull *)ptr,
                                           (uint64_t)left, (uint64_t)right);
    }

    return (double)0;
}

/* Get single element */
double fenwickDoubleGet(const void *fw, size_t idx) {
    if (!fw) {
        return (double)0;
    }

    fenwickDoubleType type = FENWICK_DOUBLE_TYPE(fw);
    void *ptr = FENWICK_DOUBLE_UNTAG(fw);

    switch (type) {
    case FENWICK_DOUBLE_TYPE_SMALL:
        return fenwickDoubleSmallGet((const fenwickDoubleSmall *)ptr,
                                     (uint32_t)idx);
    case FENWICK_DOUBLE_TYPE_FULL:
        return fenwickDoubleFullGet((const fenwickDoubleFull *)ptr,
                                    (uint64_t)idx);
    }

    return (double)0;
}

/* Set single element */
bool fenwickDoubleSet(void **fw, size_t idx, double value) {
    if (!fw) {
        return false;
    }

    if (!*fw) {
        *fw = fenwickDoubleNew();
    }

    fenwickDoubleType type = FENWICK_DOUBLE_TYPE(*fw);
    void *ptr = FENWICK_DOUBLE_UNTAG(*fw);

    switch (type) {
    case FENWICK_DOUBLE_TYPE_SMALL: {
        fenwickDoubleSmall *small = (fenwickDoubleSmall *)ptr;

        if (fenwickDoubleSmallShouldUpgrade(small)) {
            *fw = fenwickDoubleUpgradeSmallToFull(small);
            return fenwickDoubleSet(fw, idx, value);
        }

        bool success = false;
        fenwickDoubleSmall *newSmall =
            fenwickDoubleSmallSet(small, (uint32_t)idx, value, &success);
        *fw = FENWICK_DOUBLE_TAG(newSmall, FENWICK_DOUBLE_TYPE_SMALL);
        return success;
    }

    case FENWICK_DOUBLE_TYPE_FULL: {
        fenwickDoubleFull *full = (fenwickDoubleFull *)ptr;

        bool success = false;
        fenwickDoubleFull *newFull =
            fenwickDoubleFullSet(full, (uint64_t)idx, value, &success);
        *fw = FENWICK_DOUBLE_TAG(newFull, FENWICK_DOUBLE_TYPE_FULL);
        return success;
    }
    }

    return false;
}

/* Get count */
size_t fenwickDoubleCount(const void *fw) {
    if (!fw) {
        return 0;
    }

    fenwickDoubleType type = FENWICK_DOUBLE_TYPE(fw);
    void *ptr = FENWICK_DOUBLE_UNTAG(fw);

    switch (type) {
    case FENWICK_DOUBLE_TYPE_SMALL:
        return fenwickDoubleSmallCount((const fenwickDoubleSmall *)ptr);
    case FENWICK_DOUBLE_TYPE_FULL:
        return fenwickDoubleFullCount((const fenwickDoubleFull *)ptr);
    }

    return 0;
}

/* Get bytes */
size_t fenwickDoubleBytes(const void *fw) {
    if (!fw) {
        return 0;
    }

    fenwickDoubleType type = FENWICK_DOUBLE_TYPE(fw);
    void *ptr = FENWICK_DOUBLE_UNTAG(fw);

    switch (type) {
    case FENWICK_DOUBLE_TYPE_SMALL:
        return fenwickDoubleSmallBytes((const fenwickDoubleSmall *)ptr);
    case FENWICK_DOUBLE_TYPE_FULL:
        return fenwickDoubleFullBytes((const fenwickDoubleFull *)ptr);
    }

    return 0;
}

/* Lower bound */
size_t fenwickDoubleLowerBound(const void *fw, double target) {
    if (!fw) {
        return SIZE_MAX;
    }

    fenwickDoubleType type = FENWICK_DOUBLE_TYPE(fw);
    void *ptr = FENWICK_DOUBLE_UNTAG(fw);

    switch (type) {
    case FENWICK_DOUBLE_TYPE_SMALL: {
        uint32_t result = fenwickDoubleSmallLowerBound(
            (const fenwickDoubleSmall *)ptr, target);
        return (result == (uint32_t)-1) ? SIZE_MAX : result;
    }
    case FENWICK_DOUBLE_TYPE_FULL: {
        uint64_t result =
            fenwickDoubleFullLowerBound((const fenwickDoubleFull *)ptr, target);
        return (result == (uint64_t)-1) ? SIZE_MAX : result;
    }
    }

    return SIZE_MAX;
}

/* Clear */
void fenwickDoubleClear(void *fw) {
    if (!fw) {
        return;
    }

    fenwickDoubleType type = FENWICK_DOUBLE_TYPE(fw);
    void *ptr = FENWICK_DOUBLE_UNTAG(fw);

    switch (type) {
    case FENWICK_DOUBLE_TYPE_SMALL:
        fenwickDoubleSmallClear((fenwickDoubleSmall *)ptr);
        break;
    case FENWICK_DOUBLE_TYPE_FULL:
        fenwickDoubleFullClear((fenwickDoubleFull *)ptr);
        break;
    }
}

#ifdef DATAKIT_TEST
/* Debug representation */
void fenwickDoubleRepr(const void *fw) {
    if (!fw) {
        printf("fenwickDouble: (nil)\n");
        return;
    }

    fenwickDoubleType type = FENWICK_DOUBLE_TYPE(fw);
    void *ptr = FENWICK_DOUBLE_UNTAG(fw);

    const char *tierName = "UNKNOWN";
    switch (type) {
    case FENWICK_DOUBLE_TYPE_SMALL:
        tierName = "SMALL";
        fenwickDoubleSmallRepr((const fenwickDoubleSmall *)ptr);
        break;
    case FENWICK_DOUBLE_TYPE_FULL:
        tierName = "FULL";
        fenwickDoubleFullRepr((const fenwickDoubleFull *)ptr);
        break;
    }

    printf("  Tier: %s, Count: %zu, Bytes: %zu\n", tierName,
           fenwickDoubleCount(fw), fenwickDoubleBytes(fw));
}
#endif
