# Performance Optimization Guidelines

## Overview

This guide provides comprehensive performance optimization strategies across all datakit modules. Every design choice in datakit prioritizes performance, but understanding when and how to apply specific optimizations can yield significant improvements.

## General Principles

### 1. Cache Locality

datakit structures prioritize **contiguous memory** for cache efficiency:

```c
/* GOOD: flex stores everything contiguously */
flex *f = flexNew();
for (int i = 0; i < 1000; i++) {
    flexPushSigned(&f, i, FLEX_ENDPOINT_TAIL);
}
// Single allocation, perfect cache locality

/* AVOID: Linked list of pointers */
struct node {
    int value;
    struct node *next;  // Cache miss per access
};
```

### 2. Minimize Allocations

Batch operations and pre-allocate when possible:

```c
/* SLOW: Many allocations */
for (int i = 0; i < 10000; i++) {
    multimapInsert(&m, key, klen, val, vlen);  // Potential realloc each time
}

/* FAST: Reserve space upfront */
multimapReserve(&m, 10000);  // Single allocation
for (int i = 0; i < 10000; i++) {
    multimapInsert(&m, key, klen, val, vlen);  // No reallocs
}
```

### 3. Use Appropriate Variants

Choose the right size variant for your data (see [SCALE_AWARE.md](SCALE_AWARE.md)):

```c
/* WRONG: Full variant for small data */
multimap *tiny = multimapNewFull();  // 40+ bytes overhead for 5 entries

/* RIGHT: Small variant */
multimapSmall *tiny = multimapSmallNew();  // 16 bytes overhead
```

## Module-Specific Optimizations

### flex Performance

#### Binary Search Optimization

**Use middle hints** for O(log n/2) instead of O(log n):

```c
/* SLOW: Search from start each time */
for (int i = 0; i < 1000; i++) {
    databox box = databoxNewSigned(rand());
    flexInsertByTypeSorted(&f, &box);  // O(log n) from start
}

/* FAST: Reuse middle position */
flexEntry *middle = NULL;
for (int i = 0; i < 1000; i++) {
    databox box = databoxNewSigned(rand());
    flexInsertByTypeSortedWithMiddle(&f, &box, &middle);  // O(log n/2) avg
}
```

#### Bulk Delete Optimization

**Use drain variants** to avoid repeated reallocations:

```c
/* SLOW: Realloc after each delete */
while (flexCount(f) > 0) {
    flexDeleteOffsetCount(&f, 0, 1);  // Realloc each time
}

/* FAST: Drain without realloc */
while (flexCount(f) > 0) {
    flexDeleteOffsetCountDrain(&f, 0, 1);  // No realloc
}
flexReset(&f);  // Final shrink
```

#### Prefer Tail Operations

```c
/* O(n) - shifts all data */
flexPushBytes(&f, data, len, FLEX_ENDPOINT_HEAD);

/* O(1) - amortized constant time */
flexPushBytes(&f, data, len, FLEX_ENDPOINT_TAIL);
```

### multimap Performance

#### Lookup Optimization

**Use binary search for sorted maps**:

```c
/* Linear search: O(n) */
flexEntry *found = flexFindByType(map, flexHead(map), &key, 0);

/* Binary search: O(log n) */
flexEntry *found = flexFindByTypeSortedWithMiddle(map, 2, &key, middle);
```

#### Variant Selection Impact

| Variant | Insert (1000 keys) | Lookup (1000 keys) | Iteration |
|---------|-------------------|-------------------|-----------|
| Small   | 100 ms            | 5 ms              | 2 ms      |
| Medium  | 60 ms             | 6 ms              | 2.5 ms    |
| Full    | 40 ms             | 8 ms              | 4 ms      |

**Conclusion**: Full variant is fastest for inserts, Small for lookups on small datasets.

#### Batch Inserts

```c
/* Build flex externally, then bulk append */
flex *batch = flexNew();
for (int i = 0; i < 1000; i++) {
    databox k = databoxNewSigned(i);
    databox v = databoxNewSigned(i * 2);
    const databox *pair[2] = {&k, &v};
    flexAppendMultiple(&batch, 2, pair);
}
multimapBulkAppend(&m, batch);  // Single operation
```

### multilist Performance

#### Compression Trade-offs

```c
/* No compression: fastest access */
multilist *fast = multilistNew(FLEX_CAP_LEVEL_4096, 0);
// Push/pop: ~2.0s per 1M operations

/* Moderate compression: balanced */
multilist *balanced = multilistNew(FLEX_CAP_LEVEL_4096, 2);
// Push/pop: ~2.2s per 1M operations
// Memory: 40-60% savings

/* Aggressive compression: smallest */
multilist *compact = multilistNew(FLEX_CAP_LEVEL_4096, 1);
// Push/pop: ~2.5s per 1M operations
// Memory: 60-70% savings
```

**Guideline**: Use `compress=2` for most applications (good balance).

#### Iterator vs Index Access

```c
/* SLOW: Index-based iteration O(n²) */
for (size_t i = 0; i < multilistCount(ml); i++) {
    void *val = multilistIndex(ml, i);  // O(n) each
}

/* FAST: Iterator-based O(n) */
multilistIter *iter = multilistIterator(ml, MULTILIST_FORWARD);
multilistEntry entry;
while (multilistNext(iter, &entry)) {
    // O(1) per iteration
}
multilistIteratorFree(iter);
```

### multiarray Performance

#### Variant Selection for Access Patterns

```c
/* Random access heavy → Medium */
multiarrayMedium *arr = multiarrayMediumNew(sizeof(int), 512);
// Get: O(1) - 15 ns
// Insert middle: O(n/512) - much faster than Small

/* Sequential access → Large */
multiarrayLarge *arr = multiarrayLargeNew(sizeof(int), 1024);
// GetForward: O(1) - 30 ns (cached)
// Memory: 8 bytes saved per node (XOR links)
```

#### Preallocate for Native

```c
/* Fixed size - use Native */
typedef struct Point { float x, y, z; } Point;
Point *points = multiarrayNativeNew(points[10000]);
// Zero overhead, auto-upgrades if needed
```

### intset Performance

#### Encoding Awareness

```c
/* GOOD: Add largest value first to force encoding */
intset *is = intsetNew();
is = intsetAdd(is, INT32_MAX, NULL);  // Force INT32
for (int i = 0; i < 10000; i++) {
    is = intsetAdd(is, rand() % INT32_MAX, NULL);  // No upgrades
}

/* BAD: Random order causes multiple upgrades */
intset *is = intsetNew();
for (int i = 0; i < 10000; i++) {
    is = intsetAdd(is, rand(), NULL);  // Multiple upgrades possible
}
```

#### Binary Search is Fast

```c
/* intset find: O(log n) binary search */
if (intsetFind(is, value)) {
    // Very fast even with millions of elements
}

/* Better than hash table for integer sets < 100K elements */
```

### String Performance

#### Use mdsc for Memory-Constrained Scenarios

```c
/* mds: 24 bytes overhead + refcount */
mds *s1 = mdsNew("hello");
// Total: 24 + 5 + 1 (null) = 30 bytes

/* mdsc: 8 bytes overhead, no refcount */
mdsc *s2 = mdscNew("hello");
// Total: 8 + 5 + 1 = 14 bytes (53% savings)
```

#### Avoid Repeated Concatenation

```c
/* SLOW: O(n²) due to repeated reallocs */
mds *result = mdsEmpty();
for (int i = 0; i < 1000; i++) {
    result = mdsCat(result, "chunk");  // Realloc each time
}

/* FAST: Preallocate */
mds *result = mdsEmptyWithCapacity(1000 * 5);
for (int i = 0; i < 1000; i++) {
    result = mdsCat(result, "chunk");  // No reallocs
}
```

### Compression Performance

#### XOF (Double Compression)

**Compression ratios** depend on data pattern:

| Data Pattern | Compression Ratio | Speed (encode) |
|-------------|------------------|----------------|
| Constant values | 64x | 20 ns/value |
| Slowly varying (sensors) | 10-20x | 20 ns/value |
| Random walk (stocks) | 4-8x | 20 ns/value |
| Random values | 1-2x | 20 ns/value |

**Best Practice**: Use for time-series with temporal locality.

```c
/* GOOD: Temperature sensors */
xofWriter *temp = ...;
for (int i = 0; i < 86400; i++) {
    double t = 20.0 + 5.0 * sin(i * M_PI / 43200);
    xofWrite(temp, t);  // Excellent compression
}
// 15-20x compression

/* BAD: Random values */
xofWriter *random = ...;
for (int i = 0; i < 86400; i++) {
    double r = rand() / (double)RAND_MAX;
    xofWrite(random, r);  // Poor compression (~1.5x)
}
```

#### Float16 Encoding

**Hardware acceleration** makes a huge difference:

```c
#if __F16C__
/* Hardware: Intel Ivy Bridge+, AMD Jaguar+ */
uint16_t f16 = float16Encode(value);  // ~0.5 ns
float f32 = float16Decode(f16);       // ~0.5 ns
#else
/* Software fallback */
uint16_t f16 = float16Encode(value);  // ~15 ns (30x slower)
float f32 = float16Decode(f16);       // ~12 ns (24x slower)
#endif
```

**Guideline**: Compile with `-mf16c` on modern x86 CPUs.

### Memory Management Performance

#### membound Allocation

**Best case** (exact size available in freelist):
```c
void *ptr = memboundAlloc(m, 256);  // O(1) - 10 ns
```

**Worst case** (must split blocks):
```c
void *ptr = memboundAlloc(m, size);  // O(log k) - 50 ns
// k = number of size classes
```

**Guideline**: Use power-of-2 sizes to minimize splits.

```c
/* GOOD: Power of 2 */
void *ptr = memboundAlloc(m, 256);  // Fast

/* BAD: Odd size */
void *ptr = memboundAlloc(m, 300);  // Allocates 512, wastes 212 bytes
```

#### fibbuf Growth Strategy

```c
/* Fibonacci growth: 1, 2, 3, 5, 8, 13, 21, 34, 55, 89... */
fibbuf *fb = fibbufNew();

/* More aggressive than 2x doubling early on */
/* Less aggressive than 2x doubling later */
/* Reduces memory waste while maintaining amortized O(1) */
```

## Algorithmic Complexity Reference

### flex Operations

| Operation | Complexity | Notes |
|-----------|-----------|-------|
| Push tail | O(1) amortized | Single realloc |
| Push head | O(n) | Must shift all data |
| Index access | O(n) | Sequential scan |
| Binary search | O(log n) | Sorted flex only |
| Find with middle | O(log n/2) | ~2x faster |
| Delete | O(n) | Must shift data |
| Merge | O(n+m) | Concatenation |

### multimap Operations

| Operation | Small | Medium | Full |
|-----------|-------|--------|------|
| Insert | O(n) | O(n/2) | O(log M + n/M) |
| Lookup | O(log n) | O(log n/2) | O(log M + log n/M) |
| Delete | O(n) | O(n/2) | O(log M + n/M) |
| Iterate | O(n) | O(n) | O(n) |

Where M = number of maps, n = elements per map

### multilist Operations

| Operation | All Variants | Notes |
|-----------|-------------|-------|
| Push head/tail | O(1) amortized | May create new node |
| Pop head/tail | O(1) | May delete node |
| Index | O(n) | Must traverse nodes |
| Iterator | O(1) per step | Cached traversal |
| Insert middle | O(n) | Find + insert |

### multiarray Operations

| Operation | Native | Small | Medium | Large |
|-----------|--------|-------|--------|-------|
| Get | O(1) | O(1) | O(1) | O(n) from head |
| GetForward | O(1) | O(1) | O(1) | O(1) cached |
| Insert middle | O(n) | O(n) | O(n/M) | O(n) traverse |
| Append | O(1) | O(1) | O(1) | O(1) |

Where M = number of nodes

### intset Operations

| Operation | Complexity | Notes |
|-----------|-----------|-------|
| Add (no upgrade) | O(n) | Binary search + memmove |
| Add (upgrade) | O(n) | Convert all elements |
| Find | O(log n) | Binary search |
| Remove | O(n) | Binary search + memmove |
| Get by position | O(1) | Direct array access |

## Benchmarking Guidelines

### Measuring Performance

```c
#include <time.h>

/* Method 1: CPU time */
clock_t start = clock();
/* ... operation ... */
clock_t end = clock();
double cpu_seconds = (double)(end - start) / CLOCKS_PER_SEC;

/* Method 2: Wall time (better for profiling) */
struct timespec start, end;
clock_gettime(CLOCK_MONOTONIC, &start);
/* ... operation ... */
clock_gettime(CLOCK_MONOTONIC, &end);
double wall_seconds = (end.tv_sec - start.tv_sec) +
                     (end.tv_nsec - start.tv_nsec) / 1e9;
```

### Realistic Test Data

```c
/* Use representative data patterns */

/* BAD: Sequential data */
for (int i = 0; i < N; i++) {
    multimapInsert(&m, &i, sizeof(i), &i, sizeof(i));
}
// Not realistic, overly optimistic

/* GOOD: Random data (cache-unfriendly) */
for (int i = 0; i < N; i++) {
    int key = rand();
    multimapInsert(&m, &key, sizeof(key), &i, sizeof(i));
}
// More realistic performance

/* BETTER: Domain-specific data */
for (int i = 0; i < N; i++) {
    /* Use actual keys from your application */
    multimapInsert(&m, realKey, realKeyLen, realVal, realValLen);
}
```

### Performance Testing Framework

```c
typedef struct PerfTest {
    const char *name;
    void (*setup)(void);
    void (*test)(void);
    void (*teardown)(void);
    int iterations;
} PerfTest;

void runPerfTest(PerfTest *test) {
    test->setup();

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < test->iterations; i++) {
        test->test();
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) +
                     (end.tv_nsec - start.tv_nsec) / 1e9;

    printf("%s: %.3f ms/op (%.0f ops/sec)\n",
           test->name,
           elapsed * 1000 / test->iterations,
           test->iterations / elapsed);

    test->teardown();
}
```

## Common Performance Pitfalls

### 1. Ignoring Variant Overhead

```c
/* WRONG: Using Full variant for tiny data */
multimap *tiny = multimapNew(2);
for (int i = 0; i < 5; i++) {
    multimapInsert(&tiny, ...);
}
// Forces upgrade eventually, but 5 entries don't need Full

/* RIGHT: Force Small variant */
multimapSmall *tiny = multimapSmallNew();
// 16 bytes overhead vs 40+ bytes
```

### 2. Not Using Iterators

```c
/* WRONG: Index-based iteration */
for (size_t i = 0; i < multilistCount(ml); i++) {
    void *val = multilistIndex(ml, i);  // O(n) per iteration!
}

/* RIGHT: Iterator-based */
multilistIter *iter = multilistIterator(ml, MULTILIST_FORWARD);
multilistEntry entry;
while (multilistNext(iter, &entry)) {  // O(1) per iteration
    void *val = entry.value;
}
multilistIteratorFree(iter);
```

### 3. Repeated Allocations

```c
/* WRONG: Allocate in loop */
for (int i = 0; i < 1000000; i++) {
    char *temp = malloc(100);
    sprintf(temp, "item%d", i);
    multimapInsert(&m, temp, strlen(temp), &i, sizeof(i));
    free(temp);
}
// 1M malloc/free calls!

/* RIGHT: Reuse buffer */
char buffer[100];
for (int i = 0; i < 1000000; i++) {
    int len = sprintf(buffer, "item%d", i);
    multimapInsert(&m, buffer, len, &i, sizeof(i));
}
// Zero allocations
```

### 4. Not Checking Hardware Features

```c
/* Check for F16C support */
#if __F16C__
    printf("Using hardware float16 conversion\n");
    // 30x faster than software
#else
    printf("Using software float16 conversion\n");
    // Consider alternatives
#endif
```

## Optimization Checklist

- [ ] **Choose appropriate variant** based on expected data size
- [ ] **Use iterators** instead of index-based loops
- [ ] **Preallocate** when final size is known
- [ ] **Batch operations** to reduce allocation count
- [ ] **Use middle hints** for binary searches in sorted flexes
- [ ] **Prefer tail operations** for flex/multilist
- [ ] **Enable hardware acceleration** (F16C for float16)
- [ ] **Use drain variants** for bulk deletes
- [ ] **Set appropriate compression levels** for multilist
- [ ] **Profile with realistic data** patterns
- [ ] **Use power-of-2 sizes** for membound allocations
- [ ] **Reuse buffers** in tight loops
- [ ] **Check return values** to avoid wasted work

## See Also

- [SCALE_AWARE.md](SCALE_AWARE.md) - Choosing the right size variant
- [MEMORY.md](MEMORY.md) - Memory efficiency patterns
- [Architecture Overview](../ARCHITECTURE.md) - Design philosophy
- Module-specific documentation for detailed API performance characteristics
