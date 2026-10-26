/* ====================================================================
 * 2D Fenwick Tree Core Template
 * ====================================================================
 *
 * The traversal, growth and transactional-write machinery of a 2D Binary
 * Indexed Tree, written once for every cell model (the typed trees in
 * fenwick2d<Suffix>.c and the adaptive databox tree in multiFenwick2d.c).
 * Every function is static to the including file.
 *
 * MODEL
 * -----
 * Elements e[r][c], 0-based.  Cell (i, j), 1-based, holds the sum of the
 * elements in rows (i - LSB(i), i] and columns (j - LSB(j), j].  Cells are
 * row-major in rowCap x colCap (both powers of two), cell (i, j) at index
 * (i - 1) * colCap + (j - 1) of the cell store.  Point updates, prefix
 * sums and element reads are O(log R * log C).
 *
 * GROWTH
 * ------
 * Capacities double independently.  A new row i > rowCap covers rows
 * (i - LSB(i), i], which reach existing rows only when i is a power of two,
 * and then cover all of them: such a row is a copy of row rowCap.  Columns
 * likewise copy column colCap.  Applying rows then columns gives every new
 * cell, including (2 rowCap, 2 colCap) = (rowCap, colCap), the total.  So
 * growth is O(new cells) (a column doubling re-lays out every row).
 * Cell (i, j) of the grown tree can also be read virtually, before the
 * growth happens (CellVirtual_), which is what keeps writes transactional.
 *
 * CELL STORE (required defines)
 * -----------------------------
 *   F2D_FN(name)               static function name for this instantiation
 *   F2D_STRUCT                 the tree struct tag (members below)
 *   F2D_STORE_T                the cell store; zero-initialized is empty
 *   F2D_PLAN_T                 one encoded cell value
 *   F2D_NUM_T, F2D_NUM_ZERO, F2D_NUM_ADD(a, b), F2D_NUM_SUB(a, b)
 *   F2D_LOAD(store, idx)       the F2D_NUM_T of cell idx
 *   F2D_ENCODE(num, planOut)   encode; false (nothing written) when no
 *                              cell can hold num
 *   F2D_PUT(store, cells, idx, plan)  store a plan; 'cells' is the
 *                              store's cell count
 *   F2D_COPY(store, dst, src)  copy one cell
 *   F2D_RELAYOUT(store, oldR, oldC, newR, newC)  grow, new cells zero
 *   F2D_CLEAR(store, cells), F2D_BYTES(store, cells), F2D_FREE(store)
 *
 * OPTIONAL INTEGER FAST PATH (define F2D_FAST_INT)
 * ------------------------------------------------
 * For stores that know when every cell is an int64:
 *   F2D_ALL_INT(store), F2D_INT(store, idx) -> int64_t,
 *   F2D_PUT_INT(store, idx, int64_t),
 *   F2D_NUM_FROM_I128(v), F2D_NUM_IS_INT(num), F2D_NUM_I128(num)
 * Sums then accumulate in int128, and in-capacity integer writes whose
 * cells all stay int64 skip encoding.
 *
 * The struct (defined here) is:
 *   struct F2D_STRUCT { F2D_STORE_T store; uint64_t rowCap, colCap,
 *                       rows, cols; };
 * rows and cols are the extents: highest written index + 1. */

#include "fenwickCommon.h"

/* Largest cell count a tree may reach (rowCap * colCap); a write that
 * would grow past it is refused.  It also bounds every chain:
 * log2(rowCap) + log2(colCap) <= 32. */
#define F2D_MAX_CELLS (UINT64_C(1) << 32)

/* Most cells one update touches: (log2 R + 1) * (log2 C + 1) with
 * log2 R + log2 C <= 32 */
#define F2D_MAX_CHAIN 289

struct F2D_STRUCT {
    F2D_STORE_T store;
    uint64_t rowCap;
    uint64_t colCap;
    uint64_t rows;
    uint64_t cols;
};

/* Smallest power of two, at least 'current' (and at least 1), above idx */
static inline uint64_t F2D_FN(CapFor_)(uint64_t current, uint64_t idx) {
    return stdc_bit_ceil(current > idx + 1 ? current : idx + 1);
}

static inline uint64_t F2D_FN(Index_)(const struct F2D_STRUCT *t, uint64_t i,
                                      uint64_t j) {
    return (i - 1) * t->colCap + (j - 1);
}

static inline F2D_NUM_T F2D_FN(Cell_)(const struct F2D_STRUCT *t, uint64_t i,
                                      uint64_t j) {
    return F2D_LOAD(&t->store, F2D_FN(Index_)(t, i, j));
}

/* Where a cell of the grown tree takes its value from: itself when it
 * exists, the last row/column when it is a new power-of-two one, else 0
 * (the cell is zero). */
static inline uint64_t F2D_FN(Source_)(uint64_t k, uint64_t cap) {
    if (k <= cap) {
        return k;
    }

    return cap > 0 && stdc_has_single_bit(k) ? cap : 0;
}

/* Cell (i, j) of the tree as it will be once grown past its capacity */
static inline F2D_NUM_T F2D_FN(CellVirtual_)(const struct F2D_STRUCT *t,
                                             uint64_t i, uint64_t j) {
    const uint64_t si = F2D_FN(Source_)(i, t->rowCap);
    const uint64_t sj = F2D_FN(Source_)(j, t->colCap);
    if (si == 0 || sj == 0) {
        return F2D_NUM_ZERO;
    }

    return F2D_FN(Cell_)(t, si, sj);
}

/* The capacities a write at (row, col) needs; false when they would pass
 * F2D_MAX_CELLS */
static inline bool F2D_FN(CapsFor_)(const struct F2D_STRUCT *t, uint64_t row,
                                    uint64_t col, uint64_t *rowCap,
                                    uint64_t *colCap) {
    if (row >= F2D_MAX_CELLS || col >= F2D_MAX_CELLS) {
        return false;
    }

    *rowCap = F2D_FN(CapFor_)(t->rowCap, row);
    *colCap = F2D_FN(CapFor_)(t->colCap, col);
    return *rowCap <= F2D_MAX_CELLS / *colCap;
}

/* Grow to rowCap x colCap (each at least the current one).  OOM is
 * fail-fast. */
static void F2D_FN(Grow_)(struct F2D_STRUCT *t, uint64_t rowCap,
                          uint64_t colCap) {
    const uint64_t oldR = t->rowCap;
    const uint64_t oldC = t->colCap;
    if (rowCap == oldR && colCap == oldC) {
        return;
    }

    F2D_RELAYOUT(&t->store, oldR, oldC, rowCap, colCap);
    t->rowCap = rowCap;
    t->colCap = colCap;
    if (oldR == 0 || oldC == 0) {
        return; /* was empty: every cell is zero */
    }

    /* New power-of-two rows copy row oldR, then new power-of-two columns
     * copy column oldC (in every row, new rows included) */
    for (uint64_t i = oldR * 2; i <= rowCap; i <<= 1) {
        for (uint64_t j = 1; j <= oldC; j++) {
            F2D_COPY(&t->store, F2D_FN(Index_)(t, i, j),
                     F2D_FN(Index_)(t, oldR, j));
        }
    }

    for (uint64_t j = oldC * 2; j <= colCap; j <<= 1) {
        for (uint64_t i = 1; i <= rowCap; i++) {
            F2D_COPY(&t->store, F2D_FN(Index_)(t, i, j),
                     F2D_FN(Index_)(t, i, oldC));
        }
    }
}

/* Plan adding 'delta' to element (row, col) in the tree grown to
 * rowCap x colCap: every touched cell's new value, encoded, into 'plan'
 * in chain order.  False (nothing changed) when a cell cannot hold its
 * new value. */
static bool F2D_FN(Plan_)(const struct F2D_STRUCT *t, uint64_t row,
                          uint64_t col, uint64_t rowCap, uint64_t colCap,
                          F2D_NUM_T delta, F2D_PLAN_T *plan) {
    size_t n = 0;
    for (uint64_t i = row + 1; i <= rowCap; i = fenwickParent(i)) {
        for (uint64_t j = col + 1; j <= colCap; j = fenwickParent(j)) {
            if (!F2D_ENCODE(F2D_NUM_ADD(F2D_FN(CellVirtual_)(t, i, j), delta),
                            &plan[n])) {
                return false;
            }

            n++;
        }
    }

    return true;
}

/* Write a plan made by Plan_ for (row, col) (the tree already grown) */
static void F2D_FN(Apply_)(struct F2D_STRUCT *t, uint64_t row, uint64_t col,
                           const F2D_PLAN_T *plan) {
    const uint64_t cells = t->rowCap * t->colCap;
    size_t n = 0;
    for (uint64_t i = row + 1; i <= t->rowCap; i = fenwickParent(i)) {
        for (uint64_t j = col + 1; j <= t->colCap; j = fenwickParent(j)) {
            F2D_PUT(&t->store, cells, F2D_FN(Index_)(t, i, j), plan[n++]);
        }
    }
}

static void F2D_FN(Extend_)(struct F2D_STRUCT *t, uint64_t row, uint64_t col) {
    if (row >= t->rows) {
        t->rows = row + 1;
    }

    if (col >= t->cols) {
        t->cols = col + 1;
    }
}

/* Sum of elements [0, row] x [0, col], both clamped to the extents (the
 * elements past them are zero); zero for an empty tree */
static F2D_NUM_T F2D_FN(Prefix_)(const struct F2D_STRUCT *t, uint64_t row,
                                 uint64_t col) {
    if (t->rows == 0 || t->cols == 0) {
        return F2D_NUM_ZERO;
    }

    const uint64_t r = (row < t->rows ? row : t->rows - 1) + 1;
    const uint64_t c = (col < t->cols ? col : t->cols - 1) + 1;
#ifdef F2D_FAST_INT
    if (F2D_ALL_INT(&t->store)) {
        __int128_t sum = 0;
        for (uint64_t i = r; i > 0; i = fenwickPrev(i)) {
            for (uint64_t j = c; j > 0; j = fenwickPrev(j)) {
                sum += F2D_INT(&t->store, F2D_FN(Index_)(t, i, j));
            }
        }

        return F2D_NUM_FROM_I128(sum);
    }
#endif

    F2D_NUM_T acc = F2D_NUM_ZERO;
    for (uint64_t i = r; i > 0; i = fenwickPrev(i)) {
        for (uint64_t j = c; j > 0; j = fenwickPrev(j)) {
            acc = F2D_NUM_ADD(acc, F2D_FN(Cell_)(t, i, j));
        }
    }

    return acc;
}

/* Sum of cells (i, j) over j = c and then c - 1, Prev(c - 1), ... down to
 * c - LSB(c) negated: the 1D element trick along one row */
static inline F2D_NUM_T F2D_FN(RowElement_)(const struct F2D_STRUCT *t,
                                            uint64_t i, uint64_t c) {
    F2D_NUM_T acc = F2D_FN(Cell_)(t, i, c);
    const uint64_t stop = fenwickPrev(c);
    for (uint64_t j = c - 1; j > stop; j = fenwickPrev(j)) {
        acc = F2D_NUM_SUB(acc, F2D_FN(Cell_)(t, i, j));
    }

    return acc;
}

/* Element (row, col), inside the extents: the element trick applied along
 * rows and then columns, O(log R * log C) with no prefix sum formed */
static F2D_NUM_T F2D_FN(Element_)(const struct F2D_STRUCT *t, uint64_t row,
                                  uint64_t col) {
    const uint64_t r = row + 1;
    const uint64_t c = col + 1;
    const uint64_t rStop = fenwickPrev(r);
#ifdef F2D_FAST_INT
    if (F2D_ALL_INT(&t->store)) {
        const uint64_t cStop = fenwickPrev(c);
        __int128_t sum = 0;
        for (uint64_t i = r; i > rStop; i = i == r ? r - 1 : fenwickPrev(i)) {
            __int128_t rowSum = 0;
            for (uint64_t j = c; j > cStop;
                 j = j == c ? c - 1 : fenwickPrev(j)) {
                const int64_t v = F2D_INT(&t->store, F2D_FN(Index_)(t, i, j));
                rowSum += j == c ? v : -(__int128_t)v;
            }

            sum += i == r ? rowSum : -rowSum;
        }

        return F2D_NUM_FROM_I128(sum);
    }
#endif

    F2D_NUM_T acc = F2D_FN(RowElement_)(t, r, c);
    for (uint64_t i = r - 1; i > rStop; i = fenwickPrev(i)) {
        acc = F2D_NUM_SUB(acc, F2D_FN(RowElement_)(t, i, c));
    }

    return acc;
}

/* Sum of the rectangle [r1, r2] x [c1, c2] by inclusion-exclusion of four
 * prefix sums; the caller checks r1 <= r2 < rows and c1 <= c2 < cols */
static F2D_NUM_T F2D_FN(Rect_)(const struct F2D_STRUCT *t, uint64_t r1,
                               uint64_t c1, uint64_t r2, uint64_t c2) {
    F2D_NUM_T acc = F2D_FN(Prefix_)(t, r2, c2);
    if (r1 > 0) {
        acc = F2D_NUM_SUB(acc, F2D_FN(Prefix_)(t, r1 - 1, c2));
    }

    if (c1 > 0) {
        acc = F2D_NUM_SUB(acc, F2D_FN(Prefix_)(t, r2, c1 - 1));
    }

    if (r1 > 0 && c1 > 0) {
        acc = F2D_NUM_ADD(acc, F2D_FN(Prefix_)(t, r1 - 1, c1 - 1));
    }

    return acc;
}

#ifdef F2D_FAST_INT
/* The integer fast write: within the capacity, into an all-int64 store,
 * every touched cell staying an int64.  False (nothing written) when any
 * of that does not hold. */
static bool F2D_FN(WriteInt_)(struct F2D_STRUCT *t, uint64_t row, uint64_t col,
                              F2D_NUM_T delta) {
    if (!F2D_ALL_INT(&t->store) || !F2D_NUM_IS_INT(delta) || row >= t->rowCap ||
        col >= t->colCap) {
        return false;
    }

    const __int128_t d = F2D_NUM_I128(delta);
    int64_t plan[F2D_MAX_CHAIN];
    size_t n = 0;
    for (uint64_t i = row + 1; i <= t->rowCap; i = fenwickParent(i)) {
        for (uint64_t j = col + 1; j <= t->colCap; j = fenwickParent(j)) {
            const __int128_t v =
                F2D_INT(&t->store, F2D_FN(Index_)(t, i, j)) + d;
            if (v < INT64_MIN || v > INT64_MAX) {
                return false;
            }

            plan[n++] = (int64_t)v;
        }
    }

    n = 0;
    for (uint64_t i = row + 1; i <= t->rowCap; i = fenwickParent(i)) {
        for (uint64_t j = col + 1; j <= t->colCap; j = fenwickParent(j)) {
            F2D_PUT_INT(&t->store, F2D_FN(Index_)(t, i, j), plan[n++]);
        }
    }

    F2D_FN(Extend_)(t, row, col);
    return true;
}
#endif

/* Write delta at (row, col): plan against the grown tree, then grow and
 * apply.  False, changing nothing, when the capacity would pass
 * F2D_MAX_CELLS or a cell cannot hold its new value. */
static bool F2D_FN(Write_)(struct F2D_STRUCT *t, uint64_t row, uint64_t col,
                           F2D_NUM_T delta) {
#ifdef F2D_FAST_INT
    if (F2D_FN(WriteInt_)(t, row, col, delta)) {
        return true;
    }
#endif

    uint64_t rowCap;
    uint64_t colCap;
    F2D_PLAN_T plan[F2D_MAX_CHAIN];
    if (!F2D_FN(CapsFor_)(t, row, col, &rowCap, &colCap) ||
        !F2D_FN(Plan_)(t, row, col, rowCap, colCap, delta, plan)) {
        return false;
    }

    F2D_FN(Grow_)(t, rowCap, colCap);
    F2D_FN(Apply_)(t, row, col, plan);
    F2D_FN(Extend_)(t, row, col);
    return true;
}

static size_t F2D_FN(Bytes_)(const struct F2D_STRUCT *t) {
    return sizeof(*t) + F2D_BYTES(&t->store, t->rowCap * t->colCap);
}

static void F2D_FN(Clear_)(struct F2D_STRUCT *t) {
    F2D_CLEAR(&t->store, t->rowCap * t->colCap);
}

static void F2D_FN(FreeStore_)(struct F2D_STRUCT *t) {
    F2D_FREE(&t->store);
}

#undef F2D_MAX_CELLS
#undef F2D_MAX_CHAIN
