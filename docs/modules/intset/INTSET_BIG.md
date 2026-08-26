# intsetBig - Bucketed Wide-Range Integer Set

## Overview

`intsetBig` is a **sorted set of very wide integers** built out of the two
pieces datakit already has: a `multimap` for the high bits of a value and an
`intsetU32` for the low bits. A value is split into a **bucket key** (all bits
above the low 20) and a **20-bit offset**; the multimap maps bucket key to the
`intsetU32` holding that bucket's offsets.

Values are handed in and out as `databoxBig`, so a single set holds signed and
unsigned, 64-bit and 128-bit values at once and compares them by numeric value
rather than by box type.

**Key Features:**

- Range far past 64 bits: `0 .. 2^84 - 1` unsigned, `-(2^83 - 1) .. 2^83 - 1`
  signed
- Bucketed storage: each bucket is one `intsetU32`, capped at 2^20 elements
  (4 MiB) so inserts and deletes never memmove a huge array
- Duplicate-free set semantics across the whole 128-bit input space
- Sorted ascending iteration through `intsetBigIterator`, negative buckets
  included
- Set algebra built in: `Intersect`, `MergeInto`, `Equal`, `Subset`
- **Stable handle**: the `intsetBig *` never moves, so every function takes
  `intsetBig *`, never `intsetBig **`
- Empty buckets are freed and removed from the multimap on the removal that
  empties them

**Header**: `intsetBig.h`

**Source**: `intsetBig.c`, built on `multimap.{c,h}`, `intsetU32.{c,h}`,
`intersectInt.{c,h}`, and `databox.h`

## Availability

`intsetBig` needs `__int128_t` / `__uint128_t` from the compiler. Consumers
gate on the compiler's own macro:

```c
#ifdef __SIZEOF_INT128__
#define USE_INTSET_BIG 1
#include "intsetBig.h"
#endif
```

That is exactly what `datakit-test.c` does, and `src/CMakeLists.txt` probes
the same capability with `check_c_source_compiles` (`DK_HAS_INT128`) and drops
the `intsetBig` entry from the CTest list when the probe fails. `intsetBig.c`
is always in the library source list; on a toolchain without 128-bit integers
nothing includes its header. Both supported compiler families (clang and gcc)
provide `__int128` on x86-64 and aarch64.

## Supported Range

The bucket key is a 64-bit integer and the offset is 20 bits, so the storable
range is 84 bits wide unsigned and 83 bits wide signed:

```c
#define INTSET_BIG_INT128_MIN -((((__int128_t)1) << (63 + 20)) - 1)
#define INTSET_BIG_INT128_MAX ((((__int128_t)1) << (63 + 20)) - 1)
#define INTSET_BIG_UINT128_MIN 0
#define INTSET_BIG_UINT128_MAX ((((__uint128_t)1) << (64 + 20)) - 1)
```

In decimal: unsigned values run up to `19342813113834066795298815`, and signed
values run from `-9671406556917033397649407` to `9671406556917033397649407`.
Every `int64_t` and `uint64_t` fits with room to spare, which is why the range
checks treat the 64-bit box types as always in range.

Check a candidate before storing it:

```c
/* Type-specific checks */
#define INTSET_BIG_RANGECHECK_INT128(a)  /* a within the signed range */
#define INTSET_BIG_RANGECHECK_UINT128(a) /* a within the unsigned range */

/* Box-driven check: 64-bit box types always pass; 128-bit box types are
 * range-checked; any other box type asserts. */
#define INTSET_BIG_RANGECHECK(box)
```

```c
databoxBig candidate;
DATABOX_BIG_SIGNED_128(&candidate, someWideValue);

if (INTSET_BIG_RANGECHECK(&candidate)) {
    intsetBigAdd(isb, &candidate);
} else {
    /* Out of range: storing it breaks the set's ordering guarantees */
}
```

Values outside the range are not rejected by `intsetBigAdd` — the range check
is the caller's job.

## Data Structure

### Handle

```c
typedef struct intsetBigSet intsetBig;
```

The struct is opaque. Internally it is a `multimap *` plus a cached element
count; both live in one small allocation that **never moves**, so an
`intsetBig *` captured anywhere stays valid across every mutation. Bucket
`intsetU32` handles do move as the multimap reallocs its storage, which is why
the multimap is the only place a bucket pointer is kept.

### Bucketing

With a divisor of `2^20` (1,048,576):

```
value  ->  bucket key = value / 2^20      (stored as the multimap key)
           offset     = value % 2^20      (stored inside that bucket's intsetU32)
```

- **Non-negative values** use an unsigned bucket key (`DATABOX_UNSIGNED_64`).
  `100` is bucket `0` offset `100`; `2^20` is bucket `1` offset `0`.
- **Negative values** bucket on their magnitude and store the key as
  `-(magnitude / 2^20 + 1)` (`DATABOX_SIGNED_64`). The `+ 1` exists because
  there is no signed zero to separate the negative side of bucket `0` from the
  positive side: `-1` is key `-1` offset `1`, while `1` is key `0` offset `1`.
  Reconstruction undoes it: `value = -(((-key - 1) * 2^20) + offset)`.

Because negative keys sort below unsigned keys in `databoxCompare`, walking
the multimap in key order walks the value space from most negative to most
positive. Inside a negative bucket a _larger_ offset means a _smaller_ value,
so the iterator walks negative buckets' offset arrays backwards — the emitted
sequence is strictly ascending end to end.

### Why 2^20

The offset width is the whole design trade. An `intsetU32` is a single sorted
array, so `Add` and `Remove` inside a bucket are `O(bucket)` memmoves. Capping
the offset at 20 bits caps a bucket at 1,048,576 elements (4 MiB), which keeps
per-bucket mutation bounded while still amortizing the multimap lookup over a
large span of values. The cap is also what makes `intsetU32` usable at all as
the leaf: 20 bits is comfortably inside `UINT32_MAX`.

### Value Reconstruction and Box Types

Reconstruction picks the narrowest box type that holds the result. A bucket
key small enough that `key * 2^20 + offset` fits in 64 bits yields a
`DATABOX_UNSIGNED_64` / `DATABOX_SIGNED_64` box; anything wider yields a
`DATABOX_UNSIGNED_128` / `DATABOX_SIGNED_128` box whose `data.u128` /
`data.i128` pointer points into that same `databoxBig`'s own `extra` storage.

**Do not shallow-copy a 128-bit `databoxBig`.** The copy's pointer still aims
at the original struct's `extra` bytes. Re-read the value, or copy the pointed
`__uint128_t` out explicitly.

## API Reference

### Creation and Destruction

```c
/* Create a new empty set */
intsetBig *intsetBigNew(void);

/* Free the set, every bucket's intsetU32, and the bucket map */
void intsetBigFree(intsetBig *isb);

/* Deep copy: new bucket map, new intsetU32 per bucket */
intsetBig *intsetBigCopy(const intsetBig *isb);
```

```c
intsetBig *isb = intsetBigNew();
/* ... use it ... */
intsetBigFree(isb);
```

`intsetBigCopy` produces a fully independent set: mutating either side leaves
the other alone.

### Adding, Removing, Testing

```c
/* Add 'val'. Returns true if newly added, false if already present. */
bool intsetBigAdd(intsetBig *isb, const databoxBig *val);

/* Remove 'val'. Returns true if it was present and is now gone. */
bool intsetBigRemove(intsetBig *isb, const databoxBig *val);

/* Membership test. */
bool intsetBigExists(const intsetBig *isb, const databoxBig *val);
```

```c
#include "intsetBig.h"

#include <inttypes.h>
#include <stdio.h>

intsetBig *isb = intsetBigNew();

databoxBig small = DATABOX_BIG_UNSIGNED(UINT64_C(42));
databoxBig negative = DATABOX_BIG_SIGNED(INT64_C(-1048577));

databoxBig wide;
DATABOX_BIG_UNSIGNED_128(&wide, ((__uint128_t)1 << 70) + 5);

printf("42 added: %s\n", intsetBigAdd(isb, &small) ? "yes" : "no");   /* yes */
printf("42 again: %s\n", intsetBigAdd(isb, &small) ? "yes" : "no");   /* no  */

intsetBigAdd(isb, &negative);
intsetBigAdd(isb, &wide);

printf("elements: %zu in %zu buckets\n", intsetBigCountElements(isb),
       intsetBigCountBuckets(isb));

printf("wide present: %s\n", intsetBigExists(isb, &wide) ? "yes" : "no");

printf("removed: %s\n", intsetBigRemove(isb, &negative) ? "yes" : "no");
printf("removed twice: %s\n",
       intsetBigRemove(isb, &negative) ? "yes" : "no");                /* no */

intsetBigFree(isb);
```

A signed box and an unsigned box holding the same non-negative number land in
the same bucket at the same offset, so `DATABOX_BIG_SIGNED(0)` and
`DATABOX_BIG_UNSIGNED(0)` are the same set member.

Removing the last element of a bucket frees that bucket's `intsetU32` and
deletes the bucket from the map, so `intsetBigCountBuckets` returns to `0` for
an emptied set.

### Random Access

```c
/* Pick a uniformly random element by element index (not by bucket). */
bool intsetBigRandom(const intsetBig *isb, databoxBig *val);

/* Pick a random element, remove it, and report it through 'deleted'. */
bool intsetBigRandomDelete(intsetBig *isb, databoxBig *deleted);
```

Selection is weighted by element count: buckets are walked accumulating counts
until the randomly chosen element index falls inside one, so a bucket holding
a thousand elements is a thousand times likelier to be drawn from than a
bucket holding one.

**Both require a non-empty set.** The index is taken modulo the element count,
so guard the call:

```c
databoxBig picked = {{0}};
if (intsetBigCountElements(isb) > 0 && intsetBigRandom(isb, &picked)) {
    /* 'picked' is a member of the set */
}
```

### Iteration

```c
typedef struct intsetBigIterator {
    multimapIterator iter;
    struct {
        databoxBig box;
    } bucket;

    struct {
        ssize_t position;
        ssize_t count;
        uint32_t *array;
    } elements;

    bool forward; /* true if iterating from smallest to largest numbers */
} intsetBigIterator;

/* Point the iterator at the smallest element of 'isb'. */
void intsetBigIteratorInit(const intsetBig *isb, intsetBigIterator *isbIter);

/* Fetch the next element in ascending order.
 * Returns false once the set is exhausted. */
bool intsetBigIteratorNextBox(intsetBigIterator *isbIter, databoxBig *val);
```

The iterator is a caller-owned stack struct; there is nothing to free. It
yields elements in **strictly ascending numeric order** across negative and
positive buckets alike, and an empty set yields nothing on the very first
`NextBox`.

```c
#include "intsetBig.h"
#include "str.h" /* StrInt128ToBuf / StrUInt128ToBuf */

#include <inttypes.h>
#include <stdio.h>

void intsetBigPrintAll(const intsetBig *isb) {
    intsetBigIterator iter;
    intsetBigIteratorInit(isb, &iter);

    databoxBig val = {{0}};
    while (intsetBigIteratorNextBox(&iter, &val)) {
        switch (val.type) {
        case DATABOX_UNSIGNED_64:
            printf("%" PRIu64 "\n", val.data.u64);
            break;
        case DATABOX_SIGNED_64:
            printf("%" PRId64 "\n", val.data.i64);
            break;
        case DATABOX_UNSIGNED_128: {
            char buf[40];
            const size_t used =
                StrUInt128ToBuf(buf, sizeof(buf), *val.data.u128);
            printf("%.*s\n", (int)used, buf);
            break;
        }
        case DATABOX_SIGNED_128: {
            char buf[41];
            const size_t used =
                StrInt128ToBuf(buf, sizeof(buf), *val.data.i128);
            printf("%.*s\n", (int)used, buf);
            break;
        }
        default:
            break;
        }
    }
}
```

**An iterator is invalidated by any mutation of the set it is walking.**
`Add`, `Remove`, `MergeInto`, and `Intersect` all reshape the bucket map and
can move the `uint32_t *` the iterator caches. Collect first, mutate after:

```c
/* Collect every element below a threshold, then delete them */
intsetBig *doomed = intsetBigNew();

intsetBigIterator iter;
intsetBigIteratorInit(isb, &iter);

databoxBig val = {{0}};
while (intsetBigIteratorNextBox(&iter, &val)) {
    if (val.type == DATABOX_UNSIGNED_64 && val.data.u64 < UINT64_C(1000)) {
        intsetBigAdd(doomed, &val);
    }
}

intsetBigIteratorInit(doomed, &iter);
while (intsetBigIteratorNextBox(&iter, &val)) {
    intsetBigRemove(isb, &val);
}

intsetBigFree(doomed);
```

### Set Operations

```c
/* Intersect 'a' and 'b', adding common elements to 'result'.
 * Returns the number of intersected elements. */
size_t intsetBigIntersect(const intsetBig *a, const intsetBig *b,
                          intsetBig *result);

/* Union: add every element of 'from' to 'into'. */
void intsetBigMergeInto(intsetBig *into, const intsetBig *from);

/* Element-wise equality. */
bool intsetBigEqual(const intsetBig *a, const intsetBig *b);

/* Is every element of 'a' also in 'b'?  True when a == b and when 'a' is
 * empty. */
bool intsetBigSubset(const intsetBig *a, const intsetBig *b);
```

Both `Intersect` and `MergeInto` are **zipper walks over bucket keys**: the two
bucket maps are already sorted, so the two iterators advance in lockstep and
only buckets whose keys compare equal do element-level work. That keeps the
cost proportional to the buckets actually shared rather than to the full
cross-product.

- `Intersect` intersects matching buckets' `intsetU32` arrays with
  `intersectIntAuto`, installs each non-empty result array as a bucket of
  `result`, and drops buckets whose intersection is empty. Pass a **distinct,
  freshly created** `result`.
- `MergeInto` merges each of `from`'s buckets into `into`, copying whole
  buckets that `into` does not have and running `intsetU32Merge` on the ones it
  does. `from` is left unmodified.
- `Equal` short-circuits on differing bucket counts, then compares bucket keys
  and hands the payload comparison to `intsetU32Equal`.
- `Subset` iterates `a` and tests each element with `intsetBigExists(b, ...)`,
  so the argument order reads "does `a` fit inside `b`".

```c
intsetBig *a = intsetBigNew();
intsetBig *b = intsetBigNew();

for (uint64_t i = 1; i <= 4; i++) {
    databoxBig v = DATABOX_BIG_UNSIGNED(i);
    intsetBigAdd(a, &v);
}

for (uint64_t i = 2; i <= 8; i += 2) {
    databoxBig v = DATABOX_BIG_UNSIGNED(i);
    intsetBigAdd(b, &v);
}

/* a = {1,2,3,4}, b = {2,4,6,8} */
intsetBig *common = intsetBigNew();
const size_t shared = intsetBigIntersect(a, b, common);
printf("shared: %zu\n", shared);                       /* 2  ({2,4}) */
printf("subset: %s\n",
       intsetBigSubset(common, a) ? "yes" : "no");     /* yes */

/* Union b into a: a becomes {1,2,3,4,6,8} */
intsetBigMergeInto(a, b);
printf("union size: %zu\n", intsetBigCountElements(a));  /* 6 */

intsetBigFree(a);
intsetBigFree(b);
intsetBigFree(common);
```

### Size Information

```c
/* Number of buckets currently allocated (multimap entries) */
size_t intsetBigCountBuckets(const intsetBig *isb);

/* Number of stored elements (a maintained counter, not a walk) */
size_t intsetBigCountElements(const intsetBig *isb);

/* Total memory: the handle, the bucket multimap, and every bucket's
 * intsetU32 (walks the buckets) */
size_t intsetBigBytes(const intsetBig *isb);
```

```c
intsetBig *isb = intsetBigNew();

for (uint64_t i = 0; i < 1000; i++) {
    databoxBig v = DATABOX_BIG_UNSIGNED(i);
    intsetBigAdd(isb, &v);
}

printf("elements: %zu\n", intsetBigCountElements(isb)); /* 1000 */
printf("buckets:  %zu\n", intsetBigCountBuckets(isb));  /* 1 (all under 2^20) */
printf("bytes:    %zu\n", intsetBigBytes(isb));

intsetBigFree(isb);
```

`intsetBigCountBuckets` is the direct way to observe the bucketing: values
within `2^20` of each other share a bucket, values further apart do not.

### Debug Helpers

```c
#ifdef DATAKIT_TEST
int intsetBigTest(int argc, char *argv[]);
void intsetBigRepr(const intsetBig *isb);
#endif
```

`intsetBigRepr` prints each bucket key followed by that bucket's `intsetU32`
contents. Both are declared only under `DATAKIT_TEST`.

## Performance Characteristics

`B` is the bucket count, `k` the element count of the bucket a value maps to,
and `n` the total element count.

| Operation      | Complexity               | Notes                                                                      |
| -------------- | ------------------------ | -------------------------------------------------------------------------- |
| New            | O(1)                     | one small allocation plus an empty multimap                                |
| Free           | O(B)                     | frees each bucket's intsetU32, then the map                                |
| Copy           | O(B + n)                 | copies the map, then every bucket array                                    |
| Add            | bucket lookup + O(k)     | multimap key lookup, then an intsetU32 insert (search + realloc + memmove) |
| Remove         | bucket lookup + O(k)     | same, plus bucket deletion when the bucket empties                         |
| Exists         | bucket lookup + O(log k) | multimap key lookup, then a binary search                                  |
| Random         | O(B)                     | walks buckets accumulating counts, then one O(1) index                     |
| RandomDelete   | O(B) + Remove            | a Random followed by a Remove of the drawn value                           |
| Iterate (full) | O(n + B)                 | each element yielded once; array pointers cached per bucket                |
| Intersect      | O(B) + per-bucket merges | zipper over bucket keys; matching buckets run intersectIntAuto             |
| MergeInto      | O(B) + per-bucket merges | zipper over bucket keys; matching buckets run intsetU32Merge               |
| Equal          | O(B) + per-bucket memcmp | bucket-count check first, then key compare + intsetU32Equal                |
| Subset         | O(n) lookups             | one Exists in `b` per element of `a`                                       |
| CountBuckets   | multimap count           | delegated to the multimap                                                  |
| CountElements  | O(1)                     | maintained counter                                                         |
| Bytes          | O(B)                     | walks the buckets summing sizes                                            |

The `O(k)` bound on `Add` and `Remove` is why the offset width is capped: `k`
cannot exceed `2^20` no matter how many elements the set holds, so a mutation
never memmoves more than one bucket's worth of data. For actual numbers, build
under the `perf` preset and measure — sanitizer builds are not representative.

## Choosing Between the intset Variants

| Need                                                                         | Use                        |
| ---------------------------------------------------------------------------- | -------------------------- |
| Values in `0 .. UINT32_MAX`, one flat array                                  | `intsetU32`                |
| Signed values in `INT64_MIN .. INT64_MAX`, tight memory for mixed magnitudes | `intset`                   |
| Values past 64 bits, or mixed signed/unsigned wide values                    | `intsetBig`                |
| Merge / intersect / subset / equality built in                               | `intsetU32` or `intsetBig` |
| Sorted iteration through a public iterator                                   | `intsetBig`                |

`intsetBig` costs a multimap lookup on every single-value operation that the
other two do not pay, so reach for it when the range genuinely demands it —
not as a default. Both `intsetU32` and `intsetBig` keep a stable handle;
`intset` does not, which is why only `intset` mutators take `intset **`.

## Best Practices

### 1. Range-Check Before Adding Wide Values

```c
databoxBig v;
DATABOX_BIG_SIGNED_128(&v, candidate);

if (!INTSET_BIG_RANGECHECK(&v)) {
    return false; /* Out of the supported 83-bit signed range */
}

intsetBigAdd(isb, &v);
```

64-bit box types always pass the check, so the guard only costs anything on
the 128-bit paths.

### 2. Keep Values Clustered When You Can

```c
/* One bucket: 1000 nearby values */
for (uint64_t i = 0; i < 1000; i++) {
    databoxBig v = DATABOX_BIG_UNSIGNED(base + i);
    intsetBigAdd(isb, &v);
}

/* 1000 buckets: values spread more than 2^20 apart */
for (uint64_t i = 0; i < 1000; i++) {
    databoxBig v = DATABOX_BIG_UNSIGNED(base + (i << 21));
    intsetBigAdd(isb, &v);
}
```

Both are correct; the second pays a multimap entry, an `intsetU32` handle, and
an allocation per element. Dense identifier spaces are what the structure is
built for.

### 3. Never Mutate While Iterating

Collect into a second `intsetBig` (or an array) and apply the mutations after
the walk finishes. `Add`, `Remove`, `MergeInto`, and `Intersect` all invalidate
an open `intsetBigIterator`.

### 4. Guard Random Calls on Empty Sets

```c
if (intsetBigCountElements(isb) > 0) {
    databoxBig picked = {{0}};
    intsetBigRandomDelete(isb, &picked);
}
```

### 5. Give Intersect a Fresh Result Set

```c
intsetBig *result = intsetBigNew();
const size_t count = intsetBigIntersect(a, b, result);
```

`result` accumulates the intersected buckets; it is not cleared for you, and
aliasing it with an input mixes the input's own buckets into the answer.

## Common Pitfalls

### 1. Shallow-Copying a 128-Bit databoxBig

```c
/* WRONG - 'saved' keeps a pointer into 'val' */
databoxBig val = {{0}};
intsetBigIteratorNextBox(&iter, &val);
databoxBig saved = val;
intsetBigIteratorNextBox(&iter, &val);   /* 'saved' now reads the new value */

/* RIGHT - copy the number out, or re-derive the box */
__uint128_t saved = 0;
if (val.type == DATABOX_UNSIGNED_128) {
    saved = *val.data.u128;
}
```

Only the 128-bit box types carry the internal pointer; 64-bit boxes are
self-contained and copy fine.

### 2. Assuming Signed and Unsigned Zero Are Different Members

```c
databoxBig uzero = DATABOX_BIG_UNSIGNED(0);
databoxBig szero = DATABOX_BIG_SIGNED(0);

intsetBigAdd(isb, &uzero);
intsetBigExists(isb, &szero);           /* true - same member */
intsetBigCountElements(isb);            /* 1 */
```

Membership is numeric, not by box type.

### 3. Storing Out-of-Range Values

Adding a value beyond `INTSET_BIG_UINT128_MAX` or below
`INTSET_BIG_INT128_MIN` gives a bucket key that does not fit its 64-bit slot;
ordering and retrieval guarantees stop holding. Range-check first.

### 4. Passing `&isb` to a Mutator

```c
/* WRONG - these take intsetBig *, not intsetBig ** */
intsetBigAdd(&isb, &val);

/* RIGHT */
intsetBigAdd(isb, &val);
```

The handle is stable; there is no new pointer to write back.

### 5. Expecting Bucket Count to Track Element Count

```c
printf("%zu\n", intsetBigCountBuckets(isb));   /* how many 2^20 spans are used */
printf("%zu\n", intsetBigCountElements(isb));  /* how many values are stored */
```

One bucket can hold up to `2^20` elements, and a set of a thousand widely
spread values has a thousand buckets. Use `intsetBigCountElements` for
cardinality.

## Persistence

`persist.h` registers `PERSIST_TYPE_INTSETBIG` with a `persistOpsIntsetBig`
operations table, so an `intsetBig` can be snapshotted, restored, and driven
through the operation log like the other registered container types.

## See Also

- [intset](INTSET.md) - Tiered signed integer set for `INT64_MIN .. INT64_MAX`
  (per-width arrays, tagged handle)
- [intsetU32](INTSET_U32.md) - Fixed 32-bit unsigned integer set; the leaf
  storage each `intsetBig` bucket is made of
- [hyperloglog](HYPERLOGLOG.md) - Probabilistic cardinality estimation

## Testing

Run the intsetBig test suite:

```bash
./src/datakit-test test intsetBig
```

The test suite validates:

- Bucket and offset derivation across positive, negative, and 128-bit inputs
- Bucket boundary values on both sides of zero
- Round-tripping the extreme supported values through iteration
- Duplicate add, missing remove, and empty-set behavior
- Deep-copy independence
- Ascending iteration order after adds, merges, and intersections
- Multi-bucket `MergeInto` and `Intersect` (disjoint, interleaved, one side
  extending past the other)
- Fuzzers against a reference set for adds, removes, existence, randomness,
  signed values, and 128-bit values
