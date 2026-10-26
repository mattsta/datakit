#include "bigmath.h"
#include "datakit.h"

/* The full imath surface (exptmod, gcd, invmod, root, codecs, ...) is
 * retained as future platform capability even though the bigmath
 * wrapper does not expose it yet; it is tested here directly. */
#include "../deps/imath/imath.h"

#ifdef DATAKIT_TEST

#include "cleaner.h"
#include "ctest.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

/* Deterministic xorshift so failures reproduce */
#define BM_NEXT() (rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17, rng)

/* Build a bigmath from a full 128-bit oracle value: hi << 64 | lo */
static void bigmathFromU128(bigmath *b, const __uint128_t v) {
    bigmath lo;
    bigmathInitUnsigned(b, (uint64_t)(v >> 64));
    bigmathShiftLeft(b, 64, b);
    bigmathInitUnsigned(&lo, (uint64_t)v);
    bigmathAdd(b, &lo, b);
    bigmathReset(&lo);
}

/* Compare a bigmath against a 128-bit oracle value */
static bool bigmathEqualsU128(bigmath *b, const __uint128_t v) {
    bigmath expect;
    bigmathFromU128(&expect, v);
    const int cmp = bigmathCompare(b, &expect);
    bigmathReset(&expect);
    return cmp == 0;
}

defcleanerName(char *, zfree, Chars);

/* Fill digits[0..n-1] with a deterministic decimal string with a
 * nonzero leading digit, and terminate it at digits[n]. */
static void bigmathTestDigits(char *digits, const size_t n, uint64_t rng) {
    for (size_t i = 0; i < n; i++) {
        digits[i] = (char)('0' + BM_NEXT() % 10);
    }
    if (n > 0 && digits[0] == '0') {
        digits[0] = '7';
    }
    digits[n] = '\0';
}

/* Build the value of a decimal digit string by Horner's rule
 * (b = b * 10 + digit), independent of imath's string reader. */
static void bigmathFromDigits(bigmath *b, const char *digits) {
    bigmathInitUnsigned(b, 0);
    for (const char *p = digits; *p; p++) {
        bigmath d;
        bigmathMultiplyValue(b, 10, b);
        bigmathInitUnsigned(&d, (uint64_t)(*p - '0'));
        bigmathAdd(b, &d, b);
        bigmathFree(&d);
    }
}

/* Print b with bigmathReprFile into a temporary file and check the
 * file holds exactly want and a newline. */
static bool bigmathTestReprIs(bigmath *b, const char *want) {
    FILE *fp = tmpfile();
    if (fp == NULL) {
        return false;
    }

    bigmathReprFile(b, fp);
    const size_t wantLen = strlen(want);
    const bool sizeOk = ftello(fp) == (off_t)(wantLen + 1);
    rewind(fp);

    cleanerName(zfree, Chars) char *got = zmalloc(wantLen + 2);
    const size_t n = fread(got, 1, wantLen + 2, fp);
    fclose(fp);

    return sizeOk && n == wantLen + 1 && memcmp(got, want, wantLen) == 0 &&
           got[wantLen] == '\n';
}

int bigmathTest(int, char *[]) {
    int err = 0;

    TEST("native round trips and ordering") {
        const int64_t signedVals[] = {0,  1,  -1, INT64_MAX, INT64_MIN + 1,
                                      42, -42};
        for (size_t i = 0; i < countof(signedVals); i++) {
            bigmath b;
            bigmathInitSigned(&b, signedVals[i]);
            if (bigmathToNativeSigned(&b) != signedVals[i]) {
                ERR("Signed round trip failed for %" PRId64, signedVals[i]);
            }
            bigmathReset(&b);
        }

        const uint64_t unsignedVals[] = {0, 1, UINT64_MAX, 1ULL << 63,
                                         10000000000000000000ULL};
        for (size_t i = 0; i < countof(unsignedVals); i++) {
            bigmath b;
            bigmathInitUnsigned(&b, unsignedVals[i]);
            if (bigmathToNativeUnsigned(&b) != unsignedVals[i]) {
                ERR("Unsigned round trip failed for %" PRIu64, unsignedVals[i]);
            }
            bigmathReset(&b);
        }
    }

    TEST("FUZZ: add/multiply/shift/compare vs 128-bit oracle") {
        uint64_t rng = 0x0123456789ABCDEFULL;
        for (int32_t t = 0; t < 5000; t++) {
            const uint64_t a = BM_NEXT();
            const uint64_t b = BM_NEXT();
            const uint32_t sh = (uint32_t)(BM_NEXT() % 64);

            bigmath A;
            bigmath B;
            bigmath R;
            bigmathInitUnsigned(&A, a);
            bigmathInitUnsigned(&B, b);
            bigmathInit(&R);

            bigmathAdd(&A, &B, &R);
            if (!bigmathEqualsU128(&R, (__uint128_t)a + b)) {
                ERR("Add oracle mismatch: %" PRIu64 " + %" PRIu64, a, b);
            }

            bigmathMultiply(&A, &B, &R);
            if (!bigmathEqualsU128(&R, (__uint128_t)a * b)) {
                ERR("Multiply oracle mismatch: %" PRIu64 " * %" PRIu64, a, b);
            }

            bigmathShiftLeft(&A, sh, &R);
            if (!bigmathEqualsU128(&R, (__uint128_t)a << sh)) {
                ERR("ShiftLeft oracle mismatch: %" PRIu64 " << %u", a, sh);
            }

            const int cmp = bigmathCompare(&A, &B);
            const int want = a < b ? -1 : a > b ? 1 : 0;
            if ((cmp < 0 ? -1 : cmp > 0 ? 1 : 0) != want) {
                ERR("Compare oracle mismatch: %" PRIu64 " vs %" PRIu64, a, b);
            }

            bigmathReset(&A);
            bigmathReset(&B);
            bigmathReset(&R);
        }
    }

    TEST("FUZZ: divide-remainder vs 128-bit oracle") {
        uint64_t rng = 0xFEDCBA9876543210ULL;
        for (int32_t t = 0; t < 5000; t++) {
            const uint64_t a = BM_NEXT();
            const uint64_t b = BM_NEXT();
            const uint64_t d = (BM_NEXT() % 1'000'000) + 1;

            /* top = a * b (up to 128 bits), bottom = small divisor */
            const __uint128_t top = (__uint128_t)a * b;

            bigmath TOP;
            bigmath BOT;
            bigmath Q;
            bigmath REM;
            bigmathFromU128(&TOP, top);
            bigmathInitUnsigned(&BOT, d);
            bigmathInit(&Q);
            bigmathInit(&REM);

            bigmathDivideRemainder(&TOP, &BOT, &Q, &REM);
            if (!bigmathEqualsU128(&Q, top / d)) {
                ERR("Quotient mismatch at trial %d (divisor %" PRIu64 ")", t,
                    d);
            }
            if (bigmathToNativeUnsigned(&REM) != (uint64_t)(top % d)) {
                ERR("Remainder mismatch at trial %d (divisor %" PRIu64 ")", t,
                    d);
            }

            bigmathReset(&TOP);
            bigmathReset(&BOT);
            bigmathReset(&Q);
            bigmathReset(&REM);
        }
    }

    TEST("FUZZ: beyond-128-bit algebraic identities") {
        /* No native oracle exists above 128 bits, so pin operations
         * against each other: (a * base^k) / base^k == a rem 0, and
         * shiftLeft(n) == multiply by 2^n */
        uint64_t rng = 0x1122334455667788ULL;
        for (int32_t t = 0; t < 2000; t++) {
            const uint64_t a = BM_NEXT() | 1;
            const uint64_t k = (BM_NEXT() % 200) + 1;
            const uint32_t n = (uint32_t)(BM_NEXT() % 300) + 1;

            bigmath A;
            bigmath P;
            bigmath T;
            bigmath Q;
            bigmath REM;
            bigmathInitUnsigned(&A, a);
            bigmathInit(&P);
            bigmathInit(&T);
            bigmathInit(&Q);
            bigmathInit(&REM);

            bigmathExponent(10, k, &P);
            bigmathMultiply(&A, &P, &T);
            bigmathDivideRemainder(&T, &P, &Q, &REM);
            if (bigmathCompare(&Q, &A) != 0 ||
                bigmathToNativeUnsigned(&REM) != 0) {
                ERR("(a * 10^%" PRIu64 ") / 10^%" PRIu64 " != a for a=%" PRIu64,
                    k, k, a);
            }

            bigmathShiftLeft(&A, n, &T);
            bigmathExponent(2, n, &P);
            bigmathMultiply(&A, &P, &Q);
            if (bigmathCompare(&T, &Q) != 0) {
                ERR("shiftLeft(%u) != multiply 2^%u for a=%" PRIu64, n, n, a);
            }

            bigmathReset(&A);
            bigmathReset(&P);
            bigmathReset(&T);
            bigmathReset(&Q);
            bigmathReset(&REM);
        }
    }

    TEST("string parsing round trips") {
        bigmath b;
        bigmath expect;

        if (!bigmathInitFromString(&b, "0")) {
            ERR("Failed to parse zero");
        }
        if (bigmathToNativeUnsigned(&b) != 0) {
            ERR("Zero string parsed wrong");
        }
        bigmathReset(&b);

        if (!bigmathInitFromString(&b, "18446744073709551615")) {
            ERR("Failed to parse UINT64_MAX");
        }
        if (bigmathToNativeUnsigned(&b) != UINT64_MAX) {
            ERR("UINT64_MAX string parsed wrong");
        }
        bigmathReset(&b);

        if (!bigmathInitFromString(&b, "-12345678901234567")) {
            ERR("Failed to parse negative");
        }
        if (bigmathToNativeSigned(&b) != -12345678901234567LL) {
            ERR("Negative string parsed wrong");
        }
        bigmathReset(&b);

        /* Past the inline-buffer spill: 10^40 as a string must equal
         * 10^40 built arithmetically */
        if (!bigmathInitFromString(
                &b, "10000000000000000000000000000000000000000")) {
            ERR("Failed to parse 10^40");
        }
        bigmathInit(&expect);
        bigmathExponent(10, 40, &expect);
        if (bigmathCompare(&b, &expect) != 0) {
            ERR("10^40 string mismatch vs arithmetic");
        }
        bigmathReset(&b);
        bigmathReset(&expect);

        if (bigmathInitFromString(&b, "not a number")) {
            ERR("Parsed garbage as a number");
        }
        bigmathReset(&b);
    }

    TEST("FUZZ: signed divide across all sign combinations") {
        uint64_t rng = 0x2468ACE013579BDFULL;
        for (int32_t t = 0; t < 3000; t++) {
            const int64_t a = (int64_t)(BM_NEXT() >> 1);
            const int64_t d = (int64_t)((BM_NEXT() % 1'000'000) + 1);
            const int64_t sa = (t & 1) ? -a : a;
            const int64_t sd = (t & 2) ? -d : d;

            bigmath TOP;
            bigmath BOT;
            bigmath Q;
            bigmath REM;
            bigmathInitSigned(&TOP, sa);
            bigmathInitSigned(&BOT, sd);
            bigmathInit(&Q);
            bigmathInit(&REM);

            bigmathDivideRemainder(&TOP, &BOT, &Q, &REM);
            if (bigmathToNativeSigned(&Q) != sa / sd ||
                bigmathToNativeSigned(&REM) != sa % sd) {
                ERR("Signed divide wrong: %" PRId64 " / %" PRId64
                    " got q=%" PRId64 " r=%" PRId64,
                    sa, sd, bigmathToNativeSigned(&Q),
                    bigmathToNativeSigned(&REM));
            }

            bigmathReset(&TOP);
            bigmathReset(&BOT);
            bigmathReset(&Q);
            bigmathReset(&REM);
        }
    }

    TEST("FUZZ: DivideDigitInPlace extracts digit and remainder") {
        uint64_t rng = 0x13579BDF2468ACE0ULL;
        for (int32_t t = 0; t < 3000; t++) {
            const uint64_t bottom = (BM_NEXT() >> 4) + 1;
            const uint32_t digit = (uint32_t)(BM_NEXT() % 10);
            const uint64_t rem = BM_NEXT() % bottom;

            /* top = bottom * digit + rem, all within the contract */
            bigmath TOP;
            bigmath BOT;
            bigmath EXPECT;
            bigmathInitUnsigned(&BOT, bottom);
            bigmathInitUnsigned(&TOP, bottom);
            bigmathMultiplyValue(&TOP, digit, &TOP);
            bigmathInitUnsigned(&EXPECT, rem);
            bigmathAdd(&TOP, &EXPECT, &TOP);

            const uint32_t got = bigmathDivideDigitInPlace(&TOP, &BOT);
            if (got != digit) {
                ERR("Digit wrong at trial %d: got %u want %u", t, got, digit);
            }
            if (bigmathCompare(&TOP, &EXPECT) != 0) {
                ERR("Remainder wrong at trial %d", t);
            }

            bigmathReset(&TOP);
            bigmathReset(&BOT);
            bigmathReset(&EXPECT);
        }
    }

    TEST("inline-buffer spill boundary stays exact") {
        /* single-digit storage covers 384 bits; pin both sides of the
         * heap-spill edge with the shift/multiply identity */
        const uint32_t edges[] = {382, 383, 384, 385, 700};
        for (size_t i = 0; i < countof(edges); i++) {
            bigmath A;
            bigmath T;
            bigmath P;
            bigmath Q;
            bigmath REM;
            bigmathInitUnsigned(&A, 0xDEADBEEFULL);
            bigmathInit(&T);
            bigmathInit(&P);
            bigmathInit(&Q);
            bigmathInit(&REM);

            bigmathShiftLeft(&A, edges[i], &T);
            bigmathExponent(2, edges[i], &P);
            bigmathDivideRemainder(&T, &P, &Q, &REM);
            if (bigmathCompare(&Q, &A) != 0 ||
                bigmathToNativeUnsigned(&REM) != 0) {
                ERR("Spill boundary %u broke shift/divide identity", edges[i]);
            }

            bigmathReset(&A);
            bigmathReset(&T);
            bigmathReset(&P);
            bigmathReset(&Q);
            bigmathReset(&REM);
        }
    }

    TEST("Karatsuba threshold crossings stay exact") {
        /* MP_MULT_THRESH is 22 digits (704 bits); verify multiply at
         * sizes below/at/above via the (x*y)/y == x identity */
        const uint32_t bits[] = {640, 672, 704, 736, 1408};
        for (size_t i = 0; i < countof(bits); i++) {
            bigmath X;
            bigmath Y;
            bigmath T;
            bigmath Q;
            bigmath REM;
            bigmathInitUnsigned(&X, 0x123456789ABCDEFULL);
            bigmathShiftLeft(&X, bits[i], &X);
            bigmathInitUnsigned(&Y, 0xFEDCBA987654321ULL);
            bigmathShiftLeft(&Y, bits[i], &Y);
            bigmathInit(&T);
            bigmathInit(&Q);
            bigmathInit(&REM);

            bigmathMultiply(&X, &Y, &T);
            bigmathDivideRemainder(&T, &Y, &Q, &REM);
            if (bigmathCompare(&Q, &X) != 0 ||
                bigmathToNativeUnsigned(&REM) != 0) {
                ERR("Multiply broke (x*y)/y identity at %u bits", bits[i]);
            }

            bigmathReset(&X);
            bigmathReset(&Y);
            bigmathReset(&T);
            bigmathReset(&Q);
            bigmathReset(&REM);
        }
    }

    TEST("exponent edge cases") {
        bigmath R;
        bigmathInit(&R);

        bigmathExponent(10, 0, &R);
        if (bigmathToNativeUnsigned(&R) != 1) {
            ERR("10^0 != 1");
        }

        bigmathExponent(0, 5, &R);
        if (bigmathToNativeUnsigned(&R) != 0) {
            ERR("0^5 != 0");
        }

        bigmathExponent(2, 0, &R);
        if (bigmathToNativeUnsigned(&R) != 1) {
            ERR("2^0 != 1");
        }

        bigmathReset(&R);
    }

    TEST("FUZZ: result aliasing an operand matches non-aliased") {
        /* The backend contract allows result == operand; verify every
         * exported operation against its non-aliased twin */
        uint64_t rng = 0x0F0F0F0FF0F0F0F0ULL;
        for (int32_t t = 0; t < 2000; t++) {
            const uint64_t a = BM_NEXT();
            const uint64_t b = BM_NEXT();
            const uint32_t sh = (uint32_t)(BM_NEXT() % 200);

            bigmath A;
            bigmath B;
            bigmath REF;
            bigmathInitUnsigned(&A, a);
            bigmathInitUnsigned(&B, b);
            bigmathInit(&REF);

            bigmathAdd(&A, &B, &REF);
            bigmathAdd(&A, &B, &A);
            if (bigmathCompare(&A, &REF) != 0) {
                ERR("Aliased Add diverged at trial %d", t);
            }

            bigmathInitUnsigned(&A, a);
            bigmathMultiply(&A, &B, &REF);
            bigmathMultiply(&A, &B, &A);
            if (bigmathCompare(&A, &REF) != 0) {
                ERR("Aliased Multiply diverged at trial %d", t);
            }

            bigmathInitUnsigned(&A, a);
            bigmathShiftLeft(&A, sh, &REF);
            bigmathShiftLeft(&A, sh, &A);
            if (bigmathCompare(&A, &REF) != 0) {
                ERR("Aliased ShiftLeft diverged at trial %d", t);
            }

            bigmathInitUnsigned(&A, a);
            bigmathMultiplyValue(&A, 10, &REF);
            bigmathMultiplyValue(&A, 10, &A);
            if (bigmathCompare(&A, &REF) != 0) {
                ERR("Aliased MultiplyValue diverged at trial %d", t);
            }

            bigmathReset(&A);
            bigmathReset(&B);
            bigmathReset(&REF);
        }
    }

    TEST("reset then reuse leaves no state bleed") {
        bigmath b;
        bigmathInitUnsigned(&b, 12345);
        bigmathMultiplyValue(&b, 1'000'000, &b);
        bigmathReset(&b);

        bigmathInitUnsigned(&b, 7);
        if (bigmathToNativeUnsigned(&b) != 7) {
            ERR("Reused bigmath carried stale state");
        }
        bigmathReset(&b);
    }

    TEST("copy is independent of source mutation") {
        bigmath src;
        bigmath copy;
        bigmathInitUnsigned(&src, 999);
        bigmathInitCopy(&copy, &src);
        bigmathMultiplyValue(&src, 1000, &src);

        if (bigmathToNativeUnsigned(&copy) != 999) {
            ERR("Copy mutated alongside source");
        }

        bigmathReset(&src);
        bigmathReset(&copy);
    }

    TEST("multiplyValue negative and in-place aliasing") {
        bigmath b;
        bigmathInitSigned(&b, 21);
        bigmathMultiplyValue(&b, -2, &b);
        if (bigmathToNativeSigned(&b) != -42) {
            ERR("In-place signed multiply wrong: %" PRId64,
                bigmathToNativeSigned(&b));
        }
        bigmathReset(&b);
    }

    TEST("imath: FUZZ exptmod vs modular square-multiply oracle") {
        uint64_t rng = 0x9D8C7B6A5F4E3D2CULL;
        for (int32_t t = 0; t < 500; t++) {
            const uint64_t base = BM_NEXT() % 100000;
            const uint64_t expo = BM_NEXT() % 64;
            const uint64_t mod = (BM_NEXT() % 999999) + 2;

            /* Oracle: __uint128_t modular exponentiation */
            __uint128_t want = 1;
            __uint128_t acc = base % mod;
            for (uint64_t e = expo; e; e >>= 1) {
                if (e & 1) {
                    want = (want * acc) % mod;
                }
                acc = (acc * acc) % mod;
            }

            mpz_t A;
            mpz_t B;
            mpz_t M;
            mpz_t C;
            mp_int_init_uvalue(&A, base);
            mp_int_init_uvalue(&B, expo);
            mp_int_init_uvalue(&M, mod);
            mp_int_init(&C);

            const mp_result res = mp_int_exptmod(&A, &B, &M, &C);
            mp_usmall got = 0;
            mp_int_to_uint(&C, &got);
            if (res != MP_OK || got != (uint64_t)want) {
                ERR("exptmod %" PRIu64 "^%" PRIu64 " mod %" PRIu64
                    ": res %d got %" PRIu64 " want %" PRIu64,
                    base, expo, mod, res, (uint64_t)got, (uint64_t)want);
            }

            mp_int_clear(&A);
            mp_int_clear(&B);
            mp_int_clear(&M);
            mp_int_clear(&C);
        }
    }

    TEST("imath: FUZZ gcd/lcm/invmod identities") {
        uint64_t rng = 0x1F2E3D4C5B6A7988ULL;
        for (int32_t t = 0; t < 1000; t++) {
            const uint64_t a = (BM_NEXT() % 1'000'000) + 1;
            const uint64_t b = (BM_NEXT() % 1'000'000) + 1;

            /* Euclid oracle */
            uint64_t x = a;
            uint64_t y = b;
            while (y) {
                const uint64_t r = x % y;
                x = y;
                y = r;
            }
            const uint64_t g = x;

            mpz_t A;
            mpz_t B;
            mpz_t R;
            mp_int_init_uvalue(&A, a);
            mp_int_init_uvalue(&B, b);
            mp_int_init(&R);

            mp_usmall got = 0;
            if (mp_int_gcd(&A, &B, &R) != MP_OK ||
                (mp_int_to_uint(&R, &got), got) != g) {
                ERR("gcd(%" PRIu64 ",%" PRIu64 ") got %" PRIu64
                    " want %" PRIu64,
                    a, b, (uint64_t)got, g);
            }

            if (mp_int_lcm(&A, &B, &R) != MP_OK ||
                (mp_int_to_uint(&R, &got), got) != (a / g) * b) {
                ERR("lcm(%" PRIu64 ",%" PRIu64 ") got %" PRIu64, a, b,
                    (uint64_t)got);
            }

            /* invmod: a * a^-1 == 1 (mod m) whenever coprime */
            const uint64_t m = 1000003; /* prime */
            mpz_t M;
            mp_int_init_uvalue(&M, m);
            if (a % m != 0) {
                if (mp_int_invmod(&A, &M, &R) != MP_OK) {
                    ERR("invmod(%" PRIu64 ", %" PRIu64 ") failed", a, m);
                } else {
                    mp_int_to_uint(&R, &got);
                    if (((__uint128_t)(a % m) * got) % m != 1) {
                        ERR("invmod(%" PRIu64 ") * a != 1 mod %" PRIu64, a, m);
                    }
                }
            }
            mp_int_clear(&M);

            mp_int_clear(&A);
            mp_int_clear(&B);
            mp_int_clear(&R);
        }
    }

    TEST("imath: mod rejects a negative modulus") {
        mpz_t A;
        mpz_t M;
        mpz_t R;
        mp_int_init_uvalue(&A, 17);
        mp_int_init_value(&M, -5);
        mp_int_init(&R);

        if (mp_int_mod(&A, &M, &R) != MP_RANGE) {
            ERR("mod by a negative modulus must return MP_RANGE");
        }

        mp_int_set_value(&M, 5);
        mp_small got = 0;
        if (mp_int_mod(&A, &M, &R) != MP_OK ||
            (mp_int_to_int(&R, &got), got) != 2) {
            ERR("17 mod 5 should be 2");
        }

        mp_int_clear(&A);
        mp_int_clear(&M);
        mp_int_clear(&R);
    }

    TEST("imath: root recovers exact powers and rejects even roots of "
         "negatives") {
        uint64_t rng = 0xABCDEF0123456789ULL;
        for (int32_t t = 0; t < 300; t++) {
            const uint64_t base = (BM_NEXT() % 1000) + 2;
            const uint64_t k = (BM_NEXT() % 5) + 2;

            mpz_t B;
            mpz_t P;
            mpz_t R;
            mp_int_init_uvalue(&B, base);
            mp_int_init(&P);
            mp_int_init(&R);

            mp_int_expt_value((mp_small)base, (mp_small)k, &P);
            mp_usmall got = 0;
            if (mp_int_root(&P, (mp_small)k, &R) != MP_OK ||
                (mp_int_to_uint(&R, &got), got) != base) {
                ERR("root_%" PRIu64 "(%" PRIu64 "^%" PRIu64 ") got %" PRIu64, k,
                    base, k, (uint64_t)got);
            }

            mp_int_clear(&B);
            mp_int_clear(&P);
            mp_int_clear(&R);
        }

        mpz_t N;
        mpz_t R;
        mp_int_init_value(&N, -8);
        mp_int_init(&R);
        if (mp_int_root(&N, 2, &R) == MP_OK) {
            ERR("Even root of a negative should fail");
        }
        mp_small sgot = 0;
        if (mp_int_root(&N, 3, &R) != MP_OK ||
            (mp_int_to_int(&R, &sgot), sgot) != -2) {
            ERR("Cube root of -8 should be -2");
        }
        mp_int_clear(&N);
        mp_int_clear(&R);
    }

    TEST("imath: binary codec round trips including sign") {
        const int64_t vals[] = {0,   1,    -1,        127,          -128,
                                255, -256, INT64_MAX, INT64_MIN + 1};
        for (size_t i = 0; i < countof(vals); i++) {
            mpz_t A;
            mpz_t B;
            mp_int_init_value(&A, vals[i]);
            mp_int_init(&B);

            uint8_t buf[32] = {};
            const mp_result lenRes = mp_int_binary_len(&A);
            if (lenRes <= 0 || (size_t)lenRes > sizeof(buf)) {
                ERR("binary_len bogus for %" PRId64, vals[i]);
            } else if (mp_int_to_binary(&A, buf, lenRes) != MP_OK ||
                       mp_int_read_binary(&B, buf, lenRes) != MP_OK ||
                       mp_int_compare(&A, &B) != 0) {
                ERR("binary round trip failed for %" PRId64, vals[i]);
            }

            mp_int_clear(&A);
            mp_int_clear(&B);
        }
    }

    TEST("imath: is_pow2 contract including zero") {
        mpz_t Z;
        mp_int_init(&Z);
        if (mp_int_is_pow2(&Z) >= 0) {
            ERR("is_pow2(0) must be negative (no k gives 2^k == 0)");
        }

        for (uint32_t k = 0; k <= 200; k += 13) {
            mpz_t P;
            mp_int_init_uvalue(&P, 1);
            mp_int_mul_pow2(&P, (mp_small)k, &P);
            if (mp_int_is_pow2(&P) != (mp_result)k) {
                ERR("is_pow2(2^%u) wrong", k);
            }
            mp_int_clear(&P);
        }
        mp_int_clear(&Z);
    }

    TEST("imath: swap across inline/heap storage combinations") {
        /* small = inline single[], big = heap-spilled digits; swap
         * must never cross-link one struct's pointer into the other's
         * inline buffer */
        mpz_t SMALL;
        mpz_t BIG;
        mp_int_init_uvalue(&SMALL, 42);
        mp_int_init_uvalue(&BIG, 7);
        mp_int_mul_pow2(&BIG, 700, &BIG); /* force heap spill */

        mpz_t BIGREF;
        mp_int_init_copy(&BIGREF, &BIG);

        mp_int_swap(&SMALL, &BIG);
        mp_usmall got = 0;
        mp_int_to_uint(&BIG, &got);
        if (got != 42 || mp_int_compare(&SMALL, &BIGREF) != 0) {
            ERR("Swap mixed inline/heap corrupted a value");
        }

        mp_int_swap(&SMALL, &BIG);
        mp_int_to_uint(&SMALL, &got);
        if (got != 42 || mp_int_compare(&BIG, &BIGREF) != 0) {
            ERR("Swap back corrupted a value");
        }

        mp_int_clear(&SMALL);
        mp_int_clear(&BIG);
        mp_int_clear(&BIGREF);
    }

    TEST("Repr prints values of every size in full") {
        const size_t lens[] = {1, 1023, 1024, 1025, 5000};
        for (size_t i = 0; i < countof(lens); i++) {
            const size_t n = lens[i];
            /* digits[0] is reserved for the negative form's '-' */
            cleanerName(zfree, Chars) char *digits = zmalloc(n + 2);
            bigmathTestDigits(digits + 1, n, 0x5EED5EED00000000ULL + n);

            bigmath b;
            bigmathFromDigits(&b, digits + 1);
            if (!bigmathTestReprIs(&b, digits + 1)) {
                ERR("Repr of a %zu-digit value is not exact", n);
            }

            digits[0] = '-';
            bigmathMultiplyValue(&b, -1, &b);
            if (!bigmathTestReprIs(&b, digits)) {
                ERR("Repr of a negative %zu-digit value is not exact", n);
            }
            bigmathReset(&b);
        }

        bigmath zero;
        bigmathInit(&zero);
        if (!bigmathTestReprIs(&zero, "0")) {
            ERR("Repr of zero is not \"0\"");
        }
        bigmathReset(&zero);
    }

    TEST("imath: to_string never writes past limit") {
        char digits[41];
        bigmathTestDigits(digits, 40, 0xC0FFEE00C0FFEE00ULL);

        for (int32_t neg = 0; neg < 2; neg++) {
            mpz_t z;
            mp_int_init(&z);
            for (const char *p = digits; *p; p++) {
                mp_int_mul_value(&z, 10, &z);
                mp_int_add_value(&z, *p - '0', &z);
            }
            if (neg) {
                mp_int_neg(&z, &z);
            }

            char full[42];
            snprintf(full, sizeof(full), "%s%s", neg ? "-" : "", digits);
            const size_t fullLen = strlen(full);
            const size_t signLen = neg ? 1 : 0;

            for (size_t limit = 2; limit <= fullLen + 3; limit++) {
                char buf[64];
                memset(buf, 'X', sizeof(buf));
                const mp_result res = mp_int_to_string(&z, 10, buf, limit);

                for (size_t k = limit; k < sizeof(buf); k++) {
                    if (buf[k] != 'X') {
                        ERR("to_string wrote byte %zu with limit %zu", k,
                            limit);
                        break;
                    }
                }

                if (memchr(buf, '\0', limit) == NULL) {
                    ERR("to_string left no terminator within limit %zu", limit);
                    continue;
                }

                if (limit > fullLen) {
                    if (res != MP_OK || strcmp(buf, full) != 0) {
                        ERR("to_string limit %zu: res %d, got \"%s\"", limit,
                            res, buf);
                    }
                    continue;
                }

                /* Truncated: the sign, then the lowest-order digits that
                 * fit, limit - 1 characters in all. */
                const size_t keep = limit - 1 - signLen;
                const char *tail = digits + (40 - keep);
                if (res != MP_TRUNC || strlen(buf) != limit - 1 ||
                    (neg && buf[0] != '-') ||
                    memcmp(buf + signLen, tail, keep) != 0) {
                    ERR("to_string truncation at limit %zu: res %d, got "
                        "\"%s\"",
                        limit, res, buf);
                }
            }

            mp_int_clear(&z);
        }
    }

    TEST("Repr: zero and a negative value print without faults") {
        bigmath b;
        bigmathInit(&b);
        bigmathRepr(&b);
        bigmathFree(&b);
        bigmathInitSigned(&b, -123456789);
        bigmathRepr(&b);
        bigmathFree(&b);
    }

    TEST_FINAL_RESULT;
}

#undef BM_NEXT

#endif /* DATAKIT_TEST */
