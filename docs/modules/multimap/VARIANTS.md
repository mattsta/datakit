# multimap Variants - Scale-Aware Design

## Overview

The multimap family implements a **scale-aware architecture** with four distinct implementations optimized for different dataset sizes. Your code uses a single `multimap*` pointer, and the implementation automatically chooses and transitions between variants as your data grows.

## The Four Variants

### Summary Table

| Variant    | Size      | Maps        | Overhead        | Best For           | Max Recommended |
| ---------- | --------- | ----------- | --------------- | ------------------ | --------------- |
| **Small**  | 16 bytes  | 1 flex      | 16 bytes        | < 100 entries      | 2KB total       |
| **Medium** | 28 bytes  | 2 flex      | 28 bytes        | 100-1000 entries   | 6KB total       |
| **Full**   | 40+ bytes | N flex      | 40 + N×20 bytes | 1000+ entries      | Unlimited (TBs) |
| **Atom**   | N/A       | 2 multimaps | Wrapper         | Reference counting | Special purpose |

## multimapSmall

### Structure

```c
struct multimapSmall {
    flex *map;                    /* Single flex array */
    uint32_t middle;              /* Offset to middle element for binary search */
    uint16_t elementsPerEntry;    /* Columns per row */
    uint16_t flags;               /* compress, mapIsSet, etc. */
};

/* Total: 16 bytes fixed overhead */
```

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
        │          ▲                           │
        │          └─ middle points here       │
        └─────────────────────────────────────┘
```

### Characteristics

**Advantages:**

- **Minimal overhead**: Only 16 bytes for the entire container
- **Cache-friendly**: Single contiguous allocation
- **Fast sequential access**: All data in one array
- **Simple implementation**: Single binary search

**Disadvantages:**

- **Slow inserts**: O(n) memory moves for insertions
- **Limited scalability**: Becomes inefficient beyond 2KB
- **No parallelism**: Single map prevents multi-threaded operations

**Upgrade Trigger:**

```c
/* Upgrades to Medium when: */
if (bytes > sizeLimit && count > elementsPerEntry * 2) {
    /* Need at least 2 complete entries to split properly */
    upgrade_to_medium();
}
```

### Best Use Cases

1. **Small dictionaries** (< 100 entries)
2. **Configuration maps**
3. **Temporary lookups**
4. **Embedded data structures**

### Example

```c
/* Perfect for small config */
multimap *config = multimapNewLimit(2, FLEX_CAP_LEVEL_512);

databox k1 = databoxNewBytesString("timeout");
databox v1 = databoxNewSigned(30);
multimapInsert(&config, (const databox*[]){&k1, &v1});

databox k2 = databoxNewBytesString("retries");
databox v2 = databoxNewSigned(3);
multimapInsert(&config, (const databox*[]){&k2, &v2});

/* Still Small, only ~50 bytes total */
assert(multimapBytes(config) < 100);
```

## multimapMedium

### Structure

```c
struct multimapMedium {
    flex *map[2];                 /* Two flex arrays */
    uint32_t middle[2];           /* Middle offset for each map */
    uint16_t elementsPerEntry;
    uint16_t flags;
};

/* Total: 28 bytes fixed overhead */
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
│ elementsPerEntry│ │                │
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
   keys < split         keys >= split
```

### How Medium Works

When Small upgrades to Medium, it:

1. Splits the single flex at its middle point
2. Creates two new flex arrays: `map[0]` (lower) and `map[1]` (higher)
3. Distributes elements: lower keys → map[0], higher keys → map[1]

**Binary Search Strategy:**

```c
/* To find a key: */
1. Check head of map[1]
2. If key < head[1], search map[0]
3. Else, search map[1]
```

This provides a **2-way branching** that reduces search space by ~50%.

### Characteristics

**Advantages:**

- **Better insert performance**: Only move ~half the data on average
- **Improved cache behavior**: Smaller individual maps
- **Stepping stone**: Prepares for Full variant
- **Still compact**: Only 12 extra bytes vs Small

**Disadvantages:**

- **More complex**: Two binary searches possible
- **Conformance overhead**: Must keep map[0] populated
- **Limited scaling**: Still only 2 maps

**Upgrade Trigger:**

```c
/* Upgrades to Full when: */
if (bytes > sizeLimit * 3 && count > elementsPerEntry * 2) {
    upgrade_to_full();
}
```

### Best Use Cases

1. **Medium-sized caches** (100-1000 entries)
2. **Session stores**
3. **Transitional datasets**
4. **Index structures**

### Example

```c
multimap *m = multimapNewLimit(2, FLEX_CAP_LEVEL_2048);

/* Insert 500 entries - grows to Medium */
for (int i = 0; i < 500; i++) {
    databox k = databoxNewSigned(i);
    databox v = databoxNewSigned(i * 2);
    multimapInsert(&m, (const databox*[]){&k, &v});
}

/* Now Medium with 2 maps */
/* map[0]: keys 0-249 */
/* map[1]: keys 250-499 */
```

### Conformance Rules

Medium has a special rule: **map[0] must always have elements if any elements exist**.

If you delete all elements from map[0], Medium automatically moves map[1] → map[0]:

```c
/* Before delete: */
/* map[0]: [10, 20]  map[1]: [30, 40, 50] */

multimapDelete(&m, &key10);
multimapDelete(&m, &key20);

/* After delete: */
/* map[0]: [30, 40, 50]  map[1]: [] */
/* map[1] automatically moved to map[0]! */
```

## multimapFull

### Structure

```c
struct multimapFull {
    multiarray *map;              /* Array of flex* (dynamic) */
    multiarray *middle;           /* Array of middle offsets */
    multiarray *rangeBox;         /* Array of databox (range markers) */
    uint32_t count;               /* Number of maps */
    uint32_t values;              /* Total number of rows */
    uint32_t elementsPerEntry;
    uint32_t maxSize : 17;
    uint32_t flags : 15;
};

/* Total: 40 bytes + (count × 20 bytes) overhead */
```

### Architecture

```
┌─────────────────────────────────────────┐
│ multimapFull (40 bytes base)            │
├─────────────────────────────────────────┤
│ map ──────┐                             │
│ middle ───┼─┐                           │
│ rangeBox ─┼─┼─┐                         │
│ count = N │ │ │                         │
│ values    │ │ │                         │
└───────────┼─┼─┼───────────────────────┘
            │ │ │
            │ │ │
    ┌───────┘ │ │
    │   ┌─────┘ │
    │   │  ┌────┘
    ▼   ▼  ▼
 ┌─────┬─────┬─────────┐
 │map[]│mid[]│rangeBox[]│
 │  0  │  0  │    -     │
 │  1  │  1  │  RB[0]   │
 │  2  │  2  │  RB[1]   │
 │ ... │ ... │   ...    │
 │ N-1 │ N-1 │  RB[N-2] │
 └─────┴─────┴─────────┘
   │     │       │
   │     │       └─ Head key of each map (for binary search)
   │     └───────── Middle offset for binary search within map
   └─────────────── Actual flex* to data

Maps grow dynamically:
map[0]: [K1...K100]
map[1]: [K101...K200]
map[2]: [K201...K300]
...
map[N]: [K(N*100)...K(N*100+100)]
```

### How Full Works

**Multi-Level Binary Search:**

```c
/* To find key K: */
1. Binary search rangeBox array to find which map
2. Binary search within that specific map
```

Example with 5 maps:

```
rangeBox[0] = 100    (map[1] starts at 100)
rangeBox[1] = 500    (map[2] starts at 500)
rangeBox[2] = 1000   (map[3] starts at 1000)
rangeBox[3] = 5000   (map[4] starts at 5000)

Looking for key 750:
1. Binary search rangeBox: 750 is between 500 and 1000
2. Therefore, search map[2]
3. Binary search within map[2] to find exact position
```

**Map Splitting:**

When a map exceeds `maxSize` bytes:

1. Find the middle element
2. Split into [LOW, HIGH]
3. Insert HIGH as new map after LOW
4. Update rangeBox for the new map

### Characteristics

**Advantages:**

- **Unlimited scaling**: Grows to terabytes
- **Efficient large inserts**: Only realloc small arrays
- **Parallelizable**: Different maps can be accessed concurrently
- **Balanced performance**: O(log N) map find + O(log M) element find

**Disadvantages:**

- **Higher overhead**: 40 + (N × 20) bytes base cost
- **Complex memory management**: Multiple dynamic arrays
- **Indirection cost**: Extra pointer chasing
- **Not worth it for small data**: Overkill < 1000 entries

**No Further Upgrades**: Full is the final form.

### Best Use Cases

1. **Large databases** (10,000+ entries)
2. **Analytics datasets**
3. **Time-series at scale**
4. **In-memory tables**
5. **Big data structures**

### Example

```c
multimap *big = multimapNewLimit(2, FLEX_CAP_LEVEL_2048);

/* Insert 100,000 entries */
for (int i = 0; i < 100000; i++) {
    databox k = databoxNewSigned(i);
    databox v = databoxNewReal(i * 3.14);
    multimapInsert(&big, (const databox*[]){&k, &v});
}

/* Now Full with ~50 maps (depends on maxSize) */
/* Each map holds ~2000 entries */
```

### Range Box Details

The `rangeBox` array stores the **first key** of each map (except map[0]):

```c
/* With 4 maps containing keys: */
/* map[0]: [1, 5, 10, 15] */
/* map[1]: [20, 25, 30]    <- rangeBox[0] = 20 */
/* map[2]: [50, 60, 70]    <- rangeBox[1] = 50 */
/* map[3]: [100, 150, 200] <- rangeBox[2] = 100 */

/* To find key 65: */
/* rangeBox search: 65 is between 50 and 100 */
/* Search map[2] */
```

**Important**: Range boxes hold **actual key values**, not references, even when using reference containers. This ensures comparison always works.

## multimapAtom

### Purpose

multimapAtom is **not a multimap variant** - it's a **reference counting system** for string deduplication.

### Structure

```c
struct multimapAtom {
    multimap *mapAtomForward;  /* ID → {String, Refcount} */
    multimap *mapAtomReverse;  /* String → ID (sorted by string) */
    uint64_t highest;          /* Next ID to allocate */
};
```

### How It Works

**String Interning:**

```c
multimapAtom *atoms = multimapAtomNew();

/* First insert: "hello" */
databox str1 = databoxNewBytesString("hello");
multimapAtomInsertConvert(atoms, &str1);
/* str1 is now: {type: REFERENCE, data: 0} */
/* atoms contains: Forward[0] = {"hello", refcount=0} */
/*                 Reverse["hello"] = 0 */

/* Second insert: "hello" again */
databox str2 = databoxNewBytesString("hello");
multimapAtomInsertIfNewConvert(atoms, &str2);
/* str2 is now: {type: REFERENCE, data: 0} (same ID!) */
/* Both str1 and str2 now point to ID 0 */

/* "world" gets a different ID */
databox str3 = databoxNewBytesString("world");
multimapAtomInsertConvert(atoms, &str3);
/* str3 is now: {type: REFERENCE, data: 1} */
```

### Reference Counting

```c
/* Increment reference count */
multimapAtomRetain(atoms, &str1);  /* refcount: 0 → 1 */

/* Decrement reference count */
bool deleted = multimapAtomRelease(atoms, &str1);  /* refcount: 1 → 0 */
/* If refcount goes negative, entry is deleted */
```

### Best Use Cases

1. **String deduplication**: Many copies of same strings
2. **Reference counting**: Track string lifetime
3. **String interning**: Constant string pool
4. **Memory optimization**: Reduce duplicate string storage

### Example

```c
multimapAtom *atoms = multimapAtomNew();

/* Store user comments with deduplicated authors */
multimap *comments = multimapNew(3);

for (int i = 0; i < 1000; i++) {
    databox id = databoxNewSigned(i);

    /* Same authors appear multiple times */
    databox author = databoxNewBytesString(
        i % 10 == 0 ? "alice" :
        i % 10 == 1 ? "bob" : "charlie"
    );

    /* Convert to atom reference */
    multimapAtomInsertIfNewConvert(atoms, &author);
    /* author is now a small integer reference! */

    databox text = databoxNewBytesString("Great post!");
    multimapInsert(&comments, (const databox*[]){&id, &author, &text});
}

/* "alice", "bob", "charlie" stored only once in atoms */
/* comments stores small integer IDs instead of full strings */
```

## Variant Comparison

### Memory Overhead

For a map with 1000 entries, 2 elements per entry, avg 20 bytes per element:

```
Small:   16 + 40,000 = 40,016 bytes  (0.04% overhead)
Medium:  28 + 40,000 = 40,028 bytes  (0.07% overhead)
Full:    40 + (25×20) + 40,000 = 40,540 bytes  (1.3% overhead)
         ^     ^        ^
         base  maps     data
```

### Performance Comparison

**Insertion (1000 random keys):**

```
Small:   ~100 ms   (many large memmoves)
Medium:  ~60 ms    (smaller memmoves)
Full:    ~40 ms    (minimal memmoves per map)
```

**Lookup (1000 random keys):**

```
Small:   ~5 ms     (single binary search)
Medium:  ~6 ms     (map select + binary search)
Full:    ~8 ms     (rangeBox search + map search + binary search)
```

**Iteration (1000 entries):**

```
Small:   ~2 ms     (single array)
Medium:  ~2.5 ms   (two arrays)
Full:    ~4 ms     (N arrays with overhead)
```

### When to Use Each Variant

```
       Small          Medium          Full
         |              |              |
    0 ──┴── 100 ───────┴──── 1000 ────┴──── ∞

         ← Manual Selection Not Needed →

         Automatic transitions handle it!
```

**Manual Selection** (rare, for special cases):

```c
/* Force staying Small for tiny embedded use */
multimap *tiny = multimapNewLimit(2, FLEX_CAP_LEVEL_128);

/* Force early Full for known large data */
multimap *big = multimapNewLimit(2, FLEX_CAP_LEVEL_512);
/* Will upgrade to Full quickly */
```

## Transition Mechanics

### Small → Medium Transition

```c
/* Before: */
multimapSmall {
    map = [K1,V1, K2,V2, K3,V3, K4,V4]
    middle = offset to K3
}

/* Trigger: bytes > limit && count > 4 */

/* After: */
multimapMedium {
    map[0] = [K1,V1, K2,V2]
    map[1] = [K3,V3, K4,V4]
    middle[0] = offset to K1-K2 split
    middle[1] = offset to K3-K4 split
}
```

### Medium → Full Transition

```c
/* Before: */
multimapMedium {
    map[0] = [100 entries]
    map[1] = [100 entries]
}

/* Trigger: bytes > limit * 3 && count > 4 */

/* After: */
multimapFull {
    map[0] = [100 entries]
    map[1] = [100 entries]
    rangeBox[0] = head key of map[1]
    count = 2
}

/* As more data inserted, splits continue: */
/* map[2], map[3], ... map[N] */
```

### Transition Transparency

```c
multimap *m = multimapNew(2);
printf("Type: Small\n");

for (int i = 0; i < 10000; i++) {
    databox k = databoxNewSigned(i);
    databox v = databoxNewSigned(i);
    multimapInsert(&m, (const databox*[]){&k, &v});

    /* Somewhere around i=100: transitions to Medium */
    /* Somewhere around i=1000: transitions to Full */
    /* Your code doesn't change! */
}

printf("Type: Full\n");
/* Same API, different implementation */
```

## Design Philosophy

The multimap family follows the principle of **progressive enhancement**:

1. **Start minimal**: 16-byte Small for tiny datasets
2. **Grow gracefully**: 28-byte Medium for intermediate scale
3. **Scale infinitely**: Variable Full for unlimited growth
4. **Transparent to user**: Same API throughout

This approach optimizes for the **common case** (small data) while supporting the **exceptional case** (massive data) without requiring users to make upfront decisions about expected dataset size.

## See Also

- [MULTIMAP.md](MULTIMAP.md) - Main API documentation
- [EXAMPLES.md](EXAMPLES.md) - Real-world usage patterns
- [flex](../flex/FLEX.md) - Underlying storage mechanism
- [multiarray](../multiarray/MULTIARRAY.md) - Dynamic array for Full variant
