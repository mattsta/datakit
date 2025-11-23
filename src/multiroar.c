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

static const uint64_t _allZeroes[BITMAP_SIZE_IN_BYTES / sizeof(uint64_t)] = {0};
static const uint64_t _allOnes[BITMAP_SIZE_IN_BYTES / sizeof(uint64_t)] = {
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

#define divCeil(a, b) (((a) + (b)-1) / (b))

/* This MAX_ENTRIES calculation makes sure we don't store more elements in
 * our explicit lists than would be used by just using a bitmap directly. */
#define MAX_ENTRIES_PER_DIRECT_LISTING                                         \
    ((BITMAP_SIZE_IN_BITS / DIRECT_STORAGE_BITS) - 1)

#define BYTES_FOR_PACKED_ARRAY_COUNT(count) divCeil(count, 8)

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

#define _metaCols(r) varintSplitFullGet(_metaOffsetToColValue_(r))
#define _metaRows(r) varintSplitFullGet(_metaOffsetToRowValue_(r))

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

multiroar *multiroarValueNew(const uint8_t bitWidth, const size_t rows,
                             const size_t cols) {
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

    D("count: %" PRIu16 ", len: %zu (required: %f)\n", currentElementCount,
      (size_t)GET_CHUNK_PACKED_LEN(value), ((double)currentElementCount * 13) / 8);
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
     * Use 0xFF to set all bits to 1 (not 0x01 which only sets 1 bit per byte) */
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

        const size_t byteOffset = BYTE_OFFSET(position);
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

DK_STATIC void convertNegativePositionPackedArrayToBitmap(multiroar *r,
                                                          const databox *key,
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
    const size_t lenAsLong = BITMAP_SIZE_IN_BYTES / (sizeof(unsigned long));
    size_t idx = 0;

    for (size_t i = 0; i < lenAsLong; i++) {
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
            D("setting [%zu] = %zu\n", idx, i * 64 + (size_t)r);
            varintPacked13Set(positions, idx++, i * 64 + r);
            myword ^= unsetAfterCheck;
        }
    }
    return idx;
}

DK_STATIC inline uint16_t bitmapToSetPositions(const void *bitmap,
                                               uint8_t positions[]) {
    return _bitmapToPositions(bitmap, positions, true);
}

DK_STATIC inline uint16_t bitmapToNegativePositions(const void *bitmap,
                                                    uint8_t positions[]) {
    return _bitmapToPositions(bitmap, positions, false);
}

DK_STATIC void convertBitmapToPositionList(multiroar *r, const databox *key,
                                           const databox *value,
                                           multimapEntry *me,
                                           bool trackSetPositions) {
    uint8_t packedArray[BITMAP_SIZE_IN_BYTES + 16] = {0};
    uint8_t *packedArrayStart = packedArray + 1;

    if (trackSetPositions) {
        packedArray[0] = CHUNK_TYPE_UNDER_FULL_DIRECT_POSITION_NUMBERS;
    } else {
        packedArray[0] = CHUNK_TYPE_OVER_FULL_DIRECT_NOT_SET_POSITION_NUMBERS;
    }

    const int64_t packedArrayCount = _bitmapToPositions(
        GET_CHUNK_BITMAP_START(value), packedArrayStart, trackSetPositions);

    const size_t moveArrayBytes = divCeil(packedArrayCount * 13, 8);
    memmove(packedArray + 3, packedArray + 1, moveArrayBytes);

    varintTaggedPut64(packedArray + 1, packedArrayCount);

    const databox box = {.data.bytes.start = packedArray,
                         .len = 1 + 2 + moveArrayBytes,
                         .type = DATABOX_BYTES};
    multimapReplaceEntry(&r->map, me, &box);
}

DK_STATIC void convertBitmapToSparsePositionPackedArray(multiroar *r,
                                                        const databox *key,
                                                        const databox *value,
                                                        multimapEntry *me) {
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

bool multiroarBitSet(multiroar *r, size_t position) {
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
            const size_t byteOffset =
                BYTE_OFFSET(GLOBAL_POSITION_TO_CHUNK_POSITION(position));
            const uint32_t bitOffset = BIT_OFFSET(position);
            uint8_t *bitmapStart = GET_CHUNK_BITMAP_START(&value);

            D("Byte offset: %zu, bit offset: %" PRIu32 "\n", byteOffset, bitOffset);
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
        D("EEEEEEEEEEEEEEEEELSE AT CHUNK %zu!\n", CHUNK(position));
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
bool multiroarBitGet(multiroar *r, size_t position) {
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
    D("At CHUNK: %zu (%zu bytes)\n", CHUNK(position), multimapBytes(r->map));
    if (multimapLookup(r->map, &key, values)) {
        switch (GET_CHUNK_TYPE(&value)) {
        case CHUNK_TYPE_ALL_1:
            return true;
            break;
        case CHUNK_TYPE_UNDER_FULL_DIRECT_POSITION_NUMBERS: {
            D("Looking up direct (%" PRIu64 ", %" PRIu64 ")...\n",
              (uint64_t)PACKED_COUNT_FROM_VALUE(&value), (uint64_t)DIRECT_BIT_POSITION(position));
            if (varintPacked13Member(GET_CHUNK_PACKED_START(&value),
                                     PACKED_COUNT_FROM_VALUE(&value),
                                     DIRECT_BIT_POSITION(position)) >= 0) {
                return true;
            }
            break;
        }
        case CHUNK_TYPE_FULL_BITMAP: {
            D("Looking up bitmap...\n");
            const size_t byteOffset =
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
 * Logical Operations
 * ==================================================================== */

/* ====================================================================
 * Testing
 * ==================================================================== */

#ifdef DATAKIT_TEST

#include "ctest.h"
#include "timeUtil.h"

void multiroarRepr(multiroar *r, size_t highest) {
    for (size_t i = 0; i < divCeil(highest, BITMAP_SIZE_IN_BITS); i++) {
        databox value;
        databox *values[1] = {&value};
        if (multimapLookup(r->map,
                           &(databox){.data.u = i, .type = DATABOX_UNSIGNED_64},
                           values)) {
            printf("[%zu] = %d (%" PRIu64 " byte%s)\n", i,
                   GET_CHUNK_TYPE(&value), value.len,
                   value.len == 1 ? "" : "s");
        }
    }
}

void multiroarTestCompare(multiroar *r, size_t highest) {
    const size_t b = multimapBytes(r->map);
    printf("Final size: %zu bytes; Highest bit set: %zu; Size if linear: %zu "
           "bytes; Savings: %.2fx\n",
           b, highest, highest / 8, ((float)(highest / 8) / b) - 1);

    const size_t maps = multimapCount(r->map) / 2;
    size_t asPositional = b;
    size_t minusMaps = maps;
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

    printf("With positional encoding, it would use %zu bytes and be a savings "
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
            const size_t position = (size_t)rand() % SIZE_MAX;

            multiroar *r = multiroarBitNew();

            const bool previouslySet = multiroarBitSet(r, position);

            if (previouslySet) {
                ERR("Detected previously set on new assignment at position "
                    "%zu!",
                    position);
            }

            const bool currentlySet = multiroarBitGet(r, position);

            if (!currentlySet) {
                ERR("Didn't find set bit after setting the bit at position "
                    "%zu!",
                    position);
            }

            multiroarFree(r);
        }
    }

    TEST_DESC("set and get (%d of (SMALL) random positions; common roar)",
              lots) {
        multiroar *r = multiroarBitNew();
        size_t highest = 0;
        for (int32_t i = 0; i < lots; i++) {
            const size_t position = ((size_t)rand() % UINT16_MAX);

            if (position > highest) {
                highest = position;
            }

            multiroarBitSet(r, position);
            const bool currentlySet = multiroarBitGet(r, position);

            if (!currentlySet) {
                ERR("Didn't find set bit after setting the bit at position "
                    "%zu!",
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
        size_t highest = 0;
        for (int32_t i = 0; i < lots; i++) {
            const size_t position = ((size_t)rand());

            if (position > highest) {
                highest = position;
            }

            multiroarBitSet(r, position);
            const bool currentlySet = multiroarBitGet(r, position);

            if (!currentlySet) {
                ERR("Didn't find set bit after setting the bit at position "
                    "%zu!",
                    position);
            }
        }
        multiroarTestCompare(r, highest);
        multiroarFree(r);
    }

    TEST_DESC("set and get (%d of (BIG) random positions; common roar)", lots) {
        multiroar *r = multiroarBitNew();
        size_t highest = 0;
        for (int32_t i = 0; i < lots; i++) {
            const size_t position = ((size_t)rand() * rand()) % SIZE_MAX;

            if (position > highest) {
                highest = position;
            }

            multiroarBitSet(r, position);
            const bool currentlySet = multiroarBitGet(r, position);

            if (!currentlySet) {
                ERR("Didn't find set bit after setting the bit at position "
                    "%zu!",
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
        const size_t boundaries[] = {0, 8191, 8192, 8193, 16383, 16384, 16385,
                                     24575, 24576, 24577};
        const size_t numBoundaries = sizeof(boundaries) / sizeof(boundaries[0]);

        /* Set all boundary bits */
        for (size_t i = 0; i < numBoundaries; i++) {
            multiroarBitSet(r, boundaries[i]);
        }

        /* Verify all boundary bits are set */
        for (size_t i = 0; i < numBoundaries; i++) {
            if (!multiroarBitGet(r, boundaries[i])) {
                ERR("Boundary bit %zu not set!", boundaries[i]);
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
        const size_t testCount = 1000; /* Well above 629 conversion threshold */

        for (size_t i = 0; i < testCount; i++) {
            multiroarBitSet(r, i);
            if (!multiroarBitGet(r, i)) {
                ERR("Bit %zu NOT SET immediately after setting!", i);
            }
        }

        /* Verify all bits are still set */
        for (size_t i = 0; i < testCount; i++) {
            if (!multiroarBitGet(r, i)) {
                ERR("Bit %zu lost after all sets!", i);
            }
        }

        multiroarFree(r);
    }

    TEST("conversion from UNDER_FULL to FULL_BITMAP") {
        /* Test that conversion to full bitmap preserves all bits */
        multiroar *r = multiroarBitNew();
        const size_t maxDirect = 629; /* MAX_ENTRIES_PER_DIRECT_LISTING */

        /* Set exactly maxDirect positions to trigger conversion to bitmap */
        size_t *positions = zcalloc(maxDirect, sizeof(size_t));
        for (size_t i = 0; i < maxDirect; i++) {
            positions[i] = i * 10; /* Spread positions to stay in one chunk */
            multiroarBitSet(r, positions[i]);
        }

        /* Verify all positions are still set after potential conversion */
        for (size_t i = 0; i < maxDirect; i++) {
            if (!multiroarBitGet(r, positions[i])) {
                ERR("Position %zu lost after bitmap conversion!", positions[i]);
            }
        }

        /* Verify non-set positions are not set */
        for (size_t i = 0; i < maxDirect - 1; i++) {
            for (size_t j = positions[i] + 1; j < positions[i + 1]; j++) {
                if (multiroarBitGet(r, j)) {
                    ERR("Position %zu incorrectly set!", j);
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
        const size_t positions[] = {0, 100, 200, 300, 400, 500, 600, 700, 800,
                                    900, 1000, 1500, 2000, 2500, 3000, 3500,
                                    4000, 4500, 5000, 5500, 6000, 6500, 7000};
        const size_t numPositions = sizeof(positions) / sizeof(positions[0]);

        /* First, fill up to trigger bitmap conversion */
        for (size_t i = 0; i < 700; i++) {
            multiroarBitSet(r, i);
        }

        /* Set some additional positions */
        for (size_t i = 0; i < numPositions; i++) {
            multiroarBitSet(r, positions[i]);
        }

        /* Verify the specific positions are set */
        for (size_t i = 0; i < numPositions; i++) {
            if (!multiroarBitGet(r, positions[i])) {
                ERR("Position %zu not set!", positions[i]);
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
        const size_t chunkBits = 8192;

        /* Chunk 0: sparse (few bits) */
        multiroarBitSet(r, 100);
        multiroarBitSet(r, 200);
        multiroarBitSet(r, 300);

        /* Chunk 1: moderately filled */
        for (size_t i = chunkBits; i < chunkBits + 500; i++) {
            multiroarBitSet(r, i);
        }

        /* Chunk 2: also moderately filled */
        for (size_t i = chunkBits * 2; i < chunkBits * 2 + 1000; i++) {
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
        for (size_t i = chunkBits; i < chunkBits + 500; i++) {
            if (!multiroarBitGet(r, i)) {
                ERR("Bit %zu in chunk 1 should be set!", i);
            }
        }

        /* Verify chunk 2 */
        for (size_t i = chunkBits * 2; i < chunkBits * 2 + 1000; i++) {
            if (!multiroarBitGet(r, i)) {
                ERR("Bit %zu in chunk 2 should be set!", i);
            }
        }
        if (multiroarBitGet(r, chunkBits * 2 + 1500)) {
            ERRR("Unset bit in chunk 2 incorrectly set!");
        }

        multiroarFree(r);
    }

    TEST("high-scale stress test (100K random bits)") {
        multiroar *r = multiroarBitNew();
        const size_t scale = 100000;
        const size_t sampleSize = 10000; /* Track a sample for verification */
        size_t *sampleBits = zcalloc(sampleSize, sizeof(size_t));

        /* Set random bits, tracking a sample for verification */
        for (size_t i = 0; i < scale; i++) {
            const size_t position = ((size_t)rand() * rand()) % (SIZE_MAX / 2);

            /* Track every Nth position for verification */
            if (i < sampleSize) {
                sampleBits[i] = position;
            }

            multiroarBitSet(r, position);

            /* Verify immediately after set */
            if (!multiroarBitGet(r, position)) {
                ERR("High-scale: bit %zu not set immediately after setting!",
                    position);
            }
        }

        /* Verify sample bits are still set after all operations */
        size_t verified = 0;
        for (size_t i = 0; i < sampleSize; i++) {
            if (multiroarBitGet(r, sampleBits[i])) {
                verified++;
            } else {
                ERR("High-scale: sample bit %zu not set!", sampleBits[i]);
            }
        }

        if (verified != sampleSize) {
            ERR("High-scale: only %zu of %zu sample bits verified!", verified,
                sampleSize);
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
        const size_t scale = 100000;
        multiroar *r = multiroarBitNew();

        int64_t startNs = timeUtilMonotonicNs();
        for (size_t i = 0; i < scale; i++) {
            multiroarBitSet(r, i);
        }
        int64_t elapsed = timeUtilMonotonicNs() - startNs;
        printf("Sequential insert: %.1f ns/op, %.0f ops/sec\n",
               (double)elapsed / scale, scale / (elapsed / 1e9));

        multiroarFree(r);
    }

    TEST("PERF: random insert performance") {
        const size_t scale = 100000;
        multiroar *r = multiroarBitNew();

        int64_t startNs = timeUtilMonotonicNs();
        for (size_t i = 0; i < scale; i++) {
            multiroarBitSet(r, ((size_t)rand() * rand()) % (SIZE_MAX / 2));
        }
        int64_t elapsed = timeUtilMonotonicNs() - startNs;
        printf("Random insert: %.1f ns/op, %.0f ops/sec\n",
               (double)elapsed / scale, scale / (elapsed / 1e9));

        multiroarFree(r);
    }

    TEST("PERF: lookup performance (dense)") {
        const size_t scale = 100000;
        multiroar *r = multiroarBitNew();

        /* Create dense bitmap */
        for (size_t i = 0; i < scale; i++) {
            multiroarBitSet(r, i);
        }

        int64_t startNs = timeUtilMonotonicNs();
        for (size_t i = 0; i < scale; i++) {
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
        const size_t maxDirect = 629; /* MAX_ENTRIES_PER_DIRECT_LISTING */

        /* Insert exactly 628 entries (one below threshold) */
        for (size_t i = 0; i < maxDirect - 1; i++) {
            multiroarBitSet(r, i * 13); /* Spread to stay in one chunk */
        }

        /* Verify all are still set (should still be UNDER_FULL) */
        for (size_t i = 0; i < maxDirect - 1; i++) {
            if (!multiroarBitGet(r, i * 13)) {
                ERR("Position %zu not set before threshold!", i * 13);
            }
        }

        /* Insert the 629th entry - triggers conversion to FULL_BITMAP */
        multiroarBitSet(r, (maxDirect - 1) * 13);

        /* Verify all entries including the new one */
        for (size_t i = 0; i < maxDirect; i++) {
            if (!multiroarBitGet(r, i * 13)) {
                ERR("Position %zu not set after conversion!", i * 13);
            }
        }

        multiroarFree(r);
    }

    TEST("FULL_BITMAP exact boundary positions") {
        /* Test bits at exact byte and word boundaries within bitmap */
        multiroar *r = multiroarBitNew();

        /* Fill to get into bitmap mode */
        for (size_t i = 0; i < 700; i++) {
            multiroarBitSet(r, i);
        }

        /* Test byte boundary positions: 0, 7, 8, 15, 16, ... */
        const size_t byteBoundaries[] = {0, 7, 8, 15, 16, 23, 24, 31, 32,
                                          63, 64, 127, 128, 255, 256, 511, 512};
        for (size_t i = 0; i < sizeof(byteBoundaries)/sizeof(byteBoundaries[0]); i++) {
            size_t pos = byteBoundaries[i];
            if (pos < 700) {
                if (!multiroarBitGet(r, pos)) {
                    ERR("Byte boundary position %zu should be set!", pos);
                }
            }
        }

        /* Test 64-bit word boundary positions */
        const size_t wordBoundaries[] = {63, 64, 127, 128, 191, 192, 255, 256,
                                          319, 320, 383, 384, 447, 448, 511, 512};
        for (size_t i = 0; i < sizeof(wordBoundaries)/sizeof(wordBoundaries[0]); i++) {
            size_t pos = wordBoundaries[i];
            if (pos < 700) {
                if (!multiroarBitGet(r, pos)) {
                    ERR("Word boundary position %zu should be set!", pos);
                }
            }
        }

        multiroarFree(r);
    }

    TEST("sparse positions across chunk boundary") {
        /* Test sparse positions that span multiple chunks */
        multiroar *r = multiroarBitNew();
        const size_t chunkBits = 8192;

        /* Set sparse positions across 4 chunks */
        const size_t positions[] = {
            0, 100, 1000,                           /* Chunk 0 */
            chunkBits + 50, chunkBits + 500,        /* Chunk 1 */
            chunkBits * 2 + 1, chunkBits * 2 + 999, /* Chunk 2 */
            chunkBits * 3 + 8191                    /* Chunk 3, last bit */
        };
        const size_t numPositions = sizeof(positions) / sizeof(positions[0]);

        for (size_t i = 0; i < numPositions; i++) {
            multiroarBitSet(r, positions[i]);
        }

        /* Verify all positions */
        for (size_t i = 0; i < numPositions; i++) {
            if (!multiroarBitGet(r, positions[i])) {
                ERR("Sparse cross-chunk position %zu not set!", positions[i]);
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
        for (size_t i = 0; i < 100; i++) {
            size_t pos = i * 100; /* Spread positions */
            multiroarBitSet(r, pos);

            /* Immediate verification */
            if (!multiroarBitGet(r, pos)) {
                ERR("UNDER_FULL: position %zu not set immediately!", pos);
            }

            /* Verify previously set positions still exist */
            for (size_t j = 0; j <= i; j++) {
                if (!multiroarBitGet(r, j * 100)) {
                    ERR("UNDER_FULL: earlier position %zu lost!", j * 100);
                }
            }
        }

        multiroarFree(r);
    }

    TEST("set and immediate re-read in FULL_BITMAP mode") {
        /* Test that set followed by immediate get works in FULL_BITMAP */
        multiroar *r = multiroarBitNew();

        /* First, fill to trigger bitmap mode */
        for (size_t i = 0; i < 700; i++) {
            multiroarBitSet(r, i);
        }

        /* Now set additional positions and verify immediately */
        for (size_t i = 700; i < 1000; i++) {
            multiroarBitSet(r, i);

            if (!multiroarBitGet(r, i)) {
                ERR("FULL_BITMAP: position %zu not set immediately!", i);
            }
        }

        /* Verify all positions are still set */
        for (size_t i = 0; i < 1000; i++) {
            if (!multiroarBitGet(r, i)) {
                ERR("FULL_BITMAP: position %zu lost!", i);
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
        for (size_t i = 0; i < 700; i++) {
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
        const size_t largePositions[] = {
            SIZE_MAX - 1,
            SIZE_MAX - 8192,
            SIZE_MAX - 8193,
            SIZE_MAX / 2,
            SIZE_MAX / 2 + 1
        };
        const size_t numLarge = sizeof(largePositions) / sizeof(largePositions[0]);

        for (size_t i = 0; i < numLarge; i++) {
            multiroarBitSet(r, largePositions[i]);
        }

        for (size_t i = 0; i < numLarge; i++) {
            if (!multiroarBitGet(r, largePositions[i])) {
                ERR("Large position %zu not set!", largePositions[i]);
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
        const size_t testRange = 2000;

        /* Set even positions first */
        for (size_t i = 0; i < testRange; i += 2) {
            multiroarBitSet(r, i);
        }

        /* Verify even positions are set, odd are not */
        for (size_t i = 0; i < testRange; i++) {
            bool isSet = multiroarBitGet(r, i);
            if (i % 2 == 0 && !isSet) {
                ERR("Even position %zu should be set!", i);
            }
            if (i % 2 == 1 && isSet) {
                ERR("Odd position %zu should NOT be set!", i);
            }
        }

        /* Now set odd positions */
        for (size_t i = 1; i < testRange; i += 2) {
            multiroarBitSet(r, i);
        }

        /* Verify all positions are now set */
        for (size_t i = 0; i < testRange; i++) {
            if (!multiroarBitGet(r, i)) {
                ERR("After fill: position %zu should be set!", i);
            }
        }

        multiroarFree(r);
    }

    TEST("reverse order insertion") {
        /* Insert bits in reverse order */
        multiroar *r = multiroarBitNew();
        const size_t testCount = 1000;

        for (size_t i = testCount; i > 0; i--) {
            multiroarBitSet(r, i - 1);

            /* Verify this position is set */
            if (!multiroarBitGet(r, i - 1)) {
                ERR("Reverse insert: position %zu not set!", i - 1);
            }
        }

        /* Verify all positions are set */
        for (size_t i = 0; i < testCount; i++) {
            if (!multiroarBitGet(r, i)) {
                ERR("Reverse insert: position %zu lost!", i);
            }
        }

        multiroarFree(r);
    }

    TEST("random order insertion with verification") {
        /* Insert bits in pseudo-random order using LCG */
        multiroar *r = multiroarBitNew();
        const size_t testCount = 500;
        size_t *positions = zcalloc(testCount, sizeof(size_t));

        /* Generate pseudo-random positions */
        uint32_t seed = 12345;
        for (size_t i = 0; i < testCount; i++) {
            seed = seed * 1103515245 + 12345;
            positions[i] = (seed >> 8) % 10000;
        }

        /* Insert all positions */
        for (size_t i = 0; i < testCount; i++) {
            multiroarBitSet(r, positions[i]);
        }

        /* Verify all positions are set */
        for (size_t i = 0; i < testCount; i++) {
            if (!multiroarBitGet(r, positions[i])) {
                ERR("Random order: position %zu not set!", positions[i]);
            }
        }

        zfree(positions);
        multiroarFree(r);
    }

    TEST("chunk type transitions (UNDER_FULL -> FULL_BITMAP)") {
        /* Carefully test the transition from UNDER_FULL to FULL_BITMAP */
        multiroar *r = multiroarBitNew();

        /* Insert 628 positions (just under threshold) */
        for (size_t i = 0; i < 628; i++) {
            multiroarBitSet(r, i);
            if (!multiroarBitGet(r, i)) {
                ERR("Pre-transition: position %zu not set!", i);
            }
        }

        /* Insert 629th position - triggers transition */
        multiroarBitSet(r, 628);
        if (!multiroarBitGet(r, 628)) {
            ERRR("Transition position 628 not set!");
        }

        /* Verify all previous positions survived transition */
        for (size_t i = 0; i < 629; i++) {
            if (!multiroarBitGet(r, i)) {
                ERR("Post-transition: position %zu lost!", i);
            }
        }

        /* Continue inserting to verify bitmap mode works */
        for (size_t i = 629; i < 1000; i++) {
            multiroarBitSet(r, i);
            if (!multiroarBitGet(r, i)) {
                ERR("Bitmap mode: position %zu not set!", i);
            }
        }

        multiroarFree(r);
    }

    TEST("OVER_FULL transition (FULL_BITMAP -> negative list)") {
        /* Test transition to OVER_FULL when most bits are set */
        multiroar *r = multiroarBitNew();
        const size_t chunkBits = 8192;
        const size_t threshold = 7564; /* Just past OVER_FULL threshold */

        /* Set bits sequentially up to threshold */
        for (size_t i = 0; i < threshold; i++) {
            multiroarBitSet(r, i);

            /* Verify immediately */
            if (!multiroarBitGet(r, i)) {
                ERR("OVER_FULL transition: position %zu not set immediately!", i);
            }
        }

        /* Verify all positions still set after potential transition */
        size_t failures = 0;
        for (size_t i = 0; i < threshold; i++) {
            if (!multiroarBitGet(r, i)) {
                if (failures < 10) {
                    ERR("OVER_FULL: position %zu lost!", i);
                }
                failures++;
            }
        }
        if (failures > 10) {
            printf("... and %zu more positions lost\n", failures - 10);
        }

        /* Verify positions past threshold are NOT set */
        for (size_t i = threshold; i < chunkBits; i += 100) {
            if (multiroarBitGet(r, i)) {
                ERR("OVER_FULL: position %zu incorrectly set!", i);
            }
        }

        multiroarFree(r);
    }

    TEST("ALL_1 transition (fill entire chunk)") {
        /* Fill an entire chunk to trigger ALL_1 type */
        multiroar *r = multiroarBitNew();
        const size_t chunkBits = 8192;

        printf("    Filling entire chunk (%zu bits)...\n", chunkBits);

        /* Set all bits in chunk 0 */
        for (size_t i = 0; i < chunkBits; i++) {
            multiroarBitSet(r, i);

            /* Periodic verification */
            if (i % 1000 == 0 || i == chunkBits - 1) {
                if (!multiroarBitGet(r, i)) {
                    ERR("ALL_1 fill: position %zu not set!", i);
                }
            }
        }

        /* Verify all bits in chunk are set */
        size_t failures = 0;
        for (size_t i = 0; i < chunkBits; i++) {
            if (!multiroarBitGet(r, i)) {
                if (failures < 5) {
                    ERR("ALL_1 verify: position %zu not set!", i);
                }
                failures++;
            }
        }
        if (failures > 5) {
            printf("    ... and %zu more bits not set (total failures: %zu)\n",
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
        const size_t chunkBits = 8192;

        /* Chunk 0: UNDER_FULL (sparse, few bits) */
        multiroarBitSet(r, 10);
        multiroarBitSet(r, 100);
        multiroarBitSet(r, 500);

        /* Chunk 1: FULL_BITMAP (moderate density) */
        for (size_t i = chunkBits; i < chunkBits + 2000; i++) {
            multiroarBitSet(r, i);
        }

        /* Chunk 2: High density (triggers OVER_FULL if working) */
        for (size_t i = chunkBits * 2; i < chunkBits * 2 + 7600; i++) {
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
        for (size_t i = chunkBits; i < chunkBits + 2000; i += 100) {
            if (!multiroarBitGet(r, i)) {
                ERR("Chunk 1 position %zu not set!", i);
            }
        }

        /* Verify chunk 2 */
        for (size_t i = chunkBits * 2; i < chunkBits * 2 + 7600; i += 100) {
            if (!multiroarBitGet(r, i)) {
                ERR("Chunk 2 position %zu not set!", i);
            }
        }

        multiroarFree(r);
    }

    TEST("packed array sorted insertion stress") {
        /* Stress test the sorted insertion in UNDER_FULL mode */
        multiroar *r = multiroarBitNew();

        /* Insert positions that stress the sorted insert algorithm */
        const size_t positions[] = {
            500, 100, 900, 50, 950, 25, 975, 12, 988, 6, 994, 3, 997, 1, 999,
            0, 1000, 2, 998, 4, 996, 8, 992, 16, 984, 32, 968, 64, 936, 128,
            872, 256, 744, 512
        };
        const size_t numPos = sizeof(positions) / sizeof(positions[0]);

        for (size_t i = 0; i < numPos; i++) {
            multiroarBitSet(r, positions[i]);
        }

        /* Verify all positions are set */
        for (size_t i = 0; i < numPos; i++) {
            if (!multiroarBitGet(r, positions[i])) {
                ERR("Sorted insert stress: position %zu not set!", positions[i]);
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
        const size_t numPositions = 1000;
        const size_t positionSpread = 1000000;
        multiroar *r = multiroarBitNew();

        /* Create sparse bitmap across many chunks */
        for (size_t i = 0; i < numPositions; i++) {
            multiroarBitSet(r, i * positionSpread);
        }

        int64_t startNs = timeUtilMonotonicNs();
        for (size_t round = 0; round < 100; round++) {
            for (size_t i = 0; i < numPositions; i++) {
                multiroarBitGet(r, i * positionSpread);
            }
        }
        int64_t elapsed = timeUtilMonotonicNs() - startNs;
        printf("Sparse lookup: %.1f ns/op, %.0f ops/sec\n",
               (double)elapsed / (numPositions * 100),
               (numPositions * 100) / (elapsed / 1e9));

        multiroarFree(r);
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
