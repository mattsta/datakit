# intset - Variable-Width Integer Set

The lifecycle, `intsetStats`, the tier rules and `intsetCompact` are shared
with the other tiered containers: [Tiered containers](../core/TIERS.md).

## Overview

`intset` is a **memory-efficient sorted set of signed integers** built as a
tiered structure. Each width class (`int16_t`, `int32_t`, `int64_t`) lives in
its own sorted array, so adding one large value no longer widens every
element that was already stored.

**Key Features:**

- Three tiers selected by pointer tagging: Small, Medium, Full
- Per-width sorted arrays with branchless binary search
- Duplicate-free set semantics
- O(1) `intsetMin` / `intsetMax`
- Range: `INT64_MIN` to `INT64_MAX`

**Header**: `intset.h`

**Source**: `intset.c`, plus the tier implementations `intsetSmall.{c,h}`,
`intsetMedium.{c,h}`, `intsetFull.{c,h}`, and shared helpers in
`intsetCommon.h`

## What are Integer Sets?

Integer sets are specialized data structures for storing sorted collections of unique integers efficiently. Unlike hash sets or tree sets, integer sets:

1. **Use compact array storage** - Elements stored in contiguous memory
2. **Automatically size themselves** - Start small (16-bit) and grow as needed
3. **Maintain sorted order** - Binary search for lookups
4. **Prevent duplicates** - Set semantics built-in
5. **Trade CPU for memory** - Slower inserts/deletes, but minimal memory overhead

### When to Use Integer Sets

**Use intset when:**

- Storing unique integer IDs or identifiers
- Memory efficiency is critical
- Elements are primarily added, rarely removed
- Membership tests (`intsetFind`) dominate the workload

**Don't use intset when:**

- Frequent insertions/deletions at scale
- Elements are mostly non-integers
- You need O(1) lookup (use a hash table instead)
- You need positional iteration over a Medium/Full tier — `intsetGet` walks
  the virtual merge, so an index loop is quadratic (see
  [Iteration](#iteration))
- Values exceed 64 bits (use [intsetBig](INTSET_BIG.md))

## Data Structure

### Opaque Handle, Tagged Pointer

```c
/* intset is opaque: the tier lives in the low bits of the pointer */
typedef struct intset intset;
```

There is no public struct. `intset *` is a **tagged pointer**: its low two
bits carry the tier, and the rest points at the tier's own header. Never
dereference an `intset *` — every field access must go through the API.

```c
typedef enum intsetType {
    INTSET_TYPE_SMALL  = 1,  /* int16_t only */
    INTSET_TYPE_MEDIUM = 2,  /* int16_t and int32_t */
    INTSET_TYPE_FULL   = 3,  /* int16_t, int32_t, and int64_t */
} intsetType;
```

`intsetEnc` still exists in `intset.h` for backward compatibility, and its
values are element widths in bytes:

```c
typedef enum intsetEnc {
    INTSET_ENC_INT16 = sizeof(int16_t),  /* 2 */
    INTSET_ENC_INT32 = sizeof(int32_t),  /* 4 */
    INTSET_ENC_INT64 = sizeof(int64_t)   /* 8 */
} intsetEnc;
```

It no longer describes the whole set — a Medium or Full intset holds several
widths at once.

### The Three Tiers

Each tier keeps **one sorted, duplicate-free array per width**:

```c
/* Small: a single inline array — the header and values are one allocation */
typedef struct intsetSmall {
    uint32_t count16;
    int16_t values16[];
} intsetSmall;

/* Medium: two independently allocated arrays */
typedef struct intsetMedium {
    uint32_t count16;
    uint32_t count32;
    int16_t *values16;
    int32_t *values32;
} intsetMedium;

/* Full: three independently allocated arrays */
typedef struct intsetFull {
    uint64_t count16;
    uint64_t count32;
    uint64_t count64;
    int16_t *values16;
    int32_t *values32;
    int64_t *values64;
} intsetFull;
```

A value is filed by the **narrowest width that holds it**, so `5` stays two
bytes even in a Full intset that also holds `INT64_MAX`. Sorted order across
the whole set is a _virtual merge_ of the per-width arrays; it is never
materialized.

## Tier Promotion

### What Triggers an Upgrade

Promotion happens before an add, and for either of two reasons — the value
does not fit the tier's widths, or the tier has grown past its size budget:

| From   | To     | Value trigger           | Size triggers                         |
| ------ | ------ | ----------------------- | ------------------------------------- |
| Small  | Medium | value outside `int16_t` | 32768 elements, or 64 KiB of storage  |
| Medium | Full   | value outside `int32_t` | 2097152 elements, or 8 MiB of storage |

Full is terminal: there is no further tier, and every 64-bit value is
representable there.

**Promotion is one-way.** Removing the value that forced an upgrade does not
demote the set back down. What it does _not_ do — and this is the difference
from a flat width-promoting design — is rewrite the narrow elements: the
`int16_t` array stays an `int16_t` array across every promotion.

### How Promotion Works

```c
intset *is = intsetNew();       /* Small tier */

intsetAdd(&is, 100, NULL);      /* Small: values16 = [100] */

/* 50000 does not fit int16_t -> upgrade to Medium.
 * 100 stays a 2-byte value; 50000 lands in the int32 array. */
intsetAdd(&is, 50000, NULL);    /* Medium: values16=[100], values32=[50000] */

/* INT64_MAX does not fit int32_t -> upgrade to Full.
 * The 16- and 32-bit arrays are carried over unchanged. */
intsetAdd(&is, INT64_MAX, NULL);
/* Full: values16=[100], values32=[50000], values64=[INT64_MAX] */
```

Because the tier is encoded in the pointer, **every mutating call may return
a different `intset *`** — both for reallocation and for a tier change. That
is why `intsetAdd` and `intsetRemove` take `intset **`.

Removes never move a set down a tier: one add of a wide value would move it
straight back up. When the wide values are gone for good,
`intsetCompact(&is)` moves the set to the narrowest tier its values allow.
A Full with no 64-bit values becomes a Medium, its arrays moved rather than
copied. A Medium with no 32-bit values becomes a Small.

## API Reference

### Creation and Destruction

```c
/* Create new empty intset (starts with INT16 encoding) */
intset *intsetNew(void);

/* Free intset */
void intsetFree(intset *is);

/* Empty it (a new Small), a deep copy, and the narrowest tier */
void intsetReset(intset **is);
intset *intsetCopy(const intset *is);
void intsetCompact(intset **is);

/* Example */
intset *ids = intsetNew();
/* ... use it ... */
intsetFree(ids);
```

### Adding Elements

```c
/* Add integer to set
 * is: pointer to intset pointer (may be reallocated)
 * value: integer to add
 * success: optional bool pointer, set to true if added, false if duplicate
 */
void intsetAdd(intset **is, int64_t value, bool *success);

/* Example: Basic add */
intset *is = intsetNew();
bool added;

intsetAdd(&is, 42, &added);
assert(added);  // true - first time

intsetAdd(&is, 42, &added);
assert(!added);  // false - duplicate

intsetAdd(&is, 100, NULL);  // success parameter is optional
intsetFree(is);
```

**Important**: Pass address of intset pointer (`&is`) because reallocation may move the structure.

### Removing Elements

```c
/* Remove integer from set
 * is: pointer to intset pointer (may be reallocated)
 * value: integer to remove
 * success: optional bool pointer, set to true if removed, false if not found
 */
void intsetRemove(intset **is, int64_t value, bool *success);

/* Example */
intset *is = intsetNew();
intsetAdd(&is, 10, NULL);
intsetAdd(&is, 20, NULL);
intsetAdd(&is, 30, NULL);

bool removed;
intsetRemove(&is, 20, &removed);
assert(removed);  // true

intsetRemove(&is, 99, &removed);
assert(!removed);  // false - doesn't exist

intsetFree(is);
```

**Note**: Removal does NOT demote the tier. A set that was pushed to Full
stays Full, keeping its (now possibly empty) wider arrays and their headers.
The narrow values were never widened, though, so the cost of that is a few
pointers and counters, not a per-element penalty.

### Searching

```c
/* Check if value exists in set */
bool intsetFind(intset *is, int64_t value);

/* Example */
intset *is = intsetNew();
intsetAdd(&is, 5, NULL);
intsetAdd(&is, 15, NULL);
intsetAdd(&is, 25, NULL);

if (intsetFind(is, 15)) {
    printf("Found 15!\n");
}

if (!intsetFind(is, 99)) {
    printf("99 not found\n");
}

intsetFree(is);
```

### Accessing by Position

```c
/* Get value at position (0-indexed) in the merged sorted order.
 * pos: position in sorted order
 * value: output pointer for the value
 * Returns: true if position valid, false otherwise
 *
 * On the Medium and Full tiers this walks the virtual merge from the
 * start, so it is O(pos) — NOT O(1). */
bool intsetGet(intset *is, uint32_t pos, int64_t *value);

/* O(1) extremes; false when the set is empty */
bool intsetMin(const intset *is, int64_t *value);
bool intsetMax(const intset *is, int64_t *value);

/* Example */
intset *is = intsetNew();
intsetAdd(&is, 30, NULL);
intsetAdd(&is, 10, NULL);
intsetAdd(&is, 20, NULL);

int64_t lo = 0;
int64_t hi = 0;
if (intsetMin(is, &lo) && intsetMax(is, &hi)) {
    printf("range %" PRId64 "..%" PRId64 "\n", lo, hi);  /* range 10..30 */
}

intsetFree(is);
```

`intsetMin` / `intsetMax` are O(1) because every per-width array is sorted:
the global minimum is the smallest of the array heads and the global maximum
is the largest of the array tails.

### Iteration

```c
/* WRONG for anything but a Small intset — O(n^2) total.
 * Each intsetGet re-walks the merge from position 0. */
for (uint32_t i = 0; i < intsetCount(is); i++) {
    int64_t val;
    intsetGet(is, i, &val);
    consume(val);
}
```

The public `intset.h` API deliberately exposes no iterator: the merged
sorted order is virtual, so there is no cheap way to hand out a cursor
through the opaque handle. When you need every element:

- If **sorted order matters**, keep the count small enough that the
  quadratic walk is acceptable, or track the values alongside the set.
- If **order does not matter**, iterate each width array directly by
  including the tier header (`intsetSmall.h` / `intsetMedium.h` /
  `intsetFull.h`), which also declare per-tier iterators.

`intsetRandom(is)` is likewise O(pos) on Medium and Full: it picks a random
position and calls `intsetGet`. It returns `0` for an empty set, which is
indistinguishable from a stored `0` — check `intsetCount` first.

### Random Access

```c
/* Get random element from set */
int64_t intsetRandom(intset *is);

/* Example: Random sampling */
intset *is = intsetNew();
for (int i = 1; i <= 100; i++) {
    intsetAdd(&is, i, NULL);
}

/* Get 5 random samples */
for (int i = 0; i < 5; i++) {
    int64_t sample = intsetRandom(is);
    printf("%" PRId64 " ", sample);
}
printf("\n");

intsetFree(is);
```

### Size Information

```c
/* Get number of elements */
size_t intsetCount(const intset *is);

/* Get total memory usage in bytes */
size_t intsetBytes(intset *is);

/* Example */
intset *is = intsetNew();
intsetAdd(&is, 100, NULL);
intsetAdd(&is, 200, NULL);
intsetAdd(&is, 300, NULL);

printf("Elements: %zu\n", intsetCount(is));     /* 3 */
printf("Memory: %zu bytes\n", intsetBytes(is)); /* Small tier: 4 + 3*2 = 10 */

intsetFree(is);
```

`intsetBytes` reports the current tier's storage: the header plus the
populated width arrays. Only the Small tier is a single allocation (its
values are inline); Medium and Full hold separately allocated arrays, so the
reported total spans several allocations.

## Real-World Examples

### Example 1: User ID Set

```c
/* Store active user IDs efficiently */
intset *activeUsers = intsetNew();

/* User logs in */
void userLogin(int64_t userId) {
    bool wasNew;
    intsetAdd(&activeUsers, userId, &wasNew);
    if (wasNew) {
        printf("User %" PRId64 " logged in (new session)\n", userId);
    } else {
        printf("User %" PRId64 " already active\n", userId);
    }
}

/* User logs out */
void userLogout(int64_t userId) {
    bool existed;
    intsetRemove(&activeUsers, userId, &existed);
    if (existed) {
        printf("User %" PRId64 " logged out\n", userId);
    }
}

/* Check if user is active */
bool isUserActive(int64_t userId) {
    return intsetFind(activeUsers, userId);
}

/* Get count of active users */
size_t getActiveUserCount(void) {
    return intsetCount(activeUsers);
}

/* Example usage */
userLogin(1001);  // User 1001 logged in (new session)
userLogin(1002);  // User 1002 logged in (new session)
userLogin(1001);  // User 1001 already active
printf("Active: %zu\n", getActiveUserCount());  // Active: 2
userLogout(1001);  // User 1001 logged out
printf("Active: %zu\n", getActiveUserCount());  // Active: 1
```

### Example 2: Sorted Tag IDs

```c
/* Store article tag IDs in sorted order */
typedef struct article {
    char *title;
    intset *tags;
} article;

/* datakit deprecates strdup(); copy explicitly through zmalloc */
article *articleNew(const char *title) {
    article *a = zcalloc(1, sizeof(*a));
    const size_t len = strlen(title) + 1;
    a->title = zmalloc(len);
    memcpy(a->title, title, len);
    a->tags = intsetNew();
    return a;
}

void articleAddTag(article *a, int64_t tagId) {
    intsetAdd(&a->tags, tagId, NULL);
}

bool articleHasTag(article *a, int64_t tagId) {
    return intsetFind(a->tags, tagId);
}

/* Tag IDs are small and few, so this stays on the Small tier where
 * intsetGet is a direct array index. */
void articlePrintTags(article *a) {
    printf("Tags for '%s': ", a->title);
    for (uint32_t i = 0; i < intsetCount(a->tags); i++) {
        int64_t tag = 0;
        if (intsetGet(a->tags, i, &tag)) {
            printf("%" PRId64 " ", tag);
        }
    }
    printf("\n");
}

void articleFree(article *a) {
    zfree(a->title);
    intsetFree(a->tags);
    zfree(a);
}

/* Usage */
article *post = articleNew("Understanding intsets");
articleAddTag(post, 42);   // programming
articleAddTag(post, 17);   // data-structures
articleAddTag(post, 99);   // tutorial
articleAddTag(post, 17);   // duplicate - ignored

articlePrintTags(post);  // Tags for 'Understanding intsets': 17 42 99
articleFree(post);
```

### Example 3: Tier Promotion Demonstration

The tier is not observable through the public API — `intset` is opaque and
there is no `is->encoding` to read. What you _can_ observe is `intsetBytes`,
which reflects both the tier's header and its populated arrays.

```c
intset *is = intsetNew();               /* Small tier */

intsetAdd(&is, 100, NULL);
intsetAdd(&is, 200, NULL);
printf("two small values: %zu bytes\n", intsetBytes(is));

/* 100000 needs int32_t -> promotes to Medium.
 * 100 and 200 stay in the int16 array at 2 bytes each. */
intsetAdd(&is, 100000, NULL);
printf("after adding 100000: %zu bytes\n", intsetBytes(is));

/* Removing it frees the int32 slot but does NOT demote the tier */
bool removed = false;
intsetRemove(&is, 100000, &removed);
printf("after removing 100000: %zu bytes\n", intsetBytes(is));

intsetFree(is);
```

Under `DATAKIT_TEST` builds, `intsetRepr(is)` prints the tier and its arrays
directly.

```c
#ifdef DATAKIT_TEST
void intsetRepr(const intset *is);
#endif
```

### Example 4: Set Operations (Manual Implementation)

```c
/* Union of two intsets */
intset *intsetUnion(intset *a, intset *b) {
    intset *result = intsetNew();

    /* Add all elements from a */
    for (uint32_t i = 0; i < intsetCount(a); i++) {
        int64_t val;
        intsetGet(a, i, &val);
        intsetAdd(&result, val, NULL);
    }

    /* Add all elements from b (duplicates ignored) */
    for (uint32_t i = 0; i < intsetCount(b); i++) {
        int64_t val;
        intsetGet(b, i, &val);
        intsetAdd(&result, val, NULL);
    }

    return result;
}

/* Intersection of two intsets */
intset *intsetIntersect(intset *a, intset *b) {
    intset *result = intsetNew();

    /* For each element in a, check if it's in b */
    for (uint32_t i = 0; i < intsetCount(a); i++) {
        int64_t val;
        intsetGet(a, i, &val);
        if (intsetFind(b, val)) {
            intsetAdd(&result, val, NULL);
        }
    }

    return result;
}

/* Usage */
intset *set1 = intsetNew();
intset *set2 = intsetNew();

intsetAdd(&set1, 1, NULL);
intsetAdd(&set1, 2, NULL);
intsetAdd(&set1, 3, NULL);

intsetAdd(&set2, 2, NULL);
intsetAdd(&set2, 3, NULL);
intsetAdd(&set2, 4, NULL);

intset *unionSet = intsetUnion(set1, set2);
printf("Union count: %zu\n", intsetCount(unionSet));  // 4: [1,2,3,4]

intset *intersectSet = intsetIntersect(set1, set2);
printf("Intersect count: %zu\n", intsetCount(intersectSet));  // 2: [2,3]

intsetFree(set1);
intsetFree(set2);
intsetFree(unionSet);
intsetFree(intersectSet);
```

## Performance Characteristics

| Operation        | Complexity                     | Notes                                                     |
| ---------------- | ------------------------------ | --------------------------------------------------------- |
| Create           | O(1)                           | one small allocation                                      |
| Add (no promote) | O(n)                           | branchless binary search + memmove within one width array |
| Add (promote)    | O(n)                           | copies the existing arrays into the new tier              |
| Remove           | O(n)                           | binary search + memmove within one width array            |
| Find             | O(log n)                       | branchless lower-bound with prefetch, per width array     |
| Get by position  | O(1) Small, O(pos) Medium/Full | walks the virtual merge on multi-array tiers              |
| Min / Max        | O(1)                           | array heads / tails                                       |
| Random           | O(1) Small, O(pos) Medium/Full | picks a position, then `intsetGet`                        |
| Count            | O(1)                           | summed counters                                           |
| Bytes            | O(1)                           | summed array sizes                                        |

The binary search is branchless (the midpoint step compiles to a
conditional move) and issues two prefetches per level.

`./build/perf/intsetSearchBench` (10M random probes) compares it with a
branchy search. Medians of 3 runs on an Apple M1 Max, clang 22 `perf`
preset:

| Elements  | Speed-up |
| --------- | -------- |
| 64        | 2.4x     |
| 1,024     | 2.0x     |
| 65,536    | 1.5x     |
| 1,048,576 | 1.5x     |
| 2,097,152 | 1.3x     |

The memory-bound large sizes vary from run to run: at 1M elements the three
runs gave 1.0x, 1.5x and 1.9x.

## Memory Efficiency

### The Tier Advantage

The point of the tiered design is that **a wide value costs only its own
width**. A flat width-promoting set widens every element; a tiered set does
not:

```c
/* 999 small values plus one huge one */
intset *mixed = intsetNew();
for (int i = 1; i <= 999; i++) {
    intsetAdd(&mixed, i, NULL);     /* int16 array */
}
intsetAdd(&mixed, INT64_MAX, NULL); /* promotes to Full */

printf("Memory: %zu bytes\n", intsetBytes(mixed));
/* The 999 small values are still 2 bytes each; only INT64_MAX
 * costs 8.  A flat int64 promotion would have charged 8 bytes
 * for all 1000. */

intsetFree(mixed);
```

The residual cost of a promotion is the wider tier's header — extra counters
and array pointers — not a per-element penalty.

### Where the Bytes Go

| Tier   | Header                                   | Arrays                     |
| ------ | ---------------------------------------- | -------------------------- |
| Small  | one `uint32_t` count, values inline      | single allocation          |
| Medium | two `uint32_t` counts + two pointers     | two separate allocations   |
| Full   | three `uint64_t` counts + three pointers | three separate allocations |

`intsetBytes` sums the header and the populated arrays across all of those
allocations.

## Best Practices

### 1. Pass Pointer to Pointer

```c
/* WRONG - the handle is a TAGGED pointer; a promotion or a realloc
 * makes the old value both stale and mis-tagged. */
intset *is = intsetNew();
intset *alias = is;
intsetAdd(&is, 100000, NULL);   /* may promote Small -> Medium */
intsetFind(alias, 100000);      /* reads a freed, wrongly-tagged pointer */

/* RIGHT - one owner, always updated through the API */
intset *is = intsetNew();
intsetAdd(&is, 100, NULL);
intsetAdd(&is, 100000, NULL);
```

Never keep a second copy of an `intset *` across a mutation, and never
compute anything from its bits — the low two bits are the tier tag.

### 2. Let the Tiers Do Their Job

```c
/* Fine.  The wide value costs 8 bytes; nothing else is affected. */
intset *mixed = intsetNew();
intsetAdd(&mixed, 1, NULL);
intsetAdd(&mixed, 5000000000LL, NULL);
```

There is no "add the biggest value first" trick to play any more: filing by
width means insertion order does not change the final layout. The only thing
still worth avoiding is repeatedly crossing a tier boundary, since each
promotion copies the existing arrays.

### 3. Prefer Find and Min/Max Over Positional Access

```c
/* GOOD - O(log n), no merge walk */
if (intsetFind(is, needle)) { ... }

int64_t lo = 0;
if (intsetMin(is, &lo)) { ... }        /* O(1) */

/* AVOID on Medium/Full - O(n^2) across the loop */
for (uint32_t i = 0; i < intsetCount(is); i++) {
    int64_t v;
    intsetGet(is, i, &v);
}
```

### 4. Check Operation Results

```c
/* Use success parameter to detect duplicates */
intset *is = intsetNew();
bool added;

intsetAdd(&is, 42, &added);
if (added) {
    printf("Added new element\n");
} else {
    printf("Element already exists\n");
}

/* Use success for removal too */
bool removed;
intsetRemove(&is, 42, &removed);
if (removed) {
    printf("Element removed\n");
} else {
    printf("Element not found\n");
}
```

## Common Pitfalls

### 1. Expecting a Tier to Demote

```c
intset *is = intsetNew();
intsetAdd(&is, INT64_MAX, NULL);     /* promotes all the way to Full */
intsetRemove(&is, INT64_MAX, NULL);  /* removed, but the tier stays Full */
```

The int64 array is emptied, so the per-element cost is gone; what remains is
the Full header. If you truly need to shed that, rebuild:

```c
intset *rebuilt = intsetNew();
for (uint32_t i = 0; i < intsetCount(is); i++) {
    int64_t val = 0;
    if (intsetGet(is, i, &val)) {
        intsetAdd(&rebuilt, val, NULL);
    }
}
intsetFree(is);
is = rebuilt;
```

That rebuild is O(n^2) on Medium/Full because of `intsetGet` — only worth it
for small sets, or drive it from the per-tier iterators instead.

### 2. Not Checking Return Values

```c
/* BAD - ignoring the result */
int64_t val;
intsetGet(is, 1000, &val);            /* leaves val untouched if invalid */
printf("%" PRId64 "\n", val);         /* reads uninitialized memory */

/* GOOD - check the return */
int64_t val = 0;
if (intsetGet(is, 1000, &val)) {
    printf("%" PRId64 "\n", val);
} else {
    printf("Invalid position\n");
}
```

`intsetRandom` has no such signal: it returns `0` both for an empty set and
for a set containing `0`. Guard with `intsetCount`.

### 3. Inefficient Set Operations

```c
/* SLOW - nested positional scans; each intsetGet may walk the merge */
for (uint32_t i = 0; i < intsetCount(a); i++) {
    int64_t val;
    intsetGet(a, i, &val);
    for (uint32_t j = 0; j < intsetCount(b); j++) {
        int64_t val2;
        intsetGet(b, j, &val2);
        if (val == val2) { /* found */ }
    }
}

/* FASTER - binary search the second set */
for (uint32_t i = 0; i < intsetCount(a); i++) {
    int64_t val;
    intsetGet(a, i, &val);
    if (intsetFind(b, val)) { /* found */ }
}
```

Even the second form still pays `intsetGet`'s merge walk on Medium and Full.
For real set algebra over large sets, iterate the width arrays via the tier
headers, or reach for a container built for it.

## See Also

- [intsetU32](INTSET_U32.md) - Fixed 32-bit unsigned integer set (stable handle, single array)
- [intsetBig](INTSET_BIG.md) - Bucketed wide-range integer set for 128-bit inputs
- [hyperloglog](HYPERLOGLOG.md) - Probabilistic cardinality estimation
- [flex](../flex/FLEX.md) - Flexible arrays that can use intset internally

## Testing

Run the intset test suite:

```bash
./src/datakit-test test intset
```

The test suite validates:

- Per-width value classification
- Tier promotion (Small → Medium → Full) by value and by size budget
- Binary search correctness (including the branchless lower bound)
- Insertion and deletion
- Sorted order maintenance across the virtual merge
- Stress tests with random operations
