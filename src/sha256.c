/* SHA-256 (FIPS 180-4) — portable scalar implementation.
 *
 * Straight transcription of the spec: 64-round Merkle-Damgård compression
 * over 512-bit blocks with a 256-bit chaining state. Endian-safe input
 * decoding via byte-by-byte load; no unaligned-access assumptions and no
 * dependence on host byte order. */

#include "sha256.h"

#include "ctmem.h" /* dkSecureZero */
#include "dkC23.h" /* stdc_rotate_right */

#include <string.h>

/* ------------------------------------------------------------------------ */
/* Constants                                                                */
/* ------------------------------------------------------------------------ */

/* First 32 bits of fractional parts of cube roots of first 64 primes. */
static constexpr uint32_t K[64] = {
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

/* First 32 bits of fractional parts of square roots of first 8 primes. */
static constexpr uint32_t H0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                   0xa54ff53a, 0x510e527f, 0x9b05688c,
                                   0x1f83d9ab, 0x5be0cd19};

/* ------------------------------------------------------------------------ */
/* Inline helpers                                                           */
/* ------------------------------------------------------------------------ */

/* SHA-256 logical functions per FIPS 180-4 §4.1.2. */
static inline uint32_t Ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}
static inline uint32_t Maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}
static inline uint32_t BigSigma0(uint32_t x) {
    return stdc_rotate_right(x, 2) ^ stdc_rotate_right(x, 13) ^
           stdc_rotate_right(x, 22);
}
static inline uint32_t BigSigma1(uint32_t x) {
    return stdc_rotate_right(x, 6) ^ stdc_rotate_right(x, 11) ^
           stdc_rotate_right(x, 25);
}
static inline uint32_t smallSigma0(uint32_t x) {
    return stdc_rotate_right(x, 7) ^ stdc_rotate_right(x, 18) ^ (x >> 3);
}
static inline uint32_t smallSigma1(uint32_t x) {
    return stdc_rotate_right(x, 17) ^ stdc_rotate_right(x, 19) ^ (x >> 10);
}

static inline uint32_t loadBE32(const uint8_t *p) {
    /* Byte-by-byte: portable regardless of host endianness or alignment. */
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | ((uint32_t)p[3]);
}

static inline void storeBE32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline void storeBE64(uint8_t *p, uint64_t v) {
    p[0] = (uint8_t)(v >> 56);
    p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40);
    p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);
    p[7] = (uint8_t)v;
}

/* ------------------------------------------------------------------------ */
/* Compression                                                              */
/* ------------------------------------------------------------------------ */

#include "hash/sha256ArmCrypto.h"
#include "hash/sha256ShaNi.h"

/* Bulk compress dispatch.  Inline at the call site (no static function
 * pointer) so concurrent first-callers can't race on a lazy
 * initialization write.  The Available() probes return constants on
 * platforms where the compile-time feature flags fix the answer
 * (e.g. on aarch64 with __ARM_FEATURE_CRYPTO, sha256ArmCryptoAvailable
 * is a constant `return true`), so the compiler reduces this to a
 * single predicted branch — same effective cost as the old function
 * pointer indirection. */
static inline void sha256BulkCompress(uint32_t state[8], const uint8_t *blocks,
                                      size_t num_blocks) {
    if (sha256ArmCryptoAvailable()) {
        sha256ArmCryptoCompressBlocks(state, blocks, num_blocks);
        return;
    }
    if (sha256ShaNiAvailable()) {
        sha256ShaNiCompressBlocks(state, blocks, num_blocks);
        return;
    }
    sha256PortableCompressBlocks(state, blocks, num_blocks);
}

static void sha256Compress(uint32_t state[8], const uint8_t block[64]) {
    uint32_t W[64];
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t T1, T2;

    for (uint32_t i = 0; i < 16; i++) {
        W[i] = loadBE32(block + i * 4);
    }
    for (uint32_t i = 16; i < 64; i++) {
        W[i] = smallSigma1(W[i - 2]) + W[i - 7] + smallSigma0(W[i - 15]) +
               W[i - 16];
    }

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    f = state[5];
    g = state[6];
    h = state[7];

    for (uint32_t i = 0; i < 64; i++) {
        T1 = h + BigSigma1(e) + Ch(e, f, g) + K[i] + W[i];
        T2 = BigSigma0(a) + Maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + T1;
        d = c;
        c = b;
        b = a;
        a = T1 + T2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void sha256PortableCompressBlocks(uint32_t state[8], const uint8_t *blocks,
                                  size_t num_blocks) {
    for (size_t i = 0; i < num_blocks; i++) {
        sha256Compress(state, blocks + i * SHA256_BLOCK_LEN);
    }
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

void sha256Init(sha256Ctx *ctx) {
    ctx->bit_count = 0;
    ctx->buf_len = 0;
    memcpy(ctx->state, H0, sizeof(H0));
}

void sha256Update(sha256Ctx *ctx, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;

    /* Every ctx that sha256Init, sha256Update and sha256Final leave behind
     * has buf_len < SHA256_BLOCK_LEN: Init and Final zero it and Update
     * keeps at most SHA256_BLOCK_LEN - 1 leftover bytes.  Stating that here
     * bounds the partial-block copies for the compiler; it emits no code. */
    if (ctx->buf_len >= SHA256_BLOCK_LEN) {
        unreachable();
    }

    ctx->bit_count += (uint64_t)len * 8;

    /* Drain any partial block first. */
    if (ctx->buf_len > 0) {
        uint32_t need = SHA256_BLOCK_LEN - ctx->buf_len;
        if (len < need) {
            memcpy(ctx->buf + ctx->buf_len, p, len);
            ctx->buf_len += (uint32_t)len;
            return;
        }
        memcpy(ctx->buf + ctx->buf_len, p, need);
        sha256BulkCompress(ctx->state, ctx->buf, 1);
        p += need;
        len -= need;
        ctx->buf_len = 0;
    }

    /* Process complete blocks directly from the caller's buffer.  The
     * dispatched function may compress many blocks in a single call
     * (the ARM-crypto/SHA-NI paths amortize per-block overhead). */
    if (len >= SHA256_BLOCK_LEN) {
        const size_t nblocks = len / SHA256_BLOCK_LEN;
        sha256BulkCompress(ctx->state, p, nblocks);
        const size_t consumed = nblocks * SHA256_BLOCK_LEN;
        p += consumed;
        len -= consumed;
    }

    if (len > 0) {
        memcpy(ctx->buf, p, len);
        ctx->buf_len = (uint32_t)len;
    }
}

void sha256Final(sha256Ctx *ctx, uint8_t out[SHA256_DIGEST_LEN]) {
    uint64_t bit_count = ctx->bit_count;

    /* Append 0x80 then zero-pad so length lands in the last 8 bytes of a
     * 64-byte block. If we'd overflow this block, finalize it first. */
    ctx->buf[ctx->buf_len++] = 0x80;
    if (ctx->buf_len > 56) {
        memset(ctx->buf + ctx->buf_len, 0, SHA256_BLOCK_LEN - ctx->buf_len);
        sha256BulkCompress(ctx->state, ctx->buf, 1);
        ctx->buf_len = 0;
    }
    memset(ctx->buf + ctx->buf_len, 0, 56 - ctx->buf_len);
    storeBE64(ctx->buf + 56, bit_count);
    sha256BulkCompress(ctx->state, ctx->buf, 1);

    for (uint32_t i = 0; i < 8; i++) {
        storeBE32(out + i * 4, ctx->state[i]);
    }

    /* Wipe state to limit incidental leakage of intermediate values. */
    dkSecureZero(ctx, sizeof(*ctx));
}

void sha256Hash(const void *data, size_t len, uint8_t out[SHA256_DIGEST_LEN]) {
    sha256Ctx ctx;
    sha256Init(&ctx);
    sha256Update(&ctx, data, len);
    sha256Final(&ctx, out);
}

/* ------------------------------------------------------------------------ */
/* Test driver                                                              */
/* ------------------------------------------------------------------------ */

#ifdef DATAKIT_TEST

#include "cleaner.h"
#include "ctest.h"
#include "datakit.h"
#include "hash/sha256TestVectors.h"
#include "str.h" /* StrBytesToHex */
#include "timeUtil.h"

#include <stdlib.h>

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleanerName(uint8_t *, zfree, U8);

static int hashEqual(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, SHA256_DIGEST_LEN) == 0;
}

/* Report a digest mismatch and count it in the caller's *errCount (ERR
 * counts into the 'err' in scope, here a local) */
static void reportMismatch(const char *name, const uint8_t *got,
                           const uint8_t *expected, int *errCount) {
    char gotHex[65], expHex[65];
    StrBytesToHex(got, SHA256_DIGEST_LEN, gotHex);
    StrBytesToHex(expected, SHA256_DIGEST_LEN, expHex);
    int err = 0;
    ERR("vector '%s': got %s, expected %s", name, gotHex, expHex);
    *errCount += err;
}

int sha256Test(int, char *[]) {
    int err = 0;

    TEST("a reported mismatch counts as one error") {
        /* prints one intentional "ERROR!" line for the probe */
        const uint8_t zeros[SHA256_DIGEST_LEN] = {};
        const uint8_t ones[SHA256_DIGEST_LEN] = {1};
        int counted = 0;
        reportMismatch("probe: an intentional mismatch", zeros, ones, &counted);
        if (counted != 1) {
            ERR("reportMismatch counted %d errors, want 1", counted);
        }
    }

    TEST("known vectors") {
        for (size_t i = 0; i < SHA256_TEST_VECTOR_COUNT; i++) {
            const sha256TestVector *v = &sha256TestVectorsTable[i];
            uint8_t out[SHA256_DIGEST_LEN];
            sha256Hash(v->input, v->input_len, out);
            if (!hashEqual(out, v->expected)) {
                reportMismatch(v->name, out, v->expected, &err);
            }
        }
    }

    TEST("one million 'a's") {
        const size_t n = 1'000'000;
        cleanerName(zfree, U8) uint8_t *buf = zmalloc(n);
        memset(buf, 'a', n);
        uint8_t out[SHA256_DIGEST_LEN];
        sha256Hash(buf, n, out);
        static constexpr uint8_t expected[SHA256_DIGEST_LEN] = {
            0xcd, 0xc7, 0x6e, 0x5c, 0x99, 0x14, 0xfb, 0x92, 0x81, 0xa1, 0xc7,
            0xe2, 0x84, 0xd7, 0x3e, 0x67, 0xf1, 0x80, 0x9a, 0x48, 0xa4, 0x97,
            0x20, 0x0e, 0x04, 0x6d, 0x39, 0xcc, 0xc7, 0x11, 0x2c, 0xd0};
        if (!hashEqual(out, expected)) {
            reportMismatch("1M 'a's", out, expected, &err);
        }
    }

    TEST("boundary sizes") {
        static constexpr size_t sizes[] = {0,   1,   55,   56,   63,
                                           64,  65,  119,  120,  127,
                                           128, 129, 1023, 1024, 1025};
        const size_t nSizes = countof(sizes);
        const size_t maxSize = 1025;
        cleanerName(zfree, U8) uint8_t *buf = zmalloc(maxSize);
        /* Deterministic fill so streaming/one-shot use identical input. */
        for (size_t i = 0; i < maxSize; i++) {
            buf[i] = (uint8_t)((i * 31 + 7) & 0xff);
        }
        for (size_t i = 0; i < nSizes; i++) {
            size_t n = sizes[i];
            uint8_t oneShot[SHA256_DIGEST_LEN];
            uint8_t streamed[SHA256_DIGEST_LEN];
            sha256Hash(buf, n, oneShot);

            /* Randomized split points over multiple update calls. */
            sha256Ctx ctx;
            sha256Init(&ctx);
            size_t off = 0;
            while (off < n) {
                size_t remain = n - off;
                size_t chunk = (size_t)(rand() % 17) + 1;
                if (chunk > remain) {
                    chunk = remain;
                }
                sha256Update(&ctx, buf + off, chunk);
                off += chunk;
            }
            sha256Final(&ctx, streamed);

            if (!hashEqual(oneShot, streamed)) {
                char nameBuf[64];
                snprintf(nameBuf, sizeof(nameBuf), "stream@%zu", n);
                reportMismatch(nameBuf, streamed, oneShot, &err);
            }
        }
    }

    TEST("streaming agreement on random inputs") {
        srand(0xC0FFEE);
        for (uint32_t trial = 0; trial < 32; trial++) {
            size_t n = (size_t)(rand() % 4096);
            cleanerName(zfree, U8) uint8_t *buf = zmalloc(n + 1);
            for (size_t i = 0; i < n; i++) {
                buf[i] = (uint8_t)rand();
            }

            uint8_t oneShot[SHA256_DIGEST_LEN];
            uint8_t streamed[SHA256_DIGEST_LEN];
            sha256Hash(buf, n, oneShot);

            sha256Ctx ctx;
            sha256Init(&ctx);
            size_t off = 0;
            while (off < n) {
                size_t remain = n - off;
                size_t chunk = (size_t)(rand() % 257) + 1;
                if (chunk > remain) {
                    chunk = remain;
                }
                sha256Update(&ctx, buf + off, chunk);
                off += chunk;
            }
            sha256Final(&ctx, streamed);

            if (!hashEqual(oneShot, streamed)) {
                char nameBuf[64];
                snprintf(nameBuf, sizeof(nameBuf), "random trial %u n=%zu",
                         trial, n);
                reportMismatch(nameBuf, streamed, oneShot, &err);
            }
        }
    }

    TEST("performance bench") {
        /* Throughput on a moderate buffer. Never fails. */
        const size_t bufSize = 1 << 20; /* 1 MiB */
        const uint32_t iters = 64;      /* total 64 MiB */
        cleanerName(zfree, U8) uint8_t *buf = zmalloc(bufSize);
        memset(buf, 0xa5, bufSize);

        uint8_t out[SHA256_DIGEST_LEN];
        sha256Ctx ctx;
        sha256Init(&ctx);

        const double t0 = timeUtilMonotonicSec();
        for (uint32_t i = 0; i < iters; i++) {
            sha256Update(&ctx, buf, bufSize);
        }
        sha256Final(&ctx, out);
        double secs = timeUtilMonotonicSec() - t0;
        double mib = (double)((uint64_t)bufSize * iters) / (1024.0 * 1024.0);
        printf("    sha256: %.2f MiB in %.3fs = %.2f MiB/s\n", mib, secs,
               secs > 0 ? mib / secs : 0.0);
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
