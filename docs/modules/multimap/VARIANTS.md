# multimap Variants - Scale-Aware Design

## Overview

The multimap family implements a **scale-aware architecture**: three storage
tiers behind one opaque `multimap *`, plus `multimapAtom`, a reference-counting
string-interning container built _out of_ multimaps rather than being a tier of
its own.

Your code holds a single `multimap *` and the implementation picks the tier;
the only requirement is that mutating calls take `multimap **` so the pointer
can be rewritten after an upgrade.

## The Tiers

| Tier       | Container | Submaps     | Per-submap metadata | Promotes when         |
| ---------- | --------- | ----------- | ------------------- | --------------------- |
| **Small**  | 16 bytes  | 1 flex      | none                | bytes > limit         |
| **Medium** | 28 bytes  | 2 flex      | none                | bytes > limit \* 3    |
| **Full**   | 48 bytes  | N flex      | 8 + 4 + 16 bytes    | never (final tier)    |
| **Atom**   | wrapper   | 2 multimaps | n/a                 | not a tier; see below |

"limit" is `flexOptimizationSizeLimit[level]` for the `flexCapSizeLimit` level
stored in the tagged pointer (default `FLEX_CAP_LEVEL_2048` = 2048 bytes).

There is no downgrade path. A container that reached Full stays Full.

## multimapSmall

### Structure

From `multimapSmallInternal.h`:

```c
/* 8 + 4 + 4 = 16 bytes */
struct multimapSmall {
    flex *map;                              /* single map */
    uint32_t middle;                        /* byte offset of the middle row */
    multimapElements elementsPerEntry : 16; /* max 64k columns per row */
    multimapElements compress : 1;          /* recorded, not yet consumed */
    multimapElements mapIsSet : 1;          /* true if rows must be unique */
    multimapElements isSurrogate : 1;       /* true if all keys need refs */
    multimapElements unused : 13;
};
```

A static assertion pins the size at `sizeof(flex *) + 2 * sizeof(uint32_t)`.

### Architecture

```
┌──────────────────────────────────────┐
│ multimapSmall (16 bytes)             │
├──────────────────────────────────────┤
│ flex *map          ──────────────┐   │
│ middle = 0x0120                  │   │
│ elementsPerEntry = 2             │   │
│ flags                            │   │
└──────────────────────────────────┼───┘
                                   │
                                   ▼
        ┌─────────────────────────────────────┐
        │  Single flex (sorted key-value)     │
        ├─────────────────────────────────────┤
        │ [K1][V1][K2][V2][K3][V3]...[Kn][Vn] │
        │          ▲                          │
        │          └─ middle points here      │
        └─────────────────────────────────────┘
```

`middle` is a cached byte offset, not a row index. It seeds the binary search so
a find does not have to locate the midpoint first. Every mutation path either
maintains it incrementally or forces a full `flexMiddle()` rescan; a raw flex
write that bypasses the multimap API leaves it stale and silently poisons every
later search.

### Characteristics

**Advantages**

- Minimal overhead: 16 bytes of container for the whole structure
- Cache friendly: one contiguous allocation
- Simplest search: one binary search, no routing step

**Disadvantages**

- Insert and delete memmove around the whole flex
- One allocation means every write touches the entire structure

**Upgrade trigger** (from `multimapUpgradeIfNecessary_()`):

```c
if (multimapSmallBytes(small) > flexOptimizationSizeLimit[limit] &&
    multimapSmallCount(small) > (small->elementsPerEntry * 2)) {
    /* Need at least two complete rows so the split yields two non-empty
     * halves; splitting [ROW, <nothing>] would leave map[1] empty and all
     * later inserts would pile into map[0]. */
}
```

Note `multimapSmallCount()` already divides by `elementsPerEntry`, so the guard
means "more than `2 * elementsPerEntry` rows".

### Best Use Cases

1. Small dictionaries and configuration maps
2. Short-lived lookup tables
3. Any container expected to stay under the size limit

### Example

```c
multimap *config = multimapNewLimit(2, FLEX_CAP_LEVEL_512);

databox k1 = databoxNewBytesString("timeout");
databox v1 = databoxNewSigned(30);
multimapInsert(&config, (const databox *[]){&k1, &v1});

databox k2 = databoxNewBytesString("retries");
databox v2 = databoxNewSigned(3);
multimapInsert(&config, (const databox *[]){&k2, &v2});

/* Still Small: well under the 512-byte level */
assert(multimapBytes(config) < 512);
```

## multimapMedium

### Structure

From `multimapMediumInternal.h`:

```c
/* 8 + 8 + 4 + 4 + 4 = 28 bytes */
struct multimapMedium {
    flex *map[2];       /* maps stored in low->high order */
    uint32_t middle[2]; /* byte offsets of each map's middle row */
    multimapElements elementsPerEntry : 16;
    multimapElements compress : 1;
    multimapElements mapIsSet : 1;
    multimapElements isSurrogate : 1;
    multimapElements unused : 13;
};
```

### Architecture

```
┌────────────────────────────────────┐
│ multimapMedium (28 bytes)          │
├────────────────────────────────────┤
│ map[0] ────────┐                   │
│ map[1] ────────┼─┐                 │
│ middle[0]      │ │                 │
│ middle[1]      │ │                 │
│ elementsPerEntry │ │               │
│ flags          │ │                 │
└────────────────┼─┼─────────────────┘
                 │ │
        ┌────────┘ └─────────┐
        ▼                    ▼
  ┌──────────┐         ┌──────────┐
  │ map[0]   │         │ map[1]   │
  │ [LOWER]  │         │ [HIGHER] │
  ├──────────┤         ├──────────┤
  │ K1...Kn  │         │ Kn+1...Km│
  └──────────┘         └──────────┘
   keys < head(map[1])  keys >= head(map[1])
```

### How Medium Works

Upgrading from Small `zrealloc()`s the 16-byte container into a 28-byte one and
calls `flexSplitMiddle()` at the cached middle offset, producing `map[0]` (low)
and `map[1]` (high). Both middles are then recomputed from scratch.

**Routing** (`multimapMediumBinarySearch_`) is a single comparison, not a search:

```c
if (flexCount(map[1]) == 0) {
    return 0;                       /* everything lives in map[0] */
}
compare head(map[1]) against key;
return (head <= key) ? 1 : 0;
```

So a Medium lookup is O(1) routing plus one binary search over roughly half the
rows. The head of `map[1]` is decoded into a plain `databox` temporary with a
stack `databoxWide` slot (resolving a reference through the atom container on
surrogate-keyed maps), so a 96/128-bit head key is compared by value and a
narrow one never touches the slot.

### Characteristics

**Advantages**

- Insert/delete memmove touches about half the data
- Two smaller allocations behave better in cache than one large one
- Only 12 bytes more container state than Small

**Disadvantages**

- Still only two submaps, so the halving does not compound
- Every mutation must preserve the map[0]-non-empty invariant

**Upgrade trigger**:

```c
if (multimapMediumBytes(medium) > (flexOptimizationSizeLimit[limit] * 3) &&
    flexCount(medium->map[0]) && flexCount(medium->map[1])) {
    /* Both submaps must be non-empty to seed a Full with two valid maps. */
}
```

Note this is a **non-emptiness** requirement, not a row-count multiple of
`elementsPerEntry`.

### Best Use Cases

1. Mid-size caches that have outgrown a single flex
2. Session and index structures with steady growth
3. Transitional datasets whose final size is unknown

### Example

```c
multimap *m = multimapNewLimit(2, FLEX_CAP_LEVEL_2048);

for (int64_t i = 0; i < 500; i++) {
    databox k = databoxNewSigned(i);
    databox v = databoxNewSigned(i * 2);
    multimapInsert(&m, (const databox *[]){&k, &v});
}

/* Whether this is still Small or already Medium depends on encoded byte
 * size, not row count: small integers pack tightly. */
```

### Conformance Rule

Medium requires: **if any rows exist, `map[0]` is non-empty.** Searches stop
early on an empty `map[0]`, so data stranded in `map[1]` would be unreachable.

`multimapMediumConform_()` enforces it by **swapping the two slots** (pointers
and middles) whenever `map[0]` empties while `map[1]` still holds rows. Nothing
is copied.

```c
/* Before:  map[0] = [10, 20]   map[1] = [30, 40, 50] */

multimapDelete(&m, &key10);
multimapDelete(&m, &key20);

/* After:   map[0] = [30, 40, 50]   map[1] = []       */
```

The same conform runs at the end of `multimapMediumAppend()`, because appending
into `map[1]` while `map[0]` is empty (for instance, the first insert after a
full drain) would otherwise strand the row.

## multimapFull

### Structure

From `multimapFullInternal.h`:

```c
struct multimapFull {
    multiarray *map;           /* flex *; maps in low->high order */
    multiarray *middle;        /* multimapFullMiddle; middle byte offsets */
    multiarray *rangeBox;      /* databox; head key of maps 1..N-1 */
    multimapFullIdx count;     /* number of submaps */
    multimapFullValues values; /* total rows across every submap */
    multimapElements elementsPerEntry;
    uint32_t maxSize : 17;     /* per-submap split cutoff, in bytes */
    uint32_t mapIsSet : 1;
    uint32_t compress : 1;
    uint32_t isSurrogate : 1;
    uint32_t unused : 12;
};
```

On a 64-bit target this lays out as 24 bytes of pointers, a 4-byte `count`,
4 bytes of padding, an 8-byte `values`, then `elementsPerEntry` and the flag
word: **48 bytes**, plus whatever the three `multiarray` containers allocate.

### Architecture

```
┌─────────────────────────────────────────┐
│ multimapFull (48 bytes)                 │
├─────────────────────────────────────────┤
│ map ──────┐                             │
│ middle ───┼─┐                           │
│ rangeBox ─┼─┼─┐                         │
│ count = N │ │ │                         │
│ values    │ │ │                         │
└───────────┼─┼─┼─────────────────────────┘
            │ │ │
    ┌───────┘ │ │
    │   ┌─────┘ │
    │   │  ┌────┘
    ▼   ▼  ▼
 ┌─────┬─────┬──────────┐
 │map[]│mid[]│rangeBox[]│
 │  0  │  0  │    -     │
 │  1  │  1  │  RB[0]   │
 │  2  │  2  │  RB[1]   │
 │ ... │ ... │   ...    │
 │ N-1 │ N-1 │  RB[N-2] │
 └─────┴─────┴──────────┘
   │     │       │
   │     │       └─ head key of map[i+1] (routing)
   │     └───────── cached middle byte offset per submap
   └─────────────── the flex* holding the rows
```

`rangeBox` has `count - 1` entries: `map[0]`'s range is implicitly "below
`rangeBox[0]`", so no box is stored for it.

### How Full Works

**Two-level search**:

```c
/* To find key K: */
1. Binary search rangeBox[0 .. count-2] to pick the submap
2. Binary search inside that submap's flex
```

Step 1 is a genuine binary search (`multimapFullBinarySearch`), so routing is
O(log M) for M submaps.

```
rangeBox[0] = 100    (map[1] starts at 100)
rangeBox[1] = 500    (map[2] starts at 500)
rangeBox[2] = 1000   (map[3] starts at 1000)
rangeBox[3] = 5000   (map[4] starts at 5000)

Looking for key 750:
1. rangeBox search: 750 falls between 500 and 1000
2. Search map[2]
```

**Splitting.** `maxSize` is a **performance cutoff, not a hard limit**. A submap
is split only when both conditions hold:

```c
flexCount(*map) > m->elementsPerEntry &&   /* more than ONE row */
flexBytes(*map) > m->maxSize
```

A submap holding a single oversized row is never split, no matter how large it
grows. `flexMiddle()` of a one-row map is its head, so "splitting" it would move
the whole row to the high half and leave an empty low half whose rangeBox would
still reference the freed pre-split allocation. Both the insert path and
`multimapFullRegularizeMap()` refuse one-row splits for that reason.

A REPLACE of a key that already lives in an oversized submap also stays in that
submap rather than routing through the split path, otherwise two submaps could
end up holding the same key and range routing would pick the wrong one.

Right after a Small/Medium upgrade the new Full may hold submaps that already
exceed `maxSize`; that is deliberate. Pre-splitting during the upgrade was
measured as pure cost, so oversized submaps are split lazily on the next insert
that touches them.

**Merging.** When a delete leaves two adjacent submaps whose combined size fits
under `maxSize`, they are merged and one slot is reclaimed. A submap that
becomes completely empty is freed and its `map`/`middle`/`rangeBox` slots are
removed. `multimapReset()` collapses everything back to a single empty submap.

### Characteristics

**Advantages**

- Scales without bound; writes only touch one submap
- Deletes reclaim submaps instead of leaking empty slots
- O(log M) routing plus O(log n) in-submap search

**Disadvantages**

- Three `multiarray` allocations plus per-submap metadata
- Extra pointer chasing on every access
- Overkill for data that fits in one or two flexes

**No further upgrades**: Full is the final tier.

### Best Use Cases

1. Large in-memory tables and indexes
2. Analytics and time-series datasets
3. Anything expected to outgrow a few kilobytes

### Example

```c
multimap *big = multimapNewLimit(2, FLEX_CAP_LEVEL_2048);

for (int64_t i = 0; i < 100000; i++) {
    databox k = databoxNewSigned(i);
    databox v = databoxNewReal((double)i * 3.14);
    multimapInsert(&big, (const databox *[]){&k, &v});
}

/* Now Full; submap count depends on encoded row size vs maxSize (2048). */
```

### Range Box Details

`rangeBox[i]` holds the **head key of `map[i + 1]`**:

```
map[0]: [1, 5, 10, 15]
map[1]: [20, 25, 30]     <- rangeBox[0] = 20
map[2]: [50, 60, 70]     <- rangeBox[1] = 50
map[3]: [100, 150, 200]  <- rangeBox[2] = 100

To find key 65: rangeBox search puts 65 between 50 and 100 -> search map[2]
```

**Range boxes hold resolved key values, never references**, even when the
multimap is keyed by `multimapAtom` references. Comparison must work without a
reference container in hand.

Range boxes may own an allocation (a copy of the head key's bytes) or point
into the submap's flex. Any in-submap mutation that can realloc the flex -
replace, resize, field increment - refreshes the owning rangeBox afterwards, so
a stale box can never survive into the next range search.

Each range box is a 16-byte `databox`. A 96/128-bit head key points at a
16-byte heap payload the map owns (`big = 1`, `allocated = 1`):

- A refresh reuses that payload in place, so it allocates only when a head key
  changes from narrow to wide and frees only when it changes back. Refreshes
  run on every insert and delete that can change a submap's head, not only on
  splits and merges.
- Every removal frees it: deleting a submap (including `map[0]`, which drops
  `rangeBox[0]`), merging, trimming, `multimapReset()` and `multimapFree()`.
- The multiarray moves boxes as submaps are added, removed and regrouped, but
  the heap payload stays put, so reading a range box never writes to it.
  Concurrent readers of a map with wide boundary keys share the boxes
  read-only.
- `multimapCopy()` rebuilds every range box from the copied submaps' heads, so
  a copy never shares a payload.

## multimapAtom

### Purpose

`multimapAtom` is **not a multimap tier**. It is a string-interning and
reference-counting table built from two multimaps, letting callers store a small
integer reference wherever a repeated byte string would otherwise go.

**Header**: `multimapAtom.h` **Source**: `multimapAtom.c`

### Structure

```c
struct multimapAtom {
    multimap *mapAtomForward; /* 3-element rows: {ID, Key, Refcount} */
    multimap *mapAtomReverse; /* 1-element rows: IDs sorted by their Key */
    uint64_t highest;         /* next ID to hand out */
};
```

Both inner multimaps are created with `FLEX_CAP_LEVEL_512`.

`mapAtomReverse` is a self-sorting list: it stores only the atom ID, but is
inserted through `multimapInsertWithSurrogateKey()` so it sorts by the
_resolved_ string. That is what makes "find the ID for this string" a binary
search over the strings.

### API

```c
multimapAtom *multimapAtomNew(void);
void multimapAtomInit(multimapAtom *ma);
void multimapAtomFree(multimapAtom *ma);

void multimapAtomInsert(multimapAtom *ma, const databox *box);
void multimapAtomInsertConvert(multimapAtom *ma, databox *box);
bool multimapAtomInsertIfNewConvert(multimapAtom *ma, databox *key);
bool multimapAtomInsertIfNewConvertAndRetain(multimapAtom *ma, databox *key);
void multimapAtomInsertWithExactAtomID(multimapAtom *ma, uint64_t atomRef,
                                       const databox *key);

bool multimapAtomLookup(const multimapAtom *ma, const databox *ref,
                        databox *key);
void multimapAtomLookupConvert(const multimapAtom *ma, databox *box);
void multimapAtomLookupConvertBig(const multimapAtom *ma, databoxBig *box);
void multimapAtomLookupResult(const multimapAtom *ma, const databox *key,
                              multimapAtomResult *result);
void multimapAtomLookupRefcount(const multimapAtom *ma, const databox *key,
                                databox *count);
bool multimapAtomLookupReference(const multimapAtom *ma, const databox *key,
                                 databox *atomRef);
bool multimapAtomLookupMin(const multimapAtom *ma, databox *minRef);

void multimapAtomRetain(multimapAtom *ma, const databox *key);
void multimapAtomRetainByRef(multimapAtom *ma, const databox *foundRef);
void multimapAtomRetainById(multimapAtom *ma, uint64_t id);

bool multimapAtomRelease(multimapAtom *ma, const databox *key);
bool multimapAtomReleaseById(multimapAtom *ma, const databox *foundRef);
bool multimapAtomDelete(multimapAtom *ma, const databox *key);
bool multimapAtomDeleteByRef(multimapAtom *ma, const databox *ref);
bool multimapAtomDeleteById(multimapAtom *ma, uint64_t id);

size_t multimapAtomCount(const multimapAtom *ma);
size_t multimapAtomBytes(const multimapAtom *ma);
```

`multimapAtomResult` is `{databox val; uint64_t refcount;}`.

### 96/128-bit Atom Values

`multimapAtomLookupConvert()` copies the reference out before decoding, so the
box is never both the lookup key and the output. A plain `databox` has no room
for a wide value, so a 96/128-bit atom value comes back type-only
(`data.ptr == NULL`, `big = 0`); `multimapAtomLookupResult()` reports
`result->val` the same way. `multimapAtomLookupConvertBig()` resolves into a
`databoxBig`: it reads only the reference from the box, then sets `big = 1`
exactly for a wide value, which lives in `box->extra`. A reference that is not
in the table is left in the box. `multimapAtomLookup()` decodes into the
caller's box under the usual rule: arm a `databoxBig` to receive a wide value.

### String Interning

The `*Convert` calls rewrite the caller's `databox` in place into a
`DATABOX_CONTAINER_REFERENCE_EXTERNAL` box whose `data.u` is the atom ID. The
whole box is replaced: the `big` and `allocated` bits of the value it held
(a 96/128-bit value's, for one) are cleared, and nothing it owned is freed.

```c
multimapAtom *atoms = multimapAtomNew();

databox str1 = databoxNewBytesString("hello");
multimapAtomInsertConvert(atoms, &str1);
/* str1 is now {type: CONTAINER_REFERENCE_EXTERNAL, data.u: 0} */

databox str2 = databoxNewBytesString("hello");
bool existed = multimapAtomInsertIfNewConvert(atoms, &str2);
/* existed == true; str2 becomes the same reference, data.u == 0 */

databox str3 = databoxNewBytesString("world");
multimapAtomInsertConvert(atoms, &str3);
/* str3 -> data.u == 1 */
```

`multimapAtomInsertIfNewConvert()` returns **true when the string was already
interned** and false when it created a new entry. It also sets the box's
`created` flag to match.

### Reference Counting

Refcounts are stored 0-based so that a single-reference entry costs one byte:
internal 0 means one outstanding reference, internal 1 means two, and so on.
`multimapAtomLookupRefcount()` / `multimapAtomLookupResult()` report the
adjusted, human-facing count.

```c
multimapAtomRetain(atoms, &helloKey);   /* one more reference */

bool gone = multimapAtomRelease(atoms, &helloKey);
/* Decrements; when the internal counter goes below zero the last reference
 * is gone and the entry is deleted from both inner multimaps. */
```

Releasing a string that is not in the table is a programming error and asserts.

> **ID reuse contract**: when the table drains to empty, the ID counter resets
> to 0. Any reference handed out before that point may resolve to an unrelated
> later entry. Do not cache bare atom IDs across a boundary where the table can
> empty and refill; re-resolve them instead.

### Using an Atom Container With a multimap

The `*WithReference` and `*WithSurrogateKey` multimap entry points take the atom
container so comparisons resolve references to the underlying strings.

```c
multimapAtom *atoms = multimapAtomNew();
multimap *comments = multimapNew(3);   /* id | authorRef | text */

for (int64_t i = 0; i < 1000; i++) {
    databox id = databoxNewSigned(i);

    databox author = databoxNewBytesString(i % 10 == 0   ? "alice"
                                           : i % 10 == 1 ? "bob"
                                                         : "charlie");
    multimapAtomInsertIfNewConvert(atoms, &author);
    /* 'author' is now a small integer reference */

    databox text = databoxNewBytesString("Great post!");
    multimapInsert(&comments, (const databox *[]){&id, &author, &text});
}

/* "alice", "bob" and "charlie" are stored once each in 'atoms';
 * 'comments' stores references instead of repeated strings. */

multimapFree(comments);
multimapAtomFree(atoms);
```

## Tier Comparison

### Container Overhead

For 1000 rows of 2 elements averaging 20 bytes per element (about 40,000 bytes
of flex payload):

```
Small:   16 + 40,000                         (container is noise)
Medium:  28 + 40,000                         (container is noise)
Full:    48 + N * (8 + 4 + 16) + 40,000      (plus multiarray overhead)
```

With a 2048-byte `maxSize` that payload lands in roughly 20 submaps, so Full's
metadata is on the order of 600 bytes - still about 1.5% - while buying
per-submap writes instead of whole-structure memmoves.

`multimapBytes()` reports only the flex payload; none of the container or
metadata figures above are included.

### Where the Time Goes

Rather than absolute timings, which depend entirely on key encoding and machine:

| Cost                  | Small                  | Medium              | Full                     |
| --------------------- | ---------------------- | ------------------- | ------------------------ |
| Routing to a submap   | none                   | 1 comparison        | O(log M) binary search   |
| Search within submap  | O(log n) over all rows | O(log n) over ~half | O(log n) over one submap |
| Bytes moved on insert | whole flex             | ~half               | one submap (<= maxSize)  |
| Iteration             | 1 array                | 2 arrays            | N arrays                 |

Small wins on read-mostly data that fits in one flex. Full wins as soon as
insert-time memmove dominates, because its move cost is bounded by `maxSize`
rather than by total size.

### When to Use Each Tier

```
       Small          Medium          Full
         │              │              │
    0 ───┴──── limit ───┴── limit*3 ───┴──── ∞
                (bytes, not rows)

         ← Manual selection not needed →
```

The thresholds are **byte** thresholds. Row count only matters through the
guards that guarantee a split can produce two non-empty halves.

**Influencing the tier** (rare):

```c
/* Stay Small for a tiny embedded map */
multimap *tiny = multimapNewLimit(2, FLEX_CAP_LEVEL_128);

/* Reach Full sooner, with small submaps, for insert-heavy workloads */
multimap *big = multimapNewLimit(2, FLEX_CAP_LEVEL_512);
```

## Transition Mechanics

### Small -> Medium

```c
/* Before */
multimapSmall {
    map    = [K1,V1, K2,V2, K3,V3, K4,V4]
    middle = byte offset of K3
}

/* Trigger: bytes > limit && rows > elementsPerEntry * 2 */

/* zrealloc() the 16-byte container into 28 bytes, then
 * flexSplitMiddle() at the cached middle: */
multimapMedium {
    map[0] = [K1,V1, K2,V2]
    map[1] = [K3,V3, K4,V4]
    middle[0], middle[1] recomputed by full rescan
}
```

### Medium -> Full

```c
/* Before */
multimapMedium {
    map[0] = [...]   map[1] = [...]     /* both non-empty */
}

/* Trigger: bytes > limit * 3 && both submaps non-empty */

/* After */
multimapFull {
    count = 2
    map[0], map[1] adopted as-is (may exceed maxSize; split lazily)
    middle[0], middle[1] carried over
    rangeBox[0] = head key of map[1]
    maxSize = flexOptimizationSizeLimit[limit]
}
```

Because the container struct is reallocated, the caller's pointer changes -
which is exactly why every mutating API takes `multimap **`.

### Transition Transparency

```c
multimap *m = multimapNew(2);

for (int64_t i = 0; i < 10000; i++) {
    databox k = databoxNewSigned(i);
    databox v = databoxNewSigned(i);
    multimapInsert(&m, (const databox *[]){&k, &v});
    /* 'm' may be rewritten to a different tier on any iteration;
     * the call site never changes. */
}
```

## Design Philosophy

The multimap family follows **progressive enhancement**:

1. **Start minimal**: 16-byte Small for tiny datasets
2. **Grow gracefully**: 28-byte Medium halves write cost
3. **Scale without bound**: Full bounds write cost by `maxSize`
4. **Stay transparent**: one API across all three tiers

This optimizes the common case (small data) while supporting the exceptional
case (very large data) without asking callers to predict their final size.

## See Also

- [MULTIMAP.md](MULTIMAP.md) - Main API documentation
- [EXAMPLES.md](EXAMPLES.md) - Worked multimap programs
- [flex](../flex/FLEX.md) - Underlying sorted byte-packed storage
- [multiarray](../multiarray/MULTIARRAY.md) - Dynamic arrays behind Full
