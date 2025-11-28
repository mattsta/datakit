#include "varintAdaptive.h"
#include "ctest.h"
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

int varintAdaptiveTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int32_t err = 0;

    TEST("Encoding selection for timestamps (sorted sequential)") {
        /* Sorted timestamps with small deltas - should choose DELTA */
        uint64_t values[100];
        uint64_t base = 1700000000000ULL;
        for (int i = 0; i < 100; i++) {
            values[i] = base + ((uint64_t)i * 1000); /* 1 second increments */
        }

        uint8_t buffer[2048];
        varintAdaptiveMeta meta;
        size_t encoded = varintAdaptiveEncode(buffer, values, 100, &meta);

        if (encoded == 0) {
            ERRR("Failed to encode timestamp data");
        }

        /* Should achieve good compression */
        if (encoded >= 800) { /* Naive would be 100*8=800 */
            ERR("Poor compression for timestamps: %zu bytes", encoded);
        }

        /* Verify round-trip */
        uint64_t decoded[100];
        varintAdaptiveDecode(buffer, decoded, 100, NULL);

        for (size_t i = 0; i < 100; i++) {
            if (decoded[i] != values[i]) {
                ERR("Timestamp[%zu] = %" PRIu64 ", expected %" PRIu64 "", i,
                    decoded[i], values[i]);
                break;
            }
        }
    }

    TEST("Encoding selection for status codes (high repetition)") {
        /* Highly repetitive data with few unique values - should choose DICT */
        uint64_t values[200];
        const uint64_t codes[] = {200, 404, 500, 304, 403};
        for (int i = 0; i < 200; i++) {
            values[i] = codes[i % 5];
        }

        uint8_t buffer[2048];
        varintAdaptiveMeta meta;
        size_t encoded = varintAdaptiveEncode(buffer, values, 200, &meta);

        if (meta.encodingType != VARINT_ADAPTIVE_DICT) {
            ERR("Status code encoding = %d, expected DICT (%d)",
                meta.encodingType, VARINT_ADAPTIVE_DICT);
        }

        /* Verify high compression ratio */
        size_t naive = 200 * 8;
        float ratio = (float)naive / (float)encoded;
        if (ratio < 3.0f) {
            ERR("Dictionary compression ratio %.2fx too low", ratio);
        }

        /* Verify correctness */
        uint64_t decoded[200];
        varintAdaptiveDecode(buffer, decoded, 200, NULL);

        for (size_t i = 0; i < 200; i++) {
            if (decoded[i] != values[i]) {
                ERR("Status code[%zu] mismatch", i);
                break;
            }
        }
    }

    TEST("Encoding with outliers (should choose PFOR)") {
        /* Mostly clustered with a few outliers */
        uint64_t values[100];
        for (int i = 0; i < 97; i++) {
            values[i] = 1000 + (uint64_t)i; /* Clustered 1000-1096 */
        }
        values[97] = 100000; /* Outlier */
        values[98] = 100001; /* Outlier */
        values[99] = 100002; /* Outlier */

        uint8_t buffer[2048];
        varintAdaptiveMeta meta;
        size_t encoded = varintAdaptiveEncode(buffer, values, 100, &meta);

        /* Should choose PFOR or FOR */
        if (meta.encodingType != VARINT_ADAPTIVE_PFOR &&
            meta.encodingType != VARINT_ADAPTIVE_FOR) {
            ERR("Outlier encoding = %d, expected PFOR or FOR",
                meta.encodingType);
        }

        /* Should compress well */
        if (encoded >= 800) {
            ERR("Poor compression for clustered data: %zu bytes", encoded);
        }

        uint64_t decoded[100];
        varintAdaptiveDecode(buffer, decoded, 100, NULL);

        for (size_t i = 0; i < 100; i++) {
            if (decoded[i] != values[i]) {
                ERR("Outlier value[%zu] mismatch", i);
                break;
            }
        }
    }

    TEST("Round-trip for various data patterns") {
        /* Test different patterns and verify correctness */
        struct {
            const char *name;
            uint64_t values[50];
        } patterns[] = {
            {"Sequential", {0}}, {"Repetitive", {0}}, {"Random range", {0}}};

        /* Initialize sequential */
        for (int i = 0; i < 50; i++) {
            patterns[0].values[i] = (uint64_t)i;
        }

        /* Initialize repetitive */
        for (int i = 0; i < 50; i++) {
            patterns[1].values[i] = (uint64_t)(i % 5);
        }

        /* Initialize random in range */
        for (int i = 0; i < 50; i++) {
            patterns[2].values[i] = (uint64_t)((i * 37) % 1000);
        }

        uint8_t buffer[1024];

        for (int p = 0; p < 3; p++) {
            varintAdaptiveMeta meta;
            size_t encoded =
                varintAdaptiveEncode(buffer, patterns[p].values, 50, &meta);

            if (encoded == 0) {
                ERR("Failed to encode pattern: %s", patterns[p].name);
                continue;
            }

            /* Verify correctness */
            uint64_t decoded[50];
            varintAdaptiveDecode(buffer, decoded, 50, NULL);

            for (int i = 0; i < 50; i++) {
                if (decoded[i] != patterns[p].values[i]) {
                    ERR("Pattern '%s' value[%d] mismatch: %" PRIu64
                        " != %" PRIu64 "",
                        patterns[p].name, i, decoded[i], patterns[p].values[i]);
                    break;
                }
            }
        }
    }

    TEST("Single value array") {
        const uint64_t value[] = {12345};
        uint8_t buffer[256];

        varintAdaptiveMeta meta;
        size_t encoded = varintAdaptiveEncode(buffer, value, 1, &meta);

        if (encoded == 0) {
            ERRR("Failed to encode single value");
        }

        uint64_t decoded[1];
        varintAdaptiveDecode(buffer, decoded, 1, NULL);

        if (decoded[0] != value[0]) {
            ERR("Decoded value = %" PRIu64 ", expected %" PRIu64 "", decoded[0],
                value[0]);
        }
    }

    TEST("All identical values") {
        uint64_t values[100];
        for (int i = 0; i < 100; i++) {
            values[i] = 777;
        }

        uint8_t buffer[1024];
        varintAdaptiveMeta meta;
        size_t encoded = varintAdaptiveEncode(buffer, values, 100, &meta);
        (void)encoded; /* Intentionally unused in test */

        /* Should compress very well (uniqueRatio = 1/100 < 15%) */
        if (meta.encodingType != VARINT_ADAPTIVE_DICT) {
            ERR("Identical values encoding = %d, expected DICT (%d)",
                meta.encodingType, VARINT_ADAPTIVE_DICT);
        }

        /* Verify correctness */
        uint64_t decoded[100];
        varintAdaptiveDecode(buffer, decoded, 100, NULL);

        for (size_t i = 0; i < 100; i++) {
            if (decoded[i] != 777) {
                ERR("Identical value[%zu] = %" PRIu64 ", expected 777", i,
                    decoded[i]);
                break;
            }
        }
    }

    TEST("Data statistics analysis") {
        const uint64_t values[] = {10, 20, 10, 30, 20, 10};
        varintAdaptiveDataStats stats;

        varintAdaptiveAnalyze(values, 6, &stats);

        if (stats.minValue != 10) {
            ERR("Stats min = %" PRIu64 ", expected 10", stats.minValue);
        }

        if (stats.maxValue != 30) {
            ERR("Stats max = %" PRIu64 ", expected 30", stats.maxValue);
        }

        if (stats.range != 20) {
            ERR("Stats range = %" PRIu64 ", expected 20", stats.range);
        }

        if (stats.uniqueCount != 3) {
            ERR("Stats uniqueCount = %zu, expected 3", stats.uniqueCount);
        }

        /* Uniqueness ratio should be 3/6 = 0.5 */
        if (fabs(stats.uniqueRatio - 0.5) > 0.01) {
            ERR("Stats uniqueRatio = %.2f, expected 0.5", stats.uniqueRatio);
        }
    }

    TEST("Encoding selection API") {
        /* Test that we can force specific encodings */
        const uint64_t values[] = {1, 2, 3, 4, 5};
        uint8_t buffer[256];

        /* Force DELTA encoding */
        varintAdaptiveMeta meta;
        size_t encoded = varintAdaptiveEncodeWith(buffer, values, 5,
                                                  VARINT_ADAPTIVE_DELTA, &meta);
        (void)encoded; /* Intentionally unused in test */

        if (meta.encodingType != VARINT_ADAPTIVE_DELTA) {
            ERR("EncodeWith DELTA: got %d", meta.encodingType);
        }

        uint64_t decoded[5];
        varintAdaptiveDecode(buffer, decoded, 5, NULL);

        for (int i = 0; i < 5; i++) {
            if (decoded[i] != values[i]) {
                ERR("EncodeWith value[%d] mismatch", i);
                break;
            }
        }
    }

    TEST("Large dataset compression") {
        /* 1000 timestamps with realistic deltas */
        uint64_t *values = malloc(1000 * sizeof(uint64_t));
        if (values == NULL) {
            ERRR("Failed to allocate memory for values");
        }
        uint64_t base = 1700000000000ULL;

        for (int i = 0; i < 1000; i++) {
            /* 1-60 second deltas */
            values[i] =
                base + ((uint64_t)i * 1000) + ((uint64_t)(i * 37) % 60000);
        }

        uint8_t *buffer = malloc(16384);
        if (buffer == NULL) {
            free(values);
            ERRR("Failed to allocate memory for buffer");
        }
        varintAdaptiveMeta meta;
        size_t encoded = varintAdaptiveEncode(buffer, values, 1000, &meta);

        if (encoded == 0) {
            ERRR("Failed to encode large dataset");
        }

        /* Should achieve at least 2x compression */
        size_t naive = 1000 * 8;
        float ratio = (float)naive / (float)encoded;
        if (ratio < 2.0f) {
            ERR("Large dataset compression ratio %.2fx < 2.0x", ratio);
        }

        /* Verify correctness */
        uint64_t *decoded = malloc(1000 * sizeof(uint64_t));
        if (decoded == NULL) {
            free(values);
            free(buffer);
            ERRR("Failed to allocate memory for decoded");
        }
        varintAdaptiveDecode(buffer, decoded, 1000, NULL);

        for (int i = 0; i < 1000; i++) {
            if (decoded[i] != values[i]) {
                ERR("Large dataset[%d] = %" PRIu64 ", expected %" PRIu64 "", i,
                    decoded[i], values[i]);
                break;
            }
        }

        free(values);
        free(buffer);
        free(decoded);
    }

    TEST("Metadata reading") {
        const uint64_t values[] = {100, 200, 300};
        uint8_t buffer[256];

        varintAdaptiveMeta meta_encode;
        varintAdaptiveEncode(buffer, values, 3, &meta_encode);

        varintAdaptiveMeta meta_read;
        varintAdaptiveReadMeta(buffer, &meta_read);

        if (meta_read.encodingType != meta_encode.encodingType) {
            ERR("Read encoding type %d != encoded type %d",
                meta_read.encodingType, meta_encode.encodingType);
        }
    }

    TEST_FINAL_RESULT;
}

#ifdef VARINT_ADAPTIVE_TEST_STANDALONE
int main(int argc, char *argv[]) {
    return varintAdaptiveTest(argc, argv);
}
#endif
