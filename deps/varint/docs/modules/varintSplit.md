# varintSplit: Three-Level Encoding with Known Bit Boundaries

## Overview

**varintSplit** uses a novel three-level encoding scheme where each level has **known bit boundaries**. This allows you to pack other data types on top of split varints or extract specific levels without full decoding.

**Key Innovation**: Predictable bit offsets within each encoding level enable advanced packing strategies.

**Origin**: Metadata layouts inspired by legacy Redis ziplist bit-type management (circa 2015), with novel implementation and expansions.

## Key Characteristics

| Property         | Value                                  |
| ---------------- | -------------------------------------- |
| Metadata Storage | First byte (2-bit prefix)              |
| Endianness       | Big-endian type, little-endian payload |
| Size Range       | 1-9 bytes                              |
| 1-Byte Maximum   | 63                                     |
| Sortable         | No                                     |
| Performance      | Fast (O(1), macro-based)               |
| Unique Feature   | Known bit boundaries per level         |

## Three-Level Encoding

### Level 1: 6-Bit Embedded (1 byte total)

```
Prefix: 00
Format: [00pppppp]
Range:  0 - 63
Bytes:  1
```

### Level 2: 14-Bit Embedded (2 bytes total)

```
Prefix: 01
Format: [01pppppp][qqqqqqqq]
Range:  64 - 16,446 (includes previous level: 63 + 16,383)
Bytes:  2
```

### Level 3: External varint (2-9 bytes total)

```
Prefix: 10
Format: [10000xxx][external varint: 1-8 bytes]
Range:  16,447 - 2^64-1 (includes previous levels: 16,446 + value)
Bytes:  2-9 (1 type byte + 1-8 data bytes)
```

### Why Three Levels?

1. **Small values** (0-63): Single byte, 6 bits of data
2. **Medium values** (64-16,446): Two bytes, 14 bits total
3. **Large values** (16,447+): External encoding with offset

**Cumulative offsets**: Each level adds previous maximums, achieving higher values with fewer bytes.

## Byte-Width Maximums

| Bytes | Level | Maximum Value          | Calculation              |
| ----- | ----- | ---------------------- | ------------------------ |
| 1     | 1     | 63                     | 2^6 - 1                  |
| 2     | 2     | 16,446                 | 63 + (2^14 - 1)          |
| 2     | 3     | 16,701                 | 16,446 + 2^8             |
| 3     | 3     | 81,982                 | 16,446 + 2^16            |
| 4     | 3     | 16,793,661             | 16,446 + 2^24            |
| 5     | 3     | 4,294,983,741          | 16,446 + 2^32            |
| 6     | 3     | 1,099,511,644,221      | 16,446 + 2^40            |
| 7     | 3     | 281,474,976,727,101    | 16,446 + 2^48            |
| 8     | 3     | 72,057,594,037,944,381 | 16,446 + 2^56            |
| 9     | 3     | 2^64 - 1               | 16,446 + (2^64 - 16,447) |

## API Reference (Macro-Based)

All varintSplit operations are **macros**, not functions. This enables inline expansion and compile-time optimization.

### Encoding

```c
varintSplitPut_(buffer, encodedLen, value)
```

Encodes `value` into `buffer`. Sets `encodedLen` to bytes used (1-9).

**Parameters:**

- `buffer`: uint8_t array (min 9 bytes)
- `encodedLen`: varintWidth variable (output)
- `value`: uint64_t to encode

**Example:**

```c
uint8_t buf[9];
varintWidth len;
varintSplitPut_(buf, len, 1000);
// len = 2 (value 1000 fits in level 2)
```

### Decoding

```c
varintSplitGet_(buffer, valsize, val)
```

Decodes from `buffer`. Sets `valsize` to bytes consumed and `val` to the value.

**Parameters:**

- `buffer`: const uint8_t\* to encoded data
- `valsize`: varintWidth variable (output)
- `val`: uint64_t variable (output)

**Example:**

```c
const uint8_t *buf = ...;
varintWidth size;
uint64_t value;
varintSplitGet_(buf, size, value);
// size = bytes consumed, value = decoded value
```

### Length Operations

```c
varintSplitLength_(encodedLen, value)
```

Calculates encoding length without encoding.

```c
varintSplitGetLen_(buffer, valsize)
```

Gets length of encoded varint from buffer.

```c
varintSplitGetLenQuick_(buffer)
```

Fast inline macro to get length (returns varintWidth).

## Reversed Split Varints

For **reverse traversal** (scanning backwards through arrays), use reversed split:

### Reversed Put (Grows Backwards)

```c
varintSplitReversedPutReversed_(buffer, encodedLen, value)
```

Writes type byte at `buffer[0]`, data bytes at negative offsets.

**Use when:** Building arrays from end to beginning.

### Forward Put (Type at End)

```c
varintSplitReversedPutForward_(buffer, encodedLen, value)
```

Writes data bytes forward, type byte at end.

**Use when:** Type byte must be last for reverse scanning.

### Reversed Get

```c
varintSplitReversedGet_(buffer, valsize, val)
```

Decodes reversed split varint (type byte at `buffer[0]`, data at negative offsets).

## Real-World Examples

### Example 1: Custom Bit Packing

Pack additional flags into the unused prefix bits:

```c
#include "varintSplit.h"

// Split varints use 2-bit prefix: 00, 01, 10
// Prefix 11 is reserved - use for custom data!

#define CUSTOM_FLAG_PREFIX 0xC0  // 11000000

typedef struct {
    uint8_t encoded[9];
    varintWidth len;
} FlaggedValue;

void encodeWithFlag(FlaggedValue *fv, uint64_t value, bool flag) {
    if (flag && value <= 63) {
        // Encode as custom 11XXXXXX format for values 0-63
        fv->encoded[0] = CUSTOM_FLAG_PREFIX | (value & 0x3F);
        fv->len = 1;
    } else {
        // Use standard split encoding
        varintSplitPut_(fv->encoded, fv->len, value);
    }
}

bool decodeWithFlag(const FlaggedValue *fv, uint64_t *value) {
    if ((fv->encoded[0] & 0xC0) == CUSTOM_FLAG_PREFIX) {
        // Custom flagged format
        *value = fv->encoded[0] & 0x3F;
        return true;  // Flag was set
    } else {
        // Standard split encoding
        varintWidth size;
        varintSplitGet_(fv->encoded, size, *value);
        return false;  // No flag
    }
}

// Use case: Store values with a "deleted" flag or "type" bit
```

### Example 2: Known-Boundary Array Packing

Build tightly packed arrays where you know offsets:

```c
#include "varintSplit.h"

typedef struct {
    uint8_t *buffer;
    size_t capacity;
    size_t used;
    size_t count;
} SplitArray;

void arrayInit(SplitArray *arr, size_t capacity) {
    arr->buffer = malloc(capacity);
    arr->capacity = capacity;
    arr->used = 0;
    arr->count = 0;
}

void arrayPush(SplitArray *arr, uint64_t value) {
    varintWidth len;
    varintSplitPut_(arr->buffer + arr->used, len, value);
    arr->used += len;
    arr->count++;
}

uint64_t arrayGet(SplitArray *arr, size_t index) {
    // Sequential scan to find index (no random access)
    size_t offset = 0;
    for (size_t i = 0; i < index; i++) {
        varintWidth len = varintSplitGetLenQuick_(arr->buffer + offset);
        offset += len;
    }

    uint64_t value;
    varintWidth len;
    varintSplitGet_(arr->buffer + offset, len, value);
    return value;
}

void demonstratePacking() {
    SplitArray arr;
    arrayInit(&arr, 1024);

    // Push values with different sizes
    for (uint64_t i = 0; i < 1000; i++) {
        arrayPush(&arr, i);  // 0-63: 1 byte, 64-1000: 2 bytes
    }

    // Storage: (64 × 1) + (936 × 2) = 1936 bytes
    // vs uint64_t: 1000 × 8 = 8000 bytes
    // Savings: 75.8%

    printf("Value at index 500: %lu\n", arrayGet(&arr, 500));

    free(arr.buffer);
}
```

### Example 3: Hybrid Encoding (Split + Fixed Data)

Combine split varints with fixed-size data:

```c
#include "varintSplit.h"

// Record: [split varint ID][32-bit timestamp][16-bit value]
typedef struct {
    uint8_t *buffer;
    size_t recordSize;  // Variable per record
} HybridRecord;

void encodeRecord(HybridRecord *rec, uint64_t id,
                  uint32_t timestamp, uint16_t value) {
    varintWidth idLen;
    varintSplitPut_(rec->buffer, idLen, id);

    // Fixed-size fields follow varint
    size_t offset = idLen;
    memcpy(rec->buffer + offset, &timestamp, 4);
    offset += 4;
    memcpy(rec->buffer + offset, &value, 2);
    offset += 2;

    rec->recordSize = offset;
}

void decodeRecord(const HybridRecord *rec, uint64_t *id,
                  uint32_t *timestamp, uint16_t *value) {
    varintWidth idLen;
    varintSplitGet_(rec->buffer, idLen, *id);

    size_t offset = idLen;
    memcpy(timestamp, rec->buffer + offset, 4);
    offset += 4;
    memcpy(value, rec->buffer + offset, 2);
}

// Record sizes: (idLen) + 4 + 2 = 7-15 bytes depending on ID
// Fixed size would be: 8 + 4 + 2 = 14 bytes
```

### Example 4: Reverse-Scanning Log Files

Build log files that can be scanned backwards:

```c
#include "varintSplit.h"

typedef struct {
    uint8_t *buffer;
    size_t capacity;
    size_t writePos;
} ReversibleLog;

void logInit(ReversibleLog *log, size_t capacity) {
    log->buffer = calloc(1, capacity);
    log->capacity = capacity;
    log->writePos = 0;
}

void logAppend(ReversibleLog *log, uint64_t timestamp, uint64_t eventId) {
    // Write timestamp (reversed split)
    varintWidth tsLen;
    varintSplitReversedPutForward_(log->buffer + log->writePos, tsLen, timestamp);
    log->writePos += tsLen;

    // Write event ID (reversed split)
    varintWidth idLen;
    varintSplitReversedPutForward_(log->buffer + log->writePos, idLen, eventId);
    log->writePos += idLen;
}

void logScanBackwards(ReversibleLog *log) {
    size_t pos = log->writePos;

    while (pos > 0) {
        // Scan backwards: type byte is last
        pos--;  // Move to type byte

        // Decode event ID (reversed)
        uint64_t eventId;
        varintWidth idLen;
        varintSplitReversedGet_(log->buffer + pos, idLen, eventId);
        pos -= (idLen - 1);  // Move back to start of eventId

        // Decode timestamp (reversed)
        pos--;
        uint64_t timestamp;
        varintWidth tsLen;
        varintSplitReversedGet_(log->buffer + pos, tsLen, timestamp);
        pos -= (tsLen - 1);

        printf("Event %lu at time %lu\n", eventId, timestamp);
    }
}
```

### Example 5: Boundary-Aware Compression

Use known boundaries to optimize for common ranges:

```c
#include "varintSplit.h"

// Optimize storage based on expected value distributions
typedef enum {
    RANGE_TINY,    // 0-63 (1 byte)
    RANGE_SMALL,   // 64-16,446 (2 bytes)
    RANGE_MEDIUM,  // 16,447-65,535 (3 bytes)
    RANGE_LARGE,   // 65,536+ (4+ bytes)
} ValueRange;

typedef struct {
    ValueRange expectedRange;
    uint8_t data[9];
    varintWidth actualLen;
} OptimizedValue;

void encodeOptimized(OptimizedValue *ov, uint64_t value) {
    varintSplitPut_(ov->data, ov->actualLen, value);

    // Determine actual range
    if (value <= 63) {
        ov->expectedRange = RANGE_TINY;
    } else if (value <= 16446) {
        ov->expectedRange = RANGE_SMALL;
    } else if (value <= 65535) {
        ov->expectedRange = RANGE_MEDIUM;
    } else {
        ov->expectedRange = RANGE_LARGE;
    }
}

void reportStatistics(OptimizedValue *values, size_t count) {
    size_t rangeCounts[4] = {0};
    size_t totalBytes = 0;

    for (size_t i = 0; i < count; i++) {
        rangeCounts[values[i].expectedRange]++;
        totalBytes += values[i].actualLen;
    }

    printf("Tiny (1B):   %zu values\n", rangeCounts[RANGE_TINY]);
    printf("Small (2B):  %zu values\n", rangeCounts[RANGE_SMALL]);
    printf("Medium (3B): %zu values\n", rangeCounts[RANGE_MEDIUM]);
    printf("Large (4B+): %zu values\n", rangeCounts[RANGE_LARGE]);
    printf("Total bytes: %zu (avg %.2f bytes/value)\n",
           totalBytes, (double)totalBytes / count);
}
```

### Example 6: Level-Based Prefetching

Use level detection for performance optimization:

```c
#include "varintSplit.h"

// Prefetch strategy based on encoding level
void prefetchOptimized(const uint8_t *buffer, size_t count) {
    size_t offset = 0;

    for (size_t i = 0; i < count; i++) {
        // Check encoding level
        uint8_t firstByte = buffer[offset];
        uint8_t prefix = firstByte & VARINT_SPLIT_MASK;  // Top 2 bits

        if (prefix == VARINT_SPLIT_6) {
            // Level 1: 1 byte, no prefetch needed
            offset += 1;
        } else if (prefix == VARINT_SPLIT_14) {
            // Level 2: 2 bytes, prefetch next cache line if near boundary
            offset += 2;
        } else {
            // Level 3: Variable length, prefetch more aggressively
            varintWidth len = varintSplitGetLenQuick_(buffer + offset);
            __builtin_prefetch(buffer + offset + len, 0, 1);
            offset += len;
        }
    }
}
```

## Split Variants

### varintSplitFull

Uses **ALL** first-byte values (including 11xxxxxx prefix):

- 6-bit embedded: 00xxxxxx (0-63)
- 14-bit embedded: 01xxxxxx (64-16,446)
- 22-bit embedded: 11xxxxxx (larger range in 3 bytes)
- External: 10000xxx (variable)

**Benefit**: Better 3-byte encoding (up to 4,276,284)

### varintSplitFullNoZero

Same as SplitFull but without zero optimization.

**Use when**: Zero values are rare.

### varintSplitFull16

Starts at **16-bit minimum** (no 1-byte encoding):

- 14-bit embedded: First level (2 bytes)
- 22-bit embedded: Second level (3 bytes)
- External: Variable (4-9 bytes)

**Use when**: All values are known to be ≥ 256.

## Performance Characteristics

### Space Efficiency

| Value Distribution     | Avg Bytes | Savings vs uint64_t |
| ---------------------- | --------- | ------------------- |
| 90% ≤ 63, 10% ≤ 16,446 | 1.1       | 86%                 |
| Uniform 0-10,000       | 2.0       | 75%                 |
| Uniform 0-100,000      | 3.0       | 62.5%               |

### Time Complexity

| Operation  | Complexity | Notes                |
| ---------- | ---------- | -------------------- |
| Encode     | O(1)       | Macro expands inline |
| Decode     | O(1)       | Macro expands inline |
| Get Length | O(1)       | Read first byte only |

### CPU Performance

Macros expand inline, eliminating function call overhead:

- **Encode**: ~2-4 cycles (inline)
- **Decode**: ~2-4 cycles (inline)
- **Length**: ~1 cycle (bit mask + shift)

## When to Use varintSplit

### Use When:

- You need **known bit boundaries** for custom packing
- Building **hybrid encodings** (varint + fixed data)
- Want to **reserve prefix values** for custom use
- Need **reverse-traversable** data structures
- Values cluster in 0-63 or 64-16,446 ranges

### Don't Use When:

- You need **sortable** encodings (use varintTagged)
- **Maximum space efficiency** required (use varintExternal)
- Values are uniformly large (overhead hurts)
- Function calls are acceptable (tagged/external simpler)

## Common Pitfalls

### Pitfall 1: Macro Argument Side Effects

```c
// WRONG: Macro evaluates arguments multiple times
varintSplitPut_(buf, len, getNextValue());  // getNextValue() called multiple times!

// RIGHT: Evaluate once before macro
uint64_t value = getNextValue();
varintSplitPut_(buf, len, value);
```

### Pitfall 2: Prefix Collision

```c
// WRONG: Using reserved 11xxxxxx prefix without checking
buf[0] = 0xC0 | someValue;  // Collides with SplitFull!

// RIGHT: Only use 11xxxxxx with base varintSplit (not SplitFull)
```

### Pitfall 3: Forgetting Cumulative Offsets

```c
// WRONG: Expecting value 16,000 in 2 bytes
// It's in level 2 which includes offset of 63, so max is 16,446

// RIGHT: Understand level boundaries
if (value <= 63) {
    // 1 byte (level 1)
} else if (value <= 16446) {
    // 2 bytes (level 2)
} else {
    // 3+ bytes (level 3 with external encoding)
}
```

## Implementation Details

### Source Files

- **Header**: `src/varintSplit.h` - Macro implementations
- **No .c file**: Fully macro-based, header-only

### Key Macros

```c
VARINT_SPLIT_MASK       // 0xC0 (top 2 bits)
VARINT_SPLIT_6_MASK     // 0x3F (bottom 6 bits)
VARINT_SPLIT_MAX_6      // 63
VARINT_SPLIT_MAX_14     // 16,446
```

### Encoding Tags

```c
typedef enum varintSplitTag {
    VARINT_SPLIT_6 = 0x00,    // 00xxxxxx
    VARINT_SPLIT_14 = 0x40,   // 01xxxxxx
    VARINT_SPLIT_VAR = 0x80,  // 10000xxx
} varintSplitTag;
```

## See Also

- [Architecture Overview](../ARCHITECTURE.md)
- [Choosing Varint Types](../CHOOSING_VARINTS.md)
- [varintTagged](varintTagged.md) - Sortable encoding
- [varintExternal](varintExternal.md) - Maximum efficiency
- [varintPacked](varintPacked.md) - Bit-level packing
