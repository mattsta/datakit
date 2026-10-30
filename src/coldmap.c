#include "coldmap.h"

#include "cleaner.h"

defcleaner(flex *, flexFree);
defcleanerName(void *, zfree, Void);

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
/* Heap bytes a routing key owns */
static uint64_t coldmapKeyBytes_(const databox *key) {
    return key->allocated ? (key->big ? sizeof(databoxWide) : key->len) : 0;
}

/* Block 'b's numbers out of the totals, before it changes or goes */
static void coldmapBlockUncount_(coldmap *cm, const coldmapBlock *b) {
    cm->rows -= b->count;
    cm->rawBytes -= b->raw;
    cm->storedBytes -= mflexBytesActual(b->rows);
    cm->compressedBlocks -= mflexIsCompressed(b->rows);
    cm->keyBytes -= coldmapKeyBytes_(&b->first);
}

/* Keep 'f' (rows, at least one) as block 'b' and count it: 'f' was opened
 * from b->rows through the coldmap's state ('grew': rows came in, else
 * only went), or is a new flex when b->rows is NULL */
static void coldmapBlockStore_(coldmap *cm, coldmapBlock *b, flex *f,
                               bool grew) {
    databoxFreeData(&b->first);
    databoxBig first = {};
    flexGetByTypeBig(flexHead(f), &first);
    b->first = databoxCopy((const databox *)&first);
    b->count = (uint32_t)(flexCount(f) / cm->elementsPerEntry);
    b->middle = (uint32_t)(flexMiddle(f, cm->elementsPerEntry) - f);
    b->raw = (uint32_t)flexBytes(f);
    if (!b->rows) {
        /* @own: container; free=mflexFree */
        b->rows = mflexConvertFromFlex(f, cm->state);
    } else if (grew) {
        mflexCloseGrow(&b->rows, cm->state, f);
    } else {
        mflexCloseShrink(&b->rows, cm->state, f);
    }

    cm->rows += b->count;
    cm->rawBytes += b->raw;
    cm->storedBytes += mflexBytesActual(b->rows);
    cm->compressedBlocks += mflexIsCompressed(b->rows);
    cm->keyBytes += coldmapKeyBytes_(&b->first);
}

/* A new, empty block at index 'at' (the blocks from 'at' move up one) */
static coldmapBlock *coldmapBlockInsert_(coldmap *cm, size_t at) {
    if (cm->blockCount == cm->blockCap) {
        cm->blockCap = cm->blockCap ? cm->blockCap * 2 : 8;
        /* @own: container; free=coldmapFree */
        cm->blocks = zrealloc(cm->blocks, cm->blockCap * sizeof(*cm->blocks));
    }

    memmove(&cm->blocks[at + 1], &cm->blocks[at],
            (cm->blockCount - at) * sizeof(*cm->blocks));
    cm->blockCount++;
    cm->blocks[at] = (coldmapBlock){};
    return &cm->blocks[at];
}

static void coldmapBlockRelease_(coldmapBlock *b) {
    mflexFree(b->rows);
    databoxFreeData(&b->first);
}

/* Drop block 'at', already counted out of the totals */
static void coldmapBlockDrop_(coldmap *cm, size_t at) {
    coldmapBlockRelease_(&cm->blocks[at]);
    memmove(&cm->blocks[at], &cm->blocks[at + 1],
            (cm->blockCount - at - 1) * sizeof(*cm->blocks));
    cm->blockCount--;
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
        coldmapBlock *b = coldmapBlockInsert_(cm, cm->blockCount);
        coldmapBlockStore_(cm, b, block, true);
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
 * Writing
 * ==================================================================== */
/* Block 'at + 1' appended to block 'at' */
static void coldmapMergeNext_(coldmap *cm, size_t at) {
    coldmapBlock *upperBlock = &cm->blocks[at + 1];
    cleaner(flexFree) flex *upper =
        flexDuplicate(mflexOpenReadOnly(upperBlock->rows, cm->state));
    coldmapBlockUncount_(cm, upperBlock);
    coldmapBlockDrop_(cm, at + 1);

    coldmapBlock *b = &cm->blocks[at];
    coldmapBlockUncount_(cm, b);
    flex *f = mflexOpen(b->rows, cm->state);
    flexBulkAppendFlex(&f, upper);
    coldmapBlockStore_(cm, b, f, true);
}

/* Block 'at' merged with a neighbour when it stores under half the target
 * and the two fit the raw limit together */
static void coldmapMaybeMerge_(coldmap *cm, size_t at) {
    if (at >= cm->blockCount || cm->blockCount < 2 ||
        mflexBytesActual(cm->blocks[at].rows) * 2 >= cm->storedTarget) {
        return;
    }

    const size_t lower = at + 1 < cm->blockCount ? at : at - 1;
    if ((uint64_t)cm->blocks[lower].raw + cm->blocks[lower + 1].raw <=
        cm->rawCap) {
        coldmapMergeNext_(cm, lower);
    }
}

bool coldmapInsert(coldmap *cm, const databox *elements[]) {
    const multimapElements epe = cm->elementsPerEntry;
    if (!cm->blockCount) {
        /* @own: container; free=mflexFree */
        flex *f = flexNew();
        for (multimapElements e = 0; e < epe; e++) {
            flexPushByTypeExact(&f, elements[e], FLEX_ENDPOINT_TAIL);
        }
        coldmapBlockStore_(cm, coldmapBlockInsert_(cm, 0), f, true);
        return false;
    }

    /* Below every key: the first block takes it */
    size_t at;
    if (!coldmapRoute_(cm, elements[0], &at)) {
        at = 0;
    }

    coldmapBlock *b = &cm->blocks[at];
    coldmapBlockUncount_(cm, b);
    flex *f = mflexOpen(b->rows, cm->state);
    flexEntry *middle = f + b->middle;
    const bool replaced =
        flexInsertReplaceByTypeSortedWithMiddleMultiDirectReplacing(
            &f, epe, elements, &middle, true, nullptr);

    /* Past the raw limit: the upper half becomes the next block */
    flex *upper = nullptr;
    if (flexBytes(f) > cm->rawCap && flexCount(f) >= 2 * (size_t)epe) {
        /* @own: container; free=mflexFree */
        upper = flexSplitMiddle(&f, epe, flexMiddle(f, epe));
    }

    coldmapBlockStore_(cm, b, f, true);
    if (upper) {
        coldmapBlockStore_(cm, coldmapBlockInsert_(cm, at + 1), upper, true);
    }
    return replaced;
}

/* Row 'row' of a dump's rows decoded into 'box' (bytes stay in 'rows') */
static flexEntry *coldmapDecodeRow_(const flex *rows, flexEntry *fe,
                                    multimapElements elementsPerEntry,
                                    databoxBig *box) {
    for (multimapElements e = 0; e < elementsPerEntry; e++) {
        flexGetByTypeBig(fe, &box[e]);
        fe = flexNext(rows, fe);
    }
    return fe;
}

bool coldmapInsertMany(coldmap *cm, const flex *rows, size_t *replaced) {
    const multimapElements epe = cm->elementsPerEntry;
    if (!coldmapRowsAscend_(rows, epe)) {
        return false;
    }

    cleanerName(zfree, Void) void *storage =
        zcalloc(epe, sizeof(databoxBig) + sizeof(databox *));
    databoxBig *box = storage;
    const databox **row = (const databox **)(box + epe);
    for (multimapElements e = 0; e < epe; e++) {
        row[e] = (const databox *)&box[e];
    }

    const size_t total = flexCount(rows) / epe;
    flexEntry *fe = total ? flexHead(rows) : nullptr;
    size_t done = 0;
    size_t replacedRows = 0;
    while (done < total) {
        fe = coldmapDecodeRow_(rows, fe, epe, box);
        size_t at;
        if (!cm->blockCount || !coldmapRoute_(cm, row[0], &at)) {
            /* No blocks, or below every key: the single-row path */
            replacedRows += coldmapInsert(cm, row);
            done++;
            continue;
        }

        /* Every following row that routes to this block goes in with it:
         * one decompression and one compression for the batch */
        coldmapBlock *b = &cm->blocks[at];
        const databox *next =
            at + 1 < cm->blockCount ? &cm->blocks[at + 1].first : nullptr;
        coldmapBlockUncount_(cm, b);
        flex *f = mflexOpen(b->rows, cm->state);
        flexEntry *middle = f + b->middle;
        for (;;) {
            replacedRows +=
                flexInsertReplaceByTypeSortedWithMiddleMultiDirectReplacing(
                    &f, epe, row, &middle, true, nullptr);
            done++;
            if (done == total || flexBytes(f) > cm->rawCap) {
                break;
            }

            flexEntry *after = coldmapDecodeRow_(rows, fe, epe, box);
            if (next && databoxCompare(row[0], next) >= 0) {
                break; /* the next block's row: decoded again there */
            }
            fe = after;
        }

        flex *upper = nullptr;
        if (flexBytes(f) > cm->rawCap && flexCount(f) >= 2 * (size_t)epe) {
            /* @own: container; free=mflexFree */
            upper = flexSplitMiddle(&f, epe, flexMiddle(f, epe));
        }
        coldmapBlockStore_(cm, b, f, true);
        if (upper) {
            coldmapBlockStore_(cm, coldmapBlockInsert_(cm, at + 1), upper,
                               true);
        }
    }

    if (replaced) {
        *replaced = replacedRows;
    }
    return true;
}

bool coldmapDelete(coldmap *cm, const databox *key) {
    size_t at;
    if (!coldmapRoute_(cm, key, &at)) {
        return false;
    }

    /* An open that changes nothing needs no close */
    coldmapBlock *b = &cm->blocks[at];
    flex *f = mflexOpen(b->rows, cm->state);
    flexEntry *fe = flexFindByTypeSortedWithMiddle(f, cm->elementsPerEntry, key,
                                                   f + b->middle);
    if (!fe) {
        return false;
    }

    coldmapBlockUncount_(cm, b);
    flexEntry *middle = f + b->middle;
    flexDeleteSortedValueWithMiddle(&f, cm->elementsPerEntry, fe, &middle);
    if (!flexCount(f)) {
        mflexCloseShrink(&b->rows, cm->state, f);
        coldmapBlockDrop_(cm, at);
        return true;
    }

    coldmapBlockStore_(cm, b, f, false);
    coldmapMaybeMerge_(cm, at);
    return true;
}

size_t coldmapDeleteRange(coldmap *cm, const databox *lo, const databox *hi) {
    if (lo && hi && databoxCompare(lo, hi) > 0) {
        return 0;
    }

    size_t at = 0;
    if (lo && !coldmapRoute_(cm, lo, &at)) {
        at = 0;
    }

    const size_t start = at;
    size_t deleted = 0;
    while (at < cm->blockCount) {
        coldmapBlock *b = &cm->blocks[at];
        if (hi && databoxCompare(&b->first, hi) > 0) {
            break;
        }

        /* A block whose keys all lie in the range goes whole, never
         * decompressed: it starts at or after 'lo', and the next block
         * starts at or before 'hi' (or there is no 'hi') */
        const bool fromInside = !lo || databoxCompare(&b->first, lo) >= 0;
        const bool toInside =
            !hi || (at + 1 < cm->blockCount &&
                    databoxCompare(&cm->blocks[at + 1].first, hi) <= 0);
        coldmapBlockUncount_(cm, b);
        if (fromInside && toInside) {
            deleted += b->count;
            coldmapBlockDrop_(cm, at);
            continue;
        }

        flex *f = mflexOpen(b->rows, cm->state);
        deleted += flexDeleteSortedKeyRange(&f, cm->elementsPerEntry, lo, hi,
                                            f + b->middle);
        if (!flexCount(f)) {
            mflexCloseShrink(&b->rows, cm->state, f);
            coldmapBlockDrop_(cm, at);
            continue;
        }

        coldmapBlockStore_(cm, b, f, false);
        at++;
    }

    /* The edges may have shrunk: the upper one first, so the lower keeps
     * its index */
    coldmapMaybeMerge_(cm, start + 1);
    coldmapMaybeMerge_(cm, start);
    return deleted;
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

/* Open block 'block' with the iterator at its first row (forward) or its
 * last row's last element; false past either end */
static bool coldmapIteratorOpen_(coldmapIterator *it, size_t block) {
    if (block >= it->cm->blockCount) {
        it->done = true;
        return false;
    }

    it->block = block;
    it->f = mflexOpenReadOnly(it->cm->blocks[block].rows, it->state);
    it->entry = it->forward ? flexHead(it->f) : flexTail(it->f);
    return true;
}

static void coldmapIteratorStart_(const coldmap *cm, mflexState *state,
                                  coldmapIterator *it, bool forward) {
    *it = (coldmapIterator){
        .cm = cm, .state = state, .forward = forward, .done = true};
}

bool coldmapIteratorInit(const coldmap *cm, mflexState *state,
                         coldmapIterator *it, bool forward) {
    coldmapIteratorStart_(cm, state, it, forward);
    if (!cm->blockCount) {
        return false;
    }

    it->done = false;
    return coldmapIteratorOpen_(it, forward ? 0 : cm->blockCount - 1);
}

bool coldmapIteratorInitAt(const coldmap *cm, mflexState *state,
                           coldmapIterator *it, bool forward,
                           const databox *key) {
    coldmapIteratorStart_(cm, state, it, forward);
    size_t at;
    if (!coldmapRoute_(cm, key, &at)) {
        /* Below every key: forward starts at the first row; reverse has
         * none */
        return forward && coldmapIteratorInit(cm, state, it, true);
    }

    it->done = false;
    coldmapIteratorOpen_(it, at);
    const coldmapBlock *b = &cm->blocks[at];
    flexEntry *row = flexFindByTypeSortedWithMiddleGetEntry(
        it->f, cm->elementsPerEntry, key, (flexEntry *)it->f + b->middle);
    const bool pastAll = row == (flexEntry *)it->f + flexBytes(it->f);
    if (forward) {
        /* Past every row of this block: the next block's first row */
        it->entry = pastAll ? nullptr : row;
        return !pastAll || at + 1 < cm->blockCount;
    }

    /* Reverse: the row at 'key', else the row before the insert position
     * (the block's first key is at most 'key', so there is one) */
    if (pastAll) {
        it->entry = flexTail(it->f);
        return true;
    }

    databox found = {};
    flexGetByType(row, &found);
    if (databoxCompare(&found, key) == 0) {
        for (multimapElements e = 1; e < cm->elementsPerEntry; e++) {
            row = flexNext(it->f, row);
        }
        it->entry = row;
    } else {
        it->entry = flexPrev(it->f, row);
    }
    return true;
}

bool coldmapIteratorNext(coldmapIterator *it, databox *elements[]) {
    if (it->done) {
        return false;
    }

    /* A block is opened when its first row is read, so the last row read
     * from the one before stays valid until now */
    if (!it->entry) {
        const size_t next = it->forward ? it->block + 1 : it->block - 1;
        if ((!it->forward && it->block == 0) ||
            !coldmapIteratorOpen_(it, next)) {
            it->done = true;
            return false;
        }
    }

    const multimapElements epe = it->cm->elementsPerEntry;
    flexEntry *fe = it->entry;
    for (multimapElements i = 0; i < epe; i++) {
        if (it->forward) {
            flexGetByType(fe, elements[i]);
            fe = flexNext(it->f, fe);
        } else {
            flexGetByType(fe, elements[epe - 1 - i]);
            fe = flexPrev(it->f, fe);
        }
    }
    it->entry = fe;
    return true;
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
defcleaner(mflexState *, mflexStateFree);
defcleaner(multimap *, multimapFree);
defcleanerName(databox *, zfree, Boxes);

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

/* Every block holds rows, its routing key is its first row's key, first
 * keys ascend, and the totals are the blocks' sums */
static bool coldmapVerify_(const coldmap *cm, mflexState *state) {
    uint64_t rows = 0;
    uint64_t raw = 0;
    uint64_t stored = 0;
    uint64_t compressed = 0;
    for (size_t i = 0; i < cm->blockCount; i++) {
        const coldmapBlock *b = &cm->blocks[i];
        const flex *f = mflexOpenReadOnly(b->rows, state);
        databoxBig head = {};
        flexGetByTypeBig(flexHead(f), &head);
        if (!b->count || b->count * cm->elementsPerEntry != flexCount(f) ||
            b->raw != flexBytes(f) ||
            (flexEntry *)f + b->middle != flexMiddle(f, cm->elementsPerEntry) ||
            databoxCompare(&b->first, (const databox *)&head) != 0 ||
            (i && databoxCompare(&cm->blocks[i - 1].first, &b->first) >= 0)) {
            return false;
        }
        rows += b->count;
        raw += b->raw;
        stored += mflexBytesActual(b->rows);
        compressed += mflexIsCompressed(b->rows);
    }
    return rows == cm->rows && raw == cm->rawBytes &&
           stored == cm->storedBytes && compressed == cm->compressedBlocks;
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

    TEST("iterators walk the dump's rows in order and reverse, from either "
         "end and from every key and every gap") {
        static const coldmapConfig configs[] = {
            {}, {.rawLimit = FLEX_CAP_LEVEL_512, .storedTarget = 128}};
        cleaner(mflexStateFree) mflexState *state = mflexStateCreate();
        for (size_t s = 0; s < 2; s++) {
            const size_t n = 2000;
            cleaner(multimapFree) multimap *m =
                coldmapTestRows_((coldmapShape_)s, n);
            /* The reference: the dump's rows, decoded in place */
            cleaner(flexFree) flex *ref = multimapDump(m);
            cleanerName(zfree, Boxes) databox *refRows =
                zcalloc(n * 2, sizeof(databox));
            flexEntry *fe = flexHead(ref);
            for (size_t i = 0; i < n * 2; i++) {
                flexGetByType(fe, &refRows[i]);
                fe = flexNext(ref, fe);
            }

            for (size_t c = 0; c < countof(configs); c++) {
                cleaner(coldmapFree) coldmap *cm =
                    coldmapNewFromDump(2, &configs[c], multimapDump(m));
                databox k;
                databox v;
                databox *row[2] = {&k, &v};
                for (int forward = 0; forward < 2; forward++) {
                    coldmapIterator it;
                    size_t seen = 0;
                    coldmapIteratorInit(cm, state, &it, forward);
                    while (coldmapIteratorNext(&it, row)) {
                        const size_t at = forward ? seen : n - 1 - seen;
                        if (seen >= n || databoxCompare(&k, &refRows[at * 2]) ||
                            databoxCompare(&v, &refRows[at * 2 + 1])) {
                            ERR("shape %zu config %zu forward %d: row %zu", s,
                                c, forward, seen);
                            break;
                        }
                        seen++;
                    }
                    if (seen != n) {
                        ERR("shape %zu config %zu forward %d: %zu rows", s, c,
                            forward, seen);
                    }
                }

                char buf[32];
                for (size_t i = 0; i < n * 2 + 2; i++) {
                    const databox key =
                        coldmapTestKey_((coldmapShape_)s, i, buf, sizeof(buf));
                    for (int forward = 0; forward < 2; forward++) {
                        /* Keys are 2 * row: the first row at least i, or the
                         * last at most i */
                        const size_t first = (i + 1) / 2;
                        const size_t last = i / 2 < n ? i / 2 : n - 1;
                        coldmapIterator it;
                        const bool any = coldmapIteratorInitAt(cm, state, &it,
                                                               forward, &key);
                        if (any != (forward ? first < n : true)) {
                            ERR("shape %zu config %zu: at %zu any %d", s, c, i,
                                any);
                        }
                        for (size_t step = 0; step < 3; step++) {
                            const bool has = coldmapIteratorNext(&it, row);
                            const bool want =
                                forward ? first + step < n : step <= last;
                            const size_t at =
                                forward ? first + step : last - step;
                            if (has != want ||
                                (has &&
                                 (databoxCompare(&k, &refRows[at * 2]) ||
                                  databoxCompare(&v, &refRows[at * 2 + 1])))) {
                                ERR("shape %zu config %zu forward %d: at %zu "
                                    "step %zu",
                                    s, c, forward, i, step);
                                break;
                            }
                        }
                    }
                }
            }
        }

        /* Below every key, and an empty coldmap */
        cleaner(multimapFree) multimap *ints =
            coldmapTestRows_(COLDMAP_SHAPE_INTS, 10);
        cleaner(coldmapFree) coldmap *cm =
            coldmapNewFromDump(2, nullptr, multimapDump(ints));
        /* @own: container; free=coldmapFree */
        flex *noRows = flexNew();
        cleaner(coldmapFree) coldmap *empty =
            coldmapNewFromDump(2, nullptr, noRows);
        const databox below = databoxNewSigned(-1);
        coldmapIterator it;
        databox k;
        databox v;
        databox *row[2] = {&k, &v};
        const bool fromBelow =
            coldmapIteratorInitAt(cm, state, &it, true, &below) &&
            coldmapIteratorNext(&it, row) && k.data.i64 == 0;
        if (!fromBelow ||
            coldmapIteratorInitAt(cm, state, &it, false, &below) ||
            coldmapIteratorNext(&it, row) ||
            coldmapIteratorInit(empty, state, &it, true) ||
            coldmapIteratorNext(&it, row)) {
            ERR("%s", "below every key, or an empty coldmap, iterated");
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

    TEST("inserts, deletes and range deletes agree with a multimap, blocks "
         "splitting and merging") {
        static const coldmapConfig configs[] = {
            {}, {.rawLimit = FLEX_CAP_LEVEL_512, .storedTarget = 128}};
        cleaner(mflexStateFree) mflexState *state = mflexStateCreate();
        for (size_t c = 0; c < countof(configs); c++) {
            cleaner(multimapFree) multimap *m =
                coldmapTestRows_(COLDMAP_SHAPE_RECORDS, 300);
            cleaner(coldmapFree) coldmap *cm =
                coldmapNewFromDump(2, &configs[c], multimapDump(m));
            uint64_t rng = 0x243F6A8885A308D3ULL + c;
            char key[32];
            char hiKey[32];
            char value[64];
            size_t splitsSeen = 0;
            for (size_t step = 0; step < 6000; step++) {
                rng ^= rng << 13;
                rng ^= rng >> 7;
                rng ^= rng << 17;
                const size_t id = (rng >> 8) % 1200;
                const uint32_t op = (uint32_t)(rng % 16);
                const size_t blocksBefore = cm->blockCount;
                const databox k = coldmapTestKey_(COLDMAP_SHAPE_RECORDS, id,
                                                  key, sizeof(key));
                if (op < 9) {
                    const int vlen = snprintf(
                        value, sizeof(value),
                        "{\"name\":\"user %zu\",\"step\":%zu}", id, step % 5);
                    const databox v =
                        DATABOX_WITH_BYTES((uint8_t *)value, (size_t)vlen);
                    const databox *row[2] = {&k, &v};
                    const bool had = multimapExists(m, &k);
                    multimapInsert(&m, row);
                    if (coldmapInsert(cm, row) != had) {
                        ERR("config %zu step %zu: insert replaced", c, step);
                    }
                } else if (op < 15) {
                    if (multimapDelete(&m, &k) != coldmapDelete(cm, &k)) {
                        ERR("config %zu step %zu: delete", c, step);
                    }
                } else {
                    /* A range of up to 40 ids, sometimes open at one end */
                    const databox hi = coldmapTestKey_(COLDMAP_SHAPE_RECORDS,
                                                       id + (rng >> 40) % 40,
                                                       hiKey, sizeof(hiKey));
                    const databox *lo = (rng >> 50) % 8 ? &k : nullptr;
                    const databox *top = (rng >> 54) % 8 ? &hi : nullptr;
                    if (lo || top) {
                        const size_t want = multimapDeleteRange(&m, lo, top);
                        if (coldmapDeleteRange(cm, lo, top) != want) {
                            ERR("config %zu step %zu: range", c, step);
                        }
                    }
                }
                splitsSeen += cm->blockCount > blocksBefore;

                if (step % 500 == 499 || coldmapCount(cm) != multimapCount(m)) {
                    cleaner(flexFree) flex *want = multimapDump(m);
                    cleaner(flexFree) flex *got = coldmapDump(cm, state);
                    if (!flexEqual(want, got) || !coldmapVerify_(cm, state)) {
                        ERR("config %zu step %zu: rows or blocks differ", c,
                            step);
                        break;
                    }
                }
            }
            if (c == 1 && !splitsSeen) {
                ERR("%s", "512-byte blocks never split");
            }
        }
    }

    TEST("batches of sorted rows go in as row-at-a-time inserts would, into "
         "empty, sparse and dense coldmaps") {
        static const coldmapConfig configs[] = {
            {}, {.rawLimit = FLEX_CAP_LEVEL_512, .storedTarget = 128}};
        static const size_t starts[] = {0, 3, 400};
        cleaner(mflexStateFree) mflexState *state = mflexStateCreate();
        for (size_t c = 0; c < countof(configs); c++) {
            for (size_t s = 0; s < countof(starts); s++) {
                cleaner(multimapFree) multimap *m =
                    coldmapTestRows_(COLDMAP_SHAPE_RECORDS, starts[s]);
                cleaner(coldmapFree) coldmap *cm =
                    coldmapNewFromDump(2, &configs[c], multimapDump(m));
                uint64_t rng = 0x13198A2E03707344ULL + c * 7 + s;
                char key[32];
                char value[64];
                for (size_t batch = 0; batch < 40; batch++) {
                    /* Ids ascend: a random stride from a random start */
                    cleaner(multimapFree) multimap *rows = multimapNew(2);
                    size_t id = (rng >> 20) % 900;
                    size_t replacedWant = 0;
                    const size_t count = 1 + (rng >> 30) % 120;
                    for (size_t r = 0; r < count; r++) {
                        rng ^= rng << 13;
                        rng ^= rng >> 7;
                        rng ^= rng << 17;
                        id += 1 + (rng >> 33) % 9;
                        const databox k = coldmapTestKey_(COLDMAP_SHAPE_RECORDS,
                                                          id, key, sizeof(key));
                        const int vlen =
                            snprintf(value, sizeof(value),
                                     "{\"batch\":%zu,\"row\":%zu}", batch, r);
                        const databox v =
                            DATABOX_WITH_BYTES((uint8_t *)value, (size_t)vlen);
                        const databox *row[2] = {&k, &v};
                        replacedWant += multimapExists(m, &k);
                        multimapInsert(&m, row);
                        multimapInsert(&rows, row);
                    }

                    cleaner(flexFree) flex *dump = multimapDump(rows);
                    size_t replaced = 0;
                    if (!coldmapInsertMany(cm, dump, &replaced) ||
                        replaced != replacedWant) {
                        ERR("config %zu start %zu batch %zu: %zu replaced, "
                            "want %zu",
                            c, s, batch, replaced, replacedWant);
                    }
                }

                cleaner(flexFree) flex *want = multimapDump(m);
                cleaner(flexFree) flex *got = coldmapDump(cm, state);
                if (!flexEqual(want, got) || !coldmapVerify_(cm, state)) {
                    ERR("config %zu start %zu: rows or blocks differ", c, s);
                }
            }
        }

        /* Rows that do not ascend change nothing */
        cleaner(multimapFree) multimap *m =
            coldmapTestRows_(COLDMAP_SHAPE_INTS, 5);
        cleaner(coldmapFree) coldmap *cm =
            coldmapNewFromDump(2, nullptr, multimapDump(m));
        const databox two = databoxNewSigned(2);
        const databox one = databoxNewSigned(1);
        cleaner(flexFree) flex *bad = flexNew();
        flexPushByTypeExact(&bad, &two, FLEX_ENDPOINT_TAIL);
        flexPushByTypeExact(&bad, &two, FLEX_ENDPOINT_TAIL);
        flexPushByTypeExact(&bad, &one, FLEX_ENDPOINT_TAIL);
        flexPushByTypeExact(&bad, &one, FLEX_ENDPOINT_TAIL);
        if (coldmapInsertMany(cm, bad, nullptr) || coldmapCount(cm) != 5) {
            ERR("%s", "rows out of order were taken");
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
