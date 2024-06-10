#pragma once

#include "varint.h"
#include "varintExternal.h"
__BEGIN_DECLS

/* ====================================================================
 * SplitFull varints
 * ==================================================================== */
/* varint model SplitFull Container:
 *   Type encoded inside: first byte
 *   Size: 1 byte to 9 bytes
 *   Layout: big endian type data, big endian split, little endian external.
 *   Meaning: full width contained in first byte. First byte also stores value.
 *   Pro: known bit boundaries so you can pack other types on top if necessary.
 *        This differs from the regular 'Split' varint beacuse here we
 *        *also* use the byte prefix '11' instead of reserving it for use by
 *        users of the library.
 *        Because of this, our 'second type' encodings can include a starting
 *        off point of:
 *              2^6 - 1 + 2^14 - 1 + 2^22 - 1 = 4210749
 *   Con: One byte only stores numbers up to 63. */

/* SplitFull Data Layout */
/* ================= */
/*
 * Encodings of the first type (type byte holds user data)
 * -------------------------------------------------------
 * 1 byte:
 * |00pppppp| (6 bits)
 *      Unsigned numeric value less than or equal to:
 *        2^6 - 1 = 63 (6 bits)
 * 2 bytes:
 * |01pppppp|qqqqqqqq| (14 bits) (~16k)
 *      Unsigned numeric value less than or equal to:
 *        2^14 - 1 + 63 = 16446 (14 bits + previous level)
 * 3 bytes:
 * |10pppppp|qqqqqqqq|rrrrrrrr| (22 bits) (~4M)
 *      Unsigned numeric value less than or equal to:
 *        2^22 - 1 + 16446 = 4210749 (22 bits + previous level)
 *
 * Encodings of the second type (type byte begins with 11)
 * -------------------------------------------------------
 * 2 bytes:
 * |11000001|qqqqqqqq|
 *      XX==NOT USED==XX
 *      If we did use this range, integers between
 *      [4210750, 4211004] would take 2 bytes even though
 *      integers from the smaller range [16447, 4210749] take
 *      three bytes.  We don't want to store larger numbers in
 *      fewer bytes of storage because that confuses some allocation
 *      schemes.
 *      (If used, this range would be:
 *        4210749 + 2^8 - 1 = 4211004)
 * 3 bytes:
 * |11000010|qqqqqqqq|rrrrrrrr|
 *      Unsigned numeric value less than or equal to:
 *        4210749 + 2^16 - 1 = 4276284 (~4M)
 * 4 bytes:
 * |11000011|qqqqqqqq|rrrrrrrr|ssssssss|
 *      Unsigned numeric value less than or equal to:
 *        4210749 + 2^24 - 1 = 20987964 (~20M)
 * 5 bytes:
 * |11000100|qqqqqqqq|rrrrrrrr|ssssssss|tttttttt|
 *      Unsigned numeric value less than or equal to:
 *        4210749 + 2^32 - 1 = 4299178044 (~4G)
 * 6 bytes:
 * |11000101|qqqqqqqq|rrrrrrrr|ssssssss|tttttttt|vvvvvvvv|
 *      Unsigned numeric value less than or equal to:
 *        4210749 + 2^40 - 1 = 1099515838524 (~1T)
 * 7 bytes:
 * |11000110|qqqqqqqq|rrrrrrrr|ssssssss|tttttttt|vvvvvvvv|uuuuuuuu|
 *      Unsigned numeric value less than or equal to:
 *        4210749 + 2^48 - 1 = 281474980921404 (~281T)
 * 8 bytes:
 * |11000111|qqqqqqqq|rrrrrrrr|ssssssss|tttttttt|vvvvvvvv|uuuuuuuu|wwwwwwww|
 *      Unsigned numeric value less than or equal to:
 *        4210749 + 2^56 - 1 = 72057594042138684 (~72P)
 * 9 bytes:
 * |11001000|qqqqqqqq|rrrrrrrr|ssssssss|tttttttt|
 *          |vvvvvvvv|uuuuuuuu|wwwwwwww|zzzzzzzz|
 *      Unsigned numeric value less than or equal to:
 *        2^64 - 1 = 18446744073709551615 (~18E)
 * Currently unused: |11001001| to |11111111| */

/* Mask to grab the top two bits of our type determination byte. */
#define VARINT_SPLIT_FULL_MASK 0xc0 /* MASK: 11000000 */

/* Mask to select the 6 bits in our type byte */
#define VARINT_SPLIT_FULL_6_MASK (0x3f) /* MASK: 00111111 */

/* Max for 6 bits: (1 << 6) - 1 */
#define VARINT_SPLIT_FULL_MAX_6 (0x3f)

/* Max for our 14 bits includes previous level: MAX_6 + ((1 << 14) - 1) */
#define VARINT_SPLIT_FULL_MAX_14 (VARINT_SPLIT_FULL_MAX_6 + 0x3fff)

/* Max for our 22 bits includes previous level: MAX_14 + ((1 << 22) - 1) */
#define VARINT_SPLIT_FULL_MAX_22 (VARINT_SPLIT_FULL_MAX_14 + 0x3fffff)

/* Our type determiniation byte values */
typedef enum varintSplitFullTag {
    VARINT_SPLIT_FULL_6 = 0x00,  /* 00000000; 00XXXXXX; max 63 */
    VARINT_SPLIT_FULL_14 = 0x40, /* 01000000; 01XXXXXX; max 63 + 2^14 - 1 */
    VARINT_SPLIT_FULL_22 = 0x80, /* 10000000; 10XXXXXX; max 16446 + 2^22 - 1 */
    VARINT_SPLIT_FULL_VAR = 0xc0 /* 11000000; 11000XXX; max 2^64 - 1 */
} varintSplitFullTag;

/* If we remove VARINT_SPLIT_FULL_VAR from these enum fields, we get the
 * varint external storage width. */
/* This typedef defines the type byte for storing external varints after
 * we grow beyond the first level storage size maximum. */
typedef enum varintSplitFullByte {
    VARINT_SPLIT_FULL_BYTE_VAR_START__ = VARINT_SPLIT_FULL_VAR, /* 11000000 */
    VARINT_SPLIT_FULL_BYTE_1, /* XX===NOT USED===XX; 11000001 */
    VARINT_SPLIT_FULL_BYTE_2, /* 4210749 + uint16_t; 11000010 */
    VARINT_SPLIT_FULL_BYTE_3, /* 4210749 + uint24_t; 11000011 */
    VARINT_SPLIT_FULL_BYTE_4, /* 4210749 + uint32_t; 11000100 */
    VARINT_SPLIT_FULL_BYTE_5, /* 4210749 + uint40_t; 11000101 */
    VARINT_SPLIT_FULL_BYTE_6, /* 4210749 + uint48_t; 11000110 */
    VARINT_SPLIT_FULL_BYTE_7, /* 4210749 + uint56_t; 11000111 */
    VARINT_SPLIT_FULL_BYTE_8, /* 4210749 + uint64_t; 11001000 */
    /* Ranges between 11001001 and 11111111 are available. */
} varintSplitFullByte;

/* For encoding, we have three total prefixes:
 *   00 - 6 bits direct
 *   01 - 14 bits direct
 *   10 - 22 bits direct
 *   11 - external encoding. */
#define varintSplitFullEncoding2_(p) ((p)[0] & VARINT_SPLIT_FULL_MASK)

/* We obtain the number of bytes needed for the external encoding by using
 * the last four bits of the encoding type. See 'varintSplitFullByte'
 * for a map of each encoding to each binary value. */
#define varintSplitFullEncodingWidthBytesExternal_(p)                          \
    (varintWidth)((p)[0] & 0x0f)

/* There's a tiny 256 integer range of [4210750, 4211004] that causes
 * varintSplitFull to shrink from 3 bytes to 2 bytes even though a previous
 * integer range takes 3 bytes to store.  By default, we don't allow
 * varintSplitFull to shrink when storing larger numbers, but if you enable
 * this define, you can enable grow-shrink-grow behavior for that tiny range. */
#ifdef VARINT_SPLIT_FULL_USE_MAXIMUM_RANGE
#define varintSplitFullLengthVAR_(encodedLen, _val)                            \
    do {                                                                       \
        /* NB: Val must be subtracted by previous level *before* here */       \
        varintWidth _vimp_valLen;                                              \
        varintExternalUnsignedEncoding((_val), _vimp_valLen);                  \
        (encodedLen) = 1 + _vimp_valLen;                                       \
    } while (0)
#else
#define varintSplitFullLengthVAR_(encodedLen, _val)                            \
    do {                                                                       \
        /* NB: Val must be subtracted by previous level *before* here */       \
        varintWidth _vimp_valLen;                                              \
        varintExternalUnsignedEncoding((_val), _vimp_valLen);                  \
        /* If external varint encoding is only 1 byte, we increase it to       \
         * 2 byte storage because we don't want varintSplitFull to store       \
         * larger values (SPLIT_3 + uint8_t) as 2 bytes when SPLIT_3 already   \
         * uses 3 bytes for smaller values.  The impact here is there's a      \
         * range of 256 numbers where we *could* store them as 2 bytes, but We \
         * store them as 3 bytes intead.                                       \
         * We only grow byte storage widths monotonically with integer size */ \
        if (_vimp_valLen == 1) {                                               \
            (encodedLen) = 1 + 2;                                              \
        } else {                                                               \
            (encodedLen) = 1 + _vimp_valLen;                                   \
        }                                                                      \
    } while (0)
#endif

#define varintSplitFullLength_(encodedLen, _val)                               \
    do {                                                                       \
        if ((_val) <= VARINT_SPLIT_FULL_MAX_6) {                               \
            (encodedLen) = 1 + 0;                                              \
        } else if ((_val) <= VARINT_SPLIT_FULL_MAX_14) {                       \
            (encodedLen) = 1 + 1;                                              \
        } else if ((_val) <= VARINT_SPLIT_FULL_MAX_22) {                       \
            (encodedLen) = 1 + 2;                                              \
        } else {                                                               \
            varintSplitFullLengthVAR_((encodedLen),                            \
                                      (_val)-VARINT_SPLIT_FULL_MAX_22);        \
        }                                                                      \
    } while (0)

#define varintSplitFullPut_(dst, encodedLen, _val)                             \
    do {                                                                       \
        uint64_t _vimp__val = (_val);                                          \
        if (_vimp__val <= VARINT_SPLIT_FULL_MAX_6) {                           \
            /* buf[0] = 00[val] */                                             \
            (dst)[0] = VARINT_SPLIT_FULL_6 | _vimp__val;                       \
            (encodedLen) = 1;                                                  \
        } else if (_vimp__val <= VARINT_SPLIT_FULL_MAX_14) {                   \
            _vimp__val -= VARINT_SPLIT_FULL_MAX_6; /* Remove 63 */             \
            /* buf[0] = 01[val][val] */                                        \
            (dst)[0] = VARINT_SPLIT_FULL_14 |                                  \
                       ((_vimp__val >> 8) & VARINT_SPLIT_FULL_6_MASK);         \
            (dst)[1] = _vimp__val & 0xff;                                      \
            (encodedLen) = 2;                                                  \
        } else if (_vimp__val <= VARINT_SPLIT_FULL_MAX_22) {                   \
            _vimp__val -= VARINT_SPLIT_FULL_MAX_14; /* Remove 16446 */         \
            /* buf[0] = 01[val][val][val] */                                   \
            (dst)[0] = VARINT_SPLIT_FULL_22 |                                  \
                       ((_vimp__val >> 16) & VARINT_SPLIT_FULL_6_MASK);        \
            (dst)[1] = (_vimp__val >> 8) & 0xff;                               \
            (dst)[2] = _vimp__val & 0xff;                                      \
            (encodedLen) = 3;                                                  \
        } else {                                                               \
            _vimp__val -= VARINT_SPLIT_FULL_MAX_22; /* Remove (16383 + 63) */  \
            varintSplitFullLengthVAR_((encodedLen), _vimp__val);               \
            varintWidth _vimp_width = (encodedLen)-1;                          \
            /* buf[0] = 10[width][val]...[val] */                              \
            (dst)[0] = VARINT_SPLIT_FULL_VAR | _vimp_width;                    \
            varintExternalPutFixedWidthQuickMedium_((dst) + 1, _vimp__val,     \
                                                    _vimp_width);              \
        }                                                                      \
    } while (0)

/* We can cheat a little here and only do one comparison.
 * If we're VAR, get VAR length.  else, our other three prefixes are:
 *   - 00 for embedded 6-bit values
 *   - 01 for embedded 14-bit values
 *   - 10 for embedded 22-bit values
 * Since 6 bit values have no additional data and 14 bit values have 1 byte
 * of additional data and 22 bit values have 2 bytes of additional data,
 * we can just shift down our type byte by 6 to obtain
 * the "additional" width of the embedded type (if any). */
#define varintSplitFullGetLenQuick_(ptr)                                       \
    (1 + (varintSplitFullEncoding2_(ptr) == VARINT_SPLIT_FULL_VAR              \
              ? varintSplitFullEncodingWidthBytesExternal_(ptr)                \
              : (ptr)[0] >> 6))

#define varintSplitFullGetLen_(ptr, valsize)                                   \
    do {                                                                       \
        switch (varintSplitFullEncoding2_(ptr)) {                              \
        case VARINT_SPLIT_FULL_6:                                              \
            (valsize) = 1 + 0;                                                 \
            break;                                                             \
        case VARINT_SPLIT_FULL_14:                                             \
            (valsize) = 1 + 1;                                                 \
            break;                                                             \
        case VARINT_SPLIT_FULL_22:                                             \
            (valsize) = 1 + 2;                                                 \
            break;                                                             \
        case VARINT_SPLIT_FULL_VAR:                                            \
            (valsize) = 1 + varintSplitFullEncodingWidthBytesExternal_(ptr);   \
            break;                                                             \
        default:                                                               \
            (valsize) = 0;                                                     \
        }                                                                      \
    } while (0)

#define varintSplitFullGet_(ptr, valsize, val)                                 \
    do {                                                                       \
        switch (varintSplitFullEncoding2_(ptr)) {                              \
        case VARINT_SPLIT_FULL_6:                                              \
            (valsize) = 1 + 0;                                                 \
            (val) = (ptr)[0] & VARINT_SPLIT_FULL_6_MASK;                       \
            break;                                                             \
        case VARINT_SPLIT_FULL_14:                                             \
            (valsize) = 1 + 1;                                                 \
            (val) = (((ptr)[0] & VARINT_SPLIT_FULL_6_MASK) << 8) | (ptr)[1];   \
            (val) += VARINT_SPLIT_FULL_MAX_6; /* Restore 63 */                 \
            break;                                                             \
        case VARINT_SPLIT_FULL_22:                                             \
            (valsize) = 1 + 2;                                                 \
            (val) = (((ptr)[0] & VARINT_SPLIT_FULL_6_MASK) << 16) |            \
                    ((ptr)[1] << 8) | (ptr)[2];                                \
            (val) += VARINT_SPLIT_FULL_MAX_14; /* Restore 16446 */             \
            break;                                                             \
        case VARINT_SPLIT_FULL_VAR:                                            \
            (valsize) = 1 + varintSplitFullEncodingWidthBytesExternal_(ptr);   \
            varintExternalGetQuickMedium_((ptr) + 1, (valsize)-1, (val));      \
            (val) += VARINT_SPLIT_FULL_MAX_22; /* Restore MAX_22 */            \
            break;                                                             \
        default:                                                               \
            (valsize) = (val) = 0;                                             \
        }                                                                      \
    } while (0)

/* ====================================================================
 * Reversed SplitFull varints
 * ==================================================================== */
/* varint model Reversed SplitFull Container:
 *   Type encoded inside: last byte
 *   Size: 1 byte to 9 bytes
 *   Layout: little endian
 *   Meaning: full width contained in last byte. Last byte also stores value.
 *   Pro: Allows for reverse traversal of split full varints. */

#define varintSplitFullReversedPutReversed_(dst, encodedLen, _val)             \
    do {                                                                       \
        uint64_t _vimp__val = (_val);                                          \
        if (_vimp__val <= VARINT_SPLIT_FULL_MAX_6) {                           \
            /* buf[0] = 00[val] */                                             \
            (encodedLen) = 1;                                                  \
            (dst)[0] = VARINT_SPLIT_FULL_6 | _vimp__val;                       \
        } else if (_vimp__val <= VARINT_SPLIT_FULL_MAX_14) {                   \
            _vimp__val -= VARINT_SPLIT_FULL_MAX_6; /* Remove 63 */             \
            (encodedLen) = 2;                                                  \
            (dst)[0] = VARINT_SPLIT_FULL_14 |                                  \
                       ((_vimp__val >> 8) & VARINT_SPLIT_FULL_6_MASK);         \
            (dst)[-1] = _vimp__val & 0xff;                                     \
        } else if (_vimp__val <= VARINT_SPLIT_FULL_MAX_22) {                   \
            _vimp__val -= VARINT_SPLIT_FULL_MAX_14; /* Remove 16446 */         \
            (encodedLen) = 3;                                                  \
            (dst)[0] = VARINT_SPLIT_FULL_22 |                                  \
                       ((_vimp__val >> 16) & VARINT_SPLIT_FULL_6_MASK);        \
            (dst)[-1] = (_vimp__val >> 8) & 0xff;                              \
            (dst)[-2] = _vimp__val & 0xff;                                     \
        } else {                                                               \
            _vimp__val -= VARINT_SPLIT_FULL_MAX_22; /* Remove (16383 + 63) */  \
            varintSplitFullLengthVAR_((encodedLen), _vimp__val);               \
            varintWidth _vimp_width = (encodedLen)-1;                          \
            varintExternalPutFixedWidthQuickMedium_((dst)-_vimp_width,         \
                                                    _vimp__val, _vimp_width);  \
            (dst)[0] = VARINT_SPLIT_FULL_VAR | _vimp_width;                    \
        }                                                                      \
    } while (0)

#define varintSplitFullReversedPutForward_(dst, encodedLen, _val)              \
    do {                                                                       \
        uint64_t _vimp__val = (_val);                                          \
        if (_vimp__val <= VARINT_SPLIT_FULL_MAX_6) {                           \
            /* buf[0] = 00[val] */                                             \
            (encodedLen) = 1;                                                  \
            (dst)[0] = VARINT_SPLIT_FULL_6 | _vimp__val;                       \
        } else if (_vimp__val <= VARINT_SPLIT_FULL_MAX_14) {                   \
            _vimp__val -= VARINT_SPLIT_FULL_MAX_6; /* Remove 63 */             \
            (encodedLen) = 2;                                                  \
            (dst)[1] = VARINT_SPLIT_FULL_14 |                                  \
                       ((_vimp__val >> 8) & VARINT_SPLIT_FULL_6_MASK);         \
            (dst)[0] = _vimp__val & 0xff;                                      \
        } else if (_vimp__val <= VARINT_SPLIT_FULL_MAX_22) {                   \
            _vimp__val -= VARINT_SPLIT_FULL_MAX_14; /* Remove 16446 */         \
            (encodedLen) = 3;                                                  \
            (dst)[2] = VARINT_SPLIT_FULL_22 |                                  \
                       ((_vimp__val >> 16) & VARINT_SPLIT_FULL_6_MASK);        \
            (dst)[1] = (_vimp__val >> 8) & 0xff;                               \
            (dst)[0] = _vimp__val & 0xff;                                      \
        } else {                                                               \
            _vimp__val -= VARINT_SPLIT_FULL_MAX_22; /* Remove MAX_22 */        \
            varintSplitFullLengthVAR_((encodedLen), _vimp__val);               \
            varintWidth _vimp_width = (encodedLen)-1;                          \
            (dst)[_vimp_width] = VARINT_SPLIT_FULL_VAR | _vimp_width;          \
            varintExternalPutFixedWidthQuickMedium_((dst), _vimp__val,         \
                                                    _vimp_width);              \
        }                                                                      \
    } while (0)

#define varintSplitFullReversedGet_(ptr, valsize, val)                         \
    do {                                                                       \
        switch (varintSplitFullEncoding2_(ptr)) {                              \
        case VARINT_SPLIT_FULL_6:                                              \
            (valsize) = 1 + 0;                                                 \
            (val) = (ptr)[0] & VARINT_SPLIT_FULL_6_MASK;                       \
            break;                                                             \
        case VARINT_SPLIT_FULL_14:                                             \
            (valsize) = 1 + 1;                                                 \
            (val) = (((ptr)[0] & VARINT_SPLIT_FULL_6_MASK) << 8) | (ptr)[-1];  \
            (val) += VARINT_SPLIT_FULL_MAX_6; /* Restore 63 */                 \
            break;                                                             \
        case VARINT_SPLIT_FULL_22:                                             \
            (valsize) = 1 + 2;                                                 \
            (val) = (((ptr)[0] & VARINT_SPLIT_FULL_6_MASK) << 16) |            \
                    ((ptr)[-1] << 8) | (ptr)[-2];                              \
            (val) += VARINT_SPLIT_FULL_MAX_14; /* Restore 16446 */             \
            break;                                                             \
        case VARINT_SPLIT_FULL_VAR: {                                          \
            varintWidth _vimp_width =                                          \
                varintSplitFullEncodingWidthBytesExternal_(ptr);               \
            (valsize) = 1 + _vimp_width;                                       \
            varintExternalGetQuickMedium_((ptr)-_vimp_width, _vimp_width,      \
                                          (val));                              \
            (val) += VARINT_SPLIT_FULL_MAX_22; /* Restore MAX_22 */            \
            break;                                                             \
        }                                                                      \
        default:                                                               \
            (valsize) = (val) = 0;                                             \
        }                                                                      \
    } while (0)

__END_DECLS
