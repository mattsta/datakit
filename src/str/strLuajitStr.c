#include "../str.h"

/* ====================================================================
 * String to Number Conversion (from luajit 2.0)
 * ==================================================================== */
/*
** String scanning.
** Copyright (C) 2005-2015 Mike Pall. See Copyright Notice in luajit.h
*/
/* All of luajit is MIT licensed. */

#include <math.h>

/*
** Rationale for the builtin string to number conversion library:
**
** It removes a dependency on libc's strtod(), which is a true portability
** nightmare. Mainly due to the plethora of supported OS and toolchain
** combinations. Sadly, the various implementations
** a) are often buggy, incomplete (no hex floats) and/or imprecise,
** b) sometimes crash or hang on certain inputs,
** c) return non-standard NaNs that need to be filtered out, and
** d) fail if the locale-specific decimal separator is not a dot,
**    which can only be fixed with atrocious workarounds.
**
** Also, most of the strtod() implementations are hopelessly bloated,
** which is not just an I-cache hog, but a problem for static linkage
** on embedded systems, too.
**
** OTOH the builtin conversion function is very compact. Even though it
** does a lot more, like parsing long longs, octal or imaginary numbers
** and returning the result in different formats:
** a) It needs less than 3 KB (!) of machine code (on x64 with -Os),
** b) it doesn't perform any dynamic allocation and,
** c) it needs only around 600 bytes of stack space.
**
** The builtin function is faster than strtod() for typical inputs, e.g.
** "123", "1.5" or "1e6". Arguably, it's slower for very large exponents,
** which are not very common (this could be fixed, if needed).
**
** And most importantly, the builtin function is equally precise on all
** platforms. It correctly converts and rounds any input to a double.
** If this is not the case, please send a bug report -- but PLEASE verify
** that the implementation you're comparing to is not the culprit!
**
** The implementation quickly pre-scans the entire string first and
** handles simple integers on-the-fly. Otherwise, it dispatches to the
** base-specific parser. Hex and octal are simple.
**
** Decimal to binary conversion uses a fixed-length circular buffer in
** base 100. Some simple cases are handled directly. For other cases, the
** number in the buffer is up-scaled or down-scaled until the integer part
** is in the proper range. Then the integer part is rounded and converted
** to a double which is finally rescaled to the result. Denormals need
** special treatment to prevent incorrect 'double rounding'.
*/

/* Definitions for circular decimal digit buffer (base 100 = 2 digits/byte). */
#define STRSCAN_DIG 1024
#define STRSCAN_MAXDIG 800 /* 772 + extra are sufficient. */
#define STRSCAN_DDIG (STRSCAN_DIG / 2)
#define STRSCAN_DMASK (STRSCAN_DDIG - 1)

/* Helpers for circular buffer. */
#define DNEXT(a) (((a) + 1) & STRSCAN_DMASK)
#define DPREV(a) (((a)-1) & STRSCAN_DMASK)
#define DLEN(lo, hi) ((int32_t)(((lo) - (hi)) & STRSCAN_DMASK))

#define casecmp(c, k) (((c) | 0x20) == (k))

/* Final conversion to double. */
DK_INLINE_ALWAYS void StrScanDouble(uint64_t x, databox *box, int32_t ex2,
                                    int32_t neg) {
    /* Avoid double rounding for denormals. */
    if (unlikely(ex2 <= -1075 && x != 0)) {
/* NYI: all of this generates way too much code on 32 bit CPUs. */
#if DK_HAS_CLZ
        int32_t b = (int32_t)(__builtin_clzll(x) ^ 63);
#else
#error "We don't have a fallback non-intrinsic clz here."
#endif
        if ((int32_t)b + ex2 <= -1023 && (int32_t)b + ex2 >= -1075) {
            uint64_t rb = (uint64_t)1 << (-1075 - ex2);
            if ((x & rb) && ((x & (rb + rb + rb - 1)))) {
                x += rb + rb;
            }

            x = (x & ~(rb + rb - 1));
        }
    }

    /* Convert to double using a signed int64_t conversion, then rescale. */
    assert((int64_t)x >= 0);
    double n = (double)(int64_t)x;
    if (neg) {
        n = -n;
    }

    if (ex2) {
        n = ldexp(n, ex2);
    }

    DATABOX_SET_DOUBLE(box, n);
}

#define U64x(hi, lo) (((uint64_t)0x##hi << 32) + (uint64_t)0x##lo)

/* Parse hexadecimal number. */
DK_STATIC StrScanFmt StrScanHex(const uint8_t *p, databox *box, StrScanFmt fmt,
                                StrScanOpt opt, int32_t ex2, int32_t neg,
                                uint32_t dig) {
    uint64_t x = 0;
    uint32_t i;

    /* Scan hex digits. */
    for (i = dig > 16 ? 16 : dig; i; i--, p++) {
        uint32_t d = (*p != '.' ? *p : *++p);
        if (d > '9') {
            d += 9;
        }

        x = (x << 4) + (d & 15);
    }

    /* Summarize rounding-effect of excess digits. */
    for (i = 16; i < dig; i++, p++) {
        x |= ((*p != '.' ? *p : *++p) != '0'), ex2 += 4;
    }

    /* Format-specific handling. */
    switch (fmt) {
    case STRSCAN_INT:
        if (!(opt & STRSCAN_OPT_TONUM) && x < 0x80000000u + neg) {
            DATABOX_SET_SIGNED(box, neg ? -(int32_t)x : (int32_t)x);
            return STRSCAN_INT; /* Fast path for 32 bit integers. */
        }

        if (!(opt & STRSCAN_OPT_C)) {
            fmt = STRSCAN_NUM;
            break;
        }

    /* fallthrough */
    case STRSCAN_U32:
        if (dig > 8) {
            return STRSCAN_ERROR;
        }

        DATABOX_SET_UNSIGNED(box, neg ? -(int32_t)x : (int32_t)x);
        return STRSCAN_U32;
    case STRSCAN_I64:
    case STRSCAN_U64:
        if (dig > 16) {
            return STRSCAN_ERROR;
        }

        DATABOX_SET_UNSIGNED(box, neg ? (uint64_t) - (int64_t)x : x);
        return fmt;
    default:
        break;
    }

    /* Reduce range then convert to double. */
    if ((x & U64x(c0000000, 0000000))) {
        x = (x >> 2) | (x & 3);
        ex2 += 2;
    }

    StrScanDouble(x, box, ex2, neg);
    return fmt;
}

/* Parse octal number. */
DK_STATIC StrScanFmt StrScanOctal(const uint8_t *p, databox *box,
                                  StrScanFmt fmt, int32_t neg, uint32_t dig) {
    uint64_t x = 0;

    /* Scan octal digits. */
    if (dig > 22 || (dig == 22 && *p > '1')) {
        return STRSCAN_ERROR;
    }

    while (dig-- > 0) {
        if (!(*p >= '0' && *p <= '7')) {
            return STRSCAN_ERROR;
        }

        x = (x << 3) + (*p++ & 7);
    }

    /* Format-specific handling. */
    switch (fmt) {
    case STRSCAN_INT:
        if (x >= 0x80000000u + neg) {
            fmt = STRSCAN_U32;
        }

    /* fallthrough */
    case STRSCAN_U32:
        if ((x >> 32)) {
            return STRSCAN_ERROR;
        }

        DATABOX_SET_SIGNED(box, neg ? -(int32_t)x : (int32_t)x);
        break;
    default:
    case STRSCAN_I64:
    case STRSCAN_U64:
        DATABOX_SET_UNSIGNED(box, neg ? (uint64_t) - (int64_t)x : x);
        break;
    }

    return fmt;
}

/* Parse decimal number. */
DK_STATIC StrScanFmt StrScanDecimal(const uint8_t *p, databox *box,
                                    StrScanFmt fmt, StrScanOpt opt,
                                    int32_t ex10, int32_t neg, uint32_t dig) {
    uint8_t xi[STRSCAN_DDIG];
    uint8_t *xip = xi;

    if (dig) {
        uint32_t i = dig;
        if (i > STRSCAN_MAXDIG) {
            ex10 += (int32_t)(i - STRSCAN_MAXDIG);
            i = STRSCAN_MAXDIG;
        }

        /* Scan unaligned leading digit. */
        if (((ex10 ^ i) & 1)) {
            *xip++ = ((*p != '.' ? *p : *++p) & 15);
            i--;
            p++;
        }

        /* Scan aligned double-digits. */
        for (; i > 1; i -= 2) {
            uint32_t d = 10 * ((*p != '.' ? *p : *++p) & 15);
            p++;
            *xip++ = d + ((*p != '.' ? *p : *++p) & 15);
            p++;
        }

        /* Scan and realign trailing digit. */
        if (i) {
            *xip++ = 10 * ((*p != '.' ? *p : *++p) & 15);
            ex10--;
            dig++;
            p++;
        }

        /* Summarize rounding-effect of excess digits. */
        if (dig > STRSCAN_MAXDIG) {
            do {
                if ((*p != '.' ? *p : *++p) != '0') {
                    xip[-1] |= 1;
                    break;
                }

                p++;
            } while (--dig > STRSCAN_MAXDIG);
            dig = STRSCAN_MAXDIG;
        } else { /* Simplify exponent. */
            while (ex10 > 0 && dig <= 18) {
                *xip++ = 0;
                ex10 -= 2;
                dig += 2;
            }
        }
    } else { /* Only got zeros. */
        ex10 = 0;
        xi[0] = 0;
    }

    /* Fast path for numbers in integer format (but handles e.g. 1e6, too). */
    if (dig <= 20 && ex10 == 0) {
        uint64_t x = xi[0];
        double n;
        _Pragma("GCC diagnostic push") _Pragma(
            "GCC diagnostic ignored \"-Wstrict-overflow\"") for (uint8_t *xis =
                                                                     xi + 1;
                                                                 xis < xip;
                                                                 xis++) {
            x = StrValTimes100(x) + *xis;
        }
        _Pragma("GCC diagnostic pop")

            if (!(dig == 20 &&
                  (xi[0] > 18 || (int64_t)x >= 0))) { /* No overflow? */
            /* Format-specific handling. */
            switch (fmt) {
            case STRSCAN_INT:
                if (!(opt & STRSCAN_OPT_TONUM) && x < 0x80000000u + neg) {
                    DATABOX_SET_SIGNED(box, neg ? -(int32_t)x : (int32_t)x);
                    return STRSCAN_INT; /* Fast path for 32 bit integers. */
                }

                if (!(opt & STRSCAN_OPT_C)) {
                    fmt = STRSCAN_NUM;
                    goto plainnumber;
                }

            /* fallthrough */
            case STRSCAN_U32:
                if ((x >> 32) != 0) {
                    return STRSCAN_ERROR;
                }

                DATABOX_SET_UNSIGNED(box, neg ? -(int32_t)x : (int32_t)x);
                return STRSCAN_U32;
            case STRSCAN_I64:
            case STRSCAN_U64:
                DATABOX_SET_UNSIGNED(box, neg ? (uint64_t) - (int64_t)x : x);
                return fmt;
            default:
            plainnumber: /* Fast path for plain numbers < 2^63. */
                if ((int64_t)x < 0) {
                    break;
                }

                n = (double)(int64_t)x;
                if (neg) {
                    n = -n;
                }

                DATABOX_SET_DOUBLE(box, n);
                return fmt;
            }
        }
    }

    /* Slow non-integer path. */
    if (fmt == STRSCAN_INT) {
        if ((opt & STRSCAN_OPT_C)) {
            return STRSCAN_ERROR;
        }

        fmt = STRSCAN_NUM;
    } else if (fmt > STRSCAN_INT) {
        return STRSCAN_ERROR;
    }

    {
        uint32_t hi = 0;
        uint32_t lo = (uint32_t)(xip - xi);
        int32_t ex2 = 0;
        int32_t idig = (int32_t)lo + (ex10 >> 1);

        assert(lo > 0 && (ex10 & 1) == 0);

        /* Handle simple overflow/underflow. */
        if (idig > 310 / 2) {
            if (neg) {
                *box = DATABOX_INFINITY_NEGATIVE;
            } else {
                *box = DATABOX_INFINITY_POSITIVE;
            }
            return fmt;
        }

        if (idig < -326 / 2) {
            DATABOX_SET_DOUBLE(box, neg ? -0.0 : 0.0);
            return fmt;
        }

        /* Scale up until we have at least 17 or 18 integer part digits. */
        while (idig < 9 && idig < DLEN(lo, hi)) {
            uint32_t i;
            uint32_t cy = 0;
            ex2 -= 6;
            for (i = DPREV(lo);; i = DPREV(i)) {
                uint32_t d = (xi[i] << 6) + cy;
                cy = (((d >> 2) * 5243) >> 17);
                d = d - StrValTimes100(cy); /* Div/mod 100. */
                xi[i] = (uint8_t)d;
                if (i == hi) {
                    break;
                }

                if (d == 0 && i == DPREV(lo)) {
                    lo = i;
                }
            }

            if (cy) {
                hi = DPREV(hi);
                if (xi[DPREV(lo)] == 0) {
                    lo = DPREV(lo);
                } else if (hi == lo) {
                    lo = DPREV(lo);
                    xi[DPREV(lo)] |= xi[lo];
                }

                xi[hi] = (uint8_t)cy;
                idig++;
            }
        }

        /* Scale down until no more than 17 or 18 integer part digits remain. */
        while (idig > 9) {
            uint32_t i = hi;
            uint32_t cy = 0;
            ex2 += 6;
            do {
                cy += xi[i];
                xi[i] = (cy >> 6);
                cy = 100 * (cy & 0x3f);
                if (xi[i] == 0 && i == hi) {
                    hi = DNEXT(hi);
                    idig--;
                }

                i = DNEXT(i);
            } while (i != lo);

            while (cy) {
                if (hi == lo) {
                    xi[DPREV(lo)] |= 1;
                    break;
                }

                xi[lo] = (cy >> 6);
                lo = DNEXT(lo);
                cy = 100 * (cy & 0x3f);
            }
        }

        /* Collect integer part digits and convert to rescaled double. */
        {
            uint64_t x = xi[hi];
            uint32_t i;
            for (i = DNEXT(hi); --idig > 0 && i != lo; i = DNEXT(i)) {
                x = StrValTimes100(x) + xi[i];
            }

            if (i == lo) {
                while (--idig >= 0) {
                    x = StrValTimes100(x);
                }
            } else { /* Gather round bit from remaining digits. */
                x <<= 1;
                ex2--;
                do {
                    if (xi[i]) {
                        x |= 1;
                        break;
                    }

                    i = DNEXT(i);
                } while (i != lo);
            }

            StrScanDouble(x, box, ex2, neg);
        }
    }
    return fmt;
}

/* The table below is from luajit and is similar in purpose (but not similar
 * in physical layout) to the sqlite table-based character conversions at
 * the start of this file. */
/*
** Character types.
** Donated to the public domain.
**
** This is intended to replace the problematic libc single-byte NLS functions.
** These just don't make sense anymore with UTF-8 locales becoming the norm
** on POSIX systems. It never worked too well on Windows systems since hardly
** anyone bothered to call setlocale().
**
** This table is hardcoded for ASCII. Identifiers include the characters
** 128-255, too. This allows for the use of all non-ASCII chars as identifiers
** in the lexer. This is a broad definition, but works well in practice
** for both UTF-8 locales and most single-byte locales (such as ISO-8859-*).
**
** If you really need proper character types for UTF-8 strings, please use
** an add-on library such as slnunicode: http://luaforge.net/projects/sln/
*/
DK_STATIC const uint8_t StrCharBits[257] = {
    0,   1,   1,   1,   1,   1,   1,   1,   1,   1,   3,   3,   3,   3,   3,
    1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,
    1,   1,   1,   2,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,   4,
    4,   4,   4,   4,   152, 152, 152, 152, 152, 152, 152, 152, 152, 152, 4,
    4,   4,   4,   4,   4,   4,   176, 176, 176, 176, 176, 176, 160, 160, 160,
    160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160, 160,
    160, 160, 4,   4,   4,   4,   132, 4,   208, 208, 208, 208, 208, 208, 192,
    192, 192, 192, 192, 192, 192, 192, 192, 192, 192, 192, 192, 192, 192, 192,
    192, 192, 192, 192, 4,   4,   4,   4,   1,   128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
    128, 128};

typedef enum StrChar {
    StrCharCNTRL = 0x01,
    StrCharSPACE = 0x02,
    StrCharPUNCT = 0x04,
    StrCharDIGIT = 0x08,
    StrCharXDIGIT = 0x10,
    StrCharUPPER = 0x20,
    StrCharLOWER = 0x40,
    StrCharIDENT = 0x80,
    StrCharALPHA = (StrCharLOWER | StrCharUPPER),
    StrCharALNUM = (StrCharALPHA | StrCharDIGIT),
    StrCharGRAPH = (StrCharALNUM | StrCharPUNCT)
} StrChar;

/* Only pass -1 or 0..255 to these macros. Never pass a signed char! */
#define StrCharIsa(c, t) ((StrCharBits + 1)[(c)] & (t))
#define StrCharIsspace(c) StrCharIsa((c), StrCharSPACE)
#define StrCharIsdigit(c) StrCharIsa((c), StrCharDIGIT)

/* These are available, but unused. */
#if 0
#define StrCharIscntrl(c) StrCharIsa((c), StrCharCNTRL)
#define StrCharIspunct(c) StrCharIsa((c), StrCharPUNCT)
#define StrCharIsxdigit(c) StrCharIsa((c), StrCharXDIGIT)
#define StrCharIsupper(c) StrCharIsa((c), StrCharUPPER)
#define StrCharIslower(c) StrCharIsa((c), StrCharLOWER)
#define StrCharIsident(c) StrCharIsa((c), StrCharIDENT)
#define StrCharIsalpha(c) StrCharIsa((c), StrCharALPHA)
#define StrCharIsalnum(c) StrCharIsa((c), StrCharALNUM)
#define StrCharIsgraph(c) StrCharIsa((c), StrCharGRAPH)

#define StrCharToupper(c) ((c) - (StrCharIslower(c) >> 1))
#define StrCharTolower(c) ((c) + StrCharIsupper(c))
#endif

/* Scan string containing a number. Returns format. Returns value in box. */
StrScanFmt StrScanScan(const uint8_t *p, databox *box, StrScanOpt opt,
                       const bool allowFloatWords, const bool skipSpaces) {
    int32_t neg = 0;

    /* Remove leading space, parse sign and non-numbers. */
    if (allowFloatWords && !StrCharIsdigit(*p)) {
        if (skipSpaces) {
            while (StrCharIsspace(*p)) {
                p++;
            }
        }

        if (*p == '+' || *p == '-') {
            neg = (*p++ == '-');
        }

        if (unlikely(*p >= 'A')) { /* Parse "inf", "infinity" or "nan". */
            databox tmp = DATABOX_NAN;
            if (casecmp(p[0], 'i') && casecmp(p[1], 'n') &&
                casecmp(p[2], 'f')) {
                if (neg) {
                    tmp.data.d64 = -INFINITY;
                } else {
                    tmp.data.d64 = INFINITY;
                }

                p += 3;
                if (casecmp(p[0], 'i') && casecmp(p[1], 'n') &&
                    casecmp(p[2], 'i') && casecmp(p[3], 't') &&
                    casecmp(p[4], 'y')) {
                    p += 5;
                }
            } else if (casecmp(p[0], 'n') && casecmp(p[1], 'a') &&
                       casecmp(p[2], 'n')) {
                p += 3;
            }

            if (skipSpaces) {
                while (StrCharIsspace(*p)) {
                    p++;
                }
            }

            if (*p) {
                return STRSCAN_ERROR;
            }

            *box = tmp;
            return STRSCAN_NUM;
        }
    }

    /* Parse regular number. */
    {
        StrScanFmt fmt = STRSCAN_INT;
        int cmask = StrCharDIGIT;
        int base = (opt & STRSCAN_OPT_C) && *p == '0' ? 0 : 10;
        const uint8_t *sp;
        const uint8_t *dp = NULL;
        uint32_t dig = 0;
        uint32_t hasdig = 0;
        uint32_t x = 0;
        int32_t ex = 0;

        /* Determine base and skip leading zeros. */
        if (unlikely(*p <= '0')) {
            if (*p == '0' && casecmp(p[1], 'x')) {
                base = 16, cmask = StrCharXDIGIT, p += 2;
            }

            for (;; p++) {
                if (*p == '0') {
                    hasdig = 1;
                } else if (*p == '.') {
                    if (dp) {
                        return STRSCAN_ERROR;
                    }

                    dp = p;
                } else {
                    break;
                }
            }
        }

        /* Preliminary digit and decimal point scan. */
        for (sp = p;; p++) {
            if (likely(StrCharIsa(*p, cmask))) {
                x = StrValTimes10(x) + (*p & 15); /* For fast path below. */
                dig++;
            } else if (*p == '.') {
                if (dp) {
                    return STRSCAN_ERROR;
                }

                dp = p;
            } else {
                break;
            }
        }

        if (!(hasdig | dig)) {
            return STRSCAN_ERROR;
        }

        /* Handle decimal point. */
        if (dp) {
            fmt = STRSCAN_NUM;
            if (dig) {
                ex = (int32_t)(dp - (p - 1));
                dp = p - 1;
                while (ex < 0 && *dp-- == '0') {
                    ex++;
                    dig--; /* Skip trailing zeros. */
                }

                if (base == 16) {
                    ex *= 4;
                }
            }
        }

        /* Parse exponent. */
        if (casecmp(*p, (uint32_t)(base == 16 ? 'p' : 'e'))) {
            uint32_t xx;
            int negx = 0;
            fmt = STRSCAN_NUM;
            p++;
            if (*p == '+' || *p == '-') {
                negx = (*p++ == '-');
            }

            if (!StrCharIsdigit(*p)) {
                return STRSCAN_ERROR;
            }

            xx = (*p++ & 15);
            while (StrCharIsdigit(*p)) {
                if (xx < 65536) {
                    xx = StrValTimes10(xx) + (*p & 15);
                }

                p++;
            }

            ex += negx ? -(int32_t)xx : (int32_t)xx;
        }

        /* Parse suffix. */
        if (*p) {
            /* I (IMAG), U (U32), LL (I64), ULL/LLU (U64), L (long), UL/LU
             * (ulong). */
            /* NYI: f (float). Not needed until cp_number() handles
             * non-integers. */
            if (casecmp(*p, 'i')) {
                if (!(opt & STRSCAN_OPT_IMAG)) {
                    return STRSCAN_ERROR;
                }

                p++;
                fmt = STRSCAN_IMAG;
            } else if (fmt == STRSCAN_INT) {
                if (casecmp(*p, 'u')) {
                    p++, fmt = STRSCAN_U32;
                }

                if (casecmp(*p, 'l')) {
                    p++;
                    if (casecmp(*p, 'l')) {
                        p++;
                        fmt += STRSCAN_I64 - STRSCAN_INT;
                    } else if (!(opt & STRSCAN_OPT_C)) {
                        return STRSCAN_ERROR;
                    } else if (sizeof(long) == 8) {
                        fmt += STRSCAN_I64 - STRSCAN_INT;
                    }
                }
                if (casecmp(*p, 'u') &&
                    (fmt == STRSCAN_INT || fmt == STRSCAN_I64)) {
                    p++;
                    fmt += STRSCAN_U32 - STRSCAN_INT;
                }

                if ((fmt == STRSCAN_U32 && !(opt & STRSCAN_OPT_C)) ||
                    (fmt >= STRSCAN_I64 && !(opt & STRSCAN_OPT_LL))) {
                    return STRSCAN_ERROR;
                }
            }

            if (skipSpaces) {
                while (StrCharIsspace(*p)) {
                    p++;
                }
            }

            if (*p) {
                return STRSCAN_ERROR;
            }
        }

        /* Fast path for decimal 32 bit integers. */
        if (fmt == STRSCAN_INT && base == 10 &&
            (dig < 10 || (dig == 10 && *sp <= '2' && x < 0x80000000u + neg))) {
            int32_t y = neg ? -(int32_t)x : (int32_t)x;
            if ((opt & STRSCAN_OPT_TONUM)) {
                DATABOX_SET_DOUBLE(box, (double)y);
                return STRSCAN_NUM;
            }

            DATABOX_SET_SIGNED(box, y);
            return STRSCAN_INT;
        }

        /* Dispatch to base-specific parser. */
        if (base == 0 && !(fmt == STRSCAN_NUM || fmt == STRSCAN_IMAG)) {
            return StrScanOctal(sp, box, fmt, neg, dig);
        }

        if (base == 16) {
            fmt = StrScanHex(sp, box, fmt, opt, ex, neg, dig);
        } else {
            fmt = StrScanDecimal(sp, box, fmt, opt, ex, neg, dig);
        }

        /* Try to convert number to integer, if requested. */
        if (fmt == STRSCAN_NUM && (opt & STRSCAN_OPT_TOINT)) {
            double n = box->data.d64;
            int32_t i = (int32_t)n;
            if (n == (double)i) {
                DATABOX_SET_SIGNED(box, i);
                return STRSCAN_INT;
            }
        }

        return fmt;
    }
}

#undef DNEXT
#undef DPREV
#undef DLEN
