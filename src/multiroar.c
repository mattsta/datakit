// #define DK_TEST_VERBOSE 1

#include "multiroar.h"
#include "../deps/varint/src/varintSplitFull.h"
#include "../deps/varint/src/varintTagged.h"

#include "str.h" /* optimized popcnt helpers */
#include <inttypes.h>

struct multiroar {
    multimap *map;
    uint8_t meta[];
    /* meta is an array in this order:
     *   - 1 byte; bit width of elements as uint8_t
     *   - 1 to 9 bytes; col count as SplitFull varint
     *   - 1 to 9 bytes; row count as SplitFull varint */
    /* meta is between 3 bytes and 19 bytes long. */
    /* Note: we store 'col' before 'row' because when this is
     *       just a bitmap, we only care about 'col' and not 'row' */
};

/* Overall file todo:
 *   - Finish flexing out grow/shrink operations.
 *   - Write bitmap combining functions (and, or, xor, etc)
 *   - Cleanup unused things warnings
 *   - Add optional mode for auto sparse/dense general value matrices */

/* TODO: make BITMAP_SIZE_IN_BITS configurable?  Include it in metadata? */
#define BITMAP_SIZE_IN_BITS 8192

#define BITMAP_SIZE_IN_BYTES (BITMAP_SIZE_IN_BITS / 8)

__attribute__((unused)) static const uint64_t
    _allZeroes[BITMAP_SIZE_IN_BYTES / sizeof(uint64_t)] = {0};
__attribute__((unused)) static const uint64_t
    _allOnes[BITMAP_SIZE_IN_BYTES / sizeof(uint64_t)] = {
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL,
        0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL};

/* 13 bits because log2(8192) == 13 */
#define DIRECT_STORAGE_BITS 13

#define divCeil(a, b) (((a) + (b) - 1) / (b))

/* This MAX_ENTRIES calculation makes sure we don't store more elements in
 * our explicit lists than would be used by just using a bitmap directly. */
#define MAX_ENTRIES_PER_DIRECT_LISTING                                         \
    ((BITMAP_SIZE_IN_BITS / DIRECT_STORAGE_BITS) - 1)

/* Unused but kept for potential future use */
/* #define BYTES_FOR_PACKED_ARRAY_COUNT(count) divCeil(count, 8) */

#define MAX_BITMAP_ENTIRES_BEFORE_NEGATIVE_LISTING                             \
    (BITMAP_SIZE_IN_BITS - MAX_ENTRIES_PER_DIRECT_LISTING)

/* ====================================================================
 * Packed Bit Management
 * ==================================================================== */
#define PACK_STORAGE_BITS DIRECT_STORAGE_BITS
#define PACK_MAX_ELEMENTS MAX_ENTRIES_PER_DIRECT_LISTING
#define PACK_STORAGE_SLOT_STORAGE_TYPE uint16_t
#define PACK_STORAGE_MICRO_PROMOTION_TYPE uint16_t
#define PACK_FUNCTION_PREFIX varintPacked
#include "../deps/varint/src/varintPacked.h"

/* ====================================================================
 * Management Macros
 * ==================================================================== */
#define _metaOffsetToBitWidthValue(r) ((r)->meta)
#define _metaOffsetToColValue(r) ((r)->meta + 1)
#define _metaOffsetToRowValue(r)                                               \
    ((r)->meta + 1 + varintSplitFullGetLenQuick_(_metaOffsetToColValue(r)))

/* Unused but kept for potential future use */
/* #define _metaCols(r) varintSplitFullGet(_metaOffsetToColValue_(r)) */
/* #define _metaRows(r) varintSplitFullGet(_metaOffsetToRowValue_(r)) */

/* 2 elements per entry because our layout is:
 *   - chunk number (unsigned integer)
 *   - chunk value (custom binary format) */
#define ELEMENTS_PER_ENTRY 2

typedef enum chunkType {
    CHUNK_TYPE_ALL_0 = 0, /* implicit; when all == 0, chunk doesn't exist. */
    CHUNK_TYPE_ALL_1, /* represented by value being TRUE instead of bytes. */
    CHUNK_TYPE_UNDER_FULL_DIRECT_POSITION_NUMBERS,
    CHUNK_TYPE_FULL_BITMAP,
    CHUNK_TYPE_OVER_FULL_DIRECT_NOT_SET_POSITION_NUMBERS,
    CHUNKY_MONKEY,
    CHUNK_TYPE_MAX_TYPE = 255
} chunkType;

/* ====================================================================
 * Creation and Destruction
 * ==================================================================== */
multiroar *multiroarBitNew(void) {
    multiroar *r = zcalloc(1, sizeof(*r));
    r->map = multimapNew(ELEMENTS_PER_ENTRY);
    return r;
}

multiroar *multiroarValueNew(const uint8_t bitWidth, const uint64_t rows,
                             const uint64_t cols) {
    varintWidth rowWidth;
    varintWidth colWidth;

    varintSplitFullLength_(rowWidth, rows);
    varintSplitFullLength_(colWidth, cols);

    /* Allocate multiroar, which is:
     *   - 1 pointer
     *   - 1 byte for bit width (uint8_t)
     *   - 1 to 9 bytes for col count (SplitFull Varint)
     *   - 1 to 9 bytes for row count (SplitFull Varint) */
    multiroar *r = zcalloc(1, sizeof(*r) + 1 + colWidth + rowWidth);
    r->map = multimapNew(ELEMENTS_PER_ENTRY);

    varintTaggedPut64FixedWidthQuick_(_metaOffsetToColValue(r), cols, colWidth);
    varintTaggedPut64FixedWidthQuick_(_metaOffsetToRowValue(r), rows, rowWidth);

    uint8_t *valueBitWidth = _metaOffsetToBitWidthValue(r);
    *valueBitWidth = bitWidth;

    return r;
}

void multiroarFree(multiroar *r) {
    if (r) {
        multimapFree(r->map);
        zfree(r);
    }
}

/* ====================================================================
 * Set and Clear
 * ==================================================================== */
#define CHUNK(positionInBits) ((positionInBits) / BITMAP_SIZE_IN_BITS)
#define OFFSET(positionInBits) ((positionInBits) % BITMAP_SIZE_IN_BITS)

/* This depends on floor integer division (that's why (A/B * B) != A) */
#define BYTE_OFFSET(positionInBits) ((positionInBits) / 8)
#define BIT_OFFSET(positionInBits) ((positionInBits) % 8)

#define GLOBAL_POSITION_TO_CHUNK_POSITION(globalPositionInBits)                \
    ((globalPositionInBits) -                                                  \
     (CHUNK(globalPositionInBits) * BITMAP_SIZE_IN_BITS))

#define DIRECT_BIT_POSITION(position) OFFSET(position)

#define GET_CHUNK_TYPE(value) ((value)->data.bytes.start[0])
#define _CHUNK_PACKED_METADATA_SIZE(value)                                     \
    (1 + varintTaggedGetLenQuick_((value)->data.bytes.start + 1))
#define GET_CHUNK_BITMAP_START(value) ((value)->data.bytes.start + 1)
#define GET_CHUNK_PACKED_START(value)                                          \
    ((value)->data.bytes.start + _CHUNK_PACKED_METADATA_SIZE(value))
/* Minus one below because the first byte is our type indicator and isn't
 * counted as part of our packed number array. */
#define GET_CHUNK_BITMAP_LEN(value) ((value)->len - 1)
#define GET_CHUNK_PACKED_LEN(value)                                            \
    ((value)->len - _CHUNK_PACKED_METADATA_SIZE(value))

#define PACKED_COUNT_FROM_VALUE(value)                                         \
    (varintTaggedGet64Quick_((value)->data.bytes.start + 1))

DK_STATIC uint16_t insertPositionalNumber(multiroar *r, const databox *key,
                                          databox *value, multimapEntry *me,
                                          uint32_t positionalNumber) {
    /* Step 1: Check if element already exists */
    const uint16_t currentElementCount = PACKED_COUNT_FROM_VALUE(value);
    const uint16_t newElementCount = currentElementCount + 1;

    D("count: %" PRIu16 ", len: %" PRIu64 " (required: %f)\n",
      currentElementCount, (uint64_t)GET_CHUNK_PACKED_LEN(value),
      ((double)currentElementCount * 13) / 8);
    if (varintPacked13Member(GET_CHUNK_PACKED_START(value), currentElementCount,
                             positionalNumber) >= 0) {
        /* element found!  do no further processing. */
        return currentElementCount;
    }

    /* Step 2: Grow enough to fit new element */
    /* If current allocated size can hold MORE entries than our current
     * count, then we don't need to allocate more space! */
    const bool hasRoomForNewEntry = ((GET_CHUNK_PACKED_LEN(value) * 8) /
                                     DIRECT_STORAGE_BITS) > newElementCount;
    const bool growVarint = currentElementCount == VARINT_TAGGED_MAX_1;
    const uint32_t grow = (hasRoomForNewEntry ? 0 : 2) + growVarint;

    if (grow) {
        D("GROWING! (current, new counts: %d, %d; %d)\n", currentElementCount,
          newElementCount, growVarint);
        multimapResizeEntry(&r->map, me, value->len + grow);

        /* Step 2a: Re-get value because the Resize could have moved it. */
        databox *values[] = {value};
        multimapLookup(r->map, key, values);

        /* Bigger varint means the start of our packed array needs
         * to move down by exactly one byte. */
        if (growVarint) {
            memmove(value->data.bytes.start + 3, value->data.bytes.start + 2,
                    value->len - grow);
        }
    }

    /* Step 3: Increment count */
    /* NOTE: we must increment count *before* inserting below, because
     *       GET_CHUNK_PACKED_START detects the metadata length by the
     *       value of the count.  If we grew the count above, we now need
     *       to set it so GET_CHUNK_PACKED_START will return the correct
     *       start offset. */
    varintTaggedPut64(value->data.bytes.start + 1, newElementCount);

    /* Step 4: Insert new value */
    varintPacked13InsertSorted(GET_CHUNK_PACKED_START(value),
                               currentElementCount, positionalNumber);

    /* Return new count of all elements in packed array */
    return newElementCount;
}

DK_STATIC void
_convertPositionPackedArrayToBitmap(multiroar *r, const databox *key,
                                    const databox *value, multimapEntry *me,
                                    const bool convertToSetPositions) {
    (void)key;

    const uint8_t *packedArrayStart = GET_CHUNK_PACKED_START(value);
    const uint16_t currentElementCount = PACKED_COUNT_FROM_VALUE(value);

    uint8_t newBitmap[BITMAP_SIZE_IN_BYTES + 1] = {0};

    /* If converting Unset Positions, start with a fully set bitmap.
     * Use 0xFF to set all bits to 1 (not 0x01 which only sets 1 bit per byte)
     */
    if (!convertToSetPositions) {
        memset(newBitmap, 0xFF, sizeof(newBitmap));
    }

    /* Set type byte for converted chunk */
    newBitmap[0] = CHUNK_TYPE_FULL_BITMAP;

    uint8_t *bitmapStart = newBitmap + 1;
    /* Populate new bitmap with set (or unset) bits for each
     * position listed in the packed array. */
    for (uint16_t i = 0; i < currentElementCount; i++) {
        const uint16_t position = varintPacked13Get(packedArrayStart, i);

        const uint64_t byteOffset = BYTE_OFFSET(position);
        const uint32_t bitOffset = BIT_OFFSET(position);

        if (convertToSetPositions) {
            bitmapStart[byteOffset] |= (1 << bitOffset);
        } else {
            bitmapStart[byteOffset] &= ~(1 << bitOffset);
        }
    }

    /* Replace packed array with bitmap */
    const databox box = {.data.bytes.start = newBitmap,
                         .len = sizeof(newBitmap),
                         .type = DATABOX_BYTES};
    multimapReplaceEntry(&r->map, me, &box);
}

DK_STATIC void convertPositionPackedArrayToBitmap(multiroar *r,
                                                  const databox *key,
                                                  const databox *value,
                                                  multimapEntry *me) {
    _convertPositionPackedArrayToBitmap(r, key, value, me, true);
}

__attribute__((unused)) DK_STATIC void
convertNegativePositionPackedArrayToBitmap(multiroar *r, const databox *key,
                                           const databox *value,
                                           multimapEntry *me) {
    _convertPositionPackedArrayToBitmap(r, key, value, me, false);
}

/* Populate bit positions of 'bitmap' into 'positions'.
 * Parameter 'trackSetPositions' is:
 *   - 'true' if you want 'positions' to hold SET (1) positions
 *   - 'false' if you want 'positions' to hold UNSET (0) positions.
 * Note: we do no pre-alignment or over-sized cleanup, so 'bitmap'
 * must be exactly BITMAP_SIZE_IN_BYTES long. */
DK_STATIC uint16_t _bitmapToPositions(const void *bitmap, uint8_t positions[],
                                      const bool trackSetPositions) {
    const unsigned long *data = bitmap;
    const uint64_t lenAsLong = BITMAP_SIZE_IN_BYTES / (sizeof(unsigned long));
    uint64_t idx = 0;

    for (uint64_t i = 0; i < lenAsLong; i++) {
        unsigned long myword = data[i];

        if (!trackSetPositions) {
            myword = ~myword;
        }

        while (myword != 0) {
            /* Work on this 4 or 8 byte quantity until we've
             * eaten all the set bits. */
            const unsigned long unsetAfterCheck = myword & -myword;
            const int r = __builtin_ctzl(myword);
            /* Note: we use 'Set' instead of 'InsertSorted' because
             *       the set positions are emitted in already sorted
             *       order (from low to high). */
            /* Also note: Set() is 62x faster than InsertSorted(),
             *            so we get a nice latency minimization boost
             *            since we don't have to do binary searches
             *            on each insert here. */
            D("setting [%" PRIu64 "] = %" PRIu64 "\n", idx,
              i * 64 + (uint64_t)r);
            varintPacked13Set(positions, idx++, i * 64 + r);
            myword ^= unsetAfterCheck;
        }
    }
    return idx;
}

__attribute__((unused)) DK_STATIC inline uint16_t
bitmapToSetPositions(const void *bitmap, uint8_t positions[]) {
    return _bitmapToPositions(bitmap, positions, true);
}

__attribute__((unused)) DK_STATIC inline uint16_t
bitmapToNegativePositions(const void *bitmap, uint8_t positions[]) {
    return _bitmapToPositions(bitmap, positions, false);
}

DK_STATIC void convertBitmapToPositionList(
    multiroar *r, const databox *key __attribute__((unused)),
    const databox *value, multimapEntry *me, bool trackSetPositions) {
    uint8_t packedArray[BITMAP_SIZE_IN_BYTES + 16] = {0};
    uint8_t *packedArrayStart = packedArray + 1;

    if (trackSetPositions) {
        packedArray[0] = CHUNK_TYPE_UNDER_FULL_DIRECT_POSITION_NUMBERS;
    } else {
        packedArray[0] = CHUNK_TYPE_OVER_FULL_DIRECT_NOT_SET_POSITION_NUMBERS;
    }

    const int64_t packedArrayCount = _bitmapToPositions(
        GET_CHUNK_BITMAP_START(value), packedArrayStart, trackSetPositions);

    const uint64_t moveArrayBytes = divCeil(packedArrayCount * 13, 8);
    memmove(packedArray + 3, packedArray + 1, moveArrayBytes);

    varintTaggedPut64(packedArray + 1, packedArrayCount);

    const databox box = {.data.bytes.start = packedArray,
                         .len = 1 + 2 + moveArrayBytes,
                         .type = DATABOX_BYTES};
    multimapReplaceEntry(&r->map, me, &box);
}

__attribute__((unused)) DK_STATIC void convertBitmapToSparsePositionPackedArray(
    multiroar *r, const databox *key, const databox *value, multimapEntry *me) {
    convertBitmapToPositionList(r, key, value, me, true);
}

DK_STATIC void convertBitmapToSparseNegativePositionPackedArray(
    multiroar *r, const databox *key, const databox *value, multimapEntry *me) {
    convertBitmapToPositionList(r, key, value, me, false);
}

DK_STATIC uint16_t deletePackedArrayMember(multiroar *r, const databox *value,
                                           multimapEntry *me,
                                           uint16_t position) {
    uint8_t *packedArrayStart = GET_CHUNK_PACKED_START(value);
    const uint16_t currentElementCount = PACKED_COUNT_FROM_VALUE(value);
    const uint16_t newElementCount = currentElementCount - 1;

    if (varintPacked13DeleteMember(packedArrayStart, currentElementCount,
                                   position)) {
        /* We just deleted the member. Check if we need to shrink allocation. */
        const bool hasExcessSpace = ((GET_CHUNK_PACKED_LEN(value) * 8) /
                                     DIRECT_STORAGE_BITS) < newElementCount;
        const bool shrinkVarint = newElementCount == VARINT_TAGGED_MAX_1;
        const uint32_t shrink = (hasExcessSpace ? 2 : 0) + shrinkVarint;

        /* Write new decremented count */
        varintTaggedPut64(value->data.bytes.start + 1, newElementCount);

        if (shrink) {
            if (shrinkVarint) {
                /* Varint is shrinking, so move values down to cover
                 * the old second varint byte we don't need anymore. */
                memmove(value->data.bytes.start + 2,
                        value->data.bytes.start + 3, value->len - shrink);
            }

            /* Now shrink the actual entry */
            multimapResizeEntry(&r->map, me, value->len - shrink);
        }

        return newElementCount;
    }

    return currentElementCount;
}

bool multiroarBitSet(multiroar *r, uint64_t position) {
    bool previouslySet = false;
    /* Steps:
     *   For position, lookup corresponding chunk.
     *     If chunk doesn't exist, create as sparse immediate value.
     *     If exists, check encoding.
     *       If sparse immediate value && values < cutoff, add new value.
     *         If values == cutoff, convert to bitmap.
     *       If bitmap, set bit.
     *       If sparse inverse immediate value, if position exists, delete.
     *         If no remaining values, convert to ALL_SET type. */

    const databox key = {.data.u = CHUNK(position),
                         .type = DATABOX_UNSIGNED_64};
    multimapEntry me;
    if (multimapGetUnderlyingEntry(r->map, &key, &me)) {
        databox value = {{0}};
        flexGetNextByType(*me.map, &me.fe, &value);

        switch (GET_CHUNK_TYPE(&value)) {
        case CHUNK_TYPE_ALL_1:
            previouslySet = true;
            break;
        case CHUNK_TYPE_UNDER_FULL_DIRECT_POSITION_NUMBERS: {
            /* Steps:
             *   - check if position exists
             *     - true: previouslySet == true
             *     - false: previouslySet == false, grow allocation by one
             *              size, insert new position.
             */
            const uint16_t countBefore = PACKED_COUNT_FROM_VALUE(&value);
            uint16_t packedArrayCount = insertPositionalNumber(
                r, &key, &value, &me, DIRECT_BIT_POSITION(position));

            /* If count didn't change, element already existed */
            if (packedArrayCount == countBefore) {
                previouslySet = true;
            }

            if (packedArrayCount == MAX_ENTRIES_PER_DIRECT_LISTING) {
                /* Re-fetch entry because the existing value could have
                 * been realloc'd away in the insert. */
                multimapGetUnderlyingEntry(r->map, &key, &me);
                flexGetNextByType(*me.map, &me.fe, &value);

                D("CONVERTING TO FULL BITMAP!\n");
                convertPositionPackedArrayToBitmap(r, &key, &value, &me);
            }
            /* TODO: Re-order this so we immediate convert to bitmap then
             * set the new proper bit instead of setting new position
             * then throwing away all positions by upgrading to a bitmap. */
            break;
        }
        case CHUNK_TYPE_FULL_BITMAP: {
            const uint64_t byteOffset =
                BYTE_OFFSET(GLOBAL_POSITION_TO_CHUNK_POSITION(position));
            const uint32_t bitOffset = BIT_OFFSET(position);
            uint8_t *bitmapStart = GET_CHUNK_BITMAP_START(&value);

            D("Byte offset: %" PRIu64 ", bit offset: %" PRIu32 "\n", byteOffset,
              bitOffset);
            previouslySet = (bitmapStart[byteOffset] >> bitOffset) & 0x01;
            bitmapStart[byteOffset] |= (1 << bitOffset);

            /* If we now have enough set bits where storing the positions
             * of *not* set bits would be smaller, convert to sparse
             * negative direct entry list.
             * Note: we can give this check a little wiggle room.  We just
             *       need to make sure we have >= BEFORE_NEGATIVE_LISTING
             *       entries before converting so the negative sparse lists
             *       is actually smaller than the full bitmap. */
            const uint32_t population =
                StrPopCntExact(bitmapStart, GET_CHUNK_BITMAP_LEN(&value));

            if (population > MAX_BITMAP_ENTIRES_BEFORE_NEGATIVE_LISTING) {
                convertBitmapToSparseNegativePositionPackedArray(r, &key,
                                                                 &value, &me);
            }

            break;
        }
        case CHUNK_TYPE_OVER_FULL_DIRECT_NOT_SET_POSITION_NUMBERS: {
            /* Steps:
             *   - check if position exists
             *     - true: previouslySet == false, delete position, shrink
             *             allocation by one size.
             *     - false: previouslySet == true
             * Note: because these are only DELETE operations,
             *       we never have to worry about bitmap conversion here.
             *       For the NOT_SET range, we would only convert to a
             *       bitmap if we added too many entries, and we only
             *       add NOT SET entries, so convert to bitmap only
             *       happens for NOT SET during a set 0, not a set 1. */
            const uint16_t bitOffset = DIRECT_BIT_POSITION(position);
            const uint16_t currentElementCount =
                deletePackedArrayMember(r, &value, &me, bitOffset);

            /* If our negative array is empty, that means *ALL* bits are
             * set and we can reduce the entire entry down to just one type. */
            if (currentElementCount == 0) {
                uint8_t createAllOnes[1] = {CHUNK_TYPE_ALL_1};
                const databox allOnesBox = {.data.bytes.start = createAllOnes,
                                            .len = 1,
                                            .type = DATABOX_BYTES};
                multimapReplaceEntry(&r->map, &me, &allOnesBox);
            }

            break;
        }
        default:
            assert(NULL && "Invalid type byte in bitmap!");
        }
    } else {
        D("EEEEEEEEEEEEEEEEELSE AT CHUNK %" PRIu64 "!\n", CHUNK(position));
        /* else, not found, so create a sparse direct set packed array */
        uint8_t createSparse[8] = {0};
        createSparse[0] = CHUNK_TYPE_UNDER_FULL_DIRECT_POSITION_NUMBERS;
        createSparse[1] = 1; /* count of elements; actually a TaggedVarint */
        varintPacked13Set(createSparse + 2, 0, OFFSET(position));

        databox value = {{0}};
        value.data.bytes.start = createSparse;
        value.len = 5;
        value.type = DATABOX_BYTES;

        const databox *inserting[] = {&key, &value};
        multimapInsert(&r->map, inserting);
    }

    return previouslySet;
}

/* ====================================================================
 * Get
 * ==================================================================== */
bool multiroarBitGet(const multiroar *r, uint64_t position) {
    /* Steps:
     *   For position, lookup corresponding chunk.
     *     If chunk doesn't exist, value is 0.
     *     If exists, check encoding.
     *       If sparse immediate value && values < cutoff, add new value.
     *         If values == cutoff, convert to bitmap.
     *       If bitmap, set bit.
     *       If sparse inverse immediate value, if position exists, delete.
     *         If no remaining values, convert to ALL_SET type. */

    const databox key = {.data.u = CHUNK(position),
                         .type = DATABOX_UNSIGNED_64};
    databox value;
    databox *values[] = {&value};
    D("At CHUNK: %" PRIu64 " (%" PRIu64 " bytes)\n", CHUNK(position),
      multimapBytes(r->map));
    if (multimapLookup(r->map, &key, values)) {
        switch (GET_CHUNK_TYPE(&value)) {
        case CHUNK_TYPE_ALL_1:
            return true;
            break;
        case CHUNK_TYPE_UNDER_FULL_DIRECT_POSITION_NUMBERS: {
            D("Looking up direct (%" PRIu64 ", %" PRIu64 ")...\n",
              (uint64_t)PACKED_COUNT_FROM_VALUE(&value),
              (uint64_t)DIRECT_BIT_POSITION(position));
            if (varintPacked13Member(GET_CHUNK_PACKED_START(&value),
                                     PACKED_COUNT_FROM_VALUE(&value),
                                     DIRECT_BIT_POSITION(position)) >= 0) {
                return true;
            }
            break;
        }
        case CHUNK_TYPE_FULL_BITMAP: {
            D("Looking up bitmap...\n");
            const uint64_t byteOffset =
                BYTE_OFFSET(GLOBAL_POSITION_TO_CHUNK_POSITION(position));
            const uint32_t bitOffset = BIT_OFFSET(position);
            const uint8_t *bitmapStart = GET_CHUNK_BITMAP_START(&value);

            return (bitmapStart[byteOffset] >> bitOffset) & 0x01;
            break;
        }
        case CHUNK_TYPE_OVER_FULL_DIRECT_NOT_SET_POSITION_NUMBERS: {
            if (varintPacked13Member(GET_CHUNK_PACKED_START(&value),
                                     PACKED_COUNT_FROM_VALUE(&value),
                                     DIRECT_BIT_POSITION(position)) == -1) {
                return true;
            }
            break;
        }
        default:
            D("Type byte is: %d (WHY?)\n", GET_CHUNK_TYPE(&value));
            assert(NULL && "Invalid type byte in bitmap!");
        }
    }

    /* else, not found / not set */
    return false;
}

/* ====================================================================
 * Remove (Clear Bit)
 * ==================================================================== */
bool multiroarRemove(multiroar *r, uint64_t position) {
    /* Clear a bit. Returns true if bit was previously set. */
    const databox key = {.data.u = CHUNK(position),
                         .type = DATABOX_UNSIGNED_64};
    multimapEntry me;

    if (!multimapGetUnderlyingEntry(r->map, &key, &me)) {
        /* Chunk doesn't exist, bit was already 0 */
        return false;
    }

    databox value = {{0}};
    flexGetNextByType(*me.map, &me.fe, &value);

    switch (GET_CHUNK_TYPE(&value)) {
    case CHUNK_TYPE_ALL_1: {
        /* All bits were set. Convert to full bitmap with one bit clear */
        uint8_t newBitmap[BITMAP_SIZE_IN_BYTES + 1];
        memset(newBitmap, 0xFF, sizeof(newBitmap));
        newBitmap[0] = CHUNK_TYPE_FULL_BITMAP;

        /* Clear the specific bit */
        const uint64_t byteOffset =
            BYTE_OFFSET(GLOBAL_POSITION_TO_CHUNK_POSITION(position));
        const uint32_t bitOffset = BIT_OFFSET(position);
        newBitmap[1 + byteOffset] &= ~(1 << bitOffset);

        const databox box = {.data.bytes.start = newBitmap,
                             .len = sizeof(newBitmap),
                             .type = DATABOX_BYTES};
        multimapReplaceEntry(&r->map, &me, &box);
        return true;
    }
    case CHUNK_TYPE_UNDER_FULL_DIRECT_POSITION_NUMBERS: {
        /* Sparse list - need to remove position if present */
        uint8_t *packedArrayStart = GET_CHUNK_PACKED_START(&value);
        const uint16_t currentElementCount = PACKED_COUNT_FROM_VALUE(&value);
        const uint16_t bitOffset = DIRECT_BIT_POSITION(position);

        int64_t memberIdx = varintPacked13Member(
            packedArrayStart, currentElementCount, bitOffset);
        if (memberIdx < 0) {
            /* Bit wasn't set */
            return false;
        }

        /* Delete the position */
        varintPacked13Delete(packedArrayStart, currentElementCount,
                             (uint16_t)memberIdx);
        const uint16_t newCount = currentElementCount - 1;

        if (newCount == 0) {
            /* Chunk is now empty, delete it entirely */
            multimapDelete(&r->map, &key);
        } else {
            /* Update count */
            varintTaggedPut64(value.data.bytes.start + 1, newCount);
        }
        return true;
    }
    case CHUNK_TYPE_FULL_BITMAP: {
        const uint64_t byteOffset =
            BYTE_OFFSET(GLOBAL_POSITION_TO_CHUNK_POSITION(position));
        const uint32_t bitOffset = BIT_OFFSET(position);
        uint8_t *bitmapStart = GET_CHUNK_BITMAP_START(&value);

        bool wasSet = (bitmapStart[byteOffset] >> bitOffset) & 0x01;
        bitmapStart[byteOffset] &= ~(1 << bitOffset);

        /* Check if we should convert back to sparse */
        const uint32_t population =
            StrPopCntExact(bitmapStart, GET_CHUNK_BITMAP_LEN(&value));

        if (population == 0) {
            /* Chunk is empty, delete it */
            multimapDelete(&r->map, &key);
        } else if (population < MAX_ENTRIES_PER_DIRECT_LISTING / 2) {
            /* Convert back to sparse position list */
            convertBitmapToSparsePositionPackedArray(r, &key, &value, &me);
        }

        return wasSet;
    }
    case CHUNK_TYPE_OVER_FULL_DIRECT_NOT_SET_POSITION_NUMBERS: {
        /* Nearly full - this list stores UNset positions.
         * To clear a bit, we need to ADD the position to this list. */
        const uint16_t countBefore = PACKED_COUNT_FROM_VALUE(&value);
        uint16_t packedArrayCount = insertPositionalNumber(
            r, &key, &value, &me, DIRECT_BIT_POSITION(position));

        if (packedArrayCount == countBefore) {
            /* Position was already in unset list, so bit was already 0 */
            return false;
        }

        /* Check if we now have too many unset positions */
        if (packedArrayCount >= MAX_ENTRIES_PER_DIRECT_LISTING) {
            /* Convert back to full bitmap */
            multimapGetUnderlyingEntry(r->map, &key, &me);
            flexGetNextByType(*me.map, &me.fe, &value);
            convertNegativePositionPackedArrayToBitmap(r, &key, &value, &me);
        }

        return true;
    }
    default:
        assert(NULL && "Invalid type byte in bitmap!");
        return false;
    }
}

/* ====================================================================
 * Duplicate
 * ==================================================================== */
multiroar *multiroarDuplicate(const multiroar *r) {
    if (!r) {
        return NULL;
    }

    multiroar *dup = zcalloc(1, sizeof(*dup));
    dup->map = multimapCopy(r->map);
    return dup;
}

/* ====================================================================
 * Range Operations
 * ==================================================================== */
void multiroarBitSetRange(multiroar *r, uint64_t start, uint64_t extent) {
    /* Set a range of bits from 'start' to 'start + extent - 1' */
    for (uint64_t i = 0; i < extent; i++) {
        multiroarBitSet(r, start + i);
    }
    /* TODO: Optimize by setting full chunks at once */
}

/* ====================================================================
 * Bitcount - Count Total Set Bits
 * ==================================================================== */
uint64_t multiroarBitCount(const multiroar *r) {
    if (!r || !r->map) {
        return 0;
    }

    uint64_t totalCount = 0;

    /* Iterate through all chunks */
    multimapIterator iter;
    multimapIteratorInit(r->map, &iter, true);

    databox keyValue[2];
    databox *kvPtr[2] = {&keyValue[0], &keyValue[1]};

    while (multimapIteratorNext(&iter, kvPtr)) {
        const databox *value = &keyValue[1];

        switch (GET_CHUNK_TYPE(value)) {
        case CHUNK_TYPE_ALL_0:
            /* Should never happen (chunks with all zeros aren't stored) */
            break;

        case CHUNK_TYPE_ALL_1:
            /* All 8192 bits are set */
            totalCount += BITMAP_SIZE_IN_BITS;
            break;

        case CHUNK_TYPE_UNDER_FULL_DIRECT_POSITION_NUMBERS: {
            /* Sparse - count is stored in the varint */
            const uint16_t count = PACKED_COUNT_FROM_VALUE(value);
            totalCount += count;
            break;
        }

        case CHUNK_TYPE_FULL_BITMAP: {
            /* Dense bitmap - use SIMD-optimized popcount */
            const uint8_t *bitmap = GET_CHUNK_BITMAP_START(value);
            const uint32_t count =
                StrPopCntExact(bitmap, GET_CHUNK_BITMAP_LEN(value));
            totalCount += count;
            break;
        }

        case CHUNK_TYPE_OVER_FULL_DIRECT_NOT_SET_POSITION_NUMBERS: {
            /* Inverted - stores unset positions, so count = total - unset */
            const uint16_t unsetCount = PACKED_COUNT_FROM_VALUE(value);
            totalCount += (BITMAP_SIZE_IN_BITS - unsetCount);
            break;
        }

        default:
            /* Unknown chunk type - skip */
            break;
        }
    }

    return totalCount;
}

/* ====================================================================
 * Min/Max/Extrema Operations
 * ==================================================================== */

/* Forward declaration for expandChunkToBitmap */
DK_STATIC void expandChunkToBitmap(const databox *value, uint8_t *bitmap);

/* Helper: Find first set bit in chunk bitmap */
DK_STATIC uint64_t findFirstSetBitInBitmap(const uint8_t *bitmap,
                                           uint64_t bytes) {
    for (uint64_t i = 0; i < bytes; i++) {
        if (bitmap[i] != 0) {
            /* Find first set bit in this byte */
            uint8_t byte = bitmap[i];
            for (int bit = 0; bit < 8; bit++) {
                if (byte & (1 << bit)) {
                    return i * 8 + bit;
                }
            }
        }
    }
    return UINT64_MAX;
}

/* Helper: Find last set bit in chunk bitmap */
DK_STATIC uint64_t findLastSetBitInBitmap(const uint8_t *bitmap,
                                          uint64_t bytes) {
    for (uint64_t i = bytes; i > 0; i--) {
        if (bitmap[i - 1] != 0) {
            /* Find last set bit in this byte */
            uint8_t byte = bitmap[i - 1];
            for (int bit = 7; bit >= 0; bit--) {
                if (byte & (1 << bit)) {
                    return (i - 1) * 8 + bit;
                }
            }
        }
    }
    return UINT64_MAX;
}

/* Find first set bit position */
bool multiroarMin(const multiroar *r, uint64_t *position) {
    if (!r || !r->map || !position) {
        return false;
    }

    /* Get first chunk using multimap iterator */
    multimapIterator iter;
    multimapIteratorInit(r->map, &iter, true);

    databox keyValue[2];
    databox *kvPtr[2] = {&keyValue[0], &keyValue[1]};

    if (!multimapIteratorNext(&iter, kvPtr)) {
        return false; /* Empty */
    }

    const databox *key = &keyValue[0];
    const databox *value = &keyValue[1];
    uint64_t chunkId = key->data.u;
    uint64_t chunkBase = chunkId * BITMAP_SIZE_IN_BITS;

    switch (GET_CHUNK_TYPE(value)) {
    case CHUNK_TYPE_ALL_0:
        return false; /* Shouldn't happen */

    case CHUNK_TYPE_ALL_1:
        *position = chunkBase; /* First bit of chunk */
        return true;

    case CHUNK_TYPE_UNDER_FULL_DIRECT_POSITION_NUMBERS: {
        /* First position in packed array */
        const uint8_t *data = GET_CHUNK_PACKED_START(value);
        uint64_t firstPos = varintPacked13Get(data, 0);
        *position = chunkBase + firstPos;
        return true;
    }

    case CHUNK_TYPE_FULL_BITMAP: {
        /* Scan bitmap for first set bit */
        const uint8_t *bitmap = GET_CHUNK_BITMAP_START(value);
        uint64_t bitPos =
            findFirstSetBitInBitmap(bitmap, GET_CHUNK_BITMAP_LEN(value));
        if (bitPos == UINT64_MAX) {
            return false; /* Shouldn't happen for valid chunk */
        }
        *position = chunkBase + bitPos;
        return true;
    }

    case CHUNK_TYPE_OVER_FULL_DIRECT_NOT_SET_POSITION_NUMBERS: {
        /* Find first bit not in unset list */
        const uint8_t *data = GET_CHUNK_PACKED_START(value);
        const uint16_t count = PACKED_COUNT_FROM_VALUE(value);

        for (uint64_t pos = 0; pos < BITMAP_SIZE_IN_BITS; pos++) {
            bool isUnset = false;
            for (uint16_t i = 0; i < count; i++) {
                if (varintPacked13Get(data, i) == pos) {
                    isUnset = true;
                    break;
                }
            }
            if (!isUnset) {
                *position = chunkBase + pos;
                return true;
            }
        }
        return false;
    }

    default:
        return false;
    }
}

/* Find last set bit position */
bool multiroarMax(const multiroar *r, uint64_t *position) {
    if (!r || !r->map || !position) {
        return false;
    }

    /* Get last chunk using reverse multimap iterator */
    multimapIterator iter;
    multimapIteratorInit(r->map, &iter,
                         false); /* false = reverse iteration, start from end */

    databox keyValue[2];
    databox *kvPtr[2] = {&keyValue[0], &keyValue[1]};

    if (!multimapIteratorNext(&iter, kvPtr)) {
        return false; /* Empty */
    }

    const databox *key = &keyValue[0];
    const databox *value = &keyValue[1];
    uint64_t chunkId = key->data.u;
    uint64_t chunkBase = chunkId * BITMAP_SIZE_IN_BITS;

    switch (GET_CHUNK_TYPE(value)) {
    case CHUNK_TYPE_ALL_0:
        return false; /* Shouldn't happen */

    case CHUNK_TYPE_ALL_1:
        *position = chunkBase + BITMAP_SIZE_IN_BITS - 1; /* Last bit of chunk */
        return true;

    case CHUNK_TYPE_UNDER_FULL_DIRECT_POSITION_NUMBERS: {
        /* Last position in packed array */
        const uint8_t *data = GET_CHUNK_PACKED_START(value);
        const uint16_t count = PACKED_COUNT_FROM_VALUE(value);
        if (count == 0) {
            return false;
        }
        uint64_t lastPos = varintPacked13Get(data, count - 1);
        *position = chunkBase + lastPos;
        return true;
    }

    case CHUNK_TYPE_FULL_BITMAP: {
        /* Scan bitmap for last set bit */
        const uint8_t *bitmap = GET_CHUNK_BITMAP_START(value);
        uint64_t bitPos =
            findLastSetBitInBitmap(bitmap, GET_CHUNK_BITMAP_LEN(value));
        if (bitPos == UINT64_MAX) {
            return false;
        }
        *position = chunkBase + bitPos;
        return true;
    }

    case CHUNK_TYPE_OVER_FULL_DIRECT_NOT_SET_POSITION_NUMBERS: {
        /* Find last bit not in unset list */
        const uint8_t *data = GET_CHUNK_PACKED_START(value);
        const uint16_t count = PACKED_COUNT_FROM_VALUE(value);

        for (uint64_t pos = BITMAP_SIZE_IN_BITS; pos > 0; pos--) {
            uint64_t checkPos = pos - 1;
            bool isUnset = false;
            for (uint16_t i = 0; i < count; i++) {
                if (varintPacked13Get(data, i) == checkPos) {
                    isUnset = true;
                    break;
                }
            }
            if (!isUnset) {
                *position = chunkBase + checkPos;
                return true;
            }
        }
        return false;
    }

    default:
        return false;
    }
}

/* Check if bitmap is empty */
bool multiroarIsEmpty(const multiroar *r) {
    if (!r || !r->map) {
        return true;
    }
    return multimapCount(r->map) == 0;
}

/* Check if two bitmaps have any overlapping bits */
bool multiroarIntersects(const multiroar *a, const multiroar *b) {
    if (!a || !b || !a->map || !b->map) {
        return false;
    }

    /* Iterate through chunks of first bitmap */
    multimapIterator iter;
    multimapIteratorInit(a->map, &iter, true);

    databox keyValue[2];
    databox *kvPtr[2] = {&keyValue[0], &keyValue[1]};

    uint8_t bitmapA[BITMAP_SIZE_IN_BYTES];
    uint8_t bitmapB[BITMAP_SIZE_IN_BYTES];

    while (multimapIteratorNext(&iter, kvPtr)) {
        const databox *key = &keyValue[0];

        /* Check if b has this chunk */
        databox valueB;
        databox *valuesB[] = {&valueB};
        if (!multimapLookup(b->map, key, valuesB)) {
            continue; /* No overlap in this chunk */
        }

        /* Expand both chunks to bitmaps and check for any AND != 0 */
        expandChunkToBitmap(&keyValue[1], bitmapA);
        expandChunkToBitmap(&valueB, bitmapB);

        /* Check if any bit is set in both */
        for (uint64_t i = 0; i < BITMAP_SIZE_IN_BYTES; i++) {
            if ((bitmapA[i] & bitmapB[i]) != 0) {
                return true; /* Found overlap */
            }
        }
    }

    return false; /* No overlaps found */
}

/* Check if a is subset of b (a ⊆ b) */
bool multiroarIsSubset(const multiroar *a, const multiroar *b) {
    if (!a || !a->map) {
        return true; /* Empty set is subset of anything */
    }
    if (!b || !b->map) {
        return multiroarIsEmpty(a); /* Only empty is subset of empty */
    }

    /* Iterate through chunks of a */
    multimapIterator iter;
    multimapIteratorInit(a->map, &iter, true);

    databox keyValue[2];
    databox *kvPtr[2] = {&keyValue[0], &keyValue[1]};

    uint8_t bitmapA[BITMAP_SIZE_IN_BYTES];
    uint8_t bitmapB[BITMAP_SIZE_IN_BYTES];

    while (multimapIteratorNext(&iter, kvPtr)) {
        const databox *key = &keyValue[0];

        /* Check if b has this chunk */
        databox valueB;
        databox *valuesB[] = {&valueB};
        if (!multimapLookup(b->map, key, valuesB)) {
            /* a has bits in this chunk but b doesn't - not a subset */
            return false;
        }

        /* Expand both chunks and verify (a AND b) == a */
        expandChunkToBitmap(&keyValue[1], bitmapA);
        expandChunkToBitmap(&valueB, bitmapB);

        for (uint64_t i = 0; i < BITMAP_SIZE_IN_BYTES; i++) {
            if ((bitmapA[i] & bitmapB[i]) != bitmapA[i]) {
                return false; /* Found bit in a not in b */
            }
        }
    }

    return true; /* All bits of a are in b */
}

/* Check if two bitmaps are equal */
bool multiroarEquals(const multiroar *a, const multiroar *b) {
    if (!a || !b) {
        return (!a && !b);
    }
    if (!a->map || !b->map) {
        return (!a->map && !b->map);
    }

    /* Quick check: same number of chunks */
    uint64_t countA = multimapCount(a->map);
    uint64_t countB = multimapCount(b->map);
    if (countA != countB) {
        return false;
    }

    /* Iterate through chunks of a and verify b has identical chunks */
    multimapIterator iter;
    multimapIteratorInit(a->map, &iter, true);

    databox keyValue[2];
    databox *kvPtr[2] = {&keyValue[0], &keyValue[1]};

    uint8_t bitmapA[BITMAP_SIZE_IN_BYTES];
    uint8_t bitmapB[BITMAP_SIZE_IN_BYTES];

    while (multimapIteratorNext(&iter, kvPtr)) {
        const databox *key = &keyValue[0];

        /* Check if b has this chunk */
        databox valueB;
        databox *valuesB[] = {&valueB};
        if (!multimapLookup(b->map, key, valuesB)) {
            return false; /* Chunk in a but not in b */
        }

        /* Expand both chunks and compare */
        expandChunkToBitmap(&keyValue[1], bitmapA);
        expandChunkToBitmap(&valueB, bitmapB);

        if (memcmp(bitmapA, bitmapB, BITMAP_SIZE_IN_BYTES) != 0) {
            return false; /* Chunks differ */
        }
    }

    return true; /* All chunks match */
}

/* ====================================================================
 * Rank/Select Operations (Succinct Data Structure Support)
 * ==================================================================== */

/* Helper: Count set bits in a chunk */
DK_STATIC uint64_t countBitsInChunk(const databox *value) {
    switch (GET_CHUNK_TYPE(value)) {
    case CHUNK_TYPE_ALL_0:
        return 0;
    case CHUNK_TYPE_ALL_1:
        return BITMAP_SIZE_IN_BITS;
    case CHUNK_TYPE_UNDER_FULL_DIRECT_POSITION_NUMBERS:
        return PACKED_COUNT_FROM_VALUE(value);
    case CHUNK_TYPE_FULL_BITMAP: {
        const uint8_t *bitmap = GET_CHUNK_BITMAP_START(value);
        return StrPopCntExact(bitmap, GET_CHUNK_BITMAP_LEN(value));
    }
    case CHUNK_TYPE_OVER_FULL_DIRECT_NOT_SET_POSITION_NUMBERS:
        return BITMAP_SIZE_IN_BITS - PACKED_COUNT_FROM_VALUE(value);
    default:
        return 0;
    }
}

/* Helper: Count set bits in range [0, endPos) within a chunk bitmap */
DK_STATIC uint64_t countBitsInBitmapRange(const uint8_t *bitmap,
                                          uint64_t endPos) {
    uint64_t count = 0;

    /* Count full bytes */
    uint64_t fullBytes = endPos / 8;
    for (uint64_t i = 0; i < fullBytes; i++) {
        count += __builtin_popcount(bitmap[i]);
    }

    /* Count remaining bits in partial byte */
    uint64_t remainingBits = endPos % 8;
    if (remainingBits > 0) {
        uint8_t mask = (1 << remainingBits) - 1;
        count += __builtin_popcount(bitmap[fullBytes] & mask);
    }

    return count;
}

/* Rank: Count set bits in range [0, position) */
uint64_t multiroarRank(const multiroar *r, uint64_t position) {
    if (!r || !r->map) {
        return 0;
    }

    uint64_t targetChunkId = position / BITMAP_SIZE_IN_BITS;
    uint64_t offsetInChunk = position % BITMAP_SIZE_IN_BITS;
    uint64_t totalCount = 0;

    /* Iterate through chunks before target chunk */
    multimapIterator iter;
    multimapIteratorInit(r->map, &iter, true);

    databox keyValue[2];
    databox *kvPtr[2] = {&keyValue[0], &keyValue[1]};

    while (multimapIteratorNext(&iter, kvPtr)) {
        const databox *key = &keyValue[0];
        const databox *value = &keyValue[1];
        uint64_t chunkId = key->data.u;

        if (chunkId < targetChunkId) {
            /* Count all bits in chunks before target */
            totalCount += countBitsInChunk(value);
        } else if (chunkId == targetChunkId) {
            /* Count bits in range [0, offsetInChunk) within target chunk */
            if (offsetInChunk == 0) {
                return totalCount;
            }

            switch (GET_CHUNK_TYPE(value)) {
            case CHUNK_TYPE_ALL_0:
                break;
            case CHUNK_TYPE_ALL_1:
                totalCount += offsetInChunk;
                break;
            case CHUNK_TYPE_UNDER_FULL_DIRECT_POSITION_NUMBERS: {
                const uint8_t *data = GET_CHUNK_PACKED_START(value);
                uint16_t count = PACKED_COUNT_FROM_VALUE(value);
                for (uint16_t i = 0; i < count; i++) {
                    uint64_t pos = varintPacked13Get(data, i);
                    if (pos < offsetInChunk) {
                        totalCount++;
                    } else {
                        break; /* Sorted, so we're done */
                    }
                }
                break;
            }
            case CHUNK_TYPE_FULL_BITMAP: {
                const uint8_t *bitmap = GET_CHUNK_BITMAP_START(value);
                totalCount += countBitsInBitmapRange(bitmap, offsetInChunk);
                break;
            }
            case CHUNK_TYPE_OVER_FULL_DIRECT_NOT_SET_POSITION_NUMBERS: {
                /* Start with all bits set up to offsetInChunk */
                uint64_t bitsSet = offsetInChunk;
                /* Subtract unset positions before offsetInChunk */
                const uint8_t *data = GET_CHUNK_PACKED_START(value);
                uint16_t count = PACKED_COUNT_FROM_VALUE(value);
                for (uint16_t i = 0; i < count; i++) {
                    uint64_t pos = varintPacked13Get(data, i);
                    if (pos < offsetInChunk) {
                        bitsSet--;
                    } else {
                        break;
                    }
                }
                totalCount += bitsSet;
                break;
            }
            }
            return totalCount;
        } else {
            /* Beyond target chunk */
            break;
        }
    }

    return totalCount;
}

/* Select: Find position of the k-th set bit (1-indexed) */
bool multiroarSelect(const multiroar *r, uint64_t k, uint64_t *position) {
    if (!r || !r->map || k == 0 || !position) {
        return false;
    }

    uint64_t accumulatedRank = 0;

    multimapIterator iter;
    multimapIteratorInit(r->map, &iter, true);

    databox keyValue[2];
    databox *kvPtr[2] = {&keyValue[0], &keyValue[1]};

    while (multimapIteratorNext(&iter, kvPtr)) {
        const databox *key = &keyValue[0];
        const databox *value = &keyValue[1];
        uint64_t chunkId = key->data.u;
        uint64_t chunkBase = chunkId * BITMAP_SIZE_IN_BITS;

        uint64_t chunkCount = countBitsInChunk(value);

        if (accumulatedRank + chunkCount >= k) {
            /* The k-th bit is in this chunk */
            uint64_t targetInChunk =
                k - accumulatedRank; /* 1-indexed within chunk */

            switch (GET_CHUNK_TYPE(value)) {
            case CHUNK_TYPE_ALL_0:
                return false; /* Shouldn't happen */

            case CHUNK_TYPE_ALL_1:
                *position = chunkBase + (targetInChunk - 1);
                return true;

            case CHUNK_TYPE_UNDER_FULL_DIRECT_POSITION_NUMBERS: {
                const uint8_t *data = GET_CHUNK_PACKED_START(value);
                uint64_t pos = varintPacked13Get(data, targetInChunk - 1);
                *position = chunkBase + pos;
                return true;
            }

            case CHUNK_TYPE_FULL_BITMAP: {
                const uint8_t *bitmap = GET_CHUNK_BITMAP_START(value);
                uint64_t bitsLen = GET_CHUNK_BITMAP_LEN(value);
                uint64_t foundCount = 0;

                for (uint64_t i = 0; i < bitsLen; i++) {
                    uint8_t byte = bitmap[i];
                    uint8_t bitCount = __builtin_popcount(byte);

                    if (foundCount + bitCount >= targetInChunk) {
                        /* The target bit is in this byte */
                        for (int bit = 0; bit < 8; bit++) {
                            if (byte & (1 << bit)) {
                                foundCount++;
                                if (foundCount == targetInChunk) {
                                    *position = chunkBase + (i * 8 + bit);
                                    return true;
                                }
                            }
                        }
                    }
                    foundCount += bitCount;
                }
                return false; /* Shouldn't reach here */
            }

            case CHUNK_TYPE_OVER_FULL_DIRECT_NOT_SET_POSITION_NUMBERS: {
                const uint8_t *data = GET_CHUNK_PACKED_START(value);
                uint16_t unsetCount = PACKED_COUNT_FROM_VALUE(value);
                uint64_t foundCount = 0;

                for (uint64_t pos = 0; pos < BITMAP_SIZE_IN_BITS; pos++) {
                    /* Check if this position is NOT in the unset list */
                    bool isSet = true;
                    for (uint16_t i = 0; i < unsetCount; i++) {
                        if (varintPacked13Get(data, i) == pos) {
                            isSet = false;
                            break;
                        }
                    }

                    if (isSet) {
                        foundCount++;
                        if (foundCount == targetInChunk) {
                            *position = chunkBase + pos;
                            return true;
                        }
                    }
                }
                return false; /* Shouldn't reach here */
            }
            }
        }

        accumulatedRank += chunkCount;
    }

    return false; /* k exceeds total bit count */
}

/* ====================================================================
 * Range Operations
 * ==================================================================== */

/* Count bits in range [start, end) using rank */
uint64_t multiroarRangeCount(const multiroar *r, uint64_t start, uint64_t end) {
    if (!r || start >= end) {
        return 0;
    }
    return multiroarRank(r, end) - multiroarRank(r, start);
}

/* Clear range [start, start+extent) */
void multiroarBitClearRange(multiroar *r, uint64_t start, uint64_t extent) {
    if (!r || extent == 0) {
        return;
    }

    uint64_t end = start + extent;

    /* Guard against overflow */
    if (end < start) {
        end = UINT64_MAX;
    }

    /* Use AND-NOT with a temporary roar containing the range */
    multiroar *rangeRoar = multiroarBitNew();
    if (!rangeRoar) {
        return;
    }

    /* Set all bits in range */
    for (uint64_t pos = start; pos < end; pos++) {
        multiroarBitSet(rangeRoar, pos);
    }

    /* r = r AND NOT rangeRoar */
    multiroarAndNot(r, rangeRoar);
    multiroarFree(rangeRoar);
}

/* Flip range [start, start+extent) */
void multiroarBitFlipRange(multiroar *r, uint64_t start, uint64_t extent) {
    if (!r || extent == 0) {
        return;
    }

    uint64_t end = start + extent;

    /* Guard against overflow */
    if (end < start) {
        end = UINT64_MAX;
    }

    /* Use XOR with a temporary roar containing the range */
    multiroar *rangeRoar = multiroarBitNew();
    if (!rangeRoar) {
        return;
    }

    /* Set all bits in range */
    for (uint64_t pos = start; pos < end; pos++) {
        multiroarBitSet(rangeRoar, pos);
    }

    /* r = r XOR rangeRoar (flips all bits in range) */
    multiroarXor(r, rangeRoar);
    multiroarFree(rangeRoar);
}

/* Set difference: A AND NOT B (bits in A but not in B) - returns new */
multiroar *multiroarNewAndNot(const multiroar *a, const multiroar *b) {
    if (!a) {
        return NULL;
    }

    if (!b || !b->map) {
        return multiroarDuplicate(a);
    }

    multiroar *result = multiroarBitNew();
    if (!result) {
        return NULL;
    }

    /* Iterate through chunks in a */
    multimapIterator iter;
    multimapIteratorInit(a->map, &iter, true);

    databox keyValue[2];
    databox *kvPtr[2] = {&keyValue[0], &keyValue[1]};

    while (multimapIteratorNext(&iter, kvPtr)) {
        const databox *key = &keyValue[0];
        const databox *aValue = &keyValue[1];
        uint64_t chunkId = key->data.u;

        /* Check if b has this chunk */
        databox bKey = {.data.u = chunkId, .type = DATABOX_UNSIGNED_64};
        databox bValue;
        databox *bValues[] = {&bValue};

        /* Expand a's chunk to bitmap */
        uint8_t bitmapA[BITMAP_SIZE_IN_BYTES];
        expandChunkToBitmap(aValue, bitmapA);

        if (multimapLookup(b->map, &bKey, bValues)) {
            /* Both have this chunk - perform AND NOT */
            uint8_t bitmapB[BITMAP_SIZE_IN_BYTES];
            expandChunkToBitmap(&bValue, bitmapB);

            /* A & ~B */
            for (uint64_t i = 0; i < BITMAP_SIZE_IN_BYTES; i++) {
                bitmapA[i] &= ~bitmapB[i];
            }
        }
        /* else: b doesn't have this chunk, so keep all bits from a */

        /* Set the resulting bits in result */
        uint64_t chunkBase = chunkId * BITMAP_SIZE_IN_BITS;
        for (uint64_t i = 0; i < BITMAP_SIZE_IN_BITS; i++) {
            if (bitmapA[i / 8] & (1 << (i % 8))) {
                multiroarBitSet(result, chunkBase + i);
            }
        }
    }

    return result;
}

/* Set difference: r = r AND NOT b (in-place) */
void multiroarAndNot(multiroar *r, multiroar *b) {
    if (!r || !r->map || !b || !b->map) {
        return;
    }

    /* Create result using newAndNot, then swap */
    multiroar *result = multiroarNewAndNot(r, b);
    if (!result) {
        return;
    }

    /* Swap the maps */
    multimap *temp = r->map;
    r->map = result->map;
    result->map = temp;

    /* Free the old data */
    multiroarFree(result);
}

/* ====================================================================
 * Iterator - Efficient Traversal
 * ==================================================================== */

/* Initialize iterator to first set bit */
void multiroarIteratorInit(const multiroar *r, multiroarIterator *iter) {
    if (!iter) {
        return;
    }

    memset(iter, 0, sizeof(*iter));
    iter->roar = r;
    iter->valid = false;

    if (!r || !r->map) {
        return;
    }

    /* Initialize multimap iterator */
    multimapIteratorInit(r->map, &iter->mapIter, true);
    iter->valid = true;
    iter->chunkId = 0;
    iter->positionInChunk = 0;
    iter->indexInChunk = 0;
    iter->countInChunk = 0;
}

/* Get next set bit position */
bool multiroarIteratorNext(multiroarIterator *iter, uint64_t *position) {
    if (!iter || !iter->valid || !position) {
        return false;
    }

    while (true) {
        /* If we have bits remaining in current chunk, return next one */
        if (iter->indexInChunk < iter->countInChunk) {
            uint64_t chunkBase = iter->chunkId * BITMAP_SIZE_IN_BITS;

            switch (GET_CHUNK_TYPE(&iter->currentChunk)) {
            case CHUNK_TYPE_ALL_1:
                *position = chunkBase + iter->positionInChunk;
                iter->positionInChunk++;
                iter->indexInChunk++;
                return true;

            case CHUNK_TYPE_UNDER_FULL_DIRECT_POSITION_NUMBERS: {
                const uint8_t *data =
                    GET_CHUNK_PACKED_START(&iter->currentChunk);
                uint64_t bitPos = varintPacked13Get(data, iter->indexInChunk);
                *position = chunkBase + bitPos;
                iter->indexInChunk++;
                return true;
            }

            case CHUNK_TYPE_FULL_BITMAP: {
                const uint8_t *bitmap =
                    GET_CHUNK_BITMAP_START(&iter->currentChunk);
                const uint64_t bitmapLen =
                    GET_CHUNK_BITMAP_LEN(&iter->currentChunk);

                /* Scan for next set bit */
                while (iter->positionInChunk < BITMAP_SIZE_IN_BITS) {
                    uint64_t byteIdx = iter->positionInChunk / 8;
                    uint64_t bitIdx = iter->positionInChunk % 8;

                    if (byteIdx >= bitmapLen) {
                        break;
                    }

                    if (bitmap[byteIdx] & (1 << bitIdx)) {
                        *position = chunkBase + iter->positionInChunk;
                        iter->positionInChunk++;
                        iter->indexInChunk++;
                        return true;
                    }
                    iter->positionInChunk++;
                }
                break;
            }

            case CHUNK_TYPE_OVER_FULL_DIRECT_NOT_SET_POSITION_NUMBERS: {
                /* Inverted: iterate all positions, skip unset ones */
                const uint8_t *data =
                    GET_CHUNK_PACKED_START(&iter->currentChunk);
                const uint16_t unsetCount =
                    PACKED_COUNT_FROM_VALUE(&iter->currentChunk);

                while (iter->positionInChunk < BITMAP_SIZE_IN_BITS) {
                    /* Check if current position is in unset list */
                    bool isUnset = false;
                    for (uint16_t i = 0; i < unsetCount; i++) {
                        if (varintPacked13Get(data, i) ==
                            iter->positionInChunk) {
                            isUnset = true;
                            break;
                        }
                    }

                    uint64_t currentPos = iter->positionInChunk;
                    iter->positionInChunk++;

                    if (!isUnset) {
                        *position = chunkBase + currentPos;
                        iter->indexInChunk++;
                        return true;
                    }
                }
                break;
            }

            default:
                break;
            }
        }

        /* Move to next chunk */
        databox keyValue[2];
        databox *kvPtr[2] = {&keyValue[0], &keyValue[1]};

        if (!multimapIteratorNext(&iter->mapIter, kvPtr)) {
            /* No more chunks */
            iter->valid = false;
            return false;
        }

        const databox *key = &keyValue[0];
        const databox *value = &keyValue[1];

        iter->chunkId = key->data.u;
        iter->currentChunk = *value;
        iter->positionInChunk = 0;
        iter->indexInChunk = 0;

        /* Set count based on chunk type */
        switch (GET_CHUNK_TYPE(value)) {
        case CHUNK_TYPE_ALL_1:
            iter->countInChunk = BITMAP_SIZE_IN_BITS;
            break;
        case CHUNK_TYPE_UNDER_FULL_DIRECT_POSITION_NUMBERS:
            iter->countInChunk = PACKED_COUNT_FROM_VALUE(value);
            break;
        case CHUNK_TYPE_FULL_BITMAP: {
            const uint8_t *bitmap = GET_CHUNK_BITMAP_START(value);
            iter->countInChunk =
                StrPopCntExact(bitmap, GET_CHUNK_BITMAP_LEN(value));
            break;
        }
        case CHUNK_TYPE_OVER_FULL_DIRECT_NOT_SET_POSITION_NUMBERS: {
            const uint16_t unsetCount = PACKED_COUNT_FROM_VALUE(value);
            iter->countInChunk = BITMAP_SIZE_IN_BITS - unsetCount;
            break;
        }
        default:
            iter->countInChunk = 0;
            break;
        }
    }
}

/* Reset iterator to beginning */
void multiroarIteratorReset(multiroarIterator *iter) {
    if (!iter || !iter->roar) {
        return;
    }

    multiroarIteratorInit(iter->roar, iter);
}

/* ====================================================================
 * Bulk Operations
 * ==================================================================== */

/* Set multiple positions efficiently */
void multiroarBitSetMany(multiroar *r, const uint64_t *positions,
                         uint64_t count) {
    if (!r || !positions || count == 0) {
        return;
    }

    /* Simply set each position - could be optimized by batching by chunk */
    for (uint64_t i = 0; i < count; i++) {
        multiroarBitSet(r, positions[i]);
    }
}

/* Check multiple positions efficiently */
void multiroarBitGetMany(const multiroar *r, const uint64_t *positions,
                         uint64_t count, bool *results) {
    if (!r || !positions || !results || count == 0) {
        return;
    }

    /* Get each position - could be optimized by batching by chunk */
    for (uint64_t i = 0; i < count; i++) {
        results[i] = multiroarBitGet(r, positions[i]);
    }
}

/* Convert to sorted position array - returns actual count */
uint64_t multiroarToArray(const multiroar *r, uint64_t *positions,
                          uint64_t maxCount) {
    if (!r || !positions || maxCount == 0) {
        return 0;
    }

    /* Use iterator to fill array */
    multiroarIterator iter;
    multiroarIteratorInit(r, &iter);

    uint64_t count = 0;
    uint64_t pos;
    while (count < maxCount && multiroarIteratorNext(&iter, &pos)) {
        positions[count++] = pos;
    }

    return count;
}

/* Create from position array */
multiroar *multiroarFromArray(const uint64_t *positions, uint64_t count) {
    if (!positions || count == 0) {
        return NULL;
    }

    multiroar *r = multiroarBitNew();
    if (!r) {
        return NULL;
    }

    /* Set all positions */
    for (uint64_t i = 0; i < count; i++) {
        multiroarBitSet(r, positions[i]);
    }

    return r;
}

/* ====================================================================
 * Similarity and Distance Metrics
 * ==================================================================== */

/* Jaccard similarity: |A ∩ B| / |A ∪ B| ∈ [0, 1] */
double multiroarJaccard(const multiroar *a, const multiroar *b) {
    if (!a || !b) {
        return 0.0;
    }

    /* Handle empty cases */
    uint64_t countA = multiroarBitCount(a);
    uint64_t countB = multiroarBitCount(b);

    if (countA == 0 && countB == 0) {
        return 1.0; /* Both empty - perfect similarity */
    }

    /* Calculate intersection and union */
    multiroar *intersection = multiroarNewAnd(a, b);
    multiroar *unionSet = multiroarNewOr(a, b);

    uint64_t intersectionCount = multiroarBitCount(intersection);
    uint64_t unionCount = multiroarBitCount(unionSet);

    multiroarFree(intersection);
    multiroarFree(unionSet);

    if (unionCount == 0) {
        return 1.0;
    }

    return (double)intersectionCount / (double)unionCount;
}

/* Hamming distance: number of differing bits */
uint64_t multiroarHammingDistance(const multiroar *a, const multiroar *b) {
    if (!a || !b) {
        return 0;
    }

    /* Hamming distance is count of XOR */
    multiroar *xorResult = multiroarNewXor(a, b);
    uint64_t distance = multiroarBitCount(xorResult);
    multiroarFree(xorResult);

    return distance;
}

/* Overlap coefficient: |A ∩ B| / min(|A|, |B|) ∈ [0, 1] */
double multiroarOverlap(const multiroar *a, const multiroar *b) {
    if (!a || !b) {
        return 0.0;
    }

    uint64_t countA = multiroarBitCount(a);
    uint64_t countB = multiroarBitCount(b);

    if (countA == 0 || countB == 0) {
        return 0.0;
    }

    multiroar *intersection = multiroarNewAnd(a, b);
    uint64_t intersectionCount = multiroarBitCount(intersection);
    multiroarFree(intersection);

    uint64_t minCount = countA < countB ? countA : countB;
    return (double)intersectionCount / (double)minCount;
}

/* Dice coefficient: 2|A ∩ B| / (|A| + |B|) ∈ [0, 1] */
double multiroarDice(const multiroar *a, const multiroar *b) {
    if (!a || !b) {
        return 0.0;
    }

    uint64_t countA = multiroarBitCount(a);
    uint64_t countB = multiroarBitCount(b);

    if (countA == 0 && countB == 0) {
        return 1.0; /* Both empty - perfect similarity */
    }

    if (countA + countB == 0) {
        return 0.0;
    }

    multiroar *intersection = multiroarNewAnd(a, b);
    uint64_t intersectionCount = multiroarBitCount(intersection);
    multiroarFree(intersection);

    return (2.0 * intersectionCount) / (double)(countA + countB);
}

/* ====================================================================
 * Statistics and Memory
 * ==================================================================== */

/* Get memory usage in bytes */
uint64_t multiroarMemoryUsage(const multiroar *r) {
    if (!r || !r->map) {
        return sizeof(multiroar);
    }

    uint64_t totalBytes = sizeof(multiroar);

    /* Add multimap overhead */
    totalBytes += multimapBytes(r->map);

    return totalBytes;
}

/* ====================================================================
 * Serialization
 * ==================================================================== */

/*
 * Wire format:
 * - Magic: 4 bytes "ROAR"
 * - Version: 1 byte (current: 1)
 * - Flags: 1 byte (reserved, must be 0)
 * - Chunk count: varint
 * - For each chunk:
 *   - Chunk ID: varint
 *   - Chunk type: 1 byte
 *   - Chunk data (depends on type):
 *     - ALL_0: nothing
 *     - ALL_1: nothing
 *     - UNDER_FULL: varint count, then varintPacked data
 *     - FULL_BITMAP: 1024 bytes (8192 bits)
 *     - OVER_FULL: varint count, then varintPacked data
 */

#define MULTIROAR_MAGIC_0 'R'
#define MULTIROAR_MAGIC_1 'O'
#define MULTIROAR_MAGIC_2 'A'
#define MULTIROAR_MAGIC_3 'R'
#define MULTIROAR_VERSION 1

/* Helper: write varint to buffer, return bytes written */
DK_STATIC uint64_t writeVarint(uint8_t *buf, uint64_t value) {
    uint64_t bytes = 0;
    while (value >= 0x80) {
        buf[bytes++] = (uint8_t)((value & 0x7F) | 0x80);
        value >>= 7;
    }
    buf[bytes++] = (uint8_t)value;
    return bytes;
}

/* Helper: read varint from buffer, return bytes read (0 on error) */
DK_STATIC uint64_t readVarint(const uint8_t *buf, uint64_t bufSize,
                              uint64_t *value) {
    *value = 0;
    uint64_t shift = 0;
    uint64_t bytes = 0;

    while (bytes < bufSize) {
        uint8_t byte = buf[bytes++];
        *value |= ((uint64_t)(byte & 0x7F)) << shift;

        if (!(byte & 0x80)) {
            return bytes;
        }

        shift += 7;
        if (shift >= 64) {
            return 0; /* Overflow */
        }
    }

    return 0; /* Incomplete varint */
}

/* Calculate serialized size without actually serializing */
uint64_t multiroarSerializedSize(const multiroar *r) {
    if (!r || !r->map) {
        return 7; /* Magic (4) + version (1) + flags (1) + varint(0) (1) */
    }

    uint64_t size = 6; /* Magic (4) + version (1) + flags (1) */

    /* Count chunks */
    uint64_t chunkCount = multimapCount(r->map);

    /* Varint for chunk count (worst case: 10 bytes) */
    uint8_t temp[10];
    size += writeVarint(temp, chunkCount);

    /* Iterate chunks */
    multimapIterator iter;
    multimapIteratorInit(r->map, &iter, true);

    databox keyValue[2];
    databox *kvPtr[2] = {&keyValue[0], &keyValue[1]};

    while (multimapIteratorNext(&iter, kvPtr)) {
        const databox *key = kvPtr[0];
        const databox *value = kvPtr[1];

        uint64_t chunkId = key->data.u;

        /* Chunk ID varint */
        size += writeVarint(temp, chunkId);

        /* Chunk type: 1 byte */
        size += 1;

        /* Chunk data */
        uint8_t chunkType = GET_CHUNK_TYPE(value);

        switch (chunkType) {
        case CHUNK_TYPE_ALL_0:
        case CHUNK_TYPE_ALL_1:
            /* No data */
            break;

        case CHUNK_TYPE_UNDER_FULL_DIRECT_POSITION_NUMBERS:
        case CHUNK_TYPE_OVER_FULL_DIRECT_NOT_SET_POSITION_NUMBERS: {
            /* varintPacked data */
            const uint8_t *packedStart = GET_CHUNK_PACKED_START(value);
            uint64_t count = PACKED_COUNT_FROM_VALUE(value);

            /* Count varint */
            size += writeVarint(temp, count);

            /* Calculate actual varint size for each position */
            for (uint64_t i = 0; i < count; i++) {
                uint64_t pos = varintPacked13Get(packedStart, i);
                size += writeVarint(temp, pos);
            }
            break;
        }

        case CHUNK_TYPE_FULL_BITMAP:
            /* 1024 bytes */
            size += 1024;
            break;
        }
    }

    return size;
}

/* Serialize multiroar to buffer */
uint64_t multiroarSerialize(const multiroar *r, void *buf, uint64_t bufSize) {
    if (!buf || bufSize < 6) {
        return 0;
    }

    uint8_t *p = (uint8_t *)buf;
    uint8_t *end = p + bufSize;

    /* Magic */
    *p++ = MULTIROAR_MAGIC_0;
    *p++ = MULTIROAR_MAGIC_1;
    *p++ = MULTIROAR_MAGIC_2;
    *p++ = MULTIROAR_MAGIC_3;

    /* Version */
    *p++ = MULTIROAR_VERSION;

    /* Flags */
    *p++ = 0;

    if (!r || !r->map) {
        /* Empty roar: 0 chunks */
        if (p > end) {
            return 0;
        }

        uint64_t written = writeVarint(p, 0);
        return 6 + written;
    }

    /* Chunk count */
    uint64_t chunkCount = multimapCount(r->map);

    uint64_t written = writeVarint(p, chunkCount);

    p += written;

    if (p > end) {
        return 0;
    }

    /* Serialize each chunk */
    multimapIterator iter;
    multimapIteratorInit(r->map, &iter, true);

    databox keyValue[2];
    databox *kvPtr[2] = {&keyValue[0], &keyValue[1]};

    while (multimapIteratorNext(&iter, kvPtr)) {
        const databox *key = kvPtr[0];
        const databox *value = kvPtr[1];

        uint64_t chunkId = key->data.u;

        /* Chunk ID */
        written = writeVarint(p, chunkId);
        p += written;

        if (p > end) {
            return 0;
        }

        /* Chunk type */
        if (p >= end) {
            return 0;
        }
        uint8_t chunkType = GET_CHUNK_TYPE(value);
        *p++ = chunkType;

        /* Chunk data */
        switch (chunkType) {
        case CHUNK_TYPE_ALL_0:
        case CHUNK_TYPE_ALL_1:
            /* No data */
            break;

        case CHUNK_TYPE_UNDER_FULL_DIRECT_POSITION_NUMBERS:
        case CHUNK_TYPE_OVER_FULL_DIRECT_NOT_SET_POSITION_NUMBERS: {
            /* varintPacked data */
            const uint8_t *packedStart = GET_CHUNK_PACKED_START(value);
            uint64_t count = PACKED_COUNT_FROM_VALUE(value);

            /* Write count */
            written = writeVarint(p, count);
            p += written;

            if (p > end) {
                return 0;
            }

            /* Write positions */
            for (uint64_t i = 0; i < count; i++) {
                uint64_t pos = varintPacked13Get(packedStart, i);

                written = writeVarint(p, pos);
                p += written;

                if (p > end) {
                    return 0;
                }
            }
            break;
        }

        case CHUNK_TYPE_FULL_BITMAP: {
            /* Write bitmap data */
            if (p + 1024 > end) {
                return 0;
            }

            const uint8_t *bitmap = GET_CHUNK_BITMAP_START(value);
            memcpy(p, bitmap, 1024);
            p += 1024;
            break;
        }

        default:
            /* Unknown chunk type */
            return 0;
        }
    }

    return (uint64_t)(p - (uint8_t *)buf);
}

/* Deserialize multiroar from buffer */
multiroar *multiroarDeserialize(const void *buf, uint64_t bufSize) {
    if (!buf || bufSize < 6) {
        return NULL;
    }

    const uint8_t *p = (const uint8_t *)buf;
    const uint8_t *end = p + bufSize;

    /* Verify magic */
    if (p[0] != MULTIROAR_MAGIC_0 || p[1] != MULTIROAR_MAGIC_1 ||
        p[2] != MULTIROAR_MAGIC_2 || p[3] != MULTIROAR_MAGIC_3) {
        return NULL;
    }
    p += 4;

    /* Check version */
    uint8_t version = *p++;
    if (version != MULTIROAR_VERSION) {
        return NULL;
    }

    /* Flags (currently unused) */
    uint8_t flags = *p++;
    (void)flags;

    /* Read chunk count */
    uint64_t chunkCount;
    uint64_t bytesRead = readVarint(p, (uint64_t)(end - p), &chunkCount);
    if (bytesRead == 0) {
        return NULL;
    }
    p += bytesRead;

    /* Create new roar */
    multiroar *r = multiroarBitNew();
    if (!r) {
        return NULL;
    }

    /* Deserialize each chunk */
    for (uint64_t i = 0; i < chunkCount; i++) {
        /* Read chunk ID */
        uint64_t chunkId;
        bytesRead = readVarint(p, (uint64_t)(end - p), &chunkId);
        if (bytesRead == 0) {
            multiroarFree(r);
            return NULL;
        }
        p += bytesRead;

        /* Read chunk type */
        if (p >= end) {
            multiroarFree(r);
            return NULL;
        }
        uint8_t chunkType = *p++;

        /* Reconstruct chunk directly as bytes blob */
        switch (chunkType) {
        case CHUNK_TYPE_ALL_0:
            /* Skip - not stored in map */
            continue;

        case CHUNK_TYPE_ALL_1: {
            /* Create single-byte ALL_1 chunk */
            uint8_t *chunk = zcalloc(1, 1);
            if (!chunk) {
                multiroarFree(r);
                return NULL;
            }
            chunk[0] = CHUNK_TYPE_ALL_1;

            databox key = {{0}};
            key.data.u = chunkId;
            key.type = DATABOX_UNSIGNED_64;

            databox value = {{0}};
            value.data.bytes.start = chunk;
            value.len = 1;
            value.type = DATABOX_BYTES;

            const databox *inserting[] = {&key, &value};
            multimapInsert(&r->map, inserting);
            break;
        }

        case CHUNK_TYPE_UNDER_FULL_DIRECT_POSITION_NUMBERS:
        case CHUNK_TYPE_OVER_FULL_DIRECT_NOT_SET_POSITION_NUMBERS: {
            /* Read count */
            uint64_t count;
            bytesRead = readVarint(p, (uint64_t)(end - p), &count);
            if (bytesRead == 0) {
                multiroarFree(r);
                return NULL;
            }
            p += bytesRead;

            /* Calculate chunk size: 1 type + TaggedVarint count + positions */
            /* varintPacked13 is 13 bits per element, ceil to bytes */
            /* Add 1 extra byte for safety as varintPacked13Set may write
             * partial bytes */
            uint64_t packedBytes = ((count * 13) + 7) / 8 + 1;

            /* TaggedVarint can be 1-5 bytes for values up to 629 */
            uint8_t tempCount[5];
            uint64_t countBytes = varintTaggedPut64(tempCount, count);

            uint64_t chunkSize = 1 + countBytes + packedBytes;

            uint8_t *chunk = zcalloc(chunkSize, 1);
            if (!chunk) {
                multiroarFree(r);
                return NULL;
            }

            chunk[0] = chunkType;
            memcpy(chunk + 1, tempCount, countBytes); /* TaggedVarint count */

            /* Read positions and pack them */
            for (uint64_t j = 0; j < count; j++) {
                uint64_t pos;
                bytesRead = readVarint(p, (uint64_t)(end - p), &pos);
                if (bytesRead == 0) {
                    zfree(chunk);
                    multiroarFree(r);
                    return NULL;
                }
                p += bytesRead;

                varintPacked13Set(chunk + 1 + countBytes, j, pos);
            }

            databox key = {{0}};
            key.data.u = chunkId;
            key.type = DATABOX_UNSIGNED_64;

            databox value = {{0}};
            value.data.bytes.start = chunk;
            value.len = chunkSize;
            value.type = DATABOX_BYTES;

            const databox *inserting[] = {&key, &value};
            multimapInsert(&r->map, inserting);
            break;
        }

        case CHUNK_TYPE_FULL_BITMAP: {
            /* Read bitmap data */
            if (p + 1024 > end) {
                multiroarFree(r);
                return NULL;
            }

            /* Create chunk: 1 type + 1024 bitmap */
            uint8_t *chunk = zcalloc(1025, 1);
            if (!chunk) {
                multiroarFree(r);
                return NULL;
            }

            chunk[0] = CHUNK_TYPE_FULL_BITMAP;
            memcpy(chunk + 1, p, 1024);
            p += 1024;

            databox key = {{0}};
            key.data.u = chunkId;
            key.type = DATABOX_UNSIGNED_64;

            databox value = {{0}};
            value.data.bytes.start = chunk;
            value.len = 1025;
            value.type = DATABOX_BYTES;

            const databox *inserting[] = {&key, &value};
            multimapInsert(&r->map, inserting);
            break;
        }

        default:
            /* Unknown chunk type */
            multiroarFree(r);
            return NULL;
        }
    }

    return r;
}

/* ====================================================================
 * Logical Operations - SIMD Optimized
 * ==================================================================== */

/* SIMD-optimized bitmap operations */
#if defined(__AVX2__)
#include <immintrin.h>
#define HAVE_SIMD_BITMAP_AVX2 1

DK_STATIC void bitmap_and_simd(uint8_t *dst, const uint8_t *src,
                               uint64_t bytes) {
    uint64_t i = 0;
    for (; i + 32 <= bytes; i += 32) {
        __m256i a = _mm256_loadu_si256((__m256i *)(dst + i));
        __m256i b = _mm256_loadu_si256((__m256i *)(src + i));
        _mm256_storeu_si256((__m256i *)(dst + i), _mm256_and_si256(a, b));
    }
    for (; i < bytes; i++) {
        dst[i] &= src[i];
    }
}

DK_STATIC void bitmap_or_simd(uint8_t *dst, const uint8_t *src,
                              uint64_t bytes) {
    uint64_t i = 0;
    for (; i + 32 <= bytes; i += 32) {
        __m256i a = _mm256_loadu_si256((__m256i *)(dst + i));
        __m256i b = _mm256_loadu_si256((__m256i *)(src + i));
        _mm256_storeu_si256((__m256i *)(dst + i), _mm256_or_si256(a, b));
    }
    for (; i < bytes; i++) {
        dst[i] |= src[i];
    }
}

DK_STATIC void bitmap_xor_simd(uint8_t *dst, const uint8_t *src,
                               uint64_t bytes) {
    uint64_t i = 0;
    for (; i + 32 <= bytes; i += 32) {
        __m256i a = _mm256_loadu_si256((__m256i *)(dst + i));
        __m256i b = _mm256_loadu_si256((__m256i *)(src + i));
        _mm256_storeu_si256((__m256i *)(dst + i), _mm256_xor_si256(a, b));
    }
    for (; i < bytes; i++) {
        dst[i] ^= src[i];
    }
}

DK_STATIC void bitmap_not_simd(uint8_t *data, uint64_t bytes) {
    __m256i ones = _mm256_set1_epi8((char)0xFF);
    uint64_t i = 0;
    for (; i + 32 <= bytes; i += 32) {
        __m256i a = _mm256_loadu_si256((__m256i *)(data + i));
        _mm256_storeu_si256((__m256i *)(data + i), _mm256_xor_si256(a, ones));
    }
    for (; i < bytes; i++) {
        data[i] = ~data[i];
    }
}

#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>

DK_STATIC void bitmap_and_simd(uint8_t *dst, const uint8_t *src,
                               uint64_t bytes) {
    uint64_t i = 0;
    for (; i + 16 <= bytes; i += 16) {
        uint8x16_t a = vld1q_u8(dst + i);
        uint8x16_t b = vld1q_u8(src + i);
        vst1q_u8(dst + i, vandq_u8(a, b));
    }
    for (; i < bytes; i++) {
        dst[i] &= src[i];
    }
}

DK_STATIC void bitmap_or_simd(uint8_t *dst, const uint8_t *src,
                              uint64_t bytes) {
    uint64_t i = 0;
    for (; i + 16 <= bytes; i += 16) {
        uint8x16_t a = vld1q_u8(dst + i);
        uint8x16_t b = vld1q_u8(src + i);
        vst1q_u8(dst + i, vorrq_u8(a, b));
    }
    for (; i < bytes; i++) {
        dst[i] |= src[i];
    }
}

DK_STATIC void bitmap_xor_simd(uint8_t *dst, const uint8_t *src,
                               uint64_t bytes) {
    uint64_t i = 0;
    for (; i + 16 <= bytes; i += 16) {
        uint8x16_t a = vld1q_u8(dst + i);
        uint8x16_t b = vld1q_u8(src + i);
        vst1q_u8(dst + i, veorq_u8(a, b));
    }
    for (; i < bytes; i++) {
        dst[i] ^= src[i];
    }
}

DK_STATIC void bitmap_not_simd(uint8_t *data, uint64_t bytes) {
    uint64_t i = 0;
    for (; i + 16 <= bytes; i += 16) {
        uint8x16_t a = vld1q_u8(data + i);
        vst1q_u8(data + i, vmvnq_u8(a));
    }
    for (; i < bytes; i++) {
        data[i] = ~data[i];
    }
}

#else
/* Scalar fallback - use 64-bit operations for efficiency */

DK_STATIC void bitmap_and_simd(uint8_t *dst, const uint8_t *src,
                               uint64_t bytes) {
    uint64_t *d64 = (uint64_t *)dst;
    const uint64_t *s64 = (const uint64_t *)src;
    uint64_t words = bytes / 8;
    for (uint64_t i = 0; i < words; i++) {
        d64[i] &= s64[i];
    }
    for (uint64_t i = words * 8; i < bytes; i++) {
        dst[i] &= src[i];
    }
}

DK_STATIC void bitmap_or_simd(uint8_t *dst, const uint8_t *src,
                              uint64_t bytes) {
    uint64_t *d64 = (uint64_t *)dst;
    const uint64_t *s64 = (const uint64_t *)src;
    uint64_t words = bytes / 8;
    for (uint64_t i = 0; i < words; i++) {
        d64[i] |= s64[i];
    }
    for (uint64_t i = words * 8; i < bytes; i++) {
        dst[i] |= src[i];
    }
}

DK_STATIC void bitmap_xor_simd(uint8_t *dst, const uint8_t *src,
                               uint64_t bytes) {
    uint64_t *d64 = (uint64_t *)dst;
    const uint64_t *s64 = (const uint64_t *)src;
    uint64_t words = bytes / 8;
    for (uint64_t i = 0; i < words; i++) {
        d64[i] ^= s64[i];
    }
    for (uint64_t i = words * 8; i < bytes; i++) {
        dst[i] ^= src[i];
    }
}

DK_STATIC void bitmap_not_simd(uint8_t *data, uint64_t bytes) {
    uint64_t *d64 = (uint64_t *)data;
    uint64_t words = bytes / 8;
    for (uint64_t i = 0; i < words; i++) {
        d64[i] = ~d64[i];
    }
    for (uint64_t i = words * 8; i < bytes; i++) {
        data[i] = ~data[i];
    }
}
#endif

/* Helper: Expand chunk to full bitmap for logical operations */
DK_STATIC void expandChunkToBitmap(const databox *value, uint8_t *bitmap) {
    switch (GET_CHUNK_TYPE(value)) {
    case CHUNK_TYPE_ALL_1:
        memset(bitmap, 0xFF, BITMAP_SIZE_IN_BYTES);
        break;
    case CHUNK_TYPE_UNDER_FULL_DIRECT_POSITION_NUMBERS: {
        memset(bitmap, 0, BITMAP_SIZE_IN_BYTES);
        const uint8_t *packed = GET_CHUNK_PACKED_START(value);
        uint16_t count = PACKED_COUNT_FROM_VALUE(value);
        for (uint16_t i = 0; i < count; i++) {
            uint16_t pos = varintPacked13Get(packed, i);
            bitmap[pos / 8] |= (1 << (pos % 8));
        }
        break;
    }
    case CHUNK_TYPE_FULL_BITMAP:
        memcpy(bitmap, GET_CHUNK_BITMAP_START(value), BITMAP_SIZE_IN_BYTES);
        break;
    case CHUNK_TYPE_OVER_FULL_DIRECT_NOT_SET_POSITION_NUMBERS: {
        memset(bitmap, 0xFF, BITMAP_SIZE_IN_BYTES);
        const uint8_t *packed = GET_CHUNK_PACKED_START(value);
        uint16_t count = PACKED_COUNT_FROM_VALUE(value);
        for (uint16_t i = 0; i < count; i++) {
            uint16_t pos = varintPacked13Get(packed, i);
            bitmap[pos / 8] &= ~(1 << (pos % 8));
        }
        break;
    }
    default:
        memset(bitmap, 0, BITMAP_SIZE_IN_BYTES);
        break;
    }
}

/* Helper: Compress bitmap back to optimal representation */
DK_STATIC void compressBitmapToChunk(multiroar *r, const databox *key,
                                     uint8_t *bitmap, multimapEntry *me,
                                     bool chunkExists) {
    (void)me; /* No longer used - we use delete+insert pattern */
    uint32_t popcount = StrPopCntExact(bitmap, BITMAP_SIZE_IN_BYTES);

    /* Delete existing chunk first if it exists (simpler than replace) */
    if (chunkExists) {
        multimapDelete(&r->map, key);
    }

    if (popcount == 0) {
        /* All zeros - no chunk needed (already deleted above) */
        return;
    } else if (popcount == BITMAP_SIZE_IN_BITS) {
        /* All ones */
        uint8_t allOnes[1] = {CHUNK_TYPE_ALL_1};
        const databox box = {
            .data.bytes.start = allOnes, .len = 1, .type = DATABOX_BYTES};
        const databox *inserting[] = {key, &box};
        multimapInsert(&r->map, inserting);
    } else if (popcount < MAX_ENTRIES_PER_DIRECT_LISTING) {
        /* Sparse - convert to packed position list.
         * Layout: [type:1] [count:1-2] [positions:N]
         * We need to write positions first to a temp location, then move them
         * to the correct offset based on count's varint length. */
        uint8_t packed[BITMAP_SIZE_IN_BYTES + 16] = {0};
        packed[0] = CHUNK_TYPE_UNDER_FULL_DIRECT_POSITION_NUMBERS;

        /* Write positions to temporary location at offset 1 */
        uint16_t count = bitmapToSetPositions(bitmap, packed + 1);
        const uint64_t positionsBytes = divCeil(count * 13, 8);

        /* Determine varint length for count and move positions to correct spot
         */
        varintWidth countWidth = varintTaggedLen(count);
        memmove(packed + 1 + countWidth, packed + 1, positionsBytes);

        /* Now write the count at offset 1 */
        varintTaggedPut64(packed + 1, count);

        uint64_t packedLen = 1 + countWidth + positionsBytes;

        const databox box = {.data.bytes.start = packed,
                             .len = packedLen,
                             .type = DATABOX_BYTES};
        const databox *inserting[] = {key, &box};
        multimapInsert(&r->map, inserting);
    } else if (popcount > MAX_BITMAP_ENTIRES_BEFORE_NEGATIVE_LISTING) {
        /* Nearly full - convert to negative position list.
         * Same layout as sparse, but stores unset positions. */
        uint8_t packed[BITMAP_SIZE_IN_BYTES + 16] = {0};
        packed[0] = CHUNK_TYPE_OVER_FULL_DIRECT_NOT_SET_POSITION_NUMBERS;

        /* Write positions to temporary location at offset 1 */
        uint16_t count = bitmapToNegativePositions(bitmap, packed + 1);
        const uint64_t positionsBytes = divCeil(count * 13, 8);

        /* Determine varint length for count and move positions to correct spot
         */
        varintWidth countWidth = varintTaggedLen(count);
        memmove(packed + 1 + countWidth, packed + 1, positionsBytes);

        /* Now write the count at offset 1 */
        varintTaggedPut64(packed + 1, count);

        uint64_t packedLen = 1 + countWidth + positionsBytes;

        const databox box = {.data.bytes.start = packed,
                             .len = packedLen,
                             .type = DATABOX_BYTES};
        const databox *inserting[] = {key, &box};
        multimapInsert(&r->map, inserting);
    } else {
        /* Medium density - keep as bitmap */
        uint8_t bitmapWithType[BITMAP_SIZE_IN_BYTES + 1];
        bitmapWithType[0] = CHUNK_TYPE_FULL_BITMAP;
        memcpy(bitmapWithType + 1, bitmap, BITMAP_SIZE_IN_BYTES);

        const databox box = {.data.bytes.start = bitmapWithType,
                             .len = sizeof(bitmapWithType),
                             .type = DATABOX_BYTES};
        const databox *inserting[] = {key, &box};
        multimapInsert(&r->map, inserting);
    }
}

/* OR: r = r OR b */
void multiroarOr(multiroar *r, multiroar *b) {
    if (!r || !b) {
        return;
    }

    uint8_t bitmapR[BITMAP_SIZE_IN_BYTES];
    uint8_t bitmapB[BITMAP_SIZE_IN_BYTES];

    /* First, collect all keys from b */
    uint64_t numChunks = multimapCount(b->map);
    if (numChunks == 0) {
        return;
    }

    /* Collect just keys from b via iteration */
    uint64_t *chunkKeys = zmalloc(numChunks * sizeof(uint64_t));
    uint64_t keyCount = 0;

    multimapIterator iter;
    multimapIteratorInit(b->map, &iter, true);

    databox keyStorage[2];
    databox *keyPtr[2] = {&keyStorage[0], &keyStorage[1]};
    while (multimapIteratorNext(&iter, keyPtr)) {
        chunkKeys[keyCount++] = keyStorage[0].data.u;
    }

    /* Process each chunk from b - use multimapLookup to get values */
    for (uint64_t i = 0; i < keyCount; i++) {
        databox key = {.data.u = chunkKeys[i], .type = DATABOX_UNSIGNED_64};

        /* Get value from b using multimapLookup */
        databox bValue;
        databox *bValues[] = {&bValue};
        if (!multimapLookup(b->map, &key, bValues)) {
            continue; /* shouldn't happen */
        }
        expandChunkToBitmap(&bValue, bitmapB);

        /* Look up corresponding chunk in r */
        databox rValue;
        databox *rValues[] = {&rValue};
        bool rHasChunk = multimapLookup(r->map, &key, rValues);

        if (rHasChunk) {
            expandChunkToBitmap(&rValue, bitmapR);
            bitmap_or_simd(bitmapR, bitmapB, BITMAP_SIZE_IN_BYTES);
        } else {
            memcpy(bitmapR, bitmapB, BITMAP_SIZE_IN_BYTES);
        }

        /* Store result */
        multimapEntry me;
        compressBitmapToChunk(r, &key, bitmapR, &me, rHasChunk);
    }

    zfree(chunkKeys);
}

/* AND: r = r AND b */
void multiroarAnd(multiroar *r, multiroar *b) {
    if (!r || !b) {
        return;
    }

    /* For AND, chunks only in r (not in b) become zeros.
     * We need to collect keys to delete after iteration. */
    uint8_t bitmapR[BITMAP_SIZE_IN_BYTES];
    uint8_t bitmapB[BITMAP_SIZE_IN_BYTES];

    /* Collect chunk keys from r that need processing */
    uint64_t numChunks = multimapCount(r->map);
    if (numChunks == 0) {
        return;
    }

    uint64_t *chunkKeys = zmalloc(numChunks * sizeof(uint64_t));
    uint64_t keyCount = 0;

    multimapIterator iter;
    multimapIteratorInit(r->map, &iter, true);

    databox keyStorage[2];
    databox *keyPtr[2] = {&keyStorage[0], &keyStorage[1]};
    while (multimapIteratorNext(&iter, keyPtr)) {
        chunkKeys[keyCount++] = keyStorage[0].data.u;
    }

    /* Process each chunk */
    for (uint64_t i = 0; i < keyCount; i++) {
        databox key = {.data.u = chunkKeys[i], .type = DATABOX_UNSIGNED_64};
        multimapEntry me;

        if (!multimapGetUnderlyingEntry(r->map, &key, &me)) {
            continue;
        }

        databox rValue;
        flexGetNextByType(*me.map, &me.fe, &rValue);
        expandChunkToBitmap(&rValue, bitmapR);

        /* Look up corresponding chunk in b */
        databox bValue;
        databox *bValues[] = {&bValue};

        if (multimapLookup(b->map, &key, bValues)) {
            /* Both have this chunk - AND them */
            expandChunkToBitmap(&bValue, bitmapB);
            bitmap_and_simd(bitmapR, bitmapB, BITMAP_SIZE_IN_BYTES);
        } else {
            /* b doesn't have this chunk - result is all zeros */
            memset(bitmapR, 0, BITMAP_SIZE_IN_BYTES);
        }

        /* Re-get entry (may have moved) and update */
        if (multimapGetUnderlyingEntry(r->map, &key, &me)) {
            compressBitmapToChunk(r, &key, bitmapR, &me, true);
        }
    }

    zfree(chunkKeys);
}

/* XOR: r = r XOR b */
void multiroarXor(multiroar *r, multiroar *b) {
    if (!r || !b) {
        return;
    }

    uint8_t bitmapR[BITMAP_SIZE_IN_BYTES];
    uint8_t bitmapB[BITMAP_SIZE_IN_BYTES];

    /* First, collect all keys from b */
    uint64_t numChunks = multimapCount(b->map);
    if (numChunks == 0) {
        return;
    }

    /* Store both keys and bitmaps from b */
    typedef struct {
        uint64_t key;
        uint8_t bitmap[BITMAP_SIZE_IN_BYTES];
    } ChunkBitmap;
    ChunkBitmap *chunks = zmalloc(numChunks * sizeof(ChunkBitmap));
    uint64_t chunkCount = 0;

    multimapIterator iter;
    multimapIteratorInit(b->map, &iter, true);

    databox keyStorage[2];
    databox *keyPtr[2] = {&keyStorage[0], &keyStorage[1]};
    while (multimapIteratorNext(&iter, keyPtr)) {
        chunks[chunkCount].key = keyStorage[0].data.u;
        expandChunkToBitmap(&keyStorage[1], chunks[chunkCount].bitmap);
        chunkCount++;
    }

    /* Process each chunk from b */
    for (uint64_t i = 0; i < chunkCount; i++) {
        databox key = {.data.u = chunks[i].key, .type = DATABOX_UNSIGNED_64};
        memcpy(bitmapB, chunks[i].bitmap, BITMAP_SIZE_IN_BYTES);

        /* Look up corresponding chunk in r */
        multimapEntry me;
        bool rHasChunk = multimapGetUnderlyingEntry(r->map, &key, &me);

        if (rHasChunk) {
            databox rValue;
            flexGetNextByType(*me.map, &me.fe, &rValue);
            expandChunkToBitmap(&rValue, bitmapR);
            bitmap_xor_simd(bitmapR, bitmapB, BITMAP_SIZE_IN_BYTES);
            compressBitmapToChunk(r, &key, bitmapR, &me, true);
        } else {
            /* r doesn't have this chunk - XOR with 0 = copy */
            compressBitmapToChunk(r, &key, bitmapB, &me, false);
        }
    }

    zfree(chunks);
}

/* NOT: r = NOT r (within existing chunks only) */
void multiroarNot(multiroar *r) {
    if (!r) {
        return;
    }

    uint8_t bitmap[BITMAP_SIZE_IN_BYTES];

    /* Collect chunk keys first to avoid iterator invalidation */
    uint64_t numChunks = multimapCount(r->map);
    if (numChunks == 0) {
        return;
    }

    uint64_t *chunkKeys = zmalloc(numChunks * sizeof(uint64_t));
    uint64_t keyCount = 0;

    multimapIterator iter;
    multimapIteratorInit(r->map, &iter, true);

    databox keyStorage[2];
    databox *keyPtr[2] = {&keyStorage[0], &keyStorage[1]};
    while (multimapIteratorNext(&iter, keyPtr)) {
        chunkKeys[keyCount++] = keyStorage[0].data.u;
    }

    /* Process each chunk */
    for (uint64_t i = 0; i < keyCount; i++) {
        databox key = {.data.u = chunkKeys[i], .type = DATABOX_UNSIGNED_64};

        /* Look up the value in r */
        databox rValue;
        databox *rValues[] = {&rValue};
        if (!multimapLookup(r->map, &key, rValues)) {
            continue;
        }

        expandChunkToBitmap(&rValue, bitmap);
        bitmap_not_simd(bitmap, BITMAP_SIZE_IN_BYTES);

        /* Re-get entry and update */
        multimapEntry me;
        if (multimapGetUnderlyingEntry(r->map, &key, &me)) {
            compressBitmapToChunk(r, &key, bitmap, &me, true);
        }
    }

    zfree(chunkKeys);
}

/* Create new roars from operations */
multiroar *multiroarNewAnd(const multiroar *a, const multiroar *b) {
    multiroar *result = multiroarDuplicate(a);
    multiroarAnd(result, (multiroar *)b);
    return result;
}

multiroar *multiroarNewOr(const multiroar *a, const multiroar *b) {
    multiroar *result = multiroarDuplicate(a);
    multiroarOr(result, (multiroar *)b);
    return result;
}

multiroar *multiroarNewXor(const multiroar *a, const multiroar *b) {
    multiroar *result = multiroarDuplicate(a);
    multiroarXor(result, (multiroar *)b);
    return result;
}

multiroar *multiroarNewNot(const multiroar *r) {
    multiroar *result = multiroarDuplicate(r);
    multiroarNot(result);
    return result;
}

/* ====================================================================
 * N-way Set Operations (for N >= 2 inputs)
 * ==================================================================== */

/* Helper: Collect all unique chunk IDs across N multiroars */
DK_STATIC uint64_t *collectAllChunkKeys(uint64_t n, multiroar **roars,
                                        uint64_t *outCount) {
    if (n == 0 || !roars) {
        *outCount = 0;
        return NULL;
    }

    /* Estimate total number of chunks across all inputs */
    uint64_t totalChunks = 0;
    for (uint64_t i = 0; i < n; i++) {
        if (roars[i] && roars[i]->map) {
            totalChunks += multimapCount(roars[i]->map);
        }
    }

    if (totalChunks == 0) {
        *outCount = 0;
        return NULL;
    }

    /* Collect all chunk keys */
    uint64_t *allKeys = zmalloc(totalChunks * sizeof(uint64_t));
    uint64_t keyCount = 0;

    for (uint64_t i = 0; i < n; i++) {
        if (!roars[i] || !roars[i]->map) {
            continue;
        }

        multimapIterator iter;
        multimapIteratorInit(roars[i]->map, &iter, true);

        databox keyValue[2];
        databox *kvPtr[2] = {&keyValue[0], &keyValue[1]};

        while (multimapIteratorNext(&iter, kvPtr)) {
            uint64_t chunkId = keyValue[0].data.u;

            /* Check if we already have this key */
            bool found = false;
            for (uint64_t j = 0; j < keyCount; j++) {
                if (allKeys[j] == chunkId) {
                    found = true;
                    break;
                }
            }

            if (!found) {
                allKeys[keyCount++] = chunkId;
            }
        }
    }

    *outCount = keyCount;
    return allKeys;
}

/* N-way AND: Modifies first roar in array */
void multiroarAndN(uint64_t n, multiroar **roars) {
    if (n < 2 || !roars || !roars[0]) {
        return;
    }

    multiroar *result = roars[0];

    /* Collect all chunk IDs that exist in ALL inputs */
    uint64_t allKeyCount;
    uint64_t *allKeys = collectAllChunkKeys(n, roars, &allKeyCount);

    if (!allKeys) {
        return;
    }

    uint8_t *bitmaps = zmalloc(n * BITMAP_SIZE_IN_BYTES);
    uint8_t resultBitmap[BITMAP_SIZE_IN_BYTES];

    /* Process each unique chunk */
    for (uint64_t i = 0; i < allKeyCount; i++) {
        databox key = {.data.u = allKeys[i], .type = DATABOX_UNSIGNED_64};

        /* Check if ALL roars have this chunk */
        bool allHaveChunk = true;
        for (uint64_t j = 0; j < n; j++) {
            if (!roars[j] || !roars[j]->map) {
                allHaveChunk = false;
                break;
            }

            databox value;
            databox *values[] = {&value};
            if (!multimapLookup(roars[j]->map, &key, values)) {
                allHaveChunk = false;
                break;
            }
        }

        if (!allHaveChunk) {
            /* AND with missing chunk (all zeros) = all zeros
             * So delete this chunk from result if it exists */
            multimapDelete(&result->map, &key);
            continue;
        }

        /* Expand all chunks to bitmaps */
        for (uint64_t j = 0; j < n; j++) {
            databox value;
            databox *values[] = {&value};
            multimapLookup(roars[j]->map, &key, values);
            expandChunkToBitmap(&value, bitmaps + j * BITMAP_SIZE_IN_BYTES);
        }

        /* Initialize result with first bitmap */
        memcpy(resultBitmap, bitmaps, BITMAP_SIZE_IN_BYTES);

        /* AND with all other bitmaps */
        for (uint64_t j = 1; j < n; j++) {
            bitmap_and_simd(resultBitmap, bitmaps + j * BITMAP_SIZE_IN_BYTES,
                            BITMAP_SIZE_IN_BYTES);
        }

        /* Store result */
        multimapEntry me;
        bool resultHasChunk =
            multimapGetUnderlyingEntry(result->map, &key, &me);
        compressBitmapToChunk(result, &key, resultBitmap, &me, resultHasChunk);
    }

    zfree(bitmaps);
    zfree(allKeys);
}

/* N-way OR: Modifies first roar in array */
void multiroarOrN(uint64_t n, multiroar **roars) {
    if (n < 2 || !roars || !roars[0]) {
        return;
    }

    multiroar *result = roars[0];

    /* Collect all chunk IDs across all inputs */
    uint64_t allKeyCount;
    uint64_t *allKeys = collectAllChunkKeys(n, roars, &allKeyCount);

    if (!allKeys) {
        return;
    }

    uint8_t *bitmaps = zmalloc(n * BITMAP_SIZE_IN_BYTES);
    uint8_t resultBitmap[BITMAP_SIZE_IN_BYTES];

    /* Process each unique chunk */
    for (uint64_t i = 0; i < allKeyCount; i++) {
        databox key = {.data.u = allKeys[i], .type = DATABOX_UNSIGNED_64};

        /* Expand all chunks to bitmaps (use zeros for missing chunks) */
        bool anyHasChunk = false;
        for (uint64_t j = 0; j < n; j++) {
            databox value;
            databox *values[] = {&value};

            if (roars[j] && roars[j]->map &&
                multimapLookup(roars[j]->map, &key, values)) {
                expandChunkToBitmap(&value, bitmaps + j * BITMAP_SIZE_IN_BYTES);
                anyHasChunk = true;
            } else {
                /* Missing chunk = all zeros */
                memset(bitmaps + j * BITMAP_SIZE_IN_BYTES, 0,
                       BITMAP_SIZE_IN_BYTES);
            }
        }

        if (!anyHasChunk) {
            /* All chunks are zeros - skip */
            continue;
        }

        /* Initialize result with first bitmap */
        memcpy(resultBitmap, bitmaps, BITMAP_SIZE_IN_BYTES);

        /* OR with all other bitmaps */
        for (uint64_t j = 1; j < n; j++) {
            bitmap_or_simd(resultBitmap, bitmaps + j * BITMAP_SIZE_IN_BYTES,
                           BITMAP_SIZE_IN_BYTES);
        }

        /* Store result */
        multimapEntry me;
        bool resultHasChunk =
            multimapGetUnderlyingEntry(result->map, &key, &me);
        compressBitmapToChunk(result, &key, resultBitmap, &me, resultHasChunk);
    }

    zfree(bitmaps);
    zfree(allKeys);
}

/* N-way XOR: Modifies first roar in array */
void multiroarXorN(uint64_t n, multiroar **roars) {
    if (n < 2 || !roars || !roars[0]) {
        return;
    }

    multiroar *result = roars[0];

    /* Collect all chunk IDs across all inputs */
    uint64_t allKeyCount;
    uint64_t *allKeys = collectAllChunkKeys(n, roars, &allKeyCount);

    if (!allKeys) {
        return;
    }

    uint8_t *bitmaps = zmalloc(n * BITMAP_SIZE_IN_BYTES);
    uint8_t resultBitmap[BITMAP_SIZE_IN_BYTES];

    /* Process each unique chunk */
    for (uint64_t i = 0; i < allKeyCount; i++) {
        databox key = {.data.u = allKeys[i], .type = DATABOX_UNSIGNED_64};

        /* Expand all chunks to bitmaps (use zeros for missing chunks) */
        for (uint64_t j = 0; j < n; j++) {
            databox value;
            databox *values[] = {&value};

            if (roars[j] && roars[j]->map &&
                multimapLookup(roars[j]->map, &key, values)) {
                expandChunkToBitmap(&value, bitmaps + j * BITMAP_SIZE_IN_BYTES);
            } else {
                /* Missing chunk = all zeros */
                memset(bitmaps + j * BITMAP_SIZE_IN_BYTES, 0,
                       BITMAP_SIZE_IN_BYTES);
            }
        }

        /* Initialize result with first bitmap */
        memcpy(resultBitmap, bitmaps, BITMAP_SIZE_IN_BYTES);

        /* XOR with all other bitmaps */
        for (uint64_t j = 1; j < n; j++) {
            bitmap_xor_simd(resultBitmap, bitmaps + j * BITMAP_SIZE_IN_BYTES,
                            BITMAP_SIZE_IN_BYTES);
        }

        /* Store result */
        multimapEntry me;
        bool resultHasChunk =
            multimapGetUnderlyingEntry(result->map, &key, &me);
        compressBitmapToChunk(result, &key, resultBitmap, &me, resultHasChunk);
    }

    zfree(bitmaps);
    zfree(allKeys);
}

/* N-way operations returning new multiroar */
multiroar *multiroarNewAndN(uint64_t n, multiroar **roars) {
    if (n == 0 || !roars || !roars[0]) {
        return multiroarBitNew();
    }

    multiroar *result = multiroarDuplicate(roars[0]);

    if (n > 1) {
        /* Create temp array starting from result */
        multiroar **tempRoars = zmalloc(n * sizeof(multiroar *));
        tempRoars[0] = result;
        for (uint64_t i = 1; i < n; i++) {
            tempRoars[i] = roars[i];
        }

        multiroarAndN(n, tempRoars);
        zfree(tempRoars);
    }

    return result;
}

multiroar *multiroarNewOrN(uint64_t n, multiroar **roars) {
    if (n == 0 || !roars || !roars[0]) {
        return multiroarBitNew();
    }

    multiroar *result = multiroarDuplicate(roars[0]);

    if (n > 1) {
        /* Create temp array starting from result */
        multiroar **tempRoars = zmalloc(n * sizeof(multiroar *));
        tempRoars[0] = result;
        for (uint64_t i = 1; i < n; i++) {
            tempRoars[i] = roars[i];
        }

        multiroarOrN(n, tempRoars);
        zfree(tempRoars);
    }

    return result;
}

multiroar *multiroarNewXorN(uint64_t n, multiroar **roars) {
    if (n == 0 || !roars || !roars[0]) {
        return multiroarBitNew();
    }

    multiroar *result = multiroarDuplicate(roars[0]);

    if (n > 1) {
        /* Create temp array starting from result */
        multiroar **tempRoars = zmalloc(n * sizeof(multiroar *));
        tempRoars[0] = result;
        for (uint64_t i = 1; i < n; i++) {
            tempRoars[i] = roars[i];
        }

        multiroarXorN(n, tempRoars);
        zfree(tempRoars);
    }

    return result;
}

/* ====================================================================
 * Testing
 * ==================================================================== */

#ifdef DATAKIT_TEST

#include "ctest.h"
#include "timeUtil.h"

void multiroarRepr(multiroar *r, uint64_t highest) {
    for (uint64_t i = 0; i < divCeil(highest, BITMAP_SIZE_IN_BITS); i++) {
        databox value;
        databox *values[1] = {&value};
        if (multimapLookup(r->map,
                           &(databox){.data.u = i, .type = DATABOX_UNSIGNED_64},
                           values)) {
            printf("[%" PRIu64 "] = %d (%" PRIu64 " byte%s)\n", i,
                   GET_CHUNK_TYPE(&value), value.len,
                   value.len == 1 ? "" : "s");
        }
    }
}

void multiroarTestCompare(multiroar *r, uint64_t highest) {
    const uint64_t b = multimapBytes(r->map);
    printf("Final size: %" PRIu64 " bytes; Highest bit set: %" PRIu64
           "; Size if linear: %" PRIu64 " "
           "bytes; Savings: %.2fx\n",
           b, highest, highest / 8, ((float)(highest / 8) / b) - 1);

    const uint64_t maps = multimapCount(r->map);
    uint64_t asPositional = b;
    uint64_t minusMaps = maps;
    if (maps <= 255) {
        asPositional -= (maps * 3);
        minusMaps -= maps;
    } else {
        asPositional -= (255 * 3);
        minusMaps -= 255;

        /* rough estimate of remaining index data */
        asPositional -= (minusMaps * 4);
    }

    /* add positional lookup bitmap overhead */
    asPositional += divCeil(divCeil(highest, BITMAP_SIZE_IN_BITS), 8);

    printf("With positional encoding, it would use %" PRIu64
           " bytes and be a savings "
           "of %0.2f%%\n",
           asPositional, (((double)b / asPositional) - 1) * 100);
}

int multiroarTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;

    TEST("create") {
        multiroar *r = multiroarBitNew();
        multiroarFree(r);
    }

    TEST("set and get") {
        multiroar *r = multiroarBitNew();
        bool previouslySet = multiroarBitSet(r, 1700);

        if (previouslySet) {
            ERRR("Detected previously set on new assignment!");
        }

        bool currentlySet = multiroarBitGet(r, 1700);

        if (!currentlySet) {
            ERRR("Didn't find set bit after setting the bit!");
        }

        multiroarFree(r);
    }

    const int32_t lots = 72000;

#if 1
    TEST_DESC("set and get (%d of random positions; individual roars)", lots) {
        for (int32_t i = 0; i < lots; i++) {
            const uint64_t position = (uint64_t)rand() % SIZE_MAX;

            multiroar *r = multiroarBitNew();

            const bool previouslySet = multiroarBitSet(r, position);

            if (previouslySet) {
                ERR("Detected previously set on new assignment at position "
                    "%" PRIu64 "!",
                    position);
            }

            const bool currentlySet = multiroarBitGet(r, position);

            if (!currentlySet) {
                ERR("Didn't find set bit after setting the bit at position "
                    "%" PRIu64 "!",
                    position);
            }

            multiroarFree(r);
        }
    }

    TEST_DESC("set and get (%d of (SMALL) random positions; common roar)",
              lots) {
        multiroar *r = multiroarBitNew();
        uint64_t highest = 0;
        for (int32_t i = 0; i < lots; i++) {
            const uint64_t position = ((uint64_t)rand() % UINT16_MAX);

            if (position > highest) {
                highest = position;
            }

            multiroarBitSet(r, position);
            const bool currentlySet = multiroarBitGet(r, position);

            if (!currentlySet) {
                ERR("Didn't find set bit after setting the bit at position "
                    "%" PRIu64 "!",
                    position);
            }
        }
        multiroarRepr(r, highest);
        multiroarTestCompare(r, highest);
        multiroarFree(r);
    }

    TEST_DESC("set and get (%d of (MEDIUM) random positions; common roar)",
              lots) {
        multiroar *r = multiroarBitNew();
        uint64_t highest = 0;
        for (int32_t i = 0; i < lots; i++) {
            const uint64_t position = ((uint64_t)rand());

            if (position > highest) {
                highest = position;
            }

            multiroarBitSet(r, position);
            const bool currentlySet = multiroarBitGet(r, position);

            if (!currentlySet) {
                ERR("Didn't find set bit after setting the bit at position "
                    "%" PRIu64 "!",
                    position);
            }
        }
        multiroarTestCompare(r, highest);
        multiroarFree(r);
    }

    TEST_DESC("set and get (%d of (BIG) random positions; common roar)", lots) {
        multiroar *r = multiroarBitNew();
        uint64_t highest = 0;
        for (int32_t i = 0; i < lots; i++) {
            const uint64_t position = ((uint64_t)rand() * rand()) % SIZE_MAX;

            if (position > highest) {
                highest = position;
            }

            multiroarBitSet(r, position);
            const bool currentlySet = multiroarBitGet(r, position);

            if (!currentlySet) {
                ERR("Didn't find set bit after setting the bit at position "
                    "%" PRIu64 "!",
                    position);
            }
        }
        multiroarTestCompare(r, highest);
        multiroarFree(r);
    }
#endif

    TEST_DESC("set and get (%d of sequential positions; common roar)", lots) {
        multiroar *r = multiroarBitNew();
        for (int32_t i = 0; i < lots; i++) {
            const bool previouslySet = multiroarBitSet(r, i);
            if (previouslySet) {
                ERR("Detected previously set on new assignment at position %d!",
                    i);
            }

            const bool currentlySet = multiroarBitGet(r, i);

            if (!currentlySet) {
                ERR("Didn't find set bit after setting the bit at position %d!",
                    i);
            }
        }
        multiroarRepr(r, lots - 1);
        multiroarTestCompare(r, lots - 1);
        multiroarFree(r);
    }

    TEST("chunk boundary correctness") {
        /* Test bits at and around chunk boundaries (8192 bits per chunk) */
        multiroar *r = multiroarBitNew();
        const uint64_t boundaries[] = {0,     8191,  8192,  8193,  16383,
                                       16384, 16385, 24575, 24576, 24577};
        const uint64_t numBoundaries =
            sizeof(boundaries) / sizeof(boundaries[0]);

        /* Set all boundary bits */
        for (uint64_t i = 0; i < numBoundaries; i++) {
            multiroarBitSet(r, boundaries[i]);
        }

        /* Verify all boundary bits are set */
        for (uint64_t i = 0; i < numBoundaries; i++) {
            if (!multiroarBitGet(r, boundaries[i])) {
                ERR("Boundary bit %" PRIu64 " not set!", boundaries[i]);
            }
        }

        /* Verify bits between boundaries are NOT set */
        if (multiroarBitGet(r, 100)) {
            ERRR("Bit 100 should not be set!");
        }
        if (multiroarBitGet(r, 8100)) {
            ERRR("Bit 8100 should not be set!");
        }
        if (multiroarBitGet(r, 16000)) {
            ERRR("Bit 16000 should not be set!");
        }

        multiroarFree(r);
    }

    TEST("sequential set up to bitmap conversion threshold") {
        /* Test setting many bits sequentially triggers bitmap conversion
         * and continues to work correctly */
        multiroar *r = multiroarBitNew();
        const uint64_t testCount =
            1000; /* Well above 629 conversion threshold */

        for (uint64_t i = 0; i < testCount; i++) {
            multiroarBitSet(r, i);
            if (!multiroarBitGet(r, i)) {
                ERR("Bit %" PRIu64 " NOT SET immediately after setting!", i);
            }
        }

        /* Verify all bits are still set */
        for (uint64_t i = 0; i < testCount; i++) {
            if (!multiroarBitGet(r, i)) {
                ERR("Bit %" PRIu64 " lost after all sets!", i);
            }
        }

        multiroarFree(r);
    }

    TEST("conversion from UNDER_FULL to FULL_BITMAP") {
        /* Test that conversion to full bitmap preserves all bits */
        multiroar *r = multiroarBitNew();
        const uint64_t maxDirect = 629; /* MAX_ENTRIES_PER_DIRECT_LISTING */

        /* Set exactly maxDirect positions to trigger conversion to bitmap */
        uint64_t *positions = zcalloc(maxDirect, sizeof(uint64_t));
        for (uint64_t i = 0; i < maxDirect; i++) {
            positions[i] = i * 10; /* Spread positions to stay in one chunk */
            multiroarBitSet(r, positions[i]);
        }

        /* Verify all positions are still set after potential conversion */
        for (uint64_t i = 0; i < maxDirect; i++) {
            if (!multiroarBitGet(r, positions[i])) {
                ERR("Position %" PRIu64 " lost after bitmap conversion!",
                    positions[i]);
            }
        }

        /* Verify non-set positions are not set */
        for (uint64_t i = 0; i < maxDirect - 1; i++) {
            for (uint64_t j = positions[i] + 1; j < positions[i + 1]; j++) {
                if (multiroarBitGet(r, j)) {
                    ERR("Position %" PRIu64 " incorrectly set!", j);
                }
            }
        }

        zfree(positions);
        multiroarFree(r);
    }

    TEST("bitmap mode with various positions") {
        /* Test that bitmap mode (> 629 positions) works correctly */
        multiroar *r = multiroarBitNew();

        /* Set positions to trigger bitmap mode, then verify */
        const uint64_t positions[] = {0,    100,  200,  300,  400,  500,
                                      600,  700,  800,  900,  1000, 1500,
                                      2000, 2500, 3000, 3500, 4000, 4500,
                                      5000, 5500, 6000, 6500, 7000};
        const uint64_t numPositions = sizeof(positions) / sizeof(positions[0]);

        /* First, fill up to trigger bitmap conversion */
        for (uint64_t i = 0; i < 700; i++) {
            multiroarBitSet(r, i);
        }

        /* Set some additional positions */
        for (uint64_t i = 0; i < numPositions; i++) {
            multiroarBitSet(r, positions[i]);
        }

        /* Verify the specific positions are set */
        for (uint64_t i = 0; i < numPositions; i++) {
            if (!multiroarBitGet(r, positions[i])) {
                ERR("Position %" PRIu64 " not set!", positions[i]);
            }
        }

        /* Verify positions outside our set are not set */
        if (multiroarBitGet(r, 7500)) {
            ERRR("Position 7500 should not be set!");
        }

        multiroarFree(r);
    }

    TEST("multi-chunk correctness") {
        /* Create roar with multiple chunks */
        multiroar *r = multiroarBitNew();
        const uint64_t chunkBits = 8192;

        /* Chunk 0: sparse (few bits) */
        multiroarBitSet(r, 100);
        multiroarBitSet(r, 200);
        multiroarBitSet(r, 300);

        /* Chunk 1: moderately filled */
        for (uint64_t i = chunkBits; i < chunkBits + 500; i++) {
            multiroarBitSet(r, i);
        }

        /* Chunk 2: also moderately filled */
        for (uint64_t i = chunkBits * 2; i < chunkBits * 2 + 1000; i++) {
            multiroarBitSet(r, i);
        }

        /* Verify chunk 0 (sparse) */
        if (!multiroarBitGet(r, 100) || !multiroarBitGet(r, 200) ||
            !multiroarBitGet(r, 300)) {
            ERRR("Sparse chunk bits not set!");
        }
        if (multiroarBitGet(r, 150)) {
            ERRR("Unset bit in sparse chunk incorrectly set!");
        }

        /* Verify chunk 1 */
        for (uint64_t i = chunkBits; i < chunkBits + 500; i++) {
            if (!multiroarBitGet(r, i)) {
                ERR("Bit %" PRIu64 " in chunk 1 should be set!", i);
            }
        }

        /* Verify chunk 2 */
        for (uint64_t i = chunkBits * 2; i < chunkBits * 2 + 1000; i++) {
            if (!multiroarBitGet(r, i)) {
                ERR("Bit %" PRIu64 " in chunk 2 should be set!", i);
            }
        }
        if (multiroarBitGet(r, chunkBits * 2 + 1500)) {
            ERRR("Unset bit in chunk 2 incorrectly set!");
        }

        multiroarFree(r);
    }

    TEST("high-scale stress test (100K random bits)") {
        multiroar *r = multiroarBitNew();
        const uint64_t scale = 100000;
        const uint64_t sampleSize = 10000; /* Track a sample for verification */
        uint64_t *sampleBits = zcalloc(sampleSize, sizeof(uint64_t));

        /* Set random bits, tracking a sample for verification */
        for (uint64_t i = 0; i < scale; i++) {
            const uint64_t position =
                ((uint64_t)rand() * rand()) % (SIZE_MAX / 2);

            /* Track every Nth position for verification */
            if (i < sampleSize) {
                sampleBits[i] = position;
            }

            multiroarBitSet(r, position);

            /* Verify immediately after set */
            if (!multiroarBitGet(r, position)) {
                ERR("High-scale: bit %" PRIu64
                    " not set immediately after setting!",
                    position);
            }
        }

        /* Verify sample bits are still set after all operations */
        uint64_t verified = 0;
        for (uint64_t i = 0; i < sampleSize; i++) {
            if (multiroarBitGet(r, sampleBits[i])) {
                verified++;
            } else {
                ERR("High-scale: sample bit %" PRIu64 " not set!",
                    sampleBits[i]);
            }
        }

        if (verified != sampleSize) {
            ERR("High-scale: only %" PRIu64 " of %" PRIu64
                " sample bits verified!",
                verified, sampleSize);
        }

        zfree(sampleBits);
        multiroarFree(r);
    }

    TEST("set same bit twice returns correct previouslySet") {
        multiroar *r = multiroarBitNew();

        bool first = multiroarBitSet(r, 12345);
        if (first) {
            ERRR("First set should return false for previouslySet!");
        }

        bool second = multiroarBitSet(r, 12345);
        if (!second) {
            ERRR("Second set should return true for previouslySet!");
        }

        /* Verify bit is still set */
        if (!multiroarBitGet(r, 12345)) {
            ERRR("Bit should remain set after double set!");
        }

        multiroarFree(r);
    }

    TEST("PERF: sequential insert performance") {
        const uint64_t scale = 100000;
        multiroar *r = multiroarBitNew();

        int64_t startNs = timeUtilMonotonicNs();
        for (uint64_t i = 0; i < scale; i++) {
            multiroarBitSet(r, i);
        }
        int64_t elapsed = timeUtilMonotonicNs() - startNs;
        printf("Sequential insert: %.1f ns/op, %.0f ops/sec\n",
               (double)elapsed / scale, scale / (elapsed / 1e9));

        multiroarFree(r);
    }

    TEST("PERF: random insert performance") {
        const uint64_t scale = 100000;
        multiroar *r = multiroarBitNew();

        int64_t startNs = timeUtilMonotonicNs();
        for (uint64_t i = 0; i < scale; i++) {
            multiroarBitSet(r, ((uint64_t)rand() * rand()) % (SIZE_MAX / 2));
        }
        int64_t elapsed = timeUtilMonotonicNs() - startNs;
        printf("Random insert: %.1f ns/op, %.0f ops/sec\n",
               (double)elapsed / scale, scale / (elapsed / 1e9));

        multiroarFree(r);
    }

    TEST("PERF: lookup performance (dense)") {
        const uint64_t scale = 100000;
        multiroar *r = multiroarBitNew();

        /* Create dense bitmap */
        for (uint64_t i = 0; i < scale; i++) {
            multiroarBitSet(r, i);
        }

        int64_t startNs = timeUtilMonotonicNs();
        for (uint64_t i = 0; i < scale; i++) {
            multiroarBitGet(r, i);
        }
        int64_t elapsed = timeUtilMonotonicNs() - startNs;
        printf("Dense lookup: %.1f ns/op, %.0f ops/sec\n",
               (double)elapsed / scale, scale / (elapsed / 1e9));

        multiroarFree(r);
    }

    /* ================================================================
     * Edge Case and Boundary Tests
     * ================================================================ */

    TEST("UNDER_FULL growth to exactly MAX_ENTRIES") {
        /* Test growth from 1 to exactly 629 entries (conversion threshold) */
        multiroar *r = multiroarBitNew();
        const uint64_t maxDirect = 629; /* MAX_ENTRIES_PER_DIRECT_LISTING */

        /* Insert exactly 628 entries (one below threshold) */
        for (uint64_t i = 0; i < maxDirect - 1; i++) {
            multiroarBitSet(r, i * 13); /* Spread to stay in one chunk */
        }

        /* Verify all are still set (should still be UNDER_FULL) */
        for (uint64_t i = 0; i < maxDirect - 1; i++) {
            if (!multiroarBitGet(r, i * 13)) {
                ERR("Position %" PRIu64 " not set before threshold!", i * 13);
            }
        }

        /* Insert the 629th entry - triggers conversion to FULL_BITMAP */
        multiroarBitSet(r, (maxDirect - 1) * 13);

        /* Verify all entries including the new one */
        for (uint64_t i = 0; i < maxDirect; i++) {
            if (!multiroarBitGet(r, i * 13)) {
                ERR("Position %" PRIu64 " not set after conversion!", i * 13);
            }
        }

        multiroarFree(r);
    }

    TEST("FULL_BITMAP exact boundary positions") {
        /* Test bits at exact byte and word boundaries within bitmap */
        multiroar *r = multiroarBitNew();

        /* Fill to get into bitmap mode */
        for (uint64_t i = 0; i < 700; i++) {
            multiroarBitSet(r, i);
        }

        /* Test byte boundary positions: 0, 7, 8, 15, 16, ... */
        const uint64_t byteBoundaries[] = {0,   7,   8,   15,  16, 23,
                                           24,  31,  32,  63,  64, 127,
                                           128, 255, 256, 511, 512};
        for (uint64_t i = 0;
             i < sizeof(byteBoundaries) / sizeof(byteBoundaries[0]); i++) {
            uint64_t pos = byteBoundaries[i];
            if (pos < 700) {
                if (!multiroarBitGet(r, pos)) {
                    ERR("Byte boundary position %" PRIu64 " should be set!",
                        pos);
                }
            }
        }

        /* Test 64-bit word boundary positions */
        const uint64_t wordBoundaries[] = {63,  64,  127, 128, 191, 192,
                                           255, 256, 319, 320, 383, 384,
                                           447, 448, 511, 512};
        for (uint64_t i = 0;
             i < sizeof(wordBoundaries) / sizeof(wordBoundaries[0]); i++) {
            uint64_t pos = wordBoundaries[i];
            if (pos < 700) {
                if (!multiroarBitGet(r, pos)) {
                    ERR("Word boundary position %" PRIu64 " should be set!",
                        pos);
                }
            }
        }

        multiroarFree(r);
    }

    TEST("sparse positions across chunk boundary") {
        /* Test sparse positions that span multiple chunks */
        multiroar *r = multiroarBitNew();
        const uint64_t chunkBits = 8192;

        /* Set sparse positions across 4 chunks */
        const uint64_t positions[] = {
            0,
            100,
            1000, /* Chunk 0 */
            chunkBits + 50,
            chunkBits + 500, /* Chunk 1 */
            chunkBits * 2 + 1,
            chunkBits * 2 + 999, /* Chunk 2 */
            chunkBits * 3 + 8191 /* Chunk 3, last bit */
        };
        const uint64_t numPositions = sizeof(positions) / sizeof(positions[0]);

        for (uint64_t i = 0; i < numPositions; i++) {
            multiroarBitSet(r, positions[i]);
        }

        /* Verify all positions */
        for (uint64_t i = 0; i < numPositions; i++) {
            if (!multiroarBitGet(r, positions[i])) {
                ERR("Sparse cross-chunk position %" PRIu64 " not set!",
                    positions[i]);
            }
        }

        /* Verify positions between are NOT set */
        if (multiroarBitGet(r, 50)) {
            ERRR("Position 50 should not be set!");
        }
        if (multiroarBitGet(r, chunkBits + 100)) {
            ERRR("Position in chunk 1 should not be set!");
        }

        multiroarFree(r);
    }

    TEST("set and immediate re-read in UNDER_FULL mode") {
        /* Test that set followed by immediate get works in UNDER_FULL */
        multiroar *r = multiroarBitNew();

        /* Set and verify in sequence, staying in UNDER_FULL mode */
        for (uint64_t i = 0; i < 100; i++) {
            uint64_t pos = i * 100; /* Spread positions */
            multiroarBitSet(r, pos);

            /* Immediate verification */
            if (!multiroarBitGet(r, pos)) {
                ERR("UNDER_FULL: position %" PRIu64 " not set immediately!",
                    pos);
            }

            /* Verify previously set positions still exist */
            for (uint64_t j = 0; j <= i; j++) {
                if (!multiroarBitGet(r, j * 100)) {
                    ERR("UNDER_FULL: earlier position %" PRIu64 " lost!",
                        j * 100);
                }
            }
        }

        multiroarFree(r);
    }

    TEST("set and immediate re-read in FULL_BITMAP mode") {
        /* Test that set followed by immediate get works in FULL_BITMAP */
        multiroar *r = multiroarBitNew();

        /* First, fill to trigger bitmap mode */
        for (uint64_t i = 0; i < 700; i++) {
            multiroarBitSet(r, i);
        }

        /* Now set additional positions and verify immediately */
        for (uint64_t i = 700; i < 1000; i++) {
            multiroarBitSet(r, i);

            if (!multiroarBitGet(r, i)) {
                ERR("FULL_BITMAP: position %" PRIu64 " not set immediately!",
                    i);
            }
        }

        /* Verify all positions are still set */
        for (uint64_t i = 0; i < 1000; i++) {
            if (!multiroarBitGet(r, i)) {
                ERR("FULL_BITMAP: position %" PRIu64 " lost!", i);
            }
        }

        multiroarFree(r);
    }

    TEST("previouslySet behavior across all chunk types") {
        multiroar *r = multiroarBitNew();

        /* Test in UNDER_FULL mode */
        bool prev = multiroarBitSet(r, 100);
        if (prev) {
            ERRR("UNDER_FULL: first set should return false!");
        }
        prev = multiroarBitSet(r, 100);
        if (!prev) {
            ERRR("UNDER_FULL: second set should return true!");
        }

        /* Fill to get into FULL_BITMAP mode */
        for (uint64_t i = 0; i < 700; i++) {
            multiroarBitSet(r, i);
        }

        /* Test in FULL_BITMAP mode with new position */
        prev = multiroarBitSet(r, 5000);
        if (prev) {
            ERRR("FULL_BITMAP: first set of new pos should return false!");
        }
        prev = multiroarBitSet(r, 5000);
        if (!prev) {
            ERRR("FULL_BITMAP: second set should return true!");
        }

        /* Test in FULL_BITMAP mode with existing position */
        prev = multiroarBitSet(r, 100); /* Was set earlier */
        if (!prev) {
            ERRR("FULL_BITMAP: set of existing pos should return true!");
        }

        multiroarFree(r);
    }

    TEST("large position values near SIZE_MAX") {
        multiroar *r = multiroarBitNew();

        /* Test very large positions */
        const uint64_t largePositions[] = {SIZE_MAX - 1, SIZE_MAX - 8192,
                                           SIZE_MAX - 8193, SIZE_MAX / 2,
                                           SIZE_MAX / 2 + 1};
        const uint64_t numLarge =
            sizeof(largePositions) / sizeof(largePositions[0]);

        for (uint64_t i = 0; i < numLarge; i++) {
            multiroarBitSet(r, largePositions[i]);
        }

        for (uint64_t i = 0; i < numLarge; i++) {
            if (!multiroarBitGet(r, largePositions[i])) {
                ERR("Large position %" PRIu64 " not set!", largePositions[i]);
            }
        }

        /* Verify nearby positions are NOT set */
        if (multiroarBitGet(r, SIZE_MAX - 2)) {
            ERRR("Position SIZE_MAX-2 should not be set!");
        }

        multiroarFree(r);
    }

    TEST("interleaved set pattern") {
        /* Set every other bit, then fill in the gaps */
        multiroar *r = multiroarBitNew();
        const uint64_t testRange = 2000;

        /* Set even positions first */
        for (uint64_t i = 0; i < testRange; i += 2) {
            multiroarBitSet(r, i);
        }

        /* Verify even positions are set, odd are not */
        for (uint64_t i = 0; i < testRange; i++) {
            bool isSet = multiroarBitGet(r, i);
            if (i % 2 == 0 && !isSet) {
                ERR("Even position %" PRIu64 " should be set!", i);
            }
            if (i % 2 == 1 && isSet) {
                ERR("Odd position %" PRIu64 " should NOT be set!", i);
            }
        }

        /* Now set odd positions */
        for (uint64_t i = 1; i < testRange; i += 2) {
            multiroarBitSet(r, i);
        }

        /* Verify all positions are now set */
        for (uint64_t i = 0; i < testRange; i++) {
            if (!multiroarBitGet(r, i)) {
                ERR("After fill: position %" PRIu64 " should be set!", i);
            }
        }

        multiroarFree(r);
    }

    TEST("reverse order insertion") {
        /* Insert bits in reverse order */
        multiroar *r = multiroarBitNew();
        const uint64_t testCount = 1000;

        for (uint64_t i = testCount; i > 0; i--) {
            multiroarBitSet(r, i - 1);

            /* Verify this position is set */
            if (!multiroarBitGet(r, i - 1)) {
                ERR("Reverse insert: position %" PRIu64 " not set!", i - 1);
            }
        }

        /* Verify all positions are set */
        for (uint64_t i = 0; i < testCount; i++) {
            if (!multiroarBitGet(r, i)) {
                ERR("Reverse insert: position %" PRIu64 " lost!", i);
            }
        }

        multiroarFree(r);
    }

    TEST("random order insertion with verification") {
        /* Insert bits in pseudo-random order using LCG */
        multiroar *r = multiroarBitNew();
        const uint64_t testCount = 500;
        uint64_t *positions = zcalloc(testCount, sizeof(uint64_t));

        /* Generate pseudo-random positions */
        uint32_t seed = 12345;
        for (uint64_t i = 0; i < testCount; i++) {
            seed = seed * 1103515245 + 12345;
            positions[i] = (seed >> 8) % 10000;
        }

        /* Insert all positions */
        for (uint64_t i = 0; i < testCount; i++) {
            multiroarBitSet(r, positions[i]);
        }

        /* Verify all positions are set */
        for (uint64_t i = 0; i < testCount; i++) {
            if (!multiroarBitGet(r, positions[i])) {
                ERR("Random order: position %" PRIu64 " not set!",
                    positions[i]);
            }
        }

        zfree(positions);
        multiroarFree(r);
    }

    TEST("chunk type transitions (UNDER_FULL -> FULL_BITMAP)") {
        /* Carefully test the transition from UNDER_FULL to FULL_BITMAP */
        multiroar *r = multiroarBitNew();

        /* Insert 628 positions (just under threshold) */
        for (uint64_t i = 0; i < 628; i++) {
            multiroarBitSet(r, i);
            if (!multiroarBitGet(r, i)) {
                ERR("Pre-transition: position %" PRIu64 " not set!", i);
            }
        }

        /* Insert 629th position - triggers transition */
        multiroarBitSet(r, 628);
        if (!multiroarBitGet(r, 628)) {
            ERRR("Transition position 628 not set!");
        }

        /* Verify all previous positions survived transition */
        for (uint64_t i = 0; i < 629; i++) {
            if (!multiroarBitGet(r, i)) {
                ERR("Post-transition: position %" PRIu64 " lost!", i);
            }
        }

        /* Continue inserting to verify bitmap mode works */
        for (uint64_t i = 629; i < 1000; i++) {
            multiroarBitSet(r, i);
            if (!multiroarBitGet(r, i)) {
                ERR("Bitmap mode: position %" PRIu64 " not set!", i);
            }
        }

        multiroarFree(r);
    }

    TEST("OVER_FULL transition (FULL_BITMAP -> negative list)") {
        /* Test transition to OVER_FULL when most bits are set */
        multiroar *r = multiroarBitNew();
        const uint64_t chunkBits = 8192;
        const uint64_t threshold = 7564; /* Just past OVER_FULL threshold */

        /* Set bits sequentially up to threshold */
        for (uint64_t i = 0; i < threshold; i++) {
            multiroarBitSet(r, i);

            /* Verify immediately */
            if (!multiroarBitGet(r, i)) {
                ERR("OVER_FULL transition: position %" PRIu64
                    " not set immediately!",
                    i);
            }
        }

        /* Verify all positions still set after potential transition */
        uint64_t failures = 0;
        for (uint64_t i = 0; i < threshold; i++) {
            if (!multiroarBitGet(r, i)) {
                if (failures < 10) {
                    ERR("OVER_FULL: position %" PRIu64 " lost!", i);
                }
                failures++;
            }
        }
        if (failures > 10) {
            printf("... and %" PRIu64 " more positions lost\n", failures - 10);
        }

        /* Verify positions past threshold are NOT set */
        for (uint64_t i = threshold; i < chunkBits; i += 100) {
            if (multiroarBitGet(r, i)) {
                ERR("OVER_FULL: position %" PRIu64 " incorrectly set!", i);
            }
        }

        multiroarFree(r);
    }

    TEST("ALL_1 transition (fill entire chunk)") {
        /* Fill an entire chunk to trigger ALL_1 type */
        multiroar *r = multiroarBitNew();
        const uint64_t chunkBits = 8192;

        printf("    Filling entire chunk (%" PRIu64 " bits)...\n", chunkBits);

        /* Set all bits in chunk 0 */
        for (uint64_t i = 0; i < chunkBits; i++) {
            multiroarBitSet(r, i);

            /* Periodic verification */
            if (i % 1000 == 0 || i == chunkBits - 1) {
                if (!multiroarBitGet(r, i)) {
                    ERR("ALL_1 fill: position %" PRIu64 " not set!", i);
                }
            }
        }

        /* Verify all bits in chunk are set */
        uint64_t failures = 0;
        for (uint64_t i = 0; i < chunkBits; i++) {
            if (!multiroarBitGet(r, i)) {
                if (failures < 5) {
                    ERR("ALL_1 verify: position %" PRIu64 " not set!", i);
                }
                failures++;
            }
        }
        if (failures > 5) {
            printf("    ... and %" PRIu64
                   " more bits not set (total failures: %" PRIu64 ")\n",
                   failures - 5, failures);
        }

        /* Verify bits in next chunk are NOT set */
        if (multiroarBitGet(r, chunkBits)) {
            ERRR("Bit in next chunk should not be set!");
        }
        if (multiroarBitGet(r, chunkBits + 100)) {
            ERRR("Bit in next chunk should not be set!");
        }

        /* Setting a bit in the ALL_1 chunk should return true (already set) */
        bool prev = multiroarBitSet(r, 4096);
        if (!prev) {
            ERRR("ALL_1: setting existing bit should return true!");
        }

        multiroarFree(r);
    }

    TEST("mixed chunk types in single roar") {
        /* Create a roar with all different chunk types */
        multiroar *r = multiroarBitNew();
        const uint64_t chunkBits = 8192;

        /* Chunk 0: UNDER_FULL (sparse, few bits) */
        multiroarBitSet(r, 10);
        multiroarBitSet(r, 100);
        multiroarBitSet(r, 500);

        /* Chunk 1: FULL_BITMAP (moderate density) */
        for (uint64_t i = chunkBits; i < chunkBits + 2000; i++) {
            multiroarBitSet(r, i);
        }

        /* Chunk 2: High density (triggers OVER_FULL if working) */
        for (uint64_t i = chunkBits * 2; i < chunkBits * 2 + 7600; i++) {
            multiroarBitSet(r, i);
        }

        /* Verify chunk 0 */
        if (!multiroarBitGet(r, 10) || !multiroarBitGet(r, 100) ||
            !multiroarBitGet(r, 500)) {
            ERRR("Chunk 0 bits not set!");
        }
        if (multiroarBitGet(r, 50) || multiroarBitGet(r, 200)) {
            ERRR("Chunk 0 has unexpected bits set!");
        }

        /* Verify chunk 1 */
        for (uint64_t i = chunkBits; i < chunkBits + 2000; i += 100) {
            if (!multiroarBitGet(r, i)) {
                ERR("Chunk 1 position %" PRIu64 " not set!", i);
            }
        }

        /* Verify chunk 2 */
        for (uint64_t i = chunkBits * 2; i < chunkBits * 2 + 7600; i += 100) {
            if (!multiroarBitGet(r, i)) {
                ERR("Chunk 2 position %" PRIu64 " not set!", i);
            }
        }

        multiroarFree(r);
    }

    TEST("packed array sorted insertion stress") {
        /* Stress test the sorted insertion in UNDER_FULL mode */
        multiroar *r = multiroarBitNew();

        /* Insert positions that stress the sorted insert algorithm */
        const uint64_t positions[] = {
            500, 100, 900, 50, 950,  25,  975, 12,  988, 6,  994, 3,
            997, 1,   999, 0,  1000, 2,   998, 4,   996, 8,  992, 16,
            984, 32,  968, 64, 936,  128, 872, 256, 744, 512};
        const uint64_t numPos = sizeof(positions) / sizeof(positions[0]);

        for (uint64_t i = 0; i < numPos; i++) {
            multiroarBitSet(r, positions[i]);
        }

        /* Verify all positions are set */
        for (uint64_t i = 0; i < numPos; i++) {
            if (!multiroarBitGet(r, positions[i])) {
                ERR("Sorted insert stress: position %" PRIu64 " not set!",
                    positions[i]);
            }
        }

        multiroarFree(r);
    }

    TEST("empty chunk queries") {
        /* Test queries on chunks that don't exist (empty) */
        multiroar *r = multiroarBitNew();

        /* Query various positions without setting anything */
        if (multiroarBitGet(r, 0)) {
            ERRR("Empty roar: position 0 should not be set!");
        }
        if (multiroarBitGet(r, 100)) {
            ERRR("Empty roar: position 100 should not be set!");
        }
        if (multiroarBitGet(r, 8192)) {
            ERRR("Empty roar: position 8192 should not be set!");
        }
        if (multiroarBitGet(r, SIZE_MAX / 2)) {
            ERRR("Empty roar: large position should not be set!");
        }

        /* Set one position, verify others still not set */
        multiroarBitSet(r, 5000);
        if (!multiroarBitGet(r, 5000)) {
            ERRR("Position 5000 should be set!");
        }
        if (multiroarBitGet(r, 4999) || multiroarBitGet(r, 5001)) {
            ERRR("Adjacent positions should not be set!");
        }

        multiroarFree(r);
    }

    TEST("PERF: sparse lookup performance") {
        const uint64_t numPositions = 1000;
        const uint64_t positionSpread = 1000000;
        multiroar *r = multiroarBitNew();

        /* Create sparse bitmap across many chunks */
        for (uint64_t i = 0; i < numPositions; i++) {
            multiroarBitSet(r, i * positionSpread);
        }

        int64_t startNs = timeUtilMonotonicNs();
        for (uint64_t round = 0; round < 100; round++) {
            for (uint64_t i = 0; i < numPositions; i++) {
                multiroarBitGet(r, i * positionSpread);
            }
        }
        int64_t elapsed = timeUtilMonotonicNs() - startNs;
        printf("Sparse lookup: %.1f ns/op, %.0f ops/sec\n",
               (double)elapsed / (numPositions * 100),
               (numPositions * 100) / (elapsed / 1e9));

        multiroarFree(r);
    }

    /* ========================================
     * FUZZ TESTS - Oracle-Based Verification
     * ======================================== */
    printf("\n=== MULTIROAR FUZZ TESTING ===\n\n");

/* Simple bitmap oracle - just use bits in an array of uint64_t */
#define FUZZ_ORACLE_MAX_BIT 100000
#define FUZZ_ORACLE_WORDS ((FUZZ_ORACLE_MAX_BIT + 63) / 64)
#define FUZZ_ORACLE_SET(oracle, pos)                                           \
    ((oracle)[(pos) / 64] |= (1ULL << ((pos) % 64)))
#define FUZZ_ORACLE_CLEAR(oracle, pos)                                         \
    ((oracle)[(pos) / 64] &= ~(1ULL << ((pos) % 64)))
#define FUZZ_ORACLE_GET(oracle, pos)                                           \
    (((oracle)[(pos) / 64] >> ((pos) % 64)) & 1)

    TEST("FUZZ: random bit operations with oracle") {
        multiroar *r = multiroarBitNew();
        uint64_t oracle[FUZZ_ORACLE_WORDS] = {0};

        srand(12345);
        const uint64_t numOps = 10000;
        uint64_t setOps = 0, getOps = 0;

        for (uint64_t i = 0; i < numOps; i++) {
            uint64_t pos = (uint64_t)rand() % FUZZ_ORACLE_MAX_BIT;
            int op = rand() % 10;

            if (op < 7) {
                /* Set operation (70%) */
                bool mrPrev = multiroarBitSet(r, pos);
                bool oraclePrev = FUZZ_ORACLE_GET(oracle, pos);
                FUZZ_ORACLE_SET(oracle, pos);

                if (mrPrev != oraclePrev) {
                    ERR("Set at %" PRIu64
                        ": multiroar said prev=%d, oracle said "
                        "prev=%d",
                        pos, mrPrev, oraclePrev);
                }
                setOps++;
            } else {
                /* Get operation (30%) */
                bool mrVal = multiroarBitGet(r, pos);
                bool oracleVal = FUZZ_ORACLE_GET(oracle, pos);

                if (mrVal != oracleVal) {
                    ERR("Get at %" PRIu64 ": multiroar=%d, oracle=%d", pos,
                        mrVal, oracleVal);
                }
                getOps++;
            }
        }

        /* Full verification */
        uint64_t mismatches = 0;
        for (uint64_t pos = 0; pos < FUZZ_ORACLE_MAX_BIT; pos++) {
            bool mrVal = multiroarBitGet(r, pos);
            bool oracleVal = FUZZ_ORACLE_GET(oracle, pos);
            if (mrVal != oracleVal) {
                if (mismatches < 10) {
                    ERR("Final verify at %" PRIu64 ": multiroar=%d, oracle=%d",
                        pos, mrVal, oracleVal);
                }
                mismatches++;
            }
        }

        if (mismatches > 0) {
            ERR("Total mismatches: %" PRIu64 "", mismatches);
        }

        printf("  set=%" PRIu64 " get=%" PRIu64 " verified=%d\n", setOps,
               getOps, FUZZ_ORACLE_MAX_BIT);
        multiroarFree(r);
    }

    TEST("FUZZ: sequential then random access") {
        multiroar *r = multiroarBitNew();
        uint64_t oracle[FUZZ_ORACLE_WORDS] = {0};

        /* Sequential fill */
        for (uint64_t i = 0; i < 5000; i++) {
            multiroarBitSet(r, i);
            FUZZ_ORACLE_SET(oracle, i);
        }

        /* Random access verification */
        srand(54321);
        for (uint64_t i = 0; i < 10000; i++) {
            uint64_t pos = (uint64_t)rand() % FUZZ_ORACLE_MAX_BIT;
            bool mrVal = multiroarBitGet(r, pos);
            bool oracleVal = FUZZ_ORACLE_GET(oracle, pos);

            if (mrVal != oracleVal) {
                ERR("Pos %" PRIu64 ": multiroar=%d, oracle=%d", pos, mrVal,
                    oracleVal);
            }
        }

        printf("  sequential fill 5000, random verify 10000\n");
        multiroarFree(r);
    }

    TEST("FUZZ: sparse bits across many chunks") {
        multiroar *r = multiroarBitNew();

        srand(99999);
        const uint64_t numSparse = 1000;
        uint64_t *positions = zmalloc(numSparse * sizeof(uint64_t));

        /* Generate sparse positions across many chunks (8192 bits per chunk) */
        for (uint64_t i = 0; i < numSparse; i++) {
            /* Spread across up to 1000 chunks */
            positions[i] =
                ((uint64_t)rand() % 1000) * 8192 + ((uint64_t)rand() % 8192);
            multiroarBitSet(r, positions[i]);
        }

        /* Verify all set positions */
        for (uint64_t i = 0; i < numSparse; i++) {
            if (!multiroarBitGet(r, positions[i])) {
                ERR("Sparse position %" PRIu64 " not set!", positions[i]);
            }
        }

        /* Verify some unset positions */
        uint64_t verified = 0;
        for (uint64_t chunk = 0; chunk < 100; chunk++) {
            for (uint64_t offset = 0; offset < 100; offset++) {
                uint64_t pos =
                    chunk * 8192 + offset * 80 + 40; /* Unlikely to be set */
                bool isSet = multiroarBitGet(r, pos);
                bool shouldBeSet = false;

                for (uint64_t i = 0; i < numSparse; i++) {
                    if (positions[i] == pos) {
                        shouldBeSet = true;
                        break;
                    }
                }

                if (isSet != shouldBeSet) {
                    ERR("Unset verify at %" PRIu64 ": got %d expected %d", pos,
                        isSet, shouldBeSet);
                }
                verified++;
            }
        }

        printf("  sparse bits=%" PRIu64 ", verified unset=%" PRIu64 "\n",
               numSparse, verified);
        zfree(positions);
        multiroarFree(r);
    }

    TEST("FUZZ: chunk boundary stress") {
        multiroar *r = multiroarBitNew();
        uint64_t oracle[FUZZ_ORACLE_WORDS] = {0};
        const uint64_t chunkBits = 8192;

        srand(77777);

        /* Set bits around chunk boundaries */
        for (uint64_t chunk = 0; chunk < 10; chunk++) {
            uint64_t base = chunk * chunkBits;

            /* Around boundary start */
            for (int offset = -5; offset <= 5; offset++) {
                if (base + offset < FUZZ_ORACLE_MAX_BIT) {
                    uint64_t pos = base + offset;
                    if (rand() % 2) {
                        multiroarBitSet(r, pos);
                        FUZZ_ORACLE_SET(oracle, pos);
                    }
                }
            }

            /* Around boundary end */
            if (base + chunkBits - 1 < FUZZ_ORACLE_MAX_BIT) {
                for (int offset = -5; offset <= 5; offset++) {
                    uint64_t pos = base + chunkBits - 1 + offset;
                    if (pos < FUZZ_ORACLE_MAX_BIT && rand() % 2) {
                        multiroarBitSet(r, pos);
                        FUZZ_ORACLE_SET(oracle, pos);
                    }
                }
            }
        }

        /* Verify around boundaries */
        uint64_t mismatches = 0;
        for (uint64_t chunk = 0; chunk < 10; chunk++) {
            uint64_t base = chunk * chunkBits;

            for (int offset = -10; offset <= (int)chunkBits + 10; offset++) {
                uint64_t pos = base + offset;
                if (pos >= FUZZ_ORACLE_MAX_BIT) {
                    continue;
                }

                bool mrVal = multiroarBitGet(r, pos);
                bool oracleVal = FUZZ_ORACLE_GET(oracle, pos);

                if (mrVal != oracleVal) {
                    if (mismatches < 5) {
                        ERR("Boundary pos %" PRIu64 " (chunk %" PRIu64
                            ", offset %d): mr=%d "
                            "oracle=%d",
                            pos, chunk, offset, mrVal, oracleVal);
                    }
                    mismatches++;
                }
            }
        }

        printf("  tested 10 chunk boundaries, mismatches=%" PRIu64 "\n",
               mismatches);
        multiroarFree(r);
    }

    TEST("FUZZ: duplicate set operations") {
        multiroar *r = multiroarBitNew();

        srand(11111);
        const uint64_t numUnique = 500;
        uint64_t *positions = zmalloc(numUnique * sizeof(uint64_t));

        /* Generate unique positions */
        for (uint64_t i = 0; i < numUnique; i++) {
            positions[i] = (uint64_t)rand() % 50000;
        }

        /* First pass: set all */
        for (uint64_t i = 0; i < numUnique; i++) {
            multiroarBitSet(r, positions[i]);
        }

        /* Second pass: set all again, should all report previously set */
        uint64_t reportedNew = 0;
        for (uint64_t i = 0; i < numUnique; i++) {
            bool prev = multiroarBitSet(r, positions[i]);
            if (!prev) {
                /* This might happen if we have duplicate positions */
                bool duplicate = false;
                for (uint64_t j = 0; j < i; j++) {
                    if (positions[j] == positions[i]) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    reportedNew++;
                    if (reportedNew <= 5) {
                        ERR("Position %" PRIu64 " reported new on second set!",
                            positions[i]);
                    }
                }
            }
        }

        /* Verify all still set */
        for (uint64_t i = 0; i < numUnique; i++) {
            if (!multiroarBitGet(r, positions[i])) {
                ERR("Position %" PRIu64 " not set after double-set!",
                    positions[i]);
            }
        }

        printf("  double-set %" PRIu64 " positions\n", numUnique);
        zfree(positions);
        multiroarFree(r);
    }

    TEST("FUZZ: adversarial patterns - reverse order") {
        multiroar *r = multiroarBitNew();
        const uint64_t count = 2000;

        /* Insert in reverse order - stresses sorted insertion */
        for (uint64_t i = count; i > 0; i--) {
            multiroarBitSet(r, i - 1);
        }

        /* Verify all set */
        for (uint64_t i = 0; i < count; i++) {
            if (!multiroarBitGet(r, i)) {
                ERR("Reverse insert: position %" PRIu64 " not set!", i);
            }
        }

        printf("  reverse order insert %" PRIu64 " positions\n", count);
        multiroarFree(r);
    }

    TEST("FUZZ: adversarial patterns - alternating") {
        multiroar *r = multiroarBitNew();
        const uint64_t count = 5000;

        /* Set every other bit */
        for (uint64_t i = 0; i < count * 2; i += 2) {
            multiroarBitSet(r, i);
        }

        /* Verify even positions set, odd not set */
        for (uint64_t i = 0; i < count * 2; i++) {
            bool expected = (i % 2 == 0);
            bool actual = multiroarBitGet(r, i);
            if (actual != expected) {
                ERR("Alternating at %" PRIu64 ": got %d expected %d", i, actual,
                    expected);
            }
        }

        printf("  alternating pattern %" PRIu64 " positions\n", count);
        multiroarFree(r);
    }

    TEST("FUZZ: large position values") {
        multiroar *r = multiroarBitNew();

        /* Test very large position values - avoid adjacent positions */
        const uint64_t largePositions[] = {
            SIZE_MAX - 1,      SIZE_MAX - 200,    SIZE_MAX - 8192,
            SIZE_MAX - 16384,  SIZE_MAX / 2,      SIZE_MAX / 4,
            (uint64_t)1 << 40, (uint64_t)1 << 50, (uint64_t)1 << 60};
        const uint64_t numLarge =
            sizeof(largePositions) / sizeof(largePositions[0]);

        for (uint64_t i = 0; i < numLarge; i++) {
            multiroarBitSet(r, largePositions[i]);
        }

        for (uint64_t i = 0; i < numLarge; i++) {
            if (!multiroarBitGet(r, largePositions[i])) {
                ERR("Large position %" PRIu64 " not set!", largePositions[i]);
            }
        }

        /* Verify adjacent positions not set (check position - 2 to avoid
         * collision with positions that differ by 1) */
        for (uint64_t i = 0; i < numLarge; i++) {
            if (largePositions[i] > 2) {
                uint64_t adjacent = largePositions[i] - 2;
                /* Make sure this adjacent position isn't one we set */
                bool shouldBeSet = false;
                for (uint64_t j = 0; j < numLarge; j++) {
                    if (largePositions[j] == adjacent) {
                        shouldBeSet = true;
                        break;
                    }
                }
                if (!shouldBeSet && multiroarBitGet(r, adjacent)) {
                    ERR("Adjacent-2 to large position %" PRIu64
                        " incorrectly set!",
                        largePositions[i]);
                }
            }
        }

        printf("  verified %" PRIu64 " large positions\n", numLarge);
        multiroarFree(r);
    }

    TEST("FUZZ: stress 100K random operations") {
        multiroar *r = multiroarBitNew();
        uint64_t oracle[FUZZ_ORACLE_WORDS] = {0};

        srand(88888);
        const uint64_t numOps = 100000;

        for (uint64_t i = 0; i < numOps; i++) {
            uint64_t pos = (uint64_t)rand() % FUZZ_ORACLE_MAX_BIT;

            bool mrPrev = multiroarBitSet(r, pos);
            bool oraclePrev = FUZZ_ORACLE_GET(oracle, pos);
            FUZZ_ORACLE_SET(oracle, pos);

            if (mrPrev != oraclePrev) {
                ERR("100K stress at %" PRIu64 ": prev mismatch mr=%d oracle=%d",
                    pos, mrPrev, oraclePrev);
            }

            /* Periodic spot check */
            if (i % 10000 == 0) {
                uint64_t checkPos = (uint64_t)rand() % FUZZ_ORACLE_MAX_BIT;
                bool mrVal = multiroarBitGet(r, checkPos);
                bool oracleVal = FUZZ_ORACLE_GET(oracle, checkPos);
                if (mrVal != oracleVal) {
                    ERR("Spot check at round %" PRIu64 " pos %" PRIu64
                        ": mr=%d oracle=%d",
                        i, checkPos, mrVal, oracleVal);
                }
            }
        }

        /* Final full verification */
        uint64_t mismatches = 0;
        uint64_t bitsSet = 0;
        for (uint64_t pos = 0; pos < FUZZ_ORACLE_MAX_BIT; pos++) {
            bool mrVal = multiroarBitGet(r, pos);
            bool oracleVal = FUZZ_ORACLE_GET(oracle, pos);
            if (mrVal != oracleVal) {
                mismatches++;
            }
            if (oracleVal) {
                bitsSet++;
            }
        }

        if (mismatches > 0) {
            ERR("100K stress final: %" PRIu64 " mismatches", mismatches);
        }

        printf("  100K ops, %" PRIu64 " unique bits set, %" PRIu64
               " mismatches\n",
               bitsSet, mismatches);
        multiroarFree(r);
    }

    printf("\n=== NEW FUNCTIONALITY TESTS ===\n\n");

    TEST("multiroarRemove - basic remove from UNDER_FULL") {
        multiroar *r = multiroarBitNew();
        multiroarBitSet(r, 100);
        multiroarBitSet(r, 200);
        multiroarBitSet(r, 300);

        if (!multiroarBitGet(r, 100)) {
            ERRR("bit 100 should be set");
        }
        if (!multiroarBitGet(r, 200)) {
            ERRR("bit 200 should be set");
        }
        if (!multiroarBitGet(r, 300)) {
            ERRR("bit 300 should be set");
        }

        /* Remove middle element */
        bool wasSet = multiroarRemove(r, 200);
        if (!wasSet) {
            ERRR("wasSet should be true for bit 200");
        }
        if (multiroarBitGet(r, 200)) {
            ERRR("bit 200 should be clear after remove");
        }
        if (!multiroarBitGet(r, 100)) {
            ERRR("bit 100 should still be set");
        }
        if (!multiroarBitGet(r, 300)) {
            ERRR("bit 300 should still be set");
        }

        /* Remove again - should return false */
        wasSet = multiroarRemove(r, 200);
        if (wasSet) {
            ERRR("wasSet should be false for already-cleared bit");
        }

        multiroarFree(r);
    }

    TEST("multiroarRemove - remove from FULL_BITMAP") {
        multiroar *r = multiroarBitNew();
        /* Fill enough bits to trigger FULL_BITMAP mode (>629) */
        for (uint64_t i = 0; i < 1000; i++) {
            multiroarBitSet(r, i);
        }

        if (!multiroarBitGet(r, 500)) {
            ERRR("bit 500 should be set");
        }
        bool wasSet = multiroarRemove(r, 500);
        if (!wasSet) {
            ERRR("wasSet should be true");
        }
        if (multiroarBitGet(r, 500)) {
            ERRR("bit 500 should be clear");
        }

        /* Other bits should still be set */
        if (!multiroarBitGet(r, 499)) {
            ERRR("bit 499 should still be set");
        }
        if (!multiroarBitGet(r, 501)) {
            ERRR("bit 501 should still be set");
        }

        multiroarFree(r);
    }

    TEST("multiroarRemove - remove all bits from chunk") {
        multiroar *r = multiroarBitNew();
        multiroarBitSet(r, 100);

        bool wasSet = multiroarRemove(r, 100);
        if (!wasSet) {
            ERRR("wasSet should be true");
        }
        if (multiroarBitGet(r, 100)) {
            ERRR("bit 100 should be clear");
        }

        /* Chunk should be gone, but we can still add to it */
        multiroarBitSet(r, 100);
        if (!multiroarBitGet(r, 100)) {
            ERRR("bit 100 should be set again");
        }

        multiroarFree(r);
    }

    TEST("multiroarDuplicate - basic clone") {
        multiroar *r = multiroarBitNew();
        multiroarBitSet(r, 100);
        multiroarBitSet(r, 200);
        multiroarBitSet(r, 10000);

        multiroar *dup = multiroarDuplicate(r);
        if (dup == NULL) {
            ERRR("dup should not be NULL");
        }
        if (dup == r) {
            ERRR("dup should be different pointer");
        }

        /* Verify all bits match */
        if (!multiroarBitGet(dup, 100)) {
            ERRR("dup bit 100 should be set");
        }
        if (!multiroarBitGet(dup, 200)) {
            ERRR("dup bit 200 should be set");
        }
        if (!multiroarBitGet(dup, 10000)) {
            ERRR("dup bit 10000 should be set");
        }
        if (multiroarBitGet(dup, 101)) {
            ERRR("dup bit 101 should be clear");
        }

        /* Modifications to original shouldn't affect dup */
        multiroarBitSet(r, 999);
        if (!multiroarBitGet(r, 999)) {
            ERRR("original bit 999 should be set");
        }
        if (multiroarBitGet(dup, 999)) {
            ERRR("dup bit 999 should be clear");
        }

        multiroarFree(r);
        multiroarFree(dup);
    }

    TEST("multiroarOr - basic OR operation") {
        multiroar *a = multiroarBitNew();
        multiroar *b = multiroarBitNew();

        multiroarBitSet(a, 100);
        multiroarBitSet(a, 200);
        multiroarBitSet(b, 200);
        multiroarBitSet(b, 300);

        multiroarOr(a, b);

        /* a should have: 100, 200, 300 */
        if (!multiroarBitGet(a, 100)) {
            ERRR("OR: bit 100 should be set");
        }
        if (!multiroarBitGet(a, 200)) {
            ERRR("OR: bit 200 should be set");
        }
        if (!multiroarBitGet(a, 300)) {
            ERRR("OR: bit 300 should be set");
        }
        if (multiroarBitGet(a, 150)) {
            ERRR("OR: bit 150 should be clear");
        }

        multiroarFree(a);
        multiroarFree(b);
    }

    TEST("multiroarAnd - basic AND operation") {
        multiroar *a = multiroarBitNew();
        multiroar *b = multiroarBitNew();

        multiroarBitSet(a, 100);
        multiroarBitSet(a, 200);
        multiroarBitSet(a, 300);
        multiroarBitSet(b, 200);
        multiroarBitSet(b, 300);
        multiroarBitSet(b, 400);

        multiroarAnd(a, b);

        /* a should have: 200, 300 (intersection) */
        if (multiroarBitGet(a, 100)) {
            ERRR("AND: bit 100 should be clear");
        }
        if (!multiroarBitGet(a, 200)) {
            ERRR("AND: bit 200 should be set");
        }
        if (!multiroarBitGet(a, 300)) {
            ERRR("AND: bit 300 should be set");
        }
        if (multiroarBitGet(a, 400)) {
            ERRR("AND: bit 400 should be clear");
        }

        multiroarFree(a);
        multiroarFree(b);
    }

    TEST("multiroarXor - basic XOR operation") {
        multiroar *a = multiroarBitNew();
        multiroar *b = multiroarBitNew();

        multiroarBitSet(a, 100);
        multiroarBitSet(a, 200);
        multiroarBitSet(b, 200);
        multiroarBitSet(b, 300);

        multiroarXor(a, b);

        /* a should have: 100, 300 (symmetric difference) */
        if (!multiroarBitGet(a, 100)) {
            ERRR("XOR: bit 100 should be set");
        }
        if (multiroarBitGet(a, 200)) {
            ERRR("XOR: bit 200 should be clear");
        }
        if (!multiroarBitGet(a, 300)) {
            ERRR("XOR: bit 300 should be set");
        }

        multiroarFree(a);
        multiroarFree(b);
    }

    TEST("multiroarNot - basic NOT operation") {
        multiroar *r = multiroarBitNew();
        /* Set specific bits in a chunk */
        multiroarBitSet(r, 0);
        multiroarBitSet(r, 1);
        multiroarBitSet(r, 100);

        multiroarNot(r);

        /* The set bits should now be clear, unset bits should be set */
        if (multiroarBitGet(r, 0)) {
            ERRR("NOT: bit 0 should be clear");
        }
        if (multiroarBitGet(r, 1)) {
            ERRR("NOT: bit 1 should be clear");
        }
        if (multiroarBitGet(r, 100)) {
            ERRR("NOT: bit 100 should be clear");
        }
        if (!multiroarBitGet(r, 2)) {
            ERRR("NOT: bit 2 should be set");
        }
        if (!multiroarBitGet(r, 50)) {
            ERRR("NOT: bit 50 should be set");
        }
        if (!multiroarBitGet(r, 8191)) {
            ERRR("NOT: bit 8191 should be set");
        }

        /* Bits outside the existing chunk are still 0 */
        if (multiroarBitGet(r, 8192)) {
            ERRR("NOT: bit 8192 should be clear");
        }

        multiroarFree(r);
    }

    TEST("multiroarNewOr - creates new roar") {
        multiroar *a = multiroarBitNew();
        multiroar *b = multiroarBitNew();

        multiroarBitSet(a, 100);
        multiroarBitSet(b, 200);

        multiroar *result = multiroarNewOr(a, b);
        if (result == NULL) {
            ERRR("NewOr result should not be NULL");
        }
        if (result == a) {
            ERRR("NewOr result should not be a");
        }
        if (result == b) {
            ERRR("NewOr result should not be b");
        }

        if (!multiroarBitGet(result, 100)) {
            ERRR("NewOr: bit 100 should be set");
        }
        if (!multiroarBitGet(result, 200)) {
            ERRR("NewOr: bit 200 should be set");
        }

        /* Originals unchanged */
        if (multiroarBitGet(a, 200)) {
            ERRR("NewOr: a bit 200 should be clear");
        }
        if (multiroarBitGet(b, 100)) {
            ERRR("NewOr: b bit 100 should be clear");
        }

        multiroarFree(a);
        multiroarFree(b);
        multiroarFree(result);
    }

    TEST("FUZZ: set operations with oracle verification") {
        printf("  Testing set operations with oracle...\n");

#define FUZZ_OP_SIZE 500
        uint64_t oracleA[divCeil(FUZZ_OP_SIZE, 64)] = {0};
        uint64_t oracleB[divCeil(FUZZ_OP_SIZE, 64)] = {0};

        multiroar *a = multiroarBitNew();
        multiroar *b = multiroarBitNew();

        /* Populate both with random bits */
        for (uint64_t i = 0; i < 200; i++) {
            uint64_t posA = rand() % FUZZ_OP_SIZE;
            uint64_t posB = rand() % FUZZ_OP_SIZE;
            multiroarBitSet(a, posA);
            multiroarBitSet(b, posB);
            FUZZ_ORACLE_SET(oracleA, posA);
            FUZZ_ORACLE_SET(oracleB, posB);
        }

        /* Test OR */
        multiroar *orResult = multiroarNewOr(a, b);
        uint64_t orMismatches = 0;
        for (uint64_t i = 0; i < FUZZ_OP_SIZE; i++) {
            bool expected =
                FUZZ_ORACLE_GET(oracleA, i) || FUZZ_ORACLE_GET(oracleB, i);
            bool actual = multiroarBitGet(orResult, i);
            if (expected != actual) {
                orMismatches++;
            }
        }
        if (orMismatches > 0) {
            ERR("OR operation: %" PRIu64 " mismatches", orMismatches);
        }
        multiroarFree(orResult);

        /* Test AND */
        multiroar *andResult = multiroarNewAnd(a, b);
        uint64_t andMismatches = 0;
        for (uint64_t i = 0; i < FUZZ_OP_SIZE; i++) {
            bool expected =
                FUZZ_ORACLE_GET(oracleA, i) && FUZZ_ORACLE_GET(oracleB, i);
            bool actual = multiroarBitGet(andResult, i);
            if (expected != actual) {
                andMismatches++;
            }
        }
        if (andMismatches > 0) {
            ERR("AND operation: %" PRIu64 " mismatches", andMismatches);
        }
        multiroarFree(andResult);

        /* Test XOR */
        multiroar *xorResult = multiroarNewXor(a, b);
        uint64_t xorMismatches = 0;
        for (uint64_t i = 0; i < FUZZ_OP_SIZE; i++) {
            bool expected =
                FUZZ_ORACLE_GET(oracleA, i) != FUZZ_ORACLE_GET(oracleB, i);
            bool actual = multiroarBitGet(xorResult, i);
            if (expected != actual) {
                xorMismatches++;
            }
        }
        if (xorMismatches > 0) {
            ERR("XOR operation: %" PRIu64 " mismatches", xorMismatches);
        }
        multiroarFree(xorResult);

        /* Test NOT */
        multiroar *notResult = multiroarDuplicate(a);
        multiroarNot(notResult);
        uint64_t notMismatches = 0;
        for (uint64_t i = 0; i < FUZZ_OP_SIZE; i++) {
            bool expected = !FUZZ_ORACLE_GET(oracleA, i);
            bool actual = multiroarBitGet(notResult, i);
            if (expected != actual) {
                notMismatches++;
            }
        }
        if (notMismatches > 0) {
            ERR("NOT operation: %" PRIu64 " mismatches (within existing chunk)",
                notMismatches);
        }
        multiroarFree(notResult);

        printf("  OR=%" PRIu64 " AND=%" PRIu64 " XOR=%" PRIu64 " NOT=%" PRIu64
               " mismatches\n",
               orMismatches, andMismatches, xorMismatches, notMismatches);

        multiroarFree(a);
        multiroarFree(b);
#undef FUZZ_OP_SIZE
    }

    TEST("FUZZ: Remove operations with oracle") {
        printf("  Testing remove operations...\n");

#define FUZZ_REMOVE_SIZE 2000
        uint64_t oracle[divCeil(FUZZ_REMOVE_SIZE, 64)] = {0};
        multiroar *r = multiroarBitNew();

        /* First add some bits */
        for (uint64_t i = 0; i < 500; i++) {
            uint64_t pos = rand() % FUZZ_REMOVE_SIZE;
            multiroarBitSet(r, pos);
            FUZZ_ORACLE_SET(oracle, pos);
        }

        /* Then remove some */
        for (uint64_t i = 0; i < 200; i++) {
            uint64_t pos = rand() % FUZZ_REMOVE_SIZE;
            bool expectedWasSet = FUZZ_ORACLE_GET(oracle, pos);
            bool actualWasSet = multiroarRemove(r, pos);

            if (expectedWasSet != actualWasSet) {
                ERR("Remove wasSet mismatch at pos %" PRIu64 "", pos);
            }
            FUZZ_ORACLE_CLEAR(oracle, pos);
        }

        /* Verify final state */
        uint64_t mismatches = 0;
        for (uint64_t i = 0; i < FUZZ_REMOVE_SIZE; i++) {
            if (FUZZ_ORACLE_GET(oracle, i) != multiroarBitGet(r, i)) {
                mismatches++;
            }
        }

        if (mismatches > 0) {
            ERR("Remove verification: %" PRIu64 " mismatches", mismatches);
        }

        printf("  add=500 remove=200 mismatches=%" PRIu64 "\n", mismatches);
        multiroarFree(r);
#undef FUZZ_REMOVE_SIZE
    }

    TEST("FUZZ: Set operations across multiple chunks") {
        printf("  Testing multi-chunk set operations...\n");

        multiroar *a = multiroarBitNew();
        multiroar *b = multiroarBitNew();

        /* Set bits across multiple chunks */
        for (uint64_t chunk = 0; chunk < 5; chunk++) {
            uint64_t base = chunk * 8192;
            for (uint64_t i = 0; i < 100; i++) {
                multiroarBitSet(a, base + i * 10);
                multiroarBitSet(b, base + i * 10 + 5);
            }
        }

        /* OR should combine both */
        multiroar *orResult = multiroarNewOr(a, b);
        uint64_t orCount = 0;
        for (uint64_t chunk = 0; chunk < 5; chunk++) {
            uint64_t base = chunk * 8192;
            for (uint64_t i = 0; i < 100; i++) {
                if (multiroarBitGet(orResult, base + i * 10)) {
                    orCount++;
                }
                if (multiroarBitGet(orResult, base + i * 10 + 5)) {
                    orCount++;
                }
            }
        }
        if (orCount != 1000) {
            ERR("multi-chunk OR: expected 1000 bits, got %" PRIu64 "", orCount);
        }

        /* AND should be empty (no overlap) */
        multiroar *andResult = multiroarNewAnd(a, b);
        uint64_t andCount = 0;
        for (uint64_t chunk = 0; chunk < 5; chunk++) {
            uint64_t base = chunk * 8192;
            for (uint64_t i = 0; i < 200; i++) {
                if (multiroarBitGet(andResult, base + i)) {
                    andCount++;
                }
            }
        }
        if (andCount != 0) {
            ERR("multi-chunk AND: expected 0 bits, got %" PRIu64 "", andCount);
        }

        printf("  multi-chunk: OR count=%" PRIu64 ", AND count=%" PRIu64 "\n",
               orCount, andCount);

        multiroarFree(a);
        multiroarFree(b);
        multiroarFree(orResult);
        multiroarFree(andResult);
    }

    TEST("set operations with dense bitmaps") {
        multiroar *a = multiroarBitNew();
        multiroar *b = multiroarBitNew();

        /* Fill to trigger FULL_BITMAP mode */
        for (uint64_t i = 0; i < 2000; i++) {
            multiroarBitSet(a, i);
        }
        for (uint64_t i = 1000; i < 3000; i++) {
            multiroarBitSet(b, i);
        }

        /* AND should give intersection: 1000-1999 */
        multiroar *andResult = multiroarNewAnd(a, b);
        uint64_t errors = 0;
        for (uint64_t i = 0; i < 1000; i++) {
            if (multiroarBitGet(andResult, i)) {
                errors++;
            }
        }
        for (uint64_t i = 1000; i < 2000; i++) {
            if (!multiroarBitGet(andResult, i)) {
                errors++;
            }
        }
        for (uint64_t i = 2000; i < 3000; i++) {
            if (multiroarBitGet(andResult, i)) {
                errors++;
            }
        }
        if (errors > 0) {
            ERR("dense AND: %" PRIu64 " bit errors", errors);
        }

        multiroarFree(a);
        multiroarFree(b);
        multiroarFree(andResult);
    }

    printf("\n=== All multiroar fuzz tests completed! ===\n\n");

    /* ========================================
     * BITCOUNT TESTS
     * ======================================== */
    printf("\n=== BITCOUNT TESTS ===\n\n");

    TEST("multiroarBitCount - empty roar") {
        multiroar *r = multiroarBitNew();
        uint64_t count = multiroarBitCount(r);
        if (count != 0) {
            ERR("Empty roar should have count=0, got %" PRIu64 "", count);
        }
        multiroarFree(r);
    }

    TEST("multiroarBitCount - single bit") {
        multiroar *r = multiroarBitNew();
        multiroarBitSet(r, 100);
        uint64_t count = multiroarBitCount(r);
        if (count != 1) {
            ERR("Single bit should have count=1, got %" PRIu64 "", count);
        }
        multiroarFree(r);
    }

    TEST("multiroarBitCount - sparse chunk") {
        multiroar *r = multiroarBitNew();
        for (uint64_t i = 0; i < 100; i++) {
            multiroarBitSet(r, i * 10);
        }
        uint64_t count = multiroarBitCount(r);
        if (count != 100) {
            ERR("Sparse chunk should have count=100, got %" PRIu64 "", count);
        }
        multiroarFree(r);
    }

    TEST("multiroarBitCount - dense chunk") {
        multiroar *r = multiroarBitNew();
        /* Set 1000 bits to trigger FULL_BITMAP */
        for (uint64_t i = 0; i < 1000; i++) {
            multiroarBitSet(r, i);
        }
        uint64_t count = multiroarBitCount(r);
        if (count != 1000) {
            ERR("Dense chunk should have count=1000, got %" PRIu64 "", count);
        }
        multiroarFree(r);
    }

    TEST("multiroarBitCount - full chunk (ALL_1)") {
        multiroar *r = multiroarBitNew();
        /* Set all 8192 bits in chunk */
        for (uint64_t i = 0; i < 8192; i++) {
            multiroarBitSet(r, i);
        }
        uint64_t count = multiroarBitCount(r);
        if (count != 8192) {
            ERR("Full chunk should have count=8192, got %" PRIu64 "", count);
        }
        multiroarFree(r);
    }

    TEST("multiroarBitCount - multiple chunks") {
        multiroar *r = multiroarBitNew();
        /* Chunk 0: 100 bits */
        for (uint64_t i = 0; i < 100; i++) {
            multiroarBitSet(r, i);
        }
        /* Chunk 1: 50 bits */
        for (uint64_t i = 0; i < 50; i++) {
            multiroarBitSet(r, 8192 + i);
        }
        /* Chunk 2: 200 bits */
        for (uint64_t i = 0; i < 200; i++) {
            multiroarBitSet(r, 16384 + i);
        }
        uint64_t count = multiroarBitCount(r);
        if (count != 350) {
            ERR("Multi-chunk should have count=350, got %" PRIu64 "", count);
        }
        multiroarFree(r);
    }

    TEST("FUZZ: bitcount correctness oracle") {
        printf("  Testing bitcount with oracle...\n");
        multiroar *r = multiroarBitNew();
        uint64_t expectedCount = 0;

        srand(33333);
        for (uint64_t i = 0; i < 10000; i++) {
            uint64_t pos = (uint64_t)rand() % 100000;
            if (!multiroarBitGet(r, pos)) {
                multiroarBitSet(r, pos);
                expectedCount++;
            }
        }

        uint64_t actualCount = multiroarBitCount(r);
        if (actualCount != expectedCount) {
            ERR("Bitcount mismatch: expected %" PRIu64 ", got %" PRIu64 "",
                expectedCount, actualCount);
        }

        printf("  verified %" PRIu64 " set bits\n", actualCount);
        multiroarFree(r);
    }

    /* ========================================
     * N-WAY OPERATIONS TESTS
     * ======================================== */
    printf("\n=== N-WAY OPERATIONS TESTS ===\n\n");

    TEST("multiroarAndN - 3 roars basic") {
        multiroar *r1 = multiroarBitNew();
        multiroar *r2 = multiroarBitNew();
        multiroar *r3 = multiroarBitNew();

        /* r1: 0-99 */
        for (uint64_t i = 0; i < 100; i++) {
            multiroarBitSet(r1, i);
        }
        /* r2: 50-149 */
        for (uint64_t i = 50; i < 150; i++) {
            multiroarBitSet(r2, i);
        }
        /* r3: 75-174 */
        for (uint64_t i = 75; i < 175; i++) {
            multiroarBitSet(r3, i);
        }

        multiroar *roars[] = {r1, r2, r3};
        multiroar *result = multiroarNewAndN(3, roars);

        /* Intersection: 75-99 = 25 bits */
        uint64_t count = 0;
        for (uint64_t i = 0; i < 200; i++) {
            if (multiroarBitGet(result, i)) {
                if (i < 75 || i >= 100) {
                    ERR("AndN: bit %" PRIu64 " should not be set", i);
                }
                count++;
            }
        }

        if (count != 25) {
            ERR("AndN: expected 25 bits, got %" PRIu64 "", count);
        }

        multiroarFree(r1);
        multiroarFree(r2);
        multiroarFree(r3);
        multiroarFree(result);
    }

    TEST("multiroarOrN - 3 roars basic") {
        multiroar *r1 = multiroarBitNew();
        multiroar *r2 = multiroarBitNew();
        multiroar *r3 = multiroarBitNew();

        multiroarBitSet(r1, 100);
        multiroarBitSet(r2, 200);
        multiroarBitSet(r3, 300);

        multiroar *roars[] = {r1, r2, r3};
        multiroar *result = multiroarNewOrN(3, roars);

        /* Union: all 3 bits */
        if (!multiroarBitGet(result, 100)) {
            ERRR("OrN: bit 100 should be set");
        }
        if (!multiroarBitGet(result, 200)) {
            ERRR("OrN: bit 200 should be set");
        }
        if (!multiroarBitGet(result, 300)) {
            ERRR("OrN: bit 300 should be set");
        }

        uint64_t count = multiroarBitCount(result);
        if (count != 3) {
            ERR("OrN: expected 3 bits, got %" PRIu64 "", count);
        }

        multiroarFree(r1);
        multiroarFree(r2);
        multiroarFree(r3);
        multiroarFree(result);
    }

    TEST("multiroarXorN - 3 roars basic") {
        multiroar *r1 = multiroarBitNew();
        multiroar *r2 = multiroarBitNew();
        multiroar *r3 = multiroarBitNew();

        multiroarBitSet(r1, 100);
        multiroarBitSet(r2, 100); /* Cancels out */
        multiroarBitSet(r3, 100); /* Sets again */

        multiroarBitSet(r1, 200);
        multiroarBitSet(r2, 200); /* Cancels out */

        multiroarBitSet(r3, 300);

        multiroar *roars[] = {r1, r2, r3};
        multiroar *result = multiroarNewXorN(3, roars);

        /* 100: 1^1^1 = 1 */
        /* 200: 1^1^0 = 0 */
        /* 300: 0^0^1 = 1 */
        if (!multiroarBitGet(result, 100)) {
            ERRR("XorN: bit 100 should be set");
        }
        if (multiroarBitGet(result, 200)) {
            ERRR("XorN: bit 200 should not be set");
        }
        if (!multiroarBitGet(result, 300)) {
            ERRR("XorN: bit 300 should be set");
        }

        multiroarFree(r1);
        multiroarFree(r2);
        multiroarFree(r3);
        multiroarFree(result);
    }

    TEST("N-way operations equivalence to chained binary") {
        multiroar *r1 = multiroarBitNew();
        multiroar *r2 = multiroarBitNew();
        multiroar *r3 = multiroarBitNew();
        multiroar *r4 = multiroarBitNew();

        srand(44444);
        for (uint64_t i = 0; i < 100; i++) {
            multiroarBitSet(r1, rand() % 500);
            multiroarBitSet(r2, rand() % 500);
            multiroarBitSet(r3, rand() % 500);
            multiroarBitSet(r4, rand() % 500);
        }

        multiroar *roars[] = {r1, r2, r3, r4};

        /* Test N-way AND vs chained binary */
        multiroar *nwayAnd = multiroarNewAndN(4, roars);
        multiroar *chainedAnd = multiroarDuplicate(r1);
        multiroarAnd(chainedAnd, r2);
        multiroarAnd(chainedAnd, r3);
        multiroarAnd(chainedAnd, r4);

        uint64_t nwayCount = multiroarBitCount(nwayAnd);
        uint64_t chainedCount = multiroarBitCount(chainedAnd);

        if (nwayCount != chainedCount) {
            ERR("N-way AND count=%" PRIu64 ", chained count=%" PRIu64 "",
                nwayCount, chainedCount);
        }

        /* Verify bit-by-bit */
        for (uint64_t i = 0; i < 500; i++) {
            bool nway = multiroarBitGet(nwayAnd, i);
            bool chained = multiroarBitGet(chainedAnd, i);
            if (nway != chained) {
                ERR("AND equivalence: bit %" PRIu64 " mismatch", i);
            }
        }

        /* Test N-way OR vs chained binary */
        multiroar *nwayOr = multiroarNewOrN(4, roars);
        multiroar *chainedOr = multiroarDuplicate(r1);
        multiroarOr(chainedOr, r2);
        multiroarOr(chainedOr, r3);
        multiroarOr(chainedOr, r4);

        nwayCount = multiroarBitCount(nwayOr);
        chainedCount = multiroarBitCount(chainedOr);

        if (nwayCount != chainedCount) {
            ERR("N-way OR count=%" PRIu64 ", chained count=%" PRIu64 "",
                nwayCount, chainedCount);
        }

        for (uint64_t i = 0; i < 500; i++) {
            bool nway = multiroarBitGet(nwayOr, i);
            bool chained = multiroarBitGet(chainedOr, i);
            if (nway != chained) {
                ERR("OR equivalence: bit %" PRIu64 " mismatch", i);
            }
        }

        multiroarFree(r1);
        multiroarFree(r2);
        multiroarFree(r3);
        multiroarFree(r4);
        multiroarFree(nwayAnd);
        multiroarFree(chainedAnd);
        multiroarFree(nwayOr);
        multiroarFree(chainedOr);
    }

    TEST("N-way operations with many inputs (N=10)") {
        const uint64_t n = 10;
        multiroar **roars = zmalloc(n * sizeof(multiroar *));

        srand(55555);
        for (uint64_t i = 0; i < n; i++) {
            roars[i] = multiroarBitNew();
            /* Each roar sets random bits */
            for (uint64_t j = 0; j < 50; j++) {
                multiroarBitSet(roars[i], rand() % 1000);
            }
        }

        /* Test N-way OR - should combine all */
        multiroar *orResult = multiroarNewOrN(n, roars);
        uint64_t orCount = multiroarBitCount(orResult);

        /* OR count should be > 0 */
        if (orCount == 0) {
            ERRR("N=10 OR: count should be > 0");
        }

        /* Test N-way AND - should be subset of any individual */
        multiroar *andResult = multiroarNewAndN(n, roars);
        uint64_t andCount = multiroarBitCount(andResult);

        for (uint64_t i = 0; i < n; i++) {
            uint64_t individualCount = multiroarBitCount(roars[i]);
            if (andCount > individualCount) {
                ERR("N=10 AND: intersection (%" PRIu64
                    ") > individual (%" PRIu64 ")",
                    andCount, individualCount);
            }
        }

        printf("  N=10: OR count=%" PRIu64 ", AND count=%" PRIu64 "\n", orCount,
               andCount);

        for (uint64_t i = 0; i < n; i++) {
            multiroarFree(roars[i]);
        }
        zfree(roars);
        multiroarFree(orResult);
        multiroarFree(andResult);
    }

    TEST("FUZZ: N-way operations with random data") {
        printf("  Fuzzing N-way operations...\n");

        srand(66666);
        const uint64_t trials = 100;
        const uint64_t maxN = 7;

        for (uint64_t trial = 0; trial < trials; trial++) {
            uint64_t n = 2 + (rand() % (maxN - 1)); /* 2 to 7 inputs */
            multiroar **roars = zmalloc(n * sizeof(multiroar *));

            /* Create random roars */
            for (uint64_t i = 0; i < n; i++) {
                roars[i] = multiroarBitNew();
                uint64_t numBits = rand() % 200;
                for (uint64_t j = 0; j < numBits; j++) {
                    multiroarBitSet(roars[i], rand() % 1000);
                }
            }

            /* Test N-way operations don't crash */
            multiroar *andResult = multiroarNewAndN(n, roars);
            multiroar *orResult = multiroarNewOrN(n, roars);
            multiroar *xorResult = multiroarNewXorN(n, roars);

            /* Basic sanity: AND count <= OR count */
            uint64_t andCount = multiroarBitCount(andResult);
            uint64_t orCount = multiroarBitCount(orResult);

            if (andCount > orCount) {
                ERR("Trial %" PRIu64 ": AND count %" PRIu64
                    " > OR count %" PRIu64 "",
                    trial, andCount, orCount);
            }

            /* Cleanup */
            for (uint64_t i = 0; i < n; i++) {
                multiroarFree(roars[i]);
            }
            zfree(roars);
            multiroarFree(andResult);
            multiroarFree(orResult);
            multiroarFree(xorResult);
        }

        printf("  completed %" PRIu64 " fuzz trials\n", trials);
    }

    TEST("N-way operations with empty roars") {
        multiroar *r1 = multiroarBitNew();
        multiroar *r2 = multiroarBitNew();
        multiroar *r3 = multiroarBitNew();

        multiroarBitSet(r2, 100); /* Only r2 has a bit */

        multiroar *roars[] = {r1, r2, r3};

        /* AND with empty = empty */
        multiroar *andResult = multiroarNewAndN(3, roars);
        uint64_t andCount = multiroarBitCount(andResult);
        if (andCount != 0) {
            ERR("AND with empty should be 0, got %" PRIu64 "", andCount);
        }

        /* OR with empty = union of non-empty */
        multiroar *orResult = multiroarNewOrN(3, roars);
        uint64_t orCount = multiroarBitCount(orResult);
        if (orCount != 1) {
            ERR("OR with empty should be 1, got %" PRIu64 "", orCount);
        }

        multiroarFree(r1);
        multiroarFree(r2);
        multiroarFree(r3);
        multiroarFree(andResult);
        multiroarFree(orResult);
    }

    TEST("bitcount + N-way integration") {
        multiroar *r1 = multiroarBitNew();
        multiroar *r2 = multiroarBitNew();
        multiroar *r3 = multiroarBitNew();

        /* Set known counts with proper overlaps for AND operation */
        for (uint64_t i = 0; i < 150; i++) {
            multiroarBitSet(r1, i);
        }
        for (uint64_t i = 50; i < 200; i++) {
            multiroarBitSet(r2, i);
        }
        for (uint64_t i = 100; i < 250; i++) {
            multiroarBitSet(r3, i);
        }

        /* Verify individual counts */
        if (multiroarBitCount(r1) != 150) {
            ERR("r1 count should be 150, got %" PRIu64 "",
                multiroarBitCount(r1));
        }
        if (multiroarBitCount(r2) != 150) {
            ERR("r2 count should be 150, got %" PRIu64 "",
                multiroarBitCount(r2));
        }
        if (multiroarBitCount(r3) != 150) {
            ERR("r3 count should be 150, got %" PRIu64 "",
                multiroarBitCount(r3));
        }

        multiroar *roars[] = {r1, r2, r3};

        /* AND: intersection [100..149] = 50 bits */
        multiroar *andResult = multiroarNewAndN(3, roars);
        uint64_t andCount = multiroarBitCount(andResult);
        if (andCount != 50) {
            ERR("AND count should be 50, got %" PRIu64 "", andCount);
        }

        /* OR: union [0..249] = 250 bits */
        multiroar *orResult = multiroarNewOrN(3, roars);
        uint64_t orCount = multiroarBitCount(orResult);
        if (orCount != 250) {
            ERR("OR count should be 250, got %" PRIu64 "", orCount);
        }

        printf("  Integration: AND=%" PRIu64 " OR=%" PRIu64 "\n", andCount,
               orCount);

        multiroarFree(r1);
        multiroarFree(r2);
        multiroarFree(r3);
        multiroarFree(andResult);
        multiroarFree(orResult);
    }

    printf("\n=== All new functionality tests completed! ===\n\n");

    /* ================================================================
     * ADVANCED FUZZING AND EDGE CASE TESTS
     * ================================================================ */

    printf("=== ADVANCED FUZZING TESTS ===\n\n");

    TEST("FUZZ: bitcount with all chunk types (1K iterations)") {
        srand(99999);
        for (int trial = 0; trial < 1000; trial++) {
            multiroar *r = multiroarBitNew();
            uint64_t expectedCount = 0;

            /* Set random number of bits to trigger different chunk types */
            int numBits = rand() % 10000;
            for (int i = 0; i < numBits; i++) {
                uint64_t pos = ((uint64_t)rand() * rand()) % 1000000;
                if (!multiroarBitGet(r, pos)) {
                    multiroarBitSet(r, pos);
                    expectedCount++;
                }
            }

            uint64_t actualCount = multiroarBitCount(r);
            if (actualCount != expectedCount) {
                ERR("FUZZ bitcount: expected %" PRIu64 ", got %" PRIu64
                    " (trial %d)",
                    expectedCount, actualCount, trial);
                break;
            }

            multiroarFree(r);
        }
        printf("  Completed 1K bitcount fuzz iterations\n");
    }

    TEST("FUZZ: N-way AND with random overlaps (1K iterations)") {
        srand(111111);
        for (int trial = 0; trial < 1000; trial++) {
            int n = 2 + (rand() % 8); /* 2-9 inputs */
            multiroar **roars = zmalloc(n * sizeof(multiroar *));

            for (int i = 0; i < n; i++) {
                roars[i] = multiroarBitNew();
                /* Set overlapping ranges */
                uint64_t start = (rand() % 500);
                uint64_t count = 50 + (rand() % 200);
                for (uint64_t j = start; j < start + count; j++) {
                    multiroarBitSet(roars[i], j);
                }
            }

            /* Test N-way vs chained binary */
            multiroar *nwayResult = multiroarNewAndN(n, roars);
            multiroar *chainedResult = multiroarDuplicate(roars[0]);
            for (int i = 1; i < n; i++) {
                multiroarAnd(chainedResult, roars[i]);
            }

            uint64_t nwayCount = multiroarBitCount(nwayResult);
            uint64_t chainedCount = multiroarBitCount(chainedResult);

            if (nwayCount != chainedCount) {
                ERR("FUZZ N-way AND: nway=%" PRIu64 " chained=%" PRIu64
                    " (trial %d, n=%d)",
                    nwayCount, chainedCount, trial, n);
                break;
            }

            /* Verify random positions */
            for (int i = 0; i < 100; i++) {
                uint64_t pos = rand() % 1000;
                bool nway = multiroarBitGet(nwayResult, pos);
                bool chained = multiroarBitGet(chainedResult, pos);
                if (nway != chained) {
                    ERR("FUZZ AND: bit %" PRIu64 " mismatch at trial %d", pos,
                        trial);
                    break;
                }
            }

            for (int i = 0; i < n; i++) {
                multiroarFree(roars[i]);
            }
            zfree(roars);
            multiroarFree(nwayResult);
            multiroarFree(chainedResult);
        }
        printf("  Completed 1K N-way AND fuzz iterations\n");
    }

    TEST("FUZZ: N-way OR with random overlaps (1K iterations)") {
        srand(222222);
        for (int trial = 0; trial < 1000; trial++) {
            int n = 2 + (rand() % 8);
            multiroar **roars = zmalloc(n * sizeof(multiroar *));

            for (int i = 0; i < n; i++) {
                roars[i] = multiroarBitNew();
                uint64_t start = (rand() % 500);
                uint64_t count = 50 + (rand() % 200);
                for (uint64_t j = start; j < start + count; j++) {
                    multiroarBitSet(roars[i], j);
                }
            }

            multiroar *nwayResult = multiroarNewOrN(n, roars);
            multiroar *chainedResult = multiroarDuplicate(roars[0]);
            for (int i = 1; i < n; i++) {
                multiroarOr(chainedResult, roars[i]);
            }

            uint64_t nwayCount = multiroarBitCount(nwayResult);
            uint64_t chainedCount = multiroarBitCount(chainedResult);

            if (nwayCount != chainedCount) {
                ERR("FUZZ N-way OR: nway=%" PRIu64 " chained=%" PRIu64
                    " (trial %d)",
                    nwayCount, chainedCount, trial);
                break;
            }

            for (int i = 0; i < n; i++) {
                multiroarFree(roars[i]);
            }
            zfree(roars);
            multiroarFree(nwayResult);
            multiroarFree(chainedResult);
        }
        printf("  Completed 1K N-way OR fuzz iterations\n");
    }

    TEST("FUZZ: N-way XOR with random overlaps (1K iterations)") {
        srand(333333);
        for (int trial = 0; trial < 1000; trial++) {
            int n = 2 + (rand() % 8);
            multiroar **roars = zmalloc(n * sizeof(multiroar *));

            for (int i = 0; i < n; i++) {
                roars[i] = multiroarBitNew();
                uint64_t start = (rand() % 500);
                uint64_t count = 50 + (rand() % 200);
                for (uint64_t j = start; j < start + count; j++) {
                    multiroarBitSet(roars[i], j);
                }
            }

            multiroar *nwayResult = multiroarNewXorN(n, roars);
            multiroar *chainedResult = multiroarDuplicate(roars[0]);
            for (int i = 1; i < n; i++) {
                multiroarXor(chainedResult, roars[i]);
            }

            uint64_t nwayCount = multiroarBitCount(nwayResult);
            uint64_t chainedCount = multiroarBitCount(chainedResult);

            if (nwayCount != chainedCount) {
                ERR("FUZZ N-way XOR: nway=%" PRIu64 " chained=%" PRIu64
                    " (trial %d)",
                    nwayCount, chainedCount, trial);
                break;
            }

            for (int i = 0; i < n; i++) {
                multiroarFree(roars[i]);
            }
            zfree(roars);
            multiroarFree(nwayResult);
            multiroarFree(chainedResult);
        }
        printf("  Completed 1K N-way XOR fuzz iterations\n");
    }

    TEST("EDGE: bitcount with chunk type transitions") {
        multiroar *r = multiroarBitNew();
        uint64_t count = 0;

        /* Start with UNDER_FULL (sparse) */
        for (uint64_t i = 0; i < 100; i++) {
            multiroarBitSet(r, i * 100);
            count++;
        }
        if (multiroarBitCount(r) != count) {
            ERR("Sparse chunk bitcount: expected %" PRIu64 ", got %" PRIu64 "",
                count, multiroarBitCount(r));
        }

        /* Transition to FULL_BITMAP (dense) */
        for (uint64_t i = 0; i < 2000; i++) {
            if (!multiroarBitGet(r, i)) {
                multiroarBitSet(r, i);
                count++;
            }
        }
        if (multiroarBitCount(r) != count) {
            ERR("Dense chunk bitcount: expected %" PRIu64 ", got %" PRIu64 "",
                count, multiroarBitCount(r));
        }

        /* Transition to OVER_FULL (inverted) */
        for (uint64_t i = 0; i < 8000; i++) {
            if (!multiroarBitGet(r, i)) {
                multiroarBitSet(r, i);
                count++;
            }
        }
        if (multiroarBitCount(r) != count) {
            ERR("Inverted chunk bitcount: expected %" PRIu64 ", got %" PRIu64
                "",
                count, multiroarBitCount(r));
        }

        /* Transition to ALL_1 (all bits set) */
        for (uint64_t i = 0; i < 8192; i++) {
            if (!multiroarBitGet(r, i)) {
                multiroarBitSet(r, i);
                count++;
            }
        }
        if (multiroarBitCount(r) != count) {
            ERR("All-ones chunk bitcount: expected %" PRIu64 ", got %" PRIu64
                "",
                count, multiroarBitCount(r));
        }

        multiroarFree(r);
    }

    TEST("EDGE: N-way operations with very large N (N=100)") {
        const int n = 100;
        multiroar **roars = zmalloc(n * sizeof(multiroar *));

        srand(444444);
        for (int i = 0; i < n; i++) {
            roars[i] = multiroarBitNew();
            /* Each sets bit at position i to guarantee some overlap */
            multiroarBitSet(roars[i], i);
            /* Plus some random bits */
            for (int j = 0; j < 10; j++) {
                multiroarBitSet(roars[i], rand() % 1000);
            }
        }

        multiroar *orResult = multiroarNewOrN(n, roars);
        uint64_t orCount = multiroarBitCount(orResult);

        /* OR should have at least N bits (positions 0..N-1) */
        if (orCount < (uint64_t)n) {
            ERR("N=100 OR: count %" PRIu64 " should be >= %d", orCount, n);
        }

        multiroar *andResult = multiroarNewAndN(n, roars);
        uint64_t andCount = multiroarBitCount(andResult);

        /* AND should be small (likely 0) since each roar has unique bits */
        if (andCount > 50) {
            ERR("N=100 AND: count %" PRIu64 " unexpectedly large", andCount);
        }

        printf("  N=100: OR=%" PRIu64 " AND=%" PRIu64 "\n", orCount, andCount);

        for (int i = 0; i < n; i++) {
            multiroarFree(roars[i]);
        }
        zfree(roars);
        multiroarFree(orResult);
        multiroarFree(andResult);
    }

    TEST("EDGE: Multi-chunk operations across boundaries") {
        multiroar *r1 = multiroarBitNew();
        multiroar *r2 = multiroarBitNew();
        multiroar *r3 = multiroarBitNew();

        /* Set bits in different chunks (chunks are 8192 bits each) */
        /* Chunk 0: bits 0-8191 - with proper overlaps */
        for (uint64_t i = 0; i < 2000; i++) {
            multiroarBitSet(r1, i);
        }
        for (uint64_t i = 500; i < 2500; i++) {
            multiroarBitSet(r2, i);
        }
        for (uint64_t i = 1000; i < 3000; i++) {
            multiroarBitSet(r3, i);
        }

        /* Chunk 1: bits 8192-16383 - with proper overlaps */
        for (uint64_t i = 8192; i < 10192; i++) {
            multiroarBitSet(r1, i);
        }
        for (uint64_t i = 8692; i < 10692; i++) {
            multiroarBitSet(r2, i);
        }
        for (uint64_t i = 9192; i < 11192; i++) {
            multiroarBitSet(r3, i);
        }

        /* Chunk 10: bits 81920-90111 - with proper overlaps */
        for (uint64_t i = 81920; i < 83920; i++) {
            multiroarBitSet(r1, i);
        }
        for (uint64_t i = 82420; i < 84420; i++) {
            multiroarBitSet(r2, i);
        }
        for (uint64_t i = 82920; i < 84920; i++) {
            multiroarBitSet(r3, i);
        }

        multiroar *roars[] = {r1, r2, r3};

        /* Test AND across chunks - should have overlaps */
        multiroar *andResult = multiroarNewAndN(3, roars);
        uint64_t andCount = multiroarBitCount(andResult);

        /* Should have overlaps: chunk0=[1000..1999], chunk1=[9192..10191],
         * chunk10=[82920..83919] */
        uint64_t expectedAnd = 1000 + 1000 + 1000; /* 3000 bits total overlap */
        if (andCount != expectedAnd) {
            ERR("Multi-chunk AND: expected %" PRIu64 ", got %" PRIu64 "",
                expectedAnd, andCount);
        }

        /* Test OR across chunks */
        multiroar *orResult = multiroarNewOrN(3, roars);
        uint64_t orCount = multiroarBitCount(orResult);

        /* OR should be union of all ranges */
        /* chunk0: [0..2999], chunk1: [8192..11191], chunk10: [81920..84919] */
        uint64_t expectedOr = 3000 + 3000 + 3000; /* 9000 bits total */
        if (orCount != expectedOr) {
            ERR("Multi-chunk OR: expected %" PRIu64 ", got %" PRIu64 "",
                expectedOr, orCount);
        }

        printf("  Multi-chunk: AND=%" PRIu64 " OR=%" PRIu64 "\n", andCount,
               orCount);

        multiroarFree(r1);
        multiroarFree(r2);
        multiroarFree(r3);
        multiroarFree(andResult);
        multiroarFree(orResult);
    }

    TEST("EDGE: Operations with all chunk types mixed") {
        multiroar *sparse = multiroarBitNew();
        multiroar *dense = multiroarBitNew();
        multiroar *inverted = multiroarBitNew();
        multiroar *full = multiroarBitNew();

        /* Sparse chunk (< 631 bits) */
        for (uint64_t i = 0; i < 300; i++) {
            multiroarBitSet(sparse, i * 10);
        }

        /* Dense chunk (631-7561 bits) */
        for (uint64_t i = 0; i < 2000; i++) {
            multiroarBitSet(dense, i);
        }

        /* Inverted chunk (> 7561 bits) */
        for (uint64_t i = 0; i < 8000; i++) {
            multiroarBitSet(inverted, i);
        }

        /* Full chunk (all 8192 bits) */
        for (uint64_t i = 0; i < 8192; i++) {
            multiroarBitSet(full, i);
        }

        /* Verify counts */
        if (multiroarBitCount(sparse) != 300) {
            ERR("Sparse count: expected 300, got %" PRIu64 "",
                multiroarBitCount(sparse));
        }
        if (multiroarBitCount(dense) != 2000) {
            ERR("Dense count: expected 2000, got %" PRIu64 "",
                multiroarBitCount(dense));
        }
        if (multiroarBitCount(inverted) != 8000) {
            ERR("Inverted count: expected 8000, got %" PRIu64 "",
                multiroarBitCount(inverted));
        }
        if (multiroarBitCount(full) != 8192) {
            ERR("Full count: expected 8192, got %" PRIu64 "",
                multiroarBitCount(full));
        }

        multiroar *roars[] = {sparse, dense, inverted, full};

        /* AND of all types - should be sparse positions only */
        multiroar *andResult = multiroarNewAndN(4, roars);
        uint64_t andCount = multiroarBitCount(andResult);
        /* All have bits [0, 10, 20, ... 290] in common = ~30 bits */
        if (andCount == 0 || andCount > 300) {
            ERR("Mixed types AND: count %" PRIu64 " unexpected", andCount);
        }

        /* OR of all types - should be full chunk */
        multiroar *orResult = multiroarNewOrN(4, roars);
        uint64_t orCount = multiroarBitCount(orResult);
        if (orCount != 8192) {
            ERR("Mixed types OR: expected 8192, got %" PRIu64 "", orCount);
        }

        printf("  Mixed chunk types: AND=%" PRIu64 " OR=%" PRIu64 "\n",
               andCount, orCount);

        multiroarFree(sparse);
        multiroarFree(dense);
        multiroarFree(inverted);
        multiroarFree(full);
        multiroarFree(andResult);
        multiroarFree(orResult);
    }

    TEST("EDGE: Bitcount with very large positions") {
        multiroar *r = multiroarBitNew();

        /* Set bits at very large positions */
        uint64_t positions[] = {
            1000000,        10000000,        100000000,       1000000000,
            10000000000ULL, 100000000000ULL, 1000000000000ULL};

        for (uint64_t i = 0; i < sizeof(positions) / sizeof(positions[0]);
             i++) {
            multiroarBitSet(r, positions[i]);
        }

        uint64_t count = multiroarBitCount(r);
        if (count != sizeof(positions) / sizeof(positions[0])) {
            ERR("Large positions bitcount: expected %zu, got %" PRIu64,
                sizeof(positions) / sizeof(positions[0]), count);
        }

        multiroarFree(r);
    }

    TEST("EDGE: N=2 equivalence to binary operations") {
        multiroar *r1 = multiroarBitNew();
        multiroar *r2 = multiroarBitNew();

        srand(555555);
        for (int i = 0; i < 1000; i++) {
            multiroarBitSet(r1, rand() % 5000);
            multiroarBitSet(r2, rand() % 5000);
        }

        multiroar *roars[] = {r1, r2};

        /* Test N=2 AND vs binary AND */
        multiroar *nwayAnd = multiroarNewAndN(2, roars);
        multiroar *binaryAnd = multiroarNewAnd(r1, r2);

        uint64_t nwayCount = multiroarBitCount(nwayAnd);
        uint64_t binaryCount = multiroarBitCount(binaryAnd);

        if (nwayCount != binaryCount) {
            ERR("N=2 AND: nway=%" PRIu64 " binary=%" PRIu64 "", nwayCount,
                binaryCount);
        }

        /* Verify bit-by-bit */
        for (uint64_t i = 0; i < 5000; i++) {
            if (multiroarBitGet(nwayAnd, i) != multiroarBitGet(binaryAnd, i)) {
                ERR("N=2 AND: bit %" PRIu64 " mismatch", i);
                break;
            }
        }

        /* Test N=2 OR vs binary OR */
        multiroar *nwayOr = multiroarNewOrN(2, roars);
        multiroar *binaryOr = multiroarNewOr(r1, r2);

        nwayCount = multiroarBitCount(nwayOr);
        binaryCount = multiroarBitCount(binaryOr);

        if (nwayCount != binaryCount) {
            ERR("N=2 OR: nway=%" PRIu64 " binary=%" PRIu64 "", nwayCount,
                binaryCount);
        }

        /* Test N=2 XOR vs binary XOR */
        multiroar *nwayXor = multiroarNewXorN(2, roars);
        multiroar *binaryXor = multiroarNewXor(r1, r2);

        nwayCount = multiroarBitCount(nwayXor);
        binaryCount = multiroarBitCount(binaryXor);

        if (nwayCount != binaryCount) {
            ERR("N=2 XOR: nway=%" PRIu64 " binary=%" PRIu64 "", nwayCount,
                binaryCount);
        }

        multiroarFree(r1);
        multiroarFree(r2);
        multiroarFree(nwayAnd);
        multiroarFree(binaryAnd);
        multiroarFree(nwayOr);
        multiroarFree(binaryOr);
        multiroarFree(nwayXor);
        multiroarFree(binaryXor);
    }

    TEST("PERF: Bitcount performance across different densities") {
        const int iterations = 10000;

        /* Sparse */
        multiroar *sparse = multiroarBitNew();
        for (int i = 0; i < 100; i++) {
            multiroarBitSet(sparse, i * 100);
        }

        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);
        for (int i = 0; i < iterations; i++) {
            volatile uint64_t count = multiroarBitCount(sparse);
            (void)count;
        }
        clock_gettime(CLOCK_MONOTONIC, &end);
        double sparseNs =
            (end.tv_sec - start.tv_sec) * 1e9 + (end.tv_nsec - start.tv_nsec);
        sparseNs /= iterations;

        /* Dense */
        multiroar *dense = multiroarBitNew();
        for (int i = 0; i < 5000; i++) {
            multiroarBitSet(dense, i);
        }

        clock_gettime(CLOCK_MONOTONIC, &start);
        for (int i = 0; i < iterations; i++) {
            volatile uint64_t count = multiroarBitCount(dense);
            (void)count;
        }
        clock_gettime(CLOCK_MONOTONIC, &end);
        double denseNs =
            (end.tv_sec - start.tv_sec) * 1e9 + (end.tv_nsec - start.tv_nsec);
        denseNs /= iterations;

        printf("  Sparse: %.1f ns/op, Dense: %.1f ns/op\n", sparseNs, denseNs);

        multiroarFree(sparse);
        multiroarFree(dense);
    }

    TEST("PERF: N-way operations vs chained binary (N=10)") {
        const int n = 10;
        multiroar **roars = zmalloc(n * sizeof(multiroar *));

        srand(666666);
        for (int i = 0; i < n; i++) {
            roars[i] = multiroarBitNew();
            for (int j = 0; j < 500; j++) {
                multiroarBitSet(roars[i], rand() % 5000);
            }
        }

        /* N-way AND performance */
        struct timespec start, end;
        const int iterations = 1000;

        clock_gettime(CLOCK_MONOTONIC, &start);
        for (int i = 0; i < iterations; i++) {
            multiroar *result = multiroarNewAndN(n, roars);
            multiroarFree(result);
        }
        clock_gettime(CLOCK_MONOTONIC, &end);
        double nwayNs =
            (end.tv_sec - start.tv_sec) * 1e9 + (end.tv_nsec - start.tv_nsec);
        nwayNs /= iterations;

        /* Chained binary AND performance */
        clock_gettime(CLOCK_MONOTONIC, &start);
        for (int i = 0; i < iterations; i++) {
            multiroar *result = multiroarDuplicate(roars[0]);
            for (int j = 1; j < n; j++) {
                multiroarAnd(result, roars[j]);
            }
            multiroarFree(result);
        }
        clock_gettime(CLOCK_MONOTONIC, &end);
        double chainedNs =
            (end.tv_sec - start.tv_sec) * 1e9 + (end.tv_nsec - start.tv_nsec);
        chainedNs /= iterations;

        printf("  N=10 AND: N-way=%.0f ns, Chained=%.0f ns, Ratio=%.2fx\n",
               nwayNs, chainedNs, chainedNs / nwayNs);

        for (int i = 0; i < n; i++) {
            multiroarFree(roars[i]);
        }
        zfree(roars);
    }

    printf("\n=== All advanced fuzzing tests completed! ===\n\n");

    /* ====================================================================
     * Min/Max/IsEmpty/Comparison Operations Tests
     * ==================================================================== */

    TEST("multiroarMin/Max - empty bitmap") {
        multiroar *r = multiroarBitNew();
        uint64_t position;

        if (multiroarMin(r, &position)) {
            ERRR("Min should return false for empty bitmap");
        }
        if (multiroarMax(r, &position)) {
            ERRR("Max should return false for empty bitmap");
        }

        multiroarFree(r);
    }

    TEST("multiroarIsEmpty - empty and non-empty") {
        multiroar *r = multiroarBitNew();

        if (!multiroarIsEmpty(r)) {
            ERRR("Newly created bitmap should be empty");
        }

        multiroarBitSet(r, 42);
        if (multiroarIsEmpty(r)) {
            ERRR("Bitmap with bit set should not be empty");
        }

        multiroarRemove(r, 42);
        if (!multiroarIsEmpty(r)) {
            ERRR("Bitmap after removing all bits should be empty");
        }

        multiroarFree(r);
    }

    TEST("multiroarMin/Max - single bit at various positions") {
        uint64_t testPositions[] = {0,    1,    100,    8191,
                                    8192, 8193, 100000, 1000000};

        for (uint64_t i = 0;
             i < sizeof(testPositions) / sizeof(testPositions[0]); i++) {
            multiroar *r = multiroarBitNew();
            uint64_t pos = testPositions[i];

            multiroarBitSet(r, pos);

            uint64_t minPos, maxPos;
            if (!multiroarMin(r, &minPos) || minPos != pos) {
                ERR("Min position mismatch: expected %" PRIu64 ", got %" PRIu64
                    "",
                    pos, minPos);
            }
            if (!multiroarMax(r, &maxPos) || maxPos != pos) {
                ERR("Max position mismatch: expected %" PRIu64 ", got %" PRIu64
                    "",
                    pos, maxPos);
            }

            multiroarFree(r);
        }
    }

    TEST("multiroarMin/Max - sparse chunk (UNDER_FULL)") {
        multiroar *r = multiroarBitNew();

        /* Set bits at positions that will stay sparse (<631 bits per chunk) */
        multiroarBitSet(r, 100);
        multiroarBitSet(r, 500);
        multiroarBitSet(r, 1000);

        uint64_t minPos, maxPos;
        if (!multiroarMin(r, &minPos) || minPos != 100) {
            ERR("Min for sparse chunk: expected 100, got %" PRIu64 "", minPos);
        }
        if (!multiroarMax(r, &maxPos) || maxPos != 1000) {
            ERR("Max for sparse chunk: expected 1000, got %" PRIu64 "", maxPos);
        }

        multiroarFree(r);
    }

    TEST("multiroarMin/Max - dense chunk (FULL_BITMAP)") {
        multiroar *r = multiroarBitNew();

        /* Set many bits to trigger dense encoding (631-7561 bits) */
        for (uint64_t i = 100; i < 1100; i++) {
            multiroarBitSet(r, i);
        }

        uint64_t minPos, maxPos;
        if (!multiroarMin(r, &minPos) || minPos != 100) {
            ERR("Min for dense chunk: expected 100, got %" PRIu64 "", minPos);
        }
        if (!multiroarMax(r, &maxPos) || maxPos != 1099) {
            ERR("Max for dense chunk: expected 1099, got %" PRIu64 "", maxPos);
        }

        multiroarFree(r);
    }

    TEST("multiroarMin/Max - inverted chunk (OVER_FULL)") {
        multiroar *r = multiroarBitNew();

        /* Set most bits in a chunk to trigger inverted encoding (>7561 bits) */
        for (uint64_t i = 10; i < 8190; i++) {
            multiroarBitSet(r, i);
        }

        uint64_t minPos, maxPos;
        if (!multiroarMin(r, &minPos) || minPos != 10) {
            ERR("Min for inverted chunk: expected 10, got %" PRIu64 "", minPos);
        }
        if (!multiroarMax(r, &maxPos) || maxPos != 8189) {
            ERR("Max for inverted chunk: expected 8189, got %" PRIu64 "",
                maxPos);
        }

        multiroarFree(r);
    }

    TEST("multiroarMin/Max - full chunk (ALL_1)") {
        multiroar *r = multiroarBitNew();

        /* Set all bits in a chunk */
        for (uint64_t i = 0; i < 8192; i++) {
            multiroarBitSet(r, i);
        }

        uint64_t minPos, maxPos;
        if (!multiroarMin(r, &minPos) || minPos != 0) {
            ERR("Min for full chunk: expected 0, got %" PRIu64 "", minPos);
        }
        if (!multiroarMax(r, &maxPos) || maxPos != 8191) {
            ERR("Max for full chunk: expected 8191, got %" PRIu64 "", maxPos);
        }

        multiroarFree(r);
    }

    TEST("multiroarMin/Max - multiple chunks") {
        multiroar *r = multiroarBitNew();

        /* Set bits across multiple chunks */
        multiroarBitSet(r, 100);     /* Chunk 0 */
        multiroarBitSet(r, 10000);   /* Chunk 1 */
        multiroarBitSet(r, 100000);  /* Chunk 12 */
        multiroarBitSet(r, 1000000); /* Chunk 122 */

        uint64_t minPos, maxPos;
        if (!multiroarMin(r, &minPos) || minPos != 100) {
            ERR("Min for multi-chunk: expected 100, got %" PRIu64 "", minPos);
        }
        if (!multiroarMax(r, &maxPos) || maxPos != 1000000) {
            ERR("Max for multi-chunk: expected 1000000, got %" PRIu64 "",
                maxPos);
        }

        multiroarFree(r);
    }

    TEST("multiroarMin/Max - chunk boundaries") {
        multiroar *r = multiroarBitNew();

        /* Test at chunk boundaries (8192 bits per chunk) */
        multiroarBitSet(r, 0);     /* First bit of chunk 0 */
        multiroarBitSet(r, 8191);  /* Last bit of chunk 0 */
        multiroarBitSet(r, 8192);  /* First bit of chunk 1 */
        multiroarBitSet(r, 16383); /* Last bit of chunk 1 */

        uint64_t minPos, maxPos;
        if (!multiroarMin(r, &minPos) || minPos != 0) {
            ERR("Min at chunk boundary: expected 0, got %" PRIu64 "", minPos);
        }
        if (!multiroarMax(r, &maxPos) || maxPos != 16383) {
            ERR("Max at chunk boundary: expected 16383, got %" PRIu64 "",
                maxPos);
        }

        multiroarFree(r);
    }

    TEST("multiroarIntersects - empty and disjoint") {
        multiroar *r1 = multiroarBitNew();
        multiroar *r2 = multiroarBitNew();

        /* Both empty */
        if (multiroarIntersects(r1, r2)) {
            ERRR("Empty bitmaps should not intersect");
        }

        /* One empty */
        multiroarBitSet(r1, 100);
        if (multiroarIntersects(r1, r2)) {
            ERRR("One empty bitmap should not intersect");
        }

        /* Disjoint */
        multiroarBitSet(r2, 200);
        if (multiroarIntersects(r1, r2)) {
            ERRR("Disjoint bitmaps should not intersect");
        }

        multiroarFree(r1);
        multiroarFree(r2);
    }

    TEST("multiroarIntersects - overlapping") {
        multiroar *r1 = multiroarBitNew();
        multiroar *r2 = multiroarBitNew();

        /* Same chunk, overlapping bits */
        multiroarBitSet(r1, 100);
        multiroarBitSet(r1, 200);
        multiroarBitSet(r2, 200);
        multiroarBitSet(r2, 300);

        if (!multiroarIntersects(r1, r2)) {
            ERRR("Overlapping bitmaps should intersect");
        }

        multiroarFree(r1);
        multiroarFree(r2);
    }

    TEST("multiroarIntersects - different chunks") {
        multiroar *r1 = multiroarBitNew();
        multiroar *r2 = multiroarBitNew();

        /* Different chunks, no overlap */
        multiroarBitSet(r1, 100);
        multiroarBitSet(r2, 10000);

        if (multiroarIntersects(r1, r2)) {
            ERRR("Bits in different chunks should not intersect");
        }

        /* Same bit in different chunk */
        multiroarBitSet(r1, 10000);

        if (!multiroarIntersects(r1, r2)) {
            ERRR("Same bit in same chunk should intersect");
        }

        multiroarFree(r1);
        multiroarFree(r2);
    }

    TEST("multiroarIsSubset - empty sets") {
        multiroar *r1 = multiroarBitNew();
        multiroar *r2 = multiroarBitNew();

        /* Empty set is subset of empty set */
        if (!multiroarIsSubset(r1, r2)) {
            ERRR("Empty set should be subset of empty set");
        }

        /* Empty set is subset of any set */
        multiroarBitSet(r2, 100);
        if (!multiroarIsSubset(r1, r2)) {
            ERRR("Empty set should be subset of non-empty set");
        }

        /* Non-empty set is not subset of empty set */
        if (multiroarIsSubset(r2, r1)) {
            ERRR("Non-empty set should not be subset of empty set");
        }

        multiroarFree(r1);
        multiroarFree(r2);
    }

    TEST("multiroarIsSubset - equal sets") {
        multiroar *r1 = multiroarBitNew();
        multiroar *r2 = multiroarBitNew();

        multiroarBitSet(r1, 100);
        multiroarBitSet(r1, 200);
        multiroarBitSet(r2, 100);
        multiroarBitSet(r2, 200);

        /* Equal sets are subsets of each other */
        if (!multiroarIsSubset(r1, r2)) {
            ERRR("Equal sets should be subsets of each other (r1 ⊆ r2)");
        }
        if (!multiroarIsSubset(r2, r1)) {
            ERRR("Equal sets should be subsets of each other (r2 ⊆ r1)");
        }

        multiroarFree(r1);
        multiroarFree(r2);
    }

    TEST("multiroarIsSubset - proper subset") {
        multiroar *r1 = multiroarBitNew();
        multiroar *r2 = multiroarBitNew();

        multiroarBitSet(r1, 100);
        multiroarBitSet(r2, 100);
        multiroarBitSet(r2, 200);

        /* r1 ⊂ r2 (proper subset) */
        if (!multiroarIsSubset(r1, r2)) {
            ERRR("r1 should be subset of r2");
        }

        /* r2 ⊄ r1 (not a subset) */
        if (multiroarIsSubset(r2, r1)) {
            ERRR("r2 should not be subset of r1");
        }

        multiroarFree(r1);
        multiroarFree(r2);
    }

    TEST("multiroarIsSubset - non-subset") {
        multiroar *r1 = multiroarBitNew();
        multiroar *r2 = multiroarBitNew();

        multiroarBitSet(r1, 100);
        multiroarBitSet(r1, 300);
        multiroarBitSet(r2, 200);
        multiroarBitSet(r2, 400);

        /* Disjoint sets are not subsets */
        if (multiroarIsSubset(r1, r2)) {
            ERRR("Disjoint r1 should not be subset of r2");
        }
        if (multiroarIsSubset(r2, r1)) {
            ERRR("Disjoint r2 should not be subset of r1");
        }

        multiroarFree(r1);
        multiroarFree(r2);
    }

    TEST("multiroarEquals - empty bitmaps") {
        multiroar *r1 = multiroarBitNew();
        multiroar *r2 = multiroarBitNew();

        if (!multiroarEquals(r1, r2)) {
            ERRR("Empty bitmaps should be equal");
        }

        multiroarFree(r1);
        multiroarFree(r2);
    }

    TEST("multiroarEquals - identical bitmaps") {
        multiroar *r1 = multiroarBitNew();
        multiroar *r2 = multiroarBitNew();

        multiroarBitSet(r1, 100);
        multiroarBitSet(r1, 200);
        multiroarBitSet(r1, 10000);

        multiroarBitSet(r2, 100);
        multiroarBitSet(r2, 200);
        multiroarBitSet(r2, 10000);

        if (!multiroarEquals(r1, r2)) {
            ERRR("Identical bitmaps should be equal");
        }

        multiroarFree(r1);
        multiroarFree(r2);
    }

    TEST("multiroarEquals - different sizes") {
        multiroar *r1 = multiroarBitNew();
        multiroar *r2 = multiroarBitNew();

        multiroarBitSet(r1, 100);
        multiroarBitSet(r2, 100);
        multiroarBitSet(r2, 200);

        if (multiroarEquals(r1, r2)) {
            ERRR("Bitmaps with different sizes should not be equal");
        }

        multiroarFree(r1);
        multiroarFree(r2);
    }

    TEST("multiroarEquals - different positions") {
        multiroar *r1 = multiroarBitNew();
        multiroar *r2 = multiroarBitNew();

        multiroarBitSet(r1, 100);
        multiroarBitSet(r2, 200);

        if (multiroarEquals(r1, r2)) {
            ERRR("Bitmaps with different positions should not be equal");
        }

        multiroarFree(r1);
        multiroarFree(r2);
    }

    TEST("multiroarEquals - different chunks") {
        multiroar *r1 = multiroarBitNew();
        multiroar *r2 = multiroarBitNew();

        multiroarBitSet(r1, 100);
        multiroarBitSet(r2, 10000);

        if (multiroarEquals(r1, r2)) {
            ERRR("Bitmaps with bits in different chunks should not be equal");
        }

        multiroarFree(r1);
        multiroarFree(r2);
    }

    TEST_DESC("min/max/comparison fuzzing - %d random operations", 1000) {
        for (int trial = 0; trial < 1000; trial++) {
            multiroar *r1 = multiroarBitNew();
            multiroar *r2 = multiroarBitNew();

            /* Create random bitmaps */
            int numBits1 = rand() % 500;
            int numBits2 = rand() % 500;

            uint64_t min1 = UINT64_MAX, max1 = 0;
            uint64_t min2 = UINT64_MAX, max2 = 0;

            for (int i = 0; i < numBits1; i++) {
                uint64_t pos = ((uint64_t)rand() * rand()) % 100000;
                multiroarBitSet(r1, pos);
                if (pos < min1) {
                    min1 = pos;
                }
                if (pos > max1) {
                    max1 = pos;
                }
            }

            for (int i = 0; i < numBits2; i++) {
                uint64_t pos = ((uint64_t)rand() * rand()) % 100000;
                multiroarBitSet(r2, pos);
                if (pos < min2) {
                    min2 = pos;
                }
                if (pos > max2) {
                    max2 = pos;
                }
            }

            /* Verify Min/Max */
            if (numBits1 > 0) {
                uint64_t actualMin, actualMax;
                if (!multiroarMin(r1, &actualMin)) {
                    ERRR("Min should succeed for non-empty bitmap");
                }
                if (!multiroarMax(r1, &actualMax)) {
                    ERRR("Max should succeed for non-empty bitmap");
                }

                /* Verify min/max are actually set */
                if (!multiroarBitGet(r1, actualMin)) {
                    ERR("Min position %" PRIu64 " should be set", actualMin);
                }
                if (!multiroarBitGet(r1, actualMax)) {
                    ERR("Max position %" PRIu64 " should be set", actualMax);
                }
            } else {
                uint64_t pos;
                if (multiroarMin(r1, &pos)) {
                    ERRR("Min should fail for empty bitmap");
                }
                if (multiroarMax(r1, &pos)) {
                    ERRR("Max should fail for empty bitmap");
                }
            }

            /* Verify IsEmpty */
            bool shouldBeEmpty1 = (numBits1 == 0);
            if (multiroarIsEmpty(r1) != shouldBeEmpty1) {
                ERR("IsEmpty mismatch: expected %d", shouldBeEmpty1);
            }

            /* Verify Intersects */
            bool hasIntersection = false;
            for (uint64_t pos = 0; pos < 100000; pos++) {
                if (multiroarBitGet(r1, pos) && multiroarBitGet(r2, pos)) {
                    hasIntersection = true;
                    break;
                }
            }
            if (multiroarIntersects(r1, r2) != hasIntersection) {
                ERR("Intersects mismatch: expected %d", hasIntersection);
            }

            /* Verify Equals */
            bool shouldBeEqual = true;
            if (multiroarBitCount(r1) != multiroarBitCount(r2)) {
                shouldBeEqual = false;
            } else {
                for (uint64_t pos = 0; pos < 100000; pos++) {
                    if (multiroarBitGet(r1, pos) != multiroarBitGet(r2, pos)) {
                        shouldBeEqual = false;
                        break;
                    }
                }
            }
            if (multiroarEquals(r1, r2) != shouldBeEqual) {
                ERR("Equals mismatch: expected %d", shouldBeEqual);
            }

            multiroarFree(r1);
            multiroarFree(r2);
        }
    }

    printf("\n=== Min/Max/Comparison tests completed! ===\n\n");

    /* ====================================================================
     * Rank/Select Operations Tests
     * ==================================================================== */

    TEST("multiroarRank - empty bitmap") {
        multiroar *r = multiroarBitNew();

        if (multiroarRank(r, 0) != 0) {
            ERRR("Rank at 0 for empty bitmap should be 0");
        }
        if (multiroarRank(r, 100) != 0) {
            ERRR("Rank at 100 for empty bitmap should be 0");
        }

        multiroarFree(r);
    }

    TEST("multiroarRank - single bit") {
        multiroar *r = multiroarBitNew();
        multiroarBitSet(r, 100);

        if (multiroarRank(r, 0) != 0) {
            ERRR("Rank before bit should be 0");
        }
        if (multiroarRank(r, 100) != 0) {
            ERRR("Rank at bit position should be 0 (excludes position)");
        }
        if (multiroarRank(r, 101) != 1) {
            ERRR("Rank after bit should be 1");
        }
        if (multiroarRank(r, 1000) != 1) {
            ERRR("Rank far after bit should be 1");
        }

        multiroarFree(r);
    }

    TEST("multiroarRank - sparse chunk") {
        multiroar *r = multiroarBitNew();
        /* Set bits at positions 10, 20, 30 */
        multiroarBitSet(r, 10);
        multiroarBitSet(r, 20);
        multiroarBitSet(r, 30);

        if (multiroarRank(r, 0) != 0) {
            ERR("Rank at 0: expected 0, got %" PRIu64 "", multiroarRank(r, 0));
        }
        if (multiroarRank(r, 10) != 0) {
            ERR("Rank at 10: expected 0, got %" PRIu64 "",
                multiroarRank(r, 10));
        }
        if (multiroarRank(r, 11) != 1) {
            ERR("Rank at 11: expected 1, got %" PRIu64 "",
                multiroarRank(r, 11));
        }
        if (multiroarRank(r, 20) != 1) {
            ERR("Rank at 20: expected 1, got %" PRIu64 "",
                multiroarRank(r, 20));
        }
        if (multiroarRank(r, 21) != 2) {
            ERR("Rank at 21: expected 2, got %" PRIu64 "",
                multiroarRank(r, 21));
        }
        if (multiroarRank(r, 31) != 3) {
            ERR("Rank at 31: expected 3, got %" PRIu64 "",
                multiroarRank(r, 31));
        }

        multiroarFree(r);
    }

    TEST("multiroarRank - dense chunk") {
        multiroar *r = multiroarBitNew();
        /* Set bits 0..999 (1000 bits, triggers dense mode) */
        for (uint64_t i = 0; i < 1000; i++) {
            multiroarBitSet(r, i);
        }

        if (multiroarRank(r, 0) != 0) {
            ERR("Rank at 0: expected 0, got %" PRIu64 "", multiroarRank(r, 0));
        }
        if (multiroarRank(r, 500) != 500) {
            ERR("Rank at 500: expected 500, got %" PRIu64 "",
                multiroarRank(r, 500));
        }
        if (multiroarRank(r, 1000) != 1000) {
            ERR("Rank at 1000: expected 1000, got %" PRIu64 "",
                multiroarRank(r, 1000));
        }
        if (multiroarRank(r, 1001) != 1000) {
            ERR("Rank at 1001: expected 1000, got %" PRIu64 "",
                multiroarRank(r, 1001));
        }

        multiroarFree(r);
    }

    TEST("multiroarRank - multi-chunk") {
        multiroar *r = multiroarBitNew();
        /* Set bits in multiple chunks */
        multiroarBitSet(r, 100);    /* Chunk 0 */
        multiroarBitSet(r, 200);    /* Chunk 0 */
        multiroarBitSet(r, 10000);  /* Chunk 1 */
        multiroarBitSet(r, 10100);  /* Chunk 1 */
        multiroarBitSet(r, 100000); /* Chunk 12 */

        if (multiroarRank(r, 100) != 0) {
            ERR("Rank at 100: expected 0, got %" PRIu64 "",
                multiroarRank(r, 100));
        }
        if (multiroarRank(r, 201) != 2) {
            ERR("Rank at 201: expected 2, got %" PRIu64 "",
                multiroarRank(r, 201));
        }
        if (multiroarRank(r, 10000) != 2) {
            ERR("Rank at 10000: expected 2, got %" PRIu64 "",
                multiroarRank(r, 10000));
        }
        if (multiroarRank(r, 10101) != 4) {
            ERR("Rank at 10101: expected 4, got %" PRIu64 "",
                multiroarRank(r, 10101));
        }
        if (multiroarRank(r, 100001) != 5) {
            ERR("Rank at 100001: expected 5, got %" PRIu64 "",
                multiroarRank(r, 100001));
        }

        multiroarFree(r);
    }

    TEST("multiroarSelect - empty bitmap") {
        multiroar *r = multiroarBitNew();
        uint64_t pos;

        if (multiroarSelect(r, 0, &pos)) {
            ERRR("Select with k=0 should fail");
        }
        if (multiroarSelect(r, 1, &pos)) {
            ERRR("Select on empty bitmap should fail");
        }

        multiroarFree(r);
    }

    TEST("multiroarSelect - single bit") {
        multiroar *r = multiroarBitNew();
        multiroarBitSet(r, 100);

        uint64_t pos;
        if (!multiroarSelect(r, 1, &pos) || pos != 100) {
            ERR("Select 1st bit: expected 100, got %" PRIu64, pos);
        }
        if (multiroarSelect(r, 2, &pos)) {
            ERRR("Select 2nd bit should fail (only 1 bit set)");
        }

        multiroarFree(r);
    }

    TEST("multiroarSelect - sparse chunk") {
        multiroar *r = multiroarBitNew();
        /* Set bits at positions 10, 20, 30 */
        multiroarBitSet(r, 10);
        multiroarBitSet(r, 20);
        multiroarBitSet(r, 30);

        uint64_t pos;
        if (!multiroarSelect(r, 1, &pos) || pos != 10) {
            ERR("Select 1st: expected 10, got %" PRIu64, pos);
        }
        if (!multiroarSelect(r, 2, &pos) || pos != 20) {
            ERR("Select 2nd: expected 20, got %" PRIu64, pos);
        }
        if (!multiroarSelect(r, 3, &pos) || pos != 30) {
            ERR("Select 3rd: expected 30, got %" PRIu64, pos);
        }
        if (multiroarSelect(r, 4, &pos)) {
            ERRR("Select 4th should fail");
        }

        multiroarFree(r);
    }

    TEST("multiroarSelect - dense chunk") {
        multiroar *r = multiroarBitNew();
        /* Set bits 0..999 */
        for (uint64_t i = 0; i < 1000; i++) {
            multiroarBitSet(r, i);
        }

        uint64_t pos;
        if (!multiroarSelect(r, 1, &pos) || pos != 0) {
            ERR("Select 1st: expected 0, got %" PRIu64, pos);
        }
        if (!multiroarSelect(r, 500, &pos) || pos != 499) {
            ERR("Select 500th: expected 499, got %" PRIu64, pos);
        }
        if (!multiroarSelect(r, 1000, &pos) || pos != 999) {
            ERR("Select 1000th: expected 999, got %" PRIu64, pos);
        }
        if (multiroarSelect(r, 1001, &pos)) {
            ERRR("Select 1001st should fail");
        }

        multiroarFree(r);
    }

    TEST("multiroarSelect - multi-chunk") {
        multiroar *r = multiroarBitNew();
        multiroarBitSet(r, 100);    /* 1st bit */
        multiroarBitSet(r, 200);    /* 2nd bit */
        multiroarBitSet(r, 10000);  /* 3rd bit (chunk 1) */
        multiroarBitSet(r, 10100);  /* 4th bit */
        multiroarBitSet(r, 100000); /* 5th bit (chunk 12) */

        uint64_t pos;
        if (!multiroarSelect(r, 1, &pos) || pos != 100) {
            ERR("Select 1st: expected 100, got %" PRIu64, pos);
        }
        if (!multiroarSelect(r, 2, &pos) || pos != 200) {
            ERR("Select 2nd: expected 200, got %" PRIu64, pos);
        }
        if (!multiroarSelect(r, 3, &pos) || pos != 10000) {
            ERR("Select 3rd: expected 10000, got %" PRIu64, pos);
        }
        if (!multiroarSelect(r, 4, &pos) || pos != 10100) {
            ERR("Select 4th: expected 10100, got %" PRIu64, pos);
        }
        if (!multiroarSelect(r, 5, &pos) || pos != 100000) {
            ERR("Select 5th: expected 100000, got %" PRIu64, pos);
        }
        if (multiroarSelect(r, 6, &pos)) {
            ERRR("Select 6th should fail");
        }

        multiroarFree(r);
    }

    TEST("multiroarRank/Select - invariant: rank(select(k)) == k") {
        multiroar *r = multiroarBitNew();

        /* Set various bits */
        multiroarBitSet(r, 5);
        multiroarBitSet(r, 15);
        multiroarBitSet(r, 25);
        multiroarBitSet(r, 100);
        multiroarBitSet(r, 1000);
        multiroarBitSet(r, 10000);

        /* Verify rank(select(k)) == k for all k */
        for (uint64_t k = 1; k <= 6; k++) {
            uint64_t pos;
            if (!multiroarSelect(r, k, &pos)) {
                ERR("Select(%" PRIu64 ") failed", k);
                continue;
            }
            uint64_t rank = multiroarRank(r, pos + 1);
            if (rank != k) {
                ERR("Invariant broken: rank(select(%" PRIu64 ")+1) = %" PRIu64
                    ", expected %" PRIu64 "",
                    k, rank, k);
            }
        }

        multiroarFree(r);
    }

    TEST_DESC("rank/select fuzzing - %d random operations", 1000) {
        for (int trial = 0; trial < 1000; trial++) {
            multiroar *r = multiroarBitNew();

            /* Create random bitmap */
            int numBits = 10 + (rand() % 491); /* Max 500 to fit in array */
            uint64_t positions[500];

            for (int i = 0; i < numBits; i++) {
                positions[i] = ((uint64_t)rand() * rand()) % 50000;
                multiroarBitSet(r, positions[i]);
            }

            /* Sort positions for oracle */
            for (int i = 0; i < numBits; i++) {
                for (int j = i + 1; j < numBits; j++) {
                    if (positions[i] > positions[j]) {
                        uint64_t tmp = positions[i];
                        positions[i] = positions[j];
                        positions[j] = tmp;
                    }
                }
            }

            /* Remove duplicates */
            int uniqueCount = 0;
            for (int i = 0; i < numBits; i++) {
                if (i == 0 || positions[i] != positions[uniqueCount - 1]) {
                    positions[uniqueCount++] = positions[i];
                }
            }

            /* Test Rank at random positions */
            for (int test = 0; test < 10; test++) {
                uint64_t testPos = ((uint64_t)rand() * rand()) % 60000;
                uint64_t rank = multiroarRank(r, testPos);

                /* Count bits < testPos manually */
                uint64_t expectedRank = 0;
                for (int i = 0; i < uniqueCount; i++) {
                    if (positions[i] < testPos) {
                        expectedRank++;
                    }
                }

                if (rank != expectedRank) {
                    ERR("Rank mismatch at %" PRIu64 ": expected %" PRIu64
                        ", got %" PRIu64 "",
                        testPos, expectedRank, rank);
                }
            }

            /* Test Select for all bits */
            for (int k = 1; k <= uniqueCount; k++) {
                uint64_t pos;
                if (!multiroarSelect(r, k, &pos)) {
                    ERR("Select(%d) failed but should succeed", k);
                    continue;
                }
                if (pos != positions[k - 1]) {
                    ERR("Select(%d): expected %" PRIu64 ", got %" PRIu64, k,
                        positions[k - 1], pos);
                }
            }

            /* Test Select beyond bounds */
            uint64_t pos;
            if (multiroarSelect(r, uniqueCount + 1, &pos)) {
                ERRR("Select beyond bounds should fail");
            }

            /* Verify rank/select invariant */
            for (int k = 1; k <= uniqueCount; k++) {
                uint64_t selectPos;
                if (multiroarSelect(r, k, &selectPos)) {
                    uint64_t rankVal = multiroarRank(r, selectPos + 1);
                    if (rankVal != (uint64_t)k) {
                        ERR("Invariant broken: rank(select(%d)+1) = %" PRIu64
                            ", "
                            "expected %d",
                            k, rankVal, k);
                    }
                }
            }

            multiroarFree(r);
        }
    }

    TEST("rank/select - edge case: max uint64_t positions") {
        multiroar *r = multiroarBitNew();

        /* Test near max uint64_t boundaries */
        uint64_t largePos = UINT64_MAX - 1000;
        multiroarBitSet(r, largePos);
        multiroarBitSet(r, largePos + 500);

        /* Test rank at boundaries */
        uint64_t rank1 = multiroarRank(r, largePos);
        if (rank1 != 0) {
            ERR("Rank before first large bit should be 0, got %" PRIu64 "",
                rank1);
        }

        uint64_t rank2 = multiroarRank(r, largePos + 1);
        if (rank2 != 1) {
            ERR("Rank after first large bit should be 1, got %" PRIu64 "",
                rank2);
        }

        uint64_t rank3 = multiroarRank(r, UINT64_MAX);
        if (rank3 != 2) {
            ERR("Rank at UINT64_MAX should be 2, got %" PRIu64 "", rank3);
        }

        /* Test select */
        uint64_t pos;
        if (!multiroarSelect(r, 1, &pos) || pos != largePos) {
            ERR("Select(1) should return %" PRIu64 ", got %" PRIu64, largePos,
                pos);
        }
        if (!multiroarSelect(r, 2, &pos) || pos != largePos + 500) {
            ERR("Select(2) should return %" PRIu64 ", got %" PRIu64,
                largePos + 500, pos);
        }

        multiroarFree(r);
    }

    TEST("rank/select - edge case: sparse bitmaps") {
        multiroar *r = multiroarBitNew();

        /* Very sparse bitmap with widely separated bits */
        uint64_t positions[] = {0, 1ULL << 20, 1ULL << 30, 1ULL << 40};
        for (int i = 0; i < 4; i++) {
            multiroarBitSet(r, positions[i]);
        }

        /* Verify rank at each position */
        for (int i = 0; i < 4; i++) {
            uint64_t rank = multiroarRank(r, positions[i] + 1);
            if (rank != (uint64_t)(i + 1)) {
                ERR("Rank after position %d should be %d, got %" PRIu64 "", i,
                    i + 1, rank);
            }
        }

        /* Verify select */
        for (int i = 0; i < 4; i++) {
            uint64_t pos;
            if (!multiroarSelect(r, i + 1, &pos) || pos != positions[i]) {
                ERR("Select(%d) should return %" PRIu64 ", got %" PRIu64, i + 1,
                    positions[i], pos);
            }
        }

        multiroarFree(r);
    }

    TEST("rank/select - edge case: dense consecutive bits") {
        multiroar *r = multiroarBitNew();

        /* Set 10000 consecutive bits starting at position 1000000 */
        uint64_t start = 1000000;
        uint64_t count = 10000;
        for (uint64_t i = 0; i < count; i++) {
            multiroarBitSet(r, start + i);
        }

        /* Test rank */
        if (multiroarRank(r, start) != 0) {
            ERR("Rank before dense region should be 0, got %" PRIu64 "",
                multiroarRank(r, start));
        }
        if (multiroarRank(r, start + count) != count) {
            ERR("Rank after dense region should be %" PRIu64 ", got %" PRIu64
                "",
                count, multiroarRank(r, start + count));
        }

        /* Test select at boundaries */
        uint64_t pos;
        if (!multiroarSelect(r, 1, &pos) || pos != start) {
            ERR("Select(1) should return %" PRIu64 ", got %" PRIu64, start,
                pos);
        }
        if (!multiroarSelect(r, count, &pos) || pos != start + count - 1) {
            ERR("Select(%" PRIu64 ") should return %" PRIu64 ", got %" PRIu64,
                count, start + count - 1, pos);
        }
        if (multiroarSelect(r, count + 1, &pos)) {
            ERRR("Select beyond count should fail");
        }

        multiroarFree(r);
    }

    TEST("rank/select - edge case: alternating bits") {
        multiroar *r = multiroarBitNew();

        /* Set alternating bits in a range */
        uint64_t start = 50000;
        uint64_t range = 10000;
        for (uint64_t i = 0; i < range; i += 2) {
            multiroarBitSet(r, start + i);
        }

        /* Test rank at various positions */
        uint64_t expectedCount = range / 2;
        if (multiroarRank(r, start + range) != expectedCount) {
            ERR("Rank should be %" PRIu64 ", got %" PRIu64 "", expectedCount,
                multiroarRank(r, start + range));
        }

        /* Test select for every 100th bit */
        for (uint64_t k = 1; k <= expectedCount; k += 100) {
            uint64_t pos;
            uint64_t expected = start + (k - 1) * 2;
            if (!multiroarSelect(r, k, &pos) || pos != expected) {
                ERR("Select(%" PRIu64 ") should return %" PRIu64
                    ", got %" PRIu64,
                    k, expected, pos);
            }
        }

        multiroarFree(r);
    }

    TEST("rank/select - edge case: single bit") {
        multiroar *r = multiroarBitNew();

        multiroarBitSet(r, 42);

        /* Test rank */
        if (multiroarRank(r, 0) != 0) {
            ERRR("Rank(0) should be 0");
        }
        if (multiroarRank(r, 42) != 0) {
            ERRR("Rank(42) should be 0");
        }
        if (multiroarRank(r, 43) != 1) {
            ERRR("Rank(43) should be 1");
        }
        if (multiroarRank(r, 1000) != 1) {
            ERRR("Rank(1000) should be 1");
        }

        /* Test select */
        uint64_t pos;
        if (!multiroarSelect(r, 1, &pos) || pos != 42) {
            ERR("Select(1) should return 42, got %" PRIu64, pos);
        }
        if (multiroarSelect(r, 2, &pos)) {
            ERRR("Select(2) should fail");
        }
        if (multiroarSelect(r, 0, &pos)) {
            ERRR("Select(0) should fail");
        }

        multiroarFree(r);
    }

    TEST("rank/select - edge case: duplicate positions in fuzzing") {
        multiroar *r = multiroarBitNew();

        /* Set same position multiple times */
        for (int i = 0; i < 100; i++) {
            multiroarBitSet(r, 1000);
        }

        /* Should only count once */
        if (multiroarBitCount(r) != 1) {
            ERR("Count should be 1, got %" PRIu64 "", multiroarBitCount(r));
        }
        if (multiroarRank(r, 1001) != 1) {
            ERR("Rank(1001) should be 1, got %" PRIu64 "",
                multiroarRank(r, 1001));
        }

        uint64_t pos;
        if (!multiroarSelect(r, 1, &pos) || pos != 1000) {
            ERR("Select(1) should return 1000, got %" PRIu64, pos);
        }

        multiroarFree(r);
    }

    printf("\n=== Rank/Select tests completed! ===\n\n");

    /* ====================================================================
     * Range Operations Tests
     * ==================================================================== */

    TEST("multiroarRangeCount - empty bitmap") {
        multiroar *r = multiroarBitNew();

        if (multiroarRangeCount(r, 0, 100) != 0) {
            ERRR("RangeCount on empty bitmap should be 0");
        }

        multiroarFree(r);
    }

    TEST("multiroarRangeCount - basic range") {
        multiroar *r = multiroarBitNew();
        /* Set bits 10, 20, 30, 40, 50 */
        for (uint64_t i = 10; i <= 50; i += 10) {
            multiroarBitSet(r, i);
        }

        if (multiroarRangeCount(r, 0, 25) != 2) {
            ERR("RangeCount [0,25): expected 2, got %" PRIu64 "",
                multiroarRangeCount(r, 0, 25));
        }
        if (multiroarRangeCount(r, 15, 45) != 3) {
            ERR("RangeCount [15,45): expected 3, got %" PRIu64 "",
                multiroarRangeCount(r, 15, 45));
        }
        if (multiroarRangeCount(r, 10, 51) != 5) {
            ERR("RangeCount [10,51): expected 5, got %" PRIu64 "",
                multiroarRangeCount(r, 10, 51));
        }

        multiroarFree(r);
    }

    TEST("multiroarBitClearRange - basic clear") {
        multiroar *r = multiroarBitNew();
        /* Set bits 0..99 */
        for (uint64_t i = 0; i < 100; i++) {
            multiroarBitSet(r, i);
        }

        /* Clear 20..29 (10 bits) */
        multiroarBitClearRange(r, 20, 10);

        if (multiroarBitCount(r) != 90) {
            ERR("After clearing 10 bits: expected 90, got %" PRIu64 "",
                multiroarBitCount(r));
        }

        for (uint64_t i = 20; i < 30; i++) {
            if (multiroarBitGet(r, i)) {
                ERR("Bit %" PRIu64 " should be cleared", i);
            }
        }

        multiroarFree(r);
    }

    TEST("multiroarBitClearRange - multi-chunk") {
        multiroar *r = multiroarBitNew();
        /* Set bits across chunks */
        for (uint64_t i = 8000; i < 10000; i++) {
            multiroarBitSet(r, i);
        }

        uint64_t beforeCount = multiroarBitCount(r);

        /* Count how many bits are actually set in the range we're about to
         * clear */
        uint64_t bitsInRange = multiroarRangeCount(r, 8100, 9000);

        multiroarBitClearRange(r, 8100, 900); /* Clear across chunk boundary */
        uint64_t afterCount = multiroarBitCount(r);

        if (afterCount != beforeCount - bitsInRange) {
            ERR("Clear range: expected %" PRIu64 ", got %" PRIu64
                " (range had %" PRIu64 " bits)",
                beforeCount - bitsInRange, afterCount, bitsInRange);
        }

        multiroarFree(r);
    }

    TEST("multiroarBitFlipRange - basic flip") {
        multiroar *r = multiroarBitNew();
        /* Set alternating bits 0, 2, 4, 6, 8 */
        for (uint64_t i = 0; i < 10; i += 2) {
            multiroarBitSet(r, i);
        }

        /* Flip range 0..9 (should flip all bits in range) */
        multiroarBitFlipRange(r, 0, 10);

        /* Now bits 1, 3, 5, 7, 9 should be set */
        for (uint64_t i = 0; i < 10; i++) {
            bool shouldBeSet = (i % 2 == 1);
            if (multiroarBitGet(r, i) != shouldBeSet) {
                ERR("After flip: bit %" PRIu64 " should be %d", i, shouldBeSet);
            }
        }

        multiroarFree(r);
    }

    TEST("multiroarBitFlipRange - double flip") {
        multiroar *r = multiroarBitNew();
        multiroarBitSet(r, 50);

        /* Flip twice should restore */
        multiroarBitFlipRange(r, 40, 20);
        multiroarBitFlipRange(r, 40, 20);

        if (!multiroarBitGet(r, 50)) {
            ERRR("Double flip should restore original state");
        }

        multiroarFree(r);
    }

    TEST("multiroarAndNot - basic difference") {
        multiroar *a = multiroarBitNew();
        multiroar *b = multiroarBitNew();

        /* A = {10, 20, 30, 40, 50} */
        for (uint64_t i = 10; i <= 50; i += 10) {
            multiroarBitSet(a, i);
        }

        /* B = {20, 40} */
        multiroarBitSet(b, 20);
        multiroarBitSet(b, 40);

        /* A - B = {10, 30, 50} */
        multiroarAndNot(a, b);

        if (multiroarBitCount(a) != 3) {
            ERR("A - B count: expected 3, got %" PRIu64 "",
                multiroarBitCount(a));
        }
        if (!multiroarBitGet(a, 10) || !multiroarBitGet(a, 30) ||
            !multiroarBitGet(a, 50)) {
            ERRR("A - B should contain {10, 30, 50}");
        }
        if (multiroarBitGet(a, 20) || multiroarBitGet(a, 40)) {
            ERRR("A - B should not contain {20, 40}");
        }

        multiroarFree(a);
        multiroarFree(b);
    }

    TEST("multiroarNewAndNot - creates new result") {
        multiroar *a = multiroarBitNew();
        multiroar *b = multiroarBitNew();

        for (uint64_t i = 0; i < 100; i++) {
            multiroarBitSet(a, i);
        }
        for (uint64_t i = 50; i < 150; i++) {
            multiroarBitSet(b, i);
        }

        /* A - B = [0, 50) */
        multiroar *result = multiroarNewAndNot(a, b);

        if (multiroarBitCount(result) != 50) {
            ERR("NewAndNot count: expected 50, got %" PRIu64 "",
                multiroarBitCount(result));
        }

        /* Verify a is unchanged */
        if (multiroarBitCount(a) != 100) {
            ERRR("Original A should be unchanged");
        }

        multiroarFree(a);
        multiroarFree(b);
        multiroarFree(result);
    }

    TEST("multiroarAndNot - disjoint sets") {
        multiroar *a = multiroarBitNew();
        multiroar *b = multiroarBitNew();

        for (uint64_t i = 0; i < 50; i++) {
            multiroarBitSet(a, i);
        }
        for (uint64_t i = 100; i < 150; i++) {
            multiroarBitSet(b, i);
        }

        uint64_t beforeCount = multiroarBitCount(a);
        multiroarAndNot(a, b);

        if (multiroarBitCount(a) != beforeCount) {
            ERRR("Disjoint A - B should equal A");
        }

        multiroarFree(a);
        multiroarFree(b);
    }

    TEST("multiroarAndNot - complete overlap") {
        multiroar *a = multiroarBitNew();
        multiroar *b = multiroarBitNew();

        for (uint64_t i = 0; i < 50; i++) {
            multiroarBitSet(a, i);
            multiroarBitSet(b, i);
        }

        multiroarAndNot(a, b);

        if (multiroarBitCount(a) != 0) {
            ERRR("Complete overlap: A - A should be empty");
        }

        multiroarFree(a);
        multiroarFree(b);
    }

    TEST("multiroarBitFlipRange - minimal debug test") {
        /* Minimal test to debug the assertion failure */
        multiroar *r = multiroarBitNew();

        /* Set some bits */
        for (uint64_t i = 0; i < 20; i++) {
            multiroarBitSet(r, i * 5); /* 0, 5, 10, 15, ... 95 */
        }

        uint64_t countBefore = multiroarBitCount(r);

        /* Flip a range */
        multiroarBitFlipRange(r, 10, 30); /* Flip [10, 40) */

        /* Flip the same range again - should restore */
        multiroarBitFlipRange(r, 10, 30); /* Flip [10, 40) again */

        uint64_t countAfter = multiroarBitCount(r);

        if (countBefore != countAfter) {
            ERR("Minimal flip test: expected count %" PRIu64 ", got %" PRIu64
                "",
                countBefore, countAfter);
        }

        multiroarFree(r);
    }

    TEST_DESC("range operations fuzzing - %d random operations", 500) {
        for (int trial = 0; trial < 500; trial++) {
            multiroar *r = multiroarBitNew();

            /* Set random bits */
            int numBits = 50 + (rand() % 200);
            for (int i = 0; i < numBits; i++) {
                uint64_t pos = ((uint64_t)rand() * rand()) % 10000;
                multiroarBitSet(r, pos);
            }

            uint64_t initialCount = multiroarBitCount(r);

            /* Test RangeCount */
            uint64_t rangeStart = ((uint64_t)rand() * rand()) % 8000;
            uint64_t rangeEnd = rangeStart + (rand() % 2000);

            uint64_t rangeCount = multiroarRangeCount(r, rangeStart, rangeEnd);
            uint64_t manualCount = 0;
            for (uint64_t pos = rangeStart; pos < rangeEnd; pos++) {
                if (multiroarBitGet(r, pos)) {
                    manualCount++;
                }
            }

            if (rangeCount != manualCount) {
                ERR("RangeCount mismatch: expected %" PRIu64 ", got %" PRIu64
                    "",
                    manualCount, rangeCount);
            }

            /* Test ClearRange */
            multiroar *r2 = multiroarDuplicate(r);
            uint64_t clearStart = rand() % 1000;
            uint64_t clearExtent = rand() % 500;

            uint64_t bitsInRange =
                multiroarRangeCount(r2, clearStart, clearStart + clearExtent);
            multiroarBitClearRange(r2, clearStart, clearExtent);
            uint64_t afterClear = multiroarBitCount(r2);

            if (afterClear != initialCount - bitsInRange) {
                ERR("ClearRange: expected %" PRIu64 ", got %" PRIu64 "",
                    initialCount - bitsInRange, afterClear);
            }

            multiroarFree(r2);

            /* Test FlipRange */
            multiroar *r3 = multiroarDuplicate(r);
            uint64_t flipStart = rand() % 1000;
            uint64_t flipExtent = rand() % 500;

            multiroarBitFlipRange(r3, flipStart, flipExtent);
            multiroarBitFlipRange(r3, flipStart, flipExtent); /* Flip twice */

            /* Should equal original */
            if (!multiroarEquals(r, r3)) {
                ERRR("Double flip should restore original");
            }

            multiroarFree(r3);
            multiroarFree(r);
        }
    }

    printf("\n=== Range operations tests completed! ===\n\n");

    /* ================================================================
     * Iterator Tests
     * ================================================================ */

    TEST("multiroarIterator - empty bitmap") {
        multiroar *r = multiroarBitNew();
        multiroarIterator iter;
        multiroarIteratorInit(r, &iter);

        uint64_t pos;
        if (multiroarIteratorNext(&iter, &pos)) {
            ERRR("Empty bitmap should have no positions");
        }

        multiroarFree(r);
    }

    TEST("multiroarIterator - single bit") {
        multiroar *r = multiroarBitNew();
        multiroarBitSet(r, 42);

        multiroarIterator iter;
        multiroarIteratorInit(r, &iter);

        uint64_t pos;
        if (!multiroarIteratorNext(&iter, &pos) || pos != 42) {
            ERR("Expected position 42, got %" PRIu64, pos);
        }

        if (multiroarIteratorNext(&iter, &pos)) {
            ERRR("Should only have one position");
        }

        multiroarFree(r);
    }

    TEST("multiroarIterator - sparse chunk") {
        multiroar *r = multiroarBitNew();
        /* Set bits 10, 20, 30, 40, 50 */
        for (uint64_t i = 10; i <= 50; i += 10) {
            multiroarBitSet(r, i);
        }

        multiroarIterator iter;
        multiroarIteratorInit(r, &iter);

        const uint64_t expected[] = {10, 20, 30, 40, 50};
        for (int i = 0; i < 5; i++) {
            uint64_t pos;
            if (!multiroarIteratorNext(&iter, &pos) || pos != expected[i]) {
                ERR("Expected %" PRIu64 ", got %" PRIu64, expected[i], pos);
            }
        }

        uint64_t pos;
        if (multiroarIteratorNext(&iter, &pos)) {
            ERRR("Should have exactly 5 positions");
        }

        multiroarFree(r);
    }

    TEST("multiroarIterator - dense chunk") {
        multiroar *r = multiroarBitNew();
        /* Set 1000 consecutive bits */
        for (uint64_t i = 0; i < 1000; i++) {
            multiroarBitSet(r, i);
        }

        multiroarIterator iter;
        multiroarIteratorInit(r, &iter);

        uint64_t count = 0;
        uint64_t pos;
        uint64_t lastPos = 0;
        while (multiroarIteratorNext(&iter, &pos)) {
            if (count > 0 && pos != lastPos + 1) {
                ERR("Non-consecutive positions: %" PRIu64 " -> %" PRIu64,
                    lastPos, pos);
            }
            lastPos = pos;
            count++;
        }

        if (count != 1000) {
            ERR("Expected 1000 positions, got %" PRIu64, count);
        }

        multiroarFree(r);
    }

    TEST("multiroarIterator - multiple chunks") {
        multiroar *r = multiroarBitNew();
        /* Set bits in different chunks */
        multiroarBitSet(r, 100);   /* Chunk 0 */
        multiroarBitSet(r, 8200);  /* Chunk 1 */
        multiroarBitSet(r, 16400); /* Chunk 2 */

        multiroarIterator iter;
        multiroarIteratorInit(r, &iter);

        const uint64_t expected[] = {100, 8200, 16400};
        for (int i = 0; i < 3; i++) {
            uint64_t pos;
            if (!multiroarIteratorNext(&iter, &pos) || pos != expected[i]) {
                ERR("Expected %" PRIu64 ", got %" PRIu64, expected[i], pos);
            }
        }

        multiroarFree(r);
    }

    TEST("multiroarIterator - verify count matches bitcount") {
        multiroar *r = multiroarBitNew();
        /* Set random bits */
        for (int i = 0; i < 500; i++) {
            uint64_t pos = ((uint64_t)rand() * rand()) % 50000;
            multiroarBitSet(r, pos);
        }

        uint64_t expectedCount = multiroarBitCount(r);

        multiroarIterator iter;
        multiroarIteratorInit(r, &iter);

        uint64_t iterCount = 0;
        uint64_t pos;
        while (multiroarIteratorNext(&iter, &pos)) {
            iterCount++;
        }

        if (iterCount != expectedCount) {
            ERR("Iterator count %" PRIu64 " != bitcount %" PRIu64 "", iterCount,
                expectedCount);
        }

        multiroarFree(r);
    }

    TEST("multiroarIterator - ascending order") {
        multiroar *r = multiroarBitNew();
        /* Set bits in random order */
        uint64_t positions[] = {500, 100, 300, 200, 400};
        for (int i = 0; i < 5; i++) {
            multiroarBitSet(r, positions[i]);
        }

        multiroarIterator iter;
        multiroarIteratorInit(r, &iter);

        uint64_t lastPos = 0;
        uint64_t pos;
        bool first = true;
        while (multiroarIteratorNext(&iter, &pos)) {
            if (!first && pos <= lastPos) {
                ERR("Positions not ascending: %" PRIu64 " -> %" PRIu64, lastPos,
                    pos);
            }
            lastPos = pos;
            first = false;
        }

        multiroarFree(r);
    }

    TEST("multiroarIteratorReset - reset and re-iterate") {
        multiroar *r = multiroarBitNew();
        for (uint64_t i = 0; i < 10; i++) {
            multiroarBitSet(r, i * 10);
        }

        multiroarIterator iter;
        multiroarIteratorInit(r, &iter);

        /* First iteration */
        uint64_t count1 = 0;
        uint64_t pos;
        while (multiroarIteratorNext(&iter, &pos)) {
            count1++;
        }

        /* Reset and iterate again */
        multiroarIteratorReset(&iter);
        uint64_t count2 = 0;
        while (multiroarIteratorNext(&iter, &pos)) {
            count2++;
        }

        if (count1 != count2 || count1 != 10) {
            ERR("Reset failed: first=%" PRIu64 " second=%" PRIu64, count1,
                count2);
        }

        multiroarFree(r);
    }

    TEST_DESC("iterator fuzzing - %d random bitmaps", 500) {
        for (int trial = 0; trial < 500; trial++) {
            multiroar *r = multiroarBitNew();

            /* Create random bitmap */
            int numBits = 50 + (rand() % 200);
            for (int i = 0; i < numBits; i++) {
                uint64_t pos = ((uint64_t)rand() * rand()) % 100000;
                multiroarBitSet(r, pos);
            }

            uint64_t expectedCount = multiroarBitCount(r);

            /* Iterate and verify */
            multiroarIterator iter;
            multiroarIteratorInit(r, &iter);

            uint64_t iterCount = 0;
            uint64_t pos;
            uint64_t lastPos = 0;
            bool first = true;

            while (multiroarIteratorNext(&iter, &pos)) {
                /* Verify ascending order */
                if (!first && pos <= lastPos) {
                    ERR("Trial %d: positions not ascending: %" PRIu64
                        " -> %" PRIu64,
                        trial, lastPos, pos);
                }

                /* Verify bit is actually set */
                if (!multiroarBitGet(r, pos)) {
                    ERR("Trial %d: iterator returned unset position %" PRIu64,
                        trial, pos);
                }

                lastPos = pos;
                first = false;
                iterCount++;
            }

            if (iterCount != expectedCount) {
                ERR("Trial %d: iterator count %" PRIu64 " != bitcount %" PRIu64
                    "",
                    trial, iterCount, expectedCount);
            }

            multiroarFree(r);
        }
    }

    printf("\n=== Iterator tests completed! ===\n\n");

    /* ================================================================
     * Bulk Operations Tests
     * ================================================================ */

    TEST("multiroarBitSetMany - basic") {
        multiroar *r = multiroarBitNew();
        uint64_t positions[] = {10, 20, 30, 40, 50};
        multiroarBitSetMany(r, positions, 5);

        for (int i = 0; i < 5; i++) {
            if (!multiroarBitGet(r, positions[i])) {
                ERR("Position %" PRIu64 " should be set", positions[i]);
            }
        }

        if (multiroarBitCount(r) != 5) {
            ERR("Expected 5 bits, got %" PRIu64 "", multiroarBitCount(r));
        }

        multiroarFree(r);
    }

    TEST("multiroarBitGetMany - basic") {
        multiroar *r = multiroarBitNew();
        multiroarBitSet(r, 10);
        multiroarBitSet(r, 30);
        multiroarBitSet(r, 50);

        uint64_t positions[] = {10, 20, 30, 40, 50};
        bool results[5];
        multiroarBitGetMany(r, positions, 5, results);

        bool expected[] = {true, false, true, false, true};
        for (int i = 0; i < 5; i++) {
            if (results[i] != expected[i]) {
                ERR("Position %" PRIu64 ": expected %d, got %d", positions[i],
                    expected[i], results[i]);
            }
        }

        multiroarFree(r);
    }

    TEST("multiroarToArray - basic") {
        multiroar *r = multiroarBitNew();
        uint64_t setPositions[] = {100, 200, 300, 400, 500};
        for (int i = 0; i < 5; i++) {
            multiroarBitSet(r, setPositions[i]);
        }

        uint64_t array[10];
        uint64_t count = multiroarToArray(r, array, 10);

        if (count != 5) {
            ERR("Expected 5 positions, got %" PRIu64 "", count);
        }

        for (uint64_t i = 0; i < count; i++) {
            if (array[i] != setPositions[i]) {
                ERR("Position %" PRIu64 ": expected %" PRIu64 ", got %" PRIu64,
                    i, setPositions[i], array[i]);
            }
        }

        multiroarFree(r);
    }

    TEST("multiroarToArray - limited capacity") {
        multiroar *r = multiroarBitNew();
        for (uint64_t i = 0; i < 100; i++) {
            multiroarBitSet(r, i);
        }

        uint64_t array[50];
        uint64_t count = multiroarToArray(r, array, 50);

        if (count != 50) {
            ERR("Expected 50 positions (limited by maxCount), got %" PRIu64 "",
                count);
        }

        /* Verify positions are ascending */
        for (uint64_t i = 1; i < count; i++) {
            if (array[i] <= array[i - 1]) {
                ERR("Positions not ascending at index %" PRIu64 "", i);
            }
        }

        multiroarFree(r);
    }

    TEST("multiroarFromArray - basic") {
        uint64_t positions[] = {10, 20, 30, 40, 50};
        multiroar *r = multiroarFromArray(positions, 5);

        if (multiroarBitCount(r) != 5) {
            ERR("Expected 5 bits, got %" PRIu64 "", multiroarBitCount(r));
        }

        for (int i = 0; i < 5; i++) {
            if (!multiroarBitGet(r, positions[i])) {
                ERR("Position %" PRIu64 " should be set", positions[i]);
            }
        }

        multiroarFree(r);
    }

    TEST("multiroarFromArray - with duplicates") {
        uint64_t positions[] = {10, 20, 10, 30, 20, 40};
        multiroar *r = multiroarFromArray(positions, 6);

        /* Should have 4 unique positions */
        if (multiroarBitCount(r) != 4) {
            ERR("Expected 4 unique bits, got %" PRIu64 "",
                multiroarBitCount(r));
        }

        multiroarFree(r);
    }

    TEST("ToArray/FromArray round-trip") {
        multiroar *r1 = multiroarBitNew();
        /* Set random bits */
        for (int i = 0; i < 100; i++) {
            uint64_t pos = ((uint64_t)rand() * rand()) % 10000;
            multiroarBitSet(r1, pos);
        }

        uint64_t count1 = multiroarBitCount(r1);

        /* Convert to array */
        uint64_t *array = zcalloc(count1, sizeof(uint64_t));
        uint64_t arrayCount = multiroarToArray(r1, array, count1);

        if (arrayCount != count1) {
            ERR("ToArray returned %" PRIu64 ", expected %" PRIu64 "",
                arrayCount, count1);
        }

        /* Convert back from array */
        multiroar *r2 = multiroarFromArray(array, arrayCount);

        uint64_t count2 = multiroarBitCount(r2);
        if (count2 != count1) {
            ERR("Round-trip count mismatch: %" PRIu64 " != %" PRIu64 "", count2,
                count1);
        }

        /* Verify equality */
        if (!multiroarEquals(r1, r2)) {
            ERRR("Round-trip bitmaps not equal");
        }

        zfree(array);
        multiroarFree(r1);
        multiroarFree(r2);
    }

    TEST_DESC("bulk operations fuzzing - %d trials", 500) {
        for (int trial = 0; trial < 500; trial++) {
            multiroar *r = multiroarBitNew();

            /* Generate random positions */
            int numPositions = 50 + (rand() % 200);
            uint64_t *positions = zcalloc(numPositions, sizeof(uint64_t));

            for (int i = 0; i < numPositions; i++) {
                positions[i] = ((uint64_t)rand() * rand()) % 100000;
            }

            /* Test SetMany */
            multiroarBitSetMany(r, positions, numPositions);

            /* Test GetMany */
            bool *results = zcalloc(numPositions, sizeof(bool));
            multiroarBitGetMany(r, positions, numPositions, results);

            for (int i = 0; i < numPositions; i++) {
                if (!results[i]) {
                    ERR("Trial %d: Position %" PRIu64 " should be set", trial,
                        positions[i]);
                }
            }

            /* Test ToArray */
            uint64_t bitCount = multiroarBitCount(r);
            uint64_t *array = zcalloc(bitCount, sizeof(uint64_t));
            uint64_t arrayCount = multiroarToArray(r, array, bitCount);

            if (arrayCount != bitCount) {
                ERR("Trial %d: ToArray count mismatch: %" PRIu64 " != %" PRIu64
                    "",
                    trial, arrayCount, bitCount);
            }

            /* Verify array is sorted */
            for (uint64_t i = 1; i < arrayCount; i++) {
                if (array[i] <= array[i - 1]) {
                    ERR("Trial %d: Array not sorted at index %" PRIu64 "",
                        trial, i);
                }
            }

            zfree(positions);
            zfree(results);
            zfree(array);
            multiroarFree(r);
        }
    }

    printf("\n=== Bulk operations tests completed! ===\n\n");

    /* ================================================================
     * Similarity Metrics Tests
     * ================================================================ */

    TEST("multiroarJaccard - identical sets") {
        multiroar *a = multiroarBitNew();
        multiroar *b = multiroarBitNew();

        for (uint64_t i = 0; i < 100; i++) {
            multiroarBitSet(a, i);
            multiroarBitSet(b, i);
        }

        double jaccard = multiroarJaccard(a, b);
        if (jaccard < 0.999 || jaccard > 1.001) {
            ERR("Identical sets should have Jaccard = 1.0, got %f", jaccard);
        }

        multiroarFree(a);
        multiroarFree(b);
    }

    TEST("multiroarJaccard - disjoint sets") {
        multiroar *a = multiroarBitNew();
        multiroar *b = multiroarBitNew();

        for (uint64_t i = 0; i < 100; i++) {
            multiroarBitSet(a, i);
        }
        for (uint64_t i = 100; i < 200; i++) {
            multiroarBitSet(b, i);
        }

        double jaccard = multiroarJaccard(a, b);
        if (jaccard > 0.001) {
            ERR("Disjoint sets should have Jaccard = 0.0, got %f", jaccard);
        }

        multiroarFree(a);
        multiroarFree(b);
    }

    TEST("multiroarJaccard - partial overlap") {
        multiroar *a = multiroarBitNew();
        multiroar *b = multiroarBitNew();

        /* A = [0..99], B = [50..149] */
        /* Intersection = [50..99] = 50 bits */
        /* Union = [0..149] = 150 bits */
        /* Jaccard = 50/150 = 0.333... */
        for (uint64_t i = 0; i < 100; i++) {
            multiroarBitSet(a, i);
        }
        for (uint64_t i = 50; i < 150; i++) {
            multiroarBitSet(b, i);
        }

        double jaccard = multiroarJaccard(a, b);
        double expected = 50.0 / 150.0;
        if (jaccard < expected - 0.01 || jaccard > expected + 0.01) {
            ERR("Expected Jaccard ~= %f, got %f", expected, jaccard);
        }

        multiroarFree(a);
        multiroarFree(b);
    }

    TEST("multiroarJaccard - empty sets") {
        multiroar *a = multiroarBitNew();
        multiroar *b = multiroarBitNew();

        double jaccard = multiroarJaccard(a, b);
        if (jaccard < 0.999 || jaccard > 1.001) {
            ERR("Empty sets should have Jaccard = 1.0, got %f", jaccard);
        }

        multiroarFree(a);
        multiroarFree(b);
    }

    TEST("multiroarHammingDistance - identical sets") {
        multiroar *a = multiroarBitNew();
        multiroar *b = multiroarBitNew();

        for (uint64_t i = 0; i < 100; i++) {
            multiroarBitSet(a, i);
            multiroarBitSet(b, i);
        }

        uint64_t distance = multiroarHammingDistance(a, b);
        if (distance != 0) {
            ERR("Identical sets should have Hamming distance = 0, got %" PRIu64
                "",
                distance);
        }

        multiroarFree(a);
        multiroarFree(b);
    }

    TEST("multiroarHammingDistance - disjoint sets") {
        multiroar *a = multiroarBitNew();
        multiroar *b = multiroarBitNew();

        for (uint64_t i = 0; i < 100; i++) {
            multiroarBitSet(a, i);
        }
        for (uint64_t i = 100; i < 200; i++) {
            multiroarBitSet(b, i);
        }

        uint64_t distance = multiroarHammingDistance(a, b);
        if (distance != 200) {
            ERR("Disjoint sets (100+100) should have Hamming distance = 200, "
                "got %" PRIu64 "",
                distance);
        }

        multiroarFree(a);
        multiroarFree(b);
    }

    TEST("multiroarOverlap - perfect overlap") {
        multiroar *a = multiroarBitNew();
        multiroar *b = multiroarBitNew();

        for (uint64_t i = 0; i < 50; i++) {
            multiroarBitSet(a, i);
        }
        for (uint64_t i = 0; i < 100; i++) {
            multiroarBitSet(b, i);
        }

        /* A ⊆ B, so overlap = 1.0 */
        double overlap = multiroarOverlap(a, b);
        if (overlap < 0.999 || overlap > 1.001) {
            ERR("Perfect overlap should be 1.0, got %f", overlap);
        }

        multiroarFree(a);
        multiroarFree(b);
    }

    TEST("multiroarDice - identical sets") {
        multiroar *a = multiroarBitNew();
        multiroar *b = multiroarBitNew();

        for (uint64_t i = 0; i < 100; i++) {
            multiroarBitSet(a, i);
            multiroarBitSet(b, i);
        }

        double dice = multiroarDice(a, b);
        if (dice < 0.999 || dice > 1.001) {
            ERR("Identical sets should have Dice = 1.0, got %f", dice);
        }

        multiroarFree(a);
        multiroarFree(b);
    }

    TEST("multiroarDice - partial overlap") {
        multiroar *a = multiroarBitNew();
        multiroar *b = multiroarBitNew();

        /* A = [0..99] (100 bits), B = [50..149] (100 bits) */
        /* Intersection = [50..99] = 50 bits */
        /* Dice = 2*50 / (100+100) = 100/200 = 0.5 */
        for (uint64_t i = 0; i < 100; i++) {
            multiroarBitSet(a, i);
        }
        for (uint64_t i = 50; i < 150; i++) {
            multiroarBitSet(b, i);
        }

        double dice = multiroarDice(a, b);
        double expected = 0.5;
        if (dice < expected - 0.01 || dice > expected + 0.01) {
            ERR("Expected Dice = %f, got %f", expected, dice);
        }

        multiroarFree(a);
        multiroarFree(b);
    }

    TEST_DESC("similarity metrics fuzzing - %d trials", 500) {
        for (int trial = 0; trial < 500; trial++) {
            multiroar *a = multiroarBitNew();
            multiroar *b = multiroarBitNew();

            /* Create random bitmaps */
            int numBitsA = 50 + (rand() % 200);
            int numBitsB = 50 + (rand() % 200);

            for (int i = 0; i < numBitsA; i++) {
                uint64_t pos = ((uint64_t)rand() * rand()) % 10000;
                multiroarBitSet(a, pos);
            }
            for (int i = 0; i < numBitsB; i++) {
                uint64_t pos = ((uint64_t)rand() * rand()) % 10000;
                multiroarBitSet(b, pos);
            }

            /* Test all metrics are in valid ranges */
            double jaccard = multiroarJaccard(a, b);
            if (jaccard < 0.0 || jaccard > 1.0) {
                ERR("Trial %d: Jaccard out of range [0,1]: %f", trial, jaccard);
            }

            uint64_t hamming = multiroarHammingDistance(a, b);
            uint64_t maxHamming = multiroarBitCount(a) + multiroarBitCount(b);
            if (hamming > maxHamming) {
                ERR("Trial %d: Hamming distance %" PRIu64
                    " exceeds max %" PRIu64 "",
                    trial, hamming, maxHamming);
            }

            double overlap = multiroarOverlap(a, b);
            if (overlap < 0.0 || overlap > 1.0) {
                ERR("Trial %d: Overlap out of range [0,1]: %f", trial, overlap);
            }

            double dice = multiroarDice(a, b);
            if (dice < 0.0 || dice > 1.0) {
                ERR("Trial %d: Dice out of range [0,1]: %f", trial, dice);
            }

            multiroarFree(a);
            multiroarFree(b);
        }
    }

    printf("\n=== Similarity metrics tests completed! ===\n\n");

    /* ================================================================
     * Statistics and Memory Tests
     * ================================================================ */

    TEST("multiroarMemoryUsage - empty") {
        multiroar *r = multiroarBitNew();
        uint64_t usage = multiroarMemoryUsage(r);

        if (usage < sizeof(multiroar)) {
            ERR("Memory usage should be at least %zu, got %" PRIu64 "",
                sizeof(multiroar), usage);
        }

        multiroarFree(r);
    }

    TEST("multiroarMemoryUsage - non-empty") {
        multiroar *r = multiroarBitNew();

        for (uint64_t i = 0; i < 1000; i++) {
            multiroarBitSet(r, i);
        }

        uint64_t usage = multiroarMemoryUsage(r);
        if (usage < sizeof(multiroar)) {
            ERR("Memory usage should be at least %zu, got %" PRIu64 "",
                sizeof(multiroar), usage);
        }

        multiroarFree(r);
    }

    printf("\n=== Statistics and memory tests completed! ===\n\n");

    /* ================================================================
     * Serialization Tests
     * ================================================================ */

    TEST("multiroarSerialize/Deserialize - empty roar") {
        multiroar *r = multiroarBitNew();

        uint64_t size = multiroarSerializedSize(r);
        void *buf = zcalloc(size, 1);

        uint64_t written = multiroarSerialize(r, buf, size);
        if (written == 0) {
            ERRR("Failed to serialize empty roar");
        }

        multiroar *r2 = multiroarDeserialize(buf, written);
        if (!r2) {
            ERRR("Failed to deserialize empty roar");
        }

        if (!multiroarEquals(r, r2)) {
            ERRR("Deserialized roar doesn't match original");
        }

        zfree(buf);
        multiroarFree(r);
        multiroarFree(r2);
    }

    TEST("multiroarSerialize/Deserialize - single bit") {
        multiroar *r = multiroarBitNew();
        multiroarBitSet(r, 42);

        uint64_t size = multiroarSerializedSize(r);
        void *buf = zcalloc(size, 1);

        uint64_t written = multiroarSerialize(r, buf, size);
        if (written == 0) {
            ERRR("Failed to serialize single bit");
        }

        multiroar *r2 = multiroarDeserialize(buf, written);
        if (!r2) {
            ERRR("Failed to deserialize single bit");
        }

        if (!multiroarBitGet(r2, 42)) {
            ERRR("Bit 42 not set after deserialization");
        }

        if (!multiroarEquals(r, r2)) {
            ERRR("Deserialized roar doesn't match original");
        }

        zfree(buf);
        multiroarFree(r);
        multiroarFree(r2);
    }

    TEST("multiroarSerialize/Deserialize - sparse chunk (UNDER_FULL)") {
        multiroar *r = multiroarBitNew();

        /* Set 50 bits in one chunk */
        for (uint64_t i = 0; i < 100; i += 2) {
            multiroarBitSet(r, i);
        }

        uint64_t size = multiroarSerializedSize(r);
        void *buf = zcalloc(size, 1);

        uint64_t written = multiroarSerialize(r, buf, size);
        if (written == 0) {
            ERRR("Failed to serialize sparse chunk");
        }

        multiroar *r2 = multiroarDeserialize(buf, written);
        if (!r2) {
            ERRR("Failed to deserialize sparse chunk");
        }

        if (!multiroarEquals(r, r2)) {
            ERRR("Deserialized roar doesn't match original");
        }

        /* Verify bitcount matches */
        if (multiroarBitCount(r) != multiroarBitCount(r2)) {
            ERR("Bitcount mismatch: %" PRIu64 " vs %" PRIu64 "",
                multiroarBitCount(r), multiroarBitCount(r2));
        }

        zfree(buf);
        multiroarFree(r);
        multiroarFree(r2);
    }

    TEST("multiroarSerialize/Deserialize - dense chunk (FULL_BITMAP)") {
        multiroar *r = multiroarBitNew();

        /* Set 4000 bits to force FULL_BITMAP */
        for (uint64_t i = 0; i < 4000; i++) {
            multiroarBitSet(r, i);
        }

        uint64_t size = multiroarSerializedSize(r);
        void *buf = zcalloc(size, 1);

        uint64_t written = multiroarSerialize(r, buf, size);
        if (written == 0) {
            ERRR("Failed to serialize dense chunk");
        }

        multiroar *r2 = multiroarDeserialize(buf, written);
        if (!r2) {
            ERRR("Failed to deserialize dense chunk");
        }

        if (!multiroarEquals(r, r2)) {
            ERRR("Deserialized roar doesn't match original");
        }

        if (multiroarBitCount(r) != multiroarBitCount(r2)) {
            ERR("Bitcount mismatch: %" PRIu64 " vs %" PRIu64 "",
                multiroarBitCount(r), multiroarBitCount(r2));
        }

        zfree(buf);
        multiroarFree(r);
        multiroarFree(r2);
    }

    TEST("multiroarSerialize/Deserialize - inverted chunk (OVER_FULL)") {
        multiroar *r = multiroarBitNew();

        /* Fill entire chunk except 100 bits to force OVER_FULL */
        for (uint64_t i = 0; i < 8192; i++) {
            multiroarBitSet(r, i);
        }
        for (uint64_t i = 0; i < 100; i += 2) {
            multiroarRemove(r, i);
        }

        uint64_t size = multiroarSerializedSize(r);
        void *buf = zcalloc(size, 1);

        uint64_t written = multiroarSerialize(r, buf, size);
        if (written == 0) {
            ERRR("Failed to serialize inverted chunk");
        }

        multiroar *r2 = multiroarDeserialize(buf, written);
        if (!r2) {
            ERRR("Failed to deserialize inverted chunk");
        }

        if (!multiroarEquals(r, r2)) {
            ERRR("Deserialized roar doesn't match original");
        }

        if (multiroarBitCount(r) != multiroarBitCount(r2)) {
            ERR("Bitcount mismatch: %" PRIu64 " vs %" PRIu64 "",
                multiroarBitCount(r), multiroarBitCount(r2));
        }

        zfree(buf);
        multiroarFree(r);
        multiroarFree(r2);
    }

    TEST("multiroarSerialize/Deserialize - full chunk (ALL_1)") {
        multiroar *r = multiroarBitNew();

        /* Fill entire chunk */
        for (uint64_t i = 0; i < 8192; i++) {
            multiroarBitSet(r, i);
        }

        uint64_t size = multiroarSerializedSize(r);
        void *buf = zcalloc(size, 1);

        uint64_t written = multiroarSerialize(r, buf, size);
        if (written == 0) {
            ERRR("Failed to serialize full chunk");
        }

        multiroar *r2 = multiroarDeserialize(buf, written);
        if (!r2) {
            ERRR("Failed to deserialize full chunk");
        }

        if (!multiroarEquals(r, r2)) {
            ERRR("Deserialized roar doesn't match original");
        }

        if (multiroarBitCount(r) != multiroarBitCount(r2)) {
            ERR("Bitcount mismatch: %" PRIu64 " vs %" PRIu64 "",
                multiroarBitCount(r), multiroarBitCount(r2));
        }

        zfree(buf);
        multiroarFree(r);
        multiroarFree(r2);
    }

    TEST("multiroarSerialize/Deserialize - multiple chunks") {
        multiroar *r = multiroarBitNew();

        /* Create multiple chunks with different types */
        /* Chunk 0: sparse */
        for (uint64_t i = 0; i < 100; i += 3) {
            multiroarBitSet(r, i);
        }

        /* Chunk 1: dense */
        for (uint64_t i = 8192; i < 12192; i++) {
            multiroarBitSet(r, i);
        }

        /* Chunk 5: sparse */
        for (uint64_t i = 5 * 8192; i < 5 * 8192 + 50; i += 2) {
            multiroarBitSet(r, i);
        }

        uint64_t size = multiroarSerializedSize(r);
        void *buf = zcalloc(size, 1);

        uint64_t written = multiroarSerialize(r, buf, size);
        if (written == 0) {
            ERRR("Failed to serialize multiple chunks");
        }

        multiroar *r2 = multiroarDeserialize(buf, written);
        if (!r2) {
            ERRR("Failed to deserialize multiple chunks");
        }

        if (!multiroarEquals(r, r2)) {
            ERRR("Deserialized roar doesn't match original");
        }

        if (multiroarBitCount(r) != multiroarBitCount(r2)) {
            ERR("Bitcount mismatch: %" PRIu64 " vs %" PRIu64 "",
                multiroarBitCount(r), multiroarBitCount(r2));
        }

        zfree(buf);
        multiroarFree(r);
        multiroarFree(r2);
    }

    TEST("multiroarSerialize - buffer too small") {
        multiroar *r = multiroarBitNew();
        multiroarBitSet(r, 42);

        /* Try to serialize with insufficient buffer */
        uint8_t buf[4]; /* Too small */
        uint64_t written = multiroarSerialize(r, buf, sizeof(buf));

        if (written != 0) {
            ERR("Should fail with buffer too small, but wrote %" PRIu64
                " bytes",
                written);
        }

        multiroarFree(r);
    }

    TEST("multiroarDeserialize - invalid magic") {
        uint8_t buf[10] = {'X', 'X', 'X', 'X', 1, 0, 0};

        multiroar *r = multiroarDeserialize(buf, sizeof(buf));
        if (r != NULL) {
            ERRR("Should fail with invalid magic");
            multiroarFree(r);
        }
    }

    TEST("multiroarDeserialize - invalid version") {
        uint8_t buf[10] = {'R', 'O', 'A', 'R', 99, 0, 0}; /* Version 99 */

        multiroar *r = multiroarDeserialize(buf, sizeof(buf));
        if (r != NULL) {
            ERRR("Should fail with invalid version");
            multiroarFree(r);
        }
    }

    TEST("multiroarSerialize/Deserialize - round-trip iterator verification") {
        multiroar *r = multiroarBitNew();

        /* Create diverse bitmap */
        for (uint64_t i = 0; i < 10000; i += 7) {
            multiroarBitSet(r, i);
        }

        uint64_t size = multiroarSerializedSize(r);
        void *buf = zcalloc(size, 1);

        uint64_t written = multiroarSerialize(r, buf, size);
        if (written == 0) {
            ERRR("Serialization failed");
        }

        multiroar *r2 = multiroarDeserialize(buf, written);
        if (!r2) {
            ERRR("Deserialization failed");
        }

        /* Verify using iterator */
        multiroarIterator iter1, iter2;
        multiroarIteratorInit(r, &iter1);
        multiroarIteratorInit(r2, &iter2);

        uint64_t pos1, pos2;
        int count = 0;
        while (true) {
            bool has1 = multiroarIteratorNext(&iter1, &pos1);
            bool has2 = multiroarIteratorNext(&iter2, &pos2);

            if (has1 != has2) {
                ERR("Iterator mismatch at position %d", count);
            }

            if (!has1) {
                break; /* Both exhausted */
            }

            if (pos1 != pos2) {
                ERR("Position mismatch: %" PRIu64 " vs %" PRIu64, pos1, pos2);
            }

            count++;
        }

        zfree(buf);
        multiroarFree(r);
        multiroarFree(r2);
    }

    TEST_DESC("serialization fuzzing - %d trials", 200) {
        for (int trial = 0; trial < 200; trial++) {
            multiroar *r = multiroarBitNew();

            /* Create random bitmap */
            int numBits = 10 + (rand() % 5000);
            for (int i = 0; i < numBits; i++) {
                uint64_t pos = ((uint64_t)rand() * rand() * rand()) % 1000000;
                multiroarBitSet(r, pos);
            }

            uint64_t size = multiroarSerializedSize(r);
            void *buf = zcalloc(size, 1);

            uint64_t written = multiroarSerialize(r, buf, size);
            if (written == 0) {
                ERR("Trial %d: Serialization failed", trial);
            }

            multiroar *r2 = multiroarDeserialize(buf, written);
            if (!r2) {
                ERR("Trial %d: Deserialization failed", trial);
            }

            if (!multiroarEquals(r, r2)) {
                ERR("Trial %d: Round-trip equality failed", trial);
            }

            if (multiroarBitCount(r) != multiroarBitCount(r2)) {
                ERR("Trial %d: Bitcount mismatch", trial);
            }

            zfree(buf);
            multiroarFree(r);
            multiroarFree(r2);
        }
    }

    printf("\n=== Serialization tests completed! ===\n\n");

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
