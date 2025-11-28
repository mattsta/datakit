#include "varintElias.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================
 * Bit-level I/O Implementation
 * ==================================================================== */

void varintBitWriterInit(varintBitWriter *w, uint8_t *buffer, size_t capacity) {
    w->buffer = buffer;
    w->bitPos = 0;
    w->capacity = capacity;
    memset(buffer, 0, capacity);
}

void varintBitWriterWrite(varintBitWriter *w, uint64_t value, size_t nBits) {
    assert(nBits <= 64);

    for (size_t i = 0; i < nBits; i++) {
        size_t byteIdx = w->bitPos / 8;
        size_t bitIdx = 7 - (w->bitPos % 8); /* MSB first */

        assert(byteIdx < w->capacity);

        if ((value >> (nBits - 1 - i)) & 1) {
            w->buffer[byteIdx] |= (1 << bitIdx);
        }
        w->bitPos++;
    }
}

size_t varintBitWriterBytes(const varintBitWriter *w) {
    return (w->bitPos + 7) / 8;
}

void varintBitReaderInit(varintBitReader *r, const uint8_t *buffer,
                         size_t totalBits) {
    r->buffer = buffer;
    r->bitPos = 0;
    r->totalBits = totalBits;
}

uint64_t varintBitReaderRead(varintBitReader *r, size_t nBits) {
    assert(nBits <= 64);

    uint64_t result = 0;
    for (size_t i = 0; i < nBits; i++) {
        size_t byteIdx = r->bitPos / 8;
        size_t bitIdx = 7 - (r->bitPos % 8); /* MSB first */

        if ((r->buffer[byteIdx] >> bitIdx) & 1) {
            result |= (1ULL << (nBits - 1 - i));
        }
        r->bitPos++;
    }
    return result;
}

bool varintBitReaderHasMore(const varintBitReader *r, size_t nBits) {
    return r->bitPos + nBits <= r->totalBits;
}

/* ====================================================================
 * Helper: Floor log2 (position of highest set bit)
 * ==================================================================== */
static inline size_t floorLog2(uint64_t value) {
    assert(value > 0);
    size_t log = 0;
    while (value > 1) {
        value >>= 1;
        log++;
    }
    return log;
}

/* ====================================================================
 * Elias Gamma Implementation
 * ==================================================================== */

size_t varintEliasGammaBits(uint64_t value) {
    assert(value >= 1);
    size_t n = floorLog2(value);
    /* n zeros + (n+1) bits for value = 2n+1 bits */
    return 2 * n + 1;
}

size_t varintEliasGammaEncode(varintBitWriter *w, uint64_t value) {
    assert(value >= 1);

    size_t n = floorLog2(value);

    /* Write n zeros */
    for (size_t i = 0; i < n; i++) {
        varintBitWriterWrite(w, 0, 1);
    }

    /* Write value in binary (n+1 bits) */
    varintBitWriterWrite(w, value, n + 1);

    return 2 * n + 1;
}

uint64_t varintEliasGammaDecode(varintBitReader *r) {
    /* Count leading zeros */
    size_t n = 0;
    while (varintBitReaderRead(r, 1) == 0) {
        n++;
        if (n > 63) {
            return 0; /* Overflow protection */
        }
    }

    /* We've read the leading 1, now read remaining n bits */
    if (n == 0) {
        return 1;
    }

    uint64_t remaining = varintBitReaderRead(r, n);
    return (1ULL << n) | remaining;
}

size_t varintEliasGammaEncodeArray(uint8_t *dst, const uint64_t *values,
                                   size_t count, varintEliasMeta *meta) {
    varintBitWriter writer;
    varintBitWriterInit(&writer, dst, varintEliasGammaMaxBytes(count));

    size_t totalBits = 0;
    for (size_t i = 0; i < count; i++) {
        totalBits += varintEliasGammaEncode(&writer, values[i]);
    }

    if (meta) {
        meta->count = count;
        meta->totalBits = totalBits;
        meta->encodedBytes = varintBitWriterBytes(&writer);
    }

    return varintBitWriterBytes(&writer);
}

size_t varintEliasGammaDecodeArray(const uint8_t *src, size_t srcBits,
                                   uint64_t *values, size_t maxCount) {
    varintBitReader reader;
    varintBitReaderInit(&reader, src, srcBits);

    size_t decoded = 0;
    while (decoded < maxCount && varintBitReaderHasMore(&reader, 1)) {
        uint64_t value = varintEliasGammaDecode(&reader);
        if (value == 0) {
            break; /* Decode error */
        }
        values[decoded++] = value;
    }

    return decoded;
}

/* ====================================================================
 * Elias Delta Implementation
 * ==================================================================== */

size_t varintEliasDeltaBits(uint64_t value) {
    assert(value >= 1);
    size_t n = floorLog2(value);
    size_t lenN = n + 1;
    /* Gamma encoding of (n+1) + n remaining bits */
    return varintEliasGammaBits(lenN) + n;
}

size_t varintEliasDeltaEncode(varintBitWriter *w, uint64_t value) {
    assert(value >= 1);

    size_t n = floorLog2(value);
    size_t lenN = n + 1;

    /* Write length (n+1) in Gamma code */
    size_t gammaBits = varintEliasGammaEncode(w, lenN);

    /* Write remaining n bits (without leading 1) */
    if (n > 0) {
        uint64_t remaining = value & ((1ULL << n) - 1);
        varintBitWriterWrite(w, remaining, n);
    }

    return gammaBits + n;
}

uint64_t varintEliasDeltaDecode(varintBitReader *r) {
    /* Read length in Gamma code */
    uint64_t lenN = varintEliasGammaDecode(r);
    if (lenN == 0) {
        return 0; /* Decode error */
    }

    size_t n = (size_t)lenN - 1;

    if (n == 0) {
        return 1;
    }

    /* Read remaining n bits */
    uint64_t remaining = varintBitReaderRead(r, n);
    return (1ULL << n) | remaining;
}

size_t varintEliasDeltaEncodeArray(uint8_t *dst, const uint64_t *values,
                                   size_t count, varintEliasMeta *meta) {
    varintBitWriter writer;
    varintBitWriterInit(&writer, dst, varintEliasDeltaMaxBytes(count));

    size_t totalBits = 0;
    for (size_t i = 0; i < count; i++) {
        totalBits += varintEliasDeltaEncode(&writer, values[i]);
    }

    if (meta) {
        meta->count = count;
        meta->totalBits = totalBits;
        meta->encodedBytes = varintBitWriterBytes(&writer);
    }

    return varintBitWriterBytes(&writer);
}

size_t varintEliasDeltaDecodeArray(const uint8_t *src, size_t srcBits,
                                   uint64_t *values, size_t maxCount) {
    varintBitReader reader;
    varintBitReaderInit(&reader, src, srcBits);

    size_t decoded = 0;
    while (decoded < maxCount && varintBitReaderHasMore(&reader, 1)) {
        uint64_t value = varintEliasDeltaDecode(&reader);
        if (value == 0) {
            break; /* Decode error */
        }
        values[decoded++] = value;
    }

    return decoded;
}

/* ====================================================================
 * Utility Functions
 * ==================================================================== */

bool varintEliasGammaIsBeneficial(const uint64_t *values, size_t count) {
    size_t totalBits = 0;
    for (size_t i = 0; i < count; i++) {
        if (values[i] < 1) {
            return false; /* Cannot encode 0 */
        }
        totalBits += varintEliasGammaBits(values[i]);
    }

    size_t encodedBytes = (totalBits + 7) / 8;
    return encodedBytes < count * sizeof(uint64_t);
}

bool varintEliasDeltaIsBeneficial(const uint64_t *values, size_t count) {
    size_t totalBits = 0;
    for (size_t i = 0; i < count; i++) {
        if (values[i] < 1) {
            return false; /* Cannot encode 0 */
        }
        totalBits += varintEliasDeltaBits(values[i]);
    }

    size_t encodedBytes = (totalBits + 7) / 8;
    return encodedBytes < count * sizeof(uint64_t);
}

/* ====================================================================
 * Unit Tests
 * ==================================================================== */
#ifdef VARINT_ELIAS_TEST
#include "ctest.h"

int varintEliasTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int32_t err = 0;

    TEST("Gamma code bit counts") {
        /* 1 = 1 (1 bit), 2 = 010 (3 bits), 3 = 011 (3 bits) */
        /* 4 = 00100 (5 bits), 5 = 00101 (5 bits) */
        if (varintEliasGammaBits(1) != 1) {
            ERR("Gamma(1) should be 1 bit, got %zu", varintEliasGammaBits(1));
        }
        if (varintEliasGammaBits(2) != 3) {
            ERR("Gamma(2) should be 3 bits, got %zu", varintEliasGammaBits(2));
        }
        if (varintEliasGammaBits(4) != 5) {
            ERR("Gamma(4) should be 5 bits, got %zu", varintEliasGammaBits(4));
        }
        if (varintEliasGammaBits(8) != 7) {
            ERR("Gamma(8) should be 7 bits, got %zu", varintEliasGammaBits(8));
        }
    }

    TEST("Gamma encode/decode single values") {
        uint8_t buffer[16];
        varintBitWriter writer;
        varintBitReader reader;

        uint64_t testValues[] = {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 100, 1000};
        size_t numTests = sizeof(testValues) / sizeof(testValues[0]);

        for (size_t i = 0; i < numTests; i++) {
            uint64_t val = testValues[i];
            varintBitWriterInit(&writer, buffer, sizeof(buffer));
            size_t bits = varintEliasGammaEncode(&writer, val);

            varintBitReaderInit(&reader, buffer, bits);
            uint64_t decoded = varintEliasGammaDecode(&reader);

            if (decoded != val) {
                ERR("Gamma roundtrip failed: %llu -> %llu",
                    (unsigned long long)val, (unsigned long long)decoded);
            }
        }
    }

    TEST("Gamma encode/decode array") {
        uint64_t values[] = {1, 2, 3, 4, 5, 10, 100, 255};
        size_t count = sizeof(values) / sizeof(values[0]);

        uint8_t encoded[128]; /* Must be >= varintEliasGammaMaxBytes(count) */
        varintEliasMeta meta;
        size_t encodedBytes =
            varintEliasGammaEncodeArray(encoded, values, count, &meta);

        if (meta.count != count) {
            ERR("Meta count mismatch: expected %zu, got %zu", count,
                meta.count);
        }

        uint64_t decoded[10];
        size_t decodedCount =
            varintEliasGammaDecodeArray(encoded, meta.totalBits, decoded, 10);

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

        (void)encodedBytes;
    }

    TEST("Delta code bit counts") {
        /* Delta is more efficient for larger values */
        if (varintEliasDeltaBits(1) != 1) {
            ERR("Delta(1) should be 1 bit, got %zu", varintEliasDeltaBits(1));
        }
        /* For value=2: n=1, len=2, gamma(2)=3 bits + 1 remaining = 4 bits */
        if (varintEliasDeltaBits(2) != 4) {
            ERR("Delta(2) should be 4 bits, got %zu", varintEliasDeltaBits(2));
        }
    }

    TEST("Delta encode/decode single values") {
        uint8_t buffer[16];
        varintBitWriter writer;
        varintBitReader reader;

        uint64_t testValues[] = {1,  2,  3,   4,    5,     7,      8,      9,
                                 15, 16, 100, 1000, 10000, 100000, 1000000};
        size_t numTests = sizeof(testValues) / sizeof(testValues[0]);

        for (size_t i = 0; i < numTests; i++) {
            uint64_t val = testValues[i];
            varintBitWriterInit(&writer, buffer, sizeof(buffer));
            size_t bits = varintEliasDeltaEncode(&writer, val);

            varintBitReaderInit(&reader, buffer, bits);
            uint64_t decoded = varintEliasDeltaDecode(&reader);

            if (decoded != val) {
                ERR("Delta roundtrip failed: %llu -> %llu",
                    (unsigned long long)val, (unsigned long long)decoded);
            }
        }
    }

    TEST("Delta encode/decode array") {
        uint64_t values[] = {1, 5, 10, 50, 100, 500, 1000, 5000};
        size_t count = sizeof(values) / sizeof(values[0]);

        uint8_t encoded[80]; /* Must be >= varintEliasDeltaMaxBytes(count) */
        varintEliasMeta meta;
        size_t encodedBytes =
            varintEliasDeltaEncodeArray(encoded, values, count, &meta);

        if (meta.count != count) {
            ERR("Meta count mismatch: expected %zu, got %zu", count,
                meta.count);
        }

        uint64_t decoded[10];
        size_t decodedCount =
            varintEliasDeltaDecodeArray(encoded, meta.totalBits, decoded, 10);

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

        (void)encodedBytes;
    }

    TEST("Compression benefit analysis") {
        /* Small values - Gamma should be beneficial */
        const uint64_t small[] = {1, 2, 3, 1, 2, 1, 1, 2, 3, 1};
        if (!varintEliasGammaIsBeneficial(small, 10)) {
            ERRR("Gamma should be beneficial for small values");
        }

        /* Large values - neither may be beneficial */
        uint64_t large[10];
        for (size_t i = 0; i < 10; i++) {
            large[i] = UINT64_MAX - i;
        }
        if (varintEliasGammaIsBeneficial(large, 10)) {
            ERRR("Gamma should NOT be beneficial for large values");
        }
    }

    TEST("Large value encoding") {
        uint8_t buffer[32];
        varintBitWriter writer;
        varintBitReader reader;

        /* Test with moderately large value */
        uint64_t val = (1ULL << 32) - 1; /* 4 billion */

        varintBitWriterInit(&writer, buffer, sizeof(buffer));
        size_t bits = varintEliasDeltaEncode(&writer, val);

        varintBitReaderInit(&reader, buffer, bits);
        uint64_t decoded = varintEliasDeltaDecode(&reader);

        if (decoded != val) {
            ERR("Large value roundtrip failed: %llu -> %llu",
                (unsigned long long)val, (unsigned long long)decoded);
        }
    }

    TEST("Gamma powers of 2") {
        /* Test all powers of 2 which are important boundary cases */
        uint8_t buffer[32];
        varintBitWriter writer;
        varintBitReader reader;

        for (int p = 0; p < 60; p++) {
            uint64_t val = 1ULL << p;

            varintBitWriterInit(&writer, buffer, sizeof(buffer));
            size_t bits = varintEliasGammaEncode(&writer, val);

            /* Power of 2: n = p zeros, then n+1 = p+1 bits */
            size_t expectedBits = 2 * p + 1;
            if (bits != expectedBits) {
                ERR("Gamma(2^%d): expected %zu bits, got %zu", p, expectedBits,
                    bits);
            }

            varintBitReaderInit(&reader, buffer, bits);
            uint64_t decoded = varintEliasGammaDecode(&reader);

            if (decoded != val) {
                ERR("Gamma(2^%d) roundtrip failed: %llu -> %llu", p,
                    (unsigned long long)val, (unsigned long long)decoded);
            }
        }
    }

    TEST("Delta powers of 2") {
        uint8_t buffer[32];
        varintBitWriter writer;
        varintBitReader reader;

        for (int p = 0; p < 60; p++) {
            uint64_t val = 1ULL << p;

            varintBitWriterInit(&writer, buffer, sizeof(buffer));
            size_t bits = varintEliasDeltaEncode(&writer, val);

            varintBitReaderInit(&reader, buffer, bits);
            uint64_t decoded = varintEliasDeltaDecode(&reader);

            if (decoded != val) {
                ERR("Delta(2^%d) roundtrip failed: %llu -> %llu", p,
                    (unsigned long long)val, (unsigned long long)decoded);
            }
        }
    }

    TEST("Gamma vs Delta efficiency comparison") {
        /* Delta should be more efficient for larger values */
        uint64_t testValues[] = {1, 10, 100, 1000, 10000, 100000, 1000000};
        size_t numTests = sizeof(testValues) / sizeof(testValues[0]);

        for (size_t i = 0; i < numTests; i++) {
            uint64_t val = testValues[i];
            size_t gammaBits = varintEliasGammaBits(val);
            size_t deltaBits = varintEliasDeltaBits(val);

            /* Delta should be <= Gamma for values > 31 */
            if (val > 31 && deltaBits > gammaBits) {
                ERR("Delta should be more efficient for %llu: "
                    "gamma=%zu, delta=%zu",
                    (unsigned long long)val, gammaBits, deltaBits);
            }
        }
    }

    TEST("Multiple values in sequence") {
        uint8_t buffer[128];
        varintBitWriter writer;
        varintBitReader reader;

        const uint64_t values[] = {1, 2, 3, 4, 5, 10, 100, 1000};
        size_t count = sizeof(values) / sizeof(values[0]);

        /* Encode all values sequentially */
        varintBitWriterInit(&writer, buffer, sizeof(buffer));
        size_t totalBits = 0;
        for (size_t i = 0; i < count; i++) {
            totalBits += varintEliasGammaEncode(&writer, values[i]);
        }

        /* Decode all values */
        varintBitReaderInit(&reader, buffer, totalBits);
        for (size_t i = 0; i < count; i++) {
            uint64_t decoded = varintEliasGammaDecode(&reader);
            if (decoded != values[i]) {
                ERR("Sequence decode[%zu]: expected %llu, got %llu", i,
                    (unsigned long long)values[i], (unsigned long long)decoded);
            }
        }
    }

    TEST("BitWriter/BitReader edge cases") {
        uint8_t buffer[16];
        varintBitWriter writer;
        varintBitReader reader;

        /* Test single bit writes */
        varintBitWriterInit(&writer, buffer, sizeof(buffer));
        varintBitWriterWrite(&writer, 1, 1);
        varintBitWriterWrite(&writer, 0, 1);
        varintBitWriterWrite(&writer, 1, 1);
        varintBitWriterWrite(&writer, 1, 1);
        /* Should be 0b1011 = 0xB in first 4 bits */

        varintBitReaderInit(&reader, buffer, 4);
        if (varintBitReaderRead(&reader, 1) != 1) {
            ERRR("Bit 0 should be 1");
        }
        if (varintBitReaderRead(&reader, 1) != 0) {
            ERRR("Bit 1 should be 0");
        }
        if (varintBitReaderRead(&reader, 1) != 1) {
            ERRR("Bit 2 should be 1");
        }
        if (varintBitReaderRead(&reader, 1) != 1) {
            ERRR("Bit 3 should be 1");
        }

        /* Test multi-bit values */
        varintBitWriterInit(&writer, buffer, sizeof(buffer));
        varintBitWriterWrite(&writer, 0xABCD, 16);

        varintBitReaderInit(&reader, buffer, 16);
        uint64_t val = varintBitReaderRead(&reader, 16);
        if (val != 0xABCD) {
            ERR("16-bit write/read: expected 0xABCD, got 0x%llX",
                (unsigned long long)val);
        }

        /* Test bytes written calculation */
        varintBitWriterInit(&writer, buffer, sizeof(buffer));
        varintBitWriterWrite(&writer, 0xFF, 7); /* 7 bits */
        if (varintBitWriterBytes(&writer) != 1) {
            ERR("7 bits should use 1 byte, got %zu",
                varintBitWriterBytes(&writer));
        }
        varintBitWriterWrite(&writer, 1, 1); /* Now 8 bits */
        if (varintBitWriterBytes(&writer) != 1) {
            ERR("8 bits should use 1 byte, got %zu",
                varintBitWriterBytes(&writer));
        }
        varintBitWriterWrite(&writer, 1, 1); /* Now 9 bits */
        if (varintBitWriterBytes(&writer) != 2) {
            ERR("9 bits should use 2 bytes, got %zu",
                varintBitWriterBytes(&writer));
        }
    }

    TEST("BitReader hasMore") {
        const uint8_t buffer[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        varintBitReader reader;

        varintBitReaderInit(&reader, buffer, 32);

        if (!varintBitReaderHasMore(&reader, 32)) {
            ERRR("Should have 32 bits available");
        }
        if (varintBitReaderHasMore(&reader, 33)) {
            ERRR("Should not have 33 bits available");
        }

        /* Read 16 bits */
        varintBitReaderRead(&reader, 16);

        if (!varintBitReaderHasMore(&reader, 16)) {
            ERRR("Should have 16 bits remaining");
        }
        if (varintBitReaderHasMore(&reader, 17)) {
            ERRR("Should not have 17 bits remaining");
        }

        /* Read remaining */
        varintBitReaderRead(&reader, 16);
        if (varintBitReaderHasMore(&reader, 1)) {
            ERRR("Should have no bits remaining");
        }
    }

    TEST("Gamma boundary values") {
        uint8_t buffer[32];
        varintBitWriter writer;
        varintBitReader reader;

        /* Test values just before and after powers of 2 */
        uint64_t testValues[] = {
            1,     2,     3,     /* Smallest values */
            7,     8,     9,     /* Around 2^3 */
            15,    16,    17,    /* Around 2^4 */
            127,   128,   129,   /* Around 2^7 */
            255,   256,   257,   /* Around 2^8 */
            65535, 65536, 65537, /* Around 2^16 */
        };
        size_t numTests = sizeof(testValues) / sizeof(testValues[0]);

        for (size_t i = 0; i < numTests; i++) {
            uint64_t val = testValues[i];

            varintBitWriterInit(&writer, buffer, sizeof(buffer));
            varintEliasGammaEncode(&writer, val);

            varintBitReaderInit(&reader, buffer, varintEliasGammaBits(val));
            uint64_t decoded = varintEliasGammaDecode(&reader);

            if (decoded != val) {
                ERR("Gamma boundary %llu roundtrip failed: got %llu",
                    (unsigned long long)val, (unsigned long long)decoded);
            }
        }
    }

    TEST("Delta boundary values") {
        uint8_t buffer[32];
        varintBitWriter writer;
        varintBitReader reader;

        uint64_t testValues[] = {
            1,
            2,
            3,
            7,
            8,
            9,
            15,
            16,
            17,
            127,
            128,
            129,
            255,
            256,
            257,
            65535,
            65536,
            65537,
            (1ULL << 30) - 1,
            1ULL << 30,
            (1ULL << 30) + 1,
        };
        size_t numTests = sizeof(testValues) / sizeof(testValues[0]);

        for (size_t i = 0; i < numTests; i++) {
            uint64_t val = testValues[i];

            varintBitWriterInit(&writer, buffer, sizeof(buffer));
            varintEliasDeltaEncode(&writer, val);

            varintBitReaderInit(&reader, buffer, varintEliasDeltaBits(val));
            uint64_t decoded = varintEliasDeltaDecode(&reader);

            if (decoded != val) {
                ERR("Delta boundary %llu roundtrip failed: got %llu",
                    (unsigned long long)val, (unsigned long long)decoded);
            }
        }
    }

    TEST("Gamma array various sizes") {
        size_t testSizes[] = {1, 2, 10, 100, 1000};

        for (size_t t = 0; t < sizeof(testSizes) / sizeof(testSizes[0]); t++) {
            size_t count = testSizes[t];
            uint64_t *values = malloc(count * sizeof(uint64_t));
            if (!values) {
                continue;
            }

            /* Generate test values */
            for (size_t i = 0; i < count; i++) {
                values[i] = (i % 100) + 1; /* 1-100, must be >= 1 */
            }

            uint8_t *encoded = malloc(varintEliasGammaMaxBytes(count));
            if (!encoded) {
                free(values);
                continue;
            }
            varintEliasMeta meta;
            varintEliasGammaEncodeArray(encoded, values, count, &meta);

            if (meta.count != count) {
                ERR("Gamma array size %zu: meta.count=%zu", count, meta.count);
            }

            uint64_t *decoded = malloc(count * sizeof(uint64_t));
            if (!decoded) {
                free(values);
                free(encoded);
                continue;
            }
            size_t decodedCount = varintEliasGammaDecodeArray(
                encoded, meta.totalBits, decoded, count);

            if (decodedCount != count) {
                ERR("Gamma array size %zu: decoded %zu values", count,
                    decodedCount);
            }

            bool match = true;
            for (size_t i = 0; i < count && match; i++) {
                if (decoded[i] != values[i]) {
                    ERR("Gamma array size %zu: mismatch at %zu", count, i);
                    match = false;
                }
            }

            free(values);
            free(encoded);
            free(decoded);
        }
    }

    TEST("Delta array various sizes") {
        size_t testSizes[] = {1, 2, 10, 100, 1000};

        for (size_t t = 0; t < sizeof(testSizes) / sizeof(testSizes[0]); t++) {
            size_t count = testSizes[t];
            uint64_t *values = malloc(count * sizeof(uint64_t));
            if (!values) {
                continue;
            }

            /* Generate test values with larger range */
            for (size_t i = 0; i < count; i++) {
                values[i] = (i * 100) + 1; /* 1, 101, 201, ... */
            }

            uint8_t *encoded = malloc(varintEliasDeltaMaxBytes(count));
            if (!encoded) {
                free(values);
                continue;
            }
            varintEliasMeta meta;
            varintEliasDeltaEncodeArray(encoded, values, count, &meta);

            if (meta.count != count) {
                ERR("Delta array size %zu: meta.count=%zu", count, meta.count);
            }

            uint64_t *decoded = malloc(count * sizeof(uint64_t));
            if (!decoded) {
                free(values);
                free(encoded);
                continue;
            }
            size_t decodedCount = varintEliasDeltaDecodeArray(
                encoded, meta.totalBits, decoded, count);

            if (decodedCount != count) {
                ERR("Delta array size %zu: decoded %zu values", count,
                    decodedCount);
            }

            bool match = true;
            for (size_t i = 0; i < count && match; i++) {
                if (decoded[i] != values[i]) {
                    ERR("Delta array size %zu: mismatch at %zu", count, i);
                    match = false;
                }
            }

            free(values);
            free(encoded);
            free(decoded);
        }
    }

    TEST("All ones pattern (worst case for Gamma)") {
        /* All 1s is the best case for Gamma (1 bit each) */
        size_t count = 1000;
        uint64_t *values = malloc(count * sizeof(uint64_t));
        if (!values) {
            ERRR("malloc failed");
        }
        for (size_t i = 0; i < count; i++) {
            values[i] = 1;
        }

        uint8_t *encoded = malloc(varintEliasGammaMaxBytes(count));
        if (!encoded) {
            free(values);
            ERRR("malloc failed");
        }
        varintEliasMeta meta;
        varintEliasGammaEncodeArray(encoded, values, count, &meta);

        /* 1000 values of 1 should take exactly 1000 bits = 125 bytes */
        if (meta.totalBits != count) {
            ERR("All ones: expected %zu bits, got %zu", count, meta.totalBits);
        }

        uint64_t *decoded = malloc(count * sizeof(uint64_t));
        if (!decoded) {
            free(values);
            free(encoded);
            ERRR("malloc failed");
        }
        size_t decodedCount = varintEliasGammaDecodeArray(
            encoded, meta.totalBits, decoded, count);

        if (decodedCount != count) {
            ERR("All ones: decoded %zu values", decodedCount);
        }

        for (size_t i = 0; i < count; i++) {
            if (decoded[i] != 1) {
                ERR("All ones: mismatch at %zu", i);
                break;
            }
        }

        free(values);
        free(encoded);
        free(decoded);
    }

    TEST("Stress test large arrays") {
        size_t count = 10000;
        uint64_t *values = malloc(count * sizeof(uint64_t));
        if (!values) {
            ERRR("malloc failed");
        }

        /* Generate values with mixed sizes */
        for (size_t i = 0; i < count; i++) {
            /* Values ranging from 1 to ~1000 */
            values[i] = (i % 1000) + 1;
        }

        /* Test Gamma */
        uint8_t *encodedGamma = malloc(varintEliasGammaMaxBytes(count));
        if (!encodedGamma) {
            free(values);
            ERRR("malloc failed");
        }
        varintEliasMeta metaGamma;
        varintEliasGammaEncodeArray(encodedGamma, values, count, &metaGamma);

        uint64_t *decodedGamma = malloc(count * sizeof(uint64_t));
        if (!decodedGamma) {
            free(values);
            free(encodedGamma);
            ERRR("malloc failed");
        }
        size_t decodedCountGamma = varintEliasGammaDecodeArray(
            encodedGamma, metaGamma.totalBits, decodedGamma, count);

        if (decodedCountGamma != count) {
            ERR("Gamma stress: decoded %zu values", decodedCountGamma);
        }

        bool matchGamma = true;
        for (size_t i = 0; i < count && matchGamma; i++) {
            if (decodedGamma[i] != values[i]) {
                ERR("Gamma stress: mismatch at %zu", i);
                matchGamma = false;
            }
        }

        /* Test Delta */
        uint8_t *encodedDelta = malloc(varintEliasDeltaMaxBytes(count));
        if (!encodedDelta) {
            free(values);
            free(encodedGamma);
            free(decodedGamma);
            ERRR("malloc failed");
        }
        varintEliasMeta metaDelta;
        varintEliasDeltaEncodeArray(encodedDelta, values, count, &metaDelta);

        uint64_t *decodedDelta = malloc(count * sizeof(uint64_t));
        if (!decodedDelta) {
            free(values);
            free(encodedGamma);
            free(decodedGamma);
            free(encodedDelta);
            ERRR("malloc failed");
        }
        size_t decodedCountDelta = varintEliasDeltaDecodeArray(
            encodedDelta, metaDelta.totalBits, decodedDelta, count);

        if (decodedCountDelta != count) {
            ERR("Delta stress: decoded %zu values", decodedCountDelta);
        }

        bool matchDelta = true;
        for (size_t i = 0; i < count && matchDelta; i++) {
            if (decodedDelta[i] != values[i]) {
                ERR("Delta stress: mismatch at %zu", i);
                matchDelta = false;
            }
        }

        free(values);
        free(encodedGamma);
        free(decodedGamma);
        free(encodedDelta);
        free(decodedDelta);
    }

    TEST("Meta structure verification") {
        const uint64_t values[] = {1, 10, 100, 1000};
        size_t count = 4;

        uint8_t encodedGamma[128];
        varintEliasMeta metaGamma;
        size_t bytesGamma = varintEliasGammaEncodeArray(encodedGamma, values,
                                                        count, &metaGamma);

        /* Verify meta fields */
        if (metaGamma.count != count) {
            ERR("Gamma meta.count: expected %zu, got %zu", count,
                metaGamma.count);
        }
        if (metaGamma.encodedBytes != bytesGamma) {
            ERR("Gamma meta.encodedBytes: expected %zu, got %zu", bytesGamma,
                metaGamma.encodedBytes);
        }
        if (metaGamma.encodedBytes != (metaGamma.totalBits + 7) / 8) {
            ERRR("Gamma meta.encodedBytes inconsistent with totalBits");
        }

        uint8_t encodedDelta[128];
        varintEliasMeta metaDelta;
        size_t bytesDelta = varintEliasDeltaEncodeArray(encodedDelta, values,
                                                        count, &metaDelta);

        if (metaDelta.count != count) {
            ERR("Delta meta.count: expected %zu, got %zu", count,
                metaDelta.count);
        }
        if (metaDelta.encodedBytes != bytesDelta) {
            ERR("Delta meta.encodedBytes: expected %zu, got %zu", bytesDelta,
                metaDelta.encodedBytes);
        }
    }

    TEST_FINAL_RESULT;
    return 0;
}

#endif /* VARINT_ELIAS_TEST */
