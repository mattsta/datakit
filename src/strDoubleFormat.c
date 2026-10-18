#include "datakit.h"

#include "bigmath.h"
#include "str.h"

#include "endianIsLittle.h"

#include <float.h>
#include <math.h> /* trunc, fpclassify */

#include <inttypes.h>
#include <stdio.h>

#include "strDoubleFormat.h"

/* Disable unsigned overflow sanitizer for intentional wraparound in digit math.
 * The overflow checks in the digit generation loops are part of the algorithm.
 */
DK_FN_NO_SANITIZE_UNSIGNED_WRAP size_t
StrDoubleFormatToBufNice(void *buf_, const size_t len, const double v);

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
static int32_t resolvedPlaces = 0;
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
/* Always inline hot path helper functions for performance */
DK_INLINE_ALWAYS int64_t int_ceil(double v) {
    double t = trunc(v);
    if (v - t > 0) {
        return t + 1;
    }

    return t;
}

/* Decimal-scale estimate for v = frac * 2^exp (frac >= 1), replacing
 * ceil(log10(v)): returns floor(x) + 1 where x underestimates
 * log10(v) by less than 0.027, built from the binary magnitude l2
 * plus the mantissa chord (m - 1) <= log2(m). Because x < log10(v)
 * strictly (the 1e-9 fudge covers exact powers of two, where the
 * chord is tight), the result NEVER exceeds ceil(log10(v)) — the
 * side the scale() bump loops cannot repair — and errs low by at
 * most one, and only for the ~3% of values whose log10 fraction is
 * below the chord error. */
DK_INLINE_ALWAYS int32_t estimateDecimalScale(const int32_t exp,
                                              const uint64_t frac) {
    const int32_t b = 63 - (int32_t)__builtin_clzll(frac);
    const int32_t l2 = exp + b;
    const double m = (double)frac / (double)(1ULL << b);
    const double x = ((double)l2 + (m - 1.0)) * 0.30102999566398119521 - 1.0e-9;
    return (int32_t)floor(x) + 1;
}

/* Returns 52-bit fractional segment and populates 'exponent' */
/* Always inline - called on every conversion */
DK_INLINE_ALWAYS uint64_t fractionAndExponent(const double v,
                                              int32_t *exponent) {
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
    bigmath tmpA;
    bigmath RMPLUS;
    bigmathInit(&tmpA);
    bigmathInit(&RMPLUS);
    size_t i;
    for (i = 0; i < 64; i++) {
        /* r0 becomes the remainder in place; the quotient is a single
         * digit by construction, so the allocation-free digit divide
         * replaces a full long division per generated digit */
        const uint32_t d = bigmathDivideDigitInPlace(r0, s);

        assert(d <= 9);

        const int cmpRmin = bigmathCompare(r0, mminus);

        bigmathAdd(r0, mplus, &RMPLUS);
        const int cmpRSMplus = bigmathCompare(&RMPLUS, s);

        const bool tc1 = lowOk ? cmpRmin <= 0 : cmpRmin < 0;
        const bool tc2 = highOk ? cmpRSMplus >= 0 : cmpRSMplus > 0;

        if (tc1) {
            if (tc2) {
                bigmathMultiplyValue(r0, 2, &tmpA);
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
                bigmathMultiplyValue(r0, 10, r0);
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
    /* Same bump-loop contract as scale(): the estimate never exceeds
     * the true decimal scale and errs low by at most one. Unlike the
     * fixed-width arms (which shrink the r-side for negative scales to
     * dodge overflow), bignum can always grow s - the sign-dependent
     * pre-scaling already happened in Bscale. tmpA = r + mplus is
     * loop-invariant; only s grows per bump. */
    bigmath tmpA;
    bigmathInit(&tmpA);
    bigmathAdd(r, mplus, &tmpA);
    int compared = bigmathCompare(&tmpA, s);
    while (highOk ? compared >= 0 : compared > 0) {
        bigmathMultiplyValue(s, 10, s);
        k++;
        compared = bigmathCompare(&tmpA, s);
    }
    bigmathReset(&tmpA);

    *places = k;
    bigmathMultiplyValue(r, 10, r);
    bigmathMultiplyValue(mplus, 10, mplus);
    bigmathMultiplyValue(mminus, 10, mminus);

    return Bgenerate(r, s, mplus, mminus, lowOk, highOk, generated);
}

static size_t Bscale(bigmath *r, bigmath *s, bigmath *mplus, bigmath *mminus,
                     bool lowOk, bool highOk, const int32_t est,
                     int32_t *places, uint8_t generated[64]) {
    /* 'est' is also ~k, the digit offset for the decimal point. */
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

static size_t BniceDoubleHelper(int32_t exp /* 11 bits */,
                                uint64_t frac, /* max 53 bits */
                                int32_t *places, uint8_t generated[64]) {
    const bool round = (frac & 1) == 0;
    const int32_t est = estimateDecimalScale(exp, frac);

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

    return Bscale(&BR, &BS, &BMPLUS, &BMMINUS, round, round, est, places,
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

/* Inline hot path scale helper. The integer estimate `est` never
 * exceeds the true decimal scale but can err low by at most one; each
 * bump shifts the decimal point one place. Bump style differs by
 * sign: for est >= 0 growing s stays within s_base * 10^k, the same
 * bound an exact estimate produces; for est < 0 the r-side carries
 * 10^-est factors, so removing one per bump divides exactly and only
 * shrinks magnitudes (est < 0 implies the true scale is <= 0, so
 * bumps never outrun the available factors). */
static inline size_t scale(uint64_t r, uint64_t s, uint64_t mplus,
                           uint64_t mminus, bool lowOk, bool highOk,
                           int32_t est, int32_t *places,
                           uint8_t generated[64]) {
    if (est >= 0) {
        s *= StrTenPow(est);
        while (highOk ? (r + mplus) >= s : (r + mplus) > s) {
            s *= 10;
            est++;
        }
    } else {
        const uint64_t power = StrTenPow(-est);

        /* If we hit big or tiny floats, we can't cope here because,
         * for example 10^323 is larger than 2^64 and we can't
         * calculate things that big natively. */
        assert(power);

        r *= power;
        mplus *= power;
        mminus *= power;
        while (highOk ? (r + mplus) >= s : (r + mplus) > s) {
            r /= 10;
            mplus /= 10;
            mminus /= 10;
            est++;
        }
    }

    *places = est;
    return generate(r * 10, s, mplus * 10, mminus * 10, lowOk, highOk,
                    generated);
}

static size_t niceDoubleHelper(int32_t exp /* 11 bits */,
                               uint64_t frac, /* max 53 bits */
                               int32_t *places, uint8_t generated[64]) {
    const bool round = (frac & 1) == 0;
    const int32_t est = estimateDecimalScale(exp, frac);
    if (exp >= 0) {
        uint64_t bexp = 1ULL << exp;
        if (frac != I754_BIG_POWER) {
            return scale((frac * bexp * 2), 2, bexp, bexp, round, round, est,
                         places, generated);
        }

        return scale((frac * bexp * 4), 4, bexp * 2, bexp, round, round, est,
                     places, generated);
    }

    /* else, exp < 0 for these statements */
    if (exp == I754_MIN_EXPONENT || frac != I754_BIG_POWER) {
        assert(exp >= -62);
        return scale((frac * 2), 1ULL << (1 - exp), 1, 1, round, round, est,
                     places, generated);
    }

    assert(exp >= -61);
    return scale((frac * 4), 1ULL << (2 - exp), 2, 1, round, round, est, places,
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

/* Inline 128-bit path scale helper */
/* Inline 128-bit path scale helper; same bump contract as scale() */
static inline size_t Oscale(__uint128_t r, __uint128_t s, __uint128_t mplus,
                            __uint128_t mminus, bool lowOk, bool highOk,
                            int32_t est, int32_t *places,
                            uint8_t generated[64]) {
    if (est >= 0) {
        s *= StrTenPowBig(est);
        while (highOk ? (r + mplus) >= s : (r + mplus) > s) {
            s *= 10;
            est++;
        }
    } else {
        const __uint128_t power = StrTenPowBig(-est);

        /* If we hit big or tiny floats, we can't cope here because,
         * for example 10^323 is larger than 2^64 and we can't
         * calculate things that big natively. */
        assert(power);

        r *= power;
        mplus *= power;
        mminus *= power;
        while (highOk ? (r + mplus) >= s : (r + mplus) > s) {
            r /= 10;
            mplus /= 10;
            mminus /= 10;
            est++;
        }
    }

    *places = est;
    return Ogenerate(r * 10, s, mplus * 10, mminus * 10, lowOk, highOk,
                     generated);
}

static size_t OniceDoubleHelper(int32_t exp /* 11 bits */,
                                uint64_t frac, /* max 53 bits */
                                int32_t *places, uint8_t generated[64]) {
    const bool round = (frac & 1) == 0;
    const int32_t est = estimateDecimalScale(exp, frac);
    if (exp >= 0) {
        __uint128_t bexp = (__uint128_t)1 << exp;
        if (frac != I754_BIG_POWER) {
            return Oscale((frac * bexp * 2), 2, bexp, bexp, round, round, est,
                          places, generated);
        }

        return Oscale((frac * bexp * 4), 4, bexp * 2, bexp, round, round, est,
                      places, generated);
    }

    /* else, exp < 0 for these statements */
    if (exp == I754_MIN_EXPONENT || frac != I754_BIG_POWER) {
        assert(exp >= -(128 - 2));
        return Oscale((frac * 2), (__uint128_t)1 << (1 - exp), 1, 1, round,
                      round, est, places, generated);
    }

    assert(exp >= -(128 - 3));
    return Oscale((frac * 4), (__uint128_t)1 << (2 - exp), 2, 1, round, round,
                  est, places, generated);
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
#define SAFE_EXPONENT(width) ((width) - 6)

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
    return useNative      ? niceDoubleHelper(exp, frac, places, generated)
           : useBigNative ? OniceDoubleHelper(exp, frac, places, generated)
                          : BniceDoubleHelper(exp, frac, places, generated);
}

/* ====================================================================
 * Public Interface
 * ==================================================================== */
DK_FN_NO_SANITIZE_UNSIGNED_WRAP size_t
StrDoubleFormatToBufNice(void *buf_, const size_t len, const double v) {
    uint8_t *const buf = buf_;

    /* We do weak bounds checks, so verify up front that the buffer
     * holds the true worst case across every placement branch:
     *   fixed-point append:  Sign(1) + Digits(18) + PadZeros(5) + ".0"(2) = 26
     *   fixed-point prepend: Sign(1) + "0."(2) + LeadZeros(5) + Digits(18) = 26
     *   exponential:         Sign(1) + Digit(1) + "."(1) + Digits(17)
     *                        + "e±"(2) + Exp(3, up to 324) = 25
     * so 26 bytes is the exact bound the unchecked writes rely on. */
    if (len < 26) {
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

#ifdef DATAKIT_TEST
    resolvedPlaces = places;
#endif

    /* The longest output is 18 digits
     * (DBL_DIG = 15 decimal digits precision, plus margin for rounding) */
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

    /* A converged scale always yields a nonzero leading digit
     * (Steele-White invariant; every placement branch depends on it
     * for its decimal/exponent math) */
    assert(generated[0] != 0);
    ssize_t gi = 0; /* index into 'generated' */

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

#define DOUBLE_NEWLINE 0
#include "perf.h"

int strDoubleFormatTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;

/* 'assertEqual' tests are mostly copied from mochinum */
#define assertEqual(a, b) (assert(a == b))

    /* NEW TESTS - Performance optimization verification */
    TEST("decimal-scale estimate contract across all exponents") {
        /* estimateDecimalScale must never exceed ceil(log10(v)) (the
         * side the scale() bump loops cannot repair) and may err low
         * by at most one. Sweep every I754 exponent with boundary and
         * mixed mantissas, plus every exact decimal power. */
        const uint64_t fracs[] = {1,
                                  3,
                                  (1ULL << 52),
                                  (1ULL << 52) + 1,
                                  (1ULL << 53) - 1,
                                  0x1999999999999ULL,
                                  0x1000000000001ULL};
        for (int32_t e = -1074; e <= 970; e++) {
            for (size_t fi = 0; fi < COUNT_ARRAY(fracs); fi++) {
                const double v = ldexp((double)fracs[fi], e);
                if (!isfinite(v) || v == 0.0) {
                    continue;
                }

                const double lg = log10(v);
                const double estF = (double)estimateDecimalScale(e, fracs[fi]);
                if (estF > lg + 1.0 + 1e-9 || estF < lg - 0.03) {
                    ERR("estimate %d out of contract for exp %d frac %" PRIu64
                        " (log10 %f)",
                        estimateDecimalScale(e, fracs[fi]), e, fracs[fi], lg);
                }
            }
        }

        /* Exact decimal powers pin the never-too-high side hard: the
         * estimate must be <= p for v = 10^p */
        for (int32_t p = -300; p <= 300; p++) {
            const double v = pow(10.0, p);
            int32_t vexp;
            const uint64_t vfrac = fractionAndExponent(v, &vexp);
            const int32_t estF = estimateDecimalScale(vexp, vfrac);
            if (estF > p) {
                ERR("estimate %d exceeds exact power %d", estF, p);
            }
        }
    }

    TEST("UNIT: fractionAndExponent reconstructs every decode exactly") {
        /* frac * 2^exp must equal |v| bit-for-bit: frac <= 2^53 is
         * exactly representable, so ldexp is a lossless oracle */
        uint64_t rng = 0xA5A5A5A55A5A5A5AULL;
#define FE_NEXT() (rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17, rng)
        const double pinned[] = {1.0,  2.0,    0.5, DBL_MIN,  DBL_MAX, 5e-324,
                                 1e17, 1e-300, 0.1, 6.25e-310 /* denormal */};
        for (size_t t = 0; t < 20000 + COUNT_ARRAY(pinned); t++) {
            double v;
            if (t < COUNT_ARRAY(pinned)) {
                v = pinned[t];
            } else {
                const uint64_t bits = FE_NEXT();
                memcpy(&v, &bits, sizeof(v));
                if (!isfinite(v) || v == 0.0) {
                    continue;
                }
            }

            int32_t dexp;
            const uint64_t dfrac = fractionAndExponent(v, &dexp);
            const double rebuilt = ldexp((double)dfrac, dexp);
            const double mag = fabs(v);
            if (memcmp(&rebuilt, &mag, sizeof(mag)) != 0) {
                ERR("decode broken for %.17g (frac %" PRIu64 " exp %d)", v,
                    dfrac, dexp);
            }
        }
#undef FE_NEXT
    }

    TEST("UNIT DIFFERENTIAL: 64/128/bignum arms agree digit-for-digit") {
        /* Each arm implements the same algorithm over different integer
         * widths; any divergence in digits or decimal placement is a
         * bug in exactly one arm. This is the small-scope test that
         * catches a wrong power table or bump-loop overflow instantly,
         * without waiting for a roundtrip sweep to trip over it. */
        uint64_t rng = 0xDEADBEEFCAFEF00DULL;
#define ARM_NEXT() (rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17, rng)
        int32_t armChecked64 = 0;
        int32_t armChecked128 = 0;
        for (size_t t = 0; t < 20000; t++) {
            double v;
            if (t & 1) {
                /* Raw bit patterns (mostly extreme magnitudes) */
                const uint64_t bits = ARM_NEXT();
                memcpy(&v, &bits, sizeof(v));
            } else {
                /* Moderate magnitudes so the 64-bit arm participates:
                 * mantissa in [1,2) times 2^[-56, 56) */
                const uint64_t mbits = (0x3FFULL << 52) | (ARM_NEXT() >> 12);
                memcpy(&v, &mbits, sizeof(v));
                v = ldexp(v, (int32_t)(ARM_NEXT() % 113) - 56);
            }

            if (!isfinite(v) || v == 0.0) {
                continue;
            }

            int32_t dexp;
            const uint64_t dfrac = fractionAndExponent(v, &dexp);

            const bool safe64 =
                dexp >= -58 && dexp <= 58 && v >= -1e17 && v <= 1e17;
            const bool safe128 =
                dexp >= -122 && dexp <= 122 && v >= -1e36 && v <= 1e36;

            int32_t placesB;
            uint8_t genB[64] = {0};
            const size_t lenB = BniceDoubleHelper(dexp, dfrac, &placesB, genB);

            if (safe64) {
                int32_t places64;
                uint8_t gen64[64] = {0};
                const size_t len64 =
                    niceDoubleHelper(dexp, dfrac, &places64, gen64);
                if (len64 != lenB || places64 != placesB ||
                    memcmp(gen64, genB, len64) != 0) {
                    ERR("64-bit arm disagrees with bignum for %.17g "
                        "(len %zu/%zu places %d/%d)",
                        v, len64, lenB, places64, placesB);
                }
                armChecked64++;
            }

            if (safe128) {
                int32_t places128;
                uint8_t gen128[64] = {0};
                const size_t len128 =
                    OniceDoubleHelper(dexp, dfrac, &places128, gen128);
                if (len128 != lenB || places128 != placesB ||
                    memcmp(gen128, genB, len128) != 0) {
                    ERR("128-bit arm disagrees with bignum for %.17g "
                        "(len %zu/%zu places %d/%d)",
                        v, len128, lenB, places128, placesB);
                }
                armChecked128++;
            }
        }

        /* The differential is vacuous if an arm never runs */
        if (armChecked64 < 1000 || armChecked128 < 1000) {
            ERR("Arm coverage too thin: 64-bit %d, 128-bit %d", armChecked64,
                armChecked128);
        }
#undef ARM_NEXT
    }

    TEST("UNIT: minimum buffer contract at worst-case widths") {
        /* Two adversarial values whose fixed-point renderings are the
         * widest possible (25 bytes: sign + 17 digits + pad + '.0' /
         * leading zeros); the entry guard must reject anything under
         * the documented minimum and succeed at it */
        const uint64_t worstBits[] = {0xc451a2101c943a1eULL,
                                      0xbecca11f1ad2249dULL};
        for (size_t i = 0; i < COUNT_ARRAY(worstBits); i++) {
            double v;
            memcpy(&v, &worstBits[i], sizeof(v));

            uint8_t tiny[25];
            if (StrDoubleFormatToBufNice(tiny, sizeof(tiny), v) != 0) {
                ERR("Accepted undersized buffer for worst case %zu", i);
            }

            uint8_t exact[26] = {0};
            const size_t len =
                StrDoubleFormatToBufNice(exact, sizeof(exact), v);
            if (!len) {
                ERR("Rejected minimum-size buffer for worst case %zu", i);
            } else {
                const double back = strtod((const char *)exact, NULL);
                if (memcmp(&back, &v, sizeof(back)) != 0) {
                    ERR("Worst-case %zu did not roundtrip: %.*s", i, (int)len,
                        exact);
                }
            }
        }
    }

    TEST("UNIT: renderer branch vectors (one per decimal placement)") {
        /* One labeled vector per placement branch in
         * StrDoubleFormatToBufNice so a renderer regression names the
         * exact branch instead of failing a distant sweep */
        const struct {
            const char *branch;
            double value;
            const char *expected;
        } vectors[] = {
            {"places==0 leading zero", 0.5, "0.5"},
            {"placeOffset==0 append .0", 1234.0, "1234.0"},
            {"placeOffset<0 split middle", 12.5, "12.5"},
            {"placeOffset<6 zero pad", 12300.0, "12300.0"},
            {"placeOffset>=6 exponential", 15000000000.0, "1.5e+10"},
            {"exponential single digit", 1e100, "1.0e+100"},
            {"places>-6 leading zeros", 0.001, "0.001"},
            {"places<=-6 exponential negative", 1.5e-10, "1.5e-10"},
            {"negative sign passthrough", -12.5, "-12.5"},
        };
        for (size_t i = 0; i < COUNT_ARRAY(vectors); i++) {
            uint8_t buf[64] = {0};
            const size_t len =
                StrDoubleFormatToBufNice(buf, sizeof(buf), vectors[i].value);
            if (len != strlen(vectors[i].expected) ||
                memcmp(buf, vectors[i].expected, len) != 0) {
                ERR("Branch '%s': got %.*s want %s", vectors[i].branch,
                    (int)len, buf, vectors[i].expected);
            }
        }
    }

    TEST("integer exponent estimation output pinning") {
        /* Verify that our fast exponent estimation is working correctly
         * across a wide range of values */
        struct {
            double value;
            const char *expected;
        } tests[] = {
            {1.0, "1.0"},
            {10.0, "10.0"},
            {100.0, "100.0"},
            {1000.0, "1000.0"},
            {0.1, "0.1"},
            {0.01, "0.01"},
            {0.001, "0.001"},
            {1e15, "1.0e+15"},
            {1e-15, "1.0e-15"},
            {1e100, "1.0e+100"},
            {1e-100, "1.0e-100"},
            {3.14159265358979, "3.14159265358979"},
            {2.71828182845905, "2.71828182845905"},
        };

        for (size_t i = 0; i < COUNT_ARRAY(tests); i++) {
            uint8_t buf[64] = {0};
            size_t len =
                StrDoubleFormatToBufNice(buf, sizeof(buf), tests[i].value);
            assert(len == strlen(tests[i].expected));
            assert(memcmp(buf, tests[i].expected, len) == 0);
        }
    }

    TEST("prefetch and inline optimization verification") {
        /* Test that our optimizations don't break edge cases */
        double edgeCases[] = {
            DBL_MIN,                 /* Smallest positive normal */
            DBL_MAX,                 /* Largest finite */
            1.0 / DBL_MAX,           /* Very small */
            DBL_MAX / 2,             /* Large */
            1e-308,                  /* Near minimum */
            1e308,                   /* Near maximum */
            0x1.fffffffffffffp+1023, /* Just below DBL_MAX */
        };

        for (size_t i = 0; i < COUNT_ARRAY(edgeCases); i++) {
            uint8_t buf[64] = {0};
            size_t len =
                StrDoubleFormatToBufNice(buf, sizeof(buf), edgeCases[i]);
            /* Just verify it produces output and doesn't crash */
            assert(len > 0);
            assert(len < sizeof(buf));
            /* Verify buffer is null-terminated for safety */
            buf[len] = '\0';
        }
    }

    TEST("boundary values for fast path dispatch") {
        /* Test values around the 64-bit/128-bit/bignum boundaries */
        double boundaryTests[] = {
            1e17, /* 64-bit boundary */
            -1e17,
            9.99999999999e16, /* Just under 1e17 */
            -9.99999999999e16,
            1e36, /* 128-bit boundary */
            -1e36,
            9.99999999999e35, /* Just under 1e36 */
            -9.99999999999e35,
        };

        for (size_t i = 0; i < COUNT_ARRAY(boundaryTests); i++) {
            uint8_t buf[64] = {0};
            size_t len =
                StrDoubleFormatToBufNice(buf, sizeof(buf), boundaryTests[i]);
            assert(len > 0);
            assert(len < sizeof(buf));
            /* Just verify it produces valid output - exact round-trip is tested
             * elsewhere */
            buf[len] = '\0';
        }
    }

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
            printf("Got: %s (len %" PRIu32 ") (expecting: %s (len %zu))\n", buf,
                   (uint32_t)resultLen, results[i], strlen(results[i]));
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

    /* LC_NUMERIC stays "C": the roundtrip checks below parse this
     * formatter's '.'-radix output with strtod, which is
     * locale-sensitive - a comma-decimal environment locale would
     * false-fail every roundtrip. (Costs the %'zu banner grouping.) */
    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "C");

    const size_t testMax = 1e6;
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
            /* Shortest-roundtrip contract: parsing the printed form
             * must recover the exact bits */
            if (len && isfinite(thisRandAttempt)) {
                const double back = strtod((const char *)buf, NULL);
                if (memcmp(&back, &thisRandAttempt, sizeof(back)) != 0) {
                    ERR("Roundtrip failed: %.*s (bits %016" PRIx64 ")",
                        (int)len, buf, randBig);
                }
            }
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
            /* Shortest-roundtrip contract: parsing the printed form
             * must recover the exact bits */
            if (len && isfinite(thisRandAttempt)) {
                const double back = strtod((const char *)buf, NULL);
                if (memcmp(&back, &thisRandAttempt, sizeof(back)) != 0) {
                    ERR("Roundtrip failed: %.*s (value %.17g)", (int)len, buf,
                        thisRandAttempt);
                }
            }
            if (i % (testMax / 100) == 0) {
                printf(".");
                fflush(stdout);
            }
        }
        printf("\n");
        printf("{64-bit: %'d} {128-bit: %'d} {bignums: %'d}\n", methods[1],
               methods[2], methods[3]);
    }

    TEST("PERF: per-path formatting cycles") {
        /* Deterministic per-dispatch-path corpora so scale/estimate
         * changes are measurable in isolation from wall-clock noise */
        uint64_t rng = 0x9E3779B97F4A7C15ULL;
#define SDF_NEXT() (rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17, rng)
        enum { SDF_CORPUS = 4096 };
        static double corpus64[SDF_CORPUS];
        static double corpus128[SDF_CORPUS];
        static double corpusBig[SDF_CORPUS];
        size_t n64 = 0;
        size_t n128 = 0;
        size_t nBig = 0;

        while (n64 < SDF_CORPUS || n128 < SDF_CORPUS || nBig < SDF_CORPUS) {
            const uint64_t bits = SDF_NEXT();
            double v;
            memcpy(&v, &bits, sizeof(v));
            if (!isfinite(v) || v == 0.0) {
                continue;
            }

            uint8_t buf[64] = {0};
            const size_t len = StrDoubleFormatToBufNice(buf, sizeof(buf), v);

            /* Every corpus value must satisfy the roundtrip contract */
            if (len) {
                const double back = strtod((const char *)buf, NULL);
                if (memcmp(&back, &v, sizeof(back)) != 0) {
                    int32_t vexp;
                    const uint64_t vfrac = fractionAndExponent(v, &vexp);
                    ERR("Corpus roundtrip failed: %.*s (bits %016" PRIx64
                        " method %u places %d est %d %%.17g %.17g)",
                        (int)len, buf, bits, resolvedMethod, resolvedPlaces,
                        estimateDecimalScale(vexp, vfrac), v);
                }
            }

            if (resolvedMethod == 1 && n64 < SDF_CORPUS) {
                corpus64[n64++] = v;
            } else if (resolvedMethod == 2 && n128 < SDF_CORPUS) {
                corpus128[n128++] = v;
            } else if (resolvedMethod == 3 && nBig < SDF_CORPUS) {
                corpusBig[nBig++] = v;
            }
        }

        /* checksum keeps the formatting calls observable so the
         * optimizer cannot delete the measured work */
        uint64_t sink = 0;

        {
            PERF_TIMERS_SETUP;
            for (size_t l = 0; l < 32; l++) {
                for (size_t i = 0; i < SDF_CORPUS; i++) {
                    uint8_t buf[64];
                    const size_t len =
                        StrDoubleFormatToBufNice(buf, sizeof(buf), corpus64[i]);
                    sink += len + buf[0];
                }
            }
            PERF_TIMERS_FINISH_PRINT_RESULTS(32 * SDF_CORPUS,
                                             "64-bit path formats");
        }

        {
            PERF_TIMERS_SETUP;
            for (size_t l = 0; l < 8; l++) {
                for (size_t i = 0; i < SDF_CORPUS; i++) {
                    uint8_t buf[64];
                    const size_t len = StrDoubleFormatToBufNice(
                        buf, sizeof(buf), corpus128[i]);
                    sink += len + buf[0];
                }
            }
            PERF_TIMERS_FINISH_PRINT_RESULTS(8 * SDF_CORPUS,
                                             "128-bit path formats");
        }

        {
            PERF_TIMERS_SETUP;
            for (size_t l = 0; l < 2; l++) {
                for (size_t i = 0; i < SDF_CORPUS; i++) {
                    uint8_t buf[64];
                    const size_t len = StrDoubleFormatToBufNice(
                        buf, sizeof(buf), corpusBig[i]);
                    sink += len + buf[0];
                }
            }
            PERF_TIMERS_FINISH_PRINT_RESULTS(2 * SDF_CORPUS,
                                             "bignum path formats");
        }

        printf("    corpus checksum: %" PRIu64 "\n", sink);
#undef SDF_NEXT
    }

    return err;
}

#endif
