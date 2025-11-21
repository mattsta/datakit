# datakit Architecture Overview

This document explains the design philosophy, architectural patterns, and key concepts used throughout the datakit library.

## Design Philosophy

### Core Principles

1. **Performance First**: Every data structure is optimized for speed and memory efficiency
2. **Zero Overhead**: Minimal abstraction layers - get as close to the metal as possible
3. **Compression Awareness**: Many structures use compression to reduce memory footprint
4. **Scale Adaptability**: Different implementations for different data sizes
5. **Opaque Types**: Hide implementation details to allow future optimization
6. **Deterministic Behavior**: Predictable performance characteristics

### Memory Efficiency

datakit achieves exceptional memory efficiency through:

- **Packed Structures**: Minimize padding and alignment waste
- **Compression**: Use flex (compressed arrays) as underlying storage
- **Inline Storage**: Small data stored directly in containers
- **Variable Encoding**: Integer sets use 16/32/64-bit encoding as needed
- **Shared Storage**: Multiple values can share the same backing memory

## Key Architectural Patterns

### 1. Scale-Aware Collections

One of the most important patterns in datakit is **scale-aware design**. Many containers come in multiple size variants:

```
┌──────────────────┬──────────────┬─────────────────────────────┐
│ Variant          │ Size Range   │ Optimization                │
├──────────────────┼──────────────┼─────────────────────────────┤
│ Small            │ < 1KB        │ Inline storage, simple      │
│ Medium           │ 1KB - 100KB  │ Balanced performance        │
│ Large/Full       │ > 100KB      │ Complex indexing, scalable  │
└──────────────────┴──────────────┴─────────────────────────────┘
```

**Examples:**

- `multimapSmall`, `multimapMedium`, `multimapFull`
- `multilistSmall`, `multilistMedium`, `multilistFull`
- `multiarraySmall`, `multiarrayMedium`, `multiarrayLarge`

**Why This Matters:**

- Small data doesn't need complex indexing structures
- Large data benefits from sophisticated algorithms
- Automatic promotion is possible in some containers

**Choosing the Right Variant:**

```c
/* For small, bounded collections (< 1KB) */
multimapSmall *sm = multimapSmallNew();

/* For medium-sized data (1KB - 100KB) */
multimapMedium *mm = multimapMediumNew();

/* For large or unbounded data (> 100KB) */
multimap *fm = NULL;
multimapNew(&fm);  /* Uses multimapFull internally */
```

### 2. Opaque Types and Double Pointers

Most datakit containers use **opaque types** - you cannot directly access the structure's fields:

```c
/* WRONG - structure is opaque */
struct multimap {  /* Not visible to users */
    /* Internal fields hidden */
};

/* RIGHT - use API functions */
multimap *m = NULL;
multimapNew(&m);
multimapInsert(&m, key, keylen, value, vallen);
```

**Double Pointer Pattern:**

Many functions take `container **` (pointer to pointer):

```c
void multimapNew(multimap **m);
void multimapInsert(multimap **m, ...);
void multimapFree(multimap **m);
```

**Why?**

- Allows in-place reallocation (container can grow/shrink)
- Simplifies memory management
- Makes NULL handling clearer

**Usage Pattern:**

```c
multimap *m = NULL;  /* Always initialize to NULL */
multimapNew(&m);     /* Pass address of pointer */
/* m now points to allocated multimap */

multimapInsert(&m, ...);  /* May reallocate m */
multimapFree(&m);         /* Frees and sets m = NULL */
/* m is now NULL again */
```

### 3. Universal Container - databox

The `databox` provides a **16-byte fixed-size container** that can hold any type:

```c
typedef struct databox {
    union {
        int64_t i64;
        uint64_t u64;
        double d64;
        void *ptr;
        struct {
            uint32_t u32a;
            uint32_t u32b;
        };
        uint8_t bytes[16];
    } data;
    databoxType type;  /* Type tag */
} databox;
```

**Use Cases:**

- Polymorphic storage (store different types in same container)
- Avoiding heap allocation for small values
- Type-safe access with runtime checks

**Example:**

```c
databox box;

/* Store integer */
databoxNewSignedInt(&box, -42);

/* Store double */
databoxNewDouble(&box, 3.14159);

/* Store pointer */
databoxNewPtr(&box, mydata);

/* Retrieve with type checking */
if (box.type == DATABOX_SIGNED_64) {
    int64_t val = databoxGetSignedInt(&box);
}
```

### 4. Flexible Arrays (flex)

The `flex` module is the **foundation** of many datakit containers:

```
┌─────────────────────────────────────────────────────┐
│ flex - Compressed Byte Array                       │
│                                                     │
│  [Header: size, element count, metadata]           │
│  [Entry 1: length prefix + data]                   │
│  [Entry 2: length prefix + data]                   │
│  [Entry 3: length prefix + data]                   │
│  ...                                                │
└─────────────────────────────────────────────────────┘
```

**Features:**

- Variable-length entries
- Compressed headers (small integers use fewer bytes)
- Automatic reallocation
- Forward/reverse iteration
- Insert/delete at any position

**Used By:**

- multilist (nodes stored as flex)
- multiarray (elements stored as flex)
- multimap values
- mds/mdsc string buffers

**Comparison:**

```c
/* Traditional array - fixed element size */
int array[100];  /* 400 bytes minimum */

/* flex - variable element size */
flex *f = NULL;
flexPushBytes(&f, "a", 1);        /* 1 byte data + overhead */
flexPushBytes(&f, "hello", 5);    /* 5 bytes data + overhead */
flexPushBytes(&f, "x", 1);        /* 1 byte data + overhead */
/* Total: ~10-15 bytes instead of 300+ for fixed strings */
```

### 5. String Buffer Template (dks)

The `dks.h` header is a **C template** (using preprocessor) that generates two string buffer variants:

```c
/* Full variant - tracks length, allocated size, reference count */
#define DK_MAKE_STRING mds
#include "dks.h"

/* Compact variant - no reference count, minimal overhead */
#define DK_MAKE_STRING mdsc
#include "dks.h"
```

**Generated API:**

- `mdsNew()`, `mdsFree()`, `mdsLen()`, `mdsAppend()`, etc.
- `mdscNew()`, `mdscFree()`, `mdscLen()`, `mdscAppend()`, etc.

**When to Use:**

- **mds**: When you need reference counting or large strings
- **mdsc**: When memory is tight and you don't need ref counting

### 6. Iterator Pattern

Most containers support **bidirectional iteration**:

```c
/* flex iterator */
flexIter *iter = flexIterator(f, FLEX_START_HEAD);  /* Forward */
while (flexNext(iter, &entry)) {
    /* Process entry */
}
flexIteratorFree(iter);

/* Reverse iteration */
flexIter *rev = flexIterator(f, FLEX_START_TAIL);
while (flexPrev(rev, &entry)) {
    /* Process in reverse */
}
flexIteratorFree(rev);
```

**Containers with Iterators:**

- flex
- multilist
- multimap
- multiarray
- intset

### 7. Compression and Encoding

datakit includes several compression techniques:

#### Delta-of-Delta (dod)

For sequences of **increasing integers**:

```c
/* Original: 100, 105, 108, 115, 120, ... */
/* First deltas: 5, 3, 7, 5, ... */
/* Delta-of-delta: 5, -2, 4, -2, ... (smaller values!) */

dod *d = dodNew();
for (int64_t val : values) {
    dodAdd(d, val);
}
/* Compressed storage, fast retrieval */
```

#### XOR Filter (xof)

For sequences of **floating-point** numbers:

```c
/* Use XOR of doubles + delta-of-delta */
xof *x = xofNew();
xofAdd(x, 1.234);
xofAdd(x, 1.235);  /* Similar values compress well */
xofAdd(x, 1.236);

/* Retrieve compressed values */
double val;
xofGet(x, 0, &val);
```

#### Half-Precision Float (float16)

Store floats in **16 bits** instead of 32/64:

```c
/* Uses CPU F16C instructions if available */
float f = 3.14159f;
uint16_t f16 = floatToFloat16(f);    /* 16-bit encoding */
float restored = float16ToFloat(f16); /* Decode */

/* ~50% memory savings for float arrays */
```

### 8. Memory Management Patterns

#### Memory Bounds (membound)

Thread-safe global memory limits:

```c
/* Set global limit: 100 MB */
memboundSet(100 * 1024 * 1024);

/* Allocations will fail if limit exceeded */
void *ptr = memboundMalloc(1024);
if (!ptr) {
    /* Out of memory budget */
}
```

#### Allocation Strategies

**Fibonacci Sizing (fibbuf):**

```c
/* Grow by fibonacci numbers: 1, 2, 3, 5, 8, 13, 21, ... */
fibbuf *fb = fibbufNew();
void *mem = fibbufGrow(fb);  /* Allocates next fibonacci size */
```

**Jemalloc Alignment (jebuf):**

```c
/* Align to jemalloc size classes for efficiency */
jebuf *jb = jebufNew();
void *mem = jebufAlloc(jb, 1000);  /* Rounds to optimal jemalloc size */
```

### 9. Integer Set Encoding

The `intset` uses **variable-width encoding**:

```
┌────────────┬─────────────────────────────────┐
│ Values     │ Encoding                        │
├────────────┼─────────────────────────────────┤
│ -32K..32K  │ 16-bit (INTSET_ENC_INT16)      │
│ -2B..2B    │ 32-bit (INTSET_ENC_INT32)      │
│ Any        │ 64-bit (INTSET_ENC_INT64)      │
└────────────┴─────────────────────────────────┘
```

**Auto-Promotion:**

```c
intset *is = intsetNew();  /* Starts as 16-bit */

is = intsetAdd(is, 100, &success);     /* Still 16-bit */
is = intsetAdd(is, 50000, &success);   /* Promotes to 32-bit! */
is = intsetAdd(is, 5000000000L, &success);  /* Promotes to 64-bit! */

/* All previous values automatically converted */
```

### 10. Roaring Bitmap (multiroar)

Compressed bitmap for **sparse integer sets**:

```c
multiroar *mr = multiroarNew();

/* Add individual bits */
multiroarSet(mr, 1000);
multiroarSet(mr, 1000000);
multiroarSet(mr, 1000000000);

/* Set operations */
multiroar *result = multiroarAnd(mr1, mr2);  /* Intersection */
multiroar *result = multiroarOr(mr1, mr2);   /* Union */
multiroar *result = multiroarXor(mr1, mr2);  /* Symmetric diff */

/* Very efficient for sparse sets */
```

### 11. Probabilistic Counting (hyperloglog)

**Approximate cardinality** with ~0.81% error:

```c
hyperloglog *hll = hyperloglogCreate();

/* Add millions of elements */
for (int i = 0; i < 10000000; i++) {
    char buf[32];
    int len = snprintf(buf, 32, "user:%d", i);
    hyperloglogAdd(hll, (uint8_t *)buf, len);
}

/* Get estimate - uses only ~12KB memory! */
uint64_t estimate = hyperloglogCount(hll);
/* estimate ≈ 10000000 ± 0.81% */
```

**Modes:**

- **Sparse**: For low cardinality (< 10K elements)
- **Dense**: Automatically promoted for high cardinality

## Module Layering

```
┌─────────────────────────────────────────────────────┐
│ Application Layer                                   │
└─────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────┐
│ High-Level Containers                               │
│ (multimap, multilist, multiarray, multilru, etc.)   │
└─────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────┐
│ Mid-Level Structures                                │
│ (flex, mflex, intset, hyperloglog, multiroar)       │
└─────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────┐
│ Low-Level Utilities                                 │
│ (str, databox, membound, ptrPrevNext)               │
└─────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────┐
│ Platform Layer                                      │
│ (config, datakit, OSRegulate, portableRandom)       │
└─────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────┐
│ Dependencies                                        │
│ (lz4, xxHash, sha1, imath, varint)                  │
└─────────────────────────────────────────────────────┘
```

## Choosing the Right Container

### Decision Tree

```
Need to store data?
│
├─ Integers only?
│  ├─ Dense (many consecutive values)
│  │  └─ Use: intset or intsetU32
│  │
│  └─ Sparse (scattered values)
│     └─ Use: multiroar (roaring bitmap)
│
├─ Just counting unique items?
│  └─ Use: hyperloglog (probabilistic)
│
├─ Key-value pairs?
│  ├─ Multiple values per key
│  │  └─ Use: multimap (small/medium/full)
│  │
│  └─ Single value per key
│     └─ Use: multidict
│
├─ Ordered sequence?
│  ├─ Need fast random access
│  │  └─ Use: multiarray or flex
│  │
│  └─ Need fast insert/delete in middle
│     └─ Use: multilist
│
├─ Need LRU eviction?
│  └─ Use: multilru
│
├─ Variable-length byte sequences?
│  └─ Use: flex or mflex
│
└─ Strings?
   ├─ Need reference counting
   │  └─ Use: mds
   │
   └─ Minimal overhead
      └─ Use: mdsc
```

### Performance Characteristics

```
┌──────────────┬───────────┬────────┬────────┬─────────────┐
│ Container    │ Insert    │ Lookup │ Delete │ Memory      │
├──────────────┼───────────┼────────┼────────┼─────────────┤
│ flex         │ O(n)      │ O(n)   │ O(n)   │ Excellent   │
│ multimap     │ O(1)      │ O(1)   │ O(1)   │ Good        │
│ multilist    │ O(1)*     │ O(n)   │ O(1)*  │ Excellent   │
│ multiarray   │ O(1)**    │ O(1)   │ O(n)   │ Good        │
│ intset       │ O(log n)  │ O(log n)│ O(n)   │ Excellent   │
│ hyperloglog  │ O(1)      │ N/A    │ N/A    │ Fixed (12KB)│
│ multiroar    │ O(1)      │ O(1)   │ O(1)   │ Excellent   │
└──────────────┴───────────┴────────┴────────┴─────────────┘
* When iterator positioned
** Amortized
```

## Thread Safety

**General Rule**: datakit containers are **NOT thread-safe** by default.

**Options for Concurrent Access:**

1. **External Locking**: Use mutexes around container operations
2. **Per-Thread Containers**: Each thread has its own instance
3. **Copy-on-Write**: Clone containers for readers

**Thread-Safe Modules:**

- `membound` - Uses internal locking
- `fastmutex` - Thread-safe mutex implementation
- `OSRegulate` - Thread-safe resource limiting

**Example with External Locking:**

```c
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
multimap *m = NULL;
multimapNew(&m);

/* Thread-safe insert */
pthread_mutex_lock(&lock);
multimapInsert(&m, key, klen, val, vlen);
pthread_mutex_unlock(&lock);

/* Thread-safe lookup */
pthread_mutex_lock(&lock);
void *result;
size_t rlen;
bool found = multimapLookup(m, key, klen, &result, &rlen);
pthread_mutex_unlock(&lock);
```

## Platform Abstraction

datakit supports multiple platforms through:

1. **config.h**: Compile-time feature detection
2. **datakit.h**: Platform-specific includes and macros
3. **CMake**: Build system configuration

**Supported Platforms:**

- Linux (primary target)
- macOS / Darwin
- FreeBSD, OpenBSD, NetBSD
- Solaris, AIX
- Windows (Cygwin, MinGW)

**Feature Detection:**

- Random number generation (getrandom, getentropy, /dev/urandom)
- CPU instructions (F16C for float16, AVX for vectorization)
- System calls (madvise, mlock, etc.)
- Compiler features (atomic operations, intrinsics)

## Error Handling

datakit uses **multiple error handling strategies**:

### 1. Return Values

```c
/* Boolean success/failure */
bool found = multimapLookup(m, key, klen, &value, &vlen);
if (!found) {
    /* Handle not found */
}

/* Pointer returns (NULL = error) */
flex *f = flexNew();
if (!f) {
    /* Allocation failed */
}
```

### 2. Output Parameters

```c
uint8_t success;
intset *is = intsetAdd(is, value, &success);
if (!success) {
    /* Value already exists or allocation failed */
}
```

### 3. Assertions (Debug Mode)

```c
/* In debug builds, assertions catch programming errors */
assert(m != NULL);  /* Catches NULL pointer bugs */
assert(index < count);  /* Catches out-of-bounds access */
```

### 4. Graceful Degradation

```c
/* Some modules return best effort */
uint64_t estimate = hyperloglogCount(hll);  /* Never fails, may be approximate */
```

## Best Practices

### 1. Always Initialize to NULL

```c
/* WRONG */
multimap *m;
multimapNew(&m);  /* m has garbage value! */

/* RIGHT */
multimap *m = NULL;
multimapNew(&m);  /* Clear initialization */
```

### 2. Check Return Values

```c
/* WRONG */
flex *f = flexNew();
flexPushBytes(&f, data, len);  /* What if flexNew failed? */

/* RIGHT */
flex *f = flexNew();
if (!f) {
    /* Handle allocation failure */
    return ERROR_NO_MEMORY;
}
flexPushBytes(&f, data, len);
```

### 3. Always Free Resources

```c
multimap *m = NULL;
multimapNew(&m);

/* ... use m ... */

multimapFree(&m);  /* Free memory */
/* m is now NULL */
```

### 4. Use Appropriate Size Variant

```c
/* WRONG - using Full variant for small data */
multimap *m = NULL;  /* Uses multimapFull */
multimapNew(&m);
/* Only storing 10 elements - wasting memory on hash table */

/* RIGHT - using Small variant */
multimapSmall *sm = multimapSmallNew();
/* Optimized for small collections */
```

### 5. Understand Ownership

```c
/* flex stores COPIES of data */
flexPushBytes(&f, data, len);
free(data);  /* Safe - flex has its own copy */

/* multimap can store REFERENCES */
multimapInsertByRef(&m, key, klen, value, vlen);
/* value must remain valid while in map! */
free(value);  /* WRONG - map still references it! */
```

## Performance Optimization

### 1. Pre-allocate When Possible

```c
/* Some containers support capacity hints */
flex *f = flexNew();
flexReserve(&f, expected_count);  /* Avoid multiple reallocations */

for (int i = 0; i < expected_count; i++) {
    flexPushBytes(&f, data, len);
}
```

### 2. Batch Operations

```c
/* WRONG - many small operations */
for (int i = 0; i < 1000000; i++) {
    multimapInsert(&m, key, klen, val, vlen);
}

/* BETTER - batch if possible */
multimapReserve(&m, 1000000);  /* If supported */
for (int i = 0; i < 1000000; i++) {
    multimapInsert(&m, key, klen, val, vlen);
}
```

### 3. Use Iterators for Scans

```c
/* WRONG - index-based iteration (slow for lists) */
for (size_t i = 0; i < multilistCount(ml); i++) {
    void *val = multilistIndex(ml, i);  /* O(n) for each! */
}

/* RIGHT - iterator-based (fast) */
multilistIter *iter = multilistIterator(ml, MULTILIST_FORWARD);
multilistEntry entry;
while (multilistNext(iter, &entry)) {
    /* O(1) for each */
}
multilistIteratorFree(iter);
```

### 4. Choose Right Container

```c
/* Need to count unique IPs seen? */

/* WRONG - using multimap (overkill) */
multimap *m = NULL;
multimapNew(&m);  /* Hash table overhead */

/* BETTER - using hyperloglog */
hyperloglog *hll = hyperloglogCreate();  /* Fixed 12KB memory */
/* Count millions of IPs efficiently */
```

## Summary

Key architectural concepts:

1. **Scale-Aware**: Different implementations for different sizes
2. **Opaque Types**: Hide implementation, allow optimization
3. **Double Pointers**: Enable in-place reallocation
4. **Compression**: flex, dod, xof, float16 reduce memory usage
5. **Variable Encoding**: intset adapts to data range
6. **Iterators**: Efficient traversal of containers
7. **Platform Abstraction**: Works across many OS and architectures
8. **Performance Focus**: Every design choice optimized for speed/memory

Next steps:

- Read module-specific documentation for detailed APIs
- Study test code for usage examples
- Experiment with different size variants
- Profile your application to choose optimal containers
