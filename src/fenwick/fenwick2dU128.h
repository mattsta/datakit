#pragma once

/* 2D Fenwick Tree - __uint128_t: a growable grid of __uint128_t elements
 * with point updates and rectangle sums, 16 bytes per capacity cell.
 * Declared only where the compiler has __int128 (__SIZEOF_INT128__).
 * Contract (growth, values, ranges): fenwick2d.h */

#include <stddef.h>
#include <stdint.h>

#ifdef __SIZEOF_INT128__

typedef struct fenwick2dU128 fenwick2dU128;

[[nodiscard]] fenwick2dU128 *fenwick2dU128New(void);
void fenwick2dU128Free(fenwick2dU128 *t);
bool fenwick2dU128Update(fenwick2dU128 **t, size_t row, size_t col,
                         __uint128_t delta);
bool fenwick2dU128Set(fenwick2dU128 **t, size_t row, size_t col,
                      __uint128_t value);
__uint128_t fenwick2dU128Get(const fenwick2dU128 *t, size_t row, size_t col);
__uint128_t fenwick2dU128Query(const fenwick2dU128 *t, size_t row, size_t col);
__uint128_t fenwick2dU128RangeQuery(const fenwick2dU128 *t, size_t r1,
                                    size_t c1, size_t r2, size_t c2);
size_t fenwick2dU128Rows(const fenwick2dU128 *t);
size_t fenwick2dU128Cols(const fenwick2dU128 *t);
size_t fenwick2dU128Bytes(const fenwick2dU128 *t);
void fenwick2dU128Clear(fenwick2dU128 *t);
/* A rows x cols tree of row-major elements in O(cells) (either extent 0
 * or NULL: an empty tree; NULL past the cell limit) */
[[nodiscard]] fenwick2dU128 *
fenwick2dU128NewFromArray(const __uint128_t *values, size_t rows, size_t cols);
/* Every element, as Get returns it, row-major into out[Rows * Cols] */
void fenwick2dU128CopyTo(const fenwick2dU128 *t, __uint128_t *out);

#endif /* __SIZEOF_INT128__ */
