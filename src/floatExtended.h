#pragma once

/* floatExtended.h - Cross-platform extended precision floating point
 *
 * This header provides a portable abstraction for extended-precision floating
 * point operations. The primary use case is comparing 64-bit integers with
 * double-precision floats without losing precision at the boundaries.
 *
 * Problem:
 *   - On x86_64: long double is 80-bit extended precision (64-bit mantissa)
 *   - On ARM64:  long double is 64-bit (same as double, 53-bit mantissa)
 *   - We need at least 64 mantissa bits to represent all int64_t values exactly
 *
 * Solution hierarchy (compile-time detection):
 *   1. C23 _Float128 (IEEE 754 binary128, 113-bit mantissa)
 *   2. GCC/Clang __float128 (IEEE 754 binary128)
 *   3. Native long double if LDBL_MANT_DIG >= 64 (x86 extended precision)
 *   4. Fallback to double with integer-based comparison logic
 *
 * Usage:
 *   #include "floatExtended.h"
 *
 *   #if DK_HAS_FLOAT_EXTENDED
 *       dk_float_extended x = (dk_float_extended)some_int64;
 *       // Direct comparison is safe
 *   #else
 *       // Use integer-based fallback
 *   #endif
 */

#include <float.h>
#include <stdint.h>

/* ============================================================================
 * Feature Detection
 * ============================================================================
 */

/* Detection priority:
 * 1. Check for explicit CMake-provided DK_USE_FLOAT128 (user override)
 * 2. Check for C23 _Float128 support
 * 3. Check for compiler __float128 support
 * 4. Check if native long double has sufficient precision
 */

/* Allow CMake to force-enable or force-disable float128 */
#ifdef DK_FORCE_NO_FLOAT128
#undef DK_HAS_FLOAT128
#define DK_HAS_FLOAT128 0
#endif

#ifndef DK_HAS_FLOAT128

/* C23 _Float128 detection */
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 202311L)
#if defined(__STDC_IEC_60559_BFP__) || defined(__FLT128_MANT_DIG__)
#define DK_HAS_FLOAT128 1
#define DK_FLOAT128_TYPE_C23 1
#endif
#endif

/* GCC __float128 detection (works on x86_64 and ARM64 with libquadmath) */
#if !defined(DK_HAS_FLOAT128) || !DK_HAS_FLOAT128
#if defined(__SIZEOF_FLOAT128__) || defined(__FLOAT128__)
#define DK_HAS_FLOAT128 1
#define DK_FLOAT128_TYPE_GCC 1
#endif
#endif

/* Clang __float128 detection (x86_64 only currently) */
#if !defined(DK_HAS_FLOAT128) || !DK_HAS_FLOAT128
#if defined(__clang__) && defined(__x86_64__) && __has_extension(float128)
#define DK_HAS_FLOAT128 1
#define DK_FLOAT128_TYPE_GCC 1
#endif
#endif

#endif /* !DK_HAS_FLOAT128 */

/* Final fallback: no float128 available */
#ifndef DK_HAS_FLOAT128
#define DK_HAS_FLOAT128 0
#endif

/* ============================================================================
 * Long Double Precision Detection
 * ============================================================================
 * LDBL_MANT_DIG gives us the mantissa bits:
 *   - x86 extended precision: 64 bits
 *   - IEEE double: 53 bits
 *   - IEEE quad: 113 bits
 *
 * We need >= 64 bits to exactly represent all int64_t values.
 */

#if defined(LDBL_MANT_DIG) && (LDBL_MANT_DIG >= 64)
#define DK_LONG_DOUBLE_HAS_EXTENDED_PRECISION 1
#else
#define DK_LONG_DOUBLE_HAS_EXTENDED_PRECISION 0
#endif

/* ============================================================================
 * Type Definitions
 * ============================================================================
 */

#if DK_HAS_FLOAT128

/* Use 128-bit IEEE float - best precision, portable behavior */
#if defined(DK_FLOAT128_TYPE_C23)
typedef _Float128 dk_float128;
#define DK_FLOAT128_SUFFIX(x) x##f128
#define DK_FLOAT128_LITERAL(x) x##F128
#else
typedef __float128 dk_float128;
#define DK_FLOAT128_SUFFIX(x) x##q
#define DK_FLOAT128_LITERAL(x) x##Q
#endif

/* dk_float_extended: the best available extended precision type */
typedef dk_float128 dk_float_extended;
#define DK_HAS_FLOAT_EXTENDED 1
#define DK_FLOAT_EXTENDED_MANTISSA_BITS 113

#elif DK_LONG_DOUBLE_HAS_EXTENDED_PRECISION

/* Use native long double - sufficient precision on this platform */
typedef long double dk_float_extended;
#define DK_HAS_FLOAT_EXTENDED 1
#define DK_FLOAT_EXTENDED_MANTISSA_BITS LDBL_MANT_DIG

#else

/* No extended precision available - use double as placeholder,
 * but DK_HAS_FLOAT_EXTENDED will be 0 to indicate fallback is needed */
typedef double dk_float_extended;
#define DK_HAS_FLOAT_EXTENDED 0
#define DK_FLOAT_EXTENDED_MANTISSA_BITS DBL_MANT_DIG

#endif

/* ============================================================================
 * Compile-Time Assertions
 * ============================================================================
 */

/* Verify our detection is working correctly */
#if DK_HAS_FLOAT_EXTENDED
_Static_assert(sizeof(dk_float_extended) >= 10 ||
                   (DK_HAS_FLOAT128 && sizeof(dk_float_extended) == 16),
               "dk_float_extended should be at least 80-bit or 128-bit");
#endif

/* ============================================================================
 * Runtime Information (for debugging/logging)
 * ============================================================================
 */

/* Returns a string describing the extended float type in use */
static inline const char *dk_float_extended_type_name(void) {
#if DK_HAS_FLOAT128 && defined(DK_FLOAT128_TYPE_C23)
    return "C23 _Float128 (IEEE 754 binary128)";
#elif DK_HAS_FLOAT128
    return "__float128 (IEEE 754 binary128)";
#elif DK_LONG_DOUBLE_HAS_EXTENDED_PRECISION
    return "long double (x86 80-bit extended)";
#else
    return "double (no extended precision - using integer fallback)";
#endif
}

/* Returns the mantissa bits of the extended float type */
static inline int dk_float_extended_mantissa_bits(void) {
    return DK_FLOAT_EXTENDED_MANTISSA_BITS;
}

/* Returns true if extended precision is available */
static inline bool dk_has_float_extended(void) {
    return DK_HAS_FLOAT_EXTENDED;
}

/* ============================================================================
 * Helper Macros for Integer-Float Comparison
 * ============================================================================
 * These provide a consistent interface regardless of whether extended
 * precision is available.
 */

/* Maximum int64_t value exactly representable in double (2^53) */
#define DK_DOUBLE_MAX_EXACT_INT64 ((int64_t)1 << DBL_MANT_DIG)

/* Check if an int64 can be exactly represented as a double */
#define DK_INT64_FITS_IN_DOUBLE(x)                                             \
    ((x) >= -DK_DOUBLE_MAX_EXACT_INT64 && (x) <= DK_DOUBLE_MAX_EXACT_INT64)

/* Check if a uint64 can be exactly represented as a double */
#define DK_UINT64_FITS_IN_DOUBLE(x) ((x) <= (uint64_t)DK_DOUBLE_MAX_EXACT_INT64)

/* ============================================================================
 * Integer-Float Comparison Helpers
 * ============================================================================
 * These functions handle the tricky edge cases when comparing integers
 * with floating point values, especially when extended precision is not
 * available.
 */

/* Compare int64_t with double, returns -1, 0, or 1 */
static inline int dk_compare_int64_double(int64_t i, double d) {
#if DK_HAS_FLOAT_EXTENDED
    /* Extended precision path - direct comparison is safe */
    const dk_float_extended fi = (dk_float_extended)i;
    const dk_float_extended fd = (dk_float_extended)d;
    if (fi < fd) {
        return -1;
    }

    if (fi > fd) {
        return 1;
    }

    return 0;
#else
    /* Fallback path - handle edge cases explicitly */

    /* Handle special float values */
    if (d != d) { /* NaN */
        return 1; /* NaN compares as greater (arbitrary but consistent) */
    }

    /* Check double range against int64 limits.
     * Note: (double)INT64_MAX rounds up to 9223372036854775808.0
     * which is > INT64_MAX, so we need careful bounds checking. */
    const double int64_min_as_double = -9223372036854775808.0; /* -2^63 exact */
    const double int64_max_as_double = 9223372036854775808.0; /* 2^63 rounded */

    if (d < int64_min_as_double) {
        return 1; /* i > d */
    }

    if (d >= int64_max_as_double) {
        /* d is >= 2^63, which is larger than any int64_t */
        return -1; /* i < d */
    }

    /* If the integer fits exactly in double, compare directly */
    if (DK_INT64_FITS_IN_DOUBLE(i)) {
        const double di = (double)i;
        if (di < d) {
            return -1;
        }

        if (di > d) {
            return 1;
        }

        return 0;
    }

    /* At this point:
     * - d is in range [INT64_MIN, 2^63)
     * - i does not fit exactly in double
     * We can safely truncate d to int64_t */
    const int64_t truncated = (int64_t)d;

    if (i < truncated) {
        return -1;
    }

    if (i > truncated) {
        return 1;
    }

    /* Integer parts equal - check fractional part */
    const double frac = d - (double)truncated;
    if (frac > 0.0) {
        return -1; /* i < d because d has positive fractional part */
    }

    if (frac < 0.0) {
        return 1; /* i > d because d has negative fractional part */
    }

    return 0;
#endif
}

/* Compare uint64_t with double, returns -1, 0, or 1 */
static inline int dk_compare_uint64_double(uint64_t u, double d) {
#if DK_HAS_FLOAT_EXTENDED
    /* Extended precision path - direct comparison is safe */
    const dk_float_extended fu = (dk_float_extended)u;
    const dk_float_extended fd = (dk_float_extended)d;
    if (fu < fd) {
        return -1;
    }

    if (fu > fd) {
        return 1;
    }

    return 0;
#else
    /* Fallback path - handle edge cases explicitly */

    /* Handle special float values */
    if (d != d) { /* NaN */
        return 1;
    }

    /* Negative double is always less than unsigned */
    if (d < 0.0) {
        return 1; /* u > d */
    }

    /* Check if double exceeds uint64 range.
     * Note: (double)UINT64_MAX rounds up to 18446744073709551616.0
     * which is > UINT64_MAX, so we need >= comparison. */
    const double uint64_max_as_double =
        18446744073709551616.0; /* 2^64 rounded */

    if (d >= uint64_max_as_double) {
        return -1; /* u < d */
    }

    /* If the integer fits exactly in double, compare directly */
    if (DK_UINT64_FITS_IN_DOUBLE(u)) {
        const double du = (double)u;
        if (du < d) {
            return -1;
        }

        if (du > d) {
            return 1;
        }

        return 0;
    }

    /* At this point:
     * - d is in range [0, 2^64) - guaranteed by check at line 321
     * - u does not fit exactly in double
     * We can safely truncate d to uint64_t
     * cppcheck doesn't track the range check, but the cast is safe */
    // cppcheck-suppress floatConversionOverflow - d < 2^64 guaranteed by early
    // return above
    const uint64_t truncated = (uint64_t)d;

    if (u < truncated) {
        return -1;
    }

    if (u > truncated) {
        return 1;
    }

    /* Integer parts equal - check fractional part */
    const double frac = d - (double)truncated;
    if (frac > 0.0) {
        return -1; /* u < d */
    }

    return 0; /* frac <= 0 means u >= d, and since truncated == u, u == d */
#endif
}

/* ============================================================================
 * Power of 10 Computation
 * ============================================================================
 * Compute 10^E with extended precision when available.
 * Used for string-to-float conversion.
 */

/* Compute 10^E using extended precision if available.
 * E must be between 1 and 341.
 * Returns the result as a double (potentially with precision loss if
 * extended precision is not available).
 */
static inline double dk_pow10_extended(int e) {
#if DK_HAS_FLOAT_EXTENDED
    dk_float_extended x = 10.0;
    dk_float_extended r = 1.0;
    while (1) {
        if (e & 1) {
            r *= x;
        }

        e >>= 1;
        if (e == 0) {
            break;
        }

        x *= x;
    }

    return (double)r;
#else
    /* Without extended precision, use double directly.
     * This may lose some precision for large exponents. */
    double x = 10.0;
    double r = 1.0;
    while (1) {
        if (e & 1) {
            r *= x;
        }

        e >>= 1;
        if (e == 0) {
            break;
        }

        x *= x;
    }

    return r;
#endif
}

/* Same as dk_pow10_extended but returns dk_float_extended type.
 * Use this when you need to keep the extended precision for further
 * calculations.
 */
static inline dk_float_extended dk_pow10_extended_full(int e) {
    dk_float_extended x = 10.0;
    dk_float_extended r = 1.0;
    while (1) {
        if (e & 1) {
            r *= x;
        }

        e >>= 1;
        if (e == 0) {
            break;
        }

        x *= x;
    }

    return r;
}

/* ============================================================================
 * Test function declaration
 * ============================================================================
 */
#ifdef DATAKIT_TEST
int floatExtendedTest(int argc, char *argv[]);
#endif
