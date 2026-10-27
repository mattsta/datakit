/* BLAKE3 compression — AVX2 implementation for x86_64.
 *
 * Layout of the file:
 *   - feature detection (blake3AVX2Available)
 *   - single-block compression (4x32 state held in __m128i lanes)
 *   - 8-way batch compression (16x __m256i, transposed across 8 lanes)
 *   - DATAKIT_TEST: differential check vs. the portable scalar core
 *     plus a small 1 MiB perf bench
 *
 * Why two layouts?
 *
 *   Single-block:  one input, 16-word state. The natural SIMD layout
 *   is the same as SSE4.1 — pack the state into 4 __m128i registers
 *   (rows 0..3, 4..7, 8..11, 12..15) and operate vertically. AVX2's
 *   256-bit lanes don't help much here because each operand is only
 *   16 bytes wide. We still gate on AVX2 so we can use VEX-encoded
 *   forms (no SSE/AVX transition penalty in callers that mix code).
 *
 *   8-way (Compress8): eight independent inputs hashed in parallel.
 *   We transpose so that vector i holds state[i] across all 8 chunks
 *   (lane j of the vector = chunk j's state word i). Every BLAKE3 G
 *   then operates on eight chunks at once — that's the real AVX2 win.
 *
 * The 8-way path is the standard "horizontal SIMD" pattern used by
 * the official BLAKE3 reference SIMD implementations. */

#include "blake3AVX2.h"

#include "../dkEndian.h" /* dkStoreLe32 */

#include <stdint.h>
#include <string.h>

/* The IV: the x86 kernels read it, and so does the test driver on every
 * host. */
#if defined(__x86_64__) || defined(DATAKIT_TEST)
static constexpr uint32_t B3_IV[8] = {
    0x6A09E667ul, 0xBB67AE85ul, 0x3C6EF372ul, 0xA54FF53Aul,
    0x510E527Ful, 0x9B05688Cul, 0x1F83D9ABul, 0x5BE0CD19ul,
};
#endif

#if defined(__x86_64__)

#include <immintrin.h>

static constexpr uint8_t B3_MSG_SCHEDULE[7][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8},
    {3, 4, 10, 12, 13, 2, 7, 14, 6, 5, 9, 0, 11, 15, 8, 1},
    {10, 7, 12, 9, 14, 3, 13, 15, 4, 0, 11, 2, 5, 8, 1, 6},
    {12, 13, 9, 11, 15, 10, 14, 8, 7, 2, 5, 3, 0, 1, 6, 4},
    {9, 14, 11, 5, 8, 12, 15, 1, 13, 3, 0, 10, 2, 6, 4, 7},
    {11, 15, 5, 0, 1, 9, 8, 6, 14, 10, 2, 12, 3, 4, 7, 13},
};

bool blake3AVX2Available(void) {
    /* __builtin_cpu_init is implicit on recent GCC/Clang for cpu_supports,
     * but calling it explicitly is harmless and avoids surprises with
     * older toolchains. */
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") != 0;
}

/* ====================================================================== */
/* Single-block path (4 x __m128i state)                                  */
/* ====================================================================== */

/* Rotates each 32-bit lane right by n. AVX2 has no rotate; we synthesize
 * it. For n=16/8 we can also use _mm_shuffle_epi8 with a byte-mask, but
 * the shift-or form is short and compiles to two ops either way. */
[[gnu::target("avx2"), gnu::always_inline]] static inline __m128i
rotr_epi32_128(__m128i x, int n) {
    return _mm_or_si128(_mm_srli_epi32(x, n), _mm_slli_epi32(x, 32 - n));
}

/* Byte-shuffle masks for the two power-of-byte rotations (rotr16/rotr8). */
[[gnu::target("avx2"), gnu::always_inline]] static inline __m128i
rotr16_128(__m128i x) {
    const __m128i mask =
        _mm_set_epi8(13, 12, 15, 14, 9, 8, 11, 10, 5, 4, 7, 6, 1, 0, 3, 2);
    return _mm_shuffle_epi8(x, mask);
}

[[gnu::target("avx2"), gnu::always_inline]] static inline __m128i
rotr8_128(__m128i x) {
    const __m128i mask =
        _mm_set_epi8(12, 15, 14, 13, 8, 11, 10, 9, 4, 7, 6, 5, 0, 3, 2, 1);
    return _mm_shuffle_epi8(x, mask);
}

/* One G-quarterround vertically over the state rows. m0/m1 are message
 * words selected by the caller for this quarterround. */
[[gnu::target("avx2"), gnu::always_inline]] static inline void
g_round_128(__m128i *row_a, __m128i *row_b, __m128i *row_c, __m128i *row_d,
            __m128i m0, __m128i m1) {
    *row_a = _mm_add_epi32(_mm_add_epi32(*row_a, *row_b), m0);
    *row_d = rotr16_128(_mm_xor_si128(*row_d, *row_a));
    *row_c = _mm_add_epi32(*row_c, *row_d);
    *row_b = rotr_epi32_128(_mm_xor_si128(*row_b, *row_c), 12);
    *row_a = _mm_add_epi32(_mm_add_epi32(*row_a, *row_b), m1);
    *row_d = rotr8_128(_mm_xor_si128(*row_d, *row_a));
    *row_c = _mm_add_epi32(*row_c, *row_d);
    *row_b = rotr_epi32_128(_mm_xor_si128(*row_b, *row_c), 7);
}

/* Diagonalize / un-diagonalize between column and diagonal G groups.
 * The state is held as four rows of {0..3,4..7,8..11,12..15}; to make
 * the diagonal G groups vertical, we cyclically rotate rows b/c/d. */
[[gnu::target("avx2"), gnu::always_inline]] static inline void
diagonalize(__m128i *row_b, __m128i *row_c, __m128i *row_d) {
    *row_b = _mm_shuffle_epi32(*row_b, _MM_SHUFFLE(0, 3, 2, 1));
    *row_c = _mm_shuffle_epi32(*row_c, _MM_SHUFFLE(1, 0, 3, 2));
    *row_d = _mm_shuffle_epi32(*row_d, _MM_SHUFFLE(2, 1, 0, 3));
}

[[gnu::target("avx2"), gnu::always_inline]] static inline void
undiagonalize(__m128i *row_b, __m128i *row_c, __m128i *row_d) {
    *row_b = _mm_shuffle_epi32(*row_b, _MM_SHUFFLE(2, 1, 0, 3));
    *row_c = _mm_shuffle_epi32(*row_c, _MM_SHUFFLE(1, 0, 3, 2));
    *row_d = _mm_shuffle_epi32(*row_d, _MM_SHUFFLE(0, 3, 2, 1));
}

/* Build the four message-word vectors {m[s[0,2,4,6]], m[s[1,3,5,7]],
 * m[s[8,10,12,14]], m[s[9,11,13,15]]} for one round. Used by both the
 * column step and the diagonal step. */
[[gnu::target("avx2"), gnu::always_inline]] static inline void
load_msg_vectors_128(const uint32_t m[16], const uint8_t s[16], __m128i *col0,
                     __m128i *col1, __m128i *diag0, __m128i *diag1) {
    *col0 =
        _mm_set_epi32((int)m[s[6]], (int)m[s[4]], (int)m[s[2]], (int)m[s[0]]);
    *col1 =
        _mm_set_epi32((int)m[s[7]], (int)m[s[5]], (int)m[s[3]], (int)m[s[1]]);
    *diag0 = _mm_set_epi32((int)m[s[14]], (int)m[s[12]], (int)m[s[10]],
                           (int)m[s[8]]);
    *diag1 = _mm_set_epi32((int)m[s[15]], (int)m[s[13]], (int)m[s[11]],
                           (int)m[s[9]]);
}

/* Core single-block compressor — writes the full 16-word state. */
[[gnu::target("avx2")]] static void
compress_pre_avx2(__m128i rows[4], const uint32_t cv[8],
                  const uint8_t block[BLAKE3_BLOCK_LEN], uint8_t block_len,
                  uint64_t counter, uint8_t flags) {
    uint32_t m[16];
    memcpy(m, block, BLAKE3_BLOCK_LEN); /* BLAKE3 is little-endian */

    rows[0] = _mm_loadu_si128((const __m128i *)&cv[0]);
    rows[1] = _mm_loadu_si128((const __m128i *)&cv[4]);
    rows[2] = _mm_set_epi32((int)B3_IV[3], (int)B3_IV[2], (int)B3_IV[1],
                            (int)B3_IV[0]);
    rows[3] = _mm_set_epi32((int)flags, (int)block_len, (int)(counter >> 32),
                            (int)(counter & 0xFFFFFFFFul));

    for (uint32_t r = 0; r < 7; r++) {
        __m128i col0, col1, diag0, diag1;
        load_msg_vectors_128(m, B3_MSG_SCHEDULE[r], &col0, &col1, &diag0,
                             &diag1);
        g_round_128(&rows[0], &rows[1], &rows[2], &rows[3], col0, col1);
        diagonalize(&rows[1], &rows[2], &rows[3]);
        g_round_128(&rows[0], &rows[1], &rows[2], &rows[3], diag0, diag1);
        undiagonalize(&rows[1], &rows[2], &rows[3]);
    }
}

[[gnu::target("avx2")]] void
blake3AVX2CompressInPlace(uint32_t cv[8], const uint8_t block[BLAKE3_BLOCK_LEN],
                          uint8_t block_len, uint64_t counter, uint8_t flags) {
    __m128i rows[4];
    compress_pre_avx2(rows, cv, block, block_len, counter, flags);
    /* CV out = state[i] ^ state[i+8]. The lower 8 words live in
     * rows[0..1], the upper 8 in rows[2..3]. */
    __m128i out0 = _mm_xor_si128(rows[0], rows[2]);
    __m128i out1 = _mm_xor_si128(rows[1], rows[3]);
    _mm_storeu_si128((__m128i *)&cv[0], out0);
    _mm_storeu_si128((__m128i *)&cv[4], out1);
}

[[gnu::target("avx2")]] void
blake3AVX2CompressXof(const uint32_t cv[8],
                      const uint8_t block[BLAKE3_BLOCK_LEN], uint8_t block_len,
                      uint64_t counter, uint8_t flags, uint8_t out[64]) {
    __m128i rows[4];
    compress_pre_avx2(rows, cv, block, block_len, counter, flags);
    __m128i cv_lo = _mm_loadu_si128((const __m128i *)&cv[0]);
    __m128i cv_hi = _mm_loadu_si128((const __m128i *)&cv[4]);
    /* Lower 8 words: state[i] ^ state[i+8]. */
    _mm_storeu_si128((__m128i *)(out + 0), _mm_xor_si128(rows[0], rows[2]));
    _mm_storeu_si128((__m128i *)(out + 16), _mm_xor_si128(rows[1], rows[3]));
    /* Upper 8 words: state[i+8] ^ cv[i] (yes, original cv, not folded). */
    _mm_storeu_si128((__m128i *)(out + 32), _mm_xor_si128(rows[2], cv_lo));
    _mm_storeu_si128((__m128i *)(out + 48), _mm_xor_si128(rows[3], cv_hi));
}

/* ====================================================================== */
/* 8-way batch path (16 x __m256i, transposed)                            */
/* ====================================================================== */

/* In the 8-way layout, vector v_i holds state word i across 8 chunks:
 *   lane j of v_i  ==  chunk_j.state[i]
 * Likewise vector mv_i holds message word i across 8 chunks. Every G
 * operation is therefore lane-parallel across 8 chunks at once. */

[[gnu::target("avx2"), gnu::always_inline]] static inline __m256i
rotr_epi32_256(__m256i x, int n) {
    return _mm256_or_si256(_mm256_srli_epi32(x, n),
                           _mm256_slli_epi32(x, 32 - n));
}

[[gnu::target("avx2"), gnu::always_inline]] static inline __m256i
rotr16_256(__m256i x) {
    const __m256i mask =
        _mm256_set_epi8(13, 12, 15, 14, 9, 8, 11, 10, 5, 4, 7, 6, 1, 0, 3, 2,
                        13, 12, 15, 14, 9, 8, 11, 10, 5, 4, 7, 6, 1, 0, 3, 2);
    return _mm256_shuffle_epi8(x, mask);
}

[[gnu::target("avx2"), gnu::always_inline]] static inline __m256i
rotr8_256(__m256i x) {
    const __m256i mask =
        _mm256_set_epi8(12, 15, 14, 13, 8, 11, 10, 9, 4, 7, 6, 5, 0, 3, 2, 1,
                        12, 15, 14, 13, 8, 11, 10, 9, 4, 7, 6, 5, 0, 3, 2, 1);
    return _mm256_shuffle_epi8(x, mask);
}

/* The 8-way G. Operates on the 4 state vectors (a, b, c, d) — each
 * vector holds the same state word across all 8 chunks — and two
 * message vectors. This is the same G as scalar, replicated 8x. */
[[gnu::target("avx2"), gnu::always_inline]] static inline void
g_round_256(__m256i *a, __m256i *b, __m256i *c, __m256i *d, __m256i mx,
            __m256i my) {
    *a = _mm256_add_epi32(_mm256_add_epi32(*a, *b), mx);
    *d = rotr16_256(_mm256_xor_si256(*d, *a));
    *c = _mm256_add_epi32(*c, *d);
    *b = rotr_epi32_256(_mm256_xor_si256(*b, *c), 12);
    *a = _mm256_add_epi32(_mm256_add_epi32(*a, *b), my);
    *d = rotr8_256(_mm256_xor_si256(*d, *a));
    *c = _mm256_add_epi32(*c, *d);
    *b = rotr_epi32_256(_mm256_xor_si256(*b, *c), 7);
}

/* Compute one full round over the transposed 16-vector state v[0..15]. */
[[gnu::target("avx2"), gnu::always_inline]] static inline void
round_256(__m256i v[16], const __m256i m[16], const uint8_t s[16]) {
    /* Column step. */
    g_round_256(&v[0], &v[4], &v[8], &v[12], m[s[0]], m[s[1]]);
    g_round_256(&v[1], &v[5], &v[9], &v[13], m[s[2]], m[s[3]]);
    g_round_256(&v[2], &v[6], &v[10], &v[14], m[s[4]], m[s[5]]);
    g_round_256(&v[3], &v[7], &v[11], &v[15], m[s[6]], m[s[7]]);
    /* Diagonal step. */
    g_round_256(&v[0], &v[5], &v[10], &v[15], m[s[8]], m[s[9]]);
    g_round_256(&v[1], &v[6], &v[11], &v[12], m[s[10]], m[s[11]]);
    g_round_256(&v[2], &v[7], &v[8], &v[13], m[s[12]], m[s[13]]);
    g_round_256(&v[3], &v[4], &v[9], &v[14], m[s[14]], m[s[15]]);
}

[[gnu::target("avx2")]] void
blake3AVX2Compress8(const uint32_t *const cv_inputs[8],
                    const uint8_t *const blocks[8], const uint8_t block_lens[8],
                    const uint64_t counters[8], const uint8_t flag_set[8],
                    uint8_t out_cvs[8][32]);

[[gnu::target("avx2")]] void
blake3AVX2Compress8(const uint32_t *const cv_inputs[8],
                    const uint8_t *const blocks[8], const uint8_t block_lens[8],
                    const uint64_t counters[8], const uint8_t flag_set[8],
                    uint8_t out_cvs[8][32]) {
    /* Build the 16 transposed message vectors via row-major load +
     * 8x8 transpose.  mv[i] ends up holding message word i across 8
     * chunks.
     *
     * The previous build used 16 _mm256_set_epi32 calls — each one is
     * effectively a gather (8 individual 32-bit loads packed into a
     * register), which becomes the dominant cost of Compress8 for
     * cache-resident inputs.  The transpose approach loads each
     * block's 16 words with two _mm256_loadu_si256 (16 unaligned
     * 256-bit loads total) and then permutes in register.  Two 8x8
     * transposes (one for words 0..7, one for words 8..15) replace
     * the 16 gathers.
     *
     * Reference: this is the standard AVX2 register transpose used by
     * the official BLAKE3 reference SIMD implementations. */
    __m256i mv[16];
    {
        /* Load each block's 16 words as two 256-bit halves. */
        __m256i r0lo = _mm256_loadu_si256((const __m256i *)(blocks[0] + 0));
        __m256i r0hi = _mm256_loadu_si256((const __m256i *)(blocks[0] + 32));
        __m256i r1lo = _mm256_loadu_si256((const __m256i *)(blocks[1] + 0));
        __m256i r1hi = _mm256_loadu_si256((const __m256i *)(blocks[1] + 32));
        __m256i r2lo = _mm256_loadu_si256((const __m256i *)(blocks[2] + 0));
        __m256i r2hi = _mm256_loadu_si256((const __m256i *)(blocks[2] + 32));
        __m256i r3lo = _mm256_loadu_si256((const __m256i *)(blocks[3] + 0));
        __m256i r3hi = _mm256_loadu_si256((const __m256i *)(blocks[3] + 32));
        __m256i r4lo = _mm256_loadu_si256((const __m256i *)(blocks[4] + 0));
        __m256i r4hi = _mm256_loadu_si256((const __m256i *)(blocks[4] + 32));
        __m256i r5lo = _mm256_loadu_si256((const __m256i *)(blocks[5] + 0));
        __m256i r5hi = _mm256_loadu_si256((const __m256i *)(blocks[5] + 32));
        __m256i r6lo = _mm256_loadu_si256((const __m256i *)(blocks[6] + 0));
        __m256i r6hi = _mm256_loadu_si256((const __m256i *)(blocks[6] + 32));
        __m256i r7lo = _mm256_loadu_si256((const __m256i *)(blocks[7] + 0));
        __m256i r7hi = _mm256_loadu_si256((const __m256i *)(blocks[7] + 32));

/* Macro factoring the 8x8 32-bit transpose: takes 8 input
 * vectors (rows), writes 8 output vectors (columns) into the
 * mv[] slots starting at OFFSET. */
#define B3_TRANSPOSE_8x8(r0, r1, r2, r3, r4, r5, r6, r7, OFFSET)               \
    do {                                                                       \
        __m256i t0 = _mm256_unpacklo_epi32(r0, r1);                            \
        __m256i t1 = _mm256_unpackhi_epi32(r0, r1);                            \
        __m256i t2 = _mm256_unpacklo_epi32(r2, r3);                            \
        __m256i t3 = _mm256_unpackhi_epi32(r2, r3);                            \
        __m256i t4 = _mm256_unpacklo_epi32(r4, r5);                            \
        __m256i t5 = _mm256_unpackhi_epi32(r4, r5);                            \
        __m256i t6 = _mm256_unpacklo_epi32(r6, r7);                            \
        __m256i t7 = _mm256_unpackhi_epi32(r6, r7);                            \
        __m256i u0 = _mm256_unpacklo_epi64(t0, t2);                            \
        __m256i u1 = _mm256_unpackhi_epi64(t0, t2);                            \
        __m256i u2 = _mm256_unpacklo_epi64(t1, t3);                            \
        __m256i u3 = _mm256_unpackhi_epi64(t1, t3);                            \
        __m256i u4 = _mm256_unpacklo_epi64(t4, t6);                            \
        __m256i u5 = _mm256_unpackhi_epi64(t4, t6);                            \
        __m256i u6 = _mm256_unpacklo_epi64(t5, t7);                            \
        __m256i u7 = _mm256_unpackhi_epi64(t5, t7);                            \
        mv[(OFFSET) + 0] = _mm256_permute2x128_si256(u0, u4, 0x20);            \
        mv[(OFFSET) + 1] = _mm256_permute2x128_si256(u1, u5, 0x20);            \
        mv[(OFFSET) + 2] = _mm256_permute2x128_si256(u2, u6, 0x20);            \
        mv[(OFFSET) + 3] = _mm256_permute2x128_si256(u3, u7, 0x20);            \
        mv[(OFFSET) + 4] = _mm256_permute2x128_si256(u0, u4, 0x31);            \
        mv[(OFFSET) + 5] = _mm256_permute2x128_si256(u1, u5, 0x31);            \
        mv[(OFFSET) + 6] = _mm256_permute2x128_si256(u2, u6, 0x31);            \
        mv[(OFFSET) + 7] = _mm256_permute2x128_si256(u3, u7, 0x31);            \
    } while (0)

        B3_TRANSPOSE_8x8(r0lo, r1lo, r2lo, r3lo, r4lo, r5lo, r6lo, r7lo, 0);
        B3_TRANSPOSE_8x8(r0hi, r1hi, r2hi, r3hi, r4hi, r5hi, r6hi, r7hi, 8);

#undef B3_TRANSPOSE_8x8
    }

    /* Build the 16 transposed state vectors. v[0..7] = cv lanes,
     * v[8..11] = IV broadcast, v[12..15] = counter lo/hi, block_len, flags. */
    __m256i v[16];
    for (int i = 0; i < 8; i++) {
        v[i] = _mm256_set_epi32((int)cv_inputs[7][i], (int)cv_inputs[6][i],
                                (int)cv_inputs[5][i], (int)cv_inputs[4][i],
                                (int)cv_inputs[3][i], (int)cv_inputs[2][i],
                                (int)cv_inputs[1][i], (int)cv_inputs[0][i]);
    }
    v[8] = _mm256_set1_epi32((int)B3_IV[0]);
    v[9] = _mm256_set1_epi32((int)B3_IV[1]);
    v[10] = _mm256_set1_epi32((int)B3_IV[2]);
    v[11] = _mm256_set1_epi32((int)B3_IV[3]);

    v[12] = _mm256_set_epi32(
        (int)(counters[7] & 0xFFFFFFFFul), (int)(counters[6] & 0xFFFFFFFFul),
        (int)(counters[5] & 0xFFFFFFFFul), (int)(counters[4] & 0xFFFFFFFFul),
        (int)(counters[3] & 0xFFFFFFFFul), (int)(counters[2] & 0xFFFFFFFFul),
        (int)(counters[1] & 0xFFFFFFFFul), (int)(counters[0] & 0xFFFFFFFFul));
    v[13] =
        _mm256_set_epi32((int)(counters[7] >> 32), (int)(counters[6] >> 32),
                         (int)(counters[5] >> 32), (int)(counters[4] >> 32),
                         (int)(counters[3] >> 32), (int)(counters[2] >> 32),
                         (int)(counters[1] >> 32), (int)(counters[0] >> 32));
    v[14] = _mm256_set_epi32((int)block_lens[7], (int)block_lens[6],
                             (int)block_lens[5], (int)block_lens[4],
                             (int)block_lens[3], (int)block_lens[2],
                             (int)block_lens[1], (int)block_lens[0]);
    v[15] = _mm256_set_epi32(
        (int)flag_set[7], (int)flag_set[6], (int)flag_set[5], (int)flag_set[4],
        (int)flag_set[3], (int)flag_set[2], (int)flag_set[1], (int)flag_set[0]);

    /* 7 rounds. */
    for (int r = 0; r < 7; r++) {
        round_256(v, mv, B3_MSG_SCHEDULE[r]);
    }

    /* CV out = state[i] ^ state[i+8] for i in 0..7. v[0..7] each holds
     * the corresponding output word across 8 chunks; we de-transpose by
     * extracting lanes back out per chunk. */
    __m256i out_words[8];
    for (int i = 0; i < 8; i++) {
        out_words[i] = _mm256_xor_si256(v[i], v[i + 8]);
    }
    /* Spill to a staging array and pack per-chunk. The transpose cost
     * is amortized over 8 chunks worth of work; a true 8x8 in-register
     * transpose with unpacks/permutes would be marginally faster. */
    alignas(32) uint32_t staging[8][8];
    for (int i = 0; i < 8; i++) {
        _mm256_store_si256((__m256i *)staging[i], out_words[i]);
    }
    for (int j = 0; j < 8; j++) {
        for (int i = 0; i < 8; i++) {
            dkStoreLe32(out_cvs[j] + i * 4, staging[i][j]);
        }
    }
}

#else /* !x86_64 */

bool blake3AVX2Available(void) {
    return false;
}

/* On non-x86 we still provide working entry points so callers don't
 * have to branch on the platform — they branch on Available(). The
 * implementations defer to the portable scalar. */
void blake3AVX2CompressInPlace(uint32_t cv[8],
                               const uint8_t block[BLAKE3_BLOCK_LEN],
                               uint8_t block_len, uint64_t counter,
                               uint8_t flags) {
    blake3PortableCompressInPlace(cv, block, block_len, counter, flags);
}

void blake3AVX2CompressXof(const uint32_t cv[8],
                           const uint8_t block[BLAKE3_BLOCK_LEN],
                           uint8_t block_len, uint64_t counter, uint8_t flags,
                           uint8_t out[64]) {
    blake3PortableCompressXof(cv, block, block_len, counter, flags, out);
}

void blake3AVX2Compress8(const uint32_t *const cv_inputs[8],
                         const uint8_t *const blocks[8],
                         const uint8_t block_lens[8],
                         const uint64_t counters[8], const uint8_t flag_set[8],
                         uint8_t out_cvs[8][32]) {
    blake3PortableCompressBatch(8, cv_inputs, blocks, block_lens, counters,
                                flag_set, out_cvs);
}

#endif /* x86_64 */

/* ====================================================================== */
/* Test driver                                                            */
/* ====================================================================== */

#ifdef DATAKIT_TEST

#include "cleaner.h"
#include "ctest.h"
#include "datakit.h" /* zmalloc/zfree */
#include "timeUtil.h"

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleanerName(uint8_t *, zfree, U8);

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

/* Deterministic xorshift64 — keeps the test reproducible across runs
 * without dragging libc rand state into it. */
static uint64_t xs_state = 0x9E3779B97F4A7C15ull;
static uint64_t xs64(void) {
    return dkXorshift64Next(&xs_state);
}

static void fill_random(uint8_t *p, size_t n) {
    size_t i = 0;
    while (i + 8 <= n) {
        uint64_t r = xs64();
        memcpy(p + i, &r, 8);
        i += 8;
    }
    while (i < n) {
        p[i++] = (uint8_t)(xs64() & 0xff);
    }
}

int blake3AVX2Test(int, char *[]) {
    int err = 0;

    /* Stub-delegation guard: a direct call to blake3AVX2CompressInPlace
     * must produce the portable digest on every platform, including
     * those where AVX2 is unavailable and the symbol is a stub.  Catch
     * the silent-no-op regression class before doing anything else. */
    TEST("stub delegation: avx2-compress matches portable on direct call") {
        uint32_t cv[8];
        uint8_t block[BLAKE3_BLOCK_LEN];
        for (uint32_t i = 0; i < 8u; i++) {
            cv[i] = (uint32_t)(0xC0FFEE00u + i);
        }
        for (uint32_t i = 0; i < BLAKE3_BLOCK_LEN; i++) {
            block[i] = (uint8_t)(i * 7u + 1u);
        }
        uint32_t cv_ref[8], cv_simd[8];
        memcpy(cv_ref, cv, sizeof(cv));
        memcpy(cv_simd, cv, sizeof(cv));
        blake3PortableCompressInPlace(cv_ref, block, BLAKE3_BLOCK_LEN,
                                      0xDEADBEEFCAFEull, 0x0Bu);
        blake3AVX2CompressInPlace(cv_simd, block, BLAKE3_BLOCK_LEN,
                                  0xDEADBEEFCAFEull, 0x0Bu);
        TEST_BYTES(err, cv_ref, cv_simd, sizeof(cv_ref),
                   "Stub/SIMD compress-in-place output != portable");
    }

    if (!blake3AVX2Available()) {
        printf("blake3AVX2: AVX2 not available on this CPU — remaining suite "
               "skipped\n");
        TEST_FINAL_RESULT;
    }

    /* 1. Single-block parity vs. portable on a fixed input. */
    TEST("blake3AVX2 single-block compress matches portable") {
        uint32_t cv_p[8], cv_a[8];
        uint8_t block[BLAKE3_BLOCK_LEN];
        for (int i = 0; i < 8; i++) {
            cv_p[i] = cv_a[i] = (uint32_t)(0xC0FFEE00u + i);
        }
        for (int i = 0; i < BLAKE3_BLOCK_LEN; i++) {
            block[i] = (uint8_t)(i * 7 + 1);
        }
        blake3PortableCompressInPlace(cv_p, block, BLAKE3_BLOCK_LEN,
                                      0xDEADBEEFCAFEull, 0x0B);
        blake3AVX2CompressInPlace(cv_a, block, BLAKE3_BLOCK_LEN,
                                  0xDEADBEEFCAFEull, 0x0B);
        TEST_BYTES(err, cv_p, cv_a, sizeof(cv_p),
                   "AVX2 CompressInPlace mismatch on fixed input");

        uint8_t xof_p[64], xof_a[64];
        uint32_t cv0[8];
        for (int i = 0; i < 8; i++) {
            cv0[i] = (uint32_t)(0x12345670u + i);
        }
        blake3PortableCompressXof(cv0, block, BLAKE3_BLOCK_LEN, 0x1ull, 0x09,
                                  xof_p);
        blake3AVX2CompressXof(cv0, block, BLAKE3_BLOCK_LEN, 0x1ull, 0x09,
                              xof_a);
        TEST_BYTES(err, xof_p, xof_a, 64,
                   "AVX2 CompressXof mismatch on fixed input");
    }

    /* 2. 100K random inputs differential test. */
    TEST("blake3AVX2 single-block matches portable across 100K random inputs") {
        uint32_t cv_p[8], cv_a[8];
        uint8_t block[BLAKE3_BLOCK_LEN];
        int mismatches = 0;
        for (int iter = 0; iter < 100'000; iter++) {
            for (int i = 0; i < 8; i++) {
                cv_p[i] = cv_a[i] = (uint32_t)xs64();
            }
            fill_random(block, BLAKE3_BLOCK_LEN);
            uint64_t counter = xs64();
            uint8_t block_len = (uint8_t)(1 + (xs64() % BLAKE3_BLOCK_LEN));
            uint8_t flags = (uint8_t)(xs64() & 0x7F);
            blake3PortableCompressInPlace(cv_p, block, block_len, counter,
                                          flags);
            blake3AVX2CompressInPlace(cv_a, block, block_len, counter, flags);
            if (memcmp(cv_p, cv_a, sizeof(cv_p)) != 0) {
                if (mismatches++ < 3) {
                    printf("mismatch at iter=%d counter=%" PRIu64
                           " flags=0x%02x len=%u\n",
                           iter, counter, flags, block_len);
                }
            }
        }
        if (mismatches != 0) {
            ERR("AVX2 vs portable mismatched %d/100000 random inputs",
                mismatches);
        }
    }

    /* 3. Compress8 vs 8 single-blocks. */
    TEST("blake3AVX2 Compress8 matches 8 single-blocks") {
        uint32_t cvs[8][8];
        uint8_t blks[8][BLAKE3_BLOCK_LEN];
        uint8_t block_lens[8];
        uint64_t counters[8];
        uint8_t flag_set[8];

        const uint32_t *cv_ptrs[8];
        const uint8_t *blk_ptrs[8];
        uint8_t out_batch[8][32];

        int mismatches = 0;
        for (int iter = 0; iter < 1000; iter++) {
            for (int j = 0; j < 8; j++) {
                for (int i = 0; i < 8; i++) {
                    cvs[j][i] = (uint32_t)xs64();
                }
                fill_random(blks[j], BLAKE3_BLOCK_LEN);
                block_lens[j] = (uint8_t)(1 + (xs64() % BLAKE3_BLOCK_LEN));
                counters[j] = xs64();
                flag_set[j] = (uint8_t)(xs64() & 0x7F);
                cv_ptrs[j] = cvs[j];
                blk_ptrs[j] = blks[j];
            }
            blake3AVX2Compress8(cv_ptrs, blk_ptrs, block_lens, counters,
                                flag_set, out_batch);
            for (int j = 0; j < 8; j++) {
                uint32_t cv_ref[8];
                memcpy(cv_ref, cvs[j], sizeof(cv_ref));
                blake3PortableCompressInPlace(cv_ref, blks[j], block_lens[j],
                                              counters[j], flag_set[j]);
                uint8_t expected[32];
                for (int i = 0; i < 8; i++) {
                    dkStoreLe32(expected + i * 4, cv_ref[i]);
                }
                if (memcmp(expected, out_batch[j], 32) != 0) {
                    if (mismatches++ < 3) {
                        printf("Compress8 mismatch at iter=%d lane=%d\n", iter,
                               j);
                    }
                }
            }
        }
        if (mismatches != 0) {
            ERR("Compress8 vs scalar mismatched %d outputs", mismatches);
        }
    }

    /* 4. Perf bench: 1 MiB worth of block compressions. */
    TEST("blake3AVX2 perf bench") {
        const size_t MB = 1024 * 1024;
        const size_t n_blocks = MB / BLAKE3_BLOCK_LEN; /* 16384 */
        cleanerName(zfree, U8) uint8_t *buf = zmalloc(MB);
        fill_random(buf, MB);
        uint32_t cv[8];
        for (int i = 0; i < 8; i++) {
            cv[i] = B3_IV[i];
        }

        /* Portable. */
        double t0 = timeUtilMonotonicSec();
        uint32_t cv_p[8];
        memcpy(cv_p, cv, sizeof(cv));
        for (size_t b = 0; b < n_blocks; b++) {
            blake3PortableCompressInPlace(cv_p, buf + b * BLAKE3_BLOCK_LEN,
                                          BLAKE3_BLOCK_LEN, b, 0);
        }
        double t1 = timeUtilMonotonicSec();
        double mib_per_s_p = 1.0 / (t1 - t0);

        /* AVX2 single-block. */
        uint32_t cv_a[8];
        memcpy(cv_a, cv, sizeof(cv));
        t0 = timeUtilMonotonicSec();
        for (size_t b = 0; b < n_blocks; b++) {
            blake3AVX2CompressInPlace(cv_a, buf + b * BLAKE3_BLOCK_LEN,
                                      BLAKE3_BLOCK_LEN, b, 0);
        }
        t1 = timeUtilMonotonicSec();
        double mib_per_s_a = 1.0 / (t1 - t0);

        /* AVX2 8-way batch. We hash the same 1 MiB by feeding the
         * blocks in groups of 8 — each lane gets a distinct block, so
         * we cover the same 16384 blocks in n_blocks/8 batch calls. */
        uint32_t batch_cv[8][8];
        for (int j = 0; j < 8; j++) {
            memcpy(batch_cv[j], cv, sizeof(cv));
        }
        const uint32_t *batch_cv_ptrs[8];
        for (int j = 0; j < 8; j++) {
            batch_cv_ptrs[j] = batch_cv[j];
        }
        const uint8_t *batch_blk_ptrs[8];
        uint8_t batch_block_lens[8];
        uint64_t batch_counters[8];
        uint8_t batch_flags[8];
        for (int j = 0; j < 8; j++) {
            batch_block_lens[j] = BLAKE3_BLOCK_LEN;
            batch_flags[j] = 0;
        }
        uint8_t batch_out[8][32];

        t0 = timeUtilMonotonicSec();
        for (size_t b = 0; b < n_blocks; b += 8) {
            for (int j = 0; j < 8; j++) {
                batch_blk_ptrs[j] = buf + (b + j) * BLAKE3_BLOCK_LEN;
                batch_counters[j] = b + j;
            }
            blake3AVX2Compress8(batch_cv_ptrs, batch_blk_ptrs, batch_block_lens,
                                batch_counters, batch_flags, batch_out);
        }
        t1 = timeUtilMonotonicSec();
        double mib_per_s_b = 1.0 / (t1 - t0);

        printf("  blake3 portable:    %.1f MiB/s\n", mib_per_s_p);
        printf("  blake3 AVX2 single: %.1f MiB/s  (%.2fx)\n", mib_per_s_a,
               mib_per_s_a / mib_per_s_p);
        printf("  blake3 AVX2 8-way:  %.1f MiB/s  (%.2fx)\n", mib_per_s_b,
               mib_per_s_b / mib_per_s_p);
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
