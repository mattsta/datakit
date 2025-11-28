/**
 * example_split.c - Demonstrates varintSplit usage
 *
 * varintSplit provides three-level encoding with known bit boundaries.
 * Fast encoding/decoding with efficient space usage for small values.
 * Perfect for data with predictable ranges and bit-packing requirements.
 *
 * Compile: gcc -I../src example_split.c ../src/varintExternal.c -o
 * example_split Run: ./example_split
 */

#include "varintExternal.h"
#include "varintSplit.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Example 1: Basic encode/decode
void example_basic(void) {
    printf("\n=== Example 1: Basic Encode/Decode ===\n");

    uint8_t buffer[9];
    uint64_t original = 12345;
    varintWidth width;

    // Encode
    varintSplitPut_(buffer, width, original);
    printf("Encoded %" PRIu64 " in %d bytes\n", original, width);

    // Decode
    uint64_t decoded;
    varintWidth decodedWidth;
    varintSplitGet_(buffer, decodedWidth, decoded);

    printf("Decoded: %" PRIu64 " (%d bytes)\n", decoded, decodedWidth);

    assert(original == decoded);
    assert(width == decodedWidth);
    printf("✓ Round-trip successful\n");
}

// Example 2: Three-level encoding boundaries
void example_three_levels(void) {
    printf("\n=== Example 2: Three-Level Encoding ===\n");

    struct {
        uint64_t value;
        varintWidth expectedWidth;
        const char *description;
    } tests[] = {
        {0, 1, "Zero"},
        {63, 1, "Level 1 max (6 bits)"},
        {64, 2, "Level 2 min"},
        {16446, 2, "Level 2 max (14 bits)"},
        {16447, 2, "Level 3 min"},
        {16702, 3, "Level 3 (16447+255)"},
        {16703, 3, "Level 3 (16447+256)"},
        {81982, 4, "Level 3 (16447+65535)"},
        {UINT64_MAX, 9, "uint64_t max"},
    };

    printf("Three encoding levels:\n");
    printf("  Level 1 (00xxxxxx): 0-63      (6 bits in first byte)\n");
    printf("  Level 2 (01xxxxxx): 64-16,446 (14 bits total)\n");
    printf("  Level 3 (10xxxxxx): 16,447+   (varintExternal)\n\n");

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        uint8_t buffer[9];
        varintWidth width;

        varintSplitPut_(buffer, width, tests[i].value);

        printf("%-25s: %10" PRIu64 " -> %d bytes (type: 0x%02x)",
               tests[i].description, tests[i].value, width,
               buffer[0] & 0xC0); // Show type bits

        assert(width == tests[i].expectedWidth);

        uint64_t decoded;
        varintWidth decodedWidth;
        varintSplitGet_(buffer, decodedWidth, decoded);
        assert(decoded == tests[i].value);
        assert(decodedWidth == width);

        printf(" ✓\n");
    }
}

// Example 3: Type detection
void example_type_detection(void) {
    printf("\n=== Example 3: Type Detection ===\n");

    struct {
        uint64_t value;
        const char *expectedType;
    } tests[] = {
        {50, "Level 1 (6-bit)"},
        {1000, "Level 2 (14-bit)"},
        {100000, "Level 3 (VAR)"},
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        uint8_t buffer[9];
        varintWidth width;

        varintSplitPut_(buffer, width, tests[i].value);

        // Decode type from first byte
        uint8_t typeMarker = varintSplitEncoding2_(buffer);
        const char *typeName;

        switch (typeMarker) {
        case VARINT_SPLIT_6:
            typeName = "Level 1 (6-bit)";
            break;
        case VARINT_SPLIT_14:
            typeName = "Level 2 (14-bit)";
            break;
        case VARINT_SPLIT_VAR:
            typeName = "Level 3 (VAR)";
            break;
        default:
            typeName = "Unknown";
        }

        printf("Value %10" PRIu64 " -> %s ", tests[i].value, typeName);
        assert(strcmp(typeName, tests[i].expectedType) == 0);
        printf("✓\n");
    }
}

// Example 4: Reversed split (for backward traversal)
void example_reversed(void) {
    printf("\n=== Example 4: Reversed Split Encoding ===\n");

    const uint64_t values[] = {10, 100, 1000, 10000};
    uint8_t buffer[64] = {0};
    size_t offset = 0;

    printf("Forward encoding:\n");
    for (size_t i = 0; i < 4; i++) {
        varintWidth width;
        varintSplitPut_(buffer + offset, width, values[i]);
        printf("  Value %" PRIu64 " at offset %zu (width %d)\n", values[i],
               offset, width);
        offset += width;
    }

    printf("Total size: %zu bytes\n", offset);

    // Decode forward
    printf("\nForward decoding:\n");
    offset = 0;
    for (size_t i = 0; i < 4; i++) {
        uint64_t decoded;
        varintWidth width;
        varintSplitGet_(buffer + offset, width, decoded);
        printf("  Offset %zu: %" PRIu64 "\n", offset, decoded);
        assert(decoded == values[i]);
        offset += width;
    }

    printf("✓ Forward encoding/decoding works\n");

    // Now demonstrate reversed encoding
    printf("\nReversed encoding (for backward traversal):\n");
    uint8_t reversed[128]; // Increased buffer size
    memset(reversed, 0, sizeof(reversed));
    offset = 20; // Start earlier to fit all values

    for (size_t i = 0; i < 4; i++) {
        varintWidth width;
        varintSplitReversedPutReversed_(reversed + offset, width, values[i]);
        printf("  Value %" PRIu64
               " at position %zu (width %d, type at [%zu])\n",
               values[i], offset, width, offset);
        offset += 10; // Move to next position
    }

    printf("✓ Reversed encoding demonstrated\n");
}

// Example 5: Bit-packing integration
void example_bitpacking(void) {
    printf("\n=== Example 5: Integration with Bit-Packing ===\n");

    // Store array of values with mixed sizes
    typedef struct {
        uint8_t *data;
        size_t *offsets;
        size_t count;
        size_t totalSize;
    } VarArray;

    uint64_t values[] = {5, 10, 50, 100, 500, 1000, 5000, 10000, 50000};
    size_t count = sizeof(values) / sizeof(values[0]);

    VarArray array;
    array.count = count;
    array.offsets = malloc((count + 1) * sizeof(size_t));
    array.data = malloc(count * 9); // Max size

    size_t offset = 0;
    for (size_t i = 0; i < count; i++) {
        varintWidth width;
        array.offsets[i] = offset;
        varintSplitPut_(array.data + offset, width, values[i]);
        offset += width;
    }
    array.offsets[count] = offset;
    array.totalSize = offset;

    printf("Stored %zu values in %zu bytes\n", count, array.totalSize);
    printf("Average: %.2f bytes/value\n", (float)array.totalSize / count);

    // Random access
    printf("\nRandom access:\n");
    const size_t testIndices[] = {0, 4, 8};
    for (size_t i = 0; i < 3; i++) {
        size_t idx = testIndices[i];
        uint64_t decoded;
        varintWidth width;
        varintSplitGet_(array.data + array.offsets[idx], width, decoded);
        printf("  Index %zu: %" PRIu64 "\n", idx, decoded);
        assert(decoded == values[idx]);
    }

    printf("✓ Variable-width array works\n");

    free(array.offsets);
    free(array.data);
}

// Example 6: Performance comparison
void example_performance(void) {
    printf("\n=== Example 6: Encoding Type Distribution ===\n");

    uint64_t ranges[] = {0, 64, 16447, 1000000};
    const char *rangeNames[] = {"0-63", "64-16,446", "16,447-999,999",
                                "1,000,000+"};

    printf("Range          | Level      | Bytes | Efficiency\n");
    printf("---------------|------------|-------|------------\n");

    for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
        uint64_t testVal = ranges[i];
        if (i > 0) {
            testVal += 10; // Move into range
        }

        uint8_t buffer[9];
        varintWidth width;
        varintSplitPut_(buffer, width, testVal);

        uint8_t type = varintSplitEncoding2_(buffer);
        const char *levelName;
        switch (type) {
        case VARINT_SPLIT_6:
            levelName = "Level 1";
            break;
        case VARINT_SPLIT_14:
            levelName = "Level 2";
            break;
        case VARINT_SPLIT_VAR:
            levelName = "Level 3";
            break;
        default:
            levelName = "Unknown";
        }

        printf("%-14s | %-10s | %5d | %s\n", rangeNames[i], levelName, width,
               width == 1   ? "Excellent"
               : width == 2 ? "Good"
                            : "Variable");
    }
}

// Example 7: Length calculation
void example_length(void) {
    printf("\n=== Example 7: Length Calculation ===\n");

    uint8_t buffer[64] = {0};
    const uint64_t values[] = {10, 1000, 100000};
    size_t offset = 0;

    // Encode multiple values
    for (size_t i = 0; i < 3; i++) {
        varintWidth width;
        varintSplitPut_(buffer + offset, width, values[i]);
        offset += width;
    }

    // Decode with length checking
    printf("Encoded values with length detection:\n");
    offset = 0;
    for (size_t i = 0; i < 3; i++) {
        // Get length without full decode
        varintWidth len = varintSplitGetLenQuick_(buffer + offset);

        uint64_t decoded;
        varintWidth width;
        varintSplitGet_(buffer + offset, width, decoded);

        printf("  Value: %" PRIu64 ", Length: %d bytes ", decoded, width);
        assert(len == width);
        printf("✓\n");

        offset += width;
    }
}

int main(void) {
    printf("===========================================\n");
    printf("     varintSplit Example Suite\n");
    printf("===========================================\n");

    example_basic();
    example_three_levels();
    example_type_detection();
    example_reversed();
    example_bitpacking();
    example_performance();
    example_length();

    printf("\n===========================================\n");
    printf("All examples completed successfully!\n");
    printf("===========================================\n");

    return 0;
}
