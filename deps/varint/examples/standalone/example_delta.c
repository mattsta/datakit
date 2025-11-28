/**
 * example_delta.c - Demonstrates varintDelta usage
 *
 * varintDelta provides delta encoding with ZigZag for signed deltas.
 * Perfect for sorted arrays, time series, and sequential data.
 * Achieves 70-90% compression on typical sorted datasets.
 *
 * Compile: gcc -I../../src example_delta.c ../../src/varintDelta.c
 * ../../src/varintExternal.c -o example_delta Run: ./example_delta
 */

#include "varintDelta.h"
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

/* Example 1: Basic delta encoding with sorted array */
void example_basic() {
    printf("\n=== Example 1: Basic Delta Encoding ===\n");

    /* Sorted array of document IDs */
    int64_t docIds[] = {100, 102, 103, 105, 110, 115, 120};
    size_t count = sizeof(docIds) / sizeof(docIds[0]);

    /* Allocate output buffer */
    size_t maxSize = varintDeltaMaxEncodedSize(count);
    uint8_t *encoded = malloc(maxSize);
    CHECK_ALLOC(encoded);

    /* Encode as base + deltas */
    size_t encodedSize = varintDeltaEncode(encoded, docIds, count);

    printf("Original values: ");
    for (size_t i = 0; i < count; i++) {
        printf("%" PRId64 " ", docIds[i]);
    }
    printf("\n");

    /* Show deltas */
    printf("Deltas: base=%" PRId64 ", ", docIds[0]);
    for (size_t i = 1; i < count; i++) {
        printf("%+" PRId64 " ", docIds[i] - docIds[i - 1]);
    }
    printf("\n");

    printf("Encoded size: %zu bytes (vs %zu uncompressed)\n", encodedSize,
           count * sizeof(int64_t));
    printf("Compression: %.1f%%\n",
           (1.0 - (double)encodedSize / (count * sizeof(int64_t))) * 100);

    /* Decode and verify */
    int64_t *decoded = malloc(count * sizeof(int64_t));
    CHECK_ALLOC(decoded);
    size_t decodedBytes = varintDeltaDecode(encoded, count, decoded);

    assert(decodedBytes == encodedSize);
    for (size_t i = 0; i < count; i++) {
        assert(decoded[i] == docIds[i]);
    }

    printf("Decoded values: ");
    for (size_t i = 0; i < count; i++) {
        printf("%" PRId64 " ", decoded[i]);
    }
    printf("\n✓ Round-trip successful\n");

    free(encoded);
    free(decoded);
}

/* Example 2: Time series data with timestamps */
void example_time_series() {
    printf("\n=== Example 2: Time Series Timestamps ===\n");

    /* Unix timestamps (seconds since epoch) - typically increment by small
     * amounts */
    int64_t timestamps[] = {
        1700000000, /* 2023-11-14 22:13:20 UTC */
        1700000060, /* +60 seconds */
        1700000120, /* +60 seconds */
        1700000180, /* +60 seconds */
        1700000240, /* +60 seconds */
        1700000300, /* +60 seconds */
        1700000360, /* +60 seconds */
        1700000420, /* +60 seconds */
        1700000480, /* +60 seconds */
        1700000540, /* +60 seconds */
    };
    size_t count = sizeof(timestamps) / sizeof(timestamps[0]);

    /* Encode */
    size_t maxSize = varintDeltaMaxEncodedSize(count);
    uint8_t *encoded = malloc(maxSize);
    CHECK_ALLOC(encoded);
    size_t encodedSize = varintDeltaEncode(encoded, timestamps, count);

    printf("Timestamps: %zu values\n", count);
    printf("First: %" PRId64 ", Last: %" PRId64 "\n", timestamps[0],
           timestamps[count - 1]);
    printf("Delta: +60 seconds each\n");

    size_t uncompressedSize = count * sizeof(int64_t);
    printf("\nSize comparison:\n");
    printf("  Uncompressed: %zu bytes (%zu × 8)\n", uncompressedSize, count);
    printf("  Delta encoded: %zu bytes\n", encodedSize);
    printf("  Compression: %.1f%%\n",
           (1.0 - (double)encodedSize / uncompressedSize) * 100);

    /* Decode and verify */
    int64_t *decoded = malloc(count * sizeof(int64_t));
    CHECK_ALLOC(decoded);
    varintDeltaDecode(encoded, count, decoded);

    for (size_t i = 0; i < count; i++) {
        assert(decoded[i] == timestamps[i]);
    }

    printf("✓ Time series encoded efficiently\n");

    free(encoded);
    free(decoded);
}

/* Example 3: ZigZag encoding demonstration */
void example_zigzag() {
    printf("\n=== Example 3: ZigZag Encoding ===\n");

    /* Test values showing ZigZag mapping */
    int64_t testValues[] = {0, -1, 1, -2, 2, -3, 3, -100, 100, -1000, 1000};
    size_t count = sizeof(testValues) / sizeof(testValues[0]);

    printf("ZigZag mapping (signed → unsigned):\n");
    printf("Signed    | ZigZag    | Binary Pattern\n");
    printf("----------|-----------|------------------\n");

    for (size_t i = 0; i < count; i++) {
        int64_t signed_val = testValues[i];
        uint64_t zigzag = varintDeltaZigZag(signed_val);
        int64_t decoded = varintDeltaZigZagDecode(zigzag);

        printf("%9" PRId64 " | %9" PRIu64 " | ", signed_val, zigzag);

        /* Print binary pattern of ZigZag value (low 16 bits) */
        for (int b = 15; b >= 0; b--) {
            printf("%d", (int)((zigzag >> b) & 1));
            if (b % 4 == 0 && b > 0) {
                printf(" ");
            }
        }
        printf("\n");

        if (decoded != signed_val) {
            printf("ERROR: ZigZag decode failed! signed=%" PRId64
                   ", zigzag=%" PRIu64 ", decoded=%" PRId64 "\n",
                   signed_val, zigzag, decoded);
        }
        assert(decoded == signed_val);
    }

    printf("\n✓ ZigZag encoding preserves values\n");
}

/* Example 4: Mixed positive and negative deltas */
void example_mixed_deltas() {
    printf("\n=== Example 4: Mixed Positive/Negative Deltas ===\n");

    /* Stock prices (in cents) - goes up and down */
    int64_t prices[] = {10000, 10050, 10025, 10100, 10075, 10200, 10150};
    size_t count = sizeof(prices) / sizeof(prices[0]);

    printf("Stock prices (cents): ");
    for (size_t i = 0; i < count; i++) {
        printf("%" PRId64 " ", prices[i]);
    }
    printf("\n");

    printf("Deltas: ");
    for (size_t i = 1; i < count; i++) {
        printf("%+" PRId64 " ", prices[i] - prices[i - 1]);
    }
    printf("\n");

    /* Encode */
    size_t maxSize = varintDeltaMaxEncodedSize(count);
    uint8_t *encoded = malloc(maxSize);
    CHECK_ALLOC(encoded);
    size_t encodedSize = varintDeltaEncode(encoded, prices, count);

    printf("Encoded: %zu bytes (vs %zu uncompressed)\n", encodedSize,
           count * sizeof(int64_t));

    /* Decode and verify */
    int64_t *decoded = malloc(count * sizeof(int64_t));
    CHECK_ALLOC(decoded);
    varintDeltaDecode(encoded, count, decoded);

    for (size_t i = 0; i < count; i++) {
        assert(decoded[i] == prices[i]);
    }

    printf("✓ Negative deltas handled correctly\n");

    free(encoded);
    free(decoded);
}

/* Example 5: Sorted array compression comparison */
void example_sorted_compression() {
    printf("\n=== Example 5: Sorted Array Compression ===\n");

    /* Generate sorted array: 1, 2, 3, ..., 1000 */
    size_t count = 1000;
    int64_t *sorted = malloc(count * sizeof(int64_t));
    CHECK_ALLOC(sorted);
    for (size_t i = 0; i < count; i++) {
        sorted[i] = (int64_t)(i + 1);
    }

    /* Encode */
    size_t maxSize = varintDeltaMaxEncodedSize(count);
    uint8_t *encoded = malloc(maxSize);
    CHECK_ALLOC(encoded);
    size_t encodedSize = varintDeltaEncode(encoded, sorted, count);

    size_t uncompressedSize = count * sizeof(int64_t);

    printf("Array: 1, 2, 3, ..., %zu\n", count);
    printf("Count: %zu values\n", count);
    printf("Deltas: all +1\n\n");

    printf("Size comparison:\n");
    printf("  Uncompressed: %zu bytes\n", uncompressedSize);
    printf("  Delta encoded: %zu bytes\n", encodedSize);
    printf("  Bytes per value: %.2f\n", (double)encodedSize / count);
    printf("  Compression ratio: %.1fx\n",
           (double)uncompressedSize / encodedSize);
    printf("  Space savings: %.1f%%\n",
           (1.0 - (double)encodedSize / uncompressedSize) * 100);

    /* Decode and verify sample values */
    int64_t *decoded = malloc(count * sizeof(int64_t));
    CHECK_ALLOC(decoded);
    varintDeltaDecode(encoded, count, decoded);

    assert(decoded[0] == 1);
    assert(decoded[count - 1] == (int64_t)count);
    assert(decoded[500] == 501);

    printf("✓ High compression for sorted sequential data\n");

    free(sorted);
    free(encoded);
    free(decoded);
}

/* Example 6: Unsigned values with delta encoding */
void example_unsigned() {
    printf("\n=== Example 6: Unsigned Delta Encoding ===\n");

    /* Array of increasing unsigned IDs */
    uint64_t userIds[] = {1000, 1005, 1010, 1008, 1020, 1025};
    size_t count = sizeof(userIds) / sizeof(userIds[0]);

    printf("User IDs: ");
    for (size_t i = 0; i < count; i++) {
        printf("%" PRIu64 " ", userIds[i]);
    }
    printf("\n");

    /* Encode */
    size_t maxSize = varintDeltaMaxEncodedSize(count);
    uint8_t *encoded = malloc(maxSize);
    CHECK_ALLOC(encoded);
    size_t encodedSize = varintDeltaEncodeUnsigned(encoded, userIds, count);

    printf("Encoded: %zu bytes (vs %zu uncompressed)\n", encodedSize,
           count * sizeof(uint64_t));

    /* Decode and verify */
    uint64_t *decoded = malloc(count * sizeof(uint64_t));
    CHECK_ALLOC(decoded);
    varintDeltaDecodeUnsigned(encoded, count, decoded);

    for (size_t i = 0; i < count; i++) {
        assert(decoded[i] == userIds[i]);
    }

    printf("✓ Unsigned values encoded correctly\n");

    free(encoded);
    free(decoded);
}

/* Example 7: Space efficiency analysis */
void example_space_analysis() {
    printf("\n=== Example 7: Space Efficiency Analysis ===\n");

    struct {
        const char *description;
        int64_t *values;
        size_t count;
    } tests[4];

    /* Test 1: Tightly packed sequential (delta = 1) */
    size_t count1 = 100;
    int64_t *seq1 = malloc(count1 * sizeof(int64_t));
    CHECK_ALLOC(seq1);
    for (size_t i = 0; i < count1; i++) {
        seq1[i] = (int64_t)i;
    }
    tests[0].description = "Sequential (0,1,2,...)";
    tests[0].values = seq1;
    tests[0].count = count1;

    /* Test 2: Sparse sequential (delta = 10) */
    size_t count2 = 100;
    int64_t *seq2 = malloc(count2 * sizeof(int64_t));
    CHECK_ALLOC(seq2);
    for (size_t i = 0; i < count2; i++) {
        seq2[i] = (int64_t)(i * 10);
    }
    tests[1].description = "Sparse (0,10,20,...)";
    tests[1].values = seq2;
    tests[1].count = count2;

    /* Test 3: Large base, small deltas */
    size_t count3 = 100;
    int64_t *seq3 = malloc(count3 * sizeof(int64_t));
    CHECK_ALLOC(seq3);
    for (size_t i = 0; i < count3; i++) {
        seq3[i] = 1000000 + (int64_t)i;
    }
    tests[2].description = "Large base (1000000+)";
    tests[2].values = seq3;
    tests[2].count = count3;

    /* Test 4: Mixed deltas */
    size_t count4 = 100;
    int64_t *seq4 = malloc(count4 * sizeof(int64_t));
    CHECK_ALLOC(seq4);
    for (size_t i = 0; i < count4; i++) {
        seq4[i] = (int64_t)(i * 10 + (i % 2 ? -5 : 5));
    }
    tests[3].description = "Mixed deltas";
    tests[3].values = seq4;
    tests[3].count = count4;

    printf("Pattern                  | Count | Uncompressed | Delta | "
           "Bytes/Value | Savings\n");
    printf("-------------------------|-------|--------------|-------|----------"
           "---|--------\n");

    for (int t = 0; t < 4; t++) {
        size_t maxSize = varintDeltaMaxEncodedSize(tests[t].count);
        uint8_t *encoded = malloc(maxSize);
        CHECK_ALLOC(encoded);
        size_t encodedSize =
            varintDeltaEncode(encoded, tests[t].values, tests[t].count);
        size_t uncompressed = tests[t].count * sizeof(int64_t);

        printf("%-24s | %5zu | %12zu | %5zu | %11.2f | %6.1f%%\n",
               tests[t].description, tests[t].count, uncompressed, encodedSize,
               (double)encodedSize / tests[t].count,
               (1.0 - (double)encodedSize / uncompressed) * 100);

        free(encoded);
        free(tests[t].values);
    }
}

/* Example 8: Round-trip verification with edge cases */
void example_edge_cases() {
    printf("\n=== Example 8: Edge Cases ===\n");

    /* Test various edge cases */
    struct {
        const char *description;
        int64_t *values;
        size_t count;
    } tests[] = {
        {"Single value", (int64_t[]){42}, 1},
        {"Two values", (int64_t[]){10, 20}, 2},
        {"All zeros", (int64_t[]){0, 0, 0, 0, 0}, 5},
        {"All same", (int64_t[]){100, 100, 100, 100}, 4},
        {"Decreasing", (int64_t[]){100, 90, 80, 70, 60}, 5},
        {"Large values", (int64_t[]){INT64_MAX - 2, INT64_MAX - 1, INT64_MAX},
         3},
        {"Alternating", (int64_t[]){1, 2, 1, 2, 1, 2}, 6},
    };

    for (size_t t = 0; t < sizeof(tests) / sizeof(tests[0]); t++) {
        size_t maxSize = varintDeltaMaxEncodedSize(tests[t].count);
        uint8_t *encoded = malloc(maxSize);
        CHECK_ALLOC(encoded);
        size_t encodedSize =
            varintDeltaEncode(encoded, tests[t].values, tests[t].count);

        int64_t *decoded = malloc(tests[t].count * sizeof(int64_t));
        CHECK_ALLOC(decoded);
        varintDeltaDecode(encoded, tests[t].count, decoded);

        /* Verify */
        bool match = true;
        for (size_t i = 0; i < tests[t].count; i++) {
            if (decoded[i] != tests[t].values[i]) {
                match = false;
                break;
            }
        }

        printf("%-20s: %zu bytes ", tests[t].description, encodedSize);
        printf("%s\n", match ? "✓" : "✗ FAILED");

        assert(match);

        free(encoded);
        free(decoded);
    }

    printf("✓ All edge cases handled correctly\n");
}

int main() {
    printf("===========================================\n");
    printf("     varintDelta Example Suite\n");
    printf("===========================================\n");
    printf("Delta encoding with ZigZag for signed deltas\n");

    example_basic();
    example_time_series();
    example_zigzag();
    example_mixed_deltas();
    example_sorted_compression();
    example_unsigned();
    example_space_analysis();
    example_edge_cases();

    printf("\n===========================================\n");
    printf("All examples completed successfully!\n");
    printf("===========================================\n");

    return 0;
}
