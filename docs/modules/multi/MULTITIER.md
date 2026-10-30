# multitier: a key-value map in hot, cold and sink levels

**Header**: `multitier.h` **Source**: `multitier.c` **Persistence**: `persistOpsMultitier`,
`persist/multitierP.h`

multitier holds key-value rows in levels, each costing memory only for the rows it holds:

| Level | Holds                                               | Bounded by                     |
| ----- | --------------------------------------------------- | ------------------------------ |
| hot   | a multidict tracking use in S4LRU levels (multilru) | `hotMaxBytes` of key and value |
| cold  | a [coldmap](COLDMAP.md): sorted, compressed blocks  | `coldMaxBytes` stored          |
| sink  | a callback taking the rows the cold level gives up  | (dropped when there is none)   |

A cache that drops its least used rows loses them. multitier keeps them compressed below the
hot level, holding several times more rows in the same memory, read back in microseconds, and
hands the rows it finally gives up to a sink (a disk store, another process) instead of
losing them silently. With no limits it is a multidict with LRU tracking.

## API

| Call                                                      | Does                                                                     |
| --------------------------------------------------------- | ------------------------------------------------------------------------ |
| `multitierNew(config)` / `multitierFree(t)`               | make and free (config NULL: no limits)                                   |
| `multitierSet(t, key, value)`                             | set in the hot level; a copy below goes (true: the key is new)           |
| `multitierGet(t, key, value)`                             | the value, moving a lower row up to the hot level (one LRU use)          |
| `multitierPeek(t, key, value)`                            | the value where it is: no move, no LRU use                               |
| `multitierDelete(t, key)`                                 | from whichever level holds it                                            |
| `multitierFlush(t)`                                       | the rows waiting for the cold level, put into it now                     |
| `multitierCount` / `multitierBytes` / `multitierGetStats` | rows, memory at every level, per-level rows, bytes and counters          |
| `multitierSetSink` / `multitierGetConfig`                 | the sink (a restored multitier has none until set) / the config in force |

`multitierConfig`: `hotMaxBytes` (0: no limit, nothing leaves the hot level), `hotLevels` (LRU
levels, 1-64, 0: 4), `demoteBatch` (rows waiting before they go cold, at least; 0: 64), `coldMaxBytes`
(0: no limit, nothing reaches the sink), `cold` (the cold level's `coldmapConfig`), `sink` and
`sinkCtx`.

```c
#include "multitier.h"

static void toDisk(void *ctx, const flex *rows) {
    /* rows: [key, value] in key order, borrowed for the call */
}

multitierConfig config = {
    .hotMaxBytes = 64u << 20,   /* 64 MB of hot keys and values */
    .coldMaxBytes = 256u << 20, /* 256 MB stored, compressed */
    .sink = toDisk,
};
multitier *t = multitierNew(&config);

const databox key = databoxNewBytesString("session:7f3a");
const databox value = databoxNewBytesString("{\"user\":42}");
multitierSet(t, &key, &value);

databox got = {};
if (multitierGet(t, &key, &got)) {
    /* 'got' is valid until the next call on 't' */
}

multitierFree(t);
```

Rules worth knowing:

- **Every key lives in one level**, so counts are exact: a Set of a key held lower takes the
  lower copy out.
- **Get moves, Peek reads**: a Get that finds a row below the hot level moves it up (the hot
  level may push others down to make room); a Peek leaves everything where it is.
- **The row a call just placed stays**: a Set or a Get never pushes its own key back down.
- **Values read are valid until the next call** on the multitier. Boxes are filled as
  multidict's are: zero them, or `DATABOX_BIG_INIT` a `databoxBig` for 96/128-bit values.
- **Rows go down in batches**: the rows the hot level gives up wait in a sorted batch (reads and
  writes see it) of at least `demoteBatch` and at least 4 per cold block, then go into the cold
  level together, so each block rewrite takes several rows. Cold rows a copy above supersedes
  leave the cold level the same way.
- **The cold level sheds whole blocks**: over `coldMaxBytes`, a block chosen at random leaves
  and its rows go to the sink.
- **A sink must not call back into the multitier**: it runs in the middle of the call that
  pushed the rows out.
- **A config out of range is refused**: `multitierNew` returns NULL for `hotLevels` past 64 or a
  cold config coldmap refuses.
- **Not thread-safe.**

## Costs

Apple M1 Max, clang 22, `perf` preset; 100,000 rows of `"user:NNNNNNNN"` -> a small record set
in a scattered order (perfRegression `multitier`):

| Setup                             | Memory  | Set     | Get, 9 in 10 on a tenth of keys | Get, any key |
| --------------------------------- | ------- | ------- | ------------------------------- | ------------ |
| all hot (no limits)               | 6.04 MB | 0.94 us | 0.33 us                         | 0.35 us      |
| hot held to a quarter of the rows | 2.79 MB | 4.7 us  | 1.5 us                          | 9.3 us       |

The hot path is multidict's. A read below the hot level costs a block decompression (about 2 us
at the default 8 KB cold blocks) plus the moves it causes; the cold level's block size is the
`DK_TIER_COLDMAP` default, which `dktools tune --families coldmap` sets per machine.

## Persistence

Persistence records what the multitier holds, not where: a snapshot is the config and every
live row in key order, and a restore fills the hot level in key order up to its limit with the
rest cold, with no use history (the first Gets warm rows again). A move between levels changes
no row and is never logged. WAL ops: INSERT `[key, value]`, DELETE `[key]`, and BULK_DELETE of
the keys a sink took. `multitierP` (`persist/multitierP.h`) logs a Set before making it, a
Delete only of a present key, and the rows its sink takes as they leave, whichever call pushed
them out; `multitierOpenP` (or `multitierSetSinkP`) takes the sink, which the files cannot
hold. The sink receives rows before their record is written, so a crash between the two, or a
refused record (`multitierSinkLogRefusedP` counts them), leaves the rows in the files as well:
they come back on the next open, held twice and never lost. A restored cold level is packed
afresh and holds what the batch held too, so it can stand over its limit until the first write
sheds blocks.
