/* aeadNonceLogContractTest.c - persistOpsAeadNonceLog in the persistence
 * contract
 *
 * Snapshot-only: the window is made durable with a new snapshot, so
 * contracts 1 and 3 apply. */

#include "persistContract.h"

#ifdef DATAKIT_TEST

#include "../../aeadNonceLog.h"
#include "../../str.h"

#include <string.h>

static void *pcNonceLogBuild(const persistContractBuild *b) {
    uint64_t rng = b->seed;

    /* A window from 64 slots up to about half the build, so small builds
     * stay below the load limit and larger ones evict */
    size_t cap = 64;
    while (cap < b->size / 2) {
        cap <<= 1u;
    }
    /* @own: caller; free=aeadNonceLogFree */
    aeadNonceLog *log = aeadNonceLogCreate(cap);
    persistContractStart(b->log, log);

    /* Observations from 5 keys over a nonce space twice the build, so
     * reuses are refused; now and then a key is cleared */
    const uint64_t space = 2 + 2 * (uint64_t)b->size;
    for (size_t i = 0; i < b->size; i++) {
        const uint32_t key = 1 + (uint32_t)(splitmix64(&rng) % 5);
        if (splitmix64(&rng) % 1500 == 0) {
            aeadNonceLogClearKey(log, key);
            continue;
        }
        const uint64_t v = splitmix64(&rng) % space;
        uint8_t nonce[AEAD_NONCE_LOG_NONCE_LEN] = {};
        memcpy(nonce, &v, sizeof(v));
        nonce[8] = (uint8_t)(v * 31);
        aeadNonceLogObserve(log, key, nonce);
    }
    return log;
}

const persistContractType persistContractAeadNonceLog = {
    .ops = &persistOpsAeadNonceLog,
    .build = pcNonceLogBuild,
};

#endif /* DATAKIT_TEST */
