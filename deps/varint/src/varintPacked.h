#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef PACKED_CAT
#define PACKED_CAT(A, B) A##B
#define PACKED_NAME(A, B) PACKED_CAT(A, B)
#endif

/* We give you a 12 bit reader/writer if you don't request otherwise. */
#ifndef PACK_STORAGE_BITS
#define PACK_STORAGE_BITS 12
#endif

#if PACK_STORAGE_BITS > 64
#error "Can't pack larger than 64 bits."
#endif

#if (PACK_STORAGE_BITS == 8) || (PACK_STORAGE_BITS == 16) ||                   \
    (PACK_STORAGE_BITS == 32) || (PACK_STORAGE_BITS == 64)
#warning "Why pack system-level widths?  Native arrays would be faster."
#endif

/* Faster: use 32 bits to pack 12 bit integers.
 * Slower: use 16 bits to pack 12 bit integers.
 * (e.g. storing one 12 bit value in 32 bits requires a 4 byte allocation.
 *       storing one 12 bit value in 16 bits requires a 2 byte allocation.
 *  The "waste overhead" only matters if you think you may have unpredictable
 *  storage patterns where sometimes you store 1,000 values and other times
 *  you store one value.)
 * Obviously, your SLOT_STORAGE_TYPE can't contain fewer bytes than your
 * STORAGE_BITS (e.g. STORAGE_BITS of 17 *must* use uint32_t) */

/* If user didn't request explicit slot storage, provide defaults. */
#ifndef PACK_STORAGE_SLOT_STORAGE_TYPE
/* If user requests smallest possible storage, give small storage. */
#ifdef PACK_STORAGE_COMPACT
#if PACK_STORAGE_BITS <= 16
#define PACK_STORAGE_SLOT_STORAGE_TYPE uint8_t
#elif PACK_STORAGE_BITS <= 32
#define PACK_STORAGE_SLOT_STORAGE_TYPE uint16_t
#elif PACK_STORAGE_BITS <= 64
#define PACK_STORAGE_SLOT_STORAGE_TYPE uint32_t
#endif
/* else, give default 4 byte storage.
 * It's the fastest, but may waste a little overhead. */
#else
#define PACK_STORAGE_SLOT_STORAGE_TYPE uint32_t
#endif
#endif

/* Set our offset/length storage type based on our known max number
 * of elements.  If nothing provided, default to uint32_t length storage. */
#ifdef PACK_MAX_ELEMENTS
#if PACK_MAX_ELEMENTS <= UINT8_MAX
#define PACKED_LEN_TYPE uint8_t
#elif PACK_MAX_ELEMENTS <= UINT16_MAX
#define PACKED_LEN_TYPE uint16_t
#elif PACK_MAX_ELEMENTS <= UINT32_MAX
#define PACKED_LEN_TYPE uint32_t
#elif PACK_MAX_ELEMENTS <= UINT64_MAX
#define PACKED_LEN_TYPE uint64_t
#endif
#else
#define PACKED_LEN_TYPE uint32_t
#endif

/* Automatically create our return type based on the size of bits. */
/* e.g. <= 8 bits gets returned as uint8_t
 *      <= 16 bits gets returned as uint16_t
 *      <= 32 bits gets returned as uint32_t.
 *      <= 64 bits gets returned as uint64_t. */

/* Automatically create value type based on storage bit width. */
#ifndef PACK_STORAGE_VALUE_TYPE
#if PACK_STORAGE_BITS <= 8
#define PACK_STORAGE_VALUE_TYPE uint8_t
#elif PACK_STORAGE_BITS <= 16
#define PACK_STORAGE_VALUE_TYPE uint16_t
#elif PACK_STORAGE_BITS <= 32
#define PACK_STORAGE_VALUE_TYPE uint32_t
#elif PACK_STORAGE_BITS <= 64
#define PACK_STORAGE_VALUE_TYPE uint64_t
#endif
#endif

/* Users can override the function prefix if they need the same
 * pack bit widths but with different implementation details
 * (e.g. use multiple slot storage widths at once, etc). */
#ifndef PACK_FUNCTION_PREFIX
#ifdef PACK_STORAGE_COMPACT
#define PACK_FUNCTION_PREFIX varintPackedCompact
#else
#define PACK_FUNCTION_PREFIX varintPacked
#endif
#endif

#ifndef PACKED_STATIC
#ifdef PACK_STATIC
#define PACKED_STATIC static
#else
#define PACKED_STATIC
#endif
#endif

/* Create custom bit width function names for getting and setting.
 * e.g. varintPack12Set, varintPack12Get */
#define PACKED_ARRAY_SET                                                       \
    PACKED_NAME(PACKED_NAME(PACK_FUNCTION_PREFIX, PACK_STORAGE_BITS), Set)
#define PACKED_ARRAY_SET_HALF                                                  \
    PACKED_NAME(PACKED_NAME(PACK_FUNCTION_PREFIX, PACK_STORAGE_BITS), SetHalf)
#define PACKED_ARRAY_SET_INCR                                                  \
    PACKED_NAME(PACKED_NAME(PACK_FUNCTION_PREFIX, PACK_STORAGE_BITS), SetIncr)
#define PACKED_ARRAY_GET                                                       \
    PACKED_NAME(PACKED_NAME(PACK_FUNCTION_PREFIX, PACK_STORAGE_BITS), Get)
#define PACKED_ARRAY_INSERT                                                    \
    PACKED_NAME(PACKED_NAME(PACK_FUNCTION_PREFIX, PACK_STORAGE_BITS), Insert)
#define PACKED_ARRAY_INSERT_SORTED                                             \
    PACKED_NAME(PACKED_NAME(PACK_FUNCTION_PREFIX, PACK_STORAGE_BITS),          \
                InsertSorted)
#define PACKED_ARRAY_DELETE                                                    \
    PACKED_NAME(PACKED_NAME(PACK_FUNCTION_PREFIX, PACK_STORAGE_BITS), Delete)
#define PACKED_ARRAY_DELETE_MEMBER                                             \
    PACKED_NAME(PACKED_NAME(PACK_FUNCTION_PREFIX, PACK_STORAGE_BITS),          \
                DeleteMember)
#define PACKED_ARRAY_MEMBER                                                    \
    PACKED_NAME(PACKED_NAME(PACK_FUNCTION_PREFIX, PACK_STORAGE_BITS), Member)
#define PACKED_ARRAY_BINARY_SEARCH                                             \
    PACKED_NAME(PACKED_NAME(PACK_FUNCTION_PREFIX, PACK_STORAGE_BITS),          \
                BinarySearch)

#define PACKED_ARRAY_COUNT_FROM_STORAGE_BYTES                                  \
    PACKED_NAME(PACKED_NAME(PACK_FUNCTION_PREFIX, PACK_STORAGE_BITS),          \
                CountFromStorageBytes)
#define PACKED_ARRAY_MEMBER_BYTES                                              \
    PACKED_NAME(PACKED_NAME(PACK_FUNCTION_PREFIX, PACK_STORAGE_BITS),          \
                MemberBytes)
#define PACKED_ARRAY_INSERT_BYTES                                              \
    PACKED_NAME(PACKED_NAME(PACK_FUNCTION_PREFIX, PACK_STORAGE_BITS),          \
                InsertBytes)
#define PACKED_ARRAY_INSERT_SORTED_BYTES                                       \
    PACKED_NAME(PACKED_NAME(PACK_FUNCTION_PREFIX, PACK_STORAGE_BITS),          \
                InsertSortedBytes)
#define PACKED_ARRAY_DELETE_BYTES                                              \
    PACKED_NAME(PACKED_NAME(PACK_FUNCTION_PREFIX, PACK_STORAGE_BITS),          \
                DeleteBytes)
#define PACKED_ARRAY_DELETE_MEMBER_BYTES                                       \
    PACKED_NAME(PACKED_NAME(PACK_FUNCTION_PREFIX, PACK_STORAGE_BITS),          \
                DeleteMemberBytes)

/* Populate internal macros from user macros */
#define BITS_PER_VALUE PACK_STORAGE_BITS
#define SLOT_STORAGE_TYPE PACK_STORAGE_SLOT_STORAGE_TYPE
#define VALUE_TYPE PACK_STORAGE_VALUE_TYPE

#ifdef PACK_STORAGE_MICRO_PROMOTION_TYPE
#define MICRO_PROMOTION_TYPE PACK_STORAGE_MICRO_PROMOTION_TYPE
#define MICRO_PROMOTION_TYPE_CAST(thing)                                       \
    ((PACK_STORAGE_MICRO_PROMOTION_TYPE)(thing))
#else
#define MICRO_PROMOTION_TYPE VALUE_TYPE
#define MICRO_PROMOTION_TYPE_CAST(thing) (thing)
#endif

/* Shared defines */
#define BITS_PER_SLOT (sizeof(SLOT_STORAGE_TYPE) * 8)
#define VALUE_MASK (MICRO_PROMOTION_TYPE)((1ULL << BITS_PER_VALUE) - 1)

/* This define is an optimization.  If we are using compact storage
 * (example: storing 12 bit packed across slots of uint8_t), then we
 * know up front we can *never* store a packed value inside just one
 * slot.  If we let the compiler know this too, it can optimize away
 * the "if (value in one slot)" branch and just use always-two-slot
 * reading/writing. */
#ifndef PACK_STORAGE_COMPACT
#define SLOT_CAN_HOLD_ENTIRE_VALUE 1
#endif
/* We can't define HOLD_ENTIRE_VALUE as below because BITS_PER_SLOT has sizeof()
 * the preprocessor doesn't know about.  We don't want to manually define bit
 * widths per type, so we juse use the STORAGE_COMPACT setting to determine
 * if we are using sub-slot-widths or not.
 * #define SLOT_CAN_HOLD_ENTIRE_VALUE (BITS_PER_VALUE <= BITS_PER_SLOT) */

/* Math helpers */
#define startOffset(offset) ((uint64_t)(offset)*BITS_PER_VALUE)

PACKED_STATIC void PACKED_ARRAY_SET(void *_dst, const PACKED_LEN_TYPE offset,
                                    const VALUE_TYPE val) {
    SLOT_STORAGE_TYPE *restrict out;
    uint32_t startBit;
    uint32_t bitsAvailable;
    const uint64_t startBitOffset = startOffset(offset);

    SLOT_STORAGE_TYPE *dst = (SLOT_STORAGE_TYPE *)_dst;

    out = &dst[startBitOffset / BITS_PER_SLOT];
    startBit = startBitOffset % BITS_PER_SLOT;

    bitsAvailable = BITS_PER_SLOT - startBit;

    assert(0 == (~VALUE_MASK & val));

#if SLOT_CAN_HOLD_ENTIRE_VALUE
    if (BITS_PER_VALUE <= bitsAvailable) {
        /* target position is fully inside out[0] */
        /* We set bits starting at position zero, so values are stored
         * from RIGHT to LEFT.
         * Example: storing one 12-bit value of 4095 at packed array position 0
         * backed by a 32 bit (4 byte) slot looks like:
         * [00000000000000000000111111111111]
         * Storing the same 4095 in position 0 backed by a 64 bit slot is:
         * [0000000000000000000000000000000000000000000000000000111111111111]
         */
        out[0] = (out[0] & ~(VALUE_MASK << startBit)) |
                 (MICRO_PROMOTION_TYPE_CAST(val) << startBit);
    } else {
#endif
        /* target position is split across two slots */
        MICRO_PROMOTION_TYPE low, high;

        /* Because our packed arrays store values from RIGHT to LEFT,
         * setting across slots may seem backwards, but it works.
         * Example: if we're storing value 3048 split across two 8 bit slots
         * at array position 0, we need split 12 bits across a byte boundary.
         * We end up with:
         * low = 3048 << 0 = [0000101111101000]
         * high = 4095 >> (8 - 0) = [0000000000001011]
         *
         * Then, we make:
         * out[0] = (save existing bits (in this case, none since we are
         *           setting the whole width)) | [11101000]
         * out[1] = (save existing bits) | [00001011]
         *
         * Remember, in this case, 'out' is an array of single bytes
         * so our intermediate values get truncated from 16 bit calculation
         * values down to 8 bit storage values we already shifted properly.
         *
         * Now,
         * out[0] = [11101000]
         * out[1] = [XXXX1011]
         * (where XXXX are bits we saved and didn't touch because they could
         * belong to another packed integer).
         *
         * So, linear in-memory byte storage looks like:
         * [11101000][XXXX1011]
         * even though the *actual value* is 12 bits in this order:
         * [101111101000]
         * we store the 'top bits' *after* the 'low bits' unlike may
         * be expected.  If you're debugging a raw byte array of packed
         * integers, remember to not read the bit values in the bytes
         * as if they should just be concatenated together. */
        low = MICRO_PROMOTION_TYPE_CAST(val) << startBit;
        high = MICRO_PROMOTION_TYPE_CAST(val) >> bitsAvailable;

        out[0] = (out[0] & ~(VALUE_MASK << startBit)) | low;
        out[1] = (out[1] & ~(VALUE_MASK >> bitsAvailable)) | high;
#if SLOT_CAN_HOLD_ENTIRE_VALUE
    }
#endif
}

PACKED_STATIC void PACKED_ARRAY_SET_HALF(void *_dst,
                                         const PACKED_LEN_TYPE offset) {
    SLOT_STORAGE_TYPE *restrict out;
    uint32_t startBit;
    uint32_t bitsAvailable;
    const uint64_t startBitOffset = startOffset(offset);

    SLOT_STORAGE_TYPE *dst = (SLOT_STORAGE_TYPE *)_dst;

    out = &dst[startBitOffset / BITS_PER_SLOT];
    startBit = startBitOffset % BITS_PER_SLOT;

    bitsAvailable = BITS_PER_SLOT - startBit;

#if SLOT_CAN_HOLD_ENTIRE_VALUE
    if (BITS_PER_VALUE <= bitsAvailable) {
        /* target position is fully inside out[0] */
        const VALUE_TYPE current =
            (MICRO_PROMOTION_TYPE_CAST(out[0]) >> startBit) & VALUE_MASK;

        if (!current) {
            /* No sense trying to divide and set nothing */
            return;
        }

        const VALUE_TYPE val = current / 2;
        out[0] = (out[0] & ~(VALUE_MASK << startBit)) |
                 (MICRO_PROMOTION_TYPE_CAST(val) << startBit);
    } else {
#endif
        /* target position is split across two slots */
        MICRO_PROMOTION_TYPE low, high;

        /* GET */
        low = MICRO_PROMOTION_TYPE_CAST(out[0]) >> startBit;
        high = MICRO_PROMOTION_TYPE_CAST(out[1]) << bitsAvailable;

        const VALUE_TYPE current =
            low | (high & ((VALUE_MASK >> bitsAvailable) << bitsAvailable));

        if (!current) {
            /* No sense trying to divide and set nothing */
            return;
        }

        const VALUE_TYPE val = current / 2;

        /* SET */
        low = MICRO_PROMOTION_TYPE_CAST(val) << startBit;
        high = MICRO_PROMOTION_TYPE_CAST(val) >> bitsAvailable;

        out[0] = (out[0] & ~(VALUE_MASK << startBit)) | low;
        out[1] = (out[1] & ~(VALUE_MASK >> bitsAvailable)) | high;
#if SLOT_CAN_HOLD_ENTIRE_VALUE
    }
#endif
}

PACKED_STATIC void PACKED_ARRAY_SET_INCR(void *_dst,
                                         const PACKED_LEN_TYPE offset,
                                         const int64_t incrBy) {
    SLOT_STORAGE_TYPE *restrict out;
    uint32_t startBit;
    uint32_t bitsAvailable;
    const uint64_t startBitOffset = startOffset(offset);

    SLOT_STORAGE_TYPE *dst = (SLOT_STORAGE_TYPE *)_dst;

    out = &dst[startBitOffset / BITS_PER_SLOT];
    startBit = startBitOffset % BITS_PER_SLOT;

    bitsAvailable = BITS_PER_SLOT - startBit;

#if SLOT_CAN_HOLD_ENTIRE_VALUE
    if (BITS_PER_VALUE <= bitsAvailable) {
        /* target position is fully inside out[0] */
        const VALUE_TYPE current =
            (MICRO_PROMOTION_TYPE_CAST(out[0]) >> startBit) & VALUE_MASK;
        VALUE_TYPE val = current + incrBy;
        val = val >= (1 << BITS_PER_VALUE) ? current - incrBy : val;
        out[0] = (out[0] & ~(VALUE_MASK << startBit)) |
                 (MICRO_PROMOTION_TYPE_CAST(val) << startBit);
    } else {
#endif
        /* target position is split across two slots */
        MICRO_PROMOTION_TYPE low, high;

        /* GET */
        low = MICRO_PROMOTION_TYPE_CAST(out[0]) >> startBit;
        high = MICRO_PROMOTION_TYPE_CAST(out[1]) << bitsAvailable;

        const VALUE_TYPE current =
            low | (high & ((VALUE_MASK >> bitsAvailable) << bitsAvailable));
        VALUE_TYPE val = current + incrBy;
        val = val >= (1 << BITS_PER_VALUE) ? current - incrBy : val;

        /* SET */
        low = MICRO_PROMOTION_TYPE_CAST(val) << startBit;
        high = MICRO_PROMOTION_TYPE_CAST(val) >> bitsAvailable;

        out[0] = (out[0] & ~(VALUE_MASK << startBit)) | low;
        out[1] = (out[1] & ~(VALUE_MASK >> bitsAvailable)) | high;
#if SLOT_CAN_HOLD_ENTIRE_VALUE
    }
#endif
}

PACKED_STATIC VALUE_TYPE PACKED_ARRAY_GET(const void *src_,
                                          const PACKED_LEN_TYPE offset) {
    const SLOT_STORAGE_TYPE *restrict in = NULL;
    uint32_t startBit = 0;
    uint32_t bitsAvailable = 0;
    VALUE_TYPE out = 0;
    const uint64_t startBitOffset = startOffset(offset);

    SLOT_STORAGE_TYPE *src = (SLOT_STORAGE_TYPE *)src_;

    in = &src[startBitOffset / BITS_PER_SLOT];
    startBit = startBitOffset % BITS_PER_SLOT;

    bitsAvailable = BITS_PER_SLOT - startBit;

#if SLOT_CAN_HOLD_ENTIRE_VALUE
    if (BITS_PER_VALUE <= bitsAvailable) {
        /* stored value is fully contained inside in[0] */
        /* If value is entirely in one slot, we just need to shift down
         * the packed integer then mask away other values. */
        out = (MICRO_PROMOTION_TYPE_CAST(in[0]) >> startBit) & VALUE_MASK;
    } else {
#endif
        /* stored value is split across two slots */
        MICRO_PROMOTION_TYPE low, high;

        /* Restore from two slots by moving in[0] bits down and
         * in[1] bits up */
        low = MICRO_PROMOTION_TYPE_CAST(in[0]) >> startBit;
        high = MICRO_PROMOTION_TYPE_CAST(in[1]) << bitsAvailable;

        /* Re-create the packed integer by combining the shifted
         * down 'low' bits and mask away bits in 'high' not part of
         * this packed integer. */
        out = low | (high & ((VALUE_MASK >> bitsAvailable) << bitsAvailable));
#if SLOT_CAN_HOLD_ENTIRE_VALUE
    }
#endif

    return out;
}

static inline PACKED_LEN_TYPE
PACKED_ARRAY_BINARY_SEARCH(const void *src_, const PACKED_LEN_TYPE len,
                           const VALUE_TYPE val) {
    PACKED_LEN_TYPE min = 0;
    PACKED_LEN_TYPE max = len;

    /* Note: we run binary search until we find the absolute min position
     * for 'val' (even with duplicates).
     * Not terminating early means we have one less branch in each iteration
     * and since we are usually searching L1-cache-sized arrays, the fewer
     * branches improves performance more than stopping at the first match
     * (if duplicates exist). */
    while (min < max) {
        const PACKED_LEN_TYPE mid = (min + max) >> 1;
        if (PACKED_ARRAY_GET(src_, mid) < val) {
            min = mid + 1;
        } else {
            max = mid;
        }
    }

    return min;
}

static size_t PACKED_ARRAY_COUNT_FROM_STORAGE_BYTES(size_t bytes) {
    return (bytes * 8) / PACK_STORAGE_BITS;
}

/* If found, returns offset position of element.
 * If not found, returns -1. */
PACKED_STATIC int64_t PACKED_ARRAY_MEMBER(const void *src_,
                                          const PACKED_LEN_TYPE len,
                                          const VALUE_TYPE val) {
    PACKED_LEN_TYPE min = PACKED_ARRAY_BINARY_SEARCH(src_, len, val);

    /* Check bounds before accessing - binary search may return 'len' if
     * element should be inserted past the end of the array */
    if (min < len && PACKED_ARRAY_GET(src_, min) == val) {
        return min;
    }

    return -1;
}

PACKED_STATIC int64_t PACKED_ARRAY_MEMBER_BYTES(const void *src_,
                                                const size_t bytes,
                                                const VALUE_TYPE val) {
    return PACKED_ARRAY_MEMBER(
        src_, PACKED_ARRAY_COUNT_FROM_STORAGE_BYTES(bytes), val);
}

PACKED_STATIC void PACKED_ARRAY_INSERT(void *_dst, const PACKED_LEN_TYPE len,
                                       const PACKED_LEN_TYPE offset,
                                       const VALUE_TYPE val) {
    /* Move all values from 'len' to 'offset' up one position so we can
     * insert the new value at 'offset' */
    /* TODO: instead, do this by shfiting bits across the array up one slot
     * position. */
    for (PACKED_LEN_TYPE i = len; i > offset; i--) {
        /* Move all entries up exactly one slot so we can
         * write to 'offset' without overwriting any entries. */
        PACKED_ARRAY_SET(_dst, i, PACKED_ARRAY_GET(_dst, i - 1));
    }

    PACKED_ARRAY_SET(_dst, offset, val);
}

PACKED_STATIC void PACKED_ARRAY_INSERT_BYTES(void *_dst, const size_t bytes,
                                             const PACKED_LEN_TYPE offset,
                                             const VALUE_TYPE val) {
    PACKED_ARRAY_INSERT(_dst, PACKED_ARRAY_COUNT_FROM_STORAGE_BYTES(bytes),
                        offset, val);
}

PACKED_STATIC void PACKED_ARRAY_INSERT_SORTED(void *_dst,
                                              const PACKED_LEN_TYPE len,
                                              const VALUE_TYPE val) {
    /* binary search for position to insert 'val' so we remain sorted. */
    PACKED_LEN_TYPE min = PACKED_ARRAY_BINARY_SEARCH(_dst, len, val);

    PACKED_ARRAY_INSERT(_dst, len, min, val);
}

PACKED_STATIC void PACKED_ARRAY_INSERT_SORTED_BYTES(void *_dst,
                                                    const size_t bytes,
                                                    const VALUE_TYPE val) {
    PACKED_ARRAY_INSERT_SORTED(
        _dst, PACKED_ARRAY_COUNT_FROM_STORAGE_BYTES(bytes), val);
}

PACKED_STATIC void PACKED_ARRAY_DELETE(void *_dst, const PACKED_LEN_TYPE len,
                                       const PACKED_LEN_TYPE offset) {
    /* Move all values above 'offset' down one position .*/
    /* todo: do this by shfiting bits across the array down one slot
     * position. */
    for (PACKED_LEN_TYPE i = offset; i < len - 1; i++) {
        PACKED_ARRAY_SET(_dst, i, PACKED_ARRAY_GET(_dst, i + 1));
    }
}

PACKED_STATIC void PACKED_ARRAY_DELETE_BYTES(void *_dst, const size_t bytes,
                                             const PACKED_LEN_TYPE offset) {
    PACKED_ARRAY_DELETE(_dst, PACKED_ARRAY_COUNT_FROM_STORAGE_BYTES(bytes),
                        offset);
}

/* Returns 'true' if member found and deleted.
 * Returns 'false' if no member found to delete. */
PACKED_STATIC bool PACKED_ARRAY_DELETE_MEMBER(void *_dst,
                                              const PACKED_LEN_TYPE len,
                                              const VALUE_TYPE member) {
    /* If 'member' exists in packed array, delete and return true.
     * else, return false. */

    int64_t memberOffset = PACKED_ARRAY_MEMBER(_dst, len, member);
    if (memberOffset >= 0) {
        PACKED_ARRAY_DELETE(_dst, len, memberOffset);
        return true;
    }

    return false;
}

PACKED_STATIC bool PACKED_ARRAY_DELETE_MEMBER_BYTES(void *_dst,
                                                    const size_t bytes,
                                                    const VALUE_TYPE member) {
    return PACKED_ARRAY_DELETE_MEMBER(
        _dst, PACKED_ARRAY_COUNT_FROM_STORAGE_BYTES(bytes), member);
}

#undef PACKED_ARRAY_COUNT_FROM_STORAGE_BYTES
#undef PACKED_ARRAY_MEMBER_BYTES
#undef PACKED_ARRAY_INSERT_BYTES
#undef PACKED_ARRAY_INSERT_SORTED_BYTES
#undef PACKED_ARRAY_DELETE_BYTES
#undef PACKED_ARRAY_DELETE_MEMBER_BYTES

#undef PACK_STORAGE_BITS
#undef PACK_STORAGE_SLOT_STORAGE_TYPE
#undef PACK_STORAGE_COMPACT
#undef PACK_STORAGE_VALUE_TYPE
#undef PACK_FUNCTION_PREFIX
#undef PACK_STORAGE_MICRO_PROMOTION_TYPE
#undef PACK_MAX_ELEMENTS
#undef PACKED_LEN_TYPE
#undef MICRO_PROMOTION_TYPE
#undef MICRO_PROMOTION_TYPE_CAST
#undef BITS_PER_VALUE
#undef BITS_PER_SLOT
#undef VALUE_MASK
#undef PACKED_ARRAY_SET
#undef PACKED_ARRAY_GET
#undef starOffset
#undef SLOT_CAN_HOLD_ENTIRE_VALUE

#undef PACKED_STATIC
#undef PACK_STATIC

#undef PACKED_ARRAY_SET
#undef PACKED_ARRAY_SET_HALF
#undef PACKED_ARRAY_SET_INCR
#undef PACKED_ARRAY_GET
#undef PACKED_ARRAY_INSERT
#undef PACKED_ARRAY_INSERT_SORTED
#undef PACKED_ARRAY_DELETE
#undef PACKED_ARRAY_DELETE_MEMBER
#undef PACKED_ARRAY_MEMBER
#undef PACKED_ARRAY_BINARY_SEARCH
