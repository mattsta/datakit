#pragma once

/* persistOps for the typed Fenwick trees (fenwickCoreDispatchImpl.h).
 *
 * Integer trees snapshot their elements in index order and restore them
 * through fenwick<T>NewFromArray.  Float and Double trees snapshot their
 * capacity, count and every cell, restored bit for bit: their cells are
 * rounded sums, so the elements alone do not determine the tree.
 *
 * WAL (args: const databox *const *, two of them):
 *   NUM_INCR [index (UNSIGNED_64), delta]   fenwick<T>Update
 *   UPDATE   [index (UNSIGNED_64), value]   fenwick<T>Set
 *   CLEAR                                   fenwick<T>Clear
 * A value box is any integer box whose value fits T (128-bit boxes for
 * the 128-bit trees), FLOAT_32 for Float, DOUBLE_64 or FLOAT_32 for
 * Double.  Log only writes the tree accepted: a refused Update or Set is
 * refused again on replay. */

#include "../persist.h"

extern const persistOps persistOpsFenwickI16;
extern const persistOps persistOpsFenwickI32;
extern const persistOps persistOpsFenwickI64;
extern const persistOps persistOpsFenwickU16;
extern const persistOps persistOpsFenwickU32;
extern const persistOps persistOpsFenwickU64;
extern const persistOps persistOpsFenwickFloat;
extern const persistOps persistOpsFenwickDouble;
#ifdef __SIZEOF_INT128__
extern const persistOps persistOpsFenwickI128;
extern const persistOps persistOpsFenwickU128;
#endif

/* The 2D trees (fenwick2dTypedImpl.h), dense in the same way:
 *   integer: flex [rows, cols, element (0,0), (0,1), ... row-major]
 *   float:   flex [rowCap, colCap, rows, cols, every cell row-major]
 * WAL as above with [row, col, value] operands (three boxes). */
extern const persistOps persistOpsFenwick2dI16;
extern const persistOps persistOpsFenwick2dI32;
extern const persistOps persistOpsFenwick2dI64;
extern const persistOps persistOpsFenwick2dU16;
extern const persistOps persistOpsFenwick2dU32;
extern const persistOps persistOpsFenwick2dU64;
extern const persistOps persistOpsFenwick2dFloat;
extern const persistOps persistOpsFenwick2dDouble;
#ifdef __SIZEOF_INT128__
extern const persistOps persistOpsFenwick2dI128;
extern const persistOps persistOpsFenwick2dU128;
#endif
