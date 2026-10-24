# multiarray - Scale-Aware Dynamic Array

## Overview

`multiarray` is a **scale-aware dynamic array** that transitions between internal
representations as the element count grows. A `multiarray *` is a _tagged
pointer_: the low two bits of the pointer carry the current representation, so
the same variable can point at a raw C array today and a chunked container after
the next insert.

**Key features:**

- One pointer variable, four representations (Native, Small, Medium, Large)
- Automatic promotion on insert; the variable is rewritten in place
- Contiguous storage for small counts, chunked storage for large counts
- Arbitrary element widths (`len` is a per-container byte width)
- Macro "Native" API that operates on a plain typed array with no container
  struct at all

**Headers**: `multiarray.h`, `multiarraySmall.h`, `multiarrayMedium.h`,
`multiarrayLarge.h`

**Internal headers** (needed only to touch struct fields):
`multiarraySmallInternal.h`, `multiarrayMediumInternal.h`,
`multiarrayLargeInternal.h`, `multiarrayMediumLarge.h`

**Source**: `multiarray.c`, `multiarraySmall.c`, `multiarrayMedium.c`,
`multiarrayLarge.c`

`multiarray.h` already pulls in `multiarrayMediumInternal.h` (which pulls in
`multiarraySmallInternal.h`), so the `multiarraySmall` and `multiarrayMedium`
struct layouts are visible from `multiarray.h` alone. `multiarrayLarge`'s
layout is **not**; include `multiarrayLargeInternal.h` explicitly if you need
`head`/`tail`/`prevNext`.

## Architecture

```
┌──────────────┐
│ multiarray * │  tagged pointer: low 2 bits = multiarrayType
└──────┬───────┘
       │
       ├──[tag=0]──> Native   raw typed array, caller tracks the count
       ├──[tag=1]──> Small    16-byte struct + one contiguous array
       ├──[tag=2]──> Medium   16-byte struct + array of 16-byte nodes
       └──[tag=3]──> Large    24-byte struct + XOR linked list of 16-byte nodes
```

The tag helpers live in `multiarray.h`:

```c
#define _MULTIARRAY_TAGGED_PTR_MASK (uintptr_t)(0x03)
#define _multiarrayType(map)                                                   \
    (multiarrayType)((uintptr_t)(map) & _MULTIARRAY_TAGGED_PTR_MASK)
#define _MULTIARRAY_USE(map)                                                   \
    ((void *)((uintptr_t)(map) & ~(_MULTIARRAY_TAGGED_PTR_MASK)))
#define _MULTIARRAY_TAG(map, type) ((multiarray *)((uintptr_t)(map) | (type)))

#define mars(m) ((multiarraySmall *)_MULTIARRAY_USE(m))
#define marm(m) ((multiarrayMedium *)_MULTIARRAY_USE(m))
#define marl(m) ((multiarrayLarge *)_MULTIARRAY_USE(m))
```

There are **two entry points with different promotion ladders**:

| Entry point                     | Starts as | Ladder                    |
| ------------------------------- | --------- | ------------------------- |
| `multiarrayNew()` (container)   | Small     | Small -> Medium -> Large  |
| `multiarrayNativeNew()` (macro) | Native    | Native -> Medium -> Large |

The Native macro ladder skips Small entirely: a full Native array is handed
directly to `multiarrayMediumNewWithData()` as node 0. The container ladder
never uses Native: given a native multiarray, `multiarrayGet()` and the
other container calls refuse with a defined answer (0 / NULL, no change,
an iterator that yields nothing) and report a `DK_DIAG_MULTIARRAY_NATIVE`
misuse diagnostic ([DIAGNOSTICS.md](../core/DIAGNOSTICS.md)).  NULL is
refused the same way without a report.

There is no automatic _demotion_. `multiarrayDelete()` shrinks nodes but never
converts Large back to Medium or Medium back to Small (there is an explicit
`TODO` about this in `multiarray.c`).

## Data Structures

### multiarray type and tag enum

```c
typedef enum multiarrayType {
    MULTIARRAY_TYPE_NATIVE = 0, /* 8 bytes, fixed. */
    MULTIARRAY_TYPE_SMALL = 1,  /* 16 + 8 bytes, fixed. */
    MULTIARRAY_TYPE_MEDIUM = 2, /* 16 + (16 * N) bytes pointer array. */
    MULTIARRAY_TYPE_LARGE = 3   /* 24 + (16 * N) bytes pointer linked list.  */
} multiarrayType;

/* opaque multiarray type; there's no user accessible data here. */
typedef struct multiarray multiarray;
typedef uint32_t multiarrayIdx;
```

### multiarraySmall (`multiarraySmallInternal.h`)

```c
struct multiarraySmall {
    uint8_t *data;
    uint16_t len;    /* width of each entry in 'data' */
    uint16_t count;  /* number of entries (i.e. size of data is len * count) */
    uint16_t rowMax; /* not used here, but used for upgrading. */
    /* 2 bytes padding */
};
```

16 bytes total. Field order is `data`, `len`, `count`, `rowMax` — `len`
precedes `count`. `count` and `rowMax` are `uint16_t`, so a single Small holds
at most 65535 elements.

### multiarrayMedium (`multiarrayMediumInternal.h`)

```c
typedef multiarraySmall multiarrayMediumNode;

struct multiarrayMedium {
    multiarrayMediumNode *node;
    uint32_t count;  /* number of 'node' (i.e. node is size: len * count) */
    uint16_t len;    /* width of individual entries in node->data */
    uint16_t rowMax; /* maximum entires inside each 'node' */
};
```

16 bytes plus a flat array of `count` nodes. A node **is** a `multiarraySmall`
(the typedef reuses it for its `data`/`count` pair); a node's own `len` and
`rowMax` fields are unused, so each node costs 16 bytes. `multiarrayMedium.count`
is the **node** count, not the element count.

### multiarrayLarge (`multiarrayLargeInternal.h`)

```c
typedef struct multiarrayLargeNode {
    uint8_t *data;
    uint64_t prevNext : 48; /* xor of addrs is likely much less than 48 bits */
    uint64_t count : 16;    /* we should only have max 32k entries per node. */
} multiarrayLargeNode;

struct multiarrayLarge {
    multiarrayLargeNode *head;
    multiarrayLargeNode *tail;
    uint16_t len;    /* width of individual node->data entires */
    uint16_t rowMax; /* maximum entries per node before creating new node */
    /* 4 bytes padding */
};
```

The container is 24 bytes; each node is **16 bytes**, not 24. `prevNext` is a
48-bit bitfield holding `prev XOR next`, packed into the same 8-byte word as the
16-bit per-node `count`. That packing is what makes a Large node the same size
as a Medium node while still being doubly linked.

## Container API

Declared in `multiarray.h`. This is the complete public container surface:

```c
multiarray *multiarrayNew(uint16_t len, uint16_t rowMax);
void multiarrayFree(multiarray *m);

void *multiarrayGet(multiarray *m, multiarrayIdx idx);
void *multiarrayGetHead(multiarray *m);
void *multiarrayGetTail(multiarray *m);
void multiarrayInsert(multiarray **m, multiarrayIdx idx, void *what);
void multiarrayDelete(multiarray **m, const uint32_t index);

/* Element count and data-storage bytes (metadata overhead excluded).
 * Container API only — the Native macro API tracks count externally.
 * Count is O(1) on Small, O(nodes) on Medium/Large. */
size_t multiarrayCount(const multiarray *m);
size_t multiarrayBytes(const multiarray *m);

/* The element width and per-node capacity the array was created with */
uint16_t multiarrayElementLen(const multiarray *m);
uint16_t multiarrayRowMax(const multiarray *m);

/* Bulk copy out (O(n) node walk) and bulk build (straight into full nodes) */
void multiarrayCopyTo(const multiarray *m, void *out);
multiarray *multiarrayNewFromArray(uint16_t len, uint16_t rowMax,
                                   const void *elements, size_t count);

/* Overwrite one element in place */
void multiarrayReplace(multiarray *m, multiarrayIdx idx, const void *what);
/* Copy one element out and delete it, locating it once */
void multiarrayPop(multiarray **m, multiarrayIdx idx, void *out);
/* Delete 'count' elements from 'start': located once, whole nodes dropped,
 * a partial node moved once */
void multiarrayDeleteRange(multiarray **m, multiarrayIdx start,
                           uint32_t count);
/* Move the last element to the front */
void multiarrayRotate(multiarray **m);

/* Node-by-node walk in either direction, from an end or from an index */
void multiarrayIteratorInit(const multiarray *m, multiarrayIter *iter,
                            bool forward);
void multiarrayIteratorInitAt(const multiarray *m, multiarrayIter *iter,
                              multiarrayIdx idx, bool forward);
void *multiarrayIteratorNext(multiarrayIter *iter);
```

Notes:

- There is no `multiarrayInsertAfter()` or `multiarrayInsertBefore()`. There is
  one `multiarrayInsert()`, and `idx` is the position the new element will
  occupy (so "insert after element `i`" is `multiarrayInsert(&m, i + 1, x)`).
- To visit every element, use the iterator: each `multiarrayIteratorNext()` is
  O(1) amortized, while `multiarrayGet(m, i)` on a Medium or Large array walks
  nodes from the front, so a `Get` loop is quadratic.  `Next` returns a pointer
  to the element in place; the array must not change during the walk.  The
  iterator is a stack struct and needs no release.

`multiarrayInsert()`, `multiarrayDelete()`, `multiarrayPop()`,
`multiarrayDeleteRange()` and `multiarrayRotate()` take `multiarray **` because
the insert path may replace the pointer with a promoted representation.

```c
#include "multiarray.h"
#include <inttypes.h>

typedef struct s16 {
    int64_t a;
    int64_t b;
} s16;

multiarray *m = multiarrayNew(sizeof(s16), 512);

for (int32_t i = 0; i < 4096; i++) {
    s16 v = {.a = i, .b = -i};
    multiarrayInsert(&m, i, &v); /* append */
}

s16 *got = (s16 *)multiarrayGet(m, 2048);
printf("elem 2048: (%" PRId64 ", %" PRId64 ")\n", got->a, got->b);

s16 *first = (s16 *)multiarrayGetHead(m);
s16 *last = (s16 *)multiarrayGetTail(m);
printf("head a=%" PRId64 " tail a=%" PRId64 "\n", first->a, last->a);

multiarrayFree(m);
```

## Native Macro API

The Native API works on a plain typed array with **no container struct**. You
supply the element type (`holder`), the per-node maximum (`rowMax`), and your
own element counter; the macros mutate `mar` in place when a promotion happens.

```c
#define multiarrayNativeNew(holder)  /* -> multiarraySmallNativeNew(holder) */
#define multiarrayNativeFree(mar)
#define multiarrayNativeGet(mar, holder, result, idx)
#define multiarrayNativeGetForward(mar, holder, result, idx)
#define multiarrayNativeGetHead(mar, holder, result)
#define multiarrayNativeGetTail(mar, holder, result, count)
#define multiarrayNativeInsert(mar, holder, rowMax, localCount, idx, s)
#define multiarrayNativeDelete(mar, holder, count, idx)
```

Contract details that are easy to get wrong:

- `multiarrayNativeNew(holder)` expands to `zcalloc(1, sizeof(holder))`. The
  argument is the **element type**, and the result is a **one-element**
  allocation. It is not a capacity request: `multiarrayNativeNew(MyType)` is
  correct, and passing an array expression does not reserve a block.
- `mar` must be a `multiarray *` lvalue. `multiarrayNativeInsert()` assigns a
  promoted container back into it, so a typed pointer variable would be a
  pointer-type mismatch. Cast at the access sites instead (the `result`
  parameter of the Get macros absorbs the cast for you).
- The Get macros are statements, not expressions: they write into `result`.
- `localCount` / `count` are **your** counters. Insert increments them; delete
  decrements them. The macros never read a count out of the container.
- `multiarrayNativeFree()` dispatches on the tag and handles Native, Medium, and
  Large. It asserts on a Small tag — Native-rooted arrays never become Small.
- Native growth is a `zrealloc()` to exactly `count + 1` elements on every
  insert (see `_multiarraySmallNativeGrowShrink`). There is no geometric
  overallocation, and deletes realloc back down.

```c
#include "multiarray.h"
#include <inttypes.h>

typedef struct Entry {
    uint64_t key;
    uint64_t value;
} Entry;

multiarray *arr = multiarrayNativeNew(Entry);
int32_t count = 0;
const int32_t rowMax = 512;

for (int32_t i = 0; i < 20000; i++) {
    Entry e = {.key = (uint64_t)i, .value = (uint64_t)i * 10};
    multiarrayNativeInsert(arr, Entry, rowMax, count, count, &e);
    /* count was incremented by the macro */
}

Entry *got = NULL;
multiarrayNativeGet(arr, Entry, got, 5000);
printf("key=%" PRIu64 " value=%" PRIu64 "\n", got->key, got->value);

Entry *head = NULL;
multiarrayNativeGetHead(arr, Entry, head);

Entry *tail = NULL;
multiarrayNativeGetTail(arr, Entry, tail, count);

multiarrayNativeDelete(arr, Entry, count, 3); /* count decremented */

multiarrayNativeFree(arr);
```

## Small API

```c
multiarraySmall *multiarraySmallNew(uint16_t len, uint16_t rowMax);
void multiarraySmallFree(multiarraySmall *mar);
void multiarraySmallInsert(multiarraySmall *mar, uint16_t idx, void *s);
void multiarraySmallDelete(multiarraySmall *mar, uint16_t idx);

#define multiarraySmallGet(mar, idx) ((mar)->data + _marsOff((mar)->len, idx))
#define multiarraySmallGetHead(mar) multiarraySmallGet(mar, 0)
#define multiarraySmallGetTail(mar) multiarraySmallGet(mar, (mar)->count - 1)
```

`multiarraySmallGet()` returns `uint8_t *`; cast it to your element type.
Both `multiarraySmallInsert()` and `multiarraySmallDelete()` assert their index:
insert accepts `idx <= mar->count` (append position included), delete requires a
non-empty array and `idx < mar->count`.

```c
multiarraySmall *small = multiarraySmallNew(sizeof(int64_t), 512);

int64_t v = 42;
multiarraySmallInsert(small, 0, &v);          /* head */
multiarraySmallInsert(small, small->count, &v); /* append */

int64_t *got = (int64_t *)multiarraySmallGet(small, 0);
printf("elem 0: %" PRId64 "\n", *got);

multiarraySmallDelete(small, 0);
multiarraySmallFree(small);
```

### Small Direct macros

The Direct macros manipulate an array whose element type is exactly
`typeof(*e)`, with no container:

```c
#define multiarraySmallDirectNew(e)  (zcalloc(1, sizeof(*(e))))
#define multiarraySmallDirectFree(e) (zfree(e))
#define multiarraySmallDirectGet(e, idx) (&(e)[idx])
#define multiarraySmallDirectGetHead(e) multiarraySmallDirectGet(e, 0)
#define multiarraySmallDirectGetTail(e, count)                                 \
    multiarraySmallDirectGet(e, (count) - 1)
#define multiarraySmallDirectInsert(e, count, idx)
#define multiarraySmallDirectDelete(e, count, idx)
```

`multiarraySmallDirectInsert()` only _opens a slot_ — it reallocs to
`count + 1` elements and memmoves the tail up. It does not copy a value in and
it does not touch `count`. Write the element and bump your counter yourself.
`count` is always the count **before** the operation.

```c
typedef struct Point {
    float x, y;
} Point;

Point *points = multiarraySmallDirectNew(points);
int32_t count = 0;

multiarraySmallDirectInsert(points, count, 0);
points[0].x = 1.0f;
points[0].y = 2.0f;
count++;

multiarraySmallDirectDelete(points, count, 0);
count--;

multiarraySmallDirectFree(points);
```

### Small Native macros

Same idea, but the element width is supplied per call so the array can be held
as an untyped pointer:

```c
#define multiarraySmallNativeNew(what) (zcalloc(1, sizeof(what)))
#define multiarraySmallNativeFree(e) (zfree(e))
#define multiarraySmallNativeGet(e, what, idx) (&((what *)(e))[idx])
#define multiarraySmallNativeGetHead(e, what)  multiarraySmallNativeGet(e, what, 0)
#define multiarraySmallNativeGetTail(e, what, count)                           \
    multiarraySmallNativeGet(e, what, (count) - 1)
#define multiarraySmallNativeInsert(e, holder, count, idx)
#define multiarraySmallNativeDelete(e, holder, count, idx)
```

Like the Direct variants, `multiarraySmallNativeInsert()` only opens the slot.
`multiarrayNativeInsert()` is what pairs it with the `memcpy` of your value.

## Medium API

```c
multiarrayMedium *multiarrayMediumNew(int16_t len, int16_t rowMax);
multiarrayMedium *multiarrayMediumFromSmall(multiarraySmall *small);
multiarrayMedium *multiarrayMediumNewWithData(int16_t len, int16_t rowMax,
                                              uint16_t count, void *data);

void multiarrayMediumInsert(multiarrayMedium *mar, const int32_t idx,
                            const void *s);
void multiarrayMediumDelete(multiarrayMedium *mar, const int32_t idx);
void *multiarrayMediumGet(const multiarrayMedium *mar, const int32_t idx);
void *multiarrayMediumGetForward(const multiarrayMedium *mar,
                                 const uint32_t idx);
void *multiarrayMediumGetHead(const multiarrayMedium *mar);
void *multiarrayMediumGetTail(const multiarrayMedium *mar);

void multiarrayMediumFreeInside(multiarrayMedium *e);
void multiarrayMediumFree(multiarrayMedium *e);
```

`len` and `rowMax` are `int16_t` here (not `uint16_t` as in Small and Large).

`multiarrayMediumGet()` accepts a **negative** index meaning "from the tail":
`-1` is the last element. `multiarrayMediumGetForward()` takes a `uint32_t` and
has no reverse mode.

`multiarrayMediumFromSmall()` consumes the Small: it `zrealloc()`s the
`multiarraySmall` allocation into the `multiarrayMedium` and reuses its data
array as node 0. Do not free the Small afterwards.

`multiarrayMediumNewWithData()` adopts `data` as node 0's array with `count`
elements and allocates a second, empty node, so the new container starts with
`mar->count == 2`.

## Large API

```c
multiarrayLarge *multiarrayLargeNew(uint16_t len, uint16_t rowMax);
multiarrayLarge *multiarrayLargeFromMedium(multiarrayMedium *medium);

void multiarrayLargeInsert(multiarrayLarge *mar, const int32_t idx,
                           const void *s);
void multiarrayLargeDelete(multiarrayLarge *mar, const int32_t idx);
void *multiarrayLargeGet(const multiarrayLarge *mar, const int32_t idx);
void *multiarrayLargeGetForward(const multiarrayLarge *mar,
                                const uint32_t index);
void *multiarrayLargeGetHead(const multiarrayLarge *mar);
void *multiarrayLargeGetTail(const multiarrayLarge *mar);

void multiarrayLargeFreeInside(multiarrayLarge *e);
void multiarrayLargeFree(multiarrayLarge *e);
```

`multiarrayLargeGet()` also accepts negative indexes and walks backwards from
`tail`. `multiarrayLargeFromMedium()` consumes the Medium the same way
`multiarrayMediumFromSmall()` consumes the Small.

## Promotion Thresholds

Both ladders are driven by `rowMax`, but they test different things.

### Container ladder (`_multiarrayUpgrade` in `multiarray.c`)

Checked **before** each `multiarrayInsert()`:

```c
if (_multiarrayType(*m) == MULTIARRAY_TYPE_SMALL) {
    small = mars(*m);
    if (small->count == small->rowMax) {
        medium = multiarrayMediumFromSmall(small);
        *m = _MULTIARRAY_TAG(medium, MULTIARRAY_TYPE_MEDIUM);
    }
} else if (_multiarrayType(*m) == MULTIARRAY_TYPE_MEDIUM) {
    medium = marm(*m);
    if (medium->count == medium->rowMax) {
        full = multiarrayLargeFromMedium(medium);
        *m = _MULTIARRAY_TAG(full, MULTIARRAY_TYPE_LARGE);
    }
}
```

- Small -> Medium when the **element** count reaches `rowMax`.
- Medium -> Large when the **node** count reaches `rowMax`. Note the asymmetry:
  `medium->count` counts nodes, so with `rowMax = 512` the Medium tier holds up
  to roughly `512 * 512` elements before becoming Large.

The upgrade runs before the insert so a container that stops exactly at the
boundary is not left holding an oversized representation.

### Native ladder (`multiarrayNativeInsert` in `multiarray.h`)

```c
/* Native -> Medium */
if ((localCount) == (rowMax)) {
    (mar) = (multiarray *)multiarrayMediumNewWithData(
        sizeof(holder), rowMax, localCount, mar);
    (mar) = _MULTIARRAY_TAG(mar, MULTIARRAY_TYPE_MEDIUM);
    multiarrayMediumInsert(marm(mar), idx, s);
}

/* Medium -> Large: pointer array outgrew what Small would have cost */
if ((sizeof(void *) * marm(mar)->count) > (sizeof(holder) * (rowMax))) {
    (mar) = (multiarray *)multiarrayLargeFromMedium(marm(mar));
    (mar) = _MULTIARRAY_TAG(mar, MULTIARRAY_TYPE_LARGE);
    multiarrayLargeInsert(marl(mar), idx, s);
}
```

The Medium -> Large test here compares the node-pointer array's byte cost
against one Small row's byte cost, so it depends on `sizeof(holder)` as well as
`rowMax`.

### Node splitting (Medium and Large)

Both tiers share the split macros in `multiarrayMediumLarge.h`. When the target
node is already at `rowMax`, insert picks one of four shapes based on the offset
inside the node:

| Offset in node    | Action                                              |
| ----------------- | --------------------------------------------------- |
| `0`               | new single-element node inserted **before** current |
| `rowMax`          | new single-element node inserted **after** current  |
| `remaining < off` | split, new node **after**, tail half moved into it  |
| otherwise         | split, new node **before**, head half moved into it |

Delete removes an entire node when its `count` drops to zero, except that both
tiers always keep at least one node alive (its contents are zeroed instead).

## Performance Characteristics

`N` is the element count, `K` the node count (`K ~= N / rowMax`).

| Operation    | Small             | Medium                         | Large                          |
| ------------ | ----------------- | ------------------------------ | ------------------------------ |
| `Get`        | O(1)              | O(K) node scan + O(1)          | O(K) list walk + O(1)          |
| `GetForward` | n/a               | O(K) node scan + O(1)          | O(K) list walk + O(1)          |
| `Insert`     | O(N) realloc+move | O(K + rowMax)                  | O(K + rowMax)                  |
| `Delete`     | O(N) realloc+move | O(K + rowMax)                  | O(K + rowMax)                  |
| `GetHead`    | O(1)              | O(1)                           | O(1)                           |
| `GetTail`    | O(1)              | O(1) (O(K) if tail node empty) | O(1) (O(K) if tail node empty) |

`GetForward` is **not** a stateful iterator. It does not memoize the previous
traversal position; it is the forward-only entry point (no negative-index
handling) and, for Large, it short-circuits the single-node case. Use it when
you know the index is non-negative; do not expect it to make an
index-by-index loop cheaper than O(N\*K).

To iterate cheaply, use `multiarrayIteratorInit()` / `multiarrayIteratorNext()`
on a container array (or walk a Native Large's node list directly) instead of
calling `Get`/`GetForward` per index — see EXAMPLES.md.

| Representation | Container bytes | Per-node bytes | Notes                        |
| -------------- | --------------- | -------------- | ---------------------------- |
| Native         | 0               | n/a            | exact-fit realloc per insert |
| Small          | 16              | n/a            | one contiguous data array    |
| Medium         | 16              | 16             | flat node array              |
| Large          | 24              | 16             | XOR-linked nodes             |

`multiarraySmall.h` notes that a single contiguous Small array stays efficient
for inserts and deletes up to roughly 8 KB - 16 KB of data, i.e. about 512-1024
entries at 16 bytes each.

## Best Practices

### Pass the address for mutations

```c
/* WRONG - the promoted pointer is written into a local copy */
void addItem(multiarray *m, void *item) {
    multiarrayInsert(&m, 0, item);
}

/* RIGHT */
void addItem(multiarray **m, void *item) {
    multiarrayInsert(m, 0, item);
}
```

The same rule applies to the Native macros: `mar` must be the real variable,
because `multiarrayNativeInsert()` assigns through it.

### Track your own count

Neither the container API nor the Native API exposes a total element count.
Keep a counter next to the array; the Native macros already update yours for
you.

### Size rowMax for the element width

`rowMax` is the per-node element capacity. A small `rowMax` means more nodes
(more scan work in `Get`, more pointer-array bytes, earlier Medium -> Large in
the Native ladder); a large `rowMax` means fewer, bigger memmoves per insert.
Roughly 8 KB - 16 KB per node (`rowMax * sizeof(element)`) matches the Small
guidance in the source.

### Append in ascending order when you can

Inserting at index 0 repeatedly forces a memmove of the whole node on every
insert in Small, and a node split at the head in Medium and Large. Appending
touches only the tail node.

## Thread Safety

multiarray is **not thread-safe**. Nothing in the module takes a lock, and the
insert path rewrites the caller's pointer, so even a "reader" that races an
insert can observe a stale representation tag. Serialize all access:

```c
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
multiarray *shared = multiarrayNew(sizeof(int64_t), 1024);

pthread_mutex_lock(&lock);
int64_t *val = (int64_t *)multiarrayGet(shared, 42);
pthread_mutex_unlock(&lock);

pthread_mutex_lock(&lock);
int64_t data = 999;
multiarrayInsert(&shared, 0, &data);
pthread_mutex_unlock(&lock);
```

## Memory Management

| Created with                 | Free with                     |
| ---------------------------- | ----------------------------- |
| `multiarrayNew()`            | `multiarrayFree()`            |
| `multiarrayNativeNew()`      | `multiarrayNativeFree()`      |
| `multiarraySmallNew()`       | `multiarraySmallFree()`       |
| `multiarrayMediumNew()`      | `multiarrayMediumFree()`      |
| `multiarrayLargeNew()`       | `multiarrayLargeFree()`       |
| `multiarraySmallDirectNew()` | `multiarraySmallDirectFree()` |

`multiarrayFree()` dispatches on the tag and handles Small, Medium, and Large.
`multiarrayNativeFree()` handles Native, Medium, and Large. Use the one that
matches how the array was created — they cover different tag sets.

`multiarrayMediumFreeInside()` and `multiarrayLargeFreeInside()` release the
nodes and their data but not the container struct itself; they exist for callers
that embedded or realloc'd the container.

Because `multiarrayMediumFromSmall()` and `multiarrayLargeFromMedium()` realloc
the source container into the destination, the source pointer is dead after the
call. Never free it separately.

## See Also

- [VARIANTS.md](VARIANTS.md) - How the four representations differ
- [EXAMPLES.md](EXAMPLES.md) - Worked multiarray walkthroughs
- [databox](../core/DATABOX.md) - Universal container type

## Testing

```bash
./src/datakit-test test multiarray
./src/datakit-test test multiarraySmall
./src/datakit-test test multiarrayMedium
./src/datakit-test test multiarrayLarge
```

The `multiarray` suite covers both entry points (container and Native macro),
head/tail/random insert ordering, tier transitions across `rowMax` boundaries,
and oracle-verified fuzz runs against a plain C array.
