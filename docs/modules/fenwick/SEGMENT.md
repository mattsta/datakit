# Segment Trees (range SUM / MIN / MAX)

## Overview

A segment tree keeps an array and answers a **range fold** - the sum,
minimum or maximum of `e[left..right]` - in O(log n), with point assignment
in O(log n) and element reads in O(1). Choose the operation when creating
the tree. Use one when you need range **min/max**, range **assignment**, or
O(1) element reads; for sums alone a [Fenwick tree](FENWICK.md) is about
half the memory and 2-3x faster to query.

Types: `segmentI16`, `I32`, `I64`, `U16`, `U32`, `U64`, `I128`, `U128`,
`Float`, `Double`.

**Headers**: `segment/segment<T>.h` **Contract**: `segment/segmentCore.h`

## API

```c
void *seg = segmentI64New(SEGMENT_OP_MIN);    /* or SEGMENT_OP_SUM / _MAX */
segmentI64Update(&seg, 7, 42);                /* e[7] = 42 (assigns) */
segmentI64RangeUpdate(seg, 0, 99, 5);         /* e[0..99] = 5, existing only */
int64_t lo = segmentI64Query(seg, 10, 20);    /* min of e[10..20] */
int64_t e7 = segmentI64Get(seg, 7);
size_t n = segmentI64Count(seg);
segmentI64Free(seg);
```

## Contract

- **Update** assigns one element. An index past the end extends the tree;
  the elements in between hold the operation's **identity**: 0 for SUM, the
  type's maximum for MIN, its minimum for MAX.
- **Update on a NULL tree creates a SUM tree**: it cannot know which
  operation you meant. Create MIN and MAX trees with `New(op)` first.
- **RangeUpdate** assigns every _existing_ element in `[left, right]` in
  O(k + log n). It never extends or moves the tree, so the handle stays valid.
- **Query** folds `[left, right]` in O(log n). An integer SUM adds with
  wrapping: exact whenever the sum fits the type, modulo 2^bits otherwise
  (never undefined behaviour). Query and Get outside the tree return the
  identity (0 for a NULL tree).

## Architecture

The same template layout as the typed Fenwick trees:

```
segmentCoreAlgo.h          identity, combine, build, refresh, fold (static)
segmentCoreImpl.h          Small tier: header + nodes in one allocation
segmentCoreFullImpl.h      Full tier: separately allocated nodes
segmentCoreDispatchImpl.h  the public API (tagged pointer to Small or Full)
segment<T>.c               #define SEGMENT_SUFFIX / VALUE_TYPE + include
```

Nodes live in an implicit binary tree of `2 x leaves` slots (root at 1,
leaves at `[leaves, 2 x leaves)`). Leaves past the count hold the identity,
so extending the count within the leaves only sets one leaf and refreshes
its ancestors; growth doubles the leaves (amortized O(1) per appended
index). Queries fold bottom-up iteratively.

## Performance

`fenwickBench`, perf preset, clang 22, Apple M1 Max (ns per operation; the
counter-table update is a Get plus an Update, since Update assigns):

| tree                   |    N | update | prefix query | get | range | B/elem |
| ---------------------- | ---: | -----: | -----------: | --: | ----: | -----: |
| segmentI64 SUM         | 100K |     44 |           63 | 1.5 |    62 |   21.0 |
| segmentI64 MIN         | 100K |     46 |           62 | 1.4 |    64 |   21.0 |
| segmentI64 SUM         |   4M |    108 |           86 |   7 |    84 |   16.8 |
| segmentDouble MIN      |   4M |    132 |           84 |   6 |    84 |   16.8 |
| (fenwickI64, for sums) |   4M |     50 |           27 |  12 |    49 |    8.4 |

Sparse growth (random-order writes from empty): 42 ns per write at 100K,
106 at 4M.

## Bulk build and persistence

`segment<T>NewFromArray(op, values, count)` builds a tree in O(n), and
`segment<T>CopyTo(seg, out)` copies every element in O(n).

Every node is its children's fold, so the elements determine a tree, float
sums included: a tree built from an array answers every query bit for bit
as one built by `Update`.  `persistOpsSegment<T>` (`segment/segmentPersist.h`)
persist every type that way (see [PERSIST.md](../persist/PERSIST.md)):

- Snapshot: the operation, then the elements; restore through
  `NewFromArray`.
- WAL: `UPDATE [index, value]` replays `Update`, `UPDATE [left, right,
  value]` replays `RangeUpdate`.

## Testing

- `segmentTypes`: every type and operation against a naive array (range
  updates, tier crossings, identity past the end, wrapping sums), and
  NewFromArray / CopyTo against the source array and an Update-built tree.
- `segmentI64`: the original I64 suite with smoke-size benchmarks.
- `persistContract`: the ten `persistOpsSegment<T>` descriptors.

## See Also

- [FENWICK.md](FENWICK.md) - prefix-sum trees, the choosing guide
