/* Poly1305 — ARM NEON 2-block batched MAC accelerator.
 *
 * Strategy: classical poly1305-donna-N-block, N=2.
 *
 * Scalar Poly1305 absorbs 1 message block per iteration:
 *     h := (h + m) * r mod p     where p = 2^130 - 5
 *
 * Two-block unroll:
 *     h_1 = (h_0 + m_0) * r
 *     h_2 = (h_1 + m_1) * r
 *         = (h_0 + m_0) * r^2 + m_1 * r mod p
 *
 * So per iteration we precompute r^2 once and perform two parallel
 * 5-limb-by-5-limb modular multiplies (one with r^2, one with r), then
 * add their reduced limbs into the new accumulator.  The two multiplies
 * are SIMD-fused: each 5x5 partial product is computed lane-parallel
 * via vmull_u32 (32x32 -> 64).  Reduction is identical to the scalar
 * path so output is bit-exact.
 *
 * Layout: the two parallel multiplications are paired in a uint32x2_t
 * with lane 0 = "block 0 operand" (h+m0 vs r^2) and lane 1 = "block 1
 * operand" (m1 vs r).  After all 25 partial products are accumulated
 * we extract and sum the two lanes per limb, then carry-reduce.
 *
 * Differential parity vs the scalar poly1305Block (one block at a time)
 * is enforced by dkTestPoly1305SimdParity. */

#include "poly1305Neon.h"
#include "poly1305Limbs.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__aarch64__) && defined(__ARM_NEON)
#define POLY1305_NEON_BUILD 1
#else
#define POLY1305_NEON_BUILD 0
#endif

/* r^2 through the scalar MAC's own multiply, so both paths use the same
 * r^2 */
void poly1305NeonPrecomputeR2(const uint32_t r[5], uint32_t r2[5]) {
    poly1305LimbsMulMod32(r, r, r2);
}

#if POLY1305_NEON_BUILD

#include <arm_neon.h>

bool poly1305NeonAvailable(void) {
    /* AArch64 mandates NEON; Apple Silicon and every shipping Cortex-A
     * exposes it.  No runtime probe needed. */
    return true;
}

void poly1305NeonBlock2x(uint64_t h[5], const uint32_t r[5],
                         const uint32_t r2[5], const uint8_t *msg,
                         size_t num_pairs) {
    /* Precompute 5*b for the wrap-around terms of r and r^2.  We pair
     * them into uint32x2_t lane 0 = r^2 slots, lane 1 = r slots — the
     * accumulator updates per iteration use these uniformly. */
    uint32_t r1_5 = r[1] * 5, r2_5 = r[2] * 5, r3_5 = r[3] * 5, r4_5 = r[4] * 5;
    uint32_t r2_1_5 = r2[1] * 5, r2_2_5 = r2[2] * 5;
    uint32_t r2_3_5 = r2[3] * 5, r2_4_5 = r2[4] * 5;

    /* Pack [r2_i, r_i] in lane 0 / lane 1 of uint32x2_t. */
    uint32x2_t v_r_0 = {r2[0], r[0]};
    uint32x2_t v_r_1 = {r2[1], r[1]};
    uint32x2_t v_r_2 = {r2[2], r[2]};
    uint32x2_t v_r_3 = {r2[3], r[3]};
    uint32x2_t v_r_4 = {r2[4], r[4]};
    uint32x2_t v_s_1 = {r2_1_5, r1_5};
    uint32x2_t v_s_2 = {r2_2_5, r2_5};
    uint32x2_t v_s_3 = {r2_3_5, r3_5};
    uint32x2_t v_s_4 = {r2_4_5, r4_5};

    uint64_t h0 = h[0];
    uint64_t h1 = h[1];
    uint64_t h2 = h[2];
    uint64_t h3 = h[3];
    uint64_t h4 = h[4];

    for (size_t i = 0; i < num_pairs; i++) {
        /* Block 0: a = h + m_0 (still under 2^32 since h limbs < 2^26
         * and m limbs <= 2^26 + 1<<24 < 2^27).
         * Block 1: a = m_1 (already under 2^27). */
        uint64_t m0[5], m1[5];
        poly1305LimbsLoad(msg + 32 * i + 0, POLY1305_HIBIT, m0);
        poly1305LimbsLoad(msg + 32 * i + 16, POLY1305_HIBIT, m1);

        uint32_t a0_0 = (uint32_t)(h0 + m0[0]);
        uint32_t a1_0 = (uint32_t)(h1 + m0[1]);
        uint32_t a2_0 = (uint32_t)(h2 + m0[2]);
        uint32_t a3_0 = (uint32_t)(h3 + m0[3]);
        uint32_t a4_0 = (uint32_t)(h4 + m0[4]);

        uint32_t a0_1 = (uint32_t)m1[0];
        uint32_t a1_1 = (uint32_t)m1[1];
        uint32_t a2_1 = (uint32_t)m1[2];
        uint32_t a3_1 = (uint32_t)m1[3];
        uint32_t a4_1 = (uint32_t)m1[4];

        /* Pack the 5 limbs of the two accumulators into uint32x2_t. */
        uint32x2_t v_a_0 = {a0_0, a0_1};
        uint32x2_t v_a_1 = {a1_0, a1_1};
        uint32x2_t v_a_2 = {a2_0, a2_1};
        uint32x2_t v_a_3 = {a3_0, a3_1};
        uint32x2_t v_a_4 = {a4_0, a4_1};

        /* d_j_lane = sum_k a_k * b_{j-k} (with wraparound *5 terms).
         *
         * Each uint64x2_t holds [d_j_block0, d_j_block1].  vmull_u32
         * does both 32x32->64 multiplications in parallel.  vmlal_u32
         * does multiply-accumulate. */
        uint64x2_t d0 = vmull_u32(v_a_0, v_r_0);
        d0 = vmlal_u32(d0, v_a_1, v_s_4);
        d0 = vmlal_u32(d0, v_a_2, v_s_3);
        d0 = vmlal_u32(d0, v_a_3, v_s_2);
        d0 = vmlal_u32(d0, v_a_4, v_s_1);

        uint64x2_t d1 = vmull_u32(v_a_0, v_r_1);
        d1 = vmlal_u32(d1, v_a_1, v_r_0);
        d1 = vmlal_u32(d1, v_a_2, v_s_4);
        d1 = vmlal_u32(d1, v_a_3, v_s_3);
        d1 = vmlal_u32(d1, v_a_4, v_s_2);

        uint64x2_t d2 = vmull_u32(v_a_0, v_r_2);
        d2 = vmlal_u32(d2, v_a_1, v_r_1);
        d2 = vmlal_u32(d2, v_a_2, v_r_0);
        d2 = vmlal_u32(d2, v_a_3, v_s_4);
        d2 = vmlal_u32(d2, v_a_4, v_s_3);

        uint64x2_t d3 = vmull_u32(v_a_0, v_r_3);
        d3 = vmlal_u32(d3, v_a_1, v_r_2);
        d3 = vmlal_u32(d3, v_a_2, v_r_1);
        d3 = vmlal_u32(d3, v_a_3, v_r_0);
        d3 = vmlal_u32(d3, v_a_4, v_s_4);

        uint64x2_t d4 = vmull_u32(v_a_0, v_r_4);
        d4 = vmlal_u32(d4, v_a_1, v_r_3);
        d4 = vmlal_u32(d4, v_a_2, v_r_2);
        d4 = vmlal_u32(d4, v_a_3, v_r_1);
        d4 = vmlal_u32(d4, v_a_4, v_r_0);

        /* Sum the two lanes per limb: total_d_j = d_j_block0 + d_j_block1.
         * vaddvq_u64 horizontally adds both lanes of a uint64x2_t. */
        uint64_t t0 = vaddvq_u64(d0);
        uint64_t t1 = vaddvq_u64(d1);
        uint64_t t2 = vaddvq_u64(d2);
        uint64_t t3 = vaddvq_u64(d3);
        uint64_t t4 = vaddvq_u64(d4);

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

#else /* !aarch64 NEON */

bool poly1305NeonAvailable(void) {
    return false;
}

void poly1305NeonBlock2x(uint64_t[5], const uint32_t[5], const uint32_t[5],
                         const uint8_t *, size_t) {
    assert(!"poly1305NeonBlock2x invoked on non-aarch64 build — caller "
            "must check poly1305NeonAvailable() first");
}

#endif /* POLY1305_NEON_BUILD */
