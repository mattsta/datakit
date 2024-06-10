#include "../str.h"
#include <float.h> /* DBL_MANT_DIG */

/* ====================================================================
 * Global String Helper Tables (sqlite3)
 * ==================================================================== */
/* An array to map all upper-case characters into their corresponding
** lower-case character.
**
** SQLite only considers US-ASCII characters.  We do not
** handle case conversions for the UTF character set since the tables
** involved are nearly as big or bigger than SQLite itself.
*/
const uint8_t StrUpperToLower[] = {
    0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,
    15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,
    30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,
    45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,
    60,  61,  62,  63,  64,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106,
    107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121,
    122, 91,  92,  93,  94,  95,  96,  97,  98,  99,  100, 101, 102, 103, 104,
    105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119,
    120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134,
    135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149,
    150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164,
    165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 178, 179,
    180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194,
    195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209,
    210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224,
    225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239,
    240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254,
    255};

/*
** The following 256 byte lookup table is used to support SQLites built-in
** equivalents to the following standard library functions:
**
**   isspace()                        0x01
**   isalpha()                        0x02
**   isdigit()                        0x04
**   isalnum()                        0x06
**   isxdigit()                       0x08
**   toupper()                        0x20
**   SQLite identifier character      0x40
**
** Bit 0x20 is set if the mapped character requires translation to upper
** case. i.e. if the character is a lower-case ASCII character.
** If x is a lower-case ASCII character, then its upper-case equivalent
** is (x - 0x20). Therefore toupper() can be implemented as:
**
**   (x & ~(map[x]&0x20))
**
** Standard function tolower() is implemented using the StrUpperToLower[]
** array. tolower() is used more often than toupper() by SQLite.
**
** Bit 0x40 is set if the character non-alphanumeric and can be used in an
** SQLite identifier.  Identifiers are alphanumerics, "_", "$", and any
** non-ASCII UTF character. Hence the test for whether or not a character is
** part of an identifier is 0x46.
**
** SQLite's versions are identical to the standard versions assuming a
** locale of "C". They are implemented as macros in sqliteInt.h.
*/
const uint8_t StrCtypeMap[256] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 00..07    ........ */
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, /* 08..0f    ........ */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 10..17    ........ */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 18..1f    ........ */
    0x01, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, /* 20..27     !"#$%&' */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 28..2f    ()*+,-./ */
    0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, /* 30..37    01234567 */
    0x0c, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 38..3f    89:;<=>? */

    0x00, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x02, /* 40..47    @ABCDEFG */
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, /* 48..4f    HIJKLMNO */
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, /* 50..57    PQRSTUVW */
    0x02, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x40, /* 58..5f    XYZ[\]^_ */
    0x00, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x22, /* 60..67    `abcdefg */
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, /* 68..6f    hijklmno */
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, /* 70..77    pqrstuvw */
    0x22, 0x22, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, /* 78..7f    xyz{|}~. */

    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* 80..87    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* 88..8f    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* 90..97    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* 98..9f    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* a0..a7    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* a8..af    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* b0..b7    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* b8..bf    ........ */

    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* c0..c7    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* c8..cf    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* d0..d7    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* d8..df    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* e0..e7    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* e8..ef    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* f0..f7    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40  /* f8..ff    ........ */
};

/*
** If X is a character that can be used in an identifier then
** IdChar(X) will be true.  Otherwise it is false.
**
** For ASCII, any character with the high-order bit set is
** allowed in an identifier.  For 7-bit characters,
** StrIsIdChar[X] must be 1.
**
** Ticket #1066.  the SQL standard does not allow '$' in the
** middle of identfiers.  But many SQL implementations do.
** SQLite will allow '$' in identifiers for compatibility.
** But the feature is undocumented.
*/
const char StrIdChar[] = {
    /* x0 x1 x2 x3 x4 x5 x6 x7 x8 x9 xA xB xC xD xE xF */
    0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 2x */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, /* 3x */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 4x */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, /* 5x */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 6x */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, /* 7x */
};

/* ====================================================================
 * String Helpers (sqlite3)
 * ==================================================================== */
/*
** Convert an SQL-style quoted string into a normal string by removing
** the quote characters.  The conversion is done in-place.  If the
** input does not begin with a quote character, then this routine
** is a no-op.
**
** The input string must be zero-terminated.  A new zero-terminator
** is added to the dequoted string.
**
** The return value is -1 if no dequoting occurs or the length of the
** dequoted string, exclusive of the zero terminator, if dequoting does
** occur.
**
** 2002-Feb-14: This routine is extended to remove MS-Access style
** brackets from around identifiers.  For example:  "[a-b-c]" becomes
** "a-b-c".
*/
void StrDequote(char *z) {
    if (!z) {
        return;
    }

    char quote = z[0];

    if (!StrIsquote(quote)) {
        return;
    }

    if (quote == '[') {
        quote = ']';
    }

    size_t j = 0;
    for (size_t i = 1;; i++) {
        assert(z[i]);
        if (z[i] == quote) {
            if (z[i + 1] == quote) {
                z[j++] = quote;
                i++;
            } else {
                break;
            }
        } else {
            z[j++] = z[i];
        }
    }

    z[j] = 0;
}

/* Convenient short-hand */
#define UpperToLower StrUpperToLower

/*
** Some systems have stricmp().  Others have strcasecmp().  Because
** there is no consistency, we will define our own.
**
** IMPLEMENTATION-OF: R-30243-02494 The Str_stricmp() and
** Str_strnicmp() APIs allow applications and extensions to compare
** the contents of two buffers containing UTF-8 strings in a
** case-independent fashion, using the same definition of "case
** independence" that SQLite uses internally when comparing identifiers.
*/
int32_t StrICmp(const char *zLeft, const char *zRight) {
#if 0
    register uint8_t *a, *b;
    if (zLeft == 0) {
        return zRight ? -1 : 0;
    } else if (zRight == 0) {
        return 1;
    }

    a = (uint8_t *)zLeft;
    b = (uint8_t *)zRight;
    while (*a != 0 && UpperToLower[*a] == UpperToLower[*b]) {
        a++;
        b++;
    }

    return UpperToLower[*a] - UpperToLower[*b];
#else
    unsigned char *a, *b;
    int c, x;
    a = (unsigned char *)zLeft;
    b = (unsigned char *)zRight;
    for (;;) {
        c = (int)UpperToLower[*a] - (int)UpperToLower[*b];
        if (c || *a == 0) {
            break;
        }

        c = *a;
        x = *b;
        if (c == x) {
            if (c == 0) {
                break;
            }
        } else {
            c = (int)UpperToLower[c] - (int)UpperToLower[x];
            if (c) {
                break;
            }
        }
        a++;
        b++;
    }

    return c;
#endif
}

int32_t StrNIcmp(const char *zLeft, const char *zRight, int32_t N) {
    register uint8_t *a, *b;
    if (zLeft == 0) {
        return zRight ? -1 : 0;
    } else if (zRight == 0) {
        return 1;
    }

    a = (uint8_t *)zLeft;
    b = (uint8_t *)zRight;
    while (N-- > 0 && *a != 0 && UpperToLower[*a] == UpperToLower[*b]) {
        a++;
        b++;
    }

    return N < 0 ? 0 : UpperToLower[*a] - UpperToLower[*b];
}

/* ====================================================================
 * Convert text to double (sqlite3)
 * ==================================================================== */
/*
** Compute 10 to the E-th power.  Examples:  E==1 results in 10.
** E==2 results in 100.  E==50 results in 1.0e50.
**
** This routine only works for values of E between 1 and 341.
*/
static long double StrLDPow10(int E) {
    long double x = 10.0;
    long double r = 1.0;
    while (1) {
        if (E & 1) {
            r *= x;
        }

        E >>= 1;
        if (E == 0) {
            break;
        }

        x *= x;
    }

    return r;
}

/*
** The string z[] is an text representation of a real number.
** Convert this string to a double and write it into *pResult.
**
** The string z[] is length bytes in length (bytes, not characters) and
** uses the encoding enc.  The string is not necessarily zero-terminated.
**
** Return TRUE if the result is a valid real number (or integer) and FALSE
** if the string is empty or contains extraneous text.  Valid numbers
** are in one of these formats:
**
**    [+-]digits[E[+-]digits]
**    [+-]digits.[digits][E[+-]digits]
**    [+-].digits[E[+-]digits]
**
** Leading and trailing whitespace is ignored for the purpose of determining
** validity.
**
** If some prefix of the input string is a valid number, this routine
** returns FALSE but it still converts the prefix and writes the result
** into *pResult.
*/
bool StrAtoF(const void *z_, double *pResult, int32_t length, strEnc enc,
             bool skipSpaces) {
    const uint8_t *restrict z = z_;
    int32_t incr;
    int32_t sign = 1;   /* sign of significand */
    int64_t s = 0;      /* significand */
    int32_t d = 0;      /* adjust exponent for shifting decimal point32_t */
    int32_t esign = 1;  /* sign of exponent */
    int32_t e = 0;      /* exponent */
    int32_t eValid = 1; /* True exponent is either not used or is well-formed */
    int32_t nDigits = 0;
    int32_t nonNum = 0;
    const uint8_t *zEnd;
    zEnd = z + length; /* sign * significand * (10 ^ (esign * exponent)) */
    double result;

    /* We aren't using weird encoding types, so just globally set UTF8 here */
    enc = STR_UTF8;

    assert(enc == STR_UTF8 || enc == STR_UTF16LE || enc == STR_UTF16BE);
    *pResult = 0.0; /* Default return value, in case of an error */

    if (enc == STR_UTF8) {
        incr = 1;
    } else {
        int32_t i;
        assert(STR_UTF16LE == 2 && STR_UTF16BE == 3);
        for (i = 3 - enc; i < length && z[i] == 0; i += 2) {
        }

        nonNum = i < length;
        zEnd = z + i + enc - 3;
        z += (enc & 1);
    }

    if (skipSpaces) {
        /* skip leading spaces */
        while (z < zEnd && StrIsspace(*z)) {
            z += incr;
        }
    }

    if (z >= zEnd) {
        return false;
    }

    /* get sign of significand */
    if (*z == '-') {
        sign = -1;
        z += incr;
    } else if (*z == '+') {
        z += incr;
    }

    /* skip leading zeroes */
    while (z < zEnd && z[0] == '0') {
        z += incr, nDigits++;
    }

    /* copy max significant digits to significand */
    while (z < zEnd && StrIsdigit(*z) && s < ((INT64_MAX - 9) / 10)) {
        s = StrValTimes10(s) + (*z - '0');
        z += incr;
        nDigits++;
    }

    /* skip non-significant significand digits
    ** (increase exponent by d to shift decimal left) */
    while (z < zEnd && StrIsdigit(*z)) {
        z += incr, nDigits++, d++;
    }

    if (z >= zEnd) {
        goto do_atof_calc;
    }

    /* if decimal point is present */
    if (*z == '.') {
        z += incr;
        /* copy digits from after decimal to significand
        ** (decrease exponent by d to shift decimal right) */
        while (z < zEnd && StrIsdigit(*z) && s < ((INT64_MAX - 9) / 10)) {
            s = StrValTimes10(s) + (*z - '0');
            z += incr;
            nDigits++;
            d--;
        }

        /* skip non-significant digits */
        while (z < zEnd && StrIsdigit(*z)) {
            z += incr, nDigits++;
        }
    }
    if (z >= zEnd) {
        goto do_atof_calc;
    }

    /* if exponent is present */
    if (*z == 'e' || *z == 'E') {
        z += incr;
        eValid = 0;
        if (z >= zEnd) {
            goto do_atof_calc;
        }

        /* get sign of exponent */
        if (*z == '-') {
            esign = -1;
            z += incr;
        } else if (*z == '+') {
            z += incr;
        }

        /* copy digits to exponent */
        while (z < zEnd && StrIsdigit(*z)) {
            e = e < 10000 ? (StrValTimes10(e) + (*z - '0')) : 10000;
            z += incr;
            eValid = 1;
        }
    }

    if (skipSpaces) {
        /* skip trailing spaces */
        if (nDigits && eValid) {
            while (z < zEnd && StrIsspace(*z)) {
                z += incr;
            }
        }
    }

do_atof_calc:
    /* adjust exponent by d, and update sign */
    e = (e * esign) + d;
    if (e < 0) {
        esign = -1;
        e *= -1;
    } else {
        esign = 1;
    }

    /* if 0 significand */
    if (!s) {
        /* In the IEEE 754 standard, zero is signed.
        ** Add the sign if we've seen at least one digit */
        result = (sign < 0 && nDigits) ? -(double)0 : (double)0;
    } else {
        /* attempt to reduce exponent */
        if (esign > 0) {
            for (; s < (INT64_MAX / 10) && e > 0; e--) {
                s = StrValTimes10(s);
            }
        } else {
            _Pragma("GCC diagnostic push") _Pragma(
                "GCC diagnostic ignored \"-Wstrict-overflow\"") for (; !(s %
                                                                         10) &&
                                                                       e > 0;
                                                                     e--) {
                s /= 10;
            }
            _Pragma("GCC diagnostic pop")
        }

        /* adjust the sign of significand */
        s = sign < 0 ? -s : s;

        /* if exponent, scale significand as appropriate
        ** and store in result. */
        if (e) {
            /* attempt to handle extremely small/large numbers better */
            if (e > 307 && e < 342) {
                const long double scale = StrLDPow10(e - 308);

                if (esign < 0) {
                    result = s / scale;
                    result /= 1.0e+308;
                } else {
                    result = s * scale;
                    result *= 1.0e+308;
                }
            } else if (e >= 342) {
                if (esign < 0) {
                    result = 0.0 * s;
                } else {
                    result = 1e308 * 1e308 * s; /* Infinity */
                }
            } else {
                const long double scale = StrLDPow10(e);
                if (esign < 0) {
                    result = s / scale;
                } else {
                    result = s * scale;
                }
            }
        } else {
            result = (double)s;
        }
    }

    /* store the result */
    *pResult = result;

    /* return true if number and no extra non-whitespace chracters after */
    return z >= zEnd && nDigits > 0 && eValid && nonNum == 0;
}

/*
** Compute 10 to the E-th power.  Examples:  E==1 results in 10.
** E==2 results in 100.  E==50 results in 1.0e50.
**
** This routine only works for values of E between 1 and 341.
*/
static long double strPow10(int32_t E) {
    long double x = 10.0;
    long double r = 1.0;
    while (true) {
        if (E & 1) {
            r *= x;
        }

        E >>= 1;
        if (E == 0) {
            break;
        }

        x *= x;
    }

    return r;
}

DK_INLINE_ALWAYS bool StrAtoFReliable(int64_t s, const int32_t sign,
                                      const void *z_, double *pResult,
                                      int32_t length) {
    const uint8_t *restrict z = z_;
    const int32_t incr = 1;
    int32_t d = 0;     /* adjust exponent for shifting decimal point32_t */
    int32_t esign = 1; /* sign of exponent */
    int32_t e = 0;     /* exponent */
    int32_t nDigits = 0;
    const uint8_t *zEnd;
    zEnd = z + length; /* sign * significand * (10 ^ (esign * exponent)) */
    double result;

    /* if decimal point is present */
    if (*z == '.') {
        z += incr;

        /* If decimal digits are greater than exact decimal precision of double,
         * return failure. */
        /* minus one to jump over '.' */
        if (length - 1 > DBL_DIG) {
            return false;
        }

        /* copy digits from after decimal to significand
        ** (decrease exponent by d to shift decimal right) */
        while (z < zEnd && StrIsdigit(*z) && s < ((INT64_MAX - 9) / 10)) {
            s = StrValTimes10(s) + (*z - '0');
            z += incr;
            nDigits++;
            d--;
        }

        /* skip non-significant digits */
        while (z < zEnd && StrIsdigit(*z)) {
            z += incr;
            nDigits++;
        }
    }

    /* adjust exponent by d, and update sign */
    e = (e * esign) + d;
    if (e < 0) {
        esign = -1;
        e *= -1;
    } else {
        esign = 1;
    }

    /* if 0 significand */
    if (s == 0) {
        /* In the IEEE 754 standard, zero is signed.
        ** Add the sign if we've seen at least one digit */
        result = (sign < 0 && nDigits) ? -(double)0 : (double)0;
    } else {
        /* attempt to reduce exponent */
        while (e > 0) { /*OPTIMIZATION-IF-TRUE*/
            if (esign > 0) {
                if (s >= (INT64_MAX / 10)) {
                    break;
                }
                s *= 10;
            } else {
                if (s % 10 != 0) {
                    break;
                }
                s /= 10;
            }
            e--;
        }

        /* adjust the sign of significand */
        s = sign < 0 ? -s : s;

        /* if exponent, scale significand as appropriate
        ** and store in result. */
        if (e == 0) {
            result = (double)s;
        } else {
#if 0 /* we don't need these for ScanReliable since exponents can't grow big   \
       */
            /* attempt to handle extremely small/large numbers better */
            if (e > 307 && e < 342) {
                const long double scale = strPow10(e - 308);

                if (esign < 0) {
                    result = s / scale;
                    result /= 1.0e+308;
                } else {
                    result = s * scale;
                    result *= 1.0e+308;
                }
            } else if (e >= 342) {
                if (esign < 0) {
                    result = 0.0 * s;
                } else {
                    result = 1e308 * 1e308 * s; /* Infinity */
                }
            } else {
#endif
            /* This could be slightly cleaner if our compiler
             * supported __float128, but clang is a little
             * behind with releases at the moment. */
            const long double scale = strPow10(e);
            if (esign < 0) {
                result = s / scale;
            } else {
                result = s * scale;
            }
        }
    }

    /* store the result */
    *pResult = result;

    /* return true if number and no extra non-whitespace chracters after */
    return z >= zEnd && nDigits > 0;
}

/* ====================================================================
 * Convert text to int64_t (sqlite3)
 * ==================================================================== */
/*
** Compare the 19-character string zNum against the text representation
** value 2^63:  9223372036854775808.  Return negative, zero, or positive
** if zNum is less than, equal to, or greater than the string.
** Note that zNum must contain exactly 19 characters.
**
** Unlike memcmp() this routine is guaranteed to return the difference
** in the values of the last digit if the only difference is in the
** last digit.  So, for example,
**
**      compare2pow63("9223372036854775800", 1)
**
** will return -8.
*/
DK_STATIC int32_t compare2pow63(const uint8_t *zNum, int32_t incr) {
    int32_t c = 0;
    /* 012345678901234567 */
    const char *pow63 = "922337203685477580";
    for (int32_t i = 0; c == 0 && i < 18; i++) {
        c = (zNum[i * incr] - pow63[i]);
        c = StrValTimes10(c);
    }

    if (c == 0) {
        c = zNum[18 * incr] - '8';
    }

    return c;
}

/*
** Convert zNum to a 64-bit signed integer.  zNum must be decimal. This
** routine does *not* accept hexadecimal notation.
**
** If the zNum value is representable as a 64-bit twos-complement
** integer, then write that value into *pNum and return 0.
**
** If zNum is exactly 9223372036854775808, return 2.  This special
** case is broken out because while 9223372036854775808 cannot be a
** signed 64-bit integer, its negative -9223372036854775808 can be.
**
** If zNum is too big for a 64-bit integer and is not
** 9223372036854775808  or if zNum contains any non-numeric text,
** then return 1.
**
** length is the number of bytes in the string (bytes, not characters).
** The string is not necessarily zero-terminated.  The encoding is
** given by enc.
*/
int32_t StrAtoi64(const void *_zNum, int64_t *pNum, int32_t length, uint8_t enc,
                  bool skipSpaces) {
    const uint8_t *zNum = _zNum;
    int32_t incr;
    uint64_t u = 0;
    bool neg = false; /* assume positive */
    int32_t i;
    int32_t c = 0;
    int32_t nonNum = 0;
    const uint8_t *zStart;
    const uint8_t *zEnd = zNum + length;
    assert(enc == STR_UTF8 || enc == STR_UTF16LE || enc == STR_UTF16BE);
    if (enc == STR_UTF8) {
        incr = 1;
    } else {
        assert(STR_UTF16LE == 2 && STR_UTF16BE == 3);
        for (i = 3 - enc; i < length && zNum[i] == 0; i += 2) {
        }

        incr = 2;
        nonNum = i < length;
        zEnd = zNum + i + enc - 3;
        zNum += (enc & 1);
    }

    if (skipSpaces) {
        while (zNum < zEnd && StrIsspace(*zNum)) {
            zNum += incr;
        }
    }

    if (zNum < zEnd) {
        if (*zNum == '-') {
            neg = true;
            zNum += incr;
        } else if (*zNum == '+') {
            zNum += incr;
        }
    }
    zStart = zNum;
    while (zNum < zEnd && zNum[0] == '0') {
        zNum += incr;
    } /* Skip leading zeros. */
    for (i = 0; &zNum[i] < zEnd && (c = zNum[i]) >= '0' && c <= '9';
         i += incr) {
        u = StrValTimes10(u) + c - '0';
    }

    if (u > INT64_MAX) {
        *pNum = neg ? INT64_MIN : INT64_MAX;
    } else if (neg) {
        *pNum = -(int64_t)u;
    } else {
        *pNum = (int64_t)u;
    }

    if ((c != 0 && &zNum[i] < zEnd) || (i == 0 && zStart == zNum) ||
        i > 19 * incr || nonNum) {
        /* zNum is empty or contains non-numeric text or is longer
        ** than 19 digits (thus guaranteeing that it is too large) */
        return 1;
    } else if (i < 19 * incr) {
        /* Less than 19 digits, so we know that it fits in 64 bits */
        assert(u <= INT64_MAX);
        return 0;
    } else {
        /* zNum is a 19-digit numbers.  Compare it against 9223372036854775808.
         */
        c = compare2pow63(zNum, incr);
        if (c < 0) {
            /* zNum is less than 9223372036854775808 so it fits */
            assert(u <= INT64_MAX);
            return 0;
        }

        if (c > 0) {
            /* zNum is greater than 9223372036854775808 so it overflows */
            return 1;
        }

        /* zNum is exactly 9223372036854775808.  Fits if negative.  The
        ** special case 2 overflow if positive */
        assert(u - 1 == INT64_MAX);
        return neg ? 0 : 2;
    }
}

/* ====================================================================
 * Hex to Integer Conversion Helpers (sqlite3)
 * ==================================================================== */
/*
** Translate a single byte of Hex into an integer.
** This routine only works if h really is a valid hexadecimal
** character:  0..9a..fA..F
*/
uint8_t StrHexToInt(int32_t h) {
    assert((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') ||
           (h >= 'A' && h <= 'F'));
    h += 9 * (1 & (h >> 6));
    return (uint8_t)(h & 0xf);
}

/*
** Transform a UTF-8 integer literal, in either decimal or hexadecimal,
** into a 64-bit signed integer.  This routine accepts hexadecimal literals,
** whereas StrAtoi64() does not.
**
** Returns:
**
**     0    Successful transformation.  Fits in a 64-bit signed integer.
**     1    Integer too large for a 64-bit signed integer or is malformed
**     2    Special case of 9223372036854775808
*/
int32_t StrDecOrHexToInt64(const char *z, int64_t *pOut) {
    if (z[0] == '0' && (z[1] == 'x' || z[1] == 'X') && StrIsxdigit(z[2])) {
        uint64_t u = 0;
        int32_t i, k;
        for (i = 2; z[i] == '0'; i++) {
        }

        for (k = i; StrIsxdigit(z[k]); k++) {
            u = u * 16 + StrHexToInt(z[k]);
        }

        memcpy(pOut, &u, 8);
        return (z[k] == 0 && k - i <= 16) ? 0 : 1;
    }

    return StrAtoi64(z, pOut, strlen(z), STR_UTF8, true);
}

/* ====================================================================
 * Convert text to int32_t (sqlite3)
 * ==================================================================== */
/*
** If zNum represents an integer that will fit in 32-bits, then set
** *pValue to that integer and return true.  Otherwise return false.
**
** This routine accepts both decimal and hexadecimal notation for integers.
**
** Any non-numeric characters that following zNum are ignored.
** This is different from StrAtoi64() which requires the
** input number to be zero-terminated.
*/
bool StrGetInt32(const char *zNum, int32_t *pValue) {
    int64_t v = 0;
    int32_t i, c;
    bool neg = false;
    if (zNum[0] == '-') {
        neg = true;
        zNum++;
    } else if (zNum[0] == '+') {
        zNum++;
    } else if (zNum[0] == '0' && (zNum[1] == 'x' || zNum[1] == 'X') &&
               StrIsxdigit(zNum[2])) {
        uint32_t u = 0;
        zNum += 2;
        while (zNum[0] == '0') {
            zNum++;
        }

        for (i = 0; StrIsxdigit(zNum[i]) && i < 8; i++) {
            u = u * 16 + StrHexToInt(zNum[i]);
        }

        if ((u & 0x80000000) == 0 && StrIsxdigit(zNum[i]) == 0) {
            memcpy(pValue, &u, 4);
            return true;
        } else {
            return false;
        }
    }
    while (zNum[0] == '0') {
        zNum++;
    }

    for (i = 0; i < 11 && (c = zNum[i] - '0') >= 0 && c <= 9; i++) {
        v = StrValTimes10(v) + c;
    }

    /* The longest decimal representation of a 32 bit integer is 10 digits:
    **
    **             1234567890
    **     2^31 -> 2147483648
    */
    if (i > 10) {
        return false;
    }

    if (v - neg > 2147483647) {
        return false;
    }

    if (neg) {
        v = -v;
    }

    *pValue = (int32_t)v;
    return true;
}

/*
** Return a 32-bit integer value extracted from a string.  If the
** string is not an integer, just return 0.
*/
int32_t StrAtoi(const char *z) {
    int32_t x = 0;
    if (z) {
        StrGetInt32(z, &x);
    }

    return x;
}

/*
** Convert a BLOB literal of the form "x'hhhhhh'" into its binary
** value.  Return a pointer to its binary value.  Space to hold the
** binary value has been obtained from malloc and must be freed by
** the calling routine.
*/
void *StrHexToBlob(const char *z, int32_t n) {
    char *zBlob = (char *)zmalloc(n / 2 + 1);
    n--;
    if (zBlob) {
        int32_t i;
        for (i = 0; i < n; i += 2) {
            zBlob[i / 2] = (StrHexToInt(z[i]) << 4) | StrHexToInt(z[i + 1]);
        }

        zBlob[i / 2] = 0;
    }

    return zBlob;
}

/* ====================================================================
 * Math Overflow Detection Helpers (sqlite3)
 * ==================================================================== */
/*
** Attempt to add, substract, or multiply the 64-bit signed value iB against
** the other 64-bit signed integer at *pA and store the result in *pA.
** Return true on success.  Or if the operation would have resulted in an
** overflow, leave *pA unchanged and return false.
*/
bool StrAddInt64(int64_t *pA, int64_t iB) {
    int64_t iA = *pA;
    if (iB >= 0) {
        if (iA > 0 && INT64_MAX - iA < iB) {
            return false;
        }
    } else {
        if (iA < 0 && -(iA + INT64_MAX) > iB + 1) {
            return false;
        }
    }

    *pA += iB;
    return true;
}

bool StrSubInt64(int64_t *pA, int64_t iB) {
    if (iB == INT64_MIN) {
        if ((*pA) >= 0) {
            return false;
        }

        *pA -= iB;
        return true;
    }

    return StrAddInt64(pA, -iB);
}

#define TWOPOWER32 (((int64_t)1) << 32)
#define TWOPOWER31 (((int64_t)1) << 31)
int32_t StrMulInt64(int64_t *pA, int64_t iB) {
    int64_t iA = *pA;
    int64_t iA1, iA0, iB1, iB0, r;

    iA1 = iA / TWOPOWER32;
    iA0 = iA % TWOPOWER32;
    iB1 = iB / TWOPOWER32;
    iB0 = iB % TWOPOWER32;
    if (iA1 == 0) {
        if (iB1 == 0) {
            *pA *= iB;
            return 0;
        }

        r = iA0 * iB1;
    } else if (iB1 == 0) {
        r = iA1 * iB0;
    } else {
        /* If both iA1 and iB1 are non-zero, overflow will result */
        return 1;
    }

    if (r < (-TWOPOWER31) || r >= TWOPOWER31) {
        return 1;
    }

    r *= TWOPOWER32;
    if (StrAddInt64(&r, iA0 * iB0)) {
        return 1;
    }

    *pA = r;
    return 0;
}

/*
** Compute the absolute value of a 32-bit signed integer, if possible.
** Or, if the integer has a value of -2147483648, return +2147483647
*/
int32_t StrAbsInt32(int32_t x) {
    if (x >= 0) {
        return x;
    }

    if (x == (int32_t)0x80000000) {
        return 0x7fffffff;
    }

    return -x;
}
