/*
** 2012 January 17
**
** The authors renounce all claim of copyright to this code and dedicate
** this code to the public domain.  In place of legal notice, here is
** a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
** This was originally varint.c from sqlite4 as of commit:
**      "2015-08-15 [7df97b5937] Fix typo in comments."
**
*************************************************************************
**/

#include "varint.h"

/*
**
** This file contains routines used to encode or decode variable-length
** integers.
**
** A variable length integer is an encoding of 64-bit integers
** into between 1 and 9 bytes.  The encoding is designed so that small
** (and common) values take much less space that larger values.  Additional
** properties:
**
**    *  The length of the varint can be determined after examining just
**       the first byte of the encoding.
**
**    *  Varints compare in numerical order using memcmp().
**
**************************************************************************
**
** Treat each byte of the encoding as an integer between 0 and 255.
** Let the bytes of the encoding be called A0, A1, A2, ..., A8.
**
** DECODE
**
** If A0 is between 0 and 240 inclusive, then the result is the value of A0.
**
** If A0 is between 241 and 248 inclusive, then the result is
** 240+256*(A0-241)+A1.
**
** If A0 is 249 then the result is 2288+256*A1+A2.
**
** If A0 is 250 then the result is A1..A3 as a 3-byte big-endian integer.
**
** If A0 is 251 then the result is A1..A4 as a 4-byte big-endian integer.
**
** If A0 is 252 then the result is A1..A5 as a 5-byte big-endian integer.
**
** If A0 is 253 then the result is A1..A6 as a 6-byte big-endian integer.
**
** If A0 is 254 then the result is A1..A7 as a 7-byte big-endian integer.
**
** If A0 is 255 then the result is A1..A8 as a 8-byte big-endian integer.
**
** ENCODE
**
** Let the input value be V.
**
** If V<=240 then output a single by A0 equal to V.
**
** If V<=2287 then output A0 as (V-240)/256 + 241 and A1 as (V-240)%256.
**
** If V<=67823 then output A0 as 249, A1 as (V-2288)/256, and A2
** as (V-2288)%256.
**
** If V<=16777215 then output A0 as 250 and A1 through A3 as a big-endian
** 3-byte integer.
**
** If V<=4294967295 then output A0 as 251 and A1..A4 as a big-endian
** 4-byte integer.
**
** If V<=1099511627775 then output A0 as 252 and A1..A5 as a big-endian
** 5-byte integer.
**
** If V<=281474976710655 then output A0 as 253 and A1..A6 as a big-endian
** 6-byte integer.
**
** If V<=72057594037927935 then output A0 as 254 and A1..A7 as a
** big-endian 7-byte integer.
**
** Otherwise then output A0 as 255 and A1..A8 as a big-endian 8-byte integer.
**
** SUMMARY
**
**    Bytes    Max Value    Digits
**    -------  ---------    ---------
**      1      240           2.3
**      2      2287          3.3
**      3      67823         4.8
**      4      2**24-1       7.2
**      5      2**32-1       9.6
**      6      2**40-1      12.0
**      7      2**48-1      14.4
**      8      2**56-1      16.8
**      9      2**64-1      19.2
**
*/

/*
** Decode the varint in the first n bytes z[].  Write the integer value
** into *pResult and return the number of bytes in the varint.
**
** If the decode fails because there are not enough bytes in z[] then
** return 0;
*/
varintWidth varintTaggedGet(const uint8_t *__restrict z, int32_t n,
                            uint64_t *pResult) {
    if (n < 1) {
        return 0;
    }

    if (z[0] <= 240) {
        *pResult = z[0];
        return 1;
    }

    if (z[0] <= 248) {
        if (n < 2) {
            return 0;
        }

        *pResult = (z[0] - 241) * 256 + z[1] + 240;
        return 2;
    }

    if (n < z[0] - 246) {
        return 0;
    }

    switch (z[0]) {
    case 249:
        *pResult = 2288 + 256 * z[1] + z[2];
        return 3;
    case 250:
        *pResult = (z[1] << 16) | (z[2] << 8) | z[3];
        return 4;
    default: {
        uint64_t x =
            (((uint64_t)z[1]) << 24) | (z[2] << 16) | (z[3] << 8) | z[4];
        switch (z[0]) {
        case 251:
            *pResult = x;
            return 5;
        case 252:
            *pResult = (x << 8) | z[5];
            return 6;
        case 253:
            *pResult = (x << 16) | (z[5] << 8) | z[6];
            return 7;
        case 254:
            *pResult = (x << 24) | (z[5] << 16) | (z[6] << 8) | z[7];
            return 8;
        case 255:
            *pResult =
                (x << 32) | (0xffffffff & ((((uint64_t)z[5]) << 24) |
                                           (z[6] << 16) | (z[7] << 8) | z[8]));
            return 9;
        default:
            return 0;
        }
    }
    }
}

/* Return number of bytes used by varint 'z'.
 * This is just TaggedGet() with the retrieval code removed. It only
 * touches the first byte of 'z' */
varintWidth varintTaggedGetLen(const uint8_t *__restrict z) {
    if (z[0] <= 240) {
        return 1;
    }

    if (z[0] <= 248) {
        return 2;
    }

    /* z[0] of 249 is 3 bytes.
     * z[0] of 250 is 4 bytes.
     * .
     * .
     * .
     * z[0] of 255 is 9 bytes. */
    return z[0] - 246;
}

/*
** Write a 32-bit integer as 4 big-endian bytes.
*/
static void _varintWrite32(uint8_t *__restrict z, uint32_t y) {
    z[0] = (uint8_t)(y >> 24);
    z[1] = (uint8_t)(y >> 16);
    z[2] = (uint8_t)(y >> 8);
    z[3] = (uint8_t)(y);
}

/*
** Write a varint into z[].  The buffer z[] must be at least 9 characters
** long to accommodate the largest possible varint.  Return the number of
** bytes of z[] used.
*/
varintWidth varintTaggedPut64(uint8_t *__restrict z, uint64_t x) {
    uint32_t w, y;
    if (x <= 240) {
        z[0] = (uint8_t)x;
        return 1;
    }

    if (x <= 2287) {
        y = (uint32_t)(x - 240);
        z[0] = (uint8_t)(y / 256 + 241);
        z[1] = (uint8_t)(y % 256);
        return 2;
    }

    if (x <= 67823) {
        y = (uint32_t)(x - 2288);
        z[0] = 249;
        z[1] = (uint8_t)(y / 256);
        z[2] = (uint8_t)(y % 256);
        return 3;
    }

    y = (uint32_t)x;
    w = (uint32_t)(x >> 32);
    if (w == 0) {
        if (y <= 16777215) {
            z[0] = 250;
            z[1] = (uint8_t)(y >> 16);
            z[2] = (uint8_t)(y >> 8);
            z[3] = (uint8_t)(y);
            return 4;
        }

        z[0] = 251;
        _varintWrite32(z + 1, y);
        return 5;
    }

    if (w <= 255) {
        z[0] = 252;
        z[1] = (uint8_t)w;
        _varintWrite32(z + 2, y);
        return 6;
    }

    if (w <= 65535) {
        z[0] = 253;
        z[1] = (uint8_t)(w >> 8);
        z[2] = (uint8_t)w;
        _varintWrite32(z + 3, y);
        return 7;
    }

    if (w <= 16777215) {
        z[0] = 254;
        z[1] = (uint8_t)(w >> 16);
        z[2] = (uint8_t)(w >> 8);
        z[3] = (uint8_t)w;
        _varintWrite32(z + 4, y);
        return 8;
    }

    z[0] = 255;
    _varintWrite32(z + 1, w);
    _varintWrite32(z + 5, y);
    return 9;
}

varintWidth varintTaggedPut64FixedWidth(uint8_t *__restrict z, uint64_t x,
                                        varintWidth width) {
    uint32_t y;
    switch (width) {
    case 1:
        z[0] = (uint8_t)x;
        return 1;
    case 2:
        y = (uint32_t)(x - 240);
        z[0] = (uint8_t)(y / 256 + 241);
        z[1] = (uint8_t)(y % 256);
        return 2;
    case 3:
        y = (uint32_t)(x - 2288);
        z[0] = 249;
        z[1] = (uint8_t)(y / 256);
        z[2] = (uint8_t)(y % 256);
        return 3;
    default: {
        y = (uint32_t)x;
        uint32_t w = (uint32_t)(x >> 32);
        switch (width) {
        case 4:
            z[0] = 250;
            z[1] = (uint8_t)(y >> 16);
            z[2] = (uint8_t)(y >> 8);
            z[3] = (uint8_t)(y);
            return 4;
        case 5:
            z[0] = 251;
            _varintWrite32(z + 1, y);
            return 5;
        case 6:
            z[0] = 252;
            z[1] = (uint8_t)w;
            _varintWrite32(z + 2, y);
            return 6;
        case 7:
            z[0] = 253;
            z[1] = (uint8_t)(w >> 8);
            z[2] = (uint8_t)w;
            _varintWrite32(z + 3, y);
            return 7;
        case 8:
            z[0] = 254;
            z[1] = (uint8_t)(w >> 16);
            z[2] = (uint8_t)(w >> 8);
            z[3] = (uint8_t)w;
            _varintWrite32(z + 4, y);
            return 8;
        case 9:
            z[0] = 255;
            _varintWrite32(z + 1, w);
            _varintWrite32(z + 5, y);
            return 9;
        default:
            return 0;
        }
    }
    }
}

/*
** Return the number of bytes required to encode value v as a varint.
** This is just Put64 without the storage operations.
*/
varintWidth varintTaggedLen(uint64_t x) {
    if (x <= 240) {
        return 1;
    }

    if (x <= 2287) {
        return 2;
    }

    if (x <= 67823) {
        return 3;
    }

    uint32_t y = (uint32_t)x;
    uint32_t w = (uint32_t)(x >> 32);
    if (w == 0) {
        if (y <= 16777215) {
            return 4;
        }

        return 5;
    }

    if (w <= 255) {
        return 6;
    }

    if (w <= 65535) {
        return 7;
    }

    if (w <= 16777215) {
        return 8;
    }

    return 9;
}

/*
** Read a varint from buffer z and set *pResult to the value read.
** Return the number of bytes read from the buffer.
*/
varintWidth varintTaggedGetVarint32(const uint8_t *z, uint32_t *pResult) {
    uint64_t iRes = 0;
    varintWidth ret = varintTaggedGet(z, 9, &iRes);
    *pResult = (uint32_t)iRes;
    return ret;
}

/*
** Read a varint from buffer z and set *pResult to the value read.
** Return the number of bytes read from the buffer.
*/
varintWidth varintTaggedGet64(const uint8_t *z, uint64_t *pResult) {
    return varintTaggedGet(z, 9, pResult);
}

uint64_t varintTaggedGet64ReturnValue(const uint8_t *z) {
    uint64_t result = 0;
    varintTaggedGet(z, 9, &result);
    return result;
}
/*
** Encode v as a varint and write the result to buffer p. Return the
** number of bytes written.
*/
varintWidth varintTaggedPutVarint32(uint8_t *p, uint32_t v) {
    return varintTaggedPut64(p, v);
}

static varintWidth varintTaggedAdd(uint8_t *p, int64_t add, bool force) {
    /* we're pulling the value out as just an 'int64_t' because we want to
     * allow signed math. */
    uint64_t retrieve = 0;
    varintWidth origEncoding = varintTaggedGet64(p, &retrieve);
    int64_t updatingVal = (int64_t)retrieve;
    long long newVal;

    VARINT_ADD_OR_ABORT_OVERFLOW_(updatingVal, add, newVal);

    varintWidth newEncoding = varintTaggedLen(newVal);

    /* If new encoding is larger than current encoding, we don't
     * want to overwrite memory beyond our current varint.
     * Bail out unless this was requested as "safe to grow" addition. */
    if (newEncoding > origEncoding && !force) {
        return newEncoding;
    }

    varintTaggedPut64(p, newVal);
    return newEncoding;
}

/* If math can't fit into current encoding, fail the write and return
 * the new encoding length we need for this math to complete.
 * (Then the user can manually run the add to update.) */
varintWidth varintTaggedAddNoGrow(uint8_t *p, int64_t add) {
    return varintTaggedAdd(p, add, false);
}

varintWidth varintTaggedAddGrow(uint8_t *p, int64_t add) {
    return varintTaggedAdd(p, add, true);
}
