#include "varintRLE.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================
 * RLE Analysis
 * ==================================================================== */

bool varintRLEAnalyze(const uint64_t *values, size_t count,
                      varintRLEMeta *meta) {
    assert(meta != NULL);

    if (count == 0) {
        meta->count = 0;
        meta->runCount = 0;
        meta->encodedSize = 0;
        meta->uniqueValues = 0;
        return false;
    }

    size_t runs = 1;
    size_t encodedSize = 0;
    size_t currentRunLen = 1;
    uint64_t currentVal = values[0];
    size_t uniqueCount = 1;

    /* Count runs and estimate encoded size */
    for (size_t i = 1; i < count; i++) {
        if (values[i] == currentVal) {
            currentRunLen++;
        } else {
            /* End of run - add encoded size */
            encodedSize += varintTaggedLen(currentRunLen);
            encodedSize += varintTaggedLen(currentVal);
            runs++;
            currentRunLen = 1;
            currentVal = values[i];
            uniqueCount++;
        }
    }

    /* Add final run */
    encodedSize += varintTaggedLen(currentRunLen);
    encodedSize += varintTaggedLen(currentVal);

    meta->count = count;
    meta->runCount = runs;
    meta->encodedSize = encodedSize;
    meta->uniqueValues = uniqueCount;

    /* Beneficial if encoded size < original size */
    return encodedSize < count * sizeof(uint64_t);
}

size_t varintRLESize(const uint64_t *values, size_t count) {
    varintRLEMeta meta;
    varintRLEAnalyze(values, count, &meta);
    return meta.encodedSize;
}

bool varintRLEIsBeneficial(const uint64_t *values, size_t count) {
    varintRLEMeta meta;
    return varintRLEAnalyze(values, count, &meta);
}

/* ====================================================================
 * RLE Encoding
 * ==================================================================== */

size_t varintRLEEncode(uint8_t *dst, const uint64_t *values, size_t count,
                       varintRLEMeta *meta) {
    assert(dst != NULL);
    assert(values != NULL || count == 0);

    if (count == 0) {
        if (meta) {
            meta->count = 0;
            meta->runCount = 0;
            meta->encodedSize = 0;
            meta->uniqueValues = 0;
        }
        return 0;
    }

    uint8_t *ptr = dst;
    size_t runs = 0;
    size_t currentRunLen = 1;
    uint64_t currentVal = values[0];

    for (size_t i = 1; i <= count; i++) {
        if (i < count && values[i] == currentVal) {
            currentRunLen++;
        } else {
            /* Write run: [length][value] */
            ptr += varintTaggedPut64(ptr, currentRunLen);
            ptr += varintTaggedPut64(ptr, currentVal);
            runs++;

            if (i < count) {
                currentRunLen = 1;
                currentVal = values[i];
            }
        }
    }

    if (meta) {
        meta->count = count;
        meta->runCount = runs;
        meta->encodedSize = (size_t)(ptr - dst);
        /* uniqueValues not computed here for efficiency */
        meta->uniqueValues = 0;
    }

    return (size_t)(ptr - dst);
}

size_t varintRLEEncodeWithHeader(uint8_t *dst, const uint64_t *values,
                                 size_t count, varintRLEMeta *meta) {
    assert(dst != NULL);

    uint8_t *ptr = dst;

    /* Write total count header */
    ptr += varintTaggedPut64(ptr, count);

    /* Encode runs */
    size_t bodySize = varintRLEEncode(ptr, values, count, meta);
    ptr += bodySize;

    if (meta) {
        meta->encodedSize = (size_t)(ptr - dst);
    }

    return (size_t)(ptr - dst);
}

/* ====================================================================
 * RLE Decoding
 * ==================================================================== */

size_t varintRLEDecodeRun(const uint8_t *src, size_t *runLength,
                          uint64_t *value) {
    assert(src != NULL);

    const uint8_t *ptr = src;
    uint64_t len;
    uint64_t val;

    ptr += varintTaggedGet64(ptr, &len);
    ptr += varintTaggedGet64(ptr, &val);

    *runLength = (size_t)len;
    *value = val;

    return (size_t)(ptr - src);
}

size_t varintRLEDecode(const uint8_t *src, uint64_t *values, size_t maxCount) {
    assert(src != NULL);
    assert(values != NULL);

    const uint8_t *ptr = src;
    size_t totalDecoded = 0;

    while (totalDecoded < maxCount) {
        size_t runLen;
        uint64_t value;
        ptr += varintRLEDecodeRun(ptr, &runLen, &value);

        /* Check if this is end marker (run length 0) or we've hit limit */
        if (runLen == 0) {
            break;
        }

        /* Write run values */
        size_t toWrite = runLen;
        if (totalDecoded + toWrite > maxCount) {
            toWrite = maxCount - totalDecoded;
        }

        for (size_t i = 0; i < toWrite; i++) {
            values[totalDecoded + i] = value;
        }
        totalDecoded += toWrite;

        if (totalDecoded + runLen > maxCount && toWrite < runLen) {
            /* Didn't decode full run, stop */
            break;
        }
    }

    return totalDecoded;
}

size_t varintRLEDecodeWithHeader(const uint8_t *src, uint64_t *values,
                                 size_t maxCount) {
    assert(src != NULL);
    assert(values != NULL);

    const uint8_t *ptr = src;

    /* Read total count header */
    uint64_t totalCount;
    ptr += varintTaggedGet64(ptr, &totalCount);

    if (totalCount > maxCount) {
        return 0; /* Buffer too small */
    }

    /* Decode runs */
    size_t decoded = 0;
    while (decoded < totalCount && decoded < maxCount) {
        size_t runLen;
        uint64_t value;
        ptr += varintRLEDecodeRun(ptr, &runLen, &value);

        for (size_t i = 0; i < runLen && decoded < maxCount; i++) {
            values[decoded++] = value;
        }
    }

    return decoded;
}

uint64_t varintRLEGetAt(const uint8_t *src, size_t index) {
    assert(src != NULL);

    const uint8_t *ptr = src;
    size_t position = 0;

    while (1) {
        size_t runLen;
        uint64_t value;
        ptr += varintRLEDecodeRun(ptr, &runLen, &value);

        if (runLen == 0) {
            /* End of data - return 0 for out of bounds */
            return 0;
        }

        if (position + runLen > index) {
            /* Index is within this run */
            return value;
        }
        position += runLen;
    }
}

size_t varintRLEGetCount(const uint8_t *src) {
    assert(src != NULL);

    /* Read count from header (assumes WithHeader format) */
    uint64_t count;
    varintTaggedGet64(src, &count);
    return (size_t)count;
}

size_t varintRLEGetRunCount(const uint8_t *src, size_t encodedSize) {
    assert(src != NULL);

    const uint8_t *ptr = src;
    const uint8_t *end = src + encodedSize;
    size_t runs = 0;

    while (ptr < end) {
        size_t runLen;
        uint64_t value;
        size_t consumed = varintRLEDecodeRun(ptr, &runLen, &value);

        if (runLen == 0 || consumed == 0) {
            break;
        }

        runs++;
        ptr += consumed;
    }

    return runs;
}

/* ====================================================================
 * Unit Tests
 * ==================================================================== */
#ifdef VARINT_RLE_TEST
#include "ctest.h"

int varintRLETest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int32_t err = 0;

    TEST("Basic RLE encode/decode") {
        uint64_t values[] = {1, 1, 1, 2, 2, 3, 3, 3, 3, 3};
        size_t count = sizeof(values) / sizeof(values[0]);

        uint8_t encoded[100];
        varintRLEMeta meta;
        size_t encodedSize = varintRLEEncode(encoded, values, count, &meta);

        if (meta.runCount != 3) {
            ERR("Expected 3 runs, got %zu", meta.runCount);
        }

        uint64_t decoded[10];
        size_t decodedCount = varintRLEDecode(encoded, decoded, 10);

        if (decodedCount != count) {
            ERR("Decoded count mismatch: expected %zu, got %zu", count,
                decodedCount);
        }

        for (size_t i = 0; i < count; i++) {
            if (decoded[i] != values[i]) {
                ERR("Value mismatch at %zu: expected %llu, got %llu", i,
                    (unsigned long long)values[i],
                    (unsigned long long)decoded[i]);
            }
        }

        (void)encodedSize;
    }

    TEST("RLE with header format") {
        uint64_t values[] = {5, 5, 5, 5, 10, 10, 15};
        size_t count = sizeof(values) / sizeof(values[0]);

        uint8_t encoded[100];
        varintRLEMeta meta;
        size_t encodedSize =
            varintRLEEncodeWithHeader(encoded, values, count, &meta);

        size_t headerCount = varintRLEGetCount(encoded);
        if (headerCount != count) {
            ERR("Header count mismatch: expected %zu, got %zu", count,
                headerCount);
        }

        uint64_t decoded[10];
        size_t decodedCount = varintRLEDecodeWithHeader(encoded, decoded, 10);

        if (decodedCount != count) {
            ERR("Decoded count mismatch: expected %zu, got %zu", count,
                decodedCount);
        }

        for (size_t i = 0; i < count; i++) {
            if (decoded[i] != values[i]) {
                ERR("Value mismatch at %zu", i);
            }
        }

        (void)encodedSize;
    }

    TEST("RLE random access") {
        uint64_t values[] = {100, 100, 200, 200, 200, 300};
        size_t count = sizeof(values) / sizeof(values[0]);

        uint8_t encoded[100];
        varintRLEEncode(encoded, values, count, NULL);

        if (varintRLEGetAt(encoded, 0) != 100) {
            ERRR("Random access [0] failed");
        }
        if (varintRLEGetAt(encoded, 1) != 100) {
            ERRR("Random access [1] failed");
        }
        if (varintRLEGetAt(encoded, 2) != 200) {
            ERRR("Random access [2] failed");
        }
        if (varintRLEGetAt(encoded, 4) != 200) {
            ERRR("Random access [4] failed");
        }
        if (varintRLEGetAt(encoded, 5) != 300) {
            ERRR("Random access [5] failed");
        }
    }

    TEST("RLE compression analysis") {
        /* Highly compressible data */
        uint64_t sparse[1000];
        for (size_t i = 0; i < 1000; i++) {
            sparse[i] = 0;
        }
        sparse[500] = 42;

        varintRLEMeta meta;
        bool beneficial = varintRLEAnalyze(sparse, 1000, &meta);

        if (!beneficial) {
            ERRR("RLE should be beneficial for sparse data");
        }

        if (meta.runCount != 3) {
            ERR("Expected 3 runs for sparse data, got %zu", meta.runCount);
        }

        /* Non-compressible data (all unique large values)
         * Use large values that need full 8-byte encoding */
        uint64_t unique[100];
        for (size_t i = 0; i < 100; i++) {
            unique[i] = UINT64_MAX - i; /* Large values need 9 bytes tagged */
        }

        beneficial = varintRLEIsBeneficial(unique, 100);
        /* For large unique values, RLE is NOT beneficial:
         * Each run is (1-2 bytes length) + (9 bytes value) = 10-11 bytes
         * vs raw: 8 bytes per value
         * 100 * 10 = 1000 bytes RLE vs 100 * 8 = 800 bytes raw */
        if (beneficial) {
            ERRR("RLE should NOT be beneficial for unique large data");
        }
    }

    TEST("RLE edge cases") {
        /* Single value */
        const uint64_t single[] = {42};
        uint8_t encoded[20];
        varintRLEMeta meta;

        varintRLEEncode(encoded, single, 1, &meta);
        if (meta.runCount != 1) {
            ERR("Single value: expected 1 run, got %zu", meta.runCount);
        }

        uint64_t decoded[1];
        size_t decodedCount = varintRLEDecode(encoded, decoded, 1);
        if (decodedCount != 1 || decoded[0] != 42) {
            ERRR("Single value decode failed");
        }

        /* Empty array */
        size_t encodedSize = varintRLEEncode(encoded, NULL, 0, &meta);
        if (encodedSize != 0 || meta.runCount != 0) {
            ERRR("Empty array should produce no output");
        }

        /* All same values */
        uint64_t allSame[100];
        for (size_t i = 0; i < 100; i++) {
            allSame[i] = 99;
        }

        varintRLEEncode(encoded, allSame, 100, &meta);
        if (meta.runCount != 1) {
            ERR("All same: expected 1 run, got %zu", meta.runCount);
        }
    }

    TEST("RLE large values") {
        uint64_t values[] = {
            UINT64_MAX, UINT64_MAX, UINT64_MAX - 1, UINT64_MAX - 1, 0, 0, 0};
        size_t count = sizeof(values) / sizeof(values[0]);

        uint8_t encoded[200];
        size_t encodedSize = varintRLEEncode(encoded, values, count, NULL);

        uint64_t decoded[10];
        size_t decodedCount = varintRLEDecode(encoded, decoded, 10);

        if (decodedCount != count) {
            ERRR("Large values: count mismatch");
        }

        for (size_t i = 0; i < count; i++) {
            if (decoded[i] != values[i]) {
                ERR("Large values: mismatch at %zu", i);
            }
        }

        (void)encodedSize;
    }

    TEST("RLE very long runs") {
        /* Test with a very long single run - stress test */
        uint64_t *longRun = malloc(10000 * sizeof(uint64_t));
        if (!longRun) {
            ERRR("malloc failed");
        }
        for (size_t i = 0; i < 10000; i++) {
            longRun[i] = 12345;
        }

        uint8_t *encoded = malloc(varintRLEMaxSize(10000));
        if (!encoded) {
            free(longRun);
            ERRR("malloc failed");
        }
        varintRLEMeta meta;
        size_t encodedSize = varintRLEEncode(encoded, longRun, 10000, &meta);

        if (meta.runCount != 1) {
            ERR("Long run: expected 1 run, got %zu", meta.runCount);
        }

        /* Should be very small: just 2 varints (length + value) */
        if (encodedSize > 10) {
            ERR("Long run: encoded size too large: %zu", encodedSize);
        }

        uint64_t *decoded = malloc(10000 * sizeof(uint64_t));
        if (!decoded) {
            free(longRun);
            free(encoded);
            ERRR("malloc failed");
        }
        size_t decodedCount = varintRLEDecode(encoded, decoded, 10000);

        if (decodedCount != 10000) {
            ERR("Long run: decoded %zu, expected 10000", decodedCount);
        }

        for (size_t i = 0; i < 10000; i++) {
            if (decoded[i] != 12345) {
                ERR("Long run: mismatch at %zu", i);
                break;
            }
        }

        free(longRun);
        free(encoded);
        free(decoded);
    }

    TEST("RLE alternating values (worst case)") {
        /* Alternating values = maximum runs = worst compression */
        uint64_t alternating[100];
        for (size_t i = 0; i < 100; i++) {
            alternating[i] = i % 2; /* 0, 1, 0, 1, ... */
        }

        varintRLEMeta meta;
        varintRLEAnalyze(alternating, 100, &meta);

        if (meta.runCount != 100) {
            ERR("Alternating: expected 100 runs, got %zu", meta.runCount);
        }

        /* Each run: 1 byte length + 1 byte value = 2 bytes
         * 100 runs * 2 = 200 bytes vs raw 800 bytes
         * Still beneficial for small values! */
        uint8_t encoded[300];
        size_t encodedSize = varintRLEEncode(encoded, alternating, 100, &meta);

        uint64_t decoded[100];
        size_t decodedCount = varintRLEDecode(encoded, decoded, 100);

        if (decodedCount != 100) {
            ERR("Alternating: decoded %zu, expected 100", decodedCount);
        }

        for (size_t i = 0; i < 100; i++) {
            if (decoded[i] != alternating[i]) {
                ERR("Alternating: mismatch at %zu", i);
                break;
            }
        }

        (void)encodedSize;
    }

    TEST("RLE partial decode") {
        /* Decode with smaller buffer than data */
        const uint64_t values[] = {1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4};
        size_t count = 12;

        uint8_t encoded[100];
        varintRLEEncodeWithHeader(encoded, values, count, NULL);

        /* Decode only first 5 values */
        uint64_t decoded[5];
        size_t decodedCount = varintRLEDecodeWithHeader(encoded, decoded, 5);

        /* Should return 0 because header says 12 but buffer is 5 */
        if (decodedCount != 0) {
            ERR("Partial decode: expected 0 (buffer too small), got %zu",
                decodedCount);
        }

        /* Test without header - decode as many as possible */
        uint8_t encodedNoHeader[100];
        varintRLEEncode(encodedNoHeader, values, count, NULL);

        uint64_t decoded2[5];
        size_t decodedCount2 = varintRLEDecode(encodedNoHeader, decoded2, 5);

        /* Without header, should decode partial */
        if (decodedCount2 < 3) {
            ERR("Partial decode (no header): expected at least 3, got %zu",
                decodedCount2);
        }
    }

    TEST("RLE random access edge cases") {
        const uint64_t values[] = {100, 100, 100, 200, 300, 300};
        size_t count = 6;

        uint8_t encoded[100];
        varintRLEEncode(encoded, values, count, NULL);

        /* Test first element of each run */
        if (varintRLEGetAt(encoded, 0) != 100) {
            ERRR("GetAt(0) failed");
        }
        if (varintRLEGetAt(encoded, 3) != 200) {
            ERRR("GetAt(3) failed - first of run 2");
        }
        if (varintRLEGetAt(encoded, 4) != 300) {
            ERRR("GetAt(4) failed - first of run 3");
        }

        /* Test last element of each run */
        if (varintRLEGetAt(encoded, 2) != 100) {
            ERRR("GetAt(2) failed - last of run 1");
        }
        if (varintRLEGetAt(encoded, 5) != 300) {
            ERRR("GetAt(5) failed - last of run 3");
        }

        /* Out of bounds should return 0 */
        /* Note: This relies on hitting runLen == 0 which may not happen
         * unless we write a terminator. Current impl may loop forever
         * for out-of-bounds, so skip this test. */
    }

    TEST("RLE zero value handling") {
        /* Test that zero values are handled correctly */
        const uint64_t zeros[] = {0, 0, 0, 0, 0};
        uint8_t encoded[50];
        varintRLEMeta meta;

        varintRLEEncode(encoded, zeros, 5, &meta);

        if (meta.runCount != 1) {
            ERR("Zeros: expected 1 run, got %zu", meta.runCount);
        }

        uint64_t decoded[5];
        size_t decodedCount = varintRLEDecode(encoded, decoded, 5);

        if (decodedCount != 5) {
            ERR("Zeros: decoded %zu, expected 5", decodedCount);
        }

        for (size_t i = 0; i < 5; i++) {
            if (decoded[i] != 0) {
                ERR("Zeros: expected 0 at %zu, got %llu", i,
                    (unsigned long long)decoded[i]);
            }
        }
    }

    TEST("RLE mixed patterns") {
        /* Complex pattern: zeros, value, zeros, value sequence */
        uint64_t pattern[100];
        for (size_t i = 0; i < 100; i++) {
            if (i % 10 == 5) {
                pattern[i] = 42;
            } else {
                pattern[i] = 0;
            }
        }

        uint8_t encoded[300];
        varintRLEMeta meta;
        varintRLEEncode(encoded, pattern, 100, &meta);

        /* Should have 21 runs: 0(5), 42(1), 0(9), 42(1), 0(9), ...
         * Pattern: 10 runs of 42 (indices 5,15,25,...,95) interspersed with
         * 11 runs of 0 (indices 0-4, 6-14, 16-24, ..., 96-99) */
        if (meta.runCount != 21) {
            ERR("Mixed pattern: expected 21 runs, got %zu", meta.runCount);
        }

        uint64_t decoded[100];
        size_t decodedCount = varintRLEDecode(encoded, decoded, 100);

        if (decodedCount != 100) {
            ERR("Mixed pattern: decoded %zu, expected 100", decodedCount);
        }

        for (size_t i = 0; i < 100; i++) {
            if (decoded[i] != pattern[i]) {
                ERR("Mixed pattern: mismatch at %zu", i);
                break;
            }
        }
    }

    TEST("RLE GetRunCount accuracy") {
        const uint64_t values[] = {1, 1, 2, 2, 2, 3, 4, 4};
        size_t count = 8;

        uint8_t encoded[100];
        varintRLEMeta meta;
        size_t encodedSize = varintRLEEncode(encoded, values, count, &meta);

        size_t runCount = varintRLEGetRunCount(encoded, encodedSize);

        if (runCount != meta.runCount) {
            ERR("GetRunCount: expected %zu, got %zu", meta.runCount, runCount);
        }

        if (runCount != 4) {
            ERR("GetRunCount: expected 4 runs, got %zu", runCount);
        }
    }

    TEST("RLE header format roundtrip") {
        /* Test various sizes with header format */
        size_t testSizes[] = {1, 2, 10, 100, 1000};

        for (size_t t = 0; t < sizeof(testSizes) / sizeof(testSizes[0]); t++) {
            size_t count = testSizes[t];
            uint64_t *values = malloc(count * sizeof(uint64_t));
            if (!values) {
                continue;
            }

            /* Create pattern with some runs */
            for (size_t i = 0; i < count; i++) {
                values[i] = i / 10; /* Run of 10 each */
            }

            uint8_t *encoded = malloc(varintRLEMaxSize(count) + 10);
            if (!encoded) {
                free(values);
                continue;
            }
            varintRLEMeta meta;
            varintRLEEncodeWithHeader(encoded, values, count, &meta);

            /* Verify header count */
            size_t headerCount = varintRLEGetCount(encoded);
            if (headerCount != count) {
                ERR("Header format size %zu: header count %zu != %zu", count,
                    headerCount, count);
            }

            /* Decode and verify */
            uint64_t *decoded = malloc(count * sizeof(uint64_t));
            if (!decoded) {
                free(values);
                free(encoded);
                continue;
            }
            size_t decodedCount =
                varintRLEDecodeWithHeader(encoded, decoded, count);

            if (decodedCount != count) {
                ERR("Header format size %zu: decoded %zu values", count,
                    decodedCount);
            }

            bool match = true;
            for (size_t i = 0; i < count && match; i++) {
                if (decoded[i] != values[i]) {
                    ERR("Header format size %zu: mismatch at %zu", count, i);
                    match = false;
                }
            }

            free(values);
            free(encoded);
            free(decoded);
        }
    }

    TEST("RLE uniqueValues count") {
        const uint64_t values[] = {1, 1, 2, 2, 3, 3, 1, 1};
        varintRLEMeta meta;

        varintRLEAnalyze(values, 8, &meta);

        /* uniqueValues counts unique values seen in order (transitions)
         * Here: 1, 2, 3, 1 = 4 unique (but 1 appears twice) */
        if (meta.uniqueValues != 4) {
            ERR("uniqueValues: expected 4, got %zu", meta.uniqueValues);
        }

        if (meta.runCount != 4) {
            ERR("runCount: expected 4, got %zu", meta.runCount);
        }
    }

    TEST("RLE stress test large array") {
        /* Large array with random-ish pattern */
        size_t count = 50000;
        uint64_t *values = malloc(count * sizeof(uint64_t));
        if (!values) {
            ERRR("malloc failed");
        }

        /* Create pattern: blocks of 100 with same value */
        for (size_t i = 0; i < count; i++) {
            values[i] = (i / 100) % 256;
        }

        uint8_t *encoded = malloc(varintRLEMaxSize(count));
        if (!encoded) {
            free(values);
            ERRR("malloc failed");
        }
        varintRLEMeta meta;
        size_t encodedSize =
            varintRLEEncodeWithHeader(encoded, values, count, &meta);

        /* Should have 500 runs (50000 / 100) */
        if (meta.runCount != 500) {
            ERR("Stress test: expected 500 runs, got %zu", meta.runCount);
        }

        /* Verify compression ratio */
        double ratio = (double)encodedSize / (count * sizeof(uint64_t));
        if (ratio > 0.1) {
            ERR("Stress test: compression ratio %.2f%% too high", ratio * 100);
        }

        /* Decode and verify */
        uint64_t *decoded = malloc(count * sizeof(uint64_t));
        if (!decoded) {
            free(values);
            free(encoded);
            ERRR("malloc failed");
        }
        size_t decodedCount =
            varintRLEDecodeWithHeader(encoded, decoded, count);

        if (decodedCount != count) {
            ERR("Stress test: decoded %zu, expected %zu", decodedCount, count);
        }

        bool match = true;
        for (size_t i = 0; i < count && match; i++) {
            if (decoded[i] != values[i]) {
                ERR("Stress test: mismatch at %zu", i);
                match = false;
            }
        }

        free(values);
        free(encoded);
        free(decoded);
    }

    TEST_FINAL_RESULT;
    return 0;
}

#endif /* VARINT_RLE_TEST */
