#pragma once

#include "varint.h"
__BEGIN_DECLS

/* ====================================================================
 * External Big Endian varints
 * ==================================================================== */
/* varint model External Container:
 *   (same as varint External, except stored byte order is big endian) */

varintWidth varintExternalBigEndianPut(void *p, const uint64_t v);
void varintExternalBigEndianPutFixedWidth(void *p, uint64_t v,
                                          varintWidth encoding);
uint64_t varintExternalBigEndianGet(const void *p, varintWidth encoding);

#define varintExternalBigEndianUnsignedEncoding(value, encoding)               \
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
#define varintExternalBigEndianPutFixedWidthQuick_(dst, val, encoding)         \
    do {                                                                       \
        uint8_t *__restrict _vimp_dst = (uint8_t *)(dst);                      \
        switch (encoding) {                                                    \
        case VARINT_WIDTH_8B:                                                  \
            _vimp_dst[0] = (uint8_t)(val);                                     \
            break;                                                             \
        case VARINT_WIDTH_24B:                                                 \
            _vimp_dst[2] = (uint8_t)((val) & 0xff);                            \
            _vimp_dst[1] = (uint8_t)(((val) >> 8) & 0xff);                     \
            _vimp_dst[0] = (uint8_t)(((val) >> 16) & 0xff);                    \
            break;                                                             \
        case VARINT_WIDTH_16B:                                                 \
            _vimp_dst[1] = (uint8_t)((val) & 0xff);                            \
            _vimp_dst[0] = (uint8_t)(((val) >> 8) & 0xff);                     \
            break;                                                             \
        default:                                                               \
            varintExternalBigEndianPutFixedWidth((dst), (val), (encoding));    \
        }                                                                      \
    } while (0)

/* Use temp variable because 'src' will often contain offset math
 * (e.g. (p + offset)) and we don't want to math every operation. */
#define varintExternalBigEndianGetQuick_(src, width, result)                   \
    do {                                                                       \
        const uint8_t *__restrict _vimp_src = (uint8_t *)(src);                \
        switch (width) {                                                       \
        case VARINT_WIDTH_8B:                                                  \
            (result) = (_vimp_src)[0];                                         \
            break;                                                             \
        case VARINT_WIDTH_16B:                                                 \
            (result) =                                                         \
                (uint64_t)(_vimp_src[0]) << 8 | (uint64_t)(_vimp_src[1]);      \
            break;                                                             \
        case VARINT_WIDTH_24B:                                                 \
            (result) = (uint64_t)(_vimp_src[0]) << 16 |                        \
                       (uint64_t)(_vimp_src[1]) << 8 |                         \
                       (uint64_t)(_vimp_src[2]);                               \
            break;                                                             \
        default:                                                               \
            (result) = varintExternalBigEndianGet((src), (width));             \
        }                                                                      \
    } while (0)

__END_DECLS
