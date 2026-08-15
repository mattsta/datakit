#include "varintStride.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================
 * SIMD / SWAR platform detection
 * ====================================================================
 * Stride detection is a perfect SIMD target: load N consecutive deltas
 * and compare-equal against the candidate stride broadcast. NEON gives
 * us int64x2_t pairwise equality; AVX2 gives us 4x int64 at once. */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#define VARINT_STRIDE_NEON 1
#include <arm_neon.h>
#endif

#if defined(__AVX2__)
#define VARINT_STRIDE_AVX2 1
#include <immintrin.h>
#endif

/* Minimum count to bother with SIMD vs scalar. */
#define VARINT_STRIDE_SIMD_MIN_COUNT 16

/* ====================================================================
 * Internal helpers
 * ==================================================================== */

/* Predicted bytes for a tagged-ZigZag signed varint write. */
static inline size_t stridePredictSigned_(int64_t v) {
    uint64_t zz = varintDeltaZigZag(v);
    varintWidth w;
    varintExternalUnsignedEncoding(zz, w);
    /* varintDeltaPut emits 1 width byte + w data bytes. */
    return 1 + (size_t)w;
}

/* Predicted bytes for a tagged-varint write (varintTaggedPut64). */
static inline size_t stridePredictTagged_(uint64_t v) {
    return (size_t)varintTaggedLen(v);
}

/* ====================================================================
 * Stride mismatch counting (SWAR / SIMD)
 * ====================================================================
 * Count how many values differ from the arithmetic progression
 * values[0] + i*stride (wrapping mod 2^64) — the exact sequence the
 * decoder materializes, so the exception set patches every deviating
 * position. (Comparing consecutive DELTAS against the stride is NOT
 * equivalent: one level shift changes a single delta but displaces
 * every subsequent value from the progression.)
 * Returns the mismatch count; also outputs an array of mismatch indices
 * (caller-allocated, size at least count-1). If excIdx is NULL, only
 * counts. */
static size_t strideCountMismatchesScalar_(const int64_t *values, size_t count,
                                           int64_t stride, size_t *excIdx) {
    size_t mismatches = 0;
    uint64_t expect = (uint64_t)values[0];
    for (size_t i = 1; i < count; i++) {
        expect += (uint64_t)stride;
        if ((uint64_t)values[i] != expect) {
            if (excIdx) {
                excIdx[mismatches] = i;
            }
            mismatches++;
        }
    }
    return mismatches;
}

#ifdef VARINT_STRIDE_NEON
/* NEON path — compare 2 values per iteration against an expected-value
 * vector that steps by 2*stride; no per-iteration loads beyond the data. */
static size_t strideCountMismatchesNEON_(const int64_t *values, size_t count,
                                         int64_t stride, size_t *excIdx) {
    if (count < 4) {
        return strideCountMismatchesScalar_(values, count, stride, excIdx);
    }

    size_t mismatches = 0;
    const uint64_t base = (uint64_t)values[0];
    const uint64_t s = (uint64_t)stride;

    uint64_t lanes[2] = {base + s, base + 2 * s};
    uint64x2_t expectVec = vld1q_u64(lanes);
    uint64x2_t stepVec = vdupq_n_u64(2 * s);

    size_t i = 1;
    for (; i + 1 < count; i += 2) {
        uint64x2_t cur = vld1q_u64((const uint64_t *)&values[i]);
        uint64x2_t eq = vceqq_u64(cur, expectVec);
        expectVec = vaddq_u64(expectVec, stepVec);

        if (vgetq_lane_u64(eq, 0) == 0) {
            if (excIdx) {
                excIdx[mismatches] = i;
            }
            mismatches++;
        }
        if (vgetq_lane_u64(eq, 1) == 0) {
            if (excIdx) {
                excIdx[mismatches] = i + 1;
            }
            mismatches++;
        }
    }
    /* Tail */
    for (; i < count; i++) {
        if ((uint64_t)values[i] != base + (uint64_t)i * s) {
            if (excIdx) {
                excIdx[mismatches] = i;
            }
            mismatches++;
        }
    }
    return mismatches;
}
#endif /* VARINT_STRIDE_NEON */

#ifdef VARINT_STRIDE_AVX2
static size_t strideCountMismatchesAVX2_(const int64_t *values, size_t count,
                                         int64_t stride, size_t *excIdx) {
    if (count < 8) {
        return strideCountMismatchesScalar_(values, count, stride, excIdx);
    }

    size_t mismatches = 0;
    const uint64_t base = (uint64_t)values[0];
    const uint64_t s = (uint64_t)stride;

    __m256i expectVec =
        _mm256_set_epi64x((long long)(base + 4 * s), (long long)(base + 3 * s),
                          (long long)(base + 2 * s), (long long)(base + s));
    const __m256i stepVec = _mm256_set1_epi64x((long long)(4 * s));

    size_t i = 1;
    for (; i + 3 < count; i += 4) {
        __m256i cur = _mm256_loadu_si256((const __m256i *)&values[i]);
        __m256i eq = _mm256_cmpeq_epi64(cur, expectVec);
        expectVec = _mm256_add_epi64(expectVec, stepVec);

        /* Extract mask: each int64 lane contributes 8 identical bits. */
        int mask = _mm256_movemask_epi8(eq);
        for (int lane = 0; lane < 4; lane++) {
            int laneByteMask = (mask >> (lane * 8)) & 0xFF;
            if (laneByteMask != 0xFF) {
                if (excIdx) {
                    excIdx[mismatches] = i + (size_t)lane;
                }
                mismatches++;
            }
        }
    }
    for (; i < count; i++) {
        if ((uint64_t)values[i] != base + (uint64_t)i * s) {
            if (excIdx) {
                excIdx[mismatches] = i;
            }
            mismatches++;
        }
    }
    return mismatches;
}
#endif /* VARINT_STRIDE_AVX2 */

/* Dispatcher */
static size_t strideCountMismatches_(const int64_t *values, size_t count,
                                     int64_t stride, size_t *excIdx) {
    if (count < VARINT_STRIDE_SIMD_MIN_COUNT) {
        return strideCountMismatchesScalar_(values, count, stride, excIdx);
    }
#if defined(VARINT_STRIDE_AVX2)
    return strideCountMismatchesAVX2_(values, count, stride, excIdx);
#elif defined(VARINT_STRIDE_NEON)
    return strideCountMismatchesNEON_(values, count, stride, excIdx);
#else
    return strideCountMismatchesScalar_(values, count, stride, excIdx);
#endif
}

/* ====================================================================
 * Analysis
 * ==================================================================== */

bool varintStrideAnalyze(const int64_t *values, size_t count,
                         varintStrideMeta *meta) {
    assert(meta != NULL);

    memset(meta, 0, sizeof(*meta));
    meta->count = count;
    meta->mode = VARINT_STRIDE_MODE_EXACT;

    if (count == 0) {
        meta->encodedSize = 0;
        return false;
    }
    meta->base = values[0];

    if (count == 1) {
        /* Trivial: just base. Mode exact, stride=0. */
        meta->stride = 0;
        meta->encodedSize = 1 + stridePredictSigned_(meta->base) +
                            stridePredictSigned_(0) + stridePredictTagged_(1);
        return meta->encodedSize < count * sizeof(int64_t);
    }

    meta->stride = (int64_t)((uint64_t)values[1] - (uint64_t)values[0]);

    /* Count mismatches against candidate stride. */
    size_t mismatches =
        strideCountMismatches_(values, count, meta->stride, NULL);

    size_t fixedHeader = 1 + stridePredictSigned_(meta->base) +
                         stridePredictSigned_(meta->stride) +
                         stridePredictTagged_(count);

    if (mismatches == 0) {
        meta->mode = VARINT_STRIDE_MODE_EXACT;
        meta->exceptionCount = 0;
        meta->encodedSize = fixedHeader;
        return meta->encodedSize < count * sizeof(int64_t);
    }

    /* Fuzzy candidate: only viable if mismatches stay below threshold. */
    if ((double)mismatches > (double)count * VARINT_STRIDE_FUZZY_MAX_FRACTION) {
        meta->mode = VARINT_STRIDE_MODE_FUZZY;
        meta->exceptionCount = mismatches;
        /* Predict encodedSize even when "not beneficial" — caller may
         * still want the number. */
        meta->encodedSize = fixedHeader + stridePredictTagged_(mismatches) +
                            mismatches * (stridePredictTagged_(count) +
                                          stridePredictSigned_(0));
        return false;
    }

    /* Fuzzy worth pursuing. Compute precise size by predicting each
     * exception's (idx, value) cost. */
    size_t *excIdx = malloc(mismatches * sizeof(size_t));
    if (!excIdx) {
        meta->mode = VARINT_STRIDE_MODE_FUZZY;
        meta->exceptionCount = mismatches;
        /* Pessimistic upper bound */
        meta->encodedSize =
            fixedHeader + stridePredictTagged_(mismatches) + mismatches * 18;
        return meta->encodedSize < count * sizeof(int64_t);
    }
    (void)strideCountMismatches_(values, count, meta->stride, excIdx);

    size_t excBytes = stridePredictTagged_(mismatches);
    for (size_t k = 0; k < mismatches; k++) {
        size_t i = excIdx[k];
        excBytes += stridePredictTagged_(i);
        excBytes += stridePredictSigned_(values[i]);
    }
    free(excIdx);

    meta->mode = VARINT_STRIDE_MODE_FUZZY;
    meta->exceptionCount = mismatches;
    meta->encodedSize = fixedHeader + excBytes;
    return meta->encodedSize < count * sizeof(int64_t);
}

bool varintStrideAnalyzeUnsigned(const uint64_t *values, size_t count,
                                 varintStrideMeta *meta) {
    /* Interpret as signed for stride math — wrap semantics fine for fixed
     * stride detection. Cast on the way in and out as needed by caller. */
    return varintStrideAnalyze((const int64_t *)values, count, meta);
}

/* ====================================================================
 * Encoding
 * ==================================================================== */

static size_t strideWriteHeader_(uint8_t *dst, const varintStrideMeta *meta) {
    uint8_t *p = dst;
    *p++ = (uint8_t)meta->mode;
    p += varintDeltaPut(p, meta->base);
    p += varintDeltaPut(p, meta->stride);
    /* count as unsigned tagged */
    p += varintTaggedPut64(p, meta->count);
    return (size_t)(p - dst);
}

size_t varintStrideEncodeWithMode(uint8_t *dst, const int64_t *values,
                                  size_t count, varintStrideMode mode,
                                  varintStrideMeta *meta) {
    assert(dst != NULL);
    assert(values != NULL || count == 0);

    varintStrideMeta local;
    if (!meta) {
        meta = &local;
    }

    if (count == 0) {
        memset(meta, 0, sizeof(*meta));
        return 0;
    }

    varintStrideAnalyze(values, count, meta);

    /* Caller forces a mode; we just enforce that exact actually fits. */
    if (mode == VARINT_STRIDE_MODE_EXACT && meta->exceptionCount > 0) {
        return 0;
    }
    meta->mode = mode;

    uint8_t *p = dst;
    p += strideWriteHeader_(p, meta);

    if (mode == VARINT_STRIDE_MODE_FUZZY) {
        /* Re-scan to collect exception indices (no big deal since data is
         * likely cache-warm from analyze). */
        size_t *excIdx = NULL;
        if (meta->exceptionCount > 0) {
            excIdx = malloc(meta->exceptionCount * sizeof(size_t));
            if (!excIdx) {
                return 0;
            }
        }
        size_t found =
            strideCountMismatches_(values, count, meta->stride, excIdx);
        (void)found;
        assert(found == meta->exceptionCount);

        p += varintTaggedPut64(p, meta->exceptionCount);
        for (size_t k = 0; k < meta->exceptionCount; k++) {
            size_t i = excIdx[k];
            p += varintTaggedPut64(p, i);
            p += varintDeltaPut(p, values[i]);
        }
        free(excIdx);
    }

    size_t total = (size_t)(p - dst);
    meta->encodedSize = total;
    return total;
}

size_t varintStrideEncode(uint8_t *dst, const int64_t *values, size_t count,
                          varintStrideMeta *meta) {
    varintStrideMeta local;
    if (!meta) {
        meta = &local;
    }
    const bool viable = varintStrideAnalyze(values, count, meta);
    /* A non-viable fuzzy encoding is unbounded (up to ~13 bytes per
     * exception with no exception cap), so decline instead of emitting
     * something larger than callers' scratch bounds. Exact/trivial modes
     * are constant-size and may proceed regardless of benefit. */
    if (!viable && meta->mode == VARINT_STRIDE_MODE_FUZZY) {
        return 0;
    }
    return varintStrideEncodeWithMode(dst, values, count, meta->mode, meta);
}

size_t varintStrideEncodeUnsigned(uint8_t *dst, const uint64_t *values,
                                  size_t count, varintStrideMeta *meta) {
    return varintStrideEncode(dst, (const int64_t *)values, count, meta);
}

/* ====================================================================
 * Decoding
 * ==================================================================== */

size_t varintStrideReadMeta(const uint8_t *src, varintStrideMeta *meta) {
    assert(src != NULL);
    assert(meta != NULL);

    memset(meta, 0, sizeof(*meta));
    const uint8_t *p = src;
    meta->mode = (varintStrideMode)*p++;
    p += varintDeltaGet(p, &meta->base);
    p += varintDeltaGet(p, &meta->stride);
    uint64_t c;
    p += varintTaggedGet64(p, &c);
    meta->count = (size_t)c;
    /* exceptionCount and encodedSize unfilled here — that requires a full scan
     */
    return (size_t)(p - src);
}

size_t varintStrideDecode(const uint8_t *src, size_t count, int64_t *output) {
    assert(src != NULL);
    assert(output != NULL || count == 0);

    if (count == 0) {
        return 0;
    }

    varintStrideMeta meta;
    const uint8_t *p = src;
    p += varintStrideReadMeta(src, &meta);

    if (meta.count != count) {
        /* count mismatch — caller passed the wrong number. */
        return 0;
    }

    /* Materialize arithmetic progression first. Wrapping addition
     * mirrors the wrapping stride subtraction used during analysis. */
    int64_t v = meta.base;
    for (size_t i = 0; i < count; i++) {
        output[i] = v;
        v = (int64_t)((uint64_t)v + (uint64_t)meta.stride);
    }

    if (meta.mode == VARINT_STRIDE_MODE_FUZZY) {
        uint64_t excCount;
        p += varintTaggedGet64(p, &excCount);
        for (uint64_t k = 0; k < excCount; k++) {
            uint64_t idx;
            int64_t actual;
            p += varintTaggedGet64(p, &idx);
            p += varintDeltaGet(p, &actual);
            if (idx < count) {
                output[idx] = actual;
            }
        }
    }

    return (size_t)(p - src);
}

size_t varintStrideDecodeUnsigned(const uint8_t *src, size_t count,
                                  uint64_t *output) {
    return varintStrideDecode(src, count, (int64_t *)output);
}

int64_t varintStrideGetAt(const uint8_t *src, size_t index) {
    assert(src != NULL);

    varintStrideMeta meta;
    const uint8_t *p = src;
    p += varintStrideReadMeta(src, &meta);

    if (index >= meta.count) {
        return 0;
    }

    int64_t base = (int64_t)((uint64_t)meta.base +
                             (uint64_t)index * (uint64_t)meta.stride);

    if (meta.mode == VARINT_STRIDE_MODE_FUZZY) {
        uint64_t excCount;
        p += varintTaggedGet64(p, &excCount);
        for (uint64_t k = 0; k < excCount; k++) {
            uint64_t idx;
            int64_t actual;
            p += varintTaggedGet64(p, &idx);
            p += varintDeltaGet(p, &actual);
            if (idx == index) {
                return actual;
            }
        }
    }
    return base;
}

/* ====================================================================
 * Unit Tests
 * ==================================================================== */
#ifdef VARINT_STRIDE_TEST
#include "ctest.h"
#include <stdio.h>

int varintStrideTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int32_t err = 0;

    TEST("Stride exact: linear ramp encodes in constant size") {
        int64_t values[1000];
        for (size_t i = 0; i < 1000; i++) {
            values[i] = 50000 + (int64_t)i * 7;
        }

        uint8_t buf[varintStrideMaxEncodedSize(1000)];
        varintStrideMeta meta;
        size_t written = varintStrideEncode(buf, values, 1000, &meta);

        if (meta.mode != VARINT_STRIDE_MODE_EXACT) {
            ERR("Expected EXACT mode, got %d", (int)meta.mode);
        }
        if (meta.exceptionCount != 0) {
            ERR("Expected 0 exceptions, got %zu", meta.exceptionCount);
        }
        /* Exact stride header: mode + base + stride + count, all small => <16B
         */
        if (written > 20) {
            ERR("Exact stride encoding too large: %zu bytes", written);
        }

        int64_t dec[1000];
        size_t read = varintStrideDecode(buf, 1000, dec);
        if (read != written) {
            ERR("byte count mismatch: wrote %zu, read %zu", written, read);
        }
        for (size_t i = 0; i < 1000; i++) {
            if (dec[i] != values[i]) {
                ERR("mismatch at %zu: %lld vs %lld", i, (long long)values[i],
                    (long long)dec[i]);
                break;
            }
        }
    }

    TEST("Stride exact: negative stride works") {
        int64_t values[100];
        for (size_t i = 0; i < 100; i++) {
            values[i] = 100000 - (int64_t)i * 13;
        }
        uint8_t buf[varintStrideMaxEncodedSize(100)];
        varintStrideMeta meta;
        varintStrideEncode(buf, values, 100, &meta);

        if (meta.stride != -13) {
            ERR("Expected stride=-13, got %lld", (long long)meta.stride);
        }

        int64_t dec[100];
        varintStrideDecode(buf, 100, dec);
        for (size_t i = 0; i < 100; i++) {
            if (dec[i] != values[i]) {
                ERR("neg stride mismatch at %zu", i);
                break;
            }
        }
    }

    TEST("Stride fuzzy: handles outliers") {
        int64_t values[100];
        for (size_t i = 0; i < 100; i++) {
            values[i] = 5000 + (int64_t)i * 10;
        }
        /* Inject 3 outliers */
        values[17] = 99999;
        values[50] = -42;
        values[83] = 12345;

        uint8_t buf[varintStrideMaxEncodedSize(100)];
        varintStrideMeta meta;
        size_t written = varintStrideEncode(buf, values, 100, &meta);

        if (meta.mode != VARINT_STRIDE_MODE_FUZZY) {
            ERR("Expected FUZZY mode, got %d", (int)meta.mode);
        }
        /* One outlier breaks two consecutive deltas (in and out), so a
         * "3-outlier" array typically produces 3-6 mismatched deltas. */
        if (meta.exceptionCount < 3 || meta.exceptionCount > 6) {
            ERR("Unexpected exceptionCount=%zu", meta.exceptionCount);
        }

        int64_t dec[100];
        size_t read = varintStrideDecode(buf, 100, dec);
        if (read != written) {
            ERRR("byte count mismatch");
        }
        for (size_t i = 0; i < 100; i++) {
            if (dec[i] != values[i]) {
                ERR("fuzzy mismatch at %zu: %lld vs %lld", i,
                    (long long)values[i], (long long)dec[i]);
            }
        }
    }

    TEST("Stride GetAt random access — exact mode O(1)") {
        int64_t values[500];
        for (size_t i = 0; i < 500; i++) {
            values[i] = 1000 + (int64_t)i * 3;
        }
        uint8_t buf[varintStrideMaxEncodedSize(500)];
        varintStrideEncode(buf, values, 500, NULL);

        if (varintStrideGetAt(buf, 0) != values[0]) {
            ERRR("GetAt(0)");
        }
        if (varintStrideGetAt(buf, 250) != values[250]) {
            ERRR("GetAt(250)");
        }
        if (varintStrideGetAt(buf, 499) != values[499]) {
            ERRR("GetAt(499)");
        }
    }

    TEST("Stride not beneficial for random data") {
        int64_t values[20];
        for (size_t i = 0; i < 20; i++) {
            values[i] = (int64_t)(i * 17 ^ (i << 3));
        }
        varintStrideMeta meta;
        bool ok = varintStrideAnalyze(values, 20, &meta);
        if (ok) {
            /* If random data happens to fit fuzzy, that's fine — but check
             * we don't claim exact. */
            if (meta.mode == VARINT_STRIDE_MODE_EXACT &&
                meta.exceptionCount == 0) {
                ERRR("Random data incorrectly classified as exact stride");
            }
        }
    }

    TEST("Stride edge cases: 0, 1, 2 elements") {
        uint8_t buf[64];
        varintStrideMeta meta;

        /* 0 */
        size_t w = varintStrideEncode(buf, NULL, 0, &meta);
        if (w != 0) {
            ERR("Empty encode wrote %zu", w);
        }

        /* 1 */
        int64_t one = 999;
        w = varintStrideEncode(buf, &one, 1, &meta);
        int64_t out1 = 0;
        varintStrideDecode(buf, 1, &out1);
        if (out1 != 999) {
            ERR("single decode wrong: %lld", (long long)out1);
        }

        /* 2 */
        int64_t two[2] = {100, 107};
        w = varintStrideEncode(buf, two, 2, &meta);
        if (meta.stride != 7) {
            ERR("2-element stride: expected 7, got %lld",
                (long long)meta.stride);
        }
        int64_t out2[2] = {0, 0};
        varintStrideDecode(buf, 2, out2);
        if (out2[0] != 100 || out2[1] != 107) {
            ERRR("2-element decode wrong");
        }
        (void)w;
    }

    TEST("Stride analyze matches actual encode byte count") {
        int64_t values[200];
        for (size_t i = 0; i < 200; i++) {
            values[i] = (int64_t)i * 8;
        }
        /* Sprinkle exceptions */
        values[33] = 999999;
        values[150] = -1;

        varintStrideMeta a;
        varintStrideAnalyze(values, 200, &a);

        uint8_t buf[varintStrideMaxEncodedSize(200)];
        varintStrideMeta e;
        size_t written = varintStrideEncode(buf, values, 200, &e);

        if (a.encodedSize != written) {
            ERR("analyze predicted %zu, actual %zu", a.encodedSize, written);
        }
        if (e.encodedSize != written) {
            ERR("encode-returned size %zu != actual %zu", e.encodedSize,
                written);
        }
    }

    TEST("Stride large SIMD path — 4096 elements") {
        size_t count = 4096;
        int64_t *vals = malloc(count * sizeof(int64_t));
        for (size_t i = 0; i < count; i++) {
            vals[i] = 1000000 + (int64_t)i * 60;
        }
        uint8_t *buf = malloc(varintStrideMaxEncodedSize(count));
        varintStrideMeta meta;
        size_t written = varintStrideEncode(buf, vals, count, &meta);
        if (meta.mode != VARINT_STRIDE_MODE_EXACT) {
            ERR("4K linear ramp not detected as exact (mode=%d)",
                (int)meta.mode);
        }
        if (written > 30) {
            ERR("4K exact stride too large: %zu", written);
        }

        int64_t *dec = malloc(count * sizeof(int64_t));
        varintStrideDecode(buf, count, dec);
        for (size_t i = 0; i < count; i++) {
            if (dec[i] != vals[i]) {
                ERR("4K mismatch at %zu", i);
                break;
            }
        }
        free(vals);
        free(buf);
        free(dec);
    }

    TEST_FINAL_RESULT;
    return 0;
}

#endif /* VARINT_STRIDE_TEST */
