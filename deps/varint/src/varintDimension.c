#include "varintDimension.h"
#include "varintExternal.h"

/* ====================================================================
 * Dimension Packing (row, col) = [XY] as single integer
 * ==================================================================== */
bool varintDimensionPack(const size_t row, const size_t col, uint64_t *result,
                         varintDimensionPacked *dimension) {
    varintDimensionPacked foundDimension = VARINT_DIMENSION_PACKED_1;

    /* Find minimum dimension that can hold the larger of row or col.
     * Each dimension level adds 4 bits:
     *   PACKED_1 =  4 bits = 0-15
     *   PACKED_2 =  8 bits = 0-255
     *   PACKED_3 = 12 bits = 0-4095
     *   PACKED_4 = 16 bits = 0-65535
     *   PACKED_5 = 20 bits = 0-1048575
     *   PACKED_6 = 24 bits = 0-16777215
     *   PACKED_7 = 28 bits = 0-268435455
     *   PACKED_8 = 32 bits = 0-4294967295
     */
    size_t maxCoord = (row > col) ? row : col;

    /* Check which level we need by shifting right by 4 bits per level */
    while (maxCoord >=
           (1ULL << VARINT_DIMENSION_PACKED_TO_BITS(foundDimension))) {
        foundDimension++;
        if (foundDimension > VARINT_DIMENSION_PACKED_8) {
            return false;
        }
    }

    *result = row;
    *result <<= VARINT_DIMENSION_PACKED_TO_BITS(foundDimension);
    *result |= col;

    *dimension = foundDimension;

    return true;
}

void varintDimensionUnpack(size_t *rows, size_t *cols, const uint64_t packed,
                           const varintDimensionPacked dimension) {
    varintDimensionUnpack_(*rows, *cols, packed, dimension);
}

/* ====================================================================
 * Dimension Pairing (row, col) = [X][Y] as individual external varints
 * ==================================================================== */
varintDimensionPair varintDimensionPairDimension(const size_t rows,
                                                 const size_t cols) {
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

varintDimensionPair varintDimensionPairEncode(void *_dst, const size_t row,
                                              const size_t col) {
    uint8_t *dst = (uint8_t *)_dst;

    varintWidth widthRows;
    varintWidth widthCols;
    const varintDimensionPair dimension =
        varintDimensionPairDimension(row, col);
    VARINT_DIMENSION_PAIR_DEPAIR(widthRows, widthCols, dimension);

    /* Handle zero-row case (vectors): don't encode row count if width is 0 */
    if (widthRows) {
        varintExternalPutFixedWidth(dst, row, widthRows);
    }
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
        (uint8_t)VARINT_DIMENSION_PAIR_BYTE_LENGTH(dimension);
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
        const uint8_t metadataSize = (uint8_t)(widthRows + widthCols);         \
        size_t _bit_offsetTotal;                                               \
        if (row) {                                                             \
            const size_t cols =                                                \
                varintExternalGet((arr) + widthRows, widthCols);               \
            _bit_offsetTotal = (((row) * cols) + (col));                       \
        } else {                                                               \
            _bit_offsetTotal = (col);                                          \
        }                                                                      \
        (offsetByte) = metadataSize + (_bit_offsetTotal / 8);                  \
        (offsetBit) = _bit_offsetTotal % 8;                                    \
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

    memcpy(dst + entryOffset, &entryValue, sizeof(float));
}

float varintDimensionPairEntryGetFloat(const void *_src, const size_t row,
                                       const size_t col,
                                       const varintDimensionPair dimension) {
    const uint8_t *src = (const uint8_t *)_src;

    const size_t entryOffset =
        getEntryByteOffset(src, row, col, sizeof(float), dimension);

    float result;
    memcpy(&result, src + entryOffset, sizeof(float));
    return result;
}

/* ====================================================================
 * Half-Precision (FP16) Float Operations
 * ==================================================================== */

/* x86/x64 F16C intrinsics */
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) ||             \
    defined(_M_IX86)
#include <x86intrin.h>
#endif

/* ARM NEON FP16 intrinsics */
#if defined(__ARM_NEON) && defined(__ARM_FP16_FORMAT_IEEE)
#include <arm_neon.h>
#endif

#ifdef __F16C__
/* x86/x64 F16C implementation */
void varintDimensionPairEntrySetFloatHalf(void *_dst, const size_t row,
                                          const size_t col,
                                          const float entryValue,
                                          const varintDimensionPair dimension) {
    uint8_t *dst = (uint8_t *)_dst;

    const size_t entryOffset =
        getEntryByteOffset(dst, row, col, sizeof(uint16_t), dimension);

    /* Use aligned buffers for SSE operations */
    float holder[4] __attribute__((aligned(16))) = {entryValue, 0, 0, 0};
    uint16_t half_holder[8] __attribute__((aligned(16))) = {0};

    __m128 float_vector = _mm_load_ps(holder);
    __m128i half_vector = _mm_cvtps_ph(float_vector, 0);
    _mm_store_si128((__m128i *)half_holder, half_vector);

    memcpy(dst + entryOffset, half_holder, sizeof(uint16_t));
}

float varintDimensionPairEntryGetFloatHalf(
    const void *_src, const size_t row, const size_t col,
    const varintDimensionPair dimension) {
    const uint8_t *src = (const uint8_t *)_src;

    const size_t entryOffset =
        getEntryByteOffset(src, row, col, sizeof(uint16_t), dimension);

    uint16_t half_holder[8] __attribute__((aligned(16))) = {0};
    memcpy(half_holder, src + entryOffset, sizeof(uint16_t));

    __m128 vector = _mm_cvtph_ps(_mm_load_si128((__m128i *)half_holder));
    float result[4] __attribute__((aligned(16)));
    _mm_store_ps(result, vector);
    return result[0];
}
#elif defined(__ARM_NEON) && defined(__ARM_FP16_FORMAT_IEEE)
/* ARM NEON FP16 implementation */
void varintDimensionPairEntrySetFloatHalf(void *_dst, const size_t row,
                                          const size_t col,
                                          const float entryValue,
                                          const varintDimensionPair dimension) {
    uint8_t *dst = (uint8_t *)_dst;

    const size_t entryOffset =
        getEntryByteOffset(dst, row, col, sizeof(uint16_t), dimension);

    /* Convert single-precision float to half-precision using NEON */
    float32x4_t float_vec = vdupq_n_f32(entryValue);
    float16x4_t half_vec = vcvt_f16_f32(float_vec);
    uint16_t half_value = vget_lane_u16(vreinterpret_u16_f16(half_vec), 0);

    memcpy(dst + entryOffset, &half_value, sizeof(uint16_t));
}

float varintDimensionPairEntryGetFloatHalf(
    const void *_src, const size_t row, const size_t col,
    const varintDimensionPair dimension) {
    const uint8_t *src = (const uint8_t *)_src;

    const size_t entryOffset =
        getEntryByteOffset(src, row, col, sizeof(uint16_t), dimension);

    uint16_t half_value;
    memcpy(&half_value, src + entryOffset, sizeof(uint16_t));

    /* Convert half-precision to single-precision using NEON */
    uint16x4_t half_vec = vdup_n_u16(half_value);
    float16x4_t f16_vec = vreinterpret_f16_u16(half_vec);
    float32x4_t float_vec = vcvt_f32_f16(f16_vec);
    return vgetq_lane_f32(float_vec, 0);
}
#endif /* __F16C__ / __ARM_NEON */

void varintDimensionPairEntrySetDouble(void *_dst, const size_t row,
                                       const size_t col,
                                       const double entryValue,
                                       const varintDimensionPair dimension) {
    uint8_t *dst = (uint8_t *)_dst;

    const size_t entryOffset =
        getEntryByteOffset(dst, row, col, sizeof(double), dimension);

    memcpy(dst + entryOffset, &entryValue, sizeof(double));
}

double varintDimensionPairEntryGetDouble(const void *_src, const size_t row,
                                         const size_t col,
                                         const varintDimensionPair dimension) {
    const uint8_t *src = (const uint8_t *)_src;

    const size_t entryOffset =
        getEntryByteOffset(src, row, col, sizeof(double), dimension);

    double result;
    memcpy(&result, src + entryOffset, sizeof(double));
    return result;
}

void varintDimensionPairEntrySetBit(void *_dst, const size_t row,
                                    const size_t col, const bool setBit,
                                    const varintDimensionPair dimension) {
    uint8_t *dst = (uint8_t *)_dst;
    size_t offsetByte;
    uint8_t offsetBit;
    _bitOffsets(dst, row, col, dimension, offsetByte, offsetBit);

    dst[offsetByte] |= setBit << offsetBit;
}

bool varintDimensionPairEntryToggleBit(void *_dst, const size_t row,
                                       const size_t col,
                                       const varintDimensionPair dimension) {
    uint8_t *dst = (uint8_t *)_dst;
    size_t offsetByte;
    uint8_t offsetBit;
    _bitOffsets(dst, row, col, dimension, offsetByte, offsetBit);

    const bool oldValue = ((dst[offsetByte] >> offsetBit) & 0x01);
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
        const varintDimensionPair widthSizes =
            varintDimensionPairDimension(64, 64);
        if (VARINT_DIMENSION_PAIR_BYTE_LENGTH(widthSizes) != 2) {
            ERRR("Didn't get correct length storage byte width!");
        }

        uint8_t totalMatrix[2 + (64 * 64)] = {0};
        const varintDimensionPair mat =
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
        const varintDimensionPair widthSizes =
            varintDimensionPairDimension(6400, 6400);
        if (VARINT_DIMENSION_PAIR_BYTE_LENGTH(widthSizes) != 4) {
            ERRR("Didn't get correct length storage byte width!");
        }

        uint8_t *totalMatrix = calloc(1, 4 + (6400 * 6400));
        if (!totalMatrix) {
            ERRR("Failed to allocate memory for totalMatrix!");
        }
        const varintDimensionPair mat =
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
        const varintDimensionPair widthSizes =
            varintDimensionPairDimension(64, 64);
        if (VARINT_DIMENSION_PAIR_BYTE_LENGTH(widthSizes) != 2) {
            ERRR("Didn't get correct length storage byte width!");
        }

        uint8_t *totalMatrix = calloc(1, 2 + ((64 * 64) * sizeof(uint8_t)));
        if (!totalMatrix) {
            ERRR("Failed to allocate memory for totalMatrix!");
        }
        const varintDimensionPair mat =
            varintDimensionPairEncode(totalMatrix, 64, 64);

        for (int row = 0; row < 64; row++) {
            for (int col = 0; col < 64; col++) {
                uint8_t val = (row + col);
                varintDimensionPairEntrySetUnsigned(totalMatrix, row, col, val,
                                                    VARINT_WIDTH_8B, mat);
                uint8_t result = varintDimensionPairEntryGetUnsigned(
                    totalMatrix, row, col, VARINT_WIDTH_8B, mat);
                if (result != val) {
                    ERR("Didn't get %u at (%u, %u); got: %u!", (unsigned)val,
                        (unsigned)row, (unsigned)col, (unsigned)result);
                }
            }
        }

        free(totalMatrix);
    }

    TEST("64x64 int8_t matrix: set and get every entry position") {
        const varintDimensionPair widthSizes =
            varintDimensionPairDimension(64, 64);
        if (VARINT_DIMENSION_PAIR_BYTE_LENGTH(widthSizes) != 2) {
            ERRR("Didn't get correct length storage byte width!");
        }

        uint8_t *totalMatrix = calloc(1, 2 + ((64 * 64) * sizeof(uint8_t)));
        if (!totalMatrix) {
            ERRR("Failed to allocate memory for totalMatrix!");
        }
        const varintDimensionPair mat =
            varintDimensionPairEncode(totalMatrix, 64, 64);

        for (int row = 0; row < 64; row++) {
            for (int col = 0; col < 64; col++) {
                int8_t val = -1 * (row + col);
                varintDimensionPairEntrySetUnsigned(totalMatrix, row, col, val,
                                                    VARINT_WIDTH_8B, mat);
                int8_t result = varintDimensionPairEntryGetUnsigned(
                    totalMatrix, row, col, VARINT_WIDTH_8B, mat);
                if (result != val) {
                    ERR("Didn't get %u at (%u, %u); got: %u!", (unsigned)val,
                        (unsigned)row, (unsigned)col, (unsigned)result);
                }
            }
        }

        free(totalMatrix);
    }

    TEST("64x64 uint24_t matrix: set and get every entry position") {
        const varintDimensionPair widthSizes =
            varintDimensionPairDimension(64, 64);
        if (VARINT_DIMENSION_PAIR_BYTE_LENGTH(widthSizes) != 2) {
            ERRR("Didn't get correct length storage byte width!");
        }

        uint8_t *totalMatrix =
            calloc(1, 2 + ((64 * 64) * (3 * sizeof(uint8_t))));
        if (!totalMatrix) {
            ERRR("Failed to allocate memory for totalMatrix!");
        }
        const varintDimensionPair mat =
            varintDimensionPairEncode(totalMatrix, 64, 64);

        for (int row = 0; row < 64; row++) {
            for (int col = 0; col < 64; col++) {
                uint32_t val = 81944 + row + col;
                varintDimensionPairEntrySetUnsigned(totalMatrix, row, col, val,
                                                    VARINT_WIDTH_24B, mat);
                uint32_t result = varintDimensionPairEntryGetUnsigned(
                    totalMatrix, row, col, VARINT_WIDTH_24B, mat);
                if (result != val) {
                    ERR("Didn't get %u at (%u, %u); got: %u!", (unsigned)val,
                        (unsigned)row, (unsigned)col, (unsigned)result);
                }
            }
        }

        free(totalMatrix);
    }

    TEST("64x64 int24_t matrix: set and get every entry position") {
        const varintDimensionPair widthSizes =
            varintDimensionPairDimension(64, 64);
        if (VARINT_DIMENSION_PAIR_BYTE_LENGTH(widthSizes) != 2) {
            ERRR("Didn't get correct length storage byte width!");
        }

        uint8_t *totalMatrix =
            calloc(1, 2 + ((64 * 64) * (3 * sizeof(uint8_t))));
        if (!totalMatrix) {
            ERRR("Failed to allocate memory for totalMatrix!");
        }
        const varintDimensionPair mat =
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
                    ERR("Didn't get %u at (%u, %u); got: %u!", (unsigned)val,
                        (unsigned)row, (unsigned)col, (unsigned)result);
                }
            }
        }

        free(totalMatrix);
    }

    TEST("DEPAIR macro: correctly unpacks dimension pair widths") {
        /* Test the DEPAIR macro fix - it should use the passed dimension */
        const varintDimensionPair dim1 = varintDimensionPairDimension(100, 200);
        const varintDimensionPair dim2 = varintDimensionPairDimension(1000, 50);

        varintWidth rowWidth1, colWidth1;
        VARINT_DIMENSION_PAIR_DEPAIR(rowWidth1, colWidth1, dim1);
        if (rowWidth1 != 1 || colWidth1 != 1) {
            ERR("DEPAIR(100,200): expected (1,1), got (%d,%d)", rowWidth1,
                colWidth1);
        }

        varintWidth rowWidth2, colWidth2;
        VARINT_DIMENSION_PAIR_DEPAIR(rowWidth2, colWidth2, dim2);
        if (rowWidth2 != 2 || colWidth2 != 1) {
            ERR("DEPAIR(1000,50): expected (2,1), got (%d,%d)", rowWidth2,
                colWidth2);
        }
    }

    TEST("Pack and Unpack coordinate pairs") {
        /* Test various coordinate values */
        struct {
            size_t row;
            size_t col;
        } testCases[] = {
            {0, 0},       {1, 1},       {15, 15},     {100, 200},
            {255, 255},   {1000, 500},  {4095, 4095}, {65535, 65535},
            {100000, 50}, {50, 100000},
        };

        for (size_t i = 0; i < sizeof(testCases) / sizeof(testCases[0]); i++) {
            size_t row = testCases[i].row;
            size_t col = testCases[i].col;
            uint64_t packed;
            varintDimensionPacked dim;

            if (!varintDimensionPack(row, col, &packed, &dim)) {
                ERR("Pack failed for (%zu, %zu)", row, col);
                continue;
            }

            size_t unpackedRow, unpackedCol;
            varintDimensionUnpack(&unpackedRow, &unpackedCol, packed, dim);

            if (unpackedRow != row || unpackedCol != col) {
                ERR("Pack/Unpack mismatch: (%zu,%zu) -> packed -> (%zu,%zu)",
                    row, col, unpackedRow, unpackedCol);
            }
        }
    }

    TEST("Various dimension sizes (edge cases)") {
        /* Test boundary conditions for dimension storage */
        struct {
            size_t rows;
            size_t cols;
            varintWidth expectedRowWidth;
            varintWidth expectedColWidth;
        } testCases[] = {
            {1, 1, 1, 1},         /* Minimum */
            {255, 255, 1, 1},     /* 1-byte boundary */
            {256, 256, 2, 2},     /* Just over 1-byte */
            {65535, 65535, 2, 2}, /* 2-byte boundary */
            {65536, 65536, 3, 3}, /* Just over 2-byte */
            {1, 65536, 1, 3},     /* Asymmetric */
            {65536, 1, 3, 1},     /* Asymmetric reversed */
        };

        for (size_t i = 0; i < sizeof(testCases) / sizeof(testCases[0]); i++) {
            size_t rows = testCases[i].rows;
            size_t cols = testCases[i].cols;
            varintWidth expectedRow = testCases[i].expectedRowWidth;
            varintWidth expectedCol = testCases[i].expectedColWidth;

            const varintDimensionPair dim =
                varintDimensionPairDimension(rows, cols);
            varintWidth actualRow = VARINT_DIMENSION_PAIR_WIDTH_ROW_COUNT(dim);
            varintWidth actualCol = VARINT_DIMENSION_PAIR_WIDTH_COL_COUNT(dim);

            if (actualRow != expectedRow || actualCol != expectedCol) {
                ERR("Dimension(%zu,%zu): expected widths (%d,%d), got (%d,%d)",
                    rows, cols, expectedRow, expectedCol, actualRow, actualCol);
            }
        }
    }

    TEST("float matrix: set and get every entry position") {
        const varintDimensionPair widthSizes =
            varintDimensionPairDimension(64, 64);
        if (VARINT_DIMENSION_PAIR_BYTE_LENGTH(widthSizes) != 2) {
            ERRR("Didn't get correct length storage byte width!");
        }

        uint8_t *totalMatrix = calloc(1, 2 + ((64 * 64) * sizeof(float)));
        if (!totalMatrix) {
            ERRR("Failed to allocate memory for totalMatrix!");
        }
        const varintDimensionPair mat =
            varintDimensionPairEncode(totalMatrix, 64, 64);

        for (int row = 0; row < 64; row++) {
            for (int col = 0; col < 64; col++) {
                float val = (float)(row * 100 + col) / 10.0f + 0.123f;
                varintDimensionPairEntrySetFloat(totalMatrix, row, col, val,
                                                 mat);
                float result = varintDimensionPairEntryGetFloat(totalMatrix,
                                                                row, col, mat);
                if (result != val) {
                    ERR("Float: Didn't get %f at (%d, %d); got: %f!", val, row,
                        col, result);
                }
            }
        }

        free(totalMatrix);
    }

    TEST("double matrix: set and get every entry position") {
        const varintDimensionPair widthSizes =
            varintDimensionPairDimension(64, 64);
        if (VARINT_DIMENSION_PAIR_BYTE_LENGTH(widthSizes) != 2) {
            ERRR("Didn't get correct length storage byte width!");
        }

        uint8_t *totalMatrix = calloc(1, 2 + ((64 * 64) * sizeof(double)));
        if (!totalMatrix) {
            ERRR("Failed to allocate memory for totalMatrix!");
        }
        const varintDimensionPair mat =
            varintDimensionPairEncode(totalMatrix, 64, 64);

        for (int row = 0; row < 64; row++) {
            for (int col = 0; col < 64; col++) {
                double val =
                    (double)(row * 100 + col) / 10.0 + 3.14159265358979323846;
                varintDimensionPairEntrySetDouble(totalMatrix, row, col, val,
                                                  mat);
                double result = varintDimensionPairEntryGetDouble(
                    totalMatrix, row, col, mat);
                if (result != val) {
                    ERR("Double: Didn't get %f at (%d, %d); got: %f!", val, row,
                        col, result);
                }
            }
        }

        free(totalMatrix);
    }

    TEST("half float matrix: set and get every entry position") {
#if defined(__F16C__) ||                                                       \
    (defined(__ARM_NEON) && defined(__ARM_FP16_FORMAT_IEEE))
        const varintDimensionPair widthSizes =
            varintDimensionPairDimension(32, 32);
        if (VARINT_DIMENSION_PAIR_BYTE_LENGTH(widthSizes) != 2) {
            ERRR("Didn't get correct length storage byte width!");
        }

        uint8_t *totalMatrix = calloc(1, 2 + ((32 * 32) * sizeof(uint16_t)));
        if (!totalMatrix) {
            ERRR("Failed to allocate memory for totalMatrix!");
        }
        const varintDimensionPair mat =
            varintDimensionPairEncode(totalMatrix, 32, 32);

        for (int row = 0; row < 32; row++) {
            for (int col = 0; col < 32; col++) {
                /* Use values that can be represented in half precision */
                float val = (float)(row + col) * 0.5f;
                varintDimensionPairEntrySetFloatHalf(totalMatrix, row, col, val,
                                                     mat);
                float result = varintDimensionPairEntryGetFloatHalf(
                    totalMatrix, row, col, mat);
                /* Half precision has limited accuracy, so allow some tolerance
                 */
                float diff = result - val;
                if (diff < 0) {
                    diff = -diff;
                }
                if (diff > 0.01f) {
                    ERR("Half: Didn't get ~%f at (%d, %d); got: %f!", val, row,
                        col, result);
                }
            }
        }

        free(totalMatrix);
#else
        /* Skip test if FP16 hardware not available */
        (void)0;
#endif
    }

    TEST_FINAL_RESULT;
    return 0;
}

#endif
