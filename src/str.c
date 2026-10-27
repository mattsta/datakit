#include "str.h"

/* str.{c,h} contain utilities extracted from:
 *   - sqlite3 (public domain)
 *   - luajit (mit license)
 *   - other misc open licensed projects */

#include "datakit.h"
#include "floatExtended.h"

#include "str/strBitmapGetSetPositionsExact.c"
#include "str/strCountDigits.c"
#include "str/strDigitsVerify.c"
#include "str/strLuajitStr.c"
#include "str/strPopcnt.c"
#include "str/strPow.c"
#include "str/strRandom.c"
#include "str/strSqliteLog.c"
#include "str/strSqliteNumeric.c"
#include "str/strSqliteStr.c"
#include "str/strToBufFast.c"
#include "str/strToBufSplat.c"
#include "str/strToBufTable.c"
#include "str/strToNative.c"
#include "str/strUTF8.c"
#include "str/strUTF8Case.c"
#include "str/strUTF8Compare.c"
#include "str/strUTF8Cursor.c"
#include "str/strUTF8Grapheme.c"
#include "str/strUTF8Search.c"
#include "str/strUTF8Substr.c"
#include "str/strUTF8Valid.c"
#include "str/strUTF8Width.c"
#include "str/strUnicodeData.c"

#include "strDoubleFormat.h"
/* StrScanScanReliable reads a byte buffer and attempts to convert the bytes
 * inside to a native type exactly representing the input bytes.
 *
 * Goal: allow efficient storage of user bytes by converting to native types
 * when possible (integers, reals), while ALSO allowing 100% reliable round-trip
 * printing of values created. Re-converting the native type to a string agan
 * must be EXACTLY the same value the user provided to generate the native
 * representation.
 *
 * Resulting 'box' may end up containing the converted value of 'p'
 * as exactly one of:
 *   - signed 64 bit integers (INT64_MIN to INT64_MAX)
 *   - unsigned 64 bit integers (INT64_MAX + 1 to UINT64_MAX), only for
 *     positives above the signed range
 *   - float
 *   - double
 *
 * Returns 'true' on successful conversion.
 * Returns 'false' if we can't represent the buffer as a primitive type.
 *
 * If 'false' is returned, 'box' remains untouched. */
bool StrScanScanReliable(const void *const p_, const size_t len,
                         databox *restrict box) {
    const uint8_t *restrict p = p_;
    int_fast32_t sign = 1; /* positive = 1, negative = -1 */

    uint32_t dig = 0; /* byte index into 'p', the sign included */

    /* ckd_mul and ckd_add below see every operand as uint64_t: their
     * constants are spelled UINT64_C so all types match. */
    uint64_t x = 0; /* significand */
    /* Largest magnitude a SIGNED_64 holds for the sign: INT64_MAX, or 2^63
     * after a leading '-'. */
    uint64_t maxAbsoluteValueX = INT64_MAX;

    /* Don't allow leading zeroes (unless it's 0.xxxx)
     * Don't allow leading dot (we don't consider .1234 a valid parse) */
    if (!len || (len >= 2 && ((p[0] == '0' && p[1] != '.') || (p[0] == '.')))) {
        return false;
    }

    /* TODO: we could also early bail out here if the buffer is clearly too big
     *       to be a number (20 digits is max [U]INT64, plus doubles have max
     *       of 15 digits lossless precision, so the largest size here is,
     *       generously (20 + 1 + 15) */

    /* TODO:
     *  Compare current implementation against up-front early termination:
     *      - memchr of '.' in string with below
     *  - vs -
     *      - checking if all digits using StrIsDigitsFast() then converting
     *        to {signed/unsigned}{64,128} directly. */

    /* NOTE: This function converts _reasonable_ floats to native types.
     *       If you provide DBL_MAX as a string (300+ digits), the parse
     *       will fail because we bail out with parsing when the initial digits
     *       overflow.
     *
     *       Also, we re-use the intial integer-part components re-constructed
     *       in each loop as input to the float creator, so we can't just
     *       jump into the float creator if we detect a decimal. We have to
     *       pre-parse and convert the non-fractional portion too. */

    for (; dig < len; dig++, p++) {
        /* This simple cast of *p to (word-size)*p helps our compiler
         * perform better organization of math assembly below. */
        const uint64_t pDeref = *p;

        if (StrCharIsa(pDeref, StrCharDIGIT)) {
            /* This is just: x = (x * 10) + (*p & 0x0f) */
            /* Note: DO NOT remove the UINT64_C type specifiers below
             *       or else this function goes from 500 cycles to 12,000 cycles
             *       per run: every operand must have the same type. */
            if (ckd_mul(&x, x, UINT64_C(10)) ||
                ckd_add(&x, x, pDeref & UINT64_C(0x0F))) {
                /* ckd_* return true when the result overflowed: the
                 * number does not fit 64 bits */
                return false;
            }
        } else if (pDeref == '.' && (x < (1ULL << DBL_MANT_DIG))) {
            /* 2^53 is the highest real value we can reasonably
             * convert with no loss */

            /* If decimal AND last number is ZERO, then fail because
             * we can't reliably reproduce client input with zero
             * on the end (e.g. 255.900000 would get converted to 255.9) */
            if (((uint8_t *)p_)[len - 1] == '0') {
                if (((uint8_t *)p_)[len - 2] != '.') {
                    /* Allow '213.0' but not '123.10' due to inability
                     * to guarantee trailing zero will be reproduced
                     * on output. */
                    return false;
                }
            }

            double potentialResult;
            if (StrAtoFReliable(x, sign, p, &potentialResult, len - dig)) {
                /* Extra assertive check: verify forward and reverse string
                 * conversion matches exactly! */
                /* We could potentially remove this double check once we get on
                 * board with __float128 in a supported compiler
                 * Or, potentially use decimal64 / decimal128 encodings too */
                uint8_t buf[64];
                const size_t convertedLength =
                    StrDoubleFormatToBufNice(buf, sizeof(buf), potentialResult);

#if 0
                printf("Converted: %d, %.*s vs original: %d, %s\n",
                       convertedLength, convertedLength, buf, len, p_);
#endif
                /* If generated length doesn't equal input length, fail */
                if (convertedLength != len) {
                    return false;
                }

                /* If generated value doesn't equal input value, also fail */
                if (memcmp(buf, p_, len)) {
                    return false;
                }

                if ((double)(float)potentialResult == potentialResult) {
                    box->data.f32 = potentialResult;
                    box->type = DATABOX_FLOAT_32;
                } else {
                    box->data.d64 = potentialResult;
                    box->type = DATABOX_DOUBLE_64;
                }

                return true;
            }

            return false;
        } else if (*p == '-') {
            /* Only accept negation if it's the first character *AND*
             * if more characters exist. (i.e. '-' is not standalone number)
             * No leading zero after the sign either: "-0" and "-01" would
             * render back as "0" and "-1".  "-0." may still begin a float,
             * whose round trip is checked below. */
            if (dig == 0 && len > 1 &&
                !(p[1] == '0' && (len == 2 || p[2] != '.'))) {
                sign = -1;
                maxAbsoluteValueX = DK_INT64_TO_UINT64(INT64_MIN);
            } else {
                /* either dash in the number not at start or 'just a dash' */
                return false;
            }
        } else {
            /* encountered non-numeric when parsing number! */
            return false;
        }
    }

    /* 'x' is the exact magnitude (accumulation is overflow-checked), so
     * classify by value: SIGNED_64 if it fits the sign's bound (INT64_MIN
     * included), UNSIGNED_64 for positives above INT64_MAX, otherwise no
     * 64-bit integer form. */
    if (x <= maxAbsoluteValueX) {
        /* 'x' is unsigned, so 'x * sign' is the two's-complement negation
         * for a negative sign, INT64_MIN's magnitude 2^63 included. */
        const int64_t y = x * sign;
        box->data.i = y;
        box->type = DATABOX_SIGNED_64;
        return true;
    }

    if (sign > 0) {
        box->data.u = x;
        box->type = DATABOX_UNSIGNED_64;
        return true;
    }

    return false;
}

/* This is a weird interface because our common use case is:
 *   - we have an inbound databox from a user,
 *   - but we may want to generate a 128 bit integer from user input,
 *   - but we don't want to always pass around databoxBig *and* we don't
 *     want to copy the 'databox' content to a 'databoxBig' every time we
 *     run a conversion.
 *   SOLUTION:
 *   - pass in the original 'databox' and a 'databoxBig',
 *   - this conversion function populates the 'databoxBig' ONLY if it is
 * required
 *   - and whichever databox is populated gets assigned to '*use' */
/* This is an abstract implementation because sometimes we know we ALREADY have
 * 100% digits in the buffer so we can skip running StrIsDigitsFast() again.
 *
 * On success exactly one of 'small' or 'big' is written, as a fully defined
 * box (no stale allocated, created, big, unused or len bits from the
 * caller's storage), and *use points at it.  On failure nothing is
 * written. */
DK_INLINE_ALWAYS bool StrScanScanReliableConvert128_(
    const void *const p_, const size_t len, databox *restrict small,
    databoxBig *restrict big, databoxBig **use, const bool requireDigitCheck) {
    const uint8_t *buf = (const uint8_t *)p_;
    const bool negative = len > 0 && buf[0] == '-';
    const size_t signLen = negative ? 1 : 0;

    /* A 20 to 40 byte buffer may be an integer beyond 64 bits.  Only a
     * canonical integer (digits after an optional '-', no leading zero)
     * takes the 128-bit path; anything else of that length, such as a
     * real, gets the 64-bit scanner's verdict like any shorter buffer. */
    const bool wideInteger =
        len >= 20 && len <= 40 && buf[signLen] != '0' &&
        (!requireDigitCheck || StrIsDigitsFast(buf + signLen, len - signLen));

    if (!wideInteger) {
        databox narrow = {};
        if (!StrScanScanReliable(p_, len, &narrow)) {
            return false;
        }

        *small = narrow;
        *use = (databoxBig *)small;
        return true;
    }

    if (negative) {
        /* Its form is verified above, so skip the public parser's check */
        __int128_t result;
        if (!strBufToInt128Digits_(buf, len, &result)) {
            return false;
        }

        /* A negative value from -1 down to INT64_MIN is a SIGNED_64, as
         * StrScanScanReliable makes it; anything smaller needs 128 bits */
        if (result >= INT64_MIN) {
            *small = (databox){.data.i64 = (int64_t)result,
                               .type = DATABOX_SIGNED_64};
            *use = (databoxBig *)small;
            return true;
        }

        DATABOX_BIG_SIGNED_128(big, result);
    } else {
        __uint128_t result;
        if (!strBufToUInt128Digits_(buf, len, &result)) {
            return false;
        }

        /* 20 digits is above INT64_MAX, so a value that fits 64 bits is an
         * UNSIGNED_64, again as StrScanScanReliable makes it */
        if (result <= UINT64_MAX) {
            *small = (databox){.data.u64 = (uint64_t)result,
                               .type = DATABOX_UNSIGNED_64};
            *use = (databoxBig *)small;
            return true;
        }

        DATABOX_BIG_UNSIGNED_128(big, result);
    }

    /* The macro set the type, the payload pointer and 'big'; clear the
     * rest of the metadata the caller's storage may hold */
    big->allocated = false;
    big->created = false;
    big->unused = 0;
    big->len = 0;
    *use = big;
    return true;
}

bool StrScanScanReliableConvert128(const void *const p_, const size_t len,
                                   databox *restrict small,
                                   databoxBig *restrict big, databoxBig **use) {
    return StrScanScanReliableConvert128_(p_, len, small, big, use, true);
}

bool StrScanScanReliableConvert128PreVerified(const void *const p_,
                                              const size_t len,
                                              databox *restrict small,
                                              databoxBig *restrict big,
                                              databoxBig **use) {
    return StrScanScanReliableConvert128_(p_, len, small, big, use, false);
}

bool StrScanToDouble(const void *str, databox *box, bool allowFloatWords,
                     bool skipSpaces) {
    StrScanFmt fmt = StrScanScan((const uint8_t *)str, box, STRSCAN_OPT_TONUM,
                                 allowFloatWords, skipSpaces);
    assert(fmt == STRSCAN_ERROR || fmt == STRSCAN_NUM);
    return (fmt != STRSCAN_ERROR);
}

/* ====================================================================
 * Tests
 * ==================================================================== */
#ifdef DATAKIT_TEST
#include "cleaner.h"
#include "ctest.h"

#include <errno.h> /* the strtoull speed test checks errno */

#define DOUBLE_NEWLINE 0
#include "perf.h"

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleanerName(char *, zfree, Chars);
defcleanerName(uint8_t *, zfree, U8);

#define REPORT_TIME 1
#if REPORT_TIME
#define TIME_INIT PERF_TIMERS_SETUP
#define TIME_FINISH(i, what) PERF_TIMERS_FINISH_PRINT_RESULTS(i, what)
#else
#define TIME_INIT
#define TIME_FINISH(i, what)
#endif

DK_FN_NO_OPTIMIZE static void testStrBufToInt64(void) {
    char buf[32];
    int64_t v;

    /* May not start with +. */
    strcpy(buf, "+1");
    assert(StrBufToInt64(buf, strlen(buf), &v) == false);

    /* Leading space. */
    strcpy(buf, " 1");
    assert(StrBufToInt64(buf, strlen(buf), &v) == false);

    /* Trailing space. */
    strcpy(buf, "1 ");
    assert(StrBufToInt64(buf, strlen(buf), &v) == false);

    strcpy(buf, "01");
    assert(StrBufToInt64(buf, strlen(buf), &v) == false);

    strcpy(buf, "-1");
    assert(StrBufToInt64(buf, strlen(buf), &v) == true);
    assert(v == -1);

    strcpy(buf, "0");
    assert(StrBufToInt64(buf, strlen(buf), &v) == true);
    assert(v == 0);

    strcpy(buf, "1");
    assert(StrBufToInt64(buf, strlen(buf), &v) == true);
    assert(v == 1);

    strcpy(buf, "99");
    assert(StrBufToInt64(buf, strlen(buf), &v) == true);
    assert(v == 99);

    strcpy(buf, "-99");
    assert(StrBufToInt64(buf, strlen(buf), &v) == true);
    assert(v == -99);

    strcpy(buf, "-9223372036854775808");
    assert(StrBufToInt64(buf, strlen(buf), &v) == true);
    assert(v == INT64_MIN);

    strcpy(buf, "-9223372036854775809"); /* overflow */
    assert(StrBufToInt64(buf, strlen(buf), &v) == false);

    strcpy(buf, "9223372036854775807");
    TIME_INIT;

    const size_t loopers = 1ULL << 22;
    for (size_t i = 0; i < loopers; i++) {
        assert(StrBufToInt64(buf, strlen(buf), &v) == true);
    }

    TIME_FINISH(loopers, "StrBufToInt64");
    assert(v == INT64_MAX);

    strcpy(buf, "9223372036854775808"); /* overflow */
    assert(StrBufToInt64(buf, strlen(buf), &v) == false);

    /* The negative edge, exact on both sides, and malformed signs */
    strcpy(buf, "-9223372036854775807");
    assert(StrBufToInt64(buf, strlen(buf), &v) == true);
    assert(v == -INT64_MAX);

    static const char *const refused[] = {"-9223372036854775810",
                                          "-18446744073709551615",
                                          "-18446744073709551616",
                                          "-99999999999999999999",
                                          "-0",
                                          "-",
                                          "--1",
                                          "-01",
                                          "+1",
                                          "1-"};
    for (size_t i = 0; i < countof(refused); i++) {
        v = 12345;
        if (StrBufToInt64(refused[i], strlen(refused[i]), &v) || v != 12345) {
            printf("StrBufToInt64 accepted '%s'\n", refused[i]);
            assert(false);
        }
    }
}

DK_FN_NO_OPTIMIZE static void testStrBufToUInt64(void) {
    char buf[32];
    uint64_t v;

    TIME_INIT;

    /* May not start with +. */
    strcpy(buf, "+1");
    assert(StrBufToUInt64(buf, strlen(buf), &v) == false);

    /* Leading space. */
    strcpy(buf, " 1");
    assert(StrBufToUInt64(buf, strlen(buf), &v) == false);

    /* Trailing space. */
    strcpy(buf, "1 ");
    assert(StrBufToUInt64(buf, strlen(buf), &v) == false);

    strcpy(buf, "01");
    assert(StrBufToUInt64(buf, strlen(buf), &v) == false);

    strcpy(buf, "0");
    assert(StrBufToUInt64(buf, strlen(buf), &v) == true);
    assert(v == 0);

    strcpy(buf, "1");
    assert(StrBufToUInt64(buf, strlen(buf), &v) == true);
    assert(v == 1);

    strcpy(buf, "99");
    assert(StrBufToUInt64(buf, strlen(buf), &v) == true);
    assert(v == 99);

    strcpy(buf, "1129223372036854775809"); /* overflow */
    assert(StrBufToUInt64(buf, strlen(buf), &v) == false);

    strcpy(buf, "9223372036854775807");
    TIME_INIT;

    const size_t loopers = 1ULL << 22;
    for (size_t i = 0; i < loopers; i++) {
        assert(StrBufToUInt64(buf, strlen(buf), &v) == true);
    }

    TIME_FINISH(loopers, "StrBufToInt64");

    assert(v == INT64_MAX);

    strcpy(buf, "9223372036854775808");
    assert(StrBufToUInt64(buf, strlen(buf), &v) == true);
    assert(v == INT64_MAX + 1ULL);

    strcpy(buf, "18446744073709551615");
    assert(StrBufToUInt64(buf, strlen(buf), &v) == true);
    assert(v == UINT64_MAX);

    /* Values above UINT64_MAX are refused, including those whose wrapped
     * v*10+d is not smaller than v (2050000000000000000 * 10 wraps to
     * 2053255926290448384) */
    static const char *const above[] = {
        "18446744073709551616", /* UINT64_MAX + 1 */
        "18446744073709551619",  "18446744073709551620",
        "20500000000000000000",  "36893488147419103232",  /* 2^65 */
        "99999999999999999999",  "184467440737095516150", /* UINT64_MAX*10 */
        "100000000000000000000",                          /* 21 digits */
    };
    for (size_t i = 0; i < countof(above); i++) {
        v = 12345;
        if (StrBufToUInt64(above[i], strlen(above[i]), &v) || v != 12345) {
            printf("StrBufToUInt64 accepted %s\n", above[i]);
            assert(false);
        }

        uint64_t n = 0;
        if (StrBufToUInt64FastCheckNumeric(above[i], strlen(above[i]), &n)) {
            printf("StrBufToUInt64FastCheckNumeric accepted %s\n", above[i]);
            assert(false);
        }

        n = 0;
        if (StrBufToUInt64FastCheckOverflow(above[i], strlen(above[i]), &n) ||
            n != UINT64_MAX) {
            printf("StrBufToUInt64FastCheckOverflow accepted %s\n", above[i]);
            assert(false);
        }
    }

    /* The largest 20-digit values in range, and 19-digit ones near 10^19 */
    static const struct {
        const char *s;
        uint64_t want;
    } edge[] = {
        {"18446744073709551614", UINT64_MAX - 1},
        {"18446744073709551610", UINT64_MAX - 5},
        {"10000000000000000000", UINT64_C(10000000000000000000)},
        {"9999999999999999999", UINT64_C(9999999999999999999)},
        {"1844674407370955161", UINT64_MAX / 10},
        {"10", 10},
    };
    for (size_t i = 0; i < countof(edge); i++) {
        const size_t len = strlen(edge[i].s);
        uint64_t a = 0;
        uint64_t b = 0;
        uint64_t c = 0;
        if (!StrBufToUInt64(edge[i].s, len, &a) || a != edge[i].want ||
            !StrBufToUInt64FastCheckNumeric(edge[i].s, len, &b) ||
            b != edge[i].want ||
            !StrBufToUInt64FastCheckOverflow(edge[i].s, len, &c) ||
            c != edge[i].want) {
            printf("StrBufToUInt64 family misparsed %s\n", edge[i].s);
            assert(false);
        }
    }

    /* Grammar: leading zeros, signs and non-digits fail; only the
     * FastCheck parsers allow leading zeros */
    static const char *const malformed[] = {
        "", "00", "007", "-1", "-0", "1a", "a1", "1.0", "1e3", "1-", "1 2"};
    for (size_t i = 0; i < countof(malformed); i++) {
        v = 12345;
        if (StrBufToUInt64(malformed[i], strlen(malformed[i]), &v) ||
            v != 12345) {
            printf("StrBufToUInt64 accepted '%s'\n", malformed[i]);
            assert(false);
        }
    }

    uint64_t lz = 0;
    assert(StrBufToUInt64FastCheckNumeric("007", 3, &lz) && lz == 7);
    assert(!StrBufToUInt64FastCheckNumeric("0x7", 3, &lz));
}

static void testStrInt64ToBuf(size_t (*strFn)(void *, size_t, int64_t)) {
    char buf[32];
    int64_t v;
    int sz;

    v = 0;
    sz = strFn(buf, sizeof buf, v);
    assert(sz == 1);
    assert(!strncmp(buf, "0", sz));

    v = -1;
    sz = strFn(buf, sizeof buf, v);
    assert(sz == 2);
    assert(!strncmp(buf, "-1", sz));

    v = 99;
    sz = strFn(buf, sizeof buf, v);
    assert(sz == 2);
    assert(!strncmp(buf, "99", sz));

    v = -99;
    sz = strFn(buf, sizeof buf, v);
    assert(sz == 3);
    assert(!strncmp(buf, "-99", sz));

    v = 9999999999999;
    sz = strFn(buf, sizeof buf, v);
    assert(sz == 13);
    assert(!strncmp(buf, "9999999999999", sz));

    v = -2147483648;
    sz = strFn(buf, sizeof buf, v);
    assert(sz == 11);
    assert(!strncmp(buf, "-2147483648", sz));

    v = INT64_MIN;
    sz = strFn(buf, sizeof buf, v);
    assert(sz == 20);
    assert(!strncmp(buf, "-9223372036854775808", sz));

    v = INT64_MAX;
    sz = strFn(buf, sizeof buf, v);
    assert(sz == 19);
    assert(!strncmp(buf, "9223372036854775807", sz));
}

static uint64_t microTestA(uint32_t start) {
#define _mStrValTimes10(val) (((val) << 1) + ((val) << 3))
#define _mStrValTimes100(val) _mStrValTimes10(_mStrValTimes10(val))
    uint64_t result = 0;
    while (start--) {
        result += _mStrValTimes100(start);
        assert(result);
    }
    return result;
}

static uint64_t microTestB(uint32_t start) {
    uint64_t result = 0;
    while (start--) {
        result += start * 10;
        assert(result);
    }
    return result;
}

#define CHECK_NEWLINE_STEP const size_t
#define CHECK_NEWLINE_STEP_SIZE sizeof(CHECK_NEWLINE_STEP)
#define TEST_ONEMASK                                                           \
    ((CHECK_NEWLINE_STEP)(-1) / 0xFF) /* same: ~(size_t)0/255 */

/* also see datakit/str.c:StrLenUtf8() for why this works */
#define newLineCheck (ONEMASK * NL)

/* using bytes of width CHECK_NEWLINE_STEP, compare all at once
 * checking for NL */
/* from:
 * https://graphics.stanford.edu/~seander/bithacks.html##ValueInWord
 * modified using our own constants above */
#define haszero(v) (((v) - TEST_ONEMASK) & ~(v) & (TEST_ONEMASK * 0x80))
#define hasvalue(x, n) (haszero((x) ^ (n)))
#define hasnewline(x) (hasvalue(x, newLineCheck))

/* target */
#define NL '\n'

#ifdef __AVX2__
#include <x86intrin.h>
#define CHECK_NEWLINE_STEP_AVX const __m256i
#define CHECK_NEWLINE_STEP_SIZE_AVX sizeof(CHECK_NEWLINE_STEP_AVX)
#endif

#include "strDoubleFormat.h"
#include <wchar.h>
DK_FN_NO_OPTIMIZE int strTest(int, char *[]) {
    int err = 0;

    TEST("StrTenPow / StrTenPowBig exact powers") {
        /* Every entry verified against iterated multiplication; the
         * doubleFormat 128-bit path scales by these, so one wrong or
         * missing entry silently corrupts formatted output */
        __uint128_t want = 1;
        for (size_t p = 0; p <= 38; p++) {
            if (p <= 18 && StrTenPow(p) != (uint64_t)want) {
                ERR("StrTenPow(%zu) wrong", p);
            }
            if (StrTenPowBig(p) != want) {
                ERR("StrTenPowBig(%zu) wrong (high %" PRIu64 " low %" PRIu64
                    ")",
                    p, (uint64_t)(StrTenPowBig(p) >> 64),
                    (uint64_t)StrTenPowBig(p));
            }
            want *= 10;
        }
    }

    TEST("StrDigitCount: every power-of-ten boundary, every width") {
        /* 10^k - 1 has k digits and 10^k has k + 1, for each width's
         * whole range (the 128-bit count recurses above 10^12), plus 0,
         * each maximum and the signed extremes */
        __uint128_t p = 10;
        for (size_t k = 1; k <= 38; k++, p *= 10) {
            if (StrDigitCountUInt128(p - 1) != k ||
                StrDigitCountUInt128(p) != k + 1) {
                ERR("128-bit count wrong at 10^%zu", k);
            }
            if (k <= 19 &&
                (StrDigitCountUInt64((uint64_t)(p - 1)) != k ||
                 (k < 19 && StrDigitCountUInt64((uint64_t)p) != k + 1))) {
                ERR("64-bit count wrong at 10^%zu", k);
            }
            if (k <= 9 && (StrDigitCountUInt32((uint32_t)(p - 1)) != k ||
                           StrDigitCountUInt32((uint32_t)p) != k + 1)) {
                ERR("32-bit count wrong at 10^%zu", k);
            }
        }
        if (StrDigitCountUInt128(0) != 1 || StrDigitCountUInt64(0) != 1 ||
            StrDigitCountUInt32(0) != 1 ||
            StrDigitCountUInt128(~(__uint128_t)0) != 39 ||
            StrDigitCountUInt64(UINT64_MAX) != 20 ||
            StrDigitCountUInt32(UINT32_MAX) != 10 ||
            StrDigitCountInt64(INT64_MIN) != 20 ||
            StrDigitCountInt64(INT64_MAX) != 19 ||
            StrDigitCountInt64(-1) != 2 || StrDigitCountInt64(0) != 1) {
            ERR("digit count wrong at 0, a maximum or a signed extreme");
        }
    }

    TEST("StrUtf8 width functions agree with each other, invalid bytes too") {
        /* Random strings of narrow, wide, zero-width and invalid pieces
         * (each invalid byte is one cell).  The whole width is the same
         * by every route; truncation keeps the longest prefix that fits
         * and splits no character; the index at a width reaches it (one
         * less when a wide character straddles it); pad, wide and narrow
         * queries follow. */
        static const struct {
            const char *bytes;
            size_t len;
        } pieces[] = {
            {"a", 1},
            {"\xe4\xb8\xad", 3}, /* wide */
            {"\xcc\x81", 2},     /* combining: zero width */
            {"\xf0\x9f\x98\x80", 4},
            {"\xff", 1},
            {"\xe4\xb8", 2}, /* truncated */
            {"\x80", 1},
            {"\xc0\x80", 2},     /* overlong */
            {"\xed\xa0\x80", 3}, /* surrogate */
        };
        uint64_t rng = 0x07f8;
        for (int iter = 0; iter < 3000 && !err; iter++) {
            uint8_t s[64];
            size_t L = 0;
            dkLcg64Next(&rng);
            const size_t count = (size_t)((rng >> 33) % 12);
            for (size_t k = 0; k < count; k++) {
                rng = rng * 6364136223846793005ULL + 1;
                const size_t p = (size_t)((rng >> 40) % (countof(pieces)));
                memcpy(s + L, pieces[p].bytes, pieces[p].len);
                L += pieces[p].len;
            }

            const size_t W = StrUtf8Width(s, L);
            bool hasWide = false;
            bool narrow = true;
            for (size_t i = 0; i < L;) {
                const uint8_t *pos = s + i;
                size_t rem = L - i;
                const uint32_t cp = StrUtf8Decode(&pos, &rem);
                if (cp == 0xFFFFFFFF) {
                    i++;
                    continue;
                }
                const int w = StrUnicodeEastAsianWidth(cp);
                hasWide = hasWide || w == 2;
                narrow = narrow && w == 1;
                i = (size_t)(pos - s);
            }
            if (StrUtf8WidthN(s, L, SIZE_MAX) != W ||
                StrUtf8WidthN(s, L, 0) != 0 ||
                StrUtf8WidthBetween(s, L, 0, L) != W ||
                StrUtf8WidthBetween(s, L, L, L + 4) != 0 ||
                StrUtf8WidthBetween(s, L, 3, 1) != 0 ||
                StrUtf8WidthAt(s, L, L + 10) != W ||
                StrUtf8PadWidth(s, L, W + 3) != 3 ||
                StrUtf8PadWidth(s, L, W) != 0 ||
                StrUtf8HasWide(s, L) != hasWide ||
                StrUtf8IsNarrow(s, L) != narrow) {
                ERR("iter %d: whole-string width queries disagree (W %zu)",
                    iter, W);
                break;
            }

            for (size_t w = 0; w <= W + 1; w++) {
                const size_t t = StrUtf8TruncateWidth(s, L, w);
                /* the next character (or invalid byte) after t */
                size_t next = t;
                if (t < L) {
                    const uint8_t *pos = s + t;
                    size_t rem = L - t;
                    next = StrUtf8Decode(&pos, &rem) == 0xFFFFFFFF
                               ? t + 1
                               : (size_t)(pos - s);
                }
                const size_t at = StrUtf8IndexAtWidth(s, L, w);
                const size_t atWidth = StrUtf8Width(s, at);
                if (t > L || StrUtf8Width(s, t) > w || (t == L) != (W <= w) ||
                    (t < L && StrUtf8Width(s, next) <= w) || at > L ||
                    atWidth > w || (W >= w && atWidth + 1 < w) ||
                    (W < w && at != L)) {
                    ERR("iter %d width %zu: truncate %zu / index %zu wrong",
                        iter, w, t, at);
                    break;
                }
            }
        }
    }

    TEST("StrUnicodeIsLetter: letters of each script, and the non-letters "
         "beside them") {
        static constexpr uint32_t letters[] = {'A',
                                               'z',
                                               0x00C0 /* À */,
                                               0x00E9 /* é */,
                                               0x00F1 /* ñ */,
                                               0x00DF /* ß */,
                                               0x00FF /* ÿ */,
                                               0x00AA /* ª */,
                                               0x00B5 /* µ */,
                                               0x0101,
                                               0x0250 - 1,
                                               0x0391 /* Α */,
                                               0x03C9 /* ω */,
                                               0x0416 /* Ж */,
                                               0x05D0 /* א */,
                                               0x0628 /* ب */,
                                               0x4E2D /* 中 */,
                                               0x3042 /* あ */,
                                               0x30A2 /* ア */,
                                               0xAC00 /* 가 */};
        static constexpr uint32_t others[] = {'0',
                                              '9',
                                              ' ',
                                              '_',
                                              0x00D7 /* × */,
                                              0x00F7 /* ÷ */,
                                              0x00B7 /* · */,
                                              0x00BF /* ¿ */,
                                              0x037E /* Greek ; */,
                                              0x0387 /* · */,
                                              0x03F6,
                                              0x0482 /* ҂ */,
                                              0x0483 /* combining */,
                                              0x0660 /* Arabic 0 */,
                                              0x3000 /* ideographic space */};
        for (size_t i = 0; i < countof(letters); i++) {
            if (!StrUnicodeIsLetter(letters[i])) {
                ERR("U+%04" PRIX32 " is a letter", letters[i]);
            }
        }
        for (size_t i = 0; i < countof(others); i++) {
            if (StrUnicodeIsLetter(others[i])) {
                ERR("U+%04" PRIX32 " is not a letter", others[i]);
            }
        }
        /* The digits it knows */
        static constexpr uint32_t digits[] = {'5', 0xFF15, 0x0665, 0x06F5,
                                              0x096B};
        for (size_t i = 0; i < countof(digits); i++) {
            if (!StrUnicodeIsDigit(digits[i]) ||
                StrUnicodeIsLetter(digits[i])) {
                ERR("U+%04" PRIX32 " is a digit, not a letter", digits[i]);
            }
        }
    }

    TEST("StrUnicodeIsGraphemeBreak follows the segmentation rules") {
        /* Pairs from UAX #29's rules: false where the rule joins, true
         * where it breaks */
        static const struct {
            uint32_t a;
            uint32_t b;
            bool brk;
            const char *rule;
        } pairs[] = {
            {0x000D, 0x000A, false, "GB3 CR x LF"},
            {0x000A, 0x0061, true, "GB4 LF /"},
            {0x0061, 0x200B, true, "GB5 / ZWSP (Control)"},
            {0x200B, 0x0061, true, "GB4 ZWSP (Control) /"},
            {0x1100, 0x1161, false, "GB6 L x V"},
            {0x1100, 0xAC00, false, "GB6 L x LV"},
            {0xAC00, 0x11A8, false, "GB7 LV x T"},
            {0x1161, 0x11A8, false, "GB7 V x T"},
            {0xAC01, 0x11A8, false, "GB8 LVT x T"},
            {0x11A8, 0x1100, true, "T / L"},
            {0x0061, 0x0308, false, "GB9 x Extend"},
            {0x0061, 0x200C, false, "GB9 x ZWNJ (Extend)"},
            {0x1F44D, 0x1F3FB, false, "GB9 emoji x skin tone"},
            {0x1F3F4, 0xE0067, false, "GB9 x tag"},
            {0xE0067, 0xE007F, false, "GB9 tag x cancel tag"},
            {0x1F468, 0x200D, false, "GB9 x ZWJ"},
            {0x200D, 0x1F469, false, "GB11 ZWJ x ExtPict"},
            {0x200D, 0x2764, false, "GB11 ZWJ x heart"},
            {0x200D, 0x0061, true, "ZWJ / letter"},
            {0x1F1E6, 0x1F1E7, false, "GB12 RI x RI"},
            {0x0061, 0x0062, true, "GB999"},
        };
        for (size_t i = 0; i < countof(pairs); i++) {
            if (StrUnicodeIsGraphemeBreak(pairs[i].a, pairs[i].b) !=
                pairs[i].brk) {
                ERR("%s: U+%04" PRIX32 " U+%04" PRIX32 " wrong", pairs[i].rule,
                    pairs[i].a, pairs[i].b);
            }
        }
    }

    TEST("StrBloom / StrBloomBig: full bit range usable") {
        /* 64-bit filter round-trip */
        StrBloomFilter small = 0;
        StrBloomSet(small, 3);
        StrBloomSet(small, 63);
        assert(StrBloomTest(small, 3));
        assert(StrBloomTest(small, 63));
        assert(!StrBloomTest(small, 4));

        /* 128-bit filter: high-half positions (>= 64) must be REAL bits
         * distinct from their (x & 63) aliases, and each position must
         * set exactly one bit. */
        StrBloomFilterBig big = 0;
        StrBloomBigSet(big, 100);
        assert(StrBloomBigTest(big, 100));
        assert(!StrBloomBigTest(big, 100 & 63)); /* no low-half aliasing */
        assert(!StrBloomBigTest(big, 99));

        StrBloomBigSet(big, 127);
        StrBloomBigSet(big, 0);
        assert(StrBloomBigTest(big, 127));
        assert(StrBloomBigTest(big, 0));

        /* every position 0..127 sets a distinct single bit */
        for (uint32_t i = 0; i < 128; i++) {
            StrBloomFilterBig one = 0;
            StrBloomBigSet(one, i);
            assert(one == ((StrBloomFilterBig)1 << i));
        }
    }

    /* ====================================================================
     * Stress tests for SIMD vs baseline implementations - run first!
     * ==================================================================== */
    TEST("StrIsDigitsFast vs StrIsDigitsIndividual stress test") {
        printf("  Testing StrIsDigitsFast matches baseline...\n");

        /* Test all-digits strings of various sizes */
        for (size_t size = 0; size <= 256; size++) {
            cleanerName(zfree, Chars) char *buf = zmalloc(size + 1);
            for (size_t i = 0; i < size; i++) {
                buf[i] = '0' + (i % 10);
            }
            buf[size] = '\0';

            bool fastResult = StrIsDigitsFast(buf, size);
            bool baseResult = StrIsDigitsIndividual(buf, size);
            if (fastResult != baseResult) {
                ERR("StrIsDigitsFast mismatch at size %zu (all digits): "
                    "fast=%d base=%d",
                    size, fastResult, baseResult);
            }
        }

        /* Test strings with non-digit at various positions */
        for (size_t size = 1; size <= 128; size++) {
            for (size_t badPos = 0; badPos < size; badPos++) {
                cleanerName(zfree, Chars) char *buf = zmalloc(size + 1);
                for (size_t i = 0; i < size; i++) {
                    buf[i] = '0' + (i % 10);
                }
                buf[badPos] = 'X'; /* Insert non-digit */
                buf[size] = '\0';

                bool fastResult = StrIsDigitsFast(buf, size);
                bool baseResult = StrIsDigitsIndividual(buf, size);
                if (fastResult != baseResult) {
                    ERR("StrIsDigitsFast mismatch at size %zu, badPos %zu: "
                        "fast=%d base=%d",
                        size, badPos, fastResult, baseResult);
                }
            }
        }

        /* Test boundary characters */
        const char boundaryChars[] = {'\0', '/', ':',  'a',
                                      'A',  ' ', 0x7F, (char)0xFF};
        for (size_t ci = 0; ci < sizeof(boundaryChars); ci++) {
            for (size_t size = 1; size <= 64; size++) {
                cleanerName(zfree, Chars) char *buf = zmalloc(size + 1);
                for (size_t i = 0; i < size; i++) {
                    buf[i] = '5';
                }
                buf[size / 2] = boundaryChars[ci];
                buf[size] = '\0';

                bool fastResult = StrIsDigitsFast(buf, size);
                bool baseResult = StrIsDigitsIndividual(buf, size);
                if (fastResult != baseResult) {
                    ERR("StrIsDigitsFast boundary mismatch at size %zu, "
                        "char 0x%02X: fast=%d base=%d",
                        size, (uint8_t)boundaryChars[ci], fastResult,
                        baseResult);
                }
            }
        }

        printf("    StrIsDigitsFast stress test passed!\n");
    }

    TEST("StrUInt9DigitsToBuf correctness stress test") {
        printf("  Testing StrUInt9DigitsToBuf correctness...\n");

        /* Test all 9-digit boundary values */
        const uint32_t testValues[] = {
            0,         1,         9,         10,         99,       100,
            999,       1000,      9999,      10000,      99999,    100'000,
            999999,    1'000'000, 9999999,   10'000'000, 99999999, 100'000'000,
            999999999, 123456789, 987654321, 111111111,  500000000};

        for (size_t i = 0; i < countof(testValues); i++) {
            uint32_t val = testValues[i];
            char buf[10] = {};
            StrUInt9DigitsToBuf(buf, val);

            /* Verify by parsing back */
            uint64_t parsed = 0;
            for (int j = 0; j < 9; j++) {
                if (buf[j] < '0' || buf[j] > '9') {
                    ERR("StrUInt9DigitsToBuf produced non-digit at pos %d for "
                        "value %u: 0x%02X",
                        j, val, (uint8_t)buf[j]);
                }
                parsed = parsed * 10 + (buf[j] - '0');
            }
            if (parsed != val) {
                ERR("StrUInt9DigitsToBuf mismatch for %u: got %s (parsed as "
                    "%" PRIu64 ")",
                    val, buf, parsed);
            }
        }

        /* Exhaustive test for smaller ranges */
        for (uint32_t val = 0; val < 100'000; val++) {
            char buf[10] = {};
            StrUInt9DigitsToBuf(buf, val);

            uint64_t parsed = 0;
            for (int j = 0; j < 9; j++) {
                parsed = parsed * 10 + (buf[j] - '0');
            }
            if (parsed != val) {
                ERR("StrUInt9DigitsToBuf exhaustive mismatch for %u", val);
            }
        }

        /* Random test for larger values */
        uint64_t rngState = 0x12345678;
        for (int i = 0; i < 100'000; i++) {
            rngState = rngState * 6364136223846793005ULL + 1;
            uint32_t val = (uint32_t)(rngState >> 32) % 1'000'000'000;

            char buf[10] = {};
            StrUInt9DigitsToBuf(buf, val);

            uint64_t parsed = 0;
            for (int j = 0; j < 9; j++) {
                parsed = parsed * 10 + (buf[j] - '0');
            }
            if (parsed != val) {
                ERR("StrUInt9DigitsToBuf random mismatch for %u", val);
            }
        }

        printf("    StrUInt9DigitsToBuf stress test passed!\n");
    }

#if defined(__SSE2__) || defined(__ARM_NEON)
    TEST("StrUInt4DigitsToBuf and StrUInt8DigitsToBuf stress test") {
        printf("  Testing StrUInt4/8DigitsToBuf correctness...\n");

        /* Test StrUInt4DigitsToBuf */
        for (uint32_t val = 0; val < 10000; val++) {
            char buf[5] = {};
            StrUInt4DigitsToBuf(buf, val);

            uint32_t parsed = 0;
            for (int j = 0; j < 4; j++) {
                if (buf[j] < '0' || buf[j] > '9') {
                    ERR("StrUInt4DigitsToBuf non-digit at pos %d for %u", j,
                        val);
                }
                parsed = parsed * 10 + (buf[j] - '0');
            }
            if (parsed != val) {
                ERR("StrUInt4DigitsToBuf mismatch for %u: got %s", val, buf);
            }
        }

        /* Test StrUInt8DigitsToBuf */
        const uint32_t test8Values[] = {0,        1,        99,       999,
                                        9999,     99999,    999999,   9999999,
                                        99999999, 12345678, 87654321, 50000000};
        for (size_t i = 0; i < countof(test8Values); i++) {
            uint32_t val = test8Values[i];
            char buf[9] = {};
            StrUInt8DigitsToBuf(buf, val);

            uint32_t parsed = 0;
            for (int j = 0; j < 8; j++) {
                if (buf[j] < '0' || buf[j] > '9') {
                    ERR("StrUInt8DigitsToBuf non-digit at pos %d for %u", j,
                        val);
                }
                parsed = parsed * 10 + (buf[j] - '0');
            }
            if (parsed != val) {
                ERR("StrUInt8DigitsToBuf mismatch for %u: got %s", val, buf);
            }
        }

        /* Exhaustive test for StrUInt8DigitsToBuf on smaller range */
        for (uint32_t val = 0; val < 100'000; val++) {
            char buf[9] = {};
            StrUInt8DigitsToBuf(buf, val);

            uint32_t parsed = 0;
            for (int j = 0; j < 8; j++) {
                parsed = parsed * 10 + (buf[j] - '0');
            }
            if (parsed != val) {
                ERR("StrUInt8DigitsToBuf exhaustive mismatch for %u", val);
            }
        }

        printf("    StrUInt4/8DigitsToBuf stress test passed!\n");
    }
#endif

    TEST("StrScanScan applies the sign to INT32_MIN and INT64_MIN exactly") {
        /* Each magnitude here sits exactly at the bound the scanner
         * accepts for a negative value, where negating the signed cast
         * would overflow */
        static const struct {
            const char *s;
            StrScanOpt opt;
            StrScanFmt fmt;
            int64_t i;  /* for STRSCAN_INT */
            uint64_t u; /* for STRSCAN_I64 / STRSCAN_U64 */
        } cases[] = {
            {"-2147483648", STRSCAN_OPT_TOINT, STRSCAN_INT, INT32_MIN, 0},
            {"-2147483647", STRSCAN_OPT_TOINT, STRSCAN_INT, -INT32_MAX, 0},
            {"2147483647", STRSCAN_OPT_TOINT, STRSCAN_INT, INT32_MAX, 0},
            {"-0x80000000", STRSCAN_OPT_TOINT, STRSCAN_INT, INT32_MIN, 0},
            {"-0x7fffffff", STRSCAN_OPT_TOINT, STRSCAN_INT, -INT32_MAX, 0},
            {"-9223372036854775808LL", STRSCAN_OPT_C | STRSCAN_OPT_LL,
             STRSCAN_I64, 0, UINT64_C(0x8000000000000000)},
            {"-9223372036854775807LL", STRSCAN_OPT_C | STRSCAN_OPT_LL,
             STRSCAN_I64, 0, UINT64_C(0x8000000000000001)},
            {"-0x8000000000000000LL", STRSCAN_OPT_C | STRSCAN_OPT_LL,
             STRSCAN_I64, 0, UINT64_C(0x8000000000000000)},
            {"-0xffffffffffffffffULL", STRSCAN_OPT_C | STRSCAN_OPT_LL,
             STRSCAN_U64, 0, 1},
            {"-1LL", STRSCAN_OPT_C | STRSCAN_OPT_LL, STRSCAN_I64, 0,
             UINT64_MAX},
        };

        /* The sign is parsed with or without the float words */
        for (size_t c = 0; c < 2 * countof(cases); c++) {
            const size_t i = c / 2;
            const bool words = c % 2;
            databox box = {};
            const StrScanFmt fmt = StrScanScan(
                (const uint8_t *)cases[i].s, &box, cases[i].opt, words, false);
            if (fmt != cases[i].fmt) {
                ERR("StrScanScan(%s) format %d, want %d", cases[i].s, fmt,
                    cases[i].fmt);
            } else if (fmt == STRSCAN_INT && box.data.i64 != cases[i].i) {
                ERR("StrScanScan(%s) = %" PRId64 ", want %" PRId64, cases[i].s,
                    box.data.i64, cases[i].i);
            } else if (fmt != STRSCAN_INT && box.data.u64 != cases[i].u) {
                ERR("StrScanScan(%s) = %" PRIx64 ", want %" PRIx64, cases[i].s,
                    box.data.u64, cases[i].u);
            }
        }
    }

    TEST("StrAtoi64 clamps magnitudes past 64 bits instead of wrapping") {
        static const struct {
            const char *s;
            int32_t rc;
            int64_t want;
        } cases[] = {
            {"-9223372036854775808", 0, INT64_MIN},
            {"9223372036854775807", 0, INT64_MAX},
            {"9223372036854775808", 2, INT64_MAX},
            {"18446744073709551616", 1, INT64_MAX},
            {"20500000000000000000", 1, INT64_MAX},
            {"36893488147419103232", 1, INT64_MAX},
            {"-36893488147419103232", 1, INT64_MIN},
            {"123456789012345678901234567890", 1, INT64_MAX},
            {"-123456789012345678901234567890", 1, INT64_MIN},
        };
        for (size_t i = 0; i < countof(cases); i++) {
            int64_t got = 0;
            const int32_t rc = StrAtoi64(
                cases[i].s, &got, (int32_t)strlen(cases[i].s), STR_UTF8, false);
            if (rc != cases[i].rc || got != cases[i].want) {
                ERR("StrAtoi64(%s) = %" PRId32 ", %" PRId64 "; want %" PRId32
                    ", %" PRId64,
                    cases[i].s, rc, got, cases[i].rc, cases[i].want);
            }
        }
    }

    TEST("StrAtoF: exponents, limits and partial numbers against strtod") {
        static const struct {
            const char *s;
            bool ok;
        } cases[] = {
            {"1e5", true},
            {"-2.5E-3", true},
            {"+.5e+2", true},
            {"0.0", true},
            {"-0", true},
            {"1e400", true},
            {"1e-400", true},
            {"1e310", true},
            {"1e-310", true},
            {"1.5e308", true},
            {"2e-320", true},
            {"4.9e-324", true},
            {"123456789012345678901234", true},
            {"0.000123", true},
            {"1e", false},
            {"1e+", false},
            {"4x", false},
            {"", false},
            {"-", false},
            {".", false},
        };
        for (size_t i = 0; i < countof(cases); i++) {
            const char *s = cases[i].s;
            double got = -1.0;
            const bool ok =
                StrAtoF(s, &got, (int32_t)strlen(s), STR_UTF8, false);
            /* strtod reads the same prefix StrAtoF converts, correctly
             * rounded: StrAtoF must give the same double, subnormals,
             * overflow to infinity and dropped digits included */
            const double want = strtod(s, nullptr);
            const bool close = memcmp(&got, &want, sizeof(got)) == 0;
            if (ok != cases[i].ok || !close || signbit(got) != signbit(want)) {
                ERR("StrAtoF(%s) = %d, %.17g; want %d, %.17g", s, ok, got,
                    cases[i].ok, want);
            }
        }
        /* Random decimals across the double range, 1-25 digits */
        uint64_t rng = 0xa70f;
        for (int n = 0; n < 3000; n++) {
            char s[64];
            size_t at = 0;
            if (splitmix64(&rng) & 1) {
                s[at++] = '-';
            }
            const size_t digits = 1 + splitmix64(&rng) % 25;
            const size_t point = splitmix64(&rng) % (digits + 1);
            for (size_t k = 0; k < digits; k++) {
                if (k == point && k > 0) {
                    s[at++] = '.';
                }
                s[at++] = (char)('0' + splitmix64(&rng) % 10);
            }
            at += (size_t)snprintf(s + at, sizeof(s) - at, "e%d",
                                   (int)(splitmix64(&rng) % 680) - 340);
            double got = 0;
            const bool ok = StrAtoF(s, &got, (int32_t)at, STR_UTF8, false);
            const double want = strtod(s, nullptr);
            if (!ok || memcmp(&got, &want, sizeof(got)) != 0) {
                ERR("StrAtoF(%s) = %d, %.17g; strtod %.17g", s, ok, got, want);
                break;
            }
        }

        double spaced = 0;
        if (!StrAtoF("  42  ", &spaced, 6, STR_UTF8, true) || spaced != 42.0 ||
            StrAtoF("  42  ", &spaced, 6, STR_UTF8, false)) {
            ERR("StrAtoF space skipping");
        }
    }

    TEST("StrScanScan: decimals and hex floats against strtod") {
        /* Rounding edges: the halfway cases at DBL_MIN, the smallest
         * subnormal and its half, the largest double and just past it */
        static const char *edges[] = {
            "2.2250738585072011e-308",
            "2.2250738585072012e-308",
            "4.9406564584124654e-324",
            "2.4703282292062327e-324",
            "2.4703282292062328e-324",
            "1.7976931348623157e308",
            "1.7976931348623158e308",
            "1.7976931348623159e308",
            "9007199254740993",
            "9007199254740992.5000000000000000000000001",
            "0.1",
            "123456789012345678901234567890123456789e-20",
            "0x1.8p3",
            "0x.1p-2",
            "-0x1P-1074",
            "0x1p-1075",
            "0x1.0000000000000800000001p0",
            "0x1.fffffffffffff8p1023",
            "0x123456789abcdef0123p-40",
        };
        for (size_t i = 0; i < countof(edges); i++) {
            databox box = {};
            const StrScanFmt fmt = StrScanScan((const uint8_t *)edges[i], &box,
                                               STRSCAN_OPT_TONUM, false, false);
            const double want = strtod(edges[i], nullptr);
            if (fmt != STRSCAN_NUM ||
                memcmp(&box.data.d64, &want, sizeof(want)) != 0) {
                ERR("StrScanScan(%s) = %d, %.17g; strtod %.17g", edges[i], fmt,
                    box.data.d64, want);
            }
        }

        /* Random decimals of up to 40 digits and random hex floats */
        static constexpr char hexd[] = "0123456789abcdef";
        uint64_t rng = 0x5ca9;
        for (int n = 0; n < 4000; n++) {
            const bool hex = n & 1;
            char s[96];
            size_t at = 0;
            if (splitmix64(&rng) & 1) {
                s[at++] = '-';
            }
            if (hex) {
                s[at++] = '0';
                s[at++] = 'x';
            }
            const size_t digits = 1 + splitmix64(&rng) % (hex ? 24 : 40);
            const size_t point = splitmix64(&rng) % (digits + 1);
            for (size_t k = 0; k < digits; k++) {
                if (k == point) {
                    s[at++] = '.';
                }
                s[at++] = hexd[splitmix64(&rng) % (hex ? 16 : 10)];
            }
            const int span = hex ? 2300 : 700;
            at += (size_t)snprintf(s + at, sizeof(s) - at, "%c%d",
                                   hex ? 'p' : 'e',
                                   (int)(splitmix64(&rng) % span) - span / 2);
            databox box = {};
            const StrScanFmt fmt = StrScanScan((const uint8_t *)s, &box,
                                               STRSCAN_OPT_TONUM, false, false);
            const double want = strtod(s, nullptr);
            if (fmt != STRSCAN_NUM ||
                memcmp(&box.data.d64, &want, sizeof(want)) != 0) {
                ERR("StrScanScan(%s) = %d, %.17g; strtod %.17g", s, fmt,
                    box.data.d64, want);
                break;
            }
        }
    }

    TEST("StrScanScan: C integers, suffixes, imaginary and refusals") {
        enum {
            C = STRSCAN_OPT_C,
            LL = STRSCAN_OPT_LL,
            IM = STRSCAN_OPT_IMAG,
        };
        static const struct {
            const char *s;
            int opt;
            StrScanFmt fmt;
            uint64_t bits; /* integers: the box's 64 bits */
            double d;      /* numbers */
        } cases[] = {
            {"0777", C, STRSCAN_INT, 511, 0},
            {"0777", 0, STRSCAN_INT, 777, 0},
            {"-0777", C, STRSCAN_INT, (uint64_t)-511, 0},
            {"08", C, STRSCAN_ERROR, 0, 0},
            {"077777777777", C, STRSCAN_ERROR, 0, 0},
            {"020000000000", C, STRSCAN_U32, 0x80000000u, 0},
            {"0777LL", C | LL, STRSCAN_I64, 511, 0},
            {"0x1fu", C, STRSCAN_U32, 31, 0},
            {"123u", C, STRSCAN_U32, 123, 0},
            {"123u", 0, STRSCAN_ERROR, 0, 0},
            {"4294967295u", C, STRSCAN_U32, 0xffffffffu, 0},
            {"4294967296u", C, STRSCAN_ERROR, 0, 0},
            {"123LL", LL, STRSCAN_I64, 123, 0},
            {"123LL", 0, STRSCAN_ERROR, 0, 0},
            {"-5LL", LL, STRSCAN_I64, (uint64_t)-5, 0},
            {"5LLU", LL, STRSCAN_U64, 5, 0},
            {"18446744073709551615ULL", LL, STRSCAN_U64, UINT64_MAX, 0},
            {"18446744073709551616ULL", LL, STRSCAN_ERROR, 0, 0},
            {"0x10ULL", LL, STRSCAN_U64, 16, 0},
            {"0x12345678901234567ULL", LL, STRSCAN_ERROR, 0, 0},
            {"10l", 0, STRSCAN_ERROR, 0, 0},
            {"0x123456789", C, STRSCAN_ERROR, 0, 0},
            {"0x80000000", C, STRSCAN_U32, 0x80000000u, 0},
            {"0x80000000", 0, STRSCAN_NUM, 0, 2147483648.0},
            {"9999999999", C, STRSCAN_ERROR, 0, 0},
            {"9999999999", 0, STRSCAN_NUM, 0, 9999999999.0},
            {"2i", IM, STRSCAN_IMAG, 0, 2.0},
            {"2.5I", IM, STRSCAN_IMAG, 0, 2.5},
            {"2i", 0, STRSCAN_ERROR, 0, 0},
            {"1..2", 0, STRSCAN_ERROR, 0, 0},
            {"..1", 0, STRSCAN_ERROR, 0, 0},
            {"1e", 0, STRSCAN_ERROR, 0, 0},
            {"1e+", 0, STRSCAN_ERROR, 0, 0},
            {"0x", 0, STRSCAN_ERROR, 0, 0},
            {".", 0, STRSCAN_ERROR, 0, 0},
            {"1.2.3", 0, STRSCAN_ERROR, 0, 0},
            {"++1", 0, STRSCAN_ERROR, 0, 0},
            {"1x", 0, STRSCAN_ERROR, 0, 0},
            {"0.000", 0, STRSCAN_NUM, 0, 0.0},
            {"1e99999", 0, STRSCAN_NUM, 0, INFINITY},
            {"-1e-99999", 0, STRSCAN_NUM, 0, -0.0},
        };
        for (size_t i = 0; i < countof(cases); i++) {
            databox box = {};
            const StrScanFmt fmt =
                StrScanScan((const uint8_t *)cases[i].s, &box,
                            (StrScanOpt)cases[i].opt, false, false);
            const bool number =
                cases[i].fmt == STRSCAN_NUM || cases[i].fmt == STRSCAN_IMAG;
            if (fmt != cases[i].fmt ||
                (fmt != STRSCAN_ERROR && !number &&
                 box.data.u64 != cases[i].bits) ||
                (number &&
                 memcmp(&box.data.d64, &cases[i].d, sizeof(double)) != 0)) {
                ERR("StrScanScan(%s, opt %d) = %d, %" PRIx64 " / %g; want %d",
                    cases[i].s, cases[i].opt, fmt, box.data.u64, box.data.d64,
                    cases[i].fmt);
            }
        }

        /* Float words, spaces, and TOINT */
        databox box = {};
        if (StrScanScan((const uint8_t *)"-inf", &box, 0, true, false) !=
                STRSCAN_NUM ||
            box.data.d64 != -INFINITY ||
            StrScanScan((const uint8_t *)"Infinity", &box, 0, true, false) !=
                STRSCAN_NUM ||
            box.data.d64 != INFINITY ||
            StrScanScan((const uint8_t *)"  NaN  ", &box, 0, true, true) !=
                STRSCAN_NUM ||
            !isnan(box.data.d64) ||
            StrScanScan((const uint8_t *)"infx", &box, 0, true, false) !=
                STRSCAN_ERROR ||
            StrScanScan((const uint8_t *)" 12 ", &box, 0, false, false) !=
                STRSCAN_ERROR ||
            StrScanScan((const uint8_t *)" 1.5e1 ", &box, 0, false, true) !=
                STRSCAN_NUM ||
            box.data.d64 != 15.0 ||
            StrScanScan((const uint8_t *)"3.0", &box, STRSCAN_OPT_TOINT, false,
                        false) != STRSCAN_INT ||
            box.data.i64 != 3 ||
            StrScanScan((const uint8_t *)"3.5", &box, STRSCAN_OPT_TOINT, false,
                        false) != STRSCAN_NUM) {
            ERR("StrScanScan float words, spaces or TOINT");
        }
    }

    TEST("StrAtoF and StrAtoi64 read UTF-16 as they read UTF-8") {
        static const char *nums[] = {"1e5",
                                     "-2.5E-3",
                                     "123",
                                     "  7 ",
                                     "-0",
                                     "4x",
                                     "1e310",
                                     "",
                                     "92233720368547758070",
                                     "-9223372036854775808"};
        for (size_t i = 0; i < countof(nums); i++) {
            const size_t n = strlen(nums[i]);
            uint8_t le[64] = {};
            uint8_t be[64] = {};
            for (size_t k = 0; k < n; k++) {
                le[2 * k] = (uint8_t)nums[i][k];
                be[2 * k + 1] = (uint8_t)nums[i][k];
            }
            const int32_t len16 = (int32_t)(2 * n);
            double d8 = 0;
            double dle = 0;
            double dbe = 0;
            const bool f8 = StrAtoF(nums[i], &d8, (int32_t)n, STR_UTF8, true);
            const bool fle = StrAtoF(le, &dle, len16, STR_UTF16LE, true);
            const bool fbe = StrAtoF(be, &dbe, len16, STR_UTF16BE, true);
            int64_t i8 = 0;
            int64_t ile = 0;
            int64_t ibe = 0;
            const int32_t r8 =
                StrAtoi64(nums[i], &i8, (int32_t)n, STR_UTF8, true);
            const int32_t rle = StrAtoi64(le, &ile, len16, STR_UTF16LE, true);
            const int32_t rbe = StrAtoi64(be, &ibe, len16, STR_UTF16BE, true);
            if (f8 != fle || f8 != fbe || memcmp(&d8, &dle, sizeof(d8)) != 0 ||
                memcmp(&d8, &dbe, sizeof(d8)) != 0 || r8 != rle || r8 != rbe ||
                i8 != ile || i8 != ibe) {
                ERR("UTF-16 read of \"%s\" differs: atof %d/%d/%d, atoi "
                    "%" PRId32 "/%" PRId32 "/%" PRId32,
                    nums[i], f8, fle, fbe, r8, rle, rbe);
            }
        }
        /* A unit above 0xFF is not part of a number */
        const uint8_t wide[] = {'1', 0, '2', 0, 0x31, 0x01};
        double dw = 0;
        int64_t iw = 0;
        if (StrAtoF(wide, &dw, 6, STR_UTF16LE, true) || dw != 12.0 ||
            StrAtoi64(wide, &iw, 6, STR_UTF16LE, true) != 1 || iw != 12) {
            ERR("UTF-16 non-Latin unit read as part of a number");
        }
    }

    TEST("sqlite integer and hex text helpers") {
        static const struct {
            const char *s;
            int32_t rc;
            int64_t want;
        } h64[] = {
            {"0x10", 0, 16},
            {"0xffffffffffffffff", 0, -1},
            {"0x1ffffffffffffffff", 1, 0},
            {"0x0000000000000000001", 0, 1},
            {"0x1g", 1, 0},
            {"123", 0, 123},
            {"-45", 0, -45},
            {"9223372036854775808", 2, INT64_MAX},
            {"12a", 1, 12},
        };
        for (size_t i = 0; i < countof(h64); i++) {
            int64_t got = 0;
            const int32_t rc = StrDecOrHexToInt64(h64[i].s, &got);
            if (rc != h64[i].rc || (rc != 1 && got != h64[i].want) ||
                (h64[i].rc == 1 && h64[i].want && got != h64[i].want)) {
                ERR("StrDecOrHexToInt64(%s) = %" PRId32 ", %" PRId64, h64[i].s,
                    rc, got);
            }
        }

        static const struct {
            const char *s;
            bool ok;
            int32_t want;
        } i32[] = {
            {"2147483647", true, INT32_MAX},
            {"2147483648", false, 0},
            {"-2147483648", true, INT32_MIN},
            {"-2147483649", false, 0},
            {"+12", true, 12},
            {"0x7fffffff", true, INT32_MAX},
            {"0x80000000", false, 0},
            {"0x123456789", false, 0},
            {"0x00000000ff", true, 255},
            {"00000000042abc", true, 42},
            {"12345678901", false, 0},
        };
        for (size_t i = 0; i < countof(i32); i++) {
            int32_t got = -1;
            const bool ok = StrGetInt32(i32[i].s, &got);
            if (ok != i32[i].ok || (ok && got != i32[i].want) ||
                StrAtoi(i32[i].s) != (ok ? i32[i].want : 0)) {
                ERR("StrGetInt32(%s) = %d, %" PRId32, i32[i].s, ok, got);
            }
        }
        if (StrAtoi(nullptr) != 0 || StrAtoi("-17") != -17) {
            ERR("StrAtoi");
        }

        static constexpr char digits[] = "0123456789abcdefABCDEF";
        for (size_t i = 0; digits[i]; i++) {
            const char one[2] = {digits[i], 0};
            if (StrHexToInt(digits[i]) != strtol(one, nullptr, 16)) {
                ERR("StrHexToInt(%c)", digits[i]);
            }
        }

        cleanerName(zfree, Chars) char *blob = StrHexToBlob("00ff7A", 6);
        cleanerName(zfree, Chars) char *odd = StrHexToBlob("abc", 3);
        cleanerName(zfree, Chars) char *none = StrHexToBlob("", 0);
        if (memcmp(blob, "\x00\xff\x7a", 4) != 0 ||
            memcmp(odd, "\xab", 2) != 0 || none[0] != 0) {
            ERR("StrHexToBlob");
        }

        if (StrAbsInt32(5) != 5 || StrAbsInt32(-5) != 5 ||
            StrAbsInt32(0) != 0 || StrAbsInt32(INT32_MIN) != INT32_MAX) {
            ERR("StrAbsInt32");
        }

        static const struct {
            const char *in;
            const char *out;
        } dq[] = {
            {"'a''b'", "a'b"},  {"\"x\"", "x"},
            {"`y`", "y"},       {"[a-b-c]", "a-b-c"},
            {"plain", "plain"}, {"'unterminated", "unterminated"},
            {"''", ""},         {"'", ""},
            {"'a'tail", "a"},
        };
        for (size_t i = 0; i < countof(dq); i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%s", dq[i].in);
            StrDequote(buf);
            if (strcmp(buf, dq[i].out) != 0) {
                ERR("StrDequote(%s) = %s, want %s", dq[i].in, buf, dq[i].out);
            }
        }
        StrDequote(nullptr);

        if (StrNICmp(nullptr, nullptr, 3) != 0 ||
            StrNICmp(nullptr, "a", 3) >= 0 || StrNICmp("a", nullptr, 3) <= 0 ||
            StrNICmp("abcX", "ABCY", 3) != 0 ||
            StrNICmp("abcX", "ABCY", 4) >= 0 || StrICmp("ABC", "abd") >= 0 ||
            StrICmp("abc", "ABC") != 0 || StrICmp("abc", "ab") <= 0) {
            ERR("StrICmp / StrNICmp");
        }
    }

    TEST("StrAddInt64 / StrSubInt64 / StrMulInt64 detect overflow exactly") {
        static const struct {
            char op; /* '+', '-', '*' */
            int64_t a;
            int64_t b;
            bool ok;
            int64_t want;
        } cases[] = {
            {'+', INT64_MAX, 1, false, 0},
            {'+', INT64_MAX - 1, 1, true, INT64_MAX},
            {'+', INT64_MIN, -1, false, 0},
            {'+', INT64_MIN, INT64_MAX, true, -1},
            {'-', INT64_MIN, 1, false, 0},
            {'-', 0, INT64_MIN, false, 0},
            {'-', -1, INT64_MIN, true, INT64_MAX},
            {'*', 3, 4, true, 12},
            {'*', -3, 5, true, -15},
            {'*', INT64_C(1) << 32, INT64_C(1) << 30, true, INT64_C(1) << 62},
            {'*', INT64_C(3037000499), INT64_C(3037000499), true,
             INT64_C(9223372030926249001)},
            {'*', INT64_C(3037000500), INT64_C(3037000500), false, 0},
            {'*', INT64_MAX, 2, false, 0},
            {'*', INT64_MIN, -1, false, 0},
            {'*', INT64_MIN, 1, true, INT64_MIN},
            {'*', INT64_C(1) << 62, -2, true, INT64_MIN},
        };

        for (size_t i = 0; i < countof(cases); i++) {
            int64_t v = cases[i].a;
            bool ok;
            if (cases[i].op == '+') {
                ok = StrAddInt64(&v, cases[i].b);
            } else if (cases[i].op == '-') {
                ok = StrSubInt64(&v, cases[i].b);
            } else {
                ok = StrMulInt64(&v, cases[i].b);
            }

            const int64_t want = cases[i].ok ? cases[i].want : cases[i].a;
            if (ok != cases[i].ok || v != want) {
                ERR("%" PRId64 " %c %" PRId64 ": ok=%d value=%" PRId64
                    ", want ok=%d value=%" PRId64,
                    cases[i].a, cases[i].op, cases[i].b, ok, v, cases[i].ok,
                    want);
            }
        }
    }

#if 1
    TEST("verify empty string doesn't convert to zero") {
        databox got;
        assert(StrScanScanReliable("", 0, &got) == false);
    }

    TEST("non-canonical integer spellings do not convert") {
        databox got;
        assert(StrScanScanReliable("-0", 2, &got) == false);
        assert(StrScanScanReliable("-01", 3, &got) == false);
        assert(StrScanScanReliable("-007", 4, &got) == false);
        assert(StrScanScanReliable("01", 2, &got) == false);
        assert(StrScanScanReliable("-1", 2, &got) == true);
        assert(got.type == DATABOX_SIGNED_64 && got.data.i == -1);
        assert(StrScanScanReliable("-0.5", 4, &got) == true);

        const char *padded = "00000000000000000001";
        databox sm;
        databoxBig smb;
        databoxBig *final;
        assert(!StrScanScanReliableConvert128_(padded, strlen(padded), &sm,
                                               &smb, &final, true));
        const char *negPadded = "-0000000000000000001";
        assert(!StrScanScanReliableConvert128_(negPadded, strlen(negPadded),
                                               &sm, &smb, &final, true));
    }

    TEST("128 smallest") {
        const char *smallest = "-170141183460469231731687303715884105728";
        const size_t slen = strlen(smallest);
        databox sm;
        databoxBig smb;
        databoxBig *final;
        const bool convertedSmallest = StrScanScanReliableConvert128_(
            smallest, slen, &sm, &smb, &final, true);
        assert(convertedSmallest);
        assert(*final->data.i128 == INT128_MIN);
    }

    TEST("128 biggest") {
        const char *smallest = "340282366920938463463374607431768211455";
        const size_t slen = strlen(smallest);
        databox sm;
        databoxBig smb;
        databoxBig *final;
        const bool convertedSmallest = StrScanScanReliableConvert128_(
            smallest, slen, &sm, &smb, &final, true);
        assert(convertedSmallest);
        assert(*final->data.u128 == (__uint128_t)-1);
    }

    TEST("Small integer convert") {
        const size_t loopers = 10'000'000;
        const char *numeristr = "1234567891234567";
        [[maybe_unused]] uint64_t result = 0;

        TIME_INIT;
        for (size_t i = 0; i < loopers; i++) {
            for (size_t numericLength = 1; numericLength < 16;
                 numericLength++) {
                result = 0;
                if (0) { // numericLength == 1) {
                    if (numeristr[0] >= '0' && numeristr[0] <= '9') {
                        /* Fast path for lengths <= 9 */
                        result = numeristr[0] - '0';
                    } else {
                        /* else, not a number! */
                        result = -1;
                    }
                } else {
                    /* TODO: for reasonable lengths (< 10 digits) we could
                     *       just do an inline while loop to consume digits too
                     */
#if 0
                    if (!StrBufToUInt64(numeristr, numericLength, &result)) {
                        result = -1;
                    }
#else
#if 1
#if 0
                    if (numericLength <= 15) {
                        result = StrBufToUInt64Fast(numeristr, numericLength);
                    } else {
                        result = -1;
                    }
#else
                    result = StrBufToUInt64Fast(numeristr, numericLength);
#endif
#else
                    if (!StrBufToUInt64FastCheckNumeric(
                            numeristr, numericLength, &result)) {
                        result = -1;
                    }
#endif
#endif
                }
            }
        }

        TIME_FINISH(loopers * 15, "Byte Lengths Type A");
    }

    TEST("Medium integer convert") {
        const size_t loopers = 10'000'000;
        const char *numeristr = "1234567891234567";
        [[maybe_unused]] uint64_t result = 0;

        TIME_INIT;
        for (size_t i = 0; i < loopers; i++) {
            for (size_t numericLength = 1; numericLength < 16;
                 numericLength++) {
                result = 0;
                if (numericLength > 15 ||
                    !StrIsDigitsFast(numeristr, numericLength)) {
                    result = -1;
                }

                result = StrBufToUInt64Fast(numeristr, numericLength);
            }
        }

        TIME_FINISH(loopers * 15, "Byte Lengths Type B");
    }

    TEST("Speeds of IsDigits") {
        uint8_t buf[] = "798542789413789432789437209583490854903859823748912748"
                        "923784329543809543798547389572309842309742398753489754"
                        "38975438950934859043750894350934785943798543";

        for (size_t offset = 0; offset < 1 /* 8 */; offset++) {
            size_t printBooster = 1'000'000;
            {
                TIME_INIT;
                for (size_t i = 0; i < printBooster; i++) {
                    assert(StrIsDigitsFast(buf + offset,
                                           strlen((char *)buf) - offset));
                }
                TIME_FINISH(printBooster, "StrIsDigitsFast");
            }
            {
                TIME_INIT;
                for (size_t i = 0; i < printBooster; i++) {
                    assert(StrIsDigitsIndividual(buf + offset,
                                                 strlen((char *)buf) - offset));
                }
                TIME_FINISH(printBooster, "StrIsDigitsIndividual");
            }
        }
    }

    TEST("Speed of __uint128_t printing") {
        uint8_t buf[40];
        size_t printBooster = 1'000'000;
#if 0
        const __uint128_t bigThing =
            (((__uint128_t)54210108624275221ULL) << 64) |
            12919594847110692864ULL;
#else
        /* (2^128) - 12 */
        const __uint128_t bigThing =
            (((__uint128_t)18446744073709551615ULL) << 64) |
            18446744073709551604ULL;
#endif
        TIME_INIT;
        for (size_t i = 0; i < printBooster; i++) {
            StrUInt128ToBuf(&buf, sizeof(buf), bigThing);
        }
        TIME_FINISH(printBooster, "StrUInt128ToBuf");
    }

    TEST("Speed of uint64_t printing") {
        uint8_t buf[40];
        size_t printBooster = 100'000;
        const uint64_t bigThing = UINT64_MAX / 64;
        TIME_INIT;
        for (size_t i = 0; i < printBooster; i++) {
            /* Currently reports ~60 cycles per conversion: */
            StrUInt64ToBuf(&buf, sizeof(buf), bigThing);
        }
        TIME_FINISH(printBooster, "StrUIntToBuf");
    }

    TEST("Reliable borderline number parsing not too big?") {
        /* This test checks a borderline case for float round-tripping.
         * On x86 with 80-bit long double, "7074451188.598104" parses to
         * 7074451188.5981045 (ONE BIT HIGHER), so round-trip fails.
         *
         * On ARM where long double == double (64-bit), the behavior differs
         * and the round-trip may succeed.
         *
         * Fun fact: 7074451188.5981035 == 7074451188.598104 as well,
         *           so good luck round tripping data (also another reason
         *           we're doing string double conversion checks even
         *           though they are slower and may require in-line
         *           bignum allocs */
        databox got = {};
        bool scanResult = StrScanScanReliable("7074451188.598104", 17, &got);

#if DK_HAS_FLOAT_EXTENDED && DK_FLOAT_EXTENDED_MANTISSA_BITS >= 113
        /* binary128 scales without a second rounding: the nearest double,
         * which prints back as the input */
        if (!scanResult || got.type != DATABOX_DOUBLE_64 ||
            got.data.d64 != 7074451188.598104) {
            ERR("binary128 scan of 7074451188.598104: %d, %.17g", scanResult,
                got.data.d64);
        }
#elif DK_HAS_FLOAT_EXTENDED
        /* x86's 64-bit significand rounds twice and lands on the neighbor
         * double, which prints differently: not a reliable conversion */
        assert(scanResult == false);
#else
        /* Without extended precision, we have less strict expectations.
         * Report on what actually happened for diagnostic purposes. */
        const double expected = 7074451188.598104;
        if (scanResult) {
            /* Conversion claimed success - verify the value is close */
            if (got.type == DATABOX_DOUBLE_64) {
                const double delta = got.data.d64 - expected;
                const double relError = (delta < 0 ? -delta : delta) /
                                        (expected < 0 ? -expected : expected);
                /* Allow up to 1e-14 relative error (double precision limit) */
                if (relError > 1e-14) {
                    ERR("No float128: got %.*g, expected %.*g, rel error %.2e",
                        17, got.data.d64, 17, expected, relError);
                }
            }
        }
        /* Note: Without extended precision, it's acceptable for this to
         * either succeed (with limited precision) or fail. */
#endif
    }

    TEST("Reliable number parsing simple 0?") {
        databox result = {};
        bool worked = StrScanScanReliable("0", 1, &result);
        assert(worked);

        if (result.type != DATABOX_SIGNED_64) {
            databoxReprSay("Expected SIGNED, but got", &result);
            assert(false && "Fix me!");
        }

        if (result.data.i != 0) {
            ERR("Expected %d but got %d instead!", 0, result.data.i32);
        }
    }

    TEST("Reliable number parsing simple 1?") {
        databox result = {};
        bool worked = StrScanScanReliable("1", 1, &result);
        assert(worked);

        if (result.type != DATABOX_SIGNED_64) {
            databoxReprSay("Expected SIGNED, but got", &result);
            assert(false && "Fix me!");
        }

        if (result.data.i != 1) {
            ERR("Expected %d but got %d instead!", 1, result.data.i32);
        }
    }

    TEST("Reliable number parsing simple -1?") {
        databox result = {};
        bool worked = StrScanScanReliable("-1", 2, &result);
        assert(worked);

        if (result.type != DATABOX_SIGNED_64) {
            databoxReprSay("Expected SIGNED, but got", &result);
            assert(false && "Fix me!");
        }

        if (result.data.i != -1) {
            ERR("Expected %d but got %d instead!", -1, result.data.i32);
        }
    }

#if 0
    TEST("Reliable number parsing simple 0 to 999999999?") {
        for (size_t i = 0; i < 999999999; i++) {
            if (i % 100'000 == 0) {
                printf(".");
                fflush(stdout);
            }

            char buf[21] = {};
            StrUInt64ToBuf(buf, sizeof(buf), i);

            databox result = {};
            bool worked = StrScanScanReliable(buf, strlen(buf), &result);
            assert(worked);

            if (result.type != DATABOX_SIGNED_64) {
                databoxReprSay("Expected SIGNED, but got", &result);
                assert(false && "Fix me!");
            }

            if (result.data.u != i) {
                ERR("Expected %d but got %d instead!", i, result.data.i32);
            }
        }
    }
#endif

    TEST("Reliable number parsing simple?") {
        databox result = {};
        bool worked = StrScanScanReliable("299.5", 5, &result);
        assert(worked);

        if (result.type != DATABOX_FLOAT_32) {
            databoxReprSay("Expected FLOAT32, but got", &result);
            assert(false && "Fix me!");
        }

        if (result.data.f32 != (float)299.5) {
            ERR("Expected %f but got %f instead!", 299.5, result.data.f32);
        }
    }

    TEST("Reliable number parsing simple end in zero?") {
        databox result = {};
        bool worked = StrScanScanReliable("299.0", 5, &result);
        assert(worked);

        if (result.type != DATABOX_FLOAT_32) {
            databoxReprSay("Expected FLOAT32, but got", &result);
            assert(false && "Fix me!");
        }

        /* We do an extra string conversion test here because
         * 299 == 299.0 but we want to make sure we'll print
         * the 299.0 in the future since we started as a float. */
        char buf[64];
        size_t len = StrDoubleFormatToBufNice(buf, 64, result.data.f32);
        assert(!strncmp(buf, "299.0", len));

        if (result.data.f32 != (float)299.0) {
            ERR("Expected %f but got %f instead!", 299.0, result.data.f32);
        }
    }

    TEST("Reliable number parsing simple trailing zeroes fails?") {
        databox result = {};
        bool worked = StrScanScanReliable("299.5000", 8, &result);
        if (worked) {
            databoxReprSay("Expected failure, but got", &result);
        }
    }

    TEST("Reliable number parsing too long decimal fails?") {
        databox result = {};
        bool worked = StrScanScanReliable("299.500010101010101", 18, &result);
        if (worked) {
            databoxReprSay("Expected failure, but got", &result);
        }
    }

    TEST("Reliable number parsing too long decimal fails II?") {
        databox result = {};
        bool worked =
            StrScanScanReliable("2.22222222222222222222222222", 28, &result);
        if (worked) {
            databoxReprSay("Expected failure, but got", &result);
        }
    }

#if 0
    TEST("Reliable number parsing double extremes?") {
        databox resultMin = {};
        databox resultMax = {};
        const char *min = "0.000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000022250738585072014";

        const char *max = "179769313486231570000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000.0";

        bool worked = StrScanScanReliable(min, strlen(min), &resultMin);
        if (!worked) {
            databoxReprSay("Expected success, but got", &resultMin);
            assert(false);
        }

        worked = StrScanScanReliable(max, strlen(max), &resultMax);
        if (!worked) {
            databoxReprSay("Expected success, but got", &resultMax);
            assert(false);
        }
    }
#endif

    TEST("Reliable number parsing bigger real ok?") {
        databox result = {};
        bool worked = StrScanScanReliable("21542136560502.848", 18, &result);
        if (!worked) {
            databoxReprSay("Expected success, but got", &result);
            assert(false);
        }

        if (result.data.d64 != 21542136560502.847656) {
            ERR("Expected %f but got %f!", 21542136560502.848, result.data.d64);
        }
    }

    TEST("Reliable number parsing bigger real ok III?") {
        databox result = {};
        bool worked = StrScanScanReliable("0.4", 3, &result);
        if (!worked) {
            databoxReprSay("Expected success, but got", &result);
            assert(false);
        }

        char restoredBuf[64] = {};
        StrDoubleFormatToBufNice(restoredBuf, sizeof(restoredBuf), 0.4);

        assert(strlen("0.4") == strlen(restoredBuf));

        if (result.data.d64 != 0.4) {
            ERR("Expected %f but got %f!", 0.4, result.data.d64);
        }
    }

    TEST("Reliable number parsing bigger real ok IV?") {
        databox result = {};
        bool worked = StrScanScanReliable("0.456789", 8, &result);
        if (!worked) {
            databoxReprSay("Expected success, but got", &result);
            assert(false);
        }

        if (result.data.d64 != 0.456789) {
            ERR("Expected %f but got %f!", 0.456789, result.data.d64);
        }
    }
    TEST("Reliable number parsing bigger real ok II?") {
        databox result = {};
        bool worked = StrScanScanReliable("9543769205953.803", 17, &result);
        const double expected = 9543769205953.803;
#if DK_HAS_FLOAT_EXTENDED
        const double expectedConverted =
            9543769205953.802734; /* actual double value */
        /* With extended precision, conversion round-trips reliably */
        if (!worked) {
            databoxReprSay("Expected success, but got", &result);
            assert(false);
        }

        if (result.data.d64 != expectedConverted) {
            ERR("Expected %f but got %f!", expected, result.data.d64);
        }
#else
        /* Without extended precision, report on what happened.
         * The conversion may succeed or fail depending on precision. */
        if (worked) {
            if (result.type == DATABOX_DOUBLE_64) {
                const double delta = result.data.d64 - expected;
                const double relError = (delta < 0 ? -delta : delta) /
                                        (expected < 0 ? -expected : expected);
                /* With double precision only, we expect relative error up to
                 * ~1e-15 */
                if (relError > 1e-12) {
                    ERR("No float128: got %.*g, expected %.*g, rel error %.2e",
                        17, result.data.d64, 17, expected, relError);
                }
            }
        } else {
            /* Without extended precision, it's acceptable for the conversion
             * to fail (round-trip unreliable) for borderline cases. */
        }
#endif
    }

    TEST("Reliable number parsing bigger real not ok?") {
        databox result = {};
        bool worked = StrScanScanReliable("9543769205953.8029999999999999734",
                                          34, &result);
        if (worked) {
            databoxReprSay("Expected failure, but got", &result);
            assert(false);
        }
    }

    TEST("Reliable number parsing smaller leading zeroes not ok?") {
        databox result = {};
        bool worked = StrScanScanReliable("03", 2, &result);
        if (worked) {
            databoxReprSay("Expected failure, but got", &result);
            assert(false);
        }
    }

    TEST("Reliable number parsing bigger leading zeroes not ok?") {
        databox result = {};
        bool worked = StrScanScanReliable("0009543769205953", 16, &result);
        if (worked) {
            databoxReprSay("Expected failure, but got", &result);
            assert(false);
        }
    }

    TEST("Reliable number parsing bigger leading dot not ok?") {
        databox result = {};
        bool worked = StrScanScanReliable(".0009543769205953", 17, &result);
        if (worked) {
            databoxReprSay("Expected failure, but got", &result);
            assert(false);
        }
    }

    TEST("Reliable number parsing bigger integer fails?") {
        databox result = {};
        bool worked = StrScanScanReliable("24032321013100332443", 20, &result);
        if (worked) {
            databoxReprSay("Expected failure, but got", &result);
            assert(false);
        }
    }

    TEST("Reliable number parsing bigger float fails?") {
        databox result = {};
        bool worked = StrScanScanReliable("240323210131003243.3", 20, &result);
        if (worked) {
            databoxReprSay("Expected failure, but got", &result);
            assert(false);
        }
    }

    TEST("Reliable number parsing a dash fails?") {
        databox result = {};
        bool worked = StrScanScanReliable("-", 1, &result);
        if (worked) {
            databoxReprSay("Expected failure, but got", &result);
            assert(false);
        }
    }

    TEST("Reliable number parsing a dot fails?") {
        databox result = {};
        bool worked = StrScanScanReliable(".", 1, &result);
        if (worked) {
            databoxReprSay("Expected failure, but got", &result);
            assert(false);
        }
    }

    TEST("Reliable number parsing integer?") {
        databox result = {};
        bool worked = StrScanScanReliable("9223372036854775808", 19, &result);
        if (!worked) {
            databoxReprSay("Expected success, but got", &result);
            assert(false);
        }
        /* A positive above INT64_MAX reads as UNSIGNED_64. */
        assert(result.type == DATABOX_UNSIGNED_64);
        assert(result.data.u == (uint64_t)INT64_MAX + 1);
    }

    TEST("Reliable number parsing integer edges, by value?") {
        /* Canonical spellings at and just past the edges of
         * [INT64_MIN, UINT64_MAX], each with its expected reading. */
        const struct {
            const char *s;
            bool ok;
            databoxType type;
            int64_t i;
            uint64_t u;
        } rows[] = {
            {"-9223372036854775808", true, DATABOX_SIGNED_64, INT64_MIN, 0},
            {"-9223372036854775807", true, DATABOX_SIGNED_64, -INT64_MAX, 0},
            {"-1000000000000000000", true, DATABOX_SIGNED_64,
             INT64_C(-1'000'000'000'000'000'000), 0},
            {"-999999999999999999", true, DATABOX_SIGNED_64,
             INT64_C(-999999999999999999), 0},
            {"9223372036854775807", true, DATABOX_SIGNED_64, INT64_MAX, 0},
            {"1000000000000000000", true, DATABOX_SIGNED_64,
             INT64_C(1'000'000'000'000'000'000), 0},
            {"9999999999999999999", true, DATABOX_UNSIGNED_64, 0,
             UINT64_C(9999999999999999999)},
            {"18446744073709551615", true, DATABOX_UNSIGNED_64, 0, UINT64_MAX},
            {"-9223372036854775809", false, DATABOX_VOID, 0, 0},
            {"-10000000000000000000", false, DATABOX_VOID, 0, 0},
            {"18446744073709551616", false, DATABOX_VOID, 0, 0},
        };
        for (size_t i = 0; i < countof(rows); i++) {
            databox result = {};
            const bool worked =
                StrScanScanReliable(rows[i].s, strlen(rows[i].s), &result);
            if (worked != rows[i].ok) {
                ERR("'%s': expected %s, got %s", rows[i].s,
                    rows[i].ok ? "a number" : "no number",
                    worked ? "a number" : "no number");
                continue;
            }
            if (!worked) {
                continue;
            }
            if (result.type != rows[i].type) {
                databoxReprSay("Unexpected type for", &result);
                ERR("'%s': wrong type", rows[i].s);
                continue;
            }
            if (rows[i].type == DATABOX_SIGNED_64 &&
                result.data.i != rows[i].i) {
                ERR("'%s': expected %" PRId64 ", got %" PRId64, rows[i].s,
                    rows[i].i, (int64_t)result.data.i);
            }
            if (rows[i].type == DATABOX_UNSIGNED_64 &&
                result.data.u != rows[i].u) {
                ERR("'%s': expected %" PRIu64 ", got %" PRIu64, rows[i].s,
                    rows[i].u, (uint64_t)result.data.u);
            }
        }
    }

    TEST("Reliable number parsing biggest integer?") {
        databox result = {};
        bool worked = StrScanScanReliable("18446744073709551615", 20, &result);
        if (!worked) {
            databoxReprSay("Expected success, but got", &result);
            assert(false);
        }

        assert(result.data.u == UINT64_MAX);
    }

    TEST("Reliable number parsing too big integer signed failed?") {
        databox result = {};
        bool worked = StrScanScanReliable("-18446744073709551615", 21, &result);
        if (worked) {
            databoxReprSay("Expected failure, but got", &result);
            assert(false);
        }
    }

    TEST("Reliable number parsing biggerest integer fails?") {
        databox result = {};
        bool worked = StrScanScanReliable("18446744073709551616", 20, &result);
        if (worked) {
            databoxReprSay("Expected success, but got", &result);
            assert(false);
        }
    }

    TEST("Reliable number parsing biggerest integer fails II?") {
        char bad[] = "18446744073709551615";
        /* For each character in 'bad', replace with 9 then
         * try to see if the convert can be tricked into returning
         * a bad value for an over-sized unsigned integer */
        for (size_t i = 0; i < strlen(bad); i++) {
            char useBad[sizeof(bad)];
            memcpy(useBad, bad, sizeof(bad));
            if (useBad[i] == '9') {
                continue;
            }

            useBad[i] = '9';

            databox result = {};
            bool worked = StrScanScanReliable(useBad, strlen(useBad), &result);
            if (worked) {
                databoxReprSay("Expected failure, but got", &result);
                assert(false);
            }
        }
    }

    TEST("Reliable number parsing biggerest integer fails III?") {
        databox result;
        /* This is larger than 2^64, so the conversion MUST fail our
         * reliable convert. */
        bool worked = StrScanScanReliable("33100300424244333022", 20, &result);
        if (worked) {
            databoxReprSay("Expected failure, but got", &result);
            assert(false);
        }
    }

    TEST("Reliable number parsing biggerest integer works III?") {
        databox result;
        bool worked = StrScanScanReliable("2411321300310020112", 19, &result);
        if (!worked) {
            databoxReprSay("Expected success, but got", &result);
            assert(false);
        }

        if (result.type != DATABOX_SIGNED_64) {
            databoxReprSay("Expected SIGNED, but got", &result);
            assert(false && "Fix me!");
        }

        if (result.data.i != 2411321300310020112LL) {
            ERR("Expected %" PRId64 ", but got %" PRId64, 2411321300310020112LL,
                (int64_t)result.data.i);
        }
    }

    TEST("Perf Number Conversions") {
        /* Smoke size: every string parses; perf builds time larger runs */
        const size_t loopBooster = 100'000;
        const size_t looperDeDoBooster = 2;

        char *str[] = {"454545454545.5",       "9223372036854775808",
                       "-9223372036854775809", "2147483648",
                       "2147483649",           "1.234",
                       "789498543789543.13",   "21542136560502.847656"};

        size_t len[countof(str)] = {};
        for (size_t i = 0; i < countof(str); i++) {
            len[i] = strlen(str[i]);
        }

        {
            for (size_t j = 0; j < looperDeDoBooster; j++) {
                TIME_INIT;
                databox result = {};
                for (size_t i = 0; i < loopBooster; i++) {
                    PERF_TIMERS_STAT_START;
                    const size_t useStr = i % countof(str);
                    if (StrScanScanReliable(str[useStr], len[useStr],
                                            &result)) {
                        assert(result.type);
                    }
                    PERF_TIMERS_STAT_STOP(i);
                }

                if (j > 2) {
                    TIME_FINISH(loopBooster, "Reliable");
                }
            }
        }

        {
            for (size_t j = 0; j < looperDeDoBooster; j++) {
                TIME_INIT;
                for (size_t i = 0; i < loopBooster; i++) {
                    PERF_TIMERS_STAT_START;
                    int64_t ivalue = 0;
                    double dvalue = 0;
                    const size_t useStr = i % countof(str);
                    if (len[useStr] <= 32 &&
                        StrAtoi64(str[useStr], &ivalue, len[useStr], STR_UTF8,
                                  false) == 0) {
                        assert(ivalue > 0);
                    } else if (len[useStr] <= 19 &&
                               StrAtoF(str[useStr], &dvalue, len[useStr],
                                       STR_UTF8, false)) {
                        assert(dvalue > 0);
                    }
                    PERF_TIMERS_STAT_STOP(i);
                }

                if (j > 2) {
                    TIME_FINISH(loopBooster, "Individual");
                }
            }
        }
    }

    for (size_t boost = 0; boost < 4; boost++) {
        size_t loopBooster = 0;

        switch (boost) {
        case 0:
            loopBooster = 1;
            break;
        case 1:
            loopBooster = 50;
            break;
        case 2:
            loopBooster = 1e5;
            break;
        case 3:
            loopBooster = 1e6;
            break;
        default:
            assert(false && "Need case!");
        }

#if 0
        /* newline a few hundred chars in */
        char *newlineProcessStr =
            "Let's present him to the duke, like a Roman"
            "conqueror; and it would do well to set the deer's"
            "horns upon his head, for a branch of victory. Have"
            "you no song, forester, for this purpose?\n";
#else
        /* newline ~40 chars in */
        char *newlineProcessStr =
            "Let's present him to the duke, like a Roman\n"
            "conqueror; and it would do well to set the deer's\n"
            "horns upon his head, for a branch of victory. Have\n"
            "you no song, forester, for this purpose?\n";
#endif
        const size_t origLen = strlen(newlineProcessStr);
        size_t actuallyReadLen = 0;

#ifdef __AVX2__
        TEST_DESC("AVX string detect newline at looping %zu", loopBooster) {
            TIME_INIT;
            const __m256i spaces = _mm256_set1_epi8('\n');
            for (size_t i = 0; i < loopBooster; i++) {
                PERF_TIMERS_STAT_START;
                char *b = newlineProcessStr;
                size_t readLen = origLen;
                while (readLen >= (ssize_t)CHECK_NEWLINE_STEP_SIZE_AVX) {
                    const __m256i x =
                        _mm256_loadu_si256((CHECK_NEWLINE_STEP_AVX *)(b));
                    const __m256i xspaces = _mm256_cmpeq_epi8(x, spaces);

                    readLen -= CHECK_NEWLINE_STEP_SIZE_AVX;
                    b += CHECK_NEWLINE_STEP_SIZE_AVX;

                    if (_mm_popcnt_u32(_mm256_movemask_epi8(xspaces))) {
                        break;
                    }
                }
                actuallyReadLen = (origLen)-readLen;
                PERF_TIMERS_STAT_STOP(i);
            }
            TIME_FINISH(loopBooster, "AVX find newline");
            PERF_TIMERS_RESULT_PRINT_BYTES(loopBooster, actuallyReadLen);
        }
#endif

        TEST_DESC("word-size string detect newline at loopBooster %zu",
                  loopBooster) {
            TIME_INIT;
            for (size_t i = 0; i < loopBooster; i++) {
                PERF_TIMERS_STAT_START;
                char *b = newlineProcessStr;
                size_t readLen = origLen;
                /* Attempt to process in 8-byte steps */
                while (readLen >= (ssize_t)CHECK_NEWLINE_STEP_SIZE) {
                    readLen -= CHECK_NEWLINE_STEP_SIZE;
                    b += CHECK_NEWLINE_STEP_SIZE;

                    if (hasnewline(*(CHECK_NEWLINE_STEP *)b)) {
                        break;
                    }
                }
                actuallyReadLen = (origLen)-readLen;
                PERF_TIMERS_STAT_STOP(i);
            }
            TIME_FINISH(loopBooster, "word find newline");
            PERF_TIMERS_RESULT_PRINT_BYTES(loopBooster, actuallyReadLen);
        }

        TEST_DESC("byte-by-byte string detect newline at loopBooster %zu",
                  loopBooster) {
            TIME_INIT;
            for (size_t i = 0; i < loopBooster; i++) {
                PERF_TIMERS_STAT_START;
                char *b = newlineProcessStr;
                size_t readLen = origLen;
                /* Process individual remainder bytes */
                while (readLen) {
                    readLen--;

                    if (*b++ == NL) {
                        break;
                    }
                }
                actuallyReadLen = (origLen)-readLen;
                PERF_TIMERS_STAT_STOP(i);
            }
            TIME_FINISH(loopBooster, "byte-by-byte find newline");
            PERF_TIMERS_RESULT_PRINT_BYTES(loopBooster, actuallyReadLen);
        }

        TEST_DESC("byte-by-byte no math at loopBooster %zu", loopBooster) {
            TIME_INIT;
            for (size_t i = 0; i < loopBooster; i++) {
                PERF_TIMERS_STAT_START;
                char *b = newlineProcessStr;
                size_t readLen = origLen;
                while (*b++ != NL) {
                    readLen--;
                }
                actuallyReadLen = (origLen)-readLen;
                PERF_TIMERS_STAT_STOP(i);
            }
            TIME_FINISH(loopBooster, "byte-by-byte find newline");
            PERF_TIMERS_RESULT_PRINT_BYTES(loopBooster, actuallyReadLen);
        }

        TEST_DESC("memmem detect newline at loopBooster %zu", loopBooster) {
            TIME_INIT;
            void *found;
            for (size_t i = 0; i < loopBooster; i++) {
                PERF_TIMERS_STAT_START;
                found = memmem(newlineProcessStr, origLen, "\n", 1);
                assert(found);
                PERF_TIMERS_STAT_STOP(i);
            }
            TIME_FINISH(loopBooster, "memmem find newline");
            actuallyReadLen = (uintptr_t)found - (uintptr_t)newlineProcessStr;
            PERF_TIMERS_RESULT_PRINT_BYTES(loopBooster, actuallyReadLen);
        }

        TEST_DESC("strchr detect newline at loopBooster %zu", loopBooster) {
            TIME_INIT;
            void *found;
            for (size_t i = 0; i < loopBooster; i++) {
                PERF_TIMERS_STAT_START;
                found = strchr(newlineProcessStr, '\n');
                assert(found);
                PERF_TIMERS_STAT_STOP(i);
            }
            TIME_FINISH(loopBooster, "strchr find newline");
            actuallyReadLen = (uintptr_t)found - (uintptr_t)newlineProcessStr;
            PERF_TIMERS_RESULT_PRINT_BYTES(loopBooster, actuallyReadLen);
        }

        printf("\n");
    }
#endif

    testStrBufToInt64();
    testStrBufToUInt64();
    testStrInt64ToBuf(StrInt64ToBufTable);
    testStrInt64ToBuf(StrInt64ToBuf);

    TEST("ToBuf: one byte short of its bound refuses untouched; at the "
         "bound, the widest value") {
        /* 'fn' given bound - 1 bytes returns 0 and writes nothing; given
         * 'bound' it writes 'text' */
#define STR_TOBUF_BOUND_(fn, bound, value, text)                               \
    do {                                                                       \
        uint8_t b_[64];                                                        \
        memset(b_, '#', sizeof(b_));                                           \
        if (fn(b_, (bound) - 1, (value)) != 0) {                               \
            ERR("%s accepted %d bytes", #fn, (bound) - 1);                     \
        }                                                                      \
        for (size_t i_ = 0; i_ < sizeof(b_); i_++) {                           \
            if (b_[i_] != '#') {                                               \
                ERR("%s wrote byte %zu of a refused buffer", #fn, i_);         \
                break;                                                         \
            }                                                                  \
        }                                                                      \
        const size_t n_ = fn(b_, (bound), (value));                            \
        if (n_ != strlen(text) || memcmp(b_, (text), n_) != 0) {               \
            ERR("%s at %d bytes: %zu digits, want %s", #fn, (bound), n_,       \
                (text));                                                       \
        }                                                                      \
    } while (0)
        STR_TOBUF_BOUND_(StrUInt32ToBuf, 10, UINT32_MAX, "4294967295");
        STR_TOBUF_BOUND_(StrUInt64ToBuf, 20, UINT64_MAX,
                         "18446744073709551615");
        STR_TOBUF_BOUND_(StrInt64ToBuf, 21, INT64_MIN, "-9223372036854775808");
        STR_TOBUF_BOUND_(StrUInt128ToBuf, 40, ~(__uint128_t)0,
                         "340282366920938463463374607431768211455");
        STR_TOBUF_BOUND_(StrInt128ToBuf, 41,
                         (__int128_t)((__uint128_t)1 << 127),
                         "-170141183460469231731687303715884105728");
#undef STR_TOBUF_BOUND_
    }

    TEST("benchmark multiply vs. shifting by 100") {
        /* Smoke size: both forms agree on every input */
        const size_t loopBooster = 100;
        const uint32_t innerFunctionIteration = 1e6;

        {
            TIME_INIT;
            for (size_t i = 1; i < loopBooster; i++) {
                PERF_TIMERS_STAT_START;
                uint64_t intval = microTestA(innerFunctionIteration);
                PERF_TIMERS_STAT_STOP(i * innerFunctionIteration);
                assert(intval);
            }
            TIME_FINISH(loopBooster * innerFunctionIteration,
                        "multiply by 100 using bit shifting");
        }

        {
            TIME_INIT;
            for (size_t i = 1; i < loopBooster; i++) {
                PERF_TIMERS_STAT_START;
                uint64_t intval = microTestB(innerFunctionIteration);
                PERF_TIMERS_STAT_STOP(i * innerFunctionIteration);
                assert(intval);
            }
            TIME_FINISH(loopBooster * innerFunctionIteration,
                        "multiply by 100 using '*'");
        }
    }

    TEST("string to unsigned integer speeds") {
        /* Smoke size: every parser agrees on the input */
        const size_t loopBooster = 100'000;

        {
            TIME_INIT;
            for (size_t i = 0; i < loopBooster; i++) {
                PERF_TIMERS_STAT_START;
                /* Timed with the checks a correct strtoull caller makes:
                 * errno reset, then errno and end pointer tested */
                const char *digits = "18446744073709551615";
                char *end = nullptr;
                errno = 0;
                const uint64_t intval = strtoull(digits, &end, 10);
                const bool parsed = errno == 0 && end == digits + 20;
                PERF_TIMERS_STAT_STOP(i);
                assert(parsed);
                assert(intval == 18446744073709551615ULL);
            }
            TIME_FINISH(loopBooster, "strtoull");
        }

        {
            TIME_INIT;
            for (size_t i = 0; i < loopBooster; i++) {
                PERF_TIMERS_STAT_START;
                uint64_t result;
                (void)StrBufToUInt64("18446744073709551615", 20, &result);
                PERF_TIMERS_STAT_STOP(i);
                assert(result);
                assert(result == 18446744073709551615ULL);
            }
            TIME_FINISH(loopBooster, "StrBufToUInt64");
        }

        {
            TIME_INIT;
            for (size_t i = 0; i < loopBooster; i++) {
                PERF_TIMERS_STAT_START;
                uint64_t result;
                (void)StrBufToUInt64FastCheckOverflow("18446744073709551615",
                                                      20, &result);
                PERF_TIMERS_STAT_STOP(i);
                assert(result);
                assert(result == 18446744073709551615ULL);
            }
            TIME_FINISH(loopBooster, "StrBufToUInt64FastCheckOverflow");
        }

        {
            TIME_INIT;
            for (size_t i = 0; i < loopBooster; i++) {
                PERF_TIMERS_STAT_START;
                uint64_t intval =
                    StrBufToUInt64Fast("18446744073709551615", 20);
                PERF_TIMERS_STAT_STOP(i);
                assert(intval);
                assert(intval == 18446744073709551615ULL);
            }
            TIME_FINISH(loopBooster, "StrBufToUInt64Fast");
        }
    }

    TEST("bit positions 16/32/64 against a naive walk, set and unset") {
        uint64_t words[64];
        uint64_t rng = 0xb17ab17aULL;
        for (size_t i = 0; i < 64; i++) {
            dkLcg64Next(&rng);
            words[i] = rng & (rng >> 7);
        }
        uint64_t setWant[4096];
        uint64_t unsetWant[4096];
        size_t nSet = 0;
        size_t nUnset = 0;
        for (uint64_t bit = 0; bit < 4096; bit++) {
            if (words[bit / 64] >> (bit % 64) & 1) {
                setWant[nSet++] = bit;
            } else {
                unsetWant[nUnset++] = bit;
            }
        }
        uint16_t p16[4096];
        uint32_t p32[4096];
        uint64_t p64[4096];
        bool ok =
            StrBitmapGetSetPositionsExact16(words, sizeof(words), p16) ==
                nSet &&
            StrBitmapGetSetPositionsExact32(words, sizeof(words), p32) ==
                nSet &&
            StrBitmapGetSetPositionsExact64(words, sizeof(words), p64) == nSet;
        for (size_t i = 0; ok && i < nSet; i++) {
            ok = p16[i] == setWant[i] && p32[i] == setWant[i] &&
                 p64[i] == setWant[i];
        }
        ok = ok &&
             StrBitmapGetUnsetPositionsExact16(words, sizeof(words), p16) ==
                 nUnset &&
             StrBitmapGetUnsetPositionsExact32(words, sizeof(words), p32) ==
                 nUnset &&
             StrBitmapGetUnsetPositionsExact64(words, sizeof(words), p64) ==
                 nUnset;
        for (size_t i = 0; ok && i < nUnset; i++) {
            ok = p16[i] == unsetWant[i] && p32[i] == unsetWant[i] &&
                 p64[i] == unsetWant[i];
        }
        if (!ok) {
            ERR("a wide bitmap reader disagrees with the naive walk");
        }
    }

    TEST("number formatting, digit counts and double casts against libc") {
        static constexpr uint64_t edges[] = {0,
                                             9,
                                             10,
                                             99,
                                             100,
                                             65535,
                                             4294967295ULL,
                                             4294967296ULL,
                                             9999999999999999999ULL,
                                             UINT64_MAX};
        char buf[32];
        char want[32];
        for (size_t i = 0; i < countof(edges); i++) {
            const int wantLen =
                snprintf(want, sizeof(want), "%" PRIu64, edges[i]);
            const size_t got = StrUInt64ToBufTable(buf, sizeof(buf), edges[i]);
            if (got != (size_t)wantLen || memcmp(buf, want, got) != 0) {
                ERR("StrUInt64ToBufTable(%" PRIu64 ")", edges[i]);
            }
            if (edges[i] <= UINT32_MAX) {
                const size_t got32 =
                    StrUInt32ToBuf(buf, sizeof(buf), (uint32_t)edges[i]);
                if (got32 != (size_t)wantLen || memcmp(buf, want, got32) != 0) {
                    ERR("StrUInt32ToBuf(%" PRIu64 ")", edges[i]);
                }
            }
            const int64_t s = (int64_t)(edges[i] >> 1) * (i % 2 ? -1 : 1);
            /* The formatted length: a minus sign counts */
            if (StrDigitCountInt64(s) !=
                (size_t)snprintf(want, sizeof(want), "%" PRId64, s)) {
                ERR("StrDigitCountInt64(%" PRId64 ")", s);
            }
        }

        if (StrDoubleToInt64(3.9) != 3 || StrDoubleToInt64(-3.9) != -3 ||
            StrDoubleToInt64(1e300) != INT64_MAX ||
            StrDoubleToInt64(-1e300) != INT64_MIN ||
            !StrDoubleCanBeCastToInt64(42.0) ||
            StrDoubleCanBeCastToInt64(42.5) ||
            StrDoubleCanBeCastToInt64(1e300)) {
            ERR("a double cast is wrong");
        }

        /* ASCII case-insensitive comparison agrees with strcasecmp */
        static const char *const pairs[][2] = {
            {"Hello", "hello"}, {"abc", "abd"}, {"ABD", "abc"}, {"", "a"}};
        for (size_t i = 0; i < 4; i++) {
            const int a = StrICmp(pairs[i][0], pairs[i][1]);
            const int b = strcasecmp(pairs[i][0], pairs[i][1]);
            if ((a > 0) != (b > 0) || (a < 0) != (b < 0)) {
                ERR("StrICmp(\"%s\", \"%s\")", pairs[i][0], pairs[i][1]);
            }

            /* The bounded form agrees with strncasecmp at every bound */
            for (int32_t n = 0; n <= 6; n++) {
                const int32_t an = StrNICmp(pairs[i][0], pairs[i][1], n);
                const int bn = strncasecmp(pairs[i][0], pairs[i][1], (size_t)n);
                if ((an > 0) != (bn > 0) || (an < 0) != (bn < 0)) {
                    ERR("StrNICmp(\"%s\", \"%s\", %d)", pairs[i][0],
                        pairs[i][1], n);
                }
            }
        }
    }

    TEST("log estimates and the double and 128-bit scanners") {
        /* sqlite's LogEst: ten times log2, exact for powers of two */
        if (StrLogEst(1) != 0 || StrLogEst(2) != 10 || StrLogEst(10) != 33 ||
            StrLogEst(1000) != 99 || StrLogEst(1 << 20) != 200 ||
            StrLogEstFromDouble(1000.0) != 99 || StrLogEstAdd(99, 99) != 109) {
            ERR("a LogEst value is wrong");
        }
        for (int k = 0; k < 60; k++) {
            if (StrLogEstToInt(StrLogEst((uint64_t)1 << k)) != (uint64_t)1
                                                                   << k) {
                ERR("LogEst round trip of 2^%d", k);
            }
        }
        const LogEst big = StrLogEstFromDouble(1e30);
        if (big < 990 || big > 1010) {
            ERR("LogEstFromDouble(1e30) = %d", (int)big);
        }

        databox box = {};
        if (!StrScanToDouble("1.5", &box, false, false) ||
            box.data.d64 != 1.5 || StrScanToDouble("x", &box, false, false) ||
            StrScanToDouble("inf", &box, false, false) ||
            !StrScanToDouble("inf", &box, true, false) ||
            !isinf(box.data.d64)) {
            ERR("StrScanToDouble disagrees with the scanner rules");
        }

        /* "nan" and decimal overflow are real NaN and infinities */
        if (!StrScanToDouble("nan", &box, true, false) ||
            box.type != DATABOX_DOUBLE_64 || !isnan(box.data.d64) ||
            !StrScanToDouble("1e400", &box, false, false) ||
            box.data.d64 != INFINITY ||
            !StrScanToDouble("-1e400", &box, false, false) ||
            box.data.d64 != -INFINITY) {
            ERR("nan / overflow scanned to %g", box.data.d64);
        }

        /* Sign and spaces do not depend on the float words */
        if (!StrScanToDouble("-1.5", &box, false, false) ||
            box.data.d64 != -1.5 ||
            !StrScanToDouble("  2.5 ", &box, false, true) ||
            box.data.d64 != 2.5 ||
            StrScanToDouble(" 2.5", &box, false, false) ||
            StrScanToDouble("-nan", &box, false, true)) {
            ERR("sign/space scanning depends on the float words");
        }

        databox setBox = {};
        DATABOX_SET_NAN(&setBox);
        const databox nanBox = DATABOX_NAN;
        const databox posBox = DATABOX_INFINITY_POSITIVE;
        const databox negBox = DATABOX_INFINITY_NEGATIVE;
        if (!isnan(setBox.data.d64) || !isnan(nanBox.data.d64) ||
            posBox.data.d64 != INFINITY || negBox.data.d64 != -INFINITY) {
            ERR("the NaN/infinity box constants are not NaN/infinity");
        }

        /* The PreVerified 128-bit scan matches the checking one on
         * canonical digits */
        static constexpr char int128Max[] =
            "170141183460469231731687303715884105727";
        static const char *const numbers[] = {"0", "18446744073709551616",
                                              int128Max};
        for (size_t i = 0; i < 3; i++) {
            databox smallA;
            databox smallB;
            databoxBig bigA;
            databoxBig bigB;
            databoxBig *useA = nullptr;
            databoxBig *useB = nullptr;
            const size_t len = strlen(numbers[i]);
            const bool a = StrScanScanReliableConvert128(numbers[i], len,
                                                         &smallA, &bigA, &useA);
            const bool b = StrScanScanReliableConvert128PreVerified(
                numbers[i], len, &smallB, &bigB, &useB);
            if (!a || !b || (useA == &bigA) != (useB == &bigB) ||
                (useA == &bigA && *bigA.data.u128 != *bigB.data.u128) ||
                (useA != &bigA && smallA.data.u64 != smallB.data.u64)) {
                ERR("the 128-bit scans disagree on %s", numbers[i]);
            }
        }
    }

    TEST("bit set positions 8") {
        const uint64_t pickles = 0xc0c0c0c0c0c0c0c0ULL;
        const uint8_t positionsValidate[64] = {
            6, 7, 14, 15, 22, 23, 30, 31, 38, 39, 46, 47, 54, 55, 62, 63,
            0, 0, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
            0, 0, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
            0, 0, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0};

        uint8_t positions[64] = {};
        StrBitmapGetSetPositionsExact8(&pickles, sizeof(pickles), positions);
        assert(
            !memcmp(positions, positionsValidate, sizeof(positionsValidate)));
    }

    TEST("bit unset positions 8") {
        const uint64_t pickles = 0xc0c0c0c0c0c0c0c0ULL;

        /* The union of this 'positionsValidate' and the set positions
         * version should always be exactly the set {0, 63} inclusive. */
        const uint8_t positionsValidate[64] = {
            0,  1,  2,  3,  4,  5,  8,  9,  10, 11, 12, 13, 16, 17, 18, 19,
            20, 21, 24, 25, 26, 27, 28, 29, 32, 33, 34, 35, 36, 37, 40, 41,
            42, 43, 44, 45, 48, 49, 50, 51, 52, 53, 56, 57, 58, 59, 60, 61,
            0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0};

        uint8_t positions[64] = {};
        StrBitmapGetUnsetPositionsExact8(&pickles, sizeof(pickles), positions);
        assert(
            !memcmp(positions, positionsValidate, sizeof(positionsValidate)));
    }

    TEST("small bit offset (single storage)") {
        const uint32_t data = 0x000C0000;
        const uint64_t ones = StrPopCntAligned(&data, sizeof(data));
        assert(ones == 2);

        /* Test for 18th element in array */
        uint32_t mask = 1 << 18; /* {18, 19} */

        /* Existence test */
        assert(data & mask);

        /* Get offset of 18th element;
         * (in this case, offset is 0) */
        uint32_t work = data & (mask - 1);
        assert(StrPopCntAligned(&work, sizeof(work)) == 0);

        /* Test for 19th element in array */
        mask = 1 << 19;

        /* Existence test */
        assert(data & mask);

        /* Get offset of 19th element;
         * (in this case, offset is 1) */
        work = data & (mask - 1);
        assert(StrPopCntAligned(&work, sizeof(work)) == 1);
    }

    TEST("large bit offset (multi storage)") {
        /* 32 bits * 8 = 256 bits total.
         * Set positions are:
         *  18,  19,  50,  51,  82,  83,  114,
         *  115, 146, 147, 178, 179, 210, 211,
         *  242, 243 */
        uint32_t more[8] = {0x000C0000, 0x000C0000, 0x000C0000, 0x000C0000,
                            0x000C0000, 0x000C0000, 0x000C0000, 0x000C0000};
        const uint8_t setPositions[] = {18,  19,  50,  51,  82,  83,  114, 115,
                                        146, 147, 178, 179, 210, 211, 242, 243};
        /* setOffsets are just the index of each set position, so
         * offset 0 == 18, offset 1 == 19, ..., offset 17 == 243. */
        const uint32_t ones = StrPopCntAligned(more, sizeof(more));
        assert(ones == 16);

        for (size_t i = 0; i < countof(setPositions); i++) {
            /* The lowest number element we need to compare against */
            const uint32_t extent = setPositions[i] / (sizeof(*more) * 8);

            /* Test for element in array */
            const uint32_t existenceTestMask =
                1 << (setPositions[i] - (extent * (sizeof(*more) * 8)));
            assert(more[extent] & existenceTestMask);

#if 0
            /* Get offset of 'i' */
            uint32_t work[8]; // extent + 1];

            /* Copy set bits up to just before 'extent' */
            for (uint32_t j = 0; j < extent; j++) {
                work[j] = more[j];
            }
            /* Now copy the number of bits *before* the bit we're
             * testing for. */
            work[extent] = more[extent] & (existenceTestMask - 1);
            assert(StrPopCntAligned(work, sizeof(uint32_t) * (extent + 1)) ==
                   i);
#else
            /* OPTIONAL:
             *   - We could copy more[extent] to a temp variable,
             *     replace more[extent] with &= (existenceTestMask - 1),
             *     run the popcnt, then replace more[extent] with its
             *     original value.
             *     That would save us from needing to copy
             *     'more' into 'work' each time, but it makes
             *     this offset calculation a "write" for 'more' so
             *     we couldn't have multiple readers operating at once. */
            uint32_t moreSaved = more[extent];
            more[extent] &= (existenceTestMask - 1);
            assert(StrPopCntAligned(more, sizeof(uint32_t) * (extent + 1)) ==
                   i);
            more[extent] = moreSaved;
#endif
        }
    }

#if 1
    TEST("lenutf8 ascii small") {
        const char *foo = "hellooooooooooooooooooooooooooooo foo";
        const size_t len = strlen(foo);
        const size_t utf8len = StrLenUtf8(foo, len);
        if (len != utf8len) {
            ERR("Expected len %zu but got %zu instead!", len, utf8len);
        }
    }

    TEST("lenutf8countbytes ascii small") {
        const char *foo = "hellooooooooooooooooooooooooooooo foo";
        const size_t len = strlen(foo);
        const size_t utf8len = StrLenUtf8CountBytes(foo, len, len);
        if (len != utf8len) {
            ERR("Expected len %zu but got %zu instead!", len, utf8len);
        }
    }

    TEST("lenutf8 ascii large") {
        const char *foo = "hellooooooooooooooooooooooooooooo foo";
        const size_t len = strlen(foo);
        const int booster = 1'000'000;

        cleanerName(zfree, Chars) char *tester =
            zmalloc(len * booster); /* 37 MB */
        for (size_t i = 0; i < len * booster; i += len) {
            memcpy(tester + i, foo, len);
        }

        /* Each pass reads the whole ~38 MB buffer; a few passes check
         * the count (perf builds can raise this to time it) */
        const size_t loopBooster = 3;
        size_t utf8len;
        {
            TIME_INIT;
            for (size_t i = 0; i < loopBooster; i++) {
                PERF_TIMERS_STAT_START;
                utf8len = StrLenUtf8(tester, len * booster);
                assert(utf8len);
                PERF_TIMERS_STAT_STOP(i);
            }
            TIME_FINISH(loopBooster, "utf8len ascii large");
            PERF_TIMERS_RESULT_PRINT_BYTES(loopBooster, len * booster);
        }

        if ((len * booster) != utf8len) {
            ERR("Expected len %zu but got %zu instead!", len * booster,
                utf8len);
        }
    }

    TEST("lenutf8countbytes ascii large") {
        const char *foo = "hellooooooooooooooooooooooooooooo foo";
        const size_t len = strlen(foo);
        const int booster = 1'000'000;

        cleanerName(zfree, Chars) char *tester =
            zmalloc(len * booster); /* 37 MB */
        for (size_t i = 0; i < len * booster; i += len) {
            memcpy(tester + i, foo, len);
        }

        /* Each pass reads the whole ~38 MB buffer; a few passes check
         * the count (perf builds can raise this to time it) */
        const size_t loopBooster = 3;
        size_t utf8len;
        {
            TIME_INIT;
            for (size_t i = 0; i < loopBooster; i++) {
                PERF_TIMERS_STAT_START;
                utf8len = StrLenUtf8CountBytes(tester, len * booster,
                                               (len * booster) / 2);
                assert(utf8len);
                PERF_TIMERS_STAT_STOP(i);
            }
            TIME_FINISH(loopBooster, "utf8lencountbytes ascii large");
            /* Only report on 'utf8len' bytes we traversed for the count;
             * we don't process 'len * booster' bytes because we didn't
             * traverse that far! */
            PERF_TIMERS_RESULT_PRINT_BYTES(loopBooster, utf8len);
        }

        if ((len * booster) / 2 != utf8len) {
            ERR("Expected len %zu but got %zu instead!", (len * booster) / 2,
                utf8len);
        }
    }

    TEST("lenutf8 utf8 small") {
        const char foo[] = u8"😀";
        const size_t things = 1;
        const size_t len = strlen((const char *)foo);
        const size_t utf8len = StrLenUtf8(foo, len);
        if (things != utf8len) {
            ERR("Expected len %zu but got %zu instead!", things, utf8len);
        }
    }

    TEST("lenutf8countbytes utf8 small") {
        const char foo[] = u8"😀";
        const size_t len = strlen((const char *)foo);
        const size_t utf8len = StrLenUtf8CountBytes(foo, len, 1);
        if (len != utf8len) {
            ERR("Expected len %zu but got %zu instead!", len, utf8len);
        }
    }

    TEST("lenutf8countbytes utf8 small (mixed, extract)") {
        const char foo[] = u8"abcdef😀abcdef";
        const size_t len = strlen((const char *)foo);
        const size_t utf8len = StrLenUtf8CountBytes(foo + 6, len, 1);
        if (4 != utf8len) {
            ERR("Expected len %d but got %zu instead!", 4, utf8len);
        }
    }

    TEST("lenutf8countbytes every lead byte") {
        /* Each lead byte declares its character's length; the bytes of
         * the first k characters always end on a character boundary.
         * Prefixes of 1 and 10 bytes reach the tail loop and the
         * step-by-word loop. */
        static const char prefix[] = "abcdefghij";
        static const size_t prefixLens[] = {1, 10};
        for (uint32_t lead = 0xC2; lead <= 0xF4; lead++) {
            const size_t n = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
            for (size_t p = 0; p < countof(prefixLens); p++) {
                const size_t pre = prefixLens[p];
                uint8_t buf[16];
                memcpy(buf, prefix, pre);
                buf[pre] = (uint8_t)lead;
                memset(buf + pre + 1, 0x80, n - 1);
                buf[pre + n] = 'z';
                const size_t len = pre + n + 1;

                const size_t through = StrLenUtf8CountBytes(buf, len, pre + 1);
                if (through != pre + n) {
                    ERR("lead 0x%02" PRIX32 " after %zu: %zu bytes, want %zu",
                        lead, pre, through, pre + n);
                }

                const size_t all = StrLenUtf8CountBytes(buf, len, pre + 2);
                if (all != len) {
                    ERR("lead 0x%02" PRIX32 " after %zu: %zu bytes, want %zu",
                        lead, pre, all, len);
                }
            }
        }
    }

#define _TU8MIDDLES                                                            \
    "\xF0\x9F\x98\x81"                                                         \
    "\xF0\x9F\x98\x82"                                                         \
    "\xF0\x9F\x98\x83"                                                         \
    "\xF0\x9F\x98\x84"                                                         \
    "\xF0\x9F\x98\x85"

    for (int i = 0; i < (int)sizeof(STRLEN_UTF8_STEP); i++) {
        TEST_DESC("lenutf8countbytes utf8 small (substr; offset by %d)", i) {
            const char foo[] = u8"abcdefhello " _TU8MIDDLES "fedcba";
            uint8_t space[640] = {};

            memset(space, 'Q', sizeof(space));

            const size_t len = strlen((const char *)foo);
            memcpy(space + i, foo, len);

            const size_t startBytes =
                StrLenUtf8CountBytes(space, len + i, 6 + i);
            const size_t extentBytes = StrLenUtf8CountBytes(
                space + startBytes, len + i - startBytes, 11);

            if (26 != extentBytes) {
                ERR("Expected len %d but got %zu instead!", 26, extentBytes);
            }
        }
    }

    TEST("lenutf8countbytes utf8 small (overshot)") {
        const char foo[] = u8"😀";
        const size_t len = strlen((const char *)foo);
        const size_t utf8len = StrLenUtf8CountBytes(foo, len, 20);
        if (len != utf8len) {
            ERR("Expected len %zu but got %zu instead!", len, utf8len);
        }
    }

    TEST("lenutf8 utf8 bigger") {
        const char foo[] = u8"💛💙💜💔";
        const wchar_t wfoo[] = {0xF09F929B, 0xF09F929C, 0xF09F929D, 0xF09F929E,
                                0x0};
        const size_t things = 4;
        const size_t len = strlen((const char *)foo);
        const size_t utf8len = StrLenUtf8(foo, len);

        /* wcslen just counts the number of 4 byte quantities in
         * a wchar_t before the zero byte.  It does *not* count
         * unicode characters.  */
        const size_t wcharlen = wcslen(wfoo);
        if (things != utf8len) {
            ERR("Expected len %zu but got %zu instead!", things, utf8len);
        }

        if (wcharlen != utf8len) {
            ERR("Expected len %zu but got %zu instead!", wcharlen, utf8len);
        }
    }

    TEST("lenutf8countbytes utf8 bigger") {
        const char foo[] = u8"💛💙💜💔";
        const size_t things = 4;
        const size_t len = strlen((const char *)foo);
        const size_t utf8len = StrLenUtf8CountBytes(foo, len, things);

        if (len != utf8len) {
            ERR("Expected len %zu but got %zu instead!", len, utf8len);
        }
    }

    TEST("lenutf8countbytes utf8 bigger (overshot)") {
        const char foo[] = u8"💛💙💜💔";
        const size_t things = 4;
        const size_t len = strlen((const char *)foo);
        const size_t utf8len = StrLenUtf8CountBytes(foo, len, things * 2);

        if (len != utf8len) {
            ERR("Expected len %zu but got %zu instead!", len, utf8len);
        }
    }

    for (int i = 0; i < (int)sizeof(STRLEN_UTF8_STEP); i++) {
        TEST_DESC("lenutf8countbytes utf8 bigger (alignment offset %d)", i) {
            cleanerName(zfree, Chars) char *alignmentHelper = zcalloc(1, 640);
            const char foo[] = u8"💛💙💜💔";
            const size_t things = 4;
            const size_t len = strlen((const char *)foo);

            /* Populate our data starting with offset alignment 'i' */
            memcpy(alignmentHelper + i, foo, sizeof(foo));

            const size_t utf8len =
                StrLenUtf8CountBytes(alignmentHelper + i, len, things);

            if (len != utf8len) {
                ERR("Expected len %zu but got %zu instead!", len, utf8len);
            }
        }
    }

    for (int i = 0; i < (int)sizeof(STRLEN_UTF8_STEP); i++) {
        TEST_DESC("lenutf8countbytes utf8 multi-boundary (alignment offset %d)",
                  i) {
            cleanerName(zfree, Chars) char *alignmentHelper = zcalloc(1, 640);
            /* pattern:
             *   emoji, ascii, emoji , ascii, emoji, ascii, emoji, ascii */
            const char foo[] = u8"💛q💙m💜z💔p";
            const size_t things = 8; /* character count == 8 */
            const size_t len = strlen((const char *)foo); /* len == 20 */

            /* Populate our data starting with offset alignment 'i' */
            memcpy(alignmentHelper + i, foo, sizeof(foo));

            const size_t utf8len =
                StrLenUtf8CountBytes(alignmentHelper + i, len, things);

            if (len != utf8len) {
                ERR("Expected len %zu but got %zu instead!", len, utf8len);
            }
        }
    }

    TEST("lenutf8 utf8 lots") {
        const wchar_t wfoo[] = {0xF09F9881, 0xF09F9882, 0xF09F9883, 0xF09F9884,
                                0xF09F9885, 0xF09F9886, 0xF09F9887, 0xF09F9888,
                                0xF09F9889, 0xF09F988A, 0xF09F988B, 0xF09F988C,
                                0xF09F988D, 0xF09F988E, 0xF09F988F, 0x0};
        const size_t things = 15;
        const size_t len = strlen((const char *)wfoo);
        const size_t utf8len = StrLenUtf8(wfoo, len);
        const size_t wcharlen = wcslen(wfoo);
        if (things != utf8len) {
            ERR("Expected len %zu but got %zu instead!", things, utf8len);
        }

        if (wcharlen != utf8len) {
            ERR("Expected len %zu but got %zu instead!", wcharlen, utf8len);
        }
    }

    TEST("lenutf8 utf8 large") {
        const char foo[] = u8"💛💙💜💔💛💙💜💔💛💙💜💔💛💙💜💔💛💙💜💔💛💙💜💔"
                           u8"💛💙💜💔💛💙💜💔";
        const size_t things = 32; /* 32 things, each 4 bytes, 128 bytes total */
        const size_t len = strlen(foo); /* len == 128 */
        const int booster = 300'000;    /* total of 32,000 final things */

        cleanerName(zfree, Chars) char *tester =
            zcalloc(1, len * booster + 1); /* 38.4 MB */
        for (size_t i = 0; i < len * booster; i += len) {
            memcpy(tester + i, foo, len);
        }

        /* Each pass reads the whole ~38 MB buffer; a few passes check
         * the count (perf builds can raise this to time it) */
        const size_t loopBooster = 3;
        size_t utf8len;
        {
            TIME_INIT;
            for (size_t i = 0; i < loopBooster; i++) {
                PERF_TIMERS_STAT_START;
                utf8len = StrLenUtf8(tester, len * booster);
                PERF_TIMERS_STAT_STOP(i);
                assert(utf8len);
            }
            TIME_FINISH(loopBooster, "utf8len utf8 large");
            PERF_TIMERS_RESULT_PRINT_BYTES(loopBooster, len * booster);
        }

        if ((things * booster) != utf8len) {
            ERR("Expected len %zu but got %zu instead!", things * booster,
                utf8len);
        }
    }
#endif

    for (int alignmentOffset = 0;
         alignmentOffset < (int)sizeof(STRLEN_UTF8_STEP); alignmentOffset++) {
        TEST_DESC("lenutf8countbytes utf8 large (alignment offset %d)",
                  alignmentOffset) {
            const char foo[] = u8"💛💙💜💔💛💙💜💔💛💙💜💔💛💙💜💔💛💙💜💔💛💙"
                               u8"💜💔💛💙💜💔💛💙💜💔";
            const size_t len = strlen(foo); /* len == 128 */
            const int booster = 300'000;    /* total of 32,000 final things */
            const size_t countCharacters = (32 * booster) / 2;

            /* each emoji is a 4 byte UTF-8 character */
            const size_t countedBytes = 4 * countCharacters;

            cleanerName(zfree, Chars) char *tester =
                zcalloc(1, len * booster + 1 + 8); /* 38.4 MB */
            char *startTester = tester + alignmentOffset;

            for (size_t i = 0; i < len * booster; i += len) {
                memcpy(startTester + i, foo, len);
            }

            /* Each pass reads the whole ~38 MB buffer; a few passes check
             * the count (perf builds can raise this to time it) */
            const size_t loopBooster = 3;
            size_t utf8len;
            {
                TIME_INIT;
                for (size_t i = 0; i < loopBooster; i++) {
                    PERF_TIMERS_STAT_START;
                    utf8len = StrLenUtf8CountBytes(startTester, len * booster,
                                                   countCharacters);
                    PERF_TIMERS_STAT_STOP(i);
                    assert(utf8len);
                }
                TIME_FINISH(loopBooster, "utf8lencountbytes utf8 large");
                PERF_TIMERS_RESULT_PRINT_BYTES(loopBooster, utf8len);
            }

            if (countedBytes != utf8len) {
                ERR("Expected len %zu but got %zu instead!", countedBytes,
                    utf8len);
            }
        }
    }

    const size_t xorBooster = 700000;
    TEST("xorshift128 700,000") {
        TIME_INIT;
        uint32_t x = 5;
        uint32_t y = 5;
        uint32_t z = 5;
        uint32_t w = 5;
        for (size_t i = 0; i < xorBooster; i++) {
            xorshift128(&x, &y, &z, &w);
            assert(w);
        }
        TIME_FINISH(xorBooster, "xorshift128");
    }

    TEST("xorshift64star 700,000") {
        TIME_INIT;
        uint64_t x = 5;
        for (size_t i = 0; i < xorBooster; i++) {
            const uint64_t result = xorshift64star(&x);
            assert(result);
        }
        TIME_FINISH(xorBooster, "xorshift64star");
    }

    TEST("xorshift1024star 700,000") {
        TIME_INIT;
        uint64_t s[16] = {5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5};
        uint_fast8_t sIndex = 0;
        for (size_t i = 0; i < xorBooster; i++) {
            const uint64_t result = xorshift1024star(s, &sIndex);
            assert(result);
        }
        TIME_FINISH(xorBooster, "xorshift1024star");
    }

    TEST("xorshift128plus 700,000") {
        TIME_INIT;
        uint64_t s[2] = {5, 5};
        for (size_t i = 0; i < xorBooster; i++) {
            const uint64_t result = xorshift128plus(s);
            assert(result);
        }
        TIME_FINISH(xorBooster, "xorshift128plus");
    }

    /* ================================================================
     * SIMD vs Scalar Performance Comparison Benchmarks
     * ================================================================ */
    TEST("Benchmark: StrBufToUInt64 SWAR vs Scalar") {
        printf("  Comparing string-to-integer: SWAR vs Scalar...\n");
        const char *testNumbers[] = {
            "12345678",         /* 8 digits - SWAR path */
            "1234567890123456", /* 16 digits - 2x SWAR */
            "1234",             /* 4 digits - scalar only */
        };
        const size_t iterations = 2'000'000;

        for (size_t t = 0; t < countof(testNumbers); t++) {
            const char *num = testNumbers[t];
            const size_t len = strlen(num);
            uint64_t resultSWAR = 0;
            uint64_t resultScalar = 0;

            /* SWAR version */
            TIME_INIT;
            for (size_t i = 0; i < iterations; i++) {
                resultSWAR = StrBufToUInt64Fast(num, len);
            }
            TIME_FINISH(iterations, "SWAR");

            /* Scalar version */
            TIME_INIT;
            for (size_t i = 0; i < iterations; i++) {
                resultScalar = StrBufToUInt64Scalar(num, len);
            }
            TIME_FINISH(iterations, "Scalar");

            /* Verify correctness */
            if (resultSWAR != resultScalar) {
                ERR("Mismatch! SWAR=%" PRIu64 " Scalar=%" PRIu64, resultSWAR,
                    resultScalar);
            }
            printf("    %zu digits: result=%" PRIu64 " (verified)\n", len,
                   resultSWAR);
        }
    }

    TEST("Benchmark: StrPopCnt NEON/SIMD vs Scalar") {
        printf("  Comparing popcount: Optimized vs Scalar (lookup table)...\n");
        const size_t bufSize = 4096;
        cleanerName(zfree, U8) uint8_t *buf = zmalloc(bufSize);
        for (size_t i = 0; i < bufSize; i++) {
            buf[i] = (uint8_t)(i * 31 + 17);
        }

        const size_t iterations = 100'000;
        uint64_t resultOpt = 0;
        uint64_t resultScalar = 0;

        /* Optimized version */
        TIME_INIT;
        for (size_t i = 0; i < iterations; i++) {
            resultOpt = StrPopCntAligned(buf, bufSize);
        }
        TIME_FINISH(iterations, "Optimized");
        PERF_TIMERS_RESULT_PRINT_BYTES(iterations, bufSize);

        /* Scalar version */
        TIME_INIT;
        for (size_t i = 0; i < iterations; i++) {
            resultScalar = StrPopCntScalar(buf, bufSize);
        }
        TIME_FINISH(iterations, "Scalar");
        PERF_TIMERS_RESULT_PRINT_BYTES(iterations, bufSize);

        if (resultOpt != resultScalar) {
            ERR("Mismatch! Opt=%" PRIu64 " Scalar=%" PRIu64, resultOpt,
                resultScalar);
        }
        printf("    4KB popcount: %" PRIu64 " bits (verified)\n", resultOpt);
    }

    TEST("Benchmark: StrLenUtf8 SIMD vs Scalar") {
        printf("  Comparing UTF-8 strlen: SIMD vs Scalar...\n");
        const size_t bufSize = 4096;
        cleanerName(zfree, Chars) char *buf = zmalloc(bufSize);
        memset(buf, 'a', bufSize - 1);
        buf[bufSize - 1] = '\0';

        const size_t iterations = 100'000;
        size_t resultOpt = 0;
        size_t resultScalar = 0;

        /* SIMD version */
        TIME_INIT;
        for (size_t i = 0; i < iterations; i++) {
            resultOpt = StrLenUtf8(buf, bufSize - 1);
        }
        TIME_FINISH(iterations, "SIMD");
        PERF_TIMERS_RESULT_PRINT_BYTES(iterations, bufSize - 1);

        /* Scalar version */
        TIME_INIT;
        for (size_t i = 0; i < iterations; i++) {
            resultScalar = StrLenUtf8Scalar(buf, bufSize - 1);
        }
        TIME_FINISH(iterations, "Scalar");
        PERF_TIMERS_RESULT_PRINT_BYTES(iterations, bufSize - 1);

        if (resultOpt != resultScalar) {
            ERR("Mismatch! SIMD=%zu Scalar=%zu", resultOpt, resultScalar);
        }
        printf("    4KB ASCII: %zu chars (verified)\n", resultOpt);
    }

    TEST("Benchmark: StrLenUtf8 on mixed UTF-8") {
        printf("  Comparing UTF-8 strlen on mixed content...\n");
        const char *mixedUtf8 = "Hello 世界! 🎉 Testing UTF-8 lengths 日本語";
        const size_t len = strlen(mixedUtf8);
        const size_t iterations = 2'000'000;
        size_t resultOpt = 0;
        size_t resultScalar = 0;

        /* SIMD version */
        TIME_INIT;
        for (size_t i = 0; i < iterations; i++) {
            resultOpt = StrLenUtf8(mixedUtf8, len);
        }
        TIME_FINISH(iterations, "SIMD");

        /* Scalar version */
        TIME_INIT;
        for (size_t i = 0; i < iterations; i++) {
            resultScalar = StrLenUtf8Scalar(mixedUtf8, len);
        }
        TIME_FINISH(iterations, "Scalar");

        if (resultOpt != resultScalar) {
            ERR("Mismatch! SIMD=%zu Scalar=%zu", resultOpt, resultScalar);
        }
        printf("    %zu bytes -> %zu chars (verified)\n", len, resultOpt);
    }

    TEST("Benchmark: StrUInt9DigitsToBuf") {
        printf("  Benchmarking integer-to-string conversion...\n");
        const size_t iterations = 1'000'000;
        uint8_t buf[16];
        uint64_t checksum = 0;

        TIME_INIT;
        for (size_t i = 0; i < iterations; i++) {
            StrUInt9DigitsToBuf(buf, (uint32_t)(i % 1'000'000'000));
            checksum += buf[0];
        }
        TIME_FINISH(iterations, "StrUInt9DigitsToBuf");

        printf("    Checksum: %" PRIu64 "\n", checksum);
    }

    /* ====================================================================
     * UTF-8 Validation Tests
     * ==================================================================== */
    TEST("StrUtf8Valid: Valid ASCII strings") {
        /* Empty string is valid */
        assert(StrUtf8Valid("", 0) == true);
        assert(StrUtf8ValidScalar("", 0) == true);

        /* Simple ASCII */
        const char *ascii = "Hello, World!";
        assert(StrUtf8Valid(ascii, strlen(ascii)) == true);
        assert(StrUtf8ValidScalar(ascii, strlen(ascii)) == true);

        /* All printable ASCII */
        char allAscii[128];
        for (int i = 0; i < 95; i++) {
            allAscii[i] = (char)(32 + i);
        }
        allAscii[95] = '\0';
        assert(StrUtf8Valid(allAscii, 95) == true);
        assert(StrUtf8ValidScalar(allAscii, 95) == true);

        /* Long ASCII string (tests SIMD paths) */
        cleanerName(zfree, Chars) char *longAscii = zmalloc(1024);
        for (int i = 0; i < 1023; i++) {
            longAscii[i] = 'A' + (i % 26);
        }
        longAscii[1023] = '\0';
        assert(StrUtf8Valid(longAscii, 1023) == true);
        assert(StrUtf8ValidScalar(longAscii, 1023) == true);
    }

    TEST("StrUtf8Valid: Valid 2-byte sequences") {
        /* Latin Extended: ñ = C3 B1 */
        const uint8_t latin[] = {0xC3, 0xB1, 0x00}; /* ñ */
        assert(StrUtf8Valid(latin, 2) == true);
        assert(StrUtf8ValidScalar(latin, 2) == true);

        /* Smallest valid 2-byte: U+0080 = C2 80 */
        const uint8_t smallest2[] = {0xC2, 0x80, 0x00};
        assert(StrUtf8Valid(smallest2, 2) == true);

        /* Largest valid 2-byte: U+07FF = DF BF */
        const uint8_t largest2[] = {0xDF, 0xBF, 0x00};
        assert(StrUtf8Valid(largest2, 2) == true);

        /* Multiple 2-byte chars */
        const uint8_t multi2[] = {0xC3, 0xA9, 0xC3, 0xA0, 0xC3, 0xBC}; /* éàü */
        assert(StrUtf8Valid(multi2, 6) == true);
    }

    TEST("StrUtf8Valid: Valid 3-byte sequences") {
        /* Chinese: 中 = E4 B8 AD */
        const uint8_t chinese[] = {0xE4, 0xB8, 0xAD, 0x00};
        assert(StrUtf8Valid(chinese, 3) == true);
        assert(StrUtf8ValidScalar(chinese, 3) == true);

        /* Smallest valid 3-byte: U+0800 = E0 A0 80 */
        const uint8_t smallest3[] = {0xE0, 0xA0, 0x80};
        assert(StrUtf8Valid(smallest3, 3) == true);

        /* Largest valid 3-byte: U+FFFF = EF BF BF */
        const uint8_t largest3[] = {0xEF, 0xBF, 0xBF};
        assert(StrUtf8Valid(largest3, 3) == true);

        /* Just before surrogates: U+D7FF = ED 9F BF */
        const uint8_t beforeSurr[] = {0xED, 0x9F, 0xBF};
        assert(StrUtf8Valid(beforeSurr, 3) == true);

        /* Just after surrogates: U+E000 = EE 80 80 */
        const uint8_t afterSurr[] = {0xEE, 0x80, 0x80};
        assert(StrUtf8Valid(afterSurr, 3) == true);
    }

    TEST("StrUtf8Valid: Valid 4-byte sequences") {
        /* Emoji: 😀 = F0 9F 98 80 */
        const uint8_t emoji[] = {0xF0, 0x9F, 0x98, 0x80};
        assert(StrUtf8Valid(emoji, 4) == true);
        assert(StrUtf8ValidScalar(emoji, 4) == true);

        /* Smallest valid 4-byte: U+10000 = F0 90 80 80 */
        const uint8_t smallest4[] = {0xF0, 0x90, 0x80, 0x80};
        assert(StrUtf8Valid(smallest4, 4) == true);

        /* Largest valid 4-byte: U+10FFFF = F4 8F BF BF */
        const uint8_t largest4[] = {0xF4, 0x8F, 0xBF, 0xBF};
        assert(StrUtf8Valid(largest4, 4) == true);
    }

    TEST("StrUtf8Valid: Mixed valid sequences") {
        /* "Hello 世界 😀" */
        const char *mixed = "Hello \xE4\xB8\x96\xE7\x95\x8C \xF0\x9F\x98\x80";
        assert(StrUtf8Valid(mixed, strlen(mixed)) == true);
        assert(StrUtf8ValidScalar(mixed, strlen(mixed)) == true);

        /* Japanese text */
        const char *japanese = "日本語テスト";
        assert(StrUtf8Valid(japanese, strlen(japanese)) == true);
    }

    TEST("StrUtf8Valid: Invalid - Overlong 2-byte") {
        /* C0 80 = overlong NUL (should be 00) */
        const uint8_t overlongNul[] = {0xC0, 0x80};
        assert(StrUtf8Valid(overlongNul, 2) == false);
        assert(StrUtf8ValidScalar(overlongNul, 2) == false);

        /* C1 BF = overlong DEL (should be 7F) */
        const uint8_t overlongDel[] = {0xC1, 0xBF};
        assert(StrUtf8Valid(overlongDel, 2) == false);

        /* C0 AF = overlong slash (security issue in old code) */
        const uint8_t overlongSlash[] = {0xC0, 0xAF};
        assert(StrUtf8Valid(overlongSlash, 2) == false);
    }

    TEST("StrUtf8Valid: Invalid - Overlong 3-byte") {
        /* E0 80 80 = overlong NUL */
        const uint8_t overlong3Nul[] = {0xE0, 0x80, 0x80};
        assert(StrUtf8Valid(overlong3Nul, 3) == false);
        assert(StrUtf8ValidScalar(overlong3Nul, 3) == false);

        /* E0 9F BF = overlong (should fit in 2 bytes) */
        const uint8_t overlong3[] = {0xE0, 0x9F, 0xBF};
        assert(StrUtf8Valid(overlong3, 3) == false);
    }

    TEST("StrUtf8Valid: Invalid - Overlong 4-byte") {
        /* F0 80 80 80 = overlong NUL */
        const uint8_t overlong4Nul[] = {0xF0, 0x80, 0x80, 0x80};
        assert(StrUtf8Valid(overlong4Nul, 4) == false);
        assert(StrUtf8ValidScalar(overlong4Nul, 4) == false);

        /* F0 8F BF BF = overlong (should fit in 3 bytes) */
        const uint8_t overlong4[] = {0xF0, 0x8F, 0xBF, 0xBF};
        assert(StrUtf8Valid(overlong4, 4) == false);
    }

    TEST("StrUtf8Valid: Invalid - Surrogates") {
        /* ED A0 80 = U+D800 (high surrogate start) */
        const uint8_t surrHigh[] = {0xED, 0xA0, 0x80};
        assert(StrUtf8Valid(surrHigh, 3) == false);
        assert(StrUtf8ValidScalar(surrHigh, 3) == false);

        /* ED AF BF = U+DBFF (high surrogate end) */
        const uint8_t surrHighEnd[] = {0xED, 0xAF, 0xBF};
        assert(StrUtf8Valid(surrHighEnd, 3) == false);

        /* ED B0 80 = U+DC00 (low surrogate start) */
        const uint8_t surrLow[] = {0xED, 0xB0, 0x80};
        assert(StrUtf8Valid(surrLow, 3) == false);

        /* ED BF BF = U+DFFF (low surrogate end) */
        const uint8_t surrLowEnd[] = {0xED, 0xBF, 0xBF};
        assert(StrUtf8Valid(surrLowEnd, 3) == false);
    }

    TEST("StrUtf8Valid: Invalid - Codepoints > U+10FFFF") {
        /* F4 90 80 80 = U+110000 (too large) */
        const uint8_t tooLarge1[] = {0xF4, 0x90, 0x80, 0x80};
        assert(StrUtf8Valid(tooLarge1, 4) == false);
        assert(StrUtf8ValidScalar(tooLarge1, 4) == false);

        /* F5 80 80 80 would be > U+10FFFF (invalid start byte) */
        const uint8_t f5[] = {0xF5, 0x80, 0x80, 0x80};
        assert(StrUtf8Valid(f5, 4) == false);

        /* FF is always invalid */
        const uint8_t ff[] = {0xFF};
        assert(StrUtf8Valid(ff, 1) == false);

        /* FE is always invalid */
        const uint8_t fe[] = {0xFE};
        assert(StrUtf8Valid(fe, 1) == false);
    }

    TEST("StrUtf8Valid: Invalid - Truncated sequences") {
        /* 2-byte truncated */
        const uint8_t trunc2[] = {0xC3};
        assert(StrUtf8Valid(trunc2, 1) == false);
        assert(StrUtf8ValidScalar(trunc2, 1) == false);

        /* 3-byte truncated (missing 1) */
        const uint8_t trunc3a[] = {0xE4, 0xB8};
        assert(StrUtf8Valid(trunc3a, 2) == false);

        /* 3-byte truncated (missing 2) */
        const uint8_t trunc3b[] = {0xE4};
        assert(StrUtf8Valid(trunc3b, 1) == false);

        /* 4-byte truncated (missing 1) */
        const uint8_t trunc4a[] = {0xF0, 0x9F, 0x98};
        assert(StrUtf8Valid(trunc4a, 3) == false);

        /* 4-byte truncated (missing 2) */
        const uint8_t trunc4b[] = {0xF0, 0x9F};
        assert(StrUtf8Valid(trunc4b, 2) == false);

        /* 4-byte truncated (missing 3) */
        const uint8_t trunc4c[] = {0xF0};
        assert(StrUtf8Valid(trunc4c, 1) == false);
    }

    TEST("StrUtf8Valid: Invalid - Orphan continuation bytes") {
        /* Single continuation byte at start */
        const uint8_t orphan1[] = {0x80};
        assert(StrUtf8Valid(orphan1, 1) == false);
        assert(StrUtf8ValidScalar(orphan1, 1) == false);

        const uint8_t orphan2[] = {0xBF};
        assert(StrUtf8Valid(orphan2, 1) == false);

        /* Continuation after ASCII */
        const uint8_t orphan3[] = {'a', 0x80, 'b'};
        assert(StrUtf8Valid(orphan3, 3) == false);

        /* Double continuation */
        const uint8_t orphan4[] = {0x80, 0x80};
        assert(StrUtf8Valid(orphan4, 2) == false);
    }

    TEST("StrUtf8Valid: Invalid - Wrong continuation count") {
        /* 2-byte start with non-continuation second byte */
        const uint8_t bad2[] = {0xC3, 0x30}; /* C3 followed by ASCII '0' */
        assert(StrUtf8Valid(bad2, 2) == false);
        assert(StrUtf8ValidScalar(bad2, 2) == false);

        /* 3-byte start with non-continuation */
        const uint8_t bad3a[] = {0xE4, 0xB8, 0x30};
        assert(StrUtf8Valid(bad3a, 3) == false);

        const uint8_t bad3b[] = {0xE4, 0x30, 0x80};
        assert(StrUtf8Valid(bad3b, 3) == false);

        /* 4-byte start with non-continuation */
        const uint8_t bad4a[] = {0xF0, 0x9F, 0x98, 0x30};
        assert(StrUtf8Valid(bad4a, 4) == false);
    }

    TEST("StrUtf8ValidCStr: Null-terminated validation") {
        assert(StrUtf8ValidCStr(nullptr) == false);
        assert(StrUtf8ValidCStr("") == true);
        assert(StrUtf8ValidCStr("Hello") == true);

        /* Valid UTF-8 C string */
        assert(StrUtf8ValidCStr("Héllo 世界") == true);
        assert(StrUtf8ValidCStrScalar("Héllo 世界") == true);

        /* Invalid embedded in C string */
        char badStr[10] = "ab";
        badStr[2] = (char)0xC0;
        badStr[3] = (char)0x80;
        badStr[4] = 'c';
        badStr[5] = '\0';
        assert(StrUtf8ValidCStr(badStr) == false);
    }

    TEST("StrUtf8ValidCount: Validate and count codepoints") {
        bool valid;

        /* ASCII */
        assert(StrUtf8ValidCount("Hello", 5, &valid) == 5 && valid == true);

        /* Mixed UTF-8: "Hi 世界" = 2 ASCII + 1 space + 2 Chinese = 5 chars */
        const char *mixed = "Hi \xE4\xB8\x96\xE7\x95\x8C";
        assert(StrUtf8ValidCount(mixed, strlen(mixed), &valid) == 5 &&
               valid == true);

        /* With emoji: "A😀B" = 3 chars */
        const char *emoji = "A\xF0\x9F\x98\x80"
                            "B";
        assert(StrUtf8ValidCount(emoji, strlen(emoji), &valid) == 3 &&
               valid == true);

        /* Invalid - should stop and return false */
        const uint8_t badSeq[] = {'a', 'b', 0xC0, 0x80, 'c'};
        size_t count = StrUtf8ValidCount(badSeq, 5, &valid);
        assert(valid == false);
        assert(count == 2); /* Counted 'a', 'b' before hitting invalid */
    }

    TEST("StrUtf8ValidCountBytes: Get byte length for N codepoints") {
        bool valid;

        /* ASCII: 3 chars = 3 bytes */
        assert(StrUtf8ValidCountBytes("Hello", 5, 3, &valid) == 3 &&
               valid == true);

        /* Mixed: "世界Hi" - 2 Chinese (6 bytes) + 1 ASCII (1 byte) = 7 bytes */
        const char *mixed = "\xE4\xB8\x96\xE7\x95\x8CHi";
        assert(StrUtf8ValidCountBytes(mixed, strlen(mixed), 3, &valid) == 7 &&
               valid == true);

        /* With emoji: "😀A" - 1 emoji (4 bytes) = 4 bytes for 1 char */
        const char *emoji = "\xF0\x9F\x98\x80"
                            "A";
        assert(StrUtf8ValidCountBytes(emoji, strlen(emoji), 1, &valid) == 4 &&
               valid == true);
    }

    TEST("StrUtf8Encode/Decode roundtrip") {
        uint8_t buf[4];

        /* Test various codepoints */
        uint32_t testCps[] = {
            0x00,     /* NUL */
            0x41,     /* 'A' */
            0x7F,     /* DEL (max 1-byte) */
            0x80,     /* min 2-byte */
            0xFF,     /* Latin-1 ÿ */
            0x7FF,    /* max 2-byte */
            0x800,    /* min 3-byte */
            0x4E2D,   /* 中 */
            0xD7FF,   /* just before surrogates */
            0xE000,   /* just after surrogates */
            0xFFFD,   /* replacement char */
            0xFFFF,   /* max 3-byte */
            0x10000,  /* min 4-byte */
            0x1F600,  /* 😀 */
            0x10FFFF, /* max valid codepoint */
        };

        for (size_t i = 0; i < countof(testCps); i++) {
            uint32_t cp = testCps[i];

            /* Encode */
            size_t encLen = StrUtf8Encode(buf, cp);
            assert(encLen > 0 && encLen <= 4);

            /* Validate the encoded bytes */
            assert(StrUtf8Valid(buf, encLen) == true);

            /* Decode and verify round-trip */
            const uint8_t *decPtr = buf;
            size_t remaining = encLen;
            uint32_t decoded = StrUtf8Decode(&decPtr, &remaining);
            assert(decoded == cp);
            assert(remaining == 0);
        }

        /* Test surrogates are rejected */
        assert(StrUtf8Encode(buf, 0xD800) == 0);
        assert(StrUtf8Encode(buf, 0xDFFF) == 0);

        /* Test too-large codepoints rejected */
        assert(StrUtf8Encode(buf, 0x110000) == 0);
        assert(StrUtf8Encode(buf, 0xFFFFFFFF) == 0);
    }

    TEST("StrUtf8SequenceLen: First byte classification") {
        /* ASCII: 1 byte */
        for (int i = 0; i < 0x80; i++) {
            assert(StrUtf8SequenceLen((uint8_t)i) == 1);
        }

        /* Continuation bytes: 0 (invalid as start) */
        for (int i = 0x80; i < 0xC0; i++) {
            assert(StrUtf8SequenceLen((uint8_t)i) == 0);
        }

        /* Overlong indicators: 0 (invalid) */
        assert(StrUtf8SequenceLen(0xC0) == 0);
        assert(StrUtf8SequenceLen(0xC1) == 0);

        /* Valid 2-byte: C2-DF */
        for (int i = 0xC2; i <= 0xDF; i++) {
            assert(StrUtf8SequenceLen((uint8_t)i) == 2);
        }

        /* Valid 3-byte: E0-EF */
        for (int i = 0xE0; i <= 0xEF; i++) {
            assert(StrUtf8SequenceLen((uint8_t)i) == 3);
        }

        /* Valid 4-byte: F0-F4 */
        for (int i = 0xF0; i <= 0xF4; i++) {
            assert(StrUtf8SequenceLen((uint8_t)i) == 4);
        }

        /* Invalid (> U+10FFFF): F5-FF */
        for (int i = 0xF5; i <= 0xFF; i++) {
            assert(StrUtf8SequenceLen((uint8_t)i) == 0);
        }
    }

    TEST("StrUtf8CodepointLen") {
        /* 1-byte range */
        assert(StrUtf8CodepointLen(0) == 1);
        assert(StrUtf8CodepointLen(0x7F) == 1);

        /* 2-byte range */
        assert(StrUtf8CodepointLen(0x80) == 2);
        assert(StrUtf8CodepointLen(0x7FF) == 2);

        /* 3-byte range */
        assert(StrUtf8CodepointLen(0x800) == 3);
        assert(StrUtf8CodepointLen(0xD7FF) == 3);
        assert(StrUtf8CodepointLen(0xE000) == 3);
        assert(StrUtf8CodepointLen(0xFFFF) == 3);

        /* Surrogates: 0 (invalid) */
        assert(StrUtf8CodepointLen(0xD800) == 0);
        assert(StrUtf8CodepointLen(0xDBFF) == 0);
        assert(StrUtf8CodepointLen(0xDC00) == 0);
        assert(StrUtf8CodepointLen(0xDFFF) == 0);

        /* 4-byte range */
        assert(StrUtf8CodepointLen(0x10000) == 4);
        assert(StrUtf8CodepointLen(0x10FFFF) == 4);

        /* Too large: 0 (invalid) */
        assert(StrUtf8CodepointLen(0x110000) == 0);
    }

    TEST("StrUtf8Valid vs StrUtf8ValidScalar stress test") {
        printf("  Testing StrUtf8Valid matches baseline...\n");

        /* Test valid strings of various sizes */
        for (size_t size = 0; size <= 256; size++) {
            cleanerName(zfree, Chars) char *buf = zmalloc(size + 1);
            for (size_t i = 0; i < size; i++) {
                buf[i] = 'A' + (i % 26);
            }
            buf[size] = '\0';

            bool fastResult = StrUtf8Valid(buf, size);
            bool baseResult = StrUtf8ValidScalar(buf, size);
            if (fastResult != baseResult) {
                ERR("StrUtf8Valid mismatch at size %zu (ASCII): fast=%d "
                    "base=%d",
                    size, fastResult, baseResult);
            }
        }

        /* Test with mixed valid UTF-8 at various sizes */
        /* "日" = E6 97 A5 (3 bytes) */
        for (size_t numChars = 0; numChars <= 64; numChars++) {
            size_t size = numChars * 3;
            cleanerName(zfree, U8) uint8_t *buf = zmalloc(size + 1);
            for (size_t i = 0; i < numChars; i++) {
                buf[i * 3 + 0] = 0xE6;
                buf[i * 3 + 1] = 0x97;
                buf[i * 3 + 2] = 0xA5;
            }
            buf[size] = '\0';

            bool fastResult = StrUtf8Valid(buf, size);
            bool baseResult = StrUtf8ValidScalar(buf, size);
            if (fastResult != baseResult) {
                ERR("StrUtf8Valid mismatch at size %zu (Chinese): fast=%d "
                    "base=%d",
                    size, fastResult, baseResult);
            }
            assert(fastResult == true);
        }

        /* Test with single invalid byte at various positions */
        for (size_t size = 1; size <= 128; size++) {
            for (size_t badPos = 0; badPos < size; badPos++) {
                cleanerName(zfree, U8) uint8_t *buf = zmalloc(size + 1);
                for (size_t i = 0; i < size; i++) {
                    buf[i] = 'a';
                }
                buf[badPos] = 0x80; /* Invalid: continuation without start */
                buf[size] = '\0';

                bool fastResult = StrUtf8Valid(buf, size);
                bool baseResult = StrUtf8ValidScalar(buf, size);
                if (fastResult != baseResult) {
                    ERR("StrUtf8Valid mismatch at size %zu, badPos %zu: "
                        "fast=%d base=%d",
                        size, badPos, fastResult, baseResult);
                }
                assert(fastResult == false);
            }
        }
    }

    TEST("Benchmark: StrUtf8Valid vs StrUtf8ValidScalar") {
        printf("  Comparing UTF-8 validation performance...\n");

        /* ASCII-heavy string */
        const char *asciiHeavy = "The quick brown fox jumps over the lazy dog. "
                                 "The quick brown fox jumps over the lazy dog. "
                                 "The quick brown fox jumps over the lazy dog.";
        const size_t asciiLen = strlen(asciiHeavy);
        const size_t iterations = 2'000'000;
        bool resultOpt = false;
        bool resultScalar = false;

        /* SIMD version on ASCII */
        TIME_INIT;
        for (size_t i = 0; i < iterations; i++) {
            resultOpt = StrUtf8Valid(asciiHeavy, asciiLen);
        }
        TIME_FINISH(iterations, "SIMD (ASCII)");

        /* Scalar version on ASCII */
        TIME_INIT;
        for (size_t i = 0; i < iterations; i++) {
            resultScalar = StrUtf8ValidScalar(asciiHeavy, asciiLen);
        }
        TIME_FINISH(iterations, "Scalar (ASCII)");

        if (resultOpt != resultScalar) {
            ERR("Mismatch on ASCII! SIMD=%d Scalar=%d", resultOpt,
                resultScalar);
        }

        /* Mixed UTF-8 string */
        const char *mixedUtf8 = "Hello 世界! 🎉 Testing UTF-8 日本語";
        const size_t mixedLen = strlen(mixedUtf8);

        /* SIMD version on mixed */
        TIME_INIT;
        for (size_t i = 0; i < iterations; i++) {
            resultOpt = StrUtf8Valid(mixedUtf8, mixedLen);
        }
        TIME_FINISH(iterations, "SIMD (Mixed)");

        /* Scalar version on mixed */
        TIME_INIT;
        for (size_t i = 0; i < iterations; i++) {
            resultScalar = StrUtf8ValidScalar(mixedUtf8, mixedLen);
        }
        TIME_FINISH(iterations, "Scalar (Mixed)");

        if (resultOpt != resultScalar) {
            ERR("Mismatch on mixed! SIMD=%d Scalar=%d", resultOpt,
                resultScalar);
        }

        printf("    ASCII: %zu bytes, Mixed: %zu bytes (both valid)\n",
               asciiLen, mixedLen);
    }

    /* ====================================================================
     * UTF-8 Cursor Operations Tests
     * ==================================================================== */
    TEST("StrUtf8Advance - basic tests") {
        /* ASCII string */
        const char *ascii = "Hello, World!";
        size_t asciiLen = strlen(ascii);

        /* Advance by 0 should return start */
        const uint8_t *pos = StrUtf8Advance(ascii, asciiLen, 0);
        assert(pos == (const uint8_t *)ascii);

        /* Advance by 1 */
        pos = StrUtf8Advance(ascii, asciiLen, 1);
        assert(pos == (const uint8_t *)ascii + 1);

        /* Advance by 5 */
        pos = StrUtf8Advance(ascii, asciiLen, 5);
        assert(pos == (const uint8_t *)ascii + 5);

        /* Advance past end should stop at end */
        pos = StrUtf8Advance(ascii, asciiLen, 100);
        assert(pos == (const uint8_t *)ascii + asciiLen);

        /* Empty string */
        pos = StrUtf8Advance("", 0, 5);
        assert(*pos == '\0');
    }

    TEST("StrUtf8Advance - multibyte sequences") {
        /* "Hello 世界!" - 9 codepoints, 13 bytes */
        /* H=1, e=1, l=1, l=1, o=1, space=1, 世=3, 界=3, !=1 */
        const char *mixed = "Hello 世界!";
        size_t mixedLen = strlen(mixed);
        assert(mixedLen == 13);

        /* Advance to '世' (6th codepoint, 0-indexed: 6) */
        const uint8_t *pos = StrUtf8Advance(mixed, mixedLen, 6);
        assert(pos == (const uint8_t *)mixed + 6); /* 6 ASCII bytes */

        /* Advance to '界' (7th codepoint) */
        pos = StrUtf8Advance(mixed, mixedLen, 7);
        assert(pos == (const uint8_t *)mixed + 9); /* 6 + 3 bytes */

        /* Advance to '!' (8th codepoint) */
        pos = StrUtf8Advance(mixed, mixedLen, 8);
        assert(pos == (const uint8_t *)mixed + 12); /* 6 + 3 + 3 bytes */

        /* Count total codepoints */
        size_t count = StrLenUtf8(mixed, mixedLen);
        assert(count == 9); /* Hello(5) + space(1) + 世界(2) + !(1) = 9 */
    }

    TEST("StrUtf8Advance - 4-byte emoji") {
        /* "Hi 👋!" - 5 codepoints, 9 bytes */
        /* H=1, i=1, space=1, 👋=4, !=1 */
        const char *emoji = "Hi 👋!";
        size_t emojiLen = strlen(emoji);

        assert(StrLenUtf8(emoji, emojiLen) == 5);

        /* Advance to emoji (3rd codepoint) */
        const uint8_t *pos = StrUtf8Advance(emoji, emojiLen, 3);
        assert(pos == (const uint8_t *)emoji + 3);

        /* Advance past emoji (4th codepoint = '!') */
        pos = StrUtf8Advance(emoji, emojiLen, 4);
        assert(pos == (const uint8_t *)emoji + 7); /* 3 + 4 bytes */
    }

    TEST("StrUtf8Retreat - basic tests") {
        const char *ascii = "Hello, World!";
        size_t asciiLen = strlen(ascii);
        const uint8_t *start = (const uint8_t *)ascii;
        const uint8_t *end = start + asciiLen;

        /* Retreat by 0 from end should stay at end */
        const uint8_t *pos = StrUtf8Retreat(ascii, asciiLen, end, 0);
        assert(pos == end);

        /* Retreat by 1 from end */
        pos = StrUtf8Retreat(ascii, asciiLen, end, 1);
        assert(pos == end - 1);
        assert(*pos == '!');

        /* Retreat by 5 from end */
        pos = StrUtf8Retreat(ascii, asciiLen, end, 5);
        assert(pos == end - 5);

        /* Retreat past start should stop at start */
        pos = StrUtf8Retreat(ascii, asciiLen, end, 100);
        assert(pos == start);
    }

    TEST("StrUtf8Retreat - multibyte sequences") {
        /* "AB世界CD" - 6 codepoints */
        const char *mixed = "AB世界CD";
        size_t mixedLen = strlen(mixed);
        const uint8_t *start = (const uint8_t *)mixed;
        const uint8_t *end = start + mixedLen;

        /* Retreat by 1 from end - should land on 'D' */
        const uint8_t *pos = StrUtf8Retreat(mixed, mixedLen, end, 1);
        assert(*pos == 'D');

        /* Retreat by 2 from end - should land on 'C' */
        pos = StrUtf8Retreat(mixed, mixedLen, end, 2);
        assert(*pos == 'C');

        /* Retreat by 3 from end - should land on '界' */
        pos = StrUtf8Retreat(mixed, mixedLen, end, 3);
        /* '界' starts at byte 5 (A=1, B=1, 世=3 = 5) */
        assert(pos == start + 5);

        /* Retreat by 4 from end - should land on '世' */
        pos = StrUtf8Retreat(mixed, mixedLen, end, 4);
        assert(pos == start + 2);

        /* Retreat by 5 from end - should land on 'B' */
        pos = StrUtf8Retreat(mixed, mixedLen, end, 5);
        assert(pos == start + 1);
        assert(*pos == 'B');
    }

    TEST("StrUtf8Peek - basic tests") {
        const char *ascii = "Hello";
        size_t asciiLen = strlen(ascii);
        const uint8_t *start = (const uint8_t *)ascii;

        /* Peek at first character */
        uint32_t cp = StrUtf8Peek(ascii, asciiLen, start);
        assert(cp == 'H');

        /* Peek at second character */
        cp = StrUtf8Peek(ascii, asciiLen, start + 1);
        assert(cp == 'e');

        /* Peek at last character */
        cp = StrUtf8Peek(ascii, asciiLen, start + 4);
        assert(cp == 'o');

        /* Peek past end returns error */
        cp = StrUtf8Peek(ascii, asciiLen, start + 5);
        assert(cp == 0xFFFFFFFF);

        /* Peek with NULL returns error */
        cp = StrUtf8Peek(ascii, asciiLen, nullptr);
        assert(cp == 0xFFFFFFFF);
    }

    TEST("StrUtf8Peek - multibyte") {
        /* "日本" - 2 codepoints, 6 bytes */
        /* 日 = U+65E5, 本 = U+672C */
        const char *jp = "日本";
        size_t jpLen = strlen(jp);
        const uint8_t *start = (const uint8_t *)jp;

        uint32_t cp = StrUtf8Peek(jp, jpLen, start);
        assert(cp == 0x65E5); /* 日 */

        cp = StrUtf8Peek(jp, jpLen, start + 3);
        assert(cp == 0x672C); /* 本 */
    }

    TEST("StrUtf8OffsetAt - basic tests") {
        /* ASCII */
        const char *ascii = "Hello";
        size_t asciiLen = strlen(ascii);

        assert(StrUtf8OffsetAt(ascii, asciiLen, 0) == 0);
        assert(StrUtf8OffsetAt(ascii, asciiLen, 1) == 1);
        assert(StrUtf8OffsetAt(ascii, asciiLen, 4) == 4);
        assert(StrUtf8OffsetAt(ascii, asciiLen, 5) == 5);
        assert(StrUtf8OffsetAt(ascii, asciiLen, 10) == 5); /* past end */

        /* Multibyte: "A世B" = A(1) + 世(3) + B(1) = 5 bytes, 3 chars */
        const char *mixed = "A世B";
        size_t mixedLen = strlen(mixed);

        assert(StrUtf8OffsetAt(mixed, mixedLen, 0) == 0); /* A */
        assert(StrUtf8OffsetAt(mixed, mixedLen, 1) == 1); /* 世 */
        assert(StrUtf8OffsetAt(mixed, mixedLen, 2) == 4); /* B */
        assert(StrUtf8OffsetAt(mixed, mixedLen, 3) == 5); /* end */
    }

    TEST("StrUtf8IndexAt - basic tests") {
        /* ASCII */
        const char *ascii = "Hello";
        size_t asciiLen = strlen(ascii);

        assert(StrUtf8IndexAt(ascii, asciiLen, 0) == 0);
        assert(StrUtf8IndexAt(ascii, asciiLen, 1) == 1);
        assert(StrUtf8IndexAt(ascii, asciiLen, 4) == 4);
        assert(StrUtf8IndexAt(ascii, asciiLen, 5) == 5);
        assert(StrUtf8IndexAt(ascii, asciiLen, 10) == 5); /* past end */

        /* Multibyte: "A世B" = A(1) + 世(3) + B(1) = 5 bytes, 3 chars */
        /* StrUtf8IndexAt counts codepoints (start bytes) in first N bytes */
        const char *mixed = "A世B";
        size_t mixedLen = strlen(mixed);

        assert(StrUtf8IndexAt(mixed, mixedLen, 0) == 0); /* empty */
        assert(StrUtf8IndexAt(mixed, mixedLen, 1) == 1); /* A = 1 codepoint */
        assert(StrUtf8IndexAt(mixed, mixedLen, 2) == 2); /* A + 世start = 2 */
        assert(StrUtf8IndexAt(mixed, mixedLen, 3) == 2); /* A + 世 partial */
        assert(StrUtf8IndexAt(mixed, mixedLen, 4) == 2); /* A + 世 complete */
        assert(StrUtf8IndexAt(mixed, mixedLen, 5) == 3); /* A + 世 + B */
    }

    TEST("StrUtf8Advance vs StrUtf8AdvanceScalar") {
        /* Ensure SIMD and scalar versions produce identical results */
        const char *testStrings[] = {
            "Hello, World!",
            "日本語テスト",
            "Hi 👋 there! 🎉",
            "Mixed: ABC日本XYZ世界123",
            "",
        };

        for (size_t i = 0; i < countof(testStrings); i++) {
            const char *str = testStrings[i];
            size_t len = strlen(str);
            size_t charCount = StrLenUtf8(str, len);

            for (size_t n = 0; n <= charCount + 5; n++) {
                const uint8_t *simd = StrUtf8Advance(str, len, n);
                const uint8_t *scalar = StrUtf8AdvanceScalar(str, len, n);
                if (simd != scalar) {
                    ERR("StrUtf8Advance mismatch at string %zu, n=%zu: "
                        "simd=%p scalar=%p",
                        i, n, simd, scalar);
                }
            }
        }
    }

    TEST("Benchmark: StrUtf8Advance vs StrUtf8AdvanceScalar") {
        printf("  Comparing UTF-8 cursor advance performance...\n");

        /* ASCII-heavy string */
        const char *asciiHeavy = "The quick brown fox jumps over the lazy dog. "
                                 "The quick brown fox jumps over the lazy dog.";
        const size_t asciiLen = strlen(asciiHeavy);
        const size_t iterations = 1'000'000;
        [[maybe_unused]] const uint8_t *result;

        /* SIMD version */
        TIME_INIT;
        for (size_t i = 0; i < iterations; i++) {
            result = StrUtf8Advance(asciiHeavy, asciiLen, 45);
        }
        TIME_FINISH(iterations, "SIMD Advance (ASCII)");

        /* Scalar version */
        TIME_INIT;
        for (size_t i = 0; i < iterations; i++) {
            result = StrUtf8AdvanceScalar(asciiHeavy, asciiLen, 45);
        }
        TIME_FINISH(iterations, "Scalar Advance (ASCII)");

        /* Mixed UTF-8 string */
        const char *mixedUtf8 = "Hello 世界! 日本語 🎉 Testing UTF-8";
        const size_t mixedLen = strlen(mixedUtf8);
        const size_t mixedChars = StrLenUtf8(mixedUtf8, mixedLen);

        /* SIMD version on mixed */
        TIME_INIT;
        for (size_t i = 0; i < iterations; i++) {
            result = StrUtf8Advance(mixedUtf8, mixedLen, mixedChars / 2);
        }
        TIME_FINISH(iterations, "SIMD Advance (Mixed)");

        /* Scalar version on mixed */
        TIME_INIT;
        for (size_t i = 0; i < iterations; i++) {
            result = StrUtf8AdvanceScalar(mixedUtf8, mixedLen, mixedChars / 2);
        }
        TIME_FINISH(iterations, "Scalar Advance (Mixed)");

        printf("    ASCII: %zu bytes, Mixed: %zu bytes (%zu chars)\n", asciiLen,
               mixedLen, mixedChars);
    }

    /* ====================================================================
     * UTF-8 Truncation/Substring Tests
     * ==================================================================== */
    TEST("StrUtf8Truncate - basic tests") {
        /* ASCII string */
        const char *ascii = "Hello, World!";
        size_t asciiLen = strlen(ascii);

        assert(StrUtf8Truncate(ascii, asciiLen, 0) == 0);
        assert(StrUtf8Truncate(ascii, asciiLen, 5) == 5);
        assert(StrUtf8Truncate(ascii, asciiLen, 13) == 13);
        assert(StrUtf8Truncate(ascii, asciiLen, 100) == 13); /* past end */

        /* Multibyte: "Hello 世界!" = 9 codepoints, 13 bytes */
        const char *mixed = "Hello 世界!";
        size_t mixedLen = strlen(mixed);

        assert(StrUtf8Truncate(mixed, mixedLen, 0) == 0);
        assert(StrUtf8Truncate(mixed, mixedLen, 6) == 6);  /* "Hello " */
        assert(StrUtf8Truncate(mixed, mixedLen, 7) == 9);  /* "Hello 世" */
        assert(StrUtf8Truncate(mixed, mixedLen, 8) == 12); /* "Hello 世界" */
        assert(StrUtf8Truncate(mixed, mixedLen, 9) == 13); /* full string */
    }

    TEST("StrUtf8TruncateBytes - basic tests") {
        /* ASCII - all positions are valid boundaries */
        const char *ascii = "Hello";
        size_t asciiLen = strlen(ascii);

        assert(StrUtf8TruncateBytes(ascii, asciiLen, 0) == 0);
        assert(StrUtf8TruncateBytes(ascii, asciiLen, 3) == 3);
        assert(StrUtf8TruncateBytes(ascii, asciiLen, 100) == 5);

        /* Multibyte: "A世B" = A(1) + 世(3) + B(1) = 5 bytes */
        const char *mixed = "A世B";
        size_t mixedLen = strlen(mixed);

        assert(StrUtf8TruncateBytes(mixed, mixedLen, 0) == 0);
        assert(StrUtf8TruncateBytes(mixed, mixedLen, 1) == 1); /* "A" valid */
        assert(StrUtf8TruncateBytes(mixed, mixedLen, 2) ==
               1); /* mid-世, back to "A" */
        assert(StrUtf8TruncateBytes(mixed, mixedLen, 3) ==
               1); /* mid-世, back to "A" */
        assert(StrUtf8TruncateBytes(mixed, mixedLen, 4) == 4); /* "A世" valid */
        assert(StrUtf8TruncateBytes(mixed, mixedLen, 5) == 5); /* full string */
    }

    TEST("StrUtf8Substring - basic tests") {
        /* ASCII */
        const char *ascii = "Hello, World!";
        size_t asciiLen = strlen(ascii);
        size_t offset, len;

        /* Extract "Hello" (0 to 5) */
        StrUtf8Substring(ascii, asciiLen, 0, 5, &offset, &len);
        assert(offset == 0 && len == 5);

        /* Extract "World" (7 to 12) */
        StrUtf8Substring(ascii, asciiLen, 7, 12, &offset, &len);
        assert(offset == 7 && len == 5);

        /* Extract to end using SIZE_MAX */
        StrUtf8Substring(ascii, asciiLen, 7, SIZE_MAX, &offset, &len);
        assert(offset == 7 && len == 6); /* "World!" */

        /* Empty range */
        StrUtf8Substring(ascii, asciiLen, 5, 5, &offset, &len);
        assert(len == 0);

        /* Invalid range (start > end) */
        StrUtf8Substring(ascii, asciiLen, 5, 3, &offset, &len);
        assert(len == 0);

        /* Past end */
        StrUtf8Substring(ascii, asciiLen, 100, 105, &offset, &len);
        assert(offset == 13 && len == 0);
    }

    TEST("StrUtf8Substring - multibyte") {
        /* "Hello 世界!" = 9 codepoints, 13 bytes */
        const char *mixed = "Hello 世界!";
        size_t mixedLen = strlen(mixed);
        size_t offset, len;

        /* Extract "世界" (chars 6-8) */
        StrUtf8Substring(mixed, mixedLen, 6, 8, &offset, &len);
        assert(offset == 6); /* starts after "Hello " */
        assert(len == 6);    /* 世(3) + 界(3) = 6 bytes */

        /* Extract "Hello " (chars 0-6) */
        StrUtf8Substring(mixed, mixedLen, 0, 6, &offset, &len);
        assert(offset == 0 && len == 6);

        /* Extract "世界!" to end (chars 6-end) */
        StrUtf8Substring(mixed, mixedLen, 6, SIZE_MAX, &offset, &len);
        assert(offset == 6 && len == 7); /* 世(3) + 界(3) + !(1) = 7 */
    }

    TEST("StrUtf8SubstringCopy - basic tests") {
        const char *mixed = "Hello 世界!";
        size_t mixedLen = strlen(mixed);
        char buf[32];

        /* Copy "世界" */
        size_t written =
            StrUtf8SubstringCopy(mixed, mixedLen, 6, 8, buf, sizeof(buf));
        assert(written == 6);
        assert(memcmp(buf, "世界", 6) == 0);
        assert(buf[6] == '\0');

        /* Query mode (bufLen = 0) */
        written = StrUtf8SubstringCopy(mixed, mixedLen, 6, 8, nullptr, 0);
        assert(written == 6);

        /* Buffer too small - should truncate at valid boundary */
        written = StrUtf8SubstringCopy(mixed, mixedLen, 6, 8, buf, 5);
        /* 世 is 3 bytes, 界 is 3 bytes. Buffer can hold 4 chars + null.
         * So we can fit 世 (3 bytes), but not 世界 (6 bytes).
         * Truncation at byte 4 is mid-界, so back up to byte 3 (after 世). */
        assert(written == 3);
        assert(memcmp(buf, "世", 3) == 0);
        assert(buf[3] == '\0');
    }

    TEST("StrUtf8Split - basic tests") {
        const char *mixed = "Hello 世界!";
        size_t mixedLen = strlen(mixed);

        /* Split at various positions */
        assert(StrUtf8Split(mixed, mixedLen, 0) == 0);
        assert(StrUtf8Split(mixed, mixedLen, 5) == 5);  /* "Hello" */
        assert(StrUtf8Split(mixed, mixedLen, 6) == 6);  /* "Hello " */
        assert(StrUtf8Split(mixed, mixedLen, 7) == 9);  /* "Hello 世" */
        assert(StrUtf8Split(mixed, mixedLen, 9) == 13); /* full string */
    }

    /* ====================================================================
     * UTF-8 String Comparison Tests
     * ==================================================================== */
    TEST("StrUtf8Compare - basic tests") {
        /* Equal strings */
        assert(StrUtf8Compare("hello", 5, "hello", 5) == 0);
        assert(StrUtf8Compare("", 0, "", 0) == 0);

        /* Less than */
        assert(StrUtf8Compare("abc", 3, "abd", 3) < 0);
        assert(StrUtf8Compare("abc", 3, "abcd", 4) < 0);
        assert(StrUtf8Compare("", 0, "a", 1) < 0);

        /* Greater than */
        assert(StrUtf8Compare("abd", 3, "abc", 3) > 0);
        assert(StrUtf8Compare("abcd", 4, "abc", 3) > 0);
        assert(StrUtf8Compare("a", 1, "", 0) > 0);

        /* UTF-8 strings */
        const char *hello = "hello";
        const char *utf8_1 = "héllo";                     /* é = 0xC3 0xA9 */
        const char *utf8_2 = "hëllo";                     /* ë = 0xC3 0xAB */
        assert(StrUtf8Compare(hello, 5, utf8_1, 6) < 0);  /* 'e' < 0xC3 */
        assert(StrUtf8Compare(utf8_1, 6, utf8_2, 6) < 0); /* 0xA9 < 0xAB */
    }

    TEST("StrUtf8CompareN - codepoint-limited comparison") {
        /* Compare first N codepoints */
        assert(StrUtf8CompareN("hello", 5, "helloworld", 10, 5) == 0);
        assert(StrUtf8CompareN("abc", 3, "abd", 3, 2) == 0); /* First 2 match */
        assert(StrUtf8CompareN("abc", 3, "abd", 3, 3) < 0);  /* Third differs */

        /* UTF-8: 世界 vs 世人 */
        const char *s1 = "世界"; /* 2 codepoints, 6 bytes */
        const char *s2 = "世人"; /* 2 codepoints, 6 bytes */
        assert(StrUtf8CompareN(s1, 6, s2, 6, 1) ==
               0); /* First codepoint same */
        assert(StrUtf8CompareN(s1, 6, s2, 6, 2) !=
               0); /* Second codepoint differs */

        /* n=0 always equal */
        assert(StrUtf8CompareN("a", 1, "z", 1, 0) == 0);
    }

    TEST("StrUtf8CompareCaseInsensitiveAscii - ASCII case folding") {
        /* Equal with different cases */
        assert(StrUtf8CompareCaseInsensitiveAscii("HELLO", 5, "hello", 5) == 0);
        assert(StrUtf8CompareCaseInsensitiveAscii("HeLLo", 5, "hEllO", 5) == 0);
        assert(StrUtf8CompareCaseInsensitiveAscii("ABC", 3, "abc", 3) == 0);

        /* Not equal */
        assert(StrUtf8CompareCaseInsensitiveAscii("ABC", 3, "ABD", 3) < 0);
        assert(StrUtf8CompareCaseInsensitiveAscii("ABD", 3, "ABC", 3) > 0);

        /* Length differences */
        assert(StrUtf8CompareCaseInsensitiveAscii("ABC", 3, "ABCD", 4) < 0);
        assert(StrUtf8CompareCaseInsensitiveAscii("ABCD", 4, "ABC", 3) > 0);

        /* Non-ASCII unchanged */
        const char *upper_a = "Ä"; /* U+00C4, not converted */
        const char *lower_a = "ä"; /* U+00E4, not converted */
        assert(StrUtf8CompareCaseInsensitiveAscii(upper_a, 2, lower_a, 2) != 0);
    }

    TEST("StrUtf8StartsWith - prefix matching") {
        const char *str = "Hello 世界!";
        size_t strLen = strlen(str);

        /* ASCII prefix */
        assert(StrUtf8StartsWith(str, strLen, "Hello", 5) == true);
        assert(StrUtf8StartsWith(str, strLen, "H", 1) == true);
        assert(StrUtf8StartsWith(str, strLen, "", 0) == true);
        assert(StrUtf8StartsWith(str, strLen, "hello", 5) == false);

        /* UTF-8 prefix */
        assert(StrUtf8StartsWith(str, strLen, "Hello ", 6) == true);
        assert(StrUtf8StartsWith(str, strLen, "Hello 世", 9) == true);
        assert(StrUtf8StartsWith(str, strLen, "Hello 世界", 12) == true);

        /* Full string as prefix */
        assert(StrUtf8StartsWith(str, strLen, str, strLen) == true);

        /* Prefix longer than string */
        assert(StrUtf8StartsWith("Hi", 2, "Hello", 5) == false);
    }

    TEST("StrUtf8EndsWith - suffix matching") {
        const char *str = "Hello 世界!";
        size_t strLen = strlen(str);

        /* ASCII suffix */
        assert(StrUtf8EndsWith(str, strLen, "!", 1) == true);
        assert(StrUtf8EndsWith(str, strLen, "", 0) == true);

        /* UTF-8 suffix */
        assert(StrUtf8EndsWith(str, strLen, "界!", 4) == true);
        assert(StrUtf8EndsWith(str, strLen, "世界!", 7) == true);

        /* Full string as suffix */
        assert(StrUtf8EndsWith(str, strLen, str, strLen) == true);

        /* Suffix longer than string */
        assert(StrUtf8EndsWith("Hi", 2, "Hello", 5) == false);

        /* Non-matching suffix */
        assert(StrUtf8EndsWith(str, strLen, "世", 3) == false);
    }

    TEST("StrUtf8Equal - equality tests") {
        assert(StrUtf8Equal("hello", 5, "hello", 5) == true);
        assert(StrUtf8Equal("", 0, "", 0) == true);
        assert(StrUtf8Equal("hello", 5, "hello!", 6) == false);
        assert(StrUtf8Equal("hello", 5, "HELLO", 5) == false);

        /* UTF-8 */
        const char *utf8 = "世界";
        assert(StrUtf8Equal(utf8, 6, utf8, 6) == true);
        assert(StrUtf8Equal(utf8, 6, "世人", 6) == false);
    }

    TEST("StrUtf8EqualCaseInsensitiveAscii - case-insensitive equality") {
        assert(StrUtf8EqualCaseInsensitiveAscii("HELLO", 5, "hello", 5) ==
               true);
        assert(StrUtf8EqualCaseInsensitiveAscii("HeLLo", 5, "hEllO", 5) ==
               true);
        assert(StrUtf8EqualCaseInsensitiveAscii("ABC", 3, "abc", 3) == true);
        assert(StrUtf8EqualCaseInsensitiveAscii("ABC", 3, "ABD", 3) == false);
        assert(StrUtf8EqualCaseInsensitiveAscii("ABC", 3, "ABCD", 4) == false);
    }

    /* ====================================================================
     * UTF-8 Search Tests
     * ==================================================================== */
    TEST("StrUtf8Find - basic substring search") {
        const char *str = "Hello, World!";
        size_t len = strlen(str);

        /* Find existing substring */
        assert(StrUtf8Find(str, len, "World", 5) == 7);
        assert(StrUtf8Find(str, len, "Hello", 5) == 0);
        assert(StrUtf8Find(str, len, "!", 1) == 12);

        /* Not found */
        assert(StrUtf8Find(str, len, "world", 5) ==
               SIZE_MAX); /* Case sensitive */
        assert(StrUtf8Find(str, len, "xyz", 3) == SIZE_MAX);

        /* Empty needle */
        assert(StrUtf8Find(str, len, "", 0) == 0);

        /* Needle longer than haystack */
        assert(StrUtf8Find("Hi", 2, "Hello", 5) == SIZE_MAX);
    }

    TEST("StrUtf8Find - UTF-8 substring search") {
        const char *str = "Hello 世界!";
        size_t len = strlen(str);

        assert(StrUtf8Find(str, len, "世界", 6) == 6);
        assert(StrUtf8Find(str, len, "世", 3) == 6);
        assert(StrUtf8Find(str, len, "界", 3) == 9);
        assert(StrUtf8Find(str, len, "人", 3) == SIZE_MAX);
    }

    TEST("StrUtf8FindLast - find last occurrence") {
        const char *str = "abcabc";
        size_t len = strlen(str);

        assert(StrUtf8FindLast(str, len, "abc", 3) == 3);
        assert(StrUtf8FindLast(str, len, "a", 1) == 3);
        assert(StrUtf8FindLast(str, len, "c", 1) == 5);
        assert(StrUtf8FindLast(str, len, "", 0) == len);
        assert(StrUtf8FindLast(str, len, "xyz", 3) == SIZE_MAX);
    }

    TEST("StrUtf8FindChar - find codepoint") {
        const char *str = "Hello 世界!";
        size_t len = strlen(str);

        assert(StrUtf8FindChar(str, len, 'H') == 0);
        assert(StrUtf8FindChar(str, len, '!') == 12);
        assert(StrUtf8FindChar(str, len, 0x4E16) == 6); /* 世 */
        assert(StrUtf8FindChar(str, len, 0x754C) == 9); /* 界 */
        assert(StrUtf8FindChar(str, len, 'x') == SIZE_MAX);
    }

    TEST("StrUtf8FindCharLast - find last codepoint") {
        const char *str = "ababa";
        size_t len = strlen(str);

        assert(StrUtf8FindCharLast(str, len, 'a') == 4);
        assert(StrUtf8FindCharLast(str, len, 'b') == 3);
        assert(StrUtf8FindCharLast(str, len, 'c') == SIZE_MAX);
    }

    TEST("StrUtf8FindCharNth - find Nth occurrence") {
        const char *str = "ababab";
        size_t len = strlen(str);

        assert(StrUtf8FindCharNth(str, len, 'a', 0) == 0);
        assert(StrUtf8FindCharNth(str, len, 'a', 1) == 2);
        assert(StrUtf8FindCharNth(str, len, 'a', 2) == 4);
        assert(StrUtf8FindCharNth(str, len, 'a', 3) == SIZE_MAX);
        assert(StrUtf8FindCharNth(str, len, 'b', 0) == 1);
    }

    TEST("StrUtf8Contains - substring exists") {
        const char *str = "Hello, World!";
        size_t len = strlen(str);

        assert(StrUtf8Contains(str, len, "World", 5) == true);
        assert(StrUtf8Contains(str, len, "Hello", 5) == true);
        assert(StrUtf8Contains(str, len, "xyz", 3) == false);
        assert(StrUtf8Contains(str, len, "", 0) == true);
    }

    TEST("StrUtf8Count - count substring occurrences") {
        assert(StrUtf8Count("ababab", 6, "ab", 2) == 3);
        assert(StrUtf8Count("aaaa", 4, "aa", 2) == 2); /* Non-overlapping */
        assert(StrUtf8Count("hello", 5, "l", 1) == 2);
        assert(StrUtf8Count("hello", 5, "x", 1) == 0);
        assert(StrUtf8Count("", 0, "a", 1) == 0);
    }

    TEST("StrUtf8CountChar - count codepoint occurrences") {
        const char *str = "Hello 世界世!";
        size_t len = strlen(str);

        assert(StrUtf8CountChar(str, len, 'l') == 2);
        assert(StrUtf8CountChar(str, len, 0x4E16) == 2); /* 世 appears twice */
        assert(StrUtf8CountChar(str, len, 0x754C) == 1); /* 界 */
        assert(StrUtf8CountChar(str, len, 'x') == 0);
    }

    TEST("StrUtf8FindAnyChar - find any char from set") {
        const char *str = "Hello, World!";
        size_t len = strlen(str);

        /* Find any vowel */
        assert(StrUtf8FindAnyChar(str, len, "aeiou", 5) ==
               1); /* 'e' at pos 1 */
        /* Find punctuation */
        assert(StrUtf8FindAnyChar(str, len, ",.!", 3) == 5); /* ',' at pos 5 */
        /* Not found */
        assert(StrUtf8FindAnyChar(str, len, "xyz", 3) == SIZE_MAX);
    }

    TEST("StrUtf8FindNotChar - find char NOT in set") {
        const char *str = "aaabbb";
        size_t len = strlen(str);

        assert(StrUtf8FindNotChar(str, len, "a", 1) == 3); /* First 'b' */
        assert(StrUtf8FindNotChar(str, len, "ab", 2) ==
               SIZE_MAX);                                 /* All in set */
        assert(StrUtf8FindNotChar(str, len, "", 0) == 0); /* Empty set */
    }

    TEST("StrUtf8SpanChar - span of chars in set") {
        const char *str = "aaabbbccc";
        size_t len = strlen(str);

        assert(StrUtf8SpanChar(str, len, "a", 1) == 3);
        assert(StrUtf8SpanChar(str, len, "ab", 2) == 6);
        assert(StrUtf8SpanChar(str, len, "abc", 3) == 9);
        assert(StrUtf8SpanChar(str, len, "x", 1) == 0);
    }

    TEST("StrUtf8SpanNotChar - span of chars NOT in set") {
        const char *str = "aaabbbccc";
        size_t len = strlen(str);

        assert(StrUtf8SpanNotChar(str, len, "b", 1) == 3);
        assert(StrUtf8SpanNotChar(str, len, "c", 1) == 6);
        assert(StrUtf8SpanNotChar(str, len, "a", 1) == 0);
        assert(StrUtf8SpanNotChar(str, len, "xyz", 3) == 9);
    }

    /* ====================================================================
     * ASCII Case Conversion Tests
     * ==================================================================== */
    TEST("StrAsciiToLower - in-place lowercase") {
        char buf[32];

        /* Basic conversion */
        strcpy(buf, "HELLO");
        StrAsciiToLower(buf, 5);
        assert(strcmp(buf, "hello") == 0);

        /* Mixed case */
        strcpy(buf, "HeLLo WoRLd");
        StrAsciiToLower(buf, 11);
        assert(strcmp(buf, "hello world") == 0);

        /* Already lowercase */
        strcpy(buf, "hello");
        StrAsciiToLower(buf, 5);
        assert(strcmp(buf, "hello") == 0);

        /* Non-alpha unchanged */
        strcpy(buf, "ABC123!@#");
        StrAsciiToLower(buf, 9);
        assert(strcmp(buf, "abc123!@#") == 0);

        /* UTF-8 unchanged */
        strcpy(buf, "HELLO\xC3\x89"); /* HELLOÉ */
        StrAsciiToLower(buf, 7);
        assert(strcmp(buf, "hello\xC3\x89") == 0);
    }

    TEST("StrAsciiToUpper - in-place uppercase") {
        char buf[32];

        /* Basic conversion */
        strcpy(buf, "hello");
        StrAsciiToUpper(buf, 5);
        assert(strcmp(buf, "HELLO") == 0);

        /* Mixed case */
        strcpy(buf, "HeLLo WoRLd");
        StrAsciiToUpper(buf, 11);
        assert(strcmp(buf, "HELLO WORLD") == 0);

        /* Already uppercase */
        strcpy(buf, "HELLO");
        StrAsciiToUpper(buf, 5);
        assert(strcmp(buf, "HELLO") == 0);

        /* Non-alpha unchanged */
        strcpy(buf, "abc123!@#");
        StrAsciiToUpper(buf, 9);
        assert(strcmp(buf, "ABC123!@#") == 0);
    }

    TEST("StrAsciiToLowerCopy - copy with lowercase") {
        char src[] = "HELLO WORLD";
        char dst[32];

        size_t written = StrAsciiToLowerCopy(dst, sizeof(dst), src, 11);
        assert(written == 11);
        dst[written] = '\0';
        assert(strcmp(dst, "hello world") == 0);

        /* Source unchanged */
        assert(strcmp(src, "HELLO WORLD") == 0);
    }

    TEST("StrAsciiToUpperCopy - copy with uppercase") {
        char src[] = "hello world";
        char dst[32];

        size_t written = StrAsciiToUpperCopy(dst, sizeof(dst), src, 11);
        assert(written == 11);
        dst[written] = '\0';
        assert(strcmp(dst, "HELLO WORLD") == 0);
    }

    TEST("StrAsciiIsLower - check all lowercase") {
        assert(StrAsciiIsLower("hello", 5) == true);
        assert(StrAsciiIsLower("hello world", 11) == true);
        assert(StrAsciiIsLower("hello123", 8) == true);
        assert(StrAsciiIsLower("HELLO", 5) == false);
        assert(StrAsciiIsLower("Hello", 5) == false);
        assert(StrAsciiIsLower("123!@#", 6) == true); /* No letters */
        assert(StrAsciiIsLower("", 0) == true);
    }

    TEST("StrAsciiIsUpper - check all uppercase") {
        assert(StrAsciiIsUpper("HELLO", 5) == true);
        assert(StrAsciiIsUpper("HELLO WORLD", 11) == true);
        assert(StrAsciiIsUpper("HELLO123", 8) == true);
        assert(StrAsciiIsUpper("hello", 5) == false);
        assert(StrAsciiIsUpper("Hello", 5) == false);
        assert(StrAsciiIsUpper("123!@#", 6) == true); /* No letters */
        assert(StrAsciiIsUpper("", 0) == true);
    }

    TEST("StrAsciiToLower - SIMD test with long string") {
        /* Test string longer than 16 bytes to exercise SIMD paths */
        char buf[128];
        strcpy(
            buf,
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyz");
        StrAsciiToLower(buf, strlen(buf));
        assert(strcmp(buf, "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmno"
                           "pqrstuvwxyz") == 0);
    }

    TEST("StrAsciiToUpper - SIMD test with long string") {
        char buf[128];
        strcpy(
            buf,
            "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ");
        StrAsciiToUpper(buf, strlen(buf));
        assert(strcmp(buf, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCDEFGHIJKLMNO"
                           "PQRSTUVWXYZ") == 0);
    }

    /* ================================================================
     * Unicode Character Properties Tests
     * ================================================================ */

    TEST("StrUnicodeEastAsianWidth - width calculation") {
        /* ASCII is narrow (1) */
        assert(StrUnicodeEastAsianWidth('A') == 1);
        assert(StrUnicodeEastAsianWidth('z') == 1);
        assert(StrUnicodeEastAsianWidth(' ') == 1);

        /* CJK is wide (2) */
        assert(StrUnicodeEastAsianWidth(0x4E00) == 2); /* CJK Unified */
        assert(StrUnicodeEastAsianWidth(0x3042) == 2); /* Hiragana あ */
        assert(StrUnicodeEastAsianWidth(0x30A2) == 2); /* Katakana ア */
        assert(StrUnicodeEastAsianWidth(0xAC00) == 2); /* Hangul */

        /* Fullwidth ASCII is wide (2) */
        assert(StrUnicodeEastAsianWidth(0xFF01) == 2); /* Fullwidth ! */
        assert(StrUnicodeEastAsianWidth(0xFF21) == 2); /* Fullwidth A */

        /* Combining marks are zero-width (0) */
        assert(StrUnicodeEastAsianWidth(0x0300) == 0); /* Combining grave */
        assert(StrUnicodeEastAsianWidth(0x0301) == 0); /* Combining acute */
        assert(StrUnicodeEastAsianWidth(0x200D) == 0); /* ZWJ */
    }

    TEST("StrUnicodeIsLetter - letter detection") {
        /* ASCII letters */
        assert(StrUnicodeIsLetter('A') == true);
        assert(StrUnicodeIsLetter('Z') == true);
        assert(StrUnicodeIsLetter('a') == true);
        assert(StrUnicodeIsLetter('z') == true);

        /* ASCII non-letters */
        assert(StrUnicodeIsLetter('0') == false);
        assert(StrUnicodeIsLetter(' ') == false);
        assert(StrUnicodeIsLetter('!') == false);

        /* CJK ideographs are letters */
        assert(StrUnicodeIsLetter(0x4E00) == true);
        assert(StrUnicodeIsLetter(0x9FFF) == true);

        /* Hiragana/Katakana are letters */
        assert(StrUnicodeIsLetter(0x3042) == true); /* あ */
        assert(StrUnicodeIsLetter(0x30A2) == true); /* ア */

        /* Cyrillic */
        assert(StrUnicodeIsLetter(0x0410) == true); /* А */
        assert(StrUnicodeIsLetter(0x044F) == true); /* я */
    }

    TEST("StrUnicodeIsDigit - digit detection") {
        /* ASCII digits */
        assert(StrUnicodeIsDigit('0') == true);
        assert(StrUnicodeIsDigit('9') == true);

        /* ASCII non-digits */
        assert(StrUnicodeIsDigit('A') == false);
        assert(StrUnicodeIsDigit(' ') == false);

        /* Fullwidth digits */
        assert(StrUnicodeIsDigit(0xFF10) == true); /* Fullwidth 0 */
        assert(StrUnicodeIsDigit(0xFF19) == true); /* Fullwidth 9 */

        /* Arabic-Indic digits */
        assert(StrUnicodeIsDigit(0x0660) == true);
        assert(StrUnicodeIsDigit(0x0669) == true);
    }

    TEST("StrUnicodeIsSpace - whitespace detection") {
        /* ASCII whitespace */
        assert(StrUnicodeIsSpace(' ') == true);
        assert(StrUnicodeIsSpace('\t') == true);
        assert(StrUnicodeIsSpace('\n') == true);
        assert(StrUnicodeIsSpace('\r') == true);

        /* ASCII non-whitespace */
        assert(StrUnicodeIsSpace('A') == false);
        assert(StrUnicodeIsSpace('0') == false);

        /* Unicode whitespace */
        assert(StrUnicodeIsSpace(0x00A0) == true); /* No-Break Space */
        assert(StrUnicodeIsSpace(0x2003) == true); /* Em Space */
        assert(StrUnicodeIsSpace(0x3000) == true); /* Ideographic Space */
    }

    TEST("StrUnicodeIsAlnum - alphanumeric detection") {
        /* Letters */
        assert(StrUnicodeIsAlnum('A') == true);
        assert(StrUnicodeIsAlnum(0x4E00) == true);

        /* Digits */
        assert(StrUnicodeIsAlnum('5') == true);
        assert(StrUnicodeIsAlnum(0xFF15) == true);

        /* Non-alphanumeric */
        assert(StrUnicodeIsAlnum(' ') == false);
        assert(StrUnicodeIsAlnum('!') == false);
    }

    TEST("StrUnicodeGraphemeBreak - grapheme break property") {
        /* CR and LF */
        assert(StrUnicodeGraphemeBreak('\r') == 1); /* GBP_CR */
        assert(StrUnicodeGraphemeBreak('\n') == 2); /* GBP_LF */

        /* Control */
        assert(StrUnicodeGraphemeBreak(0x00) == 3); /* GBP_Control */
        assert(StrUnicodeGraphemeBreak(0x7F) == 3); /* DEL */

        /* ZWJ */
        assert(StrUnicodeGraphemeBreak(0x200D) == 5); /* GBP_ZWJ */

        /* Regional Indicator */
        assert(StrUnicodeGraphemeBreak(0x1F1E6) ==
               6); /* GBP_Regional_Indicator */

        /* Combining marks are Extend */
        assert(StrUnicodeGraphemeBreak(0x0300) == 4); /* GBP_Extend */
    }

    TEST("StrUnicodeIsGraphemeBreak - grapheme cluster boundaries") {
        /* CR x LF - no break */
        assert(StrUnicodeIsGraphemeBreak('\r', '\n') == false);

        /* Control ÷ anything - break */
        assert(StrUnicodeIsGraphemeBreak('\n', 'a') == true);
        assert(StrUnicodeIsGraphemeBreak('\r', 'a') == true);

        /* anything x Extend - no break */
        assert(StrUnicodeIsGraphemeBreak('a', 0x0301) ==
               false); /* a + combining acute */
        assert(StrUnicodeIsGraphemeBreak(0x4E00, 0x0300) == false);

        /* anything x ZWJ - no break */
        assert(StrUnicodeIsGraphemeBreak('a', 0x200D) == false);

        /* Normal characters - break */
        assert(StrUnicodeIsGraphemeBreak('a', 'b') == true);
        assert(StrUnicodeIsGraphemeBreak(0x4E00, 0x4E01) == true);

        /* Regional Indicators - no break between pair */
        assert(StrUnicodeIsGraphemeBreak(0x1F1E6, 0x1F1E7) == false);
    }

    /* ================================================================
     * UTF-8 Display Width Tests
     * ================================================================ */

    TEST("StrUtf8Width - basic width calculation") {
        /* ASCII is width 1 */
        assert(StrUtf8Width("hello", 5) == 5);
        assert(StrUtf8Width("", 0) == 0);

        /* CJK is width 2 each */
        const char *cjk = "\xE4\xB8\xAD\xE6\x96\x87"; /* 中文 */
        assert(StrUtf8Width(cjk, 6) == 4);            /* 2 chars * 2 width */

        /* Mix of ASCII and CJK */
        const char *mix = "a\xE4\xB8\xAD"
                          "b";             /* a中b */
        assert(StrUtf8Width(mix, 5) == 4); /* 1 + 2 + 1 */

        /* Combining marks are width 0 */
        const char *combining = "e\xCC\x81";     /* é (e + combining acute) */
        assert(StrUtf8Width(combining, 3) == 1); /* e=1, combining=0 */
    }

    TEST("StrUtf8WidthN - width of first N codepoints") {
        const char *mix = "a\xE4\xB8\xAD"
                          "b";                  /* a中b */
        assert(StrUtf8WidthN(mix, 5, 1) == 1);  /* just 'a' */
        assert(StrUtf8WidthN(mix, 5, 2) == 3);  /* 'a' + '中' */
        assert(StrUtf8WidthN(mix, 5, 3) == 4);  /* 'a' + '中' + 'b' */
        assert(StrUtf8WidthN(mix, 5, 10) == 4); /* all of it */
    }

    TEST("StrUtf8TruncateWidth - truncate to max width") {
        /* ASCII */
        assert(StrUtf8TruncateWidth("hello world", 11, 5) == 5);
        assert(StrUtf8TruncateWidth("hello", 5, 10) == 5); /* fits */

        /* CJK - don't split wide chars */
        const char *cjk = "\xE4\xB8\xAD\xE6\x96\x87"; /* 中文 */
        assert(StrUtf8TruncateWidth(cjk, 6, 3) == 3); /* just 中 (width 2) */
        assert(StrUtf8TruncateWidth(cjk, 6, 2) == 3); /* just 中 */
        assert(StrUtf8TruncateWidth(cjk, 6, 1) == 0); /* 中 doesn't fit */
        assert(StrUtf8TruncateWidth(cjk, 6, 4) == 6); /* both fit */

        /* Mix */
        const char *mix = "a\xE4\xB8\xAD"
                          "b";                        /* a中b */
        assert(StrUtf8TruncateWidth(mix, 5, 3) == 4); /* a中 */
        assert(StrUtf8TruncateWidth(mix, 5, 2) == 1); /* just a (中 too wide) */
    }

    TEST("StrUtf8IndexAtWidth - byte index at width") {
        const char *mix = "a\xE4\xB8\xAD"
                          "b"; /* a中b */
        assert(StrUtf8IndexAtWidth(mix, 5, 0) == 0);
        assert(StrUtf8IndexAtWidth(mix, 5, 1) == 1); /* after 'a' */
        assert(StrUtf8IndexAtWidth(mix, 5, 2) ==
               1); /* in middle of 中's width */
        assert(StrUtf8IndexAtWidth(mix, 5, 3) == 4); /* after 中 */
        assert(StrUtf8IndexAtWidth(mix, 5, 4) == 5); /* after 'b' */
    }

    TEST("StrUtf8WidthAt - width at byte offset") {
        const char *mix = "a\xE4\xB8\xAD"
                          "b"; /* a中b */
        assert(StrUtf8WidthAt(mix, 5, 0) == 0);
        assert(StrUtf8WidthAt(mix, 5, 1) == 1); /* 'a' */
        assert(StrUtf8WidthAt(mix, 5, 4) == 3); /* 'a' + '中' */
        assert(StrUtf8WidthAt(mix, 5, 5) == 4); /* all */
    }

    TEST("StrUtf8PadWidth - calculate padding") {
        assert(StrUtf8PadWidth("hello", 5, 10) == 5);
        assert(StrUtf8PadWidth("hello", 5, 5) == 0);
        assert(StrUtf8PadWidth("hello", 5, 3) == 0);

        /* CJK */
        const char *cjk = "\xE4\xB8\xAD";        /* 中 (width 2) */
        assert(StrUtf8PadWidth(cjk, 3, 5) == 3); /* need 3 to reach 5 */
    }

    TEST("StrUtf8WidthBetween - width of substring") {
        const char *mix = "a\xE4\xB8\xAD"
                          "b";                          /* a中b */
        assert(StrUtf8WidthBetween(mix, 5, 0, 1) == 1); /* 'a' */
        assert(StrUtf8WidthBetween(mix, 5, 1, 4) == 2); /* '中' */
        assert(StrUtf8WidthBetween(mix, 5, 0, 5) == 4); /* all */
        assert(StrUtf8WidthBetween(mix, 5, 4, 5) == 1); /* 'b' */
    }

    TEST("StrUtf8IsNarrow - check for narrow-only") {
        assert(StrUtf8IsNarrow("hello", 5) == true);
        assert(StrUtf8IsNarrow("", 0) == true);

        /* CJK is not narrow */
        const char *cjk = "\xE4\xB8\xAD"; /* 中 */
        assert(StrUtf8IsNarrow(cjk, 3) == false);

        /* Mix */
        const char *mix = "a\xE4\xB8\xAD"
                          "b";
        assert(StrUtf8IsNarrow(mix, 5) == false);
    }

    TEST("StrUtf8HasWide - check for wide characters") {
        assert(StrUtf8HasWide("hello", 5) == false);
        assert(StrUtf8HasWide("", 0) == false);

        /* CJK is wide */
        const char *cjk = "\xE4\xB8\xAD"; /* 中 */
        assert(StrUtf8HasWide(cjk, 3) == true);

        /* Mix */
        const char *mix = "a\xE4\xB8\xAD"
                          "b";
        assert(StrUtf8HasWide(mix, 5) == true);
    }

    /* ================================================================
     * UTF-8 Grapheme Cluster Tests
     * ================================================================ */

    TEST("StrUtf8GraphemeNext - find next grapheme") {
        /* Simple ASCII */
        assert(StrUtf8GraphemeNext("abc", 3) == 1);

        /* Base + combining mark is single grapheme */
        const char *combining = "e\xCC\x81"
                                "x";                    /* é + x */
        assert(StrUtf8GraphemeNext(combining, 4) == 3); /* e + combining */

        /* Empty string */
        assert(StrUtf8GraphemeNext("", 0) == 0);

        /* CJK character */
        const char *cjk = "\xE4\xB8\xAD"
                          "a";                    /* 中a */
        assert(StrUtf8GraphemeNext(cjk, 4) == 3); /* just 中 */
    }

    TEST("StrUtf8GraphemeCount - count grapheme clusters") {
        /* ASCII */
        assert(StrUtf8GraphemeCount("hello", 5) == 5);
        assert(StrUtf8GraphemeCount("", 0) == 0);

        /* Base + combining = 1 grapheme */
        const char *combining = "e\xCC\x81"; /* é */
        assert(StrUtf8GraphemeCount(combining, 3) == 1);

        /* Multiple combining marks */
        const char *multi = "a\xCC\x80\xCC\x81"; /* a + grave + acute */
        assert(StrUtf8GraphemeCount(multi, 5) == 1);

        /* Mix */
        const char *mix = "ae\xCC\x81"
                          "b"; /* a + é + b */
        assert(StrUtf8GraphemeCount(mix, 5) == 3);
    }

    TEST("StrUtf8GraphemeAdvance - advance by N graphemes") {
        const char *str = "ae\xCC\x81"
                          "bc"; /* a + é + b + c */
        assert(StrUtf8GraphemeAdvance(str, 6, 0) == 0);
        assert(StrUtf8GraphemeAdvance(str, 6, 1) == 1);  /* past 'a' */
        assert(StrUtf8GraphemeAdvance(str, 6, 2) == 4);  /* past 'é' */
        assert(StrUtf8GraphemeAdvance(str, 6, 3) == 5);  /* past 'b' */
        assert(StrUtf8GraphemeAdvance(str, 6, 4) == 6);  /* past 'c' */
        assert(StrUtf8GraphemeAdvance(str, 6, 10) == 6); /* all */
    }

    TEST("StrUtf8GraphemeAt - get grapheme range") {
        const char *str = "ae\xCC\x81"
                          "b"; /* a + é + b */
        size_t start, end;

        /* First grapheme: 'a' */
        assert(StrUtf8GraphemeAt(str, 5, 0, &start, &end) == true);
        assert(start == 0 && end == 1);

        /* Second grapheme: 'é' (3 bytes) */
        assert(StrUtf8GraphemeAt(str, 5, 1, &start, &end) == true);
        assert(start == 1 && end == 4);

        /* Third grapheme: 'b' */
        assert(StrUtf8GraphemeAt(str, 5, 2, &start, &end) == true);
        assert(start == 4 && end == 5);

        /* Out of bounds */
        assert(StrUtf8GraphemeAt(str, 5, 10, &start, &end) == false);
    }

    TEST("StrUtf8GraphemeWidth - width with grapheme clusters") {
        /* ASCII */
        assert(StrUtf8GraphemeWidth("hello", 5) == 5);

        /* Base + combining = 1 cell */
        const char *combining = "e\xCC\x81"; /* é */
        assert(StrUtf8GraphemeWidth(combining, 3) == 1);

        /* CJK = 2 cells */
        const char *cjk = "\xE4\xB8\xAD"; /* 中 */
        assert(StrUtf8GraphemeWidth(cjk, 3) == 2);

        /* Mix: a(1) + é(1) + 中(2) = 4 */
        const char *mix = "ae\xCC\x81\xE4\xB8\xAD";
        assert(StrUtf8GraphemeWidth(mix, 7) == 4);
    }

    TEST("StrUtf8GraphemeTruncate - truncate by graphemes") {
        const char *str = "ae\xCC\x81"
                          "bc";                          /* a + é + b + c */
        assert(StrUtf8GraphemeTruncate(str, 6, 2) == 4); /* a + é */
        assert(StrUtf8GraphemeTruncate(str, 6, 1) == 1); /* a */
        assert(StrUtf8GraphemeTruncate(str, 6, 0) == 0);
    }

    TEST("StrUtf8GraphemeReverse - reverse by graphemes") {
        /* Simple ASCII */
        char buf1[] = "abc";
        StrUtf8GraphemeReverse(buf1, 3);
        assert(strcmp(buf1, "cba") == 0);

        /* With combining marks - should keep combining with base */
        char buf2[] = "ae\xCC\x81"
                      "b"; /* a + é + b */
        StrUtf8GraphemeReverse(buf2, 5);
        /* Should be: b + é + a = "be\xCC\x81a" */
        assert(buf2[0] == 'b');
        assert(buf2[1] == 'e');
        assert((uint8_t)buf2[2] == 0xCC);
        assert((uint8_t)buf2[3] == 0x81);
        assert(buf2[4] == 'a');
    }

    TEST("StrBufToInt128: exact signed range boundaries") {
        /* Magnitudes past the signed range must REJECT, not wrap into
         * silently wrong values */
        const char *maxStr = "170141183460469231731687303715884105727";
        const char *maxPlus1 = "170141183460469231731687303715884105728";
        const char *minStr = "-170141183460469231731687303715884105728";
        const char *minMinus1 = "-170141183460469231731687303715884105729";

        __int128_t v = 0;

        if (!StrBufToInt128(maxStr, strlen(maxStr), &v)) {
            ERR("INT128_MAX string must parse");
        } else {
            const __int128_t expectMax =
                (__int128_t)((((__uint128_t)1 << 127) - 1));
            if (v != expectMax) {
                ERR("INT128_MAX parsed to wrong value");
            }
        }

        if (!StrBufToInt128(minStr, strlen(minStr), &v)) {
            ERR("INT128_MIN string must parse");
        } else {
            const __int128_t expectMin =
                -(__int128_t)((((__uint128_t)1 << 127) - 1)) - 1;
            if (v != expectMin) {
                ERR("INT128_MIN parsed to wrong value");
            }
        }

        if (StrBufToInt128(maxPlus1, strlen(maxPlus1), &v)) {
            ERR("INT128_MAX+1 must be rejected");
        }
        if (StrBufToInt128(minMinus1, strlen(minMinus1), &v)) {
            ERR("INT128_MIN-1 must be rejected");
        }

        /* The originally observed silent corruption: -1.8e38 */
        const char *badNeg = "-180000000000000000000000000000000000000";
        if (StrBufToInt128(badNeg, strlen(badNeg), &v)) {
            ERR("-1.8e38 must be rejected (was silently wrapping)");
        }
    }

    TEST("StrBufToUInt128 / StrBufToInt128: canonical digits only") {
        /* The same grammar as StrBufToUInt64 / StrBufToInt64 */
        static const char *const refused[] = {
            "", "-", "00", "01", "-0", "-01", "+1", " 1", "1 ", "1a", "a",
            "1.0", "--1", "1-", "0x10",
            /* UINT128_MAX + 1, then 40 digits */
            "340282366920938463463374607431768211456",
            "1000000000000000000000000000000000000000"};
        for (size_t i = 0; i < countof(refused); i++) {
            const size_t len = strlen(refused[i]);
            __uint128_t u = 7;
            __int128_t s = 7;
            if (StrBufToUInt128(refused[i], len, &u) || u != 7) {
                ERR("StrBufToUInt128 accepted '%s'", refused[i]);
            }
            if (StrBufToInt128(refused[i], len, &s) || s != 7) {
                ERR("StrBufToInt128 accepted '%s'", refused[i]);
            }
        }

        __uint128_t u = 0;
        const char *umax = "340282366920938463463374607431768211455";
        if (!StrBufToUInt128(umax, strlen(umax), &u) || u != ~(__uint128_t)0) {
            ERR("UINT128_MAX must parse exactly");
        }
        if (!StrBufToUInt128("0", 1, &u) || u != 0) {
            ERR("\"0\" must parse as 0");
        }

        __int128_t s = 1;
        if (!StrBufToInt128("0", 1, &s) || s != 0) {
            ERR("\"0\" must parse as signed 0");
        }
        if (!StrBufToInt128("-1", 2, &s) || s != -1) {
            ERR("\"-1\" must parse as -1");
        }
    }

    TEST("StrScanScanReliableConvert128 writes fully defined boxes") {
        /* Poison every metadata bit of both outputs, then check the box
         * *use names holds exactly the expected type, payload and clear
         * metadata */
        static const struct {
            const char *s;
            databoxType type;
            bool wide;
        } cases[] = {
            {"340282366920938463463374607431768211455", DATABOX_UNSIGNED_128,
             true},
            {"18446744073709551616", DATABOX_UNSIGNED_128, true},
            {"-170141183460469231731687303715884105728", DATABOX_SIGNED_128,
             true},
            {"-9223372036854775809", DATABOX_SIGNED_128, true},
            {"18446744073709551615", DATABOX_UNSIGNED_64, false},
            {"-9223372036854775808", DATABOX_SIGNED_64, false},
            {"-1000000000000000000", DATABOX_SIGNED_64, false},
            {"12345", DATABOX_SIGNED_64, false},
        };

        for (size_t i = 0; i < countof(cases); i++) {
            databox small;
            databoxBig big;
            memset(&small, 0xA5, sizeof(small));
            memset(&big, 0xA5, sizeof(big));
            databoxBig *use = nullptr;
            const size_t len = strlen(cases[i].s);
            if (!StrScanScanReliableConvert128(cases[i].s, len, &small, &big,
                                               &use)) {
                ERR("Convert128 refused %s", cases[i].s);
                continue;
            }

            if (use != (cases[i].wide ? &big : (databoxBig *)&small)) {
                ERR("Convert128 put %s in the wrong box", cases[i].s);
                continue;
            }

            if (use->type != cases[i].type || use->allocated || use->created ||
                use->unused || use->len || use->big != cases[i].wide ||
                (cases[i].wide && use->data.u128 != (__uint128_t *)big.extra)) {
                ERR("Convert128 left stale metadata for %s", cases[i].s);
                continue;
            }

            /* The payload round-trips through the public parsers */
            if (cases[i].type == DATABOX_UNSIGNED_128) {
                __uint128_t want = 0;
                StrBufToUInt128(cases[i].s, len, &want);
                if (*use->data.u128 != want) {
                    ERR("Convert128 payload wrong for %s", cases[i].s);
                }
            } else if (cases[i].type == DATABOX_SIGNED_128) {
                __int128_t want = 0;
                StrBufToInt128(cases[i].s, len, &want);
                if (*use->data.i128 != want) {
                    ERR("Convert128 payload wrong for %s", cases[i].s);
                }
            } else {
                databox want = {};
                StrScanScanReliable(cases[i].s, len, &want);
                if (want.type != use->type || want.data.u64 != use->data.u64) {
                    ERR("Convert128 payload wrong for %s", cases[i].s);
                }
            }
        }

        /* A refused buffer writes neither output nor *use */
        databox small;
        databoxBig big;
        memset(&small, 0xA5, sizeof(small));
        memset(&big, 0xA5, sizeof(big));
        databoxBig *use = (databoxBig *)&small;
        const char *bad = "340282366920938463463374607431768211456";
        if (StrScanScanReliableConvert128(bad, strlen(bad), &small, &big,
                                          &use)) {
            ERR("Convert128 accepted UINT128_MAX + 1");
        }
        const uint8_t *sp = (const uint8_t *)&small;
        const uint8_t *bp = (const uint8_t *)&big;
        bool untouched = use == (databoxBig *)&small;
        for (size_t b = 0; b < sizeof(small); b++) {
            untouched = untouched && sp[b] == 0xA5;
        }
        for (size_t b = 0; b < sizeof(big); b++) {
            untouched = untouched && bp[b] == 0xA5;
        }
        if (!untouched) {
            ERR("Convert128 wrote output on failure");
        }
    }

    TEST("StrScanScanReliableConvert128 gives 20-40 byte non-integers the "
         "64-bit scanner's verdict") {
        /* Reals and other non-integers of integer-like length must get
         * exactly what StrScanScanReliable says, accepted or not */
        static const char *const inputs[] = {
            "-0.30000000000000004",  "0.30000000000000004",
            "0.12345678901234567",   "-0.12345678901234567",
            "1234567.8901234567",    "-1234567.8901234567",
            "0.1000000000000000000", "1.5000000000000000000",
            "12345678901234567.89",  "-0.0000000000000000001",
            "00000000000000000001",  "-00000000000000000001",
            "1e100000000000000000",  "+1000000000000000000000",
            "1000000000000000000.0", "12345678901234567890a",
        };
        size_t accepted = 0;
        for (size_t i = 0; i < countof(inputs); i++) {
            const size_t len = strlen(inputs[i]);
            databox narrow = {};
            const bool want = StrScanScanReliable(inputs[i], len, &narrow);

            databox small = {};
            databoxBig big = {};
            databoxBig *use = nullptr;
            const bool got = StrScanScanReliableConvert128(inputs[i], len,
                                                           &small, &big, &use);
            if (got != want) {
                ERR("Convert128(%s) = %d, 64-bit scanner says %d", inputs[i],
                    got, want);
                continue;
            }

            if (got) {
                accepted++;
                if (use != (databoxBig *)&small || small.type != narrow.type ||
                    small.data.u64 != narrow.data.u64) {
                    ERR("Convert128(%s) differs from the 64-bit scanner",
                        inputs[i]);
                }
            }
        }

        printf("    %zu of %zu long non-integers are numerals\n", accepted,
               countof(inputs));
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
