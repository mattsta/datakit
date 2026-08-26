# intsetU32 - Fixed 32-bit Integer Set

## Overview

`intsetU32` is a **fixed-width sorted set of unsigned 32-bit integers**. Unlike the variable-width `intset`, it always uses exactly 4 bytes per element, providing predictable performance and simpler implementation at the cost of limited range.

**Key Features:**

- Fixed 32-bit unsigned integer storage
- No encoding changes (always 4 bytes per element)
- Sorted storage with O(log n) binary search
- Duplicate-free set semantics
- Additional operations: merge, subset, equality
- Range: 0 to 4,294,967,295 (UINT32_MAX)
- **Stable outer handle**: the `intsetU32 *` never moves, so mutators take
  `intsetU32 *`, not `intsetU32 **`

**Header**: `intsetU32.h`

**Source**: `intsetU32.c`

## Why Use intsetU32 Instead of intset?

### Advantages of intsetU32:

1. **Simpler** - one array, no tiers
2. **Faster** - no tier checks or conversions
3. **Predictable** - always 4 bytes per element
4. **Stable handle** - the pointer survives every mutation
5. **Positional access is O(1)** - no virtual merge to walk
6. **More operations** - merge, subset, equality built in

### Disadvantages of intsetU32:

1. **Limited range** - Only 0 to 4,294,967,295
2. **Less memory efficient for small values** - 4 bytes even for a `1`
3. **Unsigned only** - No negative numbers

**Choose intsetU32 when:**

- All values are unsigned 32-bit (e.g., hash values, counters, IDs)
- Performance is critical and values fit in 32 bits
- You need set operations (merge, subset, equality)

**Choose intset when:**

- Values may be negative
- Values have wide range (some small, some large) — its tiers file each
  value by the narrowest width that holds it
- Values might exceed UINT32_MAX

## Data Structure

```c
typedef struct intsetU32 {
    uint32_t count;      /* Number of elements */
    uint32_t *contents;  /* Separately allocated sorted array */
} intsetU32;
```

The handle is a **fixed-size header**; the element buffer lives in its own
heap block reached through `contents`. An empty set carries a `NULL` buffer.

**Memory Layout:**

```
For intsetU32 with values [5, 100, 500]:

   handle (one allocation)          buffer (a second allocation)
+--------+----------+            +--------------------+
| count  | contents |----------->| [5, 100, 500]      |
|   3    |  ptr     |            | (12 bytes)         |
+--------+----------+            +--------------------+

intsetU32Bytes() = sizeof(intsetU32) + count * 4
```

### The Stable-Handle Invariant

`Add`, `Remove`, `Resize`, `ShrinkToSize`, and `RandomDelete` reallocate
`contents` **in place** and never relocate the handle itself. A pointer to
an `intsetU32` therefore stays valid across any mutation: a keyspace value
slot, a captured key databox, or a deferred snapshot reading it later on
another thread may all keep the _same_ pointer while the buffer moves
underneath it.

Because the handle never moves, the mutators take `intsetU32 *` — there is
no new handle to hand back. This is the opposite convention from `intset`,
whose tagged handle changes on promotion and therefore requires
`intset **`. If you have code passing `&is` to an `intsetU32` mutator, it is
stale.

This is the same contract [intsetBig](INTSET_BIG.md) exposes: its
`{multimap *; count}` header never moves while its buckets grow.

## API Reference

### Creation and Destruction

```c
/* Create new empty intsetU32 */
intsetU32 *intsetU32New(void);

/* Create intsetU32 with pre-allocated space for 'len' elements */
intsetU32 *intsetU32NewLen(uint32_t len);

/* Create deep copy of intsetU32 */
intsetU32 *intsetU32Copy(const intsetU32 *is);

/* Free intsetU32 */
void intsetU32Free(intsetU32 *is);

/* Example */
intsetU32 *is = intsetU32New();
/* ... use it ... */
intsetU32Free(is);

/* intsetU32NewLen pre-sizes the buffer, but note that every Add
 * reallocs to EXACTLY count+1 — so an Add loop discards the
 * pre-allocation immediately.  NewLen is for the direct-array fill
 * pattern below, not for Add loops. */
intsetU32 *big = intsetU32NewLen(10000);
uint32_t *arr = intsetU32Array(big);
for (uint32_t i = 0; i < 10000; i++) {
    arr[i] = i;                 /* must be ascending and duplicate-free */
}
intsetU32UpdateCount(big, 10000);
intsetU32Free(big);

/* Copying */
intsetU32 *original = intsetU32New();
intsetU32Add(original, 42);
intsetU32 *copy = intsetU32Copy(original);
intsetU32Free(original);
intsetU32Free(copy);
```

### Adding Elements

```c
/* Add element to set
 * Returns: true if added (new element), false if duplicate
 */
bool intsetU32Add(intsetU32 *is, uint32_t value);

/* Example */
intsetU32 *is = intsetU32New();

bool added = intsetU32Add(is, 42);
assert(added);  // true - new element

added = intsetU32Add(is, 42);
assert(!added);  // false - duplicate

intsetU32Add(is, 100);
intsetU32Add(is, 50);
/* Set is now: [42, 50, 100] (automatically sorted) */

intsetU32Free(is);
```

### Removing Elements

```c
/* Remove element from set
 * Returns: true if removed, false if not found
 */
bool intsetU32Remove(intsetU32 *is, uint32_t value);

/* Example */
intsetU32 *is = intsetU32New();
intsetU32Add(is, 10);
intsetU32Add(is, 20);
intsetU32Add(is, 30);

bool removed = intsetU32Remove(is, 20);
assert(removed);  // true

removed = intsetU32Remove(is, 99);
assert(!removed);  // false - not found

intsetU32Free(is);
```

### Searching

```c
/* Check if value exists in set */
bool intsetU32Exists(const intsetU32 *is, uint32_t value);

/* Example */
intsetU32 *is = intsetU32New();
intsetU32Add(is, 5);
intsetU32Add(is, 15);
intsetU32Add(is, 25);

if (intsetU32Exists(is, 15)) {
    printf("Found 15!\n");
}

if (!intsetU32Exists(is, 99)) {
    printf("99 not in set\n");
}

intsetU32Free(is);
```

### Accessing by Position

```c
/* Get value at position (0-indexed)
 * Returns: true if position valid, false otherwise
 */
bool intsetU32Get(const intsetU32 *is, uint32_t pos, uint32_t *value);

/* Example: Iterate all elements in sorted order */
intsetU32 *is = intsetU32New();
intsetU32Add(is, 30);
intsetU32Add(is, 10);
intsetU32Add(is, 20);

printf("Elements in sorted order: ");
for (uint32_t i = 0; i < intsetU32Count(is); i++) {
    uint32_t val;
    if (intsetU32Get(is, i, &val)) {
        printf("%u ", val);  // Prints: 10 20 30
    }
}
printf("\n");

intsetU32Free(is);
```

### Random Access

```c
/* Get random element from set.
 * Returns 0 for an empty set — check intsetU32Count() first when 0 is a
 * legitimate member. */
uint32_t intsetU32Random(intsetU32 *is);

/* Remove and return random element
 * deleted: output pointer for removed value
 * Returns: true if removed, false if set was empty
 */
bool intsetU32RandomDelete(intsetU32 *is, uint32_t *deleted);

/* Example: Random operations */
intsetU32 *is = intsetU32New();
for (uint32_t i = 1; i <= 100; i++) {
    intsetU32Add(is, i);
}

/* Get random element */
uint32_t random = intsetU32Random(is);
printf("Random element: %u\n", random);

/* Remove random element */
uint32_t removed;
if (intsetU32RandomDelete(is, &removed)) {
    printf("Randomly removed: %u\n", removed);
}

intsetU32Free(is);
```

### Set Operations

```c
/* Merge set 'b' into set 'dst'
 * Returns: number of new elements added
 */
uint32_t intsetU32Merge(intsetU32 *dst, const intsetU32 *b);

/* Check if two sets are equal */
bool intsetU32Equal(const intsetU32 *a, const intsetU32 *b);

/* Is 'a' a subset of 'b'?  Iterates 'a' and tests membership in 'b',
 * so the argument order reads "does a fit inside b".
 * Returns true when a == b, and true for an empty 'a'. */
bool intsetU32Subset(const intsetU32 *a, const intsetU32 *b);

/* Example: Set operations */
intsetU32 *set1 = intsetU32New();
intsetU32 *set2 = intsetU32New();

intsetU32Add(set1, 1);
intsetU32Add(set1, 2);
intsetU32Add(set1, 3);

intsetU32Add(set2, 2);
intsetU32Add(set2, 3);
intsetU32Add(set2, 4);

/* Merge set2 into set1 */
uint32_t added = intsetU32Merge(set1, set2);
printf("Added %u new elements\n", added);  // Added 1 new elements (4)
// set1 is now [1, 2, 3, 4]

/* Equality check */
intsetU32 *set3 = intsetU32Copy(set1);
assert(intsetU32Equal(set1, set3));  // true

/* Subset check: first argument is the candidate subset */
intsetU32 *subset = intsetU32New();
intsetU32Add(subset, 1);
intsetU32Add(subset, 2);
assert(intsetU32Subset(subset, set1));   /* [1,2] is inside [1,2,3,4] */
assert(!intsetU32Subset(set1, subset));  /* and not the other way around */

intsetU32Free(set1);
intsetU32Free(set2);
intsetU32Free(set3);
intsetU32Free(subset);
```

### Size and Memory

```c
/* Get number of elements */
size_t intsetU32Count(const intsetU32 *is);

/* Get total memory usage in bytes */
size_t intsetU32Bytes(const intsetU32 *is);

/* Example */
intsetU32 *is = intsetU32New();
intsetU32Add(is, 100);
intsetU32Add(is, 200);
intsetU32Add(is, 300);

printf("Count: %zu\n", intsetU32Count(is));         /* 3 */
printf("Memory: %zu bytes\n", intsetU32Bytes(is));  /* sizeof(intsetU32) + 12 */

intsetU32Free(is);
```

`intsetU32Bytes` is `sizeof(intsetU32) + count * sizeof(uint32_t)`. The
handle is a count plus a pointer, so on a 64-bit target the fixed part is 16
bytes after padding, not 4.

### Advanced: Manual Management

```c
/* Get direct pointer to the contents array (NULL for an empty set) */
uint32_t *intsetU32Array(intsetU32 *is);

/* Reallocate the buffer to hold exactly 'len' elements.
 * len == 0 frees the buffer and sets contents to NULL.
 * Does NOT change 'count'. */
void intsetU32Resize(intsetU32 *is, uint32_t len);

/* Shrink allocation to fit current count */
void intsetU32ShrinkToSize(intsetU32 *is);

/* Set the element count directly (use with care!) */
void intsetU32UpdateCount(intsetU32 *is, uint32_t len);

/* Debug dump: "[v0, v1, ..., ]"  (declared only under DATAKIT_TEST) */
void intsetU32Repr(const intsetU32 *is);

/* Example: Bulk build with direct array access */
intsetU32 *is = intsetU32NewLen(1000);
uint32_t *arr = intsetU32Array(is);

/* Fill directly — YOU must keep it ascending and duplicate-free */
for (uint32_t i = 0; i < 1000; i++) {
    arr[i] = i * 2;  /* even numbers */
}
intsetU32UpdateCount(is, 1000);

intsetU32Free(is);
```

The array pointer is invalidated by any call that resizes the buffer
(`Add`, `Remove`, `Resize`, `ShrinkToSize`, `RandomDelete`, `Merge`) — but
the **handle** stays valid across all of them. Re-fetch through
`intsetU32Array()` after any mutation.

Order matters when using `Resize` and `UpdateCount` together: shrink the
count first, then the buffer, so `count` never exceeds the allocation.

## Real-World Examples

### Example 1: Hash Value Set

```c
/* Store unique hash values for deduplication */
typedef struct hashSet {
    intsetU32 *hashes;
} hashSet;

hashSet *hashSetNew(void) {
    hashSet *hs = zcalloc(1, sizeof(*hs));
    hs->hashes = intsetU32New();
    return hs;
}

/* djb2 hash function */
uint32_t hash(const char *str) {
    uint32_t hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

bool hashSetAddString(hashSet *hs, const char *str) {
    uint32_t h = hash(str);
    return intsetU32Add(hs->hashes, h);
}

bool hashSetContainsString(hashSet *hs, const char *str) {
    uint32_t h = hash(str);
    return intsetU32Exists(hs->hashes, h);
}

void hashSetFree(hashSet *hs) {
    intsetU32Free(hs->hashes);
    zfree(hs);
}

/* Usage: Deduplication */
hashSet *seen = hashSetNew();
const char *words[] = {"hello", "world", "hello", "foo", "world"};

for (int i = 0; i < 5; i++) {
    if (hashSetAddString(seen, words[i])) {
        printf("New word: %s\n", words[i]);
    } else {
        printf("Duplicate: %s\n", words[i]);
    }
}
/* Output:
 * New word: hello
 * New word: world
 * Duplicate: hello
 * New word: foo
 * Duplicate: world
 */

printf("Unique words: %zu\n", intsetU32Count(seen->hashes));  // 3
hashSetFree(seen);
```

### Example 2: User Permission Sets

```c
/* Store user permissions as bit flags */
typedef enum permission {
    PERM_READ = 1,
    PERM_WRITE = 2,
    PERM_EXECUTE = 4,
    PERM_DELETE = 8,
    PERM_ADMIN = 16
} permission;

typedef struct user {
    char *name;
    intsetU32 *resourcePermissions;  /* resource_id -> permission bits */
} user;

/* datakit deprecates strdup(); copy explicitly through zmalloc */
user *userNew(const char *name) {
    user *u = zcalloc(1, sizeof(*u));
    const size_t len = strlen(name) + 1;
    u->name = zmalloc(len);
    memcpy(u->name, name, len);
    u->resourcePermissions = intsetU32New();
    return u;
}

/* Grant access to a resource */
void userGrantResource(user *u, uint32_t resourceId) {
    intsetU32Add(u->resourcePermissions, resourceId);
}

/* Revoke access to a resource */
void userRevokeResource(user *u, uint32_t resourceId) {
    intsetU32Remove(u->resourcePermissions, resourceId);
}

/* Check if user can access resource */
bool userCanAccess(user *u, uint32_t resourceId) {
    return intsetU32Exists(u->resourcePermissions, resourceId);
}

/* Copy permissions from one user to another */
void userCopyPermissions(user *from, user *to) {
    intsetU32Merge(to->resourcePermissions, from->resourcePermissions);
}

void userFree(user *u) {
    zfree(u->name);
    intsetU32Free(u->resourcePermissions);
    zfree(u);
}

/* Usage */
user *alice = userNew("Alice");
user *bob = userNew("Bob");

/* Alice gets access to resources 10, 20, 30 */
userGrantResource(alice, 10);
userGrantResource(alice, 20);
userGrantResource(alice, 30);

/* Bob gets access to resources 20, 40 */
userGrantResource(bob, 20);
userGrantResource(bob, 40);

/* Copy Alice's permissions to Bob */
userCopyPermissions(alice, bob);
/* Bob now has: 10, 20, 30, 40 */

printf("Bob can access resource 30: %s\n",
       userCanAccess(bob, 30) ? "yes" : "no");  // yes

userFree(alice);
userFree(bob);
```

### Example 3: Bloom Filter Indices

```c
/* Simple Bloom filter using intsetU32 for hash indices */
#define BLOOM_SIZE 10000

typedef struct bloomFilter {
    uint8_t bits[BLOOM_SIZE];
    intsetU32 *hashIndices;  /* For debugging/stats */
} bloomFilter;

bloomFilter *bloomNew(void) {
    bloomFilter *bf = zcalloc(1, sizeof(*bf));
    bf->hashIndices = intsetU32New();
    return bf;
}

void bloomAdd(bloomFilter *bf, const char *str) {
    /* Use multiple hash functions */
    for (int i = 0; i < 3; i++) {
        uint32_t h = hash(str) + (i * 97);  // Simple multi-hash
        uint32_t idx = h % BLOOM_SIZE;
        bf->bits[idx] = 1;
        intsetU32Add(bf->hashIndices, idx);
    }
}

bool bloomMightContain(bloomFilter *bf, const char *str) {
    for (int i = 0; i < 3; i++) {
        uint32_t h = hash(str) + (i * 97);
        uint32_t idx = h % BLOOM_SIZE;
        if (!bf->bits[idx]) {
            return false;
        }
    }
    return true;  // Might be false positive
}

void bloomPrintStats(bloomFilter *bf) {
    printf("Unique bit positions set: %zu\n",
           intsetU32Count(bf->hashIndices));
    printf("Fill rate: %.2f%%\n",
           (float)intsetU32Count(bf->hashIndices) / BLOOM_SIZE * 100);
}

void bloomFree(bloomFilter *bf) {
    intsetU32Free(bf->hashIndices);
    zfree(bf);
}

/* Usage */
bloomFilter *bf = bloomNew();
bloomAdd(bf, "apple");
bloomAdd(bf, "banana");
bloomAdd(bf, "cherry");

printf("Contains 'apple': %s\n",
       bloomMightContain(bf, "apple") ? "maybe" : "no");  // maybe
printf("Contains 'grape': %s\n",
       bloomMightContain(bf, "grape") ? "maybe" : "no");  // no

bloomPrintStats(bf);
bloomFree(bf);
```

### Example 4: Set Comparison Operations

```c
/* Compute set difference: elements in 'a' but not in 'b' */
intsetU32 *intsetU32Difference(const intsetU32 *a, const intsetU32 *b) {
    intsetU32 *result = intsetU32New();

    for (uint32_t i = 0; i < intsetU32Count(a); i++) {
        uint32_t val;
        intsetU32Get(a, i, &val);
        if (!intsetU32Exists(b, val)) {
            intsetU32Add(result, val);
        }
    }

    return result;
}

/* Compute set intersection: elements in both 'a' and 'b' */
intsetU32 *intsetU32Intersection(const intsetU32 *a, const intsetU32 *b) {
    intsetU32 *result = intsetU32New();

    /* Iterate the smaller set */
    const intsetU32 *smaller = intsetU32Count(a) < intsetU32Count(b) ? a : b;
    const intsetU32 *larger = smaller == a ? b : a;

    for (uint32_t i = 0; i < intsetU32Count(smaller); i++) {
        uint32_t val;
        intsetU32Get(smaller, i, &val);
        if (intsetU32Exists(larger, val)) {
            intsetU32Add(result, val);
        }
    }

    return result;
}

/* Usage */
intsetU32 *evens = intsetU32New();
intsetU32 *primes = intsetU32New();

for (uint32_t i = 2; i <= 20; i += 2) intsetU32Add(evens, i);
intsetU32Add(primes, 2);
intsetU32Add(primes, 3);
intsetU32Add(primes, 5);
intsetU32Add(primes, 7);
intsetU32Add(primes, 11);
intsetU32Add(primes, 13);

/* Difference: evens not in primes */
intsetU32 *diff = intsetU32Difference(evens, primes);
printf("Even non-primes: ");
for (uint32_t i = 0; i < intsetU32Count(diff); i++) {
    uint32_t val;
    intsetU32Get(diff, i, &val);
    printf("%u ", val);  // 4 6 8 10 12 14 16 18 20
}
printf("\n");

/* Intersection: numbers that are both even and prime */
intsetU32 *inter = intsetU32Intersection(evens, primes);
printf("Even primes: ");
for (uint32_t i = 0; i < intsetU32Count(inter); i++) {
    uint32_t val;
    intsetU32Get(inter, i, &val);
    printf("%u ", val);  // 2
}
printf("\n");

intsetU32Free(evens);
intsetU32Free(primes);
intsetU32Free(diff);
intsetU32Free(inter);
```

## Performance Characteristics

| Operation       | Complexity | Notes                                               |
| --------------- | ---------- | --------------------------------------------------- |
| Create          | O(1)       | one small allocation; buffer is NULL until needed   |
| Add             | O(n)       | binary search + exact-size realloc + memmove        |
| Remove          | O(n)       | binary search + memmove + exact-size realloc        |
| Exists          | O(log n)   | binary search, with an O(1) "greater than max" exit |
| Get by position | O(1)       | direct array index                                  |
| Random          | O(1)       | random index; returns 0 when empty                  |
| RandomDelete    | O(n)       | random pick then Remove                             |
| Merge           | O(m\*n)    | Add per element of the source, each an O(n) insert  |
| Equal           | O(n)       | single memcmp when counts match, else O(1) false    |
| Subset          | O(n log m) | binary-search each member of `a` inside `b`         |
| Count           | O(1)       | stored counter                                      |
| Bytes           | O(1)       | header size plus count \* 4                         |

**Comparison with intset:**

- **Faster**: no tier dispatch, and `Get` is a direct index
- **More predictable**: always 4 bytes per element, one buffer
- **Stable handle**: no `intsetU32 **` plumbing through your call graph
- **Worse for mixed magnitudes**: a set of mostly-tiny values pays 4 bytes
  each, where `intset`'s Small tier would pay 2

## Memory Efficiency

```c
intsetU32 *is = intsetU32NewLen(1000);
uint32_t *arr = intsetU32Array(is);
for (uint32_t i = 0; i < 1000; i++) {
    arr[i] = i;
}
intsetU32UpdateCount(is, 1000);

printf("Total: %zu bytes\n", intsetU32Bytes(is));
/* sizeof(intsetU32) + 1000 * 4 */

intsetU32Free(is);
```

**Comparison with other structures, 1000 elements:**

```
- intsetU32:         header + 4,000 bytes of payload, two allocations
- Array of uint32_t: 4,000 bytes, one allocation, no dedup or ordering
- Hash table:        several times larger (buckets, pointers, load factor)
- Linked list:       12,000+ bytes (value + pointer + allocator header each)
```

## Best Practices

### 1. Use for Known 32-bit Ranges

```c
/* GOOD - IDs that fit in 32 bits */
intsetU32 *userIds = intsetU32New();
intsetU32Add(userIds, 12345);
intsetU32Add(userIds, 67890);

/* GOOD - Hash values */
intsetU32 *hashes = intsetU32New();
intsetU32Add(hashes, 0xDEADBEEF);

/* BAD - Values might exceed UINT32_MAX */
// intsetU32Add(userIds, 5000000000ULL);  // Truncated! Use intset instead
```

### 2. Build in Bulk Through the Array, Not Through Add

`intsetU32Add` calls `intsetU32Resize(is, count + 1)` every time, which is an
exact-size `zrealloc`. There is no capacity, so `intsetU32NewLen` cannot make
an `Add` loop cheaper — the first `Add` shrinks the buffer back to one
element.

```c
/* SLOW - one exact-size realloc per element */
intsetU32 *is = intsetU32New();
for (uint32_t i = 0; i < 10000; i++) {
    intsetU32Add(is, i);
}

/* FAST - one allocation, filled directly.
 * You are responsible for ascending order and no duplicates. */
intsetU32 *is2 = intsetU32NewLen(10000);
uint32_t *arr = intsetU32Array(is2);
for (uint32_t i = 0; i < 10000; i++) {
    arr[i] = i;
}
intsetU32UpdateCount(is2, 10000);
```

If the source data is not already sorted and deduplicated, sort and unique
it before the fill — every other operation (`Exists`, `Add`, `Remove`,
`Subset`, `Equal`) assumes a sorted, duplicate-free array and will
misbehave otherwise.

### 3. Use the Built-in Set Operations

```c
/* Hand-rolled */
for (uint32_t i = 0; i < intsetU32Count(setB); i++) {
    uint32_t val;
    intsetU32Get(setB, i, &val);
    intsetU32Add(setA, val);
}

/* Same work, and it reports how many were actually new */
const uint32_t added = intsetU32Merge(setA, setB);
```

`intsetU32Merge` _is_ that loop internally, so it is not faster — it is
shorter, and it returns the count of newly added elements. Both forms are
O(m\*n) because each insert reallocs and memmoves. For a large union,
build the merged array yourself and install it with `Array` +
`UpdateCount`.

### 4. Check Results for Set Operations

```c
/* Check merge result */
uint32_t added = intsetU32Merge(dest, src);
if (added > 0) {
    printf("Added %u new elements\n", added);
}

/* Check equality */
if (intsetU32Equal(set1, set2)) {
    printf("Sets are identical\n");
}

/* Check subset relationship */
if (intsetU32Subset(small, large)) {
    printf("small is a subset of large\n");
}
```

## Common Pitfalls

### 1. Value Truncation

```c
/* BAD - silently truncates! */
intsetU32 *is = intsetU32New();
uint64_t bigValue = 5000000000ULL;  // > UINT32_MAX
intsetU32Add(is, (uint32_t)bigValue);  // Truncated to 705032704!

/* GOOD - check range first */
if (bigValue <= UINT32_MAX) {
    intsetU32Add(is, (uint32_t)bigValue);
} else {
    // Use intset or intsetBig instead
}
```

### 2. Signed vs Unsigned

```c
/* BAD - negative values don't work! */
intsetU32 *is = intsetU32New();
int32_t negative = -100;
intsetU32Add(is, (uint32_t)negative);  // Becomes large positive: 4294967196

/* GOOD - use intset for signed values */
intset *signedSet = intsetNew();
intsetAdd(&signedSet, -100, NULL);  // Correctly stores -100
```

### 3. Passing `&is` to a Mutator

```c
/* WRONG - these take intsetU32 *, not intsetU32 **.
 * The handle is stable, so there is no new pointer to write back. */
intsetU32Add(&is, 42);

/* RIGHT */
intsetU32Add(is, 42);
```

Older code written against a flexible-array-member layout used `&is`. The
element buffer is now a separate allocation and the handle never moves, so
the double pointer is gone from `Add`, `Remove`, `Merge`, `Resize`,
`ShrinkToSize`, and `RandomDelete`.

### 4. Holding a Stale Array Pointer

```c
/* WRONG - Add reallocs the buffer */
uint32_t *arr = intsetU32Array(is);
intsetU32Add(is, 42);
arr[0] = 7;                   /* writes into freed memory */

/* RIGHT - re-fetch after any mutation */
intsetU32Add(is, 42);
uint32_t *arr = intsetU32Array(is);
```

### 5. Assuming Sorted Iteration

```c
/* This is already sorted! No need to re-sort */
intsetU32 *is = intsetU32New();
intsetU32Add(is, 30);
intsetU32Add(is, 10);
intsetU32Add(is, 20);

/* Elements are automatically sorted: [10, 20, 30] */
for (uint32_t i = 0; i < intsetU32Count(is); i++) {
    uint32_t val;
    intsetU32Get(is, i, &val);
    printf("%u ", val);  // Always prints in order: 10 20 30
}
```

## See Also

- [intset](INTSET.md) - Tiered signed integer set (per-width arrays, tagged handle)
- [intsetBig](INTSET_BIG.md) - Bucketed wide-range integer set built from intsetU32 buckets
- [hyperloglog](HYPERLOGLOG.md) - Probabilistic cardinality estimation

## Testing

Run the intsetU32 test suite:

```bash
./src/datakit-test test intsetU32
```

The test suite validates:

- Basic insertion and removal
- Duplicate detection
- Binary search correctness
- Merge operations
- Equality and subset checks
- Stress tests with random data
