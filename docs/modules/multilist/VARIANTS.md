# Multilist Variants - Small, Medium, and Full

## Overview

The multilist family provides three variants optimized for different data
scales. The system **automatically upgrades** between variants as your data
grows, so you get an appropriate layout at every scale without manual
intervention.

All three are reached through the same `multilist *` handle: the low 2 bits of
the pointer carry the variant tag, and the top two 8-bit fields carry the
compression depth and the size limit (`ptrlib.h` / `ptrlibCompress.h`).

## Quick Comparison

| Feature         | Small           | Medium           | Full                      |
| --------------- | --------------- | ---------------- | ------------------------- |
| **Struct size** | 8 bytes         | 16 bytes         | 24 bytes + node array     |
| **Node count**  | 1 flex          | 2 flexes         | N mflex nodes (dynamic)   |
| **Holds up to** | `limitBytes`    | `limitBytes * 3` | unbounded                 |
| **Compression** | No              | No               | Yes (LZ4, interior nodes) |
| **Ignores**     | state, readOnly | state, readOnly  | nothing                   |
| **Upgrades to** | Medium          | Full             | N/A (final)               |

`limitBytes` is `flexOptimizationSizeLimit[limit]` for the `flexCapSizeLimit`
you passed to `multilistNewLimit()`. With `FLEX_CAP_LEVEL_2048` that is 2 KB, so
Small holds up to 2 KB and Medium up to 6 KB; with `FLEX_CAP_LEVEL_512` the same
boundaries are 512 bytes and 1.5 KB.

## Architecture Details

### Small Variant

A Small list has no header: its `multilist *` is its flex, with the tier
in the pointer's low 2 bits and the depth and limit in its top 16 bits.
`multilistSmallInternal.h` holds the view each call builds on the stack
from the pointer:

```c
struct multilistSmall {
    flex *fl;
};
```

**Structure:**

- Single flex array containing all elements
- No header: one allocation, the flex
- No compression support (the `mflexState *` argument is discarded by
  `multilistSmallAdapter*` in `multilistAdapter.h`)
- Suited to small, short-lived lists

**Memory Layout:**

```
multilist * (tagged: the flex itself)
    |
    v
[flex: elem elem elem elem elem ...]
```

Calls that can move the flex take the handle (`multilist **`): pushes,
inserts, deletes, pops, replace, rotate, and writable iterators (whose
`multilistDelEntry()` stores the moved flex back through it).

**When to Use:**

- Temporary work queues
- Small caches
- Function-local lists
- Startup phase before data grows

**Automatic Upgrade:**
Upgrades to Medium when:

```c
flexBytes(fl) > flexOptimizationSizeLimit[limit]
```

### Medium Variant

`multilistMediumInternal.h`:

```c
struct multilistMedium {
    flex *fl[2];
};
```

**Structure:**

- Two flex arrays: `fl[0]` (head side) and `fl[1]` (tail side)
- Both flexes always exist; `multilistMediumCreate()` allocates two empty ones
- `fl[0]` always holds the list head whenever the list is non-empty
- `fl[1]` may be empty
- 16-byte struct, no compression support

**Memory Layout:**

```
multilistMedium (16 bytes)
    |
    +-- fl[0] -> [flex: elem elem elem ...]  (HEAD section)
    |
    +-- fl[1] -> [flex: elem elem elem ...]  (TAIL section)
```

**Balancing Rules (from `multilistMedium.c`):**

- `multilistMediumPushByTypeHead()` always pushes onto `fl[0]`
- `multilistMediumPushByTypeTail()` pushes onto `fl[1]`, _except_ when both
  flexes are empty - then the element goes to `fl[0]`, because `fl[0]` must
  hold elements whenever the list is non-empty
- After a pop, if `fl[0]` became empty while `fl[1]` still has data, the two
  pointers are swapped to restore that invariant
- The tail flex is `fl[flexCount(fl[1]) > 0]`
- There is no size-based rebalancing between the two flexes (the source marks
  it `todo`), so a head-heavy or tail-heavy access pattern leaves one flex
  much larger than the other until the list upgrades to Full

**When to Use:**

- Lists that outgrew Small but are still under `limitBytes * 3`
- Growing lists that have not hit full size yet
- Lists with mixed push head/tail patterns

**Automatic Upgrade:**
Upgrades to Full when:

```c
multilistMediumBytes(medium) > (flexOptimizationSizeLimit[limit] * 3)
```

### Full Variant

`multilistFull.c`:

```c
struct multilistFull {
    multiarray *node;  /* array nodes holding flexes */
    mlOffsetId values; /* total number of all values in all flexes */
    mlNodeId count;    /* number of mflexes */
    uint16_t fill;     /* fill factor for individual nodes */
    uint16_t compress; /* depth of end nodes not to compress; 0 = off */
};
```

**Structure:**

- Dynamic array of `mflex` nodes (managed flex, each a tagged pointer to either
  a plain flex or a compressed cflex)
- Interior nodes can be LZ4 compressed
- Compression depth controls which nodes are eligible
- 24-byte struct plus the `multiarray` of node pointers

**Memory Layout:**

```
multilistFull (24 bytes)
    |
    +-- node -> multiarray [
                  mflex*,    // Node 0 (head) - never compressed
                  mflex*,    // Node 1
                  mflex*,    // Node 2 - may be compressed
                  ...
                  mflex*,    // Node N-1
                  mflex*     // Node N (tail) - never compressed
                ]
    |
    +-- values: total element count
    +-- count: number of nodes
    +-- fill: flexCapSizeLimit level used to decide node splits
    +-- compress: depth from the ends to leave uncompressed
```

The node array is a `multiarray`, which promotes its own backing storage as it
grows; `STORAGE_MAX` in `multilistFull.c` is 2048 node pointers per chunk.

**Compression Depth:**

```
compress = 0:  No compression
compress = 1:  Head and tail nodes uncompressed
compress = 2:  First 2 and last 2 nodes uncompressed
compress = N:  First N and last N nodes uncompressed
```

Head and tail nodes are never compressed regardless of depth, and when
`count <= compress * 2` nothing is compressed at all.

**When to Use:**

- Lists larger than `limitBytes * 3`
- Long-lived data structures
- When compression would save space
- Lists that may grow to millions of elements

**No Further Upgrades:**
Full is the final variant and grows without a built-in bound.

## Detailed Comparison

### Memory Overhead

**Small:**

```
8 bytes (single pointer)
+ one flex header (FLEX_EMPTY_SIZE is 2 bytes for an empty flex)
```

**Medium:**

```
16 bytes (two pointers)
+ two flex headers
```

**Full:**

```
24 bytes (struct)
+ multiarray of node pointers (8 bytes per node, chunked)
+ each node's own flex (or compressed cflex) header
```

No variant stores per-element `prev`/`next` pointers - that is the whole point
of packing elements into flex nodes.

### Performance Characteristics

**Push Head/Tail:**

- **Small:** O(1) amortized into the single flex; O(n) when the flex grows or
  when the byte threshold triggers the upgrade to Medium
- **Medium:** O(1) into the target flex; O(n) on reallocation or upgrade
- **Full:** O(1) amortized; a push that would exceed `fill` for the end node
  first inserts a fresh empty node

**Pop Head/Tail:**

- **Small:** O(1)
- **Medium:** O(1), possibly followed by a pointer swap of `fl[0]`/`fl[1]`
- **Full:** O(1), possibly followed by deleting an emptied node

**Index Access:**

- **Small:** O(n) - scan the single flex
- **Medium:** O(n) - pick the flex by count, then scan
- **Full:** O(n) - walk the node array to find the containing node,
  decompress if needed, then scan inside it

**Iteration:**

- **Small:** O(n) - single flex traversal
- **Medium:** O(n) - traverse `fl[0]` then `fl[1]`
- **Full:** O(n) - traverse all nodes, decompressing as needed

### Insert Implementations

**Small** (`multilistSmall.c`) - one flex, nothing to decide:

```c
void multilistSmallPushByTypeTail(multilistSmall *ml, const databox *box) {
    flexPushByType(&ml->fl, box, FLEX_ENDPOINT_TAIL);
}
```

**Medium** (`multilistMedium.c`) - keep `fl[0]` populated:

```c
void multilistMediumPushByTypeTail(multilistMedium *ml, const databox *box) {
    /* F0 must hold elements whenever the list is nonempty, so the
     * first element of an empty list goes to F0, not F1 */
    if (flexCount(F0) == 0 && flexCount(F1) == 0) {
        flexPushByType(&F0, box, FLEX_ENDPOINT_TAIL);
    } else {
        flexPushByType(&F1, box, FLEX_ENDPOINT_TAIL);
    }
}
```

**Full** (`multilistFull.c`) - make room, then push into the tail node:

```c
void multilistFullPushByTypeTail(multilistFull *ml, mflexState *state,
                                 const databox *box) {
    /* If the tail node cannot absorb DATABOX_SIZE(box) under 'fill',
     * this inserts a new empty node after the current tail and
     * recompresses whatever fell outside the compression depth. */
    (void)_multilistFullMflexForTail(ml, state, DATABOX_SIZE(box));

    mflexPushByType(mlTailPtr(ml), state, box, FLEX_ENDPOINT_TAIL);

    ml->values++;
}
```

The capacity test itself is `flexCapAllowInsert(mflexBytesUncompressed(node),
ml->fill, bytes)`, which accounts for flex encoding overhead in both directions
before comparing against `flexOptimizationSizeLimit[fill]`.

## Upgrade Mechanics

Both upgrades happen in `multilistUpgradeIfNecessary()` (`multilist.c`), called
at the end of every push and insert.

### Small to Medium Upgrade

**Trigger:**

```c
if (multilistSmallBytes(small) > flexOptimizationSizeLimit[limit]) {
    /* upgrade */
}
```

**Process:**

1. `zrealloc()` the 8-byte Small struct up to the 16-byte Medium struct
   (the allocation is grown in place, not copied to a fresh struct)
2. `fl` becomes `fl[0]`
3. `flexSplit(&fl[0], 1)` splits at the flex middle and returns the second half
   as `fl[1]`; if the flex held a single element, `fl[1]` is a new empty flex
4. Re-tag the pointer as Medium, preserving the depth/limit high bits

**Example:**

```
Before (Small):
[flex: a b c d e f g h]

After (Medium):
fl[0]: [flex: a b c d]
fl[1]: [flex: e f g h]
```

### Medium to Full Upgrade

**Trigger:**

```c
if (multilistMediumBytes(medium) > (flexOptimizationSizeLimit[limit] * 3)) {
    /* upgrade */
}
```

**Process:**

1. `multilistFullNewFromFlexConsumeGrow()` reallocs the Medium struct into the
   24-byte Full struct, records `fill` and `compress`, and takes ownership of
   both flexes
2. Each incoming flex holding more than one element is itself split at its
   middle, so a two-flex Medium becomes up to **four** nodes (an empty flex is
   freed and adds no node)
3. Each resulting flex is converted with `mflexConvertFromFlexNoCompress()` and
   appended to the node array
4. The depth rule is applied once over all the nodes: the `compress` nodes at
   each end stay uncompressed and every node between them is compressed. The
   function takes any number of input flexes, not just the Medium tier's two.
5. Re-tag the pointer as Full, preserving the depth/limit high bits

**Example:**

```
Before (Medium):
fl[0]: [flex: a b c d e f]
fl[1]: [flex: g h i j k l]

After (Full):
node[0]: [mflex: a b c]  (head, never compressed)
node[1]: [mflex: d e f]
node[2]: [mflex: g h i]
node[3]: [mflex: j k l]  (tail, never compressed)
```

### Upgrade Transparency

The upgrade process is **transparent** to the caller - the only requirement is
passing `multilist **` so the new tagged pointer can be written back:

```c
multilist *ml = multilistNewLimit(FLEX_CAP_LEVEL_2048, 0);
mflexState *state = mflexStateCreate();

/* Starts as Small */
for (uint32_t i = 0; i < 100; i++) {
    databox item = databoxNewBytesString(someData);
    multilistPushByTypeTail(&ml, state, &item);
}

/* Crossing 2 KB of stored bytes makes it Medium */
for (uint32_t i = 0; i < 200; i++) {
    databox item = databoxNewBytesString(someData);
    multilistPushByTypeTail(&ml, state, &item);
}

/* Crossing 6 KB makes it Full */
for (uint32_t i = 0; i < 1000; i++) {
    databox item = databoxNewBytesString(someData);
    multilistPushByTypeTail(&ml, state, &item);
}

multilistFree(ml);  /* Works on any variant */
mflexStateFree(state);
```

Upgrades are one-way: shrinking a Full list back under the thresholds does not
downgrade it.

## Compression (Full Variant Only)

### How Compression Works

The Full variant uses LZ4 compression (through `mflex`/`cflex`) on interior
nodes:

```
compress = 2:

node[0]:   [UNCOMPRESSED] - head (fast access)
node[1]:   [UNCOMPRESSED] - near head
node[2]:   [COMPRESSED]   - interior
...
node[N-2]: [COMPRESSED]   - interior
node[N-1]: [UNCOMPRESSED] - near tail
node[N]:   [UNCOMPRESSED] - tail (fast access)
```

Compression is applied incrementally: after a structural change,
`multilistFullCompressRenew__()` compresses the node just past the depth
boundary at each end rather than sweeping the whole list. Nodes are also
decompressed on demand when an operation needs to read or modify them.

### Compression Trade-offs

- Compressed nodes must be decompressed before access, into the caller's
  `mflexState` scratch buffers
- Head and tail stay uncompressed, so push/pop at the ends never pay for it
- Incompressible data stays uncompressed (LZ4 keeps the literal form)
- `multilistBytes()` reports **uncompressed** bytes, so it will not show the
  saving; there is no public accessor for the compressed footprint

### Choosing Compression Depth

```c
/* No compression - fastest */
multilist *none = multilistNewLimit(FLEX_CAP_LEVEL_4096, 0);

/* Compress everything except head and tail - most space saved */
multilist *aggressive = multilistNewLimit(FLEX_CAP_LEVEL_4096, 1);

/* Keep 2 nodes hot at each end */
multilist *balanced = multilistNewLimit(FLEX_CAP_LEVEL_4096, 2);

/* Keep 10 nodes hot at each end - very little gets compressed */
multilist *shallow = multilistNewLimit(FLEX_CAP_LEVEL_4096, 10);
```

**Guidelines:**

- `depth = 0`: no compression at all (maximum speed)
- `depth = 1`: compresses the most nodes (maximum space savings)
- `depth = 2-5`: balanced; good for lists accessed mostly near the ends
- larger depths: progressively less compression, since a list with
  `count <= depth * 2` nodes compresses nothing

The depth is stored in an 8-bit field of the tagged pointer, so 0-255 is the
usable range from `multilistNewLimit()`.

## Choosing the Right Variant

### You Don't Choose - The System Does

You do not create Small, Medium, or Full directly. You always use the unified
`multilist` API and the system upgrades as needed:

```c
/* Correct */
multilist *ml = multilistNewLimit(FLEX_CAP_LEVEL_2048, 0);
/* becomes Small -> Medium -> Full as it grows */
```

What you _do_ choose is the size limit and the compression depth, and those two
choices fix the byte thresholds at which the upgrades happen:

```
limit level        Small holds up to     Medium holds up to
FLEX_CAP_LEVEL_512      512 B                  1.5 KB
FLEX_CAP_LEVEL_2048       2 KB                    6 KB
FLEX_CAP_LEVEL_8192       8 KB                   24 KB
```

### About Direct Variant Construction

The per-variant headers do expose constructors (`multilistSmallCreate()`,
`multilistMediumCreate()`, `multilistFullCreate()`, `multilistFullNew()`), but
there is **no public way to wrap one in a `multilist *` handle**: the tagging
macros (`_MULTILIST_TAG`, `SET_COMPRESS_DEPTH_LIMIT`) are private to
`multilist.c`. A directly-created variant can only be used through that
variant's own API, and it will never auto-upgrade.

Application code should use `multilistNewLimit()`.

## Variant-Specific Functions

Each variant has its own header with its own signatures. Note that Small and
Medium take no `mflexState`, while Full takes one (or two):

### Small-Specific (`multilistSmall.h`)

```c
multilistSmall *multilistSmallCreate(void);
void multilistSmallPushByTypeHead(multilistSmall *ml, const databox *box);
void multilistSmallPushByTypeTail(multilistSmall *ml, const databox *box);
bool multilistSmallPop(multilistSmall *ml, databox *box, bool fromTail);
bool multilistSmallIndex(multilistSmall *ml, mlOffsetId index,
                         multilistEntry *entry);
void multilistSmallRotate(multilistSmall *ml);
flex *multilistSmallDump(const multilistSmall *ml);
```

### Medium-Specific (`multilistMedium.h`)

```c
multilistMedium *multilistMediumCreate(void);
void multilistMediumPushByTypeHead(multilistMedium *ml, const databox *box);
void multilistMediumPushByTypeTail(multilistMedium *ml, const databox *box);
bool multilistMediumPop(multilistMedium *ml, databox *box, bool fromTail);
void multilistMediumRotate(multilistMedium *ml);
flex *multilistMediumDump(const multilistMedium *ml);
```

### Full-Specific (`multilistFull.h`)

```c
multilistFull *multilistFullCreate(void);
multilistFull *multilistFullNew(uint32_t fill, uint32_t compress);
void multilistFullPushByTypeHead(multilistFull *ml, mflexState *state,
                                 const databox *box);
void multilistFullPushByTypeTail(multilistFull *ml, mflexState *state,
                                 const databox *box);
bool multilistFullPop(multilistFull *ml, mflexState *state, databox *box,
                      bool fromTail);
void multilistFullRotate(multilistFull *ml, mflexState *state[2]);
flex *multilistFullDump(const multilistFull *ml, mflexState *state);
void multilistFullSetCompressDepth(multilistFull *ml, uint32_t depth);
void multilistFullSetCompressDepthWithState(multilistFull *ml,
                                            mflexState *state, uint32_t depth);
void multilistFullAppendFlex(multilistFull *ml, flex *fl);
void multilistFullAppendFlexWithState(multilistFull *ml, mflexState *state,
                                      flex *fl);
```

Setting the depth of a populated list re-applies the depth rule to every node,
and `AppendFlex` compresses the node the new tail pushes into the interior. The
plain forms borrow a temporary `mflexState` only when a node actually has to be
compressed or decompressed; the `WithState` forms use the caller's.

Every tier's `Pop` returns an owned copy of the removed element (see
`multilistPop` in [MULTILIST.md](MULTILIST.md)), and every tier's `Dump`
returns its elements as one new flex (see `multilistDump`).

`multilistAdapter.h` exists precisely to paper over this signature mismatch: it
defines `multilistSmallAdapter*` / `multilistMediumAdapter*` macros that accept
the Full-shaped argument list and discard the state, `readOnly`, and `openNode`
parameters, so `multilist.c` can dispatch all three variants through one macro.
Every adapter macro names a defined function. Small and Medium never compress
and have no fill limit of their own (the owning `multilist` keeps both in its
tagged pointer), so their `New(fill, compress)`, `SetCompressDepth`, `SetFill`
and `SetOptions` accept the values for signature parity and ignore them. Full's
`multilistFullSetOptions` is `multilistFullSetDepth` (fill, then depth), and
`multilistFullAppendValuesFlex` / `multilistFulllCreateFlex` are the same calls
as `multilistFullAppendValuesFromFlex` / `multilistFullNewFromFlex`.

**Note:** direct variant functions are primarily for internal use. Application
code should use the unified `multilist` API.

## Performance Expectations

There is no published multilist benchmark in
[PERFORMANCE_BASELINES.md](../../PERFORMANCE_BASELINES.md) yet, so this file
deliberately carries no timing table. What the structure guarantees is
structural, not numeric:

- No per-element `prev`/`next` pointers in any variant
- Elements of a node are contiguous, so iteration is a linear memory walk
- Values are type/varint encoded by flex rather than stored in fixed 8-byte
  slots, so small integers and short strings cost far less than a machine word
  plus pointers
- Head/tail work touches only the end node, whatever the list length

Measure your own workload with the cmake `perf` preset (sanitizer builds
distort these numbers badly) before committing to a size limit or a compression
depth.

## Best Practices

### 1. Always Use the Unified API

```c
/* GOOD */
multilist *ml = multilistNewLimit(FLEX_CAP_LEVEL_2048, 0);
multilistPushByTypeTail(&ml, state, &item);

/* AVOID: cannot auto-upgrade, cannot be used as a multilist * */
multilistSmall *mls = multilistSmallCreate();
multilistSmallPushByTypeTail(mls, &item);
```

### 2. Set Appropriate Size Limits

```c
/* Small lists, or lists with frequent mid-node inserts: smaller nodes
 * mean shorter memmoves */
multilist *small = multilistNewLimit(FLEX_CAP_LEVEL_512, 0);

/* Large append-mostly lists: bigger nodes amortize more overhead */
multilist *large = multilistNewLimit(FLEX_CAP_LEVEL_8192, 2);
```

### 3. Use Compression Wisely

```c
/* No compression for speed-critical code */
multilist *fast = multilistNewLimit(FLEX_CAP_LEVEL_2048, 0);

/* Compression for memory-critical code with cold interiors */
multilist *compact = multilistNewLimit(FLEX_CAP_LEVEL_4096, 2);
```

Compression only ever applies once a list has become Full and has more than
`depth * 2` nodes, so it does nothing for small lists.

### 4. Trust the Automatic Upgrades

```c
multilist *ml = multilistNewLimit(FLEX_CAP_LEVEL_2048, 0);

/* Add 1 element or 1 million - it adapts */
for (uint32_t i = 0; i < 1000000; i++) {
    multilistPushByTypeTail(&ml, state, &item);
}
```

## See Also

- [MULTILIST.md](MULTILIST.md) - Main API documentation
- [EXAMPLES.md](EXAMPLES.md) - Module-specific walkthroughs
- [flex](../flex/FLEX.md) - Underlying storage for Small/Medium
- [mflex](../flex/MFLEX.md) - Managed flex with compression (Full)
- [multiarray](../multiarray/MULTIARRAY.md) - Dynamic node array used by Full
