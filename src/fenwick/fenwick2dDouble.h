#pragma once

/* 2D Fenwick Tree - double: a growable grid of finite double elements
 * with point updates and rectangle sums, 8 bytes per capacity cell.
 * Contract (growth, values, ranges): fenwick2d.h */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct fenwick2dDouble fenwick2dDouble;

fenwick2dDouble *fenwick2dDoubleNew(void);
void fenwick2dDoubleFree(fenwick2dDouble *t);
bool fenwick2dDoubleUpdate(fenwick2dDouble **t, size_t row, size_t col,
                           double delta);
bool fenwick2dDoubleSet(fenwick2dDouble **t, size_t row, size_t col,
                        double value);
double fenwick2dDoubleGet(const fenwick2dDouble *t, size_t row, size_t col);
double fenwick2dDoubleQuery(const fenwick2dDouble *t, size_t row, size_t col);
double fenwick2dDoubleRangeQuery(const fenwick2dDouble *t, size_t r1, size_t c1,
                                 size_t r2, size_t c2);
size_t fenwick2dDoubleRows(const fenwick2dDouble *t);
size_t fenwick2dDoubleCols(const fenwick2dDouble *t);
size_t fenwick2dDoubleBytes(const fenwick2dDouble *t);
void fenwick2dDoubleClear(fenwick2dDouble *t);
