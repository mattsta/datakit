/**
 * example_pfor.c - Demonstrates varintPFOR (Patched Frame-of-Reference) usage
 *
 * varintPFOR provides exceptional compression for clustered data with outliers.
 * Perfect for stock prices, response times, network latency, and sensor data.
 * Supports random access and configurable exception thresholds.
 *
 * Compile: gcc -I../../src example_pfor.c ../../src/varintPFOR.c
 * ../../src/varintTagged.c ../../src/varintExternal.c -o example_pfor Run:
 * ./example_pfor
 */

#include "varintPFOR.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Example 1: Basic PFOR encoding and decoding */
void example_basic() {
    printf("\n=== Example 1: Basic PFOR Encode/Decode ===\n");

    /* Data: mostly clustered around 100-110, with one outlier */
    uint64_t values[] = {100, 102, 105, 103, 500, 108, 107, 101};
    uint32_t count = sizeof(values) / sizeof(values[0]);

    printf("Original values: ");
    for (uint32_t i = 0; i < count; i++) {
        printf("%" PRIu64 " ", values[i]);
    }
    printf("\n");

    /* Encode */
    varintPFORMeta meta;
    uint8_t buffer[256];
    size_t encodedSize = varintPFOREncode(buffer, values, count,
                                          VARINT_PFOR_THRESHOLD_95, &meta);

    printf("Encoded in %zu bytes\n", encodedSize);
    printf("Metadata: min=%" PRIu64 ", width=%d, exceptions=%u\n", meta.min,
           meta.width, meta.exceptionCount);

    /* Decode */
    uint64_t decoded[8];
    varintPFORMeta decodeMeta = {0};
    uint32_t decodedCount = varintPFORDecode(buffer, decoded, &decodeMeta);

    printf("Decoded %u values: ", decodedCount);
    for (uint32_t i = 0; i < decodedCount; i++) {
        printf("%" PRIu64 " ", decoded[i]);
    }
    printf("\n");

    /* Verify */
    assert(decodedCount == count);
    for (uint32_t i = 0; i < count; i++) {
        assert(values[i] == decoded[i]);
    }

    /* Calculate compression ratio */
    size_t uncompressedSize = count * sizeof(uint64_t);
    printf("Compression: %zu bytes -> %zu bytes (%.1f%% savings)\n",
           uncompressedSize, encodedSize,
           ((float)(uncompressedSize - encodedSize) / uncompressedSize) * 100);

    printf("✓ Basic PFOR round-trip successful\n");
}

/* Example 2: Stock prices with rare spikes */
void example_stock_prices() {
    printf("\n=== Example 2: Stock Prices ===\n");

    /* Simulated stock prices: mostly $100-$105, rare spike to $150 */
    uint64_t prices[] = {10050, 10075, 10100, 10090, 10110, 10095, 10105,
                         10088, 10092, 10098, 15000, 10102, 10097, 10091,
                         10099, 10103, 10096, 10094, 10101, 10089};
    uint32_t count = sizeof(prices) / sizeof(prices[0]);

    printf("Stock prices (cents): ");
    for (uint32_t i = 0; i < count && i < 10; i++) {
        printf("%" PRIu64 " ", prices[i]);
    }
    printf("... (%u total)\n", count);

    /* Encode with 95th percentile threshold */
    varintPFORMeta meta;
    uint8_t *buffer = malloc(1024);
    size_t encodedSize = varintPFOREncode(buffer, prices, count,
                                          VARINT_PFOR_THRESHOLD_95, &meta);

    printf("Encoded in %zu bytes (min=%" PRIu64 ", width=%d bytes)\n",
           encodedSize, meta.min, meta.width);
    printf("Exceptions: %u out of %u values (%.1f%%)\n", meta.exceptionCount,
           count, ((float)meta.exceptionCount / count) * 100);

    /* Decode and verify */
    uint64_t *decoded = malloc(count * sizeof(uint64_t));
    if (!decoded) {
        fprintf(stderr, "Memory allocation failed\n");
        free(buffer);
        return;
    }
    varintPFORMeta decodeMeta = {0};
    varintPFORDecode(buffer, decoded, &decodeMeta);

    for (uint32_t i = 0; i < count; i++) {
        assert(prices[i] == decoded[i]);
    }

    /* Space comparison */
    size_t uint64Size = count * sizeof(uint64_t);
    printf("Space: %zu bytes (vs %zu with uint64_t)\n", encodedSize,
           uint64Size);
    printf("Savings: %.1f%%\n",
           ((float)(uint64Size - encodedSize) / uint64Size) * 100);

    printf("✓ Stock price compression successful\n");

    free(buffer);
    free(decoded);
}

/* Example 3: Response times (mostly fast, rare slow) */
void example_response_times() {
    printf("\n=== Example 3: HTTP Response Times ===\n");

    /* Response times in microseconds: mostly 50-100us, few slow outliers */
    uint64_t responseTimes[] = {
        52, 48, 61, 55, 58, 63, 51, 59, 54, 62,    5000, /* timeout */
        56, 60, 53, 57, 49, 64, 58, 52, 61, 55,    50,
        59, 62, 54, 58, 51, 63, 57, 60, 53, 12000, /* slow query */
        56, 61, 54, 59, 52, 58, 63, 55, 60, 57};
    uint32_t count = sizeof(responseTimes) / sizeof(responseTimes[0]);

    printf("Response times (us): %u samples\n", count);

    /* Compare different thresholds */
    const uint32_t thresholds[] = {VARINT_PFOR_THRESHOLD_90,
                                   VARINT_PFOR_THRESHOLD_95,
                                   VARINT_PFOR_THRESHOLD_99};
    const char *thresholdNames[] = {"90th", "95th", "99th"};

    printf("\nThreshold | Width | Exceptions | Size\n");
    printf("----------|-------|------------|------\n");

    for (size_t t = 0; t < 3; t++) {
        varintPFORMeta meta;
        uint8_t buffer[512];
        size_t size = varintPFOREncode(buffer, responseTimes, count,
                                       thresholds[t], &meta);

        printf("%-9s | %d     | %-10u | %zu\n", thresholdNames[t], meta.width,
               meta.exceptionCount, size);

        /* Verify decoding */
        uint64_t decoded[sizeof(responseTimes) / sizeof(responseTimes[0])];
        varintPFORMeta decodeMeta = {0};
        varintPFORDecode(buffer, decoded, &decodeMeta);

        for (uint32_t i = 0; i < count; i++) {
            assert(responseTimes[i] == decoded[i]);
        }
    }

    printf("✓ Response time encoding with multiple thresholds successful\n");
}

/* Example 4: Random access without full decode */
void example_random_access() {
    printf("\n=== Example 4: Random Access ===\n");

    /* Sensor readings: mostly 20-25°C, occasional spikes */
    uint64_t temperatures[] = {
        20, 21, 22, 21, 23, 22, 24, 21, 22, 23, 45, /* heater turned on */
        22, 21, 23, 22, 24, 23, 21, 22, 20, 23, 22,
        24, 21, 23, 22, 21, 23, 22, 24, 21};
    uint32_t count = sizeof(temperatures) / sizeof(temperatures[0]);

    /* Encode */
    varintPFORMeta meta;
    uint8_t buffer[256];
    varintPFOREncode(buffer, temperatures, count, VARINT_PFOR_THRESHOLD_95,
                     &meta);

    printf("Encoded %u temperature readings\n", count);
    printf("Random access test:\n");

    /* Test random access at various indices */
    uint32_t testIndices[] = {0, 10, 15, 29};
    for (size_t i = 0; i < sizeof(testIndices) / sizeof(testIndices[0]); i++) {
        uint32_t idx = testIndices[i];
        uint64_t value = varintPFORGetAt(buffer, idx, &meta);
        printf("  Index %2u: %" PRIu64 "°C ", idx, value);

        assert(value == temperatures[idx]);
        printf("✓\n");
    }

    printf("✓ Random access successful\n");
}

/* Example 5: Exception handling edge cases */
void example_exception_handling() {
    printf("\n=== Example 5: Exception Handling ===\n");

    /* Test 1: All values are exceptions (worst case) */
    printf("Test 1: All exceptions (scattered distribution)\n");
    uint64_t scattered[] = {1, 1000, 2000000, 50, 300000000UL};
    uint32_t scatteredCount = sizeof(scattered) / sizeof(scattered[0]);

    varintPFORMeta meta1;
    uint8_t buffer1[256];
    size_t size1 = varintPFOREncode(buffer1, scattered, scatteredCount,
                                    VARINT_PFOR_THRESHOLD_95, &meta1);

    printf("  Encoded %u scattered values in %zu bytes\n", scatteredCount,
           size1);
    printf("  Exceptions: %u (%.0f%%)\n", meta1.exceptionCount,
           ((float)meta1.exceptionCount / scatteredCount) * 100);

    uint64_t decoded1[5];
    varintPFORMeta decodeMeta1 = {0};
    varintPFORDecode(buffer1, decoded1, &decodeMeta1);

    for (uint32_t i = 0; i < scatteredCount; i++) {
        assert(scattered[i] == decoded1[i]);
    }
    printf("  ✓ Decoded correctly\n");

    /* Test 2: No exceptions (perfectly clustered) */
    printf("\nTest 2: No exceptions (perfectly clustered)\n");
    uint64_t clustered[] = {100, 101, 102, 103, 104, 105, 106, 107};
    uint32_t clusteredCount = sizeof(clustered) / sizeof(clustered[0]);

    varintPFORMeta meta2;
    uint8_t buffer2[128];
    size_t size2 = varintPFOREncode(buffer2, clustered, clusteredCount,
                                    VARINT_PFOR_THRESHOLD_95, &meta2);

    printf("  Encoded %u clustered values in %zu bytes\n", clusteredCount,
           size2);
    printf("  Exceptions: %u (%.0f%%)\n", meta2.exceptionCount,
           ((float)meta2.exceptionCount / clusteredCount) * 100);
    printf("  Width: %d byte(s)\n", meta2.width);

    uint64_t decoded2[8];
    varintPFORMeta decodeMeta2 = {0};
    varintPFORDecode(buffer2, decoded2, &decodeMeta2);

    for (uint32_t i = 0; i < clusteredCount; i++) {
        assert(clustered[i] == decoded2[i]);
    }

    /* Space efficiency for perfectly clustered data */
    size_t uint64Size = clusteredCount * sizeof(uint64_t);
    printf("  Space: %zu bytes (vs %zu with uint64_t, %.1f%% savings)\n", size2,
           uint64Size, ((float)(uint64Size - size2) / uint64Size) * 100);
    printf("  ✓ Decoded correctly\n");

    /* Test 3: Single value */
    printf("\nTest 3: Single value\n");
    const uint64_t single[] = {42};
    uint32_t singleCount = 1;

    varintPFORMeta meta3;
    uint8_t buffer3[32];
    size_t size3 = varintPFOREncode(buffer3, single, singleCount,
                                    VARINT_PFOR_THRESHOLD_95, &meta3);

    printf("  Encoded 1 value in %zu bytes\n", size3);

    uint64_t decoded3[1];
    varintPFORMeta decodeMeta3 = {0};
    varintPFORDecode(buffer3, decoded3, &decodeMeta3);

    assert(single[0] == decoded3[0]);
    printf("  ✓ Decoded correctly\n");

    printf("✓ All exception handling tests passed\n");
}

/* Example 6: Network latency monitoring */
void example_network_latency() {
    printf("\n=== Example 6: Network Latency Monitoring ===\n");

    /* Ping times in milliseconds: mostly 10-20ms, rare packet loss/timeout */
    uint64_t latencies[] = {
        12, 15, 11, 16, 13, 14,   17, 12, 15, 13, 11, 16, 14, 13, 15,
        12, 17, 14, 11, 16, 3000, /* packet loss/timeout */
        13, 15, 12, 14, 16, 11,   15, 13, 17, 12, 14, 16, 13, 15, 11,
        14, 12, 16, 15, 13, 12,   14, 17, 15, 11, 13, 16, 12, 14, 15};
    uint32_t count = sizeof(latencies) / sizeof(latencies[0]);

    printf("Monitoring %u ping samples\n", count);

    /* Encode */
    varintPFORMeta meta;
    uint8_t *buffer = malloc(512);
    size_t encodedSize = varintPFOREncode(buffer, latencies, count,
                                          VARINT_PFOR_THRESHOLD_95, &meta);

    printf("Encoded in %zu bytes\n", encodedSize);
    printf("Range: min=%" PRIu64 " ms, marker=%" PRIu64 "\n", meta.min,
           meta.exceptionMarker);
    printf("Frame width: %d byte(s)\n", meta.width);
    printf("Anomalies detected: %u (%.1f%%)\n", meta.exceptionCount,
           ((float)meta.exceptionCount / count) * 100);

    /* Decode and verify */
    uint64_t *decoded = malloc(count * sizeof(uint64_t));
    if (!decoded) {
        fprintf(stderr, "Memory allocation failed\n");
        free(buffer);
        return;
    }
    varintPFORMeta decodeMeta = {0};
    varintPFORDecode(buffer, decoded, &decodeMeta);

    for (uint32_t i = 0; i < count; i++) {
        assert(latencies[i] == decoded[i]);
    }

    /* Report compression efficiency */
    size_t uncompressedSize = count * sizeof(uint64_t);
    printf("Compression ratio: %.1fx\n", (float)uncompressedSize / encodedSize);

    printf("✓ Network latency encoding successful\n");

    free(buffer);
    free(decoded);
}

/* Example 7: Size calculation and pre-allocation */
void example_size_calculation() {
    printf("\n=== Example 7: Size Calculation ===\n");

    uint64_t values[] = {100, 105, 110, 108, 500, 102, 107, 103};
    uint32_t count = sizeof(values) / sizeof(values[0]);

    /* First pass: compute metadata */
    varintPFORMeta meta;
    varintPFORComputeThreshold(values, count, VARINT_PFOR_THRESHOLD_95, &meta);

    printf("Metadata computed:\n");
    printf("  Min: %" PRIu64 "\n", meta.min);
    printf("  Width: %d bytes\n", meta.width);
    printf("  Count: %u\n", meta.count);
    printf("  Exceptions: %u\n", meta.exceptionCount);

    /* Calculate required size */
    size_t requiredSize = varintPFORSize(&meta);
    printf("Required buffer size: %zu bytes\n", requiredSize);

    /* Allocate exact size and encode */
    uint8_t *buffer = malloc(requiredSize);
    size_t actualSize = varintPFOREncode(buffer, values, count,
                                         VARINT_PFOR_THRESHOLD_95, &meta);

    printf("Actual encoded size: %zu bytes\n", actualSize);
    assert(actualSize <= requiredSize);

    /* Decode and verify */
    uint64_t decoded[8];
    varintPFORMeta decodeMeta = {0};
    varintPFORDecode(buffer, decoded, &decodeMeta);

    for (uint32_t i = 0; i < count; i++) {
        assert(values[i] == decoded[i]);
    }

    printf("✓ Size calculation accurate\n");

    free(buffer);
}

/* Example 8: Comparison with uncompressed storage */
void example_space_analysis() {
    printf("\n=== Example 8: Space Efficiency Analysis ===\n");

    /* Various data patterns */
    struct {
        const char *name;
        uint64_t *values;
        uint32_t count;
    } datasets[] = {
        {"Tightly clustered (100-110)",
         (uint64_t[]){100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110},
         11},
        {"Mostly clustered + 1 outlier",
         (uint64_t[]){100, 101, 102, 103, 10000, 105, 106, 107, 108, 109}, 10},
        {"Mostly clustered + 3 outliers",
         (uint64_t[]){100, 101, 5000, 103, 104, 105, 10000, 107, 108, 15000},
         10},
        {"Wide distribution",
         (uint64_t[]){10, 1000, 100000, 50, 200, 5000, 300, 800, 10000, 150},
         10},
    };

    printf("Dataset                          | uint64 | PFOR | Savings\n");
    printf("---------------------------------|--------|------|--------\n");

    for (size_t d = 0; d < sizeof(datasets) / sizeof(datasets[0]); d++) {
        uint32_t count = datasets[d].count;
        size_t uint64Size = count * sizeof(uint64_t);

        varintPFORMeta meta;
        uint8_t buffer[256];
        size_t pforSize = varintPFOREncode(buffer, datasets[d].values, count,
                                           VARINT_PFOR_THRESHOLD_95, &meta);

        float savings = ((float)(uint64Size - pforSize) / uint64Size) * 100;

        printf("%-32s | %6zu | %4zu | %5.1f%%\n", datasets[d].name, uint64Size,
               pforSize, savings);

        /* Verify encoding */
        uint64_t decoded[11];
        varintPFORMeta decodeMeta = {0};
        varintPFORDecode(buffer, decoded, &decodeMeta);

        for (uint32_t i = 0; i < count; i++) {
            assert(datasets[d].values[i] == decoded[i]);
        }
    }

    printf("✓ Space analysis complete\n");
}

int main() {
    printf("============================================\n");
    printf("    varintPFOR Example Suite\n");
    printf("    Patched Frame-of-Reference Encoding\n");
    printf("============================================\n");

    example_basic();
    example_stock_prices();
    example_response_times();
    example_random_access();
    example_exception_handling();
    example_network_latency();
    example_size_calculation();
    example_space_analysis();

    printf("\n============================================\n");
    printf("All varintPFOR examples completed successfully!\n");
    printf("============================================\n");

    return 0;
}
