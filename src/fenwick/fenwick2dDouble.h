#pragma once

/* 2D Fenwick Tree - double: a growable grid of finite double elements
 * with point updates and rectangle sums, 8 bytes per capacity cell.
 * Contract (growth, values, ranges): fenwick2d.h */

#include <stddef.h>
#include <stdint.h>

/* @handle: moves: a call that can move it takes its address ('**') */
typedef struct fenwick2dDouble fenwick2dDouble;

[[nodiscard]] fenwick2dDouble *fenwick2dDoubleNew(void);
void fenwick2dDoubleFree(fenwick2dDouble *t);
bool fenwick2dDoubleUpdate(fenwick2dDouble **t, size_t row, size_t col,
                           double delta);
bool fenwick2dDoubleSet(fenwick2dDouble **t, size_t row, size_t col,
                        double value);
double fenwick2dDoubleGet(const fenwick2dDouble *t, size_t row, size_t col);
double fenwick2dDoubleQuery(const fenwick2dDouble *t, size_t row, size_t col);
double fenwick2dDoubleRangeQuery(const fenwick2dDouble *t, size_t r1, size_t c1,
                                 size_t r2, size_t c2);
size_t fenwick2dDoubleRows(const fenwick2dDouble *t);
size_t fenwick2dDoubleCols(const fenwick2dDouble *t);
size_t fenwick2dDoubleBytes(const fenwick2dDouble *t);
void fenwick2dDoubleClear(fenwick2dDouble *t);
/* A rows x cols tree of row-major elements in O(cells) (either extent 0
 * or NULL: an empty tree; NULL past the cell limit or for a value that is
 * not finite) */
[[nodiscard]] fenwick2dDouble *
fenwick2dDoubleNewFromArray(const double *values, size_t rows, size_t cols);
/* Every element, as Get returns it, row-major into out[Rows * Cols] */
void fenwick2dDoubleCopyTo(const fenwick2dDouble *t, double *out);
