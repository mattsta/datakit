# Tiered containers: one lifecycle, one set of tier rules

multimap, multiOrderedSet, multilist, multiarray and intset each keep their
data in the smallest representation that fits it (a tier, encoded in the
container pointer's tag), grow into larger tiers as they fill, and move
back down as they drain. They share one lifecycle and introspection shape,
described here; each module's own guide covers its operations.

Because the tier lives in the pointer, every call that can change the tier
takes `X **` and may rewrite the pointer: keep only the pointer you pass.

## The calls every family has

| Call                | multimap                        | multiOrderedSet              | multilist                     | multiarray                   | intset              |
| ------------------- | ------------------------------- | ---------------------------- | ----------------------------- | ---------------------------- | ------------------- |
| new, default limit  | `multimapNew(elements)`         | `multiOrderedSetNew()`       | `multilistNew()`              | `multiarrayNew(len, rowMax)` | `intsetNew()`       |
| new, explicit limit | `multimapNewLimit(elements, L)` | `multiOrderedSetNewLimit(L)` | `multilistNewLimit(L, depth)` | (rowMax is the limit)        | (no limit)          |
| free / reset        | `Free(*)` / `Reset(**)`         | same                         | same                          | same                         | same                |
| copy                | `multimapCopy`                  | `multiOrderedSetCopy`        | `multilistCopy`               | `multiarrayCopy`             | `intsetCopy`        |
| count / bytes       | `Count` / `Bytes`               | same                         | same                          | same                         | same                |
| shape               | `multimapStats`                 | `multiOrderedSetStats`       | `multilistStats`              | `multiarrayStats`            | `intsetStats`       |
| limit               | `multimapLimit`                 | `multiOrderedSetLimit`       | `multilistLimit`              | `multiarrayRowMax`           | (none)              |
| compact             | `multimapCompact(**)`           | `multiOrderedSetCompact(**)` | `multilistCompact(**, state)` | `multiarrayCompact(**)`      | `intsetCompact(**)` |

`Reset` leaves an empty first tier with the same configuration (row width,
set mode, limit, compression); a larger tier's structures are freed. A
multimap of references compacts with
`multimapCompactWithReference(**, atoms)`.

## Shape: dkTierStats

`XStats(x, &stats)` fills one `dkTierStats` (`src/dkTier.h`) the same way
for every family:

| field        | meaning                                                            |
| ------------ | ------------------------------------------------------------------ |
| `tier`       | 1 Small, 2 Medium, 3 Full (Large for multiarray)                   |
| `count`      | rows, members or elements held                                     |
| `units`      | inner maps, sub-maps, nodes or arrays holding them                 |
| `unitLimit`  | bytes a unit grows to before it splits (0: none)                   |
| `dataBytes`  | the stored data itself (compressed nodes at their compressed size) |
| `totalBytes` | `dataBytes` plus the tier's own structs and arrays                 |

`dataBytes / units` against `unitLimit` shows how full the units are; a
large gap after many deletes is what `Compact` closes.

## The size limit and its default

L is the size limit's byte cutoff (`flexOptimizationSizeLimit[limit]`).
The plain constructors of multimap, multiOrderedSet and multilist read a
process default per family, 2048 until changed:

```c
#include "dkTierLimit.h"

dkTierLimitResult r =
    dkTierSetDefaultLimit(DK_TIER_MULTIMAP, FLEX_CAP_LEVEL_1024);
/* DK_TIER_LIMIT_SET, DK_TIER_LIMIT_BAD_FAMILY or
 * DK_TIER_LIMIT_OUT_OF_RANGE (outside 64 B..64 KB); a refusal changes
 * nothing.  Containers made earlier keep their limit. */

flexCapSizeLimit now = dkTierDefaultLimit(DK_TIER_MULTIMAP);   /* in force */
flexCapSizeLimit built = dkTierCompiledLimit(DK_TIER_MULTIMAP); /* to reset to */
```

`dktools tune NAME` measures every limit on the machine it runs on, reads
each against the default, and per workload mix (every operation equally
often, 80% reads, 80% writes, or a `--mix` of your own) says `keep` unless
another limit is a clear win: 15% faster for the mix in every run, or 5%
fewer bytes while under 15% slower, and never with one operation 15% or
more slower. A win prints the `dkTierSetDefaultLimit` call that applies it;
a keep says how close the nearest limit came (tools/README.md). On Apple
M1 Max every family keeps 2048: the best alternatives are 10-13% faster
for multimap at 1024, within noise for multilist, and for multiOrderedSet
4096 slows `add` by a third.

## Tier rules

Each family grows at one point and shrinks well below it, so a container
never flips tiers at a boundary (one insert after a shrink never grows it
back).

| family          | grows                                                                 | deletes move it down                                                                                                    | deletes merge neighbours when the pair fits in  |
| --------------- | --------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------- |
| multimap        | Small -> Medium past L; Medium -> Full past 3L                        | Full with 3 inner maps or fewer -> Medium at 1.5L or less (Small at L/2); Medium -> Small at L/2                        | one inner map                                   |
| multiOrderedSet | same; also Small -> Medium at an add once the middle row is past 2 KB | same, but Small only at 2 KB or less (a Small set is a bare flex; its pointer holds the middle row's offset in 11 bits) | half a sub-map                                  |
| multilist       | same                                                                  | same (pops and range deletes)                                                                                           | half a node (uncompressed pairs, range deletes) |
| multiarray      | Small -> Medium at rowMax elements; Medium -> Large at rowMax nodes   | Large -> Medium at rowMax/2 nodes or fewer; Medium -> Small at rowMax/2 elements in at most 2 nodes                     | half a node                                     |
| intset          | by value width (16 -> 32 -> 64 bit) and count/byte caps               | never (one wide add would move it straight back)                                                                        | (one array per width)                           |

## Compact

Deletes keep containers reasonably dense without thrashing; `Compact` is
the explicit call for the densest layout. Every unit fills to the limit
(every one but the last within one row of it), then the container moves to
the smallest tier that holds its data without growing: Small at L or less
(multiOrderedSet: at L or 2 KB, whichever is less), Medium at 3L or less (multiarray: the tier `multiarrayNewFromArray` would
choose; intset: the narrowest width tier its values allow). Data and order
never change and the unit count never grows; multilist also keeps its old
layout when a repack of compressed nodes would come out larger. It costs
O(bytes), and the next insert may split or grow again.

Measured by each family's test (512-byte limit unless noted):

| family          | before                                      | after                   |
| --------------- | ------------------------------------------- | ----------------------- |
| multimap        | 3,000 rows, 90 inner maps, 26,480 B         | 48 inner maps, 25,178 B |
| multiOrderedSet | 75 members left in Full, 4,288 B            | Medium, 631 B           |
| multilist       | depth 2, 3 in 4 values deleted, 14,842 B    | 45 nodes, 6,804 B       |
| multiarray      | rowMax 8, 2 in 3 deleted, 75 nodes, 2,824 B | 25 nodes, 2,024 B       |
| intset          | Full with no 64-bit values, 228 B           | Medium, 204 B           |
