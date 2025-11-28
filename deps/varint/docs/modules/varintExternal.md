# varintExternal: Zero-Overhead Metadata-External Varints

## Overview

**varintExternal** provides the most space-efficient variable-length integer encoding by storing **zero metadata** within the encoding itself. You must track the width externally (in a separate byte, implicit type system, or schema).

**Key Advantage**: Maximum storage density - stores only the actual data bytes needed.

**Origin**: Novel implementation based on common byte-slicing patterns.

## Key Characteristics

| Property         | Value                                    |
| ---------------- | ---------------------------------------- |
| Metadata Storage | External (caller manages)                |
| Endianness       | Little-endian (or big-endian variant)    |
| Size Range       | 1-8 bytes                                |
| 1-Byte Maximum   | 255                                      |
| Sortable         | No                                       |
| Performance      | Fastest (O(1), can cast to native types) |

## Encoding Format

### Byte-Width Maximums

| Bytes | Maximum Value              | Native Type Equivalent |
| ----- | -------------------------- | ---------------------- |
| 1     | 255                        | uint8_t                |
| 2     | 65,535                     | uint16_t               |
| 3     | 16,777,215                 | uint24_t (3 bytes)     |
| 4     | 4,294,967,295              | uint32_t               |
| 5     | 1,099,511,627,775          | uint40_t (5 bytes)     |
| 6     | 281,474,976,710,655        | uint48_t (6 bytes)     |
| 7     | 72,057,594,037,927,935     | uint56_t (7 bytes)     |
| 8     | 18,446,744,073,709,551,615 | uint64_t               |

### Why No Metadata = Maximum Efficiency

```
Traditional varint (with metadata): [META|DATA|DATA|...]
External varint:                    [DATA|DATA|DATA|...]
                                     ^^^^ All bits for your value!
```

**Result**: 8 bytes max instead of 9, and every byte fully utilizes all bits for data.

## API Reference

### Encoding Functions

```c
varintWidth varintExternalPut(void *buffer, uint64_t value);
```

Encodes a value using minimum bytes needed. Returns the width (1-8).

```c
void varintExternalPutFixedWidth(void *buffer, uint64_t value, varintWidth encoding);
```

Encodes at a specific width. Use when you need consistent sizing or pre-know the width.

```c
void varintExternalPutFixedWidthBig(__uint128_t value, varintWidth encoding);
```

Encodes 128-bit values (up to 16 bytes).

### Decoding Functions

```c
uint64_t varintExternalGet(const void *buffer, varintWidth encoding);
```

Decodes from buffer. **You must provide the width** - it's not stored in the buffer.

```c
__uint128_t varintBigExternalGet(const void *buffer, varintWidth encoding);
```

Decodes 128-bit values.

### Utility Functions

```c
varintWidth varintExternalSignedEncoding(int64_t value);
```

Calculates the minimum width needed for a **signed** value (handles negative numbers).

```c
#define varintExternalLen(value)
```

Macro to calculate minimum width for unsigned value.

```c
#define varintExternalUnsignedEncoding(value, encoding)
```

Macro that efficiently calculates width by counting bytes with non-zero bits.

### Fast Path Macros

```c
#define varintExternalPutFixedWidthQuick_(buffer, value, encoding)
```

Fast inline encoding for 1-3 byte values.

```c
#define varintExternalGetQuick_(buffer, width, result)
```

Fast inline decoding for 1-3 byte values.

```c
#define varintExternalGetQuickMedium_(buffer, width, result)
```

Fast inline decoding for 2-3 byte values (skips 1-byte case).

### Arithmetic Functions

```c
varintWidth varintExternalAddNoGrow(uint8_t *buffer, varintWidth encoding, int64_t add);
```

Adds `add` to value in-place. **Asserts if result would exceed current width.**

```c
varintWidth varintExternalAddGrow(uint8_t *buffer, varintWidth encoding, int64_t add);
```

Adds `add` to value, allowing width to grow if needed.

## Signed Value Encoding

Since varints store unsigned bytes, signed values need special handling:

### Zigzag Encoding (Recommended)

```c
// Encode signed to unsigned (zigzag)
uint64_t zigzagEncode(int64_t value) {
    return (value << 1) ^ (value >> 63);
}

// Decode unsigned back to signed
int64_t zigzagDecode(uint64_t value) {
    return (value >> 1) ^ (-(value & 1));
}

// Usage
int64_t signedVal = -100;
uint64_t encoded = zigzagEncode(signedVal);
varintWidth w = varintExternalPut(buffer, encoded);

// Later...
uint64_t decoded = varintExternalGet(buffer, w);
int64_t original = zigzagDecode(decoded);
```

### Built-in Sign Bit Macros

For non-native widths (24, 40, 48, 56 bits):

```c
int32_t signedVal = -1000;
varintWidth width = VARINT_WIDTH_24B;

// Prepare: moves sign bit to top of 24-bit width
varintPrepareSigned32to24_(signedVal);
varintExternalPutFixedWidth(buffer, (uint64_t)signedVal, width);

// Restore
uint64_t encoded = varintExternalGet(buffer, width);
varintRestoreSigned24to32_(encoded);
int32_t restored = (int32_t)encoded;
```

Available macros:

- `varintPrepareSigned32to24_` / `varintRestoreSigned24to32_`
- `varintPrepareSigned64to40_` / `varintRestoreSigned40to64_`
- `varintPrepareSigned64to48_` / `varintRestoreSigned48to64_`
- `varintPrepareSigned64to56_` / `varintRestoreSigned56to64_`

## Real-World Examples

### Example 1: Column Store with Schema

Store typed columns where the schema defines widths:

```c
#include "varintExternal.h"

typedef enum {
    COL_USER_ID,      // uint64_t - but most are small
    COL_AGE,          // uint8_t  - always 1 byte
    COL_SCORE,        // uint32_t - varies
    COL_COUNT
} ColumnType;

typedef struct {
    varintWidth widths[COL_COUNT];  // Schema: external metadata
    uint8_t *columnData[COL_COUNT]; // Actual data arrays
    size_t rowCount;
} ColumnStore;

void encodeRow(ColumnStore *store, size_t rowIdx,
               uint64_t userId, uint8_t age, uint32_t score) {
    size_t offset;

    // Column 0: User ID (variable width per row)
    offset = rowIdx * store->widths[COL_USER_ID];
    varintWidth userIdWidth = varintExternalPut(
        store->columnData[COL_USER_ID] + offset, userId);
    // Store width in schema if variable, or pre-set it

    // Column 1: Age (always 1 byte)
    offset = rowIdx * 1;
    store->columnData[COL_AGE][offset] = age;

    // Column 2: Score (variable)
    offset = rowIdx * store->widths[COL_SCORE];
    varintExternalPutFixedWidth(
        store->columnData[COL_SCORE] + offset, score,
        store->widths[COL_SCORE]);
}

uint64_t getUserId(ColumnStore *store, size_t rowIdx) {
    size_t offset = rowIdx * store->widths[COL_USER_ID];
    return varintExternalGet(
        store->columnData[COL_USER_ID] + offset,
        store->widths[COL_USER_ID]);
}

// For 1 million rows with typical distribution:
// uint64_t arrays: 3 columns × 8 bytes × 1M = 24 MB
// varintExternal:  (3 + 1 + 4) × 1M = 8 MB
// Savings: 66%
```

### Example 2: Cache with External Type Byte

Prefix each entry with a type byte indicating width:

```c
#include "varintExternal.h"

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t used;
} CompactCache;

// Format: [width:1byte][value:width bytes][width:1byte][value:width bytes]...

void cacheAdd(CompactCache *cache, uint64_t value) {
    varintWidth width = varintExternalLen(value);

    // Store width byte
    cache->data[cache->used++] = width;

    // Store value
    varintExternalPutFixedWidth(cache->data + cache->used, value, width);
    cache->used += width;
}

uint64_t cacheGet(CompactCache *cache, size_t *offset) {
    // Read width byte
    varintWidth width = cache->data[(*offset)++];

    // Read value
    uint64_t value = varintExternalGet(cache->data + *offset, width);
    *offset += width;

    return value;
}

void demonstrateCache() {
    CompactCache cache = { .data = malloc(1024), .capacity = 1024, .used = 0 };

    cacheAdd(&cache, 100);        // 1+1 = 2 bytes
    cacheAdd(&cache, 50000);      // 1+2 = 3 bytes
    cacheAdd(&cache, 10000000);   // 1+4 = 5 bytes

    size_t offset = 0;
    printf("%lu\n", cacheGet(&cache, &offset));  // 100
    printf("%lu\n", cacheGet(&cache, &offset));  // 50000
    printf("%lu\n", cacheGet(&cache, &offset));  // 10000000

    free(cache.data);
}
```

### Example 3: Network Protocol with Implicit Sizes

Protocol where message types imply value sizes:

```c
#include "varintExternal.h"

typedef enum {
    MSG_USER_ID,     // Implicit: 4 bytes (uint32_t)
    MSG_TIMESTAMP,   // Implicit: 5 bytes (40-bit timestamp)
    MSG_COUNTER,     // Implicit: 2 bytes (uint16_t)
} MessageType;

typedef struct {
    MessageType type;
    uint8_t payload[8];  // Max size
} NetworkMessage;

void encodeMessage(NetworkMessage *msg, MessageType type, uint64_t value) {
    msg->type = type;

    varintWidth width;
    switch (type) {
        case MSG_USER_ID:
            width = VARINT_WIDTH_32B;
            break;
        case MSG_TIMESTAMP:
            width = VARINT_WIDTH_40B;
            break;
        case MSG_COUNTER:
            width = VARINT_WIDTH_16B;
            break;
    }

    varintExternalPutFixedWidth(msg->payload, value, width);
}

uint64_t decodeMessage(const NetworkMessage *msg) {
    varintWidth width;
    switch (msg->type) {
        case MSG_USER_ID:
            width = VARINT_WIDTH_32B;
            break;
        case MSG_TIMESTAMP:
            width = VARINT_WIDTH_40B;
            break;
        case MSG_COUNTER:
            width = VARINT_WIDTH_16B;
            break;
    }

    return varintExternalGet(msg->payload, width);
}
```

### Example 4: Zero-Copy Casting (Little-Endian Only)

Directly cast to native types for system-width values:

```c
#include "varintExternal.h"
#include "endianIsLittle.h"

void demonstrateZeroCopy(void *buffer) {
    // Only works on little-endian systems!
    assert(endianIsLittle());

    // Encode uint32_t
    uint32_t value32 = 123456;
    varintExternalPutFixedWidth(buffer, value32, VARINT_WIDTH_32B);

    // ZERO-COPY READ: Cast directly!
    uint32_t *ptr = (uint32_t*)buffer;
    uint32_t readValue = *ptr;  // No decoding needed!
    assert(readValue == value32);

    // Works for 8, 16, 32, 64 bit widths
    // Does NOT work for 24, 40, 48, 56 bit widths (no native type)
}

// Use case: High-performance in-memory databases on x86_64
```

### Example 5: Packed Array with Known Width

Store arrays where all elements have the same width:

```c
#include "varintExternal.h"

typedef struct {
    uint8_t *data;
    varintWidth elementWidth;
    size_t elementCount;
} PackedArray;

void arrayInit(PackedArray *arr, varintWidth width, size_t capacity) {
    arr->elementWidth = width;
    arr->elementCount = 0;
    arr->data = calloc(capacity, width);
}

void arraySet(PackedArray *arr, size_t index, uint64_t value) {
    size_t offset = index * arr->elementWidth;
    varintExternalPutFixedWidth(arr->data + offset, value, arr->elementWidth);
}

uint64_t arrayGet(PackedArray *arr, size_t index) {
    size_t offset = index * arr->elementWidth;
    return varintExternalGet(arr->data + offset, arr->elementWidth);
}

void demonstrateArray() {
    // Store 10000 values that fit in 3 bytes (0 - 16,777,215)
    PackedArray arr;
    arrayInit(&arr, VARINT_WIDTH_24B, 10000);

    for (size_t i = 0; i < 10000; i++) {
        arraySet(&arr, i, i * 100);  // Values 0, 100, 200, ..., 999900
    }

    // Storage: 10000 × 3 = 30 KB
    // vs uint64_t: 10000 × 8 = 80 KB
    // Savings: 62.5%

    printf("Value at index 50: %lu\n", arrayGet(&arr, 50));  // 5000
    free(arr.data);
}
```

### Example 6: Adaptive Width for Growing Data

Start small and grow as needed:

```c
#include "varintExternal.h"

typedef struct {
    uint8_t *buffer;
    varintWidth currentWidth;
    size_t capacity;
} AdaptiveCounter;

void counterInit(AdaptiveCounter *counter) {
    counter->currentWidth = VARINT_WIDTH_8B;  // Start with 1 byte
    counter->capacity = 8;
    counter->buffer = calloc(1, counter->capacity);
    varintExternalPutFixedWidth(counter->buffer, 0, counter->currentWidth);
}

void counterIncrement(AdaptiveCounter *counter) {
    uint64_t current = varintExternalGet(counter->buffer, counter->currentWidth);
    current++;

    // Check if we need to grow
    varintWidth neededWidth = varintExternalLen(current);
    if (neededWidth > counter->currentWidth) {
        // Reallocate if needed
        if (neededWidth > counter->capacity) {
            counter->buffer = realloc(counter->buffer, neededWidth);
            counter->capacity = neededWidth;
        }
        counter->currentWidth = neededWidth;
    }

    varintExternalPutFixedWidth(counter->buffer, current, counter->currentWidth);
}

uint64_t counterGet(AdaptiveCounter *counter) {
    return varintExternalGet(counter->buffer, counter->currentWidth);
}

void demonstrateAdaptive() {
    AdaptiveCounter counter;
    counterInit(&counter);

    // Increment 300 times
    for (int i = 0; i < 300; i++) {
        counterIncrement(&counter);
    }

    printf("Counter: %lu, Width: %d bytes\n",
           counterGet(&counter), counter->currentWidth);
    // Output: Counter: 300, Width: 2 bytes

    free(counter.buffer);
}
```

## Performance Characteristics

### Space Efficiency

**BEST POSSIBLE** for variable-length encoding:

| Value Range         | Bytes | Savings vs uint64_t |
| ------------------- | ----- | ------------------- |
| 0-255               | 1     | 87.5%               |
| 256-65535           | 2     | 75%                 |
| 65536-16777215      | 3     | 62.5%               |
| 16777216-4294967295 | 4     | 50%                 |
| >4294967295         | 5-8   | 37.5% - 0%          |

**No metadata overhead** - every bit stores user data.

### Time Complexity

| Operation        | Complexity | Cycles (typical)         |
| ---------------- | ---------- | ------------------------ |
| Encode           | O(1)       | 2-4                      |
| Decode           | O(1)       | 2-4                      |
| Native cast (LE) | O(1)       | 0 (direct memory access) |

**Fastest varint type** due to:

1. No metadata parsing
2. Simple byte copying
3. Zero-copy reads possible (system widths, LE)

### Big-Endian Variant

For cross-platform compatibility or network byte order:

```c
#include "varintExternalBigEndian.h"

// Same API, big-endian storage
varintWidth width = varintExternalPutBigEndian(buffer, value);
uint64_t decoded = varintExternalGetBigEndian(buffer, width);
```

Use when:

- Sending over network (network byte order is big-endian)
- Cross-platform file formats
- Need memcmp-sortable external varints (rare - use varintTagged instead)

## When to Use varintExternal

### Use When:

- You have a **schema or type system** that tracks widths
- **Maximum space efficiency** is critical
- You can track metadata **externally** (separate byte, implicit, etc.)
- Working on **little-endian systems** and want zero-copy reads
- Building **column stores** or **typed data structures**

### Don't Use When:

- You need **self-describing** data (use varintTagged or varintSplit)
- You need **sortable** encodings (use varintTagged)
- Tracking external metadata is **too complex** for your use case
- Data is **dynamically typed** without schema

## Common Pitfalls

### Pitfall 1: Forgetting to Track Width

```c
// WRONG: Lost the width!
uint8_t buffer[8];
varintWidth width = varintExternalPut(buffer, 1000);
// ... later, width is lost ...
uint64_t value = varintExternalGet(buffer, ???);  // No width!

// RIGHT: Store width externally
struct {
    uint8_t width;
    uint8_t data[8];
} record;
record.width = varintExternalPut(record.data, 1000);
uint64_t value = varintExternalGet(record.data, record.width);
```

### Pitfall 2: Wrong Endianness Assumption

```c
// WRONG: Assuming big-endian
uint8_t buffer[4];
varintExternalPutFixedWidth(buffer, 0x12345678, VARINT_WIDTH_32B);
// buffer = [78 56 34 12] on little-endian!

// RIGHT: Use big-endian variant if needed
#include "varintExternalBigEndian.h"
varintExternalPutBigEndian(buffer, 0x12345678, VARINT_WIDTH_32B);
// buffer = [12 34 56 78] everywhere
```

### Pitfall 3: Zero-Copy on Big-Endian Systems

```c
// WRONG: Zero-copy cast on big-endian system
varintExternalPutFixedWidth(buffer, value, VARINT_WIDTH_32B);
uint32_t *ptr = (uint32_t*)buffer;
uint32_t wrong = *ptr;  // Byte order is reversed!

// RIGHT: Always use decode function (or check endianness first)
uint64_t correct = varintExternalGet(buffer, VARINT_WIDTH_32B);
```

## Testing Your Code

```c
#include "varintExternal.h"
#include <assert.h>

void testRoundtrip(uint64_t value, varintWidth expectedWidth) {
    uint8_t buffer[8];

    varintWidth width = varintExternalPut(buffer, value);
    assert(width == expectedWidth);

    uint64_t decoded = varintExternalGet(buffer, width);
    assert(decoded == value);
}

int main() {
    testRoundtrip(0, VARINT_WIDTH_8B);
    testRoundtrip(255, VARINT_WIDTH_8B);
    testRoundtrip(256, VARINT_WIDTH_16B);
    testRoundtrip(65535, VARINT_WIDTH_16B);
    testRoundtrip(65536, VARINT_WIDTH_24B);
    testRoundtrip(UINT32_MAX, VARINT_WIDTH_32B);
    testRoundtrip(UINT64_MAX, VARINT_WIDTH_64B);

    printf("All tests passed!\n");
}
```

## Implementation Details

### Source Files

- **Little-Endian**: `src/varintExternal.h` / `src/varintExternal.c`
- **Big-Endian**: `src/varintExternalBigEndian.h` / `src/varintExternalBigEndian.c`

### Width Encoding Constants

```c
typedef enum varintWidth {
    VARINT_WIDTH_8B = 1,   // 1 byte
    VARINT_WIDTH_16B = 2,  // 2 bytes
    VARINT_WIDTH_24B = 3,  // 3 bytes
    VARINT_WIDTH_32B = 4,  // 4 bytes
    VARINT_WIDTH_40B = 5,  // 5 bytes
    VARINT_WIDTH_48B = 6,  // 6 bytes
    VARINT_WIDTH_56B = 7,  // 7 bytes
    VARINT_WIDTH_64B = 8,  // 8 bytes
    // ... up to VARINT_WIDTH_128B = 16 for __uint128_t
} varintWidth;
```

## See Also

- [Architecture Overview](../ARCHITECTURE.md)
- [Choosing Varint Types](../CHOOSING_VARINTS.md)
- [varintTagged](varintTagged.md) - Sortable encoding
- [varintSplit](varintSplit.md) - Known boundaries
- [varintPacked](varintPacked.md) - Fixed-width bit packing
