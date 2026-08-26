/* ChaCha20 — x86 AVX2 8-block batched keystream generator.
 *
 * Layout: state-per-lane horizontal SIMD.  Each of 16 __m256i vectors
 * holds the same state-word position across 8 independent blocks
 * (32-bit lane i = block i).  Every quarter-round operates lane-
 * parallel across 8 blocks.
 *
 * Reference: the standard AVX2 ChaCha layout used by libsodium /
 * floodyberry's chacha20-avx2 implementation.  We do not depend on
 * AVX-512 (VPROLD); instead synthesize rotates as shift+or for n=12,7
 * and as _mm256_shuffle_epi8 for n=16,8.
 *
 * Per-function __attribute__((target("avx2"))) lets us build on hosts
 * whose baseline ISA is below AVX2; the dispatcher will only invoke
 * Block8 after a positive runtime probe.
 *
 * Differential parity vs the scalar chacha20Block is enforced by
 * dkTestChaChaSimdParity in the test driver. */

#include "chacha20AVX2.h"

#include "../ctmem.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__x86_64__)
#define CHACHA20_AVX2_BUILD 1
#else
#define CHACHA20_AVX2_BUILD 0
#endif

#if CHACHA20_AVX2_BUILD

#include <immintrin.h>

bool chacha20AVX2Available(void) {
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") != 0;
}

/* ------------------------------------------------------------------ */
/* AVX2 rotate helpers                                                 */
/* ------------------------------------------------------------------ */

__attribute__((target("avx2"), always_inline)) static inline __m256i
rotl32_avx2(__m256i x, int n) {
    return _mm256_or_si256(_mm256_slli_epi32(x, n),
                           _mm256_srli_epi32(x, 32 - n));
}

__attribute__((target("avx2"), always_inline)) static inline __m256i
rotl32_16_avx2(__m256i x) {
    /* Byte permute: per 32-bit lane, byte j <- byte (j-2 mod 4).
     * Equivalent to rotate-left by 16. */
    const __m256i mask =
        _mm256_set_epi8(13, 12, 15, 14, 9, 8, 11, 10, 5, 4, 7, 6, 1, 0, 3, 2,
                        13, 12, 15, 14, 9, 8, 11, 10, 5, 4, 7, 6, 1, 0, 3, 2);
    return _mm256_shuffle_epi8(x, mask);
}

__attribute__((target("avx2"), always_inline)) static inline __m256i
rotl32_8_avx2(__m256i x) {
    /* Byte permute: per 32-bit lane, byte j <- byte (j-1 mod 4).
     * Equivalent to rotate-left by 8. */
    const __m256i mask =
        _mm256_set_epi8(14, 13, 12, 15, 10, 9, 8, 11, 6, 5, 4, 7, 2, 1, 0, 3,
                        14, 13, 12, 15, 10, 9, 8, 11, 6, 5, 4, 7, 2, 1, 0, 3);
    return _mm256_shuffle_epi8(x, mask);
}

/* ChaCha quarter-round on 8 lanes in parallel.  Each variable is a
 * __m256i holding one state-word position across 8 blocks. */
#define QR_AVX2(a, b, c, d)                                                    \
    do {                                                                       \
        (a) = _mm256_add_epi32((a), (b));                                      \
        (d) = _mm256_xor_si256((d), (a));                                      \
        (d) = rotl32_16_avx2(d);                                               \
        (c) = _mm256_add_epi32((c), (d));                                      \
        (b) = _mm256_xor_si256((b), (c));                                      \
        (b) = rotl32_avx2((b), 12);                                            \
        (a) = _mm256_add_epi32((a), (b));                                      \
        (d) = _mm256_xor_si256((d), (a));                                      \
        (d) = rotl32_8_avx2(d);                                                \
        (c) = _mm256_add_epi32((c), (d));                                      \
        (b) = _mm256_xor_si256((b), (c));                                      \
        (b) = rotl32_avx2((b), 7);                                             \
    } while (0)

__attribute__((target("avx2"))) void
chacha20AVX2Block8(const uint32_t state[16], uint8_t out[512]) {
    /* Initialise the 16 transposed state vectors.  state[0..11] are
     * the same across all 8 blocks; state[12] is the counter and
     * increments by 1 per block. */
    __m256i s0 = _mm256_set1_epi32((int)state[0]);
    __m256i s1 = _mm256_set1_epi32((int)state[1]);
    __m256i s2 = _mm256_set1_epi32((int)state[2]);
    __m256i s3 = _mm256_set1_epi32((int)state[3]);
    __m256i s4 = _mm256_set1_epi32((int)state[4]);
    __m256i s5 = _mm256_set1_epi32((int)state[5]);
    __m256i s6 = _mm256_set1_epi32((int)state[6]);
    __m256i s7 = _mm256_set1_epi32((int)state[7]);
    __m256i s8 = _mm256_set1_epi32((int)state[8]);
    __m256i s9 = _mm256_set1_epi32((int)state[9]);
    __m256i s10 = _mm256_set1_epi32((int)state[10]);
    __m256i s11 = _mm256_set1_epi32((int)state[11]);
    uint32_t c_base = state[12];
    /* Counter lane: 8 sequential counter values. */
    __m256i s12 = _mm256_set_epi32((int)(c_base + 7u), (int)(c_base + 6u),
                                   (int)(c_base + 5u), (int)(c_base + 4u),
                                   (int)(c_base + 3u), (int)(c_base + 2u),
                                   (int)(c_base + 1u), (int)(c_base + 0u));
    __m256i s13 = _mm256_set1_epi32((int)state[13]);
    __m256i s14 = _mm256_set1_epi32((int)state[14]);
    __m256i s15 = _mm256_set1_epi32((int)state[15]);

    __m256i x0 = s0, x1 = s1, x2 = s2, x3 = s3;
    __m256i x4 = s4, x5 = s5, x6 = s6, x7 = s7;
    __m256i x8 = s8, x9 = s9, x10 = s10, x11 = s11;
    __m256i x12 = s12, x13 = s13, x14 = s14, x15 = s15;

    /* 20 rounds = 10 doublerounds. */
    for (uint32_t i = 0; i < 10; i++) {
        /* Column rounds. */
        QR_AVX2(x0, x4, x8, x12);
        QR_AVX2(x1, x5, x9, x13);
        QR_AVX2(x2, x6, x10, x14);
        QR_AVX2(x3, x7, x11, x15);
        /* Diagonal rounds. */
        QR_AVX2(x0, x5, x10, x15);
        QR_AVX2(x1, x6, x11, x12);
        QR_AVX2(x2, x7, x8, x13);
        QR_AVX2(x3, x4, x9, x14);
    }

    /* Add the original state back. */
    x0 = _mm256_add_epi32(x0, s0);
    x1 = _mm256_add_epi32(x1, s1);
    x2 = _mm256_add_epi32(x2, s2);
    x3 = _mm256_add_epi32(x3, s3);
    x4 = _mm256_add_epi32(x4, s4);
    x5 = _mm256_add_epi32(x5, s5);
    x6 = _mm256_add_epi32(x6, s6);
    x7 = _mm256_add_epi32(x7, s7);
    x8 = _mm256_add_epi32(x8, s8);
    x9 = _mm256_add_epi32(x9, s9);
    x10 = _mm256_add_epi32(x10, s10);
    x11 = _mm256_add_epi32(x11, s11);
    x12 = _mm256_add_epi32(x12, s12);
    x13 = _mm256_add_epi32(x13, s13);
    x14 = _mm256_add_epi32(x14, s14);
    x15 = _mm256_add_epi32(x15, s15);

    /* De-transpose: block i's word j lives in lane i of x[j].  Spill
     * each vector to a 16x8 staging area aligned for AVX2 stores,
     * then pack out per block. */
    uint32_t staging[16][8] __attribute__((aligned(32)));
    _mm256_store_si256((__m256i *)staging[0], x0);
    _mm256_store_si256((__m256i *)staging[1], x1);
    _mm256_store_si256((__m256i *)staging[2], x2);
    _mm256_store_si256((__m256i *)staging[3], x3);
    _mm256_store_si256((__m256i *)staging[4], x4);
    _mm256_store_si256((__m256i *)staging[5], x5);
    _mm256_store_si256((__m256i *)staging[6], x6);
    _mm256_store_si256((__m256i *)staging[7], x7);
    _mm256_store_si256((__m256i *)staging[8], x8);
    _mm256_store_si256((__m256i *)staging[9], x9);
    _mm256_store_si256((__m256i *)staging[10], x10);
    _mm256_store_si256((__m256i *)staging[11], x11);
    _mm256_store_si256((__m256i *)staging[12], x12);
    _mm256_store_si256((__m256i *)staging[13], x13);
    _mm256_store_si256((__m256i *)staging[14], x14);
    _mm256_store_si256((__m256i *)staging[15], x15);

    for (uint32_t b = 0; b < 8; b++) {
        uint8_t *o = out + b * 64u;
        for (uint32_t w = 0; w < 16; w++) {
            uint32_t v = staging[w][b];
            o[w * 4 + 0] = (uint8_t)(v);
            o[w * 4 + 1] = (uint8_t)(v >> 8);
            o[w * 4 + 2] = (uint8_t)(v >> 16);
            o[w * 4 + 3] = (uint8_t)(v >> 24);
        }
    }

    /* staging[][] holds the full ChaCha20 keystream for 8 blocks (the
     * post-rounds-plus-original-state values); XORing this under the
     * same (key, nonce) pair recovers plaintext, so wipe before the
     * stack frame is released. */
    dkSecureZero(staging, sizeof(staging));
}

#else /* !x86_64 */

bool chacha20AVX2Available(void) {
    return false;
}

void chacha20AVX2Block8(const uint32_t state[16], uint8_t out[512]) {
    (void)state;
    if (out != NULL) {
        memset(out, 0, 512);
    }
    assert(!"chacha20AVX2Block8 invoked on non-x86_64 build — caller "
            "must check chacha20AVX2Available() first");
}

#endif /* CHACHA20_AVX2_BUILD */
