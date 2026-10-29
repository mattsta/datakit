# multilru - Multi-Level LRU Cache

## Overview

`multilru` is a **segmented (S4LRU-style) LRU cache index** that organizes
entries into access-frequency tiers. Instead of one list ordered by access
time, multilru maintains N levels: new entries start at level 0, each hit
promotes an entry one level up, and eviction pressure demotes entries one
level down before anything is truly removed. That second-chance ladder is
what makes the cache scan-resistant.

multilru stores **no data of its own**. It hands out an integer handle
(`multilruPtr`) per entry; you keep the actual payload in your own structure
keyed by that handle.

**Key Features:**

- Configurable level count (default 7, hard cap 64)
- O(1) insert, promote, delete, and remove-minimum
- Adaptive entry width: 5 to 16 bytes per entry, chosen by cache scale
- Optional per-entry weights for size-aware eviction
- Policy-driven auto-eviction with an eviction callback
- Intrusive O(1) free list plus a fresh-slot high water mark
- Lifetime statistics snapshot in O(1)
- Bulk eviction helpers and coldest/hottest sampling

**Headers**: `multilru.h`

**Source**: `multilru.c`

## Architecture

```
Access Frequency Levels (7 levels by default):

Level 6 (hottest)  ------>  [MRU] <- entries <- [LRU]
Level 5            ------>  [MRU] <- entries <- [LRU]
...
Level 0 (coldest)  ------>  [MRU] <- entries <- [LRU] --> EVICT

- New entries enter at level 0
- multilruIncrease() promotes an entry to level+1 (capped at maxLevels-1)
- multilruRemoveMinimum() demotes the coldest entry to level-1,
  or truly evicts it when it is already at level 0
```

Index 0 is reserved as the invalid handle. Indices `1 .. maxLevels` are the
per-level head markers (sentinels), so real entries always start at index
`maxLevels + 1`.

A `levelMask` bitmap records which levels currently hold entries, which is
how "find the coldest entry" stays O(1) no matter how many levels are
configured.

### Adaptive Entry Width

Entry metadata is bit-packed, and the packing width grows with the cache:

| Width (bytes) | Address bits | Max entries | Migration cost from previous tier |
| ------------- | ------------ | ----------- | --------------------------------- |
| 5             | 16           | 64K         | -                                 |
| 6             | 20           | 1M          | 64K x 1B = 64KB                   |
| 7             | 24           | 16M         | 1M x 1B = 1MB                     |
| 8             | 28           | 256M        | 16M x 1B = 16MB                   |
| 9             | 32           | 4B          | 256M x 1B = 256MB                 |
| 10            | 36           | 64B         | 4B x 1B = 4GB                     |
| 11            | 40           | 1T          | 64B x 1B = 64GB                   |
| 12            | 44           | 16T         | 1T x 1B = 1TB                     |
| 16            | 60           | 1E          | 16T x 4B = 64TB                   |

Every entry, at every width, carries the same four fields:

```
Bits 0 .. 2N-1:   prev (N bits), next (N bits)
Bits 2N .. 2N+5:  level (6 bits, so at most 64 levels)
Bit 2N+6:         isPopulated
Bit 2N+7:         isHead (level marker)
```

Width **only ever upgrades**. There is no downgrade path today, so a cache
that grew to 100M entries and then shed 99% of them keeps its 8-byte entries
until it is freed and rebuilt.

## Data Structures

`multilru` is **opaque** — `multilru.h` exposes only
`typedef struct multilru multilru;`. Everything below is part of the public
API surface.

### Handle Type

```c
typedef size_t multilruPtr;  /* 0 == invalid/null handle */
```

Handle lifecycle:

- **Stable**: once assigned, a handle refers to that entry until the entry is
  deleted or truly evicted.
- **Recycled**: after delete/eviction the handle goes on a free list and is
  reused LIFO, so the most recently freed handle is the next one issued.
- **Dense-ish**: allocation prefers recycled handles, then fresh sequential
  ones, so handle values stay compact.

If you store handles externally (a hash map, an array), you **must** drop the
stale mapping when an entry is evicted. A recycled handle silently refers to a
different entry.

### Configuration

```c
typedef enum multilruPolicy {
    MLRU_POLICY_COUNT = 0, /* Evict when count > maxCount */
    MLRU_POLICY_SIZE,      /* Evict when totalWeight > maxWeight */
    MLRU_POLICY_HYBRID,    /* Evict when either limit is exceeded */
} multilruPolicy;

typedef enum multilruEvictStrategy {
    MLRU_EVICT_LRU = 0,       /* The oldest entry of the lowest level */
    MLRU_EVICT_SIZE_WEIGHTED, /* The heaviest of its 8 oldest */
    MLRU_EVICT_SIZE_LRU,      /* Over maxWeight: the oldest of those 8 that
                                 covers the excess, else the heaviest */
} multilruEvictStrategy;

typedef enum multilruEntryWidth {
    MLRU_WIDTH_5 = 5,  MLRU_WIDTH_6 = 6,  MLRU_WIDTH_7 = 7,
    MLRU_WIDTH_8 = 8,  MLRU_WIDTH_9 = 9,  MLRU_WIDTH_10 = 10,
    MLRU_WIDTH_11 = 11, MLRU_WIDTH_12 = 12, MLRU_WIDTH_16 = 16,
} multilruEntryWidth;

typedef struct multilruConfig {
    size_t maxLevels;      /* Number of LRU levels (default 7, capped at 64) */
    size_t startCapacity;  /* Initial entry capacity (0 = auto) */
    uint64_t maxWeight;    /* Max total weight in bytes (0 = unlimited) */
    uint64_t maxCount;     /* Max entries (0 = unlimited) */
    multilruPolicy policy;
    multilruEvictStrategy evictStrategy;
    bool enableWeights;    /* Allocate the weight array */
} multilruConfig;
```

Weight tracking is allocated when **any** of these is true: `enableWeights`,
`policy` is `MLRU_POLICY_SIZE` or `MLRU_POLICY_HYBRID`, or `evictStrategy` is
`MLRU_EVICT_SIZE_WEIGHTED` or `MLRU_EVICT_SIZE_LRU`. You do not have to set
`enableWeights` by hand when the policy already implies it.

The strategy picks the entry each eviction step takes (`multilruPeekMinimum`
names it; at level > 0 the step demotes it, at level 0 it evicts it). The
size-aware strategies look only at the 8 oldest entries of the lowest
populated level, so a step stays O(1):

| Strategy                   | Takes                                                                                                               |
| -------------------------- | ------------------------------------------------------------------------------------------------------------------- |
| `MLRU_EVICT_LRU`           | the oldest entry of the lowest level                                                                                |
| `MLRU_EVICT_SIZE_WEIGHTED` | the heaviest of the 8 (the oldest of equals): the most weight freed per step                                        |
| `MLRU_EVICT_SIZE_LRU`      | over `maxWeight`, the oldest of the 8 whose weight alone covers the excess, else the heaviest; otherwise the oldest |

Without weights (a strategy set at runtime on a cache created without them)
both size-aware strategies take the oldest entry.

### Statistics

```c
typedef struct multilruStats {
    /* Current state */
    uint64_t totalWeight;
    size_t count;
    size_t capacity;
    size_t bytesUsed;

    /* Slot allocation state */
    uint64_t nextFresh; /* Next never-used slot index (high water mark) */
    uint64_t freeCount; /* Recycled slots waiting in the free list */

    /* Lifetime counters */
    uint64_t inserts;
    uint64_t evictions;  /* True evictions from level 0 */
    uint64_t demotions;  /* Level N -> N-1 */
    uint64_t promotions; /* Level N -> N+1 via multilruIncrease() */
    uint64_t deletes;    /* Direct multilruDelete() calls */

    /* Configuration snapshot */
    size_t maxLevels;
    uint64_t maxWeight;
    uint64_t maxCount;
    uint8_t entryWidth;
    bool autoEvict;
} multilruStats;

void multilruGetStats(const multilru *mlru, multilruStats *stats);
```

The snapshot is O(1) — every counter is maintained incrementally.

## Creation Functions

```c
/* Default: 7 levels, count policy, no limits, no weights */
multilru *multilruNew(void);

/* Specific level count (values above 64 are clamped to 64; 0 becomes 7) */
multilru *multilruNewWithLevels(size_t maxLevels);

/* Levels plus pre-allocated capacity */
multilru *multilruNewWithLevelsCapacity(size_t maxLevels, size_t startCapacity);

/* Full control */
multilru *multilruNewWithConfig(const multilruConfig *config);

/* Free all resources. Safe with NULL. */
void multilruFree(multilru *mlru);

/* Example: default cache */
multilru *mlru = multilruNew();

/* Example: 5 levels, room for 10000 entries up front */
multilru *custom = multilruNewWithLevelsCapacity(5, 10000);
```

`startCapacity` is a hint: the implementation adds `maxLevels + 1` for the
sentinels and rounds the byte size up through `jebufSizeAllocation()`. A
`startCapacity` of 0 starts at 256 slots.

## Insertion

```c
/* Insert a new entry at level 0.
 * Returns a non-zero handle, or 0 on failure. */
multilruPtr multilruInsert(multilru *mlru);

/* Insert with an associated weight (requires weight tracking) */
multilruPtr multilruInsertWeighted(multilru *mlru, uint64_t weight);

/* Example: track 1000 cache entries */
multilruPtr cacheKeys[1000];
for (uint64_t i = 0; i < 1000; i++) {
    cacheKeys[i] = multilruInsert(mlru);
    /* Store cacheKeys[i] alongside the actual cached data */
}
```

**How it works**:

1. Pop a recycled slot from the free list; if the list is empty, take the
   next fresh index, growing the entry array first if needed.
2. Record the weight (when weight tracking is on) and add it to `totalWeight`.
3. Link the entry at the head of level 0 and bump `count`.
4. Run policy enforcement, which may auto-evict.

### Insert Can Return 0 After a Successful Allocation

Insert returns 0 in **two** distinct situations:

- Allocation failed and no slot could be obtained.
- The entry was inserted, but the policy pass that runs immediately after
  evicted it again. A weighted insert larger than the remaining budget can
  cascade demotions all the way back to the entry that was just added.

The second case is deliberate: returning the handle would hand the caller an
already-freed slot that the next insert could reissue to a different owner.
Treat `0` as "did not fit" and do not store it.

```c
multilruPtr p = multilruInsertWeighted(cache, hugeObjectBytes);
if (p == 0) {
    /* Object never fit under maxWeight - do not cache it */
    return false;
}
```

## Access Tracking

```c
/* Promote entry one level (call on a cache hit) */
void multilruIncrease(multilru *mlru, multilruPtr ptr);

/* Change an entry's weight in place (no-op without weight tracking) */
void multilruUpdateWeight(multilru *mlru, multilruPtr ptr, uint64_t newWeight);

/* Example: record a cache hit */
void cacheHit(multilru *mlru, multilruPtr ptr) {
    multilruIncrease(mlru, ptr);
}

/* Example: build an access pattern */
multilruPtr p1 = multilruInsert(mlru);
multilruPtr p2 = multilruInsert(mlru);
multilruPtr p3 = multilruInsert(mlru);

multilruIncrease(mlru, p1);   /* p1 -> level 1 */
multilruIncrease(mlru, p1);   /* p1 -> level 2 */
multilruIncrease(mlru, p2);   /* p2 -> level 1 */
/* p3 is still at level 0 and is the eviction candidate */
```

**Level promotion**:

- Target is `currentLevel + 1`, clamped to `maxLevels - 1`.
- A call that does not actually move up (already at the top level) still
  refreshes recency within that level, but does not count as a promotion in
  the statistics.
- Invalid, out-of-range, unpopulated, and head-marker handles are silently
  ignored.

## Eviction

### Remove the Coldest Entry (S4LRU semantics)

```c
bool multilruRemoveMinimum(multilru *mlru, multilruPtr *out);
```

This is **not** an unconditional removal. Given the current coldest entry:

- If it sits at level > 0, it is **demoted** to `level - 1` and given a second
  chance. `*out` receives its handle and the handle is still valid.
- If it sits at level 0, it is **truly evicted**: the eviction callback fires
  (before the slot is released), the slot returns to the free list, `count`
  drops, and the handle becomes invalid.
- Returns `false` only when the cache is empty.

Distinguish the two outcomes with `multilruIsPopulated()`:

```c
multilruPtr victim;
if (multilruRemoveMinimum(mlru, &victim)) {
    if (multilruIsPopulated(mlru, victim)) {
        /* Demoted - the entry is still live. Do NOT free your payload. */
    } else {
        /* Truly evicted - release the payload you stored for 'victim'. */
        freeData(victim);
    }
}
```

Because a single call may only demote, a naive "evict one entry when full"
loop can spin without ever freeing anything. Use `multilruEvictN()` instead,
which keeps calling `RemoveMinimum` until it accumulates real evictions.

### Bulk Eviction

```c
/* Evict up to n entries; out[] receives only TRUE evictions.
 * Returns the number of true evictions performed. */
size_t multilruEvictN(multilru *mlru, multilruPtr out[], size_t n);

/* Evict until totalWeight <= targetWeight, or maxN evictions, whichever
 * comes first. Returns 0 immediately when weight tracking is disabled. */
size_t multilruEvictToSize(multilru *mlru, uint64_t targetWeight,
                           multilruPtr out[], size_t maxN);

/* Example: make room for 100 entries in one batch */
multilruPtr evicted[100];
const size_t n = multilruEvictN(mlru, evicted, 100);
for (size_t i = 0; i < n; i++) {
    freeData(evicted[i]);
}
```

Both helpers absorb demotions internally, so every handle written into `out[]`
is a genuinely evicted, now-invalid handle.

### Delete a Specific Entry

```c
/* Immediate removal - bypasses the demotion ladder entirely */
void multilruDelete(multilru *mlru, multilruPtr ptr);
```

`multilruDelete` is counted as a delete, not an eviction, and does **not**
invoke the eviction callback. Invalid handles are ignored.

**Delete vs RemoveMinimum**:

|                   | `multilruDelete`    | `multilruRemoveMinimum`    |
| ----------------- | ------------------- | -------------------------- |
| Target            | Any handle you name | The current coldest entry  |
| Demotion          | Never               | Yes, when level > 0        |
| Eviction callback | Not fired           | Fired on true eviction     |
| Statistic         | `deletes`           | `demotions` or `evictions` |

### Logging Before Acting

A writer that logs each change before making it (the persistence wrapper
does) needs to know what a change will do:

```c
/* What multilruRemoveMinimum takes next (0: empty): demoted when its
 * level is above 0, evicted at level 0 */
multilruPtr multilruPeekMinimum(const multilru *mlru);

/* The handle the next insert returns: the most recently freed slot, else
 * the next new one */
multilruPtr multilruPeekNextHandle(const multilru *mlru);

/* One demotion of 'ptr', exactly as RemoveMinimum demotes (no-op at level
 * 0): for replaying a logged demotion */
void multilruDecrease(multilru *mlru, multilruPtr ptr);

/* The freed slots in the order inserts reuse them (up to 'n'): a snapshot
 * that keeps this order restores to the same future handles */
size_t multilruFreeHandles(const multilru *mlru, multilruPtr out[], size_t n);
```

## Query Operations

```c
size_t multilruCount(const multilru *mlru);       /* Active entries */
size_t multilruBytes(const multilru *mlru);       /* Total structure bytes */
uint64_t multilruTotalWeight(const multilru *mlru);

size_t multilruLevelCount(const multilru *mlru, size_t level);
uint64_t multilruLevelWeight(const multilru *mlru, size_t level);

uint64_t multilruGetWeight(const multilru *mlru, multilruPtr ptr);
size_t multilruGetLevel(const multilru *mlru, multilruPtr ptr);
bool multilruIsPopulated(const multilru *mlru, multilruPtr ptr);

/* Example */
printf("LRU has %zu/%zu entries using %zu bytes\n",
       multilruCount(mlru), multilruCapacity(mlru), multilruBytes(mlru));
```

Out-of-range levels return 0 from `multilruLevelCount` and
`multilruLevelWeight`; invalid handles return 0 from `multilruGetWeight` and
`multilruGetLevel`. Because level 0 is a legitimate answer, use
`multilruIsPopulated()` when you need to tell "level 0" from "not a handle".

`multilruBytes()` accounts for the `multilru` struct, `capacity * entryWidth`
for the entry array, `maxLevels * sizeof(multilruLevel)` for the level table,
and `capacity * 8` more when weights are allocated.

### Sampling Cold and Hot Entries

```c
size_t multilruCollectLowest(multilru *mlru, multilruPtr out[], size_t n);
size_t multilruCollectHighest(multilru *mlru, multilruPtr out[], size_t n);
void multilruGetNLowest(multilru *mlru, multilruPtr out[], size_t n);
void multilruGetNHighest(multilru *mlru, multilruPtr out[], size_t n);

/* Example: inspect up to 10 eviction candidates */
multilruPtr candidates[10];
const size_t got = multilruCollectLowest(mlru, candidates, 10);

for (size_t i = 0; i < got; i++) {
    printf("candidate %zu: entry %zu\n", i, candidates[i]);
}
```

The `Collect` forms return how many entries they wrote and leave the slots
past that count untouched. The `GetN` forms fill all `n` slots: the entries
found, then `0` (never a live entry) in every slot past the last one, so a
`0` marks the end when the cache holds fewer than `n` entries.

`CollectLowest`/`GetNLowest` walk levels upward from 0, oldest-first within
each level. `CollectHighest`/`GetNHighest` walk levels downward from the top,
newest-first.

## Runtime Configuration

Limits can change after creation.

```c
void multilruSetPolicy(multilru *mlru, multilruPolicy policy);
void multilruSetEvictStrategy(multilru *mlru, multilruEvictStrategy strategy);
multilruPolicy multilruGetPolicy(const multilru *mlru);
multilruEvictStrategy multilruGetEvictStrategy(const multilru *mlru);

void multilruSetMaxCount(multilru *mlru, uint64_t maxCount);
uint64_t multilruGetMaxCount(const multilru *mlru);

void multilruSetMaxWeight(multilru *mlru, uint64_t maxWeight);
uint64_t multilruGetMaxWeight(const multilru *mlru);
```

**Expanding** a limit takes effect immediately and does nothing else.

**Shrinking** a limit does **not** trigger an immediate eviction sweep, so a
large shrink cannot stall the caller. Drain gradually instead:

```c
multilruSetMaxCount(cache, 5000);   /* was 10000 */

while (multilruNeedsEviction(cache)) {
    multilruPtr evicted[100];
    const size_t n = multilruEvictN(cache, evicted, 100);
    if (n == 0) {
        break;
    }

    for (size_t i = 0; i < n; i++) {
        cleanupExternalData(evicted[i]);
    }

    maybeYield();  /* return to the event loop between batches */
}
```

## Eviction Control

```c
/* Auto-evict on insert when over limits (default: true) */
void multilruSetAutoEvict(multilru *mlru, bool autoEvict);
bool multilruGetAutoEvict(const multilru *mlru);

/* Called with the entry handle BEFORE the slot is released.
 * Pass NULL to disable. */
void multilruSetEvictCallback(multilru *mlru,
                              void (*callback)(size_t evictedPtr,
                                               void *userData),
                              void *userData);

/* True when the cache currently exceeds its configured limits */
bool multilruNeedsEviction(const multilru *mlru);
```

### Workflow 1: Automatic Eviction with Callback (default)

```c
static void onEvict(size_t ptr, void *data) {
    MyHashMap *map = data;
    myHashMapDeleteByLruPtr(map, ptr);  /* drop the stale handle mapping */
}

multilru *cache = multilruNewWithConfig(&config);
multilruSetEvictCallback(cache, onEvict, myHashMap);
/* Inserts now auto-evict and notify through the callback */
```

The callback runs only for **true** evictions from level 0, never for
demotions and never for `multilruDelete()`.

### Workflow 2: Manual Eviction

```c
multilru *cache = multilruNewWithConfig(&config);
multilruSetAutoEvict(cache, false);  /* cache may now exceed its limits */

for (...) {
    multilruInsert(cache);
}

/* Drain on your own schedule */
while (multilruNeedsEviction(cache)) {
    multilruPtr evicted[64];
    const size_t n = multilruEvictN(cache, evicted, 64);
    if (n == 0) {
        break;
    }
    for (size_t i = 0; i < n; i++) {
        myHashMapDeleteByLruPtr(map, evicted[i]);
    }
}
```

Turning auto-eviction off also removes the "insert may return 0 because the
new entry was evicted" case, since no policy pass runs on insert.

## Introspection

```c
size_t multilruMaxLevels(const multilru *mlru);
multilruEntryWidth multilruGetEntryWidth(const multilru *mlru);
size_t multilruCapacity(const multilru *mlru);
bool multilruHasWeights(const multilru *mlru);
```

## Slot Allocation and Fragmentation

Slots come from two sources:

1. **Recycled slots** — an intrusive free list threaded through the `next`
   field of freed entries. Push and pop are both O(1) with no side array.
2. **Fresh slots** — `nextFresh`, a monotonically increasing high water mark
   that starts at `maxLevels + 1`.

Insert prefers recycled slots; delete and eviction push slots back onto the
free list. The invariant is:

```
count + freeCount == nextFresh - (maxLevels + 1)
```

Fragmentation ratio is `freeCount / (nextFresh - maxLevels - 1)`:

| Observation                     | Interpretation                       |
| ------------------------------- | ------------------------------------ |
| `nextFresh=1000, freeCount=0`   | Dense: 1000 slots, all in use        |
| `nextFresh=1000, freeCount=500` | Fragmented: 500 active, 500 holes    |
| `nextFresh=1000, freeCount=900` | Very sparse: only 100 active entries |

High fragmentation after heavy churn is normal and harmless — holes are
reused before any fresh slot is taken — but the arrays keep their high-water
size until a repack.

### Repacking

```c
typedef void multilruMovedFn(multilruPtr from, multilruPtr to, void *ctx);
size_t multilruRepack(multilru *mlru, multilruMovedFn *moved, void *ctx);
```

`multilruRepack` moves the last live entry into the first hole until no hole
is left below an entry, then shrinks the entry and weight arrays to the size
class that holds them, at the narrowest entry width their handles fit. Recency
order, levels, weights, limits and counters are unchanged; `nextFresh` becomes
`maxLevels + 1 + count` and the free list is empty. It runs in O(slots used)
and returns how many entries moved.

A moved entry's handle changes. `moved(from, to, ctx)` is called once per
move so a caller can rename what it stored (it must not call into the
multilru being repacked); pass `NULL` only when the caller keeps no handles.

```c
static void renameHandle(multilruPtr from, multilruPtr to, void *ctx) {
    MyHashMap *map = ctx;
    myHashMapRenameLruPtr(map, from, to);
}

multilruStats s;
multilruGetStats(cache, &s);
if (s.freeCount > s.count) { /* more holes than entries */
    multilruRepack(cache, renameHandle, myHashMap);
}
```

A persisted cache repacks through `multilruRepackP`, which logs the repack
so replay moves the same entries to the same handles.

## Performance Characteristics

### Time Complexity

| Operation         | Complexity       | Notes                                          |
| ----------------- | ---------------- | ---------------------------------------------- |
| Insert            | O(1) amortized   | Array growth and width migration amortize      |
| InsertWeighted    | O(1) amortized   | Plus the policy pass, which may evict          |
| Increase          | O(1)             | Unlink and relink, `levelMask` update          |
| UpdateWeight      | O(1)             | Adjusts entry, level, and total weight         |
| RemoveMinimum     | O(1)             | `levelMask` gives the coldest level directly   |
| Delete            | O(1)             | Direct index access                            |
| EvictN            | O(n + demotions) | Demotions are bounded by `maxLevels` per entry |
| Count / Bytes     | O(1)             | Cached values                                  |
| GetStats          | O(1)             | Counters maintained incrementally              |
| LevelCount/Weight | O(1)             | Per-level totals are maintained                |
| Collect/GetN      | O(n + levels)    | Walks the level lists                          |
| Width migration   | O(capacity)      | One-time cost per tier boundary                |
| Repack            | O(slots used)    | Moves entries down, shrinks the arrays         |

### Space Complexity

**Per entry** (LRU metadata only, excluding your payload):

- Without weights: 5 to 16 bytes, chosen by scale
- With weights: 13 to 24 bytes (8 bytes added per entry)

**Per cache**: the `multilru` struct, plus one `multilruLevel` per level
(head, tail, count, and weight as four `uint64_t` fields, so 32 bytes each),
plus `capacity * entryWidth` for the entry array, plus `capacity * 8` more
when weights are allocated.

**Example**: 10,000 entries, 7 levels, no weights, width 5

- Entries: 10,000 x 5 = 50,000 bytes
- Level table and struct: a few hundred bytes
- **Total**: roughly 50 KB

### Growth Behavior

The entry array grows through `jebufSizeAllocation()`, which rounds the
requested byte count up to an allocator size class, minimizing fragmentation.
When the new capacity would exceed what the current entry width can address,
the width upgrades to the next tier and all existing entries are re-packed.

## Common Patterns

### Pattern 1: Cache with External Payloads

Handles index directly into the entry array, so a side array indexed by
handle is the natural payload store. Size it from `multilruCapacity()`, not
from your entry limit — capacity is rounded up by the allocator and can grow.

```c
#define MAX_CACHE_ENTRIES 1000

typedef struct cache {
    multilru *lru;
    void **data;    /* indexed by multilruPtr */
    size_t dataLen; /* tracks multilruCapacity(lru) */
} cache;

static void onEvict(size_t ptr, void *userData) {
    cache *c = userData;
    if (ptr < c->dataLen) {
        free(c->data[ptr]);
        c->data[ptr] = NULL;
    }
}

static void cacheSyncCapacity(cache *c) {
    const size_t capacity = multilruCapacity(c->lru);
    if (capacity > c->dataLen) {
        c->data = zrealloc(c->data, capacity * sizeof(*c->data));
        memset(c->data + c->dataLen, 0,
               (capacity - c->dataLen) * sizeof(*c->data));
        c->dataLen = capacity;
    }
}

cache *cacheNew(void) {
    cache *c = zcalloc(1, sizeof(*c));

    multilruConfig config = {
        .maxLevels = 7,
        .startCapacity = MAX_CACHE_ENTRIES,
        .policy = MLRU_POLICY_COUNT,
        .maxCount = MAX_CACHE_ENTRIES,
    };

    c->lru = multilruNewWithConfig(&config);
    multilruSetEvictCallback(c->lru, onEvict, c);
    cacheSyncCapacity(c);
    return c;
}

void *cacheGet(cache *c, multilruPtr ptr) {
    if (!multilruIsPopulated(c->lru, ptr)) {
        return NULL;  /* stale handle: entry was evicted */
    }

    multilruIncrease(c->lru, ptr);
    return c->data[ptr];
}

multilruPtr cachePut(cache *c, void *value) {
    /* Auto-eviction keeps the cache at maxCount and calls onEvict for us */
    const multilruPtr ptr = multilruInsert(c->lru);
    if (ptr == 0) {
        return 0;  /* did not fit */
    }

    cacheSyncCapacity(c);  /* the insert may have grown the entry array */
    c->data[ptr] = value;
    return ptr;
}
```

Two ordering details matter here:

- `onEvict` fires **before** the slot is released, so `c->data[ptr]` still
  refers to the payload being retired.
- `onEvict` can fire from **inside** `multilruInsert`, so the payload array
  must already cover the evicted handle. That is why `cacheSyncCapacity()`
  runs at construction and after every insert, never lazily on first write.

### Pattern 2: Weighted Cache with a Byte Budget

```c
multilruConfig config = {
    .maxLevels = 7,
    .policy = MLRU_POLICY_SIZE,
    .maxWeight = 15ULL * 1024 * 1024 * 1024,  /* 15GB */
    .evictStrategy = MLRU_EVICT_SIZE_WEIGHTED,
};

multilru *cache = multilruNewWithConfig(&config);
/* enableWeights is implied by MLRU_POLICY_SIZE */

const multilruPtr bigVideo =
    multilruInsertWeighted(cache, 12ULL * 1024 * 1024 * 1024);
const multilruPtr smallVideo =
    multilruInsertWeighted(cache, 100ULL * 1024 * 1024);

/* An object larger than maxWeight can never be admitted */
if (multilruInsertWeighted(cache, 20ULL * 1024 * 1024 * 1024) == 0) {
    /* expected: 20GB does not fit in a 15GB budget */
}

/* Re-encode changed the object size */
multilruUpdateWeight(cache, smallVideo, 150ULL * 1024 * 1024);

printf("using %" PRIu64 " of %" PRIu64 " bytes\n",
       multilruTotalWeight(cache), multilruGetMaxWeight(cache));
```

### Pattern 3: Level Distribution Report

Entry levels are queryable directly — no shadow array required.

```c
void reportDistribution(multilru *mlru) {
    const size_t levels = multilruMaxLevels(mlru);
    const size_t total = multilruCount(mlru);

    printf("Access Distribution:\n");
    for (size_t i = 0; i < levels; i++) {
        const size_t atLevel = multilruLevelCount(mlru, i);
        printf("  Level %zu: %zu entries (%.1f%%), weight %" PRIu64 "\n",
               i, atLevel,
               total ? 100.0 * (double)atLevel / (double)total : 0.0,
               multilruLevelWeight(mlru, i));
    }
}
```

### Pattern 4: Health Monitoring

```c
void logCacheHealth(multilru *mlru) {
    multilruStats s;
    multilruGetStats(mlru, &s);

    const uint64_t slots = s.nextFresh > s.maxLevels + 1
                               ? s.nextFresh - s.maxLevels - 1
                               : 0;
    const double fragmentation =
        slots ? (double)s.freeCount / (double)slots : 0.0;

    printf("entries=%zu bytes=%zu width=%" PRIu8 "\n",
           s.count, s.bytesUsed, s.entryWidth);
    printf("inserts=%" PRIu64 " evictions=%" PRIu64 " demotions=%" PRIu64
           " promotions=%" PRIu64 " deletes=%" PRIu64 "\n",
           s.inserts, s.evictions, s.demotions, s.promotions, s.deletes);
    printf("fragmentation=%.2f autoEvict=%d\n", fragmentation, s.autoEvict);
}
```

A promotions-to-inserts ratio near zero means the workload is a scan and the
level ladder is not buying you anything; consider fewer levels.

## Best Practices

### 1. Choose a Level Count That Matches the Workload

```c
/* Few levels (3-5): simple hot/warm/cold distinction */
multilru *simple = multilruNewWithLevels(3);

/* Default (7): good balance for most uses */
multilru *balanced = multilruNew();

/* Many levels (16+): strong protection for adversarial workloads */
multilru *hardened = multilruNewWithLevels(16);
```

**Trade-offs**:

- More levels give better frequency discrimination and scan resistance.
- More levels cost one `multilruLevel` record each and one reserved sentinel
  slot each.
- More levels mean more `multilruIncrease()` calls before an entry reaches the
  top, and more demotion steps before it is finally evicted.
- The level field is 6 bits, so 64 is a hard ceiling.

### 2. Pre-allocate When the Size Is Known

```c
/* Avoids both reallocation and width migration during population */
multilru *mlru = multilruNewWithLevelsCapacity(7, 10000);

for (uint64_t i = 0; i < 10000; i++) {
    multilruInsert(mlru);
}
```

Pre-sizing matters more here than in a typical dynamic array: crossing a width
tier re-packs every existing entry.

### 3. Let the Callback Own Cleanup

Registering an eviction callback keeps external bookkeeping correct even when
eviction happens implicitly during an insert. Polling with
`multilruNeedsEviction()` is only necessary when auto-eviction is disabled.

### 4. Never Cache a Handle Across an Eviction

Handles are recycled LIFO, so a stale handle is likely to resolve to a live
but _different_ entry rather than to nothing. Remove the mapping the moment
the eviction callback fires.

## Testing

`multilru.c` carries its own suite behind `DATAKIT_TEST`, entered through
`int multilruTest(int argc, char *argv[])`. It covers insert/promote/evict
ordering, weighted policies, auto-eviction and callbacks, width migration
across tiers, free-list recycling, and the slot-accounting invariant.

```c
/* Shape of a basic ordering test */
multilru *mlru = multilruNew();
multilruPtr p1 = multilruInsert(mlru);
multilruPtr p2 = multilruInsert(mlru);

multilruIncrease(mlru, p1);  /* p1 -> level 1, p2 is now the coldest */

multilruPtr evicted;
assert(multilruRemoveMinimum(mlru, &evicted));
assert(evicted == p2);
assert(!multilruIsPopulated(mlru, p2));  /* p2 was at level 0: true eviction */

multilruFree(mlru);
```

## Debugging

```c
#ifdef DATAKIT_TEST
/* Print a human-readable dump of the LRU state */
void multilruRepr(const multilru *mlru);

/* Traverse and count entries, for invariant validation */
ssize_t multilruTraverseSize(const multilru *mlru);
#endif
```

## Thread Safety

multilru is **not thread-safe**, and even read-shaped operations mutate it:
`multilruIncrease()` relinks list nodes, and `multilruInsert()` can evict.
Guard every entry point with the same exclusive lock.

```c
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_lock(&lock);
multilruPtr ptr = multilruInsert(mlru);
pthread_mutex_unlock(&lock);

pthread_mutex_lock(&lock);
multilruIncrease(mlru, ptr);   /* a "read" that still writes */
pthread_mutex_unlock(&lock);
```

A reader-writer lock buys nothing here because there is no read-only path.

## Comparison with Other LRU Implementations

| Feature            | multilru         | Hash + DLL  | Clock/CLOCK-Pro |
| ------------------ | ---------------- | ----------- | --------------- |
| Eviction           | O(1)             | O(1)        | O(n) worst case |
| Access             | O(1)             | O(1)        | O(1)            |
| Memory/Entry       | 5-16 bytes       | 24-40 bytes | 1-2 bits        |
| Frequency Tracking | Multi-level      | Single list | Reference bits  |
| Scan Resistance    | Good             | Poor        | Good            |
| Lookup by key      | No (handle only) | Yes         | Depends         |

**When to use multilru**:

- You want recency plus a cheap frequency signal
- You want compact per-entry metadata at large scale
- You can key your own storage by an integer handle
- You do not need true LFU counting

**When to use alternatives**:

- You need lookup by key (pair multilru with a hash table, or use one that
  owns its keys)
- You need exact frequency counts (use an LFU structure)
- You are extremely memory constrained (use CLOCK)

## Known Limitations

- **No compaction**: the entry array never shrinks. After mass deletion the
  holes are reused, but the allocation stays at its high water mark.
- **No width downgrade**: entry width only increases. A cache that once grew
  large keeps its wider entries.
- **`RemoveMinimum` may only demote**: callers that expect one call to free
  one entry must use `multilruEvictN()` instead.
- **`GetNLowest`/`GetNHighest` do not report a fill count**: they mark the
  end with `0` slots instead. Use `multilruCollectLowest()` /
  `multilruCollectHighest()` when you need the count.

## See Also

- [multidict](MULTIDICT.md) - Hash table whose optional LRU eviction levels
  are backed by multilru
- [multimap](../multimap/MULTIMAP.md) - Sorted key-value container often paired
  with multilru to build a complete cache
- [databox](../core/DATABOX.md) - Universal value container

## Implementation Notes

**Design philosophy**:

- Index-based architecture keeps entries down to 5 bytes at small scale
- Bit-packed fields, not a packed struct, so the width can adapt at runtime
- Multi-level design balances recency and frequency without per-entry counters
- Level markers reuse the same entry layout, so the array stays uniform
- An intrusive free list avoids any side allocation for recycling

**Use cases**:

- Page caches
- Object caches
- Database buffer pools
- Any LRU scenario where scan resistance matters and the payload lives
  elsewhere
