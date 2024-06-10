#include "varintDimension.h"
#include "varintExternal.h"

/* ====================================================================
 * Dimension Packing (row, col) = [XY] as single integer
 * ==================================================================== */
bool varintDimensionPackedEncode(const size_t row, const size_t col,
                                 uint64_t *result,
                                 varintDimensionPacked *dimension) {
    varintDimensionPacked foundDimension = VARINT_DIMENSION_PACKED_1;

    if (row >= 15 && col >= 15) {
        size_t rrow = row;
        size_t ccol = col;
        while ((rrow >>= 8) != 0 || (ccol >>= 8) != 0) {
            foundDimension++;
        }
    }

    if (foundDimension > VARINT_DIMENSION_PACKED_8) {
        return false;
    }

    *result = row;
    *result <<= VARINT_DIMENSION_PACKED_TO_BITS(foundDimension);
    *result |= col;

    *dimension = foundDimension;

    return true;
}

void varintDimensionPackedDecode(size_t *rows, size_t *cols,
                                 const uint64_t packed,
                                 const varintDimensionPacked dimension) {
    varintDimensionUnpack_(*rows, *cols, packed, dimension);
}

/* ====================================================================
 * Dimension Pairing (row, col) = [X][Y] as individual external varints
 * ==================================================================== */
varintDimensionPair varintDimensionPairDimension(size_t rows, size_t cols) {
    varintWidth widthRows = 0;
    /* It's okay if we make a "zero row" matrix; we just treat
     * it as a vector of length 'cols' */
    if (rows) {
        varintExternalUnsignedEncoding(rows, widthRows);
    }

    varintWidth widthCols = 0;
    if (cols) {
        varintExternalUnsignedEncoding(cols, widthCols);
    } else {
        /* It's actually an error to create a matrix with
         * no columns, but we don't have an "error out" here. */
    }

    return VARINT_DIMENSION_PAIR_PAIR(widthRows, widthCols, false);
}

varintDimensionPair varintDimensionPairEncode(void *_dst, size_t row,
                                              size_t col) {
    uint8_t *dst = (uint8_t *)_dst;

    varintWidth widthRows;
    varintWidth widthCols;
    varintDimensionPair dimension = varintDimensionPairDimension(row, col);
    VARINT_DIMENSION_PAIR_DEPAIR(widthRows, widthCols, dimension);

    varintExternalPutFixedWidth(dst, row, widthRows);
    varintExternalPutFixedWidth(dst + widthRows, col, widthCols);

    return dimension;
}

static inline void
varintDimensionPairDecode(const void *_pair, size_t *x, size_t *y,
                          const varintDimensionPair dimension) {
    const uint8_t *pair = (const uint8_t *)_pair;

    varintWidth widthRows;
    varintWidth widthCols;
    VARINT_DIMENSION_PAIR_DEPAIR(widthRows, widthCols, dimension);

    if (widthRows) {
        *x = varintExternalGet(pair, widthRows);
    } else {
        *x = 0;
    }

    *y = varintExternalGet(pair + widthRows, widthCols);
}

static inline size_t getEntryByteOffset(const void *_src, const size_t row,
                                        const size_t col,
                                        const varintWidth entryWidthBytes,
                                        const varintDimensionPair dimension) {
    const uint8_t *src = (const uint8_t *)_src;

    const uint8_t dataStartOffset =
        VARINT_DIMENSION_PAIR_BYTE_LENGTH(dimension);
    size_t entryOffset;
    if (row) {
        size_t rows;
        size_t cols;
        varintDimensionPairDecode(src, &rows, &cols, dimension);
        entryOffset = ((row * cols) + col) * entryWidthBytes;
    } else {
        entryOffset = col * entryWidthBytes;
    }

    return dataStartOffset + entryOffset;
}

/* ====================================================================
 * Access Dimension Pairing Matrix Array Entries
 * ==================================================================== */
uint64_t varintDimensionPairEntryGetUnsigned(
    const void *_src, const size_t row, const size_t col,
    const varintWidth entryWidthBytes, const varintDimensionPair dimension) {
    const uint8_t *src = (const uint8_t *)_src;
    const size_t entryOffset =
        getEntryByteOffset(src, row, col, entryWidthBytes, dimension);
    return varintExternalGet(src + entryOffset, entryWidthBytes);
}

#define _bitOffsets(arr, row, col, dim, offsetByte, offsetBit)                 \
    do {                                                                       \
        const varintWidth widthRows =                                          \
            VARINT_DIMENSION_PAIR_WIDTH_ROW_COUNT(dim);                        \
        const varintWidth widthCols =                                          \
            VARINT_DIMENSION_PAIR_WIDTH_COL_COUNT(dim);                        \
        const uint8_t metadataSize = widthRows + widthCols;                    \
        size_t _bit_offsetTotal;                                               \
        if (row) {                                                             \
            const size_t cols =                                                \
                varintExternalGet((arr) + widthRows, widthCols);               \
            _bit_offsetTotal = (((row)*cols) + (col));                         \
        } else {                                                               \
            _bit_offsetTotal = (col);                                          \
        }                                                                      \
        (offsetByte) = metadataSize + (_bit_offsetTotal / sizeof(uint8_t));    \
        (offsetBit) = _bit_offsetTotal % sizeof(uint8_t);                      \
    } while (0)

bool varintDimensionPairEntryGetBit(const void *_src, const size_t row,
                                    const size_t col,
                                    const varintDimensionPair dimension) {
    const uint8_t *src = (const uint8_t *)(_src);
    size_t offsetByte;
    uint8_t offsetBit;
    _bitOffsets(src, row, col, dimension, offsetByte, offsetBit);

    return ((src[offsetByte] >> offsetBit) & 0x01);
}

void varintDimensionPairEntrySetUnsigned(void *_dst, const size_t row,
                                         const size_t col,
                                         const uint64_t entryValue,
                                         const varintWidth entryWidthBytes,
                                         const varintDimensionPair dimension) {
    uint8_t *dst = (uint8_t *)_dst;

    const size_t entryOffset =
        getEntryByteOffset(dst, row, col, entryWidthBytes, dimension);

    varintExternalPutFixedWidth(dst + entryOffset, entryValue, entryWidthBytes);
}

void varintDimensionPairEntrySetFloat(void *_dst, const size_t row,
                                      const size_t col, const float entryValue,
                                      const varintDimensionPair dimension) {
    uint8_t *dst = (uint8_t *)_dst;

    const size_t entryOffset =
        getEntryByteOffset(dst, row, col, sizeof(float), dimension);

    *(float *)(dst + entryOffset) = entryValue;
}

#if __arm64__ || __aarch64__
#warning "No ARM implementation of varintDimension half float intrinsics yet!"
#elif __x86_64__
#include <x86intrin.h>
void varintDimensionPairEntrySetFloatHalfIntrinsic(
    void *_dst, const size_t row, const size_t col, const float entryValue,
    const varintDimensionPair dimension) {
    uint8_t *dst = (uint8_t *)_dst;

    const size_t entryOffset =
        getEntryByteOffset(dst, row, col, sizeof(float), dimension);

    float holder[128 / sizeof(uint16_t)] = {0};
    uint16_t half_holder[128 / sizeof(uint16_t)] = {0};
    holder[0] = entryValue;

    __m128 float_vector = _mm_load_ps(holder);
    __m128i half_vector = _mm_cvtps_ph(float_vector, 0);
    _mm_store_si128((__m128i *)half_holder, half_vector);
    *(uint16_t *)(dst + entryOffset) = *(uint16_t *)half_holder;
}

float varintDimensionPairEntryGetFloatHalfIntrinsic(
    void *_dst, const size_t row, const size_t col,
    const varintDimensionPair dimension) {
    uint8_t *dst = (uint8_t *)_dst;

    const size_t entryOffset =
        getEntryByteOffset(dst, row, col, sizeof(float), dimension);

    float holder[128 / sizeof(uint16_t)] = {0};
    uint16_t half_holder[128 / sizeof(uint16_t)] = {0};
    half_holder[0] = *(uint16_t *)(dst + entryOffset);

    __m128 vector = _mm_cvtph_ps(_mm_load_si128((__m128i *)(holder)));
    return vector[0];
}
#endif

void varintDimensionPairEntrySetDouble(void *_dst, const size_t row,
                                       const size_t col,
                                       const double entryValue,
                                       const varintDimensionPair dimension) {
    uint8_t *dst = (uint8_t *)_dst;

    const size_t entryOffset =
        getEntryByteOffset(dst, row, col, sizeof(double), dimension);

    *(double *)(dst + entryOffset) = entryValue;
}

void varintDimensionPairEntrySetBit(const void *_dst, const size_t row,
                                    const size_t col, const bool setBit,
                                    const varintDimensionPair dimension) {
    uint8_t *dst = (uint8_t *)_dst;
    size_t offsetByte;
    uint8_t offsetBit;
    _bitOffsets(dst, row, col, dimension, offsetByte, offsetBit);

    dst[offsetByte] |= setBit << offsetBit;
}

bool varintDimensionPairEntryToggleBit(const void *_dst, const size_t row,
                                       const size_t col,
                                       const varintDimensionPair dimension) {
    uint8_t *dst = (uint8_t *)_dst;
    size_t offsetByte;
    uint8_t offsetBit;
    _bitOffsets(dst, row, col, dimension, offsetByte, offsetBit);

    bool oldValue = ((dst[offsetByte] >> offsetBit) & 0x01);
    dst[offsetByte] ^= 1 << offsetBit;
    return oldValue;
}

#define PACK_STORAGE_BITS 12
#define PACK_MAX_ELEMENTS 3700
#define PACK_STORAGE_SLOT_STORAGE_TYPE uint8_t
#define PACK_STORAGE_MICRO_PROMOTION_TYPE uint16_t
#include "varintPacked.h"

#ifdef VARINT_DIMENSION_TEST
#include "ctest.h"
int varintDimensionTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int32_t err = 0;

    TEST("Pack 12 bit integers into bytes") {
        uint8_t holder[8192] = {0};

        varintPacked12InsertSorted(holder, 0, 3);

        /* Note: these test extractions are little endian.
         * Our bit packing is *not* endian independent. */
        if (holder[0] != 3) {
            ERR("Didn't set first value! holder[0] is %d", holder[0]);
        }

        varintPacked12InsertSorted(holder, 1, 255);

        uint16_t secondA = holder[2];
        uint16_t secondB = holder[1];
        uint16_t second = (secondA << 4) | (secondB & 0xf0) >> 4;
        if (second != 255) {
            ERR("Didn't set second value! Instead of 255, got: %d (from: %d, "
                "%d)",
                second, secondA, secondB);
        }

        varintPacked12InsertSorted(holder, 2, 4);

        uint8_t thirdA = holder[2];
        uint8_t thirdB = holder[1];
        uint16_t third = (thirdA << 4) | (thirdB & 0xf0) >> 4;
        if (third != 4) {
            ERR("Didn't set third value! Instead of 4, got: %d (from: %d, %d)",
                third, thirdA, thirdB);
        }

        varintPacked12InsertSorted(holder, 3, 1);
        varintPacked12InsertSorted(holder, 4, 20);
        for (int i = 0; i < 3600; i++) {
            uint16_t num = rand() % 0xfff;
            varintPacked12InsertSorted(holder, 5 + i, num);
            if (varintPacked12Member(holder, 5 + i, num) == -1) {
                ERR("Failed to find %d after insert!\n", num);
            }
        }

        uint16_t prev = 0;
        for (int i = 0; i < (3600 + 5); i++) {
            uint16_t current = varintPacked12Get(holder, i);
            if (prev > current) {
                ERR("Sorted list isn't sorted at index %d!\n", i);
            }

            prev = current;
        }
    }

    const varintDimensionPair d = varintDimensionPairDimension(12, 12);
    const varintDimensionPair f = varintDimensionPairDimension(300, 67001);
    TEST("Set and get dimension pair storage width sizes") {
        if (VARINT_DIMENSION_PAIR_WIDTH_ROW_COUNT(d) != 1) {
            ERR("Didn't retrieve row dimension width 1, got: %d",
                VARINT_DIMENSION_PAIR_WIDTH_ROW_COUNT(d));
        }

        if (VARINT_DIMENSION_PAIR_WIDTH_COL_COUNT(d) != 1) {
            ERR("Didn't retrieve col dimension width 1, got: %d",
                VARINT_DIMENSION_PAIR_WIDTH_COL_COUNT(d));
        }

        if (VARINT_DIMENSION_PAIR_WIDTH_ROW_COUNT(f) != 2) {
            ERR("Didn't retrieve row dimension width 2, got: %d",
                VARINT_DIMENSION_PAIR_WIDTH_ROW_COUNT(f));
        }

        if (VARINT_DIMENSION_PAIR_WIDTH_COL_COUNT(f) != 3) {
            ERR("Didn't retrieve col dimension width 3, got: %d",
                VARINT_DIMENSION_PAIR_WIDTH_COL_COUNT(f));
        }
    }

    TEST("Get total pair width") {
        if (VARINT_DIMENSION_PAIR_BYTE_LENGTH(d) != 2) {
            ERR("Didn't get total width of 2, got: %d\n",
                VARINT_DIMENSION_PAIR_BYTE_LENGTH(d));
        }

        if (VARINT_DIMENSION_PAIR_BYTE_LENGTH(f) != 5) {
            ERR("Didn't get total width of 5, got: %d\n",
                VARINT_DIMENSION_PAIR_BYTE_LENGTH(f));
        }
    }

    TEST("64x64 (4k) boolean matrix: set and get every entry position") {
        varintDimensionPair widthSizes = varintDimensionPairDimension(64, 64);
        if (VARINT_DIMENSION_PAIR_BYTE_LENGTH(widthSizes) != 2) {
            ERRR("Didn't get correct length storage byte width!");
        }

        uint8_t totalMatrix[2 + (64 * 64)] = {0};
        varintDimensionPair mat =
            varintDimensionPairEncode(totalMatrix, 64, 64);

        for (int row = 0; row < 64; row++) {
            for (int col = 0; col < 64; col++) {
                varintDimensionPairEntrySetBit(totalMatrix, row, col, 1, mat);
                bool result =
                    varintDimensionPairEntryGetBit(totalMatrix, row, col, mat);
                if (result != 1) {
                    ERRR("Didn't get set bit!");
                }

                bool sameAsResult = varintDimensionPairEntryToggleBit(
                    totalMatrix, row, col, mat);
                if (sameAsResult != 1) {
                    ERRR("Toggle didn't return previous bit value!");
                }

                bool toggledResult =
                    varintDimensionPairEntryGetBit(totalMatrix, row, col, mat);
                if (toggledResult != 0) {
                    ERRR("Toggle didn't toggle!");
                }
            }
        }
    }

    /* TODO: loop these sizes / creations */
    TEST("6400x6400 (40 million) boolean matrix: set and get every entry "
         "position") {
        varintDimensionPair widthSizes =
            varintDimensionPairDimension(6400, 6400);
        if (VARINT_DIMENSION_PAIR_BYTE_LENGTH(widthSizes) != 4) {
            ERRR("Didn't get correct length storage byte width!");
        }

        uint8_t *totalMatrix = calloc(1, 4 + (6400 * 6400));
        varintDimensionPair mat =
            varintDimensionPairEncode(totalMatrix, 6400, 6400);

        for (int row = 0; row < 6400; row++) {
            for (int col = 0; col < 6400; col++) {
                varintDimensionPairEntrySetBit(totalMatrix, row, col, 1, mat);
                bool result =
                    varintDimensionPairEntryGetBit(totalMatrix, row, col, mat);
                if (result != 1) {
                    ERRR("Didn't get set bit!");
                }

                bool sameAsResult = varintDimensionPairEntryToggleBit(
                    totalMatrix, row, col, mat);
                if (sameAsResult != 1) {
                    ERRR("Toggle didn't return previous bit value!");
                }

                bool toggledResult =
                    varintDimensionPairEntryGetBit(totalMatrix, row, col, mat);
                if (toggledResult != 0) {
                    ERRR("Toggle didn't toggle!");
                }
            }
        }

        free(totalMatrix);
    }

    TEST("64x64 uint8_t matrix: set and get every entry position") {
        varintDimensionPair widthSizes = varintDimensionPairDimension(64, 64);
        if (VARINT_DIMENSION_PAIR_BYTE_LENGTH(widthSizes) != 2) {
            ERRR("Didn't get correct length storage byte width!");
        }

        uint8_t *totalMatrix = calloc(1, 2 + ((64 * 64) * sizeof(uint8_t)));
        varintDimensionPair mat =
            varintDimensionPairEncode(totalMatrix, 64, 64);

        for (int row = 0; row < 64; row++) {
            for (int col = 0; col < 64; col++) {
                uint8_t val = (row + col);
                varintDimensionPairEntrySetUnsigned(totalMatrix, row, col, val,
                                                    VARINT_WIDTH_8B, mat);
                uint8_t result = varintDimensionPairEntryGetUnsigned(
                    totalMatrix, row, col, VARINT_WIDTH_8B, mat);
                if (result != val) {
                    ERR("Didn't get %d at (%d, %d); got: %d!", val, row, col,
                        result);
                }
            }
        }

        free(totalMatrix);
    }

    TEST("64x64 int8_t matrix: set and get every entry position") {
        varintDimensionPair widthSizes = varintDimensionPairDimension(64, 64);
        if (VARINT_DIMENSION_PAIR_BYTE_LENGTH(widthSizes) != 2) {
            ERRR("Didn't get correct length storage byte width!");
        }

        uint8_t *totalMatrix = calloc(1, 2 + ((64 * 64) * sizeof(uint8_t)));
        varintDimensionPair mat =
            varintDimensionPairEncode(totalMatrix, 64, 64);

        for (int row = 0; row < 64; row++) {
            for (int col = 0; col < 64; col++) {
                int8_t val = -1 * (row + col);
                varintDimensionPairEntrySetUnsigned(totalMatrix, row, col, val,
                                                    VARINT_WIDTH_8B, mat);
                int8_t result = varintDimensionPairEntryGetUnsigned(
                    totalMatrix, row, col, VARINT_WIDTH_8B, mat);
                if (result != val) {
                    ERR("Didn't get %d at (%d, %d); got: %d!", val, row, col,
                        result);
                }
            }
        }

        free(totalMatrix);
    }

    TEST("64x64 uint24_t matrix: set and get every entry position") {
        varintDimensionPair widthSizes = varintDimensionPairDimension(64, 64);
        if (VARINT_DIMENSION_PAIR_BYTE_LENGTH(widthSizes) != 2) {
            ERRR("Didn't get correct length storage byte width!");
        }

        uint8_t *totalMatrix =
            calloc(1, 2 + ((64 * 64) * (3 * sizeof(uint8_t))));
        varintDimensionPair mat =
            varintDimensionPairEncode(totalMatrix, 64, 64);

        for (int row = 0; row < 64; row++) {
            for (int col = 0; col < 64; col++) {
                uint32_t val = 81944 + row + col;
                varintDimensionPairEntrySetUnsigned(totalMatrix, row, col, val,
                                                    VARINT_WIDTH_24B, mat);
                uint32_t result = varintDimensionPairEntryGetUnsigned(
                    totalMatrix, row, col, VARINT_WIDTH_24B, mat);
                if (result != val) {
                    ERR("Didn't get %d at (%d, %d); got: %d!", val, row, col,
                        result);
                }
            }
        }

        free(totalMatrix);
    }

    TEST("64x64 int24_t matrix: set and get every entry position") {
        varintDimensionPair widthSizes = varintDimensionPairDimension(64, 64);
        if (VARINT_DIMENSION_PAIR_BYTE_LENGTH(widthSizes) != 2) {
            ERRR("Didn't get correct length storage byte width!");
        }

        uint8_t *totalMatrix =
            calloc(1, 2 + ((64 * 64) * (3 * sizeof(uint8_t))));
        varintDimensionPair mat =
            varintDimensionPairEncode(totalMatrix, 64, 64);

        for (int row = 0; row < 64; row++) {
            for (int col = 0; col < 64; col++) {
                int32_t val = -81944 + row + col;
                int32_t originalVal = val;
                /* Since we are 24 bit values, we must place our sign bit
                 * in the bounds of a 24 bit value, not at 32 bit boundary. */
                varintPrepareSigned32to24_(val);
                varintDimensionPairEntrySetUnsigned(totalMatrix, row, col, val,
                                                    VARINT_WIDTH_24B, mat);
                int32_t result = varintDimensionPairEntryGetUnsigned(
                    totalMatrix, row, col, VARINT_WIDTH_24B, mat);
                /* If our 24 bit value has a sign bit at the 24th position,
                 * move the sign bit up to proper 32-bit signed value location.
                 */
                varintRestoreSigned24to32_(result);
                if (result != originalVal) {
                    ERR("Didn't get %d at (%d, %d); got: %d!", val, row, col,
                        result);
                }
            }
        }

        free(totalMatrix);
    }

    TEST("float matrix: set and get every entry position") {
    }

    TEST("double matrix: set and get every entry position") {
    }

    TEST("half float matrix: set and get every entry position") {
    }

    TEST_FINAL_RESULT;
    return 0;
}

#endif
