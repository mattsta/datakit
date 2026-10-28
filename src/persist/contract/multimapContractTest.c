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

/* One row's elements and the bytes they point into */
typedef struct pcMultimapRow {
    uint8_t bufs[3][64];
    databoxBig boxes[3];
} pcMultimapRow;

static void pcMultimapRowAim(pcMultimapRow *row, const databox *elements[3]) {
    for (uint32_t w = 0; w < 3; w++) {
        elements[w] = (const databox *)&row->boxes[w];
    }
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
    /* ... compressing or not, at a small, the default or a large limit */
    const bool compress = (shape >> 9) & 1;
    static const flexCapSizeLimit limits[] = {
        FLEX_CAP_LEVEL_256, FLEX_CAP_LEVEL_2048, FLEX_CAP_LEVEL_4096};
    const flexCapSizeLimit limit = limits[(shape >> 10) % countof(limits)];
    /* @own: caller; free=multimapFree */
    multimap *m = multimapNewConfigure(width, set, compress, limit);
    persistContractStart(b->log, m);

    /* Two rows: each insert fills the other one, so the last inserted row
     * stays intact for a full-width delete */
    pcMultimapRow rows[2];
    size_t cur = 0;
    bool haveLast = false;

    for (size_t i = 0; i < b->size; i++) {
        pcMultimapRow *row = &rows[cur];
        const uint64_t roll = splitmix64(&rng) % 8;
        if (roll == 0 && i > 0) {
            /* Delete an earlier key */
            const size_t j = (size_t)(splitmix64(&rng) % i);
            pcMultimapKey(b->seed, j, &row->boxes[0], row->bufs[0],
                          sizeof(row->bufs[0]));
            const databox *key[1] = {(const databox *)&row->boxes[0]};
            multimapDelete(&m, key[0]);
            persistContractRecord(b->log, PERSIST_OP_DELETE, key, 1);
        } else if (roll == 2 && haveLast) {
            /* Delete exactly the last inserted row */
            const databox *last[3];
            pcMultimapRowAim(&rows[cur ^ 1], last);
            multimapDeleteFullWidth(&m, last);
            persistContractRecord(b->log, PERSIST_OP_DELETE, last, width);
        } else if (roll == 3 && i > 0 && width > 1 && !set) {
            /* Add a small delta to a field of an earlier key: refused
             * unless that field holds a counter (a set's values order its
             * rows, so an increment there is misuse, never sent) */
            const size_t j = (size_t)(splitmix64(&rng) % i);
            pcMultimapKey(b->seed, j, &row->boxes[0], row->bufs[0],
                          sizeof(row->bufs[0]));
            const uint32_t offset =
                1 + (uint32_t)(splitmix64(&rng) % (width - 1));
            const int64_t delta = (int64_t)(splitmix64(&rng) % 7) - 3;
            const databox offsetBox = DATABOX_UNSIGNED(offset);
            const databox deltaBox = DATABOX_SIGNED(delta);
            const databox *args[3] = {(const databox *)&row->boxes[0],
                                      &offsetBox, &deltaBox};
            multimapFieldIncr(&m, args[0], offset, delta);
            persistContractRecord(b->log, PERSIST_OP_NUM_INCR, args, 3);
        } else {
            /* Insert key i; one in eight re-inserts an earlier key */
            const size_t k =
                (roll == 1 && i > 0) ? (size_t)(splitmix64(&rng) % i) : i;
            pcMultimapKey(b->seed, k, &row->boxes[0], row->bufs[0],
                          sizeof(row->bufs[0]));
            for (uint32_t w = 1; w < width; w++) {
                persistContractValue(&rng, &row->boxes[w], row->bufs[w],
                                     sizeof(row->bufs[w]));
            }
            const databox *elements[3];
            pcMultimapRowAim(row, elements);
            pcMultimapInsert(&m, b, elements, width);
            cur ^= 1;
            haveLast = true;
        }

        if (b->size >= 100 && i == b->size / 3) {
            multimapReset(&m);
            persistContractRecord(b->log, PERSIST_OP_CLEAR, nullptr, 0);
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

static bool pcMultimapSameSettings(const void *original, const void *restored) {
    const multimap *a = original;
    const multimap *b = restored;
    return multimapElementsPerEntry(a) == multimapElementsPerEntry(b) &&
           multimapIsSet(a) == multimapIsSet(b) &&
           multimapCompresses(a) == multimapCompresses(b) &&
           multimapLimit(a) == multimapLimit(b);
}

const persistContractType persistContractMultimap = {
    .ops = &persistOpsMultimap,
    .build = pcMultimapBuild,
    .holdsBytes = pcMultimapHoldsBytes,
    .sameSettings = pcMultimapSameSettings,
};

#endif /* DATAKIT_TEST */
