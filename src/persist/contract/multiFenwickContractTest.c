/* multiFenwickContractTest.c - the databox Fenwick trees (multiFenwick,
 * multiFenwickFlat, multiFenwick2d) in the persistence contract */

#include "persistContract.h"

#ifdef DATAKIT_TEST

#include "../../multiFenwick.h"
#include "../../multiFenwick2d.h"
#include "../../multiFenwickFlat.h"
#include "../../str.h"

/* A value the trees take: mostly small signed integers, sometimes a
 * large unsigned one, and (when 'mixed') sometimes a double, so cells
 * reach the unsigned and double kinds */
static databox pcMultiFenwickValue(uint64_t *rng, bool mixed) {
    const uint64_t roll = splitmix64(rng) % 10;
    if (roll == 0) {
        return DATABOX_UNSIGNED(splitmix64(rng) >> 2);
    }

    if (mixed && roll < 3) {
        const double unit =
            (double)(splitmix64(rng) >> 11) / (double)(UINT64_C(1) << 53);
        return DATABOX_DOUBLE((unit - 0.5) * 1e6);
    }

    return DATABOX_SIGNED((int64_t)(splitmix64(rng) % 2001) - 1000);
}

/* The 1D trees: Update or Set at an index over a span a little past the
 * size; a Clear a third of the way in */
#define PC_MULTI_FENWICK_1D(NAME, TYPE)                                        \
    static void *pc##NAME##Build(const persistContractBuild *b) {              \
        TYPE *t = NAME##New();                                                 \
        persistContractStart(b->log, t);                                       \
                                                                               \
        uint64_t rng = b->seed;                                                \
        const bool mixed = b->seed % 2 == 0;                                   \
        const size_t span = b->size + 1;                                       \
        for (size_t i = 0; i < b->size; i++) {                                 \
            const bool isUpdate = splitmix64(&rng) % 8 < 5;                    \
            const size_t idx = (size_t)(splitmix64(&rng) % span);              \
            const databox ib = DATABOX_UNSIGNED(idx);                          \
            const databox vb = pcMultiFenwickValue(&rng, mixed);               \
            const bool ok = isUpdate ? NAME##Update(&t, idx, &vb)              \
                                     : NAME##Set(&t, idx, &vb);                \
            if (ok) {                                                          \
                const databox *args[] = {&ib, &vb};                            \
                persistContractRecord(b->log,                                  \
                                      isUpdate ? PERSIST_OP_NUM_INCR           \
                                               : PERSIST_OP_UPDATE,            \
                                      args, 2);                                \
            }                                                                  \
                                                                               \
            if (b->size >= 100 && i == b->size / 3) {                          \
                NAME##Clear(t);                                                \
                persistContractRecord(b->log, PERSIST_OP_CLEAR, nullptr, 0);   \
            }                                                                  \
        }                                                                      \
                                                                               \
        return t;                                                              \
    }

PC_MULTI_FENWICK_1D(multiFenwick, multiFenwick)
PC_MULTI_FENWICK_1D(multiFenwickFlat, multiFenwickFlat)

static void *pcmultiFenwick2dBuild(const persistContractBuild *b) {
    /* @own: caller; free=multiFenwick2dFree */
    multiFenwick2d *t = multiFenwick2dNew();
    persistContractStart(b->log, t);

    uint64_t rng = b->seed;
    const bool mixed = b->seed % 2 == 0;
    size_t side = 1;
    while (side * side < b->size) {
        side++;
    }

    for (size_t i = 0; i < b->size; i++) {
        const bool isUpdate = splitmix64(&rng) % 8 < 5;
        const size_t row = (size_t)(splitmix64(&rng) % (side + 1));
        const size_t col = (size_t)(splitmix64(&rng) % (side + 1));
        const databox rb = DATABOX_UNSIGNED(row);
        const databox cb = DATABOX_UNSIGNED(col);
        const databox vb = pcMultiFenwickValue(&rng, mixed);
        const bool ok = isUpdate ? multiFenwick2dUpdate(&t, row, col, &vb)
                                 : multiFenwick2dSet(&t, row, col, &vb);
        if (ok) {
            const databox *args[] = {&rb, &cb, &vb};
            persistContractRecord(
                b->log, isUpdate ? PERSIST_OP_NUM_INCR : PERSIST_OP_UPDATE,
                args, 3);
        }

        if (b->size >= 100 && i == b->size / 3) {
            multiFenwick2dClear(t);
            persistContractRecord(b->log, PERSIST_OP_CLEAR, nullptr, 0);
        }
    }

    return t;
}

const persistContractType persistContractMultiFenwick = {
    .ops = &persistOpsMultiFenwick,
    .build = pcmultiFenwickBuild,
};

const persistContractType persistContractMultiFenwickFlat = {
    .ops = &persistOpsMultiFenwickFlat,
    .build = pcmultiFenwickFlatBuild,
};

const persistContractType persistContractMultiFenwick2d = {
    .ops = &persistOpsMultiFenwick2d,
    .build = pcmultiFenwick2dBuild,
};

#endif /* DATAKIT_TEST */
