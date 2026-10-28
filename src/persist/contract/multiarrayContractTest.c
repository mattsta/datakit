/* multiarrayContractTest.c - persistOpsMultiarray in the persistence
 * contract */

#include "persistContract.h"

#ifdef DATAKIT_TEST

#include "../../multiarray.h"
#include "../../str.h"

#include <string.h>

/* 12-byte elements and 8 per node, so builds reach Small, Medium and
 * Large */
static constexpr int PC_MA_WIDTH = 12;
static constexpr int PC_MA_ROWMAX = 8;

static void pcMultiarrayElement(uint64_t *rng, uint8_t out[PC_MA_WIDTH]) {
    const uint64_t a = splitmix64(rng);
    const uint32_t b = (uint32_t)splitmix64(rng);
    memcpy(out, &a, sizeof(a));
    memcpy(out + sizeof(a), &b, sizeof(b));
}

static void *pcMultiarrayBuild(const persistContractBuild *b) {
    /* The row maximum varies by seed, so the snapshot carries it */
    const uint16_t rowMax = b->seed & 1 ? PC_MA_ROWMAX * 4 : PC_MA_ROWMAX;
    /* @own: caller; free=multiarrayFree */
    multiarray *m = multiarrayNew(PC_MA_WIDTH, rowMax);
    persistContractStart(b->log, m);

    uint64_t rng = b->seed;
    uint8_t element[PC_MA_WIDTH];
    for (size_t i = 0; i < b->size; i++) {
        const size_t count = multiarrayCount(m);
        const uint64_t roll = splitmix64(&rng) % 8;
        const uint64_t at = splitmix64(&rng);
        if (roll == 0 && count) {
            const databox idx = DATABOX_UNSIGNED(at % count);
            const databox *args[] = {&idx};
            multiarrayDelete(&m, (uint32_t)(at % count));
            persistContractRecord(b->log, PERSIST_OP_DELETE_AT, args, 1);
        } else if (roll == 1 && count) {
            pcMultiarrayElement(&rng, element);
            const databox idx = DATABOX_UNSIGNED(at % count);
            const databox value = DATABOX_WITH_BYTES(element, PC_MA_WIDTH);
            const databox *args[] = {&idx, &value};
            memcpy(multiarrayGet(m, (multiarrayIdx)(at % count)), element,
                   PC_MA_WIDTH);
            persistContractRecord(b->log, PERSIST_OP_UPDATE, args, 2);
        } else {
            /* Insert anywhere, the end included */
            pcMultiarrayElement(&rng, element);
            const databox idx = DATABOX_UNSIGNED(at % (count + 1));
            const databox value = DATABOX_WITH_BYTES(element, PC_MA_WIDTH);
            const databox *args[] = {&idx, &value};
            multiarrayInsert(&m, (multiarrayIdx)(at % (count + 1)), element);
            persistContractRecord(b->log, PERSIST_OP_INSERT_AT, args, 2);
        }

        if (b->size >= 100 && i == b->size / 3) {
            /* A clear keeps the array's settings, as replay's does */
            multiarrayFree(m);
            /* @own: caller; free=multiarrayFree */
            m = multiarrayNew(PC_MA_WIDTH, rowMax);
            persistContractRecord(b->log, PERSIST_OP_CLEAR, nullptr, 0);
        }
    }
    return m;
}

static bool pcMultiarraySameSettings(const void *original,
                                     const void *restored) {
    return multiarrayElementLen(original) == multiarrayElementLen(restored) &&
           multiarrayRowMax(original) == multiarrayRowMax(restored);
}

const persistContractType persistContractMultiarray = {
    .ops = &persistOpsMultiarray,
    .build = pcMultiarrayBuild,
    .sameSettings = pcMultiarraySameSettings,
};

#endif /* DATAKIT_TEST */
