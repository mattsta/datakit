#pragma once

/* flexEnvelope — integrity-checked wrapper around an opaque byte payload.
 *
 * Wraps any byte buffer (typically a flex) with either a checksum or a keyed
 * MAC. Lives outside flex.c — does not modify the core flex hot path.
 * Wrapping is opt-in at I/O boundaries (replication, on-disk snapshot,
 * cluster apply mailbox, plugin ingest).
 */

#include "flex.h"

#include "blake3.h"
#include "hmac.h"
#include "rapidhash.h"

#define XXH_STATIC_LINKING_ONLY
#include "../deps/xxHash/xxhash.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* === Module constants =============================================== */

/* Envelope magic bytes — placed at offset 0 and 1. Chosen so a quick
 * peek can reject non-envelope buffers without varint decoding. */
#define FLEX_ENVELOPE_MAGIC_0 ((uint8_t)0xFE)
#define FLEX_ENVELOPE_MAGIC_1 ((uint8_t)0x01)

/* Current envelope wire-format version. Bumped only for FRAMING changes,
 * not for adding algorithms (those use a new enum value). Old versions
 * stay decodable as long as we want; new versions add reader support. */
#define FLEX_ENVELOPE_VERSION_CURRENT ((uint8_t)1)

/* The envelope framing as a dkFormat (the format manifest lists it); the
 * version travels on the wire as a varint */
struct dkFormatSpec;
extern const struct dkFormatSpec flexEnvelopeFormat;

/* Default cap on payload_len during verify. An attacker forging a huge
 * length field would otherwise force readers into multi-GB allocations
 * before failing the digest check. 1 GiB is generous for any data
 * structure datakit currently produces; raise it with the
 * FLEX_ENV_VERIFY_ALLOW_OVERSIZE flag or BOUND_PAYLOAD + your own cap.
 *
 * Compile-time override: -DFLEX_ENVELOPE_DEFAULT_MAX_PAYLOAD=... */
#ifndef FLEX_ENVELOPE_DEFAULT_MAX_PAYLOAD
#define FLEX_ENVELOPE_DEFAULT_MAX_PAYLOAD ((uint64_t)1 << 30)
#endif

/* === Algorithm registry ============================================= */

/* Algorithm IDs partition by class:
 * 0 — none (test only)
 * 1..127 — checksum class (no key required, integrity only)
 * 128..255 — keyed / MAC class (key required, tamper-evident)
 * 256+ — reserved for future extension classes
 *
 * Adding an algorithm: append a new enum value, add a descriptor in
 * flexEnvelope.c's static table, wire the hash function. No wire-format
 * bump needed. */
/* Two bytes: the header and stream structs store it in a 16-bit field */
typedef enum flexEnvelopeAlgo : uint16_t {
    FLEX_ENV_ALGO_NONE = 0, /* 0-byte digest; test-only */

    /* Checksum class (1..127) — integrity only, no key */
    FLEX_ENV_ALGO_XXH64 = 1,    /* 8 B */
    FLEX_ENV_ALGO_XXH3_64 = 2,  /* 8 B */
    FLEX_ENV_ALGO_XXH3_128 = 3, /* 16 B */
    /* 8 B.  rapidhash V3, main variant, FAST mode, seed=0, default
     * secret.  All four pins are part of the wire format and CANNOT
     * be changed under this slot — a future seeded / PROTECTED /
     * non-Main rapidhash envelope must occupy a new descriptor slot. */
    FLEX_ENV_ALGO_RAPIDHASH = 4,

    /* Keyed / MAC class (128..199) — tamper-evident, key required */
    FLEX_ENV_ALGO_BLAKE3_KEYED = 128, /* 32 B (BLAKE3 native keyed) */
    FLEX_ENV_ALGO_HMAC_SHA256 = 129,  /* 32 B */
    FLEX_ENV_ALGO_HMAC_BLAKE3 = 130,  /* 32 B */

    /* AEAD class (200..255) — authenticated encryption. Payload bytes
     * on disk are CIPHERTEXT; the verify counterpart is
     * flexEnvelopeOpen (not flexEnvelopeVerify), which writes
     * decrypted plaintext to a caller-supplied buffer. Wrap/Seal
     * requires the caller to supply a fresh per-envelope nonce. */
    FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305 = 200, /* 16 B tag, 12 B nonce */
    FLEX_ENV_ALGO_AEAD_AES128_GCM = 201,        /* 16 B tag, 12 B nonce */
    FLEX_ENV_ALGO_AEAD_AES256_GCM = 202,        /* 16 B tag, 12 B nonce */
} flexEnvelopeAlgo;

/* True iff algo is a keyed / MAC class (requires a key, tamper-evident). */
#define FLEX_ENV_ALGO_IS_KEYED(a) ((a) >= 128 && (a) < 200)
/* True iff algo is an AEAD class (encrypt + authenticate; requires a key
 * AND a per-envelope nonce). These algos go through flexEnvelopeSeal /
 * flexEnvelopeOpen rather than Wrap / Verify. */
#define FLEX_ENV_ALGO_IS_AEAD(a) ((a) >= 200 && (a) < 256)

/* === On-wire flags (in the envelope framing) ======================== */

typedef enum flexEnvelopeFlag {
    FLEX_ENV_FLAG_NONE = 0,

    /* Producer asserts the payload is itself a flex; consumers may
     * automatically run flexValidateBuffer on it after digest passes. */
    FLEX_ENV_FLAG_PAYLOAD_FLEX = 1u << 0,

    /* A 32-bit key id varint follows immediately after the `flags`
     * varint in the wire. When set, a verifier with a bound keyring
     * (flexEnvelopeVerifyParams::keyring) can auto-resolve the prepared
     * key by id — no out-of-band key selection needed. When clear, no
     * key id is on the wire and the verifier must supply `key` /
     * `key_len` or `prepared_key` directly. Multi-key rotation
     * windows: producers tag with the current id; receivers accept
     * any id their keyring holds. */
    FLEX_ENV_FLAG_KEYID_PRESENT = 1u << 1,

    /* Reserved for future compression marker. Producer must not set
     * this bit in V1; verify rejects envelopes with unknown bits set. */
    /* FLEX_ENV_FLAG_PAYLOAD_COMPRESSED = 1u << 2, */
} flexEnvelopeFlag;

/* Mask of all flag bits valid in the current envelope version. Any bit
 * set in the wire flags but NOT in this mask causes verify to reject. */
#define FLEX_ENV_FLAGS_VALID_V1                                                \
    ((uint32_t)(FLEX_ENV_FLAG_PAYLOAD_FLEX | FLEX_ENV_FLAG_KEYID_PRESENT))

/* === Verify policy (caller-supplied, OR-able) =======================
 *
 * Default behavior (opts == 0) is the strictest sensible policy.
 * Permissive behavior — accepting trailing bytes, accepting ALGO_NONE,
 * accepting deprecated algorithms, fast (non-constant-time) compare —
 * requires an explicit flag. This keeps callers from accidentally
 * downgrading to no-integrity ("alg=none" footgun) and similar. */
typedef enum flexEnvelopeVerifyOpt {
    FLEX_ENV_VERIFY_DEFAULT = 0,

    FLEX_ENV_VERIFY_ALLOW_TRAILING = 1u << 0,
    FLEX_ENV_VERIFY_REQUIRE_FLEX = 1u << 1,
    FLEX_ENV_VERIFY_FAST_COMPARE = 1u << 2,
    FLEX_ENV_VERIFY_REQUIRE_HMAC = 1u << 3,
    FLEX_ENV_VERIFY_ALLOW_DEPRECATED = 1u << 4,
    FLEX_ENV_VERIFY_PEEK_ONLY = 1u << 5,
    FLEX_ENV_VERIFY_ALLOW_NONE_ALGO = 1u << 6,
    FLEX_ENV_VERIFY_ALLOW_OVERSIZE = 1u << 7,
    FLEX_ENV_VERIFY_BOUND_PAYLOAD = 1u << 8,

    /* explicit-opt-in to let a wire `key_id` override a
     * caller-supplied key. When the verify call is given (a) a bound
     * keyring AND (b) caller-supplied key material (`prepared_key` or
     * raw `key`/`key_len`) AND (c) the wire frame carries
     * FLEX_ENV_FLAG_KEYID_PRESENT, the intent is ambiguous: should the
     * keyring's id-resolved key win (the historical silent override) or
     * should the caller-supplied key win? Default behavior REJECTS the
     * frame with FLEX_ENV_VERIFY_FAILED_AMBIGUOUS_KEY so the caller is
     * forced to disambiguate explicitly. Asserting this flag tells
     * verify "I know about the override; please honor the wire id" and
     * restores the prior silent-override behavior for the duration of
     * the call. Callers that intend the caller-supplied key to always
     * win should simply leave `keyring` NULL (or set this flag while
     * supplying a keyring-only verify path elsewhere). */
    FLEX_ENV_VERIFY_ACCEPT_WIRE_KEYID = 1u << 9,
} flexEnvelopeVerifyOpt;

/* Typed error codes returned via flexEnvelopeLastVerifyError after a
 * flexEnvelopeVerify or flexEnvelopeOpen call that returned false. The
 * codes are best-effort and additive — only failure modes the caller
 * needs to disambiguate get a code; legacy framing failures collapse to
 * FLEX_ENV_VERIFY_FAILED_GENERIC.
 *
 * The error is stored in thread-local storage and is sticky until the
 * next successful or failed verify on the same thread. Inspecting it
 * after a successful verify returns FLEX_ENV_VERIFY_OK. */
typedef enum [[nodiscard]] flexEnvelopeVerifyErr {
    FLEX_ENV_VERIFY_OK = 0,
    /* Generic failure — framing/parse/digest/etc — no further
     * disambiguation available. Most failure paths land here today. */
    FLEX_ENV_VERIFY_FAILED_GENERIC = 1,
    /* the envelope's wire `key_id` is below the bound
     * keyring's revoked-key floor (hmacKeyringMinValidKeyId). Strong
     * signal to the caller that the frame is a replay under an id the
     * operator already declared revoked. */
    FLEX_ENV_VERIFY_FAILED_KEY_REVOKED = 2,
    /* the envelope's (key_id, nonce) tuple was already
     * observed in the bound aeadNonceLog window. Surfaced from both the
     * Seal side (caller MUST stop — would destroy AEAD confidentiality)
     * and the Open side (defense-in-depth replay catch when caller binds
     * a consumer-side log). */
    FLEX_ENV_VERIFY_FAILED_NONCE_REUSED = 3,
    /* ambiguous key resolution. The caller supplied
     * BOTH a key handle (raw `key`/`key_len` or `prepared_key`) AND a
     * bound `keyring`, AND the wire frame carries
     * FLEX_ENV_FLAG_KEYID_PRESENT — so the verifier cannot tell which
     * key the caller wants used. Default fail-safe; assert
     * FLEX_ENV_VERIFY_ACCEPT_WIRE_KEYID to opt in to the historical
     * silent-override (wire id wins) or omit the keyring (caller key
     * wins). */
    FLEX_ENV_VERIFY_FAILED_AMBIGUOUS_KEY = 4,
} flexEnvelopeVerifyErr;

/* Read the most recent typed verify error for the calling thread.
 * Cleared (set to FLEX_ENV_VERIFY_OK) on every successful
 * flexEnvelopeVerify and overwritten on every failed one. Returns
 * FLEX_ENV_VERIFY_OK on threads that have never called verify. */
flexEnvelopeVerifyErr flexEnvelopeLastVerifyError(void);

/* === Public structs (packed largest-to-smallest, fixed-width) ======== */

typedef struct flexEnvelopeHeader {
    uint64_t payload_len;
    uint64_t digest_len;
    uint64_t header_size;
    uint32_t flags;
    uint32_t key_id; /* valid iff (flags & FLEX_ENV_FLAG_KEYID_PRESENT) */
    flexEnvelopeAlgo algo;
    uint8_t version;
    /* GUARANTEED zero — populated to {0,0,0} on every successful Peek /
     * Verify return. Defense-in-depth assertion lives inside
     * flexEnvelopePeek: callers may rely on these bytes being zero in
     * any header struct returned from this module. The wire format does
     * NOT carry a reserved-bytes field today; "reserved" space on the
     * wire lives in the unused high bits of the `flags` varint, which
     * parseFraming already rejects via FLEX_ENV_FLAGS_VALID_V1 masking. */
    uint8_t _reserved[3];
} flexEnvelopeHeader;

typedef struct flexEnvelopeWrapParams {
    const uint8_t *key;
    size_t key_len;
    flexEnvelopeAlgo algo;
    uint32_t on_wire_flags;
    /* OPTIONAL: when non-zero AND the algorithm is keyed-class, the
     * envelope wire format carries this 32-bit id (FLEX_ENV_FLAG_KEYID_
     * PRESENT is set automatically). Receivers with a bound keyring
     * use the id to auto-resolve the verify key. Zero means "no key
     * id on the wire" — the legacy form, equivalent to omitting the
     * field. */
    uint32_t key_id;
    uint8_t version;
    /* MUST be zero — reserved for future use. flexEnvelopeWrap asserts
     * every byte is 0x00 at entry and refuses the call otherwise.
     * Callers MUST NOT stash side-channel data here: a future field
     * expansion will steal these bytes, and a non-zero value today is
     * almost certainly a stack-uninitialized-params bug. The matching
     * sibling field on flexEnvelopeSealParams carries the same contract. */
    uint8_t _pad[3];
    /* OPTIONAL: ignored by flexEnvelopeWrap (no nonce in checksum/MAC
     * class). Present so cluster setup can populate one params struct
     * and pass it to both Wrap and Seal call sites; AEAD-class operations
     * go through flexEnvelopeSeal which reads its sibling
     * flexEnvelopeSealParams::nonceLog. */
    struct aeadNonceLog *nonceLog;
} flexEnvelopeWrapParams;

/* Forward decls — full types live in hmac.h / hmacKeyring.h. */
struct hmacKey;
struct hmacKeyring;
/* Forward decl — full type lives in aeadNonceLog.h. Opt-in defense-in-
 * depth detector for (key_id, nonce) reuse on AEAD-class envelopes.
 * Producers wire it via flexEnvelopeSealParams::nonceLog; consumers may
 * wire it via flexEnvelopeVerifyParams::nonceLog for replay catch. */
struct aeadNonceLog;

typedef struct flexEnvelopeVerifyParams {
    uint64_t max_payload_len;
    const uint8_t *key;
    size_t key_len;
    /* OPTIONAL: caller-supplied pre-prepared HMAC key (hmac.h
     * hmacPrepareKey output). When non-NULL AND the envelope's
     * algorithm is hmac_sha256 / hmac_blake3, verify uses the prepared
     * state directly and skips the two block-compressions that
     * hmacCompute would otherwise pay per call. Ignored for non-HMAC
     * algorithms. When set, `key` / `key_len` may be NULL/0 since the
     * key bytes are already absorbed into the prepared state. */
    const struct hmacKey *prepared_key;
    /* OPTIONAL: caller-supplied keyring. When non-NULL AND the wire
     * envelope carries FLEX_ENV_FLAG_KEYID_PRESENT, verify looks up
     * the prepared key by the wire's key id and uses it; this lets a
     * receiver auto-select among a rotation window's worth of keys
     * without out-of-band coordination. Falls back to `prepared_key`
     * / `key` if lookup misses or the flag is not set.
     *
     * — ambiguous-key fail-safe: if the caller ALSO
     * supplies `prepared_key` or raw `key`/`key_len` while the wire has
     * KEYID_PRESENT, verify REJECTS the frame with the typed error
     * FLEX_ENV_VERIFY_FAILED_AMBIGUOUS_KEY unless the caller asserts
     * FLEX_ENV_VERIFY_ACCEPT_WIRE_KEYID in `opts`. Legacy callers that
     * relied on the silent-override semantics must either (a) add the
     * flag, or (b) drop the redundant caller-supplied key. */
    const struct hmacKeyring *keyring;
    /* OPTIONAL : consumer-side defense-in-depth replay
     * detector for AEAD envelopes (flexEnvelopeOpen path). When non-NULL
     * and the framed algo is AEAD-class, Open calls
     * aeadNonceLogObserve(log, key_id, nonce) AFTER tag verification; if
     * Observe returns false the call fails with
     * FLEX_ENV_VERIFY_FAILED_NONCE_REUSED. Ignored for non-AEAD algos
     * and ignored entirely when NULL (legacy callers stay opt-out). Note
     * the producer side wires its log via flexEnvelopeSealParams::nonceLog
     * — the two sides are independent; binding both gives end-to-end
     * coverage even across process boundaries. */
    struct aeadNonceLog *nonceLog;
    uint32_t opts;
    uint32_t _pad;
} flexEnvelopeVerifyParams;

/* === Algorithm queries ============================================== */

size_t flexEnvelopeAlgoDigestLen(flexEnvelopeAlgo algo);
const char *flexEnvelopeAlgoName(flexEnvelopeAlgo algo);
bool flexEnvelopeAlgoIsKeyed(flexEnvelopeAlgo algo);
bool flexEnvelopeAlgoIsDeprecated(flexEnvelopeAlgo algo);

/* === Sizing ========================================================== */

/* Exact bytes a wrap of `payload_len` under `algo` with `on_wire_flags`
 * will consume. Returns 0 if algo is unknown or payload_len overflows. */
size_t flexEnvelopeWrappedSize(uint64_t payload_len, flexEnvelopeAlgo algo,
                               uint32_t on_wire_flags);

/* === Wrap ============================================================
 *
 * Writes a complete envelope into `out`. `out` must be at least
 * flexEnvelopeWrappedSize(...) bytes. On success returns true and sets
 * *out_written to the actual byte count (== wrapped size).
 *
 * Refuses to emit FLEX_ENV_ALGO_NONE unless built with
 * -DDATAKIT_ENABLE_INSECURE_TEST_ALGOS. Refuses to emit a keyed-class
 * algorithm without a key. These checks are unconditional — they are
 * not policy flags. */
bool flexEnvelopeWrap(uint8_t *out, size_t out_cap, const uint8_t *payload,
                      uint64_t payload_len,
                      const flexEnvelopeWrapParams *params,
                      size_t *out_written);

/* === Zero-copy Wrap =================================================
 *
 * Fast-path bypass for callers that can write payload bytes DIRECTLY
 * into the envelope buffer rather than staging in a temp and memcpy'ing
 * into Wrap. The on-wire output is byte-identical to flexEnvelopeWrap;
 * only the caller's dataflow changes.
 *
 * Usage:
 *   uint8_t *region = NULL;
 *   size_t header_len = 0;
 *   if (!flexEnvelopeReserveWrap(out, out_cap, payload_len, &params,
 *                                &region, &header_len)) { ... }
 *   <emit payload_len bytes into `region` here — direct write,
 *    no temp buffer>
 *   size_t written = 0;
 *   if (!flexEnvelopeFinalizeWrap(out, out_cap, header_len, payload_len,
 *                                 &params, &written)) { ... }
 *
 * Contract:
 *  - `params` MUST be byte-identical between Reserve and Finalize (same
 *    algo, key, key_id, version, flags). Finalize re-validates and
 *    refuses on mismatch.
 *  - The caller MUST NOT touch envelope bytes outside
 *    [header_len, header_len + payload_len) between Reserve and Finalize
 *    — the magic / version / algo / flags / payload_len fields are
 *    sanity-checked on Finalize entry.
 *  - On Reserve failure, `out_cap` bytes of `out` may be partially
 *    written but no caller invariant relies on a particular leftover
 *    state. Finalize without a prior successful Reserve is undefined.
 *
 * Returns false on validation failure (unknown algo, missing key,
 * undersized `out_cap`, NULL out-pointers, etc); see flexEnvelope.c for
 * the exact gate list — it mirrors flexEnvelopeWrap one-for-one. */
bool flexEnvelopeReserveWrap(uint8_t *out, size_t out_cap, uint64_t payload_len,
                             const flexEnvelopeWrapParams *params,
                             uint8_t **payload_region_out,
                             size_t *header_len_out);

bool flexEnvelopeFinalizeWrap(uint8_t *out, size_t out_cap, size_t header_len,
                              uint64_t payload_len,
                              const flexEnvelopeWrapParams *params,
                              size_t *written_out);

/* === Peek (framing only, no digest work) ============================
 *
 * Parses the framing fields and reports them in *out. Does NOT compute
 * or verify the digest. Use this for routing / inspection / when the
 * digest verify will happen elsewhere (e.g., later in a streaming
 * pipeline).
 *
 * Returns true iff the framing parses cleanly and the algorithm is
 * recognized. On failure *out is left untouched. */
bool flexEnvelopePeek(const uint8_t *buf, size_t buf_len,
                      flexEnvelopeHeader *out);

/* === Verify =========================================================
 *
 * Parses the framing, enforces secure-default invariants and any opt-in
 * policy flags in params->opts, computes the digest, and compares it
 * against the wire digest. On success, *out_payload points INTO `buf`
 * (do NOT free separately) and *out_payload_len is the payload byte
 * count.
 *
 * Constant-time compare is the default for keyed-class algorithms;
 * caller can opt into fast non-constant-time compare with
 * FLEX_ENV_VERIFY_FAST_COMPARE if the threat model permits.
 *
 * `params` may be NULL — equivalent to a zero-initialized params struct
 * (strictest defaults, no key, no max). */
bool flexEnvelopeVerify(const uint8_t *buf, size_t buf_len,
                        const flexEnvelopeVerifyParams *params,
                        const uint8_t **out_payload, uint64_t *out_payload_len);

/* === Convenience: verify + heap-copy as flex ========================
 *
 * Calls flexEnvelopeVerify and, on success, allocates a fresh flex
 * containing the payload bytes (caller owns; free via flexFree).
 *
 * If params->opts includes FLEX_ENV_VERIFY_REQUIRE_FLEX (or
 * payload-flex on-wire flag is set), runs flexValidateBuffer on the
 * payload and returns NULL if the structural check fails.
 *
 * Returns NULL on any verify or validate failure. */
flex *flexEnvelopeUnwrapAsFlex(const uint8_t *buf, size_t buf_len,
                               const flexEnvelopeVerifyParams *params);

/* === AEAD Seal / Open (authenticated encryption) ====================
 *
 * AEAD-class algorithms (FLEX_ENV_ALGO_IS_AEAD(algo) == true) encrypt
 * the payload and produce an authentication tag in one pass. They go
 * through this dedicated API rather than Wrap/Verify because:
 * - The payload bytes ON DISK are CIPHERTEXT. Verify cannot return a
 * pointer-into-buffer for the plaintext.
 * - Sealing requires a fresh per-envelope nonce, which the framing
 * stores alongside the ciphertext.
 *
 * Nonce semantics (critical, READ):
 * - Caller MUST supply a unique nonce for every (key, plaintext)
 * triple. Reuse with the same key destroys confidentiality AND
 * authenticity. Typical strategies: 96-bit counter, random with
 * enough entropy, or a derived hash of a sequence number.
 * - Suggested counter pattern: start at 0 for a fresh key, increment
 * per envelope, retire the key well before 2^96 envelopes.
 * - The envelope verifies the tag using the on-wire nonce — modifying
 * it on the wire fails Open. */

typedef struct flexEnvelopeSealParams {
    const uint8_t *key;
    const uint8_t *nonce; /* per-algo length; 12 bytes for chacha20_poly1305 */
    size_t key_len;
    size_t nonce_len;
    flexEnvelopeAlgo algo;
    uint32_t on_wire_flags;
    /* OPTIONAL : when non-NULL, Seal calls
     * aeadNonceLogObserve(log, key_id, nonce) BEFORE running the cipher.
     * If Observe returns false (duplicate / chain saturated), Seal aborts
     * and flexEnvelopeLastVerifyError reports
     * FLEX_ENV_VERIFY_FAILED_NONCE_REUSED. This is the producer-side
     * guard against catastrophic (key, nonce) reuse — a single repeat
     * destroys ChaCha20-Poly1305 confidentiality AND integrity.
     *
     * `key_id` is taken from the wire field below (when KEYID_PRESENT
     * is set in on_wire_flags) or, if not wire-attached, from the
     * `effective_key_id` field below. When neither identifies the key,
     * the log is bypassed (NULL nonceLog is the only correct behavior
     * for nonces issued via a non-keyring channel). The aeadNonceLog
     * itself rejects key_id == 0. */
    struct aeadNonceLog *nonceLog;
    /* OPTIONAL: key id used when nonceLog is non-NULL but the wire is
     * not carrying KEYID_PRESENT. Ignored when nonceLog is NULL or
     * KEYID_PRESENT is set (then the wire key_id is used). Zero means
     * "no key id available" — nonceLog observation is then skipped. */
    uint32_t key_id;
    uint8_t version;
    /* MUST be zero — reserved for future use. flexEnvelopeSeal asserts
     * every byte is 0x00 at entry and refuses the call otherwise.
     * Same contract as flexEnvelopeWrapParams::_pad — a non-zero value
     * is almost certainly a stack-uninitialized-params bug, and future
     * field expansion will reclaim these bytes. */
    uint8_t _pad[3];
    /* OPTIONAL : when non-NULL AND `key_id != 0` AND
     * `key == NULL && key_len == 0`, Seal resolves the raw key bytes
     * via hmacKeyringGetRawCopy(keyring, key_id) into a local stack
     * buffer and — for AES-GCM frames — copies the pre-computed
     * schedule cached on the keyring slot
     * (hmacKeyringGetAesGcmKeyCopy) to skip per-frame aesGcmInit cost.
     * Copy-style lookups close the borrowed-pointer UAF window a
     * concurrent hmacKeyringRetire would otherwise open mid-Seal.
     *
     * When the caller supplies `key` directly (`key != NULL`), the
     * keyring is ignored; the cold-path aesGcmInit then runs as before.
     * This keeps the historical caller-supplied-key contract intact
     * while enabling the cache for keyring-bound producers. */
    const struct hmacKeyring *keyring;
} flexEnvelopeSealParams;

/* Encrypt + authenticate `plaintext` into `out`. `out_cap` must be at
 * least flexEnvelopeWrappedSize(plaintext_len, algo, on_wire_flags)
 * bytes — this accounts for framing + nonce slot + ciphertext + tag.
 *
 * Refuses to operate if:
 * - algo is not AEAD-class
 * - key is NULL or wrong length for the algo
 * - nonce is NULL or nonce_len doesn't match the algo (12 for
 * chacha20_poly1305)
 *
 * On success returns true and writes the byte count to *out_written. */
bool flexEnvelopeSeal(uint8_t *out, size_t out_cap, const uint8_t *plaintext,
                      uint64_t plaintext_len,
                      const flexEnvelopeSealParams *params,
                      size_t *out_written);

/* === Zero-copy Seal =================================================
 *
 * AEAD counterpart to flexEnvelopeReserveWrap / flexEnvelopeFinalizeWrap.
 * The caller writes plaintext bytes directly into the reserved region;
 * Finalize then encrypts in place (the underlying ChaCha20-Poly1305 and
 * AES-GCM seal entry points support `src == dst` aliasing).
 *
 * Sequencing note: the (key_id, nonce) reuse check (aeadNonceLogObserve)
 * fires INSIDE Reserve — BEFORE the caller wastes cycles producing
 * plaintext. A reuse failure surfaces via flexEnvelopeLastVerifyError()
 * just like flexEnvelopeSeal would.
 *
 * Same contract as the Wrap variant: `params` must match between Reserve
 * and Finalize; the caller must not touch envelope bytes outside the
 * reserved [header+nonce, header+nonce+plaintext_len) region.
 *
 * `header_plus_nonce_len_out` is the byte count of the framing header
 * PLUS the in-line nonce — the AAD region the cipher will authenticate.
 * Pass it back unchanged to FinalizeSeal. */
bool flexEnvelopeReserveSeal(uint8_t *out, size_t out_cap,
                             uint64_t plaintext_len,
                             const flexEnvelopeSealParams *params,
                             uint8_t **plaintext_region_out,
                             size_t *header_plus_nonce_len_out);

bool flexEnvelopeFinalizeSeal(uint8_t *out, size_t out_cap,
                              size_t header_plus_nonce_len,
                              uint64_t plaintext_len,
                              const flexEnvelopeSealParams *params,
                              size_t *written_out);

/* Decrypt and authenticate an AEAD envelope. On success, writes the
 * plaintext into `plaintext_out` (which must be at least the envelope's
 * ciphertext length — equal to the framing payload_len) and reports the
 * byte count to *plaintext_written.
 *
 * Returns false on:
 * - framing parse failure
 * - algo not AEAD-class
 * - tag mismatch (constant-time compare)
 * - plaintext_out_cap too small
 * - no key supplied AND no keyring auto-resolution succeeded
 * - wire key_id below the bound keyring's revoked-key floor (typed as
 * FLEX_ENV_VERIFY_FAILED_KEY_REVOKED, mirrors flexEnvelopeVerify)
 *
 * Key resolution precedence :
 * 1. If `params->key` is non-NULL with `params->key_len > 0`, that key
 * is used unconditionally — wins over any bound keyring (lets a
 * caller override a ring-resolved key without rebuilding the ring).
 * 2. Else if `params->keyring` is bound AND the wire frame carries
 * FLEX_ENV_FLAG_KEYID_PRESENT, the raw key bytes are auto-resolved
 * via hmacKeyringGetRawCopy(ring, wire_key_id) into a local stack
 * buffer. Mirrors the symmetric auto-resolution flexEnvelopeVerify
 * performs for HMAC class. The copy-style lookup closes the
 * borrowed-pointer UAF a concurrent Retire would otherwise open
 * mid-Open.
 * 3. Else Open fails (no key material).
 *
 * On failure, `plaintext_out` (up to plaintext_out_cap bytes) is zeroed
 * — defense against callers that forget to check the return value.
 * `plaintext_out` may alias the envelope's ciphertext region for
 * in-place decryption. */
bool flexEnvelopeOpen(const uint8_t *buf, size_t buf_len,
                      const flexEnvelopeVerifyParams *params,
                      uint8_t *plaintext_out, size_t plaintext_out_cap,
                      size_t *plaintext_written);

/* === Streaming API ==================================================
 *
 * Wrap and verify payloads that don't fit comfortably in RAM (snapshots,
 * replication batches, multi-gigabyte transfers). The wire format is
 * byte-identical to one-shot wrap, so a streaming-wrapped envelope is
 * verifiable by one-shot Verify and vice-versa.
 *
 * Lifecycle (wrap):
 * 1. Init(ctx, payload_len, params, header_out, header_out_cap,
 * &header_written) — emits the framing header bytes. Caller
 * MUST know payload_len up front; the wire format encodes it.
 * 2. Update(ctx, chunk, len) zero or more times — feeds the payload
 * bytes into the running digest. THE STREAM API DOES NOT BUFFER
 * the payload; the caller is responsible for emitting the same
 * bytes to its downstream consumer (file write, socket send, etc).
 * Total bytes fed across all Update calls MUST equal payload_len.
 * 3. Final(ctx, digest_out, digest_out_cap, &digest_written) — emits
 * the trailing digest bytes. After Final, the ctx is consumed and
 * the secret-bearing regions (`hmac_storage` and the `u` union)
 * are wiped by Final itself. Callers MUST treat the ctx as
 * discarded after Final — reuse without a fresh Init is rejected.
 *
 * Lifecycle (verify) is the same shape; Update accepts arbitrary chunk
 * sizes and Final returns true iff the running digest matches the wire
 * digest under the same constant-time / fast policy rules as one-shot.
 * Final wipes the same secret-bearing regions on both the success and
 * the byte-/digest-mismatch failure paths.
 *
 * The streaming context is caller-owned, fixed size, never allocates on
 * the hot path. sizeof is bounded (under 16 KiB) regardless of total
 * payload size.
 *
 * Every checksum and keyed algorithm streams.  XXH3_64 and XXH3_128 need
 * the context at its type's 64-byte alignment, which stack, static and
 * alignas storage have; Init refuses them for a context at a lesser
 * address (a plain malloc of the context may give one).
 *
 * Both contexts internally union the per-algorithm running-hash state
 * (XXH64_state_t / XXH3_state_t / blake3Hasher) and embed an HMAC
 * scratch area sized for the worst case (2 * blake3Hasher + 1 block). */

/* Worst-case HMAC storage: two blake3 hash contexts plus a 2*block_size
 * scratch region. HMAC-BLAKE3 dominates over HMAC-SHA-256. */
#define FLEX_ENV_STREAM_HMAC_STORAGE                                           \
    ((size_t)(2 * sizeof(blake3Hasher) + 2 * BLAKE3_BLOCK_LEN))

/* Maximum digest length emitted by any registered envelope algorithm.
 * Stack scratch buffers in verify paths use this; if a future algorithm
 * exceeds it the static_assert in flexEnvelope.c (next to the algo
 * descriptor table) will trip at compile time. */
#define FLEX_ENV_MAX_DIGEST_LEN ((size_t)64)

/* Maximum nonce length carried in the framing slot of an AEAD-class
 * envelope. ChaCha20-Poly1305 uses 12 bytes; AES-GCM (a likely future
 * addition) also uses 12 bytes per RFC 5116. Bumping past 16 would
 * require auditing every nonce-touching path; the static_assert in
 * flexEnvelope.c locks the assumption. */
#define FLEX_ENV_MAX_NONCE_LEN ((size_t)16)

/* Streaming context lifecycle states, one byte wide so the contexts
 * stay packed. */
typedef enum flexEnvelopeStreamPhase : uint8_t {
    FLEX_ENV_STREAM_PHASE_UNINIT = 0, /* freshly memset; Init not yet called */
    FLEX_ENV_STREAM_PHASE_OPEN = 1, /* Init succeeded; Update calls accepted */
    FLEX_ENV_STREAM_PHASE_FINALIZED = 2, /* Final returned; no further ops */
    FLEX_ENV_STREAM_PHASE_POISONED =
        3, /* verify-side: an Update or Init failure
            * permanently rejects every later op,
            * since the protocol state is already
            * corrupted and resuming would mask it */
} flexEnvelopeStreamPhase;

typedef struct flexEnvelopeWrapStream {
    /* Largest fields first. hmac_storage is byte-flat and sized for the
     * worst case (HMAC-BLAKE3). Unused for non-HMAC algorithms. */
    uint8_t hmac_storage[FLEX_ENV_STREAM_HMAC_STORAGE];
    /* Per-algorithm running-hash state. Only one variant is active at a
     * time, selected by `algo`. */
    union flexEnvelopeWrapStreamState {
        blake3Hasher blake3;
        XXH3_state_t xxh3;
        XXH64_state_t xxh64;
        rapidhashCtx rapidhash;
    } u;
    hmacCtx hmac;
    uint64_t payload_len_declared;
    uint64_t payload_bytes_seen;
    flexEnvelopeAlgo algo;
    uint8_t digest_len;
    flexEnvelopeStreamPhase phase;
    uint8_t _pad[4];
} flexEnvelopeWrapStream;

typedef struct flexEnvelopeVerifyStream {
    /* Layout mirrors the wrap stream. Additionally stashes the wire
     * digest bytes (parsed from the framing-suffix on Final) — but
     * since the digest lives at the END of the envelope, the caller
     * supplies it via the last Update or as a separate Final argument.
     * Implementation: Update accumulates trailing bytes into a
     * digest_tail buffer of size <= max_digest_len so that on Final we
     * can compare. */
    uint8_t hmac_storage[FLEX_ENV_STREAM_HMAC_STORAGE];
    union flexEnvelopeVerifyStreamState {
        blake3Hasher blake3;
        XXH3_state_t xxh3;
        XXH64_state_t xxh64;
        rapidhashCtx rapidhash;
    } u;
    hmacCtx hmac;
    const struct hmacKey *prepared_key;
    const uint8_t *raw_key;
    size_t raw_key_len;
    uint64_t payload_len_declared;
    uint64_t payload_bytes_seen;
    uint32_t opts;
    flexEnvelopeAlgo algo;
    uint8_t digest_len;
    flexEnvelopeStreamPhase phase;
    /* Wire digest, captured byte-by-byte from the trailing portion of
     * the stream. Sized for the largest current algorithm digest. */
    uint8_t wire_digest[FLEX_ENV_MAX_DIGEST_LEN];
    uint8_t wire_digest_seen;
    uint8_t _pad[7];
} flexEnvelopeVerifyStream;

/* --- Wrap streaming -------------------------------------------------- */

bool flexEnvelopeWrapStreamInit(flexEnvelopeWrapStream *ctx,
                                uint64_t payload_len,
                                const flexEnvelopeWrapParams *params,
                                uint8_t *header_out, size_t header_out_cap,
                                size_t *header_written);

bool flexEnvelopeWrapStreamUpdate(flexEnvelopeWrapStream *ctx,
                                  const uint8_t *chunk, size_t len);

bool flexEnvelopeWrapStreamFinal(flexEnvelopeWrapStream *ctx,
                                 uint8_t *digest_out, size_t digest_out_cap,
                                 size_t *digest_written);

/* --- Verify streaming ------------------------------------------------- */

/* Init parses the framing header from `header_buf` (the caller supplies
 * enough leading bytes — at minimum the full framing header. The
 * required header length is reported on success in *header_consumed, so
 * the caller knows how many bytes from `header_buf` were framing vs.
 * payload). Reports the parsed payload_len and algo to the caller. */
bool flexEnvelopeVerifyStreamInit(flexEnvelopeVerifyStream *ctx,
                                  const flexEnvelopeVerifyParams *params,
                                  const uint8_t *header_buf, size_t header_len,
                                  size_t *header_consumed,
                                  uint64_t *out_payload_len,
                                  flexEnvelopeAlgo *out_algo);

bool flexEnvelopeVerifyStreamUpdate(flexEnvelopeVerifyStream *ctx,
                                    const uint8_t *chunk, size_t len);

/* Returns true iff the running digest matches the captured wire digest.
 * Caller must have fed exactly payload_len + digest_len bytes across all
 * Update calls. */
bool flexEnvelopeVerifyStreamFinal(flexEnvelopeVerifyStream *ctx);

#ifdef DATAKIT_TEST
int flexEnvelopeTest(int argc, char *argv[]);
int dkTestFlexEnvelopeKeyringRetireRaceTest(int argc, char *argv[]);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif
