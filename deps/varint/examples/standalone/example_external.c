/**
 * example_external.c - Demonstrates varintExternal usage
 *
 * varintExternal provides zero-overhead variable-length integers where
 * width metadata is stored externally. Most space-efficient encoding.
 * Perfect for columnar storage, arrays with external metadata, and caches.
 *
 * Compile: gcc -I../src example_external.c ../src/varintExternal.c -o
 * example_external Run: ./example_external
 */

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

// Example 1: Basic encode/decode with external width
void example_basic(void) {
    printf("\n=== Example 1: Basic Encode/Decode ===\n");

    uint8_t buffer[8];
    uint64_t original = 12345;

    // Determine width needed
    varintWidth width;
    varintExternalUnsignedEncoding(original, width);
    printf("Value %" PRIu64 " requires %d bytes\n", original, width);

    // Encode
    varintExternalPutFixedWidthQuick_(buffer, original, width);
    printf("Encoded in %d bytes\n", width);

    // Decode
    uint64_t decoded;
    varintExternalGetQuick_(buffer, width, decoded);
    printf("Decoded: %" PRIu64 "\n", decoded);

    assert(original == decoded);
    printf("✓ Round-trip successful\n");
}

// Example 2: Width detection for various values
void example_width_detection(void) {
    printf("\n=== Example 2: Width Detection ===\n");

    struct {
        uint64_t value;
        varintWidth expectedWidth;
        const char *description;
    } tests[] = {
        {0, 1, "Zero"},
        {1, 1, "One"},
        {255, 1, "1-byte max"},
        {256, 2, "2-byte min"},
        {65535UL, 2, "2-byte max"},
        {65536UL, 3, "3-byte min"},
        {16777215UL, 3, "3-byte max (2^24-1)"},
        {16777216UL, 4, "4-byte min (2^24)"},
        {4294967295UL, 4, "4-byte max (2^32-1)"},
        {4294967296UL, 5, "5-byte min (2^32)"},
        {UINT64_MAX, 8, "uint64_t max"},
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        varintWidth width;
        varintExternalUnsignedEncoding(tests[i].value, width);

        printf("%-25s: %20" PRIu64 " -> %d bytes ", tests[i].description,
               tests[i].value, width);

        assert(width == tests[i].expectedWidth);

        uint8_t buffer[8];
        varintExternalPutFixedWidthQuick_(buffer, tests[i].value, width);

        uint64_t decoded;
        varintExternalGetQuick_(buffer, width, decoded);
        assert(decoded == tests[i].value);

        printf("✓\n");
    }
}

// Example 3: Column store with external metadata
typedef struct {
    varintWidth *widths; // Width for each column
    uint8_t **columns;   // Column data
    size_t numRows;
    size_t numCols;
} ColumnStore;

ColumnStore *createColumnStore(size_t rows, size_t cols) {
    ColumnStore *store = malloc(sizeof(ColumnStore));
    CHECK_ALLOC(store);
    store->numRows = rows;
    store->numCols = cols;
    store->widths = calloc(cols, sizeof(varintWidth));
    CHECK_ALLOC(store->widths);
    store->columns = calloc(cols, sizeof(uint8_t *));
    CHECK_ALLOC(store->columns);
    return store;
}

void setColumn(ColumnStore *store, size_t col, const uint64_t *values) {
    // Determine maximum width needed for this column
    varintWidth maxWidth = 0;
    for (size_t i = 0; i < store->numRows; i++) {
        varintWidth width;
        varintExternalUnsignedEncoding(values[i], width);
        if (width > maxWidth) {
            maxWidth = width;
        }
    }

    store->widths[col] = maxWidth;
    store->columns[col] = malloc(maxWidth * store->numRows);
    CHECK_ALLOC(store->columns[col]);

    // Encode all values at same width
    for (size_t i = 0; i < store->numRows; i++) {
        varintExternalPutFixedWidthQuick_(store->columns[col] + (i * maxWidth),
                                          values[i], maxWidth);
    }
}

uint64_t getColumnValue(const ColumnStore *store, size_t row, size_t col) {
    uint64_t value;
    varintExternalGetQuick_(store->columns[col] + (row * store->widths[col]),
                            store->widths[col], value);
    return value;
}

void freeColumnStore(ColumnStore *store) {
    for (size_t i = 0; i < store->numCols; i++) {
        free(store->columns[i]);
    }
    free(store->columns);
    free(store->widths);
    free(store);
}

void example_column_store(void) {
    printf("\n=== Example 3: Column Store ===\n");

    ColumnStore *store = createColumnStore(5, 3);

    // Column 0: Small IDs (1-100)
    const uint64_t col0[] = {1, 2, 3, 4, 5};
    setColumn(store, 0, col0);

    // Column 1: Medium values (0-10000)
    const uint64_t col1[] = {100, 500, 1000, 5000, 10000};
    setColumn(store, 1, col1);

    // Column 2: Large values
    const uint64_t col2[] = {1000000, 2000000, 3000000, 4000000, 5000000};
    setColumn(store, 2, col2);

    printf("Column widths: %d, %d, %d bytes\n", store->widths[0],
           store->widths[1], store->widths[2]);

    // Calculate space savings
    size_t varintSize = 0;
    size_t uint64Size = 0;
    for (size_t col = 0; col < store->numCols; col++) {
        varintSize += store->widths[col] * store->numRows;
        uint64Size += 8 * store->numRows;
    }

    printf("Space used: %zu bytes (vs %zu with uint64_t)\n", varintSize,
           uint64Size);
    printf("Savings: %.1f%%\n",
           ((float)(uint64Size - varintSize) / uint64Size) * 100);

    // Verify data
    for (size_t row = 0; row < store->numRows; row++) {
        assert(getColumnValue(store, row, 0) == col0[row]);
        assert(getColumnValue(store, row, 1) == col1[row]);
        assert(getColumnValue(store, row, 2) == col2[row]);
    }

    printf("✓ All values stored and retrieved correctly\n");

    freeColumnStore(store);
}

// Example 4: Array compression with uniform width
void example_array_compression(void) {
    printf("\n=== Example 4: Array Compression ===\n");

    // Array of timestamps (40-bit values)
    uint64_t timestamps[] = {
        1700000000UL, 1700000060UL, 1700000120UL, 1700000180UL, 1700000240UL,
    };
    size_t count = sizeof(timestamps) / sizeof(timestamps[0]);

    // Find max width
    varintWidth width = 0;
    for (size_t i = 0; i < count; i++) {
        varintWidth w;
        varintExternalUnsignedEncoding(timestamps[i], w);
        if (w > width) {
            width = w;
        }
    }

    printf("Array of %zu timestamps requires %d bytes each\n", count, width);

    // Compress array
    uint8_t *compressed = malloc(width * count);
    CHECK_ALLOC(compressed);
    for (size_t i = 0; i < count; i++) {
        varintExternalPutFixedWidthQuick_(compressed + (i * width),
                                          timestamps[i], width);
    }

    // Decompress and verify
    for (size_t i = 0; i < count; i++) {
        uint64_t value;
        varintExternalGetQuick_(compressed + (i * width), width, value);
        assert(value == timestamps[i]);
    }

    size_t compressedSize = width * count;
    size_t originalSize = sizeof(uint64_t) * count;

    printf("Compressed: %zu bytes (vs %zu uncompressed)\n", compressedSize,
           originalSize);
    printf("Compression ratio: %.1fx\n", (float)originalSize / compressedSize);
    printf("✓ Array compressed successfully\n");

    free(compressed);
}

// Example 5: Endianness handling
void example_endianness(void) {
    printf("\n=== Example 5: Endianness ===\n");

    uint64_t value = 0x0102030405060708UL;
    uint8_t buffer[8];

    varintExternalPutFixedWidthQuick_(buffer, value, 8);

    printf("Value: 0x%016" PRIx64 "\n", value);
    printf("Encoded bytes (little-endian on this system): ");
    for (int i = 0; i < 8; i++) {
        printf("%02x ", buffer[i]);
    }
    printf("\n");

    uint64_t decoded;
    varintExternalGetQuick_(buffer, 8, decoded);

    assert(decoded == value);
    printf("✓ Endianness handled correctly\n");
}

// Example 6: Signed values
void example_signed(void) {
    printf("\n=== Example 6: Signed Values ===\n");

    int64_t signedValues[] = {0, 100, 1000, 10000, 100000};

    printf("Signed values stored as unsigned:\n");

    for (size_t i = 0; i < sizeof(signedValues) / sizeof(signedValues[0]);
         i++) {
        varintWidth width = varintExternalSignedEncoding(signedValues[i]);
        uint8_t buffer[8];

        // Store as unsigned (since values are positive)
        varintExternalPutFixedWidthQuick_(buffer, (uint64_t)signedValues[i],
                                          width);

        uint64_t decoded;
        varintExternalGetQuick_(buffer, width, decoded);

        printf("  %10" PRId64 " -> %d bytes\n", signedValues[i], width);
        assert((int64_t)decoded == signedValues[i]);
    }

    printf("✓ Signed values handled correctly\n");
}

// Example 7: Performance and space analysis
void example_performance(void) {
    printf("\n=== Example 7: Space Efficiency Comparison ===\n");

    uint64_t testValues[] = {10, 100, 1000, 10000, 100000, 1000000, 10000000};

    printf("Value      | External | uint64_t | Savings\n");
    printf("-----------|----------|----------|--------\n");

    for (size_t i = 0; i < sizeof(testValues) / sizeof(testValues[0]); i++) {
        varintWidth width;
        varintExternalUnsignedEncoding(testValues[i], width);
        float savings = ((8.0 - width) / 8.0) * 100.0;

        printf("%10" PRIu64 " | %2d       | 8        | %5.1f%%\n",
               testValues[i], width, savings);
    }
}

int main(void) {
    printf("===========================================\n");
    printf("   varintExternal Example Suite\n");
    printf("===========================================\n");

    example_basic();
    example_width_detection();
    example_column_store();
    example_array_compression();
    example_endianness();
    example_signed();
    example_performance();

    printf("\n===========================================\n");
    printf("All examples completed successfully!\n");
    printf("===========================================\n");

    return 0;
}
