#pragma once

/* 2D Fenwick Tree - float: a growable grid of finite float elements with
 * point updates and rectangle sums (computed in float), 4 bytes per
 * capacity cell.  Contract (growth, values, ranges): fenwick2d.h */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct fenwick2dFloat fenwick2dFloat;

fenwick2dFloat *fenwick2dFloatNew(void);
void fenwick2dFloatFree(fenwick2dFloat *t);
bool fenwick2dFloatUpdate(fenwick2dFloat **t, size_t row, size_t col,
                          float delta);
bool fenwick2dFloatSet(fenwick2dFloat **t, size_t row, size_t col, float value);
float fenwick2dFloatGet(const fenwick2dFloat *t, size_t row, size_t col);
float fenwick2dFloatQuery(const fenwick2dFloat *t, size_t row, size_t col);
float fenwick2dFloatRangeQuery(const fenwick2dFloat *t, size_t r1, size_t c1,
                               size_t r2, size_t c2);
size_t fenwick2dFloatRows(const fenwick2dFloat *t);
size_t fenwick2dFloatCols(const fenwick2dFloat *t);
size_t fenwick2dFloatBytes(const fenwick2dFloat *t);
void fenwick2dFloatClear(fenwick2dFloat *t);
