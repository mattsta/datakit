#pragma once

#include "varint.h"
__BEGIN_DECLS

/* ====================================================================
 * Split varints
 * ==================================================================== */
/* varint model Split Container:
 *   Type encoded inside: first byte
 *   Size: 1 byte to 9 bytes
 *   Layout: big endian type data, big endian split, little endian external.
 *   Meaning: full width contained in first byte. First byte also stores value.
 *   Pro: known bit boundaries so you can pack other types on top if necessary.
 *        because of known offsets, we can also include previous max values in
 *        higher values resulting in larger storage with fewer bytes
 *        (e.g. all our 'second type' encodings automatically include
 *        16446 as a starting value).
 *   Con: One byte can store numbers up to 63.
 *        Inefficient for medium-large number storage. */

/* Split Data Layout */
/* ================= */
/*
 * Encodings of the first type (type byte holds user data)
 * -------------------------------------------------------
 * 1 byte:
 * |00pppppp| (6 bits)
 *      Unsigned numeric value less than or equal to:
 *        2^6 - 1 = 63 (6 bits)
 * 2 bytes:
 * |01pppppp|qqqqqqqq| (14 bits)
 *      Unsigned numeric value less than or equal to:
 *        2^14 - 1 + 63 = 16446 (14 bits + previous level)
 *
 * Encodings of the second type (type byte begins with 10)
 * -------------------------------------------------------
 * 2 bytes:
 * |10000000|qqqqqqqq|
 *      Unsigned numeric value less than or equal to:
 *        16446 + 2^8 - 1 = 16701
 * 3 bytes:
 * |10000010|qqqqqqqq|rrrrrrrr|
 *      Unsigned numeric value less than or equal to:
 *        16446 + 2^16 - 1 = 81981
 * 4 bytes:
 * |10000011|qqqqqqqq|rrrrrrrr|ssssssss|
 *      Unsigned numeric value less than or equal to:
 *        16446 + 2^24 - 1 = 16793661
 * 5 bytes:
 * |10000100|qqqqqqqq|rrrrrrrr|ssssssss|tttttttt|
 *      Unsigned numeric value less than or equal to:
 *        16446 + 2^32 - 1 = 4294983741
 * 6 bytes:
 * |10000101|qqqqqqqq|rrrrrrrr|ssssssss|tttttttt|vvvvvvvv|
 *      Unsigned numeric value less than or equal to:
 *        16446 + 2^40 - 1 = 1099511644221
 * 7 bytes:
 * |10000110|qqqqqqqq|rrrrrrrr|ssssssss|tttttttt|vvvvvvvv|uuuuuuuu|
 *      Unsigned numeric value less than or equal to:
 *        16446 + 2^48 - 1 = 281474976727101
 * 8 bytes:
 * |10000111|qqqqqqqq|rrrrrrrr|ssssssss|tttttttt|vvvvvvvv|uuuuuuuu|wwwwwwww|
 *      Unsigned numeric value less than or equal to:
 *        16446 + 2^56 - 1 = 72057594037944381
 * 9 bytes:
 * |10001000|qqqqqqqq|rrrrrrrr|ssssssss|tttttttt|
 *          |vvvvvvvv|uuuuuuuu|wwwwwwww|zzzzzzzz|
 *      Unsigned numeric value less than or equal to:
 *        2^64 - 1 = 18446744073709551615
 * Currently unused: |10001001| to |10011111|
 * Note: |10100000| to |10111111| is reserved for a 'first type'
 * encoding with embedded user data as noted above. */

/* Mask to grab the top two bits of our type determination byte. */
#define VARINT_SPLIT_MASK 0xc0 /* MASK: 11000000 */

/* Mask to select the 6 bits in our type byte */
#define VARINT_SPLIT_6_MASK (0x3f) /* MASK: 00111111 */

/* Max for 6 bits: (1 << 6) - 1 */
#define VARINT_SPLIT_MAX_6 (0x3f)

/* Max for our 14 bits includes previous level: MAX_6 + ((1 << 14) - 1) */
#define VARINT_SPLIT_MAX_14 (VARINT_SPLIT_MAX_6 + 0x3fff)

/* Our type determiniation byte values */
/* Implementation note:
 *   Since we only use 8 values in the 10XXXXXX prefix, it is possible
 *   to introduce *another* one-byte encoding but prefixed with 101XXXXX,
 *   giving us a 5-bit immediate value.  But, if we add a 5-bit immediate
 *   value, it then has a 3 bit prefix instead of all the other types with
 *   a 2-bit prefix.  Adding a 3-bit prefix on top of a 2-bit prefix requires
 *   our code to add additional range checks and add additional comparisons
 *   during type extraction.  So, even though we *could* add another 5-bit
 *   immediate value type, it isn't worth the added code complexity and overall
 *   slowdown due to extra comparisons just to add a new 0-31 integer range
 *   capable of also being stored in 1 byte. */
typedef enum varintSplitTag {
    VARINT_SPLIT_6 = 0x00,   /* 00000000; 00XXXXXX; max 63 */
    VARINT_SPLIT_14 = 0x40,  /* 01000000; 01XXXXXX; max (63) + 2^14 - 1 */
    VARINT_SPLIT_VAR = 0x80, /* 10000000; 10000XXX; max 2^64 - 1 */
                             /* These bytes *must* remain below 10111111.
                              * Do not enter the range of 11XXXXXX
                              * since these type bytes are shared without
                              * external users using the 11 prefix. */
} varintSplitTag;

/* If we remove VARINT_SPLIT_VAR from these enum fields, we get the
 * varint external storage width. */
typedef enum varintSplitByte {
    VARINT_SPLIT_BYTE_VAR_START__ = VARINT_SPLIT_VAR, /* 10000000 */
    VARINT_SPLIT_BYTE_1, /* 16446 + uint8_t;  10000001 */
    VARINT_SPLIT_BYTE_2, /* 16446 + uint16_t; 10000010 */
    VARINT_SPLIT_BYTE_3, /* 16446 + uint24_t; 10000011 */
    VARINT_SPLIT_BYTE_4, /* 16446 + uint32_t; 10000100 */
    VARINT_SPLIT_BYTE_5, /* 16446 + uint32_t; 10000101 */
    VARINT_SPLIT_BYTE_6, /* 16446 + uint40_t; 10000110 */
    VARINT_SPLIT_BYTE_7, /* 16446 + uint48_t; 10000111 */
    VARINT_SPLIT_BYTE_8, /* 16446 + uint56_t; 10001000 */
    VARINT_SPLIT_BYTE_9, /* 16446 + uint64_t; 10001001 */
    /* Ranges between 10001010 and 10111111 are available. */
    /* (including a full 5-bit range of: 10100000 to 10111111) */
    VARINT_SPLIT_BYTE_VAR_MAX_POSSIBLE__ = VARINT_SPLIT_MASK - 1, /* 10111111 */
} varintSplitByte;

/* For encoding, we have three total prefixes:
 *   00 - 6 bits direct
 *   01 - 14 bits direct
 *   10 - external encoding. */
#define varintSplitEncoding2_(p) ((p)[0] & VARINT_SPLIT_MASK)

/* We obtain the number of bytes needed for the external encoding by subtracting
 * our external encoding prefix from the encoding byte.  The external encoding
 * types are created in-order so the width value is embedded in the type once we
 * remove the encoding prefix. */
#define varintSplitEncodingWidthBytesExternal_(p)                              \
    (varintWidth)((p)[0] - varintSplitEncoding2_(p))

#define varintSplitLengthVAR_(encodedLen, _val)                                \
    do {                                                                       \
        /* NB: Val must be subtracted by previous level *before* here */       \
        varintWidth _vimp_valLen;                                              \
        varintExternalUnsignedEncoding((_val), _vimp_valLen);                  \
        (encodedLen) = (uint8_t)(1 + _vimp_valLen);                            \
    } while (0)

#define varintSplitLength_(encodedLen, _val)                                   \
    do {                                                                       \
        if ((_val) <= VARINT_SPLIT_MAX_6) {                                    \
            (encodedLen) = 1 + 0;                                              \
        } else if ((_val) <= VARINT_SPLIT_MAX_14) {                            \
            (encodedLen) = 1 + 1;                                              \
        } else {                                                               \
            varintSplitLengthVAR_((encodedLen), (_val) - VARINT_SPLIT_MAX_14); \
        }                                                                      \
    } while (0)

#define varintSplitPut_(dst, encodedLen, _val)                                 \
    do {                                                                       \
        uint64_t _vimp__val = (_val);                                          \
        if (_vimp__val <= VARINT_SPLIT_MAX_6) {                                \
            /* buf[0] = 00[val] */                                             \
            (dst)[0] = (uint8_t)(VARINT_SPLIT_6 | _vimp__val);                 \
            (encodedLen) = 1;                                                  \
        } else if (_vimp__val <= VARINT_SPLIT_MAX_14) {                        \
            _vimp__val -= VARINT_SPLIT_MAX_6; /* Remove 63 */                  \
            /* buf[0] = 01[val][val] */                                        \
            (dst)[0] = (uint8_t)(VARINT_SPLIT_14 |                             \
                                 ((_vimp__val >> 8) & VARINT_SPLIT_6_MASK));   \
            (dst)[1] = (uint8_t)(_vimp__val & 0xff);                           \
            (encodedLen) = 2;                                                  \
        } else {                                                               \
            _vimp__val -= VARINT_SPLIT_MAX_14; /* Remove (16383 + 63) */       \
            varintSplitLengthVAR_((encodedLen), _vimp__val);                   \
            varintWidth _vimp_width = (encodedLen) - 1;                        \
            /* buf[0] = 10[width][val]...[val] */                              \
            (dst)[0] = (uint8_t)(VARINT_SPLIT_VAR | _vimp_width);              \
            varintExternalPutFixedWidthQuickMedium_((dst) + 1, _vimp__val,     \
                                                    _vimp_width);              \
        }                                                                      \
    } while (0)

/* We can cheat a little here and only do one comparison.
 * If we're VAR, get VAR length.  else, our other two prefixes are:
 *   - 00 for embedded 6-bit values
 *   - 01 for embedded 14-bit values
 * Since 6 bit values have no additional data and 14 bit values have 1 byte
 * of additional data, we can just shift down our type byte by 6 to obtain
 * the "additional" width of the embedded type (if any). */
#define varintSplitGetLenQuick_(ptr)                                           \
    (1 + (varintSplitEncoding2_(ptr) == VARINT_SPLIT_VAR                       \
              ? varintSplitEncodingWidthBytesExternal_(ptr)                    \
              : (ptr)[0] >> 6))

#define varintSplitGetLen_(ptr, valsize)                                       \
    do {                                                                       \
        switch (varintSplitEncoding2_(ptr)) {                                  \
        case VARINT_SPLIT_6:                                                   \
            (valsize) = 1 + 0;                                                 \
            break;                                                             \
        case VARINT_SPLIT_14:                                                  \
            (valsize) = 1 + 1;                                                 \
            break;                                                             \
        case VARINT_SPLIT_VAR:                                                 \
            (valsize) = 1 + varintSplitEncodingWidthBytesExternal_(ptr);       \
            break;                                                             \
        default:                                                               \
            (valsize) = 0;                                                     \
        }                                                                      \
    } while (0)

#define varintSplitGet_(ptr, valsize, val)                                     \
    do {                                                                       \
        switch (varintSplitEncoding2_(ptr)) {                                  \
        case VARINT_SPLIT_6:                                                   \
            (valsize) = 1 + 0;                                                 \
            (val) = (ptr)[0] & VARINT_SPLIT_6_MASK;                            \
            break;                                                             \
        case VARINT_SPLIT_14:                                                  \
            (valsize) = 1 + 1;                                                 \
            (val) = ((uint64_t)((ptr)[0] & VARINT_SPLIT_6_MASK) << 8) |        \
                    (uint64_t)(ptr)[1];                                        \
            (val) += VARINT_SPLIT_MAX_6; /* Restore 63 */                      \
            break;                                                             \
        case VARINT_SPLIT_VAR:                                                 \
            (valsize) = 1 + varintSplitEncodingWidthBytesExternal_(ptr);       \
            varintExternalGetQuickMedium_((ptr) + 1, (valsize) - 1, (val));    \
            (val) += VARINT_SPLIT_MAX_14; /* Restore 16383 + 63 */             \
            break;                                                             \
        default:                                                               \
            (valsize) = (val) = 0;                                             \
        }                                                                      \
    } while (0)

/* ====================================================================
 * Reversed Split varints
 * ==================================================================== */
/* varint model Reversed Split Container:
 *   Type encoded inside: last byte
 *   Size: 1 byte to 9 bytes
 *   Layout: little endian
 *   Meaning: full width contained in last byte. Last byte also stores value.
 *   Pro: Allows for reverse traversal of split varints. */

#define varintSplitReversedPutReversed_(dst, encodedLen, _val)                 \
    do {                                                                       \
        uint64_t _vimp__val = (_val);                                          \
        if (_vimp__val <= VARINT_SPLIT_MAX_6) {                                \
            /* buf[0] = 00[val] */                                             \
            (encodedLen) = 1;                                                  \
            (dst)[0] = (uint8_t)(VARINT_SPLIT_6 | _vimp__val);                 \
        } else if (_vimp__val <= VARINT_SPLIT_MAX_14) {                        \
            _vimp__val -= VARINT_SPLIT_MAX_6; /* Remove 63 */                  \
            (encodedLen) = 2;                                                  \
            (dst)[0] = (uint8_t)(VARINT_SPLIT_14 |                             \
                                 ((_vimp__val >> 8) & VARINT_SPLIT_6_MASK));   \
            (dst)[-1] = (uint8_t)(_vimp__val & 0xff);                          \
        } else {                                                               \
            _vimp__val -= VARINT_SPLIT_MAX_14; /* Remove (16383 + 63) */       \
            varintSplitLengthVAR_((encodedLen), _vimp__val);                   \
            varintWidth _vimp_width = (encodedLen) - 1;                        \
            varintExternalPutFixedWidthQuickMedium_((dst) - _vimp_width,       \
                                                    _vimp__val, _vimp_width);  \
            (dst)[0] = (uint8_t)(VARINT_SPLIT_VAR | _vimp_width);              \
        }                                                                      \
    } while (0)

#define varintSplitReversedPutForward_(dst, encodedLen, _val)                  \
    do {                                                                       \
        uint64_t _vimp__val = (_val);                                          \
        if (_vimp__val <= VARINT_SPLIT_MAX_6) {                                \
            /* buf[0] = 00[val] */                                             \
            (encodedLen) = 1;                                                  \
            (dst)[0] = (uint8_t)(VARINT_SPLIT_6 | _vimp__val);                 \
        } else if (_vimp__val <= VARINT_SPLIT_MAX_14) {                        \
            _vimp__val -= VARINT_SPLIT_MAX_6; /* Remove 63 */                  \
            (encodedLen) = 2;                                                  \
            (dst)[1] = (uint8_t)(VARINT_SPLIT_14 |                             \
                                 ((_vimp__val >> 8) & VARINT_SPLIT_6_MASK));   \
            (dst)[0] = (uint8_t)(_vimp__val & 0xff);                           \
        } else {                                                               \
            _vimp__val -= VARINT_SPLIT_MAX_14; /* Remove (16383 + 63) */       \
            varintSplitLengthVAR_((encodedLen), _vimp__val);                   \
            varintWidth _vimp_width = (encodedLen) - 1;                        \
            (dst)[_vimp_width] = (uint8_t)(VARINT_SPLIT_VAR | _vimp_width);    \
            varintExternalPutFixedWidthQuickMedium_((dst), _vimp__val,         \
                                                    _vimp_width);              \
        }                                                                      \
    } while (0)

#define varintSplitReversedGet_(ptr, valsize, val)                             \
    do {                                                                       \
        switch (varintSplitEncoding2_(ptr)) {                                  \
        case VARINT_SPLIT_6:                                                   \
            (valsize) = 1 + 0;                                                 \
            (val) = (ptr)[0] & VARINT_SPLIT_6_MASK;                            \
            break;                                                             \
        case VARINT_SPLIT_14:                                                  \
            (valsize) = 1 + 1;                                                 \
            (val) = ((uint64_t)((ptr)[0] & VARINT_SPLIT_6_MASK) << 8) |        \
                    (uint64_t)(ptr)[-1];                                       \
            (val) += VARINT_SPLIT_MAX_6; /* Restore 63 */                      \
            break;                                                             \
        case VARINT_SPLIT_VAR: {                                               \
            varintWidth _vimp_width =                                          \
                varintSplitEncodingWidthBytesExternal_(ptr);                   \
            (valsize) = 1 + _vimp_width;                                       \
            varintExternalGetQuickMedium_((ptr) - _vimp_width, _vimp_width,    \
                                          (val));                              \
            (val) += VARINT_SPLIT_MAX_14; /* Restore 16383 + 63 */             \
            break;                                                             \
        }                                                                      \
        default:                                                               \
            (valsize) = (val) = 0;                                             \
        }                                                                      \
    } while (0)

__END_DECLS
