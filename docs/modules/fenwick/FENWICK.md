# Fenwick Trees (Binary Indexed Trees)

## Overview

A Fenwick tree keeps an array of numbers and answers **prefix sums**
(`e[0] + ... + e[i]`) and **point updates** (`e[i] += d`) in O(log n), with
element reads, range sums and rank queries (LowerBound) built on those.
datakit has three families of them, written once and instantiated many
ways:

| Family                                                                           | Values                                | Use it for                                                           |
| -------------------------------------------------------------------------------- | ------------------------------------- | -------------------------------------------------------------------- |
| **Typed** `fenwick<T>` (I16, I32, I64, U16, U32, U64, I128, U128, Float, Double) | one C type                            | the fastest and smallest tree when every value has one known type    |
| **Databox** `multiFenwickFlat`, `multiFenwick`                                   | any 64-bit int or finite float, mixed | exact sums over mixed or 65-bit-range values; results typed by value |
| **2D** `fenwick2d<T>` (I64, U64, Double), `multiFenwick2d`                       | as above                              | grids: point updates and rectangle sums                              |

For range **minimum/maximum**, or a range assignment, use a
[segment tree](SEGMENT.md) instead.

**Headers**: `fenwick/fenwick<T>.h`, `fenwick/fenwick2d.h`,
`multiFenwickFlat.h`, `multiFenwick.h`, `multiFenwick2d.h`

## Choosing one

| You need                                                                                                         | Choose                                                                     | Why                                                                                                 |
| ---------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------- |
| Counts or sums of one integer type, fastest                                                                      | `fenwickI64` (or `I32`/`I16` when every element and the sums you read fit) | 8 B (4/2 B) per element; updates 26-50 ns, queries 15-27 ns                                         |
| The smallest memory for large counter tables                                                                     | `fenwickI16` / `fenwickI32`                                                | 2/4 B per element, and at 4M elements rank queries are 2-4x faster than I64 (cache)                 |
| Doubles                                                                                                          | `fenwickDouble`                                                            | no kinds, no int128 bookkeeping                                                                     |
| Values that mix integers and doubles, or span the whole `[INT64_MIN, UINT64_MAX]` range, with exact integer sums | `multiFenwickFlat`                                                         | as fast as `fenwickI64` while every value is an int64; 1-2x slower and 9 B/elem once doubles appear |
| The most compact databox tree, speed secondary                                                                   | `multiFenwick`                                                             | about 3-4 B per element (flex cells), but 100-1000x slower                                          |
| A grid of counts                                                                                                 | `fenwick2dI64` / `multiFenwick2d`                                          | O(log R log C) updates and rectangle sums                                                           |
| Range min/max, or range assignment                                                                               | `segment<T>` ([SEGMENT.md](SEGMENT.md))                                    | Fenwick trees only sum                                                                              |

## Architecture

### Shared model

Cell `i` (1-based) holds the sum of elements `(i - LSB(i), i]`, where
`LSB(i) = i & -i`. Every tree stores `capacity` cells, the smallest power of
two above the highest index written, and grows by doubling. On growth only the
new power-of-two cells are touched: a new cell `j` covers existing elements
only when `j` is a power of two, and then holds the old total. The same
reasoning gives every tree:

- **Element reads in one walk**: element `i` is cell `i` minus the cells
  `i - 1, Prev(i - 1), ...` down to `i - LSB(i)`, with no prefix sums formed.
- **O(n) builds**: start the cells as the elements, then add each cell into
  its parent in index order.
- **Binary-lifting LowerBound**: descend the implicit tree, taking each block
  while the running prefix sum stays below the target. The result is always a
  crossing (`prefix(i) >= target` and, past 0, `prefix(i - 1) < target`), and
  the smallest index when elements are non-negative.

### Typed trees: one template, ten types

```
fenwickCoreAlgo.h          algorithms and arithmetic (static, per instantiation)
fenwickCoreImpl.h          Small tier: header + cells in one allocation
fenwickCoreFullImpl.h      Full tier: separately allocated cells, uint64 indexes
fenwickCoreDispatchImpl.h  the public API: a tagged pointer to Small or Full
fenwick<T>.c               #define FENWICK_SUFFIX / VALUE_TYPE + include
```

A tree starts in the Small tier and moves to the Full tier past the Small
tier's limits (about 128 KiB of cells) or when an index jumps more than 1000
past the end. The move copies the cells.

### Databox trees: arithmetic plus an adaptive cell store

All databox trees share the exact arithmetic of `multiFenwickCommon.h`
(`multiFenwickNum`: an int128 integer part plus a double part), so integer
sums stay exact even when some cells hold doubles.

- **`multiFenwickFlat`** and **`multiFenwick2d`** keep their cells in the
  adaptive store `multiFenwickCells.h`: one array of 64-bit cell values, plus
  a one-byte kind (signed, unsigned or double) per cell **only once some cell
  needs one**. While every cell is an int64 the store is 8 bytes per cell,
  and sums, updates and lifting take integer-only paths (int128 accumulation,
  no boxing). The first double, or value above INT64_MAX, adds the kinds
  array (9 B per cell, general path); `Clear` drops it again.
- **`multiFenwick`** keeps its cells in a multilist of flex nodes: 3-4 bytes
  per cell, but each cell access walks a flex node.

### 2D trees: one core, two cell models

`fenwick/fenwick2dCoreImpl.h` owns the 2D chains, growth and transactional
writes behind a small cell-store interface (load, encode, put, copy,
relayout, plus an optional integer fast path). `fenwick2dTypedImpl.h` plugs
in a plain value array for the typed trees; `multiFenwick2d.c` plugs in the
adaptive store. Rows and columns double independently; a column doubling
re-lays out every row. A write plans its cells against the grown tree,
reading the cells that growth would create virtually, so a refused write
changes nothing.

## Value contracts

|               | Typed `fenwick<T>`, `fenwick2d<T>`                                                        | Databox trees                                                                                                       |
| ------------- | ----------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------- |
| Accepted      | the C type; float types refuse NaN/inf                                                    | SIGNED_64, UNSIGNED_64, finite FLOAT_32/DOUBLE_64; anything else is refused                                         |
| Elements      | stored exactly; an Update whose element would leave the type is refused                   | each stored cell must fit `[INT64_MIN, UINT64_MAX]` (or be a finite double); a write pushing one outside is refused |
| Sums          | exact when they fit the type; otherwise modulo 2^bits (cells add with wrapping, never UB) | exact (int128); a result outside 64 bits returns false (result VOID)                                                |
| Results       | the C type                                                                                | typed by value: SIGNED_64 when negative, UNSIGNED_64 otherwise, DOUBLE_64 with any floating part                    |
| Refused write | changes nothing; a NULL tree stays NULL                                                   | the same                                                                                                            |

Out of range: 1D `Query`/`Get` past the count return 0 (typed) or false
(databox); 2D `Query`/`RangeQuery` clamp to the extents and `Get` outside them
is 0.

## API at a glance

```c
/* Typed 1D: void * handle, created on first write */
void *fw = NULL;
fenwickI64Update(&fw, 42, 1);               /* e[42] += 1 (false if refused) */
int64_t upTo42 = fenwickI64Query(fw, 42);   /* e[0] + ... + e[42] */
int64_t span = fenwickI64RangeQuery(fw, 10, 42);
int64_t e42 = fenwickI64Get(fw, 42);
fenwickI64Set(&fw, 42, 7);                  /* e[42] = 7 */
size_t rank = fenwickI64LowerBound(fw, 100); /* first i with prefix >= 100 */
fenwickI64Free(fw);

/* Databox 1D: same shape, databox values and results */
multiFenwickFlat *mf = NULL;
const databox one = DATABOX_SIGNED(1);
multiFenwickFlatUpdate(&mf, 42, &one);
databox sum;
if (multiFenwickFlatQuery(mf, 42, &sum)) { /* sum.type: by value */ }
multiFenwickFlatFree(mf);

/* 2D */
fenwick2dI64 *grid = NULL;
fenwick2dI64Update(&grid, row, col, 1);
int64_t box = fenwick2dI64RangeQuery(grid, r1, c1, r2, c2);
fenwick2dI64Free(grid);
```

Every write takes a pointer to the handle and creates the tree when it is
NULL; a refused write on a NULL tree leaves it NULL.

## Complexity

| Operation         | 1D                                                         | 2D                        |
| ----------------- | ---------------------------------------------------------- | ------------------------- |
| Update, Set       | O(log n) (typed: plus an element read for the range check) | O(log R log C)            |
| Query, RangeQuery | O(log n)                                                   | O(log R log C)            |
| Get               | O(log n), one walk                                         | O(log R log C)            |
| LowerBound        | O(log n)                                                   | -                         |
| NewFromArray      | O(n)                                                       | -                         |
| Growth            | amortized O(1) per appended index                          | O(new cells) per doubling |

`multiFenwick` has the same operation counts, but each cell access walks a
flex node (O(node size)).

## Performance

Measured with `fenwickBench` (`datakit-test test fenwickBench`, perf preset,
clang 22, Apple M1 Max, single thread, 2026-10-21). ns per operation;
B/elem includes the power-of-two capacity (10.5 B at 100K is 8 B cells over
a 131072-cell capacity). The full tables, with every size, are in
[PERFORMANCE_BASELINES.md](../../PERFORMANCE_BASELINES.md).

**All-integer counters** (+1 updates):

| tree                    | N    | update | query | lowerBound | B/elem |
| ----------------------- | ---- | -----: | ----: | ---------: | -----: |
| fenwickI64 (typed)      | 100K |     31 |    19 |         47 |   10.5 |
| multiFenwickFlat        | 100K |     37 |    19 |         76 |   10.5 |
| multiFenwick            | 100K | 32,510 | 15,910 |    32,341 |    3.9 |
| fenwickI64 (typed)      | 4M   |     50 |    27 |        327 |    8.4 |
| multiFenwickFlat        | 4M   |     74 |    38 |        455 |    8.4 |
| multiFenwick            | 4M   | 70,103 | 34,166 |    67,082 |    3.2 |

**Mixed values** (1% of values and updates are doubles; the typed tree that
can hold them is `fenwickDouble`):

| tree                    | N    | update | query | lowerBound | B/elem |
| ----------------------- | ---- | -----: | ----: | ---------: | -----: |
| fenwickDouble (typed)   | 100K |     30 |    19 |         53 |   10.5 |
| multiFenwickFlat        | 100K |     73 |    33 |        258 |   11.8 |
| multiFenwick            | 100K | 39,394 | 18,677 |    37,473 |    4.0 |
| fenwickDouble (typed)   | 4M   |     61 |    22 |        300 |    8.4 |
| multiFenwickFlat        | 4M   |    158 |    55 |        650 |    9.4 |
| multiFenwick            | 4M   | 79,703 | 39,086 |    73,656 |    3.2 |

`fenwickDouble` holds every integer as a double, so it is exact only up to
2^53; `multiFenwickFlat` keeps integer cells exact.

**Other types and trees** (4M elements, all-integer): `fenwickI16` update 44,
query 22, lowerBound 90, 2.1 B/elem; `fenwickI128` 106 / 46 / 557, 16.8
B/elem; segment SUM 108 / 86, get 7, 16.8 B/elem.

**Sparse growth** (n writes at random indexes of an empty tree, ns per write,
growth included): `fenwickI64` 35/50 ns at 100K/4M, `multiFenwickFlat`
38/86, segment SUM 42/106, `multiFenwick` 32,800 at 100K.

**2D grids** (about N cells): `fenwick2dI64` update 113 ns, query 27, rectangle
59 at 100K cells; 317/71/227 at 4M. `multiFenwick2d` is level with it on
integers (98/27/63 at 100K) and about 1.5-2x slower with 1% doubles.

### Where they trade places

- **Memory vs speed**: `multiFenwick` is the only tree under 4 B/elem with
  databox values, at 100-1000x the time. Below about 10K elements (or when
  operations are rare) that cost can be acceptable; above, use
  `multiFenwickFlat`.
- **Narrow types win at scale**: up to 100K elements every typed tree runs
  at the same speed; at 4M, cells stop fitting in cache and the narrow types
  pull ahead (I16 lower bound 90 ns vs 327 for I64; I128 is 2x slower than
  I64). Use the narrowest type whose elements and read sums fit.
- **Flat vs typed**: while every value is an int64, `multiFenwickFlat` runs
  level with `fenwickI64` up to about 100K elements (update 1.2x) and
  1.4-1.5x slower at 4M, in the same memory. Mixed values make it about 2x
  slower (about 3x for lowerBound) and 12% larger; it is then 2-2.5x behind
  `fenwickDouble`, which is faster when the data is all doubles but exact
  only to 2^53.
- **Fenwick vs segment for sums**: segment trees use 2x the memory and run
  queries 2-3x slower, but read an element in O(1) (1.5 ns), and support
  min/max and range assignment. For sums alone, prefer Fenwick.
- **Growth**: all flat trees grow in amortized O(1); random-order growth
  costs about one more update.

## Examples

### Frequency table with rank/select

```c
void *freq = NULL;
for (size_t i = 0; i < nScores; i++) {
    fenwickI32Update(&freq, scores[i], 1);       /* count each score */
}

/* How many scores are <= 70?  The median score? */
int32_t atMost70 = fenwickI32Query(freq, 70);
size_t median = fenwickI32LowerBound(freq, (int32_t)(nScores + 1) / 2);
fenwickI32Free(freq);
```

### Running totals of mixed amounts

```c
multiFenwickFlat *ledger = NULL;
const databox fee = DATABOX_DOUBLE(2.5);
const databox units = DATABOX_UNSIGNED(UINT64_MAX / 4);
multiFenwickFlatUpdate(&ledger, 0, &fee);
multiFenwickFlatUpdate(&ledger, 1, &units);   /* exact: int part is int128 */

databox total;
multiFenwickFlatQuery(ledger, 1, &total);     /* DOUBLE_64: has a float part */
multiFenwickFlatFree(ledger);
```

### Heatmap counts

```c
fenwick2dU64 *heat = NULL;
fenwick2dU64Update(&heat, y, x, 1);
uint64_t inBox = fenwick2dU64RangeQuery(heat, y0, x0, y1, x1);
fenwick2dU64Free(heat);
```

## Testing

- `fenwickTypes`: every typed tree against a naive array (refusals, wrapping
  sums, tier crossings, LowerBound, Clear).
- `fenwickI64`: the original I64 suite (tiers, edge cases, smoke benchmarks).
- `multiFenwick`, `multiFenwickFlat`: exact arithmetic, limits, refusals,
  growth; Flat is checked call by call against `multiFenwick`.
- `fenwick2d`, `multiFenwick2d`: against dense grid oracles, growth in rows,
  columns and both, sums past 64 bits.
- `fenwickBench`: the workload tables above (perf preset; validate skips it).

## See Also

- [SEGMENT.md](SEGMENT.md) - range min/max/sum trees
- [DATABOX.md](../core/DATABOX.md) - the value type and its numeric update
  policy
- [PERFORMANCE_BASELINES.md](../../PERFORMANCE_BASELINES.md) - full bench
  tables
