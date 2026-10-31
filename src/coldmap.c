#include "coldmap.h"

#include "cleaner.h"
#include "dkTierLimit.h"
#include "persist.h"

defcleaner(mflexState *, mflexStateFree);

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
    coldmapConfig config;  /* as made, defaults resolved */
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

/* Block 'b's routing key and reading numbers, from 'f': its rows (at least
 * one), open */
static void coldmapBlockDescribe_(const coldmap *cm, coldmapBlock *b,
                                  const flex *f) {
    databoxFreeData(&b->first);
    databoxBig first = {};
    flexGetByTypeBig(flexHead(f), &first);
    b->first = databoxCopy((const databox *)&first);
    b->count = (uint32_t)(flexCount(f) / cm->elementsPerEntry);
    b->middle = (uint32_t)(flexMiddle(f, cm->elementsPerEntry) - f);
    b->raw = (uint32_t)flexBytes(f);
}

/* Block 'b's numbers into the totals, with b->rows as it is kept */
static void coldmapBlockCount_(coldmap *cm, const coldmapBlock *b) {
    cm->rows += b->count;
    cm->rawBytes += b->raw;
    cm->storedBytes += mflexBytesActual(b->rows);
    cm->compressedBlocks += mflexIsCompressed(b->rows);
    cm->keyBytes += coldmapKeyBytes_(&b->first);
}

/* Keep 'f' (rows, at least one) as block 'b' and count it: 'f' was opened
 * from b->rows through the coldmap's state ('grew': rows came in, else
 * only went), or is a new flex when b->rows is NULL */
static void coldmapBlockStore_(coldmap *cm, coldmapBlock *b, flex *f,
                               bool grew) {
    coldmapBlockDescribe_(cm, b, f);
    if (!b->rows) {
        /* @own: container; free=mflexFree */
        b->rows = mflexConvertFromFlex(f, cm->state);
    } else if (grew) {
        mflexCloseGrow(&b->rows, cm->state, f);
    } else {
        mflexCloseShrink(&b->rows, cm->state, f);
    }
    coldmapBlockCount_(cm, b);
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

/* The first entry (the key) of the last row of 'rows' (at least one) */
static flexEntry *coldmapLastRow_(const flex *rows,
                                  multimapElements elementsPerEntry) {
    flexEntry *rowStart = flexTail(rows);
    for (multimapElements e = 1; e < elementsPerEntry; e++) {
        rowStart = flexPrev(rows, rowStart);
    }
    return rowStart;
}

/* How many rows, counted back from the tail of 'rows', reach 'cut' bytes
 * (at least one) */
static size_t coldmapTailRows_(const flex *rows,
                               multimapElements elementsPerEntry, size_t cut) {
    const size_t total = flexCount(rows) / elementsPerEntry;
    const flexEntry *tail = flexTail(rows);
    flexEntry *rowStart = coldmapLastRow_(rows, elementsPerEntry);

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
    if (elementsPerEntry > MULTIMAP_ELEMENTS_MAX ||
        c->rawLimit > FLEX_CAP_LEVEL_65536 ||
        c->minSavingPercent > MFLEX_SAVING_PERCENT_MAX ||
        c->level > CFLEX_LEVEL_MAX ||
        !coldmapRowsAscend_(rows, elementsPerEntry)) {
        flexFree(rows);
        return nullptr;
    }

    /* @own: caller; free=coldmapFree */
    coldmap *cm = zcalloc(1, sizeof(*cm));
    cm->elementsPerEntry = elementsPerEntry;
    cm->config = *c;
    cm->config.rawLimit =
        c->rawLimit ? c->rawLimit : dkTierDefaultLimit(DK_TIER_COLDMAP);
    cm->rawCap = flexOptimizationSizeLimit[cm->config.rawLimit];
    cm->config.storedTarget =
        c->storedTarget ? c->storedTarget : cm->rawCap / 4;
    cm->storedTarget = cm->config.storedTarget;
    /* @own: container; free=mflexStateFree */
    cm->state = mflexStateNew(cm->rawCap); /* sized to one block */
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

/* 'f' stored as block 'at' (in place, holding nothing) and, while a
 * piece of two or more rows is past the raw limit, split at its middle
 * into blocks after it; returns the blocks it took */
static size_t coldmapStoreFitting_(coldmap *cm, size_t at, flex *f) {
    const multimapElements epe = cm->elementsPerEntry;
    flex *upper = nullptr;
    if (flexBytes(f) > cm->rawCap && flexCount(f) >= 2 * (size_t)epe) {
        /* @own: container; free=mflexFree */
        upper = flexSplitMiddle(&f, epe, flexMiddle(f, epe));
    }
    if (!upper) {
        coldmapBlockStore_(cm, &cm->blocks[at], f, true);
        return 1;
    }
    const size_t low = coldmapStoreFitting_(cm, at, f);
    (void)coldmapBlockInsert_(cm, at + low);
    return low + coldmapStoreFitting_(cm, at + low, upper);
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

bool coldmapDeleteMany(coldmap *cm, const flex *keys, size_t *deleted) {
    if (!coldmapRowsAscend_(keys, 1)) {
        return false;
    }

    const multimapElements epe = cm->elementsPerEntry;
    const size_t total = flexCount(keys);
    flexEntry *fe = total ? flexHead(keys) : nullptr;
    size_t done = 0;
    size_t gone = 0;
    databoxBig key = {};
    while (done < total) {
        fe = coldmapDecodeRow_(keys, fe, 1, &key);
        size_t at;
        if (!coldmapRoute_(cm, (const databox *)&key, &at)) {
            done++; /* below every key: no row */
            continue;
        }

        /* Every following key routed to this block goes with it: one
         * decompression and one compression for the batch */
        coldmapBlock *b = &cm->blocks[at];
        const databox *next =
            at + 1 < cm->blockCount ? &cm->blocks[at + 1].first : nullptr;
        coldmapBlockUncount_(cm, b);
        flex *f = mflexOpen(b->rows, cm->state);
        flexEntry *middle = f + b->middle;
        for (;;) {
            flexEntry *row = flexFindByTypeSortedWithMiddle(
                f, epe, (const databox *)&key, middle);
            if (row) {
                flexDeleteSortedValueWithMiddle(&f, epe, row, &middle);
                gone++;
            }
            done++;
            if (done == total || !flexCount(f)) {
                break;
            }

            flexEntry *after = coldmapDecodeRow_(keys, fe, 1, &key);
            if (next && databoxCompare((const databox *)&key, next) >= 0) {
                break; /* the next block's key: decoded again there */
            }
            fe = after;
        }

        if (!flexCount(f)) {
            mflexCloseShrink(&b->rows, cm->state, f);
            coldmapBlockDrop_(cm, at);
            continue;
        }
        coldmapBlockStore_(cm, b, f, false);
        coldmapMaybeMerge_(cm, at);
    }

    if (deleted) {
        *deleted = gone;
    }
    return true;
}

size_t coldmapSweepBlock(coldmap *cm, size_t index, coldmapSweepFn *fn,
                         void *ctx, flex **removed, size_t *next) {
    *next = index + 1 < cm->blockCount ? index + 1 : 0;
    if (index >= cm->blockCount) {
        *next = 0;
        return 0;
    }

    const multimapElements epe = cm->elementsPerEntry;
    cleanerName(zfree, Void) void *storage =
        zcalloc(epe, sizeof(databoxBig) + sizeof(databox *));
    databoxBig *box = storage;
    const databox **row = (const databox **)(box + epe);
    for (multimapElements e = 0; e < epe; e++) {
        row[e] = (const databox *)&box[e];
    }

    /* The block rebuilt from what is kept and changed, unless every row
     * is kept as it is */
    coldmapBlock *b = &cm->blocks[index];
    const flex *f = mflexOpenReadOnly(b->rows, cm->state);
    /* @own: container; free=flexFree */
    flex *kept = flexNew();
    bool changed = false;
    size_t gone = 0;
    flexEntry *fe = flexHead(f);
    for (uint32_t r = 0; r < b->count; r++) {
        fe = coldmapDecodeRow_(f, fe, epe, box);
        databoxBig last;
        DATABOX_BIG_INIT(&last);
        const coldmapSweepAction action = fn(ctx, row, (databox *)&last);
        if (action == COLDMAP_SWEEP_REMOVE) {
            if (removed) {
                flexAppendMultipleExact(removed, epe, row);
            }
            gone++;
            changed = true;
            continue;
        }
        if (action == COLDMAP_SWEEP_CHANGE) {
            row[epe - 1] = (const databox *)&last;
            changed = true;
        }
        flexAppendMultipleExact(&kept, epe, row);
        row[epe - 1] = (const databox *)&box[epe - 1];
    }

    if (!changed) {
        flexFree(kept);
        return 0;
    }

    coldmapBlockUncount_(cm, b);
    if (!flexCount(kept)) {
        flexFree(kept);
        coldmapBlockDrop_(cm, index);
        /* the next block moved into this place (0 past the last) */
        *next = index < cm->blockCount ? index : 0;
        return gone;
    }
    coldmapBlockRelease_(b);
    *b = (coldmapBlock){};
    /* Changed values that grew it past the raw limit split it; the next
     * block to sweep is past every piece */
    const size_t took = coldmapStoreFitting_(cm, index, kept);
    b = &cm->blocks[index];
    /* A small block merges into the one before it (already swept), never
     * the one after: a sweep skips no row and passes none twice */
    if (took == 1 && index &&
        mflexBytesActual(b->rows) * 2 < cm->storedTarget &&
        (uint64_t)cm->blocks[index - 1].raw + b->raw <= cm->rawCap) {
        coldmapMergeNext_(cm, index - 1);
        *next = index < cm->blockCount ? index : 0;
        return gone;
    }
    *next = index + took < cm->blockCount ? index + took : 0;
    return gone;
}

size_t coldmapRemoveIf(coldmap *cm, coldmapRowFn *remove, void *ctx,
                       flex **removed) {
    const multimapElements epe = cm->elementsPerEntry;
    cleanerName(zfree, Void) void *storage =
        zcalloc(epe, sizeof(databoxBig) + sizeof(databox *));
    databoxBig *box = storage;
    const databox **row = (const databox **)(box + epe);
    for (multimapElements e = 0; e < epe; e++) {
        row[e] = (const databox *)&box[e];
    }

    size_t gone = 0;
    size_t at = 0;
    while (at < cm->blockCount) {
        coldmapBlock *b = &cm->blocks[at];
        const flex *f = mflexOpenReadOnly(b->rows, cm->state);

        /* The rows kept, rebuilt only once a row goes: a block with none
         * to remove is left as it is */
        /* @own: container; free=flexFree */
        flex *kept = nullptr;
        flexEntry *fe = flexHead(f);
        for (uint32_t r = 0; r < b->count; r++) {
            flexEntry *rowStart = fe;
            fe = coldmapDecodeRow_(f, fe, epe, box);
            if (remove(ctx, row)) {
                if (!kept) {
                    /* @own: container; free=flexFree */
                    kept = flexNew();
                    flexEntry *k = flexHead(f);
                    while (k != rowStart) {
                        databoxBig copy[1] = {};
                        flexGetByTypeBig(k, &copy[0]);
                        flexPushByTypeExact(&kept, (const databox *)&copy[0],
                                            FLEX_ENDPOINT_TAIL);
                        k = flexNext(f, k);
                    }
                }
                if (removed) {
                    flexAppendMultipleExact(removed, epe, row);
                }
                gone++;
            } else if (kept) {
                flexAppendMultipleExact(&kept, epe, row);
            }
        }

        if (!kept) {
            at++;
            continue;
        }

        /* The block replaced by its kept rows (none: the block goes) */
        coldmapBlockUncount_(cm, b);
        if (!flexCount(kept)) {
            flexFree(kept);
            coldmapBlockDrop_(cm, at);
            continue;
        }
        coldmapBlockRelease_(b);
        *b = (coldmapBlock){};
        coldmapBlockStore_(cm, b, kept, true);
        at++;
    }

    /* Shrunken neighbours merge, from the top so lower indices hold */
    for (size_t i = cm->blockCount; i-- > 0;) {
        coldmapMaybeMerge_(cm, i);
    }
    return gone;
}

flex *coldmapRemoveBlock(coldmap *cm, size_t index) {
    if (index >= cm->blockCount) {
        return nullptr;
    }

    coldmapBlock *b = &cm->blocks[index];
    /* @own: caller; free=flexFree */
    flex *rows = flexDuplicate(mflexOpenReadOnly(b->rows, cm->state));
    coldmapBlockUncount_(cm, b);
    coldmapBlockDrop_(cm, index);
    return rows;
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
size_t coldmapBlockCount(const coldmap *cm) {
    return cm->blockCount;
}

size_t coldmapBlockFor(const coldmap *cm, const databox *key) {
    size_t at;
    return coldmapRoute_(cm, key, &at) ? at : 0;
}

flex *coldmapBlockFirstKey(const coldmap *cm, size_t index) {
    if (index >= cm->blockCount) {
        return nullptr;
    }
    /* @own: caller; free=flexFree */
    flex *key = flexNew();
    flexPushByTypeExact(&key, &cm->blocks[index].first, FLEX_ENDPOINT_TAIL);
    return key;
}

size_t coldmapCount(const coldmap *cm) {
    return cm->rows;
}

size_t coldmapBytes(const coldmap *cm) {
    return sizeof(*cm) + cm->blockCap * sizeof(*cm->blocks) + cm->storedBytes +
           cm->keyBytes + mflexStateBytes(cm->state);
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

    databoxBig found = {}; /* a 96/128-bit key compares whole */
    flexGetByTypeBig(row, &found);
    if (databoxCompare((const databox *)&found, key) == 0) {
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
        .rawLimitBytes = cm->rawCap,
        .storedTargetBytes = cm->storedTarget,
    };
}

coldmap *coldmapNew(multimapElements elementsPerEntry,
                    const coldmapConfig *config) {
    /* @own: container; free=coldmapFree */
    flex *rows = flexNew();
    /* @own: caller; free=coldmapFree */
    return coldmapNewFromDump(elementsPerEntry, config, rows);
}

void coldmapGetConfig(const coldmap *cm, coldmapConfig *config) {
    *config = cm->config;
}

bool coldmapRowsAscend(const flex *rows, multimapElements elementsPerEntry) {
    return coldmapRowsAscend_(rows, elementsPerEntry);
}

/* ====================================================================
 * Persistence (persistOpsColdmap; the format is described in persist.h)
 * ==================================================================== */
/* The unsigneds before the blocks, in order */
typedef enum coldmapPersistField_ {
    CM_PF_ELEMENTS, /* elementsPerEntry */
    CM_PF_RAW_LIMIT,
    CM_PF_STORED_TARGET,
    CM_PF_MIN_SAVING,
    CM_PF_LEVEL,
    CM_PF_BLOCKS,
    CM_PF_COUNT
} coldmapPersistField_;
/* Elements per block: compressed (0 or 1), the bytes as kept */
static constexpr uint32_t COLDMAP_PERSIST_BLOCK = 2;

static uint8_t *coldmapPersistSnapshot_(const void *structure, size_t *len) {
    const coldmap *cm = structure;
    const coldmapConfig *c = &cm->config;
    const databox fields[CM_PF_COUNT] = {
        [CM_PF_ELEMENTS] = DATABOX_UNSIGNED(cm->elementsPerEntry),
        [CM_PF_RAW_LIMIT] = DATABOX_UNSIGNED(c->rawLimit),
        [CM_PF_STORED_TARGET] = DATABOX_UNSIGNED(c->storedTarget),
        [CM_PF_MIN_SAVING] = DATABOX_UNSIGNED(c->minSavingPercent),
        [CM_PF_LEVEL] = DATABOX_UNSIGNED(c->level),
        [CM_PF_BLOCKS] = DATABOX_UNSIGNED(cm->blockCount),
    };
    const databox *header[CM_PF_COUNT];
    for (uint32_t i = 0; i < CM_PF_COUNT; i++) {
        header[i] = &fields[i];
    }

    /* @own: caller; free=zfree */
    flex *f = flexNew();
    flexAppendMultipleExact(&f, CM_PF_COUNT, header);
    for (size_t i = 0; i < cm->blockCount; i++) {
        const mflex *rows = cm->blocks[i].rows;
        size_t bytes;
        const void *stored = mflexStoredBytes(rows, &bytes);
        const databox block[COLDMAP_PERSIST_BLOCK] = {
            DATABOX_UNSIGNED(mflexIsCompressed(rows)),
            DATABOX_WITH_BYTES((uint8_t *)stored, bytes),
        };
        const databox *entries[COLDMAP_PERSIST_BLOCK] = {&block[0], &block[1]};
        flexAppendMultipleExact(&f, COLDMAP_PERSIST_BLOCK, entries);
    }
    *len = flexBytes(f);
    return (uint8_t *)f;
}

/* The snapshot's blocks, from 'fe' on, appended to 'cm' (empty) as they
 * were kept: each opens whole (mflexOpenChecked) into whole rows, at
 * least one, whose keys ascend from past the block before's last key */
static bool coldmapPersistRestoreBlocks_(coldmap *cm, const flex *f,
                                         flexEntry *fe, uint64_t blocks) {
    const multimapElements epe = cm->elementsPerEntry;
    databox last = {}; /* owned: the block before's last key */
    bool ok = true;
    for (uint64_t i = 0; i < blocks; i++) {
        uint64_t compressed;
        databox stored = {};
        if (!flexReadUnsigned(f, &fe, &compressed) || compressed > 1) {
            ok = false;
            break;
        }
        flexGetByType(fe, &stored);
        fe = flexNext(f, fe);
        /* @own: container; free=coldmapFree */
        mflex *m = DATABOX_IS_BYTES(&stored)
                       ? mflexNewFromStored(databoxBytes(&stored), stored.len,
                                            compressed)
                       : nullptr;
        if (!m) {
            ok = false;
            break;
        }

        coldmapBlock *b = coldmapBlockInsert_(cm, cm->blockCount);
        b->rows = m;
        const flex *rows = mflexOpenChecked(m, cm->state);
        if (!rows || flexCount(rows) < epe || flexBytes(rows) > UINT32_MAX ||
            !coldmapRowsAscend_(rows, epe)) {
            ok = false;
            break;
        }

        coldmapBlockDescribe_(cm, b, rows);
        if (i && databoxCompare(&last, &b->first) >= 0) {
            ok = false;
            break;
        }
        coldmapBlockCount_(cm, b);
        databoxBig tail = {};
        flexGetByTypeBig(coldmapLastRow_(rows, epe), &tail);
        databoxFreeData(&last);
        last = databoxCopy((const databox *)&tail);
    }

    databoxFreeData(&last);
    return ok;
}

static void *coldmapPersistRestore_(const uint8_t *data, size_t len) {
    if (!data || !flexValidateBufferDeep(data, len)) {
        return nullptr;
    }

    const flex *f = (const flex *)data;
    const size_t count = flexCount(f);
    flexEntry *fe = flexHead(f);
    uint64_t v[CM_PF_COUNT];
    if (count < CM_PF_COUNT) {
        return nullptr;
    }
    for (uint32_t i = 0; i < CM_PF_COUNT; i++) {
        if (!flexReadUnsigned(f, &fe, &v[i])) {
            return nullptr;
        }
    }

    /* Here: each field fits its type and holds a resolved value (never a
     * 0 that means "the default"), and the blocks are whole.  coldmapNew
     * checks the ranges. */
    const size_t blockEntries = count - CM_PF_COUNT;
    if (!v[CM_PF_ELEMENTS] || v[CM_PF_ELEMENTS] > UINT32_MAX ||
        !v[CM_PF_RAW_LIMIT] || v[CM_PF_RAW_LIMIT] > UINT8_MAX ||
        !v[CM_PF_STORED_TARGET] || v[CM_PF_STORED_TARGET] > UINT32_MAX ||
        v[CM_PF_MIN_SAVING] > UINT8_MAX || v[CM_PF_LEVEL] > UINT8_MAX ||
        blockEntries % COLDMAP_PERSIST_BLOCK ||
        v[CM_PF_BLOCKS] != blockEntries / COLDMAP_PERSIST_BLOCK) {
        return nullptr;
    }

    const coldmapConfig config = {
        .rawLimit = (flexCapSizeLimit)v[CM_PF_RAW_LIMIT],
        .storedTarget = (uint32_t)v[CM_PF_STORED_TARGET],
        .minSavingPercent = (uint8_t)v[CM_PF_MIN_SAVING],
        .level = (cflexLevel)v[CM_PF_LEVEL]};
    /* @own: caller; free=coldmapFree */
    coldmap *cm = coldmapNew((multimapElements)v[CM_PF_ELEMENTS], &config);
    if (cm && !coldmapPersistRestoreBlocks_(cm, f, fe, v[CM_PF_BLOCKS])) {
        coldmapFree(cm);
        return nullptr;
    }
    return cm;
}

/* BULK_DELETE's body: [which bounds are given (1 lo, 2 hi), lo?, hi?] */
static constexpr uint64_t COLDMAP_PERSIST_LO = 1;
static constexpr uint64_t COLDMAP_PERSIST_HI = 2;

static uint8_t *coldmapPersistEncodeOp_(persistOp op, const void *args,
                                        size_t argc, size_t *len) {
    *len = 0;
    switch (op) {
    case PERSIST_OP_INSERT:
        return argc >= 1 ? persistOpEncodeBoxes(args, argc, len) : nullptr;
    case PERSIST_OP_DELETE:
        /* one key, or a sorted batch (coldmapDeleteMany) */
        return argc >= 1 ? persistOpEncodeBoxes(args, argc, len) : nullptr;
    case PERSIST_OP_BULK_INSERT: {
        /* args: the rows flex itself */
        const flex *rows = args;
        if (argc != 1 || !rows) {
            return nullptr;
        }
        /* @own: caller; free=zfree */
        uint8_t *body = zmalloc(flexBytes(rows));
        memcpy(body, rows, flexBytes(rows));
        *len = flexBytes(rows);
        return body;
    }
    case PERSIST_OP_BULK_DELETE: {
        /* args: const databox *bounds[2], either may be NULL */
        const databox *const *bounds = args;
        if (argc != 2 || !bounds) {
            return nullptr;
        }
        const databox which =
            DATABOX_UNSIGNED((bounds[0] ? COLDMAP_PERSIST_LO : 0) |
                             (bounds[1] ? COLDMAP_PERSIST_HI : 0));
        const databox *body[3] = {&which};
        size_t n = 1;
        for (size_t i = 0; i < 2; i++) {
            if (bounds[i]) {
                body[n++] = bounds[i];
            }
        }
        return persistOpEncodeBoxes(body, n, len);
    }
    default:
        return nullptr;
    }
}

/* A WAL body's elements (from untrusted bytes) decoded into 'box' */
static bool coldmapPersistDecode_(const uint8_t *data, size_t len,
                                  databoxBig *box, const databox **ptrs,
                                  size_t want) {
    if (!flexValidateBufferDeep(data, len) ||
        flexCount((const flex *)data) != want) {
        return false;
    }

    coldmapDecodeRow_((const flex *)data, flexHead((const flex *)data),
                      (multimapElements)want, box);
    for (size_t i = 0; i < want; i++) {
        ptrs[i] = (const databox *)&box[i];
    }
    return true;
}

static bool coldmapPersistApplyOp_(void *structure, persistOp op,
                                   const uint8_t *data, size_t len) {
    coldmap *cm = *(coldmap **)structure;
    const multimapElements epe = cm->elementsPerEntry;
    switch (op) {
    case PERSIST_OP_DELETE:
        /* A batch of keys: as coldmapDeleteMany takes them (ascending) */
        if (flexValidateBufferDeep(data, len) &&
            flexCount((const flex *)data) > 1) {
            return coldmapDeleteMany(cm, (const flex *)data, nullptr);
        }
        [[fallthrough]];
    case PERSIST_OP_INSERT: {
        const size_t want = op == PERSIST_OP_INSERT ? epe : 1;
        cleanerName(zfree, Void) void *storage =
            zcalloc(want, sizeof(databoxBig) + sizeof(databox *));
        databoxBig *box = storage;
        const databox **ptrs = (const databox **)(box + want);
        if (!coldmapPersistDecode_(data, len, box, ptrs, want)) {
            return false;
        }
        /* A delete of an absent key was refused where it was logged too:
         * replaying it changes nothing */
        if (op == PERSIST_OP_INSERT) {
            coldmapInsert(cm, ptrs);
        } else {
            coldmapDelete(cm, ptrs[0]);
        }
        return true;
    }
    case PERSIST_OP_BULK_INSERT:
        return flexValidateBufferDeep(data, len) &&
               coldmapInsertMany(cm, (const flex *)data, nullptr);
    case PERSIST_OP_BULK_DELETE: {
        if (!flexValidateBufferDeep(data, len)) {
            return false;
        }
        const flex *f = (const flex *)data;
        flexEntry *fe = flexHead(f);
        uint64_t which;
        if (!flexCount(f) || !flexReadUnsigned(f, &fe, &which) ||
            which > (COLDMAP_PERSIST_LO | COLDMAP_PERSIST_HI) ||
            flexCount(f) != 1 + (size_t)stdc_count_ones(which)) {
            return false;
        }
        databoxBig bound[2] = {};
        const databox *lo = nullptr;
        const databox *hi = nullptr;
        if (which & COLDMAP_PERSIST_LO) {
            flexGetByTypeBig(fe, &bound[0]);
            lo = (const databox *)&bound[0];
            fe = flexNext(f, fe);
        }
        if (which & COLDMAP_PERSIST_HI) {
            flexGetByTypeBig(fe, &bound[1]);
            hi = (const databox *)&bound[1];
        }
        coldmapDeleteRange(cm, lo, hi);
        return true;
    }
    default:
        return false;
    }
}

static size_t coldmapPersistCount_(const void *structure) {
    return coldmapCount(structure);
}

static size_t coldmapPersistBytes_(const void *structure) {
    return coldmapBytes(structure);
}

static void coldmapPersistFree_(void *structure) {
    coldmapFree(structure);
}

/* The row width is fixed at creation, so there is no newEmpty */
const persistOps persistOpsColdmap = {
    .type = PERSIST_TYPE_COLDMAP,
    .name = "coldmap",
    .formatVersion = 1,
    .minReadVersion = 1,
    .snapshot = coldmapPersistSnapshot_,
    .restore = coldmapPersistRestore_,
    .count = coldmapPersistCount_,
    .estimateSize = coldmapPersistBytes_,
    .encodeOp = coldmapPersistEncodeOp_,
    .applyOp = coldmapPersistApplyOp_,
    .free = coldmapPersistFree_,
};

/* ====================================================================
 * Tests
 * ==================================================================== */
#ifdef DATAKIT_TEST
#include "ctest.h"
#include "multimap.h"

defcleaner(coldmap *, coldmapFree);
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
        databox k = {};
        databox v = {};
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

/* Whether 'a' and 'b' keep the same blocks: the same bytes, compressed
 * alike, the same reading numbers, and the same totals */
static bool coldmapSameBlocks_(const coldmap *a, const coldmap *b) {
    if (a->blockCount != b->blockCount || a->rows != b->rows ||
        a->rawBytes != b->rawBytes || a->storedBytes != b->storedBytes ||
        a->keyBytes != b->keyBytes ||
        a->compressedBlocks != b->compressedBlocks) {
        return false;
    }

    for (size_t i = 0; i < a->blockCount; i++) {
        const coldmapBlock *x = &a->blocks[i];
        const coldmapBlock *y = &b->blocks[i];
        size_t xLen;
        size_t yLen;
        const void *xBytes = mflexStoredBytes(x->rows, &xLen);
        const void *yBytes = mflexStoredBytes(y->rows, &yLen);
        if (xLen != yLen || memcmp(xBytes, yBytes, xLen) ||
            mflexIsCompressed(x->rows) != mflexIsCompressed(y->rows) ||
            x->count != y->count || x->middle != y->middle ||
            x->raw != y->raw || databoxCompare(&x->first, &y->first)) {
            return false;
        }
    }
    return true;
}

/* A copy of snapshot 'snap' (a flex) with its first two blocks' entries
 * in swapped order when 'swap', else in order */
static flex *coldmapTestReorder_(const flex *snap, bool swap) {
    /* @own: caller; free=flexFree */
    flex *out = flexNew();
    const size_t first = CM_PF_COUNT;
    const size_t pair = 2 * COLDMAP_PERSIST_BLOCK;
    for (size_t n = 0; n < flexCount(snap); n++) {
        size_t from = n;
        if (swap && n >= first && n < first + pair) {
            from = n < first + COLDMAP_PERSIST_BLOCK
                       ? n + COLDMAP_PERSIST_BLOCK
                       : n - COLDMAP_PERSIST_BLOCK;
        }
        databox box = {};
        flexGetByType(flexIndex(snap, (int32_t)from), &box);
        flexPushByTypeExact(&out, &box, FLEX_ENDPOINT_TAIL);
    }
    return out;
}

/* The offset in snapshot 'snap' of the last byte of its first compressed
 * block (0: none) */
static size_t coldmapTestCompressedTail_(const flex *snap) {
    flexEntry *fe = flexIndex(snap, CM_PF_COUNT);
    while (fe) {
        uint64_t compressed = 0;
        databox stored = {};
        flexReadUnsigned(snap, &fe, &compressed);
        flexGetByType(fe, &stored);
        if (compressed) {
            return (size_t)((const uint8_t *)databoxBytes(&stored) - snap) +
                   stored.len - 1;
        }
        fe = flexNext(snap, fe);
    }
    return 0;
}

/* RemoveIf's test: rows whose key (2 * row number) has a row number that
 * is 0 mod *ctx */
static bool coldmapTestRemoveMod_(void *ctx, const databox *const row[]) {
    const uint64_t mod = *(const uint64_t *)ctx;
    return (uint64_t)row[0]->data.i64 / 2 % mod == 0;
}

/* Row number 0 mod 3 goes, 1 mod 3 gets its value negated, 2 mod 3 stays;
 * counts the rows seen */
static coldmapSweepAction
coldmapTestSweep_(void *ctx, const databox *const row[], databox *last) {
    uint64_t *seen = ctx;
    (*seen)++;
    const uint64_t n = (uint64_t)row[0]->data.i64 / 2;
    if (n % 3 == 0) {
        return COLDMAP_SWEEP_REMOVE;
    }
    if (n % 3 == 1) {
        *last = databoxNewSigned(-row[1]->data.i64);
        return COLDMAP_SWEEP_CHANGE;
    }
    return COLDMAP_SWEEP_KEEP;
}

/* Every value becomes 100 bytes */
static coldmapSweepAction
coldmapTestWiden_(void *ctx, const databox *const row[], databox *last) {
    (void)row;
    static const char wide[101] =
        "0123456789012345678901234567890123456789012345678901234567890123456"
        "789012345678901234567890123456789";
    (*(uint64_t *)ctx)++;
    *last = databoxNewBytesString(wide);
    return COLDMAP_SWEEP_CHANGE;
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
                databox k = {};
                databox v = {};
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
        databox k = {};
        databox v = {};
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

    TEST("128-bit keys: lookups and iterators from every key and gap, both "
         "ways") {
        const __uint128_t base = (__uint128_t)1 << 64;
        cleaner(multimapFree) multimap *m = multimapNew(2);
        for (uint64_t i = 0; i < 200; i++) {
            __uint128_t wide = base + i * 10;
            const databox k = {
                .type = DATABOX_UNSIGNED_128, .data.u128 = &wide, .big = 1};
            const databox v = databoxNewSigned((int64_t)i);
            const databox *row[2] = {&k, &v};
            multimapInsert(&m, row);
        }
        const coldmapConfig small = {.rawLimit = FLEX_CAP_LEVEL_512,
                                     .storedTarget = 128};
        cleaner(coldmapFree) coldmap *cm =
            coldmapNewFromDump(2, &small, multimapDump(m));
        cleaner(mflexStateFree) mflexState *state = mflexStateCreate();
        for (uint64_t j = 0; j < 2000 + 15; j++) {
            __uint128_t probe = base + j;
            const databox key = {
                .type = DATABOX_UNSIGNED_128, .data.u128 = &probe, .big = 1};
            databox got = {};
            databox *values[1] = {&got};
            const bool has = coldmapLookup(cm, state, &key, values);
            if (has != (j % 10 == 0 && j < 2000) ||
                (has && got.data.i64 != (int64_t)(j / 10))) {
                ERR("lookup %" PRIu64, j);
            }

            for (int forward = 0; forward < 2; forward++) {
                const uint64_t first = (j + 9) / 10;
                const uint64_t last = j / 10 < 200 ? j / 10 : 199;
                coldmapIterator it;
                coldmapIteratorInitAt(cm, state, &it, forward, &key);
                databoxBig k;
                DATABOX_BIG_INIT(&k);
                databox v = {};
                databox *row[2] = {(databox *)&k, &v};
                const bool next = coldmapIteratorNext(&it, row);
                const uint64_t want = forward ? first : last;
                if (next != (forward ? first < 200 : true) ||
                    (next && (k.type != DATABOX_UNSIGNED_128 ||
                              *k.data.u128 != base + want * 10))) {
                    ERR("iterator from %" PRIu64 " forward %d", j, forward);
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

    TEST("RemoveIf takes exactly the rows its test names, once each, in key "
         "order") {
        static const coldmapConfig configs[] = {
            {}, {.rawLimit = FLEX_CAP_LEVEL_512, .storedTarget = 128}};
        cleaner(mflexStateFree) mflexState *state = mflexStateCreate();
        for (size_t c = 0; c < countof(configs); c++) {
            for (uint64_t mod = 2; mod <= 5; mod++) {
                cleaner(multimapFree) multimap *m =
                    coldmapTestRows_(COLDMAP_SHAPE_INTS, 2000);
                cleaner(coldmapFree) coldmap *cm =
                    coldmapNewFromDump(2, &configs[c], multimapDump(m));
                cleaner(flexFree) flex *removed = flexNew();
                const size_t gone =
                    coldmapRemoveIf(cm, coldmapTestRemoveMod_, &mod, &removed);

                /* The model: every key whose row number is 0 mod 'mod' */
                cleaner(flexFree) flex *wantRemoved = flexNew();
                for (uint64_t i = 0; i < 2000; i += mod) {
                    const databox k = databoxNewSigned((int64_t)i * 2);
                    databox v = {};
                    databox *vals[1] = {&v};
                    multimapLookup(m, &k, vals);
                    const databox *row[2] = {&k, &v};
                    flexAppendMultipleExact(&wantRemoved, 2, row);
                    multimapDelete(&m, &k);
                }
                cleaner(flexFree) flex *want = multimapDump(m);
                cleaner(flexFree) flex *got = coldmapDump(cm, state);
                if (gone != flexCount(wantRemoved) / 2 ||
                    !flexEqual(removed, wantRemoved) || !flexEqual(want, got) ||
                    !coldmapVerify_(cm, state)) {
                    ERR("config %zu mod %" PRIu64 ": %zu removed", c, mod,
                        gone);
                }
            }
        }
    }

    TEST("a sweep that widens values splits blocks to the raw limit and "
         "passes every row once") {
        const coldmapConfig config = {.rawLimit = FLEX_CAP_LEVEL_512,
                                      .storedTarget = 128};
        cleaner(mflexStateFree) mflexState *state = mflexStateCreate();
        cleaner(multimapFree) multimap *m =
            coldmapTestRows_(COLDMAP_SHAPE_INTS, 600);
        cleaner(coldmapFree) coldmap *cm =
            coldmapNewFromDump(2, &config, multimapDump(m));
        const size_t blocksBefore = coldmapBlockCount(cm);
        uint64_t seen = 0;
        size_t at = 0;
        size_t passes = 0;
        do {
            size_t next;
            (void)coldmapSweepBlock(cm, at, coldmapTestWiden_, &seen, nullptr,
                                    &next);
            at = next;
            passes++;
        } while (at != 0 && passes < 10000);
        bool fits = true;
        for (size_t i = 0; i < cm->blockCount; i++) {
            if (cm->blocks[i].count > 1 && cm->blocks[i].raw > cm->rawCap) {
                fits = false;
            }
        }
        if (seen != 600 || !fits || cm->rows != 600 ||
            coldmapBlockCount(cm) <= blocksBefore ||
            !coldmapVerify_(cm, state)) {
            ERR("%" PRIu64 " rows seen, %zu blocks (from %zu), %s", seen,
                coldmapBlockCount(cm), blocksBefore,
                fits ? "all fit" : "one past the raw limit");
        }
    }

    TEST("a sweep from block 0 round to it again passes every row once, "
         "keeping, removing and changing each as told") {
        static const coldmapConfig configs[] = {
            {}, {.rawLimit = FLEX_CAP_LEVEL_512, .storedTarget = 128}};
        cleaner(mflexStateFree) mflexState *state = mflexStateCreate();
        for (size_t c = 0; c < countof(configs); c++) {
            cleaner(multimapFree) multimap *m =
                coldmapTestRows_(COLDMAP_SHAPE_INTS, 2000);
            cleaner(coldmapFree) coldmap *cm =
                coldmapNewFromDump(2, &configs[c], multimapDump(m));
            cleaner(flexFree) flex *removed = flexNew();
            uint64_t seen = 0;
            size_t gone = 0;
            size_t at = 0;
            size_t passes = 0;
            do {
                size_t next;
                gone += coldmapSweepBlock(cm, at, coldmapTestSweep_, &seen,
                                          &removed, &next);
                at = next;
                passes++;
            } while (at != 0 && passes < 10000);

            /* The model, changed the same way */
            cleaner(flexFree) flex *wantRemoved = flexNew();
            for (uint64_t i = 0; i < 2000; i++) {
                const databox k = databoxNewSigned((int64_t)i * 2);
                databox v = {};
                databox *vals[1] = {&v};
                multimapLookup(m, &k, vals);
                if (i % 3 == 0) {
                    const databox *row[2] = {&k, &v};
                    flexAppendMultipleExact(&wantRemoved, 2, row);
                    multimapDelete(&m, &k);
                } else if (i % 3 == 1) {
                    const databox neg = databoxNewSigned(-v.data.i64);
                    const databox *row[2] = {&k, &neg};
                    multimapInsert(&m, row);
                }
            }
            cleaner(flexFree) flex *want = multimapDump(m);
            cleaner(flexFree) flex *got = coldmapDump(cm, state);
            if (seen != 2000 || gone != flexCount(wantRemoved) / 2 ||
                !flexEqual(removed, wantRemoved) || !flexEqual(want, got) ||
                !coldmapVerify_(cm, state)) {
                ERR("config %zu: %" PRIu64 " rows seen, %zu removed", c, seen,
                    gone);
            }
        }
    }

    TEST("batches of sorted keys delete as key-at-a-time deletes would") {
        static const coldmapConfig configs[] = {
            {}, {.rawLimit = FLEX_CAP_LEVEL_512, .storedTarget = 128}};
        cleaner(mflexStateFree) mflexState *state = mflexStateCreate();
        for (size_t c = 0; c < countof(configs); c++) {
            cleaner(multimapFree) multimap *m =
                coldmapTestRows_(COLDMAP_SHAPE_RECORDS, 1500);
            cleaner(coldmapFree) coldmap *cm =
                coldmapNewFromDump(2, &configs[c], multimapDump(m));
            uint64_t rng = 0xA4093822299F31D0ULL + c;
            char key[32];
            for (size_t batch = 0; batch < 30; batch++) {
                /* Ascending ids, present (even) and absent (odd) alike */
                cleaner(flexFree) flex *keys = flexNew();
                size_t id = (rng >> 20) % 400;
                size_t want = 0;
                const size_t count = 1 + (rng >> 30) % 150;
                for (size_t r = 0; r < count; r++) {
                    rng ^= rng << 13;
                    rng ^= rng >> 7;
                    rng ^= rng << 17;
                    id += 1 + (rng >> 33) % 7;
                    const databox k = coldmapTestKey_(COLDMAP_SHAPE_RECORDS, id,
                                                      key, sizeof(key));
                    flexPushByTypeExact(&keys, &k, FLEX_ENDPOINT_TAIL);
                    want += multimapDelete(&m, &k);
                }

                size_t deleted = 0;
                if (!coldmapDeleteMany(cm, keys, &deleted) || deleted != want) {
                    ERR("config %zu batch %zu: %zu deleted, want %zu", c, batch,
                        deleted, want);
                }
            }

            cleaner(flexFree) flex *want = multimapDump(m);
            cleaner(flexFree) flex *got = coldmapDump(cm, state);
            if (!flexEqual(want, got) || !coldmapVerify_(cm, state)) {
                ERR("config %zu: rows or blocks differ", c);
            }

            /* Keys out of order change nothing */
            cleaner(flexFree) flex *bad = flexNew();
            const databox hi =
                coldmapTestKey_(COLDMAP_SHAPE_RECORDS, 900, key, sizeof(key));
            flexPushByTypeExact(&bad, &hi, FLEX_ENDPOINT_TAIL);
            flexPushByTypeExact(&bad, &hi, FLEX_ENDPOINT_TAIL);
            if (coldmapDeleteMany(cm, bad, nullptr) ||
                coldmapCount(cm) != multimapCount(m)) {
                ERR("config %zu: repeated keys taken", c);
            }
        }
    }

    TEST("a snapshot whose header field is out of range, or wraps into "
         "range in its type, is refused") {
        cleaner(multimapFree) multimap *m =
            coldmapTestRows_(COLDMAP_SHAPE_RECORDS, 200);
        cleaner(coldmapFree) coldmap *cm =
            coldmapNewFromDump(2, nullptr, multimapDump(m));
        size_t len = 0;
        cleanerName(zfree, Void) void *snap =
            persistOpsColdmap.snapshot(cm, &len);
        cleaner(coldmapFree) coldmap *same =
            persistOpsColdmap.restore(snap, len);
        if (!same) {
            ERR("%s", "an untouched snapshot was refused");
        }

        static const struct {
            coldmapPersistField_ field;
            uint64_t value;
        } bad[] = {
            {CM_PF_ELEMENTS, 0},
            {CM_PF_ELEMENTS, MULTIMAP_ELEMENTS_MAX + 1},
            {CM_PF_ELEMENTS, UINT64_C(1) << 32 | 2}, /* wraps to 2 */
            {CM_PF_RAW_LIMIT, 0},
            {CM_PF_RAW_LIMIT, FLEX_CAP_LEVEL_65536 + 1},
            {CM_PF_STORED_TARGET, 0},
            {CM_PF_MIN_SAVING, MFLEX_SAVING_PERCENT_MAX + 1},
            {CM_PF_MIN_SAVING, 256 + 50}, /* wraps to 50 */
            {CM_PF_LEVEL, CFLEX_LEVEL_MAX + 1},
            {CM_PF_LEVEL, 256 + 1}, /* wraps to 1 */
            {CM_PF_BLOCKS, 0},
        };
        for (size_t b = 0; b < countof(bad); b++) {
            const databoxBig with = {.data.u64 = bad[b].value,
                                     .type = DATABOX_UNSIGNED_64};
            cleaner(flexFree) flex *damaged =
                flexTestReplaceEntry((const flex *)snap, bad[b].field, &with);
            coldmap *r = persistOpsColdmap.restore(damaged, flexBytes(damaged));
            if (r) {
                ERR("field %d = %" PRIu64 " restored", (int)bad[b].field,
                    bad[b].value);
                coldmapFree(r);
            }
        }
    }

    TEST("a snapshot keeps the blocks as stored: a restore has the same "
         "blocks and snapshots to the same bytes; damaged blocks are "
         "refused") {
        /* Records compress; integers under a 20% saving stay plain */
        static const coldmapConfig configs[] = {
            {.rawLimit = FLEX_CAP_LEVEL_512, .storedTarget = 128},
            {.rawLimit = FLEX_CAP_LEVEL_512,
             .storedTarget = 128,
             .minSavingPercent = 20},
        };
        static const coldmapShape_ shapes[] = {COLDMAP_SHAPE_RECORDS,
                                               COLDMAP_SHAPE_INTS};
        cleaner(mflexStateFree) mflexState *state = mflexStateCreate();
        for (size_t c = 0; c < countof(configs); c++) {
            cleaner(multimapFree) multimap *m =
                coldmapTestRows_(shapes[c], 300);
            cleaner(coldmapFree) coldmap *cm =
                coldmapNewFromDump(2, &configs[c], multimapDump(m));
            /* Inserts split blocks; deletes shrink and merge them */
            uint64_t rng = 0x452821E638D01377ULL + c;
            char key[32];
            for (size_t step = 0; step < 3000; step++) {
                rng ^= rng << 13;
                rng ^= rng >> 7;
                rng ^= rng << 17;
                const databox k = coldmapTestKey_(shapes[c], (rng >> 8) % 1200,
                                                  key, sizeof(key));
                if (rng % 3) {
                    const databox v = databoxNewSigned((int64_t)(rng >> 20));
                    const databox *row[2] = {&k, &v};
                    coldmapInsert(cm, row);
                } else {
                    coldmapDelete(cm, &k);
                }
            }
            if (cm->blockCount < 3 || (c == 0) != (cm->compressedBlocks > 0)) {
                ERR("config %zu: %zu blocks, %" PRIu64 " compressed", c,
                    cm->blockCount, cm->compressedBlocks);
            }

            size_t len = 0;
            cleanerName(zfree, Void) void *snap =
                persistOpsColdmap.snapshot(cm, &len);
            cleaner(coldmapFree) coldmap *back =
                persistOpsColdmap.restore(snap, len);
            size_t againLen = 0;
            cleanerName(zfree, Void) void *again =
                back ? persistOpsColdmap.snapshot(back, &againLen) : nullptr;
            if (!back || !coldmapSameBlocks_(cm, back) ||
                !coldmapVerify_(back, state) || againLen != len ||
                memcmp(snap, again, len)) {
                ERR("config %zu: the restore's blocks or snapshot differ", c);
                continue;
            }

            /* The snapshot rebuilt entry by entry restores; with its
             * first two blocks swapped, keys no longer ascend */
            cleaner(flexFree) flex *inOrder = coldmapTestReorder_(snap, false);
            cleaner(flexFree) flex *swapped = coldmapTestReorder_(snap, true);
            cleaner(coldmapFree) coldmap *rebuilt =
                persistOpsColdmap.restore(inOrder, flexBytes(inOrder));
            cleaner(coldmapFree) coldmap *outOfOrder =
                persistOpsColdmap.restore(swapped, flexBytes(swapped));
            if (!rebuilt || !coldmapSameBlocks_(cm, rebuilt) || outOfOrder) {
                ERR("config %zu: blocks in order refused or out of order "
                    "taken",
                    c);
            }

            /* The last byte of a compressed block is the last byte of its
             * rows (a trailer): flipped, the block does not open */
            if (c == 0) {
                const size_t at = coldmapTestCompressedTail_(snap);
                cleanerName(zfree, Void) void *bad = zmalloc(len);
                memcpy(bad, snap, len);
                ((uint8_t *)bad)[at] ^= 0xFF;
                cleaner(coldmapFree) coldmap *flipped =
                    persistOpsColdmap.restore(bad, len);
                if (!at || flipped) {
                    ERR("%s", "a flipped byte in a compressed block taken");
                }
            }
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

        /* A row width a snapshot could not restore */
        if (coldmapNew(UINT16_MAX + 1u, nullptr)) {
            ERR("%s", "a 65536-element row width taken");
        }
    }

    TEST_FINAL_RESULT;
}
#endif
