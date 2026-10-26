#include "dod.h"

#include <assert.h>
#include <inttypes.h> /* PRIu64 for debugging */

#include "datakit.h"

#include <stdio.h>

/* Manually undo two's compliment negative number */
#define DOD_INT64_TO_UINT64(i)                                                 \
    ({                                                                         \
        assert((i) < 0);                                                       \
        (uint64_t)(~(i)) + 1;                                                  \
    })

#if 0
#define DOD_DIV_CEIL(a, b) (((a) + (b) - 1) / (b))
#define DOD_BYTESUSED(value)                                                   \
    (value ? DOD_DIV_CEIL(((sizeof(value) * 8) - __builtin_clzll(value)), 8)   \
           : 0)
#endif

/* Writer streams (dodWrite) start with a header holding the first two
 * values, so the stream alone is enough to decode it:
 *   - 2 bits: header code
 *   - t0: 32 bits (codes 0 and 1, t0 in [0, UINT32_MAX]) or 64 bits
 *   - t1: the difference t1 - t0 (modulo 2^64) in sign-magnitude form in
 *     14, 16 or 45 bits (codes 0, 1, 2), or t1 itself in 64 bits (code 3)
 * The writer always picks the smallest code that holds the pair.
 * Delta-of-delta entries follow the header. */
typedef enum dodHeader {
    DOD_HEADER_T0_32_DELTA_14 = 0,
    DOD_HEADER_T0_32_DELTA_16 = 1,
    DOD_HEADER_T0_64_DELTA_45 = 2,
    DOD_HEADER_T0_64_T1_64 = 3
    /* two bits of code: no room for more */
} dodHeader;

/* ====================================================================
 * Give varintBitstream our types for the bitstream
 * ==================================================================== */
#define VBITS dod
#define VBITSVAL dod
#include "../deps/varint/src/varintBitstream.h"

/* ====================================================================
 * Writer stream header
 * ==================================================================== */
static_assert(DOD_WRITER_HEADER_MAX_BITS == 2 + 64 + 64,
              "header size bound out of date");

/* Write the header for first values t0, t1 at bit 0 of 'd'; returns its
 * length in bits (where the first delta-of-delta entry starts). */
static size_t dodHeaderWrite(dod *const d, const dodVal t0, const dodVal t1) {
    const uint64_t delta = (uint64_t)t1 - (uint64_t)t0;
    const bool negative = (int64_t)delta < 0;
    const uint64_t magnitude = negative ? (uint64_t)0 - delta : delta;
    const bool t0Fits32 = t0 >= 0 && (uint64_t)t0 <= UINT32_MAX;

    dodHeader code;
    uint32_t t0Bits = 64;
    uint32_t deltaBits = 0;
    if (t0Fits32 && magnitude < (1ULL << 13)) {
        code = DOD_HEADER_T0_32_DELTA_14;
        t0Bits = 32;
        deltaBits = 14;
    } else if (t0Fits32 && magnitude < (1ULL << 15)) {
        code = DOD_HEADER_T0_32_DELTA_16;
        t0Bits = 32;
        deltaBits = 16;
    } else if (magnitude < (1ULL << 44)) {
        code = DOD_HEADER_T0_64_DELTA_45;
        deltaBits = 45;
    } else {
        code = DOD_HEADER_T0_64_T1_64;
    }

    size_t bits = 0;
    varintBitstreamSet(d, bits, 2, code);
    bits += 2;
    varintBitstreamSet(d, bits, t0Bits, (uint64_t)t0);
    bits += t0Bits;

    if (code == DOD_HEADER_T0_64_T1_64) {
        varintBitstreamSet(d, bits, 64, (uint64_t)t1);
        bits += 64;
    } else {
        /* Top bit of the field is the sign, the rest is the magnitude */
        const uint64_t field =
            magnitude | (negative ? 1ULL << (deltaBits - 1) : 0);
        varintBitstreamSet(d, bits, deltaBits, field);
        bits += deltaBits;
    }

    return bits;
}

/* Read the header of writer stream 'd' into *t0, *t1; returns its length in
 * bits (where the first delta-of-delta entry starts). */
static size_t dodHeaderRead(const dod *const d, dodVal *const t0,
                            dodVal *const t1) {
    const dodHeader code = (dodHeader)varintBitstreamGet(d, 0, 2);
    size_t bits = 2;

    uint32_t deltaBits = 0;
    if (code == DOD_HEADER_T0_32_DELTA_14 ||
        code == DOD_HEADER_T0_32_DELTA_16) {
        *t0 = (dodVal)varintBitstreamGet(d, bits, 32);
        bits += 32;
        deltaBits = code == DOD_HEADER_T0_32_DELTA_14 ? 14 : 16;
    } else {
        *t0 = (dodVal)varintBitstreamGet(d, bits, 64);
        bits += 64;
        deltaBits = code == DOD_HEADER_T0_64_DELTA_45 ? 45 : 0;
    }

    if (code == DOD_HEADER_T0_64_T1_64) {
        *t1 = (dodVal)varintBitstreamGet(d, bits, 64);
        bits += 64;
    } else {
        const uint64_t field = varintBitstreamGet(d, bits, deltaBits);
        bits += deltaBits;
        const uint64_t signBit = 1ULL << (deltaBits - 1);
        const uint64_t magnitude = field & (signBit - 1);
        const uint64_t delta =
            (field & signBit) ? (uint64_t)0 - magnitude : magnitude;
        *t1 = (dodVal)((uint64_t)*t0 + delta);
    }

    return bits;
}

/* ====================================================================
 * Extended helper values for stacked ranges
 * ==================================================================== */

/* Note: these are *exclusive* dodMax, not *inclusive* dodMax;
 *       the commented ranges below are one minus the max value */
typedef enum dodMax {
    MAX_0 = 1,                     /* (special, actually 0); range: [0, 0] */
    MAX_7 = (1 << 6) + MAX_0,      /* (1 << 6); [-64, 64] */
    MAX_9 = (1 << 8) + MAX_7,      /* (1 << 8); [-320, 320] */
    MAX_12 = (1 << 11) + MAX_9,    /* (1 << 11); [-2368, 2368] */
    MAX_V8 = (1 << 8) + MAX_12,    /* [-2624, 2624] */
    MAX_V16 = (1 << 16) + MAX_V8,  /* [-68160, 68160] */
    MAX_V24 = (1 << 24) + MAX_V16, /* [-16845376, 16845376] */
    MAX_V32 = (1ULL << 32) + MAX_V24,
    MAX_V40 = (1ULL << 40) + MAX_V32,
    MAX_V48 = (1ULL << 48) + MAX_V40,
    MAX_V56 = (1ULL << 56) + MAX_V48
} dodMax;

/* ====================================================================
 * Bits designating types for values
 * ==================================================================== */
/* Note: these type designations are verified optimal bit patterns because we
 *       extract the designate bits to determine the type. Designate bits
 *       must be unique across all types (i.e. for the 7/9/12 types,
 *       they must have a 0 terminator after so they don't get confused
 *       for the wider 8/16/24/32/40/48/56/64 bit types). */
typedef enum [[gnu::packed]] dodType {
    /* 0 bits */
    DOD_TYPE_ZERO = 0x00, /* 00000000 */

    /* 3 bits type; 6 bits data */
    /* (1 bit desigante, 1 bit sub-designate, 1 bit positive/negative) */
    DOD_TYPE_SEVEN = 0x04,          /* 00000100 */
    DOD_TYPE_SEVEN_NEGATIVE = 0x05, /* 00000101 */

    /* 4 bits type; 8 bits data */
    /* (2 bits desigante, 1 bit sub-designate,1 bit positive/negative) */
    DOD_TYPE_NINE = 0x0c,          /* 00001100 */
    DOD_TYPE_NINE_NEGATIVE = 0x0d, /* 00001101 */

    /* 5 bits type; 11 bits data */
    /* (3 bits desigante, 1 bit sub-designate, 1 bit positive/negative) */
    DOD_TYPE_TWELVE = 0x1c,          /* 00011100 */
    DOD_TYPE_TWELVE_NEGATIVE = 0x1d, /* 00011101 */

    /* 8 bits type; 8 bits data */
    /* (4 bits desigante, 3 bits sub-designate, 1 bit positive/negative) */
    DOD_TYPE_VAR_8 = 0xf0,          /* 11110000 */
    DOD_TYPE_VAR_8_NEGATIVE = 0xf1, /* 11110001 */

    /* 8 bits type; 16 bits data */
    /* (4 bits desigante, 3 bits sub-designate, 1 bit positive/negative) */
    DOD_TYPE_VAR_16 = 0xf2,          /* 11110010 */
    DOD_TYPE_VAR_16_NEGATIVE = 0xf3, /* 11110011 */

    /* 8 bits type; 24 bits data */
    /* (4 bits desigante, 3 bits sub-designate, 1 bit positive/negative) */
    DOD_TYPE_VAR_24 = 0xf4,          /* 11110100 */
    DOD_TYPE_VAR_24_NEGATIVE = 0xf5, /* 11110101 */

    /* 8 bits type; 32 bits data */
    /* (4 bits desigante, 3 bits sub-designate, 1 bit positive/negative) */
    DOD_TYPE_VAR_32 = 0xf6,          /* 11110110 */
    DOD_TYPE_VAR_32_NEGATIVE = 0xf7, /* 11110111 */

    /* 8 bits type; 40 bits data */
    /* (4 bits desigante, 3 bits sub-designate, 1 bit positive/negative) */
    DOD_TYPE_VAR_40 = 0xf8,          /* 11111000 */
    DOD_TYPE_VAR_40_NEGATIVE = 0xf9, /* 11111001 */

    /* 8 bits type; 48 bits data */
    /* (4 bits desigante, 3 bits sub-designate, 1 bit positive/negative) */
    DOD_TYPE_VAR_48 = 0xfa,          /* 11111010 */
    DOD_TYPE_VAR_48_NEGATIVE = 0xfb, /* 11111011 */

    /* 8 bits type; 56 bits data */
    /* (4 bits desigante, 3 bits sub-designate, 1 bit positive/negative) */
    DOD_TYPE_VAR_56 = 0xfc,          /* 11111100 */
    DOD_TYPE_VAR_56_NEGATIVE = 0xfd, /* 11111101 */

    /* 8 bits type; 64 bits data */
    /* (4 bits desigante, 3 bits sub-designate, 1 bit positive/negative) */
    DOD_TYPE_VAR_64 = 0xfe,         /* 111111110 */
    DOD_TYPE_VAR_64_NEGATIVE = 0xff /* 111111111 */
} dodType;

static_assert(sizeof(dodType) == 1, "dodType not packed?");

/* Details about:
 *  - metadata bit length
 *  - value bit length
 *  - type enum
 * for write iterating. */
typedef struct dodFormat {
    const uint8_t meta;
    const uint8_t val;
    const dodType type;
    const uint8_t unused;
} dodFormat;

static_assert(sizeof(dodFormat) == 4, "dodFormat unnecessarily big?");

/* Array of formats for iterating during hierarchical write offsetting. */
static const dodFormat dodBitCategory[] = {
    {.meta = 1, .val = 0, .type = DOD_TYPE_ZERO},
    {.meta = 3, .val = 6, .type = DOD_TYPE_SEVEN},
    {.meta = 4, .val = 8, .type = DOD_TYPE_NINE},
    {.meta = 5, .val = 11, .type = DOD_TYPE_TWELVE},
    {.meta = 8, .val = 8, .type = DOD_TYPE_VAR_8},
    {.meta = 8, .val = 16, .type = DOD_TYPE_VAR_16},
    {.meta = 8, .val = 24, .type = DOD_TYPE_VAR_24},
    {.meta = 8, .val = 32, .type = DOD_TYPE_VAR_32},
    {.meta = 8, .val = 40, .type = DOD_TYPE_VAR_40},
    {.meta = 8, .val = 48, .type = DOD_TYPE_VAR_48},
    {.meta = 8, .val = 56, .type = DOD_TYPE_VAR_56},
    {.meta = 8, .val = 64, .type = DOD_TYPE_VAR_64}};

/* ====================================================================
 * Core delta math
 * ==================================================================== */
/* Delta-of-delta arithmetic runs in uint64_t (arithmetic modulo 2^64).
 * Encode and decode are then exact inverses for every int64_t input,
 * including sequences whose deltas do not fit in int64_t: the stored
 * value is the true delta-of-delta reduced to its int64_t residue, and
 * decoding adds it back modulo 2^64. Whenever the true delta-of-delta
 * fits in int64_t, the residue equals it. */
static inline int64_t dodDelta(const dodVal t_n, const dodVal t_n_1,
                               const dodVal t_n_0) {
    const uint64_t n = (uint64_t)t_n;
    const uint64_t n1 = (uint64_t)t_n_1;
    const uint64_t n0 = (uint64_t)t_n_0;
    return (int64_t)((n - n1) - (n1 - n0));
}

/* Inverse of dodDelta(): 'delta' is the delta-of-delta as a uint64_t
 * residue (a negative delta-of-delta is its two's complement). */
static inline dodVal dodUndelta(const uint64_t delta, const dodVal t_n_1,
                                const dodVal t_n_0) {
    const uint64_t n1 = (uint64_t)t_n_1;
    const uint64_t n0 = (uint64_t)t_n_0;
    return (dodVal)((delta + n1) + (n1 - n0));
}

/* ====================================================================
 * Get one value from a dod given previous conditions
 * ==================================================================== */
dodVal dodGet(const dod *restrict const d, size_t *restrict consumedBits,
              const dodVal originalStartVal, dodVal currentVal,
              const size_t valueOffsetToReturn) {
    /* This is a 512 byte lookup table.
     * It would be a 1024 byte lookup table if we stored the void* labels
     * directly, but if we chop them in half with the base offset,
     * we get to save half our cachelines.
     *
     * If the 'next 8 bits' are negative, use the table.
     * else, it's the zero marker for immediate processing. */

    /* This looks weird because our types are between two bits and eight
     * bits, but we can't know before read the type how long it is.
     * So, we *always* read 8 type bits, then if the type is less than
     * 8 bits, we have a lookup table covering all bytes starting with
     * our type indicator bits to collapse the search space to one
     * lookup instead of searching/masking for each lookup. */
    static const int32_t dispatchNoZero[128] = {
        &&Seven - &&Seven,          &&Seven - &&Seven,
        &&Seven - &&Seven,          &&Seven - &&Seven,
        &&Seven - &&Seven,          &&Seven - &&Seven,
        &&Seven - &&Seven,          &&Seven - &&Seven,
        &&Seven - &&Seven,          &&Seven - &&Seven,
        &&Seven - &&Seven,          &&Seven - &&Seven,
        &&Seven - &&Seven,          &&Seven - &&Seven,
        &&Seven - &&Seven,          &&Seven - &&Seven,
        &&Seven - &&Seven,          &&Seven - &&Seven,
        &&Seven - &&Seven,          &&Seven - &&Seven,
        &&Seven - &&Seven,          &&Seven - &&Seven,
        &&Seven - &&Seven,          &&Seven - &&Seven,
        &&Seven - &&Seven,          &&Seven - &&Seven,
        &&Seven - &&Seven,          &&Seven - &&Seven,
        &&Seven - &&Seven,          &&Seven - &&Seven,
        &&Seven - &&Seven,          &&Seven - &&Seven,
        &&SevenNegative - &&Seven,  &&SevenNegative - &&Seven,
        &&SevenNegative - &&Seven,  &&SevenNegative - &&Seven,
        &&SevenNegative - &&Seven,  &&SevenNegative - &&Seven,
        &&SevenNegative - &&Seven,  &&SevenNegative - &&Seven,
        &&SevenNegative - &&Seven,  &&SevenNegative - &&Seven,
        &&SevenNegative - &&Seven,  &&SevenNegative - &&Seven,
        &&SevenNegative - &&Seven,  &&SevenNegative - &&Seven,
        &&SevenNegative - &&Seven,  &&SevenNegative - &&Seven,
        &&SevenNegative - &&Seven,  &&SevenNegative - &&Seven,
        &&SevenNegative - &&Seven,  &&SevenNegative - &&Seven,
        &&SevenNegative - &&Seven,  &&SevenNegative - &&Seven,
        &&SevenNegative - &&Seven,  &&SevenNegative - &&Seven,
        &&SevenNegative - &&Seven,  &&SevenNegative - &&Seven,
        &&SevenNegative - &&Seven,  &&SevenNegative - &&Seven,
        &&SevenNegative - &&Seven,  &&SevenNegative - &&Seven,
        &&SevenNegative - &&Seven,  &&SevenNegative - &&Seven,
        &&Nine - &&Seven,           &&Nine - &&Seven,
        &&Nine - &&Seven,           &&Nine - &&Seven,
        &&Nine - &&Seven,           &&Nine - &&Seven,
        &&Nine - &&Seven,           &&Nine - &&Seven,
        &&Nine - &&Seven,           &&Nine - &&Seven,
        &&Nine - &&Seven,           &&Nine - &&Seven,
        &&Nine - &&Seven,           &&Nine - &&Seven,
        &&Nine - &&Seven,           &&Nine - &&Seven,
        &&NineNegative - &&Seven,   &&NineNegative - &&Seven,
        &&NineNegative - &&Seven,   &&NineNegative - &&Seven,
        &&NineNegative - &&Seven,   &&NineNegative - &&Seven,
        &&NineNegative - &&Seven,   &&NineNegative - &&Seven,
        &&NineNegative - &&Seven,   &&NineNegative - &&Seven,
        &&NineNegative - &&Seven,   &&NineNegative - &&Seven,
        &&NineNegative - &&Seven,   &&NineNegative - &&Seven,
        &&NineNegative - &&Seven,   &&NineNegative - &&Seven,
        &&Twelve - &&Seven,         &&Twelve - &&Seven,
        &&Twelve - &&Seven,         &&Twelve - &&Seven,
        &&Twelve - &&Seven,         &&Twelve - &&Seven,
        &&Twelve - &&Seven,         &&Twelve - &&Seven,
        &&TwelveNegative - &&Seven, &&TwelveNegative - &&Seven,
        &&TwelveNegative - &&Seven, &&TwelveNegative - &&Seven,
        &&TwelveNegative - &&Seven, &&TwelveNegative - &&Seven,
        &&TwelveNegative - &&Seven, &&TwelveNegative - &&Seven,
        &&Variable8 - &&Seven,      &&Variable8Negative - &&Seven,
        &&Variable16 - &&Seven,     &&Variable16Negative - &&Seven,
        &&Variable24 - &&Seven,     &&Variable24Negative - &&Seven,
        &&Variable32 - &&Seven,     &&Variable32Negative - &&Seven,
        &&Variable40 - &&Seven,     &&Variable40Negative - &&Seven,
        &&Variable48 - &&Seven,     &&Variable48Negative - &&Seven,
        &&Variable56 - &&Seven,     &&Variable56Negative - &&Seven,
        &&Variable64 - &&Seven,     &&Variable64Negative - &&Seven};

    size_t consumedValueCount = 0;

    /* These positions are required for the initial
     * goto Dispatch to happen properly. */
    dodVal t0 = 0;
    dodVal t1 = originalStartVal;

#define UNDELTA(delta) dodUndelta((uint64_t)(delta), t1, t0)

/* The magnitude (boost + payload) reaches 2^63 for INT64_MIN, so it is
 * formed and negated in uint64_t. */
#define UNDELTA_BITS_BOOST(boost, valBits)                                     \
    UNDELTA((uint64_t)(boost) + varintBitstreamGet(d, *consumedBits, valBits))

#define UNDELTA_BITS_BOOST_NEGATIVE(boost, valBits)                            \
    UNDELTA((uint64_t)0 - ((uint64_t)(boost) +                                 \
                           varintBitstreamGet(d, *consumedBits, valBits)))

Dispatch:
    while (true) {
        if (consumedValueCount == valueOffsetToReturn) {
            return currentVal;
        }

        t0 = t1;
        t1 = currentVal;
        consumedValueCount++;

        /* Read the next 8 bits from the bitstream.
         * (IMPORTANT: DO NOT CHANGE THIS TYPE! The type is correct.)
         * If the first bit is zero, process zero.
         * else, process a 2 to 8 byte type as described in
         * 'dispatchNoZero' */
        int8_t next = (int8_t)varintBitstreamGet(d, *consumedBits, 8);
        if (next >= 0) {
            currentVal = UNDELTA(0);
            *consumedBits += 1; /* jump over data bit */
            /* continue looping as long as we're processing zeroes.
             * No need to jump to another processing block yet. */
        } else {
            /* by pre-extracting the zero entry above, we get to use a 50%
             * smaller dispatch table (since 'starts with 0' covers half
             * of the entire 8 bit range.) */

            /* Re-construct our target goto label based on the table
             * offset and jump to the next instruction. */
            goto *(&&Seven + dispatchNoZero[(uint8_t)next - 128]);
        }
    }

#define CONSUME_UPDATE_CONSUME(boost, typeBits, dataBits)                      \
    do {                                                                       \
        *consumedBits += (typeBits); /* jump over type bits */                 \
        currentVal = UNDELTA_BITS_BOOST(boost, dataBits);                      \
        *consumedBits += (dataBits); /* jump over data bits */                 \
    } while (0)

#define CONSUME_UPDATE_CONSUME_NEGATIVE(boost, typeBits, dataBits)             \
    do {                                                                       \
        *consumedBits += (typeBits); /* jump over type bits */                 \
        currentVal = UNDELTA_BITS_BOOST_NEGATIVE(boost, dataBits);             \
        *consumedBits += (dataBits); /* jump over data bits */                 \
    } while (0)

#if 0
Zero:
    /* UNUSED */
    /* We handle this case directly in goto Dispatch */
    currentVal = UNDELTA(0);
    *consumedBits += 1; /* jump over data bit */
    goto Dispatch;
#endif
Seven:
    CONSUME_UPDATE_CONSUME(MAX_0, 3, 6);
    goto Dispatch;
SevenNegative:
    CONSUME_UPDATE_CONSUME_NEGATIVE(MAX_0, 3, 6);
    goto Dispatch;
Nine:
    CONSUME_UPDATE_CONSUME(MAX_7, 4, 8);
    goto Dispatch;
NineNegative:
    CONSUME_UPDATE_CONSUME_NEGATIVE(MAX_7, 4, 8);
    goto Dispatch;
Twelve:
    CONSUME_UPDATE_CONSUME(MAX_9, 5, 11);
    goto Dispatch;
TwelveNegative:
    CONSUME_UPDATE_CONSUME_NEGATIVE(MAX_9, 5, 11);
    goto Dispatch;
Variable8:
    CONSUME_UPDATE_CONSUME(MAX_12, 8, 8);
    goto Dispatch;
Variable8Negative:
    CONSUME_UPDATE_CONSUME_NEGATIVE(MAX_12, 8, 8);
    goto Dispatch;
Variable16:
    CONSUME_UPDATE_CONSUME(MAX_V8, 8, 16);
    goto Dispatch;
Variable16Negative:
    CONSUME_UPDATE_CONSUME_NEGATIVE(MAX_V8, 8, 16);
    goto Dispatch;
Variable24:
    CONSUME_UPDATE_CONSUME(MAX_V16, 8, 24);
    goto Dispatch;
Variable24Negative:
    CONSUME_UPDATE_CONSUME_NEGATIVE(MAX_V16, 8, 24);
    goto Dispatch;
Variable32:
    CONSUME_UPDATE_CONSUME(MAX_V24, 8, 32);
    goto Dispatch;
Variable32Negative:
    CONSUME_UPDATE_CONSUME_NEGATIVE(MAX_V24, 8, 32);
    goto Dispatch;
Variable40:
    CONSUME_UPDATE_CONSUME(MAX_V32, 8, 40);
    goto Dispatch;
Variable40Negative:
    CONSUME_UPDATE_CONSUME_NEGATIVE(MAX_V32, 8, 40);
    goto Dispatch;
Variable48:
    CONSUME_UPDATE_CONSUME(MAX_V40, 8, 48);
    goto Dispatch;
Variable48Negative:
    CONSUME_UPDATE_CONSUME_NEGATIVE(MAX_V40, 8, 48);
    goto Dispatch;
Variable56:
    CONSUME_UPDATE_CONSUME(MAX_V48, 8, 56);
    goto Dispatch;
Variable56Negative:
    CONSUME_UPDATE_CONSUME_NEGATIVE(MAX_V48, 8, 56);
    goto Dispatch;
Variable64:
    CONSUME_UPDATE_CONSUME(MAX_V56, 8, 64);
    goto Dispatch;
Variable64Negative:
    CONSUME_UPDATE_CONSUME_NEGATIVE(MAX_V56, 8, 64);
    goto Dispatch;

    unreachable();
}

#ifdef DATAKIT_TEST
/* ====================================================================
 * Get integer without doing the delta-of-delta magic
 * ==================================================================== */
/* This exists only for debugging purposes so we can verify
 * the correctness of integer storage.
 * It's basically copy/paste of dodGet() except we return
 * the exact value at 'offset' instead of accumulating
 * the dod on each new value consumption. */
static int64_t dodGetIntegerAtOffset(const dod *restrict const d,
                                     const size_t offset) {
    /* This is a 512 byte lookup table.
     * If the 'next 8 bits' are negative, use the table.
     * else, it's the zero marker for immediate processing. */

    /* This looks weird because our types are between two bits and eight
     * bits, but we can't know before read the type how long it is.
     * So, we *always* read 8 type bits, then if the type is less than
     * 8 bits, we have a lookup table covering all bytes starting with
     * our type indicator bits to collapse the search space to one
     * lookup instead of searching/masking for each lookup. */
    static const int32_t dispatchNoZero[128] = {
        &&SevenI - &&SevenI,          &&SevenI - &&SevenI,
        &&SevenI - &&SevenI,          &&SevenI - &&SevenI,
        &&SevenI - &&SevenI,          &&SevenI - &&SevenI,
        &&SevenI - &&SevenI,          &&SevenI - &&SevenI,
        &&SevenI - &&SevenI,          &&SevenI - &&SevenI,
        &&SevenI - &&SevenI,          &&SevenI - &&SevenI,
        &&SevenI - &&SevenI,          &&SevenI - &&SevenI,
        &&SevenI - &&SevenI,          &&SevenI - &&SevenI,
        &&SevenI - &&SevenI,          &&SevenI - &&SevenI,
        &&SevenI - &&SevenI,          &&SevenI - &&SevenI,
        &&SevenI - &&SevenI,          &&SevenI - &&SevenI,
        &&SevenI - &&SevenI,          &&SevenI - &&SevenI,
        &&SevenI - &&SevenI,          &&SevenI - &&SevenI,
        &&SevenI - &&SevenI,          &&SevenI - &&SevenI,
        &&SevenI - &&SevenI,          &&SevenI - &&SevenI,
        &&SevenI - &&SevenI,          &&SevenI - &&SevenI,
        &&SevenNegativeI - &&SevenI,  &&SevenNegativeI - &&SevenI,
        &&SevenNegativeI - &&SevenI,  &&SevenNegativeI - &&SevenI,
        &&SevenNegativeI - &&SevenI,  &&SevenNegativeI - &&SevenI,
        &&SevenNegativeI - &&SevenI,  &&SevenNegativeI - &&SevenI,
        &&SevenNegativeI - &&SevenI,  &&SevenNegativeI - &&SevenI,
        &&SevenNegativeI - &&SevenI,  &&SevenNegativeI - &&SevenI,
        &&SevenNegativeI - &&SevenI,  &&SevenNegativeI - &&SevenI,
        &&SevenNegativeI - &&SevenI,  &&SevenNegativeI - &&SevenI,
        &&SevenNegativeI - &&SevenI,  &&SevenNegativeI - &&SevenI,
        &&SevenNegativeI - &&SevenI,  &&SevenNegativeI - &&SevenI,
        &&SevenNegativeI - &&SevenI,  &&SevenNegativeI - &&SevenI,
        &&SevenNegativeI - &&SevenI,  &&SevenNegativeI - &&SevenI,
        &&SevenNegativeI - &&SevenI,  &&SevenNegativeI - &&SevenI,
        &&SevenNegativeI - &&SevenI,  &&SevenNegativeI - &&SevenI,
        &&SevenNegativeI - &&SevenI,  &&SevenNegativeI - &&SevenI,
        &&SevenNegativeI - &&SevenI,  &&SevenNegativeI - &&SevenI,
        &&NineI - &&SevenI,           &&NineI - &&SevenI,
        &&NineI - &&SevenI,           &&NineI - &&SevenI,
        &&NineI - &&SevenI,           &&NineI - &&SevenI,
        &&NineI - &&SevenI,           &&NineI - &&SevenI,
        &&NineI - &&SevenI,           &&NineI - &&SevenI,
        &&NineI - &&SevenI,           &&NineI - &&SevenI,
        &&NineI - &&SevenI,           &&NineI - &&SevenI,
        &&NineI - &&SevenI,           &&NineI - &&SevenI,
        &&NineNegativeI - &&SevenI,   &&NineNegativeI - &&SevenI,
        &&NineNegativeI - &&SevenI,   &&NineNegativeI - &&SevenI,
        &&NineNegativeI - &&SevenI,   &&NineNegativeI - &&SevenI,
        &&NineNegativeI - &&SevenI,   &&NineNegativeI - &&SevenI,
        &&NineNegativeI - &&SevenI,   &&NineNegativeI - &&SevenI,
        &&NineNegativeI - &&SevenI,   &&NineNegativeI - &&SevenI,
        &&NineNegativeI - &&SevenI,   &&NineNegativeI - &&SevenI,
        &&NineNegativeI - &&SevenI,   &&NineNegativeI - &&SevenI,
        &&TwelveI - &&SevenI,         &&TwelveI - &&SevenI,
        &&TwelveI - &&SevenI,         &&TwelveI - &&SevenI,
        &&TwelveI - &&SevenI,         &&TwelveI - &&SevenI,
        &&TwelveI - &&SevenI,         &&TwelveI - &&SevenI,
        &&TwelveNegativeI - &&SevenI, &&TwelveNegativeI - &&SevenI,
        &&TwelveNegativeI - &&SevenI, &&TwelveNegativeI - &&SevenI,
        &&TwelveNegativeI - &&SevenI, &&TwelveNegativeI - &&SevenI,
        &&TwelveNegativeI - &&SevenI, &&TwelveNegativeI - &&SevenI,
        &&Variable8I - &&SevenI,      &&Variable8NegativeI - &&SevenI,
        &&Variable16I - &&SevenI,     &&Variable16NegativeI - &&SevenI,
        &&Variable24I - &&SevenI,     &&Variable24NegativeI - &&SevenI,
        &&Variable32I - &&SevenI,     &&Variable32NegativeI - &&SevenI,
        &&Variable40I - &&SevenI,     &&Variable40NegativeI - &&SevenI,
        &&Variable48I - &&SevenI,     &&Variable48NegativeI - &&SevenI,
        &&Variable56I - &&SevenI,     &&Variable56NegativeI - &&SevenI,
        &&Variable64I - &&SevenI,     &&Variable64NegativeI - &&SevenI};

    int64_t currentVal = 0;
    size_t consumedBits = 0;
    size_t consumedValueCount = 0;

IDispatch:
    while (true) {
        if (consumedValueCount == offset) {
            return currentVal;
        }

        consumedValueCount++;

        /* Read the next 8 bits from the bitstream.
         * If the first bit is zero, process zero.
         * else, process the two to eight byte type as described in
         * 'dispatchNoZero' */
        int8_t next = (int8_t)varintBitstreamGet(d, consumedBits, 8);
        if (next >= 0) {
            currentVal = 0;
            consumedBits += 1; /* jump over data bit */
            /* we continue looping here as long as we're processing
             * zeroes.  No need to jump to another processing block yet. */
        } else {
            /* by pre-extracting the zero entry above, we get to use a 50%
             * smaller dispatch table (since 'starts with 0' covers half
             * of the entire 8 bit range.) */
            goto *(&&SevenI + dispatchNoZero[(uint8_t)next - 128]);
        }
    }

#define ICONSUME_UPDATE_CONSUME(boost, typeBits, dataBits)                     \
    do {                                                                       \
        consumedBits += (typeBits); /* jump over type bits */                  \
        currentVal = (int64_t)((uint64_t)(boost) +                             \
                               varintBitstreamGet(d, consumedBits, dataBits)); \
        consumedBits += (dataBits); /* jump over data bits */                  \
    } while (0)

#define ICONSUME_UPDATE_CONSUME_NEGATIVE(boost, typeBits, dataBits)            \
    do {                                                                       \
        consumedBits += (typeBits); /* jump over type bits */                  \
        currentVal =                                                           \
            (int64_t)((uint64_t)0 -                                            \
                      ((uint64_t)(boost) +                                     \
                       varintBitstreamGet(d, consumedBits, dataBits)));        \
        consumedBits += (dataBits); /* jump over data bits */                  \
    } while (0)

#if 0
Zero:
    /* UNUSED */
    /* We handle this case directly in goto Dispatch */
    currentVal = UNDELTA(0);
    *consumedBits += 1; /* jump over data bit */
    goto IDispatch;
#endif
SevenI:
    ICONSUME_UPDATE_CONSUME(MAX_0, 3, 6);
    goto IDispatch;
SevenNegativeI:
    ICONSUME_UPDATE_CONSUME_NEGATIVE(MAX_0, 3, 6);
    goto IDispatch;
NineI:
    ICONSUME_UPDATE_CONSUME(MAX_7, 4, 8);
    goto IDispatch;
NineNegativeI:
    ICONSUME_UPDATE_CONSUME_NEGATIVE(MAX_7, 4, 8);
    goto IDispatch;
TwelveI:
    ICONSUME_UPDATE_CONSUME(MAX_9, 5, 11);
    goto IDispatch;
TwelveNegativeI:
    ICONSUME_UPDATE_CONSUME_NEGATIVE(MAX_9, 5, 11);
    goto IDispatch;
Variable8I:
    ICONSUME_UPDATE_CONSUME(MAX_12, 8, 8);
    goto IDispatch;
Variable8NegativeI:
    ICONSUME_UPDATE_CONSUME_NEGATIVE(MAX_12, 8, 8);
    goto IDispatch;
Variable16I:
    ICONSUME_UPDATE_CONSUME(MAX_V8, 8, 16);
    goto IDispatch;
Variable16NegativeI:
    ICONSUME_UPDATE_CONSUME_NEGATIVE(MAX_V8, 8, 16);
    goto IDispatch;
Variable24I:
    ICONSUME_UPDATE_CONSUME(MAX_V16, 8, 24);
    goto IDispatch;
Variable24NegativeI:
    ICONSUME_UPDATE_CONSUME_NEGATIVE(MAX_V16, 8, 24);
    goto IDispatch;
Variable32I:
    ICONSUME_UPDATE_CONSUME(MAX_V24, 8, 32);
    goto IDispatch;
Variable32NegativeI:
    ICONSUME_UPDATE_CONSUME_NEGATIVE(MAX_V24, 8, 32);
    goto IDispatch;
Variable40I:
    ICONSUME_UPDATE_CONSUME(MAX_V32, 8, 40);
    goto IDispatch;
Variable40NegativeI:
    ICONSUME_UPDATE_CONSUME_NEGATIVE(MAX_V32, 8, 40);
    goto IDispatch;
Variable48I:
    ICONSUME_UPDATE_CONSUME(MAX_V40, 8, 48);
    goto IDispatch;
Variable48NegativeI:
    ICONSUME_UPDATE_CONSUME_NEGATIVE(MAX_V40, 8, 48);
    goto IDispatch;
Variable56I:
    ICONSUME_UPDATE_CONSUME(MAX_V48, 8, 56);
    goto IDispatch;
Variable56NegativeI:
    ICONSUME_UPDATE_CONSUME_NEGATIVE(MAX_V48, 8, 56);
    goto IDispatch;
Variable64I:
    ICONSUME_UPDATE_CONSUME(MAX_V56, 8, 64);
    goto IDispatch;
Variable64NegativeI:
    ICONSUME_UPDATE_CONSUME_NEGATIVE(MAX_V56, 8, 64);
    goto IDispatch;

    return currentVal;
}
#endif

/* ====================================================================
 * Write integer to bitstream
 * ==================================================================== */
static void dodWriteInteger(dod *restrict const d,
                            size_t *restrict const currentBits,
                            const int64_t dval) {
    uint32_t meta = 0;
    uint32_t metaBits = 0;
    uint32_t valBits = 0;
    uint64_t writeIntoBitmap;

    /* These types are stacked, so each higher range can be exempt
     * from the entire lower range (since the lower range would
     * fully capture the value before it hits the upper range) */
    if (dval == 0) {
        /* values of zero are represented by a zero bit,
         * so there's nothing to write, just increase the
         * metadata of how many bits we're using. */
        (*currentBits) += 1;
        return;
    }

    /* Accumulate total "previous value" offset reductions during the loop */
    int64_t adjustmentBase = 1;

    /* Shorthand... */
#define fmt dodBitCategory[i]

    /* clang and GCC both honor this spelling; GCC ignores "unroll 5" */
#pragma GCC unroll 5
    for (size_t i = 1; i < countof(dodBitCategory) - 1; i++) {
        const int64_t rangeCheck = (1LL << fmt.val) + adjustmentBase;

        /* Yes, our ranges are symmetric.
         * We don't have larger signed storage per level due to 'adjustmentBase'
         * uncompensating for the lower additive compliment offset.
         * Don't worry because everything works for INT64_MIN to INT64_MAX. */
        if (dval > -rangeCheck && dval < rangeCheck) {
#if 0
            printf("Found match for %" PRId64 " at range %" PRId64
                   " with adjustment %" PRIu64 "\n",
                   dval, rangeCheck, adjustmentBase);
#endif
            metaBits = fmt.meta;
            valBits = fmt.val;
            meta = fmt.type;

            if (dval < 0) {
                writeIntoBitmap = -dval;
                meta++;
            } else {
                writeIntoBitmap = dval;
            }

            writeIntoBitmap -= adjustmentBase;
            goto doGoodWriting;
        }

        adjustmentBase = rangeCheck;
    }

    /* If we reach here, 'dval' is a 64-bit wide type... */
    metaBits = 8;
    valBits = 64;
    meta = DOD_TYPE_VAR_64;

    if (dval < 0) {
        /* Safe convert to protect against INT64_MIN */
        writeIntoBitmap = DOD_INT64_TO_UINT64(dval);
        meta++;
    } else {
        writeIntoBitmap = dval;
    }

    /* 'adjustmentBase' is at max value because we iterated completely above. */
    writeIntoBitmap -= adjustmentBase;

doGoodWriting:
    /* 'meta' can't equal (2^metaBits) because then we have no mask */
    assert(meta < (1ULL << metaBits));
    assert(writeIntoBitmap < ((__uint128_t)1 << valBits));

    varintBitstreamSet(d, *currentBits, metaBits, meta);
    (*currentBits) += metaBits;

    varintBitstreamSet(d, *currentBits, valBits, writeIntoBitmap);
    (*currentBits) += valBits;

#if 0
    printf("writing (meta, data): (%d: %d, %d: %d)\n", metaBits, meta, valBits,
           dval);
    for (size_t i = 0; i < 10; i++) {
        printf("\t[0] %" PRIu64 "\n", d[i]);
    }
#endif
}

/* ====================================================================
 * Append new value to dod bitstream at offset 'currentBits'
 * ==================================================================== */
void dodAppend(dod *const d, const dodVal t0, const dodVal t1,
               const dodVal newVal, size_t *const currentBits) {
#if 0
    printf("Appending: %zd; %zd; %zd\n", (ssize_t)t0, (ssize_t)t1,
           (ssize_t)newVal);
#endif

    const int64_t dval = dodDelta(newVal, t1, t0);

#if 0
    printf("Conforming: %" PRId64 "\n", dval);
#endif

    dodWriteInteger(d, currentBits, dval);
}

/* ====================================================================
 * Friendly dod interface
 * ==================================================================== */

/* ====================================================================
 * Create new cannonical dod
 * ==================================================================== */
void dodInit(dodWriter *const w) {
    w->d = nullptr;
    w->t[0] = 0;
    w->t[1] = 0;
    w->count = 0;
    w->usedBits = 0;
    w->totalBytes = 0;
}

/* ====================================================================
 * Read from cannonical dod
 * ==================================================================== */
dodVal dodRead(dodWriter *const w, const size_t offset) {
    /* Fewer than two values: they live only in t[], no stream yet */
    if (w->count < 2) {
        return offset < w->count ? w->t[offset] : 0;
    }

    if (!w->d) {
        return 0;
    }

    /* The header holds values 0 and 1; entries follow it */
    dodVal t0;
    dodVal t1;
    size_t consumedBits = dodHeaderRead(w->d, &t0, &t1);
    if (offset == 0) {
        return t0;
    }

    if (offset == 1) {
        return t1;
    }

    return dodGet(w->d, &consumedBits, t0, t1, offset - 1);
}

/* ====================================================================
 * Write to cannonical dod
 * ==================================================================== */
/* Make 'w->d' big enough for 'extraBits' more bits after 'usedBits', plus
 * what a reader touches past the end: decoding the last entry reads its
 * 8-bit type, up to 7 bits beyond the stream, through whole 64-bit words.
 * Grows with zrealloc (at least doubling) and zeroes the new tail, since
 * zero entries are only skipped bits and rely on it. A NULL 'w->d' is
 * allocated here. */
static void dodWriterReserve(dodWriter *const w, const size_t extraBits) {
    const size_t endBits = w->usedBits + extraBits;
    const size_t needBytes = (((endBits + 6) / 64) + 1) * sizeof(dod);
    const size_t haveBytes = w->d ? w->totalBytes : 0;
    if (haveBytes >= needBytes) {
        return;
    }

    size_t newBytes = haveBytes * 2;
    if (newBytes < needBytes) {
        newBytes = needBytes;
    }

    /* @own: container; free=zfree */
    w->d = zrealloc(w->d, newBytes);
    memset((uint8_t *)w->d + haveBytes, 0, newBytes - haveBytes);
    w->totalBytes = newBytes;
}

void dodWrite(dodWriter *const w, const dodVal val) {
    if (w->count == 0) {
        /* First value: kept in t[0] until the second one arrives */
        w->t[0] = val;
    } else if (w->count == 1) {
        /* Second value: the stream starts with a header holding both */
        w->usedBits = 0;
        dodWriterReserve(w, DOD_WRITER_HEADER_MAX_BITS);
        w->t[1] = val;
        w->usedBits = dodHeaderWrite(w->d, w->t[0], val);
    } else {
        /* else, we can write to bitmap directly: one entry is at most
         * 72 bits */
        dodWriterReserve(w, 72);

        dodAppend(w->d, w->t[0], w->t[1], val, &w->usedBits);

        /* Rotate preconditions for next append */
        w->t[0] = w->t[1];
        w->t[1] = val;
    }

    /* New element added on this writer... */
    w->count++;
}

void dodCloseWrites(dodWriter *const w) {
    if (w->count >= 2) {
        /* t[1] already holds the last value; the first is in the header */
        dodVal second;
        dodHeaderRead(w->d, &w->t[0], &second);
    } else if (w->count == 1) {
        /* One value: it is both the first and the last */
        w->t[1] = w->t[0];
    }
}

/* ====================================================================
 * Get dod cached reader
 * ==================================================================== */
void dodInitFromExisting(dodWriter *const w, const dod *d) {
    /* The header holds the first two values; the stream does not record
     * how many follow, so 'count' covers just those two. */
    w->usedBits = dodHeaderRead(d, &w->t[0], &w->t[1]);
    w->count = 2;
}

/* NOTE: 'vals' must have length >= 2 because we ALWAYS write [0] AND [1] */
bool dodReadAll(const dod *const d, uint64_t *vals, size_t count) {
    if (!count) {
        return false;
    }

    /* We always write a minimum of two slots, so if count is 1,
     * make two just because. */
    if (count < 2) {
        count = 2;
    }

    dodVal t0;
    dodVal t1;
    size_t consumedBits = dodHeaderRead(d, &t0, &t1);

    vals[0] = (uint64_t)t0;
    vals[1] = (uint64_t)t1;

    for (size_t i = 2; i < count; i++) {
        const dodVal retrieved = dodGet(d, &consumedBits, t0, t1, 1);
        t0 = t1;
        t1 = retrieved;

        vals[i] = (uint64_t)retrieved;
    }

    return true;
}

/* ====================================================================
 * dodReader - O(1) resumable sequential access
 * ==================================================================== */

void dodReaderInit(dodReader *r, dodVal firstVal, dodVal secondVal) {
    r->consumedBits = 0;
    r->t0 = firstVal;
    r->t1 = secondVal;
    r->valuesRead = 2; /* We already have the first two values */
}

void dodReaderInitFromWriter(dodReader *r, const dodWriter *w) {
    if (w->count >= 2 && w->d) {
        /* Start after the header, from the first two values it holds. This
         * works for open and closed writers alike. */
        r->consumedBits = dodHeaderRead(w->d, &r->t0, &r->t1);
    } else {
        r->consumedBits = 0;
        r->t0 = w->t[0];
        r->t1 = w->t[1];
    }

    r->valuesRead = 2; /* We already have the first two values */
}

dodVal dodReaderNext(dodReader *r, const dod *d) {
    /* Read one value forward from current position */
    dodVal val = dodGet(d, &r->consumedBits, r->t0, r->t1, 1);

    /* Rotate state for next read */
    r->t0 = r->t1;
    r->t1 = val;
    r->valuesRead++;

    return val;
}

dodVal dodReaderCurrent(const dodReader *r) {
    return r->t1;
}

size_t dodReaderNextN(dodReader *r, const dod *d, dodVal *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        dodVal val = dodGet(d, &r->consumedBits, r->t0, r->t1, 1);
        out[i] = val;

        /* Rotate state for next read */
        r->t0 = r->t1;
        r->t1 = val;
        r->valuesRead++;
    }

    return n;
}

size_t dodReaderRemaining(const dodReader *r, size_t totalCount) {
    if (r->valuesRead >= totalCount) {
        return 0;
    }

    return totalCount - r->valuesRead;
}

/* ====================================================================
 * Tests
 * ==================================================================== */
#ifdef DATAKIT_TEST
#include "cleaner.h"
#include "ctest.h"

#define DOUBLE_NEWLINE 1
#include "perf.h"

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleanerName(dod *, zfree, Dods);
defcleanerName(int64_t *, zfree, I64);

/* multi-type, multi-data testing and validation */
static int testBits(const uint8_t typeBits, const uint8_t dataBits,
                    const int64_t boosterVal, const size_t loopers) {
    int err = 0;
    dodVal highest = 0;
    const uint8_t bitStorage = typeBits + dataBits;

    /* +1 below because it's a cheap "ceiling" operation to fix int division */
    cleanerName(zfree, Dods) dod *bits =
        zcalloc(((loopers * (typeBits + dataBits)) / 8) + 1, sizeof(*bits));

    TEST_DESC(
        "%d+%d bit storage - encode %zu, reconstruct %zu (offset: %" PRId64 ")",
        typeBits, dataBits, loopers, loopers, boosterVal) {
        size_t bitsUsed = 0;

        TEST_DESC("%d bit - encoding values", bitStorage) {
            dodVal t0 = 300;
            dodVal t1 = 400 + boosterVal;

            dodVal currentVal = 500 + boosterVal; /* 500 */

            PERF_TIMERS_SETUP;
            dodAppend(bits, t0, t1, currentVal, &bitsUsed);
            t0 = t1;
            t1 = currentVal;

            if (bitsUsed != bitStorage) {
                ERR("Expected %d bit used, but used %zu bits!", bitStorage,
                    bitsUsed);
                assert(false);
            }

            for (size_t i = 2; i <= loopers; i++) {
                currentVal += 100 + (boosterVal * ((i % 2) == 0 ? -1 : 1));
                dodAppend(bits, t0, t1, currentVal, &bitsUsed);
                t0 = t1;
                t1 = currentVal;

                if (bitsUsed != (i * bitStorage)) {
                    ERR("[%zu] Expected %zu bits used, but used %zu bits!", i,
                        i * bitStorage, bitsUsed);
                    assert(false);
                }
            }

            highest = currentVal;

            size_t expectedBitsUsed = (bitStorage)*loopers;
            if (bitsUsed != expectedBitsUsed) {
                ERR("Expected %zu bits used, but used %zu bits!",
                    expectedBitsUsed, bitsUsed);
            }

            PERF_TIMERS_FINISH_PRINT_RESULTS(loopers, "encode");
        }

        TEST_DESC("%d bit - reading end value from beginning (once)",
                  bitStorage) {
            PERF_TIMERS_SETUP;

            size_t consumedBits = 0;

            dodVal retrieved =
                dodGet(bits, &consumedBits, 300, 400 + boosterVal, loopers);

            if (retrieved != highest) {
                ERR("[] Expected %" PRIu64 " but got %" PRIu64 " instead!",
                    highest, retrieved);
            }
            PERF_TIMERS_FINISH_PRINT_RESULTS(1, "decode from beginning (once)");
        }

        if (loopers <= 20000) {
            TEST_DESC("%d bit - reading values from beginning (n^2 lookup "
                      "across entire dod)",
                      bitStorage) {
                PERF_TIMERS_SETUP;

                size_t consumedBits = 0;

                dodVal currentExpectedVal = 500 + boosterVal; /* 500 */
                dodVal retrieved =
                    dodGet(bits, &consumedBits, 300, 400 + boosterVal, 1);
                consumedBits = 0;

                if (retrieved != currentExpectedVal) {
                    ERR("[] Expected %" PRIu64 " but got %" PRIu64 " instead!",
                        currentExpectedVal, retrieved);
                }

                /* Now try to read them all back... */
                for (size_t i = 2; i <= loopers; i++) {
                    currentExpectedVal +=
                        100 + (boosterVal * ((i % 2) == 0 ? -1 : 1));
                    retrieved =
                        dodGet(bits, &consumedBits, 300, 400 + boosterVal, i);
                    consumedBits =
                        0; /* restart reading from beginning next time */

                    if (retrieved != currentExpectedVal) {
                        ERR("[%zu] Expected %" PRIu64 " but got %" PRIu64
                            " instead!",
                            i, currentExpectedVal, retrieved);
                    }
                }
                PERF_TIMERS_FINISH_PRINT_RESULTS(loopers,
                                                 "decode from beginning");
            }
        }

        TEST_DESC("%d bit - reading values while remembering position",
                  bitStorage) {
            PERF_TIMERS_SETUP;

            dodVal t0 = 300;
            dodVal t1 = 400 + boosterVal;

            size_t consumedBits = 0;

            dodVal currentExpectedVal = 500 + boosterVal; /* 500 */
            dodVal retrieved = dodGet(bits, &consumedBits, t0, t1, 1);
            t0 = t1;
            t1 = retrieved;

            if (retrieved != currentExpectedVal) {
                ERR("[] Expected %" PRIu64 " but got %" PRIu64 " instead!",
                    currentExpectedVal, retrieved);
            }

            /* Now try to read them all back... */
            for (size_t i = 2; i <= loopers; i++) {
                currentExpectedVal +=
                    100 + (boosterVal * ((i % 2) == 0 ? -1 : 1));
                retrieved = dodGet(bits, &consumedBits, t0, t1, 1);

                if (retrieved != currentExpectedVal) {
                    ERR("[%zu] Expected %" PRIu64 " but got %" PRIu64
                        " instead!",
                        i, currentExpectedVal, retrieved);
                }

                t0 = t1;
                t1 = retrieved;
            }
            PERF_TIMERS_FINISH_PRINT_RESULTS(loopers, "decode from end");
        }
    }

    return err;
}

/* Reference encoder, written from the format description rather than
 * from dodAppend(): the delta-of-delta is computed exactly in 128 bits
 * and reduced to its int64_t residue, the magnitude is kept in 128 bits,
 * and bits are written one at a time, most significant first. */
typedef struct dodModelCategory {
    uint8_t metaBits;
    uint8_t valBits;
    uint8_t code; /* positive designator; +1 is the negative one */
} dodModelCategory;

static const dodModelCategory dodModelCategories[] = {
    {3, 6, 0x04},  {4, 8, 0x0c},  {5, 11, 0x1c}, {8, 8, 0xf0},
    {8, 16, 0xf2}, {8, 24, 0xf4}, {8, 32, 0xf6}, {8, 40, 0xf8},
    {8, 48, 0xfa}, {8, 56, 0xfc}, {8, 64, 0xfe}};

static void dodModelPutBits(uint64_t *buf, size_t *pos, const uint32_t nbits,
                            const uint64_t val) {
    for (uint32_t b = nbits; b-- > 0;) {
        if ((val >> b) & 1) {
            buf[*pos / 64] |= 1ULL << (63 - (*pos % 64));
        }

        (*pos)++;
    }
}

static void dodModelAppend(uint64_t *buf, size_t *pos, const dodVal t0,
                           const dodVal t1, const dodVal tn) {
    const __int128 two63 = (__int128)1 << 63;
    const __int128 two64 = (__int128)1 << 64;
    __int128 r = ((__int128)tn - t1) - ((__int128)t1 - t0);
    while (r >= two63) {
        r -= two64;
    }

    while (r < -two63) {
        r += two64;
    }

    if (r == 0) {
        (*pos)++; /* a single zero bit */
        return;
    }

    const __int128 mag = r < 0 ? -r : r;
    __int128 base = 1; /* smallest magnitude of the current category */
    for (size_t k = 0; k < countof(dodModelCategories); k++) {
        const dodModelCategory *c = &dodModelCategories[k];
        const __int128 span = (__int128)1 << c->valBits;
        if (mag < base + span) {
            dodModelPutBits(buf, pos, c->metaBits, c->code + (r < 0 ? 1 : 0));
            dodModelPutBits(buf, pos, c->valBits, (uint64_t)(mag - base));
            return;
        }

        base += span;
    }

    assert(false && "magnitude outside every category");
}

/* Encode 'values' with dodAppend() and with the reference model, require
 * identical bitstreams, then decode with dodGet(), dodReader and the
 * absolute-offset dodGet() form and require the original values back. */
static int dodCheckRoundTrip(const char *what, const dodVal *values,
                             const size_t count) {
    int err = 0;
    assert(count >= 2);

    /* 72 bits per value worst case, plus a slot of slack for the
     * two-slot split write at the end */
    const size_t slots = ((count * 72) / 64) + 2;
    cleanerName(zfree, Dods) dod *bits = zcalloc(slots, sizeof(*bits));
    cleanerName(zfree, Dods) uint64_t *model = zcalloc(slots, sizeof(*model));

    size_t bitsUsed = 0;
    size_t modelBits = 0;
    for (size_t i = 2; i < count; i++) {
        dodAppend(bits, values[i - 2], values[i - 1], values[i], &bitsUsed);
        dodModelAppend(model, &modelBits, values[i - 2], values[i - 1],
                       values[i]);
    }

    if (bitsUsed != modelBits) {
        ERR("%s: dodAppend used %zu bits but the model used %zu", what,
            bitsUsed, modelBits);
    }

    for (size_t s = 0; s < slots; s++) {
        if (bits[s] != model[s]) {
            ERR("%s: slot %zu is 0x%016" PRIx64 " but the model wrote "
                "0x%016" PRIx64,
                what, s, bits[s], model[s]);
            break;
        }
    }

    dodReader r;
    dodReaderInit(&r, values[0], values[1]);
    for (size_t i = 2; i < count; i++) {
        const dodVal got = dodReaderNext(&r, bits);
        if (got != values[i]) {
            ERR("%s: [%zu] dodReaderNext gave %" PRId64 ", expected %" PRId64,
                what, i, got, values[i]);
            break;
        }
    }

    if (r.consumedBits != bitsUsed) {
        ERR("%s: reader consumed %zu bits, writer used %zu", what,
            r.consumedBits, bitsUsed);
    }

    /* dodGet() with an absolute offset replays from the start */
    size_t consumedBits = 0;
    const dodVal last =
        dodGet(bits, &consumedBits, values[0], values[1], count - 2);
    if (last != values[count - 1]) {
        ERR("%s: dodGet(offset %zu) gave %" PRId64 ", expected %" PRId64, what,
            count - 2, last, values[count - 1]);
    }

    return err;
}

/* Build a sequence whose delta-of-deltas are exactly 'dods' (modulo 2^64),
 * starting from v0 and v1. */
static void dodSequenceFromDods(dodVal *out, const dodVal v0, const dodVal v1,
                                const int64_t *dods, const size_t ndods) {
    out[0] = v0;
    out[1] = v1;
    for (size_t i = 0; i < ndods; i++) {
        const uint64_t prev = (uint64_t)out[i + 1];
        const uint64_t delta = prev - (uint64_t)out[i];
        out[i + 2] = (dodVal)(prev + delta + (uint64_t)dods[i]);
    }
}

/* Write 'vals' through a dodWriter, then read every value back through
 * every reader of writer streams: dodRead() and dodReaderInitFromWriter()
 * on the open and the closed writer, dodInitFromExisting() plus dodGet()
 * (absolute and sequential), and dodReadAll(). 'expectedCode' is the header
 * code the first two values must select (-1: don't check). */
static void dodTestWriterFree_cleanup(dodWriter *w) {
    zfree(w->d);
}

static int dodCheckWriterStream(const char *what, const dodVal *vals,
                                const size_t n, const int expectedCode,
                                const size_t initialBytes) {
    int err = 0;

    /* The writer owns its stream: NULL (initialBytes 0) or a zeroed
     * preallocation, grown by dodWrite() as needed */
    cleaner(dodTestWriterFree) dodWriter w;
    dodInit(&w);
    if (initialBytes) {
        /* @own: container; free=zfree */
        w.d = zcalloc(1, initialBytes);
        w.totalBytes = initialBytes;
    }

    for (size_t i = 0; i < n; i++) {
        dodWrite(&w, vals[i]);
    }

    const dod *const stream = w.d;

    if (w.count != n) {
        ERR("%s: count %zu after %zu writes", what, w.count, n);
    }

    /* Room for everything a reader touches: whole words through the 8-bit
     * type read at the last entry */
    if (n >= 2 && w.totalBytes < (((w.usedBits + 6) / 64) + 1) * sizeof(dod)) {
        ERR("%s: %zu bytes for %zu bits", what, w.totalBytes, w.usedBits);
    }

    if (n >= 2 && expectedCode >= 0 &&
        varintBitstreamGet(stream, 0, 2) != (uint64_t)expectedCode) {
        ERR("%s: header code %" PRIu64 ", expected %d", what,
            varintBitstreamGet(stream, 0, 2), expectedCode);
    }

    for (int closed = 0; closed < 2; closed++) {
        if (closed) {
            dodCloseWrites(&w);
            if (n >= 1 && (w.t[0] != vals[0] || w.t[1] != vals[n - 1])) {
                ERR("%s: closed t = {%" PRId64 ", %" PRId64
                    "}, expected first/last {%" PRId64 ", %" PRId64 "}",
                    what, w.t[0], w.t[1], vals[0], vals[n - 1]);
            }
        }

        for (size_t i = 0; i < n; i++) {
            const dodVal got = dodRead(&w, i);
            if (got != vals[i]) {
                ERR("%s (%s): dodRead(%zu) = %" PRId64 ", expected %" PRId64,
                    what, closed ? "closed" : "open", i, got, vals[i]);
                break;
            }
        }

        if (n >= 2) {
            dodReader r;
            dodReaderInitFromWriter(&r, &w);
            if (r.t0 != vals[0] || r.t1 != vals[1]) {
                ERR("%s (%s): reader starts at {%" PRId64 ", %" PRId64 "}",
                    what, closed ? "closed" : "open", r.t0, r.t1);
            }

            for (size_t i = 2; i < n; i++) {
                const dodVal got = dodReaderNext(&r, stream);
                if (got != vals[i]) {
                    ERR("%s (%s): reader [%zu] = %" PRId64
                        ", expected %" PRId64,
                        what, closed ? "closed" : "open", i, got, vals[i]);
                    break;
                }
            }

            if (dodReaderRemaining(&r, n) != 0 ||
                r.consumedBits != w.usedBits) {
                ERR("%s (%s): reader ended at bit %zu, writer at %zu", what,
                    closed ? "closed" : "open", r.consumedBits, w.usedBits);
            }
        }
    }

    if (n >= 2) {
        /* Reopen from the stream alone */
        dodWriter r;
        memset(&r, 0xA5, sizeof(r));
        dodInitFromExisting(&r, stream);
        if (r.t[0] != vals[0] || r.t[1] != vals[1] || r.count != 2) {
            ERR("%s: reopened t = {%" PRId64 ", %" PRId64 "} count %zu", what,
                r.t[0], r.t[1], r.count);
        }

        /* dodGet() from the header's end: absolute offsets... */
        for (size_t k = 1; k + 1 < n; k++) {
            size_t bits = r.usedBits;
            const dodVal got = dodGet(stream, &bits, r.t[0], r.t[1], k);
            if (got != vals[k + 1]) {
                ERR("%s: dodGet(offset %zu) = %" PRId64 ", expected %" PRId64,
                    what, k, got, vals[k + 1]);
                break;
            }
        }

        /* ...and one value at a time */
        size_t bits = r.usedBits;
        dodVal t0 = r.t[0];
        dodVal t1 = r.t[1];
        for (size_t i = 2; i < n; i++) {
            const dodVal got = dodGet(stream, &bits, t0, t1, 1);
            if (got != vals[i]) {
                ERR("%s: sequential dodGet [%zu] = %" PRId64
                    ", expected %" PRId64,
                    what, i, got, vals[i]);
                break;
            }

            t0 = t1;
            t1 = got;
        }

        if (bits != w.usedBits) {
            ERR("%s: decoding ended at bit %zu, writer at %zu", what, bits,
                w.usedBits);
        }

        cleanerName(zfree, Dods) uint64_t *all = zcalloc(n, sizeof(*all));
        if (!dodReadAll(stream, all, n)) {
            ERR("%s: dodReadAll refused %zu values", what, n);
        }

        for (size_t i = 0; i < n; i++) {
            if (all[i] != (uint64_t)vals[i]) {
                ERR("%s: dodReadAll [%zu] = %" PRId64 ", expected %" PRId64,
                    what, i, (dodVal)all[i], vals[i]);
                break;
            }
        }
    }

    return err;
}

static uint64_t dodTestRand(uint64_t *state) {
    /* splitmix64 */
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

int dodTest(int, char *[]) {
    int err = 0;
    const size_t loopers = 10000;

#if 1
    TEST("basic bitstream set/get") {
        size_t usedBits = 0;

        uint64_t bits[1024] = {}; /* 64k bits */

        for (size_t i = 1; i < 64; i++) {
            varintBitstreamSet(bits, usedBits, i, i);
            uint64_t retrieved = varintBitstreamGet(bits, usedBits, i);
            usedBits += i;

            if (retrieved != i) {
                ERR("[%zu] expected %zu but got %" PRIu64 " instead!", i, i,
                    retrieved);
            }
        }
    }

    TEST("variable length bitstream set/get") {
        size_t usedBits = 0;

        uint64_t bits[1024] = {}; /* 64k bits */
        uint8_t lengths[64] = {
            2,  3,  5,  7,  9,  4,  6,  12, 11, 28, 14, 32, 15, 18, 8,  22,
            18, 19, 20, 21, 22, 23, 24, 7,  9,  6,  18, 10, 11, 12, 24, 9,
            11, 28, 14, 32, 15, 18, 20, 22, 18, 19, 20, 21, 22, 23, 24, 7,
            9,  6,  18, 10, 11, 12, 24, 18, 19, 20, 21, 22, 23, 24, 7,  6};

        for (size_t i = 1; i < 64; i++) {
            varintBitstreamSet(bits, usedBits, lengths[i], i);
            uint64_t retrieved = varintBitstreamGet(bits, usedBits, lengths[i]);
            usedBits += lengths[i];

            if (retrieved != i) {
                ERR("[%zu] expected %zu but got %" PRIu64 " instead!", i, i,
                    retrieved);
            }
        }
    }

    TEST("given bitstream set/get") {
        uint64_t bits[1] = {0xA5ULL << (64 - 8)}; /* 10100101 */

        uint8_t retrieved = varintBitstreamGet(bits, 0, 8);
        assert(retrieved == (uint8_t)0xA5);

        retrieved = varintBitstreamGet(bits, 0, 3);
        assert(retrieved == 5);

        retrieved = varintBitstreamGet(bits, 3, 5);
        assert(retrieved == 5);
    }

    //    for (int boosterI = -1; boosterI < 2; boosterI++) {
    TEST_DESC("verify integer encoding works at powers of two (%d)",
              1) { // boosterI) {
        cleanerName(zfree, Dods) dod *bits = zcalloc(loopers, sizeof(*bits));
        cleanerName(zfree, I64) int64_t *values =
            zcalloc(loopers * 2, sizeof(*values));

        size_t currentBits = 0;
        for (uint64_t powers = 1, i = 0; i < loopers && powers < (1ULL << 63);
             powers *= 2, i++) {
            if (powers == 0) {
                /* we looped to the end, so start over. */
                powers = 1;
            }

            values[i] = (int64_t)powers; // + (int64_t)boosterI;
            dodWriteInteger(bits, &currentBits, values[i]);
            assert(currentBits > 0);

            int64_t got = dodGetIntegerAtOffset(bits, i + 1);
            if (got != values[i]) {
                ERR("[%" PRIu64 "] expected %" PRIu64 " but got %" PRIu64
                    " instead!",
                    i, values[i], got);
                assert(false);
            }
        }

        for (size_t i = 0; i < loopers; i++) {
            int64_t got = dodGetIntegerAtOffset(bits, i + 1);

            if (values[i] != got) {
                ERR("[%zu] Expected %" PRIu64 " but got %" PRIu64 "\n", i,
                    values[i], got);
            }
        }
    }
    //    }

    for (size_t j = 0; j < 2; j++) {
        TEST_DESC("verify integer encoding works at big (%s) sequential range",
                  j == 0 ? "+" : "-") {
            int32_t modifier = j == 0 ? 1 : -1;
            const int64_t localMax = 1 << 10;
            const size_t localLoopers = 10000;
            cleanerName(zfree, Dods) dod *bits =
                zcalloc(localLoopers * 2, sizeof(*bits));

            size_t currentBits = 0;
            for (int64_t tracker = 0, i = 0; tracker <= localMax;
                 tracker++, i++) {
                if (tracker % localLoopers == 0) {
                    /* use 'bits' as a circular buffer so we don't need to
                     * allocate jiggabytes for testing */
                    currentBits = 0;
                    i = 0;
                    printf(".");
                    fflush(stdout);
                }

                dodWriteInteger(bits, &currentBits, tracker * modifier);
                assert(currentBits > 0);

                const int64_t got = dodGetIntegerAtOffset(bits, i + 1);

                if (j == 0) {
                    assert(got >= 0);
                } else {
                    assert(got <= 0);
                }

                if (got != tracker * modifier) {
                    ERR("[%" PRId64 "] Expected %" PRId64 " but got %" PRIu64
                        " instead!",
                        tracker, tracker * modifier, got);
                }
            }
            printf("\n");
        }
    }
#endif

#if 1
    err += testBits(0, 1, 0, loopers);
    err += testBits(2, 7, MAX_0 * 2, loopers);
    err += testBits(3, 9, MAX_7 * 2, loopers);
    err += testBits(4, 12, MAX_9 * 2, loopers);
    err += testBits(8, 8, 1200, loopers);
    err += testBits(8, 16, 8000, loopers);
    err += testBits(8, 24, 1048576, loopers);                /* 2^20 */
    err += testBits(8, 32, 1073741824, loopers);             /* 2^30 */
    err += testBits(8, 40, 34359738368ULL, loopers);         /* 2^35 */
    err += testBits(8, 48, 35184372088832ULL, loopers);      /* 2^45 */
    err += testBits(8, 56, 36028797018963968ULL, loopers);   /* 2^55 */
    err += testBits(8, 64, 1152921504606846976ULL, loopers); /* 2^60 */
#if 0
    err += testBits(8, 64, (1ULL << 63) - 1, loopers); /* 2^63 - 1 */
    err += testBits(8, 64, -(1ULL << 63), loopers); /* -2^63 */
#endif
#endif

    for (size_t loop = 1; loop <= 2; loop++) {
        TEST_DESC("randomized testing (%s offsets)",
                  loop == 1 ? "random" : "powers of two") {
            cleanerName(zfree, I64) dodVal *values =
                zcalloc(loopers, sizeof(*values));
            cleanerName(zfree, Dods) dod *bits =
                zcalloc(loopers * 2, sizeof(*bits));

            dodVal previousVal = 0;
            if (loop == 1) {
                for (size_t i = 0; i < loopers; i++) {
                    values[i] =
                        previousVal +
                        (rand() % 1209600); /* clamp the values, somewhat */
                    previousVal = values[i];
                }
            } else {
                for (uint64_t powers = 1, i = 0;
                     i < loopers && powers < (1ULL << 63); powers *= 2, i++) {
                    if (powers == 0) {
                        /* we looped to the end, so start over. */
                        powers = 1;
                    }
                    values[i] =
                        previousVal +
                        (int64_t)powers * (int64_t)(rand() % 7 == 0 ? -1 : 1);
                    previousVal = values[i];
                }
            }

            dodVal t0 = values[0];
            dodVal t1 = values[1];

            size_t bitsUsed = 0;
            for (size_t i = 2; i < loopers; i++) {
                dodAppend(bits, t0, t1, values[i], &bitsUsed);
                t0 = t1;
                t1 = values[i];
            }

            printf("Used %zu bits in random test!\n", bitsUsed);

            t0 = values[0];
            t1 = values[1];

            size_t consumedBits = 0;
            for (size_t i = 2; i < loopers; i++) {
                dodVal retrieved = dodGet(bits, &consumedBits, t0, t1, 1);
                t0 = t1;
                t1 = retrieved;

                if (consumedBits > bitsUsed) {
                    ERR("Read more bits than written! Wrote %zu bits, read %zu "
                        "bits!\n",
                        bitsUsed, consumedBits);
                }

                if (retrieved != values[i]) {
                    ERR("[%zu] Expected %" PRIu64 " but got %" PRIu64
                        " instead!\n",
                        i, values[i], retrieved);
                }
            }
        }
    }

    TEST("full int64 range: extremes round trip exactly") {
        static const dodVal altMinMax[] = {INT64_MIN, INT64_MAX, INT64_MIN,
                                           INT64_MAX, INT64_MIN, INT64_MAX,
                                           INT64_MIN, INT64_MAX};
        static const dodVal altMaxMin[] = {INT64_MAX, INT64_MIN, INT64_MAX,
                                           INT64_MIN, INT64_MAX, INT64_MIN,
                                           INT64_MAX, INT64_MIN};
        static const dodVal mixed[] = {
            0,         INT64_MAX,     INT64_MIN,     0,
            -1,        INT64_MIN,     INT64_MAX,     1,
            INT64_MIN, INT64_MIN,     INT64_MAX,     INT64_MAX,
            0,         INT64_MIN + 1, INT64_MAX - 1, INT64_MIN};
        static const dodVal constMin[] = {INT64_MIN, INT64_MIN, INT64_MIN,
                                          INT64_MIN, INT64_MIN};
        static const dodVal constMax[] = {INT64_MAX, INT64_MAX, INT64_MAX,
                                          INT64_MAX, INT64_MAX};
        static const dodVal wrapRamp[] = {INT64_MAX - 2, INT64_MAX - 1,
                                          INT64_MAX,     INT64_MIN,
                                          INT64_MIN + 1, INT64_MIN + 2};
        /* deltas of +2^62 then -6755390026418175967: a true
         * delta-of-delta below INT64_MIN */
        static const dodVal bigDeltas[] = {0, 4611686018427387904LL,
                                           -2143704007990788063LL,
                                           7079668028863987745LL, INT64_MIN};

        err += dodCheckRoundTrip("alternating MIN/MAX", altMinMax,
                                 countof(altMinMax));
        err += dodCheckRoundTrip("alternating MAX/MIN", altMaxMin,
                                 countof(altMaxMin));
        err += dodCheckRoundTrip("mixed extremes", mixed, countof(mixed));
        err += dodCheckRoundTrip("constant MIN", constMin, countof(constMin));
        err += dodCheckRoundTrip("constant MAX", constMax, countof(constMax));
        err += dodCheckRoundTrip("ramp across the wrap", wrapRamp,
                                 countof(wrapRamp));
        err += dodCheckRoundTrip("deltas past int64", bigDeltas,
                                 countof(bigDeltas));
    }

    TEST("full int64 range: every category boundary and the int64 ends") {
        int64_t dods[128];
        size_t ndods = 0;

        /* Category k holds magnitudes [base_k, base_k + 2^valBits) */
        int64_t base = 1;
        for (size_t k = 0; k + 1 < countof(dodModelCategories); k++) {
            const int64_t edges[] = {base - 1, base, base + 1};
            for (size_t e = 0; e < countof(edges); e++) {
                dods[ndods++] = edges[e];
                dods[ndods++] = -edges[e];
            }

            base += (int64_t)1 << dodModelCategories[k].valBits;
        }

        /* 'base' is now where the 64-bit category starts */
        const int64_t ends[] = {base - 1,      base,          base + 1,
                                -(base - 1),   -base,         -(base + 1),
                                INT64_MAX,     INT64_MIN,     INT64_MAX - 1,
                                INT64_MIN + 1, INT64_MIN + 2, 0};
        for (size_t e = 0; e < countof(ends); e++) {
            dods[ndods++] = ends[e];
        }

        assert(ndods + 2 <= countof(dods));

        const dodVal starts[][2] = {{0, 0},
                                    {INT64_MAX, INT64_MAX},
                                    {INT64_MIN, INT64_MIN},
                                    {INT64_MIN, INT64_MAX},
                                    {1700000000000LL, 1700000001000LL}};
        dodVal seq[130];
        for (size_t s = 0; s < countof(starts); s++) {
            dodSequenceFromDods(seq, starts[s][0], starts[s][1], dods, ndods);

            /* The generated sequence must carry exactly these dods */
            for (size_t i = 0; i < ndods; i++) {
                const int64_t got = dodDelta(seq[i + 2], seq[i + 1], seq[i]);
                if (got != dods[i]) {
                    ERR("start %zu: dod[%zu] is %" PRId64 ", built %" PRId64, s,
                        i, got, dods[i]);
                }
            }

            err += dodCheckRoundTrip("category boundaries", seq, ndods + 2);
        }
    }

    TEST("full int64 range: random differential against the model") {
        uint64_t rng = 0x243f6a8885a308d3ULL;
        const size_t count = 4096;
        cleanerName(zfree, I64) dodVal *values =
            zcalloc(count, sizeof(*values));

        for (size_t round = 0; round < 8; round++) {
            for (size_t i = 0; i < count; i++) {
                const uint64_t pick = dodTestRand(&rng);
                const uint64_t prev = i ? (uint64_t)values[i - 1] : 0;
                switch (pick % 5) {
                case 0: /* any int64 */
                    values[i] = (dodVal)dodTestRand(&rng);
                    break;
                case 1: { /* an int64 end or its neighbour */
                    static const dodVal edge[] = {INT64_MIN, INT64_MIN + 1,
                                                  INT64_MAX, INT64_MAX - 1,
                                                  0,         -1};
                    values[i] = edge[(pick >> 8) % countof(edge)];
                    break;
                }
                case 2: /* small step, wrapping */
                    values[i] = (dodVal)(prev + ((pick >> 8) % 4096) - 2048);
                    break;
                case 3: { /* step of a random width, wrapping */
                    const uint32_t width = (uint32_t)((pick >> 8) % 64);
                    const uint64_t step =
                        dodTestRand(&rng) & (~0ULL >> (63 - width));
                    values[i] =
                        (dodVal)((pick >> 16) & 1 ? prev + step : prev - step);
                    break;
                }
                default: /* repeat the last step: zero dod */
                    values[i] =
                        i >= 2
                            ? (dodVal)(prev + (prev - (uint64_t)values[i - 2]))
                            : (dodVal)prev;
                    break;
                }
            }

            err += dodCheckRoundTrip("random differential", values, count);
        }
    }

    TEST("dodInit sets every writer field") {
        /* Poison the struct first so a field dodInit skips reads as 0xA5 */
        dodWriter w;
        memset(&w, 0xA5, sizeof(w));
        dodInit(&w);

        if (w.d != nullptr || w.t[0] != 0 || w.t[1] != 0 || w.count != 0 ||
            w.usedBits != 0 || w.totalBytes != 0) {
            ERR("dodInit left a field unset: d=%p t={%" PRId64 ", %" PRId64
                "} count=%zu usedBits=%zu totalBytes=%zu",
                (void *)w.d, w.t[0], w.t[1], w.count, w.usedBits, w.totalBytes);
        }

        /* The first two values go to t[] and are counted from zero; the
         * second allocates the stream */
        dodWrite(&w, 11);
        dodWrite(&w, 22);
        if (w.count != 2 || w.t[0] != 11 || w.t[1] != 22 || !w.d) {
            ERR("fresh writer after two writes: count=%zu t={%" PRId64
                ", %" PRId64 "} d=%p",
                w.count, w.t[0], w.t[1], (void *)w.d);
        }

        zfree(w.d);
    }

    TEST("writer streams: write, close, reopen and read back") {
        /* Each pair of first values selects one header code */
        const struct {
            dodVal first;
            dodVal second;
            int code;
        } pairs[] = {
            {100, 150, DOD_HEADER_T0_32_DELTA_14},
            {0, 8191, DOD_HEADER_T0_32_DELTA_14},
            {UINT32_MAX, (dodVal)UINT32_MAX - 8191, DOD_HEADER_T0_32_DELTA_14},
            {100, 100 + 8192, DOD_HEADER_T0_32_DELTA_16},
            {5, 5 - 32767, DOD_HEADER_T0_32_DELTA_16},
            {-1, 5, DOD_HEADER_T0_64_DELTA_45},
            {(dodVal)UINT32_MAX + 1, (dodVal)UINT32_MAX + 2,
             DOD_HEADER_T0_64_DELTA_45},
            {100, 100 + 32768, DOD_HEADER_T0_64_DELTA_45},
            {0, (1LL << 44) - 1, DOD_HEADER_T0_64_DELTA_45},
            {0, -((1LL << 44) - 1), DOD_HEADER_T0_64_DELTA_45},
            {INT64_MIN, INT64_MIN, DOD_HEADER_T0_64_DELTA_45},
            /* INT64_MIN - INT64_MAX is 1 modulo 2^64 */
            {INT64_MAX, INT64_MIN, DOD_HEADER_T0_64_DELTA_45},
            {0, 1LL << 44, DOD_HEADER_T0_64_T1_64},
            {0, -(1LL << 44), DOD_HEADER_T0_64_T1_64},
            /* INT64_MAX - INT64_MIN is -1 modulo 2^64 */
            {INT64_MIN, INT64_MAX, DOD_HEADER_T0_64_DELTA_45},
            {INT64_MIN, 0, DOD_HEADER_T0_64_T1_64},
            {0, INT64_MIN, DOD_HEADER_T0_64_T1_64},
            {INT64_MAX, -1, DOD_HEADER_T0_64_T1_64},
            {1700000000000LL, 1700000001000LL, DOD_HEADER_T0_64_DELTA_45},
        };
        const size_t lengths[] = {1, 2, 3, 4, 40};

        uint64_t rng = 0x13198a2e03707344ULL;
        dodVal vals[40];
        for (size_t p = 0; p < countof(pairs); p++) {
            for (size_t l = 0; l < countof(lengths); l++) {
                const size_t n = lengths[l];
                vals[0] = pairs[p].first;
                vals[1] = pairs[p].second;
                for (size_t i = 2; i < n; i++) {
                    const uint64_t pick = dodTestRand(&rng);
                    const uint64_t prev = (uint64_t)vals[i - 1];
                    const uint64_t step = prev - (uint64_t)vals[i - 2];
                    /* Mostly regular steps, some jitter, some anything */
                    vals[i] = pick % 4 == 0 ? (dodVal)dodTestRand(&rng)
                              : pick % 4 == 1
                                  ? (dodVal)(prev + step + (pick >> 60))
                                  : (dodVal)(prev + step);
                }

                /* Writer-allocated, tiny and roomy preallocations */
                const size_t initialBytes[] = {0, 8, 4096};
                err += dodCheckWriterStream("writer stream", vals, n,
                                            pairs[p].code,
                                            initialBytes[(p + l) % 3]);
            }
        }

        /* A writer with no values reads 0 and closes to zeros */
        dodWriter empty;
        dodInit(&empty);
        dodCloseWrites(&empty);
        if (dodRead(&empty, 0) != 0 || empty.t[0] != 0 || empty.t[1] != 0) {
            ERR("empty writer did not stay empty");
        }
    }

    TEST("writer streams grow well past their initial capacity") {
        /* Worst-case entries (72 bits each) from a 16-byte preallocation
         * and from no allocation at all; every read path then runs over
         * the grown stream, so ASan sees any write or read past it */
        const size_t n = 3000;
        cleanerName(zfree, I64) dodVal *vals = zcalloc(n, sizeof(*vals));
        uint64_t rng = 0xa4093822299f31d0ULL;
        for (size_t i = 0; i < n; i++) {
            vals[i] = (dodVal)dodTestRand(&rng);
        }

        err += dodCheckWriterStream("grown from 16 bytes", vals, n, -1, 16);
        err += dodCheckWriterStream("grown from NULL", vals, n, -1, 0);

        /* Regular timestamps: one bit per entry, many entries per word */
        for (size_t i = 0; i < n; i++) {
            vals[i] = 1700000000000LL + (dodVal)i * 1000;
        }

        err += dodCheckWriterStream("grown, regular", vals, n, -1, 8);
    }

    TEST("dodReader - O(1) sequential access") {
        cleanerName(zfree, I64) dodVal *values =
            zcalloc(loopers, sizeof(*values));
        cleanerName(zfree, Dods) dod *bits =
            zcalloc(loopers * 2, sizeof(*bits));

        /* Generate monotonic timestamps */
        for (size_t i = 0; i < loopers; i++) {
            values[i] = 1700000000000LL + (int64_t)i * 1000;
        }

        /* Encode */
        dodVal t0 = values[0];
        dodVal t1 = values[1];
        size_t bitsUsed = 0;
        for (size_t i = 2; i < loopers; i++) {
            dodAppend(bits, t0, t1, values[i], &bitsUsed);
            t0 = t1;
            t1 = values[i];
        }

        /* Test dodReaderNext() */
        dodReader r;
        dodReaderInit(&r, values[0], values[1]);

        /* Verify first two values via dodReaderCurrent */
        if (dodReaderCurrent(&r) != values[1]) {
            ERR("dodReaderCurrent returned wrong initial value!");
        }

        /* Read remaining values */
        for (size_t i = 2; i < loopers; i++) {
            dodVal retrieved = dodReaderNext(&r, bits);
            if (retrieved != values[i]) {
                ERR("[%zu] dodReaderNext: expected %" PRId64
                    " but got %" PRId64,
                    i, values[i], retrieved);
            }
        }

        /* Test dodReaderRemaining */
        if (dodReaderRemaining(&r, loopers) != 0) {
            ERR("dodReaderRemaining should be 0 after reading all values!");
        }

        /* Test dodReaderNextN - encode and read again */
        dodReaderInit(&r, values[0], values[1]);
        cleanerName(zfree, I64) dodVal *batch =
            zcalloc(loopers, sizeof(*batch));
        size_t read = dodReaderNextN(&r, bits, batch, loopers - 2);
        if (read != loopers - 2) {
            ERR("dodReaderNextN returned wrong count: %zu vs %zu", read,
                loopers - 2);
        }
        for (size_t i = 0; i < read; i++) {
            if (batch[i] != values[i + 2]) {
                ERR("[%zu] dodReaderNextN: expected %" PRId64
                    " but got %" PRId64,
                    i, values[i + 2], batch[i]);
            }
        }

        printf("dodReader passed with %zu values!\n", loopers);
    }

    TEST_FINAL_RESULT;
}
#endif

/* Additionally, this is how the dispatch table was generated: */
#if 0
#!/ usr / bin / env python

import itertools

t = []

for f in  ["".join(seq) for seq in itertools.product("01", repeat=8)]:
    i = int(f, 2)
    if f.startswith("0"):
        t.append((i, "Zero"))
    elif f.startswith("100"):
        t.append((i, "Seven"))
    elif f.startswith("101"):
        t.append((i, "SevenNegative"))
    elif f.startswith("1100"):
        t.append((i, "Nine"))
    elif f.startswith("1101"):
        t.append((i, "NineNegative"))
    elif f.startswith("11100"):
        t.append((i, "Twelve"))
    elif f.startswith("11101"):
        t.append((i, "TwelveNegative"))
    elif f.startswith("11110000"):
        t.append((i, "Variable8"))
    elif f.startswith("11110001"):
        t.append((i, "Variable8Negative"))
    elif f.startswith("11110010"):
        t.append((i, "Variable16"))
    elif f.startswith("11110011"):
        t.append((i, "Variable16Negative"))
    elif f.startswith("11110100"):
        t.append((i, "Variable24"))
    elif f.startswith("11110101"):
        t.append((i, "Variable24Negative"))
    elif f.startswith("11110110"):
        t.append((i, "Variable32"))
    elif f.startswith("11110111"):
        t.append((i, "Variable32Negative"))
    elif f.startswith("11111000"):
        t.append((i, "Variable40"))
    elif f.startswith("11111001"):
        t.append((i, "Variable40Negative"))
    elif f.startswith("11111010"):
        t.append((i, "Variable48"))
    elif f.startswith("11111011"):
        t.append((i, "Variable48Negative"))
    elif f.startswith("11111100"):
        t.append((i, "Variable56"))
    elif f.startswith("11111101"):
        t.append((i, "Variable56Negative"))
    elif f.startswith("11111110"):
        t.append((i, "Variable64"))
    elif f.startswith("11111111"):
        t.append((i, "Variable64Negative"))
    else:
        print "NO MATCH FOR", f, i

sortedByIndex = sorted(t, key=lambda tup: tup[0])

finalDispatchTable = []
finalDispatchTableNoZero = []
for f in sortedByIndex:
    finalDispatchTable.append("&&" + f[1])
    if (f[0] > 127):
        finalDispatchTableNoZero.append("&&" + f[1])

print "static const void *dispatch[] = {" + ", ".join(finalDispatchTable) + "};"
print
print "static const void *dispatchNoZero[] = {" + ", ".join(finalDispatchTableNoZero) + "};"
#endif
