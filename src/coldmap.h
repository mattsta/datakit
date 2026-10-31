#pragma once

#include "databox.h"
#include "flex.h"
#include "flexCapacityManagement.h"
#include "mflex.h"
#include "multimapCommon.h"

/* coldmap: a sorted map kept in compressed blocks, for rows that are read
 * far more than written.
 *
 * Rows [key, values...] in key order (databoxCompare), keys unique, as a
 * multimap map holds them.  The rows live in blocks: each a flex of whole
 * rows, LZ4-compressed when that saves its policy's share (mflex), with an
 * owned copy of its first key that routes lookups to it.  Sorted keys
 * share prefixes with their neighbours, so record-like rows compress to a
 * fraction of their flex size; rows that do not compress stay plain.
 *
 * A build (and a restore) packs blocks by STORED size: cutting from the
 * last row back, each block takes rows until it is expected to store
 * about 'storedTarget' bytes (from the ratio of the block cut before it,
 * the next in key order), up to a raw size of 'rawLimit' (the most one
 * read decompresses).  Writes split a block only at 'rawLimit' and merge
 * small ones.
 *
 * Reads take the caller's mflexState: a read decompresses its block into
 * that state, so readers with their own states may share one coldmap.
 * A value read as bytes points into the state (or into the coldmap for a
 * plain block): it stays valid until the next read through that state or
 * the next change to the coldmap.  Writers exclude readers.
 *
 * Elements decode into the caller's boxes as multimap's lookups do: a
 * box's 'big' flag must be set up (zero the box, or DATABOX_BIG_INIT a
 * databoxBig); a zeroed box reports a 96/128-bit value by type alone, a
 * databoxBig gets it whole. */

/* @handle: fixed: its address holds from creation to free */
typedef struct coldmap coldmap;

typedef struct coldmapConfig {
    /* The most raw (decompressed) bytes a block holds: the cost of one
     * read, as a size level (FLEX_CAP_LEVEL_1024, ...; not a byte count).
     * FLEX_CAP_LEVEL_0: the DK_TIER_COLDMAP default (dkTierLimit.h,
     * FLEX_CAP_LEVEL_8192 until set). */
    flexCapSizeLimit rawLimit;
    /* The stored bytes a block aims for (0: a quarter of the raw limit) */
    uint32_t storedTarget;
    /* A block stays compressed only when that saves this share (0 ..
     * MFLEX_SAVING_PERCENT_MAX) */
    uint8_t minSavingPercent;
    /* LZ4 level (0 .. CFLEX_LEVEL_MAX; flex.h) */
    cflexLevel level;
} coldmapConfig;

/* A coldmap of the rows of 'rows' (a flex of whole rows of
 * 'elementsPerEntry' elements in strictly ascending key order, as
 * multimapDump gives; consumed).  'config' NULL: the defaults.  NULL, with
 * 'rows' freed, when the rows do not ascend, 'elementsPerEntry' is 0 or
 * past MULTIMAP_ELEMENTS_MAX, or the config is out of range. */
[[nodiscard]] coldmap *coldmapNewFromDump(multimapElements elementsPerEntry,
                                          const coldmapConfig *config,
                                          flex *rows);
/* An empty coldmap (coldmapNewFromDump of no rows) */
[[nodiscard]] coldmap *coldmapNew(multimapElements elementsPerEntry,
                                  const coldmapConfig *config);
void coldmapFree(coldmap *cm);
/* The config it was made with, defaults resolved (the raw limit and
 * stored target in force) */
void coldmapGetConfig(const coldmap *cm, coldmapConfig *config);
/* Whether 'rows' holds whole rows of 'elementsPerEntry' with strictly
 * ascending keys: the form coldmapNewFromDump and coldmapInsertMany take */
bool coldmapRowsAscend(const flex *rows, multimapElements elementsPerEntry);

size_t coldmapCount(const coldmap *cm);
/* Memory held: stored blocks, routing keys and the coldmap itself */
size_t coldmapBytes(const coldmap *cm);
multimapElements coldmapElementsPerEntry(const coldmap *cm);

/* The values of 'key's row into elements[0 .. elementsPerEntry - 2]; false
 * when no row has the key */
bool coldmapLookup(const coldmap *cm, mflexState *state, const databox *key,
                   databox *elements[]);
bool coldmapExists(const coldmap *cm, mflexState *state, const databox *key);

/* Writes decompress the block they change and compress it again (several
 * microseconds a block): a coldmap suits rows that change rarely, or
 * arrive in batches.  A block past the raw limit splits in two; one that
 * stores under half the target merges with a neighbour when the two fit
 * the raw limit.  A write's boxes must not point into the coldmap's own
 * rows (values read from it): the block they point into is the one the
 * write changes, so copy such values first. */

/* Insert the row elements[0 .. elementsPerEntry - 1], replacing the row
 * with its key; true when it replaced one */
bool coldmapInsert(coldmap *cm, const databox *elements[]);
/* Insert every row of 'rows' (a dump's form: whole rows, strictly
 * ascending keys; not consumed), each replacing the row with its key: the
 * rows bound for one block go in with one decompression and one
 * compression.  'replaced' (may be NULL): how many replaced a row.  False,
 * nothing changed, when the rows do not ascend. */
bool coldmapInsertMany(coldmap *cm, const flex *rows, size_t *replaced);
/* Delete the row of every key in 'keys' (a flex of keys alone, strictly
 * ascending; not consumed): the keys bound for one block go with one
 * decompression and one compression.  'deleted' (may be NULL): rows
 * deleted.  False, nothing changed, when the keys do not ascend. */
bool coldmapDeleteMany(coldmap *cm, const flex *keys, size_t *deleted);
/* Remove every row 'remove' says to (it sees each row, key first, in key
 * order, and may tally the rows it keeps): each block holding one is
 * rewritten once, with what remains; blocks that shrink merge as a
 * delete's do.  The removed rows are appended to '*removed' (NULL: not
 * kept), in key order.  Returns the rows removed.  'remove' must not
 * change the coldmap. */
typedef bool coldmapRowFn(void *ctx, const databox *const row[]);
size_t coldmapRemoveIf(coldmap *cm, coldmapRowFn *remove, void *ctx,
                       flex **removed);
/* One block's rows, each kept, removed, or kept with its last element
 * changed ('fn' fills '*last', set up for any value, and answers CHANGE):
 * the block is rewritten once when any row is removed or changed (an
 * emptied block goes, a shrunken one may merge into the block before it),
 * and left as it is otherwise.  Removed rows are appended to '*removed' (NULL:
 * not kept) in key order.  '*next' is the block to sweep after it (0 past
 * the last: a sweep goes round).  Returns the rows removed. */
typedef enum coldmapSweepAction {
    COLDMAP_SWEEP_KEEP = 0,
    COLDMAP_SWEEP_REMOVE = 1,
    COLDMAP_SWEEP_CHANGE = 2,
} coldmapSweepAction;
typedef coldmapSweepAction coldmapSweepFn(void *ctx, const databox *const row[],
                                          databox *last);
size_t coldmapSweepBlock(coldmap *cm, size_t index, coldmapSweepFn *fn,
                         void *ctx, flex **removed, size_t *next);
/* Remove block 'index' (0 .. coldmapBlockCount - 1) whole: its rows, in
 * key order as a dump-form flex the caller frees; NULL past the end */
[[nodiscard]] flex *coldmapRemoveBlock(coldmap *cm, size_t index);
size_t coldmapBlockCount(const coldmap *cm);
/* Delete 'key's row; false when there is none */
bool coldmapDelete(coldmap *cm, const databox *key);
/* Delete the rows whose keys lie in [lo, hi] (a NULL bound: open at that
 * end); a block wholly inside goes without being decompressed.  The bounds
 * must not point into the coldmap's own rows.  Returns the rows deleted. */
size_t coldmapDeleteRange(coldmap *cm, const databox *lo, const databox *hi);

/* Rows in key order (forward) or reverse.  Next fills elements[0 ..
 * elementsPerEntry - 1] (key first) and opens blocks through 'state' as it
 * reaches them; a row's bytes stay valid until the next Next.  The
 * iterator holds its open block in 'state' and a place in the coldmap's
 * blocks: another read through 'state', or any change to the coldmap,
 * ends it (Init again to continue). */
typedef struct coldmapIterator {
    const coldmap *cm;
    mflexState *state;
    const flex *f;    /* the open block */
    flexEntry *entry; /* the next row's key (forward) or last element */
    size_t block;
    bool forward;
    bool done;
} coldmapIterator;

/* From the first row (forward) or the last; false when there is none */
bool coldmapIteratorInit(const coldmap *cm, mflexState *state,
                         coldmapIterator *it, bool forward);
/* From the first row whose key is at least 'key' (forward), or the last
 * row whose key is at most 'key' (reverse); false when there is none */
bool coldmapIteratorInitAt(const coldmap *cm, mflexState *state,
                           coldmapIterator *it, bool forward,
                           const databox *key);
bool coldmapIteratorNext(coldmapIterator *it, databox *elements[]);

/* Every row, in key order, as one flex (the multimapDump form) */
[[nodiscard]] flex *coldmapDump(const coldmap *cm, mflexState *state);

typedef struct coldmapStats {
    uint64_t rows;
    uint64_t blocks;
    uint64_t compressedBlocks;
    uint64_t rawBytes;          /* the rows as flex */
    uint64_t storedBytes;       /* the blocks as kept */
    uint32_t rawLimitBytes;     /* the most a block holds, open */
    uint32_t storedTargetBytes; /* what a block aims to store */
} coldmapStats;
void coldmapGetStats(const coldmap *cm, coldmapStats *stats);

#ifdef DATAKIT_TEST
int coldmapTest(int argc, char *argv[]);
#endif
