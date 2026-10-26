/* BLAKE3 - x86 SSE4.1 accelerated single-block compression.
 *
 * The portable compressor in blake3.c maintains a 16-word state and
 * mixes it via eight calls to G per round, for seven rounds. The four
 * G calls of the column step are independent of one another, as are
 * the four G calls of the diagonal step; that's the dimension we
 * exploit with SSE.
 *
 * Layout: pack the 4x4 state matrix into four __m128i vectors
 *     row0 = (s0,  s1,  s2,  s3)
 *     row1 = (s4,  s5,  s6,  s7)
 *     row2 = (s8,  s9,  s10, s11)
 *     row3 = (s12, s13, s14, s15)
 * The column step then becomes one fused "vector G" applied across the
 * four rows; the diagonal step is the same operation after cyclically
 * rotating row1 by 1, row2 by 2, row3 by 3 lanes (and undoing the
 * rotation afterwards). SSE4.1 isn't strictly required for the math
 * here (SSE2 suffices), but we tag the functions with target("sse4.1")
 * so the compiler can use the broader SSE4.1 ISA freely and so this
 * mirrors typical BLAKE3 vendor SIMD builds.
 *
 * The byte-shuffle rotate optimization (using _mm_shuffle_epi8 from
 * SSSE3 to implement rotr32 by 16 and 8 as a single PSHUFB) is enabled
 * here since SSSE3 is a subset of SSE4.1.
 *
 * On non-x86 builds, the entry points fall back to a weakly-declared
 * portable compressor exported from blake3.c. */

#include "blake3SSE41.h"

#include <string.h>

/* Portable reference compress functions exported by blake3.c — used
 * for differential validation and as the non-SIMD fallback. */
extern void blake3PortableCompressInPlace(uint32_t cv[8],
                                          const uint8_t block[BLAKE3_BLOCK_LEN],
                                          uint8_t block_len, uint64_t counter,
                                          uint8_t flags);
extern void blake3PortableCompressXof(const uint32_t cv[8],
                                      const uint8_t block[BLAKE3_BLOCK_LEN],
                                      uint8_t block_len, uint64_t counter,
                                      uint8_t flags, uint8_t out[64]);

#if defined(__x86_64__)

#include <immintrin.h>

/* BLAKE3 IV (SHA-256 IV: sqrt of first 8 primes). Mirrors blake3.c. */
static constexpr uint32_t SSE41_IV[8] = {
    0x6A09E667ul, 0xBB67AE85ul, 0x3C6EF372ul, 0xA54FF53Aul,
    0x510E527Ful, 0x9B05688Cul, 0x1F83D9ABul, 0x5BE0CD19ul,
};

/* Message-schedule permutation, identical to blake3.c. */
static constexpr uint8_t SSE41_MSG_SCHEDULE[7][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8},
    {3, 4, 10, 12, 13, 2, 7, 14, 6, 5, 9, 0, 11, 15, 8, 1},
    {10, 7, 12, 9, 14, 3, 13, 15, 4, 0, 11, 2, 5, 8, 1, 6},
    {12, 13, 9, 11, 15, 10, 14, 8, 7, 2, 5, 3, 0, 1, 6, 4},
    {9, 14, 11, 5, 8, 12, 15, 1, 13, 3, 0, 10, 2, 6, 4, 7},
    {11, 15, 5, 0, 1, 9, 8, 6, 14, 10, 2, 12, 3, 4, 7, 13},
};

#define SSE41_TARGET __attribute__((target("sse4.1")))

/* --- Rotation helpers (right rotates of 32-bit lanes). ----------------- */

/* Logical SRLI + SLLI fallback rotates (used for 12 and 7). */
SSE41_TARGET static inline __m128i rotr_12(__m128i x) {
    return _mm_or_si128(_mm_srli_epi32(x, 12), _mm_slli_epi32(x, 20));
}
SSE41_TARGET static inline __m128i rotr_7(__m128i x) {
    return _mm_or_si128(_mm_srli_epi32(x, 7), _mm_slli_epi32(x, 25));
}

/* For rotr16 and rotr8 we can use a byte shuffle (PSHUFB), which is a
 * single-cycle op vs. two shifts + OR. This is the main reason we tag
 * for SSE4.1 (SSSE3+) rather than plain SSE2. */
SSE41_TARGET static inline __m128i rotr_16(__m128i x) {
    const __m128i mask =
        _mm_set_epi8(13, 12, 15, 14, 9, 8, 11, 10, 5, 4, 7, 6, 1, 0, 3, 2);
    return _mm_shuffle_epi8(x, mask);
}
SSE41_TARGET static inline __m128i rotr_8(__m128i x) {
    const __m128i mask =
        _mm_set_epi8(12, 15, 14, 13, 8, 11, 10, 9, 4, 7, 6, 5, 0, 3, 2, 1);
    return _mm_shuffle_epi8(x, mask);
}

/* --- Vector G mix ------------------------------------------------------- *
 * Apply the BLAKE3 G function across all four lanes simultaneously. m1
 * and m2 are the per-lane message words for the first/second halves of
 * G. Operates over rows {0, 1, 2, 3} of the packed state. */
SSE41_TARGET static inline void g_vec(__m128i *r0, __m128i *r1, __m128i *r2,
                                      __m128i *r3, __m128i m1, __m128i m2) {
    *r0 = _mm_add_epi32(_mm_add_epi32(*r0, *r1), m1);
    *r3 = rotr_16(_mm_xor_si128(*r3, *r0));
    *r2 = _mm_add_epi32(*r2, *r3);
    *r1 = rotr_12(_mm_xor_si128(*r1, *r2));
    *r0 = _mm_add_epi32(_mm_add_epi32(*r0, *r1), m2);
    *r3 = rotr_8(_mm_xor_si128(*r3, *r0));
    *r2 = _mm_add_epi32(*r2, *r3);
    *r1 = rotr_7(_mm_xor_si128(*r1, *r2));
}

/* --- Diagonal lane rotations ------------------------------------------- *
 *
 * After the column step, the diagonals of the original 4x4 are no
 * longer aligned with our row-major SIMD layout. To run the same G
 * across diagonals, we cyclically rotate row1 by 1 lane, row2 by 2,
 * row3 by 3 — turning each diagonal into a column in the new layout.
 * After running G we undo the rotation.
 *
 * _MM_SHUFFLE(d,c,b,a) packs four 2-bit selectors as
 *   result = (lane[a], lane[b], lane[c], lane[d])
 * remembering Intel's little-end lane convention. */

#define SHUFFLE_LEFT_1 _MM_SHUFFLE(0, 3, 2, 1) /* (a,b,c,d) -> (b,c,d,a) */
#define SHUFFLE_LEFT_2 _MM_SHUFFLE(1, 0, 3, 2) /* (a,b,c,d) -> (c,d,a,b) */
#define SHUFFLE_LEFT_3 _MM_SHUFFLE(2, 1, 0, 3) /* (a,b,c,d) -> (d,a,b,c) */
#define SHUFFLE_RIGHT_1 _MM_SHUFFLE(2, 1, 0, 3)
#define SHUFFLE_RIGHT_2 _MM_SHUFFLE(1, 0, 3, 2)
#define SHUFFLE_RIGHT_3 _MM_SHUFFLE(0, 3, 2, 1)

SSE41_TARGET static inline void diagonalize(__m128i *r1, __m128i *r2,
                                            __m128i *r3) {
    *r1 = _mm_shuffle_epi32(*r1, SHUFFLE_LEFT_1);
    *r2 = _mm_shuffle_epi32(*r2, SHUFFLE_LEFT_2);
    *r3 = _mm_shuffle_epi32(*r3, SHUFFLE_LEFT_3);
}
SSE41_TARGET static inline void undiagonalize(__m128i *r1, __m128i *r2,
                                              __m128i *r3) {
    *r1 = _mm_shuffle_epi32(*r1, SHUFFLE_RIGHT_1);
    *r2 = _mm_shuffle_epi32(*r2, SHUFFLE_RIGHT_2);
    *r3 = _mm_shuffle_epi32(*r3, SHUFFLE_RIGHT_3);
}

/* --- Per-round message packing ----------------------------------------- *
 *
 * For each round we need four pairs (m1, m2) of __m128i — one pair per
 * row, packing the four message words consumed by the four G calls.
 * The schedule tables are 16 indices: schedule[0..7] feeds the column
 * step (m1=even, m2=odd), schedule[8..15] feeds the diagonal step.
 *
 * Building these via _mm_set_epi32 lets the compiler hoist constants
 * where possible. */
SSE41_TARGET static inline __m128i pack_msg(const uint32_t m[16],
                                            const uint8_t s[16], int i0, int i1,
                                            int i2, int i3) {
    return _mm_set_epi32((int)m[s[i3]], (int)m[s[i2]], (int)m[s[i1]],
                         (int)m[s[i0]]);
}

SSE41_TARGET static inline void round_fn_sse(__m128i *r0, __m128i *r1,
                                             __m128i *r2, __m128i *r3,
                                             const uint32_t m[16],
                                             const uint8_t schedule[16]) {
    /* Column step: G across (row0, row1, row2, row3). Lanewise message
     * words for the four columns are schedule[0..7]. */
    __m128i col_m1 = pack_msg(m, schedule, 0, 2, 4, 6);
    __m128i col_m2 = pack_msg(m, schedule, 1, 3, 5, 7);
    g_vec(r0, r1, r2, r3, col_m1, col_m2);

    /* Diagonal step: re-align state so diagonals become columns,
     * then run the same G again with schedule[8..15]. */
    diagonalize(r1, r2, r3);
    __m128i diag_m1 = pack_msg(m, schedule, 8, 10, 12, 14);
    __m128i diag_m2 = pack_msg(m, schedule, 9, 11, 13, 15);
    g_vec(r0, r1, r2, r3, diag_m1, diag_m2);
    undiagonalize(r1, r2, r3);
}

/* --- Compress_pre: 7 rounds; emits 16-word state in 4 __m128i. -------- */
SSE41_TARGET static void compress_pre_sse(__m128i out_rows[4],
                                          const uint32_t cv[8],
                                          const uint8_t block[BLAKE3_BLOCK_LEN],
                                          uint8_t block_len, uint64_t counter,
                                          uint8_t flags) {
    /* Load message words. BLAKE3 is little-endian throughout, so a raw
     * unaligned load gives us 4 message words per __m128i with no
     * byte-swap. */
    __m128i mvec0 = _mm_loadu_si128((const __m128i *)(block + 0));
    __m128i mvec1 = _mm_loadu_si128((const __m128i *)(block + 16));
    __m128i mvec2 = _mm_loadu_si128((const __m128i *)(block + 32));
    __m128i mvec3 = _mm_loadu_si128((const __m128i *)(block + 48));

    /* Materialize as a uint32_t[16] so the schedule indexing is trivial.
     * Storing then reading back is cheap on x86 (store-to-load forward). */
    uint32_t m[16];
    _mm_storeu_si128((__m128i *)(m + 0), mvec0);
    _mm_storeu_si128((__m128i *)(m + 4), mvec1);
    _mm_storeu_si128((__m128i *)(m + 8), mvec2);
    _mm_storeu_si128((__m128i *)(m + 12), mvec3);

    /* Initial state rows. */
    __m128i r0 = _mm_loadu_si128((const __m128i *)(cv + 0));
    __m128i r1 = _mm_loadu_si128((const __m128i *)(cv + 4));
    __m128i r2 = _mm_set_epi32((int)SSE41_IV[3], (int)SSE41_IV[2],
                               (int)SSE41_IV[1], (int)SSE41_IV[0]);
    __m128i r3 = _mm_set_epi32((int)flags, (int)block_len,
                               (int)(uint32_t)(counter >> 32),
                               (int)(uint32_t)(counter & 0xFFFFFFFFul));

    for (uint32_t r = 0; r < 7; r++) {
        round_fn_sse(&r0, &r1, &r2, &r3, m, SSE41_MSG_SCHEDULE[r]);
    }

    out_rows[0] = r0;
    out_rows[1] = r1;
    out_rows[2] = r2;
    out_rows[3] = r3;
}

/* --- Public entry points ----------------------------------------------- */

SSE41_TARGET
void blake3SSE41CompressInPlace(uint32_t cv[8],
                                const uint8_t block[BLAKE3_BLOCK_LEN],
                                uint8_t block_len, uint64_t counter,
                                uint8_t flags) {
    __m128i rows[4];
    compress_pre_sse(rows, cv, block, block_len, counter, flags);
    /* Truncated CV: low 8 words XOR high 8 words. */
    __m128i out_lo = _mm_xor_si128(rows[0], rows[2]);
    __m128i out_hi = _mm_xor_si128(rows[1], rows[3]);
    _mm_storeu_si128((__m128i *)(cv + 0), out_lo);
    _mm_storeu_si128((__m128i *)(cv + 4), out_hi);
}

SSE41_TARGET
void blake3SSE41CompressXof(const uint32_t cv[8],
                            const uint8_t block[BLAKE3_BLOCK_LEN],
                            uint8_t block_len, uint64_t counter, uint8_t flags,
                            uint8_t out[64]) {
    __m128i rows[4];
    compress_pre_sse(rows, cv, block, block_len, counter, flags);
    /* XOF output: low half = state_lo XOR state_hi (the truncated CV);
     * high half = state_hi XOR original cv. The trailing 2 words of the
     * high half cover counter/block_len/flags via cv-padded zeros — but
     * BLAKE3 spec defines them XORed with the original input_cv slots,
     * which for cv[8..15] correspond to IV[0..3] and the block tail;
     * however the reference implementation XORs only the user-provided
     * cv[0..7] into state[8..15] for the upper output half. We match
     * exactly the portable compress_xof in blake3.c, which does:
     *   out_lo[i]   = state[i]     ^ state[i+8]
     *   out_hi[i]   = state[i+8]   ^ cv[i]            (for i in 0..7) */
    __m128i out_lo0 = _mm_xor_si128(rows[0], rows[2]);
    __m128i out_lo1 = _mm_xor_si128(rows[1], rows[3]);
    __m128i cv_lo = _mm_loadu_si128((const __m128i *)(cv + 0));
    __m128i cv_hi = _mm_loadu_si128((const __m128i *)(cv + 4));
    __m128i out_hi0 = _mm_xor_si128(rows[2], cv_lo);
    __m128i out_hi1 = _mm_xor_si128(rows[3], cv_hi);
    _mm_storeu_si128((__m128i *)(out + 0), out_lo0);
    _mm_storeu_si128((__m128i *)(out + 16), out_lo1);
    _mm_storeu_si128((__m128i *)(out + 32), out_hi0);
    _mm_storeu_si128((__m128i *)(out + 48), out_hi1);
}

/* --- 4-way batched compression ----------------------------------------
 *
 * Mirror of blake3NeonCompress4: transposed layout where vector v_i
 * holds state word i across 4 chunks (one chunk per 32-bit lane). Each
 * G call broadcasts across the four lanes (one per chunk) — the same G
 * formula as the scalar compressor, replicated 4x in lockstep.
 *
 * This is the SSE4.1 fallback the dispatcher selects when AVX2 is not
 * available on the host. Throughput is below the 8-wide AVX2 path but
 * still ~3-4x scalar by replacing per-block sequential rounds with
 * lane-parallel rounds. */

SSE41_TARGET static inline void g4_sse(__m128i *a, __m128i *b, __m128i *c,
                                       __m128i *d, __m128i mx, __m128i my) {
    *a = _mm_add_epi32(_mm_add_epi32(*a, *b), mx);
    *d = rotr_16(_mm_xor_si128(*d, *a));
    *c = _mm_add_epi32(*c, *d);
    *b = rotr_12(_mm_xor_si128(*b, *c));
    *a = _mm_add_epi32(_mm_add_epi32(*a, *b), my);
    *d = rotr_8(_mm_xor_si128(*d, *a));
    *c = _mm_add_epi32(*c, *d);
    *b = rotr_7(_mm_xor_si128(*b, *c));
}

SSE41_TARGET
void blake3SSE41Compress4(const uint32_t *const cv_inputs[4],
                          const uint8_t *const blocks[4],
                          const uint8_t block_lens[4],
                          const uint64_t counters[4], const uint8_t flag_set[4],
                          uint8_t out_cvs[4][32]) {
    /* Load 16 message words per chunk and transpose so that m[i] holds
     * word-i from each of the 4 chunks (one chunk per lane). The simple
     * gather-via-loads pattern mirrors blake3NeonCompress4 — for a 4-way
     * batch the per-call overhead of a fancier 4x4 in-register transpose
     * is not clearly a win on SSE4.1. */
    __m128i m[16];
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t w[4];
        for (uint32_t c = 0; c < 4; c++) {
            memcpy(&w[c], blocks[c] + i * 4, sizeof(uint32_t));
        }
        m[i] = _mm_loadu_si128((const __m128i *)w);
    }

    /* Transposed state: v[i] = vector of (chunk0..3).state[i]. */
    __m128i v[16];
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t w[4] = {
            cv_inputs[0][i],
            cv_inputs[1][i],
            cv_inputs[2][i],
            cv_inputs[3][i],
        };
        v[i] = _mm_loadu_si128((const __m128i *)w);
    }
    v[8] = _mm_set1_epi32((int)SSE41_IV[0]);
    v[9] = _mm_set1_epi32((int)SSE41_IV[1]);
    v[10] = _mm_set1_epi32((int)SSE41_IV[2]);
    v[11] = _mm_set1_epi32((int)SSE41_IV[3]);
    v[12] = _mm_set_epi32(
        (int)(counters[3] & 0xFFFFFFFFul), (int)(counters[2] & 0xFFFFFFFFul),
        (int)(counters[1] & 0xFFFFFFFFul), (int)(counters[0] & 0xFFFFFFFFul));
    v[13] = _mm_set_epi32((int)(counters[3] >> 32), (int)(counters[2] >> 32),
                          (int)(counters[1] >> 32), (int)(counters[0] >> 32));
    v[14] = _mm_set_epi32((int)block_lens[3], (int)block_lens[2],
                          (int)block_lens[1], (int)block_lens[0]);
    v[15] = _mm_set_epi32((int)flag_set[3], (int)flag_set[2], (int)flag_set[1],
                          (int)flag_set[0]);

    for (uint32_t r = 0; r < 7; r++) {
        const uint8_t *s = SSE41_MSG_SCHEDULE[r];
        /* Column step. */
        g4_sse(&v[0], &v[4], &v[8], &v[12], m[s[0]], m[s[1]]);
        g4_sse(&v[1], &v[5], &v[9], &v[13], m[s[2]], m[s[3]]);
        g4_sse(&v[2], &v[6], &v[10], &v[14], m[s[4]], m[s[5]]);
        g4_sse(&v[3], &v[7], &v[11], &v[15], m[s[6]], m[s[7]]);
        /* Diagonal step. */
        g4_sse(&v[0], &v[5], &v[10], &v[15], m[s[8]], m[s[9]]);
        g4_sse(&v[1], &v[6], &v[11], &v[12], m[s[10]], m[s[11]]);
        g4_sse(&v[2], &v[7], &v[8], &v[13], m[s[12]], m[s[13]]);
        g4_sse(&v[3], &v[4], &v[9], &v[14], m[s[14]], m[s[15]]);
    }

    /* Output CV: state[i] ^ state[i+8] for i in 0..7; then de-transpose
     * by extracting lanes back out per chunk. */
    uint32_t buf[8][4];
    for (uint32_t i = 0; i < 8; i++) {
        _mm_storeu_si128((__m128i *)buf[i], _mm_xor_si128(v[i], v[i + 8]));
    }
    for (uint32_t c = 0; c < 4; c++) {
        for (uint32_t i = 0; i < 8; i++) {
            uint32_t w = buf[i][c];
            memcpy(out_cvs[c] + i * 4, &w, sizeof(uint32_t));
        }
    }
}

bool blake3SSE41Available(void) {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_cpu_init();
    return __builtin_cpu_supports("sse4.1");
#else
    /* MSVC: assume yes on x86_64 (SSE4.1 is universal on modern x64). */
    return true;
#endif
}

#else /* !x86_64 */

/* Stubs: not built on this platform. Available=false, fall back to
 * portable for any caller that ignores Available. */

bool blake3SSE41Available(void) {
    return false;
}

void blake3SSE41CompressInPlace(uint32_t cv[8],
                                const uint8_t block[BLAKE3_BLOCK_LEN],
                                uint8_t block_len, uint64_t counter,
                                uint8_t flags) {
    blake3PortableCompressInPlace(cv, block, block_len, counter, flags);
}

void blake3SSE41CompressXof(const uint32_t cv[8],
                            const uint8_t block[BLAKE3_BLOCK_LEN],
                            uint8_t block_len, uint64_t counter, uint8_t flags,
                            uint8_t out[64]) {
    blake3PortableCompressXof(cv, block, block_len, counter, flags, out);
}

void blake3SSE41Compress4(const uint32_t *const cv_inputs[4],
                          const uint8_t *const blocks[4],
                          const uint8_t block_lens[4],
                          const uint64_t counters[4], const uint8_t flag_set[4],
                          uint8_t out_cvs[4][32]) {
    /* No SSE4.1 ISA on this build target: serialize through the
     * portable compress to produce the same output. Available() returns
     * false, so the dispatcher never reaches here in production; this
     * branch only triggers if a caller invokes the symbol directly. */
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

#endif /* x86_64 */

/* ====================================================================== */
/* Test driver                                                            */
/* ====================================================================== */

#ifdef DATAKIT_TEST

#include "cleaner.h"
#include "ctest.h"
#include "datakit.h" /* zmalloc/zfree */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleanerName(uint8_t *, zfree, U8);

/* Cheap xorshift64* for reproducible random fill. */
static uint64_t rng_state = 0xDEADBEEFCAFEBABEull;
static uint64_t xrand(void) {
    uint64_t x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * 2685821657736338717ull;
}
static void rng_seed(uint64_t s) {
    rng_state = s ? s : 1;
}
static void rng_fill(uint8_t *p, size_t n) {
    while (n >= 8) {
        uint64_t v = xrand();
        memcpy(p, &v, 8);
        p += 8;
        n -= 8;
    }
    if (n) {
        uint64_t v = xrand();
        memcpy(p, &v, n);
    }
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int blake3SSE41Test(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    /* Stub-delegation guard: ensure direct calls to
     * blake3SSE41CompressInPlace produce the portable digest even when
     * SSE4.1 is unavailable on this CPU.  Without this check, a silent
     * no-op stub would only surface when a user happens to disable the
     * adaptive dispatcher. */
    TEST("stub delegation: sse41-compress matches portable on direct call") {
        uint32_t cv[8];
        uint8_t block[BLAKE3_BLOCK_LEN];
        for (uint32_t i = 0; i < 8u; i++) {
            cv[i] = 0xCAFEBABEu ^ (i * 0x01020304u);
        }
        for (uint32_t i = 0; i < BLAKE3_BLOCK_LEN; i++) {
            block[i] = (uint8_t)(i + 0x37u);
        }
        const uint64_t counter = 7u;
        const uint8_t block_len = (uint8_t)BLAKE3_BLOCK_LEN;
        const uint8_t flags = 0x0Bu;

        uint32_t cv_ref[8], cv_simd[8];
        memcpy(cv_ref, cv, sizeof(cv));
        memcpy(cv_simd, cv, sizeof(cv));
        blake3PortableCompressInPlace(cv_ref, block, block_len, counter, flags);
        blake3SSE41CompressInPlace(cv_simd, block, block_len, counter, flags);
        if (memcmp(cv_ref, cv_simd, sizeof(cv_ref)) != 0) {
            ERR("Stub/SIMD compress-in-place output != portable");
        }
    }

    if (!blake3SSE41Available()) {
        printf("blake3SSE41: not available on this CPU/build - remaining suite "
               "skipped\n");
        TEST_FINAL_RESULT;
    }

    /* Test 1: single-block compress matches portable on random inputs. */
    TEST("blake3SSE41 single-block matches portable") {
        rng_seed(0x1234567890ABCDEFull);
        for (int iter = 0; iter < 1024; iter++) {
            uint32_t cv_a[8], cv_b[8];
            uint8_t block[BLAKE3_BLOCK_LEN];
            for (int i = 0; i < 8; i++) {
                uint64_t v = xrand();
                cv_a[i] = (uint32_t)v;
            }
            memcpy(cv_b, cv_a, sizeof(cv_a));
            rng_fill(block, sizeof(block));
            uint8_t block_len = (uint8_t)((xrand() % BLAKE3_BLOCK_LEN) + 1);
            uint64_t counter = xrand();
            uint8_t flags = (uint8_t)(xrand() & 0x7F);

            blake3PortableCompressInPlace(cv_a, block, block_len, counter,
                                          flags);
            blake3SSE41CompressInPlace(cv_b, block, block_len, counter, flags);
            if (memcmp(cv_a, cv_b, sizeof(cv_a)) != 0) {
                ERR("compress_in_place mismatch at iter=%d", iter);
                break;
            }
        }
    }

    /* Test 2: XOF compress matches portable on random inputs. */
    TEST("blake3SSE41 XOF matches portable") {
        rng_seed(0xFEDCBA0987654321ull);
        for (int iter = 0; iter < 1024; iter++) {
            uint32_t cv[8];
            uint8_t block[BLAKE3_BLOCK_LEN];
            uint8_t out_a[64], out_b[64];
            for (int i = 0; i < 8; i++) {
                cv[i] = (uint32_t)xrand();
            }
            rng_fill(block, sizeof(block));
            uint8_t block_len = (uint8_t)((xrand() % BLAKE3_BLOCK_LEN) + 1);
            uint64_t counter = xrand();
            uint8_t flags = (uint8_t)(xrand() & 0x7F);

            blake3PortableCompressXof(cv, block, block_len, counter, flags,
                                      out_a);
            blake3SSE41CompressXof(cv, block, block_len, counter, flags, out_b);
            if (memcmp(out_a, out_b, 64) != 0) {
                ERR("compress_xof mismatch at iter=%d", iter);
                break;
            }
        }
    }

    /* Test 3: 100K random-sized inputs hashed via portable blake3Hash
     * vs. a hasher state that uses the portable streaming machinery.
     * Since hasher Update/Finalize routes through file-static
     * compress_in_place inside blake3.c, we can't easily swap in our
     * SSE compress without modifying blake3.c. Instead we drive a
     * "manual" hash by directly calling our compress on each block of
     * a chunked one-shot path and comparing to blake3Hash.
     *
     * Scope: this manual driver covers inputs up to a single BLAKE3
     * chunk (1024 bytes); the Merkle-tree path is exercised by
     * blake3Test's cross-impl differential fuzz in blake3.c. */
    TEST("blake3SSE41 differential hash equivalence (<=1024 bytes)") {
        rng_seed(0xABCD1234FEED5678ull);
        uint8_t buf[BLAKE3_CHUNK_LEN];
        const uint32_t IV[8] = {
            0x6A09E667ul, 0xBB67AE85ul, 0x3C6EF372ul, 0xA54FF53Aul,
            0x510E527Ful, 0x9B05688Cul, 0x1F83D9ABul, 0x5BE0CD19ul,
        };
        const uint8_t FLAG_CHUNK_START = 1u << 0;
        const uint8_t FLAG_CHUNK_END = 1u << 1;
        const uint8_t FLAG_ROOT = 1u << 3;

        for (int iter = 0; iter < 100000; iter++) {
            size_t n = (size_t)(xrand() % (BLAKE3_CHUNK_LEN + 1));
            rng_fill(buf, n);

            uint8_t expected[BLAKE3_OUT_LEN];
            blake3Hash(buf, n, expected);

            /* Drive a single-chunk root hash via our SSE compress. */
            uint32_t cv[8];
            memcpy(cv, IV, sizeof(cv));
            size_t off = 0;
            uint32_t blocks_compressed = 0;
            /* Compress all but the last block as non-terminal. */
            while (off + BLAKE3_BLOCK_LEN < n) {
                uint8_t flags = blocks_compressed == 0 ? FLAG_CHUNK_START : 0u;
                blake3SSE41CompressInPlace(cv, buf + off, BLAKE3_BLOCK_LEN, 0,
                                           flags);
                off += BLAKE3_BLOCK_LEN;
                blocks_compressed++;
            }
            /* Terminal block (also covers the empty-input case where
             * off==0 and n==0; we emit a zero-padded final block). */
            uint8_t last_block[BLAKE3_BLOCK_LEN];
            memset(last_block, 0, sizeof(last_block));
            size_t last_len = n - off;
            memcpy(last_block, buf + off, last_len);
            uint8_t last_flags =
                (uint8_t)((blocks_compressed == 0 ? FLAG_CHUNK_START : 0u) |
                          FLAG_CHUNK_END | FLAG_ROOT);
            uint8_t got_full[64];
            blake3SSE41CompressXof(cv, last_block, (uint8_t)last_len, 0,
                                   last_flags, got_full);
            if (memcmp(got_full, expected, BLAKE3_OUT_LEN) != 0) {
                ERR("hash mismatch at iter=%d size=%zu", iter, n);
                break;
            }
        }
    }

    /* Test 4: perf bench. 1 MiB input -> repeatedly hash via direct SSE
     * compress over the data as one virtual chunk-block stream. This is
     * a microbenchmark on the compress kernel, not a true blake3Hash
     * benchmark (no Merkle tree); it isolates the speedup contributed
     * by this file. */
    TEST("blake3SSE41 perf benchmark (1 MiB)") {
        const size_t N = 1024 * 1024;
        cleanerName(zfree, U8) uint8_t *data = (uint8_t *)zmalloc(N);
        rng_seed(0x9999AAAABBBBCCCCull);
        rng_fill(data, N);

        const uint32_t IV[8] = {
            0x6A09E667ul, 0xBB67AE85ul, 0x3C6EF372ul, 0xA54FF53Aul,
            0x510E527Ful, 0x9B05688Cul, 0x1F83D9ABul, 0x5BE0CD19ul,
        };

        const int iters = 64;
        size_t blocks = N / BLAKE3_BLOCK_LEN;

        /* SSE timing. */
        double t0 = now_sec();
        for (int it = 0; it < iters; it++) {
            uint32_t cv[8];
            memcpy(cv, IV, sizeof(cv));
            for (size_t b = 0; b < blocks; b++) {
                blake3SSE41CompressInPlace(cv, data + b * BLAKE3_BLOCK_LEN,
                                           BLAKE3_BLOCK_LEN, b, 0);
            }
        }
        double dt_sse = now_sec() - t0;

        /* Portable timing for comparison. */
        t0 = now_sec();
        for (int it = 0; it < iters; it++) {
            uint32_t cv[8];
            memcpy(cv, IV, sizeof(cv));
            for (size_t b = 0; b < blocks; b++) {
                blake3PortableCompressInPlace(cv, data + b * BLAKE3_BLOCK_LEN,
                                              BLAKE3_BLOCK_LEN, b, 0);
            }
        }
        double dt_port = now_sec() - t0;

        double total_bytes = (double)iters * (double)N;
        double mib_sse = (total_bytes / (1024.0 * 1024.0)) / dt_sse;
        double mib_port = (total_bytes / (1024.0 * 1024.0)) / dt_port;
        printf(
            "blake3SSE41 compress: %.1f MiB/s  (portable: %.1f MiB/s, %.2fx)\n",
            mib_sse, mib_port, mib_sse / mib_port);
        fflush(stdout);
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
