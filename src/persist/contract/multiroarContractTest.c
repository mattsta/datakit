/* multiroarContractTest.c - persistOpsMultiroar in the persistence contract
 *
 * Even seeds build bit-mode roars, odd seeds value-mode roars (seeded
 * width and dimensions); both log the ops multiroarP writes. */

#include "persistContract.h"

#ifdef DATAKIT_TEST

#include "../../multiroar.h"
#include "../../str.h"

/* A position near one of a few chunks (dense, so chunks move through
 * their sparse, dense and full forms) or anywhere in a wide space */
static uint64_t pcMultiroarPosition(uint64_t *rng) {
    static constexpr uint64_t bases[] = {0, 8192, 3 * 8192, UINT64_C(1) << 40};
    const uint64_t r = splitmix64(rng);
    if (r % 8 == 0) {
        return splitmix64(rng) % (UINT64_C(1) << 44);
    }
    return bases[(r >> 3) % 4] + splitmix64(rng) % 9000;
}

/* Values set and removed at positions inside seeded dimensions, values
 * spanning the seeded width, with a CLEAR partway */
static void *pcMultiroarBuildValues(const persistContractBuild *b) {
    uint64_t rng = b->seed;
    const uint8_t width = (uint8_t)(1 + splitmix64(&rng) % 64);
    const uint64_t rows = splitmix64(&rng) % 3 ? 1 + splitmix64(&rng) % 500 : 0;
    const uint64_t cols = splitmix64(&rng) % 3 ? 1 + splitmix64(&rng) % 500 : 0;
    const uint64_t space = rows && cols ? rows * cols : UINT64_C(1) << 40;
    const uint64_t top = width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;

    /* @own: caller; free=multiroarFree */
    multiroar *r = multiroarValueNew(width, rows, cols);
    persistContractStart(b->log, r);

    for (size_t i = 0; i < b->size; i++) {
        const uint64_t position = splitmix64(&rng) % space;
        if (splitmix64(&rng) % 4) {
            const uint64_t words[2] = {position, splitmix64(&rng) & top};
            const databox value = databoxNewUnsigned(words[1]);
            const multiroarValueSetResult set =
                multiroarValueSet(r, position, (databox *)&value);
            assert(set != MULTIROAR_VALUE_REFUSED); /* in bounds, fits */
            persistContractRecord(b->log, PERSIST_OP_INSERT, words, 2);
        } else {
            multiroarValueRemoveGetRemoved(r, position, nullptr);
            persistContractRecord(b->log, PERSIST_OP_DELETE, &position, 1);
        }

        if (b->size >= 100 && i == b->size / 3) {
            multiroarClear(r);
            persistContractRecord(b->log, PERSIST_OP_CLEAR, nullptr, 0);
        }
    }
    return r;
}

static void *pcMultiroarBuild(const persistContractBuild *b) {
    if (b->seed & 1) {
        return pcMultiroarBuildValues(b);
    }

    /* @own: caller; free=multiroarFree */
    multiroar *r = multiroarBitNew();
    persistContractStart(b->log, r);

    uint64_t rng = b->seed;
    for (size_t i = 0; i < b->size; i++) {
        const uint64_t roll = splitmix64(&rng) % 16;
        const uint64_t pos = pcMultiroarPosition(&rng);
        if (roll < 8) {
            multiroarBitSet(r, pos);
            persistContractRecord(b->log, PERSIST_OP_INSERT, &pos, 1);
        } else if (roll < 12) {
            multiroarBitClear(r, pos);
            persistContractRecord(b->log, PERSIST_OP_DELETE, &pos, 1);
        } else {
            /* Up to just over a chunk, so ranges cover whole chunks and
             * cross chunk edges */
            const uint64_t range[2] = {pos, 1 + splitmix64(&rng) % 9000};
            if (roll == 12) {
                multiroarBitSetRange(r, range[0], range[1]);
                persistContractRecord(b->log, PERSIST_OP_SET_RANGE, range, 2);
            } else if (roll == 13) {
                multiroarBitClearRange(r, range[0], range[1]);
                persistContractRecord(b->log, PERSIST_OP_CLEAR_RANGE, range, 2);
            } else {
                multiroarBitFlipRange(r, range[0], range[1]);
                persistContractRecord(b->log, PERSIST_OP_FLIP_RANGE, range, 2);
            }
        }

        if (b->size >= 100 && i == b->size / 3) {
            multiroarClear(r);
            persistContractRecord(b->log, PERSIST_OP_CLEAR, nullptr, 0);
        }
    }
    return r;
}

const persistContractType persistContractMultiroar = {
    .ops = &persistOpsMultiroar,
    .build = pcMultiroarBuild,
};

#endif /* DATAKIT_TEST */
