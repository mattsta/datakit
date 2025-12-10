#pragma once

#include <stdint.h>

#if __F16C__
#include <immintrin.h>
/* The 0 here is the rounding mode.
 *  0x00 = Round to Nearest
 *  0x01 = Round Down
 *  0x02 = Round Up
 *  0x03 = Truncate */
#define float16Encode(v) _cvtss_sh(v, 0)
#define float16Decode(v) _cvtsh_ss(v)

#elif defined(__aarch64__)
/* ARM64 with FP16: use NEON hardware conversion for consistency
 * with batch operations. This ensures single-value and batch
 * conversions produce identical results. */
#include <arm_neon.h>

static inline uint16_t float16EncodeNeon_(float value) {
    float32x4_t vec = vdupq_n_f32(value);
    float16x4_t half = vcvt_f16_f32(vec);
    return vget_lane_u16(vreinterpret_u16_f16(half), 0);
}

static inline float float16DecodeNeon_(uint16_t value) {
    uint16x4_t half_vec = vdup_n_u16(value);
    float32x4_t float_vec = vcvt_f32_f16(vreinterpret_f16_u16(half_vec));
    return vgetq_lane_f32(float_vec, 0);
}

#define float16Encode(v) float16EncodeNeon_(v)
#define float16Decode(v) float16DecodeNeon_(v)

#else
#define float16Encode(v) float16Encode_(v)
#define float16Decode(v) float16Decode_(v)
#endif

/* Make these macros because Intel CPUs are adding hardware-level
 * bfloat16 conversion soon, then we can special case these with
 * CPU intrinsics like above for float16 */
#define bfloat16Encode(v) bfloat16Encode_(v)
#define bfloat16Decode(v) bfloat16Decode_(v)

/* Always make the function versions available even if we are using builtins */
uint16_t float16Encode_(float value);
float float16Decode_(uint16_t value);

uint16_t bfloat16Encode_(float value);
float bfloat16Decode_(uint16_t value);

#include <stddef.h>

/* ====================================================================
 * Batch Float16 Conversion APIs
 * ==================================================================== */

/* Batch encode: convert 'count' floats to float16.
 * Uses SIMD when available (AVX2/F16C on x86, NEON on ARM64).
 * Returns number of elements converted. */
size_t float16EncodeBatch(const float *src, uint16_t *dst, size_t count);
size_t float16DecodeBatch(const uint16_t *src, float *dst, size_t count);

/* Scalar baseline versions for benchmarking */
size_t float16EncodeBatchScalar(const float *src, uint16_t *dst, size_t count);
size_t float16DecodeBatchScalar(const uint16_t *src, float *dst, size_t count);

#ifdef DATAKIT_TEST
int float16Test(int argc, char *argv[]);
#endif
