# varintDimension: Bit-Packed Matrix Storage

## Overview

**varintDimension** provides efficient storage for matrices and 2D arrays where row/column dimensions can be compactly encoded. Built on top of varintPacked, it supports **144 different encoding combinations** for various dimension sizes and sparsity patterns.

**Key Features**: Dense and sparse matrix representations, variable dimension widths (1-8 bytes per axis), efficient storage for matrices with small dimensions.

## Key Characteristics

| Property              | Value                                               |
| --------------------- | --------------------------------------------------- |
| Implementation        | Header (.h) + Compiled (.c)                         |
| Dimension Widths      | 1-8 bytes per axis (row/column)                     |
| Encoding Combinations | 144 (9 row widths × 8 col widths × 2 density modes) |
| Density Modes         | Dense and Sparse                                    |
| Use Case              | Matrices with bounded dimensions                    |

## Matrix Encoding System

### Dimension Pairs

Matrices are described by `varintDimensionPair` enum values encoding:

- **Row width**: 0-8 bytes (0 = vector/1D array)
- **Column width**: 1-8 bytes
- **Density**: Dense or Sparse

**Format**: `VARINT_DIMENSION_PAIR_[DENSE|SPRSE]_R_C`

Examples:

```c
VARINT_DIMENSION_PAIR_DENSE_2_2  // 2-byte rows × 2-byte cols, dense
VARINT_DIMENSION_PAIR_SPRSE_4_1  // 4-byte rows × 1-byte cols, sparse
VARINT_DIMENSION_PAIR_DENSE_0_4  // Vector (0 rows) × 4-byte cols, dense
```

### Dimension Macros

```c
// Extract row width (in bytes)
uint8_t rowWidth = VARINT_DIMENSION_PAIR_WIDTH_ROW_COUNT(dimensionPair);

// Extract column width (in bytes)
uint8_t colWidth = VARINT_DIMENSION_PAIR_WIDTH_COL_COUNT(dimensionPair);

// Check if sparse
bool isSparse = VARINT_DIMENSION_PAIR_IS_SPARSE(dimensionPair);

// Create dimension pair
varintDimensionPair pair = VARINT_DIMENSION_PAIR_PAIR(rowBytes, colBytes, isSparse);
```

### Packed Dimensions

For matrices where both dimensions fit in a single byte:

```c
typedef enum varintDimensionPacked {
    VARINT_DIMENSION_PACKED_1,  // up to 15 × 15
    VARINT_DIMENSION_PACKED_2,  // up to 255 × 255
    VARINT_DIMENSION_PACKED_3,  // up to 4095 × 4095
    VARINT_DIMENSION_PACKED_4,  // up to 65535 × 65535
    VARINT_DIMENSION_PACKED_5,  // up to 1,048,575 × 1,048,575
    VARINT_DIMENSION_PACKED_6,  // up to 16,777,215 × 16,777,215
    VARINT_DIMENSION_PACKED_7,  // up to 268,435,455 × 268,435,455
    VARINT_DIMENSION_PACKED_8,  // up to 4,294,967,295 × 4,294,967,295
} varintDimensionPacked;
```

## Real-World Examples

### Example 1: Small Integer Matrix

Store a 100×100 matrix of 8-bit values:

```c
#include "varintDimension.h"

typedef struct {
    uint8_t *data;
    size_t rows;
    size_t cols;
    varintDimensionPair encoding;
} Matrix8;

void matrixInit(Matrix8 *m, size_t rows, size_t cols) {
    assert(rows <= 255 && cols <= 255);  // Fit in 1 byte each

    m->rows = rows;
    m->cols = cols;
    m->encoding = VARINT_DIMENSION_PAIR_DENSE_1_1;  // 1 byte × 1 byte

    // Allocate: rows × cols × element_size
    // For 8-bit elements: 100 × 100 × 1 = 10,000 bytes
    m->data = calloc(rows * cols, sizeof(uint8_t));
}

void matrixSet(Matrix8 *m, size_t row, size_t col, uint8_t value) {
    assert(row < m->rows && col < m->cols);
    m->data[row * m->cols + col] = value;
}

uint8_t matrixGet(const Matrix8 *m, size_t row, size_t col) {
    assert(row < m->rows && col < m->cols);
    return m->data[row * m->cols + col];
}

// Standard matrix: 100 × 100 × 1 byte = 10 KB
// With dimension metadata: 10 KB + 1 byte (encoding) = 10,001 bytes
```

### Example 2: Sparse Matrix with Coordinates

Store only non-zero entries with (row, col, value) tuples:

```c
#include "varintDimension.h"

typedef struct {
    uint16_t row;
    uint16_t col;
    float value;
} SparseEntry;

typedef struct {
    SparseEntry *entries;
    size_t count;
    size_t capacity;
    varintDimensionPair encoding;
} SparseMatrix;

void sparseInit(SparseMatrix *m, size_t maxRows, size_t maxCols) {
    assert(maxRows <= 65535 && maxCols <= 65535);  // 2 bytes each

    m->entries = NULL;
    m->count = 0;
    m->capacity = 0;
    m->encoding = VARINT_DIMENSION_PAIR_SPRSE_2_2;  // 2 byte × 2 byte, sparse
}

void sparseSet(SparseMatrix *m, uint16_t row, uint16_t col, float value) {
    // Find existing entry or add new one
    for (size_t i = 0; i < m->count; i++) {
        if (m->entries[i].row == row && m->entries[i].col == col) {
            m->entries[i].value = value;
            return;
        }
    }

    // Add new entry
    if (m->count >= m->capacity) {
        m->capacity = m->capacity ? m->capacity * 2 : 16;
        m->entries = realloc(m->entries, m->capacity * sizeof(SparseEntry));
    }

    m->entries[m->count++] = (SparseEntry){row, col, value};
}

// For 1000×1000 matrix with 1% density:
// Dense: 1,000,000 × 4 bytes = 4 MB
// Sparse: 10,000 entries × 8 bytes = 80 KB
// Savings: 98%
```

### Example 3: Graph Adjacency Matrix

Store graph with bounded node IDs:

```c
#include "varintDimension.h"

typedef struct {
    uint8_t *adjacency;  // Bit matrix
    size_t nodeCount;
    varintDimensionPair encoding;
} Graph;

void graphInit(Graph *g, size_t maxNodes) {
    assert(maxNodes <= 255);  // 1 byte per dimension

    g->nodeCount = maxNodes;
    g->encoding = VARINT_DIMENSION_PAIR_DENSE_1_1;

    // Store as bit matrix: maxNodes × maxNodes bits
    size_t bitsNeeded = maxNodes * maxNodes;
    size_t bytesNeeded = (bitsNeeded + 7) / 8;
    g->adjacency = calloc(1, bytesNeeded);
}

void graphAddEdge(Graph *g, uint8_t from, uint8_t to) {
    assert(from < g->nodeCount && to < g->nodeCount);

    // Set bit at position [from][to]
    size_t bitIndex = from * g->nodeCount + to;
    size_t byteIndex = bitIndex / 8;
    uint8_t bitOffset = bitIndex % 8;

    g->adjacency[byteIndex] |= (1 << bitOffset);
}

bool graphHasEdge(const Graph *g, uint8_t from, uint8_t to) {
    size_t bitIndex = from * g->nodeCount + to;
    size_t byteIndex = bitIndex / 8;
    uint8_t bitOffset = bitIndex % 8;

    return (g->adjacency[byteIndex] & (1 << bitOffset)) != 0;
}

// For 256 nodes:
// Full matrix (4 bytes/edge): 256 × 256 × 4 = 262 KB
// Bit matrix: 256 × 256 / 8 = 8 KB
// Savings: 97%
```

### Example 4: Image Thumbnail Grid

Store thumbnails with coordinates:

```c
#include "varintDimension.h"

typedef struct {
    uint32_t x;       // X coordinate (4 bytes)
    uint32_t y;       // Y coordinate (4 bytes)
    uint8_t thumbnail[64];  // 8×8 thumbnail
} ThumbnailEntry;

typedef struct {
    ThumbnailEntry *entries;
    size_t count;
    varintDimensionPair encoding;
} ThumbnailGrid;

void gridInit(ThumbnailGrid *grid) {
    grid->entries = NULL;
    grid->count = 0;
    // Coordinates up to uint32_t max
    grid->encoding = VARINT_DIMENSION_PAIR_DENSE_4_4;
}

void gridAddThumbnail(ThumbnailGrid *grid, uint32_t x, uint32_t y,
                      const uint8_t thumbnail[64]) {
    grid->entries = realloc(grid->entries,
                           (grid->count + 1) * sizeof(ThumbnailEntry));
    grid->entries[grid->count].x = x;
    grid->entries[grid->count].y = y;
    memcpy(grid->entries[grid->count].thumbnail, thumbnail, 64);
    grid->count++;
}

// Dimension encoding tells us coordinate space is 32-bit × 32-bit
```

### Example 5: Feature Matrix (ML/AI)

Store feature vectors with variable-width feature IDs:

```c
#include "varintDimension.h"

// Samples: up to 65535 (2 bytes)
// Features: up to 16,777,215 (3 bytes)
typedef struct {
    float *values;  // Dense array: samples × features
    size_t sampleCount;
    size_t featureCount;
    varintDimensionPair encoding;
} FeatureMatrix;

void featureInit(FeatureMatrix *fm, size_t samples, size_t features) {
    assert(samples <= 65535);     // 2 bytes
    assert(features <= 16777215); // 3 bytes

    fm->sampleCount = samples;
    fm->featureCount = features;
    fm->encoding = VARINT_DIMENSION_PAIR_DENSE_2_3;
    fm->values = calloc(samples * features, sizeof(float));
}

void featureSet(FeatureMatrix *fm, uint16_t sample, uint32_t feature, float value) {
    assert(sample < fm->sampleCount);
    assert(feature < fm->featureCount);

    fm->values[sample * fm->featureCount + feature] = value;
}

float featureGet(const FeatureMatrix *fm, uint16_t sample, uint32_t feature) {
    return fm->values[sample * fm->featureCount + feature];
}

// Dimension encoding: DENSE_2_3 tells us this is 65K × 16M matrix
```

### Example 6: Vector (1D Array) with Large Index Space

Use 0-row dimension for vectors:

```c
#include "varintDimension.h"

typedef struct {
    uint64_t *data;
    size_t length;
    varintDimensionPair encoding;
} LargeVector;

void vectorInit(LargeVector *v, size_t maxLength) {
    v->length = maxLength;

    // 0 rows = vector, column width depends on maxLength
    if (maxLength <= 255) {
        v->encoding = VARINT_DIMENSION_PAIR_DENSE_0_1;  // 1 byte indices
    } else if (maxLength <= 65535) {
        v->encoding = VARINT_DIMENSION_PAIR_DENSE_0_2;  // 2 byte indices
    } else if (maxLength <= 4294967295UL) {
        v->encoding = VARINT_DIMENSION_PAIR_DENSE_0_4;  // 4 byte indices
    } else {
        v->encoding = VARINT_DIMENSION_PAIR_DENSE_0_8;  // 8 byte indices
    }

    v->data = calloc(maxLength, sizeof(uint64_t));
}

// Encoding tells us index space size (column dimension)
```

## Performance Characteristics

### Space Efficiency

Dimension metadata is **1 byte** (varintDimensionPair enum).

**Savings** depend on actual dimensions vs. worst-case:

| Matrix Size | Standard (4B×4B) | Optimal Encoding | Savings |
| ----------- | ---------------- | ---------------- | ------- |
| 100×100     | 40 KB            | 10 KB (1B×1B)    | 75%     |
| 1000×1000   | 4 MB             | 1 MB (2B×2B)     | 75%     |
| 10000×100   | 4 MB             | 200 KB (2B×1B)   | 95%     |

### Time Complexity

Same as underlying storage:

- **Dense**: O(1) access
- **Sparse**: O(n) search or O(log n) with sorted entries

## When to Use varintDimension

### Use When:

- Matrix dimensions are **bounded** and known
- Dimensions are **small** relative to pointer size (e.g., 16-bit vs 64-bit)
- Need to encode **dimension metadata** compactly
- Working with **many small matrices**
- Building **sparse matrix** representations

### Don't Use When:

- Dimensions are **unbounded** or unpredictable
- Matrix is **very large** (dimensions close to uint64_t max)
- Standard arrays are sufficient
- Dimension metadata overhead isn't justified

## Implementation Details

### Source Files

- **Header**: `src/varintDimension.h` - Enums and macros
- **Implementation**: `src/varintDimension.c` - Matrix operations

### 144 Encoding Combinations

9 row widths (0-8 bytes) × 8 column widths (1-8 bytes) × 2 modes (dense/sparse) = 144

All combinations enumerated in `varintDimensionPair` enum.

### Testing

See `src/varintDimensionTest.c` for usage examples and tests.

## See Also

- [Architecture Overview](../ARCHITECTURE.md)
- [Choosing Varint Types](../CHOOSING_VARINTS.md)
- [varintPacked](varintPacked.md) - Underlying bit-packing system
- [varintExternal](varintExternal.md) - Variable-width encoding for dimensions
