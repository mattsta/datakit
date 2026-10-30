#include "multitier.h"

#include "cleaner.h"
#include "multidict.h"
#include "multimap.h"
#include "persist.h"

defcleaner(flex *, flexFree);
defcleaner(mflexState *, mflexStateFree);
defcleaner(multimap *, multimapFree);
defcleaner(multitier *, multitierFree);

/* Rows a demotion batch holds per cold block, at least */
static constexpr size_t MULTITIER_ROWS_PER_COLD_BLOCK = 4;

struct multitier {
    multidict *hot;
    multidictClass *hotClass;
    multimap *pending; /* [key, value] rows the hot level gave up, sorted */
    /* Keys whose cold row a copy above supersedes (in the hot level or
     * the batch), sorted: those rows leave the cold level together */
    multimap *shadowed;
    coldmap *cold;
    mflexState *state; /* the cold level's reads */
    multitierConfig config;
    uint64_t rng;        /* which block leaves the cold level */
    const databox *keep; /* the key the running Set or Get just placed */
    bool replaying; /* applying a WAL: rows reach the sink only as logged */
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
    if (!t->config.coldMaxBytes || t->replaying) {
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

        /* A shadowed row is superseded by its copy above: it goes, and
         * only the live rows reach the sink */
        cleaner(flexFree) flex *live = flexNew();
        flexEntry *fe = flexHead(rows);
        for (size_t r = 0; r < flexCount(rows) / 2; r++) {
            databoxBig row[2] = {};
            flexGetByTypeBig(fe, &row[0]);
            fe = flexNext(rows, fe);
            flexGetByTypeBig(fe, &row[1]);
            fe = flexNext(rows, fe);
            if (!multimapDelete(&t->shadowed, (const databox *)&row[0])) {
                const databox *both[2] = {(const databox *)&row[0],
                                          (const databox *)&row[1]};
                flexAppendMultipleExact(&live, 2, both);
            }
        }

        if (t->config.sink && flexCount(live)) {
            t->config.sink(t->config.sinkCtx, live);
        }
        t->sunkRows += flexCount(live) / 2;
        t->sunkBlocks++;
        coldmapGetStats(t->cold, &cs);
    }
}

/* The shadowed rows leave the cold level together once there are several
 * per block: one recompression per block they share */
static void multitierShadowPressure_(multitier *t, bool all) {
    const size_t perBlock =
        MULTITIER_ROWS_PER_COLD_BLOCK * coldmapBlockCount(t->cold);
    const size_t count = multimapCount(t->shadowed);
    if (!count || (!all && count < (perBlock > t->config.demoteBatch
                                        ? perBlock
                                        : t->config.demoteBatch))) {
        return;
    }

    cleaner(flexFree) flex *keys = multimapDump(t->shadowed);
    coldmapDeleteMany(t->cold, keys, nullptr);
    multimapReset(&t->shadowed);
}

void multitierFlush(multitier *t) {
    if (multimapCount(t->pending)) {
        /* Each row replaces its key's cold row: a shadow mark on it ends */
        cleaner(flexFree) flex *rows = multimapDump(t->pending);
        if (multimapCount(t->shadowed)) {
            flexEntry *fe = flexHead(rows);
            for (size_t r = 0; r < flexCount(rows) / 2; r++) {
                databoxBig key = {};
                flexGetByTypeBig(fe, &key);
                multimapDelete(&t->shadowed, (const databox *)&key);
                fe = flexNext(rows, fe);
                fe = flexNext(rows, fe);
            }
        }
        coldmapInsertMany(t->cold, rows, nullptr);
        multimapReset(&t->pending);
    }

    multitierShadowPressure_(t, true);
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
    /* Rows land in random blocks: a batch of several per cold block
     * shares each block's recompression among them */
    const size_t perBlock =
        MULTITIER_ROWS_PER_COLD_BLOCK * coldmapBlockCount(t->cold);
    if (multimapCount(t->pending) >=
        (perBlock > t->config.demoteBatch ? perBlock : t->config.demoteBatch)) {
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
    if (c->hotLevels > 64) {
        return nullptr; /* multilru holds at most 64 levels */
    }
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
    coldmapGetConfig(cold, &t->config.cold); /* defaults resolved */
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
    /* @own: container; free=multimapFree */
    t->shadowed = multimapSetNew(1);
    /* @own: container; free=mflexStateFree */
    t->state = mflexStateNew(
        flexOptimizationSizeLimit[t->config.cold.rawLimit]); /* one block */
    return t;
}

void multitierFree(multitier *t) {
    if (!t) {
        return;
    }

    multidictFree(t->hot);
    multidictDefaultClassFree(t->hotClass);
    multimapFree(t->pending);
    multimapFree(t->shadowed);
    coldmapFree(t->cold);
    mflexStateFree(t->state);
    zfree(t);
}

/* ====================================================================
 * Reads and writes
 * ==================================================================== */
/* 'key's live cold row is superseded: it leaves with the next batch */
static void multitierShadow_(multitier *t, const databox *key) {
    const databox *row[1] = {key};
    multimapInsert(&t->shadowed, row);
    multitierShadowPressure_(t, false);
}

bool multitierSet(multitier *t, const databox *key, const databox *value) {
    /* A key new to the hot level may sit lower: that copy goes (a cold
     * row by a shadow mark, not a rewrite of its block) */
    bool isNew = multidictPut(t->hot, key, value);
    if (isNew) {
        if (multimapDelete(&t->pending, key)) {
            isNew = false;
        } else if (coldmapCount(t->cold) &&
                   coldmapExists(t->cold, t->state, key)) {
            isNew = false;
            multitierShadow_(t, key);
        }
    }

    multitierHotPressure_(t, key);
    return isNew;
}

/* 'key's value below the hot level (borrowed from the batch or the cold
 * level's read state); a shadowed cold row is not live */
static bool multitierFindBelow_(multitier *t, const databox *key,
                                databox *value, bool *fromBatch) {
    databox *values[1] = {value};
    *fromBatch = multimapLookup(t->pending, key, values);
    return *fromBatch || (!multimapExists(t->shadowed, key) &&
                          coldmapLookup(t->cold, t->state, key, values));
}

bool multitierGet(multitier *t, const databox *key, databox *value) {
    if (multidictFind(t->hot, key, value)) {
        t->hotHits++;
        return true;
    }

    /* Wide-capable: the hot level stores a 96/128-bit value whole */
    databoxBig below;
    DATABOX_BIG_INIT(&below);
    bool fromBatch;
    if (!multitierFindBelow_(t, key, (databox *)&below, &fromBatch)) {
        t->misses++;
        return false;
    }

    /* Up to the hot level (it copies the value), then out of the level
     * below */
    multidictPut(t->hot, key, (const databox *)&below);
    if (fromBatch) {
        multimapDelete(&t->pending, key);
    } else {
        multitierShadow_(t, key);
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
    if (multidictDelete(t->hot, key) || multimapDelete(&t->pending, key)) {
        /* A cold row it superseded goes with it */
        if (multimapDelete(&t->shadowed, key)) {
            coldmapDelete(t->cold, key);
        }
        return true;
    }

    return coldmapDelete(t->cold, key);
}

size_t multitierCount(const multitier *t) {
    return multidictCount(t->hot) + multimapCount(t->pending) +
           coldmapCount(t->cold) - multimapCount(t->shadowed);
}

size_t multitierBytes(const multitier *t) {
    return sizeof(*t) + multidictBytes(t->hot) + multimapBytes(t->pending) +
           multimapBytes(t->shadowed) + coldmapBytes(t->cold) +
           mflexStateBytes(t->state);
}

void multitierGetStats(const multitier *t, multitierStats *stats) {
    coldmapStats cs;
    coldmapGetStats(t->cold, &cs);
    *stats = (multitierStats){
        .hotRows = multidictCount(t->hot),
        .hotBytes = multidictKeyBytes(t->hot) + multidictValBytes(t->hot),
        .pendingRows = multimapCount(t->pending),
        .coldRows = cs.rows - multimapCount(t->shadowed),
        .coldShadowedRows = multimapCount(t->shadowed),
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

void multitierSetSink(multitier *t, multitierSinkFn *sink, void *sinkCtx) {
    t->config.sink = sink;
    t->config.sinkCtx = sinkCtx;
}

void multitierGetConfig(const multitier *t, multitierConfig *config) {
    *config = t->config;
}

/* ====================================================================
 * Persistence (persistOpsMultitier; the format is described in persist.h)
 * ==================================================================== */
/* Rows appended in key order, kept in bounded chunks and joined once (a
 * flex grows by exact reallocation, so one long append would copy the
 * rows over and over) */
typedef struct multitierRows_ {
    flex **chunk;
    size_t count;
    size_t cap;
} multitierRows_;

static constexpr size_t MULTITIER_ROWS_CHUNK_BYTES = 64 * 1024;

static void multitierRowsAppend_(multitierRows_ *r, const databox *key,
                                 const databox *val) {
    if (!r->count ||
        flexBytes(r->chunk[r->count - 1]) >= MULTITIER_ROWS_CHUNK_BYTES) {
        if (r->count == r->cap) {
            r->cap = r->cap ? r->cap * 2 : 8;
            /* @own: container; free=multitierRowsJoin_ */
            r->chunk = zrealloc(r->chunk, r->cap * sizeof(*r->chunk));
        }
        /* @own: container; free=multitierRowsJoin_ */
        r->chunk[r->count++] = flexNew();
    }
    const databox *row[2] = {key, val};
    flexAppendMultipleExact(&r->chunk[r->count - 1], 2, row);
}

/* A [key, value] row of 'rows' at 'fe' decoded into 'kv' (bytes stay in
 * 'rows'); the entry after it */
static flexEntry *multitierDecodeRow_(const flex *rows, flexEntry *fe,
                                      databoxBig kv[2]) {
    flexGetByTypeBig(fe, &kv[0]);
    fe = flexNext(rows, fe);
    flexGetByTypeBig(fe, &kv[1]);
    return flexNext(rows, fe);
}

/* Every chunk joined, in order, into 'into' (after what it holds) */
static void multitierRowsJoin_(multitierRows_ *r, flex **into) {
    if (r->count) {
        cleaner(flexFree) flex *rows =
            flexBulkMergeFlex((const flex *const *)r->chunk, r->count);
        flexBulkAppendFlex(into, rows);
    }
    for (size_t i = 0; i < r->count; i++) {
        flexFree(r->chunk[i]);
    }
    zfree(r->chunk);
    *r = (multitierRows_){};
}

/* Elements before the rows: the hot limit, LRU levels, demotion batch,
 * cold limit, then the cold level's raw limit, stored target, required
 * saving and LZ4 level */
static constexpr uint32_t MULTITIER_PERSIST_CONFIG = 8;

/* The next row of snapshot stream 's' (0 the hot rows, 1 the batch, 2 the
 * live cold rows) into 'kv', aimed afresh before each decode so a
 * 96/128-bit element comes back whole */
static bool multitierStreamNext_(const multitier *t, int s,
                                 multimapIterator *hot, multimapIterator *batch,
                                 coldmapIterator *cold, databoxBig kv[2]) {
    databox *row[2] = {(databox *)&kv[0], (databox *)&kv[1]};
    for (;;) {
        DATABOX_BIG_INIT(&kv[0]);
        DATABOX_BIG_INIT(&kv[1]);
        const bool got = s == 0   ? multimapIteratorNext(hot, row)
                         : s == 1 ? multimapIteratorNext(batch, row)
                                  : coldmapIteratorNext(cold, row);
        if (!got) {
            return false;
        }
        if (s != 2 || !multimapExists(t->shadowed, row[0])) {
            return true;
        }
    }
}

/* The snapshot holds what the multitier holds, not where: every live
 * row, in key order (the hot level's rows sorted in, the batch and the
 * live cold rows merged with them) */
static uint8_t *multitierPersistSnapshot_(const void *structure, size_t *len) {
    const multitier *t = structure;
    const multitierConfig *c = &t->config;
    const databox fields[MULTITIER_PERSIST_CONFIG] = {
        DATABOX_UNSIGNED(c->hotMaxBytes),
        DATABOX_UNSIGNED(c->hotLevels),
        DATABOX_UNSIGNED(c->demoteBatch),
        DATABOX_UNSIGNED(c->coldMaxBytes),
        DATABOX_UNSIGNED(c->cold.rawLimit),
        DATABOX_UNSIGNED(c->cold.storedTarget),
        DATABOX_UNSIGNED(c->cold.minSavingPercent),
        DATABOX_UNSIGNED(c->cold.level),
    };
    const databox *header[MULTITIER_PERSIST_CONFIG];
    for (uint32_t i = 0; i < MULTITIER_PERSIST_CONFIG; i++) {
        header[i] = &fields[i];
    }
    /* @own: caller; free=zfree */
    flex *out = flexNew();
    flexAppendMultipleExact(&out, MULTITIER_PERSIST_CONFIG, header);

    /* The hot rows sorted: a multimap of them (the hot level is bounded,
     * and the copy is its rows' bytes) */
    cleaner(multimapFree) multimap *hot = multimapNew(2);
    multidictIterator di;
    if (multidictIteratorInit(t->hot, &di)) {
        multidictEntry e;
        while (multidictIteratorNext(&di, &e)) {
            databoxBig key;
            databoxBig val;
            databoxBigCopyFrom(&key, &e.key);
            databoxBigCopyFrom(&val, &e.val);
            const databox *row[2] = {(const databox *)&key,
                                     (const databox *)&val};
            multimapInsert(&hot, row);
        }
        multidictIteratorRelease(&di);
    }

    /* Three sorted streams, each key in one of them */
    multimapIterator hi;
    multimapIterator pi;
    coldmapIterator ci;
    multimapIteratorInit(hot, &hi, true);
    multimapIteratorInit(t->pending, &pi, true);
    cleaner(mflexStateFree) mflexState *state = mflexStateCreate();
    coldmapIteratorInit(t->cold, state, &ci, true);
    databoxBig rows[3][2];
    bool has[3];
    for (int s = 0; s < 3; s++) {
        has[s] = multitierStreamNext_(t, s, &hi, &pi, &ci, rows[s]);
    }
    multitierRows_ acc = {};
    while (has[0] || has[1] || has[2]) {
        int least = -1;
        for (int s = 0; s < 3; s++) {
            if (has[s] &&
                (least < 0 ||
                 databoxCompare((const databox *)&rows[s][0],
                                (const databox *)&rows[least][0]) < 0)) {
                least = s;
            }
        }
        multitierRowsAppend_(&acc, (const databox *)&rows[least][0],
                             (const databox *)&rows[least][1]);
        has[least] = multitierStreamNext_(t, least, &hi, &pi, &ci, rows[least]);
    }
    multitierRowsJoin_(&acc, &out);

    *len = flexBytes(out);
    return (uint8_t *)out;
}

static void *multitierPersistRestore_(const uint8_t *data, size_t len) {
    if (!data || !flexValidateBufferDeep(data, len) ||
        flexCount((const flex *)data) < MULTITIER_PERSIST_CONFIG) {
        return nullptr;
    }

    const flex *f = (const flex *)data;
    flexEntry *fe = flexHead(f);
    uint64_t v[MULTITIER_PERSIST_CONFIG];
    for (uint32_t i = 0; i < MULTITIER_PERSIST_CONFIG; i++) {
        if (!flexReadUnsigned(f, &fe, &v[i])) {
            return nullptr;
        }
    }
    if (v[1] == 0 || v[1] > 64 || v[2] == 0 || v[2] > UINT32_MAX ||
        v[4] < FLEX_CAP_LEVEL_64 || v[4] > FLEX_CAP_LEVEL_65536 || v[5] == 0 ||
        v[5] > UINT32_MAX || v[6] > 99 || v[7] > 12) {
        return nullptr;
    }

    const multitierConfig config = {
        .hotMaxBytes = v[0],
        .hotLevels = (uint32_t)v[1],
        .demoteBatch = (uint32_t)v[2],
        .coldMaxBytes = v[3],
        .cold = {.rawLimit = (flexCapSizeLimit)v[4],
                 .storedTarget = (uint32_t)v[5],
                 .minSavingPercent = (uint8_t)v[6],
                 .level = (uint8_t)v[7]},
    };
    cleaner(multitierFree) multitier *t = multitierNew(&config);
    if (!t) {
        return nullptr;
    }

    /* The rows, checked as any dump is (whole, keys strictly ascending) */
    cleaner(flexFree) flex *rows = flexDuplicate(f);
    flexDeleteRange(&rows, 0, MULTITIER_PERSIST_CONFIG);
    if (!coldmapRowsAscend(rows, 2)) {
        return nullptr;
    }

    /* Placement is not recorded: rows fill the hot level in key order up
     * to its limit, and the rest are cold */
    const size_t total = flexCount(rows) / 2;
    size_t hotRows = 0;
    flexEntry *at = total ? flexHead(rows) : nullptr;
    while (hotRows < total) {
        databoxBig kv[2] = {};
        flexEntry *next = multitierDecodeRow_(rows, at, kv);
        multidictPut(t->hot, (const databox *)&kv[0], (const databox *)&kv[1]);
        if (multidictIsOverLimit(t->hot)) {
            multidictDelete(t->hot, (const databox *)&kv[0]);
            break;
        }
        hotRows++;
        at = next;
    }

    /* @own: container; free=coldmapFree */
    flex *coldRows = flexSplitRange(&rows, (int32_t)(hotRows * 2),
                                    (uint32_t)((total - hotRows) * 2));
    coldmapFree(t->cold);
    t->cold = coldmapNewFromDump(2, &config.cold, coldRows);
    if (!t->cold) {
        t->cold = coldmapNew(2, &config.cold);
        return nullptr;
    }

    multitier *out = t;
    t = nullptr;
    return out;
}

static uint8_t *multitierPersistEncodeOp_(persistOp op, const void *args,
                                          size_t argc, size_t *len) {
    *len = 0;
    switch (op) {
    case PERSIST_OP_INSERT:
        return argc == 2 ? persistOpEncodeBoxes(args, argc, len) : nullptr;
    case PERSIST_OP_DELETE:
        return argc == 1 ? persistOpEncodeBoxes(args, argc, len) : nullptr;
    case PERSIST_OP_BULK_DELETE:
        /* The keys a sink took, one element each */
        return argc >= 1 ? persistOpEncodeBoxes(args, argc, len) : nullptr;
    default:
        return nullptr;
    }
}

static bool multitierPersistApplyOp_(void *structure, persistOp op,
                                     const uint8_t *data, size_t len) {
    multitier *t = *(multitier **)structure;
    if (!flexValidateBufferDeep(data, len)) {
        return false;
    }

    const flex *f = (const flex *)data;
    const size_t n = flexCount(f);
    if ((op == PERSIST_OP_INSERT && n != 2) ||
        (op == PERSIST_OP_DELETE && n != 1) ||
        (op == PERSIST_OP_BULK_DELETE && n == 0) ||
        (op != PERSIST_OP_INSERT && op != PERSIST_OP_DELETE &&
         op != PERSIST_OP_BULK_DELETE)) {
        return false;
    }

    /* Moves between levels are not logged (they change no row), so
     * replay runs them as it likes; a row leaves for the sink only where
     * the log says */
    t->replaying = true;
    flexEntry *fe = flexHead(f);
    if (op == PERSIST_OP_INSERT) {
        databoxBig kv[2] = {};
        multitierDecodeRow_(f, fe, kv);
        multitierSet(t, (const databox *)&kv[0], (const databox *)&kv[1]);
    } else {
        for (size_t i = 0; i < n; i++) {
            databoxBig key = {};
            flexGetByTypeBig(fe, &key);
            multitierDelete(t, (const databox *)&key);
            fe = flexNext(f, fe);
        }
    }
    t->replaying = false;
    return true;
}

static size_t multitierPersistCount_(const void *structure) {
    return multitierCount(structure);
}

static size_t multitierPersistBytes_(const void *structure) {
    return multitierBytes(structure);
}

static void multitierPersistFree_(void *structure) {
    multitierFree(structure);
}

/* The limits are fixed at creation, so there is no newEmpty */
const persistOps persistOpsMultitier = {
    .type = PERSIST_TYPE_MULTITIER,
    .name = "multitier",
    .formatVersion = 1,
    .minReadVersion = 1,
    .snapshot = multitierPersistSnapshot_,
    .restore = multitierPersistRestore_,
    .count = multitierPersistCount_,
    .estimateSize = multitierPersistBytes_,
    .encodeOp = multitierPersistEncodeOp_,
    .applyOp = multitierPersistApplyOp_,
    .free = multitierPersistFree_,
};

/* ====================================================================
 * Tests
 * ==================================================================== */
#ifdef DATAKIT_TEST
#include "ctest.h"

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

    TEST("a config a snapshot could not restore is refused") {
        const multitierConfig tooMany = {.hotLevels = 65};
        const multitierConfig most = {.hotLevels = 64};
        cleaner(multitierFree) multitier *ok = multitierNew(&most);
        if (multitierNew(&tooMany) || !ok) {
            ERR("%s", "65 levels taken or 64 refused");
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
