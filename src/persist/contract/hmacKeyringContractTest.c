/* hmacKeyringContractTest.c - persistOpsHmacKeyring in the persistence
 * contract
 *
 * Snapshot-only: every keyring change is made durable with a new
 * snapshot, so contracts 1 and 3 apply. */

#include "persistContract.h"

#ifdef DATAKIT_TEST

#include "../../hmacKeyring.h"
#include "../../str.h"

#include <string.h>

static void *pcKeyringBuild(const persistContractBuild *b) {
    /* @own: caller; free=hmacKeyringFree */
    hmacKeyring *ring = hmacKeyringCreate();
    persistContractStart(b->log, ring);

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
    return ring;
}

const persistContractType persistContractHmacKeyring = {
    .ops = &persistOpsHmacKeyring,
    .build = pcKeyringBuild,
};

#endif /* DATAKIT_TEST */
