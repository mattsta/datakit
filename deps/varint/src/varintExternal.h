#pragma once

#include "varint.h"

__BEGIN_DECLS

/* ====================================================================
 * External varints
 * ==================================================================== */
/* varint model External Container:
 *   Type encoded by: saving the varint type/width external to the varint
 *   Size: 1 byte to 8 bytes (in bits: 8, 16, 24, 32, 40, 48, 56, 64)
 *   Layout: little endian (common machine representation)
 *   Meaning: must provide explicit type/length for every operation.
 *   Pros: Each varint width uses all bits for user values.  8 bytes full width.
 *         On little endian machines, you can cast byte offsets to integers
 *         for reading (at known-widths of 8, 16, 32, 64).
 *   Con: Must track width/type of varint external to the varint itself. */

#define varintSignBitOffset_(width) (((width)*8) - 1)

/* Native signed value to varint signed value */
/* Sign bit is greater than our storage size, so move it down to our
 * varint storage width and clear top sign bit. */
/* TODO: fix negation below to properly handle -INT_MIN, etc */
#define varintPrepareSigned_(val, externalVarintWidth)                         \
    do {                                                                       \
        /* If value is negative, move native sign to varint sign bit. */       \
        if ((val) < 0) {                                                       \
            /* Remove sign bit from native-level width */                      \
            (val) = -(val);                                                    \
            /* Add sign bit to varint-level width. (toggle == add) */          \
            (val) ^= (1 << varintSignBitOffset_(externalVarintWidth));         \
        }                                                                      \
    } while (0)

/* Varint signed value back to native signed value */
/* Restore previously stashed varint sign bit back to native-level
 * integer sign bit position. */
#define varintRestoreSigned_(result, externalVarintWidth)                      \
    do {                                                                       \
        /* If topmost bit in varint is set, convert to signed integer. */      \
        if (((result) >> varintSignBitOffset_(externalVarintWidth)) & 0x01) {  \
            /* Remove sign bit from varint-level width. (toggle == remove) */  \
            (result) ^= (1 << varintSignBitOffset_(externalVarintWidth));      \
            /* Restore sign bit to native-level width. */                      \
            (result) = -(result);                                              \
        }                                                                      \
    } while (0)

/* Note: these are **only** needed for non-native-width varints.
 * For native-width integers (8, 16, 32, 64 bits), the sign bit is always saved
 * and restored in the proper positions since it's native-width and no byte
 * tuncation happens. */
#define varintPrepareSigned32to24_(val)                                        \
    varintPrepareSigned_(val, VARINT_WIDTH_24B)
#define varintRestoreSigned24to32_(result)                                     \
    varintRestoreSigned_(result, VARINT_WIDTH_24B);
#define varintPrepareSigned64to40_(val)                                        \
    varintPrepareSigned_(val, VARINT_WIDTH_40B)
#define varintRestoreSigned40to64_(result)                                     \
    varintRestoreSigned_(result, VARINT_WIDTH_40B);
#define varintPrepareSigned64to48_(val)                                        \
    varintPrepareSigned_(val, VARINT_WIDTH_48B)
#define varintRestoreSigned48to64_(result)                                     \
    varintRestoreSigned_(result, VARINT_WIDTH_48B);
#define varintPrepareSigned64to56_(val)                                        \
    varintPrepareSigned_(val, VARINT_WIDTH_56B)
#define varintRestoreSigned56to64_(result)                                     \
    varintRestoreSigned_(result, VARINT_WIDTH_56B);

varintWidth varintExternalPut(void *p, uint64_t v);
void varintExternalPutFixedWidth(void *p, uint64_t v, varintWidth encoding);
void varintExternalPutFixedWidthBig(void *p, __uint128_t v,
                                    varintWidth encoding);
uint64_t varintExternalGet(const void *p, varintWidth encoding);
__uint128_t varintBigExternalGet(const void *p, varintWidth encoding);
#define varintExternalLen(v) varintExternalSignedEncoding((uint64_t)(v))
varintWidth varintExternalSignedEncoding(int64_t value);
varintWidth varintExternalAddNoGrow(uint8_t *p, varintWidth encoding,
                                    int64_t add);
varintWidth varintExternalAddGrow(uint8_t *p, varintWidth encoding,
                                  int64_t add);

#define varintExternalUnsignedEncoding(value, encoding)                        \
    do {                                                                       \
        /* Increment encoding for each byte of 'value' with bits set. */       \
        uint64_t _vimp_v = (value);                                            \
        (encoding) = VARINT_WIDTH_8B;                                          \
        while (((_vimp_v) >>= 8) != 0) {                                       \
            (encoding)++;                                                      \
        }                                                                      \
    } while (0)

/* Use temp variable because 'dst' will often contain offset math
 * (e.g. (p + offset)) and we don't want to math every operation. */
#define varintExternalPutFixedWidthQuick_(dst, val, encoding)                  \
    do {                                                                       \
        uint8_t *restrict _vimp_dst = (uint8_t *)(dst);                        \
        switch (encoding) {                                                    \
        case VARINT_WIDTH_8B:                                                  \
            _vimp_dst[0] = (uint8_t)(val);                                     \
            break;                                                             \
        case VARINT_WIDTH_24B:                                                 \
            _vimp_dst[2] = ((val) >> 16) & 0xff;                               \
            varint_fallthrough;                                                \
        case VARINT_WIDTH_16B:                                                 \
            _vimp_dst[1] = ((val) >> 8) & 0xff;                                \
            _vimp_dst[0] = (val)&0xff;                                         \
            break;                                                             \
        default:                                                               \
            varintExternalPutFixedWidth((dst), (val), (encoding));             \
        }                                                                      \
    } while (0)

#define varintExternalPutFixedWidthQuickMedium_(dst, val, encoding)            \
    do {                                                                       \
        uint8_t *restrict _vimp_dst = (uint8_t *)(dst);                        \
        switch (encoding) {                                                    \
        case VARINT_WIDTH_24B:                                                 \
            _vimp_dst[2] = ((val) >> 16) & 0xff;                               \
            varint_fallthrough;                                                \
        case VARINT_WIDTH_16B:                                                 \
            _vimp_dst[1] = ((val) >> 8) & 0xff;                                \
            _vimp_dst[0] = (val)&0xff;                                         \
            break;                                                             \
        default:                                                               \
            varintExternalPutFixedWidth(_vimp_dst, (val), (encoding));         \
        }                                                                      \
    } while (0)

/* Use temp variable because 'src' will often contain offset math
 * (e.g. (p + offset)) and we don't want to math every operation. */
#define varintExternalGetQuick_(src, width, result)                            \
    do {                                                                       \
        const uint8_t *restrict vimp_src_ = (uint8_t *)(src);                  \
        switch (width) {                                                       \
        case VARINT_WIDTH_8B:                                                  \
            (result) = (vimp_src_)[0];                                         \
            break;                                                             \
        case VARINT_WIDTH_16B:                                                 \
            (result) = vimp_src_[1] << 8 | vimp_src_[0];                       \
            break;                                                             \
        case VARINT_WIDTH_24B:                                                 \
            (result) = vimp_src_[2] << 16 | vimp_src_[1] << 8 | vimp_src_[0];  \
            break;                                                             \
        default:                                                               \
            (result) = varintExternalGet((src), (width));                      \
        }                                                                      \
    } while (0)

#define varintExternalGetQuickMedium_(src, width, result)                      \
    do {                                                                       \
        const uint8_t *restrict vimp_src_ = (uint8_t *)(src);                  \
        switch (width) {                                                       \
        case VARINT_WIDTH_24B:                                                 \
            (result) = vimp_src_[2] << 16 | vimp_src_[1] << 8 | vimp_src_[0];  \
            break;                                                             \
        case VARINT_WIDTH_16B:                                                 \
            (result) = vimp_src_[1] << 8 | vimp_src_[0];                       \
            break;                                                             \
        default:                                                               \
            (result) = varintExternalGet(vimp_src_, (width));                  \
        }                                                                      \
    } while (0)

#define varintExternalGetQuickMediumReturnValue_(src, width)                   \
    ((width) == VARINT_WIDTH_24B                                               \
         ? (uint32_t)((src)[2] << 16 | (src)[1] << 8 | (src)[0])               \
         : (width) == VARINT_WIDTH_16B ? (uint16_t)((src)[1] << 8 | (src)[0])  \
                                       : varintExternalGet((src), (width)))

__END_DECLS
