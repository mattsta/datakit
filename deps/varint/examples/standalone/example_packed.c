/**
 * example_packed.c - Demonstrates varintPacked usage
 *
 * varintPacked provides fixed-width bit-packed arrays where each element
 * uses exactly the specified number of bits (not bytes). Perfect for:
 * - Storing bounded integers (e.g., 0-999 needs 10 bits, not 16)
 * - Game coordinates with known ranges
 * - IP address components (0-255 = 8 bits)
 * - Efficient array storage with uniform bit width
 *
 * Compile: gcc -I../src example_packed.c -o example_packed
 * Run: ./example_packed
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Define packed array for 12-bit values (0-4095)
#define PACK_STORAGE_BITS 12
#include "varintPacked.h"

// Helper macro for calculating bytes needed
/* varintPacked12 uses uint32_t slots by default, so we need to account for
 * 32-bit alignment */
#define BYTES_FOR_COUNT(count) ((((count) * 12 + 31) / 32) * 4)

// Example 1: Basic set/get operations
void example_basic(void) {
    printf("\n=== Example 1: Basic Set/Get Operations ===\n");

    // Array for 10 elements, each 12 bits
    size_t count = 10;
    size_t bytes = BYTES_FOR_COUNT(count);
    uint8_t *array = calloc(1, bytes);

    printf("Array: %zu elements × 12 bits = %zu bytes\n", count, bytes);
    printf("(vs %zu bytes for uint16_t)\n", count * sizeof(uint16_t));

    // Set some values
    varintPacked12Set(array, 0, 100);
    varintPacked12Set(array, 1, 200);
    varintPacked12Set(array, 2, 4095); // Max 12-bit value
    varintPacked12Set(array, 9, 999);

    // Get and verify
    printf("\nStored values:\n");
    assert(varintPacked12Get(array, 0) == 100);
    printf("  [0] = %u ✓\n", varintPacked12Get(array, 0));

    assert(varintPacked12Get(array, 1) == 200);
    printf("  [1] = %u ✓\n", varintPacked12Get(array, 1));

    assert(varintPacked12Get(array, 2) == 4095);
    printf("  [2] = %u ✓\n", varintPacked12Get(array, 2));

    assert(varintPacked12Get(array, 9) == 999);
    printf("  [9] = %u ✓\n", varintPacked12Get(array, 9));

    free(array);
}

// Example 2: Sorted array with binary search
void example_sorted(void) {
    printf("\n=== Example 2: Sorted Array with Binary Search ===\n");

    size_t count = 8;
    size_t bytes = BYTES_FOR_COUNT(count);
    uint8_t *array = calloc(1, bytes);

    // Build sorted array manually
    const uint16_t values[] = {10, 25, 50, 100, 200, 500, 1000, 2000};
    for (size_t i = 0; i < count; i++) {
        varintPacked12Set(array, i, values[i]);
    }

    printf("Sorted array: ");
    for (size_t i = 0; i < count; i++) {
        printf("%u ", varintPacked12Get(array, i));
    }
    printf("\n");

    // Binary search
    const uint16_t searchValues[] = {10, 100, 2000, 99, 1001};
    const char *searchDesc[] = {"10 (exists)", "100 (exists)", "2000 (exists)",
                                "99 (not found)", "1001 (not found)"};

    printf("\nBinary search:\n");
    for (size_t i = 0; i < 5; i++) {
        size_t pos = varintPacked12BinarySearch(array, count, searchValues[i]);
        uint16_t atPos = varintPacked12Get(array, pos);

        printf("  Search %s: position %zu (value %u)\n", searchDesc[i], pos,
               atPos);
    }

    printf("✓ Binary search works\n");
    free(array);
}

// Example 3: Sorted insert
void example_sorted_insert(void) {
    printf("\n=== Example 3: Sorted Insert ===\n");

    // Start with small array, grow as needed
    size_t capacity = 8;
    size_t count = 0;
    size_t bytes = BYTES_FOR_COUNT(capacity);
    uint8_t *array = calloc(1, bytes);

    // Insert values in random order (will be kept sorted)
    const uint16_t insertValues[] = {500, 100, 1000, 50, 750, 25};
    printf("Inserting: ");
    for (size_t i = 0; i < 6; i++) {
        printf("%u ", insertValues[i]);
    }
    printf("\n");

    for (size_t i = 0; i < 6; i++) {
        varintPacked12InsertSorted(array, count, insertValues[i]);
        count++;
    }

    printf("Sorted result: ");
    for (size_t i = 0; i < count; i++) {
        printf("%u ", varintPacked12Get(array, i));
        if (i > 0) {
            // Verify sorted order
            assert(varintPacked12Get(array, i - 1) <=
                   varintPacked12Get(array, i));
        }
    }
    printf("\n✓ Sorted insert maintains order\n");

    free(array);
}

// Example 4: Member testing
void example_member(void) {
    printf("\n=== Example 4: Membership Testing ===\n");

    size_t count = 5;
    size_t bytes = BYTES_FOR_COUNT(count);
    uint8_t *set = calloc(1, bytes);

    // Create a sorted set
    const uint16_t members[] = {10, 20, 30, 40, 50};
    for (size_t i = 0; i < count; i++) {
        varintPacked12Set(set, i, members[i]);
    }

    printf("Set: {");
    for (size_t i = 0; i < count; i++) {
        printf("%u", varintPacked12Get(set, i));
        if (i < count - 1) {
            printf(", ");
        }
    }
    printf("}\n\n");

    // Test membership
    const uint16_t testValues[] = {10, 15, 30, 45, 50};
    const char *expected[] = {"member", "not member", "member", "not member",
                              "member"};

    printf("Membership tests:\n");
    for (size_t i = 0; i < 5; i++) {
        int64_t memberIndex = varintPacked12Member(set, count, testValues[i]);
        bool isMember = (memberIndex >= 0); // Index 0 is valid, so check >= 0
        printf("  %u: %s ", testValues[i], isMember ? "member" : "not member");

        bool expectedMember = (strcmp(expected[i], "member") == 0);
        assert(isMember == expectedMember);
        printf("✓\n");
    }

    free(set);
}

// Example 5: Space efficiency
void example_space_efficiency(void) {
    printf("\n=== Example 5: Space Efficiency ===\n");

    size_t arraySize = 1000;

    // Compare different bit widths
    struct {
        int bits;
        size_t maxValue;
        const char *useCase;
    } configs[] = {
        {8, 255, "IP address octets"},     {10, 1023, "Small IDs (0-1023)"},
        {12, 4095, "Medium IDs (0-4095)"}, {14, 16383, "Large IDs (0-16383)"},
        {16, 65535, "Standard uint16_t"},
    };

    printf("Array of %zu elements:\n\n", arraySize);
    printf("Bits | Max Value | Use Case                 | Bytes  | vs uint16 | "
           "vs uint32\n");
    printf("-----|-----------|--------------------------|--------|-----------|-"
           "---------\n");

    for (size_t i = 0; i < sizeof(configs) / sizeof(configs[0]); i++) {
        // Calculate bytes needed
        size_t bitsTotal = (size_t)configs[i].bits * arraySize;
        size_t bytesNeeded = (bitsTotal + 7) / 8;
        size_t uint16Bytes = arraySize * 2;
        size_t uint32Bytes = arraySize * 4;

        float vs16 = ((float)(uint16Bytes - bytesNeeded) / uint16Bytes) * 100;
        float vs32 = ((float)(uint32Bytes - bytesNeeded) / uint32Bytes) * 100;

        printf("%4d | %9zu | %-24s | %6zu | %6.1f%%  | %6.1f%%\n",
               configs[i].bits, configs[i].maxValue, configs[i].useCase,
               bytesNeeded, vs16, vs32);
    }
}

// Example 6: Game coordinates
void example_game_coordinates(void) {
    printf("\n=== Example 6: Game Coordinates (12-bit) ===\n");

    // Game world: 4096×4096 grid (needs 12 bits per coordinate)
    typedef struct {
        uint16_t x;
        uint16_t y;
    } Coord;

    Coord entities[] = {
        {100, 200}, {500, 750}, {1000, 1500}, {2048, 2048}, {4095, 4095},
    };
    size_t entityCount = sizeof(entities) / sizeof(entities[0]);

    // Pack X and Y coordinates
    size_t bytes = BYTES_FOR_COUNT(entityCount);
    uint8_t *xCoords = calloc(1, bytes);
    uint8_t *yCoords = calloc(1, bytes);

    for (size_t i = 0; i < entityCount; i++) {
        varintPacked12Set(xCoords, i, entities[i].x);
        varintPacked12Set(yCoords, i, entities[i].y);
    }

    printf("%zu entities stored:\n", entityCount);
    for (size_t i = 0; i < entityCount; i++) {
        uint16_t x = varintPacked12Get(xCoords, i);
        uint16_t y = varintPacked12Get(yCoords, i);
        printf("  Entity %zu: (%u, %u)\n", i, x, y);
        assert(x == entities[i].x);
        assert(y == entities[i].y);
    }

    size_t packedSize = bytes * 2; // x and y arrays
    size_t uint16Size = entityCount * sizeof(Coord);

    printf("\nSpace usage:\n");
    printf("  Packed (12-bit): %zu bytes\n", packedSize);
    printf("  uint16_t:        %zu bytes\n", uint16Size);
    printf("  Savings:         %.1f%%\n",
           ((float)(uint16Size - packedSize) / uint16Size) * 100);

    free(xCoords);
    free(yCoords);
}

// Example 7: Deletion
void example_deletion(void) {
    printf("\n=== Example 7: Deletion ===\n");

    size_t count = 6;
    size_t bytes = BYTES_FOR_COUNT(count);
    uint8_t *array = calloc(1, bytes);

    // Initialize array
    for (size_t i = 0; i < count; i++) {
        varintPacked12Set(array, i, (i + 1) * 100);
    }

    printf("Original: ");
    for (size_t i = 0; i < count; i++) {
        printf("%u ", varintPacked12Get(array, i));
    }
    printf("\n");

    // Delete element at index 2 (value 300)
    varintPacked12Delete(array, count, 2);
    count--;

    printf("After deleting index 2: ");
    for (size_t i = 0; i < count; i++) {
        printf("%u ", varintPacked12Get(array, i));
    }
    printf("\n");

    // Verify values shifted correctly
    assert(varintPacked12Get(array, 0) == 100);
    assert(varintPacked12Get(array, 1) == 200);
    assert(varintPacked12Get(array, 2) == 400); // Was at index 3
    assert(varintPacked12Get(array, 3) == 500);
    assert(varintPacked12Get(array, 4) == 600);

    printf("✓ Deletion shifts elements correctly\n");

    free(array);
}

int main(void) {
    printf("===========================================\n");
    printf("    varintPacked Example Suite\n");
    printf("===========================================\n");
    printf("12-bit packed arrays (0-4095)\n");

    example_basic();
    example_sorted();
    example_sorted_insert();
    example_member();
    example_space_efficiency();
    example_game_coordinates();
    example_deletion();

    printf("\n===========================================\n");
    printf("All examples completed successfully!\n");
    printf("===========================================\n");

    return 0;
}
