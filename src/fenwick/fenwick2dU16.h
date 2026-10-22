#pragma once

/* 2D Fenwick Tree - uint16_t: a growable grid of uint16_t elements with
 * point updates and rectangle sums, 2 bytes per capacity cell.
 * Contract (growth, values, ranges): fenwick2d.h */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct fenwick2dU16 fenwick2dU16;

fenwick2dU16 *fenwick2dU16New(void);
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
