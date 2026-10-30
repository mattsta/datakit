#include "multitier.h"

#include "cleaner.h"
#include "multidict.h"
#include "multimap.h"

defcleaner(flex *, flexFree);

struct multitier {
    multidict *hot;
    multidictClass *hotClass;
    multimap *pending; /* [key, value] rows the hot level gave up, sorted */
    coldmap *cold;
    mflexState *state; /* the cold level's reads */
    multitierConfig config;
    uint64_t rng;        /* which block leaves the cold level */
    const databox *keep; /* the key the running Set or Get just placed */
    uint64_t hotHits;
    uint64_t coldHits;
    uint64_t misses;
    uint64_t demotions;
    uint64_t sunkRows;
    uint64_t sunkBlocks;
};

/* ====================================================================
 * Moving rows down
 * ==================================================================== */
/* The hot level gives up a row: it waits in the sorted batch.  The key
 * the running call just placed stays (a veto: the next victim goes). */
static bool multitierOnEvict_(void *privdata, const databox *key,
                              const databox *val) {
    multitier *t = privdata;
    if (t->keep && databoxCompare(key, t->keep) == 0) {
        return false;
    }

    const databox *row[2] = {key, val};
    multimapInsert(&t->pending, row);
    t->demotions++;
    return true;
}

/* While the cold level is over its limit, a block at random leaves whole,
 * its rows to the sink */
static void multitierColdPressure_(multitier *t) {
    if (!t->config.coldMaxBytes) {
        return;
    }

    coldmapStats cs;
    coldmapGetStats(t->cold, &cs);
    while (cs.storedBytes > t->config.coldMaxBytes && cs.blocks) {
        t->rng ^= t->rng << 13;
        t->rng ^= t->rng >> 7;
        t->rng ^= t->rng << 17;
        cleaner(flexFree) flex *rows =
            coldmapRemoveBlock(t->cold, t->rng % cs.blocks);
        if (t->config.sink) {
            t->config.sink(t->config.sinkCtx, rows);
        }
        t->sunkRows += flexCount(rows) / 2;
        t->sunkBlocks++;
        coldmapGetStats(t->cold, &cs);
    }
}

void multitierFlush(multitier *t) {
    if (!multimapCount(t->pending)) {
        return;
    }

    cleaner(flexFree) flex *rows = multimapDump(t->pending);
    coldmapInsertMany(t->cold, rows, nullptr);
    multimapReset(&t->pending);
    multitierColdPressure_(t);
}

/* After the hot level grew: rows over its limit move down, and a full
 * batch goes into the cold level */
static void multitierHotPressure_(multitier *t, const databox *keep) {
    if (!t->config.hotMaxBytes) {
        return;
    }

    t->keep = keep;
    multidictEvictToLimit(t->hot);
    t->keep = nullptr;
    if (multimapCount(t->pending) >= t->config.demoteBatch) {
        multitierFlush(t);
    }
}

/* ====================================================================
 * Lifecycle
 * ==================================================================== */
multitier *multitierNew(const multitierConfig *config) {
    const multitierConfig defaults = {};
    const multitierConfig *c = config ? config : &defaults;
    /* @own: container; free=coldmapFree */
    coldmap *cold = coldmapNew(2, &c->cold);
    if (!cold) {
        return nullptr; /* a cold config out of range */
    }

    /* @own: caller; free=multitierFree */
    multitier *t = zcalloc(1, sizeof(*t));
    t->config = *c;
    t->config.hotLevels = c->hotLevels ? c->hotLevels : 4;
    t->config.demoteBatch = c->demoteBatch ? c->demoteBatch : 64;
    t->cold = cold;
    t->rng = 0x9E3779B97F4A7C15ULL;
    /* @own: container; free=multidictDefaultClassFree */
    t->hotClass = multidictDefaultClassNew();
    /* @own: container; free=multidictFree */
    t->hot = multidictNew(&multidictTypeExactKey, t->hotClass,
                          multidictRandomSeed());
    multidictEnableLRU(t->hot, t->config.hotLevels);
    multidictSetEvictPolicy(t->hot, MULTIDICT_EVICT_LRU);
    multidictSetEvictionCallback(t->hot, multitierOnEvict_, t);
    multidictSetMaxMemory(t->hot, c->hotMaxBytes);
    /* @own: container; free=multimapFree */
    t->pending = multimapNew(2);
    /* @own: container; free=mflexStateFree */
    t->state = mflexStateCreate();
    return t;
}

void multitierFree(multitier *t) {
    if (!t) {
        return;
    }

    multidictFree(t->hot);
    multidictDefaultClassFree(t->hotClass);
    multimapFree(t->pending);
    coldmapFree(t->cold);
    mflexStateFree(t->state);
    zfree(t);
}

/* ====================================================================
 * Reads and writes
 * ==================================================================== */
bool multitierSet(multitier *t, const databox *key, const databox *value) {
    /* A key new to the hot level may sit lower: that copy goes */
    bool isNew = multidictPut(t->hot, key, value);
    if (isNew) {
        if (multimapDelete(&t->pending, key)) {
            isNew = false;
        } else if (coldmapCount(t->cold) && coldmapDelete(t->cold, key)) {
            isNew = false;
        }
    }

    multitierHotPressure_(t, key);
    return isNew;
}

/* 'key's value below the hot level (borrowed from the batch or the cold
 * level's read state) */
static bool multitierFindBelow_(multitier *t, const databox *key,
                                databox *value, bool *fromBatch) {
    databox *values[1] = {value};
    *fromBatch = multimapLookup(t->pending, key, values);
    return *fromBatch || coldmapLookup(t->cold, t->state, key, values);
}

bool multitierGet(multitier *t, const databox *key, databox *value) {
    if (multidictFind(t->hot, key, value)) {
        t->hotHits++;
        return true;
    }

    databox below = {};
    bool fromBatch;
    if (!multitierFindBelow_(t, key, &below, &fromBatch)) {
        t->misses++;
        return false;
    }

    /* Up to the hot level (it copies the value), then out of the level
     * below */
    multidictPut(t->hot, key, &below);
    if (fromBatch) {
        multimapDelete(&t->pending, key);
    } else {
        coldmapDelete(t->cold, key);
    }
    t->coldHits++;
    multitierHotPressure_(t, key);

    /* The pressure kept it: the value, borrowed from the hot level */
    return multidictFind(t->hot, key, value);
}

bool multitierPeek(multitier *t, const databox *key, databox *value) {
    bool fromBatch;
    return multidictFindNoTouch(t->hot, key, value) ||
           multitierFindBelow_(t, key, value, &fromBatch);
}

bool multitierDelete(multitier *t, const databox *key) {
    return multidictDelete(t->hot, key) || multimapDelete(&t->pending, key) ||
           coldmapDelete(t->cold, key);
}

size_t multitierCount(const multitier *t) {
    return multidictCount(t->hot) + multimapCount(t->pending) +
           coldmapCount(t->cold);
}

size_t multitierBytes(const multitier *t) {
    return sizeof(*t) + multidictBytes(t->hot) + multimapBytes(t->pending) +
           coldmapBytes(t->cold);
}

void multitierGetStats(const multitier *t, multitierStats *stats) {
    coldmapStats cs;
    coldmapGetStats(t->cold, &cs);
    *stats = (multitierStats){
        .hotRows = multidictCount(t->hot),
        .hotBytes = multidictKeyBytes(t->hot) + multidictValBytes(t->hot),
        .pendingRows = multimapCount(t->pending),
        .coldRows = cs.rows,
        .coldStoredBytes = cs.storedBytes,
        .coldRawBytes = cs.rawBytes,
        .coldBlocks = cs.blocks,
        .hotHits = t->hotHits,
        .coldHits = t->coldHits,
        .misses = t->misses,
        .demotions = t->demotions,
        .sunkRows = t->sunkRows,
        .sunkBlocks = t->sunkBlocks,
    };
}

/* ====================================================================
 * Tests
 * ==================================================================== */
#ifdef DATAKIT_TEST
#include "ctest.h"

defcleaner(multitier *, multitierFree);
defcleaner(multimap *, multimapFree);

/* The model a test checks against: every live row; the sink takes the
 * rows the multitier gives up out of it */
static void multitierTestSink_(void *ctx, const flex *rows) {
    multimap **model = ctx;
    flexEntry *fe = flexHead(rows);
    for (size_t r = 0; r < flexCount(rows) / 2; r++) {
        databox key = {};
        flexGetByType(fe, &key);
        multimapDelete(model, &key);
        fe = flexNext(rows, fe);
        fe = flexNext(rows, fe);
    }
}

static databox multitierTestKey_(char *buf, size_t len, uint64_t k) {
    const int n = snprintf(buf, len, "key:%06" PRIu64, k);
    return databoxNewBytes(buf, (size_t)n);
}

int multitierTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    TEST("every level, every move: Get, Peek, Set and Delete agree with a "
         "model of the live rows") {
        static const struct {
            uint64_t hot;
            uint64_t cold;
        } limits[] = {{0, 0}, {4096, 0}, {4096, 16384}, {512, 4096}};
        for (size_t l = 0; l < countof(limits); l++) {
            cleaner(multimapFree) multimap *model = multimapNew(2);
            const multitierConfig config = {
                .hotMaxBytes = limits[l].hot,
                .coldMaxBytes = limits[l].cold,
                .demoteBatch = 16,
                .cold = {.rawLimit = FLEX_CAP_LEVEL_512, .storedTarget = 128},
                .sink = multitierTestSink_,
                .sinkCtx = &model,
            };
            cleaner(multitierFree) multitier *t = multitierNew(&config);
            uint64_t rng = 0x5EED + l;
            char key[32];
            char value[64];
            for (size_t step = 0; step < 8000; step++) {
                rng ^= rng << 13;
                rng ^= rng >> 7;
                rng ^= rng << 17;
                const databox k =
                    multitierTestKey_(key, sizeof(key), (rng >> 8) % 1500);
                const uint32_t op = (uint32_t)(rng % 10);
                /* The expected value, owned: the sink edits the model
                 * during the call */
                databox found = {};
                databox *wantRow[1] = {&found};
                const bool had = multimapLookup(model, &k, wantRow);
                databox want = had ? databoxCopy(&found) : (databox){};
                databox got = {};
                if (op < 4) {
                    const int vn = snprintf(
                        value, sizeof(value), "{\"step\":%zu,\"pad\":\"%.*s\"}",
                        step, (int)(step % 24), "xxxxxxxxxxxxxxxxxxxxxxxx");
                    const databox v = databoxNewBytes(value, (size_t)vn);
                    const databox *row[2] = {&k, &v};
                    multimapInsert(&model, row);
                    if (multitierSet(t, &k, &v) != !had) {
                        ERR("limits %zu step %zu: Set new", l, step);
                    }
                } else if (op < 7) {
                    const bool has = multitierGet(t, &k, &got);
                    if (has != had || (has && databoxCompare(&got, &want))) {
                        ERR("limits %zu step %zu: Get", l, step);
                    }
                } else if (op < 9) {
                    const bool has = multitierPeek(t, &k, &got);
                    if (has != had || (has && databoxCompare(&got, &want))) {
                        ERR("limits %zu step %zu: Peek", l, step);
                    }
                } else {
                    multimapDelete(&model, &k);
                    if (multitierDelete(t, &k) != had) {
                        ERR("limits %zu step %zu: Delete", l, step);
                    }
                }
                databoxFreeData(&want);

                multitierStats st;
                multitierGetStats(t, &st);
                if (multitierCount(t) != multimapCount(model) ||
                    (limits[l].hot && st.hotBytes > limits[l].hot)) {
                    ERR("limits %zu step %zu: %zu rows, model %zu; hot %" PRIu64
                        " bytes",
                        l, step, multitierCount(t), multimapCount(model),
                        st.hotBytes);
                    break;
                }
            }

            /* Rows moved down at every limit, and sank when cold has one */
            multitierStats st;
            multitierGetStats(t, &st);
            if ((limits[l].hot && (!st.demotions || !st.coldHits)) ||
                (limits[l].cold && !st.sunkRows) ||
                (!limits[l].hot && st.demotions)) {
                ERR("limits %zu: %" PRIu64 " demoted, %" PRIu64
                    " promoted, %" PRIu64 " sunk",
                    l, st.demotions, st.coldHits, st.sunkRows);
            }
        }
    }

    TEST("Peek reads a lower level where it is; Get moves it up") {
        const multitierConfig config = {.hotMaxBytes = 600, .demoteBatch = 4};
        cleaner(multitierFree) multitier *t = multitierNew(&config);
        char key[32];
        const databox v =
            databoxNewBytes("a value of some thirty bytes...", 31);
        for (uint64_t i = 0; i < 40; i++) {
            const databox k = multitierTestKey_(key, sizeof(key), i);
            multitierSet(t, &k, &v);
        }
        multitierFlush(t);

        /* Key 0 went down first */
        const databox k0 = multitierTestKey_(key, sizeof(key), 0);
        multitierStats before;
        multitierGetStats(t, &before);
        databox got = {};
        const bool peeked = multitierPeek(t, &k0, &got);
        multitierStats afterPeek;
        multitierGetStats(t, &afterPeek);
        const bool fetched = multitierGet(t, &k0, &got);
        multitierStats afterGet;
        multitierGetStats(t, &afterGet);
        if (!before.coldRows || !peeked ||
            afterPeek.coldRows != before.coldRows ||
            afterPeek.coldHits != before.coldHits || !fetched ||
            afterGet.coldHits != before.coldHits + 1) {
            ERR("cold %" PRIu64 " -> %" PRIu64 " -> %" PRIu64
                ", promotions %" PRIu64 " -> %" PRIu64,
                before.coldRows, afterPeek.coldRows, afterGet.coldRows,
                before.coldHits, afterGet.coldHits);
        }
    }

    TEST_FINAL_RESULT;
}
#endif
