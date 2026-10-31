#pragma once

/* Generic, hash-agnostic HMAC (RFC 2104).
 *
 * The hash function is supplied at runtime via an hmacHashAlgo vtable, so
 * one HMAC implementation can drive SHA-256, BLAKE3, or any future hash
 * with the same init/update/final shape. The caller owns all storage:
 * HMAC never touches the heap on the hot path. */

#include <stddef.h>
#include <stdint.h>

/* A keyring key's caller-chosen 32-bit identifier (hmacKeyring.h),
 * as envelopes, nonce logs and persisted files carry it. Zero is reserved as
 * "invalid / no key" so callers can use it as a sentinel. Otherwise
 * any non-zero value is acceptable — typical schemes use a small
 * monotonically-increasing generation counter (1, 2, 3, ...) or an
 * opaque hash of the key material itself. */
typedef uint32_t hmacKeyId;
#define HMAC_KEY_ID_INVALID ((hmacKeyId)0)

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*hmacHashInitFn)(void *ctx);
typedef void (*hmacHashUpdateFn)(void *ctx, const void *data, size_t len);
typedef void (*hmacHashFinalFn)(void *ctx, uint8_t *out);

typedef struct hmacHashAlgo {
    /* Fields ordered largest -> smallest to minimize padding. */
    hmacHashInitFn init;
    hmacHashUpdateFn update;
    /* Writes the digest, then wipes the ctx with dkSecureZero: an HMAC
     * hash ctx has absorbed K' XOR pad, and hmacFinal relies on this */
    hmacHashFinalFn final;
    size_t ctx_size;    /* sizeof underlying hash ctx */
    size_t block_size;  /* hash internal block size (64 for SHA-256) */
    size_t digest_size; /* digest length in bytes */
} hmacHashAlgo;

typedef struct hmacCtx {
    const hmacHashAlgo *algo;
    /* The three pointers below alias into a single caller-supplied buffer
     * whose required size is reported by hmacCtxSize(). We split it into
     * (inner hash ctx)(outer hash ctx)(scratch for ipad/opad keying). */
    void *inner_ctx;
    void *outer_ctx;
    uint8_t *scratch;
} hmacCtx;

/* Bytes of caller-owned storage required by hmacInit for `algo`. */
size_t hmacCtxSize(const hmacHashAlgo *algo);

/* Initialise `ctx` for HMAC over `algo` keyed by `key`. `storage` must
 * be at least hmacCtxSize(algo) bytes and is bound to the ctx for its
 * lifetime (the inner/outer hash ctxs and the K_ipad/K_opad scratch
 * region all alias into it).
 *
 * SECURITY: this function writes K' XOR ipad / K' XOR opad blocks
 * (key-equivalent material) into `ctx->scratch`. Those blocks remain
 * resident until `hmacFinal` overwrites them — treat `storage` as
 * holding sensitive material between Init and Final.
 *
 * `key_len` may exceed `algo->block_size`; the implementation hashes
 * the key down per RFC 2104. `algo->block_size` is capped at 256 bytes;
 * larger block sizes cause Init to leave `ctx` zeroed (subsequent
 * Update/Final become no-ops). */
void hmacInit(hmacCtx *ctx, const hmacHashAlgo *algo, void *storage,
              size_t storage_size, const uint8_t *key, size_t key_len);
void hmacUpdate(hmacCtx *ctx, const void *data, size_t len);
/* Finalise: writes `algo->digest_size` bytes of HMAC tag to `out`.
 *
 * SIDE EFFECT: zeroes, with dkSecureZero, every key-bearing byte of the
 * storage: the K_ipad / K_opad scratch and both hash states.  After
 * Final the ctx is discarded: re-initialise it with hmacInit or
 * hmacInitFromPrepared before reuse.  A ctx abandoned before Final still
 * holds them: zero its storage. */
void hmacFinal(hmacCtx *ctx, uint8_t *out);

/* One-shot convenience: writes algo->digest_size bytes to out. */
void hmacCompute(const hmacHashAlgo *algo, const uint8_t *key, size_t key_len,
                 const void *data, size_t data_len, uint8_t *out);

/* ---- Precomputed-key API ---------------------------------------------
 *
 * For high-throughput verify paths (replication ingest, cluster apply
 * mailbox, RPC body checks) where the SAME key is used across many
 * messages, hmacPrepareKey computes the inner/outer block-absorbed
 * states ONCE; subsequent hmacComputeWithKey calls skip 2 block
 * compressions per message.
 *
 * Concrete win: HMAC-SHA-256 on a 64-byte message goes from 3 block
 * compresses (K_ipad + 1 message block + K_opad) to 1 + 1 = 2.  Bigger
 * relative gain the smaller the message.
 *
 * Storage is caller-owned; size is hmacPreparedSize(algo).  The
 * prepared key contains no allocations and can be safely copied or
 * shared across threads as long as no thread mutates it. */

typedef struct hmacKey {
    const hmacHashAlgo *algo;
    /* Two opaque ctx-shaped slots: one with K_ipad absorbed, one with
     * K_opad absorbed.  Aliased into caller storage by hmacPrepareKey. */
    void *inner_ctx;
    void *outer_ctx;
} hmacKey;

/* Bytes of caller-owned storage required by hmacPrepareKey for `algo`.
 * Always 2 * algo->ctx_size, exposed as an API so callers don't need to
 * reach into the algo struct. */
size_t hmacPreparedSize(const hmacHashAlgo *algo);

/* Build a prepared-key from `key`.  After return, `prep` is bound to
 * `storage` (which must remain valid for `prep`'s lifetime). */
void hmacPrepareKey(hmacKey *prep, const hmacHashAlgo *algo, void *storage,
                    size_t storage_size, const uint8_t *key, size_t key_len);

/* One-shot HMAC using a prepared key.  Faster than hmacCompute by 2
 * block compressions; otherwise identical output. */
void hmacComputeWithKey(const hmacKey *prep, const void *data, size_t data_len,
                        uint8_t *out);

/* Initialize a streaming hmacCtx from an already-prepared key.  Skips the
 * two block-compressions hmacInit would otherwise pay to absorb K_ipad
 * and K_opad into fresh hash contexts; the prepared key already carries
 * those absorbed states, which are byte-copied into `storage` and then
 * extended via hmacUpdate/hmacFinal as normal.
 *
 * Required when long-running consumers (streaming verify, replication
 * ingest, large WAL replay) need both (a) the prepared-key speed and
 * (b) streaming input.  `storage` must be at least hmacCtxSize(algo)
 * bytes — same buffer size hmacInit demands.  On failure (storage too
 * small) the ctx is zeroed; hmacUpdate/Final on such a ctx are no-ops. */
void hmacInitFromPrepared(hmacCtx *ctx, const hmacKey *prep, void *storage,
                          size_t storage_size);

/* Batch-of-4 HMAC for high-throughput verifies (e.g. replication ingest
 * applying four frames per tick).  Computes four independent HMAC tags
 * concurrently, routing the bulk-compress through the platform's 4-lane
 * multi-buffer SHA-256 path (ARMv8 crypto + NEON: 1.4-1.5x over four
 * serial calls).  When the 4-lane path is unavailable, or when the
 * lanes use different algorithms, falls back transparently to four
 * serial hmacComputeWithKey calls.
 *
 * Contract:
 *   - All four `prep[i]` MUST be non-NULL and use the same algorithm.
 *     Mixed algorithms or any NULL: falls back to serial.
 *   - All four messages can have DIFFERENT lengths; lanes whose data
 *     runs out early are dropped from the multi-buffer pipeline and
 *     finalised serially.
 *   - `data[i]` may point anywhere; no alignment requirement.
 *   - `out[i]` receives `algo->digest_size` bytes (32 for both
 *     supported algos). */
void hmacComputeBatch4(const hmacKey *const prep[4], const void *const data[4],
                       const size_t len[4], uint8_t out[4][32]);

/* Built-in algorithm descriptors.
 *
 *   hmacAlgoSHA256: standard HMAC-SHA-256 (RFC 4231).
 *   hmacAlgoBLAKE3: HMAC over BLAKE3 (RFC 2104 construction; distinct
 *                   from BLAKE3's native keyed-hash mode — use that
 *                   directly via blake3HashKeyed when you don't need
 *                   the standard HMAC framing). */
extern const hmacHashAlgo hmacAlgoSHA256;
extern const hmacHashAlgo hmacAlgoBLAKE3;

#ifdef DATAKIT_TEST
int hmacTest(int argc, char *argv[]);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif
