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
    uint32_t origin_id;
    uint64_t sequence;
    uint32_t consecutive_rejects;
} replicatorRejectState;

struct clusterReplicator {
    clusterMailbox *mb;                  /* OWNED — freed in Free */
    const persistOps *const *type_table; /* borrowed */
    size_t type_table_len;
    clusterReplicatorApplyFn on_restored;
    void *apply_ctx;
    uint32_t max_apply_retries;        /* 0 = infinite (legacy) */
    replicatorRejectState last_reject; /* trampoline-local, sole writer */
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
        if (ops == NULL) {
            continue;
        }
        if (ops->free == NULL) {
            return i;
        }
    }
    return len;
}

/* Mailbox apply trampoline. The ctx is the replicator itself. */
static bool replicatorMailboxApply(const clusterFrame *frame, void *ctx) {
    clusterReplicator *rep = (clusterReplicator *)ctx;
    const uint8_t type_byte = frame->msg_type;
    if ((size_t)type_byte >= rep->type_table_len) {
        return false;
    }
    const persistOps *ops = rep->type_table[type_byte];
    if (ops == NULL || ops->restore == NULL) {
        return false;
    }
    /* Audit invariant: enforced at Create — every non-NULL entry has
     * a non-NULL free. Defensive assert here so a debug build trips
     * loudly if a caller later mutates the type table behind our back. */
    if (ops->free == NULL) {
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
    if (structure == NULL) {
        return false;
    }
    const bool accepted = rep->on_restored(frame->origin_id, frame->sequence,
                                           ops, structure, rep->apply_ctx);
    if (accepted) {
        /* Successful apply resets the consecutive-reject tracker. We
         * cannot key only on (origin, sequence) because the next
         * accepted frame typically has a different sequence anyway. */
        rep->last_reject.consecutive_rejects = 0;
        rep->last_reject.origin_id = 0;
        rep->last_reject.sequence = 0;
        return true;
    }

    /* Apply rejected. Always free the restored structure to avoid the
     * leak the original code only guarded with a NULL-check. */
    ops->free(structure);

    /* Increment consecutive-reject counter for THIS (origin, seq). */
    if (rep->last_reject.origin_id == frame->origin_id &&
        rep->last_reject.sequence == frame->sequence) {
        rep->last_reject.consecutive_rejects++;
    } else {
        rep->last_reject.origin_id = frame->origin_id;
        rep->last_reject.sequence = frame->sequence;
        rep->last_reject.consecutive_rejects = 1;
    }

    /* Drop policy: only when a finite cap is configured and the
     * counter has reached it. `0` means "infinite retries", matching
     * the legacy prior behaviour. */
    if (rep->max_apply_retries != 0 &&
        rep->last_reject.consecutive_rejects >= rep->max_apply_retries) {
        /* @sync: relaxed counter */
        atomic_fetch_add_explicit(&rep->dropped_count, 1u,
                                  memory_order_relaxed);
        /* Reset so the next reject after the drop starts a fresh count
         * (covers the case where the producer's retry happens to land
         * a different sequence next, but the trampoline must not roll
         * the budget over). */
        rep->last_reject.consecutive_rejects = 0;
        rep->last_reject.origin_id = 0;
        rep->last_reject.sequence = 0;
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
    hmacKeyring *ring, flexEnvelopeAlgo envelope_algo, uint32_t mailbox_flags,
    const persistOps *const *type_table, size_t type_table_len,
    clusterReplicatorApplyFn on_restored, void *apply_ctx,
    const clusterReplicatorConfig *cfg) {
    if (ring == NULL || type_table == NULL || type_table_len == 0 ||
        on_restored == NULL) {
        return NULL;
    }
    /* A NULL .free on any populated type-table entry would leak the
     * restored structure on every apply rejection. Refuse construction
     * outright so the bug surfaces at setup time (loud, deterministic)
     * rather than as a slow leak in production. */
    if (replicatorAuditTypeTable(type_table, type_table_len) !=
        type_table_len) {
        return NULL;
    }
    cleanerName(zfree, Replicator) clusterReplicator *rep =
        zmalloc(sizeof(*rep));
    rep->type_table = type_table;
    rep->type_table_len = type_table_len;
    rep->on_restored = on_restored;
    rep->apply_ctx = apply_ctx;
    rep->max_apply_retries = (cfg != NULL)
                                 ? cfg->max_apply_retries
                                 : CLUSTER_REPLICATOR_DEFAULT_MAX_APPLY_RETRIES;
    rep->last_reject.origin_id = 0;
    rep->last_reject.sequence = 0;
    rep->last_reject.consecutive_rejects = 0;
    atomic_init(&rep->dropped_count, (uint64_t)0);
    atomic_init(&rep->format_refused_count, (uint64_t)0);
    /* build the mailbox via the full-options constructor so the
     * caller's nonce-log knobs (disable / capacity) actually plumb
     * through to the underlying mailbox. Defaults are equivalent to
     * the legacy clusterMailboxCreateEx path. */
    const bool disable_nonce_log =
        (cfg != NULL) ? cfg->disable_nonce_log : false;
    const size_t nonce_log_capacity =
        (cfg != NULL && cfg->nonce_log_capacity != 0u)
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
    if (rep->mb == NULL) {
        return NULL;
    }

    clusterReplicator *out = rep;
    rep = NULL;
    return out;
}

clusterReplicator *clusterReplicatorCreate(
    hmacKeyring *ring, flexEnvelopeAlgo envelope_algo, uint32_t mailbox_flags,
    const persistOps *const *type_table, size_t type_table_len,
    clusterReplicatorApplyFn on_restored, void *apply_ctx) {
    /* Legacy entry: zero max_apply_retries preserves the prior
     * infinite-retry behaviour. Callers that want the drop-after-N
     * safety must use clusterReplicatorCreateEx. */
    const clusterReplicatorConfig cfg = {.max_apply_retries = 0u};
    return clusterReplicatorCreateEx(ring, envelope_algo, mailbox_flags,
                                     type_table, type_table_len, on_restored,
                                     apply_ctx, &cfg);
}

uint64_t clusterReplicatorDroppedCount(const clusterReplicator *rep) {
    if (rep == NULL) {
        return 0u;
    }
    /* Cast-away of const is local to this read of an _Atomic field;
     * the public contract is "thread-safe observer". */
    /* @sync: relaxed counter */
    return atomic_load_explicit(&((clusterReplicator *)rep)->dropped_count,
                                memory_order_relaxed);
}

uint64_t clusterReplicatorFormatRefusedCount(const clusterReplicator *rep) {
    if (rep == NULL) {
        return 0u;
    }
    /* @sync: relaxed counter */
    return atomic_load_explicit(
        &((clusterReplicator *)rep)->format_refused_count,
        memory_order_relaxed);
}

void clusterReplicatorFree(clusterReplicator *rep) {
    if (rep == NULL) {
        return;
    }
    clusterMailboxFree(rep->mb);
    zfree(rep);
}

clusterMailbox *clusterReplicatorMailbox(clusterReplicator *rep) {
    return (rep != NULL) ? rep->mb : NULL;
}

size_t clusterReplicatorSnapshotSize(clusterReplicator *rep,
                                     const persistOps *ops,
                                     const void *structure) {
    if (rep == NULL || ops == NULL || ops->snapshot == NULL ||
        structure == NULL) {
        return 0;
    }
    /* The snapshot's own length: ops->estimateSize is an approximation
     * for pre-allocation and falls short for many types, and Seal needs
     * a buffer the snapshot fits. */
    size_t snapshot_len = 0;
    cleanerName(zfree, Wire) uint8_t *snapshot =
        ops->snapshot(structure, &snapshot_len);
    if (snapshot == NULL) {
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
    if (rep == NULL || ops == NULL || ops->snapshot == NULL ||
        ops->type > UINT8_MAX) {
        return false;
    }
    size_t snapshot_len = 0;
    cleanerName(zfree, Wire) uint8_t *snapshot =
        ops->snapshot(structure, &snapshot_len);
    if (snapshot == NULL) {
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
    if (rep == NULL) {
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

int clusterReplicatorTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    static const persistOps *type_table[256];
    memset((void *)type_table, 0, sizeof(type_table));
    type_table[PERSIST_TYPE_FLEX] = &persistOpsFlex;

    TEST("end-to-end: flex ships through envelope + mailbox + restore") {
        const uint8_t key[32] = {0xAB};
        hmacKeyring *ring = hmacKeyringCreate();
        const hmacKeyId KID = 1u;
        hmacKeyringInstallUnsalted(ring, KID, &hmacAlgoSHA256, key,
                                   sizeof(key));

        replicatorTestCtx app_ctx = {0};
        clusterReplicator *rep = clusterReplicatorCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, CLUSTER_MAILBOX_FLAG_NONE,
            type_table, 256u, testApply, &app_ctx);
        if (rep == NULL) {
            ERRR("create");
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
            ERRR("seal");
        }
        if (clusterReplicatorIngest(rep, wire, written, KID) !=
            CLUSTER_MAILBOX_OK) {
            ERRR("ingest");
        }
        if (app_ctx.count != 1u) {
            ERR("expected 1 restored, got %zu", app_ctx.count);
        }
        if (app_ctx.origins[0] != 7u || app_ctx.seqs[0] != 1u) {
            ERRR("origin/seq mismatch");
        }
        /* Byte-exact integrity: a flex is uniquely defined by its
         * bytes, so memcmp of the source and restored is the
         * canonical equivalence test. */
        if (flexCount(app_ctx.restored[0]) != flexCount(src) ||
            flexBytes(app_ctx.restored[0]) != flexBytes(src) ||
            memcmp(app_ctx.restored[0], src, flexBytes(src)) != 0) {
            ERRR("restored flex differs from source");
        }
        flexFree(app_ctx.restored[0]);

        /* Tamper test. */
        app_ctx.count = 0;
        wire[written / 2] ^= 0x80;
        if (clusterReplicatorIngest(rep, wire, written, KID) !=
            CLUSTER_MAILBOX_ERR_VERIFY_FAILED) {
            ERRR("tampered frame must fail verify");
        }
        if (app_ctx.count != 0u) {
            ERRR("apply must not fire on verify failure");
        }
        wire[written / 2] ^= 0x80;

        /* Replay rejection. */
        if (clusterReplicatorIngest(rep, wire, written, KID) !=
            CLUSTER_MAILBOX_ERR_REPLAY) {
            ERRR("replay must be rejected");
        }

        /* Apply rejection + retry. */
        app_ctx.reject_next = true;
        size_t w2 = 0;
        clusterReplicatorSeal(rep, &persistOpsFlex, src, 7u, 2u, KID, wire, cap,
                              &w2);
        if (clusterReplicatorIngest(rep, wire, w2, KID) !=
            CLUSTER_MAILBOX_ERR_APPLY_REJECTED) {
            ERRR("apply-rejected expected");
        }
        if (clusterMailboxLastApplied(clusterReplicatorMailbox(rep), 7u) !=
            1u) {
            ERRR("watermark must stay at 1 after apply-rejected");
        }
        app_ctx.reject_next = false;
        if (clusterReplicatorIngest(rep, wire, w2, KID) != CLUSTER_MAILBOX_OK) {
            ERRR("retry must succeed");
        }
        if (clusterMailboxLastApplied(clusterReplicatorMailbox(rep), 7u) !=
            2u) {
            ERRR("watermark must advance to 2 after successful retry");
        }
        flexFree(app_ctx.restored[0]);

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
        replicatorTestCtx app_ctx = {0};
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
        replicatorTestCtx app_ctx = {0};
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
        for (size_t c = 0; c < sizeof(cases) / sizeof(*cases); c++) {
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
            clusterReplicatorIngest(rep, wire, written, KID);
            refused += !cases[c].restored;
            if ((app_ctx.count > before) != cases[c].restored ||
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

        replicatorTestCtx app_ctx = {0};
        clusterReplicator *rep = clusterReplicatorCreate(
            ring, FLEX_ENV_ALGO_HMAC_SHA256, CLUSTER_MAILBOX_FLAG_NONE,
            type_table, 256u, testApply, &app_ctx);

        /* Forge via the underlying mailbox: an unknown persistType
         * byte must surface as apply-rejected. */
        const uint8_t payload[4] = {0};
        clusterMailbox *mb = clusterReplicatorMailbox(rep);
        const size_t cap = clusterMailboxSealedSize(mb, sizeof(payload));
        cleanerName(zfree, Wire) uint8_t *wire = zmalloc(cap);
        size_t written = 0;
        clusterMailboxSeal(mb, 7u, 1u, (uint8_t)PERSIST_TYPE_MAX, payload,
                           sizeof(payload), KID, wire, cap, &written);
        if (clusterMailboxIngest(mb, wire, written, KID) !=
            CLUSTER_MAILBOX_ERR_APPLY_REJECTED) {
            ERRR("unknown type must surface as apply-rejected");
        }
        if (app_ctx.count != 0) {
            ERRR("no apply must fire on unknown type");
        }
        clusterReplicatorFree(rep);
        hmacKeyringFree(ring);
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
