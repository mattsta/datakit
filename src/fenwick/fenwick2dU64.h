#pragma once

/* 2D Fenwick Tree - uint64_t: a growable grid of uint64_t elements with
 * point updates and rectangle sums, 8 bytes per capacity cell.
 * Contract (growth, values, ranges): fenwick2d.h */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct fenwick2dU64 fenwick2dU64;

fenwick2dU64 *fenwick2dU64New(void);
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
