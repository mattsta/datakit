/* multitierContractTest.c - persistOpsMultitier in the persistence contract */

#include "persistContract.h"

#ifdef DATAKIT_TEST

#include "../../cleaner.h"
#include "../../multitier.h"
#include "../../str.h"

defcleanerName(uint8_t *, zfree, PcTierBytes);
defcleanerName(void *, zfree, Void);

/* Key number 'k' as "key:NNNNNN" into 'buf' */
static databox pcMultitierKey(char *buf, size_t len, uint64_t k) {
    const int n = snprintf(buf, len, "key:%06" PRIu64, k);
    return databoxNewBytes(buf, (size_t)n);
}

/* The rows a sink takes leave the multitier: recorded as the keys gone */
static void pcMultitierSink(void *ctx, const flex *rows) {
    persistContractLog *log = ctx;
    const size_t n = flexCount(rows) / 2;
    cleanerName(zfree, Void) void *storage =
        zcalloc(n, sizeof(databoxBig) + sizeof(databox *));
    databoxBig *keys = storage;
    const databox **args = (const databox **)(keys + n);
    flexEntry *fe = flexHead(rows);
    for (size_t r = 0; r < n; r++) {
        flexGetByTypeBig(fe, &keys[r]);
        args[r] = (const databox *)&keys[r];
        fe = flexNext(rows, fe);
        fe = flexNext(rows, fe);
    }
    persistContractRecord(log, PERSIST_OP_BULK_DELETE, args, n);
}

static void *pcMultitierBuild(const persistContractBuild *b) {
    /* Odd seeds: every level small, so rows demote, rise and sink */
    const multitierConfig small = {
        .hotMaxBytes = 2048,
        .coldMaxBytes = 4096,
        .demoteBatch = 8,
        .cold = {.rawLimit = FLEX_CAP_LEVEL_512, .storedTarget = 128},
        .sink = pcMultitierSink,
        .sinkCtx = b->log,
    };
    /* @own: caller; free=multitierFree */
    multitier *t = multitierNew(b->seed % 2 ? &small : nullptr);
    persistContractStart(b->log, t);

    uint64_t rng = b->seed;
    uint8_t valueBuf[64];
    char key[32];
    const uint64_t space = b->size + 8;
    for (size_t i = 0; i < b->size; i++) {
        const uint64_t roll = splitmix64(&rng) % 10;
        const databox k =
            pcMultitierKey(key, sizeof(key), splitmix64(&rng) % space);
        if (roll < 2) {
            /* Only a change made is logged */
            const databox *args[1] = {&k};
            if (multitierDelete(t, &k)) {
                persistContractRecord(b->log, PERSIST_OP_DELETE, args, 1);
            }
        } else if (roll < 5) {
            /* A read moves rows (logged only when one sinks) */
            databox got = {};
            (void)multitierGet(t, &k, &got);
        } else {
            databoxBig value;
            persistContractValue(&rng, &value, valueBuf, sizeof(valueBuf));
            const databox *row[2] = {&k, (const databox *)&value};
            multitierSet(t, &k, (const databox *)&value);
            persistContractRecord(b->log, PERSIST_OP_INSERT, row, 2);
        }
    }

    /* Each string as a row's value, under keys of its own */
    for (size_t i = 0; i < b->stringCount; i++) {
        const databox k = pcMultitierKey(key, sizeof(key), space + i);
        const databox *row[2] = {&k, &b->strings[i]};
        multitierSet(t, &k, &b->strings[i]);
        persistContractRecord(b->log, PERSIST_OP_INSERT, row, 2);
    }

    /* The contract's own snapshots and frees follow: no sink after this */
    multitierSetSink(t, nullptr, nullptr);
    return t;
}

/* Values are read from a snapshot: every live row, whatever its level */
static bool pcMultitierHoldsBytes(const void *structure, const databox *bytes) {
    size_t len = 0;
    cleanerName(zfree, PcTierBytes) uint8_t *snap =
        persistOpsMultitier.snapshot(structure, &len);
    const flex *f = (const flex *)snap;
    flexEntry *fe = flexHead(f);
    for (size_t i = 0; i < 8 && fe; i++) {
        fe = flexNext(f, fe); /* past the config */
    }
    while (fe) {
        fe = flexNext(f, fe); /* past the key */
        databox v = {};
        flexGetByType(fe, &v);
        if (persistContractSameBytes(&v, bytes)) {
            return true;
        }
        fe = flexNext(f, fe);
    }
    return false;
}

static bool pcMultitierSameSettings(const void *original,
                                    const void *restored) {
    multitierConfig a;
    multitierConfig b;
    multitierGetConfig(original, &a);
    multitierGetConfig(restored, &b);
    return a.hotMaxBytes == b.hotMaxBytes && a.hotLevels == b.hotLevels &&
           a.demoteBatch == b.demoteBatch && a.coldMaxBytes == b.coldMaxBytes &&
           a.cold.rawLimit == b.cold.rawLimit &&
           a.cold.storedTarget == b.cold.storedTarget &&
           a.cold.minSavingPercent == b.cold.minSavingPercent &&
           a.cold.level == b.cold.level;
}

const persistContractType persistContractMultitier = {
    .ops = &persistOpsMultitier,
    .build = pcMultitierBuild,
    .holdsBytes = pcMultitierHoldsBytes,
    .sameSettings = pcMultitierSameSettings,
};

#endif /* DATAKIT_TEST */
