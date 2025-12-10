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
 * as one of:
 *   - unsigned 64 bit integers (0 to UINT64_MAX)
 *   - signed 64 bit integers (INT64_MIN to -1)
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

    uint32_t dig = 0; /* number of digits */

    /* This dumb type is because GCC sees 'uint64_t'
     * as 'long unsigned int' but the overflow functions
     * only take 'long long unsigned int' */
    unsigned long long int x = 0; /* significand */
    uint64_t maxAbsoluteValueX = UINT64_MAX;

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
            /* Note: DO NOT remove the ULL type specifiers below
             *       or else this function goes from 500 cycles to 12,000 cycles
             *       per run.
             *       (We could also use __builtin_uaddll_overflow, etc here
             *        which also fixes the performance problem because it forces
             *        types at compile time too, but the current situation is
             *        fine as long as all the types match.) */
            if (__builtin_mul_overflow(x, 10ULL, &x) ||
                __builtin_add_overflow(x, pDeref & 0x0FULL, &x)) {
                /* If multiply or add caused overflow, we can't
                 * continue.
                 *
                 * These are elderly gcc builtins, so they return 0/false
                 * on success and 1/true as an error condition. */
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
             * if more characters exist. (i.e. '-' is not standalone number) */
            if (dig == 0 && len > 1) {
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

    /* Signed numbers (prefer first so future math can handle going
     * negative fairly easily) */
    if (dig < 19 || (dig == 19 && x <= maxAbsoluteValueX)) {
        /* Fast path for decimal 64 bit signed integers. */
        /* If digits < 19 or digits == 19 and accumulator < 2^63 (+neg) ... */
        /* We can safely multiply the known-unsigned 'x' by 'sign' to obtain
         * the final result. */
        const int64_t y = x * sign;
        box->data.i = y;
        box->type = DATABOX_SIGNED_64;
        return true;
    }

    /* Unsigned numbers (prefer second only if number is 2^63+) */
    if (dig <= 20 && sign > 0) {
        /* Fast path for decimal 64 bit unsigned integers. */
        /* If digits <= 20 and NOT signed... */
        /* This is okay because we use overflow-safe multiply and
         * addition checks in the original calculation of 'x',
         * so we don't have to worry about wrap-around while decoding */
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
 * 100% digits in the buffer so we can skip running StrIsDigitsFast() again. */
DK_INLINE_ALWAYS bool StrScanScanReliableConvert128_(
    const void *const p_, const size_t len, databox *restrict small,
    databoxBig *restrict big, databoxBig **use, const bool requireDigitCheck) {
    const uint8_t *buf = (const uint8_t *)p_;
    /* If byte length is within the range of a 128 bit integer... */
    if (len >= 20 && len <= 40) {
        if (buf[0] == '-') {
            /* and if all bytes of the buffer are numeric... */
            if (requireDigitCheck) {
                if (!StrIsDigitsFast(buf + 1, len - 1)) {
                    return false;
                }
            }

            /* Attempt to convert the buffer to an integer */
            __int128_t result;
            if (!StrBufToInt128(buf, len, &result)) {
                return false;
            }

            /* If we converted a value fitting int64_t, use int64 */
            /* Note: since this is the NEGATIVE comparison branch, we
             * are checking for result between -1 and INT64_MIN.
             *       Anything smaller _must_ be in the 128 bit
             * container. */
            if (result >= INT64_MIN) {
                small->type = DATABOX_SIGNED_64;
                small->data.i = (int64_t)result;
                *use = (databoxBig *)small;
            } else {
                /* else, populate an actual int128 */
                DATABOX_BIG_SIGNED_128(big, result);
                *use = big;
            }
        } else {
            /* The unsigned comparsion branch */
            if (requireDigitCheck) {
                if (!StrIsDigitsFast(buf, len)) {
                    return false;
                }
            }

            __uint128_t result;
            if (!StrBufToUInt128(buf, len, &result)) {
                return false;
            }

            /* If we converted a value fitting uint64_t, use uint64 */
            if (result <= UINT64_MAX) {
                small->type = DATABOX_UNSIGNED_64;
                small->data.u = (uint64_t)result;
                *use = (databoxBig *)small;
            } else {
                /* else, value requires more than 64 bits of storage: */
                DATABOX_BIG_UNSIGNED_128(big, result);
                *use = big;
            }
        }

        /* successfully converted user input! */
        return true;
    }

    /* else, try to convert to a smaller integer or float or something */
    *use = (databoxBig *)small;
    return StrScanScanReliable(p_, len, small);
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
#include "ctest.h"

#define DOUBLE_NEWLINE 0
#include "perf.h"

#define REPORT_TIME 1
#if REPORT_TIME
#define TIME_INIT PERF_TIMERS_SETUP
#define TIME_FINISH(i, what) PERF_TIMERS_FINISH_PRINT_RESULTS(i, what)
#else
#define TIME_INIT
#define TIME_FINISH(i, what)
#endif

__attribute__((optnone)) static void testStrBufToInt64(void) {
    char buf[32];
    int64_t v;
    (void)v;

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
}

__attribute__((optnone)) static void testStrBufToUInt64(void) {
    char buf[32];
    uint64_t v;
    (void)v;

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
__attribute__((optnone)) int strTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;

    /* ====================================================================
     * Stress tests for SIMD vs baseline implementations - run first!
     * ==================================================================== */
    TEST("StrIsDigitsFast vs StrIsDigitsIndividual stress test") {
        printf("  Testing StrIsDigitsFast matches baseline...\n");

        /* Test all-digits strings of various sizes */
        for (size_t size = 0; size <= 256; size++) {
            char *buf = zmalloc(size + 1);
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
            zfree(buf);
        }

        /* Test strings with non-digit at various positions */
        for (size_t size = 1; size <= 128; size++) {
            for (size_t badPos = 0; badPos < size; badPos++) {
                char *buf = zmalloc(size + 1);
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
                zfree(buf);
            }
        }

        /* Test boundary characters */
        const char boundaryChars[] = {'\0', '/', ':',  'a',
                                      'A',  ' ', 0x7F, (char)0xFF};
        for (size_t ci = 0; ci < sizeof(boundaryChars); ci++) {
            for (size_t size = 1; size <= 64; size++) {
                char *buf = zmalloc(size + 1);
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
                zfree(buf);
            }
        }

        printf("    StrIsDigitsFast stress test passed!\n");
    }

    TEST("StrUInt9DigitsToBuf correctness stress test") {
        printf("  Testing StrUInt9DigitsToBuf correctness...\n");

        /* Test all 9-digit boundary values */
        const uint32_t testValues[] = {
            0,         1,         9,         10,        99,       100,
            999,       1000,      9999,      10000,     99999,    100000,
            999999,    1000000,   9999999,   10000000,  99999999, 100000000,
            999999999, 123456789, 987654321, 111111111, 500000000};

        for (size_t i = 0; i < sizeof(testValues) / sizeof(testValues[0]);
             i++) {
            uint32_t val = testValues[i];
            char buf[10] = {0};
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
        for (uint32_t val = 0; val < 100000; val++) {
            char buf[10] = {0};
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
        for (int i = 0; i < 100000; i++) {
            rngState = rngState * 6364136223846793005ULL + 1;
            uint32_t val = (uint32_t)(rngState >> 32) % 1000000000;

            char buf[10] = {0};
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

#if __SSE2__ || defined(__aarch64__) || defined(__ARM_NEON) ||                 \
    defined(__ARM_NEON__)
    TEST("StrUInt4DigitsToBuf and StrUInt8DigitsToBuf stress test") {
        printf("  Testing StrUInt4/8DigitsToBuf correctness...\n");

        /* Test StrUInt4DigitsToBuf */
        for (uint32_t val = 0; val < 10000; val++) {
            char buf[5] = {0};
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
        for (size_t i = 0; i < sizeof(test8Values) / sizeof(test8Values[0]);
             i++) {
            uint32_t val = test8Values[i];
            char buf[9] = {0};
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
        for (uint32_t val = 0; val < 100000; val++) {
            char buf[9] = {0};
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

#if 1
    TEST("verify empty string doesn't convert to zero") {
        databox got;
        assert(StrScanScanReliable("", 0, &got) == false);
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
        const size_t loopers = 10000000;
        const char *numeristr = "1234567891234567";
        uint64_t result = 0;

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

        (void)result; /* Used for benchmarking only */
        TIME_FINISH(loopers * 15, "Byte Lengths Type A");
    }

    TEST("Medium integer convert") {
        const size_t loopers = 10000000;
        const char *numeristr = "1234567891234567";
        uint64_t result = 0;

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

        (void)result; /* Used for benchmarking only */
        TIME_FINISH(loopers * 15, "Byte Lengths Type B");
    }

    TEST("Speeds of IsDigits") {
        uint8_t buf[] = "798542789413789432789437209583490854903859823748912748"
                        "923784329543809543798547389572309842309742398753489754"
                        "38975438950934859043750894350934785943798543";

        for (size_t offset = 0; offset < 1 /* 8 */; offset++) {
            size_t printBooster = 1000000;
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
        size_t printBooster = 1000000;
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
        size_t printBooster = 100000;
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
        databox got;
        bool scanResult = StrScanScanReliable("7074451188.598104", 17, &got);

#if DK_HAS_FLOAT_EXTENDED
        /* With extended precision, round-trip detection catches the precision
         * loss, so this should return false (unreliable conversion). */
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
        databox result = {{0}};
        bool worked = StrScanScanReliable("0", 1, &result);
        assert(worked);

        if (result.type != DATABOX_SIGNED_64) {
            databoxReprSay("Expected SIGNED, but got", &result);
            assert(NULL && "Fix me!");
        }

        if (result.data.i != 0) {
            ERR("Expected %d but got %d instead!", 0, result.data.i32);
        }
    }

    TEST("Reliable number parsing simple 1?") {
        databox result = {{0}};
        bool worked = StrScanScanReliable("1", 1, &result);
        assert(worked);

        if (result.type != DATABOX_SIGNED_64) {
            databoxReprSay("Expected SIGNED, but got", &result);
            assert(NULL && "Fix me!");
        }

        if (result.data.i != 1) {
            ERR("Expected %d but got %d instead!", 1, result.data.i32);
        }
    }

    TEST("Reliable number parsing simple -1?") {
        databox result = {{0}};
        bool worked = StrScanScanReliable("-1", 2, &result);
        assert(worked);

        if (result.type != DATABOX_SIGNED_64) {
            databoxReprSay("Expected SIGNED, but got", &result);
            assert(NULL && "Fix me!");
        }

        if (result.data.i != -1) {
            ERR("Expected %d but got %d instead!", -1, result.data.i32);
        }
    }

#if 0
    TEST("Reliable number parsing simple 0 to 999999999?") {
        for (size_t i = 0; i < 999999999; i++) {
            if (i % 100000 == 0) {
                printf(".");
                fflush(stdout);
            }

            char buf[10] = {0};
            StrUInt64ToBuf(buf, sizeof(buf), i);

            databox result = {{0}};
            bool worked = StrScanScanReliable(buf, strlen(buf), &result);
            assert(worked);

            if (result.type != DATABOX_SIGNED_64) {
                databoxReprSay("Expected SIGNED, but got", &result);
                assert(NULL && "Fix me!");
            }

            if (result.data.u != i) {
                ERR("Expected %d but got %d instead!", i, result.data.i32);
            }
        }
    }
#endif

    TEST("Reliable number parsing simple?") {
        databox result = {{0}};
        bool worked = StrScanScanReliable("299.5", 5, &result);
        assert(worked);

        if (result.type != DATABOX_FLOAT_32) {
            databoxReprSay("Expected FLOAT32, but got", &result);
            assert(NULL && "Fix me!");
        }

        if (result.data.f32 != (float)299.5) {
            ERR("Expected %f but got %f instead!", 299.5, result.data.f32);
        }
    }

    TEST("Reliable number parsing simple end in zero?") {
        databox result = {{0}};
        bool worked = StrScanScanReliable("299.0", 5, &result);
        assert(worked);

        if (result.type != DATABOX_FLOAT_32) {
            databoxReprSay("Expected FLOAT32, but got", &result);
            assert(NULL && "Fix me!");
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
        databox result = {{0}};
        bool worked = StrScanScanReliable("299.5000", 8, &result);
        if (worked) {
            databoxReprSay("Expected failure, but got", &result);
        }
    }

    TEST("Reliable number parsing too long decimal fails?") {
        databox result = {{0}};
        bool worked = StrScanScanReliable("299.500010101010101", 18, &result);
        if (worked) {
            databoxReprSay("Expected failure, but got", &result);
        }
    }

    TEST("Reliable number parsing too long decimal fails II?") {
        databox result = {{0}};
        bool worked =
            StrScanScanReliable("2.22222222222222222222222222", 28, &result);
        if (worked) {
            databoxReprSay("Expected failure, but got", &result);
        }
    }

#if 0
    TEST("Reliable number parsing double extremes?") {
        databox resultMin = {{0}};
        databox resultMax = {{0}};
        const char *min = "0.000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000022250738585072014";

        const char *max = "179769313486231570000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000.0";

        bool worked = StrScanScanReliable(min, strlen(min), &resultMin);
        if (!worked) {
            databoxReprSay("Expected success, but got", &resultMin);
            assert(NULL);
        }

        worked = StrScanScanReliable(max, strlen(max), &resultMax);
        if (!worked) {
            databoxReprSay("Expected success, but got", &resultMax);
            assert(NULL);
        }
    }
#endif

    TEST("Reliable number parsing bigger real ok?") {
        databox result = {{0}};
        bool worked = StrScanScanReliable("21542136560502.848", 18, &result);
        if (!worked) {
            databoxReprSay("Expected success, but got", &result);
            assert(NULL);
        }

        if (result.data.d64 != 21542136560502.847656) {
            ERR("Expected %f but got %f!", 21542136560502.848, result.data.d64);
        }
    }

    TEST("Reliable number parsing bigger real ok III?") {
        databox result = {{0}};
        bool worked = StrScanScanReliable("0.4", 3, &result);
        if (!worked) {
            databoxReprSay("Expected success, but got", &result);
            assert(NULL);
        }

        char restoredBuf[64] = {0};
        StrDoubleFormatToBufNice(restoredBuf, sizeof(restoredBuf), 0.4);

        assert(strlen("0.4") == strlen(restoredBuf));

        if (result.data.d64 != 0.4) {
            ERR("Expected %f but got %f!", 0.4, result.data.d64);
        }
    }

    TEST("Reliable number parsing bigger real ok IV?") {
        databox result = {{0}};
        bool worked = StrScanScanReliable("0.456789", 8, &result);
        if (!worked) {
            databoxReprSay("Expected success, but got", &result);
            assert(NULL);
        }

        if (result.data.d64 != 0.456789) {
            ERR("Expected %f but got %f!", 0.456789, result.data.d64);
        }
    }
    TEST("Reliable number parsing bigger real ok II?") {
        databox result = {{0}};
        bool worked = StrScanScanReliable("9543769205953.803", 17, &result);
        const double expected = 9543769205953.803;
#if DK_HAS_FLOAT_EXTENDED
        const double expectedConverted =
            9543769205953.802734; /* actual double value */
        /* With extended precision, conversion round-trips reliably */
        if (!worked) {
            databoxReprSay("Expected success, but got", &result);
            assert(NULL);
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
        databox result = {{0}};
        bool worked = StrScanScanReliable("9543769205953.8029999999999999734",
                                          34, &result);
        if (worked) {
            databoxReprSay("Expected failure, but got", &result);
            assert(NULL);
        }
    }

    TEST("Reliable number parsing smaller leading zeroes not ok?") {
        databox result = {{0}};
        bool worked = StrScanScanReliable("03", 2, &result);
        if (worked) {
            databoxReprSay("Expected failure, but got", &result);
            assert(NULL);
        }
    }

    TEST("Reliable number parsing bigger leading zeroes not ok?") {
        databox result = {{0}};
        bool worked = StrScanScanReliable("0009543769205953", 16, &result);
        if (worked) {
            databoxReprSay("Expected failure, but got", &result);
            assert(NULL);
        }
    }

    TEST("Reliable number parsing bigger leading dot not ok?") {
        databox result = {{0}};
        bool worked = StrScanScanReliable(".0009543769205953", 17, &result);
        if (worked) {
            databoxReprSay("Expected failure, but got", &result);
            assert(NULL);
        }
    }

    TEST("Reliable number parsing bigger integer fails?") {
        databox result = {{0}};
        bool worked = StrScanScanReliable("24032321013100332443", 20, &result);
        if (worked) {
            databoxReprSay("Expected failure, but got", &result);
            assert(NULL);
        }
    }

    TEST("Reliable number parsing bigger float fails?") {
        databox result = {{0}};
        bool worked = StrScanScanReliable("240323210131003243.3", 20, &result);
        if (worked) {
            databoxReprSay("Expected failure, but got", &result);
            assert(NULL);
        }
    }

    TEST("Reliable number parsing a dash fails?") {
        databox result = {{0}};
        bool worked = StrScanScanReliable("-", 1, &result);
        if (worked) {
            databoxReprSay("Expected failure, but got", &result);
            assert(NULL);
        }
    }

    TEST("Reliable number parsing a dot fails?") {
        databox result = {{0}};
        bool worked = StrScanScanReliable(".", 1, &result);
        if (worked) {
            databoxReprSay("Expected failure, but got", &result);
            assert(NULL);
        }
    }

    TEST("Reliable number parsing integer?") {
        databox result = {{0}};
        bool worked = StrScanScanReliable("9223372036854775808", 19, &result);
        if (!worked) {
            databoxReprSay("Expected success, but got", &result);
            assert(NULL);
        }
    }

    TEST("Reliable number parsing biggest integer?") {
        databox result = {{0}};
        bool worked = StrScanScanReliable("18446744073709551615", 20, &result);
        if (!worked) {
            databoxReprSay("Expected success, but got", &result);
            assert(NULL);
        }

        assert(result.data.u == UINT64_MAX);
    }

    TEST("Reliable number parsing too big integer signed failed?") {
        databox result = {{0}};
        bool worked = StrScanScanReliable("-18446744073709551615", 21, &result);
        if (worked) {
            databoxReprSay("Expected failure, but got", &result);
            assert(NULL);
        }
    }

    TEST("Reliable number parsing biggerest integer fails?") {
        databox result = {{0}};
        bool worked = StrScanScanReliable("18446744073709551616", 20, &result);
        if (worked) {
            databoxReprSay("Expected success, but got", &result);
            assert(NULL);
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

            databox result = {{0}};
            bool worked = StrScanScanReliable(useBad, strlen(useBad), &result);
            if (worked) {
                databoxReprSay("Expected failure, but got", &result);
                assert(NULL);
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
            assert(NULL);
        }
    }

    TEST("Reliable number parsing biggerest integer works III?") {
        databox result;
        bool worked = StrScanScanReliable("2411321300310020112", 19, &result);
        if (!worked) {
            databoxReprSay("Expected success, but got", &result);
            assert(NULL);
        }

        if (result.type != DATABOX_SIGNED_64) {
            databoxReprSay("Expected SIGNED, but got", &result);
            assert(NULL && "Fix me!");
        }

        if (result.data.i != 2411321300310020112LL) {
            ERR("Expected %" PRId64 ", but got %" PRId64, 2411321300310020112LL,
                (int64_t)result.data.i);
        }
    }

    TEST("Perf Number Conversions") {
        const size_t loopBooster = 9000000;
        const size_t looperDeDoBooster = 12;

        char *str[] = {"454545454545.5",       "9223372036854775808",
                       "-9223372036854775809", "2147483648",
                       "2147483649",           "1.234",
                       "789498543789543.13",   "21542136560502.847656"};

        size_t len[COUNT_ARRAY(str)] = {0};
        for (size_t i = 0; i < COUNT_ARRAY(str); i++) {
            len[i] = strlen(str[i]);
        }

        {
            for (size_t j = 0; j < looperDeDoBooster; j++) {
                TIME_INIT;
                databox result = {{0}};
                for (size_t i = 0; i < loopBooster; i++) {
                    PERF_TIMERS_STAT_START;
                    const size_t useStr = i % COUNT_ARRAY(str);
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
                    const size_t useStr = i % COUNT_ARRAY(str);
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
            assert(NULL && "Need case!");
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

    TEST("benchmark multiply vs. shifting by 100") {
        const size_t loopBooster = 1e4;
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
        const size_t loopBooster = 4 * 1e7;

        {
            TIME_INIT;
            for (size_t i = 0; i < loopBooster; i++) {
                PERF_TIMERS_STAT_START;
                uint64_t intval = strtoull("18446744073709551615", NULL, 10);
                PERF_TIMERS_STAT_STOP(i);
                assert(intval);
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

    TEST("bit set positions 8") {
        const uint64_t pickles = 0xc0c0c0c0c0c0c0c0ULL;
        const uint8_t positionsValidate[64] = {
            6, 7, 14, 15, 22, 23, 30, 31, 38, 39, 46, 47, 54, 55, 62, 63,
            0, 0, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
            0, 0, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
            0, 0, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0};

        uint8_t positions[64] = {0};
        StrBitmapGetSetPositionsExact8(&pickles, sizeof(pickles), positions);
        assert(
            !memcmp(positions, positionsValidate, sizeof(*positionsValidate)));
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

        uint8_t positions[64] = {0};
        StrBitmapGetUnsetPositionsExact8(&pickles, sizeof(pickles), positions);
        assert(
            !memcmp(positions, positionsValidate, sizeof(*positionsValidate)));
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

        for (size_t i = 0; i < sizeof(setPositions) / sizeof(*setPositions);
             i++) {
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
        const int booster = 1000000;

        char *tester = zmalloc(len * booster); /* 37 MB */
        for (size_t i = 0; i < len * booster; i += len) {
            memcpy(tester + i, foo, len);
        }

        const size_t loopBooster = 1000;
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
        zfree(tester);
    }

    TEST("lenutf8countbytes ascii large") {
        const char *foo = "hellooooooooooooooooooooooooooooo foo";
        const size_t len = strlen(foo);
        const int booster = 1000000;

        char *tester = zmalloc(len * booster); /* 37 MB */
        for (size_t i = 0; i < len * booster; i += len) {
            memcpy(tester + i, foo, len);
        }

        const size_t loopBooster = 1000;
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
        zfree(tester);
    }

    TEST("lenutf8 utf8 small") {
        const char *foo = u8"😀";
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

#define _TU8MIDDLES                                                            \
    "\xF0\x9F\x98\x81"                                                         \
    "\xF0\x9F\x98\x82"                                                         \
    "\xF0\x9F\x98\x83"                                                         \
    "\xF0\x9F\x98\x84"                                                         \
    "\xF0\x9F\x98\x85"

    for (int i = 0; i < (int)sizeof(STRLEN_UTF8_STEP); i++) {
        TEST_DESC("lenutf8countbytes utf8 small (substr; offset by %d)", i) {
            const char foo[] = u8"abcdefhello " _TU8MIDDLES "fedcba";
            uint8_t space[640] = {0};

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
        const char *foo = u8"💛💙💜💔";
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
        const char *foo = u8"💛💙💜💔";
        const size_t things = 4;
        const size_t len = strlen((const char *)foo);
        const size_t utf8len = StrLenUtf8CountBytes(foo, len, things);

        if (len != utf8len) {
            ERR("Expected len %zu but got %zu instead!", len, utf8len);
        }
    }

    TEST("lenutf8countbytes utf8 bigger (overshot)") {
        const char *foo = u8"💛💙💜💔";
        const size_t things = 4;
        const size_t len = strlen((const char *)foo);
        const size_t utf8len = StrLenUtf8CountBytes(foo, len, things * 2);

        if (len != utf8len) {
            ERR("Expected len %zu but got %zu instead!", len, utf8len);
        }
    }

    for (int i = 0; i < (int)sizeof(STRLEN_UTF8_STEP); i++) {
        TEST_DESC("lenutf8countbytes utf8 bigger (alignment offset %d)", i) {
            char *alignmentHelper = zcalloc(1, 640);
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
            zfree(alignmentHelper);
        }
    }

    for (int i = 0; i < (int)sizeof(STRLEN_UTF8_STEP); i++) {
        TEST_DESC("lenutf8countbytes utf8 multi-boundary (alignment offset %d)",
                  i) {
            char *alignmentHelper = zcalloc(1, 640);
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
            zfree(alignmentHelper);
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
        const int booster = 300000;     /* total of 32,000 final things */

        char *tester = zcalloc(1, len * booster + 1); /* 38.4 MB */
        for (size_t i = 0; i < len * booster; i += len) {
            memcpy(tester + i, foo, len);
        }

        const size_t loopBooster = 1000;
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

        zfree(tester);
    }
#endif

    for (int alignmentOffset = 0;
         alignmentOffset < (int)sizeof(STRLEN_UTF8_STEP); alignmentOffset++) {
        TEST_DESC("lenutf8countbytes utf8 large (alignment offset %d)",
                  alignmentOffset) {
            const char foo[] = u8"💛💙💜💔💛💙💜💔💛💙💜💔💛💙💜💔💛💙💜💔💛💙"
                               u8"💜💔💛💙💜💔💛💙💜💔";
            const size_t len = strlen(foo); /* len == 128 */
            const int booster = 300000;     /* total of 32,000 final things */
            const size_t countCharacters = (32 * booster) / 2;

            /* each emoji is a 4 byte UTF-8 character */
            const size_t countedBytes = 4 * countCharacters;

            char *tester = zcalloc(1, len * booster + 1 + 8); /* 38.4 MB */
            char *startTester = tester + alignmentOffset;

            for (size_t i = 0; i < len * booster; i += len) {
                memcpy(startTester + i, foo, len);
            }

            const size_t loopBooster = 1000;
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

            zfree(tester);
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
        const size_t iterations = 2000000;

        for (size_t t = 0; t < sizeof(testNumbers) / sizeof(testNumbers[0]);
             t++) {
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
                ERR("Mismatch! SWAR=%lu Scalar=%lu", (unsigned long)resultSWAR,
                    (unsigned long)resultScalar);
            }
            printf("    %zu digits: result=%lu (verified)\n", len,
                   (unsigned long)resultSWAR);
        }
    }

    TEST("Benchmark: StrPopCnt NEON/SIMD vs Scalar") {
        printf("  Comparing popcount: Optimized vs Scalar (lookup table)...\n");
        const size_t bufSize = 4096;
        uint8_t *buf = zmalloc(bufSize);
        for (size_t i = 0; i < bufSize; i++) {
            buf[i] = (uint8_t)(i * 31 + 17);
        }

        const size_t iterations = 100000;
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
            ERR("Mismatch! Opt=%lu Scalar=%lu", (unsigned long)resultOpt,
                (unsigned long)resultScalar);
        }
        printf("    4KB popcount: %lu bits (verified)\n",
               (unsigned long)resultOpt);
        zfree(buf);
    }

    TEST("Benchmark: StrLenUtf8 SIMD vs Scalar") {
        printf("  Comparing UTF-8 strlen: SIMD vs Scalar...\n");
        const size_t bufSize = 4096;
        char *buf = zmalloc(bufSize);
        memset(buf, 'a', bufSize - 1);
        buf[bufSize - 1] = '\0';

        const size_t iterations = 100000;
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
        zfree(buf);
    }

    TEST("Benchmark: StrLenUtf8 on mixed UTF-8") {
        printf("  Comparing UTF-8 strlen on mixed content...\n");
        const char *mixedUtf8 = "Hello 世界! 🎉 Testing UTF-8 lengths 日本語";
        const size_t len = strlen(mixedUtf8);
        const size_t iterations = 2000000;
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
        const size_t iterations = 1000000;
        uint8_t buf[16];
        uint64_t checksum = 0;

        TIME_INIT;
        for (size_t i = 0; i < iterations; i++) {
            StrUInt9DigitsToBuf(buf, (uint32_t)(i % 1000000000));
            checksum += buf[0];
        }
        TIME_FINISH(iterations, "StrUInt9DigitsToBuf");

        printf("    Checksum: %lu\n", (unsigned long)checksum);
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
        char *longAscii = zmalloc(1024);
        for (int i = 0; i < 1023; i++) {
            longAscii[i] = 'A' + (i % 26);
        }
        longAscii[1023] = '\0';
        assert(StrUtf8Valid(longAscii, 1023) == true);
        assert(StrUtf8ValidScalar(longAscii, 1023) == true);
        zfree(longAscii);
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
        assert(StrUtf8ValidCStr(NULL) == false);
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

        for (size_t i = 0; i < COUNT_ARRAY(testCps); i++) {
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
            char *buf = zmalloc(size + 1);
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
            zfree(buf);
        }

        /* Test with mixed valid UTF-8 at various sizes */
        /* "日" = E6 97 A5 (3 bytes) */
        for (size_t numChars = 0; numChars <= 64; numChars++) {
            size_t size = numChars * 3;
            uint8_t *buf = zmalloc(size + 1);
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
            zfree(buf);
        }

        /* Test with single invalid byte at various positions */
        for (size_t size = 1; size <= 128; size++) {
            for (size_t badPos = 0; badPos < size; badPos++) {
                uint8_t *buf = zmalloc(size + 1);
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
                zfree(buf);
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
        const size_t iterations = 2000000;
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
        assert(pos == (const uint8_t *)"");
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
        cp = StrUtf8Peek(ascii, asciiLen, NULL);
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

        for (size_t i = 0; i < sizeof(testStrings) / sizeof(testStrings[0]);
             i++) {
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
        const size_t iterations = 1000000;
        const uint8_t *result;

        /* SIMD version */
        TIME_INIT;
        for (size_t i = 0; i < iterations; i++) {
            result = StrUtf8Advance(asciiHeavy, asciiLen, 45);
            (void)result;
        }
        TIME_FINISH(iterations, "SIMD Advance (ASCII)");

        /* Scalar version */
        TIME_INIT;
        for (size_t i = 0; i < iterations; i++) {
            result = StrUtf8AdvanceScalar(asciiHeavy, asciiLen, 45);
            (void)result;
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
            (void)result;
        }
        TIME_FINISH(iterations, "SIMD Advance (Mixed)");

        /* Scalar version on mixed */
        TIME_INIT;
        for (size_t i = 0; i < iterations; i++) {
            result = StrUtf8AdvanceScalar(mixedUtf8, mixedLen, mixedChars / 2);
            (void)result;
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
        written = StrUtf8SubstringCopy(mixed, mixedLen, 6, 8, NULL, 0);
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

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
