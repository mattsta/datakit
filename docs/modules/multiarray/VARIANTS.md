# multiarray Variants - Scale-Aware Design

## Overview

The multiarray family implements a **scale-aware architecture** with four distinct implementations optimized for different dataset sizes and usage patterns. Your code uses a single `multiarray*` pointer, and the implementation automatically chooses and transitions between variants as your data grows.

## The Four Variants

### Summary Table

| Variant    | Size      | Storage         | Overhead  | Best For                     | Max Recommended       |
| ---------- | --------- | --------------- | --------- | ---------------------------- | --------------------- |
| **Native** | 0 bytes   | Direct array    | 0 bytes   | Stack arrays, inline storage | < 1024 elements       |
| **Small**  | 16 bytes  | Single array    | 16 bytes  | Simple dynamic arrays        | < 2048 elements       |
| **Medium** | 16+ bytes | Pointer array   | 16 + N×16 | Moderate datasets            | 2K - 100K elements    |
| **Large**  | 24+ bytes | XOR linked list | 24 + N×24 | Massive datasets             | Unlimited (millions+) |

## Native Variant

### Purpose

Native is a **zero-overhead wrapper** for direct array manipulation. It's not actually a multiarray struct but rather macros that work with raw pointers, allowing automatic upgrade to Small/Medium/Large when needed.

### Architecture

```
┌────────────────────────────┐
│  uint8_t *arr (raw pointer)│
├────────────────────────────┤
│  Element 0                 │
│  Element 1                 │
│  Element 2                 │
│  ...                       │
│  Element N                 │
└────────────────────────────┘

No struct overhead - just a tagged pointer!
```

### Structure

```c
/* Native has NO structure - it's just a pointer! */
/* The pointer tag (last 2 bits = 00) indicates Native */

/* Example allocation */
typedef struct MyData {
    int64_t a, b;
} MyData;

MyData *native = multiarrayNativeNew(native[1024]);
/* This is actually: calloc(1024, sizeof(MyData)) */
```

### Characteristics

**Advantages:**

- **Zero overhead**: No struct allocation, just the data array
- **Maximum cache efficiency**: Contiguous memory
- **Direct access**: Normal array indexing
- **Stack-friendly**: Can allocate small arrays on stack
- **Automatic upgrade**: Seamlessly becomes Medium when full

**Disadvantages:**

- **Fixed size initially**: Must know max size upfront
- **No resize without upgrade**: Growing requires conversion to Medium
- **Manual count tracking**: You track count, not the structure

**Upgrade Trigger:**

```c
/* Upgrades to Medium when: */
if (localCount == rowMax) {
    /* Convert to Medium variant with 2 nodes */
    upgrade_to_medium();
}
```

### Best Use Cases

1. **Temporary arrays** with known maximum size
2. **Stack-based storage** for small datasets
3. **Performance-critical loops** where overhead matters
4. **Embedded systems** with tight memory constraints

### Example

```c
typedef struct Point {
    float x, y, z;
} Point;

/* Allocate for 1000 points */
Point *points = multiarrayNativeNew(points[1000]);
int count = 0;
const int rowMax = 1000;

/* Insert points */
for (int i = 0; i < 1000; i++) {
    Point p = {.x = i, .y = i * 2, .z = i * 3};
    multiarrayNativeInsert(points, Point, rowMax, count, count, &p);
}

/* Direct access */
Point *p;
multiarrayNativeGet(points, Point, p, 500);
printf("Point 500: (%.1f, %.1f, %.1f)\n", p->x, p->y, p->z);

/* On 1001st insert, automatically becomes Medium! */
Point extra = {.x = 999, .y = 999, .z = 999};
multiarrayNativeInsert(points, Point, rowMax, count, count, &extra);
/* points is now a Medium variant */

multiarrayNativeFree(points);  /* Works for all variants */
```

## multiarraySmall

### Structure

```c
struct multiarraySmall {
    uint8_t *data;      /* Single contiguous array */
    uint16_t count;     /* Current number of elements */
    uint16_t len;       /* Size of each element in bytes */
    uint16_t rowMax;    /* Maximum elements before upgrade */
};

/* Total: 16 bytes fixed overhead + data array */
```

### Architecture

```
┌──────────────────────────────────────┐
│ multiarraySmall (16 bytes)           │
├──────────────────────────────────────┤
│ data ────────────────┐               │
│ count = 5            │               │
│ len = 16             │               │
│ rowMax = 1024        │               │
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

### How Small Works

- **Single allocation**: All elements in one contiguous block
- **Direct indexing**: `data + (len * idx)` gives element address
- **Realloc on insert/delete**: Uses `realloc()` to grow/shrink
- **Memmove for shifts**: Moving elements on insert/delete

```c
/* Insertion example (simplified) */
void multiarraySmallInsert(multiarraySmall *mar, uint16_t idx, void *s) {
    /* 1. Grow array */
    mar->data = realloc(mar->data, mar->len * (mar->count + 1));

    /* 2. Shift elements right */
    if (idx < mar->count) {
        memmove(mar->data + (idx + 1) * mar->len,
                mar->data + idx * mar->len,
                (mar->count - idx) * mar->len);
    }

    /* 3. Copy new element */
    memcpy(mar->data + idx * mar->len, s, mar->len);
    mar->count++;
}
```

### Characteristics

**Advantages:**

- **Minimal overhead**: Only 16 bytes for the entire container
- **Cache-friendly**: Single contiguous allocation
- **Simple implementation**: Direct array manipulation
- **Fast sequential access**: No pointer chasing

**Disadvantages:**

- **Slow inserts/deletes**: O(n) memmove operations
- **Realloc thrashing**: Frequent reallocations on growth
- **Limited scalability**: Inefficient beyond ~2000 elements
- **Memory fragmentation**: Large reallocs can fragment heap

**Upgrade Trigger:**

```c
/* Small is typically created from Native upgrade */
/* No automatic upgrade from Small - must manually upgrade to Medium */
```

### Best Use Cases

1. **Small dynamic arrays** (< 100 elements)
2. **Append-only lists** where inserts are at the end
3. **Simple containers** with infrequent modifications
4. **Embedded structures** in other containers

### Example

```c
multiarraySmall *small = multiarraySmallNew(sizeof(int64_t), 512);

/* Insert at head (slow - shifts all elements) */
for (int i = 0; i < 100; i++) {
    int64_t val = i;
    multiarraySmallInsert(small, 0, &val);
}

/* Insert at tail (fast - no shifts) */
for (int i = 0; i < 100; i++) {
    int64_t val = i + 100;
    multiarraySmallInsert(small, small->count, &val);
}

/* Access is O(1) */
int64_t *val = (int64_t *)multiarraySmallGet(small, 50);
printf("Element 50: %ld\n", *val);

multiarraySmallFree(small);
```

## multiarrayMedium

### Structure

```c
struct multiarrayMedium {
    multiarrayMediumNode *node;  /* Array of data pointers */
    uint32_t count;              /* Number of nodes */
    uint16_t len;                /* Size of each element */
    uint16_t rowMax;             /* Max elements per node */
};

typedef struct multiarrayMediumNode {
    uint8_t *data;      /* Pointer to data chunk */
    uint16_t count;     /* Elements in this node */
} multiarrayMediumNode;

/* Total: 16 bytes + (count * 16 bytes) overhead */
```

### Architecture

```
┌────────────────────────────────────┐
│ multiarrayMedium (16 bytes)        │
├────────────────────────────────────┤
│ node ──────┐                       │
│ count = 3  │                       │
│ len = 16   │                       │
│ rowMax =512│                       │
└────────────┼───────────────────────┘
             │
             ▼
      ┌─────────────────┐
      │  Node Array     │
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

### How Medium Works

**Node-Based Storage:**

- Each node holds up to `rowMax` elements
- Nodes stored in a dynamic array for O(1) access
- Elements within each node are contiguous

**Finding an Element:**

```c
/* Pseudo-code for get operation */
void *multiarrayMediumGet(mar, idx) {
    int accum = 0;
    int nodeIdx = 0;

    /* Find which node contains idx */
    while ((accum + node[nodeIdx]->count) <= idx) {
        accum += node[nodeIdx]->count;
        nodeIdx++;
    }

    /* Index within the node */
    int localIdx = idx - accum;
    return node[nodeIdx]->data + (localIdx * mar->len);
}
```

**Node Splitting:**
When a node becomes full, insert creates a new node:

```c
if (node->count >= mar->rowMax) {
    /* Create new node */
    multiarrayMediumNode *newNode = allocateNode();

    if (insertingAtHead) {
        /* Insert new node before current */
        insertNodeBefore(newNode, currentNode);
    } else {
        /* Split current node into two */
        splitNode(currentNode, newNode);
    }
}
```

### Characteristics

**Advantages:**

- **Better insert performance**: Only moves elements within one node
- **Reduced reallocs**: Nodes grow independently
- **Chunked memory**: Better for large datasets
- **Moderate overhead**: 16 bytes per node

**Disadvantages:**

- **Two-level access**: Must find node, then element within node
- **More complex**: Harder to debug and reason about
- **Pointer indirection**: Extra memory access per lookup
- **Fragmentation**: Multiple small allocations

**Upgrade Trigger:**

```c
/* Upgrades to Large when: */
if ((sizeof(void *) * medium->count) > (sizeof(element) * rowMax)) {
    /* Pointer array is bigger than it would be in Small */
    /* Time to use linked list instead */
    upgrade_to_large();
}
```

### Best Use Cases

1. **Growing arrays** (1000 - 100K elements)
2. **Insert-heavy workloads** where splits are cheaper than large moves
3. **Chunked processing** where cache locality per chunk matters
4. **Moderate datasets** that exceed Small but don't need Large

### Example

```c
multiarrayMedium *medium = multiarrayMediumNew(sizeof(double), 512);

/* Insert 10,000 elements */
for (int i = 0; i < 10000; i++) {
    double val = i * 3.14;
    multiarrayMediumInsert(medium, medium->node[0].count, &val);
}

/* Medium now has ~20 nodes of 512 elements each */
printf("Nodes: %u\n", medium->count);

/* Access element 5000 */
double *val = (double *)multiarrayMediumGet(medium, 5000);
printf("Element 5000: %.2f\n", *val);

multiarrayMediumFree(medium);
```

## multiarrayLarge

### Structure

```c
struct multiarrayLarge {
    multiarrayLargeNode *head;   /* First node */
    multiarrayLargeNode *tail;   /* Last node */
    uint16_t len;                /* Size of each element */
    uint16_t rowMax;             /* Max elements per node */
};

typedef struct multiarrayLargeNode {
    uint8_t *data;          /* Pointer to data chunk */
    uint16_t count;         /* Elements in this node */
    uintptr_t prevNext;     /* XOR of prev and next pointers */
} multiarrayLargeNode;

/* Total: 24 bytes + (nodes * 24 bytes) overhead */
```

### Architecture

```
┌─────────────────────────────┐
│ multiarrayLarge (24 bytes)  │
├─────────────────────────────┤
│ head ──────┐                │
│ tail ──────┼──┐             │
│ len = 16   │  │             │
│ rowMax=1024│  │             │
└────────────┼──┼─────────────┘
             │  │
             │  │
    ┌────────┘  └───────────┐
    ▼                       ▼
  ┌────┐  XOR  ┌────┐  XOR  ┌────┐
  │Node│◄─────►│Node│◄─────►│Node│
  │  0 │ linked│  1 │ linked│  2 │
  └─┬──┘  list └─┬──┘  list └─┬──┘
    │           │           │
    ▼           ▼           ▼
  [data]      [data]      [data]
  (1024 elem) (1024 elem) (512 elem)
```

### XOR Linked List

Large uses **XOR doubly-linked lists** to save memory:

```c
/* Traditional doubly-linked list */
struct Node {
    Node *prev;  /* 8 bytes */
    Node *next;  /* 8 bytes */
    /* Total: 16 bytes per node for links */
};

/* XOR linked list */
struct XORNode {
    uintptr_t prevNext;  /* 8 bytes: XOR of prev and next */
    /* Total: 8 bytes per node for links - 50% savings! */
};

/* Traversal using XOR properties */
Node *getNext(Node *prev, Node *current) {
    return (Node *)(prev ^ current->prevNext);
}
```

**XOR Linked List Properties:**

- `current->prevNext = prev XOR next`
- Given `prev` and `current`, can find `next`: `next = prev XOR current->prevNext`
- Given `next` and `current`, can find `prev`: `prev = next XOR current->prevNext`
- Saves 8 bytes per node vs traditional doubly-linked list

### How Large Works

**Finding an Element:**

```c
/* Must traverse from head */
void *multiarrayLargeGet(mar, idx) {
    Node *prev = NULL;
    Node *current = mar->head;
    int accum = 0;

    /* Traverse until we find the node */
    while (accum + current->count <= idx) {
        accum += current->count;
        Node *next = XOR(prev, current->prevNext);
        prev = current;
        current = next;
    }

    /* Element is in 'current' node */
    int localIdx = idx - accum;
    return current->data + (localIdx * mar->len);
}
```

**Forward Iteration Optimization:**

```c
/* GetForward maintains state for sequential access */
void *multiarrayLargeGetForward(mar, idx) {
    /* Caches previous traversal state */
    /* If idx is sequential, continues from last position */
    /* Much faster than Get for iteration */
}
```

### Characteristics

**Advantages:**

- **Unlimited scaling**: No practical size limit
- **Memory efficient**: XOR links save 8 bytes per node
- **Large dataset support**: Designed for millions of elements
- **Head/Tail O(1)**: Direct access to ends

**Disadvantages:**

- **O(n) random access**: Must traverse from head
- **Complex traversal**: XOR pointer arithmetic
- **No backwards efficient**: Forward-only optimization
- **Higher base overhead**: 24 bytes + node overhead

**No Further Upgrades**: Large is the final form.

### Best Use Cases

1. **Massive arrays** (100K+ elements)
2. **Sequential access patterns** where forward iteration is common
3. **Memory-constrained environments** where XOR links save space
4. **Append/prepend heavy** workloads (O(1) at ends)

### Example

```c
multiarrayLarge *large = multiarrayLargeNew(sizeof(uint64_t), 1024);

/* Insert 1 million elements */
for (int i = 0; i < 1000000; i++) {
    uint64_t val = i;
    multiarrayLargeInsert(large, i, &val);
}

/* Head and tail access is O(1) */
uint64_t *first = (uint64_t *)multiarrayLargeGetHead(large);
uint64_t *last = (uint64_t *)multiarrayLargeGetTail(large);
printf("First: %lu, Last: %lu\n", *first, *last);

/* Random access requires traversal */
uint64_t *middle = (uint64_t *)multiarrayLargeGet(large, 500000);
printf("Middle: %lu\n", *middle);

/* Forward iteration is optimized */
multiarrayLargeNode *prev = NULL;
multiarrayLargeNode *current = large->head;
int count = 0;

while (current) {
    /* Process entire node */
    for (int i = 0; i < current->count; i++) {
        uint64_t *val = (uint64_t *)(current->data + i * large->len);
        count++;
    }

    /* Move to next node */
    multiarrayLargeNode *next = (multiarrayLargeNode *)(
        (uintptr_t)prev ^ current->prevNext
    );
    prev = current;
    current = next;
}

printf("Total elements: %d\n", count);

multiarrayLargeFree(large);
```

## Variant Comparison

### Memory Overhead (1000 elements, 16 bytes each)

```
Native:    0 + 16,000 = 16,000 bytes   (0.0% overhead)
Small:    16 + 16,000 = 16,016 bytes   (0.1% overhead)
Medium:   16 + (2×16) + 16,000 = 16,048 bytes  (0.3% overhead)
Large:    24 + (2×24) + 16,000 = 16,072 bytes  (0.4% overhead)

At 1000 elements, overhead is negligible.
```

### Memory Overhead (100,000 elements, 16 bytes each)

```
Native:   N/A (too large for single allocation typically)
Small:    16 + 1,600,000 = 1,600,016 bytes     (0.001% overhead)
Medium:   16 + (200×16) + 1,600,000 = 1,603,216 bytes  (0.2% overhead)
Large:    24 + (100×24) + 1,600,000 = 1,602,424 bytes  (0.15% overhead)

Large actually has less overhead than Medium at this scale!
```

### Performance Comparison

**Random Access (100K elements):**

```
Native:   N/A (would need upgrade)
Small:    O(1) ~5 ns     (single array index)
Medium:   O(1) ~15 ns    (node lookup + index)
Large:    O(n) ~500 ns   (traverse from head)
```

**Sequential Access (100K elements):**

```
Native:   N/A
Small:    ~0.5 ms total  (perfect cache locality)
Medium:   ~0.8 ms total  (cache misses between nodes)
Large:    ~1.0 ms total  (pointer chasing, but GetForward helps)
```

**Insertion at middle (100K elements):**

```
Native:   N/A
Small:    ~250 ms        (memmove 50K elements)
Medium:   ~2 ms          (memmove ~500 elements in one node)
Large:    ~50 ms         (traverse 50 nodes + memmove in node)
```

### When to Use Each Variant

```
       Native      Small       Medium       Large
         |          |            |            |
    0 ───┴── 100 ──┴─── 1000 ───┴─── 10000 ──┴─── ∞

         ← Manual Selection Possible →
         ← Automatic Transitions (Native API) →
```

**Decision Matrix:**

| Scenario                   | Best Variant  | Why                               |
| -------------------------- | ------------- | --------------------------------- |
| Fixed size < 1000          | Native        | Zero overhead, stack-friendly     |
| Dynamic, sequential access | Small         | Simple, cache-friendly            |
| 1K-100K, random access     | Medium        | Balanced overhead and performance |
| 100K+, sequential access   | Large         | XOR links save memory             |
| Frequent inserts/deletes   | Medium        | Per-node operations               |
| Unknown final size         | Native → auto | Starts cheap, grows optimally     |

## Transition Mechanics

### Native → Medium Transition

```c
/* Before: */
Entry *native = /* 1024 Entry array */
int count = 1024;

/* Trigger: count == rowMax */
Entry newItem = {...};
multiarrayNativeInsert(native, Entry, 1024, count, count, &newItem);

/* After: Native array converted to Medium */
multiarrayMedium {
    node[0] = {data: <original array>, count: 1024}
    node[1] = {data: <new 1-element array>, count: 1}
    count = 2
    len = sizeof(Entry)
    rowMax = 1024
}
```

### Medium → Large Transition

```c
/* Before: */
multiarrayMedium {
    node[0..99] = 100 nodes, each with 512 elements
    count = 100
}

/* Trigger: pointer array size > data size */
sizeof(void *) * 100 > sizeof(element) * rowMax

/* After: Converted to XOR linked list */
multiarrayLarge {
    head → node[0] ↔ node[1] ↔ ... ↔ node[99] ← tail
    Each node: {data, count, prevNext}
}
```

## Design Philosophy

The multiarray family follows the principle of **progressive enhancement**:

1. **Start minimal**: 0-byte Native for inline storage
2. **Grow when needed**: 16-byte Small for simple dynamics
3. **Scale efficiently**: Medium when chunking helps
4. **Support unlimited**: Large for massive datasets
5. **Transparent to user**: Same API throughout

This approach optimizes for the **common case** (small, sequential data) while supporting the **exceptional case** (massive, random access) without requiring users to make upfront decisions about expected dataset size.

## See Also

- [MULTIARRAY.md](MULTIARRAY.md) - Main API documentation
- [EXAMPLES.md](EXAMPLES.md) - Real-world usage patterns
- [databox](../core/DATABOX.md) - Universal container type
