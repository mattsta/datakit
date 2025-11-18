# multilist - Scale-Aware Linked List

## Overview

`multilist` is a **high-performance linked list** that stores multiple elements per node instead of one element per node. By packing multiple values into compact memory arrays (flex), multilist dramatically reduces pointer overhead and improves cache locality compared to traditional linked lists.

**Key Features:**
- Automatic scaling from 8 bytes (small) → 16 bytes (medium) → 24+ bytes (full)
- Multi-element nodes reduce pointer overhead by 8-32x
- O(1) push/pop at both ends (efficient queue/stack/deque)
- Forward and reverse iteration
- Random access by index with O(n) traversal
- Insert before/after any position
- Optional LZ4 compression (Full variant only)
- Transparent automatic upgrades between variants

**Headers**: `multilist.h`, `multilistCommon.h`

**Source**: `multilist.c`, `multilistSmall.c`, `multilistMedium.c`, `multilistFull.c`

## Architecture

The multilist family uses **pointer tagging** to transparently manage three underlying implementations:

```
┌─────────────┐
│  multilist* │  Opaque pointer with 2-bit type tag
└──────┬──────┘
       │
       ├──[tag=1]──> multilistSmall  (8 bytes, single flex)
       ├──[tag=2]──> multilistMedium (16 bytes, two flexes)
       └──[tag=3]──> multilistFull   (24+ bytes, array of flexes)
```

### What is a Multilist Node?

Traditional linked lists store one element per node:

```
Traditional Linked List:
[ptr|data|ptr] -> [ptr|data|ptr] -> [ptr|data|ptr] -> NULL
 16 bytes each     16 bytes each     16 bytes each
```

Multilist stores multiple elements per node in a compact flex array:

```
Multilist:
[ptr] -> [flex: elem elem elem elem elem] -> [flex: elem elem elem] -> NULL
          ~200 elements in one node           ~150 elements
```

**Benefits:**
- **Reduced Overhead**: One 8-byte pointer per 100-200 elements instead of per element
- **Cache Efficiency**: Elements packed together in memory
- **Compression**: Full variant can compress middle nodes with LZ4
- **Flexibility**: Nodes grow/shrink as needed

When you call `multilistPushHead()`, the implementation:
1. Checks the current variant via pointer tag
2. Routes to the appropriate function (Small/Medium/Full)
3. Automatically upgrades to the next variant if size threshold exceeded
4. Transparently updates your pointer to the new variant

**You never need to know which variant you're using** - the API is identical.

## Data Structures

### Common Types

```c
/* Node ID type - index into node array (Full variant) */
typedef int32_t mlNodeId;

/* Offset ID type - index into entire list across all nodes */
typedef int64_t mlOffsetId;

/* Iterator for traversing multilist */
typedef struct multilistIterator {
    void *ml;           /* Back pointer to multilist */
    flexEntry *fe;      /* Current position in current flex */
    int32_t offset;     /* Offset within current flex */
    mlNodeId nodeIdx;   /* Node index (Medium/Full only) */
    bool forward;       /* Direction of iteration */
    uint32_t type;      /* Variant type (for dispatching) */

    /* Full variant only */
    flex *f;            /* Current flex being iterated */
    mflexState *state[2];
    bool readOnly;
} multilistIterator;

/* Entry reference from index or iteration */
typedef struct multilistEntry {
    void *ml;           /* Back pointer to multilist */
    flexEntry *fe;      /* Position in flex */
    databox box;        /* The actual data value */
    mlNodeId nodeIdx;   /* Node index (Medium/Full only) */
    int32_t offset;     /* Offset within flex */
    flex *f;            /* Current flex (Full only) */
} multilistEntry;

/* State for compression operations (Full variant) */
typedef mflexState multilistState;
```

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

/* Example: Basic creation */
multilist *ml = multilistNew(FLEX_CAP_LEVEL_2048, 0);
/* Creates Small variant with 2KB node limit, no compression */

/* Example: With compression */
multilist *ml = multilistNew(FLEX_CAP_LEVEL_4096, 2);
/* Creates Small variant with 4KB nodes, compress middle nodes
 * (depth=2 means first 2 and last 2 nodes stay uncompressed) */

/* Example: From existing data */
flex *existingData = /* ... */;
multilist *ml = multilistNewFromFlex(FLEX_CAP_LEVEL_2048, 0, existingData);
```

### Size Limits

Available `flexCapSizeLimit` values control when nodes split:

```c
FLEX_CAP_LEVEL_128     /* Split at 128 bytes */
FLEX_CAP_LEVEL_256     /* Split at 256 bytes */
FLEX_CAP_LEVEL_512     /* Split at 512 bytes */
FLEX_CAP_LEVEL_1024    /* Split at 1KB */
FLEX_CAP_LEVEL_2048    /* Split at 2KB (default) */
FLEX_CAP_LEVEL_4096    /* Split at 4KB (recommended for large data) */
FLEX_CAP_LEVEL_8192    /* Split at 8KB */
```

**Compression Depth**: Only applies to Full variant. Depth N means the first N and last N nodes remain uncompressed (for fast head/tail access), while middle nodes are LZ4 compressed.

### Metadata Functions

```c
/* Get total element count */
size_t multilistCount(const multilist *m);

/* Get total bytes used */
size_t multilistBytes(const multilist *m);

/* Example */
printf("List contains %zu elements\n", multilistCount(ml));
printf("Using %zu bytes of memory\n", multilistBytes(ml));
```

## Insertion Operations

### Push to Ends

```c
/* Push to head (front) */
void multilistPushByTypeHead(multilist **m, mflexState *state,
                             const databox *box);

/* Push to tail (back) */
void multilistPushByTypeTail(multilist **m, mflexState *state,
                             const databox *box);

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

**Important**: Insertion takes `multilist **m` (pointer to pointer). This allows automatic upgrading between variants as the list grows.

### Insert at Position

```c
/* Insert before specific entry */
void multilistInsertByTypeBefore(multilist **m, mflexState *state[2],
                                 multilistEntry *node, const databox *box);

/* Insert after specific entry */
void multilistInsertByTypeAfter(multilist **m, mflexState *state[2],
                                multilistEntry *node, const databox *box);

/* Example: Insert in middle of list */
multilist *ml = multilistNew(FLEX_CAP_LEVEL_2048, 0);
mflexState *s[2] = {mflexStateCreate(), mflexStateCreate()};

/* Add some items */
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

## Deletion Operations

### Pop from Ends

```c
/* Pop from head or tail */
bool multilistPop(multilist **m, mflexState *state, databox *got,
                  bool fromTail);

/* Convenience macros */
#define multilistPopTail(ml, s, box) multilistPop(ml, s, box, true)
#define multilistPopHead(ml, s, box) multilistPop(ml, s, box, false)

/* Example: Queue pattern (FIFO) */
multilist *queue = multilistNew(FLEX_CAP_LEVEL_2048, 0);
mflexState *state = mflexStateCreate();

/* Enqueue items */
databox item1 = databoxNewBytesString("first");
multilistPushByTypeTail(&queue, state, &item1);

databox item2 = databoxNewBytesString("second");
multilistPushByTypeTail(&queue, state, &item2);

/* Dequeue items */
databox result;
if (multilistPopHead(&queue, state, &result)) {
    printf("Dequeued: %s\n", result.data.bytes.cstart);
    /* Prints: "first" */
    databoxFreeData(&result);
}

/* Example: Stack pattern (LIFO) */
multilist *stack = multilistNew(FLEX_CAP_LEVEL_2048, 0);

/* Push items */
databox val1 = databoxNewSigned(10);
multilistPushByTypeHead(&stack, state, &val1);

databox val2 = databoxNewSigned(20);
multilistPushByTypeHead(&stack, state, &val2);

/* Pop items */
databox result;
if (multilistPopHead(&stack, state, &result)) {
    printf("Popped: %ld\n", result.data.i);
    /* Prints: 20 (last in, first out) */
}
```

### Delete by Position

```c
/* Delete range of elements */
bool multilistDelRange(multilist **m, mflexState *state, mlOffsetId start,
                       int64_t values);

/* Delete specific entry during iteration */
void multilistDelEntry(multilistIterator *iter, multilistEntry *entry);

/* Example: Delete range */
multilist *ml = /* ... populated with 100 items ... */;
mflexState *state = mflexStateCreate();

/* Delete 10 elements starting at index 50 */
bool deleted = multilistDelRange(&ml, state, 50, 10);

/* Delete from end: negative indices work too */
multilistDelRange(&ml, state, -5, 5);  /* Delete last 5 elements */

/* Example: Delete during iteration */
multilistIterator iter;
multilistIteratorInitForward(ml, s, &iter);
multilistEntry entry;

while (multilistNext(&iter, &entry)) {
    /* Delete all entries containing "delete_me" */
    if (strstr(entry.box.data.bytes.cstart, "delete_me")) {
        multilistDelEntry(&iter, &entry);
        break;  /* Must exit iteration after delete */
    }
}
```

### Replace at Index

```c
/* Replace element at specific index */
bool multilistReplaceByTypeAtIndex(multilist **m, mflexState *state,
                                   mlNodeId index, const databox *box);

/* Example */
multilist *ml = /* ... populated ... */;
mflexState *state = mflexStateCreate();

/* Replace element at index 5 */
databox newValue = databoxNewBytesString("replaced");
bool replaced = multilistReplaceByTypeAtIndex(&ml, state, 5, &newValue);
```

## Access Operations

### Index Access

```c
/* Get element at specific index */
bool multilistIndex(const multilist *ml, mflexState *state, mlOffsetId index,
                    multilistEntry *entry, bool openNode);

/* Convenience macros */
#define multilistIndexGet(ml, s, i, e) multilistIndex(ml, s, i, e, true)
#define multilistIndexCheck(ml, s, i, e) multilistIndex(ml, s, i, e, false)

/* Example: Random access */
multilist *ml = /* ... populated ... */;
mflexState *state = mflexStateCreate();
multilistEntry entry;

/* Get first element (index 0) */
if (multilistIndexGet(ml, state, 0, &entry)) {
    printf("First: %s\n", entry.box.data.bytes.cstart);
}

/* Get last element (index -1) */
if (multilistIndexGet(ml, state, -1, &entry)) {
    printf("Last: %s\n", entry.box.data.bytes.cstart);
}

/* Get middle element */
size_t count = multilistCount(ml);
if (multilistIndexGet(ml, state, count / 2, &entry)) {
    printf("Middle: %s\n", entry.box.data.bytes.cstart);
}

/* Check if index exists without opening compressed node */
if (multilistIndexCheck(ml, state, 100, &entry)) {
    printf("Index 100 exists\n");
}
```

## Iteration

### Basic Iteration

```c
/* Initialize iterator */
void multilistIteratorInit(multilist *ml, mflexState *state[2],
                           multilistIterator *iter, bool forward,
                           bool readOnly);

/* Convenience macros */
#define multilistIteratorInitReadOnly(ml, s, iter, forward)
#define multilistIteratorInitForwardReadOnly(ml, s, iter)
#define multilistIteratorInitForward(ml, s, iter)
#define multilistIteratorInitReverse(ml, s, iter)
#define multilistIteratorInitReverseReadOnly(ml, s, iter)

/* Initialize at specific index */
bool multilistIteratorInitAtIdx(const multilist *ml, mflexState *state[2],
                                multilistIterator *iter, mlOffsetId idx,
                                bool forward, bool readOnly);

/* Advance iterator */
bool multilistNext(multilistIterator *iter, multilistEntry *entry);

/* Release iterator resources (Full variant only) */
void multilistIteratorRelease(multilistIterator *iter);

/* Example: Forward iteration */
multilist *ml = /* ... populated ... */;
mflexState *s[2] = {mflexStateCreate(), mflexStateCreate()};

multilistIterator iter;
multilistIteratorInitForwardReadOnly(ml, s, &iter);

multilistEntry entry;
while (multilistNext(&iter, &entry)) {
    printf("Element: %s\n", entry.box.data.bytes.cstart);
}

multilistIteratorRelease(&iter);

/* Example: Reverse iteration */
multilistIteratorInitReverseReadOnly(ml, s, &iter);

while (multilistNext(&iter, &entry)) {
    printf("Element (reverse): %s\n", entry.box.data.bytes.cstart);
}

multilistIteratorRelease(&iter);

/* Example: Start at specific index */
if (multilistIteratorInitAtIdxForwardReadOnly(ml, s, &iter, 50)) {
    while (multilistNext(&iter, &entry)) {
        printf("From index 50: %s\n", entry.box.data.bytes.cstart);
    }
}

multilistIteratorRelease(&iter);
```

### Rewind

```c
/* Reset iterator to head */
void multilistRewind(multilist *ml, multilistIterator *iter);

/* Reset iterator to tail */
void multilistRewindTail(multilist *ml, multilistIterator *iter);

/* Example: Multiple passes */
multilistIterator iter;
multilistIteratorInitForward(ml, s, &iter);

/* First pass */
while (multilistNext(&iter, &entry)) {
    /* Process... */
}

/* Second pass without creating new iterator */
multilistRewind(ml, &iter);
while (multilistNext(&iter, &entry)) {
    /* Process again... */
}
```

## Special Operations

### Rotate

```c
/* Move tail element to head */
void multilistRotate(multilist *m, mflexState *state[2]);

/* Example: Round-robin processing */
multilist *tasks = /* ... populated with tasks ... */;
mflexState *s[2] = {mflexStateCreate(), mflexStateCreate()};

for (int round = 0; round < 1000; round++) {
    multilistEntry entry;

    /* Get head task */
    if (multilistIndexGet(tasks, s[0], 0, &entry)) {
        printf("Processing: %s\n", entry.box.data.bytes.cstart);

        /* Move to back of queue */
        multilistRotate(tasks, s);
    }
}
```

## Performance Characteristics

### Time Complexity

| Operation | Small | Medium | Full |
|-----------|-------|--------|------|
| Push Head/Tail | O(1) - O(n)* | O(1) - O(n)* | O(1) amortized |
| Pop Head/Tail | O(1) | O(1) | O(1) |
| Index Access | O(n) | O(n) | O(n) |
| Insert | O(n) | O(n) | O(n) |
| Delete | O(n) | O(n) | O(n) |
| Iteration | O(n) | O(n) | O(n) |
| Rotate | O(1) | O(1) | O(1) |

\* O(1) if space available in current node, O(n) if node must be split or list must be upgraded to next variant.

### Space Complexity

| Variant | Fixed Overhead | Per Element | Max Recommended Size |
|---------|---------------|-------------|---------------------|
| Small | 8 bytes | ~varies~ | < 2KB total |
| Medium | 16 bytes | ~varies~ | 2KB - 6KB total |
| Full | 24+ bytes + N*20 | ~varies~ | Unlimited (TBs) |

### Automatic Upgrade Thresholds

```c
Small → Medium:  when bytes > limit
Medium → Full:   when bytes > limit * 3
```

### vs Traditional Linked List

**Traditional Linked List:**
- 16 bytes overhead per element (prev + next pointers on 64-bit)
- Poor cache locality
- No compression

**Multilist:**
- 8 bytes overhead per ~100-200 elements
- Excellent cache locality (elements packed together)
- Optional compression (Full variant)
- 8-32x less memory overhead

**Example:** Storing 10,000 integers:
- Traditional list: 10,000 * (8 bytes data + 16 bytes pointers) = 240 KB
- Multilist: 10,000 * 8 bytes + ~500 bytes overhead = 80.5 KB
- **Savings: 66% less memory**

## Common Patterns

### Pattern 1: Queue (FIFO)

```c
/* Push to tail, pop from head */
multilist *queue = multilistNew(FLEX_CAP_LEVEL_2048, 0);
mflexState *state = mflexStateCreate();

/* Enqueue */
databox item = databoxNewBytesString("task");
multilistPushByTypeTail(&queue, state, &item);

/* Dequeue */
databox result;
if (multilistPopHead(&queue, state, &result)) {
    /* Process result */
    databoxFreeData(&result);
}
```

### Pattern 2: Stack (LIFO)

```c
/* Push to head, pop from head */
multilist *stack = multilistNew(FLEX_CAP_LEVEL_2048, 0);
mflexState *state = mflexStateCreate();

/* Push */
databox item = databoxNewSigned(42);
multilistPushByTypeHead(&stack, state, &item);

/* Pop */
databox result;
if (multilistPopHead(&stack, state, &result)) {
    printf("Popped: %ld\n", result.data.i);
}
```

### Pattern 3: Deque (Double-Ended Queue)

```c
/* Push and pop from both ends */
multilist *deque = multilistNew(FLEX_CAP_LEVEL_2048, 0);
mflexState *state = mflexStateCreate();

/* Add to front */
databox front = databoxNewBytesString("front");
multilistPushByTypeHead(&deque, state, &front);

/* Add to back */
databox back = databoxNewBytesString("back");
multilistPushByTypeTail(&deque, state, &back);

/* Remove from front */
databox result;
multilistPopHead(&deque, state, &result);

/* Remove from back */
multilistPopTail(&deque, state, &result);
```

### Pattern 4: Circular Buffer

```c
/* Fixed size buffer with rotation */
multilist *buffer = multilistNew(FLEX_CAP_LEVEL_2048, 0);
mflexState *s[2] = {mflexStateCreate(), mflexStateCreate()};

const size_t maxSize = 100;

/* Add item */
databox item = databoxNewBytesString("new data");
multilistPushByTypeHead(&buffer, s[0], &item);

/* Maintain max size */
if (multilistCount(buffer) > maxSize) {
    databox old;
    multilistPopTail(&buffer, s[0], &old);
    databoxFreeData(&old);
}

/* Or use rotate for round-robin */
multilistRotate(buffer, s);
```

### Pattern 5: Range Processing

```c
/* Process elements in chunks */
multilist *ml = /* ... populated ... */;
mflexState *s[2] = {mflexStateCreate(), mflexStateCreate()};

const size_t chunkSize = 100;
size_t total = multilistCount(ml);

for (size_t start = 0; start < total; start += chunkSize) {
    multilistIterator iter;
    if (multilistIteratorInitAtIdxForward(ml, s, &iter, start)) {
        multilistEntry entry;
        size_t processed = 0;

        while (multilistNext(&iter, &entry) && processed < chunkSize) {
            /* Process chunk */
            processed++;
        }

        multilistIteratorRelease(&iter);
    }
}
```

## Best Practices

### 1. Choose Appropriate Size Limits

```c
/* Small, frequent updates - use smaller nodes */
multilist *hotList = multilistNew(FLEX_CAP_LEVEL_512, 0);

/* Large, bulk operations - use larger nodes */
multilist *bulkList = multilistNew(FLEX_CAP_LEVEL_4096, 0);
```

### 2. Use Compression for Large Lists

```c
/* Compress middle nodes, keep ends fast */
multilist *bigList = multilistNew(FLEX_CAP_LEVEL_4096, 2);
/* First 2 and last 2 nodes uncompressed */
```

### 3. Always Release Iterators

```c
multilistIterator iter;
multilistIteratorInitForward(ml, s, &iter);

while (multilistNext(&iter, &entry)) {
    /* ... */
}

multilistIteratorRelease(&iter);  /* Important for Full variant! */
```

### 4. Free Data from Pop Operations

```c
databox result;
if (multilistPopHead(&ml, state, &result)) {
    /* Use result... */
    databoxFreeData(&result);  /* Don't forget! */
}
```

### 5. Use Read-Only Iterators When Possible

```c
/* Read-only is more efficient */
multilistIteratorInitForwardReadOnly(ml, s, &iter);

/* Use read-write only when modifying */
multilistIteratorInitForward(ml, s, &iter);
```

## Thread Safety

multilist is **not thread-safe** by default. For concurrent access, use external synchronization:

```c
pthread_rwlock_t lock = PTHREAD_RWLOCK_INITIALIZER;
multilist *shared = multilistNew(FLEX_CAP_LEVEL_2048, 0);

/* Reader */
pthread_rwlock_rdlock(&lock);
size_t count = multilistCount(shared);
pthread_rwlock_unlock(&lock);

/* Writer */
pthread_rwlock_wrlock(&lock);
databox item = databoxNewBytesString("data");
multilistPushByTypeTail(&shared, state, &item);
pthread_rwlock_unlock(&lock);
```

## See Also

- [VARIANTS.md](VARIANTS.md) - Detailed comparison of Small/Medium/Full variants
- [EXAMPLES.md](EXAMPLES.md) - Real-world usage examples
- [databox](../core/DATABOX.md) - Universal container used for all values
- [flex](../flex/FLEX.md) - Underlying compact storage for multilist nodes
- [mflex](../flex/MFLEX.md) - Managed flex with compression support

## Testing

Run the multilist test suite:

```bash
./datakit-test multilist
```

The test suite includes:
- Small → Medium → Full automatic transitions
- Push/pop operations on all variants
- Forward and reverse iteration
- Insert before/after at various positions
- Delete ranges and individual entries
- Rotation operations
- Index access with positive and negative indices
- Compression behavior (Full variant)
- Performance benchmarks at different size limits
