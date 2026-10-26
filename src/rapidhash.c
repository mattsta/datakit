/* rapidhash V3 — see rapidhash.h for the public contract.
 *
 * Ported from the upstream reference by Nicolas De Carli
 * (https://github.com/Nicoshev/rapidhash), itself based on wyhash by
 * Wang Yi (https://github.com/wangyi-fudan/wyhash).
 *
 * Implementation notes:
 *   - Single source-of-truth per variant: each variant's algorithm body
 *     is an `always_inline` static function parameterized by `bool prot`;
 *     the FAST and PROTECTED public entries are thin wrappers that pin
 *     the constant so the compiler folds away the per-mum branch.
 *   - 64x64 -> 128 multiply uses `__uint128_t` (mandatory under
 *     __SIZEOF_INT128__) with a pure-C SWAR fallback for the rare
 *     toolchain that lacks it.
 *   - Streaming `rapidhashStream*` matches the one-shot output bit-for-
 *     bit by re-running the same tail mixer over the residual buffer at
 *     Finalize time; bulk-stride iterations during Update share the same
 *     7-lane mix body the one-shot uses. */

#include "rapidhash.h"

#include <string.h>

/* ============================================================ *
 * Default secret                                                *
 * ============================================================ */

static const uint64_t rapidhashDefaultSecret[RAPIDHASH_SECRET_LEN] = {
    0x2d358dccaa6c78a5ULL, 0x8bb84b93962eacc9ULL, 0x4b33a62ed433d4a3ULL,
    0x4d5a2da51de1aa47ULL, 0xa0761d6478bd642fULL, 0xe7037ed1a0b428dbULL,
    0x90ed1765281c388cULL, 0xaaaaaaaaaaaaaaaaULL};

const uint64_t *rapidhashGetDefaultSecret(void) {
    return rapidhashDefaultSecret;
}

/* ============================================================ *
 * Branch hints                                                  *
 * ============================================================ */

#define rapidLikely(x) __builtin_expect(!!(x), 1)

/* ============================================================ *
 * Endian-aware unaligned reads                                  *
 * ============================================================ */

static inline __attribute__((always_inline)) uint64_t
rapidRead64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap64(v);
#endif
    return v;
}

static inline __attribute__((always_inline)) uint64_t
rapidRead32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap32(v);
#endif
    return (uint64_t)v;
}

/* ============================================================ *
 * 64x64 -> 128 multiply                                         *
 * ============================================================ */

static inline __attribute__((always_inline)) void
rapidMum(uint64_t *A, uint64_t *B, bool prot) {
#if defined(__SIZEOF_INT128__)
    __uint128_t r = (__uint128_t)*A * (__uint128_t)*B;
    uint64_t lo = (uint64_t)r;
    uint64_t hi = (uint64_t)(r >> 64);
#else
    /* Portable SWAR fallback for the rare 64-bit toolchain that lacks
     * __uint128_t.  Identical numerical result. */
    uint64_t ha = *A >> 32, hb = *B >> 32;
    uint64_t la = (uint32_t)*A, lb = (uint32_t)*B;
    uint64_t rh = ha * hb, rm0 = ha * lb, rm1 = hb * la, rl = la * lb;
    uint64_t t = rl + (rm0 << 32), c = t < rl;
    uint64_t lo = t + (rm1 << 32);
    c += lo < t;
    uint64_t hi = rh + (rm0 >> 32) + (rm1 >> 32) + c;
#endif
    if (prot) {
        *A ^= lo;
        *B ^= hi;
    } else {
        *A = lo;
        *B = hi;
    }
}

static inline __attribute__((always_inline)) uint64_t rapidMix(uint64_t a,
                                                               uint64_t b,
                                                               bool prot) {
    rapidMum(&a, &b, prot);
    return a ^ b;
}

/* ============================================================ *
 * Variant bodies                                                *
 *                                                               *
 * Each is `always_inline` and takes `prot` as a constant.  The  *
 * thin public entries below pin `prot` so the per-mum branch    *
 * folds away in the FAST/PROTECTED specializations.             *
 * ============================================================ */

/* The Main body's small-input branch, lane reduction, tail ladder, and
 * final fold are identical between COMPACT and UNROLLED — only the
 * bulk-loop shape differs.  Both implementations always_inline so the
 * `prot` constant folds at each call site. */

/* One 7-lane mix pass over a single 112-byte chunk.  Used both as the
 * COMPACT bulk-loop body and as the UNROLLED tail when residual is in
 * (112, 224]. */
#define RAPID_MAIN_CHUNK_112(P)                                                \
    do {                                                                       \
        seed = rapidMix(rapidRead64((P)) ^ secret[0],                          \
                        rapidRead64((P) + 8) ^ seed, prot);                    \
        s1 = rapidMix(rapidRead64((P) + 16) ^ secret[1],                       \
                      rapidRead64((P) + 24) ^ s1, prot);                       \
        s2 = rapidMix(rapidRead64((P) + 32) ^ secret[2],                       \
                      rapidRead64((P) + 40) ^ s2, prot);                       \
        s3 = rapidMix(rapidRead64((P) + 48) ^ secret[3],                       \
                      rapidRead64((P) + 56) ^ s3, prot);                       \
        s4 = rapidMix(rapidRead64((P) + 64) ^ secret[4],                       \
                      rapidRead64((P) + 72) ^ s4, prot);                       \
        s5 = rapidMix(rapidRead64((P) + 80) ^ secret[5],                       \
                      rapidRead64((P) + 88) ^ s5, prot);                       \
        s6 = rapidMix(rapidRead64((P) + 96) ^ secret[6],                       \
                      rapidRead64((P) + 104) ^ s6, prot);                      \
    } while (0)

/* Common post-bulk path: lane reduction (only if bulk fired), tail
 * ladder, final fold.  Same for both COMPACT and UNROLLED. */
static inline __attribute__((always_inline)) uint64_t rapidhashMainPostBulk(
    const uint8_t *p, size_t i, uint64_t seed, uint64_t s1, uint64_t s2,
    uint64_t s3, uint64_t s4, uint64_t s5, uint64_t s6, const uint64_t *secret,
    bool bulk_engaged, bool prot) {
    if (bulk_engaged) {
        /* Asymmetric reduction tree — order-sensitive. */
        seed ^= s1;
        s2 ^= s3;
        s4 ^= s5;
        seed ^= s6;
        s2 ^= s4;
        seed ^= s2;
    }
    if (i > 16) {
        seed = rapidMix(rapidRead64(p) ^ secret[2], rapidRead64(p + 8) ^ seed,
                        prot);
        if (i > 32) {
            seed = rapidMix(rapidRead64(p + 16) ^ secret[2],
                            rapidRead64(p + 24) ^ seed, prot);
            if (i > 48) {
                seed = rapidMix(rapidRead64(p + 32) ^ secret[1],
                                rapidRead64(p + 40) ^ seed, prot);
                if (i > 64) {
                    seed = rapidMix(rapidRead64(p + 48) ^ secret[1],
                                    rapidRead64(p + 56) ^ seed, prot);
                    if (i > 80) {
                        seed = rapidMix(rapidRead64(p + 64) ^ secret[2],
                                        rapidRead64(p + 72) ^ seed, prot);
                        if (i > 96) {
                            seed = rapidMix(rapidRead64(p + 80) ^ secret[1],
                                            rapidRead64(p + 88) ^ seed, prot);
                        }
                    }
                }
            }
        }
    }
    uint64_t a = rapidRead64(p + i - 16) ^ i;
    uint64_t b = rapidRead64(p + i - 8);
    a ^= secret[1];
    b ^= seed;
    rapidMum(&a, &b, prot);
    return rapidMix(a ^ secret[7], b ^ secret[1] ^ i, prot);
}

/* Common small-input ≤16 branch + final fold. */
static inline __attribute__((always_inline)) uint64_t
rapidhashMainSmall(const uint8_t *p, size_t len, uint64_t seed,
                   const uint64_t *secret, bool prot) {
    uint64_t a = 0, b = 0;
    if (len >= 4) {
        seed ^= len;
        if (len >= 8) {
            a = rapidRead64(p);
            b = rapidRead64(p + len - 8);
        } else {
            a = rapidRead32(p);
            b = rapidRead32(p + len - 4);
        }
    } else if (len > 0) {
        a = (((uint64_t)p[0]) << 45) | p[len - 1];
        b = p[len >> 1];
    }
    a ^= secret[1];
    b ^= seed;
    rapidMum(&a, &b, prot);
    return rapidMix(a ^ secret[7], b ^ secret[1] ^ len, prot);
}

/* ----- Main variant, COMPACT shape (7-lane / 112 B stride) ----- */
static inline __attribute__((always_inline)) uint64_t
rapidhashBodyMainCompact(const void *key, size_t len, uint64_t seed,
                         const uint64_t *secret, bool prot) {
    const uint8_t *p = (const uint8_t *)key;
    seed ^= rapidMix(seed ^ secret[2], secret[1], prot);
    if (rapidLikely(len <= 16)) {
        return rapidhashMainSmall(p, len, seed, secret, prot);
    }
    size_t i = len;
    uint64_t s1 = seed, s2 = seed, s3 = seed;
    uint64_t s4 = seed, s5 = seed, s6 = seed;
    bool bulk_engaged = false;
    if (len > 112) {
        do {
            RAPID_MAIN_CHUNK_112(p);
            p += 112;
            i -= 112;
        } while (i > 112);
        bulk_engaged = true;
    }
    return rapidhashMainPostBulk(p, i, seed, s1, s2, s3, s4, s5, s6, secret,
                                 bulk_engaged, prot);
}

/* ----- Main variant, UNROLLED shape (14-lane / 224 B stride) -----
 *
 * Bit-identical output to COMPACT.  Wins ~10%% on long inputs from
 * additional ILP; for inputs <= 224 B the outer doubled loop never
 * fires and execution reduces to the same path COMPACT would take. */
static inline __attribute__((always_inline)) uint64_t
rapidhashBodyMainUnrolled(const void *key, size_t len, uint64_t seed,
                          const uint64_t *secret, bool prot) {
    const uint8_t *p = (const uint8_t *)key;
    seed ^= rapidMix(seed ^ secret[2], secret[1], prot);
    if (rapidLikely(len <= 16)) {
        return rapidhashMainSmall(p, len, seed, secret, prot);
    }
    size_t i = len;
    uint64_t s1 = seed, s2 = seed, s3 = seed;
    uint64_t s4 = seed, s5 = seed, s6 = seed;
    bool bulk_engaged = false;
    if (len > 112) {
        while (i > 224) {
            RAPID_MAIN_CHUNK_112(p);
            RAPID_MAIN_CHUNK_112(p + 112);
            p += 224;
            i -= 224;
        }
        if (i > 112) {
            RAPID_MAIN_CHUNK_112(p);
            p += 112;
            i -= 112;
        }
        bulk_engaged = true;
    }
    return rapidhashMainPostBulk(p, i, seed, s1, s2, s3, s4, s5, s6, secret,
                                 bulk_engaged, prot);
}

/* Dispatcher: pick COMPACT or UNROLLED per call.  Inline so the picked
 * body inlines through and the per-mum prot branch folds at the call
 * site for FAST vs PROTECTED. */

/* Inputs strictly longer than this engage UNROLLED.  Below it both
 * bodies execute identical machine code (UNROLLED's outer loop won't
 * fire) so the choice is irrelevant — but biasing the dispatch to
 * COMPACT below the threshold avoids paying for even one dead branch
 * on the small-input fast path. */
#define RAPID_AUTO_UNROLL_THRESHOLD ((size_t)224)

static rapidhashImpl rapidForcedImpl = RAPIDHASH_IMPL_AUTO;

/* `always_inline` is deliberate: inlining the dispatcher into each
 * public entry lets the compiler specialize the branch on `prot`
 * (constant per entry) and keeps the per-call function-call overhead
 * out of the bench's tight loop.  Measured: removing always_inline
 * here costs ~36% on the 1 MB one-shot row on Apple Silicon — the
 * audit's theoretical i-cache argument did not survive the bench. */
static inline __attribute__((always_inline)) uint64_t
rapidhashBodyMain(const void *key, size_t len, uint64_t seed,
                  const uint64_t *secret, bool prot) {
    rapidhashImpl pick = rapidForcedImpl;
    if (pick == RAPIDHASH_IMPL_AUTO) {
        pick = (len > RAPID_AUTO_UNROLL_THRESHOLD) ? RAPIDHASH_IMPL_UNROLLED
                                                   : RAPIDHASH_IMPL_COMPACT;
    }
    if (pick == RAPIDHASH_IMPL_UNROLLED) {
        return rapidhashBodyMainUnrolled(key, len, seed, secret, prot);
    }
    return rapidhashBodyMainCompact(key, len, seed, secret, prot);
}

void rapidhashSetForceImpl(rapidhashImpl impl) {
    rapidForcedImpl = impl;
}

rapidhashImpl rapidhashGetForceImpl(void) {
    return rapidForcedImpl;
}

size_t rapidhashAutoUnrollThreshold(void) {
    return RAPID_AUTO_UNROLL_THRESHOLD;
}

/* ----- 5-lane micro variant (stride 80) ----- */
static inline __attribute__((always_inline)) uint64_t
rapidhashBodyMicro(const void *key, size_t len, uint64_t seed,
                   const uint64_t *secret, bool prot) {
    const uint8_t *p = (const uint8_t *)key;
    seed ^= rapidMix(seed ^ secret[2], secret[1], prot);
    uint64_t a = 0, b = 0;
    size_t i = len;

    if (rapidLikely(len <= 16)) {
        if (len >= 4) {
            seed ^= len;
            if (len >= 8) {
                a = rapidRead64(p);
                b = rapidRead64(p + len - 8);
            } else {
                a = rapidRead32(p);
                b = rapidRead32(p + len - 4);
            }
        } else if (len > 0) {
            a = (((uint64_t)p[0]) << 45) | p[len - 1];
            b = p[len >> 1];
        }
    } else {
        if (i > 80) {
            uint64_t s1 = seed, s2 = seed, s3 = seed, s4 = seed;
            do {
                seed = rapidMix(rapidRead64(p) ^ secret[0],
                                rapidRead64(p + 8) ^ seed, prot);
                s1 = rapidMix(rapidRead64(p + 16) ^ secret[1],
                              rapidRead64(p + 24) ^ s1, prot);
                s2 = rapidMix(rapidRead64(p + 32) ^ secret[2],
                              rapidRead64(p + 40) ^ s2, prot);
                s3 = rapidMix(rapidRead64(p + 48) ^ secret[3],
                              rapidRead64(p + 56) ^ s3, prot);
                s4 = rapidMix(rapidRead64(p + 64) ^ secret[4],
                              rapidRead64(p + 72) ^ s4, prot);
                p += 80;
                i -= 80;
            } while (i > 80);
            seed ^= s1;
            s2 ^= s3;
            seed ^= s4;
            seed ^= s2;
        }
        if (i > 16) {
            seed = rapidMix(rapidRead64(p) ^ secret[2],
                            rapidRead64(p + 8) ^ seed, prot);
            if (i > 32) {
                seed = rapidMix(rapidRead64(p + 16) ^ secret[2],
                                rapidRead64(p + 24) ^ seed, prot);
                if (i > 48) {
                    seed = rapidMix(rapidRead64(p + 32) ^ secret[1],
                                    rapidRead64(p + 40) ^ seed, prot);
                    if (i > 64) {
                        seed = rapidMix(rapidRead64(p + 48) ^ secret[1],
                                        rapidRead64(p + 56) ^ seed, prot);
                    }
                }
            }
        }
        a = rapidRead64(p + i - 16) ^ i;
        b = rapidRead64(p + i - 8);
    }
    a ^= secret[1];
    b ^= seed;
    rapidMum(&a, &b, prot);
    return rapidMix(a ^ secret[7], b ^ secret[1] ^ i, prot);
}

/* ----- 3-lane nano variant (stride 48) ----- */
static inline __attribute__((always_inline)) uint64_t
rapidhashBodyNano(const void *key, size_t len, uint64_t seed,
                  const uint64_t *secret, bool prot) {
    const uint8_t *p = (const uint8_t *)key;
    seed ^= rapidMix(seed ^ secret[2], secret[1], prot);
    uint64_t a = 0, b = 0;
    size_t i = len;

    if (rapidLikely(len <= 16)) {
        if (len >= 4) {
            seed ^= len;
            if (len >= 8) {
                a = rapidRead64(p);
                b = rapidRead64(p + len - 8);
            } else {
                a = rapidRead32(p);
                b = rapidRead32(p + len - 4);
            }
        } else if (len > 0) {
            a = (((uint64_t)p[0]) << 45) | p[len - 1];
            b = p[len >> 1];
        }
    } else {
        if (i > 48) {
            uint64_t s1 = seed, s2 = seed;
            do {
                seed = rapidMix(rapidRead64(p) ^ secret[0],
                                rapidRead64(p + 8) ^ seed, prot);
                s1 = rapidMix(rapidRead64(p + 16) ^ secret[1],
                              rapidRead64(p + 24) ^ s1, prot);
                s2 = rapidMix(rapidRead64(p + 32) ^ secret[2],
                              rapidRead64(p + 40) ^ s2, prot);
                p += 48;
                i -= 48;
            } while (i > 48);
            seed ^= s1;
            seed ^= s2;
        }
        if (i > 16) {
            seed = rapidMix(rapidRead64(p) ^ secret[2],
                            rapidRead64(p + 8) ^ seed, prot);
            if (i > 32) {
                seed = rapidMix(rapidRead64(p + 16) ^ secret[2],
                                rapidRead64(p + 24) ^ seed, prot);
            }
        }
        a = rapidRead64(p + i - 16) ^ i;
        b = rapidRead64(p + i - 8);
    }
    a ^= secret[1];
    b ^= seed;
    rapidMum(&a, &b, prot);
    return rapidMix(a ^ secret[7], b ^ secret[1] ^ i, prot);
}

/* ============================================================ *
 * Public one-shot entries                                       *
 * ============================================================ */

/* Main variant — FAST */
uint64_t rapidhashWithSecret(const void *key, size_t len, uint64_t seed,
                             const uint64_t secret[RAPIDHASH_SECRET_LEN]) {
    return rapidhashBodyMain(key, len, seed, secret, false);
}
uint64_t rapidhashWithSeed(const void *key, size_t len, uint64_t seed) {
    return rapidhashBodyMain(key, len, seed, rapidhashDefaultSecret, false);
}
uint64_t rapidhash(const void *key, size_t len) {
    return rapidhashBodyMain(key, len, 0, rapidhashDefaultSecret, false);
}

/* Main variant — PROTECTED */
uint64_t
rapidhashProtectedWithSecret(const void *key, size_t len, uint64_t seed,
                             const uint64_t secret[RAPIDHASH_SECRET_LEN]) {
    return rapidhashBodyMain(key, len, seed, secret, true);
}
uint64_t rapidhashProtectedWithSeed(const void *key, size_t len,
                                    uint64_t seed) {
    return rapidhashBodyMain(key, len, seed, rapidhashDefaultSecret, true);
}
uint64_t rapidhashProtected(const void *key, size_t len) {
    return rapidhashBodyMain(key, len, 0, rapidhashDefaultSecret, true);
}

/* Micro variant — FAST */
uint64_t rapidhashMicroWithSecret(const void *key, size_t len, uint64_t seed,
                                  const uint64_t secret[RAPIDHASH_SECRET_LEN]) {
    return rapidhashBodyMicro(key, len, seed, secret, false);
}
uint64_t rapidhashMicroWithSeed(const void *key, size_t len, uint64_t seed) {
    return rapidhashBodyMicro(key, len, seed, rapidhashDefaultSecret, false);
}
uint64_t rapidhashMicro(const void *key, size_t len) {
    return rapidhashBodyMicro(key, len, 0, rapidhashDefaultSecret, false);
}

/* Micro variant — PROTECTED */
uint64_t
rapidhashMicroProtectedWithSecret(const void *key, size_t len, uint64_t seed,
                                  const uint64_t secret[RAPIDHASH_SECRET_LEN]) {
    return rapidhashBodyMicro(key, len, seed, secret, true);
}
uint64_t rapidhashMicroProtectedWithSeed(const void *key, size_t len,
                                         uint64_t seed) {
    return rapidhashBodyMicro(key, len, seed, rapidhashDefaultSecret, true);
}
uint64_t rapidhashMicroProtected(const void *key, size_t len) {
    return rapidhashBodyMicro(key, len, 0, rapidhashDefaultSecret, true);
}

/* Nano variant — FAST */
uint64_t rapidhashNanoWithSecret(const void *key, size_t len, uint64_t seed,
                                 const uint64_t secret[RAPIDHASH_SECRET_LEN]) {
    return rapidhashBodyNano(key, len, seed, secret, false);
}
uint64_t rapidhashNanoWithSeed(const void *key, size_t len, uint64_t seed) {
    return rapidhashBodyNano(key, len, seed, rapidhashDefaultSecret, false);
}
uint64_t rapidhashNano(const void *key, size_t len) {
    return rapidhashBodyNano(key, len, 0, rapidhashDefaultSecret, false);
}

/* Nano variant — PROTECTED */
uint64_t
rapidhashNanoProtectedWithSecret(const void *key, size_t len, uint64_t seed,
                                 const uint64_t secret[RAPIDHASH_SECRET_LEN]) {
    return rapidhashBodyNano(key, len, seed, secret, true);
}
uint64_t rapidhashNanoProtectedWithSeed(const void *key, size_t len,
                                        uint64_t seed) {
    return rapidhashBodyNano(key, len, seed, rapidhashDefaultSecret, true);
}
uint64_t rapidhashNanoProtected(const void *key, size_t len) {
    return rapidhashBodyNano(key, len, 0, rapidhashDefaultSecret, true);
}

/* ============================================================ *
 * Streaming context                                             *
 * ============================================================ */

static inline __attribute__((always_inline)) void
rapidStreamBulkMain(rapidhashCtx *ctx, const uint8_t *p, bool prot) {
    /* One 7-lane main iteration over a single 112-byte stride. */
    const uint64_t *s = ctx->secret;
    ctx->lanes[0] = rapidMix(rapidRead64(p) ^ s[0],
                             rapidRead64(p + 8) ^ ctx->lanes[0], prot);
    ctx->lanes[1] = rapidMix(rapidRead64(p + 16) ^ s[1],
                             rapidRead64(p + 24) ^ ctx->lanes[1], prot);
    ctx->lanes[2] = rapidMix(rapidRead64(p + 32) ^ s[2],
                             rapidRead64(p + 40) ^ ctx->lanes[2], prot);
    ctx->lanes[3] = rapidMix(rapidRead64(p + 48) ^ s[3],
                             rapidRead64(p + 56) ^ ctx->lanes[3], prot);
    ctx->lanes[4] = rapidMix(rapidRead64(p + 64) ^ s[4],
                             rapidRead64(p + 72) ^ ctx->lanes[4], prot);
    ctx->lanes[5] = rapidMix(rapidRead64(p + 80) ^ s[5],
                             rapidRead64(p + 88) ^ ctx->lanes[5], prot);
    ctx->lanes[6] = rapidMix(rapidRead64(p + 96) ^ s[6],
                             rapidRead64(p + 104) ^ ctx->lanes[6], prot);
}

static inline __attribute__((always_inline)) void
rapidStreamBulkMicro(rapidhashCtx *ctx, const uint8_t *p, bool prot) {
    const uint64_t *s = ctx->secret;
    ctx->lanes[0] = rapidMix(rapidRead64(p) ^ s[0],
                             rapidRead64(p + 8) ^ ctx->lanes[0], prot);
    ctx->lanes[1] = rapidMix(rapidRead64(p + 16) ^ s[1],
                             rapidRead64(p + 24) ^ ctx->lanes[1], prot);
    ctx->lanes[2] = rapidMix(rapidRead64(p + 32) ^ s[2],
                             rapidRead64(p + 40) ^ ctx->lanes[2], prot);
    ctx->lanes[3] = rapidMix(rapidRead64(p + 48) ^ s[3],
                             rapidRead64(p + 56) ^ ctx->lanes[3], prot);
    ctx->lanes[4] = rapidMix(rapidRead64(p + 64) ^ s[4],
                             rapidRead64(p + 72) ^ ctx->lanes[4], prot);
}

static inline __attribute__((always_inline)) void
rapidStreamBulkNano(rapidhashCtx *ctx, const uint8_t *p, bool prot) {
    const uint64_t *s = ctx->secret;
    ctx->lanes[0] = rapidMix(rapidRead64(p) ^ s[0],
                             rapidRead64(p + 8) ^ ctx->lanes[0], prot);
    ctx->lanes[1] = rapidMix(rapidRead64(p + 16) ^ s[1],
                             rapidRead64(p + 24) ^ ctx->lanes[1], prot);
    ctx->lanes[2] = rapidMix(rapidRead64(p + 32) ^ s[2],
                             rapidRead64(p + 40) ^ ctx->lanes[2], prot);
}

/* Six specialized callbacks — picked once at Init, called per stride
 * flush.  Each one is a thin wrapper that pins both the variant lane
 * structure and the mum-mode (FAST/PROTECTED) so the always_inline body
 * compiles with `prot` as a constant and no per-stride runtime branch
 * survives. */
static void rapidStreamBulkMainFast(rapidhashCtx *ctx, const uint8_t *p) {
    rapidStreamBulkMain(ctx, p, false);
}
static void rapidStreamBulkMainProt(rapidhashCtx *ctx, const uint8_t *p) {
    rapidStreamBulkMain(ctx, p, true);
}
static void rapidStreamBulkMicroFast(rapidhashCtx *ctx, const uint8_t *p) {
    rapidStreamBulkMicro(ctx, p, false);
}
static void rapidStreamBulkMicroProt(rapidhashCtx *ctx, const uint8_t *p) {
    rapidStreamBulkMicro(ctx, p, true);
}
static void rapidStreamBulkNanoFast(rapidhashCtx *ctx, const uint8_t *p) {
    rapidStreamBulkNano(ctx, p, false);
}
static void rapidStreamBulkNanoProt(rapidhashCtx *ctx, const uint8_t *p) {
    rapidStreamBulkNano(ctx, p, true);
}

static rapidhashStreamBulkFn rapidStreamPickBulkFn(rapidhashVariant variant,
                                                   rapidhashMode mode) {
    const bool prot = (mode == RAPIDHASH_MODE_PROTECTED);
    switch (variant) {
    case RAPIDHASH_VARIANT_MAIN:
        return prot ? rapidStreamBulkMainProt : rapidStreamBulkMainFast;
    case RAPIDHASH_VARIANT_MICRO:
        return prot ? rapidStreamBulkMicroProt : rapidStreamBulkMicroFast;
    case RAPIDHASH_VARIANT_NANO:
        return prot ? rapidStreamBulkNanoProt : rapidStreamBulkNanoFast;
    }
    return rapidStreamBulkMainFast;
}

void rapidhashStreamInit(rapidhashCtx *ctx, rapidhashVariant variant,
                         rapidhashMode mode, uint64_t seed,
                         const uint64_t secret[RAPIDHASH_SECRET_LEN]) {
    if (secret == NULL) {
        secret = rapidhashDefaultSecret;
    }
    memset(ctx, 0, sizeof(*ctx));
    memcpy(ctx->secret, secret, sizeof(ctx->secret));
    ctx->variant = (uint8_t)variant;
    ctx->mode = (uint8_t)mode;
    ctx->initial_seed = seed;
    ctx->bulk_fn = rapidStreamPickBulkFn(variant, mode);
    switch (variant) {
    case RAPIDHASH_VARIANT_MAIN:
        ctx->stride = 112;
        break;
    case RAPIDHASH_VARIANT_MICRO:
        ctx->stride = 80;
        break;
    case RAPIDHASH_VARIANT_NANO:
        ctx->stride = 48;
        break;
    default:
        ctx->stride = 112;
        break;
    }
    /* Pre-mix the seed identically to the one-shot path. */
    const bool prot = (mode == RAPIDHASH_MODE_PROTECTED);
    seed ^= rapidMix(seed ^ ctx->secret[2], ctx->secret[1], prot);
    for (size_t i = 0; i < 7; i++) {
        ctx->lanes[i] = seed;
    }
}

/* Flush a full-stride window through the bulk mixer and remember its
 * trailing 16 bytes so Finalize can reconstruct upstream's backward
 * 16-byte read when the residual is < 16.  Dispatches through the
 * specialized callback installed at Init — no runtime branch on
 * variant/mode inside the per-stride hot loop. */
static inline __attribute__((always_inline)) void
rapidStreamFlush(rapidhashCtx *ctx, const uint8_t *p) {
    ctx->bulk_fn(ctx, p);
    ctx->bulk_engaged = 1;
    memcpy(ctx->bulk_tail, p + ctx->stride - 16, 16);
}

void rapidhashStreamUpdate(rapidhashCtx *ctx, const void *data, size_t len) {
    if (len == 0) {
        return;
    }
    const uint8_t *p = (const uint8_t *)data;
    ctx->total_len += len;
    const uint16_t stride = ctx->stride;

    /* If a previous Update left buf full at exactly `stride`, we deferred
     * its bulk-flush because we didn't yet know whether more bytes were
     * coming (the one-shot algorithm's `while (i > 112)` keeps the LAST
     * stride for the tail mixer, never for bulk).  Now that more bytes
     * have arrived, that buffered stride is definitively NOT the last —
     * flush it. */
    if (ctx->buf_len == stride) {
        rapidStreamFlush(ctx, ctx->buf);
        ctx->buf_len = 0;
    }

    /* Top up a partial buf to a full stride. */
    if (ctx->buf_len > 0) {
        uint16_t can_take = (uint16_t)(stride - ctx->buf_len);
        if (len <= can_take) {
            memcpy(ctx->buf + ctx->buf_len, p, len);
            ctx->buf_len = (uint16_t)(ctx->buf_len + len);
            return;
        }
        memcpy(ctx->buf + ctx->buf_len, p, can_take);
        p += can_take;
        len -= can_take;
        ctx->buf_len = stride;
        /* buf is full AND more data is coming -> flush. */
        rapidStreamFlush(ctx, ctx->buf);
        ctx->buf_len = 0;
    }

    /* Bulk-flush full strides directly from the input, but keep at
     * least one stride pending for Finalize to mix as the tail.  Hence
     * `len > stride` rather than `len >= stride`. */
    while (len > stride) {
        rapidStreamFlush(ctx, p);
        p += stride;
        len -= stride;
    }

    /* 1 <= len <= stride: stash for next Update or Finalize. */
    memcpy(ctx->buf, p, len);
    ctx->buf_len = (uint16_t)len;
}

/* Replays the tail mixer + final fold over `ctx->buf[0..buf_len)` after
 * any bulk strides have been consumed.  Mirrors the post-bulk path in
 * rapidhashBodyMain/Micro/Nano exactly.
 *
 * When buf_len < 16 (only possible if a bulk stride was flushed), the
 * final 16-byte read in the one-shot algorithm reaches BACKWARD into
 * the last bulk-consumed region.  We reconstruct that read by
 * splicing the trailing 16 bytes of the most recently flushed stride
 * (saved in ctx->bulk_tail) with the head of buf. */
static inline __attribute__((always_inline)) uint64_t
rapidStreamFinalTail(rapidhashCtx *ctx, bool prot) {
    const uint8_t *p = ctx->buf;
    size_t i = ctx->buf_len;
    uint64_t seed = ctx->lanes[0];
    const uint64_t *s = ctx->secret;
    const rapidhashVariant v = (rapidhashVariant)ctx->variant;

    /* Reduce lanes only if a bulk iteration was ever performed.  An
     * input <= stride bytes never engages bulk and skips this step. */
    if (ctx->bulk_engaged) {
        if (v == RAPIDHASH_VARIANT_MAIN) {
            seed ^= ctx->lanes[1];
            ctx->lanes[2] ^= ctx->lanes[3];
            ctx->lanes[4] ^= ctx->lanes[5];
            seed ^= ctx->lanes[6];
            ctx->lanes[2] ^= ctx->lanes[4];
            seed ^= ctx->lanes[2];
        } else if (v == RAPIDHASH_VARIANT_MICRO) {
            seed ^= ctx->lanes[1];
            ctx->lanes[2] ^= ctx->lanes[3];
            seed ^= ctx->lanes[4];
            seed ^= ctx->lanes[2];
        } else { /* NANO */
            seed ^= ctx->lanes[1];
            seed ^= ctx->lanes[2];
        }
    }

    /* 17..stride tail ladder.  Variant-specific depth. */
    if (i > 16) {
        seed = rapidMix(rapidRead64(p) ^ s[2], rapidRead64(p + 8) ^ seed, prot);
        if (i > 32) {
            seed = rapidMix(rapidRead64(p + 16) ^ s[2],
                            rapidRead64(p + 24) ^ seed, prot);
            if (v != RAPIDHASH_VARIANT_NANO && i > 48) {
                seed = rapidMix(rapidRead64(p + 32) ^ s[1],
                                rapidRead64(p + 40) ^ seed, prot);
                if (i > 64) {
                    seed = rapidMix(rapidRead64(p + 48) ^ s[1],
                                    rapidRead64(p + 56) ^ seed, prot);
                    if (v == RAPIDHASH_VARIANT_MAIN && i > 80) {
                        seed = rapidMix(rapidRead64(p + 64) ^ s[2],
                                        rapidRead64(p + 72) ^ seed, prot);
                        if (i > 96) {
                            seed = rapidMix(rapidRead64(p + 80) ^ s[1],
                                            rapidRead64(p + 88) ^ seed, prot);
                        }
                    }
                }
            }
        }
    }

    /* Reconstruct the final 16-byte read.  If i >= 16, the slice lives
     * entirely in buf; otherwise splice the tail of bulk_tail with the
     * head of buf to emulate the one-shot's backward read. */
    uint8_t final16[16];
    if (i >= 16) {
        memcpy(final16, p + i - 16, 16);
    } else {
        const size_t fromTail = 16 - i;
        memcpy(final16, ctx->bulk_tail + (16 - fromTail), fromTail);
        memcpy(final16 + fromTail, p, i);
    }
    uint64_t a = rapidRead64(final16) ^ i;
    uint64_t b = rapidRead64(final16 + 8);
    a ^= s[1];
    b ^= seed;
    rapidMum(&a, &b, prot);
    return rapidMix(a ^ s[7], b ^ s[1] ^ i, prot);
}

uint64_t rapidhashStreamFinal(rapidhashCtx *ctx) {
    const bool prot = (ctx->mode == RAPIDHASH_MODE_PROTECTED);
    const uint64_t *s = ctx->secret;
    uint64_t result;

    if (ctx->total_len <= 16) {
        /* Small-input one-shot path.  No lanes have been touched. */
        const uint8_t *p = ctx->buf;
        size_t len = (size_t)ctx->total_len;
        uint64_t seed = ctx->lanes[0]; /* pre-mixed during Init */
        uint64_t a = 0, b = 0;
        if (len >= 4) {
            seed ^= len;
            if (len >= 8) {
                a = rapidRead64(p);
                b = rapidRead64(p + len - 8);
            } else {
                a = rapidRead32(p);
                b = rapidRead32(p + len - 4);
            }
        } else if (len > 0) {
            a = (((uint64_t)p[0]) << 45) | p[len - 1];
            b = p[len >> 1];
        }
        a ^= s[1];
        b ^= seed;
        rapidMum(&a, &b, prot);
        result = rapidMix(a ^ s[7], b ^ s[1] ^ len, prot);
    } else {
        result = rapidStreamFinalTail(ctx, prot);
    }

    return result;
}

/* ============================================================ *
 * Secret derivation (port of upstream secret.h)                 *
 * ============================================================ */

static inline uint64_t wyrand(uint64_t *state) {
    *state += 0x2d358dccaa6c78a5ULL;
    return rapidMix(*state, *state ^ 0x8bb84b93962eacc9ULL, false);
}

static uint64_t mulMod(uint64_t a, uint64_t b, uint64_t m) {
    uint64_t r = 0;
    while (b) {
        if (b & 1) {
            uint64_t r2 = r + a;
            if (r2 < r) {
                r2 -= m;
            }
            r = r2 % m;
        }
        b >>= 1;
        if (b) {
            uint64_t a2 = a + a;
            if (a2 < a) {
                a2 -= m;
            }
            a = a2 % m;
        }
    }
    return r;
}

static uint64_t powMod(uint64_t a, uint64_t b, uint64_t m) {
    uint64_t r = 1;
    while (b) {
        if (b & 1) {
            r = mulMod(r, a, m);
        }
        b >>= 1;
        if (b) {
            a = mulMod(a, a, m);
        }
    }
    return r;
}

static int sprp(uint64_t n, uint64_t a) {
    uint64_t d = n - 1;
    unsigned s = 0;
    while (!(d & 0xff)) {
        d >>= 8;
        s += 8;
    }
    if (!(d & 0xf)) {
        d >>= 4;
        s += 4;
    }
    if (!(d & 0x3)) {
        d >>= 2;
        s += 2;
    }
    if (!(d & 0x1)) {
        d >>= 1;
        s += 1;
    }
    uint64_t b = powMod(a, d, n);
    if (b == 1 || b == n - 1) {
        return 1;
    }
    for (unsigned r = 1; r < s; r++) {
        b = mulMod(b, b, n);
        if (b <= 1) {
            return 0;
        }
        if (b == n - 1) {
            return 1;
        }
    }
    return 0;
}

static int isPrime(uint64_t n) {
    if (n < 2 || !(n & 1)) {
        return 0;
    }
    if (n < 4) {
        return 1;
    }
    static const uint64_t bases[] = {2,  3,  5,  7,  11, 13,
                                     17, 19, 23, 29, 31, 37};
    /* Deterministic for 64-bit n via Miller-Rabin with these 12 bases. */
    for (size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
        if (bases[i] >= n) {
            break;
        }
        if (!sprp(n, bases[i])) {
            return 0;
        }
    }
    return 1;
}

void rapidhashMakeSecret(uint64_t seed, uint64_t secret[RAPIDHASH_SECRET_LEN]) {
    /* Byte palette: each byte has popcount 4 (so a 64-bit secret built
     * from 8 such bytes has popcount 32 — guarantees balanced bits). */
    static const uint8_t palette[] = {
        15,  23,  27,  29,  30,  39,  43,  45,  46,  51,  53,  54,  57,  58,
        60,  71,  75,  77,  78,  83,  85,  86,  89,  90,  92,  99,  101, 102,
        105, 106, 108, 113, 114, 116, 120, 135, 139, 141, 142, 147, 149, 150,
        153, 154, 156, 163, 165, 166, 169, 170, 172, 177, 178, 180, 184, 195,
        197, 198, 201, 202, 204, 209, 210, 212, 216, 225, 226, 228, 232, 240};
    const size_t palette_len = sizeof(palette);

    for (size_t i = 0; i < 4; i++) {
        int ok;
        do {
            ok = 1;
            secret[i] = 0;
            for (size_t j = 0; j < 64; j += 8) {
                secret[i] |= ((uint64_t)palette[wyrand(&seed) % palette_len])
                             << j;
            }
            if ((secret[i] & 1ULL) == 0) {
                ok = 0;
                continue;
            }
            for (size_t j = 0; j < i; j++) {
                if (__builtin_popcountll(secret[j] ^ secret[i]) != 32) {
                    ok = 0;
                    break;
                }
            }
            if (ok && !isPrime(secret[i])) {
                ok = 0;
            }
        } while (!ok);
    }
    /* Fill slots 4..7 from the canonical default so callers get a
     * well-formed 8-word secret without needing to populate the tail. */
    for (size_t i = 4; i < RAPIDHASH_SECRET_LEN; i++) {
        secret[i] = rapidhashDefaultSecret[i];
    }
}

/* ============================================================ *
 * Tests                                                         *
 * ============================================================ */

#ifdef DATAKIT_TEST

#include "cleaner.h"
#include "ctest.h"
#include "datakit.h"
#include "hash/rapidhashTestVectors.h"

#include <inttypes.h>
#include <stdlib.h>

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleanerName(uint8_t *, zfree, U8);

/* Re-derive the deterministic input the upstream capture used: input
 * bytes are `(uint8_t)(i*131u + 17u)`, tagged "deterministic-fill-v1"
 * in the captured header. */
static void rapidhashFillDeterministic(uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(i * 131u + 17u);
    }
}

typedef uint64_t (*rapidhashFn)(const void *, size_t, uint64_t);

static const rapidhashFn fastFns[3] = {
    rapidhashWithSeed,      /* MAIN  -> field expected_rapidhash */
    rapidhashMicroWithSeed, /* MICRO -> field expected_micro */
    rapidhashNanoWithSeed,  /* NANO  -> field expected_nano */
};

static const rapidhashFn protectedFns[3] = {
    rapidhashProtectedWithSeed,
    rapidhashMicroProtectedWithSeed,
    rapidhashNanoProtectedWithSeed,
};

static const char *variantName(int idx) {
    static const char *names[3] = {"rapidhash", "rapidhashMicro",
                                   "rapidhashNano"};
    return names[idx];
}

static uint64_t expectedFor(const rapidhashTestVector *v, int idx) {
    switch (idx) {
    case 0:
        return v->expected_rapidhash;
    case 1:
        return v->expected_micro;
    default:
        return v->expected_nano;
    }
}

int rapidhashTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    /* Find the largest input we'll need so a single buffer covers
     * everything. */
    size_t maxLen = 0;
    for (size_t i = 0; i < rapidhashTestVectorsCount; i++) {
        if (rapidhashTestVectorsTable[i].input_len > maxLen) {
            maxLen = rapidhashTestVectorsTable[i].input_len;
        }
    }
    cleanerName(zfree, U8) uint8_t *buf = zmalloc(maxLen > 0 ? maxLen : 1);
    rapidhashFillDeterministic(buf, maxLen);

    TEST("known vectors: FAST one-shot, all three variants") {
        for (size_t i = 0; i < rapidhashTestVectorsCount; i++) {
            const rapidhashTestVector *v = &rapidhashTestVectorsTable[i];
            for (int idx = 0; idx < 3; idx++) {
                uint64_t got = fastFns[idx](buf, v->input_len, v->seed);
                uint64_t want = expectedFor(v, idx);
                if (got != want) {
                    ERR("%s %s: got 0x%016" PRIx64 " want 0x%016" PRIx64,
                        variantName(idx), v->name, got, want);
                }
            }
        }
    }

    TEST("known vectors: PROTECTED one-shot, all three variants") {
        for (size_t i = 0; i < rapidhashTestVectorsCount; i++) {
            const rapidhashTestVector *v =
                &rapidhashProtectedTestVectorsTable[i];
            for (int idx = 0; idx < 3; idx++) {
                uint64_t got = protectedFns[idx](buf, v->input_len, v->seed);
                uint64_t want = expectedFor(v, idx);
                if (got != want) {
                    ERR("PROTECTED %s %s: got 0x%016" PRIx64
                        " want 0x%016" PRIx64,
                        variantName(idx), v->name, got, want);
                }
            }
        }
    }

    /* Cap streaming chunk sizes by input length so the test stays under
     * a few seconds even with ASAN.  Tiny chunks vs. huge inputs would
     * produce hundreds of thousands of Update calls per vector. */
    const rapidhashVariant variants[3] = {RAPIDHASH_VARIANT_MAIN,
                                          RAPIDHASH_VARIANT_MICRO,
                                          RAPIDHASH_VARIANT_NANO};
    /* Chunk sizes selected to exercise every Update branch shape:
     *   1, 17, 113   — partial-top-up, mid-stride, just-over-Main-stride
     *   48           — exactly Nano stride (deferred-flush boundary)
     *   80           — exactly Micro stride (deferred-flush boundary)
     *   112          — exactly Main stride (deferred-flush boundary) */
    const size_t chunkSizes[] = {1, 17, 48, 80, 112, 113};
    const size_t nChunkSizes = sizeof(chunkSizes) / sizeof(chunkSizes[0]);

    TEST("streaming matches one-shot, FAST, all three variants") {
        for (size_t i = 0; i < rapidhashTestVectorsCount; i++) {
            const rapidhashTestVector *v = &rapidhashTestVectorsTable[i];
            for (int idx = 0; idx < 3; idx++) {
                uint64_t oneShot = fastFns[idx](buf, v->input_len, v->seed);
                for (size_t c = 0; c < nChunkSizes; c++) {
                    const size_t chunk = chunkSizes[c];
                    /* Bound total Update calls per (vector, chunk) ~4096. */
                    if (chunk == 1 && v->input_len > 1024) {
                        continue;
                    }
                    if (chunk == 17 && v->input_len > 8192) {
                        continue;
                    }
                    rapidhashCtx ctx;
                    rapidhashStreamInit(&ctx, variants[idx],
                                        RAPIDHASH_MODE_FAST, v->seed, NULL);
                    size_t off = 0;
                    while (off < v->input_len) {
                        size_t take = chunk;
                        if (take > v->input_len - off) {
                            take = v->input_len - off;
                        }
                        rapidhashStreamUpdate(&ctx, buf + off, take);
                        off += take;
                    }
                    uint64_t got = rapidhashStreamFinal(&ctx);
                    if (got != oneShot) {
                        ERR("stream %s chunk=%zu len=%zu seed=0x%" PRIx64
                            ": got 0x%016" PRIx64 " want 0x%016" PRIx64,
                            variantName(idx), chunk, v->input_len, v->seed, got,
                            oneShot);
                    }
                }
            }
        }
    }

    TEST("streaming matches one-shot, PROTECTED, all three variants") {
        for (size_t i = 0; i < rapidhashTestVectorsCount; i++) {
            const rapidhashTestVector *v = &rapidhashTestVectorsTable[i];
            for (int idx = 0; idx < 3; idx++) {
                uint64_t oneShot =
                    protectedFns[idx](buf, v->input_len, v->seed);
                for (size_t c = 0; c < nChunkSizes; c++) {
                    const size_t chunk = chunkSizes[c];
                    if (chunk == 1 && v->input_len > 1024) {
                        continue;
                    }
                    if (chunk == 17 && v->input_len > 8192) {
                        continue;
                    }
                    rapidhashCtx ctx;
                    rapidhashStreamInit(&ctx, variants[idx],
                                        RAPIDHASH_MODE_PROTECTED, v->seed,
                                        NULL);
                    size_t off = 0;
                    while (off < v->input_len) {
                        size_t take = chunk;
                        if (take > v->input_len - off) {
                            take = v->input_len - off;
                        }
                        rapidhashStreamUpdate(&ctx, buf + off, take);
                        off += take;
                    }
                    uint64_t got = rapidhashStreamFinal(&ctx);
                    if (got != oneShot) {
                        ERR("stream-PROTECTED %s chunk=%zu len=%zu "
                            "seed=0x%" PRIx64 ": got 0x%016" PRIx64
                            " want 0x%016" PRIx64,
                            variantName(idx), chunk, v->input_len, v->seed, got,
                            oneShot);
                    }
                }
            }
        }
    }

    TEST("dispatcher: AUTO / COMPACT / UNROLLED produce identical output") {
        /* All three modes must agree with the captured vector and with
         * each other for every input length and seed. */
        const rapidhashImpl modes[3] = {RAPIDHASH_IMPL_AUTO,
                                        RAPIDHASH_IMPL_COMPACT,
                                        RAPIDHASH_IMPL_UNROLLED};
        const rapidhashImpl saved = rapidhashGetForceImpl();
        for (size_t i = 0; i < rapidhashTestVectorsCount; i++) {
            const rapidhashTestVector *v = &rapidhashTestVectorsTable[i];
            uint64_t got[3];
            for (int m = 0; m < 3; m++) {
                rapidhashSetForceImpl(modes[m]);
                got[m] = rapidhashWithSeed(buf, v->input_len, v->seed);
            }
            if (got[0] != v->expected_rapidhash || got[1] != got[0] ||
                got[2] != got[0]) {
                ERR("dispatch len=%zu seed=0x%" PRIx64 ": AUTO=0x%016" PRIx64
                    " COMPACT=0x%016" PRIx64 " UNROLLED=0x%016" PRIx64
                    " want=0x%016" PRIx64,
                    v->input_len, v->seed, got[0], got[1], got[2],
                    v->expected_rapidhash);
            }
        }
        /* PROTECTED entry must also dispatch correctly. */
        for (size_t i = 0; i < rapidhashTestVectorsCount; i++) {
            const rapidhashTestVector *v =
                &rapidhashProtectedTestVectorsTable[i];
            uint64_t got[3];
            for (int m = 0; m < 3; m++) {
                rapidhashSetForceImpl(modes[m]);
                got[m] = rapidhashProtectedWithSeed(buf, v->input_len, v->seed);
            }
            if (got[0] != v->expected_rapidhash || got[1] != got[0] ||
                got[2] != got[0]) {
                ERR("dispatch-PROTECTED len=%zu seed=0x%" PRIx64
                    ": AUTO=0x%016" PRIx64 " COMPACT=0x%016" PRIx64
                    " UNROLLED=0x%016" PRIx64 " want=0x%016" PRIx64,
                    v->input_len, v->seed, got[0], got[1], got[2],
                    v->expected_rapidhash);
            }
        }
        rapidhashSetForceImpl(saved);
    }

    TEST("make_secret matches upstream") {
        const uint64_t *def = rapidhashGetDefaultSecret();
        for (size_t i = 0; i < rapidhashSecretTestVectorsCount; i++) {
            const rapidhashSecretTestVector *v =
                &rapidhashSecretTestVectorsTable[i];
            uint64_t got[RAPIDHASH_SECRET_LEN];
            rapidhashMakeSecret(v->seed, got);
            for (size_t j = 0; j < 4; j++) {
                if (got[j] != v->expected[j]) {
                    ERR("make_secret seed=0x%016" PRIx64 " word[%zu]: "
                        "got 0x%016" PRIx64 " want 0x%016" PRIx64,
                        v->seed, j, got[j], v->expected[j]);
                    break;
                }
            }
            /* Tail (slots 4..7) must come from the default secret — the
             * generator only populates the first 4 words. */
            for (size_t j = 4; j < RAPIDHASH_SECRET_LEN; j++) {
                if (got[j] != def[j]) {
                    ERR("make_secret seed=0x%016" PRIx64 " tail[%zu]: "
                        "got 0x%016" PRIx64 " want 0x%016" PRIx64 " (default)",
                        v->seed, j, got[j], def[j]);
                    break;
                }
            }
        }
    }

    TEST("dispatcher: rapidhashSetForceImpl does not affect Micro/Nano") {
        /* Force-impl knob applies ONLY to the Main variant.  Confirm
         * Micro and Nano produce their captured outputs regardless of
         * what FORCE knob is set. */
        const rapidhashImpl saved = rapidhashGetForceImpl();
        const rapidhashImpl modes[3] = {RAPIDHASH_IMPL_AUTO,
                                        RAPIDHASH_IMPL_COMPACT,
                                        RAPIDHASH_IMPL_UNROLLED};
        for (int m = 0; m < 3; m++) {
            rapidhashSetForceImpl(modes[m]);
            for (size_t i = 0; i < rapidhashTestVectorsCount; i++) {
                const rapidhashTestVector *v = &rapidhashTestVectorsTable[i];
                uint64_t mi =
                    rapidhashMicroWithSeed(buf, v->input_len, v->seed);
                uint64_t na = rapidhashNanoWithSeed(buf, v->input_len, v->seed);
                if (mi != v->expected_micro || na != v->expected_nano) {
                    ERR("force=%d affected micro/nano len=%zu: "
                        "micro=0x%016" PRIx64 " want 0x%016" PRIx64
                        " nano=0x%016" PRIx64 " want 0x%016" PRIx64,
                        modes[m], v->input_len, mi, v->expected_micro, na,
                        v->expected_nano);
                }
            }
        }
        rapidhashSetForceImpl(saved);
    }

    return err;
}

#endif /* DATAKIT_TEST */
