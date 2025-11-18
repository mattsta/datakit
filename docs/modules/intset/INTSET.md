# intset - Variable-Width Integer Set

## Overview

`intset` is a **memory-efficient sorted set of integers** with automatic width promotion. It stores integers in the smallest possible encoding (16/32/64-bit) that can hold all current elements, automatically upgrading when larger values are added.

**Key Features:**
- Automatic encoding promotion (16-bit → 32-bit → 64-bit)
- Sorted storage with O(log n) binary search
- Compact memory representation
- Duplicate-free set semantics
- No encoding downgrade (memory never shrinks)
- Range: INT16_MIN to INT64_MAX

**Header**: `intset.h`

**Source**: `intset.c`

**Origin**: Adapted from Redis (Pieter Noordhuis, Salvatore Sanfilippo)

## What are Integer Sets?

Integer sets are specialized data structures for storing sorted collections of unique integers efficiently. Unlike hash sets or tree sets, integer sets:

1. **Use compact array storage** - Elements stored in contiguous memory
2. **Automatically size themselves** - Start small (16-bit) and grow as needed
3. **Maintain sorted order** - Binary search for O(log n) lookups
4. **Prevent duplicates** - Set semantics built-in
5. **Trade CPU for memory** - Slower inserts/deletes, but minimal memory overhead

### When to Use Integer Sets

**Use intset when:**
- Storing unique integer IDs or identifiers
- Memory efficiency is critical
- Elements are primarily added, rarely removed
- Set size is small to medium (< 100K elements)
- You need sorted iteration

**Don't use intset when:**
- Frequent insertions/deletions at scale
- Elements are mostly non-integers
- Need O(1) lookup (use hash table instead)
- Very large sets (> 1M elements, consider intsetBig)

## Data Structure

### Encoding Types

```c
typedef enum intsetEnc {
    INTSET_ENC_INT16 = 2,  /* 16-bit signed integers */
    INTSET_ENC_INT32 = 4,  /* 32-bit signed integers */
    INTSET_ENC_INT64 = 8   /* 64-bit signed integers */
} intsetEnc;
```

### Structure Layout

```c
typedef struct intset {
    intsetEnc encoding;   /* Current encoding: 2, 4, or 8 bytes */
    uint32_t length;      /* Number of elements */
    int8_t contents[];    /* Variable-length array (flexible) */
} intset;
```

**Memory Layout Example:**

```
For intset with values [5, 10, 20] in INT16 encoding:

+----------+--------+------------------+
| encoding | length |    contents      |
+----------+--------+------------------+
|    2     |   3    | [5, 10, 20]      |
| (2 bytes)| (4B)   | (6 bytes total)  |
+----------+--------+------------------+
Total: 4 + 4 + 6 = 14 bytes

After adding 70000 (requires INT32):

+----------+--------+-------------------------+
| encoding | length |       contents          |
+----------+--------+-------------------------+
|    4     |   4    | [5, 10, 20, 70000]     |
| (4 bytes)| (4B)   | (16 bytes total)        |
+----------+--------+-------------------------+
Total: 4 + 4 + 16 = 24 bytes
```

## Variable-Width Encoding

### Automatic Promotion

The intset automatically upgrades its encoding when a value is added that doesn't fit in the current encoding:

```c
/* Value ranges that trigger encoding */
INT16: -32,768 to 32,767          (2 bytes per element)
INT32: -2,147,483,648 to 2,147,483,647   (4 bytes per element)
INT64: -9,223,372,036,854,775,808 to 9,223,372,036,854,775,807  (8 bytes)
```

**Promotion Rules:**
1. All values fit in INT16 → use INT16 encoding
2. Any value requires INT32 → upgrade all to INT32
3. Any value requires INT64 → upgrade all to INT64
4. **Never downgrade** - even if large values are removed

### How Promotion Works

```c
intset *is = intsetNew();  /* Starts as INT16 */

/* Add 100 - stays INT16 */
intsetAdd(&is, 100, NULL);
// encoding = INT16, contents = [100]

/* Add 50000 - requires INT32, ALL elements upgraded */
intsetAdd(&is, 50000, NULL);
// encoding = INT32, contents = [100, 50000]
// Note: 100 now uses 4 bytes even though it fits in 2

/* Add INT64_MAX - upgrades to INT64 */
intsetAdd(&is, 9223372036854775807LL, NULL);
// encoding = INT64, contents = [100, 50000, 9223372036854775807]
// All elements now use 8 bytes each
```

## API Reference

### Creation and Destruction

```c
/* Create new empty intset (starts with INT16 encoding) */
intset *intsetNew(void);

/* Free intset */
void intsetFree(intset *is);

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

**Note**: Removal does NOT downgrade encoding. If you added a large value then removed it, the intset stays at the larger encoding.

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
/* Get value at position (0-indexed)
 * pos: position in sorted order
 * value: output pointer for the value
 * Returns: true if position valid, false otherwise
 */
bool intsetGet(intset *is, uint32_t pos, int64_t *value);

/* Example: Iterate all elements */
intset *is = intsetNew();
intsetAdd(&is, 30, NULL);
intsetAdd(&is, 10, NULL);
intsetAdd(&is, 20, NULL);

/* Elements are sorted: [10, 20, 30] */
for (uint32_t i = 0; i < intsetCount(is); i++) {
    int64_t val;
    if (intsetGet(is, i, &val)) {
        printf("%ld ", val);  // Prints: 10 20 30
    }
}
printf("\n");

intsetFree(is);
```

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
    int64_t random = intsetRandom(is);
    printf("%ld ", random);
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

printf("Elements: %zu\n", intsetCount(is));  // 3
printf("Memory: %zu bytes\n", intsetBytes(is));  // 14 bytes (header + 6 bytes data)

intsetFree(is);
```

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
        printf("User %ld logged in (new session)\n", userId);
    } else {
        printf("User %ld already active\n", userId);
    }
}

/* User logs out */
void userLogout(int64_t userId) {
    bool existed;
    intsetRemove(&activeUsers, userId, &existed);
    if (existed) {
        printf("User %ld logged out\n", userId);
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

article *articleNew(const char *title) {
    article *a = malloc(sizeof(*a));
    a->title = strdup(title);
    a->tags = intsetNew();
    return a;
}

void articleAddTag(article *a, int64_t tagId) {
    intsetAdd(&a->tags, tagId, NULL);
}

bool articleHasTag(article *a, int64_t tagId) {
    return intsetFind(a->tags, tagId);
}

void articlePrintTags(article *a) {
    printf("Tags for '%s': ", a->title);
    for (uint32_t i = 0; i < intsetCount(a->tags); i++) {
        int64_t tag;
        intsetGet(a->tags, i, &tag);
        printf("%ld ", tag);
    }
    printf("\n");
}

void articleFree(article *a) {
    free(a->title);
    intsetFree(a->tags);
    free(a);
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

### Example 3: Encoding Upgrade Demonstration

```c
/* Show automatic encoding upgrades */
intset *is = intsetNew();

printf("Initial encoding: %d bytes per element\n", is->encoding);
// Output: Initial encoding: 2 bytes per element

/* Add small values - stays INT16 */
intsetAdd(&is, 100, NULL);
intsetAdd(&is, 200, NULL);
printf("After adding 100, 200: %d bytes per element\n", is->encoding);
// Output: After adding 100, 200: 2 bytes per element

/* Add value requiring INT32 - upgrades all elements */
intsetAdd(&is, 100000, NULL);
printf("After adding 100000: %d bytes per element\n", is->encoding);
// Output: After adding 100000: 4 bytes per element

/* Remove large value - encoding stays INT32 */
bool removed;
intsetRemove(&is, 100000, &removed);
printf("After removing 100000: %d bytes per element\n", is->encoding);
// Output: After removing 100000: 4 bytes per element (NO DOWNGRADE)

printf("Memory for 2 elements: %zu bytes\n", intsetBytes(is));
// Output: Memory for 2 elements: 16 bytes
// (8 byte header + 8 bytes for two INT32 values)

intsetFree(is);
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

| Operation | Complexity | Notes |
|-----------|-----------|-------|
| Create | O(1) | Allocates small header + minimal array |
| Add (no upgrade) | O(n) | Binary search O(log n) + memmove O(n) |
| Add (upgrade) | O(n) | Must convert all elements |
| Remove | O(n) | Binary search O(log n) + memmove O(n) |
| Find | O(log n) | Binary search on sorted array |
| Get by position | O(1) | Direct array access (after encoding calc) |
| Random | O(1) | Random index selection |
| Count | O(1) | Cached in structure |
| Bytes | O(1) | Simple calculation |

**Insertion Performance:**
- Best case: O(log n) for finding position
- Worst case: O(n) for memmove + potential upgrade

**Memory Efficiency:**
```
Overhead: 8 bytes (encoding + length)
Per element: 2, 4, or 8 bytes depending on encoding
No pointers, no allocation headers per element

Example with 1000 elements in INT16:
- intset: 8 + (1000 * 2) = 2,008 bytes
- Array of int64_t: 1000 * 8 = 8,000 bytes
- Savings: ~75% memory reduction
```

## Memory Efficiency

### Encoding Size Comparison

```c
/* Compare memory usage across encodings */
intset *small = intsetNew();  // INT16
for (int i = 0; i < 1000; i++) {
    intsetAdd(&small, i, NULL);
}
printf("1000 small ints: %zu bytes\n", intsetBytes(small));
// Output: 1000 small ints: 2008 bytes (8 + 1000*2)

intset *medium = intsetNew();  // INT32
for (int i = 0; i < 1000; i++) {
    intsetAdd(&medium, i * 100000, NULL);
}
printf("1000 medium ints: %zu bytes\n", intsetBytes(medium));
// Output: 1000 medium ints: 4008 bytes (8 + 1000*4)

intset *large = intsetNew();  // INT64
for (int i = 0; i < 1000; i++) {
    intsetAdd(&large, (int64_t)i * 10000000000LL, NULL);
}
printf("1000 large ints: %zu bytes\n", intsetBytes(large));
// Output: 1000 large ints: 8008 bytes (8 + 1000*8)
```

### When Encoding Matters

```c
/* Bad: One large value wastes memory */
intset *wasteful = intsetNew();
for (int i = 1; i <= 999; i++) {
    intsetAdd(&wasteful, i, NULL);  // All fit in INT16
}
intsetAdd(&wasteful, INT64_MAX, NULL);  // Forces INT64 for all!
printf("Memory (wasteful): %zu bytes\n", intsetBytes(wasteful));
// Output: 8008 bytes - all 1000 elements now use 8 bytes each

/* Good: Keep values in similar ranges */
intset *efficient = intsetNew();
for (int i = 1; i <= 1000; i++) {
    intsetAdd(&efficient, i, NULL);  // All fit in INT16
}
printf("Memory (efficient): %zu bytes\n", intsetBytes(efficient));
// Output: 2008 bytes - all elements use 2 bytes each
```

## Best Practices

### 1. Pass Pointer to Pointer

```c
/* WRONG - may crash after reallocation */
intset *is = intsetNew();
intsetAdd(&is, 100, NULL);
intsetAdd(&is, 200, NULL);  // 'is' may be invalid if reallocated!

/* RIGHT - always pass address */
intset *is = intsetNew();
intsetAdd(&is, 100, NULL);  // Updates 'is' if reallocated
intsetAdd(&is, 200, NULL);  // Safe
```

### 2. Consider Value Ranges

```c
/* GOOD - Values in similar range */
intset *ids = intsetNew();
for (int i = 1; i <= 10000; i++) {
    intsetAdd(&ids, i, NULL);
}
// Uses INT16 encoding: 2 bytes per element

/* BAD - Wide value range */
intset *mixed = intsetNew();
intsetAdd(&mixed, 1, NULL);
intsetAdd(&mixed, 5000000000LL, NULL);  // Forces INT64 for everything!
// Now element '1' wastes 8 bytes instead of 2
```

### 3. Batch Operations When Possible

```c
/* Less efficient - many small adds with potential upgrades */
intset *is = intsetNew();
for (int i = 0; i < 10000; i++) {
    int64_t random = rand();
    intsetAdd(&is, random, NULL);  // May trigger encoding upgrade each time
}

/* More efficient - pre-allocate if you know max value */
// (Note: intset doesn't have pre-allocation, but you can avoid upgrades
//  by adding largest values first)
intset *is2 = intsetNew();
intsetAdd(&is2, INT32_MAX, NULL);  // Force INT32 encoding upfront
for (int i = 0; i < 10000; i++) {
    int64_t random = rand() % INT32_MAX;
    intsetAdd(&is2, random, NULL);  // No upgrades needed
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

### 1. Forgetting No Downgrade

```c
/* WRONG ASSUMPTION */
intset *is = intsetNew();
intsetAdd(&is, INT64_MAX, NULL);  // Forces INT64
intsetRemove(&is, INT64_MAX, NULL);  // Removed, but...
// Encoding is STILL INT64! Memory doesn't shrink.

/* If you need downgrading, recreate the set */
intset *old = is;
intset *new = intsetNew();
for (uint32_t i = 0; i < intsetCount(old); i++) {
    int64_t val;
    intsetGet(old, i, &val);
    if (val != INT64_MAX) {  // Skip the large value
        intsetAdd(&new, val, NULL);
    }
}
intsetFree(old);
is = new;  // New intset may use smaller encoding
```

### 2. Not Checking Return Values

```c
/* BAD - ignoring return */
int64_t val;
intsetGet(is, 1000, &val);  // What if position is invalid?
printf("%ld\n", val);  // Undefined behavior!

/* GOOD - check return */
int64_t val;
if (intsetGet(is, 1000, &val)) {
    printf("%ld\n", val);
} else {
    printf("Invalid position\n");
}
```

### 3. Inefficient Set Operations

```c
/* SLOW - O(n²) membership test */
intset *a = ..., *b = ...;
for (uint32_t i = 0; i < intsetCount(a); i++) {
    int64_t val;
    intsetGet(a, i, &val);
    for (uint32_t j = 0; j < intsetCount(b); j++) {
        int64_t val2;
        intsetGet(b, j, &val2);
        if (val == val2) { /* found */ }
    }
}

/* FASTER - O(n log m) using binary search */
for (uint32_t i = 0; i < intsetCount(a); i++) {
    int64_t val;
    intsetGet(a, i, &val);
    if (intsetFind(b, val)) { /* found */ }
}
```

## See Also

- [intsetU32](INTSET_U32.md) - Fixed 32-bit integer set (faster, no encoding changes)
- [intsetBig](INTSET_BIG.md) - 128-bit integer set with bucketing
- [hyperloglog](HYPERLOGLOG.md) - Probabilistic cardinality estimation
- [flex](../flex/FLEX.md) - Flexible arrays that can use intset internally

## Testing

Run the intset test suite:

```bash
./src/datakit-test test intset
```

The test suite validates:
- Value encoding detection
- Encoding upgrades (16→32→64)
- Binary search correctness
- Insertion and deletion
- Sorted order maintenance
- Stress tests with random operations
