// #define DK_TEST_VERBOSE 1

#include "multiroar.h"
#include "../deps/varint/src/varintSplitFull.h"
#include "../deps/varint/src/varintTagged.h"

#include "cleaner.h"
#include "str.h" /* optimized popcnt helpers */
#include <inttypes.h>
#include <stdlib.h> /* qsort */

defcleanerName(uint8_t *, zfree, Bytes);
defcleanerName(uint64_t *, zfree, UInt64);
defcleanerName(multiroar **, zfree, RoarPtrs);
defcleaner(multiroar *, multiroarFree);

#ifdef DATAKIT_TEST
defcleanerName(bool *, zfree, Bools);
defcleanerName(void *, zfree, Void);

/* Runtime-sized array of OWNED multiroars: the scope-exit cleanup
 * frees every element and then the pointer array itself, so fuzz
 * failure paths (ERR + break) release the inputs too. */
typedef struct roarArray {
    multiroar **items;
    uint64_t count;
} roarArray;

static DK_FN_UNUSED void roarArrayFree_cleanup(roarArray *a) {
    if (a->items) {
        for (uint64_t i = 0; i < a->count; i++) {
            multiroarFree(a->items[i]);
        }
        zfree(a->items);
    }
}
#endif

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
/* This file's own copy of the template functions */
#undef PACKED_STATIC
#define PACKED_STATIC __attribute__((unused)) static
#include "../deps/varint/src/varintPacked.h"
#undef PACKED_STATIC

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
    /* +1: meta[0] == 0 marks BIT mode so value/bit mode is always
     * introspectable (value mode stores its nonzero bitWidth there). */
    /* @own: caller; free=multiroarFree */
    multiroar *r = zcalloc(1, sizeof(*r) + 1);
    /* @own: container; free=multimapFree */
    r->map = multimapNew(ELEMENTS_PER_ENTRY);
    return r;
}

multiroar *multiroarValueNew(const uint8_t bitWidth, const uint64_t rows,
                             const uint64_t cols) {
    /* A value multiroar stores fixed-width unsigned values; zero-width
     * values don't exist and anything wider than 64 can't round-trip
     * through the databox integer we accept. */
    if (bitWidth == 0 || bitWidth > 64) {
        return NULL;
    }

    varintWidth rowWidth;
    varintWidth colWidth;

    varintSplitFullLength_(rowWidth, rows);
    varintSplitFullLength_(colWidth, cols);

    /* Allocate multiroar, which is:
     *   - 1 pointer
     *   - 1 byte for bit width (uint8_t)
     *   - 1 to 9 bytes for col count (SplitFull Varint)
     *   - 1 to 9 bytes for row count (SplitFull Varint) */
    /* @own: caller; free=multiroarFree */
    multiroar *r = zcalloc(1, sizeof(*r) + 1 + colWidth + rowWidth);
    /* @own: container; free=multimapFree */
    r->map = multimapNew(ELEMENTS_PER_ENTRY);

    /* bitWidth must be written BEFORE the row offset is computed:
     * _metaOffsetToRowValue reads the col varint length from meta. */
    uint8_t *valueBitWidth = _metaOffsetToBitWidthValue(r);
    *valueBitWidth = bitWidth;

    /* SplitFull encode to match the SplitFull length/offset math above
     * and in the _metaOffsetTo* readers. */
    varintWidth used;
    varintSplitFullPut_(_metaOffsetToColValue(r), used, cols);
    assert(used == colWidth);
    varintSplitFullPut_(_metaOffsetToRowValue(r), used, rows);
    assert(used == rowWidth);
    (void)used;

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
        multimapLookupInto(r->map, key, value, 1);

        /* Bigger varint means the start of our packed array needs
         * to move down by exactly one byte: the packed bytes are the old
         * value after its type byte and 1-byte count. */
        if (growVarint) {
            memmove(value->data.bytes.start + 3, value->data.bytes.start + 2,
                    (value->len - grow) - 2);
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
    const uint8_t *data = bitmap;
    const uint64_t lenAsWords = BITMAP_SIZE_IN_BYTES / sizeof(uint64_t);
    uint64_t idx = 0;

    for (uint64_t i = 0; i < lenAsWords; i++) {
        /* memcpy load: 'bitmap' is a flex payload pointer with no
         * alignment guarantee; a direct word deref is UB (and traps on
         * strict-alignment targets).  Compiles to one load regardless. */
        uint64_t myword;
        memcpy(&myword, data + (i * sizeof(uint64_t)), sizeof(myword));

        if (!trackSetPositions) {
            myword = ~myword;
        }

        while (myword != 0) {
            /* Work on this 8 byte quantity until we've
             * eaten all the set bits. */
            const uint64_t unsetAfterCheck = myword & -myword;
            const int r = __builtin_ctzll(myword);
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
                 * the old second varint byte we don't need anymore: the
                 * packed bytes are the value after its type byte and
                 * 2-byte count. */
                memmove(value->data.bytes.start + 2,
                        value->data.bytes.start + 3, value->len - 3);
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

    const databox key = databoxNewUnsigned(CHUNK(position));
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
/* Test one position against an already-looked-up chunk value */
static bool chunkTestsBit(const databox *value, uint64_t position) {
    switch (GET_CHUNK_TYPE(value)) {
    case CHUNK_TYPE_ALL_1:
        return true;
    case CHUNK_TYPE_UNDER_FULL_DIRECT_POSITION_NUMBERS: {
        D("Looking up direct (%" PRIu64 ", %" PRIu64 ")...\n",
          (uint64_t)PACKED_COUNT_FROM_VALUE(value),
          (uint64_t)DIRECT_BIT_POSITION(position));
        if (varintPacked13Member(GET_CHUNK_PACKED_START(value),
                                 PACKED_COUNT_FROM_VALUE(value),
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
        const uint8_t *bitmapStart = GET_CHUNK_BITMAP_START(value);

        return (bitmapStart[byteOffset] >> bitOffset) & 0x01;
    }
    case CHUNK_TYPE_OVER_FULL_DIRECT_NOT_SET_POSITION_NUMBERS: {
        if (varintPacked13Member(GET_CHUNK_PACKED_START(value),
                                 PACKED_COUNT_FROM_VALUE(value),
                                 DIRECT_BIT_POSITION(position)) == -1) {
            return true;
        }
        break;
    }
    default:
        D("Type byte is: %d (WHY?)\n", GET_CHUNK_TYPE(value));
        assert(NULL && "Invalid type byte in bitmap!");
    }

    return false;
}

bool multiroarBitGet(const multiroar *r, uint64_t position) {
    /* For position, lookup corresponding chunk; missing chunk = 0. */
    const databox key = databoxNewUnsigned(CHUNK(position));
    databox value;
    D("At CHUNK: %" PRIu64 " (%" PRIu64 " bytes)\n", CHUNK(position),
      multimapBytes(r->map));
    if (multimapLookupInto(r->map, &key, &value, 1)) {
        return chunkTestsBit(&value, position);
    }

    /* else, not found / not set */
    return false;
}

/* ====================================================================
 * Remove (Clear Bit)
 * ==================================================================== */
bool multiroarRemove(multiroar *r, uint64_t position) {
    /* Clear a bit. Returns true if bit was previously set. */
    const databox key = databoxNewUnsigned(CHUNK(position));
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
        /* Sparse list - need to remove position if present.  The shared
         * delete rewrites the count, moving the packed array when the
         * count's varint shrinks (241 -> 240). */
        const uint16_t currentElementCount = PACKED_COUNT_FROM_VALUE(&value);
        const uint16_t newCount = deletePackedArrayMember(
            r, &value, &me, DIRECT_BIT_POSITION(position));
        if (newCount == currentElementCount) {
            /* Bit wasn't set */
            return false;
        }

        if (newCount == 0) {
            /* Chunk is now empty, delete it entirely */
            multimapDelete(&r->map, &key);
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

    /* meta length: 1 mode/bitWidth byte, plus (value mode only) the col
     * and row SplitFull varints. */
    size_t metaLen = 1;
    if (r->meta[0] != 0) {
        const size_t colLen =
            varintSplitFullGetLenQuick_(_metaOffsetToColValue(r));
        metaLen += colLen;
        metaLen += varintSplitFullGetLenQuick_(r->meta + 1 + colLen);
    }

    /* @own: caller; free=multiroarFree */
    multiroar *dup = zcalloc(1, sizeof(*dup) + metaLen);
    memcpy(dup->meta, r->meta, metaLen);
    /* @own: container; free=multimapFree */
    dup->map = multimapCopy(r->map);
    return dup;
}

/* ====================================================================
 * Value Operations (value-mode multiroars from multiroarValueNew)
 * ==================================================================== */
/* Value mode stores 'position -> unsigned value' rows directly in the
 * backing multimap.  Bit-mode chunk keys and value-mode position keys
 * share the same integer keyspace, so bit ops and value ops must never
 * be mixed on one multiroar — the meta mode byte enforces it here.
 * Values round-trip as unsigned databoxes (booleans store as 0/1). */
DK_STATIC void _multiroarValueModeCheck(const multiroar *r,
                                        const uint64_t position) {
    assert(r && r->map);
    assert(r->meta[0] != 0 && "value ops require multiroarValueNew");

    /* rows/cols of 0 mean "unbounded" in that dimension; when both are
     * declared, position must address a cell inside rows * cols. */
    assert(multiroarValueInBounds(r, position) &&
           "position outside declared rows*cols");
    (void)position;
}

DK_STATIC uint64_t _multiroarValueAsUnsigned(const multiroar *r,
                                             const databox *value) {
    uint64_t v;
    switch (value->type) {
    case DATABOX_UNSIGNED_64:
        v = value->data.u;
        break;
    case DATABOX_SIGNED_64:
        assert(value->data.i >= 0 && "value multiroars store unsigned values");
        v = (uint64_t)value->data.i;
        break;
    case DATABOX_TRUE:
        v = 1;
        break;
    case DATABOX_FALSE:
        v = 0;
        break;
    default:
        assert(NULL && "value multiroars store integer values only");
        __builtin_unreachable();
    }

    const uint8_t width = r->meta[0];
    assert(width == 64 || v < (UINT64_C(1) << width));
    (void)width;
    return v;
}

bool multiroarValueSetGetPrevious(multiroar *r, uint64_t position,
                                  databox *value, databox *overwritten) {
    _multiroarValueModeCheck(r, position);
    const uint64_t v = _multiroarValueAsUnsigned(r, value);

    const databox key = databoxNewUnsigned(position);

    databox prev = {{0}};
    databox *lookup[1] = {&prev};
    const bool previouslySet = multimapLookup(r->map, &key, lookup);
    if (previouslySet && overwritten) {
        /* Integer values live by-value inside the box, so this copy
         * stays valid after the insert below mutates the map. */
        *overwritten = prev;
    }

    const databox store = databoxNewUnsigned(v);
    const databox *elements[ELEMENTS_PER_ENTRY] = {&key, &store};
    multimapInsert(&r->map, elements);
    return previouslySet;
}

bool multiroarValueSet(multiroar *r, uint64_t position, databox *value) {
    return multiroarValueSetGetPrevious(r, position, value, NULL);
}

uint8_t multiroarValueWidth(const multiroar *r) {
    return r ? r->meta[0] : 0;
}

void multiroarValueDimensions(const multiroar *r, uint64_t *rows,
                              uint64_t *cols) {
    uint64_t c = 0;
    uint64_t rw = 0;
    if (r && r->meta[0] != 0) {
        varintWidth colLen;
        varintSplitFullGet_(_metaOffsetToColValue(r), colLen, c);
        varintWidth rowLen;
        varintSplitFullGet_(r->meta + 1 + colLen, rowLen, rw);
        (void)rowLen;
    }
    if (rows) {
        *rows = rw;
    }
    if (cols) {
        *cols = c;
    }
}

bool multiroarValueInBounds(const multiroar *r, uint64_t position) {
    uint64_t rows;
    uint64_t cols;
    uint64_t bound;
    multiroarValueDimensions(r, &rows, &cols);
    return !rows || !cols || __builtin_mul_overflow(rows, cols, &bound) ||
           position < bound;
}

uint64_t multiroarValueCount(const multiroar *r) {
    return r && r->meta[0] != 0 ? multimapCount(r->map) : 0;
}

void multiroarClear(multiroar *r) {
    if (r) {
        /* A fresh map: a cleared roar keeps none of the old one's memory */
        multimapFree(r->map);
        /* @own: container; free=multimapFree */
        r->map = multimapNew(ELEMENTS_PER_ENTRY);
    }
}

bool multiroarValueGet(const multiroar *r, uint64_t position, databox *value) {
    _multiroarValueModeCheck(r, position);

    const databox key = databoxNewUnsigned(position);
    databox got = {{0}};
    databox *lookup[1] = {&got};
    if (!multimapLookup(r->map, &key, lookup)) {
        return false;
    }

    if (value) {
        *value = got;
    }

    return true;
}

bool multiroarValueRemoveGetRemoved(multiroar *r, uint64_t position,
                                    databox *removed) {
    _multiroarValueModeCheck(r, position);

    const databox key = databoxNewUnsigned(position);
    databox prev = {{0}};
    databox *lookup[1] = {&prev};
    if (!multimapLookup(r->map, &key, lookup)) {
        return false;
    }

    if (removed) {
        *removed = prev;
    }

    return multimapDelete(&r->map, &key);
}

/* ====================================================================
 * Range Operations
 * ==================================================================== */
typedef enum multiroarRangeOp_ {
    MULTIROAR_RANGE_SET_,
    MULTIROAR_RANGE_CLEAR_,
    MULTIROAR_RANGE_FLIP_,
} multiroarRangeOp_;

/* Defined with the other range operations below */
DK_STATIC void multiroarRangeApply_(multiroar *r, uint64_t start,
                                    uint64_t extent, multiroarRangeOp_ op);

void multiroarBitSetRange(multiroar *r, uint64_t start, uint64_t extent) {
    multiroarRangeApply_(r, start, extent, MULTIROAR_RANGE_SET_);
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

    while (multimapIteratorNextInto(&iter, keyValue, 2)) {
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
/* Forward declaration: defined far below, used by multiroarNewAndNot. */
DK_STATIC void compressBitmapToChunk(multiroar *r, const databox *key,
                                     uint8_t *bitmap, multimapEntry *me,
                                     bool chunkExists);

/* Helper: Find first set bit in chunk bitmap. Scans 64 bits at a time and uses
 * CTZ; bit numbering is byte-array order, LSB-first within byte, which on a
 * little-endian load (arm64/x86-64) is exactly the word's CTZ. */
DK_STATIC uint64_t findFirstSetBitInBitmap(const uint8_t *bitmap,
                                           uint64_t bytes) {
    const uint64_t words = bytes / 8;
    for (uint64_t i = 0; i < words; i++) {
        /* memcpy load: 'bitmap' may point into a flex payload with no
         * alignment guarantee; a direct word deref is UB. */
        uint64_t word;
        memcpy(&word, bitmap + (i * 8), sizeof(word));
        if (word != 0) {
            return i * 64 + (uint64_t)__builtin_ctzll(word);
        }
    }
    for (uint64_t i = words * 8; i < bytes; i++) { /* tail bytes (if any) */
        if (bitmap[i] != 0) {
            return i * 8 + (uint64_t)__builtin_ctz(bitmap[i]);
        }
    }
    return UINT64_MAX;
}

/* Helper: Find last set bit in chunk bitmap. Highest addresses hold highest
 * positions, so scan tail bytes (if any) before whole words; uses CLZ. */
DK_STATIC uint64_t findLastSetBitInBitmap(const uint8_t *bitmap,
                                          uint64_t bytes) {
    const uint64_t words = bytes / 8;
    for (uint64_t i = bytes; i > words * 8; i--) {
        if (bitmap[i - 1] != 0) {
            return (i - 1) * 8 +
                   (uint64_t)(31 - __builtin_clz((uint32_t)bitmap[i - 1]));
        }
    }
    for (uint64_t i = words; i > 0; i--) {
        /* memcpy load: 'bitmap' may point into a flex payload with no
         * alignment guarantee; a direct word deref is UB. */
        uint64_t word;
        memcpy(&word, bitmap + ((i - 1) * 8), sizeof(word));
        if (word != 0) {
            return (i - 1) * 64 + (uint64_t)(63 - __builtin_clzll(word));
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

    if (!multimapIteratorNextInto(&iter, keyValue, 2)) {
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

    if (!multimapIteratorNextInto(&iter, keyValue, 2)) {
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

    _Alignas(uint64_t) uint8_t bitmapA[BITMAP_SIZE_IN_BYTES];
    _Alignas(uint64_t) uint8_t bitmapB[BITMAP_SIZE_IN_BYTES];

    while (multimapIteratorNextInto(&iter, keyValue, 2)) {
        const databox *key = &keyValue[0];

        /* Check if b has this chunk */
        databox valueB;
        if (!multimapLookupInto(b->map, key, &valueB, 1)) {
            continue; /* No overlap in this chunk */
        }

        /* Expand both chunks to bitmaps and check for any AND != 0 */
        expandChunkToBitmap(&keyValue[1], bitmapA);
        expandChunkToBitmap(&valueB, bitmapB);

        /* Check if any bit is set in both (64 bits at a time) */
        const uint64_t *a64 = (const uint64_t *)bitmapA;
        const uint64_t *b64 = (const uint64_t *)bitmapB;
        for (uint64_t i = 0; i < BITMAP_SIZE_IN_BYTES / 8; i++) {
            if ((a64[i] & b64[i]) != 0) {
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

    _Alignas(uint64_t) uint8_t bitmapA[BITMAP_SIZE_IN_BYTES];
    _Alignas(uint64_t) uint8_t bitmapB[BITMAP_SIZE_IN_BYTES];

    while (multimapIteratorNextInto(&iter, keyValue, 2)) {
        const databox *key = &keyValue[0];

        /* Check if b has this chunk */
        databox valueB;
        if (!multimapLookupInto(b->map, key, &valueB, 1)) {
            /* a has bits in this chunk but b doesn't - not a subset */
            return false;
        }

        /* Expand both chunks and verify (a AND b) == a */
        expandChunkToBitmap(&keyValue[1], bitmapA);
        expandChunkToBitmap(&valueB, bitmapB);

        const uint64_t *a64 = (const uint64_t *)bitmapA;
        const uint64_t *b64 = (const uint64_t *)bitmapB;
        for (uint64_t i = 0; i < BITMAP_SIZE_IN_BYTES / 8; i++) {
            if ((a64[i] & b64[i]) != a64[i]) {
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

    _Alignas(uint64_t) uint8_t bitmapA[BITMAP_SIZE_IN_BYTES];
    _Alignas(uint64_t) uint8_t bitmapB[BITMAP_SIZE_IN_BYTES];

    while (multimapIteratorNextInto(&iter, keyValue, 2)) {
        const databox *key = &keyValue[0];

        /* Check if b has this chunk */
        databox valueB;
        if (!multimapLookupInto(b->map, key, &valueB, 1)) {
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

    /* Count full bytes word-at-a-time (8 bytes per popcount), then any
     * trailing full bytes that don't fill a word. */
    uint64_t fullBytes = endPos / 8;
    uint64_t fullWords = fullBytes / 8;
    for (uint64_t i = 0; i < fullWords; i++) {
        /* memcpy load: 'bitmap' may point into a flex payload with no
         * alignment guarantee; a direct word deref is UB. */
        uint64_t word;
        memcpy(&word, bitmap + (i * 8), sizeof(word));
        count += (uint64_t)__builtin_popcountll(word);
    }
    for (uint64_t i = fullWords * 8; i < fullBytes; i++) {
        count += (uint64_t)__builtin_popcount(bitmap[i]);
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

    while (multimapIteratorNextInto(&iter, keyValue, 2)) {
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

    while (multimapIteratorNextInto(&iter, keyValue, 2)) {
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
/* Set, clear or flip bits [lo, hi) of one chunk's bitmap: edge bits one at
 * a time, whole bytes in between */
DK_STATIC void bitmapRangeApply_(uint8_t *bitmap, uint32_t lo, uint32_t hi,
                                 multiroarRangeOp_ op) {
    while (lo < hi && (lo & 7)) {
        const uint8_t bit = (uint8_t)(1u << (lo & 7));
        bitmap[lo / 8] = op == MULTIROAR_RANGE_SET_     ? bitmap[lo / 8] | bit
                         : op == MULTIROAR_RANGE_CLEAR_ ? bitmap[lo / 8] & ~bit
                                                        : bitmap[lo / 8] ^ bit;
        lo++;
    }
    if (hi - lo >= 8) {
        const uint32_t bytes = (hi - lo) / 8;
        uint8_t *b = bitmap + lo / 8;
        if (op == MULTIROAR_RANGE_SET_) {
            memset(b, 0xFF, bytes);
        } else if (op == MULTIROAR_RANGE_CLEAR_) {
            memset(b, 0, bytes);
        } else {
            for (uint32_t i = 0; i < bytes; i++) {
                b[i] ^= 0xFF;
            }
        }
        lo += bytes * 8;
    }
    while (lo < hi) {
        const uint8_t bit = (uint8_t)(1u << (lo & 7));
        bitmap[lo / 8] = op == MULTIROAR_RANGE_SET_     ? bitmap[lo / 8] | bit
                         : op == MULTIROAR_RANGE_CLEAR_ ? bitmap[lo / 8] & ~bit
                                                        : bitmap[lo / 8] ^ bit;
        lo++;
    }
}

/* Apply `op` to bits [start, start + extent) (clamped below UINT64_MAX, as
 * every range op is), one chunk at a time: a whole chunk becomes ALL_1 or
 * disappears directly; a partial one is expanded to a bitmap, edited and
 * stored back in its smallest form.  Clearing visits only chunks that
 * exist, so clearing a huge sparse range costs nothing for empty space. */
DK_STATIC void multiroarRangeApply_(multiroar *r, uint64_t start,
                                    uint64_t extent, multiroarRangeOp_ op) {
    if (!r || extent == 0) {
        return;
    }
    assert(r->meta[0] == 0 && "range ops are bit-mode operations");

    uint64_t end = start + extent;
    if (end < start) {
        end = UINT64_MAX;
    }
    const uint64_t first = CHUNK(start);
    const uint64_t last = CHUNK(end - 1);

    for (uint64_t chunk = first;;) {
        databox key = databoxNewUnsigned(chunk);
        multimapEntry me;
        bool exists = multimapGetUnderlyingEntry(r->map, &key, &me);
        if (op == MULTIROAR_RANGE_CLEAR_ && !exists) {
            /* Skip to the next chunk that exists */
            multimapIterator iter;
            databox kv[2];
            if (!multimapIteratorInitAt(r->map, &iter, true, &key) ||
                !multimapIteratorNextInto(&iter, kv, 2) ||
                kv[0].data.u > last) {
                return;
            }
            chunk = kv[0].data.u;
            key = databoxNewUnsigned(chunk);
            exists = multimapGetUnderlyingEntry(r->map, &key, &me);
            assert(exists);
        }

        const uint64_t chunkStart = chunk * BITMAP_SIZE_IN_BITS;
        const uint32_t lo = chunk == first ? (uint32_t)(start - chunkStart) : 0;
        const uint32_t hi =
            chunk == last ? (uint32_t)(end - chunkStart) : BITMAP_SIZE_IN_BITS;
        const bool whole = lo == 0 && hi == BITMAP_SIZE_IN_BITS;

        if (whole && op == MULTIROAR_RANGE_CLEAR_) {
            multimapDelete(&r->map, &key);
        } else if (whole && op == MULTIROAR_RANGE_SET_) {
            uint8_t allOnes[1] = {CHUNK_TYPE_ALL_1};
            const databox box = {
                .data.bytes.start = allOnes, .len = 1, .type = DATABOX_BYTES};
            const databox *inserting[] = {&key, &box};
            multimapInsert(&r->map, inserting);
        } else {
            uint8_t bitmap[BITMAP_SIZE_IN_BYTES];
            if (exists) {
                databox value = {{0}};
                flexGetNextByType(*me.map, &me.fe, &value);
                expandChunkToBitmap(&value, bitmap);
            } else {
                memset(bitmap, 0, sizeof(bitmap));
            }
            bitmapRangeApply_(bitmap, lo, hi, op);
            compressBitmapToChunk(r, &key, bitmap, NULL, exists);
        }

        if (chunk == last) {
            return;
        }
        chunk++;
    }
}

void multiroarBitClearRange(multiroar *r, uint64_t start, uint64_t extent) {
    multiroarRangeApply_(r, start, extent, MULTIROAR_RANGE_CLEAR_);
}

/* Flip range [start, start+extent) */
void multiroarBitFlipRange(multiroar *r, uint64_t start, uint64_t extent) {
    multiroarRangeApply_(r, start, extent, MULTIROAR_RANGE_FLIP_);
}

/* Set difference: A AND NOT B (bits in A but not in B) - returns new */
multiroar *multiroarNewAndNot(const multiroar *a, const multiroar *b) {
    if (!a) {
        return NULL;
    }

    if (!b || !b->map) {
        /* @own: caller; free=multiroarFree */
        return multiroarDuplicate(a);
    }

    /* @own: caller; free=multiroarFree */
    multiroar *result = multiroarBitNew();
    if (!result) {
        return NULL;
    }

    /* Iterate through chunks in a */
    multimapIterator iter;
    multimapIteratorInit(a->map, &iter, true);

    databox keyValue[2];

    while (multimapIteratorNextInto(&iter, keyValue, 2)) {
        const databox *key = &keyValue[0];
        const databox *aValue = &keyValue[1];
        uint64_t chunkId = key->data.u;

        /* Check if b has this chunk */
        databox bKey = databoxNewUnsigned(chunkId);
        databox bValue;
        databox *bValues[] = {&bValue};

        /* Expand a's chunk to bitmap */
        _Alignas(uint64_t) uint8_t bitmapA[BITMAP_SIZE_IN_BYTES];
        expandChunkToBitmap(aValue, bitmapA);

        if (multimapLookup(b->map, &bKey, bValues)) {
            /* Both have this chunk - perform AND NOT (64 bits at a time) */
            _Alignas(uint64_t) uint8_t bitmapB[BITMAP_SIZE_IN_BYTES];
            expandChunkToBitmap(&bValue, bitmapB);

            /* A &= ~B */
            uint64_t *a64 = (uint64_t *)bitmapA;
            const uint64_t *b64 = (const uint64_t *)bitmapB;
            for (uint64_t i = 0; i < BITMAP_SIZE_IN_BYTES / 8; i++) {
                a64[i] &= ~b64[i];
            }
        }
        /* else: b doesn't have this chunk, so keep all bits from a */

        /* Compress the resulting bitmap into the (fresh) result map in one
         * shot — replaces an O(BITMAP_SIZE_IN_BITS) per-bit multiroarBitSet
         * loop (each a full container insert) with a single chunk write, like
         * multiroarOr/multiroarAnd. result is fresh, so chunkExists = false. */
        multimapEntry me;
        compressBitmapToChunk(result, key, bitmapA, &me, false);
    }

    return result;
}

/* Set difference: r = r AND NOT b (in-place) */
void multiroarAndNot(multiroar *r, multiroar *b) {
    if (!r || !r->map || !b || !b->map) {
        return;
    }

    /* Create result using newAndNot, then swap; the scope-exit free
     * releases the swapped-out old map inside `result`. */
    cleaner(multiroarFree) multiroar *result = multiroarNewAndNot(r, b);
    if (!result) {
        return;
    }

    /* Swap the maps */
    multimap *temp = r->map;
    r->map = result->map;
    result->map = temp;
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
    iter->unsetCursor = 0;
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
                uint64_t bitmapLen = GET_CHUNK_BITMAP_LEN(&iter->currentChunk);
                if (bitmapLen > BITMAP_SIZE_IN_BYTES) {
                    bitmapLen = BITMAP_SIZE_IN_BYTES;
                }
                /* bitmapLen is word-aligned (it is passed to StrPopCntExact,
                 * which requires step alignment), so whole 64-bit words cover
                 * it exactly. */
                const uint64_t nWords = bitmapLen / 8;

                /* Scan for the next set bit 64 bits at a time, resuming from
                 * positionInChunk: mask off bits before the cursor in the
                 * starting word, then CTZ; zero words are skipped wholesale.
                 * Bit numbering is byte-array order, LSB-first within byte,
                 * which equals a little-endian word's CTZ (arm64/x86-64). */
                uint64_t pos = iter->positionInChunk;
                uint64_t wordIdx = pos >> 6;
                while (wordIdx < nWords) {
                    uint64_t w;
                    memcpy(&w, bitmap + wordIdx * 8, sizeof(w));
                    w &= ~(uint64_t)0 << (pos & 63); /* ignore bits < cursor */
                    if (w != 0) {
                        const uint64_t found =
                            wordIdx * 64 + (uint64_t)__builtin_ctzll(w);
                        *position = chunkBase + found;
                        iter->positionInChunk = found + 1;
                        iter->indexInChunk++;
                        return true;
                    }
                    wordIdx++;
                    pos = wordIdx * 64; /* later words: no masking */
                }
                break;
            }

            case CHUNK_TYPE_OVER_FULL_DIRECT_NOT_SET_POSITION_NUMBERS: {
                /* Inverted: emit all positions except the unset ones. The unset
                 * list is sorted ascending (see _bitmapToPositions), so we walk
                 * a monotonic cursor instead of rescanning it per position —
                 * O(BITMAP_SIZE_IN_BITS + unsetCount) per chunk, not O(N*M). */
                const uint8_t *data =
                    GET_CHUNK_PACKED_START(&iter->currentChunk);
                const uint16_t unsetCount =
                    PACKED_COUNT_FROM_VALUE(&iter->currentChunk);

                while (iter->positionInChunk < BITMAP_SIZE_IN_BITS) {
                    /* Advance the cursor to the first unset position >= the
                     * current bit (cursor only moves forward across calls). */
                    while (iter->unsetCursor < unsetCount &&
                           varintPacked13Get(data, iter->unsetCursor) <
                               iter->positionInChunk) {
                        iter->unsetCursor++;
                    }
                    const bool isUnset =
                        iter->unsetCursor < unsetCount &&
                        varintPacked13Get(data, iter->unsetCursor) ==
                            iter->positionInChunk;

                    const uint64_t currentPos = iter->positionInChunk;
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

        if (!multimapIteratorNextInto(&iter->mapIter, keyValue, 2)) {
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
        iter->unsetCursor = 0; /* reset OVER_FULL unset-list cursor per chunk */

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

    /* Batch consecutive same-chunk runs: one map lookup plus one
     * rebuilt chunk per run instead of one full map operation per bit.
     * Sorted or clustered inputs (bulk loads, multiroarFromArray)
     * collapse to one map op per chunk; runs too short to amortize the
     * expand/compress cost keep the exact per-bit path, so random
     * order never regresses. */
    enum { SET_MANY_RUN_MIN = 4 };

    uint64_t i = 0;
    while (i < count) {
        const uint64_t chunk = CHUNK(positions[i]);
        uint64_t runEnd = i + 1;
        while (runEnd < count && CHUNK(positions[runEnd]) == chunk) {
            runEnd++;
        }

        if (runEnd - i < SET_MANY_RUN_MIN) {
            for (; i < runEnd; i++) {
                multiroarBitSet(r, positions[i]);
            }
            continue;
        }

        const databox key = databoxNewUnsigned(chunk);
        databox value;
        const bool exists = multimapLookupInto(r->map, &key, &value, 1);

        if (exists && GET_CHUNK_TYPE(&value) == CHUNK_TYPE_ALL_1) {
            /* Every bit already set - nothing to do for this run */
            i = runEnd;
            continue;
        }

        _Alignas(uint64_t) uint8_t bitmap[BITMAP_SIZE_IN_BYTES];
        if (exists) {
            expandChunkToBitmap(&value, bitmap);
        } else {
            memset(bitmap, 0, sizeof(bitmap));
        }

        for (; i < runEnd; i++) {
            const uint64_t byteOffset =
                BYTE_OFFSET(GLOBAL_POSITION_TO_CHUNK_POSITION(positions[i]));
            bitmap[byteOffset] |= (1 << BIT_OFFSET(positions[i]));
        }

        compressBitmapToChunk(r, &key, bitmap, NULL, exists);
    }
}

/* Check multiple positions efficiently */
void multiroarBitGetMany(const multiroar *r, const uint64_t *positions,
                         uint64_t count, bool *results) {
    if (!r || !positions || !results || count == 0) {
        return;
    }

    /* One map lookup per RUN of same-chunk positions: sorted or
     * clustered inputs collapse to a single lookup per chunk, random
     * order degrades to exactly the old per-position cost. The cached
     * value box references bytes inside the map, which is safe here
     * because nothing mutates the map between lookups. */
    uint64_t cachedChunk = 0;
    bool cachedFound = false;
    bool haveCache = false;
    databox value = {{0}};

    for (uint64_t i = 0; i < count; i++) {
        const uint64_t chunk = CHUNK(positions[i]);
        if (!haveCache || chunk != cachedChunk) {
            const databox key = databoxNewUnsigned(chunk);
            cachedFound = multimapLookupInto(r->map, &key, &value, 1);
            cachedChunk = chunk;
            haveCache = true;
        }

        results[i] = cachedFound && chunkTestsBit(&value, positions[i]);
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
    /* No positions is an empty roar (what multiroarToArray gives for
     * one); only a count without positions is refused */
    if (!positions && count > 0) {
        return NULL;
    }

    /* @own: caller; free=multiroarFree */
    multiroar *r = multiroarBitNew();

    /* Set all positions */
    for (uint64_t i = 0; i < count; i++) {
        multiroarBitSet(r, positions[i]);
    }

    return r;
}

/* ====================================================================
 * Similarity and Distance Metrics
 * ==================================================================== */

/* |A ∩ B| without building it: each chunk both hold, expanded to a
 * bitmap, ANDed and counted.  Every metric below derives from this and
 * the two bit counts (|A ∪ B| = |A| + |B| - |A ∩ B|, and the symmetric
 * difference is |A| + |B| - 2|A ∩ B|). */
static uint64_t multiroarIntersectionCount_(const multiroar *a,
                                            const multiroar *b) {
    if (!a->map || !b->map) {
        return 0;
    }

    /* Walk the roar with fewer chunks, look each up in the other */
    if (multimapCount(a->map) > multimapCount(b->map)) {
        const multiroar *t = a;
        a = b;
        b = t;
    }

    _Alignas(uint64_t) uint8_t bitmapA[BITMAP_SIZE_IN_BYTES];
    _Alignas(uint64_t) uint8_t bitmapB[BITMAP_SIZE_IN_BYTES];
    uint64_t count = 0;
    multimapIterator iter;
    multimapIteratorInit(a->map, &iter, true);
    databox keyValue[2];
    while (multimapIteratorNextInto(&iter, keyValue, 2)) {
        databox valueB;
        if (!multimapLookupInto(b->map, &keyValue[0], &valueB, 1)) {
            continue;
        }

        expandChunkToBitmap(&keyValue[1], bitmapA);
        expandChunkToBitmap(&valueB, bitmapB);
        uint64_t *a64 = (uint64_t *)bitmapA;
        const uint64_t *b64 = (const uint64_t *)bitmapB;
        for (uint64_t i = 0; i < BITMAP_SIZE_IN_BYTES / 8; i++) {
            a64[i] &= b64[i];
        }
        count += StrPopCntExact(bitmapA, BITMAP_SIZE_IN_BYTES);
    }

    return count;
}

/* Jaccard similarity: |A ∩ B| / |A ∪ B| ∈ [0, 1] */
double multiroarJaccard(const multiroar *a, const multiroar *b) {
    if (!a || !b) {
        return 0.0;
    }

    const uint64_t countA = multiroarBitCount(a);
    const uint64_t countB = multiroarBitCount(b);
    if (countA == 0 && countB == 0) {
        return 1.0; /* Both empty - perfect similarity */
    }

    const uint64_t intersection = multiroarIntersectionCount_(a, b);
    return (double)intersection / (double)(countA + countB - intersection);
}

/* Hamming distance: number of differing bits */
uint64_t multiroarHammingDistance(const multiroar *a, const multiroar *b) {
    if (!a || !b) {
        return 0;
    }

    return multiroarBitCount(a) + multiroarBitCount(b) -
           2 * multiroarIntersectionCount_(a, b);
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

    const uint64_t minCount = countA < countB ? countA : countB;
    return (double)multiroarIntersectionCount_(a, b) / (double)minCount;
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

    return (2.0 * (double)multiroarIntersectionCount_(a, b)) /
           (double)(countA + countB);
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
 * Wire format (multiroarSerialize): the dkFormat prefix (format id
 * MULTIROAR_FORMAT_ID, version MULTIROAR_FORMAT_VERSION, features), then
 * the stream (multiroarSerializeRaw, which a stamping container such as
 * persist stores on its own):
 * - Mode: 1 byte, the value bit width (1..64), 0 for bit mode
 * - Value mode: cols varint, rows varint, value count varint, then per
 *   value in ascending position: the gap from the previous position (the
 *   first: the position itself) varint, the value varint
 * - Bit mode: the chunk stream
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

const dkFormatSpec multiroarFormat = {
    .name = "multiroar",
    .id = MULTIROAR_FORMAT_ID,
    .version = MULTIROAR_FORMAT_VERSION,
    .minReadVersion = MULTIROAR_FORMAT_VERSION,
    .knownFeatures = 0,
};

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

/* Write `value` as a varint at *p when it fits before `end` (advancing
 * *p); false when it does not, writing nothing */
static bool writeVarintBounded(uint8_t **p, const uint8_t *end,
                               uint64_t value) {
    uint8_t tmp[10];
    const uint64_t n = writeVarint(tmp, value);
    if ((uint64_t)(end - *p) < n) {
        return false;
    }
    memcpy(*p, tmp, n);
    *p += n;
    return true;
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

/* Size of a value-mode body: dimensions, count, then each value after the
 * gap from the previous position */
static uint64_t multiroarValuesSize(const multiroar *r) {
    uint8_t tmp[10];
    uint64_t rows;
    uint64_t cols;
    multiroarValueDimensions(r, &rows, &cols);
    uint64_t size = writeVarint(tmp, cols) + writeVarint(tmp, rows) +
                    writeVarint(tmp, multimapCount(r->map));

    multimapIterator iter;
    multimapIteratorInit(r->map, &iter, true);
    databox entry[2];
    uint64_t prev = 0;
    while (multimapIteratorNextInto(&iter, entry, 2)) {
        size += writeVarint(tmp, entry[0].data.u - prev) +
                writeVarint(tmp, entry[1].data.u);
        prev = entry[0].data.u;
    }
    return size;
}

static uint64_t multiroarChunksSize(const multiroar *r);

/* Size of the stream, without serializing: the mode byte, then the body */
uint64_t multiroarSerializedSizeRaw(const multiroar *r) {
    if (!r || !r->map) {
        return 2; /* bit mode, varint(0) chunks */
    }
    return 1 +
           (r->meta[0] != 0 ? multiroarValuesSize(r) : multiroarChunksSize(r));
}

/* Size of a bit-mode body: the chunk stream */
static uint64_t multiroarChunksSize(const multiroar *r) {
    uint64_t size = 0;

    /* Count chunks */
    uint64_t chunkCount = multimapCount(r->map);

    /* Varint for chunk count (worst case: 10 bytes) */
    uint8_t temp[10];
    size += writeVarint(temp, chunkCount);

    /* Iterate chunks */
    multimapIterator iter;
    multimapIteratorInit(r->map, &iter, true);

    databox keyValue[2];

    while (multimapIteratorNextInto(&iter, keyValue, 2)) {
        const databox *key = &keyValue[0];
        const databox *value = &keyValue[1];

        uint64_t chunkId = key->data.u;

        /* Chunk ID varint */
        size += writeVarint(temp, chunkId);

        /* Chunk type: 1 byte */
        size += 1;

        /* Chunk data */
        uint8_t chunkKind = GET_CHUNK_TYPE(value);

        switch (chunkKind) {
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

/* Write a value-mode body at *p; false when it does not fit */
static bool multiroarWriteValues(const multiroar *r, uint8_t **p,
                                 const uint8_t *end) {
    uint64_t rows;
    uint64_t cols;
    multiroarValueDimensions(r, &rows, &cols);
    if (!writeVarintBounded(p, end, cols) ||
        !writeVarintBounded(p, end, rows) ||
        !writeVarintBounded(p, end, multimapCount(r->map))) {
        return false;
    }

    multimapIterator iter;
    multimapIteratorInit(r->map, &iter, true);
    databox entry[2];
    uint64_t prev = 0;
    while (multimapIteratorNextInto(&iter, entry, 2)) {
        if (!writeVarintBounded(p, end, entry[0].data.u - prev) ||
            !writeVarintBounded(p, end, entry[1].data.u)) {
            return false;
        }
        prev = entry[0].data.u;
    }
    return true;
}

static bool multiroarWriteChunks(const multiroar *r, uint8_t **p,
                                 const uint8_t *end);

/* Serialize the stream to buffer; 0 when it does not fit */
uint64_t multiroarSerializeRaw(const multiroar *r, void *buf,
                               uint64_t bufSize) {
    if (!buf || bufSize == 0) {
        return 0;
    }

    uint8_t *p = (uint8_t *)buf;
    const uint8_t *const end = p + bufSize;
    const uint8_t width = r ? r->meta[0] : 0;
    *p++ = width;

    const bool written = width != 0 ? multiroarWriteValues(r, &p, end)
                                    : multiroarWriteChunks(r, &p, end);
    return written ? (uint64_t)(p - (uint8_t *)buf) : 0;
}

/* Write a bit-mode body (the chunk stream) at *p; false when it does not
 * fit */
static bool multiroarWriteChunks(const multiroar *r, uint8_t **pOut,
                                 const uint8_t *end) {
    uint8_t *p = *pOut;

    /* Chunk count (0 for an empty roar) */
    const uint64_t chunkCount = (r && r->map) ? multimapCount(r->map) : 0;
    if (!writeVarintBounded(&p, end, chunkCount)) {
        return false;
    }
    if (chunkCount == 0) {
        *pOut = p;
        return true;
    }

    /* Serialize each chunk */
    multimapIterator iter;
    multimapIteratorInit(r->map, &iter, true);

    databox keyValue[2];

    while (multimapIteratorNextInto(&iter, keyValue, 2)) {
        const databox *key = &keyValue[0];
        const databox *value = &keyValue[1];

        uint64_t chunkId = key->data.u;

        /* Chunk ID, then chunk type */
        if (!writeVarintBounded(&p, end, chunkId) || p >= end) {
            return false;
        }
        uint8_t chunkKind = GET_CHUNK_TYPE(value);
        *p++ = chunkKind;

        /* Chunk data */
        switch (chunkKind) {
        case CHUNK_TYPE_ALL_0:
        case CHUNK_TYPE_ALL_1:
            /* No data */
            break;

        case CHUNK_TYPE_UNDER_FULL_DIRECT_POSITION_NUMBERS:
        case CHUNK_TYPE_OVER_FULL_DIRECT_NOT_SET_POSITION_NUMBERS: {
            /* varintPacked data */
            const uint8_t *packedStart = GET_CHUNK_PACKED_START(value);
            uint64_t count = PACKED_COUNT_FROM_VALUE(value);

            /* Write count, then positions */
            if (!writeVarintBounded(&p, end, count)) {
                return false;
            }

            for (uint64_t i = 0; i < count; i++) {
                if (!writeVarintBounded(&p, end,
                                        varintPacked13Get(packedStart, i))) {
                    return false;
                }
            }
            break;
        }

        case CHUNK_TYPE_FULL_BITMAP: {
            /* Write bitmap data */
            if (end - p < 1024) {
                return false;
            }

            const uint8_t *bitmap = GET_CHUNK_BITMAP_START(value);
            memcpy(p, bitmap, 1024);
            p += 1024;
            break;
        }

        default:
            /* Unknown chunk type */
            return false;
        }
    }

    *pOut = p;
    return true;
}

/* The stamp multiroarSerialize writes ahead of the stream */
static dkFormatStamp multiroarStamp(void) {
    return dkFormatStampFor(&multiroarFormat, 0);
}

uint64_t multiroarSerializedSize(const multiroar *r) {
    const uint64_t raw = multiroarSerializedSizeRaw(r);
    if (raw == 0) {
        return 0;
    }
    const dkFormatStamp stamp = multiroarStamp();
    return dkFormatPrefixSize(&stamp) + raw;
}

uint64_t multiroarSerialize(const multiroar *r, void *buf, uint64_t bufSize) {
    if (!buf) {
        return 0;
    }
    const dkFormatStamp stamp = multiroarStamp();
    const size_t prefix = dkFormatPrefixWrite(&stamp, buf, (size_t)bufSize);
    if (prefix == 0) {
        return 0;
    }
    const uint64_t raw =
        multiroarSerializeRaw(r, (uint8_t *)buf + prefix, bufSize - prefix);
    return raw == 0 ? 0 : prefix + raw;
}

multiroar *multiroarDeserialize(const void *buf, uint64_t bufSize) {
    dkFormatStamp stamp;
    size_t prefix = 0;
    if (!buf || dkFormatPrefixRead(&multiroarFormat, buf, (size_t)bufSize,
                                   &stamp, &prefix) != DK_FORMAT_OK) {
        return NULL;
    }
    return multiroarDeserializeRaw((const uint8_t *)buf + prefix,
                                   bufSize - prefix);
}

/* Read a value-mode body of width `width` filling exactly [p, end): the
 * values must fit the width, positions ascend and stay inside a declared
 * rows x cols */
static multiroar *multiroarReadValues(uint8_t width, const uint8_t *p,
                                      const uint8_t *end) {
    if (width > 64) {
        return NULL;
    }

    uint64_t cols;
    uint64_t rows;
    uint64_t count;
    uint64_t n = readVarint(p, (uint64_t)(end - p), &cols);
    if (n == 0) {
        return NULL;
    }
    p += n;
    n = readVarint(p, (uint64_t)(end - p), &rows);
    if (n == 0) {
        return NULL;
    }
    p += n;
    n = readVarint(p, (uint64_t)(end - p), &count);
    if (n == 0) {
        return NULL;
    }
    p += n;

    cleaner(multiroarFree) multiroar *r = multiroarValueNew(width, rows, cols);
    uint64_t position = 0;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t gap;
        uint64_t value;
        n = readVarint(p, (uint64_t)(end - p), &gap);
        if (n == 0 || (i > 0 && gap == 0) ||
            __builtin_add_overflow(position, gap, &position)) {
            return NULL;
        }
        p += n;
        n = readVarint(p, (uint64_t)(end - p), &value);
        if (n == 0 || (width < 64 && value >> width) ||
            !multiroarValueInBounds(r, position)) {
            return NULL;
        }
        p += n;

        databox box = databoxNewUnsigned(value);
        multiroarValueSet(r, position, &box);
    }
    if (p != end) {
        return NULL;
    }

    multiroar *out = r;
    r = NULL;
    return out;
}

static multiroar *multiroarReadChunks(const uint8_t *p, const uint8_t *end);

/* Deserialize the stream: the mode byte, then a value or chunk body */
multiroar *multiroarDeserializeRaw(const void *buf, uint64_t bufSize) {
    if (!buf || bufSize == 0) {
        return NULL;
    }

    const uint8_t *p = (const uint8_t *)buf;
    const uint8_t *end = p + bufSize;
    const uint8_t width = *p++;
    return width != 0 ? multiroarReadValues(width, p, end)
                      : multiroarReadChunks(p, end);
}

/* Read a bit-mode body (the chunk stream) filling exactly [p, end) */
static multiroar *multiroarReadChunks(const uint8_t *p, const uint8_t *end) {
    /* Read chunk count */
    uint64_t chunkCount;
    uint64_t bytesRead = readVarint(p, (uint64_t)(end - p), &chunkCount);
    if (bytesRead == 0) {
        return NULL;
    }
    p += bytesRead;

    /* Create new roar; scope-managed so every parse-error return below
     * releases it without per-site cleanup calls. */
    cleaner(multiroarFree) multiroar *r = multiroarBitNew();
    if (!r) {
        return NULL;
    }

    /* Deserialize each chunk.  multimapInsert COPIES the chunk bytes
     * into the map, so each temporary chunk buffer is scope-managed
     * per case block: freed after the insert on success and on every
     * early-error return alike.  The bytes may be damaged, so every
     * chunk is checked against what multiroarSerialize writes: chunk ids
     * ascending, listings of 1 to MAX_ENTRIES_PER_DIRECT_LISTING
     * positions, each below BITMAP_SIZE_IN_BITS and ascending. */
    uint64_t prevChunkId = 0;
    for (uint64_t i = 0; i < chunkCount; i++) {
        /* Read chunk ID */
        uint64_t chunkId;
        bytesRead = readVarint(p, (uint64_t)(end - p), &chunkId);
        if (bytesRead == 0 || (i > 0 && chunkId <= prevChunkId)) {
            return NULL;
        }
        prevChunkId = chunkId;
        p += bytesRead;

        /* Read chunk type */
        if (p >= end) {
            return NULL;
        }
        uint8_t chunkKind = *p++;

        /* Reconstruct chunk directly as bytes blob */
        switch (chunkKind) {
        case CHUNK_TYPE_ALL_0:
            /* Skip - not stored in map */
            continue;

        case CHUNK_TYPE_ALL_1: {
            /* Create single-byte ALL_1 chunk */
            cleanerName(zfree, Bytes) uint8_t *chunk = zcalloc(1, 1);
            if (!chunk) {
                return NULL;
            }
            chunk[0] = CHUNK_TYPE_ALL_1;

            databox key = databoxNewUnsigned(chunkId);

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
            if (bytesRead == 0 || count == 0 ||
                count > MAX_ENTRIES_PER_DIRECT_LISTING) {
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

            cleanerName(zfree, Bytes) uint8_t *chunk = zcalloc(chunkSize, 1);
            if (!chunk) {
                return NULL;
            }
            chunk[0] = chunkKind;
            memcpy(chunk + 1, tempCount, countBytes); /* TaggedVarint count */

            /* Read positions and pack them */
            uint64_t prevPos = 0;
            for (uint64_t j = 0; j < count; j++) {
                uint64_t pos;
                bytesRead = readVarint(p, (uint64_t)(end - p), &pos);
                if (bytesRead == 0 || pos >= BITMAP_SIZE_IN_BITS ||
                    (j > 0 && pos <= prevPos)) {
                    return NULL;
                }
                prevPos = pos;
                p += bytesRead;

                varintPacked13Set(chunk + 1 + countBytes, j, pos);
            }

            databox key = databoxNewUnsigned(chunkId);

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
                return NULL;
            }

            /* Create chunk: 1 type + 1024 bitmap */
            cleanerName(zfree, Bytes) uint8_t *chunk = zcalloc(1025, 1);
            if (!chunk) {
                return NULL;
            }
            chunk[0] = CHUNK_TYPE_FULL_BITMAP;
            memcpy(chunk + 1, p, 1024);
            p += 1024;

            databox key = databoxNewUnsigned(chunkId);

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
            return NULL;
        }
    }

    /* Every byte belongs to the stream */
    if (p != end) {
        return NULL;
    }

    /* Success: hand ownership to the caller (disarm the cleaner) */
    multiroar *out = r;
    r = NULL;
    return out;
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

/* memcpy word round-trips: callers may hand these bitmap pointers with
 * no alignment guarantee (type-tagged buffers offset by one byte, flex
 * payloads), and the scalar fallback is exactly what strict-alignment
 * platforms run — a direct word deref is UB and traps there.  memcpy
 * compiles to the same single load/store on every supported target. */
DK_STATIC void bitmap_and_simd(uint8_t *dst, const uint8_t *src,
                               uint64_t bytes) {
    uint64_t words = bytes / 8;
    for (uint64_t i = 0; i < words; i++) {
        uint64_t d;
        uint64_t s;
        memcpy(&d, dst + (i * 8), sizeof(d));
        memcpy(&s, src + (i * 8), sizeof(s));
        d &= s;
        memcpy(dst + (i * 8), &d, sizeof(d));
    }
    for (uint64_t i = words * 8; i < bytes; i++) {
        dst[i] &= src[i];
    }
}

DK_STATIC void bitmap_or_simd(uint8_t *dst, const uint8_t *src,
                              uint64_t bytes) {
    uint64_t words = bytes / 8;
    for (uint64_t i = 0; i < words; i++) {
        uint64_t d;
        uint64_t s;
        memcpy(&d, dst + (i * 8), sizeof(d));
        memcpy(&s, src + (i * 8), sizeof(s));
        d |= s;
        memcpy(dst + (i * 8), &d, sizeof(d));
    }
    for (uint64_t i = words * 8; i < bytes; i++) {
        dst[i] |= src[i];
    }
}

DK_STATIC void bitmap_xor_simd(uint8_t *dst, const uint8_t *src,
                               uint64_t bytes) {
    uint64_t words = bytes / 8;
    for (uint64_t i = 0; i < words; i++) {
        uint64_t d;
        uint64_t s;
        memcpy(&d, dst + (i * 8), sizeof(d));
        memcpy(&s, src + (i * 8), sizeof(s));
        d ^= s;
        memcpy(dst + (i * 8), &d, sizeof(d));
    }
    for (uint64_t i = words * 8; i < bytes; i++) {
        dst[i] ^= src[i];
    }
}

DK_STATIC void bitmap_not_simd(uint8_t *data, uint64_t bytes) {
    uint64_t words = bytes / 8;
    for (uint64_t i = 0; i < words; i++) {
        uint64_t d;
        memcpy(&d, data + (i * 8), sizeof(d));
        d = ~d;
        memcpy(data + (i * 8), &d, sizeof(d));
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

    _Alignas(uint64_t) uint8_t bitmapR[BITMAP_SIZE_IN_BYTES];
    _Alignas(uint64_t) uint8_t bitmapB[BITMAP_SIZE_IN_BYTES];

    /* First, collect all keys from b */
    uint64_t numChunks = multimapCount(b->map);
    if (numChunks == 0) {
        return;
    }

    /* Collect just keys from b via iteration */
    cleanerName(zfree, UInt64) uint64_t *chunkKeys =
        zmalloc(numChunks * sizeof(uint64_t));
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
        databox key = databoxNewUnsigned(chunkKeys[i]);

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
}

/* AND: r = r AND b */
void multiroarAnd(multiroar *r, multiroar *b) {
    if (!r || !b) {
        return;
    }

    /* For AND, chunks only in r (not in b) become zeros.
     * We need to collect keys to delete after iteration. */
    _Alignas(uint64_t) uint8_t bitmapR[BITMAP_SIZE_IN_BYTES];
    _Alignas(uint64_t) uint8_t bitmapB[BITMAP_SIZE_IN_BYTES];

    /* Collect chunk keys from r that need processing */
    uint64_t numChunks = multimapCount(r->map);
    if (numChunks == 0) {
        return;
    }

    cleanerName(zfree, UInt64) uint64_t *chunkKeys =
        zmalloc(numChunks * sizeof(uint64_t));
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
        databox key = databoxNewUnsigned(chunkKeys[i]);
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
}

/* XOR: r = r XOR b */
typedef struct multiroarChunkBitmap {
    uint64_t key;
    _Alignas(uint64_t) uint8_t bitmap[BITMAP_SIZE_IN_BYTES];
} ChunkBitmap;
defcleanerName(ChunkBitmap *, zfree, ChunkBitmap);

void multiroarXor(multiroar *r, multiroar *b) {
    if (!r || !b) {
        return;
    }

    _Alignas(uint64_t) uint8_t bitmapR[BITMAP_SIZE_IN_BYTES];
    _Alignas(uint64_t) uint8_t bitmapB[BITMAP_SIZE_IN_BYTES];

    /* First, collect all keys from b */
    uint64_t numChunks = multimapCount(b->map);
    if (numChunks == 0) {
        return;
    }

    /* Store both keys and bitmaps from b */
    cleanerName(zfree, ChunkBitmap) ChunkBitmap *chunks =
        zmalloc(numChunks * sizeof(ChunkBitmap));
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
        databox key = databoxNewUnsigned(chunks[i].key);
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
}

/* NOT: r = NOT r (within existing chunks only) */
void multiroarNot(multiroar *r) {
    if (!r) {
        return;
    }

    _Alignas(uint64_t) uint8_t bitmap[BITMAP_SIZE_IN_BYTES];

    /* Collect chunk keys first to avoid iterator invalidation */
    uint64_t numChunks = multimapCount(r->map);
    if (numChunks == 0) {
        return;
    }

    cleanerName(zfree, UInt64) uint64_t *chunkKeys =
        zmalloc(numChunks * sizeof(uint64_t));
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
        databox key = databoxNewUnsigned(chunkKeys[i]);

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
}

/* Create new roars from operations */
multiroar *multiroarNewAnd(const multiroar *a, const multiroar *b) {
    /* @own: caller; free=multiroarFree */
    multiroar *result = multiroarDuplicate(a);
    multiroarAnd(result, (multiroar *)b);
    return result;
}

multiroar *multiroarNewOr(const multiroar *a, const multiroar *b) {
    /* @own: caller; free=multiroarFree */
    multiroar *result = multiroarDuplicate(a);
    multiroarOr(result, (multiroar *)b);
    return result;
}

multiroar *multiroarNewXor(const multiroar *a, const multiroar *b) {
    /* @own: caller; free=multiroarFree */
    multiroar *result = multiroarDuplicate(a);
    multiroarXor(result, (multiroar *)b);
    return result;
}

multiroar *multiroarNewNot(const multiroar *r) {
    /* @own: caller; free=multiroarFree */
    multiroar *result = multiroarDuplicate(r);
    multiroarNot(result);
    return result;
}

/* ====================================================================
 * N-way Set Operations (for N >= 2 inputs)
 * ==================================================================== */

/* Helper: Collect all unique chunk IDs across N multiroars */
static int cmpChunkKeyAsc(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *)a;
    const uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

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

    /* Collect every chunk key (with duplicates across the N inputs) into a
     * buffer sized for the worst case, then sort + collapse adjacent
     * duplicates. This is O(K log K) vs the previous O(K^2) dedup-by-rescan.
     * The resulting ascending order is fine for the N-way callers (per-chunk
     * set ops are order-independent) and matches the multimap's own order. */
    /* @own: caller; free=zfree */
    uint64_t *allKeys = zmalloc(totalChunks * sizeof(uint64_t));
    uint64_t keyCount = 0;

    for (uint64_t i = 0; i < n; i++) {
        if (!roars[i] || !roars[i]->map) {
            continue;
        }

        multimapIterator iter;
        multimapIteratorInit(roars[i]->map, &iter, true);

        databox keyValue[2];

        while (multimapIteratorNextInto(&iter, keyValue, 2)) {
            allKeys[keyCount++] = keyValue[0].data.u;
        }
    }

    if (keyCount > 1) {
        qsort(allKeys, keyCount, sizeof(uint64_t), cmpChunkKeyAsc);
        uint64_t w = 1;
        for (uint64_t r = 1; r < keyCount; r++) {
            if (allKeys[r] != allKeys[w - 1]) {
                allKeys[w++] = allKeys[r];
            }
        }
        keyCount = w;
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
    cleanerName(zfree, UInt64) uint64_t *allKeys =
        collectAllChunkKeys(n, roars, &allKeyCount);

    if (!allKeys) {
        return;
    }

    cleanerName(zfree, Bytes) uint8_t *bitmaps =
        zmalloc(n * BITMAP_SIZE_IN_BYTES);
    _Alignas(uint64_t) uint8_t resultBitmap[BITMAP_SIZE_IN_BYTES];

    /* Process each unique chunk */
    for (uint64_t i = 0; i < allKeyCount; i++) {
        databox key = databoxNewUnsigned(allKeys[i]);

        /* Check if ALL roars have this chunk */
        bool allHaveChunk = true;
        for (uint64_t j = 0; j < n; j++) {
            if (!roars[j] || !roars[j]->map) {
                allHaveChunk = false;
                break;
            }

            databox value;
            if (!multimapLookupInto(roars[j]->map, &key, &value, 1)) {
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
            multimapLookupInto(roars[j]->map, &key, &value, 1);
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
}

/* N-way OR: Modifies first roar in array */
void multiroarOrN(uint64_t n, multiroar **roars) {
    if (n < 2 || !roars || !roars[0]) {
        return;
    }

    multiroar *result = roars[0];

    /* Collect all chunk IDs across all inputs */
    uint64_t allKeyCount;
    cleanerName(zfree, UInt64) uint64_t *allKeys =
        collectAllChunkKeys(n, roars, &allKeyCount);

    if (!allKeys) {
        return;
    }

    cleanerName(zfree, Bytes) uint8_t *bitmaps =
        zmalloc(n * BITMAP_SIZE_IN_BYTES);
    _Alignas(uint64_t) uint8_t resultBitmap[BITMAP_SIZE_IN_BYTES];

    /* Process each unique chunk */
    for (uint64_t i = 0; i < allKeyCount; i++) {
        databox key = databoxNewUnsigned(allKeys[i]);

        /* Expand all chunks to bitmaps (use zeros for missing chunks) */
        bool anyHasChunk = false;
        for (uint64_t j = 0; j < n; j++) {
            databox value;

            if (roars[j] && roars[j]->map &&
                multimapLookupInto(roars[j]->map, &key, &value, 1)) {
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
}

/* N-way XOR: Modifies first roar in array */
void multiroarXorN(uint64_t n, multiroar **roars) {
    if (n < 2 || !roars || !roars[0]) {
        return;
    }

    multiroar *result = roars[0];

    /* Collect all chunk IDs across all inputs */
    uint64_t allKeyCount;
    cleanerName(zfree, UInt64) uint64_t *allKeys =
        collectAllChunkKeys(n, roars, &allKeyCount);

    if (!allKeys) {
        return;
    }

    cleanerName(zfree, Bytes) uint8_t *bitmaps =
        zmalloc(n * BITMAP_SIZE_IN_BYTES);
    _Alignas(uint64_t) uint8_t resultBitmap[BITMAP_SIZE_IN_BYTES];

    /* Process each unique chunk */
    for (uint64_t i = 0; i < allKeyCount; i++) {
        databox key = databoxNewUnsigned(allKeys[i]);

        /* Expand all chunks to bitmaps (use zeros for missing chunks) */
        for (uint64_t j = 0; j < n; j++) {
            databox value;

            if (roars[j] && roars[j]->map &&
                multimapLookupInto(roars[j]->map, &key, &value, 1)) {
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
}

/* N-way operations returning new multiroar */
multiroar *multiroarNewAndN(uint64_t n, multiroar **roars) {
    if (n == 0 || !roars || !roars[0]) {
        /* @own: caller; free=multiroarFree */
        return multiroarBitNew();
    }

    /* @own: caller; free=multiroarFree */
    multiroar *result = multiroarDuplicate(roars[0]);

    if (n > 1) {
        /* Create temp array starting from result */
        cleanerName(zfree, RoarPtrs) multiroar **tempRoars =
            zmalloc(n * sizeof(multiroar *));
        tempRoars[0] = result;
        for (uint64_t i = 1; i < n; i++) {
            tempRoars[i] = roars[i];
        }

        multiroarAndN(n, tempRoars);
    }

    return result;
}

multiroar *multiroarNewOrN(uint64_t n, multiroar **roars) {
    if (n == 0 || !roars || !roars[0]) {
        /* @own: caller; free=multiroarFree */
        return multiroarBitNew();
    }

    /* @own: caller; free=multiroarFree */
    multiroar *result = multiroarDuplicate(roars[0]);

    if (n > 1) {
        /* Create temp array starting from result */
        cleanerName(zfree, RoarPtrs) multiroar **tempRoars =
            zmalloc(n * sizeof(multiroar *));
        tempRoars[0] = result;
        for (uint64_t i = 1; i < n; i++) {
            tempRoars[i] = roars[i];
        }

        multiroarOrN(n, tempRoars);
    }

    return result;
}

multiroar *multiroarNewXorN(uint64_t n, multiroar **roars) {
    if (n == 0 || !roars || !roars[0]) {
        /* @own: caller; free=multiroarFree */
        return multiroarBitNew();
    }

    /* @own: caller; free=multiroarFree */
    multiroar *result = multiroarDuplicate(roars[0]);

    if (n > 1) {
        /* Create temp array starting from result */
        cleanerName(zfree, RoarPtrs) multiroar **tempRoars =
            zmalloc(n * sizeof(multiroar *));
        tempRoars[0] = result;
        for (uint64_t i = 1; i < n; i++) {
            tempRoars[i] = roars[i];
        }

        multiroarXorN(n, tempRoars);
    }

    return result;
}

/* ====================================================================
 * Testing
 * ==================================================================== */

#ifdef DATAKIT_TEST

#include "ctest.h"
#include "timeUtil.h"

static void multiroarRepr(multiroar *r, uint64_t highest) {
    for (uint64_t i = 0; i < divCeil(highest, BITMAP_SIZE_IN_BITS); i++) {
        databox value;
        const databox chunkKey = databoxNewUnsigned(i);
        if (multimapLookupInto(r->map, &chunkKey, &value, 1)) {
            printf("[%" PRIu64 "] = %d (%" PRIu64 " byte%s)\n", i,
                   GET_CHUNK_TYPE(&value), (uint64_t)value.len,
                   value.len == 1 ? "" : "s");
        }
    }
}

static void multiroarTestCompare(multiroar *r, uint64_t highest) {
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

/* The chunk-form matrix test: MR_FORM_CHUNKS chunks per roar, each filled
 * in one storage form, checked against a plain bit array. */
#define MR_FORM_CHUNKS 7
#define MR_FORM_BITS ((uint64_t)MR_FORM_CHUNKS * BITMAP_SIZE_IN_BITS)
#define MR_FORM_WORDS (MR_FORM_BITS / 64)

static bool mrOracleGet(const uint64_t *o, uint64_t p) {
    return (o[p / 64] >> (p % 64)) & 1;
}

/* Give chunk 'c' exactly 'count' set bits, in the roar and the oracle.
 * Above half full the chunk starts all set (BitSetRange) and loses bits
 * one Remove at a time, so dense forms are reached by removal; below,
 * bits are set one at a time. */
static void mrFillChunk(multiroar *r, uint64_t *o, uint64_t c, uint32_t count,
                        uint64_t *rng) {
    const uint64_t base = c * BITMAP_SIZE_IN_BITS;
    if (count > BITMAP_SIZE_IN_BITS / 2) {
        multiroarBitSetRange(r, base, BITMAP_SIZE_IN_BITS);
        for (uint64_t w = 0; w < BITMAP_SIZE_IN_BITS / 64; w++) {
            o[base / 64 + w] = UINT64_MAX;
        }
        for (uint32_t clear = BITMAP_SIZE_IN_BITS - count; clear > 0;) {
            const uint64_t p = base + splitmix64(rng) % BITMAP_SIZE_IN_BITS;
            if (mrOracleGet(o, p)) {
                multiroarRemove(r, p);
                o[p / 64] &= ~(UINT64_C(1) << (p % 64));
                clear--;
            }
        }
        return;
    }

    for (uint32_t left = count; left > 0;) {
        const uint64_t p = base + splitmix64(rng) % BITMAP_SIZE_IN_BITS;
        if (!mrOracleGet(o, p)) {
            multiroarBitSet(r, p);
            o[p / 64] |= UINT64_C(1) << (p % 64);
            left--;
        }
    }
}

static uint64_t mrOracleCount(const uint64_t *o, uint64_t start, uint64_t end) {
    uint64_t n = 0;
    for (uint64_t p = start; p < end; p++) {
        n += mrOracleGet(o, p);
    }
    return n;
}

/* Every read of 'r' against the oracle 'o'; returns the mismatches */
static int mrCheckForms(const multiroar *r, const uint64_t *o,
                        const char *what) {
    int bad = 0;
#define MR_BAD(...)                                                            \
    do {                                                                       \
        printf("%s: ", what);                                                  \
        printf(__VA_ARGS__);                                                   \
        printf("\n");                                                          \
        bad++;                                                                 \
    } while (0)

    uint64_t count = 0;
    uint64_t lo = UINT64_MAX;
    uint64_t hi = 0;
    multiroarIterator it;
    multiroarIteratorInit(r, &it);
    for (uint64_t p = 0; p < MR_FORM_BITS; p++) {
        const bool set = mrOracleGet(o, p);
        if (multiroarBitGet(r, p) != set) {
            MR_BAD("bit %" PRIu64 " reads %d", p, !set);
            break;
        }
        /* Rank counts the set bits below p; Select is 1-indexed */
        if ((p % 509 == 0 || p % BITMAP_SIZE_IN_BITS == 0) &&
            multiroarRank(r, p) != count) {
            MR_BAD("rank(%" PRIu64 ") = %" PRIu64 ", want %" PRIu64, p,
                   multiroarRank(r, p), count);
        }
        if (!set) {
            continue;
        }
        count++;
        lo = lo == UINT64_MAX ? p : lo;
        hi = p;
        uint64_t pos = 0;
        if (!multiroarIteratorNext(&it, &pos) || pos != p) {
            MR_BAD("iterator gave %" PRIu64 ", want %" PRIu64, pos, p);
            break;
        }
        if (count % 97 == 1 && (!multiroarSelect(r, count, &pos) || pos != p)) {
            MR_BAD("select(%" PRIu64 ") = %" PRIu64 ", want %" PRIu64, count,
                   pos, p);
        }
    }
    uint64_t pos = 0;
    if (multiroarIteratorNext(&it, &pos)) {
        MR_BAD("iterator ran past the last bit to %" PRIu64, pos);
    }
    multiroarIteratorReset(&it);
    if (count && (!multiroarIteratorNext(&it, &pos) || pos != lo)) {
        MR_BAD("iterator after reset gave %" PRIu64, pos);
    }
    if (multiroarBitCount(r) != count || multiroarIsEmpty(r) != (count == 0) ||
        multiroarRank(r, MR_FORM_BITS) != count ||
        multiroarSelect(r, count + 1, &pos)) {
        MR_BAD("count %" PRIu64 ", want %" PRIu64, multiroarBitCount(r), count);
    }
    uint64_t got = 0;
    if (count ? (!multiroarMin(r, &got) || got != lo ||
                 !multiroarMax(r, &got) || got != hi)
              : (multiroarMin(r, &got) || multiroarMax(r, &got))) {
        MR_BAD("min/max");
    }

    /* Ranges inside a chunk, across chunk edges and over everything */
    static const uint64_t ranges[][2] = {
        {0, MR_FORM_BITS}, {100, 200},     {8000, 8400},   {8192, 16384},
        {12000, 30000},    {24575, 24577}, {40000, 40000}, {50000, 57344},
    };
    for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
        const uint64_t want = mrOracleCount(o, ranges[i][0], ranges[i][1]);
        const uint64_t have =
            multiroarRangeCount(r, ranges[i][0], ranges[i][1]);
        if (have != want) {
            MR_BAD("range [%" PRIu64 ", %" PRIu64 ") has %" PRIu64
                   ", want %" PRIu64,
                   ranges[i][0], ranges[i][1], have, want);
        }
    }

    /* Copies: array, duplicate, both serialized forms */
    cleanerName(zfree, UInt64) uint64_t *arr =
        zmalloc((count + 1) * sizeof(uint64_t));
    if (multiroarToArray(r, arr, count + 1) != count ||
        (count && (arr[0] != lo || arr[count - 1] != hi))) {
        MR_BAD("to array");
    }
    cleaner(multiroarFree) multiroar *fromArr = multiroarFromArray(arr, count);
    cleaner(multiroarFree) multiroar *dup = multiroarDuplicate(r);
    const uint64_t size = multiroarSerializedSize(r);
    const uint64_t rawSize = multiroarSerializedSizeRaw(r);
    cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(size + rawSize);
    cleaner(multiroarFree) multiroar *back = NULL;
    cleaner(multiroarFree) multiroar *backRaw = NULL;
    if (multiroarSerialize(r, buf, size) == size) {
        back = multiroarDeserialize(buf, size);
    }
    if (multiroarSerializeRaw(r, buf + size, rawSize) == rawSize) {
        backRaw = multiroarDeserializeRaw(buf + size, rawSize);
    }
    if (!multiroarEquals(fromArr, r) || !multiroarEquals(dup, r) || !back ||
        !multiroarEquals(back, r) || !backRaw || !multiroarEquals(backRaw, r)) {
        MR_BAD("a copy differs");
    }
#undef MR_BAD
    return bad;
}

int multiroarTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;

    TEST("bitmap first/last set bit against a per-bit scan, any length") {
        /* Lengths with and without a partial last word: no bit, every
         * single bit, and random fills; both scans agree with walking
         * the bits one at a time (UINT64_MAX for none) */
        static const uint64_t lens[] = {1, 3, 7, 8, 9, 15, 16, 17, 23, 64};
        uint8_t bitmap[64];
        uint64_t rng = 0xb17;
        for (size_t l = 0; l < sizeof(lens) / sizeof(*lens); l++) {
            const uint64_t bytes = lens[l];
            for (uint64_t trial = 0; trial < bytes * 8 + 1 + 40; trial++) {
                memset(bitmap, 0, sizeof(bitmap));
                if (trial > 0 && trial <= bytes * 8) {
                    bitmap[(trial - 1) / 8] =
                        (uint8_t)(1u << ((trial - 1) % 8));
                } else if (trial > bytes * 8) {
                    for (uint64_t i = 0; i < bytes; i++) {
                        const uint64_t r = splitmix64(&rng);
                        /* mostly zero bytes, so the scans must skip */
                        bitmap[i] = (r & 7) == 0 ? (uint8_t)(r >> 8) : 0;
                    }
                }
                uint64_t first = UINT64_MAX;
                uint64_t last = UINT64_MAX;
                for (uint64_t bit = 0; bit < bytes * 8; bit++) {
                    if (bitmap[bit / 8] >> (bit % 8) & 1) {
                        first = first == UINT64_MAX ? bit : first;
                        last = bit;
                    }
                }
                if (findFirstSetBitInBitmap(bitmap, bytes) != first ||
                    findLastSetBitInBitmap(bitmap, bytes) != last) {
                    ERR("%" PRIu64 " bytes, trial %" PRIu64 ": first %" PRIu64
                        " last %" PRIu64 ", want %" PRIu64 " %" PRIu64,
                        bytes, trial, findFirstSetBitInBitmap(bitmap, bytes),
                        findLastSetBitInBitmap(bitmap, bytes), first, last);
                }
            }
        }
    }

    TEST("every chunk form against a bit-array oracle") {
        /* Forms per chunk number: a sparse listing, a bitmap, a listing
         * of the bits not set, all set, empty.  The three roars put a
         * different form at each chunk number, so the set operations
         * meet every pairing of forms */
        enum { SPARSE = 60, BITMAP = 3000, DENSE = 8150, ALL = 8192 };
        static const uint32_t forms[3][MR_FORM_CHUNKS] = {
            {SPARSE, BITMAP, DENSE, ALL, 0, 1, 5000},
            {ALL, DENSE, SPARSE, BITMAP, 10, 0, DENSE},
            {4000, 200, ALL, DENSE, 0, ALL, 30},
        };
        uint64_t rng = 0xf0f0;
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();
        cleaner(multiroarFree) multiroar *c3 = multiroarBitNew();
        multiroar *roars[3] = {a, b, c3};
        cleanerName(zfree, UInt64) uint64_t *oracle =
            zcalloc(3 * MR_FORM_WORDS, sizeof(uint64_t));
        uint64_t *o[3] = {oracle, oracle + MR_FORM_WORDS,
                          oracle + 2 * MR_FORM_WORDS};
        for (int i = 0; i < 3; i++) {
            for (uint64_t c = 0; c < MR_FORM_CHUNKS; c++) {
                mrFillChunk(roars[i], o[i], c, forms[i][c], &rng);
            }
            err += mrCheckForms(roars[i], o[i], "filled");
        }

        cleanerName(zfree, UInt64) uint64_t *want =
            zmalloc(MR_FORM_WORDS * sizeof(uint64_t));
        enum { AND, OR, XOR, ANDNOT };
        static const char *names[] = {"and", "or", "xor", "andnot"};
        for (int op = AND; op <= ANDNOT; op++) {
            for (uint64_t w = 0; w < MR_FORM_WORDS; w++) {
                const uint64_t x = o[0][w];
                const uint64_t y = o[1][w];
                want[w] = op == AND   ? x & y
                          : op == OR  ? x | y
                          : op == XOR ? x ^ y
                                      : x & ~y;
            }
            cleaner(multiroarFree) multiroar *fresh =
                op == AND   ? multiroarNewAnd(a, b)
                : op == OR  ? multiroarNewOr(a, b)
                : op == XOR ? multiroarNewXor(a, b)
                            : multiroarNewAndNot(a, b);
            cleaner(multiroarFree) multiroar *inPlace = multiroarDuplicate(a);
            if (op == AND) {
                multiroarAnd(inPlace, b);
            } else if (op == OR) {
                multiroarOr(inPlace, b);
            } else if (op == XOR) {
                multiroarXor(inPlace, b);
            } else {
                multiroarAndNot(inPlace, b);
            }
            err += mrCheckForms(fresh, want, names[op]);
            err += mrCheckForms(inPlace, want, names[op]);
        }

        /* Three-way, fresh and in place */
        for (int op = AND; op <= XOR; op++) {
            for (uint64_t w = 0; w < MR_FORM_WORDS; w++) {
                const uint64_t x = o[0][w];
                const uint64_t y = o[1][w];
                const uint64_t z = o[2][w];
                want[w] = op == AND  ? x & y & z
                          : op == OR ? x | y | z
                                     : x ^ y ^ z;
            }
            multiroar *three[3] = {a, b, c3};
            cleaner(multiroarFree) multiroar *fresh =
                op == AND  ? multiroarNewAndN(3, three)
                : op == OR ? multiroarNewOrN(3, three)
                           : multiroarNewXorN(3, three);
            cleaner(multiroarFree) multiroar *first = multiroarDuplicate(a);
            three[0] = first;
            if (op == AND) {
                multiroarAndN(3, three);
            } else if (op == OR) {
                multiroarOrN(3, three);
            } else {
                multiroarXorN(3, three);
            }
            err += mrCheckForms(fresh, want, names[op]);
            err += mrCheckForms(first, want, names[op]);
        }

        /* NOT flips the chunks that hold any bit and leaves the rest */
        for (uint64_t c = 0; c < MR_FORM_CHUNKS; c++) {
            const uint64_t words = BITMAP_SIZE_IN_BITS / 64;
            bool any = false;
            for (uint64_t w = 0; w < words; w++) {
                any = any || o[0][c * words + w] != 0;
            }
            for (uint64_t w = 0; w < words; w++) {
                want[c * words + w] = any ? ~o[0][c * words + w] : 0;
            }
        }
        cleaner(multiroarFree) multiroar *notA = multiroarNewNot(a);
        err += mrCheckForms(notA, want, "not");

        /* Relations and similarity, from the oracle's counts */
        uint64_t na = 0;
        uint64_t nb = 0;
        uint64_t nAnd = 0;
        uint64_t nOr = 0;
        uint64_t nXor = 0;
        for (uint64_t w = 0; w < MR_FORM_WORDS; w++) {
            na += (uint64_t)__builtin_popcountll(o[0][w]);
            nb += (uint64_t)__builtin_popcountll(o[1][w]);
            nAnd += (uint64_t)__builtin_popcountll(o[0][w] & o[1][w]);
            nOr += (uint64_t)__builtin_popcountll(o[0][w] | o[1][w]);
            nXor += (uint64_t)__builtin_popcountll(o[0][w] ^ o[1][w]);
        }
        cleaner(multiroarFree) multiroar *ab = multiroarNewAnd(a, b);
        if (multiroarIntersects(a, b) != (nAnd > 0) ||
            !multiroarIsSubset(ab, a) || !multiroarIsSubset(ab, b) ||
            multiroarIsSubset(a, b) || multiroarEquals(a, b) ||
            !multiroarEquals(a, a) ||
            multiroarJaccard(a, b) != (double)nAnd / (double)nOr ||
            multiroarHammingDistance(a, b) != nXor ||
            multiroarOverlap(a, b) !=
                (double)nAnd / (double)(na < nb ? na : nb) ||
            multiroarDice(a, b) != (2.0 * nAnd) / (double)(na + nb)) {
            ERRR("relations or similarity disagree with the oracle");
        }

        /* Uneven chunk counts both ways round, and an empty roar */
        cleaner(multiroarFree) multiroar *one = multiroarBitNew();
        cleaner(multiroarFree) multiroar *none = multiroarBitNew();
        const uint64_t p = 2 * BITMAP_SIZE_IN_BITS + 5;
        multiroarBitSet(one, p);
        const uint64_t inA = mrOracleGet(o[0], p);
        const double jOne = (double)inA / (double)(na + 1 - inA);
        if (multiroarJaccard(a, one) != jOne ||
            multiroarJaccard(one, a) != jOne ||
            multiroarHammingDistance(a, one) != na + 1 - 2 * inA ||
            multiroarHammingDistance(one, a) != na + 1 - 2 * inA ||
            multiroarOverlap(one, a) != (double)inA ||
            multiroarHammingDistance(a, none) != na ||
            multiroarJaccard(a, none) != 0.0 ||
            multiroarJaccard(none, none) != 1.0 ||
            multiroarOverlap(a, none) != 0.0 || multiroarDice(a, none) != 0.0 ||
            multiroarDice(none, none) != 1.0) {
            ERRR("similarity with uneven or empty roars");
        }

        /* Operations that cancel every chunk form (a ^ a, a & ~a, and
         * a & not(a), whose chunks are disjoint bitmaps) leave nothing:
         * every read sees empty, and no emptied chunk lingers (equal to,
         * and serialized like, a new roar) */
        cleanerName(zfree, UInt64) uint64_t *zeros =
            zcalloc(MR_FORM_WORDS, sizeof(uint64_t));
        cleaner(multiroarFree) multiroar *xorSelf = multiroarNewXor(a, a);
        cleaner(multiroarFree) multiroar *andNotSelf = multiroarNewAndNot(a, a);
        cleaner(multiroarFree) multiroar *andNotA = multiroarNewAnd(a, notA);
        cleaner(multiroarFree) multiroar *xorInPlace = multiroarDuplicate(a);
        cleaner(multiroarFree) multiroar *andNotInPlace = multiroarDuplicate(a);
        multiroarXor(xorInPlace, a);
        multiroarAndNot(andNotInPlace, a);
        multiroar *const cancelled[5] = {xorSelf, andNotSelf, andNotA,
                                         xorInPlace, andNotInPlace};
        for (int i = 0; i < 5; i++) {
            err += mrCheckForms(cancelled[i], zeros, "cancelled");
            if (!multiroarEquals(cancelled[i], none) ||
                multiroarSerializedSize(cancelled[i]) !=
                    multiroarSerializedSize(none)) {
                ERR("cancelled roar %d keeps emptied chunks", i);
            }
        }
        cleaner(multiroarFree) multiroar *fromNothing =
            multiroarFromArray(NULL, 0);
        cleaner(multiroarFree) multiroar *countOnly =
            multiroarFromArray(NULL, 3);
        if (!fromNothing || !multiroarIsEmpty(fromNothing) || countOnly) {
            ERRR("an empty position list is not an empty roar, or a count "
                 "without positions was accepted");
        }
    }

    TEST("create") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
        (void)r;
    }

    TEST("set and get") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
        bool previouslySet = multiroarBitSet(r, 1700);

        if (previouslySet) {
            ERRR("Detected previously set on new assignment!");
        }

        bool currentlySet = multiroarBitGet(r, 1700);

        if (!currentlySet) {
            ERRR("Didn't find set bit after setting the bit!");
        }
    }

    TEST("value mode: constructor bounds + set/get/previous/remove") {
        /* Invalid widths refuse construction (rejection returns NULL, so
         * nothing is ever owned here). */
        /* @own: caller; free=multiroarFree */
        assert(multiroarValueNew(0, 4, 4) == NULL);
        /* @own: caller; free=multiroarFree */
        assert(multiroarValueNew(65, 4, 4) == NULL);

        cleaner(multiroarFree) multiroar *v = multiroarValueNew(16, 100, 100);
        assert(v);

        /* Fresh position: Set reports no prior value */
        databox val = databoxNewUnsigned(4242);
        assert(!multiroarValueSet(v, 7, &val));

        /* Get round-trips exactly */
        databox got = {{0}};
        assert(multiroarValueGet(v, 7, &got));
        assert(got.type == DATABOX_UNSIGNED_64 && got.data.u == 4242);

        /* Missing position: Get false, box untouched */
        assert(!multiroarValueGet(v, 8, &got));

        /* Overwrite reports and returns the previous value */
        databox val2 = databoxNewUnsigned(65535); /* max for bitWidth 16 */
        databox prev = {{0}};
        assert(multiroarValueSetGetPrevious(v, 7, &val2, &prev));
        assert(prev.data.u == 4242);
        assert(multiroarValueGet(v, 7, &got) && got.data.u == 65535);

        /* Booleans store as 0/1; signed non-negatives accepted */
        databox tbox = databoxBool(true);
        assert(!multiroarValueSet(v, 9998, &tbox)); /* max position 9999 */
        assert(multiroarValueGet(v, 9998, &got) && got.data.u == 1);
        databox sbox = databoxNewSigned(12);
        assert(!multiroarValueSet(v, 0, &sbox));
        assert(multiroarValueGet(v, 0, &got) && got.data.u == 12);

        /* Remove returns the removed value; second remove reports miss */
        databox removed = {{0}};
        assert(multiroarValueRemoveGetRemoved(v, 7, &removed));
        assert(removed.data.u == 65535);
        assert(!multiroarValueGet(v, 7, NULL));
        assert(!multiroarValueRemoveGetRemoved(v, 7, &removed));

        /* Duplicate preserves value-mode meta (bitWidth + bounds): value
         * ops on the duplicate must keep working, with content copied. */
        cleaner(multiroarFree) multiroar *dupe = multiroarDuplicate(v);
        assert(dupe);
        assert(multiroarValueGet(dupe, 0, &got) && got.data.u == 12);
        databox val3 = databoxNewUnsigned(77);
        assert(!multiroarValueSet(dupe, 55, &val3));
        assert(multiroarValueGet(dupe, 55, &got) && got.data.u == 77);
        /* ...and the original is unaffected by writes to the copy */
        assert(!multiroarValueGet(v, 55, NULL));

        /* Unbounded (rows/cols 0) accepts any position */
        cleaner(multiroarFree) multiroar *u = multiroarValueNew(64, 0, 0);
        assert(u);
        databox big = databoxNewUnsigned(UINT64_MAX);
        assert(!multiroarValueSet(u, UINT64_MAX - 1, &big));
        assert(multiroarValueGet(u, UINT64_MAX - 1, &got) &&
               got.data.u == UINT64_MAX);
    }

    const int32_t lots = 72000;

#if 1
    TEST_DESC("set and get (%d of random positions; individual roars)", lots) {
        for (int32_t i = 0; i < lots; i++) {
            const uint64_t position = (uint64_t)rand() % SIZE_MAX;

            cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
        }
    }

    TEST_DESC("set and get (%d of (SMALL) random positions; common roar)",
              lots) {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST_DESC("set and get (%d of (MEDIUM) random positions; common roar)",
              lots) {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST_DESC("set and get (%d of (BIG) random positions; common roar)", lots) {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }
#endif

    TEST_DESC("set and get (%d of sequential positions; common roar)", lots) {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("chunk boundary correctness") {
        /* Test bits at and around chunk boundaries (8192 bits per chunk) */
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("sequential set up to bitmap conversion threshold") {
        /* Test setting many bits sequentially triggers bitmap conversion
         * and continues to work correctly */
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("conversion from UNDER_FULL to FULL_BITMAP") {
        /* Test that conversion to full bitmap preserves all bits */
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
        const uint64_t maxDirect = 629; /* MAX_ENTRIES_PER_DIRECT_LISTING */

        /* Set exactly maxDirect positions to trigger conversion to bitmap */
        cleanerName(zfree, UInt64) uint64_t *positions =
            zcalloc(maxDirect, sizeof(uint64_t));
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
    }

    TEST("bitmap mode with various positions") {
        /* Test that bitmap mode (> 629 positions) works correctly */
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    TEST("multi-chunk correctness") {
        /* Create roar with multiple chunks */
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("high-scale stress test (100K random bits)") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
        const uint64_t scale = 100000;
        const uint64_t sampleSize = 10000; /* Track a sample for verification */
        cleanerName(zfree, UInt64) uint64_t *sampleBits =
            zcalloc(sampleSize, sizeof(uint64_t));

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
    }

    TEST("set same bit twice returns correct previouslySet") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    TEST("PERF: sequential insert performance") {
        const uint64_t scale = 100000;
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

        int64_t startNs = timeUtilMonotonicNs();
        for (uint64_t i = 0; i < scale; i++) {
            multiroarBitSet(r, i);
        }
        int64_t elapsed = timeUtilMonotonicNs() - startNs;
        printf("Sequential insert: %.1f ns/op, %.0f ops/sec\n",
               (double)elapsed / scale, scale / (elapsed / 1e9));
    }

    TEST("PERF: random insert performance") {
        const uint64_t scale = 100000;
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

        int64_t startNs = timeUtilMonotonicNs();
        for (uint64_t i = 0; i < scale; i++) {
            multiroarBitSet(r, ((uint64_t)rand() * rand()) % (SIZE_MAX / 2));
        }
        int64_t elapsed = timeUtilMonotonicNs() - startNs;
        printf("Random insert: %.1f ns/op, %.0f ops/sec\n",
               (double)elapsed / scale, scale / (elapsed / 1e9));
    }

    TEST("PERF: lookup performance (dense)") {
        const uint64_t scale = 100000;
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    /* ================================================================
     * Edge Case and Boundary Tests
     * ================================================================ */

    TEST("UNDER_FULL growth to exactly MAX_ENTRIES") {
        /* Test growth from 1 to exactly 629 entries (conversion threshold) */
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("FULL_BITMAP exact boundary positions") {
        /* Test bits at exact byte and word boundaries within bitmap */
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    TEST("sparse positions across chunk boundary") {
        /* Test sparse positions that span multiple chunks */
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("set and immediate re-read in UNDER_FULL mode") {
        /* Test that set followed by immediate get works in UNDER_FULL */
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    TEST("set and immediate re-read in FULL_BITMAP mode") {
        /* Test that set followed by immediate get works in FULL_BITMAP */
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    TEST("previouslySet behavior across all chunk types") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    TEST("large position values near SIZE_MAX") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    TEST("interleaved set pattern") {
        /* Set every other bit, then fill in the gaps */
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("reverse order insertion") {
        /* Insert bits in reverse order */
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("random order insertion with verification") {
        /* Insert bits in pseudo-random order using LCG */
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
        const uint64_t testCount = 500;
        cleanerName(zfree, UInt64) uint64_t *positions =
            zcalloc(testCount, sizeof(uint64_t));

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
    }

    TEST("chunk type transitions (UNDER_FULL -> FULL_BITMAP)") {
        /* Carefully test the transition from UNDER_FULL to FULL_BITMAP */
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    TEST("OVER_FULL transition (FULL_BITMAP -> negative list)") {
        /* Test transition to OVER_FULL when most bits are set */
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("ALL_1 transition (fill entire chunk)") {
        /* Fill an entire chunk to trigger ALL_1 type */
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("mixed chunk types in single roar") {
        /* Create a roar with all different chunk types */
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("packed array sorted insertion stress") {
        /* Stress test the sorted insertion in UNDER_FULL mode */
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    TEST("empty chunk queries") {
        /* Test queries on chunks that don't exist (empty) */
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    TEST("PERF: sparse lookup performance") {
        const uint64_t numPositions = 1000;
        const uint64_t positionSpread = 1000000;
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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

    TEST("range set/clear/flip against a bit-array oracle") {
        /* Ranges inside one chunk, across chunk edges and over whole
         * chunks, mixed with single bits so chunks pass through every
         * form; every bit and the count checked as it goes */
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
        uint64_t oracle[FUZZ_ORACLE_WORDS] = {0};
        uint64_t rng = 0x5eed;
        for (int op = 0; op < 400; op++) {
            const uint64_t kind = splitmix64(&rng) % 5;
            const uint64_t start = splitmix64(&rng) % FUZZ_ORACLE_MAX_BIT;
            const uint64_t span = splitmix64(&rng) % 4 ? 700 : 20000;
            uint64_t extent = 1 + splitmix64(&rng) % span;
            if (start + extent > FUZZ_ORACLE_MAX_BIT) {
                extent = FUZZ_ORACLE_MAX_BIT - start;
            }
            if (kind == 0) {
                multiroarBitSetRange(r, start, extent);
            } else if (kind == 1) {
                multiroarBitClearRange(r, start, extent);
            } else if (kind == 2) {
                multiroarBitFlipRange(r, start, extent);
            } else {
                extent = 1;
                multiroarBitSet(r, start);
            }
            for (uint64_t p = start; p < start + extent; p++) {
                if (kind == 1) {
                    FUZZ_ORACLE_CLEAR(oracle, p);
                } else if (kind == 2 && FUZZ_ORACLE_GET(oracle, p)) {
                    FUZZ_ORACLE_CLEAR(oracle, p);
                } else {
                    FUZZ_ORACLE_SET(oracle, p);
                }
            }

            uint64_t oracleCount = 0;
            for (size_t w = 0; w < FUZZ_ORACLE_WORDS; w++) {
                oracleCount += (uint64_t)__builtin_popcountll(oracle[w]);
            }
            if (multiroarBitCount(r) != oracleCount) {
                ERR("op %d (kind %" PRIu64 " [%" PRIu64 ", +%" PRIu64
                    ")): count %" PRIu64 ", want %" PRIu64,
                    op, kind, start, extent, multiroarBitCount(r), oracleCount);
                for (uint64_t c = 0; c * 8192 < FUZZ_ORACLE_MAX_BIT; c++) {
                    uint64_t wrong = 0;
                    uint64_t firstWrong = 0;
                    for (uint64_t p = c * 8192;
                         p < (c + 1) * 8192 && p < FUZZ_ORACLE_MAX_BIT; p++) {
                        if (multiroarBitGet(r, p) !=
                            (bool)FUZZ_ORACLE_GET(oracle, p)) {
                            firstWrong = wrong++ ? firstWrong : p;
                        }
                    }
                    if (wrong) {
                        printf("  chunk %" PRIu64 ": %" PRIu64
                               " wrong bits, first %" PRIu64 "\n",
                               c, wrong, firstWrong);
                    }
                }
                break;
            }

            if (op % 50 == 49) {
                uint64_t count = 0;
                for (uint64_t p = 0; p < FUZZ_ORACLE_MAX_BIT; p++) {
                    const bool want = FUZZ_ORACLE_GET(oracle, p);
                    count += want;
                    if (multiroarBitGet(r, p) != want) {
                        ERR("op %d: bit %" PRIu64 " is %d, want %d", op, p,
                            !want, want);
                        break;
                    }
                }
                if (multiroarBitCount(r) != count) {
                    ERR("op %d: count %" PRIu64 ", want %" PRIu64, op,
                        multiroarBitCount(r), count);
                }
            }
        }

        /* Chunks stay in their smallest form: a serialize round trip
         * gives an equal roar */
        uint8_t buf[64 * 1024];
        const uint64_t len = multiroarSerialize(r, buf, sizeof(buf));
        cleaner(multiroarFree) multiroar *back = multiroarDeserialize(buf, len);
        if (!back || !multiroarEquals(r, back)) {
            ERRR("Range-built roar does not round trip");
        }

        /* A range ending past UINT64_MAX stops below it, as every range
         * op does; clearing everything visits only existing chunks */
        cleaner(multiroarFree) multiroar *top = multiroarBitNew();
        multiroarBitSetRange(top, UINT64_MAX - 10, 100);
        if (multiroarBitCount(top) != 10 ||
            !multiroarBitGet(top, UINT64_MAX - 1)) {
            ERR("Top range holds %" PRIu64 " bits, want 10",
                multiroarBitCount(top));
        }
        multiroarBitClearRange(top, 0, UINT64_MAX);
        if (!multiroarIsEmpty(top)) {
            ERRR("Clearing everything left bits");
        }
    }

    TEST("FUZZ: random bit operations with oracle") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("FUZZ: sequential then random access") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("FUZZ: sparse bits across many chunks") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

        srand(99999);
        const uint64_t numSparse = 1000;
        cleanerName(zfree, UInt64) uint64_t *positions =
            zmalloc(numSparse * sizeof(uint64_t));

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
    }

    TEST("FUZZ: chunk boundary stress") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("FUZZ: duplicate set operations") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

        srand(11111);
        const uint64_t numUnique = 500;
        cleanerName(zfree, UInt64) uint64_t *positions =
            zmalloc(numUnique * sizeof(uint64_t));

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
    }

    TEST("FUZZ: adversarial patterns - reverse order") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("FUZZ: adversarial patterns - alternating") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("FUZZ: large position values") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    TEST("FUZZ: stress 100K random operations") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    printf("\n=== NEW FUNCTIONALITY TESTS ===\n\n");

    TEST("multiroarRemove - basic remove from UNDER_FULL") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("multiroarRemove - remove from FULL_BITMAP") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("multiroarRemove - remove all bits from chunk") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("multiroarDuplicate - basic clone") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
        multiroarBitSet(r, 100);
        multiroarBitSet(r, 200);
        multiroarBitSet(r, 10000);

        cleaner(multiroarFree) multiroar *dup = multiroarDuplicate(r);
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
    }

    TEST("multiroarOr - basic OR operation") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

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
    }

    TEST("multiroarAnd - basic AND operation") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

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
    }

    TEST("multiroarXor - basic XOR operation") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

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
    }

    TEST("multiroarNewNot - a new complement, the input unchanged") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
        multiroarBitSet(r, 3);
        multiroarBitSet(r, 9000);
        cleaner(multiroarFree) multiroar *before = multiroarDuplicate(r);
        cleaner(multiroarFree) multiroar *notted = multiroarNewNot(r);
        cleaner(multiroarFree) multiroar *inPlace = multiroarDuplicate(r);
        multiroarNot(inPlace);
        if (!multiroarEquals(r, before) || !multiroarEquals(notted, inPlace) ||
            multiroarBitGet(notted, 3) || !multiroarBitGet(notted, 4)) {
            ERRR("NewNot is not Not applied to a copy");
        }
    }

    TEST("multiroarNot - basic NOT operation") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("multiroarNewOr - creates new roar") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

        multiroarBitSet(a, 100);
        multiroarBitSet(b, 200);

        cleaner(multiroarFree) multiroar *result = multiroarNewOr(a, b);
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
    }

    TEST("FUZZ: set operations with oracle verification") {
        printf("  Testing set operations with oracle...\n");

#define FUZZ_OP_SIZE 500
        uint64_t oracleA[divCeil(FUZZ_OP_SIZE, 64)] = {0};
        uint64_t oracleB[divCeil(FUZZ_OP_SIZE, 64)] = {0};

        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

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
        cleaner(multiroarFree) multiroar *orResult = multiroarNewOr(a, b);
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
        /* Test AND */
        cleaner(multiroarFree) multiroar *andResult = multiroarNewAnd(a, b);
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
        /* Test XOR */
        cleaner(multiroarFree) multiroar *xorResult = multiroarNewXor(a, b);
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

        /* Test NOT */
        cleaner(multiroarFree) multiroar *notResult = multiroarDuplicate(a);
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

        printf("  OR=%" PRIu64 " AND=%" PRIu64 " XOR=%" PRIu64 " NOT=%" PRIu64
               " mismatches\n",
               orMismatches, andMismatches, xorMismatches, notMismatches);
#undef FUZZ_OP_SIZE
    }

    TEST("FUZZ: Remove operations with oracle") {
        printf("  Testing remove operations...\n");

#define FUZZ_REMOVE_SIZE 2000
        uint64_t oracle[divCeil(FUZZ_REMOVE_SIZE, 64)] = {0};
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
#undef FUZZ_REMOVE_SIZE
    }

    TEST("FUZZ: Set operations across multiple chunks") {
        printf("  Testing multi-chunk set operations...\n");

        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

        /* Set bits across multiple chunks */
        for (uint64_t chunk = 0; chunk < 5; chunk++) {
            uint64_t base = chunk * 8192;
            for (uint64_t i = 0; i < 100; i++) {
                multiroarBitSet(a, base + i * 10);
                multiroarBitSet(b, base + i * 10 + 5);
            }
        }

        /* OR should combine both */
        cleaner(multiroarFree) multiroar *orResult = multiroarNewOr(a, b);
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
        cleaner(multiroarFree) multiroar *andResult = multiroarNewAnd(a, b);
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
    }

    TEST("set operations with dense bitmaps") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

        /* Fill to trigger FULL_BITMAP mode */
        for (uint64_t i = 0; i < 2000; i++) {
            multiroarBitSet(a, i);
        }
        for (uint64_t i = 1000; i < 3000; i++) {
            multiroarBitSet(b, i);
        }

        /* AND should give intersection: 1000-1999 */
        cleaner(multiroarFree) multiroar *andResult = multiroarNewAnd(a, b);
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
    }

    printf("\n=== All multiroar fuzz tests completed! ===\n\n");

    /* ========================================
     * BITCOUNT TESTS
     * ======================================== */
    printf("\n=== BITCOUNT TESTS ===\n\n");

    TEST("multiroarBitCount - empty roar") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
        uint64_t count = multiroarBitCount(r);
        if (count != 0) {
            ERR("Empty roar should have count=0, got %" PRIu64 "", count);
        }
    }

    TEST("multiroarBitCount - single bit") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
        multiroarBitSet(r, 100);
        uint64_t count = multiroarBitCount(r);
        if (count != 1) {
            ERR("Single bit should have count=1, got %" PRIu64 "", count);
        }
    }

    TEST("multiroarBitCount - sparse chunk") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
        for (uint64_t i = 0; i < 100; i++) {
            multiroarBitSet(r, i * 10);
        }
        uint64_t count = multiroarBitCount(r);
        if (count != 100) {
            ERR("Sparse chunk should have count=100, got %" PRIu64 "", count);
        }
    }

    TEST("multiroarBitCount - dense chunk") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
        /* Set 1000 bits to trigger FULL_BITMAP */
        for (uint64_t i = 0; i < 1000; i++) {
            multiroarBitSet(r, i);
        }
        uint64_t count = multiroarBitCount(r);
        if (count != 1000) {
            ERR("Dense chunk should have count=1000, got %" PRIu64 "", count);
        }
    }

    TEST("multiroarBitCount - full chunk (ALL_1)") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
        /* Set all 8192 bits in chunk */
        for (uint64_t i = 0; i < 8192; i++) {
            multiroarBitSet(r, i);
        }
        uint64_t count = multiroarBitCount(r);
        if (count != 8192) {
            ERR("Full chunk should have count=8192, got %" PRIu64 "", count);
        }
    }

    TEST("multiroarBitCount - multiple chunks") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("FUZZ: bitcount correctness oracle") {
        printf("  Testing bitcount with oracle...\n");
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    /* ========================================
     * N-WAY OPERATIONS TESTS
     * ======================================== */
    printf("\n=== N-WAY OPERATIONS TESTS ===\n\n");

    TEST("multiroarAndN - 3 roars basic") {
        cleaner(multiroarFree) multiroar *r1 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r2 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r3 = multiroarBitNew();

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
        cleaner(multiroarFree) multiroar *result = multiroarNewAndN(3, roars);

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
    }

    TEST("multiroarOrN - 3 roars basic") {
        cleaner(multiroarFree) multiroar *r1 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r2 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r3 = multiroarBitNew();

        multiroarBitSet(r1, 100);
        multiroarBitSet(r2, 200);
        multiroarBitSet(r3, 300);

        multiroar *roars[] = {r1, r2, r3};
        cleaner(multiroarFree) multiroar *result = multiroarNewOrN(3, roars);

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
    }

    TEST("multiroarXorN - 3 roars basic") {
        cleaner(multiroarFree) multiroar *r1 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r2 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r3 = multiroarBitNew();

        multiroarBitSet(r1, 100);
        multiroarBitSet(r2, 100); /* Cancels out */
        multiroarBitSet(r3, 100); /* Sets again */

        multiroarBitSet(r1, 200);
        multiroarBitSet(r2, 200); /* Cancels out */

        multiroarBitSet(r3, 300);

        multiroar *roars[] = {r1, r2, r3};
        cleaner(multiroarFree) multiroar *result = multiroarNewXorN(3, roars);

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
    }

    TEST("N-way: heavy duplicate chunk keys (collectAllChunkKeys dedup)") {
        /* Exercises the sort+dedup in collectAllChunkKeys: many roars all
         * touching the SAME chunks, so the raw key list is full of duplicates
         * that must collapse to the distinct chunk set. */
        enum { NR = 6, NCHUNK = 5 };
        multiroar *roars[NR];
        for (int r = 0; r < NR; r++) {
            /* @own: container; free=multiroarFree */
            roars[r] = multiroarBitNew();
            /* Every roar sets one bit in each of the NCHUNK chunks (same
             * chunk ids across all roars -> NR*NCHUNK raw keys -> NCHUNK
             * distinct), plus a private bit unique to this roar. */
            for (int c = 0; c < NCHUNK; c++) {
                multiroarBitSet(roars[r],
                                (uint64_t)c * BITMAP_SIZE_IN_BITS + 7);
            }
            multiroarBitSet(roars[r], (uint64_t)NCHUNK * BITMAP_SIZE_IN_BITS +
                                          (uint64_t)r);
        }

        /* OR: union = the NCHUNK shared bits + NR private bits. */
        cleaner(multiroarFree) multiroar *orRes = multiroarNewOrN(NR, roars);
        for (int c = 0; c < NCHUNK; c++) {
            if (!multiroarBitGet(orRes,
                                 (uint64_t)c * BITMAP_SIZE_IN_BITS + 7)) {
                ERR("OrN dedup: shared bit in chunk %d missing", c);
            }
        }
        for (int r = 0; r < NR; r++) {
            if (!multiroarBitGet(orRes, (uint64_t)NCHUNK * BITMAP_SIZE_IN_BITS +
                                            (uint64_t)r)) {
                ERR("OrN dedup: private bit of roar %d missing", r);
            }
        }
        if (multiroarBitCount(orRes) != (uint64_t)(NCHUNK + NR)) {
            ERR("OrN dedup: expected %d bits, got %" PRIu64, NCHUNK + NR,
                multiroarBitCount(orRes));
        }

        /* AND: only the NCHUNK bits common to ALL roars survive. */
        cleaner(multiroarFree) multiroar *andRes = multiroarNewAndN(NR, roars);
        if (multiroarBitCount(andRes) != (uint64_t)NCHUNK) {
            ERR("AndN dedup: expected %d common bits, got %" PRIu64, NCHUNK,
                multiroarBitCount(andRes));
        }
        for (int c = 0; c < NCHUNK; c++) {
            if (!multiroarBitGet(andRes,
                                 (uint64_t)c * BITMAP_SIZE_IN_BITS + 7)) {
                ERR("AndN dedup: common bit in chunk %d missing", c);
            }
        }
        for (int r = 0; r < NR; r++) {
            multiroarFree(roars[r]);
        }
    }

    TEST("N-way operations equivalence to chained binary") {
        cleaner(multiroarFree) multiroar *r1 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r2 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r3 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r4 = multiroarBitNew();

        srand(44444);
        for (uint64_t i = 0; i < 100; i++) {
            multiroarBitSet(r1, rand() % 500);
            multiroarBitSet(r2, rand() % 500);
            multiroarBitSet(r3, rand() % 500);
            multiroarBitSet(r4, rand() % 500);
        }

        multiroar *roars[] = {r1, r2, r3, r4};

        /* Test N-way AND vs chained binary */
        cleaner(multiroarFree) multiroar *nwayAnd = multiroarNewAndN(4, roars);
        cleaner(multiroarFree) multiroar *chainedAnd = multiroarDuplicate(r1);
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
        cleaner(multiroarFree) multiroar *nwayOr = multiroarNewOrN(4, roars);
        cleaner(multiroarFree) multiroar *chainedOr = multiroarDuplicate(r1);
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
    }

    TEST("N-way operations with many inputs (N=10)") {
        const uint64_t n = 10;
        __attribute__((cleanup(roarArrayFree_cleanup))) roarArray roarsArr = {
            .items = zcalloc(n, sizeof(multiroar *)), .count = (uint64_t)n};
        multiroar **roars = roarsArr.items;

        srand(55555);
        for (uint64_t i = 0; i < n; i++) {
            /* @own: container; free=multiroarFree */
            roars[i] = multiroarBitNew();
            /* Each roar sets random bits */
            for (uint64_t j = 0; j < 50; j++) {
                multiroarBitSet(roars[i], rand() % 1000);
            }
        }

        /* Test N-way OR - should combine all */
        cleaner(multiroarFree) multiroar *orResult = multiroarNewOrN(n, roars);
        uint64_t orCount = multiroarBitCount(orResult);

        /* OR count should be > 0 */
        if (orCount == 0) {
            ERRR("N=10 OR: count should be > 0");
        }

        /* Test N-way AND - should be subset of any individual */
        cleaner(multiroarFree) multiroar *andResult =
            multiroarNewAndN(n, roars);
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
    }

    TEST("FUZZ: N-way operations with random data") {
        printf("  Fuzzing N-way operations...\n");

        srand(66666);
        const uint64_t trials = 100;
        const uint64_t maxN = 7;

        for (uint64_t trial = 0; trial < trials; trial++) {
            uint64_t n = 2 + (rand() % (maxN - 1)); /* 2 to 7 inputs */
            __attribute__((
                cleanup(roarArrayFree_cleanup))) roarArray roarsArr = {
                .items = zcalloc(n, sizeof(multiroar *)), .count = (uint64_t)n};
            multiroar **roars = roarsArr.items;

            /* Create random roars */
            for (uint64_t i = 0; i < n; i++) {
                /* @own: container; free=multiroarFree */
                roars[i] = multiroarBitNew();
                uint64_t numBits = rand() % 200;
                for (uint64_t j = 0; j < numBits; j++) {
                    multiroarBitSet(roars[i], rand() % 1000);
                }
            }

            /* Test N-way operations don't crash */
            cleaner(multiroarFree) multiroar *andResult =
                multiroarNewAndN(n, roars);
            cleaner(multiroarFree) multiroar *orResult =
                multiroarNewOrN(n, roars);
            cleaner(multiroarFree) multiroar *xorResult =
                multiroarNewXorN(n, roars);
            (void)xorResult;

            /* Basic sanity: AND count <= OR count */
            uint64_t andCount = multiroarBitCount(andResult);
            uint64_t orCount = multiroarBitCount(orResult);

            if (andCount > orCount) {
                ERR("Trial %" PRIu64 ": AND count %" PRIu64
                    " > OR count %" PRIu64 "",
                    trial, andCount, orCount);
            }
        }

        printf("  completed %" PRIu64 " fuzz trials\n", trials);
    }

    TEST("N-way operations with empty roars") {
        cleaner(multiroarFree) multiroar *r1 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r2 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r3 = multiroarBitNew();

        multiroarBitSet(r2, 100); /* Only r2 has a bit */

        multiroar *roars[] = {r1, r2, r3};

        /* AND with empty = empty */
        cleaner(multiroarFree) multiroar *andResult =
            multiroarNewAndN(3, roars);
        uint64_t andCount = multiroarBitCount(andResult);
        if (andCount != 0) {
            ERR("AND with empty should be 0, got %" PRIu64 "", andCount);
        }

        /* OR with empty = union of non-empty */
        cleaner(multiroarFree) multiroar *orResult = multiroarNewOrN(3, roars);
        uint64_t orCount = multiroarBitCount(orResult);
        if (orCount != 1) {
            ERR("OR with empty should be 1, got %" PRIu64 "", orCount);
        }
    }

    TEST("bitcount + N-way integration") {
        cleaner(multiroarFree) multiroar *r1 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r2 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r3 = multiroarBitNew();

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
        cleaner(multiroarFree) multiroar *andResult =
            multiroarNewAndN(3, roars);
        uint64_t andCount = multiroarBitCount(andResult);
        if (andCount != 50) {
            ERR("AND count should be 50, got %" PRIu64 "", andCount);
        }

        /* OR: union [0..249] = 250 bits */
        cleaner(multiroarFree) multiroar *orResult = multiroarNewOrN(3, roars);
        uint64_t orCount = multiroarBitCount(orResult);
        if (orCount != 250) {
            ERR("OR count should be 250, got %" PRIu64 "", orCount);
        }

        printf("  Integration: AND=%" PRIu64 " OR=%" PRIu64 "\n", andCount,
               orCount);
    }

    printf("\n=== All new functionality tests completed! ===\n\n");

    /* ================================================================
     * ADVANCED FUZZING AND EDGE CASE TESTS
     * ================================================================ */

    printf("=== ADVANCED FUZZING TESTS ===\n\n");

    TEST("FUZZ: bitcount with all chunk types (100 iterations)") {
        srand(99999);
        /* Bits spread over one chunk, three chunks or 122 chunks, plus a
         * range, so chunks are sparse, bitmaps, inverted listings and
         * full */
        static const uint64_t spreads[] = {8192, 3 * 8192, 1000000};
        for (int trial = 0; trial < 100; trial++) {
            cleaner(multiroarFree) multiroar *r = multiroarBitNew();
            uint64_t expectedCount = 0;
            const uint64_t spread = spreads[trial % 3];

            const uint64_t rangeStart = (uint64_t)rand() % spread;
            const uint64_t rangeLen = (uint64_t)rand() % 20000;
            multiroarBitSetRange(r, rangeStart, rangeLen);
            expectedCount = rangeLen;

            int numBits = rand() % 10000;
            for (int i = 0; i < numBits; i++) {
                uint64_t pos = ((uint64_t)rand() * rand()) % spread;
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
        }
        printf("  Completed 1K bitcount fuzz iterations\n");
    }

    TEST("FUZZ: N-way AND with random overlaps (1K iterations)") {
        srand(111111);
        for (int trial = 0; trial < 1000; trial++) {
            int n = 2 + (rand() % 8); /* 2-9 inputs */
            __attribute__((
                cleanup(roarArrayFree_cleanup))) roarArray roarsArr = {
                .items = zcalloc(n, sizeof(multiroar *)), .count = (uint64_t)n};
            multiroar **roars = roarsArr.items;

            for (int i = 0; i < n; i++) {
                /* @own: container; free=multiroarFree */
                roars[i] = multiroarBitNew();
                /* Set overlapping ranges */
                uint64_t start = (rand() % 500);
                uint64_t count = 50 + (rand() % 200);
                for (uint64_t j = start; j < start + count; j++) {
                    multiroarBitSet(roars[i], j);
                }
            }

            /* Test N-way vs chained binary */
            cleaner(multiroarFree) multiroar *nwayResult =
                multiroarNewAndN(n, roars);
            cleaner(multiroarFree) multiroar *chainedResult =
                multiroarDuplicate(roars[0]);
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
        }
        printf("  Completed 1K N-way AND fuzz iterations\n");
    }

    TEST("FUZZ: N-way OR with random overlaps (1K iterations)") {
        srand(222222);
        for (int trial = 0; trial < 1000; trial++) {
            int n = 2 + (rand() % 8);
            __attribute__((
                cleanup(roarArrayFree_cleanup))) roarArray roarsArr = {
                .items = zcalloc(n, sizeof(multiroar *)), .count = (uint64_t)n};
            multiroar **roars = roarsArr.items;

            for (int i = 0; i < n; i++) {
                /* @own: container; free=multiroarFree */
                roars[i] = multiroarBitNew();
                uint64_t start = (rand() % 500);
                uint64_t count = 50 + (rand() % 200);
                for (uint64_t j = start; j < start + count; j++) {
                    multiroarBitSet(roars[i], j);
                }
            }

            cleaner(multiroarFree) multiroar *nwayResult =
                multiroarNewOrN(n, roars);
            cleaner(multiroarFree) multiroar *chainedResult =
                multiroarDuplicate(roars[0]);
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
        }
        printf("  Completed 1K N-way OR fuzz iterations\n");
    }

    TEST("FUZZ: N-way XOR with random overlaps (1K iterations)") {
        srand(333333);
        for (int trial = 0; trial < 1000; trial++) {
            int n = 2 + (rand() % 8);
            __attribute__((
                cleanup(roarArrayFree_cleanup))) roarArray roarsArr = {
                .items = zcalloc(n, sizeof(multiroar *)), .count = (uint64_t)n};
            multiroar **roars = roarsArr.items;

            for (int i = 0; i < n; i++) {
                /* @own: container; free=multiroarFree */
                roars[i] = multiroarBitNew();
                uint64_t start = (rand() % 500);
                uint64_t count = 50 + (rand() % 200);
                for (uint64_t j = start; j < start + count; j++) {
                    multiroarBitSet(roars[i], j);
                }
            }

            cleaner(multiroarFree) multiroar *nwayResult =
                multiroarNewXorN(n, roars);
            cleaner(multiroarFree) multiroar *chainedResult =
                multiroarDuplicate(roars[0]);
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
        }
        printf("  Completed 1K N-way XOR fuzz iterations\n");
    }

    TEST("EDGE: bitcount with chunk type transitions") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("EDGE: N-way operations with very large N (N=100)") {
        const int n = 100;
        __attribute__((cleanup(roarArrayFree_cleanup))) roarArray roarsArr = {
            .items = zcalloc(n, sizeof(multiroar *)), .count = (uint64_t)n};
        multiroar **roars = roarsArr.items;

        srand(444444);
        for (int i = 0; i < n; i++) {
            /* @own: container; free=multiroarFree */
            roars[i] = multiroarBitNew();
            /* Each sets bit at position i to guarantee some overlap */
            multiroarBitSet(roars[i], i);
            /* Plus some random bits */
            for (int j = 0; j < 10; j++) {
                multiroarBitSet(roars[i], rand() % 1000);
            }
        }

        cleaner(multiroarFree) multiroar *orResult = multiroarNewOrN(n, roars);
        uint64_t orCount = multiroarBitCount(orResult);

        /* OR should have at least N bits (positions 0..N-1) */
        if (orCount < (uint64_t)n) {
            ERR("N=100 OR: count %" PRIu64 " should be >= %d", orCount, n);
        }

        cleaner(multiroarFree) multiroar *andResult =
            multiroarNewAndN(n, roars);
        uint64_t andCount = multiroarBitCount(andResult);

        /* AND should be small (likely 0) since each roar has unique bits */
        if (andCount > 50) {
            ERR("N=100 AND: count %" PRIu64 " unexpectedly large", andCount);
        }

        printf("  N=100: OR=%" PRIu64 " AND=%" PRIu64 "\n", orCount, andCount);
    }

    TEST("EDGE: Multi-chunk operations across boundaries") {
        cleaner(multiroarFree) multiroar *r1 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r2 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r3 = multiroarBitNew();

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
        cleaner(multiroarFree) multiroar *andResult =
            multiroarNewAndN(3, roars);
        uint64_t andCount = multiroarBitCount(andResult);

        /* Should have overlaps: chunk0=[1000..1999], chunk1=[9192..10191],
         * chunk10=[82920..83919] */
        uint64_t expectedAnd = 1000 + 1000 + 1000; /* 3000 bits total overlap */
        if (andCount != expectedAnd) {
            ERR("Multi-chunk AND: expected %" PRIu64 ", got %" PRIu64 "",
                expectedAnd, andCount);
        }

        /* Test OR across chunks */
        cleaner(multiroarFree) multiroar *orResult = multiroarNewOrN(3, roars);
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
    }

    TEST("EDGE: Operations with all chunk types mixed") {
        cleaner(multiroarFree) multiroar *sparse = multiroarBitNew();
        cleaner(multiroarFree) multiroar *dense = multiroarBitNew();
        cleaner(multiroarFree) multiroar *inverted = multiroarBitNew();
        cleaner(multiroarFree) multiroar *full = multiroarBitNew();

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
        cleaner(multiroarFree) multiroar *andResult =
            multiroarNewAndN(4, roars);
        uint64_t andCount = multiroarBitCount(andResult);
        /* All have bits [0, 10, 20, ... 290] in common = ~30 bits */
        if (andCount == 0 || andCount > 300) {
            ERR("Mixed types AND: count %" PRIu64 " unexpected", andCount);
        }

        /* OR of all types - should be full chunk */
        cleaner(multiroarFree) multiroar *orResult = multiroarNewOrN(4, roars);
        uint64_t orCount = multiroarBitCount(orResult);
        if (orCount != 8192) {
            ERR("Mixed types OR: expected 8192, got %" PRIu64 "", orCount);
        }

        printf("  Mixed chunk types: AND=%" PRIu64 " OR=%" PRIu64 "\n",
               andCount, orCount);
    }

    TEST("EDGE: Bitcount with very large positions") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    TEST("EDGE: N=2 equivalence to binary operations") {
        cleaner(multiroarFree) multiroar *r1 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r2 = multiroarBitNew();

        srand(555555);
        for (int i = 0; i < 1000; i++) {
            multiroarBitSet(r1, rand() % 5000);
            multiroarBitSet(r2, rand() % 5000);
        }

        multiroar *roars[] = {r1, r2};

        /* Test N=2 AND vs binary AND */
        cleaner(multiroarFree) multiroar *nwayAnd = multiroarNewAndN(2, roars);
        cleaner(multiroarFree) multiroar *binaryAnd = multiroarNewAnd(r1, r2);

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
        cleaner(multiroarFree) multiroar *nwayOr = multiroarNewOrN(2, roars);
        cleaner(multiroarFree) multiroar *binaryOr = multiroarNewOr(r1, r2);

        nwayCount = multiroarBitCount(nwayOr);
        binaryCount = multiroarBitCount(binaryOr);

        if (nwayCount != binaryCount) {
            ERR("N=2 OR: nway=%" PRIu64 " binary=%" PRIu64 "", nwayCount,
                binaryCount);
        }

        /* Test N=2 XOR vs binary XOR */
        cleaner(multiroarFree) multiroar *nwayXor = multiroarNewXorN(2, roars);
        cleaner(multiroarFree) multiroar *binaryXor = multiroarNewXor(r1, r2);

        nwayCount = multiroarBitCount(nwayXor);
        binaryCount = multiroarBitCount(binaryXor);

        if (nwayCount != binaryCount) {
            ERR("N=2 XOR: nway=%" PRIu64 " binary=%" PRIu64 "", nwayCount,
                binaryCount);
        }
    }

    TEST("PERF: Bitcount performance across different densities") {
        const int iterations = 10000;

        /* Sparse */
        cleaner(multiroarFree) multiroar *sparse = multiroarBitNew();
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
        cleaner(multiroarFree) multiroar *dense = multiroarBitNew();
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
    }

    TEST("PERF: similarity metrics") {
        /* Two overlapping roars over 24 chunks of mixed density; ns per
         * call for each metric */
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();
        uint64_t rng = 0x51b;
        for (uint64_t c = 0; c < 24; c++) {
            const uint64_t fill = c % 3 == 0 ? 7000 : c % 3 == 1 ? 3000 : 200;
            for (uint64_t k = 0; k < fill; k++) {
                multiroarBitSet(a, c * 8192 + splitmix64(&rng) % 8192);
                multiroarBitSet(b, c * 8192 + splitmix64(&rng) % 8192);
            }
        }
        const int rounds = 200;
        double sink = 0;
        int64_t t0 = timeUtilMonotonicNs();
        for (int i = 0; i < rounds; i++) {
            sink += multiroarJaccard(a, b);
        }
        int64_t t1 = timeUtilMonotonicNs();
        for (int i = 0; i < rounds; i++) {
            sink += (double)multiroarHammingDistance(a, b);
        }
        int64_t t2 = timeUtilMonotonicNs();
        for (int i = 0; i < rounds; i++) {
            sink += multiroarDice(a, b) + multiroarOverlap(a, b);
        }
        int64_t t3 = timeUtilMonotonicNs();
        printf("similarity: jaccard %" PRId64 " ns, hamming %" PRId64
               " ns, dice+overlap %" PRId64 " ns per call (sink %g)\n",
               (t1 - t0) / rounds, (t2 - t1) / rounds, (t3 - t2) / rounds,
               sink);
    }

    TEST("PERF: N-way operations vs chained binary (N=10)") {
        const int n = 10;
        __attribute__((cleanup(roarArrayFree_cleanup))) roarArray roarsArr = {
            .items = zcalloc(n, sizeof(multiroar *)), .count = (uint64_t)n};
        multiroar **roars = roarsArr.items;

        srand(666666);
        for (int i = 0; i < n; i++) {
            /* @own: container; free=multiroarFree */
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
            cleaner(multiroarFree) multiroar *result =
                multiroarNewAndN(n, roars);
            (void)result;
        }
        clock_gettime(CLOCK_MONOTONIC, &end);
        double nwayNs =
            (end.tv_sec - start.tv_sec) * 1e9 + (end.tv_nsec - start.tv_nsec);
        nwayNs /= iterations;

        /* Chained binary AND performance */
        clock_gettime(CLOCK_MONOTONIC, &start);
        for (int i = 0; i < iterations; i++) {
            cleaner(multiroarFree) multiroar *result =
                multiroarDuplicate(roars[0]);
            for (int j = 1; j < n; j++) {
                multiroarAnd(result, roars[j]);
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &end);
        double chainedNs =
            (end.tv_sec - start.tv_sec) * 1e9 + (end.tv_nsec - start.tv_nsec);
        chainedNs /= iterations;

        printf("  N=10 AND: N-way=%.0f ns, Chained=%.0f ns, Ratio=%.2fx\n",
               nwayNs, chainedNs, chainedNs / nwayNs);
    }

    printf("\n=== All advanced fuzzing tests completed! ===\n\n");

    /* ====================================================================
     * Min/Max/IsEmpty/Comparison Operations Tests
     * ==================================================================== */

    TEST("multiroarMin/Max - empty bitmap") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
        uint64_t position;

        if (multiroarMin(r, &position)) {
            ERRR("Min should return false for empty bitmap");
        }
        if (multiroarMax(r, &position)) {
            ERRR("Max should return false for empty bitmap");
        }
    }

    TEST("multiroarIsEmpty - empty and non-empty") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    TEST("multiroarMin/Max - single bit at various positions") {
        uint64_t testPositions[] = {0,    1,    100,    8191,
                                    8192, 8193, 100000, 1000000};

        for (uint64_t i = 0;
             i < sizeof(testPositions) / sizeof(testPositions[0]); i++) {
            cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
        }
    }

    TEST("multiroarMin/Max - sparse chunk (UNDER_FULL)") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    TEST("multiroarMin/Max - dense chunk (FULL_BITMAP)") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    TEST("multiroarMin/Max - inverted chunk (OVER_FULL)") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    TEST("multiroarMin/Max - full chunk (ALL_1)") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    TEST("multiroarMin/Max - multiple chunks") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    TEST("multiroarMin/Max - chunk boundaries") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    TEST("multiroarIntersects - empty and disjoint") {
        cleaner(multiroarFree) multiroar *r1 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r2 = multiroarBitNew();

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
    }

    TEST("multiroarIntersects - overlapping") {
        cleaner(multiroarFree) multiroar *r1 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r2 = multiroarBitNew();

        /* Same chunk, overlapping bits */
        multiroarBitSet(r1, 100);
        multiroarBitSet(r1, 200);
        multiroarBitSet(r2, 200);
        multiroarBitSet(r2, 300);

        if (!multiroarIntersects(r1, r2)) {
            ERRR("Overlapping bitmaps should intersect");
        }
    }

    TEST("multiroarIntersects - different chunks") {
        cleaner(multiroarFree) multiroar *r1 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r2 = multiroarBitNew();

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
    }

    TEST("multiroarIsSubset - empty sets") {
        cleaner(multiroarFree) multiroar *r1 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r2 = multiroarBitNew();

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
    }

    TEST("multiroarIsSubset - equal sets") {
        cleaner(multiroarFree) multiroar *r1 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r2 = multiroarBitNew();

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
    }

    TEST("multiroarIsSubset - proper subset") {
        cleaner(multiroarFree) multiroar *r1 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r2 = multiroarBitNew();

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
    }

    TEST("multiroarIsSubset - non-subset") {
        cleaner(multiroarFree) multiroar *r1 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r2 = multiroarBitNew();

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
    }

    TEST("multiroarEquals - empty bitmaps") {
        cleaner(multiroarFree) multiroar *r1 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r2 = multiroarBitNew();

        if (!multiroarEquals(r1, r2)) {
            ERRR("Empty bitmaps should be equal");
        }
    }

    TEST("multiroarEquals - identical bitmaps") {
        cleaner(multiroarFree) multiroar *r1 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r2 = multiroarBitNew();

        multiroarBitSet(r1, 100);
        multiroarBitSet(r1, 200);
        multiroarBitSet(r1, 10000);

        multiroarBitSet(r2, 100);
        multiroarBitSet(r2, 200);
        multiroarBitSet(r2, 10000);

        if (!multiroarEquals(r1, r2)) {
            ERRR("Identical bitmaps should be equal");
        }
    }

    TEST("multiroarEquals - different sizes") {
        cleaner(multiroarFree) multiroar *r1 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r2 = multiroarBitNew();

        multiroarBitSet(r1, 100);
        multiroarBitSet(r2, 100);
        multiroarBitSet(r2, 200);

        if (multiroarEquals(r1, r2)) {
            ERRR("Bitmaps with different sizes should not be equal");
        }
    }

    TEST("multiroarEquals - different positions") {
        cleaner(multiroarFree) multiroar *r1 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r2 = multiroarBitNew();

        multiroarBitSet(r1, 100);
        multiroarBitSet(r2, 200);

        if (multiroarEquals(r1, r2)) {
            ERRR("Bitmaps with different positions should not be equal");
        }
    }

    TEST("multiroarEquals - different chunks") {
        cleaner(multiroarFree) multiroar *r1 = multiroarBitNew();
        cleaner(multiroarFree) multiroar *r2 = multiroarBitNew();

        multiroarBitSet(r1, 100);
        multiroarBitSet(r2, 10000);

        if (multiroarEquals(r1, r2)) {
            ERRR("Bitmaps with bits in different chunks should not be equal");
        }
    }

    TEST_DESC("min/max/comparison fuzzing - %d random operations", 300) {
        /* Word-array oracles: exact min/max, IsEmpty, Intersects and
         * Equals; every fourth trial builds r2 from r1's bits in reverse
         * order, so Equals sees equal roars built differently */
        for (int trial = 0; trial < 300; trial++) {
            cleaner(multiroarFree) multiroar *r1 = multiroarBitNew();
            cleaner(multiroarFree) multiroar *r2 = multiroarBitNew();
            uint64_t o1[FUZZ_ORACLE_WORDS] = {0};
            uint64_t o2[FUZZ_ORACLE_WORDS] = {0};

            const int numBits1 = rand() % 500;
            uint64_t min1 = UINT64_MAX;
            uint64_t max1 = 0;
            for (int i = 0; i < numBits1; i++) {
                const uint64_t pos =
                    ((uint64_t)rand() * rand()) % FUZZ_ORACLE_MAX_BIT;
                multiroarBitSet(r1, pos);
                FUZZ_ORACLE_SET(o1, pos);
                min1 = pos < min1 ? pos : min1;
                max1 = pos > max1 ? pos : max1;
            }

            if (trial % 4 == 0) {
                for (uint64_t pos = FUZZ_ORACLE_MAX_BIT; pos-- > 0;) {
                    if (FUZZ_ORACLE_GET(o1, pos)) {
                        multiroarBitSet(r2, pos);
                        FUZZ_ORACLE_SET(o2, pos);
                    }
                }
            } else {
                const int numBits2 = rand() % 500;
                for (int i = 0; i < numBits2; i++) {
                    const uint64_t pos =
                        ((uint64_t)rand() * rand()) % FUZZ_ORACLE_MAX_BIT;
                    multiroarBitSet(r2, pos);
                    FUZZ_ORACLE_SET(o2, pos);
                }
            }

            uint64_t actualMin = 0;
            uint64_t actualMax = 0;
            const bool hasMin = multiroarMin(r1, &actualMin);
            const bool hasMax = multiroarMax(r1, &actualMax);
            if (numBits1 > 0) {
                if (!hasMin || !hasMax || actualMin != min1 ||
                    actualMax != max1) {
                    ERR("min/max %" PRIu64 "/%" PRIu64 ", want %" PRIu64
                        "/%" PRIu64,
                        actualMin, actualMax, min1, max1);
                }
            } else if (hasMin || hasMax) {
                ERRR("Min/Max should fail for an empty bitmap");
            }

            if (multiroarIsEmpty(r1) != (numBits1 == 0)) {
                ERR("IsEmpty mismatch: expected %d", numBits1 == 0);
            }

            bool intersects = false;
            bool equal = true;
            for (size_t w = 0; w < FUZZ_ORACLE_WORDS; w++) {
                intersects |= (o1[w] & o2[w]) != 0;
                equal &= o1[w] == o2[w];
            }
            if (multiroarIntersects(r1, r2) != intersects) {
                ERR("Intersects mismatch: expected %d", intersects);
            }
            if (multiroarEquals(r1, r2) != equal) {
                ERR("Equals mismatch: expected %d", equal);
            }
        }
    }

    printf("\n=== Min/Max/Comparison tests completed! ===\n\n");

    /* ====================================================================
     * Rank/Select Operations Tests
     * ==================================================================== */

    TEST("multiroarRank - empty bitmap") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

        if (multiroarRank(r, 0) != 0) {
            ERRR("Rank at 0 for empty bitmap should be 0");
        }
        if (multiroarRank(r, 100) != 0) {
            ERRR("Rank at 100 for empty bitmap should be 0");
        }
    }

    TEST("multiroarRank - single bit") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("multiroarRank - sparse chunk") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("multiroarRank - dense chunk") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("multiroarRank - multi-chunk") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("multiroarSelect - empty bitmap") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
        uint64_t pos;

        if (multiroarSelect(r, 0, &pos)) {
            ERRR("Select with k=0 should fail");
        }
        if (multiroarSelect(r, 1, &pos)) {
            ERRR("Select on empty bitmap should fail");
        }
    }

    TEST("multiroarSelect - single bit") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
        multiroarBitSet(r, 100);

        uint64_t pos;
        if (!multiroarSelect(r, 1, &pos) || pos != 100) {
            ERR("Select 1st bit: expected 100, got %" PRIu64, pos);
        }
        if (multiroarSelect(r, 2, &pos)) {
            ERRR("Select 2nd bit should fail (only 1 bit set)");
        }
    }

    TEST("multiroarSelect - sparse chunk") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("multiroarSelect - dense chunk") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("multiroarSelect - multi-chunk") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("multiroarRank/Select - invariant: rank(select(k)) == k") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    TEST_DESC("rank/select fuzzing - %d random operations", 1000) {
        for (int trial = 0; trial < 1000; trial++) {
            cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
        }
    }

    TEST("rank/select - edge case: max uint64_t positions") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    TEST("rank/select - edge case: sparse bitmaps") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    TEST("rank/select - edge case: dense consecutive bits") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    TEST("rank/select - edge case: alternating bits") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    TEST("rank/select - edge case: single bit") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    TEST("rank/select - edge case: duplicate positions in fuzzing") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    printf("\n=== Rank/Select tests completed! ===\n\n");

    /* ====================================================================
     * Range Operations Tests
     * ==================================================================== */

    TEST("multiroarRangeCount - empty bitmap") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

        if (multiroarRangeCount(r, 0, 100) != 0) {
            ERRR("RangeCount on empty bitmap should be 0");
        }
    }

    TEST("multiroarRangeCount - basic range") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("multiroarBitClearRange - basic clear") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("multiroarBitClearRange - multi-chunk") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("multiroarBitFlipRange - basic flip") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("multiroarBitFlipRange - double flip") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
        multiroarBitSet(r, 50);

        /* Flip twice should restore */
        multiroarBitFlipRange(r, 40, 20);
        multiroarBitFlipRange(r, 40, 20);

        if (!multiroarBitGet(r, 50)) {
            ERRR("Double flip should restore original state");
        }
    }

    TEST("multiroarAndNot - basic difference") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

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
    }

    TEST("multiroarNewAndNot - creates new result") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

        for (uint64_t i = 0; i < 100; i++) {
            multiroarBitSet(a, i);
        }
        for (uint64_t i = 50; i < 150; i++) {
            multiroarBitSet(b, i);
        }

        /* A - B = [0, 50) */
        cleaner(multiroarFree) multiroar *result = multiroarNewAndNot(a, b);

        if (multiroarBitCount(result) != 50) {
            ERR("NewAndNot count: expected 50, got %" PRIu64 "",
                multiroarBitCount(result));
        }

        /* Verify a is unchanged */
        if (multiroarBitCount(a) != 100) {
            ERRR("Original A should be unchanged");
        }
    }

    TEST("multiroarAndNot - disjoint sets") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

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
    }

    TEST("multiroarAndNot - complete overlap") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

        for (uint64_t i = 0; i < 50; i++) {
            multiroarBitSet(a, i);
            multiroarBitSet(b, i);
        }

        multiroarAndNot(a, b);

        if (multiroarBitCount(a) != 0) {
            ERRR("Complete overlap: A - A should be empty");
        }
    }

    TEST("multiroarBitFlipRange - minimal debug test") {
        /* Minimal test to debug the assertion failure */
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
    }

    TEST_DESC("range operations fuzzing - %d random operations", 500) {
        for (int trial = 0; trial < 500; trial++) {
            cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
            cleaner(multiroarFree) multiroar *r2 = multiroarDuplicate(r);
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

            /* Test FlipRange */
            cleaner(multiroarFree) multiroar *r3 = multiroarDuplicate(r);
            uint64_t flipStart = rand() % 1000;
            uint64_t flipExtent = rand() % 500;

            multiroarBitFlipRange(r3, flipStart, flipExtent);
            multiroarBitFlipRange(r3, flipStart, flipExtent); /* Flip twice */

            /* Should equal original */
            if (!multiroarEquals(r, r3)) {
                ERRR("Double flip should restore original");
            }
        }
    }

    printf("\n=== Range operations tests completed! ===\n\n");

    /* ================================================================
     * Iterator Tests
     * ================================================================ */

    TEST("multiroarIterator - empty bitmap") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
        multiroarIterator iter;
        multiroarIteratorInit(r, &iter);

        uint64_t pos;
        if (multiroarIteratorNext(&iter, &pos)) {
            ERRR("Empty bitmap should have no positions");
        }
    }

    TEST("multiroarIterator - single bit") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("multiroarIterator - sparse chunk") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("multiroarIterator - dense chunk") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("multiroarIterator - multiple chunks") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("multiroarIterator - verify count matches bitcount") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("multiroarIterator - ascending order") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("multiroarIteratorReset - reset and re-iterate") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST_DESC("iterator fuzzing - %d random bitmaps", 500) {
        for (int trial = 0; trial < 500; trial++) {
            cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
        }
    }

    printf("\n=== Iterator tests completed! ===\n\n");

    /* ================================================================
     * Bulk Operations Tests
     * ================================================================ */

    TEST("multiroarBitSetMany - basic") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("multiroarBitGetMany - basic") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("multiroarToArray - basic") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("multiroarToArray - limited capacity") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
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
    }

    TEST("multiroarFromArray - basic") {
        uint64_t positions[] = {10, 20, 30, 40, 50};
        cleaner(multiroarFree) multiroar *r = multiroarFromArray(positions, 5);

        if (multiroarBitCount(r) != 5) {
            ERR("Expected 5 bits, got %" PRIu64 "", multiroarBitCount(r));
        }

        for (int i = 0; i < 5; i++) {
            if (!multiroarBitGet(r, positions[i])) {
                ERR("Position %" PRIu64 " should be set", positions[i]);
            }
        }
    }

    TEST("multiroarFromArray - with duplicates") {
        uint64_t positions[] = {10, 20, 10, 30, 20, 40};
        cleaner(multiroarFree) multiroar *r = multiroarFromArray(positions, 6);

        /* Should have 4 unique positions */
        if (multiroarBitCount(r) != 4) {
            ERR("Expected 4 unique bits, got %" PRIu64 "",
                multiroarBitCount(r));
        }
    }

    TEST("ToArray/FromArray round-trip") {
        cleaner(multiroarFree) multiroar *r1 = multiroarBitNew();
        /* Set random bits */
        for (int i = 0; i < 100; i++) {
            uint64_t pos = ((uint64_t)rand() * rand()) % 10000;
            multiroarBitSet(r1, pos);
        }

        uint64_t count1 = multiroarBitCount(r1);

        /* Convert to array */
        cleanerName(zfree, UInt64) uint64_t *array =
            zcalloc(count1, sizeof(uint64_t));
        uint64_t arrayCount = multiroarToArray(r1, array, count1);

        if (arrayCount != count1) {
            ERR("ToArray returned %" PRIu64 ", expected %" PRIu64 "",
                arrayCount, count1);
        }

        /* Convert back from array */
        cleaner(multiroarFree) multiroar *r2 =
            multiroarFromArray(array, arrayCount);

        uint64_t count2 = multiroarBitCount(r2);
        if (count2 != count1) {
            ERR("Round-trip count mismatch: %" PRIu64 " != %" PRIu64 "", count2,
                count1);
        }

        /* Verify equality */
        if (!multiroarEquals(r1, r2)) {
            ERRR("Round-trip bitmaps not equal");
        }
    }

    TEST_DESC("bulk operations fuzzing - %d trials", 500) {
        for (int trial = 0; trial < 500; trial++) {
            cleaner(multiroarFree) multiroar *r = multiroarBitNew();

            /* Generate random positions */
            int numPositions = 50 + (rand() % 200);
            cleanerName(zfree, UInt64) uint64_t *positions =
                zcalloc(numPositions, sizeof(uint64_t));

            for (int i = 0; i < numPositions; i++) {
                positions[i] = ((uint64_t)rand() * rand()) % 100000;
            }

            /* Test SetMany */
            multiroarBitSetMany(r, positions, numPositions);

            /* Test GetMany */
            cleanerName(zfree, Bools) bool *results =
                zcalloc(numPositions, sizeof(bool));
            multiroarBitGetMany(r, positions, numPositions, results);

            for (int i = 0; i < numPositions; i++) {
                if (!results[i]) {
                    ERR("Trial %d: Position %" PRIu64 " should be set", trial,
                        positions[i]);
                }
            }

            /* Test ToArray */
            uint64_t bitCount = multiroarBitCount(r);
            cleanerName(zfree, UInt64) uint64_t *array =
                zcalloc(bitCount, sizeof(uint64_t));
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
        }
    }

    printf("\n=== Bulk operations tests completed! ===\n\n");

    /* ================================================================
     * Similarity Metrics Tests
     * ================================================================ */

    TEST("multiroarJaccard - identical sets") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

        for (uint64_t i = 0; i < 100; i++) {
            multiroarBitSet(a, i);
            multiroarBitSet(b, i);
        }

        double jaccard = multiroarJaccard(a, b);
        if (jaccard < 0.999 || jaccard > 1.001) {
            ERR("Identical sets should have Jaccard = 1.0, got %f", jaccard);
        }
    }

    TEST("multiroarJaccard - disjoint sets") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

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
    }

    TEST("multiroarJaccard - partial overlap") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

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
    }

    TEST("multiroarJaccard - empty sets") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

        double jaccard = multiroarJaccard(a, b);
        if (jaccard < 0.999 || jaccard > 1.001) {
            ERR("Empty sets should have Jaccard = 1.0, got %f", jaccard);
        }
    }

    TEST("multiroarHammingDistance - identical sets") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

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
    }

    TEST("multiroarHammingDistance - disjoint sets") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

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
    }

    TEST("multiroarOverlap - perfect overlap") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

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
    }

    TEST("multiroarDice - identical sets") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

        for (uint64_t i = 0; i < 100; i++) {
            multiroarBitSet(a, i);
            multiroarBitSet(b, i);
        }

        double dice = multiroarDice(a, b);
        if (dice < 0.999 || dice > 1.001) {
            ERR("Identical sets should have Dice = 1.0, got %f", dice);
        }
    }

    TEST("multiroarDice - partial overlap") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

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
    }

    TEST_DESC("similarity metrics fuzzing - %d trials", 500) {
        for (int trial = 0; trial < 500; trial++) {
            cleaner(multiroarFree) multiroar *a = multiroarBitNew();
            cleaner(multiroarFree) multiroar *b = multiroarBitNew();

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
        }
    }

    printf("\n=== Similarity metrics tests completed! ===\n\n");

    /* ================================================================
     * Statistics and Memory Tests
     * ================================================================ */

    TEST("multiroarMemoryUsage - empty") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
        uint64_t usage = multiroarMemoryUsage(r);

        if (usage < sizeof(multiroar)) {
            ERR("Memory usage should be at least %zu, got %" PRIu64 "",
                sizeof(multiroar), usage);
        }
    }

    TEST("multiroarMemoryUsage - non-empty") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

        for (uint64_t i = 0; i < 1000; i++) {
            multiroarBitSet(r, i);
        }

        uint64_t usage = multiroarMemoryUsage(r);
        if (usage < sizeof(multiroar)) {
            ERR("Memory usage should be at least %zu, got %" PRIu64 "",
                sizeof(multiroar), usage);
        }
    }

    printf("\n=== Statistics and memory tests completed! ===\n\n");

    /* ================================================================
     * Serialization Tests
     * ================================================================ */

    TEST("multiroarSerialize/Deserialize - empty roar") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

        uint64_t size = multiroarSerializedSize(r);
        cleanerName(zfree, Void) void *buf = zcalloc(size, 1);

        uint64_t written = multiroarSerialize(r, buf, size);
        if (written == 0) {
            ERRR("Failed to serialize empty roar");
        }

        cleaner(multiroarFree) multiroar *r2 =
            multiroarDeserialize(buf, written);
        if (!r2) {
            ERRR("Failed to deserialize empty roar");
        }

        if (!multiroarEquals(r, r2)) {
            ERRR("Deserialized roar doesn't match original");
        }
    }

    /* Every buffer short of the serialized size is refused without a
     * write past its end (exact-size heap buffers under ASan), and the
     * stamp gates deserialization */
    TEST("multiroarSerialize: short buffers and format stamps") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
        for (uint64_t b = 0; b < 600; b += 7) {
            multiroarBitSet(r, b);
        }
        multiroarBitSet(r, 70000); /* a second chunk */
        for (uint64_t b = 8192 * 3; b < 8192 * 4; b++) {
            multiroarBitSet(r, b); /* a full chunk */
        }
        const uint64_t size = multiroarSerializedSize(r);
        for (uint64_t cap = 0; cap < size; cap++) {
            cleanerName(zfree, Bytes) uint8_t *shortBuf =
                zmalloc(cap ? cap : 1);
            if (multiroarSerialize(r, shortBuf, cap) != 0) {
                ERR("serialized into %" PRIu64 " of %" PRIu64 " bytes", cap,
                    size);
            }
        }

        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(size);
        if (multiroarSerialize(r, buf, size) != size) {
            ERRR("exact-size serialize failed");
        }
        cleaner(multiroarFree) multiroar *back =
            multiroarDeserialize(buf, size);
        if (!back || !multiroarEquals(r, back)) {
            ERRR("round trip failed");
        }

        /* The same chunk stream under other stamps */
        const uint64_t raw = multiroarSerializedSizeRaw(r);
        const dkFormatStamp stamps[] = {
            {MULTIROAR_FORMAT_ID, MULTIROAR_FORMAT_VERSION + 1, 0},
            {MULTIROAR_FORMAT_ID, MULTIROAR_FORMAT_VERSION, 1},
            {MULTIROAR_FORMAT_ID + 1, MULTIROAR_FORMAT_VERSION, 0},
            {MULTIROAR_FORMAT_ID, MULTIROAR_FORMAT_VERSION, UINT64_C(1) << 40},
        };
        for (size_t s = 0; s < 4; s++) {
            cleanerName(zfree, Bytes) uint8_t *stamped = zmalloc(32 + raw);
            const size_t prefix = dkFormatPrefixWrite(&stamps[s], stamped, 32);
            memcpy(stamped + prefix, buf + (size - raw), raw);
            cleaner(multiroarFree) multiroar *got =
                multiroarDeserialize(stamped, prefix + raw);
            if ((got != NULL) != (s == 3)) {
                ERR("stamp %zu: restored %d", s, got != NULL);
            }
        }
    }

    TEST("value-mode roars round-trip with their width and dimensions") {
        static const struct {
            uint8_t width;
            uint64_t rows;
            uint64_t cols;
        } shapes[] = {{1, 0, 0}, {8, 100, 100}, {33, 0, 7}, {64, 3, 0}};
        for (size_t s = 0; s < 4; s++) {
            cleaner(multiroarFree) multiroar *r = multiroarValueNew(
                shapes[s].width, shapes[s].rows, shapes[s].cols);
            const uint64_t limit = shapes[s].rows && shapes[s].cols
                                       ? shapes[s].rows * shapes[s].cols
                                       : UINT64_MAX;
            const uint64_t top = shapes[s].width == 64
                                     ? UINT64_MAX
                                     : (UINT64_C(1) << shapes[s].width) - 1;

            /* Positions at both ends of the space, values at both ends of
             * the width, in no particular order */
            uint64_t positions[64];
            uint64_t values[64];
            size_t n = 0;
            uint64_t rng = 0x5eed + s;
            for (; n < 64; n++) {
                positions[n] =
                    n < 2 ? (n ? limit - 1 : 0) : splitmix64(&rng) % limit;
                values[n] = n % 3 == 0 ? top : splitmix64(&rng) & top;
                databox v = databoxNewUnsigned(values[n]);
                multiroarValueSet(r, positions[n], &v);
            }

            const uint64_t size = multiroarSerializedSize(r);
            cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(size);
            if (multiroarSerialize(r, buf, size) != size) {
                ERR("shape %zu: serialize failed", s);
                continue;
            }
            cleaner(multiroarFree) multiroar *back =
                multiroarDeserialize(buf, size);
            uint64_t rows;
            uint64_t cols;
            multiroarValueDimensions(back, &rows, &cols);
            if (!back || multiroarValueWidth(back) != shapes[s].width ||
                rows != shapes[s].rows || cols != shapes[s].cols ||
                multiroarValueCount(back) != multiroarValueCount(r)) {
                ERR("shape %zu: width, dimensions or count lost", s);
                continue;
            }

            /* The last value set at each position survives */
            for (size_t i = 0; i < n; i++) {
                uint64_t expect = values[i];
                for (size_t later = i + 1; later < n; later++) {
                    if (positions[later] == positions[i]) {
                        expect = values[later];
                    }
                }
                databox got = {{0}};
                if (!multiroarValueGet(back, positions[i], &got) ||
                    got.data.u != expect) {
                    ERR("shape %zu: position %" PRIu64 " lost its value", s,
                        positions[i]);
                    break;
                }
            }

            /* Clearing keeps the mode, width and dimensions */
            multiroarClear(back);
            multiroarValueDimensions(back, &rows, &cols);
            if (multiroarValueCount(back) != 0 ||
                multiroarValueWidth(back) != shapes[s].width ||
                rows != shapes[s].rows || cols != shapes[s].cols) {
                ERR("shape %zu: clear changed the roar's shape", s);
            }
        }
    }

    TEST("value-mode streams that break their declarations are refused") {
        /* Raw streams: [width][cols][rows][count]([gap][value])* */
        static const struct {
            uint8_t bytes[12];
            uint8_t len;
            bool valid;
        } streams[] = {
            {{4, 0, 0, 1, 5, 15}, 6, true},       /* 15 fits 4 bits */
            {{4, 0, 0, 1, 5, 16}, 6, false},      /* 16 does not */
            {{65, 0, 0, 0}, 4, false},            /* no 65-bit width */
            {{8, 2, 2, 1, 3, 1}, 6, true},        /* position 3 < 2 x 2 */
            {{8, 2, 2, 1, 4, 1}, 6, false},       /* position 4 is outside */
            {{8, 0, 0, 2, 1, 1, 0, 1}, 8, false}, /* repeated position */
            {{8, 0, 0, 1, 1, 1, 0}, 7, false},    /* a stray byte */
            {{8, 0, 0, 2, 1, 1}, 6, false},       /* a value missing */
        };
        for (size_t s = 0; s < sizeof(streams) / sizeof(*streams); s++) {
            cleaner(multiroarFree) multiroar *r =
                multiroarDeserializeRaw(streams[s].bytes, streams[s].len);
            if ((r != NULL) != streams[s].valid) {
                ERR("stream %zu: restored %d", s, r != NULL);
            }
        }
    }

    TEST("multiroarDeserialize refuses malformed listings") {
        /* One UNDER_FULL chunk: [magic][version][flags][1 chunk][id 0]
         * [kind][count][positions...] */
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
        multiroarBitSet(r, 5);
        multiroarBitSet(r, 9);
        uint8_t good[64];
        const uint64_t len = multiroarSerialize(r, good, sizeof(good));
        cleaner(multiroarFree) multiroar *ok = multiroarDeserialize(good, len);
        if (!ok || !multiroarEquals(r, ok)) {
            ERRR("The well-formed listing did not restore");
        }

        /* The two positions are the last two bytes (varints 5 and 9) */
        uint8_t bad[64];
        memcpy(bad, good, len);
        bad[len - 1] = 5; /* not ascending */
        multiroar *dup = multiroarDeserialize(bad, len);
        memcpy(bad, good, len);
        bad[len - 3] = 0; /* count 0 */
        multiroar *empty = multiroarDeserialize(bad, len - 2);
        if (dup || empty) {
            ERRR("A malformed listing restored");
        }
        multiroarFree(dup);
        multiroarFree(empty);
    }

    TEST("multiroarSerialize/Deserialize - single bit") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
        multiroarBitSet(r, 42);

        uint64_t size = multiroarSerializedSize(r);
        cleanerName(zfree, Void) void *buf = zcalloc(size, 1);

        uint64_t written = multiroarSerialize(r, buf, size);
        if (written == 0) {
            ERRR("Failed to serialize single bit");
        }

        cleaner(multiroarFree) multiroar *r2 =
            multiroarDeserialize(buf, written);
        if (!r2) {
            ERRR("Failed to deserialize single bit");
        }

        if (!multiroarBitGet(r2, 42)) {
            ERRR("Bit 42 not set after deserialization");
        }

        if (!multiroarEquals(r, r2)) {
            ERRR("Deserialized roar doesn't match original");
        }
    }

    TEST("multiroarSerialize/Deserialize - sparse chunk (UNDER_FULL)") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

        /* Set 50 bits in one chunk */
        for (uint64_t i = 0; i < 100; i += 2) {
            multiroarBitSet(r, i);
        }

        uint64_t size = multiroarSerializedSize(r);
        cleanerName(zfree, Void) void *buf = zcalloc(size, 1);

        uint64_t written = multiroarSerialize(r, buf, size);
        if (written == 0) {
            ERRR("Failed to serialize sparse chunk");
        }

        cleaner(multiroarFree) multiroar *r2 =
            multiroarDeserialize(buf, written);
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
    }

    TEST("multiroarSerialize/Deserialize - dense chunk (FULL_BITMAP)") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

        /* Set 4000 bits to force FULL_BITMAP */
        for (uint64_t i = 0; i < 4000; i++) {
            multiroarBitSet(r, i);
        }

        uint64_t size = multiroarSerializedSize(r);
        cleanerName(zfree, Void) void *buf = zcalloc(size, 1);

        uint64_t written = multiroarSerialize(r, buf, size);
        if (written == 0) {
            ERRR("Failed to serialize dense chunk");
        }

        cleaner(multiroarFree) multiroar *r2 =
            multiroarDeserialize(buf, written);
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
    }

    TEST("multiroarSerialize/Deserialize - inverted chunk (OVER_FULL)") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

        /* Fill entire chunk except 100 bits to force OVER_FULL */
        for (uint64_t i = 0; i < 8192; i++) {
            multiroarBitSet(r, i);
        }
        for (uint64_t i = 0; i < 100; i += 2) {
            multiroarRemove(r, i);
        }

        uint64_t size = multiroarSerializedSize(r);
        cleanerName(zfree, Void) void *buf = zcalloc(size, 1);

        uint64_t written = multiroarSerialize(r, buf, size);
        if (written == 0) {
            ERRR("Failed to serialize inverted chunk");
        }

        cleaner(multiroarFree) multiroar *r2 =
            multiroarDeserialize(buf, written);
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
    }

    TEST("multiroarSerialize/Deserialize - full chunk (ALL_1)") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

        /* Fill entire chunk */
        for (uint64_t i = 0; i < 8192; i++) {
            multiroarBitSet(r, i);
        }

        uint64_t size = multiroarSerializedSize(r);
        cleanerName(zfree, Void) void *buf = zcalloc(size, 1);

        uint64_t written = multiroarSerialize(r, buf, size);
        if (written == 0) {
            ERRR("Failed to serialize full chunk");
        }

        cleaner(multiroarFree) multiroar *r2 =
            multiroarDeserialize(buf, written);
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
    }

    TEST("multiroarSerialize/Deserialize - multiple chunks") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

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
        cleanerName(zfree, Void) void *buf = zcalloc(size, 1);

        uint64_t written = multiroarSerialize(r, buf, size);
        if (written == 0) {
            ERRR("Failed to serialize multiple chunks");
        }

        cleaner(multiroarFree) multiroar *r2 =
            multiroarDeserialize(buf, written);
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
    }

    TEST("multiroarSerialize - buffer too small") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
        multiroarBitSet(r, 42);

        /* Try to serialize with insufficient buffer */
        uint8_t buf[4]; /* Too small */
        uint64_t written = multiroarSerialize(r, buf, sizeof(buf));

        if (written != 0) {
            ERR("Should fail with buffer too small, but wrote %" PRIu64
                " bytes",
                written);
        }
    }

    TEST("multiroarDeserialize - invalid magic") {
        uint8_t buf[10] = {'X', 'X', 'X', 'X', 1, 0, 0};

        cleaner(multiroarFree) multiroar *r =
            multiroarDeserialize(buf, sizeof(buf));
        if (r != NULL) {
            ERRR("Should fail with invalid magic");
        }
    }

    TEST("multiroarDeserialize - invalid version") {
        uint8_t buf[10] = {'R', 'O', 'A', 'R', 99, 0, 0}; /* Version 99 */

        cleaner(multiroarFree) multiroar *r =
            multiroarDeserialize(buf, sizeof(buf));
        if (r != NULL) {
            ERRR("Should fail with invalid version");
        }
    }

    TEST("multiroarSerialize/Deserialize - round-trip iterator verification") {
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();

        /* Create diverse bitmap */
        for (uint64_t i = 0; i < 10000; i += 7) {
            multiroarBitSet(r, i);
        }

        uint64_t size = multiroarSerializedSize(r);
        cleanerName(zfree, Void) void *buf = zcalloc(size, 1);

        uint64_t written = multiroarSerialize(r, buf, size);
        if (written == 0) {
            ERRR("Serialization failed");
        }

        cleaner(multiroarFree) multiroar *r2 =
            multiroarDeserialize(buf, written);
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
    }

    TEST_DESC("serialization fuzzing - %d trials", 200) {
        for (int trial = 0; trial < 200; trial++) {
            cleaner(multiroarFree) multiroar *r = multiroarBitNew();

            /* Create random bitmap */
            int numBits = 10 + (rand() % 5000);
            for (int i = 0; i < numBits; i++) {
                uint64_t pos = ((uint64_t)rand() * rand() * rand()) % 1000000;
                multiroarBitSet(r, pos);
            }

            uint64_t size = multiroarSerializedSize(r);
            cleanerName(zfree, Void) void *buf = zcalloc(size, 1);

            uint64_t written = multiroarSerialize(r, buf, size);
            if (written == 0) {
                ERR("Trial %d: Serialization failed", trial);
            }

            cleaner(multiroarFree) multiroar *r2 =
                multiroarDeserialize(buf, written);
            if (!r2) {
                ERR("Trial %d: Deserialization failed", trial);
            }

            if (!multiroarEquals(r, r2)) {
                ERR("Trial %d: Round-trip equality failed", trial);
            }

            if (multiroarBitCount(r) != multiroarBitCount(r2)) {
                ERR("Trial %d: Bitcount mismatch", trial);
            }
        }
    }

    printf("\n=== Serialization tests completed! ===\n\n");

    /* ================================================================
     * CROSS-CONTAINER-TYPE OPERATION TESTS
     *
     * Roaring bitmaps use different container types based on density:
     * - UNDER_FULL: sparse array of positions (< 629 bits)
     * - FULL_BITMAP: actual 8192-bit bitmap (629-7563 bits)
     * - OVER_FULL: negative listing (> 7563 bits)
     * - ALL_1: implicit all-ones (all 8192 bits set)
     *
     * These tests verify set operations work correctly when combining
     * chunks of different container types.
     * ================================================================ */

    printf("\n=== Cross-Container-Type Operation Tests ===\n\n");

/* Helper to create a chunk with specific container type by population */
#define FILL_SPARSE(r, chunk, count)                                           \
    do {                                                                       \
        uint64_t base = (uint64_t)(chunk) * 8192;                              \
        for (int i = 0; i < (count); i++) {                                    \
            multiroarBitSet((r), base + (i * 12));                             \
        }                                                                      \
    } while (0)

#define FILL_DENSE(r, chunk, start, end)                                       \
    do {                                                                       \
        uint64_t base = (uint64_t)(chunk) * 8192;                              \
        for (int i = (start); i < (end); i++) {                                \
            multiroarBitSet((r), base + i);                                    \
        }                                                                      \
    } while (0)

#define FILL_ALL_ONES(r, chunk)                                                \
    do {                                                                       \
        uint64_t base = (uint64_t)(chunk) * 8192;                              \
        for (int i = 0; i < 8192; i++) {                                       \
            multiroarBitSet((r), base + i);                                    \
        }                                                                      \
    } while (0)

    TEST("AND: UNDER_FULL x UNDER_FULL (same chunk)") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

        /* Create sparse chunks (< 629 positions) */
        FILL_SPARSE(a, 0, 100); /* Positions 0, 12, 24, ... */
        FILL_SPARSE(b, 0, 50);  /* Overlaps first 50 positions of a */

        cleaner(multiroarFree) multiroar *result = multiroarNewAnd(a, b);

        /* Result should have 50 bits (the overlap) */
        uint64_t countResult = multiroarBitCount(result);
        if (countResult != 50) {
            ERR("UNDER_FULL x UNDER_FULL: expected 50, got %" PRIu64,
                countResult);
        }

        /* Verify specific bits */
        for (int i = 0; i < 50; i++) {
            if (!multiroarBitGet(result, i * 12)) {
                ERR("Missing bit at position %d", i * 12);
            }
        }
        for (int i = 50; i < 100; i++) {
            if (multiroarBitGet(result, i * 12)) {
                ERR("Unexpected bit at position %d", i * 12);
            }
        }
    }

    TEST("AND: FULL_BITMAP x FULL_BITMAP") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

        /* Create bitmap chunks (> 629 positions, contiguous) */
        FILL_DENSE(a, 0, 0, 4096);    /* First half of chunk */
        FILL_DENSE(b, 0, 2048, 6144); /* Middle portion, overlaps [2048,4096) */

        cleaner(multiroarFree) multiroar *result = multiroarNewAnd(a, b);

        /* Result should have 2048 bits (the overlap [2048, 4096)) */
        uint64_t countResult = multiroarBitCount(result);
        if (countResult != 2048) {
            ERR("FULL_BITMAP x FULL_BITMAP: expected 2048, got %" PRIu64,
                countResult);
        }

        /* Verify boundaries */
        if (multiroarBitGet(result, 2047)) {
            ERRR("Bit 2047 should NOT be set");
        }
        if (!multiroarBitGet(result, 2048)) {
            ERRR("Bit 2048 should be set");
        }
        if (!multiroarBitGet(result, 4095)) {
            ERRR("Bit 4095 should be set");
        }
        if (multiroarBitGet(result, 4096)) {
            ERRR("Bit 4096 should NOT be set");
        }
    }

    TEST("AND: UNDER_FULL x FULL_BITMAP") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

        /* a: sparse (100 positions spread out) */
        FILL_SPARSE(a, 0, 100);
        /* b: dense bitmap covering first 2000 positions */
        FILL_DENSE(b, 0, 0, 2000);

        cleaner(multiroarFree) multiroar *result = multiroarNewAnd(a, b);

        /* Positions 0,12,24,...,1188 (i*12 where i*12 < 2000) are in both
         * 1188 / 12 = 99 positions, but we need i*12 < 2000
         * Max i where i*12 < 2000: i < 166.67, so i <= 166
         * But we only filled 100 sparse positions, so all 100 should match */
        uint64_t countResult = multiroarBitCount(result);
        if (countResult != 100) {
            ERR("UNDER_FULL x FULL_BITMAP: expected 100, got %" PRIu64,
                countResult);
        }
    }

    TEST("AND: FULL_BITMAP x OVER_FULL (negative listing)") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

        /* a: bitmap covering [0, 4096) */
        FILL_DENSE(a, 0, 0, 4096);

        /* b: negative listing - almost all bits set (> 7563) */
        FILL_ALL_ONES(b, 0);
        /* Clear just a few bits to verify difference */
        multiroarRemove(b, 100);
        multiroarRemove(b, 200);
        multiroarRemove(b, 300);

        cleaner(multiroarFree) multiroar *result = multiroarNewAnd(a, b);

        /* Should have 4096 - 3 bits (the ones we cleared in b) */
        uint64_t countResult = multiroarBitCount(result);
        if (countResult != 4093) {
            ERR("FULL_BITMAP x OVER_FULL: expected 4093, got %" PRIu64,
                countResult);
        }

        /* Verify the cleared bits are not set */
        if (multiroarBitGet(result, 100)) {
            ERRR("Bit 100 should NOT be set");
        }
        if (multiroarBitGet(result, 200)) {
            ERRR("Bit 200 should NOT be set");
        }
        if (multiroarBitGet(result, 300)) {
            ERRR("Bit 300 should NOT be set");
        }
    }

    TEST("AND: ALL_1 x UNDER_FULL") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

        /* a: all ones */
        FILL_ALL_ONES(a, 0);
        /* b: sparse */
        FILL_SPARSE(b, 0, 200);

        cleaner(multiroarFree) multiroar *result = multiroarNewAnd(a, b);

        /* Result should equal b */
        uint64_t countResult = multiroarBitCount(result);
        if (countResult != 200) {
            ERR("ALL_1 x UNDER_FULL: expected 200, got %" PRIu64, countResult);
        }

        if (!multiroarEquals(result, b)) {
            ERRR("ALL_1 AND sparse should equal sparse");
        }
    }

    TEST("OR: UNDER_FULL x FULL_BITMAP") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

        /* a: sparse in chunk 0, positions 0,12,24,...,1188 */
        FILL_SPARSE(a, 0, 100);
        /* b: dense in chunk 0, positions [2000, 4000) */
        FILL_DENSE(b, 0, 2000, 4000);

        cleaner(multiroarFree) multiroar *result = multiroarNewOr(a, b);

        /* Result should have 100 + 2000 = 2100 bits (no overlap) */
        uint64_t countResult = multiroarBitCount(result);
        if (countResult != 2100) {
            ERR("OR UNDER_FULL x FULL_BITMAP: expected 2100, got %" PRIu64,
                countResult);
        }
    }

    TEST("OR: UNDER_FULL x OVER_FULL across different chunks") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

        /* a: sparse in chunk 0 */
        FILL_SPARSE(a, 0, 100);
        /* b: negative listing in chunk 1 (almost all ones) */
        FILL_ALL_ONES(b, 1);
        multiroarRemove(b, 8192 + 50);

        cleaner(multiroarFree) multiroar *result = multiroarNewOr(a, b);

        /* Result: 100 bits in chunk 0, 8191 bits in chunk 1 */
        uint64_t countResult = multiroarBitCount(result);
        if (countResult != 100 + 8191) {
            ERR("OR across chunks: expected %d, got %" PRIu64, 100 + 8191,
                countResult);
        }
    }

    TEST("XOR: FULL_BITMAP x FULL_BITMAP (partial overlap)") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

        /* a: [0, 3000) */
        FILL_DENSE(a, 0, 0, 3000);
        /* b: [2000, 5000) */
        FILL_DENSE(b, 0, 2000, 5000);

        cleaner(multiroarFree) multiroar *result = multiroarNewXor(a, b);

        /* XOR: [0,2000) + [3000,5000) = 2000 + 2000 = 4000 */
        uint64_t countResult = multiroarBitCount(result);
        if (countResult != 4000) {
            ERR("XOR FULL_BITMAP x FULL_BITMAP: expected 4000, got %" PRIu64,
                countResult);
        }

        /* Verify boundaries */
        if (!multiroarBitGet(result, 1999)) {
            ERRR("Bit 1999 should be set (in a only)");
        }
        if (multiroarBitGet(result, 2000)) {
            ERRR("Bit 2000 should NOT be set (in both)");
        }
        if (multiroarBitGet(result, 2999)) {
            ERRR("Bit 2999 should NOT be set (in both)");
        }
        if (!multiroarBitGet(result, 3000)) {
            ERRR("Bit 3000 should be set (in b only)");
        }
    }

    TEST("XOR: ALL_1 x FULL_BITMAP") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

        /* a: all ones in chunk 0 */
        FILL_ALL_ONES(a, 0);
        /* b: [0, 4096) in chunk 0 */
        FILL_DENSE(b, 0, 0, 4096);

        cleaner(multiroarFree) multiroar *result = multiroarNewXor(a, b);

        /* XOR: bits [4096, 8192) should be set (8192 - 4096 = 4096 bits) */
        uint64_t countResult = multiroarBitCount(result);
        if (countResult != 4096) {
            ERR("XOR ALL_1 x FULL_BITMAP: expected 4096, got %" PRIu64,
                countResult);
        }

        if (multiroarBitGet(result, 4095)) {
            ERRR("Bit 4095 should NOT be set");
        }
        if (!multiroarBitGet(result, 4096)) {
            ERRR("Bit 4096 should be set");
        }
    }

    TEST("ANDNOT: FULL_BITMAP - UNDER_FULL") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

        /* a: dense [0, 2000) */
        FILL_DENSE(a, 0, 0, 2000);
        /* b: sparse at 0,12,24,...,588 (50 positions) */
        FILL_SPARSE(b, 0, 50);

        cleaner(multiroarFree) multiroar *result = multiroarNewAndNot(a, b);

        /* Result: 2000 - 50 = 1950 bits */
        uint64_t countResult = multiroarBitCount(result);
        if (countResult != 1950) {
            ERR("ANDNOT FULL_BITMAP - UNDER_FULL: expected 1950, got %" PRIu64,
                countResult);
        }

        /* Verify removed bits */
        for (int i = 0; i < 50; i++) {
            if (multiroarBitGet(result, i * 12)) {
                ERR("Bit %d should be removed", i * 12);
            }
        }
    }

    TEST("ANDNOT: OVER_FULL - FULL_BITMAP") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

        /* a: almost all ones (negative listing) */
        FILL_ALL_ONES(a, 0);
        multiroarRemove(a, 100); /* 8191 bits set */

        /* b: dense [0, 4096) */
        FILL_DENSE(b, 0, 0, 4096);

        cleaner(multiroarFree) multiroar *result = multiroarNewAndNot(a, b);

        /* Result: bits in a not in b = [4096, 8192) minus bit 100 wasn't in a
         * But bit 100 is in [0,4096) so it's removed by ANDNOT anyway
         * a has all except 100, b has [0,4096)
         * a - b = [4096, 8192) = 4096 bits */
        uint64_t countResult = multiroarBitCount(result);
        if (countResult != 4096) {
            ERR("ANDNOT OVER_FULL - FULL_BITMAP: expected 4096, got %" PRIu64,
                countResult);
        }
    }

    TEST("Complex: mixed container types across multiple chunks") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();

        /* Chunk 0: a=sparse, b=bitmap */
        FILL_SPARSE(a, 0, 100);
        FILL_DENSE(b, 0, 0, 1000);

        /* Chunk 1: a=bitmap, b=sparse */
        FILL_DENSE(a, 1, 0, 2000);
        FILL_SPARSE(b, 1, 150);

        /* Chunk 2: a=all_ones, b=over_full */
        FILL_ALL_ONES(a, 2);
        FILL_ALL_ONES(b, 2);
        multiroarRemove(b, 2 * 8192 + 500);

        /* Chunk 3: a=empty, b=bitmap */
        FILL_DENSE(b, 3, 1000, 3000);

        /* Chunk 4: a=over_full, b=empty */
        FILL_ALL_ONES(a, 4);
        multiroarRemove(a, 4 * 8192 + 100);

        /* Test AND */
        cleaner(multiroarFree) multiroar *andResult = multiroarNewAnd(a, b);
        /* Chunk 0: sparse 100 positions at i*12, dense [0,1000)
         *          i*12 < 1000 for i < 83.33, so 84 positions match (i=0..83)
         */
        /* Chunk 1: dense [0,2000), sparse 150 positions at i*12
         *          i*12 < 2000 for i < 167, we have 150 positions, all match */
        /* Chunk 2: 8192 AND (8192-1) = 8191 */
        /* Chunk 3: empty AND bitmap = empty */
        /* Chunk 4: over_full AND empty = empty */
        uint64_t expectedAnd = 84 + 150 + 8191;

        if (multiroarBitCount(andResult) != expectedAnd) {
            ERR("Complex AND: expected %" PRIu64 ", got %" PRIu64, expectedAnd,
                multiroarBitCount(andResult));
        }
        /* Test OR */
        cleaner(multiroarFree) multiroar *orResult = multiroarNewOr(a, b);
        /* Chunk 0: 100 sparse + 1000 dense with overlap */
        /* Chunk 1: 2000 dense + 150 sparse with overlap */
        /* Chunk 2: all_ones OR (all-1) = all_ones = 8192 */
        /* Chunk 3: empty + 2000 = 2000 */
        /* Chunk 4: 8191 + 0 = 8191 */

        /* Chunk 0: positions 0,12,24,...1188 (100) union [0,1000)
         *          The sparse positions i*12 for i<84 are in [0,1000)
         *          So union is [0,1000) + positions 1008,1020,...,1188 = 16
         * more = 1000 + (100-84) = 1016 */
        /* Chunk 1: [0,2000) union positions i*12 for i in [0,150)
         *          i*12 < 2000 for i < 167, so all 150 in [0,2000)
         *          = 2000 (no additional) */
        /* So OR = 1016 + 2000 + 8192 + 2000 + 8191 = 21399 */
        /* Actually let me recalculate chunk 0:
         * sparse has i*12 for i in [0,100), so max is 99*12 = 1188
         * dense has [0,1000)
         * overlap: i*12 < 1000 for i < 83.33, so i <= 83 -> positions
         * 0,12,...996 union: 1000 + (100 - 84) = 1000 + 16 = 1016 */

        uint64_t expectedOr = 1016 + 2000 + 8192 + 2000 + 8191;
        if (multiroarBitCount(orResult) != expectedOr) {
            ERR("Complex OR: expected %" PRIu64 ", got %" PRIu64, expectedOr,
                multiroarBitCount(orResult));
        }
    }

    TEST("FUZZ: random cross-container operations") {
        uint64_t seed[2] = {0xDEADBEEF12345678ULL, 0x8765432100000001ULL};

        for (int trial = 0; trial < 20; trial++) {
            cleaner(multiroarFree) multiroar *a = multiroarBitNew();
            cleaner(multiroarFree) multiroar *b = multiroarBitNew();

            /* Randomly fill chunks with different densities */
            for (int chunk = 0; chunk < 4; chunk++) {
                uint64_t base = chunk * 8192ULL;
                uint64_t densityA = xoroshiro128plus(seed) % 100;
                uint64_t densityB = xoroshiro128plus(seed) % 100;

                /* 0-20%: empty, 20-40%: sparse, 40-80%: bitmap, 80-100%: dense
                 */
                if (densityA >= 20) {
                    int count = (densityA < 40)   ? 50 + (densityA % 100)
                                : (densityA < 80) ? 1000 + (densityA % 2000)
                                                  : 7500 + (densityA % 692);
                    for (int i = 0; i < count; i++) {
                        multiroarBitSet(a,
                                        base + (xoroshiro128plus(seed) % 8192));
                    }
                }
                if (densityB >= 20) {
                    int count = (densityB < 40)   ? 50 + (densityB % 100)
                                : (densityB < 80) ? 1000 + (densityB % 2000)
                                                  : 7500 + (densityB % 692);
                    for (int i = 0; i < count; i++) {
                        multiroarBitSet(b,
                                        base + (xoroshiro128plus(seed) % 8192));
                    }
                }
            }

            uint64_t countA = multiroarBitCount(a);
            uint64_t countB = multiroarBitCount(b);

            /* Test AND: result count <= min(countA, countB) */
            cleaner(multiroarFree) multiroar *andResult = multiroarNewAnd(a, b);
            uint64_t andCount = multiroarBitCount(andResult);
            uint64_t minAB = (countA < countB) ? countA : countB;
            if (andCount > minAB) {
                ERR("Trial %d: AND count %" PRIu64 " > min(%" PRIu64 ",%" PRIu64
                    ")",
                    trial, andCount, countA, countB);
            }

            /* Test OR: result count >= max(countA, countB) */
            cleaner(multiroarFree) multiroar *orResult = multiroarNewOr(a, b);
            uint64_t orCount = multiroarBitCount(orResult);
            uint64_t maxAB = (countA > countB) ? countA : countB;
            if (orCount < maxAB) {
                ERR("Trial %d: OR count %" PRIu64 " < max(%" PRIu64 ",%" PRIu64
                    ")",
                    trial, orCount, countA, countB);
            }

            /* Test XOR: result count should satisfy |countA - countB| <=
             * xorCount <= countA + countB */
            cleaner(multiroarFree) multiroar *xorResult = multiroarNewXor(a, b);
            uint64_t xorCount = multiroarBitCount(xorResult);
            uint64_t diffAB =
                (countA > countB) ? (countA - countB) : (countB - countA);
            if (xorCount < diffAB || xorCount > countA + countB) {
                ERR("Trial %d: XOR count %" PRIu64 " out of range [%" PRIu64
                    ", %" PRIu64 "]",
                    trial, xorCount, diffAB, countA + countB);
            }

            /* Verify AND + XOR = OR (set theory identity) */
            /* Actually: |A ∪ B| = |A| + |B| - |A ∩ B|
             * and: |A △ B| = |A| + |B| - 2|A ∩ B|
             * So: |A △ B| + |A ∩ B| = |A| + |B| - |A ∩ B|... no
             * |A △ B| = |A ∪ B| - |A ∩ B|
             * So: xorCount = orCount - andCount */
            if (xorCount != orCount - andCount) {
                ERR("Trial %d: set identity failed: XOR=%" PRIu64
                    " != OR=%" PRIu64 " - AND=%" PRIu64,
                    trial, xorCount, orCount, andCount);
            }
        }
    }

    /* ================================================================
     * Edge cases for the word-at-a-time bitmap helpers (find-first/last
     * via ctz/clz, rank via word popcount, subset via word compare, and
     * NewAndNot's compress path). Dense fills force FULL_BITMAP storage so
     * these exercise the expanded-bitmap word scans, including the chunk's
     * last bit (8191, the clz boundary) and word-boundary positions.
     * ================================================================ */
    TEST("Min/Max/Rank: word-scan boundaries (FULL_BITMAP)") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        FILL_DENSE(a, 0, 100, 2100); /* bits [100,2100): first set bit not 0 */
        multiroarBitSet(a, 8191);    /* last bit in the chunk (clz boundary) */

        uint64_t pos = 0;
        if (!multiroarMin(a, &pos) || pos != 100) {
            ERR("Min: expected 100, got %" PRIu64, pos);
        }
        if (!multiroarMax(a, &pos) || pos != 8191) {
            ERR("Max: expected 8191, got %" PRIu64, pos);
        }
        /* rank(p) = set bits in [0,p); bits = [100,2100) (2000) + {8191}. */
        const struct {
            uint64_t at;
            uint64_t want;
        } cases[] = {{100, 0},     {101, 1},     {164, 64}, /* crosses word 1 */
                     {2100, 2000}, {8191, 2000}, {8192, 2001}};
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            uint64_t got = multiroarRank(a, cases[i].at);
            if (got != cases[i].want) {
                ERR("Rank(%" PRIu64 "): expected %" PRIu64 ", got %" PRIu64,
                    cases[i].at, cases[i].want, got);
            }
        }
    }

    TEST("IsSubset: word-wise compare (FULL_BITMAP)") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();
        FILL_DENSE(b, 0, 0, 4000);    /* [0,4000) */
        FILL_DENSE(a, 0, 1000, 3000); /* [1000,3000) ⊆ b */
        if (!multiroarIsSubset(a, b)) {
            ERRR("IsSubset: [1000,3000) should be subset of [0,4000)");
        }
        multiroarBitSet(a, 5000); /* now a has a bit outside b */
        if (multiroarIsSubset(a, b)) {
            ERRR("IsSubset: should be false after adding bit 5000 not in b");
        }
        cleaner(multiroarFree) multiroar *empty = multiroarBitNew();
        if (!multiroarIsSubset(empty, b)) {
            ERRR("IsSubset: empty set must be a subset");
        }
    }

    TEST("ANDNOT: identical chunks compress to empty (popcount==0 path)") {
        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        cleaner(multiroarFree) multiroar *b = multiroarBitNew();
        FILL_DENSE(a, 0, 0, 3000);
        FILL_DENSE(b, 0, 0, 3000); /* a & ~b == 0 for the whole chunk */
        cleaner(multiroarFree) multiroar *result = multiroarNewAndNot(a, b);
        if (multiroarBitCount(result) != 0) {
            ERR("ANDNOT identical: expected empty, got %" PRIu64,
                multiroarBitCount(result));
        }
        uint64_t pos = 0;
        if (multiroarMin(result, &pos)) {
            ERRR("ANDNOT identical: empty result must have no min");
        }
    }

    TEST("Iterator: FULL_BITMAP exact ordered sequence (word-skip resume)") {
        /* Dense range forces FULL_BITMAP; bit 8191 sits past a long run of
         * zero words. Exercises the word-skip scan resuming mid-word across
         * every Next() call, word-boundary crossings, the zero-word skip, and
         * the final bit. Asserts completeness + strict ascending order. */
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
        FILL_DENSE(r, 0, 100, 2100); /* bits [100,2100) */
        multiroarBitSet(r, 8191);    /* last bit in the chunk */

        multiroarIterator it;
        multiroarIteratorInit(r, &it);
        uint64_t pos = 0;
        uint64_t prev = 0;
        uint64_t n = 0;
        bool first = true;
        while (multiroarIteratorNext(&it, &pos)) {
            if (first) {
                if (pos != 100) {
                    ERR("iter: first pos expected 100, got %" PRIu64, pos);
                }
                first = false;
            } else if (pos <= prev) {
                ERR("iter: not strictly ascending: %" PRIu64 " after %" PRIu64,
                    pos, prev);
            }
            prev = pos;
            n++;
        }
        if (n != 2001) {
            ERR("iter: expected 2001 positions, got %" PRIu64, n);
        }
        if (prev != 8191) {
            ERR("iter: last pos expected 8191, got %" PRIu64, prev);
        }
    }

    TEST("Iterator: OVER_FULL exact ordered sequence (cursor skip)") {
        /* A nearly-full chunk stores its UNSET positions (OVER_FULL). Removing
         * a scattered few from an all-ones chunk produces one. The iterator
         * must emit every set position ascending, skipping the unset ones via
         * the monotonic cursor — incl. unset bits at word boundaries (63/64)
         * and the last bit (8191). */
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
        FILL_ALL_ONES(r, 0);
        const uint64_t unset[] = {0, 63, 64, 100, 4000, 8191};
        const int U = (int)(sizeof(unset) / sizeof(unset[0]));
        for (int i = 0; i < U; i++) {
            multiroarRemove(r, unset[i]);
        }

        multiroarIterator it;
        multiroarIteratorInit(r, &it);
        uint64_t pos = 0;
        uint64_t prev = 0;
        uint64_t n = 0;
        bool first = true;
        while (multiroarIteratorNext(&it, &pos)) {
            if (!first && pos <= prev) {
                ERR("OVER_FULL iter: not ascending at %" PRIu64, pos);
            }
            for (int i = 0; i < U; i++) {
                if (pos == unset[i]) {
                    ERR("OVER_FULL iter: emitted unset position %" PRIu64, pos);
                }
            }
            prev = pos;
            first = false;
            n++;
        }
        if (n != (uint64_t)(BITMAP_SIZE_IN_BITS - U)) {
            ERR("OVER_FULL iter: expected %d positions, got %" PRIu64,
                BITMAP_SIZE_IN_BITS - U, n);
        }
    }

#undef FILL_SPARSE
#undef FILL_DENSE
#undef FILL_ALL_ONES

    printf("=== Cross-Container-Type Tests Passed! ===\n\n");

    TEST("BitSetMany/BitGetMany: batched equals per-bit across patterns") {
        /* The batched run path must produce byte-identical membership
         * to the per-bit path across every chunk representation it can
         * create or update (missing, packed, bitmap, negative, ALL_1)
         * and every input shape (sorted dense, sorted sparse, random,
         * short runs below the batch threshold). */
        struct {
            const char *name;
            uint64_t count;
            uint64_t (*gen)(uint64_t i);
        } patterns[] = {
            {"sorted dense", 20000, NULL},
            {"sorted sparse", 5000, NULL},
            {"random order", 8000, NULL},
            {"short runs", 300, NULL},
        };

        for (size_t p = 0; p < sizeof(patterns) / sizeof(patterns[0]); p++) {
            const uint64_t n = patterns[p].count;
            cleanerName(zfree, UInt64) uint64_t *positions =
                zmalloc(n * sizeof(uint64_t));

            for (uint64_t i = 0; i < n; i++) {
                switch (p) {
                case 0: /* dense: fills chunks fully -> bitmap/ALL_1 */
                    positions[i] = i;
                    break;
                case 1: /* sparse: a few bits per chunk -> packed */
                    positions[i] = i * 977;
                    break;
                case 2: /* deterministic pseudo-random order */
                    positions[i] = (i * 2654435761ULL) % 200000;
                    break;
                default: /* runs of 2 (below threshold) far apart */
                    positions[i] = (i / 2) * 50000 + (i % 2);
                    break;
                }
            }

            cleaner(multiroarFree) multiroar *batched = multiroarBitNew();
            cleaner(multiroarFree) multiroar *single = multiroarBitNew();

            multiroarBitSetMany(batched, positions, n);
            for (uint64_t i = 0; i < n; i++) {
                multiroarBitSet(single, positions[i]);
            }

            /* Membership must match everywhere set... */
            uint64_t mismatches = 0;
            cleanerName(zfree, Bools) bool *results = zmalloc(n * sizeof(bool));
            multiroarBitGetMany(batched, positions, n, results);
            for (uint64_t i = 0; i < n; i++) {
                if (!results[i] || !multiroarBitGet(single, positions[i])) {
                    mismatches++;
                }
            }
            /* ...and at probe points that were never set */
            for (uint64_t probe = 1; probe < 200000; probe += 7919) {
                if (multiroarBitGet(batched, probe) !=
                    multiroarBitGet(single, probe)) {
                    mismatches++;
                }
            }

            if (mismatches > 0) {
                ERR("pattern '%s': %" PRIu64 " membership mismatches",
                    patterns[p].name, mismatches);
            }
        }
    }

    TEST("PERF: BitSetMany batched vs per-bit loop") {
        const uint64_t n = 100000;
        cleanerName(zfree, UInt64) uint64_t *positions =
            zmalloc(n * sizeof(uint64_t));
        for (uint64_t i = 0; i < n; i++) {
            positions[i] = i * 3; /* sorted, ~3 bits per byte density */
        }

        cleaner(multiroarFree) multiroar *a = multiroarBitNew();
        uint64_t start = timeUtilMonotonicNs();
        for (uint64_t i = 0; i < n; i++) {
            multiroarBitSet(a, positions[i]);
        }
        const uint64_t perBitNs = timeUtilMonotonicNs() - start;

        cleaner(multiroarFree) multiroar *b = multiroarBitNew();
        start = timeUtilMonotonicNs();
        multiroarBitSetMany(b, positions, n);
        const uint64_t batchedNs = timeUtilMonotonicNs() - start;
        printf("    per-bit: %.1f ns/bit, batched: %.1f ns/bit (%.1fx)\n",
               (double)perBitNs / n, (double)batchedNs / n,
               (double)perBitNs / (double)batchedNs);
    }

    TEST("removing every set bit leaves an empty bitmap, in any chunk form") {
        /* Counts that make each chunk form (sparse list, bitmap, all-set,
         * near-full list) and cross the sparse count's encoding widths,
         * removed in both orders and from the middle out; after every
         * removal the remaining bits, the minimum and emptiness must
         * match an oracle. */
        const uint32_t counts[] = {1,    2,    240,  241,  300, 2287,
                                   2288, 4000, 8191, 8192, 9000};
        for (size_t c = 0; c < COUNT_ARRAY(counts); c++) {
            for (uint64_t stride = 1; stride <= 2; stride++) {
                for (int order = 0; order < 3; order++) {
                    const uint32_t n = counts[c];
                    /* Two sentinel bits in a later chunk, whose entry sits
                     * right after the tested chunk's: rewriting a chunk
                     * must never touch the bytes that follow it */
                    const uint64_t sentinel = (uint64_t)1 << 20;
                    cleaner(multiroarFree) multiroar *r = multiroarBitNew();
                    multiroarBitSet(r, sentinel);
                    multiroarBitSet(r, sentinel + 3);
                    for (uint32_t i = 0; i < n; i++) {
                        multiroarBitSet(r, (uint64_t)i * stride);
                    }

                    bool failed = false;
                    for (uint32_t k = 0; k < n && !failed; k++) {
                        const uint32_t i = order == 0   ? k
                                           : order == 1 ? n - 1 - k
                                                        : (k * 7919u) % n;
                        if (!multiroarRemove(r, (uint64_t)i * stride) ||
                            multiroarBitGet(r, (uint64_t)i * stride) ||
                            !multiroarBitGet(r, sentinel) ||
                            !multiroarBitGet(r, sentinel + 3) ||
                            multiroarBitCount(r) != n - 1 - k + 2) {
                            ERR("n %u stride %" PRIu64 " order %d: removing "
                                "%u of %u broke the bitmap",
                                n, stride, order, k, n);
                            failed = true;
                        }
                    }

                    uint64_t min = 0;
                    if (!failed &&
                        (!multiroarMin(r, &min) || min != sentinel ||
                         !multiroarRemove(r, sentinel) ||
                         !multiroarRemove(r, sentinel + 3) ||
                         !multiroarIsEmpty(r) || multiroarMin(r, &min))) {
                        ERR("n %u stride %" PRIu64 " order %d: no bits are "
                            "set but the bitmap is not empty",
                            n, stride, order);
                    }
                }
            }
        }
    }

    TEST("random bit, range and set operations agree with a bit oracle") {
        /* Four chunks of bits under sets, removes, range sets and
         * clears, and in-place AND / OR / XOR / AND-NOT with a second
         * random roar: chunks empty and refill in every form.  After each
         * step the count, min, max, emptiness, ranks, selects and range
         * counts agree with the oracle (min and max scan past chunks
         * other operations left empty). */
        enum { MR_BITS = 4 * BITMAP_SIZE_IN_BITS };
        static uint8_t oracle[MR_BITS];
        static uint8_t other[MR_BITS];
        memset(oracle, 0, sizeof(oracle));
        cleaner(multiroarFree) multiroar *r = multiroarBitNew();
        uint64_t rng = 0x5eedu;
        for (int step = 0; step < 300 && !err; step++) {
            const uint64_t op = splitmix64(&rng) % 10;
            const uint64_t at = splitmix64(&rng) % MR_BITS;
            const uint64_t span =
                1 + splitmix64(&rng) % (2 * BITMAP_SIZE_IN_BITS);
            const uint64_t end = at + span < MR_BITS ? at + span : MR_BITS;
            if (op < 2) {
                for (int i = 0; i < 40; i++) {
                    const uint64_t p = splitmix64(&rng) % MR_BITS;
                    multiroarBitSet(r, p);
                    oracle[p] = 1;
                }
            } else if (op == 2) {
                for (uint64_t p = at; p < end; p++) {
                    if (oracle[p] && splitmix64(&rng) % 3 == 0) {
                        multiroarRemove(r, p);
                        oracle[p] = 0;
                    }
                }
            } else if (op == 3) {
                multiroarBitSetRange(r, at, end - at);
                memset(oracle + at, 1, end - at);
            } else if (op == 4 || op == 5) {
                /* clear a span, or exactly one whole chunk */
                const uint64_t lo =
                    op == 4 ? at
                            : (at / BITMAP_SIZE_IN_BITS) * BITMAP_SIZE_IN_BITS;
                const uint64_t hi = op == 4 ? end : lo + BITMAP_SIZE_IN_BITS;
                multiroarBitClearRange(r, lo, hi - lo);
                memset(oracle + lo, 0, hi - lo);
            } else {
                /* a second roar: dense in one chunk, sparse elsewhere */
                cleaner(multiroarFree) multiroar *b = multiroarBitNew();
                memset(other, 0, sizeof(other));
                const uint64_t dense = splitmix64(&rng) % 4;
                for (uint64_t p = 0; p < MR_BITS; p++) {
                    const uint64_t odds =
                        p / BITMAP_SIZE_IN_BITS == dense ? 2 : 97;
                    if (splitmix64(&rng) % odds == 0) {
                        multiroarBitSet(b, p);
                        other[p] = 1;
                    }
                }
                for (uint64_t p = 0; p < MR_BITS; p++) {
                    oracle[p] = op == 6   ? oracle[p] & other[p]
                                : op == 7 ? oracle[p] | other[p]
                                : op == 8 ? oracle[p] ^ other[p]
                                          : oracle[p] & !other[p];
                }
                if (op == 6) {
                    multiroarAnd(r, b);
                } else if (op == 7) {
                    multiroarOr(r, b);
                } else if (op == 8) {
                    multiroarXor(r, b);
                } else {
                    multiroarAndNot(r, b);
                }
            }

            uint64_t count = 0;
            uint64_t lo = UINT64_MAX;
            uint64_t hi = 0;
            for (uint64_t p = 0; p < MR_BITS; p++) {
                if (oracle[p]) {
                    count++;
                    lo = lo == UINT64_MAX ? p : lo;
                    hi = p;
                }
            }
            uint64_t min = 0;
            uint64_t max = 0;
            const bool hasMin = multiroarMin(r, &min);
            const bool hasMax = multiroarMax(r, &max);
            if (multiroarBitCount(r) != count ||
                multiroarIsEmpty(r) != (count == 0) || hasMin != (count > 0) ||
                hasMax != (count > 0) || (count && (min != lo || max != hi))) {
                ERR("step %d (op %" PRIu64 "): count %" PRIu64 "/%" PRIu64
                    ", min %d %" PRIu64 "/%" PRIu64 ", max %d %" PRIu64
                    "/%" PRIu64,
                    step, op, multiroarBitCount(r), count, hasMin, min, lo,
                    hasMax, max, hi);
                break;
            }
            for (int k = 0; k < 4; k++) {
                const uint64_t p = splitmix64(&rng) % (MR_BITS + 1);
                const uint64_t q = p + splitmix64(&rng) % 9000;
                uint64_t below = 0;
                uint64_t within = 0;
                uint64_t first = UINT64_MAX; /* first set bit at or after p */
                for (uint64_t i = 0; i < MR_BITS; i++) {
                    below += i < p && oracle[i];
                    within += i >= p && i < q && oracle[i];
                    if (i >= p && oracle[i] && first == UINT64_MAX) {
                        first = i;
                    }
                }
                uint64_t sel = 0;
                const bool selected = multiroarSelect(r, below + 1, &sel);
                if (multiroarRank(r, p) != below ||
                    multiroarRangeCount(r, p, q) != within ||
                    selected != (below < count) || (selected && sel != first)) {
                    ERR("step %d: rank / range / select at %" PRIu64, step, p);
                    break;
                }
            }
        }
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
