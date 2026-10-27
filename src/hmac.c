/* HMAC (RFC 2104) — hash-agnostic implementation.
 *
 * The hash primitive is supplied by an hmacHashAlgo vtable, so this single
 * file implements HMAC for SHA-256, BLAKE3, or any future hash that fits
 * the init/update/final mould. All working storage (two hash contexts plus
 * an ipad/opad scratch region) lives in a single caller-owned buffer; the
 * hot path performs no heap allocation. */

#include "hmac.h"

#include "blake3.h"
#include "cleaner.h"
#include "ctmem.h" /* dkSecureZero */
#include "datakit.h"
#include "hash/sha256ArmCrypto.h"
#include "hash/sha256ShaNi.h"
#include "sha256.h"

#include <string.h>

/* Scope-exit cleaner for the overflow heap storage buffers (see
 * cleaner.h idioms). */
defcleanerName(uint8_t *, zfree, Bytes);

#ifdef DATAKIT_TEST
/* Scope-exit cleaner for a fixed 4-lane array of owned buffers
 * (defcleanerName only covers single pointers). */
DK_FN_UNUSED static void freeLaneStorage4_cleanup(uint8_t *(*lanes)[4]) {
    for (uint32_t lane = 0; lane < 4; lane++) {
        if ((*lanes)[lane]) {
            zfree((*lanes)[lane]);
        }
    }
}
#endif

/* RFC 2104 §2: inner pad byte 0x36 and outer pad byte 0x5c are XOR'd into
 * every byte of the (zero-padded) key to derive the inner and outer keys. */
static constexpr int HMAC_IPAD = 0x36;
static constexpr int HMAC_OPAD = 0x5c;

/* Cap for the in-storage scratch region holding ipad/opad keys. RFC 2104
 * does not bound the hash's block size in principle, but every real-world
 * hash we care about has block_size <= 256. The runtime check in hmacInit
 * will refuse anything larger rather than silently overflow. */
static constexpr int HMAC_MAX_BLOCK = 256;

size_t hmacCtxSize(const hmacHashAlgo *algo) {
    /* Layout: [inner ctx][outer ctx][scratch sized to 2 * block_size].
     * The scratch holds the derived inner+outer key blocks long enough to
     * seed both hash contexts; we keep them around in case a future caller
     * wants to reset/replay without re-deriving. */
    return algo->ctx_size * 2 + algo->block_size * 2;
}

void hmacInit(hmacCtx *ctx, const hmacHashAlgo *algo, void *storage,
              size_t storage_size, const uint8_t *key, size_t key_len) {
    /* Defensive: a misconfigured caller should fail loudly via memset rather
     * than scribble into the wrong region. */
    if (storage_size < hmacCtxSize(algo) || algo->block_size > HMAC_MAX_BLOCK) {
        memset(ctx, 0, sizeof(*ctx));
        return;
    }

    uint8_t *base = (uint8_t *)storage;
    ctx->algo = algo;
    ctx->inner_ctx = base;
    ctx->outer_ctx = base + algo->ctx_size;
    ctx->scratch = base + algo->ctx_size * 2;

    uint8_t *ipad_key = ctx->scratch;
    uint8_t *opad_key = ctx->scratch + algo->block_size;

    /* RFC 2104 §2: if the key is longer than the hash block size, hash it
     * down first; otherwise zero-pad to one block. A 32- to 64-byte digest
     * fits trivially inside block_size for every standard hash. */
    uint8_t key_block[HMAC_MAX_BLOCK];
    memset(key_block, 0, algo->block_size);
    if (key_len > algo->block_size) {
        /* Use the inner_ctx storage as a temporary hash to derive K'. */
        algo->init(ctx->inner_ctx);
        algo->update(ctx->inner_ctx, key, key_len);
        algo->final(ctx->inner_ctx, key_block);
        /* Remaining bytes already zero from the memset above. */
    } else if (key_len > 0) {
        memcpy(key_block, key, key_len);
    }

    for (size_t i = 0; i < algo->block_size; i++) {
        ipad_key[i] = key_block[i] ^ HMAC_IPAD;
        opad_key[i] = key_block[i] ^ HMAC_OPAD;
    }

    /* Seed both contexts. The inner context is left "primed" with the ipad
     * key so subsequent hmacUpdate calls just feed message bytes; the outer
     * context is primed the same way with opad and finished later against
     * the inner digest in hmacFinal. */
    algo->init(ctx->inner_ctx);
    algo->update(ctx->inner_ctx, ipad_key, algo->block_size);

    algo->init(ctx->outer_ctx);
    algo->update(ctx->outer_ctx, opad_key, algo->block_size);

    /* Wipe the local key copy — it may contain the hashed-down K' for
     * oversized keys, which is sensitive. */
    dkSecureZero(key_block, sizeof(key_block));
}

void hmacUpdate(hmacCtx *ctx, const void *data, size_t len) {
    ctx->algo->update(ctx->inner_ctx, data, len);
}

void hmacFinal(hmacCtx *ctx, uint8_t *out) {
    /* inner = H(K' XOR ipad || message); then outer = H(K' XOR opad || inner).
     */
    uint8_t inner_digest[64]; /* large enough for SHA-512; HMAC_MAX_BLOCK
                                 overkill */
    ctx->algo->final(ctx->inner_ctx, inner_digest);
    ctx->algo->update(ctx->outer_ctx, inner_digest, ctx->algo->digest_size);
    ctx->algo->final(ctx->outer_ctx, out);
    dkSecureZero(inner_digest, sizeof(inner_digest));
    /* Wipe the K_ipad/K_opad blocks left in caller-owned scratch by
     * hmacInit. Those blocks are key-equivalent material (K' XOR pad)
     * and would otherwise persist for the entire caller storage
     * lifetime — a leaked ctx storage discloses them directly. The
     * prepared-key path (hmacInitFromPrepared) already wipes its
     * scratch region; matching that here keeps the contract uniform
     * regardless of which init was used.
     *
     * Callers MUST treat ctx as discarded after Final: subsequent
     * hmacUpdate / hmacFinal on the same ctx is undefined (scratch is
     * zero so a re-init would seed K' XOR 0x36 / 0x5C from an all-zero
     * key — silent wrong output rather than fail-closed). Re-initialise
     * via hmacInit / hmacInitFromPrepared before reuse. */
    if (ctx->scratch != nullptr && ctx->algo != nullptr) {
        dkSecureZero(ctx->scratch, ctx->algo->block_size * 2u);
    }
}

void hmacCompute(const hmacHashAlgo *algo, const uint8_t *key, size_t key_len,
                 const void *data, size_t data_len, uint8_t *out) {
    /* Fast path: keep everything on the stack for typical algorithms. The
     * 1024-byte threshold comfortably covers SHA-256 (2*112 + 2*64 = 352)
     * and most future hashes; only exotic large-state hashes hit zmalloc. */
    uint8_t stack_storage[1024];
    size_t need = hmacCtxSize(algo);
    void *storage = nullptr;
    cleanerName(zfree, Bytes) uint8_t *heap = nullptr;

    if (need <= sizeof(stack_storage)) {
        storage = stack_storage;
    } else {
        heap = (uint8_t *)zmalloc(need);
        storage = heap;
    }

    hmacCtx ctx;
    hmacInit(&ctx, algo, storage, need, key, key_len);
    hmacUpdate(&ctx, data, data_len);
    hmacFinal(&ctx, out);
}

/* ------------------------------------------------------------------------ */
/* Prepared-key API (RFC 2104 with cached K_ipad/K_opad block state)        */
/* ------------------------------------------------------------------------ */

size_t hmacPreparedSize(const hmacHashAlgo *algo) {
    return algo->ctx_size * 2;
}

void hmacPrepareKey(hmacKey *prep, const hmacHashAlgo *algo, void *storage,
                    size_t storage_size, const uint8_t *key, size_t key_len) {
    if (storage_size < hmacPreparedSize(algo) ||
        algo->block_size > HMAC_MAX_BLOCK) {
        memset(prep, 0, sizeof(*prep));
        return;
    }

    uint8_t *base = (uint8_t *)storage;
    prep->algo = algo;
    prep->inner_ctx = base;
    prep->outer_ctx = base + algo->ctx_size;

    /* Derive K' (same construction as hmacInit). */
    uint8_t key_block[HMAC_MAX_BLOCK];
    memset(key_block, 0, algo->block_size);
    if (key_len > algo->block_size) {
        algo->init(prep->inner_ctx);
        algo->update(prep->inner_ctx, key, key_len);
        algo->final(prep->inner_ctx, key_block);
    } else if (key_len > 0) {
        memcpy(key_block, key, key_len);
    }

    uint8_t pad_block[HMAC_MAX_BLOCK];
    /* Inner context: absorb (K' XOR ipad). */
    for (size_t i = 0; i < algo->block_size; i++) {
        pad_block[i] = key_block[i] ^ HMAC_IPAD;
    }
    algo->init(prep->inner_ctx);
    algo->update(prep->inner_ctx, pad_block, algo->block_size);

    /* Outer context: absorb (K' XOR opad). */
    for (size_t i = 0; i < algo->block_size; i++) {
        pad_block[i] = key_block[i] ^ HMAC_OPAD;
    }
    algo->init(prep->outer_ctx);
    algo->update(prep->outer_ctx, pad_block, algo->block_size);

    /* Wipe sensitive working bytes. */
    dkSecureZero(key_block, sizeof(key_block));
    dkSecureZero(pad_block, sizeof(pad_block));
}

void hmacComputeWithKey(const hmacKey *prep, const void *data, size_t data_len,
                        uint8_t *out) {
    const hmacHashAlgo *algo = prep->algo;
    /* Stack-only fast path covers SHA-256 and any hash whose ctx fits
     * in 1 KiB.  Larger ctxs (exotic hashes) hit the heap. */
    uint8_t stack_storage[2048];
    void *storage = nullptr;
    cleanerName(zfree, Bytes) uint8_t *heap = nullptr;
    if (algo->ctx_size * 2 <= sizeof(stack_storage)) {
        storage = stack_storage;
    } else {
        heap = (uint8_t *)zmalloc(algo->ctx_size * 2);
        storage = heap;
    }
    void *inner = storage;
    void *outer = (uint8_t *)storage + algo->ctx_size;

    /* Fresh ctxs are byte-for-byte copies of the prepared (K' XOR pad)-
     * absorbed states.  This is safe for hash ctxs that are plain memory
     * (SHA-256, BLAKE3 — both are POD-like structs). */
    memcpy(inner, prep->inner_ctx, algo->ctx_size);
    memcpy(outer, prep->outer_ctx, algo->ctx_size);

    /* Inner: continue hashing with the caller's message; finalize. */
    algo->update(inner, data, data_len);
    uint8_t inner_digest[64];
    algo->final(inner, inner_digest);

    /* Outer: feed the inner digest and finalize for the HMAC output. */
    algo->update(outer, inner_digest, algo->digest_size);
    algo->final(outer, out);

    dkSecureZero(inner_digest, sizeof(inner_digest));
}

/* Type signature of the platform 4-lane SHA-256 compressor we pick at
 * runtime.  Both ARM crypto and x86 SHA-NI variants expose this shape. */
typedef void (*hmacSha256Multi4Fn)(uint32_t states[4][8],
                                   const uint8_t *const blocks[4],
                                   size_t num_blocks);

/* Resolve the best available 4-lane SHA-256 compressor for this host.
 * Returns NULL when none is available — caller falls back to serial. */
static hmacSha256Multi4Fn hmacBatch4PickMulti4(void) {
    if (sha256ArmCryptoAvailable()) {
        return sha256ArmCryptoCompressBlocksMulti4;
    }
    if (sha256ShaNiAvailable()) {
        return sha256ShaNiCompressBlocksMulti4;
    }
    return nullptr;
}

/* Drive the inner-hash bulk-compress for four HMAC-SHA-256 lanes via
 * the multi-buffer crypto-extension path.  Each lane's prepared key
 * carries an already-K_ipad-absorbed sha256Ctx; we copy those into
 * four per-lane working ctxs, then stream the per-lane message bytes.
 *
 * Strategy: while at least two lanes still have >= 64 unconsumed bytes
 * AND those lanes are aligned on a block boundary (no leftover buf),
 * step them all forward by one block via the multi-buffer compressor.
 * Lanes that finish, fall behind, or have partial-block leftovers
 * drain on the serial sha256Update fall-through path.  This captures
 * the dominant "four similar-sized frames" case without giving up on
 * the mismatched-length one. */
static void hmacBatch4Sha256(const hmacKey *const prep[4],
                             const void *const data[4], const size_t len[4],
                             uint8_t out[4][32], hmacSha256Multi4Fn multi4) {
    sha256Ctx inner_ctx[4];
    for (uint32_t i = 0; i < 4u; i++) {
        memcpy(&inner_ctx[i], (const sha256Ctx *)prep[i]->inner_ctx,
               sizeof(sha256Ctx));
    }

    const uint8_t *p[4];
    size_t remaining[4];
    for (uint32_t i = 0; i < 4u; i++) {
        p[i] = (const uint8_t *)data[i];
        remaining[i] = len[i];
    }

    /* Lockstep multi-buffer phase: process as many synchronous full
     * blocks as all four lanes can supply.  Each lane must have its
     * partial-block buffer empty (no prior leftover bytes) so the
     * next 64 bytes are guaranteed block-aligned wrt the running
     * state.  Because the prepared inner_ctx is freshly cloned from
     * a one-block ipad absorb, buf_len is always 0 here — but check
     * defensively so a future change to the prepared-state shape
     * cannot silently break the alignment invariant. */
    bool aligned = true;
    for (uint32_t i = 0; i < 4u; i++) {
        if (inner_ctx[i].buf_len != 0u) {
            aligned = false;
            break;
        }
    }
    size_t common_blocks = 0;
    if (aligned) {
        common_blocks = remaining[0];
        for (uint32_t i = 1; i < 4u; i++) {
            if (remaining[i] < common_blocks) {
                common_blocks = remaining[i];
            }
        }
        common_blocks /= SHA256_BLOCK_LEN;
        if (common_blocks > 0) {
            /* Pack the four state pointers + four data pointers into the
             * shapes the multi-buffer API expects. */
            uint32_t states_buf[4][8];
            for (uint32_t i = 0; i < 4u; i++) {
                memcpy(states_buf[i], inner_ctx[i].state,
                       sizeof(states_buf[i]));
            }
            const uint8_t *block_ptrs[4] = {p[0], p[1], p[2], p[3]};
            multi4(states_buf, block_ptrs, common_blocks);
            /* Push state back into each lane's ctx; advance pointers.
             * Account for the multi-buffer-consumed bits here, exactly
             * matching what was consumed — the multi-buffer kernels do
             * NOT touch bit_count themselves, and the serial tail path
             * (sha256Update below) only accounts for its own bytes.  No
             * pre-add / subtract dance, so future SHA variants that may
             * pre-compute from bit_count inside sha256Update can never
             * observe a transiently-overshot counter. */
            const uint64_t consumed_bits =
                (uint64_t)common_blocks * SHA256_BLOCK_LEN * 8u;
            for (uint32_t i = 0; i < 4u; i++) {
                memcpy(inner_ctx[i].state, states_buf[i],
                       sizeof(states_buf[i]));
                inner_ctx[i].bit_count += consumed_bits;
                p[i] += common_blocks * SHA256_BLOCK_LEN;
                remaining[i] -= common_blocks * SHA256_BLOCK_LEN;
            }
        }
    }

    /* Per-lane tail: feed any remaining bytes through the standard
     * sha256Update path (which absorbs whole blocks one-by-one and
     * buffers the partial, advancing bit_count internally), then
     * finalise — but for HMAC we want the INNER digest only here,
     * since the outer pass is small enough (single block) that
     * batching doesn't pay back the setup cost. */
    uint8_t inner_digest[4][32];
    for (uint32_t i = 0; i < 4u; i++) {
        if (remaining[i] > 0u) {
            sha256Update(&inner_ctx[i], p[i], remaining[i]);
        }
        sha256Final(&inner_ctx[i], inner_digest[i]);
    }

    /* Outer pass: copy each prep->outer_ctx and feed the inner digest.
     * Small (32 bytes -> one padded block); serial is fine. */
    for (uint32_t i = 0; i < 4u; i++) {
        sha256Ctx outer;
        memcpy(&outer, (const sha256Ctx *)prep[i]->outer_ctx, sizeof(outer));
        sha256Update(&outer, inner_digest[i], 32u);
        sha256Final(&outer, out[i]);
    }
    /* Wipe transient inner digests — they reveal the K_ipad-absorbed
     * state if leaked. */
    dkSecureZero(inner_digest, sizeof(inner_digest));
}

void hmacComputeBatch4(const hmacKey *const prep[4], const void *const data[4],
                       const size_t len[4], uint8_t out[4][32]) {
    /* Bail to serial for any precondition that breaks the lockstep
     * assumption.  Production callers should structure their work to
     * hit the fast path; the fallback exists so the API can be called
     * unconditionally (e.g. by a generic envelope-verify-batch). */
    if (prep == nullptr || data == nullptr || len == nullptr ||
        out == nullptr) {
        return;
    }
    if (prep[0] == nullptr || prep[1] == nullptr || prep[2] == nullptr ||
        prep[3] == nullptr) {
        return;
    }
    const hmacHashAlgo *algo = prep[0]->algo;
    if (algo == nullptr || algo != prep[1]->algo || algo != prep[2]->algo ||
        algo != prep[3]->algo) {
        for (uint32_t i = 0; i < 4u; i++) {
            hmacComputeWithKey(prep[i], data[i], len[i], out[i]);
        }
        return;
    }
    if (algo == &hmacAlgoSHA256) {
        hmacSha256Multi4Fn multi4 = hmacBatch4PickMulti4();
        if (multi4 != nullptr) {
            hmacBatch4Sha256(prep, data, len, out, multi4);
            return;
        }
    }
    /* No accelerated path for this algo — serial fallback preserves
     * the API contract. */
    for (uint32_t i = 0; i < 4u; i++) {
        hmacComputeWithKey(prep[i], data[i], len[i], out[i]);
    }
}

void hmacInitFromPrepared(hmacCtx *ctx, const hmacKey *prep, void *storage,
                          size_t storage_size) {
    const hmacHashAlgo *algo = prep->algo;
    if (algo == nullptr || storage_size < hmacCtxSize(algo) ||
        algo->block_size > HMAC_MAX_BLOCK) {
        memset(ctx, 0, sizeof(*ctx));
        return;
    }

    uint8_t *base = (uint8_t *)storage;
    ctx->algo = algo;
    ctx->inner_ctx = base;
    ctx->outer_ctx = base + algo->ctx_size;
    ctx->scratch = base + algo->ctx_size * 2;

    /* Hash ctxs in this codebase are plain memory (sha256Ctx and
     * blake3Hasher are POD-like structs), so a byte-for-byte copy of
     * the prepared (K' XOR ipad/opad)-absorbed states is sufficient to
     * resume them via subsequent update()/final() calls. */
    memcpy(ctx->inner_ctx, prep->inner_ctx, algo->ctx_size);
    memcpy(ctx->outer_ctx, prep->outer_ctx, algo->ctx_size);
    /* Scratch is unused on this path — the K_ipad/K_opad blocks already
     * live inside inner_ctx/outer_ctx.  Securely zero it so test
     * inspection never sees leftover state from a previous hmacInit on
     * the same storage (which may have held the derived K' or
     * K' XOR ipad/opad blocks). */
    dkSecureZero(ctx->scratch, algo->block_size * 2);
}

/* ------------------------------------------------------------------------ */
/* SHA-256 algorithm descriptor (production — used by flexEnvelope too)     */
/* ------------------------------------------------------------------------ */

/* Adapters from the void*-typed vtable to SHA-256's strongly typed API.
 * The casts are safe because the storage buffer is always sized to
 * sizeof(sha256Ctx) for this algo descriptor. */
static void sha256_init_wrapper(void *ctx) {
    sha256Init((sha256Ctx *)ctx);
}
static void sha256_update_wrapper(void *ctx, const void *data, size_t len) {
    sha256Update((sha256Ctx *)ctx, data, len);
}
static void sha256_final_wrapper(void *ctx, uint8_t *out) {
    sha256Final((sha256Ctx *)ctx, out);
}

const hmacHashAlgo hmacAlgoSHA256 = {sha256_init_wrapper,
                                     sha256_update_wrapper,
                                     sha256_final_wrapper,
                                     sizeof(sha256Ctx),
                                     64,
                                     32};

/* ---- BLAKE3-as-HMAC-hash descriptor --------------------------------
 *
 * HMAC-BLAKE3 is the standard RFC 2104 HMAC construction layered over
 * BLAKE3 — distinct from BLAKE3's native keyed-hash mode (which
 * callers should prefer when they don't need the standard HMAC
 * framing).  This descriptor exists for interop with tools that
 * expect HMAC's K_ipad/K_opad envelope. */
static void blake3_init_wrapper(void *ctx) {
    blake3HasherInit((blake3Hasher *)ctx);
}
static void blake3_update_wrapper(void *ctx, const void *data, size_t len) {
    blake3HasherUpdate((blake3Hasher *)ctx, data, len);
}
static void blake3_final_wrapper(void *ctx, uint8_t *out) {
    /* HMAC always wants the standard 32-byte hash output. */
    blake3HasherFinalize((const blake3Hasher *)ctx, out, BLAKE3_OUT_LEN);
}

const hmacHashAlgo hmacAlgoBLAKE3 = {
    blake3_init_wrapper,  blake3_update_wrapper, blake3_final_wrapper,
    sizeof(blake3Hasher), BLAKE3_BLOCK_LEN,      BLAKE3_OUT_LEN};

/* ------------------------------------------------------------------------ */
/* Test driver                                                              */
/* ------------------------------------------------------------------------ */

#ifdef DATAKIT_TEST

#include "ctest.h"
#include "datakit.h"
#include "hash/hmacTestVectors.h"
#include "str.h" /* StrBytesToHex */
#include "timeUtil.h"

#include <stdlib.h>

static int digestEqual(const uint8_t *a, const uint8_t *b, size_t n) {
    return memcmp(a, b, n) == 0;
}

/* Reports the mismatch and returns its failure count (1) for the caller to
 * add to its own */
static int reportMismatch(const char *name, const uint8_t *got,
                          const uint8_t *expected, size_t n) {
    char gotHex[129], expHex[129];
    StrBytesToHex(got, n, gotHex);
    StrBytesToHex(expected, n, expHex);
    int err = 0;
    ERR("vector '%s': got %s, expected %s", name, gotHex, expHex);
    return err;
}

static void runBench(size_t payload, uint32_t iters) {
    cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(payload);
    memset(buf, 0x5a, payload);
    uint8_t key[32];
    memset(key, 0xa5, sizeof(key));
    uint8_t out[32];

    const double t0 = timeUtilMonotonicSec();
    for (uint32_t i = 0; i < iters; i++) {
        hmacCompute(&hmacAlgoSHA256, key, sizeof(key), buf, payload, out);
    }
    double secs = timeUtilMonotonicSec() - t0;
    double mib = (double)((uint64_t)payload * iters) / (1024.0 * 1024.0);
    printf("    hmac-sha256: payload=%zu iters=%u  %.2f MiB in %.3fs = %.2f "
           "MiB/s\n",
           payload, iters, mib, secs, secs > 0 ? mib / secs : 0.0);
}

int hmacTest(int, char *[]) {
    int err = 0;

    TEST("RFC 4231 HMAC-SHA-256 vectors") {
        for (size_t i = 0; i < HMAC_TEST_VECTOR_COUNT; i++) {
            const hmacTestVector *v = &hmacTestVectorsTable[i];
            uint8_t out[32];
            hmacCompute(&hmacAlgoSHA256, v->key, v->key_len, v->data,
                        v->data_len, out);
            if (!digestEqual(out, v->expected, 32)) {
                err += reportMismatch(v->name, out, v->expected, 32);
            }
        }
    }

    TEST("streaming agreement on random inputs") {
        srand(0xC0FFEE);
        uint8_t key[40];
        for (size_t i = 0; i < sizeof(key); i++) {
            key[i] = (uint8_t)rand();
        }

        size_t storageSize = hmacCtxSize(&hmacAlgoSHA256);
        cleanerName(zfree, Bytes) uint8_t *storage = zmalloc(storageSize);

        for (uint32_t trial = 0; trial < 32; trial++) {
            size_t n = (size_t)(rand() % 4096);
            cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(n + 1);
            for (size_t i = 0; i < n; i++) {
                buf[i] = (uint8_t)rand();
            }

            uint8_t oneShot[32];
            uint8_t streamed[32];
            hmacCompute(&hmacAlgoSHA256, key, sizeof(key), buf, n, oneShot);

            hmacCtx ctx;
            hmacInit(&ctx, &hmacAlgoSHA256, storage, storageSize, key,
                     sizeof(key));
            size_t off = 0;
            while (off < n) {
                size_t remain = n - off;
                size_t chunk = (size_t)(rand() % 257) + 1;
                if (chunk > remain) {
                    chunk = remain;
                }
                hmacUpdate(&ctx, buf + off, chunk);
                off += chunk;
            }
            hmacFinal(&ctx, streamed);

            if (!digestEqual(oneShot, streamed, 32)) {
                char nameBuf[64];
                snprintf(nameBuf, sizeof(nameBuf), "random trial %u n=%zu",
                         trial, n);
                err += reportMismatch(nameBuf, streamed, oneShot, 32);
            }
        }
    }

    TEST("one-shot matches incremental on RFC vectors") {
        size_t storageSize = hmacCtxSize(&hmacAlgoSHA256);
        cleanerName(zfree, Bytes) uint8_t *storage = zmalloc(storageSize);
        for (size_t i = 0; i < HMAC_TEST_VECTOR_COUNT; i++) {
            const hmacTestVector *v = &hmacTestVectorsTable[i];
            uint8_t oneShot[32];
            uint8_t incremental[32];
            hmacCompute(&hmacAlgoSHA256, v->key, v->key_len, v->data,
                        v->data_len, oneShot);

            hmacCtx ctx;
            hmacInit(&ctx, &hmacAlgoSHA256, storage, storageSize, v->key,
                     v->key_len);
            /* Split into byte-by-byte updates for maximum stress. */
            for (size_t j = 0; j < v->data_len; j++) {
                hmacUpdate(&ctx, v->data + j, 1);
            }
            hmacFinal(&ctx, incremental);

            if (!digestEqual(oneShot, incremental, 32)) {
                err += reportMismatch(v->name, incremental, oneShot, 32);
            }
        }
    }

    TEST("testHmacFinalWipesScratch") {
        /* hmacInit stamps K_ipad and K_opad blocks (key-equivalent
         * material) into ctx->scratch. hmacFinal MUST zero those bytes
         * via dkSecureZero so a leaked caller storage buffer does not
         * disclose the pad blocks. We pattern-fill the storage before
         * Init so any byte that survives Final stands out. */
        const size_t storage_size = hmacCtxSize(&hmacAlgoSHA256);
        cleanerName(zfree, Bytes) uint8_t *storage = zmalloc(storage_size);
        /* Pattern-fill defeats "happened to be zero" coincidences. */
        memset(storage, 0xA5, storage_size);

        const uint8_t key[40] = {
            0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9,
            0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF, 0xD0, 0xD1, 0xD2, 0xD3,
            0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD,
            0xDE, 0xDF, 0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7,
        };
        hmacCtx ctx;
        hmacInit(&ctx, &hmacAlgoSHA256, storage, storage_size, key,
                 sizeof(key));
        const uint8_t msg[] = "wipe-scratch test payload";
        hmacUpdate(&ctx, msg, sizeof(msg) - 1u);
        uint8_t tag[32];
        hmacFinal(&ctx, tag);

        /* Post-Final, the scratch region (block_size * 2 bytes at
         * ctx->scratch) must be all zero. With SHA-256, block_size==64
         * so scratch is 128 bytes. */
        const size_t scratch_bytes = hmacAlgoSHA256.block_size * 2u;
        bool all_zero = true;
        for (size_t i = 0; i < scratch_bytes; i++) {
            if (ctx.scratch[i] != 0u) {
                all_zero = false;
                break;
            }
        }
        if (!all_zero) {
            ERR("hmacFinal did not zero ctx->scratch (K_ipad/K_opad "
                "remained resident)");
        }

        /* Mirror on BLAKE3 so the contract holds for every algorithm. */
        const size_t b3_storage_size = hmacCtxSize(&hmacAlgoBLAKE3);
        cleanerName(zfree, Bytes) uint8_t *b3_storage =
            zmalloc(b3_storage_size);
        memset(b3_storage, 0x5A, b3_storage_size);
        hmacCtx b3_ctx;
        hmacInit(&b3_ctx, &hmacAlgoBLAKE3, b3_storage, b3_storage_size, key,
                 sizeof(key));
        hmacUpdate(&b3_ctx, msg, sizeof(msg) - 1u);
        uint8_t b3_tag[32];
        hmacFinal(&b3_ctx, b3_tag);
        const size_t b3_scratch_bytes = hmacAlgoBLAKE3.block_size * 2u;
        bool b3_all_zero = true;
        for (size_t i = 0; i < b3_scratch_bytes; i++) {
            if (b3_ctx.scratch[i] != 0u) {
                b3_all_zero = false;
                break;
            }
        }
        if (!b3_all_zero) {
            ERR("hmacFinal did not zero BLAKE3 ctx->scratch");
        }
    }

    TEST("different keys produce different outputs") {
        const uint8_t data[] = "the quick brown fox jumps over the lazy dog";
        uint8_t key_a[32];
        uint8_t key_b[32];
        memset(key_a, 0x11, sizeof(key_a));
        memset(key_b, 0x11, sizeof(key_b));
        key_b[17] ^= 0x01; /* one-bit difference */

        uint8_t out_a[32];
        uint8_t out_b[32];
        hmacCompute(&hmacAlgoSHA256, key_a, sizeof(key_a), data,
                    sizeof(data) - 1, out_a);
        hmacCompute(&hmacAlgoSHA256, key_b, sizeof(key_b), data,
                    sizeof(data) - 1, out_b);
        if (digestEqual(out_a, out_b, 32)) {
            ERR("HMAC outputs equal under different keys (avalanche failure)");
        }
    }

    TEST("determinism: same key+data => same output") {
        const uint8_t data[] = "repeatability check";
        uint8_t key[16];
        memset(key, 0x42, sizeof(key));
        uint8_t out_first[32];
        uint8_t out_second[32];
        hmacCompute(&hmacAlgoSHA256, key, sizeof(key), data, sizeof(data) - 1,
                    out_first);
        hmacCompute(&hmacAlgoSHA256, key, sizeof(key), data, sizeof(data) - 1,
                    out_second);
        if (!digestEqual(out_first, out_second, 32)) {
            ERR("HMAC nondeterministic across identical calls");
        }
    }

    TEST("performance bench") {
        /* Three payload sizes, fixed total work ~64 MiB. Never fails. */
        runBench(1024, 64 * 1024); /* 1 KiB  x 64K = 64 MiB */
        runBench(65536, 1024);     /* 64 KiB x 1K  = 64 MiB */
        runBench(1u << 20, 64);    /* 1 MiB  x 64  = 64 MiB */
    }

    /* ---- Prepared-key API ------------------------------------------- */

    TEST("prepared-key API: matches hmacCompute on all RFC vectors") {
        size_t prepSize = hmacPreparedSize(&hmacAlgoSHA256);
        cleanerName(zfree, Bytes) uint8_t *prepStorage = zmalloc(prepSize);
        for (size_t i = 0; i < HMAC_TEST_VECTOR_COUNT; i++) {
            const hmacTestVector *v = &hmacTestVectorsTable[i];

            uint8_t fromCompute[32];
            uint8_t fromPrepared[32];
            hmacCompute(&hmacAlgoSHA256, v->key, v->key_len, v->data,
                        v->data_len, fromCompute);

            hmacKey prep;
            hmacPrepareKey(&prep, &hmacAlgoSHA256, prepStorage, prepSize,
                           v->key, v->key_len);
            hmacComputeWithKey(&prep, v->data, v->data_len, fromPrepared);

            if (!digestEqual(fromCompute, fromPrepared, 32)) {
                err += reportMismatch(v->name, fromPrepared, fromCompute, 32);
            }
        }
    }

    TEST("prepared-key API: reuse across many messages") {
        /* Verify that one prep can drive many computes with consistent
         * results.  This is the hot-path use case the API targets. */
        uint8_t key[32];
        for (size_t i = 0; i < sizeof(key); i++) {
            key[i] = (uint8_t)(i * 7);
        }
        size_t prepSize = hmacPreparedSize(&hmacAlgoSHA256);
        cleanerName(zfree, Bytes) uint8_t *prepStorage = zmalloc(prepSize);
        hmacKey prep;
        hmacPrepareKey(&prep, &hmacAlgoSHA256, prepStorage, prepSize, key,
                       sizeof(key));

        for (uint32_t trial = 0; trial < 1000; trial++) {
            uint8_t msg[128];
            for (size_t i = 0; i < sizeof(msg); i++) {
                msg[i] = (uint8_t)(trial * 31 + i);
            }
            uint8_t expected[32];
            uint8_t got[32];
            hmacCompute(&hmacAlgoSHA256, key, sizeof(key), msg, sizeof(msg),
                        expected);
            hmacComputeWithKey(&prep, msg, sizeof(msg), got);
            if (!digestEqual(expected, got, 32)) {
                char nameBuf[64];
                snprintf(nameBuf, sizeof(nameBuf), "trial %u", trial);
                err += reportMismatch(nameBuf, got, expected, 32);
                break;
            }
        }
    }

    TEST("prepared-key streaming init matches plain init on random sizes") {
        /* hmacInitFromPrepared seeds an hmacCtx with the prepared K_ipad
         * / K_opad-absorbed states.  Streaming updates and final must
         * produce the SAME digest as hmacInit(...)+update+final on the
         * same key and message bytes — proven across a spread of sizes
         * exercising every block boundary. */
        uint8_t key[32];
        memset(key, 0xa7, sizeof(key));
        const size_t prep_size = hmacPreparedSize(&hmacAlgoSHA256);
        cleanerName(zfree, Bytes) uint8_t *prep_storage = zmalloc(prep_size);
        hmacKey prep;
        hmacPrepareKey(&prep, &hmacAlgoSHA256, prep_storage, prep_size, key,
                       sizeof(key));

        const size_t ctx_size = hmacCtxSize(&hmacAlgoSHA256);
        cleanerName(zfree, Bytes) uint8_t *plain_storage = zmalloc(ctx_size);
        cleanerName(zfree, Bytes) uint8_t *prep_init_storage =
            zmalloc(ctx_size);

        const size_t sizes[] = {0u,  1u,   31u,  32u,  63u,  64u,
                                65u, 127u, 128u, 129u, 511u, 1023u};
        for (size_t s = 0; s < countof(sizes); s++) {
            const size_t n = sizes[s];
            cleanerName(zfree, Bytes) uint8_t *msg =
                (n > 0) ? zmalloc(n) : nullptr;
            for (size_t i = 0; i < n; i++) {
                msg[i] = (uint8_t)((i * 1103515245u + 12345u) >> 8);
            }

            uint8_t plain_out[32];
            hmacCtx plain;
            hmacInit(&plain, &hmacAlgoSHA256, plain_storage, ctx_size, key,
                     sizeof(key));
            if (n > 0) {
                hmacUpdate(&plain, msg, n);
            }
            hmacFinal(&plain, plain_out);

            uint8_t prep_out[32];
            hmacCtx prepCtx;
            hmacInitFromPrepared(&prepCtx, &prep, prep_init_storage, ctx_size);
            if (n > 0) {
                hmacUpdate(&prepCtx, msg, n);
            }
            hmacFinal(&prepCtx, prep_out);

            if (memcmp(plain_out, prep_out, sizeof(plain_out)) != 0) {
                ERR("Mismatch at len=%zu", n);
            }
        }

        /* Multiple Updates with the same prepared init must still match
         * a single Update over the concatenation. */
        const uint8_t a[] = "Lorem ipsum dolor sit amet, ";
        const uint8_t b[] = "consectetur adipiscing elit.";
        uint8_t one_shot[32], split[32];
        hmacCtx mono;
        hmacInitFromPrepared(&mono, &prep, prep_init_storage, ctx_size);
        hmacUpdate(&mono, a, sizeof(a) - 1u);
        hmacUpdate(&mono, b, sizeof(b) - 1u);
        hmacFinal(&mono, split);
        hmacCtx single;
        hmacInitFromPrepared(&single, &prep, prep_init_storage, ctx_size);
        uint8_t joined[sizeof(a) + sizeof(b)];
        memcpy(joined, a, sizeof(a) - 1u);
        memcpy(joined + sizeof(a) - 1u, b, sizeof(b) - 1u);
        hmacUpdate(&single, joined, (sizeof(a) - 1u) + (sizeof(b) - 1u));
        hmacFinal(&single, one_shot);
        if (memcmp(one_shot, split, sizeof(one_shot)) != 0) {
            ERR("Chunked vs concatenated streaming update mismatch");
        }
    }

    TEST("prepared-key API: perf bench (small messages where it matters)") {
        const size_t payload = 64;
        const uint32_t iters = 500'000;
        uint8_t key[32];
        memset(key, 0x5a, sizeof(key));
        uint8_t buf[64];
        memset(buf, 0xa5, sizeof(buf));
        uint8_t out[32];

        double t0 = timeUtilMonotonicSec();
        for (uint32_t i = 0; i < iters; i++) {
            hmacCompute(&hmacAlgoSHA256, key, sizeof(key), buf, payload, out);
        }
        double secs = timeUtilMonotonicSec() - t0;
        printf("    plain hmacCompute    : %u x %zuB in %.3fs "
               "= %.0f ns/op\n",
               iters, payload, secs, secs * 1e9 / iters);

        size_t prepSize = hmacPreparedSize(&hmacAlgoSHA256);
        cleanerName(zfree, Bytes) uint8_t *prepStorage = zmalloc(prepSize);
        hmacKey prep;
        hmacPrepareKey(&prep, &hmacAlgoSHA256, prepStorage, prepSize, key,
                       sizeof(key));
        t0 = timeUtilMonotonicSec();
        for (uint32_t i = 0; i < iters; i++) {
            hmacComputeWithKey(&prep, buf, payload, out);
        }
        double prep_secs = timeUtilMonotonicSec() - t0;
        printf("    hmacComputeWithKey   : %u x %zuB in %.3fs "
               "= %.0f ns/op  (%.2fx vs plain)\n",
               iters, payload, prep_secs, prep_secs * 1e9 / iters,
               secs / (prep_secs > 0 ? prep_secs : 1.0));
    }

    TEST("hmacComputeBatch4: matches serial hmacComputeWithKey on SHA-256") {
        const uint8_t key_a[32] = {0x10, 0x20, 0x30};
        const uint8_t key_b[32] = {0x40, 0x50, 0x60};
        const uint8_t key_c[32] = {0x70, 0x80, 0x90};
        const uint8_t key_d[32] = {0xA0, 0xB0, 0xC0};
        const size_t ps = hmacPreparedSize(&hmacAlgoSHA256);
        cleanerName(zfree, Bytes) uint8_t *st_a = zmalloc(ps);
        cleanerName(zfree, Bytes) uint8_t *st_b = zmalloc(ps);
        cleanerName(zfree, Bytes) uint8_t *st_c = zmalloc(ps);
        cleanerName(zfree, Bytes) uint8_t *st_d = zmalloc(ps);
        hmacKey p_a, p_b, p_c, p_d;
        hmacPrepareKey(&p_a, &hmacAlgoSHA256, st_a, ps, key_a, 32);
        hmacPrepareKey(&p_b, &hmacAlgoSHA256, st_b, ps, key_b, 32);
        hmacPrepareKey(&p_c, &hmacAlgoSHA256, st_c, ps, key_c, 32);
        hmacPrepareKey(&p_d, &hmacAlgoSHA256, st_d, ps, key_d, 32);

        /* Try a matrix of lengths including under-block, exact-block,
         * over-block, and unequal lengths so the lockstep + serial-tail
         * paths both get exercised. */
        const size_t test_lens[][4] = {
            {0, 0, 0, 0},         /* empty */
            {1, 1, 1, 1},         /* tiny */
            {63, 63, 63, 63},     /* one byte under block */
            {64, 64, 64, 64},     /* one full block */
            {128, 128, 128, 128}, /* two blocks */
            {1024, 1024, 1024, 1024},
            {4096, 4096, 4096, 4096},
            {100, 200, 300, 400}, /* unequal */
            {0, 64, 128, 192},    /* ascending incl. empty */
            {4096, 64, 1, 0},     /* descending */
        };
        const size_t kCases = countof(test_lens);
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(8192);
        for (size_t i = 0; i < 8192; i++) {
            buf[i] = (uint8_t)(i * 13u + 7u);
        }

        const hmacKey *preps[4] = {&p_a, &p_b, &p_c, &p_d};
        for (size_t c = 0; c < kCases; c++) {
            const void *data[4] = {buf, buf + 1, buf + 2, buf + 3};
            size_t lens[4] = {test_lens[c][0], test_lens[c][1], test_lens[c][2],
                              test_lens[c][3]};
            uint8_t expected[4][32], got[4][32];
            for (uint32_t i = 0; i < 4u; i++) {
                hmacComputeWithKey(preps[i], data[i], lens[i], expected[i]);
            }
            hmacComputeBatch4(preps, data, lens, got);
            for (uint32_t i = 0; i < 4u; i++) {
                if (memcmp(expected[i], got[i], 32) != 0) {
                    char eh[65], gh[65];
                    StrBytesToHex(expected[i], 32, eh);
                    StrBytesToHex(got[i], 32, gh);
                    ERR("case %zu lane %u (len=%zu): batch %s, serial %s", c, i,
                        lens[i], gh, eh);
                }
            }
        }
    }

    TEST("hmacComputeBatch4: mixed algos fall back to serial") {
        const uint8_t key[32] = {0xAA};
        const size_t ps_sha = hmacPreparedSize(&hmacAlgoSHA256);
        const size_t ps_b3 = hmacPreparedSize(&hmacAlgoBLAKE3);
        cleanerName(zfree, Bytes) uint8_t *st0 = zmalloc(ps_sha);
        cleanerName(zfree, Bytes) uint8_t *st1 = zmalloc(ps_b3);
        cleanerName(zfree, Bytes) uint8_t *st2 = zmalloc(ps_sha);
        cleanerName(zfree, Bytes) uint8_t *st3 = zmalloc(ps_sha);
        hmacKey p0, p1, p2, p3;
        hmacPrepareKey(&p0, &hmacAlgoSHA256, st0, ps_sha, key, 32);
        hmacPrepareKey(&p1, &hmacAlgoBLAKE3, st1, ps_b3, key, 32);
        hmacPrepareKey(&p2, &hmacAlgoSHA256, st2, ps_sha, key, 32);
        hmacPrepareKey(&p3, &hmacAlgoSHA256, st3, ps_sha, key, 32);
        const hmacKey *preps[4] = {&p0, &p1, &p2, &p3};
        const uint8_t msg[] = "test";
        const void *data[4] = {msg, msg, msg, msg};
        size_t lens[4] = {4, 4, 4, 4};
        uint8_t exp[4][32], got[4][32];
        for (uint32_t i = 0; i < 4u; i++) {
            hmacComputeWithKey(preps[i], data[i], lens[i], exp[i]);
        }
        hmacComputeBatch4(preps, data, lens, got);
        for (uint32_t i = 0; i < 4u; i++) {
            if (memcmp(exp[i], got[i], 32) != 0) {
                ERR("mixed-algo fallback lane %u mismatch", i);
            }
        }
    }

    TEST("hmacComputeBatch4: perf bench (4 lanes, 1KiB each)") {
        const uint8_t key[32] = {0xBB};
        const size_t ps = hmacPreparedSize(&hmacAlgoSHA256);
        cleaner(freeLaneStorage4)
            uint8_t *st[4] = {nullptr, nullptr, nullptr, nullptr};
        hmacKey p[4];
        const hmacKey *preps[4];
        for (uint32_t i = 0; i < 4u; i++) {
            st[i] = zmalloc(ps);
            hmacPrepareKey(&p[i], &hmacAlgoSHA256, st[i], ps, key, 32);
            preps[i] = &p[i];
        }
        const size_t payload = 1024;
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(payload);
        memset(buf, 0xC3, payload);
        const void *data[4] = {buf, buf, buf, buf};
        const size_t lens[4] = {payload, payload, payload, payload};

        const uint32_t iters = 50000;
        uint8_t serial_out[4][32], batch_out[4][32];
        double t0 = timeUtilMonotonicSec();
        for (uint32_t i = 0; i < iters; i++) {
            for (uint32_t j = 0; j < 4u; j++) {
                hmacComputeWithKey(preps[j], data[j], lens[j], serial_out[j]);
            }
        }
        const double secs_serial = timeUtilMonotonicSec() - t0;

        t0 = timeUtilMonotonicSec();
        for (uint32_t i = 0; i < iters; i++) {
            hmacComputeBatch4(preps, data, lens, batch_out);
        }
        const double secs_batch = timeUtilMonotonicSec() - t0;

        const double mib =
            (double)((uint64_t)payload * iters * 4u) / (1024.0 * 1024.0);
        printf("    serial x4: %.2f MiB in %.3fs = %.0f ns/4-batch\n", mib,
               secs_serial, secs_serial * 1e9 / iters);
        printf("    batch4   : %.2f MiB in %.3fs = %.0f ns/4-batch  "
               "(%.2fx vs serial)\n",
               mib, secs_batch, secs_batch * 1e9 / iters,
               secs_batch > 0 ? secs_serial / secs_batch : 0.0);

        /* Sanity: lane 0 of both must match. */
        if (memcmp(serial_out[0], batch_out[0], 32) != 0) {
            ERR("perf bench: serial vs batch lane 0 mismatch");
        }
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
