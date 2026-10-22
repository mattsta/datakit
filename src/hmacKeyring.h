#pragma once

/* hmacKeyring — caller-owned key registry for envelope MAC algorithms.
 *
 * Wraps `hmacPrepareKey` from hmac.h so callers can install a key once,
 * obtain a prepared-key handle, and hand that handle to every subsequent
 * envelope verify on the hot path — no per-message key derivation, no
 * per-message memory allocation.
 *
 * Concurrency model:
 * - One writer (install / retire) at a time.
 * - Unlimited concurrent readers (lookup). Backed by pthread_rwlock.
 * - The handle returned by lookup remains valid until the same key id
 * is retired or the keyring is freed. Callers must not hold onto
 * a handle across a retire — that is the only invalidation point.
 *
 * Borrowed-pointer lifetime contract (read once, internalise):
 * - The pointers written by `hmacKeyringLookup`, `hmacKeyringGetRaw`,
 * `hmacKeyringGetAesGcmKey`, and any other lookup-style accessor are
 * BORROWED into the keyring's internal slot storage. The rwlock is
 * released BEFORE these functions return.
 * - "Valid" means "valid until the next mutation of this slot on ANY
 * thread". The set of mutating calls is: `hmacKeyringRetire`,
 * `hmacKeyringInstall`, `hmacKeyringInstallUnsalted`,
 * `hmacKeyringSetAeadSalt`, `hmacKeyringRestore`, `hmacKeyringFree`.
 * - Callers that need to hold key material past any of those calls
 * MUST use `hmacKeyringGetRawCopy` (raw bytes) or
 * `hmacKeyringGetAesGcmKeyCopy` (AEAD key schedule), both below, and
 * own the resulting copy themselves.
 * - Multi-thread callers that consume a borrowed pointer MUST pair the
 * read with `hmacKeyringGetGeneration` and re-check the generation
 * before each consume; on mismatch, refuse the operation rather than
 * dereference the (possibly freed) bytes. The persist framework's
 * `persistResolveAttachedKey` (persist.c) is the canonical example.
 *
 * Security hygiene:
 * - Stored key bytes are zalloc'd, never realloc'd, and zero-cleaned
 * before free via OPENSSL_cleanse-style explicit_bzero. A leaked
 * keyring handle therefore does not leak a usable key.
 * - Install copies the caller's key bytes; the caller is free to
 * immediately wipe their own buffer.
 *
 * Out-of-scope (intentionally minimal):
 * - Persistence to disk — the user-facing module that loads keys
 * decides how to store them. This struct lives in RAM only.
 * - Network distribution — operators choreograph rotation across
 * nodes; this module just accepts whatever id/algo/bytes triple
 * the operator provides.
 *
 * =====================================================================
 * Lifecycle contracts (post-rotation hardening) — canonical reference
 * =====================================================================
 *
 * The per-API docstrings below carry the authoritative wording for each
 * entry point. This block is a quick map of how the pieces compose;
 * read it once, then follow the cross-references to the relevant entry
 * point.
 *
 * 1. INSTALL: salted vs. unsalted
 * - `hmacKeyringInstall(..., aead_salt)` is the production path.
 * The 4-byte `aead_salt` is required and becomes the suffix of
 * every 12-byte AEAD nonce issued via `hmacKeyringNextAeadNonce`.
 * The salt closes the cross-process nonce-collision footgun:
 * two processes installing the same key both start their counter
 * at zero, so without distinct salts they would emit identical
 * nonces — silently destroying ChaCha20-Poly1305 confidentiality
 * AND integrity. Install refuses an all-zero salt for that
 * reason.
 * - `hmacKeyringInstallUnsalted(...)` is the explicitly-named
 * path for single-process tests / scenarios that derive nonce
 * uniqueness from another channel. The slot is tagged "unsalted"
 * so `hmacKeyringNextAeadNonce` will issue zero-salt nonces. Do
 * not use in multi-process production.
 * - Runtime enforcement: `hmacKeyringNextAeadNonce` checks the per-
 * slot salt — if it is all-zero AND the slot was NOT installed via
 * the explicit unsalted entry point, the call refuses. Fail-
 * closed, even if a caller forgets the salt argument migration.
 *
 * 2. GENERATION counter (attach-style consumers)
 * - `hmacKeyringGetGeneration(ring, id, &gen)` reads a monotonically-
 * increasing per-slot uint64 that bumps on every mutation of that
 * slot: Install, Retire, SetAeadSalt.
 * - Any long-lived "attach" consumer (e.g. `persistAttachKeyring`)
 * MUST cache `(ring, key_id, generation)` at attach time and re-
 * check the generation on every consume. Mismatch means the key
 * was rotated (retired and/or re-installed) since attach — the
 * cached raw-key pointer would be a use-after-free, so the
 * operation refuses with a typed error. See `persist.h`:
 * `persistAttachKeyring` for the canonical implementation pattern
 * and `persistRebindKeyring` for the rotation-recovery entry
 * point ("I know the key changed; please pick up the new one").
 * Typed error codes flow through `persistLastError` —
 * `PERSIST_ERR_KEY_GENERATION_MISMATCH`, `_KEY_NOT_FOUND`,
 * `_NO_KEY_ATTACHED`.
 *
 * 3. REVOKED-key floor
 * - `hmacKeyringMinValidKeyId(ring)` reads the per-keyring floor.
 * - `hmacKeyringSetMinValidKeyId(ring, floor)` raises it. Strictly
 * monotonic-non-decreasing — backward moves are refused because
 * they would re-open the replay window for ids the operator has
 * already declared revoked.
 * - The floor and Retire are independent layers: Retire physically
 * removes the key bytes; the floor is policy. Set both when an
 * operator intends to revoke an id.
 * - Survives Snapshot/Restore as the snapshot's first entry, so a
 * restart cannot silently drop revocations.
 * - At envelope verify time: `flexEnvelopeVerify` consults the floor
 * via the wire-key-id auto-resolution path and rejects below-floor
 * ids with `FLEX_ENV_VERIFY_FAILED_KEY_REVOKED` (read via
 * `flexEnvelopeLastVerifyError` after the verify returns false).
 *
 * 4. PERSIST binding (header carries key_id)
 * - `persistAttachKeyring(p, ring, key_id)` consumes the chosen id
 * and from then on every snapshot/WAL header records that id in
 * the on-disk `keyId` field.
 * - On read-back: if the on-disk `keyId` is non-zero AND differs
 * from the currently-attached id, verify fails fast with
 * `PERSIST_ERR_KEY_ID_MISMATCH` (rather than the opaque generic
 * verify-failed an HMAC mismatch would produce). The expected id
 * is recoverable via `persistLastExpectedKeyId(p)` so operator
 * tooling can rebind the reader to the correct key.
 * - When integrating: pair Attach with a Rebind-on-rotation step.
 * Code that just calls Attach once and ignores generation
 * mismatches is racing the keyring; see contract #2 above.
 *
 * 5. ENVELOPE keyring auto-resolution (see flexEnvelope.h)
 * - The wire format may carry a `key_id` (set via
 * `flexEnvelopeWrapParams::key_id`). Verify-time auto-resolution
 * via `flexEnvelopeVerifyParams::keyring` looks the id up in the
 * bound keyring and uses the prepared handle, so a receiver in a
 * rotation window can pick the right key without out-of-band
 * coordination.
 * - Ambiguous-key fail-safe : when a caller
 * provides BOTH a key handle (`prepared_key` or raw `key`/`key_len`)
 * AND a keyring AND the wire frame carries
 * FLEX_ENV_FLAG_KEYID_PRESENT, `flexEnvelopeVerify` REJECTS the
 * frame with the typed error FLEX_ENV_VERIFY_FAILED_AMBIGUOUS_KEY
 * by default — the override-vs-caller-key intent must be made
 * explicit. The opt-in flag FLEX_ENV_VERIFY_ACCEPT_WIRE_KEYID
 * (in `flexEnvelopeVerifyParams::opts`) restores the historical
 * silent-override (wire id wins). Callers that want the caller-
 * supplied key to always win should simply leave `keyring` NULL.
 * `flexEnvelopeOpen` is not affected by this rule because its
 * precedence rule already makes the explicit `key` win
 * unconditionally — no silent override exists in Open.
 *
 * 6. THREADING (rwlock semantics)
 * - Read lock (many concurrent readers): `hmacKeyringLookup`,
 * `hmacKeyringGetRaw`, `hmacKeyringSize`, `hmacKeyringGetGeneration`,
 * `hmacKeyringMinValidKeyId`, `hmacKeyringNextAeadNonce` (the
 * per-slot counter bump is an atomic CAS under the read lock),
 * `hmacKeyringPeekAeadCounter`, `hmacKeyringSnapshot`.
 * - Write lock (single writer, blocks readers): `hmacKeyringInstall`,
 * `hmacKeyringInstallUnsalted`, `hmacKeyringRetire`,
 * `hmacKeyringSetAeadSalt`, `hmacKeyringSetMinValidKeyId`,
 * `hmacKeyringRestoreAeadCounter`, `hmacKeyringRestore`.
 * - Metrics scrapers and stats accessors take the read lock; they
 * are safe to call concurrently with hot-path NextAeadNonce /
 * Lookup traffic without disrupting the writer. Avoid calling a
 * write-locked entry point from a stats scraper — that would
 * stall the entire ring.
 * - All `out_*` pointer outputs are valid until the next mutation
 * of the same slot (Retire / re-Install / SetAeadSalt) or
 * `hmacKeyringFree`. Use the generation API (contract #2) to
 * detect stale handles cheaply. */

#ifdef __cplusplus
extern "C" {
#endif

#include "hmac.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward decl — full type lives in aesGcm.h. The cached AEAD key
 * schedule is opaque to most callers; flexEnvelopeOpen
 * holds the only consumer pointer at this time. */
struct aesGcmKey;

/* Caller-chosen 32-bit identifier for a key. Zero is reserved as
 * "invalid / no key" so callers can use it as a sentinel. Otherwise
 * any non-zero value is acceptable — typical schemes use a small
 * monotonically-increasing generation counter (1, 2, 3, ...) or an
 * opaque hash of the key material itself. */
typedef uint32_t hmacKeyId;
#define HMAC_KEY_ID_INVALID ((hmacKeyId)0)

/* Width of the per-key AEAD nonce salt. 4 bytes is the suffix appended
 * to the 8-byte counter to form the 12-byte ChaCha20-Poly1305 nonce. */
#define HMAC_KEYRING_AEAD_SALT_BYTES ((size_t)4)

/* Upper bound on a single keyring slot's raw-key length. Covers every
 * supported algorithm with headroom:
 *   - HMAC-SHA-256 key (one block): 64 bytes
 *   - HMAC-SHA-512 key (one block): 128 bytes (not yet wired, sized for it)
 *   - BLAKE3 keyed-hash key: 32 bytes
 *   - ChaCha20-Poly1305 / AES-256 / AES-128 keys: 16-32 bytes
 * Enforced by both `hmacKeyringInstall*` and `hmacKeyringRestore` so a
 * tampered snapshot cannot drive an attacker-controlled allocation. */
#define HMAC_KEYRING_MAX_KEY_BYTES ((size_t)256)

typedef struct hmacKeyring hmacKeyring;

/* Construct an empty keyring. Returns NULL on allocation failure. */
hmacKeyring *hmacKeyringCreate(void);

/* Destroy a keyring. Zero-cleans every installed key before freeing
 * its backing memory. Safe to call with NULL. */
void hmacKeyringFree(hmacKeyring *ring);

/* Install key bytes under `id` with an explicit AEAD nonce salt. The
 * salt becomes the 4-byte suffix of every 12-byte AEAD nonce issued via
 * hmacKeyringNextAeadNonce for this slot.
 *
 * Why the salt is required (fail-closed contract):
 * The (counter || salt) construction is what guarantees nonce
 * uniqueness ACROSS processes that install the same key — two nodes
 * both starting their counter at zero would collide nonces without
 * distinct salts, which would break ChaCha20-Poly1305 confidentiality
 * AND integrity. Forcing the caller to supply a salt at install time
 * removes the "I forgot" footgun. If the caller is single-process or
 * has another mechanism guaranteeing nonce uniqueness, the explicit
 * `hmacKeyringInstallUnsalted` variant below documents the choice.
 *
 * Returns false when:
 * - `id` is HMAC_KEY_ID_INVALID
 * - `id` is already installed and not yet retired
 * - `algo` / `key_bytes` is NULL or `key_len` is 0
 * - `aead_salt` is NULL
 * - `aead_salt` is all-zero (callers wanting all-zero must use the
 * explicit `hmacKeyringInstallUnsalted` variant)
 * - allocation fails
 *
 * On success the caller's `key_bytes` buffer can be wiped immediately —
 * the keyring has copied everything it needs. */
bool hmacKeyringInstall(hmacKeyring *ring, hmacKeyId id,
                        const hmacHashAlgo *algo, const uint8_t *key_bytes,
                        size_t key_len,
                        const uint8_t aead_salt[HMAC_KEYRING_AEAD_SALT_BYTES]);

/* DANGEROUS variant: install with an all-zero AEAD salt and mark the
 * slot so that hmacKeyringNextAeadNonce will allow zero-salt nonces.
 *
 * This is only safe in scenarios where nonce uniqueness is otherwise
 * guaranteed — single-process tests, legacy callers that derive
 * uniqueness from another channel, etc. Production multi-process
 * deployments MUST NOT use this entry point: two processes installing
 * the same key would collide their counter-0 nonces and silently
 * destroy confidentiality + integrity.
 *
 * Same return semantics as `hmacKeyringInstall` minus the salt checks. */
bool hmacKeyringInstallUnsalted(hmacKeyring *ring, hmacKeyId id,
                                const hmacHashAlgo *algo,
                                const uint8_t *key_bytes, size_t key_len);

/* Retire the key under `id`. Lookups for the id immediately start
 * returning NULL; the id slot becomes available for re-install. Any
 * pointer previously returned by hmacKeyringLookup for this id MUST
 * NOT be dereferenced after retire returns — callers that hold long-
 * lived handles must drop or re-resolve them. Returns false if `id`
 * was not present. */
bool hmacKeyringRetire(hmacKeyring *ring, hmacKeyId id);

/* Number of currently installed (non-retired) keys. */
size_t hmacKeyringSize(const hmacKeyring *ring);

/* Look up the prepared-key handle for `id`. Returns NULL if `id` is
 * not present (never installed, or installed-then-retired).
 *
 * LIFETIME CONTRACT: the returned pointer is BORROWED into the
 * keyring's internal slot. The rwlock is released BEFORE this function
 * returns; a concurrent Retire/Install/SetAeadSalt/Restore/Free on
 * another thread can invalidate the pointer the moment this call
 * returns. Callers MUST NOT retain the pointer past any keyring
 * mutation; for long-lived holdings, pair with `hmacKeyringGetGeneration`
 * and re-check generation before each use, or copy the underlying raw
 * bytes via `hmacKeyringGetRawCopy`. See the top-of-header contract
 * block for the full mutation list. */
const hmacKey *hmacKeyringLookup(hmacKeyring *ring, hmacKeyId id);

/* Resolve the raw key bytes for `id`. Useful when the caller needs to
 * pass `key`/`key_len` to a flexEnvelopeVerifyParams that does NOT yet
 * support the prepared-key fast path (e.g. envelope wrap is plain-key
 * only). On success returns true and writes pointers into the
 * keyring-owned storage.
 *
 * LIFETIME CONTRACT (read carefully):
 * The returned `*out_key_bytes` pointer is BORROWED into the keyring's
 * internal storage. It is valid ONLY until the NEXT keyring mutation
 * affecting this slot — on ANY thread — namely:
 *   - hmacKeyringRetire(ring, id)
 *   - hmacKeyringInstall / hmacKeyringInstallUnsalted that reuses the
 *     slot freed by a prior Retire (or first-time install under id)
 *   - hmacKeyringSetAeadSalt(ring, id, ...) — bumps generation; the
 *     bytes do not change but a generation-tracking caller MUST treat
 *     the pointer as stale
 *   - hmacKeyringRestore(ring, ...) — may install or retire any id
 *   - hmacKeyringFree(ring)
 *
 * Callers MUST NOT hold the pointer across any of those calls. The
 * keyring takes the rwlock during this function and releases it BEFORE
 * returning, so a concurrent Retire on another thread can begin freeing
 * the bytes the moment this call returns. The "safe to use immediately"
 * window is bounded by your own thread's next call into the keyring (or
 * by a designated rotation barrier in your application).
 *
 * For callers that need a snapshot of the bytes safe to hold without
 * keyring coordination, use `hmacKeyringGetRawCopy` (below) which
 * copies the bytes into a caller-supplied buffer under the rwlock.
 *
 * Multi-thread callers SHOULD pair this with `hmacKeyringGetGeneration`
 * — capture the generation at first use, re-check before consuming the
 * pointer, and refuse on mismatch. The persist framework's
 * `persistResolveAttachedKey` is the canonical reference for that
 * pattern; see persist.c. */
bool hmacKeyringGetRaw(hmacKeyring *ring, hmacKeyId id,
                       const uint8_t **out_key_bytes, size_t *out_key_len,
                       const hmacHashAlgo **out_algo);

/* Resolve the raw key bytes for `id` by COPY into a caller-supplied
 * buffer. Closes the lifetime hazard of `hmacKeyringGetRaw` for any
 * caller that needs to hold the bytes past a keyring-mutating window
 * — the copy is made under the keyring rwlock, so a concurrent Retire
 * cannot race with the memcpy.
 *
 * Returns true on success and writes the byte count to `*out_len`.
 * Returns false if:
 *   - `ring` or `out_buf` is NULL
 *   - `id` is HMAC_KEY_ID_INVALID
 *   - `id` is not currently installed
 *   - `out_cap` is smaller than the installed key length
 *
 * On false return, `out_buf` is left untouched and `*out_len` (if
 * non-NULL) is set to zero. On true return, `*out_len` (if non-NULL)
 * receives the number of bytes written into `out_buf` (always ==
 * the installed key length for that id).
 *
 * `out_algo` (optional, may be NULL) receives the hash-algo descriptor
 * pointer. Algo descriptors are module-static; the pointer remains
 * valid past keyring teardown.
 *
 * Security: the caller now owns key-equivalent material in `out_buf`.
 * Wipe it via `dkSecureZero` when done (see ctmem.h). */
bool hmacKeyringGetRawCopy(hmacKeyring *ring, hmacKeyId id, uint8_t *out_buf,
                           size_t out_cap, size_t *out_len,
                           const hmacHashAlgo **out_algo);

/* Read the current generation counter for slot `id`. The generation is
 * a monotonically-increasing per-slot uint64 that bumps on EVERY
 * mutation of that slot:
 *
 * - Install (including reuse of a previously-retired id)
 * - Retire (which then leaves the slot free; the bumped value is
 * observable until the next Install re-grabs the slot)
 * - SetAeadSalt (changes the AEAD nonce derivation surface)
 *
 * Long-lived "attach"-style consumers (e.g. persistAttachKeyring) cache
 * the generation observed at attach time alongside the (ring, key_id)
 * pair. Each time they consume the key they re-call Lookup AND
 * GetGeneration; a generation mismatch means "the key under this id has
 * been rotated since you attached — refuse to use it". This closes the
 * use-after-free hazard where retiring a key after attach would have
 * left attach-cached raw-key pointers dangling into freed memory.
 *
 * Returns false if `id` is not installed (and leaves `*out_generation`
 * unchanged); on success writes the current generation and returns true.
 *
 * Concurrency: cheap read under the read lock. Many concurrent readers
 * are safe; a write that bumps the generation is serialised against
 * readers by the same rwlock. */
bool hmacKeyringGetGeneration(hmacKeyring *ring, hmacKeyId id,
                              uint64_t *out_generation);

/* ---- Revoked key floor -------------------------------
 *
 * Closes the rotation-propagation replay window: during a key rotation
 * the producer has Promoted to a new id, but the old id is still
 * Install'd in every consumer's keyring because the operator hasn't
 * yet broadcast "retire previous". Without an additional check an
 * attacker who captured a frame keyed under the (now-retired-in-intent)
 * old id can replay it and the verifier accepts — the keyring still
 * resolves the id to a valid prepared handle.
 *
 * The floor is a per-keyring monotonically-non-decreasing `hmacKeyId`
 * value. Verify callers consult it via the wire-key-id auto-resolution
 * path in flexEnvelopeVerify (see flexEnvelope.h: floor reject yields
 * the FLEX_ENV_VERIFY_FAILED_KEY_REVOKED typed error).
 *
 * Semantics:
 * - Default floor is zero — equivalent to no floor (every installed
 * id is accepted).
 * - Set(floor) succeeds only if floor >= current floor. Backward
 * moves are refused because they would re-open the replay window
 * for ids the operator already declared revoked.
 * - The floor survives Snapshot/Restore (the snapshot's first entry)
 * so a restart cannot silently drop revocations.
 *
 * Floor and Retire are independent layers — Retire physically removes
 * the key bytes; the floor is policy. Both should be set together when
 * an operator intends to revoke an id. */

/* Read the current floor. Returns HMAC_KEY_ID_INVALID (0) if `ring`
 * is NULL or the floor has never been set (default). */
hmacKeyId hmacKeyringMinValidKeyId(const hmacKeyring *ring);

/* Raise the floor. Refuses to move it backwards (would re-permit a
 * revoked id) and refuses if `ring` is NULL. Returns true on success;
 * a no-op call (floor == current) is treated as success. */
bool hmacKeyringSetMinValidKeyId(hmacKeyring *ring, hmacKeyId floor);

/* ---- AEAD key-schedule cache ------------------------
 *
 * AES-NI / ARMv8-AES make per-frame AES-GCM AEAD work fast enough that
 * `aesGcmInit`'s round-key expansion becomes a measurable share of the
 * Open path on small payloads. When a key is Install'd under a length
 * that is a valid AES-GCM key (16 == AES-128, 32 == AES-256) the keyring
 * pre-runs `aesGcmInit` and caches the expanded schedule alongside the
 * slot. flexEnvelopeOpen looks the cache up via this entry point and
 * skips the per-call expansion when it hits.
 *
 * Semantics:
 * - Returns NULL when `ring` is NULL, `id` is HMAC_KEY_ID_INVALID, the
 * id is not installed, or the installed key length is not a valid
 * AES-GCM length (e.g. 20- / 24- / 64-byte HMAC keys). Callers that
 * receive NULL fall back to ad-hoc `aesGcmInit` on the raw key bytes.
 * - LIFETIME CONTRACT: the returned pointer is BORROWED into the
 * keyring slot. The rwlock is released before return; the same
 * mutation set that invalidates `hmacKeyringLookup` (Retire,
 * Install, SetAeadSalt, Restore, Free) invalidates this pointer.
 * Callers MUST NOT retain it across any of those calls; pair with
 * `hmacKeyringGetGeneration` for multi-thread safety. The schedule
 * is wiped DCE-resistantly on every slot teardown so post-free
 * reads return zero rather than residual key material.
 * - Cheap read under the keyring read lock — safe to call on the hot
 * Open path. */
const struct aesGcmKey *hmacKeyringGetAesGcmKey(hmacKeyring *ring,
                                                hmacKeyId id);

/* Copy the cached AEAD key schedule for `id` into a caller-supplied
 * `aesGcmKey` buffer. Closes the borrowed-pointer lifetime hazard of
 * `hmacKeyringGetAesGcmKey` for any caller that needs the schedule past
 * a keyring-mutating window — the copy is performed under the keyring
 * rwlock so a concurrent Retire cannot race the memcpy.
 *
 * Returns true on success (the schedule was copied into `*out_key`).
 * Returns false if:
 *   - `ring` or `out_key` is NULL
 *   - `id` is HMAC_KEY_ID_INVALID
 *   - `id` is not currently installed
 *   - the slot has no cached AEAD schedule (installed key length is not
 *     a valid AES-GCM length, e.g. 20/24/64-byte HMAC keys)
 *
 * On false return, `*out_key` is left untouched. On true return, the
 * caller now owns a self-contained copy of the round keys — wipe via
 * `dkSecureZero(out_key, sizeof(*out_key))` when done, since the round
 * keys are key-equivalent material. */
bool hmacKeyringGetAesGcmKeyCopy(hmacKeyring *ring, hmacKeyId id,
                                 struct aesGcmKey *out_key);

/* ---- AEAD nonce-counter helpers --------------------------------------
 *
 * For every installed key the keyring tracks a 64-bit monotonically
 * increasing counter and an opaque 4-byte salt. Combined they form a
 * 12-byte ChaCha20-Poly1305 nonce that is guaranteed unique within the
 * lifetime of the in-RAM counter.
 *
 * Nonce wire layout (12 bytes, little-endian counter || 4-byte salt):
 *
 * bytes 0..7 : counter LE (next-issued value)
 * bytes 8..11 : salt (caller-provided at install or via SetAeadSalt)
 *
 * Concurrency: NextNonce uses an atomic CAS bump on the per-slot counter
 * while holding the keyring read lock. Many writers per key are safe;
 * each call gets a distinct counter. Retire is serialised against
 * NextNonce by the rwlock so the slot cannot disappear mid-bump.
 *
 * Persistence: the counter is process-local memory. After process
 * restart the counter starts at zero unless the caller invokes
 * RestoreAeadCounter with a saved high-water mark. Production callers
 * MUST either (a) restore on startup from durable state, OR (b) rotate
 * the key on restart, OR (c) supply a unique per-process salt at
 * install time. Failing any of these reuses a (key, nonce) pair —
 * which destroys both confidentiality and integrity for ChaCha20-
 * Poly1305. The AEAD nonce-reuse detection layer (see aeadNonceLog.h)
 * sits on top of this. */

/* Reserve the next AEAD nonce for `id`. Refuses if:
 * - `id` is not installed
 * - the counter is exhausted (would wrap past UINT64_MAX)
 * - the slot's salt is all-zero AND the slot was not installed via
 * `hmacKeyringInstallUnsalted` (fail-closed against the cross-process
 * nonce collision footgun — see InstallUnsalted docs)
 *
 * On success writes 12 bytes to `out_nonce`. */
bool hmacKeyringNextAeadNonce(hmacKeyring *ring, hmacKeyId id,
                              uint8_t out_nonce[12]);

/* Install a 4-byte salt suffix for the AEAD nonce of `id`. May be
 * called at any time; the next NextAeadNonce call picks up the new
 * salt. If `salt` is non-zero this also clears the slot's "unsalted"
 * flag (a non-zero salt makes the slot safe for nonce issuance even if
 * it was originally installed via InstallUnsalted). A zero salt leaves
 * the unsalted flag untouched — callers must use InstallUnsalted at
 * install time to declare the "no salt" intent. Returns false if `id`
 * is not installed or `salt` is NULL. */
bool hmacKeyringSetAeadSalt(hmacKeyring *ring, hmacKeyId id,
                            const uint8_t salt[HMAC_KEYRING_AEAD_SALT_BYTES]);

/* Peek at the next-to-be-issued counter for `id`. Useful right before
 * a graceful shutdown so the caller can durably save the value.
 * Returns false if `id` is not installed. */
bool hmacKeyringPeekAeadCounter(hmacKeyring *ring, hmacKeyId id,
                                uint64_t *out_counter);

/* Restore the counter for `id` to `value` (i.e. the next NextAeadNonce
 * call will issue `value`, then advance to `value + 1`).
 *
 * Strict forward-only: refuses if `value` is less than the current
 * counter, because going backwards invites nonce reuse. Returns false
 * on backward-move attempts, on UINT64_MAX (exhausted), or if `id` is
 * not installed. */
bool hmacKeyringRestoreAeadCounter(hmacKeyring *ring, hmacKeyId id,
                                   uint64_t value);

/* ---- Snapshot / restore -------------------------------------------------
 *
 * Snapshot serialises every installed slot through datakit's `flex`
 * format. No hand-rolled integer encodings — flex carries its own
 * framing, length prefixes, and typed entries.
 *
 * The first entry is the revoked-key floor (unsigned; 0 = no floor),
 * then five entries per installed slot:
 *
 * 1. id (unsigned, non-zero)
 * 2. algo_tag (unsigned: 0 = SHA-256, 1 = BLAKE3)
 * 3. salt (bytes, exactly 4; all zero for an unsalted slot)
 * 4. aead_counter (unsigned)
 * 5. key (bytes, raw key)
 *
 * Total flex entry count is 1 + 5 * N.  Restore bounds every entry of
 * the buffer with `flexValidateBufferDeep` before reading it, and
 * refuses a buffer that fails or does not have that shape.
 *
 * SECURITY: snapshot bytes carry the raw keys. They MUST be wrapped
 * through flexEnvelopeSeal (AEAD class) before being written anywhere
 * that crosses a trust boundary. This API exposes the raw form by
 * design — composing it with envelope sealing is the caller's choice.
 *
 * For pipeline integration use `persistOpsHmacKeyring` with the
 * `persist` framework — the snapshot bytes flow through the same
 * envelope + WAL + recovery machinery as every other persisted
 * structure (see persist.h). */

/* Bytes a snapshot of the current keyring will consume. */
size_t hmacKeyringSnapshotSize(const hmacKeyring *ring);

/* Serialise `ring` into `out`. Returns false if `out_cap` is too small
 * (use hmacKeyringSnapshotSize). On success writes the byte count to
 * *out_written. */
bool hmacKeyringSnapshot(const hmacKeyring *ring, uint8_t *out, size_t out_cap,
                         size_t *out_written);

/* Reload installed keys from a flex-encoded snapshot buffer. Returns
 * false on malformed flex framing, a field of the wrong type (every
 * integer field must be a 64-bit unsigned value, so a 96/128-bit entry
 * is refused), unknown algo tag, or duplicate id collision with an
 * already-present entry. Successful restore installs
 * every record atomically — a mid-way failure rolls back so the
 * keyring is left exactly as it was. */
bool hmacKeyringRestore(hmacKeyring *ring, const uint8_t *buf, size_t len);

/* persistOps descriptor for plugging hmacKeyring into the `persist`
 * pipeline. Snapshot/restore route through hmacKeyringSnapshot /
 * hmacKeyringRestore above (which use flex); the persist framework
 * wraps every byte through a flexEnvelope under the operator's chosen
 * integrity algorithm. See persist.h for the surrounding contract. */
extern const struct persistOps persistOpsHmacKeyring;

#ifdef DATAKIT_TEST
int hmacKeyringTest(int argc, char *argv[]);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif
