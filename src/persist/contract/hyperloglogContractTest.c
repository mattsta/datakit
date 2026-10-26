/* hyperloglogContractTest.c - persistOpsHyperloglog in the persistence
 * contract */

#include "persistContract.h"

#ifdef DATAKIT_TEST

#include "../../hyperloglog.h"
#include "../../mdsc.h"
#include "../../str.h"

#include <inttypes.h>
#include <stdio.h>

/* Member bytes: numerals and words, since the exact bytes are hashed */
static size_t pcHllMember(uint64_t *rng, char *buf, size_t bufLen) {
    const uint64_t r = splitmix64(rng);
    const int n = r & 1 ? snprintf(buf, bufLen, "%" PRIu64, r % 1'000'000)
                        : snprintf(buf, bufLen, "member-%" PRIu64, r % 50000);
    return (size_t)n;
}

static void *pcHllBuild(const persistContractBuild *b) {
    /* @own: caller; free=hyperloglogFree */
    hyperloglog *h = hyperloglogNew();
    persistContractStart(b->log, h);

    uint64_t rng = b->seed;
    char buf[32];
    for (size_t i = 0; i < b->size; i++) {
        if (splitmix64(&rng) % 64 == 0) {
            /* Merge in another HLL of a few hundred members */
            /* @own: container; free=hyperloglogFree */
            hyperloglog *other = hyperloglogNew();
            const size_t members = splitmix64(&rng) % 300;
            for (size_t m = 0; m < members; m++) {
                const size_t len = pcHllMember(&rng, buf, sizeof(buf));
                hyperloglogAdd(&other, buf, len);
            }
            hyperloglogMergeInto(&h, other);
            persistContractRecord(b->log, PERSIST_OP_MERGE, other,
                                  mdsclen((const mdsc *)other));
            hyperloglogFree(other);
        } else {
            const size_t len = pcHllMember(&rng, buf, sizeof(buf));
            hyperloglogAdd(&h, buf, len);
            hyperloglogInvalidateCache(h);
            persistContractRecord(b->log, PERSIST_OP_INSERT, buf, len);
        }
    }
    return h;
}

const persistContractType persistContractHyperloglog = {
    .ops = &persistOpsHyperloglog,
    .build = pcHllBuild,
};

#endif /* DATAKIT_TEST */
