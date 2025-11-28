/**
 * example_dict.c - Demonstrates varintDict usage
 *
 * varintDict provides dictionary encoding for highly repetitive data.
 * Perfect for log sources, enum values, status codes, and categorical data
 * with low cardinality but high repetition.
 *
 * Compression efficiency:
 *   - Excellent: 10 unique values in 1M entries = 99%+ savings
 *   - Good: < 10% unique values = significant savings
 *   - Poor: > 50% unique values = may cause expansion
 *
 * Compile: gcc -I../../src example_dict.c ../../src/varintDict.c
 * ../../src/varintTagged.c ../../src/varintExternal.c -o example_dict Run:
 * ./example_dict
 */

#include "varintDict.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================
 * Example 1: Basic Dictionary Encoding
 * ==================================================================== */
void example_basic() {
    printf("\n=== Example 1: Basic Encode/Decode ===\n");

    /* Highly repetitive data: only 3 unique values */
    uint64_t values[] = {100, 200, 100, 300, 200, 100, 200, 100};
    size_t count = sizeof(values) / sizeof(values[0]);

    printf("Original values: ");
    for (size_t i = 0; i < count; i++) {
        printf("%" PRIu64 " ", values[i]);
    }
    printf("\n");

    /* Encode */
    uint8_t buffer[1024];
    size_t encodedSize = varintDictEncode(buffer, values, count);
    printf("Encoded size: %zu bytes\n", encodedSize);

    /* Decode */
    size_t decodedCount;
    uint64_t *decoded = varintDictDecode(buffer, encodedSize, &decodedCount);
    assert(decoded != NULL);
    assert(decodedCount == count);

    printf("Decoded values: ");
    for (size_t i = 0; i < decodedCount; i++) {
        printf("%" PRIu64 " ", decoded[i]);
        assert(decoded[i] == values[i]);
    }
    printf("\n");

    /* Calculate savings */
    size_t originalSize = count * sizeof(uint64_t);
    printf("Original size: %zu bytes\n", originalSize);
    printf("Savings: %.1f%%\n",
           ((float)(originalSize - encodedSize) / originalSize) * 100);

    free(decoded);
    printf("✓ Round-trip successful\n");
}

/* ====================================================================
 * Example 2: Log Source Codes
 * ==================================================================== */
void example_log_sources() {
    printf("\n=== Example 2: Log Source Codes ===\n");

    /* Simulate 100 log entries from only 5 different sources */
    const uint64_t KERNEL __attribute__((unused)) = 1;
    const uint64_t NETWORK = 2;
    const uint64_t DATABASE = 3;
    const uint64_t WEBSERVER = 4;
    const uint64_t AUTH = 5;

    uint64_t *logSources = (uint64_t *)malloc(100 * sizeof(uint64_t));
    if (!logSources) {
        fprintf(stderr, "Memory allocation failed\n");
        return;
    }
    for (int i = 0; i < 100; i++) {
        /* Distribute logs across sources (realistic pattern) */
        if (i % 10 < 4) {
            logSources[i] = WEBSERVER; /* 40% web logs */
        } else if (i % 10 < 7) {
            logSources[i] = DATABASE; /* 30% database logs */
        } else if (i % 10 < 9) {
            logSources[i] = NETWORK; /* 20% network logs */
        } else {
            logSources[i] = AUTH; /* 10% auth logs */
        }
    }

    printf("Log entries: 100\n");
    printf("Unique sources: 5\n");

    /* Get detailed statistics */
    varintDictStats stats;
    int result = varintDictGetStats(logSources, 100, &stats);
    assert(result == 0);

    printf("\nCompression Statistics:\n");
    printf("  Unique values: %zu\n", stats.uniqueCount);
    printf("  Dictionary bytes: %zu\n", stats.dictBytes);
    printf("  Index bytes: %zu\n", stats.indexBytes);
    printf("  Total encoded: %zu bytes\n", stats.totalBytes);
    printf("  Original size: %zu bytes\n", stats.originalBytes);
    printf("  Compression ratio: %.1fx\n", stats.compressionRatio);
    printf("  Space reduction: %.1f%%\n", stats.spaceReduction);

    /* Verify encoding works */
    uint8_t *buffer = (uint8_t *)malloc(1024);
    size_t encodedSize = varintDictEncode(buffer, logSources, 100);
    assert(encodedSize == stats.totalBytes);

    size_t decodedCount;
    uint64_t *decoded = varintDictDecode(buffer, encodedSize, &decodedCount);
    assert(decoded != NULL);
    assert(decodedCount == 100);

    /* Verify all values match */
    for (int i = 0; i < 100; i++) {
        assert(decoded[i] == logSources[i]);
    }

    free(decoded);
    free(buffer);
    free(logSources);
    printf("✓ Log source encoding highly efficient\n");
}

/* ====================================================================
 * Example 3: HTTP Status Codes
 * ==================================================================== */
void example_status_codes() {
    printf("\n=== Example 3: HTTP Status Codes ===\n");

    /* Simulate 1000 HTTP requests with common status codes */
    uint64_t statusCodes[] = {
        200, 200, 200, 200, 200, 200, 200, 404, /* Mostly 200 OK */
        200, 200, 200, 200, 304, 200, 200, 200,
        500, 200, 200, 200, 200, 403, 200, 200, /* Occasional errors */
        200, 200, 200, 200, 200, 200, 301, 200,
        200, 200, 404, 200, 200, 200, 200, 200,
    };
    size_t count = sizeof(statusCodes) / sizeof(statusCodes[0]);

    printf("HTTP responses: %zu\n", count);

    /* Build dictionary manually to inspect it */
    varintDict *dict = varintDictCreate();
    assert(dict != NULL);

    int result = varintDictBuild(dict, statusCodes, count);
    assert(result == 0);

    printf("Unique status codes: %u\n", dict->size);
    printf("Status code dictionary: ");
    for (uint32_t i = 0; i < dict->size; i++) {
        printf("%" PRIu64 " ", dict->values[i]);
    }
    printf("\n");
    printf("Index width: %d byte(s)\n", dict->indexWidth);

    /* Encode with pre-built dictionary */
    uint8_t buffer[1024];
    size_t encodedSize =
        varintDictEncodeWithDict(buffer, dict, statusCodes, count);
    printf("Encoded size: %zu bytes\n", encodedSize);

    size_t originalSize = count * sizeof(uint64_t);
    printf("Original size: %zu bytes\n", originalSize);
    printf("Compression: %.1fx (%.1f%% savings)\n",
           (float)originalSize / encodedSize,
           ((float)(originalSize - encodedSize) / originalSize) * 100);

    /* Verify decoding */
    size_t decodedCount;
    uint64_t *decoded = varintDictDecode(buffer, encodedSize, &decodedCount);
    assert(decoded != NULL);
    assert(decodedCount == count);

    for (size_t i = 0; i < count; i++) {
        assert(decoded[i] == statusCodes[i]);
    }

    free(decoded);
    varintDictFree(dict);
    printf("✓ Status code compression excellent\n");
}

/* ====================================================================
 * Example 4: Enum Values
 * ==================================================================== */
typedef enum {
    STATE_IDLE = 0,
    STATE_CONNECTING = 1,
    STATE_CONNECTED = 2,
    STATE_SENDING = 3,
    STATE_RECEIVING = 4,
    STATE_DISCONNECTING = 5,
    STATE_ERROR = 6,
} ConnectionState;

void example_enum_values() {
    printf("\n=== Example 4: Enum State Transitions ===\n");

    /* Simulate connection state transitions */
    uint64_t states[] = {
        STATE_IDLE,      STATE_CONNECTING,    STATE_CONNECTED, STATE_SENDING,
        STATE_RECEIVING, STATE_SENDING,       STATE_RECEIVING, STATE_SENDING,
        STATE_RECEIVING, STATE_DISCONNECTING, STATE_IDLE,      STATE_CONNECTING,
        STATE_CONNECTED, STATE_SENDING,       STATE_RECEIVING, STATE_ERROR,
        STATE_IDLE,      STATE_CONNECTING,    STATE_CONNECTED, STATE_SENDING,
    };
    size_t count = sizeof(states) / sizeof(states[0]);

    printf("State transitions: %zu\n", count);

    /* Encode */
    uint8_t buffer[512];
    size_t encodedSize = varintDictEncode(buffer, states, count);

    /* Calculate compression ratio */
    float ratio = varintDictCompressionRatio(states, count);
    printf("Compression ratio: %.1fx\n", ratio);

    /* Decode and verify */
    size_t decodedCount;
    uint64_t *decoded = varintDictDecode(buffer, encodedSize, &decodedCount);
    assert(decoded != NULL);
    assert(decodedCount == count);

    printf("Verifying state transitions... ");
    for (size_t i = 0; i < count; i++) {
        assert(decoded[i] == states[i]);
    }
    printf("✓\n");

    free(decoded);
    printf("✓ Enum encoding efficient\n");
}

/* ====================================================================
 * Example 5: Shared Dictionary Across Multiple Arrays
 * ==================================================================== */
void example_shared_dictionary() {
    printf("\n=== Example 5: Shared Dictionary ===\n");

    /* Multiple arrays with same value domain */
    uint64_t array1[] = {10, 20, 30, 10, 20, 10};
    uint64_t array2[] = {20, 30, 20, 10, 30, 20};
    uint64_t array3[] = {30, 10, 20, 30, 10, 20};

    size_t count1 = sizeof(array1) / sizeof(array1[0]);
    size_t count2 = sizeof(array2) / sizeof(array2[0]);
    size_t count3 = sizeof(array3) / sizeof(array3[0]);

    /* Build shared dictionary from all values */
    uint64_t allValues[18];
    memcpy(allValues, array1, sizeof(array1));
    memcpy(allValues + count1, array2, sizeof(array2));
    memcpy(allValues + count1 + count2, array3, sizeof(array3));

    varintDict *sharedDict = varintDictCreate();
    varintDictBuild(sharedDict, allValues, 18);

    printf("Shared dictionary has %u unique values\n", sharedDict->size);
    printf("Dictionary values: ");
    for (uint32_t i = 0; i < sharedDict->size; i++) {
        printf("%" PRIu64 " ", sharedDict->values[i]);
    }
    printf("\n");

    /* Encode each array with shared dictionary */
    uint8_t buffer1[256], buffer2[256], buffer3[256];
    size_t size1 =
        varintDictEncodeWithDict(buffer1, sharedDict, array1, count1);
    size_t size2 =
        varintDictEncodeWithDict(buffer2, sharedDict, array2, count2);
    size_t size3 =
        varintDictEncodeWithDict(buffer3, sharedDict, array3, count3);

    printf("Array 1 encoded: %zu bytes\n", size1);
    printf("Array 2 encoded: %zu bytes\n", size2);
    printf("Array 3 encoded: %zu bytes\n", size3);

    /* Note: In practice, you'd store the shared dictionary once and
     * only store the indices for each array, achieving even better
     * compression. This example shows encoding with the full dictionary
     * in each buffer for independence. */

    /* Verify all decodings */
    size_t dec1, dec2, dec3;
    uint64_t *out1 = varintDictDecode(buffer1, size1, &dec1);
    uint64_t *out2 = varintDictDecode(buffer2, size2, &dec2);
    uint64_t *out3 = varintDictDecode(buffer3, size3, &dec3);

    assert(out1 && out2 && out3);
    for (size_t i = 0; i < count1; i++) {
        assert(out1[i] == array1[i]);
    }
    for (size_t i = 0; i < count2; i++) {
        assert(out2[i] == array2[i]);
    }
    for (size_t i = 0; i < count3; i++) {
        assert(out3[i] == array3[i]);
    }

    free(out1);
    free(out2);
    free(out3);
    varintDictFree(sharedDict);
    printf("✓ Shared dictionary works across multiple arrays\n");
}

/* ====================================================================
 * Example 6: Dictionary Lookup
 * ==================================================================== */
void example_lookup() {
    printf("\n=== Example 6: Dictionary Lookup ===\n");

    uint64_t values[] = {100, 200, 300, 200, 100, 300};
    size_t count = sizeof(values) / sizeof(values[0]);

    /* Build dictionary */
    varintDict *dict = varintDictCreate();
    varintDictBuild(dict, values, count);

    printf("Dictionary contents:\n");
    for (uint32_t i = 0; i < dict->size; i++) {
        printf("  Index %u -> Value %" PRIu64 "\n", i, dict->values[i]);
    }

    /* Find indices for values */
    printf("\nValue -> Index lookups:\n");
    const uint64_t testValues[] = {100, 200, 300, 400};
    for (size_t i = 0; i < 4; i++) {
        int32_t index = varintDictFind(dict, testValues[i]);
        if (index >= 0) {
            printf("  Value %" PRIu64 " -> Index %d\n", testValues[i], index);
        } else {
            printf("  Value %" PRIu64 " -> Not found\n", testValues[i]);
        }
    }

    /* Lookup values by index */
    printf("\nIndex -> Value lookups:\n");
    for (uint32_t i = 0; i < dict->size; i++) {
        uint64_t value = varintDictLookup(dict, i);
        printf("  Index %u -> Value %" PRIu64 "\n", i, value);
    }

    varintDictFree(dict);
    printf("✓ Dictionary lookup operations work\n");
}

/* ====================================================================
 * Example 7: When Dictionary Encoding is NOT Beneficial
 * ==================================================================== */
void example_poor_compression() {
    printf("\n=== Example 7: Poor Compression Case ===\n");

    /* Unique values (no repetition) - worst case for dictionary encoding */
    uint64_t uniqueValues[20];
    for (size_t i = 0; i < 20; i++) {
        uniqueValues[i] = i * 100;
    }

    printf("Testing with 20 unique values (no repetition)...\n");

    varintDictStats stats;
    varintDictGetStats(uniqueValues, 20, &stats);

    printf("Unique values: %zu\n", stats.uniqueCount);
    printf("Total count: %zu\n", stats.totalCount);
    printf("Original size: %zu bytes\n", stats.originalBytes);
    printf("Encoded size: %zu bytes\n", stats.totalBytes);

    if (stats.totalBytes >= stats.originalBytes) {
        printf("⚠ Dictionary encoding causes EXPANSION (%.1f%%)\n",
               ((float)(stats.totalBytes - stats.originalBytes) /
                stats.originalBytes) *
                   100);
        printf(
            "⚠ Recommendation: Use varintTagged or varintExternal instead\n");
    } else {
        printf("Space reduction: %.1f%%\n", stats.spaceReduction);
    }

    /* Show when it becomes beneficial */
    printf("\nComparison with repetitive data:\n");
    uint64_t repetitiveValues[20];
    for (size_t i = 0; i < 20; i++) {
        repetitiveValues[i] = (i % 3) * 100; /* Only 3 unique values */
    }

    varintDictGetStats(repetitiveValues, 20, &stats);
    printf("With 20 values, 3 unique:\n");
    printf("  Encoded size: %zu bytes\n", stats.totalBytes);
    printf("  Compression ratio: %.1fx\n", stats.compressionRatio);
    printf("  Space reduction: %.1f%%\n", stats.spaceReduction);

    printf("✓ Dictionary encoding best for repetitive data\n");
}

/* ====================================================================
 * Example 8: Large-Scale Simulation
 * ==================================================================== */
void example_large_scale() {
    printf("\n=== Example 8: Large-Scale (1M entries) ===\n");

    const size_t MILLION = 1000000;
    const int UNIQUE_SOURCES = 10;

    /* Allocate 1 million log source IDs */
    uint64_t *largeSources = (uint64_t *)malloc(MILLION * sizeof(uint64_t));
    assert(largeSources != NULL);

    /* Distribute across 10 unique sources */
    for (size_t i = 0; i < MILLION; i++) {
        largeSources[i] = (i % UNIQUE_SOURCES) + 1;
    }

    printf("Entries: 1,000,000\n");
    printf("Unique values: %d\n", UNIQUE_SOURCES);

    /* Get statistics */
    varintDictStats stats;
    int result = varintDictGetStats(largeSources, MILLION, &stats);
    assert(result == 0);

    printf("\nCompression Results:\n");
    printf("  Original size: %zu bytes (%.2f MB)\n", stats.originalBytes,
           stats.originalBytes / 1024.0 / 1024.0);
    printf("  Encoded size: %zu bytes (%.2f KB)\n", stats.totalBytes,
           stats.totalBytes / 1024.0);
    printf("  Compression ratio: %.1fx\n", stats.compressionRatio);
    printf("  Space reduction: %.2f%%\n", stats.spaceReduction);

    printf("\nBreakdown:\n");
    printf("  Dictionary: %zu bytes\n", stats.dictBytes);
    printf("  Indices: %zu bytes (%zu entries × %d bytes/entry)\n",
           stats.indexBytes, stats.totalCount,
           (int)(stats.indexBytes / stats.totalCount));

    /* Verify encoding/decoding (sample only for performance) */
    printf("\nVerifying encoding... ");
    uint8_t *buffer = (uint8_t *)malloc(stats.totalBytes + 1024);
    size_t encodedSize = varintDictEncode(buffer, largeSources, MILLION);
    assert(encodedSize == stats.totalBytes);
    printf("✓\n");

    printf("Verifying decoding... ");
    size_t decodedCount;
    uint64_t *decoded = varintDictDecode(buffer, encodedSize, &decodedCount);
    assert(decoded != NULL);
    assert(decodedCount == MILLION);

    /* Spot check values */
    for (size_t i = 0; i < MILLION; i += 100000) {
        assert(decoded[i] == largeSources[i]);
    }
    printf("✓\n");

    free(decoded);
    free(buffer);
    free(largeSources);
    printf("✓ Large-scale compression achieves %.1f%% savings\n",
           stats.spaceReduction);
}

/* ====================================================================
 * Main
 * ==================================================================== */
int main() {
    printf("===========================================\n");
    printf("   varintDict Example Suite\n");
    printf("===========================================\n");
    printf("Dictionary encoding for repetitive data\n");
    printf("Optimal for: logs, enums, status codes\n");

    example_basic();
    example_log_sources();
    example_status_codes();
    example_enum_values();
    example_shared_dictionary();
    example_lookup();
    example_poor_compression();
    example_large_scale();

    printf("\n===========================================\n");
    printf("All examples completed successfully!\n");
    printf("===========================================\n");

    return 0;
}
