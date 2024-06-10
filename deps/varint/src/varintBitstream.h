#pragma once

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef VBITS
typedef VBITS vbits;
#else
typedef uint64_t vbits;
#endif

#ifdef VBITSVAL
typedef VBITSVAL vbitsVal;
#else
typedef uint64_t vbitsVal;
#endif

#define BITS_PER_SLOT (sizeof(vbits) * 8)

/* Native signed value to varint signed value */
/* Sign bit is greater than our storage size, so move it down to our
 * varint storage width and clear top sign bit. */
#define _varintBitstreamPrepareSigned(val, fullCompactBitWidth)                \
    do {                                                                       \
        /* value is negative, move native sign to varint sign bit. */          \
        /* Remove sign bit from native-level width */                          \
        (val) = -(val);                                                        \
        /* Add sign bit to varint-level width. (toggle == add) */              \
        (val) ^= (1ULL << (fullCompactBitWidth - 1));                          \
    } while (0)

/* Varint signed value back to native signed value */
/* Restore previously stashed varint sign bit back to native-level
 * integer sign bit position. */
#define _varintBitstreamRestoreSigned(result, fullCompactBitWidth)             \
    do {                                                                       \
        /* If topmost bit in varint is set, convert to signed integer. */      \
        if (((result) >> (fullCompactBitWidth - 1)) & 0x01) {                  \
            /* Remove sign bit from varint-level width. (toggle == remove) */  \
            (result) ^= (1ULL << (fullCompactBitWidth - 1));                   \
            /* Restore sign bit to native-level width. */                      \
            (result) = -(result);                                              \
        }                                                                      \
    } while (0)

/* For commented versions of these functions, see varintPacked.h
 * Note: unlike varintPacked, we write in order and not in reverse order. */
static void varintBitstreamSet(vbits *const dst, const size_t startBitOffset,
                               const size_t bitsPerValue, const vbitsVal val) {
    vbitsVal valueMask;
    vbits *__restrict out = NULL;
    int32_t highDataBitPosition = 0;
    int32_t lowDataBitPosition;

    out = &dst[startBitOffset / BITS_PER_SLOT];

    highDataBitPosition = BITS_PER_SLOT - (startBitOffset % BITS_PER_SLOT);
    lowDataBitPosition = highDataBitPosition - (int32_t)bitsPerValue;

    /* This assert triggers if your 'val' is too big to be stored
     * using 'bitsPerValue' */
    valueMask = (~0ULL >> (BITS_PER_SLOT - bitsPerValue));
    assert(0 == (~valueMask & val));

    if (lowDataBitPosition >= 0) {
        out[0] = (out[0] & ~(valueMask << lowDataBitPosition)) |
                 (val << lowDataBitPosition);
    } else {
        vbitsVal low, high;

        const uint32_t highBitInCurrentSlot = -lowDataBitPosition;
        const uint32_t lowBitInOverflowSlot =
            (BITS_PER_SLOT - highBitInCurrentSlot);

        high = val >> highBitInCurrentSlot;
        low = val << lowBitInOverflowSlot;

        out[0] = (out[0] & ~(valueMask >> highBitInCurrentSlot)) | high;
        out[1] = (out[1] & ~(valueMask << lowBitInOverflowSlot)) | low;
    }
}

static vbitsVal varintBitstreamGet(const vbits *const src,
                                   const size_t startBitOffset,
                                   const size_t bitsPerValue) {
    vbitsVal valueMask;
    const vbits *__restrict in = NULL;
    int32_t highDataBitPosition = 0;
    vbitsVal out = 0;
    int32_t lowDataBitPosition;

    in = &src[startBitOffset / BITS_PER_SLOT];

    highDataBitPosition = BITS_PER_SLOT - (startBitOffset % BITS_PER_SLOT);
    lowDataBitPosition = highDataBitPosition - (int32_t)bitsPerValue;

    valueMask = (~0ULL >> (BITS_PER_SLOT - bitsPerValue));

    if (lowDataBitPosition >= 0) {
        out = (in[0] >> lowDataBitPosition) & valueMask;
    } else {
        vbitsVal low, high;

        const uint32_t highBitInCurrentSlot = -lowDataBitPosition;
        const uint32_t lowBitInOverflowSlot =
            (BITS_PER_SLOT - highBitInCurrentSlot);

        high = in[0] & (valueMask >> highBitInCurrentSlot);
        low = in[1] >> lowBitInOverflowSlot;

        out = (high << highBitInCurrentSlot) | low;
    }

    return out;
}
