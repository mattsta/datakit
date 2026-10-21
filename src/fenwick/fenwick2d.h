#pragma once

/* ====================================================================
 * 2D Fenwick Trees (typed): fenwick2dI64, fenwick2dU64, fenwick2dDouble
 * ====================================================================
 *
 * A grid of numbers e[row][col] (0-based) with point updates and
 * rectangle sums, each O(log R * log C).  The grid grows on demand: any
 * row and column can be written, and the extents (Rows, Cols) are the
 * highest written index + 1 in each dimension; elements never written
 * are zero.  Storage is one contiguous array of capacity cells
 * (rowCap x colCap, each a power of two at or above the extent), so
 * memory is sizeof(value) * rowCap * colCap.  For databox values with
 * exact int128 sums, see multiFenwick2d.h.
 *
 * VALUES (as for the 1D typed trees, fenwickCore.h)
 * ------
 * - Elements are stored values and never wrap: Update refuses (returns
 *   false, changing nothing; a NULL tree stays NULL) a delta whose new
 *   element would not fit the type; Set always fits.  The Double tree
 *   refuses a non-finite delta, value or element.
 * - Get returns the element exactly.  Query and RangeQuery return the
 *   exact sum whenever it fits the type; an integer sum that does not fit
 *   comes back modulo 2^64 (cells add with wrapping, never UB).
 * - A write that would grow the grid past 2^32 cells is refused.
 *
 * RANGES
 * ------
 * - Query(row, col) is the sum of [0, row] x [0, col], clamped to the
 *   extents (so any row and column may be asked; elements past the
 *   extents are zero).  RangeQuery(r1, c1, r2, c2) is the sum of the
 *   rectangle [r1, r2] x [c1, c2], likewise clamped; an empty rectangle
 *   (r1 > r2 or c1 > c2 after clamping) sums to zero.
 * - Get outside the extents returns zero.  A NULL tree is empty.
 *
 * Not thread-safe. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FENWICK2D_DECLARE(suffix, value_type)                                  \
    typedef struct fenwick2d##suffix fenwick2d##suffix;                        \
    fenwick2d##suffix *fenwick2d##suffix##New(void);                           \
    void fenwick2d##suffix##Free(fenwick2d##suffix *t);                        \
    bool fenwick2d##suffix##Update(fenwick2d##suffix **t, size_t row,          \
                                   size_t col, value_type delta);              \
    bool fenwick2d##suffix##Set(fenwick2d##suffix **t, size_t row, size_t col, \
                                value_type value);                             \
    value_type fenwick2d##suffix##Get(const fenwick2d##suffix *t, size_t row,  \
                                      size_t col);                             \
    value_type fenwick2d##suffix##Query(const fenwick2d##suffix *t,            \
                                        size_t row, size_t col);               \
    value_type fenwick2d##suffix##RangeQuery(const fenwick2d##suffix *t,       \
                                             size_t r1, size_t c1, size_t r2,  \
                                             size_t c2);                       \
    size_t fenwick2d##suffix##Rows(const fenwick2d##suffix *t);                \
    size_t fenwick2d##suffix##Cols(const fenwick2d##suffix *t);                \
    size_t fenwick2d##suffix##Bytes(const fenwick2d##suffix *t);               \
    void fenwick2d##suffix##Clear(fenwick2d##suffix *t);

FENWICK2D_DECLARE(I64, int64_t)
FENWICK2D_DECLARE(U64, uint64_t)
FENWICK2D_DECLARE(Double, double)

#ifdef DATAKIT_TEST
int fenwick2dTest(int argc, char *argv[]);
#endif
