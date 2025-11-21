# fibbuf - Fibonacci Buffer Growth

## Overview

`fibbuf` provides **Fibonacci-based buffer growth calculations** for dynamic memory structures. Instead of exponential growth (doubling), it uses Fibonacci sequences to achieve sub-exponential growth with better memory efficiency and reduced over-allocation.

**Key Features:**

- Sub-exponential growth reduces memory waste
- Three optimized lookup tables (16-bit, 32-bit, 64-bit)
- Branch-free binary search for O(log n) performance
- Starts at 34 bytes (Fibonacci 9) for sane minimum
- Automatic 20% growth fallback beyond table limits
- Integrates with jemalloc size classes

**Header**: `fibbuf.h`

**Source**: `fibbuf.c`

## What is Fibonacci Buffer Growth?

Traditional buffer growth strategies use **exponential growth** (doubling):

```
32 → 64 → 128 → 256 → 512 → 1024 → ...
```

This wastes memory when you need, say, 300 bytes but get 512.

**Fibonacci growth** is gentler:

```
34 → 55 → 89 → 144 → 233 → 377 → 610 → 987 → ...
```

This provides:

1. **Better memory efficiency** - Smaller growth steps
2. **Still fast growth** - Sub-exponential but not linear
3. **Predictable behavior** - Mathematical sequence
4. **Golden ratio convergence** - Each step ≈ 1.618× previous

### Growth Comparison

```
Size: 100 bytes needed

Doubling:    128 → 256 (28% waste → 156% waste)
Fibonacci:   144 → 233 (44% waste → 133% waste)

Size: 1000 bytes needed

Doubling:    1024 → 2048 (2% waste → 104% waste)
Fibonacci:   1597 → 2584 (60% waste → 158% waste)

Size: 1 MB needed

Doubling:    1 MiB → 2 MiB (0% waste → 100% waste)
Fibonacci:   1.35 MiB → 2.18 MiB (35% waste → 118% waste)
```

On average, Fibonacci wastes **less memory for large buffers** while keeping reasonable growth rates.

## API Reference

### Buffer Size Calculation

```c
/* Get next Fibonacci buffer size larger than current size
 * currentBufSize: current buffer capacity
 * Returns: next Fibonacci number >= currentBufSize
 */
size_t fibbufNextSizeBuffer(const size_t currentBufSize);

/* Get allocation size rounded to jemalloc size class
 * currentBufSize: current buffer capacity
 * Returns: jemalloc-aligned size for fibbufNextSizeBuffer(currentBufSize)
 */
size_t fibbufNextSizeAllocation(const size_t currentBufSize);

/* Example: Basic buffer growth */
size_t capacity = 100;
size_t newCapacity = fibbufNextSizeBuffer(capacity);
// newCapacity = 144 (next Fibonacci >= 100)

/* Example: With jemalloc alignment */
size_t allocSize = fibbufNextSizeAllocation(capacity);
// allocSize = jemalloc size class containing 144
```

## Fibonacci Sequence Tables

fibbuf uses three pre-computed tables optimized for different size ranges:

### 16-bit Table (34 - 46,368 bytes)

```c
static const uint16_t fibbuf16[] = {
    34,    55,    89,    144,   233,   377,   610,   987,
    1597,  2584,  4181,  6765,  10946, 17711, 28657, 46368
};
```

**Range**: Fibonacci(9) to Fibonacci(24)
**Use case**: Small buffers, strings, small arrays

### 32-bit Table (75,025 - 2,971,215,073 bytes ≈ 2.8 GiB)

```c
static const uint32_t fibbuf32[] = {
    75025,     121393,    196418,     317811,     514229,
    832040,    1346269,   2178309,    3524578,    5702887,
    9227465,   14930352,  24157817,   39088169,   63245986,
    102334155, 165580141, 267914296,  433494437,  701408733,
    1134903170, 1836311903, 2971215073
};
```

**Range**: Fibonacci(25) to Fibonacci(47)
**Use case**: Medium buffers, MB-range allocations

### 64-bit Table (4.8 GiB - 1.54 TiB)

```c
static const uint64_t fibbuf64[] = {
    4807526976,   7778742049,   12586269025,  20365011074,
    32951280099,  53316291173,  86267571272,  139583862445,
    225851433717, 365435296162, 591286729879, 956722026041,
    1548008755920  /* 1.54 TB */
};
```

**Range**: Fibonacci(48) to Fibonacci(60)
**Use case**: Large buffers, GB-range allocations, big data

### Table Selection Logic

```c
if (size < 46,368)
    use fibbuf16
else if (size <= 2,971,215,073 OR on 32-bit system)
    use fibbuf32
else
    use fibbuf64
```

## Algorithm Details

### Binary Search Implementation

fibbuf uses a **branch-free binary search** for cache efficiency:

```c
#define BINARY_SEARCH_BRANCH_FREE                           \
    do {                                                    \
        /* binary search for the nearest fib */             \
        do {                                                \
            mid = &result[half];                            \
            result = (*mid < currentBufSize) ? mid : result;\
            n -= half;                                      \
            half = n / 2;                                   \
        } while (half > 0);                                 \
    } while (0)
```

**Why branch-free?**

- No conditional jumps → better CPU pipeline utilization
- More predictable performance
- Cache-friendly memory access patterns

**Complexity**: O(log n) where n = table size (max 23 iterations)

### Return Value Logic

```c
if (*result > currentBufSize) {
    /* Found larger value, return it */
    return *result;
}

if ((result + 1) <= TABLE_END) {
    /* Exact match found, return next Fibonacci */
    return *(result + 1);
}

if (result == TABLE_END) {
    /* Beyond table, grow by 20% */
    return currentBufSize * 1.2;
}
```

## Real-World Examples

### Example 1: Dynamic String Buffer

```c
typedef struct dynString {
    char *data;
    size_t length;
    size_t capacity;
} dynString;

dynString *dynStringCreate(void) {
    dynString *s = malloc(sizeof(*s));
    s->capacity = 34; // Start at Fibonacci(9)
    s->data = malloc(s->capacity);
    s->length = 0;
    return s;
}

void dynStringAppend(dynString *s, const char *str) {
    size_t needed = s->length + strlen(str) + 1;

    if (needed > s->capacity) {
        /* Grow using Fibonacci */
        size_t newCapacity = fibbufNextSizeBuffer(needed);
        char *newData = realloc(s->data, newCapacity);

        printf("Growing: %zu → %zu (needed %zu)\n",
               s->capacity, newCapacity, needed);

        s->data = newData;
        s->capacity = newCapacity;
    }

    strcpy(s->data + s->length, str);
    s->length = needed - 1;
}

/* Usage */
dynString *s = dynStringCreate();
dynStringAppend(s, "Hello");      // capacity: 34
dynStringAppend(s, " World");     // capacity: 34 (still fits)
dynStringAppend(s, "! This is a longer string"); // capacity: 55
// ... continues growing: 55 → 89 → 144 → ...
```

### Example 2: Vector with Fibonacci Growth

```c
typedef struct vector {
    int *data;
    size_t size;
    size_t capacity;
} vector;

void vectorPush(vector *v, int value) {
    if (v->size >= v->capacity) {
        /* Grow using Fibonacci + jemalloc alignment */
        size_t newCap = fibbufNextSizeBuffer(v->capacity + 1);
        size_t allocSize = fibbufNextSizeAllocation(v->capacity + 1);

        printf("Vector grow: %zu → %zu (alloc %zu)\n",
               v->capacity, newCap, allocSize);

        v->data = realloc(v->data, allocSize * sizeof(int));
        v->capacity = newCap;
    }

    v->data[v->size++] = value;
}

/* Growth pattern */
vector v = {0};
for (int i = 0; i < 1000; i++) {
    vectorPush(&v, i);
}
// Grows: 34 → 55 → 89 → 144 → 233 → 377 → 610 → 987 → 1597
```

### Example 3: Network Buffer

```c
typedef struct netBuffer {
    uint8_t *data;
    size_t used;
    size_t capacity;
} netBuffer;

bool netBufferEnsureSpace(netBuffer *buf, size_t needed) {
    if (buf->used + needed <= buf->capacity) {
        return true; // Already have space
    }

    /* Need to grow */
    size_t newCap = fibbufNextSizeBuffer(buf->used + needed);

    uint8_t *newData = realloc(buf->data, newCap);
    if (!newData) {
        return false;
    }

    printf("Network buffer: %zu → %zu bytes\n", buf->capacity, newCap);

    buf->data = newData;
    buf->capacity = newCap;
    return true;
}

ssize_t netBufferRecv(netBuffer *buf, int sock) {
    /* Ensure we have at least 4 KB available */
    if (!netBufferEnsureSpace(buf, 4096)) {
        return -1;
    }

    ssize_t received = recv(sock, buf->data + buf->used,
                            buf->capacity - buf->used, 0);
    if (received > 0) {
        buf->used += received;
    }

    return received;
}
```

### Example 4: Comparing Growth Strategies

```c
void compareGrowth(size_t maxSize) {
    printf("Size\tFibonacci\tDoubling\tFib Waste\tDouble Waste\n");

    size_t fibCap = 34;
    size_t doubleCap = 32;

    for (size_t needed = 100; needed < maxSize; needed *= 2) {
        /* Fibonacci growth */
        while (fibCap < needed) {
            fibCap = fibbufNextSizeBuffer(fibCap);
        }

        /* Doubling growth */
        while (doubleCap < needed) {
            doubleCap *= 2;
        }

        float fibWaste = ((float)fibCap - needed) / needed * 100;
        float doubleWaste = ((float)doubleCap - needed) / needed * 100;

        printf("%zu\t%zu\t\t%zu\t\t%.1f%%\t\t%.1f%%\n",
               needed, fibCap, doubleCap, fibWaste, doubleWaste);
    }
}

/* Output:
Size    Fibonacci   Doubling    Fib Waste   Double Waste
100     144         128         44.0%       28.0%
200     233         256         16.5%       28.0%
400     610         512         52.5%       28.0%
800     987         1024        23.4%       28.0%
...
*/
```

## Performance Characteristics

| Operation                | Complexity | Notes                       |
| ------------------------ | ---------- | --------------------------- |
| fibbufNextSizeBuffer     | O(log n)   | n = table size (max 23)     |
| fibbufNextSizeAllocation | O(log n)   | Includes jebuf lookup       |
| Table lookup             | O(1)       | Select table by range check |

### Benchmark Results

From the test suite (70 million iterations):

```
Operation                   Time per call
fibbuf 16-bit (small)      ~2-3 ns
fibbuf 32-bit (medium)     ~2-3 ns
fibbuf 64-bit (large)      ~2-3 ns
fibbuf out-of-range (×1.2) ~1-2 ns
```

**Observation**: Virtually identical performance across all ranges due to branch-free search.

## Integration with jemalloc

`fibbufNextSizeAllocation()` combines Fibonacci growth with jemalloc size classes:

```c
size_t fibbufNextSizeAllocation(const size_t currentBufSize) {
    /* Get next Fibonacci size */
    size_t fibSize = fibbufNextSizeBuffer(currentBufSize);

    /* Round up to jemalloc size class */
    return jebufSizeAllocation(fibSize);
}
```

**Why?** Prevents memory waste from jemalloc internal rounding:

```
Without jebuf:
- Request 987 bytes
- jemalloc allocates 1024 bytes
- Waste: 37 bytes

With jebuf:
- Request 987 bytes → rounds to 1024
- Use full 1024 bytes
- Waste: 0 bytes
```

See [jebuf](JEBUF.md) for details on jemalloc size classes.

## Best Practices

### 1. Use for Long-Lived Buffers

```c
/* GOOD - buffer will grow over time */
dynString *log = dynStringCreate(); // Uses Fibonacci
for (int i = 0; i < 1000000; i++) {
    dynStringAppend(log, "Log entry\n");
}

/* BAD - short-lived, doubling is fine */
char *temp = malloc(32);
for (int i = 0; i < 10; i++) {
    if (full) temp = realloc(temp, capacity * 2); // Doubling OK
}
```

### 2. Start at Minimum (34 bytes)

```c
/* GOOD - let fibbuf handle small sizes */
size_t capacity = 0;
capacity = fibbufNextSizeBuffer(capacity); // Returns 34

/* BAD - wasting the Fibonacci sequence */
size_t capacity = 1024; // Skips first 10 Fibonacci numbers!
```

### 3. Combine with jebuf for Allocations

```c
/* GOOD - uses actual allocation size */
size_t allocSize = fibbufNextSizeAllocation(currentSize);
buffer = realloc(buffer, allocSize);

/* BAD - may waste jemalloc overhead */
size_t fibSize = fibbufNextSizeBuffer(currentSize);
buffer = realloc(buffer, fibSize); // jemalloc may round up
```

### 4. Don't Use for Fixed-Size Allocations

```c
/* BAD - Fibonacci is for growth, not sizing */
int *array = malloc(fibbufNextSizeBuffer(100) * sizeof(int));

/* GOOD - just allocate what you need */
int *array = malloc(100 * sizeof(int));
```

## Common Pitfalls

### 1. Requesting Exact Fibonacci Number

```c
/* UNEXPECTED - returns NEXT Fibonacci */
size_t size = fibbufNextSizeBuffer(144); // Returns 233, not 144!

/* If you want >= 144, need to check */
size_t size = fibbufNextSizeBuffer(143); // Returns 144
```

### 2. Not Accounting for Waste

```c
/* Size needed: 1000 bytes
   Fibonacci gives: 1597 bytes
   Waste: 597 bytes (59.7%)

   Consider: Maybe exponential growth is better for this use case? */
```

### 3. Very Large Buffers Beyond Table

```c
/* Beyond 1.54 TB, uses 20% growth */
size_t huge = 2ULL * 1024 * 1024 * 1024 * 1024; // 2 TB
size_t next = fibbufNextSizeBuffer(huge);
// next = 2TB * 1.2 = 2.4 TB (not Fibonacci!)
```

### 4. Forgetting Size is in Bytes

```c
/* WRONG - grows array count, not bytes */
int *array;
size_t count = 100;
count = fibbufNextSizeBuffer(count); // count = 144 (bytes, not ints!)

/* RIGHT - grow byte size, convert to count */
size_t bytes = 100 * sizeof(int);
bytes = fibbufNextSizeBuffer(bytes);
count = bytes / sizeof(int);
```

## When NOT to Use Fibonacci Growth

1. **Small, short-lived buffers** - Overhead of calculation not worth it
2. **Fixed-size requirements** - Just allocate what you need
3. **Very fast growth needed** - Exponential doubling is faster
4. **Memory-constrained** - Fibonacci still wastes space, use exact sizing
5. **Non-uniform access patterns** - Consider a different strategy

## Comparison Table

| Strategy      | Growth Rate     | Memory Efficiency | Complexity |
| ------------- | --------------- | ----------------- | ---------- |
| Linear (+k)   | Slow            | Excellent         | O(1)       |
| Fibonacci     | Sub-exponential | Good              | O(log n)   |
| Doubling (×2) | Exponential     | Poor              | O(1)       |
| Custom        | Varies          | Varies            | Varies     |

## See Also

- [jebuf](JEBUF.md) - Jemalloc size class calculator (used by fibbufNextSizeAllocation)
- [membound](MEMBOUND.md) - Bounded memory allocator

## Testing

Run the fibbuf test suite:

```bash
./src/datakit-test test fibbuf
```

The test suite validates:

- Correct Fibonacci sequence generation
- Binary search correctness
- Table boundary conditions
- Performance benchmarks (70M iterations)
- Integration with jebuf sizing
