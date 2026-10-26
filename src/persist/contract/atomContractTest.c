/* atomContractTest.c - the atom tables (atomPersist.h) in the persistence
 * contract */

#include "persistContract.h"

#ifdef DATAKIT_TEST

#include "../../atomPersist.h"
#include "../../atomPool.h"
#include "../../multimapAtom.h"
#include "../../multimapAtomDense.h"
#include "../../str.h"
#include "../../stringPool.h"

#include <inttypes.h>
#include <stdio.h>

/* Value number 'i' of a build, a function of (seed, i), so a later step
 * can name an earlier value again */
static void pcAtomValue(uint64_t seed, size_t i, databoxBig *value,
                        uint8_t *buf, size_t bufLen) {
    uint64_t rng = seed ^ ((uint64_t)i * UINT64_C(0x9E3779B97F4A7C15));
    persistContractValue(&rng, value, buf, bufLen);
}

static void pcAtomRecordId(persistContractLog *log, persistOp op, uint64_t id,
                           const databox *second) {
    const databox idBox = DATABOX_UNSIGNED(id);
    const databox *args[2] = {&idBox, second};
    persistContractRecord(log, op, args, second ? 2 : 1);
}

/* Interns new and known values (with and without a retain), retains by
 * id, releases by value (the last release deletes) and deletes by id; a
 * quarter of the seeds build a permanent table */
static void *pcMultimapAtomBuild(const persistContractBuild *b) {
    multimapAtom *ma =
        b->seed % 4 == 0 ? multimapAtomNewPermanent() : multimapAtomNew();
    persistContractStart(b->log, ma);

    uint64_t rng = b->seed;
    uint8_t buf[64];
    const databox plusOne = DATABOX_SIGNED(1);
    const databox minusOne = DATABOX_SIGNED(-1);
    for (size_t i = 0; i < b->size; i++) {
        const uint64_t roll = splitmix64(&rng) % 8;
        const size_t k =
            (roll >= 3 && i > 0) ? (size_t)(splitmix64(&rng) % i) : i;
        databoxBig value;
        pcAtomValue(b->seed, k, &value, buf, sizeof(buf));
        databox ref = {};
        const bool live =
            multimapAtomLookupReference(ma, (const databox *)&value, &ref);
        if (roll < 3 || !live) {
            /* Intern: a new atom is an INSERT; AndRetain on a known one is
             * a retain */
            databox key = *(const databox *)&value;
            const bool andRetain = roll == 1;
            const bool existed =
                andRetain ? multimapAtomInsertIfNewConvertAndRetain(ma, &key)
                          : multimapAtomInsertIfNewConvert(ma, &key);
            if (!existed) {
                pcAtomRecordId(b->log, PERSIST_OP_INSERT, key.data.u,
                               (const databox *)&value);
            } else if (andRetain) {
                pcAtomRecordId(b->log, PERSIST_OP_NUM_INCR, key.data.u,
                               &plusOne);
            }
        } else if (roll < 5) {
            multimapAtomRetainById(ma, ref.data.u);
            pcAtomRecordId(b->log, PERSIST_OP_NUM_INCR, ref.data.u, &plusOne);
        } else if (roll < 7) {
            multimapAtomRelease(ma, (const databox *)&value);
            pcAtomRecordId(b->log, PERSIST_OP_NUM_INCR, ref.data.u, &minusOne);
        } else {
            multimapAtomDeleteById(ma, ref.data.u);
            pcAtomRecordId(b->log, PERSIST_OP_DELETE, ref.data.u, NULL);
        }
    }

    for (size_t i = 0; i < b->stringCount; i++) {
        databox key = b->strings[i];
        if (!multimapAtomInsertIfNewConvert(ma, &key)) {
            pcAtomRecordId(b->log, PERSIST_OP_INSERT, key.data.u,
                           &b->strings[i]);
        }
    }

    return ma;
}

static bool pcMultimapAtomHoldsBytes(const void *structure,
                                     const databox *bytes) {
    const multimapAtom *ma = structure;
    databox ref = {};
    databox got = {};
    return multimapAtomLookupReference(ma, bytes, &ref) &&
           multimapAtomLookup(ma, &ref, &got) &&
           persistContractSameBytes(&got, bytes);
}

const persistContractType persistContractMultimapAtom = {
    .ops = &persistOpsMultimapAtom,
    .build = pcMultimapAtomBuild,
    .holdsBytes = pcMultimapAtomHoldsBytes,
};

/* As pcMultimapAtomBuild through the lowest-first table: releases and
 * deletes leave holes that later interns fill */
static void *pcMultimapAtomDenseBuild(const persistContractBuild *b) {
    multimapAtomDense *d = multimapAtomDenseNew();
    persistContractStart(b->log, d);

    uint64_t rng = b->seed;
    uint8_t buf[64];
    const databox plusOne = DATABOX_SIGNED(1);
    const databox minusOne = DATABOX_SIGNED(-1);
    for (size_t i = 0; i < b->size; i++) {
        const uint64_t roll = splitmix64(&rng) % 8;
        const size_t k =
            (roll >= 3 && i > 0) ? (size_t)(splitmix64(&rng) % i) : i;
        databoxBig value;
        pcAtomValue(b->seed, k, &value, buf, sizeof(buf));
        databox ref = {};
        const bool live = multimapAtomLookupReference(
            multimapAtomDenseBase(d), (const databox *)&value, &ref);
        if (roll < 3 || !live) {
            databox key = *(const databox *)&value;
            const bool andRetain = roll == 1;
            const bool existed =
                andRetain
                    ? multimapAtomDenseInsertIfNewConvertAndRetain(d, &key)
                    : multimapAtomDenseInsertIfNewConvert(d, &key);
            if (!existed) {
                pcAtomRecordId(b->log, PERSIST_OP_INSERT, key.data.u,
                               (const databox *)&value);
            } else if (andRetain) {
                pcAtomRecordId(b->log, PERSIST_OP_NUM_INCR, key.data.u,
                               &plusOne);
            }
        } else if (roll < 5) {
            multimapAtomDenseRetainById(d, ref.data.u);
            pcAtomRecordId(b->log, PERSIST_OP_NUM_INCR, ref.data.u, &plusOne);
        } else if (roll < 7) {
            multimapAtomDenseRelease(d, (const databox *)&value);
            pcAtomRecordId(b->log, PERSIST_OP_NUM_INCR, ref.data.u, &minusOne);
        } else {
            multimapAtomDenseDeleteById(d, ref.data.u);
            pcAtomRecordId(b->log, PERSIST_OP_DELETE, ref.data.u, NULL);
        }
    }

    for (size_t i = 0; i < b->stringCount; i++) {
        databox key = b->strings[i];
        if (!multimapAtomDenseInsertIfNewConvert(d, &key)) {
            pcAtomRecordId(b->log, PERSIST_OP_INSERT, key.data.u,
                           &b->strings[i]);
        }
    }

    return d;
}

static bool pcMultimapAtomDenseHoldsBytes(const void *structure,
                                          const databox *bytes) {
    return pcMultimapAtomHoldsBytes(
        multimapAtomDenseBase((const multimapAtomDense *)structure), bytes);
}

const persistContractType persistContractMultimapAtomDense = {
    .ops = &persistOpsMultimapAtomDense,
    .build = pcMultimapAtomDenseBuild,
    .holdsBytes = pcMultimapAtomDenseHoldsBytes,
};

/* String number 'i' of a build: words, digit strings and signed or
 * fractional numerals, a function of (seed, i) */
static databox pcStringPoolValue(uint64_t seed, size_t i, char *buf,
                                 size_t bufLen) {
    uint64_t rng = seed ^ ((uint64_t)i * UINT64_C(0x9E3779B97F4A7C15));
    const uint64_t r = splitmix64(&rng);
    int len;
    switch (r % 4) {
    case 0:
        len = snprintf(buf, bufLen, "%" PRIu64, r >> 20);
        break;
    case 1:
        len = snprintf(buf, bufLen, "00%" PRIu64, r >> 50);
        break;
    case 2:
        len = snprintf(buf, bufLen, "-%" PRIu64 ".5", r >> 44);
        break;
    default:
        len = snprintf(buf, bufLen, "word-%" PRIx64, r >> 30);
        break;
    }

    return databoxNewBytes(buf, (size_t)len);
}

/* Interns new and known strings, retains and releases by id (the last
 * release frees), and Resets once in larger builds; the seed picks the
 * id behaviour */
static void *pcStringPoolBuild(const persistContractBuild *b) {
    const stringPoolIds ids = (stringPoolIds)(b->seed % 3);
    /* @own: caller; free=stringPoolFree */
    stringPool *pool = stringPoolNewWithIds(ids);
    persistContractStart(b->log, pool);

    uint64_t rng = b->seed;
    char buf[64];
    const databox plusOne = DATABOX_SIGNED(1);
    const databox minusOne = DATABOX_SIGNED(-1);
    for (size_t i = 0; i < b->size; i++) {
        const uint64_t roll = splitmix64(&rng) % 8;
        const size_t k =
            (roll >= 3 && i > 0) ? (size_t)(splitmix64(&rng) % i) : i;
        const databox str = pcStringPoolValue(b->seed, k, buf, sizeof(buf));
        const uint64_t known = stringPoolGetId(pool, &str);
        if (roll < 3 || !known) {
            const uint64_t id = stringPoolIntern(pool, &str);
            if (!known) {
                pcAtomRecordId(b->log, PERSIST_OP_INSERT, id, &str);
            } else if (ids != STRING_POOL_IDS_PERMANENT) {
                pcAtomRecordId(b->log, PERSIST_OP_NUM_INCR, id, &plusOne);
            }
        } else if (roll < 5) {
            stringPoolRetain(pool, known);
            pcAtomRecordId(b->log, PERSIST_OP_NUM_INCR, known, &plusOne);
        } else {
            stringPoolRelease(pool, known);
            pcAtomRecordId(b->log, PERSIST_OP_NUM_INCR, known, &minusOne);
        }

        if (b->size >= 100 && i == b->size / 3) {
            stringPoolReset(pool);
            persistContractRecord(b->log, PERSIST_OP_CLEAR, NULL, 0);
        }
    }

    for (size_t i = 0; i < b->stringCount; i++) {
        const uint64_t known = stringPoolGetId(pool, &b->strings[i]);
        const uint64_t id = stringPoolIntern(pool, &b->strings[i]);
        if (!known) {
            pcAtomRecordId(b->log, PERSIST_OP_INSERT, id, &b->strings[i]);
        } else if (ids != STRING_POOL_IDS_PERMANENT) {
            pcAtomRecordId(b->log, PERSIST_OP_NUM_INCR, id, &plusOne);
        }
    }

    return pool;
}

static bool pcStringPoolHoldsBytes(const void *structure,
                                   const databox *bytes) {
    const stringPool *pool = structure;
    databox got = {};
    const uint64_t id = stringPoolGetId(pool, bytes);
    return id && stringPoolLookup(pool, id, &got) &&
           persistContractSameBytes(&got, bytes);
}

const persistContractType persistContractStringPool = {
    .ops = &persistOpsStringPool,
    .build = pcStringPoolBuild,
    .holdsBytes = pcStringPoolHoldsBytes,
};

/* As pcStringPoolBuild through the pool interface, over both backends and
 * all three id behaviours (picked by the seed) */
static void *pcAtomPoolBuild(const persistContractBuild *b) {
    const atomPoolType type = (atomPoolType)(b->seed % 2);
    const atomPoolIds ids = (atomPoolIds)((b->seed / 2) % 3);
    /* @own: caller; free=atomPoolFree */
    atomPool *pool = atomPoolNewWithIds(type, ids);
    persistContractStart(b->log, pool);

    uint64_t rng = b->seed;
    char buf[64];
    const databox plusOne = DATABOX_SIGNED(1);
    const databox minusOne = DATABOX_SIGNED(-1);
    for (size_t i = 0; i < b->size; i++) {
        const uint64_t roll = splitmix64(&rng) % 8;
        const size_t k =
            (roll >= 3 && i > 0) ? (size_t)(splitmix64(&rng) % i) : i;
        const databox str = pcStringPoolValue(b->seed, k, buf, sizeof(buf));
        const uint64_t known = atomPoolGetId(pool, &str);
        if (roll < 3 || !known) {
            const uint64_t id = atomPoolIntern(pool, &str);
            if (!known) {
                pcAtomRecordId(b->log, PERSIST_OP_INSERT, id, &str);
            } else if (ids != ATOM_POOL_IDS_PERMANENT) {
                pcAtomRecordId(b->log, PERSIST_OP_NUM_INCR, id, &plusOne);
            }
        } else if (roll < 5) {
            atomPoolRetain(pool, known);
            pcAtomRecordId(b->log, PERSIST_OP_NUM_INCR, known, &plusOne);
        } else {
            atomPoolRelease(pool, known);
            pcAtomRecordId(b->log, PERSIST_OP_NUM_INCR, known, &minusOne);
        }

        if (b->size >= 100 && i == b->size / 3) {
            atomPoolReset(pool);
            persistContractRecord(b->log, PERSIST_OP_CLEAR, NULL, 0);
        }
    }

    for (size_t i = 0; i < b->stringCount; i++) {
        const uint64_t known = atomPoolGetId(pool, &b->strings[i]);
        const uint64_t id = atomPoolIntern(pool, &b->strings[i]);
        if (!known) {
            pcAtomRecordId(b->log, PERSIST_OP_INSERT, id, &b->strings[i]);
        } else if (ids != ATOM_POOL_IDS_PERMANENT) {
            pcAtomRecordId(b->log, PERSIST_OP_NUM_INCR, id, &plusOne);
        }
    }

    return pool;
}

static bool pcAtomPoolHoldsBytes(const void *structure, const databox *bytes) {
    const atomPool *pool = structure;
    databox got = {};
    const uint64_t id = atomPoolGetId(pool, bytes);
    return id && atomPoolLookup(pool, id, &got) &&
           persistContractSameBytes(&got, bytes);
}

const persistContractType persistContractAtomPool = {
    .ops = &persistOpsAtomPool,
    .build = pcAtomPoolBuild,
    .holdsBytes = pcAtomPoolHoldsBytes,
};

#endif /* DATAKIT_TEST */
