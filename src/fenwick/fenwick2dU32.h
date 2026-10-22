#pragma once

/* 2D Fenwick Tree - uint32_t: a growable grid of uint32_t elements with
 * point updates and rectangle sums, 4 bytes per capacity cell.
 * Contract (growth, values, ranges): fenwick2d.h */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct fenwick2dU32 fenwick2dU32;

fenwick2dU32 *fenwick2dU32New(void);
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
