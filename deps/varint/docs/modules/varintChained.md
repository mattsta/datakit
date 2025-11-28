# varintChained: Legacy Continuation-Bit Varints

## Overview

**varintChained** uses continuation bits in each byte to signal whether more bytes follow. This is the **slowest** varint type because determining length requires traversing the entire encoding byte-by-byte.

**Origin**: Adapted from SQLite3 and LevelDB - the most common legacy varint format.

**Recommendation**: Use for legacy compatibility only. Prefer varintTagged or varintExternal for new systems.

## Key Characteristics

| Property             | Value                              |
| -------------------- | ---------------------------------- |
| Metadata Storage     | Continuation bit in each byte      |
| Endianness           | Little-endian (value bytes)        |
| Size Range           | 1-9 bytes                          |
| 1-Byte Maximum       | 127 (7 bits)                       |
| Sortable             | No                                 |
| Performance          | **Slowest** (O(w) operations)      |
| Legacy Compatibility | SQLite3, LevelDB, Protocol Buffers |

## Encoding Format

Each byte has a **continuation bit** (high bit) indicating if more bytes follow:

```
Byte format: [C|DDDDDDD]
  C = Continuation bit (1 = more bytes, 0 = last byte)
  D = Data bits (7 bits per byte)
```

### Bit Layout

| Bytes | Data Bits | Maximum Value                |
| ----- | --------- | ---------------------------- |
| 1     | 7         | 127 (2^7 - 1)                |
| 2     | 14        | 16,383 (2^14 - 1)            |
| 3     | 21        | 2,097,151 (~2 million)       |
| 4     | 28        | 268,435,455 (~268 million)   |
| 5     | 35        | 34,359,738,367 (~34 billion) |
| 6     | 42        | 4,398,046,511,103            |
| 7     | 49        | 562,949,953,421,311          |
| 8     | 56        | 72,057,594,037,927,935       |
| 9     | 64        | 2^64 - 1 (full uint64_t)     |

**Special optimization**: The 9th byte uses all 8 bits (no continuation bit needed).

## Why It's Slow

```c
// Other varints: Read first byte, know length immediately
varintWidth len = buffer[0] >> 6;  // O(1)

// Chained: Must scan until continuation bit is 0
varintWidth len = 0;
while (buffer[len] & 0x80) len++;  // O(w) - must scan all bytes
len++;
```

**Performance impact**: For an 8-byte chained varint, you read 8 bytes sequentially before knowing the length. Other varint types know length from the first byte.

## API Reference

### Encoding

```c
varintWidth varintChainedPutVarint(uint8_t *buffer, uint64_t value);
```

Encodes a value. Returns bytes used (1-9).

### Decoding

```c
varintWidth varintChainedGetVarint(const uint8_t *buffer, uint64_t *value);
```

Decodes a value. Returns bytes consumed.

```c
varintWidth varintChainedGetVarint32(const uint8_t *buffer, uint32_t *value);
```

Optimized decoder for 32-bit values (max 5 bytes).

### Utility

```c
varintWidth varintChainedVarintLen(uint64_t value);
```

Calculates encoding length without encoding.

### Arithmetic

```c
varintWidth varintChainedVarintAddNoGrow(uint8_t *buffer, int64_t add);
```

In-place addition (asserts if grows).

```c
varintWidth varintChainedVarintAddGrow(uint8_t *buffer, int64_t add);
```

In-place addition (allows growth).

### Fast Path Macros

```c
varintChained_getVarint32(buffer, result)
```

Inline macro for decoding. Fast path for 1-byte values.

```c
varintChained_putVarint32(buffer, value)
```

Inline macro for encoding. Fast path for values < 128.

## Real-World Examples

### Example 1: SQLite3 Compatibility

Read varints from SQLite3 database files:

```c
#include "varintChained.h"

// SQLite3 uses chained varints for rowid and record headers
uint64_t readSQLite3Rowid(const uint8_t *dbPage, size_t offset) {
    uint64_t rowid;
    varintWidth len = varintChainedGetVarint(dbPage + offset, &rowid);
    return rowid;
}

void writeSQLite3Rowid(uint8_t *dbPage, size_t offset, uint64_t rowid) {
    varintChainedPutVarint(dbPage + offset, rowid);
}
```

### Example 2: Protocol Buffers Wire Format

Encode/decode Protocol Buffer varints:

```c
#include "varintChained.h"

// Protocol Buffers use chained varints for all integer types
typedef struct {
    uint8_t buffer[1024];
    size_t used;
} ProtoMessage;

void protoAddField(ProtoMessage *msg, uint64_t fieldNumber, uint64_t value) {
    // Write field tag (field number << 3 | wire type)
    uint64_t tag = (fieldNumber << 3) | 0;  // Wire type 0 = varint
    msg->used += varintChainedPutVarint(msg->buffer + msg->used, tag);

    // Write value
    msg->used += varintChainedPutVarint(msg->buffer + msg->used, value);
}

uint64_t protoReadField(const ProtoMessage *msg, size_t *offset,
                        uint64_t *fieldNumber) {
    // Read tag
    uint64_t tag;
    *offset += varintChainedGetVarint(msg->buffer + *offset, &tag);
    *fieldNumber = tag >> 3;

    // Read value
    uint64_t value;
    *offset += varintChainedGetVarint(msg->buffer + *offset, &value);
    return value;
}
```

### Example 3: LevelDB Key Encoding

Compatible with LevelDB internal key format:

```c
#include "varintChained.h"
#include "varintChainedSimple.h"  // LevelDB-optimized variant

// LevelDB uses chained varints for sequence numbers
typedef struct {
    uint8_t userKey[256];
    size_t userKeyLen;
    uint64_t sequenceNumber;
    uint8_t type;  // kTypeValue or kTypeDeletion
} LevelDBInternalKey;

size_t encodeLevelDBKey(uint8_t *buffer, const LevelDBInternalKey *key) {
    size_t offset = 0;

    // User key
    memcpy(buffer + offset, key->userKey, key->userKeyLen);
    offset += key->userKeyLen;

    // Sequence number (7 bytes) + type (1 byte) = 8 bytes
    // LevelDB packs these, but for simplicity:
    uint64_t tag = (key->sequenceNumber << 8) | key->type;
    offset += varintChainedPutVarint(buffer + offset, tag);

    return offset;
}
```

### Example 4: Streaming Decoder

Process chained varints from a stream:

```c
#include "varintChained.h"

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t offset;
} ByteStream;

bool streamReadVarint(ByteStream *stream, uint64_t *value) {
    if (stream->offset >= stream->size) {
        return false;  // End of stream
    }

    varintWidth len = varintChainedGetVarint(
        stream->data + stream->offset, value);

    if (stream->offset + len > stream->size) {
        return false;  // Incomplete varint
    }

    stream->offset += len;
    return true;
}

void processStream(const uint8_t *data, size_t size) {
    ByteStream stream = { .data = data, .size = size, .offset = 0 };
    uint64_t value;

    while (streamReadVarint(&stream, &value)) {
        printf("Read value: %lu\n", value);
    }
}
```

### Example 5: Fast Path Optimization

Use macros for common small values:

```c
#include "varintChained.h"

// Process array of mostly-small integers
void encodeArray(uint8_t *buffer, const uint32_t *values, size_t count) {
    size_t offset = 0;

    for (size_t i = 0; i < count; i++) {
        // Fast path for values < 128 (90% of cases)
        varintWidth len = varintChained_putVarint32(buffer + offset, values[i]);
        offset += len;

        // Macro expands to:
        // if (value < 128) {
        //     buffer[offset] = value;
        //     len = 1;
        // } else {
        //     len = varintChainedPutVarint(buffer + offset, value);
        // }
    }
}
```

### Example 6: Comparison with Other Varints

Demonstrate why chained is slower:

```c
#include "varintChained.h"
#include "varintTagged.h"
#include "varintExternal.h"
#include <time.h>

void benchmarkVarints(uint64_t value, size_t iterations) {
    uint8_t buffer[9];
    clock_t start, end;

    // Chained: Slowest
    start = clock();
    for (size_t i = 0; i < iterations; i++) {
        varintChainedPutVarint(buffer, value);
        uint64_t decoded;
        varintChainedGetVarint(buffer, &decoded);
    }
    end = clock();
    printf("Chained: %.3f seconds\n",
           (double)(end - start) / CLOCKS_PER_SEC);

    // Tagged: Faster
    start = clock();
    for (size_t i = 0; i < iterations; i++) {
        varintTaggedPut64(buffer, value);
        uint64_t decoded;
        varintTaggedGet64(buffer, &decoded);
    }
    end = clock();
    printf("Tagged:  %.3f seconds\n",
           (double)(end - start) / CLOCKS_PER_SEC);

    // External: Fastest
    varintWidth width = varintExternalPut(buffer, value);
    start = clock();
    for (size_t i = 0; i < iterations; i++) {
        varintExternalPutFixedWidth(buffer, value, width);
        uint64_t decoded = varintExternalGet(buffer, width);
    }
    end = clock();
    printf("External: %.3f seconds\n",
           (double)(end - start) / CLOCKS_PER_SEC);

    // Typical results for value = 1000000:
    // Chained:  0.120 seconds
    // Tagged:   0.045 seconds (2.7x faster)
    // External: 0.030 seconds (4x faster)
}
```

## varintChainedSimple: LevelDB-Optimized Variant

A simplified variant optimized for LevelDB's usage patterns:

```c
#include "varintChainedSimple.h"

// Same API, but optimized for specific patterns
varintWidth varintChainedSimplePutVarint(uint8_t *p, uint64_t v);
varintWidth varintChainedSimpleGetVarint(const uint8_t *p, uint64_t *v);
```

**Differences from standard chained**:

- Simplified loop structure
- Better compiler optimization
- Slightly faster for LevelDB-typical value ranges

**Use when**: Compatibility with LevelDB or when you need the absolute closest match to LevelDB's implementation.

## Performance Characteristics

### Time Complexity

| Operation  | Complexity | Why                                          |
| ---------- | ---------- | -------------------------------------------- |
| Encode     | O(w)       | Must write w bytes with continuation bits    |
| Decode     | O(w)       | Must read w bytes until continuation bit = 0 |
| Get Length | O(w)       | Must scan until continuation bit = 0         |

**w** = width in bytes (1-9)

For comparison:

- **varintTagged**: O(1) - read first byte, know length
- **varintExternal**: O(1) - length provided externally
- **varintSplit**: O(1) - read first byte, know length

### Space Efficiency

| Value Range      | Chained Bytes | Tagged Bytes | Winner  |
| ---------------- | ------------- | ------------ | ------- |
| 0-127            | 1             | 1            | Tie     |
| 128-16,383       | 2             | 2            | Tie     |
| 16,384-2,097,151 | 3             | 3-4          | Chained |
| 2,097,152+       | 4-9           | 4-9          | Similar |

**Advantage**: Chained can store ~2 million in 3 bytes (21 bits), while tagged needs 4 bytes for values > 67,823.

**Disadvantage**: Everything else (performance, sortability, length detection).

### CPU Performance

From `varintCompare` benchmarks:

- **Encode**: ~8-12 cycles (2-3x slower than tagged/external)
- **Decode**: ~8-12 cycles (2-3x slower than tagged/external)
- **Length detection**: Must decode entire varint (vs. 1 byte for others)

## When to Use varintChained

### Use When:

- **Legacy compatibility** required (SQLite3, LevelDB, Protocol Buffers)
- Interfacing with **existing wire protocols**
- Values frequently in 16,384 - 2,097,151 range (3-byte sweet spot)

### Don't Use When:

- Building **new systems** (use varintTagged or varintExternal)
- **Performance is critical**
- Need **O(1) length detection**
- Need **sortable** encodings

## Common Pitfalls

### Pitfall 1: Assuming Fast Length Detection

```c
// WRONG: Thinking you can quickly get length
uint8_t firstByte = buffer[0];
// Can't determine length from first byte alone!

// RIGHT: Must scan or fully decode
varintWidth len = 0;
while (buffer[len] & 0x80) len++;
len++;
```

### Pitfall 2: Buffer Overflow in Streaming

```c
// WRONG: Not checking stream bounds
uint64_t value;
varintChainedGetVarint(buffer + offset, &value);  // Might read past end!

// RIGHT: Check bounds before decoding
if (offset + 9 <= bufferSize) {  // Max 9 bytes
    offset += varintChainedGetVarint(buffer + offset, &value);
}
```

### Pitfall 3: Mixing Chained Variants

```c
// WRONG: Encoding with one variant, decoding with another
varintChainedPutVarint(buffer, value);           // SQLite3 variant
varintChainedSimpleGetVarint(buffer, &decoded);  // LevelDB variant

// RIGHT: Use matching encode/decode
varintChainedPutVarint(buffer, value);
varintChainedGetVarint(buffer, &decoded);
```

## Migration from Chained to Faster Varints

If you have legacy data but want better performance:

```c
// Convert chained varints to tagged varints in-place
void migrateChainedToTagged(uint8_t *data, size_t dataSize) {
    size_t readOffset = 0;
    size_t writeOffset = 0;
    uint8_t tempBuffer[9];

    while (readOffset < dataSize) {
        // Decode chained
        uint64_t value;
        varintWidth chainedLen = varintChainedGetVarint(
            data + readOffset, &value);

        // Encode as tagged
        varintWidth taggedLen = varintTaggedPut64(tempBuffer, value);

        // Copy to output (may overlap, use memmove)
        memmove(data + writeOffset, tempBuffer, taggedLen);

        readOffset += chainedLen;
        writeOffset += taggedLen;
    }

    // New data size is writeOffset (may be smaller or larger)
}
```

## Implementation Details

### Source Files

- **SQLite3 variant**: `src/varintChained.h` / `src/varintChained.c`
- **LevelDB variant**: `src/varintChainedSimple.h` / `src/varintChainedSimple.c`

### Encoding Algorithm

```
For each 7-bit chunk of value (low to high):
  byte = (chunk & 0x7F) | continuation_bit
  continuation_bit = (more_chunks_follow ? 0x80 : 0x00)
  write byte
```

### Decoding Algorithm

```
result = 0
shift = 0
loop:
  byte = read_byte()
  result |= (byte & 0x7F) << shift
  shift += 7
  if (byte & 0x80) == 0:
    break
return result
```

## See Also

- [Architecture Overview](../ARCHITECTURE.md)
- [Choosing Varint Types](../CHOOSING_VARINTS.md)
- [varintTagged](varintTagged.md) - Faster, sortable alternative
- [varintExternal](varintExternal.md) - Fastest alternative
- [SQLite3 Source](https://sqlite.org/src/file/src/util.c)
- [LevelDB Source](https://github.com/google/leveldb)
