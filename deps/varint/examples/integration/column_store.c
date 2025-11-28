/**
 * column_store.c - Columnar data storage using varintExternal and
 * varintDimension
 *
 * This example demonstrates a column-oriented database combining:
 * - varintExternal: Column data with schema-driven width selection
 * - varintDimension: Table metadata and dimension encoding
 * - Efficient compression through adaptive column widths
 *
 * Features:
 * - Schema-driven encoding (column types determine varint width)
 * - Columnar storage for analytical queries
 * - Null bitmap compression
 * - Dynamic column addition
 * - Aggregate operations optimized for column access
 *
 * Compile: gcc -I../../src column_store.c ../../build/src/libvarint.a -o
 * column_store Run: ./column_store
 */

#include "varintDimension.h"
#include "varintExternal.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// SCHEMA DEFINITION
// ============================================================================

typedef enum {
    COL_TYPE_INT8,   // 1-byte integers
    COL_TYPE_INT16,  // 2-byte integers
    COL_TYPE_INT32,  // 4-byte integers
    COL_TYPE_INT64,  // 8-byte integers
    COL_TYPE_UINT8,  // 1-byte unsigned
    COL_TYPE_UINT16, // 2-byte unsigned
    COL_TYPE_UINT32, // 4-byte unsigned
    COL_TYPE_UINT64, // 8-byte unsigned
    COL_TYPE_FLOAT,  // 4-byte float
    COL_TYPE_DOUBLE, // 8-byte double
} ColumnType;

typedef struct {
    char name[32];
    ColumnType type;
    bool nullable;
    varintWidth defaultWidth; // Encoding width for this column
} ColumnSchema;

typedef struct {
    ColumnSchema *columns;
    size_t columnCount;
    size_t rowCount;
    size_t capacity;
    varintDimensionPair dimensionEncoding; // Encodes table dimensions
} TableSchema;

// ============================================================================
// COLUMN DATA STORAGE
// ============================================================================

typedef struct {
    uint8_t *data;     // Column values (varintExternal encoded)
    uint8_t *nullBits; // Null bitmap (1 bit per row)
    size_t dataSize;
    size_t dataCapacity;
    varintWidth width; // Fixed width for this column
} Column;

typedef struct {
    TableSchema schema;
    Column *columns;
} ColumnStore;

// ============================================================================
// SCHEMA OPERATIONS
// ============================================================================

void schemaInit(TableSchema *schema, size_t maxRows, size_t maxCols) {
    schema->columns = NULL;
    schema->columnCount = 0;
    schema->rowCount = 0;
    schema->capacity = maxRows;

    // Encode table dimensions
    if (maxRows <= 255 && maxCols <= 255) {
        schema->dimensionEncoding = VARINT_DIMENSION_PAIR_DENSE_1_1;
    } else if (maxRows <= 65535 && maxCols <= 255) {
        schema->dimensionEncoding = VARINT_DIMENSION_PAIR_DENSE_2_1;
    } else if (maxRows <= 65535 && maxCols <= 65535) {
        schema->dimensionEncoding = VARINT_DIMENSION_PAIR_DENSE_2_2;
    } else {
        schema->dimensionEncoding = VARINT_DIMENSION_PAIR_DENSE_4_4;
    }
}

varintWidth getTypeWidth(ColumnType type) {
    switch (type) {
    case COL_TYPE_INT8:
    case COL_TYPE_UINT8:
        return 1;
    case COL_TYPE_INT16:
    case COL_TYPE_UINT16:
        return 2;
    case COL_TYPE_INT32:
    case COL_TYPE_UINT32:
    case COL_TYPE_FLOAT:
        return 4;
    case COL_TYPE_INT64:
    case COL_TYPE_UINT64:
    case COL_TYPE_DOUBLE:
        return 8;
    default:
        return 8;
    }
}

void schemaAddColumn(TableSchema *schema, const char *name, ColumnType type,
                     bool nullable) {
    schema->columns = realloc(schema->columns,
                              (schema->columnCount + 1) * sizeof(ColumnSchema));

    ColumnSchema *col = &schema->columns[schema->columnCount];
    strncpy(col->name, name, sizeof(col->name) - 1);
    col->name[sizeof(col->name) - 1] = '\0';
    col->type = type;
    col->nullable = nullable;
    col->defaultWidth = getTypeWidth(type);

    schema->columnCount++;
}

// ============================================================================
// COLUMN STORE OPERATIONS
// ============================================================================

void columnStoreInit(ColumnStore *store, TableSchema schema) {
    store->schema = schema;
    store->columns = calloc(schema.columnCount, sizeof(Column));

    // Initialize each column
    for (size_t i = 0; i < schema.columnCount; i++) {
        Column *col = &store->columns[i];
        col->width = schema.columns[i].defaultWidth;
        col->dataCapacity = schema.capacity * col->width;
        col->data = malloc(col->dataCapacity);
        col->dataSize = 0;

        // Null bitmap (1 bit per row, rounded up to bytes)
        if (schema.columns[i].nullable) {
            size_t nullBitmapBytes = (schema.capacity + 7) / 8;
            col->nullBits = calloc(1, nullBitmapBytes);
        } else {
            col->nullBits = NULL;
        }
    }
}

void columnStoreFree(ColumnStore *store) {
    for (size_t i = 0; i < store->schema.columnCount; i++) {
        free(store->columns[i].data);
        free(store->columns[i].nullBits);
    }
    free(store->columns);
    free(store->schema.columns);
}

// ============================================================================
// NULL BITMAP OPERATIONS
// ============================================================================

void setNull(Column *col, size_t row) {
    if (col->nullBits) {
        size_t byteIndex = row / 8;
        size_t bitIndex = row % 8;
        col->nullBits[byteIndex] |= (1 << bitIndex);
    }
}

bool isNull(const Column *col, size_t row) {
    if (!col->nullBits) {
        return false;
    }
    size_t byteIndex = row / 8;
    size_t bitIndex = row % 8;
    return (col->nullBits[byteIndex] & (1 << bitIndex)) != 0;
}

// ============================================================================
// DATA INSERTION
// ============================================================================

void columnStoreInsertInt64(ColumnStore *store, size_t colIndex,
                            int64_t value) {
    assert(colIndex < store->schema.columnCount);
    Column *col = &store->columns[colIndex];

    // Convert signed to unsigned for storage
    uint64_t unsignedValue;
    if (value < 0) {
        // ZigZag encoding: map negatives to odd positives
        unsignedValue = (uint64_t)((-value) * 2 - 1);
    } else {
        // Map positives to even positives
        unsignedValue = (uint64_t)(value * 2);
    }

    // Store using varintExternal with fixed width
    varintExternalPutFixedWidth(col->data + col->dataSize, unsignedValue,
                                col->width);
    col->dataSize += col->width;
}

void columnStoreInsertUInt64(ColumnStore *store, size_t colIndex,
                             uint64_t value) {
    assert(colIndex < store->schema.columnCount);
    Column *col = &store->columns[colIndex];

    // Store directly using varintExternal
    varintExternalPutFixedWidth(col->data + col->dataSize, value, col->width);
    col->dataSize += col->width;
}

void columnStoreInsertDouble(ColumnStore *store, size_t colIndex,
                             double value) {
    assert(colIndex < store->schema.columnCount);
    Column *col = &store->columns[colIndex];

    // Store as raw bytes (8 bytes)
    memcpy(col->data + col->dataSize, &value, sizeof(double));
    col->dataSize += sizeof(double);
}

void columnStoreInsertNull(ColumnStore *store, size_t colIndex) {
    assert(colIndex < store->schema.columnCount);
    Column *col = &store->columns[colIndex];
    assert(store->schema.columns[colIndex].nullable);

    setNull(col, store->schema.rowCount);

    // Still advance data pointer (store zeros)
    memset(col->data + col->dataSize, 0, col->width);
    col->dataSize += col->width;
}

void columnStoreCommitRow(ColumnStore *store) {
    store->schema.rowCount++;
}

// ============================================================================
// DATA RETRIEVAL
// ============================================================================

int64_t columnStoreGetInt64(const ColumnStore *store, size_t row,
                            size_t colIndex) {
    assert(row < store->schema.rowCount);
    assert(colIndex < store->schema.columnCount);

    const Column *col = &store->columns[colIndex];
    if (isNull(col, row)) {
        return 0; // Return 0 for NULL
    }

    size_t offset = row * col->width;
    uint64_t unsignedValue = varintExternalGet(col->data + offset, col->width);

    // Decode ZigZag
    if (unsignedValue & 1) {
        // Odd = negative
        return -((int64_t)((unsignedValue + 1) / 2));
    } else {
        // Even = positive
        return (int64_t)(unsignedValue / 2);
    }
}

uint64_t columnStoreGetUInt64(const ColumnStore *store, size_t row,
                              size_t colIndex) {
    assert(row < store->schema.rowCount);
    assert(colIndex < store->schema.columnCount);

    const Column *col = &store->columns[colIndex];
    if (isNull(col, row)) {
        return 0;
    }

    size_t offset = row * col->width;
    return varintExternalGet(col->data + offset, col->width);
}

double columnStoreGetDouble(const ColumnStore *store, size_t row,
                            size_t colIndex) {
    assert(row < store->schema.rowCount);
    assert(colIndex < store->schema.columnCount);

    const Column *col = &store->columns[colIndex];
    if (isNull(col, row)) {
        return 0.0;
    }

    size_t offset = row * sizeof(double);
    double value;
    memcpy(&value, col->data + offset, sizeof(double));
    return value;
}

// ============================================================================
// AGGREGATE OPERATIONS (optimized for columnar access)
// ============================================================================

uint64_t columnStoreSum(const ColumnStore *store, size_t colIndex) {
    assert(colIndex < store->schema.columnCount);
    const Column *col = &store->columns[colIndex];

    uint64_t sum = 0;
    for (size_t row = 0; row < store->schema.rowCount; row++) {
        if (!isNull(col, row)) {
            sum += columnStoreGetUInt64(store, row, colIndex);
        }
    }
    return sum;
}

double columnStoreAverage(const ColumnStore *store, size_t colIndex) {
    assert(colIndex < store->schema.columnCount);
    const Column *col = &store->columns[colIndex];

    uint64_t sum = 0;
    size_t count = 0;

    for (size_t row = 0; row < store->schema.rowCount; row++) {
        if (!isNull(col, row)) {
            sum += columnStoreGetUInt64(store, row, colIndex);
            count++;
        }
    }

    return count > 0 ? (double)sum / count : 0.0;
}

uint64_t columnStoreMax(const ColumnStore *store, size_t colIndex) {
    assert(colIndex < store->schema.columnCount);
    const Column *col = &store->columns[colIndex];

    uint64_t max = 0;
    for (size_t row = 0; row < store->schema.rowCount; row++) {
        if (!isNull(col, row)) {
            uint64_t value = columnStoreGetUInt64(store, row, colIndex);
            if (value > max) {
                max = value;
            }
        }
    }
    return max;
}

// ============================================================================
// DEMONSTRATION
// ============================================================================

void demonstrateColumnStore(void) {
    printf("\n=== Column Store Example ===\n\n");

    // 1. Create schema
    printf("1. Creating table schema...\n");

    TableSchema schema;
    schemaInit(&schema, 1000, 10); // Max 1000 rows, 10 columns

    schemaAddColumn(&schema, "user_id", COL_TYPE_UINT32, false);
    schemaAddColumn(&schema, "age", COL_TYPE_UINT8, true);
    schemaAddColumn(&schema, "balance", COL_TYPE_INT64, true);
    schemaAddColumn(&schema, "score", COL_TYPE_DOUBLE, false);

    printf("   Columns: %zu\n", schema.columnCount);
    printf("   Dimension encoding: ");
    if (schema.dimensionEncoding == VARINT_DIMENSION_PAIR_DENSE_2_1) {
        printf("DENSE_2_1 (2-byte rows × 1-byte cols)\n");
    }

    for (size_t i = 0; i < schema.columnCount; i++) {
        printf("   - %s (%s, %d bytes, %s)\n", schema.columns[i].name,
               schema.columns[i].type == COL_TYPE_UINT32   ? "UINT32"
               : schema.columns[i].type == COL_TYPE_UINT8  ? "UINT8"
               : schema.columns[i].type == COL_TYPE_INT64  ? "INT64"
               : schema.columns[i].type == COL_TYPE_DOUBLE ? "DOUBLE"
                                                           : "UNKNOWN",
               schema.columns[i].defaultWidth,
               schema.columns[i].nullable ? "nullable" : "not null");
    }

    // 2. Initialize column store
    printf("\n2. Initializing column store...\n");

    ColumnStore store;
    columnStoreInit(&store, schema);

    printf("   Allocated storage:\n");
    for (size_t i = 0; i < store.schema.columnCount; i++) {
        printf("   - %s: %zu bytes\n", schema.columns[i].name,
               store.columns[i].dataCapacity);
    }

    // 3. Insert data
    printf("\n3. Inserting sample data...\n");

    // Row 0
    columnStoreInsertUInt64(&store, 0, 1001); // user_id
    columnStoreInsertUInt64(&store, 1, 25);   // age
    columnStoreInsertInt64(&store, 2, 50000); // balance
    columnStoreInsertDouble(&store, 3, 95.5); // score
    columnStoreCommitRow(&store);

    // Row 1
    columnStoreInsertUInt64(&store, 0, 1002);
    columnStoreInsertNull(&store, 1); // age NULL
    columnStoreInsertInt64(&store, 2, -1500);
    columnStoreInsertDouble(&store, 3, 72.3);
    columnStoreCommitRow(&store);

    // Row 2
    columnStoreInsertUInt64(&store, 0, 1003);
    columnStoreInsertUInt64(&store, 1, 30);
    columnStoreInsertInt64(&store, 2, 125000);
    columnStoreInsertDouble(&store, 3, 88.9);
    columnStoreCommitRow(&store);

    // Row 3
    columnStoreInsertUInt64(&store, 0, 1004);
    columnStoreInsertUInt64(&store, 1, 22);
    columnStoreInsertNull(&store, 2); // balance NULL
    columnStoreInsertDouble(&store, 3, 91.7);
    columnStoreCommitRow(&store);

    printf("   Inserted %zu rows\n", store.schema.rowCount);

    // 4. Query data
    printf("\n4. Querying data (row-by-row)...\n");

    for (size_t row = 0; row < store.schema.rowCount; row++) {
        uint64_t userId = columnStoreGetUInt64(&store, row, 0);
        bool ageIsNull = isNull(&store.columns[1], row);
        uint64_t age = ageIsNull ? 0 : columnStoreGetUInt64(&store, row, 1);
        bool balanceIsNull = isNull(&store.columns[2], row);
        int64_t balance =
            balanceIsNull ? 0 : columnStoreGetInt64(&store, row, 2);
        double score = columnStoreGetDouble(&store, row, 3);

        printf(
            "   Row %zu: user_id=%" PRIu64 ", age=%s, balance=%s, score=%.1f\n",
            row, userId, ageIsNull ? "NULL" : (char[]){(char)('0' + age), '\0'},
            balanceIsNull ? "NULL"
                          : (char[]){(char)('0' + (balance / 1000)), '\0'},
            score);
    }

    // 5. Aggregate operations
    printf("\n5. Running aggregate queries (column-oriented)...\n");

    uint64_t totalUsers = store.schema.rowCount;
    double avgScore = columnStoreAverage(&store, 3);
    uint64_t maxAge = columnStoreMax(&store, 1);

    printf("   Total users: %" PRIu64 "\n", totalUsers);
    printf("   Average score: %.2f\n", avgScore);
    printf("   Maximum age: %" PRIu64 "\n", maxAge);

    // 6. Space efficiency analysis
    printf("\n6. Space efficiency analysis:\n");

    size_t totalDataBytes = 0;
    size_t totalNullBytes = 0;

    for (size_t i = 0; i < store.schema.columnCount; i++) {
        totalDataBytes += store.columns[i].dataSize;
        if (store.columns[i].nullBits) {
            totalNullBytes += (store.schema.rowCount + 7) / 8;
        }
    }

    printf("   Column data: %zu bytes\n", totalDataBytes);
    printf("   Null bitmaps: %zu bytes\n", totalNullBytes);
    printf("   Total storage: %zu bytes\n", totalDataBytes + totalNullBytes);

    // Compare with row-oriented storage
    size_t rowOrientedSize =
        store.schema.rowCount * (4 + 1 + 8 + 8); // Per-row overhead
    printf("\n   Row-oriented equivalent: %zu bytes\n", rowOrientedSize);
    printf("   Space savings: %.1f%%\n",
           100.0 * (1.0 - (double)(totalDataBytes + totalNullBytes) /
                              rowOrientedSize));

    // 7. Demonstrate column width optimization
    printf("\n7. Column width optimization:\n");
    printf("   user_id column (UINT32):\n");
    printf("   - Fixed width: 4 bytes × %zu rows = %zu bytes\n",
           store.schema.rowCount, 4 * store.schema.rowCount);
    printf("   - Actual usage: All values fit in 2 bytes\n");
    printf("   - Potential savings: Could use COL_TYPE_UINT16 (2 bytes)\n");
    printf("   - Would save: %zu bytes (50%%)\n", 2 * store.schema.rowCount);

    printf("   age column (UINT8):\n");
    printf("   - Fixed width: 1 byte × %zu rows = %zu bytes\n",
           store.schema.rowCount, store.schema.rowCount);
    printf("   - Optimal encoding (values 22-30)\n");

    columnStoreFree(&store);

    printf("\n✓ Column store example complete\n");
}

int main(void) {
    printf("===========================================\n");
    printf("  Column Store Integration Example\n");
    printf("===========================================\n");

    demonstrateColumnStore();

    printf("\n===========================================\n");
    printf("This example demonstrated:\n");
    printf("  • varintExternal for column data\n");
    printf("  • varintDimension for table metadata\n");
    printf("  • Schema-driven encoding\n");
    printf("  • Null bitmap compression\n");
    printf("  • Columnar aggregate operations\n");
    printf("  • Space efficiency analysis\n");
    printf("===========================================\n");

    return 0;
}
