#pragma once

#include "varint.h"
__BEGIN_DECLS

/* ====================================================================
 * Tagged varints
 * ==================================================================== */
/* varint model Tagged Container:
 *   Type encoded inside: first byte of varint
 *   Size: 1 byte to 9 bytes
 *   Layout: big endian (can sort compare by memcmp())
 *   Meaning: full width known by first byte. First byte also stores value.
 *   Pro: fast, one byte can store values up to 240
 *   Con: 9 bytes for a full width uint64_t */

/* These are the maximum values for each tagged varint byte width. */
#define VARINT_TAGGED_MAX_1 240UL
#define VARINT_TAGGED_MAX_2 2287UL
#define VARINT_TAGGED_MAX_3 67823UL
#define VARINT_TAGGED_MAX_4 16777215UL /* UINT24_MAX (2^24 - 1) */
#define VARINT_TAGGED_MAX_5 UINT32_MAX
#define VARINT_TAGGED_MAX_6 1099511627775ULL     /* UINT40_MAX (2^40 - 1) */
#define VARINT_TAGGED_MAX_7 281474976710655ULL   /* UINT48_MAX */
#define VARINT_TAGGED_MAX_8 72057594037927935ULL /* UINT56_MAX */
#define VARINT_TAGGED_MAX_9 UINT64_MAX

/* lol automated formatting */
#define varintTaggedLenQuick(v)                                                \
    ((v) <= VARINT_TAGGED_MAX_1   ? 1                                          \
     : (v) <= VARINT_TAGGED_MAX_2 ? 2                                          \
     : (v) <= VARINT_TAGGED_MAX_3 ? 3                                          \
     : (v) <= VARINT_TAGGED_MAX_4 ? 4                                          \
                                  : varintTaggedLen(v))

varintWidth varintTaggedPut64(uint8_t *z, uint64_t x);
varintWidth varintTaggedPut64FixedWidth(uint8_t *z, uint64_t x,
                                        varintWidth width);
varintWidth varintTaggedPut32(uint8_t *p, uint32_t v);
varintWidth varintTaggedGet(const uint8_t *z, int32_t lenMax,
                            uint64_t *pResult);
varintWidth varintTaggedGet64(const uint8_t *z, uint64_t *pResult);
uint64_t varintTaggedGet64ReturnValue(const uint8_t *z);
varintWidth varintTaggedGet32(const uint8_t *z, uint32_t *pResult);
varintWidth varintTaggedLen(uint64_t x);
varintWidth varintTaggedGetLen(const uint8_t *z);
varintWidth varintTaggedAddNoGrow(uint8_t *z, int64_t add);
varintWidth varintTaggedAddGrow(uint8_t *z, int64_t add);

#define varintTaggedGetLenQuick_(z)                                            \
    ((z)[0] <= 240 ? 1 : (z)[0] <= 248 ? 2 : (z)[0] - 246)

/* Note: all in-macro defined variables start with _vimp_
 * (short for _varintImplementation) to hopefully not shadow
 * or reference any variables outside of our local scopes. */
/* These are the first three cases from varintTaggedPut64FixedWidth() */
#define varintTaggedPut64FixedWidthQuick_(dst, val, encoding)                  \
    do {                                                                       \
        uint32_t _vimp_y;                                                      \
        switch (encoding) {                                                    \
        case VARINT_WIDTH_8B:                                                  \
            (dst)[0] = (uint8_t)(val);                                         \
            break;                                                             \
        case VARINT_WIDTH_16B:                                                 \
            _vimp_y = (uint32_t)((val) - 240);                                 \
            (dst)[0] = (uint8_t)(_vimp_y / 256 + 241);                         \
            (dst)[1] = (uint8_t)(_vimp_y % 256);                               \
            break;                                                             \
        case VARINT_WIDTH_24B:                                                 \
            _vimp_y = (uint32_t)((val) - 2288);                                \
            (dst)[0] = 249;                                                    \
            (dst)[1] = (uint8_t)(_vimp_y / 256);                               \
            (dst)[2] = (uint8_t)(_vimp_y % 256);                               \
            break;                                                             \
        default:                                                               \
            varintTaggedPut64FixedWidth((dst), (val), (encoding));             \
        }                                                                      \
    } while (0)

/* These are the first four cases from varintTaggedGet64() */
#define varintTaggedGet64Quick_(src)                                           \
    ((src)[0] <= 240   ? (src)[0]                                              \
     : (src)[0] <= 248 ? ((src)[0] - 241U) * 256 + (src)[1] + 240              \
     : (src)[0] == 249 ? 2288U + 256 * (src)[1] + (src)[2]                     \
                       : varintTaggedGet64ReturnValue(src))

__END_DECLS
