/* ChaCha20 — ARM NEON 4-block batched keystream generator.
 *
 * Layout: state-per-lane horizontal SIMD.  Each of 16 uint32x4_t
 * vectors holds the same state-word position across 4 independent
 * blocks (lane i = block i, slot 0..3).  Every ChaCha quarter-round is
 * therefore lane-parallel across 4 blocks at once.
 *
 * Reference: Bernstein "ChaCha, a variant of Salsa20" and the
 * floodyberry / libsodium NEON implementations follow the same
 * transposed layout — 16 vector registers, 4 blocks per call.  We use
 * 4 because (a) it matches the NEON register file (16 q-regs is
 * enough), (b) the dispatch boundary (every 256 bytes) keeps cache and
 * counter arithmetic simple.
 *
 * Rotates: ChaCha needs rotr by 16, 12, 8, 7.  NEON has no native
 * rotate; we synthesize:
 *   rotr16 = vrev32q_u16 over reinterpreted u16x8 (byte-swap halves).
 *   rotr8  = byte-permute via vqtbl1q_u8 with a fixed index table
 *            (equivalent to PSHUFB / SSE).
 *   rotr12 / rotr7 = vshlq + vsriq with literal shift counts.
 *
 * Differential parity vs the scalar chacha20Block is enforced by
 * dkTestChaChaSimdParity in the test driver. */

#include "chacha20Neon.h"

#include "../ctmem.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__aarch64__) && defined(__ARM_NEON)
#define CHACHA20_NEON_BUILD 1
#else
#define CHACHA20_NEON_BUILD 0
#endif

#if CHACHA20_NEON_BUILD

#include <arm_neon.h>

bool chacha20NeonAvailable(void) {
    /* AArch64 mandates NEON; Apple Silicon and every shipping Cortex-A
     * exposes it.  No runtime probe needed — the compile-time gate
     * above is sufficient. */
    return true;
}

/* ------------------------------------------------------------------ */
/* NEON rotate helpers                                                 */
/* ------------------------------------------------------------------ */

static inline uint32x4_t rotl32_16(uint32x4_t x) {
    /* rotl by 16 == rotr by 16 == byte-swap pairs of 16-bit halves. */
    return vreinterpretq_u32_u16(vrev32q_u16(vreinterpretq_u16_u32(x)));
}

static inline uint32x4_t rotl32_8(uint32x4_t x) {
    /* rotl by 8 — byte permute via vqtbl1q_u8.  Indices below map each
     * output byte to its input source: rotated left by 8 bits per
     * 32-bit lane means byte0 <- byte1, byte1 <- byte2, byte2 <- byte3,
     * byte3 <- byte0. */
    static constexpr uint8_t tbl[16] = {3,  0, 1, 2,  7,  4,  5,  6,
                                        11, 8, 9, 10, 15, 12, 13, 14};
    uint8x16_t idx = vld1q_u8(tbl);
    return vreinterpretq_u32_u8(vqtbl1q_u8(vreinterpretq_u8_u32(x), idx));
}

/* Constant-shift macros: vsriq_n_u32 fuses the shift-right-with-insert,
 * giving a single instruction in many ChaCha-G ladders. */
#define ROTL32_12(x) vsriq_n_u32(vshlq_n_u32((x), 12), (x), 20)
#define ROTL32_7(x) vsriq_n_u32(vshlq_n_u32((x), 7), (x), 25)

/* ChaCha quarter-round on four lanes in parallel.  Each parameter is
 * a uint32x4_t holding one state-word position across 4 blocks. */
#define QR_NEON(a, b, c, d)                                                    \
    do {                                                                       \
        (a) = vaddq_u32((a), (b));                                             \
        (d) = veorq_u32((d), (a));                                             \
        (d) = rotl32_16(d);                                                    \
        (c) = vaddq_u32((c), (d));                                             \
        (b) = veorq_u32((b), (c));                                             \
        (b) = ROTL32_12(b);                                                    \
        (a) = vaddq_u32((a), (b));                                             \
        (d) = veorq_u32((d), (a));                                             \
        (d) = rotl32_8(d);                                                     \
        (c) = vaddq_u32((c), (d));                                             \
        (b) = veorq_u32((b), (c));                                             \
        (b) = ROTL32_7(b);                                                     \
    } while (0)

void chacha20NeonBlock4(const uint32_t state[16], uint8_t out[256]) {
    /* Initialise the 16 transposed state vectors.  state[0..11] are
     * the same across all 4 blocks (constants, key, nonce); state[12]
     * is the counter, which increments by 1 per block. */
    uint32x4_t s0 = vdupq_n_u32(state[0]);
    uint32x4_t s1 = vdupq_n_u32(state[1]);
    uint32x4_t s2 = vdupq_n_u32(state[2]);
    uint32x4_t s3 = vdupq_n_u32(state[3]);
    uint32x4_t s4 = vdupq_n_u32(state[4]);
    uint32x4_t s5 = vdupq_n_u32(state[5]);
    uint32x4_t s6 = vdupq_n_u32(state[6]);
    uint32x4_t s7 = vdupq_n_u32(state[7]);
    uint32x4_t s8 = vdupq_n_u32(state[8]);
    uint32x4_t s9 = vdupq_n_u32(state[9]);
    uint32x4_t s10 = vdupq_n_u32(state[10]);
    uint32x4_t s11 = vdupq_n_u32(state[11]);
    /* Counter lane: [c+0, c+1, c+2, c+3].  Wrapping mod 2^32 is
     * correct: callers ensure they never cross the wrap. */
    uint32_t c_base = state[12];
    uint32_t ctrs[4] = {c_base, c_base + 1u, c_base + 2u, c_base + 3u};
    uint32x4_t s12 = vld1q_u32(ctrs);
    uint32x4_t s13 = vdupq_n_u32(state[13]);
    uint32x4_t s14 = vdupq_n_u32(state[14]);
    uint32x4_t s15 = vdupq_n_u32(state[15]);

    /* Working copies. */
    uint32x4_t x0 = s0, x1 = s1, x2 = s2, x3 = s3;
    uint32x4_t x4 = s4, x5 = s5, x6 = s6, x7 = s7;
    uint32x4_t x8 = s8, x9 = s9, x10 = s10, x11 = s11;
    uint32x4_t x12 = s12, x13 = s13, x14 = s14, x15 = s15;

    /* 20 rounds = 10 doublerounds (column + diagonal). */
    for (uint32_t i = 0; i < 10; i++) {
        /* Column rounds. */
        QR_NEON(x0, x4, x8, x12);
        QR_NEON(x1, x5, x9, x13);
        QR_NEON(x2, x6, x10, x14);
        QR_NEON(x3, x7, x11, x15);
        /* Diagonal rounds. */
        QR_NEON(x0, x5, x10, x15);
        QR_NEON(x1, x6, x11, x12);
        QR_NEON(x2, x7, x8, x13);
        QR_NEON(x3, x4, x9, x14);
    }

    /* Add original state back. */
    x0 = vaddq_u32(x0, s0);
    x1 = vaddq_u32(x1, s1);
    x2 = vaddq_u32(x2, s2);
    x3 = vaddq_u32(x3, s3);
    x4 = vaddq_u32(x4, s4);
    x5 = vaddq_u32(x5, s5);
    x6 = vaddq_u32(x6, s6);
    x7 = vaddq_u32(x7, s7);
    x8 = vaddq_u32(x8, s8);
    x9 = vaddq_u32(x9, s9);
    x10 = vaddq_u32(x10, s10);
    x11 = vaddq_u32(x11, s11);
    x12 = vaddq_u32(x12, s12);
    x13 = vaddq_u32(x13, s13);
    x14 = vaddq_u32(x14, s14);
    x15 = vaddq_u32(x15, s15);

    /* De-transpose and store: block i's word j lives in lane i of x[j].
     * The most direct approach is to spill each vector to a 4x4 staging
     * area and pack out per block.  NEON loads are little-endian on
     * AArch64 which matches ChaCha's byte order. */
    alignas(16) uint32_t staging[16][4];
    vst1q_u32(staging[0], x0);
    vst1q_u32(staging[1], x1);
    vst1q_u32(staging[2], x2);
    vst1q_u32(staging[3], x3);
    vst1q_u32(staging[4], x4);
    vst1q_u32(staging[5], x5);
    vst1q_u32(staging[6], x6);
    vst1q_u32(staging[7], x7);
    vst1q_u32(staging[8], x8);
    vst1q_u32(staging[9], x9);
    vst1q_u32(staging[10], x10);
    vst1q_u32(staging[11], x11);
    vst1q_u32(staging[12], x12);
    vst1q_u32(staging[13], x13);
    vst1q_u32(staging[14], x14);
    vst1q_u32(staging[15], x15);

    for (uint32_t b = 0; b < 4; b++) {
        uint8_t *o = out + b * 64u;
        for (uint32_t w = 0; w < 16; w++) {
            uint32_t v = staging[w][b];
            o[w * 4 + 0] = (uint8_t)(v);
            o[w * 4 + 1] = (uint8_t)(v >> 8);
            o[w * 4 + 2] = (uint8_t)(v >> 16);
            o[w * 4 + 3] = (uint8_t)(v >> 24);
        }
    }

    /* staging[][] holds the full ChaCha20 keystream for 4 blocks (the
     * post-rounds-plus-original-state values); XORing this under the
     * same (key, nonce) pair recovers plaintext, so wipe before the
     * stack frame is released. */
    dkSecureZero(staging, sizeof(staging));
}

#else /* !aarch64 NEON */

bool chacha20NeonAvailable(void) {
    return false;
}

/* Soft stub: callers must gate on Available() and never reach here.
 * Asserting in debug builds catches regressions; production builds
 * silently produce zeros (the dispatcher will not invoke us). */
void chacha20NeonBlock4(const uint32_t[16], uint8_t out[256]) {
    if (out != nullptr) {
        memset(out, 0, 256);
    }
    assert(!"chacha20NeonBlock4 invoked on non-aarch64 build — caller "
            "must check chacha20NeonAvailable() first");
}

#endif /* CHACHA20_NEON_BUILD */
