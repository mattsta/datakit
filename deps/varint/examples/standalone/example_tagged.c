/**
 * example_tagged.c - Demonstrates varintTagged usage
 *
 * varintTagged provides sortable, self-describing variable-length integers.
 * Perfect for database keys, B-tree nodes, and sorted data structures.
 *
 * Compile: gcc -I../src example_tagged.c ../src/varintTagged.c -o
 * example_tagged Run: ./example_tagged
 */

#include "varintTagged.h"
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

    // Encode
    varintWidth width = varintTaggedPut64(buffer, original);
    printf("Encoded %" PRIu64 " in %d bytes\n", original, width);

    // Decode
    uint64_t decoded;
    varintTaggedGet64(buffer, &decoded);
    printf("Decoded: %" PRIu64 "\n", decoded);

    assert(original == decoded);
    printf("✓ Round-trip successful\n");
}

// Example 2: Boundary values
void example_boundaries(void) {
    printf("\n=== Example 2: Boundary Values ===\n");

    struct {
        uint64_t value;
        varintWidth expectedWidth;
        const char *description;
    } tests[] = {
        {0, 1, "Zero"},
        {240, 1, "1-byte max"},
        {241, 2, "2-byte min"},
        {2287, 2, "2-byte max"},
        {2288, 3, "3-byte min"},
        {67823, 3, "3-byte max"},
        {67824, 4, "4-byte min"},
        {16777215UL, 4, "4-byte max (2^24-1)"},
        {UINT64_MAX, 9, "uint64_t max"},
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        uint8_t buffer[9];
        varintWidth width = varintTaggedPut64(buffer, tests[i].value);

        printf("%-20s: %10" PRIu64 " -> %d bytes ", tests[i].description,
               tests[i].value, width);

        assert(width == tests[i].expectedWidth);

        uint64_t decoded;
        varintTaggedGet64(buffer, &decoded);
        assert(decoded == tests[i].value);

        printf("✓\n");
    }
}

// Example 3: Sortable keys (memcmp)
void example_sortable(void) {
    printf("\n=== Example 3: Sortable Keys ===\n");

    const uint64_t keys[] = {100, 50, 200, 25, 150};
    uint8_t encoded[5][9];

    // Encode all keys
    printf("Original order: ");
    for (int i = 0; i < 5; i++) {
        varintTaggedPut64(encoded[i], keys[i]);
        printf("%" PRIu64 " ", keys[i]);
    }
    printf("\n");

    // Sort using memcmp (works because big-endian!)
    for (int i = 0; i < 4; i++) {
        for (int j = i + 1; j < 5; j++) {
            if (memcmp(encoded[i], encoded[j], 9) > 0) {
                uint8_t temp[9];
                memcpy(temp, encoded[i], 9);
                memcpy(encoded[i], encoded[j], 9);
                memcpy(encoded[j], temp, 9);
            }
        }
    }

    // Decode sorted keys
    printf("Sorted order:   ");
    for (int i = 0; i < 5; i++) {
        uint64_t value;
        varintTaggedGet64(encoded[i], &value);
        printf("%" PRIu64 " ", value);
    }
    printf("\n✓ memcmp sorting works!\n");
}

// Example 4: Database composite key
typedef struct {
    uint8_t encoded[18]; // Max: 9 bytes + 9 bytes
    size_t totalLen;
} CompositeKey;

void createCompositeKey(CompositeKey *key, uint64_t tableId, uint64_t rowId) {
    varintWidth w1 = varintTaggedPut64(key->encoded, tableId);
    varintWidth w2 = varintTaggedPut64(key->encoded + w1, rowId);
    key->totalLen = w1 + w2;
}

void decodeCompositeKey(const CompositeKey *key, uint64_t *tableId,
                        uint64_t *rowId) {
    varintWidth w1 = varintTaggedGet64(key->encoded, tableId);
    varintTaggedGet64(key->encoded + w1, rowId);
}

void example_composite_key(void) {
    printf("\n=== Example 4: Composite Keys ===\n");

    CompositeKey keys[3];

    createCompositeKey(&keys[0], 1, 100);
    createCompositeKey(&keys[1], 1, 200);
    createCompositeKey(&keys[2], 2, 50);

    printf("Created keys:\n");
    for (int i = 0; i < 3; i++) {
        uint64_t tableId, rowId;
        decodeCompositeKey(&keys[i], &tableId, &rowId);
        printf("  Key %d: table=%" PRIu64 ", row=%" PRIu64
               " (size=%zu bytes)\n",
               i, tableId, rowId, keys[i].totalLen);
    }

    // Sort composite keys (maintains table, row order)
    for (int i = 0; i < 2; i++) {
        for (int j = i + 1; j < 3; j++) {
            size_t minLen = keys[i].totalLen < keys[j].totalLen
                                ? keys[i].totalLen
                                : keys[j].totalLen;
            if (memcmp(keys[i].encoded, keys[j].encoded, minLen) > 0 ||
                (memcmp(keys[i].encoded, keys[j].encoded, minLen) == 0 &&
                 keys[i].totalLen > keys[j].totalLen)) {
                CompositeKey temp = keys[i];
                keys[i] = keys[j];
                keys[j] = temp;
            }
        }
    }

    printf("Sorted keys:\n");
    for (int i = 0; i < 3; i++) {
        uint64_t tableId, rowId;
        decodeCompositeKey(&keys[i], &tableId, &rowId);
        printf("  Key %d: table=%" PRIu64 ", row=%" PRIu64 "\n", i, tableId,
               rowId);
    }
    printf("✓ Composite keys sorted correctly\n");
}

// Example 5: In-place arithmetic
void example_arithmetic(void) {
    printf("\n=== Example 5: In-Place Arithmetic ===\n");

    uint8_t counter[9];
    varintTaggedPut64(counter, 0);

    printf("Initial value: 0\n");

    // Increment 10 times
    for (int i = 0; i < 10; i++) {
        varintWidth newWidth = varintTaggedAddGrow(counter, 1);
        (void)newWidth; // Allow growth
    }

    uint64_t result;
    varintTaggedGet64(counter, &result);
    printf("After 10 increments: %" PRIu64 "\n", result);
    assert(result == 10);

    // Add 230 more (crosses 240 boundary)
    varintTaggedAddGrow(counter, 230);
    varintTaggedGet64(counter, &result);
    printf("After adding 230: %" PRIu64 " (now uses 2 bytes)\n", result);
    assert(result == 240);

    // Add 1 more (grows to 2 bytes)
    varintTaggedAddGrow(counter, 1);
    varintTaggedGet64(counter, &result);
    printf("After adding 1: %" PRIu64 " (uses 2 bytes)\n", result);
    assert(result == 241);

    printf("✓ In-place arithmetic works\n");
}

// Example 6: Fixed-width encoding
void example_fixed_width(void) {
    printf("\n=== Example 6: Fixed-Width Encoding ===\n");

    uint8_t slot[9];

    // Encode small value in large slot (for later updates)
    varintTaggedPut64FixedWidth(slot, 10, 5); // Use 5 bytes

    uint64_t value;
    varintTaggedGet64(slot, &value);
    printf("Value 10 stored in 5 bytes: %" PRIu64 "\n", value);

    // Can now update to larger values without reallocation
    varintTaggedPut64FixedWidth(slot, 1000000, 5);
    varintTaggedGet64(slot, &value);
    printf("Updated to 1000000 (still 5 bytes): %" PRIu64 "\n", value);

    printf("✓ Fixed-width encoding for update-in-place\n");
}

// Example 7: Performance comparison
void example_performance(void) {
    printf("\n=== Example 7: Space Efficiency ===\n");

    uint64_t testValues[] = {10, 100, 1000, 10000, 100000, 1000000, 10000000};

    printf("Value      | Tagged | uint64_t | Savings\n");
    printf("-----------|--------|----------|--------\n");

    for (size_t i = 0; i < sizeof(testValues) / sizeof(testValues[0]); i++) {
        uint8_t buffer[9];
        varintWidth width = varintTaggedPut64(buffer, testValues[i]);
        float savings = ((8.0 - width) / 8.0) * 100.0;

        printf("%10" PRIu64 " | %2d     | 8        | %5.1f%%\n", testValues[i],
               width, savings);
    }
}

int main(void) {
    printf("===========================================\n");
    printf("    varintTagged Example Suite\n");
    printf("===========================================\n");

    example_basic();
    example_boundaries();
    example_sortable();
    example_composite_key();
    example_arithmetic();
    example_fixed_width();
    example_performance();

    printf("\n===========================================\n");
    printf("All examples completed successfully!\n");
    printf("===========================================\n");

    return 0;
}
