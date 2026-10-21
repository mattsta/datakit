#pragma once

#include "databox.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ====================================================================
 * 2D Multi-Type Fenwick Tree (databox values)
 * ====================================================================
 *
 * A grid of numbers e[row][col] (0-based) with point updates and
 * rectangle sums, each O(log R * log C), taking and returning databox
 * values.  It is the 2D form of multiFenwickFlat: the same accepted
 * values, exact arithmetic and value-typed results (multiFenwick.h,
 * VALUES), on the 2D layout and growth of the typed trees (fenwick2d.h).
 * Cells use multiFenwickFlat's adaptive store (multiFenwickCells.h):
 * rowCap x colCap cells of 8 bytes while every cell is an int64 (sums and
 * writes then take integer-only paths), plus 1 byte per cell once any
 * cell holds a value above INT64_MAX or a double.  Clear drops back to 8.
 *
 * VALUES
 * ------
 * Accepted: SIGNED_64, UNSIGNED_64, and finite FLOAT_32 or DOUBLE_64;
 * anything else is refused.  Integer arithmetic is exact (int128).  Each
 * stored cell holds the sum of a rectangle of elements and must fit
 * [INT64_MIN, UINT64_MAX] (or be a finite double): Update and Set refuse,
 * changing nothing (a NULL tree stays NULL), any write that would push a
 * cell outside, and any write that would grow the grid past 2^32 cells.
 * Results are typed by value: an integer result is SIGNED_64 when
 * negative, UNSIGNED_64 otherwise; a result with any floating part is
 * DOUBLE_64.  Floating arithmetic runs in double.
 *
 * RANGES
 * ------
 * As for the typed trees: Query(row, col) sums [0, row] x [0, col] and
 * RangeQuery sums [r1, r2] x [c1, c2], both clamped to the extents (an
 * empty rectangle sums to 0); Get outside the extents is 0; a NULL tree
 * is empty.  Get, Query and RangeQuery return false (result VOID) only
 * for a NULL result or a result outside [INT64_MIN, UINT64_MAX] (or not
 * finite).
 *
 * Not thread-safe. */

typedef struct multiFenwick2d multiFenwick2d;

multiFenwick2d *multiFenwick2dNew(void);
void multiFenwick2dFree(multiFenwick2d *t);

bool multiFenwick2dUpdate(multiFenwick2d **t, size_t row, size_t col,
                          const databox *delta);
bool multiFenwick2dSet(multiFenwick2d **t, size_t row, size_t col,
                       const databox *value);

bool multiFenwick2dGet(const multiFenwick2d *t, size_t row, size_t col,
                       databox *result);
bool multiFenwick2dQuery(const multiFenwick2d *t, size_t row, size_t col,
                         databox *result);
bool multiFenwick2dRangeQuery(const multiFenwick2d *t, size_t r1, size_t c1,
                              size_t r2, size_t c2, databox *result);

size_t multiFenwick2dRows(const multiFenwick2d *t);
size_t multiFenwick2dCols(const multiFenwick2d *t);
size_t multiFenwick2dBytes(const multiFenwick2d *t);
void multiFenwick2dClear(multiFenwick2d *t);

#ifdef DATAKIT_TEST
int multiFenwick2dTest(int argc, char *argv[]);
#endif
