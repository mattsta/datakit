#pragma once

/* clusterReplicator — wires any persistOps-bearing datakit data
 * structure through the security stack (envelope + keyring + mailbox)
 * for cross-host shipping.
 *
 * The pattern:
 *
 * Producer:
 * structure ─► persistOps.snapshot ─► clusterMailboxSeal ─► bytes
 * (envelope-
 * protected)
 *
 * Consumer:
 * bytes ─► clusterMailboxIngest ─► frame.msg_type=persistType
 * ─► persistOps.restore
 * ─► apply callback (caller's hook)
 *
 * The replicator owns NO state of its own — it is a thin glue that
 * composes the existing self-managing modules. Construction binds:
 * - a clusterMailbox (which itself holds the keyring + algo)
 * - a type-table mapping `persistType` -> `const persistOps *`
 * - a caller-supplied apply callback that receives the restored
 * structure plus the frame's (origin_id, sequence)
 *
 * The frame's msg_type byte carries the persistType. PERSIST_TYPE_MAX
 * is 255, fitting in 8 bits. Frame payload = the structure format's
 * dkFormat prefix (persistOpsFormat of the descriptor), then the
 * persistOps.snapshot bytes: a consumer restores only a format version
 * it reads, so nodes on different releases never misread each other's
 * structures (refusals: clusterReplicatorFormatRefusedCount). */

#ifdef __cplusplus
extern "C" {
#endif

#include "clusterMailbox.h"
#include "persist.h"

#include <stddef.h>
#include <stdint.h>

/* @handle: fixed: its address holds from creation to free */
typedef struct clusterReplicator clusterReplicator;

/* Callback fired for every successfully restored structure on the
 * consumer side. Receives ownership of `structure` (a fresh
 * allocation produced by `ops->restore`); the callback MUST eventually
 * call `ops->free(structure)` or hand it to an owning collection.
 *
 * Returns true if the application accepted the structure — the
 * mailbox advances its watermark. Returning false leaves the
 * watermark in place so the producer's retry can re-deliver (and the
 * replicator immediately ops->free's the rejected structure). */
typedef bool (*clusterReplicatorApplyFn)(uint32_t origin_id, uint64_t sequence,
                                         const persistOps *ops, void *structure,
                                         void *ctx);

/* Default retry budget — see clusterReplicatorConfig::max_apply_retries. */
#define CLUSTER_REPLICATOR_DEFAULT_MAX_APPLY_RETRIES 16u

/* Default capacity for a replicator-owned aeadNonceLog. Matches the
 * mailbox default so a replicator + mailbox pair have consistent sizing. */
#define CLUSTER_REPLICATOR_DEFAULT_NONCE_LOG_CAP ((size_t)4096)

/* Replicator behavior knobs. Zero-initialising the struct yields the
 * documented defaults (see field comments). */
typedef struct clusterReplicatorConfig {
    /* After this many CONSECUTIVE APPLY_REJECTED returns for the SAME
     * (origin_id, sequence) pair, the replicator drops the frame:
     * 1. ops->frees the restored structure (so no leak),
     * 2. increments `dropped_count` (visible via
     * clusterReplicatorDroppedCount),
     * 3. returns true from the mailbox trampoline so the mailbox
     * advances its watermark past the doomed sequence — that frees
     * the producer from infinite retransmission of a frame the
     * consumer will never accept.
     *
     * `0` means "infinite retries" (legacy behaviour: keep rejecting,
     * watermark never advances past the bad sequence). This preserves
     * the prior contract for callers that have not yet been audited
     * for poison-frame handling. New callers SHOULD set a finite cap;
     * default (when zero-init'd via the explicit-defaults
     * constructor) is CLUSTER_REPLICATOR_DEFAULT_MAX_APPLY_RETRIES. */
    uint32_t max_apply_retries;
    /* when true (default false), the underlying mailbox is created
     * WITHOUT an aeadNonceLog. Use only in tests or single-process
     * scenarios that don't need (key_id, nonce) reuse detection. The
     * mailbox-level field clusterMailboxCreateOptions::disable_nonce_log
     * is what this knob ultimately sets. */
    bool disable_nonce_log;
    uint8_t _pad[3];
    /* capacity hint for the mailbox-owned aeadNonceLog. Zero means
     * use CLUSTER_REPLICATOR_DEFAULT_NONCE_LOG_CAP. Ignored when
     * `disable_nonce_log == true`. */
    size_t nonce_log_capacity;
} clusterReplicatorConfig;

/* Construct. Creates and OWNS a clusterMailbox internally — the
 * mailbox's apply hook is wired to the replicator's persistOps
 * trampoline. `ring` is borrowed (caller keeps it alive past Free).
 * `type_table` is indexed by `persistType` value; entries may be NULL
 * for unsupported types (received frames carrying NULL entries are
 * rejected as APPLY_REJECTED). `type_table_len` is at least
 * PERSIST_TYPE_MAX + 1 (typically 256). `mailbox_flags` is forwarded
 * to clusterMailboxCreateEx (e.g. CLUSTER_MAILBOX_FLAG_REPLAY_WINDOW).
 *
 * REQUIRES: every non-NULL entry in `type_table` MUST have a non-NULL
 * `free` callback. The trampoline restores into a fresh allocation on
 * every Ingest and MUST be able to free it on the apply-reject path —
 * without `ops->free` the rejected structure leaks. Create returns
 * NULL (and does not allocate the mailbox) if this contract is
 * violated.
 *
 * Backwards-compat: this constructor uses `max_apply_retries = 0`
 * (infinite retries) to preserve prior behaviour. Use
 * clusterReplicatorCreateEx to opt in to drop-after-N-retries. */
clusterReplicator *clusterReplicatorCreate(
    hmacKeyring *ring, flexEnvelopeAlgo envelope_algo, uint32_t mailbox_flags,
    const persistOps *const *type_table, size_t type_table_len,
    clusterReplicatorApplyFn on_restored, void *apply_ctx);

/* Extended constructor with explicit config. Same semantics +
 * contracts as clusterReplicatorCreate plus the policy in `cfg`.
 * `cfg` may be NULL; then defaults apply (max_apply_retries =
 * CLUSTER_REPLICATOR_DEFAULT_MAX_APPLY_RETRIES). */
clusterReplicator *clusterReplicatorCreateEx(
    hmacKeyring *ring, flexEnvelopeAlgo envelope_algo, uint32_t mailbox_flags,
    const persistOps *const *type_table, size_t type_table_len,
    clusterReplicatorApplyFn on_restored, void *apply_ctx,
    const clusterReplicatorConfig *cfg);

void clusterReplicatorFree(clusterReplicator *rep);

/* Total number of frames dropped after exhausting max_apply_retries
 * consecutive APPLY_REJECTED on the same (origin, sequence). Always
 * 0 when max_apply_retries == 0 (legacy infinite-retry mode).
 *
 * Thread-safety: relaxed atomic load — safe to call concurrently with
 * Ingest. Returns 0 for NULL `rep`. */
uint64_t clusterReplicatorDroppedCount(const clusterReplicator *rep);

/* Frames whose structure format this build does not read (a newer
 * peer's version or must-understand feature, another format, or no
 * prefix); each is refused like an unrestorable payload.  Relaxed
 * atomic load; 0 for NULL `rep`. */
uint64_t clusterReplicatorFormatRefusedCount(const clusterReplicator *rep);

/* Borrow the owned mailbox handle (e.g. for stats, snapshot/restore
 * of watermarks). Caller MUST NOT free it. */
clusterMailbox *clusterReplicatorMailbox(clusterReplicator *rep);

/* Buffer size required to seal a snapshot of `structure` of type
 * described by `ops` under the bound mailbox's envelope algorithm.
 * Returns 0 on bad args. */
size_t clusterReplicatorSnapshotSize(clusterReplicator *rep,
                                     const persistOps *ops,
                                     const void *structure);

/* Producer: snapshot `structure` via `ops` and seal it as a verified
 * frame. `out_cap` must be at least
 * clusterReplicatorSnapshotSize(rep, ops, structure). Returns false
 * on snapshot failure, oversized output, or seal failure. */
bool clusterReplicatorSeal(clusterReplicator *rep, const persistOps *ops,
                           const void *structure, uint32_t origin_id,
                           uint64_t sequence, hmacKeyId key_id, uint8_t *out,
                           size_t out_cap, size_t *out_written);

/* Consumer: ingest a verified frame. Internally calls
 * `clusterMailboxIngest`; the mailbox's apply hook (set inside
 * Create) routes the verified payload through `ops->restore` and into
 * the caller's apply callback. */
clusterMailboxIngestResult clusterReplicatorIngest(clusterReplicator *rep,
                                                   const uint8_t *bytes,
                                                   size_t len,
                                                   hmacKeyId key_id);

#ifdef DATAKIT_TEST
int clusterReplicatorTest(int argc, char *argv[]);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif
