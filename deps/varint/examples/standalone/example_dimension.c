/**
 * example_dimension.c - Demonstrates varintDimension usage
 *
 * varintDimension provides efficient storage for matrices and vectors with:
 * - Variable-width dimensions (rows/columns can be 0-8 bytes each)
 * - Variable-width entries (each value uses minimum bytes needed)
 * - Bit matrices (1 bit per entry)
 * - Sparse matrix flag
 *
 * Perfect for: sparse matrices, ML feature matrices, graph adjacency matrices.
 *
 * Compile: gcc -I../src example_dimension.c ../src/varintDimension.c
 * ../src/varintExternal.c -o example_dimension Run: ./example_dimension
 */

#include "varintDimension.h"
#include "varintExternal.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Memory allocation check macro for demo programs */
#define CHECK_ALLOC(ptr)                                                       \
    do {                                                                       \
        if (!(ptr)) {                                                          \
            fprintf(stderr, "Error: Memory allocation failed at %s:%d\n",      \
                    __FILE__, __LINE__);                                       \
            exit(EXIT_FAILURE);                                                \
        }                                                                      \
    } while (0)

// Example 1: Basic matrix storage
void example_basic_matrix(void) {
    printf("\n=== Example 1: Basic Matrix Storage ===\n");

    // 3×4 matrix of small integers (0-255)
    size_t rows = 3;
    size_t cols = 4;

    // Create dimension pair
    varintDimensionPair dim = varintDimensionPairDimension(rows, cols);

    // Allocate storage (metadata + 3*4*1 bytes for entries)
    size_t metadataSize = VARINT_DIMENSION_PAIR_BYTE_LENGTH(dim);
    varintWidth entryWidth = 1; // 1 byte per entry
    size_t totalSize = metadataSize + (rows * cols * entryWidth);

    uint8_t *matrix = calloc(1, totalSize);

    // Store dimensions in the buffer
    varintDimensionPairEncode(matrix, rows, cols);

    printf("Matrix: %zu×%zu\n", rows, cols);
    printf("Metadata: %zu bytes, Data: %zu bytes, Total: %zu bytes\n",
           metadataSize, rows * cols * entryWidth, totalSize);

    // Set some values
    const uint64_t values[3][4] = {
        {10, 20, 30, 40}, {50, 60, 70, 80}, {90, 100, 110, 120}};

    for (size_t r = 0; r < rows; r++) {
        for (size_t c = 0; c < cols; c++) {
            varintDimensionPairEntrySetUnsigned(matrix, r, c, values[r][c],
                                                entryWidth, dim);
        }
    }

    // Read back and verify
    printf("\nMatrix contents:\n");
    for (size_t r = 0; r < rows; r++) {
        printf("  [");
        for (size_t c = 0; c < cols; c++) {
            uint64_t value = varintDimensionPairEntryGetUnsigned(
                matrix, r, c, entryWidth, dim);
            printf("%3" PRIu64, value);
            if (c < cols - 1) {
                printf(", ");
            }
            assert(value == values[r][c]);
        }
        printf("]\n");
    }

    printf("✓ Matrix storage and retrieval works\n");

    free(matrix);
}

// Example 2: Vector storage (1D array)
void example_vector(void) {
    printf("\n=== Example 2: Vector Storage ===\n");

    // Vector: row=0, cols=length
    size_t length = 8;
    size_t rows = 0; // Special case for vector

    varintDimensionPair dim = varintDimensionPairDimension(rows, length);

    size_t metadataSize = VARINT_DIMENSION_PAIR_BYTE_LENGTH(dim);
    varintWidth entryWidth = 2; // 2 bytes per entry
    size_t totalSize = metadataSize + (length * entryWidth);

    uint8_t *vector = calloc(1, totalSize);

    varintDimensionPairEncode(vector, rows, length);

    printf("Vector length: %zu (stored as %zu×%zu)\n", length, rows, length);
    printf("Total size: %zu bytes (%zu metadata + %zu data)\n", totalSize,
           metadataSize, length * entryWidth);

    // Set values
    const uint64_t values[] = {100, 200, 300, 400, 500, 600, 700, 800};
    for (size_t i = 0; i < length; i++) {
        varintDimensionPairEntrySetUnsigned(vector, 0, i, values[i], entryWidth,
                                            dim);
    }

    // Read back
    printf("Vector: [");
    for (size_t i = 0; i < length; i++) {
        uint64_t value =
            varintDimensionPairEntryGetUnsigned(vector, 0, i, entryWidth, dim);
        printf("%" PRIu64, value);
        if (i < length - 1) {
            printf(", ");
        }
        assert(value == values[i]);
    }
    printf("]\n");

    printf("✓ Vector storage works\n");

    free(vector);
}

// Example 3: Bit matrix
void example_bit_matrix(void) {
    printf("\n=== Example 3: Bit Matrix (1 bit per entry) ===\n");

    // 8×8 adjacency matrix for a graph
    size_t size = 8;
    varintDimensionPair dim = varintDimensionPairDimension(size, size);

    size_t metadataSize = VARINT_DIMENSION_PAIR_BYTE_LENGTH(dim);
    size_t bitCount = size * size;
    size_t bitsBytes = (bitCount + 7) / 8;
    size_t totalSize = metadataSize + bitsBytes;

    uint8_t *adjMatrix = calloc(1, totalSize);

    varintDimensionPairEncode(adjMatrix, size, size);

    printf("Adjacency matrix: %zu×%zu\n", size, size);
    printf("Storage: %zu bytes (%zu metadata + %zu for %zu bits)\n", totalSize,
           metadataSize, bitsBytes, bitCount);

    // Set some edges (symmetric graph)
    int edges[][2] = {{0, 1}, {0, 2}, {1, 3}, {2, 3},
                      {3, 4}, {4, 5}, {5, 6}, {6, 7}};
    size_t edgeCount = sizeof(edges) / sizeof(edges[0]);

    for (size_t i = 0; i < edgeCount; i++) {
        int u = edges[i][0];
        int v = edges[i][1];

        varintDimensionPairEntrySetBit(adjMatrix, u, v, true, dim);
        varintDimensionPairEntrySetBit(adjMatrix, v, u, true, dim); // Symmetric
    }

    // Display matrix
    printf("\nAdjacency matrix (1=edge, 0=no edge):\n");
    printf("   ");
    for (size_t c = 0; c < size; c++) {
        printf(" %zu", c);
    }
    printf("\n");

    for (size_t r = 0; r < size; r++) {
        printf(" %zu [", r);
        for (size_t c = 0; c < size; c++) {
            bool hasEdge = varintDimensionPairEntryGetBit(adjMatrix, r, c, dim);
            printf(" %d", hasEdge ? 1 : 0);
        }
        printf("]\n");
    }

    // Verify edges
    for (size_t i = 0; i < edgeCount; i++) {
        int u = edges[i][0];
        int v = edges[i][1];
        assert(varintDimensionPairEntryGetBit(adjMatrix, u, v, dim));
        assert(varintDimensionPairEntryGetBit(adjMatrix, v, u, dim));
    }

    printf("✓ Bit matrix storage works\n");

    free(adjMatrix);
}

// Example 4: Sparse matrix
void example_sparse_matrix(void) {
    printf("\n=== Example 4: Sparse Matrix ===\n");

    // Large sparse matrix: 100×100 with only 10 non-zero entries
    size_t rows = 100;
    size_t cols = 100;

    varintDimensionPair dim = varintDimensionPairDimension(rows, cols);
    // Note: The sparse flag is the LSB. In practice, sparse matrices
    // would use different storage (COO/CSR), not just a flag.
    dim |= 0x01; // Set sparse bit (LSB)

    printf("Matrix: %zu×%zu (sparse flag demonstration)\n", rows, cols);
    printf("Sparse flag: %s\n",
           VARINT_DIMENSION_PAIR_IS_SPARSE(dim) ? "YES" : "NO");

    // In a real implementation, sparse matrices would use
    // coordinate list (COO) or compressed sparse row (CSR) format
    printf("✓ Sparse matrix flag works\n");
}

// Example 5: Dimension encoding/decoding
void example_dimension_encoding(void) {
    printf("\n=== Example 5: Dimension Encoding ===\n");

    struct {
        size_t rows;
        size_t cols;
    } tests[] = {
        {10, 20},          // Small dimensions
        {256, 256},        // 2-byte dimensions
        {65536, 100},      // 3-byte row, 1-byte col
        {1000000, 500000}, // Large dimensions
    };

    printf("Row Count | Col Count | Row Width | Col Width | Metadata Bytes\n");
    printf("----------|-----------|-----------|-----------|---------------\n");

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        varintDimensionPair dim =
            varintDimensionPairDimension(tests[i].rows, tests[i].cols);

        varintWidth rowWidth = VARINT_DIMENSION_PAIR_WIDTH_ROW_COUNT(dim);
        varintWidth colWidth = VARINT_DIMENSION_PAIR_WIDTH_COL_COUNT(dim);
        size_t metadataSize = VARINT_DIMENSION_PAIR_BYTE_LENGTH(dim);

        printf("%9zu | %9zu | %9d | %9d | %14zu\n", tests[i].rows,
               tests[i].cols, rowWidth, colWidth, metadataSize);

        // Encode creates the dimension metadata and stores it
        uint8_t buffer[32];
        varintDimensionPair encoded =
            varintDimensionPairEncode(buffer, tests[i].rows, tests[i].cols);

        // Verify the metadata matches what we calculated
        assert(encoded == dim);
    }

    printf("✓ Dimension encoding/decoding works\n");
}

// Example 6: ML feature matrix
void example_ml_features(void) {
    printf("\n=== Example 6: ML Feature Matrix ===\n");

    // 1000 samples × 50 features
    size_t samples = 1000;
    size_t features = 50;

    varintDimensionPair dim = varintDimensionPairDimension(samples, features);

    // Features are bounded 0-255
    varintWidth featureWidth = 1;

    size_t metadataSize = VARINT_DIMENSION_PAIR_BYTE_LENGTH(dim);
    size_t dataSize = samples * features * featureWidth;
    size_t totalSize = metadataSize + dataSize;

    printf("Dataset: %zu samples × %zu features\n", samples, features);
    printf("Storage: %zu bytes\n", totalSize);

    // Compare with standard representations
    size_t uint8Size = samples * features * sizeof(uint8_t);
    size_t doubleSize = samples * features * sizeof(double);

    printf("\nComparison:\n");
    printf("  varintDimension: %zu bytes\n", totalSize);
    printf("  uint8_t array:   %zu bytes (%.1fx)\n", uint8Size,
           (float)uint8Size / totalSize);
    printf("  double array:    %zu bytes (%.1fx)\n", doubleSize,
           (float)doubleSize / totalSize);

    printf("✓ ML feature matrix example\n");
}

// Example 7: Dynamic dimension calculation
void example_dynamic_dimensions(void) {
    printf("\n=== Example 7: Dynamic Dimension Calculation ===\n");

    // Automatically determine dimensions from data
    const uint64_t dataset[] = {10, 20, 30, 40,  50,  60,
                                70, 80, 90, 100, 110, 120};
    size_t rows = 4;
    size_t cols = 3;
    size_t count = rows * cols;

    // Find maximum value to determine entry width
    uint64_t maxValue = 0;
    for (size_t i = 0; i < count; i++) {
        if (dataset[i] > maxValue) {
            maxValue = dataset[i];
        }
    }

    // Determine entry width
    varintWidth entryWidth;
    varintExternalUnsignedEncoding(maxValue, entryWidth);

    printf("Dataset: %zu values\n", count);
    printf("Max value: %" PRIu64 "\n", maxValue);
    printf("Entry width: %d bytes\n", entryWidth);

    // Create matrix
    varintDimensionPair dim = varintDimensionPairDimension(rows, cols);
    size_t totalSize =
        VARINT_DIMENSION_PAIR_BYTE_LENGTH(dim) + (rows * cols * entryWidth);

    uint8_t *matrix = calloc(1, totalSize);
    varintDimensionPairEncode(matrix, rows, cols);

    // Store data
    for (size_t r = 0; r < rows; r++) {
        for (size_t c = 0; c < cols; c++) {
            size_t idx = r * cols + c;
            varintDimensionPairEntrySetUnsigned(matrix, r, c, dataset[idx],
                                                entryWidth, dim);
        }
    }

    printf("\nStored matrix (%zu×%zu):\n", rows, cols);
    for (size_t r = 0; r < rows; r++) {
        printf("  [");
        for (size_t c = 0; c < cols; c++) {
            uint64_t value = varintDimensionPairEntryGetUnsigned(
                matrix, r, c, entryWidth, dim);
            printf("%3" PRIu64, value);
            if (c < cols - 1) {
                printf(", ");
            }
        }
        printf("]\n");
    }

    printf("Total size: %zu bytes (vs %zu for uint64_t array)\n", totalSize,
           count * sizeof(uint64_t));

    printf("✓ Dynamic dimension calculation works\n");

    free(matrix);
}

int main(void) {
    printf("===========================================\n");
    printf("   varintDimension Example Suite\n");
    printf("===========================================\n");

    example_basic_matrix();
    example_vector();
    example_bit_matrix();
    example_sparse_matrix();
    example_dimension_encoding();
    example_ml_features();
    example_dynamic_dimensions();

    printf("\n===========================================\n");
    printf("All examples completed successfully!\n");
    printf("===========================================\n");

    return 0;
}
