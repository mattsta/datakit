# ptrPrevNext - Compact Doubly-Linked List Pointers

## Overview

`ptrPrevNext` provides **compact storage for doubly-linked list node pointers**
using variable-length integer encoding. Instead of storing three full 64-bit
words (24 bytes for atom index + prev + next), it uses 2-9 bytes per value
through `varintSplitFull16` encoding.

**Key Features:**

- Variable-length encoding, 2-9 bytes per value (6-27 bytes per entry)
- Stores atom index + previous offset + next offset
- Automatic memory management with gap filling
- All entries start on a 6-byte boundary
- Dynamic growth when space exhausted, sized through
  [jebuf](JEBUF.md)'s size classes
- In-place updates when possible, relocates when necessary
- Handles are integer offsets, so they survive pool reallocation

**Header**: `ptrPrevNext.h`

**Source**: `ptrPrevNext.c`

> **Handle 0 is a real, valid handle.** The first entry added always returns 0.
> Do not use 0 as a null/sentinel value for handles — track emptiness
> separately (a count, or a separate `hasHead` flag).

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
/* varintSplitFull16 has a TWO byte minimum:

   Values <= 16,383:        2 bytes each -> 6 bytes per entry
   Values <= 4,210,686:     3 bytes each -> 9 bytes per entry
   Values <= 1,077,952,509: 4 bytes each -> 12 bytes per entry
   ... up to 9 bytes each  -> 27 bytes per entry (full 64-bit values)
*/
```

For a million nodes whose values all fit in 14 bits, that is 6 MB instead of
24 MB.

## Data Structure

The public header exposes only an opaque type and the handle typedef:

```c
typedef struct ptrPrevNext ptrPrevNext;  /* opaque - defined in ptrPrevNext.c */
typedef size_t ptrPrevNextPtr;           /* handle, NOT a pointer or byte offset */
```

Internals, for understanding only — none of these fields are reachable from
outside `ptrPrevNext.c`:

```c
struct ptrPrevNext {
    memspace ms;  /* Internal memory space */
};

/* Internal memspace structure */
typedef struct memspace {
    void *mem;           /* Memory pool */
    size_t writeFromEnd; /* Offset to end of used space (the tail starts here) */
    size_t len;          /* Total pool length */
    uint8_t *freeBits;   /* One bit per 6-byte slot: set = free */
    ptrPrevNextFreeList free[6]; /* Free runs by size: 1..5 slots, 6+ slots */
} memspace;
```

### Memory Layout

```
┌────────────────────────────────────────────┐
│  Entry 0  │ [free] │  Entry 2  │  ...      │  Used space: entries and
├────────────────────────────────────────────┤  freed runs (tracked exactly)
│             Tail (all zero)                │  writeFromEnd → len
└────────────────────────────────────────────┘

Each entry starts on a 6-byte boundary:
┌──────────┬──────────┬──────────┐
│  atom    │   prev   │   next   │  Variable length (6-27 bytes)
│ (varint) │ (varint) │ (varint) │
└──────────┴──────────┴──────────┘
```

### Spatial Offset Alignment

All entries start at a multiple of 6 (`SPATIAL_OFFSET = 6`):

```c
#define SPATIAL_OFFSET 6

/* Byte offsets must be divisible by 6 */
Offset 0   → OK
Offset 6   → OK
Offset 12  → OK
Offset 7   → NOT ALLOWED (rounded up to 12)
```

**Why 6 bytes?** `varintSplitFull16` has a two-byte minimum, so the smallest
possible entry is exactly 3 × 2 = 6 bytes. Aligning to that lets a handle be
stored as `byteOffset / 6`, shrinking the handle itself.

## Varint Encoding

ptrPrevNext uses **varintSplitFull16** encoding, which is 2 to 9 bytes wide
(never 1):

```
Bytes  Max value                Note
2      16,383                   minimum width
3      4,210,686
4      1,077,952,509
5      5,372,919,804            (~5 G)
6      1,100,589,580,284        (~1 T)
7      281,476,054,663,164      (~281 T)
8      72,057,595,115,880,444   (~72 P)
9      18,446,744,073,709,551,615 (full uint64_t)
```

### Encoding Examples

```c
atomIndex = 100, prevOffset = 50, nextOffset = 200

Encoded as:
- atomIndex:  100 → 2 bytes (all values <= 16383 take 2 bytes)
- prevOffset:  50 → 2 bytes
- nextOffset: 200 → 2 bytes
Total: 6 bytes, exactly one aligned slot

Three raw 64-bit words would be 24 bytes.

Savings: 18 bytes (75%)

/* A worst case: three full-width values */
atomIndex = prevOffset = nextOffset = UINT64_MAX
- 9 + 9 + 9 = 27 bytes, rounded up to the next 6-byte boundary (30)
Still better than 24 bytes? No - this is the case where the encoding
costs more than raw words. Keep your indices small.
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
 * Returns: a handle, already divided down by 6.
 *          Multiply by 6 to get the raw byte offset; the API never
 *          asks you to do that, and you should not.
 */
size_t ptrPrevNextAdd(ptrPrevNext *ppn,
                      const size_t atomIndex,
                      const size_t prevOffset,
                      const size_t nextOffset);

/* Example: Building a linked list.
 * NOTE: the FIRST handle returned is 0, so a separate flag - not a
 * 0 handle - has to mark "no node". */
ptrPrevNext *ppn = ptrPrevNextNew();

/* Add first node (atom 0, no prev/next) */
ptrPrevNextPtr ptr0 = ptrPrevNextAdd(ppn, 0, 0, 0);  /* ptr0 == 0 */

/* Add second node (atom 1, prev=ptr0, no next) */
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
/* Handle 0 is a valid handle, so this list carries an explicit
 * `hasNext` bit per node instead of overloading 0 as "null". The bits
 * live outside ptrPrevNext, which only stores three integers. */
typedef struct compactList {
    ptrPrevNext *storage;
    ptrPrevNextPtr head;
    ptrPrevNextPtr tail;
    size_t length;          /* 0 means head/tail are meaningless */
    size_t nextAtomIndex;
} compactList;

compactList *compactListCreate(void) {
    compactList *list = malloc(sizeof(*list));
    list->storage = ptrPrevNextNew();
    list->head = 0;
    list->tail = 0;
    list->length = 0;
    list->nextAtomIndex = 0;
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
    /* Walk by COUNT, not by a 0 sentinel: handle 0 is the first real
     * entry, so `while (current != 0)` would stop before visiting it
     * (or loop forever once it wraps back to 0). */
    ptrPrevNextPtr current = list->head;

    for (size_t i = 0; i < list->length; i++) {
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

    /* ptrPrevNext approach.
     * The struct is opaque, so pool usage is not queryable from outside;
     * compute it from the entry width instead. Under DATAKIT_TEST,
     * ptrPrevNextRepr() prints the real internal counters. */
    ptrPrevNext *ppn = ptrPrevNextNew();

    for (size_t i = 0; i < nodeCount; i++) {
        ptrPrevNextAdd(ppn, i, i > 0 ? i - 1 : 0, 0);
    }

    /* All indices below fit in 14 bits here, so every entry is the
     * 6-byte minimum. */
    size_t compact = nodeCount * 6;
    printf("ptrPrevNext: %zu bytes (entries only)\n", compact);

    float savings = (1.0 - (float)compact / traditional) * 100;
    printf("Savings: %.1f%%\n", savings);

    ptrPrevNextFree(ppn);
}

/* Example output for 10,000 nodes:
   Traditional: 10000 nodes × 24 bytes = 240000 bytes
   ptrPrevNext: 60000 bytes (entries only)
   Savings: 75.0%
*/
```

### Example 4: Handling Updates and Relocation

```c
void demonstrateRelocation(void) {
    ptrPrevNext *ppn = ptrPrevNextNew();

    /* Add entry with small values (three 2-byte varints = 6 bytes) */
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

| Operation             | Complexity | Notes                                                        |
| --------------------- | ---------- | ------------------------------------------------------------ |
| Create                | O(1)       | Allocates 4096-byte pool                                     |
| Add (space available) | O(1)       | A listed free run that fits, else the tail                   |
| Add (need growth)     | O(n)       | Reallocates and copies pool                                  |
| Read                  | O(1)       | Direct varint decode                                         |
| Update (in-place)     | O(1)       | Overwrites its slots; slots it no longer reaches become free |
| Update (relocate)     | O(1)       | Frees its slots, adds again                                  |
| Release               | O(run)     | Frees its slots and merges them with free neighbours         |

### Memory Overhead

```
Per entry overhead:
- Alignment: Up to 5 bytes (to reach 6-byte boundary)
- Varint encoding: 0 bytes (compact encoding)
- Control structures: Shared across all entries

Total pool overhead:
- memspace struct and free lists (once)
- Free-slot bitmap: 1 bit per 6-byte slot (~2% of the pool)
- Alignment padding: up to 5 bytes per entry
```

### Space Savings

Per entry, all three values in the same width class:

```
Value Range           Traditional  ptrPrevNext  Savings
<= 16,383             24 bytes     6 bytes      75.0%
<= 4,210,686          24 bytes     9 bytes      62.5%
<= 1,077,952,509      24 bytes     12 bytes     50.0%
<= ~5 G               24 bytes     15 bytes     37.5%
<= ~1 T               24 bytes     18 bytes     25.0%
<= ~281 T             24 bytes     21 bytes     12.5%
<= ~72 P              24 bytes     24 bytes      0.0%
full uint64_t         24 bytes     27 bytes    -12.5% (worse)
```

The encoding only pays off while indices stay small. Above ~72 P per value it
costs more than three raw words.

## Memory Management

### Automatic Growth

```c
/* Pool starts at 4096 bytes, zero-filled */
ptrPrevNext *ppn = ptrPrevNextNew();

/* When an add cannot fit, the pool grows by 1.5x and the result is
 * then rounded up to a jemalloc size class via jebufSizeAllocation(),
 * so the real steps follow jebuf's table rather than exact 1.5x. */
for (int i = 0; i < 10000; i++) {
    ptrPrevNextAdd(ppn, i, i, i);
}
```

Growth uses `zrealloc`, so the pool's base address can move. Handles are
integer offsets and stay valid across growth; raw pointers into the pool do not.

### Gap Filling

When entries are released or relocated, gaps are created:

```c
ptrPrevNextPtr ptr1 = ptrPrevNextAdd(ppn, 1, 0, 0);  // handle 0, byte offset 0
ptrPrevNextPtr ptr2 = ptrPrevNextAdd(ppn, 2, 0, 0);  // handle 1, byte offset 6
ptrPrevNextPtr ptr3 = ptrPrevNextAdd(ppn, 3, 0, 0);  // handle 2, byte offset 12

ptrPrevNextRelease(ppn, ptr2);  // Creates gap at offset 6

ptrPrevNextPtr ptr4 = ptrPrevNextAdd(ppn, 4, 0, 0);
// Fills gap at offset 6 if new entry fits!
```

Free space is tracked exactly, never guessed from zero bytes (an entry of
three zeros is itself all zero bytes):

- A bitmap marks every free 6-byte slot below the tail.
- Releasing an entry (or relocating it, or shrinking it in place) frees its
  slots and merges them with any free neighbours into one run. A run that
  reaches the tail returns to the tail.
- Runs are listed by size (1 to 5 slots exactly, and one list for longer
  runs). `Add` takes the smallest listed run that fits and lists the rest of
  a longer run again. List entries whose run has since merged or been used
  are recognised through the bitmap and dropped.

So every freed byte is reusable: after releasing everything, the pool's
used space returns to empty, and a pool that is filled and emptied
repeatedly stops growing after the first round (the `churn` test checks
this).

## Best Practices

### 1. Use Appropriate Index Sizes

```c
/* GOOD - keep indices small when possible */
size_t atomIndex = 100;  // 2 bytes (the minimum width)

/* BAD - unnecessarily large indices */
size_t atomIndex = (size_t)-1;  // 9 bytes, and forces a wide entry
```

There is no width below 2 bytes, so shrinking a value from 100 to 5 saves
nothing. What matters is staying under each width threshold: 16,383 then
4,210,686 then 1,077,952,509.

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
/* WRONG - a handle is not a pointer, and not even a byte offset */
ptrPrevNextPtr off = ptrPrevNextAdd(ppn, 1, 0, 0);
size_t *ptr = (size_t *)off; // CRASH!

/* RIGHT - read through API */
size_t atom, prev, next;
ptrPrevNextRead(ppn, off, &atom, &prev, &next);
```

### 1b. Treating Handle 0 as Null

```c
/* WRONG - 0 is the first entry's handle, not "no entry" */
ptrPrevNextPtr head = ptrPrevNextAdd(ppn, 1, 0, 0);  /* head == 0 */
if (head) {                    /* false! */
    ptrPrevNextRead(ppn, head, ...);
}

/* RIGHT - track presence separately */
bool haveHead = false;
ptrPrevNextPtr head = ptrPrevNextAdd(ppn, 1, 0, 0);
haveHead = true;
if (haveHead) {
    ptrPrevNextRead(ppn, head, ...);
}
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

Handles stay valid across pool growth — they are integer offsets, not addresses.
Raw pointers into the pool do not, because growth goes through `zrealloc`. The
struct is opaque precisely so you cannot reach the base pointer and cache it;
always go back through `ptrPrevNextRead` after any `Add` or `Update`.

```c
/* This is why there is no accessor for the pool base:
 * add/update may zrealloc, moving every byte in the pool. */
size_t atom, prev, next;
ptrPrevNextRead(ppn, handle, &atom, &prev, &next);  /* re-read, don't cache */
```

## Debugging

```c
#ifdef DATAKIT_TEST
/* Print internal structure (debug builds only) */
void ptrPrevNextRepr(ptrPrevNext *ppn);

/* Usage */
ptrPrevNextRepr(ppn);
/* Output:
{meta {writeFromEnd 54} {len 4096} {bytesFree 4042}}
{offset { 0    0    0} {atomIndex {bytes 2} 1} ...}
[FREE 6]
{offset { 1   12    2} {atomIndex {bytes 2} 3} ...}
...
(free runs are read from the free-slot bitmap, so an all-zero entry still
prints as an entry)
*/
#endif
```

## See Also

- [list](../utils/LIST.md) - Traditional doubly-linked list (for comparison)
- [offsetArray](OFFSET_ARRAY.md) - Sparse array storage
- [jebuf](JEBUF.md) - size classes used when the pool grows
- [Varint Encoding](https://github.com/mattsta/varint) - Variable-length integer encoding

## Testing

Run the ptrPrevNext test suite:

```bash
./src/datakit-test test ptrPrevNext   # alias: ppn
```

The test suite validates:

- Basic add/read/update/release operations
- Sequential value insertion (4M+ entries)
- Random value insertion and updates
- Memory growth and gap filling
- Offset relocation handling
- A fixed-seed soak that re-reads every live entry after every add, update
  and release, over values of every encoding width
- A churn test: fill, update, release everything, repeat; every entry reads
  back and the pool never grows past its first-round size
