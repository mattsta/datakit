#include "multiFenwick2d.h"
#include "datakit.h"
#include "multiFenwickCells.h"
#include "multiFenwickCommon.h"

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
