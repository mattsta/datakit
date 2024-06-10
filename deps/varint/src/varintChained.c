/*
** 2001 September 15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
** This was originally extracted from util.c inside varintChained as of commit:
**      "2015-09-01 [adf9fefb] — improvement in varintChainedVarintLen()."
**
*************************************************************************
*/

#include "varintChained.h"
#include "varint.h"
#include <stdarg.h>

/*
 * ** Constants for the largest and smallest possible 64-bit signed integers.
 * ** These macros are designed to work correctly on both 32-bit and 64-bit
 * ** compilers.
 * */
#define LARGEST_INT64 (0xffffffff | (((int64_t)0x7fffffff) << 32))
#define SMALLEST_INT64 (((int64_t)-1) - LARGEST_INT64)

/*
** SQLITE_MAX_U32 is a u64 constant that is the maximum u64 value
** that can be stored in a u32 without loss of data.  The value
** is 0x00000000ffffffff.  But because of quirks of some compilers, we
** have to specify the value in the less intuitive manner shown:
*/
#define SQLITE_MAX_U32 ((((uint64_t)1) << 32) - 1)

/*
** The variable-length integer encoding is as follows:
**
** KEY:
**         A = 0xxxxxxx    7 bits of data and one flag bit
**         B = 1xxxxxxx    7 bits of data and one flag bit
**         C = xxxxxxxx    8 bits of data
**
**  7 bits - A
** 14 bits - BA
** 21 bits - BBA
** 28 bits - BBBA
** 35 bits - BBBBA
** 42 bits - BBBBBA
** 49 bits - BBBBBBA
** 56 bits - BBBBBBBA
** 64 bits - BBBBBBBBC
*/

/*
** Write a 64-bit variable-length integer to memory starting at p[0].
** The length of data write will be between 1 and 9 bytes.  The number
** of bytes written is returned.
**
** A variable-length integer consists of the lower 7 bits of each byte
** for all bytes that have the 8th bit set and one byte with the 8th
** bit clear.  Except, if we get to the 9th byte, it stores the full
** 8 bits and is the last byte.
*/
static varintWidth __attribute__((noinline))
putVarint64(uint8_t *p, uint64_t v) {
    int32_t i, j, n;
    uint8_t buf[10];
    if (v & (((uint64_t)0xff000000) << 32)) {
        p[8] = (uint8_t)v;
        v >>= 8;
        for (i = 7; i >= 0; i--) {
            p[i] = (uint8_t)((v & 0x7f) | 0x80);
            v >>= 7;
        }

        return 9;
    }

    n = 0;
    do {
        buf[n++] = (uint8_t)((v & 0x7f) | 0x80);
        v >>= 7;
    } while (v != 0);
    buf[0] &= 0x7f;
    assert(n <= 9);
    for (i = 0, j = n - 1; j >= 0; j--, i++) {
        p[i] = buf[j];
    }

    return n;
}

varintWidth varintChainedPutVarint(uint8_t *p, uint64_t v) {
    if (v <= 0x7f) {
        p[0] = v & 0x7f;
        return 1;
    }

    if (v <= 0x3fff) {
        p[0] = ((v >> 7) & 0x7f) | 0x80;
        p[1] = v & 0x7f;
        return 2;
    }

    return putVarint64(p, v);
}

/*
** Bitmasks used by varintChainedGetVarint().  These precomputed constants
** are defined here rather than simply putting the constant expressions
** inline in order to work around bugs in the RVT compiler.
**
** SLOT_2_0     A mask for  (0x7f<<14) | 0x7f
**
** SLOT_4_2_0   A mask for  (0x7f<<28) | SLOT_2_0
*/
#define SLOT_2_0 0x001fc07f
#define SLOT_4_2_0 0xf01fc07f

/*
** Read a 64-bit variable-length integer from memory starting at p[0].
** Return the number of bytes read.  The value is stored in *v.
*/
varintWidth varintChainedGetVarint(const uint8_t *p, uint64_t *v) {
    uint32_t a, b, s;

    if (((int8_t *)p)[0] >= 0) {
        *v = *p;
        return 1;
    }

    if (((int8_t *)p)[1] >= 0) {
        *v = ((uint32_t)(p[0] & 0x7f) << 7) | p[1];
        return 2;
    }

    /* Verify that constants are precomputed correctly */
    assert(SLOT_2_0 == ((0x7f << 14) | (0x7f)));
    assert(SLOT_4_2_0 == ((0xfU << 28) | (0x7f << 14) | (0x7f)));

    a = ((uint32_t)p[0]) << 14;
    b = p[1];
    p += 2;
    a |= *p;
    /* a: p0<<14 | p2 (unmasked) */
    if (!(a & 0x80)) {
        a &= SLOT_2_0;
        b &= 0x7f;
        b = b << 7;
        a |= b;
        *v = a;
        return 3;
    }

    /* CSE1 from below */
    a &= SLOT_2_0;
    p++;
    b = b << 14;
    b |= *p;
    /* b: p1<<14 | p3 (unmasked) */
    if (!(b & 0x80)) {
        b &= SLOT_2_0;
        /* moved CSE1 up */
        /* a &= (0x7f<<14)|(0x7f); */
        a = a << 7;
        a |= b;
        *v = a;
        return 4;
    }

    /* a: p0<<14 | p2 (masked) */
    /* b: p1<<14 | p3 (unmasked) */
    /* 1:save off p0<<21 | p1<<14 | p2<<7 | p3 (masked) */
    /* moved CSE1 up */
    /* a &= (0x7f<<14)|(0x7f); */
    b &= SLOT_2_0;
    s = a;
    /* s: p0<<14 | p2 (masked) */

    p++;
    a = a << 14;
    a |= *p;
    /* a: p0<<28 | p2<<14 | p4 (unmasked) */
    if (!(a & 0x80)) {
        /* we can skip these cause they were (effectively) done above in
         * calc'ing s */
        /* a &= (0x7f<<28)|(0x7f<<14)|(0x7f); */
        /* b &= (0x7f<<14)|(0x7f); */
        b = b << 7;
        a |= b;
        s = s >> 18;
        *v = ((uint64_t)s) << 32 | a;
        return 5;
    }

    /* 2:save off p0<<21 | p1<<14 | p2<<7 | p3 (masked) */
    s = s << 7;
    s |= b;
    /* s: p0<<21 | p1<<14 | p2<<7 | p3 (masked) */

    p++;
    b = b << 14;
    b |= *p;
    /* b: p1<<28 | p3<<14 | p5 (unmasked) */
    if (!(b & 0x80)) {
        /* we can skip this cause it was (effectively) done above in calc'ing s
         */
        /* b &= (0x7f<<28)|(0x7f<<14)|(0x7f); */
        a &= SLOT_2_0;
        a = a << 7;
        a |= b;
        s = s >> 18;
        *v = ((uint64_t)s) << 32 | a;
        return 6;
    }

    p++;
    a = a << 14;
    a |= *p;
    /* a: p2<<28 | p4<<14 | p6 (unmasked) */
    if (!(a & 0x80)) {
        a &= SLOT_4_2_0;
        b &= SLOT_2_0;
        b = b << 7;
        a |= b;
        s = s >> 11;
        *v = ((uint64_t)s) << 32 | a;
        return 7;
    }

    /* CSE2 from below */
    a &= SLOT_2_0;
    p++;
    b = b << 14;
    b |= *p;
    /* b: p3<<28 | p5<<14 | p7 (unmasked) */
    if (!(b & 0x80)) {
        b &= SLOT_4_2_0;
        /* moved CSE2 up */
        /* a &= (0x7f<<14)|(0x7f); */
        a = a << 7;
        a |= b;
        s = s >> 4;
        *v = ((uint64_t)s) << 32 | a;
        return 8;
    }

    p++;
    a = a << 15;
    a |= *p;
    /* a: p4<<29 | p6<<15 | p8 (unmasked) */

    /* moved CSE2 up */
    /* a &= (0x7f<<29)|(0x7f<<15)|(0xff); */
    b &= SLOT_2_0;
    b = b << 8;
    a |= b;

    s = s << 4;
    b = p[-4];
    b &= 0x7f;
    b = b >> 3;
    s |= b;

    *v = ((uint64_t)s) << 32 | a;

    return 9;
}

/*
** Read a 32-bit variable-length integer from memory starting at p[0].
** Return the number of bytes read.  The value is stored in *v.
**
** If the varint32_t stored in p[0] is larger than can fit in a 32-bit unsigned
** integer, then set *v to 0xffffffff.
**
** A MACRO version, getVarint32, is provided which inlines the
** single-byte case.  All code should use the MACRO version as
** this function assumes the single-byte case has already been handled.
*/
varintWidth varintChainedGetVarint32(const uint8_t *p, uint32_t *v) {
    uint32_t a, b;

    /* The 1-byte case.  Overwhelmingly the most common.  Handled inline
    ** by the getVarin32() macro */
    a = *p;
/* a: p0 (unmasked) */
#ifndef varintChained_getVarint32
    if (!(a & 0x80)) {
        /* Values between 0 and 127 */
        *v = a;
        return 1;
    }
#endif

    /* The 2-byte case */
    p++;
    b = *p;
    /* b: p1 (unmasked) */
    if (!(b & 0x80)) {
        /* Values between 128 and 16383 */
        a &= 0x7f;
        a = a << 7;
        *v = a | b;
        return 2;
    }

    /* The 3-byte case */
    p++;
    a = a << 14;
    a |= *p;
    /* a: p0<<14 | p2 (unmasked) */
    if (!(a & 0x80)) {
        /* Values between 16384 and 2097151 */
        a &= (0x7f << 14) | (0x7f);
        b &= 0x7f;
        b = b << 7;
        *v = a | b;
        return 3;
    }

/* A 32-bit varint32_t is used to store size information in btrees.
** Objects are rarely larger than 2MiB limit of a 3-byte varint.
** A 3-byte varint32_t is sufficient, for example, to record the size
** of a 1048569-byte BLOB or string.
**
** We only unroll the first 1-, 2-, and 3- byte cases.  The very
** rare larger cases can be handled by the slower 64-bit varint
** routine.
*/
#if 1
    {
        uint64_t v64;
        uint8_t n;

        p -= 2;
        n = varintChainedGetVarint(p, &v64);
        assert(n > 3 && n <= 9);
        if ((v64 & SQLITE_MAX_U32) != v64) {
            *v = 0xffffffff;
        } else {
            *v = (uint32_t)v64;
        }

        return n;
    }

#else
    /* For following code (kept for historical record only) shows an
    ** unrolling for the 3- and 4-byte varint32_t cases.  This code is
    ** slightly faster, but it is also larger and much harder to test.
    */
    p++;
    b = b << 14;
    b |= *p;
    /* b: p1<<14 | p3 (unmasked) */
    if (!(b & 0x80)) {
        /* Values between 2097152 and 268435455 */
        b &= (0x7f << 14) | (0x7f);
        a &= (0x7f << 14) | (0x7f);
        a = a << 7;
        *v = a | b;
        return 4;
    }

    p++;
    a = a << 14;
    a |= *p;
    /* a: p0<<28 | p2<<14 | p4 (unmasked) */
    if (!(a & 0x80)) {
        /* Values  between 268435456 and 34359738367 */
        a &= SLOT_4_2_0;
        b &= SLOT_4_2_0;
        b = b << 7;
        *v = a | b;
        return 5;
    }

    /* We can only reach this point32_t when reading a corrupt database
    ** file.  In that case we are not in any hurry.  Use the (relatively
    ** slow) general-purpose varintChainedGetVarint() routine to extract the
    ** value. */
    {
        uint64_t v64;
        uint8_t n;

        p -= 4;
        n = varintChainedGetVarint(p, &v64);
        assert(n > 5 && n <= 9);
        *v = (uint32_t)v64;
        return n;
    }
#endif
}

/*
** Return the number of bytes that will be needed to store the given
** 64-bit integer.
*/
varintWidth varintChainedVarintLen(uint64_t v) {
    varintWidth i = VARINT_WIDTH_8B;
    while (v >>= 7) {
        i++;
    }

    /* The loop will create a 10 byte length for the extreme
     * upper range of 64 bit integers, but our varints encode
     * the last byte with 8 bits, so all 10 byte length
     * calculations are actually 9 byte varints. */
    return i > 9 ? 9 : i;
}
