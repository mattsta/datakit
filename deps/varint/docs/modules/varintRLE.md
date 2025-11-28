# varintRLE: Run-Length Encoding

## Overview

**varintRLE** provides efficient compression for data with repeated consecutive values. It encodes sequences as (run-length, value) pairs using varintTagged for compact representation.

**Key Features**: Excellent compression for sparse data, random access by index, streaming encode/decode, automatic benefit analysis.

## Key Characteristics

| Property        | Value                                   |
| --------------- | --------------------------------------- |
| Implementation  | Header (.h) + Compiled (.c)             |
| Encoding Format | [length: varint][value: varint] pairs   |
| Best For        | Data with many consecutive equal values |
| Compression     | Up to 99%+ for highly repetitive data   |
| Random Access   | O(n) where n = number of runs           |

## Encoding Format

### Basic Format

Each run is encoded as two varints:

```
[run_length: 1-9 bytes][value: 1-9 bytes]
```

### With Header Format

Includes total count for decoding:

```
[total_count: varint][run1][run2]...[runN]
```

### Compression Example

```
Input:  [0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,0]  (18 values × 8 bytes = 144 bytes)
Output: [5,0][3,1][10,0]                        (6 bytes)
Compression: 96%
```

## API Reference

### Analysis Functions

```c
// Analyze data for RLE suitability
bool varintRLEAnalyze(const uint64_t *values, size_t count, varintRLEMeta *meta);

// Calculate encoded size without encoding
size_t varintRLESize(const uint64_t *values, size_t count);

// Check if RLE would reduce size
bool varintRLEIsBeneficial(const uint64_t *values, size_t count);
```

### Encoding Functions

```c
// Encode without header (caller tracks count)
size_t varintRLEEncode(uint8_t *dst, const uint64_t *values, size_t count,
                       varintRLEMeta *meta);

// Encode with count header (self-describing)
size_t varintRLEEncodeWithHeader(uint8_t *dst, const uint64_t *values,
                                 size_t count, varintRLEMeta *meta);
```

### Decoding Functions

```c
// Decode single run
size_t varintRLEDecodeRun(const uint8_t *src, size_t *runLength, uint64_t *value);

// Decode entire stream (needs external count)
size_t varintRLEDecode(const uint8_t *src, uint64_t *values, size_t maxCount);

// Decode with header (self-describing)
size_t varintRLEDecodeWithHeader(const uint8_t *src, uint64_t *values,
                                 size_t maxCount);
```

### Random Access

```c
// Get value at specific index (O(n) runs)
uint64_t varintRLEGetAt(const uint8_t *src, size_t index);

// Get total count from header
size_t varintRLEGetCount(const uint8_t *src);

// Count runs in encoded data
size_t varintRLEGetRunCount(const uint8_t *src, size_t encodedSize);
```

### Metadata Structure

```c
typedef struct varintRLEMeta {
    size_t count;        // Total values encoded
    size_t runCount;     // Number of runs
    size_t encodedSize;  // Bytes used in encoding
    size_t uniqueValues; // Unique values seen
} varintRLEMeta;
```

## Real-World Examples

### Example 1: Sparse Sensor Data

IoT sensors often report zeros when idle:

```c
#include "varintRLE.h"

typedef struct {
    uint8_t *encoded;
    size_t encodedSize;
    size_t sampleCount;
} CompressedSensorLog;

bool sensorLogCompress(CompressedSensorLog *log, const uint64_t *readings,
                       size_t count) {
    // Check if RLE is beneficial
    if (!varintRLEIsBeneficial(readings, count)) {
        return false;  // Use raw storage instead
    }

    // Allocate max possible size
    size_t maxSize = varintRLEMaxBytes(count);
    log->encoded = malloc(maxSize);

    // Encode with header for self-describing format
    varintRLEMeta meta;
    log->encodedSize = varintRLEEncodeWithHeader(log->encoded, readings,
                                                  count, &meta);

    // Shrink allocation
    log->encoded = realloc(log->encoded, log->encodedSize);
    log->sampleCount = count;

    printf("Compressed %zu readings (%zu runs) from %zu to %zu bytes (%.1f%%)\n",
           count, meta.runCount, count * 8, log->encodedSize,
           100.0 * (1.0 - (double)log->encodedSize / (count * 8)));

    return true;
}

// Typical IoT sensor: 99% zeros, 1% readings
// 86,400 daily samples (1/sec): 691 KB raw -> ~2 KB compressed
```

### Example 2: Bitmap Compression

Compress sparse bitmaps for set membership:

```c
#include "varintRLE.h"

typedef struct {
    uint8_t *rleData;
    size_t rleSize;
    size_t totalBits;
} CompressedBitmap;

void bitmapCompress(CompressedBitmap *bm, const uint64_t *bitmap,
                    size_t wordCount) {
    // Each word represents 64 bits
    // Count consecutive equal words
    bm->totalBits = wordCount * 64;

    size_t maxSize = varintRLEMaxBytes(wordCount);
    bm->rleData = malloc(maxSize);

    varintRLEMeta meta;
    bm->rleSize = varintRLEEncodeWithHeader(bm->rleData, bitmap,
                                             wordCount, &meta);

    printf("Bitmap: %zu bits in %zu runs, %zu bytes\n",
           bm->totalBits, meta.runCount, bm->rleSize);
}

bool bitmapTestBit(const CompressedBitmap *bm, size_t bitIndex) {
    size_t wordIndex = bitIndex / 64;
    size_t bitOffset = bitIndex % 64;

    uint64_t word = varintRLEGetAt(bm->rleData + 1, wordIndex);  // Skip header
    return (word >> bitOffset) & 1;
}

// Sparse bitmap with 0.1% set bits:
// Raw: 1M bits = 128 KB
// RLE: ~500 bytes (depends on clustering)
```

### Example 3: Database NULL Columns

Compress NULL indicator columns:

```c
#include "varintRLE.h"

typedef struct {
    uint8_t *nullMap;    // RLE-compressed
    size_t nullMapSize;
    uint8_t *data;       // Packed non-NULL values
    size_t dataSize;
    size_t rowCount;
} NullableColumn;

void columnStore(NullableColumn *col, const int64_t *values,
                 const bool *isNull, size_t count) {
    // Convert NULL flags to 0/1 array
    uint64_t *nullFlags = malloc(count * sizeof(uint64_t));
    size_t nonNullCount = 0;

    for (size_t i = 0; i < count; i++) {
        nullFlags[i] = isNull[i] ? 1 : 0;
        if (!isNull[i]) nonNullCount++;
    }

    // RLE compress NULL map
    col->nullMap = malloc(varintRLEMaxBytes(count));
    varintRLEMeta meta;
    col->nullMapSize = varintRLEEncodeWithHeader(col->nullMap, nullFlags,
                                                  count, &meta);
    col->nullMap = realloc(col->nullMap, col->nullMapSize);

    // Store only non-NULL values
    col->data = malloc(nonNullCount * sizeof(int64_t));
    size_t j = 0;
    for (size_t i = 0; i < count; i++) {
        if (!isNull[i]) {
            memcpy(col->data + j * sizeof(int64_t), &values[i], sizeof(int64_t));
            j++;
        }
    }
    col->dataSize = nonNullCount * sizeof(int64_t);
    col->rowCount = count;

    free(nullFlags);

    // Column with 90% NULLs, 1M rows:
    // Traditional: 1M × (8 + 1) = 9 MB
    // Compressed: ~100 bytes NULL map + 100K × 8 = 800 KB
}
```

### Example 4: Game State Delta Compression

Compress unchanged pixels/tiles between frames:

```c
#include "varintRLE.h"

typedef struct {
    uint8_t *delta;
    size_t deltaSize;
    size_t width;
    size_t height;
} FrameDelta;

void frameDelta(FrameDelta *delta, const uint32_t *prevFrame,
                const uint32_t *currFrame, size_t width, size_t height) {
    size_t pixelCount = width * height;

    // Create change mask: 0 = unchanged, pixel value = changed
    uint64_t *changes = malloc(pixelCount * sizeof(uint64_t));
    for (size_t i = 0; i < pixelCount; i++) {
        changes[i] = (prevFrame[i] != currFrame[i]) ? currFrame[i] : 0;
    }

    // RLE compress (most pixels unchanged = 0)
    delta->delta = malloc(varintRLEMaxBytes(pixelCount));
    varintRLEMeta meta;
    delta->deltaSize = varintRLEEncodeWithHeader(delta->delta, changes,
                                                  pixelCount, &meta);
    delta->width = width;
    delta->height = height;

    free(changes);

    // 1080p frame with 5% changes:
    // Raw delta: 8 MB
    // RLE: ~400 KB (depends on change clustering)
}
```

### Example 5: Time Series Compression

Compress steady-state readings:

```c
#include "varintRLE.h"

typedef struct {
    uint8_t *encoded;
    size_t encodedSize;
    uint64_t baseTimestamp;
    size_t sampleCount;
    uint32_t sampleRateHz;
} CompressedTimeSeries;

void timeSeriesCompress(CompressedTimeSeries *ts, const uint64_t *values,
                        size_t count, uint64_t baseTime, uint32_t rateHz) {
    ts->baseTimestamp = baseTime;
    ts->sampleCount = count;
    ts->sampleRateHz = rateHz;

    // Analyze compression benefit
    varintRLEMeta meta;
    varintRLEAnalyze(values, count, &meta);

    printf("Time series: %zu samples, %zu unique values, %zu runs\n",
           count, meta.uniqueValues, meta.runCount);

    // Encode
    ts->encoded = malloc(varintRLEMaxBytes(count));
    ts->encodedSize = varintRLEEncodeWithHeader(ts->encoded, values,
                                                 count, &meta);

    // Calculate compression stats
    double rawSize = count * sizeof(uint64_t);
    double ratio = ts->encodedSize / rawSize;
    printf("Compression: %.2f%% of original\n", ratio * 100);
}

// Temperature sensor (steady state): 86400 samples/day
// Raw: 691 KB
// RLE (stable temp): ~100 bytes per degree change
```

### Example 6: Document Term Positions

Compress term position lists in search indexes:

```c
#include "varintRLE.h"

typedef struct {
    uint32_t docId;
    uint8_t *positions;   // RLE-encoded gap sequence
    size_t positionsSize;
    size_t termFrequency;
} PostingEntry;

void indexTerm(PostingEntry *entry, uint32_t docId,
               const uint32_t *positions, size_t count) {
    entry->docId = docId;
    entry->termFrequency = count;

    // Convert to gap encoding (delta between positions)
    uint64_t *gaps = malloc(count * sizeof(uint64_t));
    gaps[0] = positions[0];
    for (size_t i = 1; i < count; i++) {
        gaps[i] = positions[i] - positions[i-1];
    }

    // RLE compress gaps (repeated gaps = word repetition patterns)
    entry->positions = malloc(varintRLEMaxBytes(count));
    varintRLEMeta meta;
    entry->positionsSize = varintRLEEncodeWithHeader(entry->positions,
                                                      gaps, count, &meta);

    free(gaps);

    // Common word appearing every 100 words in 10K word doc:
    // 100 positions with gap=100 each
    // Raw: 800 bytes
    // RLE: ~10 bytes (single run!)
}
```

## Performance Characteristics

### Compression Ratios

| Data Pattern           | Compression Ratio |
| ---------------------- | ----------------- |
| All same value         | 99%+              |
| 90% zeros, 10% random  | 80-90%            |
| 50% runs of length 10+ | 60-80%            |
| Mostly unique values   | -10% (overhead)   |

### Time Complexity

| Operation     | Complexity |
| ------------- | ---------- |
| Encode        | O(n)       |
| Decode        | O(n)       |
| Random access | O(r)       |
| Size analysis | O(n)       |

Where n = value count, r = run count

### Memory Usage

- Encoding buffer: `varintRLEMaxBytes(count)` = count × 18 bytes (worst case)
- Typical: 2-10 bytes per run

## When to Use varintRLE

### Use When:

- Data has **many consecutive repeated values**
- **Sparse data** (mostly zeros or a default value)
- **Clustered changes** (changes occur in groups)
- Need **random access** to compressed data
- **Streaming** encode/decode is needed

### Don't Use When:

- Data is **highly random** (each value unique)
- Values change **every element**
- Need **O(1) random access** (use uncompressed)
- Data is already compressed

## Implementation Details

### Source Files

- **Header**: `src/varintRLE.h`
- **Implementation**: `src/varintRLE.c`

### Dependencies

- `varintTagged.h` - For variable-length integer encoding

### Testing

See `src/varintRLE.c` (test section) for comprehensive test cases.

## See Also

- [Architecture Overview](../ARCHITECTURE.md)
- [Choosing Varint Types](../CHOOSING_VARINTS.md)
- [varintTagged](varintTagged.md) - Underlying varint encoding
- [varintBP128](varintBP128.md) - Alternative for sorted integers
- [varintElias](varintElias.md) - Bit-level compression for small integers
