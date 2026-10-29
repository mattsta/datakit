# Scale-Aware Collections - Choosing the Right Size Variant

## Overview

One of datakit's most powerful design patterns is **scale-aware architecture**: data structures that automatically adapt their internal representation based on dataset size. This guide helps you understand when to use each variant and how to make optimal choices for your application.

## What is Scale-Aware Design?

Scale-aware collections recognize that **small data has different optimization requirements than large data**:

- **Small data** (< 2KB): Minimize overhead, simple algorithms, cache-friendly layout
- **Medium data** (2KB - 100KB): Balance between overhead and scalability
- **Large data** (> 100KB): Sophisticated indexing, parallel-friendly, unlimited growth

Instead of choosing upfront, datakit containers **automatically transition** between variants as your data grows.

## Module Comparison

### Multi-Container Variants

| Module         | Small         | Medium    | Large/Full | Native  |
| -------------- | ------------- | --------- | ---------- | ------- |
| **multimap**   | 0 or 16 bytes | 28 bytes  | 40+ bytes  | N/A     |
| **multilist**  | 0 bytes       | 16 bytes  | 24+ bytes  | N/A     |
| **multiarray** | 16 bytes      | 16+ bytes | 24+ bytes  | 0 bytes |

### Detailed Variant Breakdown

#### multimap Variants

```
┌──────────────┬───────────┬──────────────┬─────────────────────┐
│ Variant      │ Overhead  │ Storage      │ Best For            │
├──────────────┼───────────┼──────────────┼─────────────────────┤
│ Small        │ 0 or 16 B │ 1 flex       │ < 100 entries       │
│ Medium       │ 28 bytes  │ 2 flex       │ 100-1000 entries    │
│ Full         │ 40 bytes  │ N flex       │ 1000+ entries       │
└──────────────┴───────────┴──────────────┴─────────────────────┘
```

`multimapAtom` is NOT a fourth size tier. It is the reference
container: a refcounted interning table other maps point into via the
`*WithReference` / `*WithSurrogateKey` APIs. It has its own
constructor, `multimapAtomNew()`, and is chosen for deduplication, not
for size.

**Key Characteristics:**

- **Small**: Single sorted flex, binary search, O(n) inserts
- **Medium**: Two-way branching, ~50% search space reduction
- **Full**: Dynamic multi-way branching, unlimited scalability

**Upgrade Triggers** (from `multimapUpgradeIfNeeded` in `multimap.c`):

- Small → Medium: `bytes > sizeLimit && count > elementsPerEntry * 2`.
  The count guard exists because a Medium split needs two complete
  entries to divide.
- Medium → Full: `bytes > sizeLimit * 3` AND both underlying flexes are
  non-empty.

**`maxSize` is a target, not a hard cap.** Full re-splits a sub-map
only when it is both over `maxSize` AND holds more than one entry — a
single entry larger than `maxSize` is unsplittable and stays put. A
replace into an already-oversized map also stays in that map rather
than triggering a split, because routing it elsewhere would put the
same key in two maps.

#### multilist Variants

```
┌──────────────┬───────────┬──────────────┬─────────────────────┐
│ Variant      │ Overhead  │ Nodes        │ Best For            │
├──────────────┼───────────┼──────────────┼─────────────────────┤
│ Small        │ 8 bytes   │ 1 flex       │ < 2KB total         │
│ Medium       │ 16 bytes  │ 2 flex       │ 2KB - 6KB           │
│ Full         │ 24 bytes  │ N mflex      │ > 6KB, compression  │
└──────────────┴───────────┴──────────────┴─────────────────────┘
```

**Key Characteristics:**

- **Small**: Minimal overhead, perfect for temporary lists
- **Medium**: Head/tail split, rebalancing logic
- **Full**: LZ4 compression support, massive scalability

**Compression (Full only):**

```c
compress = 0:  No compression
compress = 1:  Head and tail uncompressed (most compression)
compress = 2:  First 2 and last 2 nodes uncompressed (balanced)
compress = N:  First N and last N nodes uncompressed
```

#### multiarray Variants

```
┌──────────────┬───────────┬──────────────┬─────────────────────┐
│ Variant      │ Overhead  │ Structure    │ Best For            │
├──────────────┼───────────┼──────────────┼─────────────────────┤
│ Native       │ 0 bytes   │ Raw array    │ Stack/fixed arrays  │
│ Small        │ 16 bytes  │ Single array │ < 2K elements       │
│ Medium       │ 16+ bytes │ Node array   │ 2K-100K elements    │
│ Large        │ 24+ bytes │ XOR list     │ 100K+ elements      │
└──────────────┴───────────┴──────────────┴─────────────────────┘
```

**Key Characteristics:**

- **Native**: Zero overhead, automatic upgrade when full
- **Small**: Simple dynamic array with realloc
- **Medium**: Chunked storage, reduced realloc cost
- **Large**: XOR linked list, saves 8 bytes per node vs traditional

## Decision Trees

### Choosing multimap Variant

```
Need key-value storage?
│
├─ Any size, known or unknown?
│  └─ multimapNew(elementsPerEntry)
│     (starts Small, upgrades automatically — the normal answer)
│
├─ Want to pin the per-flex size limit?
│  └─ multimapNewLimit(elementsPerEntry, FLEX_CAP_LEVEL_*)
│
├─ Deliberately pinning one variant?
│  └─ multimapMediumNew(n) / multimapFullNew(n)
│
├─ Whole-row dedupe (a set, not a map)?
│  └─ multimapSetNew(elementsPerEntry)
│     (compares ALL elements, not just the key)
│
└─ Need string interning / reference counting?
   └─ multimapAtomNew(), used with the *WithReference APIs
```

### Choosing multilist Variant

```
Need ordered sequence?
│
├─ Temporary/small (< 2KB)?
│  └─ Let it start Small: multilistNewLimit()
│
├─ Long-lived, memory-critical?
│  └─ Enable compression: multilistNewLimit(cap, 2)
│     (compress all but 2 nodes from each end)
│
└─ Large dataset, read-heavy?
   └─ More uncompressed nodes: multilistNewLimit(cap, 10)
      (faster access to edges)
```

### Choosing multiarray Variant

```
Need dynamic array?
│
├─ Want the wrapper to pick and switch tiers for you?
│  └─ multiarrayNew(elementLen, rowMax)
│
├─ Fixed-size holder you own, no container overhead?
│  └─ multiarrayNativeNew(holder)
│     (macro over multiarraySmallNativeNew; you keep the count)
│
├─ Random access critical?
│  └─ multiarrayMediumNew(elementLen, rowMax)
│
└─ Sequential access, huge dataset?
   └─ multiarrayLargeNew(elementLen, rowMax)
```

Both `rowMax` and `elementLen` are 16-bit: `rowMax` is the number of
elements a row holds before the container splits, NOT a total capacity.

## Size Guidelines

### multimap Size Recommendations

```c
/* Configuration map (< 20 entries) */
multimap *config = multimapNewLimit(2, FLEX_CAP_LEVEL_512);
// Stays Small: 16 bytes overhead

/* Session cache (100-500 entries) */
multimap *sessions = multimapNewLimit(3, FLEX_CAP_LEVEL_2048);
// Upgrades to Medium: 28 bytes overhead
// 2 maps provide good balance

/* Database index (10,000+ entries) */
multimap *index = multimapNewLimit(2, FLEX_CAP_LEVEL_4096);
// Upgrades to Full: 40 + (N×20) bytes overhead
// Dynamic map splitting handles growth
```

### multilist Size Recommendations

```c
/* Work queue (small, temporary) */
multilist *queue = multilistNewLimit(FLEX_CAP_LEVEL_512, 0);
// Small variant: 8 bytes overhead

/* Message log (moderate, persistent) */
multilist *log = multilistNewLimit(FLEX_CAP_LEVEL_2048, 1);
// Medium → Full with compression
// compress=1: max compression for storage

/* Event stream (large, read-heavy) */
multilist *events = multilistNewLimit(FLEX_CAP_LEVEL_4096, 5);
// Full with compress=5
// Fast access to recent events (5 nodes each end)
```

### multiarray Size Recommendations

```c
/* Point array in a caller-owned holder */
typedef struct Point { float x, y, z; } Point;
Point holder[1000];
multiarray *points = multiarrayNativeNew(holder);
// Native: no container header; you track the element count

/* Growing collection (unknown size) */
multiarrayMedium *items = multiarrayMediumNew(sizeof(Item), 512);
// Medium: chunked growth, good for insert-heavy

/* Huge append-only log */
multiarrayLarge *log = multiarrayLargeNew(sizeof(LogEntry), 1024);
// Large: efficient for millions of entries

/* Or just let the wrapper choose */
multiarray *auto_ = multiarrayNew(sizeof(Item), 512);
```

None of the multiarray variants export a count or a byte size — keep
your own element count next to the container.

## Performance Characteristics by Variant

### Insert Performance

| Operation              | Small | Medium     | Full/Large           |
| ---------------------- | ----- | ---------- | -------------------- |
| Insert head            | O(n)  | O(n)       | O(1) amortized       |
| Insert tail            | O(1)  | O(1)       | O(1) amortized       |
| Insert middle          | O(n)  | O(n)       | O(n/N) where N=nodes |
| Binary insert (sorted) | O(n)  | O(n/2) avg | O(log M + n/N)       |

### Lookup Performance

| Operation              | Small    | Medium     | Full/Large         |
| ---------------------- | -------- | ---------- | ------------------ |
| Random access (map)    | O(log n) | O(log n/2) | O(log M + log n)   |
| Sequential scan        | O(n)     | O(n)       | O(n)               |
| Binary search (sorted) | O(log n) | O(log n)   | O(log M + log n/M) |

### Memory Efficiency

**1000 elements, 2 fields per entry, 20 bytes per field:**

```
Small:   16 + 40,000 = 40,016 bytes  (0.04% overhead)
Medium:  28 + 40,000 = 40,028 bytes  (0.07% overhead)
Full:    40 + (25×20) + 40,000 = 40,540 bytes  (1.3% overhead)

Conclusion: Overhead is negligible even for Full variant
```

## Transition Mechanics

### Automatic Upgrades

All transitions are **transparent** to application code:

```c
multimap *m = multimapNew(2);  // Starts as Small

/* Add 50 entries - still Small */
for (int i = 0; i < 50; i++) {
    multimapInsert(&m, ...);
}

/* Add 500 more - upgrades to Medium */
for (int i = 0; i < 500; i++) {
    multimapInsert(&m, ...);  // Transparent upgrade!
}

/* Add 10,000 more - upgrades to Full */
for (int i = 0; i < 10000; i++) {
    multimapInsert(&m, ...);  // Another transparent upgrade!
}

// Same API throughout, m pointer updated automatically
multimapFree(m);  // Works regardless of variant
```

### No Downgrades

**Important**: Variants never downgrade automatically.

```c
multimap *m = multimapNew(2);

/* Trigger upgrade to Full */
for (int i = 0; i < 10000; i++) {
    multimapInsert(&m, ...);
}
// Now Full variant

/* Delete most entries */
for (int i = 0; i < 9900; i++) {
    multimapDelete(&m, ...);
}
// Still Full variant with 100 entries!

/* To downgrade, recreate */
multimap *small = multimapNew(2);
// Copy 100 entries to small
multimapFree(m);
m = small;  // Now Small variant again
```

## Manual Variant Selection

### When to Override Automatic Selection

1. **Known dataset size**: Skip intermediate upgrades
2. **Performance testing**: Compare variants directly
3. **Memory profiling**: Control exact representation

### Direct Variant Creation

```c
/* multimap - Direct creation.
 * Small also needs the set-mode flag; Medium and Full take just the
 * entry width. */
multimapSmall *ms = multimapSmallNew(2, false /* isSet */);
multimapMedium *mm = multimapMediumNew(2);
multimapFull *mf = multimapFullNew(2);

/* multilist - Direct creation */
multilistSmall *ls = multilistSmallCreate();
multilistMedium *lm = multilistMediumCreate();
multilistFull *lf = multilistFullNew(FLEX_CAP_LEVEL_4096, 2);

/* multiarray - Direct creation (elementLen, rowMax) */
multiarraySmall *as = multiarraySmallNew(sizeof(T), 1024);
multiarrayMedium *am = multiarrayMediumNew(sizeof(T), 512);
multiarrayLarge *al = multiarrayLargeNew(sizeof(T), 1024);
```

Pinning a variant opts out of automatic retiering: the pinned type will
not upgrade itself when it outgrows its shape. Use `multimapNew()`
unless you have measured a reason not to.

**Warning**: Direct creation requires manual management and loses automatic upgrade benefits.

## Special Cases

### intset Variable Encoding

intset uses a different type of scale-awareness: **variable-width encoding**.

```c
intset *is = intsetNew();  // Starts as INT16 (2 bytes per element)

is = intsetAdd(is, 100, NULL);      // Still INT16
is = intsetAdd(is, 50000, NULL);    // Upgrades to INT32 (4 bytes)!
is = intsetAdd(is, 5000000000L, NULL);  // Upgrades to INT64 (8 bytes)!

// All elements now use 8 bytes each, even value '100'
// No downgrade - encoding is permanent
```

**Guideline**: If you know you'll have large values, add them first:

```c
intset *is = intsetNew();
is = intsetAdd(is, INT32_MAX, NULL);  // Force INT32 upfront
// Now all subsequent adds use INT32, no surprise upgrades
```

### flex Compression

flex itself doesn't have variants, but can be compressed:

```c
/* Large flex */
flex *f = /* ... 100KB of data ... */;

/* Convert to compressed form */
cflex *c = /* allocate buffer */;
if (flexConvertToCFlex(f, c, bufferSize)) {
    size_t compressed = cflexBytesCompressed(c);
    size_t original = flexBytes(f);
    printf("Compression: %.1fx\n", (double)original / compressed);
}

/* Decompress when needed */
flex *restored = NULL;
cflexConvertToFlex(c, &restored, &restoredSize);
```

## Best Practices

### 1. Trust Automatic Upgrades

```c
/* GOOD: Let the system manage */
multimap *m = multimapNew(2);
// Starts optimal, grows as needed

/* AVOID: Premature optimization */
multimap *m = multimapNewFull();  // Overkill for small data
```

### 2. Set Appropriate Size Limits

```c
/* Small data - keep it small */
multimap *config = multimapNewLimit(2, FLEX_CAP_LEVEL_512);

/* Large data - allow early upgrade */
multimap *cache = multimapNewLimit(3, FLEX_CAP_LEVEL_4096);
```

### 3. Use Compression Wisely

```c
/* Memory-critical: aggressive compression */
multilist *archive = multilistNewLimit(FLEX_CAP_LEVEL_4096, 1);

/* Performance-critical: minimal compression */
multilist *recent = multilistNewLimit(FLEX_CAP_LEVEL_4096, 10);
```

### 4. Consider Access Patterns

```c
/* Random access heavy → prefer Medium */
multiarrayMedium *random = multiarrayMediumNew(sizeof(T), 512);

/* Sequential access → Large is fine */
multiarrayLarge *sequential = multiarrayLargeNew(sizeof(T), 1024);
```

## Benchmarks

### multimap Variant Performance

No in-tree benchmark pins one multimap variant against another. The
[container matrix](../PERFORMANCE_MATRIX.md) measures the multimap as
callers use it, through its variants from 1K to 1M rows: about 0.5-3M
operations per second at every size (see
[Choosing a Container](../CHOOSING_A_CONTAINER.md)). For random
inserts, the shape is:

- **Small** (one flex) moves the whole flex for each mid-map insert.
- **Medium** splits the data across two flexes, so each memmove is smaller.
- **Full** splits it across many maps, so each insert moves the least data.

### multilist Performance at Scale

A multilist holding a million elements runs as the Full variant (Small and
Medium promote themselves as they grow), so there is no like-for-like
Small/Medium figure at this size. `compressionBench` measures the Full
variant directly. On an Apple M1 Max with clang 22 and the `perf` preset,
1,000,000 tail pushes followed by 1,000,000 head pops of 64-bit integers
take 129 ms with compression off (depth 0), the median of 7 interleaved
rounds. The figures for other depths and for 100-byte strings are in
[PERFORMANCE.md](PERFORMANCE.md#multilist-performance).

### Memory Overhead (10,000 elements, 16 bytes each)

```
Native:    0 + 160,000 = 160,000 bytes   (0.0% overhead)
Small:    16 + 160,000 = 160,016 bytes   (0.01% overhead)
Medium:   16 + 200×16 + 160,000 = 163,216 bytes  (2.0% overhead)
Full:     24 + 100×24 + 160,000 = 162,424 bytes  (1.5% overhead)
```

## Common Patterns

### Pattern 1: Configuration Storage

```c
// Always stays Small
multimap *config = multimapNewLimit(2, FLEX_CAP_LEVEL_256);
multimapInsert(&config, "timeout", 7, "30", 2);
multimapInsert(&config, "retries", 7, "3", 1);
// ~50 bytes total
```

### Pattern 2: Growing Cache

```c
// Starts Small, grows to Medium/Full as needed
multimap *cache = multimapNew(2);
for (each request) {
    multimapInsertOrUpdate(&cache, key, klen, value, vlen);
    // Transparent upgrades
}
```

### Pattern 3: Large-Scale Index

```c
// Skip Small/Medium, go straight to Full
multimap *index = multimapNew(2);
multimapReserve(&index, 1000000);  // Hint: will be large
// Efficient from the start
```

## See Also

- [multimap Variants](../modules/multimap/VARIANTS.md) - Detailed multimap variant documentation
- [multilist Variants](../modules/multilist/VARIANTS.md) - Detailed multilist variant documentation
- [multiarray Variants](../modules/multiarray/VARIANTS.md) - Detailed multiarray variant documentation
- [PERFORMANCE.md](PERFORMANCE.md) - Performance optimization guidelines
- [MEMORY.md](MEMORY.md) - Memory efficiency patterns
