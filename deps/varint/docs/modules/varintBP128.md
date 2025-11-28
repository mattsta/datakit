# varintBP128: SIMD Block-Packed Encoding

## Overview

**varintBP128** implements Binary Packing with 128-value blocks - a high-performance integer compression scheme optimized for SIMD processing. It packs integers using the minimum bit-width needed per block, with optional delta encoding for sorted sequences.

**Key Features**: SIMD-accelerated (AVX2/NEON), excellent for sorted arrays, fixed 128-value blocks for cache efficiency, supports 32-bit and 64-bit integers.

## Key Characteristics

| Property       | Value                           |
| -------------- | ------------------------------- |
| Implementation | Header (.h) + Compiled (.c)     |
| Block Size     | 128 values (SIMD-friendly)      |
| SIMD Support   | AVX2 (x86), NEON (ARM)          |
| Best For       | Sorted/clustered integer arrays |
| Compression    | 2-8x for typical sorted data    |

## Encoding Formats

### Basic BP128 Block

```
[1 byte: bit-width][packed data: 128 × bitWidth bits]
```

For 128 values needing 10 bits each:

- Header: 1 byte (bit-width = 10)
- Data: 128 × 10 = 1280 bits = 160 bytes
- Total: 161 bytes vs 512 bytes raw (32-bit) = 69% compression

### Partial Block (last block with < 128 values)

```
[1 byte: 0x80 | bit-width][1 byte: count][packed data]
```

High bit (0x80) indicates partial block.

### BP128 Delta Block

For sorted data, stores deltas instead:

```
[first value: varint][delta block 1][delta block 2]...
```

Each delta block encodes differences from previous values.

## API Reference

### Basic Encoding (32-bit)

```c
// Encode array
size_t varintBP128Encode32(uint8_t *dst, const uint32_t *values, size_t count,
                           varintBP128Meta *meta);

// Decode array
size_t varintBP128Decode32(const uint8_t *src, uint32_t *values, size_t maxCount);

// Encode single block (128 values)
size_t varintBP128EncodeBlock32(uint8_t *dst, const uint32_t *values);

// Decode single block
size_t varintBP128DecodeBlock32(const uint8_t *src, uint32_t *values);
```

### Delta Encoding (32-bit, for sorted data)

```c
// Encode sorted array with delta compression
size_t varintBP128DeltaEncode32(uint8_t *dst, const uint32_t *values,
                                size_t count, varintBP128Meta *meta);

// Decode delta-compressed array
size_t varintBP128DeltaDecode32(const uint8_t *src, uint32_t *values,
                                size_t maxCount);

// Block-level delta operations
size_t varintBP128DeltaEncodeBlock32(uint8_t *dst, const uint32_t *values,
                                     uint32_t prevValue);
size_t varintBP128DeltaDecodeBlock32(const uint8_t *src, uint32_t *values,
                                     uint32_t prevValue);
```

### 64-bit Variants

```c
// Basic encoding
size_t varintBP128Encode64(uint8_t *dst, const uint64_t *values, size_t count,
                           varintBP128Meta *meta);
size_t varintBP128Decode64(const uint8_t *src, uint64_t *values, size_t maxCount);

// Delta encoding
size_t varintBP128DeltaEncode64(uint8_t *dst, const uint64_t *values,
                                size_t count, varintBP128Meta *meta);
size_t varintBP128DeltaDecode64(const uint8_t *src, uint64_t *values,
                                size_t maxCount);
```

### Utility Functions

```c
// Calculate max buffer size needed
size_t varintBP128MaxBytes(size_t count);

// Find max bit-width in array
uint8_t varintBP128MaxBitWidth32(const uint32_t *values, size_t count);
uint8_t varintBP128MaxBitWidth64(const uint64_t *values, size_t count);

// Check if compression is beneficial
bool varintBP128IsBeneficial32(const uint32_t *values, size_t count);
bool varintBP128IsBeneficial64(const uint64_t *values, size_t count);

// Check if data is sorted (for delta encoding)
bool varintBP128IsSorted32(const uint32_t *values, size_t count);
bool varintBP128IsSorted64(const uint64_t *values, size_t count);
```

### Metadata Structure

```c
typedef struct varintBP128Meta {
    size_t count;           // Total values encoded
    size_t blockCount;      // Number of 128-value blocks
    size_t encodedBytes;    // Total bytes in output
    size_t lastBlockSize;   // Values in last (partial) block
    uint8_t maxBitWidth;    // Maximum bit-width across blocks
} varintBP128Meta;
```

## Real-World Examples

### Example 1: Document ID Lists (Search Engines)

Posting lists in search indexes are sorted document IDs:

```c
#include "varintBP128.h"

typedef struct {
    uint8_t *encoded;
    size_t encodedSize;
    size_t docCount;
} PostingList;

void postingListCompress(PostingList *pl, const uint32_t *docIds, size_t count) {
    // Verify sorted (required for delta encoding)
    assert(varintBP128IsSorted32(docIds, count));

    pl->encoded = malloc(varintBP128MaxBytes(count));
    varintBP128Meta meta;

    // Delta encoding exploits sorted property
    pl->encodedSize = varintBP128DeltaEncode32(pl->encoded, docIds, count, &meta);
    pl->docCount = count;

    printf("Posting list: %zu docs, %zu bytes (%.1f bits/doc)\n",
           count, pl->encodedSize, 8.0 * pl->encodedSize / count);

    // Typical: 1M doc IDs with small gaps
    // Raw: 4 MB
    // Delta BP128: ~500 KB (8x compression)
}

bool postingListContains(const PostingList *pl, uint32_t targetDocId) {
    // Decode and binary search
    uint32_t *docIds = malloc(pl->docCount * sizeof(uint32_t));
    varintBP128DeltaDecode32(pl->encoded, docIds, pl->docCount);

    // Binary search
    size_t lo = 0, hi = pl->docCount;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (docIds[mid] < targetDocId) lo = mid + 1;
        else hi = mid;
    }
    bool found = (lo < pl->docCount && docIds[lo] == targetDocId);

    free(docIds);
    return found;
}
```

### Example 2: Time Series Timestamps

Monotonically increasing timestamps:

```c
#include "varintBP128.h"

typedef struct {
    uint8_t *timestamps;
    size_t timestampSize;
    uint8_t *values;
    size_t valueSize;
    size_t sampleCount;
} CompressedTimeSeries;

void timeSeriesCompress(CompressedTimeSeries *ts, const uint64_t *times,
                        const double *values, size_t count) {
    ts->sampleCount = count;

    // Delta encode timestamps (sorted, small gaps)
    ts->timestamps = malloc(varintBP128MaxBytes(count));
    varintBP128Meta tmeta;
    ts->timestampSize = varintBP128DeltaEncode64(ts->timestamps, times,
                                                  count, &tmeta);

    // Store values separately (could also compress)
    ts->values = malloc(count * sizeof(double));
    memcpy(ts->values, values, count * sizeof(double));
    ts->valueSize = count * sizeof(double);

    printf("Timestamps: %zu samples, %zu bytes (%.1f bytes/sample)\n",
           count, ts->timestampSize, (double)ts->timestampSize / count);

    // 1-second resolution, 1 day of data (86400 samples):
    // Raw timestamps: 691 KB
    // Delta BP128: ~20 KB (gaps are all ~1000ms = 10 bits)
}
```

### Example 3: Graph Adjacency Lists

Sorted neighbor lists in graph storage:

```c
#include "varintBP128.h"

typedef struct {
    uint32_t nodeId;
    uint8_t *neighbors;
    size_t neighborSize;
    size_t neighborCount;
} CompressedNode;

typedef struct {
    CompressedNode *nodes;
    size_t nodeCount;
} CompressedGraph;

void graphCompress(CompressedGraph *g, const uint32_t *adjacency,
                   const size_t *offsets, size_t nodeCount) {
    g->nodes = malloc(nodeCount * sizeof(CompressedNode));
    g->nodeCount = nodeCount;

    size_t totalOriginal = 0;
    size_t totalCompressed = 0;

    for (size_t i = 0; i < nodeCount; i++) {
        size_t start = offsets[i];
        size_t end = (i + 1 < nodeCount) ? offsets[i + 1] : offsets[nodeCount];
        size_t degree = end - start;

        g->nodes[i].nodeId = i;
        g->nodes[i].neighborCount = degree;

        if (degree > 0) {
            // Neighbors are sorted - use delta encoding
            g->nodes[i].neighbors = malloc(varintBP128MaxBytes(degree));
            varintBP128Meta meta;
            g->nodes[i].neighborSize = varintBP128DeltaEncode32(
                g->nodes[i].neighbors, &adjacency[start], degree, &meta);

            totalOriginal += degree * 4;
            totalCompressed += g->nodes[i].neighborSize;
        } else {
            g->nodes[i].neighbors = NULL;
            g->nodes[i].neighborSize = 0;
        }
    }

    printf("Graph: %zu nodes, %.1f MB -> %.1f MB (%.1fx compression)\n",
           nodeCount, totalOriginal / 1e6, totalCompressed / 1e6,
           (double)totalOriginal / totalCompressed);
}
```

### Example 4: Column Store Database

Sorted column values for range queries:

```c
#include "varintBP128.h"

typedef struct {
    uint8_t *data;
    size_t dataSize;
    size_t rowCount;
    bool isSorted;
    bool isDelta;
} CompressedColumn;

void columnCompress(CompressedColumn *col, const uint32_t *values, size_t count) {
    col->rowCount = count;
    col->isSorted = varintBP128IsSorted32(values, count);

    col->data = malloc(varintBP128MaxBytes(count));
    varintBP128Meta meta;

    if (col->isSorted) {
        // Use delta for sorted columns (much better compression)
        col->dataSize = varintBP128DeltaEncode32(col->data, values, count, &meta);
        col->isDelta = true;
        printf("Sorted column: delta encoding, %u-bit max deltas\n",
               meta.maxBitWidth);
    } else {
        // Use basic BP128 for unsorted
        col->dataSize = varintBP128Encode32(col->data, values, count, &meta);
        col->isDelta = false;
        printf("Unsorted column: basic encoding, %u-bit values\n",
               meta.maxBitWidth);
    }

    // Shrink allocation
    col->data = realloc(col->data, col->dataSize);

    double ratio = (double)col->dataSize / (count * 4);
    printf("Compression: %.1f%% of original\n", ratio * 100);
}

// Example: user_id column (sorted): 10M rows
// Raw: 40 MB
// Delta BP128: ~5 MB (sequential IDs = 1-bit deltas!)
```

### Example 5: Inverted Index Compression

Position lists within documents:

```c
#include "varintBP128.h"

typedef struct {
    uint32_t docId;
    uint8_t *positions;   // Delta-encoded
    size_t posSize;
    size_t posCount;
} DocPositions;

typedef struct {
    DocPositions *docs;
    size_t docCount;
} TermPostings;

void termIndexBuild(TermPostings *tp, const uint32_t *docIds,
                    const uint32_t **positions, const size_t *posCounts,
                    size_t docCount) {
    tp->docs = malloc(docCount * sizeof(DocPositions));
    tp->docCount = docCount;

    for (size_t i = 0; i < docCount; i++) {
        tp->docs[i].docId = docIds[i];
        tp->docs[i].posCount = posCounts[i];

        if (posCounts[i] > 0) {
            // Positions are sorted within document
            tp->docs[i].positions = malloc(varintBP128MaxBytes(posCounts[i]));
            varintBP128Meta meta;
            tp->docs[i].posSize = varintBP128DeltaEncode32(
                tp->docs[i].positions, positions[i], posCounts[i], &meta);
        } else {
            tp->docs[i].positions = NULL;
            tp->docs[i].posSize = 0;
        }
    }
}

// Common word with 100 occurrences per doc, 10K docs:
// Raw positions: 4 MB
// Delta BP128: ~100 KB (positions cluster together)
```

### Example 6: Log Sequence Numbers

Database LSNs are monotonically increasing:

```c
#include "varintBP128.h"

typedef struct {
    uint8_t *lsns;
    size_t lsnSize;
    size_t count;
} CompressedWAL;

void walCompress(CompressedWAL *wal, const uint64_t *lsns, size_t count) {
    // LSNs are strictly increasing
    assert(varintBP128IsSorted64(lsns, count));

    wal->lsns = malloc(varintBP128MaxBytes(count));
    varintBP128Meta meta;
    wal->lsnSize = varintBP128DeltaEncode64(wal->lsns, lsns, count, &meta);
    wal->count = count;

    printf("WAL: %zu LSNs, %zu bytes (%.2f bytes/LSN)\n",
           count, wal->lsnSize, (double)wal->lsnSize / count);

    // 1M transactions:
    // Raw: 8 MB
    // Delta BP128: ~250 KB (LSN gaps are small)
}
```

## SIMD Optimization Details

### AVX2 (x86-64)

Used for finding max value in block:

```c
__m256i vmax = _mm256_max_epu32(vmax, v);  // 8 values at once
```

### NEON (ARM)

Used for prefix sum in delta decoding:

```c
uint32x4_t d = vld1q_u32(&deltas[i]);
d = vaddq_u32(d, vextq_u32(vdupq_n_u32(0), d, 3));
d = vaddq_u32(d, vprev);  // Add running sum
```

### Block Size Rationale

128 values chosen because:

- Fits in L1 cache (512 bytes raw)
- Multiple of SIMD width (4×32, 8×32, 2×64)
- Good compression granularity
- Amortizes block header overhead

## Performance Characteristics

### Compression Ratios

| Data Pattern              | Compression |
| ------------------------- | ----------- |
| Sequential IDs (1,2,3...) | 95%+        |
| Small gaps (avg 10)       | 75-85%      |
| Medium gaps (avg 1000)    | 50-70%      |
| Random 32-bit             | 0-10%       |

### Throughput (Approximate)

| Operation    | Scalar   | SIMD     |
| ------------ | -------- | -------- |
| Encode       | 200 MB/s | 800 MB/s |
| Decode       | 300 MB/s | 1.2 GB/s |
| Delta decode | 250 MB/s | 1.0 GB/s |

### Time Complexity

| Operation     | Complexity |
| ------------- | ---------- |
| Encode        | O(n)       |
| Decode        | O(n)       |
| Random access | O(n/128)   |

## When to Use varintBP128

### Use When:

- Data is **sorted** or **nearly sorted**
- Processing **large arrays** (1000+ values)
- Need **fast SIMD** encoding/decoding
- Values have **clustered bit-widths**
- Building **search indexes** or **databases**

### Don't Use When:

- Arrays are **small** (< 128 values)
- Data is **completely random**
- Need **individual value access** (use varintTagged)
- Values are **negative** (convert to unsigned first)

## Implementation Details

### Source Files

- **Header**: `src/varintBP128.h`
- **Implementation**: `src/varintBP128.c`

### Dependencies

- `varintTagged.h` - For first value in delta encoding

### SIMD Detection

Automatically uses:

- AVX2 on x86-64 when available
- NEON on ARM when available
- Scalar fallback otherwise

### Testing

See `src/varintBP128.c` (test section) for comprehensive test cases.

## See Also

- [Architecture Overview](../ARCHITECTURE.md)
- [Choosing Varint Types](../CHOOSING_VARINTS.md)
- [varintRLE](varintRLE.md) - For repeated consecutive values
- [varintElias](varintElias.md) - Bit-level compression
- [varintFOR](varintFOR.md) - Frame of Reference encoding
