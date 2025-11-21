# multiroar - Roaring Bitmap Implementation

## Overview

`multiroar` is a **space-efficient compressed bitmap** implementation based on the Roaring Bitmap algorithm. It stores sparse sets of integers using adaptive encoding that automatically switches between packed arrays, bitmaps, and inverted bitmaps depending on density.

**Key Features:**

- Automatic encoding selection (sparse/dense/inverted)
- 8192-bit chunks for optimal memory usage
- Packed 13-bit position arrays for sparse data
- Full bitmaps for dense data
- Inverted position arrays for nearly-full chunks
- Set operations (AND, OR, XOR, NOT)
- Built on multimap for O(log n) chunk lookup
- Support for both bitmaps and value matrices

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
Sparse (< 631 set bits):     Packed array of positions
  [Type:1][Count:2][Pos1:13bits][Pos2:13bits]...

Dense (631-7561 set bits):   Full 1024-byte bitmap
  [Type:1][Bitmap:8192bits]

Nearly Full (> 7561 bits):   Inverted packed array (unset positions)
  [Type:1][Count:2][NotPos1:13bits][NotPos2:13bits]...

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

```c
typedef struct multiroar multiroar;

struct multiroar {
    multimap *map;  /* Maps chunk_id -> encoded_chunk */
    uint8_t meta[];
    /* meta contains:
     *   - 1 byte: bit width (for value matrices)
     *   - 1-9 bytes: column count (varint)
     *   - 1-9 bytes: row count (varint) */
};
```

### Chunk Types

```c
typedef enum chunkType {
    CHUNK_TYPE_ALL_0 = 0,     /* Implicit (no entry) */
    CHUNK_TYPE_ALL_1,          /* All bits set */
    CHUNK_TYPE_UNDER_FULL_DIRECT_POSITION_NUMBERS,     /* Sparse */
    CHUNK_TYPE_FULL_BITMAP,    /* Dense bitmap */
    CHUNK_TYPE_OVER_FULL_DIRECT_NOT_SET_POSITION_NUMBERS,  /* Inverted */
} chunkType;
```

### Constants

```c
#define BITMAP_SIZE_IN_BITS 8192       /* 8KB per chunk */
#define BITMAP_SIZE_IN_BYTES 1024      /* 1KB per chunk */
#define DIRECT_STORAGE_BITS 13         /* 13 bits per position */
#define MAX_ENTRIES_PER_DIRECT_LISTING 631   /* (8192/13)-1 */
#define MAX_BITMAP_ENTIRES_BEFORE_NEGATIVE_LISTING 7561  /* 8192-631 */
```

## Creation Functions

```c
/* Create new bitmap (for tracking set bits) */
multiroar *multiroarBitNew(void);

/* Create value matrix (stores values, not just bits) */
multiroar *multiroarValueNew(uint8_t bitWidth, size_t rows, size_t cols);

/* Duplicate existing roar */
multiroar *multiroarDuplicate(multiroar *r);

/* Free multiroar */
void multiroarFree(multiroar *r);

/* Example: Create bitmap */
multiroar *r = multiroarBitNew();

/* Example: Create 2D value matrix (8-bit values) */
multiroar *matrix = multiroarValueNew(8, 1000, 1000);
```

## Bit Operations

### Set Bits

```c
/* Set bit at position (returns previous state) */
bool multiroarBitSet(multiroar *r, size_t position);

/* Set bit and get previous state */
bool multiroarBitSetGetPrevious(multiroar *r, size_t position);

/* Set range of bits */
void multiroarBitSetRange(multiroar *r, size_t start, size_t extent);

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
bool multiroarBitGet(multiroar *r, size_t position);

/* Example: Check membership */
if (multiroarBitGet(r, 1000)) {
    printf("Bit 1000 is set\n");
}
```

### Remove Bits

```c
/* Clear bit at position */
bool multiroarRemove(multiroar *r, size_t position);

/* Example */
multiroarBitSet(r, 500);
multiroarRemove(r, 500);
assert(!multiroarBitGet(r, 500));
```

## Value Operations

For value matrices (created with `multiroarValueNew`):

```c
/* Set value at position */
bool multiroarValueSet(multiroar *r, size_t position, databox *value);

/* Set value and get previous value */
bool multiroarValueSetGetPrevious(multiroar *r, size_t position,
                                  databox *value, databox *overwritten);

/* Remove value and get removed value */
bool multiroarValueRemoveGetRemoved(multiroar *r, size_t position,
                                    databox *removed);

/* Example: 2D matrix of 8-bit values */
multiroar *m = multiroarValueNew(8, 1000, 1000);

databox val = databoxNewSigned(42);
multiroarValueSet(m, 500000, &val);  /* row 500, col 0 */
```

## Set Operations

### In-Place Operations

```c
/* Modify first argument in-place */
void multiroarXor(multiroar *r, multiroar *b);  /* r = r XOR b */
void multiroarAnd(multiroar *r, multiroar *b);  /* r = r AND b */
void multiroarOr(multiroar *r, multiroar *b);   /* r = r OR b */
void multiroarNot(multiroar *r);                /* r = NOT r */

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
multiroar *multiroarNewXor(multiroar *r, multiroar *b);
multiroar *multiroarNewAnd(multiroar *r, multiroar *b);
multiroar *multiroarNewOr(multiroar *r, multiroar *b);
multiroar *multiroarNewNot(multiroar *r);

/* Example: Set union */
multiroar *result = multiroarNewOr(set1, set2);
/* set1 and set2 unchanged, result has union */
```

## Encoding Behavior

### Sparse Encoding

When a chunk has **< 631 set bits**, it uses a **packed 13-bit position array**:

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

When a chunk has **631-7561 set bits**, it uses a **full 1024-byte bitmap**:

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

When a chunk has **> 7561 set bits**, it stores **unset positions** instead:

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
  │      └──> DENSE (631st bit set)
  │             │
  │             └──> INVERTED (7562nd bit set)
  │                    │
  │                    └──> ALL_1 (last unset bit is set)
  │
  └──> ... (and reverse direction when clearing bits)
```

**Example of automatic conversion**:

```c
multiroar *r = multiroarBitNew();

/* Start: ALL_0 (no chunk) */
multiroarBitSet(r, 100);
/* Now: SPARSE (packed array with 1 position) */

for (int i = 0; i < 630; i++) {
    multiroarBitSet(r, i);
}
/* Still: SPARSE (630 positions) */

multiroarBitSet(r, 700);
/* Now: DENSE (automatically converted to 1024-byte bitmap) */

for (int i = 700; i < 7600; i++) {
    multiroarBitSet(r, i);
}
/* Still: DENSE (7600 bits set) */

multiroarBitSet(r, 7700);
/* Now: INVERTED (automatically converted to packed array of ~600 unset positions) */

/* Set remaining bits... */
for (int i = 7700; i < 8192; i++) {
    multiroarBitSet(r, i);
}
/* Now: ALL_1 (just 1 type byte) */
```

## Performance Characteristics

### Time Complexity

| Operation  | Chunk Lookup  | Within Chunk                 | Total                 |
| ---------- | ------------- | ---------------------------- | --------------------- |
| Set        | O(log chunks) | O(n) worst                   | O(log chunks + n)     |
| Get        | O(log chunks) | O(1) bitmap, O(log n) packed | O(log chunks + log n) |
| Remove     | O(log chunks) | O(n) worst                   | O(log chunks + n)     |
| AND/OR/XOR | O(chunks)     | O(1) bitmap, O(n) packed     | O(chunks × n)         |

**Notes**:

- n = number of set positions in chunk (max 631 for packed)
- "chunks" = number of non-empty chunks
- Bitmap operations within a chunk are O(1) with SIMD

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
Sparse (0-630 bits):
  Packed array: 1 + 2 + ceil(630 * 13 / 8) = ~1025 bytes
  Bitmap: 1024 bytes
  Winner: Packed array (equal at 630, better below)

Dense (631-7561 bits):
  Bitmap: 1024 bytes
  Packed array: > 1024 bytes
  Winner: Bitmap

Inverted (7562-8191 bits):
  Packed unset: 1 + 2 + ceil(630 * 13 / 8) = ~1025 bytes
  Bitmap: 1024 bytes
  Winner: Packed unset (equal at 630 unset, better above)

All-ones (8192 bits):
  Type byte: 1 byte
  Winner: Type byte
```

## Common Patterns

### Pattern 1: Sparse Integer Set

```c
/* Track user IDs (sparse, large range) */
multiroar *activeUsers = multiroarBitNew();

void userLogin(size_t userId) {
    multiroarBitSet(activeUsers, userId);
}

void userLogout(size_t userId) {
    multiroarRemove(activeUsers, userId);
}

bool isUserActive(size_t userId) {
    return multiroarBitGet(activeUsers, userId);
}
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

/* Admin-only permissions (difference) */
multiroar *adminOnly = multiroarDuplicate(adminPerms);
multiroar *notUser = multiroarNewNot(userPerms);
multiroarAnd(adminOnly, notUser);
/* adminOnly = {1, 5, 8} */
```

### Pattern 4: Bloom Filter Alternative

```c
/* Use roaring bitmap as a simple membership filter */
multiroar *seen = multiroarBitNew();

void markSeen(uint64_t id) {
    /* Hash to position */
    size_t pos = hash(id) % (1ULL << 32);
    multiroarBitSet(seen, pos);
}

bool haveSeen(uint64_t id) {
    size_t pos = hash(id) % (1ULL << 32);
    return multiroarBitGet(seen, pos);
}
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
for (int i = 0; i < 1000; i++) {
    multiroarBitSet(r1, positions[i]);
}
multiroarAnd(r1, r2);

/* BAD: Interleaved operations */
for (int i = 0; i < 1000; i++) {
    multiroarBitSet(r1, positions[i]);
    multiroarAnd(r1, r2);  /* Don't do this! */
}
```

### 3. Monitor Encoding Transitions

If you're seeing unexpected memory usage, check encoding:

```c
/* In debug builds, track encoding type */
void checkEncoding(multiroar *r, size_t chunkId) {
    databox value;
    databox *values[1] = {&value};
    databox key = {.data.u = chunkId, .type = DATABOX_UNSIGNED_64};

    if (multimapLookup(r->map, &key, values)) {
        uint8_t type = value.data.bytes.start[0];
        printf("Chunk %zu: type %d, size %lu bytes\n",
               chunkId, type, value.len);
    } else {
        printf("Chunk %zu: ALL_0 (implicit)\n", chunkId);
    }
}
```

### 4. Pre-allocate for Large Sets

```c
/* If you know approximate size, you can pre-grow multimap */
multiroar *r = multiroarBitNew();
multimapExpand(r->map, estimatedChunkCount);

for (size_t i = 0; i < MANY; i++) {
    multiroarBitSet(r, positions[i]);
}
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
        size_t position = (size_t)rand() % SIZE_MAX;
        multiroarBitSet(r, position);
        assert(multiroarBitGet(r, position));
    }

    multiroarFree(r);
}
```

## Debugging

```c
#ifdef DATAKIT_TEST
/* Print chunk encoding info */
void multiroarRepr(multiroar *r, size_t highest) {
    for (size_t i = 0; i < divCeil(highest, 8192); i++) {
        /* Print each chunk's type and size */
    }
}

/* Compare with linear bitmap */
void multiroarTestCompare(multiroar *r, size_t highest) {
    /* Prints compression ratio */
}
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

```c
/* From varintPacked.h */
varintPacked13Set(array, index, position);      /* O(1) */
varintPacked13Get(array, index);                /* O(1) */
varintPacked13Member(array, count, position);   /* O(log n) binary search */
varintPacked13InsertSorted(array, count, pos);  /* O(n) insert */
varintPacked13DeleteMember(array, count, pos);  /* O(n) delete */
```

**Why 13 bits?**

- 8192 positions require 13 bits (2^13 = 8192)
- Packing efficiency: 13/8 = 1.625 bits per position
- Better than 16 bits/position (2 bytes) by 23%

### Population Count (Popcount)

For transitioning from DENSE to INVERTED, we need to count set bits:

```c
const uint32_t population = StrPopCntExact(bitmap, 1024);

if (population > 7561) {
    /* Convert to inverted encoding */
}
```

This uses **SIMD-optimized popcount** (`__builtin_popcountll` or equivalent).

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

This is **62x faster** than using `varintPacked13InsertSorted()` because:

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

**Key Differences from CRoaring**:

- Uses multimap instead of specialized array/bitmap containers
- Integrates with datakit's databox system
- Custom 13-bit varint packing
- Simplified encoding transitions

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
