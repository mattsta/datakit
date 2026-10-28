/* perfRegression — see perfRegression.h for the contract.
 *
 * Each `bench*` function below times a single hot path against a
 * fixed-size payload, prints one stable TSV row, and moves on.
 * Iterations scale inversely with payload size so the total wall
 * time per row stays small (~50-200 ms) and the suite finishes
 * within a CI step budget. */

#include "perfRegression.h"

#ifdef DATAKIT_TEST

#include "atomPersist.h"
#include "blake3.h"
#include "chacha20poly1305.h"
#include "cleaner.h"
#include "clusterMailbox.h"
#include "ctest.h"
#include "datakit.h"
#include "dkTestTmp.h"
#include "dkTierLimit.h"
#include "fenwick/fenwickI64.h"
#include "flexEnvelope.h"
#include "hash/sha256ArmCrypto.h"
#include "hmac.h"
#include "hmacKeyring.h"
#include "idRows.h"
#include "membound.h"
#include "mflex.h"
#include "multiFenwick.h"
#include "multiFenwickFlat.h"
#include "multiOrderedSet.h"
#include "multiarray.h"
#include "multidict.h"
#include "multilist.h"
#include "multilru.h"
#include "multimapAtom.h"
#include "multimapAtomDense.h"
#include "persist/intsetP.h"
#include "persist/multiOrderedSetP.h"
#include "persist/multidictP.h"
#include "persist/multimapP.h"
#include "persist/multiroarP.h"
#include "persist/persistCtx.h"
#include "rapidhash.h"
#include "sha256.h"
#include "str.h" /* StrScanScanReliable */
#include "stringPool.h"
#include "timeUtil.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PERF_REG_KEY_LEN ((size_t)32)
#define PERF_REG_NONCE_LEN ((size_t)12)

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleanerName(uint8_t *, zfree, U8Buf);

/* benchSha256Multi4's lane buffers */
defcleanerArrayName(uint8_t *, 4, zfree, Lanes4);

/* Scope-exit cleaner for a runtime-sized array of owned buffers
 * (benchMailbox's pre-built envelope batch): frees each element then the
 * index array itself. */
typedef struct perfRegPtrArray {
    uint8_t **items;
    uint32_t count;
} perfRegPtrArray;

DK_FN_UNUSED static void perfRegPtrArray_cleanup(perfRegPtrArray *pa) {
    if (pa->items) {
        for (uint32_t i = 0; i < pa->count; i++) {
            if (pa->items[i]) {
                zfree(pa->items[i]);
            }
        }
        zfree(pa->items);
    }
}

/* True when DK_PERF_MODULES names 'module'; with DK_PERF_MODULES unset,
 * 'unsetMeans' */
static bool perfListed_(const char *module, bool unsetMeans) {
    const char *list = getenv("DK_PERF_MODULES");
    if (!list || !*list) {
        return unsetMeans;
    }

    const size_t len = strlen(module);
    for (const char *p = list; *p;) {
        const char *end = strchr(p, ',');
        const size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n == len && strncmp(p, module, n) == 0) {
            return true;
        }

        p += n + (end ? 1 : 0);
    }

    return false;
}

/* True when DK_PERF_MODULES is unset or names 'module' */
static bool perfWanted(const char *module) {
    return perfListed_(module, true);
}

/* True only when DK_PERF_MODULES names 'module': rows too slow for every
 * test run (large-scale behaviour), run by name, e.g. through
 * dktools perfcompare --modules */
static bool perfNamed(const char *module) {
    return perfListed_(module, false);
}

static void fillDeterministic(uint8_t *buf, size_t len, uint64_t seed) {
    uint64_t s = seed | 1u;
    for (size_t i = 0; i < len; i++) {
        dkLcg64Next(&s);
        buf[i] = (uint8_t)(s >> 56);
    }
}

/* Pick an iteration count so the timed window targets ~100 ms. */
static uint32_t pickIters(size_t bytes_per_op) {
    if (bytes_per_op == 0) {
        return 100'000u;
    }
    if (bytes_per_op <= 64) {
        return 200'000u;
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
    cleanerName(zfree, U8Buf) uint8_t *buf = zmalloc(bytes ? bytes : 1u);
    fillDeterministic(buf, bytes, 0xB1u);
    uint8_t out[BLAKE3_OUT_LEN];
    const uint32_t iters = pickIters(bytes);
    const double t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < iters; i++) {
        blake3Hash(buf, bytes, out);
    }
    const double secs = timeUtilMonotonicSec() - t0;
    emitRow("blake3", "hash", bytes, iters, secs);
    antiDce(out, sizeof(out));
}

/* ---- SHA-256 (auto-dispatched to ARM crypto on Apple Silicon) ----- */
static void benchSha256(size_t bytes) {
    cleanerName(zfree, U8Buf) uint8_t *buf = zmalloc(bytes ? bytes : 1u);
    fillDeterministic(buf, bytes, 0xA7u);
    uint8_t out[SHA256_DIGEST_LEN];
    const uint32_t iters = pickIters(bytes);
    const double t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < iters; i++) {
        sha256Hash(buf, bytes, out);
    }
    const double secs = timeUtilMonotonicSec() - t0;
    emitRow("sha256", "hash", bytes, iters, secs);
    antiDce(out, sizeof(out));
}

/* ---- xxHash: XXH64 / XXH3-64 / XXH3-128 (deps/xxHash) ----------- */

static void benchXxh64(size_t bytes) {
    cleanerName(zfree, U8Buf) uint8_t *buf = zmalloc(bytes ? bytes : 1u);
    fillDeterministic(buf, bytes, 0x64u);
    const uint32_t iters = pickIters(bytes);
    [[maybe_unused]] volatile uint64_t sink = 0;
    const double t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < iters; i++) {
        sink ^= XXH64(buf, bytes, 0);
    }
    const double secs = timeUtilMonotonicSec() - t0;
    emitRow("xxh64", "hash", bytes, iters, secs);
}

static void benchXxh3_64(size_t bytes) {
    cleanerName(zfree, U8Buf) uint8_t *buf = zmalloc(bytes ? bytes : 1u);
    fillDeterministic(buf, bytes, 0x33u);
    const uint32_t iters = pickIters(bytes);
    [[maybe_unused]] volatile uint64_t sink = 0;
    const double t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < iters; i++) {
        sink ^= XXH3_64bits(buf, bytes);
    }
    const double secs = timeUtilMonotonicSec() - t0;
    emitRow("xxh3_64", "hash", bytes, iters, secs);
}

static void benchXxh3_128(size_t bytes) {
    cleanerName(zfree, U8Buf) uint8_t *buf = zmalloc(bytes ? bytes : 1u);
    fillDeterministic(buf, bytes, 0x77u);
    const uint32_t iters = pickIters(bytes);
    [[maybe_unused]] volatile uint64_t sink = 0;
    const double t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < iters; i++) {
        XXH128_hash_t h = XXH3_128bits(buf, bytes);
        sink ^= h.low64 ^ h.high64;
    }
    const double secs = timeUtilMonotonicSec() - t0;
    emitRow("xxh3_128", "hash", bytes, iters, secs);
}

/* ---- rapidhash: three variants × two modes ---------------------- */

typedef uint64_t (*rapidhashOneShot)(const void *, size_t);

static void benchRapidhashOneShot(const char *label, rapidhashOneShot fn,
                                  size_t bytes) {
    cleanerName(zfree, U8Buf) uint8_t *buf = zmalloc(bytes ? bytes : 1u);
    fillDeterministic(buf, bytes, 0xD3u);
    const uint32_t iters = pickIters(bytes);
    [[maybe_unused]] volatile uint64_t sink = 0;
    const double t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < iters; i++) {
        sink ^= fn(buf, bytes);
    }
    const double secs = timeUtilMonotonicSec() - t0;
    emitRow("rapidhash", label, bytes, iters, secs);
}

/* Streaming: 64-byte chunks across the input.  This represents the
 * realistic "ingest from a network/disk reader" cost, not the
 * absolute best-case (one Update covers everything). */
static void benchRapidhashStream(const char *label, rapidhashVariant variant,
                                 rapidhashMode mode, size_t bytes) {
    cleanerName(zfree, U8Buf) uint8_t *buf = zmalloc(bytes ? bytes : 1u);
    fillDeterministic(buf, bytes, 0xD7u);
    const size_t chunk = 64;
    const uint32_t iters = pickIters(bytes);
    [[maybe_unused]] volatile uint64_t sink = 0;
    const double t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < iters; i++) {
        rapidhashCtx ctx;
        rapidhashStreamInit(&ctx, variant, mode, 0, nullptr);
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
    const double secs = timeUtilMonotonicSec() - t0;
    emitRow("rapidhash", label, bytes, iters, secs);
}

/* ---- SHA-256 4-lane multi-buffer (ARM crypto fast path) ---------- */
static void benchSha256Multi4(size_t bytes_per_lane) {
    /* Round down to a whole number of 64-byte blocks so the multi-buffer
     * entry point sees identical work per lane. */
    const size_t nb = bytes_per_lane / SHA256_BLOCK_LEN;
    const size_t actual = nb * SHA256_BLOCK_LEN;
    cleanerName(zfree, Lanes4)
        uint8_t *bufs[4] = {nullptr, nullptr, nullptr, nullptr};
    for (uint32_t lane = 0; lane < 4; lane++) {
        bufs[lane] = zmalloc(actual ? actual : 1u);
        fillDeterministic(bufs[lane], actual, 0xC1u + lane);
    }
    uint32_t st[4][8];
    for (uint32_t lane = 0; lane < 4; lane++) {
        memcpy(st[lane], sha256InitialHash, sizeof(st[lane]));
    }
    const uint8_t *bptrs[4] = {bufs[0], bufs[1], bufs[2], bufs[3]};

    const uint32_t iters = pickIters(bytes_per_lane);
    const double t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < iters; i++) {
        sha256ArmCryptoCompressBlocksMulti4(st, bptrs, nb);
    }
    const double secs = timeUtilMonotonicSec() - t0;

    /* Report aggregate throughput: bytes processed across all 4 lanes
     * per iteration. */
    emitRow("sha256", "multi4", actual * 4u, iters, secs);

    /* Anti-DCE. */
    if ((st[0][0] | st[3][7]) == 0xFFFFFFFF) {
        printf("# anti-DCE multi4 %08x\n", st[0][0]);
    }
}

/* ---- HMAC-SHA-256 plain (per-call key derivation) ----------------- */
static void benchHmacSha256Plain(size_t bytes) {
    uint8_t key[PERF_REG_KEY_LEN];
    fillDeterministic(key, sizeof(key), 0xC0u);
    cleanerName(zfree, U8Buf) uint8_t *buf = zmalloc(bytes ? bytes : 1u);
    fillDeterministic(buf, bytes, 0x42u);
    uint8_t out[32];
    const uint32_t iters = pickIters(bytes);
    const double t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < iters; i++) {
        hmacCompute(&hmacAlgoSHA256, key, sizeof(key), buf, bytes, out);
    }
    const double secs = timeUtilMonotonicSec() - t0;
    emitRow("hmac", "sha256_plain", bytes, iters, secs);
    antiDce(out, sizeof(out));
}

/* ---- HMAC-SHA-256 with prepared key (the hot path) ---------------- */
static void benchHmacSha256Prepared(size_t bytes) {
    uint8_t key[PERF_REG_KEY_LEN];
    fillDeterministic(key, sizeof(key), 0xC0u);
    cleanerName(zfree, U8Buf) uint8_t *buf = zmalloc(bytes ? bytes : 1u);
    fillDeterministic(buf, bytes, 0x42u);
    uint8_t prep_storage[2048];
    hmacKey prep;
    hmacPrepareKey(&prep, &hmacAlgoSHA256, prep_storage, sizeof(prep_storage),
                   key, sizeof(key));
    uint8_t out[32];
    const uint32_t iters = pickIters(bytes);
    const double t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < iters; i++) {
        hmacComputeWithKey(&prep, buf, bytes, out);
    }
    const double secs = timeUtilMonotonicSec() - t0;
    emitRow("hmac", "sha256_prepared", bytes, iters, secs);
    antiDce(out, sizeof(out));
}

/* ---- flexEnvelope wrap+verify under each algorithm ---------------- */
static void benchEnvelope(flexEnvelopeAlgo algo, const char *algo_label,
                          size_t bytes) {
    uint8_t key[PERF_REG_KEY_LEN];
    fillDeterministic(key, sizeof(key), 0xE1u);
    cleanerName(zfree, U8Buf) uint8_t *payload = zmalloc(bytes ? bytes : 1u);
    fillDeterministic(payload, bytes, 0x91u);
    const bool keyed = flexEnvelopeAlgoIsKeyed(algo);
    flexEnvelopeWrapParams wp = {
        .algo = algo,
        .key = keyed ? key : nullptr,
        .key_len = keyed ? sizeof(key) : 0,
    };
    const size_t total =
        flexEnvelopeWrappedSize((uint64_t)bytes, algo, FLEX_ENV_FLAG_NONE);
    cleanerName(zfree, U8Buf) uint8_t *wrapped = zmalloc(total);
    size_t written = 0;
    flexEnvelopeWrap(wrapped, total, payload, (uint64_t)bytes, &wp, &written);

    flexEnvelopeVerifyParams vp = {
        .key = keyed ? key : nullptr,
        .key_len = keyed ? sizeof(key) : 0,
    };
    const uint32_t iters = pickIters(bytes);

    /* Wrap timing. */
    const double t_w0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < iters; i++) {
        flexEnvelopeWrap(wrapped, total, payload, (uint64_t)bytes, &wp,
                         &written);
    }
    const double secs_wrap = timeUtilMonotonicSec() - t_w0;
    char label[64];
    snprintf(label, sizeof(label), "%s_wrap", algo_label);
    emitRow("envelope", label, bytes, iters, secs_wrap);

    /* Verify timing (raw key path — every call re-derives K_ipad/K_opad). */
    const uint8_t *last_out_p = nullptr;
    uint64_t last_out_pl = 0;
    const double t_v0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < iters; i++) {
        flexEnvelopeVerify(wrapped, total, &vp, &last_out_p, &last_out_pl);
    }
    const double secs_verify = timeUtilMonotonicSec() - t_v0;
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
        const double t_vp0 = timeUtilMonotonicSec();
        for (uint32_t i = 0; i < iters; i++) {
            flexEnvelopeVerify(wrapped, total, &vp_prep, &last_out_p,
                               &last_out_pl);
        }
        const double secs_vp = timeUtilMonotonicSec() - t_vp0;
        snprintf(label, sizeof(label), "%s_verify_prep", algo_label);
        emitRow("envelope", label, bytes, iters, secs_vp);
    }

    /* anti-DCE: ensure the verify path's output is consumed.  Reading
     * one byte from the returned payload pointer forces the compiler
     * to actually compute the digest comparison. */
    if (last_out_p != nullptr && last_out_pl > 0u) {
        antiDce(last_out_p, 1u);
    }
    /* The wrapped buffer is regenerated by the wrap loop above; sink
     * its trailing digest so the wrap loop's writes are observed. */
    antiDce(wrapped + total - 1u, 1u);
}

/* ---- ChaCha20-Poly1305 AEAD seal+open ---------------------------- */
static void benchAead(size_t bytes) {
    uint8_t key[PERF_REG_KEY_LEN];
    uint8_t nonce[PERF_REG_NONCE_LEN];
    fillDeterministic(key, sizeof(key), 0xAEu);
    fillDeterministic(nonce, sizeof(nonce), 0xADu);
    cleanerName(zfree, U8Buf) uint8_t *plain = zmalloc(bytes ? bytes : 1u);
    fillDeterministic(plain, bytes, 0x77u);
    cleanerName(zfree, U8Buf) uint8_t *ct = zmalloc(bytes ? bytes : 1u);
    uint8_t tag[CHACHA20_POLY1305_TAG_LEN];
    const uint32_t iters = pickIters(bytes);

    const double t_s0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < iters; i++) {
        (void)chacha20poly1305Seal(key, nonce, nullptr, 0u, plain, bytes, ct,
                                   tag);
    }
    const double secs_seal = timeUtilMonotonicSec() - t_s0;
    emitRow("aead", "chacha20poly1305_seal", bytes, iters, secs_seal);
    antiDce(ct, bytes > 0 ? 1u : 0u);
    antiDce(tag, sizeof(tag));

    const double t_o0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < iters; i++) {
        (void)chacha20poly1305Open(key, nonce, nullptr, 0u, ct, bytes, tag,
                                   plain);
    }
    const double secs_open = timeUtilMonotonicSec() - t_o0;
    emitRow("aead", "chacha20poly1305_open", bytes, iters, secs_open);
    antiDce(plain, bytes > 0 ? 1u : 0u);
}

/* ---- clusterMailbox seal + ingest end-to-end --------------------- */
static void benchMailbox(size_t bytes) {
    uint8_t key[PERF_REG_KEY_LEN];
    fillDeterministic(key, sizeof(key), 0xB0u);
    hmacKeyring *ring = hmacKeyringCreate();
    const hmacKeyId K = 1u;
    hmacKeyringInstallUnsalted(ring, K, &hmacAlgoSHA256, key, sizeof(key));
    clusterMailbox *mb =
        clusterMailboxCreate(ring, FLEX_ENV_ALGO_HMAC_SHA256, nullptr, nullptr);
    cleanerName(zfree, U8Buf) uint8_t *payload = zmalloc(bytes ? bytes : 1u);
    fillDeterministic(payload, bytes, 0x31u);
    const size_t sealed_sz = clusterMailboxSealedSize(mb, bytes);
    cleanerName(zfree, U8Buf) uint8_t *envelope = zmalloc(sealed_sz);
    /* Seal once to size + warm caches. */
    size_t written = 0;
    clusterMailboxSeal(mb, 1u, 1u, 0u, payload, bytes, K, envelope, sealed_sz,
                       &written);

    const uint32_t iters = pickIters(bytes);

    /* Seal timing. */
    const double t_s0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < iters; i++) {
        clusterMailboxSeal(mb, 1u, (uint64_t)i + 2u, 0u, payload, bytes, K,
                           envelope, sealed_sz, &written);
    }
    const double secs_seal = timeUtilMonotonicSec() - t_s0;
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
        cleaner(perfRegPtrArray) perfRegPtrArray envelopesArr = {
            .items = zmalloc(batch_n * sizeof(uint8_t *)), /* @own: container */
            .count = batch_n,
        };
        uint8_t **envelopes = envelopesArr.items;
        for (uint32_t i = 0; i < batch_n; i++) {
            /* @own: container; free=zfree */
            envelopes[i] = zmalloc(sealed_sz);
            size_t w = 0;
            clusterMailboxSeal(mb, /* origin */ 2u, /* seq */ (uint64_t)i + 1u,
                               0u, payload, bytes, K, envelopes[i], sealed_sz,
                               &w);
        }
        /* Fresh mailbox so origin 2's watermark starts at 0 and the
         * batch sequence 1..N walks monotonically. */
        clusterMailboxFree(mb);
        mb = clusterMailboxCreate(ring, FLEX_ENV_ALGO_HMAC_SHA256, nullptr,
                                  nullptr);

        /* Round iter count down to a multiple of batch_n so every
         * pass sees a complete batch sweep.  Replenish the watermark
         * by rebuilding the mailbox ONLY between full batches —
         * amortized cost is one Free+Create per batch_n Ingests, not
         * per Ingest. */
        const uint32_t passes = iters / batch_n;
        const uint32_t real_iters = passes * batch_n;
        const double t_i0 = timeUtilMonotonicSec();
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
            mb = clusterMailboxCreate(ring, FLEX_ENV_ALGO_HMAC_SHA256, nullptr,
                                      nullptr);
        }
        const double secs_ingest = timeUtilMonotonicSec() - t_i0;
        emitRow("mailbox", "ingest", bytes, real_iters > 0 ? real_iters : 1u,
                secs_ingest);
    }

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
    const uint32_t iters = 2'000'000u;
    [[maybe_unused]] volatile void *sink = nullptr;
    const double t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < iters; i++) {
        void *p = memboundAlloc(pool, bytes);
        sink = p;
        memboundFree(pool, p);
    }
    const double secs = timeUtilMonotonicSec() - t0;
    emitRow("membound", name, bytes, iters, secs);
}

/* Container rows: one operation type per row over PERF_CONTAINER_N
 * distinct 16-byte keys, in a fixed order.  "op" = one call. */
static constexpr unsigned PERF_CONTAINER_N = 100'000u;
static constexpr unsigned PERF_CONTAINER_KEY = 16u;

defcleanerName(char *, zfree, PerfKeys);
defcleanerName(uint64_t *, zfree, PerfIds);
defcleaner(multiOrderedSet *, multiOrderedSetFree);
defcleaner(multidict *, multidictFree);
defcleaner(multiarray *, multiarrayFree);
defcleaner(multidictClass *, multidictDefaultClassFree);
defcleaner(multimapAtom *, multimapAtomFree);
defcleaner(idRows *, idRowsFree);
defcleaner(multimapAtomDense *, multimapAtomDenseFree);
defcleaner(stringPool *, stringPoolFree);
defcleaner(intset *, intsetFree);
defcleaner(multilru *, multilruFree);
defcleanerName(multilruPtr *, zfree, PerfLruPtrs);
defcleaner(multimap *, multimapFree);
defcleaner(multiroar *, multiroarFree);
defcleaner(multilist *, multilistFree);
defcleaner(mflexState *, mflexStateFree);

/* PERF_CONTAINER_N keys, each PERF_CONTAINER_KEY bytes plus a NUL */
static char *perfContainerKeys(void) {
    /* @own: caller; free=zfree */
    char *keys = zmalloc(PERF_CONTAINER_N * (PERF_CONTAINER_KEY + 1));
    for (uint32_t i = 0; i < PERF_CONTAINER_N; i++) {
        snprintf(keys + i * (PERF_CONTAINER_KEY + 1), PERF_CONTAINER_KEY + 1,
                 "key%013" PRIu32, i);
    }

    return keys;
}

static databox perfContainerKey(const char *keys, uint32_t i) {
    return databoxNewBytes(keys + i * (PERF_CONTAINER_KEY + 1),
                           PERF_CONTAINER_KEY);
}

/* i in [0, PERF_CONTAINER_N) visited in a fixed scattered order */
static uint32_t perfScatter(uint32_t i) {
    return (i * 7919u) % PERF_CONTAINER_N;
}

/* Sink for results the timed loops produce, so none is optimised out */
static volatile uint64_t perfContainerSink;

static void benchMultidict(void) {
    cleanerName(zfree, PerfKeys) char *keys = perfContainerKeys();
    const uint32_t n = PERF_CONTAINER_N;
    /* Released in reverse: the dict before its class */
    cleaner(multidictDefaultClassFree) multidictClass *qdc =
        multidictDefaultClassNew();
    cleaner(multidictFree) multidict *d =
        multidictNew(&multidictTypeExactKey, qdc, 7);
    uint64_t sink = 0;

    double t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox k = perfContainerKey(keys, i);
        const databox v = databoxNewSigned(i);
        sink += multidictAdd(d, &k, &v);
    }
    emitRow("multidict", "add", 0, n, timeUtilMonotonicSec() - t0);

    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox k = perfContainerKey(keys, i);
        databox v = {};
        sink += multidictFind(d, &k, &v);
    }
    emitRow("multidict", "find", 0, n, timeUtilMonotonicSec() - t0);

    /* AddNX of present keys: every offer refused */
    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox k = perfContainerKey(keys, i);
        const databox v = databoxNewSigned(i);
        sink += multidictAddNX(d, &k, &v);
    }
    emitRow("multidict", "addNXPresent", 0, n, timeUtilMonotonicSec() - t0);

    t0 = timeUtilMonotonicSec();
    for (uint32_t r = 0; r < 3; r++) {
        for (uint32_t i = 0; i < n; i++) {
            const databox k = perfContainerKey(keys, i);
            int64_t out = 0;
            sink += multidictIncrBy(d, &k, 1, &out);
        }
    }
    emitRow("multidict", "incrBy", 0, 3 * n, timeUtilMonotonicSec() - t0);

    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox k = perfContainerKey(keys, i);
        const databox v = databoxNewSigned(-(int64_t)i);
        sink += multidictAddXX(d, &k, &v);
    }
    emitRow("multidict", "addXX", 0, n, timeUtilMonotonicSec() - t0);

    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox k = perfContainerKey(keys, i);
        double out = 0;
        sink += multidictIncrByFloat(d, &k, 0.5, &out);
    }
    emitRow("multidict", "incrByFloat", 0, n, timeUtilMonotonicSec() - t0);

    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox k = perfContainerKey(keys, i);
        sink += multidictDelete(d, &k);
    }
    emitRow("multidict", "delete", 0, n, timeUtilMonotonicSec() - t0);

    /* Presized: adds without growth, then removals that take the value */
    multidictExpand(d, n / 2);
    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox k = perfContainerKey(keys, i);
        const databox v = databoxNewSigned(i);
        sink += multidictAdd(d, &k, &v);
    }
    emitRow("multidict", "addPresized", 0, n, timeUtilMonotonicSec() - t0);

    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox k = perfContainerKey(keys, i);
        databox v;
        if (multidictGetAndDelete(d, &k, &v)) {
            sink++;
            databoxFreeData(&v);
        }
    }
    emitRow("multidict", "getAndDelete", 0, n, timeUtilMonotonicSec() - t0);

    /* AddNX of absent keys into the emptied (still presized) table */
    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox k = perfContainerKey(keys, i);
        const databox v = databoxNewSigned(i);
        sink += multidictAddNX(d, &k, &v);
    }
    emitRow("multidict", "addNX", 0, n, timeUtilMonotonicSec() - t0);

    /* Byte-based growth with 4 KiB slots (about 150 of these rows each,
     * where the default holds about 2): what a slot search costs once
     * slots are long */
    cleaner(multidictFree) multidict *wide =
        multidictNew(&multidictTypeExactKey, qdc, 7);
    multidictEnableByteBasedExpansion(wide, 4096, 16384);
    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox k = perfContainerKey(keys, i);
        const databox v = databoxNewSigned(i);
        sink += multidictAdd(wide, &k, &v);
    }
    emitRow("multidict", "addLargeSlots", 0, n, timeUtilMonotonicSec() - t0);

    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox k = perfContainerKey(keys, i);
        databox v = {};
        sink += multidictFind(wide, &k, &v);
    }
    emitRow("multidict", "findLargeSlots", 0, n, timeUtilMonotonicSec() - t0);

    perfContainerSink = sink;
}

/* The operations a multidict load measurement times, in row order */
typedef enum mdLoadOp {
    MD_LOAD_ADD = 0,
    MD_LOAD_FIND,
    MD_LOAD_FIND_SCATTERED,
    MD_LOAD_DELETE,
    MD_LOAD_OP_COUNT
} mdLoadOp;

static const char *const mdLoadOpNames[MD_LOAD_OP_COUNT] = {
    "add", "find", "findScattered", "delete"};

/* One measured table: seconds per operation kind over its 'n' entries,
 * and its live heap after the adds (metered at the allocator; 0 when the
 * allocator cannot report sizes) */
typedef struct mdLoadResult {
    double secs[MD_LOAD_OP_COUNT];
    int64_t heap;
} mdLoadResult;

/* 'n' of the keys in a dict at 'load' percent (shrinking below a
 * twentieth of it, as the defaults do): adds, finds in order and
 * scattered, deletes */
static mdLoadResult benchMultidictLoadOne(const char *keys, const uint32_t n,
                                          const uint32_t load) {
    mdLoadResult r = {};
    uint64_t sink = 0;
    const bool metered = ctestHeapMeterStart();
    cleaner(multidictDefaultClassFree) multidictClass *qdc =
        multidictDefaultClassNew();
    cleaner(multidictFree) multidict *d =
        multidictNew(&multidictTypeExactKey, qdc, 7);
    multidictSetLoadFactors(d, load, load / 20);
    double t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox k = perfContainerKey(keys, i);
        const databox v = databoxNewSigned(i);
        sink += multidictAdd(d, &k, &v);
    }
    r.secs[MD_LOAD_ADD] = timeUtilMonotonicSec() - t0;
    while (multidictRehash(d, 1024)) {
    }
    r.heap = metered ? ctestHeapMeterStop() : 0;

    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox k = perfContainerKey(keys, i);
        databox v = {};
        sink += multidictFind(d, &k, &v);
    }
    r.secs[MD_LOAD_FIND] = timeUtilMonotonicSec() - t0;

    /* Scattered over [0, n): 7919 is prime and divides no size here */
    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox k = perfContainerKey(keys, (i * 7919u) % n);
        databox v = {};
        sink += multidictFind(d, &k, &v);
    }
    r.secs[MD_LOAD_FIND_SCATTERED] = timeUtilMonotonicSec() - t0;

    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox k = perfContainerKey(keys, (i * 7919u) % n);
        sink += multidictDelete(d, &k);
    }
    r.secs[MD_LOAD_DELETE] = timeUtilMonotonicSec() - t0;
    perfContainerSink = sink;
    return r;
}

/* Where multidict find time varies (run by name): one process finds every
 * key of dicts that differ only in hash seed, at a size whose table and
 * slots outgrow the caches (100k) and one that fits them (10k).  Rows
 * agreeing within a run but moving together between runs point at the
 * process (memory placement, other load), not the seed; a seed whose row
 * stands apart points at the hash. */
static void benchMultidictSeeds(void) {
    static const uint32_t sizes[] = {10'000, PERF_CONTAINER_N};
    cleanerName(zfree, PerfKeys) char *keys = perfContainerKeys();
    uint64_t sink = 0;
    char row[48];
    for (size_t s = 0; s < countof(sizes); s++) {
        const uint32_t n = sizes[s];
        for (int32_t seed = 1; seed <= 8; seed++) {
            cleaner(multidictDefaultClassFree) multidictClass *qdc =
                multidictDefaultClassNew();
            cleaner(multidictFree) multidict *d =
                multidictNew(&multidictTypeExactKey, qdc, seed);
            for (uint32_t i = 0; i < n; i++) {
                const databox k = perfContainerKey(keys, i);
                const databox v = databoxNewSigned(i);
                sink += multidictAdd(d, &k, &v);
            }

            while (multidictRehash(d, 1024)) {
            }

            const double t0 = timeUtilMonotonicSec();
            for (uint32_t i = 0; i < n; i++) {
                const databox k = perfContainerKey(keys, i);
                databox v = {};
                sink += multidictFind(d, &k, &v);
            }

            snprintf(row, sizeof(row), "find%" PRIu32 "kSeed%" PRId32, n / 1000,
                     seed);
            emitRow("multidictSeeds", row, 0, n, timeUtilMonotonicSec() - t0);
        }
    }

    perfContainerSink = sink;
}

/* The load factors and table sizes both multidict sweeps measure: a
 * table's slot count steps in powers of two, so one size alone can put
 * two load factors on the same table */
static const uint32_t mdLoadFactors[] = {200, 400, 800, 1600, 3200};
static const uint32_t mdLoadSizes[] = {40'000, 70'000, PERF_CONTAINER_N};

/* multidict at each load factor and size (run by name): rows named op,
 * load, '@', size in thousands; each add row's bytes column is the dict's
 * live heap */
static void benchMultidictLoad(void) {
    cleanerName(zfree, PerfKeys) char *keys = perfContainerKeys();
    char row[48];
    for (size_t s = 0; s < countof(mdLoadSizes); s++) {
        const uint32_t n = mdLoadSizes[s];
        for (size_t l = 0; l < countof(mdLoadFactors); l++) {
            const mdLoadResult r =
                benchMultidictLoadOne(keys, n, mdLoadFactors[l]);
            for (int op = 0; op < MD_LOAD_OP_COUNT; op++) {
                snprintf(row, sizeof(row), "%s%" PRIu32 "@%" PRIu32 "k",
                         mdLoadOpNames[op], mdLoadFactors[l], n / 1000);
                const bool heapRow = op == MD_LOAD_ADD && r.heap > 0;
                emitRow("multidictLoad", row, heapRow ? (size_t)r.heap : 0, n,
                        r.secs[op]);
            }
        }
    }
}

/* The sweep `dktools tune --families multidict` reads (run by name):
 * per load factor, each operation over all three sizes together (a
 * growth cycle's spread of table fullness), rows named op and load; every
 * row's bytes column is the summed heap of the three tables */
static void benchMultidictLimits(void) {
    cleanerName(zfree, PerfKeys) char *keys = perfContainerKeys();
    char row[48];
    for (size_t l = 0; l < countof(mdLoadFactors); l++) {
        double secs[MD_LOAD_OP_COUNT] = {};
        uint32_t entries = 0;
        int64_t heap = 0;
        for (size_t s = 0; s < countof(mdLoadSizes); s++) {
            const mdLoadResult r =
                benchMultidictLoadOne(keys, mdLoadSizes[s], mdLoadFactors[l]);
            for (int op = 0; op < MD_LOAD_OP_COUNT; op++) {
                secs[op] += r.secs[op];
            }
            entries += mdLoadSizes[s];
            heap += r.heap;
        }

        for (int op = 0; op < MD_LOAD_OP_COUNT; op++) {
            snprintf(row, sizeof(row), "%s%" PRIu32, mdLoadOpNames[op],
                     mdLoadFactors[l]);
            emitRow("multidictLimits", row, heap > 0 ? (size_t)heap : 0,
                    entries, secs[op]);
        }
    }
}

/* A Full-tier set: adds of new members, then updates of existing ones
 * (new scores by Add, AddXX and IncrBy), score reads and removals */
static void benchMultiOrderedSet(void) {
    cleanerName(zfree, PerfKeys) char *keys = perfContainerKeys();
    const uint32_t n = PERF_CONTAINER_N;
    cleaner(multiOrderedSetFree) multiOrderedSet *mos = multiOrderedSetNew();
    uint64_t sink = 0;

    double t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox s = databoxNewSigned(i);
        const databox m = perfContainerKey(keys, i);
        sink += multiOrderedSetAdd(&mos, &s, &m);
    }
    emitRow("multiOrderedSet", "add", 0, n, timeUtilMonotonicSec() - t0);

    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox s = databoxNewSigned((int64_t)((i * 7919u) % n));
        const databox m = perfContainerKey(keys, i);
        sink += multiOrderedSetAdd(&mos, &s, &m);
    }
    emitRow("multiOrderedSet", "update", 0, n, timeUtilMonotonicSec() - t0);

    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox s = databoxNewSigned((int64_t)((i * 104729u) % n));
        const databox m = perfContainerKey(keys, i);
        sink += multiOrderedSetAddXX(&mos, &s, &m);
    }
    emitRow("multiOrderedSet", "addXX", 0, n, timeUtilMonotonicSec() - t0);

    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox delta = databoxNewSigned(3);
        const databox m = perfContainerKey(keys, i);
        databox result = {};
        sink += multiOrderedSetIncrBy(&mos, &delta, &m, &result);
    }
    emitRow("multiOrderedSet", "incrBy", 0, n, timeUtilMonotonicSec() - t0);

    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox m = perfContainerKey(keys, i);
        databox s = {};
        sink += multiOrderedSetGetScore(mos, &m, &s);
    }
    emitRow("multiOrderedSet", "getScore", 0, n, timeUtilMonotonicSec() - t0);

    /* Rank of a member spread over the whole set */
    const uint32_t ranks = 2000;
    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < ranks; i++) {
        const databox m = perfContainerKey(keys, (i * 7919u) % n);
        sink += (uint64_t)multiOrderedSetGetRank(mos, &m);
    }
    emitRow("multiOrderedSet", "getRank", 0, ranks,
            timeUtilMonotonicSec() - t0);

    /* A 100-score window near the top of the set, counted */
    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < ranks; i++) {
        const mosRangeSpec top = {.min = databoxNewSigned((int64_t)n - 100),
                                  .max = databoxNewSigned((int64_t)n)};
        sink += multiOrderedSetCountByScore(mos, &top);
    }
    emitRow("multiOrderedSet", "countByScoreTop", 0, ranks,
            timeUtilMonotonicSec() - t0);

    /* Union of two halves overlapping by half of each (n/2 members each,
     * n/4 shared), per input member: SUM and MAX combine the shared ones */
    {
        cleaner(multiOrderedSetFree) multiOrderedSet *a = multiOrderedSetNew();
        cleaner(multiOrderedSetFree) multiOrderedSet *b = multiOrderedSetNew();
        for (uint32_t i = 0; i < n / 2; i++) {
            const databox sa = databoxNewSigned(i);
            const databox ma = perfContainerKey(keys, i);
            sink += multiOrderedSetAdd(&a, &sa, &ma);
            const databox sb = databoxNewSigned((int64_t)n - i);
            const databox mb = perfContainerKey(keys, n / 4 + i);
            sink += multiOrderedSetAdd(&b, &sb, &mb);
        }
        const multiOrderedSet *both[2] = {a, b};
        static const struct {
            mosAggregate how;
            const char *name;
        } unions[] = {{MOS_AGGREGATE_SUM, "unionSum"},
                      {MOS_AGGREGATE_MAX, "unionMax"}};
        for (size_t u = 0; u < 2; u++) {
            t0 = timeUtilMonotonicSec();
            cleaner(multiOrderedSetFree) multiOrderedSet *un =
                multiOrderedSetUnion(both, nullptr, 2, unions[u].how);
            emitRow("multiOrderedSet", unions[u].name, 0, n,
                    timeUtilMonotonicSec() - t0);
            sink += multiOrderedSetCount(un);
        }
    }

    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox m = perfContainerKey(keys, i);
        sink += multiOrderedSetRemove(&mos, &m);
    }
    emitRow("multiOrderedSet", "remove", 0, n, timeUtilMonotonicSec() - t0);

    /* Every member with one score: an equal-score run never straddles two
     * of the Full tier's sub-maps, so it stays one flex and each add moves
     * a share of it (cost per add grows with the run; 10000 members keeps
     * the row short) */
    {
        const uint32_t sameN = 10000;
        cleaner(multiOrderedSetFree) multiOrderedSet *same =
            multiOrderedSetNew();
        const databox s = databoxNewSigned(7);
        t0 = timeUtilMonotonicSec();
        for (uint32_t i = 0; i < sameN; i++) {
            const databox m = perfContainerKey(keys, i);
            sink += multiOrderedSetAdd(&same, &s, &m);
        }
        emitRow("multiOrderedSet", "addEqualScore10k", 0, sameN,
                timeUtilMonotonicSec() - t0);
    }

    /* Bulk removals from a fresh n-member set (score i for member i), per
     * row removed: 1000-score windows by score, 1000-rank windows by rank,
     * then pops of 1000 from the bottom */
    {
        cleaner(multiOrderedSetFree) multiOrderedSet *bulk =
            multiOrderedSetNew();
        for (uint32_t i = 0; i < n; i++) {
            const databox s = databoxNewSigned(i);
            const databox m = perfContainerKey(keys, i);
            sink += multiOrderedSetAdd(&bulk, &s, &m);
        }

        const uint32_t windows = 20;
        size_t gone = 0;
        t0 = timeUtilMonotonicSec();
        for (uint32_t w = 0; w < windows; w++) {
            const int64_t lo = (int64_t)(n / 2 + w * 1000);
            const mosRangeSpec r = {.min = databoxNewSigned(lo),
                                    .max = databoxNewSigned(lo + 999)};
            gone += multiOrderedSetRemoveRangeByScore(&bulk, &r);
        }
        emitRow("multiOrderedSet", "removeRangeByScore", 0, (uint32_t)gone,
                timeUtilMonotonicSec() - t0);

        gone = 0;
        t0 = timeUtilMonotonicSec();
        for (uint32_t w = 0; w < windows; w++) {
            gone += multiOrderedSetRemoveRangeByRank(&bulk, n / 4, n / 4 + 999);
        }
        emitRow("multiOrderedSet", "removeRangeByRank", 0, (uint32_t)gone,
                timeUtilMonotonicSec() - t0);

        databox popMembers[1000];
        databox popScores[1000];
        gone = 0;
        t0 = timeUtilMonotonicSec();
        for (uint32_t w = 0; w < windows; w++) {
            const size_t popped =
                multiOrderedSetPopMin(&bulk, 1000, popMembers, popScores);
            for (size_t p = 0; p < popped; p++) {
                databoxFreeData(&popMembers[p]);
                databoxFreeData(&popScores[p]);
            }
            gone += popped;
        }
        emitRow("multiOrderedSet", "popMin", 0, (uint32_t)gone,
                timeUtilMonotonicSec() - t0);
    }

    perfContainerSink = sink;
}

/* What a persisted change costs against the lookup its wrapper adds:
 * multidictReplaceP (check, log, apply; no sync, no compaction) beside a
 * plain multidictReplace and the multidictExistsNoTouch the wrapper runs
 * first, over the same present keys (only when asked for by name) */
/* A persist context under 'tmp' that never syncs or compacts, so a
 * persisted row times the wrapper's check, its log write and the change */
static persistCtx *perfPersistCtx(dkTestTmpDir *tmp, const char *name) {
    char base[PATH_MAX];
    if (!dkTestTmpDirPath(tmp, name, base, sizeof(base))) {
        return nullptr;
    }

    persistCtxConfig config = persistCtxDefaultConfig();
    config.syncMode = PERSIST_CTX_SYNC_NONE;
    config.compactAfterOps = 0;
    config.compactAfterBytes = 0;
    return persistCtxNew(base, &config);
}

/* One timed row of n calls: the body runs once for each i in [0, n) */
#define PERF_ROW(module, row, n, ...)                                          \
    do {                                                                       \
        const double rowStart_ = timeUtilMonotonicSec();                       \
        for (uint32_t i = 0; i < (n); i++) {                                   \
            __VA_ARGS__;                                                       \
        }                                                                      \
        emitRow(module, row, 0, (n), timeUtilMonotonicSec() - rowStart_);      \
    } while (0)

/* ---- Size-limit sweeps ("<family>Limits" modules, run only by name;
 * `dktools tune` reads them back) ----
 *
 * Each family's speed against its density, by size limit: PERF_CONTAINER_N
 * values filled and used in scattered orders at every limit below.  Row
 * names are '<op><limit bytes>' (insert1024), and each fill row's bytes
 * column is the filled container's total bytes (dkTierStats).  A larger
 * limit packs values denser and walks longer inside each unit (a flex's
 * entries are variable-length, so a search steps entry by entry); a
 * smaller one routes across more units, and anything that walks units
 * one by one (a rank) grows with their count. */
static const flexCapSizeLimit perfSweepLimits[] = {
    FLEX_CAP_LEVEL_256,  FLEX_CAP_LEVEL_512,  FLEX_CAP_LEVEL_1024,
    FLEX_CAP_LEVEL_2048, FLEX_CAP_LEVEL_4096, FLEX_CAP_LEVEL_8192,
};

/* One timed limit row: the body runs once for each i in [0, n); 'bytes'
 * is read after the loop */
#define PERF_LIMIT_ROW(module, op, limit, bytes, n, ...)                       \
    do {                                                                       \
        const double rowStart_ = timeUtilMonotonicSec();                       \
        for (uint32_t i = 0; i < (n); i++) {                                   \
            __VA_ARGS__;                                                       \
        }                                                                      \
        const double rowSecs_ = timeUtilMonotonicSec() - rowStart_;            \
        char rowName_[48];                                                     \
        snprintf(rowName_, sizeof(rowName_), "%s%u", (op),                     \
                 flexOptimizationSizeLimit[(limit)]);                          \
        emitRow((module), rowName_, (bytes), (n), rowSecs_);                   \
    } while (0)

static uint64_t perfMultimapTotal(const multimap *m) {
    dkTierStats s;
    multimapStats(m, &s);
    return s.totalBytes;
}

/* multimap: (16-byte key, integer) rows inserted, looked up, deleted */
static void benchMultimapLimits(void) {
    cleanerName(zfree, PerfKeys) char *keys = perfContainerKeys();
    const uint32_t n = PERF_CONTAINER_N;
    uint64_t sink = 0;
    for (size_t l = 0; l < countof(perfSweepLimits); l++) {
        const flexCapSizeLimit limit = perfSweepLimits[l];
        cleaner(multimapFree) multimap *m = multimapNewLimit(2, limit);
        PERF_LIMIT_ROW(
            "multimapLimits", "insert", limit, perfMultimapTotal(m), n,
            const databox k = perfContainerKey(keys, perfScatter(i));
            const databox v = databoxNewSigned(i);
            const databox *row[2] = {&k, &v}; sink += multimapInsert(&m, row));
        PERF_LIMIT_ROW("multimapLimits", "exists", limit, 0, n,
                       const databox k =
                           perfContainerKey(keys, (i * 104729u) % n);
                       sink += multimapExists(m, &k));
        PERF_LIMIT_ROW("multimapLimits", "delete", limit, 0, n,
                       const databox k = perfContainerKey(keys, perfScatter(i));
                       sink += multimapDelete(&m, &k));
    }

    perfContainerSink = sink;
}

/* The live heap since a ctestHeapMeterStart that 'metered' says ran, for
 * a sweep row's bytes (0 when the allocator cannot report sizes) */
static size_t perfHeapStop(const bool metered) {
    const int64_t heap = metered ? ctestHeapMeterStop() : 0;
    return heap > 0 ? (size_t)heap : 0;
}

/* multimapAtom (string atoms, the default index) at each node limit of
 * the atom family (run by name; `dktools tune` reads it): new atoms
 * interned (bytes: the filled table's metered heap), present ones
 * interned again, ids read back in scattered order, atoms deleted by id */
static void benchMultimapAtomLimits(void) {
    cleanerName(zfree, PerfKeys) char *keys = perfContainerKeys();
    const uint32_t n = PERF_CONTAINER_N;
    uint64_t sink = 0;
    for (size_t l = 0; l < countof(perfSweepLimits); l++) {
        const flexCapSizeLimit limit = perfSweepLimits[l];
        if (dkTierSetDefaultLimit(DK_TIER_MULTIMAP_ATOM, limit) !=
            DK_TIER_LIMIT_SET) {
            continue;
        }

        const bool metered = ctestHeapMeterStart();
        cleaner(multimapAtomFree) multimapAtom *ma = multimapAtomNew();
        PERF_LIMIT_ROW("multimapAtomLimits", "intern", limit,
                       perfHeapStop(metered), n,
                       databox k = perfContainerKey(keys, i);
                       sink += multimapAtomInsertIfNewConvert(ma, &k));
        PERF_LIMIT_ROW("multimapAtomLimits", "find", limit, 0, n,
                       databox k = perfContainerKey(keys, perfScatter(i));
                       sink += multimapAtomInsertIfNewConvert(ma, &k));
        /* A fresh table hands out ids 0, 1, 2, ... */
        PERF_LIMIT_ROW(
            "multimapAtomLimits", "lookup", limit, 0, n,
            const databox ref = {.data.u = perfScatter(i),
                                 .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};
            databox value = {}; sink += multimapAtomLookup(ma, &ref, &value));
        PERF_LIMIT_ROW("multimapAtomLimits", "delete", limit, 0, n,
                       sink += multimapAtomDeleteById(ma, perfScatter(i)));
    }

    if (dkTierSetDefaultLimit(DK_TIER_MULTIMAP_ATOM,
                              dkTierCompiledLimit(DK_TIER_MULTIMAP_ATOM)) !=
        DK_TIER_LIMIT_SET) {
        printf("multimapAtomLimits: the atom limit was not restored\n");
    }

    perfContainerSink = sink;
}

static uint64_t perfOrderedSetTotal(const multiOrderedSet *mos) {
    dkTierStats s;
    multiOrderedSetStats(mos, &s);
    return s.totalBytes;
}

/* multiOrderedSet: (integer score, 16-byte member) pairs added, scored,
 * ranked (a rank walks sub-maps one by one) and removed */
static void benchMultiOrderedSetLimits(void) {
    cleanerName(zfree, PerfKeys) char *keys = perfContainerKeys();
    const uint32_t n = PERF_CONTAINER_N;
    uint64_t sink = 0;
    for (size_t l = 0; l < countof(perfSweepLimits); l++) {
        const flexCapSizeLimit limit = perfSweepLimits[l];
        cleaner(multiOrderedSetFree) multiOrderedSet *mos =
            multiOrderedSetNewLimit(limit);
        PERF_LIMIT_ROW("multiOrderedSetLimits", "add", limit,
                       perfOrderedSetTotal(mos), n,
                       const databox k = perfContainerKey(keys, perfScatter(i));
                       const databox s = databoxNewSigned(perfScatter(i));
                       sink += multiOrderedSetAdd(&mos, &s, &k));
        PERF_LIMIT_ROW(
            "multiOrderedSetLimits", "getScore", limit, 0, n,
            const databox k = perfContainerKey(keys, (i * 104729u) % n);
            databox s = {}; sink += multiOrderedSetGetScore(mos, &k, &s));
        PERF_LIMIT_ROW("multiOrderedSetLimits", "getRank", limit, 0, n / 10,
                       const databox k =
                           perfContainerKey(keys, (i * 104729u) % n);
                       sink += (uint64_t)multiOrderedSetGetRank(mos, &k));
        PERF_LIMIT_ROW("multiOrderedSetLimits", "remove", limit, 0, n,
                       const databox k = perfContainerKey(keys, perfScatter(i));
                       sink += multiOrderedSetRemove(&mos, &k));
    }

    perfContainerSink = sink;
}

static void perfFillList(multilist **ml, mflexState *s);

static uint64_t perfListTotal(const multilist *ml) {
    dkTierStats s;
    multilistStats(ml, &s);
    return s.totalBytes;
}

/* multilist: integers pushed, read by index, deleted at scattered
 * positions, then refilled and drained from the head as a queue */
static void benchMultilistLimits(void) {
    const uint32_t n = PERF_CONTAINER_N;
    cleaner(mflexStateFree) mflexState *s = mflexStateCreate();
    uint64_t sink = 0;
    for (size_t l = 0; l < countof(perfSweepLimits); l++) {
        const flexCapSizeLimit limit = perfSweepLimits[l];
        cleaner(multilistFree) multilist *ml = multilistNewLimit(limit, 0);
        PERF_LIMIT_ROW("multilistLimits", "pushTail", limit, perfListTotal(ml),
                       n,
                       const databox b = databoxNewSigned((int64_t)i * 1000003);
                       multilistPushByTypeTail(&ml, s, &b));
        PERF_LIMIT_ROW("multilistLimits", "index", limit, 0, n,
                       multilistEntry e;
                       sink += multilistIndexGet(ml, s, perfScatter(i), &e)
                                   ? (uint64_t)e.box.data.i
                                   : 0);
        PERF_LIMIT_ROW("multilistLimits", "delete", limit, 0, n,
                       sink += multilistDelRange(
                           &ml, s, (int64_t)((i * 7919u) % (n - i)), 1));
        perfFillList(&ml, s);
        PERF_LIMIT_ROW(
            "multilistLimits", "popHead", limit, 0, n, databox got = {};
            sink += multilistPopHead(&ml, s, &got); databoxFreeData(&got));
    }

    perfContainerSink = sink;
}

/* Whole-roar changes over 100000 chunks of one bit each ("op" = one
 * chunk): an in-place OR with a roar holding the next bit of every chunk,
 * then a range clear of every chunk */
static void benchMultiroar(void) {
    const uint32_t n = PERF_CONTAINER_N;
    cleaner(multiroarFree) multiroar *r = multiroarBitNew();
    cleaner(multiroarFree) multiroar *b = multiroarBitNew();
    for (uint32_t i = 0; i < n; i++) {
        multiroarBitSet(r, (uint64_t)i * 8192);
        multiroarBitSet(b, (uint64_t)i * 8192 + 1);
    }

    double t0 = timeUtilMonotonicSec();
    multiroarOr(r, b);
    emitRow("multiroar", "orInPlace", 0, n, timeUtilMonotonicSec() - t0);

    t0 = timeUtilMonotonicSec();
    multiroarBitClearRange(r, 0, (uint64_t)n * 8192);
    emitRow("multiroar", "clearRangeWhole", 0, n, timeUtilMonotonicSec() - t0);

    /* Whole chunks set: first into absent chunks, then over full ones */
    t0 = timeUtilMonotonicSec();
    multiroarBitSetRange(r, 0, (uint64_t)n * 8192);
    emitRow("multiroar", "setRangeWholeNew", 0, n, timeUtilMonotonicSec() - t0);

    t0 = timeUtilMonotonicSec();
    multiroarBitSetRange(r, 0, (uint64_t)n * 8192);
    emitRow("multiroar", "setRangeWholeFull", 0, n,
            timeUtilMonotonicSec() - t0);
    perfContainerSink = multiroarBitCount(r) + multiroarBitCount(b);
}

/* numeric: the two paths dk_float_extended serves (floatExtended.h), timed
 * so a change of extended type is measured: decimal text through the
 * reliable scan, and databoxCompare of large integers against nearby
 * doubles */
static void benchNumeric(void) {
    const uint32_t n = PERF_CONTAINER_N;
    constexpr int slot = 32;
    cleanerName(zfree, PerfKeys) char *texts = zcalloc(n, slot);
    cleanerName(zfree, U8Buf) uint8_t *lens = zcalloc(n, 1);
    for (uint32_t i = 0; i < n; i++) {
        lens[i] =
            (uint8_t)snprintf(&texts[(size_t)i * slot], slot, "%u.%06u",
                              7074451u + i * 131u, (i * 7919u) % 999'983u);
    }

    uint64_t sink = 0;
    PERF_ROW("numeric", "scanReliableReal", n, databox box = {};
             sink +=
             StrScanScanReliable(&texts[(size_t)i * slot], lens[i], &box);
             sink += box.type);

    PERF_ROW("numeric", "compareIntDouble", n,
             const databox a = DATABOX_SIGNED((int64_t)1 << 62 | i);
             const databox b = databoxNewReal((double)((int64_t)1 << 62) +
                                              (double)(i * 1024u));
             sink += (uint64_t)(databoxCompare(&a, &b) + 1));

    perfContainerSink = sink;
}

defcleanerName(databox *, zfree, Boxes);
defcleaner(multiFenwick *, multiFenwickFree);
defcleaner(multiFenwickFlat *, multiFenwickFlatFree);
defcleanerName(int64_t *, zfree, I64s);
defcleanerName(void *, fenwickI64Free, FenwickI64);

/* fenwick: the search both Fenwick trees answer from their prefix sums
 * (a tree of ones, so the target t lands at index t - 1) */
static void benchFenwick(void) {
    const uint32_t n = PERF_CONTAINER_N;
    cleanerName(zfree, Boxes) databox *values = zcalloc(n, sizeof(databox));
    for (uint32_t i = 0; i < n; i++) {
        values[i] = DATABOX_SIGNED(1);
    }

    cleaner(multiFenwickFree) multiFenwick *tree =
        multiFenwickNewFromArray(values, n);
    cleaner(multiFenwickFlatFree) multiFenwickFlat *flat =
        multiFenwickFlatNewFromArray(values, n);
    cleanerName(zfree, I64s) int64_t *ones = zcalloc(n, sizeof(int64_t));
    for (uint32_t i = 0; i < n; i++) {
        ones[i] = 1;
    }

    cleanerName(fenwickI64Free, FenwickI64) void *typed =
        fenwickI64NewFromArray(ones, n);
    uint64_t sink = 0;
    PERF_ROW("fenwick", "typedLowerBound", n,
             sink += fenwickI64LowerBound(typed, (int64_t)perfScatter(i) + 1));
    PERF_ROW("fenwick", "lowerBound", n,
             const databox t = DATABOX_SIGNED((int64_t)perfScatter(i) + 1);
             sink += multiFenwickLowerBound(tree, &t));
    PERF_ROW("fenwick", "flatLowerBound", n,
             const databox t = DATABOX_SIGNED((int64_t)perfScatter(i) + 1);
             sink += multiFenwickFlatLowerBound(flat, &t));
    PERF_ROW("fenwick", "query", n, databox sum = {};
             sink += multiFenwickQuery(tree, perfScatter(i), &sum);
             sink += sum.data.u);
    PERF_ROW("fenwick", "flatQuery", n, databox sum = {};
             sink += multiFenwickFlatQuery(flat, perfScatter(i), &sum);
             sink += sum.data.u);
    perfContainerSink = sink;
}

static void benchMultidictP(void) {
    cleanerName(zfree, PerfKeys) char *keys = perfContainerKeys();
    const uint32_t n = PERF_CONTAINER_N;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    persistCtx *ctx = perfPersistCtx(&tmp, "perfMultidictP");
    multidictP *dp = ctx ? multidictNewP(ctx) : nullptr;
    if (!dp) {
        persistCtxFree(ctx);
        return;
    }

    uint64_t sink = 0;
    for (uint32_t i = 0; i < n; i++) {
        const databox k = perfContainerKey(keys, i);
        const databox v = databoxNewSigned(i);
        sink += (uint64_t)multidictAddP(dp, &k, &v, nullptr);
    }

    double t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox k = perfContainerKey(keys, (i * 7919u) % n);
        const databox v = databoxNewSigned(-(int64_t)i);
        sink += (uint64_t)multidictReplaceP(dp, &k, &v);
    }
    emitRow("multidictP", "replaceP", 0, n, timeUtilMonotonicSec() - t0);

    multidict *d = multidictGetP(dp);
    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox k = perfContainerKey(keys, (i * 7919u) % n);
        const databox v = databoxNewSigned((int64_t)i);
        sink += (uint64_t)multidictReplace(d, &k, &v);
    }
    emitRow("multidictP", "replacePlain", 0, n, timeUtilMonotonicSec() - t0);

    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox k = perfContainerKey(keys, (i * 7919u) % n);
        sink += multidictExistsNoTouch(d, &k);
    }
    emitRow("multidictP", "existsNoTouch", 0, n, timeUtilMonotonicSec() - t0);

    /* The wrapper's two searches without the log between them: the check
     * leaves the key's slot warm for the replace */
    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox k = perfContainerKey(keys, (i * 7919u) % n);
        const databox v = databoxNewSigned((int64_t)i + 1);
        if (multidictExistsNoTouch(d, &k)) {
            sink += (uint64_t)multidictReplace(d, &k, &v);
        }
    }
    emitRow("multidictP", "existsThenReplace", 0, n,
            timeUtilMonotonicSec() - t0);

    multidictCloseP(dp);
    persistCtxFree(ctx);
    perfContainerSink = sink;
}

static void perfFillOrderedSet(multiOrderedSet **mos, const char *keys) {
    for (uint32_t i = 0; i < PERF_CONTAINER_N; i++) {
        const databox k = perfContainerKey(keys, i);
        const databox s = databoxNewSigned(i);
        multiOrderedSetAdd(mos, &s, &k);
    }
}

/* What each persisted change's check costs, for the wrappers that check
 * before they log: the wrapper's row, the plain change, and the check
 * followed by the plain change, over the same values (the check's share
 * is the last row minus the plain one).  Plain rows use containers of
 * their own; a removal row empties its container and an untimed refill
 * follows where a later row needs the values.  Intset values ascend on
 * add and descend on remove, so no row times moving the array (only
 * when asked for by name). */
static void benchPersistChecks(void) {
    cleanerName(zfree, PerfKeys) char *keys = perfContainerKeys();
    const uint32_t n = PERF_CONTAINER_N;
    const char *mod = "persistChecks";
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    uint64_t sink = 0;
    bool done = false;

    persistCtx *ictx = perfPersistCtx(&tmp, "intsetP");
    intsetP *isp = ictx ? intsetNewP(ictx) : nullptr;
    cleaner(intsetFree) intset *is = intsetNew();
    if (isp) {
        PERF_ROW(mod, "intsetAddP", n, sink += intsetAddP(isp, i, &done));
        PERF_ROW(mod, "intsetRemoveP", n,
                 sink += intsetRemoveP(isp, n - 1 - i, &done));
        PERF_ROW(mod, "intsetAdd", n, intsetAdd(&is, i, &done); sink += done);
        PERF_ROW(mod, "intsetRemove", n, intsetRemove(&is, n - 1 - i, &done);
                 sink += done);
        PERF_ROW(
            mod, "intsetFindThenAdd", n, if (!intsetFind(is, i)) {
                intsetAdd(&is, i, &done);
                sink += done;
            });
        PERF_ROW(
            mod, "intsetFindThenRemove", n, if (intsetFind(is, n - 1 - i)) {
                intsetRemove(&is, n - 1 - i, &done);
                sink += done;
            });

        /* The log write alone: an intset op's one int64 */
        PERF_ROW(mod, "logInt64", n, const int64_t v = i;
                 sink += persistCtxLogOp(ictx, PERSIST_OP_INSERT, &v, 1));
        intsetCloseP(isp);
    }
    persistCtxFree(ictx);

    persistCtx *octx = perfPersistCtx(&tmp, "multiOrderedSetP");
    multiOrderedSetP *mosp = octx ? multiOrderedSetNewP(octx) : nullptr;
    cleaner(multiOrderedSetFree) multiOrderedSet *mos = multiOrderedSetNew();
    if (mosp) {
        for (uint32_t i = 0; i < n; i++) {
            const databox k = perfContainerKey(keys, i);
            const databox s = databoxNewSigned(i);
            sink += multiOrderedSetAddP(mosp, &s, &k, &done);
        }
        perfFillOrderedSet(&mos, keys);
        PERF_ROW(mod, "mosRemoveP", n,
                 const databox k = perfContainerKey(keys, perfScatter(i));
                 sink += multiOrderedSetRemoveP(mosp, &k));
        PERF_ROW(mod, "mosRemove", n,
                 const databox k = perfContainerKey(keys, perfScatter(i));
                 sink += multiOrderedSetRemove(&mos, &k));
        perfFillOrderedSet(&mos, keys);
        PERF_ROW(
            mod, "mosExistsThenRemove", n,
            const databox k = perfContainerKey(keys, perfScatter(i));
            if (multiOrderedSetExists(mos, &k)) {
                sink += multiOrderedSetRemove(&mos, &k);
            });
        multiOrderedSetCloseP(mosp);
    }
    persistCtxFree(octx);

    /* Rows of (key, counter): each increment adds 1 to the counter, so
     * a full-width delete names the counter's value by then */
    persistCtx *mctx = perfPersistCtx(&tmp, "multimapP");
    multimapP *mp = mctx ? multimapNewP(mctx, 2) : nullptr;
    cleaner(multimapFree) multimap *m = multimapNew(2);
    if (mp) {
        int64_t now = 0;
        PERF_ROW(mod, "mapInsertIfNotExistsP", n,
                 const databox k = perfContainerKey(keys, perfScatter(i));
                 const databox v = databoxNewSigned(perfScatter(i));
                 const databox *row[2] = {&k, &v};
                 sink += multimapInsertIfNotExistsP(mp, row));
        PERF_ROW(mod, "mapFieldIncrP", n,
                 const databox k = perfContainerKey(keys, perfScatter(i));
                 sink += multimapFieldIncrP(mp, &k, 1, 1, &now));
        PERF_ROW(mod, "mapDeleteFullWidthP", n,
                 const databox k = perfContainerKey(keys, perfScatter(i));
                 const databox v = databoxNewSigned(perfScatter(i) + 1);
                 const databox *row[2] = {&k, &v};
                 sink += multimapDeleteFullWidthP(mp, row));

        PERF_ROW(mod, "mapInsert", n,
                 const databox k = perfContainerKey(keys, perfScatter(i));
                 const databox v = databoxNewSigned(perfScatter(i));
                 const databox *row[2] = {&k, &v};
                 sink += multimapInsert(&m, row));
        PERF_ROW(mod, "mapFieldIncr", n,
                 const databox k = perfContainerKey(keys, perfScatter(i));
                 sink += multimapFieldIncrBy(&m, &k, 1, 1, &now));
        PERF_ROW(
            mod, "mapFieldReadThenIncr", n,
            const databox k = perfContainerKey(keys, perfScatter(i));
            if (multimapFieldIncrBy(&m, &k, 1, 0, &now)) {
                sink += multimapFieldIncrBy(&m, &k, 1, 1, &now);
            });
        PERF_ROW(mod, "mapDeleteFullWidth", n,
                 const databox k = perfContainerKey(keys, perfScatter(i));
                 const databox v = databoxNewSigned(perfScatter(i) + 2);
                 const databox *row[2] = {&k, &v};
                 sink += multimapDeleteFullWidth(&m, row));
        PERF_ROW(
            mod, "mapExistsThenInsert", n,
            const databox k = perfContainerKey(keys, perfScatter(i));
            const databox v = databoxNewSigned(perfScatter(i));
            const databox *row[2] = {&k, &v};
            if (!multimapExists(m, &k)) { sink += multimapInsert(&m, row); });
        PERF_ROW(
            mod, "mapExistsThenDeleteFullWidth", n,
            const databox k = perfContainerKey(keys, perfScatter(i));
            const databox v = databoxNewSigned(perfScatter(i));
            const databox *row[2] = {&k, &v};
            if (multimapExistsFullWidth(m, row)) {
                sink += multimapDeleteFullWidth(&m, row);
            });
        PERF_ROW(mod, "mapInsertIfAbsent", n,
                 const databox k = perfContainerKey(keys, perfScatter(i));
                 const databox v = databoxNewSigned(perfScatter(i));
                 const databox *row[2] = {&k, &v};
                 sink += multimapInsertIfAbsent(&m, row));

        /* The log write alone: a field increment's three boxes */
        PERF_ROW(mod, "logBoxes3", n,
                 const databox k = perfContainerKey(keys, perfScatter(i));
                 const databox offset = DATABOX_UNSIGNED(1);
                 const databox delta = DATABOX_SIGNED(1);
                 const databox *args[3] = {&k, &offset, &delta};
                 sink += persistCtxLogOp(mctx, PERSIST_OP_NUM_INCR, args, 3));

        /* What that log's body costs to encode: the three boxes as an
         * exact flex, made and freed */
        PERF_ROW(mod, "encodeBoxes3", n,
                 const databox k = perfContainerKey(keys, perfScatter(i));
                 const databox offset = DATABOX_UNSIGNED(1);
                 const databox delta = DATABOX_SIGNED(1);
                 const databox *args[3] = {&k, &offset, &delta};
                 flex *f = flexNewFromDataboxPtrsExact(args, 3);
                 sink += flexBytes(f); flexFree(f));
        multimapCloseP(mp);
    }
    persistCtxFree(mctx);

    /* Bit positions three apart, so sets and clears reach many words */
    persistCtx *bctx = perfPersistCtx(&tmp, "multiroarP");
    multiroarP *rp = bctx ? multiroarNewP(bctx) : nullptr;
    cleaner(multiroarFree) multiroar *r = multiroarBitNew();
    if (rp) {
        PERF_ROW(mod, "bitSetP", n,
                 sink += multiroarBitSetP(rp, perfScatter(i) * 3ull, &done));
        PERF_ROW(mod, "bitRemoveP", n,
                 sink += multiroarBitClearP(rp, perfScatter(i) * 3ull, &done));
        PERF_ROW(mod, "bitSet", n,
                 sink += multiroarBitSet(r, perfScatter(i) * 3ull));
        PERF_ROW(mod, "bitRemove", n,
                 sink += multiroarBitClear(r, perfScatter(i) * 3ull));
        PERF_ROW(
            mod, "bitGetThenSet", n,
            if (!multiroarBitGet(r, perfScatter(i) * 3ull)) {
                sink += multiroarBitSet(r, perfScatter(i) * 3ull);
            });
        PERF_ROW(
            mod, "bitGetThenRemove", n,
            if (multiroarBitGet(r, perfScatter(i) * 3ull)) {
                sink += multiroarBitClear(r, perfScatter(i) * 3ull);
            });

        /* One bit per 8192-bit chunk: every set makes its chunk, every
         * remove deletes it */
        PERF_ROW(mod, "bitSetNewChunk", n,
                 sink += multiroarBitSet(r, perfScatter(i) * 8192ull));
        PERF_ROW(mod, "bitGetOwnChunk", n,
                 sink += multiroarBitGet(r, perfScatter(i) * 8192ull));
        PERF_ROW(mod, "bitRemoveOwnChunk", n,
                 sink += multiroarBitClear(r, perfScatter(i) * 8192ull));
        multiroarCloseP(rp);
    }
    persistCtxFree(bctx);

    /* Value mode: one 8-bit value per position, every position filled */
    persistCtx *vctx = perfPersistCtx(&tmp, "multiroarValueP");
    multiroarP *vp = vctx ? multiroarValueNewP(vctx, 8, n, 1) : nullptr;
    cleaner(multiroarFree) multiroar *vr = multiroarValueNew(8, n, 1);
    if (vp && vr) {
        for (uint32_t i = 0; i < n; i++) {
            databox v = databoxNewUnsigned(i & 0xff);
            sink += multiroarValueSetP(vp, i, &v);
            sink += multiroarValueSet(vr, i, &v);
        }
        PERF_ROW(mod, "valueRemoveP", n,
                 sink += multiroarValueRemoveP(vp, perfScatter(i), &done));
        PERF_ROW(mod, "valueRemove", n,
                 sink +=
                 multiroarValueRemoveGetRemoved(vr, perfScatter(i), nullptr));
        PERF_ROW(mod, "valueSet", n, databox v = databoxNewUnsigned(i & 0xff);
                 sink += multiroarValueSet(vr, perfScatter(i), &v));
        PERF_ROW(mod, "valueReplace", n,
                 databox v = databoxNewUnsigned(~i & 0xff);
                 sink += multiroarValueSet(vr, perfScatter(i), &v));
        PERF_ROW(
            mod, "valueGetThenRemove", n,
            if (multiroarValueGet(vr, perfScatter(i), nullptr)) {
                sink +=
                    multiroarValueRemoveGetRemoved(vr, perfScatter(i), nullptr);
            });
    }
    multiroarCloseP(vp);
    persistCtxFree(vctx);
    perfContainerSink = sink;
}

/* A 600000-member set, past the size where the Full tier's sub-map count
 * once stopped growing: adds of the last 100000, then ranks and score
 * lookups spread over the set (only when asked for by name) */
static void benchMultiOrderedSetLarge(void) {
    const uint32_t n = 600'000;
    const uint32_t timed = 100'000;
    cleaner(multiOrderedSetFree) multiOrderedSet *mos = multiOrderedSetNew();
    uint64_t sink = 0;
    char key[24];
    for (uint32_t i = 0; i < n - timed; i++) {
        const int klen = snprintf(key, sizeof(key), "large-%07u", i);
        const databox m = databoxNewBytes(key, klen);
        const databox s = databoxNewSigned((int64_t)((i * 7919ull) % n));
        sink += multiOrderedSetAdd(&mos, &s, &m);
    }

    double t0 = timeUtilMonotonicSec();
    for (uint32_t i = n - timed; i < n; i++) {
        const int klen = snprintf(key, sizeof(key), "large-%07u", i);
        const databox m = databoxNewBytes(key, klen);
        const databox s = databoxNewSigned((int64_t)((i * 7919ull) % n));
        sink += multiOrderedSetAdd(&mos, &s, &m);
    }
    emitRow("multiOrderedSetLarge", "add600k", 0, timed,
            timeUtilMonotonicSec() - t0);

    const uint32_t ranks = 2000;
    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < ranks; i++) {
        const int klen =
            snprintf(key, sizeof(key), "large-%07u", (i * 7919u) % n);
        const databox m = databoxNewBytes(key, klen);
        sink += (uint64_t)multiOrderedSetGetRank(mos, &m);
    }
    emitRow("multiOrderedSetLarge", "getRank600k", 0, ranks,
            timeUtilMonotonicSec() - t0);

    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < ranks; i++) {
        databox member;
        databox score;
        sink += multiOrderedSetGetByRank(mos, (int64_t)((i * 7919u) % n),
                                         &member, &score);
    }
    emitRow("multiOrderedSetLarge", "getByRank600k", 0, ranks,
            timeUtilMonotonicSec() - t0);

    perfContainerSink = sink;
}

/* A set of 'members' members (scores spaced 16 apart), small enough to
 * stay in the tier 'tier' names, driven PERF_CONTAINER_N operations at a
 * time: increments that keep each member's place, scattered updates that
 * move it, and AddNX offers of present members */
static void benchMultiOrderedSetTier(const char *tier, uint32_t members) {
    cleanerName(zfree, PerfKeys) char *keys = perfContainerKeys();
    const uint32_t n = PERF_CONTAINER_N;
    cleaner(multiOrderedSetFree) multiOrderedSet *mos = multiOrderedSetNew();
    uint64_t sink = 0;
    for (uint32_t i = 0; i < members; i++) {
        const databox s = databoxNewSigned((int64_t)i * 16);
        const databox m = perfContainerKey(keys, i);
        sink += multiOrderedSetAdd(&mos, &s, &m);
    }

    char name[48];
    double t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox delta = databoxNewSigned(1);
        const databox m = perfContainerKey(keys, i % members);
        databox result = {};
        sink += multiOrderedSetIncrBy(&mos, &delta, &m, &result);
    }
    snprintf(name, sizeof(name), "%sIncrBy", tier);
    emitRow("multiOrderedSet", name, 0, n, timeUtilMonotonicSec() - t0);

    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox s =
            databoxNewSigned((int64_t)((i * 7919u) % (members * 16)));
        const databox m = perfContainerKey(keys, i % members);
        sink += multiOrderedSetAdd(&mos, &s, &m);
    }
    snprintf(name, sizeof(name), "%sUpdate", tier);
    emitRow("multiOrderedSet", name, 0, n, timeUtilMonotonicSec() - t0);

    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox s = databoxNewSigned(i);
        const databox m = perfContainerKey(keys, i % members);
        sink += multiOrderedSetAddNX(&mos, &s, &m);
    }
    snprintf(name, sizeof(name), "%sAddNXPresent", tier);
    emitRow("multiOrderedSet", name, 0, n, timeUtilMonotonicSec() - t0);

    perfContainerSink = sink;
}

/* A multiarray of 'n' 8-byte elements, 64 per node, held in the tier
 * 'tier' names (64 or fewer: Small; up to 64 nodes: Medium; past that:
 * Large): PERF_CONTAINER_N reads at scattered indexes, then as many
 * Insert + Pop pairs at scattered indexes (the size stays 'n') */
static void benchMultiarrayTier(const char *tier, uint32_t n) {
    const uint32_t ops = PERF_CONTAINER_N;
    cleaner(multiarrayFree) multiarray *m = multiarrayNew(sizeof(uint64_t), 64);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t v = i;
        multiarrayInsert(&m, i, &v);
    }

    char name[48];
    uint64_t sink = 0;
    double t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < ops; i++) {
        const uint64_t *at = multiarrayGet(m, (i * 7919u) % n);
        sink += *at;
    }
    snprintf(name, sizeof(name), "%sGet", tier);
    emitRow("multiarray", name, 0, ops, timeUtilMonotonicSec() - t0);

    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < ops; i++) {
        const uint32_t idx = (i * 7919u) % n;
        uint64_t v = i;
        multiarrayInsert(&m, idx, &v);
        multiarrayPop(&m, idx, &v);
        sink += v;
    }
    snprintf(name, sizeof(name), "%sInsertPop", tier);
    emitRow("multiarray", name, 0, ops, timeUtilMonotonicSec() - t0);

    perfContainerSink = sink;
}

/* PERF_CONTAINER_N integers of varying widths pushed onto the tail */
static void perfFillList(multilist **ml, mflexState *s) {
    for (uint32_t i = 0; i < PERF_CONTAINER_N; i++) {
        const databox b = databoxNewSigned((int64_t)i * 1000003);
        multilistPushByTypeTail(ml, s, &b);
    }
}

/* A multilist at the default size limit used as a queue and as a list:
 * tail pushes, scattered index reads, drains from the head and from the
 * tail, and scattered single deletes (each drain empties the list and an
 * untimed refill follows) */
static void benchMultilist(void) {
    const uint32_t n = PERF_CONTAINER_N;
    cleaner(mflexStateFree) mflexState *s = mflexStateCreate();
    cleaner(multilistFree) multilist *ml =
        multilistNewLimit(FLEX_CAP_LEVEL_2048, 0);
    uint64_t sink = 0;

    PERF_ROW("multilist", "pushTail", n,
             const databox b = databoxNewSigned((int64_t)i * 1000003);
             multilistPushByTypeTail(&ml, s, &b));
    PERF_ROW("multilist", "indexScattered", n, multilistEntry e;
             sink += multilistIndexGet(ml, s, perfScatter(i), &e)
                         ? (uint64_t)e.box.data.i
                         : 0);
    PERF_ROW("multilist", "popHead", n, databox got = {};
             sink += multilistPopHead(&ml, s, &got); databoxFreeData(&got));
    perfFillList(&ml, s);
    PERF_ROW("multilist", "popTail", n, databox got = {};
             sink += multilistPopTail(&ml, s, &got); databoxFreeData(&got));
    perfFillList(&ml, s);
    PERF_ROW("multilist", "deleteScattered", n,
             sink +=
             multilistDelRange(&ml, s, (int64_t)((i * 7919u) % (n - i)), 1));
    perfContainerSink = sink;
}

/* An LRU of 'n' entries ('n' picks the entry width: 5 bytes up to 64K
 * entries, 6 bytes to 1M): inserts, promotions of scattered entries,
 * deletes of every fourth entry, and evicting the rest, each running
 * through the per-entry link and flag accessors.  The bytes column is
 * 'n'. */
static void benchMultilru(const uint32_t n) {
    cleaner(multilruFree) multilru *mlru = multilruNew();
    /* @own: scope; free=zfree */
    cleanerName(zfree, PerfLruPtrs) multilruPtr *ptrs =
        zcalloc(n, sizeof(*ptrs));
    uint64_t sink = 0;

    double t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        ptrs[i] = multilruInsert(mlru);
    }
    emitRow("multilru", "insert", n, n, timeUtilMonotonicSec() - t0);

    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        multilruIncrease(mlru, ptrs[(i * 7919u) % n]);
    }
    emitRow("multilru", "increase", n, n, timeUtilMonotonicSec() - t0);

    const uint32_t deletes = n / 4;
    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < deletes; i++) {
        /* every fourth entry, n / 4 distinct ones */
        multilruDelete(mlru, ptrs[(uint32_t)(((uint64_t)i * 4 + 1) % n)]);
    }
    emitRow("multilru", "delete", n, deletes, timeUtilMonotonicSec() - t0);

    const size_t left = multilruCount(mlru);
    t0 = timeUtilMonotonicSec();
    multilruPtr out;
    while (multilruRemoveMinimum(mlru, &out)) {
        sink += out;
    }
    emitRow("multilru", "removeMinimum", n, (uint32_t)left,
            timeUtilMonotonicSec() - t0);
    perfContainerSink = sink;
}

/* A reusing atom table of PERF_CONTAINER_N string atoms with the reverse
 * index 'index', its rows under 'module': interning new and present
 * values, retains and releases by value (the last release deletes), the
 * count update alone, and a snapshot and restore */
static void benchAtomTable(const char *module, const multimapAtomIndex index,
                           const char *keys) {
    const uint32_t n = PERF_CONTAINER_N;
    cleaner(multimapAtomFree) multimapAtom *ma =
        multimapAtomNewIndexed(index, false);
    uint64_t sink = 0;

    double t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        databox k = perfContainerKey(keys, i);
        sink += multimapAtomInsertIfNewConvert(ma, &k);
    }
    emitRow(module, "internNew", 0, n, timeUtilMonotonicSec() - t0);

    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        databox k = perfContainerKey(keys, i);
        sink += multimapAtomInsertIfNewConvert(ma, &k);
    }
    emitRow(module, "internPresent", 0, n, timeUtilMonotonicSec() - t0);

    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        databox k = perfContainerKey(keys, i);
        sink += multimapAtomInsertIfNewConvertAndRetain(ma, &k);
    }
    emitRow(module, "retain", 0, n, timeUtilMonotonicSec() - t0);

    /* The count update alone, by reference: the part of a retain after the
     * value is found (then undone by reference, untimed) */
    cleanerName(zfree, PerfIds) uint64_t *ids = zcalloc(n, sizeof(uint64_t));
    for (uint32_t i = 0; i < n; i++) {
        databox k = perfContainerKey(keys, i);
        sink += multimapAtomInsertIfNewConvert(ma, &k);
        ids[i] = k.data.u;
    }

    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        multimapAtomRetainById(ma, ids[i]);
    }
    emitRow(module, "retainById", 0, n, timeUtilMonotonicSec() - t0);
    for (uint32_t i = 0; i < n; i++) {
        const databox ref = {.data.u = ids[i],
                             .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};
        sink += multimapAtomReleaseById(ma, &ref);
    }

    /* A reference read back to its value: what every holder of an id pays
     * to use it (the forward rows alone, the same under every index) */
    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox ref = {.data.u = ids[i],
                             .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};
        databox value = {};
        sink += multimapAtomLookup(ma, &ref, &value);
        sink += databoxLen(&value);
    }
    emitRow(module, "lookupById", 0, n, timeUtilMonotonicSec() - t0);

    /* The same reads and finds in scattered order, as holders of ids
     * spread through a program make them (the rows above walk ids and
     * values in insertion order, which keeps the maps' recent nodes
     * cached) */
    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox ref = {.data.u = ids[perfScatter(i)],
                             .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};
        databox value = {};
        sink += multimapAtomLookup(ma, &ref, &value);
        sink += databoxLen(&value);
    }
    emitRow(module, "lookupByIdScattered", 0, n, timeUtilMonotonicSec() - t0);

    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox k = perfContainerKey(keys, perfScatter(i));
        databox ref = {};
        sink += multimapAtomLookupReference(ma, &k, &ref);
    }
    emitRow(module, "findScattered", 0, n, timeUtilMonotonicSec() - t0);

    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox k = perfContainerKey(keys, i);
        sink += multimapAtomRelease(ma, &k);
    }
    emitRow(module, "release", 0, n, timeUtilMonotonicSec() - t0);

    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox k = perfContainerKey(keys, i);
        sink += multimapAtomRelease(ma, &k);
    }
    emitRow(module, "releaseLast", 0, n, timeUtilMonotonicSec() - t0);

    for (uint32_t i = 0; i < n; i++) {
        databox k = perfContainerKey(keys, i);
        multimapAtomInsertConvert(ma, &k);
    }

    /* Persistence of the full table: one snapshot, one restore (ns/atom) */
    size_t snapLen = 0;
    t0 = timeUtilMonotonicSec();
    cleanerName(zfree, U8Buf) uint8_t *snap =
        persistOpsMultimapAtom.snapshot(ma, &snapLen);
    emitRow(module, "snapshot", 0, n, timeUtilMonotonicSec() - t0);

    t0 = timeUtilMonotonicSec();
    cleaner(multimapAtomFree) multimapAtom *restored =
        persistOpsMultimapAtom.restore(snap, snapLen);
    emitRow(module, "restore", 0, n, timeUtilMonotonicSec() - t0);
    sink += multimapAtomCount(restored);

    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        sink += multimapAtomDeleteById(ma, i);
    }
    emitRow(module, "deleteById", 0, n, timeUtilMonotonicSec() - t0);
    perfContainerSink = sink;
}

/* Rows of {value, count} addressed by dense ids, against a multimap of
 * {id, value, count} rows sorted by id holding the same data:
 * rows set in id order, read back in order and scattered, counts
 * incremented, rows deleted.  The set rows' bytes are each structure's
 * metered heap. */
static void benchIdRows(void) {
    cleanerName(zfree, PerfKeys) char *keys = perfContainerKeys();
    const uint32_t n = PERF_CONTAINER_N;
    uint64_t sink = 0;
    const databox one = {.type = DATABOX_FALSE};

    bool metered = ctestHeapMeterStart();
    cleaner(idRowsFree) idRows *r = idRowsNew(2);
    double t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox v = perfContainerKey(keys, i);
        const databox *row[2] = {&v, &one};
        sink += idRowsSet(r, i, row);
    }
    double secs = timeUtilMonotonicSec() - t0;
    emitRow("idRows", "set", perfHeapStop(metered), n, secs);
    PERF_ROW("idRows", "get", n, databox v = {}; databox c = {};
             databox * row[2] = {&v, &c}; sink += idRowsGet(r, i, row));
    PERF_ROW("idRows", "getScattered", n, databox v = {}; databox c = {};
             databox * row[2] = {&v, &c};
             sink += idRowsGet(r, perfScatter(i), row));
    PERF_ROW("idRows", "incr", n, int64_t now = 0;
             sink += idRowsFieldIncr(r, perfScatter(i), 1, 1, &now));
    PERF_ROW("idRows", "delete", n, sink += idRowsDelete(r, perfScatter(i)));

    metered = ctestHeapMeterStart();
    cleaner(multimapFree) multimap *m =
        multimapNewLimit(3, dkTierDefaultLimit(DK_TIER_MULTIMAP_ATOM));
    t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < n; i++) {
        const databox id = {.data.u = i,
                            .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};
        const databox v = perfContainerKey(keys, i);
        const databox *row[3] = {&id, &v, &one};
        sink += multimapInsert(&m, row);
    }
    secs = timeUtilMonotonicSec() - t0;
    emitRow("idRows", "multimapSet", perfHeapStop(metered), n, secs);
    PERF_ROW("idRows", "multimapGet", n,
             const databox id = {.data.u = i,
                                 .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};
             databox v = {}; databox c = {}; databox * row[2] = {&v, &c};
             sink += multimapLookup(m, &id, row));
    PERF_ROW("idRows", "multimapGetScattered", n,
             const databox id = {.data.u = perfScatter(i),
                                 .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};
             databox v = {}; databox c = {}; databox * row[2] = {&v, &c};
             sink += multimapLookup(m, &id, row));
    PERF_ROW("idRows", "multimapIncr", n,
             const databox id = {.data.u = perfScatter(i),
                                 .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};
             sink += (uint64_t)multimapFieldIncr(&m, &id, 2, 1));
    PERF_ROW("idRows", "multimapDelete", n,
             const databox id = {.data.u = perfScatter(i),
                                 .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};
             sink += multimapDelete(&m, &id));
    perfContainerSink = sink;
}

/* References read back to their values and nothing else (run by name,
 * for profiling the read path): 100k string atoms, 60 scattered passes
 * over every id (long enough for a sampler that attaches by pid) */
static void benchMultimapAtomRead(void) {
    cleanerName(zfree, PerfKeys) char *keys = perfContainerKeys();
    const uint32_t n = PERF_CONTAINER_N;
    cleaner(multimapAtomFree) multimapAtom *ma = multimapAtomNew();
    for (uint32_t i = 0; i < n; i++) {
        databox k = perfContainerKey(keys, i);
        multimapAtomInsertConvert(ma, &k);
    }

    uint64_t sink = 0;
    constexpr uint32_t passes = 60;
    const double t0 = timeUtilMonotonicSec();
    for (uint32_t p = 0; p < passes; p++) {
        for (uint32_t i = 0; i < n; i++) {
            const databox ref = {.data.u = perfScatter(i),
                                 .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};
            databox value = {};
            sink += multimapAtomLookup(ma, &ref, &value);
            sink += databoxLen(&value);
        }
    }
    emitRow("multimapAtomRead", "lookupByIdScattered", 0, passes * n,
            timeUtilMonotonicSec() - t0);
    perfContainerSink = sink;
}

/* A multimap of {reference id, string, count} rows read by id, in id
 * order (run by name, for profiling a find keyed by references): 100k
 * rows, 60 passes */
static void benchMultimapRefRead(void) {
    cleanerName(zfree, PerfKeys) char *keys = perfContainerKeys();
    const uint32_t n = PERF_CONTAINER_N;
    const databox one = {.type = DATABOX_FALSE};
    cleaner(multimapFree) multimap *m =
        multimapNewLimit(3, dkTierDefaultLimit(DK_TIER_MULTIMAP_ATOM));
    uint64_t sink = 0;
    for (uint32_t i = 0; i < n; i++) {
        const databox id = {.data.u = i,
                            .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};
        const databox v = perfContainerKey(keys, i);
        const databox *row[3] = {&id, &v, &one};
        sink += multimapInsert(&m, row);
    }

    constexpr uint32_t passes = 60;
    const double t0 = timeUtilMonotonicSec();
    for (uint32_t p = 0; p < passes; p++) {
        for (uint32_t i = 0; i < n; i++) {
            const databox id = {.data.u = i,
                                .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};
            databox v = {};
            databox c = {};
            databox *row[2] = {&v, &c};
            sink += multimapLookup(m, &id, row);
        }
    }
    emitRow("multimapRefRead", "lookupById", 0, passes * n,
            timeUtilMonotonicSec() - t0);
    perfContainerSink = sink;
}

/* The atom table under each reverse index (the default, hashed, then
 * ordered and dict), then the lowest-first table and the string pool */
static void benchMultimapAtom(void) {
    cleanerName(zfree, PerfKeys) char *keys = perfContainerKeys();
    benchAtomTable("multimapAtom", MULTIMAP_ATOM_INDEX_HASHED, keys);
    benchAtomTable("multimapAtomOrdered", MULTIMAP_ATOM_INDEX_ORDERED, keys);
    benchAtomTable("multimapAtomDict", MULTIMAP_ATOM_INDEX_DICT, keys);

    const uint32_t n = PERF_CONTAINER_N;
    uint64_t sink = 0;
    double t0;

    /* Interning into a lowest-first table: new values, then present ones */
    cleaner(multimapAtomDenseFree) multimapAtomDense *interned =
        multimapAtomDenseNew();
    PERF_ROW("multimapAtomDense", "internNew", n,
             databox k = perfContainerKey(keys, i);
             sink +=
             multimapAtomDenseInsertIfNewConvertAndRetain(interned, &k));
    PERF_ROW("multimapAtomDense", "internPresent", n,
             databox k = perfContainerKey(keys, i);
             sink +=
             multimapAtomDenseInsertIfNewConvertAndRetain(interned, &k));

    /* The same values in a lowest-first table, with every fourth one
     * released so the restore rebuilds holes */
    cleaner(multimapAtomDenseFree) multimapAtomDense *dense =
        multimapAtomDenseNew();
    for (uint32_t i = 0; i < n; i++) {
        databox k = perfContainerKey(keys, i);
        multimapAtomDenseInsertConvert(dense, &k);
    }
    for (uint32_t i = 0; i < n; i += 4) {
        multimapAtomDenseDeleteById(dense, i);
    }

    size_t denseLen = 0;
    t0 = timeUtilMonotonicSec();
    cleanerName(zfree, U8Buf) uint8_t *denseSnap =
        persistOpsMultimapAtomDense.snapshot(dense, &denseLen);
    emitRow("multimapAtomDense", "snapshot", 0, n, timeUtilMonotonicSec() - t0);

    t0 = timeUtilMonotonicSec();
    cleaner(multimapAtomDenseFree) multimapAtomDense *denseRestored =
        persistOpsMultimapAtomDense.restore(denseSnap, denseLen);
    emitRow("multimapAtomDense", "restore", 0, n, timeUtilMonotonicSec() - t0);
    sink += multimapAtomDenseCount(denseRestored);

    /* The hash-backed pool of the same strings */
    cleaner(stringPoolFree) stringPool *pool = stringPoolNew();
    for (uint32_t i = 0; i < n; i++) {
        const databox k = perfContainerKey(keys, i);
        sink += stringPoolIntern(pool, &k);
    }

    size_t poolLen = 0;
    t0 = timeUtilMonotonicSec();
    cleanerName(zfree, U8Buf) uint8_t *poolSnap =
        persistOpsStringPool.snapshot(pool, &poolLen);
    emitRow("stringPool", "snapshot", 0, n, timeUtilMonotonicSec() - t0);

    t0 = timeUtilMonotonicSec();
    cleaner(stringPoolFree) stringPool *poolRestored =
        persistOpsStringPool.restore(poolSnap, poolLen);
    emitRow("stringPool", "restore", 0, n, timeUtilMonotonicSec() - t0);
    sink += stringPoolCount(poolRestored);

    perfContainerSink = sink;
}

int perfRegressionTest(int, char *[]) {
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
        const uint32_t n_sizes = (uint32_t)(countof(sizes_hash));

        for (uint32_t i = 0; perfWanted("blake3") && i < n_sizes; i++) {
            benchBlake3(sizes_hash[i]);
        }
        for (uint32_t i = 0; perfWanted("sha256") && i < n_sizes; i++) {
            benchSha256(sizes_hash[i]);
        }
        for (uint32_t i = 0; perfWanted("xxh64") && i < n_sizes; i++) {
            benchXxh64(sizes_hash[i]);
        }
        for (uint32_t i = 0; perfWanted("xxh3_64") && i < n_sizes; i++) {
            benchXxh3_64(sizes_hash[i]);
        }
        for (uint32_t i = 0; perfWanted("xxh3_128") && i < n_sizes; i++) {
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
            perfWanted("rapidhash") ? (uint32_t)(countof(sizes_rh)) : 0;
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
             perfWanted("rapidhash") && i < countof(sizes_rh_stream); i++) {
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
        if (perfWanted("sha256")) {
            benchSha256Multi4(64u);
            benchSha256Multi4(65536u);
        }

        /* HMAC: prepared-key advantage shows on small messages. */
        const size_t sizes_hmac[] = {64u, 256u, 1024u, 65536u};
        for (uint32_t i = 0;
             perfWanted("hmac") && i < (uint32_t)(countof(sizes_hmac)); i++) {
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
        for (uint32_t a = 0; perfWanted("envelope") && a < countof(algos);
             a++) {
            for (uint32_t s = 0; s < n_sizes; s++) {
                benchEnvelope(algos[a].algo, algos[a].label, sizes_hash[s]);
            }
        }

        /* AEAD across the same sizes. */
        for (uint32_t i = 0; perfWanted("aead") && i < n_sizes; i++) {
            benchAead(sizes_hash[i]);
        }

        /* End-to-end cluster mailbox throughput on a handful of
         * representative message sizes. */
        const size_t sizes_mb[] = {64u, 1024u, 65536u};
        for (uint32_t i = 0;
             perfWanted("mailbox") && i < (uint32_t)(countof(sizes_mb)); i++) {
            benchMailbox(sizes_mb[i]);
        }

        /* Containers whose layered operations are tuned */
        if (perfWanted("multidict")) {
            benchMultidict();
        }

        if (perfNamed("multiOrderedSetLarge")) {
            benchMultiOrderedSetLarge();
        }

        if (perfNamed("multidictP")) {
            benchMultidictP();
        }

        if (perfNamed("multidictLoad")) {
            benchMultidictLoad();
        }

        if (perfNamed("multidictLimits")) {
            benchMultidictLimits();
        }

        if (perfNamed("multidictSeeds")) {
            benchMultidictSeeds();
        }

        if (perfNamed("persistChecks")) {
            benchPersistChecks();
        }

        if (perfNamed("multimapLimits")) {
            benchMultimapLimits();
        }

        if (perfNamed("multimapAtomLimits")) {
            benchMultimapAtomLimits();
        }

        if (perfNamed("multiOrderedSetLimits")) {
            benchMultiOrderedSetLimits();
        }

        if (perfNamed("multilistLimits")) {
            benchMultilistLimits();
        }

        if (perfWanted("multiroar")) {
            benchMultiroar();
        }

        if (perfWanted("numeric")) {
            benchNumeric();
        }

        if (perfWanted("fenwick")) {
            benchFenwick();
        }

        if (perfWanted("multiOrderedSet")) {
            benchMultiOrderedSet();
            benchMultiOrderedSetTier("small", 48);
            benchMultiOrderedSetTier("medium", 160);
        }

        if (perfWanted("multimapAtom")) {
            benchMultimapAtom();
        }

        if (perfNamed("multimapAtomRead")) {
            benchMultimapAtomRead();
        }

        if (perfNamed("multimapRefRead")) {
            benchMultimapRefRead();
        }

        if (perfWanted("idRows")) {
            benchIdRows();
        }

        if (perfWanted("multiarray")) {
            benchMultiarrayTier("small", 48);
            benchMultiarrayTier("medium", 2048);
            benchMultiarrayTier("large", 16384);
        }

        if (perfWanted("multilist")) {
            benchMultilist();
        }

        if (perfWanted("multilru")) {
            benchMultilru(50'000);
            benchMultilru(PERF_CONTAINER_N);
        }

        /* membound allocator: per-op alloc/free cost for the three
         * dynamic tiers — STANDARD (global lock), ENTERPRISE (per-extent
         * locks), and ENTERPRISE+threadCache (the lock-free fast path).
         * Single-thread by design; the MT comparison is in memboundBench. */
        const size_t sizes_mb_alloc[] = {64u, 256u, 1024u};
        const uint32_t n_mb_alloc =
            perfWanted("membound") ? (uint32_t)(countof(sizes_mb_alloc)) : 0;
        membound *mbStd = memboundCreateDynamic(8u << 20, 0, 8u << 20);
        membound *mbEnt = memboundCreateEnterprise(8u << 20, 0);
        const memboundConfig mbTcCfg = {.mode = MEMBOUND_MODE_DYNAMIC,
                                        .initialSize = 8u << 20,
                                        .tier = MEMBOUND_TIER_ENTERPRISE,
                                        .threadSafe = true,
                                        .growthSize = 8u << 20,
                                        .threadCache = true};
        membound *mbTc = memboundCreateWithConfig(&mbTcCfg);
        for (uint32_t i = 0; i < n_mb_alloc; i++) {
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
