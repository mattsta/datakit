#include "multitier.h"

#include "cleaner.h"
#include "dkTierLimit.h"
#include "multidict.h"
#include "multilru.h"
#include "multimap.h"
#include "persist.h"

defcleaner(flex *, flexFree);
defcleaner(mflexState *, mflexStateFree);
defcleaner(multimap *, multimapFree);
defcleaner(multitier *, multitierFree);
defcleanerName(uint8_t *, zfree, TierBytes);

/* Rows a demotion batch holds per cold block, at least */
static constexpr size_t MULTITIER_ROWS_PER_COLD_BLOCK = 4;
/* A sweep over the limit frees down to this share below it (1 / 8) */
static constexpr uint64_t MULTITIER_SWEEP_SHARE = 8;
/* A cold row under a cold limit: [key, value, recent]: recent (true) since
 * its demotion or the clock's last pass over its block; old (false) once
 * the clock has passed it, and then the next pass takes it.  A cold row
 * has not been read since its demotion (a Get moves it up), so a row the
 * clock finds old has been unused for at least a full turn.  With no cold
 * limit nothing leaves, and a cold row is [key, value]. */
static constexpr multimapElements MULTITIER_COLD_ELEMENTS = 3;
static multimapElements multitierColdElements_(const multitierConfig *c) {
    return c->coldMaxBytes ? MULTITIER_COLD_ELEMENTS : 2;
}

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
    /* The clock: the first key it has not passed this turn, as a flex of
     * that one entry (NULL: the turn starts at the first block).  A key,
     * not a block index, so splits and merges cannot move it. */
    flex *hand;
    const databox
        *clockFrom;      /* during a sweep: rows below it stay as they are */
    const databox *keep; /* the key the running Set or Get just placed */
    /* Applying a WAL.  Without logged uses no row leaves for the sink (the
     * log records the ones that did); with them replay repeats each sweep
     * and gives its rows to the sink, which may see a row twice */
    bool replaying;
    uint64_t hotHits;
    uint64_t coldHits;
    uint64_t misses;
    uint64_t demotions;
    uint64_t sunkRows;
    uint64_t sweptBlocks;
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

/* The clock's test of each row of a block it sweeps: an old row goes, as
 * does a row a copy above supersedes; a recent row turns old.  Rows below
 * where the clock stood (a block that took swept rows in a merge) wait
 * for the next turn as they are. */
static coldmapSweepAction multitierClock_(void *ctx, const databox *const row[],
                                          databox *last) {
    const multitier *t = ctx;
    if (t->clockFrom && databoxCompare(row[0], t->clockFrom) < 0) {
        return COLDMAP_SWEEP_KEEP;
    }
    if (row[2]->type != DATABOX_TRUE || multimapExists(t->shadowed, row[0])) {
        return COLDMAP_SWEEP_REMOVE;
    }
    *last = (databox){.type = DATABOX_FALSE};
    return COLDMAP_SWEEP_CHANGE;
}

/* Over the cold level's limit, the clock sweeps on from where it stopped,
 * a block at a time, until the level is an eighth under its limit: every
 * old row it meets goes (its live ones to the sink, in key order), every
 * recent one turns old */
static void multitierColdPressure_(multitier *t) {
    if (!t->config.coldMaxBytes || (t->replaying && !t->config.logUses)) {
        return;
    }

    coldmapStats cs;
    coldmapGetStats(t->cold, &cs);
    if (cs.storedBytes <= t->config.coldMaxBytes) {
        return;
    }

    const uint64_t target =
        t->config.coldMaxBytes - t->config.coldMaxBytes / MULTITIER_SWEEP_SHARE;
    cleaner(flexFree) flex *removed = flexNew();
    /* Two turns at most: the first turns every row old, the second takes
     * them */
    uint64_t budget = 2 * (uint64_t)cs.blocks + 1;
    while (cs.storedBytes > target && cs.blocks && budget--) {
        /* The block holding the clock's key, from that key on */
        databoxBig from;
        size_t at = 0;
        t->clockFrom = nullptr;
        if (t->hand) {
            DATABOX_BIG_INIT(&from);
            flexGetByTypeBig(flexHead(t->hand), &from);
            t->clockFrom = (const databox *)&from;
            at = coldmapBlockFor(t->cold, t->clockFrom);
        }
        size_t next;
        coldmapSweepBlock(t->cold, at, multitierClock_, t, &removed, &next);
        t->clockFrom = nullptr;
        flexFree(t->hand);
        t->hand = next ? coldmapBlockFirstKey(t->cold, next) : nullptr;
        t->sweptBlocks++;
        coldmapGetStats(t->cold, &cs);
    }

    /* A superseded row only leaves; a live one goes to the sink */
    cleaner(flexFree) flex *sunk = flexNew();
    flexEntry *fe = flexCount(removed) ? flexHead(removed) : nullptr;
    for (size_t r = 0; r < flexCount(removed) / MULTITIER_COLD_ELEMENTS; r++) {
        databoxBig row[MULTITIER_COLD_ELEMENTS] = {};
        for (multimapElements e = 0; e < MULTITIER_COLD_ELEMENTS; e++) {
            flexGetByTypeBig(fe, &row[e]);
            fe = flexNext(removed, fe);
        }
        if (!multimapDelete(&t->shadowed, (const databox *)&row[0])) {
            const databox *both[2] = {(const databox *)&row[0],
                                      (const databox *)&row[1]};
            flexAppendMultipleExact(&sunk, 2, both);
        }
    }

    /* Replay sweeps only with logged uses, and then delivers too: the
     * live run may have died before its sink saw these rows */
    if (t->config.sink && flexCount(sunk)) {
        t->config.sink(t->config.sinkCtx, sunk);
    }
    t->sunkRows += flexCount(sunk) / 2;
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
    const size_t n = multimapCount(t->pending);
    if (n) {
        /* Each row replaces its key's cold row (a shadow mark on it ends)
         * and comes down recent */
        cleaner(flexFree) flex *rows = multimapDump(t->pending);
        cleaner(flexFree) flex *cold = flexNew();
        const databox recent = {.type = DATABOX_TRUE};
        flexEntry *fe = flexHead(rows);
        for (size_t r = 0; r < n; r++) {
            databoxBig kv[2] = {};
            flexGetByTypeBig(fe, &kv[0]);
            fe = flexNext(rows, fe);
            flexGetByTypeBig(fe, &kv[1]);
            fe = flexNext(rows, fe);
            multimapDelete(&t->shadowed, (const databox *)&kv[0]);
            const databox *row[MULTITIER_COLD_ELEMENTS] = {
                (const databox *)&kv[0], (const databox *)&kv[1], &recent};
            flexAppendMultipleExact(&cold, multitierColdElements_(&t->config),
                                    row);
        }
        coldmapInsertMany(t->cold, cold, nullptr);
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
    if (c->hotLevels > MULTILRU_LEVELS_MAX) {
        return nullptr;
    }
    /* @own: container; free=coldmapFree */
    coldmap *cold = coldmapNew(multitierColdElements_(c), &c->cold);
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
    flexFree(t->hand);
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

/* 'key's live cold row: its value into 'value' (borrowed from the cold
 * level's read state) */
static bool multitierColdFind_(multitier *t, const databox *key,
                               databox *value) {
    databox recent = {};
    databox *values[2] = {value, &recent};
    if (multimapExists(t->shadowed, key) ||
        !coldmapLookup(t->cold, t->state, key, values)) {
        return false;
    }
    return true;
}

bool multitierSet(multitier *t, const databox *key, const databox *value) {
    /* A key new to the hot level may sit lower: that copy goes (a cold
     * row by a shadow mark, not a rewrite of its block) */
    bool isNew = multidictPut(t->hot, key, value);
    if (isNew) {
        databox below = {};
        if (multimapDelete(&t->pending, key)) {
            isNew = false;
        } else if (coldmapCount(t->cold) &&
                   multitierColdFind_(t, key, &below)) {
            isNew = false;
            multitierShadow_(t, key);
        }
    }

    multitierHotPressure_(t, key);
    return isNew;
}

/* 'key's value below the hot level (borrowed from the batch or the cold
 * level's read state), and whether from the batch */
static bool multitierFindBelow_(multitier *t, const databox *key,
                                databox *value, bool *fromBatch) {
    databox *values[1] = {value};
    *fromBatch = multimapLookup(t->pending, key, values);
    return *fromBatch || multitierColdFind_(t, key, value);
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
        .sweptBlocks = t->sweptBlocks,
    };
}

size_t multitierWalk(multitier *t, multitierVisitFn *visit, void *ctx) {
    size_t visited = 0;
    multidictIterator di;
    if (multidictIteratorInit(t->hot, &di)) {
        multidictEntry e;
        while (multidictIteratorNext(&di, &e)) {
            visited++;
            if (!visit(ctx, &e.key, &e.val, MULTITIER_LEVEL_HOT)) {
                multidictIteratorRelease(&di);
                return visited;
            }
        }
        multidictIteratorRelease(&di);
    }

    databoxBig key;
    databoxBig val;
    databox *pair[2] = {(databox *)&key, (databox *)&val};
    multimapIterator mi;
    multimapIteratorInit(t->pending, &mi, true);
    for (;;) {
        DATABOX_BIG_INIT(&key);
        DATABOX_BIG_INIT(&val);
        if (!multimapIteratorNext(&mi, pair)) {
            break;
        }
        visited++;
        if (!visit(ctx, (const databox *)&key, (const databox *)&val,
                   MULTITIER_LEVEL_BATCH)) {
            return visited;
        }
    }

    cleaner(mflexStateFree) mflexState *state = mflexStateCreate();
    coldmapIterator ci;
    databox recent = {};
    databox *row[MULTITIER_COLD_ELEMENTS] = {(databox *)&key, (databox *)&val,
                                             &recent};
    coldmapIteratorInit(t->cold, state, &ci, true);
    for (;;) {
        DATABOX_BIG_INIT(&key);
        DATABOX_BIG_INIT(&val);
        if (!coldmapIteratorNext(&ci, row)) {
            break;
        }
        if (multimapExists(t->shadowed, (const databox *)&key)) {
            continue;
        }
        visited++;
        if (!visit(ctx, (const databox *)&key, (const databox *)&val,
                   MULTITIER_LEVEL_COLD)) {
            break;
        }
    }
    return visited;
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
/* The unsigneds before the parts, in order: the config, then the state */
typedef enum multitierPersistField_ {
    MT_PF_HOT_MAX_BYTES,
    MT_PF_HOT_LEVELS,
    MT_PF_DEMOTE_BATCH,
    MT_PF_COLD_MAX_BYTES,
    MT_PF_LOG_USES,
    MT_PF_COLD_RAW_LIMIT,
    MT_PF_COLD_STORED_TARGET,
    MT_PF_COLD_MIN_SAVING,
    MT_PF_COLD_LEVEL,
    MT_PF_HOT_HITS,
    MT_PF_COLD_HITS,
    MT_PF_MISSES,
    MT_PF_DEMOTIONS,
    MT_PF_SUNK_ROWS,
    MT_PF_SWEPT_BLOCKS,
    MT_PF_COUNT
} multitierPersistField_;
/* Then the clock's key (a null entry when its turn starts at the first
 * block), then the hot level, the batch, the shadowed keys and the cold
 * level, each the bytes of its own snapshot or dump */
static constexpr uint32_t MULTITIER_PERSIST_PARTS = 4;

/* The sink a restore on this thread gives the multitier it builds: with
 * logged uses, replay repeats each sweep and delivers its rows there */
static thread_local struct {
    multitierSinkFn *sink;
    void *sinkCtx;
} multitierRecoverySink_;

void multitierSetRecoverySink(multitierSinkFn *sink, void *sinkCtx) {
    multitierRecoverySink_.sink = sink;
    multitierRecoverySink_.sinkCtx = sinkCtx;
}

/* One part's bytes, as a BYTES entry */
static void multitierPersistPart_(flex **out, const void *bytes, size_t len) {
    const databox part = databoxNewBytes(bytes, len);
    flexPushByTypeExact(out, &part, FLEX_ENDPOINT_TAIL);
}

/* The snapshot holds the multitier whole: config, the clock and counters,
 * and each level as it is (the hot level's use history and the cold rows'
 * recency marks included), so a restore is the live multitier again */
static uint8_t *multitierPersistSnapshot_(const void *structure, size_t *len) {
    const multitier *t = structure;
    const multitierConfig *c = &t->config;
    const uint64_t fields[MT_PF_COUNT] = {
        [MT_PF_HOT_MAX_BYTES] = c->hotMaxBytes,
        [MT_PF_HOT_LEVELS] = c->hotLevels,
        [MT_PF_DEMOTE_BATCH] = c->demoteBatch,
        [MT_PF_COLD_MAX_BYTES] = c->coldMaxBytes,
        [MT_PF_LOG_USES] = c->logUses,
        [MT_PF_COLD_RAW_LIMIT] = c->cold.rawLimit,
        [MT_PF_COLD_STORED_TARGET] = c->cold.storedTarget,
        [MT_PF_COLD_MIN_SAVING] = c->cold.minSavingPercent,
        [MT_PF_COLD_LEVEL] = c->cold.level,
        [MT_PF_HOT_HITS] = t->hotHits,
        [MT_PF_COLD_HITS] = t->coldHits,
        [MT_PF_MISSES] = t->misses,
        [MT_PF_DEMOTIONS] = t->demotions,
        [MT_PF_SUNK_ROWS] = t->sunkRows,
        [MT_PF_SWEPT_BLOCKS] = t->sweptBlocks,
    };
    /* @own: caller; free=zfree */
    flex *out = flexNew();
    flexRow head = FLEX_ROW_INIT;
    for (size_t i = 0; i < countof(fields); i++) {
        flexRowUnsigned(&head, fields[i]);
    }
    flexRowAppendExact(&out, &head);
    databoxBig hand;
    DATABOX_BIG_INIT(&hand);
    if (t->hand) {
        flexGetByTypeBig(flexHead(t->hand), &hand);
    } else {
        ((databox *)&hand)->type = DATABOX_NULL;
    }
    flexPushByTypeExact(&out, (const databox *)&hand, FLEX_ENDPOINT_TAIL);

    size_t hotLen = 0;
    cleanerName(zfree, TierBytes) uint8_t *hot =
        persistOpsMultidict.snapshot(t->hot, &hotLen);
    cleaner(flexFree) flex *pending = multimapDump(t->pending);
    cleaner(flexFree) flex *shadowed = multimapDump(t->shadowed);
    size_t coldLen = 0;
    cleanerName(zfree, TierBytes) uint8_t *cold =
        persistOpsColdmap.snapshot(t->cold, &coldLen);
    multitierPersistPart_(&out, hot, hotLen);
    multitierPersistPart_(&out, pending, flexBytes(pending));
    multitierPersistPart_(&out, shadowed, flexBytes(shadowed));
    multitierPersistPart_(&out, cold, coldLen);

    *len = flexBytes(out);
    return (uint8_t *)out;
}

/* A part's bytes (a BYTES entry), or false */
static bool multitierPersistReadPart_(const flex *f, flexEntry **fe,
                                      const uint8_t **bytes, size_t *len) {
    if (!*fe) {
        return false;
    }
    databox part = {};
    flexGetByType(*fe, &part);
    if (part.type != DATABOX_BYTES) {
        return false;
    }
    *bytes = part.data.bytes.start;
    *len = part.len;
    *fe = flexNext(f, *fe);
    return true;
}

/* The levels agree: no key in both the hot level and the batch; every
 * shadowed key in the cold level and above it; every other cold key in no
 * level above */
static bool multitierPersistConsistent_(multitier *t) {
    multimapIterator it;
    databox key = {};
    databox val = {};
    databox *pair[2] = {&key, &val};
    multimapIteratorInit(t->pending, &it, true);
    while (multimapIteratorNext(&it, pair)) {
        if (multidictExistsNoTouch(t->hot, &key)) {
            return false;
        }
    }

    databox *one[1] = {&key};
    multimapIteratorInit(t->shadowed, &it, true);
    while (multimapIteratorNext(&it, one)) {
        if (!coldmapExists(t->cold, t->state, &key) ||
            (!multidictExistsNoTouch(t->hot, &key) &&
             !multimapExists(t->pending, &key))) {
            return false;
        }
    }

    cleaner(mflexStateFree) mflexState *state = mflexStateCreate();
    coldmapIterator ci;
    databox recent = {};
    databox *row[MULTITIER_COLD_ELEMENTS] = {&key, &val, &recent};
    coldmapIteratorInit(t->cold, state, &ci, true);
    while (coldmapIteratorNext(&ci, row)) {
        const bool above = multidictExistsNoTouch(t->hot, &key) ||
                           multimapExists(t->pending, &key);
        if ((coldmapElementsPerEntry(t->cold) == MULTITIER_COLD_ELEMENTS &&
             recent.type != DATABOX_TRUE && recent.type != DATABOX_FALSE) ||
            above != multimapExists(t->shadowed, &key)) {
            return false;
        }
        key = (databox){};
        val = (databox){};
    }
    return true;
}

static void *multitierPersistRestore_(const uint8_t *data, size_t len) {
    if (!data || !flexValidateBufferDeep(data, len)) {
        return nullptr;
    }

    const flex *f = (const flex *)data;
    flexEntry *fe = flexHead(f);
    uint64_t v[MT_PF_COUNT];
    for (size_t i = 0; i < countof(v); i++) {
        if (!flexReadUnsigned(f, &fe, &v[i])) {
            return nullptr;
        }
    }
    /* Here: each field fits its type, and holds a resolved value (a
     * snapshot keeps the config in force, never a 0 that means "the
     * default").  multitierNew and coldmapNew check the ranges. */
    if (!v[MT_PF_HOT_LEVELS] || v[MT_PF_HOT_LEVELS] > UINT32_MAX ||
        !v[MT_PF_DEMOTE_BATCH] || v[MT_PF_DEMOTE_BATCH] > UINT32_MAX ||
        v[MT_PF_LOG_USES] > 1 || !v[MT_PF_COLD_RAW_LIMIT] ||
        v[MT_PF_COLD_RAW_LIMIT] > UINT8_MAX || !v[MT_PF_COLD_STORED_TARGET] ||
        v[MT_PF_COLD_STORED_TARGET] > UINT32_MAX ||
        v[MT_PF_COLD_MIN_SAVING] > UINT8_MAX ||
        v[MT_PF_COLD_LEVEL] > UINT8_MAX ||
        flexCount(f) != countof(v) + 1 + MULTITIER_PERSIST_PARTS) {
        return nullptr;
    }

    const multitierConfig config = {
        .hotMaxBytes = v[MT_PF_HOT_MAX_BYTES],
        .hotLevels = (uint32_t)v[MT_PF_HOT_LEVELS],
        .demoteBatch = (uint32_t)v[MT_PF_DEMOTE_BATCH],
        .coldMaxBytes = v[MT_PF_COLD_MAX_BYTES],
        .logUses = v[MT_PF_LOG_USES],
        .cold = {.rawLimit = (flexCapSizeLimit)v[MT_PF_COLD_RAW_LIMIT],
                 .storedTarget = (uint32_t)v[MT_PF_COLD_STORED_TARGET],
                 .minSavingPercent = (uint8_t)v[MT_PF_COLD_MIN_SAVING],
                 .level = (cflexLevel)v[MT_PF_COLD_LEVEL]},
    };
    cleaner(multitierFree) multitier *t = multitierNew(&config);
    if (!t) {
        return nullptr;
    }
    /* The clock's key: any key routes to a block, so none is refused */
    databoxBig hand;
    DATABOX_BIG_INIT(&hand);
    flexGetByTypeBig(fe, &hand);
    fe = flexNext(f, fe);
    if (hand.type != DATABOX_NULL) {
        /* @own: container; free=multitierFree */
        t->hand = flexNew();
        flexPushByTypeExact(&t->hand, (const databox *)&hand,
                            FLEX_ENDPOINT_TAIL);
    }
    t->config.sink = multitierRecoverySink_.sink;
    t->config.sinkCtx = multitierRecoverySink_.sinkCtx;
    t->hotHits = v[MT_PF_HOT_HITS];
    t->coldHits = v[MT_PF_COLD_HITS];
    t->misses = v[MT_PF_MISSES];
    t->demotions = v[MT_PF_DEMOTIONS];
    t->sunkRows = v[MT_PF_SUNK_ROWS];
    t->sweptBlocks = v[MT_PF_SWEPT_BLOCKS];

    /* Each level from its own snapshot or dump, checked as that one is */
    const uint8_t *bytes[MULTITIER_PERSIST_PARTS];
    size_t lens[MULTITIER_PERSIST_PARTS];
    for (uint32_t p = 0; p < MULTITIER_PERSIST_PARTS; p++) {
        if (!multitierPersistReadPart_(f, &fe, &bytes[p], &lens[p])) {
            return nullptr;
        }
    }

    multidict *hot = persistOpsMultidict.restore(bytes[0], lens[0]);
    if (!hot) {
        return nullptr;
    }
    multidictFree(t->hot);
    multidictDefaultClassFree(t->hotClass);
    t->hot = hot;
    t->hotClass = multidictGetClass(hot);
    if (multidictLRULevels(hot) != config.hotLevels ||
        multidictGetMaxMemory(hot) != config.hotMaxBytes) {
        return nullptr;
    }
    multidictSetEvictPolicy(hot, MULTIDICT_EVICT_LRU);
    multidictSetEvictionCallback(hot, multitierOnEvict_, t);

    const flexCapSizeLimit limit = dkTierDefaultLimit(DK_TIER_MULTIMAP);
    for (uint32_t p = 1; p <= 2; p++) {
        if (!flexValidateBufferDeep(bytes[p], lens[p])) {
            return nullptr;
        }
        /* @own: container; free=multimapFree */
        flex *rows = zmalloc(lens[p]);
        memcpy(rows, bytes[p], lens[p]);
        multimap *m = p == 1 ? multimapNewFromDump(2, false, limit, rows)
                             : multimapNewFromDump(1, true, limit, rows);
        if (!m) {
            return nullptr;
        }
        multimap **into = p == 1 ? &t->pending : &t->shadowed;
        multimapFree(*into);
        *into = m;
    }

    coldmap *cold = persistOpsColdmap.restore(bytes[3], lens[3]);
    if (!cold) {
        return nullptr;
    }
    coldmapFree(t->cold);
    t->cold = cold;
    coldmapConfig coldConfig;
    coldmapGetConfig(cold, &coldConfig);
    if (coldmapElementsPerEntry(cold) != multitierColdElements_(&config) ||
        coldConfig.rawLimit != config.cold.rawLimit ||
        coldConfig.storedTarget != config.cold.storedTarget ||
        coldConfig.minSavingPercent != config.cold.minSavingPercent ||
        coldConfig.level != config.cold.level ||
        !multitierPersistConsistent_(t)) {
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
    case PERSIST_OP_USE:
        return argc == 1 ? persistOpEncodeBoxes(args, argc, len) : nullptr;
    case PERSIST_OP_BULK_DELETE:
        /* The keys a sink took, one element each */
        return argc >= 1 ? persistOpEncodeBoxes(args, argc, len) : nullptr;
    case PERSIST_OP_FLUSH:
        return argc == 0 ? persistOpNoBody(len) : nullptr;
    default:
        return nullptr;
    }
}

static bool multitierPersistApplyOp_(void *structure, persistOp op,
                                     const uint8_t *data, size_t len) {
    multitier *t = *(multitier **)structure;
    if (op == PERSIST_OP_FLUSH) {
        if (len) {
            return false;
        }
        t->replaying = true;
        multitierFlush(t);
        t->replaying = false;
        return true;
    }
    if (!flexValidateBufferDeep(data, len)) {
        return false;
    }

    const flex *f = (const flex *)data;
    const size_t n = flexCount(f);
    if ((op == PERSIST_OP_INSERT && n != 2) ||
        ((op == PERSIST_OP_DELETE || op == PERSIST_OP_USE) && n != 1) ||
        (op == PERSIST_OP_BULK_DELETE && n == 0) ||
        (op != PERSIST_OP_INSERT && op != PERSIST_OP_DELETE &&
         op != PERSIST_OP_USE && op != PERSIST_OP_BULK_DELETE)) {
        return false;
    }

    /* With logged uses replay repeats every move the live multitier made
     * (the sink not called); without, rows leave for the sink only where
     * the log says */
    t->replaying = true;
    flexEntry *fe = flexHead(f);
    if (op == PERSIST_OP_INSERT) {
        databoxBig kv[2] = {};
        flexGetByTypeBig(fe, &kv[0]);
        flexGetByTypeBig(flexNext(f, fe), &kv[1]);
        multitierSet(t, (const databox *)&kv[0], (const databox *)&kv[1]);
    } else if (op == PERSIST_OP_USE) {
        databoxBig key = {};
        flexGetByTypeBig(fe, &key);
        databox ignored = {};
        multitierGet(t, (const databox *)&key, &ignored);
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
#include "linearBloom.h"
#include "rapidhash.h"

defcleaner(linearBloom *, linearBloomFree);

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

/* The pattern MULTITIER.md documents: the sink writes the rows it takes to
 * a backing store (a multimap here) and adds their keys to a bloom filter;
 * a miss consults the store only when the filter says the key may be
 * there */
typedef struct multitierTestStore_ {
    multimap **rows;
    linearBloom *seen;
    uint64_t storeReads;
} multitierTestStore_;

static void multitierTestKeyHash_(const databox *key, uint64_t hash[2]) {
    hash[0] = rapidhash(key->data.bytes.start, key->len);
    hash[1] = rapidhashWithSeed(key->data.bytes.start, key->len, hash[0]);
}

static void multitierTestStoreSink_(void *ctx, const flex *rows) {
    multitierTestStore_ *store = ctx;
    flexEntry *fe = flexHead(rows);
    for (size_t r = 0; r < flexCount(rows) / 2; r++) {
        databox key = {};
        databox val = {};
        flexGetByType(fe, &key);
        fe = flexNext(rows, fe);
        flexGetByType(fe, &val);
        fe = flexNext(rows, fe);
        const databox *row[2] = {&key, &val};
        multimapInsert(store->rows, row);
        uint64_t hash[2];
        multitierTestKeyHash_(&key, hash);
        linearBloomHashSet(store->seen, hash);
    }
}

static bool multitierTestStoreGet_(multitier *t, multitierTestStore_ *store,
                                   const databox *key, databox *value) {
    if (multitierGet(t, key, value)) {
        return true;
    }
    uint64_t hash[2];
    multitierTestKeyHash_(key, hash);
    if (!linearBloomHashCheckEarlyExit(store->seen, hash)) {
        return false; /* in no level and never sunk */
    }
    store->storeReads++;
    databox *values[1] = {value};
    return multimapLookup(*store->rows, key, values);
}

/* A sink keeping the keys it takes */
static void multitierTestSinkKeys_(void *ctx, const flex *rows) {
    multimap **keys = ctx;
    flexEntry *fe = flexHead(rows);
    for (size_t r = 0; r < flexCount(rows) / 2; r++) {
        databox key = {};
        flexGetByType(fe, &key);
        const databox *row[1] = {&key};
        multimapInsert(keys, row);
        fe = flexNext(rows, fe);
        fe = flexNext(rows, fe);
    }
}

/* The lowest id ("key:NNNNNN") the cold level holds */
typedef struct multitierTestOldest_ {
    uint64_t min;
} multitierTestOldest_;

static bool multitierTestOldestCold_(void *ctx, const databox *key,
                                     const databox *value,
                                     multitierLevel level) {
    (void)value;
    multitierTestOldest_ *o = ctx;
    if (level == MULTITIER_LEVEL_COLD) {
        const uint64_t id =
            strtoull((const char *)key->data.bytes.start + 4, nullptr, 10);
        o->min = id < o->min ? id : o->min;
    }
    return true;
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
        const multitierConfig tooMany = {.hotLevels = MULTILRU_LEVELS_MAX + 1};
        const multitierConfig most = {.hotLevels = MULTILRU_LEVELS_MAX};
        cleaner(multitierFree) multitier *ok = multitierNew(&most);
        if (multitierNew(&tooMany) || !ok) {
            ERR("%s", "levels past the most taken, or the most refused");
        }
    }

    TEST("a snapshot restores at every point of a run that sweeps, deletes "
         "and merges cold blocks, and snapshots the same again") {
        const multitierConfig config = {
            .hotMaxBytes = 2048,
            .coldMaxBytes = 4096,
            .demoteBatch = 8,
            .cold = {.rawLimit = FLEX_CAP_LEVEL_512, .storedTarget = 128},
        };
        cleaner(multitierFree) multitier *t = multitierNew(&config);
        char key[32];
        char value[48];
        uint64_t rng = 0xc10c4a7d;
        size_t restored = 0;
        for (int step = 0; step < 4000; step++) {
            const uint64_t r = dkSplitmix64Next(&rng);
            const databox k = multitierTestKey_(key, sizeof(key), r % 900);
            if (r % 10 < 6) {
                const int vn =
                    snprintf(value, sizeof(value),
                             "{\"step\":%d,\"pad\":\"abcdef\"}", step);
                const databox v = databoxNewBytes(value, (size_t)vn);
                multitierSet(t, &k, &v);
            } else if (r % 10 < 9) {
                (void)multitierDelete(t, &k);
            } else {
                databox got = {};
                (void)multitierGet(t, &k, &got);
            }
            if (step % 100 == 99) {
                size_t len = 0;
                cleanerName(zfree, TierBytes) uint8_t *snap =
                    persistOpsMultitier.snapshot(t, &len);
                multitier *back = persistOpsMultitier.restore(snap, len);
                size_t againLen = 0;
                cleanerName(zfree, TierBytes) uint8_t *again =
                    back ? persistOpsMultitier.snapshot(back, &againLen)
                         : nullptr;
                if (!back || againLen != len || memcmp(again, snap, len) != 0) {
                    ERR("step %d: the snapshot %s", step,
                        back ? "restored to a different state"
                             : "did not restore");
                } else {
                    restored++;
                }
                multitierFree(back);
            }
        }
        multitierStats st;
        multitierGetStats(t, &st);
        if (restored != 40 || !st.sweptBlocks || !st.sunkRows) {
            ERR("%zu of 40 restores, %" PRIu64 " blocks swept, %" PRIu64
                " rows sunk",
                restored, st.sweptBlocks, st.sunkRows);
        }
    }

    TEST("a snapshot whose config field is out of range, or wraps into "
         "range in its type, is refused") {
        const multitierConfig config = {.hotMaxBytes = 4096,
                                        .coldMaxBytes = 8192};
        cleaner(multitierFree) multitier *t = multitierNew(&config);
        size_t len = 0;
        cleanerName(zfree, TierBytes) uint8_t *snap =
            persistOpsMultitier.snapshot(t, &len);
        multitier *same = persistOpsMultitier.restore(snap, len);
        if (!same) {
            ERR("%s", "an untouched snapshot was refused");
        }
        multitierFree(same);

        static const struct {
            multitierPersistField_ field;
            uint64_t value;
        } bad[] = {
            {MT_PF_HOT_LEVELS, 0},
            {MT_PF_HOT_LEVELS, MULTILRU_LEVELS_MAX + 1},
            {MT_PF_HOT_LEVELS, UINT64_C(1) << 32 | 4}, /* wraps to 4 */
            {MT_PF_DEMOTE_BATCH, 0},
            {MT_PF_LOG_USES, 2},
            {MT_PF_COLD_RAW_LIMIT, 0},
            {MT_PF_COLD_RAW_LIMIT, FLEX_CAP_LEVEL_65536 + 1},
            {MT_PF_COLD_STORED_TARGET, 0},
            {MT_PF_COLD_MIN_SAVING, MFLEX_SAVING_PERCENT_MAX + 1},
            {MT_PF_COLD_MIN_SAVING, 256 + 50}, /* wraps to 50 */
            {MT_PF_COLD_LEVEL, CFLEX_LEVEL_MAX + 1},
            {MT_PF_COLD_LEVEL, 256 + 1}, /* wraps to 1 */
        };
        for (size_t b = 0; b < countof(bad); b++) {
            const databoxBig with = {.data.u64 = bad[b].value,
                                     .type = DATABOX_UNSIGNED_64};
            cleaner(flexFree) flex *damaged =
                flexTestReplaceEntry((const flex *)snap, bad[b].field, &with);
            multitier *r =
                persistOpsMultitier.restore(damaged, flexBytes(damaged));
            if (r) {
                ERR("field %d = %" PRIu64 " restored", (int)bad[b].field,
                    bad[b].value);
                multitierFree(r);
            }
        }
    }

    TEST("a bloom filter in front of the sink's store: every row found, "
         "misses skip the store") {
        cleaner(multimapFree) multimap *stored = multimapNew(2);
        cleaner(linearBloomFree) linearBloom *seen = linearBloomNew();
        multitierTestStore_ store = {.rows = &stored, .seen = seen};
        const multitierConfig config = {
            .hotMaxBytes = 2048,
            .coldMaxBytes = 4096,
            .demoteBatch = 16,
            .cold = {.rawLimit = FLEX_CAP_LEVEL_512, .storedTarget = 128},
            .sink = multitierTestStoreSink_,
            .sinkCtx = &store,
        };
        cleaner(multitierFree) multitier *t = multitierNew(&config);
        char key[32];
        char value[48];
        const uint64_t rows = 3000;
        for (uint64_t i = 0; i < rows; i++) {
            const databox k = multitierTestKey_(key, sizeof(key), i * 2);
            const int vn = snprintf(value, sizeof(value), "v%" PRIu64, i);
            const databox v = databoxNewBytes(value, (size_t)vn);
            multitierSet(t, &k, &v);
        }

        /* Present keys (even ids) are found at some level or in the
         * store; absent ones (odd) almost never reach the store */
        uint64_t missing = 0;
        for (uint64_t i = 0; i < rows; i++) {
            const databox k = multitierTestKey_(key, sizeof(key), i * 2);
            databox got = {};
            const int vn = snprintf(value, sizeof(value), "v%" PRIu64, i);
            const databox want = databoxNewBytes(value, (size_t)vn);
            if (!multitierTestStoreGet_(t, &store, &k, &got) ||
                databoxCompare(&got, &want) != 0) {
                missing++;
            }
        }
        const uint64_t presentReads = store.storeReads;
        uint64_t falseFinds = 0;
        for (uint64_t i = 0; i < rows; i++) {
            const databox k = multitierTestKey_(key, sizeof(key), i * 2 + 1);
            databox got = {};
            falseFinds += multitierTestStoreGet_(t, &store, &k, &got);
        }
        const uint64_t missReads = store.storeReads - presentReads;
        if (missing || falseFinds || !presentReads || missReads > 3) {
            ERR("%" PRIu64 " rows missing, %" PRIu64 " absent found, %" PRIu64
                " store reads for present keys, %" PRIu64 " for misses",
                missing, falseFinds, presentReads, missReads);
        }
    }

    TEST("the cold level gives up its least recently demoted rows first") {
        /* Keys set in id order demote in id order: every row the sink
         * takes is older (a lower id) than every row the cold level keeps */
        cleaner(multimapFree) multimap *sunk = multimapSetNew(1);
        const multitierConfig config = {
            .hotMaxBytes = 2048,
            .coldMaxBytes = 6000,
            .demoteBatch = 16,
            .cold = {.rawLimit = FLEX_CAP_LEVEL_512, .storedTarget = 128},
            .sink = multitierTestSinkKeys_,
            .sinkCtx = &sunk,
        };
        cleaner(multitierFree) multitier *t = multitierNew(&config);
        char key[32];
        char value[48];
        for (uint64_t i = 0; i < 4000; i++) {
            const databox k = multitierTestKey_(key, sizeof(key), i);
            const int vn =
                snprintf(value, sizeof(value),
                         "{\"row\":%" PRIu64 ",\"pad\":\"abcdefgh\"}", i);
            const databox v = databoxNewBytes(value, (size_t)vn);
            multitierSet(t, &k, &v);
        }

        /* The oldest cold row still held, against the newest sunk */
        multitierTestOldest_ oldest = {.min = UINT64_MAX};
        multitierWalk(t, multitierTestOldestCold_, &oldest);
        uint64_t newestSunk = 0;
        multimapIterator it;
        databox sk = {};
        databox *one[1] = {&sk};
        multimapIteratorInit(sunk, &it, true);
        while (multimapIteratorNext(&it, one)) {
            const uint64_t id =
                strtoull((const char *)sk.data.bytes.start + 4, nullptr, 10);
            newestSunk = id > newestSunk ? id : newestSunk;
        }
        multitierStats st;
        multitierGetStats(t, &st);
        if (!st.sunkRows || !st.sweptBlocks || oldest.min == UINT64_MAX ||
            newestSunk >= oldest.min || st.coldStoredBytes > 6000) {
            ERR("sunk up to id %" PRIu64 ", cold from %" PRIu64 " (%" PRIu64
                " rows over %" PRIu64 " blocks swept, cold %" PRIu64 " bytes)",
                newestSunk, oldest.min, st.sunkRows, st.sweptBlocks,
                st.coldStoredBytes);
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
