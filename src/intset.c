#include "intset.h"
#include "datakit.h"
#include "intsetCommon.h"
#include "intsetFull.h"
#include "intsetMedium.h"
#include "intsetSmall.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Create new intset - starts as small tier */
intset *intsetNew(void) {
    intsetSmall *small = intsetSmallNew();
    return INTSET_TAG(small, INTSET_TYPE_SMALL);
}

/* Free intset based on tier */
void intsetFree(intset *is) {
    if (!is) {
        return;
    }

    intsetType type = INTSET_TYPE(is);
    void *ptr = INTSET_UNTAG(is);

    switch (type) {
    case INTSET_TYPE_SMALL:
        intsetSmallFree((intsetSmall *)ptr);
        break;
    case INTSET_TYPE_MEDIUM:
        intsetMediumFree((intsetMedium *)ptr);
        break;
    case INTSET_TYPE_FULL:
        intsetFullFree((intsetFull *)ptr);
        break;
    }
}

/* Upgrade small to medium */
static intset *intsetUpgradeSmallToMedium(intsetSmall *small) {
    intsetMedium *medium = intsetMediumFromSmall(small);
    intsetSmallFree(small);
    return INTSET_TAG(medium, INTSET_TYPE_MEDIUM);
}

/* Upgrade medium to full */
static intset *intsetUpgradeMediumToFull(intsetMedium *medium) {
    intsetFull *full = intsetFullFromMedium(medium);
    intsetMediumFree(medium);
    return INTSET_TAG(full, INTSET_TYPE_FULL);
}

/* Add value to intset with tier transitions */
void intsetAdd(intset **is, int64_t value, bool *success) {
    if (!is) {
        if (success) {
            *success = false;
        }
        return;
    }

    if (!*is) {
        *is = intsetNew();
    }

    intsetType type = INTSET_TYPE(*is);
    void *ptr = INTSET_UNTAG(*is);

    switch (type) {
    case INTSET_TYPE_SMALL: {
        intsetSmall *small = (intsetSmall *)ptr;

        /* Check if we need to upgrade before adding */
        if (intsetSmallShouldUpgrade(small, value)) {
            /* Upgrade to medium */
            *is = intsetUpgradeSmallToMedium(small);
            /* Recursively add to new tier */
            intsetAdd(is, value, success);
            return;
        }

        /* Add to small tier */
        bool added = false;
        intsetSmall *newSmall = intsetSmallAdd(small, value, &added);
        *is = INTSET_TAG(newSmall, INTSET_TYPE_SMALL);

        if (success) {
            *success = added;
        }
        break;
    }

    case INTSET_TYPE_MEDIUM: {
        intsetMedium *medium = (intsetMedium *)ptr;

        /* Check if we need to upgrade before adding */
        if (intsetMediumShouldUpgrade(medium, value)) {
            /* Upgrade to full */
            *is = intsetUpgradeMediumToFull(medium);
            /* Recursively add to new tier */
            intsetAdd(is, value, success);
            return;
        }

        /* Add to medium tier */
        bool added = false;
        intsetMedium *newMedium = intsetMediumAdd(medium, value, &added);
        *is = INTSET_TAG(newMedium, INTSET_TYPE_MEDIUM);

        if (success) {
            *success = added;
        }
        break;
    }

    case INTSET_TYPE_FULL: {
        intsetFull *full = (intsetFull *)ptr;

        /* Add to full tier (no further upgrades) */
        bool added = false;
        intsetFull *newFull = intsetFullAdd(full, value, &added);
        *is = INTSET_TAG(newFull, INTSET_TYPE_FULL);

        if (success) {
            *success = added;
        }
        break;
    }
    }
}

/* Remove value from intset */
void intsetRemove(intset **is, int64_t value, bool *success) {
    if (!is || !*is) {
        if (success) {
            *success = false;
        }
        return;
    }

    intsetType type = INTSET_TYPE(*is);
    void *ptr = INTSET_UNTAG(*is);

    bool removed = false;

    switch (type) {
    case INTSET_TYPE_SMALL: {
        intsetSmall *small = (intsetSmall *)ptr;
        intsetSmall *newSmall = intsetSmallRemove(small, value, &removed);
        *is = INTSET_TAG(newSmall, INTSET_TYPE_SMALL);
        break;
    }

    case INTSET_TYPE_MEDIUM: {
        intsetMedium *medium = (intsetMedium *)ptr;
        intsetMedium *newMedium = intsetMediumRemove(medium, value, &removed);
        *is = INTSET_TAG(newMedium, INTSET_TYPE_MEDIUM);
        break;
    }

    case INTSET_TYPE_FULL: {
        intsetFull *full = (intsetFull *)ptr;
        intsetFull *newFull = intsetFullRemove(full, value, &removed);
        *is = INTSET_TAG(newFull, INTSET_TYPE_FULL);
        break;
    }
    }

    if (success) {
        *success = removed;
    }
}

/* Find if value exists */
bool intsetFind(intset *is, int64_t value) {
    if (!is) {
        return false;
    }

    intsetType type = INTSET_TYPE(is);
    void *ptr = INTSET_UNTAG(is);

    switch (type) {
    case INTSET_TYPE_SMALL: {
        intsetSmall *small = (intsetSmall *)ptr;
        uint32_t pos;
        return intsetSmallFind(small, value, &pos) == INTSET_FOUND;
    }

    case INTSET_TYPE_MEDIUM: {
        intsetMedium *medium = (intsetMedium *)ptr;
        uint64_t pos;
        return intsetMediumFind(medium, value, &pos) == INTSET_FOUND;
    }

    case INTSET_TYPE_FULL: {
        intsetFull *full = (intsetFull *)ptr;
        uint64_t pos;
        return intsetFullFind(full, value, &pos) == INTSET_FOUND;
    }
    }

    return false;
}

/* Get value at position */
bool intsetGet(intset *is, uint32_t pos, int64_t *value) {
    if (!is) {
        return false;
    }

    intsetType type = INTSET_TYPE(is);
    void *ptr = INTSET_UNTAG(is);

    switch (type) {
    case INTSET_TYPE_SMALL: {
        intsetSmall *small = (intsetSmall *)ptr;
        return intsetSmallGet(small, pos, value);
    }

    case INTSET_TYPE_MEDIUM: {
        intsetMedium *medium = (intsetMedium *)ptr;
        return intsetMediumGet(medium, pos, value);
    }

    case INTSET_TYPE_FULL: {
        intsetFull *full = (intsetFull *)ptr;
        return intsetFullGet(full, pos, value);
    }
    }

    return false;
}

/* Get random value */
int64_t intsetRandom(intset *is) {
    size_t count = intsetCount(is);
    if (count == 0) {
        return 0;
    }

    uint32_t pos = rand() % count;
    int64_t value = 0;
    intsetGet(is, pos, &value);
    return value;
}

/* Get total count */
size_t intsetCount(const intset *is) {
    if (!is) {
        return 0;
    }

    intsetType type = INTSET_TYPE(is);
    void *ptr = INTSET_UNTAG(is);

    switch (type) {
    case INTSET_TYPE_SMALL: {
        intsetSmall *small = (intsetSmall *)ptr;
        return intsetSmallCount(small);
    }

    case INTSET_TYPE_MEDIUM: {
        intsetMedium *medium = (intsetMedium *)ptr;
        return intsetMediumCount(medium);
    }

    case INTSET_TYPE_FULL: {
        intsetFull *full = (intsetFull *)ptr;
        return intsetFullCount(full);
    }
    }

    return 0;
}

/* Get total bytes */
size_t intsetBytes(intset *is) {
    if (!is) {
        return 0;
    }

    intsetType type = INTSET_TYPE(is);
    void *ptr = INTSET_UNTAG(is);

    switch (type) {
    case INTSET_TYPE_SMALL: {
        intsetSmall *small = (intsetSmall *)ptr;
        return intsetSmallBytes(small);
    }

    case INTSET_TYPE_MEDIUM: {
        intsetMedium *medium = (intsetMedium *)ptr;
        return intsetMediumBytes(medium);
    }

    case INTSET_TYPE_FULL: {
        intsetFull *full = (intsetFull *)ptr;
        return intsetFullBytes(full);
    }
    }

    return 0;
}

#ifdef DATAKIT_TEST
/* Print representation of intset */
void intsetRepr(const intset *is) {
    if (!is) {
        printf("(nil)\n");
        return;
    }

    intsetType type = INTSET_TYPE(is);
    void *ptr = INTSET_UNTAG(is);
    size_t count = intsetCount(is);
    size_t bytes = intsetBytes((intset *)is);

    const char *tierName = "UNKNOWN";
    switch (type) {
    case INTSET_TYPE_SMALL:
        tierName = "SMALL";
        break;
    case INTSET_TYPE_MEDIUM:
        tierName = "MEDIUM";
        break;
    case INTSET_TYPE_FULL:
        tierName = "FULL";
        break;
    }

    printf("Intset [tier=%s, count=%zu, bytes=%zu]\n", tierName, count, bytes);

    switch (type) {
    case INTSET_TYPE_SMALL: {
        intsetSmall *small = (intsetSmall *)ptr;
        printf("  int16: %u values\n", small->count16);
        break;
    }

    case INTSET_TYPE_MEDIUM: {
        intsetMedium *medium = (intsetMedium *)ptr;
        printf("  int16: %u values\n", medium->count16);
        printf("  int32: %u values\n", medium->count32);
        break;
    }

    case INTSET_TYPE_FULL: {
        intsetFull *full = (intsetFull *)ptr;
        printf("  int16: %" PRIu64 " values\n", full->count16);
        printf("  int32: %" PRIu64 " values\n", full->count32);
        printf("  int64: %" PRIu64 " values\n", full->count64);
        break;
    }
    }

    /* Print first few values */
    if (count > 0) {
        printf("  values: [");
        size_t toShow = count < 20 ? count : 20;
        for (size_t i = 0; i < toShow; i++) {
            int64_t val;
            if (intsetGet((intset *)is, i, &val)) {
                printf("%" PRId64, val);
                if (i < toShow - 1) {
                    printf(", ");
                }
            }
        }
        if (count > 20) {
            printf(", ... (%" PRIu64 " more)", (uint64_t)(count - 20));
        }
        printf("]\n");
    }
}
#endif
