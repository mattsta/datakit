/* perfRegression — see perfRegression.h for the contract.
 *
 * Each `bench*` function below times a single hot path against a
 * fixed-size payload, prints one stable TSV row, and moves on.
 * Iterations scale inversely with payload size so the total wall
 * time per row stays small (~50-200 ms) and the suite finishes
 * within a CI step budget. */

#include "perfRegression.h"

#ifdef DATAKIT_TEST

#include "blake3.h"
#include "chacha20poly1305.h"
#include "clusterMailbox.h"
#include "ctest.h"
#include "datakit.h"
#include "flexEnvelope.h"
#include "hash/sha256ArmCrypto.h"
#include "hmac.h"
#include "hmacKeyring.h"
#include "membound.h"
#include "rapidhash.h"
#include "sha256.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define PERF_REG_KEY_LEN ((size_t)32)
#define PERF_REG_NONCE_LEN ((size_t)12)

static double monoNowSec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void fillDeterministic(uint8_t *buf, size_t len, uint64_t seed) {
    uint64_t s = seed | 1u;
    for (size_t i = 0; i < len; i++) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        buf[i] = (uint8_t)(s >> 56);
    }
}

/* Pick an iteration count so the timed window targets ~100 ms. */
static uint32_t pickIters(size_t bytes_per_op) {
    if (bytes_per_op == 0) {
        return 100000u;
    }
    if (bytes_per_op <= 64) {
        return 200000u;
    }
    if (bytes_per_op <= 1024) {
        return 50000u;
    }
    if (bytes_per_op <= 65536) {
        return 2000u;
    }
    return 200u;
}

static void emitRow(const char *module, const char *name, size_t bytes,
                    uint32_t iters, double secs) {
    const double ns_per_op = (iters > 0) ? (secs * 1e9 / (double)iters) : 0.0;
    const double mb_per_sec =
        (secs > 0 && bytes > 0)
            ? ((double)bytes * (double)iters / (secs * 1024.0 * 1024.0))
            : 0.0;
    printf("PERF\t%s\t%s\t%zu\t%u\t%.1f\t%.1f\n", module, name, bytes, iters,
           ns_per_op, mb_per_sec);
}

/* Anti-DCE sink: forces the optimiser to keep the output buffer live
 * across the timed loop.  Reading the first and last byte through a
 * never-taken branch makes the result observable without spending real
 * cycles on the printf.  Used by every bench that writes into a stack
 * buffer the rest of the function never reads. */
static void antiDce(const uint8_t *buf, size_t len) {
    if (len == 0) {
        return;
    }
    if ((buf[0] | buf[len - 1u]) == 0xFFu &&
        (buf[0] & buf[len - 1u]) == 0xFFu) {
        /* Cryptographic outputs are random — both conditions firing on
         * the same call is astronomically improbable.  The compiler
         * cannot prove it though, so it must materialise the load. */
        printf("# anti-DCE sentinel hit (probability ~2^-16)\n");
    }
}

/* ---- BLAKE3 plain hash, varying payload --------------------------- */
static void benchBlake3(size_t bytes) {
    uint8_t *buf = zmalloc(bytes ? bytes : 1u);
    fillDeterministic(buf, bytes, 0xB1u);
    uint8_t out[BLAKE3_OUT_LEN];
    const uint32_t iters = pickIters(bytes);
    const double t0 = monoNowSec();
    for (uint32_t i = 0; i < iters; i++) {
        blake3Hash(buf, bytes, out);
    }
    const double secs = monoNowSec() - t0;
    emitRow("blake3", "hash", bytes, iters, secs);
    antiDce(out, sizeof(out));
    zfree(buf);
}

/* ---- SHA-256 (auto-dispatched to ARM crypto on Apple Silicon) ----- */
static void benchSha256(size_t bytes) {
    uint8_t *buf = zmalloc(bytes ? bytes : 1u);
    fillDeterministic(buf, bytes, 0xA7u);
    uint8_t out[SHA256_DIGEST_LEN];
    const uint32_t iters = pickIters(bytes);
    const double t0 = monoNowSec();
    for (uint32_t i = 0; i < iters; i++) {
        sha256Hash(buf, bytes, out);
    }
    const double secs = monoNowSec() - t0;
    emitRow("sha256", "hash", bytes, iters, secs);
    antiDce(out, sizeof(out));
    zfree(buf);
}

/* ---- xxHash: XXH64 / XXH3-64 / XXH3-128 (deps/xxHash) ----------- */

static void benchXxh64(size_t bytes) {
    uint8_t *buf = zmalloc(bytes ? bytes : 1u);
    fillDeterministic(buf, bytes, 0x64u);
    const uint32_t iters = pickIters(bytes);
    volatile uint64_t sink = 0;
    const double t0 = monoNowSec();
    for (uint32_t i = 0; i < iters; i++) {
        sink ^= XXH64(buf, bytes, 0);
    }
    const double secs = monoNowSec() - t0;
    emitRow("xxh64", "hash", bytes, iters, secs);
    (void)sink;
    zfree(buf);
}

static void benchXxh3_64(size_t bytes) {
    uint8_t *buf = zmalloc(bytes ? bytes : 1u);
    fillDeterministic(buf, bytes, 0x33u);
    const uint32_t iters = pickIters(bytes);
    volatile uint64_t sink = 0;
    const double t0 = monoNowSec();
    for (uint32_t i = 0; i < iters; i++) {
        sink ^= XXH3_64bits(buf, bytes);
    }
    const double secs = monoNowSec() - t0;
    emitRow("xxh3_64", "hash", bytes, iters, secs);
    (void)sink;
    zfree(buf);
}

static void benchXxh3_128(size_t bytes) {
    uint8_t *buf = zmalloc(bytes ? bytes : 1u);
    fillDeterministic(buf, bytes, 0x77u);
    const uint32_t iters = pickIters(bytes);
    volatile uint64_t sink = 0;
    const double t0 = monoNowSec();
    for (uint32_t i = 0; i < iters; i++) {
        XXH128_hash_t h = XXH3_128bits(buf, bytes);
        sink ^= h.low64 ^ h.high64;
    }
    const double secs = monoNowSec() - t0;
    emitRow("xxh3_128", "hash", bytes, iters, secs);
    (void)sink;
    zfree(buf);
}

/* ---- rapidhash: three variants × two modes ---------------------- */

typedef uint64_t (*rapidhashOneShot)(const void *, size_t);

static void benchRapidhashOneShot(const char *label, rapidhashOneShot fn,
                                  size_t bytes) {
    uint8_t *buf = zmalloc(bytes ? bytes : 1u);
    fillDeterministic(buf, bytes, 0xD3u);
    const uint32_t iters = pickIters(bytes);
    volatile uint64_t sink = 0;
    const double t0 = monoNowSec();
    for (uint32_t i = 0; i < iters; i++) {
        sink ^= fn(buf, bytes);
    }
    const double secs = monoNowSec() - t0;
    emitRow("rapidhash", label, bytes, iters, secs);
    (void)sink;
    zfree(buf);
}

/* Streaming: 64-byte chunks across the input.  This represents the
 * realistic "ingest from a network/disk reader" cost, not the
 * absolute best-case (one Update covers everything). */
static void benchRapidhashStream(const char *label, rapidhashVariant variant,
                                 rapidhashMode mode, size_t bytes) {
    uint8_t *buf = zmalloc(bytes ? bytes : 1u);
    fillDeterministic(buf, bytes, 0xD7u);
    const size_t chunk = 64;
    const uint32_t iters = pickIters(bytes);
    volatile uint64_t sink = 0;
    const double t0 = monoNowSec();
    for (uint32_t i = 0; i < iters; i++) {
        rapidhashCtx ctx;
        rapidhashStreamInit(&ctx, variant, mode, 0, NULL);
        size_t off = 0;
        while (off < bytes) {
            size_t take = chunk;
            if (take > bytes - off) {
                take = bytes - off;
            }
            rapidhashStreamUpdate(&ctx, buf + off, take);
            off += take;
        }
        sink ^= rapidhashStreamFinal(&ctx);
    }
    const double secs = monoNowSec() - t0;
    emitRow("rapidhash", label, bytes, iters, secs);
    (void)sink;
    zfree(buf);
}

/* ---- SHA-256 4-lane multi-buffer (ARM crypto fast path) ---------- */
static void benchSha256Multi4(size_t bytes_per_lane) {
    /* Round down to a whole number of 64-byte blocks so the multi-buffer
     * entry point sees identical work per lane. */
    const size_t nb = bytes_per_lane / SHA256_BLOCK_LEN;
    const size_t actual = nb * SHA256_BLOCK_LEN;
    uint8_t *bufs[4];
    for (uint32_t lane = 0; lane < 4; lane++) {
        bufs[lane] = zmalloc(actual ? actual : 1u);
        fillDeterministic(bufs[lane], actual, 0xC1u + lane);
    }
    uint32_t st[4][8];
    const uint32_t h0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    for (uint32_t lane = 0; lane < 4; lane++) {
        memcpy(st[lane], h0, sizeof(h0));
    }
    const uint8_t *bptrs[4] = {bufs[0], bufs[1], bufs[2], bufs[3]};

    const uint32_t iters = pickIters(bytes_per_lane);
    const double t0 = monoNowSec();
    for (uint32_t i = 0; i < iters; i++) {
        sha256ArmCryptoCompressBlocksMulti4(st, bptrs, nb);
    }
    const double secs = monoNowSec() - t0;

    /* Report aggregate throughput: bytes processed across all 4 lanes
     * per iteration. */
    emitRow("sha256", "multi4", actual * 4u, iters, secs);

    /* Anti-DCE. */
    if ((st[0][0] | st[3][7]) == 0xFFFFFFFF) {
        printf("# anti-DCE multi4 %08x\n", st[0][0]);
    }

    for (uint32_t lane = 0; lane < 4; lane++) {
        zfree(bufs[lane]);
    }
}

/* ---- HMAC-SHA-256 plain (per-call key derivation) ----------------- */
static void benchHmacSha256Plain(size_t bytes) {
    uint8_t key[PERF_REG_KEY_LEN];
    fillDeterministic(key, sizeof(key), 0xC0u);
    uint8_t *buf = zmalloc(bytes ? bytes : 1u);
    fillDeterministic(buf, bytes, 0x42u);
    uint8_t out[32];
    const uint32_t iters = pickIters(bytes);
    const double t0 = monoNowSec();
    for (uint32_t i = 0; i < iters; i++) {
        hmacCompute(&hmacAlgoSHA256, key, sizeof(key), buf, bytes, out);
    }
    const double secs = monoNowSec() - t0;
    emitRow("hmac", "sha256_plain", bytes, iters, secs);
    antiDce(out, sizeof(out));
    zfree(buf);
}

/* ---- HMAC-SHA-256 with prepared key (the hot path) ---------------- */
static void benchHmacSha256Prepared(size_t bytes) {
    uint8_t key[PERF_REG_KEY_LEN];
    fillDeterministic(key, sizeof(key), 0xC0u);
    uint8_t *buf = zmalloc(bytes ? bytes : 1u);
    fillDeterministic(buf, bytes, 0x42u);
    uint8_t prep_storage[2048];
    hmacKey prep;
    hmacPrepareKey(&prep, &hmacAlgoSHA256, prep_storage, sizeof(prep_storage),
                   key, sizeof(key));
    uint8_t out[32];
    const uint32_t iters = pickIters(bytes);
    const double t0 = monoNowSec();
    for (uint32_t i = 0; i < iters; i++) {
        hmacComputeWithKey(&prep, buf, bytes, out);
    }
    const double secs = monoNowSec() - t0;
    emitRow("hmac", "sha256_prepared", bytes, iters, secs);
    antiDce(out, sizeof(out));
    zfree(buf);
}

/* ---- flexEnvelope wrap+verify under each algorithm ---------------- */
static void benchEnvelope(flexEnvelopeAlgo algo, const char *algo_label,
                          size_t bytes) {
    uint8_t key[PERF_REG_KEY_LEN];
    fillDeterministic(key, sizeof(key), 0xE1u);
    uint8_t *payload = zmalloc(bytes ? bytes : 1u);
    fillDeterministic(payload, bytes, 0x91u);
    const bool keyed = flexEnvelopeAlgoIsKeyed(algo);
    flexEnvelopeWrapParams wp = {
        .algo = algo,
        .key = keyed ? key : NULL,
        .key_len = keyed ? sizeof(key) : 0,
    };
    const size_t total =
        flexEnvelopeWrappedSize((uint64_t)bytes, algo, FLEX_ENV_FLAG_NONE);
    uint8_t *wrapped = zmalloc(total);
    size_t written = 0;
    flexEnvelopeWrap(wrapped, total, payload, (uint64_t)bytes, &wp, &written);

    flexEnvelopeVerifyParams vp = {
        .key = keyed ? key : NULL,
        .key_len = keyed ? sizeof(key) : 0,
    };
    const uint32_t iters = pickIters(bytes);

    /* Wrap timing. */
    const double t_w0 = monoNowSec();
    for (uint32_t i = 0; i < iters; i++) {
        flexEnvelopeWrap(wrapped, total, payload, (uint64_t)bytes, &wp,
                         &written);
    }
    const double secs_wrap = monoNowSec() - t_w0;
    char label[64];
    snprintf(label, sizeof(label), "%s_wrap", algo_label);
    emitRow("envelope", label, bytes, iters, secs_wrap);

    /* Verify timing (raw key path — every call re-derives K_ipad/K_opad). */
    const uint8_t *last_out_p = NULL;
    uint64_t last_out_pl = 0;
    const double t_v0 = monoNowSec();
    for (uint32_t i = 0; i < iters; i++) {
        flexEnvelopeVerify(wrapped, total, &vp, &last_out_p, &last_out_pl);
    }
    const double secs_verify = monoNowSec() - t_v0;
    snprintf(label, sizeof(label), "%s_verify", algo_label);
    emitRow("envelope", label, bytes, iters, secs_verify);

    /* Prepared-key verify (HMAC class only — BLAKE3-keyed has no
     * prepared form; checksum class ignores the field).  This row
     * shows the K_ipad/K_opad savings that the cluster apply mailbox
     * and persist WAL replay paths consume via the prepared-key fast
     * path. */
    if (algo == FLEX_ENV_ALGO_HMAC_SHA256 ||
        algo == FLEX_ENV_ALGO_HMAC_BLAKE3) {
        const hmacHashAlgo *hash_algo = (algo == FLEX_ENV_ALGO_HMAC_BLAKE3)
                                            ? &hmacAlgoBLAKE3
                                            : &hmacAlgoSHA256;
        uint8_t prep_storage[4096];
        hmacKey prep;
        hmacPrepareKey(&prep, hash_algo, prep_storage, sizeof(prep_storage),
                       key, sizeof(key));
        flexEnvelopeVerifyParams vp_prep = {
            .prepared_key = &prep,
        };
        const double t_vp0 = monoNowSec();
        for (uint32_t i = 0; i < iters; i++) {
            flexEnvelopeVerify(wrapped, total, &vp_prep, &last_out_p,
                               &last_out_pl);
        }
        const double secs_vp = monoNowSec() - t_vp0;
        snprintf(label, sizeof(label), "%s_verify_prep", algo_label);
        emitRow("envelope", label, bytes, iters, secs_vp);
    }

    /* anti-DCE: ensure the verify path's output is consumed.  Reading
     * one byte from the returned payload pointer forces the compiler
     * to actually compute the digest comparison. */
    if (last_out_p != NULL && last_out_pl > 0u) {
        antiDce(last_out_p, 1u);
    }
    /* The wrapped buffer is regenerated by the wrap loop above; sink
     * its trailing digest so the wrap loop's writes are observed. */
    antiDce(wrapped + total - 1u, 1u);

    zfree(payload);
    zfree(wrapped);
}

/* ---- ChaCha20-Poly1305 AEAD seal+open ---------------------------- */
static void benchAead(size_t bytes) {
    uint8_t key[PERF_REG_KEY_LEN];
    uint8_t nonce[PERF_REG_NONCE_LEN];
    fillDeterministic(key, sizeof(key), 0xAEu);
    fillDeterministic(nonce, sizeof(nonce), 0xADu);
    uint8_t *plain = zmalloc(bytes ? bytes : 1u);
    fillDeterministic(plain, bytes, 0x77u);
    uint8_t *ct = zmalloc(bytes ? bytes : 1u);
    uint8_t tag[CHACHA20_POLY1305_TAG_LEN];
    const uint32_t iters = pickIters(bytes);

    const double t_s0 = monoNowSec();
    for (uint32_t i = 0; i < iters; i++) {
        (void)chacha20poly1305Seal(key, nonce, NULL, 0u, plain, bytes, ct, tag);
    }
    const double secs_seal = monoNowSec() - t_s0;
    emitRow("aead", "chacha20poly1305_seal", bytes, iters, secs_seal);
    antiDce(ct, bytes > 0 ? 1u : 0u);
    antiDce(tag, sizeof(tag));

    const double t_o0 = monoNowSec();
    for (uint32_t i = 0; i < iters; i++) {
        (void)chacha20poly1305Open(key, nonce, NULL, 0u, ct, bytes, tag, plain);
    }
    const double secs_open = monoNowSec() - t_o0;
    emitRow("aead", "chacha20poly1305_open", bytes, iters, secs_open);
    antiDce(plain, bytes > 0 ? 1u : 0u);

    zfree(plain);
    zfree(ct);
}

/* ---- clusterMailbox seal + ingest end-to-end --------------------- */
static void benchMailbox(size_t bytes) {
    uint8_t key[PERF_REG_KEY_LEN];
    fillDeterministic(key, sizeof(key), 0xB0u);
    hmacKeyring *ring = hmacKeyringCreate();
    const hmacKeyId K = 1u;
    hmacKeyringInstallUnsalted(ring, K, &hmacAlgoSHA256, key, sizeof(key));
    clusterMailbox *mb =
        clusterMailboxCreate(ring, FLEX_ENV_ALGO_HMAC_SHA256, NULL, NULL);
    uint8_t *payload = zmalloc(bytes ? bytes : 1u);
    fillDeterministic(payload, bytes, 0x31u);
    const size_t sealed_sz = clusterMailboxSealedSize(mb, bytes);
    uint8_t *envelope = zmalloc(sealed_sz);
    /* Seal once to size + warm caches. */
    size_t written = 0;
    clusterMailboxSeal(mb, 1u, 1u, 0u, payload, bytes, K, envelope, sealed_sz,
                       &written);

    const uint32_t iters = pickIters(bytes);

    /* Seal timing. */
    const double t_s0 = monoNowSec();
    for (uint32_t i = 0; i < iters; i++) {
        clusterMailboxSeal(mb, 1u, (uint64_t)i + 2u, 0u, payload, bytes, K,
                           envelope, sealed_sz, &written);
    }
    const double secs_seal = monoNowSec() - t_s0;
    emitRow("mailbox", "seal", bytes, iters, secs_seal);

    /* Ingest timing — measure the steady-state verify+parse+apply
     * cost, not mailbox construction.  Pre-build N envelopes with
     * monotonic sequence numbers OUTSIDE the timing window, then
     * stream them through Ingest one after the other so each call
     * walks the same code path (lookupOrigin hit, replay check pass,
     * watermark advance, no on_apply callback configured). */
    {
        /* Pre-build a batch of envelopes (each a different sequence
         * number on origin 1).  Cap the batch so we don't blow up
         * memory at large `bytes`. */
        const uint32_t batch_n = (bytes <= 1024u) ? 256u : 32u;
        uint8_t **envelopes = zmalloc(batch_n * sizeof(uint8_t *));
        for (uint32_t i = 0; i < batch_n; i++) {
            envelopes[i] = zmalloc(sealed_sz);
            size_t w = 0;
            clusterMailboxSeal(mb, /* origin */ 2u, /* seq */ (uint64_t)i + 1u,
                               0u, payload, bytes, K, envelopes[i], sealed_sz,
                               &w);
        }
        /* Fresh mailbox so origin 2's watermark starts at 0 and the
         * batch sequence 1..N walks monotonically. */
        clusterMailboxFree(mb);
        mb = clusterMailboxCreate(ring, FLEX_ENV_ALGO_HMAC_SHA256, NULL, NULL);

        /* Round iter count down to a multiple of batch_n so every
         * pass sees a complete batch sweep.  Replenish the watermark
         * by rebuilding the mailbox ONLY between full batches —
         * amortized cost is one Free+Create per batch_n Ingests, not
         * per Ingest. */
        const uint32_t passes = iters / batch_n;
        const uint32_t real_iters = passes * batch_n;
        const double t_i0 = monoNowSec();
        for (uint32_t p = 0; p < passes; p++) {
            for (uint32_t i = 0; i < batch_n; i++) {
                /* `written` came from the very first Seal (origin=1,
                 * seq=1, same payload+algo) so it equals each envelope's
                 * own length.  This is safe because
                 * flexEnvelopeWrappedSize is purely a function of
                 * (payload_len, algo, on_wire_flags), which are
                 * identical across all envelopes in the batch.  If a
                 * future Seal variant adds variable-width framing
                 * (e.g. per-envelope nonce-len) this assumption
                 * breaks. */
                (void)clusterMailboxIngest(mb, envelopes[i], written, K);
            }
            /* Reset between passes so the next batch's seq=1 isn't
             * REPLAY-rejected.  This Free+Create happens once per
             * `batch_n` ingests, not per ingest, so the per-op
             * overhead is amortised to near-zero. */
            clusterMailboxFree(mb);
            mb = clusterMailboxCreate(ring, FLEX_ENV_ALGO_HMAC_SHA256, NULL,
                                      NULL);
        }
        const double secs_ingest = monoNowSec() - t_i0;
        emitRow("mailbox", "ingest", bytes, real_iters > 0 ? real_iters : 1u,
                secs_ingest);

        for (uint32_t i = 0; i < batch_n; i++) {
            zfree(envelopes[i]);
        }
        zfree(envelopes);
    }

    zfree(payload);
    zfree(envelope);
    clusterMailboxFree(mb);
    hmacKeyringFree(ring);
}

/* ---- membound allocator: ns per alloc+free pair ------------------- */
/* Single-threaded and deterministic by design: the pool is pre-created
 * outside the timed window and one block is recycled, so the row is a
 * stable sentinel for per-op alloc/free cost (it would have caught the
 * ~1.5 ns regression from giving the per-extent counters seq_cst order).
 * The multithreaded allocator comparison lives in `memboundBench` (too
 * scheduling-noisy for a regression gate). "op" here = one alloc+free. */
static void benchMembound(const char *name, membound *pool, size_t bytes) {
    const uint32_t iters = 2000000u;
    volatile void *sink = NULL;
    const double t0 = monoNowSec();
    for (uint32_t i = 0; i < iters; i++) {
        void *p = memboundAlloc(pool, bytes);
        sink = p;
        memboundFree(pool, p);
    }
    const double secs = monoNowSec() - t0;
    emitRow("membound", name, bytes, iters, secs);
    (void)sink;
}

int perfRegressionTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    /* The TEST() macro wrappers make this discoverable through the
     * normal `datakit-test test perfRegression` invocation and let CI
     * fail the run if a bench routine crashes (UBSAN/ASAN aware).  Each
     * TEST block emits its own rows; CI scrapes lines starting with
     * "PERF\t". */
    TEST("perf baseline rows") {
        /* Comment-prefixed header — CI scrapers grep for "^PERF\t"
         * data rows and ignore "^#" comment lines (TSV convention). */
        printf("# PERF\tmodule\tname\tbytes\titers\tns_per_op\tmb_per_sec\n");

        /* Sizes shared by blake3/sha256/xxh64/xxh3 benches.  Includes
         * 16384 and 65536 so the same rows exist in sizes_rh (above)
         * and direct row-by-row diffs against rapidhash are trivial. */
        const size_t sizes_hash[] = {64u, 1024u, 16384u, 65536u, 1048576u};
        const uint32_t n_sizes =
            (uint32_t)(sizeof(sizes_hash) / sizeof(sizes_hash[0]));

        for (uint32_t i = 0; i < n_sizes; i++) {
            benchBlake3(sizes_hash[i]);
        }
        for (uint32_t i = 0; i < n_sizes; i++) {
            benchSha256(sizes_hash[i]);
        }
        for (uint32_t i = 0; i < n_sizes; i++) {
            benchXxh64(sizes_hash[i]);
        }
        for (uint32_t i = 0; i < n_sizes; i++) {
            benchXxh3_64(sizes_hash[i]);
        }
        for (uint32_t i = 0; i < n_sizes; i++) {
            benchXxh3_128(sizes_hash[i]);
        }

        /* rapidhash family: covers every bulk-threshold boundary so the
         * crossover between Nano (48 B stride), Micro (80 B stride), and
         * Main (112 B stride) is visible in the row data. */
        /* Sizes pinned to bulk-threshold boundaries AND threshold+1 so
         * each variant's first bulk iteration is timed in isolation:
         *   - Nano  bulk fires at len > 48
         *   - Micro bulk fires at len > 80
         *   - Main  bulk fires at len > 112
         *   - AUTO  flips to UNROLLED at len > 224 */
        const size_t sizes_rh[] = {
            8u,   16u,  32u,  48u,  49u,  64u,   80u,    81u,    96u,     112u,
            113u, 128u, 224u, 225u, 256u, 1024u, 16384u, 65536u, 1048576u};
        const uint32_t n_rh =
            (uint32_t)(sizeof(sizes_rh) / sizeof(sizes_rh[0]));
        for (uint32_t i = 0; i < n_rh; i++) {
            benchRapidhashOneShot("main", rapidhash, sizes_rh[i]);
            benchRapidhashOneShot("micro", rapidhashMicro, sizes_rh[i]);
            benchRapidhashOneShot("nano", rapidhashNano, sizes_rh[i]);
        }
        /* Forced-impl rows: lock the Main dispatcher to one body so the
         * COMPACT-vs-UNROLLED crossover is visible.  AUTO is what the
         * default `main` rows above already exercise. */
        const rapidhashImpl saved_impl = rapidhashGetForceImpl();
        rapidhashSetForceImpl(RAPIDHASH_IMPL_COMPACT);
        for (uint32_t i = 0; i < n_rh; i++) {
            benchRapidhashOneShot("main-compact", rapidhash, sizes_rh[i]);
        }
        rapidhashSetForceImpl(RAPIDHASH_IMPL_UNROLLED);
        for (uint32_t i = 0; i < n_rh; i++) {
            benchRapidhashOneShot("main-unrolled", rapidhash, sizes_rh[i]);
        }
        rapidhashSetForceImpl(saved_impl);
        for (uint32_t i = 0; i < n_rh; i++) {
            benchRapidhashOneShot("main-protected", rapidhashProtected,
                                  sizes_rh[i]);
            benchRapidhashOneShot("micro-protected", rapidhashMicroProtected,
                                  sizes_rh[i]);
            benchRapidhashOneShot("nano-protected", rapidhashNanoProtected,
                                  sizes_rh[i]);
        }
        /* Streaming with 64 B chunks: most useful at sizes large enough
         * that the bulk path is actually exercised. */
        const size_t sizes_rh_stream[] = {1024u, 16384u, 1048576u};
        for (uint32_t i = 0;
             i < sizeof(sizes_rh_stream) / sizeof(sizes_rh_stream[0]); i++) {
            benchRapidhashStream("stream-main", RAPIDHASH_VARIANT_MAIN,
                                 RAPIDHASH_MODE_FAST, sizes_rh_stream[i]);
            benchRapidhashStream("stream-micro", RAPIDHASH_VARIANT_MICRO,
                                 RAPIDHASH_MODE_FAST, sizes_rh_stream[i]);
            benchRapidhashStream("stream-nano", RAPIDHASH_VARIANT_NANO,
                                 RAPIDHASH_MODE_FAST, sizes_rh_stream[i]);
        }
        /* 4-lane multi-buffer SHA-256: aggregate throughput across the
         * four parallel streams.  64 B = one block per lane; 65536 B is
         * the bulk-throughput sweet spot. */
        benchSha256Multi4(64u);
        benchSha256Multi4(65536u);

        /* HMAC: prepared-key advantage shows on small messages. */
        const size_t sizes_hmac[] = {64u, 256u, 1024u, 65536u};
        for (uint32_t i = 0;
             i < (uint32_t)(sizeof(sizes_hmac) / sizeof(sizes_hmac[0])); i++) {
            benchHmacSha256Plain(sizes_hmac[i]);
            benchHmacSha256Prepared(sizes_hmac[i]);
        }

        /* Envelope across the configured algorithm matrix.  Same size
         * spread; one wrap and one verify row per (algo, size). */
        const struct {
            flexEnvelopeAlgo algo;
            const char *label;
        } algos[] = {
            {FLEX_ENV_ALGO_XXH64, "xxh64"},
            {FLEX_ENV_ALGO_XXH3_64, "xxh3_64"},
            {FLEX_ENV_ALGO_XXH3_128, "xxh3_128"},
            {FLEX_ENV_ALGO_RAPIDHASH, "rapidhash"},
            {FLEX_ENV_ALGO_BLAKE3_KEYED, "blake3_keyed"},
            {FLEX_ENV_ALGO_HMAC_SHA256, "hmac_sha256"},
            {FLEX_ENV_ALGO_HMAC_BLAKE3, "hmac_blake3"},
        };
        for (uint32_t a = 0; a < sizeof(algos) / sizeof(algos[0]); a++) {
            for (uint32_t s = 0; s < n_sizes; s++) {
                benchEnvelope(algos[a].algo, algos[a].label, sizes_hash[s]);
            }
        }

        /* AEAD across the same sizes. */
        for (uint32_t i = 0; i < n_sizes; i++) {
            benchAead(sizes_hash[i]);
        }

        /* End-to-end cluster mailbox throughput on a handful of
         * representative message sizes. */
        const size_t sizes_mb[] = {64u, 1024u, 65536u};
        for (uint32_t i = 0;
             i < (uint32_t)(sizeof(sizes_mb) / sizeof(sizes_mb[0])); i++) {
            benchMailbox(sizes_mb[i]);
        }

        /* membound allocator: per-op alloc/free cost for the three
         * dynamic tiers — STANDARD (global lock), ENTERPRISE (per-extent
         * locks), and ENTERPRISE+threadCache (the lock-free fast path).
         * Single-thread by design; the MT comparison is in memboundBench. */
        const size_t sizes_mb_alloc[] = {64u, 256u, 1024u};
        membound *mbStd = memboundCreateDynamic(8u << 20, 0, 8u << 20);
        membound *mbEnt = memboundCreateEnterprise(8u << 20, 0);
        const memboundConfig mbTcCfg = {.mode = MEMBOUND_MODE_DYNAMIC,
                                        .initialSize = 8u << 20,
                                        .tier = MEMBOUND_TIER_ENTERPRISE,
                                        .threadSafe = true,
                                        .growthSize = 8u << 20,
                                        .threadCache = true};
        membound *mbTc = memboundCreateWithConfig(&mbTcCfg);
        for (uint32_t i = 0;
             i < (uint32_t)(sizeof(sizes_mb_alloc) / sizeof(sizes_mb_alloc[0]));
             i++) {
            benchMembound("standard", mbStd, sizes_mb_alloc[i]);
            benchMembound("enterprise", mbEnt, sizes_mb_alloc[i]);
            benchMembound("threadcache", mbTc, sizes_mb_alloc[i]);
        }
        memboundFlushThreadCache(mbTc);
        memboundShutdown(mbStd);
        memboundShutdown(mbEnt);
        memboundShutdown(mbTc);
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
