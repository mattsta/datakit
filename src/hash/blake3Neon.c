/* BLAKE3 — ARM NEON accelerated compression.
 *
 * Single-block path: vectorize the four parallel G operations of each
 * round across 128-bit NEON vectors v0..v3 (rows of the 4x4 state).
 * The diagonal step is a shuffled column step, achieved by rotating
 * the lanes of v1, v2, v3 left by 1, 2, 3 respectively before doing
 * the same G code, then rotating back.
 *
 * Batched path: 4-way parallel compression of four independent blocks
 * using transposed layout — each of the 16 state words lives in its
 * own uint32x4_t with one chunk per lane. This applies the scalar G
 * formula across all four chunks simultaneously.
 *
 * Differential self-tests compare against blake3PortableCompressInPlace
 * and blake3PortableCompressXof, declared in blake3.h. */

#include "blake3Neon.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)

#include <arm_neon.h>

/* ------------------------------------------------------------------ */
/* Shared constants (kept private — same values as in blake3.c)        */
/* ------------------------------------------------------------------ */

static const uint32_t BLAKE3N_IV[8] = {
    0x6A09E667ul, 0xBB67AE85ul, 0x3C6EF372ul, 0xA54FF53Aul,
    0x510E527Ful, 0x9B05688Cul, 0x1F83D9ABul, 0x5BE0CD19ul,
};

static const uint8_t BLAKE3N_MSG_SCHEDULE[7][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8},
    {3, 4, 10, 12, 13, 2, 7, 14, 6, 5, 9, 0, 11, 15, 8, 1},
    {10, 7, 12, 9, 14, 3, 13, 15, 4, 0, 11, 2, 5, 8, 1, 6},
    {12, 13, 9, 11, 15, 10, 14, 8, 7, 2, 5, 3, 0, 1, 6, 4},
    {9, 14, 11, 5, 8, 12, 15, 1, 13, 3, 0, 10, 2, 6, 4, 7},
    {11, 15, 5, 0, 1, 9, 8, 6, 14, 10, 2, 12, 3, 4, 7, 13},
};

/* ------------------------------------------------------------------ */
/* NEON helpers                                                        */
/* ------------------------------------------------------------------ */

static inline uint32x4_t rotr32_16(uint32x4_t x) {
    /* rotr by 16 = byte-swap pairs of 16-bit halves. */
    return vreinterpretq_u32_u16(vrev32q_u16(vreinterpretq_u16_u32(x)));
}

static inline uint32x4_t rotr32_8(uint32x4_t x) {
    /* rotr by 8 = byte permutation. */
    static const uint8_t tbl[16] = {1, 2,  3,  0, 5,  6,  7,  4,
                                    9, 10, 11, 8, 13, 14, 15, 12};
    uint8x16_t idx = vld1q_u8(tbl);
    return vreinterpretq_u32_u8(vqtbl1q_u8(vreinterpretq_u8_u32(x), idx));
}

/* GCC/Clang require shift-count to be an immediate constant; wrap the
 * specific rotates we need as macros so the immediates propagate.
 * Using vsriq fuses the shift-right into the shift-left register. */
#define ROTR32_12(x) vsriq_n_u32(vshlq_n_u32((x), 20), (x), 12)
#define ROTR32_7(x) vsriq_n_u32(vshlq_n_u32((x), 25), (x), 7)

/* Lane rotations for diagonal step: rotate uint32x4_t lanes left by k. */
#define ROT_LANES_LEFT1(v) vextq_u32((v), (v), 1)
#define ROT_LANES_LEFT2(v) vextq_u32((v), (v), 2)
#define ROT_LANES_LEFT3(v) vextq_u32((v), (v), 3)

/* ------------------------------------------------------------------ */
/* Single-block compression (vectorized within the state)              */
/* ------------------------------------------------------------------ */

/* Apply one G-step quartet across 4 lanes in parallel.
 *   v0' = v0 + v1 + mx
 *   v3' = rotr(v3 ^ v0', 16)
 *   v2' = v2 + v3'
 *   v1' = rotr(v1 ^ v2', 12)
 *   v0''= v0' + v1' + my
 *   v3''= rotr(v3' ^ v0'', 8)
 *   v2''= v2' + v3''
 *   v1''= rotr(v1' ^ v2'', 7)
 */
static inline void g_quartet(uint32x4_t *v0, uint32x4_t *v1, uint32x4_t *v2,
                             uint32x4_t *v3, uint32x4_t mx, uint32x4_t my) {
    *v0 = vaddq_u32(vaddq_u32(*v0, *v1), mx);
    *v3 = rotr32_16(veorq_u32(*v3, *v0));
    *v2 = vaddq_u32(*v2, *v3);
    *v1 = ROTR32_12(veorq_u32(*v1, *v2));
    *v0 = vaddq_u32(vaddq_u32(*v0, *v1), my);
    *v3 = rotr32_8(veorq_u32(*v3, *v0));
    *v2 = vaddq_u32(*v2, *v3);
    *v1 = ROTR32_7(veorq_u32(*v1, *v2));
}

/* Build a uint32x4_t from four indexed message words. */
static inline uint32x4_t pack4(const uint32_t m[16], uint8_t a, uint8_t b,
                               uint8_t c, uint8_t d) {
    uint32_t tmp[4] = {m[a], m[b], m[c], m[d]};
    return vld1q_u32(tmp);
}

/* compress_pre: produce the 16-word post-round state. */
static void neon_compress_pre(uint32x4_t out[4], const uint32_t cv[8],
                              const uint8_t block[BLAKE3_BLOCK_LEN],
                              uint8_t block_len, uint64_t counter,
                              uint8_t flags) {
    /* Load 16 message words (little-endian). NEON loads are LE on
     * AArch64, which is our target. */
    uint32_t m[16];
    memcpy(m, block, BLAKE3_BLOCK_LEN);

    uint32x4_t v0 = vld1q_u32(&cv[0]);
    uint32x4_t v1 = vld1q_u32(&cv[4]);
    uint32x4_t v2 = vld1q_u32(BLAKE3N_IV);
    uint32_t low_words[4] = {
        BLAKE3N_IV[4],
        BLAKE3N_IV[5],
        BLAKE3N_IV[6],
        BLAKE3N_IV[7],
    };
    (void)low_words; /* v3 below uses counter/block_len/flags */
    uint32_t v3_words[4] = {
        (uint32_t)(counter & 0xFFFFFFFFul),
        (uint32_t)(counter >> 32),
        (uint32_t)block_len,
        (uint32_t)flags,
    };
    uint32x4_t v3 = vld1q_u32(v3_words);

    for (uint32_t r = 0; r < 7; r++) {
        const uint8_t *s = BLAKE3N_MSG_SCHEDULE[r];

        /* Column step: G on quartets (0,4,8,12) (1,5,9,13) (2,6,10,14)
         * (3,7,11,15) — already aligned as columns of v0,v1,v2,v3. */
        uint32x4_t mx_col = pack4(m, s[0], s[2], s[4], s[6]);
        uint32x4_t my_col = pack4(m, s[1], s[3], s[5], s[7]);
        g_quartet(&v0, &v1, &v2, &v3, mx_col, my_col);

        /* Diagonal step: rotate v1/v2/v3 lanes so the diagonals line
         * up as columns; same code path; rotate back. */
        v1 = ROT_LANES_LEFT1(v1);
        v2 = ROT_LANES_LEFT2(v2);
        v3 = ROT_LANES_LEFT3(v3);

        uint32x4_t mx_diag = pack4(m, s[8], s[10], s[12], s[14]);
        uint32x4_t my_diag = pack4(m, s[9], s[11], s[13], s[15]);
        g_quartet(&v0, &v1, &v2, &v3, mx_diag, my_diag);

        v1 = ROT_LANES_LEFT3(v1); /* undo +1 */
        v2 = ROT_LANES_LEFT2(v2); /* undo +2 (self-inverse) */
        v3 = ROT_LANES_LEFT1(v3); /* undo +3 */
    }

    out[0] = v0;
    out[1] = v1;
    out[2] = v2;
    out[3] = v3;
}

void blake3NeonCompressInPlace(uint32_t cv[8],
                               const uint8_t block[BLAKE3_BLOCK_LEN],
                               uint8_t block_len, uint64_t counter,
                               uint8_t flags) {
    uint32x4_t s[4];
    neon_compress_pre(s, cv, block, block_len, counter, flags);
    /* Output CV is the XOR of the two halves of the 16-word state. */
    uint32x4_t hi0 = s[2];
    uint32x4_t hi1 = s[3];
    uint32x4_t out0 = veorq_u32(s[0], hi0);
    uint32x4_t out1 = veorq_u32(s[1], hi1);
    vst1q_u32(&cv[0], out0);
    vst1q_u32(&cv[4], out1);
}

void blake3NeonCompressXof(const uint32_t cv[8],
                           const uint8_t block[BLAKE3_BLOCK_LEN],
                           uint8_t block_len, uint64_t counter, uint8_t flags,
                           uint8_t out[64]) {
    uint32x4_t s[4];
    neon_compress_pre(s, cv, block, block_len, counter, flags);

    uint32x4_t cv0 = vld1q_u32(&cv[0]);
    uint32x4_t cv1 = vld1q_u32(&cv[4]);

    uint32x4_t lo0 = veorq_u32(s[0], s[2]);
    uint32x4_t lo1 = veorq_u32(s[1], s[3]);
    uint32x4_t hi0 = veorq_u32(s[2], cv0);
    uint32x4_t hi1 = veorq_u32(s[3], cv1);

    vst1q_u32((uint32_t *)(out + 0), lo0);
    vst1q_u32((uint32_t *)(out + 16), lo1);
    vst1q_u32((uint32_t *)(out + 32), hi0);
    vst1q_u32((uint32_t *)(out + 48), hi1);
}

/* ------------------------------------------------------------------ */
/* 4-way batched compression                                           */
/* ------------------------------------------------------------------ */

/* Transposed layout: vector i holds state word i across 4 chunks.
 * Each G operation broadcasts over the 4 lanes (one chunk per lane). */
static inline void g4(uint32x4_t *a, uint32x4_t *b, uint32x4_t *c,
                      uint32x4_t *d, uint32x4_t mx, uint32x4_t my) {
    *a = vaddq_u32(vaddq_u32(*a, *b), mx);
    *d = rotr32_16(veorq_u32(*d, *a));
    *c = vaddq_u32(*c, *d);
    *b = ROTR32_12(veorq_u32(*b, *c));
    *a = vaddq_u32(vaddq_u32(*a, *b), my);
    *d = rotr32_8(veorq_u32(*d, *a));
    *c = vaddq_u32(*c, *d);
    *b = ROTR32_7(veorq_u32(*b, *c));
}

void blake3NeonCompress4(const uint32_t *const cv_inputs[4],
                         const uint8_t *const blocks[4],
                         const uint8_t block_lens[4],
                         const uint64_t counters[4], const uint8_t flag_set[4],
                         uint8_t out_cvs[4][32]) {
    /* Load 16 message words per chunk and transpose by gathering
     * word-i from each chunk into m[i]. */
    uint32x4_t m[16];
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t w[4];
        for (uint32_t c = 0; c < 4; c++) {
            memcpy(&w[c], blocks[c] + i * 4, sizeof(uint32_t));
        }
        m[i] = vld1q_u32(w);
    }

    /* Initialize transposed state: state[i] = vector of (chunk0..3)[i]. */
    uint32x4_t v[16];
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t w[4] = {
            cv_inputs[0][i],
            cv_inputs[1][i],
            cv_inputs[2][i],
            cv_inputs[3][i],
        };
        v[i] = vld1q_u32(w);
    }
    for (uint32_t i = 0; i < 4; i++) {
        v[8 + i] = vdupq_n_u32(BLAKE3N_IV[i]);
    }
    {
        uint32_t lo[4] = {
            (uint32_t)(counters[0] & 0xFFFFFFFFul),
            (uint32_t)(counters[1] & 0xFFFFFFFFul),
            (uint32_t)(counters[2] & 0xFFFFFFFFul),
            (uint32_t)(counters[3] & 0xFFFFFFFFul),
        };
        uint32_t hi[4] = {
            (uint32_t)(counters[0] >> 32),
            (uint32_t)(counters[1] >> 32),
            (uint32_t)(counters[2] >> 32),
            (uint32_t)(counters[3] >> 32),
        };
        uint32_t bl[4] = {block_lens[0], block_lens[1], block_lens[2],
                          block_lens[3]};
        uint32_t fl[4] = {flag_set[0], flag_set[1], flag_set[2], flag_set[3]};
        v[12] = vld1q_u32(lo);
        v[13] = vld1q_u32(hi);
        v[14] = vld1q_u32(bl);
        v[15] = vld1q_u32(fl);
    }

    for (uint32_t r = 0; r < 7; r++) {
        const uint8_t *s = BLAKE3N_MSG_SCHEDULE[r];
        /* Column step. */
        g4(&v[0], &v[4], &v[8], &v[12], m[s[0]], m[s[1]]);
        g4(&v[1], &v[5], &v[9], &v[13], m[s[2]], m[s[3]]);
        g4(&v[2], &v[6], &v[10], &v[14], m[s[4]], m[s[5]]);
        g4(&v[3], &v[7], &v[11], &v[15], m[s[6]], m[s[7]]);
        /* Diagonal step. */
        g4(&v[0], &v[5], &v[10], &v[15], m[s[8]], m[s[9]]);
        g4(&v[1], &v[6], &v[11], &v[12], m[s[10]], m[s[11]]);
        g4(&v[2], &v[7], &v[8], &v[13], m[s[12]], m[s[13]]);
        g4(&v[3], &v[4], &v[9], &v[14], m[s[14]], m[s[15]]);
    }

    /* Output CV: state[0..7] ^ state[8..15], then transpose back per
     * chunk and store. */
    uint32_t buf[8][4];
    for (uint32_t i = 0; i < 8; i++) {
        vst1q_u32(buf[i], veorq_u32(v[i], v[i + 8]));
    }
    for (uint32_t c = 0; c < 4; c++) {
        for (uint32_t i = 0; i < 8; i++) {
            uint32_t w = buf[i][c];
            memcpy(out_cvs[c] + i * 4, &w, sizeof(uint32_t));
        }
    }
}

bool blake3NeonAvailable(void) {
    return true;
}

#else /* No NEON: stubs that delegate to the portable scalar path.             \
       *                                                                       \
       * Available() returns false so the dispatcher never picks               \
       * these paths.  But if a caller invokes them directly                   \
       * (a test, a future feature, a benchmark) we must produce               \
       * the correct output — a silent no-op would corrupt every             \
       * downstream hash.  Delegating is slower than the missing               \
       * NEON path would be, never wrong. */

bool blake3NeonAvailable(void) {
    return false;
}

void blake3NeonCompressInPlace(uint32_t cv[8],
                               const uint8_t block[BLAKE3_BLOCK_LEN],
                               uint8_t block_len, uint64_t counter,
                               uint8_t flags) {
    blake3PortableCompressInPlace(cv, block, block_len, counter, flags);
}

void blake3NeonCompressXof(const uint32_t cv[8],
                           const uint8_t block[BLAKE3_BLOCK_LEN],
                           uint8_t block_len, uint64_t counter, uint8_t flags,
                           uint8_t out[64]) {
    blake3PortableCompressXof(cv, block, block_len, counter, flags, out);
}

void blake3NeonCompress4(const uint32_t *const cv_inputs[4],
                         const uint8_t *const blocks[4],
                         const uint8_t block_lens[4],
                         const uint64_t counters[4], const uint8_t flag_set[4],
                         uint8_t out_cvs[4][32]) {
    /* Serialize the 4-way batch into 4 portable compresses.  Same
     * output, just no SIMD speedup. */
    for (int i = 0; i < 4; i++) {
        uint32_t cv[8];
        memcpy(cv, cv_inputs[i], sizeof(cv));
        blake3PortableCompressInPlace(cv, blocks[i], block_lens[i], counters[i],
                                      flag_set[i]);
        for (int j = 0; j < 8; j++) {
            out_cvs[i][j * 4 + 0] = (uint8_t)(cv[j]);
            out_cvs[i][j * 4 + 1] = (uint8_t)(cv[j] >> 8);
            out_cvs[i][j * 4 + 2] = (uint8_t)(cv[j] >> 16);
            out_cvs[i][j * 4 + 3] = (uint8_t)(cv[j] >> 24);
        }
    }
}

#endif /* NEON */

/* ====================================================================== */
/* Test driver                                                            */
/* ====================================================================== */

#ifdef DATAKIT_TEST

#include "ctest.h"
#include <time.h>

/* ----- Portable reference primitives (weak forward decls) ----------- */
/* If blake3.c exposes these wrappers, the linker resolves them.
 * Otherwise the test falls back to a local re-implementation. */
__attribute__((weak)) void blake3PortableCompressInPlace(
    uint32_t cv[8], const uint8_t block[BLAKE3_BLOCK_LEN], uint8_t block_len,
    uint64_t counter, uint8_t flags);
__attribute__((weak)) void blake3PortableCompressXof(
    const uint32_t cv[8], const uint8_t block[BLAKE3_BLOCK_LEN],
    uint8_t block_len, uint64_t counter, uint8_t flags, uint8_t out[64]);

/* Local scalar fallback: a direct copy of the spec compression logic.
 * Used iff the weak symbols above were not provided. */
static const uint32_t REF_IV[8] = {
    0x6A09E667ul, 0xBB67AE85ul, 0x3C6EF372ul, 0xA54FF53Aul,
    0x510E527Ful, 0x9B05688Cul, 0x1F83D9ABul, 0x5BE0CD19ul,
};
static const uint8_t REF_SCHED[7][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8},
    {3, 4, 10, 12, 13, 2, 7, 14, 6, 5, 9, 0, 11, 15, 8, 1},
    {10, 7, 12, 9, 14, 3, 13, 15, 4, 0, 11, 2, 5, 8, 1, 6},
    {12, 13, 9, 11, 15, 10, 14, 8, 7, 2, 5, 3, 0, 1, 6, 4},
    {9, 14, 11, 5, 8, 12, 15, 1, 13, 3, 0, 10, 2, 6, 4, 7},
    {11, 15, 5, 0, 1, 9, 8, 6, 14, 10, 2, 12, 3, 4, 7, 13},
};
static inline uint32_t ref_rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32u - n));
}
static inline void ref_g(uint32_t st[16], uint32_t a, uint32_t b, uint32_t c,
                         uint32_t d, uint32_t mx, uint32_t my) {
    st[a] = st[a] + st[b] + mx;
    st[d] = ref_rotr32(st[d] ^ st[a], 16);
    st[c] = st[c] + st[d];
    st[b] = ref_rotr32(st[b] ^ st[c], 12);
    st[a] = st[a] + st[b] + my;
    st[d] = ref_rotr32(st[d] ^ st[a], 8);
    st[c] = st[c] + st[d];
    st[b] = ref_rotr32(st[b] ^ st[c], 7);
}
static void ref_compress_pre(uint32_t st[16], const uint32_t cv[8],
                             const uint8_t block[64], uint8_t block_len,
                             uint64_t counter, uint8_t flags) {
    uint32_t m[16];
    memcpy(m, block, 64);
    for (uint32_t i = 0; i < 8; i++) {
        st[i] = cv[i];
    }
    for (uint32_t i = 0; i < 4; i++) {
        st[8 + i] = REF_IV[i];
    }
    st[12] = (uint32_t)(counter & 0xFFFFFFFFul);
    st[13] = (uint32_t)(counter >> 32);
    st[14] = block_len;
    st[15] = flags;
    for (uint32_t r = 0; r < 7; r++) {
        const uint8_t *s = REF_SCHED[r];
        ref_g(st, 0, 4, 8, 12, m[s[0]], m[s[1]]);
        ref_g(st, 1, 5, 9, 13, m[s[2]], m[s[3]]);
        ref_g(st, 2, 6, 10, 14, m[s[4]], m[s[5]]);
        ref_g(st, 3, 7, 11, 15, m[s[6]], m[s[7]]);
        ref_g(st, 0, 5, 10, 15, m[s[8]], m[s[9]]);
        ref_g(st, 1, 6, 11, 12, m[s[10]], m[s[11]]);
        ref_g(st, 2, 7, 8, 13, m[s[12]], m[s[13]]);
        ref_g(st, 3, 4, 9, 14, m[s[14]], m[s[15]]);
    }
}
static void ref_compress_in_place(uint32_t cv[8], const uint8_t block[64],
                                  uint8_t block_len, uint64_t counter,
                                  uint8_t flags) {
    uint32_t st[16];
    ref_compress_pre(st, cv, block, block_len, counter, flags);
    for (uint32_t i = 0; i < 8; i++) {
        cv[i] = st[i] ^ st[i + 8];
    }
}
static void ref_compress_xof(const uint32_t cv[8], const uint8_t block[64],
                             uint8_t block_len, uint64_t counter, uint8_t flags,
                             uint8_t out[64]) {
    uint32_t st[16];
    ref_compress_pre(st, cv, block, block_len, counter, flags);
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t lo = st[i] ^ st[i + 8];
        uint32_t hi = st[i + 8] ^ cv[i];
        memcpy(out + i * 4, &lo, 4);
        memcpy(out + (i + 8) * 4, &hi, 4);
    }
}

/* Resolve which scalar implementation to compare against. */
static void portable_compress_in_place(uint32_t cv[8], const uint8_t block[64],
                                       uint8_t block_len, uint64_t counter,
                                       uint8_t flags) {
    if (blake3PortableCompressInPlace) {
        blake3PortableCompressInPlace(cv, block, block_len, counter, flags);
    } else {
        ref_compress_in_place(cv, block, block_len, counter, flags);
    }
}
static void portable_compress_xof(const uint32_t cv[8], const uint8_t block[64],
                                  uint8_t block_len, uint64_t counter,
                                  uint8_t flags, uint8_t out[64]) {
    if (blake3PortableCompressXof) {
        blake3PortableCompressXof(cv, block, block_len, counter, flags, out);
    } else {
        ref_compress_xof(cv, block, block_len, counter, flags, out);
    }
}

/* Lightweight xorshift64* for deterministic random data. */
static uint64_t prng_state = 0x9E3779B97F4A7C15ull;
static inline uint64_t prng_next(void) {
    uint64_t x = prng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    prng_state = x;
    return x * 0x2545F4914F6CDD1Dull;
}
static void prng_fill(uint8_t *p, size_t n) {
    while (n >= 8) {
        uint64_t v = prng_next();
        memcpy(p, &v, 8);
        p += 8;
        n -= 8;
    }
    if (n) {
        uint64_t v = prng_next();
        memcpy(p, &v, n);
    }
}

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int blake3NeonTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    /* Stub-delegation guard: on non-NEON builds blake3NeonCompressInPlace
     * must delegate to the portable path, not return as a silent no-op.
     * Verify a single direct call agrees with the portable reference,
     * before deciding whether the rest of the suite (which assumes a
     * working SIMD path) applies. */
    TEST("stub delegation: neon-compress matches portable on direct call") {
        uint32_t cv[8] = {0u};
        uint8_t block[BLAKE3_BLOCK_LEN] = {0u};
        for (uint32_t i = 0; i < 8u; i++) {
            cv[i] = 0xDEADBEEFu + i * 0x1010101u;
        }
        for (uint32_t i = 0; i < BLAKE3_BLOCK_LEN; i++) {
            block[i] = (uint8_t)(i ^ 0x5Au);
        }
        const uint64_t counter = 42u;
        const uint8_t block_len = (uint8_t)BLAKE3_BLOCK_LEN;
        const uint8_t flags = 0x07u;

        uint32_t cv_ref[8], cv_simd[8];
        memcpy(cv_ref, cv, sizeof(cv));
        memcpy(cv_simd, cv, sizeof(cv));
        portable_compress_in_place(cv_ref, block, block_len, counter, flags);
        blake3NeonCompressInPlace(cv_simd, block, block_len, counter, flags);
        if (memcmp(cv_ref, cv_simd, sizeof(cv_ref)) != 0) {
            ERRR("Stub/SIMD compress-in-place output != portable");
        }
    }

    if (!blake3NeonAvailable()) {
        printf("(NEON unavailable; remaining suite skipped)\n");
        TEST_FINAL_RESULT;
    }

    TEST("blake3 neon single-block compresses match portable") {
        const uint32_t iters = 100000;
        for (uint32_t t = 0; t < iters; t++) {
            uint32_t cv[8];
            uint8_t block[BLAKE3_BLOCK_LEN];
            prng_fill((uint8_t *)cv, sizeof(cv));
            prng_fill(block, sizeof(block));
            uint64_t counter = prng_next();
            uint8_t block_len = (uint8_t)((prng_next() % BLAKE3_BLOCK_LEN) + 1);
            uint8_t flags = (uint8_t)(prng_next() & 0x7F);

            uint32_t cv_ref[8], cv_neon[8];
            memcpy(cv_ref, cv, sizeof(cv));
            memcpy(cv_neon, cv, sizeof(cv));

            portable_compress_in_place(cv_ref, block, block_len, counter,
                                       flags);
            blake3NeonCompressInPlace(cv_neon, block, block_len, counter,
                                      flags);

            if (memcmp(cv_ref, cv_neon, sizeof(cv_ref)) != 0) {
                ERR("in-place mismatch at iter=%u", t);
                break;
            }
        }
    }

    TEST("blake3 neon xof matches portable") {
        const uint32_t iters = 50000;
        for (uint32_t t = 0; t < iters; t++) {
            uint32_t cv[8];
            uint8_t block[BLAKE3_BLOCK_LEN];
            prng_fill((uint8_t *)cv, sizeof(cv));
            prng_fill(block, sizeof(block));
            uint64_t counter = prng_next();
            uint8_t block_len = (uint8_t)((prng_next() % BLAKE3_BLOCK_LEN) + 1);
            uint8_t flags = (uint8_t)(prng_next() & 0x7F);

            uint8_t out_ref[64], out_neon[64];
            portable_compress_xof(cv, block, block_len, counter, flags,
                                  out_ref);
            blake3NeonCompressXof(cv, block, block_len, counter, flags,
                                  out_neon);

            if (memcmp(out_ref, out_neon, 64) != 0) {
                ERR("xof mismatch at iter=%u", t);
                break;
            }
        }
    }

    TEST("blake3 neon hash matches public blake3Hash on 64-byte inputs") {
        /* Drive through the public API path for short messages (0..64 bytes)
         * to confirm that swapping the compress primitive doesn't perturb
         * spec compliance — checked against the official portable. */
        for (size_t len = 0; len <= 64; len++) {
            uint8_t in[64];
            prng_fill(in, len);
            uint8_t pub[BLAKE3_OUT_LEN];
            blake3Hash(in, len, pub);

            /* Single-chunk root: CV starts as IV, block is padded with 0,
             * block_len = len, counter = 0, flags = CHUNK_START | CHUNK_END |
             * ROOT. */
            uint32_t cv[8];
            memcpy(cv, REF_IV, sizeof(cv));
            uint8_t block[64] = {0};
            memcpy(block, in, len);
            uint8_t flags = (uint8_t)(0x01 | 0x02 | 0x08);
            uint8_t neon_out[64];
            blake3NeonCompressXof(cv, block, (uint8_t)len, 0, flags, neon_out);
            if (memcmp(pub, neon_out, BLAKE3_OUT_LEN) != 0) {
                ERR("public-API mismatch at len=%zu", len);
                break;
            }
        }
    }

    TEST("blake3 neon batched 4-way matches 4 separate single-blocks") {
        const uint32_t iters = 20000;
        for (uint32_t t = 0; t < iters; t++) {
            uint32_t cvs[4][8];
            uint8_t blocks[4][BLAKE3_BLOCK_LEN];
            uint8_t block_lens[4];
            uint64_t counters[4];
            uint8_t flags[4];

            for (uint32_t i = 0; i < 4; i++) {
                prng_fill((uint8_t *)cvs[i], sizeof(cvs[i]));
                prng_fill(blocks[i], BLAKE3_BLOCK_LEN);
                counters[i] = prng_next();
                block_lens[i] = (uint8_t)((prng_next() % BLAKE3_BLOCK_LEN) + 1);
                flags[i] = (uint8_t)(prng_next() & 0x7F);
            }

            const uint32_t *cv_ptrs[4] = {cvs[0], cvs[1], cvs[2], cvs[3]};
            const uint8_t *blk_ptrs[4] = {blocks[0], blocks[1], blocks[2],
                                          blocks[3]};
            uint8_t batched[4][32];
            blake3NeonCompress4(cv_ptrs, blk_ptrs, block_lens, counters, flags,
                                batched);

            for (uint32_t i = 0; i < 4; i++) {
                uint32_t single[8];
                memcpy(single, cvs[i], sizeof(single));
                blake3NeonCompressInPlace(single, blocks[i], block_lens[i],
                                          counters[i], flags[i]);
                uint8_t expect[32];
                memcpy(expect, single, 32);
                if (memcmp(expect, batched[i], 32) != 0) {
                    ERR("compress4 lane %u mismatch at iter=%u", i, t);
                    t = iters; /* stop outer loop too */
                    break;
                }
            }
        }
    }

    TEST("blake3 neon performance bench") {
        const size_t MB = 1024 * 1024;
        uint8_t *buf = (uint8_t *)malloc(MB);
        if (!buf) {
            ERRR("alloc failed");
            return err;
        }
        prng_fill(buf, MB);

        /* Public one-shot hasher. It dispatches to the NEON paths on this
         * hardware, so it is the end-to-end figure, not a portable one. */
        uint8_t digest[BLAKE3_OUT_LEN];
        double t0 = now_seconds();
        const uint32_t iters = 64;
        for (uint32_t i = 0; i < iters; i++) {
            blake3Hash(buf, MB, digest);
        }
        double t1 = now_seconds();
        double public_mib_s = (double)iters / (t1 - t0);

        /* Portable single-block compress: the scalar kernel in the same
         * shape as the NEON single-block loop below. */
        uint32_t cv[8];
        memcpy(cv, REF_IV, sizeof(cv));
        const uint32_t niters = 64;
        volatile uint32_t portable_sink = 0;
        t0 = now_seconds();
        for (uint32_t i = 0; i < niters; i++) {
            uint32_t cur[8];
            memcpy(cur, cv, sizeof(cur));
            const size_t nblocks = MB / BLAKE3_BLOCK_LEN;
            for (size_t b = 0; b < nblocks; b++) {
                portable_compress_in_place(cur, buf + b * BLAKE3_BLOCK_LEN,
                                           BLAKE3_BLOCK_LEN, (uint64_t)b, 0);
            }
            for (int k = 0; k < 8; k++) {
                portable_sink ^= cur[k];
            }
        }
        t1 = now_seconds();
        (void)portable_sink;
        double portable_mib_s = (double)niters / (t1 - t0);

        /* NEON single-block compress throughput: raw block-level
         * compress in the same shape the hasher's bulk path uses. */
        t0 = now_seconds();
        /* Volatile sink defeats dead-code elimination: without consuming
         * `cur` after the loop, -O3 NDEBUG can fold the whole bench away
         * and report `inf MiB/s`. */
        volatile uint32_t neon_sink = 0;
        for (uint32_t i = 0; i < niters; i++) {
            uint32_t cur[8];
            memcpy(cur, cv, sizeof(cur));
            const size_t nblocks = MB / BLAKE3_BLOCK_LEN;
            for (size_t b = 0; b < nblocks; b++) {
                blake3NeonCompressInPlace(cur, buf + b * BLAKE3_BLOCK_LEN,
                                          BLAKE3_BLOCK_LEN, (uint64_t)b, 0);
            }
            for (int k = 0; k < 8; k++) {
                neon_sink ^= cur[k];
            }
        }
        t1 = now_seconds();
        (void)neon_sink;
        double neon_mib_s = (double)niters / (t1 - t0);

        /* 4-way batch: same workload spread over 4 lanes. */
        t0 = now_seconds();
        volatile uint32_t batch_sink = 0;
        for (uint32_t i = 0; i < niters; i++) {
            uint32_t cur[4][8];
            for (uint32_t k = 0; k < 4; k++) {
                memcpy(cur[k], cv, sizeof(cv));
            }
            const size_t nblocks = MB / BLAKE3_BLOCK_LEN / 4;
            for (size_t b = 0; b < nblocks; b++) {
                const uint32_t *cvp[4] = {cur[0], cur[1], cur[2], cur[3]};
                const uint8_t *bp[4] = {
                    buf + (b * 4 + 0) * BLAKE3_BLOCK_LEN,
                    buf + (b * 4 + 1) * BLAKE3_BLOCK_LEN,
                    buf + (b * 4 + 2) * BLAKE3_BLOCK_LEN,
                    buf + (b * 4 + 3) * BLAKE3_BLOCK_LEN,
                };
                uint8_t bl[4] = {64, 64, 64, 64};
                uint64_t ct[4] = {b, b, b, b};
                uint8_t fl[4] = {0, 0, 0, 0};
                uint8_t out[4][32];
                blake3NeonCompress4(cvp, bp, bl, ct, fl, out);
                for (uint32_t k = 0; k < 4; k++) {
                    memcpy(cur[k], out[k], 32);
                }
            }
            for (uint32_t k = 0; k < 4; k++) {
                for (int m = 0; m < 8; m++) {
                    batch_sink ^= cur[k][m];
                }
            }
        }
        t1 = now_seconds();
        (void)batch_sink;
        double batch_mib_s = (double)niters / (t1 - t0);

        printf("    public blake3Hash (dispatched): %7.1f MiB/s\n",
               public_mib_s);
        printf("    portable single-block compress: %7.1f MiB/s\n",
               portable_mib_s);
        printf("    NEON single-block compress:     %7.1f MiB/s (%.2fx "
               "portable)\n",
               neon_mib_s, neon_mib_s / portable_mib_s);
        printf("    NEON 4-way batched:             %7.1f MiB/s (%.2fx "
               "portable)\n",
               batch_mib_s, batch_mib_s / portable_mib_s);
        free(buf);
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
