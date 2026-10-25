# multiroar - Roaring Bitmap Implementation

## Overview

`multiroar` is a **space-efficient compressed bitmap** implementation based on the Roaring Bitmap algorithm. It stores sparse sets of integers using adaptive encoding that automatically switches between packed arrays, bitmaps, and inverted bitmaps depending on density.

**Key Features:**

- Automatic encoding selection (sparse/dense/inverted/all-ones)
- 8192-bit chunks for optimal memory usage
- Packed 13-bit position arrays for sparse data
- Full bitmaps for dense data
- Inverted position arrays for nearly-full chunks
- Set operations (AND, OR, XOR, NOT, ANDNOT) in-place and copy forms, plus
  N-way variants
- Predicates (`Equals`, `IsSubset`, `Intersects`, `IsEmpty`) and extrema
  (`Min`, `Max`)
- Succinct primitives: `Rank`, `Select`, `RangeCount`
- Forward iterator over set positions
- Bulk get/set, array conversion, serialization
- Similarity metrics (Jaccard, Hamming distance, overlap, Dice)
- Built on multimap for chunk lookup
- Value mode: sparse `position -> fixed-width unsigned value` maps
  (see [Value Operations](#value-operations))

All positions are `uint64_t`.

**Headers**: `multiroar.h`

**Source**: `multiroar.c`

## Architecture

### Roaring Bitmap Algorithm

Roaring bitmaps divide the integer space into **chunks** of fixed size (8192 bits = 1024 bytes per chunk):

```
Position: 0        8191 8192      16383 16384     24575
          |-----------|  |-----------|  |-----------|
          Chunk 0        Chunk 1        Chunk 2
```

Each chunk is stored using one of several encodings based on density:

```
Sparse (< 629 set bits):     Packed array of positions
  [Type:1][Count:1-2][Pos1:13bits][Pos2:13bits]...

Dense (629-7563 set bits):   Full 1024-byte bitmap
  [Type:1][Bitmap:8192bits]

Nearly Full (> 7563 bits):   Inverted packed array (unset positions)
  [Type:1][Count:1-2][NotPos1:13bits][NotPos2:13bits]...

All Set:                     Single type byte
  [Type:1]

All Zero:                    No entry in multimap
  (implicit)
```

### Chunk Layout

```
multimap (chunk_number -> encoded_chunk)
  ├─> Chunk 0: [sparse: positions 17, 42, 99]
  ├─> Chunk 5: [dense: 1024-byte bitmap]
  ├─> Chunk 7: [all-ones: 1 byte]
  └─> Chunk 9: [inverted: unset positions 13, 99]

Missing chunks are implicitly all-zero.
```

## Data Structures

### Main Structure

`multiroar` is an **opaque handle** — `multiroar.h` only exposes
`typedef struct multiroar multiroar;`. The definition lives in `multiroar.c`
and must not be reached into by callers.

Internally it is a `multimap *` (chunk id -> encoded chunk) followed by a
flexible metadata array holding the element bit width, then column and row
counts as varints (3 to 19 bytes total). Columns are stored before rows
because a plain bitmap only ever consults the column count.

### Chunk Types (internal)

```c
typedef enum chunkType {
    CHUNK_TYPE_ALL_0 = 0,     /* Implicit (no entry) */
    CHUNK_TYPE_ALL_1,          /* All bits set */
    CHUNK_TYPE_UNDER_FULL_DIRECT_POSITION_NUMBERS,     /* Sparse */
    CHUNK_TYPE_FULL_BITMAP,    /* Dense bitmap */
    CHUNK_TYPE_OVER_FULL_DIRECT_NOT_SET_POSITION_NUMBERS,  /* Inverted */
    CHUNK_TYPE_MAX_TYPE = 255
} chunkType;
```

### Constants (internal, `multiroar.c`)

```c
#define BITMAP_SIZE_IN_BITS 8192   /* 8192 bits per chunk */
#define BITMAP_SIZE_IN_BYTES (BITMAP_SIZE_IN_BITS / 8)  /* 1024 */
#define DIRECT_STORAGE_BITS 13     /* 13 bits per packed position */

/* (8192 / 13) - 1 == 629 */
#define MAX_ENTRIES_PER_DIRECT_LISTING                                         \
    ((BITMAP_SIZE_IN_BITS / DIRECT_STORAGE_BITS) - 1)

/* 8192 - 629 == 7563 */
#define MAX_BITMAP_ENTIRES_BEFORE_NEGATIVE_LISTING                             \
    (BITMAP_SIZE_IN_BITS - MAX_ENTRIES_PER_DIRECT_LISTING)
```

These are computed, not spelled out, in the source: `8192 / 13` truncates to
`630`, so `MAX_ENTRIES_PER_DIRECT_LISTING` is **629** and
`MAX_BITMAP_ENTIRES_BEFORE_NEGATIVE_LISTING` is **7563**.

## Creation Functions

```c
/* Create new bitmap (for tracking set bits) */
multiroar *multiroarBitNew(void);

/* Create value-mode multiroar (position -> bitWidth-bit values;
 * see Value Operations below) */
multiroar *multiroarValueNew(uint8_t bitWidth, uint64_t rows, uint64_t cols);

/* Duplicate existing roar */
multiroar *multiroarDuplicate(const multiroar *r);

/* Free multiroar */
void multiroarFree(multiroar *r);

/* Example: Create bitmap */
multiroar *r = multiroarBitNew();

/* Example: Create 2D value map (8-bit values, 1000x1000) */
multiroar *matrix = multiroarValueNew(8, 1000, 1000);
```

## Bit Operations

### Set Bits

```c
/* Set bit at position; returns the PREVIOUS state of the bit */
bool multiroarBitSet(multiroar *r, uint64_t position);

/* Set range [start, start + extent) */
void multiroarBitSetRange(multiroar *r, uint64_t start, uint64_t extent);

/* Clear range [start, start + extent) */
void multiroarBitClearRange(multiroar *r, uint64_t start, uint64_t extent);

/* Flip range [start, start + extent) */
void multiroarBitFlipRange(multiroar *r, uint64_t start, uint64_t extent);

/* Example: Set individual bits */
multiroar *r = multiroarBitNew();

bool was_set = multiroarBitSet(r, 1000);
assert(!was_set);  /* First time setting */

was_set = multiroarBitSet(r, 1000);
assert(was_set);   /* Already set */

/* Example: Set range */
multiroarBitSetRange(r, 5000, 1000);  /* Sets bits 5000-5999 */
```

### Get Bits

```c
/* Check if bit is set */
bool multiroarBitGet(const multiroar *r, uint64_t position);

/* Example: Check membership */
if (multiroarBitGet(r, 1000)) {
    printf("Bit 1000 is set\n");
}
```

### Remove Bits

```c
/* Clear bit at position; returns true if the bit was previously set */
bool multiroarRemove(multiroar *r, uint64_t position);

/* Example */
multiroarBitSet(r, 500);
multiroarRemove(r, 500);
assert(!multiroarBitGet(r, 500));
```

### Cardinality, Extrema, Rank/Select

```c
/* Total set bits */
uint64_t multiroarBitCount(const multiroar *r);

/* True when no bits are set anywhere */
bool multiroarIsEmpty(const multiroar *r);

/* First / last set bit; false when the set is empty */
bool multiroarMin(const multiroar *r, uint64_t *position);
bool multiroarMax(const multiroar *r, uint64_t *position);

/* Count set bits in [0, position) */
uint64_t multiroarRank(const multiroar *r, uint64_t position);

/* Find the k-th set bit (1-indexed); false when k > BitCount */
bool multiroarSelect(const multiroar *r, uint64_t k, uint64_t *position);

/* Count set bits in [start, end) */
uint64_t multiroarRangeCount(const multiroar *r, uint64_t start, uint64_t end);

/* Example: median-ish position of a set */
uint64_t total = multiroarBitCount(r);
uint64_t middle = 0;
if (total && multiroarSelect(r, total / 2, &middle)) {
    printf("middle set bit: %" PRIu64 "\n", middle);
}
```

### Comparison Predicates

```c
bool multiroarIntersects(const multiroar *a, const multiroar *b);
bool multiroarIsSubset(const multiroar *a, const multiroar *b);
bool multiroarEquals(const multiroar *a, const multiroar *b);
```

### Iteration

```c
typedef struct multiroarIterator {
    const multiroar *roar;
    multimapIterator mapIter;
    databox currentChunk;
    uint64_t chunkId;
    uint64_t positionInChunk;
    uint16_t countInChunk;
    uint16_t indexInChunk;
    uint16_t unsetCursor;
    bool valid;
} multiroarIterator;

void multiroarIteratorInit(const multiroar *r, multiroarIterator *iter);
bool multiroarIteratorNext(multiroarIterator *iter, uint64_t *position);
void multiroarIteratorReset(multiroarIterator *iter);

/* Example: walk every set position in ascending order */
multiroarIterator iter;
multiroarIteratorInit(r, &iter);

uint64_t position;
while (multiroarIteratorNext(&iter, &position)) {
    printf("set: %" PRIu64 "\n", position);
}
```

The iterator is stack-allocated and requires no teardown.

### Bulk and Array Conversion

```c
void multiroarBitSetMany(multiroar *r, const uint64_t *positions,
                         uint64_t count);
void multiroarBitGetMany(const multiroar *r, const uint64_t *positions,
                         uint64_t count, bool *results);

/* Drain up to maxCount set positions (ascending) into 'positions'.
 * Returns how many were written. */
uint64_t multiroarToArray(const multiroar *r, uint64_t *positions,
                          uint64_t maxCount);

/* Build a new bitmap from an array of positions (NULL when count == 0) */
multiroar *multiroarFromArray(const uint64_t *positions, uint64_t count);
```

`multiroarToArray` is implemented on top of the iterator, so it stops at
`maxCount` rather than reporting the full cardinality. Size the buffer with
`multiroarBitCount()` first when you need everything.

### Similarity Metrics

```c
double multiroarJaccard(const multiroar *a, const multiroar *b);
uint64_t multiroarHammingDistance(const multiroar *a, const multiroar *b);
double multiroarOverlap(const multiroar *a, const multiroar *b);
double multiroarDice(const multiroar *a, const multiroar *b);
```

### Memory and Serialization

```c
uint64_t multiroarMemoryUsage(const multiroar *r);

/* Size the buffer, serialize into it, restore from it */
uint64_t multiroarSerializedSize(const multiroar *r);
uint64_t multiroarSerialize(const multiroar *r, void *buf, uint64_t bufSize);
multiroar *multiroarDeserialize(const void *buf, uint64_t bufSize);

/* Example: round trip */
const uint64_t needed = multiroarSerializedSize(r);
uint8_t *buf = zmalloc(needed);
const uint64_t written = multiroarSerialize(r, buf, needed);
assert(written > 0);

multiroar *restored = multiroarDeserialize(buf, written);
assert(multiroarEquals(r, restored));

multiroarFree(restored);
zfree(buf);
```

Both modes serialize. `multiroarSerialize` writes the `multiroarFormat`
dkFormat prefix (format id, version, features), then the stream: a mode byte
(a value roar's bit width, 0 for bit mode), then for value mode the cols,
rows, value count and each value after the gap from the previous position,
and for bit mode a varint chunk count and the encoded chunks. The `Raw`
forms (`multiroarSerializeRaw`, `multiroarDeserializeRaw`,
`multiroarSerializedSizeRaw`) are the stream alone, for a container that
records the stamp itself, as persist does. Deserialize refuses malformed
bytes, including trailing bytes and values that break the roar's width or
dimensions.

## Value Operations

`multiroarValueNew(bitWidth, rows, cols)` creates a **value-mode** multiroar:
a sparse map of `position -> unsigned value`, where every value must fit the
declared `bitWidth` (1-64; out-of-range widths return NULL). `rows`/`cols`
declare the addressable space — when both are nonzero, positions must be
`< rows * cols`; either being 0 means unbounded.

```c
typedef enum multiroarValueSetResult {
    MULTIROAR_VALUE_REFUSED = 0, /* nothing stored */
    MULTIROAR_VALUE_ADDED,       /* the position held no value */
    MULTIROAR_VALUE_REPLACED,    /* the position's value was replaced */
} multiroarValueSetResult;

multiroarValueSetResult multiroarValueSet(multiroar *r, uint64_t position,
                                          databox *value);
multiroarValueSetResult multiroarValueSetGetPrevious(
    multiroar *r, uint64_t position, databox *value, databox *overwritten);
bool multiroarValueGet(const multiroar *r, uint64_t position, databox *value);
bool multiroarValueRemoveGetRemoved(multiroar *r, uint64_t position,
                                    databox *removed);
```

Contracts:

- Values are unsigned integers (signed inputs must be `>= 0`; booleans store
  as 0/1) and round-trip as unsigned databoxes.
- `Set`/`SetGetPrevious` report ADDED or REPLACED, or REFUSED (nothing
  changes) on a bit-mode roar, outside the declared `rows * cols`, or for a
  value that cannot be stored (not an integer or boolean, negative, wider than
  `bitWidth`, NULL). `Get`/`RemoveGetRemoved` return whether the position
  held a value, and false in those cases.
- Each call is one search of the position map: `SetGetPrevious` reports the
  overwritten value from the replacing insert, and `RemoveGetRemoved` the
  removed one from the delete.

## Modes

A bit-mode multiroar comes from `multiroarBitNew()`; a value-mode one from
`multiroarValueNew()`. Their rows share one keyspace, so every operation
belongs to one mode and refuses a roar of the other with a single check:

- Bit calls on a value roar answer `false`, 0 or NULL, or change nothing:
  set/get/remove, ranges, counts, min/max, rank/select, iteration, batch
  set/get, `ToArray`, comparisons, every set operation (in place, `New*`,
  N-way) and the similarity metrics.
- Value calls on a bit roar are refused (`MULTIROAR_VALUE_REFUSED` / false).
- Each such refusal also reports a `DK_DIAG_MULTIROAR_WRONG_MODE` misuse
  diagnostic naming the function ([DIAGNOSTICS.md](../core/DIAGNOSTICS.md)).
  A value a roar cannot store, or a position past its dimensions, is
  refused quietly: that is data, not a mode mix-up.
- Mode-free calls take either: `Free`, `Duplicate`, `Clear`, `IsEmpty`,
  `MemoryUsage`, `Serialize*` / `Deserialize*`, and `Equals` (two value
  roars are equal when width, dimensions and every row match; a bit roar
  never equals a value roar).
- `multiroarDuplicate` preserves value-mode metadata (bit width and bounds).
- `multiroarValueWidth` (0 for a bit-mode roar), `multiroarValueDimensions`,
  `multiroarValueCount` and `multiroarValueInBounds` describe a value roar;
  `multiroarClear` empties either mode, keeping width and dimensions.
- Persistence: `multiroarValueNewP`, `multiroarValueSetP`,
  `multiroarValueRemoveP`, `multiroarValueGetP` and `multiroarValueCountP`
  (`persist/multiroarP.h`) log value ops; a value that does not fit is
  refused (false) rather than logged.

```c
multiroar *m = multiroarValueNew(16, 1000, 1000);
databox v = databoxNewUnsigned(4242);
multiroarValueSet(m, 7, &v);

databox got = {{0}};
if (multiroarValueGet(m, 7, &got)) {
    /* got.data.u == 4242 */
}

multiroarFree(m);
```

## Set Operations

### In-Place Operations

```c
/* Modify first argument in-place */
void multiroarXor(multiroar *r, multiroar *b);     /* r = r XOR b */
void multiroarAnd(multiroar *r, multiroar *b);     /* r = r AND b */
void multiroarOr(multiroar *r, multiroar *b);      /* r = r OR b */
void multiroarAndNot(multiroar *r, multiroar *b);  /* r = r AND NOT b */
void multiroarNot(multiroar *r);                   /* r = NOT r */

/* Example: Set intersection */
multiroar *set1 = multiroarBitNew();
multiroar *set2 = multiroarBitNew();

multiroarBitSet(set1, 100);
multiroarBitSet(set1, 200);
multiroarBitSet(set2, 200);
multiroarBitSet(set2, 300);

multiroarAnd(set1, set2);
/* set1 now contains only {200} */
```

### New Result Operations

```c
/* Create new multiroar with result */
multiroar *multiroarNewXor(const multiroar *r, const multiroar *b);
multiroar *multiroarNewAnd(const multiroar *r, const multiroar *b);
multiroar *multiroarNewOr(const multiroar *r, const multiroar *b);
multiroar *multiroarNewAndNot(const multiroar *a, const multiroar *b);
multiroar *multiroarNewNot(const multiroar *r);

/* Example: Set union */
multiroar *result = multiroarNewOr(set1, set2);
/* set1 and set2 unchanged, result has union */
```

Every `New*` form is implemented as duplicate-then-apply, so it costs one
full copy of the first operand plus the in-place operation.

### Bounded Complement

`multiroarNot` complements **only the chunks that already exist** in `r`.
It never materializes chunks for an unbounded universe, so
`multiroarNot(empty)` is a no-op and `NOT` of a sparse set only flips bits
inside the 8192-bit windows that set has touched. For set difference, use
`multiroarAndNot` / `multiroarNewAndNot` rather than `NOT` followed by `AND`.

### N-Way Operations

```c
/* Fold N roars into roars[0] (N >= 2) */
void multiroarAndN(uint64_t n, multiroar **roars);
void multiroarOrN(uint64_t n, multiroar **roars);
void multiroarXorN(uint64_t n, multiroar **roars);

/* Fold N roars into a fresh result, leaving inputs untouched */
multiroar *multiroarNewAndN(uint64_t n, multiroar **roars);
multiroar *multiroarNewOrN(uint64_t n, multiroar **roars);
multiroar *multiroarNewXorN(uint64_t n, multiroar **roars);

/* Example: intersect four posting lists in one pass */
multiroar *lists[4] = {a, b, c, d};
multiroar *hits = multiroarNewAndN(4, lists);
```

The N-way forms collect the union of chunk ids once and process each chunk
across all inputs, which beats repeated pairwise calls that re-walk the
chunk index every time.

## Encoding Behavior

### Sparse Encoding

When a chunk has **fewer than 629 set bits**, it uses a **packed 13-bit position array**:

```c
/* Example: Setting sparse bits */
multiroar *r = multiroarBitNew();

for (int i = 0; i < 500; i++) {
    multiroarBitSet(r, i * 100);  /* Bits 0, 100, 200, ... */
}

/* Chunk 0: packed array with 82 positions (~133 bytes)
 * Much smaller than 1024-byte bitmap! */
```

**Format**:

```
[Type:1 byte][Count:1-2 bytes][Pos0:13bits][Pos1:13bits]...
```

**Packed Array Properties**:

- Uses varint for count (1-2 bytes)
- Each position: 13 bits
- Sorted for binary search
- Storage: 1 + varintLen(count) + ceil(count \* 13 / 8) bytes

### Dense Encoding

When a chunk has **629-7563 set bits**, it uses a **full 1024-byte bitmap**:

```c
/* Example: Dense chunk */
for (int i = 0; i < 2000; i++) {
    multiroarBitSet(r, i);  /* First 2000 bits */
}

/* Chunk 0: full bitmap (1025 bytes including type) */
```

**Format**:

```
[Type:1 byte][Bitmap:1024 bytes]
```

### Inverted Encoding

When a chunk has **more than 7563 set bits**, it stores **unset positions** instead:

```c
/* Example: Nearly full chunk */
for (int i = 0; i < 8192; i++) {
    if (i % 100 != 0) {  /* Skip every 100th */
        multiroarBitSet(r, i);
    }
}

/* Chunk 0: inverted packed array with ~82 unset positions (~133 bytes)
 * Much smaller than 1024-byte bitmap! */
```

**Format**:

```
[Type:1 byte][Count:1-2 bytes][NotSetPos0:13bits][NotSetPos1:13bits]...
```

### All-Ones Encoding

When **all 8192 bits** are set:

```c
for (int i = 0; i < 8192; i++) {
    multiroarBitSet(r, i);
}

/* Chunk 0: just 1 byte! */
```

**Format**:

```
[Type:1 byte]
```

### All-Zeroes Encoding

When **no bits** are set, the chunk **doesn't exist** in the multimap:

```c
multiroar *r = multiroarBitNew();
/* No bits set: 0 bytes! */
```

## Automatic Encoding Transitions

multiroar **automatically transitions** between encodings:

```
ALL_0 (implicit)
  │
  ├──> SPARSE (1st bit set)
  │      │
  │      └──> DENSE (packed list reaches 629 positions)
  │             │
  │             └──> INVERTED (population exceeds 7563)
  │                    │
  │                    └──> ALL_1 (last unset bit is set)
  │
  └──> ... (and reverse direction when clearing bits)
```

Clearing bits walks the same ladder downward, with hysteresis so a chunk
sitting exactly on a boundary does not thrash:

- INVERTED demotes back to DENSE once the unset list reaches 629 entries.
- DENSE demotes back to SPARSE only once the population drops below
  `629 / 2` (that is, 314 set bits) — not at 629.
- DENSE with population 0 deletes the chunk entirely (back to implicit ALL_0).

**Example of automatic conversion**:

```c
multiroar *r = multiroarBitNew();

/* Start: ALL_0 (no chunk) */
multiroarBitSet(r, 100);
/* Now: SPARSE (packed array with 1 position) */

for (uint64_t i = 0; i < 628; i++) {
    multiroarBitSet(r, i);
}
/* Still: SPARSE (fewer than 629 positions) */

multiroarBitSet(r, 700);
/* Now: DENSE (automatically converted to 1024-byte bitmap) */

for (uint64_t i = 700; i < 7500; i++) {
    multiroarBitSet(r, i);
}
/* Still: DENSE */

for (uint64_t i = 7500; i < 8100; i++) {
    multiroarBitSet(r, i);
}
/* Now: INVERTED (packed array of the remaining unset positions) */

/* Set remaining bits... */
for (uint64_t i = 8100; i < 8192; i++) {
    multiroarBitSet(r, i);
}
/* Now: ALL_1 (just 1 type byte) */
```

## Performance Characteristics

### Time Complexity

| Operation     | Chunk Lookup    | Within Chunk                 | Total           |
| ------------- | --------------- | ---------------------------- | --------------- |
| Set           | multimap seek   | O(n) worst                   | seek + O(n)     |
| Get           | multimap seek   | O(1) bitmap, O(log n) packed | seek + O(log n) |
| Remove        | multimap seek   | O(n) worst                   | seek + O(n)     |
| AND/OR/XOR    | O(chunks)       | O(1) bitmap, O(n) packed     | O(chunks x n)   |
| BitCount      | O(chunks)       | popcount over 1024 bytes     | O(chunks)       |
| Min / Max     | O(chunks) worst | O(1) bitmap, O(1) packed     | O(chunks)       |
| Rank          | O(chunks)       | popcount + partial word      | O(chunks)       |
| Select        | O(chunks)       | popcount scan                | O(chunks)       |
| Iterator Next | amortized O(1)  | O(1)                         | amortized O(1)  |
| N-way ops     | O(chunks)       | O(N) per chunk               | O(chunks x N)   |

**Notes**:

- n = number of set positions in a chunk (at most 629 for the packed forms)
- "chunks" = number of non-empty chunks
- Chunk lookup cost is whatever the backing multimap charges for a sorted
  key seek at its current size class; it is not a fixed O(log n) tree walk
- Bitmap operations within a chunk use word-at-a-time popcount / SIMD helpers

### Space Complexity

**Per bit (sparse)**:

- Packed array: ~1.625 bits per set bit (13 bits / 8)
- Overhead: ~3 bytes per chunk

**Per bit (dense)**:

- Bitmap: 1 bit per bit position
- Overhead: 1 byte per chunk

**Per bit (inverted)**:

- Packed array: ~1.625 bits per unset bit
- Overhead: ~3 bytes per chunk

**Comparison** (1M random bits):

| Encoding            | Memory    |
| ------------------- | --------- |
| Naive bitmap        | 125 KB    |
| Roaring (sparse)    | ~20-40 KB |
| Roaring (clustered) | ~10-20 KB |

### Automatic Encoding Selection

The thresholds are chosen so **each encoding is optimal** for its density range:

```
Sparse (0-628 bits):
  Packed array: 1 + varintLen(count) + ceil(628 * 13 / 8) = ~1023 bytes
  Bitmap: 1 + 1024 bytes
  Winner: Packed array

Dense (629-7563 bits):
  Bitmap: 1 + 1024 bytes
  Packed array: >= bitmap size
  Winner: Bitmap

Inverted (7564-8191 bits):
  Packed unset: 1 + varintLen(count) + ceil(628 * 13 / 8) = ~1023 bytes
  Bitmap: 1 + 1024 bytes
  Winner: Packed unset

All-ones (8192 bits):
  Type byte: 1 byte
  Winner: Type byte
```

The cutoff `(8192 / 13) - 1 == 629` is exactly the point where a 13-bit
packed list stops being smaller than the raw bitmap it replaces.

## Common Patterns

### Pattern 1: Sparse Integer Set

```c
/* Track user IDs (sparse, large range) */
multiroar *activeUsers = multiroarBitNew();

void userLogin(uint64_t userId) {
    multiroarBitSet(activeUsers, userId);
}

void userLogout(uint64_t userId) {
    multiroarRemove(activeUsers, userId);
}

bool isUserActive(uint64_t userId) {
    return multiroarBitGet(activeUsers, userId);
}

/* Snapshot the whole set without knowing the range up front */
uint64_t total = multiroarBitCount(activeUsers);
uint64_t *ids = zmalloc(total * sizeof(*ids));
const uint64_t got = multiroarToArray(activeUsers, ids, total);
assert(got == total);
```

### Pattern 2: Range Queries

```c
/* Track available time slots (dense ranges) */
multiroar *available = multiroarBitNew();

/* Block out 9am-5pm (slots 900-1700) */
multiroarBitSetRange(available, 900, 800);

/* Check if slot is available */
if (multiroarBitGet(available, 1430)) {
    printf("2:30 PM is available\n");
}
```

### Pattern 3: Set Operations

```c
/* User permissions */
multiroar *adminPerms = multiroarBitNew();
multiroar *userPerms = multiroarBitNew();

/* Admin has permissions 1, 2, 3, 5, 8 */
multiroarBitSet(adminPerms, 1);
multiroarBitSet(adminPerms, 2);
multiroarBitSet(adminPerms, 3);
multiroarBitSet(adminPerms, 5);
multiroarBitSet(adminPerms, 8);

/* User has permissions 2, 3, 4 */
multiroarBitSet(userPerms, 2);
multiroarBitSet(userPerms, 3);
multiroarBitSet(userPerms, 4);

/* Common permissions (intersection) */
multiroar *common = multiroarNewAnd(adminPerms, userPerms);
/* common = {2, 3} */

/* All permissions (union) */
multiroar *all = multiroarNewOr(adminPerms, userPerms);
/* all = {1, 2, 3, 4, 5, 8} */

/* Admin-only permissions (difference).
 * Use AndNot: NOT is bounded to existing chunks, so
 * "NOT then AND" is not a general set difference. */
multiroar *adminOnly = multiroarNewAndNot(adminPerms, userPerms);
/* adminOnly = {1, 5, 8} */

/* Relationship checks without materializing a result */
assert(multiroarIntersects(adminPerms, userPerms));
assert(!multiroarIsSubset(adminPerms, userPerms));
```

### Pattern 4: Bloom Filter Alternative

```c
/* Use roaring bitmap as a simple membership filter */
multiroar *seen = multiroarBitNew();

void markSeen(uint64_t id) {
    /* Hash to position */
    const uint64_t pos = hash(id) % (1ULL << 32);
    multiroarBitSet(seen, pos);
}

bool haveSeen(uint64_t id) {
    const uint64_t pos = hash(id) % (1ULL << 32);
    return multiroarBitGet(seen, pos);
}
```

### Pattern 5: Near-Duplicate Detection

```c
/* Shingle sets per document, compared by Jaccard similarity */
multiroar *docA = multiroarFromArray(shinglesA, countA);
multiroar *docB = multiroarFromArray(shinglesB, countB);

const double similarity = multiroarJaccard(docA, docB);
if (similarity > 0.8) {
    printf("near-duplicate (jaccard %.3f, hamming %" PRIu64 ")\n",
           similarity, multiroarHammingDistance(docA, docB));
}

multiroarFree(docA);
multiroarFree(docB);
```

## Best Practices

### 1. Use for Sparse Sets

Roaring bitmaps excel with sparse data:

```c
/* GOOD: Sparse user IDs (1-1000000000) */
multiroar *activeUsers = multiroarBitNew();
for (int i = 0; i < 10000; i++) {
    multiroarBitSet(activeUsers, rand() % 1000000000);
}

/* BAD: Dense sequential IDs (just use a regular bitmap) */
```

### 2. Batch Set Operations

```c
/* GOOD: Set multiple bits then perform operation */
multiroarBitSetMany(r1, positions, 1000);
multiroarAnd(r1, r2);

/* BAD: Interleaved operations */
for (uint64_t i = 0; i < 1000; i++) {
    multiroarBitSet(r1, positions[i]);
    multiroarAnd(r1, r2);  /* Don't do this! */
}
```

### 3. Fold Many Sets at Once

```c
/* GOOD: one chunk-id collection pass for all inputs */
multiroar *inputs[8] = {a, b, c, d, e, f, g, h};
multiroar *result = multiroarNewOrN(8, inputs);

/* WORSE: re-walks the chunk index on every pairwise step */
multiroar *acc = multiroarDuplicate(a);
for (uint64_t i = 1; i < 8; i++) {
    multiroarOr(acc, inputs[i]);
}
```

### 4. Watch Memory, Not Encodings

The chunk encoding is an internal detail and the handle is opaque, so there
is no supported way to inspect a chunk's type from outside. Track actual
footprint instead:

```c
printf("bitmap uses %" PRIu64 " bytes for %" PRIu64 " set bits\n",
       multiroarMemoryUsage(r), multiroarBitCount(r));
```

## Testing

From the test suite in `multiroar.c`:

```c
TEST("set and get") {
    multiroar *r = multiroarBitNew();

    bool previouslySet = multiroarBitSet(r, 1700);
    assert(!previouslySet);

    bool currentlySet = multiroarBitGet(r, 1700);
    assert(currentlySet);

    multiroarFree(r);
}

TEST("set sequential (triggers encoding transitions)") {
    multiroar *r = multiroarBitNew();

    /* Sets 0-71999, crossing all encoding types */
    for (int32_t i = 0; i < 72000; i++) {
        multiroarBitSet(r, i);
        assert(multiroarBitGet(r, i));
    }

    multiroarFree(r);
}

TEST("set random") {
    multiroar *r = multiroarBitNew();

    for (int32_t i = 0; i < 72000; i++) {
        const uint64_t position = (uint64_t)rand();
        multiroarBitSet(r, position);
        assert(multiroarBitGet(r, position));
    }

    multiroarFree(r);
}
```

## Debugging

`multiroar.c` provides two introspection helpers, both compiled only under
`DATAKIT_TEST` and both taking `uint64_t highest`:

```c
#ifdef DATAKIT_TEST
/* Print each chunk's type and size up to 'highest' */
void multiroarRepr(multiroar *r, uint64_t highest);

/* Print size versus an equivalent linear bitmap (compression ratio) */
void multiroarTestCompare(multiroar *r, uint64_t highest);
#endif
```

**Example output**:

```
Final size: 4352 bytes; Highest bit set: 71999; Size if linear: 9000 bytes
Savings: 1.07x
```

## Algorithm Details

### Varint Packed Arrays

Positions are stored in **13-bit packed format** using varint encoding:

The `varintPacked13*` family is generated by including
`varintPacked.h` with `PACK_STORAGE_BITS 13`, `PACK_MAX_ELEMENTS` set to
`MAX_ENTRIES_PER_DIRECT_LISTING`, and `PACK_FUNCTION_PREFIX varintPacked`:

```c
varintPacked13Set(array, index, position);      /* O(1) */
varintPacked13Get(array, index);                /* O(1) */
varintPacked13Member(array, count, position);   /* O(log n) binary search;
                                                 * returns the found index */
varintPacked13InsertSorted(array, count, pos);  /* O(n) insert */
varintPacked13DeleteMember(array, count, pos);  /* O(n) delete */
varintPacked13Delete(array, count, index);      /* O(n) delete by index */
```

**Why 13 bits?**

- 8192 positions require 13 bits (2^13 = 8192)
- Packing efficiency: 13/8 = 1.625 bits per position
- Better than 16 bits/position (2 bytes) by 23%

### Population Count (Popcount)

For transitioning from DENSE to INVERTED, we need to count set bits:

```c
const uint32_t population = StrPopCntExact(bitmapStart, 1024);

if (population > MAX_BITMAP_ENTIRES_BEFORE_NEGATIVE_LISTING) { /* 7563 */
    /* Convert to inverted encoding */
}
```

`StrPopCntExact()` comes from `str.h` and uses the platform's widest
available popcount path.

### Bitmap to Packed Array Conversion

```c
uint16_t bitmapToSetPositions(const void *bitmap, uint8_t positions[]) {
    /* For each 64-bit word: */
    while (myword != 0) {
        /* Find trailing zero count (position of lowest set bit) */
        int r = __builtin_ctzl(myword);

        /* Store position directly (already sorted!) */
        varintPacked13Set(positions, idx++, word_offset + r);

        /* Clear lowest bit */
        myword ^= (myword & -myword);
    }

    return idx;  /* count of set positions */
}
```

This is much faster than using `varintPacked13InsertSorted()` (not
benchmarked in-tree) because:

1. Bits are emitted in sorted order
2. No binary search needed
3. Direct index writes instead of shifting

## Thread Safety

multiroar is **not thread-safe**. For concurrent access:

```c
pthread_rwlock_t lock = PTHREAD_RWLOCK_INITIALIZER;

/* Readers */
pthread_rwlock_rdlock(&lock);
bool is_set = multiroarBitGet(r, position);
pthread_rwlock_unlock(&lock);

/* Writers */
pthread_rwlock_wrlock(&lock);
multiroarBitSet(r, position);
pthread_rwlock_unlock(&lock);
```

## See Also

- [multimap](../multimap/MULTIMAP.md) - Underlying storage for chunk lookup
- [databox](../core/DATABOX.md) - Value container for chunk data
- [Roaring Bitmaps Paper](https://arxiv.org/abs/1603.06549) - Original algorithm

## Implementation Notes

**Based on**: Roaring Bitmap algorithm by Lemire et al.

**Key Differences from the reference roaring implementations**:

- Uses multimap instead of specialized array/bitmap containers
- Integrates with datakit's databox system
- Custom 13-bit varint packing
- Adds an explicit all-ones chunk type on top of the usual
  array / bitmap / run trio
- `NOT` is bounded to already-materialized chunks rather than a fixed
  32-bit universe

**Chunk Size Choice** (8192 bits):

- Large enough to amortize multimap overhead
- Small enough for cache-friendly operations
- 13-bit addressing (clean power-of-2-ish)
- 1KB bitmaps fit in L1 cache

**Use Cases**:

- Sparse integer sets (user IDs, document IDs)
- Large-scale filtering (billions of IDs)
- Bitwise operations on sparse sets
- Membership testing with compression
- Index structures for databases
