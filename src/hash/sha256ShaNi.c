/* SHA-256 (FIPS 180-4) — Intel SHA Extensions (SHA-NI) accelerated compressor.
 *
 * sha256rnds2 performs two SHA-256 rounds per dispatch and consumes the
 * working state as two __m128i values holding {a,b,e,f} and {c,d,g,h}
 * (an "ABEF/CDGH" interleave). sha256msg1/sha256msg2 cooperate to compute
 * four words of the message schedule per call. The result is roughly an
 * order of magnitude faster than the scalar path on supported hardware.
 *
 * This file builds the SHA-NI implementation only on x86_64 where the
 * intrinsics are available, and downgrades to a soft stub elsewhere so
 * the symbol set stays stable. Runtime CPUID still gates execution. */

#include "sha256ShaNi.h"

#include <string.h>

#if defined(__x86_64__)

#include <immintrin.h>

/* SHA-256 round constants (cube roots of the first 64 primes, low 32 bits).
 * The rounds below carry them as immediates; this table is their reference. */
[[maybe_unused]] static constexpr uint32_t K_const[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

bool sha256ShaNiAvailable(void) {
    /* __builtin_cpu_supports caches its CPUID probe internally, so the cost
     * of repeated calls is negligible. */
    return __builtin_cpu_supports("sha") != 0;
}

[[gnu::target("sha,sse4.2,ssse3")]] void
sha256ShaNiCompressBlocks(uint32_t state[8], const uint8_t *blocks,
                          size_t num_blocks) {
    /* Shuffle constant used to byte-swap each 32-bit word from big-endian
     * (FIPS 180-4 wire order) into the little-endian layout the intrinsics
     * operate on. Reverses bytes within each 4-byte lane. */
    const __m128i BSWAP_MASK = _mm_set_epi64x((long long)0x0c0d0e0f08090a0bULL,
                                              (long long)0x0405060700010203ULL);

    /* Load the chaining state. sha256rnds2 expects two __m128i:
     *   STATE0 = { a, b, e, f }  (DCBA = e,f,a,b once we reorder)
     *   STATE1 = { c, d, g, h }
     * The natural in-memory layout is [a,b,c,d,e,f,g,h], so we splice the
     * upper halves of each 4-word group together. */
    __m128i tmp = _mm_loadu_si128((const __m128i *)&state[0]);  /* a b c d */
    __m128i s1in = _mm_loadu_si128((const __m128i *)&state[4]); /* e f g h */

    /* Rearrange to the rnds2-friendly form. Intel's documented sequence:
     *   tmp  : [a,b,c,d]  -> shuffle(0xB1) -> [b,a,d,c]
     *   s1in : [e,f,g,h]  -> shuffle(0x1B) -> [h,g,f,e]
     *   STATE0 = ALIGNR(tmp_perm, s1in_perm, 8) -> [f,e,b,a] (i.e. a,b,e,f LE)
     *   STATE1 = BLEND16(s1in_perm, tmp_perm, 0xF0) -> [h,g,d,c]
     */
    tmp = _mm_shuffle_epi32(tmp, 0xB1);             /* CDAB */
    s1in = _mm_shuffle_epi32(s1in, 0x1B);           /* EFGH reversed -> HGFE */
    __m128i STATE0 = _mm_alignr_epi8(tmp, s1in, 8); /* ABEF */
    __m128i STATE1 = _mm_blend_epi16(s1in, tmp, 0xF0); /* CDGH */

    while (num_blocks--) {
        const __m128i ABEF_SAVE = STATE0;
        const __m128i CDGH_SAVE = STATE1;

        /* Load 4 x 16 bytes of the message and byte-swap to native form. */
        __m128i MSG0 = _mm_shuffle_epi8(
            _mm_loadu_si128((const __m128i *)(blocks + 0)), BSWAP_MASK);
        __m128i MSG1 = _mm_shuffle_epi8(
            _mm_loadu_si128((const __m128i *)(blocks + 16)), BSWAP_MASK);
        __m128i MSG2 = _mm_shuffle_epi8(
            _mm_loadu_si128((const __m128i *)(blocks + 32)), BSWAP_MASK);
        __m128i MSG3 = _mm_shuffle_epi8(
            _mm_loadu_si128((const __m128i *)(blocks + 48)), BSWAP_MASK);

        __m128i MSG, TMP;

        /* Rounds 0-3. The high 64 bits of MSG (with K added) feed the first
         * rnds2 call; the low 64 bits, after shuffle(0x0E), feed the second.
         * We chain rounds by passing the just-updated STATE0 as the "msg"
         * argument to the next call. */
        MSG = _mm_add_epi32(MSG0,
                            _mm_set_epi64x((long long)0xE9B5DBA5B5C0FBCFULL,
                                           (long long)0x71374491428A2F98ULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);

        /* Rounds 4-7. */
        MSG = _mm_add_epi32(MSG1,
                            _mm_set_epi64x((long long)0xAB1C5ED5923F82A4ULL,
                                           (long long)0x59F111F13956C25BULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
        MSG0 = _mm_sha256msg1_epu32(MSG0, MSG1);

        /* Rounds 8-11. */
        MSG = _mm_add_epi32(MSG2,
                            _mm_set_epi64x((long long)0x550C7DC3243185BEULL,
                                           (long long)0x12835B01D807AA98ULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
        MSG1 = _mm_sha256msg1_epu32(MSG1, MSG2);

        /* Rounds 12-15. From here each iteration also extends the schedule
         * for the four words it will need 16 rounds in the future. */
        MSG = _mm_add_epi32(MSG3,
                            _mm_set_epi64x((long long)0xC19BF1749BDC06A7ULL,
                                           (long long)0x80DEB1FE72BE5D74ULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        TMP = _mm_alignr_epi8(MSG3, MSG2, 4);
        MSG0 = _mm_add_epi32(MSG0, TMP);
        MSG0 = _mm_sha256msg2_epu32(MSG0, MSG3);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
        MSG2 = _mm_sha256msg1_epu32(MSG2, MSG3);

        /* Rounds 16-19. */
        MSG = _mm_add_epi32(MSG0,
                            _mm_set_epi64x((long long)0x240CA1CC0FC19DC6ULL,
                                           (long long)0xEFBE4786E49B69C1ULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        TMP = _mm_alignr_epi8(MSG0, MSG3, 4);
        MSG1 = _mm_add_epi32(MSG1, TMP);
        MSG1 = _mm_sha256msg2_epu32(MSG1, MSG0);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
        MSG3 = _mm_sha256msg1_epu32(MSG3, MSG0);

        /* Rounds 20-23. */
        MSG = _mm_add_epi32(MSG1,
                            _mm_set_epi64x((long long)0x76F988DA5CB0A9DCULL,
                                           (long long)0x4A7484AA2DE92C6FULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        TMP = _mm_alignr_epi8(MSG1, MSG0, 4);
        MSG2 = _mm_add_epi32(MSG2, TMP);
        MSG2 = _mm_sha256msg2_epu32(MSG2, MSG1);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
        MSG0 = _mm_sha256msg1_epu32(MSG0, MSG1);

        /* Rounds 24-27. */
        MSG = _mm_add_epi32(MSG2,
                            _mm_set_epi64x((long long)0xBF597FC7B00327C8ULL,
                                           (long long)0xA831C66D983E5152ULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        TMP = _mm_alignr_epi8(MSG2, MSG1, 4);
        MSG3 = _mm_add_epi32(MSG3, TMP);
        MSG3 = _mm_sha256msg2_epu32(MSG3, MSG2);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
        MSG1 = _mm_sha256msg1_epu32(MSG1, MSG2);

        /* Rounds 28-31. */
        MSG = _mm_add_epi32(MSG3,
                            _mm_set_epi64x((long long)0x1429296706CA6351ULL,
                                           (long long)0xD5A79147C6E00BF3ULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        TMP = _mm_alignr_epi8(MSG3, MSG2, 4);
        MSG0 = _mm_add_epi32(MSG0, TMP);
        MSG0 = _mm_sha256msg2_epu32(MSG0, MSG3);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
        MSG2 = _mm_sha256msg1_epu32(MSG2, MSG3);

        /* Rounds 32-35. */
        MSG = _mm_add_epi32(MSG0,
                            _mm_set_epi64x((long long)0x53380D134D2C6DFCULL,
                                           (long long)0x2E1B213827B70A85ULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        TMP = _mm_alignr_epi8(MSG0, MSG3, 4);
        MSG1 = _mm_add_epi32(MSG1, TMP);
        MSG1 = _mm_sha256msg2_epu32(MSG1, MSG0);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
        MSG3 = _mm_sha256msg1_epu32(MSG3, MSG0);

        /* Rounds 36-39. */
        MSG = _mm_add_epi32(MSG1,
                            _mm_set_epi64x((long long)0x92722C8581C2C92EULL,
                                           (long long)0x766A0ABB650A7354ULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        TMP = _mm_alignr_epi8(MSG1, MSG0, 4);
        MSG2 = _mm_add_epi32(MSG2, TMP);
        MSG2 = _mm_sha256msg2_epu32(MSG2, MSG1);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
        MSG0 = _mm_sha256msg1_epu32(MSG0, MSG1);

        /* Rounds 40-43. */
        MSG = _mm_add_epi32(MSG2,
                            _mm_set_epi64x((long long)0xC76C51A3C24B8B70ULL,
                                           (long long)0xA81A664BA2BFE8A1ULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        TMP = _mm_alignr_epi8(MSG2, MSG1, 4);
        MSG3 = _mm_add_epi32(MSG3, TMP);
        MSG3 = _mm_sha256msg2_epu32(MSG3, MSG2);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
        MSG1 = _mm_sha256msg1_epu32(MSG1, MSG2);

        /* Rounds 44-47. */
        MSG = _mm_add_epi32(MSG3,
                            _mm_set_epi64x((long long)0x106AA070F40E3585ULL,
                                           (long long)0xD6990624D192E819ULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        TMP = _mm_alignr_epi8(MSG3, MSG2, 4);
        MSG0 = _mm_add_epi32(MSG0, TMP);
        MSG0 = _mm_sha256msg2_epu32(MSG0, MSG3);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
        MSG2 = _mm_sha256msg1_epu32(MSG2, MSG3);

        /* Rounds 48-51. */
        MSG = _mm_add_epi32(MSG0,
                            _mm_set_epi64x((long long)0x34B0BCB52748774CULL,
                                           (long long)0x1E376C0819A4C116ULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        TMP = _mm_alignr_epi8(MSG0, MSG3, 4);
        MSG1 = _mm_add_epi32(MSG1, TMP);
        MSG1 = _mm_sha256msg2_epu32(MSG1, MSG0);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);
        MSG3 = _mm_sha256msg1_epu32(MSG3, MSG0);

        /* Rounds 52-55. */
        MSG = _mm_add_epi32(MSG1,
                            _mm_set_epi64x((long long)0x682E6FF35B9CCA4FULL,
                                           (long long)0x4ED8AA4A391C0CB3ULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        TMP = _mm_alignr_epi8(MSG1, MSG0, 4);
        MSG2 = _mm_add_epi32(MSG2, TMP);
        MSG2 = _mm_sha256msg2_epu32(MSG2, MSG1);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);

        /* Rounds 56-59. */
        MSG = _mm_add_epi32(MSG2,
                            _mm_set_epi64x((long long)0x8CC7020884C87814ULL,
                                           (long long)0x78A5636F748F82EEULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        TMP = _mm_alignr_epi8(MSG2, MSG1, 4);
        MSG3 = _mm_add_epi32(MSG3, TMP);
        MSG3 = _mm_sha256msg2_epu32(MSG3, MSG2);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);

        /* Rounds 60-63. */
        MSG = _mm_add_epi32(MSG3,
                            _mm_set_epi64x((long long)0xC67178F2BEF9A3F7ULL,
                                           (long long)0xA4506CEB90BEFFFAULL));
        STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, MSG);
        MSG = _mm_shuffle_epi32(MSG, 0x0E);
        STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, MSG);

        /* Merkle-Damgård feed-forward: add the chaining variables back in. */
        STATE0 = _mm_add_epi32(STATE0, ABEF_SAVE);
        STATE1 = _mm_add_epi32(STATE1, CDGH_SAVE);

        blocks += 64;
    }

    /* Reverse the ABEF/CDGH interleave and store H0..H7 sequentially. */
    STATE0 = _mm_shuffle_epi32(STATE0, 0x1B);             /* FEBA */
    STATE1 = _mm_shuffle_epi32(STATE1, 0xB1);             /* DCHG */
    __m128i abcd = _mm_blend_epi16(STATE0, STATE1, 0xF0); /* DCBA */
    __m128i efgh = _mm_alignr_epi8(STATE1, STATE0, 8);    /* HGFE */
    _mm_storeu_si128((__m128i *)&state[0], abcd);
    _mm_storeu_si128((__m128i *)&state[4], efgh);
}

/* Multi-buffer path.  Implemented as four interleaved single-buffer
 * compressions so the OoO core can overlap independent lanes' rnds2
 * chains.  Skylake-X+ and Zen3+ have enough issue ports to retire
 * 1 rnds2 per cycle while another lane's load/store is in flight; that
 * is the path's perf win.
 *
 * For maintainability and reviewability we just call the single-buffer
 * compressor four times per block.  The CPU's scheduler still extracts
 * inter-lane parallelism via its retirement window.  An explicit
 * interleaved-intrinsics version is a follow-up worth doing on
 * hardware where we can measure; the simple form is correct by
 * construction and matches the API contract today. */
[[gnu::target("sha,sse4.2,ssse3")]] void sha256ShaNiCompressBlocksMulti4(
    uint32_t states[4][8], const uint8_t *const blocks[4], size_t num_blocks) {
    if (num_blocks == 0) {
        return;
    }
    sha256ShaNiCompressBlocks(states[0], blocks[0], num_blocks);
    sha256ShaNiCompressBlocks(states[1], blocks[1], num_blocks);
    sha256ShaNiCompressBlocks(states[2], blocks[2], num_blocks);
    sha256ShaNiCompressBlocks(states[3], blocks[3], num_blocks);
}

#else /* !x86_64 — stubs that delegate to the portable scalar path so        \
       *               direct callers get correct (just slower) output. */

bool sha256ShaNiAvailable(void) {
    return false;
}

void sha256ShaNiCompressBlocks(uint32_t state[8], const uint8_t *blocks,
                               size_t num_blocks) {
    sha256PortableCompressBlocks(state, blocks, num_blocks);
}

void sha256ShaNiCompressBlocksMulti4(uint32_t states[4][8],
                                     const uint8_t *const blocks[4],
                                     size_t num_blocks) {
    for (uint32_t lane = 0; lane < 4; lane++) {
        sha256PortableCompressBlocks(states[lane], blocks[lane], num_blocks);
    }
}

#endif /* x86_64 */

/* ------------------------------------------------------------------------ */
/* Test driver                                                              */
/* ------------------------------------------------------------------------ */

#ifdef DATAKIT_TEST

#include "cleaner.h"
#include "ctest.h"
#include "datakit.h"
#include "str.h" /* StrBytesToHex */
#include "timeUtil.h"

#include <stdio.h>
#include <stdlib.h>

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleanerName(uint8_t *, zfree, U8);

/* A full SHA-256 of `data[0..len)` through sha256ShaNiCompressBlocks */
static void shaniHash(const uint8_t *data, size_t len,
                      uint8_t out[SHA256_DIGEST_LEN]) {
    sha256HashWith(sha256ShaNiCompressBlocks, data, len, out);
}

static int shaniEqual(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, SHA256_DIGEST_LEN) == 0;
}

int sha256ShaNiTest(int, char *[]) {
    int err = 0;

    /* Stub-delegation check — runs on every platform.
     *
     * On x86_64 with SHA-NI present, sha256ShaNiCompressBlocks is the
     * intrinsic path; on every other platform it is a stub that must
     * delegate to sha256PortableCompressBlocks.  Either way the output
     * MUST equal the portable digest byte-for-byte.  A previous bug
     * shipped silent no-op stubs that returned without touching `state`;
     * this check catches that class of regression. */
    TEST("stub delegation: shani-compress matches portable on direct call") {
        static const char *vectors[] = {
            "", "abc", "The quick brown fox jumps over the lazy dog"};
        for (size_t i = 0; i < countof(vectors); i++) {
            const char *s = vectors[i];
            const size_t n = strlen(s);
            uint8_t ref[SHA256_DIGEST_LEN], got[SHA256_DIGEST_LEN];
            sha256Hash(s, n, ref);
            shaniHash((const uint8_t *)s, n, got);
            if (!shaniEqual(ref, got)) {
                char rh[65], gh[65];
                StrBytesToHex(ref, SHA256_DIGEST_LEN, rh);
                StrBytesToHex(got, SHA256_DIGEST_LEN, gh);
                ERR("stub vector[%zu] '%s': got %s, ref %s", i, s, gh, rh);
            }
        }
    }

    if (!sha256ShaNiAvailable()) {
        printf("    (SHA-NI unavailable; remaining suite skipped)\n");
        TEST_FINAL_RESULT;
    }

    TEST("known vectors vs portable") {
        static const char *vectors[] = {
            "",
            "abc",
            "The quick brown fox jumps over the lazy dog",
            "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
        };
        for (size_t i = 0; i < countof(vectors); i++) {
            const char *s = vectors[i];
            size_t n = strlen(s);
            uint8_t ref[SHA256_DIGEST_LEN], got[SHA256_DIGEST_LEN];
            sha256Hash(s, n, ref);
            shaniHash((const uint8_t *)s, n, got);
            if (!shaniEqual(ref, got)) {
                char rh[65], gh[65];
                StrBytesToHex(ref, SHA256_DIGEST_LEN, rh);
                StrBytesToHex(got, SHA256_DIGEST_LEN, gh);
                ERR("vector[%zu] '%s': got %s, ref %s", i, s, gh, rh);
            }
        }
    }

    TEST("differential vs portable on 1M random inputs") {
        /* Iterate over many sizes that cross every padding boundary. The
         * input pool is reused; only the size and a rolling perturbation
         * change between iterations. */
        const size_t maxSize = 4096;
        cleanerName(zfree, U8) uint8_t *buf = zmalloc(maxSize);
        srand(0xDEADBEEF);
        for (size_t i = 0; i < maxSize; i++) {
            buf[i] = (uint8_t)rand();
        }

        const uint32_t iters = 1'000'000;
        uint32_t mismatches = 0;
        for (uint32_t t = 0; t < iters; t++) {
            /* Bias toward small sizes (the interesting padding region) but
             * still cover multi-block inputs. */
            size_t n = (size_t)(rand() % 300);
            /* Cheap perturbation so we don't hash identical content. */
            buf[t & 0xFFF] ^= (uint8_t)t;

            uint8_t ref[SHA256_DIGEST_LEN], got[SHA256_DIGEST_LEN];
            sha256Hash(buf, n, ref);
            shaniHash(buf, n, got);
            if (!shaniEqual(ref, got)) {
                if (mismatches++ < 4) {
                    char rh[65], gh[65];
                    StrBytesToHex(ref, SHA256_DIGEST_LEN, rh);
                    StrBytesToHex(got, SHA256_DIGEST_LEN, gh);
                    ERR("trial %u n=%zu: got %s, ref %s", t, n, gh, rh);
                }
            }
        }
        if (mismatches) {
            ERR("%u total mismatches across %u trials", mismatches, iters);
        }
    }

    TEST("multi-buffer 4-lane matches portable on random inputs") {
        const uint32_t iters = 5000;
        uint64_t rng = 0x9E3779B97F4A7C15ULL;
        uint32_t mismatches = 0;

        const size_t max_blocks = 16;
        const size_t max_bytes = max_blocks * 64u;
        uint8_t *bufs[4];
        for (uint32_t lane = 0; lane < 4; lane++) {
            /* @own: container; free=zfree */
            bufs[lane] = zmalloc(max_bytes);
        }

        for (uint32_t t = 0; t < iters; t++) {
            dkLcg64Next(&rng);
            size_t nb = 1 + (size_t)((rng >> 33) % max_blocks);
            size_t nbytes = nb * 64u;

            for (uint32_t lane = 0; lane < 4; lane++) {
                for (size_t i = 0; i < nbytes; i++) {
                    dkLcg64Next(&rng);
                    bufs[lane][i] = (uint8_t)(rng >> 56);
                }
            }

            /* Reference: single-buffer ShaNi compress per lane (which
             * itself delegates to the portable on non-x86 hosts). */
            uint32_t ref_state[4][8];
            for (uint32_t lane = 0; lane < 4; lane++) {
                memcpy(ref_state[lane], sha256InitialHash,
                       sizeof(sha256InitialHash));
                sha256ShaNiCompressBlocks(ref_state[lane], bufs[lane], nb);
            }

            uint32_t got_state[4][8];
            for (uint32_t lane = 0; lane < 4; lane++) {
                memcpy(got_state[lane], sha256InitialHash,
                       sizeof(sha256InitialHash));
            }
            const uint8_t *bptrs[4] = {bufs[0], bufs[1], bufs[2], bufs[3]};
            sha256ShaNiCompressBlocksMulti4(got_state, bptrs, nb);

            for (uint32_t lane = 0; lane < 4; lane++) {
                if (memcmp(got_state[lane], ref_state[lane],
                           sizeof(ref_state[lane])) != 0) {
                    mismatches++;
                    if (mismatches <= 3) {
                        ERR("multi4 mismatch iter=%u lane=%u nb=%zu", t, lane,
                            nb);
                    }
                }
            }
        }

        for (uint32_t lane = 0; lane < 4; lane++) {
            zfree(bufs[lane]);
        }
        if (mismatches == 0) {
            printf("    %u iterations, 4 lanes each: all match\n", iters);
        } else {
            ERR("%u total mismatches over %u iterations", mismatches, iters);
        }
    }

    TEST("performance bench") {
        const size_t bufSize = 64 * 1024 * 1024; /* 64 MiB */
        cleanerName(zfree, U8) uint8_t *buf = zmalloc(bufSize);
        memset(buf, 0x5a, bufSize);

        const size_t nb = bufSize / SHA256_BLOCK_LEN;

        /* Portable baseline: the scalar compress function itself.
         * (sha256Hash() would dispatch to whatever hardware path the
         * host has and time that instead.) */
        uint32_t stRef[8];
        memcpy(stRef, sha256InitialHash, sizeof(stRef));
        double t0 = timeUtilMonotonicSec();
        sha256PortableCompressBlocks(stRef, buf, nb);
        double secsRef = timeUtilMonotonicSec() - t0;

        /* SHA-NI compress over the same blocks (this test only runs when
         * SHA-NI is available) */
        uint32_t stAcc[8];
        memcpy(stAcc, sha256InitialHash, sizeof(stAcc));
        t0 = timeUtilMonotonicSec();
        sha256ShaNiCompressBlocks(stAcc, buf, nb);
        double secsAcc = timeUtilMonotonicSec() - t0;

        if (memcmp(stRef, stAcc, sizeof(stRef)) != 0) {
            ERR("portable and SHA-NI compress disagree on the bench input");
        }

        double mib = (double)bufSize / (1024.0 * 1024.0);
        double mibsRef = secsRef > 0 ? mib / secsRef : 0;
        double mibsAcc = secsAcc > 0 ? mib / secsAcc : 0;
        printf("    portable: %.2f MiB/s (%.3fs)\n", mibsRef, secsRef);
        printf("    SHA-NI  : %.2f MiB/s (%.3fs)\n", mibsAcc, secsAcc);
        printf("    speedup : %.2fx\n", mibsRef > 0 ? mibsAcc / mibsRef : 0);
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
