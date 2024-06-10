#include "varint.h"

static const uint8_t extent = 128;

/* By checking notAtMaximumWidth(), we can detect when there is only
 * one byte value remaining.  When there is only one remaining
 * byte of user data, we can store the full byte of user data as
 * the last varint byte *without* a continuation bit in the last byte.
 *
 * With this optimization, a full 64-bit varintChained is (9 bytes):
 * 1XXXXXXX,
 * 1XXXXXXX,
 * 1XXXXXXX,
 * 1XXXXXXX,
 * 1XXXXXXX,
 * 1XXXXXXX,
 * 1XXXXXXX,
 * 1XXXXXXX,
 * XXXXXXXX <-- final byte has 8 bits of user data
 *
 * Without this optimization, a 64-bit varintChained would be (10 bytes):
 * 1XXXXXXX,
 * 1XXXXXXX,
 * 1XXXXXXX,
 * 1XXXXXXX,
 * 1XXXXXXX,
 * 1XXXXXXX,
 * 1XXXXXXX,
 * 1XXXXXXX,
 * 1XXXXXXX,
 * 0000000X <-- final byte only has one bit of user data
 *
 * Without this optimization, a full 64-bit width varintChainedSimple
 * takes *10* bytes because the last byte only has one bit of information.
 *
 * This means varintChainedSimple has a max encoding of 9 bytes instead
 * of 10 bytes as was pulled from the original leveldb source. */
#define notAtMaximumWidth(mover, orig) (((mover) - (orig)) < 8)

varintWidth varintChainedSimpleEncode64(uint8_t *p, uint64_t v) {
    uint8_t *writeP = p;
    for (; v >= extent && notAtMaximumWidth(writeP, p); writeP++) {
        *writeP = (v & (extent - 1)) | extent;
        v >>= 7;
    }

    *writeP = v;
    /* Plus one because 'writeP' only advances at the end of
     * the loop.  If we're the first time through,
     * we processed one byte, but writeP == p, but the writeP
     * counter never advanced, so we need to account for it here. */
    return 1 + writeP - p;
}

varintWidth varintChainedSimpleLength(uint64_t v) {
    varintWidth i = VARINT_WIDTH_8B;
    while (v >>= 7) {
        i++;
    }

    /* varintChainedSimple is caped at 9 bytes */
    return i > 9 ? 9 : i;
}

varintWidth varintChainedSimpleDecode64(const uint8_t *p, uint64_t *v) {
    uint64_t result = 0;
    const uint8_t *mover = p;
    /* Need uint64_t holder for byte so shifting doesn't break. */
    uint64_t holder = *mover;
    for (uint8_t shift = 0; shift <= 63; shift += 7, mover++, holder = *mover) {
        if ((holder & extent) && notAtMaximumWidth(mover, p)) {
            result |= ((holder & 127) << shift);
        } else {
            result |= (holder << shift);
            *v = result;
            /* Below, +1 is because 'mover' only advances at the end of
             * the loop.  If we're the first time through,
             * we processed one byte, but mover == p, so return 1. */
            return 1 + mover - p;
        }
    }
    return VARINT_WIDTH_INVALID;
}

varintWidth varintChainedSimpleEncode32(uint8_t *p, uint32_t v) {
    uint8_t *ptr = p;
    if (v < (1 << 7)) {
        *(ptr++) = v;
    } else if (v < (1 << 14)) {
        *(ptr++) = v | extent;
        *(ptr++) = v >> 7;
    } else if (v < (1 << 21)) {
        *(ptr++) = v | extent;
        *(ptr++) = (v >> 7) | extent;
        *(ptr++) = v >> 14;
    } else if (v < (1 << 28)) {
        *(ptr++) = v | extent;
        *(ptr++) = (v >> 7) | extent;
        *(ptr++) = (v >> 14) | extent;
        *(ptr++) = v >> 21;
    } else {
        *(ptr++) = v | extent;
        *(ptr++) = (v >> 7) | extent;
        *(ptr++) = (v >> 14) | extent;
        *(ptr++) = (v >> 21) | extent;
        *(ptr++) = v >> 28;
    }

    return ptr - p;
}

varintWidth varintChainedSimpleDecode32Fallback(const uint8_t *p,
                                                uint32_t *value) {
    uint64_t v = 0;
    varintWidth len = varintChainedSimpleDecode64(p, &v);
    *value = v;
    return len;
}

varintWidth varintChainedSimpleDecode32(const uint8_t *p, uint32_t *value) {
    if ((*p & extent) == 0) {
        *value = *p;
        return 1;
    }

    return varintChainedSimpleDecode32Fallback(p, value);
}
