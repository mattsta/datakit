#include "varintPFOR.h"
#include "ctest.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

int varintPFORTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int32_t err = 0;

    TEST("Basic PFOR encode/decode with 95th percentile") {
        uint64_t *values = (uint64_t *)malloc(100 * sizeof(uint64_t));
        if (!values) {
            ERRR("Failed to allocate memory for values");
        }
        for (int i = 0; i < 95; i++) {
            values[i] = 100 + (uint64_t)i; /* Clustered 100-194 */
        }
        for (int i = 95; i < 100; i++) {
            values[i] = 50000 + (uint64_t)i; /* Outliers */
        }
        size_t count = 100;
        uint8_t buffer[2048];

        varintPFORMeta meta;
        size_t encoded = varintPFOREncode(buffer, values, (uint32_t)count,
                                          VARINT_PFOR_THRESHOLD_95, &meta);

        if (encoded == 0) {
            ERRR("Failed to encode PFOR array");
        }

        /* Should have ~5 exceptions */
        if (meta.exceptionCount < 4 || meta.exceptionCount > 6) {
            ERR("Exception count = %u, expected ~5", meta.exceptionCount);
        }

        uint64_t *decoded = (uint64_t *)malloc(100 * sizeof(uint64_t));
        if (!decoded) {
            free(values);
            ERRR("Failed to allocate memory for decoded");
        }
        uint32_t decoded_count =
            (uint32_t)varintPFORDecode(buffer, decoded, &meta);

        if (decoded_count != count) {
            ERR("Decoded count %u != expected count %zu", decoded_count, count);
        }

        for (size_t i = 0; i < count; i++) {
            if (decoded[i] != values[i]) {
                ERR("Decoded[%zu] = %" PRIu64 ", expected %" PRIu64 "", i,
                    decoded[i], values[i]);
                break;
            }
        }

        free(decoded);
        free(values);
    }

    TEST("PFOR with 90th percentile threshold") {
        uint64_t values[100];
        for (int i = 0; i < 90; i++) {
            values[i] = 1000 + (uint64_t)i; /* Clustered */
        }
        for (int i = 90; i < 100; i++) {
            values[i] = 100000 + (uint64_t)i; /* Outliers */
        }

        uint8_t buffer[2048];
        varintPFORMeta meta;
        size_t encoded = varintPFOREncode(buffer, values, 100,
                                          VARINT_PFOR_THRESHOLD_90, &meta);
        (void)encoded; /* Intentionally unused in test */

        /* Should have ~10 exceptions */
        if (meta.exceptionCount < 9 || meta.exceptionCount > 11) {
            ERR("Exception count = %u, expected ~10", meta.exceptionCount);
        }

        uint64_t decoded[100];
        varintPFORDecode(buffer, decoded, &meta);

        for (int i = 0; i < 100; i++) {
            if (decoded[i] != values[i]) {
                ERR("90th percentile: value[%d] mismatch", i);
                break;
            }
        }
    }

    TEST("PFOR with 99th percentile threshold") {
        uint64_t values[1000];
        for (int i = 0; i < 990; i++) {
            values[i] = 500 + (uint64_t)i;
        }
        for (int i = 990; i < 1000; i++) {
            values[i] = 500000 + (uint64_t)i; /* 10 outliers (1% of 1000) */
        }

        uint8_t buffer[8192];
        varintPFORMeta meta;
        size_t encoded = varintPFOREncode(buffer, values, 1000,
                                          VARINT_PFOR_THRESHOLD_99, &meta);

        (void)encoded; /* Intentionally unused in test */
        /* Should have ~10 exceptions (1% of 1000) */
        if (meta.exceptionCount < 9 || meta.exceptionCount > 11) {
            ERR("99th percentile exception count = %u, expected ~10",
                meta.exceptionCount);
        }

        uint64_t decoded[1000];
        varintPFORDecode(buffer, decoded, &meta);

        for (int i = 0; i < 1000; i++) {
            if (decoded[i] != values[i]) {
                ERR("99th percentile: value[%d] = %" PRIu64
                    ", expected %" PRIu64 "",
                    i, decoded[i], values[i]);
                break;
            }
        }
    }

    TEST("PFOR random access with exceptions") {
        const uint64_t values[] = {10, 20, 30, 40, 50, 10000};
        size_t count = 6;
        uint8_t buffer[512];

        varintPFORMeta meta;
        varintPFOREncode(buffer, values, (uint32_t)count,
                         VARINT_PFOR_THRESHOLD_95, &meta);

        /* Access each value individually */
        for (size_t i = 0; i < count; i++) {
            uint64_t val = varintPFORGetAt(buffer, (uint32_t)i, &meta);
            if (val != values[i]) {
                ERR("GetAt(%zu) = %" PRIu64 ", expected %" PRIu64 "", i, val,
                    values[i]);
            }
        }
    }

    TEST("PFOR with no exceptions") {
        /* All values in tight cluster */
        uint64_t values[50];
        for (int i = 0; i < 50; i++) {
            values[i] = 1000 + (uint64_t)i;
        }

        uint8_t buffer[1024];
        varintPFORMeta meta;
        varintPFOREncode(buffer, values, 50, VARINT_PFOR_THRESHOLD_95, &meta);

        /* Should have 0 exceptions (or very few) */
        if (meta.exceptionCount > 3) {
            ERR("Too many exceptions for tight cluster: %u",
                meta.exceptionCount);
        }

        uint64_t decoded[50];
        varintPFORDecode(buffer, decoded, &meta);

        for (int i = 0; i < 50; i++) {
            if (decoded[i] != values[i]) {
                ERR("No exceptions: value[%d] mismatch", i);
                break;
            }
        }
    }

    TEST("PFOR size calculation") {
        const uint64_t values[] = {100, 200, 300, 10000};
        uint8_t buffer[512];

        varintPFORMeta meta;
        size_t encoded = varintPFOREncode(buffer, values, 4,
                                          VARINT_PFOR_THRESHOLD_95, &meta);

        size_t calculated = varintPFORSize(&meta);
        if (calculated != encoded) {
            ERR("Calculated size %zu != encoded size %zu", calculated, encoded);
        }
    }

    TEST("PFOR single value") {
        const uint64_t value[] = {12345};
        uint8_t buffer[256];

        varintPFORMeta meta;
        size_t encoded =
            varintPFOREncode(buffer, value, 1, VARINT_PFOR_THRESHOLD_95, &meta);
        (void)encoded; /* Intentionally unused in test */

        uint64_t decoded[1];
        varintPFORDecode(buffer, decoded, &meta);

        if (decoded[0] != value[0]) {
            ERR("Single value = %" PRIu64 ", expected %" PRIu64 "", decoded[0],
                value[0]);
        }
    }

    TEST("PFOR metadata reading") {
        const uint64_t values[] = {10, 20, 30, 40, 50000};
        uint8_t buffer[512];

        varintPFORMeta meta_encode;
        varintPFOREncode(buffer, values, 5, VARINT_PFOR_THRESHOLD_95,
                         &meta_encode);

        varintPFORMeta meta_read;
        varintPFORReadMeta(buffer, &meta_read);

        if (meta_read.min != meta_encode.min) {
            ERR("Read min %" PRIu64 " != encoded min %" PRIu64 "",
                meta_read.min, meta_encode.min);
        }

        if (meta_read.width != meta_encode.width) {
            ERR("Read width %d != encoded width %d", meta_read.width,
                meta_encode.width);
        }

        if (meta_read.exceptionCount != meta_encode.exceptionCount) {
            ERR("Read exception count %u != encoded %u",
                meta_read.exceptionCount, meta_encode.exceptionCount);
        }
    }

    TEST("PFOR compression efficiency") {
        /* Simulate stock prices: mostly around 100-150, few spikes */
        uint64_t values[100];
        for (int i = 0; i < 95; i++) {
            values[i] = 100 + (uint64_t)(i % 50);
        }
        values[95] = 1000; /* Flash crash */
        values[96] = 120;
        values[97] = 5000; /* Spike */
        values[98] = 110;
        values[99] = 130;

        uint8_t buffer[2048];
        varintPFORMeta meta;
        size_t encoded = varintPFOREncode(buffer, values, 100,
                                          VARINT_PFOR_THRESHOLD_95, &meta);

        /* Naive: 100 * 8 = 800 bytes */
        /* PFOR should be much smaller */
        if (encoded >= 800) {
            ERR("PFOR not efficient: %zu bytes (expected < 800)", encoded);
        }

        /* Verify correctness */
        uint64_t decoded[100];
        varintPFORDecode(buffer, decoded, &meta);

        for (int i = 0; i < 100; i++) {
            if (decoded[i] != values[i]) {
                ERR("Compression test: value[%d] mismatch", i);
                break;
            }
        }
    }

    TEST_FINAL_RESULT;
}

#ifdef VARINT_PFOR_TEST_STANDALONE
int main(int argc, char *argv[]) {
    return varintPFORTest(argc, argv);
}
#endif
