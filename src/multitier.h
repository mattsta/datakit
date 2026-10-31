#pragma once

#include "coldmap.h"
#include "databox.h"
#include "flex.h"

/* multitier: a key-value map in levels, each costing memory only for the
 * keys it holds:
 *   hot  - a multidict tracking use in S4LRU levels (multilru), up to a
 *          byte limit of key and value bytes;
 *   cold - a coldmap: the keys the hot level gave up, sorted and
 *          compressed in blocks, up to a limit of stored bytes;
 *   sink - a callback taking the rows the cold level gives up (dropped
 *          when there is none).
 * Every key lives in one level.  A Get that finds a key below the hot
 * level moves it back up; a Peek reads it where it is.  Rows the hot
 * level gives up wait in a sorted batch and go into the cold level
 * together (one compression per block they touch); reads and writes see
 * the batch.  When the cold level passes its limit, a clock sweeps its
 * blocks in turn from where it last stopped until the level is an eighth
 * under the limit: a row it passes once is marked old, and a row still
 * old when it comes round again leaves (a cold row has not been read
 * since its demotion: a Get moves it up), its live rows to the sink in
 * key order.  Each block the clock passes is rewritten once.
 *
 * Values read are valid until the next call on the multitier.  Boxes are
 * filled as multidict's are: zero them (a 96/128-bit value then comes back
 * by type alone) or DATABOX_BIG_INIT a databoxBig.  Not thread-safe. */

/* @handle: fixed: its address holds from creation to free */
typedef struct multitier multitier;

/* Rows leaving the cold level: a dump-form flex of [key, value] rows in
 * key order, borrowed for the call.  A sink must not call back into the
 * multitier: it runs in the middle of the call that pushed the rows out */
typedef void multitierSinkFn(void *ctx, const flex *rows);

typedef struct multitierConfig {
    /* The hot level's limit of key and value bytes (0: no limit, so no
     * key ever leaves it) */
    uint64_t hotMaxBytes;
    /* The hot level's LRU levels, 1 .. MULTILRU_LEVELS_MAX (0: 4) */
    uint32_t hotLevels;
    /* Rows waiting for the cold level before they go in together: at
     * least this many (0: 64), and at least 4 per cold block, so each
     * block a batch rewrites takes several rows */
    uint32_t demoteBatch;
    /* The cold level's limit of stored bytes (0: no limit, so no row ever
     * reaches the sink) */
    uint64_t coldMaxBytes;
    /* multitierP logs each Get and Flush as well, so a restore after a
     * crash repeats every move: the exact levels and use history, for a
     * WAL record per read (default false: reads log nothing) */
    bool logUses;
    coldmapConfig cold;
    multitierSinkFn *sink;
    void *sinkCtx;
} multitierConfig;

/* NULL config: the defaults (no limits: a multidict with LRU tracking).
 * NULL when the config is out of range (hotLevels past MULTILRU_LEVELS_MAX,
 * a cold config coldmapNew refuses) */
[[nodiscard]] multitier *multitierNew(const multitierConfig *config);
void multitierFree(multitier *t);
/* The sink rows leaving the cold level go to (NULL: dropped); a restored
 * multitier has none until set */
void multitierSetSink(multitier *t, multitierSinkFn *sink, void *sinkCtx);
/* The config in force, defaults resolved */
void multitierGetConfig(const multitier *t, multitierConfig *config);

/* Persistence (persistOpsMultitier) records the multitier whole: a
 * snapshot is its config, counters and every level as it is (the hot
 * level's use history, the batch, the shadowed keys, the cold blocks with
 * each row's demotion), and a restore is the live multitier again.  The
 * WAL logs Sets, Deletes and the rows a sink takes (multitierP); with
 * logUses it also logs each Get and Flush, so replay repeats every move
 * and a restore after a crash is exact too.  Without logUses a Get logs
 * nothing: a crash restores every row, with the levels and use history of
 * the last snapshot plus the logged writes (a clean close writes a
 * snapshot, so a clean restart is exact either way). */

/* Set 'key' to 'value' in the hot level (a copy at any lower level goes);
 * true when the key was new to the multitier */
bool multitierSet(multitier *t, const databox *key, const databox *value);
/* 'key's value, moving the key to the hot level (one LRU use there) */
bool multitierGet(multitier *t, const databox *key, databox *value);
/* 'key's value where it is: no move, no LRU use */
bool multitierPeek(multitier *t, const databox *key, databox *value);
bool multitierDelete(multitier *t, const databox *key);
/* Every live row once, hot level first (in no order), then the batch and
 * the cold level (each in key order), until 'visit' returns false; moves
 * nothing, uses nothing.  Rows are borrowed for the call.  Returns the
 * rows visited. */
typedef enum multitierLevel {
    MULTITIER_LEVEL_HOT = 0,
    MULTITIER_LEVEL_BATCH = 1, /* waiting for the cold level */
    MULTITIER_LEVEL_COLD = 2,
} multitierLevel;
typedef bool multitierVisitFn(void *ctx, const databox *key,
                              const databox *value, multitierLevel level);
size_t multitierWalk(multitier *t, multitierVisitFn *visit, void *ctx);
/* Rows at every level */
size_t multitierCount(const multitier *t);
/* Memory held at every level, overhead included */
size_t multitierBytes(const multitier *t);
/* The rows waiting for the cold level, put into it now */
void multitierFlush(multitier *t);

typedef struct multitierStats {
    uint64_t hotRows;
    uint64_t hotBytes; /* key and value bytes */
    uint64_t pendingRows;
    uint64_t coldRows;         /* live: a copy above supersedes the shadowed */
    uint64_t coldShadowedRows; /* superseded, leaving with the next batch */
    uint64_t coldStoredBytes;
    uint64_t coldRawBytes;
    uint64_t coldBlocks;
    /* Since creation */
    uint64_t hotHits;
    uint64_t coldHits; /* Gets that moved a key up (pending included) */
    uint64_t misses;
    uint64_t demotions;   /* rows the hot level gave up */
    uint64_t sunkRows;    /* rows the cold level gave up */
    uint64_t sweptBlocks; /* cold blocks the clock passed over */
} multitierStats;
void multitierGetStats(const multitier *t, multitierStats *stats);

#ifdef DATAKIT_TEST
int multitierTest(int argc, char *argv[]);
#endif
