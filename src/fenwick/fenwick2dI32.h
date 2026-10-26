#pragma once

/* 2D Fenwick Tree - int32_t: a growable grid of int32_t elements with
 * point updates and rectangle sums, 4 bytes per capacity cell.
 * Contract (growth, values, ranges): fenwick2d.h */

#include <stddef.h>
#include <stdint.h>

typedef struct fenwick2dI32 fenwick2dI32;

[[nodiscard]] fenwick2dI32 *fenwick2dI32New(void);
void fenwick2dI32Free(fenwick2dI32 *t);
bool fenwick2dI32Update(fenwick2dI32 **t, size_t row, size_t col,
                        int32_t delta);
bool fenwick2dI32Set(fenwick2dI32 **t, size_t row, size_t col, int32_t value);
int32_t fenwick2dI32Get(const fenwick2dI32 *t, size_t row, size_t col);
int32_t fenwick2dI32Query(const fenwick2dI32 *t, size_t row, size_t col);
int32_t fenwick2dI32RangeQuery(const fenwick2dI32 *t, size_t r1, size_t c1,
                               size_t r2, size_t c2);
size_t fenwick2dI32Rows(const fenwick2dI32 *t);
size_t fenwick2dI32Cols(const fenwick2dI32 *t);
size_t fenwick2dI32Bytes(const fenwick2dI32 *t);
void fenwick2dI32Clear(fenwick2dI32 *t);
/* A rows x cols tree of row-major elements in O(cells) (either extent 0
 * or NULL: an empty tree; NULL past the cell limit) */
[[nodiscard]] fenwick2dI32 *fenwick2dI32NewFromArray(const int32_t *values,
                                                     size_t rows, size_t cols);
/* Every element, as Get returns it, row-major into out[Rows * Cols] */
void fenwick2dI32CopyTo(const fenwick2dI32 *t, int32_t *out);
