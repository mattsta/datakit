#pragma once

/* ====================================================================
 * 2D Fenwick Trees (typed): fenwick2dI16 ... fenwick2dDouble
 * ====================================================================
 *
 * One tree per value type, each in its own header and source, like the
 * 1D family: fenwick2dI16.h, fenwick2dI32.h, fenwick2dI64.h,
 * fenwick2dI128.h, fenwick2dU16.h, fenwick2dU32.h, fenwick2dU64.h,
 * fenwick2dU128.h, fenwick2dFloat.h and fenwick2dDouble.h.  Include one
 * of them for one type, or this header for all.  The 128-bit trees exist
 * where the compiler has __int128 (__SIZEOF_INT128__).  Every type shares
 * the contract below.
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
 *   element would not fit the type; Set always fits.  The Float and
 *   Double trees refuse a non-finite delta, value or element.
 * - Integer trees: Get returns the element exactly.  Query and
 *   RangeQuery return the exact sum whenever it fits the type; an integer
 *   sum that does not fit comes back modulo 2^N for the type's N bits
 *   (cells add with wrapping, never UB).
 * - Float and Double trees round as the 1D ones do (fenwickCore.h):
 *   cells are sums in the tree's type and Get is a difference of cells,
 *   so elements and sums are exact only while every partial sum is.
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

#include "fenwick2dDouble.h"
#include "fenwick2dFloat.h"
#include "fenwick2dI128.h"
#include "fenwick2dI16.h"
#include "fenwick2dI32.h"
#include "fenwick2dI64.h"
#include "fenwick2dU128.h"
#include "fenwick2dU16.h"
#include "fenwick2dU32.h"
#include "fenwick2dU64.h"

#ifdef DATAKIT_TEST
int fenwick2dTest(int argc, char *argv[]);
#endif
