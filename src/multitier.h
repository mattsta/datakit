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
 * the batch.  When the cold level passes its limit, a block at random
 * leaves whole, its rows to the sink.
 *
 * Values read are valid until the next call on the multitier.  Boxes are
 * filled as multidict's are: zero them (a 96/128-bit value then comes back
 * by type alone) or DATABOX_BIG_INIT a databoxBig.  Not thread-safe. */

/* @handle: fixed: its address holds from creation to free */
typedef struct multitier multitier;

/* Rows leaving the cold level: a dump-form flex of [key, value] rows in
 * key order, borrowed for the call */
typedef void multitierSinkFn(void *ctx, const flex *rows);

typedef struct multitierConfig {
    /* The hot level's limit of key and value bytes (0: no limit, so no
     * key ever leaves it) */
    uint64_t hotMaxBytes;
    /* The hot level's LRU levels (0: 4) */
    uint32_t hotLevels;
    /* Rows waiting for the cold level before they go in together (0: 64) */
    uint32_t demoteBatch;
    /* The cold level's limit of stored bytes (0: no limit, so no row ever
     * reaches the sink) */
    uint64_t coldMaxBytes;
    coldmapConfig cold;
    multitierSinkFn *sink;
    void *sinkCtx;
} multitierConfig;

/* NULL config: the defaults (no limits: a multidict with LRU tracking) */
[[nodiscard]] multitier *multitierNew(const multitierConfig *config);
void multitierFree(multitier *t);

/* Set 'key' to 'value' in the hot level (a copy at any lower level goes);
 * true when the key was new to the multitier */
bool multitierSet(multitier *t, const databox *key, const databox *value);
/* 'key's value, moving the key to the hot level (one LRU use there) */
bool multitierGet(multitier *t, const databox *key, databox *value);
/* 'key's value where it is: no move, no LRU use */
bool multitierPeek(multitier *t, const databox *key, databox *value);
bool multitierDelete(multitier *t, const databox *key);
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
    uint64_t coldRows;
    uint64_t coldStoredBytes;
    uint64_t coldRawBytes;
    uint64_t coldBlocks;
    /* Since creation */
    uint64_t hotHits;
    uint64_t coldHits; /* Gets that moved a key up (pending included) */
    uint64_t misses;
    uint64_t demotions; /* rows the hot level gave up */
    uint64_t sunkRows;  /* rows the cold level gave up */
    uint64_t sunkBlocks;
} multitierStats;
void multitierGetStats(const multitier *t, multitierStats *stats);

#ifdef DATAKIT_TEST
int multitierTest(int argc, char *argv[]);
#endif
