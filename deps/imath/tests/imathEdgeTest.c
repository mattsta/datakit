/* imath-edge: canary-bounded checks for imath's string writers and the
 * gmp_compat layer.
 *
 * Every writer is called at every limit from its minimum to the full
 * length + 3 inside a buffer pre-filled with 'X'. Each call must leave
 * every byte at or past `limit` untouched and leave a terminated
 * string. It must return MP_OK with the exact expected text when the
 * output fits, and MP_TRUNC otherwise. Expected strings are written
 * out by hand.
 *
 * Covered: mp_int_to_string, mp_rat_to_string, mp_rat_to_decimal
 * (including rounding that carries out of the fractional digits),
 * impz_get_str / impq_get_str, impq_set_str and
 * impz_export / impz_import.
 *
 * Built from imath.c, imrat.c and gmp_compat.c by deps/imath's
 * CMakeLists.txt and registered with ctest as imath-edge. */
#include "gmp_compat.h"
#include "imath.h"
#include "imrat.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUF 256
static int failures = 0;

#define FAIL(...)                                                              \
    do {                                                                       \
        printf("FAIL: " __VA_ARGS__);                                          \
        printf("\n");                                                          \
        failures++;                                                            \
    } while (0)

static int canaryIntact(const char *buf, size_t from) {
    for (size_t k = from; k < BUF; k++) {
        if (buf[k] != 'X') {
            return 0;
        }
    }
    return 1;
}

static void checkIntString(const char *digits, int negative) {
    mpz_t z;
    mp_int_init(&z);
    mp_int_read_string(&z, 10, digits);
    if (negative) {
        mp_int_neg(&z, &z);
    }

    char full[BUF];
    snprintf(full, sizeof(full), "%s%s", negative ? "-" : "", digits);
    const size_t fullLen = strlen(full);
    const size_t digitLen = strlen(digits);
    const size_t signLen = negative ? 1 : 0;

    for (size_t limit = 2; limit <= fullLen + 3; limit++) {
        char buf[BUF];
        memset(buf, 'X', sizeof(buf));
        const mp_result res = mp_int_to_string(&z, 10, buf, limit);
        if (!canaryIntact(buf, limit)) {
            FAIL("int_to_string %s wrote past limit %zu", full, limit);
        }
        if (memchr(buf, '\0', limit) == NULL) {
            FAIL("int_to_string %s unterminated at limit %zu", full, limit);
            continue;
        }
        if (limit > fullLen) {
            if (res != MP_OK || strcmp(buf, full) != 0) {
                FAIL("int_to_string limit %zu: res %d got \"%s\"", limit, res,
                     buf);
            }
            continue;
        }

        /* Truncated: the sign, then the lowest-order digits that fit */
        const size_t keep = limit - 1 - signLen;
        if (res != MP_TRUNC || strlen(buf) != limit - 1 ||
            (negative && buf[0] != '-') ||
            memcmp(buf + signLen, digits + (digitLen - keep), keep) != 0) {
            FAIL("int_to_string %s truncated at limit %zu: res %d got \"%s\"",
                 full, limit, res, buf);
        }
    }
    mp_int_clear(&z);
}

typedef struct {
    mp_small num, den;
    mp_size prec;
    mp_round_mode round;
    const char *want;
} decCase;

static void checkDecimal(const decCase *c) {
    mpq_t q;
    mp_rat_init(&q);
    mp_rat_set_value(&q, c->num, c->den);
    const size_t fullLen = strlen(c->want);

    for (size_t limit = 0; limit <= fullLen + 3; limit++) {
        char buf[BUF];
        memset(buf, 'X', sizeof(buf));
        const mp_result res =
            mp_rat_to_decimal(&q, 10, c->prec, c->round, buf, (int)limit);
        if (!canaryIntact(buf, limit)) {
            FAIL("decimal %" PRId64 "/%" PRId64 " wrote past limit %zu",
                 (int64_t)c->num, (int64_t)c->den, limit);
        }
        if (limit == 0) {
            if (res != MP_TRUNC || buf[0] != 'X') {
                FAIL("decimal limit 0: res %d, wrote %c", res, buf[0]);
            }
            continue;
        }
        if (memchr(buf, '\0', limit) == NULL) {
            FAIL("decimal %" PRId64 "/%" PRId64 " unterminated at limit %zu",
                 (int64_t)c->num, (int64_t)c->den, limit);
            continue;
        }
        if (limit > fullLen) {
            if (res != MP_OK || strcmp(buf, c->want) != 0) {
                FAIL("decimal %" PRId64 "/%" PRId64
                     " prec %u limit %zu: res %d got \"%s\" want \"%s\"",
                     (int64_t)c->num, (int64_t)c->den, c->prec, limit, res, buf,
                     c->want);
            }
        } else if (res != MP_TRUNC) {
            FAIL("decimal %" PRId64 "/%" PRId64
                 " limit %zu: res %d (want MP_TRUNC), got \"%s\"",
                 (int64_t)c->num, (int64_t)c->den, limit, res, buf);
        }
    }
    mp_rat_clear(&q);
}

static void checkRatString(mp_small num, mp_small den, const char *want) {
    mpq_t q;
    mp_rat_init(&q);
    mp_rat_set_value(&q, num, den);
    const size_t fullLen = strlen(want);

    for (size_t limit = 2; limit <= fullLen + 3; limit++) {
        char buf[BUF];
        memset(buf, 'X', sizeof(buf));
        const mp_result res = mp_rat_to_string(&q, 10, buf, (int)limit);
        if (!canaryIntact(buf, limit)) {
            FAIL("rat_to_string %s wrote past limit %zu", want, limit);
        }
        if (memchr(buf, '\0', limit) == NULL) {
            FAIL("rat_to_string %s unterminated at limit %zu", want, limit);
            continue;
        }
        if (limit > fullLen) {
            if (res != MP_OK || strcmp(buf, want) != 0) {
                FAIL("rat_to_string limit %zu: res %d got \"%s\"", limit, res,
                     buf);
            }
        } else if (res != MP_TRUNC) {
            FAIL("rat_to_string %s limit %zu: res %d (want MP_TRUNC)", want,
                 limit, res);
        }
    }
    mp_rat_clear(&q);
}

static void checkGetStr(void) {
    mpz_t z;
    mp_int_init(&z);
    const char *wantLower = "-bfd5e0e56c0f8fb2d43bd2e000000000";
    const char *wantUpper = "-BFD5E0E56C0F8FB2D43BD2E000000000";
    mp_int_read_string(&z, 16, wantUpper);

    /* Lower and upper case, allocated by get_str */
    char *lower = impz_get_str(NULL, 16, &z);
    char *upper = impz_get_str(NULL, -16, &z);
    if (lower == NULL || strcmp(lower, wantLower) != 0) {
        FAIL("impz_get_str radix 16 got \"%s\"", lower ? lower : "(null)");
    }
    if (upper == NULL || strcmp(upper, wantUpper) != 0) {
        FAIL("impz_get_str radix -16 got \"%s\"", upper ? upper : "(null)");
    }
    free(lower);
    free(upper);

    /* Caller buffer: only the string and its terminator are written; the
     * case loop must stop at the terminator, not at the length estimate */
    char buf[BUF];
    memset(buf, 'X', sizeof(buf));
    if (impz_get_str(buf, -16, &z) != buf || strcmp(buf, wantUpper) != 0 ||
        !canaryIntact(buf, strlen(wantUpper) + 1)) {
        FAIL("impz_get_str into a caller buffer");
    }

    /* Unsupported radixes */
    if (impz_get_str(NULL, 37, &z) != NULL ||
        impz_get_str(NULL, 1, &z) != NULL ||
        impz_get_str(NULL, -37, &z) != NULL ||
        impz_get_str(NULL, INT32_MIN, &z) != NULL) {
        FAIL("impz_get_str accepted an unsupported radix");
    }
    mp_int_clear(&z);

    mpq_t q;
    mp_rat_init(&q);
    mp_rat_set_value(&q, -2748, 7); /* -0xABC / 7 */
    char *qs = impq_get_str(NULL, -16, &q);
    if (qs == NULL || strcmp(qs, "-ABC/7") != 0) {
        FAIL("impq_get_str got \"%s\"", qs ? qs : "(null)");
    }
    free(qs);
    memset(buf, 'X', sizeof(buf));
    if (impq_get_str(buf, 16, &q) != buf || strcmp(buf, "-abc/7") != 0 ||
        !canaryIntact(buf, strlen("-abc/7") + 1)) {
        FAIL("impq_get_str into a caller buffer");
    }
    mp_rat_clear(&q);

    mpq_t parsed;
    mp_rat_init(&parsed);
    if (impq_set_str(&parsed, "-22/7", 10) != 0 ||
        mp_rat_compare_value(&parsed, -22, 7) != 0) {
        FAIL("impq_set_str round trip");
    }
    mp_rat_clear(&parsed);
}

static void checkExportImport(void) {
    mpz_t z, back;
    mp_int_init(&z);
    mp_int_init(&back);
    mp_int_read_string(&z, 16, "123456789ABCDEF0FEDCBA987654321F00D");

    static const size_t sizes[] = {1, 2, 3, 4, 8, 16};
    static const int orders[] = {1, -1};
    static const int endians[] = {1, -1, 0};
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        for (size_t o = 0; o < 2; o++) {
            for (size_t e = 0; e < 3; e++) {
                size_t count = 0;
                void *words = impz_export(NULL, &count, orders[o], sizes[s],
                                          endians[e], 0, &z);
                if (words == NULL || count == 0) {
                    FAIL("impz_export size %zu returned no words", sizes[s]);
                    continue;
                }
                mp_int_zero(&back);
                impz_import(&back, count, orders[o], sizes[s], endians[e], 0,
                            words);
                if (mp_int_compare(&z, &back) != 0) {
                    FAIL("export/import round trip size %zu order %d "
                         "endian %d",
                         sizes[s], orders[o], endians[e]);
                }
                free(words);
            }
        }
    }

    /* A zero word size is rejected and leaves rop unchanged */
    size_t count = 99;
    if (impz_export(NULL, &count, 1, 0, 1, 0, &z) != NULL || count != 0) {
        FAIL("impz_export accepted a zero word size");
    }
    mp_int_set_value(&back, 42);
    unsigned char one = 1;
    impz_import(&back, 1, 1, 0, 1, 0, &one);
    if (mp_int_compare_value(&back, 42) != 0) {
        FAIL("impz_import with a zero word size changed rop");
    }

    /* Unsupported nails, order or endian fail in every build */
    count = 99;
    if (impz_export(NULL, &count, 1, 1, 1, 1, &z) != NULL || count != 0 ||
        impz_export(NULL, &count, 2, 1, 1, 0, &z) != NULL ||
        impz_export(NULL, &count, 1, 1, 2, 0, &z) != NULL) {
        FAIL("impz_export accepted unsupported nails/order/endian");
    }
    impz_import(&back, 1, 1, 1, 1, 1, &one);
    impz_import(&back, 1, 0, 1, 1, 0, &one);
    impz_import(&back, 1, 1, 1, -2, 0, &one);
    if (mp_int_compare_value(&back, 42) != 0) {
        FAIL("impz_import with unsupported nails/order/endian changed rop");
    }

    /* sizeinbase: exact or one over for a supported base, 0 otherwise */
    mpz_t v;
    mp_int_init_value(&v, -255);
    const size_t hexDigits = impz_sizeinbase(&v, 16);
    if (hexDigits < 2 || hexDigits > 3 || impz_sizeinbase(&v, 37) != 0) {
        FAIL("impz_sizeinbase(-255) got %zu in base 16", hexDigits);
    }
    mp_int_clear(&v);

    mp_int_clear(&z);
    mp_int_clear(&back);
}

int main(void) {
    checkIntString("7", 0);
    checkIntString("7", 1);
    checkIntString("1234567890123456789012345678901234567890", 0);
    checkIntString("1234567890123456789012345678901234567890", 1);

    static const decCase cases[] = {
        {-22, 7, 5, MP_ROUND_DOWN, "-3.14285"},
        {1, 8, 3, MP_ROUND_DOWN, "0.125"},
        {1, 1000, 5, MP_ROUND_DOWN, "0.00100"},
        {-1, 3, 4, MP_ROUND_DOWN, "-0.3333"},
        {2, 3, 4, MP_ROUND_HALF_UP, "0.6667"},
        {5, 1, 2, MP_ROUND_DOWN, "5.00"},
        {0, 1, 3, MP_ROUND_DOWN, "0.000"},
        {-7, 2, 0, MP_ROUND_DOWN, "-3"},
        {123456789, 1000, 2, MP_ROUND_DOWN, "123456.78"},
        /* Rounding that carries out of the fractional digits */
        {96, 100, 1, MP_ROUND_UP, "1.0"},
        {-96, 100, 1, MP_ROUND_UP, "-1.0"},
        {1996, 1000, 2, MP_ROUND_HALF_UP, "2.00"},
        {9996, 1000, 2, MP_ROUND_HALF_UP, "10.00"},
        {96, 1000, 2, MP_ROUND_HALF_UP, "0.10"},
        {96, 10000, 3, MP_ROUND_HALF_UP, "0.010"},
        {5, 1000, 2, MP_ROUND_HALF_UP, "0.01"},
        {5, 1000, 2, MP_ROUND_HALF_DOWN, "0.00"},
        {999, 1000, 2, MP_ROUND_DOWN, "0.99"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        checkDecimal(&cases[i]);
    }

    checkRatString(-22, 7, "-22/7");
    checkRatString(1, 3, "1/3");
    checkRatString(-123456789, 1000, "-123456789/1000");
    checkRatString(0, 5, "0");

    checkGetStr();
    checkExportImport();

    printf("imath-edge: %s (%d failures)\n", failures ? "FAILED" : "PASSED",
           failures);
    return failures ? 1 : 0;
}
