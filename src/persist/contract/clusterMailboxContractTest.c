/* clusterMailboxContractTest.c - persistOpsClusterMailbox in the
 * persistence contract
 *
 * Snapshot-only: the watermark table is made durable with a new
 * snapshot, so contracts 1 and 3 apply. */

#include "persistContract.h"

#ifdef DATAKIT_TEST

#include "../../clusterMailbox.h"
#include "../../str.h"

#include <string.h>

#define PC_MAILBOX_KEY_ID ((hmacKeyId)1)

/* The keyring the generated mailboxes borrow: a process-lifetime test
 * fixture, since a mailbox borrows its ring until clusterMailboxFree
 * and the harness frees mailboxes through the descriptor alone. */
static hmacKeyring *pcMailboxRing(void) {
    static hmacKeyring *ring = NULL;
    if (ring == NULL) {
        /* @own: container; free=hmacKeyringFree */
        ring = hmacKeyringCreate();
        static constexpr char key[] = "persistContract mailbox test key";
        hmacKeyringInstallUnsalted(ring, PC_MAILBOX_KEY_ID, &hmacAlgoSHA256,
                                   (const uint8_t *)key, sizeof(key) - 1);
    }
    return ring;
}

/* Applies a frame unless its first payload byte asks for a rejection */
static bool pcMailboxApply(const clusterFrame *frame, void *) {
    return frame->payload_len == 0 || frame->payload[0] != 0;
}

static void *pcMailboxBuild(const persistContractBuild *b) {
    const clusterMailboxCreateOptions opts = {
        .ring = pcMailboxRing(),
        .envelope_algo = FLEX_ENV_ALGO_HMAC_SHA256,
        .on_apply = pcMailboxApply,
        .flags = (b->seed & 1) ? CLUSTER_MAILBOX_FLAG_REPLAY_WINDOW
                               : CLUSTER_MAILBOX_FLAG_NONE,
        .disable_nonce_log = true,
    };
    /* @own: caller; free=clusterMailboxFree */
    clusterMailbox *mb = clusterMailboxCreateOpts(&opts);
    persistContractStart(b->log, mb);

    /* Frames from 70 origins (more than the table holds), each origin
     * moving forward with gaps and stepping back inside and beyond the
     * replay window; one frame in 16 is apply-rejected */
    uint64_t rng = b->seed;
    uint64_t next[70] = {};
    uint8_t frame[256];
    for (size_t i = 0; i < b->size; i++) {
        const size_t o = (size_t)(splitmix64(&rng) % 70);
        const uint64_t roll = splitmix64(&rng);
        uint64_t seq = next[o] + 1 + (roll % 5);
        if ((roll >> 8) % 8 == 0 && next[o] > 1) {
            seq = 1 + (roll >> 16) % next[o];
        } else {
            next[o] = seq;
        }

        const uint8_t payload[1] = {(uint8_t)((roll >> 24) % 16)};
        size_t len = 0;
        if (clusterMailboxSeal(mb, (uint32_t)(o + 1), seq, 0, payload,
                               sizeof(payload), PC_MAILBOX_KEY_ID, frame,
                               sizeof(frame), &len)) {
            /* Refused as a replay or for a full origin table, else
             * applied as the payload asks */
            const clusterMailboxIngestResult rc =
                clusterMailboxIngest(mb, frame, len, PC_MAILBOX_KEY_ID);
            assert(rc == CLUSTER_MAILBOX_ERR_REPLAY ||
                   rc == CLUSTER_MAILBOX_ERR_ORIGIN_TABLE_FULL ||
                   rc == (payload[0] ? CLUSTER_MAILBOX_OK
                                     : CLUSTER_MAILBOX_ERR_APPLY_REJECTED));
        }
    }
    return mb;
}

const persistContractType persistContractClusterMailbox = {
    .ops = &persistOpsClusterMailbox,
    .build = pcMailboxBuild,
};

#endif /* DATAKIT_TEST */
