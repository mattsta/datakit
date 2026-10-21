#include "multiFenwick2d.h"
#include "datakit.h"
#include "multiFenwickCommon.h"

#include <string.h>

/* What a cell's 64 bits hold.  UNSIGNED is 0, so zero-filled memory is a
 * cell holding the unsigned integer 0. */
typedef enum multiFenwick2dKind {
    MFW2D_UNSIGNED = 0,
    MFW2D_SIGNED = 1,
    MFW2D_DOUBLE = 2,
} multiFenwick2dKind;

/* One cell: 8 bytes of value bits and its kind, packed to 9 bytes */
typedef struct __attribute__((packed)) multiFenwick2dCell {
    uint64_t bits;
    uint8_t kind;
} multiFenwick2dCell;

static multiFenwickNum multiFenwick2dLoad_(const multiFenwick2dCell *cell) {
    const uint64_t bits = cell->bits;
    switch ((multiFenwick2dKind)cell->kind) {
    case MFW2D_SIGNED:
        return (multiFenwickNum){.i = (int64_t)bits};
    case MFW2D_DOUBLE: {
        double d;
        memcpy(&d, &bits, sizeof(d));
        return (multiFenwickNum){.d = d, .isFloat = true};
    }
    case MFW2D_UNSIGNED:
    default:
        return (multiFenwickNum){.i = bits};
    }
}

/* Encode 'n' into '*out'; false (nothing written) when no cell holds it */
static bool multiFenwick2dEncode_(multiFenwickNum n, multiFenwick2dCell *out) {
    databox box;
    if (!multiFenwickNumBox_(n, &box)) {
        return false;
    }

    switch (box.type) {
    case DATABOX_SIGNED_64:
        *out = (multiFenwick2dCell){.bits = (uint64_t)box.data.i64,
                                    .kind = MFW2D_SIGNED};
        break;
    case DATABOX_DOUBLE_64: {
        uint64_t bits;
        memcpy(&bits, &box.data.d64, sizeof(bits));
        *out = (multiFenwick2dCell){.bits = bits, .kind = MFW2D_DOUBLE};
        break;
    }
    case DATABOX_UNSIGNED_64:
    default:
        *out =
            (multiFenwick2dCell){.bits = box.data.u64, .kind = MFW2D_UNSIGNED};
        break;
    }

    return true;
}

#define F2D_FN(name) multiFenwick2d##name
#define F2D_STRUCT multiFenwick2d
#define F2D_CELL_T multiFenwick2dCell
#define F2D_NUM_T multiFenwickNum
#define F2D_NUM_ZERO ((multiFenwickNum){0})
#define F2D_NUM_ADD(a, b) multiFenwickNumAdd_(a, b)
#define F2D_NUM_SUB(a, b) multiFenwickNumSub_(a, b)
#define F2D_CELL_LOAD(cell) multiFenwick2dLoad_(cell)
#define F2D_CELL_ENCODE(num, out) multiFenwick2dEncode_(num, out)
#include "fenwick/fenwick2dCoreImpl.h"

multiFenwick2d *multiFenwick2dNew(void) {
    /* @own: caller */
    return zcalloc(1, sizeof(multiFenwick2d));
}

void multiFenwick2dFree(multiFenwick2d *t) {
    if (t) {
        zfree(t->cells);
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
