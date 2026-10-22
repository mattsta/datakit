/* intsetContractTest.c - persistOpsIntset in the persistence contract */

#include "persistContract.h"

#ifdef DATAKIT_TEST

#include "../../intset.h"
#include "../../str.h"

/* int16, int32 and int64 values of both signs (and the int64 extremes),
 * so every tier and every run of the tier arrays is reached; a narrow
 * range, so deletes and re-adds hit values that exist */
static int64_t pcIntsetValue(uint64_t *rng) {
    const uint64_t r = splitmix64(rng);
    const int64_t pick = (int64_t)(splitmix64(rng) % 512) - 256;
    switch (r % 8) {
    case 0:
        return pick * 100000; /* int32 */
    case 1:
        return pick * INT64_C(10000000000); /* int64 */
    case 2:
        return r & 1 ? INT64_MAX - (pick & 7) : INT64_MIN + (pick & 7);
    default:
        return pick; /* int16 */
    }
}

static void *pcIntsetBuild(const persistContractBuild *b) {
    /* @own: caller; free=intsetFree */
    intset *is = intsetNew();
    persistContractStart(b->log, is);

    uint64_t rng = b->seed;
    for (size_t i = 0; i < b->size; i++) {
        const int64_t value = pcIntsetValue(&rng);
        bool changed;
        if (splitmix64(&rng) % 4 == 0) {
            intsetRemove(&is, value, &changed);
            persistContractRecord(b->log, PERSIST_OP_DELETE, &value, 1);
        } else {
            intsetAdd(&is, value, &changed);
            persistContractRecord(b->log, PERSIST_OP_INSERT, &value, 1);
        }

        if (b->size >= 100 && i == b->size / 3) {
            intsetFree(is);
            /* @own: caller; free=intsetFree */
            is = intsetNew();
            persistContractRecord(b->log, PERSIST_OP_CLEAR, NULL, 0);
        }
    }
    return is;
}

const persistContractType persistContractIntset = {
    .ops = &persistOpsIntset,
    .build = pcIntsetBuild,
};

#endif /* DATAKIT_TEST */
