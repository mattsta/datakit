#pragma once

/* clusterMailbox — verified-frame router for cluster replication and
 * apply paths.
 *
 * Producers wrap each outgoing operation with `clusterMailboxSeal`,
 * which encodes `(origin_id, sequence, msg_type, payload)` and
 * envelope-wraps the result using a key drawn from an hmacKeyring.
 *
 * Consumers feed received bytes into `clusterMailboxIngest`, which:
 * 1. Looks up the integrity key in the keyring by id.
 * 2. Verifies the envelope (HMAC class — tamper-evident).
 * 3. Decodes the inner frame.
 * 4. Drops replays (sequence <= last_applied_seq for this origin).
 * 5. Invokes the caller-supplied apply callback with the verified
 * frame.
 *
 * Design properties:
 * - Self-managing memory: the mailbox owns its origin table. Free
 * with `clusterMailboxFree`.
 * - Ingest is allocation-free: it verifies the envelope in place
 * and dispatches without heap traffic.
 * - Seal pays one tiny heap allocation for the flex-encoded inner
 * header , then stage-stack-or-heap for the
 * [header][payload] concatenation: stack for small payloads
 * (<= ~1 KiB) and heap for larger ones.
 *
 * Threading contract:
 * - Single-writer-multi-reader for the steady-state hot path:
 * * EXACTLY ONE thread (the I/O thread that owns the mailbox)
 * performs the mutating operations Seal and Ingest.
 * * Any number of OBSERVER threads (a metrics scraper, an admin
 * introspection endpoint, an alerting daemon) may call the
 * stats accessors -- clusterMailboxLastApplied,
 * clusterMailboxOriginCount, and clusterMailboxGetStats --
 * concurrently with the writer's Ingest/Seal. Reads are
 * consistent per-counter and per-slot (no torn 64-bit value)
 * and observe a non-decreasing watermark; the writer publishes
 * each new last_applied with a release store and the reader
 * loads with an acquire that pairs with the publish. Readers
 * may briefly observe a lagging last_applied for an in-flight
 * Ingest -- that is the normal "snapshot is per-counter, not
 * cross-counter" semantic also documented on
 * clusterMailboxGetStats.
 * - The lifecycle and bulk-mutation operations (Create, Free,
 * Restore, RestoreWrapped, Snapshot, SnapshotWrapped, ResetStats)
 * are still single-threaded. Caller MUST serialise them against
 * each other AND against the writer's Ingest/Seal -- e.g. quiesce
 * the I/O thread before issuing a Restore, or hold a caller-owned
 * write-lock for the duration. Observer threads may continue to
 * run during Snapshot (it only reads), but Restore atomically
 * mutates the watermark table and observers will see the merged
 * state once Restore returns.
 * - The typical deployment binds one mailbox per follower
 * connection (or per replication shard), processed by a
 * dedicated I/O thread. Observability and replay watermarks are
 * exposed safely to any monitoring thread without locks.
 * - If a future use case needs MULTIPLE concurrent WRITERS, wrap
 * the mailbox in a caller-owned pthread_mutex around every Seal
 * and Ingest call. The bound keyring (hmacKeyring) IS thread-safe
 * via its own internal rwlock.
 * - Multi-origin: a small open-addressed table tracks per-origin
 * replay watermarks. Capacity is a compile-time constant.
 * - Replay-evident, not ordering-fixing: V1 rejects out-of-order
 * frames whose sequence is <= last_applied. Higher-sequence
 * frames are dispatched immediately; the caller deals with gaps
 * according to its protocol (e.g. NACK + retransmit). */

#ifdef __cplusplus
extern "C" {
#endif

#include "flexEnvelope.h"
#include "hmacKeyring.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward decl — full type lives in aeadNonceLog.h. The mailbox owns
 * (or borrows) a nonceLog so AEAD-class Seal/Open paths called via the
 * mailbox's params can detect (key_id, nonce) reuse end-to-end. For the
 * keyed-MAC algos the mailbox currently pins (HMAC_SHA256, HMAC_BLAKE3,
 * BLAKE3_KEYED), the nonceLog is plumbed through to params but is a
 * no-op on the wire path — keyed-MAC has no nonce. The wiring exists
 * so that any future AEAD-class mailbox variant inherits the defense
 * automatically. */
struct aeadNonceLog;

/* Verified frame handed to the apply callback. Pointers are valid
 * only for the duration of the callback invocation. */
typedef struct clusterFrame {
    const uint8_t *payload; /* verified opaque bytes */
    size_t payload_len;
    uint64_t sequence;  /* monotonically increasing per origin */
    uint32_t origin_id; /* sender identifier (caller-defined) */
    uint8_t msg_type;   /* application-defined opcode */
    uint8_t _pad[3];
} clusterFrame;

/* Returns true if the frame was applied successfully. If the
 * callback returns false, the mailbox treats the frame as rejected
 * (its origin's last_applied_seq is NOT advanced) so a future retry
 * can re-deliver it. */
typedef bool (*clusterApplyFn)(const clusterFrame *frame, void *ctx);

typedef struct clusterMailbox clusterMailbox;

/* Construct a mailbox. `ring` is borrowed — caller is responsible
 * for keeping it alive at least until clusterMailboxFree. `on_apply`
 * and `apply_ctx` may be NULL only if the caller never calls Ingest.
 *
 * `envelope_algo` chooses the integrity algorithm for outgoing wraps
 * (Seal) and is enforced on incoming verifies (Ingest rejects any
 * envelope using a different algorithm — pins the protocol). Must
 * be a keyed-class algorithm (HMAC_SHA256, HMAC_BLAKE3, or
 * BLAKE3_KEYED). Returns NULL on bad arguments or allocation
 * failure. */
clusterMailbox *clusterMailboxCreate(hmacKeyring *ring,
                                     flexEnvelopeAlgo envelope_algo,
                                     clusterApplyFn on_apply, void *apply_ctx);

/* Feature flags for clusterMailboxCreateEx. None of these change the
 * wire format — they only adjust the consumer-side replay policy and
 * observability behaviour. */
typedef enum clusterMailboxFlag {
    CLUSTER_MAILBOX_FLAG_NONE = 0,
    /* Enable a 64-bit per-origin replay window. Without this flag the
     * consumer is strict-monotonic: any sequence <= last_applied is
     * rejected as REPLAY. With the flag, sequences in
     * (last_applied - 63, last_applied] are accepted if their slot in
     * the per-origin bitmap is still clear; older or duplicate
     * sequences are rejected as REPLAY. Use this when the underlying
     * transport reorders frames — set, then Ingest will tolerate the
     * common "frame 100 arrived before frame 99" case without losing
     * the legitimate frame 99. Memory cost: 8 bytes per origin
     * (64 bits) so for the default 64-origin table the total cost is
     * 512 bytes whether the flag is on or off. */
    CLUSTER_MAILBOX_FLAG_REPLAY_WINDOW = (1u << 0)
} clusterMailboxFlag;

/* Extended constructor — same semantics as clusterMailboxCreate plus a
 * `flags` argument for opt-in features (see clusterMailboxFlag).
 * `clusterMailboxCreate(ring, algo, fn, ctx)` is exactly equivalent to
 * `clusterMailboxCreateEx(ring, algo, fn, ctx, CLUSTER_MAILBOX_FLAG_NONE)`. */
clusterMailbox *clusterMailboxCreateEx(hmacKeyring *ring,
                                       flexEnvelopeAlgo envelope_algo,
                                       clusterApplyFn on_apply, void *apply_ctx,
                                       uint32_t flags);

/* Default capacity for a mailbox-owned aeadNonceLog when the caller
 * does not specify one in clusterMailboxCreateOptions. Sized for the
 * common cluster control-message rate without wasting memory: 4096
 * entries cover ~30s of replication traffic at the typical 100/s/peer. */
#define CLUSTER_MAILBOX_DEFAULT_NONCE_LOG_CAP ((size_t)4096)

/* Options struct for clusterMailboxCreateOpts. Zero-initialise to get
 * the documented defaults; only set the fields you want to override. */
typedef struct clusterMailboxCreateOptions {
    /* REQUIRED: borrowed keyring (caller keeps alive past Free). */
    hmacKeyring *ring;
    /* REQUIRED: keyed-class envelope algo (HMAC_SHA256, HMAC_BLAKE3, or
     * BLAKE3_KEYED). AEAD-class algos are rejected at Create time. */
    flexEnvelopeAlgo envelope_algo;
    /* OPTIONAL: apply callback. NULL only if the caller never Ingests. */
    clusterApplyFn on_apply;
    /* OPTIONAL: opaque context passed to on_apply. */
    void *apply_ctx;
    /* Bitmask of clusterMailboxFlag values. */
    uint32_t flags;
    /* When true (default false), the constructor REFUSES to create a
     * nonceLog and the mailbox runs without (key_id, nonce) reuse
     * detection. Use only in tests or single-process scenarios that
     * have no AEAD-class concerns. Ignored when `nonce_log` is set
     * explicitly. */
    bool disable_nonce_log;
    uint8_t _pad[3];
    /* OPTIONAL: caller-provided nonceLog. When non-NULL the mailbox
     * BORROWS it (caller frees) and the `nonce_log_capacity` field is
     * ignored. When NULL and `disable_nonce_log == false`, the
     * constructor allocates a fresh log of capacity
     * `nonce_log_capacity` (rounded up to next power of two, floor 64)
     * and OWNS it — clusterMailboxFree releases it. */
    struct aeadNonceLog *nonce_log;
    /* Capacity hint for a default-allocated nonceLog. Zero means use
     * CLUSTER_MAILBOX_DEFAULT_NONCE_LOG_CAP. Ignored when `nonce_log`
     * is non-NULL or `disable_nonce_log == true`. */
    size_t nonce_log_capacity;
} clusterMailboxCreateOptions;

/* Full-options constructor. All other Create variants funnel through
 * this one. Returns NULL on bad arguments (NULL ring, non-keyed or
 * AEAD-class envelope_algo, unknown flag bits, or nonce-log allocation
 * failure when one is requested implicitly). */
clusterMailbox *
clusterMailboxCreateOpts(const clusterMailboxCreateOptions *opts);

/* Tear down the mailbox. Does not touch the underlying keyring.
 * Safe to call with NULL. */
void clusterMailboxFree(clusterMailbox *mb);

/* Attach a keyring + algo + apply callback to a mailbox that was
 * constructed without them — specifically, a mailbox produced by the
 * persist framework's restore path (persistOpsClusterMailbox.restore
 * or .newEmpty). Such a mailbox carries recovered watermarks but
 * cannot perform live Seal/Ingest until a keyring is wired up.
 *
 * Validation mirrors clusterMailboxCreateEx: rejects NULL mb / NULL
 * ring, rejects non-keyed or AEAD-class envelope_algo (keyed-class
 * MAC algorithms only), and leaves the watermark table untouched.
 *
 * Returns false on bad arguments (mailbox state unchanged); returns
 * true once the keyring, algo, and callback are bound. Calling this
 * on a mailbox constructed via clusterMailboxCreate{,Ex} overwrites
 * the previously-set ring/algo/callback — typically used to swap a
 * provisional keyring at startup after the durable watermarks have
 * been restored.
 *
 * Threading: the caller MUST serialise this against any concurrent
 * Seal/Ingest on the same mailbox (it mutates fields the I/O thread
 * reads on every call). Stats accessors and Snapshot/Restore are
 * unaffected and may run concurrently. */
bool clusterMailboxAttachKeyring(clusterMailbox *mb, hmacKeyring *ring,
                                 flexEnvelopeAlgo envelope_algo,
                                 clusterApplyFn on_apply, void *apply_ctx);

/* Borrow the bound aeadNonceLog (may be NULL if the mailbox was
 * created with `disable_nonce_log = true`). Caller MUST NOT free —
 * the mailbox manages ownership. Useful for tests / observability /
 * keyring rotate hooks that need to call aeadNonceLogClearKey on the
 * mailbox's log when retiring a key id. */
struct aeadNonceLog *clusterMailboxNonceLog(const clusterMailbox *mb);

/* Number of origins currently tracked (have at least one applied
 * frame). Useful for tests and observability.
 *
 * Thread-safety: SAFE to call concurrently with the writer thread's
 * Ingest/Seal (atomic-acquire load). Returns a value that is consistent
 * with the writer's most recent committed cmInsertOrigin — never a
 * value higher than the number of slots whose origin_id is already
 * visible to a concurrent clusterMailboxLastApplied probe. */
size_t clusterMailboxOriginCount(const clusterMailbox *mb);

/* Most recently applied sequence for `origin_id`, or 0 if never seen.
 * Note: an actual sequence of 0 is indistinguishable from "never seen"
 * — producers should start at sequence 1.
 *
 * Thread-safety: SAFE to call concurrently with the writer thread's
 * Ingest (atomic-acquire load). The returned value is one of:
 * - 0 (origin not yet seen, OR origin slot was just inserted but its
 * first sequence hasn't been applied yet — equivalent observable),
 * - some sequence the writer has committed (release-stored) to the
 * slot.
 * The value is monotonic non-decreasing across consecutive reads in
 * the absence of a single-threaded Restore. Caller must serialise
 * Restore/RestoreWrapped against this read if the policy requires
 * cross-restore monotonicity. */
uint64_t clusterMailboxLastApplied(const clusterMailbox *mb,
                                   uint32_t origin_id);

/* Maximum bytes a sealed frame will consume for the given
 * payload_len. Caller pre-allocates `out` to at least this size. */
size_t clusterMailboxSealedSize(const clusterMailbox *mb, size_t payload_len);

/* Producer: wrap `(origin_id, sequence, msg_type, payload)` into an
 * envelope using the key indexed by `key_id` in the bound keyring.
 *
 * Returns false on:
 * - mailbox or out is NULL
 * - key_id is invalid or absent from the keyring
 * - out_cap is too small (use clusterMailboxSealedSize to size)
 * - sequence is 0 (reserved as "never seen" sentinel)
 *
 * On success writes the byte count to *out_written. */
bool clusterMailboxSeal(clusterMailbox *mb, uint32_t origin_id,
                        uint64_t sequence, uint8_t msg_type,
                        const uint8_t *payload, size_t payload_len,
                        hmacKeyId key_id, uint8_t *out, size_t out_cap,
                        size_t *out_written);

/* Result codes for Ingest — distinguishes between protocol-level
 * rejections (replay, gap, bad-frame) and crypto-level failures
 * (tag mismatch, key missing). Callers typically log all non-OK
 * outcomes for security monitoring. */
typedef enum clusterMailboxIngestResult {
    CLUSTER_MAILBOX_OK = 0,
    CLUSTER_MAILBOX_ERR_PARAM = 1,             /* NULL pointer or bad arg */
    CLUSTER_MAILBOX_ERR_KEY_MISSING = 2,       /* key_id not in keyring */
    CLUSTER_MAILBOX_ERR_VERIFY_FAILED = 3,     /* envelope verify rejected */
    CLUSTER_MAILBOX_ERR_BAD_FRAME = 4,         /* inner header malformed */
    CLUSTER_MAILBOX_ERR_ALGO_MISMATCH = 5,     /* envelope uses wrong algo */
    CLUSTER_MAILBOX_ERR_REPLAY = 6,            /* sequence <= last applied */
    CLUSTER_MAILBOX_ERR_APPLY_REJECTED = 7,    /* on_apply returned false */
    CLUSTER_MAILBOX_ERR_ORIGIN_TABLE_FULL = 8, /* too many distinct origins */
} clusterMailboxIngestResult;

/* Consumer: verify and dispatch a single envelope. The caller-
 * supplied `key_id` is used to resolve a prepared HMAC key from the
 * mailbox's keyring; in transports that already key by id (e.g. the
 * envelope's outer header carries it), the caller hands the same id
 * here. Pinning the algo at create time prevents downgrade attacks. */
clusterMailboxIngestResult clusterMailboxIngest(clusterMailbox *mb,
                                                const uint8_t *bytes,
                                                size_t len, hmacKeyId key_id);

/* === Observability =================================================
 *
 * Per-mailbox event counters. Snapshot via clusterMailboxStats and
 * read; counters never wrap in practice (uint64_t covers ~580 years
 * at 1 GHz event rate). The struct is wire-stable: new counters MUST
 * be appended (never inserted) so callers compiled against an older
 * header remain compatible.
 *
 * Suggested operator monitoring (rules of thumb):
 * - verify_failed > 0 over short windows: alert (active attacker
 * or wire corruption).
 * - algo_mismatch > 0: alert (downgrade attempt or misconfigured
 * peer).
 * - replay_rejected sustained spike: alert (replay attack OR upstream
 * reorder issue — investigate before silencing).
 * - apply_rejected high relative to apply_ok: investigate application
 * logic; producers will retransmit indefinitely.
 * - table_full > 0: capacity exhausted; either grow CAP or expect
 * missed frames from new origins.
 * - key_missing > 0: rotation desync; new producer key not yet
 * installed on this consumer. */
typedef struct clusterMailboxStats {
    uint64_t ingest_total;      /* every Ingest call, success or not */
    uint64_t apply_ok;          /* OK results (frame applied) */
    uint64_t apply_rejected;    /* on_apply returned false */
    uint64_t replay_rejected;   /* ERR_REPLAY */
    uint64_t algo_mismatch;     /* ERR_ALGO_MISMATCH */
    uint64_t verify_failed;     /* ERR_VERIFY_FAILED */
    uint64_t key_missing;       /* ERR_KEY_MISSING */
    uint64_t bad_frame;         /* ERR_BAD_FRAME */
    uint64_t origin_table_full; /* ERR_ORIGIN_TABLE_FULL */
    uint64_t param_error;       /* ERR_PARAM */
    uint64_t seal_total;        /* successful Seal calls */
    uint64_t seal_param_error;  /* failed Seal calls (bad args) */
    uint64_t bytes_ingested;    /* sum of `len` over Ingest calls */
    uint64_t bytes_sealed;      /* sum of `out_written` over Seal OK */
} clusterMailboxStats;

/* Atomically snapshot counters into `out`. Safe to call concurrently
 * with Ingest/Seal — readers may observe individual counters lagging
 * slightly but never see a torn 64-bit value (counters are _Atomic). */
void clusterMailboxGetStats(const clusterMailbox *mb, clusterMailboxStats *out);

/* Zero every counter. Useful for window-based monitoring (call at
 * each minute/hour boundary and diff). */
void clusterMailboxResetStats(clusterMailbox *mb);

/* === Key rotation choreography ======================================
 *
 * Rotation is inherently multi-party: producers must continue sealing
 * under the old key for long enough that every consumer has installed
 * the new key, then switch. Consumers must accept BOTH old and new
 * during the overlap window, then retire the old.
 *
 * `clusterRotationState` is a tiny piece of policy state that callers
 * can hold alongside their hmacKeyring + clusterMailbox to track the
 * (primary, previous) pair. It does NOT mutate the keyring or the
 * mailbox — those remain self-managing. It just answers two questions:
 *
 * - "Which key_id should I Seal under right now?" (primary)
 * - "Should I accept Ingest under this key_id?" (Accepts)
 *
 * Choreography pattern (caller-driven):
 *
 * 1. Producer + consumers all hold a clusterRotationState init'd with
 * key id K1.
 * 2. Operator distributes K2 raw bytes out-of-band.
 * 3. Each node installs K2 in its keyring (hmacKeyringInstall).
 * 4. Producer Seal's a CLUSTER_MAILBOX_MSG_KEY_ROTATE_ANNOUNCE frame
 * under K1 carrying the announcement payload built via
 * clusterMailboxBuildRotateAnnounce(K2).
 * 5. Consumers decode the announcement and call
 * clusterRotationPromote(state, K2). They now Accept both ids.
 * 6. Producer also Promotes its own state, then starts Sealing
 * under K2.
 * 7. Once every consumer has applied at least one K2-sealed frame,
 * operator broadcasts "retire K1" — every node calls
 * clusterRotationRetirePrevious + hmacKeyringRetire(K1).
 *
 * The state struct never reads outside its own fields — operator/
 * application policy decides when to advance steps. */

/* Reserved msg_type opcode for the rotation announcement frame. Picked
 * at the top of the 8-bit range so the typical [0..0x7F] application
 * range stays untouched. */
#define CLUSTER_MAILBOX_MSG_KEY_ROTATE_ANNOUNCE ((uint8_t)0xFE)

/* Worst-case payload length for the announcement, sized to fit the
 * 1-entry flex of an unsigned u32 id. Worst-case flex bytes: 5B
 * total-bytes varint + 1B count + 9B u64 entry (varint-tagged
 * unsigned) + 1B tail marker = 16B. Realistic encodings for sane key
 * id ranges consume 13-15B. Sized to 16 to leave callers' buffers a
 * stable contract that fits every value. The actual encoded length is
 * returned by clusterMailboxBuildRotateAnnounce — callers MUST treat
 * the returned size as authoritative and not assume the buffer is fully
 * used. */
#define CLUSTER_MAILBOX_ROTATE_ANNOUNCE_PAYLOAD_LEN ((size_t)16)

typedef struct clusterRotationState {
    hmacKeyId primary;  /* used by Seal */
    hmacKeyId previous; /* still accepted by Ingest; INVALID = none */
} clusterRotationState;

/* Initialise: primary = `initial`, previous = INVALID. Refuses
 * INVALID as initial. */
bool clusterRotationInit(clusterRotationState *st, hmacKeyId initial);

/* Promote `new_primary` to primary; demote the current primary into
 * the `previous` slot. Refuses if new_primary equals current primary
 * (a no-op rotation hides a bug), is INVALID, or `st` is NULL.
 *
 * After promote: Accepts returns true for both new_primary AND the
 * old primary, until RetirePrevious is called. */
bool clusterRotationPromote(clusterRotationState *st, hmacKeyId new_primary);

/* Clear the `previous` slot. Call only after every peer has applied
 * at least one frame under the new primary. After this returns,
 * Accepts will return false for the previous id. */
void clusterRotationRetirePrevious(clusterRotationState *st);

/* Is `id` currently accepted for Ingest? */
bool clusterRotationAccepts(const clusterRotationState *st, hmacKeyId id);

/* Build a rotation-announcement payload (1-entry
 * flex containing the new_id as an unsigned value) into `out`.
 *
 * `out` is sized to CLUSTER_MAILBOX_ROTATE_ANNOUNCE_PAYLOAD_LEN bytes
 * (the worst-case encoding size). Returns the ACTUAL byte count the
 * flex consumed (typically 13-15 B for realistic key ids; <= 16 B
 * worst case) or 0 on bad args. Callers MUST treat the return value
 * as authoritative for the encoded length — the buffer is not
 * necessarily fully used. */
size_t clusterMailboxBuildRotateAnnounce(
    uint8_t out[CLUSTER_MAILBOX_ROTATE_ANNOUNCE_PAYLOAD_LEN], hmacKeyId new_id);

/* Parse a rotation-announcement payload back into a key id. Returns
 * false on malformed flex framing, wrong entry count, non-unsigned
 * type, out-of-range value, or zero-id (zero is reserved as
 * HMAC_KEY_ID_INVALID).  Validates via flexValidateBufferDeep + flexCount
 * == 1 + strict type check on the single entry — the deep validator
 * walks every entry's length-encoding bytes so this routine is safe
 * under arbitrary input (no upstream envelope-MAC pre-gate required). */
bool clusterMailboxParseRotateAnnounce(const uint8_t *payload, size_t len,
                                       hmacKeyId *out_new_id);

/* === Inner-frame header codec (test-exposed) ========================
 *
 * The inner frame is a 3-entry flex of unsigned values:
 *   origin_id (u32) | sequence (u64) | msg_type (u8)
 *
 * These two functions expose the encoder/decoder used internally by
 * Seal/Ingest so unit tests can exercise the framing in isolation
 * (round-trip + tamper-every-byte + truncation + wrong-count +
 * out-of-range). They are also useful for any future cluster code that
 * needs to compose or inspect a pre-framed inner header without going
 * through the full envelope path. */

/* Worst-case bytes the inner-header flex can occupy. Caller-supplied
 * buffer to cmEncodeInnerHeader MUST be at least this large. */
#define CLUSTER_MAILBOX_INNER_HEADER_MAX_BYTES ((size_t)32)

/* Encode an inner-frame header into `out` (cap >=
 * CLUSTER_MAILBOX_INNER_HEADER_MAX_BYTES). Returns the encoded byte length on
 * success or 0 on bad args / insufficient capacity / allocation failure. */
size_t cmEncodeInnerHeader(uint8_t *out, size_t out_cap, uint32_t origin_id,
                           uint64_t sequence, uint8_t msg_type);

/* Decode an inner-frame header from `src` (length `src_len`, which may
 * include trailing opaque payload bytes after the flex prefix).
 *
 * On success returns true; sets `*origin_id`, `*sequence`, `*msg_type`
 * to the decoded values and `*hdr_len` to the byte length the flex
 * prefix consumed (caller computes payload region as `src + *hdr_len`,
 * `src_len - *hdr_len`).
 *
 * Returns false on any of: short buffer, malformed flex framing, wrong
 * entry count, non-unsigned entry types, or any value outside its
 * representable range (origin_id > UINT32_MAX, msg_type > UINT8_MAX).
 *
 * Safe under arbitrary input: the framing check uses
 * flexValidateBufferDeep, which performs an O(N) walk of every entry's
 * length-encoding bytes before any iterator runs.  A synthetic byte-
 * tamper that survives envelope verification (computationally infeasible
 * in production, trivial inside a unit test) cannot induce flexNext /
 * flexGetByTypeCopy to overread the buffer here. */
bool cmDecodeInnerHeader(const uint8_t *src, size_t src_len,
                         uint32_t *origin_id, uint64_t *sequence,
                         uint8_t *msg_type, size_t *hdr_len);

/* === Durable watermarks (snapshot / restore) ========================
 *
 * Replay protection is only useful if the watermark table survives
 * restart — otherwise an attacker who knocks a node offline can replay
 * messages from before the crash. Snapshot writes the table through
 * datakit's `flex` format; Restore reads it back into a fresh (or
 * non-fresh) mailbox.
 *
 * Wire layout: 3 flex entries per persisted origin slot, in order:
 *
 * 1. origin_id (unsigned)
 * 2. last_applied (unsigned, > 0 — slots with last_applied == 0
 * are not persisted)
 * 3. window_bits (unsigned, 64-bit replay bitmap; 0 if window
 * feature is disabled or no in-window state)
 *
 * Total flex entry count is always 3 * N persisted slots.  Restore
 * validates the buffer via flexValidateBufferDeep (O(N) per-entry
 * bound check) before walking it, so a corrupted on-disk record cannot
 * over-read.
 *
 * Each (last_applied, window_bits) pair is treated as an atomic unit
 * — Restore re-orients bits into the merged frame before OR-ing so
 * mid-life restores can't silently drop bits even when the in-memory
 * mailbox has already advanced past the snapshot. See the Restore
 * docstring + cmReorientWindowBits commentary for the policy.
 *
 * The bare bytes do not carry an integrity tag — callers wrap them
 * through flexEnvelope (HMAC or AEAD class) before durable storage,
 * either directly via clusterMailboxSnapshotWrapped or by routing
 * through the persist framework (see persistOpsClusterMailbox). */

/* Bytes a snapshot of the current watermark table will consume. */
size_t clusterMailboxSnapshotSize(const clusterMailbox *mb);

/* Serialize `mb`'s watermark table into `out`. `out_cap` must be at
 * least clusterMailboxSnapshotSize(mb). Returns false on bad args or
 * short buffer. On success writes the byte count to *out_written. */
bool clusterMailboxSnapshot(const clusterMailbox *mb, uint8_t *out,
                            size_t out_cap, size_t *out_written);

/* Restore the watermark table from `buf`. Replaces any existing
 * watermarks atomically (call before first Ingest on a fresh mailbox
 * for the cleanest semantic, but also safe to call mid-life — it
 * unions the loaded entries with any already-present ones, keeping
 * the MAX of last_applied per origin).
 *
 * Window-bit merge policy (mid-life restore): when both the snapshot
 * and the in-memory mailbox carry (last_applied, window_bits) for the
 * same origin, the bitmap is re-oriented into the higher
 * last_applied's coordinate frame (`window_bits[i]` represents
 * `last_applied - i`) and the two are OR'd. Bits whose absolute
 * sequence falls outside the 64-wide merged window are dropped (they
 * would always be ancient-replay-rejected by Ingest, so no
 * information is lost). This preserves the "snapshot's record of
 * already-applied sequences cannot be silently forgotten" invariant
 * — without it, a stale snapshot would let an attacker re-deliver
 * sequences the snapshot had marked as seen. See the
 * cmReorientWindowBits doc-comment in clusterMailbox.c for rationale.
 *
 * Returns false on malformed input. */
bool clusterMailboxRestore(clusterMailbox *mb, const uint8_t *buf, size_t len);

/* --- Envelope-wrapped snapshot/restore convenience helpers --------
 *
 * The plain Snapshot/Restore above produces/consumes raw watermark
 * bytes with no integrity tag. The Wrapped variants compose the
 * raw bytes inside a flexEnvelope so the on-disk (or on-wire)
 * representation is tamper-evident under the caller's chosen
 * keyed algorithm.
 *
 * Use these when the snapshot will cross a trust boundary — e.g.
 * stored on a disk that another process can write, or shipped to a
 * cold-standby peer over an untrusted channel. When the snapshot
 * lives entirely inside one trusted process, the plain variants are
 * sufficient. */

/* Exact byte count clusterMailboxSnapshotWrapped will produce, given
 * the current origin table and the requested envelope algorithm. */
size_t clusterMailboxSnapshotWrappedSize(const clusterMailbox *mb,
                                         flexEnvelopeAlgo envelope_algo);

/* Take a snapshot of `mb`'s watermarks and flexEnvelope-wrap it under
 * `params`. The algo MUST be keyed-class (Wrap rejects non-keyed
 * algos for snapshots — checksum-only would leak the integrity gap).
 * On success `*out_written` equals
 * clusterMailboxSnapshotWrappedSize(mb, params->algo). */
bool clusterMailboxSnapshotWrapped(const clusterMailbox *mb,
                                   const flexEnvelopeWrapParams *params,
                                   uint8_t *out, size_t out_cap,
                                   size_t *out_written);

/* Verify the envelope, decode the inner snapshot, MAX-merge it into
 * `mb`'s watermark table. On verify failure the mailbox is left
 * untouched. Same semantics as plain Restore otherwise: monotonic
 * merge per origin. */
bool clusterMailboxRestoreWrapped(clusterMailbox *mb,
                                  const flexEnvelopeVerifyParams *params,
                                  const uint8_t *buf, size_t len);

/* persistOps descriptor for plugging clusterMailbox watermarks into
 * the `persist` pipeline. Snapshot/restore route through
 * clusterMailboxSnapshot / clusterMailboxRestore above (which use
 * flex); the persist framework wraps every byte through a
 * flexEnvelope under the operator's chosen integrity algorithm.
 *
 * Restore semantics : `.restore` allocates a fresh mailbox in a
 * "restore shell" state (no bound keyring/algo/apply callback) and
 * populates it from the bytes. Caller takes ownership and frees
 * via the descriptor's `.free` hook (clusterMailboxFree). The
 * returned mailbox is usable immediately for watermark inspection
 * (clusterMailboxLastApplied, clusterMailboxOriginCount,
 * clusterMailboxGetStats) and for taking further snapshots, but is
 * NOT yet wired for live Seal/Ingest — callers MUST attach a
 * keyring + algo + apply callback via clusterMailboxAttachKeyring
 * before any Seal or Ingest call. This split mirrors the lifecycle
 * the persist framework expects: durable state is restored first,
 * then runtime configuration (keyring binding, apply trampoline) is
 * attached separately.
 *
 * `.newEmpty` mirrors `.restore` and returns the same restore-shell
 * shape (no keyring) for the persist framework's WAL-replay path.
 * Watermark mutations are snapshot/restore only — there are no
 * encodeOp/applyOp deltas — so newEmpty is rarely consulted in
 * practice, but is provided for symmetry with the rest of the
 * persist descriptor contract. */
extern const struct persistOps persistOpsClusterMailbox;

#ifdef DATAKIT_TEST
int clusterMailboxTest(int argc, char *argv[]);
int dkTestMailboxWindowOrientationTest(int argc, char *argv[]);
int dkTestMailboxStatsRaceTest(int argc, char *argv[]);
int dkTestMailboxStatsAlignmentTest(int argc, char *argv[]);
int dkTestKeyringGetRawCopyMigrationClusterTest(int argc, char *argv[]);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif