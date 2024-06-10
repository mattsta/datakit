#include "../str.h"

#include "../datakit.h"
#include <float.h>
#include <math.h>

/* ====================================================================
 * Integer to String conversions from acf
 * ==================================================================== */
/* Powers of 10. */
#define PO2 100ULL
#define PO4 10000ULL
#define PO8 100000000ULL
#define PO10 10000000000ULL
#define PO16 10000000000000000ULL

/* 64 bits worth of '0' characters (i.e., 8 characters). */
/* 0x30 is character '0' */
#define ZERO_CHARS 0x3030303030303030ULL

/**
 * encode_* functions unpack (pairs of) numbers into BCD: each byte
 * contains exactly one decimal digit.
 *
 * The basic idea is to use SWAR (SIMD within a register) and perform
 * low-precision arithmetic on several values in parallel.
 *
 * Most non-obviousness lies in the conversion of integer division
 * constants to multiplication, shift and mask by hand.  Decent
 * compilers do it for scalars, but we can't easily express a SWAR
 * integer division.
 *
 * The trick is to choose a low enough precision that the fixed point
 * multiplication won't overflow into the next packed value (and high
 * enough that the truncated division is exact for the relevant
 * range), and to pack values so that the final result ends up in the
 * byte we want.  There are formulae to determine how little precision
 * we need given an input range and a constant divisor, but for our
 * cases, one can also check exhaustively (:
 *
 * The remainder is simple: given d = x / k, x % k = x - k * d.
 */

/**
 * SWAR unpack [100 * hi + lo] to 4 decimal bytes, assuming hi and lo
 * \in [0, 100)
 */
DK_INLINE_ALWAYS uint32_t encodeHundreds_(const uint32_t hi,
                                          const uint32_t lo) {
    /*
     * Pack everything in a single 32 bit value.
     *
     * merged = [ hi 0 lo 0 ]
     */
    const uint32_t merged = hi | (lo << 16);
    /*
     * Fixed-point multiplication by 103/1024 ~= 1/10.
     */
    uint32_t tens = (merged * 103UL) >> 10;

    /*
     * Mask away garbage bits between our digits.
     *
     * tens = [ hi/10 0 lo/10 0 ]
     *
     * On a platform with more restricted literals (ARM, for
     * instance), it may make sense to and-not with the middle
     * bits.
     */
    tens &= (0xFUL << 16) | 0xFUL;

    /*
     * x mod 10 = x - 10 * (x div 10).
     *
     * (merged - 10 * tens) = [ hi%10 0 lo%10 0 ]
     *
     * Then insert these values between tens.  Arithmetic instead
     * of bitwise operation helps the compiler merge this with
     * later increments by a constant (e.g., ZERO_CHARS).
     */
    return tens + ((merged - 10UL * tens) << 8);
}

/**
 * SWAR encode 10000 hi + lo to byte (unpacked) BCD.
 */
DK_INLINE_ALWAYS uint64_t encodeTenThousands_(const uint64_t hi,
                                              const uint64_t lo) {
    const uint64_t merged = hi | (lo << 32);

    /* Truncate division by 100: 10486 / 2**20 ~= 1/100. */
    const uint64_t top =
        ((merged * 10486ULL) >> 20) & ((0x7FULL << 32) | 0x7FULL);

    /* Trailing 2 digits in the 1e4 chunks. */
    const uint64_t bot = merged - 100ULL * top;

    /*
     * We now have 4 radix-100 digits in little-endian order, each
     * in its own 16 bit area.
     */
    const uint64_t hundreds = (bot << 16) + top;

    /* Divide and mod by 10 all 4 radix-100 digits in parallel. */
    uint64_t tens = (hundreds * 103ULL) >> 10;
    tens &= (0xFULL << 48) | (0xFULL << 32) | (0xFULL << 16) | 0xFULL;
    tens += (hundreds - 10ULL * tens) << 8;

    return tens;
}

/**
 * Range-specialised version of itoa.
 *
 * We always convert to fixed-width BCD then shift away any leading
 * zero.  The slop will manifest as writing zero bytes after our
 * encoded string, which is acceptable: we never write more than the
 * maximal length (10 or 20 characters).
 */

/**
 * itoa for x < 100.
 */
__attribute__((no_sanitize("unsigned-integer-overflow"))) /* x - 10 */
DK_INLINE_ALWAYS char *
itoaHundred_(char *out, const uint32_t x) {
    /*
     * -1 if x < 10, 0 otherwise.  Tried to get an sbb, but ?:
     * gets us branches.
     */
    const int32_t small = (int32_t)(x - 10) >> 8;
    uint32_t base = (uint32_t)'0' | ((uint32_t)'0' << 8);
    /*
     * Probably not necessary, but why not abuse smaller constants?
     * Also, see block comment above idiv_POx functions.
     */
    const uint32_t hi = (x * 103UL) >> 10;
    const uint32_t lo = x - 10UL * hi;

    base += hi + (lo << 8);

    /* Shift away the leading zero (shift by 8) if x < 10. */
    base >>= small & 8;

    memcpy(out, &base, 2);

    /* 2 + small = 1 if x < 10, 2 otherwise. */
    return out + 2 + small;
}

/**
 * itoa for x < 10k.
 */
/* overflow catches the -8U and conversion catches the + ZERO_CHARS */
__attribute__((no_sanitize("unsigned-integer-overflow", "implicit-conversion")))
DK_INLINE_ALWAYS char *
itoaTenThousand_(char *out, const uint32_t x) {
    const uint32_t x_div_PO2 = (x * 10486UL) >> 20;
    const uint32_t x_mod_PO2 = x - PO2 * x_div_PO2;
    uint32_t buf = encodeHundreds_(x_div_PO2, x_mod_PO2);

    /*
     * Count leading (in memory, trailing in register: we're
     * little endian) zero bytes: count leading zero bits and
     * round down to 8.
     */
    uint32_t zeros = __builtin_ctz(buf) & -8U;
    buf += ZERO_CHARS; /* BCD -> ASCII. */
    buf >>= zeros;     /* Shift away leading zero characters */
    memcpy(out, &buf, 4);

    /* zeros is in bits; convert to bytes to find actual length. */
    return out + 4 - zeros / 8;
}

/**
 * 32 bit helpers for truncation by constant.
 *
 * We only need them because GCC is stupid with likely/unlikely
 * annotations: unlikely code is compiled with an extreme emphasis on
 * size, up to compiling integer division by constants to actual div
 * instructions.  In turn, we want likely annotations because we only
 * get a nice ladder of forward conditional jumps when there is no
 * code between if blocks.  We convince GCC that our "special" cases
 * for shorter integers aren't slowpathed guards by marking each
 * conditional as likely.
 *
 * The constants are easily proven correct (or compared with those
 * generated by a reference compiler, e.g., GCC or clang).  For
 * example,
 *
 *   1/10000 ~= k = 3518437209 / 2**45 = 1/10000 + 73/21990232555520000.
 *
 * Let eps = 73/21990232555520000; for any 0 <= x < 2**32,
 * floor(k * x) <= floor(x / 10000 + x * eps)
 *              <= floor(x / 10000 + 2**32 * eps)
 *              <= floor(x / 10000 + 2e-5).
 *
 * Given that x is unsigned, flooring the left and right -hand sides
 * will yield the same value as long as the error term
 * (x * eps <= 2e-5) is less than 1/10000, and 2e-5 < 10000.  We finally
 * conclude that 3518437209 / 2**45, our fixed point approximation of
 * 1/10000, is always correct for truncated division of 32 bit
 * unsigned ints.
 */

/**
 * Divide a 32 bit int by 1e4.
 */
DK_INLINE_ALWAYS uint32_t idiv_PO4(const uint32_t x) {
    const uint64_t wide = x;
    const uint64_t mul = 3518437209UL;

    return (wide * mul) >> 45;
}

/**
 * Divide a 32 bit int by 1e8.
 */
DK_INLINE_ALWAYS uint64_t idivPO8_(const uint32_t x) {
    const uint64_t wide = x;
    const uint64_t mul = 1441151881UL;

    return (wide * mul) >> 57;
}

__attribute__((no_sanitize("unsigned-integer-overflow", "implicit-conversion")))
size_t
StrUInt32ToBuf(void *out_, size_t dstLen, uint32_t x) {
    char *out = out_;
    const char *const outStart = out_;

    if (dstLen < 10) {
        return 0;
    }

    /*
     * Smaller numbers can be encoded more quickly.  Special
     * casing them makes a significant difference compared to
     * always going through 8-digit encoding.
     */
    if (likely(x < PO2)) {
        return itoaHundred_(out, x) - outStart;
    }

    if (likely(x < PO4)) {
        return itoaTenThousand_(out, x) - outStart;
    }

    /*
     * Manual souped up common subexpression elimination.
     *
     * The sequel always needs x / PO4 and x % PO4.  Compute them
     * here, before branching.  We may also need x / PO8 if
     * x >= PO8.  Benchmarking shows that performing this division
     * by constant unconditionally doesn't hurt.  If x >= PO8, we'll
     * always want x_div_PO4 = (x % PO8) / PO4.  We compute that
     * in a roundabout manner to reduce the makespan, i.e., the
     * length of the dependency chain for (x % PO8) % PO4 = x % PO4.
     */
    uint32_t x_div_PO4 = idiv_PO4(x);
    const uint32_t x_mod_PO4 = x - x_div_PO4 * PO4;
    const uint32_t x_div_PO8 = idivPO8_(x);
    /*
     * We actually want x_div_PO4 = (x % PO8) / PO4.
     * Subtract what would have been removed by (x % PO8) from
     * x_div_PO4.
     */
    x_div_PO4 -= x_div_PO8 * PO4;
    /*
     * Finally, we can unconditionally encodeTenThousands_ the
     * values we obtain after division by PO8 and fixup by
     * x_div_PO8 * PO4.
     */
    uint64_t buf = encodeTenThousands_(x_div_PO4, x_mod_PO4);

    if (likely(x < PO8)) {
        const uint32_t zeros = __builtin_ctzll(buf) & -8U;
        buf += ZERO_CHARS;
        buf >>= zeros;

        memcpy(out, &buf, 8);
        return (out + 8 - zeros / 8) - outStart;
    }

    /* 32 bit integers are always below 1e10. */
    buf += ZERO_CHARS;
    out = itoaHundred_(out, x_div_PO8);

    memcpy(out, &buf, 8);
    return (out + 8) - outStart;
}

/**
 * 64 bit helpers for truncation by constant.
 */

/**
 * Divide a 64 bit int by 1e4.
 */
DK_INLINE_ALWAYS uint64_t ldivPO4_(const uint64_t x) {
    const __uint128_t wide = x;
    const __uint128_t mul = 3777893186295716171ULL;

    return (wide * mul) >> 75;
}

/**
 * Divide a 64 bit int by 1e8.
 */
DK_INLINE_ALWAYS uint64_t ldivPO8_(const uint64_t x) {
    const __uint128_t wide = x;
    const __uint128_t mul = 12379400392853802749ULL;

    return (wide * mul) >> 90;
}

/**
 * Divide a 64 bit int by 1e16.
 */
DK_INLINE_ALWAYS uint64_t ldivPO16_(const uint64_t x) {
    const __uint128_t wide = x;
    const __uint128_t mul = 4153837486827862103ULL;

    return (wide * mul) >> 115;
}

/* Requires 20 byte 'out' */
__attribute__((no_sanitize("unsigned-integer-overflow", "implicit-conversion")))
size_t
StrUInt64ToBuf(void *out_, size_t dstLen, uint64_t x) {
    char *out = out_;
    const char *const outStart = out_;
    (void)dstLen;

    assert(dstLen >= 20);

    if (likely(x < PO2)) {
        return itoaHundred_(out, x) - outStart;
    }

    if (likely(x < PO4)) {
        return itoaTenThousand_(out, x) - outStart;
    }

    uint64_t x_div_PO4 = ldivPO4_(x);
    const uint64_t x_mod_PO4 = x - x_div_PO4 * PO4;

    /* Benchmarking shows the long division by PO8 hurts
     * performance for PO4 <= x < PO8.  Keep encodeTenThousands_
     * conditional for an_ltoa. */
    if (likely(x < PO8)) {
        uint64_t buf8;
        uint32_t zeros;

        buf8 = encodeTenThousands_(x_div_PO4, x_mod_PO4);
        zeros = __builtin_ctzll(buf8) & -8U;
        buf8 += ZERO_CHARS;
        buf8 >>= zeros;

        memcpy(out, &buf8, 8);
        return (out + 8 - zeros / 8) - outStart;
    }

    /* See block comment in StrUInt64ToBuf. */
    const uint64_t x_div_PO8 = ldivPO8_(x);
    x_div_PO4 = x_div_PO4 - x_div_PO8 * PO4;
    uint64_t buf = encodeTenThousands_(x_div_PO4, x_mod_PO4) + ZERO_CHARS;

    /*
     * Add a case for PO8 <= x < PO10 because itoaHundred_ is much
     * quicker than a second call to encodeTenThousands_; the
     * same isn't true of itoaTenThousand_.
     */
    if (likely(x < PO10)) {
        out = itoaHundred_(out, x_div_PO8);
        memcpy(out, &buf, 8);
        return (out + 8) - outStart;
    }

    /*
     * Again, long division by PO16 hurts, so do the rest
     * conditionally.
     */
    if (likely(x < PO16)) {
        /* x_div_PO8 < PO8 < 2**32, so idiv_PO4 is safe. */
        const uint64_t hi_hi = idiv_PO4(x_div_PO8);
        const uint32_t hi_lo = x_div_PO8 - hi_hi * PO4;
        uint64_t buf_hi = encodeTenThousands_(hi_hi, hi_lo);
        const uint32_t zeros = __builtin_ctzll(buf_hi) & -8U;
        buf_hi += ZERO_CHARS;
        buf_hi >>= zeros;

        memcpy(out, &buf_hi, 8);
        out += 8 - zeros / 8;
        memcpy(out, &buf, 8);
        return (out + 8) - outStart;
    }

    uint64_t hi = ldivPO16_(x);
    uint64_t mid = x_div_PO8 - hi * PO8;

    const uint32_t mid_hi = idiv_PO4(mid);
    const uint32_t mid_lo = mid - mid_hi * PO4;
    const uint64_t buf_mid = encodeTenThousands_(mid_hi, mid_lo) + ZERO_CHARS;

    out = itoaTenThousand_(out, hi);
    memcpy(out, &buf_mid, 8);
    memcpy(out + 8, &buf, 8);
    return (out + 16) - outStart;
}

/* Requires 21 byte 'out' */
size_t StrInt64ToBuf(void *out_, size_t dstLen, int64_t x) {
    char *out = out_;
    bool negative = x < 0;
    assert(dstLen > 0);

    /* The main loop works with 64bit unsigned integers for simplicity, so
     * we convert the number here and remember if it is negative. */
    if (negative) {
        const size_t written =
            StrUInt64ToBuf(out + 1, dstLen - 1, DK_INT64_TO_UINT64(x));

        /* Number is negative, so make it negative in the string. */
        out[0] = '-';

        return written + 1;
    }

    return StrUInt64ToBuf(out, dstLen, x);
}

/* Obsolete poorly written legacy functions */
#if 0
#include <stdio.h> /* snprintf */
/* Convert a double to a string representation. Returns the number of bytes
 * required. The representation should always be parsable by strtod(3). */
int32_t StrDoubleToBuf(void *_buf, size_t len, double value) {
    char *buf = _buf;
    if (isnan(value)) {
        len = snprintf(buf, len, "nan");
    } else if (isinf(value)) {
        if (value < 0) {
            len = snprintf(buf, len, "-inf");
        } else {
            len = snprintf(buf, len, "inf");
        }
    } else if (value == 0) {
        /* See: http://en.wikipedia.org/wiki/Signed_zero, "Comparisons". */
        if (1.0 / value < 0) {
            len = snprintf(buf, len, "-0");
        } else {
            len = snprintf(buf, len, "0");
        }
    } else {
        double min = -(1ULL << DBL_MANT_DIG);
        double max = (1ULL << DBL_MANT_DIG) - 1;
        if (value > min && value < max && value == ((double)((int64_t)value))) {
            len = StrInt64ToBuf(buf, len, (int64_t)value);
        } else {
            len = snprintf(buf, len, "%.17g", value);
        }
    }

    return len;
}
#endif

/* ====================================================================
 * 128 bit integer string printing
 * ==================================================================== */
/* Note: a full-width __uint128_t printed in base 10 is 40 characters */
#define PO9 1000000000ULL
size_t StrUInt128ToBuf(void *buf_, size_t bufLen, __uint128_t n) {
    assert(bufLen >= 40);

    uint8_t *buf = (uint8_t *)buf_;

    /* We wite from back-to-front, so the offset starts at the end
     * of the buffer and "grows" towards the front. */
    size_t offset = bufLen - 1;

    /* Extract nine digits quickly with SIMD printing until fewer than
     * nine digits remain: */
    while (likely(n >= PO9)) {
        /* Implementation note:
         * clang properly optimizes this into an optimal math re-ordered:
         *  a = n / 1000000000;
         *  rem = (n - 1000000000 * a);
         *  n = a; */
        const uint64_t rem = n % PO9;
        n /= PO9;

        offset -= 9;
        StrUInt9DigitsToBuf(buf + offset, rem);
    }

    /* Now at this point, we know we have fewer than nine digits remaining, so
     * we can just call the nine digit extractor *one* more time and re-align
     * results (since the nine digit extractor produces right-aligned and
     * front-zero-padded results instead of left-aligned strings when the
     * value isn't a full 9 digits) */
    assert(n < PO9);

    /* Cheat a bit here by counting the remaining digits for
     * buffer offset math. */
    const size_t digits = StrDigitCountUInt32((uint32_t)n);

    if (offset >= 9) {
        /* Back up the buffer again to prepare for the last 9-digit splat
         * printing (which is actually the _first_ up-to-9 digits in the
         * result). */
        offset -= 9;
        StrUInt9DigitsToBuf(buf + offset, (uint32_t)n);

        /* Return offset to front-of-written-digits by removing zero-padded
         * digit
         * positions in front of the final actually written digits splat. */
        offset += (9 - digits);
    } else {
        /* buffer to small for direct 9 splat, so do some wiggles */
        uint8_t lastSplat[9];

        /* Copy last digits to a custom buffer, then copy the digits in the
         * custom buffer to the actual buffer */

        StrUInt9DigitsToBuf(lastSplat, (uint32_t)n);
        memcpy(buf + offset, lastSplat + 9 - digits, digits);

        /* Update accounting so the final single memmove() can do its thing */
        offset -= digits;
    }

    /* The "populate string reverse because we don't know its final size"
     * math is resolved here to figure out the actual populated digits. */
    const size_t written = bufLen - offset - 1;

    /* Now move the the entire buffer right-aligned output back to the beginning
     * of the user's buffer so they can read the result from offset 0 as one
     * would expect to happen as the result of filling a character buffer. */
    memmove(buf, buf + offset, written);

    /* Return length of result in bytes starting from front of buffer */
    return written;
}

size_t StrInt128ToBuf(void *buf_, size_t bufLen, __int128_t n) {
    uint8_t *buf = (uint8_t *)buf_;

    assert(bufLen > 0);

    if (n < 0) {
        buf[0] = '-';
        return 1 +
               StrUInt128ToBuf(buf + 1, bufLen - 1, DK_INT128_TO_UINT128(n));
    }

    return StrUInt128ToBuf(buf, bufLen, n);
}
