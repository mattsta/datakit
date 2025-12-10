#include "../str.h"

/* ====================================================================
 * String to int64_t
 * ==================================================================== */
/* Convert a string into int64_t. Returns true if string could be parsed
 * into a (non-overflowing) int64_t, false otherwise. */
bool StrBufToInt64(const void *s, const size_t slen, int64_t *value) {
    const uint8_t *p = s;
    size_t plen = 0;
    bool negative = false;
    uint64_t v = 0;

    if (slen == 0) {
        return false;
    }

    /* Special case: first and only digit is 0. */
    if (slen == 1 && p[0] == '0') {
        *value = 0;
        return true;
    }

    if (p[0] == '-') {
        negative = true;
        p++;
        plen++;

        /* Abort on only a negative sign. */
        if (slen == 1) {
            return false;
        }
    }

    /* First digit should be 1-9, otherwise the string should just be 0. */
    /* This verifies we don't convert "00003" to just "3" for storage
     * when the user _really_ wanted us to store "00003". */
    if (p[0] >= '1' && p[0] <= '9') {
        v = p[0] - '0';
        p++;
        plen++;
    } else {
        return false;
    }

    for (; (plen < slen) && (p[0] >= '0' && p[0] <= '9'); plen++, p++) {
#if 1
        /* Faster... */
        if (__builtin_mul_overflow(v, 10ULL, &v) ||
            __builtin_add_overflow(v, *p & 0x0FULL, &v)) {
            return false;
        }
#else
        if (v > (UINT64_MAX / 10)) { /* Overflow. */
            return false;
        }

        v = StrValTimes10(v);

        if (v > (UINT64_MAX - (p[0] - '0'))) { /* Overflow. */
            return false;
        }

        v += p[0] - '0';
#endif
    }

    /* Return error if not all bytes were used. */
    if (plen != slen) {
        return false;
    }

    if (negative) {
        if (v > DK_INT64_TO_UINT64(INT64_MIN)) {
            return false;
        }

        *value = -(int64_t)v;
    } else {
        if (v > INT64_MAX) { /* Overflow. */
            return false;
        }

        *value = v;
    }

    return true;
}

bool StrBufToUInt64(const void *s, const size_t slen,
                    uint64_t *restrict value) {
    const uint8_t *restrict p = s;
    size_t plen = 0;
    uint64_t v = 0;

    if (slen == 0) {
        return false;
    }

    /* Special case: first and only digit is 0. */
    if (slen == 1 && p[0] == '0') {
        *value = 0;
        return true;
    }

    /* First digit should be 1-9, otherwise the string should just be 0. */
    /* This verifies we don't convert "00003" to just "3" for storage
     * when the user _really_ wanted us to store "00003". */
    if (p[0] >= '1' && p[0] <= '9') {
        v = p[0] - '0';
        p++;
        plen++;
    } else {
        return false;
    }

    for (; (plen < slen) && (p[0] >= '0' && p[0] <= '9'); plen++, p++) {
#if 0
        /* Slower... */
        if (__builtin_mul_overflow(v, 10ULL, &v) ||
            __builtin_add_overflow(v, pDeref & 0x0FULL, &v)) {
            return false;
        }
#else
        const uint64_t before = v;

        v = v * 10ULL + p[0] - '0';

        /* If we added the current character, but got *smaller*,
         * we wrapped around. */
        if (v < before) {
            /* overflow */
            return false;
        }
#endif
    }

    /* Return error if not all bytes were used. */
    if (plen < slen) {
        return false;
    }

    *value = v;

    return true;
}

/* Fast string to uint64_t conversion with no error checking,
 * so as a caller you must only pass in numbers of exactly
 * length len.  Number must not be higher than UINT64_MAX */

/* SWAR (SIMD Within A Register) conversion for 8 ASCII digits to integer.
 * This uses standard C operations on a 64-bit integer - no intrinsics needed.
 *
 * Algorithm: Pack 8 digits into uint64_t, then use parallel
 * multiply-add to combine: d0*10^7 + d1*10^6 + ... + d7*10^0
 */
DK_INLINE_ALWAYS uint64_t parse8DigitsSWAR(const uint8_t *buf) {
    uint64_t val;
    memcpy(&val, buf, sizeof(val));

    /* Subtract '0' (0x30) from each byte */
    val -= 0x3030303030303030ULL;

    /* The key insight: we can combine pairs of digits efficiently.
     * First, pack adjacent pairs: ab cd ef gh -> (a*10+b) (c*10+d) (e*10+f)
     * (g*10+h) as 16-bit values, then combine those pairs, etc.
     *
     * Step 1: Combine pairs of digits into 16-bit values
     * For little-endian: val = [d0, d1, d2, d3, d4, d5, d6, d7]
     * We want: [d0*10+d1, d2*10+d3, d4*10+d5, d6*10+d7] as 16-bit values
     *
     * Mask even bytes (indices 0,2,4,6 in little-endian = d0,d2,d4,d6):
     * even = val & 0x00FF00FF00FF00FF = [d0, 0, d2, 0, d4, 0, d6, 0]
     *
     * Shift odd bytes down (indices 1,3,5,7 = d1,d3,d5,d7):
     * odd = (val >> 8) & 0x00FF00FF00FF00FF = [d1, 0, d3, 0, d5, 0, d7, 0]
     *
     * Combine: even * 10 + odd
     */
    const uint64_t mask = 0x00FF00FF00FF00FFULL;

    /* Combine pairs of digits: d0d1, d2d3, d4d5, d6d7 as 16-bit values */
    val = (val & mask) * 10 + ((val >> 8) & mask);

    /* Now val has 4 x 16-bit values: [d0d1, d2d3, d4d5, d6d7]
     * Combine pairs into 32-bit: [d0d1d2d3, d4d5d6d7] */
    val = (val & 0x0000FFFF0000FFFFULL) * 100 +
          ((val >> 16) & 0x0000FFFF0000FFFFULL);

    /* Finally combine into single 64-bit value */
    val = (val & 0x00000000FFFFFFFFULL) * 10000 + (val >> 32);

    return val;
}

/* Scalar baseline for comparison benchmarking */
DK_INLINE_ALWAYS uint64_t strBufToUInt64Scalar(const uint8_t *buf, size_t len) {
    uint64_t ret = 0;
    for (size_t i = 0; i < len; i++, buf++) {
        ret = (ret * 10ULL) + (*buf - '0');
    }
    return ret;
}

uint64_t StrBufToUInt64Fast(const void *buf_, size_t len) {
    const uint8_t *buf = (const uint8_t *)buf_;
    uint64_t ret = 0;

    /* For 8+ digits, use SWAR conversion */
    while (len >= 8) {
        ret = ret * 100000000ULL + parse8DigitsSWAR(buf);
        buf += 8;
        len -= 8;
    }

    /* Handle remaining digits with scalar loop */
    for (size_t i = 0; i < len; i++, buf++) {
        ret = (ret * 10ULL) + (*buf - '0');
    }

    return ret;
}

/* Expose scalar version for benchmarking comparison */
uint64_t StrBufToUInt64Scalar(const void *buf_, size_t len) {
    return strBufToUInt64Scalar((const uint8_t *)buf_, len);
}

bool StrBufToUInt64FastCheckNumeric(const void *restrict buf_, size_t len,
                                    uint64_t *restrict ret) {
    const uint8_t *buf = (const uint8_t *)buf_;
    *ret = 0;

    for (size_t i = 0; i < len; i++, buf++) {
        const char thing = *buf;
        if (likely(thing >= '0' && thing <= '9')) {
            *ret = (*ret * 10ULL) + (thing - '0');
        } else {
            return false;
        }
    }

    return true;
}

bool StrBufToUInt64FastCheckOverflow(const void *buf_, const size_t len,
                                     uint64_t *restrict value) {
    const uint8_t *restrict buf = (const uint8_t *)buf_;
    uint64_t ret = 0;

    for (size_t i = 0; i < len; i++, buf++) {
        const uint64_t before = ret;
        ret = (ret * 10ULL) + (*buf - '0');

        /* If value got smaller after addition, we wrapped
         * around and our integer parsing is no longer valid. */
        if (ret < before) {
            /* You want overflow let's just give you a biiiig value. */
            *value = UINT64_MAX;
            return false;
        }
    }

    *value = ret;
    return true;
}

#define MIN(a, b) ((a) < (b) ? (a) : (b))
/* Adapted from https://github.com/apache/orc/blob/master/c%2B%2B/src/Int128.cc
 * (Apache2) */
bool StrBufToUInt128(const void *buf, const size_t bufLen, __uint128_t *value) {
    const uint8_t *s = buf;

    size_t position = 0;
    __uint128_t result = 0;

    /* Create 128 bit integer by extracting 64 bit integers then multiplying
     * up to power of (base 10) digits converted.
     * Repeat until all input buffer digits consumed.
     * Digits are extracted forward (from leading digit to trailing digit). */

    /* We break these out into two cases because in the bufLen==39 case, the
     * overflow multiplication checks are noticably slower than direct math. */
#if 1
    if (likely(bufLen < 39)) {
        while (position < bufLen) {
            const size_t lengthOfExtraction = MIN(18, (bufLen - position));
            const uint64_t converted =
                StrBufToUInt64Fast(&s[position], lengthOfExtraction);
            const uint64_t multiply = StrTenPow(lengthOfExtraction);
            result *= multiply;
            result += converted;
            position += lengthOfExtraction;
        }
    } else if (bufLen == 39) {
        /* else, bufLen is exactly 39, so we need to run overflow checking */
        while (position < bufLen) {
            const size_t lengthOfExtraction = MIN(18, (bufLen - position));
            const uint64_t converted =
                StrBufToUInt64Fast(&s[position], lengthOfExtraction);
            const uint64_t multiply = StrTenPow(lengthOfExtraction);
            if (__builtin_mul_overflow(result, multiply, &result) ||
                __builtin_add_overflow(result, converted, &result)) {
                return false;
            }
            position += lengthOfExtraction;
        }
#else

    /* TODO: perf test combinations of:
     *  - one while loop with the length conditions in-lined
     *  - moving __builtins to:
     *    - save 'result' before
     *    - result *= multiply;
     *    - result == converted;
     *    - if (result < previousValue) { return false; }
     */
#endif
    } else {
        /* else, bufLen > 39 */
        /* Maximum length of 2^128 is 39 decimal digits */
        return false;
    }

    *value = result;
    return true;
}

bool StrBufToInt128(const void *buf, const size_t bufLen, __int128_t *value) {
    const uint8_t *s = buf;
    int sign = 1;

    assert(bufLen <= 40);

    size_t digits = bufLen;
    size_t position = 0;
    if (*s == '-') {
        /* Remember number was negative in buffer */
        sign = -1;

        /* Remove buffer length because of negative sign. */
        digits--;

        /* Increase parse start position because of negative sign. */
        position++;
    }

    __uint128_t got;
    const bool success = StrBufToUInt128(&s[position], digits, &got);

    *value = got;

    if (sign < 0 && *value > 0) {
        *value *= -1;
    }

    return success;
}

/* Obsolete poorly written legacy functions */
#if 0
#include <errno.h> /* errno */
bool StrBufToLongDouble(const void *_s, size_t slen, long double *ld) {
    const uint8_t *s = _s;
    char buf[256] = {0};
    if (slen >= sizeof(buf)) {
        return false;
    }

    memcpy(buf, s, slen);
    buf[slen] = '\0';

    errno = 0;
    long double value;
    char *eptr;
    value = strtold(buf, &eptr);
    if (StrIsspace(buf[0]) || eptr[0] != '\0' ||
        (errno == ERANGE &&
         (value == HUGE_VAL || value == -HUGE_VAL || value == 0)) ||
        errno == EINVAL || isnan(value)) {
        return false;
    }

    if (ld) {
        *ld = value;
    }

    return true;
}
#endif
