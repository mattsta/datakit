/* ====================================================================
 * 2D Fenwick Tree Typed Template: the public API of one value type
 * ====================================================================
 *
 * Included by each type's source (fenwick2dI16.c ... fenwick2dDouble.c),
 * after that type's header, with:
 *   F2D_SUFFIX       - I16, I32, I64, I128, U16, U32, U64, U128, Float,
 *                      Double
 *   F2D_VALUE_T      - the C value type
 *   F2D_IS_FLOATING  - 1 for a float type
 * Cells are the value type; integer cells add with wrapping (exact for
 * every representable sum), elements are checked before any write.
 * Contract: fenwick2d.h. */

#include <math.h>

#define F2D_CAT2_(a, b) a##b
#define F2D_CAT_(a, b) F2D_CAT2_(a, b)
#define F2D_PUB_(name) F2D_CAT_(F2D_CAT_(fenwick2d, F2D_SUFFIX), name)
#define F2D_T_ F2D_CAT_(fenwick2d, F2D_SUFFIX)

static inline F2D_VALUE_T F2D_PUB_(Add_)(F2D_VALUE_T a, F2D_VALUE_T b) {
#if F2D_IS_FLOATING
    return a + b;
#else
    F2D_VALUE_T r;
    (void)ckd_add(&r, a, b); /* wraps by design */
    return r;
#endif
}

static inline F2D_VALUE_T F2D_PUB_(Sub_)(F2D_VALUE_T a, F2D_VALUE_T b) {
#if F2D_IS_FLOATING
    return a - b;
#else
    F2D_VALUE_T r;
    (void)ckd_sub(&r, a, b); /* wraps by design */
    return r;
#endif
}

/* The cell store: one row-major array of values */
typedef struct F2D_PUB_(Store_) {
    F2D_VALUE_T *cells;
} F2D_PUB_(Store_);

/* Re-lay oldR x oldC cells out as newR x newC (each at least the old
 * one), the new cells zero.  OOM is fail-fast. */
static void F2D_PUB_(Relayout_)(F2D_PUB_(Store_) * s, uint64_t oldR,
                                uint64_t oldC, uint64_t newR, uint64_t newC) {
    if (newC == oldC) {
        /* @own: container; free=zfree */
        s->cells = zrealloc(s->cells, newR * newC * sizeof(*s->cells));
        memset(&s->cells[oldR * oldC], 0,
               (newR - oldR) * newC * sizeof(*s->cells));
        return;
    }

    /* @own: container; free=zfree */
    F2D_VALUE_T *cells = zcalloc(newR * newC, sizeof(*cells));
    for (uint64_t r = 0; r < oldR; r++) {
        memcpy(&cells[r * newC], &s->cells[r * oldC], oldC * sizeof(*cells));
    }

    zfree(s->cells);
    s->cells = cells;
}

#define F2D_FN(name) F2D_PUB_(name)
#define F2D_STRUCT F2D_T_
#define F2D_STORE_T F2D_PUB_(Store_)
#define F2D_PLAN_T F2D_VALUE_T
#define F2D_NUM_T F2D_VALUE_T
#define F2D_NUM_ZERO ((F2D_VALUE_T)0)
#define F2D_NUM_ADD(a, b) F2D_PUB_(Add_)(a, b)
#define F2D_NUM_SUB(a, b) F2D_PUB_(Sub_)(a, b)
#define F2D_LOAD(s, idx) ((s)->cells[idx])
#define F2D_ENCODE(num, out) (*(out) = (num), true)
#define F2D_PUT(s, n, idx, plan) ((void)(n), (s)->cells[idx] = (plan))
#define F2D_COPY(s, dst, src) ((s)->cells[dst] = (s)->cells[src])
#define F2D_RELAYOUT(s, oR, oC, nR, nC) F2D_PUB_(Relayout_)(s, oR, oC, nR, nC)
#define F2D_CLEAR(s, n)                                                        \
    ((s)->cells ? (void)memset((s)->cells, 0, (n) * sizeof(*(s)->cells))       \
                : (void)0)
#define F2D_BYTES(s, n) ((n) * sizeof(*(s)->cells))
#define F2D_FREE(s) zfree((s)->cells)
#include "fenwick2dCoreImpl.h"
#undef F2D_FN
#undef F2D_STRUCT
#undef F2D_STORE_T
#undef F2D_PLAN_T
#undef F2D_NUM_T
#undef F2D_NUM_ZERO
#undef F2D_NUM_ADD
#undef F2D_NUM_SUB
#undef F2D_LOAD
#undef F2D_ENCODE
#undef F2D_PUT
#undef F2D_COPY
#undef F2D_RELAYOUT
#undef F2D_CLEAR
#undef F2D_BYTES
#undef F2D_FREE

F2D_T_ *F2D_PUB_(New)(void) {
    /* @own: caller */
    return zcalloc(1, sizeof(F2D_T_));
}

void F2D_PUB_(Free)(F2D_T_ *t) {
    if (t) {
        F2D_PUB_(FreeStore_)(t);
        zfree(t);
    }
}

/* Element (row, col), zero outside the extents */
static F2D_VALUE_T F2D_PUB_(ElementOrZero_)(const F2D_T_ *t, uint64_t row,
                                            uint64_t col) {
    if (!t || row >= t->rows || col >= t->cols) {
        return 0;
    }

    return F2D_PUB_(Element_)(t, row, col);
}

/* Add 'delta' at (row, col), creating the tree when *t is NULL and
 * freeing it again when the write is refused */
static bool F2D_PUB_(WriteOrCreate_)(F2D_T_ **t, size_t row, size_t col,
                                     F2D_VALUE_T delta) {
    const bool created = !*t;
    if (created) {
        /* @own: caller */
        *t = F2D_PUB_(New)();
    }

    if (!F2D_PUB_(Write_)(*t, row, col, delta)) {
        if (created) {
            F2D_PUB_(Free)(*t);
            *t = nullptr;
        }

        return false;
    }

    return true;
}

bool F2D_PUB_(Update)(F2D_T_ **t, size_t row, size_t col, F2D_VALUE_T delta) {
    if (!t) {
        return false;
    }

    const F2D_VALUE_T old = F2D_PUB_(ElementOrZero_)(*t, row, col);
    F2D_VALUE_T element;
#if F2D_IS_FLOATING
    element = old + delta;
    if (!isfinite(delta) || !isfinite(element)) {
        return false;
    }
#else
    if (ckd_add(&element, old, delta)) {
        return false;
    }
#endif

    return F2D_PUB_(WriteOrCreate_)(t, row, col, delta);
}

bool F2D_PUB_(Set)(F2D_T_ **t, size_t row, size_t col, F2D_VALUE_T value) {
    if (!t) {
        return false;
    }

    const F2D_VALUE_T delta =
        F2D_PUB_(Sub_)(value, F2D_PUB_(ElementOrZero_)(*t, row, col));
#if F2D_IS_FLOATING
    if (!isfinite(value) || !isfinite(delta)) {
        return false;
    }
#endif

    return F2D_PUB_(WriteOrCreate_)(t, row, col, delta);
}

F2D_VALUE_T F2D_PUB_(Get)(const F2D_T_ *t, size_t row, size_t col) {
    return F2D_PUB_(ElementOrZero_)(t, row, col);
}

F2D_VALUE_T F2D_PUB_(Query)(const F2D_T_ *t, size_t row, size_t col) {
    return t ? F2D_PUB_(Prefix_)(t, row, col) : 0;
}

F2D_VALUE_T F2D_PUB_(RangeQuery)(const F2D_T_ *t, size_t r1, size_t c1,
                                 size_t r2, size_t c2) {
    if (!t || t->rows == 0 || t->cols == 0) {
        return 0;
    }

    r2 = r2 < t->rows ? r2 : t->rows - 1;
    c2 = c2 < t->cols ? c2 : t->cols - 1;
    if (r1 > r2 || c1 > c2) {
        return 0;
    }

    return F2D_PUB_(Rect_)(t, r1, c1, r2, c2);
}

size_t F2D_PUB_(Rows)(const F2D_T_ *t) {
    return t ? t->rows : 0;
}

size_t F2D_PUB_(Cols)(const F2D_T_ *t) {
    return t ? t->cols : 0;
}

size_t F2D_PUB_(Bytes)(const F2D_T_ *t) {
    return t ? F2D_PUB_(Bytes_)(t) : 0;
}

void F2D_PUB_(Clear)(F2D_T_ *t) {
    if (t) {
        F2D_PUB_(Clear_)(t);
    }
}

/* ====================================================================
 * Bulk build, element copy and persistence
 * ==================================================================== */
#include "../cleaner.h"
#include "../flex.h"
#include "../persist.h"
#include "fenwickPersist.h" /* the persistOps declarations defined below */

defcleanerName(F2D_VALUE_T *, zfree, F2dValues);

#define FPV_T F2D_VALUE_T
#define FPV_IS_FLOATING F2D_IS_FLOATING
#define FPV_FN(name) F2D_PUB_(F2D_CAT_(Persist, name))
#include "fenwickPersistValueImpl.h"

/* The largest cell count (rowCap * colCap) a tree reaches */
#define F2D_P_MAX_CELLS_ (UINT64_C(1) << 32)
#define F2D_P_STR2_(x) #x
#define F2D_P_STR_(x) F2D_P_STR2_(x)

/* A tree whose extents are rows x cols (both zero or neither), its cells
 * zero, at the capacities writes would have grown it to; NULL past the
 * cell limit */
static F2D_T_ *F2D_PUB_(NewExtents_)(uint64_t rows, uint64_t cols) {
    /* @own: caller */
    F2D_T_ *t = F2D_PUB_(New)();
    if (rows == 0 || cols == 0) {
        return t;
    }

    uint64_t rowCap;
    uint64_t colCap;
    if (!F2D_PUB_(CapsFor_)(t, rows - 1, cols - 1, &rowCap, &colCap)) {
        F2D_PUB_(Free)(t);
        return nullptr;
    }

    /* @own: container; free=zfree */
    t->store.cells = zcalloc(rowCap * colCap, sizeof(F2D_VALUE_T));
    t->rowCap = rowCap;
    t->colCap = colCap;
    t->rows = rows;
    t->cols = cols;
    return t;
}

F2D_T_ *F2D_PUB_(NewFromArray)(const F2D_VALUE_T *values, size_t rows,
                               size_t cols) {
    if (!values || rows == 0 || cols == 0) {
        return F2D_PUB_(New)();
    }

#if F2D_IS_FLOATING
    for (size_t i = 0; i < rows * cols; i++) {
        if (!isfinite(values[i])) {
            return nullptr;
        }
    }
#endif

    F2D_T_ *t = F2D_PUB_(NewExtents_)(rows, cols);
    if (!t) {
        return nullptr;
    }

    F2D_VALUE_T *cells = t->store.cells;
    const uint64_t rowCap = t->rowCap;
    const uint64_t colCap = t->colCap;
    for (uint64_t r = 0; r < rows; r++) {
        memcpy(&cells[r * colCap], &values[r * cols], cols * sizeof(*cells));
    }

    /* The 1D build along every row, then along the columns (each row,
     * final once reached in order, is added into its parent row) */
    for (uint64_t i = 0; i < rowCap; i++) {
        F2D_VALUE_T *row = &cells[i * colCap];
        for (uint64_t j = 1; j <= colCap; j++) {
            const uint64_t p = fenwickParent(j);
            if (p <= colCap) {
                row[p - 1] = F2D_PUB_(Add_)(row[p - 1], row[j - 1]);
            }
        }
    }

    for (uint64_t i = 1; i <= rowCap; i++) {
        const uint64_t p = fenwickParent(i);
        if (p > rowCap) {
            continue;
        }

        F2D_VALUE_T *dst = &cells[(p - 1) * colCap];
        const F2D_VALUE_T *src = &cells[(i - 1) * colCap];
        for (uint64_t j = 0; j < colCap; j++) {
            dst[j] = F2D_PUB_(Add_)(dst[j], src[j]);
        }
    }

    return t;
}

void F2D_PUB_(CopyTo)(const F2D_T_ *t, F2D_VALUE_T *out) {
    if (!t || t->rows == 0 || t->cols == 0) {
        return;
    }

    const uint64_t rows = t->rows;
    const uint64_t cols = t->cols;
#if F2D_IS_FLOATING
    /* Float cells round, so read each element as Get does */
    for (uint64_t r = 0; r < rows; r++) {
        for (uint64_t c = 0; c < cols; c++) {
            out[r * cols + c] = F2D_PUB_(Element_)(t, r, c);
        }
    }
#else
    /* Undo the column pass, then the row pass, over the rows x cols
     * corner of the cells: every child of a cell there is there too */
    for (uint64_t r = 0; r < rows; r++) {
        memcpy(&out[r * cols], &t->store.cells[r * t->colCap],
               cols * sizeof(*out));
    }

    for (uint64_t i = rows; i >= 1; i--) {
        const uint64_t p = fenwickParent(i);
        if (p <= rows) {
            F2D_VALUE_T *dst = &out[(p - 1) * cols];
            const F2D_VALUE_T *src = &out[(i - 1) * cols];
            for (uint64_t c = 0; c < cols; c++) {
                dst[c] = F2D_PUB_(Sub_)(dst[c], src[c]);
            }
        }
    }

    for (uint64_t r = 0; r < rows; r++) {
        F2D_VALUE_T *row = &out[r * cols];
        for (uint64_t j = cols; j >= 1; j--) {
            const uint64_t p = fenwickParent(j);
            if (p <= cols) {
                row[p - 1] = F2D_PUB_(Sub_)(row[p - 1], row[j - 1]);
            }
        }
    }
#endif
}

/* Persistence, dense, like the 1D trees: integer trees snapshot their
 * elements, float trees their capacities, extents and every cell.
 *   integer: flex [rows, cols, element (0,0), (0,1), ... row-major]
 *   float:   flex [rowCap, colCap, rows, cols, cell 0 ... row-major]
 * WAL (args: const databox *const *, three of them): NUM_INCR [row, col,
 * delta] (Update), UPDATE [row, col, value] (Set), CLEAR. */
static void F2D_PUB_(PersistPushUnsigned_)(flex **f, uint64_t v) {
    const databox box = DATABOX_UNSIGNED(v);
    flexPushByTypeExact(f, &box, FLEX_ENDPOINT_TAIL);
}

static uint8_t *F2D_PUB_(PersistSnapshot_)(const void *structure, size_t *len) {
    const F2D_T_ *t = structure;
    /* @own: caller; free=zfree */
    flex *f = flexNew();
#if F2D_IS_FLOATING
    F2D_PUB_(PersistPushUnsigned_)(&f, t->rowCap);
    F2D_PUB_(PersistPushUnsigned_)(&f, t->colCap);
    F2D_PUB_(PersistPushUnsigned_)(&f, t->rows);
    F2D_PUB_(PersistPushUnsigned_)(&f, t->cols);
    F2D_PUB_(PersistAppend_)(&f, t->store.cells, t->rowCap * t->colCap);
#else
    F2D_PUB_(PersistPushUnsigned_)(&f, t->rows);
    F2D_PUB_(PersistPushUnsigned_)(&f, t->cols);
    if (t->rows) {
        cleanerName(zfree, F2dValues) F2D_VALUE_T *values =
            zmalloc(t->rows * t->cols * sizeof(*values));
        F2D_PUB_(CopyTo)(t, values);
        F2D_PUB_(PersistAppend_)(&f, values, t->rows * t->cols);
    }
#endif

    *len = flexBytes(f);
    return (uint8_t *)f;
}

static void *F2D_PUB_(PersistRestore_)(const uint8_t *data, size_t len) {
    if (!flexValidateBufferDeep(data, len)) {
        return nullptr;
    }

    const flex *f = (const flex *)data;
    const size_t n = flexCount(f);
    flexEntry *fe = flexHead(f);
    databoxBig box;
#if F2D_IS_FLOATING
    uint64_t rowCap;
    uint64_t colCap;
    uint64_t rows;
    uint64_t cols;
    if (n < 4 || !flexReadUnsigned(f, &fe, &rowCap) ||
        !flexReadUnsigned(f, &fe, &colCap) ||
        !flexReadUnsigned(f, &fe, &rows) || !flexReadUnsigned(f, &fe, &cols)) {
        return nullptr;
    }

    /* Empty, or power-of-two capacities holding the extents, with
     * exactly one entry per cell */
    const bool empty = rowCap == 0;
    if (empty != (colCap == 0) || empty != (rows == 0) ||
        empty != (cols == 0) || (rowCap & (rowCap - 1)) ||
        (colCap & (colCap - 1)) || rows > rowCap || cols > colCap ||
        (!empty && (colCap > (n - 4) / rowCap || rowCap * colCap != n - 4 ||
                    rowCap * colCap > F2D_P_MAX_CELLS_)) ||
        (empty && n != 4)) {
        return nullptr;
    }

    /* @own: caller */
    F2D_T_ *t = F2D_PUB_(New)();
    if (empty) {
        return t;
    }

    /* @own: container; free=zfree */
    t->store.cells = zcalloc(rowCap * colCap, sizeof(F2D_VALUE_T));
    t->rowCap = rowCap;
    t->colCap = colCap;
    t->rows = rows;
    t->cols = cols;
    for (uint64_t i = 0; i < rowCap * colCap; i++) {
        flexGetByTypeBig(fe, &box);
        fe = flexNext(f, fe);
        if (!F2D_PUB_(PersistUnbox_)(&box, &t->store.cells[i])) {
            F2D_PUB_(Free)(t);
            return nullptr;
        }
    }

    return t;
#else
    uint64_t rows;
    uint64_t cols;
    if (n < 2 || !flexReadUnsigned(f, &fe, &rows) ||
        !flexReadUnsigned(f, &fe, &cols) || (rows == 0) != (cols == 0) ||
        (rows && (cols > (n - 2) / rows || rows * cols != n - 2)) ||
        (!rows && n != 2)) {
        return nullptr;
    }

    const size_t count = n - 2;
    cleanerName(zfree, F2dValues) F2D_VALUE_T *values =
        zmalloc((count ? count : 1) * sizeof(*values));
    for (size_t i = 0; i < count; i++) {
        flexGetByTypeBig(fe, &box);
        fe = flexNext(f, fe);
        if (!F2D_PUB_(PersistUnbox_)(&box, &values[i])) {
            return nullptr;
        }
    }

    return F2D_PUB_(NewFromArray)(values, rows, cols);
#endif
}

static uint8_t *F2D_PUB_(PersistEncodeOp_)(persistOp op, const void *args,
                                           size_t argc, size_t *len) {
    const databox *const *boxes = args;
    *len = 0;
    if ((op != PERSIST_OP_NUM_INCR && op != PERSIST_OP_UPDATE) || argc != 3 ||
        !boxes) {
        /* CLEAR has no operand; nothing else is replayed */
        return nullptr;
    }

    flex *f = flexNewFromDataboxPtrsExact(boxes, argc);
    if (f) {
        *len = flexBytes(f);
    }

    return (uint8_t *)f;
}

static bool F2D_PUB_(PersistApplyOp_)(void *structure, persistOp op,
                                      const uint8_t *data, size_t len) {
    F2D_T_ **t = structure;
    if (op == PERSIST_OP_CLEAR) {
        F2D_PUB_(Clear)(*t);
        return true;
    }

    if ((op != PERSIST_OP_NUM_INCR && op != PERSIST_OP_UPDATE) ||
        !flexValidateBufferDeep(data, len)) {
        return false;
    }

    const flex *f = (const flex *)data;
    if (flexCount(f) != 3) {
        return false;
    }

    flexEntry *fe = flexHead(f);
    uint64_t row;
    uint64_t col;
    databoxBig value;
    F2D_VALUE_T v;
    if (!flexReadUnsigned(f, &fe, &row) || !flexReadUnsigned(f, &fe, &col) ||
        row > SIZE_MAX || col > SIZE_MAX) {
        return false;
    }

    flexGetByTypeBig(fe, &value);
    if (!F2D_PUB_(PersistUnbox_)(&value, &v)) {
        return false;
    }

    return op == PERSIST_OP_NUM_INCR
               ? F2D_PUB_(Update)(t, (size_t)row, (size_t)col, v)
               : F2D_PUB_(Set)(t, (size_t)row, (size_t)col, v);
}

static void *F2D_PUB_(PersistNewEmpty_)(void) {
    return F2D_PUB_(New)();
}

static size_t F2D_PUB_(PersistCount_)(const void *structure) {
    const F2D_T_ *t = structure;
    return t ? (size_t)(t->rows * t->cols) : 0;
}

static size_t F2D_PUB_(PersistBytes_)(const void *structure) {
    return F2D_PUB_(Bytes)(structure);
}

static void F2D_PUB_(PersistFree_)(void *structure) {
    F2D_PUB_(Free)(structure);
}

const persistOps F2D_CAT_(persistOpsFenwick2d, F2D_SUFFIX) = {
    .type = F2D_PERSIST_TYPE,
    .name = "fenwick2d" F2D_P_STR_(F2D_SUFFIX),
    .formatVersion = 1,
    .minReadVersion = 1,
    .snapshot = F2D_PUB_(PersistSnapshot_),
    .restore = F2D_PUB_(PersistRestore_),
    .newEmpty = F2D_PUB_(PersistNewEmpty_),
    .count = F2D_PUB_(PersistCount_),
    .estimateSize = F2D_PUB_(PersistBytes_),
    .encodeOp = F2D_PUB_(PersistEncodeOp_),
    .applyOp = F2D_PUB_(PersistApplyOp_),
    .free = F2D_PUB_(PersistFree_),
};

#undef F2D_P_MAX_CELLS_
#undef F2D_P_STR_
#undef F2D_P_STR2_
#undef F2D_PERSIST_TYPE
#undef F2D_CAT2_
#undef F2D_CAT_
#undef F2D_PUB_
#undef F2D_T_
#undef F2D_SUFFIX
#undef F2D_VALUE_T
#undef F2D_IS_FLOATING
