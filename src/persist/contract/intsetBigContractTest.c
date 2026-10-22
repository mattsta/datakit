/* intsetBigContractTest.c - persistOpsIntsetBig in the persistence
 * contract */

#include "persistContract.h"

#ifdef DATAKIT_TEST

#include "../../intsetBig.h"
#include "../../str.h"

/* Signed and unsigned members, 64 and 128-bit, up to intsetBig's range
 * edges; a narrow spread so deletes and re-adds hit members that exist */
static void pcIntsetBigValue(uint64_t *rng, databoxBig *v) {
    const uint64_t r = splitmix64(rng);
    const int64_t pick = (int64_t)(splitmix64(rng) % 512) - 256;
    *v = (databoxBig){0};
    switch (r % 6) {
    case 0:
        *v = DATABOX_BIG_SIGNED(pick * INT64_C(1000000000000));
        break;
    case 1:
        *v = DATABOX_BIG_UNSIGNED((uint64_t)(pick + 256) << 50);
        break;
    case 2:
        DATABOX_BIG_SIGNED_128(v, INTSET_BIG_INT128_MAX - (pick & 15));
        break;
    case 3:
        DATABOX_BIG_SIGNED_128(v, INTSET_BIG_INT128_MIN + (pick & 15));
        break;
    case 4:
        DATABOX_BIG_UNSIGNED_128(v, INTSET_BIG_UINT128_MAX -
                                        (__uint128_t)(pick & 15));
        break;
    default:
        *v = DATABOX_BIG_SIGNED(pick);
        break;
    }
}

static void *pcIntsetBigBuild(const persistContractBuild *b) {
    /* @own: caller; free=intsetBigFree */
    intsetBig *isb = intsetBigNew();
    persistContractStart(b->log, isb);

    uint64_t rng = b->seed;
    for (size_t i = 0; i < b->size; i++) {
        databoxBig v;
        pcIntsetBigValue(&rng, &v);
        if (splitmix64(&rng) % 4 == 0) {
            intsetBigRemove(isb, &v);
            persistContractRecord(b->log, PERSIST_OP_DELETE, &v, 1);
        } else {
            intsetBigAdd(isb, &v);
            persistContractRecord(b->log, PERSIST_OP_INSERT, &v, 1);
        }

        if (b->size >= 100 && i == b->size / 3) {
            intsetBigFree(isb);
            /* @own: caller; free=intsetBigFree */
            isb = intsetBigNew();
            persistContractRecord(b->log, PERSIST_OP_CLEAR, NULL, 0);
        }
    }
    return isb;
}

const persistContractType persistContractIntsetBig = {
    .ops = &persistOpsIntsetBig,
    .build = pcIntsetBigBuild,
};

#endif /* DATAKIT_TEST */
