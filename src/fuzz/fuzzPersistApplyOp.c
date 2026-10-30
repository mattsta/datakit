/* fuzzPersistApplyOp - WAL entry bodies replayed by their type's applyOp
 *
 * Input: one byte naming the type (fuzzPickOps), then entries, each an
 * op byte, a little-endian 16-bit body length and the body (a short
 * last entry takes the bytes left).  Every entry goes to applyOp on one
 * structure, from newEmpty, in its own exact-size buffer so a read past
 * a body is a sanitizer report.  applyOp may refuse any entry; after
 * them all the structure is one its validate accepts, whose snapshot
 * restores to the same count. */

#include "fuzzCommon.h"

#include "../flex.h"

#include <string.h>

/* The most cells a dense indexed type may span here.  The fenwick,
 * multiFenwick and segment trees are arrays as long as their largest
 * index (a 2d tree as its largest row times its largest column, up to
 * 2^32 cells), so an update at a large index allocates that much: that
 * measures the input's size, not a defect, and an op that would take
 * the structure past this is skipped */
static constexpr uint64_t FUZZ_CELLS_MAX = UINT64_C(1) << 16;

/* The largest index (2d: row and column) the applied ops named */
typedef struct fuzzSpan {
    uint64_t largest[2];
} fuzzSpan;

static bool fuzzIndexTooLarge(const persistOps *ops, const uint8_t *body,
                              size_t len, fuzzSpan *span) {
    static const char *const dense[] = {"fenwick", "multiFenwick", "segment"};
    bool isDense = false;
    for (size_t i = 0; i < countof(dense); i++) {
        isDense |= strncmp(ops->name, dense[i], strlen(dense[i])) == 0;
    }

    if (!isDense || !flexValidateBufferDeep(body, len)) {
        return false;
    }

    /* The body is the indexes (a 2d tree's row and column, a segment
     * tree's position or range), then the value */
    const bool twoD = strstr(ops->name, "2d") != NULL;
    const flex *f = (const flex *)body;
    const size_t indexes = flexCount(f) ? flexCount(f) - 1 : 0;
    fuzzSpan grown = *span;
    flexEntry *fe = flexHeadIfAny(f);
    for (size_t i = 0; i < indexes && fe; i++, fe = flexNext(f, fe)) {
        databoxBig index;
        DATABOX_BIG_INIT(&index);
        flexGetByType(fe, (databox *)&index);
        if (index.type != DATABOX_UNSIGNED_64) {
            continue;
        }

        if (index.data.u64 >= FUZZ_CELLS_MAX) {
            return true;
        }

        uint64_t *largest = &grown.largest[twoD && i == 1];
        *largest = index.data.u64 > *largest ? index.data.u64 : *largest;
    }

    const uint64_t cells =
        (grown.largest[0] + 1) * (twoD ? grown.largest[1] + 1 : 1);
    if (cells > FUZZ_CELLS_MAX) {
        return true;
    }

    *span = grown;
    return false;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t len) {
    /* A snapshot-only type (no applyOp) has no WAL, and a type with no
     * meaningful empty state has no newEmpty to replay onto */
    const persistOps *ops = fuzzPickOps(&data, &len);
    if (!ops || !ops->applyOp || !ops->newEmpty) {
        return 0;
    }

    void *token = ops->foldEnter ? ops->foldEnter() : NULL;
    void *s = ops->newEmpty();
    fuzzSpan span = {};
    FUZZ_REQUIRE(s);
    while (len >= 3) {
        const persistOp op = (persistOp)data[0];
        size_t bodyLen = (size_t)data[1] | (size_t)data[2] << 8;
        data += 3;
        len -= 3;
        if (bodyLen > len) {
            bodyLen = len;
        }

        if (fuzzIndexTooLarge(ops, data, bodyLen, &span)) {
            data += bodyLen;
            len -= bodyLen;
            continue;
        }

        {
            cleaner(zfree) void *body = zmalloc(bodyLen ? bodyLen : 1);
            if (bodyLen) {
                memcpy(body, data, bodyLen);
            }

            ops->applyOp(&s, op, body, bodyLen);
        }

        FUZZ_REQUIRE(s);
        data += bodyLen;
        len -= bodyLen;
    }

    FUZZ_REQUIRE(!ops->validate || ops->validate(s));
    size_t snapLen = 0;
    uint8_t *snap = ops->snapshot(s, &snapLen);
    FUZZ_REQUIRE(snap);
    void *again = ops->restore(snap, snapLen);
    FUZZ_REQUIRE(again);
    FUZZ_REQUIRE(!ops->count || ops->count(again) == ops->count(s));
    zfree(snap);
    ops->free(again);
    ops->free(s);

    if (ops->foldExit) {
        ops->foldExit(token);
    }

    return 0;
}
