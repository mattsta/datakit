/* multiOrderedSetContractTest.c - persistOpsMultiOrderedSet in the
 * persistence contract */

#include "persistContract.h"

#ifdef DATAKIT_TEST

#include "../../multiOrderedSet.h"
#include "../../str.h"

/* Member number 'i', a function of (seed, i), so an update or remove can
 * name an earlier member again */
static void pcMosMember(uint64_t seed, size_t i, databoxBig *member,
                        uint8_t *buf, size_t bufLen) {
    uint64_t rng = seed ^ ((uint64_t)i * UINT64_C(0x9E3779B97F4A7C15));
    persistContractValue(&rng, member, buf, bufLen);
}

/* A score: an integer, a double or (now and then) a 128-bit integer */
static void pcMosScore(uint64_t *rng, databoxBig *score) {
    const uint64_t r = splitmix64(rng);
    *score = (databoxBig){};
    switch (r % 8) {
    case 0:
        score->type = DATABOX_DOUBLE_64;
        score->data.d64 = (double)(int64_t)(r >> 8) / 4096.0;
        break;
    case 1:
        DATABOX_BIG_SIGNED_128(score,
                               (__int128_t)(int64_t)r * ((__int128_t)1 << 40));
        break;
    default:
        *score = DATABOX_BIG_SIGNED((int64_t)(r % 2000) - 1000);
        break;
    }
}

static void *pcMosBuild(const persistContractBuild *b) {
    /* The limit varies by seed, so the snapshot carries it: the default,
     * or a small limit (builds reach Medium and Full) of two sizes */
    multiOrderedSet *mos;
    switch (b->seed % 3) {
    case 0:
        /* @own: caller; free=multiOrderedSetFree */
        mos = multiOrderedSetNew();
        break;
    case 1:
        /* @own: caller; free=multiOrderedSetFree */
        mos = multiOrderedSetNewLimit(FLEX_CAP_LEVEL_256);
        break;
    default:
        /* @own: caller; free=multiOrderedSetFree */
        mos = multiOrderedSetNewLimit(FLEX_CAP_LEVEL_512);
        break;
    }
    persistContractStart(b->log, mos);

    uint64_t rng = b->seed;
    uint8_t buf[64];
    for (size_t i = 0; i < b->size; i++) {
        databoxBig member;
        const uint64_t roll = splitmix64(&rng) % 8;
        const size_t k =
            (roll < 3 && i > 0) ? (size_t)(splitmix64(&rng) % i) : i;
        pcMosMember(b->seed, k, &member, buf, sizeof(buf));
        if (roll == 0) {
            const databox *args[] = {(const databox *)&member};
            multiOrderedSetRemove(&mos, args[0]);
            persistContractRecord(b->log, PERSIST_OP_DELETE, args, 1);
        } else {
            databoxBig score;
            pcMosScore(&rng, &score);
            const databox *args[] = {(const databox *)&score,
                                     (const databox *)&member};
            multiOrderedSetAdd(&mos, args[0], args[1]);
            persistContractRecord(b->log, PERSIST_OP_INSERT, args, 2);
        }

        if (b->size >= 100 && i == b->size / 3) {
            multiOrderedSetReset(&mos);
            persistContractRecord(b->log, PERSIST_OP_CLEAR, nullptr, 0);
        }
    }

    for (size_t i = 0; i < b->stringCount; i++) {
        const databox score = DATABOX_SIGNED((int64_t)i);
        const databox *args[] = {&score, &b->strings[i]};
        multiOrderedSetAdd(&mos, args[0], args[1]);
        persistContractRecord(b->log, PERSIST_OP_INSERT, args, 2);
    }
    return mos;
}

static bool pcMosHoldsBytes(const void *structure, const databox *bytes) {
    const multiOrderedSet *mos = structure;
    mosIterator iter;
    multiOrderedSetIteratorInit(mos, &iter, true);
    bool found = false;
    databoxBig member;
    databoxBig score;
    for (;;) {
        DATABOX_BIG_INIT(&member);
        DATABOX_BIG_INIT(&score);
        if (found || !multiOrderedSetIteratorNext(&iter, (databox *)&member,
                                                  (databox *)&score)) {
            break;
        }
        found = persistContractSameBytes((const databox *)&member, bytes);
    }
    multiOrderedSetIteratorRelease(&iter);
    return found;
}

static bool pcMosSameSettings(const void *original, const void *restored) {
    return multiOrderedSetLimit(original) == multiOrderedSetLimit(restored);
}

const persistContractType persistContractMultiOrderedSet = {
    .ops = &persistOpsMultiOrderedSet,
    .build = pcMosBuild,
    .holdsBytes = pcMosHoldsBytes,
    .sameSettings = pcMosSameSettings,
};

#endif /* DATAKIT_TEST */
