#include "str.h"

/* str.{c,h} contain utilities extracted from:
 *   - sqlite3 (public domain)
 *   - luajit (mit license)
 *   - other misc open licensed projects */

#include "datakit.h"

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
        /* This parses to 7074451188.5981045 which is ONE BIT HIGHER
         * than 7074451188.598104. Ugh. So we instituted forward/reverse
         * string generation testing in the conversion routine.
         *
         * Fun fact: 7074451188.5981035 == 7074451188.598104 as well,
         *           so good luck round tripping data (also another reason
         *           we're doing string double conversion checks even
         *           though they are slower and may require in-line
         *           bignum allocs */
        databox got;
        assert(StrScanScanReliable("7074451188.598104", 17, &got) == false);

#if 0
        assert(got.type == DATABOX_DOUBLE_64);
        assert(got.data.d64 == 7074451188.598104);
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
        if (!worked) {
            databoxReprSay("Expected success, but got", &result);
            assert(NULL);
        }

        if (result.data.d64 != 9543769205953.802734) {
            ERR("Expected %f but got %f!", 9543769205953.803, result.data.d64);
        }
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

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
