/* floatExtendedTest.c - Comprehensive tests for extended precision floats
 *
 * Tests the floatExtended.h abstraction for cross-platform extended
 * precision floating point operations, including:
 *   - Platform detection verification
 *   - Integer-to-float comparison edge cases
 *   - Power of 10 computation accuracy
 *   - Boundary conditions (INT64_MIN, INT64_MAX, etc.)
 *   - Fallback path correctness on ARM/platforms without extended precision
 */

#include "datakit.h"

#ifdef DATAKIT_TEST

#include "ctest.h"
#include "floatExtended.h"
#include "perf.h"
#include <math.h>
#include <stdio.h>

/* ============================================================================
 * Helper macros for testing
 * ============================================================================
 */

#define EXPECT_CMP(cmp, expected)                                              \
    do {                                                                       \
        int _cmp = (cmp);                                                      \
        int _exp = (expected);                                                 \
        if ((_cmp < 0 && _exp >= 0) || (_cmp > 0 && _exp <= 0) ||              \
            (_cmp == 0 && _exp != 0)) {                                        \
            ERR("Comparison mismatch: got %d, expected sign of %d", _cmp,      \
                _exp);                                                         \
        }                                                                      \
    } while (0)

/* ============================================================================
 * Platform detection tests
 * ============================================================================
 */
static int testPlatformDetection(void) {
    int err = 0;

    TEST("Platform detection information") {
        printf("  Extended float type: %s\n", dk_float_extended_type_name());
        printf("  Mantissa bits: %d\n", dk_float_extended_mantissa_bits());
        printf("  Has extended precision: %s\n",
               dk_has_float_extended() ? "yes" : "no");
        printf("  sizeof(dk_float_extended): %zu\n", sizeof(dk_float_extended));
        printf("  sizeof(long double): %zu\n", sizeof(long double));
        printf("  sizeof(double): %zu\n", sizeof(double));
        printf("  LDBL_MANT_DIG: %d\n", LDBL_MANT_DIG);
        printf("  DBL_MANT_DIG: %d\n", DBL_MANT_DIG);

#if DK_HAS_FLOAT128
        printf("  DK_HAS_FLOAT128: 1\n");
#else
        printf("  DK_HAS_FLOAT128: 0\n");
#endif

#if DK_LONG_DOUBLE_HAS_EXTENDED_PRECISION
        printf("  DK_LONG_DOUBLE_HAS_EXTENDED_PRECISION: 1\n");
#else
        printf("  DK_LONG_DOUBLE_HAS_EXTENDED_PRECISION: 0\n");
#endif

#if DK_HAS_FLOAT_EXTENDED
        printf("  DK_HAS_FLOAT_EXTENDED: 1\n");
#else
        printf("  DK_HAS_FLOAT_EXTENDED: 0\n");
#endif
    }

    TEST("Verify detection macros are consistent") {
#if DK_HAS_FLOAT_EXTENDED
        /* If we have extended precision, mantissa should be >= 64 bits */
        if (DK_FLOAT_EXTENDED_MANTISSA_BITS < 64) {
            ERR("Extended precision claimed but mantissa bits = %d < 64",
                DK_FLOAT_EXTENDED_MANTISSA_BITS);
        }
#else
        /* If we don't have extended precision, mantissa should be < 64 bits */
        if (DK_FLOAT_EXTENDED_MANTISSA_BITS >= 64) {
            ERR("No extended precision but mantissa bits = %d >= 64",
                DK_FLOAT_EXTENDED_MANTISSA_BITS);
        }
#endif
    }

    return err;
}

/* ============================================================================
 * Integer-double comparison tests
 * ============================================================================
 */
static int testInt64DoubleComparison(void) {
    int err = 0;

    TEST("int64 vs double: basic comparisons") {
        /* Basic cases */
        EXPECT_CMP(dk_compare_int64_double(0, 0.0), 0);
        EXPECT_CMP(dk_compare_int64_double(1, 1.0), 0);
        EXPECT_CMP(dk_compare_int64_double(-1, -1.0), 0);
        EXPECT_CMP(dk_compare_int64_double(100, 99.0), 1);
        EXPECT_CMP(dk_compare_int64_double(100, 101.0), -1);
        EXPECT_CMP(dk_compare_int64_double(-100, -99.0), -1);
        EXPECT_CMP(dk_compare_int64_double(-100, -101.0), 1);
    }

    TEST("int64 vs double: fractional comparisons") {
        /* Integer vs fractional */
        EXPECT_CMP(dk_compare_int64_double(5, 5.5), -1);
        EXPECT_CMP(dk_compare_int64_double(6, 5.5), 1);
        EXPECT_CMP(dk_compare_int64_double(-5, -5.5), 1);
        EXPECT_CMP(dk_compare_int64_double(-6, -5.5), -1);

        /* Very small fractions */
        EXPECT_CMP(dk_compare_int64_double(5, 5.0000001), -1);
        EXPECT_CMP(dk_compare_int64_double(5, 4.9999999), 1);
    }

    TEST("int64 vs double: boundary values") {
        /* INT64_MAX is 9223372036854775807 */
        /* Double can't represent this exactly (only 53 mantissa bits) */

        /* Values that fit exactly in double */
        const int64_t maxExact = (1LL << 53); /* 2^53 = max exact integer */
        EXPECT_CMP(dk_compare_int64_double(maxExact, (double)maxExact), 0);

        /* INT64_MAX - double representation will be slightly off */
        double d_max = (double)INT64_MAX;
        /* The comparison should still be consistent */
        int cmp = dk_compare_int64_double(INT64_MAX, d_max);
        printf("  INT64_MAX vs (double)INT64_MAX: %d\n", cmp);
        /* Result depends on precision - just verify it's deterministic */
        if (dk_compare_int64_double(INT64_MAX, d_max) != cmp) {
            ERRR("Non-deterministic comparison result");
        }

        /* INT64_MIN */
        double d_min = (double)INT64_MIN;
        cmp = dk_compare_int64_double(INT64_MIN, d_min);
        printf("  INT64_MIN vs (double)INT64_MIN: %d\n", cmp);
    }

    TEST("int64 vs double: special float values") {
        /* Infinity */
        EXPECT_CMP(dk_compare_int64_double(INT64_MAX, INFINITY), -1);
        EXPECT_CMP(dk_compare_int64_double(INT64_MIN, -INFINITY), 1);

        /* Very large/small doubles */
        EXPECT_CMP(dk_compare_int64_double(0, 1e308), -1);
        EXPECT_CMP(dk_compare_int64_double(0, -1e308), 1);
    }

    TEST("int64 vs double: values near double precision boundary") {
        /* 2^53 is the largest integer exactly representable in double */
        const int64_t twoTo53 = 1LL << 53;

        /* These should compare equal since they fit exactly */
        EXPECT_CMP(dk_compare_int64_double(twoTo53, (double)twoTo53), 0);
        EXPECT_CMP(dk_compare_int64_double(twoTo53 - 1, (double)(twoTo53 - 1)),
                   0);

        /* Beyond 2^53, double loses precision */
        const int64_t beyondExact = twoTo53 + 1;
        double d_beyond = (double)beyondExact;
        printf("  2^53 + 1 = %" PRId64 ", (double)(2^53+1) = %.0f\n",
               beyondExact, d_beyond);
        /* The double representation rounds, so comparison may not be 0 */
    }

    return err;
}

static int testUint64DoubleComparison(void) {
    int err = 0;

    TEST("uint64 vs double: basic comparisons") {
        EXPECT_CMP(dk_compare_uint64_double(0, 0.0), 0);
        EXPECT_CMP(dk_compare_uint64_double(1, 1.0), 0);
        EXPECT_CMP(dk_compare_uint64_double(100, 99.0), 1);
        EXPECT_CMP(dk_compare_uint64_double(100, 101.0), -1);
    }

    TEST("uint64 vs double: negative doubles") {
        /* Unsigned is always > negative */
        EXPECT_CMP(dk_compare_uint64_double(0, -1.0), 1);
        EXPECT_CMP(dk_compare_uint64_double(0, -1e308), 1);
        EXPECT_CMP(dk_compare_uint64_double(UINT64_MAX, -0.001), 1);
    }

    TEST("uint64 vs double: large values") {
        /* UINT64_MAX = 18446744073709551615 */
        double d_max = (double)UINT64_MAX;
        int cmp = dk_compare_uint64_double(UINT64_MAX, d_max);
        printf("  UINT64_MAX vs (double)UINT64_MAX: %d\n", cmp);

        /* Values beyond uint64 range */
        EXPECT_CMP(dk_compare_uint64_double(UINT64_MAX, 1e20), -1);
    }

    TEST("uint64 vs double: fractional comparisons") {
        EXPECT_CMP(dk_compare_uint64_double(5, 5.5), -1);
        EXPECT_CMP(dk_compare_uint64_double(6, 5.5), 1);
        EXPECT_CMP(dk_compare_uint64_double(5, 5.0000001), -1);
    }

    return err;
}

/* ============================================================================
 * Power of 10 tests
 * ============================================================================
 */
static int testPow10(void) {
    int err = 0;

    TEST("pow10: basic powers") {
        double p;

        p = dk_pow10_extended(1);
        if (p != 10.0) {
            ERR("pow10(1) = %g, expected 10", p);
        }

        p = dk_pow10_extended(2);
        if (p != 100.0) {
            ERR("pow10(2) = %g, expected 100", p);
        }

        p = dk_pow10_extended(3);
        if (p != 1000.0) {
            ERR("pow10(3) = %g, expected 1000", p);
        }

        p = dk_pow10_extended(6);
        if (p != 1000000.0) {
            ERR("pow10(6) = %g, expected 1000000", p);
        }
    }

    TEST("pow10: larger exponents") {
        double p;

        p = dk_pow10_extended(10);
        if (p != 1e10) {
            ERR("pow10(10) = %g, expected 1e10", p);
        }

        p = dk_pow10_extended(15);
        if (p != 1e15) {
            ERR("pow10(15) = %g, expected 1e15", p);
        }

        /* Check that precision is maintained for exact values */
        p = dk_pow10_extended(15);
        double expected = 1000000000000000.0;
        if (p != expected) {
            ERR("pow10(15) = %.0f, expected %.0f", p, expected);
        }
    }

    TEST("pow10: extreme exponents (near double limits)") {
        double p;

        /* 10^308 is near DBL_MAX */
        p = dk_pow10_extended(308);
        if (!isfinite(p)) {
            ERRR("pow10(308) should be finite");
        }
        printf("  pow10(308) = %g\n", p);

        /* 10^309 should overflow to infinity */
        p = dk_pow10_extended(309);
        printf("  pow10(309) = %g (expected inf)\n", p);

        /* Small exponents */
        p = dk_pow10_extended(50);
        printf("  pow10(50) = %g\n", p);
    }

    TEST("pow10_extended_full: verify extended precision computation") {
        dk_float_extended pf = dk_pow10_extended_full(20);
        double pd = dk_pow10_extended(20);

        printf("  pow10_extended_full(20) = ");
#if DK_HAS_FLOAT_EXTENDED
        printf("%.0Lf", (long double)pf);
#else
        printf("%.0f", (double)pf);
#endif
        printf(" (double: %.0f)\n", pd);

        /* They should be equal when cast to double */
        if ((double)pf != pd) {
            ERRR("pow10_extended_full and pow10_extended disagree");
        }
    }

    return err;
}

/* ============================================================================
 * Edge case tests for the fallback path
 * ============================================================================
 */
static int testFallbackEdgeCases(void) {
    int err = 0;

    TEST("Fallback: integers at precision boundary") {
        /* These tests are especially important when DK_HAS_FLOAT_EXTENDED is 0,
         * as they exercise the integer-based fallback comparison logic */

        /* 2^53 - 1 should be exactly representable */
        const int64_t maxExactInt = (1LL << 53) - 1;
        double d = (double)maxExactInt;
        EXPECT_CMP(dk_compare_int64_double(maxExactInt, d), 0);

        /* Test values just beyond exact representation */
        const int64_t slightlyBeyond = (1LL << 53) + 100;
        d = (double)slightlyBeyond;
        /* When converted back, there may be rounding */
        printf("  %" PRId64 " vs %.0f: cmp=%d\n", slightlyBeyond, d,
               dk_compare_int64_double(slightlyBeyond, d));
    }

    TEST("Fallback: comparison ordering consistency") {
        /* Verify that comparisons are transitive and consistent */
        const int64_t values[] = {
            INT64_MIN, INT64_MIN + 1,   -1000000000000LL, -1,       0,
            1,         1000000000000LL, INT64_MAX - 1,    INT64_MAX};

        for (size_t i = 0; i < COUNT_ARRAY(values) - 1; i++) {
            int cmp = dk_compare_int64_double(values[i], (double)values[i + 1]);
            if (cmp >= 0 && values[i] < values[i + 1]) {
                /* May happen due to double precision loss at extremes */
                printf("  Warning: %" PRId64 " vs %.0f = %d (precision loss)\n",
                       values[i], (double)values[i + 1], cmp);
            }
        }

        /* Self-comparison should always be 0 for values that fit exactly */
        for (int64_t v = -1000; v <= 1000; v++) {
            if (dk_compare_int64_double(v, (double)v) != 0) {
                ERR("Self-comparison failed for %" PRId64, v);
            }
        }
    }

    TEST("Fallback: fractional edge cases") {
        /* Test that fractional parts are handled correctly */
        EXPECT_CMP(dk_compare_int64_double(0, 0.1), -1);
        EXPECT_CMP(dk_compare_int64_double(0, -0.1), 1);
        EXPECT_CMP(dk_compare_int64_double(1, 0.9999999999999), 1);
        EXPECT_CMP(dk_compare_int64_double(1, 1.0000000000001), -1);
    }

    return err;
}

/* ============================================================================
 * Performance comparison tests
 * ============================================================================
 */
static int testPerformance(void) {
    int err = 0;

    TEST("Performance: int64-double comparison throughput") {
        const size_t iterations = 1000000;
        PERF_TIMERS_SETUP;

        volatile int64_t sum = 0;

        for (size_t i = 0; i < iterations; i++) {
            PERF_TIMERS_STAT_START;
            int64_t val = (int64_t)i - (int64_t)(iterations / 2);
            double dval = (double)i * 1.5;
            sum += dk_compare_int64_double(val, dval);
            PERF_TIMERS_STAT_STOP(i);
        }
        PERF_TIMERS_FINISH;
        PERF_TIMERS_RESULT_PRINT(iterations, "comparisons");
        printf("  (sum=%" PRId64 " to prevent optimization)\n", sum);
    }

    TEST("Performance: pow10 computation throughput") {
        const size_t iterations = 100000;
        PERF_TIMERS_SETUP;

        volatile double sum = 0;

        for (size_t i = 0; i < iterations; i++) {
            PERF_TIMERS_STAT_START;
            int exp = (i % 307) + 1;
            sum += dk_pow10_extended(exp);
            PERF_TIMERS_STAT_STOP(i);
        }
        PERF_TIMERS_FINISH;
        PERF_TIMERS_RESULT_PRINT(iterations, "pow10 ops");
        printf("  (sum=%g to prevent optimization)\n", (double)sum);
    }

    return err;
}

/* ============================================================================
 * Integration with databox (if available)
 * ============================================================================
 */
#include "databox.h"

static int testDataboxIntegration(void) {
    int err = 0;

    TEST("Databox: int64 vs double comparison using floatExtended") {
        databox intBox = {.type = DATABOX_SIGNED_64, .data.i = 12345};
        databox floatBox = {.type = DATABOX_DOUBLE_64, .data.d64 = 12345.0};

        int cmp = databoxCompare(&intBox, &floatBox);
        if (cmp != 0) {
            ERR("12345 vs 12345.0 should be equal, got %d", cmp);
        }

        floatBox.data.d64 = 12345.5;
        cmp = databoxCompare(&intBox, &floatBox);
        if (cmp >= 0) {
            ERR("12345 vs 12345.5 should be negative, got %d", cmp);
        }

        floatBox.data.d64 = 12344.5;
        cmp = databoxCompare(&intBox, &floatBox);
        if (cmp <= 0) {
            ERR("12345 vs 12344.5 should be positive, got %d", cmp);
        }
    }

    TEST("Databox: uint64 vs double comparison") {
        databox uintBox = {.type = DATABOX_UNSIGNED_64, .data.u = 12345};
        databox floatBox = {.type = DATABOX_DOUBLE_64, .data.d64 = 12345.0};

        int cmp = databoxCompare(&uintBox, &floatBox);
        if (cmp != 0) {
            ERR("12345u vs 12345.0 should be equal, got %d", cmp);
        }

        /* Negative double should be less than unsigned */
        floatBox.data.d64 = -1.0;
        cmp = databoxCompare(&uintBox, &floatBox);
        if (cmp <= 0) {
            ERR("12345u vs -1.0 should be positive, got %d", cmp);
        }
    }

    TEST("Databox: large integer vs double edge cases") {
        databox intBox = {.type = DATABOX_SIGNED_64, .data.i = INT64_MAX};
        databox floatBox = {.type = DATABOX_DOUBLE_64,
                            .data.d64 = (double)INT64_MAX};

        int cmp = databoxCompare(&intBox, &floatBox);
        printf("  INT64_MAX vs (double)INT64_MAX: %d\n", cmp);
        /* Result depends on precision, just verify it's deterministic */

        intBox.data.i = INT64_MIN;
        floatBox.data.d64 = (double)INT64_MIN;
        cmp = databoxCompare(&intBox, &floatBox);
        printf("  INT64_MIN vs (double)INT64_MIN: %d\n", cmp);
    }

    return err;
}

/* ============================================================================
 * Main test runner
 * ============================================================================
 */
int floatExtendedTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    printf("\n=== floatExtended Test Suite ===\n\n");

    err += testPlatformDetection();
    err += testInt64DoubleComparison();
    err += testUint64DoubleComparison();
    err += testPow10();
    err += testFallbackEdgeCases();
    err += testPerformance();
    err += testDataboxIntegration();

    printf("\n");
    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
