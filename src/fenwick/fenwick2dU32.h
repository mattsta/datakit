#pragma once

/* 2D Fenwick Tree - uint32_t: a growable grid of uint32_t elements with
 * point updates and rectangle sums, 4 bytes per capacity cell.
 * Contract (growth, values, ranges): fenwick2d.h */

#include <stddef.h>
#include <stdint.h>

/* @handle: moves: a call that can move it takes its address ('**') */
typedef struct fenwick2dU32 fenwick2dU32;

[[nodiscard]] fenwick2dU32 *fenwick2dU32New(void);
void fenwick2dU32Free(fenwick2dU32 *t);
bool fenwick2dU32Update(fenwick2dU32 **t, size_t row, size_t col,
                        uint32_t delta);
bool fenwick2dU32Set(fenwick2dU32 **t, size_t row, size_t col, uint32_t value);
uint32_t fenwick2dU32Get(const fenwick2dU32 *t, size_t row, size_t col);
uint32_t fenwick2dU32Query(const fenwick2dU32 *t, size_t row, size_t col);
uint32_t fenwick2dU32RangeQuery(const fenwick2dU32 *t, size_t r1, size_t c1,
                                size_t r2, size_t c2);
size_t fenwick2dU32Rows(const fenwick2dU32 *t);
size_t fenwick2dU32Cols(const fenwick2dU32 *t);
size_t fenwick2dU32Bytes(const fenwick2dU32 *t);
void fenwick2dU32Clear(fenwick2dU32 *t);
/* A rows x cols tree of row-major elements in O(cells) (either extent 0
 * or NULL: an empty tree; NULL past the cell limit) */
[[nodiscard]] fenwick2dU32 *fenwick2dU32NewFromArray(const uint32_t *values,
                                                     size_t rows, size_t cols);
/* Every element, as Get returns it, row-major into out[Rows * Cols] */
void fenwick2dU32CopyTo(const fenwick2dU32 *t, uint32_t *out);
