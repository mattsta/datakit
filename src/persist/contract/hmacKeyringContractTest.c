/* hmacKeyringContractTest.c - persistOpsHmacKeyring in the persistence
 * contract
 *
 * Installs, retires, counter restores and the floor are made durable with
 * a new snapshot, so the generator makes them before the starting
 * snapshot.  Nonce reservations are the WAL: after the start, nonces are
 * issued under a reservation function that records each reservation as
 * the op hmacKeyringReserveThroughPersist would log, so contracts 1-3
 * apply. */

#include "persistContract.h"

#ifdef DATAKIT_TEST

#include "../../hmacKeyring.h"
#include "../../str.h"

#include <string.h>

/* Records each reservation as the WAL op persistOpsHmacKeyring replays. */
static bool pcKeyringReserve(void *ctx, hmacKeyId id, uint64_t limit) {
    const databox idBox = DATABOX_UNSIGNED(id);
    const databox limitBox = DATABOX_UNSIGNED(limit);
    const databox *const args[2] = {&idBox, &limitBox};
    persistContractRecord(ctx, PERSIST_OP_UPDATE, args, 2);
    return true;
}

static void *pcKeyringBuild(const persistContractBuild *b) {
    /* @own: caller; free=hmacKeyringFree */
    hmacKeyring *ring = hmacKeyringCreate();

    uint64_t rng = b->seed;
    hmacKeyId floor = 0;
    for (size_t i = 0; i < b->size; i++) {
        const hmacKeyId id = 1 + (hmacKeyId)(splitmix64(&rng) % 40);
        const uint64_t roll = splitmix64(&rng) % 8;
        if (roll < 4) {
            /* Install: key bytes of 1..64 digits and letters, salted or
             * not, either algorithm (a full ring or a taken id refuses) */
            uint8_t key[64];
            const size_t keyLen = 1 + (size_t)(splitmix64(&rng) % 64);
            for (size_t k = 0; k < keyLen; k++) {
                key[k] = (uint8_t)"0123456789abcdef"[splitmix64(&rng) % 16];
            }
            const hmacHashAlgo *algo =
                roll & 1 ? &hmacAlgoSHA256 : &hmacAlgoBLAKE3;
            if (roll < 2) {
                hmacKeyringInstallUnsalted(ring, id, algo, key, keyLen);
            } else {
                uint8_t salt[HMAC_KEYRING_AEAD_SALT_BYTES];
                const uint32_t s = (uint32_t)splitmix64(&rng) | 1;
                memcpy(salt, &s, sizeof(salt));
                hmacKeyringInstall(ring, id, algo, key, keyLen, salt);
            }
        } else if (roll == 4) {
            hmacKeyringRetire(ring, id);
        } else if (roll == 5) {
            hmacKeyringRestoreAeadCounter(ring, id, splitmix64(&rng) >> 20);
        } else if (roll == 6 && floor < 30) {
            floor += 1 + (hmacKeyId)(splitmix64(&rng) % 3);
            hmacKeyringSetMinValidKeyId(ring, floor);
        }
    }

    /* The WAL part: nonces under reservations of a seeded window */
    persistContractStart(b->log, ring);
    const uint64_t window = 1 + splitmix64(&rng) % 16;
    hmacKeyringSetAeadReservation(ring, window, pcKeyringReserve, b->log);
    for (size_t i = 0; i < b->size; i++) {
        const hmacKeyId id = 1 + (hmacKeyId)(splitmix64(&rng) % 40);
        uint8_t nonce[12];
        hmacKeyringNextAeadNonce(ring, id, nonce); /* absent ids refuse */
    }
    hmacKeyringSetAeadReservation(ring, 0, nullptr, nullptr);
    return ring;
}

const persistContractType persistContractHmacKeyring = {
    .ops = &persistOpsHmacKeyring,
    .build = pcKeyringBuild,
};

#endif /* DATAKIT_TEST */
