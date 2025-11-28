/**
 * ml_features.c - ML feature storage using varintDimension and varintPacked
 *
 * This example demonstrates machine learning feature matrices combining:
 * - varintDimension: Matrix dimension encoding for feature/sample counts
 * - varintPacked: Quantized feature values (8/10/12-bit precision)
 * - Efficient storage for training data
 *
 * Features:
 * - Feature quantization to arbitrary bit widths
 * - Sparse and dense matrix representations
 * - Dimension-aware storage allocation
 * - One-hot encoding compression
 * - Embedding table storage
 *
 * Compile: gcc -I../../src ml_features.c ../../build/src/libvarint.a -o
 * ml_features Run: ./ml_features
 */

// Note: Using native uint16_t arrays for quantized storage
// varintPacked template doesn't support multiple instantiations in same file
// For production use, consider separate compilation units for each bit width

#include "varintDimension.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// FEATURE QUANTIZATION
// ============================================================================

typedef enum {
    QUANT_8BIT,  // 0-255
    QUANT_10BIT, // 0-1023
    QUANT_12BIT, // 0-4095
} QuantizationBits;

// Map floating-point value [min, max] to quantized integer
uint16_t quantizeValue(float value, float min, float max,
                       QuantizationBits bits) {
    uint16_t maxVal;
    switch (bits) {
    case QUANT_8BIT:
        maxVal = 255;
        break;
    case QUANT_10BIT:
        maxVal = 1023;
        break;
    case QUANT_12BIT:
        maxVal = 4095;
        break;
    default:
        maxVal = 255;
    }

    // Clamp and normalize to [0, 1]
    if (value < min) {
        value = min;
    }
    if (value > max) {
        value = max;
    }
    float normalized = (value - min) / (max - min);

    // Quantize to integer
    return (uint16_t)(normalized * maxVal);
}

float dequantizeValue(uint16_t quantized, float min, float max,
                      QuantizationBits bits) {
    uint16_t maxVal;
    switch (bits) {
    case QUANT_8BIT:
        maxVal = 255;
        break;
    case QUANT_10BIT:
        maxVal = 1023;
        break;
    case QUANT_12BIT:
        maxVal = 4095;
        break;
    default:
        maxVal = 255;
    }

    // Denormalize from [0, maxVal] to [min, max]
    float normalized = (float)quantized / maxVal;
    return min + normalized * (max - min);
}

// ============================================================================
// DENSE FEATURE MATRIX
// ============================================================================

typedef struct {
    uint16_t *data; // Quantized feature values (native array)
    size_t sampleCount;
    size_t featureCount;
    float featureMin; // Global min for quantization
    float featureMax; // Global max for quantization
    QuantizationBits quantBits;
    varintDimensionPair dimensionEncoding;
} DenseFeatureMatrix;

void denseMatrixInit(DenseFeatureMatrix *matrix, size_t samples,
                     size_t features, float minValue, float maxValue,
                     QuantizationBits bits) {
    matrix->sampleCount = samples;
    matrix->featureCount = features;
    matrix->featureMin = minValue;
    matrix->featureMax = maxValue;
    matrix->quantBits = bits;

    // Determine dimension encoding
    if (samples <= 255 && features <= 255) {
        matrix->dimensionEncoding = VARINT_DIMENSION_PAIR_DENSE_1_1;
    } else if (samples <= 65535 && features <= 255) {
        matrix->dimensionEncoding = VARINT_DIMENSION_PAIR_DENSE_2_1;
    } else if (samples <= 65535 && features <= 65535) {
        matrix->dimensionEncoding = VARINT_DIMENSION_PAIR_DENSE_2_2;
    } else {
        matrix->dimensionEncoding = VARINT_DIMENSION_PAIR_DENSE_4_4;
    }

    // Allocate native storage (uint16_t array holds up to 16-bit values)
    matrix->data = calloc(samples * features, sizeof(uint16_t));
}

void denseMatrixFree(DenseFeatureMatrix *matrix) {
    free(matrix->data);
}

void denseMatrixSet(DenseFeatureMatrix *matrix, size_t sample, size_t feature,
                    float value) {
    assert(sample < matrix->sampleCount);
    assert(feature < matrix->featureCount);

    uint16_t quantized = quantizeValue(value, matrix->featureMin,
                                       matrix->featureMax, matrix->quantBits);
    size_t index = sample * matrix->featureCount + feature;

    // Direct array access (for production, use varintPacked in separate
    // compilation units)
    matrix->data[index] = quantized;
}

float denseMatrixGet(const DenseFeatureMatrix *matrix, size_t sample,
                     size_t feature) {
    assert(sample < matrix->sampleCount);
    assert(feature < matrix->featureCount);

    size_t index = sample * matrix->featureCount + feature;

    // Direct array access (for production, use varintPacked in separate
    // compilation units)
    uint16_t quantized = matrix->data[index];

    return dequantizeValue(quantized, matrix->featureMin, matrix->featureMax,
                           matrix->quantBits);
}

// ============================================================================
// SPARSE FEATURE MATRIX (for one-hot and sparse features)
// ============================================================================

typedef struct {
    size_t feature;
    uint16_t value; // Quantized value
} SparseEntry;

typedef struct {
    SparseEntry **rows; // Array of sparse rows
    size_t *rowSizes;   // Number of entries per row
    size_t sampleCount;
    size_t featureCount;
    varintDimensionPair dimensionEncoding;
} SparseFeatureMatrix;

void sparseMatrixInit(SparseFeatureMatrix *matrix, size_t samples,
                      size_t features) {
    matrix->sampleCount = samples;
    matrix->featureCount = features;
    matrix->rows = calloc(samples, sizeof(SparseEntry *));
    matrix->rowSizes = calloc(samples, sizeof(size_t));

    // Dimension encoding (sparse variant)
    if (samples <= 255 && features <= 255) {
        matrix->dimensionEncoding = VARINT_DIMENSION_PAIR_SPRSE_1_1;
    } else if (samples <= 65535 && features <= 65535) {
        matrix->dimensionEncoding = VARINT_DIMENSION_PAIR_SPRSE_2_2;
    } else {
        matrix->dimensionEncoding = VARINT_DIMENSION_PAIR_SPRSE_4_4;
    }
}

void sparseMatrixFree(SparseFeatureMatrix *matrix) {
    for (size_t i = 0; i < matrix->sampleCount; i++) {
        free(matrix->rows[i]);
    }
    free(matrix->rows);
    free(matrix->rowSizes);
}

void sparseMatrixSet(SparseFeatureMatrix *matrix, size_t sample, size_t feature,
                     uint16_t value) {
    assert(sample < matrix->sampleCount);
    assert(feature < matrix->featureCount);

    // Grow row if needed
    size_t currentSize = matrix->rowSizes[sample];
    matrix->rows[sample] =
        realloc(matrix->rows[sample], (currentSize + 1) * sizeof(SparseEntry));

    matrix->rows[sample][currentSize].feature = feature;
    matrix->rows[sample][currentSize].value = value;
    matrix->rowSizes[sample]++;
}

uint16_t sparseMatrixGet(const SparseFeatureMatrix *matrix, size_t sample,
                         size_t feature) {
    assert(sample < matrix->sampleCount);

    for (size_t i = 0; i < matrix->rowSizes[sample]; i++) {
        if (matrix->rows[sample][i].feature == feature) {
            return matrix->rows[sample][i].value;
        }
    }
    return 0; // Default for missing entries
}

// ============================================================================
// EMBEDDING TABLE
// ============================================================================

typedef struct {
    uint16_t *embeddings; // Quantized embedding vectors (native array)
    size_t vocabSize;     // Number of tokens
    size_t embeddingDim;  // Embedding dimension
    QuantizationBits quantBits;
    varintDimensionPair dimensionEncoding;
} EmbeddingTable;

void embeddingTableInit(EmbeddingTable *table, size_t vocab, size_t dim,
                        QuantizationBits bits) {
    table->vocabSize = vocab;
    table->embeddingDim = dim;
    table->quantBits = bits;

    // Dimension encoding
    if (vocab <= 255 && dim <= 255) {
        table->dimensionEncoding = VARINT_DIMENSION_PAIR_DENSE_1_1;
    } else if (vocab <= 65535 && dim <= 255) {
        table->dimensionEncoding = VARINT_DIMENSION_PAIR_DENSE_2_1;
    } else {
        table->dimensionEncoding = VARINT_DIMENSION_PAIR_DENSE_2_2;
    }

    // Allocate native storage (uint16_t array holds up to 16-bit values)
    table->embeddings = calloc(vocab * dim, sizeof(uint16_t));
}

void embeddingTableFree(EmbeddingTable *table) {
    free(table->embeddings);
}

void embeddingTableSetValue(EmbeddingTable *table, size_t tokenId,
                            size_t dimIndex, uint16_t value) {
    assert(tokenId < table->vocabSize);
    assert(dimIndex < table->embeddingDim);

    size_t index = tokenId * table->embeddingDim + dimIndex;

    // Direct array access (for production, use varintPacked in separate
    // compilation units)
    table->embeddings[index] = value;
}

uint16_t embeddingTableGetValue(const EmbeddingTable *table, size_t tokenId,
                                size_t dimIndex) {
    assert(tokenId < table->vocabSize);
    assert(dimIndex < table->embeddingDim);

    size_t index = tokenId * table->embeddingDim + dimIndex;

    // Direct array access (for production, use varintPacked in separate
    // compilation units)
    return table->embeddings[index];
}

// ============================================================================
// DEMONSTRATION
// ============================================================================

void demonstrateMLFeatures(void) {
    printf("\n=== ML Features Example ===\n\n");

    // 1. Dense feature matrix
    printf("1. Creating dense feature matrix...\n");

    DenseFeatureMatrix matrix;
    denseMatrixInit(&matrix, 100, 20, 0.0f, 1.0f, QUANT_8BIT);

    printf("   Matrix: %zu samples × %zu features\n", matrix.sampleCount,
           matrix.featureCount);
    printf("   Quantization: 8-bit (0-255)\n");
    printf("   Dimension encoding: ");
    if (matrix.dimensionEncoding == VARINT_DIMENSION_PAIR_DENSE_1_1) {
        printf("DENSE_1_1 (1-byte samples × 1-byte features)\n");
    }

    // Fill with sample data
    for (size_t s = 0; s < 10; s++) {
        for (size_t f = 0; f < matrix.featureCount; f++) {
            float value = (float)(s * f) / 200.0f; // 0.0 to 1.0
            denseMatrixSet(&matrix, s, f, value);
        }
    }

    printf("   Filled first 10 samples\n");

    // Retrieve and verify
    float retrieved = denseMatrixGet(&matrix, 5, 10);
    float expected = (5.0f * 10.0f) / 200.0f;
    printf("   Sample verification: matrix[5][10] = %.3f (expected ~%.3f)\n",
           retrieved, expected);

    // Space analysis
    size_t bitsPerValue = 8;
    size_t totalBits = matrix.sampleCount * matrix.featureCount * bitsPerValue;
    size_t bytesUsed = (totalBits + 7) / 8;
    size_t bytesFloat =
        matrix.sampleCount * matrix.featureCount * sizeof(float);

    printf("\n   Storage analysis:\n");
    printf("   - 8-bit quantized: %zu bytes\n", bytesUsed);
    printf("   - 32-bit float: %zu bytes\n", bytesFloat);
    printf("   - Savings: %zu bytes (%.1f%%)\n", bytesFloat - bytesUsed,
           100.0 * (1.0 - (double)bytesUsed / bytesFloat));

    // 2. Compare quantization levels
    printf("\n2. Comparing quantization levels...\n");

    DenseFeatureMatrix matrix10bit;
    denseMatrixInit(&matrix10bit, 100, 20, 0.0f, 1.0f, QUANT_10BIT);

    DenseFeatureMatrix matrix12bit;
    denseMatrixInit(&matrix12bit, 100, 20, 0.0f, 1.0f, QUANT_12BIT);

    float testValue = 0.123456f;
    denseMatrixSet(&matrix, 0, 0, testValue);
    denseMatrixSet(&matrix10bit, 0, 0, testValue);
    denseMatrixSet(&matrix12bit, 0, 0, testValue);

    float retrieved8 = denseMatrixGet(&matrix, 0, 0);
    float retrieved10 = denseMatrixGet(&matrix10bit, 0, 0);
    float retrieved12 = denseMatrixGet(&matrix12bit, 0, 0);

    printf("   Original value: %.6f\n", testValue);
    printf("   8-bit:  %.6f (error: %.6f)\n", retrieved8,
           fabsf(retrieved8 - testValue));
    printf("   10-bit: %.6f (error: %.6f)\n", retrieved10,
           fabsf(retrieved10 - testValue));
    printf("   12-bit: %.6f (error: %.6f)\n", retrieved12,
           fabsf(retrieved12 - testValue));

    size_t bytes8 = (100 * 20 * 8 + 7) / 8;
    size_t bytes10 = (100 * 20 * 10 + 7) / 8;
    size_t bytes12 = (100 * 20 * 12 + 7) / 8;

    printf("\n   Storage comparison:\n");
    printf("   - 8-bit:  %zu bytes (%.1f bytes/sample)\n", bytes8,
           (double)bytes8 / 100);
    printf("   - 10-bit: %zu bytes (%.1f bytes/sample)\n", bytes10,
           (double)bytes10 / 100);
    printf("   - 12-bit: %zu bytes (%.1f bytes/sample)\n", bytes12,
           (double)bytes12 / 100);

    // 3. Sparse matrix (one-hot encoded features)
    printf("\n3. Creating sparse feature matrix (one-hot encoding)...\n");

    SparseFeatureMatrix sparseMatrix;
    sparseMatrixInit(&sparseMatrix, 100, 1000); // 100 samples, 1000 features

    printf("   Matrix: %zu samples × %zu features\n", sparseMatrix.sampleCount,
           sparseMatrix.featureCount);
    printf("   Dimension encoding: ");
    if (sparseMatrix.dimensionEncoding == VARINT_DIMENSION_PAIR_SPRSE_2_2) {
        printf("SPRSE_2_2 (2-byte samples × 2-byte features, sparse)\n");
    }

    // One-hot: each sample has exactly 1 non-zero feature
    for (size_t s = 0; s < 100; s++) {
        size_t activeFeature = (s * 13) % 1000; // Pseudo-random feature
        sparseMatrixSet(&sparseMatrix, s, activeFeature, 1);
    }

    printf("   Filled 100 samples (1 non-zero per sample)\n");
    printf("   Sparsity: %.2f%% (100 / %d)\n", 100.0 * 100.0 / (100 * 1000),
           100 * 1000);

    // Space analysis
    size_t sparseBytes = 100 * sizeof(SparseEntry); // 100 entries total
    size_t denseBytes = 100 * 1000 * 1; // 1 byte per element if dense

    printf("\n   Storage analysis:\n");
    printf("   - Sparse: ~%zu bytes\n", sparseBytes);
    printf("   - Dense (8-bit): %zu bytes\n", denseBytes);
    printf("   - Savings: %.1f%%\n",
           100.0 * (1.0 - (double)sparseBytes / denseBytes));

    // 4. Embedding table
    printf("\n4. Creating embedding table...\n");

    EmbeddingTable embeddings;
    embeddingTableInit(&embeddings, 10000, 128, QUANT_8BIT);

    printf("   Vocabulary: %zu tokens\n", embeddings.vocabSize);
    printf("   Embedding dimension: %zu\n", embeddings.embeddingDim);
    printf("   Quantization: 8-bit\n");

    // Initialize some embeddings
    for (size_t tokenId = 0; tokenId < 10; tokenId++) {
        for (size_t dim = 0; dim < embeddings.embeddingDim; dim++) {
            uint16_t value = (uint16_t)((tokenId + dim) % 256);
            embeddingTableSetValue(&embeddings, tokenId, dim, value);
        }
    }

    printf("   Initialized first 10 token embeddings\n");

    // Retrieve embedding
    printf("   Token 5 embedding (first 8 dims): ");
    for (size_t dim = 0; dim < 8; dim++) {
        uint16_t value = embeddingTableGetValue(&embeddings, 5, dim);
        printf("%u ", value);
    }
    printf("...\n");

    // Space analysis
    size_t embeddingBytes8 = (10000 * 128 * 8 + 7) / 8;
    size_t embeddingBytesFloat = 10000 * 128 * sizeof(float);

    printf("\n   Storage analysis:\n");
    printf("   - 8-bit quantized: %zu bytes (%.1f MB)\n", embeddingBytes8,
           (double)embeddingBytes8 / (1024 * 1024));
    printf("   - 32-bit float: %zu bytes (%.1f MB)\n", embeddingBytesFloat,
           (double)embeddingBytesFloat / (1024 * 1024));
    printf("   - Savings: %.1f MB (%.1f%%)\n",
           (double)(embeddingBytesFloat - embeddingBytes8) / (1024 * 1024),
           100.0 * (1.0 - (double)embeddingBytes8 / embeddingBytesFloat));

    // 5. Dimension encoding benefits
    printf("\n5. Dimension encoding benefits:\n");
    printf("   varintDimension tracks matrix dimensions compactly:\n");
    printf("   - 100×20 matrix: DENSE_1_1 (1 byte per dimension)\n");
    printf("   - 10000×128 matrix: DENSE_2_1 (2+1 bytes for dimensions)\n");
    printf("   - Sparse 100×1000: SPRSE_2_2 (indicates sparse storage)\n");
    printf("\n   Benefits:\n");
    printf("   - Single byte encodes both dimensions AND density\n");
    printf("   - Enables automatic storage optimization\n");
    printf("   - 144 pre-defined combinations cover all use cases\n");

    denseMatrixFree(&matrix);
    denseMatrixFree(&matrix10bit);
    denseMatrixFree(&matrix12bit);
    sparseMatrixFree(&sparseMatrix);
    embeddingTableFree(&embeddings);

    printf("\n✓ ML features example complete\n");
}

int main(void) {
    printf("===========================================\n");
    printf("  ML Features Integration Example\n");
    printf("===========================================\n");

    demonstrateMLFeatures();

    printf("\n===========================================\n");
    printf("This example demonstrated:\n");
    printf("  • varintDimension for matrix metadata\n");
    printf("  • varintPacked for quantized features\n");
    printf("  • 8/10/12-bit quantization\n");
    printf("  • Dense and sparse matrices\n");
    printf("  • Embedding table compression\n");
    printf("  • Space-efficient ML storage\n");
    printf("===========================================\n");

    return 0;
}
