/* clusterMailbox — see clusterMailbox.h for the API contract.
 *
 * Inner wire format (the envelope's payload bytes):
 * [flex header (3 entries: origin_id unsigned, sequence unsigned,
 * msg_type unsigned)] [opaque N B]
 *
 * The flex prefix self-describes its byte length via the standard flex
 * leading varint; clusterMailboxDecodeInnerHeader peeks the claim via
 * flexPeekTotalBytes, validates the prefix as a whole via
 * flexValidateBufferDeep (O(N) per-entry bound check), then advances
 * past the prefix to expose the opaque payload bytes to the apply
 * callback.
 *
 * The outer envelope is a standard flexEnvelope using the keyed-class
 * algorithm selected at create time, so the integrity tag covers both
 * the flex inner header and the opaque payload: any tamper to
 * origin_id, sequence, msg_type, or the flex framing itself fails
 * envelope verify.  Every peer in a cluster speaks this one format. */

#include "clusterMailboxInternal.h"

#include "aeadNonceLog.h"
#include "ctmem.h"   /* dkSecureZero — wipe local key copies on scope exit */
#include "datakit.h" /* zmalloc / zfree */
#include "flex.h"
#include "persist.h"

#include <stdatomic.h>

#include "cleaner.h"

/* Scope-exit free for snapshot scratch (see cleaner.h idioms) */
defcleaner(void *, zfree);
defcleanerName(clusterMailbox *, zfree, Mailbox);
defcleanerName(uint8_t *, zfree, Bytes);
defcleaner(flex *, flexFree);
#include <stddef.h> /* offsetof — cache-line padding asserts */
#include <string.h>

#define CLUSTER_MAILBOX_ORIGIN_CAP ((size_t)64)

const dkFormatSpec clusterMailboxProtocol = {
    .name = "cluster mailbox frame protocol",
    .id = DK_FORMAT_ID_CLUSTER_MAILBOX,
    .version = 1,
    .minReadVersion = 1,
    .knownFeatures = 0,
};

/* Inner header is a 5-entry flex of unsigned values: protocol version
 * (u32), protocol features (u64), origin_id (u32), sequence (u64),
 * msg_type (u8). The flex framing carries its own bytes-claim varint,
 * so the inner-frame staging code does not need a separate length
 * prefix — the flex IS the length prefix. */
#define CLUSTER_MAILBOX_INNER_HEADER_FLEX_COUNT ((size_t)5)

/* Worst-case bytes occupied by the flex-encoded inner header: total-bytes
 * varint (5B) + count varint (1B) + entries (at most 9B each for the two
 * 64-bit values, 5B each for the two 32-bit values, 2B for msg_type) +
 * tail marker (1B) = 45B.  Typical headers (small ids, version 1, no
 * features) take about 16B.  The cap sizes stack staging and the
 * sealedSize over-approximation.
 *
 * Pinned to the public CLUSTER_MAILBOX_INNER_HEADER_MAX_BYTES (header)
 * so the contract presented to test/external callers matches the
 * internal staging math. */
#define CLUSTER_MAILBOX_INNER_HEADER_MAX_LEN                                   \
    CLUSTER_MAILBOX_INNER_HEADER_MAX_BYTES
static_assert(CLUSTER_MAILBOX_INNER_HEADER_MAX_LEN >= (size_t)45,
              "inner-header cap below the worst-case encoding above");

/* Open-addressed entry; `last_applied` of 0 means the slot is unused
 * (callers must start at sequence 1, which the API documents). */
/* Slot occupancy is signalled by `origin_id != 0` — origin 0 is reserved
 * (parallel to the sequence==0 sentinel) and rejected at both Seal and
 * Ingest. Using origin_id (rather than last_applied) for occupancy is
 * critical: a slot whose `on_apply` returned false (APPLY_REJECTED) sits
 * with last_applied still 0, but it IS occupied — a fresh ingest for
 * the same origin must find that slot via the lookup and replay-check
 * against last_applied, NOT silently overwrite via a "free slot" scan.
 *
 * window_bits: 64-bit bitmap covering sequences
 * (last_applied - 63, last_applied].
 * Bit 0 represents `last_applied` itself; bit i represents
 * `last_applied - i`. Only consulted when
 * CLUSTER_MAILBOX_FLAG_REPLAY_WINDOW is set on the owning mailbox.
 *
 * Atomicity policy (stats-accessor race fix):
 * - `origin_id` and `last_applied` are `_Atomic` to enable
 * race-free observation from the metrics scraper threads that call
 * clusterMailboxLastApplied / clusterMailboxOriginCount concurrently
 * with the single-writer I/O thread that calls Ingest.
 * - The writer (Ingest) publishes updates with release; readers
 * (the public stats accessors) observe with acquire. This guarantees
 * a reader who sees a non-zero origin_id also sees the corresponding
 * last_applied write that preceded the publish.
 * - `window_bits` is NOT atomic — it is only touched by the I/O thread
 * (replay check during Ingest + snapshot/restore which the header
 * contract still pins to single-threaded ownership).
 * - Layout: `_Atomic uint64_t` is 8B (lock-free on every supported ISA)
 * and `_Atomic uint32_t` is 4B, so the struct stays at 24 bytes
 * including the existing 4B tail pad. */
typedef struct clusterMailboxOrigin {
    _Atomic uint64_t last_applied;
    uint64_t window_bits;
    _Atomic uint32_t origin_id;
    uint8_t _pad[4];
} clusterMailboxOrigin;

/* Per-mailbox counter block. _Atomic for safe concurrent observation
 * by a metrics scraper running off-thread.
 *
 * Cache-line layout: the 14 counters are split into a HOT group
 * (incremented on every successful Ingest or Seal) and a COLD group
 * (rejection / error paths), with explicit padding putting the cold
 * group on its own cache line, so the success path never shares a
 * line with the rejection counters (~100 cycles per cross-line touch
 * under concurrent observation). The whole struct is `alignas(64)` so
 * the hot block also starts on a cache-line boundary inside the parent
 * `clusterMailbox`. A `static_assert` below pins the cold offset.
 *
 * Hot/cold rationale: ingest_total + bytes_ingested + apply_ok fire on
 * every accepted frame; bytes_sealed + seal_total fire on every Seal.
 * seal_total is small (1 increment vs bytes_sealed's payload-size add)
 * but sits in the cold group so the hot block stays at exactly 32B
 * (4 × 8B atomics) — leaves room for two more 8B hot counters in the
 * future without crossing the line. All rejection / error counters
 * are touched only on failure paths, so cold. */
typedef struct clusterMailboxCounters {
    /* HOT — every successful Ingest/Seal touches these. alignas on
     * the first member forces the whole struct (and therefore the hot
     * block within any embedding struct) to a 64B boundary. */
    alignas(64) _Atomic uint64_t ingest_total;
    _Atomic uint64_t apply_ok;
    _Atomic uint64_t bytes_ingested;
    _Atomic uint64_t bytes_sealed;
    /* Pad the remainder of the hot cache line so cold counters land on
     * a fresh 64B line. 4 × 8B atomics = 32B used; pad another 32B to
     * reach the next 64B boundary. */
    uint8_t _pad_hot[32];
    /* COLD — rejection / error / low-rate paths. alignas pins the
     * group to its own cache line independently of the _pad_hot math
     * above, so a future change to the hot count cannot silently
     * collapse the two groups back onto one line. */
    alignas(64) _Atomic uint64_t apply_rejected;
    _Atomic uint64_t replay_rejected;
    _Atomic uint64_t algo_mismatch;
    _Atomic uint64_t verify_failed;
    _Atomic uint64_t key_missing;
    _Atomic uint64_t key_revoked;
    _Atomic uint64_t bad_frame;
    _Atomic uint64_t origin_table_full;
    _Atomic uint64_t param_error;
    _Atomic uint64_t seal_total;
    _Atomic uint64_t seal_param_error;
    _Atomic uint64_t protocol_rejected;
} clusterMailboxCounters;

/* Alignment invariants (forward-looking SMP hardening). */
static_assert(alignof(clusterMailboxCounters) >= 64,
              "clusterMailboxCounters must be 64B-aligned so the hot "
              "group starts on a cache line");
static_assert(offsetof(clusterMailboxCounters, apply_rejected) % 64u == 0u,
              "cold counters must start on their own cache line so "
              "writer-side error paths do not bounce the hot line");
static_assert(offsetof(clusterMailboxCounters, apply_rejected) == 64u,
              "hot group must occupy exactly the first cache line "
              "(4 × 8B atomics + 32B pad)");

struct clusterMailbox {
    hmacKeyring *ring;
    clusterApplyFn on_apply;
    void *apply_ctx;
    bool applying; /* inside on_apply: Ingest and Restore refuse re-entry */
    clusterMailboxCounters counters;
    /* defense-in-depth (key_id, nonce) reuse detector.
     *
     * The mailbox plumbs this through to the flexEnvelope params on every
     * Seal/Ingest call (cmSealImpl / cmIngestImpl). For the keyed-MAC
     * algorithms the mailbox currently pins (HMAC_SHA256 / HMAC_BLAKE3 /
     * BLAKE3_KEYED) the field is a no-op at the wire — those algos carry
     * no nonce so envelope Wrap/Verify ignore the field. The wiring
     * remains for two forward-looking reasons:
     * 1. A mailbox that uses an AEAD envelope_algo gets the defense
     * automatically.
     * 2. End-to-end clusters that interleave keyed-MAC and AEAD
     * traffic under the same mailbox topology can attach the same
     * log via clusterMailboxNonceLog for cross-cutting checks.
     *
     * Ownership: `owns_nonce_log` is true when the constructor allocated
     * the log (default) and clusterMailboxFree releases it; false when
     * the caller passed an explicit borrowed log via the options struct. */
    aeadNonceLog *nonceLog;
    flexEnvelopeAlgo envelope_algo;
    clusterMailboxFlags flags;
    /* `origin_count` is `_Atomic` so a metrics scraper calling
     * clusterMailboxOriginCount observes a coherent value while the
     * I/O thread races through cmInsertOrigin's increment. See the
     * atomicity-policy commentary on clusterMailboxOrigin above. */
    _Atomic size_t origin_count;
    bool owns_nonce_log;
    uint8_t _pad[7];
    clusterMailboxOrigin origins[CLUSTER_MAILBOX_ORIGIN_CAP];
};

/* the hot counters live at the start of `counters` and the whole
 * `clusterMailboxCounters` carries alignas(64), so the compiler is
 * required to insert padding before the field if needed. Pin the
 * invariant so a future reordering of `clusterMailbox` that drops the
 * hot-line alignment trips at compile time instead of silently bouncing
 * the line under multi-threaded ingest. */
static_assert(offsetof(struct clusterMailbox, counters) % 64u == 0u,
              "clusterMailbox::counters must start on a 64B cache line");

static void cmBumpCounter(_Atomic uint64_t *c, uint64_t delta) {
    /* @sync: relaxed counter */
    atomic_fetch_add_explicit(c, delta, memory_order_relaxed);
}

/* Map an ingest result to its counter. */
static void cmRecordIngestResult(clusterMailbox *mb,
                                 clusterMailboxIngestResult rc) {
    switch (rc) {
    case CLUSTER_MAILBOX_OK:
        cmBumpCounter(&mb->counters.apply_ok, 1);
        break;
    case CLUSTER_MAILBOX_ERR_PARAM:
        cmBumpCounter(&mb->counters.param_error, 1);
        break;
    case CLUSTER_MAILBOX_ERR_KEY_MISSING:
        cmBumpCounter(&mb->counters.key_missing, 1);
        break;
    case CLUSTER_MAILBOX_ERR_KEY_REVOKED:
        cmBumpCounter(&mb->counters.key_revoked, 1);
        break;
    case CLUSTER_MAILBOX_ERR_VERIFY_FAILED:
        cmBumpCounter(&mb->counters.verify_failed, 1);
        break;
    case CLUSTER_MAILBOX_ERR_BAD_FRAME:
        cmBumpCounter(&mb->counters.bad_frame, 1);
        break;
    case CLUSTER_MAILBOX_ERR_PROTOCOL:
        cmBumpCounter(&mb->counters.protocol_rejected, 1);
        break;
    case CLUSTER_MAILBOX_ERR_ALGO_MISMATCH:
        cmBumpCounter(&mb->counters.algo_mismatch, 1);
        break;
    case CLUSTER_MAILBOX_ERR_REPLAY:
        cmBumpCounter(&mb->counters.replay_rejected, 1);
        break;
    case CLUSTER_MAILBOX_ERR_APPLY_REJECTED:
        cmBumpCounter(&mb->counters.apply_rejected, 1);
        break;
    case CLUSTER_MAILBOX_ERR_ORIGIN_TABLE_FULL:
        cmBumpCounter(&mb->counters.origin_table_full, 1);
        break;
    }
}

#define CLUSTER_MAILBOX_REPLAY_WINDOW_BITS ((uint64_t)64)

/* Linear probe lookup for `origin_id`. Returns NULL when the
 * origin's slot is not currently occupied. Use lookupOrInsert when
 * you need to create a new entry.
 *
 * Caller MUST guarantee origin_id != 0 — origin 0 is reserved as the
 * unoccupied-slot sentinel and would falsely match every empty slot.
 *
 * Threading: only the writer path (Ingest, and Restore serialised
 * against it) calls this, and the writer is the only thread that stores
 * origin_id, so the loads read back its own stores and need no order.
 * Observers use cmLookupOriginConst, whose acquire pairs with the
 * release that publishes a slot in cmInsertOrigin. */
static clusterMailboxOrigin *cmLookupOriginMut(clusterMailbox *mb,
                                               uint32_t origin_id) {
    for (size_t i = 0; i < CLUSTER_MAILBOX_ORIGIN_CAP; i++) {
        /* Only the writer (Ingest, or a Restore serialised against it)
         * reaches this scan, and the writer is the only thread that
         * stores origin_id: it reads back its own stores. */
        /* @sync: relaxed owner */
        const uint32_t id = atomic_load_explicit(&mb->origins[i].origin_id,
                                                 memory_order_relaxed);
        if (id == origin_id) {
            return &mb->origins[i];
        }
    }
    return nullptr;
}

/* Read-only variant for `const clusterMailbox *` accessors. Same
 * scan, just preserves const-correctness through the lookup. See the
 * threading note on cmLookupOriginMut. */
static const clusterMailboxOrigin *cmLookupOriginConst(const clusterMailbox *mb,
                                                       uint32_t origin_id) {
    for (size_t i = 0; i < CLUSTER_MAILBOX_ORIGIN_CAP; i++) {
        /* @sync: acquire mailbox.slotPublish */
        const uint32_t id = atomic_load_explicit(&mb->origins[i].origin_id,
                                                 memory_order_acquire);
        if (id == origin_id) {
            return &mb->origins[i];
        }
    }
    return nullptr;
}

/* Allocate a slot for a previously-unseen origin. Caller MUST have
 * already confirmed cmLookupOriginMut returned NULL — this function
 * assumes the origin is genuinely new.
 *
 * Threading: writes `last_applied` (relaxed — it's 0, which the reader
 * already treats as "never seen") BEFORE publishing the slot via a
 * release-store on `origin_id`. This pairs with the acquire-load in
 * cmLookupOriginConst so a reader who matches the new origin_id
 * is guaranteed to see the 0-initialised last_applied (not stale junk
 * from a previously-occupied slot). */
/* The first free slot (origin_id == 0), or NULL when the table is full.
 * Scan order doesn't matter; the typical cluster has a handful of peers
 * and the table is mostly empty. */
static clusterMailboxOrigin *cmFreeOrigin(clusterMailbox *mb) {
    for (size_t i = 0; i < CLUSTER_MAILBOX_ORIGIN_CAP; i++) {
        /* The writer is the only thread that stores origin_id. */
        /* @sync: relaxed owner */
        const uint32_t cur = atomic_load_explicit(&mb->origins[i].origin_id,
                                                  memory_order_relaxed);
        if (cur == 0u) {
            return &mb->origins[i];
        }
    }
    return nullptr;
}

/* Publish free slot 'slot' as origin_id's, its watermark at 0 */
static clusterMailboxOrigin *cmPublishOrigin(clusterMailbox *mb,
                                             clusterMailboxOrigin *slot,
                                             uint32_t origin_id) {
    /* The slot is unpublished (origin_id == 0), so no observer reaches
     * it; the origin_id release below orders this. */
    /* @sync: relaxed init */
    atomic_store_explicit(&slot->last_applied, 0u, memory_order_relaxed);
    slot->window_bits = 0u;
    /* Release-publish the slot: any reader who sees this origin_id is
     * guaranteed to also see the last_applied=0 initialisation above. */
    /* @sync: release mailbox.slotPublish */
    atomic_store_explicit(&slot->origin_id, origin_id, memory_order_release);
    /* @sync: release mailbox.originCount */
    atomic_fetch_add_explicit(&mb->origin_count, 1u, memory_order_release);
    return slot;
}

static clusterMailboxOrigin *cmInsertOrigin(clusterMailbox *mb,
                                            uint32_t origin_id) {
    clusterMailboxOrigin *slot = cmFreeOrigin(mb);
    return slot ? cmPublishOrigin(mb, slot, origin_id) : nullptr;
}

/* Encode the inner frame header (flex-based).
 *
 * Builds the 5-entry flex of unsigned values, led by this build's
 * protocol stamp (caller frees via flexFree). Output bytes are
 * bit-identical
 * across producers thanks to flex's canonical encoding for unsigned
 * values, so the envelope's integrity tag covers a deterministic byte
 * stream — a tamper that flips any framing or value byte breaks verify.
 *
 * The flex is heap-allocated rather than stack-buffered because
 * flexPushUnsigned takes a `flex **` (it may realloc on growth) and we
 * want the same code path for every payload size. Cost: one zmalloc +
 * a handful of byte appends; dwarfed by the envelope HMAC compute that
 * follows. */
static flex *cmBuildInnerHeaderFlex(uint32_t origin_id, uint64_t sequence,
                                    uint8_t msg_type) {
    /* @own: caller; free=flexFree */
    flex *f = flexNew();
    if (f == nullptr) {
        return nullptr;
    }
    flexRow row = FLEX_ROW_INIT;
    flexRowUnsigned(&row, (uint64_t)clusterMailboxProtocol.version);
    flexRowUnsigned(&row, 0); /* protocol features */
    flexRowUnsigned(&row, (uint64_t)origin_id);
    flexRowUnsigned(&row, sequence);
    flexRowUnsigned(&row, (uint64_t)msg_type);
    flexRowAppendExact(&f, &row);
    return f;
}

/* Encode an inner-frame header into `out` (cap >=
 * CLUSTER_MAILBOX_INNER_HEADER_MAX_BYTES). Returns the encoded byte length, or
 * 0 on bad args / insufficient capacity / allocation failure. Public surface so
 * unit tests can round-trip the framing without going through the full Seal
 * path. */
size_t clusterMailboxEncodeInnerHeader(uint8_t *out, size_t out_cap,
                                       uint32_t origin_id, uint64_t sequence,
                                       uint8_t msg_type) {
    if (out == nullptr || out_cap < CLUSTER_MAILBOX_INNER_HEADER_MAX_LEN) {
        return 0;
    }
    flex *f = cmBuildInnerHeaderFlex(origin_id, sequence, msg_type);
    if (f == nullptr) {
        return 0;
    }
    const size_t n = flexBytes(f);
    if (n > out_cap || n > CLUSTER_MAILBOX_INNER_HEADER_MAX_LEN) {
        flexFree(f);
        return 0;
    }
    memcpy(out, f, n);
    flexFree(f);
    return n;
}

/* Parse a flex-encoded inner header from `src`. `src_len` is the total
 * length of the payload region returned by envelope verify — it
 * includes the flex prefix AND any trailing opaque payload bytes.
 *
 * On success returns true and sets `*hdr_len` to the byte length the
 * flex prefix consumed (so the caller can compute payload start /
 * length as `src + *hdr_len` and `src_len - *hdr_len`). Returns false
 * on any of: short buffer, malformed flex framing, wrong entry count,
 * an entry that is not a 64-bit unsigned integer (96/128-bit included),
 * or any value outside its representable range (protocol version or
 * origin_id > UINT32_MAX, msg_type > UINT8_MAX). The protocol stamp is
 * returned for the caller to check. */
bool clusterMailboxDecodeInnerHeader(const uint8_t *src, size_t src_len,
                                     dkFormatStamp *protocol,
                                     uint32_t *origin_id, uint64_t *sequence,
                                     uint8_t *msg_type, size_t *hdr_len) {
    if (src == nullptr || protocol == nullptr || origin_id == nullptr ||
        sequence == nullptr || msg_type == nullptr || hdr_len == nullptr) {
        return false;
    }
    /* Step 1: safely peek the flex prefix's claimed length so we know
     * what subrange of `src` to deep-validate. */
    size_t flex_len = 0;
    if (!flexPeekTotalBytes(src, src_len, &flex_len)) {
        return false;
    }
    /* Step 2: deep O(N) framing check.  flexValidateBufferDeep walks
     * every entry's length-encoding bytes and rejects any buffer where
     * an entry would over-read the claimed bounds.  This makes the
     * decoder safe under arbitrary input bytes — no upstream envelope-
     * MAC pre-gate required.  The production path still goes through
     * the envelope MAC for replay protection and authenticity, but
     * synthetic fuzzers (and any future caller that bypasses the
     * envelope) cannot induce flexGetByTypeBig / flexNext to overread. */
    if (!flexValidateBufferDeep(src, flex_len)) {
        return false;
    }
    const flex *f = (const flex *)src;
    if (flexCount(f) != CLUSTER_MAILBOX_INNER_HEADER_FLEX_COUNT) {
        return false;
    }
    /* Step 3: walk + strict-type-check the five entries. */
    flexEntry *fe = flexHead(f);
    if (fe == nullptr) {
        return false;
    }
    uint64_t v = 0;

    /* Entries 1-2: protocol version (fits in uint32_t) and features */
    uint64_t features = 0;
    if (!flexGetUnsigned(fe, &v) || v > UINT32_MAX) {
        return false;
    }
    fe = flexNext(f, fe);
    if (fe == nullptr || !flexGetUnsigned(fe, &features)) {
        return false;
    }
    const dkFormatStamp stamp = {.id = DK_FORMAT_ID_CLUSTER_MAILBOX,
                                 .version = (uint32_t)v,
                                 .features = features};
    fe = flexNext(f, fe);
    if (fe == nullptr) {
        return false;
    }

    /* Entry 3: origin_id (unsigned, must fit in uint32_t). */
    if (!flexGetUnsigned(fe, &v) || v > UINT32_MAX) {
        return false;
    }
    const uint32_t origin = (uint32_t)v;

    fe = flexNext(f, fe);
    if (fe == nullptr) {
        return false;
    }
    /* Entry 4: sequence (unsigned 64-bit). */
    if (!flexGetUnsigned(fe, &v)) {
        return false;
    }
    const uint64_t seq = v;

    fe = flexNext(f, fe);
    if (fe == nullptr) {
        return false;
    }
    /* Entry 5: msg_type (unsigned, must fit in uint8_t). */
    if (!flexGetUnsigned(fe, &v) || v > UINT8_MAX) {
        return false;
    }
    const uint8_t mtype = (uint8_t)v;

    *protocol = stamp;
    *origin_id = origin;
    *sequence = seq;
    *msg_type = mtype;
    *hdr_len = flex_len;
    return true;
}

clusterMailbox *
clusterMailboxCreateOpts(const clusterMailboxCreateOptions *opts) {
    if (opts == nullptr || opts->ring == nullptr) {
        return nullptr;
    }
    /* Pin to keyed-class algorithms — checksum-only envelopes don't
     * carry tamper evidence, AEAD class uses a different API path. */
    if (!flexEnvelopeAlgoIsKeyed(opts->envelope_algo) ||
        FLEX_ENV_ALGO_IS_AEAD(opts->envelope_algo)) {
        return nullptr;
    }
    /* Reject unknown flag bits — future-extension hardening. */
    if ((opts->flags & ~(uint32_t)CLUSTER_MAILBOX_FLAG_REPLAY_WINDOW) != 0u) {
        return nullptr;
    }
    cleanerName(zfree, Mailbox) clusterMailbox *mb = zmalloc(sizeof(*mb));
    memset(mb, 0, sizeof(*mb));
    mb->ring = opts->ring;
    mb->on_apply = opts->on_apply;
    mb->apply_ctx = opts->apply_ctx;
    mb->envelope_algo = opts->envelope_algo;
    mb->flags = opts->flags;
    /* allocate (or borrow) the nonceLog per options. */
    if (opts->nonce_log != nullptr) {
        mb->nonceLog = opts->nonce_log;
        mb->owns_nonce_log = false;
    } else if (!opts->disable_nonce_log) {
        const size_t cap = (opts->nonce_log_capacity != 0u)
                               ? opts->nonce_log_capacity
                               : CLUSTER_MAILBOX_DEFAULT_NONCE_LOG_CAP;
        /* @own: container; free=aeadNonceLogFree */
        mb->nonceLog = aeadNonceLogCreate(cap);
        if (mb->nonceLog == nullptr) {
            /* Allocation failure: refuse to construct rather than
             * silently downgrade to no-protection. Callers that genuinely
             * want no log must set disable_nonce_log = true. */
            return nullptr;
        }
        mb->owns_nonce_log = true;
    } else {
        mb->nonceLog = nullptr;
        mb->owns_nonce_log = false;
    }

    clusterMailbox *out = mb;
    mb = nullptr;
    return out;
}

clusterMailbox *clusterMailboxCreateEx(hmacKeyring *ring,
                                       flexEnvelopeAlgo envelope_algo,
                                       clusterApplyFn on_apply, void *apply_ctx,
                                       clusterMailboxFlags flags) {
    /* Default-create a nonceLog so existing callers automatically get
     * the (key_id, nonce) reuse defense without code changes. Callers
     * that need to opt out or share a log must use the Opts constructor. */
    const clusterMailboxCreateOptions opts = {
        .ring = ring,
        .envelope_algo = envelope_algo,
        .on_apply = on_apply,
        .apply_ctx = apply_ctx,
        .flags = flags,
    };
    return clusterMailboxCreateOpts(&opts);
}

clusterMailbox *clusterMailboxCreate(hmacKeyring *ring,
                                     flexEnvelopeAlgo envelope_algo,
                                     clusterApplyFn on_apply, void *apply_ctx) {
    return clusterMailboxCreateEx(ring, envelope_algo, on_apply, apply_ctx,
                                  CLUSTER_MAILBOX_FLAG_NONE);
}

void clusterMailboxFree(clusterMailbox *mb) {
    if (mb == nullptr) {
        return;
    }
    /* release the nonceLog when we allocated it ourselves; never
     * touch a caller-borrowed log (the caller is responsible for that
     * lifecycle and may share one log across multiple mailboxes). */
    if (mb->owns_nonce_log && mb->nonceLog != nullptr) {
        aeadNonceLogFree(mb->nonceLog);
    }
    mb->nonceLog = nullptr;
    /* Wipe the origin table — sequence numbers reveal traffic
     * patterns; not strictly secret but worth zeroing along with
     * everything else. */
    dkSecureZero(mb->origins, sizeof(mb->origins));
    zfree(mb);
}

aeadNonceLog *clusterMailboxNonceLog(const clusterMailbox *mb) {
    if (mb == nullptr) {
        return nullptr;
    }
    return mb->nonceLog;
}

bool clusterMailboxAttachKeyring(clusterMailbox *mb, hmacKeyring *ring,
                                 flexEnvelopeAlgo envelope_algo,
                                 clusterApplyFn on_apply, void *apply_ctx) {
    if (mb == nullptr || ring == nullptr) {
        return false;
    }
    /* Pin to keyed-class algorithms — checksum-only envelopes don't
     * carry tamper evidence, AEAD class uses a different API path.
     * Mirrors the validation in clusterMailboxCreateEx so an attached
     * mailbox cannot end up in a state that the public constructor
     * would have rejected. */
    if (!flexEnvelopeAlgoIsKeyed(envelope_algo) ||
        FLEX_ENV_ALGO_IS_AEAD(envelope_algo)) {
        return false;
    }
    mb->ring = ring;
    mb->envelope_algo = envelope_algo;
    mb->on_apply = on_apply;
    mb->apply_ctx = apply_ctx;
    return true;
}

size_t clusterMailboxOriginCount(const clusterMailbox *mb) {
    if (mb == nullptr) {
        return 0;
    }
    /* Acquire pairs with the release-increment in cmInsertOrigin, so
     * the observed count is never higher than the number of slots whose
     * origin_id is already visible to a concurrent
     * clusterMailboxLastApplied probe. */
    /* @sync: acquire mailbox.originCount */
    return atomic_load_explicit(&mb->origin_count, memory_order_acquire);
}

uint64_t clusterMailboxLastApplied(const clusterMailbox *mb,
                                   uint32_t origin_id) {
    if (mb == nullptr || origin_id == 0u) {
        return 0;
    }
    const clusterMailboxOrigin *e = cmLookupOriginConst(mb, origin_id);
    if (e == nullptr) {
        return 0;
    }
    /* Acquire pairs with the release-store in cmAdvanceLastApplied.
     * Concurrent readers see a coherent 64-bit value (no torn read) and
     * a monotonic non-decreasing watermark — the writer is single-thread
     * and never rewinds last_applied except via single-threaded Restore. */
    /* @sync: acquire mailbox.watermark */
    return atomic_load_explicit(&e->last_applied, memory_order_acquire);
}

size_t clusterMailboxSealedSize(const clusterMailbox *mb, size_t payload_len) {
    if (mb == nullptr) {
        return 0;
    }
    /* bound by the new flex-based header's worst-case width. The
     * actual encoded length is smaller for typical values (~14B vs the
     * 32B cap), so callers may receive a buffer slightly larger than
     * the eventual `out_written` — that is the same conservatism the
     * envelope's own sizing math already applies. */
    if (payload_len > SIZE_MAX - CLUSTER_MAILBOX_INNER_HEADER_MAX_LEN) {
        return 0;
    }
    return flexEnvelopeWrappedSize(
        (uint64_t)(payload_len + CLUSTER_MAILBOX_INNER_HEADER_MAX_LEN),
        mb->envelope_algo, FLEX_ENV_FLAG_NONE);
}

static bool cmSealImpl(clusterMailbox *mb, uint32_t origin_id,
                       uint64_t sequence, uint8_t msg_type,
                       const uint8_t *payload, size_t payload_len,
                       hmacKeyId key_id, uint8_t *out, size_t out_cap,
                       size_t *out_written) {
    if (mb == nullptr || out == nullptr) {
        return false;
    }
    if (sequence == 0 || origin_id == 0u) {
        /* Sequence and origin_id 0 are reserved sentinels. Refusing
         * here keeps the wire format clean and matches Ingest's
         * symmetric rejection of bad-frame inner headers. */
        return false;
    }
    if (payload_len > 0 && payload == nullptr) {
        return false;
    }

    /* Copy key bytes into a caller-owned stack buffer so a concurrent
     * hmacKeyringRetire on this id cannot free the bytes mid-Seal. The
     * borrowed-pointer variant (hmacKeyringGetRaw) returns immediately
     * after releasing the rwlock and leaves a UAF window stretching from
     * the lookup through every byte the envelope HMAC compresses. The
     * copy is made under the rwlock and is independent of any subsequent
     * keyring mutation. dkSecureZero on every exit path scrubs the
     * key-equivalent material before the stack frame unwinds. */
    uint8_t key_copy[HMAC_KEYRING_MAX_KEY_BYTES];
    size_t key_len = 0;
    /* A revoked id seals nothing, though its bytes may still be held */
    if (key_id < hmacKeyringMinValidKeyId(mb->ring) ||
        !hmacKeyringGetRawCopy(mb->ring, key_id, key_copy, sizeof(key_copy),
                               &key_len, nullptr)) {
        return false;
    }

    /* Build inner frame: [flex-encoded header][payload].
     *
     * The header is a 3-entry flex of unsigned values
     * (origin_id, sequence, msg_type). Build it on the heap since
     * flexPushUnsigned may realloc on growth; the cost is negligible
     * compared to the envelope HMAC compute that follows.
     *
     * Zero-copy fast-path: reserve the envelope's payload region up
     * front and copy the flex bytes + payload bytes DIRECTLY into it,
     * skipping the historical [stack_inner / heap_inner] staging buffer
     * + extra memcpy into flexEnvelopeWrap. flexEnvelopeFinalizeWrap
     * then computes the HMAC in place. */
    flex *header_flex = cmBuildInnerHeaderFlex(origin_id, sequence, msg_type);
    if (header_flex == nullptr) {
        dkSecureZero(key_copy, sizeof(key_copy));
        return false;
    }
    const size_t header_len = flexBytes(header_flex);
    /* Defensive: the flex must not exceed the worst-case bound the
     * sizing helper relies on — otherwise downstream buffer math
     * (clusterMailboxSealedSize callers) would under-allocate. This
     * cannot fire for the documented value ranges but guards future
     * encoding changes. */
    if (header_len > CLUSTER_MAILBOX_INNER_HEADER_MAX_LEN) {
        flexFree(header_flex);
        dkSecureZero(key_copy, sizeof(key_copy));
        return false;
    }
    const size_t inner_len = header_len + payload_len;

    flexEnvelopeWrapParams wp = {
        .key = key_copy,
        .key_len = key_len,
        .algo = mb->envelope_algo,
        .on_wire_flags = FLEX_ENV_FLAG_NONE,
        /* plumb mailbox's nonceLog through. flexEnvelopeWrap
         * ignores the field for non-AEAD algos (which the mailbox
         * currently pins) — the plumbing is forward-looking so any
         * future AEAD-class mailbox variant inherits the protection. */
        .nonceLog = mb->nonceLog,
    };
    uint8_t *payload_region = nullptr;
    size_t env_header_len = 0;
    if (!flexEnvelopeReserveWrap(out, out_cap, (uint64_t)inner_len, &wp,
                                 &payload_region, &env_header_len)) {
        flexFree(header_flex);
        dkSecureZero(key_copy, sizeof(key_copy));
        return false;
    }
    /* Direct emit into the envelope's payload slot: flex header, then
     * caller payload. No intermediate staging. */
    memcpy(payload_region, header_flex, header_len);
    flexFree(header_flex);
    if (payload_len > 0) {
        memcpy(payload_region + header_len, payload, payload_len);
    }
    size_t written = 0;
    if (!flexEnvelopeFinalizeWrap(out, out_cap, env_header_len,
                                  (uint64_t)inner_len, &wp, &written)) {
        dkSecureZero(key_copy, sizeof(key_copy));
        return false;
    }
    if (out_written != nullptr) {
        *out_written = written;
    }
    dkSecureZero(key_copy, sizeof(key_copy));
    return true;
}

static clusterMailboxIngestResult cmIngestImpl(clusterMailbox *mb,
                                               const uint8_t *bytes, size_t len,
                                               hmacKeyId key_id) {
    if (mb == nullptr || bytes == nullptr || mb->applying) {
        return CLUSTER_MAILBOX_ERR_PARAM; /* applying: from inside on_apply */
    }

    /* Algorithm pinning: peek the framing FIRST so we can reject an
     * algo mismatch before doing any keyed work. An attacker who
     * downgrades the wrapper's algo (e.g. swaps HMAC-SHA-256 for
     * XXH64) would otherwise get the verify to "succeed" against the
     * mailbox-bound key — but only because the keyed branch was
     * bypassed. Refusing here closes that downgrade. */
    flexEnvelopeHeader peek = {};
    if (!flexEnvelopePeek(bytes, len, &peek)) {
        return CLUSTER_MAILBOX_ERR_VERIFY_FAILED;
    }
    if ((flexEnvelopeAlgo)peek.algo != mb->envelope_algo) {
        return CLUSTER_MAILBOX_ERR_ALGO_MISMATCH;
    }

    /* Copy key bytes into a caller-owned stack buffer so a concurrent
     * hmacKeyringRetire on this id cannot free the bytes mid-Ingest. The
     * borrowed-pointer variant returns immediately after releasing the
     * rwlock and leaves a UAF window stretching from this resolve through
     * envelope verify, inner-header decode, and replay-window bookkeeping
     * (every step below reads `key_copy` directly or transitively). The
     * copy is made under the rwlock and is independent of any subsequent
     * keyring mutation. dkSecureZero on every exit path scrubs the
     * key-equivalent material before the stack frame unwinds.
     *
     * The prepared-key fast path (hmacKeyringLookup) is intentionally
     * NOT used here: the `hmacKey` struct embeds borrowed pointers into
     * the keyring slot's inner/outer-ctx storage, which a concurrent
     * Retire would `dkSecureZero` + free mid-verify; the raw-bytes copy
     * closes that hazard. flexEnvelopeVerify falls back
     * to per-call hmacPrepareKey on the raw bytes — costs ~2 extra
     * block compressions per Ingest, gain is closing the prepared-
     * key UAF window. */
    uint8_t key_copy[HMAC_KEYRING_MAX_KEY_BYTES];
    size_t raw_key_len = 0;
    /* The floor is policy, independent of Retire: an id below it is
     * refused while its bytes are still installed */
    if (key_id < hmacKeyringMinValidKeyId(mb->ring)) {
        return CLUSTER_MAILBOX_ERR_KEY_REVOKED;
    }
    if (!hmacKeyringGetRawCopy(mb->ring, key_id, key_copy, sizeof(key_copy),
                               &raw_key_len, nullptr)) {
        return CLUSTER_MAILBOX_ERR_KEY_MISSING;
    }

    flexEnvelopeVerifyParams vp = {
        .max_payload_len = FLEX_ENVELOPE_DEFAULT_MAX_PAYLOAD,
        .key = key_copy,
        .key_len = raw_key_len,
        /* plumb mailbox's nonceLog through. flexEnvelopeVerify
         * ignores the field for non-AEAD algos (which the mailbox
         * currently pins). An AEAD-class mailbox variant would inherit
         * end-to-end replay detection — duplicates surface as
         * FLEX_ENV_VERIFY_FAILED with last-error
         * FLEX_ENV_VERIFY_FAILED_NONCE_REUSED. */
        .nonceLog = mb->nonceLog,
    };
    const uint8_t *payload_bytes = nullptr;
    uint64_t payload_bytes_len = 0;
    if (!flexEnvelopeVerify(bytes, len, &vp, &payload_bytes,
                            &payload_bytes_len)) {
        dkSecureZero(key_copy, sizeof(key_copy));
        return CLUSTER_MAILBOX_ERR_VERIFY_FAILED;
    }

    /* decode the flex-encoded inner header. Returns
     * false (→ BAD_FRAME) on any of: short payload, malformed flex
     * framing, wrong entry count, an entry that is not a 64-bit
     * unsigned integer, or out-of-range values. `hdr_len` is the length the
     * flex prefix consumed — the remainder of `payload_bytes` is the opaque
     * application payload that gets handed to the apply callback below. */
    uint32_t origin_id = 0;
    uint64_t sequence = 0;
    uint8_t msg_type = 0;
    size_t hdr_len = 0;
    dkFormatStamp protocol;
    if (!clusterMailboxDecodeInnerHeader(
            payload_bytes, (size_t)payload_bytes_len, &protocol, &origin_id,
            &sequence, &msg_type, &hdr_len)) {
        dkSecureZero(key_copy, sizeof(key_copy));
        return CLUSTER_MAILBOX_ERR_BAD_FRAME;
    }
    /* A frame from a peer speaking a protocol this build does not read
     * is refused before any of it is interpreted */
    if (dkFormatCheck(&clusterMailboxProtocol, &protocol) != DK_FORMAT_OK) {
        dkSecureZero(key_copy, sizeof(key_copy));
        return CLUSTER_MAILBOX_ERR_PROTOCOL;
    }
    if (sequence == 0 || origin_id == 0u) {
        /* Both sequence 0 and origin 0 are reserved sentinels — the
         * latter signals "unoccupied slot" inside the origin table, so
         * a frame claiming origin 0 cannot be tracked safely. */
        dkSecureZero(key_copy, sizeof(key_copy));
        return CLUSTER_MAILBOX_ERR_BAD_FRAME;
    }

    /* The remaining path below (replay bookkeeping + apply callback)
     * never re-reads the key bytes; the verify above was the last
     * consumer. Scrub now so the stack buffer is zero across the
     * application's apply callback, which may run user-supplied code. */
    dkSecureZero(key_copy, sizeof(key_copy));

    /* Replay check before allocating an origin slot — keeps repeated
     * replays from filling the table from an attacker. An existing
     * slot might have last_applied == 0 (apply previously rejected),
     * in which case the lookup still finds it via origin_id and the
     * replay comparison falls through to the standard logic below
     * (sequence >= 1 always > 0). */
    clusterMailboxOrigin *existing = cmLookupOriginMut(mb, origin_id);
    const bool use_window =
        (mb->flags & CLUSTER_MAILBOX_FLAG_REPLAY_WINDOW) != 0u;
    /* Cache last_applied locally (single-writer-thread, so the value is
     * stable for the remainder of this call). Reads use relaxed because
     * we are the sole writer and have no cross-thread ordering
     * obligation INTO this routine — the only outbound ordering is the
     * release-store at the end of the apply path. */
    if (existing != nullptr) {
        /* @sync: relaxed owner */
        const uint64_t cur =
            atomic_load_explicit(&existing->last_applied, memory_order_relaxed);
        if (!use_window) {
            /* Strict-monotonic: anything <= watermark is a replay. */
            if (sequence <= cur) {
                return CLUSTER_MAILBOX_ERR_REPLAY;
            }
        } else if (sequence <= cur) {
            /* Within or past the window. */
            const uint64_t age = cur - sequence;
            if (age >= CLUSTER_MAILBOX_REPLAY_WINDOW_BITS) {
                /* Older than the window — rejected as ancient replay. */
                return CLUSTER_MAILBOX_ERR_REPLAY;
            }
            const uint64_t bit = (uint64_t)1 << age;
            if ((existing->window_bits & bit) != 0u) {
                /* Already applied (bit set means "seen"). */
                return CLUSTER_MAILBOX_ERR_REPLAY;
            }
            /* In-window legitimate out-of-order — fall through to apply. */
        }
        /* sequence > last_applied: advance watermark + shift window. */
    }
    /* A new origin needs a free slot, taken once its frame is applied:
     * the table holds the origins with an applied frame, so a refused
     * one neither counts nor keeps a slot (the apply callback cannot
     * ingest, so the slot stays free until then) */
    clusterMailboxOrigin *slot = existing;
    clusterMailboxOrigin *freeSlot = nullptr;
    if (slot == nullptr) {
        freeSlot = cmFreeOrigin(mb);
        if (freeSlot == nullptr) {
            return CLUSTER_MAILBOX_ERR_ORIGIN_TABLE_FULL;
        }
    }

    /* Hand the verified frame to the caller. Pointers are valid
     * only for the duration of this call. payload offset is
     * computed from the flex prefix's actual encoded length
     * (`hdr_len`), not a fixed constant — the flex framing is
     * value-dependent. */
    clusterFrame frame = {
        .payload = payload_bytes + hdr_len,
        .payload_len = (size_t)payload_bytes_len - hdr_len,
        .sequence = sequence,
        .origin_id = origin_id,
        .msg_type = msg_type,
    };
    if (mb->on_apply != nullptr) {
        mb->applying = true;
        const bool applied = mb->on_apply(&frame, mb->apply_ctx);
        mb->applying = false;
        if (!applied) {
            /* Caller rejected — do NOT advance the watermark. This
             * lets the producer retransmit and get re-applied. */
            return CLUSTER_MAILBOX_ERR_APPLY_REJECTED;
        }
    }
    if (slot == nullptr) {
        slot = cmPublishOrigin(mb, freeSlot, origin_id);
    }
    /* Advance watermark + maintain the replay bitmap. The watermark
     * publish uses release so an observer whose acquire load in
     * clusterMailboxLastApplied reads `sequence` also sees everything
     * this thread did before it, the on_apply effects of that frame
     * included. window_bits is not part of that: it is non-atomic and
     * only the I/O thread (or a Snapshot/Restore serialised against it)
     * reads it. */
    /* @sync: relaxed owner */
    const uint64_t cur =
        atomic_load_explicit(&slot->last_applied, memory_order_relaxed);
    if (use_window) {
        if (sequence > cur) {
            /* New high water: shift the bitmap right by (sequence -
             * last_applied), then set bit 0 (representing the new
             * watermark itself). If the shift >= 64, the bitmap is
             * cleared completely. */
            const uint64_t shift = sequence - cur;
            if (shift >= CLUSTER_MAILBOX_REPLAY_WINDOW_BITS) {
                slot->window_bits = 0;
            } else {
                slot->window_bits <<= shift;
            }
            slot->window_bits |= (uint64_t)1; /* mark the new watermark */
            /* @sync: release mailbox.watermark */
            atomic_store_explicit(&slot->last_applied, sequence,
                                  memory_order_release);
        } else {
            /* In-window fill: set the corresponding bit (we already
             * confirmed it was clear above). last_applied unchanged. */
            const uint64_t age = cur - sequence;
            slot->window_bits |= ((uint64_t)1 << age);
        }
    } else if (sequence > cur) {
        /* Strict: every sequence at or below the watermark is closed, so
         * the whole window reads as seen (a snapshot of this slot
         * restored into a windowed mailbox must not reopen it) */
        slot->window_bits = UINT64_MAX;
        /* @sync: release mailbox.watermark */
        atomic_store_explicit(&slot->last_applied, sequence,
                              memory_order_release);
    }
    return CLUSTER_MAILBOX_OK;
}

/* === Public wrappers with stats accounting ========================== */

bool clusterMailboxSeal(clusterMailbox *mb, uint32_t origin_id,
                        uint64_t sequence, uint8_t msg_type,
                        const uint8_t *payload, size_t payload_len,
                        hmacKeyId key_id, uint8_t *out, size_t out_cap,
                        size_t *out_written) {
    size_t local_written = 0;
    size_t *wp = (out_written != nullptr) ? out_written : &local_written;
    const bool ok = cmSealImpl(mb, origin_id, sequence, msg_type, payload,
                               payload_len, key_id, out, out_cap, wp);
    if (mb == nullptr) {
        return false;
    }
    if (ok) {
        cmBumpCounter(&mb->counters.seal_total, 1);
        cmBumpCounter(&mb->counters.bytes_sealed, (uint64_t)*wp);
    } else {
        cmBumpCounter(&mb->counters.seal_param_error, 1);
    }
    return ok;
}

clusterMailboxIngestResult clusterMailboxIngest(clusterMailbox *mb,
                                                const uint8_t *bytes,
                                                size_t len, hmacKeyId key_id) {
    const clusterMailboxIngestResult rc = cmIngestImpl(mb, bytes, len, key_id);
    if (mb == nullptr) {
        return rc;
    }
    cmBumpCounter(&mb->counters.ingest_total, 1);
    cmBumpCounter(&mb->counters.bytes_ingested, (uint64_t)len);
    cmRecordIngestResult(mb, rc);
    return rc;
}

void clusterMailboxGetStats(const clusterMailbox *mb,
                            clusterMailboxStats *out) {
    if (mb == nullptr || out == nullptr) {
        if (out != nullptr) {
            memset(out, 0, sizeof(*out));
        }
        return;
    }
    /* Read each counter under relaxed order — torn 64-bit reads are
     * impossible on every supported ISA but _Atomic loads make that
     * portable. Snapshot is per-counter consistent, not cross-counter
     * consistent — an observer might see ingest_total advance before
     * its outcome counter does (or vice versa) by a few ns. */
    /* @sync: relaxed counter */
    out->ingest_total =
        atomic_load_explicit(&mb->counters.ingest_total, memory_order_relaxed);
    /* @sync: relaxed counter */
    out->apply_ok =
        atomic_load_explicit(&mb->counters.apply_ok, memory_order_relaxed);
    /* @sync: relaxed counter */
    out->apply_rejected = atomic_load_explicit(&mb->counters.apply_rejected,
                                               memory_order_relaxed);
    /* @sync: relaxed counter */
    out->replay_rejected = atomic_load_explicit(&mb->counters.replay_rejected,
                                                memory_order_relaxed);
    /* @sync: relaxed counter */
    out->algo_mismatch =
        atomic_load_explicit(&mb->counters.algo_mismatch, memory_order_relaxed);
    /* @sync: relaxed counter */
    out->verify_failed =
        atomic_load_explicit(&mb->counters.verify_failed, memory_order_relaxed);
    /* @sync: relaxed counter */
    out->key_missing =
        atomic_load_explicit(&mb->counters.key_missing, memory_order_relaxed);
    /* @sync: relaxed counter */
    out->key_revoked =
        atomic_load_explicit(&mb->counters.key_revoked, memory_order_relaxed);
    /* @sync: relaxed counter */
    out->bad_frame =
        atomic_load_explicit(&mb->counters.bad_frame, memory_order_relaxed);
    /* @sync: relaxed counter */
    out->origin_table_full = atomic_load_explicit(
        &mb->counters.origin_table_full, memory_order_relaxed);
    /* @sync: relaxed counter */
    out->protocol_rejected = atomic_load_explicit(
        &mb->counters.protocol_rejected, memory_order_relaxed);
    /* @sync: relaxed counter */
    out->param_error =
        atomic_load_explicit(&mb->counters.param_error, memory_order_relaxed);
    /* @sync: relaxed counter */
    out->seal_total =
        atomic_load_explicit(&mb->counters.seal_total, memory_order_relaxed);
    /* @sync: relaxed counter */
    out->seal_param_error = atomic_load_explicit(&mb->counters.seal_param_error,
                                                 memory_order_relaxed);
    /* @sync: relaxed counter */
    out->bytes_ingested = atomic_load_explicit(&mb->counters.bytes_ingested,
                                               memory_order_relaxed);
    /* @sync: relaxed counter */
    out->bytes_sealed =
        atomic_load_explicit(&mb->counters.bytes_sealed, memory_order_relaxed);
}

void clusterMailboxResetStats(clusterMailbox *mb) {
    if (mb == nullptr) {
        return;
    }
    /* @sync: relaxed counter */
    atomic_store_explicit(&mb->counters.ingest_total, 0, memory_order_relaxed);
    /* @sync: relaxed counter */
    atomic_store_explicit(&mb->counters.apply_ok, 0, memory_order_relaxed);
    /* @sync: relaxed counter */
    atomic_store_explicit(&mb->counters.apply_rejected, 0,
                          memory_order_relaxed);
    /* @sync: relaxed counter */
    atomic_store_explicit(&mb->counters.replay_rejected, 0,
                          memory_order_relaxed);
    /* @sync: relaxed counter */
    atomic_store_explicit(&mb->counters.algo_mismatch, 0, memory_order_relaxed);
    /* @sync: relaxed counter */
    atomic_store_explicit(&mb->counters.verify_failed, 0, memory_order_relaxed);
    /* @sync: relaxed counter */
    atomic_store_explicit(&mb->counters.key_missing, 0, memory_order_relaxed);
    /* @sync: relaxed counter */
    atomic_store_explicit(&mb->counters.key_revoked, 0, memory_order_relaxed);
    /* @sync: relaxed counter */
    atomic_store_explicit(&mb->counters.bad_frame, 0, memory_order_relaxed);
    /* @sync: relaxed counter */
    atomic_store_explicit(&mb->counters.origin_table_full, 0,
                          memory_order_relaxed);
    /* @sync: relaxed counter */
    atomic_store_explicit(&mb->counters.protocol_rejected, 0,
                          memory_order_relaxed);
    /* @sync: relaxed counter */
    atomic_store_explicit(&mb->counters.param_error, 0, memory_order_relaxed);
    /* @sync: relaxed counter */
    atomic_store_explicit(&mb->counters.seal_total, 0, memory_order_relaxed);
    /* @sync: relaxed counter */
    atomic_store_explicit(&mb->counters.seal_param_error, 0,
                          memory_order_relaxed);
    /* @sync: relaxed counter */
    atomic_store_explicit(&mb->counters.bytes_ingested, 0,
                          memory_order_relaxed);
    /* @sync: relaxed counter */
    atomic_store_explicit(&mb->counters.bytes_sealed, 0, memory_order_relaxed);
}

/* === Key rotation choreography ===================================== */

bool clusterRotationInit(clusterRotationState *st, hmacKeyId initial) {
    if (st == nullptr || initial == HMAC_KEY_ID_INVALID) {
        return false;
    }
    st->primary = initial;
    st->previous = HMAC_KEY_ID_INVALID;
    return true;
}

bool clusterRotationPromote(clusterRotationState *st, hmacKeyId new_primary) {
    if (st == nullptr || new_primary == HMAC_KEY_ID_INVALID) {
        return false;
    }
    if (new_primary == st->primary) {
        /* Refusing the no-op surfaces "I thought rotation just happened
         * but the operator handed me the same id twice" as an error. */
        return false;
    }
    st->previous = st->primary;
    st->primary = new_primary;
    return true;
}

void clusterRotationRetirePrevious(clusterRotationState *st) {
    if (st == nullptr) {
        return;
    }
    st->previous = HMAC_KEY_ID_INVALID;
}

bool clusterRotationAccepts(const clusterRotationState *st, hmacKeyId id) {
    if (st == nullptr || id == HMAC_KEY_ID_INVALID) {
        return false;
    }
    return id == st->primary || id == st->previous;
}

size_t clusterMailboxBuildRotateAnnounce(
    uint8_t out[CLUSTER_MAILBOX_ROTATE_ANNOUNCE_PAYLOAD_LEN],
    hmacKeyId new_id) {
    /* The rotate announce is a 1-entry flex (new_id unsigned), like
     * the inner header described at the top of the file.
     *
     * Buffer-size contract: the API still pins `out` to a fixed-size
     * array of CLUSTER_MAILBOX_ROTATE_ANNOUNCE_PAYLOAD_LEN bytes. The
     * worst-case flex encoding (5B total-bytes varint + 1B count + 9B
     * u64 entry + 1B tail = 16B) fits inside the constant. Typical
     * encodings for the realistic key id range (1..2^32) consume
     * 13-15 bytes — see the test plan for a length assertion. */
    if (out == nullptr || new_id == HMAC_KEY_ID_INVALID) {
        return 0;
    }
    cleaner(flexFree) flex *f = flexNew();
    if (f == nullptr) {
        return 0;
    }
    flexPushUnsigned(&f, (uint64_t)new_id, FLEX_ENDPOINT_TAIL);
    const size_t n = flexBytes(f);
    /* Defensive bound check: the announce payload is a fixed-size API
     * surface, so a flex larger than the published constant would
     * overflow the caller's `out`. Cannot fire for the current
     * encoder + key-id range but guards future encoding tweaks. */
    if (n > CLUSTER_MAILBOX_ROTATE_ANNOUNCE_PAYLOAD_LEN) {
        return 0;
    }
    memcpy(out, f, n);
    return n;
}

bool clusterMailboxParseRotateAnnounce(const uint8_t *payload, size_t len,
                                       hmacKeyId *out_new_id) {
    /* Parse the 1-entry flex.  flexValidateBufferDeep performs the full
     * O(N) framing walk (every entry's length-encoding bytes are bound-
     * checked) before flexGetByTypeBig touches the contents.  This
     * makes the parser safe under arbitrary input bytes; the production
     * path still gates via envelope MAC for replay/authenticity, but a
     * synthetic byte-mutation fuzzer cannot induce an overread here. */
    if (payload == nullptr || out_new_id == nullptr) {
        return false;
    }
    if (!flexValidateBufferDeep(payload, len)) {
        return false;
    }
    const flex *f = (const flex *)payload;
    if (flexCount(f) != 1u) {
        return false;
    }
    flexEntry *fe = flexHead(f);
    if (fe == nullptr) {
        return false;
    }
    uint64_t v = 0;
    if (!flexGetUnsigned(fe, &v) || v > UINT32_MAX) {
        return false;
    }
    const hmacKeyId id = (hmacKeyId)v;
    if (id == HMAC_KEY_ID_INVALID) {
        return false;
    }
    *out_new_id = id;
    return true;
}

/* === Durable watermarks ============================================ */

/* Per persisted slot: 3 flex entries (origin_id, last_applied,
 * window_bits). Apply-rejected slots (occupied with last_applied==0)
 * are dropped — they carry no replay-protection state worth persisting
 * and would surface as malformed seq==0 records to a future Restore. */
#define CLUSTER_MAILBOX_PERSIST_ENTRIES_PER_SLOT ((size_t)3)

/* Build a flex containing every persisted slot. Caller owns and frees
 * via flexFree. Returns NULL on allocation failure (zmalloc would
 * have aborted already; defensive).
 *
 * Threading: snapshot is single-threaded (the mailbox owner serialises
 * snapshot calls per the header contract), but the per-slot atomic
 * fields are still accessed via atomic loads so the underlying
 * `_Atomic` type's access rules are respected. */
static flex *cmBuildSnapshotFlex(const clusterMailbox *mb) {
    /* @own: caller; free=flexFree */
    flex *f = flexNew();
    /* The mode first (strict or replay window), which the persist
     * restore's mailbox takes */
    flexRow mode = FLEX_ROW_INIT;
    flexRowUnsigned(&mode, (uint64_t)mb->flags);
    flexRowAppendExact(&f, &mode);
    for (size_t i = 0; i < CLUSTER_MAILBOX_ORIGIN_CAP; i++) {
        const clusterMailboxOrigin *o = &mb->origins[i];
        /* @sync: relaxed owner */
        const uint64_t la =
            atomic_load_explicit(&o->last_applied, memory_order_relaxed);
        if (la == 0u) {
            continue;
        }
        /* @sync: relaxed owner */
        const uint32_t oid =
            atomic_load_explicit(&o->origin_id, memory_order_relaxed);
        flexRow row = FLEX_ROW_INIT;
        flexRowUnsigned(&row, (uint64_t)oid);
        flexRowUnsigned(&row, la);
        flexRowUnsigned(&row, o->window_bits);
        flexRowAppendExact(&f, &row);
    }
    return f;
}

size_t clusterMailboxSnapshotSize(const clusterMailbox *mb) {
    if (mb == nullptr) {
        return 0;
    }
    flex *f = cmBuildSnapshotFlex(mb);
    const size_t n = flexBytes(f);
    flexFree(f);
    return n;
}

bool clusterMailboxSnapshot(const clusterMailbox *mb, uint8_t *out,
                            size_t out_cap, size_t *out_written) {
    if (mb == nullptr || out == nullptr) {
        return false;
    }
    flex *f = cmBuildSnapshotFlex(mb);
    const size_t need = flexBytes(f);
    if (out_cap < need) {
        flexFree(f);
        return false;
    }
    memcpy(out, f, need);
    flexFree(f);
    if (out_written != nullptr) {
        *out_written = need;
    }
    return true;
}

/* Window-bit orientation policy (Option B — atomic carry + frame
 * alignment). Background and rationale:
 *
 * `window_bits` is a per-origin 64-bit replay bitmap whose orientation is
 * relative to `last_applied`: bit 0 == `last_applied` itself, bit i ==
 * `last_applied - i`. When Restore MAX-merges two (last_applied,
 * window_bits) pairs that disagree on `last_applied`, the bits MUST be
 * re-oriented to a common frame before they can be OR'd — otherwise:
 *
 * - Naive "drop snapshot bits when snapshot.last_applied <
 * current.last_applied" silently loses replay state. Concrete:
 * mailbox has last_applied=80, never saw seq 25; snapshot says
 * last_applied=30 with bit 5 set (saw seq 25). After Restore an
 * attacker re-sends seq 25 — the in-memory window has the bit clear
 * (25 is in current's window: age=55, < 64), so the frame is wrongly
 * accepted as a fresh in-window out-of-order delivery. Silent
 * replay-protection downgrade.
 *
 * - Naive "replace bits when snapshot.last_applied >
 * current.last_applied" loses bits the in-memory mailbox accumulated
 * between snapshot and restore. Same downgrade, opposite direction.
 *
 * The fix below aligns both frames to the higher last_applied and OR's
 * the bitmaps. Bits whose absolute sequence is < (max_last_applied -
 * 63) are dropped — they reference sequences outside the discriminable
 * window in the merged frame and would always be rejected as
 * "ancient replay" by the (age >= WINDOW_BITS) check in Ingest, so the
 * loss is information-preserving (no false-accept introduced).
 *
 * `last_applied` itself is still MAX-merged so a stale snapshot cannot
 * rewind the watermark backwards — that invariant is preserved. */
static uint64_t cmReorientWindowBits(uint64_t bits, uint64_t from_last_applied,
                                     uint64_t to_last_applied) {
    /* Re-express `bits` (oriented to from_last_applied) into the
     * coordinate frame of to_last_applied. Requires to_last_applied >=
     * from_last_applied (caller ensures). The shift advances the
     * watermark by `gap`, moving each bit i to bit i+gap; bits that
     * cross bit 63 fall off the end. */
    const uint64_t gap = to_last_applied - from_last_applied;
    if (gap >= CLUSTER_MAILBOX_REPLAY_WINDOW_BITS) {
        return 0;
    }
    return bits << gap;
}

/* clusterMailboxRestore; 'takeMode' also gives the mailbox the snapshot's
 * mode (the persist restore's fresh mailbox), else it keeps its own */
static bool cmRestore_(clusterMailbox *mb, const uint8_t *buf, size_t len,
                       bool takeMode) {
    if (mb != nullptr && mb->applying) {
        return false; /* from inside on_apply */
    }
    /* Restore consumes a flex buffer from on-disk persistence.  Even
     * though the persistence layer wraps its writes in its own checksum,
     * this entry point can be called with arbitrary bytes during testing
     * and recovery — use the O(N) deep validator so a corrupted on-disk
     * record (or a synthetic byte-tamper) cannot make the per-entry
     * walks below over-read the buffer. */
    if (mb == nullptr || buf == nullptr) {
        return false;
    }
    if (!flexValidateBufferDeep(buf, len)) {
        return false;
    }
    const flex *f = (const flex *)buf;
    const size_t entry_count = flexCount(f);
    if (entry_count == 0 ||
        ((entry_count - 1) % CLUSTER_MAILBOX_PERSIST_ENTRIES_PER_SLOT) != 0u) {
        return false;
    }
    const size_t slots =
        (entry_count - 1) / CLUSTER_MAILBOX_PERSIST_ENTRIES_PER_SLOT;
    if (slots > CLUSTER_MAILBOX_ORIGIN_CAP) {
        return false;
    }

    /* Restore is all or nothing: every record is parsed and validated,
     * and the origin table checked for room, before the first one is
     * merged, so a refused snapshot leaves the mailbox untouched. */
    struct {
        uint32_t origin_id;
        uint64_t seq;
        uint64_t window_bits;
    } recs[CLUSTER_MAILBOX_ORIGIN_CAP];

    /* The mode: only known flags */
    flexEntry *fe = flexHead(f);
    uint64_t raw_mode = 0;
    if (!flexGetUnsigned(fe, &raw_mode) ||
        (raw_mode & ~(uint64_t)CLUSTER_MAILBOX_FLAG_REPLAY_WINDOW) != 0u) {
        return false;
    }
    fe = flexNext(f, fe);
    for (size_t i = 0; i < slots; i++) {
        if (fe == nullptr) {
            return false;
        }
        uint64_t raw_origin = 0;
        if (!flexGetUnsigned(fe, &raw_origin) || raw_origin == 0 ||
            raw_origin > UINT32_MAX) {
            return false;
        }
        recs[i].origin_id = (uint32_t)raw_origin;
        fe = flexNext(f, fe);
        if (fe == nullptr) {
            return false;
        }

        if (!flexGetUnsigned(fe, &recs[i].seq) || recs[i].seq == 0) {
            return false;
        }
        fe = flexNext(f, fe);
        if (fe == nullptr) {
            return false;
        }

        if (!flexGetUnsigned(fe, &recs[i].window_bits)) {
            return false;
        }
        fe = flexNext(f, fe);
    }

    /* Every origin the snapshot names that the table lacks (counted once
     * when the snapshot repeats it) needs a free slot */
    size_t free_slots = 0;
    for (size_t i = 0; i < CLUSTER_MAILBOX_ORIGIN_CAP; i++) {
        /* Restore is serialised against the writer, and only writer-side
         * code stores origin_id. */
        /* @sync: relaxed owner */
        if (atomic_load_explicit(&mb->origins[i].origin_id,
                                 memory_order_relaxed) == 0u) {
            free_slots++;
        }
    }

    size_t new_origins = 0;
    for (size_t i = 0; i < slots; i++) {
        bool repeated = false;
        for (size_t j = 0; j < i && !repeated; j++) {
            repeated = recs[j].origin_id == recs[i].origin_id;
        }

        if (!repeated && cmLookupOriginMut(mb, recs[i].origin_id) == nullptr) {
            new_origins++;
        }
    }

    if (new_origins > free_slots) {
        return false;
    }

    /* Merge every record, MAX-merging with any pre-existing watermark.
     * MAX preserves the strict "only forward" semantic — a stale
     * snapshot can never RESTORE last_applied backwards. The bitmap
     * is re-oriented to the merged frame (the higher last_applied) and
     * OR'd; see cmReorientWindowBits for the orientation policy.
     * Nothing below can fail: the table has room for every insert. */
    for (size_t i = 0; i < slots; i++) {
        const uint32_t origin_id = recs[i].origin_id;
        const uint64_t seq = recs[i].seq;
        const uint64_t window_bits = recs[i].window_bits;

        clusterMailboxOrigin *slot = cmLookupOriginMut(mb, origin_id);
        const bool fresh_slot = (slot == nullptr);
        if (slot == nullptr) {
            slot = cmInsertOrigin(mb, origin_id);
            if (slot == nullptr) {
                return false; /* unreachable: room was checked above */
            }
        }
        /* Restore is serialised against the writer per the header
         * contract, so the load reads back writer-side state. The
         * last_applied stores below stay release: they replace Ingest's
         * release stores in last_applied's modification order, and an
         * observer whose acquire reads a Restore-written value still
         * synchronises with everything before it (the caller's
         * serialisation makes prior Ingest applies part of that). */
        /* @sync: relaxed owner */
        const uint64_t cur_la =
            atomic_load_explicit(&slot->last_applied, memory_order_relaxed);
        if (fresh_slot || cur_la == 0u) {
            /* Brand-new slot (or slot occupied by an APPLY_REJECTED
             * record with last_applied==0): adopt snapshot wholesale. */
            slot->window_bits = window_bits;
            /* @sync: release mailbox.watermark */
            atomic_store_explicit(&slot->last_applied, seq,
                                  memory_order_release);
        } else if (seq >= cur_la) {
            /* Snapshot is at or ahead of current: re-orient the
             * pre-existing in-memory bits forward into the snapshot's
             * frame, then OR with snapshot bits, then adopt the
             * snapshot's last_applied. */
            const uint64_t current_bits_in_snap_frame =
                cmReorientWindowBits(slot->window_bits, cur_la, seq);
            slot->window_bits = current_bits_in_snap_frame | window_bits;
            /* @sync: release mailbox.watermark */
            atomic_store_explicit(&slot->last_applied, seq,
                                  memory_order_release);
        } else {
            /* Snapshot is strictly behind current: keep current's
             * last_applied (monotonic invariant) but re-orient the
             * snapshot's bits forward into current's frame and OR
             * them in. Without this, an attacker who replays a
             * sequence the snapshot recorded as "seen" but the current
             * in-memory window has as clear would be accepted as a
             * fresh in-window out-of-order delivery. */
            const uint64_t snap_bits_in_current_frame =
                cmReorientWindowBits(window_bits, seq, cur_la);
            slot->window_bits |= snap_bits_in_current_frame;
        }
    }
    if (takeMode) {
        mb->flags = (clusterMailboxFlags)raw_mode;
    }
    return true;
}

bool clusterMailboxRestore(clusterMailbox *mb, const uint8_t *buf, size_t len) {
    return cmRestore_(mb, buf, len, false);
}

/* --- Envelope-wrapped snapshot/restore ----------------------------- */

/* A wrapped snapshot is a standalone artifact: inside the envelope, the
 * watermark bytes follow the dkFormat prefix of the descriptor's format */
static dkFormatStamp cmWrappedStamp(void) {
    const dkFormatSpec spec = persistOpsFormat(&persistOpsClusterMailbox);
    return dkFormatStampFor(&spec, 0);
}

size_t clusterMailboxSnapshotWrappedSize(const clusterMailbox *mb,
                                         flexEnvelopeAlgo envelope_algo) {
    if (mb == nullptr) {
        return 0;
    }
    const dkFormatStamp stamp = cmWrappedStamp();
    const size_t inner =
        dkFormatPrefixSize(&stamp) + clusterMailboxSnapshotSize(mb);
    return flexEnvelopeWrappedSize((uint64_t)inner, envelope_algo,
                                   FLEX_ENV_FLAG_NONE);
}

bool clusterMailboxSnapshotWrapped(const clusterMailbox *mb,
                                   const flexEnvelopeWrapParams *params,
                                   uint8_t *out, size_t out_cap,
                                   size_t *out_written) {
    if (mb == nullptr || params == nullptr || out == nullptr) {
        return false;
    }
    /* Reject non-keyed and AEAD algos at the wrapper level — same
     * security stance as clusterMailboxCreate. The plain Wrap
     * function would already reject AEAD, but checksum algos would
     * silently produce a tamper-detectable but tamper-forgeable
     * snapshot; require a keyed algo here. */
    if (!flexEnvelopeAlgoIsKeyed(params->algo) ||
        FLEX_ENV_ALGO_IS_AEAD(params->algo)) {
        return false;
    }
    const dkFormatStamp stamp = cmWrappedStamp();
    const size_t prefix_size = dkFormatPrefixSize(&stamp);
    const size_t inner_size = prefix_size + clusterMailboxSnapshotSize(mb);
    /* inner frees itself at scope exit */
    cleaner(zfree) void *inner = zmalloc(inner_size);
    uint8_t *const bytes = inner;
    size_t inner_written = 0;
    if (dkFormatPrefixWrite(&stamp, bytes, prefix_size) != prefix_size ||
        !clusterMailboxSnapshot(mb, bytes + prefix_size,
                                inner_size - prefix_size, &inner_written) ||
        prefix_size + inner_written != inner_size) {
        return false;
    }
    size_t written = 0;
    const bool ok = flexEnvelopeWrap(out, out_cap, inner, (uint64_t)inner_size,
                                     params, &written);
    if (!ok) {
        return false;
    }
    if (out_written != nullptr) {
        *out_written = written;
    }
    return true;
}

bool clusterMailboxRestoreWrapped(clusterMailbox *mb,
                                  const flexEnvelopeVerifyParams *params,
                                  const uint8_t *buf, size_t len) {
    if (mb == nullptr || buf == nullptr) {
        return false;
    }
    const uint8_t *payload = nullptr;
    uint64_t payload_len = 0;
    if (!flexEnvelopeVerify(buf, len, params, &payload, &payload_len)) {
        return false;
    }

    /* Only a watermark format this build reads is parsed */
    const dkFormatSpec spec = persistOpsFormat(&persistOpsClusterMailbox);
    dkFormatStamp stamp;
    size_t prefix = 0;
    if (dkFormatPrefixRead(&spec, payload, (size_t)payload_len, &stamp,
                           &prefix) != DK_FORMAT_OK) {
        return false;
    }
    return clusterMailboxRestore(mb, payload + prefix,
                                 (size_t)payload_len - prefix);
}

/* ---- persistOps descriptor ----------------------------------------- */

static uint8_t *cmPersistSnapshot(const void *structure, size_t *len) {
    /* The snapshot flex is the persisted bytes; the caller frees it */
    flex *f = cmBuildSnapshotFlex((const clusterMailbox *)structure);
    *len = flexBytes(f);
    return (uint8_t *)f;
}

/* Internal allocator for a "restore shell" mailbox: a mailbox with no
 * bound keyring, no apply callback, and `envelope_algo = FLEX_ENV_ALGO_NONE`.
 * Such a mailbox is sufficient for Snapshot and Restore (which only
 * touch the `origins[]` watermark table) but MUST NOT be used for
 * Seal or Ingest until the caller wires it up via
 * `clusterMailboxAttachKeyring` + the on_apply parameters. Snapshot
 * and Restore deliberately do not consult the ring/algo/apply fields,
 * so the shell is fully functional for those paths.
 *
 * This bypasses the validation in `clusterMailboxCreateEx` (which
 * rejects NULL ring) — that validation enforces "you cannot operate
 * a mailbox without a keyring", which is correct for the public API
 * but inapplicable to the deserialise-only flow used by the persist
 * framework's `restore` hook. */
static clusterMailbox *cmCreateRestoreShell(void) {
    /* @own: caller; free=clusterMailboxFree */
    clusterMailbox *mb = zmalloc(sizeof(*mb));
    memset(mb, 0, sizeof(*mb));
    mb->ring = nullptr;
    mb->on_apply = nullptr;
    mb->apply_ctx = nullptr;
    mb->envelope_algo = FLEX_ENV_ALGO_NONE;
    mb->flags = CLUSTER_MAILBOX_FLAG_NONE;
    return mb;
}

/* Restore semantics for the persist framework: allocates a fresh
 * "restore shell" mailbox (no bound keyring/algo/apply callback) and
 * populates it from `data`. Caller takes ownership and frees via
 * `persistOpsClusterMailbox.free` (which forwards to
 * `clusterMailboxFree`).
 *
 * The returned mailbox is sufficient for inspecting recovered
 * watermarks (clusterMailboxLastApplied, clusterMailboxOriginCount,
 * clusterMailboxGetStats) and for taking further snapshots, but is
 * NOT yet usable for live Seal/Ingest traffic — those require an
 * attached keyring + algo + apply callback that only the caller can
 * supply. See `clusterMailboxAttachKeyring` for the post-restore
 * wire-up path. */
static void *cmPersistRestore(const uint8_t *data, size_t len) {
    if (data == nullptr) {
        return nullptr;
    }
    clusterMailbox *mb = cmCreateRestoreShell();
    if (mb == nullptr) {
        return nullptr;
    }
    /* A fresh mailbox: it takes the snapshot's mode with its state */
    if (!cmRestore_(mb, data, len, true)) {
        clusterMailboxFree(mb);
        return nullptr;
    }
    return mb;
}

/* Allocate an empty "restore shell" mailbox for the persist framework's
 * WAL-replay path (which constructs an empty target via newEmpty when
 * the WAL has no leading snapshot). Mirrors the contract documented on
 * cmPersistRestore: the returned mailbox carries no keyring/algo/apply
 * callback and is suitable only for watermark inspection / further
 * snapshot, NOT for Seal/Ingest until the caller attaches a keyring. */
static void *cmPersistNewEmpty(void) {
    return cmCreateRestoreShell();
}

/* Persisted watermarks: origins with an applied sequence (an origin
 * whose only frame was apply-rejected holds a slot but no watermark,
 * and is not persisted) */
static size_t cmPersistCount(const void *structure) {
    const clusterMailbox *mb = (const clusterMailbox *)structure;
    size_t n = 0;
    for (size_t i = 0; i < CLUSTER_MAILBOX_ORIGIN_CAP; i++) {
        /* @sync: relaxed owner */
        n += atomic_load_explicit(&mb->origins[i].last_applied,
                                  memory_order_relaxed) != 0u;
    }
    return n;
}

static size_t cmPersistEstimateSize(const void *structure) {
    return clusterMailboxSnapshotSize((const clusterMailbox *)structure);
}

static void cmPersistFree(void *structure) {
    clusterMailboxFree((clusterMailbox *)structure);
}

const persistOps persistOpsClusterMailbox = {
    .type = PERSIST_TYPE_CLUSTER_MAILBOX_WATERMARKS,
    .name = "clusterMailboxWatermarks",
    .formatVersion = 1,
    .minReadVersion = 1,
    .snapshot = cmPersistSnapshot,
    .restore = cmPersistRestore,
    .newEmpty = cmPersistNewEmpty,
    .count = cmPersistCount,
    .estimateSize = cmPersistEstimateSize,
    .encodeOp = nullptr,
    .applyOp = nullptr,
    .streamSnapshot = nullptr,
    .streamRestore = nullptr,
    .validate = nullptr,
    .free = cmPersistFree,
};

/* ============================================================================
 * Test driver
 * ============================================================================
 */

#ifdef DATAKIT_TEST

#include "ctest.h"

#include <pthread.h>
#include <sched.h> /* sched_yield — retire/reinstall race test */

defcleaner(clusterMailbox *, clusterMailboxFree);
defcleaner(hmacKeyring *, hmacKeyringFree);

/* Apply callback that records the most recent frame's contents into
 * caller-owned storage and counts the calls. */
typedef struct {
    uint32_t origin_id;
    uint64_t sequence;
    uint8_t msg_type;
    uint8_t _pad[3];
    uint32_t apply_count;
    uint32_t reject_threshold; /* if > 0 and apply_count >= threshold,
                                * the callback returns false */
    size_t last_payload_len;
    uint8_t last_payload[256];
} cmTestCtx;

/* An on_apply that tries to ingest another frame, and to restore, into
 * its own mailbox */
typedef struct cmReentryCtx {
    clusterMailbox *mb;
    hmacKeyId key;
    uint8_t *next;
    size_t nextLen;
    clusterMailboxIngestResult nested;
    bool restored;
} cmReentryCtx;

static bool cmReentryApply(const clusterFrame *frame, void *ctx) {
    (void)frame;
    cmReentryCtx *re = ctx;
    re->nested = clusterMailboxIngest(re->mb, re->next, re->nextLen, re->key);
    const uint8_t empty[1] = {0};
    re->restored = clusterMailboxRestore(re->mb, empty, sizeof(empty));
    return true;
}

static bool cmRecordApply(const clusterFrame *frame, void *ctx) {
    cmTestCtx *t = (cmTestCtx *)ctx;
    if (t->reject_threshold != 0 && t->apply_count >= t->reject_threshold) {
        return false;
    }
    t->origin_id = frame->origin_id;
    t->sequence = frame->sequence;
    t->msg_type = frame->msg_type;
    t->last_payload_len = frame->payload_len;
    const size_t n = frame->payload_len < sizeof(t->last_payload)
                         ? frame->payload_len
                         : sizeof(t->last_payload);
    if (n > 0) {
        memcpy(t->last_payload, frame->payload, n);
    }
    t->apply_count++;
    return true;
}

/* Wrap raw watermark bytes as clusterMailboxSnapshotWrapped does (format
 * prefix, then the bytes, inside the envelope); false when `cap` is short */
static bool cmTestWrapStamped(uint8_t *out, size_t cap, const uint8_t *raw,
                              size_t rawLen,
                              const flexEnvelopeWrapParams *params,
                              size_t *written) {
    const dkFormatStamp stamp = cmWrappedStamp();
    uint8_t inner[512];
    const size_t prefix = dkFormatPrefixWrite(&stamp, inner, sizeof(inner));
    if (prefix == 0 || prefix + rawLen > sizeof(inner)) {
        return false;
    }
    memcpy(inner + prefix, raw, rawLen);
    return flexEnvelopeWrap(out, cap, inner, prefix + rawLen, params, written);
}

/* flexTestReplaceEntry with a 96/128-bit replacement, checked to be
 * stored wide, so a refusal is about the width and not a narrowed value */
static flex *cmTestReplaceEntry(const flex *src, size_t idx,
                                const databoxBig *wide) {
    /* @own: caller; free=flexFree */
    flex *out = flexTestReplaceEntry(src, idx, wide);
    if (idx != SIZE_MAX) {
        databoxBig check;
        flexGetByTypeBig(flexIndex(out, (int32_t)idx), &check);
        assert(check.big);
    }
    return out;
}

/* 2^64, 2^127 + 1 and -2^100: the 96-bit and 128-bit unsigned encodings
 * and a negative one. */
static constexpr unsigned CM_TEST_WIDE_COUNT = 3u;
static void cmTestWideValues(databoxBig wides[CM_TEST_WIDE_COUNT]) {
    DATABOX_BIG_UNSIGNED_128(&wides[0], (__uint128_t)1 << 64);
    DATABOX_BIG_UNSIGNED_128(&wides[1], ((__uint128_t)1 << 127) | 1u);
    DATABOX_BIG_SIGNED_128(&wides[2], -((__int128_t)1 << 100));
}

/* Seal 'inner' (a header flex followed by 'payload') the way
 * clusterMailboxSeal does, so Ingest verifies the MAC and reaches the
 * inner-header decoder. */
static size_t cmTestSealInner(const flex *inner, const uint8_t *key,
                              size_t key_len, uint8_t *out, size_t out_cap) {
    uint8_t frame[128];
    const size_t hlen = flexBytes(inner);
    const uint8_t payload[3] = {'a', 'b', 'c'};
    assert(hlen + sizeof(payload) <= sizeof(frame));
    memcpy(frame, inner, hlen);
    memcpy(frame + hlen, payload, sizeof(payload));
    const flexEnvelopeWrapParams wp = {
        .key = key,
        .key_len = key_len,
        .algo = FLEX_ENV_ALGO_HMAC_SHA256,
        .on_wire_flags = FLEX_ENV_FLAG_NONE,
    };
    size_t written = 0;
    if (!flexEnvelopeWrap(out, out_cap, frame, hlen + sizeof(payload), &wp,
                          &written)) {
        return 0;
    }
    return written;
}

/* ====================================================================
 * The model test: a producer seals frames for many origins and the
 * consumer ingests them, against a model of the consumer's watermarks
 * and replay windows (strict or windowed), every rejection's result
 * and counter checked, through faults (a flipped tag, an absent,
 * retired or revoked key, another algorithm), apply refusals and
 * re-entry, and snapshots restored fresh or merged back stale
 * ==================================================================== */
#include "dkRandom.h"

static constexpr uint32_t CMB_ORIGINS = 80; /* past the 64-slot table */
static constexpr uint32_t CMB_KEYS = 6;

typedef struct cmbOrigin_ {
    bool tracked;
    uint64_t last;
    uint64_t bits; /* bit i: last - i was applied */
} cmbOrigin_;

typedef struct cmbModel_ {
    hmacKeyring *ring;
    clusterMailbox *producer;
    clusterMailbox *other; /* seals under another algorithm */
    clusterMailbox *consumer;
    bool windowed;
    cmbOrigin_ o[CMB_ORIGINS + 1];
    uint32_t tracked;
    bool keyIn[CMB_KEYS + 1];
    hmacKeyId floor;
    bool accept;       /* what the apply callback answers */
    bool reenter;      /* the apply callback tries to ingest and restore */
    bool applied;      /* the callback ran */
    clusterFrame seen; /* the frame it saw */
    uint8_t seenPayload[64];
    clusterMailboxStats want;
    const char *why;
} cmbModel_;

static void cmbModelFree_(cmbModel_ *M) {
    if (M) {
        clusterMailboxFree(M->consumer);
        clusterMailboxFree(M->producer);
        clusterMailboxFree(M->other);
        hmacKeyringFree(M->ring);
        zfree(M);
    }
}

defcleaner(cmbModel_ *, cmbModelFree_);
defcleanerName(cmbOrigin_ *, zfree, Origins);

static void cmbKey_(hmacKeyId id, uint8_t key[32]) {
    for (uint32_t i = 0; i < 32; i++) {
        key[i] = (uint8_t)(id * 31 + i);
    }
}

static bool cmbApply_(const clusterFrame *frame, void *ctx) {
    cmbModel_ *M = ctx;
    M->applied = true;
    M->seen = *frame;
    memcpy(M->seenPayload, frame->payload,
           frame->payload_len < sizeof(M->seenPayload)
               ? frame->payload_len
               : sizeof(M->seenPayload));
    if (M->reenter) {
        /* Neither ingest nor restore reaches the mailbox mid-apply */
        uint8_t junk[8] = {};
        if (clusterMailboxIngest(M->consumer, junk, sizeof(junk), 1) !=
                CLUSTER_MAILBOX_ERR_PARAM ||
            clusterMailboxRestore(M->consumer, junk, sizeof(junk))) {
            M->why = "a re-entry from the apply callback was let in";
        }
        M->want.ingest_total++;
        M->want.param_error++;
        M->want.bytes_ingested += sizeof(junk);
    }
    return M->accept;
}

/* The model's verdict for a good frame at (origin, seq), applied or not */
static clusterMailboxIngestResult cmbVerdict_(const cmbModel_ *M,
                                              uint32_t origin, uint64_t seq) {
    const cmbOrigin_ *o = &M->o[origin];
    if (!o->tracked && M->tracked == CLUSTER_MAILBOX_ORIGIN_CAP) {
        return CLUSTER_MAILBOX_ERR_ORIGIN_TABLE_FULL;
    }
    if (seq > o->last) {
        return CLUSTER_MAILBOX_OK;
    }
    if (!M->windowed || o->last - seq >= 64 ||
        (o->bits >> (o->last - seq)) & 1) {
        return CLUSTER_MAILBOX_ERR_REPLAY;
    }
    return CLUSTER_MAILBOX_OK;
}

static void cmbApplyModel_(cmbModel_ *M, uint32_t origin, uint64_t seq) {
    cmbOrigin_ *o = &M->o[origin];
    if (!o->tracked) {
        o->tracked = true;
        M->tracked++;
    }
    if (seq > o->last) {
        const uint64_t shift = seq - o->last;
        o->bits = shift >= 64 ? 0 : o->bits << shift;
        o->bits |= 1;
        o->last = seq;
    } else {
        o->bits |= UINT64_C(1) << (o->last - seq);
    }
    if (!M->windowed) {
        o->bits = UINT64_MAX; /* strict: the whole window reads as seen */
    }
}

/* A snapshot's watermarks merged in: per origin the later last wins, the
 * windows re-oriented to it and joined */
static void cmbMergeModel_(cmbModel_ *M, const cmbOrigin_ *snap) {
    for (uint32_t i = 1; i <= CMB_ORIGINS; i++) {
        const cmbOrigin_ *s = &snap[i];
        cmbOrigin_ *o = &M->o[i];
        if (!s->tracked || !s->last) {
            continue;
        }
        if (!o->tracked) {
            o->tracked = true;
            M->tracked++;
        }
        if (s->last > o->last) {
            const uint64_t shift = s->last - o->last;
            o->bits = (shift >= 64 ? 0 : o->bits << shift) | s->bits;
            o->last = s->last;
        } else {
            const uint64_t shift = o->last - s->last;
            /* (the older window's bit i is sequence s->last - i, so
             * bit i + shift here) */
            o->bits |= shift >= 64 ? 0 : s->bits << shift;
        }
        if (!M->windowed) {
            o->bits = UINT64_MAX;
        }
    }
}

/* @model: src/clusterMailbox.h */
/* The consumer's watermarks and counters are the model's */
static bool cmbSame_(cmbModel_ *M, const clusterMailbox *mb) {
    if (clusterMailboxOriginCount(mb) != M->tracked) {
        M->why = "the origin count";
        return false;
    }
    for (uint32_t i = 1; i <= CMB_ORIGINS; i++) {
        if (clusterMailboxLastApplied(mb, i) != M->o[i].last) {
            M->why = "an origin's last applied";
            return false;
        }
    }
    clusterMailboxStats st = {};
    clusterMailboxGetStats(mb, &st);
    if (memcmp(&st, &M->want, sizeof(st)) != 0) {
        M->why = "the counters";
        return false;
    }
    return true;
}

/* @model: src/clusterMailbox.h */
/* One frame sealed and ingested, a fault in it now and then */
static bool cmbFrame_(cmbModel_ *M, uint64_t r) {
    const uint32_t origin = 1 + (uint32_t)(r % CMB_ORIGINS);
    const cmbOrigin_ *o = &M->o[origin];
    /* Next, a replay, an older one inside the window, or far ahead */
    const uint64_t pick = (r >> 8) % 8;
    const uint64_t seq = pick < 4 || !o->last ? o->last + 1 + (r >> 12) % 3
                         : pick < 5           ? o->last
                         : pick < 7 ? (o->last > 70 ? o->last - (r >> 12) % 70
                                                    : 1 + (r >> 12) % o->last)
                                    : o->last + 200;
    const hmacKeyId key = (hmacKeyId)(1 + (r >> 20) % CMB_KEYS);
    const uint8_t type = (uint8_t)((r >> 24) % 200);
    uint8_t payload[48];
    const size_t plen = (r >> 32) % sizeof(payload);
    for (size_t i = 0; i < plen; i++) {
        payload[i] = (uint8_t)(r >> (i % 56));
    }
    const uint64_t fault = (r >> 40) % 12;
    clusterMailbox *sealer = fault == 1 ? M->other : M->producer;
    const size_t cap = clusterMailboxSealedSize(sealer, plen);
    uint8_t out[512];
    size_t wrote = 0;
    /* A seal refused: sequence 0, or a buffer one byte short */
    const bool sealable = M->keyIn[key] && key >= M->floor;
    if (clusterMailboxSeal(sealer, origin, 0, type, payload, plen, key, out,
                           sizeof(out), &wrote) ||
        (sealable &&
         clusterMailboxSeal(sealer, origin, seq, type, payload, plen, key, out,
                            cap - 1, &wrote) &&
         wrote > cap - 1)) {
        M->why = "a seal past its refusals";
        return false;
    }
    const bool sealed = clusterMailboxSeal(sealer, origin, seq, type, payload,
                                           plen, key, out, cap, &wrote);
    if (sealed != sealable) {
        M->why = "a seal under a key";
        return false;
    }
    if (!sealed) {
        return true;
    }
    /* The fault, and the result it must give */
    hmacKeyId ingestKey = key;
    clusterMailboxIngestResult want;
    if (fault == 1) {
        want = CLUSTER_MAILBOX_ERR_ALGO_MISMATCH;
    } else if (fault == 2) {
        out[wrote - 1] ^= 0x40; /* the tag */
        want = CLUSTER_MAILBOX_ERR_VERIFY_FAILED;
    } else if (fault == 3) {
        ingestKey = CMB_KEYS + 3; /* never installed */
        want = CLUSTER_MAILBOX_ERR_KEY_MISSING;
    } else {
        want = cmbVerdict_(M, origin, seq);
    }
    M->accept = (r >> 50) % 6 != 0;
    M->reenter = (r >> 53) % 16 == 0;
    M->applied = false;
    const clusterMailboxIngestResult got =
        clusterMailboxIngest(M->consumer, out, wrote, ingestKey);
    if (want == CLUSTER_MAILBOX_OK && !M->accept) {
        want = CLUSTER_MAILBOX_ERR_APPLY_REJECTED;
    }
    M->want.ingest_total++;
    M->want.bytes_ingested += wrote;
    switch (want) {
    case CLUSTER_MAILBOX_OK:
        M->want.apply_ok++;
        break;
    case CLUSTER_MAILBOX_ERR_APPLY_REJECTED:
        M->want.apply_rejected++;
        break;
    case CLUSTER_MAILBOX_ERR_REPLAY:
        M->want.replay_rejected++;
        break;
    case CLUSTER_MAILBOX_ERR_ALGO_MISMATCH:
        M->want.algo_mismatch++;
        break;
    case CLUSTER_MAILBOX_ERR_VERIFY_FAILED:
        M->want.verify_failed++;
        break;
    case CLUSTER_MAILBOX_ERR_KEY_MISSING:
        M->want.key_missing++;
        break;
    case CLUSTER_MAILBOX_ERR_KEY_REVOKED:
        M->want.key_revoked++;
        break;
    case CLUSTER_MAILBOX_ERR_ORIGIN_TABLE_FULL:
        M->want.origin_table_full++;
        break;
    default:
        break;
    }
    if (got != want) {
        static char why[128];
        snprintf(why, sizeof(why),
                 "ingest of origin %" PRIu32 " seq %" PRIu64 " (last %" PRIu64
                 ", fault %" PRIu64 "): %d, want %d",
                 origin, seq, o->last, fault, (int)got, (int)want);
        M->why = why;
        return false;
    }
    /* The callback ran exactly for frames that reached it, and saw them */
    const bool reached = want == CLUSTER_MAILBOX_OK ||
                         want == CLUSTER_MAILBOX_ERR_APPLY_REJECTED;
    if (M->applied != reached ||
        (reached && (M->seen.origin_id != origin || M->seen.sequence != seq ||
                     M->seen.msg_type != type || M->seen.payload_len != plen ||
                     memcmp(M->seenPayload, payload, plen) != 0))) {
        M->why = "the frame the callback saw";
        return false;
    }
    if (want == CLUSTER_MAILBOX_OK) {
        cmbApplyModel_(M, origin, seq);
    }
    return true;
}

/* @model: src/clusterMailbox.h */
/* The consumer's snapshot, plain or wrapped, in a fresh mailbox that
 * then takes the consumer's place; and an older snapshot merged back */
static bool cmbRebuild_(cmbModel_ *M, cmbOrigin_ *stale, uint8_t **staleBuf,
                        size_t *staleLen, uint64_t r) {
    const bool wrapped = (r >> 8) % 2;
    uint8_t key[32];
    cmbKey_(1, key);
    const flexEnvelopeWrapParams wp = {
        .key = key, .key_len = sizeof(key), .algo = FLEX_ENV_ALGO_HMAC_SHA256};
    const flexEnvelopeVerifyParams vp = {.max_payload_len =
                                             FLEX_ENVELOPE_DEFAULT_MAX_PAYLOAD,
                                         .key = key,
                                         .key_len = sizeof(key)};
    const size_t need =
        wrapped ? clusterMailboxSnapshotWrappedSize(M->consumer, wp.algo)
                : clusterMailboxSnapshotSize(M->consumer);
    cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(need ? need : 1);
    size_t wrote = 0;
    const bool took =
        wrapped
            ? clusterMailboxSnapshotWrapped(M->consumer, &wp, buf, need, &wrote)
            : clusterMailboxSnapshot(M->consumer, buf, need, &wrote);
    if (!took || wrote != need ||
        (!wrapped &&
         clusterMailboxSnapshot(M->consumer, buf, need - 1, &wrote))) {
        M->why = "a snapshot's size";
        return false;
    }
    clusterMailboxCreateOptions opts = {
        .ring = M->ring,
        .envelope_algo = FLEX_ENV_ALGO_HMAC_BLAKE3,
        .on_apply = cmbApply_,
        .apply_ctx = M,
        .flags = M->windowed ? CLUSTER_MAILBOX_FLAG_REPLAY_WINDOW : 0,
        .disable_nonce_log = (r >> 9) % 2,
    };
    clusterMailbox *fresh = clusterMailboxCreateOpts(&opts);
    if (!fresh ||
        (clusterMailboxNonceLog(fresh) != nullptr) == opts.disable_nonce_log) {
        clusterMailboxFree(fresh);
        M->why = "a fresh mailbox";
        return false;
    }
    /* A tampered wrapped snapshot changes nothing */
    if (wrapped) {
        buf[wrote / 2] ^= 1;
        const bool tamperTook =
            clusterMailboxRestoreWrapped(fresh, &vp, buf, wrote);
        buf[wrote / 2] ^= 1;
        if (tamperTook || clusterMailboxOriginCount(fresh) != 0) {
            clusterMailboxFree(fresh);
            M->why = "a tampered wrapped snapshot was taken";
            return false;
        }
    }
    const bool restored =
        wrapped ? clusterMailboxRestoreWrapped(fresh, &vp, buf, wrote)
                : clusterMailboxRestore(fresh, buf, wrote);
    if (!restored) {
        clusterMailboxFree(fresh);
        M->why = "a restore into a fresh mailbox";
        return false;
    }
    /* The fresh mailbox's counters start at zero; the model's follow */
    clusterMailboxFree(M->consumer);
    M->consumer = fresh;
    memset(&M->want, 0, sizeof(M->want));
    if (!cmbSame_(M, M->consumer)) {
        return false;
    }
    /* An older snapshot merged back: the later watermarks win, the
     * windows join */
    if (*staleBuf) {
        if (!clusterMailboxRestore(M->consumer, *staleBuf, *staleLen)) {
            M->why = "an older snapshot was refused";
            return false;
        }
        cmbMergeModel_(M, stale);
        if (!cmbSame_(M, M->consumer)) {
            return false;
        }
    }
    /* This one is the next older snapshot */
    zfree(*staleBuf);
    *staleLen = clusterMailboxSnapshotSize(M->consumer);
    /* @own: caller; free=zfree */
    *staleBuf = zmalloc(*staleLen);
    size_t staleWrote = 0;
    memcpy(stale, M->o, sizeof(M->o));
    return clusterMailboxSnapshot(M->consumer, *staleBuf, *staleLen,
                                  &staleWrote) &&
           staleWrote == *staleLen;
}

/* @model: src/clusterMailbox.h */
static int clusterMailboxTestModel_(uint64_t seed) {
    int err = 0;
    cleaner(cmbModelFree_) cmbModel_ *M = zcalloc(1, sizeof(*M));
    M->windowed = seed % 2;
    /* @own: container; free=hmacKeyringFree */
    M->ring = hmacKeyringCreate();
    for (hmacKeyId id = 1; id <= CMB_KEYS; id++) {
        uint8_t key[32];
        cmbKey_(id, key);
        M->keyIn[id] = hmacKeyringInstallUnsalted(M->ring, id, &hmacAlgoSHA256,
                                                  key, sizeof(key));
    }
    /* @own: container; free=clusterMailboxFree */
    M->producer = clusterMailboxCreate(M->ring, FLEX_ENV_ALGO_HMAC_BLAKE3,
                                       nullptr, nullptr);
    /* @own: container; free=clusterMailboxFree */
    M->other = clusterMailboxCreateEx(M->ring, FLEX_ENV_ALGO_HMAC_SHA256,
                                      nullptr, nullptr, 0);
    /* @own: container; free=clusterMailboxFree */
    M->consumer = clusterMailboxCreateEx(
        M->ring, FLEX_ENV_ALGO_HMAC_BLAKE3, cmbApply_, M,
        M->windowed ? CLUSTER_MAILBOX_FLAG_REPLAY_WINDOW : 0);
    cleanerName(zfree, Origins) cmbOrigin_ *stale =
        zcalloc(CMB_ORIGINS + 1, sizeof(*stale));
    cleanerName(zfree, Bytes) uint8_t *staleBuf = nullptr;
    size_t staleLen = 0;
    /* A frame sealed under key 1 now, for after it is revoked */
    size_t earlyLen = clusterMailboxSealedSize(M->producer, 4);
    cleanerName(zfree, Bytes) uint8_t *early = zmalloc(earlyLen);
    if (!clusterMailboxSeal(M->producer, 1, 1000000, 7, (const uint8_t *)"late",
                            4, 1, early, earlyLen, &earlyLen)) {
        ERR("%s", "the early frame was not sealed");
    }
    uint64_t rng = seed + 11;
    for (uint32_t step = 0; step < 6000 && !err; step++) {
        const uint64_t r = dkSplitmix64Next(&rng);
        const uint32_t op = (uint32_t)(r % 20);
        bool ok = true;
        M->why = "the call's result";
        if (op < 14) {
            ok = cmbFrame_(M, r >> 5);
        } else if (op == 14) { /* a key retired, or put back */
            const hmacKeyId id = (hmacKeyId)(1 + (r >> 8) % CMB_KEYS);
            uint8_t key[32];
            cmbKey_(id, key);
            if (M->keyIn[id]) {
                ok = hmacKeyringRetire(M->ring, id);
            } else {
                ok = hmacKeyringInstallUnsalted(M->ring, id, &hmacAlgoSHA256,
                                                key, sizeof(key));
            }
            M->keyIn[id] = !M->keyIn[id];
        } else if (op == 15 && (r >> 8) % 20 == 0 && M->floor < 3) {
            M->floor++; /* revoked below it: nothing seals under them */
            ok = hmacKeyringSetMinValidKeyId(M->ring, M->floor);
            if (ok && M->floor > 1 && M->keyIn[1] && early) {
                /* A frame sealed under key 1 before its revocation */
                ok = clusterMailboxIngest(M->consumer, early, earlyLen, 1) ==
                     CLUSTER_MAILBOX_ERR_KEY_REVOKED;
                M->want.ingest_total++;
                M->want.key_revoked++;
                M->want.bytes_ingested += earlyLen;
            }
        } else if (op == 16 && (r >> 8) % 10 == 0) {
            ok = cmbRebuild_(M, stale, &staleBuf, &staleLen, r);
        } else if (op == 17 && (r >> 8) % 10 == 0) {
            clusterMailboxResetStats(M->consumer);
            memset(&M->want, 0, sizeof(M->want));
        } else if (op == 18) { /* the keyring re-attached */
            ok = clusterMailboxAttachKeyring(M->consumer, M->ring,
                                             FLEX_ENV_ALGO_HMAC_BLAKE3,
                                             cmbApply_, M) &&
                 !clusterMailboxAttachKeyring(
                     M->consumer, M->ring, FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305,
                     cmbApply_, M);
        } else if (op == 19) { /* the rotation helpers, as a pure state */
            clusterRotationState st;
            const hmacKeyId a = (hmacKeyId)(1 + (r >> 8) % 9);
            const hmacKeyId b = a % 9 + 1;
            uint8_t ann[CLUSTER_MAILBOX_ROTATE_ANNOUNCE_PAYLOAD_LEN];
            const size_t annLen = clusterMailboxBuildRotateAnnounce(ann, b);
            hmacKeyId parsed = 0;
            ok = !clusterRotationInit(&st, HMAC_KEY_ID_INVALID) &&
                 clusterRotationInit(&st, a) &&
                 clusterRotationAccepts(&st, a) &&
                 !clusterRotationAccepts(&st, b) &&
                 !clusterRotationPromote(&st, a) &&
                 clusterRotationPromote(&st, b) && st.primary == b &&
                 clusterRotationAccepts(&st, a) &&
                 clusterRotationAccepts(&st, b) && annLen > 0 &&
                 annLen <= sizeof(ann) &&
                 clusterMailboxParseRotateAnnounce(ann, annLen, &parsed) &&
                 parsed == b &&
                 !clusterMailboxParseRotateAnnounce(ann, annLen - 1, &parsed);
            clusterRotationRetirePrevious(&st);
            ok = ok && !clusterRotationAccepts(&st, a) &&
                 clusterRotationAccepts(&st, b);
        }
        if (ok && step % 40 == 39 && !cmbSame_(M, M->consumer)) {
            ok = false;
        }
        if (!ok) {
            ERR("model step %" PRIu32 ": op %" PRIu32 ": %s (%" PRIu32
                " origins, windowed %d, seed %" PRIu64 ")",
                step, op, M->why, M->tracked, (int)M->windowed, seed);
        }
    }
    return err;
}

int clusterMailboxTest(int, char *[]) {
    int err = 0;

    const uint8_t test_key[32] = {
        0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xa0, 0xb0,
        0xc0, 0xd0, 0xe0, 0xf0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0xfe, 0xff};
    const hmacKeyId K1 = 100u;

    TEST("a new origin whose frame is refused takes no slot") {
        cleaner(cmbModelFree_) cmbModel_ *M = zcalloc(1, sizeof(*M));
        /* @own: container; free=hmacKeyringFree */
        M->ring = hmacKeyringCreate();
        uint8_t key[32];
        cmbKey_(1, key);
        (void)hmacKeyringInstallUnsalted(M->ring, 1, &hmacAlgoSHA256, key,
                                         sizeof(key));
        /* @own: container; free=clusterMailboxFree */
        M->producer = clusterMailboxCreate(M->ring, FLEX_ENV_ALGO_HMAC_BLAKE3,
                                           nullptr, nullptr);
        /* @own: container; free=clusterMailboxFree */
        M->consumer = clusterMailboxCreate(M->ring, FLEX_ENV_ALGO_HMAC_BLAKE3,
                                           cmbApply_, M);
        uint8_t out[256];
        size_t wrote = 0;
        if (!clusterMailboxSeal(M->producer, 9, 1, 0, nullptr, 0, 1, out,
                                sizeof(out), &wrote)) {
            ERR("%s", "seal");
        }
        M->accept = false;
        if (clusterMailboxIngest(M->consumer, out, wrote, 1) !=
                CLUSTER_MAILBOX_ERR_APPLY_REJECTED ||
            clusterMailboxOriginCount(M->consumer) != 0 ||
            clusterMailboxSnapshotSize(M->consumer) !=
                clusterMailboxSnapshotSize(M->producer)) {
            ERR("%s", "a refused origin was kept");
        }
        M->accept = true;
        if (clusterMailboxIngest(M->consumer, out, wrote, 1) !=
                CLUSTER_MAILBOX_OK ||
            clusterMailboxOriginCount(M->consumer) != 1 ||
            clusterMailboxLastApplied(M->consumer, 9) != 1) {
            ERR("%s", "the retransmitted frame was not applied");
        }
    }

    TEST("model: frames for many origins against a model of the watermarks") {
        for (uint64_t seed = 1; seed <= 4; seed++) {
            err += clusterMailboxTestModel_(seed);
        }
    }

    TEST("Seal fits its frame exactly within SealedSize, and no smaller") {
        /* For header fields of every varint width and payloads around
         * the envelope's size steps: the frame fits SealedSize, Seal into
         * exactly its own length succeeds with the same bytes, one byte
         * less is refused and counted, and the frame ingests */
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        clusterMailbox *mb = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, nullptr, nullptr);
        clusterMailbox *rx = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, nullptr, nullptr);
        static constexpr uint32_t origins[] = {1, 300, UINT32_MAX};
        static constexpr uint64_t seqs[] = {1, 70000, UINT64_MAX / 3};
        static constexpr size_t lens[] = {0, 1, 100, 127, 128, 300, 70000};
        static uint8_t payload[70000];
        static uint8_t frame[70200];
        static uint8_t exact[70200];
        memset(payload, 0x3c, sizeof(payload));
        uint64_t refusals = 0;
        for (size_t o = 0; o < 3; o++) {
            for (size_t l = 0; l < countof(lens); l++) {
                /* Increasing per origin, a different width per origin */
                const uint64_t seq = seqs[o] + l;
                size_t n = 0;
                const size_t bound = clusterMailboxSealedSize(mb, lens[l]);
                const bool ok =
                    clusterMailboxSeal(mb, origins[o], seq, 7, payload, lens[l],
                                       K1, frame, sizeof(frame), &n);
                size_t n2 = 0;
                const bool okExact =
                    ok && clusterMailboxSeal(mb, origins[o], seq, 7, payload,
                                             lens[l], K1, exact, n, &n2);
                const bool shortRefused =
                    ok && n > 0 &&
                    !clusterMailboxSeal(mb, origins[o], seq, 7, payload,
                                        lens[l], K1, exact, n - 1, nullptr);
                refusals += shortRefused;
                const bool ingested =
                    ok && clusterMailboxIngest(rx, frame, n, K1) ==
                              CLUSTER_MAILBOX_OK;
                if (!ok || n > bound || !okExact || n2 != n ||
                    memcmp(frame, exact, n) != 0 || !shortRefused ||
                    !ingested) {
                    ERR("seal size: origin %" PRIu32 " len %zu: ok %d, %zu of "
                        "%zu, exact %d, short refused %d, ingested %d",
                        origins[o], lens[l], ok, n, bound, okExact,
                        shortRefused, ingested);
                }
            }
        }
        clusterMailboxStats st;
        clusterMailboxGetStats(mb, &st);
        if (st.seal_param_error != refusals) {
            ERR("seal size: %" PRIu64 " refusals counted, %" PRIu64 " made",
                st.seal_param_error, refusals);
        }
        clusterMailboxFree(rx);
        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    /* Frames from a peer on a protocol this build does not read (newer
     * version, unknown must-understand feature) are refused with their
     * own result and counter, never applied; unknown ignorable features
     * are read */
    TEST("inner header: the protocol stamp gates every frame") {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        cmTestCtx ctx = {};
        clusterMailbox *mb = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx);

        const uint64_t v = clusterMailboxProtocol.version;
        const struct {
            uint64_t version;
            uint64_t features;
            clusterMailboxIngestResult want;
        } cases[] = {
            {v, 0, CLUSTER_MAILBOX_OK},
            {v + 1, 0, CLUSTER_MAILBOX_ERR_PROTOCOL},
            {v, 1, CLUSTER_MAILBOX_ERR_PROTOCOL},
            {v, UINT64_C(1) << 40, CLUSTER_MAILBOX_OK},
        };
        for (size_t c = 0; c < countof(cases); c++) {
            cleaner(flexFree) flex *hdr = flexNew();
            flexRow row = FLEX_ROW_INIT;
            flexRowUnsigned(&row, cases[c].version);
            flexRowUnsigned(&row, cases[c].features);
            flexRowUnsigned(&row, 9);
            flexRowUnsigned(&row, 1 + c);
            flexRowUnsigned(&row, 0);
            flexRowAppendExact(&hdr, &row);

            uint8_t sealed[256];
            const size_t n = cmTestSealInner(hdr, test_key, sizeof(test_key),
                                             sealed, sizeof(sealed));
            const clusterMailboxIngestResult rc =
                clusterMailboxIngest(mb, sealed, n, K1);
            if (rc != cases[c].want) {
                ERR("case %zu: rc %d", c, (int32_t)rc);
            }
        }

        clusterMailboxStats stats;
        clusterMailboxGetStats(mb, &stats);
        if (stats.protocol_rejected != 2 || ctx.apply_count != 2) {
            ERR("protocol_rejected %" PRIu64 ", applied %u",
                stats.protocol_rejected, (unsigned)ctx.apply_count);
        }
        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    TEST("inner header: a 96/128-bit field is refused, sealed or not") {
        databoxBig wides[CM_TEST_WIDE_COUNT];
        cmTestWideValues(wides);
        uint8_t hdr[CLUSTER_MAILBOX_INNER_HEADER_MAX_BYTES];
        const size_t hlen =
            clusterMailboxEncodeInnerHeader(hdr, sizeof(hdr), 7u, 12345u, 42u);
        if (hlen == 0) {
            ERR("encode failed");
        }
        const flex *src = (const flex *)hdr;

        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        cmTestCtx ctx = {};
        clusterMailbox *mb = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx);
        uint8_t sealed[256];

        /* The rebuilt header, sealed by hand, must be applied, so a
         * refusal below is due to the replaced field alone. */
        {
            cleaner(flexFree) flex *same =
                cmTestReplaceEntry(src, SIZE_MAX, nullptr);
            const size_t n = cmTestSealInner(same, test_key, sizeof(test_key),
                                             sealed, sizeof(sealed));
            ctestPoisonStack();
            if (n == 0 ||
                clusterMailboxIngest(mb, sealed, n, K1) != CLUSTER_MAILBOX_OK ||
                ctx.apply_count != 1u || ctx.sequence != 12345u) {
                ERR("hand-sealed unmodified header was not applied");
            }
        }

        for (size_t w = 0; w < CM_TEST_WIDE_COUNT; w++) {
            for (size_t idx = 0; idx < CLUSTER_MAILBOX_INNER_HEADER_FLEX_COUNT;
                 idx++) {
                cleaner(flexFree) flex *bad =
                    cmTestReplaceEntry(src, idx, &wides[w]);
                uint32_t origin = 0;
                uint64_t seq = 0;
                uint8_t mtype = 0;
                size_t consumed = 0;
                ctestPoisonStack();
                dkFormatStamp protocol;
                if (clusterMailboxDecodeInnerHeader(bad, flexBytes(bad),
                                                    &protocol, &origin, &seq,
                                                    &mtype, &consumed)) {
                    ERR("decode accepted value %zu at entry %zu", w, idx);
                }

                const size_t n = cmTestSealInner(
                    bad, test_key, sizeof(test_key), sealed, sizeof(sealed));
                if (n == 0) {
                    ERR("seal failed for value %zu at entry %zu", w, idx);
                    continue;
                }
                ctestPoisonStack();
                const clusterMailboxIngestResult rc =
                    clusterMailboxIngest(mb, sealed, n, K1);
                if (rc != CLUSTER_MAILBOX_ERR_BAD_FRAME) {
                    ERR("ingest of value %zu at entry %zu gave rc=%d", w, idx,
                        (int32_t)rc);
                }
            }
        }
        if (ctx.apply_count != 1u ||
            clusterMailboxLastApplied(mb, 7u) != 12345u) {
            ERR("a refused frame reached apply or moved the watermark");
        }
        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    TEST("inner header and rotation announce: ranges and counts are exact") {
        /* Field values just past their range, a non-integer, and the
         * wrong entry count are refused; the largest values in range
         * are accepted and read back */
        uint8_t hdr[CLUSTER_MAILBOX_INNER_HEADER_MAX_BYTES];
        const size_t hlen =
            clusterMailboxEncodeInnerHeader(hdr, sizeof(hdr), 7u, 12345u, 42u);
        if (hlen == 0) {
            ERR("encode failed");
        }
        const flex *src = (const flex *)hdr;
        const databox text = databoxNewBytesString("x");
        databoxBig asText = {};
        memcpy(&asText, &text, sizeof(text));
        const databoxBig u33 = DATABOX_BIG_UNSIGNED((uint64_t)1 << 33);
        const databoxBig u32 = DATABOX_BIG_UNSIGNED((uint64_t)1 << 32);
        const databoxBig u256 = DATABOX_BIG_UNSIGNED(256);
        const databoxBig neg = DATABOX_BIG_SIGNED(-1);
        const struct {
            size_t idx;
            const databoxBig *value;
        } refused[] = {
            {0, &u33},    /* protocol version past uint32 */
            {2, &u32},    /* origin past uint32 */
            {2, &neg},    /* origin negative */
            {3, &asText}, /* sequence not an integer */
            {4, &u256},   /* message type past uint8 */
        };
        uint32_t origin = 0;
        uint64_t seq = 0;
        uint8_t mtype = 0;
        size_t consumed = 0;
        dkFormatStamp protocol;
        for (size_t i = 0; i < countof(refused); i++) {
            cleaner(flexFree) flex *bad =
                flexTestReplaceEntry(src, refused[i].idx, refused[i].value);
            if (clusterMailboxDecodeInnerHeader(bad, flexBytes(bad), &protocol,
                                                &origin, &seq, &mtype,
                                                &consumed)) {
                ERR("inner header case %zu accepted", i);
            }
        }
        const databoxBig maxOrigin = DATABOX_BIG_UNSIGNED(UINT32_MAX);
        const databoxBig maxType = DATABOX_BIG_UNSIGNED(UINT8_MAX);
        cleaner(flexFree) flex *edgeOrigin =
            flexTestReplaceEntry(src, 2, &maxOrigin);
        cleaner(flexFree) flex *edge =
            flexTestReplaceEntry(edgeOrigin, 4, &maxType);
        if (!clusterMailboxDecodeInnerHeader(edge, flexBytes(edge), &protocol,
                                             &origin, &seq, &mtype,
                                             &consumed) ||
            origin != UINT32_MAX || mtype != UINT8_MAX || seq != 12345u ||
            consumed != flexBytes(edge)) {
            ERR("inner header refused the largest in-range values");
        }
        /* Four or six entries */
        cleaner(flexFree) flex *shortHdr = flexDuplicate(src);
        flexDeleteRange(&shortHdr, -1, 1);
        cleaner(flexFree) flex *longHdr = flexDuplicate(src);
        flexPushUnsigned(&longHdr, 1, FLEX_ENDPOINT_TAIL);
        if (clusterMailboxDecodeInnerHeader(shortHdr, flexBytes(shortHdr),
                                            &protocol, &origin, &seq, &mtype,
                                            &consumed) ||
            clusterMailboxDecodeInnerHeader(longHdr, flexBytes(longHdr),
                                            &protocol, &origin, &seq, &mtype,
                                            &consumed)) {
            ERR("an inner header of 4 or 6 entries was accepted");
        }

        /* Rotation announce: one key id, 1..UINT32_MAX */
        uint8_t ann[CLUSTER_MAILBOX_ROTATE_ANNOUNCE_PAYLOAD_LEN];
        if (clusterMailboxBuildRotateAnnounce(ann, 0x1234u) == 0) {
            ERR("build failed");
        }
        const flex *annSrc = (const flex *)ann;
        const databoxBig zero = DATABOX_BIG_UNSIGNED(0);
        const databoxBig *badIds[] = {&zero, &u32, &neg, &asText};
        hmacKeyId got = 0;
        for (size_t i = 0; i < countof(badIds); i++) {
            cleaner(flexFree) flex *bad =
                flexTestReplaceEntry(annSrc, 0, badIds[i]);
            if (clusterMailboxParseRotateAnnounce(bad, flexBytes(bad), &got)) {
                ERR("announce case %zu accepted", i);
            }
        }
        cleaner(flexFree) flex *top =
            flexTestReplaceEntry(annSrc, 0, &maxOrigin);
        cleaner(flexFree) flex *twoIds = flexDuplicate(annSrc);
        flexPushUnsigned(&twoIds, 9, FLEX_ENDPOINT_TAIL);
        cleaner(flexFree) flex *noIds = flexNew();
        if (!clusterMailboxParseRotateAnnounce(top, flexBytes(top), &got) ||
            got != UINT32_MAX ||
            clusterMailboxParseRotateAnnounce(twoIds, flexBytes(twoIds),
                                              &got) ||
            clusterMailboxParseRotateAnnounce(noIds, flexBytes(noIds), &got) ||
            clusterMailboxParseRotateAnnounce(nullptr, 3, &got) ||
            clusterMailboxParseRotateAnnounce(ann, sizeof(ann), nullptr)) {
            ERR("announce bounds or counts not exact");
        }
    }

    TEST("rotation announce: a 96/128-bit key id is refused") {
        databoxBig wides[CM_TEST_WIDE_COUNT];
        cmTestWideValues(wides);
        uint8_t buf[CLUSTER_MAILBOX_ROTATE_ANNOUNCE_PAYLOAD_LEN];
        if (clusterMailboxBuildRotateAnnounce(buf, 0x1234u) == 0) {
            ERR("build failed");
        }
        const flex *src = (const flex *)buf;
        for (size_t w = 0; w < CM_TEST_WIDE_COUNT; w++) {
            cleaner(flexFree) flex *bad = cmTestReplaceEntry(src, 0, &wides[w]);
            hmacKeyId got = 0;
            ctestPoisonStack();
            if (clusterMailboxParseRotateAnnounce(bad, flexBytes(bad), &got)) {
                ERR("parse accepted value %zu", w);
            }
        }
    }

    TEST("restore: a 96/128-bit field is refused, wrapped or not") {
        databoxBig wides[CM_TEST_WIDE_COUNT];
        cmTestWideValues(wides);
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        cmTestCtx ctx = {};
        clusterMailbox *mb = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx);
        const uint8_t payload[] = "w";
        const size_t sz = clusterMailboxSealedSize(mb, sizeof(payload));
        for (uint32_t origin = 60u; origin <= 61u; origin++) {
            cleanerName(zfree, Bytes) uint8_t *b = zmalloc(sz);
            size_t w = 0;
            clusterMailboxSeal(mb, origin, origin + 3u, 0u, payload,
                               sizeof(payload), K1, b, sz, &w);
            if (clusterMailboxIngest(mb, b, w, K1) != CLUSTER_MAILBOX_OK) {
                ERR("setup ingest from origin %u failed", origin);
            }
        }
        const size_t snap_sz = clusterMailboxSnapshotSize(mb);
        cleanerName(zfree, Bytes) uint8_t *snap = zmalloc(snap_sz);
        size_t snap_len = 0;
        if (!clusterMailboxSnapshot(mb, snap, snap_sz, &snap_len)) {
            ERR("snapshot failed");
        }
        clusterMailboxFree(mb);
        const flex *src = (const flex *)snap;
        const size_t entries = flexCount(src);
        if (entries != 1u + 2u * CLUSTER_MAILBOX_PERSIST_ENTRIES_PER_SLOT) {
            ERR("snapshot has %zu entries, expected 6", entries);
        }

        flexEnvelopeWrapParams wp = {
            .algo = FLEX_ENV_ALGO_HMAC_BLAKE3,
            .key = test_key,
            .key_len = sizeof(test_key),
        };
        flexEnvelopeVerifyParams vp = {
            .key = test_key,
            .key_len = sizeof(test_key),
        };
        uint8_t wrapped[256];

        /* The rebuild must restore both ways, so a refusal below is due
         * to the replaced field alone. */
        {
            cleaner(flexFree) flex *same =
                cmTestReplaceEntry(src, SIZE_MAX, nullptr);
            clusterMailbox *dst = clusterMailboxCreate(
                ring, FLEX_ENV_ALGO_HMAC_SHA256, nullptr, nullptr);
            ctestPoisonStack();
            if (!clusterMailboxRestore(dst, same, flexBytes(same)) ||
                clusterMailboxLastApplied(dst, 61u) != 64u) {
                ERR("unmodified rebuild did not restore");
            }
            clusterMailboxFree(dst);
            size_t n = 0;
            dst = clusterMailboxCreate(ring, FLEX_ENV_ALGO_HMAC_SHA256, nullptr,
                                       nullptr);
            if (!cmTestWrapStamped(wrapped, sizeof(wrapped),
                                   (const uint8_t *)same, flexBytes(same), &wp,
                                   &n) ||
                !clusterMailboxRestoreWrapped(dst, &vp, wrapped, n) ||
                clusterMailboxLastApplied(dst, 60u) != 63u) {
                ERR("unmodified rebuild did not restore wrapped");
            }
            clusterMailboxFree(dst);
        }

        for (size_t w = 0; w < CM_TEST_WIDE_COUNT; w++) {
            for (size_t idx = 0; idx < entries; idx++) {
                cleaner(flexFree) flex *bad =
                    cmTestReplaceEntry(src, idx, &wides[w]);
                clusterMailbox *dst = clusterMailboxCreate(
                    ring, FLEX_ENV_ALGO_HMAC_SHA256, nullptr, nullptr);
                ctestPoisonStack();
                if (clusterMailboxRestore(dst, bad, flexBytes(bad))) {
                    ERR("restore accepted value %zu at entry %zu", w, idx);
                }
                clusterMailboxFree(dst);

                size_t n = 0;
                if (!cmTestWrapStamped(wrapped, sizeof(wrapped),
                                       (const uint8_t *)bad, flexBytes(bad),
                                       &wp, &n)) {
                    ERR("wrap failed for value %zu at entry %zu", w, idx);
                    continue;
                }
                dst = clusterMailboxCreate(ring, FLEX_ENV_ALGO_HMAC_SHA256,
                                           nullptr, nullptr);
                ctestPoisonStack();
                if (clusterMailboxRestoreWrapped(dst, &vp, wrapped, n)) {
                    ERR("wrapped restore accepted value %zu at entry %zu", w,
                        idx);
                }
                clusterMailboxFree(dst);
            }
        }
        hmacKeyringFree(ring);
    }

    TEST("create/free roundtrip + algo gating") {
        hmacKeyring *ring = hmacKeyringCreate();
        /* Refuse non-keyed algo. */
        clusterMailbox *bad =
            clusterMailboxCreate(ring, FLEX_ENV_ALGO_XXH64, nullptr, nullptr);
        if (bad != nullptr) {
            ERR("Create should refuse non-keyed algo (XXH64)");
        }
        /* Refuse AEAD algo. */
        bad = clusterMailboxCreate(ring, FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305,
                                   nullptr, nullptr);
        if (bad != nullptr) {
            ERR("Create should refuse AEAD algo");
        }
        /* Accept HMAC-SHA-256. */
        clusterMailbox *mb = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, nullptr, nullptr);
        if (mb == nullptr) {
            ERR("Create failed for HMAC-SHA-256");
        }
        if (clusterMailboxOriginCount(mb) != 0) {
            ERR("Origin count must start at 0");
        }
        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    TEST("unknown flags, AEAD on attach, bad sizes and NULL payloads are "
         "refused") {
        cleaner(hmacKeyringFree) hmacKeyring *ring = hmacKeyringCreate();
        static constexpr uint8_t k[32] = {7};
        hmacKeyringInstallUnsalted(ring, 3u, &hmacAlgoSHA256, k, sizeof(k));
        cleaner(clusterMailboxFree) clusterMailbox *unknownFlag =
            clusterMailboxCreateEx(ring, FLEX_ENV_ALGO_HMAC_SHA256, nullptr,
                                   nullptr, 1u << 5);
        cleaner(clusterMailboxFree) clusterMailbox *noOpts =
            clusterMailboxCreateOpts(nullptr);
        cleaner(clusterMailboxFree) clusterMailbox *mb = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, nullptr, nullptr);
        if (unknownFlag || noOpts || !mb) {
            ERR("unknown flag bits or no options were accepted");
        }

        /* Attach keeps the keyed-class pin; a refusal leaves the bound
         * algorithm, so sealing still works */
        uint8_t frame[256];
        size_t written = 0;
        static constexpr uint8_t one[1] = {1};
        if (clusterMailboxAttachKeyring(mb, ring, FLEX_ENV_ALGO_AEAD_AES256_GCM,
                                        nullptr, nullptr) ||
            clusterMailboxAttachKeyring(mb, ring, FLEX_ENV_ALGO_XXH3_64,
                                        nullptr, nullptr) ||
            clusterMailboxAttachKeyring(
                nullptr, ring, FLEX_ENV_ALGO_HMAC_SHA256, nullptr, nullptr) ||
            !clusterMailboxSeal(mb, 1u, 1u, 0u, one, sizeof(one), 3u, frame,
                                sizeof(frame), &written)) {
            ERR("attach took a non-keyed or AEAD algorithm");
        }

        /* A payload length that cannot be framed has no sealed size; a
         * NULL payload with bytes, or no output, is refused */
        if (clusterMailboxSealedSize(mb, SIZE_MAX) != 0 ||
            clusterMailboxSealedSize(nullptr, 1) != 0 ||
            clusterMailboxSeal(mb, 1u, 2u, 0u, nullptr, 4, 3u, frame,
                               sizeof(frame), &written) ||
            clusterMailboxSeal(mb, 1u, 2u, 0u, one, sizeof(one), 3u, nullptr,
                               sizeof(frame), &written)) {
            ERR("an unframeable size, a NULL payload or no output was taken");
        }
    }

    TEST("happy-path round-trip across many algorithms") {
        const flexEnvelopeAlgo algos[] = {FLEX_ENV_ALGO_HMAC_SHA256,
                                          FLEX_ENV_ALGO_HMAC_BLAKE3,
                                          FLEX_ENV_ALGO_BLAKE3_KEYED};
        for (uint32_t i = 0; i < countof(algos); i++) {
            const flexEnvelopeAlgo algo = algos[i];
            const hmacHashAlgo *hash_algo = (algo == FLEX_ENV_ALGO_HMAC_BLAKE3)
                                                ? &hmacAlgoBLAKE3
                                                : &hmacAlgoSHA256;
            hmacKeyring *ring = hmacKeyringCreate();
            if (!hmacKeyringInstallUnsalted(ring, K1, hash_algo, test_key,
                                            sizeof(test_key))) {
                ERR("Keyring install failed for algo=%u", (uint32_t)algo);
            }
            cmTestCtx ctx = {};
            clusterMailbox *mb =
                clusterMailboxCreate(ring, algo, cmRecordApply, &ctx);

            const uint8_t payload[5] = {'h', 'e', 'l', 'l', 'o'};
            const size_t sz = clusterMailboxSealedSize(mb, sizeof(payload));
            cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(sz);
            size_t written = 0;
            if (!clusterMailboxSeal(mb, 7u, 1u, 42u, payload, sizeof(payload),
                                    K1, buf, sz, &written)) {
                ERR("Seal failed for algo=%u", (uint32_t)algo);
            }
            const clusterMailboxIngestResult rc =
                clusterMailboxIngest(mb, buf, written, K1);
            if (rc != CLUSTER_MAILBOX_OK) {
                ERR("Ingest failed for algo=%u rc=%d", (uint32_t)algo,
                    (int32_t)rc);
            }
            if (ctx.apply_count != 1 || ctx.origin_id != 7u ||
                ctx.sequence != 1u || ctx.msg_type != 42u ||
                ctx.last_payload_len != 5 ||
                memcmp(ctx.last_payload, payload, 5) != 0) {
                ERR("Apply callback received wrong frame for algo=%u",
                    (uint32_t)algo);
            }
            if (clusterMailboxLastApplied(mb, 7u) != 1u) {
                ERR("LastApplied watermark not advanced for algo=%u",
                    (uint32_t)algo);
            }

            clusterMailboxFree(mb);
            hmacKeyringFree(ring);
        }
    }

    TEST("replay rejection: same sequence twice") {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        cmTestCtx ctx = {};
        clusterMailbox *mb = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx);

        const uint8_t payload[] = "first";
        const size_t sz = clusterMailboxSealedSize(mb, sizeof(payload));
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(sz);
        size_t written = 0;
        clusterMailboxSeal(mb, 1u, 5u, 1u, payload, sizeof(payload), K1, buf,
                           sz, &written);
        if (clusterMailboxIngest(mb, buf, written, K1) != CLUSTER_MAILBOX_OK) {
            ERR("First ingest failed");
        }
        if (ctx.apply_count != 1) {
            ERR("Apply count after first ingest should be 1, got %u",
                ctx.apply_count);
        }
        /* Re-ingest the same bytes — replay. */
        if (clusterMailboxIngest(mb, buf, written, K1) !=
            CLUSTER_MAILBOX_ERR_REPLAY) {
            ERR("Second ingest must be rejected as replay");
        }
        if (ctx.apply_count != 1) {
            ERR("Apply count must not advance on replay, got %u",
                ctx.apply_count);
        }
        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    TEST("replay rejection: older sequence after newer applied") {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        cmTestCtx ctx = {};
        clusterMailbox *mb = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx);

        const uint8_t payload[] = "x";
        const size_t sz = clusterMailboxSealedSize(mb, sizeof(payload));
        cleanerName(zfree, Bytes) uint8_t *seal10 = zmalloc(sz);
        cleanerName(zfree, Bytes) uint8_t *seal5 = zmalloc(sz);
        size_t w10 = 0, w5 = 0;
        clusterMailboxSeal(mb, 9u, 10u, 0u, payload, sizeof(payload), K1,
                           seal10, sz, &w10);
        clusterMailboxSeal(mb, 9u, 5u, 0u, payload, sizeof(payload), K1, seal5,
                           sz, &w5);
        /* Apply seq=10 first, then try seq=5 — must be rejected. */
        if (clusterMailboxIngest(mb, seal10, w10, K1) != CLUSTER_MAILBOX_OK) {
            ERR("Apply of seq=10 failed");
        }
        if (clusterMailboxIngest(mb, seal5, w5, K1) !=
            CLUSTER_MAILBOX_ERR_REPLAY) {
            ERR("Out-of-order seq=5 must be rejected as replay");
        }
        if (clusterMailboxLastApplied(mb, 9u) != 10u) {
            ERR("Watermark should still be 10, got %" PRIu64,
                clusterMailboxLastApplied(mb, 9u));
        }
        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    TEST("replay window: in-window out-of-order accepted; duplicate rejected") {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        cmTestCtx ctx = {};
        clusterMailbox *mb = clusterMailboxCreateEx(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx,
            CLUSTER_MAILBOX_FLAG_REPLAY_WINDOW);

        const uint8_t payload[] = "x";
        const size_t sz = clusterMailboxSealedSize(mb, sizeof(payload));
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(sz);
        size_t w = 0;

        /* Apply 100, then 99, 98, 97 — all in-window and unseen, must
         * accept. Watermark stays at 100. */
        clusterMailboxSeal(mb, 7u, 100u, 0u, payload, sizeof(payload), K1, buf,
                           sz, &w);
        if (clusterMailboxIngest(mb, buf, w, K1) != CLUSTER_MAILBOX_OK) {
            ERR("seq=100");
        }
        for (uint64_t s = 99u; s >= 97u; s--) {
            clusterMailboxSeal(mb, 7u, s, 0u, payload, sizeof(payload), K1, buf,
                               sz, &w);
            if (clusterMailboxIngest(mb, buf, w, K1) != CLUSTER_MAILBOX_OK) {
                ERR("In-window seq=%" PRIu64 " rejected", s);
            }
        }
        if (ctx.apply_count != 4u) {
            ERR("expected 4 applies, got %u", ctx.apply_count);
        }
        if (clusterMailboxLastApplied(mb, 7u) != 100u) {
            ERR("Watermark should remain 100 after in-window fills");
        }
        /* Duplicate seq=99 — bit set; reject. */
        clusterMailboxSeal(mb, 7u, 99u, 0u, payload, sizeof(payload), K1, buf,
                           sz, &w);
        if (clusterMailboxIngest(mb, buf, w, K1) !=
            CLUSTER_MAILBOX_ERR_REPLAY) {
            ERR("Duplicate in-window must be REPLAY");
        }
        /* Duplicate of watermark itself — bit 0 set; reject. */
        clusterMailboxSeal(mb, 7u, 100u, 0u, payload, sizeof(payload), K1, buf,
                           sz, &w);
        if (clusterMailboxIngest(mb, buf, w, K1) !=
            CLUSTER_MAILBOX_ERR_REPLAY) {
            ERR("Duplicate watermark must be REPLAY");
        }
        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    TEST("replay window: out-of-window old sequence rejected") {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        cmTestCtx ctx = {};
        clusterMailbox *mb = clusterMailboxCreateEx(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx,
            CLUSTER_MAILBOX_FLAG_REPLAY_WINDOW);

        const uint8_t payload[] = "y";
        const size_t sz = clusterMailboxSealedSize(mb, sizeof(payload));
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(sz);
        size_t w = 0;

        /* Apply seq=200; the window covers (200-63, 200]. seq=136 is
         * exactly at the boundary (age=64), out of window. */
        clusterMailboxSeal(mb, 8u, 200u, 0u, payload, sizeof(payload), K1, buf,
                           sz, &w);
        if (clusterMailboxIngest(mb, buf, w, K1) != CLUSTER_MAILBOX_OK) {
            ERR("seq=200");
        }
        /* age=63 (last in-window) should accept. */
        clusterMailboxSeal(mb, 8u, 137u, 0u, payload, sizeof(payload), K1, buf,
                           sz, &w);
        if (clusterMailboxIngest(mb, buf, w, K1) != CLUSTER_MAILBOX_OK) {
            ERR("age=63 must accept");
        }
        /* age=64 (just out) must reject. */
        clusterMailboxSeal(mb, 8u, 136u, 0u, payload, sizeof(payload), K1, buf,
                           sz, &w);
        if (clusterMailboxIngest(mb, buf, w, K1) !=
            CLUSTER_MAILBOX_ERR_REPLAY) {
            ERR("age=64 must REPLAY");
        }
        /* age=100 well out of window must reject. */
        clusterMailboxSeal(mb, 8u, 100u, 0u, payload, sizeof(payload), K1, buf,
                           sz, &w);
        if (clusterMailboxIngest(mb, buf, w, K1) !=
            CLUSTER_MAILBOX_ERR_REPLAY) {
            ERR("age=100 must REPLAY");
        }
        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    TEST("rotation state: init/promote/retire/accepts") {
        clusterRotationState st;
        if (clusterRotationInit(&st, 0u)) {
            ERR("init with 0 must refuse");
        }
        if (!clusterRotationInit(&st, 5u)) {
            ERR("init with 5 must succeed");
        }
        if (!clusterRotationAccepts(&st, 5u)) {
            ERR("primary should be accepted");
        }
        if (clusterRotationAccepts(&st, 7u)) {
            ERR("non-primary must not be accepted before promote");
        }
        if (clusterRotationPromote(&st, 5u)) {
            ERR("promote to same id must refuse (no-op)");
        }
        if (clusterRotationPromote(&st, 0u)) {
            ERR("promote to INVALID must refuse");
        }
        if (!clusterRotationPromote(&st, 7u)) {
            ERR("promote to 7 must succeed");
        }
        if (!clusterRotationAccepts(&st, 7u)) {
            ERR("new primary accepted");
        }
        if (!clusterRotationAccepts(&st, 5u)) {
            ERR("previous still accepted during overlap");
        }
        clusterRotationRetirePrevious(&st);
        if (clusterRotationAccepts(&st, 5u)) {
            ERR("retired previous must not be accepted");
        }
        if (!clusterRotationAccepts(&st, 7u)) {
            ERR("primary still accepted after retire");
        }
    }

    TEST("rotation announce: build/parse round-trip") {
        /* the rotate-announce payload is now a 1-entry flex
         * (typically 13-15 bytes). Buffer MUST be sized to the public
         * worst-case constant — the previous [4]-sized buffer caused
         * an ASAN stack-buffer-overflow as soon as flexBytes(f) > 4. */
        uint8_t buf[CLUSTER_MAILBOX_ROTATE_ANNOUNCE_PAYLOAD_LEN];
        if (clusterMailboxBuildRotateAnnounce(buf, 0u) != 0) {
            ERR("build with 0 must refuse");
        }
        const size_t built =
            clusterMailboxBuildRotateAnnounce(buf, 0x12345678u);
        if (built == 0) {
            ERR("build must return non-zero");
        }
        if (built > CLUSTER_MAILBOX_ROTATE_ANNOUNCE_PAYLOAD_LEN) {
            ERR("build returned %zu > payload cap", built);
        }
        hmacKeyId got = 0;
        if (!clusterMailboxParseRotateAnnounce(buf, built, &got)) {
            ERR("parse should succeed");
        }
        if (got != 0x12345678u) {
            ERR("round-trip mismatch: got 0x%x", got);
        }
        /* Malformed: truncated length, zero id. */
        if (clusterMailboxParseRotateAnnounce(buf, built - 1u, &got)) {
            ERR("short-len parse must refuse");
        }
        const uint8_t zeros[CLUSTER_MAILBOX_ROTATE_ANNOUNCE_PAYLOAD_LEN] = {};
        if (clusterMailboxParseRotateAnnounce(zeros, built, &got)) {
            ERR("zero-id parse must refuse");
        }
    }

    TEST("rotation choreography: end-to-end with two mailboxes") {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        const hmacKeyId K2 = 2u;
        hmacKeyringInstallUnsalted(ring, K2, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));

        cmTestCtx ctx_prod = {};
        cmTestCtx ctx_cons = {};
        clusterMailbox *prod = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx_prod);
        clusterMailbox *cons = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx_cons);

        clusterRotationState rs_prod, rs_cons;
        clusterRotationInit(&rs_prod, K1);
        clusterRotationInit(&rs_cons, K1);

        const uint8_t payload[] = "tick";
        const size_t sz = clusterMailboxSealedSize(prod, sizeof(payload));
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(sz);
        size_t w = 0;

        /* Phase 1: producer seals under K1; consumer accepts under K1. */
        clusterMailboxSeal(prod, 3u, 1u, 0u, payload, sizeof(payload),
                           rs_prod.primary, buf, sz, &w);
        if (!clusterRotationAccepts(&rs_cons, rs_prod.primary)) {
            ERR("consumer must accept producer's primary");
        }
        if (clusterMailboxIngest(cons, buf, w, rs_prod.primary) !=
            CLUSTER_MAILBOX_OK) {
            ERR("phase 1 ingest");
        }

        /* Phase 2: producer sends rotation announcement under K1
         * carrying K2. Consumer parses and promotes its state.
         *
         * the announce payload is a 1-entry flex
         * sized to CLUSTER_MAILBOX_ROTATE_ANNOUNCE_PAYLOAD_LEN
         * worst case; the previous [4]-byte buffer triggered ASAN
         * stack-buffer-overflow. Use the documented constant + the
         * actual returned length for the seal payload. */
        uint8_t announce_payload[CLUSTER_MAILBOX_ROTATE_ANNOUNCE_PAYLOAD_LEN];
        const size_t announce_len =
            clusterMailboxBuildRotateAnnounce(announce_payload, K2);
        clusterMailboxSeal(
            prod, 3u, 2u, CLUSTER_MAILBOX_MSG_KEY_ROTATE_ANNOUNCE,
            announce_payload, announce_len, rs_prod.primary, buf, sz, &w);
        if (clusterMailboxIngest(cons, buf, w, rs_prod.primary) !=
            CLUSTER_MAILBOX_OK) {
            ERR("phase 2 announcement ingest");
        }
        hmacKeyId new_id = 0;
        if (!clusterMailboxParseRotateAnnounce(
                ctx_cons.last_payload, ctx_cons.last_payload_len, &new_id)) {
            ERR("consumer parse announce");
        }
        if (!clusterRotationPromote(&rs_cons, new_id)) {
            ERR("consumer promote");
        }

        /* Producer also promotes; both now accept K2 (and overlap K1). */
        clusterRotationPromote(&rs_prod, K2);

        /* Phase 3: producer seals under K2; consumer accepts K2. */
        clusterMailboxSeal(prod, 3u, 3u, 0u, payload, sizeof(payload),
                           rs_prod.primary, buf, sz, &w);
        if (!clusterRotationAccepts(&rs_cons, rs_prod.primary)) {
            ERR("consumer must accept new primary");
        }
        if (clusterMailboxIngest(cons, buf, w, rs_prod.primary) !=
            CLUSTER_MAILBOX_OK) {
            ERR("phase 3 ingest under K2");
        }

        /* Phase 4: retire previous on both sides. */
        clusterRotationRetirePrevious(&rs_prod);
        clusterRotationRetirePrevious(&rs_cons);
        if (clusterRotationAccepts(&rs_cons, K1)) {
            ERR("K1 must be retired");
        }

        clusterMailboxFree(prod);
        clusterMailboxFree(cons);
        hmacKeyringFree(ring);
    }

    TEST("stats counters track each ingest result class") {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        cmTestCtx ctx = {};
        clusterMailbox *mb = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx);

        const uint8_t payload[] = "stats";
        const size_t sz = clusterMailboxSealedSize(mb, sizeof(payload));
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(sz);
        size_t w = 0;
        clusterMailboxSeal(mb, 11u, 1u, 0u, payload, sizeof(payload), K1, buf,
                           sz, &w);
        /* OK ingest. */
        if (clusterMailboxIngest(mb, buf, w, K1) != CLUSTER_MAILBOX_OK) {
            ERR("ingest OK");
        }
        /* Replay. */
        if (clusterMailboxIngest(mb, buf, w, K1) !=
            CLUSTER_MAILBOX_ERR_REPLAY) {
            ERR("ingest replay");
        }
        /* Tamper a byte — verify failure. */
        buf[w / 2] ^= 0x80;
        if (clusterMailboxIngest(mb, buf, w, K1) !=
            CLUSTER_MAILBOX_ERR_VERIFY_FAILED) {
            ERR("ingest verify_failed");
        }
        buf[w / 2] ^= 0x80; /* restore */
        /* Unknown key id. */
        if (clusterMailboxIngest(mb, buf, w, 99999u) !=
            CLUSTER_MAILBOX_ERR_KEY_MISSING) {
            ERR("ingest key_missing");
        }

        clusterMailboxStats st;
        clusterMailboxGetStats(mb, &st);
        if (st.ingest_total != 4u) {
            ERR("ingest_total expected 4, got %" PRIu64, st.ingest_total);
        }
        if (st.apply_ok != 1u) {
            ERR("apply_ok != 1");
        }
        if (st.replay_rejected != 1u) {
            ERR("replay_rejected != 1");
        }
        if (st.verify_failed != 1u) {
            ERR("verify_failed != 1");
        }
        if (st.key_missing != 1u) {
            ERR("key_missing != 1");
        }
        if (st.seal_total != 1u) {
            ERR("seal_total != 1");
        }
        if (st.bytes_sealed != (uint64_t)w) {
            ERR("bytes_sealed != w");
        }
        if (st.bytes_ingested != (uint64_t)w * 4u) {
            ERR("bytes_ingested mismatch");
        }

        clusterMailboxResetStats(mb);
        clusterMailboxGetStats(mb, &st);
        if (st.ingest_total != 0u || st.apply_ok != 0u ||
            st.bytes_sealed != 0u) {
            ERR("reset failed to clear counters");
        }

        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    TEST("replay window: large jump clears bitmap (no false replay)") {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        cmTestCtx ctx = {};
        clusterMailbox *mb = clusterMailboxCreateEx(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx,
            CLUSTER_MAILBOX_FLAG_REPLAY_WINDOW);

        const uint8_t payload[] = "z";
        const size_t sz = clusterMailboxSealedSize(mb, sizeof(payload));
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(sz);
        size_t w = 0;

        /* seq=10 then jump to seq=10000 — bitmap should clear; new
         * apply of seq=9999 (age=1) should accept (was not seen). */
        clusterMailboxSeal(mb, 9u, 10u, 0u, payload, sizeof(payload), K1, buf,
                           sz, &w);
        if (clusterMailboxIngest(mb, buf, w, K1) != CLUSTER_MAILBOX_OK) {
            ERR("seq=10");
        }
        clusterMailboxSeal(mb, 9u, 10000u, 0u, payload, sizeof(payload), K1,
                           buf, sz, &w);
        if (clusterMailboxIngest(mb, buf, w, K1) != CLUSTER_MAILBOX_OK) {
            ERR("seq=10000");
        }
        clusterMailboxSeal(mb, 9u, 9999u, 0u, payload, sizeof(payload), K1, buf,
                           sz, &w);
        if (clusterMailboxIngest(mb, buf, w, K1) != CLUSTER_MAILBOX_OK) {
            ERR("age=1 after big jump must accept");
        }
        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    TEST("algorithm-mismatch rejection (downgrade attack)") {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        cmTestCtx ctx = {};
        clusterMailbox *mb_sha = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx);
        clusterMailbox *mb_blake3 = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_BLAKE3, cmRecordApply, &ctx);

        /* Hold one mailbox to BLAKE3 binding; seal with that one. */
        const uint8_t payload[] = "z";
        const size_t sz = clusterMailboxSealedSize(mb_blake3, sizeof(payload));
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(sz);
        size_t written = 0;
        /* Install a BLAKE3 key for the seal. */
        const hmacKeyId K_B = 200u;
        hmacKeyringInstallUnsalted(ring, K_B, &hmacAlgoBLAKE3, test_key,
                                   sizeof(test_key));
        clusterMailboxSeal(mb_blake3, 1u, 1u, 0u, payload, sizeof(payload), K_B,
                           buf, sz, &written);
        /* Send to the SHA256-pinned mailbox — must be rejected. */
        const clusterMailboxIngestResult rc =
            clusterMailboxIngest(mb_sha, buf, written, K1);
        if (rc != CLUSTER_MAILBOX_ERR_ALGO_MISMATCH) {
            ERR("Cross-algo ingest should fail with ALGO_MISMATCH, got %d",
                (int32_t)rc);
        }

        clusterMailboxFree(mb_sha);
        clusterMailboxFree(mb_blake3);
        hmacKeyringFree(ring);
    }

    TEST("tamper detection: every byte position rejected") {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        cmTestCtx ctx = {};
        clusterMailbox *mb = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx);
        const uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        const size_t sz = clusterMailboxSealedSize(mb, sizeof(payload));
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(sz);
        size_t written = 0;
        clusterMailboxSeal(mb, 11u, 1u, 0u, payload, sizeof(payload), K1, buf,
                           sz, &written);
        /* Baseline ingest passes. */
        if (clusterMailboxIngest(mb, buf, written, K1) != CLUSTER_MAILBOX_OK) {
            ERR("Baseline ingest failed");
        }
        /* Reset state so we can re-test each flip. */
        clusterMailboxFree(mb);
        for (size_t pos = 0; pos < written; pos++) {
            cmTestCtx local = {};
            clusterMailbox *mb2 = clusterMailboxCreate(
                ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &local);
            const uint8_t saved = buf[pos];
            buf[pos] = saved ^ 0x01u;
            const clusterMailboxIngestResult rc =
                clusterMailboxIngest(mb2, buf, written, K1);
            buf[pos] = saved;
            clusterMailboxFree(mb2);
            if (rc == CLUSTER_MAILBOX_OK) {
                ERR("Tampered byte at offset %zu was accepted", pos);
                break;
            }
        }
        hmacKeyringFree(ring);
    }

    TEST("apply rejection does not advance watermark") {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        cmTestCtx ctx = {.reject_threshold = 1u};
        /* First apply is rejected (apply_count == 0, threshold == 1,
         * so >= true on entry — wait, condition is apply_count >=
         * threshold meaning 0 >= 1 is false → accepts. Adjust: set
         * threshold=0 means never-reject; >=1 means reject when
         * apply_count == 1. So with threshold=1, first call accepts,
         * second rejects. We want immediate rejection — bump
         * threshold to 0 won't work as documented. Use a custom
         * apply that always rejects instead. */
        clusterMailbox *mb = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx);
        const uint8_t payload[] = "p";
        const size_t sz = clusterMailboxSealedSize(mb, sizeof(payload));
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(sz);
        size_t written = 0;
        clusterMailboxSeal(mb, 3u, 1u, 0u, payload, sizeof(payload), K1, buf,
                           sz, &written);
        /* First ingest accepts (apply_count becomes 1). */
        if (clusterMailboxIngest(mb, buf, written, K1) != CLUSTER_MAILBOX_OK) {
            ERR("First ingest failed");
        }

        /* Build seq=2. With threshold=1 and apply_count now 1, the
         * next apply will be rejected. */
        cleanerName(zfree, Bytes) uint8_t *buf2 = zmalloc(sz);
        size_t w2 = 0;
        clusterMailboxSeal(mb, 3u, 2u, 0u, payload, sizeof(payload), K1, buf2,
                           sz, &w2);
        const clusterMailboxIngestResult rc =
            clusterMailboxIngest(mb, buf2, w2, K1);
        if (rc != CLUSTER_MAILBOX_ERR_APPLY_REJECTED) {
            ERR("Expected APPLY_REJECTED, got %d", (int32_t)rc);
        }
        if (clusterMailboxLastApplied(mb, 3u) != 1u) {
            ERR("Watermark must stay at 1 after apply rejection, got %" PRIu64,
                clusterMailboxLastApplied(mb, 3u));
        }

        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    TEST("missing key id rejects with KEY_MISSING") {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        cmTestCtx ctx = {};
        clusterMailbox *mb = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx);
        const uint8_t payload[] = "q";
        const size_t sz = clusterMailboxSealedSize(mb, sizeof(payload));
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(sz);
        size_t written = 0;
        clusterMailboxSeal(mb, 1u, 1u, 0u, payload, sizeof(payload), K1, buf,
                           sz, &written);
        const hmacKeyId K_missing = 999u;
        const clusterMailboxIngestResult rc =
            clusterMailboxIngest(mb, buf, written, K_missing);
        if (rc != CLUSTER_MAILBOX_ERR_KEY_MISSING) {
            ERR("Expected KEY_MISSING, got %d", (int32_t)rc);
        }
        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    TEST("a key id below the revoked-key floor neither seals nor ingests") {
        /* The rotation choreography raises the floor past the old id
         * while the old key is still installed: both directions must
         * refuse it from then on */
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        hmacKeyringInstallUnsalted(ring, K1 + 1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        cmTestCtx ctx = {};
        clusterMailbox *mb = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx);
        const uint8_t payload[] = "old key";
        const size_t sz = clusterMailboxSealedSize(mb, sizeof(payload));
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(sz);
        size_t written = 0;
        if (!clusterMailboxSeal(mb, 1u, 1u, 0u, payload, sizeof(payload), K1,
                                buf, sz, &written)) {
            ERR("%s", "the seal before the floor was refused");
        }
        if (!hmacKeyringSetMinValidKeyId(ring, K1 + 1)) {
            ERR("%s", "the floor was not raised");
        }
        const clusterMailboxIngestResult rc =
            clusterMailboxIngest(mb, buf, written, K1);
        clusterMailboxStats st;
        clusterMailboxGetStats(mb, &st);
        if (rc != CLUSTER_MAILBOX_ERR_KEY_REVOKED || ctx.apply_count ||
            st.key_revoked != 1) {
            ERR("a frame under a revoked id: result %d, %u applied, %" PRIu64
                " counted revoked",
                (int32_t)rc, (unsigned)ctx.apply_count, st.key_revoked);
        }
        size_t again = 0;
        if (clusterMailboxSeal(mb, 1u, 2u, 0u, payload, sizeof(payload), K1,
                               buf, sz, &again)) {
            ERR("%s", "a seal under a revoked id was taken");
        }
        if (!clusterMailboxSeal(mb, 1u, 2u, 0u, payload, sizeof(payload),
                                K1 + 1, buf, sz, &again) ||
            clusterMailboxIngest(mb, buf, again, K1 + 1) !=
                CLUSTER_MAILBOX_OK) {
            ERR("%s", "the id at the floor no longer seals and ingests");
        }
        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    TEST("a mailbox restored through persist keeps its replay mode") {
        hmacKeyring *ring = hmacKeyringCreate();
        static const clusterMailboxFlags modes[] = {
            CLUSTER_MAILBOX_FLAG_NONE, CLUSTER_MAILBOX_FLAG_REPLAY_WINDOW};
        for (size_t m = 0; m < countof(modes); m++) {
            clusterMailbox *mb = clusterMailboxCreateEx(
                ring, FLEX_ENV_ALGO_HMAC_SHA256, nullptr, nullptr, modes[m]);
            size_t len = 0;
            cleanerName(zfree, Bytes) uint8_t *snap =
                persistOpsClusterMailbox.snapshot(mb, &len);
            clusterMailbox *back =
                snap ? persistOpsClusterMailbox.restore(snap, len) : nullptr;
            /* A Restore into a live mailbox keeps the live one's mode */
            clusterMailbox *live =
                clusterMailboxCreateEx(ring, FLEX_ENV_ALGO_HMAC_SHA256, nullptr,
                                       nullptr, modes[1 - m]);
            const bool liveOk = snap &&
                                clusterMailboxRestore(live, snap, len) &&
                                live->flags == modes[1 - m];
            if (!back || back->flags != modes[m] || !liveOk) {
                ERR("mode %u: restored as %d, live restore %s",
                    (unsigned)modes[m], back ? (int)back->flags : -1,
                    liveOk ? "kept its mode" : "changed or failed");
            }
            clusterMailboxFree(live);
            clusterMailboxFree(back);
            clusterMailboxFree(mb);
        }
        hmacKeyringFree(ring);
    }

    TEST("a strict watermark restored into a windowed mailbox keeps every "
         "applied frame closed") {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        cmTestCtx ctx = {};
        clusterMailbox *strict = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx);
        const uint8_t payload[] = "w";
        const size_t sz = clusterMailboxSealedSize(strict, sizeof(payload));
        uint8_t *frames[11] = {};
        size_t lens[11] = {};
        for (uint64_t seq = 1; seq <= 10; seq++) {
            /* @own: container; free=zfree */
            frames[seq] = zmalloc(sz);
            if (!clusterMailboxSeal(strict, 5u, seq, 0u, payload,
                                    sizeof(payload), K1, frames[seq], sz,
                                    &lens[seq]) ||
                clusterMailboxIngest(strict, frames[seq], lens[seq], K1) !=
                    CLUSTER_MAILBOX_OK) {
                ERR("frame %" PRIu64 " did not apply", seq);
            }
        }
        const size_t snapCap = clusterMailboxSnapshotSize(strict);
        cleanerName(zfree, Bytes) uint8_t *snap = zmalloc(snapCap);
        size_t snapLen = 0;
        if (!clusterMailboxSnapshot(strict, snap, snapCap, &snapLen)) {
            ERR("%s", "the strict snapshot failed");
        }

        cmTestCtx wctx = {};
        clusterMailbox *windowed = clusterMailboxCreateEx(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &wctx,
            CLUSTER_MAILBOX_FLAG_REPLAY_WINDOW);
        if (!clusterMailboxRestore(windowed, snap, snapLen)) {
            ERR("%s", "the windowed restore failed");
        }
        for (uint64_t seq = 1; seq <= 10; seq++) {
            const clusterMailboxIngestResult rc =
                clusterMailboxIngest(windowed, frames[seq], lens[seq], K1);
            if (rc != CLUSTER_MAILBOX_ERR_REPLAY) {
                ERR("applied frame %" PRIu64 " ingested again: result %d", seq,
                    (int32_t)rc);
            }
            zfree(frames[seq]);
        }
        if (wctx.apply_count) {
            ERR("%u frames applied twice", (unsigned)wctx.apply_count);
        }
        clusterMailboxFree(windowed);
        clusterMailboxFree(strict);
        hmacKeyringFree(ring);
    }

    TEST("on_apply cannot re-enter its mailbox: the watermark never moves "
         "back") {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        cmReentryCtx re = {.key = K1};
        clusterMailbox *mb = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmReentryApply, &re);
        re.mb = mb;
        const uint8_t payload[] = "r";
        const size_t sz = clusterMailboxSealedSize(mb, sizeof(payload));
        /* @own: container; free=zfree */
        re.next = zmalloc(sz);
        cleanerName(zfree, Bytes) uint8_t *first = zmalloc(sz);
        size_t firstLen = 0;
        (void)clusterMailboxSeal(mb, 9u, 1u, 0u, payload, sizeof(payload), K1,
                                 first, sz, &firstLen);
        (void)clusterMailboxSeal(mb, 9u, 2u, 0u, payload, sizeof(payload), K1,
                                 re.next, sz, &re.nextLen);
        const clusterMailboxIngestResult rc =
            clusterMailboxIngest(mb, first, firstLen, K1);
        if (rc != CLUSTER_MAILBOX_OK ||
            re.nested != CLUSTER_MAILBOX_ERR_PARAM || re.restored ||
            clusterMailboxLastApplied(mb, 9u) != 1u) {
            ERR("re-entry: outer %d, nested %d, restore %d, watermark %" PRIu64,
                (int32_t)rc, (int32_t)re.nested, re.restored,
                clusterMailboxLastApplied(mb, 9u));
        }
        if (clusterMailboxIngest(mb, re.next, re.nextLen, K1) !=
            CLUSTER_MAILBOX_OK) {
            ERR("%s", "the frame refused inside on_apply did not apply after");
        }
        zfree(re.next);
        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    TEST("snapshot + restore preserves replay protection across restart") {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        cmTestCtx ctx = {};
        clusterMailbox *mb_a = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx);

        /* Drive a few origins forward. */
        const uint8_t payload[] = "snap";
        const size_t sz = clusterMailboxSealedSize(mb_a, sizeof(payload));
        const struct {
            uint32_t origin;
            uint64_t seq;
        } sends[] = {{10u, 3u}, {20u, 7u}, {30u, 1u}};
        for (uint32_t i = 0; i < 3u; i++) {
            cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(sz);
            size_t w = 0;
            clusterMailboxSeal(mb_a, sends[i].origin, sends[i].seq, 0u, payload,
                               sizeof(payload), K1, buf, sz, &w);
            if (clusterMailboxIngest(mb_a, buf, w, K1) != CLUSTER_MAILBOX_OK) {
                ERR("Setup ingest failed for (origin=%u, seq=%" PRIu64 ")",
                    sends[i].origin, sends[i].seq);
            }
        }

        /* Capture snapshot bytes. */
        const size_t snap_size = clusterMailboxSnapshotSize(mb_a);
        cleanerName(zfree, Bytes) uint8_t *snap = zmalloc(snap_size);
        size_t snap_written = 0;
        if (!clusterMailboxSnapshot(mb_a, snap, snap_size, &snap_written) ||
            snap_written != snap_size) {
            ERR("Snapshot write failed");
        }
        /* Snapshot is a valid flex buffer with 3 entries per slot.
         * Deep validation also catches any internal-length corruption. */
        if (!flexValidateBufferDeep(snap, snap_size)) {
            ERR("Snapshot is not a valid flex buffer");
        }
        if (flexCount((const flex *)snap) != 1u + 3u * 3u) {
            ERR("Expected 10 flex entries (the mode, 3 slots * 3), got %zu",
                flexCount((const flex *)snap));
        }

        /* "Restart": destroy the mailbox, build a new one, restore. */
        clusterMailboxFree(mb_a);
        cmTestCtx ctx_b = {};
        clusterMailbox *mb_b = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx_b);
        if (!clusterMailboxRestore(mb_b, snap, snap_size)) {
            ERR("Restore failed");
        }
        if (clusterMailboxOriginCount(mb_b) != 3u) {
            ERR("Origin count after restore should be 3, got %zu",
                clusterMailboxOriginCount(mb_b));
        }
        if (clusterMailboxLastApplied(mb_b, 10u) != 3u ||
            clusterMailboxLastApplied(mb_b, 20u) != 7u ||
            clusterMailboxLastApplied(mb_b, 30u) != 1u) {
            ERR("Watermarks did not round-trip through snapshot/restore");
        }

        /* Critical: an attacker replaying an old frame (origin=20,
         * seq=7) AFTER the restart must still be rejected. */
        cleanerName(zfree, Bytes) uint8_t *replay = zmalloc(sz);
        size_t replay_w = 0;
        clusterMailboxSeal(mb_b, 20u, 7u, 0u, payload, sizeof(payload), K1,
                           replay, sz, &replay_w);
        if (clusterMailboxIngest(mb_b, replay, replay_w, K1) !=
            CLUSTER_MAILBOX_ERR_REPLAY) {
            ERR("Replay after restore must be rejected");
        }
        /* But seq=8 (next after restored watermark) must apply. */
        cleanerName(zfree, Bytes) uint8_t *advance = zmalloc(sz);
        size_t advance_w = 0;
        clusterMailboxSeal(mb_b, 20u, 8u, 0u, payload, sizeof(payload), K1,
                           advance, sz, &advance_w);
        if (clusterMailboxIngest(mb_b, advance, advance_w, K1) !=
            CLUSTER_MAILBOX_OK) {
            ERR("Forward progress (seq=8) after restore must succeed");
        }

        clusterMailboxFree(mb_b);
        hmacKeyringFree(ring);
    }

    TEST("restore: malformed snapshots are rejected") {
        hmacKeyring *ring = hmacKeyringCreate();
        clusterMailbox *mb = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, nullptr, nullptr);

        /* Empty / non-flex buffer. */
        const uint8_t empty[1] = {};
        if (clusterMailboxRestore(mb, empty, 0)) {
            ERR("Restore must reject empty buffer");
        }
        if (clusterMailboxRestore(mb, empty, 1)) {
            ERR("Restore must reject non-flex bytes");
        }

        /* Entry count past the mode NOT a multiple of 3 — flex valid but
         * wrong shape. */
        cleaner(flexFree) flex *bad_count = flexNew();
        flexPushUnsigned(&bad_count, 0u, FLEX_ENDPOINT_TAIL); /* strict */
        flexPushUnsigned(&bad_count, 99u, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&bad_count, 42u, FLEX_ENDPOINT_TAIL);
        if (clusterMailboxRestore(mb, (const uint8_t *)bad_count,
                                  flexBytes(bad_count))) {
            ERR("Restore must reject entry count not divisible by 3");
        }
        /* Record with seq == 0 (sentinel). */
        cleaner(flexFree) flex *seq_zero = flexNew();
        flexPushUnsigned(&seq_zero, 0u, FLEX_ENDPOINT_TAIL);  /* strict */
        flexPushUnsigned(&seq_zero, 99u, FLEX_ENDPOINT_TAIL); /* origin */
        flexPushUnsigned(&seq_zero, 0u, FLEX_ENDPOINT_TAIL);  /* seq=0 */
        flexPushUnsigned(&seq_zero, 0u, FLEX_ENDPOINT_TAIL);  /* window_bits */
        if (clusterMailboxRestore(mb, (const uint8_t *)seq_zero,
                                  flexBytes(seq_zero))) {
            ERR("Restore must reject seq==0 record");
        }
        /* Record with origin_id == 0 (reserved sentinel). */
        cleaner(flexFree) flex *origin_zero = flexNew();
        flexPushUnsigned(&origin_zero, 0u, FLEX_ENDPOINT_TAIL);  /* strict */
        flexPushUnsigned(&origin_zero, 0u, FLEX_ENDPOINT_TAIL);  /* origin=0 */
        flexPushUnsigned(&origin_zero, 42u, FLEX_ENDPOINT_TAIL); /* seq */
        flexPushUnsigned(&origin_zero, 0u, FLEX_ENDPOINT_TAIL);  /* window */
        if (clusterMailboxRestore(mb, (const uint8_t *)origin_zero,
                                  flexBytes(origin_zero))) {
            ERR("Restore must reject origin_id==0 record");
        }
        /* Slot count exceeds origin table capacity. */
        cleaner(flexFree) flex *too_many = flexNew();
        flexPushUnsigned(&too_many, 0u, FLEX_ENDPOINT_TAIL); /* strict */
        for (size_t i = 0; i < CLUSTER_MAILBOX_ORIGIN_CAP + 1u; i++) {
            flexPushUnsigned(&too_many, (uint64_t)(i + 1u), FLEX_ENDPOINT_TAIL);
            flexPushUnsigned(&too_many, 1u, FLEX_ENDPOINT_TAIL);
            flexPushUnsigned(&too_many, 0u, FLEX_ENDPOINT_TAIL);
        }
        if (clusterMailboxRestore(mb, (const uint8_t *)too_many,
                                  flexBytes(too_many))) {
            ERR("Restore must reject slot count > capacity");
        }
        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    TEST("restore: a snapshot refused at record N leaves the mailbox as is") {
        hmacKeyring *ring = hmacKeyringCreate();
        clusterMailbox *mb = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, nullptr, nullptr);

        /* Origins 1..5 at sequences 100..500 with window bits set */
        cleaner(flexFree) flex *base = flexNew();
        flexPushUnsigned(&base, 0u, FLEX_ENDPOINT_TAIL); /* strict */
        for (uint64_t o = 1; o <= 5; o++) {
            flexPushUnsigned(&base, o, FLEX_ENDPOINT_TAIL);
            flexPushUnsigned(&base, o * 100u, FLEX_ENDPOINT_TAIL);
            flexPushUnsigned(&base, 0x5u << o, FLEX_ENDPOINT_TAIL);
        }
        if (!clusterMailboxRestore(mb, (const uint8_t *)base,
                                   flexBytes(base))) {
            ERR("Restore of the base snapshot failed");
        }

        clusterMailboxOrigin before[CLUSTER_MAILBOX_ORIGIN_CAP];
        memcpy(before, mb->origins, sizeof(before));
        const size_t countBefore = clusterMailboxOriginCount(mb);

        /* Each snapshot first advances existing origins 1..3 and adds new
         * origins 40..41, records Restore would have merged before
         * reaching the bad record that follows them */
        for (int kind = 0; kind < 4; kind++) {
            cleaner(flexFree) flex *bad = flexNew();
            flexPushUnsigned(&bad, 0u, FLEX_ENDPOINT_TAIL); /* strict */
            for (uint64_t o = 1; o <= 3; o++) {
                flexPushUnsigned(&bad, o, FLEX_ENDPOINT_TAIL);
                flexPushUnsigned(&bad, o * 100u + 50u, FLEX_ENDPOINT_TAIL);
                flexPushUnsigned(&bad, 0xFFu, FLEX_ENDPOINT_TAIL);
            }
            for (uint64_t o = 40; o <= 41; o++) {
                flexPushUnsigned(&bad, o, FLEX_ENDPOINT_TAIL);
                flexPushUnsigned(&bad, 7u, FLEX_ENDPOINT_TAIL);
                flexPushUnsigned(&bad, 1u, FLEX_ENDPOINT_TAIL);
            }

            switch (kind) {
            case 0: /* origin 0 */
                flexPushUnsigned(&bad, 0u, FLEX_ENDPOINT_TAIL);
                flexPushUnsigned(&bad, 9u, FLEX_ENDPOINT_TAIL);
                flexPushUnsigned(&bad, 0u, FLEX_ENDPOINT_TAIL);
                break;
            case 1: /* sequence 0 */
                flexPushUnsigned(&bad, 42u, FLEX_ENDPOINT_TAIL);
                flexPushUnsigned(&bad, 0u, FLEX_ENDPOINT_TAIL);
                flexPushUnsigned(&bad, 0u, FLEX_ENDPOINT_TAIL);
                break;
            case 2: /* window bits that are not an unsigned integer */
                flexPushUnsigned(&bad, 42u, FLEX_ENDPOINT_TAIL);
                flexPushUnsigned(&bad, 9u, FLEX_ENDPOINT_TAIL);
                flexPushBytes(&bad, "nope", 4, FLEX_ENDPOINT_TAIL);
                break;
            default:
                /* More new origins than the table has free slots */
                for (uint64_t o = 1000;
                     o < 1000 + CLUSTER_MAILBOX_ORIGIN_CAP - 5 - 2 + 1; o++) {
                    flexPushUnsigned(&bad, o, FLEX_ENDPOINT_TAIL);
                    flexPushUnsigned(&bad, 3u, FLEX_ENDPOINT_TAIL);
                    flexPushUnsigned(&bad, 0u, FLEX_ENDPOINT_TAIL);
                }
                break;
            }

            if (clusterMailboxRestore(mb, (const uint8_t *)bad,
                                      flexBytes(bad))) {
                ERR("Restore accepted bad snapshot kind %d", kind);
            }
            if (memcmp(before, mb->origins, sizeof(before)) != 0 ||
                clusterMailboxOriginCount(mb) != countBefore) {
                ERR("Refused snapshot kind %d changed the mailbox", kind);
                memcpy(before, mb->origins, sizeof(before));
            }
        }

        /* The same leading records alone still merge */
        cleaner(flexFree) flex *good = flexNew();
        flexPushUnsigned(&good, 0u, FLEX_ENDPOINT_TAIL); /* strict */
        for (uint64_t o = 1; o <= 3; o++) {
            flexPushUnsigned(&good, o, FLEX_ENDPOINT_TAIL);
            flexPushUnsigned(&good, o * 100u + 50u, FLEX_ENDPOINT_TAIL);
            flexPushUnsigned(&good, 0xFFu, FLEX_ENDPOINT_TAIL);
        }
        if (!clusterMailboxRestore(mb, (const uint8_t *)good,
                                   flexBytes(good)) ||
            clusterMailboxLastApplied(mb, 2u) != 250u ||
            clusterMailboxLastApplied(mb, 5u) != 500u ||
            clusterMailboxOriginCount(mb) != countBefore) {
            ERR("Restore of the valid leading records did not merge");
        }

        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    TEST("restore is monotonic: stale snapshot cannot rewind the watermark") {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        cmTestCtx ctx = {};
        clusterMailbox *mb = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx);

        /* Advance origin 5 to seq=10. */
        const uint8_t payload[] = "m";
        const size_t sz = clusterMailboxSealedSize(mb, sizeof(payload));
        for (uint64_t s = 1u; s <= 10u; s++) {
            cleanerName(zfree, Bytes) uint8_t *b = zmalloc(sz);
            size_t w = 0;
            clusterMailboxSeal(mb, 5u, s, 0u, payload, sizeof(payload), K1, b,
                               sz, &w);
            if (clusterMailboxIngest(mb, b, w, K1) != CLUSTER_MAILBOX_OK) {
                ERR("setup ingest of seq %" PRIu64 " failed", s);
            }
        }

        /* Build a "stale" snapshot showing origin 5 at seq=3 via flex. */
        cleaner(flexFree) flex *stale = flexNew();
        flexPushUnsigned(&stale, 0u, FLEX_ENDPOINT_TAIL); /* strict */
        flexPushUnsigned(&stale, 5u, FLEX_ENDPOINT_TAIL); /* origin */
        flexPushUnsigned(&stale, 3u, FLEX_ENDPOINT_TAIL); /* seq */
        flexPushUnsigned(&stale, 0u, FLEX_ENDPOINT_TAIL); /* window */
        const bool ok =
            clusterMailboxRestore(mb, (const uint8_t *)stale, flexBytes(stale));
        if (!ok) {
            ERR("Stale snapshot restore should succeed (it MAX-merges)");
        }
        if (clusterMailboxLastApplied(mb, 5u) != 10u) {
            ERR("Stale snapshot must NOT rewind the watermark below 10");
        }

        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    TEST("envelope-wrapped snapshot round-trips with integrity") {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        cmTestCtx ctx_a = {};
        clusterMailbox *mb_a = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx_a);

        /* Drive two origins forward so the snapshot has substance. */
        const uint8_t payload[] = "w";
        const size_t sz = clusterMailboxSealedSize(mb_a, sizeof(payload));
        const struct {
            uint32_t origin;
            uint64_t seq;
        } sends[] = {{50u, 4u}, {51u, 9u}};
        for (uint32_t i = 0; i < 2u; i++) {
            cleanerName(zfree, Bytes) uint8_t *b = zmalloc(sz);
            size_t w = 0;
            clusterMailboxSeal(mb_a, sends[i].origin, sends[i].seq, 0u, payload,
                               sizeof(payload), K1, b, sz, &w);
            if (clusterMailboxIngest(mb_a, b, w, K1) != CLUSTER_MAILBOX_OK) {
                ERR("setup ingest from origin %u failed", sends[i].origin);
            }
        }

        /* Wrap the snapshot under HMAC_BLAKE3 (different key class
         * than the mailbox's ingest pin to show they're independent). */
        flexEnvelopeWrapParams wp = {
            .algo = FLEX_ENV_ALGO_HMAC_BLAKE3,
            .key = test_key,
            .key_len = sizeof(test_key),
        };
        const size_t wrapped_sz =
            clusterMailboxSnapshotWrappedSize(mb_a, FLEX_ENV_ALGO_HMAC_BLAKE3);
        cleanerName(zfree, Bytes) uint8_t *wrapped = zmalloc(wrapped_sz);
        size_t written = 0;
        if (!clusterMailboxSnapshotWrapped(mb_a, &wp, wrapped, wrapped_sz,
                                           &written)) {
            ERR("Wrapped snapshot failed");
        }
        if (written != wrapped_sz) {
            ERR("Wrapped snapshot wrote %zu, expected %zu", written,
                wrapped_sz);
        }

        /* "Restart" — destroy and rebuild, then restore from the
         * envelope. Verify replay protection survives. */
        clusterMailboxFree(mb_a);
        cmTestCtx ctx_b = {};
        clusterMailbox *mb_b = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx_b);

        flexEnvelopeVerifyParams vp = {
            .key = test_key,
            .key_len = sizeof(test_key),
        };
        if (!clusterMailboxRestoreWrapped(mb_b, &vp, wrapped, wrapped_sz)) {
            ERR("Wrapped restore failed");
        }
        if (clusterMailboxLastApplied(mb_b, 50u) != 4u ||
            clusterMailboxLastApplied(mb_b, 51u) != 9u) {
            ERR("Watermarks did not round-trip through envelope");
        }

        /* Replay attack against the restored mailbox: send seq=9 on
         * origin 51 — must be rejected. */
        cleanerName(zfree, Bytes) uint8_t *replay = zmalloc(sz);
        size_t replay_w = 0;
        clusterMailboxSeal(mb_b, 51u, 9u, 0u, payload, sizeof(payload), K1,
                           replay, sz, &replay_w);
        if (clusterMailboxIngest(mb_b, replay, replay_w, K1) !=
            CLUSTER_MAILBOX_ERR_REPLAY) {
            ERR("Replay must be rejected after wrapped restore");
        }

        clusterMailboxFree(mb_b);
        hmacKeyringFree(ring);
    }

    /* The wrapped artifact names its format: one stamped with a version
     * newer than this build reads, a must-understand feature it lacks, or
     * no stamp at all is refused, leaving the mailbox unchanged; unknown
     * ignorable features are read */
    TEST("wrapped snapshot: format stamp gates the restore") {
        hmacKeyring *ring = hmacKeyringCreate();
        static constexpr uint8_t key[32] = {7};
        const flexEnvelopeWrapParams wp = {
            .algo = FLEX_ENV_ALGO_HMAC_SHA256, .key = key, .key_len = 32};
        const flexEnvelopeVerifyParams vp = {.key = key, .key_len = 32};

        /* Strict mode, one watermark: origin 5 at sequence 3 */
        cleaner(flexFree) flex *raw = flexNew();
        flexRow row = FLEX_ROW_INIT;
        flexRowUnsigned(&row, 0);
        flexRowUnsigned(&row, 5);
        flexRowUnsigned(&row, 3);
        flexRowUnsigned(&row, 0);
        flexRowAppendExact(&raw, &row);

        const dkFormatStamp current = cmWrappedStamp();
        const struct {
            dkFormatStamp stamp;
            bool stamped;
            bool accepted;
        } cases[] = {
            {current, true, true},
            {{current.id, current.version + 1, 0}, true, false},
            {{current.id, current.version, 1}, true, false},
            {{current.id, current.version, UINT64_C(1) << 50}, true, true},
            {{PERSIST_TYPE_CLUSTER_RING, current.version, 0}, true, false},
            {current, false, false},
        };
        for (size_t c = 0; c < countof(cases); c++) {
            uint8_t inner[128];
            size_t n = 0;
            if (cases[c].stamped) {
                n = dkFormatPrefixWrite(&cases[c].stamp, inner, sizeof(inner));
            }
            memcpy(inner + n, raw, flexBytes(raw));
            n += flexBytes(raw);

            uint8_t wrapped[256];
            size_t written = 0;
            flexEnvelopeWrap(wrapped, sizeof(wrapped), inner, n, &wp, &written);
            clusterMailbox *mb = clusterMailboxCreate(
                ring, FLEX_ENV_ALGO_HMAC_SHA256, nullptr, nullptr);
            const bool ok =
                clusterMailboxRestoreWrapped(mb, &vp, wrapped, written);
            if (ok != cases[c].accepted ||
                clusterMailboxLastApplied(mb, 5u) != (ok ? 3u : 0u)) {
                ERR("case %zu: restore %d", c, (int)ok);
            }
            clusterMailboxFree(mb);
        }
        hmacKeyringFree(ring);
    }

    TEST("wrapped snapshot: tamper detected, mailbox unchanged on bad input") {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        cmTestCtx ctx = {};
        clusterMailbox *mb = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx);

        /* Advance origin 77 to seq=5 in the current mailbox. */
        const uint8_t payload[] = "t";
        const size_t sz = clusterMailboxSealedSize(mb, sizeof(payload));
        for (uint64_t s = 1u; s <= 5u; s++) {
            cleanerName(zfree, Bytes) uint8_t *b = zmalloc(sz);
            size_t w = 0;
            clusterMailboxSeal(mb, 77u, s, 0u, payload, sizeof(payload), K1, b,
                               sz, &w);
            if (clusterMailboxIngest(mb, b, w, K1) != CLUSTER_MAILBOX_OK) {
                ERR("setup ingest of seq %" PRIu64 " failed", s);
            }
        }

        flexEnvelopeWrapParams wp = {
            .algo = FLEX_ENV_ALGO_HMAC_SHA256,
            .key = test_key,
            .key_len = sizeof(test_key),
        };
        const size_t wsz =
            clusterMailboxSnapshotWrappedSize(mb, FLEX_ENV_ALGO_HMAC_SHA256);
        cleanerName(zfree, Bytes) uint8_t *wrap_buf = zmalloc(wsz);
        size_t written = 0;
        clusterMailboxSnapshotWrapped(mb, &wp, wrap_buf, wsz, &written);

        /* Tamper a byte inside the envelope. Restore must reject. */
        wrap_buf[wsz / 2u] ^= 0x01u;
        flexEnvelopeVerifyParams vp = {
            .key = test_key,
            .key_len = sizeof(test_key),
        };
        if (clusterMailboxRestoreWrapped(mb, &vp, wrap_buf, wsz)) {
            ERR("Tampered wrapped snapshot must be rejected");
        }
        /* Mailbox state must be unchanged — we never reached the
         * inner restore. Origin 77 still at seq=5. */
        if (clusterMailboxLastApplied(mb, 77u) != 5u) {
            ERR("Failed restore must leave mailbox untouched");
        }

        /* Wrong key — also rejected, mailbox untouched. */
        wrap_buf[wsz / 2u] ^= 0x01u; /* untamper */
        uint8_t wrong_key[32];
        memcpy(wrong_key, test_key, sizeof(wrong_key));
        wrong_key[0] ^= 0xFFu;
        flexEnvelopeVerifyParams vp_wrong = {
            .key = wrong_key,
            .key_len = sizeof(wrong_key),
        };
        if (clusterMailboxRestoreWrapped(mb, &vp_wrong, wrap_buf, wsz)) {
            ERR("Wrong-key wrapped restore must be rejected");
        }
        if (clusterMailboxLastApplied(mb, 77u) != 5u) {
            ERR("Wrong-key restore must leave mailbox untouched");
        }

        /* Sanity: correct key still works. */
        if (!clusterMailboxRestoreWrapped(mb, &vp, wrap_buf, wsz)) {
            ERR("Correct-key restore should still succeed");
        }

        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    TEST("wrapped snapshot refuses non-keyed and AEAD envelope algorithms") {
        hmacKeyring *ring = hmacKeyringCreate();
        clusterMailbox *mb = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, nullptr, nullptr);
        uint8_t buf[256];
        size_t written = 0;
        flexEnvelopeWrapParams wp_ck = {.algo = FLEX_ENV_ALGO_XXH64};
        if (clusterMailboxSnapshotWrapped(mb, &wp_ck, buf, sizeof(buf),
                                          &written)) {
            ERR("Wrapped snapshot must refuse checksum algo");
        }
        flexEnvelopeWrapParams wp_aead = {
            .algo = FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305,
        };
        if (clusterMailboxSnapshotWrapped(mb, &wp_aead, buf, sizeof(buf),
                                          &written)) {
            ERR("Wrapped snapshot must refuse AEAD algo");
        }
        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    TEST("fuzz: random ingest bytes never crash, never accept") {
        /* Same security-correctness pattern that caught the
         * varintDecode SEGV in envelope: throw nonsense at Ingest with
         * a real keyring and ensure it always returns an error, never
         * crashes, never advances a watermark. */
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        cmTestCtx ctx = {};
        clusterMailbox *mb = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx);

        const uint32_t bucket_sizes[] = {0u,   1u,   2u,   16u,   32u,  64u,
                                         128u, 256u, 512u, 1024u, 4096u};
        const uint32_t num_buckets = (uint32_t)(countof(bucket_sizes));
        const uint32_t iters_per_bucket = 2048u;

        uint64_t accepted = 0;
        for (uint32_t b = 0; b < num_buckets; b++) {
            const uint32_t blen = bucket_sizes[b];
            cleanerName(zfree, Bytes) uint8_t *buf =
                blen > 0 ? zmalloc(blen) : nullptr;
            for (uint32_t it = 0; it < iters_per_bucket; it++) {
                for (size_t i = 0; i < blen; i++) {
                    buf[i] = (uint8_t)((i * 1664525u + it * 1013904223u +
                                        b * 22695477u) >>
                                       8u);
                }
                /* Sometimes splat the envelope magic so the parser
                 * exercises the post-magic varint paths. */
                if (blen >= 2u && (it & 0x3u) == 0u) {
                    buf[0] = FLEX_ENVELOPE_MAGIC_0;
                    buf[1] = FLEX_ENVELOPE_MAGIC_1;
                }
                /* Mix key_id: real, unknown, sentinel. */
                const hmacKeyId kid = (it & 0x7u) == 0u
                                          ? HMAC_KEY_ID_INVALID
                                          : ((it & 0x7u) == 1u ? K1 : 999u);
                const clusterMailboxIngestResult rc =
                    clusterMailboxIngest(mb, buf, blen, kid);
                if (rc == CLUSTER_MAILBOX_OK) {
                    accepted++;
                }
            }
        }
        /* Cryptographically vanishingly small probability that random
         * bytes form a valid envelope with the right key, framing,
         * and inner header. Anything > 0 is a regression. */
        if (accepted != 0) {
            ERR("Fuzz accepted %" PRIu64 " random inputs (expected 0)",
                accepted);
        }
        /* Watermark table must still be empty — no random input got
         * far enough to register an origin. */
        if (clusterMailboxOriginCount(mb) != 0) {
            ERR("Origin table should be empty after fuzz, got %zu",
                clusterMailboxOriginCount(mb));
        }
        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    TEST("multi-origin tracking is independent") {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        cmTestCtx ctx = {};
        clusterMailbox *mb = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx);
        const uint8_t payload[] = "m";
        const size_t sz = clusterMailboxSealedSize(mb, sizeof(payload));
        /* Apply (origin=1, seq=5), (origin=2, seq=3), (origin=1, seq=6) */
        const struct {
            uint32_t origin;
            uint64_t seq;
        } sends[] = {{1u, 5u}, {2u, 3u}, {1u, 6u}};
        for (uint32_t i = 0; i < 3u; i++) {
            cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(sz);
            size_t written = 0;
            clusterMailboxSeal(mb, sends[i].origin, sends[i].seq, 0u, payload,
                               sizeof(payload), K1, buf, sz, &written);
            if (clusterMailboxIngest(mb, buf, written, K1) !=
                CLUSTER_MAILBOX_OK) {
                ERR("Ingest failed for (origin=%u, seq=%" PRIu64 ")",
                    sends[i].origin, sends[i].seq);
            }
        }
        if (clusterMailboxLastApplied(mb, 1u) != 6u ||
            clusterMailboxLastApplied(mb, 2u) != 3u) {
            ERR("Per-origin watermarks did not advance independently");
        }
        if (clusterMailboxOriginCount(mb) != 2u) {
            ERR("Expected 2 origins, got %zu", clusterMailboxOriginCount(mb));
        }
        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    /* Regression: apply-rejected slot must (a) still be findable
     * by origin_id so the replay check applies, (b) NOT be silently
     * re-allocated by a subsequent ingest, (c) not double-count
     * origin_count. Reject of seq=10 followed by attacker replay of an
     * older seq=5 must NOT be accepted. */
    TEST("apply-rejected slot is still replay-protected on retry") {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        cmTestCtx ctx = {.reject_threshold = 1u};
        clusterMailbox *mb = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx);

        const uint8_t payload[] = "r";
        const size_t sz = clusterMailboxSealedSize(mb, sizeof(payload));

        /* Bring origin 5 to last_applied = 10 first (apply accepts on
         * the 0..(threshold-1)th call only), so the slot is occupied
         * with last_applied = 10. */
        cleanerName(zfree, Bytes) uint8_t *seal10 = zmalloc(sz);
        size_t w10 = 0;
        clusterMailboxSeal(mb, 5u, 10u, 0u, payload, sizeof(payload), K1,
                           seal10, sz, &w10);
        if (clusterMailboxIngest(mb, seal10, w10, K1) != CLUSTER_MAILBOX_OK) {
            ERR("First ingest (apply count 0 < threshold) should succeed");
        }

        /* Next apply will be rejected. Send seq=11 — apply rejects;
         * slot must retain origin_id=5 and last_applied=10 (unchanged). */
        cleanerName(zfree, Bytes) uint8_t *seal11 = zmalloc(sz);
        size_t w11 = 0;
        clusterMailboxSeal(mb, 5u, 11u, 0u, payload, sizeof(payload), K1,
                           seal11, sz, &w11);
        if (clusterMailboxIngest(mb, seal11, w11, K1) !=
            CLUSTER_MAILBOX_ERR_APPLY_REJECTED) {
            ERR("Second ingest should be apply-rejected");
        }
        if (clusterMailboxLastApplied(mb, 5u) != 10u) {
            ERR("Watermark should still be 10 after apply rejection, got "
                "%" PRIu64,
                clusterMailboxLastApplied(mb, 5u));
        }

        /* Attacker replays an OLDER seq=5 for origin 5. Pre-fix this
         * would slip through because the slot's last_applied=0 would
         * make lookupOrigin miss. After fix: lookupOrigin finds the
         * slot via origin_id, the replay check 5 <= 10 fires. */
        cleanerName(zfree, Bytes) uint8_t *replay = zmalloc(sz);
        size_t w_r = 0;
        clusterMailboxSeal(mb, 5u, 5u, 0u, payload, sizeof(payload), K1, replay,
                           sz, &w_r);
        if (clusterMailboxIngest(mb, replay, w_r, K1) !=
            CLUSTER_MAILBOX_ERR_REPLAY) {
            ERR("Older seq must be rejected as REPLAY after apply-reject");
        }

        /* origin_count must still be 1 — the apply rejection must NOT
         * have re-inserted the origin. */
        if (clusterMailboxOriginCount(mb) != 1u) {
            ERR("origin_count corrupted after apply-reject cycle: got %zu, "
                "expected 1",
                clusterMailboxOriginCount(mb));
        }

        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    /* Regression: Snapshot must agree with SnapshotSize on
     * record count, even when the origins table contains slots that
     * are occupied (origin_id != 0) but apply-rejected
     * (last_applied == 0). Pre-fix the size reservation used
     * origin_count while the emit walked last_applied != 0, leaving
     * uninitialized trailing bytes the Restore would then misparse. */
    TEST("snapshot agrees with size after apply-reject leaves dead slot") {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        cmTestCtx ctx = {.reject_threshold = 1u};
        clusterMailbox *mb = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx);

        const uint8_t payload[] = "s";
        const size_t sz = clusterMailboxSealedSize(mb, sizeof(payload));

        /* Origin 50: apply ACCEPTS (apply_count starts at 0). */
        cleanerName(zfree, Bytes) uint8_t *b50 = zmalloc(sz);
        size_t w50 = 0;
        clusterMailboxSeal(mb, 50u, 5u, 0u, payload, sizeof(payload), K1, b50,
                           sz, &w50);
        if (clusterMailboxIngest(mb, b50, w50, K1) != CLUSTER_MAILBOX_OK) {
            ERR("First ingest must succeed");
        }
        /* Origin 60: apply REJECTS (apply_count is now 1 == threshold).
         * Slot is created (origin_id=60) but last_applied stays 0. */
        cleanerName(zfree, Bytes) uint8_t *b60 = zmalloc(sz);
        size_t w60 = 0;
        clusterMailboxSeal(mb, 60u, 7u, 0u, payload, sizeof(payload), K1, b60,
                           sz, &w60);
        if (clusterMailboxIngest(mb, b60, w60, K1) !=
            CLUSTER_MAILBOX_ERR_APPLY_REJECTED) {
            ERR("Second ingest must be apply-rejected");
        }
        /* origin_count is now 2 but only origin 50 has a persistable
         * watermark. Snapshot must contain exactly 1 slot × 3 flex
         * entries — the apply-rejected origin 60 carries no watermark
         * worth persisting. */
        const size_t expected_size = clusterMailboxSnapshotSize(mb);
        uint8_t snap[64];
        size_t snap_written = 0;
        if (!clusterMailboxSnapshot(mb, snap, sizeof(snap), &snap_written)) {
            ERR("Snapshot failed");
        }
        if (snap_written != expected_size) {
            ERR("Snapshot wrote %zu, expected %zu", snap_written,
                expected_size);
        }
        if (flexCount((const flex *)snap) != 4u) {
            ERR("Expected 4 flex entries (the mode, 1 slot), got %zu",
                flexCount((const flex *)snap));
        }
        /* Restore into a fresh mailbox and confirm origin 50 is at
         * seq=5 and origin 60 is unknown. */
        cmTestCtx ctx_b = {};
        clusterMailbox *mb_b = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx_b);
        if (!clusterMailboxRestore(mb_b, snap, snap_written)) {
            ERR("Restore of one-record snapshot failed");
        }
        if (clusterMailboxLastApplied(mb_b, 50u) != 5u) {
            ERR("Origin 50 should be at seq 5 after restore, got %" PRIu64,
                clusterMailboxLastApplied(mb_b, 50u));
        }
        if (clusterMailboxLastApplied(mb_b, 60u) != 0u) {
            ERR("Origin 60 must NOT be persisted (apply-rejected has no "
                "watermark)");
        }

        clusterMailboxFree(mb_b);
        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    TEST("persistOps round-trip: snapshot/restore via the descriptor") {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        cmTestCtx ctx_a = {};
        clusterMailbox *mb_a = clusterMailboxCreateEx(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx_a,
            CLUSTER_MAILBOX_FLAG_REPLAY_WINDOW);

        /* Drive a few origins forward; the replay window will have
         * non-zero bits to persist. */
        const uint8_t payload[] = "p";
        const size_t sz = clusterMailboxSealedSize(mb_a, sizeof(payload));
        const struct {
            uint32_t origin;
            uint64_t seq;
        } ops[] = {{40u, 5u}, {41u, 12u}, {40u, 6u}, {41u, 11u}};
        for (size_t i = 0; i < 4; i++) {
            cleanerName(zfree, Bytes) uint8_t *b = zmalloc(sz);
            size_t w = 0;
            clusterMailboxSeal(mb_a, ops[i].origin, ops[i].seq, 0u, payload,
                               sizeof(payload), K1, b, sz, &w);
            if (clusterMailboxIngest(mb_a, b, w, K1) != CLUSTER_MAILBOX_OK) {
                ERR("setup ingest %zu", i);
            }
        }

        /* Snapshot via the persistOps descriptor (same path the persist
         * framework drives). */
        size_t snap_len = 0;
        cleanerName(zfree, Bytes) uint8_t *snap =
            persistOpsClusterMailbox.snapshot(mb_a, &snap_len);
        if (snap == nullptr || snap_len == 0) {
            ERR("persistOps snapshot");
        }
        if (!flexValidateBufferDeep(snap, snap_len)) {
            ERR("persistOps snapshot must be valid flex");
        }
        if (persistOpsClusterMailbox.count(mb_a) != 2u) {
            ERR("persistOps count should be 2");
        }
        if (persistOpsClusterMailbox.estimateSize(mb_a) != snap_len) {
            ERR("estimateSize must agree with snapshot length");
        }

        /* "Restart": destroy + new mailbox, restore via the direct API
         * (persistOps.restore returns NULL by design — see header). */
        clusterMailboxFree(mb_a);
        cmTestCtx ctx_b = {};
        clusterMailbox *mb_b = clusterMailboxCreateEx(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx_b,
            CLUSTER_MAILBOX_FLAG_REPLAY_WINDOW);
        if (!clusterMailboxRestore(mb_b, snap, snap_len)) {
            ERR("restore from persistOps snapshot");
        }
        if (clusterMailboxLastApplied(mb_b, 40u) != 6u ||
            clusterMailboxLastApplied(mb_b, 41u) != 12u) {
            ERR("watermarks did not survive persistOps round-trip");
        }

        /* Replay-protection survives: a producer trying to re-send
         * (40, 5) or (41, 11) must still be rejected. */
        cleanerName(zfree, Bytes) uint8_t *b = zmalloc(sz);
        size_t w = 0;
        clusterMailboxSeal(mb_b, 40u, 5u, 0u, payload, sizeof(payload), K1, b,
                           sz, &w);
        if (clusterMailboxIngest(mb_b, b, w, K1) !=
            CLUSTER_MAILBOX_ERR_REPLAY) {
            ERR("replay (40,5) must be rejected after restart");
        }
        clusterMailboxSeal(mb_b, 41u, 11u, 0u, payload, sizeof(payload), K1, b,
                           sz, &w);
        if (clusterMailboxIngest(mb_b, b, w, K1) !=
            CLUSTER_MAILBOX_ERR_REPLAY) {
            ERR("replay (41,11) must be rejected after restart "
                "(window state was persisted)");
        }
        clusterMailboxFree(mb_b);
        hmacKeyringFree(ring);
    }

    /* The descriptor's .name string must agree with persistTypeName for
     * the same persistType.  Diagnostic logs pull from both sources and
     * mismatches mislead operators auditing recovery output. */
    TEST("persistOps name agrees with persistTypeName") {
        const char *expected =
            persistTypeName(PERSIST_TYPE_CLUSTER_MAILBOX_WATERMARKS);
        if (expected == nullptr) {
            ERR("persistTypeName returned NULL for "
                "PERSIST_TYPE_CLUSTER_MAILBOX_WATERMARKS");
        } else if (persistOpsClusterMailbox.name == nullptr) {
            ERR("persistOpsClusterMailbox.name is NULL");
        } else if (strcmp(persistOpsClusterMailbox.name, expected) != 0) {
            ERR("descriptor name '%s' must match persistTypeName '%s'",
                persistOpsClusterMailbox.name, expected);
        }
    }

    TEST("origin_id == 0 is refused at Seal and Ingest") {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, K1, &hmacAlgoSHA256, test_key,
                                   sizeof(test_key));
        cmTestCtx ctx = {};
        clusterMailbox *mb = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx);

        /* Seal refuses origin_id == 0. */
        const uint8_t payload[] = "z";
        const size_t sz = clusterMailboxSealedSize(mb, sizeof(payload));
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(sz);
        size_t written = 0;
        if (clusterMailboxSeal(mb, 0u, 1u, 0u, payload, sizeof(payload), K1,
                               buf, sz, &written)) {
            ERR("Seal must refuse origin_id == 0");
        }

        /* Hand-craft an envelope-wrapped frame with inner origin_id==0
         * and feed to Ingest — must be BAD_FRAME, not silently
         * accepted. */
        const size_t inner_len = 13u + sizeof(payload);
        uint8_t inner[64];
        memset(inner, 0, sizeof(inner));
        /* origin_id (4) = 0, sequence (8) = 1, msg_type (1) = 0 */
        inner[4] = 1u; /* sequence=1, bytes 4..11 LE */
        memcpy(inner + 13u, payload, sizeof(payload));
        const flexEnvelopeWrapParams wp = {
            .algo = FLEX_ENV_ALGO_HMAC_SHA256,
            .key = test_key,
            .key_len = sizeof(test_key),
        };
        const size_t wrapped_sz = flexEnvelopeWrappedSize(
            (uint64_t)inner_len, FLEX_ENV_ALGO_HMAC_SHA256, 0);
        cleanerName(zfree, Bytes) uint8_t *wrapped = zmalloc(wrapped_sz);
        size_t w_w = 0;
        flexEnvelopeWrap(wrapped, wrapped_sz, inner, (uint64_t)inner_len, &wp,
                         &w_w);

        const clusterMailboxIngestResult rc =
            clusterMailboxIngest(mb, wrapped, w_w, K1);
        if (rc != CLUSTER_MAILBOX_ERR_BAD_FRAME) {
            ERR("Ingest of origin_id==0 frame must be BAD_FRAME, got %d",
                (int32_t)rc);
        }

        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    TEST_FINAL_RESULT;
}

/* === mailbox window-snapshot orientation =========================
 *
 * Policy under test (Option B): Restore re-orients window_bits into the
 * merged frame (the higher last_applied) and OR's bitmaps. This
 * preserves the "snapshot's record of already-applied sequences cannot
 * be silently forgotten" invariant, closing the silent replay-window
 * downgrade documented in PRODUCTION_RELEASE_PLAN.md §3 the release plan.
 *
 * The test covers four orientations:
 * 1. Snapshot-fresh case from the spec (snap.last_applied=30, advance
 * to 80, restore — bits outside the merged 64-wide window are
 * gracefully dropped; bits inside survive).
 * 2. Bug-reveal case: snapshot strictly BEHIND current mailbox with
 * in-window bits the current doesn't know about — without the
 * fix the bits silently vanish and a subsequent replay of those
 * sequences would be wrongly accepted.
 * 3. Snapshot strictly AHEAD: re-orient current's bits forward into
 * the snapshot frame and OR.
 * 4. Snapshot at exactly the same last_applied: pure OR of bitmaps. */
int dkTestMailboxWindowOrientationTest(int, char *[]) {
    int err = 0;

    const uint8_t key[32] = {0x5d, 0x05, 0xd5, 0x5d, 0x05, 0xd5, 0x5d, 0x05,
                             0xd5, 0x5d, 0x05, 0xd5, 0x5d, 0x05, 0xd5, 0x5d,
                             0x05, 0xd5, 0x5d, 0x05, 0xd5, 0x5d, 0x05, 0xd5,
                             0x5d, 0x05, 0xd5, 0x5d, 0x05, 0xd5, 0x5d, 0x05};
    const hmacKeyId KID = 5u;
    const uint32_t ORIGIN = 1u;

    /* Helper: ingest a single sealed frame at (ORIGIN, seq). */
#define D5_INGEST(mb_, seq_)                                                   \
    do {                                                                       \
        const uint8_t pl[1] = {(uint8_t)((seq_) & 0xFFu)};                     \
        const size_t sz_ = clusterMailboxSealedSize((mb_), sizeof(pl));        \
        cleanerName(zfree, Bytes) uint8_t *b_ = zmalloc(sz_);                  \
        size_t w_ = 0;                                                         \
        if (!clusterMailboxSeal((mb_), ORIGIN, (uint64_t)(seq_), 0u, pl,       \
                                sizeof(pl), KID, b_, sz_, &w_)) {              \
            ERR("snapshotRestore: seal failed (seq=%" PRIu64 ")",              \
                (uint64_t)(seq_));                                             \
        }                                                                      \
        const clusterMailboxIngestResult r_ =                                  \
            clusterMailboxIngest((mb_), b_, w_, KID);                          \
        if (r_ != CLUSTER_MAILBOX_OK) {                                        \
            ERR("snapshotRestore: ingest rc=%d (seq=%" PRIu64 ")",             \
                (int32_t)r_, (uint64_t)(seq_));                                \
        }                                                                      \
    } while (0)

    /* Helper: try to ingest (ORIGIN, seq) and assert the result. */
#define D5_TRY_INGEST(mb_, seq_, expected_)                                    \
    do {                                                                       \
        const uint8_t pl[1] = {(uint8_t)((seq_) & 0xFFu)};                     \
        const size_t sz_ = clusterMailboxSealedSize((mb_), sizeof(pl));        \
        cleanerName(zfree, Bytes) uint8_t *b_ = zmalloc(sz_);                  \
        size_t w_ = 0;                                                         \
        if (!clusterMailboxSeal((mb_), ORIGIN, (uint64_t)(seq_), 0u, pl,       \
                                sizeof(pl), KID, b_, sz_, &w_)) {              \
            ERR("snapshotRestore: try-seal failed (seq=%" PRIu64 ")",          \
                (uint64_t)(seq_));                                             \
        }                                                                      \
        const clusterMailboxIngestResult r_ =                                  \
            clusterMailboxIngest((mb_), b_, w_, KID);                          \
        if (r_ != (expected_)) {                                               \
            ERR("snapshotRestore: expected rc=%d got rc=%d (seq=%" PRIu64 ")", \
                (int32_t)(expected_), (int32_t)r_, (uint64_t)(seq_));          \
        }                                                                      \
    } while (0)

    TEST("spec scenario — snapshot at 30, advance to 80, restore") {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, KID, &hmacAlgoSHA256, key,
                                   sizeof(key));

        /* Snapshot source: ingest sequences 1, 5, 10, 30 (in order).
         * Resulting window state at last_applied=30:
         * bit 0 -> seq 30
         * bit 20 -> seq 10
         * bit 25 -> seq 5
         * bit 29 -> seq 1 */
        cmTestCtx ctx_src = {};
        clusterMailbox *mb_src = clusterMailboxCreateEx(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx_src,
            CLUSTER_MAILBOX_FLAG_REPLAY_WINDOW);
        D5_INGEST(mb_src, 1u);
        D5_INGEST(mb_src, 5u);
        D5_INGEST(mb_src, 10u);
        D5_INGEST(mb_src, 30u);
        if (clusterMailboxLastApplied(mb_src, ORIGIN) != 30u) {
            ERR("snapshotRestore: src last_applied not 30");
        }

        const size_t snap_size = clusterMailboxSnapshotSize(mb_src);
        cleanerName(zfree, Bytes) uint8_t *snap = zmalloc(snap_size);
        size_t snap_written = 0;
        if (!clusterMailboxSnapshot(mb_src, snap, snap_size, &snap_written)) {
            ERR("snapshotRestore: snapshot write failed");
        }
        clusterMailboxFree(mb_src);

        /* Target: fresh mailbox, ingest 1..80. After ingesting 1..64
         * the window has all 64 bits set; ingesting up to 80 shifts the
         * frame forward, but every shift inserts a new bit-0 set, so at
         * last_applied=80 all 64 bits remain set (covering seqs 17..80). */
        cmTestCtx ctx_tgt = {};
        clusterMailbox *mb_tgt = clusterMailboxCreateEx(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx_tgt,
            CLUSTER_MAILBOX_FLAG_REPLAY_WINDOW);
        for (uint64_t s = 1u; s <= 80u; s++) {
            D5_INGEST(mb_tgt, s);
        }
        if (clusterMailboxLastApplied(mb_tgt, ORIGIN) != 80u) {
            ERR("snapshotRestore: tgt last_applied not 80");
        }

        /* Restore — snapshot is behind current; under Policy B bits are
         * re-oriented into current's frame. Snapshot bit 0 (seq 30)
         * shifts to bit 50 in target frame (seq 80-50=30) — already set
         * because target ingested seq 30 directly. Snapshot bits at
         * offsets 20/25/29 (seqs 10/5/1) shift to 70/75/79, off the end:
         * gracefully dropped (those seqs are outside the merged window
         * and would always be ancient-replay-rejected). */
        if (!clusterMailboxRestore(mb_tgt, snap, snap_size)) {
            ERR("snapshotRestore: restore failed (spec scenario)");
        }
        if (clusterMailboxLastApplied(mb_tgt, ORIGIN) != 80u) {
            ERR("snapshotRestore: last_applied must not rewind (monotonic "
                "invariant)");
        }
        /* Replays inside the current window remain rejected. */
        D5_TRY_INGEST(mb_tgt, 30u, CLUSTER_MAILBOX_ERR_REPLAY);
        D5_TRY_INGEST(mb_tgt, 50u, CLUSTER_MAILBOX_ERR_REPLAY);
        D5_TRY_INGEST(mb_tgt, 80u, CLUSTER_MAILBOX_ERR_REPLAY);
        /* Old seqs that fell outside the merged 64-wide window are
         * rejected as ancient replays (REPLAY result), not silently
         * accepted. */
        D5_TRY_INGEST(mb_tgt, 1u, CLUSTER_MAILBOX_ERR_REPLAY);
        D5_TRY_INGEST(mb_tgt, 16u, CLUSTER_MAILBOX_ERR_REPLAY);
        /* Forward progress unaffected. */
        D5_TRY_INGEST(mb_tgt, 81u, CLUSTER_MAILBOX_OK);

        clusterMailboxFree(mb_tgt);
        hmacKeyringFree(ring);
    }

    TEST("bug-reveal — snapshot behind current carries in-window bits") {
        /* A stale snapshot mid-life carrying bits the in-memory mailbox
         * does not have, where those bits ARE in the merged window.
         * Restore keeps them (Option B): dropping the snapshot would make
         * the sequences it applied replayable. */
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, KID, &hmacAlgoSHA256, key,
                                   sizeof(key));

        /* Snapshot: ingest 1 and 10. last_applied=10, bits at
         * offsets 0 (seq 10) and 9 (seq 1). */
        cmTestCtx ctx_src = {};
        clusterMailbox *mb_src = clusterMailboxCreateEx(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx_src,
            CLUSTER_MAILBOX_FLAG_REPLAY_WINDOW);
        D5_INGEST(mb_src, 1u);
        D5_INGEST(mb_src, 10u);
        const size_t snap_size = clusterMailboxSnapshotSize(mb_src);
        cleanerName(zfree, Bytes) uint8_t *snap = zmalloc(snap_size);
        size_t snap_written = 0;
        clusterMailboxSnapshot(mb_src, snap, snap_size, &snap_written);
        clusterMailboxFree(mb_src);

        /* Target: ingest ONLY seq=50. last_applied=50, bits = bit 0
         * only. Crucially, the target has NEVER seen seqs 1 or 10. */
        cmTestCtx ctx_tgt = {};
        clusterMailbox *mb_tgt = clusterMailboxCreateEx(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx_tgt,
            CLUSTER_MAILBOX_FLAG_REPLAY_WINDOW);
        D5_INGEST(mb_tgt, 50u);
        if (clusterMailboxLastApplied(mb_tgt, ORIGIN) != 50u) {
            ERR("snapshotRestore bug-reveal: tgt last_applied != 50");
        }

        /* Restore — Policy B re-orients snap bits forward by gap=40
         * into current frame: bit 0 -> bit 40 (seq 10), bit 9 -> bit
         * 49 (seq 1). After restore current window has bits
         * 0 (seq 50), 40 (seq 10), 49 (seq 1) set; last_applied=50.
         *
         * snap.seq=10 < current.last_applied=50, so a restore that
         * dropped the snapshot would accept a replayed seq 10 as
         * in-window out-of-order: a silent replay-protection
         * downgrade. */
        if (!clusterMailboxRestore(mb_tgt, snap, snap_size)) {
            ERR("snapshotRestore bug-reveal: restore failed");
        }
        if (clusterMailboxLastApplied(mb_tgt, ORIGIN) != 50u) {
            ERR("snapshotRestore bug-reveal: last_applied rewound");
        }

        /* The critical assertion: replaying seq 10 (which the snapshot
         * recorded as already-applied) MUST be rejected. Without the
         * orientation-preserving merge this would be OK (silent bug). */
        D5_TRY_INGEST(mb_tgt, 10u, CLUSTER_MAILBOX_ERR_REPLAY);
        /* Same for seq 1 — within merged window, bit 49 set. */
        D5_TRY_INGEST(mb_tgt, 1u, CLUSTER_MAILBOX_ERR_REPLAY);
        /* Sequences the snapshot didn't cover and the target hasn't
         * seen, but which sit in the window, remain accept-able. */
        D5_TRY_INGEST(mb_tgt, 20u, CLUSTER_MAILBOX_OK);
        /* And forward progress still works. */
        D5_TRY_INGEST(mb_tgt, 51u, CLUSTER_MAILBOX_OK);

        clusterMailboxFree(mb_tgt);
        hmacKeyringFree(ring);
    }

    TEST("snapshot ahead — current bits re-oriented forward into snap frame") {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, KID, &hmacAlgoSHA256, key,
                                   sizeof(key));

        /* Snapshot: ingest 1, 100 (large advance so bitmap is cleared
         * past the first ingest). last_applied=100, bits = bit 0
         * (the bit for seq 1 falls off when ingesting 100, shift=99
         * which is >= 64). */
        cmTestCtx ctx_src = {};
        clusterMailbox *mb_src = clusterMailboxCreateEx(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx_src,
            CLUSTER_MAILBOX_FLAG_REPLAY_WINDOW);
        D5_INGEST(mb_src, 1u);
        D5_INGEST(mb_src, 100u);
        const size_t snap_size = clusterMailboxSnapshotSize(mb_src);
        cleanerName(zfree, Bytes) uint8_t *snap = zmalloc(snap_size);
        size_t snap_written = 0;
        clusterMailboxSnapshot(mb_src, snap, snap_size, &snap_written);
        clusterMailboxFree(mb_src);

        /* Target: ingest 50 and 60. last_applied=60, bits at
         * offsets 0 (seq 60) and 10 (seq 50). */
        cmTestCtx ctx_tgt = {};
        clusterMailbox *mb_tgt = clusterMailboxCreateEx(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx_tgt,
            CLUSTER_MAILBOX_FLAG_REPLAY_WINDOW);
        D5_INGEST(mb_tgt, 50u);
        D5_INGEST(mb_tgt, 60u);

        /* Restore — snap.seq=100 > current.last_applied=60. Current
         * bits are re-oriented forward by gap=40 into snap's frame:
         * bit 0 (seq 60) -> bit 40 (seq 60 in 100-frame)
         * bit 10 (seq 50) -> bit 50 (seq 50 in 100-frame)
         * Then OR with snap.bits (just bit 0 for seq 100). Final:
         * bits {0, 40, 50} set, last_applied=100. */
        if (!clusterMailboxRestore(mb_tgt, snap, snap_size)) {
            ERR("snapshotAhead: restore failed");
        }
        if (clusterMailboxLastApplied(mb_tgt, ORIGIN) != 100u) {
            ERR("snapshotAhead: last_applied should advance to 100");
        }
        D5_TRY_INGEST(mb_tgt, 100u, CLUSTER_MAILBOX_ERR_REPLAY);
        D5_TRY_INGEST(mb_tgt, 60u, CLUSTER_MAILBOX_ERR_REPLAY);
        D5_TRY_INGEST(mb_tgt, 50u, CLUSTER_MAILBOX_ERR_REPLAY);

        clusterMailboxFree(mb_tgt);
        hmacKeyringFree(ring);
    }

    TEST("snapshot at same last_applied — bitmaps OR'd") {
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, KID, &hmacAlgoSHA256, key,
                                   sizeof(key));

        /* Snapshot: ingest 5, 10. last_applied=10, bits at offsets
         * 0 (seq 10) and 5 (seq 5). */
        cmTestCtx ctx_src = {};
        clusterMailbox *mb_src = clusterMailboxCreateEx(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx_src,
            CLUSTER_MAILBOX_FLAG_REPLAY_WINDOW);
        D5_INGEST(mb_src, 5u);
        D5_INGEST(mb_src, 10u);
        const size_t snap_size = clusterMailboxSnapshotSize(mb_src);
        cleanerName(zfree, Bytes) uint8_t *snap = zmalloc(snap_size);
        size_t snap_written = 0;
        clusterMailboxSnapshot(mb_src, snap, snap_size, &snap_written);
        clusterMailboxFree(mb_src);

        /* Target: ingest 3, 10. last_applied=10, bits at offsets
         * 0 (seq 10) and 7 (seq 3). */
        cmTestCtx ctx_tgt = {};
        clusterMailbox *mb_tgt = clusterMailboxCreateEx(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmRecordApply, &ctx_tgt,
            CLUSTER_MAILBOX_FLAG_REPLAY_WINDOW);
        D5_INGEST(mb_tgt, 3u);
        D5_INGEST(mb_tgt, 10u);

        if (!clusterMailboxRestore(mb_tgt, snap, snap_size)) {
            ERR("snapshotSame: restore failed");
        }
        /* Final bits should be {0, 5, 7}: seqs {10, 5, 3} all applied. */
        D5_TRY_INGEST(mb_tgt, 5u, CLUSTER_MAILBOX_ERR_REPLAY);
        D5_TRY_INGEST(mb_tgt, 3u, CLUSTER_MAILBOX_ERR_REPLAY);
        D5_TRY_INGEST(mb_tgt, 10u, CLUSTER_MAILBOX_ERR_REPLAY);
        /* An unapplied in-window seq remains accept-able. */
        D5_TRY_INGEST(mb_tgt, 4u, CLUSTER_MAILBOX_OK);

        clusterMailboxFree(mb_tgt);
        hmacKeyringFree(ring);
    }

    TEST("snapshot a whole window or more away — the far bits drop") {
        /* Watermarks 90 apart, either way round: re-orienting a window
         * by 64 or more bits drops it (a shift that wide would be
         * undefined), the watermark is the higher of the two, and only
         * what that frame saw stays rejected */
        cleaner(hmacKeyringFree) hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, KID, &hmacAlgoSHA256, key,
                                   sizeof(key));
        for (int snapshotAhead = 0; snapshotAhead <= 1; snapshotAhead++) {
            /* low: 1..10; high: 40..100 (39 is in its window, unseen) */
            cmTestCtx ctx_src = {};
            cmTestCtx ctx_tgt = {};
            cleaner(clusterMailboxFree) clusterMailbox *mb_src =
                clusterMailboxCreateEx(ring, FLEX_ENV_ALGO_HMAC_SHA256,
                                       cmRecordApply, &ctx_src,
                                       CLUSTER_MAILBOX_FLAG_REPLAY_WINDOW);
            cleaner(clusterMailboxFree) clusterMailbox *mb_tgt =
                clusterMailboxCreateEx(ring, FLEX_ENV_ALGO_HMAC_SHA256,
                                       cmRecordApply, &ctx_tgt,
                                       CLUSTER_MAILBOX_FLAG_REPLAY_WINDOW);
            clusterMailbox *low = snapshotAhead ? mb_tgt : mb_src;
            clusterMailbox *high = snapshotAhead ? mb_src : mb_tgt;
            for (uint64_t s = 1u; s <= 10u; s++) {
                D5_INGEST(low, s);
            }
            for (uint64_t s = 40u; s <= 100u; s++) {
                D5_INGEST(high, s);
            }
            const size_t snap_size = clusterMailboxSnapshotSize(mb_src);
            cleanerName(zfree, Bytes) uint8_t *snap = zmalloc(snap_size);
            size_t snap_written = 0;
            if (!clusterMailboxSnapshot(mb_src, snap, snap_size,
                                        &snap_written) ||
                !clusterMailboxRestore(mb_tgt, snap, snap_written) ||
                clusterMailboxLastApplied(mb_tgt, ORIGIN) != 100u) {
                ERR("snapshot %s by 90: restore or watermark wrong",
                    snapshotAhead ? "ahead" : "behind");
            }
            D5_TRY_INGEST(mb_tgt, 100u, CLUSTER_MAILBOX_ERR_REPLAY);
            D5_TRY_INGEST(mb_tgt, 70u, CLUSTER_MAILBOX_ERR_REPLAY);
            D5_TRY_INGEST(mb_tgt, 5u, CLUSTER_MAILBOX_ERR_REPLAY);
            D5_TRY_INGEST(mb_tgt, 39u, CLUSTER_MAILBOX_OK);
            D5_TRY_INGEST(mb_tgt, 101u, CLUSTER_MAILBOX_OK);
        }
    }

#undef D5_INGEST
#undef D5_TRY_INGEST

    TEST_FINAL_RESULT;
}

/* === mailbox stats-accessor race =================================
 *
 * Policy under test (Option A): clusterMailboxLastApplied and
 * clusterMailboxOriginCount are safe to invoke from observer threads
 * while a single writer thread races through Ingest. Atomicity is
 * carried by `_Atomic` storage on origin_id / last_applied /
 * origin_count plus matched release/acquire memory orders.
 *
 * Coverage: spawn one writer thread that Seals + Ingests a long
 * monotone sequence under origin 1, and one reader thread that hammers
 * the two stats accessors. Invariants asserted on the reader:
 * - origin_count is always in [0, 1] (only one origin is in play).
 * - last_applied is monotonic non-decreasing across consecutive reads
 * by the same reader.
 * - last_applied never exceeds the highest sequence the writer has
 * yet handed off (it can lag, but it can never lead — that would
 * indicate a torn read).
 *
 * Limitation note: real data-race detection requires ThreadSanitizer
 * (TSAN), which the project's dev/safe presets do NOT enable
 * (ASAN+UBSAN don't catch reads/writes without synchronisation). This
 * test still serves as a smoke test against crashes/UB and as a
 * mechanical assertion on the invariants above — when run under a
 * TSAN-built binary (cmake -DEnableTSAN=ON), it doubles as the
 * race-detection regression. */
typedef struct cmStatsRaceCtx {
    clusterMailbox *mb;
    hmacKeyId key_id;
    uint32_t origin;
    _Atomic uint64_t
        writer_high_water; /* highest seq the writer has SEALED so far */
    _Atomic int writer_done;
    _Atomic int writer_seal_failures;
    _Atomic int writer_ingest_failures;
    _Atomic int reader_invariant_failures;
    _Atomic uint64_t reader_iterations;
} cmStatsRaceCtx;

static bool cmStatsRaceApply(const clusterFrame *, void *) {
    return true;
}

static void *cmStatsRaceWriter(void *vctx) {
    cmStatsRaceCtx *c = (cmStatsRaceCtx *)vctx;
    const size_t WRITER_ITERS = 1000u;
    uint8_t payload[16];
    memset(payload, 0xAB, sizeof(payload));
    const size_t cap = clusterMailboxSealedSize(c->mb, sizeof(payload));
    /* Braced so the seal buffer is released before the done signal. */
    {
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(cap);
        for (size_t i = 1; i <= WRITER_ITERS; i++) {
            const uint64_t seq = (uint64_t)i;
            size_t written = 0;
            /* Publish the high-water BEFORE Seal so a reader can sample
             * "writer-claims-to-have-tried-up-to N" and compare last_applied
             * against it; last_applied may legitimately equal seq once
             * Ingest commits, but must never exceed it. The store is
             * sequenced before Ingest's watermark release, which is what
             * orders it for the reader. */
            /* @sync: relaxed counter */
            atomic_store_explicit(&c->writer_high_water, seq,
                                  memory_order_relaxed);
            if (!clusterMailboxSeal(c->mb, c->origin, seq, 0u, payload,
                                    sizeof(payload), c->key_id, buf, cap,
                                    &written)) {
                /* @sync: relaxed counter */
                atomic_fetch_add_explicit(&c->writer_seal_failures, 1,
                                          memory_order_relaxed);
                continue;
            }
            const clusterMailboxIngestResult rc =
                clusterMailboxIngest(c->mb, buf, written, c->key_id);
            if (rc != CLUSTER_MAILBOX_OK) {
                /* @sync: relaxed counter */
                atomic_fetch_add_explicit(&c->writer_ingest_failures, 1,
                                          memory_order_relaxed);
            }
        }
    }
    /* Nothing reads writer_done; the main thread joins instead. */
    /* @sync: relaxed hint */
    atomic_store_explicit(&c->writer_done, 1, memory_order_relaxed);
    return nullptr;
}

static void *cmStatsRaceReader(void *vctx) {
    cmStatsRaceCtx *c = (cmStatsRaceCtx *)vctx;
    const size_t READER_ITERS = 5000u;
    uint64_t last_seen = 0u;
    for (size_t i = 0; i < READER_ITERS; i++) {
        /* Read last_applied FIRST, then the writer's high-water.  The
         * writer publishes a seq's high-water before it ingests that seq,
         * so whatever last_applied this read saw, the high-water read after
         * it is at least as large; last_applied > that high-water is a torn
         * or out-of-thin-air read.  (Sampling the high-water first would
         * let the writer advance and ingest between the two reads, a
         * legitimate interleaving that reports a false failure.) The
         * acquire inside clusterMailboxLastApplied (mailbox.watermark)
         * makes the high-water store of seq happen before this load. */
        const uint64_t la = clusterMailboxLastApplied(c->mb, c->origin);
        /* @sync: relaxed counter */
        const uint64_t hw =
            atomic_load_explicit(&c->writer_high_water, memory_order_relaxed);
        const size_t oc = clusterMailboxOriginCount(c->mb);

        if (la > hw) {
            /* @sync: relaxed counter */
            atomic_fetch_add_explicit(&c->reader_invariant_failures, 1,
                                      memory_order_relaxed);
        }
        if (la < last_seen) {
            /* Monotonic violation -- writer never rewinds in this
             * test, so an earlier value reappearing indicates a torn
             * or out-of-thin-air read. */
            /* @sync: relaxed counter */
            atomic_fetch_add_explicit(&c->reader_invariant_failures, 1,
                                      memory_order_relaxed);
        } else {
            last_seen = la;
        }
        if (oc > 1u) {
            /* Only one origin is in play. */
            /* @sync: relaxed counter */
            atomic_fetch_add_explicit(&c->reader_invariant_failures, 1,
                                      memory_order_relaxed);
        }
        /* @sync: relaxed counter */
        atomic_fetch_add_explicit(&c->reader_iterations, 1,
                                  memory_order_relaxed);
    }
    return nullptr;
}

int dkTestMailboxStatsRaceTest(int, char *[]) {
    int err = 0;

    const uint8_t key[32] = {0xd6, 0xd6, 0xd6, 0xd6, 0xd6, 0xd6, 0xd6, 0xd6,
                             0xd6, 0xd6, 0xd6, 0xd6, 0xd6, 0xd6, 0xd6, 0xd6,
                             0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06,
                             0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06};
    const hmacKeyId KID = 6u;
    const uint32_t ORIGIN = 1u;

    TEST("concurrent stats accessors observe coherent values") {
        hmacKeyring *ring = hmacKeyringCreate();
        if (!hmacKeyringInstallUnsalted(ring, KID, &hmacAlgoSHA256, key,
                                        sizeof(key))) {
            ERR("statsRace: keyring install failed");
        }
        clusterMailbox *mb = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, cmStatsRaceApply, nullptr);
        if (mb == nullptr) {
            ERR("statsRace: mailbox create failed");
        }

        cmStatsRaceCtx ctx = {};
        ctx.mb = mb;
        ctx.key_id = KID;
        ctx.origin = ORIGIN;

        pthread_t writer_thr;
        pthread_t reader_thr;
        if (pthread_create(&writer_thr, nullptr, cmStatsRaceWriter, &ctx) !=
            0) {
            ERR("statsRace: spawn writer failed");
        }
        if (pthread_create(&reader_thr, nullptr, cmStatsRaceReader, &ctx) !=
            0) {
            ERR("statsRace: spawn reader failed");
        }
        pthread_join(writer_thr, nullptr);
        pthread_join(reader_thr, nullptr);

        /* @sync: relaxed init */
        const int seal_fail = atomic_load_explicit(&ctx.writer_seal_failures,
                                                   memory_order_relaxed);
        /* @sync: relaxed init */
        const int ingest_fail = atomic_load_explicit(
            &ctx.writer_ingest_failures, memory_order_relaxed);
        /* @sync: relaxed init */
        const int reader_fail = atomic_load_explicit(
            &ctx.reader_invariant_failures, memory_order_relaxed);
        /* @sync: relaxed init */
        const uint64_t reader_iters =
            atomic_load_explicit(&ctx.reader_iterations, memory_order_relaxed);

        if (seal_fail != 0) {
            ERR("statsRace: writer Seal failures: %d", seal_fail);
        }
        if (ingest_fail != 0) {
            ERR("statsRace: writer Ingest failures: %d", ingest_fail);
        }
        if (reader_fail != 0) {
            ERR("statsRace: reader invariant failures: %d", reader_fail);
        }
        if (reader_iters == 0u) {
            ERR("statsRace: reader did not run any iterations");
        }
        if (clusterMailboxLastApplied(mb, ORIGIN) != 1000u) {
            ERR("statsRace: final last_applied != 1000, got %" PRIu64,
                clusterMailboxLastApplied(mb, ORIGIN));
        }
        if (clusterMailboxOriginCount(mb) != 1u) {
            ERR("statsRace: final origin_count != 1, got %zu",
                clusterMailboxOriginCount(mb));
        }

        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    TEST_FINAL_RESULT;
}

/* === mailbox stats cache-line alignment ==========================
 *
 * SMP hardening. The 14 stats counters packed into one 112B struct
 * would straddle two cache lines; if the single-writer/multi-reader
 * model relaxes to multi-writer ingest, the rejection counters would
 * ping-pong the line that holds the hot counters on every error path.
 * The struct is ordered and padded so the 4 hot counters (ingest_total,
 * apply_ok, bytes_ingested, bytes_sealed) sit on cache line 1 and the 10 cold
 * rejection counters sit on cache line 2. The static_assert at the struct
 * definition is the primary proof; this runtime test is a belt-and-braces check
 * that the compiler honoured alignas through the embedding `clusterMailbox`
 * allocation pipeline (zmalloc). If zmalloc ever stopped returning
 * 64B-aligned pointers for an aligned struct, this test would catch
 * it. */
int dkTestMailboxStatsAlignmentTest(int, char *[]) {
    int err = 0;

    TEST("mailbox stats counters land on their dedicated cache lines") {
        const uint8_t key[32] = {
            0xd7, 0xd7, 0xd7, 0xd7, 0xd7, 0xd7, 0xd7, 0xd7, 0xd7, 0xd7, 0xd7,
            0xd7, 0xd7, 0xd7, 0xd7, 0xd7, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
            0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07};
        hmacKeyring *ring = hmacKeyringCreate();
        if (!hmacKeyringInstallUnsalted(ring, 7u, &hmacAlgoSHA256, key,
                                        sizeof(key))) {
            ERR("cacheLineLayout: keyring install failed");
        }
        clusterMailbox *mb = clusterMailboxCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, nullptr, nullptr);
        if (mb == nullptr) {
            ERR("cacheLineLayout: mailbox create failed");
        }

        /* Hot block starts at offset 0 of `counters`; both the struct
         * and its containing parent are 64B-aligned by virtue of the
         * alignas on the first counter. */
        const uintptr_t hot = (uintptr_t)&mb->counters.ingest_total;
        if ((hot % 64u) != 0u) {
            ERR("cacheLineLayout: hot counters not on cache line: addr=%p, "
                "mod=%zu",
                (void *)hot, (size_t)(hot % 64u));
        }
        /* Cold block must be on its own cache line. */
        const uintptr_t cold = (uintptr_t)&mb->counters.apply_rejected;
        if ((cold % 64u) != 0u) {
            ERR("cacheLineLayout: cold counters not on cache line: addr=%p, "
                "mod=%zu",
                (void *)cold, (size_t)(cold % 64u));
        }
        /* Hot and cold are on DIFFERENT lines (cold address must be at
         * least one line past the hot start). */
        if (cold < hot + 64u) {
            ERR("cacheLineLayout: hot and cold blocks share a cache line: "
                "hot=%p cold=%p delta=%zu",
                (void *)hot, (void *)cold, (size_t)(cold - hot));
        }

        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    TEST_FINAL_RESULT;
}

/* === Concurrent retire-during-Seal/Ingest =================================
 *
 * cmSealImpl and cmIngestImpl copy the keyring raw bytes into a stack
 * buffer via hmacKeyringGetRawCopy before the envelope HMAC reads them.
 * This test exercises the race that the copy closes:
 *
 * Writer thread:  a direct hmacKeyringGetRawCopy of key id K, then
 *                 Seal + Ingest under K, in a loop.
 * Rotator thread: Retire(K), yield, Reinstall(K) with identical bytes,
 *                 yield, in a loop.
 *
 * With a borrowed pointer (hmacKeyringGetRaw) the writer could hash
 * bytes that a concurrent Retire has already zeroed or freed. With the
 * copy, every read either fails cleanly (K is not installed at that
 * instant) or returns the complete installed key. Every reinstall uses
 * the same bytes, so the test pins these deterministic properties:
 *
 *   - a GetRawCopy that returns true yields exactly the installed key
 *     bytes and the SHA-256 descriptor and writes nothing past the key
 *     length; one that returns false reports length 0 and leaves the
 *     buffer and the algo pointer untouched;
 *   - after a successful Seal, Ingest returns OK or KEY_MISSING. It
 *     never returns VERIFY_FAILED (a torn or zeroed key on either side)
 *     and never REPLAY (the writer's sequences strictly increase);
 *   - every OK Ingest runs the apply callback exactly once, with the
 *     sealed origin, sequence, msg_type and payload bytes intact;
 *   - the rotator is the only mutator, so every Retire and every
 *     Reinstall succeeds, and after both threads join K is installed
 *     and one Seal + Ingest returns OK.
 *
 * Progress is counted, not timed. The writer starts only once the
 * rotator is running, and the rotator keeps rotating until it has done
 * CM_RACE_TARGET_ROTATIONS rotations and the writer has completed
 * CM_RACE_TARGET_OK applied round trips, so every counted round trip
 * overlaps live rotation. CM_RACE_ROTATION_CAP bounds the run: a writer
 * that cannot make progress fails the test instead of hanging it. Under
 * ASan a regression to a borrowed pointer surfaces as a
 * heap-use-after-free in the envelope hash. */
#define CM_RACE_TARGET_OK ((uint64_t)1000)
#define CM_RACE_TARGET_ROTATIONS ((uint64_t)1000)
#define CM_RACE_ROTATION_CAP ((uint64_t)2'000'000)
static constexpr unsigned CM_RACE_PAYLOAD_LEN = 32u;
static constexpr unsigned CM_RACE_PAYLOAD_BYTE = 0x5Au;
static constexpr unsigned CM_RACE_MSG_TYPE = 3u;
static constexpr unsigned CM_RACE_COPY_CAP = 64u;
static constexpr unsigned CM_RACE_COPY_SENTINEL = 0xEEu;

typedef struct cmRetireRaceCtx {
    clusterMailbox *mb;
    hmacKeyring *ring;
    hmacKeyId key_id;
    const uint8_t *key_bytes;
    size_t key_len;
    uint32_t origin;

    /* Shared between the threads while they run. */
    _Atomic int rotator_started;
    _Atomic int rotator_done;
    _Atomic uint64_t writer_ok; /* applied Seal + Ingest round trips */

    /* Written only by the rotator; read after join. */
    uint64_t rotations;
    uint64_t rotator_failures; /* a Retire or Reinstall returned false */

    /* Written only by the thread calling Seal/Ingest (the writer, then
     * the main thread after join); the apply callback runs inside
     * Ingest on that same thread. */
    uint64_t next_seq;
    uint64_t expect_seq;
    uint64_t applied;
    uint64_t apply_bad;    /* applied frame did not match what was sealed */
    uint64_t result_bad;   /* Ingest result other than OK / KEY_MISSING */
    uint64_t key_missing;  /* Seal or Ingest found K retired */
    uint64_t copies_ok;    /* GetRawCopy returned the complete key */
    uint64_t copies_empty; /* GetRawCopy found K retired */
    uint64_t copies_bad;   /* GetRawCopy result broke its contract */
} cmRetireRaceCtx;

static bool cmRetireRaceApply(const clusterFrame *f, void *vctx) {
    cmRetireRaceCtx *c = (cmRetireRaceCtx *)vctx;
    bool good = f->origin_id == c->origin && f->sequence == c->expect_seq &&
                f->msg_type == CM_RACE_MSG_TYPE &&
                f->payload_len == CM_RACE_PAYLOAD_LEN;
    for (size_t i = 0; good && i < f->payload_len; i++) {
        good = f->payload[i] == CM_RACE_PAYLOAD_BYTE;
    }
    if (!good) {
        c->apply_bad++;
        return false;
    }
    c->applied++;
    return true;
}

/* Read K by copy and check the result against the GetRawCopy contract. */
static void cmRetireRaceCheckCopy(cmRetireRaceCtx *c) {
    uint8_t copy[CM_RACE_COPY_CAP];
    memset(copy, CM_RACE_COPY_SENTINEL, sizeof(copy));
    size_t len = SIZE_MAX;
    const hmacHashAlgo *algo = nullptr;
    const bool found = hmacKeyringGetRawCopy(c->ring, c->key_id, copy,
                                             sizeof(copy), &len, &algo);
    bool good;
    size_t untouchedFrom;
    if (found) {
        good = len == c->key_len && algo == &hmacAlgoSHA256 &&
               memcmp(copy, c->key_bytes, c->key_len) == 0;
        untouchedFrom = c->key_len;
    } else {
        good = len == 0 && algo == nullptr;
        untouchedFrom = 0;
    }
    for (size_t i = untouchedFrom; good && i < sizeof(copy); i++) {
        good = copy[i] == CM_RACE_COPY_SENTINEL;
    }
    if (!good) {
        c->copies_bad++;
    } else if (found) {
        c->copies_ok++;
    } else {
        c->copies_empty++;
    }
    dkSecureZero(copy, sizeof(copy));
}

/* One Seal + Ingest under K with the next sequence. Returns the Ingest
 * result, or CLUSTER_MAILBOX_ERR_KEY_MISSING when Seal could not read K.
 * Counts every outcome that breaks the round-trip contract. */
static clusterMailboxIngestResult
cmRetireRaceRoundTrip(cmRetireRaceCtx *c, uint8_t *buf, size_t cap) {
    uint8_t payload[CM_RACE_PAYLOAD_LEN];
    memset(payload, CM_RACE_PAYLOAD_BYTE, sizeof(payload));
    const uint64_t seq = ++c->next_seq;
    size_t written = 0;
    if (!clusterMailboxSeal(c->mb, c->origin, seq, CM_RACE_MSG_TYPE, payload,
                            sizeof(payload), c->key_id, buf, cap, &written)) {
        /* With valid arguments Seal fails only when K is retired at the
         * instant it copies the key. */
        c->key_missing++;
        return CLUSTER_MAILBOX_ERR_KEY_MISSING;
    }
    c->expect_seq = seq;
    const uint64_t appliedBefore = c->applied;
    const clusterMailboxIngestResult rc =
        clusterMailboxIngest(c->mb, buf, written, c->key_id);
    if (rc == CLUSTER_MAILBOX_OK) {
        if (c->applied != appliedBefore + 1u) {
            c->result_bad++;
        }
    } else if (rc == CLUSTER_MAILBOX_ERR_KEY_MISSING) {
        c->key_missing++;
    } else {
        c->result_bad++;
    }
    return rc;
}

static void *cmRetireRaceWriter(void *vctx) {
    cmRetireRaceCtx *c = (cmRetireRaceCtx *)vctx;
    const size_t cap = clusterMailboxSealedSize(c->mb, CM_RACE_PAYLOAD_LEN);
    cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(cap);
    /* Wait for the rotator so every round trip overlaps rotation. The
     * done check covers a rotator that failed to start. The flags only
     * pace the threads; the writer reads nothing the rotator wrote
     * before them (keyring state is ordered by the keyring's lock). */
    /* @sync: relaxed hint */
    while (
        !atomic_load_explicit(&c->rotator_started, memory_order_relaxed) &&
        !atomic_load_explicit(&c->rotator_done,
                              memory_order_relaxed)) { /* @sync: relaxed hint */
        sched_yield();
    }
    /* @sync: relaxed hint */
    while (!atomic_load_explicit(&c->rotator_done, memory_order_relaxed)) {
        cmRetireRaceCheckCopy(c);
        if (cmRetireRaceRoundTrip(c, buf, cap) == CLUSTER_MAILBOX_OK) {
            /* @sync: relaxed counter */
            atomic_fetch_add_explicit(&c->writer_ok, 1u, memory_order_relaxed);
        }
    }
    return nullptr;
}

static void *cmRetireRaceRotator(void *vctx) {
    cmRetireRaceCtx *c = (cmRetireRaceCtx *)vctx;
    /* @sync: relaxed hint */
    atomic_store_explicit(&c->rotator_started, 1, memory_order_relaxed);
    uint64_t i = 0;
    for (; i < CM_RACE_ROTATION_CAP; i++) {
        /* The count only decides when to stop; nothing is read through it. */
        /* @sync: relaxed counter */
        if (i >= CM_RACE_TARGET_ROTATIONS &&
            atomic_load_explicit(&c->writer_ok, memory_order_relaxed) >=
                CM_RACE_TARGET_OK) {
            break;
        }
        if (!hmacKeyringRetire(c->ring, c->key_id)) {
            c->rotator_failures++;
        }
        sched_yield();
        if (!hmacKeyringInstallUnsalted(c->ring, c->key_id, &hmacAlgoSHA256,
                                        c->key_bytes, c->key_len)) {
            c->rotator_failures++;
        }
        sched_yield();
    }
    c->rotations = i;
    /* `rotations` reaches the main thread through pthread_join. */
    /* @sync: relaxed hint */
    atomic_store_explicit(&c->rotator_done, 1, memory_order_relaxed);
    return nullptr;
}

int dkTestKeyringGetRawCopyMigrationClusterTest(int, char *[]) {
    int err = 0;

    const uint8_t key[32] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11,
                             0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
                             0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
                             0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10};
    const hmacKeyId KID = 42u;
    const uint32_t ORIGIN = 7u;

    TEST(
        "Seal/Ingest under concurrent Retire+Reinstall — no UAF, no torn key") {
        cmRetireRaceCtx ctx = {};
        ctx.key_id = KID;
        ctx.key_bytes = key;
        ctx.key_len = sizeof(key);
        ctx.origin = ORIGIN;

        hmacKeyring *ring = hmacKeyringCreate();
        clusterMailbox *mb = nullptr;
        if (ring == nullptr ||
            !hmacKeyringInstallUnsalted(ring, KID, &hmacAlgoSHA256, key,
                                        sizeof(key))) {
            ERR("retireRace: initial keyring install failed");
        } else {
            mb = clusterMailboxCreate(ring, FLEX_ENV_ALGO_HMAC_SHA256,
                                      cmRetireRaceApply, &ctx);
            if (mb == nullptr) {
                ERR("retireRace: mailbox create failed");
            }
        }
        ctx.mb = mb;
        ctx.ring = ring;

        bool ran = false;
        if (mb != nullptr) {
            pthread_t writer_thr;
            pthread_t rotator_thr;
            if (pthread_create(&writer_thr, nullptr, cmRetireRaceWriter,
                               &ctx) != 0) {
                ERR("retireRace: spawn writer failed");
            } else {
                if (pthread_create(&rotator_thr, nullptr, cmRetireRaceRotator,
                                   &ctx) != 0) {
                    ERR("retireRace: spawn rotator failed");
                    /* Release the writer from its start wait. */
                    /* @sync: relaxed hint */
                    atomic_store_explicit(&ctx.rotator_done, 1,
                                          memory_order_relaxed);
                } else {
                    pthread_join(rotator_thr, nullptr);
                    ran = true;
                }
                pthread_join(writer_thr, nullptr);
            }
        }

        if (ran) {
            /* @sync: relaxed init */
            const uint64_t writerOk =
                atomic_load_explicit(&ctx.writer_ok, memory_order_relaxed);
            if (writerOk < CM_RACE_TARGET_OK) {
                ERR("retireRace: writer completed only %" PRIu64 " of %" PRIu64
                    " applied round trips before the rotator hit its cap of "
                    "%" PRIu64 " rotations",
                    writerOk, CM_RACE_TARGET_OK, CM_RACE_ROTATION_CAP);
            }
            if (ctx.rotations < CM_RACE_TARGET_ROTATIONS) {
                ERR("retireRace: rotator did only %" PRIu64 " of %" PRIu64
                    " rotations",
                    ctx.rotations, CM_RACE_TARGET_ROTATIONS);
            }
            if (ctx.rotator_failures != 0) {
                ERR("retireRace: %" PRIu64 " Retire/Reinstall calls failed "
                    "with the rotator as the only mutator",
                    ctx.rotator_failures);
            }
            if (ctx.copies_bad != 0) {
                ERR("retireRace: %" PRIu64 " of %" PRIu64
                    " GetRawCopy reads returned a torn, wrong or overrun "
                    "copy, or touched the buffer on failure",
                    ctx.copies_bad,
                    ctx.copies_ok + ctx.copies_empty + ctx.copies_bad);
            }
            if (ctx.result_bad != 0) {
                ERR("retireRace: %" PRIu64 " round trips ended in a result "
                    "other than OK or KEY_MISSING, or applied the wrong "
                    "number of frames",
                    ctx.result_bad);
            }
            if (ctx.apply_bad != 0) {
                ERR("retireRace: %" PRIu64 " applied frames did not match "
                    "the sealed origin, sequence, msg_type or payload",
                    ctx.apply_bad);
            }
            if (ctx.applied != writerOk) {
                ERR("retireRace: apply ran %" PRIu64 " times for %" PRIu64
                    " OK round trips",
                    ctx.applied, writerOk);
            }

            /* Quiescent: the rotator's last action was a Reinstall, so K
             * is installed and a full copy and a round trip must work. */
            const uint64_t copiesOkBefore = ctx.copies_ok;
            cmRetireRaceCheckCopy(&ctx);
            if (ctx.copies_ok != copiesOkBefore + 1u) {
                ERR("retireRace: GetRawCopy after join did not return the "
                    "complete key");
            }
            const size_t cap =
                clusterMailboxSealedSize(mb, CM_RACE_PAYLOAD_LEN);
            cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(cap);
            const clusterMailboxIngestResult rc =
                cmRetireRaceRoundTrip(&ctx, buf, cap);
            if (rc != CLUSTER_MAILBOX_OK || ctx.applied != writerOk + 1u) {
                ERR("retireRace: Seal + Ingest after join returned %d "
                    "(applied %" PRIu64 ", expected %" PRIu64 ")",
                    (int)rc, ctx.applied, writerOk + 1u);
            }
        }

        clusterMailboxFree(mb);
        hmacKeyringFree(ring);
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
