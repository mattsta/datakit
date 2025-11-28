/**
 * database_system.c - Complete database system using multiple varint types
 *
 * This example demonstrates a small database system that combines:
 * - varintTagged: Sortable keys for B-tree indexes
 * - varintExternal: Space-efficient values with schema
 * - varintPacked: Compact integer indexes
 *
 * Features:
 * - Table with typed columns
 * - B-tree-ready sortable keys
 * - Schema-driven encoding
 * - Memory-efficient storage
 *
 * Compile: gcc -I../src database_system.c ../build/src/libvarint.a -o
 * database_system Run: ./database_system
 */

#include "varintExternal.h"
#include "varintTagged.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Generate 13-bit packed array for indexes
#define PACK_STORAGE_BITS 13
#include "varintPacked.h"

// ============================================================================
// DATABASE SCHEMA
// ============================================================================

typedef enum {
    COL_USER_ID,   // Primary key: uint64_t (varintTagged)
    COL_AGE,       // uint8_t
    COL_SCORE,     // uint32_t
    COL_TIMESTAMP, // uint64_t (40-bit unix time)
    COL_COUNT
} ColumnType;

typedef struct {
    const char *name;
    varintWidth maxWidth; // Maximum bytes for this column type
} ColumnSchema;

static const ColumnSchema SCHEMA[COL_COUNT] = {
    {"user_id",
     8},          // varintTagged key (up to 9 bytes, but we use external width)
    {"age", 1},   // Always 1 byte
    {"score", 4}, // Up to 4 bytes
    {"timestamp", 5}, // 40-bit timestamp (5 bytes)
};

// ============================================================================
// TABLE STRUCTURE
// ============================================================================

typedef struct {
    // Primary key (varintTagged for sortability)
    uint8_t *keys;      // Array of variable-length tagged varints
    size_t *keyOffsets; // Offset to each key in keys array

    // Column data (varintExternal for efficiency)
    uint8_t *columnData[COL_COUNT];      // One array per column
    varintWidth columnWidths[COL_COUNT]; // Actual width used per column

    // Metadata
    size_t rowCount;
    size_t capacity;
} Table;

void tableInit(Table *t, size_t initialCapacity) {
    t->keys = malloc(initialCapacity * 9); // Max 9 bytes per key
    t->keyOffsets = malloc(initialCapacity * sizeof(size_t));

    for (int i = 0; i < COL_COUNT; i++) {
        t->columnData[i] = calloc(initialCapacity, SCHEMA[i].maxWidth);
        t->columnWidths[i] = SCHEMA[i].maxWidth;
    }

    t->rowCount = 0;
    t->capacity = initialCapacity;
}

void tableFree(Table *t) {
    free(t->keys);
    free(t->keyOffsets);
    for (int i = 0; i < COL_COUNT; i++) {
        free(t->columnData[i]);
    }
}

// ============================================================================
// ROW INSERTION
// ============================================================================

typedef struct {
    uint64_t userId;
    uint8_t age;
    uint32_t score;
    uint64_t timestamp;
} Row;

void tableInsert(Table *t, const Row *row) {
    assert(t->rowCount < t->capacity);

    size_t idx = t->rowCount;

    // 1. Insert primary key (varintTagged for sortability)
    if (idx == 0) {
        t->keyOffsets[0] = 0;
    } else {
        // New key starts after previous key
        size_t prevKeyOffset = t->keyOffsets[idx - 1];
        varintWidth prevKeyLen = varintTaggedGetLen(t->keys + prevKeyOffset);
        t->keyOffsets[idx] = prevKeyOffset + prevKeyLen;
    }

    varintWidth keyLen =
        varintTaggedPut64(t->keys + t->keyOffsets[idx], row->userId);
    (void)keyLen;

    // 2. Insert column data (varintExternal for efficiency)
    // Age: always 1 byte
    t->columnData[COL_AGE][idx] = row->age;

    // Score: variable width
    size_t scoreOffset = idx * t->columnWidths[COL_SCORE];
    varintExternalPutFixedWidth(t->columnData[COL_SCORE] + scoreOffset,
                                row->score, t->columnWidths[COL_SCORE]);

    // Timestamp: 40-bit (5 bytes) for unix time
    size_t tsOffset = idx * t->columnWidths[COL_TIMESTAMP];
    varintExternalPutFixedWidth(t->columnData[COL_TIMESTAMP] + tsOffset,
                                row->timestamp, t->columnWidths[COL_TIMESTAMP]);

    t->rowCount++;
}

// ============================================================================
// ROW RETRIEVAL
// ============================================================================

void tableGet(const Table *t, size_t idx, Row *row) {
    assert(idx < t->rowCount);

    // Get primary key
    const uint8_t *keyPtr = t->keys + t->keyOffsets[idx];
    varintTaggedGet64(keyPtr, &row->userId);

    // Get columns
    row->age = t->columnData[COL_AGE][idx];

    size_t scoreOffset = idx * t->columnWidths[COL_SCORE];
    row->score = varintExternalGet(t->columnData[COL_SCORE] + scoreOffset,
                                   t->columnWidths[COL_SCORE]);

    size_t tsOffset = idx * t->columnWidths[COL_TIMESTAMP];
    row->timestamp = varintExternalGet(t->columnData[COL_TIMESTAMP] + tsOffset,
                                       t->columnWidths[COL_TIMESTAMP]);
}

// ============================================================================
// SORTING (B-TREE COMPATIBLE)
// ============================================================================

int compareKeys(const void *a, const void *b, const Table *t) {
    size_t idxA = *(size_t *)a;
    size_t idxB = *(size_t *)b;

    const uint8_t *keyA = t->keys + t->keyOffsets[idxA];
    const uint8_t *keyB = t->keys + t->keyOffsets[idxB];

    varintWidth lenA = varintTaggedGetLen(keyA);
    varintWidth lenB = varintTaggedGetLen(keyB);

    // memcmp works because varintTagged is big-endian!
    size_t minLen = lenA < lenB ? lenA : lenB;
    int cmp = memcmp(keyA, keyB, minLen);

    if (cmp != 0) {
        return cmp;
    }
    return (lenA > lenB) - (lenA < lenB);
}

// ============================================================================
// SECONDARY INDEX (PACKED INTEGERS)
// ============================================================================

typedef struct {
    uint8_t *packed; // 13-bit packed array (row indices 0-8191)
    size_t count;
} SecondaryIndex;

void indexInit(SecondaryIndex *idx, size_t capacity) {
    size_t bytes = (capacity * 13 + 7) / 8;
    idx->packed = calloc(1, bytes);
    idx->count = 0;
}

void indexAdd(SecondaryIndex *idx, uint16_t rowIdx) {
    assert(rowIdx < 8192); // 13-bit limit
    // Insert in sorted order
    varintPacked13InsertSorted(idx->packed, idx->count, rowIdx);
    idx->count++;
}

bool indexFind(const SecondaryIndex *idx, uint16_t rowIdx) {
    return varintPacked13Member(idx->packed, idx->count, rowIdx) >= 0;
}

void indexFree(SecondaryIndex *idx) {
    free(idx->packed);
}

// ============================================================================
// DEMONSTRATION
// ============================================================================

void demonstrateDatabase(void) {
    printf("\n=== Database System Example ===\n\n");

    // 1. Create table
    Table table;
    tableInit(&table, 100);

    printf("1. Creating table with schema:\n");
    for (int i = 0; i < COL_COUNT; i++) {
        printf("   - %-12s: %d bytes max\n", SCHEMA[i].name,
               SCHEMA[i].maxWidth);
    }

    // 2. Insert rows
    printf("\n2. Inserting 10 rows...\n");

    Row rows[] = {
        {1001, 25, 95, time(NULL)},     {1005, 30, 82, time(NULL) + 1},
        {1002, 22, 98, time(NULL) + 2}, {1008, 28, 76, time(NULL) + 3},
        {1003, 35, 91, time(NULL) + 4}, {1009, 40, 88, time(NULL) + 5},
        {1004, 26, 93, time(NULL) + 6}, {1007, 32, 79, time(NULL) + 7},
        {1006, 29, 85, time(NULL) + 8}, {1010, 24, 97, time(NULL) + 9},
    };

    for (size_t i = 0; i < 10; i++) {
        tableInsert(&table, &rows[i]);
    }

    printf("   Inserted %zu rows\n", table.rowCount);

    // 3. Retrieve and display
    printf("\n3. Retrieved rows (unsorted):\n");
    printf("   UserID | Age | Score | Timestamp\n");
    printf("   -------|-----|-------|----------\n");

    for (size_t i = 0; i < table.rowCount; i++) {
        Row row;
        tableGet(&table, i, &row);
        printf("   %6" PRIu64 " | %3u | %5u | %" PRIu64 "\n", row.userId,
               row.age, row.score, row.timestamp);
    }

    // 4. Sort by primary key
    printf("\n4. Sorting by primary key (varintTagged)...\n");

    size_t *indices = malloc(table.rowCount * sizeof(size_t));
    if (!indices) {
        fprintf(stderr, "Memory allocation failed\n");
        tableFree(&table);
        return;
    }
    for (size_t i = 0; i < table.rowCount; i++) {
        indices[i] = i;
    }

    // Simple selection sort using compareKeys
    for (size_t i = 0; i < table.rowCount - 1; i++) {
        for (size_t j = i + 1; j < table.rowCount; j++) {
            if (compareKeys(&indices[i], &indices[j], &table) > 0) {
                size_t temp = indices[i];
                indices[i] = indices[j];
                indices[j] = temp;
            }
        }
    }

    printf("   UserID | Age | Score | Timestamp\n");
    printf("   -------|-----|-------|----------\n");

    for (size_t i = 0; i < table.rowCount; i++) {
        Row row;
        tableGet(&table, indices[i], &row);
        printf("   %6" PRIu64 " | %3u | %5u | %" PRIu64 "\n", row.userId,
               row.age, row.score, row.timestamp);
    }

    free(indices);

    // 5. Create secondary index on high scores
    printf("\n5. Creating secondary index for scores > 90...\n");

    SecondaryIndex scoreIndex;
    indexInit(&scoreIndex, 100);

    for (size_t i = 0; i < table.rowCount; i++) {
        Row row;
        tableGet(&table, i, &row);
        if (row.score > 90) {
            indexAdd(&scoreIndex, i);
            printf("   Added row %zu (userID=%" PRIu64 ", score=%u) to index\n",
                   i, row.userId, row.score);
        }
    }

    printf("   Index contains %zu entries (13-bit packed)\n", scoreIndex.count);

    // 6. Query using index
    printf("\n6. Querying high-score users from index:\n");

    for (size_t i = 0; i < scoreIndex.count; i++) {
        uint16_t rowIdx = varintPacked13Get(scoreIndex.packed, i);
        Row row;
        tableGet(&table, rowIdx, &row);
        printf("   UserID %" PRIu64 ": score=%u\n", row.userId, row.score);
    }

    // 7. Space analysis
    printf("\n7. Space efficiency analysis:\n");

    // Calculate key storage
    size_t keyBytes = 0;
    for (size_t i = 0; i < table.rowCount; i++) {
        keyBytes += varintTaggedGetLen(table.keys + table.keyOffsets[i]);
    }

    // Calculate column storage
    size_t columnBytes = 0;
    for (int i = 0; i < COL_COUNT; i++) {
        columnBytes += table.rowCount * table.columnWidths[i];
    }

    // Calculate index storage
    size_t indexBytes = (scoreIndex.count * 13 + 7) / 8;

    size_t totalVarint = keyBytes + columnBytes + indexBytes;
    size_t totalFixed = (table.rowCount * 8) + columnBytes +
                        (scoreIndex.count * sizeof(uint16_t));

    printf("   Keys (varintTagged):     %zu bytes (vs %zu bytes uint64_t)\n",
           keyBytes, table.rowCount * 8);
    printf("   Columns (varintExternal): %zu bytes\n", columnBytes);
    printf("   Index (varintPacked13):   %zu bytes (vs %zu bytes uint16_t)\n",
           indexBytes, scoreIndex.count * 2);
    printf("   Total varint:             %zu bytes\n", totalVarint);
    printf("   Total fixed-width:        %zu bytes\n", totalFixed);
    printf("   Savings:                  %.1f%%\n",
           ((float)(totalFixed - totalVarint) / totalFixed) * 100);

    // Cleanup
    indexFree(&scoreIndex);
    tableFree(&table);

    printf("\n✓ Database system example complete\n");
}

int main(void) {
    printf("===========================================\n");
    printf("  Database System Integration Example\n");
    printf("===========================================\n");

    demonstrateDatabase();

    printf("\n===========================================\n");
    printf("This example demonstrated:\n");
    printf("  • varintTagged for sortable primary keys\n");
    printf("  • varintExternal for space-efficient columns\n");
    printf("  • varintPacked13 for compact indexes\n");
    printf("  • memcmp-based sorting (B-tree compatible)\n");
    printf("  • Schema-driven encoding\n");
    printf("===========================================\n");

    return 0;
}
