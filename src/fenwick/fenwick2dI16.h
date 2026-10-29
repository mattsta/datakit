#pragma once

/* 2D Fenwick Tree - int16_t: a growable grid of int16_t elements with
 * point updates and rectangle sums, 2 bytes per capacity cell.
 * Contract (growth, values, ranges): fenwick2d.h */

#include <stddef.h>
#include <stdint.h>

/* @handle: moves: a call that can move it takes its address ('**') */
typedef struct fenwick2dI16 fenwick2dI16;

[[nodiscard]] fenwick2dI16 *fenwick2dI16New(void);
void fenwick2dI16Free(fenwick2dI16 *t);
bool fenwick2dI16Update(fenwick2dI16 **t, size_t row, size_t col,
                        int16_t delta);
bool fenwick2dI16Set(fenwick2dI16 **t, size_t row, size_t col, int16_t value);
int16_t fenwick2dI16Get(const fenwick2dI16 *t, size_t row, size_t col);
int16_t fenwick2dI16Query(const fenwick2dI16 *t, size_t row, size_t col);
int16_t fenwick2dI16RangeQuery(const fenwick2dI16 *t, size_t r1, size_t c1,
                               size_t r2, size_t c2);
size_t fenwick2dI16Rows(const fenwick2dI16 *t);
size_t fenwick2dI16Cols(const fenwick2dI16 *t);
size_t fenwick2dI16Bytes(const fenwick2dI16 *t);
void fenwick2dI16Clear(fenwick2dI16 *t);
/* A rows x cols tree of row-major elements in O(cells) (either extent 0
 * or NULL: an empty tree; NULL past the cell limit) */
[[nodiscard]] fenwick2dI16 *fenwick2dI16NewFromArray(const int16_t *values,
                                                     size_t rows, size_t cols);
/* Every element, as Get returns it, row-major into out[Rows * Cols] */
void fenwick2dI16CopyTo(const fenwick2dI16 *t, int16_t *out);
