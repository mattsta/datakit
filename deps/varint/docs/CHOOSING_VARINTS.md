# Choosing the Right Varint Type

This guide helps you select the optimal varint encoding for your use case. The varint library provides multiple encoding strategies, each optimized for different scenarios.

## Quick Decision Tree

```
START: What are you storing?

├─ Single variable-width integers?
│  ├─ Need sortable keys? → varintTagged
│  ├─ Maximum space efficiency? → varintExternal
│  ├─ Legacy compatibility (Protocol Buffers, SQLite3)? → varintChained
│  └─ Need known bit boundaries for custom packing? → varintSplit
│
├─ Arrays of fixed-width integers?
│  ├─ Bit width is 8, 16, 32, or 64? → Use native arrays (uint8_t[], etc.)
│  ├─ Arbitrary bit width (12, 14, 20, etc.)? → varintPacked
│  └─ Need sorted set with binary search? → varintPacked with sorted operations
│
├─ Arrays with consecutive repeated values?
│  └─ Long runs of identical values? → varintRLE
│
├─ Arrays of small positive integers (1-1000)?
│  ├─ Values mostly < 30? → varintElias (Gamma)
│  └─ Values 30-1000? → varintElias (Delta)
│
├─ Large sorted/clustered integer arrays?
│  ├─ Need SIMD-speed encode/decode? → varintBP128
│  └─ Sorted data (timestamps, IDs)? → varintBP128 (delta mode)
│
├─ Matrix/2D data?
│  ├─ Dimensions are bounded and small? → varintDimension
│  └─ Sparse matrix? → varintDimension (sparse mode)
│
└─ Bit-level stream operations?
   ├─ Custom protocol with mixed-width fields? → varintBitstream
   └─ Variable bit widths per value? → varintBitstream
```

## Detailed Comparison

### Core Varint Types (for Variable-Width Single Values)

| Type               | Space Efficiency | Speed   | Sortable | O(1) Length | Best For                         |
| ------------------ | ---------------- | ------- | -------- | ----------- | -------------------------------- |
| **varintTagged**   | Good             | Fast    | Yes      | Yes         | Database keys, sorted data       |
| **varintExternal** | Best             | Fastest | No       | External    | Column stores, schemas           |
| **varintSplit**    | Good             | Fast    | No       | Yes         | Custom packing, known boundaries |
| **varintChained**  | Good             | Slowest | No       | No          | Legacy compatibility only        |

### Advanced Features (for Arrays and Matrices)

| Type                | Use Case                                  | Space Efficiency | Performance |
| ------------------- | ----------------------------------------- | ---------------- | ----------- |
| **varintPacked**    | Fixed-width arrays (arbitrary bit widths) | Excellent        | Fast        |
| **varintDimension** | Matrices with bounded dimensions          | Good             | Fast        |
| **varintBitstream** | Bit-level stream operations               | Perfect          | Moderate    |

### Batch/Array Encodings (for Large Data Sets)

| Type            | Use Case                         | Compression | Throughput | Best For                     |
| --------------- | -------------------------------- | ----------- | ---------- | ---------------------------- |
| **varintRLE**   | Consecutive repeated values      | 8-100x      | Moderate   | Audio silence, sparse data   |
| **varintElias** | Small positive integers (1-1000) | 50-80%      | Moderate   | Huffman lengths, tree depths |
| **varintBP128** | Sorted/clustered large arrays    | 2-8x        | 800+ MB/s  | Search indexes, time series  |

## Use Case Scenarios

### Scenario 1: Database Primary Keys

**Requirements**:

- Auto-incrementing IDs (start small, grow unbounded)
- Must be sortable for B-tree indexes
- Fast encode/decode
- Reasonable space efficiency

**Recommendation**: **varintTagged**

**Why**:

- Sortable with memcmp() (big-endian)
- O(1) length detection
- First byte stores values 0-240 (1 byte)
- 2 bytes for values up to 2,287
- Fast encode/decode

**Example**:

```c
#include "varintTagged.h"

uint64_t userId = 12345;
uint8_t key[9];
varintWidth len = varintTaggedPut64(key, userId);
// len = 2 (12345 fits in 2 bytes)

// Store in B-tree - key compares correctly with memcmp()
```

**Alternatives**:

- **varintExternal** if you maintain width separately (more efficient but not sortable)

---

### Scenario 2: In-Memory Column Store

**Requirements**:

- Millions of integers per column
- Column metadata available (type, width)
- Maximum space efficiency
- Fast access

**Recommendation**: **varintExternal**

**Why**:

- Zero metadata overhead
- Maximum space efficiency (8 bytes max vs 9 for others)
- Fastest encode/decode
- Can cast to native types on little-endian systems (zero-copy)
- Column already tracks metadata

**Example**:

```c
#include "varintExternal.h"

// Column of user IDs - most fit in 3 bytes
uint8_t *column = malloc(row_count * 3);
varintWidth width = VARINT_WIDTH_24B;  // External metadata

// Set values
for (size_t i = 0; i < row_count; i++) {
    varintExternalPutFixedWidth(column + (i * 3), userIds[i], width);
}

// 1M rows: 3 MB vs 8 MB (uint64_t) = 62.5% savings
```

**Alternatives**:

- **varintTagged** if you need self-describing data

---

### Scenario 3: Protocol Buffers Wire Format

**Requirements**:

- Must match Protocol Buffers specification
- Chained continuation bits
- Legacy compatibility

**Recommendation**: **varintChained**

**Why**:

- Direct Protocol Buffers compatibility
- Standard continuation-bit encoding
- Widely supported format

**Example**:

```c
#include "varintChained.h"

// Encode protobuf field
uint64_t fieldNumber = 5;
uint64_t value = 150;

uint8_t buffer[20];
size_t offset = 0;

// Write tag (field_number << 3 | wire_type)
offset += varintChainedPutVarint(buffer + offset, (fieldNumber << 3) | 0);

// Write value
offset += varintChainedPutVarint(buffer + offset, value);
```

**Alternatives**:

- None - this is specifically for legacy compatibility

---

### Scenario 4: Game World Coordinates

**Requirements**:

- 1 million entities with (x, y, z) coordinates
- Each coordinate: 0-4095 (12 bits)
- Compact storage
- Fast access

**Recommendation**: **varintPacked** (12-bit)

**Why**:

- Perfect fit for 12-bit values (non-native width)
- Array-based access (indexed by entity)
- Template generates optimized code
- Massive space savings

**Example**:

```c
#define PACK_STORAGE_BITS 12
#include "varintPacked.h"

// 3 values (x, y, z) × 12 bits = 36 bits = 5 bytes per entity
typedef struct {
    uint8_t coords[5];  // Packed (x, y, z)
} EntityPosition;

void setPos(EntityPosition *e, uint16_t x, uint16_t y, uint16_t z) {
    varintPacked12Set(e->coords, 0, x);
    varintPacked12Set(e->coords, 1, y);
    varintPacked12Set(e->coords, 2, z);
}

// 1M entities: 5 MB vs 12 MB (3× uint32_t) = 58% savings
```

**Alternatives**:

- **varintExternal** if coordinates vary significantly in size
- Native `uint16_t[3]` if 16-bit precision is acceptable (6 bytes vs 5)

---

### Scenario 5: Sorted Integer Set

**Requirements**:

- Store 10,000 unique integers (0-8191)
- Fast membership testing
- Fast sorted insertion
- No duplicates

**Recommendation**: **varintPacked** (13-bit) with sorted operations

**Why**:

- 13 bits perfect for 0-8191 range
- Built-in binary search (O(log n) membership)
- Sorted insertion maintains order
- Deletion support

**Example**:

```c
#define PACK_STORAGE_BITS 13
#include "varintPacked.h"

uint8_t *set = calloc(10000 * 13 / 8, 1);
size_t count = 0;

// Insert in sorted order
varintPacked13InsertSorted(set, count++, 500);
varintPacked13InsertSorted(set, count++, 100);
varintPacked13InsertSorted(set, count++, 750);
// Array automatically stays sorted: [100, 500, 750]

// Fast membership test (binary search)
if (varintPacked13Member(set, count, 500) >= 0) {
    printf("500 is in set\n");
}

// Delete member
varintPacked13DeleteMember(set, count--, 500);
```

**Alternatives**:

- Hash table if order doesn't matter
- Bitmap if range is small (< 10,000 values)

---

### Scenario 6: Sparse Feature Matrix (ML/AI)

**Requirements**:

- 100,000 samples × 10 million features
- 99.9% sparse (mostly zeros)
- Features have IDs (0-10M)
- Samples have IDs (0-100K)

**Recommendation**: **varintDimension** (sparse mode) + varintExternal for values

**Why**:

- Sparse mode stores only non-zero entries
- Dimension encoding: SPRSE_3_4 (sample: 3 bytes, feature: 4 bytes)
- Massive savings for sparse data

**Example**:

```c
#include "varintDimension.h"
#include "varintExternal.h"

typedef struct {
    uint32_t sample;   // 3 bytes (varintExternal)
    uint32_t feature;  // 4 bytes (varintExternal)
    float value;
} SparseEntry;

typedef struct {
    SparseEntry *entries;
    size_t count;
    varintDimensionPair encoding;  // SPRSE_3_4
} SparseMatrix;

// For 0.1% density:
// Dense: 100K × 10M × 4 bytes = 4 TB
// Sparse: 100K × 10M × 0.001 × 11 bytes = 11 GB
// Savings: 99.7%
```

**Alternatives**:

- Compressed Sparse Row (CSR) format for standard ML libraries
- Hash table for even sparser data

---

### Scenario 7: Custom Network Protocol

**Requirements**:

- Packet with mixed-width fields
- Version (3 bits), Type (5 bits), Length (12 bits), Flags (8 bits)
- Tight packing
- Bit-level precision

**Recommendation**: **varintBitstream**

**Why**:

- Arbitrary bit offsets
- Variable bit widths per field
- Perfect for custom protocols
- Handles slot-spanning automatically

**Example**:

```c
#include "varintBitstream.h"

uint64_t packet = 0;
size_t offset = 0;

varintBitstreamSet(&packet, offset, 3, 2);      // version = 2
offset += 3;

varintBitstreamSet(&packet, offset, 5, 7);      // type = 7
offset += 5;

varintBitstreamSet(&packet, offset, 12, 1024);  // length = 1024
offset += 12;

varintBitstreamSet(&packet, offset, 8, 0xFF);   // flags = 0xFF
offset += 8;

// Total: 28 bits = 4 bytes (vs 8 bytes with byte alignment)
```

**Alternatives**:

- **varintPacked** if all fields have same bit width
- Byte-aligned structs if space is not critical

---

### Scenario 8: Time-Series Sensor Data

**Requirements**:

- 1 million temperature readings per day
- Timestamp (grows monotonically)
- Temperature (0-10000, needs 14 bits)
- Humidity (0-100, needs 7 bits)

**Recommendation**: **varintExternal** for timestamps + **varintPacked** for readings

**Why**:

- Timestamps compress well with varintExternal (5 bytes current UNIX time)
- Temperature: 14-bit packed
- Humidity: 7-bit packed
- Combined: optimal for each field type

**Example**:

```c
#include "varintExternal.h"

#define PACK_STORAGE_BITS 14
#include "varintPacked.h"
#undef PACK_STORAGE_BITS

#define PACK_STORAGE_BITS 7
#define PACK_FUNCTION_PREFIX varintPackedHumidity
#include "varintPacked.h"
#undef PACK_STORAGE_BITS
#undef PACK_FUNCTION_PREFIX

typedef struct {
    uint8_t timestamp[5];     // 40-bit timestamp
    uint8_t temp[2];          // 14-bit temperature
    uint8_t humidity[1];      // 7-bit humidity
} SensorReading;  // Total: 8 bytes vs 16 bytes (3× uint64_t)
```

**Alternatives**:

- Delta encoding for timestamps (store differences)
- Fixed-width if precision is acceptable

---

### Scenario 9: AMQP-Style Pattern Matching Trie

**Requirements**:

- Message broker routing with wildcard patterns
- 100K+ patterns with hierarchical structure
- AMQP-style wildcards: `*` (one word), `#` (zero or more words)
- Sub-microsecond query latency
- Minimal memory footprint

**Recommendation**: **varintBitstream** for node flags + **varintExternal** for IDs

**Why**:

- varintBitstream: 3-bit node flags (terminal, wildcard type) - perfect bit packing
- varintExternal: Subscriber IDs, segment lengths, counts - variable width
- Prefix sharing: 0.7 bytes/pattern at 1M scale
- O(m) query time regardless of pattern count (m = segments)

**Example**:

```c
#include "varintBitstream.h"
#include "varintExternal.h"

typedef struct TrieNode {
    char segment[64];
    uint8_t flags;  // 3 bits: isTerminal, wildcardType (via varintBitstream)
    struct TrieNode **children;
    size_t childCount;
    uint32_t *subscriberIds;
    size_t subscriberCount;
} TrieNode;

// Encode node flags (3 bits total)
uint64_t flags = 0;
size_t bitOffset = 0;
varintBitstreamSet(&flags, bitOffset, 1, node->isTerminal);    // 1 bit
bitOffset += 1;
varintBitstreamSet(&flags, bitOffset, 2, node->wildcardType);  // 2 bits
// Total: 3 bits vs 3 bytes with separate fields

// Encode subscriber IDs with varintExternal (variable width)
for (size_t i = 0; i < node->subscriberCount; i++) {
    varintWidth w = varintExternalPut(buffer + offset, node->subscriberIds[i]);
    offset += w;
}

// Performance at 100K patterns:
// - Trie: 3 μs per query (2391x faster than naive linear scan)
// - Memory: 0.7 bytes/pattern with prefix sharing
// - Throughput: 55,866 queries/second
```

**Real-World Results** (from advanced/trie_pattern_matcher.c):

```
Patterns    Naive (μs)    Trie (μs)    Speedup    Memory Savings
100         3.00          1.00         3x         ~8%
1,000       37.00         1.00         37x        71%
10,000      469.00        2.00         235x       95%
100,000     7,174.00      3.00         2,391x     99.4%
1,000,000   ~17,900,000   17.90        ~1,000,000x 99.9999%
```

**Alternatives**:

- Hash table for exact patterns only (no wildcard support)
- Regex engine (100-1000x slower for complex patterns)

---

### Scenario 10: Audio/Video Silence Detection

**Requirements**:

- Audio samples with many consecutive zeros (silence)
- 16-bit samples
- Need 8-100x compression for silence regions
- Fast playback decode

**Recommendation**: **varintRLE**

**Why**:

- Consecutive zeros compress to single (length, 0) pair
- 10,000 zeros → 2-4 bytes
- Random access via `varintRLEGetAt()`
- Seamless mixed compression (silence + audio)

**Example**:

```c
#include "varintRLE.h"

// Audio buffer with silence gaps
uint64_t samples[48000];  // 1 second at 48kHz
// Fill with audio data including silence regions...

// Analyze compression potential
varintRLEMeta meta;
varintRLEAnalyze(samples, 48000, &meta);
printf("Runs: %zu, would compress: %s\n",
       meta.runCount, varintRLEIsBeneficial(samples, 48000) ? "yes" : "no");

// Encode
uint8_t *encoded = malloc(varintRLEMaxSize(48000));
size_t encodedSize = varintRLEEncode(encoded, samples, 48000, &meta);
// 90% silence → ~10x compression
```

**Alternatives**:

- **varintBP128**: Better if samples are clustered but not repeated
- Simple zero-run encoding: If values are always 0 or non-zero

---

### Scenario 11: Huffman Codebook Storage

**Requirements**:

- Store Huffman code lengths (1-15 bits typically)
- 256 symbols (one per byte value)
- Minimum storage overhead
- Self-delimiting (no length prefix needed)

**Recommendation**: **varintElias** (Gamma)

**Why**:

- Code lengths 1-15 fit in 1-7 bits with Gamma
- Self-delimiting: no delimiters needed between values
- Optimal for geometric distribution (short codes more common)

**Example**:

```c
#include "varintElias.h"

// Huffman code lengths for 256 symbols
uint64_t codeLengths[256];
// ... populate from Huffman tree building

// Encode with Gamma (optimal for small integers)
uint8_t *encoded = malloc(varintEliasGammaMaxBytes(256));
varintEliasMeta meta;
varintEliasGammaEncodeArray(encoded, codeLengths, 256, &meta);

printf("Encoded %zu symbols in %zu bits (%.1f bits/symbol)\n",
       256, meta.totalBits, (double)meta.totalBits / 256);
// Typical: ~600 bits = 75 bytes vs 256 bytes raw
```

**Alternatives**:

- **varintElias** (Delta): If code lengths are larger (>30)
- **varintRLE**: If many symbols share same code length

---

### Scenario 12: Search Engine Posting Lists

**Requirements**:

- Sorted document IDs (millions per term)
- SIMD-speed decode for query performance
- High compression for index storage
- Delta encoding friendly (sorted)

**Recommendation**: **varintBP128** (delta mode)

**Why**:

- 128-value blocks perfect for SIMD (AVX2/NEON)
- Delta encoding exploits sorted property
- 1.2+ GB/s decode throughput
- 2-8x compression for typical posting lists

**Example**:

```c
#include "varintBP128.h"

// Sorted document IDs for term "hello"
uint32_t docIds[1000000];
// ... populate from indexing

// Verify sorted (required for delta)
assert(varintBP128IsSorted32(docIds, 1000000));

// Delta encode
uint8_t *encoded = malloc(varintBP128MaxBytes(1000000));
varintBP128Meta meta;
size_t encodedSize = varintBP128DeltaEncode32(
    encoded, docIds, 1000000, &meta);

printf("Posting list: %zu docs, %zu bytes (%.1f bits/doc)\n",
       1000000, encodedSize, 8.0 * encodedSize / 1000000);
// Typical: 1M IDs → ~500KB (8x compression)

// Fast decode for query intersection
uint32_t *decoded = malloc(1000000 * sizeof(uint32_t));
varintBP128DeltaDecode32(encoded, decoded, 1000000);
```

**Alternatives**:

- **varintFOR**: For clustered but unsorted data
- **varintPFOR**: If there are outlier document IDs

---

## Performance Considerations

### Speed Ranking (Fastest to Slowest)

1. **varintExternal** (zero-copy possible, O(1))
2. **varintTagged** (O(1) with fast-path macros)
3. **varintSplit** (O(1) inline macros)
4. **varintPacked** (O(1) inline, bit operations)
5. **varintBitstream** (O(1) but more complex)
6. **varintChained** (O(w) - must traverse bytes)

### Space Ranking (Most Efficient to Least)

1. **varintExternal** (no overhead, 8 bytes max)
2. **varintPacked** (configurable, zero waste)
3. **varintTagged** (1 byte overhead, 9 bytes max)
4. **varintSplit** (1 byte overhead, 9 bytes max)
5. **varintChained** (1+ bits overhead per byte, 9 bytes max)

## Common Mistakes

### Mistake 1: Using Varints for Fixed-Size Data

```c
// WRONG: All values are always uint32_t
uint64_t value = always_32_bits();
varintTaggedPut64(buffer, value);  // Wastes encoding overhead!

// RIGHT: Use native type
uint32_t value = always_32_bits();
memcpy(buffer, &value, 4);
```

### Mistake 2: Wrong Varint for Sorted Data

```c
// WRONG: varintExternal not sortable
uint64_t key = 12345;
varintExternalPut(indexKey, key);
// Can't use memcmp() for sorting!

// RIGHT: Use varintTagged for sortable keys
varintTaggedPut64(indexKey, key);
// Now memcmp() works correctly
```

### Mistake 3: Chained for New Systems

```c
// WRONG: Using slowest type for new code
varintChainedPutVarint(buffer, value);  // 2-3x slower!

// RIGHT: Use faster alternatives
varintTaggedPut64(buffer, value);  // Much faster
// Or:
varintWidth w = varintExternalPut(buffer, value);  // Fastest
```

### Mistake 4: varintPacked for Variable Widths

```c
// WRONG: Values have different sizes
#define PACK_STORAGE_BITS 32
#include "varintPacked.h"

varintPacked32Set(array, 0, 10);         // Wastes 22 bits!
varintPacked32Set(array, 1, 1000000);    // OK
// Wasted space on small values

// RIGHT: Use varintExternal or varintTagged
varintWidth w1 = varintExternalPut(buffer, 10);
varintWidth w2 = varintExternalPut(buffer + w1, 1000000);
```

## Migration Guide

### From uint64_t Arrays to Varints

```c
// Before: Fixed 64-bit storage
uint64_t ids[1000];
for (size_t i = 0; i < 1000; i++) {
    ids[i] = getUserId(i);
}
// 1000 × 8 = 8000 bytes

// After: varintExternal with width array
uint8_t *data = malloc(1000 * 3);  // Most IDs fit in 3 bytes
varintWidth *widths = malloc(1000);  // Track widths

for (size_t i = 0; i < 1000; i++) {
    widths[i] = varintExternalPut(data + (i * 3), getUserId(i));
}
// Typically: ~3000 bytes data + 1000 bytes widths = 4000 bytes
// Savings: 50%
```

### From Protocol Buffers to Faster Encoding

```c
// Legacy: Protocol Buffers (chained)
varintChainedPutVarint(buffer, value);

// Modern: Switch to varintExternal + length prefix
varintWidth width = varintExternalPut(buffer + 1, value);
buffer[0] = width;  // Store width in first byte

// Or: varintTagged for self-describing
varintTaggedPut64(buffer, value);
// Both are 2-3x faster than chained
```

## Summary

**Choose**:

- **varintTagged**: Sortable keys, B-trees, self-describing data
- **varintExternal**: Maximum efficiency, schemas, column stores
- **varintSplit**: Custom packing, known boundaries, hybrid encodings
- **varintChained**: Legacy compatibility ONLY (SQLite3, LevelDB, Protocol Buffers)
- **varintPacked**: Arrays of same-bit-width values (non-native widths)
- **varintDimension**: Matrices with bounded dimensions
- **varintBitstream**: Bit-level protocols, variable-width streams
- **varintRLE**: Consecutive repeated values, audio silence, sparse data
- **varintElias**: Small positive integers, Huffman lengths, tree depths
- **varintBP128**: Large sorted arrays, search indexes, SIMD-speed processing

**When in doubt**:

1. Need sortable? → **varintTagged**
2. Have schema? → **varintExternal**
3. Fixed bit width? → **varintPacked**
4. Legacy format? → **varintChained**
5. Repeated consecutive values? → **varintRLE**
6. Small positive integers (1-1000)? → **varintElias**
7. Large sorted/clustered arrays? → **varintBP128**

## Further Reading

- [Architecture Overview](ARCHITECTURE.md)
- Module guides:
  - [varintTagged](modules/varintTagged.md)
  - [varintExternal](modules/varintExternal.md)
  - [varintSplit](modules/varintSplit.md)
  - [varintChained](modules/varintChained.md)
  - [varintPacked](modules/varintPacked.md)
  - [varintDimension](modules/varintDimension.md)
  - [varintBitstream](modules/varintBitstream.md)
  - [varintRLE](modules/varintRLE.md)
  - [varintElias](modules/varintElias.md)
  - [varintBP128](modules/varintBP128.md)
