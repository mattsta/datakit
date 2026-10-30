# coldmap: a sorted map in compressed blocks

**Header**: `coldmap.h` **Source**: `coldmap.c` **Persistence**: `persistOpsColdmap`,
`persist/coldmapP.h`

coldmap holds rows `[key, values...]` in key order, as a multimap map does, in blocks that are
LZ4-compressed when that saves enough. Sorted keys share prefixes with their neighbours, so
record-like rows ("user:00001234" -> a small JSON value) store at 16-46% of their
flex size (about a quarter at the default 8 KB blocks). A read decompresses one block. A write decompresses a block and compresses it again,
so coldmap suits rows that are read far more than written, or that arrive in sorted batches: a
cache's cold level (see [MULTITIER.md](MULTITIER.md)), an archive, a reference table.

## API

| Call                                                                     | Does                                                                       |
| ------------------------------------------------------------------------ | -------------------------------------------------------------------------- |
| `coldmapNew(epe, config)` / `coldmapFree(cm)`                            | an empty coldmap of rows of `epe` elements (config NULL: defaults)         |
| `coldmapNewFromDump(epe, config, rows)`                                  | packed from a dump (multimapDump form: whole rows, keys ascending)         |
| `coldmapLookup(cm, state, key, values)`                                  | the values of `key`'s row (false: absent)                                  |
| `coldmapExists(cm, state, key)`                                          | whether `key` has a row                                                    |
| `coldmapIteratorInit` / `InitAt(…, forward, key)` / `Next`               | rows in key order or reverse, from an end or from a key                    |
| `coldmapInsert(cm, row)`                                                 | insert, replacing the row with its key (true: replaced)                    |
| `coldmapInsertMany(cm, rows, &replaced)`                                 | a sorted batch: one recompression per block it touches                     |
| `coldmapDelete(cm, key)` / `coldmapDeleteMany(cm, keys, &n)`             | one key / a sorted batch of keys                                           |
| `coldmapDeleteRange(cm, lo, hi)`                                         | every row with key in `[lo, hi]` (NULL: open); whole blocks undecompressed |
| `coldmapRemoveBlock(cm, i)` / `coldmapBlockCount(cm)`                    | take one block out whole, its rows returned as a dump                      |
| `coldmapDump(cm, state)`                                                 | every row, as one flex                                                     |
| `coldmapCount` / `coldmapBytes` / `coldmapGetStats` / `coldmapGetConfig` | rows, memory held, per-block totals, the config in force                   |

`coldmapConfig`:

| Field              | Means                                                                                                         | Default                                             |
| ------------------ | ------------------------------------------------------------------------------------------------------------- | --------------------------------------------------- |
| `rawLimit`         | a size level (`FLEX_CAP_LEVEL_*`, not bytes): the most decompressed bytes a block holds, the cost of one read | `DK_TIER_COLDMAP` (`FLEX_CAP_LEVEL_8192` until set) |
| `storedTarget`     | the stored bytes a block aims for                                                                             | a quarter of `rawLimit`                             |
| `minSavingPercent` | a block stays compressed only when that saves this share                                                      | 0 (any saving)                                      |
| `level`            | LZ4 level: 0 fast, 1-12 LZ4HC (smaller blocks, slower writes)                                                 | 0                                                   |

```c
#include "coldmap.h"

/* Build from a multimap's rows: [key, record] */
multimap *m = multimapNew(2);
/* ... fill it ... */
coldmap *cm = coldmapNewFromDump(2, nullptr, multimapDump(m));

mflexState *state = mflexStateCreate(); /* one per reader */
const databox key = databoxNewBytesString("user:00001234");
databox record = {};
databox *values[1] = {&record};
if (coldmapLookup(cm, state, &key, values)) {
    /* 'record' points into 'state' (or into a plain block): valid until the
     * next read through 'state' or the next change to the coldmap */
}

/* Old rows leave by range: blocks wholly inside go without a decompression */
const databox lo = databoxNewBytesString("user:00000000");
const databox hi = databoxNewBytesString("user:00000999");
coldmapDeleteRange(cm, &lo, &hi);

mflexStateFree(state);
coldmapFree(cm);
```

Rules worth knowing:

- **Reads take the caller's `mflexState`**: a read decompresses its block into that state, so
  readers with their own states may read one coldmap together. Writers exclude readers.
- **Values read point into the state** (or into the coldmap, for a block kept plain) and stay
  valid until the next read through that state or the next change to the coldmap.
- **Boxes are filled as multimap's are**: zero them, and a 96/128-bit value comes back by type
  alone; `DATABOX_BIG_INIT` a `databoxBig` to get it whole.
- **An iterator ends at the next read through its state or the next change**: it holds its
  open block in the state and a place among the blocks; Init again to continue.
- **Writes take boxes that do not point into the coldmap**: a value read from it points into
  the block a write changes, so copy it before writing it back.
- **Bad input is refused, not repaired**: `coldmapNewFromDump` returns NULL (and frees the rows)
  for rows that are not whole or not strictly ascending, a row width of 0 or past 65535, or a
  config out of range; `InsertMany` and `DeleteMany` return false and change nothing for an
  unsorted batch.
- **Batches are cheap, single rows are not**: a sorted batch (`InsertMany`, `DeleteMany`, a range)
  pays one recompression per block it touches; a single-row write pays one per row.
- **Rows that do not compress stay plain**: a block whose compressed form does not save
  `minSavingPercent` is kept as a flex. Integer rows (already dense) save 8-13%: set
  `minSavingPercent` to 20 and they stay plain, read with no decompression.

## Layout

```
coldmap
 ├── blocks[]: in key order, each
 │     rows    an mflex: a flex of whole rows, LZ4-compressed or plain
 │     first   an owned copy of the block's first key (routing; never borrowed)
 │     middle  the middle row's offset in the open flex (search starts there)
 │     count, raw bytes
 └── totals: rows, raw bytes, stored bytes, compressed blocks
```

A lookup binary-searches the blocks' first keys, opens one block and searches it from its
middle. Blocks are packed by stored size: a build cuts each block to store about `storedTarget`
bytes at the ratio the block next to it got, up to `rawLimit` raw bytes, with one compression
per block. A block past `rawLimit` splits in two; one storing under half `storedTarget` merges
with a neighbour when the two fit `rawLimit`.

## Costs

Apple M1 Max, clang 22, `perf` preset; 100,000 rows of `"user:NNNNNNNN"` ->
`{"plan":"team","seats":12,"region":"r3"}` (perfRegression `coldmap` and `coldmapLimits`):

| `rawLimit` | Memory  | Lookup  | Missing key | Scan, per row |
| ---------- | ------- | ------- | ----------- | ------------- |
| 1 KB       | 2.48 MB | 0.55 us | 0.55 us     | 23 ns         |
| 2 KB       | 1.80 MB | 0.73 us | 0.76 us     | 20 ns         |
| 4 KB       | 1.52 MB | 1.2 us  | 1.2 us      | 19 ns         |
| 8 KB       | 1.27 MB | 2.0 us  | 2.0 us      | 19 ns         |
| 16 KB      | 1.04 MB | 3.3 us  | 3.3 us      | 18 ns         |
| 32 KB      | 0.93 MB | 5.9 us  | 6.0 us      | 17 ns         |
| 64 KB      | 0.94 MB | 11.5 us | 11.5 us     | 17 ns         |

Memory includes the coldmap's own scratch state, two buffers of one block each (128 KB at
64 KB blocks). A build costs about 100 ns a row; a single-row insert or delete about 5 us at 8 KB blocks. A
multimap holds the same rows in about 6 MB. How much a block saves depends on the rows: integer
-> integer rows store at 87-92% of their flex size (kept plain under any required saving above
that), short repeated phrases at 52-70%, records at 16-46% (perfRegression `mapBlockCompress`).

`rawLimit` trades read time against memory, and the right point depends on the machine:
`dktools tune NAME --families coldmap` measures every limit here and names the
`dkTierSetDefaultLimit(DK_TIER_COLDMAP, …)` call that applies a better one
([TIERS.md](../core/TIERS.md)).

## Persistence

A snapshot is one flex: the config (row width, raw limit, stored target, required saving, LZ4
level, defaults resolved) and every row in key order. A restore checks the rows (whole, keys
strictly ascending) and packs them as a build does. WAL ops: INSERT a row, DELETE a key or a sorted batch of keys,
BULK_INSERT a sorted batch, BULK_DELETE a key range. `coldmapP` (`persist/coldmapP.h`) logs
each change before making it: a refused log changes nothing, and a change that would change
nothing (a missing key, an empty batch, a range holding no row) logs nothing; a batch of
deletes is logged as given (a key with no row is skipped on replay).
