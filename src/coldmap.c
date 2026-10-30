#include "coldmap.h"

#include "cleaner.h"

/* One block: a flex of whole rows (compressed or plain), its first key as
 * an owned copy (routing never borrows block memory), and what reading it
 * needs without decompressing first */
typedef struct coldmapBlock {
    mflex *rows;
    databox first;   /* owned: databoxFreeData */
    uint32_t count;  /* rows */
    uint32_t middle; /* byte offset of the middle row in the open flex */
    uint32_t raw;    /* bytes of the open flex */
} coldmapBlock;

struct coldmap {
    coldmapBlock *blocks;
    size_t blockCount;
    size_t blockCap;
    size_t rows;
    uint64_t rawBytes;
    uint64_t storedBytes;
    uint64_t keyBytes; /* heap bytes the routing keys own */
    uint64_t compressedBlocks;
    uint32_t rawCap;       /* bytes */
    uint32_t storedTarget; /* bytes */
    multimapElements elementsPerEntry;
    mflexState *state; /* the coldmap's own, for its writes */
};

/* ====================================================================
 * Blocks
 * ==================================================================== */
static void coldmapBlockAppend_(coldmap *cm, flex *f) {
    if (cm->blockCount == cm->blockCap) {
        cm->blockCap = cm->blockCap ? cm->blockCap * 2 : 8;
        /* @own: container; free=coldmapFree */
        cm->blocks = zrealloc(cm->blocks, cm->blockCap * sizeof(*cm->blocks));
    }

    coldmapBlock *b = &cm->blocks[cm->blockCount++];
    databox first = {};
    flexGetByType(flexHead(f), &first);
    b->first = databoxCopy(&first);
    if (b->first.allocated) {
        cm->keyBytes += b->first.len;
    }

    b->count = (uint32_t)(flexCount(f) / cm->elementsPerEntry);
    b->middle = (uint32_t)(flexMiddle(f, cm->elementsPerEntry) - f);
    b->raw = (uint32_t)flexBytes(f);
    /* @own: container; free=mflexFree */
    b->rows = mflexConvertFromFlex(f, cm->state);

    cm->rows += b->count;
    cm->rawBytes += b->raw;
    cm->storedBytes += mflexBytesActual(b->rows);
    cm->compressedBlocks += mflexIsCompressed(b->rows);
}

static void coldmapBlockRelease_(coldmapBlock *b) {
    mflexFree(b->rows);
    databoxFreeData(&b->first);
}

/* The block whose key range holds 'key': the last block whose first key
 * is at most 'key'; false when 'key' is below every block */
static bool coldmapRoute_(const coldmap *cm, const databox *key,
                          size_t *index) {
    size_t lo = 0;
    size_t hi = cm->blockCount;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (databoxCompare(&cm->blocks[mid].first, key) <= 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    *index = lo - 1;
    return lo > 0;
}

/* 'key's row in its block, opened through 'state' (NULL: no row) */
static flexEntry *coldmapFind_(const coldmap *cm, mflexState *state,
                               const databox *key, const flex **open) {
    size_t at;
    if (!coldmapRoute_(cm, key, &at)) {
        return nullptr;
    }

    const coldmapBlock *b = &cm->blocks[at];
    const flex *f = mflexOpenReadOnly(b->rows, state);
    *open = f;
    return flexFindByTypeSortedWithMiddle(f, cm->elementsPerEntry, key,
                                          (flexEntry *)f + b->middle);
}

/* ====================================================================
 * Building
 * ==================================================================== */
/* Whether 'rows' holds whole rows with strictly ascending keys */
static bool coldmapRowsAscend_(const flex *rows,
                               multimapElements elementsPerEntry) {
    const size_t count = flexCount(rows);
    if (elementsPerEntry == 0 || count % elementsPerEntry) {
        return false;
    }

    databoxBig key[2] = {};
    flexEntry *fe = flexHead(rows);
    for (size_t row = 0; row < count / elementsPerEntry; row++) {
        databoxBig *cur = &key[row & 1];
        flexGetByTypeBig(fe, cur);
        if (row > 0 && databoxCompare((const databox *)&key[(row - 1) & 1],
                                      (const databox *)cur) >= 0) {
            return false;
        }

        for (multimapElements e = 0; e < elementsPerEntry; e++) {
            fe = flexNext(rows, fe);
        }
    }

    return true;
}

/* How many rows, counted back from the tail of 'rows', reach 'cut' bytes
 * (at least one) */
static size_t coldmapTailRows_(const flex *rows,
                               multimapElements elementsPerEntry, size_t cut) {
    const size_t total = flexCount(rows) / elementsPerEntry;
    const flexEntry *tail = flexTail(rows);
    flexEntry *rowStart = flexTail(rows);
    for (multimapElements e = 1; e < elementsPerEntry; e++) {
        rowStart = flexPrev(rows, rowStart);
    }

    size_t take = 1;
    while (take < total && (size_t)(tail - rowStart) < cut) {
        for (multimapElements e = 0; e < elementsPerEntry; e++) {
            rowStart = flexPrev(rows, rowStart);
        }
        take++;
    }

    return take;
}

coldmap *coldmapNewFromDump(multimapElements elementsPerEntry,
                            const coldmapConfig *config, flex *rows) {
    const coldmapConfig defaults = {};
    const coldmapConfig *c = config ? config : &defaults;
    if (c->rawLimit > FLEX_CAP_LEVEL_65536 || c->minSavingPercent > 99 ||
        c->level > 12 || !coldmapRowsAscend_(rows, elementsPerEntry)) {
        flexFree(rows);
        return nullptr;
    }

    /* @own: caller; free=coldmapFree */
    coldmap *cm = zcalloc(1, sizeof(*cm));
    cm->elementsPerEntry = elementsPerEntry;
    cm->rawCap =
        flexOptimizationSizeLimit[c->rawLimit ? c->rawLimit
                                              : COLDMAP_RAW_LIMIT_DEFAULT];
    cm->storedTarget = c->storedTarget ? c->storedTarget : cm->rawCap / 4;
    /* @own: container; free=mflexStateFree */
    cm->state = mflexStateCreate();
    (void)mflexStateSetPolicy(cm->state, c->minSavingPercent, c->level);

    /* Cut blocks tail first (each cut only shortens 'rows'), each sized to
     * store about the target at the ratio the block after it got */
    uint64_t storedPermille = 1000;
    while (flexCount(rows)) {
        const uint64_t want =
            (uint64_t)cm->storedTarget * 1000 / storedPermille;
        const size_t cut = want < cm->rawCap ? (size_t)want : cm->rawCap;
        const size_t entries =
            coldmapTailRows_(rows, elementsPerEntry, cut) * elementsPerEntry;
        flex *block;
        if (entries == flexCount(rows)) {
            block = rows;
            rows = nullptr;
        } else {
            /* @own: container; free=mflexFree */
            block = flexSplitRange(&rows, -(int32_t)entries, (uint32_t)entries);
        }

        const uint64_t stored = cm->storedBytes;
        coldmapBlockAppend_(cm, block);
        const coldmapBlock *b = &cm->blocks[cm->blockCount - 1];
        storedPermille = (cm->storedBytes - stored) * 1000 / b->raw;
        storedPermille = storedPermille ? storedPermille : 1;
        if (!rows) {
            break;
        }
    }
    flexFree(rows);

    /* The cuts came tail first: key order is the reverse */
    for (size_t i = 0; i < cm->blockCount / 2; i++) {
        const coldmapBlock swap = cm->blocks[i];
        cm->blocks[i] = cm->blocks[cm->blockCount - 1 - i];
        cm->blocks[cm->blockCount - 1 - i] = swap;
    }

    return cm;
}

void coldmapFree(coldmap *cm) {
    if (!cm) {
        return;
    }

    for (size_t i = 0; i < cm->blockCount; i++) {
        coldmapBlockRelease_(&cm->blocks[i]);
    }
    zfree(cm->blocks);
    mflexStateFree(cm->state);
    zfree(cm);
}

/* ====================================================================
 * Reading
 * ==================================================================== */
size_t coldmapCount(const coldmap *cm) {
    return cm->rows;
}

size_t coldmapBytes(const coldmap *cm) {
    return sizeof(*cm) + cm->blockCap * sizeof(*cm->blocks) + cm->storedBytes +
           cm->keyBytes;
}

multimapElements coldmapElementsPerEntry(const coldmap *cm) {
    return cm->elementsPerEntry;
}

bool coldmapLookup(const coldmap *cm, mflexState *state, const databox *key,
                   databox *elements[]) {
    const flex *f = nullptr;
    flexEntry *fe = coldmapFind_(cm, state, key, &f);
    if (!fe) {
        return false;
    }

    for (multimapElements i = 1; i < cm->elementsPerEntry; i++) {
        fe = flexNext(f, fe);
        flexGetByType(fe, elements[i - 1]);
    }
    return true;
}

bool coldmapExists(const coldmap *cm, mflexState *state, const databox *key) {
    const flex *f = nullptr;
    return coldmapFind_(cm, state, key, &f) != nullptr;
}

flex *coldmapDump(const coldmap *cm, mflexState *state) {
    /* @own: caller; free=flexFree */
    flex *out = flexNew();
    for (size_t i = 0; i < cm->blockCount; i++) {
        flexBulkAppendFlex(&out, mflexOpenReadOnly(cm->blocks[i].rows, state));
    }
    return out;
}

void coldmapGetStats(const coldmap *cm, coldmapStats *stats) {
    *stats = (coldmapStats){
        .rows = cm->rows,
        .blocks = cm->blockCount,
        .compressedBlocks = cm->compressedBlocks,
        .rawBytes = cm->rawBytes,
        .storedBytes = cm->storedBytes,
    };
}

/* ====================================================================
 * Tests
 * ==================================================================== */
#ifdef DATAKIT_TEST
#include "ctest.h"
#include "multimap.h"

defcleaner(coldmap *, coldmapFree);
defcleaner(flex *, flexFree);
defcleaner(mflexState *, mflexStateFree);
defcleaner(multimap *, multimapFree);

/* Row shapes: record-like rows (sorted keys share prefixes; values repeat
 * structure) and integer rows (already dense in flex) */
typedef enum coldmapShape_ {
    COLDMAP_SHAPE_RECORDS = 0,
    COLDMAP_SHAPE_INTS = 1,
} coldmapShape_;

/* A multimap of 'rows' rows of 'shape': row i has key i * 2 (every odd
 * number is absent) */
static multimap *coldmapTestRows_(coldmapShape_ shape, size_t rows) {
    /* @own: caller; free=multimapFree */
    multimap *m = multimapNew(2);
    char key[32];
    char value[96];
    for (size_t i = 0; i < rows; i++) {
        databox k;
        databox v;
        if (shape == COLDMAP_SHAPE_RECORDS) {
            const int klen = snprintf(key, sizeof(key), "user:%08zu", i * 2);
            const int vlen = snprintf(value, sizeof(value),
                                      "{\"name\":\"user %zu\",\"tier\":\"%s\","
                                      "\"visits\":%zu}",
                                      i * 2, i % 3 ? "gold" : "silver", i % 97);
            k = DATABOX_WITH_BYTES((uint8_t *)key, (size_t)klen);
            v = DATABOX_WITH_BYTES((uint8_t *)value, (size_t)vlen);
        } else {
            k = databoxNewSigned((int64_t)i * 2);
            v = databoxNewSigned((int64_t)(i * i) ^ 0x5bd1e995);
        }
        const databox *row[2] = {&k, &v};
        multimapInsert(&m, row);
    }
    return m;
}

/* The key of row 'i' (even) or of the gap after it (odd) */
static databox coldmapTestKey_(coldmapShape_ shape, size_t i, char *buf,
                               size_t len) {
    if (shape == COLDMAP_SHAPE_RECORDS) {
        const int klen = snprintf(buf, len, "user:%08zu", i);
        return DATABOX_WITH_BYTES((uint8_t *)buf, (size_t)klen);
    }
    return databoxNewSigned((int64_t)i);
}

int coldmapTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    TEST("built from a multimap's rows: every lookup, absence and the dump "
         "agree with it, at every config") {
        static const coldmapConfig configs[] = {
            {},
            {.rawLimit = FLEX_CAP_LEVEL_512, .storedTarget = 128},
            {.rawLimit = FLEX_CAP_LEVEL_65536},
            {.level = 9},
            {.minSavingPercent = 20},
        };
        static const size_t sizes[] = {0, 1, 7, 3000};
        cleaner(mflexStateFree) mflexState *state = mflexStateCreate();
        for (size_t s = 0; s < 2; s++) {
            for (size_t n = 0; n < countof(sizes); n++) {
                cleaner(multimapFree) multimap *m =
                    coldmapTestRows_((coldmapShape_)s, sizes[n]);
                for (size_t c = 0; c < countof(configs); c++) {
                    cleaner(coldmapFree) coldmap *cm =
                        coldmapNewFromDump(2, &configs[c], multimapDump(m));
                    if (!cm || coldmapCount(cm) != sizes[n]) {
                        ERR("shape %zu, %zu rows, config %zu: count", s,
                            sizes[n], c);
                        continue;
                    }

                    char buf[32];
                    for (size_t i = 0; i < sizes[n] * 2 + 1; i++) {
                        const databox key = coldmapTestKey_((coldmapShape_)s, i,
                                                            buf, sizeof(buf));
                        databox want = {};
                        databox got = {};
                        databox *wantRow[1] = {&want};
                        databox *gotRow[1] = {&got};
                        const bool had = multimapLookup(m, &key, wantRow);
                        const bool has = coldmapLookup(cm, state, &key, gotRow);
                        if (had != has ||
                            (has && databoxCompare(&want, &got)) ||
                            has != coldmapExists(cm, state, &key)) {
                            ERR("shape %zu, config %zu: key %zu differs", s, c,
                                i);
                            break;
                        }
                    }

                    cleaner(flexFree) flex *want = multimapDump(m);
                    cleaner(flexFree) flex *got = coldmapDump(cm, state);
                    if (!flexEqual(want, got)) {
                        ERR("shape %zu, config %zu: dump differs", s, c);
                    }
                }
            }
        }
    }

    TEST("records compress and pack to the stored target; integers that do "
         "not save the policy's share stay plain") {
        cleaner(multimapFree) multimap *records =
            coldmapTestRows_(COLDMAP_SHAPE_RECORDS, 20000);
        const coldmapConfig config = {.storedTarget = 1024};
        cleaner(coldmapFree) coldmap *cm =
            coldmapNewFromDump(2, &config, multimapDump(records));
        coldmapStats st;
        coldmapGetStats(cm, &st);
        const uint64_t perBlock = st.storedBytes / st.blocks;
        if (st.storedBytes * 3 > st.rawBytes ||
            st.compressedBlocks != st.blocks || perBlock < 512 ||
            perBlock > 1536) {
            ERR("records: %" PRIu64 " of %" PRIu64 " bytes stored in %" PRIu64
                " blocks (%" PRIu64 " compressed)",
                st.storedBytes, st.rawBytes, st.blocks, st.compressedBlocks);
        }

        cleaner(multimapFree) multimap *ints =
            coldmapTestRows_(COLDMAP_SHAPE_INTS, 20000);
        const coldmapConfig strict = {.minSavingPercent = 20};
        cleaner(coldmapFree) coldmap *plain =
            coldmapNewFromDump(2, &strict, multimapDump(ints));
        coldmapGetStats(plain, &st);
        if (st.compressedBlocks != 0 || st.storedBytes != st.rawBytes) {
            ERR("ints: %" PRIu64 " compressed blocks, %" PRIu64
                " stored of %" PRIu64,
                st.compressedBlocks, st.storedBytes, st.rawBytes);
        }
    }

    TEST("rows out of order, repeated keys, partial rows or a bad config are "
         "refused") {
        const databox one = databoxNewSigned(1);
        const databox two = databoxNewSigned(2);
        const databox *const orders[][4] = {
            {&two, &one, &one, &one}, /* keys 2 then 1 */
            {&one, &one, &one, &two}, /* key 1 twice */
        };
        for (size_t o = 0; o < countof(orders); o++) {
            /* @own: container; free=flexFree */
            flex *rows = flexNew();
            for (size_t e = 0; e < 4; e++) {
                flexPushByTypeExact(&rows, orders[o][e], FLEX_ENDPOINT_TAIL);
            }
            if (coldmapNewFromDump(2, nullptr, rows)) {
                ERR("order %zu taken", o);
            }
        }

        /* @own: container; free=flexFree */
        flex *partial = flexNew();
        flexPushByTypeExact(&partial, &one, FLEX_ENDPOINT_TAIL);
        const coldmapConfig bad = {.minSavingPercent = 100};
        /* @own: container; free=flexFree */
        flex *empty = flexNew();
        if (coldmapNewFromDump(2, nullptr, partial) ||
            coldmapNewFromDump(2, &bad, empty)) {
            ERR("%s", "a partial row or a bad config taken");
        }
    }

    TEST_FINAL_RESULT;
}
#endif
