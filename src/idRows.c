#include "idRows.h"
#include "cleaner.h"
#include "dkC23.h"
#include "dkEndian.h"
#include "persist.h"

defcleaner(idRows *, idRowsFree);
defcleanerName(void *, zfree, Void);

/* Page number p holds the rows of ids [p * IDROWS_PAGE, (p + 1) *
 * IDROWS_PAGE).  The directory finds a page by its number in one of two
 * forms, chosen by how many of the pages it spans hold rows:
 *   - dense: page[i] is page number base + i (NULL while it holds no
 *     row), for 'span' pages from 'base'; its ends hold rows.  A read is
 *     one subtraction and one load.  Kept while span <= 4 * live + 64.
 *   - sparse: page[i] is page number pageNo[i], ascending, every one
 *     holding rows ('span' == 'live').  A read is a binary search.  Taken
 *     when a dense directory would pass that bound, left when span fits
 *     2 * live + 64.
 * Either way the directory costs O(pages holding rows), never O(highest
 * id): ids from a counter that has run far stay cheap, and a window of ids
 * sliding up the number line stays dense. */
struct idRows {
    flex **page;
    uint64_t *pageNo; /* sparse only; NULL while dense */
    uint64_t base;    /* dense: the page number of page[0] */
    uint64_t span;    /* entries of page (and pageNo) in use */
    uint64_t cap;     /* entries allocated */
    uint64_t live;    /* pages holding rows */
    uint64_t count;   /* rows held */
    uint32_t elementsPerRow;
};

/* Dense while span <= DENSE_PER_LIVE * live + DENSE_SLACK; back to dense
 * from sparse at SPARSE_BACK_PER_LIVE (hysteresis) */
static constexpr uint64_t IDROWS_DENSE_PER_LIVE = 4;
static constexpr uint64_t IDROWS_SPARSE_BACK_PER_LIVE = 2;
static constexpr uint64_t IDROWS_DENSE_SLACK = 64;

/* Interpolation probes a sparse lookup makes before its binary search */
static constexpr int IDROWS_INTERPOLATE = 3;

/* A page's index, its entry 0: a little-endian uint64_t with bit s set
 * when id s of the page is present, then room for 'capacity' little-endian
 * uint32_t offsets, the first 'rows' in use: one per present id, in id
 * order, the offset of its row's first entry from the index's own first
 * byte.  The rank of an id among the present ones (a popcount below its
 * bit) is its offset's place.
 *
 * Capacity doubles when the offsets fill and halves when a quarter are in
 * use, so a set or a delete edits the index in place, and only a resize
 * rewrites the entry (the rows then move by the bytes the entry grew,
 * which is added to every offset).  A flex header that grows moves the
 * index and the rows alike, so it changes no offset.  The index costs 8
 * bytes a page and 4 to 8 a row: a page holding one row stays small, and
 * a full page's index is 8 + 4 * 64 bytes. */
static constexpr size_t IDROWS_PRESENT_BYTES = sizeof(uint64_t);
static constexpr size_t IDROWS_INDEX_MAX =
    IDROWS_PRESENT_BYTES + IDROWS_PAGE * sizeof(uint32_t);
static constexpr uint32_t IDROWS_CAPACITY_FIRST = 1;
static_assert(IDROWS_PAGE == 64, "a page's presence is one uint64_t");

typedef struct idRowsIndex {
    uint8_t *at;      /* the index's first byte, writable in place */
    uint64_t present; /* bit s: id s of the page is present */
    uint32_t rows;    /* offsets in use */
    uint32_t capacity;
} idRowsIndex;

static idRowsIndex idRowsIndexOf_(const flex *f) {
    databox t;
    flexGetByType(flexHead(f), &t);
    uint8_t *at = (uint8_t *)(uintptr_t)t.data.bytes.start;
    const uint64_t present = dkLoadLe64(at);
    return (idRowsIndex){
        .at = at,
        .present = present,
        .rows = (uint32_t)stdc_count_ones(present),
        .capacity =
            (uint32_t)((t.len - IDROWS_PRESENT_BYTES) / sizeof(uint32_t)),
    };
}

/* The place of 'slot' among the present slots below it */
static uint32_t idRowsRank_(const uint64_t present, const uint32_t slot) {
    return (uint32_t)stdc_count_ones(present & ((UINT64_C(1) << slot) - 1));
}

static uint32_t idRowsOffset_(const uint8_t *at, const uint32_t rank) {
    return dkLoadLe32(at + IDROWS_PRESENT_BYTES + rank * sizeof(uint32_t));
}

/* Offsets are 32-bit: a page whose rows pass 4 GB fails here rather than
 * wrapping */
static void idRowsSetOffset_(uint8_t *at, const uint32_t rank,
                             const int64_t offset) {
    assert(offset >= 0 && offset <= UINT32_MAX);
    dkStoreLe32(at + IDROWS_PRESENT_BYTES + rank * sizeof(uint32_t),
                (uint32_t)offset);
}

/* The bytes from the index's first byte to the flex's end: a change to
 * the rows, or to the index's size, moves it by exactly the bytes the
 * change added or removed past the index's start */
static size_t idRowsSpan_(const flex *f, const uint8_t *at) {
    return (size_t)((const uint8_t *)f + flexBytes(f) - at);
}

/* Give a page's index room for 'capacity' offsets (at least its rows) */
static void idRowsResize_(flex **ff, const uint32_t capacity) {
    const idRowsIndex ix = idRowsIndexOf_(*ff);
    assert(capacity >= ix.rows && capacity <= IDROWS_PAGE);
    uint8_t bytes[IDROWS_INDEX_MAX] = {};
    memcpy(bytes, ix.at, IDROWS_PRESENT_BYTES + ix.rows * sizeof(uint32_t));
    const size_t spanBefore = idRowsSpan_(*ff, ix.at);
    flexReplaceBytes(ff, flexHead(*ff), bytes,
                     IDROWS_PRESENT_BYTES + capacity * sizeof(uint32_t));

    const idRowsIndex after = idRowsIndexOf_(*ff);
    const int64_t delta =
        (int64_t)idRowsSpan_(*ff, after.at) - (int64_t)spanBefore;
    for (uint32_t k = 0; k < after.rows; k++) {
        idRowsSetOffset_(after.at, k,
                         (int64_t)idRowsOffset_(after.at, k) + delta);
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

void idRowsClear(idRows *r) {
    for (uint64_t i = 0; i < r->span; i++) {
        flexFree(r->page[i]);
    }

    zfree(r->page);
    zfree(r->pageNo);
    const uint32_t width = r->elementsPerRow;
    *r = (idRows){.elementsPerRow = width};
}

void idRowsFree(idRows *r) {
    if (!r) {
        return;
    }

    idRowsClear(r);
    zfree(r);
}

uint32_t idRowsElementsPerRow(const idRows *r) {
    return r->elementsPerRow;
}

size_t idRowsCount(const idRows *r) {
    return r->count;
}

/* The directory's own bytes: its arrays as allocated */
static size_t idRowsDirectoryBytes_(const idRows *r) {
    return r->cap * (sizeof(flex *) + (r->pageNo ? sizeof(uint64_t) : 0));
}

size_t idRowsBytes(const idRows *r) {
    size_t bytes = idRowsDirectoryBytes_(r);
    for (uint64_t i = 0; i < r->span; i++) {
        if (r->page[i]) {
            bytes += flexBytes(r->page[i]);
        }
    }

    return bytes;
}

/* ====================================================================
 * Directory
 * ==================================================================== */
/* The page number of directory entry i */
static uint64_t idRowsPageNumber_(const idRows *r, const uint64_t i) {
    return r->pageNo ? r->pageNo[i] : r->base + i;
}

/* Sparse: the first entry whose page number is at or past 'p' (span when
 * none) */
static uint64_t idRowsLowerBound_(const idRows *r, const uint64_t p) {
    uint64_t lo = 0;
    uint64_t hi = r->span;

    /* Interpolation first: sparse ids (a counter run far, a permanent
     * table with holes) spread about evenly, so a probe placed by value
     * lands near.  Each probe narrows [lo, hi) as a binary step would;
     * after IDROWS_INTERPOLATE probes, or once 'p' is outside the range's
     * values, the binary search finishes. */
    for (int probe = 0; probe < IDROWS_INTERPOLATE && hi - lo > 8; probe++) {
        const uint64_t first = r->pageNo[lo];
        const uint64_t last = r->pageNo[hi - 1];
        if (p <= first || p > last) {
            break;
        }

        const uint64_t at = lo + (uint64_t)((__uint128_t)(p - first) *
                                            (hi - 1 - lo) / (last - first));
        if (r->pageNo[at] < p) {
            lo = at + 1;
        } else {
            hi = at;
        }
    }

    while (lo < hi) {
        const uint64_t mid = lo + (hi - lo) / 2;
        if (r->pageNo[mid] < p) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    return lo;
}

/* The sparse directory's search, out of line so the dense read stays small
 * enough to inline into its callers */
[[gnu::noinline]] static flex **idRowsCellSparse_(const idRows *r,
                                                  const uint64_t p) {
    const uint64_t i = idRowsLowerBound_(r, p);
    return i < r->span && r->pageNo[i] == p ? &r->page[i] : nullptr;
}

/* The directory entry holding page number 'p', or NULL when it holds
 * no row */
DK_INLINE_ALWAYS flex **idRowsCell_(const idRows *r, const uint64_t p) {
    if (likely(!r->pageNo)) {
        /* Below 'base' the difference wraps past 'span' */
        const uint64_t i = p - r->base;
        return i < r->span && r->page[i] ? &r->page[i] : nullptr;
    }

    return idRowsCellSparse_(r, p);
}

/* Room for 'entries' directory entries */
static void idRowsReserve_(idRows *r, const uint64_t entries) {
    if (entries <= r->cap) {
        return;
    }

    uint64_t cap = r->cap ? r->cap * 2 : 4;
    if (cap < entries) {
        cap = entries;
    }

    zreallocSelf(r->page, cap * sizeof(flex *));
    if (r->pageNo) {
        zreallocSelf(r->pageNo, cap * sizeof(uint64_t));
    }

    r->cap = cap;
}

/* Give back directory room past a quarter in use */
static void idRowsShrink_(idRows *r) {
    if (r->span == 0) {
        zfree(r->page);
        zfree(r->pageNo);
        r->page = nullptr;
        r->pageNo = nullptr;
        r->base = r->cap = 0;
        return;
    }

    if (r->cap > 8 && r->span <= r->cap / 4) {
        r->cap /= 2;
        zreallocSelf(r->page, r->cap * sizeof(flex *));
        if (r->pageNo) {
            zreallocSelf(r->pageNo, r->cap * sizeof(uint64_t));
        }
    }
}

/* Rebuild the directory as sparse: only the pages holding rows */
static void idRowsToSparse_(idRows *r) {
    assert(!r->pageNo);
    const uint64_t cap = r->live ? r->live : 1;
    /* @own: container; free=idRowsFree */
    flex **page = zcalloc(cap, sizeof(flex *));
    /* @own: container; free=idRowsFree */
    uint64_t *pageNo = zcalloc(cap, sizeof(uint64_t));
    uint64_t n = 0;
    for (uint64_t i = 0; i < r->span; i++) {
        if (r->page[i]) {
            page[n] = r->page[i];
            pageNo[n++] = r->base + i;
        }
    }

    zfree(r->page);
    r->page = page;
    r->pageNo = pageNo;
    r->span = n;
    r->cap = cap;
    r->base = 0;
}

/* Rebuild the directory as dense over its pages' range */
static void idRowsToDense_(idRows *r) {
    assert(r->pageNo && r->span);
    const uint64_t base = r->pageNo[0];
    const uint64_t span = r->pageNo[r->span - 1] - base + 1;
    /* @own: container; free=idRowsFree */
    flex **page = zcalloc(span, sizeof(flex *));
    for (uint64_t i = 0; i < r->span; i++) {
        page[r->pageNo[i] - base] = r->page[i];
    }

    zfree(r->page);
    zfree(r->pageNo);
    r->page = page;
    r->pageNo = nullptr;
    r->base = base;
    r->span = span;
    r->cap = span;
}

/* Whether a dense directory of 'span' entries over 'live' pages is within
 * its bound */
static bool idRowsDenseFits_(const uint64_t span, const uint64_t live,
                             const uint64_t perLive) {
    return span <= perLive * live + IDROWS_DENSE_SLACK;
}

/* Add page 'f' as page number 'p', which holds no row yet */
static flex **idRowsAddPage_(idRows *r, const uint64_t p, flex *f) {
    if (!r->pageNo) {
        const uint64_t lo = r->span && r->base < p ? r->base : p;
        const uint64_t endOld = r->base + r->span;
        const uint64_t hi = r->span && endOld > p + 1 ? endOld : p + 1;
        if (idRowsDenseFits_(hi - lo, r->live + 1, IDROWS_DENSE_PER_LIVE)) {
            const uint64_t span = hi - lo;
            idRowsReserve_(r, span);
            if (r->span && lo < r->base) {
                /* Grow at the front: the entries move up */
                const uint64_t by = r->base - lo;
                memmove(r->page + by, r->page, r->span * sizeof(flex *));
                memset(r->page, 0, by * sizeof(flex *));
            }

            const uint64_t oldEnd = r->span ? endOld - lo : 0;
            memset(r->page + oldEnd, 0, (span - oldEnd) * sizeof(flex *));
            if (!r->span) {
                memset(r->page, 0, span * sizeof(flex *));
            }

            r->base = lo;
            r->span = span;
            r->page[p - lo] = f;
            r->live++;
            return &r->page[p - lo];
        }

        idRowsToSparse_(r);
    }

    idRowsReserve_(r, r->span + 1);
    const uint64_t i = idRowsLowerBound_(r, p);
    memmove(r->page + i + 1, r->page + i, (r->span - i) * sizeof(flex *));
    memmove(r->pageNo + i + 1, r->pageNo + i, (r->span - i) * sizeof(uint64_t));
    r->page[i] = f;
    r->pageNo[i] = p;
    r->span++;
    r->live++;
    if (idRowsDenseFits_(r->pageNo[r->span - 1] - r->pageNo[0] + 1, r->live,
                         IDROWS_SPARSE_BACK_PER_LIVE)) {
        idRowsToDense_(r);
        return idRowsCell_(r, p);
    }

    return &r->page[i];
}

/* Take out page number 'p' (its flex already freed by the caller) */
static void idRowsDropPage_(idRows *r, const uint64_t p) {
    r->live--;
    if (r->pageNo) {
        const uint64_t i = idRowsLowerBound_(r, p);
        assert(i < r->span && r->pageNo[i] == p);
        memmove(r->page + i, r->page + i + 1,
                (r->span - i - 1) * sizeof(flex *));
        memmove(r->pageNo + i, r->pageNo + i + 1,
                (r->span - i - 1) * sizeof(uint64_t));
        r->span--;
        if (r->span &&
            idRowsDenseFits_(r->pageNo[r->span - 1] - r->pageNo[0] + 1, r->live,
                             IDROWS_SPARSE_BACK_PER_LIVE)) {
            idRowsToDense_(r);
        }

        idRowsShrink_(r);
        return;
    }

    r->page[p - r->base] = nullptr;

    /* The ends hold rows: trim empty entries from both */
    uint64_t lead = 0;
    while (lead < r->span && !r->page[lead]) {
        lead++;
    }

    uint64_t end = r->span;
    while (end > lead && !r->page[end - 1]) {
        end--;
    }

    if (lead) {
        memmove(r->page, r->page + lead, (end - lead) * sizeof(flex *));
    }

    r->base += lead;
    r->span = end - lead;
    if (r->span && !idRowsDenseFits_(r->span, r->live, IDROWS_DENSE_PER_LIVE)) {
        idRowsToSparse_(r);
    }

    idRowsShrink_(r);
}

/* ====================================================================
 * Find
 * ==================================================================== */
/* A page's size and its span, before a row of it is rewritten in place */
typedef struct idRowsExtent {
    size_t bytes;
    size_t span;
} idRowsExtent;

/* The first entry of the row in 'slot' of page 'f'; NULL when the slot is
 * empty.  '*extentOut', when given, receives the page's extent. */
DK_INLINE_ALWAYS flexEntry *idRowsPageFind_(const flex *f, const uint32_t slot,
                                            idRowsExtent *extentOut) {
    const idRowsIndex ix = idRowsIndexOf_(f);
    if (!(ix.present >> slot & 1)) {
        return nullptr;
    }

    if (extentOut) {
        *extentOut = (idRowsExtent){.bytes = flexBytes(f),
                                    .span = idRowsSpan_(f, ix.at)};
    }

    return ix.at + idRowsOffset_(ix.at, idRowsRank_(ix.present, slot));
}

/* The first entry of the row of 'id', its page in '*pageOut'; NULL when
 * the id is absent */
DK_INLINE_ALWAYS flexEntry *idRowsFind_(const idRows *r, const uint64_t id,
                                        flex **pageOut) {
    flex **cell = idRowsCell_(r, id / IDROWS_PAGE);
    if (!cell) {
        return nullptr;
    }

    flexEntry *fe =
        idRowsPageFind_(*cell, (uint32_t)(id % IDROWS_PAGE), nullptr);
    if (fe) {
        *pageOut = *cell;
    }

    return fe;
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
/* The page for 'id', made (with an index of no row) when it has none */
static flex **idRowsPageFor_(idRows *r, const uint64_t id) {
    const uint64_t p = id / IDROWS_PAGE;
    flex **cell = idRowsCell_(r, p);
    if (cell) {
        return cell;
    }

    uint8_t none[IDROWS_PRESENT_BYTES +
                 IDROWS_CAPACITY_FIRST * sizeof(uint32_t)] = {};
    /* @own: container; free=idRowsFree */
    flex *f = flexNew();
    flexPushBytesExact(&f, none, sizeof(none), FLEX_ENDPOINT_TAIL);
    return idRowsAddPage_(r, p, f);
}

/* Insert 'row' as the row of 'slot', which is absent: before the next
 * present row of the page, or after the last */
static void idRowsInsertRow_(const idRows *r, flex **ff, const uint32_t slot,
                             const databox *row[]) {
    idRowsIndex ix = idRowsIndexOf_(*ff);
    if (ix.rows == ix.capacity) {
        /* The slot is absent, so rows < IDROWS_PAGE: doubling stays
         * within a page */
        idRowsResize_(ff, ix.capacity * 2);
        ix = idRowsIndexOf_(*ff);
    }

    /* The row goes where the next present row starts, or at the end */
    const uint32_t rank = idRowsRank_(ix.present, slot);
    const size_t spanBefore = idRowsSpan_(*ff, ix.at);
    const int64_t offset =
        rank < ix.rows ? idRowsOffset_(ix.at, rank) : (int64_t)spanBefore;
    if (rank < ix.rows) {
        /* Last element first at one position: they end in row order */
        for (uint32_t i = r->elementsPerRow; i-- > 0;) {
            flexInsertByType(ff, idRowsIndexOf_(*ff).at + offset, row[i]);
        }
    } else {
        for (uint32_t i = 0; i < r->elementsPerRow; i++) {
            flexPushByTypeExact(ff, row[i], FLEX_ENDPOINT_TAIL);
        }
    }

    /* The later rows move up one place and by the row's bytes */
    uint8_t *at = idRowsIndexOf_(*ff).at;
    const int64_t delta = (int64_t)idRowsSpan_(*ff, at) - (int64_t)spanBefore;
    for (uint32_t k = ix.rows; k > rank; k--) {
        idRowsSetOffset_(at, k, (int64_t)idRowsOffset_(at, k - 1) + delta);
    }

    idRowsSetOffset_(at, rank, offset);
    dkStoreLe64(at, ix.present | UINT64_C(1) << slot);
}

/* Remove the row of 'slot', which is present; false when the page is
 * left with no row (its index unchanged: the page is to be freed or
 * refilled) */
static bool idRowsDeleteRow_(const idRows *r, flex **ff, const uint32_t slot) {
    const idRowsIndex ix = idRowsIndexOf_(*ff);
    const uint32_t rank = idRowsRank_(ix.present, slot);
    const size_t spanBefore = idRowsSpan_(*ff, ix.at);
    flexEntry *fe = ix.at + idRowsOffset_(ix.at, rank);
    flexDeleteCount(ff, &fe, r->elementsPerRow);
    if (ix.rows == 1) {
        return false;
    }

    /* The later rows move down one place and by the row's bytes */
    uint8_t *at = idRowsIndexOf_(*ff).at;
    const int64_t delta = (int64_t)idRowsSpan_(*ff, at) - (int64_t)spanBefore;
    for (uint32_t k = rank; k + 1 < ix.rows; k++) {
        idRowsSetOffset_(at, k, (int64_t)idRowsOffset_(at, k + 1) + delta);
    }

    dkStoreLe64(at, ix.present & ~(UINT64_C(1) << slot));
    if (ix.capacity > IDROWS_CAPACITY_FIRST && ix.rows - 1 <= ix.capacity / 4) {
        idRowsResize_(ff, ix.capacity / 2);
    }

    return true;
}

bool idRowsSet(idRows *r, const uint64_t id, const databox *row[]) {
    flex **ff = idRowsPageFor_(r, id);
    const uint32_t slot = (uint32_t)(id % IDROWS_PAGE);
    const bool present = idRowsIndexOf_(*ff).present >> slot & 1;
    if (present && !idRowsDeleteRow_(r, ff, slot)) {
        /* The page's only row went: its index now holds no row */
        dkStoreLe64(idRowsIndexOf_(*ff).at, 0);
    }

    idRowsInsertRow_(r, ff, slot, row);
    if (!present) {
        r->count++;
    }

    return !present;
}

/* Element 'field' of the row of 'id', its page cell in '*ffOut', its
 * slot in '*slotOut' and the page's extent in '*extentOut'; NULL when the
 * id is absent */
static flexEntry *idRowsField_(idRows *r, const uint64_t id,
                               const uint32_t field, flex ***ffOut,
                               uint32_t *slotOut, idRowsExtent *extentOut) {
    assert(field < r->elementsPerRow);
    flex **cell = idRowsCell_(r, id / IDROWS_PAGE);
    if (!cell) {
        return nullptr;
    }

    const uint32_t slot = (uint32_t)(id % IDROWS_PAGE);
    flexEntry *fe = idRowsPageFind_(*cell, slot, extentOut);
    if (!fe) {
        return nullptr;
    }

    for (uint32_t i = 0; i < field; i++) {
        fe = flexNext(*cell, fe);
    }

    *ffOut = cell;
    *slotOut = slot;
    return fe;
}

/* After the row of 'slot' was rewritten in place: move the later rows'
 * offsets by the bytes gained or lost since the page had 'before'.  A
 * page of the same size kept every row's size, as most rewrites do. */
static void idRowsRowResized_(flex **ff, const uint32_t slot,
                              const idRowsExtent before) {
    if (flexBytes(*ff) == before.bytes) {
        return;
    }

    const idRowsIndex ix = idRowsIndexOf_(*ff);
    const int64_t delta =
        (int64_t)idRowsSpan_(*ff, ix.at) - (int64_t)before.span;
    for (uint32_t k = idRowsRank_(ix.present, slot) + 1; k < ix.rows; k++) {
        idRowsSetOffset_(ix.at, k, (int64_t)idRowsOffset_(ix.at, k) + delta);
    }
}

bool idRowsReplaceField(idRows *r, const uint64_t id, const uint32_t field,
                        const databox *box) {
    flex **ff;
    uint32_t slot;
    idRowsExtent before;
    flexEntry *fe = idRowsField_(r, id, field, &ff, &slot, &before);
    if (!fe) {
        return false;
    }

    flexReplaceByType(ff, fe, box);
    idRowsRowResized_(ff, slot, before);
    return true;
}

bool idRowsFieldIncr(idRows *r, const uint64_t id, const uint32_t field,
                     const int64_t incrBy, int64_t *newValue) {
    flex **ff;
    uint32_t slot;
    idRowsExtent before;
    flexEntry *fe = idRowsField_(r, id, field, &ff, &slot, &before);
    if (!fe) {
        if (newValue) {
            *newValue = 0;
        }

        return false;
    }

    int64_t sum = 0;
    if (!flexIncrbySigned(ff, fe, incrBy, &sum)) {
        if (newValue) {
            databox current;
            flexGetByTypeInto(fe, &current, nullptr);
            *newValue = databoxIsWideDeferred(&current) ? 0 : current.data.i;
        }

        return false;
    }

    idRowsRowResized_(ff, slot, before);
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

    const uint64_t p = id / IDROWS_PAGE;
    flex **ff = idRowsCell_(r, p);
    r->count--;
    if (!idRowsDeleteRow_(r, ff, (uint32_t)(id % IDROWS_PAGE))) {
        /* A page left with no row goes */
        flexFree(*ff);
        idRowsDropPage_(r, p);
    }

    return true;
}

/* ====================================================================
 * Order
 * ==================================================================== */
/* The directory's ends hold rows (dense trims empty ends; sparse holds
 * only pages with rows), and a page holding rows has a presence that is
 * never 0 */
bool idRowsFirst(const idRows *r, uint64_t *id) {
    if (!r->span) {
        return false;
    }

    const uint64_t present = idRowsIndexOf_(r->page[0]).present;
    *id = idRowsPageNumber_(r, 0) * IDROWS_PAGE + stdc_trailing_zeros(present);
    return true;
}

bool idRowsLast(const idRows *r, uint64_t *id) {
    if (!r->span) {
        return false;
    }

    const uint64_t i = r->span - 1;
    const uint64_t present = idRowsIndexOf_(r->page[i]).present;
    *id = idRowsPageNumber_(r, i) * IDROWS_PAGE + (IDROWS_PAGE - 1) -
          stdc_leading_zeros(present);
    return true;
}

void idRowsIteratorInit(const idRows *r, idRowsIterator *iter) {
    *iter = (idRowsIterator){.r = r, .next = 0, .ended = false};
}

bool idRowsIteratorNext(idRowsIterator *iter, uint64_t *id, databox *row[],
                        databoxWide *slots) {
    const idRows *r = iter->r;
    if (iter->ended) {
        return false;
    }

    /* The first directory entry at or past the next id's page */
    const uint64_t nextPage = iter->next / IDROWS_PAGE;
    uint64_t i;
    if (r->pageNo) {
        i = idRowsLowerBound_(r, nextPage);
    } else {
        i = nextPage > r->base ? nextPage - r->base : 0;
    }

    for (; i < r->span; i++) {
        flex *f = r->page[i];
        if (!f) {
            continue;
        }

        /* Its present slots at or after the next id */
        const uint64_t p = idRowsPageNumber_(r, i);
        const uint32_t from =
            p == nextPage ? (uint32_t)(iter->next % IDROWS_PAGE) : 0;
        const idRowsIndex ix = idRowsIndexOf_(f);
        const uint64_t ahead = ix.present & (UINT64_MAX << from);
        if (ahead) {
            const uint32_t slot = stdc_trailing_zeros(ahead);
            *id = p * IDROWS_PAGE + slot;
            /* The highest id has no next one */
            iter->ended = *id == UINT64_MAX;
            iter->next = *id + 1;
            idRowsDecode_(
                r, f,
                ix.at + idRowsOffset_(ix.at, idRowsRank_(ix.present, slot)),
                row, slots);
            return true;
        }
    }

    iter->ended = true;
    return false;
}

/* ====================================================================
 * Persistence (persistOpsIdRows; the format is described in persist.h)
 * ==================================================================== */
/* Scratch for one row of 'width' elements: boxes, the wide storage a
 * decode needs and the pointer arrays the row calls take */
typedef struct idRowsPersistRow_ {
    databoxWide *wide;
    databox *boxes;
    databox **into;
    const databox **from;
} idRowsPersistRow_;

/* Carve the scratch for 'cols' boxes from 'block' (zcalloc'd by
 * idRowsPersistRowBytes_(cols)), widest alignment first */
static size_t idRowsPersistRowBytes_(const size_t cols) {
    return cols * (sizeof(databoxWide) + sizeof(databox) + sizeof(databox *) +
                   sizeof(const databox *));
}

static idRowsPersistRow_ idRowsPersistRowOf_(void *block, const size_t cols) {
    uint8_t *at = block;
    idRowsPersistRow_ row = {.wide = (databoxWide *)at};
    at += cols * sizeof(databoxWide);
    row.boxes = (databox *)at;
    at += cols * sizeof(databox);
    row.into = (databox **)at;
    at += cols * sizeof(databox *);
    row.from = (const databox **)at;
    for (size_t i = 0; i < cols; i++) {
        row.into[i] = &row.boxes[i];
        row.from[i] = &row.boxes[i];
    }

    return row;
}

static uint8_t *idRowsPersistSnapshot_(const void *structure, size_t *len) {
    const idRows *r = structure;
    /* @own: caller; free=zfree */
    flex *f = flexNew();
    const databox width = DATABOX_UNSIGNED(r->elementsPerRow);
    const databox rows = DATABOX_UNSIGNED(r->count);
    const databox *header[2] = {&width, &rows};
    flexAppendMultipleExact(&f, 2, header);

    /* Each row as {id, elements}: element i decodes into box i + 1 */
    const size_t cols = (size_t)r->elementsPerRow + 1;
    cleanerName(zfree, Void) void *block =
        zcalloc(1, idRowsPersistRowBytes_(cols));
    const idRowsPersistRow_ row = idRowsPersistRowOf_(block, cols);
    idRowsIterator iter;
    idRowsIteratorInit(r, &iter);
    while (true) {
        /* A narrow decode leaves the flag bits as they were: zero them */
        for (size_t i = 0; i < cols; i++) {
            row.boxes[i] = (databox){};
        }

        uint64_t id = 0;
        if (!idRowsIteratorNext(&iter, &id, row.into + 1, row.wide + 1)) {
            break;
        }

        row.boxes[0] = DATABOX_UNSIGNED(id);
        flexAppendMultipleExact(&f, (uint_fast32_t)cols, row.from);
    }

    *len = flexBytes(f);
    return (uint8_t *)f;
}

/* Decode 'width' entries from 'fe' on into the row's boxes (wide values
 * into its wide storage); the entries are known to be there */
static void idRowsPersistDecode_(const flex *f, flexEntry *fe,
                                 const idRowsPersistRow_ *row,
                                 const size_t width) {
    for (size_t i = 0; i < width; i++) {
        row->boxes[i] = (databox){};
        flexGetByTypeInto(fe, &row->boxes[i], &row->wide[i]);
        fe = flexNext(f, fe);
    }
}

static void *idRowsPersistRestore_(const uint8_t *data, size_t len) {
    if (!flexValidateBufferDeep(data, len)) {
        return nullptr;
    }

    const flex *f = (const flex *)data;
    const size_t n = flexCount(f);
    flexEntry *fe = flexHead(f);
    uint64_t width;
    uint64_t rows;
    if (n < 2 || !flexReadUnsigned(f, &fe, &width) ||
        !flexReadUnsigned(f, &fe, &rows) || width == 0 || width >= UINT32_MAX ||
        rows > (n - 2) / (width + 1) || n - 2 != rows * (width + 1)) {
        return nullptr;
    }

    cleaner(idRowsFree) idRows *r = idRowsNew((uint32_t)width);
    cleanerName(zfree, Void) void *block =
        zcalloc(1, idRowsPersistRowBytes_((size_t)width));
    const idRowsPersistRow_ row = idRowsPersistRowOf_(block, (size_t)width);
    uint64_t previous = 0;
    for (uint64_t k = 0; k < rows; k++) {
        /* Ids strictly ascend, as a snapshot writes them */
        uint64_t id;
        if (!flexReadUnsigned(f, &fe, &id) || (k && id <= previous)) {
            return nullptr;
        }

        idRowsPersistDecode_(f, fe, &row, (size_t)width);
        for (uint64_t i = 0; i < width; i++) {
            fe = flexNext(f, fe);
        }

        idRowsSet(r, id, row.from);
        previous = id;
    }

    idRows *out = r;
    r = nullptr;
    return out;
}

static uint8_t *idRowsPersistEncodeOp_(persistOp op, const void *args,
                                       size_t argc, size_t *len) {
    *len = 0;
    switch (op) {
    case PERSIST_OP_CLEAR:
        return argc == 0 ? persistOpNoBody(len) : nullptr;
    case PERSIST_OP_INSERT:
        if (argc < 2) {
            return nullptr;
        }
        break;
    case PERSIST_OP_UPDATE:
    case PERSIST_OP_NUM_INCR:
        if (argc != 3) {
            return nullptr;
        }
        break;
    case PERSIST_OP_DELETE:
        if (argc != 1) {
            return nullptr;
        }
        break;
    default:
        return nullptr;
    }

    return persistOpEncodeBoxes(args, argc, len);
}

static bool idRowsPersistApplyOp_(void *structure, persistOp op,
                                  const uint8_t *data, size_t len) {
    idRows *r = *(idRows **)structure;
    if (op == PERSIST_OP_CLEAR) {
        if (len) {
            return false;
        }

        idRowsClear(r);
        return true;
    }

    if (!flexValidateBufferDeep(data, len)) {
        return false;
    }

    /* Every other op leads with the row's id */
    const flex *f = (const flex *)data;
    const size_t n = flexCount(f);
    flexEntry *fe = flexHead(f);
    uint64_t id;
    if (n == 0 || !flexReadUnsigned(f, &fe, &id)) {
        return false;
    }

    const size_t width = r->elementsPerRow;
    switch (op) {
    case PERSIST_OP_INSERT: {
        if (n != width + 1) {
            return false;
        }

        cleanerName(zfree, Void) void *block =
            zcalloc(1, idRowsPersistRowBytes_(width));
        const idRowsPersistRow_ row = idRowsPersistRowOf_(block, width);
        idRowsPersistDecode_(f, fe, &row, width);
        idRowsSet(r, id, row.from);
        return true;
    }
    case PERSIST_OP_UPDATE: {
        uint64_t field;
        if (n != 3 || !flexReadUnsigned(f, &fe, &field) || field >= width) {
            return false;
        }

        databox value = {};
        databoxWide wide;
        flexGetByTypeInto(fe, &value, &wide);
        return idRowsReplaceField(r, id, (uint32_t)field, &value);
    }
    case PERSIST_OP_NUM_INCR: {
        uint64_t field;
        int64_t by;
        if (n != 3 || !flexReadUnsigned(f, &fe, &field) || field >= width ||
            !flexGetSigned(fe, &by)) {
            return false;
        }

        return idRowsFieldIncr(r, id, (uint32_t)field, by, nullptr);
    }
    case PERSIST_OP_DELETE:
        return n == 1 && idRowsDelete(r, id);
    default:
        return false;
    }
}

static size_t idRowsPersistCount_(const void *structure) {
    return idRowsCount(structure);
}

static size_t idRowsPersistBytes_(const void *structure) {
    return idRowsBytes(structure);
}

/* The directory and the pages agree: every page in it holds rows, the
 * live page count and the row count match the pages, and a dense
 * directory's ends hold rows */
static bool idRowsPersistValidate_(const void *structure) {
    const idRows *r = structure;
    uint64_t live = 0;
    uint64_t rows = 0;
    for (uint64_t i = 0; i < r->span; i++) {
        if (!r->page[i]) {
            if (r->pageNo || i == 0 || i + 1 == r->span) {
                return false;
            }
            continue;
        }

        const idRowsIndex ix = idRowsIndexOf_(r->page[i]);
        if (!ix.present || ix.rows > ix.capacity ||
            (r->pageNo && i && r->pageNo[i] <= r->pageNo[i - 1])) {
            return false;
        }

        live++;
        rows += ix.rows;
    }

    return live == r->live && rows == r->count;
}

static void idRowsPersistFree_(void *structure) {
    idRowsFree(structure);
}

/* The width is fixed at creation, so there is no newEmpty */
const persistOps persistOpsIdRows = {
    .type = PERSIST_TYPE_ID_ROWS,
    .name = "idRows",
    .formatVersion = 1,
    .minReadVersion = 1,
    .snapshot = idRowsPersistSnapshot_,
    .restore = idRowsPersistRestore_,
    .count = idRowsPersistCount_,
    .estimateSize = idRowsPersistBytes_,
    .encodeOp = idRowsPersistEncodeOp_,
    .applyOp = idRowsPersistApplyOp_,
    .validate = idRowsPersistValidate_,
    .free = idRowsPersistFree_,
};

/* ====================================================================
 * Tests
 * ==================================================================== */
#ifdef DATAKIT_TEST
#include "ctest.h"
#include "multimap.h"

defcleaner(multimap *, multimapFree);

/* Test row value 'v' of a mix: small and large integers, 128-bit values,
 * and byte strings up to 3000 bytes (so a page passes 64 KB) */
static databox idRowsTestValue_(uint64_t v, uint8_t *bytes, size_t cap,
                                databoxBig *wide) {
    switch (v % 5) {
    case 0:
        return databoxNewSigned((int64_t)(v % 300) - 150);
    case 1:
        return databoxNewUnsigned(v * UINT64_C(0x9E3779B97F4A7C15));
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

/* Which ids a differential run draws */
typedef enum idRowsTestIds {
    ID_ROWS_TEST_IDS_DENSE = 0, /* 0..699 */
    /* Up to 4 ids in each of the pages 0-9 and 80-89 (pages empty and
     * refill, so the directory crosses its dense bound both ways), and
     * now and then ids at 2^63 and at the top of the range */
    ID_ROWS_TEST_IDS_SPREAD,
} idRowsTestIds;

static uint64_t idRowsTestId_(uint64_t *rng, const idRowsTestIds ids) {
    if (ids == ID_ROWS_TEST_IDS_DENSE) {
        return dkSplitmix64Bounded(rng, 700);
    }

    const uint64_t pick = dkSplitmix64Bounded(rng, 200);
    if (pick == 0) {
        return UINT64_MAX - dkSplitmix64Bounded(rng, 2);
    }

    if (pick == 1) {
        return (UINT64_C(1) << 63) + dkSplitmix64Bounded(rng, 3);
    }

    const uint64_t page = dkSplitmix64Bounded(rng, 2)
                              ? dkSplitmix64Bounded(rng, 10)
                              : 80 + dkSplitmix64Bounded(rng, 10);
    return page * IDROWS_PAGE + dkSplitmix64Bounded(rng, 4);
}

/* 20000 random sets, field replaces, increments and deletes against a
 * multimap {id, a, b} oracle: every op checks presence and results, and
 * every 2000 ops the whole order, first and last are compared.  Emptied,
 * the rows hold no byte, the directory included.  A spread run must see
 * the directory in both forms. */
static int idRowsTestDifferential_(const idRowsTestIds ids, uint64_t rng) {
    int err = 0;
    cleaner(idRowsFree) idRows *r = idRowsNew(2);
    cleaner(multimapFree) multimap *oracle = multimapNew(3);
    static uint8_t bytes[2][3000];
    typedef enum idRowsTestOp {
        ID_ROWS_TEST_SET = 0,
        ID_ROWS_TEST_REPLACE_FIELD,
        ID_ROWS_TEST_INCR,
        ID_ROWS_TEST_DELETE,
        ID_ROWS_TEST_OP_COUNT
    } idRowsTestOp;
    bool sawDense = false;
    bool sawSparse = false;
    for (int op = 0; op < 20000; op++) {
        const uint64_t id = idRowsTestId_(&rng, ids);
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

        if (r->span) {
            sawDense |= r->pageNo == nullptr;
            sawSparse |= r->pageNo != nullptr;
        }

        if (op % 2000 == 1999) {
            err += idRowsTestAgree_(r, oracle, "during");
        }
    }

    err += idRowsTestAgree_(r, oracle, "after");

    /* The directory is bounded by the pages holding rows (twice its
     * densest form's entries, for growth room), never by the ids */
    const size_t bound = 2 * (sizeof(flex *) + sizeof(uint64_t)) *
                         (IDROWS_DENSE_PER_LIVE * r->live + IDROWS_DENSE_SLACK);
    if (idRowsDirectoryBytes_(r) > bound) {
        ERR("directory %zu bytes for %" PRIu64 " pages (bound %zu)",
            idRowsDirectoryBytes_(r), r->live, bound);
    }

    if (ids == ID_ROWS_TEST_IDS_SPREAD && !(sawDense && sawSparse)) {
        ERR("the directory was dense %d, sparse %d", sawDense, sawSparse);
    }

    /* Emptied, every page and the directory go */
    uint64_t first = 0;
    while (idRowsFirst(r, &first)) {
        idRowsDelete(r, first);
    }

    if (idRowsCount(r) != 0 || idRowsBytes(r) != 0) {
        ERR("emptied: %zu rows, %zu bytes", idRowsCount(r), idRowsBytes(r));
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
         "multimap: dense ids") {
        err += idRowsTestDifferential_(ID_ROWS_TEST_IDS_DENSE, 0x1D5);
    }

    TEST("the same over spread ids: clustered pages, 2^63 and the top ids; "
         "the directory turns sparse and back") {
        err += idRowsTestDifferential_(ID_ROWS_TEST_IDS_SPREAD, 0x5B2);
    }

    TEST("a sliding window of ids keeps a small directory") {
        /* 2000 live ids moving up 1M ids: every page behind the window
         * goes, so the directory stays dense over the window alone */
        cleaner(idRowsFree) idRows *r = idRowsNew(1);
        constexpr uint64_t window = 2000;
        constexpr uint64_t travel = 1000000;
        for (uint64_t id = 0; id < travel + window; id++) {
            const databox v = databoxNewUnsigned(id);
            const databox *row[1] = {&v};
            idRowsSet(r, id, row);
            if (id >= window) {
                idRowsDelete(r, id - window);
            }
        }

        uint64_t first = 0;
        uint64_t last = 0;
        if (idRowsCount(r) != window || !idRowsFirst(r, &first) ||
            first != travel || !idRowsLast(r, &last) ||
            last != travel + window - 1 || r->pageNo ||
            idRowsDirectoryBytes_(r) >
                8 * (window / IDROWS_PAGE + 2) * sizeof(flex *)) {
            ERR("window: %zu rows, first %" PRIu64 ", directory %zu bytes",
                idRowsCount(r), first, idRowsDirectoryBytes_(r));
        }
    }

    TEST("a sparse page costs its index only for the rows it holds") {
        /* One row in each of 100 pages: per row, one flex of a 2-byte
         * header, the index entry (8 bytes of presence + one 4-byte
         * offset, between 1-byte length varints: 14) and the row (a
         * 1-byte integer entry, 3, and an immediate, 1), beside the page
         * pointers.  A full page adds 4 bytes of index per row. */
        cleaner(idRowsFree) idRows *sparse = idRowsNew(2);
        cleaner(idRowsFree) idRows *dense = idRowsNew(2);
        const databox one = databoxNewUnsigned(7);
        const databox flag = {.type = DATABOX_FALSE};
        const databox *row[2] = {&one, &flag};
        constexpr uint64_t pages = 100;
        for (uint64_t p = 0; p < pages; p++) {
            idRowsSet(sparse, p * IDROWS_PAGE + 5, row);
        }

        for (uint64_t id = 0; id < IDROWS_PAGE; id++) {
            idRowsSet(dense, id, row);
        }

        const size_t perSparse =
            (idRowsBytes(sparse) - idRowsDirectoryBytes_(sparse)) / pages;
        const size_t perDensePage =
            idRowsBytes(dense) - idRowsDirectoryBytes_(dense);
        if (perSparse != 2 + 14 + 3 + 1) {
            ERR("a sparse row costs %zu bytes", perSparse);
        }

        /* At most a 4-byte header, the 264-byte index (8 + 4 * 64)
         * between 2-byte varints, and 64 rows of 4 bytes */
        if (perDensePage > 4 + 264 + 4 + IDROWS_PAGE * 4) {
            ERR("a full page costs %zu bytes", perDensePage);
        }

        /* Emptied to one row, the full page's offsets shrink back (the
         * capacity halves at a quarter used, so one row keeps room for 2:
         * 4 bytes past a single row's page) */
        for (uint64_t id = 1; id < IDROWS_PAGE; id++) {
            idRowsDelete(dense, id);
        }

        const size_t perShrunk =
            idRowsBytes(dense) - idRowsDirectoryBytes_(dense);
        if (perShrunk != 2 + 14 + 4 + 3 + 1) {
            ERR("a page emptied to one row costs %zu bytes", perShrunk);
        }

        uint64_t first = 0, last = 0;
        if (!idRowsFirst(sparse, &first) || first != 5 ||
            !idRowsLast(sparse, &last) ||
            last != (pages - 1) * IDROWS_PAGE + 5) {
            ERR("first %" PRIu64 " last %" PRIu64, first, last);
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

    TEST("a full page's rows read back while one count grows through every "
         "integer width and shrinks back") {
        cleaner(idRowsFree) idRows *r = idRowsNew(2);
        for (uint64_t id = 0; id < IDROWS_PAGE; id++) {
            const databox name = databoxNewSigned((int64_t)id * 1000);
            const databox count = databoxNewSigned((int64_t)id);
            const databox *row[2] = {&name, &count};
            idRowsSet(r, id, row);
        }

        /* The count of row 37 grows to 1, 2, 4, 5 and 8 bytes (with steps
         * that keep its width between), turns negative, and returns to 37 */
        constexpr uint64_t grown = 37;
        static const int64_t steps[] = {
            1,  200,       -150, 70000,
            1,  INT32_MAX, 5,    INT64_C(1) << 40,
            -9, 0,         -1,   -(INT64_C(1) << 41)};
        int64_t expect = (int64_t)grown;
        for (size_t s = 0; s <= countof(steps); s++) {
            const int64_t by =
                s < countof(steps) ? steps[s] : (int64_t)grown - expect;
            int64_t now = 0;
            if (!idRowsFieldIncr(r, grown, 1, by, &now) || now != expect + by) {
                ERR("step %zu: incr by %" PRId64 " gave %" PRId64, s, by, now);
                break;
            }
            expect += by;

            for (uint64_t id = 0; id < IDROWS_PAGE; id++) {
                databox name = {};
                databox count = {};
                databox *row[2] = {&name, &count};
                const int64_t want = id == grown ? expect : (int64_t)id;
                if (!idRowsGet(r, id, row) ||
                    name.data.i != (intmax_t)id * 1000 ||
                    count.data.i != want) {
                    ERR("step %zu: row %" PRIu64 " reads %" PRIdMAX
                        " / %" PRIdMAX,
                        s, id, name.data.i, count.data.i);
                    break;
                }
            }
        }
    }

    TEST_FINAL_RESULT;
}
#endif
