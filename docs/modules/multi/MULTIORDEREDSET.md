# multiOrderedSet

A sorted set of `(score, member)` pairs: members are unique, pairs iterate in
ascending score, and a member's score and rank are found by the member. It
answers what a sorted map cannot cheaply: "what is X's score", "what rank is
X", alongside score ranges, ranks and set algebra.

The lifecycle, `multiOrderedSetStats`, the size-limit default, the tier rules
and `multiOrderedSetCompact` are shared with the other tiered containers:
[Tiered containers](../core/TIERS.md).

```c
#include "multiOrderedSet.h"

multiOrderedSet *board = multiOrderedSetNew();
const databox alice = databoxNewBytesString("alice");
const databox score = databoxNewSigned(1200);
multiOrderedSetAdd(&board, &score, &alice);           /* add or update */

databox got = {{0}};
multiOrderedSetGetScore(board, &alice, &got);          /* 1200 */
const int64_t rank = multiOrderedSetGetRank(board, &alice); /* 0-based */
multiOrderedSetFree(board);
```

Every mutating call takes `multiOrderedSet **`: the set may move to a larger
tier as it grows.

## Order

Pairs sort by score, then by member, both with `databoxCompare`: numbers by
value across integer and real types, and bytes in natural order (a digit run
compares as a number, so `"m2"` sorts before `"m10"`). Rank 0 is the lowest
score; negative ranks count from the end (`-1` is the highest).

Scores and members may be any databox the set can store, 96/128-bit integers
included; see "96/128-bit scores and members" in `multiOrderedSet.h` for where
a wide value lands when it is read.

## Tiers

The set promotes itself as it grows (never back; `Reset` empties it):

| tier   | layout                                                                                                                       | member lookup | finding a score's place                      |
| ------ | ---------------------------------------------------------------------------------------------------------------------------- | ------------- | -------------------------------------------- |
| Small  | one flex of `[score, member, ...]`                                                                                           | scan          | binary search                                |
| Medium | two flexes split by score                                                                                                    | scan of both  | binary search                                |
| Full   | a multidict member -> score, plus sub-maps (flexes split at the set's size limit, 2 KB by default) holding the rows in order | hash          | binary search to the sub-map, then within it |

In Small and Medium a rank by member is a scan, as the member lookup is.

In the Full tier the sub-maps hold the rows in `(score, member)` order and
split at their middle row when they outgrow their size, whatever the scores:
a run of members with one score (ties) spreads over as many sub-maps as it
needs, so ties cost what distinct scores do.

A drained set moves back down its tiers, well below the growth points (L,
the size limit's cutoff, and 3L), so it never flips at a boundary: Full
holding three sub-maps or fewer becomes Medium at 1.5L or less (Small at
L/2), and Medium becomes Small at L/2. Every removal (`Remove`,
`RemoveGetScore`, `RemoveIf`, `RemoveRangeByScore`, `RemoveRangeByRank`,
`PopMin`, `PopMax`) may therefore rewrite `*mos`, as additions always could.

Deletes keep Full dense as it shrinks. An emptied sub-map is freed. A
sub-map a delete leaves holding rows merges with a neighbour when the pair
fits in half a sub-map. A bulk remove or pop merges every such pair once,
when it finishes. The half-size margin keeps a score update (a remove, then
an insert) from merging a pair its insert would split again. A delete
moves the sub-map's middle row by at most one row, so its cost does not
grow with the sub-map's size.

`multiOrderedSetCompact(&mos)` packs on request: each Full sub-map fills to
the limit with the leading rows of the next (every one but the last ends
within a row of it), then the set moves to the smallest tier that holds its
bytes without growing (Small at L or less, Medium at 3L or less). Members,
scores and order are unchanged; it costs O(bytes) and rewrites `*mos`. A
3,000-member set at a 512-byte limit goes from 64 sub-maps to 50; after
removing three in four members, from 34 to 12; a 75-member set the
removals left in Full (4,288 bytes) becomes a 631-byte Medium (the
multiOrderedSet test prints these figures).

## Operations and their cost (Full tier)

| operation                                               | cost                                                                                                                          |
| ------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------- |
| `Add` / `AddNX` / `AddXX` / `AddGetPrevious` / `IncrBy` | hash lookup + O(log sub-maps) routing + one sub-map insert                                                                    |
| `Remove` / `RemoveGetScore` / `RemoveIf`                | hash lookup + routing + one sub-map delete (`RemoveIf` asks its `flexProceed` once the member is found; a no changes nothing) |
| `Exists` / `GetScore`                                   | hash lookup                                                                                                                   |
| `GetRank` / `GetReverseRank`                            | hash lookup + routing + counts of the sub-maps toward the nearer end                                                          |
| `GetByRank` / `IteratorInitAtRank`                      | counts of the sub-maps toward the nearer end, then within one sub-map                                                         |
| `CountByScore` / `IteratorInitAtScore`                  | O(log sub-maps) seek to the range start, then the rows in range                                                               |
| `RemoveRangeByScore` / `RemoveRangeByRank` / `PopMin`   | seek, then one delete per sub-map touched                                                                                     |
| `PopMax`                                                | from the last sub-map, row by row (deleting from a tail moves nothing)                                                        |
| iteration                                               | O(1) per step                                                                                                                 |

Rank positioning is linear in the number of sub-maps (about one per size
limit of rows: 2 KB by default). That is cheap through hundreds of
thousands of members; a set far larger that is rank-heavy would want
per-node row totals (not built), or a larger size limit.

`multiOrderedSetNew` takes its limit from a process default, 2048 until
`dkTierSetDefaultLimit(DK_TIER_MULTIORDEREDSET, limit)` (`dkTierLimit.h`)
changes it for sets made afterwards; `multiOrderedSetNewLimit` and
`multiOrderedSetNewCompress` take the limit directly. `dktools tune`
measures the choice on the machine it runs on.

Measured numbers per operation live in
[PERFORMANCE_BASELINES.md](../../PERFORMANCE_BASELINES.md), and
`dktools perfcompare NAME --base REV --modules multiOrderedSet` reproduces them
(`multiOrderedSetLarge` adds a 600k-member set).

## Updates

- `Add` adds or moves a member; it returns whether the member was present.
- `AddNX` adds only an absent member; `AddXX` updates only a present one.
- `AddGetPrevious` also returns the old score as a self-contained copy.
- `IncrBy` adds a delta in one member search, under the platform increment
  policy (two 64-bit integers add exactly; a real operand makes a double; an
  overflow or a non-finite result is refused and nothing changes).

Removing reads (`AddGetPrevious`, `RemoveGetScore`, `PopMin`, `PopMax`) return
owned boxes: release each with `databoxFreeData`.

## Ranges

A `mosRangeSpec` gives `min` and `max` with `minExclusive` / `maxExclusive`.
`CountByScore`, `RemoveRangeByScore` and `IteratorInitAtScore` (the first pair
at or above a score) take scores; `RemoveRangeByRank` takes an inclusive rank
range, clamped to the set (`(0, -1)` removes everything; a range outside the
set removes nothing).

## Set operations

`Union`, `Intersect` and `Difference` build a new set from `sets[]`. Union and
intersection weight each input's scores (`weights` NULL means 1.0) and combine
a member's scores with `MOS_AGGREGATE_SUM`, `MIN` or `MAX`, as doubles. Union
adds each member with one search of the result.

## Persistence

`multiOrderedSetP` (`persist/multiOrderedSetP.h`) wraps a set in a
`persistCtx`: `AddP`, `RemoveP` and `ResetP` log before they change the set,
reads go straight to it, and `OpenP` recovers it. See
[PERSIST.md](../persist/PERSIST.md).

## Choosing it

Use multiOrderedSet when lookups go by member (a player's score, an item's
rank) as well as by order. When everything goes by score (the key), a
`multimap` keyed by score is smaller and inserts faster.
