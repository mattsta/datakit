#!/usr/bin/env python3

"""Utility script for generating the bit enum in src/varintDimension.h"""

print(
    "#define VARINT_DIMENSION_PAIR_PAIR(x, y, sparse) (((x) << 4) | ((y - 1) << 1) | (sparse))"
)
print(
    """#define VARINT_DIMENSION_PAIR_WIDTH_ROW_COUNT(dim) ((dim) >> 4)
#define VARINT_DIMENSION_PAIR_WIDTH_COL_COUNT(dim) ((((dim) >> 1) & 0x03) + 1)
#define VARINT_DIMENSION_PAIR_IS_SPARSE(dim) ((dim) & 0x01)
"""
)
print()

print("typedef enum varintDimensionPair {")
for x in range(0, 9):
    for y in range(1, 9):
        print(
            f"VARINT_DIMENSION_PAIR_DENSE_{x}_{y} = VARINT_DIMENSION_PAIR_PAIR({x}, {y}, 0), /* up to {(1 << (x * 8)) - 1} x {(1 << (y * 8)) - 1} */"
        )
        print(
            f"VARINT_DIMENSION_PAIR_SPRSE_{x}_{y} = VARINT_DIMENSION_PAIR_PAIR({x}, {y}, 1), /* up to {(1 << (x * 8)) - 1} x {(1 << (y * 8)) - 1} */"
        )
print("} varintDimensionPair;")
