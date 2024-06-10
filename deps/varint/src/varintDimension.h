#pragma once

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "varint.h"
__BEGIN_DECLS

/* Format: N
 *   N is number of bytes for [X][Y] packed storage.
 *   N is: max(ceil(l(x)/l(2)), ceil(l(y)/l(2))) */
typedef enum varintDimensionPacked {
    VARINT_DIMENSION_PACKED_1 = 1, /* up to 15 x 15 */
    VARINT_DIMENSION_PACKED_2,     /* up to 255 x 255 */
    VARINT_DIMENSION_PACKED_3,     /* up to 4095 x 4095 */
    VARINT_DIMENSION_PACKED_4,     /* up to 65535 x 65535 */
    VARINT_DIMENSION_PACKED_5,     /* up to 1048575 x 1048575 */
    VARINT_DIMENSION_PACKED_6,     /* up to 16777215 x 16777215 */
    VARINT_DIMENSION_PACKED_7,     /* up to 268435455 x 268435455 */
    VARINT_DIMENSION_PACKED_8,     /* up to 4294967295 x 4294967295 */
    /* Dimensions > 8 not current supported, but here are their numbers: */
    VARINT_DIMENSION_PACKED_9,  /* up to 68719476735 x 68719476735 */
    VARINT_DIMENSION_PACKED_10, /* up to 1099511627775 * 1099511627775 */
    VARINT_DIMENSION_PACKED_11, /* up to 17592186044415 * 17592186044415 */
    VARINT_DIMENSION_PACKED_12, /* up to 281474976710655 x 281474976710655 */
} varintDimensionPacked;
/* DIMENSION_12 would support a vector of binary entries (0,1) up to 35 TB
 * (that's 281,474,976,710,655 individual elements) */

/* Format: X_Y
 *   X is number of bytes to hold integer describing rows dimension
 *   Y is number of bytes to hold integer describing columns dimension
 *   We allow X == 0 as a special case to describe a vector. */

/* Because we are storing two tiny values and each value only goes up to 8,
 * (note: cols have no *zero* width, so we use a zero-based col to mean 1
 * so we can save one bit in the representation to store spare/dense encoding
 * marker) double-store the x,y varint width in the DimensionPair enum.
 * The top 4 bits are the row width, the next 3 bits are the col width, and the
 * last bit is set iff we are a sparse representation. */
#define VARINT_DIMENSION_PAIR_WIDTH_ROW_COUNT(dim) ((dim) >> 4)
#define VARINT_DIMENSION_PAIR_WIDTH_COL_COUNT(dim) ((((dim) >> 1) & 0x03) + 1)
#define VARINT_DIMENSION_PAIR_IS_SPARSE(dim) ((dim) & 0x01)

#define VARINT_DIMENSION_PAIR_PAIR(x, y, sparse)                               \
    (((x) << 4) | (((y)-1) << 1) | (sparse))

#define VARINT_DIMENSION_PAIR_DEPAIR(x, y, sparse)                             \
    do {                                                                       \
        (x) = VARINT_DIMENSION_PAIR_WIDTH_ROW_COUNT(dimension);                \
        (y) = VARINT_DIMENSION_PAIR_WIDTH_COL_COUNT(dimension);                \
    } while (0)

/* Please ignore the ugly verbosity. Big prefixed namespaces take up space. */
/* Note: the 0xY entries describe vectors with a column length only,
 * no multiple rows. */
/* For readability reasons, we ask clang-format to not break here at 80 cols,
 * otherwise we end up with 200 weirdly formatted lines. */
/* Note: we're lame and call sparse SPRSE so it cleanly aligns with DENSE. */
/* We have 9 * 8 * 2 = 144 top-level encoding possibilities. */
/* clang-format off */
typedef enum varintDimensionPair {
	VARINT_DIMENSION_PAIR_DENSE_0_1 = VARINT_DIMENSION_PAIR_PAIR(0, 1, 0), /* up to 0 x 255 */
	VARINT_DIMENSION_PAIR_SPRSE_0_1 = VARINT_DIMENSION_PAIR_PAIR(0, 1, 1), /* up to 0 x 255 */
	VARINT_DIMENSION_PAIR_DENSE_0_2 = VARINT_DIMENSION_PAIR_PAIR(0, 2, 0), /* up to 0 x 65535 */
	VARINT_DIMENSION_PAIR_SPRSE_0_2 = VARINT_DIMENSION_PAIR_PAIR(0, 2, 1), /* up to 0 x 65535 */
	VARINT_DIMENSION_PAIR_DENSE_0_3 = VARINT_DIMENSION_PAIR_PAIR(0, 3, 0), /* up to 0 x 16777215 */
	VARINT_DIMENSION_PAIR_SPRSE_0_3 = VARINT_DIMENSION_PAIR_PAIR(0, 3, 1), /* up to 0 x 16777215 */
	VARINT_DIMENSION_PAIR_DENSE_0_4 = VARINT_DIMENSION_PAIR_PAIR(0, 4, 0), /* up to 0 x 4294967295 */
	VARINT_DIMENSION_PAIR_SPRSE_0_4 = VARINT_DIMENSION_PAIR_PAIR(0, 4, 1), /* up to 0 x 4294967295 */
	VARINT_DIMENSION_PAIR_DENSE_0_5 = VARINT_DIMENSION_PAIR_PAIR(0, 5, 0), /* up to 0 x 1099511627775 */
	VARINT_DIMENSION_PAIR_SPRSE_0_5 = VARINT_DIMENSION_PAIR_PAIR(0, 5, 1), /* up to 0 x 1099511627775 */
	VARINT_DIMENSION_PAIR_DENSE_0_6 = VARINT_DIMENSION_PAIR_PAIR(0, 6, 0), /* up to 0 x 281474976710655 */
	VARINT_DIMENSION_PAIR_SPRSE_0_6 = VARINT_DIMENSION_PAIR_PAIR(0, 6, 1), /* up to 0 x 281474976710655 */
	VARINT_DIMENSION_PAIR_DENSE_0_7 = VARINT_DIMENSION_PAIR_PAIR(0, 7, 0), /* up to 0 x 72057594037927935 */
	VARINT_DIMENSION_PAIR_SPRSE_0_7 = VARINT_DIMENSION_PAIR_PAIR(0, 7, 1), /* up to 0 x 72057594037927935 */
	VARINT_DIMENSION_PAIR_DENSE_0_8 = VARINT_DIMENSION_PAIR_PAIR(0, 8, 0), /* up to 0 x 18446744073709551615 */
	VARINT_DIMENSION_PAIR_SPRSE_0_8 = VARINT_DIMENSION_PAIR_PAIR(0, 8, 1), /* up to 0 x 18446744073709551615 */
	VARINT_DIMENSION_PAIR_DENSE_1_1 = VARINT_DIMENSION_PAIR_PAIR(1, 1, 0), /* up to 255 x 255 */
	VARINT_DIMENSION_PAIR_SPRSE_1_1 = VARINT_DIMENSION_PAIR_PAIR(1, 1, 1), /* up to 255 x 255 */
	VARINT_DIMENSION_PAIR_DENSE_1_2 = VARINT_DIMENSION_PAIR_PAIR(1, 2, 0), /* up to 255 x 65535 */
	VARINT_DIMENSION_PAIR_SPRSE_1_2 = VARINT_DIMENSION_PAIR_PAIR(1, 2, 1), /* up to 255 x 65535 */
	VARINT_DIMENSION_PAIR_DENSE_1_3 = VARINT_DIMENSION_PAIR_PAIR(1, 3, 0), /* up to 255 x 16777215 */
	VARINT_DIMENSION_PAIR_SPRSE_1_3 = VARINT_DIMENSION_PAIR_PAIR(1, 3, 1), /* up to 255 x 16777215 */
	VARINT_DIMENSION_PAIR_DENSE_1_4 = VARINT_DIMENSION_PAIR_PAIR(1, 4, 0), /* up to 255 x 4294967295 */
	VARINT_DIMENSION_PAIR_SPRSE_1_4 = VARINT_DIMENSION_PAIR_PAIR(1, 4, 1), /* up to 255 x 4294967295 */
	VARINT_DIMENSION_PAIR_DENSE_1_5 = VARINT_DIMENSION_PAIR_PAIR(1, 5, 0), /* up to 255 x 1099511627775 */
	VARINT_DIMENSION_PAIR_SPRSE_1_5 = VARINT_DIMENSION_PAIR_PAIR(1, 5, 1), /* up to 255 x 1099511627775 */
	VARINT_DIMENSION_PAIR_DENSE_1_6 = VARINT_DIMENSION_PAIR_PAIR(1, 6, 0), /* up to 255 x 281474976710655 */
	VARINT_DIMENSION_PAIR_SPRSE_1_6 = VARINT_DIMENSION_PAIR_PAIR(1, 6, 1), /* up to 255 x 281474976710655 */
	VARINT_DIMENSION_PAIR_DENSE_1_7 = VARINT_DIMENSION_PAIR_PAIR(1, 7, 0), /* up to 255 x 72057594037927935 */
	VARINT_DIMENSION_PAIR_SPRSE_1_7 = VARINT_DIMENSION_PAIR_PAIR(1, 7, 1), /* up to 255 x 72057594037927935 */
	VARINT_DIMENSION_PAIR_DENSE_1_8 = VARINT_DIMENSION_PAIR_PAIR(1, 8, 0), /* up to 255 x 18446744073709551615 */
	VARINT_DIMENSION_PAIR_SPRSE_1_8 = VARINT_DIMENSION_PAIR_PAIR(1, 8, 1), /* up to 255 x 18446744073709551615 */
	VARINT_DIMENSION_PAIR_DENSE_2_1 = VARINT_DIMENSION_PAIR_PAIR(2, 1, 0), /* up to 65535 x 255 */
	VARINT_DIMENSION_PAIR_SPRSE_2_1 = VARINT_DIMENSION_PAIR_PAIR(2, 1, 1), /* up to 65535 x 255 */
	VARINT_DIMENSION_PAIR_DENSE_2_2 = VARINT_DIMENSION_PAIR_PAIR(2, 2, 0), /* up to 65535 x 65535 */
	VARINT_DIMENSION_PAIR_SPRSE_2_2 = VARINT_DIMENSION_PAIR_PAIR(2, 2, 1), /* up to 65535 x 65535 */
	VARINT_DIMENSION_PAIR_DENSE_2_3 = VARINT_DIMENSION_PAIR_PAIR(2, 3, 0), /* up to 65535 x 16777215 */
	VARINT_DIMENSION_PAIR_SPRSE_2_3 = VARINT_DIMENSION_PAIR_PAIR(2, 3, 1), /* up to 65535 x 16777215 */
	VARINT_DIMENSION_PAIR_DENSE_2_4 = VARINT_DIMENSION_PAIR_PAIR(2, 4, 0), /* up to 65535 x 4294967295 */
	VARINT_DIMENSION_PAIR_SPRSE_2_4 = VARINT_DIMENSION_PAIR_PAIR(2, 4, 1), /* up to 65535 x 4294967295 */
	VARINT_DIMENSION_PAIR_DENSE_2_5 = VARINT_DIMENSION_PAIR_PAIR(2, 5, 0), /* up to 65535 x 1099511627775 */
	VARINT_DIMENSION_PAIR_SPRSE_2_5 = VARINT_DIMENSION_PAIR_PAIR(2, 5, 1), /* up to 65535 x 1099511627775 */
	VARINT_DIMENSION_PAIR_DENSE_2_6 = VARINT_DIMENSION_PAIR_PAIR(2, 6, 0), /* up to 65535 x 281474976710655 */
	VARINT_DIMENSION_PAIR_SPRSE_2_6 = VARINT_DIMENSION_PAIR_PAIR(2, 6, 1), /* up to 65535 x 281474976710655 */
	VARINT_DIMENSION_PAIR_DENSE_2_7 = VARINT_DIMENSION_PAIR_PAIR(2, 7, 0), /* up to 65535 x 72057594037927935 */
	VARINT_DIMENSION_PAIR_SPRSE_2_7 = VARINT_DIMENSION_PAIR_PAIR(2, 7, 1), /* up to 65535 x 72057594037927935 */
	VARINT_DIMENSION_PAIR_DENSE_2_8 = VARINT_DIMENSION_PAIR_PAIR(2, 8, 0), /* up to 65535 x 18446744073709551615 */
	VARINT_DIMENSION_PAIR_SPRSE_2_8 = VARINT_DIMENSION_PAIR_PAIR(2, 8, 1), /* up to 65535 x 18446744073709551615 */
	VARINT_DIMENSION_PAIR_DENSE_3_1 = VARINT_DIMENSION_PAIR_PAIR(3, 1, 0), /* up to 16777215 x 255 */
	VARINT_DIMENSION_PAIR_SPRSE_3_1 = VARINT_DIMENSION_PAIR_PAIR(3, 1, 1), /* up to 16777215 x 255 */
	VARINT_DIMENSION_PAIR_DENSE_3_2 = VARINT_DIMENSION_PAIR_PAIR(3, 2, 0), /* up to 16777215 x 65535 */
	VARINT_DIMENSION_PAIR_SPRSE_3_2 = VARINT_DIMENSION_PAIR_PAIR(3, 2, 1), /* up to 16777215 x 65535 */
	VARINT_DIMENSION_PAIR_DENSE_3_3 = VARINT_DIMENSION_PAIR_PAIR(3, 3, 0), /* up to 16777215 x 16777215 */
	VARINT_DIMENSION_PAIR_SPRSE_3_3 = VARINT_DIMENSION_PAIR_PAIR(3, 3, 1), /* up to 16777215 x 16777215 */
	VARINT_DIMENSION_PAIR_DENSE_3_4 = VARINT_DIMENSION_PAIR_PAIR(3, 4, 0), /* up to 16777215 x 4294967295 */
	VARINT_DIMENSION_PAIR_SPRSE_3_4 = VARINT_DIMENSION_PAIR_PAIR(3, 4, 1), /* up to 16777215 x 4294967295 */
	VARINT_DIMENSION_PAIR_DENSE_3_5 = VARINT_DIMENSION_PAIR_PAIR(3, 5, 0), /* up to 16777215 x 1099511627775 */
	VARINT_DIMENSION_PAIR_SPRSE_3_5 = VARINT_DIMENSION_PAIR_PAIR(3, 5, 1), /* up to 16777215 x 1099511627775 */
	VARINT_DIMENSION_PAIR_DENSE_3_6 = VARINT_DIMENSION_PAIR_PAIR(3, 6, 0), /* up to 16777215 x 281474976710655 */
	VARINT_DIMENSION_PAIR_SPRSE_3_6 = VARINT_DIMENSION_PAIR_PAIR(3, 6, 1), /* up to 16777215 x 281474976710655 */
	VARINT_DIMENSION_PAIR_DENSE_3_7 = VARINT_DIMENSION_PAIR_PAIR(3, 7, 0), /* up to 16777215 x 72057594037927935 */
	VARINT_DIMENSION_PAIR_SPRSE_3_7 = VARINT_DIMENSION_PAIR_PAIR(3, 7, 1), /* up to 16777215 x 72057594037927935 */
	VARINT_DIMENSION_PAIR_DENSE_3_8 = VARINT_DIMENSION_PAIR_PAIR(3, 8, 0), /* up to 16777215 x 18446744073709551615 */
	VARINT_DIMENSION_PAIR_SPRSE_3_8 = VARINT_DIMENSION_PAIR_PAIR(3, 8, 1), /* up to 16777215 x 18446744073709551615 */
	VARINT_DIMENSION_PAIR_DENSE_4_1 = VARINT_DIMENSION_PAIR_PAIR(4, 1, 0), /* up to 4294967295 x 255 */
	VARINT_DIMENSION_PAIR_SPRSE_4_1 = VARINT_DIMENSION_PAIR_PAIR(4, 1, 1), /* up to 4294967295 x 255 */
	VARINT_DIMENSION_PAIR_DENSE_4_2 = VARINT_DIMENSION_PAIR_PAIR(4, 2, 0), /* up to 4294967295 x 65535 */
	VARINT_DIMENSION_PAIR_SPRSE_4_2 = VARINT_DIMENSION_PAIR_PAIR(4, 2, 1), /* up to 4294967295 x 65535 */
	VARINT_DIMENSION_PAIR_DENSE_4_3 = VARINT_DIMENSION_PAIR_PAIR(4, 3, 0), /* up to 4294967295 x 16777215 */
	VARINT_DIMENSION_PAIR_SPRSE_4_3 = VARINT_DIMENSION_PAIR_PAIR(4, 3, 1), /* up to 4294967295 x 16777215 */
	VARINT_DIMENSION_PAIR_DENSE_4_4 = VARINT_DIMENSION_PAIR_PAIR(4, 4, 0), /* up to 4294967295 x 4294967295 */
	VARINT_DIMENSION_PAIR_SPRSE_4_4 = VARINT_DIMENSION_PAIR_PAIR(4, 4, 1), /* up to 4294967295 x 4294967295 */
	VARINT_DIMENSION_PAIR_DENSE_4_5 = VARINT_DIMENSION_PAIR_PAIR(4, 5, 0), /* up to 4294967295 x 1099511627775 */
	VARINT_DIMENSION_PAIR_SPRSE_4_5 = VARINT_DIMENSION_PAIR_PAIR(4, 5, 1), /* up to 4294967295 x 1099511627775 */
	VARINT_DIMENSION_PAIR_DENSE_4_6 = VARINT_DIMENSION_PAIR_PAIR(4, 6, 0), /* up to 4294967295 x 281474976710655 */
	VARINT_DIMENSION_PAIR_SPRSE_4_6 = VARINT_DIMENSION_PAIR_PAIR(4, 6, 1), /* up to 4294967295 x 281474976710655 */
	VARINT_DIMENSION_PAIR_DENSE_4_7 = VARINT_DIMENSION_PAIR_PAIR(4, 7, 0), /* up to 4294967295 x 72057594037927935 */
	VARINT_DIMENSION_PAIR_SPRSE_4_7 = VARINT_DIMENSION_PAIR_PAIR(4, 7, 1), /* up to 4294967295 x 72057594037927935 */
	VARINT_DIMENSION_PAIR_DENSE_4_8 = VARINT_DIMENSION_PAIR_PAIR(4, 8, 0), /* up to 4294967295 x 18446744073709551615 */
	VARINT_DIMENSION_PAIR_SPRSE_4_8 = VARINT_DIMENSION_PAIR_PAIR(4, 8, 1), /* up to 4294967295 x 18446744073709551615 */
	VARINT_DIMENSION_PAIR_DENSE_5_1 = VARINT_DIMENSION_PAIR_PAIR(5, 1, 0), /* up to 1099511627775 x 255 */
	VARINT_DIMENSION_PAIR_SPRSE_5_1 = VARINT_DIMENSION_PAIR_PAIR(5, 1, 1), /* up to 1099511627775 x 255 */
	VARINT_DIMENSION_PAIR_DENSE_5_2 = VARINT_DIMENSION_PAIR_PAIR(5, 2, 0), /* up to 1099511627775 x 65535 */
	VARINT_DIMENSION_PAIR_SPRSE_5_2 = VARINT_DIMENSION_PAIR_PAIR(5, 2, 1), /* up to 1099511627775 x 65535 */
	VARINT_DIMENSION_PAIR_DENSE_5_3 = VARINT_DIMENSION_PAIR_PAIR(5, 3, 0), /* up to 1099511627775 x 16777215 */
	VARINT_DIMENSION_PAIR_SPRSE_5_3 = VARINT_DIMENSION_PAIR_PAIR(5, 3, 1), /* up to 1099511627775 x 16777215 */
	VARINT_DIMENSION_PAIR_DENSE_5_4 = VARINT_DIMENSION_PAIR_PAIR(5, 4, 0), /* up to 1099511627775 x 4294967295 */
	VARINT_DIMENSION_PAIR_SPRSE_5_4 = VARINT_DIMENSION_PAIR_PAIR(5, 4, 1), /* up to 1099511627775 x 4294967295 */
	VARINT_DIMENSION_PAIR_DENSE_5_5 = VARINT_DIMENSION_PAIR_PAIR(5, 5, 0), /* up to 1099511627775 x 1099511627775 */
	VARINT_DIMENSION_PAIR_SPRSE_5_5 = VARINT_DIMENSION_PAIR_PAIR(5, 5, 1), /* up to 1099511627775 x 1099511627775 */
	VARINT_DIMENSION_PAIR_DENSE_5_6 = VARINT_DIMENSION_PAIR_PAIR(5, 6, 0), /* up to 1099511627775 x 281474976710655 */
	VARINT_DIMENSION_PAIR_SPRSE_5_6 = VARINT_DIMENSION_PAIR_PAIR(5, 6, 1), /* up to 1099511627775 x 281474976710655 */
	VARINT_DIMENSION_PAIR_DENSE_5_7 = VARINT_DIMENSION_PAIR_PAIR(5, 7, 0), /* up to 1099511627775 x 72057594037927935 */
	VARINT_DIMENSION_PAIR_SPRSE_5_7 = VARINT_DIMENSION_PAIR_PAIR(5, 7, 1), /* up to 1099511627775 x 72057594037927935 */
	VARINT_DIMENSION_PAIR_DENSE_5_8 = VARINT_DIMENSION_PAIR_PAIR(5, 8, 0), /* up to 1099511627775 x 18446744073709551615 */
	VARINT_DIMENSION_PAIR_SPRSE_5_8 = VARINT_DIMENSION_PAIR_PAIR(5, 8, 1), /* up to 1099511627775 x 18446744073709551615 */
	VARINT_DIMENSION_PAIR_DENSE_6_1 = VARINT_DIMENSION_PAIR_PAIR(6, 1, 0), /* up to 281474976710655 x 255 */
	VARINT_DIMENSION_PAIR_SPRSE_6_1 = VARINT_DIMENSION_PAIR_PAIR(6, 1, 1), /* up to 281474976710655 x 255 */
	VARINT_DIMENSION_PAIR_DENSE_6_2 = VARINT_DIMENSION_PAIR_PAIR(6, 2, 0), /* up to 281474976710655 x 65535 */
	VARINT_DIMENSION_PAIR_SPRSE_6_2 = VARINT_DIMENSION_PAIR_PAIR(6, 2, 1), /* up to 281474976710655 x 65535 */
	VARINT_DIMENSION_PAIR_DENSE_6_3 = VARINT_DIMENSION_PAIR_PAIR(6, 3, 0), /* up to 281474976710655 x 16777215 */
	VARINT_DIMENSION_PAIR_SPRSE_6_3 = VARINT_DIMENSION_PAIR_PAIR(6, 3, 1), /* up to 281474976710655 x 16777215 */
	VARINT_DIMENSION_PAIR_DENSE_6_4 = VARINT_DIMENSION_PAIR_PAIR(6, 4, 0), /* up to 281474976710655 x 4294967295 */
	VARINT_DIMENSION_PAIR_SPRSE_6_4 = VARINT_DIMENSION_PAIR_PAIR(6, 4, 1), /* up to 281474976710655 x 4294967295 */
	VARINT_DIMENSION_PAIR_DENSE_6_5 = VARINT_DIMENSION_PAIR_PAIR(6, 5, 0), /* up to 281474976710655 x 1099511627775 */
	VARINT_DIMENSION_PAIR_SPRSE_6_5 = VARINT_DIMENSION_PAIR_PAIR(6, 5, 1), /* up to 281474976710655 x 1099511627775 */
	VARINT_DIMENSION_PAIR_DENSE_6_6 = VARINT_DIMENSION_PAIR_PAIR(6, 6, 0), /* up to 281474976710655 x 281474976710655 */
	VARINT_DIMENSION_PAIR_SPRSE_6_6 = VARINT_DIMENSION_PAIR_PAIR(6, 6, 1), /* up to 281474976710655 x 281474976710655 */
	VARINT_DIMENSION_PAIR_DENSE_6_7 = VARINT_DIMENSION_PAIR_PAIR(6, 7, 0), /* up to 281474976710655 x 72057594037927935 */
	VARINT_DIMENSION_PAIR_SPRSE_6_7 = VARINT_DIMENSION_PAIR_PAIR(6, 7, 1), /* up to 281474976710655 x 72057594037927935 */
	VARINT_DIMENSION_PAIR_DENSE_6_8 = VARINT_DIMENSION_PAIR_PAIR(6, 8, 0), /* up to 281474976710655 x 18446744073709551615 */
	VARINT_DIMENSION_PAIR_SPRSE_6_8 = VARINT_DIMENSION_PAIR_PAIR(6, 8, 1), /* up to 281474976710655 x 18446744073709551615 */
	VARINT_DIMENSION_PAIR_DENSE_7_1 = VARINT_DIMENSION_PAIR_PAIR(7, 1, 0), /* up to 72057594037927935 x 255 */
	VARINT_DIMENSION_PAIR_SPRSE_7_1 = VARINT_DIMENSION_PAIR_PAIR(7, 1, 1), /* up to 72057594037927935 x 255 */
	VARINT_DIMENSION_PAIR_DENSE_7_2 = VARINT_DIMENSION_PAIR_PAIR(7, 2, 0), /* up to 72057594037927935 x 65535 */
	VARINT_DIMENSION_PAIR_SPRSE_7_2 = VARINT_DIMENSION_PAIR_PAIR(7, 2, 1), /* up to 72057594037927935 x 65535 */
	VARINT_DIMENSION_PAIR_DENSE_7_3 = VARINT_DIMENSION_PAIR_PAIR(7, 3, 0), /* up to 72057594037927935 x 16777215 */
	VARINT_DIMENSION_PAIR_SPRSE_7_3 = VARINT_DIMENSION_PAIR_PAIR(7, 3, 1), /* up to 72057594037927935 x 16777215 */
	VARINT_DIMENSION_PAIR_DENSE_7_4 = VARINT_DIMENSION_PAIR_PAIR(7, 4, 0), /* up to 72057594037927935 x 4294967295 */
	VARINT_DIMENSION_PAIR_SPRSE_7_4 = VARINT_DIMENSION_PAIR_PAIR(7, 4, 1), /* up to 72057594037927935 x 4294967295 */
	VARINT_DIMENSION_PAIR_DENSE_7_5 = VARINT_DIMENSION_PAIR_PAIR(7, 5, 0), /* up to 72057594037927935 x 1099511627775 */
	VARINT_DIMENSION_PAIR_SPRSE_7_5 = VARINT_DIMENSION_PAIR_PAIR(7, 5, 1), /* up to 72057594037927935 x 1099511627775 */
	VARINT_DIMENSION_PAIR_DENSE_7_6 = VARINT_DIMENSION_PAIR_PAIR(7, 6, 0), /* up to 72057594037927935 x 281474976710655 */
	VARINT_DIMENSION_PAIR_SPRSE_7_6 = VARINT_DIMENSION_PAIR_PAIR(7, 6, 1), /* up to 72057594037927935 x 281474976710655 */
	VARINT_DIMENSION_PAIR_DENSE_7_7 = VARINT_DIMENSION_PAIR_PAIR(7, 7, 0), /* up to 72057594037927935 x 72057594037927935 */
	VARINT_DIMENSION_PAIR_SPRSE_7_7 = VARINT_DIMENSION_PAIR_PAIR(7, 7, 1), /* up to 72057594037927935 x 72057594037927935 */
	VARINT_DIMENSION_PAIR_DENSE_7_8 = VARINT_DIMENSION_PAIR_PAIR(7, 8, 0), /* up to 72057594037927935 x 18446744073709551615 */
	VARINT_DIMENSION_PAIR_SPRSE_7_8 = VARINT_DIMENSION_PAIR_PAIR(7, 8, 1), /* up to 72057594037927935 x 18446744073709551615 */
	VARINT_DIMENSION_PAIR_DENSE_8_1 = VARINT_DIMENSION_PAIR_PAIR(8, 1, 0), /* up to 18446744073709551615 x 255 */
	VARINT_DIMENSION_PAIR_SPRSE_8_1 = VARINT_DIMENSION_PAIR_PAIR(8, 1, 1), /* up to 18446744073709551615 x 255 */
	VARINT_DIMENSION_PAIR_DENSE_8_2 = VARINT_DIMENSION_PAIR_PAIR(8, 2, 0), /* up to 18446744073709551615 x 65535 */
	VARINT_DIMENSION_PAIR_SPRSE_8_2 = VARINT_DIMENSION_PAIR_PAIR(8, 2, 1), /* up to 18446744073709551615 x 65535 */
	VARINT_DIMENSION_PAIR_DENSE_8_3 = VARINT_DIMENSION_PAIR_PAIR(8, 3, 0), /* up to 18446744073709551615 x 16777215 */
	VARINT_DIMENSION_PAIR_SPRSE_8_3 = VARINT_DIMENSION_PAIR_PAIR(8, 3, 1), /* up to 18446744073709551615 x 16777215 */
	VARINT_DIMENSION_PAIR_DENSE_8_4 = VARINT_DIMENSION_PAIR_PAIR(8, 4, 0), /* up to 18446744073709551615 x 4294967295 */
	VARINT_DIMENSION_PAIR_SPRSE_8_4 = VARINT_DIMENSION_PAIR_PAIR(8, 4, 1), /* up to 18446744073709551615 x 4294967295 */
	VARINT_DIMENSION_PAIR_DENSE_8_5 = VARINT_DIMENSION_PAIR_PAIR(8, 5, 0), /* up to 18446744073709551615 x 1099511627775 */
	VARINT_DIMENSION_PAIR_SPRSE_8_5 = VARINT_DIMENSION_PAIR_PAIR(8, 5, 1), /* up to 18446744073709551615 x 1099511627775 */
	VARINT_DIMENSION_PAIR_DENSE_8_6 = VARINT_DIMENSION_PAIR_PAIR(8, 6, 0), /* up to 18446744073709551615 x 281474976710655 */
	VARINT_DIMENSION_PAIR_SPRSE_8_6 = VARINT_DIMENSION_PAIR_PAIR(8, 6, 1), /* up to 18446744073709551615 x 281474976710655 */
	VARINT_DIMENSION_PAIR_DENSE_8_7 = VARINT_DIMENSION_PAIR_PAIR(8, 7, 0), /* up to 18446744073709551615 x 72057594037927935 */
	VARINT_DIMENSION_PAIR_SPRSE_8_7 = VARINT_DIMENSION_PAIR_PAIR(8, 7, 1), /* up to 18446744073709551615 x 72057594037927935 */
	VARINT_DIMENSION_PAIR_DENSE_8_8 = VARINT_DIMENSION_PAIR_PAIR(8, 8, 0), /* up to 18446744073709551615 x 18446744073709551615 */
	VARINT_DIMENSION_PAIR_SPRSE_8_8 = VARINT_DIMENSION_PAIR_PAIR(8, 8, 1), /* up to 18446744073709551615 x 18446744073709551615 */
} varintDimensionPair;
/* clang-format on */

bool varintDimensionPack(const size_t row, const size_t col, uint64_t *result,
                         varintDimensionPacked *dimension);
void varintDimensionUnpack(size_t *rows, size_t *cols, const uint64_t packed,
                           const varintDimensionPacked dimension);

#define VARINT_DIMENSION_PACKED_TO_BITS(dim) ((dim)*4)

#define varintDimensionUnpack_(x, y, packed, dimension)                        \
    do {                                                                       \
        (x) = ((packed) >> VARINT_DIMENSION_PACKED_TO_BITS(dimension));        \
        (y) = ((packed) & ~(0xFFFFFFFFFFFFFFFFULL                              \
                            << VARINT_DIMENSION_PACKED_TO_BITS(dimension)));   \
    } while (0)

varintDimensionPair varintDimensionPairEncode(void *dst, size_t row,
                                              size_t col);
varintDimensionPair varintDimensionPairDimension(size_t row, size_t col);

#define VARINT_DIMENSION_PAIR_BYTE_LENGTH(dim)                                 \
    (VARINT_DIMENSION_PAIR_WIDTH_ROW_COUNT(dim) +                              \
     VARINT_DIMENSION_PAIR_WIDTH_COL_COUNT(dim))

#define VARINT_DIMENSION_PAIR_BYTE_LENGTH_TOTAL_MATRIX(dim, entrySize)         \
    (VARINT_DIMENSION_PAIR_BYTE_LENGTH(dim) +                                  \
     (VARINT_DIMENSION_PAIR_BYTE_LENGTH(dim) * (entrySize)))

/* For boolean matrices / vectors, we must round up to the next whole number of
 * bytes.  (e.g. 10x10 = 100 bits, but 100 bits / 8 bits/byte = 12.5 bytes, so
 * we need to round up to 13 bytes of storage. */
#define VARINT_DIMENSION_PAIR_BYTE_LENGTH_TOTAL_BOOLEAN_MATRIX(dim)            \
    ((VARINT_DIMENSION_PAIR_BYTE_LENGTH(dim) +                                 \
      (VARINT_DIMENSION_PAIR_BYTE_LENGTH(dim)) + 8 - 1) /                      \
     8)

uint64_t varintDimensionPairEntryGetUnsigned(
    const void *_src, const size_t row, const size_t col,
    const varintWidth entryWidthBytes, const varintDimensionPair dimension);

void varintDimensionPairEntrySetUnsigned(void *_dst, const size_t row,
                                         const size_t col,
                                         const uint64_t entryValue,
                                         const varintWidth entryWidthBytes,
                                         const varintDimensionPair dimension);
void varintDimensionPairEntrySetFloat(void *_dst, const size_t row,
                                      const size_t col, const float entryValue,
                                      const varintDimensionPair dimension);
void varintDimensionPairEntrySetDouble(void *_dst, const size_t row,
                                       const size_t col,
                                       const double entryValue,
                                       const varintDimensionPair dimension);

bool varintDimensionPairEntryGetBit(const void *_src, const size_t row,
                                    const size_t col,
                                    const varintDimensionPair dimension);
void varintDimensionPairEntrySetBit(const void *_dst, const size_t row,
                                    const size_t col, const bool setBit,
                                    const varintDimensionPair dimension);
/* Returns previous value of bit */
bool varintDimensionPairEntryToggleBit(const void *_dst, const size_t row,
                                       const size_t col,
                                       const varintDimensionPair dimension);

/* Dimensional Sparse; Roaring
 * Chunks of 64k bits
 *   - If less than 4k entries in the 64k range, store set positions as:
 *     - packed 12 bit integers
 *   - If more than (64k - 4k) entries, store NOT SET position as:
 *     - packed 12 bit integers
 *   - else, if > 4k && < (64k - 4k):
 *     - store direct for this chunk.
 * Chunk layouts:
 *   [CHUNK FULL] (all elements set)
 *   [CHUNK EMPTY] (all elements unset)
 *   [SPARSE VALUES][COUNT][ENTRIES]
 *   [SPARSE ABSENT][COUNT][ENTRIES]
 *   [DIRECT][All 64k values]
 *
 * Smallest Sparse chunk if no values are set:
 *   [CHUNK EMPTY] = 1 byte
 *
 * Smallest Sparse chunk if one value is set:
 *   [TYPE BYTE][COUNT BYTE][VALUE_1] = 1 + 1 + 2 = 4 bytes
 *
 * Largest Sparse chunk if all but one values are set:
 *   [TYPE BYTE][COUNT BYTES][VALUES] = 1 + 3 + ((4096 - 1) * 12)/8 = 6147 bytes
 *
 * Largest Sparse chunk if all values are set:
 *   [CHUNK FULL] = 1 byte
 *
 * Size of full bitmap:
 *   [TYPE BYTE][64k bits] = 1 + (65536 / 8) = 8193 bytes
 */

/* Extended to general matrix types...
 * Chunks of 64k matrix entries of arbitrary byte width:
 *   - If less than 4k entries in the 64k range, store set positions as:
 *     - packed 12 bit integers, followed by:
 *       - array of varints of width of the matrix entries.
 *   - If more than (64k - 4k) entries, store NOT SET position as:
 *     - packed 12 bit integers
 *   - else, if > 4k && < (64k - 4k):
 *     - store direct for this chunk.
 * Chunk layouts:
 *   [CHUNK FULL][ENTRIES] (no count)
 *   [CHUNK EMPTY] (no element values)
 *   [SPARSE VALUES][COUNT][ENTRY POSITIONS][ENTRIES]
 *   [SPARSE ABSENT][COUNT][ENTRY POSITIONS]
 *   [DIRECT][All 64k values]
 *
 * Smallest Sparse chunk if no values are set:
 *   [CHUNK EMPTY] = 1 byte
 *
 * Smallest Sparse chunk if one value is set:
 *   [TYPE BYTE][COUNT BYTE][VALUE 1 POSITION][VALUE 1] = 1 + 1 + 2 +
 * sizeof(value1)= 4 + sizeof(value1) bytes
 *
 * Largest Sparse chunk if all but one values are set:
 *   [TYPE BYTE][COUNT BYTES][VALUE POSITIONS][VALUES] =
 *     1 + 3 + ((4096 - 1) * 12)/8 +
 *       sizeof(value)*(4096 - 1) = 6147 + sizeof(value)*(4096 - 1)
 *     (example: 24 bit varints:
 *       1 + 3 + 6144 + 4095 * 3 = 18,433 bytes)
 *     (storing 64k whole 3 byte entires which would be: 196,608 bytes)
 *
 * Largest Sparse chunk if all values are set:
 *   [CHUNK FULL][VALUE POSITIONS][VALUES] =
 *     1 + (4096 * 12) / 8 + sizeof(value) * (4096) = 6145 + sizeof(value)*4096
 *
 * Size of full data:
 *   [TYPE BYTE][64k entries of data width] = 1 + (65536 * width) = 1 + 64k *
 * width
 *   (example: 24 bit varints are (64k * 3 bytes) = 196k;
 *             32 bit: 64k * 4 bytes = 262k
 *             64 bit: 64k * 8 bytes = 524k)
 */

#ifdef VARINT_DIMENSION_TEST
int varintDimensionTest(int argc, char *argv[]);
#endif

__END_DECLS
