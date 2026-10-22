/* multimapContractTest.c - persistOpsMultimap in the persistence contract */

#include "persistContract.h"

#ifdef DATAKIT_TEST

#include "../../multimap.h"
#include "../../str.h"

/* The key of insert number `i`: a function of (seed, i), so a later
 * delete can name an earlier key again. */
static void pcMultimapKey(uint64_t seed, size_t i, databoxBig *key,
                          uint8_t *buf, size_t bufLen) {
    uint64_t rng = seed ^ ((uint64_t)i * UINT64_C(0x9E3779B97F4A7C15));
    persistContractValue(&rng, key, buf, bufLen);
}

static void pcMultimapInsert(multimap **m, const persistContractBuild *b,
                             const databox *const *elements, uint32_t width) {
    multimapInsert(m, (const databox **)elements);
    persistContractRecord(b->log, PERSIST_OP_INSERT, elements, width);
}

static void *pcMultimapBuild(const persistContractBuild *b) {
    uint64_t rng = b->seed;
    /* Every shape: maps and sets, one to three elements per entry */
    const uint64_t shape = splitmix64(&rng);
    const uint32_t width = 1 + (uint32_t)(shape % 3);
    const bool set = (shape >> 8) & 1;
    /* @own: caller; free=multimapFree */
    multimap *m = set ? multimapSetNew(width) : multimapNew(width);
    persistContractStart(b->log, m);

    uint8_t bufs[3][64];
    databoxBig boxes[3];
    const databox *elements[3];
    for (uint32_t w = 0; w < 3; w++) {
        elements[w] = (const databox *)&boxes[w];
    }

    for (size_t i = 0; i < b->size; i++) {
        const uint64_t roll = splitmix64(&rng) % 8;
        if (roll == 0 && i > 0) {
            /* Delete an earlier key */
            const size_t j = (size_t)(splitmix64(&rng) % i);
            pcMultimapKey(b->seed, j, &boxes[0], bufs[0], sizeof(bufs[0]));
            multimapDelete(&m, elements[0]);
            persistContractRecord(b->log, PERSIST_OP_DELETE, elements, 1);
        } else {
            /* Insert key i; one in eight re-inserts an earlier key */
            const size_t k =
                (roll == 1 && i > 0) ? (size_t)(splitmix64(&rng) % i) : i;
            pcMultimapKey(b->seed, k, &boxes[0], bufs[0], sizeof(bufs[0]));
            for (uint32_t w = 1; w < width; w++) {
                persistContractValue(&rng, &boxes[w], bufs[w], sizeof(bufs[w]));
            }
            pcMultimapInsert(&m, b, elements, width);
        }

        if (b->size >= 100 && i == b->size / 3) {
            multimapReset(m);
            persistContractRecord(b->log, PERSIST_OP_CLEAR, NULL, 0);
        }
    }

    /* Each string as a key and as every value */
    for (size_t i = 0; i < b->stringCount; i++) {
        const databox *row[3] = {&b->strings[i], &b->strings[i],
                                 &b->strings[i]};
        pcMultimapInsert(&m, b, row, width);
    }
    return m;
}

static bool pcMultimapHoldsBytes(const void *structure, const databox *bytes) {
    const multimap *m = structure;
    multimapIterator iter;
    multimapIteratorInit(m, &iter, true);
    const uint32_t width = iter.elementsPerEntry;
    /* Slots, so 128-bit elements decode into storage this code owns */
    databox row[3];
    databox *ptrs[3] = {&row[0], &row[1], &row[2]};
    databoxWide slots[3];
    while (width <= 3 && multimapIteratorNextWithSlots(&iter, ptrs, slots)) {
        for (uint32_t w = 0; w < width; w++) {
            if (persistContractSameBytes(&row[w], bytes)) {
                return true;
            }
        }
    }
    return false;
}

const persistContractType persistContractMultimap = {
    .ops = &persistOpsMultimap,
    .build = pcMultimapBuild,
    .holdsBytes = pcMultimapHoldsBytes,
};

#endif /* DATAKIT_TEST */
