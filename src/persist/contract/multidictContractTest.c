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
            multidictReplace(d, pair[0], pair[1]);
            persistContractRecord(b->log, PERSIST_OP_REPLACE, pair, 2);
        } else {
            multidictAdd(d, pair[0], pair[1]);
            persistContractRecord(b->log, PERSIST_OP_INSERT, pair, 2);
        }

        if (b->size >= 100 && i == b->size / 3) {
            multidictEmpty(d);
            persistContractRecord(b->log, PERSIST_OP_CLEAR, NULL, 0);
        }
    }

    /* Each string as a key and as a value */
    for (size_t i = 0; i < b->stringCount; i++) {
        const databox *pair[] = {&b->strings[i], &b->strings[i]};
        multidictAdd(d, pair[0], pair[1]);
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

const persistContractType persistContractMultidict = {
    .ops = &persistOpsMultidict,
    .build = pcMultidictBuild,
    .holdsBytes = pcMultidictHoldsBytes,
};

#endif /* DATAKIT_TEST */
