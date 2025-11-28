# varintBitstream: Bit-Level Stream Operations

## Overview

**varintBitstream** provides low-level primitives for reading and writing arbitrary-bit-width values at arbitrary bit offsets within byte arrays. Unlike varintPacked (which works with array indices), bitstream operates at **bit-granularity positioning**.

**Key Features**: Arbitrary bit offsets, variable bit widths per value, signed value support, can span multiple 64-bit slots.

## Key Characteristics

| Property       | Value                                 |
| -------------- | ------------------------------------- |
| Implementation | Header-only (static inline functions) |
| Positioning    | Bit-level offsets                     |
| Bit Widths     | 1-64 bits (arbitrary)                 |
| Slot Types     | Configurable (uint64_t default)       |
| Signed Support | Yes (via prepare/restore macros)      |

## API Reference

### Core Functions

```c
void varintBitstreamSet(vbits *buffer, size_t startBitOffset,
                        size_t bitsPerValue, vbitsVal value);
```

Write `value` (of `bitsPerValue` bits) at `startBitOffset` in `buffer`.

```c
vbitsVal varintBitstreamGet(const vbits *buffer, size_t startBitOffset,
                            size_t bitsPerValue);
```

Read `bitsPerValue` bits starting at `startBitOffset` from `buffer`.

### Type Configuration

```c
// Optional: Define custom slot type (default: uint64_t)
#define VBITS uint32_t      // Slot storage type
#define VBITSVAL uint32_t   // Value type
#include "varintBitstream.h"
```

### Signed Value Macros

```c
// Convert signed to unsigned (stores sign bit in top bit)
_varintBitstreamPrepareSigned(value, fullCompactBitWidth);

// Convert unsigned back to signed
_varintBitstreamRestoreSigned(value, fullCompactBitWidth);
```

## Real-World Examples

### Example 1: Custom Bit-Width Protocol

Encode a packet with mixed-width fields:

```c
#include "varintBitstream.h"

typedef struct {
    uint64_t buffer[4];  // 256 bits total
    size_t bitOffset;
} PacketBuilder;

void packetInit(PacketBuilder *p) {
    memset(p->buffer, 0, sizeof(p->buffer));
    p->bitOffset = 0;
}

void packetAddField(PacketBuilder *p, uint64_t value, size_t bits) {
    varintBitstreamSet(p->buffer, p->bitOffset, bits, value);
    p->bitOffset += bits;
}

void buildPacket(PacketBuilder *p) {
    packetInit(p);

    // Protocol: [version:3][type:5][length:12][data...]
    packetAddField(p, 2, 3);      // Version 2 (3 bits)
    packetAddField(p, 7, 5);      // Type 7 (5 bits)
    packetAddField(p, 1024, 12);  // Length 1024 (12 bits)
    packetAddField(p, 42, 8);     // Data byte (8 bits)

    // Total: 3+5+12+8 = 28 bits = 4 bytes (vs 8 bytes with byte alignment)
}

void readPacket(const PacketBuilder *p) {
    size_t offset = 0;

    uint64_t version = varintBitstreamGet(p->buffer, offset, 3);
    offset += 3;

    uint64_t type = varintBitstreamGet(p->buffer, offset, 5);
    offset += 5;

    uint64_t length = varintBitstreamGet(p->buffer, offset, 12);
    offset += 12;

    printf("Version: %lu, Type: %lu, Length: %lu\n", version, type, length);
}
```

### Example 2: Compression with Variable Bit Widths

Store integers using optimal bit width per value:

```c
#include "varintBitstream.h"

typedef struct {
    uint64_t *buffer;
    size_t bitOffset;
    size_t capacity;
} BitCompressor;

void compressorInit(BitCompressor *c, size_t estimatedBits) {
    size_t slots = (estimatedBits + 63) / 64;
    c->buffer = calloc(slots, sizeof(uint64_t));
    c->bitOffset = 0;
    c->capacity = slots * 64;
}

size_t bitsNeeded(uint64_t value) {
    if (value == 0) return 1;
    return 64 - __builtin_clzll(value);  // Count leading zeros
}

void compressorAdd(BitCompressor *c, uint64_t value) {
    size_t bits = bitsNeeded(value);

    // Store bit width (6 bits: 0-64)
    varintBitstreamSet(c->buffer, c->bitOffset, 6, bits);
    c->bitOffset += 6;

    // Store value
    varintBitstreamSet(c->buffer, c->bitOffset, bits, value);
    c->bitOffset += bits;
}

uint64_t compressorRead(BitCompressor *c, size_t *offset) {
    // Read bit width
    size_t bits = varintBitstreamGet(c->buffer, *offset, 6);
    *offset += 6;

    // Read value
    uint64_t value = varintBitstreamGet(c->buffer, *offset, bits);
    *offset += bits;

    return value;
}

void demonstrateCompression() {
    BitCompressor c;
    compressorInit(&c, 1024);

    // Add values with different sizes
    compressorAdd(&c, 7);       // 6 + 3 = 9 bits
    compressorAdd(&c, 255);     // 6 + 8 = 14 bits
    compressorAdd(&c, 65535);   // 6 + 16 = 22 bits

    // Total: 9 + 14 + 22 = 45 bits = 6 bytes
    // vs 3 × 8 bytes = 24 bytes uncompressed
    // Savings: 75%

    size_t offset = 0;
    printf("%lu\n", compressorRead(&c, &offset));  // 7
    printf("%lu\n", compressorRead(&c, &offset));  // 255
    printf("%lu\n", compressorRead(&c, &offset));  // 65535

    free(c.buffer);
}
```

### Example 3: Signed Value Encoding

Store signed values using sign-bit encoding:

```c
#include "varintBitstream.h"

void storeSignedValue(uint64_t *buffer, size_t bitOffset,
                      size_t bits, int64_t signedValue) {
    // Prepare: Move sign bit to top of our bit width
    int64_t value = signedValue;
    if (value < 0) {
        _varintBitstreamPrepareSigned(value, bits);
    }

    varintBitstreamSet(buffer, bitOffset, bits, (uint64_t)value);
}

int64_t loadSignedValue(const uint64_t *buffer, size_t bitOffset, size_t bits) {
    uint64_t value = varintBitstreamGet(buffer, bitOffset, bits);

    // Restore: Move sign bit back to native position
    int64_t result = (int64_t)value;
    _varintBitstreamRestoreSigned(result, bits);

    return result;
}

void demonstrateSigned() {
    uint64_t buffer[2] = {0};

    // Store -100 in 10 bits
    storeSignedValue(buffer, 0, 10, -100);

    // Retrieve
    int64_t value = loadSignedValue(buffer, 0, 10);
    printf("Value: %ld\n", value);  // -100
}
```

### Example 4: Bitfield Packing

Pack multiple boolean flags and small enums:

```c
#include "varintBitstream.h"

typedef enum {
    STATE_IDLE = 0,
    STATE_ACTIVE = 1,
    STATE_WAITING = 2,
    STATE_ERROR = 3
} State;

typedef struct {
    uint64_t bits;  // Single 64-bit value holds all fields
} StatusFlags;

void flagsInit(StatusFlags *f) {
    f->bits = 0;
}

void flagsSetBool(StatusFlags *f, size_t offset, bool value) {
    varintBitstreamSet(&f->bits, offset, 1, value ? 1 : 0);
}

void flagsSetState(StatusFlags *f, size_t offset, State state) {
    varintBitstreamSet(&f->bits, offset, 2, state);  // 2 bits for 4 states
}

bool flagsGetBool(const StatusFlags *f, size_t offset) {
    return varintBitstreamGet(&f->bits, offset, 1) != 0;
}

State flagsGetState(const StatusFlags *f, size_t offset) {
    return (State)varintBitstreamGet(&f->bits, offset, 2);
}

void demonstrateFlags() {
    StatusFlags flags;
    flagsInit(&flags);

    // Pack into 64 bits:
    size_t offset = 0;

    flagsSetBool(&flags, offset, true);    // enabled: 1 bit
    offset += 1;

    flagsSetBool(&flags, offset, false);   // debug: 1 bit
    offset += 1;

    flagsSetState(&flags, offset, STATE_ACTIVE);  // state: 2 bits
    offset += 2;

    flagsSetBool(&flags, offset, true);    // connected: 1 bit
    offset += 1;

    // Total: 5 bits in a single 64-bit value
    // Can pack 12 more bools and 3 more states!

    // Read back
    offset = 0;
    printf("Enabled: %d\n", flagsGetBool(&flags, offset));
    offset += 1;
    offset += 1;  // Skip debug
    printf("State: %d\n", flagsGetState(&flags, offset));
}
```

### Example 5: Spanning Multiple Slots

Write values that cross 64-bit boundaries:

```c
#include "varintBitstream.h"

void demonstrateSpanning() {
    uint64_t buffer[3] = {0};

    // Write a 40-bit value starting at bit 50
    // This spans from buffer[0] bit 50 to buffer[1] bit 25
    uint64_t largeValue = 0xABCDEF1234ULL;  // 40 bits
    varintBitstreamSet(buffer, 50, 40, largeValue);

    // bitstream automatically handles:
    // - Writing 14 bits to end of buffer[0]
    // - Writing remaining 26 bits to start of buffer[1]

    // Read back
    uint64_t retrieved = varintBitstreamGet(buffer, 50, 40);
    printf("Value: 0x%lX\n", retrieved);  // 0xABCDEF1234
}
```

### Example 6: Custom Slot Size

Use 32-bit slots for smaller memory footprint:

```c
#define VBITS uint32_t
#define VBITSVAL uint32_t
#include "varintBitstream.h"

void demonstrateCustomSlots() {
    uint32_t buffer[4] = {0};  // 4 × 32 = 128 bits

    // Pack 12-bit values
    varintBitstreamSet(buffer, 0, 12, 2048);
    varintBitstreamSet(buffer, 12, 12, 3000);
    varintBitstreamSet(buffer, 24, 12, 1500);

    // Can fit 10 × 12-bit values in 128 bits
    // vs 10 × uint16_t = 160 bits

    uint32_t val1 = varintBitstreamGet(buffer, 0, 12);
    uint32_t val2 = varintBitstreamGet(buffer, 12, 12);
    uint32_t val3 = varintBitstreamGet(buffer, 24, 12);

    printf("%u, %u, %u\n", val1, val2, val3);  // 2048, 3000, 1500
}
```

## Performance Characteristics

### Time Complexity

| Operation | Complexity | Notes                   |
| --------- | ---------- | ----------------------- |
| Set       | O(1)       | May touch 1-2 slots     |
| Get       | O(1)       | May read from 1-2 slots |

### Bit Boundary Handling

- **Within single slot**: ~5-7 cycles (single write)
- **Spanning slots**: ~10-15 cycles (two writes with masking)

### Space Efficiency

**Perfect**: Only uses exactly the bits needed, no wasted space (except up to 7 bits for byte alignment).

## When to Use varintBitstream

### Use When:

- Need **arbitrary bit offsets** (not just array indices)
- Building **custom protocols** with mixed-width fields
- **Bit-level compression** required
- Values have **different bit widths** each
- Need to pack **sub-byte values** tightly

### Don't Use When:

- All values have **same bit width** (use varintPacked instead)
- Working with **array indices** (use varintPacked instead)
- Byte-aligned access is sufficient
- Performance is extremely critical (byte-aligned is faster)

## Comparison: varintBitstream vs varintPacked

| Feature     | varintBitstream    | varintPacked              |
| ----------- | ------------------ | ------------------------- |
| Positioning | Bit offsets        | Array indices             |
| Bit Width   | Variable per value | Fixed per array           |
| Use Case    | Protocols, streams | Arrays, sets              |
| Complexity  | Lower-level        | Higher-level              |
| Performance | Slightly slower    | Optimized for fixed width |

## Common Pitfalls

### Pitfall 1: Bit Offset Arithmetic

```c
// WRONG: Forgetting to advance offset
varintBitstreamSet(buffer, 0, 10, value1);
varintBitstreamSet(buffer, 0, 10, value2);  // OVERWRITES value1!

// RIGHT: Track offset manually
size_t offset = 0;
varintBitstreamSet(buffer, offset, 10, value1);
offset += 10;
varintBitstreamSet(buffer, offset, 10, value2);
offset += 10;
```

### Pitfall 2: Buffer Overflow

```c
// WRONG: Not allocating enough slots
uint64_t buffer[1];  // Only 64 bits
varintBitstreamSet(buffer, 60, 10, value);  // Spans to bit 70 - OVERFLOW!

// RIGHT: Calculate slots needed
size_t maxBitOffset = 60 + 10;
size_t slotsNeeded = (maxBitOffset + 63) / 64;
uint64_t *buffer = calloc(slotsNeeded, sizeof(uint64_t));
```

### Pitfall 3: Value Too Large

```c
// WRONG: Value doesn't fit in bit width
varintBitstreamSet(buffer, 0, 8, 300);  // 300 > 255 (8-bit max) - ASSERT!

// RIGHT: Check value fits
uint64_t value = 300;
size_t bits = 10;  // 300 fits in 10 bits
assert(value < (1ULL << bits));
varintBitstreamSet(buffer, 0, bits, value);
```

## Implementation Details

### Source Files

- **Header-only**: `src/varintBitstream.h`
- All functions are `static inline`

### Bit Layout

Values are stored **high-to-low** within slots (similar to varintPacked).

### Slot Spanning

When a value crosses a slot boundary:

1. Calculate bits in current slot vs next slot
2. Split value into high/low parts
3. Write each part with appropriate masking

## See Also

- [Architecture Overview](../ARCHITECTURE.md)
- [Choosing Varint Types](../CHOOSING_VARINTS.md)
- [varintPacked](varintPacked.md) - Higher-level array packing
- [varintDimension](varintDimension.md) - Matrix packing
- [varintExternal](varintExternal.md) - Byte-level variable width
