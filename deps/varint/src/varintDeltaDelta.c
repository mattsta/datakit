#include "varintDeltaDelta.h"
#include <assert.h>
#include <string.h>

/* ====================================================================
 * SIMD / SWAR Platform Detection
 * ====================================================================
 * Delta-of-delta is fundamentally sequential at decode (each dod depends
 * on the running delta) so the SIMD opportunity is in the *encode*
 * analysis pass (predicting size, counting zero dods) where the work
 * is independent per element after the two delta passes. */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#define VARINT_DELTA_DELTA_NEON 1
#include <arm_neon.h>
#endif

#if defined(__AVX2__)
#define VARINT_DELTA_DELTA_AVX2 1
#include <immintrin.h>
#endif

/* ====================================================================
 * Internal helpers
 * ==================================================================== */

/* Width prediction for a ZigZag-encoded signed value. Mirrors what
 * varintDeltaPut would emit so analysis matches encode byte-for-byte. */
static inline varintWidth deltaDeltaPredictedWidth_(int64_t signedValue) {
    uint64_t zz = varintDeltaZigZag(signedValue);
    varintWidth w;
    varintExternalUnsignedEncoding(zz, w);
    /* +1 for the width tag byte we always emit. */
    return (varintWidth)(1 + (uint8_t)w);
}

/* Wrapping (mod-2^64) signed subtraction. Wrapping — not saturating —
 * because the decoder reconstructs with wrapping additions, so wrapped
 * deltas round-trip exactly for ALL int64 inputs; saturation silently
 * corrupts extreme-range pairs. Unsigned math avoids the overflow UB. */
static inline int64_t deltaDeltaSubWrap_(int64_t a, int64_t b) {
    return (int64_t)((uint64_t)a - (uint64_t)b);
}

/* Wrapping signed addition; pairs with deltaDeltaSubWrap_. */
static inline int64_t deltaDeltaAddWrap_(int64_t a, int64_t b) {
    return (int64_t)((uint64_t)a + (uint64_t)b);
}

/* ====================================================================
 * Analysis
 * ==================================================================== */

bool varintDeltaDeltaAnalyze(const int64_t *values, size_t count,
                             varintDeltaDeltaMeta *meta) {
    assert(meta != NULL);

    meta->count = count;
    meta->encodedSize = 0;
    meta->zeroDoD = 0;
    meta->oneByteDoD = 0;

    if (count == 0) {
        return false;
    }

    /* Base value */
    meta->encodedSize += deltaDeltaPredictedWidth_(values[0]);
    if (count == 1) {
        return meta->encodedSize < count * sizeof(int64_t);
    }

    /* First delta */
    int64_t prevDelta = deltaDeltaSubWrap_(values[1], values[0]);
    meta->encodedSize += deltaDeltaPredictedWidth_(prevDelta);

    /* Delta-of-deltas. Mostly tiny for regular streams — count those
     * that fit in 1 byte payload as a quality signal. */
    for (size_t i = 2; i < count; i++) {
        int64_t curDelta = deltaDeltaSubWrap_(values[i], values[i - 1]);
        int64_t dod = deltaDeltaSubWrap_(curDelta, prevDelta);
        prevDelta = curDelta;

        if (dod == 0) {
            meta->zeroDoD++;
        }
        varintWidth w = deltaDeltaPredictedWidth_(dod);
        if (w <= 2) {
            /* 1-byte width tag + 1-byte payload */
            meta->oneByteDoD++;
        }
        meta->encodedSize += w;
    }

    return meta->encodedSize < count * sizeof(int64_t);
}

/* ====================================================================
 * Signed array encode/decode
 * ==================================================================== */

size_t varintDeltaDeltaEncode(uint8_t *output, const int64_t *values,
                              size_t count, varintDeltaDeltaMeta *meta) {
    assert(output != NULL);
    assert(values != NULL || count == 0);

    if (meta) {
        meta->count = count;
        meta->encodedSize = 0;
        meta->zeroDoD = 0;
        meta->oneByteDoD = 0;
    }

    if (count == 0) {
        return 0;
    }

    uint8_t *p = output;

    /* Base value */
    p += varintDeltaPut(p, values[0]);
    if (count == 1) {
        size_t total = (size_t)(p - output);
        if (meta) {
            meta->encodedSize = total;
        }
        return total;
    }

    /* First delta */
    int64_t prevDelta = deltaDeltaSubWrap_(values[1], values[0]);
    p += varintDeltaPut(p, prevDelta);

    /* Delta-of-deltas */
    for (size_t i = 2; i < count; i++) {
        int64_t curDelta = deltaDeltaSubWrap_(values[i], values[i - 1]);
        int64_t dod = deltaDeltaSubWrap_(curDelta, prevDelta);
        prevDelta = curDelta;

        if (meta) {
            if (dod == 0) {
                meta->zeroDoD++;
            }
        }

        varintWidth written = varintDeltaPut(p, dod);
        if (meta && written <= 2) {
            meta->oneByteDoD++;
        }
        p += written;
    }

    size_t total = (size_t)(p - output);
    if (meta) {
        meta->encodedSize = total;
    }
    return total;
}

size_t varintDeltaDeltaDecode(const uint8_t *input, size_t count,
                              int64_t *output) {
    assert(input != NULL);
    assert(output != NULL || count == 0);

    if (count == 0) {
        return 0;
    }

    const uint8_t *p = input;

    /* Base value */
    int64_t base;
    p += varintDeltaGet(p, &base);
    output[0] = base;
    if (count == 1) {
        return (size_t)(p - input);
    }

    /* First delta — second value is base + first delta */
    int64_t prevDelta;
    p += varintDeltaGet(p, &prevDelta);
    int64_t prev = deltaDeltaAddWrap_(base, prevDelta);
    output[1] = prev;

    /* Reconstruct from dods */
    for (size_t i = 2; i < count; i++) {
        int64_t dod;
        p += varintDeltaGet(p, &dod);
        prevDelta = deltaDeltaAddWrap_(prevDelta, dod);
        prev = deltaDeltaAddWrap_(prev, prevDelta);
        output[i] = prev;
    }

    return (size_t)(p - input);
}

/* ====================================================================
 * Unsigned array encode/decode
 * ==================================================================== */

size_t varintDeltaDeltaEncodeUnsigned(uint8_t *output, const uint64_t *values,
                                      size_t count,
                                      varintDeltaDeltaMeta *meta) {
    assert(output != NULL);
    assert(values != NULL || count == 0);

    if (meta) {
        meta->count = count;
        meta->encodedSize = 0;
        meta->zeroDoD = 0;
        meta->oneByteDoD = 0;
    }

    if (count == 0) {
        return 0;
    }

    uint8_t *p = output;

    /* Base value: store as unsigned via width + raw payload to preserve
     * full uint64 range (the signed path would clamp at INT64_MAX). */
    varintWidth baseW;
    varintExternalUnsignedEncoding(values[0], baseW);
    *p++ = (uint8_t)baseW;
    varintExternalPutFixedWidth(p, values[0], baseW);
    p += baseW;

    if (count == 1) {
        size_t total = (size_t)(p - output);
        if (meta) {
            meta->encodedSize = total;
        }
        return total;
    }

    /* First delta — wraps modulo 2^64 then interpreted signed for ZigZag. */
    int64_t prevDelta = (int64_t)(values[1] - values[0]);
    p += varintDeltaPut(p, prevDelta);

    for (size_t i = 2; i < count; i++) {
        int64_t curDelta = (int64_t)(values[i] - values[i - 1]);
        int64_t dod = deltaDeltaSubWrap_(curDelta, prevDelta);
        prevDelta = curDelta;

        if (meta && dod == 0) {
            meta->zeroDoD++;
        }
        varintWidth written = varintDeltaPut(p, dod);
        if (meta && written <= 2) {
            meta->oneByteDoD++;
        }
        p += written;
    }

    size_t total = (size_t)(p - output);
    if (meta) {
        meta->encodedSize = total;
    }
    return total;
}

size_t varintDeltaDeltaDecodeUnsigned(const uint8_t *input, size_t count,
                                      uint64_t *output) {
    assert(input != NULL);
    assert(output != NULL || count == 0);

    if (count == 0) {
        return 0;
    }

    const uint8_t *p = input;

    /* Base */
    varintWidth baseW = (varintWidth)(*p++);
    uint64_t base = varintExternalGet(p, baseW);
    p += baseW;
    output[0] = base;

    if (count == 1) {
        return (size_t)(p - input);
    }

    int64_t prevDelta;
    p += varintDeltaGet(p, &prevDelta);
    uint64_t prev = base + (uint64_t)prevDelta;
    output[1] = prev;

    for (size_t i = 2; i < count; i++) {
        int64_t dod;
        p += varintDeltaGet(p, &dod);
        prevDelta = deltaDeltaAddWrap_(prevDelta, dod);
        prev += (uint64_t)prevDelta;
        output[i] = prev;
    }

    return (size_t)(p - input);
}

/* ====================================================================
 * Unit Tests
 * ==================================================================== */
#ifdef VARINT_DELTA_DELTA_TEST
#include "ctest.h"
#include <stdio.h>
#include <stdlib.h>

int varintDeltaDeltaTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int32_t err = 0;

    TEST("DoD basic roundtrip — regular interval timestamps") {
        /* Classic Gorilla case: timestamps every 60 seconds.
         * dod = 0 for all but first two entries. */
        int64_t values[10];
        int64_t base = 1700000000;
        for (size_t i = 0; i < 10; i++) {
            values[i] = base + (int64_t)i * 60;
        }

        uint8_t buf[varintDeltaDeltaMaxEncodedSize(10)];
        varintDeltaDeltaMeta meta;
        size_t written = varintDeltaDeltaEncode(buf, values, 10, &meta);

        if (meta.count != 10) {
            ERR("Expected count=10, got %zu", meta.count);
        }
        /* 8 of 10 entries are dods, all should be zero */
        if (meta.zeroDoD != 8) {
            ERR("Expected zeroDoD=8, got %zu", meta.zeroDoD);
        }

        int64_t decoded[10];
        size_t read = varintDeltaDeltaDecode(buf, 10, decoded);
        if (read != written) {
            ERR("Round-trip byte count mismatch: wrote %zu, read %zu", written,
                read);
        }
        for (size_t i = 0; i < 10; i++) {
            if (decoded[i] != values[i]) {
                ERR("Value mismatch at %zu: %lld vs %lld", i,
                    (long long)values[i], (long long)decoded[i]);
            }
        }
    }

    TEST("DoD compression on linear ramp — should crush") {
        size_t count = 1000;
        int64_t *vals = malloc(count * sizeof(int64_t));
        for (size_t i = 0; i < count; i++) {
            vals[i] = 10000 + (int64_t)i * 7;
        }

        uint8_t *buf = malloc(varintDeltaDeltaMaxEncodedSize(count));
        varintDeltaDeltaMeta meta;
        size_t written = varintDeltaDeltaEncode(buf, vals, count, &meta);

        /* Linear ramp: all dods are zero except boundaries */
        if (meta.zeroDoD < count - 3) {
            ERR("Expected near-all-zero dods, got %zu of %zu", meta.zeroDoD,
                count - 2);
        }
        /* Linear int64 = 8 KB raw. Our byte-tagged DoD format emits ~2
         * bytes per zero-dod element (1 width tag + 1 byte payload),
         * giving ~4x compression. (True Gorilla bit-packing would beat
         * this further; left as a future varintGorilla bit-level mode.) */
        if (written > count * 3) {
            ERR("Encoding too large for linear ramp: %zu bytes for %zu",
                written, count);
        }

        int64_t *dec = malloc(count * sizeof(int64_t));
        size_t read = varintDeltaDeltaDecode(buf, count, dec);
        if (read != written) {
            ERR("byte count mismatch: %zu vs %zu", read, written);
        }
        for (size_t i = 0; i < count; i++) {
            if (dec[i] != vals[i]) {
                ERR("mismatch at %zu", i);
                break;
            }
        }
        free(vals);
        free(buf);
        free(dec);
    }

    TEST("DoD signed values + negative deltas") {
        int64_t values[] = {-1000, -500, 0, 500, 1000, 500, 0, -500};
        size_t count = sizeof(values) / sizeof(values[0]);

        uint8_t buf[256];
        size_t written = varintDeltaDeltaEncode(buf, values, count, NULL);

        int64_t dec[8];
        size_t read = varintDeltaDeltaDecode(buf, count, dec);

        if (read != written) {
            ERR("byte count mismatch: %zu vs %zu", read, written);
        }
        for (size_t i = 0; i < count; i++) {
            if (dec[i] != values[i]) {
                ERR("signed mismatch at %zu: %lld vs %lld", i,
                    (long long)values[i], (long long)dec[i]);
            }
        }
    }

    TEST("DoD unsigned roundtrip preserving full uint64 range") {
        uint64_t values[5] = {(uint64_t)1 << 63, ((uint64_t)1 << 63) + 1000,
                              ((uint64_t)1 << 63) + 2000,
                              ((uint64_t)1 << 63) + 3000,
                              ((uint64_t)1 << 63) + 4000};

        uint8_t buf[256];
        varintDeltaDeltaMeta meta;
        size_t written = varintDeltaDeltaEncodeUnsigned(buf, values, 5, &meta);

        uint64_t dec[5];
        size_t read = varintDeltaDeltaDecodeUnsigned(buf, 5, dec);
        if (read != written) {
            ERR("unsigned byte count mismatch: %zu vs %zu", read, written);
        }
        for (size_t i = 0; i < 5; i++) {
            if (dec[i] != values[i]) {
                ERR("unsigned mismatch at %zu: %llu vs %llu", i,
                    (unsigned long long)values[i], (unsigned long long)dec[i]);
            }
        }
    }

    TEST("DoD analyze matches encode byte count") {
        int64_t values[] = {0, 60, 120, 181, 240, 300, 360, 420};
        size_t count = sizeof(values) / sizeof(values[0]);

        varintDeltaDeltaMeta analyzed;
        varintDeltaDeltaAnalyze(values, count, &analyzed);

        uint8_t buf[256];
        varintDeltaDeltaMeta encoded;
        size_t written = varintDeltaDeltaEncode(buf, values, count, &encoded);

        if (analyzed.encodedSize != encoded.encodedSize) {
            ERR("Analyze size %zu != encoded size %zu", analyzed.encodedSize,
                encoded.encodedSize);
        }
        if (analyzed.encodedSize != written) {
            ERR("Analyze size %zu != actual written %zu", analyzed.encodedSize,
                written);
        }
    }

    TEST("DoD edge cases: empty, single, double") {
        uint8_t buf[64];
        varintDeltaDeltaMeta meta;

        /* Empty */
        size_t w = varintDeltaDeltaEncode(buf, NULL, 0, &meta);
        if (w != 0 || meta.encodedSize != 0) {
            ERRR("empty encode failed");
        }

        /* Single */
        int64_t one = 42;
        w = varintDeltaDeltaEncode(buf, &one, 1, &meta);
        int64_t out1;
        varintDeltaDeltaDecode(buf, 1, &out1);
        if (out1 != 42) {
            ERR("single decode wrong: %lld", (long long)out1);
        }

        /* Double */
        int64_t two[2] = {100, 105};
        w = varintDeltaDeltaEncode(buf, two, 2, &meta);
        int64_t out2[2];
        varintDeltaDeltaDecode(buf, 2, out2);
        if (out2[0] != 100 || out2[1] != 105) {
            ERR("double decode wrong: [%lld,%lld]", (long long)out2[0],
                (long long)out2[1]);
        }
        (void)w;
    }

    TEST("DoD is beneficial for time series, not for random") {
        int64_t regular[100];
        for (size_t i = 0; i < 100; i++) {
            regular[i] = 1000000 + (int64_t)i * 60;
        }
        if (!varintDeltaDeltaIsBeneficial(regular, 100)) {
            ERRR("DoD should be beneficial for regular timestamps");
        }

        /* Random-ish wide-range data — DoD overhead may still hurt.
         * Just confirm it doesn't crash and reports a sane size. */
        int64_t scattered[20];
        for (size_t i = 0; i < 20; i++) {
            scattered[i] = (int64_t)(i * 1000003 + 7);
        }
        varintDeltaDeltaMeta meta;
        varintDeltaDeltaAnalyze(scattered, 20, &meta);
        if (meta.encodedSize == 0) {
            ERRR("analyze produced zero size for non-empty array");
        }
    }

    TEST_FINAL_RESULT;
    return 0;
}

#endif /* VARINT_DELTA_DELTA_TEST */
