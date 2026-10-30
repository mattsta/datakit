/* coldmapContractTest.c - persistOpsColdmap in the persistence contract */

#include "persistContract.h"

#ifdef DATAKIT_TEST

#include "../../cleaner.h"
#include "../../coldmap.h"
#include "../../multimap.h"
#include "../../str.h"

defcleaner(multimap *, multimapFree);
defcleaner(flex *, flexFree);
defcleaner(mflexState *, mflexStateFree);

/* Key number 'k' as "key:NNNNNN" into 'buf' */
static databox pcColdmapKey(char *buf, size_t len, uint64_t k) {
    const int n = snprintf(buf, len, "key:%06" PRIu64, k);
    return databoxNewBytes(buf, (size_t)n);
}

static void *pcColdmapBuild(const persistContractBuild *b) {
    /* Odd seeds: small blocks, so rows cross many of them */
    const coldmapConfig small = {.rawLimit = FLEX_CAP_LEVEL_512,
                                 .storedTarget = 128};
    /* @own: caller; free=coldmapFree */
    coldmap *cm = coldmapNew(2, b->seed % 2 ? &small : nullptr);
    persistContractStart(b->log, cm);

    uint64_t rng = b->seed;
    uint8_t valueBuf[64];
    char key[32];
    char hiKey[32];
    const uint64_t space = b->size + 8;
    for (size_t i = 0; i < b->size; i++) {
        const uint64_t roll = splitmix64(&rng) % 20;
        const databox k =
            pcColdmapKey(key, sizeof(key), splitmix64(&rng) % space);
        if (roll < 3) {
            /* Only a change made is logged: replay meets the same rows */
            const databox *args[1] = {&k};
            if (coldmapDelete(cm, &k)) {
                persistContractRecord(b->log, PERSIST_OP_DELETE, args, 1);
            }
        } else if (roll == 3) {
            /* A range, sometimes open at one end */
            const databox hi =
                pcColdmapKey(hiKey, sizeof(hiKey), splitmix64(&rng) % space);
            const uint64_t ends = splitmix64(&rng) % 4;
            const databox *bounds[2] = {ends & 1 ? &k : nullptr,
                                        ends & 2 ? &hi : nullptr};
            coldmapDeleteRange(cm, bounds[0], bounds[1]);
            persistContractRecord(b->log, PERSIST_OP_BULK_DELETE, bounds, 2);
        } else if (roll == 4) {
            /* A sorted batch of up to 16 rows */
            cleaner(multimapFree) multimap *batch = multimapNew(2);
            const uint64_t rows = 1 + splitmix64(&rng) % 16;
            for (uint64_t r = 0; r < rows; r++) {
                char batchKey[32];
                const databox bk = pcColdmapKey(batchKey, sizeof(batchKey),
                                                splitmix64(&rng) % space);
                databoxBig value;
                persistContractValue(&rng, &value, valueBuf, sizeof(valueBuf));
                const databox *row[2] = {&bk, (const databox *)&value};
                multimapInsert(&batch, row);
            }
            cleaner(flexFree) flex *dump = multimapDump(batch);
            if (coldmapInsertMany(cm, dump, nullptr)) {
                persistContractRecord(b->log, PERSIST_OP_BULK_INSERT, dump, 1);
            }
        } else {
            databoxBig value;
            persistContractValue(&rng, &value, valueBuf, sizeof(valueBuf));
            const databox *row[2] = {&k, (const databox *)&value};
            coldmapInsert(cm, row);
            persistContractRecord(b->log, PERSIST_OP_INSERT, row, 2);
        }
    }

    /* Each string as a row's value, under keys of its own */
    for (size_t i = 0; i < b->stringCount; i++) {
        const databox k = pcColdmapKey(key, sizeof(key), space + i);
        const databox *row[2] = {&k, &b->strings[i]};
        coldmapInsert(cm, row);
        persistContractRecord(b->log, PERSIST_OP_INSERT, row, 2);
    }

    return cm;
}

static bool pcColdmapHoldsBytes(const void *structure, const databox *bytes) {
    cleaner(mflexStateFree) mflexState *state = mflexStateCreate();
    coldmapIterator it;
    databox k = {};
    databox v = {};
    databox *row[2] = {&k, &v};
    coldmapIteratorInit(structure, state, &it, true);
    while (coldmapIteratorNext(&it, row)) {
        if (persistContractSameBytes(&v, bytes)) {
            return true;
        }
    }
    return false;
}

static bool pcColdmapSameSettings(const void *original, const void *restored) {
    coldmapConfig a;
    coldmapConfig b;
    coldmapGetConfig(original, &a);
    coldmapGetConfig(restored, &b);
    return coldmapElementsPerEntry(original) ==
               coldmapElementsPerEntry(restored) &&
           a.rawLimit == b.rawLimit && a.storedTarget == b.storedTarget &&
           a.minSavingPercent == b.minSavingPercent && a.level == b.level;
}

const persistContractType persistContractColdmap = {
    .ops = &persistOpsColdmap,
    .build = pcColdmapBuild,
    .holdsBytes = pcColdmapHoldsBytes,
    .sameSettings = pcColdmapSameSettings,
};

#endif /* DATAKIT_TEST */
