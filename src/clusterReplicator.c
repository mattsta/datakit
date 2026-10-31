/* clusterReplicator — see header for the design.
 *
 * Implementation note: the replicator OWNS its clusterMailbox. The
 * mailbox's apply callback is wired to a trampoline that:
 * 1. Reads the persistType from the verified frame's msg_type byte.
 * 2. Looks up the persistOps descriptor in the caller-supplied
 * type table.
 * 3. Calls ops->restore(payload, len) to materialize a fresh
 * structure.
 * 4. Hands the structure to the application's apply fn.
 * 5. On apply rejection, ops->free's the structure so no leak.
 *
 * That keeps the security stack composable with EVERY datakit data
 * structure that already speaks persistOps — flex, multimap,
 * multilist, intset, hyperloglog, multiroar, etc. */

#include "clusterReplicator.h"

#include "cleaner.h"
#include "datakit.h" /* zmalloc / zfree */

#include <stdatomic.h>
#include <string.h>

defcleanerName(clusterReplicator *, zfree, Replicator);
defcleanerName(uint8_t *, zfree, Wire);

/* Track the last frame on which we observed APPLY_REJECTED so the
 * trampoline can count consecutive rejections on the SAME (origin,
 * sequence) pair. A different (origin, sequence) resets the counter —
 * that's the correct policy: the retry budget is for "the producer
 * keeps retransmitting the same frame and we keep refusing it", not
 * "any rejection anywhere within a session". */
typedef struct replicatorRejectState {
    uint32_t origin_id; /* 0: a free tracker (origin 0 is reserved) */
    uint64_t sequence;
    uint32_t consecutive_rejects;
} replicatorRejectState;

/* One tracker per origin with a rejected frame, so frames between
 * retransmissions (another origin's, accepted or not) leave each
 * origin's count alone; past this many origins the one with the fewest
 * rejects gives up its tracker */
static constexpr size_t REPLICATOR_REJECT_TRACKERS = 16;

struct clusterReplicator {
    clusterMailbox *mb;                  /* OWNED — freed in Free */
    const persistOps *const *type_table; /* borrowed */
    size_t type_table_len;
    clusterReplicatorApplyFn on_restored;
    void *apply_ctx;
    uint32_t
        max_apply_retries; /* CLUSTER_REPLICATOR_RETRY_FOREVER: never drop */
    /* trampoline-local, sole writer */
    replicatorRejectState rejects[REPLICATOR_REJECT_TRACKERS];
    _Atomic uint64_t dropped_count;
    _Atomic uint64_t format_refused_count;
};

/* Type-table sanity check: every populated entry must have a non-NULL
 * `free` (the trampoline always allocates a fresh structure via
 * .restore and MUST be able to free it on the reject path). Returns
 * the index of the first offender, or `len` if every populated entry
 * is well-formed. Tested separately from create so callers that need
 * a diagnostic can introspect; create itself just rejects. */
static size_t replicatorAuditTypeTable(const persistOps *const *table,
                                       size_t len) {
    for (size_t i = 0; i < len; i++) {
        const persistOps *ops = table[i];
        if (ops == nullptr) {
            continue;
        }
        if (ops->free == nullptr) {
            return i;
        }
    }
    return len;
}

/* The tracker 'origin' holds, or NULL */
static replicatorRejectState *replicatorRejectFind_(clusterReplicator *rep,
                                                    uint32_t origin) {
    for (size_t i = 0; i < REPLICATOR_REJECT_TRACKERS; i++) {
        if (rep->rejects[i].origin_id == origin) {
            return &rep->rejects[i];
        }
    }
    return nullptr;
}

/* Mailbox apply trampoline. The ctx is the replicator itself. */
/* The tracker of 'origin': its own, else a free one, else the one with
 * the fewest rejects (handed over empty) */
static replicatorRejectState *replicatorRejectTracker_(clusterReplicator *rep,
                                                       uint32_t origin) {
    replicatorRejectState *free = nullptr;
    replicatorRejectState *least = &rep->rejects[0];
    for (size_t i = 0; i < REPLICATOR_REJECT_TRACKERS; i++) {
        replicatorRejectState *t = &rep->rejects[i];
        if (t->origin_id == origin) {
            return t;
        }
        if (!t->origin_id && !free) {
            free = t;
        }
        if (t->consecutive_rejects < least->consecutive_rejects) {
            least = t;
        }
    }
    replicatorRejectState *taken = free ? free : least;
    *taken = (replicatorRejectState){};
    return taken;
}

static bool replicatorMailboxApply(const clusterFrame *frame, void *ctx) {
    clusterReplicator *rep = (clusterReplicator *)ctx;
    const uint8_t type_byte = frame->msg_type;
    if ((size_t)type_byte >= rep->type_table_len) {
        return false;
    }
    const persistOps *ops = rep->type_table[type_byte];
    if (ops == nullptr || ops->restore == nullptr) {
        return false;
    }
    /* Audit invariant: enforced at Create — every non-NULL entry has
     * a non-NULL free. Defensive assert here so a debug build trips
     * loudly if a caller later mutates the type table behind our back. */
    if (ops->free == nullptr) {
        return false;
    }

    /* The payload names its structure format: only one this build reads
     * (from a peer on the same or an older compatible release) is
     * restored; any other is counted and refused like an unrestorable
     * payload */
    const dkFormatSpec spec = persistOpsFormat(ops);
    dkFormatStamp stamp;
    size_t prefix = 0;
    if (dkFormatPrefixRead(&spec, frame->payload, frame->payload_len, &stamp,
                           &prefix) != DK_FORMAT_OK) {
        /* @sync: relaxed counter */
        atomic_fetch_add_explicit(&rep->format_refused_count, 1u,
                                  memory_order_relaxed);
        return false;
    }

    void *structure =
        ops->restore(frame->payload + prefix, frame->payload_len - prefix);
    if (structure == nullptr) {
        return false;
    }
    const bool accepted = rep->on_restored(frame->origin_id, frame->sequence,
                                           ops, structure, rep->apply_ctx);
    if (accepted) {
        /* This origin's frame went through: its count, if it holds one,
         * starts over.  An origin without one takes none: taking one
         * would wipe another origin's count once all are in use, and a
         * poison frame whose count keeps being wiped is never dropped */
        replicatorRejectState *own =
            replicatorRejectFind_(rep, frame->origin_id);
        if (own) {
            *own = (replicatorRejectState){};
        }
        return true;
    }
    replicatorRejectState *t = replicatorRejectTracker_(rep, frame->origin_id);

    /* Apply rejected. Always free the restored structure to avoid the
     * leak the original code only guarded with a NULL-check. */
    ops->free(structure);

    /* Count this origin's rejects of THIS sequence */
    if (t->origin_id == frame->origin_id && t->sequence == frame->sequence) {
        t->consecutive_rejects++;
    } else {
        *t = (replicatorRejectState){.origin_id = frame->origin_id,
                                     .sequence = frame->sequence,
                                     .consecutive_rejects = 1};
    }

    /* Drop policy: once the counter reaches the cap, unless the caller
     * chose to retry forever */
    if (rep->max_apply_retries != CLUSTER_REPLICATOR_RETRY_FOREVER &&
        t->consecutive_rejects >= rep->max_apply_retries) {
        /* @sync: relaxed counter */
        atomic_fetch_add_explicit(&rep->dropped_count, 1u,
                                  memory_order_relaxed);
        /* Reset so the next reject after the drop starts a fresh count
         * (covers the case where the producer's retry happens to land
         * a different sequence next, but the trampoline must not roll
         * the budget over). */
        *t = (replicatorRejectState){};
        /* Returning true makes the mailbox advance its watermark past
         * the poisoned sequence. The producer's future retransmits of
         * this exact (origin, sequence) will be rejected as REPLAY
         * (not APPLY_REJECTED) and the producer will eventually move
         * on to fresh sequences. */
        return true;
    }
    return false;
}

clusterReplicator *clusterReplicatorCreateEx(
    hmacKeyring *ring, flexEnvelopeAlgo envelope_algo,
    clusterMailboxFlags mailbox_flags, const persistOps *const *type_table,
    size_t type_table_len, clusterReplicatorApplyFn on_restored,
    void *apply_ctx, const clusterReplicatorConfig *cfg) {
    if (ring == nullptr || type_table == nullptr || type_table_len == 0 ||
        on_restored == nullptr) {
        return nullptr;
    }
    /* A NULL .free on any populated type-table entry would leak the
     * restored structure on every apply rejection. Refuse construction
     * outright so the bug surfaces at setup time (loud, deterministic)
     * rather than as a slow leak in production. */
    if (replicatorAuditTypeTable(type_table, type_table_len) !=
        type_table_len) {
        return nullptr;
    }
    cleanerName(zfree, Replicator) clusterReplicator *rep =
        zmalloc(sizeof(*rep));
    rep->type_table = type_table;
    rep->type_table_len = type_table_len;
    rep->on_restored = on_restored;
    rep->apply_ctx = apply_ctx;
    rep->max_apply_retries = (cfg != nullptr && cfg->max_apply_retries != 0)
                                 ? cfg->max_apply_retries
                                 : CLUSTER_REPLICATOR_DEFAULT_MAX_APPLY_RETRIES;
    memset(rep->rejects, 0, sizeof(rep->rejects));
    atomic_init(&rep->dropped_count, (uint64_t)0);
    atomic_init(&rep->format_refused_count, (uint64_t)0);
    /* The mailbox comes from the full-options constructor so the
     * caller's nonce-log knobs (disable / capacity) reach it */
    const bool disable_nonce_log =
        (cfg != nullptr) ? cfg->disable_nonce_log : false;
    const size_t nonce_log_capacity =
        (cfg != nullptr && cfg->nonce_log_capacity != 0u)
            ? cfg->nonce_log_capacity
            : CLUSTER_REPLICATOR_DEFAULT_NONCE_LOG_CAP;
    const clusterMailboxCreateOptions mb_opts = {
        .ring = ring,
        .envelope_algo = envelope_algo,
        .on_apply = replicatorMailboxApply,
        .apply_ctx = rep,
        .flags = mailbox_flags,
        .disable_nonce_log = disable_nonce_log,
        .nonce_log_capacity = disable_nonce_log ? 0u : nonce_log_capacity,
    };
    rep->mb = clusterMailboxCreateOpts(&mb_opts);
    if (rep->mb == nullptr) {
        return nullptr;
    }

    clusterReplicator *out = rep;
    rep = nullptr;
    return out;
}

clusterReplicator *clusterReplicatorCreate(hmacKeyring *ring,
                                           flexEnvelopeAlgo envelope_algo,
                                           clusterMailboxFlags mailbox_flags,
                                           const persistOps *const *type_table,
                                           size_t type_table_len,
                                           clusterReplicatorApplyFn on_restored,
                                           void *apply_ctx) {
    return clusterReplicatorCreateEx(ring, envelope_algo, mailbox_flags,
                                     type_table, type_table_len, on_restored,
                                     apply_ctx, nullptr);
}

uint64_t clusterReplicatorDroppedCount(const clusterReplicator *rep) {
    if (rep == nullptr) {
        return 0u;
    }
    /* Cast-away of const is local to this read of an _Atomic field;
     * the public contract is "thread-safe observer". */
    /* @sync: relaxed counter */
    return atomic_load_explicit(&((clusterReplicator *)rep)->dropped_count,
                                memory_order_relaxed);
}

uint64_t clusterReplicatorFormatRefusedCount(const clusterReplicator *rep) {
    if (rep == nullptr) {
        return 0u;
    }
    /* @sync: relaxed counter */
    return atomic_load_explicit(
        &((clusterReplicator *)rep)->format_refused_count,
        memory_order_relaxed);
}

void clusterReplicatorFree(clusterReplicator *rep) {
    if (rep == nullptr) {
        return;
    }
    clusterMailboxFree(rep->mb);
    zfree(rep);
}

clusterMailbox *clusterReplicatorMailbox(clusterReplicator *rep) {
    return (rep != nullptr) ? rep->mb : nullptr;
}

size_t clusterReplicatorSnapshotSize(clusterReplicator *rep,
                                     const persistOps *ops,
                                     const void *structure) {
    if (rep == nullptr || ops == nullptr || ops->snapshot == nullptr ||
        structure == nullptr) {
        return 0;
    }
    /* The snapshot's own length: ops->estimateSize is an approximation
     * for pre-allocation and falls short for many types, and Seal needs
     * a buffer the snapshot fits. */
    size_t snapshot_len = 0;
    cleanerName(zfree, Wire) uint8_t *snapshot =
        ops->snapshot(structure, &snapshot_len);
    if (snapshot == nullptr) {
        return 0;
    }
    const dkFormatSpec spec = persistOpsFormat(ops);
    const dkFormatStamp stamp = dkFormatStampFor(&spec, 0);
    const size_t inner_payload = dkFormatPrefixSize(&stamp) + snapshot_len;
    return clusterMailboxSealedSize(rep->mb, inner_payload);
}

bool clusterReplicatorSeal(clusterReplicator *rep, const persistOps *ops,
                           const void *structure, uint32_t origin_id,
                           uint64_t sequence, hmacKeyId key_id, uint8_t *out,
                           size_t out_cap, size_t *out_written) {
    if (rep == nullptr || ops == nullptr || ops->snapshot == nullptr ||
        ops->type > UINT8_MAX) {
        return false;
    }
    size_t snapshot_len = 0;
    cleanerName(zfree, Wire) uint8_t *snapshot =
        ops->snapshot(structure, &snapshot_len);
    if (snapshot == nullptr) {
        return false;
    }

    /* The frame payload: the structure format's prefix, then the
     * snapshot, so the receiver knows which encoding it holds */
    const dkFormatSpec spec = persistOpsFormat(ops);
    const dkFormatStamp stamp = dkFormatStampFor(&spec, 0);
    const size_t prefix = dkFormatPrefixSize(&stamp);
    cleanerName(zfree, Wire) uint8_t *payload = zmalloc(prefix + snapshot_len);
    dkFormatPrefixWrite(&stamp, payload, prefix);
    memcpy(payload + prefix, snapshot, snapshot_len);
    return clusterMailboxSeal(rep->mb, origin_id, sequence, (uint8_t)ops->type,
                              payload, prefix + snapshot_len, key_id, out,
                              out_cap, out_written);
}

clusterMailboxIngestResult clusterReplicatorIngest(clusterReplicator *rep,
                                                   const uint8_t *bytes,
                                                   size_t len,
                                                   hmacKeyId key_id) {
    if (rep == nullptr) {
        return CLUSTER_MAILBOX_ERR_PARAM;
    }
    return clusterMailboxIngest(rep->mb, bytes, len, key_id);
}

/* ============================================================================
 * Test driver — end-to-end demo: real flex ships through the security
 * stack and arrives intact on the consumer side.
 * ============================================================================
 */

#ifdef DATAKIT_TEST

#include "ctest.h"
#include "flex.h"
#include "multimap.h"

#include <stdio.h>
#include <stdlib.h>

defcleaner(flex *, flexFree);
defcleaner(multimap *, multimapFree);

typedef struct {
    flex *restored[4];
    uint32_t origins[4];
    uint64_t seqs[4];
    size_t count;
    bool reject_next;
} replicatorTestCtx;

/* Accepts (and frees) every frame but those of two poison origins */
static bool testPoisonApply(uint32_t origin_id, uint64_t sequence,
                            const persistOps *ops, void *structure, void *ctx) {
    (void)sequence;
    (void)ctx;
    if (origin_id == 11u || origin_id == 12u) {
        return false;
    }
    ops->free(structure);
    return true;
}

static bool testApply(uint32_t origin_id, uint64_t sequence,
                      const persistOps *ops, void *structure, void *ctx) {
    replicatorTestCtx *t = (replicatorTestCtx *)ctx;
    if (t->reject_next) {
        t->reject_next = false;
        return false;
    }
    if (t->count >= 4 || ops->type != PERSIST_TYPE_FLEX) {
        return false;
    }
    t->restored[t->count] = (flex *)structure;
    t->origins[t->count] = origin_id;
    t->seqs[t->count] = sequence;
    t->count++;
    return true;
}

/* A restore that refuses every payload */
static void *replicatorRefuseRestore(const uint8_t *, size_t) {
    return nullptr;
}

/* A snapshot that fails */
static uint8_t *replicatorNoSnapshot(const void *, size_t *len) {
    *len = 0;
    return nullptr;
}

/* ====================================================================
 * The model test: a producer ships intsets for many origins, the
 * consumer restores and offers each to the application, which accepts
 * or refuses it, against a model of the watermarks and of the per-
 * origin retry trackers: a frame refused max_apply_retries times in a
 * row is dropped, frames from other origins between its retries (more
 * origins than trackers among them) leave its count alone
 * ==================================================================== */
#include "dkRandom.h"
#include "intset.h"

static constexpr uint32_t RP_ORIGINS = 24; /* past the 16 trackers */

typedef struct rpTracker_ {
    uint32_t origin;
    uint64_t seq;
    uint32_t rejects;
} rpTracker_;

typedef struct rpModel_ {
    hmacKeyring *ring;
    clusterReplicator *producer;
    clusterReplicator *consumer;
    uint32_t maxRetries;
    uint64_t last[RP_ORIGINS + 1];
    uint64_t pending[RP_ORIGINS + 1]; /* a refused sequence to resend */
    bool poison[RP_ORIGINS + 1];      /* refuses every frame */
    rpTracker_ t[16];
    uint64_t dropped;
    uint64_t formatRefused;
    bool accept; /* the application's answer to a frame not poisoned */
    bool called; /* the application saw the frame */
    bool contentOk;
    const char *why;
} rpModel_;

/* @model: src/clusterReplicator.h */
static void rpModelFree_(rpModel_ *M) {
    if (M) {
        clusterReplicatorFree(M->consumer);
        clusterReplicatorFree(M->producer);
        hmacKeyringFree(M->ring);
        zfree(M);
    }
}

defcleaner(rpModel_ *, rpModelFree_);

/* The intset a frame carries: members drawn from its origin and sequence */
static intset *rpContent_(uint32_t origin, uint64_t seq) {
    /* @own: caller; free=intsetFree */
    intset *is = intsetNew();
    uint64_t s = origin * UINT64_C(1000003) + seq;
    const uint32_t n = 1 + (uint32_t)(seq % 9);
    for (uint32_t i = 0; i < n; i++) {
        bool added = false;
        intsetAdd(&is, (int64_t)(dkSplitmix64Next(&s) % 5000) - 2500, &added);
    }
    return is;
}

static bool rpApply_(uint32_t origin, uint64_t seq, const persistOps *ops,
                     void *structure, void *ctx) {
    rpModel_ *M = ctx;
    M->called = true;
    intset *want = rpContent_(origin, seq);
    const intset *got = structure;
    bool same =
        ops == &persistOpsIntset && intsetCount(got) == intsetCount(want);
    for (uint32_t i = 0; same && i < intsetCount(want); i++) {
        int64_t a = 0;
        int64_t b = 0;
        same = intsetGet(got, i, &a) && intsetGet(want, i, &b) && a == b;
    }
    intsetFree(want);
    M->contentOk = same;
    const bool yes = M->accept && !M->poison[origin];
    if (yes) {
        ops->free(structure); /* accepted: the application's to free */
    }
    return yes;
}

/* The model's trackers, as the replicator keeps them: an origin's own,
 * else the first free one, else the first with the fewest rejects */
static rpTracker_ *rpTrackerFor_(rpModel_ *M, uint32_t origin, bool take) {
    rpTracker_ *free = nullptr;
    rpTracker_ *least = &M->t[0];
    for (uint32_t i = 0; i < 16; i++) {
        if (M->t[i].origin == origin) {
            return &M->t[i];
        }
        if (!M->t[i].origin && !free) {
            free = &M->t[i];
        }
        if (M->t[i].rejects < least->rejects) {
            least = &M->t[i];
        }
    }
    if (!take) {
        return nullptr;
    }
    rpTracker_ *taken = free ? free : least;
    *taken = (rpTracker_){};
    return taken;
}

/* @model: src/clusterReplicator.h */
/* One frame shipped and taken in, as the model says it goes */
static bool rpFrame_(rpModel_ *M, uint64_t r) {
    const uint32_t origin = 1 + (uint32_t)(r % RP_ORIGINS);
    /* The refused frame again, a new one, or an old one (a replay) */
    const uint64_t pick = (r >> 8) % 6;
    const uint64_t seq = pick < 3 && M->pending[origin] ? M->pending[origin]
                         : pick == 5 && M->last[origin] ? M->last[origin]
                                                        : M->last[origin] + 1;
    intset *is = rpContent_(origin, seq);
    const size_t cap =
        clusterReplicatorSnapshotSize(M->producer, &persistOpsIntset, is);
    cleanerName(zfree, Wire) uint8_t *buf = zmalloc(cap);
    size_t wrote = 0;
    const bool sealed = clusterReplicatorSeal(
        M->producer, &persistOpsIntset, is, origin, seq, 1, buf, cap, &wrote);
    intsetFree(is);
    if (!sealed) {
        M->why = "a seal";
        return false;
    }
    M->accept = (r >> 16) % 3 != 0;
    M->called = false;
    M->contentOk = false;
    const clusterMailboxIngestResult got =
        clusterReplicatorIngest(M->consumer, buf, wrote, 1);
    /* The model's verdict */
    clusterMailboxIngestResult want;
    if (seq <= M->last[origin]) {
        want = CLUSTER_MAILBOX_ERR_REPLAY;
    } else if (M->accept && !M->poison[origin]) {
        want = CLUSTER_MAILBOX_OK;
        rpTracker_ *t = rpTrackerFor_(M, origin, false);
        if (t) {
            *t = (rpTracker_){}; /* its count starts over */
        }
        M->last[origin] = seq;
        M->pending[origin] = 0;
    } else {
        rpTracker_ *t = rpTrackerFor_(M, origin, true);
        if (t->origin == origin && t->seq == seq) {
            t->rejects++;
        } else {
            *t = (rpTracker_){.origin = origin, .seq = seq, .rejects = 1};
        }
        if (M->maxRetries != CLUSTER_REPLICATOR_RETRY_FOREVER &&
            t->rejects >= M->maxRetries) {
            *t = (rpTracker_){};
            M->dropped++;
            want = CLUSTER_MAILBOX_OK; /* dropped: the watermark moves */
            M->last[origin] = seq;
            M->pending[origin] = 0;
        } else {
            want = CLUSTER_MAILBOX_ERR_APPLY_REJECTED;
            M->pending[origin] = seq;
        }
    }
    const bool reached = want != CLUSTER_MAILBOX_ERR_REPLAY;
    if (got != want || M->called != reached || (reached && !M->contentOk)) {
        static char why[128];
        snprintf(why, sizeof(why),
                 "origin %" PRIu32 " seq %" PRIu64 " (last %" PRIu64
                 "): %d, want %d; called %d, content %d",
                 origin, seq, M->last[origin], (int)got, (int)want,
                 (int)M->called, (int)M->contentOk);
        M->why = why;
        return false;
    }
    return true;
}

/* @model: src/clusterReplicator.h */
/* A frame that is not a restorable intset: no structure format, or a
 * type the consumer has no descriptor for; refused, never tracked */
static bool rpForeign_(rpModel_ *M, uint64_t r) {
    const uint32_t origin = 1 + (uint32_t)(r % RP_ORIGINS);
    const uint64_t seq = M->last[origin] + 1;
    const bool noFormat = (r >> 8) % 2;
    const uint8_t type = noFormat ? PERSIST_TYPE_INTSET : PERSIST_TYPE_MULTIMAP;
    clusterMailbox *mb = clusterReplicatorMailbox(M->producer);
    uint8_t out[256];
    size_t wrote = 0;
    if (!clusterMailboxSeal(mb, origin, seq, type, (const uint8_t *)"raw", 3, 1,
                            out, sizeof(out), &wrote)) {
        M->why = "a raw seal";
        return false;
    }
    M->called = false;
    const clusterMailboxIngestResult got =
        clusterReplicatorIngest(M->consumer, out, wrote, 1);
    M->formatRefused += noFormat;
    if (got != CLUSTER_MAILBOX_ERR_APPLY_REJECTED || M->called) {
        M->why = "a foreign frame";
        return false;
    }
    return true;
}

/* @model: src/clusterReplicator.h */
static int clusterReplicatorTestModel_(uint64_t seed) {
    int err = 0;
    cleaner(rpModelFree_) rpModel_ *M = zcalloc(1, sizeof(*M));
    /* @own: container; free=hmacKeyringFree */
    M->ring = hmacKeyringCreate();
    uint8_t key[32];
    for (uint32_t i = 0; i < sizeof(key); i++) {
        key[i] = (uint8_t)(i * 7 + 1);
    }
    (void)hmacKeyringInstallUnsalted(M->ring, 1, &hmacAlgoSHA256, key,
                                     sizeof(key));
    static const persistOps *table[PERSIST_TYPE_MAX + 1];
    table[PERSIST_TYPE_INTSET] = &persistOpsIntset;
    M->maxRetries = seed % 3 == 0 ? CLUSTER_REPLICATOR_DEFAULT_MAX_APPLY_RETRIES
                    : seed % 3 == 1 ? 3
                                    : CLUSTER_REPLICATOR_RETRY_FOREVER;
    const clusterReplicatorConfig cfg = {.max_apply_retries =
                                             seed % 3 == 0 ? 0 : M->maxRetries,
                                         .disable_nonce_log = seed % 2};
    /* @own: container; free=clusterReplicatorFree */
    M->producer = clusterReplicatorCreate(M->ring, FLEX_ENV_ALGO_HMAC_BLAKE3, 0,
                                          table, countof(table), rpApply_, M);
    /* @own: container; free=clusterReplicatorFree */
    M->consumer =
        clusterReplicatorCreateEx(M->ring, FLEX_ENV_ALGO_HMAC_BLAKE3, 0, table,
                                  countof(table), rpApply_, M, &cfg);
    for (uint32_t o = 1; o <= RP_ORIGINS; o++) {
        M->poison[o] = o % 7 == 0;
    }
    uint64_t rng = seed * 13 + 5;
    for (uint32_t step = 0; step < 3000 && !err; step++) {
        const uint64_t r = dkSplitmix64Next(&rng);
        const bool ok =
            (r % 20 == 0) ? rpForeign_(M, r >> 5) : rpFrame_(M, r >> 5);
        const bool counts =
            clusterReplicatorDroppedCount(M->consumer) == M->dropped &&
            clusterReplicatorFormatRefusedCount(M->consumer) ==
                M->formatRefused &&
            clusterMailboxLastApplied(clusterReplicatorMailbox(M->consumer),
                                      1) == M->last[1];
        if (!ok || !counts) {
            ERR("model step %" PRIu32 ": %s (dropped %" PRIu64 "/%" PRIu64
                ", refused %" PRIu64 "/%" PRIu64 ", retries %" PRIu32
                ", seed %" PRIu64 ")",
                step, ok ? "the counts" : M->why,
                clusterReplicatorDroppedCount(M->consumer), M->dropped,
                clusterReplicatorFormatRefusedCount(M->consumer),
                M->formatRefused, M->maxRetries, seed);
        }
    }
    /* A poisoned origin's frames were dropped unless retries are forever */
    if ((M->maxRetries == CLUSTER_REPLICATOR_RETRY_FOREVER) !=
        (M->dropped == 0)) {
        ERR("dropped %" PRIu64 " with retries %" PRIu32, M->dropped,
            M->maxRetries);
    }
    return err;
}

int clusterReplicatorTest(int, char *[]) {
    int err = 0;

    static const persistOps *type_table[256];
    memset((void *)type_table, 0, sizeof(type_table));
    type_table[PERSIST_TYPE_FLEX] = &persistOpsFlex;

    TEST("model: frames for many origins, refused, retried and dropped") {
        for (uint64_t seed = 0; seed < 6; seed++) {
            err += clusterReplicatorTestModel_(seed);
        }
    }

    TEST("end-to-end: flex ships through envelope + mailbox + restore") {
        const uint8_t key[32] = {0xAB};
        hmacKeyring *ring = hmacKeyringCreate();
        const hmacKeyId KID = 1u;
        hmacKeyringInstallUnsalted(ring, KID, &hmacAlgoSHA256, key,
                                   sizeof(key));

        replicatorTestCtx app_ctx = {};
        clusterReplicator *rep = clusterReplicatorCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, CLUSTER_MAILBOX_FLAG_NONE,
            type_table, 256u, testApply, &app_ctx);
        if (rep == nullptr) {
            ERR("create");
        }

        /* Build a real flex with multiple typed entries. */
        cleaner(flexFree) flex *src = flexNew();
        flexPushUnsigned(&src, 100u, FLEX_ENDPOINT_TAIL);
        flexPushBytes(&src, "hello", 5, FLEX_ENDPOINT_TAIL);
        flexPushUnsigned(&src, 200u, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&src, -42, FLEX_ENDPOINT_TAIL);

        const size_t cap =
            clusterReplicatorSnapshotSize(rep, &persistOpsFlex, src);
        cleanerName(zfree, Wire) uint8_t *wire = zmalloc(cap);
        size_t written = 0;
        if (!clusterReplicatorSeal(rep, &persistOpsFlex, src, 7u, 1u, KID, wire,
                                   cap, &written)) {
            ERR("seal");
        }
        if (clusterReplicatorIngest(rep, wire, written, KID) !=
            CLUSTER_MAILBOX_OK) {
            ERR("ingest");
        }
        if (app_ctx.count != 1u) {
            ERR("expected 1 restored, got %zu", app_ctx.count);
        }
        if (app_ctx.origins[0] != 7u || app_ctx.seqs[0] != 1u) {
            ERR("origin/seq mismatch");
        }
        /* Byte-exact integrity: a flex is uniquely defined by its
         * bytes, so memcmp of the source and restored is the
         * canonical equivalence test. */
        if (flexCount(app_ctx.restored[0]) != flexCount(src) ||
            flexBytes(app_ctx.restored[0]) != flexBytes(src) ||
            memcmp(app_ctx.restored[0], src, flexBytes(src)) != 0) {
            ERR("restored flex differs from source");
        }
        flexFree(app_ctx.restored[0]);

        /* Tamper test. */
        app_ctx.count = 0;
        wire[written / 2] ^= 0x80;
        if (clusterReplicatorIngest(rep, wire, written, KID) !=
            CLUSTER_MAILBOX_ERR_VERIFY_FAILED) {
            ERR("tampered frame must fail verify");
        }
        if (app_ctx.count != 0u) {
            ERR("apply must not fire on verify failure");
        }
        wire[written / 2] ^= 0x80;

        /* Replay rejection. */
        if (clusterReplicatorIngest(rep, wire, written, KID) !=
            CLUSTER_MAILBOX_ERR_REPLAY) {
            ERR("replay must be rejected");
        }

        /* Apply rejection + retry. */
        app_ctx.reject_next = true;
        size_t w2 = 0;
        clusterReplicatorSeal(rep, &persistOpsFlex, src, 7u, 2u, KID, wire, cap,
                              &w2);
        if (clusterReplicatorIngest(rep, wire, w2, KID) !=
            CLUSTER_MAILBOX_ERR_APPLY_REJECTED) {
            ERR("apply-rejected expected");
        }
        if (clusterMailboxLastApplied(clusterReplicatorMailbox(rep), 7u) !=
            1u) {
            ERR("watermark must stay at 1 after apply-rejected");
        }
        app_ctx.reject_next = false;
        if (clusterReplicatorIngest(rep, wire, w2, KID) != CLUSTER_MAILBOX_OK) {
            ERR("retry must succeed");
        }
        if (clusterMailboxLastApplied(clusterReplicatorMailbox(rep), 7u) !=
            2u) {
            ERR("watermark must advance to 2 after successful retry");
        }
        flexFree(app_ctx.restored[0]);

        clusterReplicatorFree(rep);
        hmacKeyringFree(ring);
    }

    TEST("each origin's poison frame is dropped after its retries, whatever "
         "arrives between") {
        /* Two poison frames (origins 11 and 12) retransmitted with good
         * frames from origin 13 between them: each still runs out its
         * own three retries and is dropped */
        const uint8_t key[32] = {0x5a};
        hmacKeyring *ring = hmacKeyringCreate();
        const hmacKeyId KID = 1u;
        hmacKeyringInstallUnsalted(ring, KID, &hmacAlgoSHA256, key,
                                   sizeof(key));
        const clusterReplicatorConfig cfg = {.max_apply_retries = 3};
        clusterReplicator *rep = clusterReplicatorCreateEx(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, CLUSTER_MAILBOX_FLAG_NONE,
            type_table, 256u, testPoisonApply, nullptr, &cfg);
        cleaner(flexFree) flex *src = flexNew();
        flexPushUnsigned(&src, 1u, FLEX_ENDPOINT_TAIL);
        const size_t cap =
            clusterReplicatorSnapshotSize(rep, &persistOpsFlex, src);
        cleanerName(zfree, Wire) uint8_t *p11 = zmalloc(cap);
        cleanerName(zfree, Wire) uint8_t *p12 = zmalloc(cap);
        cleanerName(zfree, Wire) uint8_t *good = zmalloc(cap);
        size_t w11 = 0;
        size_t w12 = 0;
        (void)clusterReplicatorSeal(rep, &persistOpsFlex, src, 11u, 1u, KID,
                                    p11, cap, &w11);
        (void)clusterReplicatorSeal(rep, &persistOpsFlex, src, 12u, 1u, KID,
                                    p12, cap, &w12);
        uint64_t goodSeq = 1;
        for (int round = 0; round < 6; round++) {
            (void)clusterReplicatorIngest(rep, p11, w11, KID);
            size_t wg = 0;
            (void)clusterReplicatorSeal(rep, &persistOpsFlex, src, 13u,
                                        goodSeq++, KID, good, cap, &wg);
            (void)clusterReplicatorIngest(rep, good, wg, KID);
            (void)clusterReplicatorIngest(rep, p12, w12, KID);
        }
        clusterMailbox *mb = clusterReplicatorMailbox(rep);
        if (clusterReplicatorDroppedCount(rep) != 2 ||
            clusterMailboxLastApplied(mb, 11u) != 1u ||
            clusterMailboxLastApplied(mb, 12u) != 1u) {
            ERR("%" PRIu64 " poison frames dropped, watermarks %" PRIu64
                " and %" PRIu64,
                clusterReplicatorDroppedCount(rep),
                clusterMailboxLastApplied(mb, 11u),
                clusterMailboxLastApplied(mb, 12u));
        }
        clusterReplicatorFree(rep);
        hmacKeyringFree(ring);
    }

    TEST("snapshot size fits the snapshot, not the type's estimate") {
        /* A multimap's estimateSize (its in-memory bytes) is below its
         * snapshot (2 vs 6 bytes empty), so a size computed from it left
         * Seal a buffer too small. */
        const uint8_t key[32] = {0xCD};
        hmacKeyring *ring = hmacKeyringCreate();
        const hmacKeyId KID = 1u;
        hmacKeyringInstallUnsalted(ring, KID, &hmacAlgoSHA256, key,
                                   sizeof(key));
        replicatorTestCtx app_ctx = {};
        clusterReplicator *rep = clusterReplicatorCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, CLUSTER_MAILBOX_FLAG_NONE,
            type_table, 256u, testApply, &app_ctx);
        cleaner(multimapFree) multimap *m = multimapNew(2);
        for (int n = 0; n < 3; n++) {
            size_t snapLen = 0;
            cleanerName(zfree, Wire) uint8_t *snap =
                persistOpsMultimap.snapshot(m, &snapLen);
            const size_t cap =
                clusterReplicatorSnapshotSize(rep, &persistOpsMultimap, m);
            cleanerName(zfree, Wire) uint8_t *wire = zmalloc(cap);
            size_t written = 0;
            if (!clusterReplicatorSeal(rep, &persistOpsMultimap, m, 3u,
                                       (uint64_t)n + 1, KID, wire, cap,
                                       &written) ||
                written > cap) {
                ERR("round %d: seal into %zu B failed (snapshot %zu B, "
                    "estimate %zu B)",
                    n, cap, snapLen, persistOpsMultimap.estimateSize(m));
            }
            const databox k = DATABOX_SIGNED(n);
            const databox v = DATABOX_SIGNED(n * 10);
            const databox *row[2] = {&k, &v};
            multimapInsert(&m, row);
        }
        clusterReplicatorFree(rep);
        hmacKeyringFree(ring);
    }

    /* A peer on a newer release ships a structure format this build does
     * not read: the frame is refused and counted, never restored; an
     * unknown ignorable feature or the current stamp is restored */
    TEST("structure format stamp gates every shipped payload") {
        const uint8_t key[32] = {0xEF};
        hmacKeyring *ring = hmacKeyringCreate();
        const hmacKeyId KID = 1u;
        hmacKeyringInstallUnsalted(ring, KID, &hmacAlgoSHA256, key,
                                   sizeof(key));
        replicatorTestCtx app_ctx = {};
        clusterReplicator *rep = clusterReplicatorCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, CLUSTER_MAILBOX_FLAG_NONE,
            type_table, 256u, testApply, &app_ctx);
        clusterMailbox *mb = clusterReplicatorMailbox(rep);

        cleaner(flexFree) flex *body = flexNew();
        flexPushUnsigned(&body, 5u, FLEX_ENDPOINT_TAIL);
        const dkFormatSpec spec = persistOpsFormat(&persistOpsFlex);
        const struct {
            dkFormatStamp stamp;
            bool restored;
        } cases[] = {
            {{spec.id, spec.version + 1, 0}, false},
            {{spec.id, spec.version, 1}, false},
            {{PERSIST_TYPE_INTSET, spec.version, 0}, false},
            {{spec.id, spec.version, UINT64_C(1) << 33}, true},
        };
        uint64_t refused = 0;
        for (size_t c = 0; c < countof(cases); c++) {
            uint8_t payload[64];
            const size_t prefix =
                dkFormatPrefixWrite(&cases[c].stamp, payload, sizeof(payload));
            memcpy(payload + prefix, body, flexBytes(body));
            const size_t len = prefix + flexBytes(body);

            uint8_t wire[256];
            size_t written = 0;
            clusterMailboxSeal(mb, 9u, 1u + c, (uint8_t)PERSIST_TYPE_FLEX,
                               payload, len, KID, wire, sizeof(wire), &written);
            const size_t before = app_ctx.count;
            const clusterMailboxIngestResult rc =
                clusterReplicatorIngest(rep, wire, written, KID);
            refused += !cases[c].restored;
            if (rc != (cases[c].restored
                           ? CLUSTER_MAILBOX_OK
                           : CLUSTER_MAILBOX_ERR_APPLY_REJECTED) ||
                (app_ctx.count > before) != cases[c].restored ||
                clusterReplicatorFormatRefusedCount(rep) != refused) {
                ERR("case %zu: restored %d, refused %" PRIu64, c,
                    (int)(app_ctx.count > before),
                    clusterReplicatorFormatRefusedCount(rep));
            }
        }
        for (size_t i = 0; i < app_ctx.count; i++) {
            flexFree(app_ctx.restored[i]);
        }
        clusterReplicatorFree(rep);
        hmacKeyringFree(ring);
    }

    TEST("rejects frame with unknown persistType in msg_type") {
        const uint8_t key[32] = {0xCD};
        hmacKeyring *ring = hmacKeyringCreate();
        const hmacKeyId KID = 1u;
        hmacKeyringInstallUnsalted(ring, KID, &hmacAlgoSHA256, key,
                                   sizeof(key));

        replicatorTestCtx app_ctx = {};
        clusterReplicator *rep = clusterReplicatorCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, CLUSTER_MAILBOX_FLAG_NONE,
            type_table, 256u, testApply, &app_ctx);

        /* Forge via the underlying mailbox: an unknown persistType
         * byte must surface as apply-rejected. */
        const uint8_t payload[4] = {};
        clusterMailbox *mb = clusterReplicatorMailbox(rep);
        const size_t cap = clusterMailboxSealedSize(mb, sizeof(payload));
        cleanerName(zfree, Wire) uint8_t *wire = zmalloc(cap);
        size_t written = 0;
        clusterMailboxSeal(mb, 7u, 1u, (uint8_t)PERSIST_TYPE_MAX, payload,
                           sizeof(payload), KID, wire, cap, &written);
        if (clusterMailboxIngest(mb, wire, written, KID) !=
            CLUSTER_MAILBOX_ERR_APPLY_REJECTED) {
            ERR("unknown type must surface as apply-rejected");
        }
        if (app_ctx.count != 0) {
            ERR("no apply must fire on unknown type");
        }
        clusterReplicatorFree(rep);
        hmacKeyringFree(ring);
    }

    TEST("missing arguments, failing snapshots and refused payloads") {
        const uint8_t key[32] = {0xEF};
        hmacKeyring *ring = hmacKeyringCreate();
        const hmacKeyId KID = 1u;
        hmacKeyringInstallUnsalted(ring, KID, &hmacAlgoSHA256, key,
                                   sizeof(key));
        replicatorTestCtx app_ctx = {};
        cleaner(flexFree) flex *src = flexNew();
        flexPushUnsigned(&src, 5u, FLEX_ENDPOINT_TAIL);

        /* Construction refuses each missing argument, and a table entry
         * with no free (it would leak every refused structure) */
        static persistOps noFree;
        noFree = persistOpsFlex;
        noFree.free = nullptr;
        static const persistOps *freeless[1] = {&noFree};
        const flexEnvelopeAlgo algo = FLEX_ENV_ALGO_HMAC_SHA256;
        const uint32_t none = CLUSTER_MAILBOX_FLAG_NONE;
        if (clusterReplicatorCreate(nullptr, algo, none, type_table, 256u,
                                    testApply, &app_ctx) ||
            clusterReplicatorCreate(ring, algo, none, nullptr, 256u, testApply,
                                    &app_ctx) ||
            clusterReplicatorCreate(ring, algo, none, type_table, 0u, testApply,
                                    &app_ctx) ||
            clusterReplicatorCreate(ring, algo, none, type_table, 256u, nullptr,
                                    &app_ctx) ||
            clusterReplicatorCreate(ring, algo, none, freeless, 1u, testApply,
                                    &app_ctx)) {
            ERR("a construction with a missing argument succeeded");
        }

        /* No replicator: every reader is empty, every writer refuses */
        uint8_t byte = 0;
        size_t written = 0;
        clusterReplicatorFree(nullptr);
        if (clusterReplicatorDroppedCount(nullptr) ||
            clusterReplicatorFormatRefusedCount(nullptr) ||
            clusterReplicatorMailbox(nullptr) ||
            clusterReplicatorSnapshotSize(nullptr, &persistOpsFlex, src) ||
            clusterReplicatorSeal(nullptr, &persistOpsFlex, src, 1u, 1u, KID,
                                  &byte, 1, &written) ||
            clusterReplicatorIngest(nullptr, &byte, 1, KID) !=
                CLUSTER_MAILBOX_ERR_PARAM) {
            ERR("a NULL replicator did not refuse");
        }

        /* Ops with no snapshot, one that fails, a NULL structure, and a
         * type that does not fit the frame's type byte */
        clusterReplicator *rep = clusterReplicatorCreate(
            ring, algo, none, type_table, 256u, testApply, &app_ctx);
        persistOps noSnapshot = persistOpsFlex;
        noSnapshot.snapshot = nullptr;
        persistOps failing = persistOpsFlex;
        failing.snapshot = replicatorNoSnapshot;
        persistOps wide = persistOpsFlex;
        wide.type = (persistType)(UINT8_MAX + 1);
        if (clusterReplicatorSnapshotSize(rep, nullptr, src) ||
            clusterReplicatorSnapshotSize(rep, &noSnapshot, src) ||
            clusterReplicatorSnapshotSize(rep, &persistOpsFlex, nullptr) ||
            clusterReplicatorSnapshotSize(rep, &failing, src) ||
            clusterReplicatorSeal(rep, nullptr, src, 1u, 1u, KID, &byte, 1,
                                  &written) ||
            clusterReplicatorSeal(rep, &failing, src, 1u, 1u, KID, &byte, 1,
                                  &written) ||
            clusterReplicatorSeal(rep, &wide, src, 1u, 1u, KID, &byte, 1,
                                  &written)) {
            ERR("a snapshot that cannot be taken was sized or sealed");
        }

        /* A sealed flex: a receiver whose table ends before the flex
         * type, and one whose restore refuses the payload, apply
         * nothing */
        const size_t cap =
            clusterReplicatorSnapshotSize(rep, &persistOpsFlex, src);
        cleanerName(zfree, Wire) uint8_t *wire = zmalloc(cap);
        if (!clusterReplicatorSeal(rep, &persistOpsFlex, src, 7u, 1u, KID, wire,
                                   cap, &written)) {
            ERR("seal");
        }

        static persistOps refusing;
        refusing = persistOpsFlex;
        refusing.restore = replicatorRefuseRestore;
        static const persistOps *refusingTable[256];
        refusingTable[PERSIST_TYPE_FLEX] = &refusing;
        clusterReplicator *shortTable =
            clusterReplicatorCreate(ring, algo, none, type_table,
                                    PERSIST_TYPE_FLEX, testApply, &app_ctx);
        clusterReplicator *refuser = clusterReplicatorCreate(
            ring, algo, none, refusingTable, 256u, testApply, &app_ctx);
        if (clusterReplicatorIngest(shortTable, wire, written, KID) ==
                CLUSTER_MAILBOX_OK ||
            clusterReplicatorIngest(refuser, wire, written, KID) ==
                CLUSTER_MAILBOX_OK ||
            app_ctx.count != 0) {
            ERR("a payload no table entry restores was applied");
        }

        clusterReplicatorFree(shortTable);
        clusterReplicatorFree(refuser);
        clusterReplicatorFree(rep);
        hmacKeyringFree(ring);
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
