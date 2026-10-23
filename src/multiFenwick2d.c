#include "multiFenwick2d.h"
#include "datakit.h"
#include "flex.h"
#include "multiFenwickCells.h"
#include "multiFenwickCommon.h"
#include "persist.h"

/* The cells are the adaptive store multiFenwickFlat uses
 * (multiFenwickCells.h): 8 bytes per cell while every cell is an int64,
 * with integer-only sums and writes; a kinds array joins once a cell
 * holds a value above INT64_MAX or a double. */
#define F2D_FN(name) multiFenwick2d##name
#define F2D_STRUCT multiFenwick2d
#define F2D_STORE_T multiFenwickCells
#define F2D_PLAN_T multiFenwickCell
#define F2D_NUM_T multiFenwickNum
#define F2D_NUM_ZERO ((multiFenwickNum){0})
#define F2D_NUM_ADD(a, b) multiFenwickNumAdd_(a, b)
#define F2D_NUM_SUB(a, b) multiFenwickNumSub_(a, b)
#define F2D_LOAD(s, idx) multiFenwickCellsGet(s, idx)
#define F2D_ENCODE(num, out) multiFenwickCellEncode(num, out)
#define F2D_PUT(s, n, idx, plan) multiFenwickCellsPut(s, n, idx, plan)
#define F2D_COPY(s, dst, src) multiFenwickCellsCopy(s, dst, src)
#define F2D_RELAYOUT(s, oR, oC, nR, nC)                                        \
    multiFenwickCellsRelayout(s, oR, oC, nR, nC)
#define F2D_CLEAR(s, n) multiFenwickCellsClear(s, n)
#define F2D_BYTES(s, n) multiFenwickCellsBytes(s, n)
#define F2D_FREE(s) multiFenwickCellsFree(s)
#define F2D_FAST_INT
#define F2D_ALL_INT(s) multiFenwickCellsAllInt(s)
#define F2D_INT(s, idx) multiFenwickCellsInt(s, idx)
#define F2D_PUT_INT(s, idx, v) multiFenwickCellsPutInt(s, idx, v)
#define F2D_NUM_FROM_I128(v) ((multiFenwickNum){.i = (v)})
#define F2D_NUM_IS_INT(num) (!(num).isFloat)
#define F2D_NUM_I128(num) ((num).i)
#include "fenwick/fenwick2dCoreImpl.h"

multiFenwick2d *multiFenwick2dNew(void) {
    /* @own: caller */
    return zcalloc(1, sizeof(multiFenwick2d));
}

void multiFenwick2dFree(multiFenwick2d *t) {
    if (t) {
        multiFenwick2dFreeStore_(t);
        zfree(t);
    }
}

/* Element (row, col), zero outside the extents */
static multiFenwickNum multiFenwick2dElementOrZero_(const multiFenwick2d *t,
                                                    size_t row, size_t col) {
    if (!t || row >= t->rows || col >= t->cols) {
        return (multiFenwickNum){0};
    }

    return multiFenwick2dElement_(t, row, col);
}

/* Add 'delta' at (row, col), creating the tree when *t is NULL and
 * freeing it again when the write is refused */
static bool multiFenwick2dWriteOrCreate_(multiFenwick2d **t, size_t row,
                                         size_t col, multiFenwickNum delta) {
    const bool created = !*t;
    if (created) {
        /* @own: caller; free=multiFenwick2dFree */
        *t = multiFenwick2dNew();
    }

    if (!multiFenwick2dWrite_(*t, row, col, delta)) {
        if (created) {
            multiFenwick2dFree(*t);
            *t = NULL;
        }

        return false;
    }

    return true;
}

bool multiFenwick2dUpdate(multiFenwick2d **t, size_t row, size_t col,
                          const databox *delta) {
    multiFenwickNum d;
    if (!t || !delta || !multiFenwickNumFrom_(delta, &d)) {
        return false;
    }

    return multiFenwick2dWriteOrCreate_(t, row, col, d);
}

/* Set: one write of value - current, kept exact (the difference may need
 * more than 64 bits) */
bool multiFenwick2dSet(multiFenwick2d **t, size_t row, size_t col,
                       const databox *value) {
    multiFenwickNum want;
    if (!t || !value || !multiFenwickNumFrom_(value, &want)) {
        return false;
    }

    return multiFenwick2dWriteOrCreate_(
        t, row, col,
        multiFenwickNumSub_(want, multiFenwick2dElementOrZero_(*t, row, col)));
}

/* Box 'n' into 'result'; false (result VOID) for a NULL result or a value
 * with no 64-bit form */
static bool multiFenwick2dResult_(multiFenwickNum n, databox *result) {
    if (!result) {
        return false;
    }

    if (!multiFenwickNumBox_(n, result)) {
        *result = DATABOX_BOX_VOID;
        return false;
    }

    return true;
}

bool multiFenwick2dGet(const multiFenwick2d *t, size_t row, size_t col,
                       databox *result) {
    return multiFenwick2dResult_(multiFenwick2dElementOrZero_(t, row, col),
                                 result);
}

bool multiFenwick2dQuery(const multiFenwick2d *t, size_t row, size_t col,
                         databox *result) {
    return multiFenwick2dResult_(
        t ? multiFenwick2dPrefix_(t, row, col) : (multiFenwickNum){0}, result);
}

bool multiFenwick2dRangeQuery(const multiFenwick2d *t, size_t r1, size_t c1,
                              size_t r2, size_t c2, databox *result) {
    multiFenwickNum sum = {0};
    if (t && t->rows > 0 && t->cols > 0) {
        r2 = r2 < t->rows ? r2 : t->rows - 1;
        c2 = c2 < t->cols ? c2 : t->cols - 1;
        if (r1 <= r2 && c1 <= c2) {
            sum = multiFenwick2dRect_(t, r1, c1, r2, c2);
        }
    }

    return multiFenwick2dResult_(sum, result);
}

size_t multiFenwick2dRows(const multiFenwick2d *t) {
    return t ? t->rows : 0;
}

size_t multiFenwick2dCols(const multiFenwick2d *t) {
    return t ? t->cols : 0;
}

size_t multiFenwick2dBytes(const multiFenwick2d *t) {
    return t ? multiFenwick2dBytes_(t) : 0;
}

void multiFenwick2dClear(multiFenwick2d *t) {
    if (t) {
        multiFenwick2dClear_(t);
    }
}

/* ====================================================================
 * Persistence
 * ====================================================================
 * Snapshot: flex [rowCap, colCap, rows, cols, every cell row-major],
 * cells as multiFenwickFlat writes them (rounding in double cells is
 * kept).  WAL (args: const databox *const *, three of them): NUM_INCR
 * [row, col, delta] (Update), UPDATE [row, col, value] (Set), CLEAR. */

/* The largest cell count a tree reaches (fenwick2dCoreImpl.h) */
#define MF2D_P_MAX_CELLS_ (UINT64_C(1) << 32)

static uint8_t *multiFenwick2dPersistSnapshot_(const void *structure,
                                               size_t *len) {
    const multiFenwick2d *t = structure;
    /* @own: caller; free=zfree */
    flex *f = flexNew();
    const uint64_t header[4] = {t->rowCap, t->colCap, t->rows, t->cols};
    for (size_t h = 0; h < 4; h++) {
        const databox box = DATABOX_UNSIGNED(header[h]);
        flexPushByTypeExact(&f, &box, FLEX_ENDPOINT_TAIL);
    }

    /* 64 cells per flex growth */
    enum { BATCH = 64 };
    databox boxes[BATCH];
    const databox *ptrs[BATCH];
    const uint64_t cells = t->rowCap * t->colCap;
    for (uint64_t at = 0; at < cells; at += BATCH) {
        const uint64_t n = cells - at < BATCH ? cells - at : BATCH;
        for (uint64_t i = 0; i < n; i++) {
            /* A stored cell always boxes */
            const bool ok = multiFenwickNumBox_(
                multiFenwickCellsGet(&t->store, at + i), &boxes[i]);
            assert(ok);
            (void)ok;
            ptrs[i] = &boxes[i];
        }

        flexAppendMultipleExact(&f, (uint_fast32_t)n, ptrs);
    }

    *len = flexBytes(f);
    return (uint8_t *)f;
}

static void *multiFenwick2dPersistRestore_(const uint8_t *data, size_t len) {
    if (!flexValidateBufferDeep(data, len)) {
        return NULL;
    }

    const flex *f = (const flex *)data;
    const size_t n = flexCount(f);
    if (n < 4) {
        return NULL;
    }

    flexEntry *fe = flexHead(f);
    databoxBig box;
    uint64_t header[4];
    for (size_t h = 0; h < 4; h++) {
        flexGetByTypeBig(fe, &box);
        fe = flexNext(f, fe);
        if (box.type != DATABOX_UNSIGNED_64) {
            return NULL;
        }

        header[h] = box.data.u64;
    }

    const uint64_t rowCap = header[0];
    const uint64_t colCap = header[1];
    const uint64_t rows = header[2];
    const uint64_t cols = header[3];
    const bool empty = rowCap == 0;
    if (empty != (colCap == 0) || empty != (rows == 0) ||
        empty != (cols == 0) || (rowCap & (rowCap - 1)) ||
        (colCap & (colCap - 1)) || rows > rowCap || cols > colCap ||
        (empty && n != 4) ||
        (!empty && (colCap > (n - 4) / rowCap || rowCap * colCap != n - 4 ||
                    rowCap * colCap > MF2D_P_MAX_CELLS_))) {
        return NULL;
    }

    /* @own: caller; free=multiFenwick2dFree */
    multiFenwick2d *t = multiFenwick2dNew();
    if (empty) {
        return t;
    }

    const uint64_t cells = rowCap * colCap;
    multiFenwickCellsGrow(&t->store, 0, cells);
    t->rowCap = rowCap;
    t->colCap = colCap;
    t->rows = rows;
    t->cols = cols;
    for (uint64_t i = 0; i < cells; i++) {
        flexGetByTypeBig(fe, &box);
        fe = flexNext(f, fe);
        multiFenwickNum num;
        multiFenwickCell cell;
        if (!multiFenwickNumFrom_((const databox *)&box, &num) ||
            !multiFenwickCellEncode(num, &cell)) {
            multiFenwick2dFree(t);
            return NULL;
        }

        multiFenwickCellsPut(&t->store, cells, i, cell);
    }

    return t;
}

static uint8_t *multiFenwick2dPersistEncodeOp_(persistOp op, const void *args,
                                               size_t argc, size_t *len) {
    const databox *const *boxes = args;
    *len = 0;
    if ((op != PERSIST_OP_NUM_INCR && op != PERSIST_OP_UPDATE) || argc != 3 ||
        !boxes) {
        /* CLEAR has no operand; nothing else is replayed */
        return NULL;
    }

    flex *f = flexNewFromDataboxPtrsExact(boxes, argc);
    if (f) {
        *len = flexBytes(f);
    }

    return (uint8_t *)f;
}

static bool multiFenwick2dPersistApplyOp_(void *structure, persistOp op,
                                          const uint8_t *data, size_t len) {
    multiFenwick2d **t = structure;
    if (op == PERSIST_OP_CLEAR) {
        multiFenwick2dClear(*t);
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
    databoxBig args[3];
    for (size_t i = 0; i < 3; i++) {
        flexGetByTypeBig(fe, &args[i]);
        fe = flexNext(f, fe);
    }

    if (args[0].type != DATABOX_UNSIGNED_64 ||
        args[1].type != DATABOX_UNSIGNED_64 || args[0].data.u64 > SIZE_MAX ||
        args[1].data.u64 > SIZE_MAX) {
        return false;
    }

    const size_t row = (size_t)args[0].data.u64;
    const size_t col = (size_t)args[1].data.u64;
    const databox *value = (const databox *)&args[2];
    return op == PERSIST_OP_NUM_INCR ? multiFenwick2dUpdate(t, row, col, value)
                                     : multiFenwick2dSet(t, row, col, value);
}

static void *multiFenwick2dPersistNewEmpty_(void) {
    /* @own: caller; free=multiFenwick2dFree */
    return multiFenwick2dNew();
}

static size_t multiFenwick2dPersistCount_(const void *structure) {
    const multiFenwick2d *t = structure;
    return t ? (size_t)(t->rows * t->cols) : 0;
}

static size_t multiFenwick2dPersistBytes_(const void *structure) {
    return multiFenwick2dBytes(structure);
}

static void multiFenwick2dPersistFree_(void *structure) {
    multiFenwick2dFree(structure);
}

const persistOps persistOpsMultiFenwick2d = {
    .type = PERSIST_TYPE_MULTIFENWICK_2D,
    .name = "multiFenwick2d",
    .formatVersion = 1,
    .minReadVersion = 1,
    .snapshot = multiFenwick2dPersistSnapshot_,
    .restore = multiFenwick2dPersistRestore_,
    .newEmpty = multiFenwick2dPersistNewEmpty_,
    .count = multiFenwick2dPersistCount_,
    .estimateSize = multiFenwick2dPersistBytes_,
    .encodeOp = multiFenwick2dPersistEncodeOp_,
    .applyOp = multiFenwick2dPersistApplyOp_,
    .free = multiFenwick2dPersistFree_,
};

#undef MF2D_P_MAX_CELLS_
