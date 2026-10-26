/* flexContractTest.c - persistOpsFlex in the persistence contract */

#include "persistContract.h"

#ifdef DATAKIT_TEST

#include "../../flex.h"
#include "../../str.h"

static void pcFlexPush(flex **f, const persistContractBuild *b,
                       const databox *v, bool tail) {
    flexPushByTypeExact(f, v, tail ? FLEX_ENDPOINT_TAIL : FLEX_ENDPOINT_HEAD);
    persistContractRecord(
        b->log, tail ? PERSIST_OP_PUSH_TAIL : PERSIST_OP_PUSH_HEAD, v, 1);
}

static void *pcFlexBuild(const persistContractBuild *b) {
    /* @own: caller; free=flexFree */
    flex *f = flexNew();
    persistContractStart(b->log, f);

    uint64_t rng = b->seed;
    uint8_t buf[64];
    for (size_t i = 0; i < b->size; i++) {
        databoxBig v;
        persistContractValue(&rng, &v, buf, sizeof(buf));
        pcFlexPush(&f, b, (const databox *)&v, splitmix64(&rng) & 1);
        if (b->size >= 100 && i == b->size / 3) {
            flexReset(&f);
            persistContractRecord(b->log, PERSIST_OP_CLEAR, nullptr, 0);
        }
    }

    for (size_t i = 0; i < b->stringCount; i++) {
        pcFlexPush(&f, b, &b->strings[i], i & 1);
    }
    return f;
}

static bool pcFlexHoldsBytes(const void *structure, const databox *bytes) {
    const flex *f = structure;
    const size_t n = flexCount(f);
    flexEntry *fe = flexHead(f);
    for (size_t i = 0; i < n; i++) {
        databoxBig v;
        flexGetByTypeBig(fe, &v);
        if (persistContractSameBytes((const databox *)&v, bytes)) {
            return true;
        }
        fe = flexNext(f, fe);
    }
    return false;
}

const persistContractType persistContractFlex = {
    .ops = &persistOpsFlex,
    .build = pcFlexBuild,
    .holdsBytes = pcFlexHoldsBytes,
};

#endif /* DATAKIT_TEST */
