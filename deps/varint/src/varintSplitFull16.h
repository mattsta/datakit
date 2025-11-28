#pragma once

#include "varint.h"
#include "varintExternal.h"
__BEGIN_DECLS

/* ====================================================================
 * SplitFull16 varints
 * ==================================================================== */
/* varint model SplitFull16 Container:
 *   Type encoded inside: first byte
 *   Size: 2 byte to 9 bytes
 *   Layout: big endian type data, big endian split, little endian external.
 *   Meaning: full width contained in first byte. First byte also stores value.
 *   Pro: known bit boundaries so you can pack other types on top if necessary.
 *        This differs from the regular 'Split' varint beacuse here we
 *        *also* use the byte prefix '11' instead of reserving it for use by
 *        users of the library.
 *   Con: Minimum two byte storage. */

/* SplitFull16 Data Layout */
/* ================= */
/*
 * Encodings of the first type (type byte holds user data)
 * -------------------------------------------------------
 * 2 bytes:
 * |00pppppp|qqqqqqqq| (14 bits) (~16k)
 *      Unsigned numeric value less than or equal to:
 *        2^14 - 1 = 16383
 * 3 bytes:
 * |01pppppp|qqqqqqqq|rrrrrrrr| (22 bits) (~4M)
 *      Unsigned numeric value less than or equal to:
 *        2^22 - 1 + 16383 = 4210686 (22 bits + previous level)
 * 4 bytes:
 * |10pppppp|qqqqqqqq|rrrrrrrr|ssssssss| (30 bits) (~1G)
 *      Unsigned numeric value less than or equal to:
 *        2^30 - 1 + 4210686 = 1077952509 (30 bits + previous levels)
 *
 * Encodings of the second type (type byte begins with 11)
 * -------------------------------------------------------
 * 5 bytes:
 * |11000100|qqqqqqqq|rrrrrrrr|ssssssss|tttttttt|
 *      Unsigned numeric value less than or equal to:
 *        1077952509 + 2^32 - 1 = 5372919804 (~5G)
 * 6 bytes:
 * |11000101|qqqqqqqq|rrrrrrrr|ssssssss|tttttttt|vvvvvvvv|
 *      Unsigned numeric value less than or equal to:
 *        1077952509 + 2^40 - 1 = 1100589580284 (~1T)
 * 7 bytes:
 * |11000110|qqqqqqqq|rrrrrrrr|ssssssss|tttttttt|vvvvvvvv|uuuuuuuu|
 *      Unsigned numeric value less than or equal to:
 *        1077952509 + 2^48 - 1 = 281476054663164 (~281T)
 * 8 bytes:
 * |11000111|qqqqqqqq|rrrrrrrr|ssssssss|tttttttt|vvvvvvvv|uuuuuuuu|wwwwwwww|
 *      Unsigned numeric value less than or equal to:
 *        1077952509 + 2^56 - 1 = 72057595115880444 (~72P)
 * 9 bytes:
 * |11001000|qqqqqqqq|rrrrrrrr|ssssssss|tttttttt|
 *          |vvvvvvvv|uuuuuuuu|wwwwwwww|zzzzzzzz|
 *      Unsigned numeric value less than or equal to:
 *        2^64 - 1 = 18446744073709551615 (~18E)
 * Currently unused: |11001001| to |11111111| */

/* Mask to grab the top two bits of our type determination byte. */
#define VARINT_SPLIT_FULL_16_MASK 0xc0 /* MASK: 11000000 */

/* Mask to select the 6 bits in our type byte */
#define VARINT_SPLIT_FULL_16_6_MASK (0x3f) /* MASK: 00111111 */

#define VARINT_SPLIT_FULL_16_MAX_14 (0x3fff)
#define VARINT_SPLIT_FULL_16_MAX_22 (VARINT_SPLIT_FULL_16_MAX_14 + 0x3fffff)
#define VARINT_SPLIT_FULL_16_MAX_30 (VARINT_SPLIT_FULL_16_MAX_22 + 0x3fffffff)

/* Our type determiniation byte values */
typedef enum varintSplitFull16Tag {
    VARINT_SPLIT_FULL_16_14 = 0x00, /* 00000000 */
    VARINT_SPLIT_FULL_16_22 = 0x40, /* 01000000 */
    VARINT_SPLIT_FULL_16_30 = 0x80, /* 10000000 */
    VARINT_SPLIT_FULL_16_VAR = 0xc0 /* 11000000 */
} varintSplitFull16Tag;

/* For encoding, we have three total prefixes:
 *   00 - 14 bits direct
 *   01 - 22 bits direct
 *   10 - 30 bits direct
 *   11 - external encoding. */
#define varintSplitFull16Encoding2_(p) ((p)[0] & VARINT_SPLIT_FULL_16_MASK)

/* We obtain the number of bytes needed for the external encoding by using
 * the last four bits of the encoding type. See 'varintSplitFull16Byte'
 * for a map of each encoding to each binary value. */
#define varintSplitFull16EncodingWidthBytesExternal_(p)                        \
    (varintWidth)((p)[0] & 0x0f)

#define varintSplitFull16LengthVAR_(encodedLen, _val)                          \
    do {                                                                       \
        /* NB: Val must be subtracted by previous level *before* here */       \
        varintWidth _vimp_valLen;                                              \
        varintExternalUnsignedEncoding((_val), _vimp_valLen);                  \
        if (_vimp_valLen <= 4) {                                               \
            (encodedLen) = (uint8_t)(1 + 4);                                   \
        } else {                                                               \
            (encodedLen) = (uint8_t)(1 + _vimp_valLen);                        \
        }                                                                      \
    } while (0)

#define varintSplitFull16Length_(encodedLen, _val)                             \
    do {                                                                       \
        if ((_val) <= VARINT_SPLIT_FULL_16_MAX_14) {                           \
            (encodedLen) = 1 + 1;                                              \
        } else if ((_val) <= VARINT_SPLIT_FULL_16_MAX_22) {                    \
            (encodedLen) = 1 + 2;                                              \
        } else if ((_val) <= VARINT_SPLIT_FULL_16_MAX_30) {                    \
            (encodedLen) = 1 + 3;                                              \
        } else {                                                               \
            varintSplitFull16LengthVAR_((encodedLen),                          \
                                        (_val) - VARINT_SPLIT_FULL_16_MAX_30); \
        }                                                                      \
    } while (0)

#define varintSplitFull16Put_(dst, encodedLen, _val)                           \
    do {                                                                       \
        uint64_t _vimp__val = (_val);                                          \
        if (_vimp__val <= VARINT_SPLIT_FULL_16_MAX_14) {                       \
            (dst)[0] =                                                         \
                (uint8_t)(VARINT_SPLIT_FULL_16_14 |                            \
                          ((_vimp__val >> 8) & VARINT_SPLIT_FULL_16_6_MASK));  \
            (dst)[1] = (uint8_t)(_vimp__val & 0xff);                           \
            (encodedLen) = 2;                                                  \
        } else if (_vimp__val <= VARINT_SPLIT_FULL_16_MAX_22) {                \
            _vimp__val -= VARINT_SPLIT_FULL_16_MAX_14;                         \
            (dst)[0] =                                                         \
                (uint8_t)(VARINT_SPLIT_FULL_16_22 |                            \
                          ((_vimp__val >> 16) & VARINT_SPLIT_FULL_16_6_MASK)); \
            (dst)[1] = (uint8_t)((_vimp__val >> 8) & 0xff);                    \
            (dst)[2] = (uint8_t)(_vimp__val & 0xff);                           \
            (encodedLen) = 3;                                                  \
        } else if (_vimp__val <= VARINT_SPLIT_FULL_16_MAX_30) {                \
            _vimp__val -= VARINT_SPLIT_FULL_16_MAX_22;                         \
            (dst)[0] =                                                         \
                (uint8_t)(VARINT_SPLIT_FULL_16_30 |                            \
                          ((_vimp__val >> 24) & VARINT_SPLIT_FULL_16_6_MASK)); \
            (dst)[1] = (uint8_t)((_vimp__val >> 16) & 0xff);                   \
            (dst)[2] = (uint8_t)((_vimp__val >> 8) & 0xff);                    \
            (dst)[3] = (uint8_t)(_vimp__val & 0xff);                           \
            (encodedLen) = 4;                                                  \
        } else {                                                               \
            _vimp__val -= VARINT_SPLIT_FULL_16_MAX_30;                         \
            varintSplitFull16LengthVAR_((encodedLen), _vimp__val);             \
            varintWidth _vimp_width = (encodedLen) - 1;                        \
            (dst)[0] = (uint8_t)(VARINT_SPLIT_FULL_16_VAR | _vimp_width);      \
            varintExternalPutFixedWidthQuickMedium_((dst) + 1, _vimp__val,     \
                                                    _vimp_width);              \
        }                                                                      \
    } while (0)

/* We can cheat a little here and only do one comparison.
 * If we're VAR, get VAR length.  else, our other three prefixes are:
 *   - 00 for embedded 14-bit values
 *   - 01 for embedded 22-bit values
 *   - 10 for embedded 30-bit values */
#define varintSplitFull16GetLenQuick_(ptr)                                     \
    (varintSplitFull16Encoding2_(ptr) == VARINT_SPLIT_FULL_16_VAR              \
         ? 1 + varintSplitFull16EncodingWidthBytesExternal_(ptr)               \
         : 2 + ((ptr)[0] >> 6))

#define varintSplitFull16GetLen_(ptr, valsize)                                 \
    do {                                                                       \
        switch (varintSplitFull16Encoding2_(ptr)) {                            \
        case VARINT_SPLIT_FULL_16_14:                                          \
            (valsize) = 1 + 1;                                                 \
            break;                                                             \
        case VARINT_SPLIT_FULL_16_22:                                          \
            (valsize) = 1 + 2;                                                 \
            break;                                                             \
        case VARINT_SPLIT_FULL_16_30:                                          \
            (valsize) = 1 + 3;                                                 \
            break;                                                             \
        case VARINT_SPLIT_FULL_16_VAR:                                         \
            (valsize) = 1 + varintSplitFull16EncodingWidthBytesExternal_(ptr); \
            break;                                                             \
        default:                                                               \
            (valsize) = 0;                                                     \
        }                                                                      \
    } while (0)

#define varintSplitFull16Get_(ptr, valsize, val)                               \
    do {                                                                       \
        switch (varintSplitFull16Encoding2_(ptr)) {                            \
        case VARINT_SPLIT_FULL_16_14:                                          \
            (valsize) = 1 + 1;                                                 \
            (val) =                                                            \
                ((uint64_t)((ptr)[0] & VARINT_SPLIT_FULL_16_6_MASK) << 8) |    \
                (uint64_t)(ptr)[1];                                            \
            break;                                                             \
        case VARINT_SPLIT_FULL_16_22:                                          \
            (valsize) = 1 + 2;                                                 \
            (val) =                                                            \
                ((uint64_t)((ptr)[0] & VARINT_SPLIT_FULL_16_6_MASK) << 16) |   \
                ((uint64_t)(ptr)[1] << 8) | (uint64_t)(ptr)[2];                \
            (val) += VARINT_SPLIT_FULL_16_MAX_14;                              \
            break;                                                             \
        case VARINT_SPLIT_FULL_16_30:                                          \
            (valsize) = 1 + 3;                                                 \
            (val) =                                                            \
                ((uint64_t)((ptr)[0] & VARINT_SPLIT_FULL_16_6_MASK) << 24) |   \
                ((uint64_t)(ptr)[1] << 16) | ((uint64_t)(ptr)[2] << 8) |       \
                (uint64_t)(ptr)[3];                                            \
            (val) += VARINT_SPLIT_FULL_16_MAX_22;                              \
            break;                                                             \
        case VARINT_SPLIT_FULL_16_VAR:                                         \
            (valsize) = 1 + varintSplitFull16EncodingWidthBytesExternal_(ptr); \
            varintExternalGetQuickMedium_((ptr) + 1, (valsize) - 1, (val));    \
            (val) += VARINT_SPLIT_FULL_16_MAX_30;                              \
            break;                                                             \
        default:                                                               \
            (valsize) = (val) = 0;                                             \
        }                                                                      \
    } while (0)

__END_DECLS
