#pragma once

/* 2D Fenwick Tree - __int128_t: a growable grid of __int128_t elements
 * with point updates and rectangle sums, 16 bytes per capacity cell.
 * Declared only where the compiler has __int128 (__SIZEOF_INT128__).
 * Contract (growth, values, ranges): fenwick2d.h */

#include <stddef.h>
#include <stdint.h>

#ifdef __SIZEOF_INT128__

typedef struct fenwick2dI128 fenwick2dI128;

[[nodiscard]] fenwick2dI128 *fenwick2dI128New(void);
void fenwick2dI128Free(fenwick2dI128 *t);
bool fenwick2dI128Update(fenwick2dI128 **t, size_t row, size_t col,
                         __int128_t delta);
bool fenwick2dI128Set(fenwick2dI128 **t, size_t row, size_t col,
                      __int128_t value);
__int128_t fenwick2dI128Get(const fenwick2dI128 *t, size_t row, size_t col);
__int128_t fenwick2dI128Query(const fenwick2dI128 *t, size_t row, size_t col);
__int128_t fenwick2dI128RangeQuery(const fenwick2dI128 *t, size_t r1, size_t c1,
                                   size_t r2, size_t c2);
size_t fenwick2dI128Rows(const fenwick2dI128 *t);
size_t fenwick2dI128Cols(const fenwick2dI128 *t);
size_t fenwick2dI128Bytes(const fenwick2dI128 *t);
void fenwick2dI128Clear(fenwick2dI128 *t);
/* A rows x cols tree of row-major elements in O(cells) (either extent 0
 * or NULL: an empty tree; NULL past the cell limit) */
[[nodiscard]] fenwick2dI128 *
fenwick2dI128NewFromArray(const __int128_t *values, size_t rows, size_t cols);
/* Every element, as Get returns it, row-major into out[Rows * Cols] */
void fenwick2dI128CopyTo(const fenwick2dI128 *t, __int128_t *out);

#endif /* __SIZEOF_INT128__ */
