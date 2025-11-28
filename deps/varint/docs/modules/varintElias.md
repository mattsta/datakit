# varintElias: Universal Codes (Gamma/Delta)

## Overview

**varintElias** implements Elias Gamma and Delta universal codes - bit-level compression schemes optimal for geometric distributions. These prefix-free codes are self-delimiting and achieve excellent compression for small positive integers.

**Key Features**: Bit-level precision, optimal for small integers, prefix-free (no delimiters needed), two encoding variants for different distributions.

## Key Characteristics

| Property       | Value                        |
| -------------- | ---------------------------- |
| Implementation | Header (.h) + Compiled (.c)  |
| Value Range    | Positive integers >= 1       |
| Gamma Code     | Optimal for P(n) = 2^(-n)    |
| Delta Code     | Better for larger integers   |
| Encoding       | Bit-level (not byte-aligned) |

## Encoding Formats

### Elias Gamma Code

Encodes positive integer N as:

1. `floor(log2(N))` zero bits
2. N in binary (including leading 1)

```
Value   Binary    Gamma Code    Bits
1       1         1             1
2       10        010           3
3       11        011           3
4       100       00100         5
5       101       00101         5
8       1000      0001000       7
9       1001      0001001       7
16      10000     000010000     9
100     1100100   00000001100100  13
```

**Formula**: Bits needed = 2 \* floor(log2(N)) + 1

### Elias Delta Code

Encodes positive integer N as:

1. Gamma-encode (floor(log2(N)) + 1)
2. Remaining log2(N) bits of N (without leading 1)

```
Value   Gamma Length  Remaining    Delta Code      Bits
1       1             (none)       1               1
2       010           0            0100            4
3       010           1            0101            4
4       011           00           01100           5
8       00100         000          00100000        8
16      00101         0000         001010000       9
100     00111         100100       00111100100     11
1000    0001010       1111101000   00010101111101000  17
```

**Formula**: Better than Gamma for N > 31

## API Reference

### Bit-Level I/O

```c
// Writer for encoding
typedef struct varintBitWriter {
    uint8_t *buffer;   // Output buffer
    size_t bitPos;     // Current bit position
    size_t capacity;   // Buffer capacity in bytes
} varintBitWriter;

// Reader for decoding
typedef struct varintBitReader {
    const uint8_t *buffer;  // Input buffer
    size_t bitPos;          // Current bit position
    size_t totalBits;       // Total bits available
} varintBitReader;

// Initialize writer
void varintBitWriterInit(varintBitWriter *w, uint8_t *buffer, size_t capacity);

// Write n bits
void varintBitWriterWrite(varintBitWriter *w, uint64_t value, size_t nBits);

// Get bytes written
size_t varintBitWriterBytes(const varintBitWriter *w);

// Initialize reader
void varintBitReaderInit(varintBitReader *r, const uint8_t *buffer, size_t totalBits);

// Read n bits
uint64_t varintBitReaderRead(varintBitReader *r, size_t nBits);

// Check remaining bits
bool varintBitReaderHasMore(const varintBitReader *r, size_t nBits);
```

### Gamma Encoding

```c
// Calculate bits needed
size_t varintEliasGammaBits(uint64_t value);

// Encode single value
size_t varintEliasGammaEncode(varintBitWriter *w, uint64_t value);

// Decode single value
uint64_t varintEliasGammaDecode(varintBitReader *r);

// Encode array
size_t varintEliasGammaEncodeArray(uint8_t *dst, const uint64_t *values,
                                   size_t count, varintEliasMeta *meta);

// Decode array
size_t varintEliasGammaDecodeArray(const uint8_t *src, size_t srcBits,
                                   uint64_t *values, size_t maxCount);

// Maximum buffer size needed
size_t varintEliasGammaMaxBytes(size_t count);

// Check if compression is beneficial
bool varintEliasGammaIsBeneficial(const uint64_t *values, size_t count);
```

### Delta Encoding

```c
// Calculate bits needed
size_t varintEliasDeltaBits(uint64_t value);

// Encode single value
size_t varintEliasDeltaEncode(varintBitWriter *w, uint64_t value);

// Decode single value
uint64_t varintEliasDeltaDecode(varintBitReader *r);

// Encode array
size_t varintEliasDeltaEncodeArray(uint8_t *dst, const uint64_t *values,
                                   size_t count, varintEliasMeta *meta);

// Decode array
size_t varintEliasDeltaDecodeArray(const uint8_t *src, size_t srcBits,
                                   uint64_t *values, size_t maxCount);

// Maximum buffer size needed
size_t varintEliasDeltaMaxBytes(size_t count);

// Check if compression is beneficial
bool varintEliasDeltaIsBeneficial(const uint64_t *values, size_t count);
```

### Metadata Structure

```c
typedef struct varintEliasMeta {
    size_t count;        // Number of values encoded
    size_t totalBits;    // Total bits used
    size_t encodedBytes; // Bytes (ceiling of bits/8)
} varintEliasMeta;
```

## Real-World Examples

### Example 1: Huffman Code Lengths

Huffman tree code lengths are typically small (1-15):

```c
#include "varintElias.h"

typedef struct {
    uint8_t *encoded;
    size_t encodedBits;
    size_t symbolCount;
} CompressedCodebook;

void codebookCompress(CompressedCodebook *cb, const uint8_t *codeLengths,
                      size_t symbolCount) {
    // Code lengths are 1-15, perfect for Gamma
    uint64_t *lengths = malloc(symbolCount * sizeof(uint64_t));
    for (size_t i = 0; i < symbolCount; i++) {
        lengths[i] = codeLengths[i];  // Already >= 1
    }

    cb->encoded = malloc(varintEliasGammaMaxBytes(symbolCount));
    varintEliasMeta meta;
    varintEliasGammaEncodeArray(cb->encoded, lengths, symbolCount, &meta);

    cb->encodedBits = meta.totalBits;
    cb->symbolCount = symbolCount;

    free(lengths);

    // 256 symbols with avg code length 8:
    // Raw: 256 bytes
    // Gamma: ~600 bits = 75 bytes (70% compression)
}
```

### Example 2: Tree Depths in Data Structures

Encode depths in B-trees, tries, or search trees:

```c
#include "varintElias.h"

typedef struct {
    uint64_t nodeId;
    uint8_t depth;      // Typically 1-30 for balanced trees
} TreeNode;

typedef struct {
    uint8_t *depthData;
    size_t depthBits;
    size_t nodeCount;
} CompressedTreeIndex;

void treeIndexCompress(CompressedTreeIndex *idx, const TreeNode *nodes,
                       size_t count) {
    uint64_t *depths = malloc(count * sizeof(uint64_t));
    for (size_t i = 0; i < count; i++) {
        depths[i] = nodes[i].depth;  // >= 1 for root at depth 1
    }

    idx->depthData = malloc(varintEliasGammaMaxBytes(count));
    varintEliasMeta meta;
    varintEliasGammaEncodeArray(idx->depthData, depths, count, &meta);

    idx->depthBits = meta.totalBits;
    idx->nodeCount = count;

    free(depths);

    // B-tree with 1M nodes, avg depth 5:
    // Raw: 1 MB
    // Gamma: ~450 KB (55% compression)
}
```

### Example 3: Small Gap Encoding

Encode small gaps between sorted values:

```c
#include "varintElias.h"

typedef struct {
    uint64_t firstValue;
    uint8_t *gaps;
    size_t gapBits;
    size_t count;
} GapEncodedList;

void gapEncode(GapEncodedList *list, const uint64_t *sortedValues, size_t count) {
    list->firstValue = sortedValues[0];
    list->count = count;

    // Calculate gaps (add 1 to handle gap=0)
    uint64_t *gaps = malloc((count - 1) * sizeof(uint64_t));
    for (size_t i = 1; i < count; i++) {
        gaps[i-1] = sortedValues[i] - sortedValues[i-1] + 1;  // +1 for zero gaps
    }

    // Use Delta for gaps (better for larger gaps)
    list->gaps = malloc(varintEliasDeltaMaxBytes(count - 1));
    varintEliasMeta meta;
    varintEliasDeltaEncodeArray(list->gaps, gaps, count - 1, &meta);
    list->gapBits = meta.totalBits;

    free(gaps);
}

uint64_t gapDecode(const GapEncodedList *list, size_t index) {
    if (index == 0) return list->firstValue;

    uint64_t *gaps = malloc(index * sizeof(uint64_t));
    varintEliasDeltaDecodeArray(list->gaps, list->gapBits, gaps, index);

    uint64_t value = list->firstValue;
    for (size_t i = 0; i < index; i++) {
        value += gaps[i] - 1;  // -1 to undo the +1 encoding
    }

    free(gaps);
    return value;
}

// Sorted document IDs with avg gap 10:
// 1M IDs, raw: 8 MB
// Gap encoded: ~800 KB
```

### Example 4: Fibonacci-like Sequences

Natural sequences often follow power laws:

```c
#include "varintElias.h"

typedef struct {
    uint8_t *encoded;
    size_t encodedBits;
    size_t length;
} CompressedSequence;

void fibonacciCompress(CompressedSequence *seq, size_t n) {
    uint64_t *fib = malloc(n * sizeof(uint64_t));
    fib[0] = 1;
    fib[1] = 1;
    for (size_t i = 2; i < n; i++) {
        fib[i] = fib[i-1] + fib[i-2];
    }

    // Delta is better for rapidly growing sequences
    seq->encoded = malloc(varintEliasDeltaMaxBytes(n));
    varintEliasMeta meta;
    varintEliasDeltaEncodeArray(seq->encoded, fib, n, &meta);

    seq->encodedBits = meta.totalBits;
    seq->length = n;

    printf("Fibonacci(%zu): %zu values in %zu bits (%.1f bits/value)\n",
           n, n, meta.totalBits, (double)meta.totalBits / n);

    free(fib);
}

// First 50 Fibonacci numbers:
// Raw: 400 bytes
// Delta: ~200 bits = 25 bytes
```

### Example 5: Rank/Select Data Structures

Succinct data structures use small integers:

```c
#include "varintElias.h"

typedef struct {
    uint8_t *superblockCounts;  // Gamma-encoded
    size_t superblockBits;
    uint8_t *blockCounts;       // Gamma-encoded
    size_t blockBits;
    size_t totalBits;
} SuccinctBitVector;

void succinctBuild(SuccinctBitVector *sbv, const uint64_t *bitmap,
                   size_t wordCount) {
    // Superblocks every 512 bits, blocks every 64 bits
    size_t superblockCount = (wordCount * 64 + 511) / 512;
    size_t blockCount = wordCount;

    uint64_t *superCounts = malloc(superblockCount * sizeof(uint64_t));
    uint64_t *blockCounts = malloc(blockCount * sizeof(uint64_t));

    uint64_t runningCount = 0;
    for (size_t i = 0; i < wordCount; i++) {
        if (i % 8 == 0) {
            superCounts[i / 8] = runningCount + 1;  // +1 for Elias
        }
        blockCounts[i] = __builtin_popcountll(bitmap[i]) + 1;  // +1 for Elias
        runningCount += __builtin_popcountll(bitmap[i]);
    }

    // Gamma encode (counts are small)
    sbv->superblockCounts = malloc(varintEliasGammaMaxBytes(superblockCount));
    varintEliasMeta meta1;
    varintEliasGammaEncodeArray(sbv->superblockCounts, superCounts,
                                 superblockCount, &meta1);
    sbv->superblockBits = meta1.totalBits;

    sbv->blockCounts = malloc(varintEliasGammaMaxBytes(blockCount));
    varintEliasMeta meta2;
    varintEliasGammaEncodeArray(sbv->blockCounts, blockCounts,
                                 blockCount, &meta2);
    sbv->blockBits = meta2.totalBits;

    sbv->totalBits = wordCount * 64;

    free(superCounts);
    free(blockCounts);
}
```

### Example 6: Wavelet Tree Level Sizes

Compress wavelet tree metadata:

```c
#include "varintElias.h"

typedef struct {
    uint8_t *levelSizes;   // Delta-encoded level sizes
    size_t levelBits;
    uint8_t depth;
} WaveletTreeMeta;

void waveletMetaEncode(WaveletTreeMeta *meta, const size_t *levelSizes,
                       uint8_t depth) {
    meta->depth = depth;

    // Level sizes grow geometrically - perfect for Delta
    uint64_t *sizes = malloc(depth * sizeof(uint64_t));
    for (uint8_t i = 0; i < depth; i++) {
        sizes[i] = levelSizes[i] + 1;  // +1 for Elias requirement
    }

    meta->levelSizes = malloc(varintEliasDeltaMaxBytes(depth));
    varintEliasMeta emeta;
    varintEliasDeltaEncodeArray(meta->levelSizes, sizes, depth, &emeta);
    meta->levelBits = emeta.totalBits;

    free(sizes);
}
```

## Comparison: Gamma vs Delta

| Characteristic  | Gamma                     | Delta                    |
| --------------- | ------------------------- | ------------------------ |
| Best for        | Very small integers (1-7) | Medium integers (8-1000) |
| Bits for N=1    | 1                         | 1                        |
| Bits for N=10   | 7                         | 8                        |
| Bits for N=100  | 13                        | 11                       |
| Bits for N=1000 | 19                        | 17                       |
| Crossover point | N <= 31: Gamma wins       | N > 31: Delta wins       |

**Rule of thumb**: Use Gamma for data with mean < 10, Delta for mean > 30.

## Performance Characteristics

### Space Efficiency

| Value Range | Gamma Bits | Delta Bits | Fixed 64-bit |
| ----------- | ---------- | ---------- | ------------ |
| 1-3         | 1-3        | 1-4        | 64           |
| 4-15        | 5-7        | 5-8        | 64           |
| 16-255      | 9-15       | 9-13       | 64           |
| 256-65535   | 17-31      | 17-23      | 64           |

### Time Complexity

| Operation    | Complexity |
| ------------ | ---------- |
| Encode       | O(log N)   |
| Decode       | O(log N)   |
| Encode array | O(n log M) |
| Decode array | O(n log M) |

Where N = value, M = max value, n = count

## When to Use varintElias

### Use When:

- Values are **small positive integers** (1-1000)
- Distribution is **geometric** or **power-law**
- Need **bit-level precision** (every bit counts)
- Values follow **natural sequences** (depths, lengths, counts)
- Building **succinct data structures**

### Don't Use When:

- Values include **zero** (add 1 first, or use different encoding)
- Values are **uniformly distributed**
- Need **byte-aligned** output
- Values are **negative** (use ZigZag encoding first)
- Random access to individual values needed

## Implementation Details

### Source Files

- **Header**: `src/varintElias.h`
- **Implementation**: `src/varintElias.c`

### Handling Zero Values

Elias codes require values >= 1. For zero handling:

```c
// Option 1: Add 1 before encoding
encoded_value = value + 1;

// Option 2: Use ZigZag for signed values
// Converts: 0 -> 1, -1 -> 2, 1 -> 3, -2 -> 4, 2 -> 5, ...
uint64_t zigzag = (value << 1) ^ (value >> 63);
```

### Testing

See `src/varintElias.c` (test section) for comprehensive test cases.

## See Also

- [Architecture Overview](../ARCHITECTURE.md)
- [Choosing Varint Types](../CHOOSING_VARINTS.md)
- [varintRLE](varintRLE.md) - Run-length encoding for repeated values
- [varintBP128](varintBP128.md) - Block-packed encoding for arrays
- [varintTagged](varintTagged.md) - Byte-aligned varint encoding
