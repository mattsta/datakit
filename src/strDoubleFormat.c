#include "datakit.h"

#include "bigmath.h"
#include "str.h"

#include "endianIsLittle.h"

#include <float.h>
#include <math.h> /* trunc, fpclassify */

#include <stdio.h>

#include "strDoubleFormat.h"

/* This is a C port of an Erlang port of Scheme code from 1996.
 * See mochinum for the origin story and where the original code came from.
 * One big change: Scheme and Erlang have built-in bignums,
 * so they use compiler-provided math directly. Here in C land,
 * we have fixed-width integer types, so, for speed, we have THREE
 * complete implementations:
 *   - system-native 64-bit integers (no prefix)
 *   - compiler-native 128-bit integers (prefix: O for One-28, not vocative)
 *   - software-implemented bignums (prefix: B for Bignum)
 * The bignums allow arbitrary high numbers to be math'd, but
 * the cost is memory allocations and freeing during math operations.
 */

#define I754_FLOAT_BIAS 1022
#define I754_MIN_EXPONENT (-1022 - 52)  /* -1074 */
#define I754_BIG_POWER 4503599627370496 /* 2^52 */

#ifdef DATAKIT_TEST
static uint32_t resolvedMethod = 0;
#endif

/* Regarding the following two structs:
 * Technically the C spec doesn't guarantee bit fields appear
 * in any given order.  Practically, compilers lay out bitfield
 * selectors according to endianness.  This same method is used in
 * compiler-provided headers for deconstructing floats as well,
 * but those pre-provided headers complicate matters more than necessary. */

/* ====================================================================
 * Common helpers (integer ceiling, integer powers)
 * ==================================================================== */
static int64_t int_ceil(double v) {
    double t = trunc(v);
    if (v - t > 0) {
        return t + 1;
    }

    return t;
}

/* Returns 52-bit fractional segment and populates 'exponent' */
static uint64_t fractionAndExponent(const double v, int32_t *exponent) {
    uint64_t fractionAsInteger;
    if (endianIsLittle()) {
        /* Struct-ize the double so we can extract its fields */
        realDoubleLayoutLittle *x = (realDoubleLayoutLittle *)&v;

        fractionAsInteger = x->fraction;
        *exponent = x->exponent;
    } else {
        realDoubleLayoutBig *x = (realDoubleLayoutBig *)&v;

        fractionAsInteger = x->fraction;
        *exponent = x->exponent;
    }

    if (*exponent == 0) {
        *exponent = I754_MIN_EXPONENT;
    } else {
        fractionAsInteger += (1ULL << 52);
        *exponent = *exponent - 53 - I754_FLOAT_BIAS;
    }

    return fractionAsInteger;
}

/* ====================================================================
 * Inner implementation details requiring bignums
 * ==================================================================== */
static size_t Bgenerate(bigmath *r0, bigmath *s, bigmath *mplus,
                        bigmath *mminus, const bool lowOk, const bool highOk,
                        uint8_t generated[64]) {
    bigmath BD;
    bigmath BR;
    bigmath tmpA;
    bigmath RMPLUS;
    bigmathInit(&BD);
    bigmathInit(&BR);
    bigmathInit(&tmpA);
    bigmathInit(&RMPLUS);
    size_t i;
    for (i = 0; i < 64; i++) {
        bigmathDivideRemainder(r0, s, &BD, &BR);
        const uint64_t d = bigmathToNativeUnsigned(&BD);

        assert(d <= 9);

        const int cmpRmin = bigmathCompare(&BR, mminus);

        bigmathAdd(&BR, mplus, &RMPLUS);
        const int cmpRSMplus = bigmathCompare(&RMPLUS, s);

        const bool tc1 = lowOk ? cmpRmin <= 0 : cmpRmin < 0;
        const bool tc2 = highOk ? cmpRSMplus >= 0 : cmpRSMplus > 0;

        if (tc1) {
            if (tc2) {
                bigmathMultiplyValue(&BR, 2, &tmpA);
                if (bigmathCompare(&tmpA, s) < 0) {
                    generated[i] = d;
                } else {
                    generated[i] = d + 1;
                }
            } else {
                generated[i] = d;
            }

            break;
        } else {
            if (tc2) {
                generated[i] = d + 1;
                break;
            } else {
                generated[i] = d;
                bigmathMultiplyValue(&BR, 10, r0);
                bigmathMultiplyValue(mplus, 10, mplus);
                bigmathMultiplyValue(mminus, 10, mminus);
            }
        }
    }

    /* This is the end of the evaluation functions.
     * We need to free all our allocated values now. */
    bigmathReset(r0);
    bigmathReset(s);
    bigmathReset(mplus);
    bigmathReset(mminus);
    bigmathReset(&BD);
    bigmathReset(&BR);
    bigmathReset(&tmpA);
    bigmathReset(&RMPLUS);

    assert(i < 64);

    /* + 1 because we return the *length* of written
     * entries, and that's 1 higher than the 0-based final index. */
    return i + 1;
}

static size_t Bfixup(bigmath *r, bigmath *s, bigmath *mplus, bigmath *mminus,
                     int32_t k, bool lowOk, bool highOk, int32_t *places,
                     uint8_t generated[64]) {
    bigmath tmpA;
    bigmathInit(&tmpA);
    bigmathAdd(r, mplus, &tmpA);
    const int compared = bigmathCompare(&tmpA, s);
    const bool tooLow = highOk ? compared >= 0 : compared > 0;
    bigmathReset(&tmpA);

    if (tooLow) {
        *places = k + 1;
    } else {
        *places = k;
        bigmathMultiplyValue(r, 10, r);
        bigmathMultiplyValue(mplus, 10, mplus);
        bigmathMultiplyValue(mminus, 10, mminus);
    }

    return Bgenerate(r, s, mplus, mminus, lowOk, highOk, generated);
}

static size_t Bscale(bigmath *r, bigmath *s, bigmath *mplus, bigmath *mminus,
                     bool lowOk, bool highOk, double v, int32_t *places,
                     uint8_t generated[64]) {
    /* 'est' is also ~k, the digit offset for the decimal point. */
    const int32_t est = int_ceil(log10(fabs(v)) - 1.0e-10);

    if (est >= 0) {
        bigmath tmpA;
        bigmathInit(&tmpA);
        bigmathExponent(10, est, &tmpA);
        bigmathMultiply(s, &tmpA, s);
        bigmathReset(&tmpA);
    } else {
        bigmath scale;
        bigmathInit(&scale);
        bigmathExponent(10, -est, &scale);
        bigmathMultiply(r, &scale, r);
        bigmathMultiply(mplus, &scale, mplus);
        bigmathMultiply(mminus, &scale, mminus);
        bigmathReset(&scale);
    }

    return Bfixup(r, s, mplus, mminus, est, lowOk, highOk, places, generated);
}

static size_t BniceDoubleHelper(double v, int32_t exp /* 11 bits */,
                                uint64_t frac, /* max 53 bits */
                                int32_t *places, uint8_t generated[64]) {
    const bool round = (frac & 1) == 0;

    /* These four parameters get reset/cleared/free'd in the
     * final function in our call chain here. */
    bigmath BR;
    bigmath BS;
    bigmath BMPLUS;
    bigmath BMMINUS;
    if (exp >= 0) {
        /* This is an unconditional promotion of 'bexp'
         * to a bigmath even though some operations can be
         * satisfied under normal shifting limits.
         * This just saves us from needing to copy/paste the next
         * if/else initializations into native and bignum
         * 'bexp' conditions. */
        bigmath BEXP;
        bigmathInitUnsigned(&BEXP, 1);
        bigmathShiftLeft(&BEXP, exp, &BEXP);
        if (frac != I754_BIG_POWER) {
            /* BR = frac * bexp * 2 */
            bigmathInitUnsigned(&BR, frac * 2);
            bigmathMultiply(&BR, &BEXP, &BR);
            bigmathInitUnsigned(&BS, 2);
            bigmathInitCopy(&BMPLUS, &BEXP);
            bigmathInitCopy(&BMMINUS, &BEXP);
        } else {
            bigmathInitUnsigned(&BR, frac * 4);
            bigmathMultiply(&BR, &BEXP, &BR);
            bigmathInitUnsigned(&BS, 4);
            bigmathInitCopy(&BMPLUS, &BEXP);
            bigmathMultiplyValue(&BMPLUS, 2, &BMPLUS);
            bigmathInitCopy(&BMMINUS, &BEXP);
        }
        bigmathReset(&BEXP);
    } else {
        if (exp == I754_MIN_EXPONENT || frac != I754_BIG_POWER) {
            bigmathInitUnsigned(&BR, frac * 2);
            bigmathInitUnsigned(&BS, 1);
            bigmathShiftLeft(&BS, 1 - exp, &BS);
            bigmathInitUnsigned(&BMPLUS, 1);
            bigmathInitUnsigned(&BMMINUS, 1);
        } else {
            bigmathInitUnsigned(&BR, frac * 4);
            bigmathInitUnsigned(&BS, 1);
            bigmathShiftLeft(&BS, 2 - exp, &BS);
            bigmathInitUnsigned(&BMPLUS, 2);
            bigmathInitUnsigned(&BMMINUS, 1);
        }
    }

    return Bscale(&BR, &BS, &BMPLUS, &BMMINUS, round, round, v, places,
                  generated);
}

/* ====================================================================
 * Non-bignum helper functions (64-bit)
 * ==================================================================== */
static size_t generate(uint64_t r0, uint64_t s, uint64_t mplus, uint64_t mminus,
                       const bool lowOk, const bool highOk,
                       uint8_t generated[64]) {
    size_t i;
    for (i = 0; i < 64; i++) {
        const uint64_t d = r0 / s;
        const uint64_t r = r0 % s;

        assert(d <= 9);

        const bool tc1 = lowOk ? r <= mminus : r < mminus;
        const bool tc2 = highOk ? (r + mplus) >= s : (r + mplus) > s;

        if (tc1) {
            if (tc2) {
                if ((r * 2) < s) {
                    generated[i] = d;
                } else {
                    generated[i] = d + 1;
                }
            } else {
                generated[i] = d;
            }

            break;
        } else {
            if (tc2) {
                generated[i] = d + 1;
                break;
            } else {
                generated[i] = d;
                assert(r * 10 > r);
                assert(mplus * 10 > mplus);
                assert(mminus * 10 > mminus);
                r0 = r * 10;
                mplus = mplus * 10;
                mminus = mminus * 10;
            }
        }
    }

    assert(i < 64);

    /* + 1 because we return the *length* of written
     * entries, and that's 1 higher than the 0-based final index. */
    return i + 1;
}

static size_t fixup(uint64_t r, uint64_t s, uint64_t mplus, uint64_t mminus,
                    int32_t k, bool lowOk, bool highOk, int32_t *places,
                    uint8_t generated[64]) {
    const bool tooLow = highOk ? (r + mplus) >= s : (r + mplus) > s;

    if (tooLow) {
        *places = k + 1;
        return generate(r, s, mplus, mminus, lowOk, highOk, generated);
    }

    *places = k;
    return generate(r * 10, s, mplus * 10, mminus * 10, lowOk, highOk,
                    generated);
}

static size_t scale(uint64_t r, uint64_t s, uint64_t mplus, uint64_t mminus,
                    bool lowOk, bool highOk, double v, int32_t *places,
                    uint8_t generated[64]) {
    const int32_t est = int_ceil(log10(fabs(v)) - 1.0e-10);

    if (est >= 0) {
        return fixup(r, s * StrTenPow(est), mplus, mminus, est, lowOk, highOk,
                     places, generated);
    } else {
        uint64_t scale = StrTenPow(-est);

        /* If we hit big or tiny floats, we can't cope here because,
         * for example 10^323 is larger than 2^64 and we can't
         * calculate things that big natively. */
        assert(scale);

        return fixup(r * scale, s, mplus * scale, mminus * scale, est, lowOk,
                     highOk, places, generated);
    }
}

static size_t niceDoubleHelper(double v, int32_t exp /* 11 bits */,
                               uint64_t frac, /* max 53 bits */
                               int32_t *places, uint8_t generated[64]) {
    const bool round = (frac & 1) == 0;
    if (exp >= 0) {
        uint64_t bexp = 1ULL << exp;
        if (frac != I754_BIG_POWER) {
            return scale((frac * bexp * 2), 2, bexp, bexp, round, round, v,
                         places, generated);
        }

        return scale((frac * bexp * 4), 4, bexp * 2, bexp, round, round, v,
                     places, generated);
    }

    /* else, exp < 0 for these statements */
    if (exp == I754_MIN_EXPONENT || frac != I754_BIG_POWER) {
        assert(exp >= -62);
        return scale((frac * 2), 1ULL << (1 - exp), 1, 1, round, round, v,
                     places, generated);
    }

    assert(exp >= -61);
    return scale((frac * 4), 1ULL << (2 - exp), 2, 1, round, round, v, places,
                 generated);
}

/* ====================================================================
 * Non-bignum helper functions (128-bit)
 * ==================================================================== */
static size_t Ogenerate(__uint128_t r0, __uint128_t s, __uint128_t mplus,
                        __uint128_t mminus, const bool lowOk, const bool highOk,
                        uint8_t generated[64]) {
    size_t i;
    for (i = 0; i < 64; i++) {
        const __uint128_t d = r0 / s;
        const __uint128_t r = r0 % s;

        assert(d <= 9);

        const bool tc1 = lowOk ? r <= mminus : r < mminus;
        const bool tc2 = highOk ? (r + mplus) >= s : (r + mplus) > s;

        if (tc1) {
            if (tc2) {
                if ((r * 2) < s) {
                    generated[i] = d;
                } else {
                    generated[i] = d + 1;
                }
            } else {
                generated[i] = d;
            }

            break;
        } else {
            if (tc2) {
                generated[i] = d + 1;
                break;
            } else {
                generated[i] = d;
                assert(r * 10 > r);
                assert(mplus * 10 > mplus);
                assert(mminus * 10 > mminus);
                r0 = r * 10;
                mplus = mplus * 10;
                mminus = mminus * 10;
            }
        }
    }

    assert(i < 64);

    /* + 1 because we return the *length* of written
     * entries, and that's 1 higher than the 0-based final index. */
    return i + 1;
}

static size_t Ofixup(__uint128_t r, __uint128_t s, __uint128_t mplus,
                     __uint128_t mminus, int32_t k, bool lowOk, bool highOk,
                     int32_t *places, uint8_t generated[64]) {
    const bool tooLow = highOk ? (r + mplus) >= s : (r + mplus) > s;

    if (tooLow) {
        *places = k + 1;
        return Ogenerate(r, s, mplus, mminus, lowOk, highOk, generated);
    }

    *places = k;
    return Ogenerate(r * 10, s, mplus * 10, mminus * 10, lowOk, highOk,
                     generated);
}

static size_t Oscale(__uint128_t r, __uint128_t s, __uint128_t mplus,
                     __uint128_t mminus, bool lowOk, bool highOk, double v,
                     int32_t *places, uint8_t generated[64]) {
    const int32_t est = int_ceil(log10(fabs(v)) - 1.0e-10);

    if (est >= 0) {
        return Ofixup(r, s * StrTenPowBig(est), mplus, mminus, est, lowOk,
                      highOk, places, generated);
    } else {
        __uint128_t scale = StrTenPowBig(-est);

        /* If we hit big or tiny floats, we can't cope here because,
         * for example 10^323 is larger than 2^64 and we can't
         * calculate things that big natively. */
        assert(scale);

        return Ofixup(r * scale, s, mplus * scale, mminus * scale, est, lowOk,
                      highOk, places, generated);
    }
}

static size_t OniceDoubleHelper(double v, int32_t exp /* 11 bits */,
                                uint64_t frac, /* max 53 bits */
                                int32_t *places, uint8_t generated[64]) {
    const bool round = (frac & 1) == 0;
    if (exp >= 0) {
        __uint128_t bexp = (__uint128_t)1 << exp;
        if (frac != I754_BIG_POWER) {
            return Oscale((frac * bexp * 2), 2, bexp, bexp, round, round, v,
                          places, generated);
        }

        return Oscale((frac * bexp * 4), 4, bexp * 2, bexp, round, round, v,
                      places, generated);
    }

    /* else, exp < 0 for these statements */
    if (exp == I754_MIN_EXPONENT || frac != I754_BIG_POWER) {
        assert(exp >= -(128 - 2));
        return Oscale((frac * 2), (__uint128_t)1 << (1 - exp), 1, 1, round,
                      round, v, places, generated);
    }

    assert(exp >= -(128 - 3));
    return Oscale((frac * 4), (__uint128_t)1 << (2 - exp), 2, 1, round, round,
                  v, places, generated);
}

/* ====================================================================
 * Common launch point
 * ==================================================================== */
static size_t niceDoubleDispatch(const double v, int32_t exp /* 11 bits */,
                                 uint64_t frac, /* max 53 bits */
                                 int32_t *places, uint8_t generated[64]) {
    bool useNative = false;
    bool useBigNative = false;

/* 64-bit safe exponent: (64 - 6) = 58
 * 128-bit safe exponent: (128 - 6) = 122 */
#define SAFE_EXPONENT(width) ((width)-6)

/* Safe exponent restricts 'exp' to interval based on integer bit width */
#define SAFE_I754_EXPONENT(width)                                              \
    ((exp >= -SAFE_EXPONENT(width)) && (exp <= SAFE_EXPONENT(width)))

/* Safe value restricts 'v' to interval based on highest sustained 10^x */
#define SAFE_VALUE(bounds) ((v >= -(bounds)) && (v <= (bounds)))

    /* Limit the exponent bounds so we don't overflow in native operations */
    if (SAFE_I754_EXPONENT(64)) {
        /* Bounds here are limited by the exponentiation size
         * multiplier of our fixed-width native data type.
         * e.g. uint64_t can only hold 10^19 max, but we need
         * a lower limit because we *multiply* by 10^x.
         * A safe limit has been tested to be [-1e17, 1e17] which
         * uses 57 bits for 10^17 and the remaining bits provide
         * padding for multiplication growth. */
        if (SAFE_VALUE(1e17)) {
            useNative = true;
        } else {
            /* If too big for regular native here, we know we'll fit
             * in the big native because of the limited I754 exponent. */
            useBigNative = true;
        }
    } else if (SAFE_I754_EXPONENT(128)) {
        /* Same as comment in 64-bit section above, except here we can limit
         * a higher value of 10^36 (120 bits) because of the 128-bit type while
         * still leaving room for multiplication growth.
         * The safe limit has been tested to be [-1e36, 1e36], but if exception
         * edge cases are found reduce further. */
        if (SAFE_VALUE(1e36)) {
            useBigNative = true;
        }
    }

#ifdef DATAKIT_TEST
    resolvedMethod = useNative ? 1 : useBigNative ? 2 : 3;
#endif

    /* If 'v' fits inside the native limits, use faster native processing.
     * Otherwise, fall back to malloc-heavy bignum processing. */
    return useNative      ? niceDoubleHelper(v, exp, frac, places, generated)
           : useBigNative ? OniceDoubleHelper(v, exp, frac, places, generated)
                          : BniceDoubleHelper(v, exp, frac, places, generated);
}

/* ====================================================================
 * Public Interface
 * ==================================================================== */
__attribute__((no_sanitize("unsigned-integer-overflow"))) size_t
StrDoubleFormatToBufNice(void *buf_, const size_t len, const double v) {
    uint8_t *const buf = buf_;

    /* We do weak bounds checks, so verify any space is big enough
     * for us to walk through without immediate checks. */
    if (len < 23) {
        /* Buffer isn't big enough to hold all possible doubles-as-strings,
         * so we can't write anything. */
        return 0;
    }

    /* First, check edge cases and non-value conditions */
    switch (fpclassify(v)) {
    case FP_INFINITE:
        if (v > 0) {
            memcpy(buf, "inf", 3);
            return 3;
        }

        memcpy(buf, "-inf", 4);
        return 4;
    case FP_NAN:
        memcpy(buf, "nan", 3);
        return 3;
    case FP_ZERO:
        memcpy(buf, "0.0", 3);
        return 3;
    default:
        /* FP_NORMAL or FP_SUBNORMAL, we can generate values directly. */
        break;
    }

    int32_t exponent;
    int32_t places;
    uint8_t generated[64] = {0};

    const uint64_t fractionAsInteger = fractionAndExponent(v, &exponent);
    ssize_t generatedLen =
        niceDoubleDispatch(v, exponent, fractionAsInteger, &places, generated);

    /* The longest output is 18 digits */
    assert(generatedLen <= 18);

    /* Now we have to place the decimal point at position
     * 'places' and there's about 22 different ways we need
     * to handle (before, after, zero padding, split in middle, ... */

    /* 'bi' is index of current 'buf' offset */
    size_t bi = 0;

    /* If value is negative, prepend a minus sign */
    if (v < 0) {
        buf[bi++] = '-';
    }

    /* If generated value starts with zero, skip the leading zero */
    ssize_t gi = 0; /* index into 'generated' */
    if (generated[0] == 0) {
        gi++;
    }

    /* We need to track any characters we place for formatting *after*
     * the generated digits so we can return the proper fully-written
     * length. 'addBonus' tracks things like added decimals and padded
     * zeros we don't traverse in the write generated numbers loop. */
    uint32_t addBonus = 0;

    /* Now place decimal in proper spot depending on
     * location returned by the formatting process. */
    if (places == 0) {
        buf[bi++] = '0';
        buf[bi++] = '.';
    } else if (places > 0) {
        const int32_t placeOffset = places - generatedLen;
        if (placeOffset == 0) {
            /* APPEND */
            /* Ends in .0 */
            buf[bi + generatedLen] = '.';
            buf[bi + generatedLen + 1] = '0';
            addBonus = 2;
        } else if (placeOffset < 0) {
            /* Split in the middle somewhere */
            for (ssize_t i = 0; gi < generatedLen && bi < len; i++) {
                if (i == generatedLen + placeOffset) {
                    /* Place decimal at correct offset */
                    buf[bi++] = '.';
                }

                buf[bi++] = '0' + generated[gi++];
            }

            /* return early here because we overrode the default formatting
             * loop ad just walked/wrote all the digits above to completion. */
            return bi;
        } else if (placeOffset < 6) {
            /* APPEND */
            /* Append zeros at proper indices (note: is AFTER
             * the digits we will write later from 'generated' */
            int_fast32_t i;
            for (i = 0; i < placeOffset; i++) {
                buf[bi + generatedLen + i] = '0';
            }

            buf[bi + generatedLen + i++] = '.';
            buf[bi + generatedLen + i++] = '0';

            addBonus = i;
        } else {
            /* too lazy to make the exponential formatter
             * a real function we can call from two places
             * (and we've already got all the state we need right here),
             * so just jump to where we do exponential formatting
             * later in this if/else chain... */
            goto exponentialOffset;
        }
    } else if (places > -6) {
        /* 0. PLACE ZEROES value */
        buf[bi++] = '0';
        buf[bi++] = '.';
        int_fast32_t i;
        for (i = 0; i < -places; i++) {
            buf[bi + i] = '0';
        }

        bi += i;

        /* No 'addBonus' here because these are PREPENDED
         * before the 'generated' output, so 'generated' will
         * end processing at the actual final 'bi' offset. */
    } else {
        /* Format large exponents as: A.VALe±exponent */
        /* clang-format off */
exponentialOffset: {
        /* clang-format on */
        const uint8_t firstDigit = generated[0];

        const char *exp = "e+";
        if (places < 0) {
            exp = "e-";
        }

        /* FORMAT:
         *   [digit].[rest of digits]e±[exponent] */
        buf[bi++] = '0' + firstDigit;
        buf[bi++] = '.';

        /* Advance 'generated index' by one because we just
         * consumed 'firstDigit' */
        gi++;

        /* Now write value digits we accumulated, even
         * though we already stole the first digit above. */
        if (generatedLen > 1) {
            while (gi < generatedLen && bi < len) {
                buf[bi++] = '0' + generated[gi++];
            }
        } else {
            /* If no more value digits, the output will be
             * 'firstDigit'.0e±exp */
            buf[bi++] = '0';
        }

        buf[bi++] = exp[0]; /* e */
        buf[bi++] = exp[1]; /* ± */

        /* Convert exponent into string and write into 'buf'
         * at proper offset. */
        const uint_fast32_t digitsLen =
            StrUInt64ToBuf(buf + bi, len - bi, abs(places - 1));

        addBonus = digitsLen;

        /* Early return because we just consumed all the values */
        return bi + addBonus;
    }
    }

    /* If we got this far, we still need to write the generated
     * digits into the output buffer. */
    for (; gi < generatedLen && bi < len; bi++, gi++) {
        buf[bi] = '0' + generated[gi];
    }

    /* Return total length of formatted string to user. */
    return bi + addBonus;
}

/* ====================================================================
 * Tests
 * ==================================================================== */
#ifdef DATAKIT_TEST
#include "ctest.h"
#include "float16.h"
#include <locale.h>

int strDoubleFormatTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;

/* 'assertEqual' tests are mostly copied from mochinum */
#define assertEqual(a, b) (assert(a == b))
    TEST("int ceiling") {
        assertEqual(1, int_ceil(0.0001));
        assertEqual(0, int_ceil(0.0));
        assertEqual(1, int_ceil(0.99));
        assertEqual(1, int_ceil(1.0));
        assertEqual(-1, int_ceil(-1.5));
        assertEqual(-2, int_ceil(-2.0));
    }

    TEST("int power") {
        assertEqual(10, StrTenPow(1));
        assertEqual(100, StrTenPow(2));
        assertEqual(1000, StrTenPow(3));
    }

    TEST("nice double printing") {
        const uint64_t one = 1;
        double smallFloat;
        memcpy(&smallFloat, &one, sizeof(double));

        /* this is
         * 2.22507385850720088902e-308
         * and also
         * <<0,15,255,255,255,255,255,255>> */
        const uint64_t ldn = 4503599627370495;
        double largeFloat;
        memcpy(&largeFloat, &ldn, sizeof(double));

        /* this is
         * 2.22507385850720138309e-308
         * and also
         * <<0,16,0,0,0,0,0,0>> */
        const uint64_t sn = 4503599627370496;
        double smallN;
        memcpy(&smallN, &sn, sizeof(double));

        /* this is
         * 1.7976931348623157e+308
         * and also
         * <<127,239,255,255,255,255,255,255>> */
        const uint64_t ln = 9218868437227405311;
        double largeN;
        memcpy(&largeN, &ln, sizeof(double));

        double digits[] = {/*0,*/ 0.0,
                           1.0,
                           -1.0,
                           0.1,
                           0.01,
                           0.001,
                           1000000.0,
                           0.5,
                           4503599627370496.0,
                           smallFloat,
                           largeFloat,
                           smallN,
                           largeN,
                           22.222,
                           299.2999,
                           7074451188.598104,
                           7074451188.5981045,
                           0.0078125};
        char *results[] = {/*"0",*/ "0.0",
                           "1.0",
                           "-1.0",
                           "0.1",
                           "0.01",
                           "0.001",
                           "1.0e+6",
                           "0.5",
                           "4503599627370496.0",
                           "5.0e-324",
                           "2.225073858507201e-308",
                           "2.2250738585072014e-308",
                           "1.7976931348623157e+308",
                           "22.222",
                           "299.2999",
                           "7074451188.598104",
                           "7074451188.5981045",
                           "0.0078125"};
        assert(COUNT_ARRAY(digits) == COUNT_ARRAY(results));
        for (size_t i = 0; i < COUNT_ARRAY(digits); i++) {
            uint8_t buf[64] = {0};
            uint32_t resultLen =
                StrDoubleFormatToBufNice(buf, sizeof(buf), digits[i]);
            printf("Got: %s (len %d) (expecting: %s (len %zu))\n", buf,
                   resultLen, results[i], strlen(results[i]));
            assert(resultLen == strlen(results[i]));
            assert(!memcmp(buf, results[i], strlen(results[i])));
        }
    }

    TEST("nice float16 printing") {
        for (size_t i = 0; i <= UINT16_MAX; i++) {
            uint8_t buf[64] = {0};
            double tryThis = float16Decode(i);
            uint32_t resultLen =
                StrDoubleFormatToBufNice(buf, sizeof(buf), tryThis);
            /* We don't compare results here; this is just to stress
             * the formatter with a 64k range of known values */
            assert(resultLen);
        }
    }

    int rs = time(NULL);
    printf("random seed: %d\n", rs);
    srand(rs);

    setlocale(LC_ALL, "");

    const size_t testMax = 1e7;
    TEST_DESC("double formatting from random bit construction (100 dots; "
              "%'zu total tests)",
              testMax) {
        uint32_t methods[4] = {0};
        /* Test random floats */
        for (size_t i = 0; i < testMax; i++) {
            const uint32_t randA = rand();
            const uint32_t randB = rand();
            const uint64_t randBig = (uint64_t)randA << 32 | randB;
            double thisRandAttempt;
            memcpy(&thisRandAttempt, &randBig, sizeof(double));
            uint8_t buf[64] = {0};
            //            printf("running against: %g\n", thisRandAttempt);
            size_t len =
                StrDoubleFormatToBufNice(buf, sizeof(buf), thisRandAttempt);

            methods[resolvedMethod]++;

            assert(buf[0]);
            assert(len || isinf(thisRandAttempt));
            //            printf("printed: %.*s\n", len, buf);
            if (i % (testMax / 100) == 0) {
                printf(".");
                fflush(stdout);
            }
        }
        printf("\n");
        printf("{64-bit: %'d} {128-bit: %'d} {bignums: %'d}\n", methods[1],
               methods[2], methods[3]);
    }

    TEST_DESC(
        "double formatting from random doubles (100 dots; %'zu total tests)",
        testMax) {
        uint32_t methods[4] = {0};
        /* Test random floats */
        for (size_t i = 0; i < testMax; i++) {
            const bool doNegative = rand() % 2 == 0;
            const bool doSmall = rand() % 2 == 0;
            const double thisRandAttempt =
                (doNegative ? -1 : 1) * ((double)rand() / (double)RAND_MAX) *
                (doSmall ? ((double)1 / rand()) * (rand() % 2 == 0 ? 10e100 : 1)
                         : rand());
            uint8_t buf[64] = {0};
            //            printf("running against: %g\n", thisRandAttempt);
            size_t len =
                StrDoubleFormatToBufNice(buf, sizeof(buf), thisRandAttempt);

            methods[resolvedMethod]++;

            assert(buf[0]);
            assert(len || isinf(thisRandAttempt));
            //            printf("printed: %.*s\n", len, buf);
            if (i % (testMax / 100) == 0) {
                printf(".");
                fflush(stdout);
            }
        }
        printf("\n");
        printf("{64-bit: %'d} {128-bit: %'d} {bignums: %'d}\n", methods[1],
               methods[2], methods[3]);
    }

    return err;
}

#endif
