/* multidictContractTest.c - persistOpsMultidict in the persistence
 * contract */

#include "persistContract.h"

#ifdef DATAKIT_TEST

#include "../../multidict.h"
#include "../../str.h"

/* The key of insert number 'i', a function of (seed, i), so a delete or
 * replace can name an earlier key again */
static void pcMultidictKey(uint64_t seed, size_t i, databoxBig *key,
                           uint8_t *buf, size_t bufLen) {
    uint64_t rng = seed ^ ((uint64_t)i * UINT64_C(0x9E3779B97F4A7C15));
    persistContractValue(&rng, key, buf, bufLen);
}

static void *pcMultidictBuild(const persistContractBuild *b) {
    /* A dict made as restore and newEmpty make one: exact keys, its own
     * default class (persistOpsMultidict's free releases both) */
    /* @own: container; free=multidictDefaultClassFree */
    multidictClass *cls = multidictDefaultClassNew();
    /* @own: caller; free=multidictFree */
    multidict *d = multidictNew(&multidictTypeExactKey, cls, 0);

    /* Settings by seed, so the snapshot carries them: the defaults; other
     * load factors with LRU tracking, an LRU policy and a memory cap (far
     * above what a build holds, so nothing is evicted); byte-based
     * expansion */
    switch (b->seed % 3) {
    case 1:
        multidictSetLoadFactors(d, 400, 50);
        multidictEnableLRU(d, 5);
        multidictSetEvictPolicy(d, MULTIDICT_EVICT_LRU);
        multidictSetMaxMemory(d, UINT64_C(1) << 40);
        break;
    case 2:
        multidictSetLoadFactors(d, 1600, 100);
        multidictEnableByteBasedExpansion(d, 4096, 65536);
        break;
    default:
        break;
    }
    persistContractStart(b->log, d);

    uint64_t rng = b->seed;
    uint8_t keyBuf[64];
    uint8_t valBuf[64];
    for (size_t i = 0; i < b->size; i++) {
        databoxBig key;
        databoxBig val;
        const uint64_t roll = splitmix64(&rng) % 8;
        const size_t k =
            (roll < 3 && i > 0) ? (size_t)(splitmix64(&rng) % i) : i;
        pcMultidictKey(b->seed, k, &key, keyBuf, sizeof(keyBuf));
        if (roll == 0) {
            multidictDelete(d, (const databox *)&key);
            persistContractRecord(b->log, PERSIST_OP_DELETE, &key, 1);
            continue;
        }

        persistContractValue(&rng, &val, valBuf, sizeof(valBuf));
        const databox *pair[] = {(const databox *)&key, (const databox *)&val};
        if (roll == 1) {
            /* Logged either way: replay meets the same absent key */
            (void)multidictReplace(d, pair[0], pair[1]);
            persistContractRecord(b->log, PERSIST_OP_REPLACE, pair, 2);
        } else {
            multidictPut(d, pair[0], pair[1]);
            persistContractRecord(b->log, PERSIST_OP_INSERT, pair, 2);
        }

        if (b->size >= 100 && i == b->size / 3) {
            multidictEmpty(d);
            persistContractRecord(b->log, PERSIST_OP_CLEAR, nullptr, 0);
        }
    }

    /* Each string as a key and as a value */
    for (size_t i = 0; i < b->stringCount; i++) {
        const databox *pair[] = {&b->strings[i], &b->strings[i]};
        multidictPut(d, pair[0], pair[1]);
        persistContractRecord(b->log, PERSIST_OP_INSERT, pair, 2);
    }
    return d;
}

static bool pcMultidictHoldsBytes(const void *structure, const databox *bytes) {
    multidict *d = (multidict *)structure;
    multidictIterator iter;
    bool found = false;
    if (multidictIteratorInit(d, &iter)) {
        multidictEntry entry;
        while (!found && multidictIteratorNext(&iter, &entry)) {
            found = persistContractSameBytes(&entry.key, bytes) ||
                    persistContractSameBytes(&entry.val, bytes);
        }
        multidictIteratorRelease(&iter);
    }
    return found;
}

static bool pcMultidictSameSettings(const void *original,
                                    const void *restored) {
    multidict *a = (multidict *)original;
    multidict *b = (multidict *)restored;
    uint32_t aExpand, aShrink, bExpand, bShrink;
    multidictGetLoadFactors(a, &aExpand, &aShrink);
    multidictGetLoadFactors(b, &bExpand, &bShrink);
    uint64_t aTarget, aMax, bTarget, bMax;
    const bool aBytes = multidictGetByteBasedExpansion(a, &aTarget, &aMax);
    const bool bBytes = multidictGetByteBasedExpansion(b, &bTarget, &bMax);
    return aExpand == bExpand && aShrink == bShrink &&
           multidictGetMaxMemory(a) == multidictGetMaxMemory(b) &&
           multidictGetEvictPolicy(a) == multidictGetEvictPolicy(b) &&
           aBytes == bBytes &&
           (!aBytes || (aTarget == bTarget && aMax == bMax)) &&
           multidictLRULevels(a) == multidictLRULevels(b) &&
           multidictGetHashFunctionSeed(a) == multidictGetHashFunctionSeed(b) &&
           multidictGetType(a) == multidictGetType(b);
}

const persistContractType persistContractMultidict = {
    .ops = &persistOpsMultidict,
    .build = pcMultidictBuild,
    .holdsBytes = pcMultidictHoldsBytes,
    .sameSettings = pcMultidictSameSettings,
};

#endif /* DATAKIT_TEST */
