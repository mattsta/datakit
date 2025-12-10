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

typedef enum dodFloor {
    DOD_FLOOR_SEC_HR_4 = 0,  /* 32 bit t0, 14 bit t1 */
    DOD_FLOOR_SEC_HR_12 = 1, /* 32 bit t0, 16 bit t1 */
    DOD_FLOOR_NS_HR_4 = 2,   /* 64 bit t0, 45 bit t1 */
    DOD_FLOOR_NS_HR_12 = 3   /* 64 bit t0, 46 bit t1 */
    /* don't add more; we only use two bits for floor storage */
} dodFloor;

/* ====================================================================
 * Give varintBitstream our types for the bitstream
 * ==================================================================== */
#define VBITS dod
#define VBITSVAL dod
#include "../deps/varint/src/varintBitstream.h"

/* ====================================================================
 * Extended helper values for stacked ranges
 * ==================================================================== */

#ifndef COUNT_ARRAY
#define COUNT_ARRAY(x) (sizeof(x) / sizeof(*(x)))
#endif

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
typedef enum __attribute__((packed)) dodType {
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

_Static_assert(sizeof(dodType) == 1, "dodType not packed?");

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

_Static_assert(sizeof(dodFormat) == 4, "dodFormat unnecessarily big?");

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
#define delta(t_n, t_n_1, t_n_0) (((t_n) - (t_n_1)) - ((t_n_1) - (t_n_0)))
//    (((int64_t)(t_n) - (t_n_1)) - ((int64_t)(t_n_1) - (t_n_2)))
//    ((int64_t)((t_n) - (t_n_1)) - (int64_t)((t_n_1) - (t_n_2)))

/* The delta calculation was this, but it caused signed overflows on
 * subtraction with big numbers:
 *      ((int64_t)((t_n) - (t_n_1)) - (int64_t)((t_n_1) - (t_n_2)))
 */

#define undelta(delta_, t_n_1, t_n_2)                                          \
    (((delta_) + (t_n_1)) + ((t_n_1) - (t_n_2)))

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

#define UNDELTA(delta) undelta(delta, t1, t0)

#define UNDELTA_BITS_BOOST(boost, valBits)                                     \
    UNDELTA(((int64_t)boost) +                                                 \
            (int64_t)varintBitstreamGet(d, *consumedBits, valBits))

#define UNDELTA_BITS_BOOST_NEGATIVE(boost, valBits)                            \
    UNDELTA(-(((int64_t)boost) +                                               \
              (int64_t)varintBitstreamGet(d, *consumedBits, valBits)))

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

    __builtin_unreachable();
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
        currentVal =                                                           \
            boost + (int64_t)varintBitstreamGet(d, consumedBits, dataBits);    \
        consumedBits += (dataBits); /* jump over data bits */                  \
    } while (0)

#define ICONSUME_UPDATE_CONSUME_NEGATIVE(boost, typeBits, dataBits)            \
    do {                                                                       \
        consumedBits += (typeBits); /* jump over type bits */                  \
        currentVal =                                                           \
            -(boost + (int64_t)varintBitstreamGet(d, consumedBits, dataBits)); \
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

#pragma unroll 5
    for (size_t i = 1; i < COUNT_ARRAY(dodBitCategory) - 1; i++) {
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

    const int64_t dval = delta(newVal, t1, t0);

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
    w->d = NULL;
    w->t[0] = 0;
    w->t[1] = 0;
    w->usedBits = 0;
    w->totalBytes = 0;
}

/* ====================================================================
 * Read from cannonical dod
 * ==================================================================== */
dodVal dodRead(dodWriter *const w, const size_t offset) {
    if (w->d) {
        size_t consumedBits = 0;
        return dodGet(w->d, &consumedBits, w->t[0], w->t[1], offset);
    }

    return 0;
}

/* ====================================================================
 * Write to cannonical dod
 * ==================================================================== */
void dodWrite(dodWriter *const w, const dodVal val) {
    /* If less than two elements, populate preconditions, not bitmap */
    if (w->count < 2) {
        w->t[w->count] = val;
    } else {
        /* else, we can write to bitmap directly. */

        /* bitmap must be created prior to here with proper extent */
        assert(w->d);

        dodAppend(w->d, w->t[0], w->t[1], val, &w->usedBits);

        /* Rotate preconditions for next append */
        w->t[0] = w->t[1];
        w->t[1] = val;
    }

    /* New element added on this writer... */
    w->count++;
}

void dodCloseWrites(dodWriter *const w) {
    dodWriter grabData;
    dodInitFromExisting(&grabData, w->d);
    w->t[0] = grabData.t[0];
}

/* ====================================================================
 * Get dod cached reader
 * ==================================================================== */
void dodInitFromExisting(dodWriter *const w, const dod *d) {
    /* Layout:
     *   - t0:
     *     - 32 or 64 bits
     *       - floor'd to 4 hours or 6 hours
     *   - t1
     *       - 14 bits (4 hrs)
     *       - 16 bits (12 hrs) */

    /* TODO:
     *   populate 'elements' and 'bytes' and cache highest value (?) */
    dodVal t1 = 0;
    const uint8_t floorControl = varintBitstreamGet(d, 0, 2);
    w->usedBits = 2;

    switch (floorControl) {
    case DOD_FLOOR_SEC_HR_4:
        w->t[0] = varintBitstreamGet(d, w->usedBits, 32);
        w->usedBits += 32;
        t1 = varintBitstreamGet(d, w->usedBits, 14);
        w->usedBits += 14;
        _varintBitstreamRestoreSigned(t1, 14);
        break;
    case DOD_FLOOR_SEC_HR_12:
        w->t[0] = varintBitstreamGet(d, w->usedBits, 32);
        w->usedBits += 32;
        t1 = varintBitstreamGet(d, w->usedBits, 16);
        w->usedBits += 16;
        _varintBitstreamRestoreSigned(t1, 16);
        break;
    case DOD_FLOOR_NS_HR_4:
        w->t[0] = varintBitstreamGet(d, w->usedBits, 64);
        w->usedBits += 64;
        t1 = varintBitstreamGet(d, w->usedBits, 45);
        w->usedBits += 45;
        _varintBitstreamRestoreSigned(t1, 45);
        break;
    case DOD_FLOOR_NS_HR_12:
        w->t[0] = varintBitstreamGet(d, w->usedBits, 64);
        w->usedBits += 64;
        t1 = varintBitstreamGet(d, w->usedBits, 46);
        w->usedBits += 46;
        _varintBitstreamRestoreSigned(t1, 46);
        break;
    }

    w->t[1] = w->t[0] + t1;
}

/* NOTE: 'vals' must have length >= 2 because we ALWAYS write [0] AND [1] */
bool dodReadAll(const dod *const d, uint64_t *vals, size_t count) {
    dodWriter tmp;
    dodInitFromExisting(&tmp, d);

    if (!count) {
        return false;
    }

    /* We always write a minimum of two slots, so if count is 1,
     * make two just because. */
    if (count < 2) {
        count = 2;
    }

    vals[0] = tmp.t[0];
    vals[1] = tmp.t[1];

    size_t consumedBits = 0;
    for (size_t i = 2; i < count; i++) {
        const dodVal retrieved =
            dodGet(d, &consumedBits, tmp.t[0], tmp.t[1], 1);
        tmp.t[0] = tmp.t[1];
        tmp.t[1] = retrieved;

        vals[i] = retrieved;
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
    r->consumedBits = 0;
    r->t0 = w->t[0];
    r->t1 = w->t[1];
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
#include "ctest.h"

#define DOUBLE_NEWLINE 1
#include "perf.h"

/* multi-type, multi-data testing and validation */
static int testBits(const uint8_t typeBits, const uint8_t dataBits,
                    const int64_t boosterVal, const size_t loopers) {
    int err = 0;
    dodVal highest = 0;
    const uint8_t bitStorage = typeBits + dataBits;

    /* +1 below because it's a cheap "ceiling" operation to fix int division */
    dod *const bits =
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
                assert(NULL);
            }

            for (size_t i = 2; i <= loopers; i++) {
                currentVal += 100 + (boosterVal * ((i % 2) == 0 ? -1 : 1));
                dodAppend(bits, t0, t1, currentVal, &bitsUsed);
                t0 = t1;
                t1 = currentVal;

                if (bitsUsed != (i * bitStorage)) {
                    ERR("[%zu] Expected %zu bits used, but used %zu bits!", i,
                        i * bitStorage, bitsUsed);
                    assert(NULL);
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

    zfree(bits);

    return err;
}

int dodTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    const size_t loopers = 10000;

#if 1
    TEST("basic bitstream set/get") {
        size_t usedBits = 0;

        uint64_t bits[1024] = {0}; /* 64k bits */

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

        uint64_t bits[1024] = {0}; /* 64k bits */
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
        dod *bits = zcalloc(loopers, sizeof(*bits));
        int64_t *values = zcalloc(loopers * 2, sizeof(*values));

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
                assert(NULL);
            }
        }

        for (size_t i = 0; i < loopers; i++) {
            int64_t got = dodGetIntegerAtOffset(bits, i + 1);

            if (values[i] != got) {
                ERR("[%zu] Expected %" PRIu64 " but got %" PRIu64 "\n", i,
                    values[i], got);
            }
        }

        zfree(values);
        zfree(bits);
    }
    //    }

    for (size_t j = 0; j < 2; j++) {
        TEST_DESC("verify integer encoding works at big (%s) sequential range",
                  j == 0 ? "+" : "-") {
            int32_t modifier = j == 0 ? 1 : -1;
            const int64_t localMax = 1 << 10;
            const size_t localLoopers = 10000;
            dod *bits = zcalloc(localLoopers * 2, sizeof(*bits));

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
            zfree(bits);
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
            dodVal *values = zcalloc(loopers, sizeof(*values));
            dod *bits = zcalloc(loopers * 2, sizeof(*bits));

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

            zfree(bits);
            zfree(values);
        }
    }

    TEST("dodReader - O(1) sequential access") {
        dodVal *values = zcalloc(loopers, sizeof(*values));
        dod *bits = zcalloc(loopers * 2, sizeof(*bits));

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
            ERRR("dodReaderCurrent returned wrong initial value!");
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
            ERRR("dodReaderRemaining should be 0 after reading all values!");
        }

        /* Test dodReaderNextN - encode and read again */
        dodReaderInit(&r, values[0], values[1]);
        dodVal *batch = zcalloc(loopers, sizeof(*batch));
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

        zfree(batch);
        zfree(bits);
        zfree(values);
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
