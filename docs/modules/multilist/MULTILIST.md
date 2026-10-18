# multilist - Scale-Aware Linked List

## Overview

`multilist` is a **high-performance linked list** that stores multiple elements per node instead of one element per node. By packing multiple values into compact memory arrays (flex), multilist reduces pointer overhead and improves cache locality compared to traditional linked lists.

**Key Features:**

- Automatic scaling from 8 bytes (Small) to 16 bytes (Medium) to 24 bytes plus a node array (Full)
- Multi-element nodes remove the per-element pointer pair
- O(1) push/pop at both ends (efficient queue/stack/deque)
- Forward and reverse iteration
- Random access by index with O(n) traversal
- Insert before/after any position
- Optional LZ4 compression of interior nodes (Full variant only)
- Transparent automatic upgrades between variants

**Headers**: `multilist.h`, `multilistCommon.h`

**Source**: `multilist.c`, `multilistSmall.c`, `multilistMedium.c`, `multilistFull.c`

## Architecture

The multilist family uses **pointer tagging** to transparently manage three underlying implementations:

```
┌─────────────┐
│  multilist* │  Opaque pointer with a 2-bit low type tag
└──────┬──────┘  plus two 8-bit high fields (compress depth, size limit)
       │
       ├──[tag=1]──> multilistSmall  (8 bytes, single flex)
       ├──[tag=2]──> multilistMedium (16 bytes, two flexes)
       └──[tag=3]──> multilistFull   (24 bytes + multiarray of mflex nodes)
```

The configuration you pass to `multilistNew()` lives in the _pointer_, not in the
Small/Medium structs: `ptrlibCompress.h` stores the compression depth and the
size limit in the top 8-bit fields of the tagged pointer (see `ptrlib.h`,
`PTRLIB_BITS_HIGH 16`). This is why every mutating call takes `multilist **`:
the tag bits and the metadata bits travel with the pointer when a variant is
replaced.

### What is a Multilist Node?

Traditional linked lists store one element per node:

```
Traditional Linked List:
[ptr|data|ptr] -> [ptr|data|ptr] -> [ptr|data|ptr] -> NULL
```

Multilist stores multiple elements per node in a compact flex array:

```
Multilist:
[node] -> [flex: elem elem elem elem elem] -> [flex: elem elem elem] -> NULL
```

How many elements land in one node depends entirely on the configured
`flexCapSizeLimit` and on the encoded size of your values, so a node holds
"as many entries as fit under the byte limit", not a fixed count.

**Benefits:**

- **Reduced Overhead**: one node pointer per _node_, not per element
- **Cache Efficiency**: elements packed contiguously in one allocation
- **Compression**: Full variant can compress interior nodes with LZ4
- **Flexibility**: nodes grow/shrink and split as needed

When you call `multilistPushByTypeHead()`, the implementation:

1. Reads the current variant from the pointer's low tag bits
2. Routes to the appropriate function (Small/Medium/Full)
3. Upgrades to the next variant if the byte threshold was exceeded
4. Writes the new tagged pointer back through your `multilist **`

**You never need to know which variant you're using** - the API is identical.

## Data Structures

### Common Types

From `multilistCommon.h`:

```c
/* Node ID type - index into the node array (Medium/Full) */
typedef int32_t mlNodeId;

/* Offset ID type - index into the entire list across all nodes */
typedef int64_t mlOffsetId;

/* Iterator for traversing a multilist.
 * Oversized for Small/Medium because the Full-only state lives here too. */
typedef struct multilistIterator {
    void *ml;       /* pointer back to the (untagged) multilist instance */
    flexEntry *fe;  /* current entry position inside the current flex */
    int32_t offset; /* offset in current flex */

    /* Medium and Full */
    mlNodeId nodeIdx;

    /* Full only */
    flex *f;
    mflexState *state[2];
    bool readOnly;

    /* All */
    bool forward;
    uint32_t type; /* variant tag, used for function dispatching */
} multilistIterator;

/* Result of multilistNext(), multilistIndex() or multilistIndexNear();
 * the value itself is in 'box' */
typedef struct multilistEntry {
    void *ml;         /* pointer back to the (untagged) multilist instance */
    flexEntry *fe;
    union {
        databox box;       /* the element */
        databoxBig boxBig; /* same bytes; 'extra' holds a wide payload */
    };
    mlNodeId nodeIdx; /* only used for Medium and Full */
    int32_t offset;
    flex *f;          /* Full only */
} multilistEntry; /* 64 bytes, 16-byte aligned */

/* State for compression operations (Full variant) */
typedef mflexState multilistState;
```

Note the field order above matches the header exactly; `multilistEntry` has no
`type` field, and neither struct is meant to be constructed by hand - always
fill one via `multilistNext()` / `multilistIndex()` / `multilistIndexNear()`.
They fill every field and never read the entry's previous contents, so an
uninitialized entry is fine.

The entry carries its own storage for 96/128-bit elements. Every read decodes
with `flexGetByTypeBig()` into `boxBig`, so a wide element reads back whole:
`box.type` is `DATABOX_SIGNED_128` / `DATABOX_UNSIGNED_128`, `box.big` is 1,
and `box.data.i128` / `box.data.u128` points at `boxBig.extra`. Every other
value has `box.big == 0`.

`entry.box` **borrows**, in two ways:

- BYTES and containers point into the list's flex. They stay valid until the
  list is next modified.
- A 96/128-bit value points into the entry itself. It stays valid until the
  entry is reused or goes out of scope, so a struct copy
  (`databox v = entry.box;`) still points back into the entry.

To keep a value beyond that, copy it. `databoxCopy(&entry.box)` gives an owned
copy (a wide payload goes to a 16-byte heap block; release it with
`databoxFreeData()`). `databoxBigCopyFrom(&dst, &entry.box)` re-homes the value
into another `databoxBig` without allocating.

## Creation Functions

### Basic Creation

```c
/* Create new multilist with size limit and compression depth */
multilist *multilistNew(flexCapSizeLimit limit, uint32_t depth);

/* Create from existing flex (consumes flex) */
multilist *multilistNewFromFlex(flexCapSizeLimit limit, uint32_t depth,
                                flex *fl);

/* Deep copy entire multilist */
multilist *multilistDuplicate(const multilist *ml);

/* Free entire multilist */
void multilistFree(multilist *m);
```

```c
/* Example: Basic creation */
multilist *ml = multilistNew(FLEX_CAP_LEVEL_2048, 0);
/* Creates a Small variant with a 2 KB node limit and no compression */

/* Example: With compression */
multilist *compressed = multilistNew(FLEX_CAP_LEVEL_4096, 2);
/* 4 KB nodes; once the list becomes Full, all but the first 2 and last 2
 * nodes are eligible for LZ4 compression */

/* Example: From existing data (multilistNewFromFlex consumes 'existing') */
flex *existing = flexNew();
multilist *fromFlex = multilistNewFromFlex(FLEX_CAP_LEVEL_2048, 0, existing);
/* 'existing' now belongs to the multilist; do not free it yourself */
```

`multilistNew()` and `multilistNewFromFlex()` both start as **Small**.
`multilistNewFromFlex()` does not upgrade eagerly even if `fl` is already larger
than the limit - the upgrade check runs on the next write operation.

### Size Limits

`flexCapSizeLimit` (from `flexCapacityManagement.h`) is an index into
`flexOptimizationSizeLimit[]`; the byte value at that index is the per-node
size target:

```c
FLEX_CAP_LEVEL_0       /* 0 bytes - every write upgrades immediately */
FLEX_CAP_LEVEL_64      /* 64 bytes */
FLEX_CAP_LEVEL_128     /* 128 bytes */
FLEX_CAP_LEVEL_256     /* 256 bytes */
FLEX_CAP_LEVEL_512     /* 512 bytes */
FLEX_CAP_LEVEL_1024    /* 1 KB */
FLEX_CAP_LEVEL_2048    /* 2 KB */
FLEX_CAP_LEVEL_4096    /* 4 KB */
FLEX_CAP_LEVEL_8192    /* 8 KB */
FLEX_CAP_LEVEL_16384   /* 16 KB */
FLEX_CAP_LEVEL_32768   /* 32 KB */
FLEX_CAP_LEVEL_65536   /* 64 KB */
```

There is no implicit default: `multilistNew()` requires an explicit level.

**Compression Depth**: only meaningful for the Full variant. Depth N leaves the
first N and last N nodes uncompressed (fast head/tail access) and compresses
interior nodes with LZ4. Depth 0 disables compression. The depth is stored in an
8-bit field of the tagged pointer, so the usable range is 0-255;
`multilistFullSetCompressDepth()` additionally clamps to 32767 internally.

### Metadata Functions

```c
/* Total element count */
size_t multilistCount(const multilist *m);

/* Total *logical* bytes of the underlying flex nodes */
size_t multilistBytes(const multilist *m);
```

`multilistBytes()` reports the sum of each node's **uncompressed** flex size
(`mflexBytesUncompressed()` for Full). It is a measure of stored data, not of
allocator footprint, and it does **not** shrink when interior nodes are
LZ4 compressed. There is no public accessor for the compressed footprint.

```c
printf("List contains %zu elements\n", multilistCount(ml));
printf("Holding %zu logical bytes\n", multilistBytes(ml));
```

### About the `mflexState` argument

Every data-path function takes an `mflexState *` (or `mflexState *state[2]`).
Compression state is only used by the Full variant; the Small and Medium
adapters discard the argument entirely (`multilistAdapter.h`). Because any list
can silently become Full on the next push, always pass a real state:

```c
mflexState *s0 = mflexStateCreate();
mflexState *s1 = mflexStateCreate();
mflexState *s[2] = {s0, s1};
/* ... use ... */
mflexStateFree(s0);
mflexStateFree(s1);
```

Functions taking a single `mflexState *` want `s[0]`; functions taking
`mflexState *state[2]` want `s`.

## Insertion Operations

### Push to Ends

```c
void multilistPushByTypeHead(multilist **m, mflexState *state,
                             const databox *box);
void multilistPushByTypeTail(multilist **m, mflexState *state,
                             const databox *box);
```

```c
/* Example: Build a queue (push tail, pop head) */
multilist *queue = multilistNew(FLEX_CAP_LEVEL_2048, 0);
mflexState *state = mflexStateCreate();

databox item1 = databoxNewBytesString("first");
multilistPushByTypeTail(&queue, state, &item1);

databox item2 = databoxNewBytesString("second");
multilistPushByTypeTail(&queue, state, &item2);

databox item3 = databoxNewBytesString("third");
multilistPushByTypeTail(&queue, state, &item3);

/* List is now: first -> second -> third */

/* Example: Build a stack (push head, pop head) */
multilist *stack = multilistNew(FLEX_CAP_LEVEL_2048, 0);

databox val1 = databoxNewSigned(10);
multilistPushByTypeHead(&stack, state, &val1);

databox val2 = databoxNewSigned(20);
multilistPushByTypeHead(&stack, state, &val2);

databox val3 = databoxNewSigned(30);
multilistPushByTypeHead(&stack, state, &val3);

/* List is now: 30 -> 20 -> 10 */
```

**Important**: insertion takes `multilist **m` (pointer to pointer) so the
variant upgrade and the pointer-carried metadata can be written back.

`databoxNewBytesString()` builds a `DATABOX_BYTES` box that only _points at_
your buffer; the push copies the bytes into the flex, so a stack buffer is safe
and no ownership transfers.

### Insert at Position

```c
void multilistInsertByTypeBefore(multilist **m, mflexState *state[2],
                                 multilistEntry *node, const databox *box);
void multilistInsertByTypeAfter(multilist **m, mflexState *state[2],
                                multilistEntry *node, const databox *box);
```

```c
/* Example: Insert in middle of list */
multilist *ml = multilistNew(FLEX_CAP_LEVEL_2048, 0);
mflexState *s[2] = {mflexStateCreate(), mflexStateCreate()};

databox a = databoxNewBytesString("a");
multilistPushByTypeTail(&ml, s[0], &a);

databox c = databoxNewBytesString("c");
multilistPushByTypeTail(&ml, s[0], &c);

/* List: a -> c */

/* Get entry at position 1 (which is "c") */
multilistEntry entry;
multilistIndexGet(ml, s[0], 1, &entry);

/* Insert "b" before "c" */
databox b = databoxNewBytesString("b");
multilistInsertByTypeBefore(&ml, s, &entry, &b);

/* List is now: a -> b -> c */
```

Note the asymmetry, which is real and not a typo: push takes a single
`mflexState *`, insert takes `mflexState *state[2]`.

## Deletion Operations

### Pop from Ends

```c
bool multilistPop(multilist **m, mflexState *state, databox *got,
                  bool fromTail);

/* Convenience macros */
#define multilistPopTail(ml, s, box) multilistPop(ml, s, box, true)
#define multilistPopHead(ml, s, box) multilistPop(ml, s, box, false)
```

The element is removed, so a successful pop returns a **self-contained** copy
in `got` (`flexGetByTypeCopy()`). The behavior is the same on every tier:

- BYTES and containers are heap copies (`DATABOX_BYTES`, `allocated = 1`).
- A 96/128-bit value goes into the storage `got->data` points at when
  `got->big` is set (a `DATABOX_BIG_INIT`'d `databoxBig`). Otherwise it goes
  into a 16-byte heap block (`allocated = 1`, `big = 1`).
- Every other value has `allocated = 0` and `big = 0`.

`got` must be zeroed (`databox got = {{0}};`) or a `DATABOX_BIG_INIT`'d
`databoxBig`, because its `big` flag is read. Always release the result with
`databoxFreeData()`. That call is a no-op for values that own nothing, and for
a wide value held inline in a `databoxBig`. When the list is empty the pop
returns false and leaves `got` untouched.

```c
/* Receive a wide value inline, without a heap block */
databoxBig wide;
DATABOX_BIG_INIT(&wide);
if (multilistPopTail(&ml, state, (databox *)&wide)) {
    /* wide.data.u128 points at wide.extra for a 96/128-bit value */
    databoxFreeData((databox *)&wide);
}
```

```c
#include <inttypes.h>

/* Example: Queue pattern (FIFO) */
multilist *queue = multilistNew(FLEX_CAP_LEVEL_2048, 0);
mflexState *state = mflexStateCreate();

databox item1 = databoxNewBytesString("first");
multilistPushByTypeTail(&queue, state, &item1);

databox item2 = databoxNewBytesString("second");
multilistPushByTypeTail(&queue, state, &item2);

databox result = {{0}};
if (multilistPopHead(&queue, state, &result)) {
    printf("Dequeued: %.*s\n", (int)result.len, databoxCBytes(&result));
    /* Prints: "first" */
    databoxFreeData(&result);
}

/* Example: Stack pattern (LIFO) */
multilist *stack = multilistNew(FLEX_CAP_LEVEL_2048, 0);

databox val1 = databoxNewSigned(10);
multilistPushByTypeHead(&stack, state, &val1);

databox val2 = databoxNewSigned(20);
multilistPushByTypeHead(&stack, state, &val2);

databox popped = {{0}};
if (multilistPopHead(&stack, state, &popped)) {
    printf("Popped: %" PRId64 "\n", popped.data.i64);
    /* Prints: 20 (last in, first out) */
    databoxFreeData(&popped);
}
```

Integer values round-trip as `DATABOX_SIGNED_64` / `DATABOX_UNSIGNED_64`; read
them from `box.data.i64` / `box.data.u64` and print with `PRId64` / `PRIu64`.
Byte values are **not** NUL-terminated by the container - always print or copy
using `box.len`, and discriminate on `box.type` (or the `DATABOX_IS_INTEGER()` /
`DATABOX_IS_FLOAT()` macros) when a list mixes value kinds. Note that a numeric
string pushed as bytes is stored by flex in its integer encoding and therefore
comes back as an integer box, not as bytes.

### Delete by Position

```c
/* Delete range of elements */
bool multilistDelRange(multilist **m, mflexState *state, mlOffsetId start,
                       int64_t values);

/* Delete the entry an iterator is currently positioned on */
void multilistDelEntry(multilistIterator *iter, multilistEntry *entry);
```

```c
/* Example: Delete range */
mflexState *state = mflexStateCreate();

/* Delete 10 elements starting at index 50 */
bool deleted = multilistDelRange(&ml, state, 50, 10);

/* Negative start indexes from the tail */
multilistDelRange(&ml, state, -5, 5);  /* Delete last 5 elements */

/* Example: Delete while iterating (supported) */
multilistIterator iter;
multilistIteratorInitForward(ml, s, &iter);
multilistEntry entry;

while (multilistNext(&iter, &entry)) {
    if (flexCompareBytes(entry.fe, "delete_me", 9)) {
        multilistDelEntry(&iter, &entry);
        /* iteration may continue: DelEntry repositions the iterator */
    }
}

multilistIteratorRelease(&iter);
```

**Delete during iteration is supported** (`multilistDelEntry()` fixes up the
iterator position, and the test suite relies on it). **Insert during iteration
is not**: after `multilistInsertByType{Before,After}()` you must stop iterating
immediately, because the insert can split a node or upgrade the variant out from
under the iterator.

Use a read-write iterator (`multilistIteratorInitForward` /
`...InitReverse`) when you intend to delete; the read-only variants exist for
pure traversal.

### Replace at Index

```c
bool multilistReplaceByTypeAtIndex(multilist **m, mflexState *state,
                                   mlNodeId index, const databox *box);
```

Note the public prototype takes `mlNodeId` (`int32_t`), not `mlOffsetId`, so
replacement is limited to indexes that fit in 32 bits. Negative indexes count
from the tail.

```c
mflexState *state = mflexStateCreate();

databox newValue = databoxNewBytesString("replaced");
bool replaced = multilistReplaceByTypeAtIndex(&ml, state, 5, &newValue);
```

## Access Operations

### Index Access

```c
bool multilistIndex(const multilist *ml, mflexState *state, mlOffsetId index,
                    multilistEntry *entry, bool openNode);

/* Convenience macros */
#define multilistIndexGet(ml, s, i, e) multilistIndex(ml, s, i, e, true)
#define multilistIndexCheck(ml, s, i, e) multilistIndex(ml, s, i, e, false)
```

`openNode` only matters for Full: `true` decompresses the containing node so
`entry.box` / `entry.fe` are usable; `false` performs an existence/bounds check
without paying decompression cost. Small and Medium ignore the flag.

```c
mflexState *state = mflexStateCreate();
multilistEntry entry;

/* Get first element (index 0) */
if (multilistIndexGet(ml, state, 0, &entry)) {
    printf("First: %.*s\n", (int)entry.box.len, entry.box.data.bytes.cstart);
}

/* Get last element (index -1) */
if (multilistIndexGet(ml, state, -1, &entry)) {
    printf("Last: %.*s\n", (int)entry.box.len, entry.box.data.bytes.cstart);
}

/* Get middle element */
const size_t count = multilistCount(ml);
if (multilistIndexGet(ml, state, (mlOffsetId)(count / 2), &entry)) {
    printf("Middle: %.*s\n", (int)entry.box.len, entry.box.data.bytes.cstart);
}

/* Check whether an index exists without decompressing its node */
if (multilistIndexCheck(ml, state, 100, &entry)) {
    printf("Index 100 exists\n");
}
```

The returned `entry.box` borrows (see the lifetime rules under
`multilistEntry`): a byte value is valid only until the next mutation, a
96/128-bit value lives in the entry itself, and the box must not be passed to
`databoxFreeData()`. An entry's byte value is always plain `DATABOX_BYTES`, so
`entry.box.data.bytes.cstart` is safe here (and `databoxCBytes(&entry.box)`
works too). With `openNode` false the Full tier only checks that the index
exists; it does not set `entry.fe` or `entry.box`.

## Iteration

### Basic Iteration

```c
void multilistIteratorInit(multilist *ml, mflexState *state[2],
                           multilistIterator *iter, bool forward,
                           bool readOnly);

/* Convenience macros */
#define multilistIteratorInitReadOnly(ml, s, iter, forward)
#define multilistIteratorInitForwardReadOnly(ml, s, iter)
#define multilistIteratorInitForward(ml, s, iter)
#define multilistIteratorInitReverse(ml, s, iter)
#define multilistIteratorInitReverseReadOnly(ml, s, iter)

/* Initialize at a specific index */
bool multilistIteratorInitAtIdx(const multilist *ml, mflexState *state[2],
                                multilistIterator *iter, mlOffsetId idx,
                                bool forward, bool readOnly);

/* Convenience macros */
#define multilistIteratorInitAtIdxForward(ml, s, iter, idx)
#define multilistIteratorInitAtIdxForwardReadOnly(ml, s, iter, idx)
#define multilistIteratorInitAtIdxReverse(ml, s, iter, idx)
#define multilistIteratorInitAtIdxReverseReadOnly(ml, s, iter, idx)

/* Advance iterator */
bool multilistNext(multilistIterator *iter, multilistEntry *entry);

/* Release iterator resources (no-op unless the list is Full) */
void multilistIteratorRelease(multilistIterator *iter);
```

`multilistIteratorInit()` returns void and always succeeds;
`multilistIteratorInitAtIdx()` returns `false` when `idx` is out of range.

```c
mflexState *s[2] = {mflexStateCreate(), mflexStateCreate()};

/* Example: Forward iteration */
multilistIterator iter = {0};
multilistIteratorInitForwardReadOnly(ml, s, &iter);

multilistEntry entry;
while (multilistNext(&iter, &entry)) {
    printf("Element: %.*s\n", (int)entry.box.len, entry.box.data.bytes.cstart);
}

multilistIteratorRelease(&iter);

/* Example: Reverse iteration */
multilistIteratorInitReverseReadOnly(ml, s, &iter);

while (multilistNext(&iter, &entry)) {
    printf("Reverse: %.*s\n", (int)entry.box.len, entry.box.data.bytes.cstart);
}

multilistIteratorRelease(&iter);

/* Example: Start at a specific index */
if (multilistIteratorInitAtIdxForwardReadOnly(ml, s, &iter, 50)) {
    while (multilistNext(&iter, &entry)) {
        printf("From 50: %.*s\n", (int)entry.box.len,
               entry.box.data.bytes.cstart);
    }

    multilistIteratorRelease(&iter);
}
```

Zero-initialize the iterator (`multilistIterator iter = {0};`) before the first
init call, matching the test suite's usage.

### Restarting Iteration

`multilist.h` declares `multilistRewind()` and `multilistRewindTail()`, but
**neither has an implementation in the multilist sources** - only the
per-variant `multilistSmallRewind()` / `multilistFullRewind()` style functions
exist, and they are not wired into the dispatcher. Referencing the unified names
will not link. To make a second pass, just initialize the iterator again:

```c
multilistIterator iter = {0};

multilistIteratorInitForwardReadOnly(ml, s, &iter);
while (multilistNext(&iter, &entry)) {
    /* first pass */
}
multilistIteratorRelease(&iter);

multilistIteratorInitForwardReadOnly(ml, s, &iter);
while (multilistNext(&iter, &entry)) {
    /* second pass */
}
multilistIteratorRelease(&iter);
```

## Special Operations

### Rotate

```c
/* Move the tail element to the head */
void multilistRotate(multilist *m, mflexState *state[2]);
```

`multilistRotate()` copies the **tail** entry to the **head** and deletes the
old tail. It takes `multilist *` (not `multilist **`) because rotation cannot
change the element count and therefore never triggers an upgrade. Rotating a
list of 0 or 1 elements is a no-op. A 96/128-bit tail moves intact.

```c
/* Example: Round-robin processing.
 * Because rotate pulls from the tail, read the tail before each rotation. */
mflexState *s[2] = {mflexStateCreate(), mflexStateCreate()};

for (uint32_t round = 0; round < 1000; round++) {
    multilistEntry entry;

    if (multilistIndexGet(tasks, s[0], -1, &entry)) {
        printf("Processing: %.*s\n", (int)entry.box.len,
               entry.box.data.bytes.cstart);

        /* Move that element to the front; the next tail becomes current */
        multilistRotate(tasks, s);
    }
}
```

### Dump

```c
/* Every element, in list order, as one new flex */
flex *multilistDump(const multilist *m, mflexState *state);
```

`multilistDump()` returns a new flex holding every element in order. Release
it with `flexFree()`. The element encodings are copied byte for byte and
never decoded. The result is therefore identical to pushing each element onto
an empty flex in order, including 96/128-bit values. Small duplicates its
flex, Medium merges its two flexes with one allocation, and Full appends each
node. `state` is scratch space for opening compressed Full nodes, and Small and
Medium ignore it. The dump only reads the list. The persist layer uses it for
multilist snapshots.

## Performance Characteristics

### Time Complexity

| Operation      | Small         | Medium        | Full           |
| -------------- | ------------- | ------------- | -------------- |
| Push Head/Tail | O(1) - O(n)\* | O(1) - O(n)\* | O(1) amortized |
| Pop Head/Tail  | O(1)          | O(1)          | O(1)           |
| Index Access   | O(n)          | O(n)          | O(n)           |
| Insert         | O(n)          | O(n)          | O(n)           |
| Delete         | O(n)          | O(n)          | O(n)           |
| Iteration      | O(n)          | O(n)          | O(n)           |
| Rotate         | O(n)\*\*      | O(n)\*\*      | O(n)\*\*       |

\* O(1) when the value fits in the current node; O(n) when the node must be
split or the list must be upgraded to the next variant.

\*\* Rotate is O(1) in _element count moved_ (exactly one), but pushing to the
head of a flex memmoves the node contents, so it is linear in the size of the
head node - and for Medium with a 1-element side, it is a pointer swap.

For Full, index access and iteration first locate the node (walking the
`multiarray` of nodes) and then scan inside it, and may need to decompress the
node; the total remains O(n) in elements.

### Space Complexity

| Variant | Fixed Overhead | Per-Node Overhead                    |
| ------- | -------------- | ------------------------------------ |
| Small   | 8 bytes        | one flex header (2 bytes when empty) |
| Medium  | 16 bytes       | two flex headers                     |
| Full    | 24 bytes       | 8 bytes per node pointer + header    |

Full's node pointers live in a `multiarray` that promotes its own storage as it
grows (`STORAGE_MAX 2048` entries per array chunk in `multilistFull.c`), so
per-node cost is the 8-byte `mflex *` plus the node's own flex/cflex header.
There is no per-element `prev`/`next` pair in any variant.

Which variant a list occupies depends on the _configured_ limit, not on fixed
byte counts - see the thresholds below.

### Automatic Upgrade Thresholds

From `multilistUpgradeIfNecessary()` in `multilist.c`, where
`limitBytes = flexOptimizationSizeLimit[limit]`:

```c
Small -> Medium:  multilistSmallBytes(small)  >  limitBytes
Medium -> Full:   multilistMediumBytes(medium) > (limitBytes * 3)
```

So with `FLEX_CAP_LEVEL_2048` a list is Small up to 2 KB of stored data, Medium
up to 6 KB, and Full beyond that. With `FLEX_CAP_LEVEL_0` the limit is 0 bytes
and the first write promotes immediately. The check runs after every push and
insert; it never downgrades.

### vs Traditional Linked List

**Traditional linked list:** two pointers (16 bytes on 64-bit) _per element_,
each element in its own allocation, no locality, no compression.

**Multilist:** node pointers are amortized across every element the node holds,
elements are contiguous inside one flex allocation, values are varint/type
encoded rather than stored as fixed 8-byte slots, and interior nodes of a Full
list can be LZ4 compressed.

The actual ratio depends on your element sizes and node limit, so measure with
your own data rather than assuming a fixed multiplier; there is no published
multilist baseline in `docs/PERFORMANCE_BASELINES.md` yet.

## Common Patterns

### Pattern 1: Queue (FIFO)

```c
/* Push to tail, pop from head */
multilist *queue = multilistNew(FLEX_CAP_LEVEL_2048, 0);
mflexState *state = mflexStateCreate();

databox item = databoxNewBytesString("task");
multilistPushByTypeTail(&queue, state, &item);

databox result = {{0}};
if (multilistPopHead(&queue, state, &result)) {
    /* Process result */
    databoxFreeData(&result);
}
```

### Pattern 2: Stack (LIFO)

```c
#include <inttypes.h>

/* Push to head, pop from head */
multilist *stack = multilistNew(FLEX_CAP_LEVEL_2048, 0);
mflexState *state = mflexStateCreate();

databox item = databoxNewSigned(42);
multilistPushByTypeHead(&stack, state, &item);

databox result = {{0}};
if (multilistPopHead(&stack, state, &result)) {
    printf("Popped: %" PRId64 "\n", result.data.i64);
    databoxFreeData(&result);
}
```

### Pattern 3: Deque (Double-Ended Queue)

```c
multilist *deque = multilistNew(FLEX_CAP_LEVEL_2048, 0);
mflexState *state = mflexStateCreate();

databox front = databoxNewBytesString("front");
multilistPushByTypeHead(&deque, state, &front);

databox back = databoxNewBytesString("back");
multilistPushByTypeTail(&deque, state, &back);

databox result = {{0}};
if (multilistPopHead(&deque, state, &result)) {
    databoxFreeData(&result);
}

if (multilistPopTail(&deque, state, &result)) {
    databoxFreeData(&result);
}
```

### Pattern 4: Bounded Recent-History Buffer

```c
multilist *buffer = multilistNew(FLEX_CAP_LEVEL_2048, 0);
mflexState *state = mflexStateCreate();

const size_t maxSize = 100;

databox item = databoxNewBytesString("new data");
multilistPushByTypeHead(&buffer, state, &item);

/* Evict the oldest entry once over the cap */
if (multilistCount(buffer) > maxSize) {
    databox old = {{0}};
    if (multilistPopTail(&buffer, state, &old)) {
        databoxFreeData(&old);
    }
}
```

### Pattern 5: Range Processing

```c
mflexState *s[2] = {mflexStateCreate(), mflexStateCreate()};

const size_t chunkSize = 100;
const size_t total = multilistCount(ml);

for (size_t start = 0; start < total; start += chunkSize) {
    multilistIterator iter = {0};

    if (multilistIteratorInitAtIdxForwardReadOnly(ml, s, &iter,
                                                  (mlOffsetId)start)) {
        multilistEntry entry;
        size_t processed = 0;

        while (processed < chunkSize && multilistNext(&iter, &entry)) {
            /* Process chunk */
            processed++;
        }

        multilistIteratorRelease(&iter);
    }
}
```

Note the loop condition order: test `processed < chunkSize` _before_ calling
`multilistNext()`, otherwise the last element of every chunk is consumed and
skipped.

## Best Practices

### 1. Choose Appropriate Size Limits

```c
/* Small, frequent updates - smaller nodes mean cheaper memmoves */
multilist *hotList = multilistNew(FLEX_CAP_LEVEL_512, 0);

/* Large, bulk operations - larger nodes amortize more overhead */
multilist *bulkList = multilistNew(FLEX_CAP_LEVEL_4096, 0);
```

### 2. Use Compression for Large Lists

```c
/* Compress interior nodes, keep the ends fast */
multilist *bigList = multilistNew(FLEX_CAP_LEVEL_4096, 2);
/* First 2 and last 2 nodes stay uncompressed once the list is Full */
```

### 3. Always Release Iterators

```c
multilistIterator iter = {0};
multilistIteratorInitForward(ml, s, &iter);

while (multilistNext(&iter, &entry)) {
    /* ... */
}

multilistIteratorRelease(&iter);  /* Required for Full; harmless otherwise */
```

### 4. Free Data from Pop Operations

```c
databox result = {{0}};
if (multilistPopHead(&ml, state, &result)) {
    /* Use result... */
    databoxFreeData(&result);  /* pop allocated a copy */
}
```

Do **not** call `databoxFreeData()` on a `multilistEntry.box` from
`multilistIndex()` or `multilistNext()` - those borrow list-owned memory.

### 5. Use Read-Only Iterators When Possible

```c
/* Read-only avoids marking Full nodes dirty for recompression */
multilistIteratorInitForwardReadOnly(ml, s, &iter);

/* Use read-write only when you will call multilistDelEntry() */
multilistIteratorInitForward(ml, s, &iter);
```

## Thread Safety

multilist is **not thread-safe**. For concurrent access, use external
synchronization, and remember that `mflexState` is scratch memory that must not
be shared between threads:

```c
pthread_rwlock_t lock = PTHREAD_RWLOCK_INITIALIZER;
multilist *shared = multilistNew(FLEX_CAP_LEVEL_2048, 0);

/* Reader (needs its own state) */
pthread_rwlock_rdlock(&lock);
const size_t count = multilistCount(shared);
pthread_rwlock_unlock(&lock);

/* Writer (needs its own state) */
mflexState *writerState = mflexStateCreate();
pthread_rwlock_wrlock(&lock);
databox item = databoxNewBytesString("data");
multilistPushByTypeTail(&shared, writerState, &item);
pthread_rwlock_unlock(&lock);
```

## See Also

- [VARIANTS.md](VARIANTS.md) - Detailed comparison of Small/Medium/Full variants
- [EXAMPLES.md](EXAMPLES.md) - Module-specific walkthroughs
- [databox](../core/DATABOX.md) - Universal container used for all values
- [flex](../flex/FLEX.md) - Underlying compact storage for multilist nodes
- [mflex](../flex/MFLEX.md) - Managed flex with compression support
- [Generic patterns](../../examples/PATTERNS.md) - Cross-module recipes

## Testing

Run the multilist test suites:

```bash
./datakit-test multilist       # alias: ml
./datakit-test multilistFull   # Full-variant-only suite
```

The `multilist` suite sweeps every compression depth 0-10 crossed with every
`flexOptimizationSizeLimit[]` level and covers:

- Small to Medium to Full automatic transitions
- Push/pop at head and tail, including popping past the end
- Forward and reverse iteration, and iteration starting at an index
- Insert before/after at the start, the middle, and on an empty list
- Delete ranges (positive, negative, and overflowing counts) and
  `multilistDelEntry()` during forward and reverse iteration
- Rotation, including rotating an empty list and 5000 rotations of 504 values
- Index access with positive and negative indexes, plus out-of-range checks
- Duplication of empty, single-element, and 500-element lists
- Replace at index
