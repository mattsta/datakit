#include "idRows.h"
#include "dkEndian.h"

struct idRows {
    /* page[p] holds the rows of ids [p * IDROWS_PAGE, (p + 1) *
     * IDROWS_PAGE); NULL while none of them is present */
    flex **page;
    uint64_t pages; /* entries of 'page' */
    uint64_t count; /* rows held */
    uint32_t elementsPerRow;
};

/* A page's table: one little-endian uint32_t per id of the page, the
 * offset of its row's first entry from the table's own first byte (0 for
 * an absent id: every row lies past the table).  A flex header that grows
 * moves the table and the rows alike, so offsets never change for it. */
static constexpr size_t IDROWS_TABLE_BYTES = IDROWS_PAGE * sizeof(uint32_t);

static uint8_t *idRowsTable_(const flex *f) {
    databox t;
    flexGetByType(flexHead(f), &t);
    return (uint8_t *)(uintptr_t)t.data.bytes.start;
}

static uint32_t idRowsOffset_(const uint8_t *table, const uint32_t slot) {
    return dkLoadLe32(table + (size_t)slot * sizeof(uint32_t));
}

static void idRowsSetOffset_(uint8_t *table, const uint32_t slot,
                             const uint32_t offset) {
    dkStoreLe32(table + (size_t)slot * sizeof(uint32_t), offset);
}

/* The bytes from the table's first byte to the flex's end: a change to
 * the rows moves it by exactly the bytes the change added or removed */
static size_t idRowsSpan_(const flex *f, const uint8_t *table) {
    return (size_t)((const uint8_t *)f + flexBytes(f) - table);
}

/* Move the rows of the slots after 'slot' by 'delta' bytes */
static void idRowsShift_(uint8_t *table, const uint32_t slot,
                         const int64_t delta) {
    for (uint32_t s = slot + 1; s < IDROWS_PAGE; s++) {
        const uint32_t offset = idRowsOffset_(table, s);
        if (offset) {
            idRowsSetOffset_(table, s, (uint32_t)((int64_t)offset + delta));
        }
    }
}

/* ====================================================================
 * Create / Free
 * ==================================================================== */
idRows *idRowsNew(const uint32_t elementsPerRow) {
    assert(elementsPerRow > 0);
    /* @own: caller; free=idRowsFree */
    idRows *r = zcalloc(1, sizeof(*r));
    r->elementsPerRow = elementsPerRow;
    return r;
}

void idRowsFree(idRows *r) {
    if (!r) {
        return;
    }

    for (uint64_t p = 0; p < r->pages; p++) {
        flexFree(r->page[p]);
    }

    zfree(r->page);
    zfree(r);
}

size_t idRowsCount(const idRows *r) {
    return r->count;
}

size_t idRowsBytes(const idRows *r) {
    size_t bytes = r->pages * sizeof(flex *);
    for (uint64_t p = 0; p < r->pages; p++) {
        if (r->page[p]) {
            bytes += flexBytes(r->page[p]);
        }
    }

    return bytes;
}

/* ====================================================================
 * Find
 * ==================================================================== */
/* The first entry of the row of 'id', its page in '*pageOut'; NULL when
 * the id is absent */
static flexEntry *idRowsFind_(const idRows *r, const uint64_t id,
                              flex **pageOut) {
    const uint64_t p = id / IDROWS_PAGE;
    if (p >= r->pages || !r->page[p]) {
        return nullptr;
    }

    flex *f = r->page[p];
    uint8_t *table = idRowsTable_(f);
    const uint32_t offset = idRowsOffset_(table, (uint32_t)(id % IDROWS_PAGE));
    if (!offset) {
        return nullptr;
    }

    *pageOut = f;
    return (flexEntry *)(table + offset);
}

/* Decode the row at 'fe' of 'f' into 'row' (wide elements into 'slots'
 * when given) */
static void idRowsDecode_(const idRows *r, const flex *f, flexEntry *fe,
                          databox *row[], databoxWide *slots) {
    for (uint32_t i = 0; i < r->elementsPerRow; i++) {
        if (slots) {
            flexGetByTypeInto(fe, row[i], &slots[i]);
        } else {
            flexGetByType(fe, row[i]);
        }

        fe = flexNext(f, fe);
    }
}

bool idRowsExists(const idRows *r, const uint64_t id) {
    flex *f;
    return idRowsFind_(r, id, &f) != nullptr;
}

bool idRowsGetWithSlots(const idRows *r, const uint64_t id, databox *row[],
                        databoxWide *slots) {
    flex *f;
    flexEntry *fe = idRowsFind_(r, id, &f);
    if (!fe) {
        return false;
    }

    idRowsDecode_(r, f, fe, row, slots);
    return true;
}

bool idRowsGet(const idRows *r, const uint64_t id, databox *row[]) {
    return idRowsGetWithSlots(r, id, row, nullptr);
}

/* ====================================================================
 * Change
 * ==================================================================== */
/* The page for 'id', made (with an empty table) when it has none */
static flex **idRowsPageFor_(idRows *r, const uint64_t id) {
    const uint64_t p = id / IDROWS_PAGE;
    if (p >= r->pages) {
        uint64_t pages = r->pages ? r->pages * 2 : 4;
        if (pages <= p) {
            pages = p + 1;
        }

        zreallocSelf(r->page, pages * sizeof(flex *));
        memset(r->page + r->pages, 0, (pages - r->pages) * sizeof(flex *));
        r->pages = pages;
    }

    if (!r->page[p]) {
        static const uint8_t empty[IDROWS_TABLE_BYTES] = {};
        /* @own: container; free=idRowsFree */
        r->page[p] = flexNew();
        flexPushBytesExact(&r->page[p], empty, sizeof(empty),
                           FLEX_ENDPOINT_TAIL);
    }

    return &r->page[p];
}

/* Insert 'row' as the row of 'slot', which is absent: before the next
 * present row of the page, or after the last */
static void idRowsInsertRow_(const idRows *r, flex **ff, const uint32_t slot,
                             const databox *row[]) {
    uint8_t *table = idRowsTable_(*ff);
    const size_t spanBefore = idRowsSpan_(*ff, table);
    uint32_t next = 0;
    for (uint32_t s = slot + 1; s < IDROWS_PAGE && !next; s++) {
        next = idRowsOffset_(table, s);
    }

    uint32_t offset;
    if (next) {
        /* Last element first at one position: they end in row order */
        for (uint32_t i = r->elementsPerRow; i-- > 0;) {
            flexInsertByType(ff, (flexEntry *)(idRowsTable_(*ff) + next),
                             row[i]);
        }

        offset = next;
    } else {
        for (uint32_t i = 0; i < r->elementsPerRow; i++) {
            flexPushByTypeExact(ff, row[i], FLEX_ENDPOINT_TAIL);
        }

        flexEntry *fe = flexTail(*ff);
        for (uint32_t i = 1; i < r->elementsPerRow; i++) {
            fe = flexPrev(*ff, fe);
        }

        offset = (uint32_t)((uint8_t *)fe - idRowsTable_(*ff));
    }

    table = idRowsTable_(*ff);
    idRowsShift_(table, slot,
                 (int64_t)idRowsSpan_(*ff, table) - (int64_t)spanBefore);
    idRowsSetOffset_(table, slot, offset);
}

/* Remove the row of 'slot', which is present */
static void idRowsDeleteRow_(const idRows *r, flex **ff, const uint32_t slot) {
    uint8_t *table = idRowsTable_(*ff);
    const size_t spanBefore = idRowsSpan_(*ff, table);
    flexEntry *fe = (flexEntry *)(table + idRowsOffset_(table, slot));
    flexDeleteCount(ff, &fe, r->elementsPerRow);
    table = idRowsTable_(*ff);
    idRowsShift_(table, slot,
                 (int64_t)idRowsSpan_(*ff, table) - (int64_t)spanBefore);
    idRowsSetOffset_(table, slot, 0);
}

bool idRowsSet(idRows *r, const uint64_t id, const databox *row[]) {
    flex **ff = idRowsPageFor_(r, id);
    const uint32_t slot = (uint32_t)(id % IDROWS_PAGE);
    const bool present = idRowsOffset_(idRowsTable_(*ff), slot) != 0;
    if (present) {
        idRowsDeleteRow_(r, ff, slot);
    }

    idRowsInsertRow_(r, ff, slot, row);
    if (!present) {
        r->count++;
    }

    return !present;
}

/* Element 'field' of the row of 'id', its page cell in '*ffOut' and its
 * slot in '*slotOut'; NULL when the id is absent */
static flexEntry *idRowsField_(idRows *r, const uint64_t id,
                               const uint32_t field, flex ***ffOut,
                               uint32_t *slotOut) {
    assert(field < r->elementsPerRow);
    flex *f;
    flexEntry *fe = idRowsFind_(r, id, &f);
    if (!fe) {
        return nullptr;
    }

    for (uint32_t i = 0; i < field; i++) {
        fe = flexNext(f, fe);
    }

    *ffOut = &r->page[id / IDROWS_PAGE];
    *slotOut = (uint32_t)(id % IDROWS_PAGE);
    return fe;
}

bool idRowsReplaceField(idRows *r, const uint64_t id, const uint32_t field,
                        const databox *box) {
    flex **ff;
    uint32_t slot;
    flexEntry *fe = idRowsField_(r, id, field, &ff, &slot);
    if (!fe) {
        return false;
    }

    const size_t spanBefore = idRowsSpan_(*ff, idRowsTable_(*ff));
    flexReplaceByType(ff, fe, box);
    uint8_t *table = idRowsTable_(*ff);
    idRowsShift_(table, slot,
                 (int64_t)idRowsSpan_(*ff, table) - (int64_t)spanBefore);
    return true;
}

bool idRowsFieldIncr(idRows *r, const uint64_t id, const uint32_t field,
                     const int64_t incrBy, int64_t *newValue) {
    flex **ff;
    uint32_t slot;
    flexEntry *fe = idRowsField_(r, id, field, &ff, &slot);
    if (!fe) {
        if (newValue) {
            *newValue = 0;
        }

        return false;
    }

    const size_t spanBefore = idRowsSpan_(*ff, idRowsTable_(*ff));
    int64_t sum = 0;
    if (!flexIncrbySigned(ff, fe, incrBy, &sum)) {
        if (newValue) {
            databox current;
            flexGetByTypeInto(fe, &current, nullptr);
            *newValue = databoxIsWideDeferred(&current) ? 0 : current.data.i;
        }

        return false;
    }

    uint8_t *table = idRowsTable_(*ff);
    idRowsShift_(table, slot,
                 (int64_t)idRowsSpan_(*ff, table) - (int64_t)spanBefore);
    if (newValue) {
        *newValue = sum;
    }

    return true;
}

bool idRowsDelete(idRows *r, const uint64_t id) {
    flex *f;
    if (!idRowsFind_(r, id, &f)) {
        return false;
    }

    flex **ff = &r->page[id / IDROWS_PAGE];
    idRowsDeleteRow_(r, ff, (uint32_t)(id % IDROWS_PAGE));
    r->count--;

    /* A page left with no row goes */
    const uint8_t *table = idRowsTable_(*ff);
    for (uint32_t s = 0; s < IDROWS_PAGE; s++) {
        if (idRowsOffset_(table, s)) {
            return true;
        }
    }

    flexFree(*ff);
    *ff = nullptr;
    return true;
}

/* ====================================================================
 * Order
 * ==================================================================== */
bool idRowsFirst(const idRows *r, uint64_t *id) {
    for (uint64_t p = 0; p < r->pages; p++) {
        if (!r->page[p]) {
            continue;
        }

        const uint8_t *table = idRowsTable_(r->page[p]);
        for (uint32_t s = 0; s < IDROWS_PAGE; s++) {
            if (idRowsOffset_(table, s)) {
                *id = p * IDROWS_PAGE + s;
                return true;
            }
        }
    }

    return false;
}

bool idRowsLast(const idRows *r, uint64_t *id) {
    for (uint64_t p = r->pages; p-- > 0;) {
        if (!r->page[p]) {
            continue;
        }

        const uint8_t *table = idRowsTable_(r->page[p]);
        for (uint32_t s = IDROWS_PAGE; s-- > 0;) {
            if (idRowsOffset_(table, s)) {
                *id = p * IDROWS_PAGE + s;
                return true;
            }
        }
    }

    return false;
}

void idRowsIteratorInit(const idRows *r, idRowsIterator *iter) {
    *iter = (idRowsIterator){.r = r, .next = 0};
}

bool idRowsIteratorNext(idRowsIterator *iter, uint64_t *id, databox *row[],
                        databoxWide *slots) {
    const idRows *r = iter->r;
    while (iter->next / IDROWS_PAGE < r->pages) {
        const uint64_t p = iter->next / IDROWS_PAGE;
        flex *f = r->page[p];
        if (!f) {
            iter->next = (p + 1) * IDROWS_PAGE;
            continue;
        }

        uint8_t *table = idRowsTable_(f);
        for (uint32_t s = (uint32_t)(iter->next % IDROWS_PAGE); s < IDROWS_PAGE;
             s++) {
            const uint32_t offset = idRowsOffset_(table, s);
            if (offset) {
                *id = p * IDROWS_PAGE + s;
                iter->next = *id + 1;
                idRowsDecode_(r, f, (flexEntry *)(table + offset), row, slots);
                return true;
            }
        }

        iter->next = (p + 1) * IDROWS_PAGE;
    }

    return false;
}

/* ====================================================================
 * Tests
 * ==================================================================== */
#ifdef DATAKIT_TEST
#include "cleaner.h"
#include "ctest.h"
#include "multimap.h"

defcleaner(idRows *, idRowsFree);
defcleaner(multimap *, multimapFree);

/* Test row value 'v' of a mix: small and large integers, 128-bit values,
 * and byte strings up to 3000 bytes (so a page passes 64 KB) */
static databox idRowsTestValue_(uint64_t v, uint8_t *bytes, size_t cap,
                                databoxBig *wide) {
    switch (v % 5) {
    case 0:
        return databoxNewSigned((int64_t)(v % 300) - 150);
    case 1:
        return databoxNewUnsigned(v * 0x9E3779B97F4A7C15ull);
    case 2:
        DATABOX_BIG_UNSIGNED_128(wide, ((__uint128_t)1 << 90) + v);
        return *(const databox *)wide;
    default: {
        const size_t len = (v * 37) % (cap < 3000 ? cap : 3000);
        for (size_t i = 0; i < len; i++) {
            bytes[i] = (uint8_t)(v + i * 7);
        }
        return databoxNewBytes(bytes, len);
    }
    }
}

/* Whether boxes 'a' and 'b' hold the same value */
static bool idRowsTestSame_(const databox *a, const databox *b) {
    return a->type == b->type && databoxCompare(a, b) == 0;
}

/* Compare every row of 'r' with the oracle {id, a, b} rows, in order */
static int idRowsTestAgree_(const idRows *r, multimap *oracle,
                            const char *when) {
    int err = 0;
    if (idRowsCount(r) != multimapCount(oracle)) {
        ERR("%s: %zu rows, oracle %zu", when, idRowsCount(r),
            multimapCount(oracle));
        return err;
    }

    multimapIterator mi;
    multimapIteratorInit(oracle, &mi, true);
    idRowsIterator ri;
    idRowsIteratorInit(r, &ri);
    for (size_t n = 0;; n++) {
        databox oid = {}, oa = {}, ob = {};
        databox *orow[3] = {&oid, &oa, &ob};
        databoxWide owide[3];
        const bool more = multimapIteratorNextWithSlots(&mi, orow, owide);
        uint64_t id = 0;
        databox ra = {}, rb = {};
        databox *rrow[2] = {&ra, &rb};
        databoxWide rwide[2];
        const bool rmore = idRowsIteratorNext(&ri, &id, rrow, rwide);
        if (more != rmore) {
            ERR("%s: iteration ends apart at row %zu", when, n);
            return err;
        }

        if (!more) {
            break;
        }

        if (id != oid.data.u || !idRowsTestSame_(&ra, &oa) ||
            !idRowsTestSame_(&rb, &ob)) {
            ERR("%s: row %zu is id %" PRIu64 ", oracle id %" PRIu64, when, n,
                id, oid.data.u64);
            return err;
        }
    }

    uint64_t first = 0;
    uint64_t last = 0;
    databox ofirst[3] = {};
    databox olast[3] = {};
    const bool any = idRowsFirst(r, &first) && idRowsLast(r, &last);
    const bool oany = multimapFirstInto(oracle, ofirst, 3) &&
                      multimapLastInto(oracle, olast, 3);
    if (any != oany ||
        (any && (first != ofirst[0].data.u || last != olast[0].data.u))) {
        ERR("%s: first/last %" PRIu64 "/%" PRIu64 ", oracle %" PRIu64
            "/%" PRIu64,
            when, first, last, ofirst[0].data.u64, olast[0].data.u64);
    }

    return err;
}

int idRowsTest(int, char *[]) {
    int err = 0;

    TEST("rows read back by id, across pages, absent ids absent") {
        cleaner(idRowsFree) idRows *r = idRowsNew(2);
        static const uint64_t ids[] = {0,   1,   63,  64,   65,
                                       127, 128, 500, 4095, 70000};
        for (size_t i = 0; i < countof(ids); i++) {
            const databox a = databoxNewUnsigned(ids[i] * 3);
            const databox b = databoxNewSigned(-(int64_t)ids[i]);
            const databox *row[2] = {&a, &b};
            if (!idRowsSet(r, ids[i], row)) {
                ERR("id %" PRIu64 " was not new", ids[i]);
            }
        }

        for (size_t i = 0; i < countof(ids); i++) {
            databox a = {}, b = {};
            databox *row[2] = {&a, &b};
            if (!idRowsGet(r, ids[i], row) || a.data.u != ids[i] * 3 ||
                b.data.i != -(int64_t)ids[i]) {
                ERR("id %" PRIu64 " reads wrong", ids[i]);
            }

            if (idRowsExists(r, ids[i] + 2) && ids[i] != 63 && ids[i] != 126) {
                ERR("id %" PRIu64 " + 2 reads as present", ids[i]);
            }
        }

        if (idRowsCount(r) != countof(ids) || idRowsExists(r, 2) ||
            idRowsExists(r, 1u << 30)) {
            ERR("count %zu or a far id present", idRowsCount(r));
        }
    }

    TEST("random sets, replaces, increments and deletes agree with a "
         "multimap") {
        cleaner(idRowsFree) idRows *r = idRowsNew(2);
        cleaner(multimapFree) multimap *oracle = multimapNew(3);
        static uint8_t bytes[2][3000];
        uint64_t rng = 0x1D5;
        typedef enum idRowsTestOp {
            ID_ROWS_TEST_SET = 0,
            ID_ROWS_TEST_REPLACE_FIELD,
            ID_ROWS_TEST_INCR,
            ID_ROWS_TEST_DELETE,
            ID_ROWS_TEST_OP_COUNT
        } idRowsTestOp;
        for (int op = 0; op < 20000; op++) {
            const uint64_t id = dkSplitmix64Bounded(&rng, 700);
            const idRowsTestOp kind =
                (idRowsTestOp)dkSplitmix64Bounded(&rng, ID_ROWS_TEST_OP_COUNT);
            const databox idBox = databoxNewUnsigned(id);
            databox o[2] = {};
            databox *orow[2] = {&o[0], &o[1]};
            const bool present = multimapLookup(oracle, &idBox, orow);
            if (present != idRowsExists(r, id)) {
                ERR("op %d: id %" PRIu64 " present %d, oracle %d", op, id,
                    idRowsExists(r, id), present);
                break;
            }

            databoxBig wide[2];
            const uint64_t v = dkSplitmix64Next(&rng);
            switch (kind) {
            case ID_ROWS_TEST_SET: {
                const databox a = idRowsTestValue_(v, bytes[0], 3000, &wide[0]);
                const databox b = databoxNewSigned((int64_t)(v % 1000));
                const databox *row[2] = {&a, &b};
                const databox *orowSet[3] = {&idBox, &a, &b};
                if (idRowsSet(r, id, row) == present) {
                    ERR("op %d: set reported new = %d", op, present);
                }
                multimapInsert(&oracle, orowSet);
                break;
            }
            case ID_ROWS_TEST_REPLACE_FIELD: {
                const databox a =
                    idRowsTestValue_(v >> 3, bytes[1], 3000, &wide[1]);
                if (idRowsReplaceField(r, id, 0, &a) != present) {
                    ERR("op %d: replace on present %d", op, present);
                }
                if (present) {
                    const databox *orowSet[3] = {&idBox, &a, &o[1]};
                    multimapInsert(&oracle, orowSet);
                }
                break;
            }
            case ID_ROWS_TEST_INCR: {
                const int64_t delta = (int64_t)(v % 7) - 3;
                int64_t got = 0;
                const bool did = idRowsFieldIncr(r, id, 1, delta, &got);
                if (did != present || (present && got != o[1].data.i + delta)) {
                    ERR("op %d: incr did %d got %" PRId64, op, did, got);
                }
                if (present) {
                    multimapFieldIncr(&oracle, &idBox, 2, delta);
                }
                break;
            }
            case ID_ROWS_TEST_DELETE:
                if (idRowsDelete(r, id) != present) {
                    ERR("op %d: delete on present %d", op, present);
                }
                multimapDelete(&oracle, &idBox);
                break;
            case ID_ROWS_TEST_OP_COUNT:
                break;
            }

            if (op % 2000 == 1999) {
                err += idRowsTestAgree_(r, oracle, "during");
            }
        }

        err += idRowsTestAgree_(r, oracle, "after");

        /* Emptied, every page goes */
        for (uint64_t id = 0; id < 700; id++) {
            idRowsDelete(r, id);
        }

        if (idRowsCount(r) != 0 || idRowsBytes(r) > r->pages * sizeof(flex *)) {
            ERR("emptied: %zu rows, %zu bytes", idRowsCount(r), idRowsBytes(r));
        }
    }

    TEST("an increment past int64_t is refused and reports the value") {
        cleaner(idRowsFree) idRows *r = idRowsNew(1);
        const databox big = databoxNewSigned(INT64_MAX - 1);
        const databox *row[1] = {&big};
        idRowsSet(r, 5, row);
        int64_t now = 0;
        if (!idRowsFieldIncr(r, 5, 0, 1, &now) || now != INT64_MAX ||
            idRowsFieldIncr(r, 5, 0, 1, &now) || now != INT64_MAX ||
            idRowsFieldIncr(r, 6, 0, 1, &now) || now != 0) {
            ERR("increments at the edge: now %" PRId64, now);
        }
    }

    TEST_FINAL_RESULT;
}
#endif
