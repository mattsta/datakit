# intersectInt - High-Performance Sorted Integer Set Intersection

## Overview

`intersectInt` provides **state-of-the-art algorithms for computing set intersections of sorted integer arrays**. It implements multiple SIMD-optimized strategies and automatically selects the best algorithm based on input characteristics, achieving significantly better performance than naive scalar approaches.

**Key Features:**
- Multiple specialized intersection algorithms
- Automatic algorithm selection based on size ratios
- SSE4.1 and AVX2 SIMD optimizations
- Galloping search for vastly different set sizes
- Designed by Nathan Kurz and Daniel Lemire
- Used in production for integer set operations

**Header**: `intersectInt.h`

**Source**: `intersectInt.c`

**Origin**: Modified from [SIMDCompressionAndIntersection](https://github.com/lemire/SIMDCompressionAndIntersection) by Daniel Lemire (Apache License 2.0)

## Algorithms Implemented

The library includes six specialized intersection algorithms:

```
┌─────────────────────────────────────────────┐
│         intersectInt (main entry)           │
│    Heuristic-based algorithm selector       │
└────────────────┬────────────────────────────┘
                 │
    ┌────────────┴────────────┐
    │   Size Ratio Analysis   │
    └────────────┬────────────┘
                 │
         ┌───────┴───────┬───────────┬──────────┐
         │               │           │          │
    ratio ≥ 1000   ratio ≥ 50   ratio < 50   Small
         │               │           │        inputs
         v               v           v          │
   SIMDgalloping        v3          v1      scalar
   (exponential)    (32-vector)  (2-vector)   │
                                               v
                                          match_scalar
```

### 1. intersectInt (Main Entry Point)

**Adaptive intersection** that analyzes input sizes and selects optimal algorithm:

```c
size_t intersectInt(const uint32_t *set1, const size_t length1,
                    const uint32_t *set2, const size_t length2,
                    uint32_t *out);
```

**Selection Logic:**
- If size ratio ≥ 1000:1 → Use `SIMDgalloping()` (exponential search)
- If size ratio ≥ 50:1 → Use `v3()` (32-way SIMD)
- Otherwise → Use `v1()` (2-way SIMD)
- Always ensures smaller set is "rare" parameter

### 2. intersectIntAVX2 (AVX2 Optimized)

**AVX2 version** using 256-bit vector operations (2x throughput of SSE):

```c
#if __AVX2__
size_t intersectIntAVX2(const uint32_t *set1, const size_t length1,
                        const uint32_t *set2, const size_t length2,
                        uint32_t *out);
#endif
```

Available only when compiled with AVX2 support (`__AVX2__` defined).

### 3. intersectIntAuto (Platform-Adaptive Macro)

**Automatically selects best version** for current platform:

```c
#if __AVX2__
#define intersectIntAuto(a, b, c, d, e) intersectIntAVX2(a, b, c, d, e)
#else
#define intersectIntAuto(a, b, c, d, e) intersectInt(a, b, c, d, e)
#endif
```

Use this for optimal performance across different CPU architectures.

### 4. intersectIntOneSidedGalloping

**Galloping search algorithm** optimized for vastly different set sizes:

```c
size_t intersectIntOneSidedGalloping(const uint32_t *smallset,
                                     const size_t smalllength,
                                     const uint32_t *largeset,
                                     const size_t largelength,
                                     uint32_t *out);
```

**Algorithm**: Exponential search (galloping) in large set, linear scan in small set.

**Best for**: Size ratios > 100:1

## API Reference

### Basic Usage

```c
/* Compute intersection of two sorted arrays
 * set1: first sorted array
 * length1: number of elements in set1
 * set2: second sorted array
 * length2: number of elements in set2
 * out: output buffer (must have space for min(length1, length2) elements)
 * Returns: number of elements in intersection
 */
size_t intersectInt(const uint32_t *set1, const size_t length1,
                    const uint32_t *set2, const size_t length2,
                    uint32_t *out);

/* Example */
uint32_t a[] = {1, 3, 5, 7, 9, 11, 13, 15};
uint32_t b[] = {2, 3, 5, 8, 11, 13, 17};
uint32_t result[7];  /* Max possible size = min(8, 7) */

size_t count = intersectInt(a, 8, b, 7, result);
/* count = 4, result = {3, 5, 11, 13} */
```

### Platform-Optimized Usage

```c
/* Use platform-specific optimizations automatically */
size_t count = intersectIntAuto(set1, len1, set2, len2, out);

/* Equivalent to: */
#if __AVX2__
size_t count = intersectIntAVX2(set1, len1, set2, len2, out);
#else
size_t count = intersectInt(set1, len1, set2, len2, out);
#endif
```

### Manual Algorithm Selection

```c
/* Force galloping algorithm for known large size difference */
size_t count = intersectIntOneSidedGalloping(
    smallSet, smallLen,
    largeSet, largeLen,
    out
);

/* Example: 1000:1 ratio */
uint32_t small[100];     /* 100 elements */
uint32_t large[100000];  /* 100,000 elements */
uint32_t result[100];    /* At most 100 results */

/* Fill arrays with sorted data... */

size_t count = intersectIntOneSidedGalloping(
    small, 100,
    large, 100000,
    result
);
```

## Real-World Examples

### Example 1: Database Index Intersection

```c
/* Intersect two database index scans */
typedef struct indexScan {
    uint32_t *rowIds;
    size_t count;
} indexScan;

indexScan *intersectScans(indexScan *scan1, indexScan *scan2) {
    /* Allocate result buffer */
    size_t maxSize = scan1->count < scan2->count ?
                     scan1->count : scan2->count;
    uint32_t *resultIds = malloc(maxSize * sizeof(uint32_t));

    /* Compute intersection */
    size_t count = intersectIntAuto(
        scan1->rowIds, scan1->count,
        scan2->rowIds, scan2->count,
        resultIds
    );

    /* Shrink to actual size */
    resultIds = realloc(resultIds, count * sizeof(uint32_t));

    indexScan *result = malloc(sizeof(*result));
    result->rowIds = resultIds;
    result->count = count;

    return result;
}

/* Usage: SELECT * FROM users WHERE age > 25 AND city = 'NYC' */
indexScan *ageIndex = scanIndex("age", ">", 25);    /* 10,000 rows */
indexScan *cityIndex = scanIndex("city", "=", "NYC"); /* 500 rows */

indexScan *matches = intersectScans(ageIndex, cityIndex);
printf("Found %zu matching rows\n", matches->count);
```

### Example 2: Inverted Index Search

```c
/* Search engine posting list intersection */
typedef struct postingList {
    uint32_t *docIds;  /* Sorted document IDs */
    size_t count;
} postingList;

postingList *searchMultiTermAND(const char **terms, size_t numTerms) {
    if (numTerms == 0) {
        return NULL;
    }

    /* Get posting list for first term */
    postingList *result = getPostingList(terms[0]);

    /* Intersect with each additional term */
    for (size_t i = 1; i < numTerms; i++) {
        postingList *next = getPostingList(terms[i]);

        uint32_t *intersected = malloc(result->count * sizeof(uint32_t));
        size_t count = intersectIntAuto(
            result->docIds, result->count,
            next->docIds, next->count,
            intersected
        );

        /* Replace result with intersection */
        free(result->docIds);
        result->docIds = intersected;
        result->count = count;

        freePostingList(next);

        /* Early exit if no matches */
        if (count == 0) {
            break;
        }
    }

    return result;
}

/* Usage: Search for "fast SIMD intersection" */
const char *terms[] = {"fast", "SIMD", "intersection"};
postingList *docs = searchMultiTermAND(terms, 3);
printf("Found in %zu documents\n", docs->count);
```

### Example 3: Integer Set Operations

```c
/* Implement set intersection using intsetU32 */
#include "intsetU32.h"
#include "intersectInt.h"

intsetU32 *intsetIntersect(const intsetU32 *a, const intsetU32 *b) {
    /* Get array pointers and counts */
    const uint32_t *aArray = intsetU32Array(a);
    const uint32_t *bArray = intsetU32Array(b);
    size_t aCount = intsetU32Count(a);
    size_t bCount = intsetU32Count(b);

    /* Allocate result set with max possible size */
    size_t maxSize = aCount < bCount ? aCount : bCount;
    intsetU32 *result = intsetU32NewLen(maxSize);

    /* Compute intersection directly into result array */
    size_t count = intersectIntAuto(
        aArray, aCount,
        bArray, bCount,
        intsetU32Array(result)
    );

    /* Update count and shrink to size */
    intsetU32UpdateCount(result, count);
    intsetU32ShrinkToSize(&result);

    return result;
}

/* Usage */
intsetU32 *setA = intsetU32New();
intsetU32 *setB = intsetU32New();

/* Add elements to sets... */
for (uint32_t i = 0; i < 1000; i += 2) {
    intsetU32Add(&setA, i);  /* Even numbers */
}
for (uint32_t i = 0; i < 1000; i += 3) {
    intsetU32Add(&setB, i);  /* Multiples of 3 */
}

/* Intersect: numbers divisible by both 2 and 3 (i.e., by 6) */
intsetU32 *intersection = intsetIntersect(setA, setB);
printf("Intersection size: %u\n", intsetU32Count(intersection));
/* Result: {0, 6, 12, 18, ..., 996} = 167 elements */
```

### Example 4: Multi-Way Intersection

```c
/* Intersect N sorted arrays efficiently */
uint32_t *intersectMultiple(uint32_t **arrays, size_t *lengths,
                            size_t numArrays, size_t *outCount) {
    if (numArrays == 0) {
        *outCount = 0;
        return NULL;
    }

    if (numArrays == 1) {
        uint32_t *result = malloc(lengths[0] * sizeof(uint32_t));
        memcpy(result, arrays[0], lengths[0] * sizeof(uint32_t));
        *outCount = lengths[0];
        return result;
    }

    /* Start with first two arrays */
    size_t maxSize = lengths[0] < lengths[1] ? lengths[0] : lengths[1];
    uint32_t *result = malloc(maxSize * sizeof(uint32_t));
    size_t count = intersectIntAuto(
        arrays[0], lengths[0],
        arrays[1], lengths[1],
        result
    );

    /* Intersect with remaining arrays */
    for (size_t i = 2; i < numArrays && count > 0; i++) {
        uint32_t *temp = malloc(count * sizeof(uint32_t));
        size_t newCount = intersectIntAuto(
            result, count,
            arrays[i], lengths[i],
            temp
        );

        free(result);
        result = temp;
        count = newCount;
    }

    /* Shrink to actual size */
    result = realloc(result, count * sizeof(uint32_t));
    *outCount = count;
    return result;
}

/* Usage: Intersect 5 different index scans */
uint32_t *scans[5];
size_t scanLengths[5];

/* Fill scan results... */

size_t resultCount;
uint32_t *matches = intersectMultiple(scans, scanLengths, 5, &resultCount);
```

## Algorithm Details

### Scalar Algorithm (match_scalar)

**Fallback algorithm** for small inputs or tail processing:

```
Algorithm: Two-pointer merge
1. Compare elements at current positions
2. If equal: add to output, advance both pointers
3. If set1[i] < set2[j]: advance i
4. If set1[i] > set2[j]: advance j
5. Repeat until either array exhausted

Time: O(n + m)
Space: O(1)
```

### v1 Algorithm (N. Kurz)

**2-way SIMD algorithm** for similar-sized sets (ratio < 50:1):

```
Algorithm: SIMD comparison with 2x4 element blocks
1. Load 2 vectors of 4 elements from freq array (8 total)
2. Broadcast single element from rare array
3. Compare rare element against all 8 freq elements using SIMD
4. If match found, output element
5. Advance rare pointer, reload freq if needed

Processes: 8 elements from freq per rare element
SIMD ops: SSE4.1 _mm_cmpeq_epi32, _mm_or_si128
```

### v3 Algorithm (N. Kurz + D. Lemire)

**32-way SIMD algorithm** for moderately skewed sets (50:1 ≤ ratio < 1000:1):

```
Algorithm: SIMD comparison with 32-element freq blocks
1. Load 32 elements (8 vectors × 4 elements) from freq
2. For each rare element:
   - Binary search within 32-element block using max values
   - Compare against relevant 16-element sub-block
   - Further narrow to 8-element sub-block
   - SIMD compare against 4 vectors
3. Advance freq by 32 elements when block exhausted

Processes: 32 elements from freq per scan
Reduces: 32→16→8→4 comparisons via binary search
```

### SIMDgalloping Algorithm

**Exponential search** for highly skewed sets (ratio ≥ 1000:1):

```
Algorithm: Galloping + Binary Search + SIMD
1. For each rare element:
   a. If current freq block max < rare: gallop forward
      - Try offsets: 1, 2, 4, 8, 16, ... (exponential)
      - Stop when freq[offset×32 + 31] ≥ rare
   b. Binary search to find exact 32-element block
   c. SIMD compare within block (like v3)
2. Output matches

Galloping: O(log(gap)) to skip large gaps
Binary search: O(log(skipped)) to refine
SIMD: O(1) comparison within block
```

### Heuristic Selection

The main `intersectInt()` function uses these heuristics:

```c
ratio = max(length1, length2) / min(length1, length2)

if (ratio >= 1000) {
    use SIMDgalloping  /* Exponential search */
} else if (ratio >= 50) {
    use v3             /* 32-way SIMD */
} else {
    use v1             /* 2-way SIMD */
}

/* Always ensure smaller array is "rare" parameter */
```

These thresholds were determined empirically by D. Lemire through extensive benchmarking.

## Performance Characteristics

### Time Complexity

| Algorithm | Best Case | Average Case | Worst Case | Notes |
|-----------|-----------|--------------|------------|-------|
| scalar | O(n+m) | O(n+m) | O(n+m) | Simple merge |
| v1 | O(n+m/8) | O(n+m/4) | O(n+m) | 2×4 SIMD |
| v3 | O(n+m/32) | O(n+m/16) | O(n+m) | 32-way SIMD |
| SIMDgalloping | O(n×log(m/n)) | O(n×log(m/n)) | O(n+m) | Exponential skip |
| galloping | O(n×log(m/n)) | O(n×log(m/n)) | O(n×log(m)) | No SIMD |

Where n = size of smaller set, m = size of larger set

### Speedup vs Scalar

Empirical results from original benchmarks (D. Lemire):

| Size Ratio | Algorithm | Speedup | Notes |
|------------|-----------|---------|-------|
| 1:1 | v1 | 3-4× | Similar sizes |
| 10:1 | v1 | 3-5× | Moderate skew |
| 50:1 | v3 | 5-8× | High skew |
| 100:1 | v3 | 6-10× | Very high skew |
| 1000:1 | SIMDgalloping | 10-50× | Extreme skew |

**AVX2 versions** provide additional 1.5-2× speedup over SSE versions.

### Space Complexity

All algorithms use **O(1) extra space** beyond the output buffer:
- No temporary arrays
- In-place pointer manipulation
- SIMD registers (128-bit SSE or 256-bit AVX2)

Output buffer must be allocated by caller: `min(length1, length2)` elements.

### Cache Behavior

- **Sequential access** patterns optimize for cache line utilization
- **Prefetching** via SIMD loads improves throughput
- **Small blocks** (32 elements = 128 bytes) fit in L1 cache

## SIMD Instructions Used

### SSE4.1 Instructions

```c
_mm_loadu_si128()      /* Load 128-bit vector (4×uint32_t) */
_mm_cmpeq_epi32()      /* Compare 4×uint32_t for equality */
_mm_or_si128()         /* Bitwise OR of comparison masks */
_mm_testz_si128()      /* Test if all bits zero (no matches) */
_mm_movemask_epi8()    /* Extract comparison mask to int */
_mm_shuffle_epi32()    /* Permute elements within vector */
_mm_shuffle_epi8()     /* Byte-level shuffle (pack matches) */
```

### AVX2 Instructions

```c
_mm256_loadu_si256()   /* Load 256-bit vector (8×uint32_t) */
_mm256_cmpeq_epi32()   /* Compare 8×uint32_t for equality */
_mm256_or_si256()      /* Bitwise OR (256-bit) */
_mm256_testz_si256()   /* Test if all bits zero (256-bit) */
_mm256_set1_epi32()    /* Broadcast scalar to 8×uint32_t */
```

## Best Practices

### 1. Ensure Inputs Are Sorted

```c
/* REQUIRED - inputs must be sorted ascending */
uint32_t sorted1[] = {1, 3, 5, 7};    /* OK */
uint32_t sorted2[] = {2, 3, 6, 7};    /* OK */

uint32_t unsorted[] = {3, 1, 7, 5};   /* WRONG - undefined behavior */
```

### 2. Allocate Sufficient Output Buffer

```c
/* GOOD - allocate max possible size */
size_t maxSize = len1 < len2 ? len1 : len2;
uint32_t *out = malloc(maxSize * sizeof(uint32_t));
size_t count = intersectInt(set1, len1, set2, len2, out);

/* Optional: shrink to actual size */
out = realloc(out, count * sizeof(uint32_t));

/* BAD - buffer too small */
uint32_t out[10];  /* May overflow if intersection > 10 */
size_t count = intersectInt(set1, 1000, set2, 1000, out);
```

### 3. Use intersectIntAuto for Portability

```c
/* GOOD - automatically uses AVX2 if available */
size_t count = intersectIntAuto(a, lenA, b, lenB, out);

/* AVOID - manual platform detection */
#if __AVX2__
size_t count = intersectIntAVX2(a, lenA, b, lenB, out);
#else
size_t count = intersectInt(a, lenA, b, lenB, out);
#endif
```

### 4. Don't Reuse Input as Output

```c
/* WRONG - aliasing not supported */
size_t count = intersectInt(set1, len1, set2, len2, set1);

/* RIGHT - use separate output buffer */
uint32_t *out = malloc(len1 * sizeof(uint32_t));
size_t count = intersectInt(set1, len1, set2, len2, out);
```

### 5. Handle Empty Sets

```c
/* Library handles empty inputs gracefully */
uint32_t a[10] = {...};
uint32_t b[0];  /* Empty */
uint32_t out[10];

size_t count = intersectInt(a, 10, b, 0, out);
/* Returns 0, out is unmodified */

/* But still allocate output buffer! */
```

## Common Pitfalls

### 1. Unsorted Input

```c
/* WRONG - produces incorrect results */
uint32_t a[] = {5, 1, 3};  /* Not sorted */
uint32_t b[] = {1, 3, 5};
uint32_t out[3];
size_t count = intersectInt(a, 3, b, 3, out);
/* Undefined behavior - may return 0, 1, 2, or 3 */

/* RIGHT - sort first */
qsort(a, 3, sizeof(uint32_t), compareUint32);
size_t count = intersectInt(a, 3, b, 3, out);
```

### 2. Integer Overflow in Output

```c
/* WRONG - output buffer on stack may overflow */
uint32_t *large1 = malloc(1000000 * sizeof(uint32_t));
uint32_t *large2 = malloc(1000000 * sizeof(uint32_t));
uint32_t out[100];  /* Too small! */

size_t count = intersectInt(large1, 1000000, large2, 1000000, out);
/* Buffer overflow if intersection > 100 */

/* RIGHT - allocate sufficient space */
uint32_t *out = malloc(1000000 * sizeof(uint32_t));
```

### 3. Ignoring Return Value

```c
/* WRONG - don't know actual size */
uint32_t out[1000];
intersectInt(a, lenA, b, lenB, out);
for (size_t i = 0; i < 1000; i++) {
    printf("%u\n", out[i]);  /* May print garbage */
}

/* RIGHT - use return value */
size_t count = intersectInt(a, lenA, b, lenB, out);
for (size_t i = 0; i < count; i++) {
    printf("%u\n", out[i]);
}
```

### 4. Using Wrong Type

```c
/* WRONG - requires uint32_t */
int a[] = {1, 2, 3};  /* Wrong type */
size_t count = intersectInt((uint32_t *)a, 3, b, lenB, out);
/* May work on some platforms, undefined on others */

/* RIGHT - use uint32_t */
uint32_t a[] = {1, 2, 3};
size_t count = intersectInt(a, 3, b, lenB, out);
```

## Compiler Flags

### Required for AVX2

```bash
# Enable AVX2 instructions
gcc -mavx2 -O3 -o program program.c intersectInt.c

# Or in CMakeLists.txt
add_compile_options(-mavx2)
```

### Required for SSE4.1

```bash
# Enable SSE4.1 instructions (usually default on x86-64)
gcc -msse4.1 -O3 -o program program.c intersectInt.c
```

### Optimization Flags

```bash
# Recommended flags for maximum performance
gcc -O3 -march=native -mtune=native \
    -ffast-math -funroll-loops \
    -o program program.c intersectInt.c
```

## Platform Support

| Platform | SSE Version | AVX2 Version | Notes |
|----------|-------------|--------------|-------|
| x86-64 | ✓ | ✓ | Full support |
| x86 (32-bit) | ✓ | ✓ | Requires SSE4.1/AVX2 CPU |
| ARM | ✗ | ✗ | Falls back to scalar |
| RISC-V | ✗ | ✗ | Falls back to scalar |

**Note**: On non-x86 platforms, the code will compile but use only scalar algorithms.

## Benchmarking

Example benchmark code:

```c
#include <time.h>

void benchmarkIntersection(void) {
    const size_t N = 1000000;
    uint32_t *a = malloc(N * sizeof(uint32_t));
    uint32_t *b = malloc(N * sizeof(uint32_t));
    uint32_t *out = malloc(N * sizeof(uint32_t));

    /* Fill with sorted random data */
    for (size_t i = 0; i < N; i++) {
        a[i] = i * 2;      /* Even numbers */
        b[i] = i * 3;      /* Multiples of 3 */
    }

    clock_t start = clock();
    size_t count = intersectIntAuto(a, N, b, N, out);
    clock_t end = clock();

    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
    double throughput = (N + N) / elapsed / 1e6;  /* Million elements/sec */

    printf("Intersection size: %zu\n", count);
    printf("Time: %.3f seconds\n", elapsed);
    printf("Throughput: %.1f M elements/sec\n", throughput);

    free(a); free(b); free(out);
}
```

## See Also

- [intsetU32](../intset/INTSET_U32.md) - Fixed 32-bit integer set (uses this module)
- [intsetBig](../intset/INTSET.md) - Variable-width integer set
- [multimap](../multimap/MULTIMAP.md) - Sorted key-value store

## References

- Original implementation: [SIMDCompressionAndIntersection](https://github.com/lemire/SIMDCompressionAndIntersection)
- Algorithm designer: Nathan Kurz
- Adaptation: Daniel Lemire
- Paper: "Faster Population Counts using AVX2 Instructions" (Lemire et al.)
- License: Apache License 2.0

## Implementation Notes

The implementation uses several advanced techniques:

1. **Pointer tagging** for algorithm dispatch
2. **Loop unrolling** for SIMD efficiency
3. **Branch prediction hints** via `__builtin_expect`
4. **Alignment assumptions** for faster loads (configurable)
5. **Shuffle masks** for efficient result packing
6. **Galloping search** for skipping large gaps

The code is heavily optimized for modern x86-64 CPUs with out-of-order execution and deep pipelines.
