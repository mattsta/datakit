/* ====================================================================
 * 2D Fenwick Tree Typed Template: the public API of one value type
 * ====================================================================
 *
 * Included once per type by fenwick2d.c, with:
 *   F2D_SUFFIX       - I64, U64, Double
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
    (void)__builtin_add_overflow(a, b, &r); /* wraps by design */
    return r;
#endif
}

static inline F2D_VALUE_T F2D_PUB_(Sub_)(F2D_VALUE_T a, F2D_VALUE_T b) {
#if F2D_IS_FLOATING
    return a - b;
#else
    F2D_VALUE_T r;
    (void)__builtin_sub_overflow(a, b, &r); /* wraps by design */
    return r;
#endif
}

#define F2D_FN(name) F2D_PUB_(name)
#define F2D_STRUCT F2D_T_
#define F2D_CELL_T F2D_VALUE_T
#define F2D_NUM_T F2D_VALUE_T
#define F2D_NUM_ZERO ((F2D_VALUE_T)0)
#define F2D_NUM_ADD(a, b) F2D_PUB_(Add_)(a, b)
#define F2D_NUM_SUB(a, b) F2D_PUB_(Sub_)(a, b)
#define F2D_CELL_LOAD(cell) (*(cell))
#define F2D_CELL_ENCODE(num, out) (*(out) = (num), true)
#include "fenwick2dCoreImpl.h"
#undef F2D_FN
#undef F2D_STRUCT
#undef F2D_CELL_T
#undef F2D_NUM_T
#undef F2D_NUM_ZERO
#undef F2D_NUM_ADD
#undef F2D_NUM_SUB
#undef F2D_CELL_LOAD
#undef F2D_CELL_ENCODE

F2D_T_ *F2D_PUB_(New)(void) {
    /* @own: caller */
    return zcalloc(1, sizeof(F2D_T_));
}

void F2D_PUB_(Free)(F2D_T_ *t) {
    if (t) {
        zfree(t->cells);
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
            *t = NULL;
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
    if (__builtin_add_overflow(old, delta, &element)) {
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

#undef F2D_CAT2_
#undef F2D_CAT_
#undef F2D_PUB_
#undef F2D_T_
#undef F2D_SUFFIX
#undef F2D_VALUE_T
#undef F2D_IS_FLOATING
