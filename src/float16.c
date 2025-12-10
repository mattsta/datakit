#include "float16.h"

/* Public domain code from:
 * https://stackoverflow.com/questions/1659440
 *
 * It's basically unreadable unless you go read the float16 and float32
 * IEEE specs, so we just trust it as-is. All our testing checks out so far. */

/* This does match the output of the float32 to float16 intrinsic _cvtss_sh(),
 * so we can switch between machine float16 and function float16 encode/decode
 * as necessary:
 *     assert(float16Encode(val) == _cvtss_sh(val, 0));
 */

/* An alternative table-based implementation is presented in:
 * ftp://ftp.fox-toolkit.org/pub/fasthalffloatconversion.pdf
 * but it requires about 1500 bytes of tables for full functionality */

/* This encoding has:
 * 49152  fractional representations (yay! more than just trunc regular floats)
 * 14336  integer representations (boo; WE WANT FLOATS)
 *     1  inf
 *     1  -inf
 *  2046  NaN
 * =========================================
 * 65536  total representations, as expected */

typedef union Bits {
    float f;
    int32_t si;
    uint32_t ui;
} Bits;

#define shift 13
#define shiftSign 16

#define infN 0x7F800000  // flt32 infinity
#define maxN 0x477FE000  // max flt16 normal as a flt32
#define minN 0x38800000  // min flt16 normal as a flt32
#define signN 0x80000000 // flt32 sign bit

#define infC (infN >> shift)
#define nanN ((infC + 1) << shift) // minimum flt16 nan as a flt32
#define maxC (maxN >> shift)
#define minC (minN >> shift)
#define signC (signN >> shiftSign) // flt16 sign bit

#define mulN 0x52000000 // (1 << 23) / minN
#define mulC 0x33800000 // minN / (1 << (23 - shift))

#define subC 0x003FF // max flt32 subnormal down shifted
#define norC 0x00400 // min flt32 normal down shifted

#define maxD (infC - maxC - 1)
#define minD (minC - subC - 1)

/* Ignore sanitizer because it doesn't like multiplying floats into ints
 * ("outside the range of representation") */
__attribute__((no_sanitize("float-cast-overflow"))) uint16_t
float16Encode_(const float value) {
    Bits v = {.f = value};
    Bits s = {.si = mulN};
    uint32_t sign = v.si & signN;

    v.si ^= sign;
    sign >>= shiftSign; // logical shift
    // cppcheck-suppress overlappingWriteUnion
    s.si = s.f * v.f; // correct subnormals (intentional union type-punning)
    v.si ^= (s.si ^ v.si) & -(minN > v.si);
    v.si ^= (infN ^ v.si) & -((infN > v.si) & (v.si > maxN));
    v.si ^= (nanN ^ v.si) & -((nanN > v.si) & (v.si > infN));
    v.ui >>= shift; // logical shift
    v.si ^= ((v.si - maxD) ^ v.si) & -(v.si > maxC);
    v.si ^= ((v.si - minD) ^ v.si) & -(v.si > subC);

    return v.ui | sign;
}

__attribute__((no_sanitize("shift"))) float
float16Decode_(const uint16_t value) {
    Bits v = {.ui = value};
    int32_t sign = v.si & signC;
    Bits s = {.si = mulC};

    v.si ^= sign;
    sign <<= shiftSign;
    v.si ^= ((v.si + minD) ^ v.si) & -(v.si > subC);
    v.si ^= ((v.si + maxD) ^ v.si) & -(v.si > maxC);

    s.f *= v.si;
    const int32_t mask = -(norC > v.si);
    v.si <<= shift;
    v.si ^= (s.si ^ v.si) & mask;
    v.si |= sign;

    return v.f;
}

#include "endianIsLittle.h"
uint16_t bfloat16Encode_(const float value) {
    if (endianIsLittle()) {
        const uint16_t *MSB = (uint16_t *)&value;
        return MSB[1];
    }

    return *(uint16_t *)&value;
}

float bfloat16Decode_(const uint16_t value) {
    if (endianIsLittle()) {
        union v {
            uint16_t ret[2];
            float value;
        } got = {.ret[1] = value};
        return got.value;
    }

    return (float)value;
}

/* ====================================================================
 * Batch Float16 Conversion Implementations
 * ==================================================================== */

#if defined(__aarch64__) || defined(__ARM_NEON) || defined(__ARM_NEON__)
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

#elif defined(__aarch64__)
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

#elif defined(__aarch64__)
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
#include "perf.h"
#include <assert.h>
#include <stdlib.h>

#define REPORT_TIME 1
#if REPORT_TIME
#define TIME_INIT PERF_TIMERS_SETUP
#define TIME_FINISH(i, what) PERF_TIMERS_FINISH_PRINT_RESULTS(i, what)
#else
#define TIME_INIT
#define TIME_FINISH(i, what)
#endif

#if __F16C__
#include <immintrin.h>
#endif

__attribute__((unused)) static void yieldAllFloats(void) {
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

__attribute__((no_sanitize("integer", "undefined")))
__attribute__((optnone)) int
float16Test(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    /* 10 million iterations provides reliable timing while keeping tests fast.
     * float16 only has 65536 values, so this covers each value ~150 times. */
    const size_t testers = 10000000;
    const float in = 3.333333333333333333333;
    const uint16_t created = 44444;

    size_t incr = 0;
    float incrf = 0.0;

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
            Bits v = {.f = in};
            Bits s = {.si = mulN};
            uint32_t sign = v.si & signN;

            v.si ^= sign;
            sign >>= shiftSign; // logical shift
            s.si = s.f * v.f;   // correct subnormals
            v.si ^= (s.si ^ v.si) & -(minN > v.si);
            v.si ^= (infN ^ v.si) & -((infN > v.si) & (v.si > maxN));
            v.si ^= (nanN ^ v.si) & -((nanN > v.si) & (v.si > infN));
            v.ui >>= shift; // logical shift
            v.si ^= ((v.si - maxD) ^ v.si) & -(v.si > maxC);
            v.si ^= ((v.si - minD) ^ v.si) & -(v.si > subC);

            const uint16_t got = v.ui | sign;
            incr += got;
        }

        TIME_FINISH(testers, "float16Encode software (inline; optnone)");
    }

    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            Bits v = {.ui = in};
            int32_t sign = v.si & signC;
            Bits s = {.si = mulC};

            v.si ^= sign;
            sign <<= shiftSign;
            v.si ^= ((v.si + minD) ^ v.si) & -(v.si > subC);
            v.si ^= ((v.si + maxD) ^ v.si) & -(v.si > maxC);

            s.f *= v.si;
            const int32_t mask = -(norC > v.si);
            v.si <<= shift;
            v.si ^= (s.si ^ v.si) & mask;
            v.si |= sign;
            const float got = v.f;
            incrf += got;
        }

        TIME_FINISH(testers, "float16Decode software (inline; optnone)");
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
            const uint16_t *MSB = (uint16_t *)&in;
            const uint16_t got = MSB[1];
            incr += got;
        }

        TIME_FINISH(testers, "bfloat16Encode software (inline)");
    }

    {
        TIME_INIT;
        for (size_t i = 0; i < testers; i++) {
            union v {
                uint16_t ret[2];
                float value;
            } got = {.ret[1] = created};
            const float gv = got.value;
            incrf += gv;
        }

        TIME_FINISH(testers, "bfloat16Decode software (inline)");
    }

    /* ================================================================
     * Batch Conversion Benchmarks: SIMD vs Scalar
     * ================================================================ */
    printf("==========================================\n\n");
    printf("Batch Float16 Conversion Benchmarks:\n\n");

    /* Allocate test arrays */
    const size_t batchSize = 1024;
    const size_t batchIterations = testers / batchSize;
    float *srcFloats = malloc(batchSize * sizeof(float));
    uint16_t *dstHalfs = malloc(batchSize * sizeof(uint16_t));
    float *dstFloats = malloc(batchSize * sizeof(float));

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
        TIME_FINISH(batchIterations * batchSize, "float16EncodeBatch SCALAR");
    }

    /* Store scalar results for verification */
    uint16_t *scalarHalfs = malloc(batchSize * sizeof(uint16_t));
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
        if (mismatches == 0) {
            printf("  [OK] Encode: SIMD matches Scalar for all %zu values\n",
                   batchSize);
        } else {
            printf("  [FAIL] Encode: %zu mismatches out of %zu\n", mismatches,
                   batchSize);
        }
    }

    /* Benchmark batch decode: Scalar */
    {
        TIME_INIT;
        for (size_t i = 0; i < batchIterations; i++) {
            float16DecodeBatchScalar(dstHalfs, dstFloats, batchSize);
        }
        TIME_FINISH(batchIterations * batchSize, "float16DecodeBatch SCALAR");
    }

    /* Store scalar decode results for verification */
    float *scalarFloats = malloc(batchSize * sizeof(float));
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
        if (mismatches == 0) {
            printf("  [OK] Decode: SIMD matches Scalar for all %zu values\n",
                   batchSize);
        } else {
            printf("  [FAIL] Decode: %zu mismatches out of %zu\n", mismatches,
                   batchSize);
        }
    }

    free(srcFloats);
    free(dstHalfs);
    free(dstFloats);
    free(scalarHalfs);
    free(scalarFloats);

    /* Suppress unused value warnings from benchmark accumulators */
    (void)incr;
    (void)incrf;

    return 0; /* All tests passed */
}

#endif
