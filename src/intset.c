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

intset *intsetCopy(const intset *is) {
    const void *ptr = INTSET_UNTAG(is);
    switch (INTSET_TYPE(is)) {
    case INTSET_TYPE_SMALL: {
        const intsetSmall *from = ptr;
        /* @own: caller; free=intsetFree */
        intsetSmall *to = zmemdup(from, sizeof(*from) + (size_t)from->count16 *
                                                            sizeof(int16_t));
        return INTSET_TAG(to, INTSET_TYPE_SMALL);
    }
    case INTSET_TYPE_MEDIUM: {
        const intsetMedium *from = ptr;
        /* @own: caller; free=intsetFree */
        intsetMedium *to = zmemdup(from, sizeof(*from));
        /* @own: container; free=intsetFree */
        to->values16 = zmemdup(from->values16, from->count16 * sizeof(int16_t));
        /* @own: container; free=intsetFree */
        to->values32 = zmemdup(from->values32, from->count32 * sizeof(int32_t));
        return INTSET_TAG(to, INTSET_TYPE_MEDIUM);
    }
    case INTSET_TYPE_FULL: {
        const intsetFull *from = ptr;
        /* @own: caller; free=intsetFree */
        intsetFull *to = zmemdup(from, sizeof(*from));
        /* @own: container; free=intsetFree */
        to->values16 = zmemdup(from->values16, from->count16 * sizeof(int16_t));
        /* @own: container; free=intsetFree */
        to->values32 = zmemdup(from->values32, from->count32 * sizeof(int32_t));
        /* @own: container; free=intsetFree */
        to->values64 = zmemdup(from->values64, from->count64 * sizeof(int64_t));
        return INTSET_TAG(to, INTSET_TYPE_FULL);
    }
    }

    return NULL;
}

void intsetReset(intset **is) {
    intsetFree(*is);
    /* @own: caller; free=intsetFree */
    *is = intsetNew();
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
        /* @own: caller; free=intsetFree */
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

/* How many of the sorted values are negative */
static uint64_t intsetNegatives32_(const int32_t *v, uint64_t n) {
    uint64_t lo = 0;
    uint64_t hi = n;
    while (lo < hi) {
        const uint64_t mid = lo + (hi - lo) / 2;
        if (v[mid] < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static uint64_t intsetNegatives64_(const int64_t *v, uint64_t n) {
    uint64_t lo = 0;
    uint64_t hi = n;
    while (lo < hi) {
        const uint64_t mid = lo + (hi - lo) / 2;
        if (v[mid] < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static void intsetRunAdd_(intsetIterator *it, const void *first, uint64_t count,
                          uint8_t width) {
    if (count) {
        it->run[it->runs] = first;
        it->left[it->runs] = count;
        it->width[it->runs] = width;
        it->runs++;
    }
}

/* Each value lives in the narrowest array it fits, so in ascending order
 * the values are runs of those arrays: the negative int64s, the negative
 * int32s, every int16, the non-negative int32s, the non-negative int64s */
void intsetIteratorInit(intsetIterator *it, const intset *is) {
    *it = (intsetIterator){0};
    if (!is) {
        return;
    }

    const void *ptr = INTSET_UNTAG(is);
    switch (INTSET_TYPE(is)) {
    case INTSET_TYPE_SMALL: {
        const intsetSmall *s = ptr;
        intsetRunAdd_(it, s->values16, s->count16, 2);
        break;
    }
    case INTSET_TYPE_MEDIUM: {
        const intsetMedium *m = ptr;
        const uint64_t neg32 = intsetNegatives32_(m->values32, m->count32);
        intsetRunAdd_(it, m->values32, neg32, 4);
        intsetRunAdd_(it, m->values16, m->count16, 2);
        intsetRunAdd_(it, m->values32 + neg32, m->count32 - neg32, 4);
        break;
    }
    case INTSET_TYPE_FULL: {
        const intsetFull *f = ptr;
        const uint64_t neg64 = intsetNegatives64_(f->values64, f->count64);
        const uint64_t neg32 = intsetNegatives32_(f->values32, f->count32);
        intsetRunAdd_(it, f->values64, neg64, 8);
        intsetRunAdd_(it, f->values32, neg32, 4);
        intsetRunAdd_(it, f->values16, f->count16, 2);
        intsetRunAdd_(it, f->values32 + neg32, f->count32 - neg32, 4);
        intsetRunAdd_(it, f->values64 + neg64, f->count64 - neg64, 8);
        break;
    }
    }
}

static int64_t intsetRunValue_(const void *run, uint8_t width, uint64_t i) {
    switch (width) {
    case 2:
        return ((const int16_t *)run)[i];
    case 4:
        return ((const int32_t *)run)[i];
    default:
        return ((const int64_t *)run)[i];
    }
}

bool intsetIteratorNext(intsetIterator *it, int64_t *value) {
    while (it->current < it->runs && it->left[it->current] == 0) {
        it->current++;
    }
    if (it->current == it->runs) {
        return false;
    }

    const uint8_t r = it->current;
    *value = intsetRunValue_(it->run[r], it->width[r], 0);
    it->run[r] = (const uint8_t *)it->run[r] + it->width[r];
    it->left[r]--;
    return true;
}

/* Get value at position: the run holding it, then an index (O(log n) for
 * the run boundaries, never a walk) */
bool intsetGet(intset *is, uint32_t pos, int64_t *value) {
    intsetIterator it;
    intsetIteratorInit(&it, is);
    uint64_t at = pos;
    for (uint8_t r = 0; r < it.runs; r++) {
        if (at < it.left[r]) {
            if (value) {
                *value = intsetRunValue_(it.run[r], it.width[r], at);
            }
            return true;
        }
        at -= it.left[r];
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

/* O(1) min/max: every per-width array is sorted, so the global
 * extremes are the min of the array heads and the max of the tails. */
bool intsetMin(const intset *is, int64_t *value) {
    if (!is || intsetCount(is) == 0) {
        return false;
    }

    const intsetType type = INTSET_TYPE(is);
    void *ptr = INTSET_UNTAG(is);
    int64_t best = INT64_MAX;

    switch (type) {
    case INTSET_TYPE_SMALL: {
        const intsetSmall *small = (const intsetSmall *)ptr;
        best = small->values16[0];
        break;
    }
    case INTSET_TYPE_MEDIUM: {
        const intsetMedium *medium = (const intsetMedium *)ptr;
        if (medium->count16 && medium->values16[0] < best) {
            best = medium->values16[0];
        }
        if (medium->count32 && medium->values32[0] < best) {
            best = medium->values32[0];
        }
        break;
    }
    case INTSET_TYPE_FULL: {
        const intsetFull *full = (const intsetFull *)ptr;
        if (full->count16 && full->values16[0] < best) {
            best = full->values16[0];
        }
        if (full->count32 && full->values32[0] < best) {
            best = full->values32[0];
        }
        if (full->count64 && full->values64[0] < best) {
            best = full->values64[0];
        }
        break;
    }
    }

    *value = best;
    return true;
}

bool intsetMax(const intset *is, int64_t *value) {
    if (!is || intsetCount(is) == 0) {
        return false;
    }

    const intsetType type = INTSET_TYPE(is);
    void *ptr = INTSET_UNTAG(is);
    int64_t best = INT64_MIN;

    switch (type) {
    case INTSET_TYPE_SMALL: {
        const intsetSmall *small = (const intsetSmall *)ptr;
        best = small->values16[small->count16 - 1];
        break;
    }
    case INTSET_TYPE_MEDIUM: {
        const intsetMedium *medium = (const intsetMedium *)ptr;
        if (medium->count16 && medium->values16[medium->count16 - 1] > best) {
            best = medium->values16[medium->count16 - 1];
        }
        if (medium->count32 && medium->values32[medium->count32 - 1] > best) {
            best = medium->values32[medium->count32 - 1];
        }
        break;
    }
    case INTSET_TYPE_FULL: {
        const intsetFull *full = (const intsetFull *)ptr;
        if (full->count16 && full->values16[full->count16 - 1] > best) {
            best = full->values16[full->count16 - 1];
        }
        if (full->count32 && full->values32[full->count32 - 1] > best) {
            best = full->values32[full->count32 - 1];
        }
        if (full->count64 && full->values64[full->count64 - 1] > best) {
            best = full->values64[full->count64 - 1];
        }
        break;
    }
    }

    *value = best;
    return true;
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

void intsetStats(intset *is, dkTierStats *out) {
    *out = (dkTierStats){0};
    if (!is) {
        return;
    }

    uint64_t n16 = 0, n32 = 0, n64 = 0;
    const void *ptr = INTSET_UNTAG(is);
    switch (INTSET_TYPE(is)) {
    case INTSET_TYPE_SMALL:
        n16 = ((const intsetSmall *)ptr)->count16;
        out->unitLimit = INTSET_SMALL_MAX_BYTES;
        break;
    case INTSET_TYPE_MEDIUM:
        n16 = ((const intsetMedium *)ptr)->count16;
        n32 = ((const intsetMedium *)ptr)->count32;
        out->unitLimit = INTSET_MEDIUM_MAX_BYTES;
        break;
    case INTSET_TYPE_FULL:
        n16 = ((const intsetFull *)ptr)->count16;
        n32 = ((const intsetFull *)ptr)->count32;
        n64 = ((const intsetFull *)ptr)->count64;
        break;
    }

    out->tier = (uint8_t)INTSET_TYPE(is);
    out->units = out->tier; /* one sorted array per value width */
    out->count = n16 + n32 + n64;
    out->dataBytes =
        n16 * sizeof(int16_t) + n32 * sizeof(int32_t) + n64 * sizeof(int64_t);
    out->totalBytes = intsetBytes(is);
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
