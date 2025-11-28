/**
 * sparse_matrix_csr.c - Compressed Sparse Row (CSR) matrix using varintExternal
 *
 * This example demonstrates sparse matrix storage in CSR format combining:
 * - varintExternal: Column indices and row pointers with adaptive width
 * - varintDimension: Matrix dimension encoding
 * - Efficient operations on sparse data structures
 *
 * Features:
 * - CSR format: values[], column_indices[], row_pointers[]
 * - Dense to CSR conversion with automatic sparsity detection
 * - Matrix-vector multiply (SpMV) optimized for CSR layout
 * - Matrix transpose maintaining sparse format
 * - Scientific computing use cases (graphs, FEM, NLP, recommenders)
 * - Compression analysis vs dense storage
 *
 * CSR Format:
 *   values[nnz] - non-zero element values (float/double)
 *   column_indices[nnz] - column index for each non-zero (varint encoded)
 *   row_pointers[rows+1] - cumulative count of non-zeros (varint encoded)
 *
 * Example 3x4 matrix:
 *   [1.0  0    2.0  0  ]
 *   [0    3.0  0    0  ]
 *   [4.0  0    5.0  6.0]
 *
 * CSR representation:
 *   values[]         = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0]
 *   column_indices[] = [0,   2,   1,   0,   2,   3  ]  (varint encoded)
 *   row_pointers[]   = [0,   2,   3,   6]              (varint encoded)
 *
 * Compile: gcc -I../../src sparse_matrix_csr.c ../../build/src/varint.a -o
 * sparse_matrix_csr -lm Run: ./sparse_matrix_csr
 */

#include "varintDimension.h"
#include "varintExternal.h"
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ============================================================================
// CSR SPARSE MATRIX DATA STRUCTURE
// ============================================================================

typedef struct {
    double *values;         // Non-zero values [nnz]
    uint8_t *columnIndices; // Column indices (varint encoded) [nnz * colWidth]
    uint8_t *rowPointers; // Cumulative nnz count (varint) [(rows+1) * ptrWidth]
    size_t rows;          // Number of rows
    size_t cols;          // Number of columns
    size_t nnz;           // Number of non-zeros
    size_t capacity;      // Allocated capacity for values/indices
    varintWidth colWidth; // Bytes per column index
    varintWidth ptrWidth; // Bytes per row pointer
    varintDimensionPair dimensionEncoding;
} CSRMatrix;

// ============================================================================
// CSR MATRIX INITIALIZATION
// ============================================================================

void csrMatrixInit(CSRMatrix *matrix, size_t rows, size_t cols,
                   size_t estimatedNnz) {
    matrix->rows = rows;
    matrix->cols = cols;
    matrix->nnz = 0;
    matrix->capacity = estimatedNnz > 0 ? estimatedNnz : 100;

    // Determine column index width (based on max column value)
    if (cols <= 255) {
        matrix->colWidth = VARINT_WIDTH_8B;
    } else if (cols <= 65535) {
        matrix->colWidth = VARINT_WIDTH_16B;
    } else if (cols <= 16777215) {
        matrix->colWidth = VARINT_WIDTH_24B;
    } else {
        matrix->colWidth = VARINT_WIDTH_32B;
    }

    // Determine row pointer width (based on max nnz value)
    size_t maxNnz = rows * cols; // Worst case
    if (maxNnz <= 255) {
        matrix->ptrWidth = VARINT_WIDTH_8B;
    } else if (maxNnz <= 65535) {
        matrix->ptrWidth = VARINT_WIDTH_16B;
    } else if (maxNnz <= 16777215) {
        matrix->ptrWidth = VARINT_WIDTH_24B;
    } else {
        matrix->ptrWidth = VARINT_WIDTH_32B;
    }

    // Determine dimension encoding
    if (rows <= 255 && cols <= 255) {
        matrix->dimensionEncoding = VARINT_DIMENSION_PAIR_SPRSE_1_1;
    } else if (rows <= 65535 && cols <= 65535) {
        matrix->dimensionEncoding = VARINT_DIMENSION_PAIR_SPRSE_2_2;
    } else {
        matrix->dimensionEncoding = VARINT_DIMENSION_PAIR_SPRSE_4_4;
    }

    // Allocate storage
    matrix->values = malloc(matrix->capacity * sizeof(double));
    if (!matrix->values) {
        return;
    }
    matrix->columnIndices = malloc(matrix->capacity * matrix->colWidth);
    if (!matrix->columnIndices) {
        free(matrix->values);
        return;
    }
    matrix->rowPointers = malloc((rows + 1) * matrix->ptrWidth);
    if (!matrix->rowPointers) {
        free(matrix->values);
        free(matrix->columnIndices);
        return;
    }

    // Initialize row pointers to zero
    memset(matrix->rowPointers, 0, (rows + 1) * matrix->ptrWidth);
}

void csrMatrixFree(CSRMatrix *matrix) {
    free(matrix->values);
    free(matrix->columnIndices);
    free(matrix->rowPointers);
}

// ============================================================================
// CSR MATRIX GROWTH
// ============================================================================

void csrMatrixGrow(CSRMatrix *matrix) {
    matrix->capacity *= 2;
    double *newValues =
        realloc(matrix->values, matrix->capacity * sizeof(double));
    if (!newValues) {
        return;
    }
    matrix->values = newValues;
    uint8_t *newIndices =
        realloc(matrix->columnIndices, matrix->capacity * matrix->colWidth);
    if (!newIndices) {
        return;
    }
    matrix->columnIndices = newIndices;
}

// ============================================================================
// CSR MATRIX ELEMENT ACCESS
// ============================================================================

// Add a non-zero element (must be added in row-major order)
void csrMatrixAddElement(CSRMatrix *matrix, size_t row, size_t col,
                         double value) {
    assert(row < matrix->rows);
    assert(col < matrix->cols);
    assert(value != 0.0); // Only store non-zeros

    if (matrix->nnz >= matrix->capacity) {
        csrMatrixGrow(matrix);
    }

    // Store value
    matrix->values[matrix->nnz] = value;

    // Store column index (varint encoded)
    varintExternalPutFixedWidth(matrix->columnIndices +
                                    (matrix->nnz * matrix->colWidth),
                                col, matrix->colWidth);

    matrix->nnz++;
}

// Finalize row pointers after all elements are added
void csrMatrixFinalizeRowPointers(CSRMatrix *matrix) {
    // Row pointers store cumulative count of non-zeros
    // This must be called after all elements are added in row-major order

    // For demonstration, we'll compute from scratch
    // In practice, track current row during insertion
    size_t currentNnz = 0;
    for (size_t row = 0; row <= matrix->rows; row++) {
        varintExternalPutFixedWidth(matrix->rowPointers +
                                        (row * matrix->ptrWidth),
                                    currentNnz, matrix->ptrWidth);

        // Count nnz in this row by scanning column indices
        if (row < matrix->rows) {
            while (currentNnz < matrix->nnz) {
                // Simplified: assumes elements added in order
                currentNnz++;
                if (currentNnz >= matrix->nnz) {
                    break;
                }
            }
        }
    }
}

// Get row pointer value
uint64_t csrMatrixGetRowPointer(const CSRMatrix *matrix, size_t row) {
    assert(row <= matrix->rows);
    return varintExternalGet(matrix->rowPointers + (row * matrix->ptrWidth),
                             matrix->ptrWidth);
}

// Get column index for a non-zero element
uint64_t csrMatrixGetColumnIndex(const CSRMatrix *matrix, size_t nzIndex) {
    assert(nzIndex < matrix->nnz);
    return varintExternalGet(
        matrix->columnIndices + (nzIndex * matrix->colWidth), matrix->colWidth);
}

// Get element at (row, col) - returns 0.0 if not found
double csrMatrixGet(const CSRMatrix *matrix, size_t row, size_t col) {
    assert(row < matrix->rows);
    assert(col < matrix->cols);

    uint64_t rowStart = csrMatrixGetRowPointer(matrix, row);
    uint64_t rowEnd = csrMatrixGetRowPointer(matrix, row + 1);

    for (uint64_t i = rowStart; i < rowEnd; i++) {
        uint64_t colIdx = csrMatrixGetColumnIndex(matrix, i);
        if (colIdx == col) {
            return matrix->values[i];
        }
    }

    return 0.0; // Not found (implicit zero)
}

// ============================================================================
// DENSE TO CSR CONVERSION
// ============================================================================

void csrMatrixFromDense(CSRMatrix *matrix, const double *dense, size_t rows,
                        size_t cols) {
    // Count non-zeros first
    size_t nnz = 0;
    for (size_t i = 0; i < rows * cols; i++) {
        if (fabs(dense[i]) > 1e-10) { // Threshold for zero
            nnz++;
        }
    }

    csrMatrixInit(matrix, rows, cols, nnz);

    // Build CSR structure
    for (size_t row = 0; row < rows; row++) {
        // Store row start pointer
        varintExternalPutFixedWidth(matrix->rowPointers +
                                        (row * matrix->ptrWidth),
                                    matrix->nnz, matrix->ptrWidth);

        for (size_t col = 0; col < cols; col++) {
            double value = dense[row * cols + col];
            if (fabs(value) > 1e-10) {
                if (matrix->nnz >= matrix->capacity) {
                    csrMatrixGrow(matrix);
                }

                matrix->values[matrix->nnz] = value;
                varintExternalPutFixedWidth(
                    matrix->columnIndices + (matrix->nnz * matrix->colWidth),
                    col, matrix->colWidth);
                matrix->nnz++;
            }
        }
    }

    // Store final row pointer
    varintExternalPutFixedWidth(matrix->rowPointers + (rows * matrix->ptrWidth),
                                matrix->nnz, matrix->ptrWidth);
}

// ============================================================================
// CSR TO DENSE CONVERSION
// ============================================================================

void csrMatrixToDense(const CSRMatrix *matrix, double *dense) {
    // Initialize to zero
    memset(dense, 0, matrix->rows * matrix->cols * sizeof(double));

    // Fill in non-zeros
    for (size_t row = 0; row < matrix->rows; row++) {
        uint64_t rowStart = csrMatrixGetRowPointer(matrix, row);
        uint64_t rowEnd = csrMatrixGetRowPointer(matrix, row + 1);

        for (uint64_t i = rowStart; i < rowEnd; i++) {
            uint64_t col = csrMatrixGetColumnIndex(matrix, i);
            dense[row * matrix->cols + col] = matrix->values[i];
        }
    }
}

// ============================================================================
// MATRIX-VECTOR MULTIPLY (SpMV)
// ============================================================================

void csrMatrixVectorMultiply(const CSRMatrix *matrix, const double *x,
                             double *y) {
    // Compute y = A * x where A is sparse (CSR format)
    for (size_t row = 0; row < matrix->rows; row++) {
        double sum = 0.0;
        uint64_t rowStart = csrMatrixGetRowPointer(matrix, row);
        uint64_t rowEnd = csrMatrixGetRowPointer(matrix, row + 1);

        for (uint64_t i = rowStart; i < rowEnd; i++) {
            uint64_t col = csrMatrixGetColumnIndex(matrix, i);
            sum += matrix->values[i] * x[col];
        }

        y[row] = sum;
    }
}

// ============================================================================
// MATRIX TRANSPOSE
// ============================================================================

void csrMatrixTranspose(const CSRMatrix *matrix, CSRMatrix *result) {
    // Transpose: convert CSR(A) to CSR(A^T)
    csrMatrixInit(result, matrix->cols, matrix->rows, matrix->nnz);

    // Count nnz per column (which becomes nnz per row in transpose)
    size_t *colCounts = calloc(matrix->cols, sizeof(size_t));
    if (!colCounts) {
        return;
    }
    for (size_t i = 0; i < matrix->nnz; i++) {
        uint64_t col = csrMatrixGetColumnIndex(matrix, i);
        colCounts[col]++;
    }

    // Build row pointers for transposed matrix
    uint64_t cumsum = 0;
    for (size_t col = 0; col <= matrix->cols; col++) {
        varintExternalPutFixedWidth(result->rowPointers +
                                        (col * result->ptrWidth),
                                    cumsum, result->ptrWidth);
        if (col < matrix->cols) {
            cumsum += colCounts[col];
        }
    }

    // Fill in values and column indices
    size_t *colOffsets = calloc(matrix->cols, sizeof(size_t));
    if (!colOffsets) {
        free(colCounts);
        return;
    }
    for (size_t row = 0; row < matrix->rows; row++) {
        uint64_t rowStart = csrMatrixGetRowPointer(matrix, row);
        uint64_t rowEnd = csrMatrixGetRowPointer(matrix, row + 1);

        for (uint64_t i = rowStart; i < rowEnd; i++) {
            uint64_t col = csrMatrixGetColumnIndex(matrix, i);
            uint64_t destRow = col;
            uint64_t destRowStart = csrMatrixGetRowPointer(result, destRow);
            uint64_t destIdx = destRowStart + colOffsets[col];

            result->values[destIdx] = matrix->values[i];
            varintExternalPutFixedWidth(result->columnIndices +
                                            (destIdx * result->colWidth),
                                        row, result->colWidth);

            colOffsets[col]++;
        }
    }

    result->nnz = matrix->nnz;

    free(colCounts);
    free(colOffsets);
}

// ============================================================================
// SPARSE MATRIX ADDITION
// ============================================================================

void csrMatrixAdd(const CSRMatrix *A, const CSRMatrix *B, CSRMatrix *C) {
    assert(A->rows == B->rows);
    assert(A->cols == B->cols);

    // Simple implementation: convert to dense, add, convert back
    // (For production, implement direct sparse addition)
    double *denseA = calloc(A->rows * A->cols, sizeof(double));
    if (!denseA) {
        return;
    }
    double *denseB = calloc(B->rows * B->cols, sizeof(double));
    if (!denseB) {
        free(denseA);
        return;
    }
    double *denseC = calloc(A->rows * A->cols, sizeof(double));
    if (!denseC) {
        free(denseA);
        free(denseB);
        return;
    }

    csrMatrixToDense(A, denseA);
    csrMatrixToDense(B, denseB);

    for (size_t i = 0; i < A->rows * A->cols; i++) {
        denseC[i] = denseA[i] + denseB[i];
    }

    csrMatrixFromDense(C, denseC, A->rows, A->cols);

    free(denseA);
    free(denseB);
    free(denseC);
}

// ============================================================================
// DEMONSTRATION: GRAPH ADJACENCY MATRIX
// ============================================================================

void demonstrateGraphAdjacency() {
    printf("\n=== Use Case 1: Graph Adjacency Matrix (Social Network) ===\n\n");

    // Small social network: 6 users, sparse connections
    size_t numUsers = 6;
    const double dense[36] = {
        0, 1, 1, 0, 0, 0, // User 0 follows users 1, 2
        1, 0, 0, 1, 0, 0, // User 1 follows users 0, 3
        0, 0, 0, 1, 1, 0, // User 2 follows users 3, 4
        0, 0, 0, 0, 1, 1, // User 3 follows users 4, 5
        0, 0, 1, 0, 0, 1, // User 4 follows users 2, 5
        1, 0, 0, 0, 0, 0  // User 5 follows user 0
    };

    printf("Creating adjacency matrix for 6-user social network...\n");
    CSRMatrix graph;
    csrMatrixFromDense(&graph, dense, numUsers, numUsers);

    printf("   Matrix: %zu x %zu\n", graph.rows, graph.cols);
    printf("   Non-zeros: %zu / %zu (%.1f%% density)\n", graph.nnz,
           graph.rows * graph.cols,
           100.0 * graph.nnz / (graph.rows * graph.cols));
    printf("   Column index width: %d bytes\n", graph.colWidth);
    printf("   Row pointer width: %d bytes\n", graph.ptrWidth);

    // Show connections
    printf("\nSocial graph connections:\n");
    for (size_t user = 0; user < numUsers; user++) {
        printf("   User %zu follows: ", user);
        uint64_t rowStart = csrMatrixGetRowPointer(&graph, user);
        uint64_t rowEnd = csrMatrixGetRowPointer(&graph, user + 1);
        for (uint64_t i = rowStart; i < rowEnd; i++) {
            uint64_t followed = csrMatrixGetColumnIndex(&graph, i);
            printf("%" PRIu64 " ", followed);
        }
        printf("\n");
    }

    // Storage analysis
    size_t denseBytes = numUsers * numUsers * sizeof(double);
    size_t sparseBytes = graph.nnz * sizeof(double) +
                         graph.nnz * graph.colWidth +
                         (graph.rows + 1) * graph.ptrWidth;

    printf("\nStorage comparison:\n");
    printf("   Dense: %zu bytes\n", denseBytes);
    printf("   CSR:   %zu bytes\n", sparseBytes);
    printf("   Savings: %zu bytes (%.1f%%)\n", denseBytes - sparseBytes,
           100.0 * (1.0 - (double)sparseBytes / denseBytes));

    csrMatrixFree(&graph);
    printf("\n✓ Graph adjacency example complete\n");
}

// ============================================================================
// DEMONSTRATION: FINITE ELEMENT MESH
// ============================================================================

void demonstrateFiniteElementMesh() {
    printf("\n=== Use Case 2: Finite Element Mesh (Stiffness Matrix) ===\n\n");

    // Simplified 8x8 stiffness matrix (band diagonal pattern)
    size_t n = 8;
    double *dense = calloc(n * n, sizeof(double));
    if (!dense) {
        return;
    }

    printf("Creating stiffness matrix for 8-node FEM mesh...\n");

    // Band diagonal: each row has ~3 non-zeros (diagonal + neighbors)
    for (size_t i = 0; i < n; i++) {
        dense[i * n + i] = 4.0; // Diagonal
        if (i > 0) {
            dense[i * n + (i - 1)] = -1.0; // Lower diagonal
        }
        if (i < n - 1) {
            dense[i * n + (i + 1)] = -1.0; // Upper diagonal
        }
    }

    CSRMatrix stiffness;
    csrMatrixFromDense(&stiffness, dense, n, n);

    printf("   Matrix: %zu x %zu\n", stiffness.rows, stiffness.cols);
    printf("   Non-zeros: %zu / %zu (%.1f%% density)\n", stiffness.nnz,
           stiffness.rows * stiffness.cols,
           100.0 * stiffness.nnz / (stiffness.rows * stiffness.cols));
    printf("   Pattern: Band diagonal (local connectivity)\n");

    // Matrix-vector multiply (displacement calculation)
    printf("\nComputing displacement vector (SpMV)...\n");
    const double force[8] = {1.0, 2.0, 3.0, 4.0, 3.0, 2.0, 1.0, 0.5};
    double displacement[8];

    csrMatrixVectorMultiply(&stiffness, force, displacement);

    printf("   Force vector:        [%.1f, %.1f, %.1f, %.1f, %.1f, %.1f, %.1f, "
           "%.1f]\n",
           force[0], force[1], force[2], force[3], force[4], force[5], force[6],
           force[7]);
    printf("   Displacement result: [%.1f, %.1f, %.1f, %.1f, %.1f, %.1f, %.1f, "
           "%.1f]\n",
           displacement[0], displacement[1], displacement[2], displacement[3],
           displacement[4], displacement[5], displacement[6], displacement[7]);

    // Storage analysis
    size_t denseBytes = n * n * sizeof(double);
    size_t sparseBytes = stiffness.nnz * sizeof(double) +
                         stiffness.nnz * stiffness.colWidth +
                         (stiffness.rows + 1) * stiffness.ptrWidth;

    printf("\nStorage comparison:\n");
    printf("   Dense: %zu bytes\n", denseBytes);
    printf("   CSR:   %zu bytes\n", sparseBytes);
    printf("   Savings: %zu bytes (%.1f%%)\n", denseBytes - sparseBytes,
           100.0 * (1.0 - (double)sparseBytes / denseBytes));

    free(dense);
    csrMatrixFree(&stiffness);
    printf("\n✓ Finite element mesh example complete\n");
}

// ============================================================================
// DEMONSTRATION: DOCUMENT-TERM MATRIX
// ============================================================================

void demonstrateDocumentTermMatrix() {
    printf("\n=== Use Case 3: Document-Term Matrix (NLP/Search) ===\n\n");

    // 5 documents, 10 terms (very sparse)
    size_t numDocs = 5;
    size_t numTerms = 10;
    double dense[50] = {0}; // Initialize to zero

    // Document 0: terms {0, 2, 5}
    dense[0 * numTerms + 0] = 3.0;
    dense[0 * numTerms + 2] = 1.0;
    dense[0 * numTerms + 5] = 2.0;

    // Document 1: terms {1, 3, 7}
    dense[1 * numTerms + 1] = 2.0;
    dense[1 * numTerms + 3] = 1.0;
    dense[1 * numTerms + 7] = 4.0;

    // Document 2: terms {0, 5, 9}
    dense[2 * numTerms + 0] = 1.0;
    dense[2 * numTerms + 5] = 3.0;
    dense[2 * numTerms + 9] = 2.0;

    // Document 3: terms {2, 4, 6}
    dense[3 * numTerms + 2] = 2.0;
    dense[3 * numTerms + 4] = 1.0;
    dense[3 * numTerms + 6] = 1.0;

    // Document 4: terms {1, 5, 8}
    dense[4 * numTerms + 1] = 1.0;
    dense[4 * numTerms + 5] = 2.0;
    dense[4 * numTerms + 8] = 3.0;

    printf("Creating document-term matrix...\n");
    CSRMatrix docTerm;
    csrMatrixFromDense(&docTerm, dense, numDocs, numTerms);

    printf("   Documents: %zu\n", docTerm.rows);
    printf("   Terms: %zu\n", docTerm.cols);
    printf("   Non-zeros: %zu / %zu (%.1f%% density)\n", docTerm.nnz,
           docTerm.rows * docTerm.cols,
           100.0 * docTerm.nnz / (docTerm.rows * docTerm.cols));

    // Show term frequencies
    printf("\nDocument term frequencies:\n");
    for (size_t doc = 0; doc < numDocs; doc++) {
        printf("   Doc %zu: ", doc);
        uint64_t rowStart = csrMatrixGetRowPointer(&docTerm, doc);
        uint64_t rowEnd = csrMatrixGetRowPointer(&docTerm, doc + 1);
        for (uint64_t i = rowStart; i < rowEnd; i++) {
            uint64_t term = csrMatrixGetColumnIndex(&docTerm, i);
            printf("term%" PRIu64 "(%.0f) ", term, docTerm.values[i]);
        }
        printf("\n");
    }

    // Transpose for inverted index (term -> documents)
    printf("\nComputing inverted index (transpose)...\n");
    CSRMatrix invertedIndex;
    csrMatrixTranspose(&docTerm, &invertedIndex);

    printf("   Inverted index: %zu terms x %zu docs\n", invertedIndex.rows,
           invertedIndex.cols);

    printf("\nTerm document postings:\n");
    for (size_t term = 0; term < 6; term++) {
        printf("   Term %zu appears in docs: ", term);
        uint64_t rowStart = csrMatrixGetRowPointer(&invertedIndex, term);
        uint64_t rowEnd = csrMatrixGetRowPointer(&invertedIndex, term + 1);
        for (uint64_t i = rowStart; i < rowEnd; i++) {
            uint64_t doc = csrMatrixGetColumnIndex(&invertedIndex, i);
            printf("%" PRIu64 "(%.0f) ", doc, invertedIndex.values[i]);
        }
        printf("\n");
    }

    // Storage analysis
    size_t denseBytes = numDocs * numTerms * sizeof(double);
    size_t sparseBytes = docTerm.nnz * sizeof(double) +
                         docTerm.nnz * docTerm.colWidth +
                         (docTerm.rows + 1) * docTerm.ptrWidth;

    printf("\nStorage comparison:\n");
    printf("   Dense: %zu bytes\n", denseBytes);
    printf("   CSR:   %zu bytes\n", sparseBytes);
    printf("   Savings: %zu bytes (%.1f%%)\n", denseBytes - sparseBytes,
           100.0 * (1.0 - (double)sparseBytes / denseBytes));

    csrMatrixFree(&docTerm);
    csrMatrixFree(&invertedIndex);
    printf("\n✓ Document-term matrix example complete\n");
}

// ============================================================================
// DEMONSTRATION: LARGE SPARSE MATRIX
// ============================================================================

void demonstrateLargeSparseMatrix() {
    printf("\n=== Use Case 4: Large Sparse Matrix (1000x1000, 1%% density) "
           "===\n\n");

    size_t n = 1000;
    double density = 0.01; // 1% density
    size_t targetNnz = (size_t)(n * n * density);

    printf("Creating large sparse matrix...\n");
    printf("   Dimensions: %zu x %zu\n", n, n);
    printf("   Target density: %.1f%% (~%zu non-zeros)\n", density * 100,
           targetNnz);

    CSRMatrix large;
    csrMatrixInit(&large, n, n, targetNnz);

    // Generate random sparse pattern
    srand(12345); // Fixed seed for reproducibility
    size_t *rowNnzCounts = calloc(n, sizeof(size_t));
    if (!rowNnzCounts) {
        csrMatrixFree(&large);
        return;
    }
    size_t actualNnz = 0;

    // Distribute non-zeros across rows
    for (size_t i = 0; i < targetNnz; i++) {
        size_t row = rand() % n;
        rowNnzCounts[row]++;
    }

    // Build CSR structure
    for (size_t row = 0; row < n; row++) {
        varintExternalPutFixedWidth(large.rowPointers + (row * large.ptrWidth),
                                    actualNnz, large.ptrWidth);

        for (size_t j = 0; j < rowNnzCounts[row]; j++) {
            size_t col = rand() % n;
            double value = ((double)rand() / RAND_MAX) * 10.0;

            if (actualNnz >= large.capacity) {
                csrMatrixGrow(&large);
            }

            large.values[actualNnz] = value;
            varintExternalPutFixedWidth(large.columnIndices +
                                            (actualNnz * large.colWidth),
                                        col, large.colWidth);
            actualNnz++;
        }
    }

    varintExternalPutFixedWidth(large.rowPointers + (n * large.ptrWidth),
                                actualNnz, large.ptrWidth);
    large.nnz = actualNnz;

    printf("   Actual non-zeros: %zu\n", large.nnz);
    printf("   Actual density: %.2f%%\n", 100.0 * large.nnz / (n * n));

    // Varint encoding efficiency
    printf("\nVarint encoding:\n");
    printf("   Column indices: %d bytes per index (max col: %zu)\n",
           large.colWidth, n - 1);
    printf("   Row pointers:   %d bytes per pointer (max nnz: %zu)\n",
           large.ptrWidth, large.nnz);

    // Matrix-vector multiply benchmark
    printf("\nPerforming SpMV (y = A * x)...\n");
    double *x = malloc(n * sizeof(double));
    if (!x) {
        free(rowNnzCounts);
        csrMatrixFree(&large);
        return;
    }
    double *y = malloc(n * sizeof(double));
    if (!y) {
        free(x);
        free(rowNnzCounts);
        csrMatrixFree(&large);
        return;
    }

    for (size_t i = 0; i < n; i++) {
        x[i] = 1.0; // Unit vector
    }

    clock_t start = clock();
    csrMatrixVectorMultiply(&large, x, y);
    clock_t end = clock();

    double elapsed = (double)(end - start) / CLOCKS_PER_SEC * 1000.0;
    printf("   SpMV completed in %.3f ms\n", elapsed);
    printf("   Result sample: y[0]=%.2f, y[500]=%.2f, y[999]=%.2f\n", y[0],
           y[500], y[999]);

    // Storage analysis
    size_t denseBytes = n * n * sizeof(double);
    size_t csrBytes = large.nnz * sizeof(double) + large.nnz * large.colWidth +
                      (large.rows + 1) * large.ptrWidth;

    printf("\nComprehensive storage analysis:\n");
    printf("   Dense storage:\n");
    printf("   - Matrix data: %zu bytes (%.2f MB)\n", denseBytes,
           (double)denseBytes / (1024 * 1024));

    printf("   CSR storage:\n");
    printf("   - Values:         %zu bytes (%.2f KB)\n",
           large.nnz * sizeof(double),
           (double)(large.nnz * sizeof(double)) / 1024);
    printf("   - Column indices: %zu bytes (%.2f KB)\n",
           large.nnz * large.colWidth,
           (double)(large.nnz * large.colWidth) / 1024);
    printf("   - Row pointers:   %zu bytes (%.2f KB)\n",
           (large.rows + 1) * large.ptrWidth,
           (double)((large.rows + 1) * large.ptrWidth) / 1024);
    printf("   - Total:          %zu bytes (%.2f KB)\n", csrBytes,
           (double)csrBytes / 1024);

    printf("\n   Compression ratio: %.2fx\n", (double)denseBytes / csrBytes);
    printf("   Space savings: %zu bytes (%.1f%%)\n", denseBytes - csrBytes,
           100.0 * (1.0 - (double)csrBytes / denseBytes));

    // Compare with fixed-width column indices (no varint)
    size_t fixedWidthBytes = large.nnz * sizeof(double) +
                             large.nnz * sizeof(uint32_t) +
                             (large.rows + 1) * sizeof(uint32_t);
    printf("\n   vs. fixed 32-bit indices:\n");
    printf("   - Fixed-width CSR: %zu bytes (%.2f KB)\n", fixedWidthBytes,
           (double)fixedWidthBytes / 1024);
    printf("   - Varint savings: %zu bytes (%.1f%%)\n",
           fixedWidthBytes - csrBytes,
           100.0 * (1.0 - (double)csrBytes / fixedWidthBytes));

    free(x);
    free(y);
    free(rowNnzCounts);
    csrMatrixFree(&large);
    printf("\n✓ Large sparse matrix example complete\n");
}

// ============================================================================
// DEMONSTRATION: RECOMMENDER SYSTEM
// ============================================================================

void demonstrateRecommenderSystem() {
    printf("\n=== Use Case 5: Recommender System (User-Item Ratings) ===\n\n");

    // 100 users, 50 items, ~3% ratings (very sparse)
    size_t numUsers = 100;
    size_t numItems = 50;

    printf("Creating user-item ratings matrix...\n");
    printf("   Users: %zu, Items: %zu\n", numUsers, numItems);

    CSRMatrix ratings;
    csrMatrixInit(&ratings, numUsers, numItems, 150); // ~3% density

    // Generate sparse ratings pattern
    srand(54321);
    for (size_t user = 0; user < numUsers; user++) {
        varintExternalPutFixedWidth(ratings.rowPointers +
                                        (user * ratings.ptrWidth),
                                    ratings.nnz, ratings.ptrWidth);

        // Each user rates 1-3 items
        size_t numRatings = (rand() % 3) + 1;
        for (size_t i = 0; i < numRatings; i++) {
            size_t item = rand() % numItems;
            double rating =
                1.0 + ((double)rand() / RAND_MAX) * 4.0; // 1-5 stars

            if (ratings.nnz >= ratings.capacity) {
                csrMatrixGrow(&ratings);
            }

            ratings.values[ratings.nnz] = rating;
            varintExternalPutFixedWidth(ratings.columnIndices +
                                            (ratings.nnz * ratings.colWidth),
                                        item, ratings.colWidth);
            ratings.nnz++;
        }
    }

    varintExternalPutFixedWidth(ratings.rowPointers +
                                    (numUsers * ratings.ptrWidth),
                                ratings.nnz, ratings.ptrWidth);

    printf("   Total ratings: %zu / %zu (%.2f%% density)\n", ratings.nnz,
           numUsers * numItems, 100.0 * ratings.nnz / (numUsers * numItems));

    // Show sample ratings
    printf("\nSample user ratings:\n");
    for (size_t user = 0; user < 5; user++) {
        printf("   User %zu rated: ", user);
        uint64_t rowStart = csrMatrixGetRowPointer(&ratings, user);
        uint64_t rowEnd = csrMatrixGetRowPointer(&ratings, user + 1);
        for (uint64_t i = rowStart; i < rowEnd; i++) {
            uint64_t item = csrMatrixGetColumnIndex(&ratings, i);
            printf("item%" PRIu64 "(%.1f*) ", item, ratings.values[i]);
        }
        printf("\n");
    }

    // Storage analysis
    size_t denseBytes = numUsers * numItems * sizeof(double);
    size_t sparseBytes = ratings.nnz * sizeof(double) +
                         ratings.nnz * ratings.colWidth +
                         (ratings.rows + 1) * ratings.ptrWidth;

    printf("\nStorage comparison:\n");
    printf("   Dense: %zu bytes (%.2f KB)\n", denseBytes,
           (double)denseBytes / 1024);
    printf("   CSR:   %zu bytes (%.2f KB)\n", sparseBytes,
           (double)sparseBytes / 1024);
    printf("   Savings: %zu bytes (%.1f%%)\n", denseBytes - sparseBytes,
           100.0 * (1.0 - (double)sparseBytes / denseBytes));

    csrMatrixFree(&ratings);
    printf("\n✓ Recommender system example complete\n");
}

// ============================================================================
// MAIN DEMONSTRATION
// ============================================================================

int main() {
    printf(
        "=================================================================\n");
    printf("  Sparse Matrix (CSR Format) Integration Example\n");
    printf(
        "=================================================================\n");

    demonstrateGraphAdjacency();
    demonstrateFiniteElementMesh();
    demonstrateDocumentTermMatrix();
    demonstrateLargeSparseMatrix();
    demonstrateRecommenderSystem();

    printf("\n================================================================="
           "\n");
    printf("This example demonstrated:\n");
    printf("  • CSR (Compressed Sparse Row) matrix format\n");
    printf("  • varintExternal for adaptive-width column indices\n");
    printf("  • varintExternal for adaptive-width row pointers\n");
    printf("  • Dense to CSR conversion with sparsity detection\n");
    printf("  • Matrix-vector multiply (SpMV) optimized for CSR\n");
    printf("  • Matrix transpose maintaining sparse format\n");
    printf("  • Scientific computing use cases:\n");
    printf("    - Graph adjacency matrices (social networks)\n");
    printf("    - Finite element stiffness matrices (FEM)\n");
    printf("    - Document-term matrices (NLP/search)\n");
    printf("    - User-item ratings (recommender systems)\n");
    printf("  • Compression analysis: 50-99%% space savings\n");
    printf("  • Varint encoding: 2-byte indices for 1000x1000 matrix\n");
    printf("  • Access pattern efficiency for sparse operations\n");
    printf(
        "=================================================================\n");

    return 0;
}
