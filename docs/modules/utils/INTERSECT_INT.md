# intersectInt - High-Performance Sorted Integer Set Intersection

## Overview

`intersectInt` provides **state-of-the-art algorithms for computing set intersections of sorted integer arrays**. It implements multiple SIMD-optimized strategies and automatically selects the best algorithm based on input characteristics, achieving significantly better performance than naive scalar approaches.

**Key Features:**

- Multiple specialized intersection algorithms
- Automatic algorithm selection based on size ratios
- SSE4.1, AVX2, and **ARM NEON** SIMD implementations, plus a scalar fallback
- Galloping search for vastly different set sizes
- Output buffer may alias the smaller input array

**Header**: `intersectInt.h`

**Source**: `intersectInt.c` (also carries the `intersectInt` test suite under
`DATAKIT_TEST`)

### Platform dispatch happens at compile time

`intersectInt()` has three mutually exclusive definitions in `intersectInt.c`,
selected by the preprocessor:

| Build target                                      | `intersectInt()` dispatches to               |
| ------------------------------------------------- | -------------------------------------------- |
| x86 with `__SSE4_1__`                             | `SIMDgalloping` / `v3` / `v1`                |
| ARM (`__aarch64__`, `__ARM_NEON`, `__ARM_NEON__`) | `SIMDgalloping_neon` / `v3_neon` / `v1_neon` |
| anything else                                     | `scalar()` only, no ratio heuristic          |

There is no runtime CPU detection. The ratio heuristic is identical on the x86
and ARM paths; the scalar fallback skips it entirely because there is nothing to
choose between.

**Origin**: Modified from [SIMDCompressionAndIntersection](https://github.com/lemire/SIMDCompressionAndIntersection) by Daniel Lemire (Apache License 2.0)

## Algorithms Implemented

Three functions and one macro are public. Everything else named below is an
internal implementation selected for you.

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
      ┌──────────┴────────┬──────────────┐
      │                   │              │
 1000*min <= max     50*min <= max    otherwise
      │                   │              │
      v                   v              v
 SIMDgalloping           v3             v1
 (exponential)      (32-element)    (2-vector)
      │                   │              │
      └───────────────────┴──────────────┘
                          │
              tails handled by match_scalar
```

The three comparisons are exact integer tests on the lengths
(`1000 * length1 <= length2`, then `50 * length1 <= length2`, symmetric in both
directions) — no division is performed. Empty inputs short-circuit to 0 before
any of it runs, and whichever array is shorter is always passed as the `rare`
argument.

### 1. intersectInt (Main Entry Point)

**Adaptive intersection** that analyzes input sizes and selects optimal algorithm:

```c
size_t intersectInt(const uint32_t *set1, const size_t length1,
                    const uint32_t *set2, const size_t length2,
                    uint32_t *out);
```

**Selection Logic:**

- Either length 0 → return 0 immediately
- If size ratio ≥ 1000:1 → `SIMDgalloping()` (exponential search)
- If size ratio ≥ 50:1 → `v3()` (32-element blocks)
- Otherwise → `v1()` (2-vector)
- Always passes the smaller set as the "rare" parameter

On ARM builds the same three branches select the `_neon` variants; on a build
with neither SSE4.1 nor NEON, this function is a direct call to `scalar()`.

**Aliasing is permitted, in one direction:** `out` may be `set1` when
`length1 <= length2`, or `set2` when `length1 > length2`. In other words the
output may overwrite the _smaller_ input in place. Aliasing the larger input is
not supported.

### 2. intersectIntAVX2 (AVX2 Optimized)

**AVX2 version** using 256-bit vector operations (2x throughput of SSE):

```c
#if __AVX2__
size_t intersectIntAVX2(const uint32_t *set1, const size_t length1,
                        const uint32_t *set2, const size_t length2,
                        uint32_t *out);
#endif
```

**Both the declaration and the definition are guarded by `__AVX2__`.** On a build
without AVX2 this symbol does not exist at all — it is not merely slower, it is
absent, and a direct call will not compile. Use `intersectIntAuto` unless you
have already guarded the call site yourself.

### 3. intersectIntAuto (Platform-Adaptive Macro)

**Selects the AVX2 entry point when the build has AVX2**, otherwise the
platform's `intersectInt`:

```c
#if __AVX2__
#define intersectIntAuto(a, b, c, d, e) intersectIntAVX2(a, b, c, d, e)
#else
#define intersectIntAuto(a, b, c, d, e) intersectInt(a, b, c, d, e)
#endif
```

Note what this does _not_ do: on ARM it resolves to `intersectInt`, which is
already the NEON implementation, so NEON is reached through the fallback branch
rather than a dedicated one. The macro is a compile-time alias, so it evaluates
each argument exactly once and carries no runtime dispatch cost.

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

Unlike `intersectInt`, this one is **not** platform-dispatched and **not**
SIMD — it is a single scalar implementation compiled on every target, and it
requires you to pass the small set first. It is the right choice when you
already know the skew and want to skip the heuristic.

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

postingList *getPostingList(const char *term); /* your index */
void freePostingList(postingList *list);

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

/* intsetU32Array() takes a non-const intsetU32 *, so the inputs are
 * non-const here too. */
intsetU32 *intsetIntersect(intsetU32 *a, intsetU32 *b) {
    /* Get array pointers and counts */
    const uint32_t *aArray = intsetU32Array(a);
    const uint32_t *bArray = intsetU32Array(b);
    size_t aCount = intsetU32Count(a);
    size_t bCount = intsetU32Count(b);

    /* Allocate result set with max possible size */
    size_t maxSize = aCount < bCount ? aCount : bCount;
    intsetU32 *result = intsetU32NewLen((uint32_t)maxSize);

    /* Compute intersection directly into result array */
    size_t count = intersectIntAuto(
        aArray, aCount,
        bArray, bCount,
        intsetU32Array(result)
    );

    /* Update count and shrink to size.
     * Both take intsetU32 *, not intsetU32 ** - the set is resized in
     * place and the handle never moves. */
    intsetU32UpdateCount(result, (uint32_t)count);
    intsetU32ShrinkToSize(result);

    return result;
}

/* Usage */
intsetU32 *setA = intsetU32New();
intsetU32 *setB = intsetU32New();

/* Add elements to sets... */
for (uint32_t i = 0; i < 1000; i += 2) {
    intsetU32Add(setA, i);  /* Even numbers */
}
for (uint32_t i = 0; i < 1000; i += 3) {
    intsetU32Add(setB, i);  /* Multiples of 3 */
}

/* Intersect: numbers divisible by both 2 and 3 (i.e., by 6) */
intsetU32 *intersection = intsetIntersect(setA, setB);
printf("Intersection size: %zu\n", intsetU32Count(intersection));
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

    /* Intersect with remaining arrays.
     * The running result only ever shrinks, so once it is the shorter
     * side it may serve as its own output - no scratch buffer, no
     * malloc/free per round. */
    for (size_t i = 2; i < numArrays && count > 0; i++) {
        if (count <= lengths[i]) {
            count = intersectIntAuto(result, count,
                                     arrays[i], lengths[i], result);
        } else {
            /* The accumulator is the LARGER side here, so it cannot
             * alias; use a temporary for this round. */
            uint32_t *temp = malloc(lengths[i] * sizeof(uint32_t));
            count = intersectIntAuto(result, count,
                                     arrays[i], lengths[i], temp);
            memcpy(result, temp, count * sizeof(uint32_t));
            free(temp);
        }
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

### Scalar Algorithms (`scalar` and `match_scalar`)

These are two distinct functions, both non-SIMD, with different roles:

- **`scalar`** — the whole-input fallback. On a target with neither SSE4.1 nor
  NEON, `intersectInt()` is nothing but a call to this.
- **`match_scalar`** — the _tail_ handler. Every SIMD algorithm on every
  platform finishes its remainder here, once the block loop can no longer fill a
  vector.

Both are two-pointer merges:

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

Both are also exercised directly by the test suite on every platform, which is
what makes them the oracle the SIMD paths are checked against.

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

The real code never divides — it multiplies the shorter side instead, which
avoids a division and any rounding question:

```c
if (length1 == 0 || length2 == 0) {
    return 0;
}

if (1000 * length1 <= length2 || 1000 * length2 <= length1) {
    /* SIMDgalloping, smaller array first */
} else if (50 * length1 <= length2 || 50 * length2 <= length1) {
    /* v3, smaller array first */
} else {
    /* v1, smaller array first */
}
```

The thresholds come from the upstream project's benchmarking, not from
measurements taken in this repository.

## Performance Characteristics

### Time Complexity

| Algorithm     | Best Case     | Average Case  | Worst Case  | Notes            |
| ------------- | ------------- | ------------- | ----------- | ---------------- |
| scalar        | O(n+m)        | O(n+m)        | O(n+m)      | Simple merge     |
| v1            | O(n+m/8)      | O(n+m/4)      | O(n+m)      | 2×4 SIMD         |
| v3            | O(n+m/32)     | O(n+m/16)     | O(n+m)      | 32-way SIMD      |
| SIMDgalloping | O(n×log(m/n)) | O(n×log(m/n)) | O(n+m)      | Exponential skip |
| galloping     | O(n×log(m/n)) | O(n×log(m/n)) | O(n×log(m)) | No SIMD          |

Where n = size of smaller set, m = size of larger set

### Speedup vs Scalar

No speedup figures are published for this module — nothing in the repository
measures them, and the upstream project's numbers were taken on different
hardware with a different compiler. The structural expectation is:

- `v1` beats scalar at similar sizes because it compares one rare element
  against 8 frequent elements per step
- `v3` widens that to 32-element blocks, so it pulls ahead as skew grows
- `SIMDgalloping` wins at extreme skew because it _skips_ rather than scans
- AVX2 doubles the vector width over SSE4.1; NEON matches SSE4.1's width

Measure your own distribution under the cmake `perf` preset before choosing a
specific entry point over `intersectIntAuto`. Sanitizer builds invalidate any
timing taken from them: the `dev` preset (ASan+UBSan) ran perfRegression's
rows 1.0-7.7x slower than `perf` on an Apple M1 Max, unevenly across code.

### Space Complexity

All algorithms use **O(1) extra space** beyond the output buffer:

- No temporary arrays
- In-place pointer manipulation
- SIMD registers (128-bit SSE4.1/NEON or 256-bit AVX2)

Output buffer must be allocated by caller: `min(length1, length2)` elements.
Because `out` may alias the smaller input, that buffer can be one of the inputs
rather than a fresh allocation.

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

### ARM NEON

The ARM path (`v1_neon`, `v3_neon`, `SIMDgalloping_neon`) uses `<arm_neon.h>`
and works on 128-bit `uint32x4_t` vectors — the same 4-lane width as SSE4.1 —
with the same block structure and the same `match_scalar` tail handling. It is
compiled whenever `__aarch64__`, `__ARM_NEON`, or `__ARM_NEON__` is defined.

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

### 4. Aliasing: Overwrite the Smaller Input, Never the Larger

The output may be written straight over the **shorter** input array, which lets
a repeated-intersection loop run without a scratch buffer. Overwriting the
longer input is not supported.

```c
/* GOOD - out aliases the smaller array */
size_t count = (len1 <= len2)
                   ? intersectInt(set1, len1, set2, len2, set1)
                   : intersectInt(set1, len1, set2, len2, set2);

/* GOOD - separate buffer always works */
uint32_t *out = malloc((len1 < len2 ? len1 : len2) * sizeof(uint32_t));
size_t count = intersectInt(set1, len1, set2, len2, out);

/* WRONG - aliasing the LARGER input */
size_t count = intersectInt(small, 10, large, 100000, large);
```

A multi-way AND can therefore accumulate in place: the running result only ever
shrinks, so it stays the smaller side and can be its own output.

### 5. Handle Empty Sets

```c
/* Library handles empty inputs gracefully: intersectInt() checks both
 * lengths before touching anything and returns 0 without writing to out.
 * (A zero-length array is not valid ISO C, so model "empty" with a NULL
 * pointer and a 0 length, or with a length of 0 over a real buffer.) */
uint32_t a[10] = {0};
const uint32_t *b = NULL;  /* Empty */
uint32_t out[10];

size_t count = intersectInt(a, 10, b, 0, out);
/* Returns 0, out is unmodified */
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

Within datakit these are set by the cmake presets; the flags below matter when
compiling `intersectInt.c` into another project.

### Required for AVX2

`intersectIntAVX2` only exists when `__AVX2__` is defined:

```bash
clang -mavx2 -O3 -c intersectInt.c
```

### Required for SSE4.1

The x86 SIMD path is gated on `__SSE4_1__`. Without it, an x86 build falls all
the way through to `scalar()`:

```bash
clang -msse4.1 -O3 -c intersectInt.c
```

### ARM NEON

No flag is needed on ARM64 — `__aarch64__` is always defined there, so the NEON
path is selected automatically.

### Optimization Flags

```bash
clang -O3 -march=native -funroll-loops -c intersectInt.c
```

`-march=native` picks up SSE4.1/AVX2/NEON in one step. Do **not** add
`-ffast-math`: this module is entirely integer code, so it gains nothing and the
flag weakens floating-point semantics everywhere else in the translation unit.

## Platform Support

| Platform         | SSE4.1 path | NEON path | AVX2 entry point  | `intersectInt()` uses |
| ---------------- | ----------- | --------- | ----------------- | --------------------- |
| x86-64           | ✓           | ✗         | ✓ when `-mavx2`   | SSE4.1 SIMD           |
| x86 (32-bit)     | ✓           | ✗         | ✓ when `-mavx2`   | SSE4.1 SIMD           |
| ARM64 / ARM NEON | ✗           | ✓         | ✗ (symbol absent) | NEON SIMD             |
| Everything else  | ✗           | ✗         | ✗ (symbol absent) | `scalar()`            |

**Note**: ARM is **not** a scalar fallback — a full NEON implementation of all
three algorithms exists and is what `intersectInt()` resolves to there. Only
targets that are neither SSE4.1-capable x86 nor NEON-capable ARM drop to
`scalar()`.

Supported build targets are Linux, macOS, and the BSD family. There is no
Windows support in datakit.

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
- Paper: "SIMD Compression and the Intersection of Sorted Integers"
  (Lemire, Boytsov, Kurz), Software: Practice and Experience
- License: Apache License 2.0

## Implementation Notes

The implementation uses several techniques:

1. **Compile-time platform dispatch** — three separate definitions of
   `intersectInt()`, chosen by the preprocessor, with no runtime branch
2. **Multiplicative size comparison** instead of division for the ratio test
3. **Loop unrolling** for SIMD efficiency
4. **Unaligned loads** (`_mm_loadu_si128` / `_mm256_loadu_si256`) so callers do
   not have to align their arrays
5. **Shuffle masks** for efficient result packing
6. **Galloping search** for skipping large gaps
7. **`match_scalar` tail handling** wherever a block loop leaves a remainder

## Testing

```bash
./src/datakit-test test intersectInt
```

The suite runs the same battery — empty arrays, single elements, disjoint sets,
identical sets, partial overlap, small and large arrays, several size ratios,
commutativity, and large (near-`UINT32_MAX`) values — against every
implementation available in the current build:

- Always: `scalar`, `match_scalar`, `intersectInt`, `intersectIntOneSidedGalloping`
- On x86: `v1`, `v3`, `SIMDgalloping`, and the two `highlyscalable_intersect_SIMD`
  variants
- On ARM: `v1_neon`, `v3_neon`, `SIMDgalloping_neon`
- With AVX2: `intersectIntAVX2`, `v1_avx2`, `v3_avx2`, `SIMDgalloping_avx2`

A cross-implementation consistency check confirms every variant agrees on the
same inputs, so a SIMD path that diverges from `scalar` fails the suite.
