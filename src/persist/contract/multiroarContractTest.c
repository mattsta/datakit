/* multiroarContractTest.c - persistOpsMultiroar in the persistence contract
 *
 * Bit mode: the mode multiroarP writes a WAL for. */

#include "persistContract.h"

#ifdef DATAKIT_TEST

#include "../../multiroar.h"
#include "../../str.h"

/* A position near one of a few chunks (dense, so chunks move through
 * their sparse, dense and full forms) or anywhere in a wide space */
static uint64_t pcMultiroarPosition(uint64_t *rng) {
    static const uint64_t bases[] = {0, 8192, 3 * 8192, UINT64_C(1) << 40};
    const uint64_t r = splitmix64(rng);
    if (r % 8 == 0) {
        return splitmix64(rng) % (UINT64_C(1) << 44);
    }
    return bases[(r >> 3) % 4] + splitmix64(rng) % 9000;
}

static void *pcMultiroarBuild(const persistContractBuild *b) {
    /* @own: caller; free=multiroarFree */
    multiroar *r = multiroarBitNew();
    persistContractStart(b->log, r);

    uint64_t rng = b->seed;
    for (size_t i = 0; i < b->size; i++) {
        const uint64_t roll = splitmix64(&rng) % 16;
        const uint64_t pos = pcMultiroarPosition(&rng);
        if (roll < 9) {
            multiroarBitSet(r, pos);
            persistContractRecord(b->log, PERSIST_OP_INSERT, &pos, 1);
        } else if (roll < 13) {
            multiroarRemove(r, pos);
            persistContractRecord(b->log, PERSIST_OP_DELETE, &pos, 1);
        } else {
            /* Up to just over a chunk, so ranges cover whole chunks and
             * cross chunk edges */
            const uint64_t range[2] = {pos, 1 + splitmix64(&rng) % 9000};
            if (roll == 13) {
                multiroarBitClearRange(r, range[0], range[1]);
                persistContractRecord(b->log, PERSIST_OP_CLEAR_RANGE, range, 2);
            } else {
                multiroarBitFlipRange(r, range[0], range[1]);
                persistContractRecord(b->log, PERSIST_OP_FLIP_RANGE, range, 2);
            }
        }

        if (b->size >= 100 && i == b->size / 3) {
            multiroarFree(r);
            /* @own: caller; free=multiroarFree */
            r = multiroarBitNew();
            persistContractRecord(b->log, PERSIST_OP_CLEAR, NULL, 0);
        }
    }
    return r;
}

const persistContractType persistContractMultiroar = {
    .ops = &persistOpsMultiroar,
    .build = pcMultiroarBuild,
};

#endif /* DATAKIT_TEST */
