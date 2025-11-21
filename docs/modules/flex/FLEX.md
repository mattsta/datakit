# flex - Compact Variable-Length Encoded Array

## Overview

`flex` is a **memory-efficient, pointer-free, variable-length encoded array** that serves as a universal sequential container for storing mixed-type data with minimal overhead. It automatically compresses data using optimal encodings and supports bidirectional traversal, sorted operations, and in-place modifications.

**Key Features:**

- **Pointer-free design**: Single contiguous memory allocation with no internal pointers
- **Variable-length encoding**: Automatic selection of minimal storage size for each element
- **Multi-type storage**: Seamlessly stores integers, floats, strings, booleans, and nested containers
- **Compact representation**: 2-byte overhead for empty flex, minimal per-entry overhead
- **Bidirectional traversal**: Efficient forward and backward iteration
- **Sorted operations**: Binary search with optional middle-entry hint for O(log n) performance
- **Zero-copy when possible**: Direct data access without intermediate allocations
- **Compression support**: Optional compression (cflex) for large flexes

**Header**: `flex.h`

**Source**: `flex.c`

## Data Structure

### Physical Layout

A flex consists of a header followed by zero or more entries:

```
┌──────────┬──────────┬─────────┬─────────┬─────────┐
│  BYTES   │  COUNT   │ ENTRY 1 │ ENTRY 2 │   ...   │
├──────────┼──────────┼─────────┼─────────┼─────────┤
│ 1-9 bytes│ 1-9 bytes│Variable │Variable │Variable │
│ (varint) │ (varint) │ length  │ length  │ length  │
└──────────┴──────────┴─────────┴─────────┴─────────┘
```

- **BYTES**: Split varint encoding of total flex size (1-9 bytes)
- **COUNT**: Tagged varint encoding of element count (1-9 bytes)
- **ENTRY**: Variable-length encoded elements

### Entry Layout

Each entry has a symmetric forward/reverse encoding structure for bidirectional traversal:

```
Regular Entry:
┌──────────┬──────────┬──────────┐
│ ENCODING │   DATA   │ ENCODING │
│ (forward)│          │ (reverse)│
└──────────┴──────────┴──────────┘

Immediate Value Entry (true/false/null):
┌──────────┐
│ ENCODING │
│ (1 byte) │
└──────────┘
```

- **Forward encoding**: Type and length information (1-9 bytes)
- **Data**: The actual element data (0-N bytes)
- **Reverse encoding**: Same as forward, but reversed for backward traversal

### Type System

```c
typedef enum flexType {
    /* String types (0-201): length-prefixed byte arrays */
    /* Encoding bytes 0-201 indicate string with that length */

    /* Integer types (202-221): variable-width signed/unsigned */
    FLEX_NEG_8B = 202,    /* Negative 8-bit integer */
    FLEX_UINT_8B,         /* Unsigned 8-bit integer */
    FLEX_NEG_16B,         /* Negative 16-bit integer */
    FLEX_UINT_16B,        /* Unsigned 16-bit integer */
    FLEX_NEG_24B,         /* 24-bit (saves space over 32-bit) */
    FLEX_UINT_24B,
    FLEX_NEG_32B,
    FLEX_UINT_32B,
    /* ... up to 64-bit, 96-bit, 128-bit */

    /* Float types (222-227) */
    FLEX_REAL_B16B,       /* bfloat16 (Google format) */
    FLEX_REAL_16B,        /* IEEE 754 binary16 */
    FLEX_REAL_32B,        /* IEEE 754 binary32 */
    FLEX_REAL_64B,        /* IEEE 754 binary64 */

    /* External reference types (228-243) */
    FLEX_EXTERNAL_MDSC_48B,  /* 48-bit pointer to mdsc */
    FLEX_EXTERNAL_MDSC_64B,  /* 64-bit pointer to mdsc */
    FLEX_CONTAINER_REFERENCE_EXTERNAL_8,  /* 8-bit reference ID */
    /* ... up to 64-bit reference IDs */

    /* Container types (244-251) */
    FLEX_CONTAINER_MAP,
    FLEX_CONTAINER_LIST,
    FLEX_CONTAINER_SET,
    FLEX_CONTAINER_TUPLE,
    /* Compressed variants */
    FLEX_CONTAINER_CMAP,
    FLEX_CONTAINER_CLIST,
    FLEX_CONTAINER_CSET,
    FLEX_CONTAINER_CTUPLE,

    /* Immediate types (252-255) */
    FLEX_BYTES_EMPTY = 252,  /* Zero-length byte array */
    FLEX_TRUE = 253,         /* Boolean true */
    FLEX_FALSE = 254,        /* Boolean false */
    FLEX_NULL = 255          /* NULL value */
} flexType;
```

### Encoding Optimization

Flex automatically selects the most compact encoding:

| Data Type                 | Storage              | Example                 |
| ------------------------- | -------------------- | ----------------------- |
| Small strings (≤64 bytes) | 1 + N + 1 bytes      | "hello" → 6 bytes total |
| Medium strings (≤16KB)    | 2 + N + 2 bytes      |                         |
| Large strings (>16KB)     | 3-9 + N + 3-9 bytes  |                         |
| Integers -128 to 127      | 1 + 1 + 1 = 3 bytes  | 42 → 3 bytes            |
| Integers -32768 to 32767  | 1 + 2 + 1 = 4 bytes  |                         |
| Float16                   | 1 + 2 + 1 = 4 bytes  | 0.578125 → 4 bytes      |
| Float32                   | 1 + 4 + 1 = 6 bytes  |                         |
| Float64                   | 1 + 8 + 1 = 10 bytes |                         |
| true/false/null           | 1 byte               |                         |

## Basic Types

```c
typedef uint8_t flex;         /* Flex itself (pointer to first byte) */
typedef uint8_t flexEntry;    /* Entry pointer (within flex) */
typedef uint8_t cflex;        /* Compressed flex */

/* Endpoints for push operations */
typedef enum flexEndpoint {
    FLEX_ENDPOINT_TAIL = -1,  /* Insert at end */
    FLEX_ENDPOINT_HEAD = 0    /* Insert at beginning */
} flexEndpoint;

/* Empty flex is just 2 bytes! */
#define FLEX_EMPTY_SIZE 2
```

## Creation and Destruction

### Basic Lifecycle

```c
/* Create a new empty flex */
flex *flexNew(void);

/* Duplicate an existing flex */
flex *flexDuplicate(const flex *f);

/* Reset flex to empty state (preserves allocation) */
void flexReset(flex **ff);

/* Free a flex */
void flexFree(flex *f);

/* Example usage */
flex *f = flexNew();
// ... use flex ...
flexFree(f);
```

### Working Example

```c
/* Create and populate a flex with mixed types */
flex *f = flexNew();

/* Push various types */
flexPushSigned(&f, -42, FLEX_ENDPOINT_TAIL);
flexPushBytes(&f, "hello", 5, FLEX_ENDPOINT_TAIL);
flexPushDouble(&f, 3.14159, FLEX_ENDPOINT_TAIL);

const databox t = databoxBool(true);
flexPushByType(&f, &t, FLEX_ENDPOINT_TAIL);

/* Flex now contains: [-42, "hello", 3.14159, true] */

flexFree(f);
```

## Push Operations (Insert at Endpoints)

### Type-Specific Push

```c
/* Push bytes (string or binary data) */
void flexPushBytes(flex **ff, const void *s, size_t len, flexEndpoint where);

/* Push signed integer */
void flexPushSigned(flex **ff, int64_t i, flexEndpoint where);

/* Push unsigned integer */
void flexPushUnsigned(flex **ff, uint64_t u, flexEndpoint where);

/* Push float (auto-selects float16, bfloat16, or float32) */
void flexPushFloat(flex **ff, float f, flexEndpoint where);

/* Push float specifically as IEEE float16 */
void flexPushFloat16(flex **ff, float f, flexEndpoint where);

/* Push float specifically as bfloat16 */
void flexPushFloatB16(flex **ff, float f, flexEndpoint where);

/* Push double */
void flexPushDouble(flex **ff, double d, flexEndpoint where);

/* Push any databox type (polymorphic) */
void flexPushByType(flex **ff, const databox *box, flexEndpoint where);
```

### Push Examples

```c
flex *f = flexNew();

/* Push to tail (append) */
flexPushBytes(&f, "apple", 5, FLEX_ENDPOINT_TAIL);
flexPushSigned(&f, 100, FLEX_ENDPOINT_TAIL);
/* f: ["apple", 100] */

/* Push to head (prepend) */
flexPushBytes(&f, "first", 5, FLEX_ENDPOINT_HEAD);
/* f: ["first", "apple", 100] */

/* Push using databox */
databox box = databoxNewReal(2.718);
flexPushByType(&f, &box, FLEX_ENDPOINT_TAIL);
/* f: ["first", "apple", 100, 2.718] */

flexFree(f);
```

### Bulk Append

```c
/* Append multiple databox elements at once */
void flexAppendMultiple(flex **ff, const uint_fast32_t elementsPerEntry,
                        const databox **const box);

/* Example: Create a record with multiple fields */
flex *f = flexNew();

databox id = databoxNewUnsigned(12345);
databox name = databoxNewBytesString("Alice");
databox score = databoxNewReal(98.5);
databox active = databoxBool(true);

const databox *fields[] = {&id, &name, &score, &active};
flexAppendMultiple(&f, 4, fields);

/* f now contains all 4 fields in order */
flexFree(f);
```

## Insert Operations (Insert at Position)

### Insert at Specific Entry

```c
/* Insert bytes at position fe */
void flexInsertBytes(flex **ff, flexEntry *fe, const void *s, const size_t len);

/* Insert signed integer */
void flexInsertSigned(flex **ff, flexEntry *fe, int64_t i);

/* Insert unsigned integer */
void flexInsertUnsigned(flex **ff, flexEntry *fe, uint64_t u);

/* Insert float (auto-encoded) */
void flexInsertFloat(flex **ff, flexEntry *fe, float f);

/* Insert half-precision float */
void flexInsertHalfFloat(flex **ff, flexEntry *fe, float f);

/* Insert double */
void flexInsertDouble(flex **ff, flexEntry *fe, double d);

/* Insert any databox type */
void flexInsertByType(flex **ff, flexEntry *fe, const databox *box);
```

### Insert Examples

```c
flex *f = flexNew();
flexPushBytes(&f, "first", 5, FLEX_ENDPOINT_TAIL);
flexPushBytes(&f, "third", 5, FLEX_ENDPOINT_TAIL);
/* f: ["first", "third"] */

/* Insert in the middle */
flexEntry *fe = flexIndex(f, 1);  /* Get position of "third" */
flexInsertBytes(&f, fe, "second", 6);
/* f: ["first", "second", "third"] */

flexFree(f);
```

### Sorted Insert (Binary Search)

```c
/* Insert into sorted flex (single element) */
bool flexInsertByTypeSorted(flex **ff, const databox *box);

/* Insert with middle hint for faster search */
bool flexInsertByTypeSortedWithMiddle(flex **ff, const databox *box,
                                      flexEntry **middleEntry);

/* Insert multiple elements (e.g., key-value pair) */
bool flexInsertByTypeSortedWithMiddleMultiDirect(
    flex **ff, uint_fast32_t elementsPerEntry, const databox *box[],
    flexEntry **middleEntry);

/* Insert or replace if exists */
bool flexInsertReplaceByTypeSortedWithMiddleMultiDirect(
    flex **ff, uint_fast32_t elementsPerEntry, const databox *box[],
    flexEntry **middleEntry, bool replace);
```

### Sorted Insert Examples

```c
/* Example 1: Sorted list of integers */
flex *f = flexNew();
flexEntry *middle = NULL;

for (int i = 0; i < 100; i++) {
    int value = rand() % 1000;
    databox box = databoxNewSigned(value);
    flexInsertByTypeSortedWithMiddle(&f, &box, &middle);
}
/* f is now sorted, and 'middle' tracks the middle element */

/* Example 2: Sorted key-value map */
flex *fmap = flexNew();
flexEntry *mid = NULL;

const databox key1 = databoxNewBytesString("apple");
const databox val1 = databoxNewSigned(100);
const databox *pair1[2] = {&key1, &val1};
flexInsertByTypeSortedWithMiddleMultiDirect(&fmap, 2, pair1, &mid);

const databox key2 = databoxNewBytesString("banana");
const databox val2 = databoxNewSigned(200);
const databox *pair2[2] = {&key2, &val2};
flexInsertByTypeSortedWithMiddleMultiDirect(&fmap, 2, pair2, &mid);

/* fmap: [("apple", 100), ("banana", 200)] - sorted by key */

flexFree(fmap);
flexFree(f);
```

## Accessing Elements

### Indexing

```c
/* Get entry at index (supports negative indexing) */
flexEntry *flexIndex(const flex *f, int32_t index);

/* Get entry at index (direct, faster) */
flexEntry *flexIndexDirect(const flex *f, int32_t index);

/* Get head (first) entry */
flexEntry *flexHead(const flex *f);

/* Get tail (last) entry */
flexEntry *flexTail(const flex *f);

/* Get tail of multi-element entries (e.g., last key-value pair) */
flexEntry *flexTailWithElements(const flex *const f,
                                uint_fast32_t elementsPerEntry);

/* Check if entry pointer is valid */
bool flexEntryIsValid(const flex *f, flexEntry *fe);
```

### Indexing Examples

```c
flex *f = flexNew();
for (int i = 0; i < 10; i++) {
    flexPushSigned(&f, i, FLEX_ENDPOINT_TAIL);
}
/* f: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9] */

/* Positive indexing */
flexEntry *fe0 = flexIndex(f, 0);   /* Points to 0 */
flexEntry *fe3 = flexIndex(f, 3);   /* Points to 3 */

/* Negative indexing (from end) */
flexEntry *feLast = flexIndex(f, -1);   /* Points to 9 */
flexEntry *fe2nd = flexIndex(f, -2);    /* Points to 8 */

/* Out of range returns NULL */
flexEntry *feInvalid = flexIndex(f, 100);  /* NULL */

/* Quick endpoint access */
flexEntry *head = flexHead(f);  /* Same as flexIndex(f, 0) */
flexEntry *tail = flexTail(f);  /* Same as flexIndex(f, -1) */

flexFree(f);
```

### Navigation

```c
/* Get next entry */
flexEntry *flexNext(const flex *f, flexEntry *fe);

/* Get previous entry */
flexEntry *flexPrev(const flex *f, flexEntry *fe);
```

### Navigation Examples

```c
flex *f = createList();  /* ["hello", "foo", "quux", 1024] */

/* Forward iteration */
flexEntry *fe = flexHead(f);
while (fe && flexEntryIsValid(f, fe)) {
    databox box = {{0}};
    flexGetByType(fe, &box);
    databoxRepr(&box);
    fe = flexNext(f, fe);
}

/* Backward iteration */
fe = flexTail(f);
while (fe && flexEntryIsValid(f, fe)) {
    databox box = {{0}};
    flexGetByType(fe, &box);
    databoxRepr(&box);
    fe = flexPrev(f, fe);
}

flexFree(f);
```

## Retrieving Data

### Get Functions

```c
/* Get data from entry into databox */
void flexGetByType(const flexEntry *fe, databox *outbox);

/* Get data with external reference resolution */
void flexGetByTypeWithReference(const flexEntry *const fe, databox *box,
                                const struct multimapAtom *referenceContainer);

/* Get data as a copy (allocated) */
void flexGetByTypeCopy(const flexEntry *const fe, databox *box);

/* Get and advance to next entry */
bool flexGetNextByType(flex *f, flexEntry **fe, databox *box);

/* Get signed integer value */
bool flexGetSigned(flexEntry *fe, int64_t *value);

/* Get unsigned integer value */
bool flexGetUnsigned(flexEntry *fe, uint64_t *value);
```

### Retrieval Examples

```c
flex *f = flexNew();
flexPushSigned(&f, 42, FLEX_ENDPOINT_TAIL);
flexPushBytes(&f, "world", 5, FLEX_ENDPOINT_TAIL);
flexPushDouble(&f, 3.14, FLEX_ENDPOINT_TAIL);

/* Get first element */
flexEntry *fe = flexHead(f);
databox box = {{0}};
flexGetByType(fe, &box);
assert(DATABOX_IS_SIGNED_INTEGER(&box));
assert(box.data.i64 == 42);

/* Iterate and get all elements */
fe = flexHead(f);
while (fe && flexGetNextByType(f, &fe, &box)) {
    if (DATABOX_IS_INTEGER(&box)) {
        printf("Integer: %ld\n", box.data.i64);
    } else if (DATABOX_IS_BYTES(&box)) {
        printf("String: %.*s\n", (int)box.len,
               databoxBytes(&box));
    } else if (DATABOX_IS_FLOAT(&box)) {
        printf("Float: %f\n", box.data.d64);
    }
}

flexFree(f);
```

## Finding Elements

### Linear Search

```c
/* Find bytes in flex (from head) */
flexEntry *flexFind(flex *f, const void *vstr, uint32_t vlen, uint32_t skip);

/* Find signed integer */
flexEntry *flexFindSigned(const flex *f, flexEntry *fe, int64_t sval,
                          uint32_t skip);

/* Find unsigned integer */
flexEntry *flexFindUnsigned(const flex *f, flexEntry *fe, uint64_t sval,
                            uint32_t skip);

/* Find string */
flexEntry *flexFindString(const flex *f, flexEntry *fe, const void *sval,
                          const size_t slen, uint32_t skip);

/* Find by databox type */
flexEntry *flexFindByType(flex *f, flexEntry *fe, const databox *box,
                          uint32_t skip);

/* Find from head specifically */
flexEntry *flexFindByTypeHead(flex *f, const databox *box, uint32_t skip);
```

### Reverse Search

```c
/* Find signed integer (tail to head) */
flexEntry *flexFindSignedReverse(const flex *f, flexEntry *fe, int64_t sval,
                                 uint32_t skip);

/* Find unsigned integer (tail to head) */
flexEntry *flexFindUnsignedReverse(const flex *f, flexEntry *fe, uint64_t sval,
                                   uint32_t skip);

/* Find string (tail to head) */
flexEntry *flexFindStringReverse(const flex *f, flexEntry *fe, const void *sval,
                                 const size_t slen, uint32_t skip);

/* Find by type (tail to head) */
flexEntry *flexFindByTypeReverse(flex *f, flexEntry *fe, const databox *box,
                                 uint32_t skip);
```

### Binary Search (Sorted Flexes)

```c
/* Binary search in sorted flex */
flexEntry *flexFindByTypeSorted(const flex *f, uint_fast32_t nextElementOffset,
                                const databox *compareAgainst);

/* Binary search with middle hint (faster) */
flexEntry *flexFindByTypeSortedWithMiddle(const flex *f,
                                          uint_fast32_t elementsPerEntry,
                                          const databox *compareAgainst,
                                          const flexEntry *middleP);

/* Binary search for full multi-element entries */
flexEntry *flexFindByTypeSortedWithMiddleFullWidth(
    const flex *f, uint_fast32_t elementsPerEntry,
    const databox **compareAgainst, const flexEntry *middleP);

/* Binary search with external references */
flexEntry *flexFindByTypeSortedWithMiddleWithReference(
    const flex *const f, const uint_fast32_t elementsPerEntry,
    const databox *compareAgainst, const flexEntry *middleFE,
    const struct multimapAtom *referenceContainer);
```

### Search Examples

```c
/* Linear search */
flex *f = flexNew();
flexPushBytes(&f, "apple", 5, FLEX_ENDPOINT_TAIL);
flexPushBytes(&f, "banana", 6, FLEX_ENDPOINT_TAIL);
flexPushBytes(&f, "cherry", 6, FLEX_ENDPOINT_TAIL);

databox search = databoxNewBytesString("banana");
flexEntry *found = flexFindByType(f, flexHead(f), &search, 0);
if (found) {
    printf("Found banana!\n");
}

/* Binary search in sorted flex */
flex *sorted = flexNew();
flexEntry *middle = NULL;

for (int i = 0; i < 100; i++) {
    databox box = databoxNewSigned(i * 10);
    flexInsertByTypeSortedWithMiddle(&sorted, &box, &middle);
}

/* Now search for a value */
databox searchFor = databoxNewSigned(420);
flexEntry *result = flexFindByTypeSortedWithMiddle(sorted, 1, &searchFor, middle);
if (result) {
    printf("Found 420 in sorted flex!\n");
}

flexFree(sorted);
flexFree(f);
```

## Comparing Elements

```c
/* Compare entry with bytes */
bool flexCompareBytes(flex *fe, const void *s, size_t slen);

/* Compare entry with string */
bool flexCompareString(flexEntry *fe, const void *sstr, size_t slen);

/* Compare entry with unsigned integer */
bool flexCompareUnsigned(flexEntry *fe, uint64_t sval);

/* Compare entry with signed integer */
bool flexCompareSigned(flexEntry *fe, int64_t sval);

/* Compare entire multi-element entries */
int flexCompareEntries(const flex *f, const databox *const *elements,
                       uint_fast32_t elementsPerEntry, int_fast32_t offset);

/* Compare two flexes for equality */
bool flexEqual(const flex *a, const flex *b);
```

### Comparison Examples

```c
flex *f = flexNew();
flexPushBytes(&f, "hello", 5, FLEX_ENDPOINT_TAIL);
flexPushSigned(&f, 1024, FLEX_ENDPOINT_TAIL);

flexEntry *fe = flexHead(f);
if (flexCompareBytes(fe, "hello", 5)) {
    printf("First element is 'hello'\n");
}

fe = flexNext(f, fe);
if (flexCompareSigned(fe, 1024)) {
    printf("Second element is 1024\n");
}

flexFree(f);
```

## Replacing Elements

```c
/* Replace entry with databox */
void flexReplaceByType(flex **ff, flexEntry *fe, const databox *box);

/* Replace entry with bytes */
void flexReplaceBytes(flex **ff, flexEntry *fe, const void *s,
                      const size_t slen);

/* Replace with signed integer */
bool flexReplaceSigned(flex **ff, flexEntry *fe, int64_t value);

/* Replace with unsigned integer */
bool flexReplaceUnsigned(flex **ff, flexEntry *fe, uint64_t value);

/* Increment signed integer in place */
bool flexIncrbySigned(flex **ff, flexEntry *fe, int64_t incrby,
                      int64_t *newval);

/* Increment unsigned integer in place */
bool flexIncrbyUnsigned(flex **ff, flexEntry *fe, int64_t incrby,
                        uint64_t *newval);

/* Resize entry to new length (advanced) */
void flexResizeEntry(flex **ff, flexEntry *fe, size_t newLenForEntry);
```

### Replace Examples

```c
flex *f = flexNew();
flexPushBytes(&f, "old", 3, FLEX_ENDPOINT_TAIL);
flexPushSigned(&f, 10, FLEX_ENDPOINT_TAIL);

/* Replace string */
flexEntry *fe = flexHead(f);
flexReplaceBytes(&f, fe, "new", 3);
/* f: ["new", 10] */

/* Replace and increment integer */
fe = flexTail(f);
int64_t newValue;
flexIncrbySigned(&f, fe, 5, &newValue);
assert(newValue == 15);
/* f: ["new", 15] */

flexFree(f);
```

## Deleting Elements

### Single Delete

```c
/* Delete entry (updates fe to next valid entry) */
void flexDelete(flex **ff, flexEntry **fe);

/* Delete entry without updating pointer */
void flexDeleteNoUpdateEntry(flex **ff, flexEntry *fe);

/* Delete entry for draining (no realloc) */
void flexDeleteDrain(flex **ff, flexEntry **fe);

/* Quick macros */
#define flexDeleteHead(ff) flexDeleteNoUpdateEntry(ff, flexHead(*(ff)))
#define flexDeleteTail(ff) flexDeleteNoUpdateEntry(ff, flexTail(*(ff)))
```

### Range Delete

```c
/* Delete count entries starting at fe */
void flexDeleteCount(flex **ff, flexEntry **fe, uint32_t count);

/* Delete count entries at offset */
void flexDeleteOffsetCount(flex **ff, int32_t offset, uint32_t count);

/* Delete num entries starting at index */
void flexDeleteRange(flex **ff, int32_t index, uint32_t num);

/* Delete from start up to and including fe */
void flexDeleteUpToInclusive(flex **ff, flexEntry *fe);

/* Delete up to fe plus N more entries */
void flexDeleteUpToInclusivePlusN(flex **ff, flexEntry *fe,
                                  const int32_t nMore);

/* Delete from sorted flex with middle tracking */
void flexDeleteSortedValueWithMiddle(flex **ff, uint_fast32_t elementsPerEntry,
                                     flexEntry *fe, flexEntry **middleEntry);
```

### Drain Variants (No Realloc)

For bulk deletions, use drain variants to avoid reallocating on each delete:

```c
void flexDeleteCountDrain(flex **ff, flexEntry **fe, uint32_t count);
void flexDeleteOffsetCountDrain(flex **ff, int32_t offset, uint32_t count);
void flexDeleteRangeDrain(flex **ff, int32_t index, uint32_t num);
```

### Delete Examples

```c
/* Example 1: Delete while iterating */
flex *f = flexNew();
flexPushBytes(&f, "hello", 5, FLEX_ENDPOINT_TAIL);
flexPushBytes(&f, "foo", 3, FLEX_ENDPOINT_TAIL);
flexPushBytes(&f, "bar", 3, FLEX_ENDPOINT_TAIL);

flexEntry *fe = flexHead(f);
while (flexEntryIsValid(f, fe)) {
    databox box = {{0}};
    flexGetByType(fe, &box);

    if (DATABOX_IS_BYTES(&box) &&
        memcmp(databoxBytes(&box), "foo", 3) == 0) {
        flexDelete(&f, &fe);  /* Deletes and advances fe */
    } else {
        fe = flexNext(f, fe);
    }
}
/* f: ["hello", "bar"] */

/* Example 2: Delete range */
flexDeleteRange(&f, 0, 2);  /* Delete first 2 elements */
/* f: [] (empty) */

flexFree(f);

/* Example 3: Efficient bulk delete with drain */
f = flexNew();
for (int i = 0; i < 1000; i++) {
    flexPushSigned(&f, i, FLEX_ENDPOINT_TAIL);
}

/* Delete all from tail without reallocating each time */
while (flexCount(f) > 0) {
    flexDeleteOffsetCountDrain(&f, -1, 1);
}
/* Now shrink to final size */
flexReset(&f);

flexFree(f);
```

## Merging and Splitting

### Merging

```c
/* Merge two flexes (both inputs consumed) */
flex *flexMerge(flex **first, flex **second);

/* Append one flex to another */
void flexBulkAppendFlex(flex **ff, const flex *zzb);

/* Merge array of flexes */
flex *flexBulkMergeFlex(const flex *const *const fs, const size_t count);
```

### Splitting

```c
/* Split flex at index, return second half */
flex *flexSplitRange(flex **ff, int32_t index, uint32_t num);

/* Split sorted flex at middle */
flex *flexSplitMiddle(flex **ff, uint_fast32_t elementsPerEntry,
                      const flexEntry *middleEntry);

/* Split sorted flex in half */
flex *flexSplit(flex **ff, uint_fast32_t elementsPerEntry);
```

### Merge/Split Examples

```c
/* Merging */
flex *f1 = flexNew();
flexPushBytes(&f1, "hello", 5, FLEX_ENDPOINT_TAIL);
flexPushSigned(&f1, 100, FLEX_ENDPOINT_TAIL);

flex *f2 = flexNew();
flexPushBytes(&f2, "world", 5, FLEX_ENDPOINT_TAIL);
flexPushSigned(&f2, 200, FLEX_ENDPOINT_TAIL);

/* Merge f1 and f2 (both are consumed, result in merged) */
flex *merged = flexMerge(&f1, &f2);
/* merged: ["hello", 100, "world", 200] */
/* f1 and f2 are now invalid (freed) */

/* Splitting */
flex *secondHalf = flexSplitRange(&merged, 2, 2);
/* merged: ["hello", 100] */
/* secondHalf: ["world", 200] */

flexFree(merged);
flexFree(secondHalf);
```

## Metadata and Introspection

```c
/* Check if flex is empty */
bool flexIsEmpty(const flex *f);

/* Get count of elements */
size_t flexCount(const flex *f);

/* Get total bytes used by flex */
size_t flexBytes(const flex *f);
size_t flexBytesLength(const flex *f);  /* Alias */

/* Get middle entry (for sorted operations) */
flexEntry *flexMiddle(const flex *f, uint_fast32_t elementsPerEntry);
```

### Metadata Examples

```c
flex *f = flexNew();

assert(flexIsEmpty(f));
assert(flexCount(f) == 0);
assert(flexBytes(f) == FLEX_EMPTY_SIZE);  /* 2 bytes */

flexPushSigned(&f, 42, FLEX_ENDPOINT_TAIL);
flexPushBytes(&f, "hello", 5, FLEX_ENDPOINT_TAIL);

assert(!flexIsEmpty(f));
assert(flexCount(f) == 2);
printf("Flex uses %zu bytes\n", flexBytes(f));

flexFree(f);
```

## Arithmetic Operations

For flexes containing all integers or all floats, perform aggregate operations:

```c
/* Signed integer operations */
int64_t flexAddSigned(const flex *f);
int64_t flexSubtractSigned(const flex *f);
int64_t flexMultiplySigned(const flex *f);

/* Unsigned integer operations */
uint64_t flexAddUnsigned(const flex *f);
uint64_t flexSubtractUnsigned(const flex *f);
uint64_t flexMultiplyUnsigned(const flex *f);

/* Float operations */
double flexAddFloat(const flex *f);
double flexSubtractFloat(const flex *f);
double flexMultiplyFloat(const flex *f);

/* Double operations */
double flexAddDouble(const flex *f);
double flexSubtractDouble(const flex *f);
double flexMultiplyDouble(const flex *f);
```

### Arithmetic Examples

```c
flex *numbers = flexNew();
for (int i = 1; i <= 10; i++) {
    flexPushSigned(&numbers, i, FLEX_ENDPOINT_TAIL);
}

/* Sum: 1 + 2 + 3 + ... + 10 = 55 */
int64_t sum = flexAddSigned(numbers);
assert(sum == 55);

/* Product: 1 * 2 * 3 * ... * 10 = 3628800 */
int64_t product = flexMultiplySigned(numbers);
assert(product == 3628800);

flexFree(numbers);
```

## Compressed Flex (CFlex)

For large flexes, compress to save memory:

```c
/* Minimum size to attempt compression */
#define CFLEX_MINIMUM_COMPRESS_BYTES /* Implementation defined */

/* Get compressed size */
size_t cflexBytesCompressed(const cflex *c);

/* Get uncompressed size */
size_t cflexBytes(const cflex *c);

/* Duplicate compressed flex */
cflex *cflexDuplicate(const cflex *c);

/* Convert flex to compressed form */
bool flexConvertToCFlex(const flex *f, cflex *cBuffer, size_t cBufferLen);

/* Convert compressed flex back to regular flex */
bool cflexConvertToFlex(const cflex *c, flex **fBuffer, size_t *fBufferLen);
```

### Compression Examples

```c
/* Create large flex */
flex *f = flexNew();
for (int i = 0; i < 100000; i++) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "entry-%d", i);
    flexPushBytes(&f, buf, len, FLEX_ENDPOINT_TAIL);
}

size_t originalSize = flexBytes(f);
printf("Original size: %zu bytes\n", originalSize);

/* Compress if large enough */
if (originalSize >= CFLEX_MINIMUM_COMPRESS_BYTES) {
    void *compressBuffer = malloc(originalSize * 2);
    cflex *c = (cflex *)compressBuffer;

    if (flexConvertToCFlex(f, c, originalSize * 2)) {
        size_t compressedSize = cflexBytesCompressed(c);
        printf("Compressed to: %zu bytes (%.1f%% reduction)\n",
               compressedSize,
               100.0 * (1.0 - (double)compressedSize / originalSize));

        /* Decompress when needed */
        flex *restored = NULL;
        size_t restoredSize = originalSize;
        if (cflexConvertToFlex(c, &restored, &restoredSize)) {
            assert(flexEqual(f, restored));
            flexFree(restored);
        }
    }

    free(compressBuffer);
}

flexFree(f);
```

## Debugging and Visualization

```c
#ifdef DATAKIT_TEST
/* Print human-readable representation of flex */
void flexRepr(const flex *f);

/* Run flex test suite */
int32_t flexTest(int32_t argc, char *argv[]);
#endif
```

### Debug Output Example

```c
flex *f = flexNew();
flexPushBytes(&f, "hello", 5, FLEX_ENDPOINT_TAIL);
flexPushSigned(&f, 42, FLEX_ENDPOINT_TAIL);
flexPushDouble(&f, 3.14, FLEX_ENDPOINT_TAIL);

#ifdef DATAKIT_TEST
flexRepr(f);
/* Output (example):
 * [3 entries, 28 bytes]
 * [0] "hello" (5 bytes, string)
 * [1] 42 (signed integer, FLEX_NEG_8B)
 * [2] 3.14 (double, FLEX_REAL_64B)
 */
#endif

flexFree(f);
```

## Performance Characteristics

| Operation                 | Complexity     | Notes                        |
| ------------------------- | -------------- | ---------------------------- |
| **Creation**              | O(1)           | Allocates 2 bytes            |
| **Push to tail**          | O(1) amortized | Single realloc               |
| **Push to head**          | O(n)           | Must shift all data          |
| **Index access**          | O(n)           | Sequential scan required     |
| **Head/tail access**      | O(1)           | Direct calculation           |
| **Next/prev**             | O(1)           | Length encoded in entry      |
| **Insert at position**    | O(n)           | Must shift data after insert |
| **Delete at position**    | O(n)           | Must shift data after delete |
| **Find (linear)**         | O(n)           | Full scan                    |
| **Find (sorted)**         | O(log n)       | Binary search                |
| **Find with middle hint** | O(log n/2)     | Binary search from middle    |
| **Merge**                 | O(n + m)       | Concatenation                |
| **Split**                 | O(1)           | Just allocate new header     |

### Performance Tips

1. **Use middle hints**: For sorted flexes, always maintain a middle entry pointer
2. **Batch operations**: Use drain variants when deleting many elements
3. **Preallocate**: Use `zrealloc()` to grow flex before bulk inserts
4. **Prefer tail operations**: Pushing to tail is O(1), head is O(n)
5. **Keep sorted**: Binary search is 100-1000x faster than linear search

## Memory Management

### Memory Layout

```
Empty flex (2 bytes):
[0x02][0x00]
  ^     ^
  |     └─ count = 0 (1 byte tagged varint)
  └─────── total bytes = 2 (1 byte split varint)

Small flex with one integer (5 bytes):
[0x05][0x01][0xCA][0x2A][0xCA]
  ^     ^     ^     ^     ^
  |     |     |     |     └─ reverse encoding
  |     |     |     └─────── data (42)
  |     |     └───────────── forward encoding (FLEX_NEG_8B)
  |     └─────────────────── count = 1
  └───────────────────────── total bytes = 5
```

### Memory Efficiency

Flex is optimized for minimal memory usage:

- **No pointers**: Entire structure is relocatable
- **Variable encoding**: Only stores necessary bytes for each value
- **Shared metadata**: Header overhead amortized across elements
- **No padding**: Byte-aligned, no alignment waste

### Memory Examples

```c
/* Compare memory usage of different structures */

/* Array of pointers (traditional) */
void *ptrs[100];
size_t ptrArraySize = sizeof(ptrs);  /* 800 bytes (64-bit) */

/* Flex with same 100 integers */
flex *f = flexNew();
for (int i = 0; i < 100; i++) {
    flexPushSigned(&f, i, FLEX_ENDPOINT_TAIL);
}
size_t flexSize = flexBytes(f);
printf("Pointer array: %zu bytes\n", ptrArraySize);
printf("Flex: %zu bytes\n", flexSize);
/* Flex typically uses 300-400 bytes for 100 small integers */

flexFree(f);
```

### Allocation Strategies

```c
/* Strategy 1: Default (reallocate on each operation) */
flex *f1 = flexNew();
for (int i = 0; i < 1000; i++) {
    flexPushSigned(&f1, i, FLEX_ENDPOINT_TAIL);
}

/* Strategy 2: Preallocate (much faster for bulk insert) */
flex *f2 = flexNew();
f2 = zrealloc(f2, 10000);  /* Preallocate ~10KB */
for (int i = 0; i < 1000; i++) {
    flexPushSigned(&f2, i, FLEX_ENDPOINT_TAIL);
}
/* f2 is same size as f1, but created much faster */

flexFree(f1);
flexFree(f2);
```

## Real-World Examples

### Example 1: Simple List

```c
/* Create a shopping list */
flex *shoppingList = flexNew();

flexPushBytes(&shoppingList, "milk", 4, FLEX_ENDPOINT_TAIL);
flexPushBytes(&shoppingList, "eggs", 4, FLEX_ENDPOINT_TAIL);
flexPushBytes(&shoppingList, "bread", 5, FLEX_ENDPOINT_TAIL);

/* Print list */
flexEntry *fe = flexHead(shoppingList);
int index = 1;
while (flexEntryIsValid(shoppingList, fe)) {
    databox box = {{0}};
    flexGetByType(fe, &box);
    printf("%d. %.*s\n", index++, (int)box.len, databoxBytes(&box));
    fe = flexNext(shoppingList, fe);
}

flexFree(shoppingList);
```

### Example 2: Key-Value Map (Sorted)

```c
/* Create a sorted map of user scores */
flex *scores = flexNew();
flexEntry *middle = NULL;

/* Insert key-value pairs */
const char *users[] = {"alice", "bob", "charlie", "diana"};
int userScores[] = {95, 87, 92, 98};

for (int i = 0; i < 4; i++) {
    databox key = databoxNewBytesString(users[i]);
    databox val = databoxNewSigned(userScores[i]);
    const databox *pair[2] = {&key, &val};

    flexInsertByTypeSortedWithMiddleMultiDirect(&scores, 2, pair, &middle);
}

/* Look up score for "charlie" */
databox searchKey = databoxNewBytesString("charlie");
flexEntry *found = flexFindByTypeSortedWithMiddle(scores, 2, &searchKey, middle);

if (found) {
    databox keyBox = {{0}};
    databox valBox = {{0}};

    flexGetByType(found, &keyBox);
    flexEntry *valueEntry = flexNext(scores, found);
    flexGetByType(valueEntry, &valBox);

    printf("Score for charlie: %ld\n", valBox.data.i64);  /* 92 */
}

flexFree(scores);
```

### Example 3: Mixed-Type Record

```c
/* Create a user record with mixed types */
typedef struct {
    uint64_t id;
    const char *name;
    double balance;
    bool active;
} User;

User user = {
    .id = 12345,
    .name = "Alice Smith",
    .balance = 1234.56,
    .active = true
};

/* Encode to flex */
flex *record = flexNew();

databox fields[4];
fields[0] = databoxNewUnsigned(user.id);
fields[1] = databoxNewBytesString(user.name);
fields[2] = databoxNewReal(user.balance);
fields[3] = databoxBool(user.active);

const databox *fieldPtrs[] = {&fields[0], &fields[1], &fields[2], &fields[3]};
flexAppendMultiple(&record, 4, fieldPtrs);

/* Decode from flex */
flexEntry *fe = flexHead(record);
databox decoded[4];

for (int i = 0; i < 4; i++) {
    flexGetByType(fe, &decoded[i]);
    fe = flexNext(record, fe);
}

/* Verify */
assert(decoded[0].data.u64 == user.id);
assert(strcmp((char *)databoxBytes(&decoded[1]), user.name) == 0);
assert(decoded[2].data.d64 == user.balance);
assert(DATABOX_IS_TRUE(&decoded[3]));

flexFree(record);
```

### Example 4: Time Series Data

```c
/* Store time series of temperature readings */
flex *timeseries = flexNew();

/* Record format: [timestamp, temperature] */
time_t now = time(NULL);

for (int i = 0; i < 100; i++) {
    databox timestamp = databoxNewUnsigned(now + i * 60);  /* Every minute */
    databox temp = databoxNewReal(20.0 + (rand() % 100) / 10.0);  /* 20-30°C */

    const databox *reading[2] = {&timestamp, &temp};
    flexAppendMultiple(&timeseries, 2, reading);
}

/* Calculate average temperature */
double sum = 0.0;
int count = 0;
flexEntry *fe = flexHead(timeseries);

while (flexEntryIsValid(timeseries, fe)) {
    /* Skip timestamp, read temperature */
    fe = flexNext(timeseries, fe);
    if (!fe) break;

    databox tempBox = {{0}};
    flexGetByType(fe, &tempBox);
    sum += tempBox.data.d64;
    count++;

    fe = flexNext(timeseries, fe);
}

printf("Average temperature: %.2f°C\n", sum / count);

flexFree(timeseries);
```

### Example 5: Sorted Insert Performance Test

From the actual test suite:

```c
/* Test sorted insert and find with middle hint optimization */
flex *f = flexNew();
flexEntry *mid = NULL;

/* Insert 100 random integers in sorted order */
printf("Inserting 100 random values in sorted order...\n");
for (int i = 0; i < 100; i++) {
    databox valbox = databoxNewSigned(rand());
    flexInsertByTypeSortedWithMiddle(&f, &valbox, &mid);
    assert(mid == flexMiddle(f, 1));
}

/* Verify all can be found */
printf("Verifying all values can be found...\n");
flexEntry *fe = flexHead(f);
while (flexEntryIsValid(f, fe)) {
    databox stored = {{0}};
    flexGetByType(fe, &stored);

    /* Should find itself */
    flexEntry *found = flexFindByTypeSortedWithMiddle(f, 1, &stored, mid);
    assert(found == fe);

    fe = flexNext(f, fe);
}

printf("SUCCESS: All %zu values found correctly\n", flexCount(f));
flexFree(f);
```

### Example 6: Iterating and Deleting

From the test suite:

```c
/* Iterate from back to front, deleting all items */
flex *f = createList();  /* ["hello", "foo", "quux", 1024] */

flexEntry *fe = flexIndex(f, -1);
assert(fe == flexTail(f));

while (flexEntryIsValid(f, fe)) {
    databox box = {{0}};
    flexGetByType(fe, &box);

    printf("Entry: ");
    if (DATABOX_IS_BYTES(&box)) {
        printf("%.*s", (int)box.len, databoxBytes(&box));
    } else if (DATABOX_IS_INTEGER(&box)) {
        printf("%ld", box.data.i64);
    }
    printf("\n");

    flexDelete(&f, &fe);
    fe = flexPrev(f, fe);
}

assert(flexIsEmpty(f));
flexFree(f);
```

## Common Use Cases

### 1. Configuration Storage

Flex is ideal for storing configuration with mixed types:

```c
flex *config = flexNew();

/* Store config as key-value pairs */
typedef struct {
    const char *key;
    databox value;
} ConfigEntry;

ConfigEntry settings[] = {
    {"port", databoxNewUnsigned(8080)},
    {"host", databoxNewBytesString("localhost")},
    {"timeout", databoxNewReal(30.5)},
    {"debug", databoxBool(true)},
};

for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) {
    databox keyBox = databoxNewBytesString(settings[i].key);
    const databox *pair[2] = {&keyBox, &settings[i].value};
    flexAppendMultiple(&config, 2, pair);
}
```

### 2. Message Queues

Flex's efficient tail operations make it perfect for queues:

```c
flex *queue = flexNew();

/* Enqueue */
void enqueue(flex **q, const databox *msg) {
    flexPushByType(q, msg, FLEX_ENDPOINT_TAIL);
}

/* Dequeue */
bool dequeue(flex **q, databox *msg) {
    if (flexIsEmpty(*q)) return false;

    flexEntry *fe = flexHead(*q);
    flexGetByType(fe, msg);
    flexDeleteHead(q);
    return true;
}

/* Usage */
databox msg = databoxNewBytesString("Hello, World!");
enqueue(&queue, &msg);

databox received = {{0}};
if (dequeue(&queue, &received)) {
    printf("Received: %.*s\n", (int)received.len, databoxBytes(&received));
}

flexFree(queue);
```

### 3. Sparse Indices

Flex's compact representation is ideal for storing sparse index data:

```c
/* Store (position, value) pairs for sparse array */
flex *sparseIndex = flexNew();
flexEntry *middle = NULL;

/* Only store non-zero values */
for (int i = 0; i < 10000; i++) {
    if (rand() % 100 == 0) {  /* 1% density */
        databox pos = databoxNewUnsigned(i);
        databox val = databoxNewSigned(rand());
        const databox *entry[2] = {&pos, &val};

        flexInsertByTypeSortedWithMiddleMultiDirect(
            &sparseIndex, 2, entry, &middle);
    }
}

printf("Sparse array: 10000 positions, %zu stored (%.1f%% density)\n",
       flexCount(sparseIndex) / 2,
       100.0 * flexCount(sparseIndex) / 2 / 10000);

flexFree(sparseIndex);
```

### 4. Log Aggregation

Flex can efficiently store structured log entries:

```c
flex *logs = flexNew();

typedef struct {
    time_t timestamp;
    const char *level;
    const char *message;
} LogEntry;

void addLog(flex **logs, LogEntry *entry) {
    databox fields[3] = {
        databoxNewUnsigned(entry->timestamp),
        databoxNewBytesString(entry->level),
        databoxNewBytesString(entry->message)
    };
    const databox *fieldPtrs[3] = {&fields[0], &fields[1], &fields[2]};
    flexAppendMultiple(logs, 3, fieldPtrs);
}

/* Add some logs */
addLog(&logs, &(LogEntry){time(NULL), "INFO", "Server started"});
addLog(&logs, &(LogEntry){time(NULL), "WARN", "High memory usage"});
addLog(&logs, &(LogEntry){time(NULL), "ERROR", "Connection failed"});

flexFree(logs);
```

## Integration with Other Modules

### With databox

Flex and databox are designed to work seamlessly together:

```c
/* databox provides the type system, flex provides the storage */
databox box = databoxNewSigned(42);
flex *f = flexNew();
flexPushByType(&f, &box, FLEX_ENDPOINT_TAIL);

flexEntry *fe = flexHead(f);
databox retrieved = {{0}};
flexGetByType(fe, &retrieved);
assert(databoxEqual(&box, &retrieved));

flexFree(f);
```

### With multimapAtom (Reference Deduplication)

Flex supports storing references instead of duplicating large keys:

```c
#include "multimapAtom.h"

multimapAtom *atom = multimapAtomNew();
flex *f = flexNew();
flexEntry *middle = NULL;

/* Instead of storing duplicate keys, store references */
for (int i = 0; i < 1000; i++) {
    databox key = databoxNewBytesString("common-key");
    databox value = databoxNewSigned(i);

    /* Convert key to atom reference (deduplicated) */
    multimapAtomInsertIfNewConvert(atom, &key);

    const databox *pair[2] = {&key, &value};
    flexInsertReplaceByTypeSortedWithMiddleMultiWithReference(
        &f, 2, pair, &middle, true, atom);
}

/* All 1000 entries share the same key reference, not 1000 copies! */

multimapAtomFree(atom);
flexFree(f);
```

### With multimap/multilist

Flex serves as the underlying storage for these higher-level structures:

```c
/* multimap internally uses flex for storing key-value pairs */
/* multilist uses flex for list operations */
/* These modules provide higher-level APIs on top of flex */
```

### As Nested Container

Flexes can contain other flexes:

```c
/* Create nested structure */
flex *inner = flexNew();
flexPushSigned(&inner, 1, FLEX_ENDPOINT_TAIL);
flexPushSigned(&inner, 2, FLEX_ENDPOINT_TAIL);
flexPushSigned(&inner, 3, FLEX_ENDPOINT_TAIL);

/* Store inner flex as container in outer flex */
databox innerBox = {
    .type = DATABOX_CONTAINER_FLEX_LIST,
    .data.bytes.start = (uint8_t *)inner,
    .len = flexBytes(inner)
};

flex *outer = flexNew();
flexPushByType(&outer, &innerBox, FLEX_ENDPOINT_TAIL);

/* Retrieve nested flex */
flexEntry *fe = flexHead(outer);
databox retrieved = {{0}};
flexGetByType(fe, &retrieved);

assert(retrieved.type == DATABOX_CONTAINER_FLEX_LIST);
flex *retrievedInner = (flex *)retrieved.data.bytes.start;
assert(flexCount(retrievedInner) == 3);

flexFree(outer);
flexFree(inner);
```

## Best Practices

### 1. Always Check Return Values

```c
/* WRONG - assumes success */
flexEntry *fe = flexIndex(f, 100);
databox box = {{0}};
flexGetByType(fe, &box);  /* Crash if fe is NULL! */

/* RIGHT - check for validity */
flexEntry *fe = flexIndex(f, 100);
if (fe && flexEntryIsValid(f, fe)) {
    databox box = {{0}};
    flexGetByType(fe, &box);
}
```

### 2. Use Middle Hints for Sorted Operations

```c
/* SLOW - O(log n) each time, searching from start */
for (int i = 0; i < 1000; i++) {
    databox box = databoxNewSigned(i);
    flexInsertByTypeSorted(&f, &box);
}

/* FAST - O(log n/2) average, reuses middle position */
flexEntry *middle = NULL;
for (int i = 0; i < 1000; i++) {
    databox box = databoxNewSigned(i);
    flexInsertByTypeSortedWithMiddle(&f, &box, &middle);
}
```

### 3. Preallocate for Bulk Inserts

```c
/* SLOW - many reallocs */
flex *f = flexNew();
for (int i = 0; i < 10000; i++) {
    flexPushSigned(&f, i, FLEX_ENDPOINT_TAIL);
}

/* FAST - one allocation */
flex *f2 = flexNew();
f2 = zrealloc(f2, 100000);  /* Preallocate space */
for (int i = 0; i < 10000; i++) {
    flexPushSigned(&f2, i, FLEX_ENDPOINT_TAIL);
}
```

### 4. Use Drain for Bulk Deletes

```c
/* SLOW - realloc on each delete */
while (flexCount(f) > 0) {
    flexDeleteOffsetCount(&f, 0, 1);
}

/* FAST - no realloc until done */
while (flexCount(f) > 0) {
    flexDeleteOffsetCountDrain(&f, 0, 1);
}
flexReset(&f);  /* Final cleanup */
```

### 5. Prefer Tail Operations

```c
/* SLOW - O(n) due to shifting all data */
flexPushBytes(&f, "new", 3, FLEX_ENDPOINT_HEAD);

/* FAST - O(1) amortized */
flexPushBytes(&f, "new", 3, FLEX_ENDPOINT_TAIL);
```

## Common Pitfalls

### 1. Stale Entry Pointers

```c
/* WRONG - entry pointer invalidated after modification */
flexEntry *fe = flexHead(f);
flexPushBytes(&f, "new", 3, FLEX_ENDPOINT_TAIL);
flexGetByType(fe, &box);  /* fe may be invalid if realloc happened! */

/* RIGHT - get fresh pointer after modification */
flexPushBytes(&f, "new", 3, FLEX_ENDPOINT_TAIL);
flexEntry *fe = flexHead(f);
flexGetByType(fe, &box);
```

### 2. Forgetting Element Count in Multi-Element Operations

```c
/* WRONG - searching for key-value pair as single element */
flexEntry *found = flexFindByTypeSortedWithMiddle(f, 1, &key, mid);

/* RIGHT - specify 2 elements per entry for key-value pairs */
flexEntry *found = flexFindByTypeSortedWithMiddle(f, 2, &key, mid);
```

### 3. Not Handling Empty Flex

```c
/* WRONG - crash if flex is empty */
flexEntry *fe = flexTail(f);
flexGetByType(fe, &box);

/* RIGHT - check first */
if (!flexIsEmpty(f)) {
    flexEntry *fe = flexTail(f);
    flexGetByType(fe, &box);
}
```

### 4. Mixing Sorted and Unsorted Operations

```c
/* WRONG - breaks sorted order */
flex *f = flexNew();
flexEntry *mid = NULL;
flexInsertByTypeSortedWithMiddle(&f, &box1, &mid);
flexPushByType(&f, &box2, FLEX_ENDPOINT_TAIL);  /* Breaks order! */
flexFindByTypeSortedWithMiddle(f, 1, &box3, mid);  /* May not find! */

/* RIGHT - stick to one mode or rebuild */
/* Either all sorted operations, or all unsorted operations */
```

## Thread Safety

Flex is **not thread-safe**. Concurrent access requires external synchronization:

```c
#include <pthread.h>

pthread_mutex_t flex_lock = PTHREAD_MUTEX_INITIALIZER;
flex *shared_flex = flexNew();

/* Thread-safe operations */
void thread_safe_push(databox *box) {
    pthread_mutex_lock(&flex_lock);
    flexPushByType(&shared_flex, box, FLEX_ENDPOINT_TAIL);
    pthread_mutex_unlock(&flex_lock);
}
```

## See Also

- [databox](../core/DATABOX.md) - Universal 16-byte container used by flex
- [multimap](../multimap/MULTIMAP.md) - Hash table implementation using flex
- [multilist](../multilist/MULTILIST.md) - List abstraction using flex
- [multimapAtom](../multi/MULTIMAPATOM.md) - Reference deduplication for flex
- [Architecture Overview](../../ARCHITECTURE.md) - Design philosophy

## Testing

Run the flex test suite:

```bash
./src/datakit-test test flex
```

The test suite includes:

- Basic push/pop operations
- Index access (positive and negative)
- Forward and backward iteration
- Sorted insert and search
- Binary search with middle hints
- Merge and split operations
- Delete operations (single and range)
- Large data handling (>255 bytes)
- Compression/decompression
- Stress tests with random data
- Performance benchmarks

## References

The flex encoding is inspired by:

- **ziplist** from Redis (compact list encoding)
- **listpack** from Redis (improved ziplist successor)
- **Variable-length encoding** from Protocol Buffers
- **Split varints** for efficient byte count storage

Key improvements over predecessors:

- Symmetric forward/backward encoding for O(1) reverse traversal
- Wider type support (128-bit ints, float16, nested containers)
- Better sorted operation support with middle hints
- Reference types for deduplication
- Compression support

## License

Copyright 2016-2020 Matt Stancliff <matt@genges.com>

Licensed under the Apache License, Version 2.0
