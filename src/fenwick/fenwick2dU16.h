#pragma once

/* 2D Fenwick Tree - uint16_t: a growable grid of uint16_t elements with
 * point updates and rectangle sums, 2 bytes per capacity cell.
 * Contract (growth, values, ranges): fenwick2d.h */

#include <stddef.h>
#include <stdint.h>

/* @handle: moves: a call that can move it takes its address ('**') */
typedef struct fenwick2dU16 fenwick2dU16;

[[nodiscard]] fenwick2dU16 *fenwick2dU16New(void);
void fenwick2dU16Free(fenwick2dU16 *t);
bool fenwick2dU16Update(fenwick2dU16 **t, size_t row, size_t col,
                        uint16_t delta);
bool fenwick2dU16Set(fenwick2dU16 **t, size_t row, size_t col, uint16_t value);
uint16_t fenwick2dU16Get(const fenwick2dU16 *t, size_t row, size_t col);
uint16_t fenwick2dU16Query(const fenwick2dU16 *t, size_t row, size_t col);
uint16_t fenwick2dU16RangeQuery(const fenwick2dU16 *t, size_t r1, size_t c1,
                                size_t r2, size_t c2);
size_t fenwick2dU16Rows(const fenwick2dU16 *t);
size_t fenwick2dU16Cols(const fenwick2dU16 *t);
size_t fenwick2dU16Bytes(const fenwick2dU16 *t);
void fenwick2dU16Clear(fenwick2dU16 *t);
/* A rows x cols tree of row-major elements in O(cells) (either extent 0
 * or NULL: an empty tree; NULL past the cell limit) */
[[nodiscard]] fenwick2dU16 *fenwick2dU16NewFromArray(const uint16_t *values,
                                                     size_t rows, size_t cols);
/* Every element, as Get returns it, row-major into out[Rows * Cols] */
void fenwick2dU16CopyTo(const fenwick2dU16 *t, uint16_t *out);
