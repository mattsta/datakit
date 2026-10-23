#pragma once

/* 2D Fenwick Tree - int64_t: a growable grid of int64_t elements with
 * point updates and rectangle sums, 8 bytes per capacity cell.
 * Contract (growth, values, ranges): fenwick2d.h */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct fenwick2dI64 fenwick2dI64;

fenwick2dI64 *fenwick2dI64New(void);
void fenwick2dI64Free(fenwick2dI64 *t);
bool fenwick2dI64Update(fenwick2dI64 **t, size_t row, size_t col,
                        int64_t delta);
bool fenwick2dI64Set(fenwick2dI64 **t, size_t row, size_t col, int64_t value);
int64_t fenwick2dI64Get(const fenwick2dI64 *t, size_t row, size_t col);
int64_t fenwick2dI64Query(const fenwick2dI64 *t, size_t row, size_t col);
int64_t fenwick2dI64RangeQuery(const fenwick2dI64 *t, size_t r1, size_t c1,
                               size_t r2, size_t c2);
size_t fenwick2dI64Rows(const fenwick2dI64 *t);
size_t fenwick2dI64Cols(const fenwick2dI64 *t);
size_t fenwick2dI64Bytes(const fenwick2dI64 *t);
void fenwick2dI64Clear(fenwick2dI64 *t);
/* A rows x cols tree of row-major elements in O(cells) (either extent 0
 * or NULL: an empty tree; NULL past the cell limit) */
fenwick2dI64 *fenwick2dI64NewFromArray(const int64_t *values, size_t rows,
                                       size_t cols);
/* Every element, as Get returns it, row-major into out[Rows * Cols] */
void fenwick2dI64CopyTo(const fenwick2dI64 *t, int64_t *out);
