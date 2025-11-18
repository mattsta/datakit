# ptrPrevNext - Compact Doubly-Linked List Pointers

## Overview

`ptrPrevNext` provides **compact storage for doubly-linked list node pointers** using variable-length integer encoding. Instead of storing full 64-bit pointers (24 bytes total for atom index + prev + next), it uses 1-9 bytes per value through varint encoding.

**Key Features:**
- Variable-length encoding (1-9 bytes per value)
- Stores atom index + previous offset + next offset
- Automatic memory management with gap filling
- 6-byte alignment for spatial locality
- Dynamic growth when space exhausted
- In-place updates when possible, relocates when necessary

**Header**: `ptrPrevNext.h`

**Source**: `ptrPrevNext.c`

## What is Compact Pointer Storage?

Traditional doubly-linked lists store full pointers:

```c
typedef struct node {
    void *data;
    struct node *prev;  // 8 bytes on 64-bit
    struct node *next;  // 8 bytes on 64-bit
} node;

// Per node: 24 bytes (8 + 8 + 8)
```

For 1 million nodes: **24 MB** just for pointers!

ptrPrevNext uses **varint encoding** for compact storage:

```c
/* For small indices (< 128): 1 byte each
   Total: 3 bytes (atom + prev + next)

   For medium indices (< 16,384): 2 bytes each
   Total: 6 bytes

   For large indices (< 2,097,152): 3 bytes each
   Total: 9 bytes
*/
```

For 1 million nodes with typical values: **~3-6 MB** savings!

## Data Structure

```c
typedef struct ptrPrevNext {
    memspace ms;  /* Internal memory space */
} ptrPrevNext;

typedef size_t ptrPrevNextPtr;  /* Offset handle (not actual pointer) */

/* Internal memspace structure */
typedef struct memspace {
    void *mem;           /* Memory pool */
    size_t bestFree;     /* Offset to best free space */
    int64_t bestFreeLen; /* Length of best free space */
    size_t writeFromEnd; /* Offset to end of used space */
    size_t len;          /* Total pool length */
} memspace;
```

### Memory Layout

```
┌────────────────────────────────────────────┐
│  Entry 0  │  Entry 1  │  Entry 2  │  ...   │  Used Space
├────────────────────────────────────────────┤
│          [Free Space / Gaps]               │
├────────────────────────────────────────────┤
│             Unused Memory                  │  writeFromEnd → len
└────────────────────────────────────────────┘

Each entry (6-byte aligned):
┌──────────┬──────────┬──────────┐
│  atom    │   prev   │   next   │  Variable length (3-27 bytes)
│ (varint) │ (varint) │ (varint) │
└──────────┴──────────┴──────────┘
```

### Spatial Offset Alignment

All entries are **6-byte aligned** (`SPATIAL_OFFSET = 6`):

```c
#define SPATIAL_OFFSET 6

/* Offsets must be divisible by 6 */
Offset 0   → OK
Offset 6   → OK
Offset 12  → OK
Offset 7   → NOT ALLOWED (aligned to 12)
```

**Why 6 bytes?**
- Minimum entry size is 3 bytes (3 × 1-byte varints)
- Allows 2 minimum entries per alignment
- Good trade-off between alignment overhead and flexibility

## Varint Encoding

ptrPrevNext uses **varintSplitFull16** encoding:

```c
Value Range          Bytes   Encoding
0 - 127             1       0xxxxxxx
128 - 16,383        2       10xxxxxx xxxxxxxx
16,384 - 2,097,151  3       110xxxxx xxxxxxxx xxxxxxxx
...
Up to 16 bytes for very large values
```

### Encoding Examples

```c
atomIndex = 100, prevOffset = 50, nextOffset = 200

Encoded as:
- atomIndex: 100 → 1 byte [0x64]
- prevOffset: 50 → 1 byte [0x32]
- nextOffset: 200 → 2 bytes [0x80, 0xC8]
Total: 4 bytes

If indices were full pointers:
- atomIndex: 8 bytes
- prevOffset: 8 bytes
- nextOffset: 8 bytes
Total: 24 bytes

Savings: 20 bytes (83%)!
```

## API Reference

### Creation and Destruction

```c
/* Create new ptrPrevNext storage
 * Returns: ptrPrevNext pointer or NULL on error
 * Initial size: 4096 bytes
 */
ptrPrevNext *ptrPrevNextNew(void);

/* Free ptrPrevNext storage
 * ppn: ptrPrevNext to free (can be NULL)
 */
void ptrPrevNextFree(ptrPrevNext *ppn);

/* Example */
ptrPrevNext *ppn = ptrPrevNextNew();
// ... use it ...
ptrPrevNextFree(ppn);
```

### Adding Entries

```c
/* Add new entry with atom index and prev/next offsets
 * ppn: ptrPrevNext storage
 * atomIndex: index of the atom/node
 * prevOffset: offset to previous node
 * nextOffset: offset to next node
 * Returns: memory offset handle (divide by 6 for storage offset)
 */
size_t ptrPrevNextAdd(ptrPrevNext *ppn,
                      const size_t atomIndex,
                      const size_t prevOffset,
                      const size_t nextOffset);

/* Example: Building a linked list */
ptrPrevNext *ppn = ptrPrevNextNew();

/* Add first node (atom 0, no prev/next) */
ptrPrevNextPtr ptr0 = ptrPrevNextAdd(ppn, 0, 0, 0);

/* Add second node (atom 1, prev=0, no next) */
ptrPrevNextPtr ptr1 = ptrPrevNextAdd(ppn, 1, ptr0, 0);

/* Add third node (atom 2, prev=ptr1, no next) */
ptrPrevNextPtr ptr2 = ptrPrevNextAdd(ppn, 2, ptr1, 0);

/* Now update ptr1 to point to ptr2 as next */
ptr1 = ptrPrevNextUpdate(ppn, ptr1, ptr0, ptr2);
```

### Reading Entries

```c
/* Read entry values
 * ppn: ptrPrevNext storage
 * memOffset: handle returned by Add/Update
 * atomRef: output pointer for atom index
 * prev: output pointer for previous offset
 * next: output pointer for next offset
 */
void ptrPrevNextRead(const ptrPrevNext *ppn,
                     const ptrPrevNextPtr memOffset,
                     size_t *atomRef,
                     size_t *prev,
                     size_t *next);

/* Example */
size_t atomIndex, prevOff, nextOff;
ptrPrevNextRead(ppn, ptr1, &atomIndex, &prevOff, &nextOff);
printf("Atom %zu: prev=%zu next=%zu\n", atomIndex, prevOff, nextOff);
```

### Updating Entries

```c
/* Update existing entry's prev/next offsets
 * ppn: ptrPrevNext storage
 * memOffset: handle to update
 * prevOffset: new previous offset
 * nextOffset: new next offset
 * Returns: new memory offset (may be same or different)
 *
 * Note: May return different offset if entry needs more space
 */
ptrPrevNextPtr ptrPrevNextUpdate(ptrPrevNext *ppn,
                                 const ptrPrevNextPtr memOffset,
                                 const size_t prevOffset,
                                 const size_t nextOffset);

/* Example: Update in place */
ptrPrevNextPtr updated = ptrPrevNextUpdate(ppn, ptr1, newPrev, newNext);
if (updated != ptr1) {
    printf("Entry relocated to new offset!\n");
    ptr1 = updated; // Use new offset
}
```

**Important**: Update may change the offset if the new values need more space!

### Releasing Entries

```c
/* Release (delete) an entry and mark space as free
 * ppn: ptrPrevNext storage
 * memOffset: handle to release
 */
void ptrPrevNextRelease(ptrPrevNext *ppn, const ptrPrevNextPtr memOffset);

/* Example */
ptrPrevNextRelease(ppn, ptr1);
// ptr1's space is now free and can be reused
```

## Real-World Examples

### Example 1: Doubly-Linked List Implementation

```c
typedef struct compactList {
    ptrPrevNext *storage;
    ptrPrevNextPtr head;
    ptrPrevNextPtr tail;
    size_t length;
    size_t nextAtomIndex;
} compactList;

compactList *compactListCreate(void) {
    compactList *list = malloc(sizeof(*list));
    list->storage = ptrPrevNextNew();
    list->head = 0;
    list->tail = 0;
    list->length = 0;
    list->nextAtomIndex = 1; // Start from 1 (0 means null)
    return list;
}

void compactListAppend(compactList *list, void *data) {
    size_t atomIndex = list->nextAtomIndex++;
    ptrPrevNextPtr newNode;

    if (list->length == 0) {
        /* First node */
        newNode = ptrPrevNextAdd(list->storage, atomIndex, 0, 0);
        list->head = newNode;
        list->tail = newNode;
    } else {
        /* Append to tail */
        newNode = ptrPrevNextAdd(list->storage, atomIndex, list->tail, 0);

        /* Update old tail to point to new node */
        size_t oldAtom, oldPrev, oldNext;
        ptrPrevNextRead(list->storage, list->tail,
                       &oldAtom, &oldPrev, &oldNext);
        ptrPrevNextUpdate(list->storage, list->tail, oldPrev, newNode);

        list->tail = newNode;
    }

    list->length++;
}

void compactListIterate(compactList *list) {
    ptrPrevNextPtr current = list->head;

    while (current != 0) {
        size_t atom, prev, next;
        ptrPrevNextRead(list->storage, current, &atom, &prev, &next);

        printf("Node: atom=%zu prev=%zu next=%zu\n", atom, prev, next);

        current = next;
    }
}

/* Usage */
compactList *list = compactListCreate();
for (int i = 0; i < 1000; i++) {
    compactListAppend(list, (void *)(intptr_t)i);
}
compactListIterate(list);
```

### Example 2: LRU Cache Node Storage

```c
typedef struct lruCache {
    ptrPrevNext *nodeStorage;  /* Store prev/next pointers */
    void *data[10000];         /* Actual cached data */
    ptrPrevNextPtr head;       /* Most recently used */
    ptrPrevNextPtr tail;       /* Least recently used */
} lruCache;

void lruCacheAccess(lruCache *cache, size_t index) {
    ptrPrevNextPtr node = /* find node for index */;

    /* Move to head (most recent) */
    size_t atom, prev, next;
    ptrPrevNextRead(cache->nodeStorage, node, &atom, &prev, &next);

    if (node == cache->head) {
        return; // Already most recent
    }

    /* Remove from current position */
    if (prev != 0) {
        size_t prevAtom, prevPrev, prevNext;
        ptrPrevNextRead(cache->nodeStorage, prev,
                       &prevAtom, &prevPrev, &prevNext);
        ptrPrevNextUpdate(cache->nodeStorage, prev, prevPrev, next);
    }

    if (next != 0) {
        size_t nextAtom, nextPrev, nextNext;
        ptrPrevNextRead(cache->nodeStorage, next,
                       &nextAtom, &nextPrev, &nextNext);
        ptrPrevNextUpdate(cache->nodeStorage, next, prev, nextNext);
    }

    /* Insert at head */
    if (cache->head != 0) {
        size_t headAtom, headPrev, headNext;
        ptrPrevNextRead(cache->nodeStorage, cache->head,
                       &headAtom, &headPrev, &headNext);
        ptrPrevNextUpdate(cache->nodeStorage, cache->head, node, headNext);
    }

    ptrPrevNextUpdate(cache->nodeStorage, node, 0, cache->head);
    cache->head = node;
}
```

### Example 3: Memory Usage Comparison

```c
void compareMemoryUsage(size_t nodeCount) {
    /* Traditional approach */
    typedef struct traditionalNode {
        size_t atomIndex;
        struct traditionalNode *prev;
        struct traditionalNode *next;
    } traditionalNode;

    size_t traditional = nodeCount * sizeof(traditionalNode);
    printf("Traditional: %zu nodes × 24 bytes = %zu bytes\n",
           nodeCount, traditional);

    /* ptrPrevNext approach */
    ptrPrevNext *ppn = ptrPrevNextNew();

    for (size_t i = 0; i < nodeCount; i++) {
        ptrPrevNextAdd(ppn, i, i > 0 ? i - 1 : 0, 0);
    }

    size_t compact = ppn->ms.writeFromEnd;
    printf("ptrPrevNext: %zu bytes (actual used)\n", compact);

    float savings = (1.0 - (float)compact / traditional) * 100;
    printf("Savings: %.1f%%\n", savings);

    ptrPrevNextFree(ppn);
}

/* Example output for 10,000 nodes:
   Traditional: 10000 nodes × 24 bytes = 240000 bytes
   ptrPrevNext: 60000 bytes (actual used)
   Savings: 75.0%
*/
```

### Example 4: Handling Updates and Relocation

```c
void demonstrateRelocation(void) {
    ptrPrevNext *ppn = ptrPrevNextNew();

    /* Add entry with small values (uses ~3 bytes) */
    ptrPrevNextPtr ptr = ptrPrevNextAdd(ppn, 10, 5, 15);
    printf("Initial offset: %zu\n", ptr);

    /* Update with values that fit in same space */
    ptrPrevNextPtr ptr2 = ptrPrevNextUpdate(ppn, ptr, 6, 16);
    if (ptr2 == ptr) {
        printf("Updated in place (same offset)\n");
    }

    /* Update with LARGE values (need more space) */
    ptrPrevNextPtr ptr3 = ptrPrevNextUpdate(ppn, ptr2,
                                            1000000,  /* Needs 3 bytes */
                                            2000000); /* Needs 3 bytes */
    if (ptr3 != ptr2) {
        printf("Relocated to new offset: %zu → %zu\n", ptr2, ptr3);
        printf("Old space marked as free and will be reused\n");
    }

    ptrPrevNextFree(ppn);
}
```

## Performance Characteristics

| Operation | Complexity | Notes |
|-----------|-----------|-------|
| Create | O(1) | Allocates 4096-byte pool |
| Add (space available) | O(1) | Append to writeFromEnd |
| Add (need growth) | O(n) | Reallocates and copies pool |
| Read | O(1) | Direct varint decode |
| Update (in-place) | O(1) | Overwrites existing space |
| Update (relocate) | O(1) | Adds to end, marks old as free |
| Release | O(1) | Marks space free |

### Memory Overhead

```
Per entry overhead:
- Alignment: Up to 5 bytes (to reach 6-byte boundary)
- Varint encoding: 0 bytes (compact encoding)
- Control structures: Shared across all entries

Total pool overhead:
- memspace struct: 40 bytes (once)
- Alignment gaps: ~10-20% of used space
```

### Space Savings

```
Value Range       Traditional  ptrPrevNext  Savings
0-127            24 bytes     3 bytes      87.5%
128-16,383       24 bytes     6 bytes      75.0%
16,384-2M        24 bytes     9 bytes      62.5%
2M-268M          24 bytes     12 bytes     50.0%
```

## Memory Management

### Automatic Growth

```c
/* Pool starts at 4096 bytes */
ptrPrevNext *ppn = ptrPrevNextNew();

/* When full, automatically grows by 1.5× */
for (int i = 0; i < 10000; i++) {
    ptrPrevNextAdd(ppn, i, i, i);
    // Grows: 4096 → 6144 → 9216 → 13824 → ...
}
```

### Gap Filling

When entries are released or relocated, gaps are created:

```c
ptrPrevNextPtr ptr1 = ptrPrevNextAdd(ppn, 1, 0, 0);  // Offset 0
ptrPrevNextPtr ptr2 = ptrPrevNextAdd(ppn, 2, 0, 0);  // Offset 6
ptrPrevNextPtr ptr3 = ptrPrevNextAdd(ppn, 3, 0, 0);  // Offset 12

ptrPrevNextRelease(ppn, ptr2);  // Creates gap at offset 6

ptrPrevNextPtr ptr4 = ptrPrevNextAdd(ppn, 4, 0, 0);
// Fills gap at offset 6 if new entry fits!
```

The allocator tracks `bestFree` and `bestFreeLen` to reuse gaps.

## Best Practices

### 1. Use Appropriate Index Sizes

```c
/* GOOD - keep indices small when possible */
size_t atomIndex = 100;  // Uses 1 byte in varint

/* BAD - unnecessarily large indices */
size_t atomIndex = (size_t)-1;  // Uses 9 bytes in varint!
```

### 2. Handle Relocation

```c
/* GOOD - check if offset changed */
ptrPrevNextPtr ptr = ptrPrevNextAdd(ppn, ...);
ptr = ptrPrevNextUpdate(ppn, ptr, newPrev, newNext);
// Always use returned value

/* BAD - assume offset stays same */
ptrPrevNextPtr ptr = ptrPrevNextAdd(ppn, ...);
ptrPrevNextUpdate(ppn, ptr, newPrev, newNext);
// ptr may now be invalid!
```

### 3. Release Unused Entries

```c
/* GOOD - release when done */
ptrPrevNextRelease(ppn, ptr);

/* BAD - never release (memory leak within pool) */
// Pool grows forever, gaps never filled
```

### 4. Pre-size for Large Collections

```c
/* If you know you'll have many entries, consider
   starting with a larger initial pool (modify source)
   or accept the growth overhead */
```

## Common Pitfalls

### 1. Using Offset as Pointer

```c
/* WRONG - offset is not a pointer! */
ptrPrevNextPtr off = ptrPrevNextAdd(ppn, 1, 0, 0);
size_t *ptr = (size_t *)off; // CRASH!

/* RIGHT - read through API */
size_t atom, prev, next;
ptrPrevNextRead(ppn, off, &atom, &prev, &next);
```

### 2. Not Updating After Relocation

```c
/* WRONG - may use stale offset */
ptrPrevNextPtr ptr = ptrPrevNextAdd(ppn, 1, 0, 0);
ptrPrevNextUpdate(ppn, ptr, 2, 3); // May relocate
ptrPrevNextRead(ppn, ptr, ...); // ptr might be invalid!

/* RIGHT - use returned offset */
ptrPrevNextPtr ptr = ptrPrevNextAdd(ppn, 1, 0, 0);
ptr = ptrPrevNextUpdate(ppn, ptr, 2, 3);
ptrPrevNextRead(ppn, ptr, ...); // Safe
```

### 3. Expecting Stable Offsets

```c
/* Pool may grow and relocate memory */
void *rawPtr = ppn->ms.mem; // DON'T STORE THIS
// ... add more entries ...
// rawPtr is now invalid if pool grew!
```

## Debugging

```c
#ifdef DATAKIT_TEST
/* Print internal structure (debug builds only) */
void ptrPrevNextRepr(ptrPrevNext *ppn);

/* Usage */
ptrPrevNextRepr(ppn);
/* Output:
{meta {bestFree 0} {bestFreeLen 0}
      {writeFromEnd 54} {len 4096}
      {bytesFree 4042}}
{offset {0 0 0} {atomIndex {bytes 1} 1} ...}
{offset {1 6 1} {atomIndex {bytes 1} 2} ...}
...
*/
#endif
```

## See Also

- [list](../../utils/LIST.md) - Traditional doubly-linked list (for comparison)
- [offsetArray](OFFSET_ARRAY.md) - Sparse array storage
- [Varint Encoding](https://github.com/mattsta/varint) - Variable-length integer encoding

## Testing

Run the ptrPrevNext test suite:

```bash
./src/datakit-test test ptrPrevNext
```

The test suite validates:
- Basic add/read/update/release operations
- Sequential value insertion (4M+ entries)
- Random value insertion and updates
- Memory growth and gap filling
- Offset relocation handling
