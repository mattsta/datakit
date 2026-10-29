#include "float16.h"

#include <string.h>

/* The portable IEEE 754 binary16 codec, used where the CPU has no
 * conversion instructions.  Both directions work on the bit patterns with
 * integer operations only, so no floating-point mode (rounding,
 * flush-to-zero) changes a result.  Encode rounds to nearest, ties to
 * even, as the F16C and NEON conversions do, so every platform stores the
 * same half for the same float; decode is exact.  The tests check both
 * against the compiler's binary16 conversions: every half pattern, every
 * rounding midpoint and a million random floats.
 *
 * binary16: 1 sign bit, 5 exponent bits (bias 15), 10 mantissa bits;
 * binary32: 1, 8 (bias 127), 23.  The 65536 halves are 63488 finite
 * values (signed zeros and subnormals included), two infinities and 2046
 * NaNs. */

uint16_t float16Encode_(const float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    const uint16_t sign = (uint16_t)((bits >> 16) & 0x8000);
    const uint32_t abs = bits & 0x7FFFFFFFu;

    if (abs > 0x7F800000u) {
        /* NaN: quiet, the top of its payload kept */
        return sign | 0x7E00 | (uint16_t)((abs >> 13) & 0x03FF);
    }
    if (abs >= 0x477FF000u) {
        /* 65520 and up (with infinity) round past the largest half,
         * 65504: infinity */
        return sign | 0x7C00;
    }
    if (abs >= 0x38800000u) {
        /* A normal half (2^-14 and up): rebias the exponent from 127 to
         * 15 and drop 13 mantissa bits, rounding to nearest, ties to even
         * (a carry into the exponent is the correct next half) */
        const uint32_t rebiased = abs - (112u << 23);
        const uint32_t odd = (rebiased >> 13) & 1;
        return sign | (uint16_t)((rebiased + 0x0FFFu + odd) >> 13);
    }
    if (abs < 0x33000000u) {
        /* Below 2^-25, half the smallest subnormal: zero */
        return sign;
    }
    /* A subnormal half: the value in units of 2^-24, rounded to nearest,
     * ties to even (rounding up to 0x0400 gives the smallest normal) */
    const uint32_t exponent = abs >> 23;
    const uint32_t mantissa = (abs & 0x007FFFFFu) | 0x00800000u;
    const uint32_t drop = 126 - exponent; /* 14 to 24 bits */
    uint32_t half = mantissa >> drop;
    const uint32_t rest = mantissa & ((1u << drop) - 1);
    const uint32_t midpoint = 1u << (drop - 1);
    half += rest > midpoint || (rest == midpoint && (half & 1));
    return sign | (uint16_t)half;
}

float float16Decode_(const uint16_t value) {
    const uint32_t sign = (uint32_t)(value & 0x8000) << 16;
    const uint32_t exponent = (value >> 10) & 0x1F;
    uint32_t mantissa = value & 0x03FF;
    uint32_t bits;
    if (exponent == 0x1F) {
        /* Infinity, or a NaN with its payload kept */
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else if (exponent != 0) {
        /* Normal: rebias the exponent from 15 to 127 */
        bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    } else if (mantissa == 0) {
        bits = sign; /* zero */
    } else {
        /* Subnormal, mantissa * 2^-24: shift its leading one up to the
         * implicit bit's place, the exponent starting at 2^-14's (113)
         * and dropping one per shift */
        uint32_t biased = 113;
        while (!(mantissa & 0x0400)) {
            mantissa <<= 1;
            biased--;
        }
        bits = sign | (biased << 23) | ((mantissa & 0x03FF) << 13);
    }
    float result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

uint16_t bfloat16Encode_(const float value) {
    /* bfloat16 is the high half of the float's bit pattern.  memcpy reads
     * the bits without an aliasing violation, and the shift picks the high
     * half on either byte order. */
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    const uint16_t high = (uint16_t)(bits >> 16);
    /* A NaN whose payload is all in the low half would truncate to
     * infinity: keep it a NaN (the quiet bit, its sign kept) */
    if ((bits & 0x7FFFFFFFu) > 0x7F800000u && (high & 0x007F) == 0) {
        return high | 0x0040;
    }
    return high;
}

float bfloat16Decode_(const uint16_t value) {
    /* The inverse of encode: place the bits as the float's high half and
     * zero the low half, on either byte order. */
    const uint32_t bits = (uint32_t)value << 16;
    float result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

/* ====================================================================
 * Batch Float16 Conversion Implementations
 * ==================================================================== */

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

/* Scalar baseline for benchmarking - uses same conversion as single-value API
 * to ensure consistency (float16Encode uses hardware NEON on ARM64, F16C on
 * x86) */
size_t float16EncodeBatchScalar(const float *src, uint16_t *dst, size_t count) {
    for (size_t i = 0; i < count; i++) {
        dst[i] = float16Encode(src[i]);
    }
    return count;
}

size_t float16DecodeBatchScalar(const uint16_t *src, float *dst, size_t count) {
    for (size_t i = 0; i < count; i++) {
        dst[i] = float16Decode(src[i]);
    }
    return count;
}

/* SIMD batch implementations */
size_t float16EncodeBatch(const float *src, uint16_t *dst, size_t count) {
#if __F16C__
    /* x86 with F16C: process 8 floats at a time using AVX */
    size_t i = 0;

    /* Process 8 floats at a time */
    while (i + 8 <= count) {
        __m256 float_vec = _mm256_loadu_ps(&src[i]);
        __m128i half_vec = _mm256_cvtps_ph(float_vec, 0);
        _mm_storeu_si128((__m128i *)&dst[i], half_vec);
        i += 8;
    }

    /* Handle remaining elements */
    for (; i < count; i++) {
        dst[i] = _cvtss_sh(src[i], 0);
    }

    return count;

#elifdef __aarch64__
    /* ARM64 with FP16: process 4 floats at a time using NEON */
    size_t i = 0;

    /* Process 8 floats at a time (2 x float32x4_t -> 1 x float16x8_t) */
    while (i + 8 <= count) {
        float32x4_t lo = vld1q_f32(&src[i]);
        float32x4_t hi = vld1q_f32(&src[i + 4]);

        /* Convert float32 to float16 */
        float16x4_t lo_half = vcvt_f16_f32(lo);
        float16x4_t hi_half = vcvt_f16_f32(hi);

        /* Combine into float16x8_t and store */
        float16x8_t result = vcombine_f16(lo_half, hi_half);
        vst1q_u16(&dst[i], vreinterpretq_u16_f16(result));
        i += 8;
    }

    /* Process 4 floats at a time */
    while (i + 4 <= count) {
        float32x4_t vec = vld1q_f32(&src[i]);
        float16x4_t half_vec = vcvt_f16_f32(vec);
        vst1_u16(&dst[i], vreinterpret_u16_f16(half_vec));
        i += 4;
    }

    /* Handle remaining elements with software conversion */
    for (; i < count; i++) {
        dst[i] = float16Encode_(src[i]);
    }

    return count;

#else
    /* Fallback to scalar */
    return float16EncodeBatchScalar(src, dst, count);
#endif
}

size_t float16DecodeBatch(const uint16_t *src, float *dst, size_t count) {
#if __F16C__
    /* x86 with F16C: process 8 halfs at a time using AVX */
    size_t i = 0;

    /* Process 8 halfs at a time */
    while (i + 8 <= count) {
        __m128i half_vec = _mm_loadu_si128((const __m128i *)&src[i]);
        __m256 float_vec = _mm256_cvtph_ps(half_vec);
        _mm256_storeu_ps(&dst[i], float_vec);
        i += 8;
    }

    /* Handle remaining elements */
    for (; i < count; i++) {
        dst[i] = _cvtsh_ss(src[i]);
    }

    return count;

#elifdef __aarch64__
    /* ARM64 with FP16: process 4/8 halfs at a time using NEON */
    size_t i = 0;

    /* Process 8 halfs at a time */
    while (i + 8 <= count) {
        float16x8_t half_vec = vreinterpretq_f16_u16(vld1q_u16(&src[i]));

        /* Convert float16 to float32 (low and high halves separately) */
        float32x4_t lo = vcvt_f32_f16(vget_low_f16(half_vec));
        float32x4_t hi = vcvt_f32_f16(vget_high_f16(half_vec));

        vst1q_f32(&dst[i], lo);
        vst1q_f32(&dst[i + 4], hi);
        i += 8;
    }

    /* Process 4 halfs at a time */
    while (i + 4 <= count) {
        float16x4_t half_vec = vreinterpret_f16_u16(vld1_u16(&src[i]));
        float32x4_t float_vec = vcvt_f32_f16(half_vec);
        vst1q_f32(&dst[i], float_vec);
        i += 4;
    }

    /* Handle remaining elements with software conversion */
    for (; i < count; i++) {
        dst[i] = float16Decode_(src[i]);
    }

    return count;

#else
    /* Fallback to scalar */
    return float16DecodeBatchScalar(src, dst, count);
#endif
}

#ifdef DATAKIT_TEST
#include "cleaner.h"
#include "ctest.h"
#include "datakit.h"
#include "dkRandom.h" /* dkSplitmix64Next */
#include "perf.h"
#include <assert.h>
#include <inttypes.h>
#include <math.h> /* nextafterf, INFINITY */

defcleanerName(float *, zfree, FloatBuf);
defcleanerName(uint16_t *, zfree, U16Buf);

#if __F16C__
#include <immintrin.h>
#endif

[[maybe_unused]] static void yieldAllFloats(void) {
    printf("float16 floats\n");
    for (size_t i = 0; i <= UINT16_MAX; i++) {
        printf("%zu: %.16g\n", i, float16Decode(i));
    }

    printf("\n");
    printf("bfloat16 floats\n");
    for (size_t i = 0; i <= UINT16_MAX; i++) {
        printf("%zu: %.16g\n", i, bfloat16Decode(i));
    }
}

/* The timing loops must not be optimized away, and the sanitizers stay off
 * for the whole test.  optnone and the "integer" sanitizer group are
 * clang-only, so GCC gets its own spelling of each. */
#if defined(__clang__)
#define FLOAT16_TEST_ATTRS                                                     \
    [[gnu::no_sanitize("integer", "undefined")]] [[clang::optnone]]
#else
#define FLOAT16_TEST_ATTRS                                                     \
    [[gnu::no_sanitize("undefined")]] [[gnu::optimize("O0")]]
#endif

#ifdef __FLT16_MANT_DIG__
/* Whether the portable encoding of 'f' is the compiler's IEEE binary16
 * conversion of it (round to nearest, ties to even); for a NaN, any NaN */
static bool float16TestEncodesAsIeee_(float f) {
    const uint16_t got = float16Encode_(f);
    if (f != f) {
        return (got & 0x7C00) == 0x7C00 && (got & 0x03FF);
    }
    const _Float16 ref = (_Float16)f;
    uint16_t want;
    memcpy(&want, &ref, sizeof(want));
    return got == want;
}
#endif

FLOAT16_TEST_ATTRS int float16Test(int, char *[]) {
    /* 10 million iterations provides reliable timing while keeping tests fast.
     * float16 only has 65536 values, so this covers each value ~150 times. */
    const size_t testers = 10'000'000;
    const float in = 3.333333333333333333333;
    const uint16_t created = 44444;

    [[maybe_unused]] size_t incr = 0;
    [[maybe_unused]] float incrf = 0.0;
    int err = 0;

    /* bfloat16 is exactly the high half of a float: every pattern decodes
     * to that float bit for bit and encodes back to itself. */
#ifdef __FLT16_MANT_DIG__
    /* The portable codec against the compiler's IEEE binary16 conversions
     * (an independent implementation of the same standard) */
    TEST("float16 portable decode matches IEEE binary16 for every pattern") {
        for (uint32_t i = 0; i <= UINT16_MAX; i++) {
            const uint16_t h = (uint16_t)i;
            _Float16 ref16;
            memcpy(&ref16, &h, sizeof(ref16));
            const float want = (float)ref16;
            const float got = float16Decode_(h);
            uint32_t gotBits, wantBits;
            memcpy(&gotBits, &got, sizeof(gotBits));
            memcpy(&wantBits, &want, sizeof(wantBits));
            if (want != want ? got == got : gotBits != wantBits) {
                ERR("float16Decode_(0x%04" PRIx32 ") = 0x%08" PRIx32
                    ", IEEE 0x%08" PRIx32,
                    i, gotBits, wantBits);
            }
        }
    }

    TEST("float16 portable encode rounds as IEEE binary16 does") {
        /* Every half round-trips; every point where rounding decides (the
         * midpoint between neighbouring halves and one float step either
         * side of it) and seeded random floats encode as the compiler's
         * round-to-nearest-even conversion does */
        size_t bad = 0;
        for (uint32_t i = 0; i <= UINT16_MAX; i++) {
            const float f = float16Decode_((uint16_t)i);
            bad += f == f && float16Encode_(f) != (uint16_t)i;
        }
        for (uint32_t i = 0; i < 0x7BFF; i++) {
            /* Neighbouring finite halves i and i + 1, both signs */
            const float lo = float16Decode_((uint16_t)i);
            const float hi = float16Decode_((uint16_t)(i + 1));
            const float mid = lo + (hi - lo) / 2;
            const float probes[3] = {mid, nextafterf(mid, -INFINITY),
                                     nextafterf(mid, INFINITY)};
            for (size_t p = 0; p < 3; p++) {
                bad += !float16TestEncodesAsIeee_(probes[p]);
                bad += !float16TestEncodesAsIeee_(-probes[p]);
            }
        }
        /* Around the largest half (65504) and the overflow to infinity
         * (65520 and up) */
        const float edges[] = {
            65504.0f, 65519.0f, 65520.0f, 65536.0f, nextafterf(65520.0f, 0),
            1e10f,    INFINITY};
        for (size_t i = 0; i < countof(edges); i++) {
            bad += !float16TestEncodesAsIeee_(edges[i]);
            bad += !float16TestEncodesAsIeee_(-edges[i]);
        }
        uint64_t seed = 0x6A09E667F3BCC908ULL;
        for (size_t i = 0; i < 1'000'000; i++) {
            const uint32_t bits = (uint32_t)dkSplitmix64Next(&seed);
            float f;
            memcpy(&f, &bits, sizeof(f));
            bad += !float16TestEncodesAsIeee_(f);
        }
        if (bad) {
            ERR("%zu floats encode otherwise than IEEE binary16", bad);
        }
    }
#endif

    TEST("bfloat16: a NaN stays a NaN") {
        /* A NaN whose payload is only in the low half would truncate to
         * infinity; it encodes as a quiet NaN of its sign instead */
        const uint32_t nans[] = {0x7F800001u, 0xFF800001u, 0x7FC00000u,
                                 0x7F80FFFFu};
        for (size_t i = 0; i < countof(nans); i++) {
            float f;
            memcpy(&f, &nans[i], sizeof(f));
            const uint16_t b = bfloat16Encode(f);
            const float back = bfloat16Decode(b);
            if (back == back || (b >> 15) != (nans[i] >> 31)) {
                ERR("NaN 0x%08" PRIx32 " encoded as 0x%04" PRIx16, nans[i], b);
            }
        }
    }

    TEST("bfloat16: every pattern decodes to its float and round-trips") {
        for (uint32_t i = 0; i <= UINT16_MAX; i++) {
            const float decoded = bfloat16Decode((uint16_t)i);
            uint32_t bits;
            memcpy(&bits, &decoded, sizeof(bits));
            if (bits != i << 16) {
                ERR("bfloat16Decode(0x%04" PRIx32 ") gave bits 0x%08" PRIx32, i,
                    bits);
            } else if (bfloat16Encode(decoded) != i) {
                ERR("bfloat16 0x%04" PRIx32 " did not round-trip", i);
            }
        }
    }

#if 0
    yieldAllFloats();
#endif

#if 0
    const float tester = -3.658203125;
    printf("Encoded -3.6582 as: %.16f\n", bfloat16Decode(bfloat16Encode(tester)));
    printf("Encoded -3.6582 as: %.16f\n", float16Decode(float16Encode(tester)));
    for (size_t i = 0; i < UINT16_MAX; i++) {
        const float converted16 = float16Decode(i);
        const float convertedB16 = bfloat16Decode(bfloat16Encode(converted16));
        if (convertedB16 != converted16) {
            printf("float16 %.16f encoded to bfloat16 as %.16f\n", converted16, convertedB16);
        }
    }

    for (size_t i = 0; i < UINT16_MAX; i++) {
        const float converted16 = bfloat16Decode(i);
        const float convertedB16 = float16Decode(float16Encode(converted16));
        if (convertedB16 != converted16) {
            printf("bfloat16 %.16g encoded to float16 as %.16g\n", converted16, convertedB16);
        }
    }
#endif

#if __F16C__
    const float floats[8] = {in, in, in, in, in, in, in, in};
    __m128i half_vector;
#endif

    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            assert(true);
        }

        TIME_FINISH(testers, "assert overhead");
    }

    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            incr += float16Encode_(in);
        }

        TIME_FINISH(testers, "float16Encode software");
    }

    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            incrf += float16Decode_(created);
        }

        TIME_FINISH(testers, "float16Decode software");
    }

#if __F16C__
    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            incr += _cvtss_sh(in, 0);
        }

        TIME_FINISH(testers, "float16Encode hardware");
    }

    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            incrf += _cvtsh_ss(created);
        }

        TIME_FINISH(testers, "float16Decode hardware");
    }
#endif

    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            incr += bfloat16Encode(in);
        }

        TIME_FINISH(testers, "bfloat16Encode software");
    }

    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            incrf += bfloat16Decode(in);
        }

        TIME_FINISH(testers, "bfloat16Decode software");
    }

#if __F16C__
    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            __m256 float_vector = _mm256_load_ps(floats);
            half_vector = _mm256_cvtps_ph(float_vector, 0);
            incr += half_vector[0];
        }

        TIME_FINISH(testers * 8, "float16Encode Vector Hardware");
    }

    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            // uint16_t halfs[16] = {0};
            __m256 float_vector = _mm256_cvtph_ps(half_vector);
            //_mm256_store_si256 ((__m256i*)halfs, // half_vector);
            incrf += float_vector[0];
        }

        TIME_FINISH(testers * 8, "float16Decode Vector Hardware");
    }
#endif

    /*************************************************/
    printf("==========================================\n\n");
    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            incr += float16Encode_(in);
        }

        TIME_FINISH(testers, "float16Encode software (portable)");
    }

    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            incrf += float16Decode_(created);
        }

        TIME_FINISH(testers, "float16Decode software (portable)");
    }

#if __F16C__
    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            incr += _cvtss_sh(in, 0);
        }

        TIME_FINISH(testers, "float16Encode hardware (inline)");
    }

    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            incrf += _cvtsh_ss(created);
        }

        TIME_FINISH(testers, "float16Decode hardware (inline)");
    }
#endif

    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            uint32_t bits;
            memcpy(&bits, &in, sizeof(bits));
            incr += (uint16_t)(bits >> 16);
        }

        TIME_FINISH(testers, "bfloat16Encode software (inline)");
    }

    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            const uint32_t bits = (uint32_t)created << 16;
            float gv;
            memcpy(&gv, &bits, sizeof(gv));
            incrf += gv;
        }

        TIME_FINISH(testers, "bfloat16Decode software (inline)");
    }

    /* ================================================================
     * Batch Conversion Benchmarks: SIMD vs Scalar
     * ================================================================ */
    TEST("batch float16 conversion: SIMD matches scalar (with timings)") {
        /* Allocate test arrays */
        const size_t batchSize = 1024;
        const size_t batchIterations = testers / batchSize;
        cleanerName(zfree, FloatBuf) float *srcFloats =
            zmalloc(batchSize * sizeof(float));
        cleanerName(zfree, U16Buf) uint16_t *dstHalfs =
            zmalloc(batchSize * sizeof(uint16_t));
        cleanerName(zfree, FloatBuf) float *dstFloats =
            zmalloc(batchSize * sizeof(float));

        /* Initialize source with varied values */
        for (size_t i = 0; i < batchSize; i++) {
            srcFloats[i] = (float)i * 0.01f + in;
        }

        /* Benchmark batch encode: Scalar */
        {
            TIME_INIT;
            for (size_t i = 0; i < batchIterations; i++) {
                float16EncodeBatchScalar(srcFloats, dstHalfs, batchSize);
            }
            TIME_FINISH(batchIterations * batchSize,
                        "float16EncodeBatch SCALAR");
        }

        /* Store scalar results for verification */
        cleanerName(zfree, U16Buf) uint16_t *scalarHalfs =
            zmalloc(batchSize * sizeof(uint16_t));
        float16EncodeBatchScalar(srcFloats, scalarHalfs, batchSize);

        /* Benchmark batch encode: SIMD */
        {
            TIME_INIT;
            for (size_t i = 0; i < batchIterations; i++) {
                float16EncodeBatch(srcFloats, dstHalfs, batchSize);
            }
            TIME_FINISH(batchIterations * batchSize, "float16EncodeBatch SIMD");
        }

        /* Verify correctness: SIMD encode matches scalar */
        float16EncodeBatch(srcFloats, dstHalfs, batchSize);
        {
            size_t mismatches = 0;
            for (size_t i = 0; i < batchSize; i++) {
                if (dstHalfs[i] != scalarHalfs[i]) {
                    if (mismatches < 5) {
                        /* Show original value, both encoded values, and decoded
                         * difference */
                        float simdDecoded = float16Decode(dstHalfs[i]);
                        float scalarDecoded = float16Decode(scalarHalfs[i]);
                        printf("  Encode mismatch at %zu:\n"
                               "    Original: %.10g\n"
                               "    SIMD:   encoded=%u decoded=%.10g\n"
                               "    Scalar: encoded=%u decoded=%.10g\n"
                               "    Difference: %.10g\n",
                               i, srcFloats[i], dstHalfs[i], simdDecoded,
                               scalarHalfs[i], scalarDecoded,
                               simdDecoded - scalarDecoded);
                    }
                    mismatches++;
                }
            }
            if (mismatches) {
                ERR("encode: SIMD differs from scalar for %zu of %zu values",
                    mismatches, batchSize);
            }
        }

        /* Benchmark batch decode: Scalar */
        {
            TIME_INIT;
            for (size_t i = 0; i < batchIterations; i++) {
                float16DecodeBatchScalar(dstHalfs, dstFloats, batchSize);
            }
            TIME_FINISH(batchIterations * batchSize,
                        "float16DecodeBatch SCALAR");
        }

        /* Store scalar decode results for verification */
        cleanerName(zfree, FloatBuf) float *scalarFloats =
            zmalloc(batchSize * sizeof(float));
        float16DecodeBatchScalar(dstHalfs, scalarFloats, batchSize);

        /* Benchmark batch decode: SIMD */
        {
            TIME_INIT;
            for (size_t i = 0; i < batchIterations; i++) {
                float16DecodeBatch(dstHalfs, dstFloats, batchSize);
            }
            TIME_FINISH(batchIterations * batchSize, "float16DecodeBatch SIMD");
        }

        /* Verify correctness: SIMD decode matches scalar */
        float16DecodeBatch(dstHalfs, dstFloats, batchSize);
        {
            size_t mismatches = 0;
            for (size_t i = 0; i < batchSize; i++) {
                if (dstFloats[i] != scalarFloats[i]) {
                    if (mismatches < 5) {
                        printf("  Decode mismatch at %zu:\n"
                               "    Encoded: %u\n"
                               "    SIMD decoded:   %.10g\n"
                               "    Scalar decoded: %.10g\n"
                               "    Difference: %.10g\n",
                               i, dstHalfs[i], dstFloats[i], scalarFloats[i],
                               dstFloats[i] - scalarFloats[i]);
                    }
                    mismatches++;
                }
            }
            if (mismatches) {
                ERR("decode: SIMD differs from scalar for %zu of %zu values",
                    mismatches, batchSize);
            }
        }
    }

    TEST_FINAL_RESULT;
}

#endif
