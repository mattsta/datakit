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

- `multimapSmall`, `multimapMedium`, `multimapFull` (plus
  `multimapAtom`, which is NOT a size tier — it is the
  reference-container variant other maps point into)
- `multilistSmall`, `multilistMedium`, `multilistFull`
- `multiarraySmall`, `multiarrayMedium`, `multiarrayLarge`, plus the
  `multiarrayNative*` macro family for caller-owned inline arrays

**Why This Matters:**

- Small data doesn't need complex indexing structures
- Large data benefits from sophisticated algorithms
- Automatic promotion is possible in some containers

**Choosing the Right Variant:**

`multimap` picks and switches its own variant. Reach for the concrete
variants only when you deliberately want to pin one:

```c
/* Normal use: the wrapper starts small and retiers as it grows.
 * The argument is elementsPerEntry, e.g. 2 for key+value. */
multimap *m = multimapNew(2);

/* Pin a size limit for the underlying flex instead of a variant */
multimap *bounded = multimapNewLimit(2, FLEX_CAP_LEVEL_2048);

/* Pinned variants, if you really want one */
multimapMedium *mm = multimapMediumNew(2);
multimapFull *fm = multimapFullNew(2);
```

### 2. Opaque Types and Double Pointers

Most datakit containers use **opaque types** - you cannot directly access the structure's fields:

```c
/* WRONG - structure is opaque */
struct multimap {  /* Not visible to users */
    /* Internal fields hidden */
};

/* RIGHT - use API functions and databox */
multimap *m = multimapNew(2);

databox key = databoxNewBytesString("k");
databox val = databoxNewBytesString("v");
const databox *entry[2] = {&key, &val};
multimapInsert(&m, entry);
```

**Double Pointer Pattern:**

Constructors RETURN the container. Only the operations that can
reallocate take `container **`:

```c
multimap *multimapNew(multimapElements elementsPerEntry);  /* returns */
bool multimapInsert(multimap **m, const databox *elements[]); /* may realloc */
bool multimapDelete(multimap **m, const databox *key);        /* may realloc */
void multimapFree(multimap *m);                               /* takes the value */
```

The same split holds elsewhere: `flexNew()` / `intsetNew()` /
`hyperloglogNew()` return, while `flexPushBytes(&f, ...)`,
`intsetAdd(&is, ...)`, and `hyperloglogAdd(&hll, ...)` take the address
because a growth step can move the allocation.

**Why?**

- Allows in-place reallocation (container can grow/shrink)
- Keeps the caller's handle valid across a retier
- Makes the "this call can move the object" contract visible at the
  call site

**Usage Pattern:**

```c
multimap *m = multimapNew(2);  /* constructor returns the handle */

multimapInsert(&m, entry);     /* may reallocate m */
multimapFree(m);               /* frees; m is dangling afterwards */
m = NULL;                      /* clear it yourself */
```

### 3. Universal Container - databox

The `databox` is a **16-byte fixed-size container** that can hold any
type: an 8-byte value union plus 8 bytes of packed metadata. A
`_Static_assert` pins the size at 16, and `datakit-test` asserts it at
startup.

```c
/* Value half: 8 bytes, one of ~20 spellings of the same storage */
typedef union databoxUnion {
    int64_t i64;
    uint64_t u64;
    double d64;
    void *ptr;
    union {
        const char *ccstart;
        uint8_t *start;
        uint8_t embed[8];  /* <= 8 bytes stored inline, no allocation */
        size_t offset;
    } bytes;
    /* ... plus i8/u8/i16/u16/i32/u32/f32 and 128-bit pointers */
} databoxUnion;

/* Metadata half: 8 bytes of bitfields */
typedef struct databox {
    databoxUnion data;      /* 8 bytes */
    uint64_t type : 8;      /* databoxType tag */
    uint64_t allocated : 1; /* data.bytes.start needs freeing */
    uint64_t created : 1;
    uint64_t big : 1;       /* actually a databoxBig */
    uint64_t unused : 5;
    uint64_t len : 48;      /* length of data.bytes */
} databox;
```

`databoxBig` extends it to 32 bytes with room for a 128-bit value.

**Use Cases:**

- Polymorphic storage (store different types in same container)
- Avoiding heap allocation for small values
- Type-safe access with runtime checks

**Example:** the constructors RETURN a box; they do not fill one
through an out-parameter.

```c
/* Store integers */
databox i = databoxNewSigned(-42);
databox u = databoxNewUnsigned(42);

/* Store a double */
databox d = databoxNewReal(3.14159);

/* Store bytes; <= 8 bytes can live inline */
databox s = databoxNewBytesString("hello");
databox tiny = databoxNewBytesAllowEmbed("hi", 2);

/* Retrieve with type checking */
if (DATABOX_IS_SIGNED_INTEGER(&i)) {
    int64_t val = i.data.i64;
}

if (DATABOX_IS_BYTES(&s)) {
    const uint8_t *p = databoxCBytes(&s);
    size_t n = databoxLen(&s);
}
```

### 4. Flexible Arrays (flex)

The `flex` module is the **foundation** of many datakit containers:

```
┌────────────────────────────────────────────────────┐
│ flex - Compressed Byte Array                       │
│                                                    │
│  [Header: size, element count, metadata]           │
│  [Entry 1: length prefix + data]                   │
│  [Entry 2: length prefix + data]                   │
│  [Entry 3: length prefix + data]                   │
│  ...                                               │
└────────────────────────────────────────────────────┘
```

**Features:**

- Variable-length entries
- Compressed headers (small integers use fewer bytes)
- Automatic reallocation
- Forward/reverse iteration
- Insert/delete at any position

**Used By:**

- multilist (nodes stored as flex, wrapped in mflex)
- multimap (all three variants store entries in flex)
- multiOrderedSet

`multiarray` is the exception: it stores fixed-size elements in plain
native arrays, not flex. `mds`/`mdsc` are also independent — they are
generated from `dks.h`, not built on flex.

**Comparison:**

```c
/* Traditional array - fixed element size */
int array[100];  /* 400 bytes minimum */

/* flex - variable element size */
flex *f = flexNew();
flexPushBytes(&f, "a", 1, FLEX_ENDPOINT_TAIL);      /* 1 byte + overhead */
flexPushBytes(&f, "hello", 5, FLEX_ENDPOINT_TAIL);  /* 5 bytes + overhead */
flexPushBytes(&f, "x", 1, FLEX_ENDPOINT_TAIL);      /* 1 byte + overhead */
/* Total: ~10-15 bytes instead of 300+ for fixed strings */
```

Every push takes a `flexEndpoint` (`FLEX_ENDPOINT_HEAD` or
`FLEX_ENDPOINT_TAIL`), so head and tail insertion share one entry
point.

### 5. String Buffer Template (dks)

The `dks.h` header is a **C template** (using the preprocessor) that
generates two string-buffer variants from one implementation. The
variant is selected by a define before the include:

```c
/* Full variant: wider length/free headers, up to 48-bit lengths */
#define DATAKIT_DKS_FULL
#include "dks.h"      /* typedef char mds;  DKS_TYPE == mds */

/* Compact variant: smaller headers, minimal per-string overhead */
#define DATAKIT_DKS_COMPACT
#include "dks.h"      /* typedef char mdsc; DKS_TYPE == mdsc */
```

Both types are `char` typedefs, so an `mds *` IS the string — the
length and free-space headers sit in the bytes immediately before the
returned pointer. That means `printf("%s", s)` works, and every
mutator returns a possibly-moved pointer you must reassign.

**Generated API** (each `DKS_*` macro becomes `<type><suffix>`):

- `mdsnew()`, `mdsempty()`, `mdsnewlen()`, `mdscat()`, `mdscatlen()`,
  `mdscatprintf()`, `mdslen()`, `mdsclear()`, `mdsdup()`, `mdsfree()`
- the same set with an `mdsc` prefix: `mdscnew()`, `mdsccat()`,
  `mdsclen()`, `mdscfree()`, …

**When to Use:**

- **mds**: general-purpose, and for strings large enough to need the
  wider headers
- **mdsc**: when memory is tight and strings are small, so the smaller
  header pays off

### 6. Iterator Pattern

Most containers support **bidirectional iteration**, but the shape
differs per module.

`flex` has no iterator object at all — you walk raw entry pointers.
Seed the forward loop with `flexHeadIfAny`, not `flexHead`:
`flexHead` returns an insertion POSITION and stays non-NULL on an empty
flex, so a `flexHead`-seeded loop reads one entry past the buffer.

```c
/* Forward */
for (flexEntry *fe = flexHeadIfAny(f); fe; fe = flexNext(f, fe)) {
    databox box;
    flexGetByType(fe, &box);
    /* Process box */
}

/* Reverse */
for (flexEntry *fe = flexTail(f); fe; fe = flexPrev(f, fe)) {
    databox box;
    flexGetByType(fe, &box);
    /* Process in reverse */
}
```

`multimap` and `multilist` use caller-stack iterator structs:

```c
multimapIterator it;
multimapIteratorInit(m, &it, true /* forward */);

databox key, val;
databox *elements[2] = {&key, &val};
while (multimapIteratorNext(&it, elements)) {
    /* Process key and val */
}
```

**Containers with Iterators:**

- flex (raw entry pointers, no iterator object)
- multimap (`multimapIterator`)
- multilist (`multilistIterator`; its init macros take `mflexState *[2]`)
- multidict (`multidictIterator`, plus a stateless `multidictScan`)
- intset (index-based via `intsetGet`, which is O(n) per call on the
  Medium/Full tiers)

`multiarray` exports no iterator and no count — index it yourself with
`multiarrayGet`.

### 7. Compression and Encoding

datakit includes several compression techniques:

#### Delta-of-Delta (dod)

For sequences of **increasing integers**:

```c
/* Original: 100, 105, 108, 115, 120, ... */
/* First deltas: 5, 3, 7, 5, ... */
/* Delta-of-delta: 5, -2, 4, -2, ... (smaller values!) */

/* dod is a bitstream, not a container. Write every value through a
 * dodWriter, which allocates and grows the stream; read back through a
 * dodReader. */
const dodVal values[] = {100, 105, 108, 115, 120, 125, 131};
const size_t count = sizeof(values) / sizeof(*values);

dodWriter w;
dodInit(&w);
for (size_t i = 0; i < count; i++) {
    dodWrite(&w, values[i]);
}
dodCloseWrites(&w);

dodReader r;
dodReaderInitFromWriter(&r, &w);
for (size_t i = 2; i < count; i++) {  /* first two seed the reader */
    const dodVal v = dodReaderNext(&r, w.d);
    (void)v; /* Process v */
}

zfree(w.d); /* the writer's stream */
```

`dodReadAll(d, vals, count)` decodes a whole stream in one call, and
`dodReaderNextN()` decodes in batches.

#### XOR-Encoded Doubles (xof)

For sequences of **floating-point** numbers (a lossless float codec, not an
XOR filter):

```c
/* Like dod, xof is a bitstream written through a xofWriter, which
 * allocates and grows the stream. Repeated values and values with few
 * significant bits compress well; full-precision computed doubles barely
 * do (measured ratios in docs/modules/compression/XOF.md). */
xofWriter w = {0};
xofWrite(&w, 1.25);
xofWrite(&w, 1.25); /* repeated: 1 bit */
xofWrite(&w, 1.5);

/* Sequential read: the reader starts on the first value */
xofReader r;
xofReaderInitFromWriter(&r, &w);
const double second = xofReaderNext(&r, w.d);

/* Or random access by index, and bulk decode */
const double first = xofGet(w.d, 0);
double out[3];
xofReadAll(w.d, out, w.count);
(void)first;
(void)second;

zfree(w.d); /* the writer's stream */
```

#### Half-Precision Float (float16)

Store floats in **16 bits** instead of 32/64:

```c
/* Macros that resolve to F16C intrinsics, NEON, or a scalar fallback
 * at compile time. */
float f = 3.14159f;
uint16_t f16 = float16Encode(f);      /* 16-bit encoding */
float restored = float16Decode(f16);  /* Decode */

/* Whole arrays convert through the batch entry points */
float16EncodeBatch(src, dst, count);
float16DecodeBatch(dst, src, count);

/* ~50% memory savings for float arrays */
```

### 8. Memory Management Patterns

#### Memory Bounds (membound)

`membound` is a bounded arena, created per-instance rather than
configured globally. Every call carries its arena:

```c
/* Create a 100 MB bounded pool */
membound *m = memboundCreate(100 * 1024 * 1024);

/* Allocations fail (return NULL) once the pool is exhausted */
void *ptr = memboundAlloc(m, 1024);
if (!ptr) {
    /* Out of memory budget */
}

memboundFree(m, ptr);

/* Accounting */
size_t used = memboundBytesUsed(m);
size_t cap = memboundCapacity(m);

memboundShutdown(m);
```

Fixed, dynamic, micro, compact, and enterprise flavors have their own
constructors (`memboundCreateFixed`, `memboundCreateDynamic`, …), and
`memboundCreateWithConfig` takes a full `memboundConfig`.

#### Allocation Strategies

`fibbuf` and `jebuf` are pure size-arithmetic helpers — they compute
sizes and allocate nothing:

**Fibonacci Sizing (fibbuf):**

```c
/* Next fibonacci-ish size up from the current buffer size */
size_t next = fibbufNextSizeBuffer(currentSize);
```

**Jemalloc Alignment (jebuf):**

```c
/* Round a request up to the enclosing jemalloc size class, so the
 * slack the allocator would waste becomes usable capacity. */
size_t allocSize = jebufSizeAllocation(requestedSize);

/* Skip the realloc entirely when the new size lands in the same class */
if (jebufUseNewAllocation(currentSize, newSize)) {
    ptr = realloc(ptr, jebufSizeAllocation(newSize));
}
```

### 9. Integer Set Encoding

The `intset` is **tiered**, not uniformly re-encoded. The tier is
carried in the pointer itself (pointer tagging), and each tier holds
separate sorted arrays per width:

```
┌────────┬───────────────────────────────────────────────────┐
│ Tier   │ Storage                                           │
├────────┼───────────────────────────────────────────────────┤
│ Small  │ one contiguous int16_t array                      │
│ Medium │ separate sorted int16_t and int32_t arrays        │
│ Full   │ separate sorted int16_t, int32_t, int64_t arrays  │
└────────┴───────────────────────────────────────────────────┘
```

This is the point of the design: a single large value promotes the
_container tier_, it does NOT widen the values already stored. A set
of a million small integers that gains one 5-billion value keeps its
million values in 16-bit storage and gains a one-element 64-bit array.

**Retiering:**

```c
intset *is = intsetNew();  /* Small tier */
bool success;

intsetAdd(&is, 100, &success);          /* stays Small */
intsetAdd(&is, 50000, &success);        /* retiers to Medium */
intsetAdd(&is, INT64_C(5000000000), &success); /* retiers to Full */

/* Existing entries stay in their original width arrays.
 * intsetAdd takes intset ** because a retier reallocates. */
```

Membership and counting fold across the tier's arrays; `intsetGet(is,
pos, &value)` performs a virtual merge and is O(n) on Medium/Full, so
prefer bulk reads over index loops.

### 10. Roaring Bitmap (multiroar)

Compressed bitmap for **sparse integer sets**:

```c
multiroar *mr = multiroarBitNew();

/* Add individual bits */
multiroarBitSet(mr, 1000);
multiroarBitSet(mr, 1000000);
multiroarBitSet(mr, 1000000000);

/* Bulk loads are far cheaper when positions are sorted/clustered —
 * one map operation per chunk run instead of one per bit. */
multiroarBitSetMany(mr, positions, count);

/* Set operations. The multiroarNew* forms return a new bitmap; the
 * unprefixed forms mutate the first argument in place. */
multiroar *both = multiroarNewAnd(a, b);     /* Intersection */
multiroar *either = multiroarNewOr(a, b);    /* Union */
multiroar *odd = multiroarNewXor(a, b);      /* Symmetric difference */
multiroar *only = multiroarNewAndNot(a, b);  /* Difference: A - B */

uint64_t population = multiroarBitCount(mr);
```

Use `multiroarNewAndNot` for set difference. Composing
`multiroarNewNot` with `multiroarNewAnd` does NOT produce it: the
complement is bounded to the chunks a bitmap has already materialized,
so the result depends on which chunks happen to exist.

### 11. Probabilistic Counting (hyperloglog)

**Approximate cardinality** with ~0.81% error:

```c
hyperloglog *hll = hyperloglogNew();

/* Add millions of elements. Takes hyperloglog ** because the sparse
 * form grows and eventually gets promoted to dense. */
for (int i = 0; i < 10000000; i++) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "user:%d", i);
    hyperloglogAdd(&hll, buf, (size_t)len);
}

/* Get estimate - dense form uses only ~12KB memory.
 * 'invalid' reports a corrupt encoding rather than a bad estimate. */
bool invalid = false;
uint64_t estimate = hyperloglogCount(hll, &invalid);
/* estimate ≈ 10000000 ± 0.81% */
```

`hyperloglogNewSparse()` and `hyperloglogNewDense()` pin the
representation, and `hyperloglogMergeInto(&dst, src)` folds one HLL
into another (idempotent and commutative).

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
├─ Running totals, prefix sums, rank/select over numbers?
│  ├─ One C type        └─ Use: fenwick<T> (fenwickI64, fenwickDouble, ...)
│  ├─ Mixed / 65-bit    └─ Use: multiFenwickFlat (multiFenwick: most compact)
│  ├─ A 2D grid         └─ Use: fenwick2d<T> or multiFenwick2d
│  └─ Range min/max     └─ Use: segment<T>
│     (see modules/fenwick/FENWICK.md "Choosing one")
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
┌──────────────┬───────────┬──────────┬──────────┬──────────────┐
│ Container    │ Insert    │ Lookup   │ Delete   │ Memory       │
├──────────────┼───────────┼──────────┼──────────┼──────────────┤
│ flex         │ O(n)      │ O(n)     │ O(n)     │ Excellent    │
│ multimap     │ O(log n)† │ O(log n)†│ O(log n)†│ Good         │
│ multidict    │ O(1)      │ O(1)     │ O(1)     │ Good         │
│ multilist    │ O(1)*     │ O(n)     │ O(1)*    │ Excellent    │
│ multiarray   │ O(1)**    │ O(1)     │ O(n)     │ Good         │
│ intset       │ O(log n)  │ O(log n) │ O(n)     │ Excellent    │
│ hyperloglog  │ O(1)      │ N/A      │ N/A      │ ~12KB dense  │
│ multiroar    │ O(1)      │ O(1)     │ O(1)     │ Excellent    │
│ fenwick<T>   │ O(log n)‡ │ O(log n) │ N/A      │ 2-16 B/elem  │
│ segment<T>   │ O(log n)‡ │ O(log n) │ N/A      │ 2x fenwick   │
└──────────────┴───────────┴──────────┴──────────┴──────────────┘
* When iterator positioned
** Amortized
† multimap is a SORTED container backed by flex, not a hash table:
  binary search to locate, plus a memmove inside the target flex.
  Use multidict when you want hashing.
‡ Point update; growth is amortized O(1) per appended index.  Lookup is
  a prefix sum or range fold.  Measured numbers, all variants and sizes:
  modules/fenwick/FENWICK.md and PERFORMANCE_BASELINES.md.
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
multimap *m = multimapNew(2);

/* Thread-safe insert */
const databox *entry[2] = {&key, &val};
pthread_mutex_lock(&lock);
multimapInsert(&m, entry);
pthread_mutex_unlock(&lock);

/* Thread-safe lookup */
databox value;
pthread_mutex_lock(&lock);
bool found = multimapLookupInto(m, &key, &value, 1);
pthread_mutex_unlock(&lock);
```

## Platform Abstraction

datakit supports multiple platforms through:

1. **dkPlatform.h**: the compiler check (clang or gcc), one `DK_OS_*` flag
   per target operating system (each always 0 or 1), and `DK_BITS`
2. **datakit.h**: Platform-specific includes and macros
3. **CMake**: Build system configuration

**Supported Platforms:**

- Linux (primary target)
- macOS / Darwin (primary target)
- FreeBSD, OpenBSD, NetBSD (each has its `DK_OS_*` flag; DragonFly builds
  through the generic POSIX paths)
- AIX has `DK_OS_AIX` detection but is neither built nor tested here —
  treat it as best effort

Windows and Solaris are **not** supported. The tree contains no
`_WIN32`, `_MSC_VER`, Win32, or `__sun` code paths, and clang/gcc are
the only supported compilers. See
[Platform Support](advanced/PLATFORMS.md).

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
databox value;
bool found = multimapLookupInto(m, &key, &value, 1);
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
bool success;
intsetAdd(&is, value, &success);
if (!success) {
    /* Value already present */
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
/* Some modules return best effort; 'invalid' separates a corrupt
 * encoding from a merely approximate answer. */
bool invalid = false;
uint64_t estimate = hyperloglogCount(hll, &invalid);
```

## Best Practices

### 1. Take the Handle the Constructor Returns

```c
/* WRONG - there is no out-parameter constructor */
multimap *m;
multimapNew(&m);

/* RIGHT */
multimap *m = multimapNew(2);
```

### 2. Check Return Values

```c
/* WRONG */
flex *f = flexNew();
flexPushBytes(&f, data, len, FLEX_ENDPOINT_TAIL);  /* What if flexNew failed? */

/* RIGHT */
flex *f = flexNew();
if (!f) {
    /* Handle allocation failure */
    return ERROR_NO_MEMORY;
}
flexPushBytes(&f, data, len, FLEX_ENDPOINT_TAIL);
```

### 3. Always Free Resources

```c
multimap *m = multimapNew(2);

/* ... use m ... */

multimapFree(m);  /* Frees; takes the value, not the address */
m = NULL;         /* Clear the handle yourself */
```

### 4. Use Appropriate Size Variant

```c
/* The wrapper already starts small and retiers upward, so this is
 * fine for 10 elements and for 10 million. */
multimap *m = multimapNew(2);

/* What you CAN pin is the underlying flex size limit, which controls
 * where the container splits. */
multimap *bounded = multimapNewLimit(2, FLEX_CAP_LEVEL_2048);
```

### 5. Understand Ownership

```c
/* flex stores COPIES of data */
flexPushBytes(&f, data, len, FLEX_ENDPOINT_TAIL);
free(data);  /* Safe - flex has its own copy */

/* Reference-container APIs (the *WithReference / *WithSurrogateKey
 * family, backed by multimapAtom) store references into a separate
 * container instead of copying. */
multimapInsertWithSurrogateKey(&m, elements, &insertKey, atomContainer);
/* The referenced container must outlive the map. */
```

## Performance Optimization

### 1. Size the Container Up Front

```c
/* flex has no reserve/capacity-hint API. Where it matters, set the
 * size limit at construction time on the container that wraps it. */
multimap *m = multimapNewLimit(2, FLEX_CAP_LEVEL_4096);
multilist *ml = multilistNew(FLEX_CAP_LEVEL_2048, 0);
```

### 2. Append Instead of Insert for Sorted Input

```c
/* multimapInsert searches for the sorted position on every entry */
for (size_t i = 0; i < n; i++) {
    multimapInsert(&m, entries[i]);
}

/* multimapAppend skips that search when input is already ordered */
for (size_t i = 0; i < n; i++) {
    multimapAppend(&m, entries[i]);
}
```

### 3. Use Iterators for Scans

```c
/* WRONG - index-based iteration (slow for lists) */
for (size_t i = 0; i < multilistCount(ml); i++) {
    multilistEntry e;
    multilistIndexGet(ml, state, i, &e);  /* re-walks nodes each time */
}

/* RIGHT - iterator-based */
mflexState *states[2] = {mflexStateCreate(), mflexStateCreate()};
multilistIterator iter;
multilistIteratorInitForward(ml, states, &iter);

multilistEntry entry;
while (multilistNext(&iter, &entry)) {
    /* amortized O(1) per entry */
}
multilistIteratorRelease(&iter);
```

### 4. Choose Right Container

```c
/* Need to count unique IPs seen? */

/* WRONG - a full map stores every distinct value */
multimap *m = multimapNew(1);

/* BETTER - using hyperloglog */
hyperloglog *hll = hyperloglogNew();  /* ~12KB once dense */
/* Count millions of IPs efficiently */
```

## Summary

Key architectural concepts:

1. **Scale-Aware**: Different implementations for different sizes
2. **Opaque Types**: Hide implementation, allow optimization
3. **Constructors return, mutators take `&`**: the double pointer marks
   exactly the calls that can move the object
4. **Compression**: flex, dod, xof, float16 reduce memory usage
5. **Tiered Encoding**: intset keeps narrow values narrow as it retiers
6. **Iterators**: Efficient traversal of containers
7. **POSIX Abstraction**: Linux, macOS, and BSD; clang/gcc only
8. **Performance Focus**: Every design choice optimized for speed/memory

Next steps:

- Read module-specific documentation for detailed APIs
- Study test code for usage examples
- Experiment with different size variants
- Profile your application to choose optimal containers
