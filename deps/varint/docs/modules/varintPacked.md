# varintPacked: Fixed-Width Bit-Packed Integer Arrays

## Overview

**varintPacked** provides template-based bit-packing for arrays of fixed-width integers at **arbitrary bit widths** (not just 8/16/32/64). Store thousands of 12-bit integers, 14-bit values, or any bit width from 1-64 bits.

**Key Innovation**: Header-only template system using C macros to generate specialized functions for any bit width at compile time.

## Key Characteristics

| Property       | Value                                          |
| -------------- | ---------------------------------------------- |
| Implementation | Header-only template (macro-based)             |
| Bit Widths     | 1-64 bits (arbitrary)                          |
| Features       | Set, Get, Binary Search, Sorted Insert, Delete |
| Optimization   | Inline expansion, zero function call overhead  |
| Use Case       | Large arrays of bounded integers               |

## Quick Example

```c
// Generate 12-bit packing functions
#define PACK_STORAGE_BITS 12
#include "varintPacked.h"

// Creates: varintPacked12Set(), varintPacked12Get(), etc.

uint8_t *array = calloc(1000, 12 / 8 + 1);  // Storage for packed values

// Store values (0-4095 in 12 bits)
varintPacked12Set(array, 0, 2048);  // array[0] = 2048
varintPacked12Set(array, 1, 3000);  // array[1] = 3000

// Retrieve values
uint16_t val = varintPacked12Get(array, 0);  // 2048
```

## Template System

The module is a **C macro template**. You configure it with `#define` before including:

### Configuration Macros

```c
#define PACK_STORAGE_BITS 12        // Required: Bit width (1-64)
#define PACK_STORAGE_COMPACT        // Optional: Use minimum storage slots
#define PACK_STORAGE_SLOT_STORAGE_TYPE uint8_t  // Optional: Slot type (default: uint32_t)
#define PACK_MAX_ELEMENTS 10000     // Optional: Max elements (optimizes length type)
#define PACK_STATIC                 // Optional: Make functions static
#include "varintPacked.h"
```

### Generated Functions

Including `varintPacked.h` generates these functions:

```c
// Basic operations
void varintPacked{N}Set(void *array, uint32_t index, uint{N}_t value);
uint{N}_t varintPacked{N}Get(const void *array, uint32_t index);

// Arithmetic operations
void varintPacked{N}SetIncr(void *array, uint32_t index, int64_t delta);
void varintPacked{N}SetHalf(void *array, uint32_t index);

// Array operations
void varintPacked{N}Insert(void *array, uint32_t count, uint32_t index, uint{N}_t value);
void varintPacked{N}InsertSorted(void *array, uint32_t count, uint{N}_t value);
void varintPacked{N}Delete(void *array, uint32_t count, uint32_t index);
bool varintPacked{N}DeleteMember(void *array, uint32_t count, uint{N}_t value);

// Search operations
int64_t varintPacked{N}Member(const void *array, uint32_t count, uint{N}_t value);
uint32_t varintPacked{N}BinarySearch(const void *array, uint32_t count, uint{N}_t value);

// Utility
size_t varintPacked{N}CountFromStorageBytes(size_t bytes);
```

where `{N}` is replaced by `PACK_STORAGE_BITS` (e.g., `varintPacked12Set` for 12-bit packing).

## Real-World Examples

### Example 1: IP Address Components

Store IPv4 address octets (0-255) efficiently:

```c
// Each octet needs 8 bits - perfect fit!
#define PACK_STORAGE_BITS 8
#include "varintPacked.h"

typedef struct {
    uint8_t packed[4];  // 4 octets × 8 bits = 32 bits = 4 bytes
} IPv4Packed;

void packIPv4(IPv4Packed *ip, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    varintPacked8Set(ip->packed, 0, a);
    varintPacked8Set(ip->packed, 1, b);
    varintPacked8Set(ip->packed, 2, c);
    varintPacked8Set(ip->packed, 3, d);
}

void unpackIPv4(const IPv4Packed *ip, uint8_t *a, uint8_t *b,
                uint8_t *c, uint8_t *d) {
    *a = varintPacked8Get(ip->packed, 0);
    *b = varintPacked8Get(ip->packed, 1);
    *c = varintPacked8Get(ip->packed, 2);
    *d = varintPacked8Get(ip->packed, 3);
}

// Note: For 8-bit values, you'd normally use uint8_t[4], but this
// demonstrates the API. The real power is for non-native bit widths.
```

### Example 2: Game World Coordinates

Store bounded 3D coordinates (0-4095 per axis):

```c
#define PACK_STORAGE_BITS 12
#include "varintPacked.h"

// 3 coordinates × 12 bits = 36 bits = 5 bytes (vs 12 bytes for 3× uint32_t)
typedef struct {
    uint8_t packed[5];  // ceil(36 / 8) = 5 bytes
} WorldPosition;

void setPosition(WorldPosition *pos, uint16_t x, uint16_t y, uint16_t z) {
    assert(x < 4096 && y < 4096 && z < 4096);  // Must fit in 12 bits

    varintPacked12Set(pos->packed, 0, x);
    varintPacked12Set(pos->packed, 1, y);
    varintPacked12Set(pos->packed, 2, z);
}

void getPosition(const WorldPosition *pos, uint16_t *x, uint16_t *y, uint16_t *z) {
    *x = varintPacked12Get(pos->packed, 0);
    *y = varintPacked12Get(pos->packed, 1);
    *z = varintPacked12Get(pos->packed, 2);
}

// Storage savings for 1 million positions:
// uint32_t[3]: 1M × 12 bytes = 12 MB
// 12-bit packed: 1M × 5 bytes = 5 MB
// Savings: 58%
```

### Example 3: Sensor Readings with Known Bounds

Store 10,000 temperature readings (0-16,383 = 14 bits):

```c
#define PACK_STORAGE_BITS 14
#include "varintPacked.h"

typedef struct {
    uint8_t *readings;  // Dynamically allocated
    size_t capacity;
    size_t count;
} SensorData;

void sensorInit(SensorData *data, size_t maxReadings) {
    // 14 bits per reading, round up to bytes
    size_t bytesNeeded = (maxReadings * 14 + 7) / 8;
    data->readings = calloc(1, bytesNeeded);
    data->capacity = maxReadings;
    data->count = 0;
}

void sensorAddReading(SensorData *data, uint16_t reading) {
    assert(reading < 16384);  // 14-bit max
    assert(data->count < data->capacity);

    varintPacked14Set(data->readings, data->count, reading);
    data->count++;
}

uint16_t sensorGetReading(const SensorData *data, size_t index) {
    assert(index < data->count);
    return varintPacked14Get(data->readings, index);
}

void demonstrateSensor() {
    SensorData data;
    sensorInit(&data, 10000);

    // Add 10,000 readings
    for (size_t i = 0; i < 10000; i++) {
        sensorAddReading(&data, i % 16384);
    }

    // Storage: 10,000 × 14 bits = 140,000 bits = 17,500 bytes
    // vs uint16_t: 10,000 × 2 bytes = 20,000 bytes
    // Savings: 12.5%

    free(data.readings);
}
```

### Example 4: Sorted Integer Set with Binary Search

Maintain a sorted set of 13-bit integers with fast membership testing:

```c
#define PACK_STORAGE_BITS 13
#include "varintPacked.h"

typedef struct {
    uint8_t *data;
    size_t capacity;     // In elements
    size_t count;
} SortedSet;

void setInit(SortedSet *set, size_t maxElements) {
    size_t bytes = (maxElements * 13 + 7) / 8;
    set->data = calloc(1, bytes);
    set->capacity = maxElements;
    set->count = 0;
}

bool setInsert(SortedSet *set, uint16_t value) {
    assert(value < 8192);  // 13-bit max

    // Check if already exists
    if (setContains(set, value)) {
        return false;  // Already in set
    }

    // Insert in sorted order
    varintPacked13InsertSorted(set->data, set->count, value);
    set->count++;
    return true;
}

bool setContains(const SortedSet *set, uint16_t value) {
    int64_t pos = varintPacked13Member(set->data, set->count, value);
    return pos >= 0;
}

bool setRemove(SortedSet *set, uint16_t value) {
    bool removed = varintPacked13DeleteMember(set->data, set->count, value);
    if (removed) {
        set->count--;
    }
    return removed;
}

// Binary search is O(log n) and very cache-friendly
void demonstrateSortedSet() {
    SortedSet set;
    setInit(&set, 1000);

    // Insert random values
    setInsert(&set, 500);
    setInsert(&set, 100);
    setInsert(&set, 750);
    setInsert(&set, 250);

    // Array is kept sorted: [100, 250, 500, 750]

    printf("Contains 250? %s\n", setContains(&set, 250) ? "Yes" : "No");  // Yes
    printf("Contains 300? %s\n", setContains(&set, 300) ? "Yes" : "No");  // No

    setRemove(&set, 250);
    // Now: [100, 500, 750]

    free(set.data);
}
```

### Example 5: Compact vs Standard Storage

Choose storage slot size for space/speed tradeoff:

```c
// COMPACT: Minimum storage, potentially slower
#define PACK_STORAGE_BITS 12
#define PACK_STORAGE_COMPACT
#define PACK_FUNCTION_PREFIX varintPackedCompact
#include "varintPacked.h"
#undef PACK_STORAGE_BITS
#undef PACK_STORAGE_COMPACT
#undef PACK_FUNCTION_PREFIX

// STANDARD: Uses uint32_t slots, faster
#define PACK_STORAGE_BITS 12
#define PACK_FUNCTION_PREFIX varintPackedFast
#include "varintPacked.h"
#undef PACK_STORAGE_BITS
#undef PACK_FUNCTION_PREFIX

void compareStorageTypes() {
    // Compact: Uses uint8_t slots for 12-bit values
    // Storing one value: 2 bytes (16 bits) for 12-bit value
    uint8_t compact[2];
    varintPackedCompact12Set(compact, 0, 2048);

    // Fast: Uses uint32_t slots for 12-bit values
    // Storing one value: 4 bytes (32 bits) for 12-bit value
    uint32_t fast[1];
    varintPackedFast12Set(fast, 0, 2048);

    // Tradeoff:
    // Compact: 2 bytes (more allocations, slightly slower)
    // Fast:    4 bytes (fewer allocations, faster bitwise ops)

    // For large arrays (1000+ elements), difference is minimal
    // For small arrays (1-10 elements), compact saves space
}
```

### Example 6: Multiple Bit Widths in One File

Use the template multiple times:

```c
// 10-bit packing
#define PACK_STORAGE_BITS 10
#include "varintPacked.h"
#undef PACK_STORAGE_BITS

// 14-bit packing
#define PACK_STORAGE_BITS 14
#include "varintPacked.h"
#undef PACK_STORAGE_BITS

// 20-bit packing
#define PACK_STORAGE_BITS 20
#include "varintPacked.h"
#undef PACK_STORAGE_BITS

// Now have: varintPacked10*, varintPacked14*, varintPacked20* functions

void demonstrateMultipleWidths() {
    uint8_t array10[200];  // For 10-bit values
    uint8_t array14[200];  // For 14-bit values
    uint8_t array20[300];  // For 20-bit values

    varintPacked10Set(array10, 0, 1023);      // Max 10-bit value
    varintPacked14Set(array14, 0, 16383);     // Max 14-bit value
    varintPacked20Set(array20, 0, 1048575);   // Max 20-bit value
}
```

## Performance Characteristics

### Time Complexity

| Operation    | Complexity | Notes                               |
| ------------ | ---------- | ----------------------------------- |
| Set/Get      | O(1)       | Constant time bit operations        |
| InsertSorted | O(n)       | Binary search O(log n) + shift O(n) |
| Member       | O(log n)   | Binary search                       |
| Delete       | O(n)       | Shift elements                      |

### Space Efficiency

For N elements with B bits each:

- **Storage**: ⌈(N × B) / 8⌉ bytes
- **Overhead**: At most 7 bits (< 1 byte) total

**Example**: 10,000 elements at 13 bits each:

- Calculation: (10,000 × 13) / 8 = 16,250 bytes
- vs uint16_t: 10,000 × 2 = 20,000 bytes
- Savings: 18.75%

### CPU Performance

Inlined macro expansion provides:

- **Set**: ~5-10 cycles
- **Get**: ~5-10 cycles
- **Binary search**: ~8-12 cycles per iteration

## Storage Layout

Values are packed **right to left** within slots:

```
12-bit values in 32-bit slots:

Value 0: [00000000000000000000111111111111]  (bits 0-11)
Value 1: [00000000000011111111111100000000]  (bits 12-23)
Value 2: [11111111111100000000000000000000]  (bits 24-31 + overflow)

Across slot boundary:
Slot 0: [11111111111100000000111111111111]
Slot 1: [00000000000000000000000000001111]
```

## When to Use varintPacked

### Use When:

- Storing **large arrays** (1000+ elements) of bounded integers
- Bit width is **not a power of 2** (12, 13, 14, 20, etc.)
- Need **sorted** data structures with binary search
- **Memory savings** justify bit manipulation overhead
- Values have **known maximum** bounds

### Don't Use When:

- Values are **variable width** (use varints instead)
- Bit width is **native** (8/16/32/64 - use normal arrays)
- Array is **small** (< 100 elements - overhead not worth it)
- Need **random insert/delete** frequently (O(n) is too slow)

## Common Pitfalls

### Pitfall 1: Value Overflow

```c
#define PACK_STORAGE_BITS 10
#include "varintPacked.h"

// WRONG: Value exceeds 10-bit max (1023)
varintPacked10Set(array, 0, 2000);  // ASSERTION FAILURE!

// RIGHT: Check bounds
uint16_t value = 2000;
assert(value < (1 << 10));  // Check before setting
```

### Pitfall 2: Incorrect Storage Allocation

```c
#define PACK_STORAGE_BITS 12
#include "varintPacked.h"

// WRONG: Not enough bytes for 1000 elements
uint8_t array[1000];  // Only 8000 bits, need 12000!

// RIGHT: Calculate properly
size_t elements = 1000;
size_t bitsNeeded = elements * 12;
size_t bytesNeeded = (bitsNeeded + 7) / 8;  // Round up
uint8_t *array = malloc(bytesNeeded);  // 1500 bytes
```

### Pitfall 3: Template Pollution

```c
// WRONG: Forgetting to #undef
#define PACK_STORAGE_BITS 12
#include "varintPacked.h"
// ... later ...
#include "varintPacked.h"  // ERROR: Redefinition!

// RIGHT: Always #undef after including
#define PACK_STORAGE_BITS 12
#include "varintPacked.h"
#undef PACK_STORAGE_BITS
```

## Implementation Details

### Source Files

- **Header-only**: `src/varintPacked.h`

### Macro Mechanism

- Generates functions using token pasting (`##`)
- Function names: `PACK_FUNCTION_PREFIX` + `PACK_STORAGE_BITS` + operation
- Inline expansion for zero-overhead abstraction

### Value Types

Automatically selected based on bit width:

- 1-8 bits: `uint8_t`
- 9-16 bits: `uint16_t`
- 17-32 bits: `uint32_t`
- 33-64 bits: `uint64_t`

## See Also

- [Architecture Overview](../ARCHITECTURE.md)
- [Choosing Varint Types](../CHOOSING_VARINTS.md)
- [varintDimension](varintDimension.md) - Matrix packing using packed arrays
- [varintBitstream](varintBitstream.md) - Lower-level bit operations
- [varintExternal](varintExternal.md) - Variable-width alternative
