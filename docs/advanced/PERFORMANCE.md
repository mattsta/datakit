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

There is no reserve/capacity API on multimap. What you control is the
per-flex size limit chosen at construction, and whether each write has
to search for a sorted position:

```c
/* SLOWER: every insert binary-searches for its sorted position */
for (int i = 0; i < 10000; i++) {
    multimapInsert(&m, entries[i]);
}

/* FASTER: input already ordered, so append and skip the search */
multimap *m = multimapNewLimit(2, FLEX_CAP_LEVEL_4096);
for (int i = 0; i < 10000; i++) {
    multimapAppend(&m, entries[i]);
}
```

A larger `FLEX_CAP_LEVEL_*` means fewer container splits but larger
memmoves inside each flex; measure both ends for your entry size.

### 3. Use Appropriate Variants

Choose the right size variant for your data (see [SCALE_AWARE.md](SCALE_AWARE.md)):

```c
/* Pin a variant only when you have a reason to */
multimapFull *big = multimapFullNew(2);

/* Normally: let the wrapper start small and retier itself */
multimap *m = multimapNew(2);
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
| ------- | ------------------ | ------------------ | --------- |
| Small   | 100 ms             | 5 ms               | 2 ms      |
| Medium  | 60 ms              | 6 ms               | 2.5 ms    |
| Full    | 40 ms              | 8 ms               | 4 ms      |

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

The depth argument of `multilistNewLimit(limit, depth)` is how many nodes at
each end stay uncompressed; every node between them is LZ4 compressed, and
depth 0 never compresses. Pushes and pops work at the ends, so a higher
depth only saves work when access spills past the end nodes.

```c
/* No compression: every node stays a plain flex */
multilist *fast = multilistNewLimit(FLEX_CAP_LEVEL_4096, 0);

/* Compress everything but the two nodes at each end */
multilist *balanced = multilistNewLimit(FLEX_CAP_LEVEL_4096, 2);

/* Compress everything but the end nodes */
multilist *compact = multilistNewLimit(FLEX_CAP_LEVEL_4096, 1);
```

Measured by `compressionBench`: 1,000,000 pushes to the tail, then 1,000,000
pops from the head, median of 7 rounds that interleave the depths.

| Data (pattern)                  | depth 0  | depth 2 | depth 1 |
| ------------------------------- | -------- | ------- | ------- |
| 64-bit integers `0, 1, 2, ...`  | 129 ms   | 143 ms  | 142 ms  |
| 100-byte structured log lines   | 495 ms   | 579 ms  | 573 ms  |
| Stored size, the same log lines | 104.1 MB | 24.9 MB | 24.9 MB |

These timings are from one machine: an Apple M1 Max (10 cores) on macOS
14.8, clang 22 with the `perf` preset (`-O3 -mcpu=native -flto`). Expect
other hardware to differ. Reproduce them with:

```sh
cmake --preset perf && cmake --build --preset perf
./build/perf/src/datakit-test test compressionBench
```

**Guideline**: for large lists, depth 1 and depth 2 store the same bytes,
since nearly every node is interior. Depth 2 keeps one more node at each
end uncompressed for workloads that read just past the ends. Depth 0 makes
pushes and pops 11-17% faster in the measurements above, at ~4x the memory
on text like the log lines. Incompressible nodes (like the integers) are
still tried with LZ4 at depth 1 or 2 and kept uncompressed, which is where
their extra time goes.

#### Iterator vs Index Access

```c
/* SLOW: Index-based iteration O(n²) */
multilistEntry entry;
for (size_t i = 0; i < multilistCount(ml); i++) {
    multilistIndexGet(ml, state, i, &entry);  // re-walks nodes each time
}

/* FAST: Iterator-based O(n).
 * The iterator is a caller-stack struct, and the init macros take an
 * ARRAY OF TWO mflexState pointers. */
mflexState *states[2] = {mflexStateCreate(), mflexStateCreate()};
multilistIterator iter;
multilistIteratorInitForwardReadOnly(ml, states, &iter);
while (multilistNext(&iter, &entry)) {
    // amortized O(1) per iteration
}
multilistIteratorRelease(&iter);
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

**Compression ratios** depend on how many low mantissa bits change between
values, not on how smooth the series looks. Measured with `xofWrite()`,
10,000 values per pattern:

| Data Pattern                        | Bits/Value | Ratio  |
| ----------------------------------- | ---------- | ------ |
| Constant                            | 1.0        | ~64x   |
| Smooth readings rounded to 0.1      | 4.7        | ~14x   |
| Integer counter `0, 1, 2, ...`      | 16         | 4x     |
| Random walk in cents                | 31         | ~2x    |
| Smooth `25 + sin(i/100)`, full bits | 55         | ~1.16x |
| Random values in [0, 1]             | 65         | ~0.99x |

For encode and decode speed, run the `compressionBench` test from a `perf`
preset build; sanitizer builds distort the timings.

**Best Practice**: Quantize time series to the precision you need before
writing them.

```c
#include "datakit.h"
#include "xof.h"

#include <math.h>
#include <stdlib.h>

void exampleSensorDay(void) {
    /* GOOD: A day of per-second temperatures kept to 0.1 degree:
     * measured 1.14 bits/value, ~56x */
    xofWriter rounded = {0};
    for (int i = 0; i < 86400; i++) {
        const double t = 20.0 + 5.0 * sin(i * M_PI / 43200);
        xofWrite(&rounded, round(t * 10.0) / 10.0);
    }

    /* POOR: The same curve at full precision: measured 50.7 bits/value,
     * ~1.26x, since the low mantissa bits change every sample */
    xofWriter full = {0};
    for (int i = 0; i < 86400; i++) {
        xofWrite(&full, 20.0 + 5.0 * sin(i * M_PI / 43200));
    }

    /* BAD: Random values: measured 64.9 bits/value, ~0.99x */
    xofWriter random = {0};
    for (int i = 0; i < 86400; i++) {
        xofWrite(&random, rand() / (double)RAND_MAX);
    }

    zfree(rounded.d);
    zfree(full.d);
    zfree(random.d);
}
```

#### Float16 Encoding

`float16Encode()` / `float16Decode()` use hardware conversion when the
build targets it: F16C (`_cvtss_sh`/`_cvtsh_ss`) on x86-64 with `-mf16c`,
NEON `vcvt` on AArch64. Everything else uses the portable bit-manipulation
functions `float16Encode_()` / `float16Decode_()`.

Measured by `compressionBench` over 4,000,000 values, median of 7
interleaved rounds, on an Apple M1 Max with clang 22 (`perf` preset):

| Path              | Encode        | Decode        |
| ----------------- | ------------- | ------------- |
| NEON hardware     | 0.38 ns/value | 0.99 ns/value |
| Portable software | 0.46 ns/value | 0.99 ns/value |

On this machine the compiler vectorizes the portable loops, so hardware
conversion gains little (1.2x encode, 1.0x decode) in bulk loops. F16C on
x86-64 has not been measured here; run the same bench on that hardware
before relying on a figure.

**Guideline**: build with `-mf16c` (or `-march=native`) on x86-64 so the
hardware path is available, and measure with the command in
[multilist Performance](#multilist-performance).

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

| Operation        | Complexity     | Notes                                                           |
| ---------------- | -------------- | --------------------------------------------------------------- |
| Push tail        | O(1) amortized | Single realloc                                                  |
| Push head        | O(n)           | Must shift all data                                             |
| Index access     | O(n)           | Sequential scan                                                 |
| Binary search    | O(log n)       | Sorted flex only                                                |
| Find with middle | O(log n)       | caller passes the middle entry, so the search skips locating it |
| Delete           | O(n)           | Must shift data                                                 |
| Merge            | O(n+m)         | Concatenation                                                   |

### multimap Operations

| Operation | Small    | Medium     | Full               |
| --------- | -------- | ---------- | ------------------ |
| Insert    | O(n)     | O(n/2)     | O(log M + n/M)     |
| Lookup    | O(log n) | O(log n/2) | O(log M + log n/M) |
| Delete    | O(n)     | O(n/2)     | O(log M + n/M)     |
| Iterate   | O(n)     | O(n)       | O(n)               |

Where M = number of maps, n = elements per map

### multilist Operations

| Operation      | All Variants   | Notes               |
| -------------- | -------------- | ------------------- |
| Push head/tail | O(1) amortized | May create new node |
| Pop head/tail  | O(1)           | May delete node     |
| Index          | O(n)           | Must traverse nodes |
| Iterator       | O(1) per step  | Cached traversal    |
| Insert middle  | O(n)           | Find + insert       |

### multiarray Operations

| Operation     | Native | Small | Medium | Large          |
| ------------- | ------ | ----- | ------ | -------------- |
| Get           | O(1)   | O(1)  | O(1)   | O(n) from head |
| GetForward    | O(1)   | O(1)  | O(1)   | O(1) cached    |
| Insert middle | O(n)   | O(n)  | O(n/M) | O(n) traverse  |
| Append        | O(1)   | O(1)  | O(1)   | O(1)           |

Where M = number of nodes

### intset Operations

| Operation        | Complexity | Notes                   |
| ---------------- | ---------- | ----------------------- |
| Add (no upgrade) | O(n)       | Binary search + memmove |
| Add (upgrade)    | O(n)       | Convert all elements    |
| Find             | O(log n)   | Binary search           |
| Remove           | O(n)       | Binary search + memmove |
| Get by position  | O(1)       | Direct array access     |

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
multilistEntry entry;

/* WRONG: Index-based iteration */
for (size_t i = 0; i < multilistCount(ml); i++) {
    multilistIndexGet(ml, state, i, &entry);  // re-walks nodes each time
}

/* RIGHT: Iterator-based */
mflexState *states[2] = {mflexStateCreate(), mflexStateCreate()};
multilistIterator iter;
multilistIteratorInitForwardReadOnly(ml, states, &iter);
while (multilistNext(&iter, &entry)) {  // amortized O(1)
    /* entry holds the decoded element */
}
multilistIteratorRelease(&iter);
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
/* Check which float16 path the build uses */
#if __F16C__ || defined(__aarch64__)
    printf("Using hardware float16 conversion\n");
    // Measure the gain on your hardware: see Float16 Encoding above
#else
    printf("Using software float16 conversion\n");
    // Build with -mf16c on x86-64 to enable the hardware path
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
