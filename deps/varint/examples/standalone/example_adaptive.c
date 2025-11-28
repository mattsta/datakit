/**
 * example_adaptive.c - Demonstrates varintAdaptive auto-selection
 *
 * varintAdaptive automatically analyzes data and selects optimal encoding:
 * - DELTA for sorted/sequential data
 * - FOR for clustered values
 * - PFOR for clustered with outliers
 * - DICT for highly repetitive data
 * - BITMAP for dense sets in 0-65535
 * - TAGGED for general purpose
 *
 * Compile: gcc -I../../src -fsanitize=address,undefined -g -O1 \
 *   example_adaptive.c \
 *   ../../src/varintAdaptive.c \
 *   ../../src/varintDelta.c ../../src/varintFOR.c ../../src/varintPFOR.c \
 *   ../../src/varintDict.c ../../src/varintBitmap.c ../../src/varintTagged.c \
 *   ../../src/varintExternal.c \
 *   -o example_adaptive
 *
 * Run: ./example_adaptive
 */

#include "varintAdaptive.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Helper to print data statistics */
void printStats(const char *name, const varintAdaptiveDataStats *stats) {
    printf("\n--- %s ---\n", name);
    printf("Count:         %zu values\n", stats->count);
    printf("Range:         %lu - %lu (range: %lu)\n", stats->minValue,
           stats->maxValue, stats->range);
    printf("Unique:        %zu (%.1f%%)\n", stats->uniqueCount,
           stats->uniqueRatio * 100);
    printf("Sorted:        %s\n", stats->isSorted          ? "Yes"
                                  : stats->isReverseSorted ? "Reverse"
                                                           : "No");
    printf("Avg Delta:     %lu\n", stats->avgDelta);
    printf("Max Delta:     %lu\n", stats->maxDelta);
    printf("Outliers:      %zu (%.1f%%)\n", stats->outlierCount,
           stats->outlierRatio * 100);
    printf("Bitmap Range:  %s\n", stats->fitsInBitmapRange ? "Yes" : "No");
}

/* Helper to print encoding results */
void printEncodingResult(const char *dataType, size_t originalCount,
                         size_t encodedSize,
                         varintAdaptiveEncodingType encodingType) {
    size_t originalSize = originalCount * sizeof(uint64_t);
    float ratio = (float)originalSize / (float)encodedSize;
    float savings = (1.0f - (float)encodedSize / (float)originalSize) * 100.0f;

    printf("\n[%s]\n", dataType);
    printf("  Selected Encoding: %s\n",
           varintAdaptiveEncodingName(encodingType));
    printf("  Original Size:     %zu bytes (%zu × 8)\n", originalSize,
           originalCount);
    printf("  Encoded Size:      %zu bytes\n", encodedSize);
    printf("  Compression Ratio: %.2fx\n", ratio);
    printf("  Space Savings:     %.1f%%\n", savings);
}

/* Example 1: Sorted timestamps (should select DELTA) */
void example_timestamps() {
    printf("\n========================================\n");
    printf("Example 1: Server Log Timestamps\n");
    printf("========================================\n");

    /* Unix timestamps from server logs - sequential with ~1 second intervals */
    uint64_t timestamps[] = {
        1700000000, 1700000001, 1700000002, 1700000004, 1700000005,
        1700000007, 1700000008, 1700000010, 1700000012, 1700000013,
        1700000015, 1700000017, 1700000019, 1700000020, 1700000022,
        1700000024, 1700000026, 1700000028, 1700000030, 1700000031,
    };
    size_t count = sizeof(timestamps) / sizeof(timestamps[0]);

    /* Analyze */
    varintAdaptiveDataStats stats;
    varintAdaptiveAnalyze(timestamps, count, &stats);
    printStats("Timestamp Data Analysis", &stats);

    /* Auto-encode */
    uint8_t *encoded = malloc(varintAdaptiveMaxSize(count));
    if (!encoded) {
        fprintf(stderr, "Failed to allocate memory for encoding\n");
        return;
    }
    varintAdaptiveMeta meta;
    size_t encodedSize =
        varintAdaptiveEncode(encoded, timestamps, count, &meta);

    printEncodingResult("Server Timestamps", count, encodedSize,
                        meta.encodingType);

    /* Decode and verify */
    uint64_t *decoded = malloc(count * sizeof(uint64_t));
    if (!decoded) {
        fprintf(stderr, "Failed to allocate memory for decoding\n");
        free(encoded);
        return;
    }
    size_t decodedCount = varintAdaptiveDecode(encoded, decoded, count, NULL);

    assert(decodedCount == count);
    for (size_t i = 0; i < count; i++) {
        assert(decoded[i] == timestamps[i]);
    }

    printf("  ✓ Verified: Round-trip successful\n");

    free(encoded);
    free(decoded);
}

/* Example 2: Highly repetitive status codes (should select DICT) */
void example_status_codes() {
    printf("\n========================================\n");
    printf("Example 2: HTTP Status Codes\n");
    printf("========================================\n");

    /* HTTP status codes - highly repetitive (only 5 unique values) */
    uint64_t statusCodes[] = {
        200, 200, 200, 200, 404, 200, 200, 500, 200, 200, 200, 200, 304,
        200, 200, 200, 200, 200, 404, 200, 200, 200, 200, 200, 200, 503,
        200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 304, 200, 200,
        200, 200, 200, 200, 404, 200, 200, 200, 200, 200, 200,
    };
    size_t count = sizeof(statusCodes) / sizeof(statusCodes[0]);

    /* Analyze */
    varintAdaptiveDataStats stats;
    varintAdaptiveAnalyze(statusCodes, count, &stats);
    printStats("Status Code Analysis", &stats);

    /* Auto-encode */
    uint8_t *encoded = malloc(varintAdaptiveMaxSize(count));
    if (!encoded) {
        fprintf(stderr, "Failed to allocate memory for encoding\n");
        return;
    }
    varintAdaptiveMeta meta;
    size_t encodedSize =
        varintAdaptiveEncode(encoded, statusCodes, count, &meta);

    printEncodingResult("HTTP Status Codes", count, encodedSize,
                        meta.encodingType);

    /* Decode and verify */
    uint64_t *decoded = malloc(count * sizeof(uint64_t));
    if (!decoded) {
        fprintf(stderr, "Failed to allocate memory for decoding\n");
        free(encoded);
        return;
    }
    size_t decodedCount = varintAdaptiveDecode(encoded, decoded, count, NULL);

    assert(decodedCount == count);
    for (size_t i = 0; i < count; i++) {
        assert(decoded[i] == statusCodes[i]);
    }

    printf("  ✓ Verified: Round-trip successful\n");

    free(encoded);
    free(decoded);
}

/* Example 3: Clustered user IDs (should select FOR) */
void example_user_ids() {
    printf("\n========================================\n");
    printf("Example 3: User IDs (Clustered)\n");
    printf("========================================\n");

    /* User IDs from recent signups - clustered around base value */
    uint64_t userIds[] = {
        500000, 500001, 500003, 500005, 500007, 500010, 500012, 500015,
        500018, 500020, 500023, 500025, 500028, 500030, 500033, 500036,
        500038, 500040, 500043, 500045, 500048, 500050, 500053, 500055,
    };
    size_t count = sizeof(userIds) / sizeof(userIds[0]);

    /* Analyze */
    varintAdaptiveDataStats stats;
    varintAdaptiveAnalyze(userIds, count, &stats);
    printStats("User ID Analysis", &stats);

    /* Auto-encode */
    uint8_t *encoded = malloc(varintAdaptiveMaxSize(count));
    if (!encoded) {
        fprintf(stderr, "Failed to allocate memory for encoding\n");
        return;
    }
    varintAdaptiveMeta meta;
    size_t encodedSize = varintAdaptiveEncode(encoded, userIds, count, &meta);

    printEncodingResult("User IDs", count, encodedSize, meta.encodingType);

    /* Decode and verify */
    uint64_t *decoded = malloc(count * sizeof(uint64_t));
    if (!decoded) {
        fprintf(stderr, "Failed to allocate memory for decoding\n");
        free(encoded);
        return;
    }
    size_t decodedCount = varintAdaptiveDecode(encoded, decoded, count, NULL);

    assert(decodedCount == count);
    for (size_t i = 0; i < count; i++) {
        assert(decoded[i] == userIds[i]);
    }

    printf("  ✓ Verified: Round-trip successful\n");

    free(encoded);
    free(decoded);
}

/* Example 4: Prices with outliers (should select PFOR) */
void example_prices() {
    printf("\n========================================\n");
    printf("Example 4: Product Prices (with outliers)\n");
    printf("========================================\n");

    /* Product prices in cents - mostly clustered 1000-5000, few outliers */
    uint64_t prices[] = {
        1999, 2499, 1599, 2999, 1899, 3499,   2199, 2799,
        1799, 2599, 1999, 3199, 2399, 1699,   2899, 2199,
        1899, 2499, 3299, 1999, 2699, 149999, // outlier: luxury item
        1799, 2399, 1999, 2599, 1899, 3099,   2199, 2799,
    };
    size_t count = sizeof(prices) / sizeof(prices[0]);

    /* Analyze */
    varintAdaptiveDataStats stats;
    varintAdaptiveAnalyze(prices, count, &stats);
    printStats("Price Analysis", &stats);

    /* Auto-encode */
    uint8_t *encoded = malloc(varintAdaptiveMaxSize(count));
    if (!encoded) {
        fprintf(stderr, "Failed to allocate memory for encoding\n");
        return;
    }
    varintAdaptiveMeta meta;
    size_t encodedSize = varintAdaptiveEncode(encoded, prices, count, &meta);

    printEncodingResult("Product Prices", count, encodedSize,
                        meta.encodingType);

    /* Decode and verify */
    uint64_t *decoded = malloc(count * sizeof(uint64_t));
    if (!decoded) {
        fprintf(stderr, "Failed to allocate memory for decoding\n");
        free(encoded);
        return;
    }
    size_t decodedCount = varintAdaptiveDecode(encoded, decoded, count, NULL);

    assert(decodedCount == count);
    for (size_t i = 0; i < count; i++) {
        assert(decoded[i] == prices[i]);
    }

    printf("  ✓ Verified: Round-trip successful\n");

    free(encoded);
    free(decoded);
}

/* Example 5: Sparse boolean flags (should select BITMAP) */
void example_flags() {
    printf("\n========================================\n");
    printf("Example 5: Feature Flags / Sparse Boolean\n");
    printf("========================================\n");

    /* Feature flag IDs that are enabled (sparse set in 0-1000 range) */
    uint64_t enabledFlags[] = {
        5,   12,  23,  45,  67,  89,  123, 156, 189, 234,
        267, 301, 345, 389, 423, 467, 501, 545, 589, 623,
    };
    size_t count = sizeof(enabledFlags) / sizeof(enabledFlags[0]);

    /* Analyze */
    varintAdaptiveDataStats stats;
    varintAdaptiveAnalyze(enabledFlags, count, &stats);
    printStats("Feature Flag Analysis", &stats);

    /* Auto-encode */
    uint8_t *encoded = malloc(varintAdaptiveMaxSize(count));
    if (!encoded) {
        fprintf(stderr, "Failed to allocate memory for encoding\n");
        return;
    }
    varintAdaptiveMeta meta;
    size_t encodedSize =
        varintAdaptiveEncode(encoded, enabledFlags, count, &meta);

    printEncodingResult("Feature Flags", count, encodedSize, meta.encodingType);

    /* Decode and verify */
    uint64_t *decoded = malloc(count * sizeof(uint64_t));
    if (!decoded) {
        fprintf(stderr, "Failed to allocate memory for decoding\n");
        free(encoded);
        return;
    }
    size_t decodedCount = varintAdaptiveDecode(encoded, decoded, count, NULL);

    assert(decodedCount == count);
    for (size_t i = 0; i < count; i++) {
        assert(decoded[i] == enabledFlags[i]);
    }

    printf("  ✓ Verified: Round-trip successful\n");

    free(encoded);
    free(decoded);
}

/* Example 6: Random data (should select TAGGED as fallback) */
void example_random() {
    printf("\n========================================\n");
    printf("Example 6: Random Data (Wide Range)\n");
    printf("========================================\n");

    /* Random values with wide range - no pattern */
    uint64_t randomData[] = {
        7234891234ULL,   123456789ULL,   98234567123ULL,  456789012ULL,
        234567890123ULL, 8901234567ULL,  345678901234ULL, 901234567890ULL,
        567890123456ULL, 12345678901ULL, 678901234567ULL, 23456789012ULL,
        789012345678ULL, 34567890123ULL, 890123456789ULL, 45678901234ULL,
    };
    size_t count = sizeof(randomData) / sizeof(randomData[0]);

    /* Analyze */
    varintAdaptiveDataStats stats;
    varintAdaptiveAnalyze(randomData, count, &stats);
    printStats("Random Data Analysis", &stats);

    /* Auto-encode */
    uint8_t *encoded = malloc(varintAdaptiveMaxSize(count));
    if (!encoded) {
        fprintf(stderr, "Failed to allocate memory for encoding\n");
        return;
    }
    varintAdaptiveMeta meta;
    size_t encodedSize =
        varintAdaptiveEncode(encoded, randomData, count, &meta);

    printEncodingResult("Random Data", count, encodedSize, meta.encodingType);

    /* Decode and verify */
    uint64_t *decoded = malloc(count * sizeof(uint64_t));
    if (!decoded) {
        fprintf(stderr, "Failed to allocate memory for decoding\n");
        free(encoded);
        return;
    }
    size_t decodedCount = varintAdaptiveDecode(encoded, decoded, count, NULL);

    assert(decodedCount == count);
    for (size_t i = 0; i < count; i++) {
        assert(decoded[i] == randomData[i]);
    }

    printf("  ✓ Verified: Round-trip successful\n");

    free(encoded);
    free(decoded);
}

/* Example 7: Incrementing counters (should select DELTA) */
void example_counters() {
    printf("\n========================================\n");
    printf("Example 7: Incrementing Counters\n");
    printf("========================================\n");

    /* Page view counters - always increasing */
    uint64_t counters[] = {
        1000, 1005, 1012, 1018, 1025, 1033, 1042, 1048, 1055, 1063, 1071, 1079,
        1088, 1095, 1103, 1112, 1120, 1129, 1137, 1145, 1154, 1162, 1171, 1179,
    };
    size_t count = sizeof(counters) / sizeof(counters[0]);

    /* Analyze */
    varintAdaptiveDataStats stats;
    varintAdaptiveAnalyze(counters, count, &stats);
    printStats("Counter Analysis", &stats);

    /* Auto-encode */
    uint8_t *encoded = malloc(varintAdaptiveMaxSize(count));
    if (!encoded) {
        fprintf(stderr, "Failed to allocate memory for encoding\n");
        return;
    }
    varintAdaptiveMeta meta;
    size_t encodedSize = varintAdaptiveEncode(encoded, counters, count, &meta);

    printEncodingResult("Page View Counters", count, encodedSize,
                        meta.encodingType);

    /* Decode and verify */
    uint64_t *decoded = malloc(count * sizeof(uint64_t));
    if (!decoded) {
        fprintf(stderr, "Failed to allocate memory for decoding\n");
        free(encoded);
        return;
    }
    size_t decodedCount = varintAdaptiveDecode(encoded, decoded, count, NULL);

    assert(decodedCount == count);
    for (size_t i = 0; i < count; i++) {
        assert(decoded[i] == counters[i]);
    }

    printf("  ✓ Verified: Round-trip successful\n");

    free(encoded);
    free(decoded);
}

/* Example 8: Comparison - Manual vs Auto selection */
void example_comparison() {
    printf("\n========================================\n");
    printf("Example 8: Manual vs Auto Comparison\n");
    printf("========================================\n");

    /* Test data - sorted IDs that work well with DELTA */
    uint64_t testData[] = {
        10000, 10002, 10005, 10008, 10012, 10015, 10019, 10023,
        10027, 10031, 10036, 10040, 10045, 10050, 10055, 10060,
    };
    size_t count = sizeof(testData) / sizeof(testData[0]);

    uint8_t *encoded = malloc(varintAdaptiveMaxSize(count));
    if (!encoded) {
        fprintf(stderr, "Failed to allocate memory for encoding\n");
        return;
    }
    varintAdaptiveMeta meta;

    printf("\nTesting different encodings on same data:\n");

    /* Try each encoding manually */
    const varintAdaptiveEncodingType encodings[] = {
        VARINT_ADAPTIVE_DELTA, VARINT_ADAPTIVE_FOR,    VARINT_ADAPTIVE_PFOR,
        VARINT_ADAPTIVE_DICT,  VARINT_ADAPTIVE_TAGGED,
    };

    size_t bestSize = SIZE_MAX;
    varintAdaptiveEncodingType bestEncoding = VARINT_ADAPTIVE_TAGGED;

    for (size_t i = 0; i < 5; i++) {
        size_t size = varintAdaptiveEncodeWith(encoded, testData, count,
                                               encodings[i], &meta);

        printf("  %-10s: %3zu bytes", varintAdaptiveEncodingName(encodings[i]),
               size);

        if (size < bestSize) {
            bestSize = size;
            bestEncoding = encodings[i];
            printf(" ← Best so far");
        }

        printf("\n");
    }

    /* Now try auto-selection */
    size_t autoSize = varintAdaptiveEncode(encoded, testData, count, &meta);

    printf("\n  Auto-select: %3zu bytes (%s)", autoSize,
           varintAdaptiveEncodingName(meta.encodingType));

    if (meta.encodingType == bestEncoding) {
        printf(" ✓ Optimal choice!\n");
    } else {
        printf(" (Best was %s)\n", varintAdaptiveEncodingName(bestEncoding));
    }

    free(encoded);
}

/* Example 9: Large dataset performance */
void example_large_dataset() {
    printf("\n========================================\n");
    printf("Example 9: Large Dataset (1000 values)\n");
    printf("========================================\n");

    /* Generate large sorted dataset with realistic characteristics */
    size_t count = 1000;
    uint64_t *largeData = malloc(count * sizeof(uint64_t));
    if (!largeData) {
        fprintf(stderr, "Failed to allocate memory for large dataset\n");
        return;
    }

    uint64_t base = 1700000000; // Unix timestamp base
    for (size_t i = 0; i < count; i++) {
        largeData[i] = base + (i * 60) + (i % 10); // ~1 minute intervals
    }

    /* Analyze */
    varintAdaptiveDataStats stats;
    varintAdaptiveAnalyze(largeData, count, &stats);
    printStats("Large Dataset Analysis", &stats);

    /* Encode */
    uint8_t *encoded = malloc(varintAdaptiveMaxSize(count));
    if (!encoded) {
        fprintf(stderr, "Failed to allocate memory for encoding\n");
        free(largeData);
        return;
    }
    varintAdaptiveMeta meta;
    size_t encodedSize = varintAdaptiveEncode(encoded, largeData, count, &meta);

    printEncodingResult("Large Dataset", count, encodedSize, meta.encodingType);

    /* Decode and verify */
    uint64_t *decoded = malloc(count * sizeof(uint64_t));
    if (!decoded) {
        fprintf(stderr, "Failed to allocate memory for decoding\n");
        free(largeData);
        free(encoded);
        return;
    }
    size_t decodedCount = varintAdaptiveDecode(encoded, decoded, count, NULL);

    assert(decodedCount == count);
    for (size_t i = 0; i < count; i++) {
        assert(decoded[i] == largeData[i]);
    }

    printf("  ✓ Verified: All 1000 values match\n");

    free(largeData);
    free(encoded);
    free(decoded);
}

/* Example 10: Mixed data patterns */
void example_mixed_patterns() {
    printf("\n========================================\n");
    printf("Example 10: Mixed Patterns\n");
    printf("========================================\n");

    printf("\nComparing adaptive encoding on different data patterns:\n\n");

    /* Pattern 1: Sequential */
    uint64_t sequential[20];
    for (size_t i = 0; i < 20; i++) {
        sequential[i] = 1000 + i;
    }

    /* Pattern 2: Constant */
    uint64_t constant[20];
    for (size_t i = 0; i < 20; i++) {
        constant[i] = 42;
    }

    /* Pattern 3: Powers of 2 */
    uint64_t powers[20];
    for (size_t i = 0; i < 20; i++) {
        powers[i] = 1ULL << i;
    }

    /* Pattern 4: Fibonacci-like */
    uint64_t fib[20];
    fib[0] = 1;
    fib[1] = 1;
    for (size_t i = 2; i < 20; i++) {
        fib[i] = fib[i - 1] + fib[i - 2];
    }

    uint8_t *encoded = malloc(varintAdaptiveMaxSize(20));
    if (!encoded) {
        fprintf(stderr, "Failed to allocate memory for encoding\n");
        return;
    }
    varintAdaptiveMeta meta;

    const char *names[] = {"Sequential", "Constant", "Powers-of-2",
                           "Fibonacci"};
    const uint64_t *datasets[] = {sequential, constant, powers, fib};

    for (int i = 0; i < 4; i++) {
        size_t size = varintAdaptiveEncode(encoded, datasets[i], 20, &meta);
        float ratio = varintAdaptiveCompressionRatio(20, size);

        printf("  %-12s: %3zu bytes, %s encoding, %.2fx compression\n",
               names[i], size, varintAdaptiveEncodingName(meta.encodingType),
               ratio);
    }

    free(encoded);
}

int main(void) {
    printf("╔══════════════════════════════════════╗\n");
    printf("║   Adaptive Varint Encoding Demo     ║\n");
    printf("║   Auto-selects optimal encoding      ║\n");
    printf("╚══════════════════════════════════════╝\n");

    example_timestamps();     // Should select DELTA
    example_status_codes();   // Should select DICT
    example_user_ids();       // Should select FOR
    example_prices();         // Should select PFOR or FOR
    example_flags();          // Should select BITMAP or FOR
    example_random();         // Should select TAGGED
    example_counters();       // Should select DELTA
    example_comparison();     // Manual vs Auto comparison
    example_large_dataset();  // Performance with 1000 values
    example_mixed_patterns(); // Different patterns side-by-side

    printf("\n========================================\n");
    printf("✓ All examples completed successfully!\n");
    printf("========================================\n");

    return 0;
}
