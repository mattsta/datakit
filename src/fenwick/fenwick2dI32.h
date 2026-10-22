#pragma once

/* 2D Fenwick Tree - int32_t: a growable grid of int32_t elements with
 * point updates and rectangle sums, 4 bytes per capacity cell.
 * Contract (growth, values, ranges): fenwick2d.h */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct fenwick2dI32 fenwick2dI32;

fenwick2dI32 *fenwick2dI32New(void);
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
