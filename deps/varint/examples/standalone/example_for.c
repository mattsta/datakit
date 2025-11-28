/**
 * example_for.c - Demonstrates varintFOR (Frame-of-Reference) usage
 *
 * varintFOR provides highly efficient encoding for clustered values by storing
 * all values as fixed-width offsets from a minimum value. Perfect for
 * timestamps, sequential IDs, prices in similar ranges, and any clustered
 * integer data.
 *
 * Compile: gcc -I../../src example_for.c ../../src/varintFOR.c
 * ../../src/varintExternal.c ../../src/varintTagged.c -o example_for Run:
 * ./example_for
 */

#include "varintFOR.h"
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

// Example 1: Basic encode/decode
void example_basic() {
    printf("\n=== Example 1: Basic Encode/Decode ===\n");

    uint64_t values[] = {1000, 1005, 1002, 1010, 1001};
    size_t count = sizeof(values) / sizeof(values[0]);

    // Analyze values
    varintFORMeta meta;
    varintFORAnalyze(values, count, &meta);

    printf("Values: ");
    for (size_t i = 0; i < count; i++) {
        printf("%" PRIu64 " ", values[i]);
    }
    printf("\n");

    printf("Min: %" PRIu64 ", Max: %" PRIu64 ", Range: %" PRIu64 "\n",
           meta.minValue, meta.maxValue, meta.range);
    printf("Offset width: %d bytes\n", meta.offsetWidth);
    printf("Encoded size: %zu bytes\n", meta.encodedSize);
    printf("vs uint64_t array: %zu bytes\n", count * 8);
    printf("Savings: %.1f%%\n",
           ((float)(count * 8 - meta.encodedSize) / (count * 8)) * 100);

    // Encode
    uint8_t *encoded = malloc(meta.encodedSize);
    CHECK_ALLOC(encoded);
    size_t encodedLen = varintFOREncode(encoded, values, count, &meta);
    assert(encodedLen == meta.encodedSize);
    printf("Encoded %zu values in %zu bytes\n", count, encodedLen);

    // Decode
    uint64_t *decoded = malloc(count * sizeof(uint64_t));
    CHECK_ALLOC(decoded);
    size_t decodedCount = varintFORDecode(encoded, decoded, count);
    assert(decodedCount == count);

    // Verify
    for (size_t i = 0; i < count; i++) {
        assert(decoded[i] == values[i]);
    }
    printf("Decoded %zu values successfully\n", decodedCount);

    free(encoded);
    free(decoded);

    printf("✓ Round-trip successful\n");
}

// Example 2: Timestamps in narrow window
void example_timestamps() {
    printf("\n=== Example 2: Timestamps (1-day window) ===\n");

    // Timestamps within a single day (Nov 19, 2025)
    uint64_t baseTime = 1732003200UL; // 2025-11-19 00:00:00 UTC
    uint64_t timestamps[] = {
        baseTime + 0,     // 00:00:00
        baseTime + 3600,  // 01:00:00
        baseTime + 7200,  // 02:00:00
        baseTime + 10800, // 03:00:00
        baseTime + 14400, // 04:00:00
        baseTime + 43200, // 12:00:00
        baseTime + 86399, // 23:59:59
    };
    size_t count = sizeof(timestamps) / sizeof(timestamps[0]);

    varintFORMeta meta;
    varintFORAnalyze(timestamps, count, &meta);

    printf("Timestamps in 24-hour window:\n");
    printf("  Min: %" PRIu64 ", Max: %" PRIu64 "\n", meta.minValue,
           meta.maxValue);
    printf("  Range: %" PRIu64 " seconds (%.1f hours)\n", meta.range,
           meta.range / 3600.0);
    printf("  Offset width: %d bytes (range fits in %d bytes)\n",
           meta.offsetWidth, meta.offsetWidth);

    uint8_t *encoded = malloc(meta.encodedSize);
    CHECK_ALLOC(encoded);
    varintFOREncode(encoded, timestamps, count, &meta);

    printf("Storage:\n");
    printf("  FOR encoded: %zu bytes\n", meta.encodedSize);
    printf("  uint64_t array: %zu bytes\n", count * 8);
    printf("  Compression: %.1fx\n", (float)(count * 8) / meta.encodedSize);

    // Verify random access
    for (size_t i = 0; i < count; i++) {
        uint64_t value = varintFORGetAt(encoded, i);
        assert(value == timestamps[i]);
    }

    free(encoded);
    printf("✓ Timestamp compression successful\n");
}

// Example 3: Sequential IDs
void example_sequential_ids() {
    printf("\n=== Example 3: Sequential ID Range ===\n");

    // User IDs in range 100000-100099
    uint64_t ids[100];
    for (size_t i = 0; i < 100; i++) {
        ids[i] = 100000 + i;
    }

    varintFORMeta meta;
    varintFORAnalyze(ids, 100, &meta);

    printf("100 sequential IDs (100000-100099):\n");
    printf("  Min: %" PRIu64 ", Max: %" PRIu64 ", Range: %" PRIu64 "\n",
           meta.minValue, meta.maxValue, meta.range);
    printf("  Offset width: %d byte(s)\n", meta.offsetWidth);

    uint8_t *encoded = malloc(meta.encodedSize);
    CHECK_ALLOC(encoded);
    varintFOREncode(encoded, ids, 100, &meta);

    printf("Storage comparison:\n");
    printf("  FOR: %zu bytes\n", meta.encodedSize);
    printf("  uint64_t: 800 bytes\n");
    printf("  uint32_t: 400 bytes\n");
    printf("  Savings vs uint64_t: %.1f%%\n",
           ((800.0 - meta.encodedSize) / 800.0) * 100);

    // Verify a few random accesses
    assert(varintFORGetAt(encoded, 0) == 100000);
    assert(varintFORGetAt(encoded, 50) == 100050);
    assert(varintFORGetAt(encoded, 99) == 100099);

    free(encoded);
    printf("✓ Sequential IDs compressed efficiently\n");
}

// Example 4: Prices in similar range
void example_prices() {
    printf("\n=== Example 4: Price Data (cents) ===\n");

    // Product prices in cents (e.g., $9.99 to $99.99)
    uint64_t prices[] = {
        999,  1499, 1999, 2499, 2999, // $9.99 - $29.99
        3499, 3999, 4499, 4999, 5499, // $34.99 - $54.99
        5999, 6499, 6999, 7499, 7999, // $59.99 - $79.99
        8499, 8999, 9499, 9999,       // $84.99 - $99.99
    };
    size_t count = sizeof(prices) / sizeof(prices[0]);

    varintFORMeta meta;
    varintFORAnalyze(prices, count, &meta);

    printf("Price range: $%.2f - $%.2f\n", meta.minValue / 100.0,
           meta.maxValue / 100.0);
    printf("Range in cents: %" PRIu64 " (fits in %d byte%s)\n", meta.range,
           meta.offsetWidth, meta.offsetWidth == 1 ? "" : "s");

    uint8_t *encoded = malloc(meta.encodedSize);
    CHECK_ALLOC(encoded);
    varintFOREncode(encoded, prices, count, &meta);

    printf("Storage: %zu bytes vs %zu bytes (uint64_t)\n", meta.encodedSize,
           count * 8);
    printf("Efficiency: %.1f%%\n",
           ((float)(count * 8 - meta.encodedSize) / (count * 8)) * 100);

    // Decode and verify
    uint64_t *decoded = malloc(count * sizeof(uint64_t));
    CHECK_ALLOC(encoded);
    varintFORDecode(encoded, decoded, count);

    printf("Sample decoded prices: ");
    for (size_t i = 0; i < 5; i++) {
        assert(decoded[i] == prices[i]);
        printf("$%.2f ", decoded[i] / 100.0);
    }
    printf("...\n");

    free(encoded);
    free(decoded);
    printf("✓ Price compression successful\n");
}

// Example 5: Random access pattern
void example_random_access() {
    printf("\n=== Example 5: Random Access ===\n");

    uint64_t values[] = {5000, 5010, 5020, 5030, 5040,
                         5050, 5060, 5070, 5080, 5090};
    size_t count = sizeof(values) / sizeof(values[0]);

    varintFORMeta meta;
    varintFORAnalyze(values, count, &meta);

    uint8_t *encoded = malloc(meta.encodedSize);
    CHECK_ALLOC(encoded);
    varintFOREncode(encoded, values, count, &meta);

    printf("Encoded %zu values\n", count);
    printf("Random access tests:\n");

    // Access values in random order
    size_t indices[] = {9, 0, 5, 2, 7, 4};
    for (size_t i = 0; i < sizeof(indices) / sizeof(indices[0]); i++) {
        size_t idx = indices[i];
        uint64_t value = varintFORGetAt(encoded, idx);
        printf("  Index %zu: %" PRIu64 " ", idx, value);
        assert(value == values[idx]);
        printf("✓\n");
    }

    // Verify metadata extraction
    uint64_t minVal = varintFORGetMinValue(encoded);
    size_t storedCount = varintFORGetCount(encoded);
    varintWidth offsetWidth = varintFORGetOffsetWidth(encoded);

    assert(minVal == meta.minValue);
    assert(storedCount == count);
    assert(offsetWidth == meta.offsetWidth);

    printf("Metadata extraction:\n");
    printf("  Min: %" PRIu64 " ✓\n", minVal);
    printf("  Count: %zu ✓\n", storedCount);
    printf("  Offset width: %d ✓\n", offsetWidth);

    free(encoded);
    printf("✓ Random access working correctly\n");
}

// Example 6: Edge cases and boundaries
void example_edge_cases() {
    printf("\n=== Example 6: Edge Cases ===\n");

    // Test 1: Single value
    printf("Test 1: Single value\n");
    const uint64_t single[] = {42};
    varintFORMeta meta1;
    varintFORAnalyze(single, 1, &meta1);
    uint8_t *enc1 = malloc(meta1.encodedSize);
    CHECK_ALLOC(enc1);
    varintFOREncode(enc1, single, 1, &meta1);
    assert(varintFORGetAt(enc1, 0) == 42);
    printf("  Single value (42): range=%" PRIu64 ", width=%d ✓\n", meta1.range,
           meta1.offsetWidth);
    free(enc1);

    // Test 2: All same values
    printf("Test 2: All same values\n");
    uint64_t same[10];
    for (int i = 0; i < 10; i++) {
        same[i] = 1000;
    }
    varintFORMeta meta2;
    varintFORAnalyze(same, 10, &meta2);
    assert(meta2.range == 0);
    assert(meta2.offsetWidth == 1); // Even 0 range uses 1 byte
    printf("  All 1000: range=%" PRIu64 ", width=%d ✓\n", meta2.range,
           meta2.offsetWidth);

    // Test 3: Maximum range (requires 8 bytes)
    printf("Test 3: Large range\n");
    const uint64_t large[] = {0, UINT64_MAX};
    varintFORMeta meta3;
    varintFORAnalyze(large, 2, &meta3);
    assert(meta3.offsetWidth == 8);
    printf("  Range 0 to MAX: width=%d ✓\n", meta3.offsetWidth);

    // Test 4: Powers of 2 boundaries
    printf("Test 4: Width boundaries\n");
    struct {
        uint64_t min;
        uint64_t max;
        varintWidth expectedWidth;
    } tests[] = {
        {0, 255, 1},        // 1 byte
        {0, 256, 2},        // 2 bytes
        {0, 65535, 2},      // 2 bytes
        {0, 65536, 3},      // 3 bytes
        {0, 16777215UL, 3}, // 3 bytes
        {0, 16777216UL, 4}, // 4 bytes
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        const uint64_t vals[] = {tests[i].min, tests[i].max};
        varintFORMeta meta;
        varintFORAnalyze(vals, 2, &meta);
        assert(meta.offsetWidth == tests[i].expectedWidth);
        printf("  Range %" PRIu64 ": %d bytes ✓\n", tests[i].max - tests[i].min,
               meta.offsetWidth);
    }

    printf("✓ All edge cases handled correctly\n");
}

// Example 7: Performance and compression analysis
void example_performance() {
    printf("\n=== Example 7: Compression Analysis ===\n");

    struct {
        const char *name;
        uint64_t *values;
        size_t count;
    } datasets[4];

    // Dataset 1: Tight cluster (1 byte offsets)
    datasets[0].count = 100;
    datasets[0].values = malloc(datasets[0].count * sizeof(uint64_t));
    CHECK_ALLOC(datasets[0].values);
    datasets[0].name = "Tight cluster (range 100)";
    for (size_t i = 0; i < datasets[0].count; i++) {
        datasets[0].values[i] = 1000000 + (i % 100);
    }

    // Dataset 2: Medium cluster (2 byte offsets)
    datasets[1].count = 100;
    datasets[1].values = malloc(datasets[1].count * sizeof(uint64_t));
    CHECK_ALLOC(datasets[1].values);
    datasets[1].name = "Medium cluster (range 10000)";
    for (size_t i = 0; i < datasets[1].count; i++) {
        datasets[1].values[i] = 1000000 + (i * 100);
    }

    // Dataset 3: Wide cluster (4 byte offsets)
    datasets[2].count = 100;
    datasets[2].values = malloc(datasets[2].count * sizeof(uint64_t));
    CHECK_ALLOC(datasets[2].values);
    datasets[2].name = "Wide cluster (range 1000000)";
    for (size_t i = 0; i < datasets[2].count; i++) {
        datasets[2].values[i] = 1000000 + (i * 10000);
    }

    // Dataset 4: Sparse (8 byte offsets)
    datasets[3].count = 10;
    datasets[3].values = malloc(datasets[3].count * sizeof(uint64_t));
    CHECK_ALLOC(datasets[3].values);
    datasets[3].name = "Sparse (large range)";
    for (size_t i = 0; i < datasets[3].count; i++) {
        datasets[3].values[i] = i * 1000000000UL;
    }

    printf("%-30s | Count | Width | FOR Size | u64 Size | Compression\n",
           "Dataset");
    printf("-------------------------------|-------|-------|----------|--------"
           "--|------------\n");

    for (int d = 0; d < 4; d++) {
        varintFORMeta meta;
        varintFORAnalyze(datasets[d].values, datasets[d].count, &meta);

        size_t u64Size = datasets[d].count * 8;
        float ratio = (float)u64Size / meta.encodedSize;

        printf("%-30s | %5zu | %5d | %8zu | %8zu | %6.2fx\n", datasets[d].name,
               datasets[d].count, meta.offsetWidth, meta.encodedSize, u64Size,
               ratio);

        free(datasets[d].values);
    }

    printf("\nKey insight: FOR encoding is most efficient when:\n");
    printf("  - Values are clustered in a narrow range\n");
    printf("  - The dataset is large (amortizes header overhead)\n");
    printf("  - Random access is needed (unlike delta encoding)\n");

    printf("✓ Compression analysis complete\n");
}

int main() {
    printf("===========================================\n");
    printf("    varintFOR Example Suite\n");
    printf("===========================================\n");
    printf("\nFrame-of-Reference (FOR) encoding stores values as\n");
    printf("fixed-width offsets from a minimum value.\n");
    printf("Perfect for clustered data: timestamps, IDs, prices.\n");

    example_basic();
    example_timestamps();
    example_sequential_ids();
    example_prices();
    example_random_access();
    example_edge_cases();
    example_performance();

    printf("\n===========================================\n");
    printf("All examples completed successfully!\n");
    printf("===========================================\n");

    return 0;
}
