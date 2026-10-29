#pragma once

/* 2D Fenwick Tree - uint64_t: a growable grid of uint64_t elements with
 * point updates and rectangle sums, 8 bytes per capacity cell.
 * Contract (growth, values, ranges): fenwick2d.h */

#include <stddef.h>
#include <stdint.h>

/* @handle: moves: a call that can move it takes its address ('**') */
typedef struct fenwick2dU64 fenwick2dU64;

[[nodiscard]] fenwick2dU64 *fenwick2dU64New(void);
void fenwick2dU64Free(fenwick2dU64 *t);
bool fenwick2dU64Update(fenwick2dU64 **t, size_t row, size_t col,
                        uint64_t delta);
bool fenwick2dU64Set(fenwick2dU64 **t, size_t row, size_t col, uint64_t value);
uint64_t fenwick2dU64Get(const fenwick2dU64 *t, size_t row, size_t col);
uint64_t fenwick2dU64Query(const fenwick2dU64 *t, size_t row, size_t col);
uint64_t fenwick2dU64RangeQuery(const fenwick2dU64 *t, size_t r1, size_t c1,
                                size_t r2, size_t c2);
size_t fenwick2dU64Rows(const fenwick2dU64 *t);
size_t fenwick2dU64Cols(const fenwick2dU64 *t);
size_t fenwick2dU64Bytes(const fenwick2dU64 *t);
void fenwick2dU64Clear(fenwick2dU64 *t);
/* A rows x cols tree of row-major elements in O(cells) (either extent 0
 * or NULL: an empty tree; NULL past the cell limit) */
[[nodiscard]] fenwick2dU64 *fenwick2dU64NewFromArray(const uint64_t *values,
                                                     size_t rows, size_t cols);
/* Every element, as Get returns it, row-major into out[Rows * Cols] */
void fenwick2dU64CopyTo(const fenwick2dU64 *t, uint64_t *out);
