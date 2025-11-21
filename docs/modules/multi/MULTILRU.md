# multilru - Multi-Level LRU Cache

## Overview

`multilru` is a **multi-level LRU (Least Recently Used) cache** that organizes entries into access frequency tiers. Instead of a simple LRU list, multilru maintains multiple levels where entries graduate to higher levels as they're accessed more frequently, providing more sophisticated cache replacement policies.

**Key Features:**

- Multi-tiered access tracking (default 7 levels)
- Configurable number of levels
- O(1) insert, access, and eviction operations
- Compact representation (9 bytes per entry with packing)
- Automatic level promotion on access
- Free list management for efficient memory reuse
- Get N lowest/highest accessed entries

**Headers**: `multilru.h`

**Source**: `multilru.c`

## Architecture

```
Access Frequency Levels (7 levels by default):

[H0] -> entry1 -> entry2 -> [H1] -> entry3 -> [H2] -> ... -> [H6]
 └─────────┬──────────────┘    └──────┬──────┘              └───┬──────
      Level 0                     Level 1                    Level 6
   (Least Recent)              (Mid Frequency)           (Most Frequent)

Legend:
- [H0], [H1], etc. are HEAD MARKERS (non-data entries)
- Arrows show doubly-linked list connections
- New entries start at Level 0
- Each access promotes entry to next higher level
- Lowest entry is always the LRU candidate
```

**Entry Structure** (9 bytes with packed attribute):

```c
typedef struct lruEntry {
    uint32_t prev;           /* Previous entry index */
    uint32_t next;           /* Next entry index */
    uint8_t currentLevel:6;  /* 0-63 (which level) */
    uint8_t isPopulated:1;   /* Is this slot in use? */
    uint8_t isHeadNode:1;    /* Is this a level marker? */
} lruEntry;
```

**LRU Pointer**: A `multilruPtr` is just a `size_t` index into the entries array. The actual data associated with an LRU entry is stored elsewhere (typically in a parallel structure), and the multilruPtr serves as the key to both.

## Data Structures

### Main Structure

```c
typedef struct multilru multilru;
typedef size_t multilruPtr;  /* Index into entries array */

struct multilru {
    lruEntry *entries;           /* Array of LRU entries */
    uint32_t highestAllocated;   /* Max entry index */
    uint32_t freePosition[256];  /* Free list for reusing entries */
    multilruPtr lowest;          /* Index of LRU entry */
    multilruPtr *level;          /* Array of level head markers */
    size_t count;                /* Active entry count */
    size_t maxLevels;            /* Number of levels */
};
```

## Creation Functions

```c
/* Create with default 7 levels */
multilru *multilruNew(void);

/* Create with specific number of levels */
multilru *multilruNewWithLevels(size_t maxLevels);

/* Create with custom levels and initial capacity */
multilru *multilruNewWithLevelsCapacity(size_t maxLevels, size_t startCapacity);

/* Example: Default LRU */
multilru *mlru = multilruNew();

/* Example: 5-level LRU with 10000 initial capacity */
multilru *custom = multilruNewWithLevelsCapacity(5, 10000);
```

## Insertion

```c
/* Insert new entry (starts at Level 0) */
multilruPtr multilruInsert(multilru *mlru);

/* Example: Track 1000 cache entries */
multilruPtr cacheKeys[1000];
for (int i = 0; i < 1000; i++) {
    cacheKeys[i] = multilruInsert(mlru);
    /* Store cacheKeys[i] alongside actual cached data */
}
```

**How it works**:

1. Allocates a new entry from the free list or grows array
2. Inserts at head of Level 0 (before H0 marker)
3. Returns `multilruPtr` index
4. Updates `lowest` if this is the only entry

## Access Tracking

```c
/* Promote entry to next higher level */
void multilruIncrease(multilru *mlru, const multilruPtr currentPtr);

/* Example: Record cache hit */
void cacheHit(multilru *mlru, multilruPtr ptr) {
    multilruIncrease(mlru, ptr);
}

/* Example: Simulate access pattern */
multilruPtr p1 = multilruInsert(mlru);
multilruPtr p2 = multilruInsert(mlru);
multilruPtr p3 = multilruInsert(mlru);

/* Access p1 twice -> moves to Level 2 */
multilruIncrease(mlru, p1);
multilruIncrease(mlru, p1);

/* Access p2 once -> moves to Level 1 */
multilruIncrease(mlru, p2);

/* p3 stays at Level 0 (least recently used) */
```

**Level Promotion Logic**:

- Each call moves entry to `currentLevel + 1`
- If already at max level, moves to head of max level
- Automatically updates `lowest` pointer if necessary

## Eviction

### Remove Least Recently Used

```c
/* Remove and return the LRU entry */
bool multilruRemoveMinimum(multilru *mlru, multilruPtr *atomRef);

/* Example: Evict LRU entry */
multilruPtr evicted;
if (multilruRemoveMinimum(mlru, &evicted)) {
    printf("Evicted entry: %zu\n", evicted);
    /* Free associated data using 'evicted' as key */
}

/* Example: LRU cache with max size */
#define MAX_CACHE_SIZE 1000

multilruPtr insertWithEviction(multilru *mlru, void *data) {
    if (multilruCount(mlru) >= MAX_CACHE_SIZE) {
        multilruPtr evicted;
        if (multilruRemoveMinimum(mlru, &evicted)) {
            /* Free data associated with evicted entry */
            freeData(evicted);
        }
    }

    return multilruInsert(mlru);
}
```

### Delete Specific Entry

```c
/* Delete entry by pointer */
void multilruDelete(multilru *mlru, const multilruPtr ptr);

/* Example: Remove specific cache entry */
void cacheRemove(multilru *mlru, multilruPtr ptr) {
    multilruDelete(mlru, ptr);
    /* Also delete associated data */
}
```

**Delete vs RemoveMinimum**:

- `multilruDelete`: Remove any entry by pointer
- `multilruRemoveMinimum`: Always removes lowest (LRU) entry

## Query Operations

### Metadata

```c
/* Get number of active entries */
size_t multilruCount(const multilru *mlru);

/* Get total allocated capacity */
size_t multilruCapacity(const multilru *mlru);

/* Get total bytes used (including free entries) */
size_t multilruBytes(const multilru *mlru);

/* Example */
printf("LRU has %zu/%zu entries using %zu bytes\n",
       multilruCount(mlru),
       multilruCapacity(mlru),
       multilruBytes(mlru));
```

### Get Multiple Entries

```c
/* Get N least recently used entries */
void multilruGetNLowest(multilru *mlru, multilruPtr N[], size_t n);

/* Get N most recently used entries */
void multilruGetNHighest(multilru *mlru, multilruPtr N[], size_t n);

/* Example: Get 10 LRU candidates for eviction */
multilruPtr candidates[10];
multilruGetNLowest(mlru, candidates, 10);

for (size_t i = 0; i < 10; i++) {
    printf("Candidate %zu: entry %zu\n", i, candidates[i]);
}

/* Example: Get 5 most frequently accessed */
multilruPtr hottest[5];
multilruGetNHighest(mlru, hottest, 5);
```

## Memory Management

```c
/* Free entire LRU */
void multilruFree(multilru *mlru);

/* Example */
multilruFree(mlru);
mlru = NULL;
```

## LRU Eviction Policy

### Standard LRU vs Multi-Level LRU

**Standard LRU**:

- Single list ordered by access time
- Every access moves entry to head
- Evict from tail

**Multi-Level LRU** (multilru):

- Multiple lists by access frequency
- Access promotes to next level (not always to head)
- Better discrimination between hot and cold data

### Eviction Behavior

Given:

```
[H0] -> e1 -> [H1] -> e2 -> e3 -> [H2] -> e4 -> [H3]
```

Where:

- e1 at Level 0 (accessed once)
- e2, e3 at Level 1 (accessed 2-3 times)
- e4 at Level 2 (accessed 4+ times)

**Eviction order**: e1, e2, e3, e4

**After access to e2**:

```
[H0] -> e1 -> [H1] -> e3 -> [H2] -> e2 -> e4 -> [H3]
```

e2 promoted to Level 2, now e1 is LRU.

### Promotion Strategy

```c
void demonstratePromotion() {
    multilru *mlru = multilruNew();

    multilruPtr p = multilruInsert(mlru);  /* p at Level 0 */

    multilruIncrease(mlru, p);  /* p now at Level 1 */
    multilruIncrease(mlru, p);  /* p now at Level 2 */
    multilruIncrease(mlru, p);  /* p now at Level 3 */
    /* ... */
    multilruIncrease(mlru, p);  /* p now at Level 6 (max) */
    multilruIncrease(mlru, p);  /* p at Level 6 (stays at max) */

    multilruFree(mlru);
}
```

## Free List Management

multilru maintains a free list to reuse deleted entry slots:

```c
uint32_t freePosition[256];  /* Up to 256 cached free entries */
```

**How it works**:

1. On delete, entry index is added to `freePosition[]`
2. On insert, checks `freePosition[]` first
3. If no free entries, allocates from `highestAllocated`
4. If `freePosition[]` is full, deleted entries are "lost" until rediscovery

**Free Discovery**:
When `freePosition[]` is exhausted, `markFreeDiscover()` scans the entire array to find free entries. This is O(n) but happens rarely.

## Performance Characteristics

### Time Complexity

| Operation     | Complexity | Notes                         |
| ------------- | ---------- | ----------------------------- |
| Insert        | O(1)       | Amortized due to array growth |
| Increase      | O(1)       | Just updates pointers         |
| RemoveMinimum | O(1)       | Direct access to lowest       |
| Delete        | O(1)       | Direct index access           |
| GetNLowest    | O(n)       | Walks linked list             |
| GetNHighest   | O(n)       | Walks linked list backwards   |
| Count         | O(1)       | Cached value                  |

### Space Complexity

**Per Entry**: 9 bytes (with packed struct)

- 4 bytes: `prev`
- 4 bytes: `next`
- 1 byte: bitfields (level, populated, headNode)

**Per LRU**:

- Base struct: ~56 bytes
- Entries array: 9 × capacity bytes
- Level markers: 8 × maxLevels bytes
- Free list: 4 × 256 = 1024 bytes

**Example**: 10,000 entry LRU with 7 levels

- Entries: 90,000 bytes
- Overhead: ~1,200 bytes
- **Total**: ~91 KB (9.1 bytes per entry)

### Growth Behavior

multilru grows the entries array using `jebufSizeAllocation()`:

```c
const size_t growTo = jebufSizeAllocation(originalSize * 2);
```

This rounds up to allocator size classes, minimizing fragmentation.

## Common Patterns

### Pattern 1: Simple LRU Cache

```c
#define MAX_CACHE_ENTRIES 1000

typedef struct cache {
    multilru *lru;
    void *data[MAX_CACHE_ENTRIES];
} cache;

cache *cacheNew() {
    cache *c = malloc(sizeof(*c));
    c->lru = multilruNew();
    memset(c->data, 0, sizeof(c->data));
    return c;
}

void *cacheGet(cache *c, multilruPtr ptr) {
    multilruIncrease(c->lru, ptr);
    return c->data[ptr];
}

multilruPtr cachePut(cache *c, void *value) {
    if (multilruCount(c->lru) >= MAX_CACHE_ENTRIES) {
        multilruPtr evicted;
        if (multilruRemoveMinimum(c->lru, &evicted)) {
            free(c->data[evicted]);
            c->data[evicted] = NULL;
        }
    }

    multilruPtr ptr = multilruInsert(c->lru);
    c->data[ptr] = value;
    return ptr;
}
```

### Pattern 2: Tiered Cache Eviction

```c
/* Evict entries only from lowest N tiers */
void evictColdEntries(multilru *mlru, size_t maxLevel) {
    multilruPtr victim;

    while (multilruRemoveMinimum(mlru, &victim)) {
        /* Get the level of evicted entry */
        /* (Would need to track this separately in real code) */

        /* Only evict if from cold tiers */
        if (getEntryLevel(victim) > maxLevel) {
            /* Re-insert if too hot */
            multilruPtr newPtr = multilruInsert(mlru);
            /* Re-promote to correct level */
            break;
        }

        /* Actually evict cold entry */
        freeData(victim);
    }
}
```

### Pattern 3: Access Pattern Analysis

```c
void analyzeAccessPatterns(multilru *mlru) {
    size_t counts[7] = {0};  /* Assuming 7 levels */

    /* Walk all entries and count by level */
    for (size_t i = 0; i < multilruCapacity(mlru); i++) {
        if (isPopulated(i)) {
            size_t level = getLevel(i);
            counts[level]++;
        }
    }

    printf("Access Distribution:\n");
    for (size_t i = 0; i < 7; i++) {
        printf("  Level %zu: %zu entries (%.1f%%)\n",
               i, counts[i],
               100.0 * counts[i] / multilruCount(mlru));
    }
}
```

## Best Practices

### 1. Choose Appropriate Level Count

```c
/* Few levels (3-5): Simple hot/warm/cold distinction */
multilru *simple = multilruNewWithLevels(3);

/* Default levels (7): Good balance for most uses */
multilru *balanced = multilruNew();

/* Many levels (15+): Fine-grained frequency tracking */
multilru *finegrained = multilruNewWithLevels(15);
```

**Trade-offs**:

- More levels = Better frequency discrimination
- More levels = More memory overhead (8 bytes per level marker)
- More levels = Slower promotion to "hot" tier

### 2. Pre-allocate for Known Sizes

```c
/* If you know max size, pre-allocate */
multilru *mlru = multilruNewWithLevelsCapacity(7, 10000);

/* Avoids reallocation during population */
for (int i = 0; i < 10000; i++) {
    multilruInsert(mlru);
}
```

### 3. Batch Evictions

```c
/* Instead of evicting one-at-a-time */
multilruPtr victims[100];
multilruGetNLowest(mlru, victims, 100);

for (size_t i = 0; i < 100; i++) {
    freeData(victims[i]);
    multilruDelete(mlru, victims[i]);
}
```

### 4. Track Level Separately for Analysis

The multilru structure doesn't expose entry level directly. If you need to query entry levels, maintain a parallel array:

```c
typedef struct cacheWithLevels {
    multilru *lru;
    uint8_t levels[MAX_ENTRIES];
    void *data[MAX_ENTRIES];
} cacheWithLevels;

void recordAccess(cacheWithLevels *c, multilruPtr ptr) {
    c->levels[ptr]++;
    if (c->levels[ptr] < 7) {
        multilruIncrease(c->lru, ptr);
    }
}
```

## Testing

From the test suite in `multilru.c`:

```c
/* Test: Insert and increase */
multilru *mlru = multilruNew();
multilruPtr p1 = multilruInsert(mlru);
multilruPtr p2 = multilruInsert(mlru);

multilruIncrease(mlru, p1);  /* p1 -> Level 1, p2 is now LRU */

multilruPtr evicted;
assert(multilruRemoveMinimum(mlru, &evicted));
assert(evicted == p2);  /* p2 was LRU */

/* Test: Remove and re-insert */
for (int i = 0; i < 768; i++) {
    multilruInsert(mlru);
}

for (int i = 0; i < 200; i++) {
    multilruRemoveMinimum(mlru, NULL);
}

/* Free entries should be reused */
for (int i = 0; i < 200; i++) {
    multilruInsert(mlru);
}

multilruFree(mlru);
```

## Debugging

```c
#ifdef DATAKIT_TEST
/* Print LRU state */
void multilruRepr(const multilru *mlru);

/* Example output:
 * {count {used 100} {capacity 1024}} {lowest 45} {bytes {allocated 9216}}
 * {45} [0] -> 0; [1] -> 12; [2] -> 23; ...
 * (45) -> ([H0]1) -> (67) -> ([H1]2) -> ... {count 100}
 */
#endif
```

## Thread Safety

multilru is **not thread-safe**. For concurrent access:

```c
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* Insert with locking */
pthread_mutex_lock(&lock);
multilruPtr ptr = multilruInsert(mlru);
pthread_mutex_unlock(&lock);

/* Access with locking */
pthread_mutex_lock(&lock);
multilruIncrease(mlru, ptr);
pthread_mutex_unlock(&lock);
```

## Comparison with Other LRU Implementations

| Feature            | multilru    | Hash + DLL  | Clock/CLOCK-Pro |
| ------------------ | ----------- | ----------- | --------------- |
| Eviction           | O(1)        | O(1)        | O(n) worst case |
| Access             | O(1)        | O(1)        | O(1)            |
| Memory/Entry       | 9 bytes     | 24-40 bytes | 1-2 bytes       |
| Frequency Tracking | Multi-level | Single list | Bit flags       |
| Scan Resistance    | Good        | Poor        | Excellent       |

**When to use multilru**:

- Need better than LRU (frequency + recency)
- Want compact representation
- Can tolerate index-based references
- Don't need true LFU (Least Frequently Used)

**When to use alternatives**:

- Need O(1) lookup by key (use hash + DLL)
- Need true LFU counting (use LFU-based algorithm)
- Extremely memory-constrained (use CLOCK)

## See Also

- [multimap](../multimap/MULTIMAP.md) - Can be used to build a complete LRU cache with key-value mapping
- [multidict](MULTIDICT.md) - Hash table for key-value lookups
- [databox](../core/DATABOX.md) - Universal value container

## Implementation Notes

**Design Philosophy**:

- Index-based architecture allows 9-byte entries
- Multi-level design balances recency and frequency
- Free list optimization for high churn workloads
- Level markers use same entry structure (uniform array)

**Packed Struct**:
Using `__attribute__((packed))` saves 7 bytes per entry:

- Without: 16 bytes (standard alignment)
- With: 9 bytes (packed)
- Savings: 43% memory reduction

**Use Cases**:

- Page caches
- Object caches
- Database buffer pools
- Any LRU scenario where multi-tiered eviction is beneficial
