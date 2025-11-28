# varintTagged: Sortable Big-Endian Varints

## Overview

**varintTagged** is a variable-length integer encoding where metadata is stored in the first byte alongside value data. The encoding is big-endian, making it **memcmp()-sortable** - a critical feature for database keys and ordered data structures.

**Origin**: Adapted from the abandoned SQLite4 project.

## Key Characteristics

| Property         | Value                  |
| ---------------- | ---------------------- |
| Metadata Storage | First byte             |
| Endianness       | Big-endian             |
| Size Range       | 1-9 bytes              |
| 1-Byte Maximum   | 240                    |
| Sortable         | Yes (memcmp)           |
| Performance      | Fast (O(1) operations) |

## Encoding Format

### Byte-Width Maximums

| Bytes | Maximum Value                       | Percentage of uint64_t |
| ----- | ----------------------------------- | ---------------------- |
| 1     | 240                                 | 0.0000013%             |
| 2     | 2,287                               | 0.000012%              |
| 3     | 67,823                              | 0.00037%               |
| 4     | 16,777,215 (2^24-1)                 | 0.09%                  |
| 5     | 4,294,967,295 (2^32-1)              | 23%                    |
| 6     | 1,099,511,627,775 (2^40-1)          | 5,960%                 |
| 7     | 281,474,976,710,655 (2^48-1)        | 1,527,099%             |
| 8     | 72,057,594,037,927,935 (2^56-1)     | 390,937,991%           |
| 9     | 18,446,744,073,709,551,615 (2^64-1) | 100%                   |

### Encoding Rules

**First byte determines everything:**

```
0-240:      Value itself (241 possible values)
241-248:    2-byte encoding (8 type values × 256 = 2,048 values)
249:        3-byte encoding
250-255:    4-9 byte encodings
```

**Why this works:** The first byte acts as both metadata AND value storage, maximizing efficiency for small numbers.

## API Reference

### Encoding Functions

```c
varintWidth varintTaggedPut64(uint8_t *buffer, uint64_t value);
```

Encodes a 64-bit unsigned integer. Returns the number of bytes used (1-9).

```c
varintWidth varintTaggedPut64FixedWidth(uint8_t *buffer, uint64_t value, varintWidth width);
```

Encodes at a specific width (useful for maintaining alignment or fixed-size slots).

```c
varintWidth varintTaggedPut32(uint8_t *buffer, uint32_t value);
```

Optimized encoder for 32-bit values (max 5 bytes).

### Decoding Functions

```c
varintWidth varintTaggedGet64(const uint8_t *buffer, uint64_t *result);
```

Decodes a tagged varint, returning the width and storing the value in `result`.

```c
uint64_t varintTaggedGet64ReturnValue(const uint8_t *buffer);
```

Decodes and directly returns the value (convenience function).

```c
varintWidth varintTaggedGet32(const uint8_t *buffer, uint32_t *result);
```

Decodes to 32-bit value.

### Utility Functions

```c
varintWidth varintTaggedLen(uint64_t value);
```

Calculates how many bytes would be needed to encode a value WITHOUT encoding it.

```c
varintWidth varintTaggedGetLen(const uint8_t *buffer);
```

Gets the length of an already-encoded varint by reading just the first byte.

```c
#define varintTaggedLenQuick(value)
```

Macro for fast length calculation for values ≤ 16,777,215 (4 bytes).

```c
#define varintTaggedGetLenQuick_(buffer)
```

Macro for fast length extraction from encoded buffer.

### Arithmetic Functions

```c
varintWidth varintTaggedAddNoGrow(uint8_t *buffer, int64_t add);
```

Adds `add` to the encoded value in-place. Returns new width. **Asserts if result would grow beyond original width.**

```c
varintWidth varintTaggedAddGrow(uint8_t *buffer, int64_t add);
```

Adds `add` to the encoded value, allowing the encoding to grow if needed.

## Real-World Examples

### Example 1: Database Auto-Increment IDs

Store user IDs that start small but can grow unbounded:

```c
#include "varintTagged.h"
#include <stdio.h>

typedef struct {
    uint8_t userIdVarint[9];  // Max size
    char username[32];
    uint64_t created_timestamp;
} UserRecord;

void saveUser(UserRecord *record, uint64_t userId, const char *name) {
    // Encode user ID - typically small (1-2 bytes for first 2000 users)
    varintWidth width = varintTaggedPut64(record->userIdVarint, userId);
    printf("User ID %lu stored in %d bytes\n", userId, width);

    strncpy(record->username, name, 31);
    record->created_timestamp = time(NULL);
}

uint64_t getUserId(const UserRecord *record) {
    uint64_t userId;
    varintTaggedGet64(record->userIdVarint, &userId);
    return userId;
}

int main() {
    UserRecord users[3];

    saveUser(&users[0], 1, "alice");        // 1 byte
    saveUser(&users[1], 240, "bob");        // 1 byte
    saveUser(&users[2], 241, "charlie");    // 2 bytes
    saveUser(&users[3], 1000000, "david");  // 4 bytes

    // Storage savings: 4 users × 8 bytes = 32 bytes (uint64_t)
    //              vs: 1+1+2+4 = 8 bytes (varintTagged)
    // Savings: 75% for this typical distribution
}
```

### Example 2: Sortable Database Keys

Create composite keys that sort correctly with memcmp:

```c
#include "varintTagged.h"
#include <string.h>

// Composite key: (table_id, row_id)
typedef struct {
    uint8_t encoded[18];  // Max: 9 bytes each
    size_t totalLen;
} CompositeKey;

void createKey(CompositeKey *key, uint64_t tableId, uint64_t rowId) {
    varintWidth w1 = varintTaggedPut64(key->encoded, tableId);
    varintWidth w2 = varintTaggedPut64(key->encoded + w1, rowId);
    key->totalLen = w1 + w2;
}

int compareKeys(const CompositeKey *a, const CompositeKey *b) {
    // Works because varintTagged is big-endian and sortable!
    size_t minLen = a->totalLen < b->totalLen ? a->totalLen : b->totalLen;
    int cmp = memcmp(a->encoded, b->encoded, minLen);
    if (cmp != 0) return cmp;
    return (a->totalLen > b->totalLen) - (a->totalLen < b->totalLen);
}

void demonstrateSorting() {
    CompositeKey keys[4];
    createKey(&keys[0], 1, 100);
    createKey(&keys[1], 1, 200);
    createKey(&keys[2], 2, 50);
    createKey(&keys[3], 1, 150);

    // Sort using compareKeys - maintains (table_id, row_id) order
    // Result order: (1,100), (1,150), (1,200), (2,50)

    // This property is CRITICAL for B-tree implementations!
}
```

### Example 3: Time-Series Data

Store timestamps efficiently (they grow monotonically but slowly):

```c
#include "varintTagged.h"
#include <time.h>

typedef struct {
    uint8_t timestampVarint[9];
    float temperature;
    float humidity;
} SensorReading;

void recordReading(SensorReading *reading, time_t timestamp,
                   float temp, float humidity) {
    // Unix timestamps are currently ~1.7 billion (31 bits)
    // Fits in 5 bytes with varintTagged
    varintWidth width = varintTaggedPut64(reading->timestampVarint,
                                          (uint64_t)timestamp);
    // width = 5 for current timestamps (saves 3 bytes vs uint64_t)

    reading->temperature = temp;
    reading->humidity = humidity;
}

// For 1 million readings:
// uint64_t timestamps: 8 MB
// varintTagged:        5 MB
// Savings:             3 MB (37.5%)
```

### Example 4: In-Place Increment (Counters)

Efficiently update counters without re-encoding:

```c
#include "varintTagged.h"

typedef struct {
    char url[256];
    uint8_t hitCountVarint[9];
} URLStats;

void initURLStats(URLStats *stats, const char *url) {
    strncpy(stats->url, url, 255);
    varintTaggedPut64(stats->hitCountVarint, 0);  // Starts as 1 byte
}

void recordHit(URLStats *stats) {
    // Add 1 to counter in-place
    // Most counters stay small, so this usually doesn't grow
    varintWidth newWidth = varintTaggedAddGrow(stats->hitCountVarint, 1);

    // newWidth changes only at boundaries:
    // 240 -> 241: grows from 1 to 2 bytes
    // 2287 -> 2288: grows from 2 to 3 bytes
    // etc.
}

uint64_t getHitCount(const URLStats *stats) {
    return varintTaggedGet64ReturnValue(stats->hitCountVarint);
}

void demonstrateCounters() {
    URLStats homepage;
    initURLStats(&homepage, "/index.html");

    // Simulate 1000 hits
    for (int i = 0; i < 1000; i++) {
        recordHit(&homepage);
    }

    printf("Total hits: %lu\n", getHitCount(&homepage));
    // Stored in 2 bytes (240 < 1000 < 2287)
}
```

### Example 5: Fast Path Optimization

Use quick macros for common small values:

```c
#include "varintTagged.h"

// Process millions of small integers quickly
void processSmallIntegers(const uint64_t *values, size_t count) {
    uint8_t buffer[9];

    for (size_t i = 0; i < count; i++) {
        varintWidth width;

        // Most values are small - use fast path when possible
        if (values[i] <= VARINT_TAGGED_MAX_4) {  // <= 16,777,215
            // Use inline macro - no function call overhead
            width = varintTaggedLenQuick(values[i]);
            varintTaggedPut64FixedWidthQuick_(buffer, values[i], width);
        } else {
            // Rare large value - use full function
            width = varintTaggedPut64(buffer, values[i]);
        }

        // Process encoded buffer...
    }
}
```

### Example 6: Fixed-Width Encoding for Alignment

Maintain fixed-size slots for in-place updates:

```c
#include "varintTagged.h"

typedef struct {
    uint8_t balance[9];  // Always 9 bytes for max uint64_t
} BankAccount;

void initAccount(BankAccount *account, uint64_t initialBalance) {
    // Encode as 9 bytes even if value is small
    // This allows in-place updates without reallocation
    varintTaggedPut64FixedWidth(account->balance, initialBalance,
                                VARINT_WIDTH_72B);  // 9 bytes
}

void deposit(BankAccount *account, uint64_t amount) {
    uint64_t current;
    varintTaggedGet64(account->balance, &current);

    // Update in-place - no memory reallocation needed
    varintTaggedPut64FixedWidth(account->balance, current + amount,
                                VARINT_WIDTH_72B);
}

// This pattern trades some space efficiency for update performance
```

## Performance Characteristics

### Space Efficiency

Typical distributions:

| Value Distribution            | Avg Bytes | Savings vs uint64_t |
| ----------------------------- | --------- | ------------------- |
| 90% ≤ 240, 10% ≤ 65535        | 1.1       | 86%                 |
| Uniform 0-1000                | 2.0       | 75%                 |
| Auto-increment IDs (millions) | 3-4       | 50-62%              |
| Timestamps (current)          | 5         | 37%                 |
| Random 64-bit                 | 9         | -12% (overhead)     |

### Time Complexity

| Operation  | Complexity    | Notes                        |
| ---------- | ------------- | ---------------------------- |
| Encode     | O(1)          | Constant time for all values |
| Decode     | O(1)          | Reads only needed bytes      |
| Get Length | O(1)          | Reads only first byte        |
| Compare    | O(min(w1,w2)) | Byte-by-byte comparison      |

### CPU Performance

From `varintCompare` benchmarks on modern x86_64:

- **Encode**: ~3-5 cycles per operation
- **Decode**: ~3-5 cycles per operation
- **Quick macro encode**: ~2-3 cycles (small values)

## When to Use varintTagged

### Use When:

- You need **sortable keys** (database indexes, B-trees)
- Values are typically **small but unbounded**
- **Big-endian ordering** is beneficial
- You need **fast O(1) length detection**
- Compatibility with SQLite4-style storage

### Don't Use When:

- All values are similar size (use fixed width)
- You need maximum space efficiency (use varintExternal)
- Little-endian is required for system compatibility
- Values are uniformly large (overhead hurts)

## Common Pitfalls

### Pitfall 1: Treating as Little-Endian

```c
// WRONG: Don't treat as native integer
uint64_t *badPtr = (uint64_t*)buffer;
uint64_t wrong = *badPtr;  // Byte order is wrong!

// RIGHT: Always use decode function
uint64_t correct;
varintTaggedGet64(buffer, &correct);
```

### Pitfall 2: Buffer Overflow

```c
// WRONG: Buffer too small
uint8_t buffer[4];
varintTaggedPut64(buffer, UINT64_MAX);  // Needs 9 bytes! Overflow!

// RIGHT: Always allocate 9 bytes max
uint8_t buffer[9];
varintTaggedPut64(buffer, UINT64_MAX);  // Safe
```

### Pitfall 3: Signed Values

```c
// varintTagged stores unsigned values only
int64_t signedValue = -100;

// WRONG: Direct encoding loses sign
varintTaggedPut64(buffer, (uint64_t)signedValue);  // Encodes huge positive!

// RIGHT: Handle sign separately or use zigzag encoding
// Store sign bit separately, or use:
uint64_t encoded = signedValue < 0 ?
                   (1ULL << 63) | (-signedValue) : signedValue;
varintTaggedPut64(buffer, encoded);
```

## Testing Your Code

Example test harness:

```c
#include "varintTagged.h"
#include <assert.h>

void testRoundtrip(uint64_t value) {
    uint8_t buffer[9];

    varintWidth encWidth = varintTaggedPut64(buffer, value);

    uint64_t decoded;
    varintWidth decWidth = varintTaggedGet64(buffer, &decoded);

    assert(encWidth == decWidth);
    assert(value == decoded);
    assert(varintTaggedGetLen(buffer) == encWidth);
    assert(varintTaggedLen(value) == encWidth);
}

int main() {
    // Test boundary values
    testRoundtrip(0);
    testRoundtrip(240);
    testRoundtrip(241);
    testRoundtrip(2287);
    testRoundtrip(2288);
    testRoundtrip(VARINT_TAGGED_MAX_4);
    testRoundtrip(VARINT_TAGGED_MAX_4 + 1);
    testRoundtrip(UINT64_MAX);

    printf("All tests passed!\n");
}
```

## Implementation Details

### Source Files

- **Header**: `src/varintTagged.h` - API declarations and macros
- **Implementation**: `src/varintTagged.c` - Full encode/decode logic

### Macros vs Functions

- Use `varintTaggedPut64()` for general encoding
- Use `varintTaggedPut64FixedWidthQuick_()` macro for small values when you know the width
- Use `varintTaggedGet64Quick_()` macro for inline decoding of small values

### Maximum Values Constants

```c
VARINT_TAGGED_MAX_1  // 240
VARINT_TAGGED_MAX_2  // 2,287
VARINT_TAGGED_MAX_3  // 67,823
VARINT_TAGGED_MAX_4  // 16,777,215
VARINT_TAGGED_MAX_5  // UINT32_MAX
VARINT_TAGGED_MAX_6  // UINT40_MAX
VARINT_TAGGED_MAX_7  // UINT48_MAX
VARINT_TAGGED_MAX_8  // UINT56_MAX
VARINT_TAGGED_MAX_9  // UINT64_MAX
```

## See Also

- [Architecture Overview](../ARCHITECTURE.md)
- [Choosing Varint Types](../CHOOSING_VARINTS.md)
- [varintExternal](varintExternal.md) - Maximum space efficiency
- [varintSplit](varintSplit.md) - Known bit boundaries
- [SQLite4 Varint Documentation](https://sqlite.org/src4/doc/trunk/www/varint.wiki)
