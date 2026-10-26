/* Poly1305 — x86 AVX2 4-block batched MAC accelerator.
 *
 * Strategy: classical poly1305-donna-N-block, N=4.
 *
 * Scalar Poly1305 absorbs 1 block per iteration:
 *     h := (h + m) * r mod p     where p = 2^130 - 5
 *
 * Four-block unroll:
 *     h_1 = (h_0 + m_0) * r
 *     h_2 = (h_1 + m_1) * r = (h_0 + m_0) * r^2 + m_1 * r
 *     h_3 = (h_2 + m_2) * r = (h_0 + m_0) * r^3 + m_1 * r^2 + m_2 * r
 *     h_4 = (h_3 + m_3) * r = (h_0 + m_0) * r^4 + m_1 * r^3 + m_2 * r^2 + m_3 *
 * r
 *
 * Per iteration we precompute r^2, r^3, r^4 once and perform four
 * parallel 5-limb-by-5-limb modular multiplies (with r^4, r^3, r^2, r
 * respectively), then add their reduced limbs into the new accumulator.
 * The four multiplies are SIMD-fused: each 5x5 partial product is
 * computed lane-parallel via _mm256_mul_epu32 (32x32 -> 64 across 4
 * 64-bit lanes of __m256i).  Reduction is identical to the scalar path
 * so output is bit-exact.
 *
 * Layout: each __m256i holds 4x 64-bit lanes.  Per limb position j of
 * the multiplier b, we hold `[r^4_j, r^3_j, r^2_j, r_j]` (in some
 * lane order — we use lane 0=r^4, 1=r^3, 2=r^2, 3=r).  Per limb
 * position j of the accumulator a, we hold `[h+m0_j, m1_j, m2_j, m3_j]`.
 * After accumulating all 25 partial products into per-limb wide
 * vectors, we horizontally sum the 4 lanes for each limb position and
 * carry-reduce.
 *
 * Differential parity vs the scalar poly1305Block (one block at a time)
 * is enforced by dkTestPoly1305SimdParity. */

#include "poly1305AVX2.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Scalar reference for r^k precompute, shared between SIMD and scalar
 * paths to keep arithmetic bit-identical. */
static inline void poly1305_mul_scalar(const uint32_t a[5], const uint32_t b[5],
                                       uint32_t out[5]) {
    uint64_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3], a4 = a[4];
    uint64_t b0 = b[0], b1 = b[1], b2 = b[2], b3 = b[3], b4 = b[4];

    uint64_t s1 = b1 * 5;
    uint64_t s2 = b2 * 5;
    uint64_t s3 = b3 * 5;
    uint64_t s4 = b4 * 5;

    uint64_t d0 = a0 * b0 + a1 * s4 + a2 * s3 + a3 * s2 + a4 * s1;
    uint64_t d1 = a0 * b1 + a1 * b0 + a2 * s4 + a3 * s3 + a4 * s2;
    uint64_t d2 = a0 * b2 + a1 * b1 + a2 * b0 + a3 * s4 + a4 * s3;
    uint64_t d3 = a0 * b3 + a1 * b2 + a2 * b1 + a3 * b0 + a4 * s4;
    uint64_t d4 = a0 * b4 + a1 * b3 + a2 * b2 + a3 * b1 + a4 * b0;

    uint64_t c;
    c = d0 >> 26;
    uint64_t h0 = d0 & 0x3ffffffu;
    d1 += c;
    c = d1 >> 26;
    uint64_t h1 = d1 & 0x3ffffffu;
    d2 += c;
    c = d2 >> 26;
    uint64_t h2 = d2 & 0x3ffffffu;
    d3 += c;
    c = d3 >> 26;
    uint64_t h3 = d3 & 0x3ffffffu;
    d4 += c;
    c = d4 >> 26;
    uint64_t h4 = d4 & 0x3ffffffu;
    h0 += c * 5;
    c = h0 >> 26;
    h0 &= 0x3ffffffu;
    h1 += c;

    out[0] = (uint32_t)h0;
    out[1] = (uint32_t)h1;
    out[2] = (uint32_t)h2;
    out[3] = (uint32_t)h3;
    out[4] = (uint32_t)h4;
}

void poly1305AVX2PrecomputePowers(const uint32_t r[5], uint32_t r2[5],
                                  uint32_t r3[5], uint32_t r4[5]) {
    poly1305_mul_scalar(r, r, r2);
    poly1305_mul_scalar(r2, r, r3);
    poly1305_mul_scalar(r3, r, r4);
}

#if POLY1305_AVX2_BUILD

#include <immintrin.h>

bool poly1305AVX2Available(void) {
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") != 0;
}

/* Load a 16-byte Poly1305 block as 5x 26-bit limbs and append the
 * 1<<24 hibit marker.  Identical to the scalar load path. */
__attribute__((target("avx2"), always_inline)) static inline void
poly1305_load_block_avx2(const uint8_t *m, uint64_t out[5]) {
    uint32_t t0 = (uint32_t)m[0] | ((uint32_t)m[1] << 8) |
                  ((uint32_t)m[2] << 16) | ((uint32_t)m[3] << 24);
    uint32_t t1 = (uint32_t)m[4] | ((uint32_t)m[5] << 8) |
                  ((uint32_t)m[6] << 16) | ((uint32_t)m[7] << 24);
    uint32_t t2 = (uint32_t)m[8] | ((uint32_t)m[9] << 8) |
                  ((uint32_t)m[10] << 16) | ((uint32_t)m[11] << 24);
    uint32_t t3 = (uint32_t)m[12] | ((uint32_t)m[13] << 8) |
                  ((uint32_t)m[14] << 16) | ((uint32_t)m[15] << 24);

    out[0] = t0 & 0x3ffffffu;
    out[1] = ((t0 >> 26) | (t1 << 6)) & 0x3ffffffu;
    out[2] = ((t1 >> 20) | (t2 << 12)) & 0x3ffffffu;
    out[3] = ((t2 >> 14) | (t3 << 18)) & 0x3ffffffu;
    out[4] = (t3 >> 8) + (1u << 24);
}

/* Horizontal sum of 4 uint64 lanes in a __m256i. */
__attribute__((target("avx2"), always_inline)) static inline uint64_t
hsum_u64_avx2(__m256i v) {
    /* Sum upper-128 with lower-128, then sum the two remaining lanes. */
    __m128i lo = _mm256_castsi256_si128(v);
    __m128i hi = _mm256_extracti128_si256(v, 1);
    __m128i s = _mm_add_epi64(lo, hi);
    /* s has 2x 64-bit lanes; add them. */
    uint64_t tmp[2];
    _mm_storeu_si128((__m128i *)tmp, s);
    return tmp[0] + tmp[1];
}

__attribute__((target("avx2"))) void
poly1305AVX2Block4x(uint64_t h[5], const uint32_t r[5], const uint32_t r2[5],
                    const uint32_t r3[5], const uint32_t r4[5],
                    const uint8_t *msg, size_t num_groups) {
    /* Pack [r^4_j, r^3_j, r^2_j, r_j] in lanes [0..3] of a __m256i,
     * each lane is a 64-bit value with the 32-bit limb in its low
     * half (high half zeroed).  _mm256_mul_epu32 reads the low 32
     * bits of each lane and multiplies; we follow the same lane
     * layout for accumulators so vector multiply gives parallel
     * products. */
    __m256i v_r_0 = _mm256_set_epi64x(r[0], r2[0], r3[0], r4[0]);
    __m256i v_r_1 = _mm256_set_epi64x(r[1], r2[1], r3[1], r4[1]);
    __m256i v_r_2 = _mm256_set_epi64x(r[2], r2[2], r3[2], r4[2]);
    __m256i v_r_3 = _mm256_set_epi64x(r[3], r2[3], r3[3], r4[3]);
    __m256i v_r_4 = _mm256_set_epi64x(r[4], r2[4], r3[4], r4[4]);
    /* s_i = b_i * 5 for wraparound; same lane layout. */
    __m256i v_s_1 =
        _mm256_set_epi64x(r[1] * 5, r2[1] * 5, r3[1] * 5, r4[1] * 5);
    __m256i v_s_2 =
        _mm256_set_epi64x(r[2] * 5, r2[2] * 5, r3[2] * 5, r4[2] * 5);
    __m256i v_s_3 =
        _mm256_set_epi64x(r[3] * 5, r2[3] * 5, r3[3] * 5, r4[3] * 5);
    __m256i v_s_4 =
        _mm256_set_epi64x(r[4] * 5, r2[4] * 5, r3[4] * 5, r4[4] * 5);

    uint64_t h0 = h[0];
    uint64_t h1 = h[1];
    uint64_t h2 = h[2];
    uint64_t h3 = h[3];
    uint64_t h4 = h[4];

    for (size_t i = 0; i < num_groups; i++) {
        /* Block 0: a = h + m_0; Blocks 1..3: a = m_k. */
        uint64_t m0[5], m1[5], m2[5], m3[5];
        poly1305_load_block_avx2(msg + 64 * i + 0, m0);
        poly1305_load_block_avx2(msg + 64 * i + 16, m1);
        poly1305_load_block_avx2(msg + 64 * i + 32, m2);
        poly1305_load_block_avx2(msg + 64 * i + 48, m3);

        uint64_t a0_0 = h0 + m0[0];
        uint64_t a1_0 = h1 + m0[1];
        uint64_t a2_0 = h2 + m0[2];
        uint64_t a3_0 = h3 + m0[3];
        uint64_t a4_0 = h4 + m0[4];

        /* Pack [a_0, a_1, a_2, a_3] per limb in lane [0..3] —
         * matching the r-power lane order. */
        __m256i v_a_0 = _mm256_set_epi64x((int64_t)m3[0], (int64_t)m2[0],
                                          (int64_t)m1[0], (int64_t)a0_0);
        __m256i v_a_1 = _mm256_set_epi64x((int64_t)m3[1], (int64_t)m2[1],
                                          (int64_t)m1[1], (int64_t)a1_0);
        __m256i v_a_2 = _mm256_set_epi64x((int64_t)m3[2], (int64_t)m2[2],
                                          (int64_t)m1[2], (int64_t)a2_0);
        __m256i v_a_3 = _mm256_set_epi64x((int64_t)m3[3], (int64_t)m2[3],
                                          (int64_t)m1[3], (int64_t)a3_0);
        __m256i v_a_4 = _mm256_set_epi64x((int64_t)m3[4], (int64_t)m2[4],
                                          (int64_t)m1[4], (int64_t)a4_0);

        /* d_j_lane = sum_k a_k * b_{j-k} (with wraparound *5 terms).
         * Each __m256i holds 4 parallel d_j values, one per block. */
        __m256i d0 = _mm256_mul_epu32(v_a_0, v_r_0);
        d0 = _mm256_add_epi64(d0, _mm256_mul_epu32(v_a_1, v_s_4));
        d0 = _mm256_add_epi64(d0, _mm256_mul_epu32(v_a_2, v_s_3));
        d0 = _mm256_add_epi64(d0, _mm256_mul_epu32(v_a_3, v_s_2));
        d0 = _mm256_add_epi64(d0, _mm256_mul_epu32(v_a_4, v_s_1));

        __m256i d1 = _mm256_mul_epu32(v_a_0, v_r_1);
        d1 = _mm256_add_epi64(d1, _mm256_mul_epu32(v_a_1, v_r_0));
        d1 = _mm256_add_epi64(d1, _mm256_mul_epu32(v_a_2, v_s_4));
        d1 = _mm256_add_epi64(d1, _mm256_mul_epu32(v_a_3, v_s_3));
        d1 = _mm256_add_epi64(d1, _mm256_mul_epu32(v_a_4, v_s_2));

        __m256i d2 = _mm256_mul_epu32(v_a_0, v_r_2);
        d2 = _mm256_add_epi64(d2, _mm256_mul_epu32(v_a_1, v_r_1));
        d2 = _mm256_add_epi64(d2, _mm256_mul_epu32(v_a_2, v_r_0));
        d2 = _mm256_add_epi64(d2, _mm256_mul_epu32(v_a_3, v_s_4));
        d2 = _mm256_add_epi64(d2, _mm256_mul_epu32(v_a_4, v_s_3));

        __m256i d3 = _mm256_mul_epu32(v_a_0, v_r_3);
        d3 = _mm256_add_epi64(d3, _mm256_mul_epu32(v_a_1, v_r_2));
        d3 = _mm256_add_epi64(d3, _mm256_mul_epu32(v_a_2, v_r_1));
        d3 = _mm256_add_epi64(d3, _mm256_mul_epu32(v_a_3, v_r_0));
        d3 = _mm256_add_epi64(d3, _mm256_mul_epu32(v_a_4, v_s_4));

        __m256i d4 = _mm256_mul_epu32(v_a_0, v_r_4);
        d4 = _mm256_add_epi64(d4, _mm256_mul_epu32(v_a_1, v_r_3));
        d4 = _mm256_add_epi64(d4, _mm256_mul_epu32(v_a_2, v_r_2));
        d4 = _mm256_add_epi64(d4, _mm256_mul_epu32(v_a_3, v_r_1));
        d4 = _mm256_add_epi64(d4, _mm256_mul_epu32(v_a_4, v_r_0));

        /* Horizontal sum across the 4 lanes per limb. */
        uint64_t t0 = hsum_u64_avx2(d0);
        uint64_t t1 = hsum_u64_avx2(d1);
        uint64_t t2 = hsum_u64_avx2(d2);
        uint64_t t3 = hsum_u64_avx2(d3);
        uint64_t t4 = hsum_u64_avx2(d4);

        /* Carry-reduce mod 2^130 - 5 (same shape as scalar Block). */
        uint64_t c;
        c = t0 >> 26;
        h0 = t0 & 0x3ffffffu;
        t1 += c;
        c = t1 >> 26;
        h1 = t1 & 0x3ffffffu;
        t2 += c;
        c = t2 >> 26;
        h2 = t2 & 0x3ffffffu;
        t3 += c;
        c = t3 >> 26;
        h3 = t3 & 0x3ffffffu;
        t4 += c;
        c = t4 >> 26;
        h4 = t4 & 0x3ffffffu;
        h0 += c * 5;
        c = h0 >> 26;
        h0 &= 0x3ffffffu;
        h1 += c;
    }

    h[0] = h0;
    h[1] = h1;
    h[2] = h2;
    h[3] = h3;
    h[4] = h4;
}

#else /* !x86_64 */

bool poly1305AVX2Available(void) {
    return false;
}

/* Unreachable: the dispatcher selects AVX2 only after
 * poly1305AVX2Available(), and returning would leave the MAC state
 * unabsorbed, so a call aborts */
[[noreturn]] void poly1305AVX2Block4x(uint64_t[5], const uint32_t[5],
                                      const uint32_t[5], const uint32_t[5],
                                      const uint32_t[5], const uint8_t *,
                                      size_t) {
    abort();
}

#endif /* POLY1305_AVX2_BUILD */
