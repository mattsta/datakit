# jebuf - Jemalloc Size Class Calculator

## Overview

`jebuf` provides **jemalloc size class calculations** to determine the actual memory allocation size for a given request. This prevents memory waste by allowing you to use the full allocation that jemalloc will provide anyway.

**Key Features:**

- Rounds allocation requests to jemalloc size classes
- Prevents wasted memory from internal fragmentation
- Three optimized lookup tables (16-bit, 32-bit, 64-bit)
- Branch-free binary search for O(log n) performance
- Supports allocations from 8 bytes to 16 TiB
- Decision helper for reallocation efficiency

**Header**: `jebuf.h`

**Source**: `jebuf.c` (also carries the `jebuf` test under `DATAKIT_TEST`)

> **Above 16 TiB the tables run out.** A request larger than the last 64-bit
> entry trips `assert(NULL && "who needs this much memory?")` in an
> assertions-enabled build. With assertions off it falls back to
> `size + size/5` (20% growth, integer math), saturating at `SIZE_MAX` rather
> than wrapping. Treat 16 TiB as the supported ceiling.

## What are Jemalloc Size Classes?

**jemalloc** (the allocator used by many high-performance systems) doesn't allocate arbitrary sizes. Instead, it uses **size classes** - predefined allocation sizes that reduce fragmentation and improve performance.

### The Problem

```c
void *ptr = malloc(100);
// You asked for:  100 bytes
// jemalloc gives: 112 bytes (next size class)
// You're using:   100 bytes
// Wasted:         12 bytes (10.7%)
```

### The Solution

```c
size_t actual = jebufSizeAllocation(100);
// actual = 112 (the size jemalloc will actually allocate)

void *ptr = malloc(actual);
// You asked for:  112 bytes
// jemalloc gives: 112 bytes
// You're using:   112 bytes
// Wasted:         0 bytes!
```

## Jemalloc Size Class Tables

jebuf maintains size class tables matching jemalloc's allocation strategy. The tables are divided into three ranges for efficiency:

### Small Sizes (8 - 54 KiB)

```c
8, 16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256,
320, 384, 448, 512, 640, 768, 896, 1024,
1280, 1536, 1792, 2048, 2560, 3072, 3584, 4096,
5 KiB, 6 KiB, 7 KiB, 8 KiB, 10 KiB, 12 KiB, 14 KiB, 16 KiB,
20 KiB, 24 KiB, 28 KiB, 32 KiB, 40 KiB, 48 KiB, 54 KiB
```

**Pattern**:

- Quantum: 16 bytes (8, 16, 32, 48...)
- Then powers of 2 with ±25% steps
- Designed to minimize waste for small objects

### Medium Sizes (64 KiB - 3.5 GiB)

```c
64 KiB, 80 KiB, 96 KiB, 112 KiB, 128 KiB, ...
1 MiB, 1.25 MiB, 1.5 MiB, 1.75 MiB, 2 MiB, ...
/* Then exponential steps: */
Step 1 MiB:  5 MiB, 6 MiB, 7 MiB, 8 MiB
Step 2 MiB:  10 MiB, 12 MiB, 14 MiB, 16 MiB
Step 4 MiB:  20 MiB, 24 MiB, 28 MiB, 32 MiB
...
Step 512 MiB: 2.5 GiB, 3 GiB, 3.5 GiB
```

**Pattern**:

- Continues quantum spacing through KiB range
- Switches to exponentially increasing steps
- Balances granularity vs table size

### Large Sizes (4 GiB - 16 TiB)

```c
Step 1 GiB:  5 GiB, 6 GiB, 7 GiB, 8 GiB
Step 2 GiB:  10 GiB, 12 GiB, 14 GiB, 16 GiB
...
Step 2 TiB:  10 TiB, 12 TiB, 14 TiB, 16 TiB
```

**Pattern**:

- Very large step sizes
- For huge allocations (databases, caches, etc.)

## API Reference

### Size Class Lookup

```c
/* Get jemalloc size class for a requested size
 * currentBufSize: desired allocation size
 * Returns: actual size jemalloc will allocate
 */
size_t jebufSizeAllocation(size_t currentBufSize);

/* Check if reallocating would use a STRICTLY SMALLER size class
 * originalSize: current allocation size
 * newSize: desired new size
 * Returns: jebufSizeAllocation(newSize) < jebufSizeAllocation(originalSize)
 *
 * This is a SHRINK test only. For a growing newSize it always returns
 * false, so never use it to decide whether a grow needs a realloc -
 * compare the two size classes directly for that.
 */
bool jebufUseNewAllocation(size_t originalSize, size_t newSize);

/* Example: Basic size class lookup */
size_t request = 100;
size_t actual = jebufSizeAllocation(request);
printf("Requesting %zu → Getting %zu\n", request, actual);
// Output: Requesting 100 → Getting 112

/* Example: Is shrinking worth a realloc? */
size_t current = 1024;
size_t needed = 900;   /* rounds to 1024 as well - answer is false */
if (jebufUseNewAllocation(current, needed)) {
    // Shrinking would move to smaller size class
    void *newPtr = realloc(ptr, jebufSizeAllocation(needed));
} else {
    // Same size class, no benefit to reallocating
    // Just use less of the current allocation
}
```

## Algorithm Details

### Binary Search Implementation

Like fibbuf, jebuf uses **branch-free binary search**:

```c
#define BINARY_SEARCH_BRANCH_FREE                           \
    do {                                                    \
        do {                                                \
            mid = &result[half];                            \
            result = (*mid < currentBufSize) ? mid : result;\
            n -= half;                                      \
            half = n / 2;                                   \
        } while (half > 0);                                 \
    } while (0)
```

**Complexity**: O(log n) where n = table size. The largest table has 64 entries,
so the search runs about 6 steps.

### Table Selection

```c
if (currentBufSize <= 54 KiB)
    use 16-bit table (44 entries, 8 B .. 54 KiB)
else if (currentBufSize <= 3584 MiB || sizeof(void *) == 4)
    use 32-bit table (64 entries, 64 KiB .. 3584 MiB)
else
    use 64-bit table (49 entries, 4 GiB .. 16 TiB)
```

On 32-bit builds the 64-bit tier is never selected, so requests above
3584 MiB fall off the end of the 32-bit table and take the assert/20% path.

## Real-World Examples

### Example 1: Efficient String Buffer

```c
typedef struct string {
    char *data;
    size_t length;
    size_t capacity;
} string;

string *stringCreate(void) {
    string *s = malloc(sizeof(*s));

    /* Start with jemalloc size class for ~32 bytes */
    s->capacity = jebufSizeAllocation(32); // Returns 32
    s->data = malloc(s->capacity);
    s->length = 0;

    return s;
}

void stringAppend(string *s, const char *str) {
    size_t needed = s->length + strlen(str) + 1;

    if (needed > s->capacity) {
        /* WITHOUT jebuf: */
        // size_t newCap = needed * 2;
        // s->data = realloc(s->data, newCap); // May waste memory!

        /* WITH jebuf: */
        size_t newCap = jebufSizeAllocation(needed * 2);
        s->data = realloc(s->data, newCap);
        s->capacity = newCap;

        printf("Grew to %zu (using full jemalloc allocation)\n", newCap);
    }

    strcpy(s->data + s->length, str);
    s->length = needed - 1;
}

/* Growth example:
   Append "Hello World" (11 bytes)
   needed = 12
   newCap = jebufSizeAllocation(24) = 32

   Append more... needed = 40
   newCap = jebufSizeAllocation(80) = 80

   Append more... needed = 100
   newCap = jebufSizeAllocation(200) = 224
*/
```

### Example 2: Smart Vector Allocation

```c
typedef struct vector {
    int *data;
    size_t size;
    size_t capacity;
} vector;

void vectorReserve(vector *v, size_t newCapacity) {
    if (newCapacity <= v->capacity) {
        return; // Already have enough space
    }

    /* Round to jemalloc size class */
    size_t bytes = newCapacity * sizeof(int);
    size_t allocBytes = jebufSizeAllocation(bytes);
    size_t actualCapacity = allocBytes / sizeof(int);

    printf("Reserve %zu → Alloc %zu bytes → Capacity %zu\n",
           newCapacity, allocBytes, actualCapacity);

    v->data = realloc(v->data, allocBytes);
    v->capacity = actualCapacity;
}

/* Usage */
vector v = {0};
vectorReserve(&v, 100);
// Reserve 100 → Alloc 448 bytes → Capacity 112
// We can actually fit 112 ints, not just 100!
```

### Example 3: Memory Pool Sizing

```c
typedef struct memPool {
    void *pool;
    size_t poolSize;
    size_t used;
} memPool;

memPool *memPoolCreate(size_t requestedSize) {
    memPool *pool = malloc(sizeof(*pool));

    /* Use jemalloc size class to avoid waste */
    pool->poolSize = jebufSizeAllocation(requestedSize);
    pool->pool = malloc(pool->poolSize);
    pool->used = 0;

    printf("Requested %zu, got %zu (%.1f%% extra)\n",
           requestedSize, pool->poolSize,
           (pool->poolSize - requestedSize) * 100.0 / requestedSize);

    return pool;
}

/* Example output:
   Requested 1000000, got 1048576 (4.9% extra)
   Better than wasting the extra space!
*/
```

### Example 4: Reallocation Decision

```c
void *smartRealloc(void *ptr, size_t oldSize, size_t newSize) {
    size_t oldClass = jebufSizeAllocation(oldSize);
    size_t newClass = jebufSizeAllocation(newSize);

    if (oldClass == newClass) {
        /* Same size class - no need to reallocate! */
        printf("Staying in same size class (%zu), no realloc\n", oldClass);
        return ptr;
    }

    if (newClass < oldClass) {
        /* Shrinking to smaller size class */
        printf("Shrinking: %zu → %zu\n", oldClass, newClass);

        /* Only reallocate if savings are significant */
        if (oldClass - newClass > 4096) { // > 4 KB savings
            return realloc(ptr, newClass);
        }
        return ptr; // Not worth the effort
    }

    /* Growing - must reallocate */
    printf("Growing: %zu → %zu\n", oldClass, newClass);
    return realloc(ptr, newClass);
}

/* Example usage:
   void *p = malloc(100);  // Size class: 112
   p = smartRealloc(p, 100, 105);
   // Output: "Staying in same size class (112), no realloc"

   p = smartRealloc(p, 105, 200);
   // Output: "Growing: 112 → 224"
*/
```

### Example 5: Waste Analysis Tool

```c
void analyzeWaste(size_t requestedSizes[], size_t count) {
    printf("Size\tActual\tWaste\tWaste%%\n");
    printf("----\t------\t-----\t-------\n");

    size_t totalRequested = 0;
    size_t totalActual = 0;

    for (size_t i = 0; i < count; i++) {
        size_t requested = requestedSizes[i];
        size_t actual = jebufSizeAllocation(requested);
        size_t waste = actual - requested;
        float wastePct = (waste * 100.0) / requested;

        printf("%zu\t%zu\t%zu\t%.1f%%\n",
               requested, actual, waste, wastePct);

        totalRequested += requested;
        totalActual += actual;
    }

    size_t totalWaste = totalActual - totalRequested;
    float avgWaste = (totalWaste * 100.0) / totalRequested;

    printf("\nTotal waste: %zu / %zu (%.1f%% overhead)\n",
           totalWaste, totalRequested, avgWaste);
}

/* Usage */
size_t sizes[] = {100, 500, 1000, 5000, 10000};
analyzeWaste(sizes, 5);

/* Output:
Size    Actual  Waste   Waste%
----    ------  -----   -------
100     112     12      12.0%
500     512     12      2.4%
1000    1024    24      2.4%
5000    5120    120     2.4%
10000   10240   240     2.4%

Total waste: 408 / 16600 (2.5% overhead)
*/
```

## Performance Characteristics

| Operation             | Complexity | Notes                   |
| --------------------- | ---------- | ----------------------- |
| jebufSizeAllocation   | O(log n)   | n = table size (max 69) |
| jebufUseNewAllocation | O(log n)   | Two table lookups       |
| Table selection       | O(1)       | Simple range checks     |

### Benchmark Results

The test suite runs timed loops over all three tiers and prints per-call
timings rather than asserting fixed numbers, so no figures are quoted here.
Measure with the cmake `perf` preset - sanitizer builds add 3-10x overhead and
invalidate the numbers:

```bash
./src/datakit-test test jebuf
```

Expect near-constant cost across size ranges: the search is branch-free with a
fixed small iteration count on tables that fit in a few cache lines.

## Memory Waste Reduction

### Worst-Case Waste by Size Range

```
Small allocations (8-128 bytes):
- Worst: 8 bytes (request 57 → get 64, waste 12.3%)
- Best: 0 bytes (request size class exactly)

Medium allocations (1-16 KB):
- Worst: ~500 bytes (request 4600 → get 5120, waste 11.3%)
- Average: ~10% waste

Large allocations (> 1 MB):
- Worst: ~1 MB (request just over size class boundary)
- Average: ~5-10% waste
```

### With vs Without jebuf

```c
/* WITHOUT jebuf: */
void *p1 = malloc(100);      // Allocates 112, uses 100, wastes 12
void *p2 = malloc(500);      // Allocates 512, uses 500, wastes 12
void *p3 = malloc(1000);     // Allocates 1024, uses 1000, wastes 24
// Total waste: 48 bytes (2.9%)

/* WITH jebuf: */
void *p1 = malloc(jebufSizeAllocation(100));   // Allocates 112, uses 112, wastes 0
void *p2 = malloc(jebufSizeAllocation(500));   // Allocates 512, uses 512, wastes 0
void *p3 = malloc(jebufSizeAllocation(1000));  // Allocates 1024, uses 1024, wastes 0
// Total waste: 0 bytes (0%)
```

## Best Practices

### 1. Use for All Dynamic Allocations

```c
/* GOOD - always round to size class */
size_t needed = calculateSize();
size_t actual = jebufSizeAllocation(needed);
void *ptr = malloc(actual);
capacity = actual; // Track actual capacity

/* BAD - waste jemalloc's extra space */
size_t needed = calculateSize();
void *ptr = malloc(needed);
capacity = needed; // Missing out on extra space!
```

### 2. Check Before Shrinking

`jebufUseNewAllocation()` answers exactly one question: _would shrinking to
`newSize` actually land in a smaller size class?_ It returns false for any grow.

```c
/* GOOD - avoid a realloc that frees nothing */
if (jebufUseNewAllocation(currentSize, newSize)) {
    ptr = realloc(ptr, jebufSizeAllocation(newSize));
} else {
    // Same (or larger) size class - keep the current allocation
}

/* GOOD - growing: compare classes directly */
const size_t want = jebufSizeAllocation(newSize);
if (want > jebufSizeAllocation(currentSize)) {
    ptr = realloc(ptr, want);
}

/* BAD - a grow never satisfies jebufUseNewAllocation, so this
 * silently never reallocates and then overruns the buffer */
if (jebufUseNewAllocation(currentSize, biggerSize)) {
    ptr = realloc(ptr, jebufSizeAllocation(biggerSize));
}
```

### 3. Combine with Growth Strategies

```c
/* EXCELLENT - fibbuf + jebuf */
size_t fibSize = fibbufNextSizeBuffer(currentSize);
size_t allocSize = jebufSizeAllocation(fibSize);
ptr = realloc(ptr, allocSize);

/* This combines:
   - Fibonacci's efficient growth
   - Jemalloc's size class alignment
*/
```

### 4. Pre-calculate for Fixed Sizes

```c
/* GOOD - calculate once at compile time or init */
static const size_t BUFFER_SIZES[] = {
    /* Pre-calculated jemalloc size classes */
    32, 64, 128, 256, 512, 1024, 2048, 4096
};

/* BAD - calculate every time */
for (int i = 0; i < 1000000; i++) {
    size_t size = jebufSizeAllocation(100); // Wastes CPU!
}
```

## Common Pitfalls

### 1. Not Using Returned Size

```c
/* WRONG - calculates but doesn't use */
size_t actual = jebufSizeAllocation(100);
void *ptr = malloc(100); // Should be malloc(actual)!
```

### 2. Applying to Non-jemalloc Systems

```c
/* WARNING - only accurate for jemalloc */
// On systems using glibc malloc or tcmalloc,
// the size classes may be different!

#ifdef USE_JEMALLOC
    size_t allocSize = jebufSizeAllocation(needed);
#else
    size_t allocSize = needed; // Can't optimize
#endif
```

### 3. Over-optimizing Small Allocations

```c
/* PROBABLY NOT WORTH IT - overhead > savings */
for (int i = 0; i < 1000000; i++) {
    int *num = malloc(jebufSizeAllocation(sizeof(int)));
    // The jebufSizeAllocation() call costs more than
    // the 4-byte savings!
}

/* BETTER - just allocate directly */
for (int i = 0; i < 1000000; i++) {
    int *num = malloc(sizeof(int));
}
```

### 4. Confusing Bytes with Elements

```c
/* WRONG - applies to element count, not bytes */
size_t count = jebufSizeAllocation(100); // Returns 112 BYTES
int *array = malloc(count * sizeof(int)); // Allocates 448 bytes!

/* RIGHT - apply to byte size */
size_t bytes = jebufSizeAllocation(100 * sizeof(int));
int *array = malloc(bytes);
size_t count = bytes / sizeof(int);
```

## Size Class Table Reference

Quick reference for common sizes:

```
Requested   → Actual
100         → 112
256         → 256
500         → 512
1000        → 1024
1 KiB       → 1024
4 KiB       → 4096
8 KiB       → 8192
16 KiB      → 16384
64 KiB      → 65536
1 MiB       → 1048576
10 MiB      → 10485760
100 MiB     → 117440512      (112 MiB - 100 MiB is NOT a size class)
1 GiB       → 1073741824
4 GiB       → 4294967296
```

Note the 100 MiB row: above 64 MiB the 32-bit table steps by 16 MiB
(80, 96, 112, 128 MiB), so a 100 MiB request rounds up to 112 MiB. Do not
assume round decimal sizes are size classes - always call the function.

## Integration with Other Systems

### With fibbuf (Fibonacci Growth)

```c
/* Perfect combination */
size_t nextSize = fibbufNextSizeBuffer(currentSize);
size_t allocSize = jebufSizeAllocation(nextSize);
ptr = realloc(ptr, allocSize);
```

See [fibbuf](FIBBUF.md) for details.

### With membound (Bounded Allocator)

Don't use jebuf with membound. membound is a buddy allocator, so it rounds to
powers of two, not to jemalloc classes - jebuf's answer would be the wrong
shape. membound exposes its own equivalents:

```c
/* What would this request actually cost? (no allocation performed) */
size_t rounded = memboundGoodSize(m, 100);

/* What did this pointer actually get? */
size_t usable = memboundUsableSize(m, ptr);
```

See [membound](MEMBOUND.md) for details.

## See Also

- [fibbuf](FIBBUF.md) - Fibonacci buffer growth (uses jebuf internally)
- [membound](MEMBOUND.md) - Bounded memory allocator
- [Jemalloc Documentation](http://jemalloc.net/jemalloc.3.html) - Official jemalloc reference

## Testing

Run the jebuf test suite:

```bash
./src/datakit-test test jebuf
```

The test suite validates:

- Correct size class rounding across all three tiers
- Tier-selection boundaries (54 KiB and 3584 MiB)
- Exact-size-class inputs returning themselves
- Timed loops across the size ranges
