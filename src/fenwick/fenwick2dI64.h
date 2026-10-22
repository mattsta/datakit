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
