# multiarray Variants - Scale-Aware Design

## Overview

The multiarray family implements a **scale-aware architecture** with four
representations. A single `multiarray *` variable carries a 2-bit tag in its low
pointer bits identifying which representation it currently points at, and the
insert paths rewrite that variable when a promotion happens.

Which promotions you get depends on which entry point you used:

| Entry point             | Starts as | Promotion ladder          |
| ----------------------- | --------- | ------------------------- |
| `multiarrayNew()`       | Small     | Small -> Medium -> Large  |
| `multiarrayNativeNew()` | Native    | Native -> Medium -> Large |

The container API also moves down: after a delete, pop or range delete, a
Large holding `rowMax / 2` nodes or fewer becomes Medium (its nodes move into
one node array, element data kept in place), and a Medium holding
`rowMax / 2` elements or fewer in at most two nodes becomes Small (one
buffer). Large keeps its node count in its struct, so the check is one
compare. The Native macro API never demotes, and an array it made must stay
on it: a container delete could turn its Medium into a Small, which the
macros don't handle.

## Summary Table

| Variant    | Container bytes | Node bytes | Storage                   | Element index cost |
| ---------- | --------------- | ---------- | ------------------------- | ------------------ |
| **Native** | 0               | n/a        | one exact-fit typed array | O(1)               |
| **Small**  | 16              | n/a        | one contiguous data array | O(1)               |
| **Medium** | 16              | 16         | flat array of nodes       | O(K) scan          |
| **Large**  | 24              | 16         | XOR doubly-linked nodes   | O(K) walk          |

`K` is the node count. Medium and Large nodes are the **same size** — 16 bytes
each. Large's advantage over Medium is not per-node bytes (see
[Why Large exists](#why-large-exists)).

## Native Variant

### Purpose

Native is not a struct. It is a set of macros in `multiarray.h` that operate on
a plain typed array plus a caller-owned count, promoting to Medium when the
count reaches `rowMax`.

### Structure

```c
/* Native has no container struct - it is just the data array.
 * The pointer tag (low 2 bits == 00) marks it Native. */

typedef struct MyData {
    int64_t a, b;
} MyData;

multiarray *native = multiarrayNativeNew(MyData);
/* expands to zcalloc(1, sizeof(MyData)) - a ONE element allocation */
```

`multiarrayNativeNew()` takes the **element type**, not a capacity. It allocates
room for exactly one element; every `multiarrayNativeInsert()` then
`zrealloc()`s to exactly `count + 1` elements:

```c
#define _multiarraySmallNativeGrowShrink(what, holder, count)                  \
    do {                                                                       \
        (what) = zrealloc((what), (sizeof(holder)) * (count));                 \
    } while (0)
```

Hold the array in a `multiarray *` variable, not a typed pointer:
`multiarrayNativeInsert()` assigns a promoted container back into it.

### Characteristics

**Advantages:**

- **No container overhead**: the allocation is nothing but elements
- **Perfect cache locality**: fully contiguous while it stays Native
- **Automatic promotion**: becomes Medium at `rowMax` with no code change
- **Type-checked access**: the `holder` argument keeps the casts honest

**Disadvantages:**

- **Realloc on every insert and delete**: exact-fit growth, no geometric
  headroom, so append-heavy loops pay a `zrealloc()` per element
- **Caller tracks the count**: the macros update your variable, but nothing
  stores it
- **Macro-only**: `mar` must be a plain lvalue, so it cannot live behind an
  accessor
- **No Small tier**: `multiarrayNativeFree()` asserts on a Small tag

**Promotion trigger** (`multiarrayNativeInsert`):

```c
if ((localCount) == (rowMax)) {
    (mar) = (multiarray *)multiarrayMediumNewWithData(
        sizeof(holder), rowMax, localCount, mar);
    (mar) = _MULTIARRAY_TAG(mar, MULTIARRAY_TYPE_MEDIUM);
    multiarrayMediumInsert(marm(mar), idx, s);
}
```

The existing array is adopted as node 0 of the new Medium — no element copy.

### Example

```c
#include "multiarray.h"
#include <inttypes.h>

typedef struct Point {
    float x, y, z;
} Point;

multiarray *points = multiarrayNativeNew(Point);
int32_t count = 0;
const int32_t rowMax = 1000;

for (int32_t i = 0; i < 1000; i++) {
    Point p = {.x = (float)i, .y = (float)i * 2, .z = (float)i * 3};
    multiarrayNativeInsert(points, Point, rowMax, count, count, &p);
}

Point *p = NULL;
multiarrayNativeGet(points, Point, p, 500);
printf("point 500: (%.1f, %.1f, %.1f)\n", p->x, p->y, p->z);

/* count == rowMax, so this insert promotes to Medium */
Point extra = {.x = 999, .y = 999, .z = 999};
multiarrayNativeInsert(points, Point, rowMax, count, count, &extra);

multiarrayNativeFree(points); /* handles Native, Medium, and Large */
```

## multiarraySmall

### Structure

```c
struct multiarraySmall {
    uint8_t *data;
    uint16_t len;    /* width of each entry in 'data' */
    uint16_t count;  /* number of entries (i.e. size of data is len * count) */
    uint16_t rowMax; /* not used here, but used for upgrading. */
    /* 2 bytes padding */
};
```

16 bytes. `len` comes before `count`. All three counters are `uint16_t`, so a
Small tops out at 65535 elements — well past the point where the container
ladder promotes it.

### Architecture

```
┌──────────────────────────────────────┐
│ multiarraySmall (16 bytes)           │
├──────────────────────────────────────┤
│ data ────────────────┐               │
│ len = 16             │               │
│ count = 5            │               │
│ rowMax = 512         │               │
└──────────────────────┼───────────────┘
                       │
                       ▼
        ┌─────────────────────────────┐
        │  Single array (contiguous)  │
        ├─────────────────────────────┤
        │ [E0][E1][E2][E3][E4]        │
        │  (16 bytes each)            │
        └─────────────────────────────┘
```

### How Small works

- **Single allocation**: every element lives in one contiguous block
- **Direct indexing**: `data + (len * idx)`
- **Exact-fit realloc**: insert reallocs to `count + 1`, delete to `count - 1`
- **memmove for shifts**: insert opens a slot, delete closes one

The real implementation (`multiarraySmall.c`), with the `size_t` offset helper
that keeps `len * count` from overflowing an `int`:

```c
#define _mo(offset) ((size_t)mar->len * (offset))

void multiarraySmallInsert(multiarraySmall *mar, uint16_t idx, void *s) {
    assert(idx <= mar->count);

    mar->data = zrealloc(mar->data, _mo(mar->count + 1));

    if (idx < mar->count) {
        const uint32_t remaining = mar->count - idx;
        memmove(mar->data + _mo(idx + 1), mar->data + _mo(idx), _mo(remaining));
    }

    memcpy(mar->data + _mo(idx), s, mar->len);
    mar->count++;
}
```

`multiarraySmallDelete()` asserts `mar->count > 0` and `idx < mar->count`;
deleting from an empty Small would otherwise request a near-`SIZE_MAX` realloc.

### Characteristics

**Advantages:**

- **Minimal overhead**: 16 bytes for the whole container
- **Cache-friendly**: one contiguous allocation
- **True O(1) indexing**: no node scan
- **Simple**: the whole tier is ~50 lines

**Disadvantages:**

- **O(n) insert and delete**: a head insert memmoves every element
- **Realloc per operation**: exact-fit sizing means the allocator is hit on
  every mutation
- **One big block**: a large Small is a single large allocation to find

**Promotion trigger** (container ladder only, checked before each insert):

```c
if (small->count == small->rowMax) {
    medium = multiarrayMediumFromSmall(small);
    *m = _MULTIARRAY_TAG(medium, MULTIARRAY_TYPE_MEDIUM);
}
```

`multiarrayMediumFromSmall()` `zrealloc()`s the `multiarraySmall` allocation
itself into a `multiarrayMedium` and keeps the data array as node 0. The Small
pointer is dead afterwards.

A standalone `multiarraySmall` created with `multiarraySmallNew()` never
promotes — only the container API's `multiarrayInsert()` runs the check.

### Best use cases

1. Arrays that stay under a few hundred elements
2. Append-mostly lists (tail insert avoids the memmove)
3. Containers embedded in a larger structure that wants a fixed 16-byte field
4. Any case where true O(1) indexing matters more than insert cost

### Example

```c
multiarraySmall *small = multiarraySmallNew(sizeof(int64_t), 512);

/* Insert at head - shifts every existing element */
for (int64_t i = 0; i < 100; i++) {
    multiarraySmallInsert(small, 0, &i);
}

/* Append - no shift */
for (int64_t i = 100; i < 200; i++) {
    multiarraySmallInsert(small, small->count, &i);
}

int64_t *val = (int64_t *)multiarraySmallGet(small, 50);
printf("element 50: %" PRId64 "\n", *val);

multiarraySmallFree(small);
```

## multiarrayMedium

### Structure

```c
typedef multiarraySmall multiarrayMediumNode;

struct multiarrayMedium {
    multiarrayMediumNode *node;
    uint32_t count;  /* number of 'node' (i.e. node is size: len * count) */
    uint16_t len;    /* width of individual entries in node->data */
    uint16_t rowMax; /* maximum entries inside each 'node' */
};
```

A node **is** a `multiarraySmall` — the typedef reuses it for the `data`/`count`
pair it already has. A node's own `len` and `rowMax` fields go unused, so each
node costs 16 bytes.

`multiarrayMedium.count` is the **node** count. The container does not store a
total element count; sum `node[i].count` if you need one.

### Architecture

```
┌────────────────────────────────────┐
│ multiarrayMedium (16 bytes)        │
├────────────────────────────────────┤
│ node ──────┐                       │
│ count = 3  │   (3 NODES, not 3 elements)
│ len = 16   │                       │
│ rowMax=512 │                       │
└────────────┼───────────────────────┘
             │
             ▼
      ┌─────────────────┐
      │  Node array     │  contiguous, realloc'd on node insert
      ├─────────────────┤
      │ Node 0: ───┐    │
      │   count=512│    │
      │ Node 1: ───┼──┐ │
      │   count=512│  │ │
      │ Node 2: ───┼──┼─┼──┐
      │   count=200│  │ │  │
      └────────────┼──┼─┼──┘
                   │  │ │
        ┌──────────┘  │ │
        │  ┌──────────┘ │
        │  │  ┌─────────┘
        ▼  ▼  ▼
      ┌──┬──┬──┐
      │[]│[]│[]│  Data chunks
      └──┴──┴──┘  (each up to rowMax elements)
```

### How Medium works

Elements are found by walking the node array and accumulating node counts —
this is a linear scan over nodes, not an O(1) computation, because nodes are not
uniformly full after splits:

```c
void *multiarrayMediumGetForward(const multiarrayMedium *mar,
                                 const uint32_t index) {
    int32_t nodeIdx = 0;

    uint32_t accum = 0;
    multiarrayMediumNode *node = getNode(nodeIdx);
    while ((accum + node->count) <= index) {
        accum += node->count;
        node = getNode(++nodeIdx);
    }

    assert(node->count > 0);

    int32_t offset = index - accum;
    return node->data + (offset * mar->len);
}
```

`multiarrayMediumGet()` is the same walk plus a reverse mode: a negative `idx`
counts back from the last node, so `-1` is the last element.
`multiarrayMediumGetForward()` takes a `uint32_t` and has no reverse mode. It
does **not** cache traversal state between calls.

**Node splitting.** When the target node is already at `rowMax`,
`multiarrayMediumInsert()` picks one of four shapes based on the offset inside
that node (all four are macros in `multiarrayMediumLarge.h`):

| Offset in node       | Action                                                                                       |
| -------------------- | -------------------------------------------------------------------------------------------- |
| `0`                  | appended to the node before when it has room, else a single-element node inserted **before** |
| `rowMax`             | single-element node inserted **after** the current one                                       |
| `remaining < offset` | split; new node **after**, tail half copied into it                                          |
| otherwise            | split; new node **before**, head half copied into it                                         |

Splitting always moves the _smaller_ half, so a split costs at most
`rowMax / 2` elements of copying.

Inserting a node is `multiarraySmallDirectInsert()` on `mar->node` — a realloc
of the whole node array plus a memmove of every node after the insertion point.
That O(K) node-array shuffle is the cost Large exists to remove.

**Node deletion.** When a node's element count would drop to zero, the node and
its data are freed and the node array shrinks — unless it is the last remaining
node, which is kept and zeroed so the container always has somewhere to insert.
A node a delete leaves holding elements merges with a neighbour when the pair
fits in half a node (Large does the same).

### Characteristics

**Advantages:**

- **Bounded insert cost**: a memmove touches at most one node, not the array
- **Bounded split cost**: the smaller half moves
- **Chunked allocations**: no single huge block to find
- **Small header**: still 16 bytes

**Disadvantages:**

- **Two-level lookup**: node scan then offset, so indexing is O(K)
- **Node-array churn**: every node insert reallocs and memmoves the node array
- **Pointer indirection**: an extra dependent load per lookup
- **No total count**: you sum node counts yourself

**Promotion triggers.** The two ladders disagree, deliberately:

```c
/* Container ladder (_multiarrayUpgrade): node count reaches rowMax */
if (medium->count == medium->rowMax) {
    full = multiarrayLargeFromMedium(medium);
    *m = _MULTIARRAY_TAG(full, MULTIARRAY_TYPE_LARGE);
}

/* Native ladder (multiarrayNativeInsert): node-pointer array outgrew
 * what one Small row would have cost */
if ((sizeof(void *) * marm(mar)->count) > (sizeof(holder) * (rowMax))) {
    (mar) = (multiarray *)multiarrayLargeFromMedium(marm(mar));
    (mar) = _MULTIARRAY_TAG(mar, MULTIARRAY_TYPE_LARGE);
}
```

The container test compares node count against `rowMax`, so with
`rowMax = 512` a Medium can hold on the order of `512 * 512` elements before
becoming Large. The Native test compares bytes and therefore also depends on
`sizeof(holder)`.

### Best use cases

1. Element counts large enough that a whole-array memmove hurts
2. Insert-heavy workloads at interior positions
3. Chunked processing that wants per-node contiguity
4. Anything between "one contiguous array is fine" and "the node array itself is
   the problem"

### Example

```c
multiarrayMedium *medium = multiarrayMediumNew(sizeof(double), 512);

int32_t total = 0;
for (int32_t i = 0; i < 10000; i++) {
    double val = i * 3.14;
    multiarrayMediumInsert(medium, total, &val); /* append */
    total++;
}

/* medium->count is the NODE count, not the element count */
printf("nodes: %" PRIu32 ", elements: %" PRId32 "\n", medium->count, total);

double *val = (double *)multiarrayMediumGet(medium, 5000);
printf("element 5000: %.2f\n", *val);

double *last = (double *)multiarrayMediumGet(medium, -1); /* reverse index */
printf("last: %.2f\n", *last);

multiarrayMediumFree(medium);
```

## multiarrayLarge

### Structure

```c
typedef struct multiarrayLargeNode {
    uint8_t *data;
    uint64_t prevNext : 48; /* xor of addrs is likely much less than 48 bits */
    uint64_t count : 16;    /* we should only have max 32k entries per node. */
} multiarrayLargeNode;

struct multiarrayLarge {
    multiarrayLargeNode *head;
    multiarrayLargeNode *tail;
    uint16_t len;    /* width of individual node->data entries */
    uint16_t rowMax; /* maximum entries per node before creating new node */
    /* 4 bytes padding */
};
```

The container is 24 bytes. Each node is **16 bytes**: an 8-byte data pointer
plus one 8-byte word shared by the 48-bit `prevNext` XOR link and the 16-bit
element `count`. That bit packing is what keeps a doubly-linked node the same
size as a Medium node.

The 48-bit link assumes pointers fit in 48 bits, which holds on the 64-bit
targets datakit supports.

### Architecture

```
┌─────────────────────────────┐
│ multiarrayLarge (24 bytes)  │
├─────────────────────────────┤
│ head ──────┐                │
│ tail ──────┼──┐             │
│ len = 16   │  │             │
│ rowMax=512 │  │             │
└────────────┼──┼─────────────┘
             │  │
    ┌────────┘  └───────────┐
    ▼                       ▼
  ┌────┐  XOR  ┌────┐  XOR  ┌────┐
  │Node│◄─────►│Node│◄─────►│Node│
  │  0 │ linked│  1 │ linked│  2 │
  └─┬──┘  list └─┬──┘  list └─┬──┘
    │            │            │
    ▼            ▼            ▼
  [data]      [data]      [data]
  (512 elem)  (512 elem)  (200 elem)
```

### XOR linked list

Each node stores `prev XOR next` in a single field instead of two pointers:

```c
#define _ptrXor(a, b) ((multiarrayLargeNode *)((uintptr_t)(a) ^ (uintptr_t)(b)))
#define getNext(prev, current) _ptrXor(prev, (current)->prevNext)
```

Properties:

- `current->prevNext == (uintptr_t)prev ^ (uintptr_t)next`
- Given `prev` and `current`: `next = prev ^ current->prevNext`
- Given `next` and `current`: `prev = next ^ current->prevNext`
- One link word instead of two saves 8 bytes per node, which is exactly what
  lets the node pack into 16 bytes alongside its `count`

The cost is that you cannot walk from an arbitrary node — you must carry the
previous node with you, starting from `head` (or `tail` going backwards).

### How Large works

Indexing walks the list, accumulating node counts:

```c
DK_STATIC inline multiarrayLargeResult
multiarrayLargeGetForwardWorker(multiarrayLargeNode *e, int32_t idx,
                                uint32_t len) {
    multiarrayLargeNode *prev = NULL;
    multiarrayLargeNode *current = e;
    multiarrayLargeNode *next = NULL;

    int32_t accum = 0;
    while ((next = getNext(prev, current)) && (accum + current->count) <= idx) {
        accum += current->count;
        prev = current;
        current = next;
    }

    idx = idx - accum;
    /* ... entry = current->data + (len * idx) ... */
}
```

`multiarrayLargeGet()` adds a reverse mode for negative indexes that walks
backwards from `tail`. `multiarrayLargeGetForward()` takes a `uint32_t`, skips
the reverse handling, and short-circuits when the list has a single node:

```c
void *multiarrayLargeGetForward(const multiarrayLarge *mar,
                                const uint32_t index) {
    multiarrayLargeNode *startNode = mar->head;

    const multiarrayLarge *prev = NULL;
    if (getNext(prev, startNode) == NULL) {
        return startNode->data + (mar->len * index);
    }
    /* ... otherwise the same walk ... */
}
```

It keeps **no state between calls**. Calling it once per index still costs
O(K) per call. To iterate cheaply, walk the node list yourself and process each
node's data array in one pass.

Insert and delete reuse the same split and shrink macros as Medium; only the
node bookkeeping differs — splicing a node into an XOR list is a constant-time
pointer update instead of a node-array realloc.

### Characteristics

**Advantages:**

- **O(1) node insertion**: splicing into the list never touches other nodes
- **No node-array reallocation**: node count can grow without a growing
  contiguous allocation
- **Compact nodes**: 16 bytes, same as Medium, thanks to the packed XOR link
- **O(1) head and tail**: both are stored directly

**Disadvantages:**

- **O(K) random access**: every index walks the list from an end
- **Pointer chasing**: node traversal is a dependent-load chain, unlike Medium's
  contiguous node array
- **XOR arithmetic**: nodes cannot be visited out of order, and a corrupted link
  breaks the whole chain
- **Slightly larger header**: 24 bytes versus Medium's 16

**No further promotion.** Large is the final representation.

### Why Large exists

Large is not a memory optimization over Medium — with 16-byte nodes on both
sides, a Large container is 8 bytes _larger_ than an equivalent Medium.

What Large removes is the **node array**. In Medium, inserting a node calls
`multiarraySmallDirectInsert()` on `mar->node`: a realloc of the entire node
array plus a memmove of every node past the insertion point. At `K` nodes that
is O(K) work and a `16 * K` byte contiguous allocation that has to keep growing.
In Large, the same operation is a handful of XOR updates.

So the trade is: Medium gives cheaper traversal (contiguous, prefetchable node
array); Large gives cheaper node insertion and no single large node allocation.
Both index in O(K).

### Best use cases

1. Node counts high enough that reallocating the node array dominates
2. Workloads that split nodes often (interior inserts across many nodes)
3. Iteration by node rather than by index
4. Head and tail access, which stay O(1) regardless of size

### Example

```c
#include "multiarrayLarge.h"
#include "multiarrayLargeInternal.h" /* needed for head/tail/prevNext */
#include <inttypes.h>

multiarrayLarge *large = multiarrayLargeNew(sizeof(uint64_t), 512);

for (int32_t i = 0; i < 1000000; i++) {
    uint64_t val = (uint64_t)i;
    multiarrayLargeInsert(large, i, &val); /* append */
}

uint64_t *first = (uint64_t *)multiarrayLargeGetHead(large);
uint64_t *last = (uint64_t *)multiarrayLargeGetTail(large);
printf("first: %" PRIu64 ", last: %" PRIu64 "\n", *first, *last);

/* Random access walks the list */
uint64_t *middle = (uint64_t *)multiarrayLargeGet(large, 500000);
printf("middle: %" PRIu64 "\n", *middle);

/* Iterate by NODE, not by index: one pass over the whole list */
multiarrayLargeNode *prev = NULL;
multiarrayLargeNode *current = large->head;
uint64_t count = 0;

while (current) {
    for (uint64_t i = 0; i < current->count; i++) {
        const uint64_t *val =
            (const uint64_t *)(current->data + i * large->len);
        (void)val;
        count++;
    }

    multiarrayLargeNode *next =
        (multiarrayLargeNode *)((uintptr_t)prev ^ (uintptr_t)current->prevNext);
    prev = current;
    current = next;
}

printf("total elements: %" PRIu64 "\n", count);

multiarrayLargeFree(large);
```

## Variant Comparison

### Memory overhead

For `E` elements of width `W`, with `K = ceil(E / rowMax)` nodes:

| Variant | Total bytes           |
| ------- | --------------------- |
| Native  | `E * W`               |
| Small   | `16 + E * W`          |
| Medium  | `16 + 16 * K + E * W` |
| Large   | `24 + 16 * K + E * W` |

Worked example, 100,000 elements of 16 bytes with `rowMax = 512`
(`K = 196` after clean appends):

```
Native:        1,600,000 bytes
Small:    16 + 1,600,000 = 1,600,016 bytes
Medium:   16 + 3,136 + 1,600,000 = 1,603,152 bytes
Large:    24 + 3,136 + 1,600,000 = 1,603,160 bytes
```

Medium and Large are within 8 bytes of each other at any size. Node counts run
higher than `ceil(E / rowMax)` in practice, because splits leave nodes partly
full.

### Access cost

| Access pattern           | Native | Small | Medium        | Large         |
| ------------------------ | ------ | ----- | ------------- | ------------- |
| Index by element         | O(1)   | O(1)  | O(K)          | O(K)          |
| Head / tail              | O(1)   | O(1)  | O(1)          | O(1)          |
| Insert / delete at index | O(E)   | O(E)  | O(K + rowMax) | O(K + rowMax) |
| Full scan, per index     | O(E)   | O(E)  | O(E \* K)     | O(E \* K)     |
| Full scan, node-by-node  | O(E)   | O(E)  | O(E)          | O(E)          |

The last two rows are the practical point: iterating a chunked variant with
`Get(i)` in a loop is quadratic in the node count. Walk the nodes instead.

### Decision matrix

| Scenario                                   | Representation    | Why                                     |
| ------------------------------------------ | ----------------- | --------------------------------------- |
| Small fixed set, indexing dominates        | Native            | no container, fully contiguous          |
| Modest count, needs true O(1) indexing     | Small             | single array, direct offset             |
| Interior inserts hurt, node count moderate | Medium            | memmove bounded to one node             |
| Node count high, node-array realloc hurts  | Large             | O(1) node splice, no growing node array |
| Iterate more than you index                | Medium/Large      | node-by-node scan is linear either way  |
| Unknown final size                         | `multiarrayNew()` | promotes on its own                     |

## Transition Mechanics

### Small -> Medium (container ladder)

```c
/* Before: a Small holding rowMax elements */

/* Trigger, checked before the next insert: small->count == small->rowMax */

/* After: multiarrayMediumFromSmall() zrealloc()s the multiarraySmall
 * allocation itself into a multiarrayMedium and keeps its data array. */
multiarrayMedium {
    node[0] = { data: <the Small's original array>, count: rowMax }
    node[1] = { data: NULL, count: 0 }   /* pre-allocated spare */
    count = 2                            /* NODES */
    len = <element width>
    rowMax = <same rowMax>
}
```

### Native -> Medium (Native ladder)

```c
/* Before: a raw array of rowMax elements, caller count == rowMax */

/* Trigger: localCount == rowMax */

/* After: multiarrayMediumNewWithData() adopts the raw array as node 0 */
multiarrayMedium {
    node[0] = { data: <the original raw array>, count: rowMax }
    node[1] = { data: NULL, count: 0 }
    count = 2
}
/* The new element is then inserted through multiarrayMediumInsert(). */
```

### Medium -> Large

```c
/* Trigger (container ladder): medium->count == medium->rowMax
 * Trigger (Native ladder):
 *     sizeof(void *) * medium->count > sizeof(holder) * rowMax */

/* multiarrayLargeFromMedium() allocates one 16-byte node per Medium node,
 * moves each node's data pointer and count across, frees the node ARRAY
 * (never the node data), then zrealloc()s the multiarrayMedium container
 * itself into a multiarrayLarge. */
multiarrayLarge {
    head -> node0 <-> node1 <-> ... <-> nodeK-1 <- tail
}
```

Element data is never copied by any transition — only container structs and node
bookkeeping move. A single-node Medium produces a Large whose `head == tail`.

## Design Philosophy

The family follows **progressive enhancement**:

1. **Start minimal**: Native or Small, both contiguous and cheap to index
2. **Chunk when moves hurt**: Medium bounds a memmove to one node
3. **Unlink when the node array hurts**: Large removes the contiguous node array
4. **Never move element data**: every transition re-points, never re-copies
5. **Same call sites throughout**: the tag dispatch is invisible to the caller

The cost of that transparency is that the chunked tiers index in O(K), not O(1).
Code that iterates should walk nodes, not indexes.

## See Also

- [MULTIARRAY.md](MULTIARRAY.md) - API reference and promotion thresholds
- [EXAMPLES.md](EXAMPLES.md) - Worked multiarray walkthroughs
- [databox](../core/DATABOX.md) - Universal container type
