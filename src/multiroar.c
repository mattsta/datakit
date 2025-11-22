// #define DATAKIT_TEST_VERBOSE

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

    /* If converting Unset Positions, start with a fully set bitmap. */
    if (!convertToSetPositions) {
        memset(newBitmap, 1, sizeof(newBitmap));
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
            uint16_t packedArrayCount = insertPositionalNumber(
                r, &key, &value, &me, DIRECT_BIT_POSITION(position));

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

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
