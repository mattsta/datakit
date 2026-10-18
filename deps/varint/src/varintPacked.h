#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef PACKED_CAT
#define PACKED_CAT(A, B) A##B
#define PACKED_NAME(A, B) PACKED_CAT(A, B)
#endif

/* We give you a 12 bit reader/writer if you don't request otherwise. */
#ifndef PACK_STORAGE_BITS
#define PACK_STORAGE_BITS 12
#endif

#if PACK_STORAGE_BITS > 64 || PACK_STORAGE_BITS < 1
#error "PACK_STORAGE_BITS must be 1 to 64."
#endif

#if (PACK_STORAGE_BITS == 8) || (PACK_STORAGE_BITS == 16) ||                   \
    (PACK_STORAGE_BITS == 32) || (PACK_STORAGE_BITS == 64)
/* Note: Packing system-level widths (8, 16, 32, 64) is inefficient.
 * Native arrays would be faster for these standard widths. */
#endif

/* Faster: use 32 bits to pack 12 bit integers.
 * Slower: use 16 bits to pack 12 bit integers.
 * (e.g. storing one 12 bit value in 32 bits requires a 4 byte allocation.
 *       storing one 12 bit value in 16 bits requires a 2 byte allocation.
 *  The "waste overhead" only matters if you think you may have unpredictable
 *  storage patterns where sometimes you store 1,000 values and other times
 *  you store one value.)
 * Every width from 1 to 64 bits works with every slot type (uint8_t,
 * uint16_t, uint32_t, uint64_t): a value may span as many slots as its width
 * needs, e.g. 61 bits in uint8_t slots touch up to nine. VALUE_TYPE must be
 * wide enough for the width (the default is).
 *
 * Set reads and rewrites whole slots, keeping the bits beside the value, so
 * the storage must be initialized (e.g. from calloc) before the first Set. */

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
/* All BITS_PER_VALUE bits set; a 64-bit constant so every width up to 64
 * keeps its high bits */
#define VALUE_MASK (UINT64_MAX >> (64 - BITS_PER_VALUE))

/* These are compile-time constants (BITS_PER_SLOT uses sizeof, so they are
 * C expressions, not preprocessor ones); every branch on them folds away.
 *
 * Values start at multiples of BITS_PER_VALUE, so within a slot (a power of
 * two wide) they start at multiples of PACKED_START_STEP, the largest power
 * of two dividing BITS_PER_VALUE, capped at the slot width. The latest
 * start in a slot is BITS_PER_SLOT - PACKED_START_STEP, which bounds how
 * many slots one value can touch. */
#define PACKED_LOWEST_SET_BIT ((BITS_PER_VALUE) & -(BITS_PER_VALUE))
#define PACKED_START_STEP                                                      \
    (PACKED_LOWEST_SET_BIT < BITS_PER_SLOT ? PACKED_LOWEST_SET_BIT             \
                                           : BITS_PER_SLOT)
#define PACKED_MAX_SLOTS                                                       \
    ((BITS_PER_SLOT - PACKED_START_STEP + BITS_PER_VALUE + BITS_PER_SLOT -     \
      1) /                                                                     \
     BITS_PER_SLOT)

/* A value can sit entirely inside one slot only if it is no wider than one;
 * with wider values (e.g. 12 bits in uint8_t slots under
 * PACK_STORAGE_COMPACT) the single-slot branch folds away. */
#define SLOT_CAN_HOLD_ENTIRE_VALUE (BITS_PER_VALUE <= BITS_PER_SLOT)

/* Math helpers */
#define startOffset(offset) ((uint64_t)(offset) * BITS_PER_VALUE)

#define PACKED_SLOT                                                            \
    PACKED_NAME(PACKED_NAME(PACK_FUNCTION_PREFIX, PACK_STORAGE_BITS), Slot)
#define PACKED_SLOT_LOAD                                                       \
    PACKED_NAME(PACKED_NAME(PACK_FUNCTION_PREFIX, PACK_STORAGE_BITS), SlotLoad)
#define PACKED_SLOT_STORE                                                      \
    PACKED_NAME(PACKED_NAME(PACK_FUNCTION_PREFIX, PACK_STORAGE_BITS), SlotStore)
#define PACKED_SATURATING_ADD                                                  \
    PACKED_NAME(PACKED_NAME(PACK_FUNCTION_PREFIX, PACK_STORAGE_BITS),          \
                SaturatingAdd)
#define PACKED_WIDE_READ                                                       \
    PACKED_NAME(PACKED_NAME(PACK_FUNCTION_PREFIX, PACK_STORAGE_BITS), WideRead)
#define PACKED_WIDE_WRITE                                                      \
    PACKED_NAME(PACKED_NAME(PACK_FUNCTION_PREFIX, PACK_STORAGE_BITS), WideWrite)

/* Packed storage may start at any address (e.g. inside a byte stream), so
 * storage is addressed through a slot type with alignment 1 and every slot
 * is read and written through memcpy. Neither assumes the natural alignment
 * of SLOT_STORAGE_TYPE, and a fixed-size memcpy still compiles to a single
 * load or store with the same address arithmetic as a plain array access. */
typedef SLOT_STORAGE_TYPE PACKED_SLOT __attribute__((aligned(1)));

static inline SLOT_STORAGE_TYPE PACKED_SLOT_LOAD(const PACKED_SLOT *src) {
    SLOT_STORAGE_TYPE slot;
    memcpy(&slot, src, sizeof(slot));
    return slot;
}

static inline void PACKED_SLOT_STORE(PACKED_SLOT *dst,
                                     const SLOT_STORAGE_TYPE slot) {
    memcpy(dst, &slot, sizeof(slot));
}

/* Values that can touch three or more slots (PACKED_MAX_SLOTS > 2, e.g. 13
 * bits in uint8_t slots or 63 bits in uint32_t slots) go through these two
 * helpers, which walk exactly the slots the value occupies. The value's low
 * bits sit in the first slot, starting at bit 'startBitOffset' of the
 * storage. */
static inline uint64_t PACKED_WIDE_READ(const PACKED_SLOT *base,
                                        const uint64_t startBitOffset) {
    const PACKED_SLOT *in = &base[startBitOffset / BITS_PER_SLOT];
    const uint32_t startBit = (uint32_t)(startBitOffset % BITS_PER_SLOT);
    uint64_t out = (uint64_t)PACKED_SLOT_LOAD(&in[0]) >> startBit;

    /* 'have' < BITS_PER_VALUE <= 64 inside the loop, so the shift is
     * defined */
    uint32_t have = (uint32_t)BITS_PER_SLOT - startBit;
    for (size_t k = 1; have < BITS_PER_VALUE; k++) {
        out |= (uint64_t)PACKED_SLOT_LOAD(&in[k]) << have;
        have += (uint32_t)BITS_PER_SLOT;
    }

    return out & VALUE_MASK;
}

static inline void PACKED_WIDE_WRITE(PACKED_SLOT *base,
                                     const uint64_t startBitOffset,
                                     const uint64_t value) {
    PACKED_SLOT *out = &base[startBitOffset / BITS_PER_SLOT];
    const uint32_t startBit = (uint32_t)(startBitOffset % BITS_PER_SLOT);

    /* First slot: the value's low bits from 'startBit' up */
    SLOT_STORAGE_TYPE slot = PACKED_SLOT_LOAD(&out[0]);
    slot = (SLOT_STORAGE_TYPE)((slot & ~(VALUE_MASK << startBit)) |
                               (value << startBit));
    PACKED_SLOT_STORE(&out[0], slot);

    /* Later slots: the next BITS_PER_SLOT bits of the value each, keeping
     * the bits above the value's end in the last one */
    uint32_t done = (uint32_t)BITS_PER_SLOT - startBit;
    for (size_t k = 1; done < BITS_PER_VALUE; k++) {
        slot = PACKED_SLOT_LOAD(&out[k]);
        slot = (SLOT_STORAGE_TYPE)((slot & ~(VALUE_MASK >> done)) |
                                   (value >> done));
        PACKED_SLOT_STORE(&out[k], slot);
        done += (uint32_t)BITS_PER_SLOT;
    }
}

/* Store 'val' at 'offset'. 'val' must fit in BITS_PER_VALUE bits (asserted);
 * any higher bits are discarded, so a value never reaches the bits of its
 * neighbours. */
PACKED_STATIC void PACKED_ARRAY_SET(void *_dst, const PACKED_LEN_TYPE offset,
                                    const VALUE_TYPE val) {
    PACKED_SLOT *restrict out;
    uint32_t startBit;
    uint32_t bitsAvailable;
    const uint64_t startBitOffset = startOffset(offset);

    PACKED_SLOT *dst = (PACKED_SLOT *)_dst;

    out = &dst[startBitOffset / BITS_PER_SLOT];
    startBit = startBitOffset % BITS_PER_SLOT;

    bitsAvailable = BITS_PER_SLOT - startBit;

    assert(0 == (~VALUE_MASK & val));
    const uint64_t fitted =
        (uint64_t)MICRO_PROMOTION_TYPE_CAST(val) & VALUE_MASK;

    if (PACKED_MAX_SLOTS > 2) {
        PACKED_WIDE_WRITE(dst, startBitOffset, fitted);
        return;
    }

    if (SLOT_CAN_HOLD_ENTIRE_VALUE && BITS_PER_VALUE <= bitsAvailable) {
        /* target position is fully inside out[0] */
        /* We set bits starting at position zero, so values are stored
         * from RIGHT to LEFT.
         * Example: storing one 12-bit value of 4095 at packed array position 0
         * backed by a 32 bit (4 byte) slot looks like:
         * [00000000000000000000111111111111]
         * Storing the same 4095 in position 0 backed by a 64 bit slot is:
         * [0000000000000000000000000000000000000000000000000000111111111111]
         */
        SLOT_STORAGE_TYPE current = PACKED_SLOT_LOAD(out);
        current = (SLOT_STORAGE_TYPE)((current &
                                       ~((uint64_t)VALUE_MASK << startBit)) |
                                      (fitted << startBit));
        PACKED_SLOT_STORE(out, current);
    } else {
        /* target position is split across two slots */
        uint64_t low, high;

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
        low = fitted << startBit;
        high = fitted >> bitsAvailable;

        SLOT_STORAGE_TYPE slot0 = PACKED_SLOT_LOAD(&out[0]);
        SLOT_STORAGE_TYPE slot1 = PACKED_SLOT_LOAD(&out[1]);

        slot0 =
            (SLOT_STORAGE_TYPE)((slot0 & ~((uint64_t)VALUE_MASK << startBit)) |
                                low);
        slot1 = (SLOT_STORAGE_TYPE)((slot1 &
                                     ~((uint64_t)VALUE_MASK >> bitsAvailable)) |
                                    high);

        PACKED_SLOT_STORE(&out[0], slot0);
        PACKED_SLOT_STORE(&out[1], slot1);
    }
}

PACKED_STATIC void PACKED_ARRAY_SET_HALF(void *_dst,
                                         const PACKED_LEN_TYPE offset) {
    PACKED_SLOT *restrict out;
    uint32_t startBit;
    uint32_t bitsAvailable;
    const uint64_t startBitOffset = startOffset(offset);

    PACKED_SLOT *dst = (PACKED_SLOT *)_dst;

    out = &dst[startBitOffset / BITS_PER_SLOT];
    startBit = startBitOffset % BITS_PER_SLOT;

    bitsAvailable = BITS_PER_SLOT - startBit;

    if (PACKED_MAX_SLOTS > 2) {
        const uint64_t current = PACKED_WIDE_READ(dst, startBitOffset);
        if (current) {
            PACKED_WIDE_WRITE(dst, startBitOffset, current / 2);
        }

        return;
    }

    if (SLOT_CAN_HOLD_ENTIRE_VALUE && BITS_PER_VALUE <= bitsAvailable) {
        /* target position is fully inside out[0] */
        SLOT_STORAGE_TYPE slot = PACKED_SLOT_LOAD(out);
        const VALUE_TYPE current =
            (VALUE_TYPE)((MICRO_PROMOTION_TYPE_CAST(slot) >> startBit) &
                         VALUE_MASK);

        if (!current) {
            /* No sense trying to divide and set nothing */
            return;
        }

        const VALUE_TYPE val = current / 2;
        slot =
            (SLOT_STORAGE_TYPE)((slot & ~((uint64_t)VALUE_MASK << startBit)) |
                                ((uint64_t)MICRO_PROMOTION_TYPE_CAST(val)
                                 << startBit));
        PACKED_SLOT_STORE(out, slot);
    } else {
        /* target position is split across two slots */
        uint64_t low, high;

        /* GET */
        SLOT_STORAGE_TYPE slot0 = PACKED_SLOT_LOAD(&out[0]);
        SLOT_STORAGE_TYPE slot1 = PACKED_SLOT_LOAD(&out[1]);

        low = MICRO_PROMOTION_TYPE_CAST(slot0) >> startBit;
        high = (uint64_t)(MICRO_PROMOTION_TYPE_CAST(slot1)) << bitsAvailable;

        const VALUE_TYPE current =
            (VALUE_TYPE)(low | (high & (((uint64_t)VALUE_MASK >> bitsAvailable)
                                        << bitsAvailable)));

        if (!current) {
            /* No sense trying to divide and set nothing */
            return;
        }

        const VALUE_TYPE val = current / 2;

        /* SET */
        low = (uint64_t)MICRO_PROMOTION_TYPE_CAST(val) << startBit;
        high = (uint64_t)MICRO_PROMOTION_TYPE_CAST(val) >> bitsAvailable;

        slot0 =
            (SLOT_STORAGE_TYPE)((slot0 & ~((uint64_t)VALUE_MASK << startBit)) |
                                low);
        slot1 = (SLOT_STORAGE_TYPE)((slot1 &
                                     ~((uint64_t)VALUE_MASK >> bitsAvailable)) |
                                    high);

        PACKED_SLOT_STORE(&out[0], slot0);
        PACKED_SLOT_STORE(&out[1], slot1);
    }
}

/* 'current' plus 'incrBy', clamped to [0, VALUE_MASK]: an increment stops at
 * the largest value BITS_PER_VALUE bits can hold and a decrement stops at 0.
 * Requires current <= VALUE_MASK (every stored value is). */
static inline VALUE_TYPE PACKED_SATURATING_ADD(const VALUE_TYPE current,
                                               const int64_t incrBy) {
    const uint64_t cur = (uint64_t)current;
    if (incrBy >= 0) {
        const uint64_t room = (uint64_t)VALUE_MASK - cur;
        return (VALUE_TYPE)((uint64_t)incrBy >= room ? (uint64_t)VALUE_MASK
                                                     : cur + (uint64_t)incrBy);
    }

    /* Magnitude of a negative int64_t, INT64_MIN included */
    const uint64_t decrBy = (uint64_t)0 - (uint64_t)incrBy;
    return (VALUE_TYPE)(decrBy >= cur ? 0 : cur - decrBy);
}

/* Add 'incrBy' (which may be negative) to the value at 'offset', saturating:
 * the result is clamped to [0, 2^BITS_PER_VALUE - 1], so counting past the
 * maximum keeps the maximum, a decrement never goes below 0 or wraps, and
 * the neighbouring values are never touched. */
PACKED_STATIC void PACKED_ARRAY_SET_INCR(void *_dst,
                                         const PACKED_LEN_TYPE offset,
                                         const int64_t incrBy) {
    PACKED_SLOT *restrict out;
    uint32_t startBit;
    uint32_t bitsAvailable;
    const uint64_t startBitOffset = startOffset(offset);

    PACKED_SLOT *dst = (PACKED_SLOT *)_dst;

    out = &dst[startBitOffset / BITS_PER_SLOT];
    startBit = startBitOffset % BITS_PER_SLOT;

    bitsAvailable = BITS_PER_SLOT - startBit;

    if (PACKED_MAX_SLOTS > 2) {
        const VALUE_TYPE current =
            (VALUE_TYPE)PACKED_WIDE_READ(dst, startBitOffset);
        PACKED_WIDE_WRITE(dst, startBitOffset,
                          (uint64_t)PACKED_SATURATING_ADD(current, incrBy));
        return;
    }

    if (SLOT_CAN_HOLD_ENTIRE_VALUE && BITS_PER_VALUE <= bitsAvailable) {
        /* target position is fully inside out[0] */
        SLOT_STORAGE_TYPE slot = PACKED_SLOT_LOAD(out);
        const VALUE_TYPE current =
            (VALUE_TYPE)((MICRO_PROMOTION_TYPE_CAST(slot) >> startBit) &
                         VALUE_MASK);
        const VALUE_TYPE val = PACKED_SATURATING_ADD(current, incrBy);
        slot =
            (SLOT_STORAGE_TYPE)((slot & ~((uint64_t)VALUE_MASK << startBit)) |
                                ((uint64_t)MICRO_PROMOTION_TYPE_CAST(val)
                                 << startBit));
        PACKED_SLOT_STORE(out, slot);
    } else {
        /* target position is split across two slots */
        uint64_t low, high;

        /* GET */
        SLOT_STORAGE_TYPE slot0 = PACKED_SLOT_LOAD(&out[0]);
        SLOT_STORAGE_TYPE slot1 = PACKED_SLOT_LOAD(&out[1]);

        low = MICRO_PROMOTION_TYPE_CAST(slot0) >> startBit;
        high = (uint64_t)(MICRO_PROMOTION_TYPE_CAST(slot1)) << bitsAvailable;

        const VALUE_TYPE current =
            (VALUE_TYPE)(low | (high & (((uint64_t)VALUE_MASK >> bitsAvailable)
                                        << bitsAvailable)));
        const VALUE_TYPE val = PACKED_SATURATING_ADD(current, incrBy);

        /* SET */
        low = (uint64_t)MICRO_PROMOTION_TYPE_CAST(val) << startBit;
        high = (uint64_t)MICRO_PROMOTION_TYPE_CAST(val) >> bitsAvailable;

        slot0 =
            (SLOT_STORAGE_TYPE)((slot0 & ~((uint64_t)VALUE_MASK << startBit)) |
                                low);
        slot1 = (SLOT_STORAGE_TYPE)((slot1 &
                                     ~((uint64_t)VALUE_MASK >> bitsAvailable)) |
                                    high);

        PACKED_SLOT_STORE(&out[0], slot0);
        PACKED_SLOT_STORE(&out[1], slot1);
    }
}

PACKED_STATIC VALUE_TYPE PACKED_ARRAY_GET(const void *src_,
                                          const PACKED_LEN_TYPE offset) {
    const PACKED_SLOT *restrict in = NULL;
    uint32_t startBit = 0;
    uint32_t bitsAvailable = 0;
    VALUE_TYPE out = 0;
    const uint64_t startBitOffset = startOffset(offset);

    const PACKED_SLOT *src = (const PACKED_SLOT *)src_;

    in = &src[startBitOffset / BITS_PER_SLOT];
    startBit = startBitOffset % BITS_PER_SLOT;

    bitsAvailable = BITS_PER_SLOT - startBit;

    if (PACKED_MAX_SLOTS > 2) {
        return (VALUE_TYPE)PACKED_WIDE_READ(src, startBitOffset);
    }

    if (SLOT_CAN_HOLD_ENTIRE_VALUE && BITS_PER_VALUE <= bitsAvailable) {
        /* stored value is fully contained inside in[0] */
        /* If value is entirely in one slot, we just need to shift down
         * the packed integer then mask away other values. */
        const SLOT_STORAGE_TYPE slot = PACKED_SLOT_LOAD(&in[0]);
        out = (VALUE_TYPE)((MICRO_PROMOTION_TYPE_CAST(slot) >> startBit) &
                           VALUE_MASK);
    } else {
        /* stored value is split across two slots */
        uint64_t low, high;

        /* Restore from two slots by moving in[0] bits down and
         * in[1] bits up */
        const SLOT_STORAGE_TYPE slot0 = PACKED_SLOT_LOAD(&in[0]);
        const SLOT_STORAGE_TYPE slot1 = PACKED_SLOT_LOAD(&in[1]);

        low = MICRO_PROMOTION_TYPE_CAST(slot0) >> startBit;
        high = (uint64_t)(MICRO_PROMOTION_TYPE_CAST(slot1)) << bitsAvailable;

        /* Re-create the packed integer by combining the shifted
         * down 'low' bits and mask away bits in 'high' not part of
         * this packed integer. */
        out = (VALUE_TYPE)(low | (high & ((VALUE_MASK >> bitsAvailable)
                                          << bitsAvailable)));
    }

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
        const PACKED_LEN_TYPE mid = (PACKED_LEN_TYPE)((min + max) >> 1);
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

    /* Bounds check is required: binary search returns 'len' when the value
     * should be inserted past the end (i.e., val > all elements).
     * Without this check, we'd read past the array boundary. */
    if (min < len && PACKED_ARRAY_GET(src_, min) == val) {
        return min;
    }

    return -1;
}

PACKED_STATIC int64_t PACKED_ARRAY_MEMBER_BYTES(const void *src_,
                                                const size_t bytes,
                                                const VALUE_TYPE val) {
    return PACKED_ARRAY_MEMBER(
        src_, (PACKED_LEN_TYPE)PACKED_ARRAY_COUNT_FROM_STORAGE_BYTES(bytes),
        val);
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
    PACKED_ARRAY_INSERT(
        _dst, (PACKED_LEN_TYPE)PACKED_ARRAY_COUNT_FROM_STORAGE_BYTES(bytes),
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
        _dst, (PACKED_LEN_TYPE)PACKED_ARRAY_COUNT_FROM_STORAGE_BYTES(bytes),
        val);
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
    PACKED_ARRAY_DELETE(
        _dst, (PACKED_LEN_TYPE)PACKED_ARRAY_COUNT_FROM_STORAGE_BYTES(bytes),
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
        PACKED_ARRAY_DELETE(_dst, len, (PACKED_LEN_TYPE)memberOffset);
        return true;
    }

    return false;
}

PACKED_STATIC bool PACKED_ARRAY_DELETE_MEMBER_BYTES(void *_dst,
                                                    const size_t bytes,
                                                    const VALUE_TYPE member) {
    return PACKED_ARRAY_DELETE_MEMBER(
        _dst, (PACKED_LEN_TYPE)PACKED_ARRAY_COUNT_FROM_STORAGE_BYTES(bytes),
        member);
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
#undef startOffset
#undef PACKED_SLOT
#undef PACKED_SLOT_LOAD
#undef PACKED_SLOT_STORE
#undef PACKED_SATURATING_ADD
#undef SLOT_CAN_HOLD_ENTIRE_VALUE
#undef PACKED_WIDE_READ
#undef PACKED_WIDE_WRITE
#undef PACKED_LOWEST_SET_BIT
#undef PACKED_START_STEP
#undef PACKED_MAX_SLOTS

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
