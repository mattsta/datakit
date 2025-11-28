#include "varintFloat.h"
#include "ctest.h"
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

int varintFloatTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int32_t err = 0;

    TEST("FULL precision (lossless) encode/decode") {
        const double values[] = {
            3.14159265358979, -2.71828182845905, 1.41421356237310, 0.0, -0.0,
            123.456789012345};
        size_t count = 6;
        uint8_t buffer[512];

        size_t encoded = varintFloatEncode(buffer, values, count,
                                           VARINT_FLOAT_PRECISION_FULL,
                                           VARINT_FLOAT_MODE_INDEPENDENT);

        if (encoded == 0) {
            ERRR("Failed to encode FULL precision array");
        }

        double decoded[6];
        varintFloatDecode(buffer, count, decoded);

        /* FULL precision should be lossless (within double epsilon) */
        for (size_t i = 0; i < count; i++) {
            double error = fabs(decoded[i] - values[i]);
            if (error > 1e-15) {
                ERR("FULL precision error at [%zu]: %.17f vs %.17f (error: "
                    "%.2e)",
                    i, decoded[i], values[i], error);
            }
        }
    }

    TEST("HIGH precision encode/decode") {
        const double values[] = {100.123456, 200.987654, 300.555555};
        size_t count = 3;
        uint8_t buffer[256];

        size_t encoded = varintFloatEncode(buffer, values, count,
                                           VARINT_FLOAT_PRECISION_HIGH,
                                           VARINT_FLOAT_MODE_INDEPENDENT);
        (void)encoded; /* Size tested via decode */

        double decoded[3];
        varintFloatDecode(buffer, count, decoded);

        /* HIGH precision: ~7 decimal digits (23-bit mantissa) */
        /* Max error should be < 1e-4 for values around 100-300 */
        for (size_t i = 0; i < count; i++) {
            double error = fabs(decoded[i] - values[i]);
            if (error > 1e-4) {
                ERR("HIGH precision error at [%zu]: %.6f vs %.6f (error: %.2e)",
                    i, decoded[i], values[i], error);
            }
        }
    }

    TEST("MEDIUM precision encode/decode") {
        const double values[] = {25.5, 26.3, 24.8, 25.1};
        size_t count = 4;
        uint8_t buffer[256];

        size_t encoded = varintFloatEncode(buffer, values, count,
                                           VARINT_FLOAT_PRECISION_MEDIUM,
                                           VARINT_FLOAT_MODE_INDEPENDENT);
        (void)encoded; /* Size tested via decode */

        double decoded[4];
        varintFloatDecode(buffer, count, decoded);

        /* MEDIUM precision: ~3 decimal digits (10-bit mantissa) */
        /* For values around 25, error should be < 0.05 */
        for (size_t i = 0; i < count; i++) {
            double error = fabs(decoded[i] - values[i]);
            if (error > 0.05) {
                ERR("MEDIUM precision error at [%zu]: %.3f vs %.3f (error: "
                    "%.4f)",
                    i, decoded[i], values[i], error);
            }
        }
    }

    TEST("LOW precision encode/decode") {
        const double values[] = {1.0, 2.0, 3.0, 4.0, 5.0};
        size_t count = 5;
        uint8_t buffer[256];

        size_t encoded =
            varintFloatEncode(buffer, values, count, VARINT_FLOAT_PRECISION_LOW,
                              VARINT_FLOAT_MODE_INDEPENDENT);
        (void)encoded; /* Size tested via decode */

        double decoded[5];
        varintFloatDecode(buffer, count, decoded);

        /* LOW precision: ~1 decimal digit (4-bit mantissa) */
        /* For small integers, error might be up to ~6% */
        for (size_t i = 0; i < count; i++) {
            double error = fabs(decoded[i] - values[i]);
            double rel_error = error / values[i];
            if (rel_error > 0.1) { /* 10% tolerance */
                ERR("LOW precision error at [%zu]: %.1f vs %.1f (rel error: "
                    "%.2f%%)",
                    i, decoded[i], values[i], rel_error * 100);
            }
        }
    }

    TEST("Special values: NaN, Infinity, Zero") {
        const double values[] = {NAN, INFINITY, -INFINITY, 0.0, -0.0};
        size_t count = 5;
        uint8_t buffer[512];

        size_t encoded = varintFloatEncode(buffer, values, count,
                                           VARINT_FLOAT_PRECISION_FULL,
                                           VARINT_FLOAT_MODE_INDEPENDENT);

        if (encoded == 0) {
            ERRR("Failed to encode special values");
        }

        double decoded[5];
        varintFloatDecode(buffer, count, decoded);

        /* Check NaN */
        if (!isnan(decoded[0])) {
            ERRR("NaN not preserved");
        }

        /* Check +Infinity */
        if (!isinf(decoded[1]) || decoded[1] < 0) {
            ERRR("+Infinity not preserved");
        }

        /* Check -Infinity */
        if (!isinf(decoded[2]) || decoded[2] > 0) {
            ERRR("-Infinity not preserved");
        }

        /* Check +0.0 */
        if (decoded[3] != 0.0) {
            ERRR("+0.0 not preserved");
        }

        /* Check -0.0 (may not preserve sign of zero, just check it's zero) */
        if (decoded[4] != 0.0 && decoded[4] != -0.0) {
            ERRR("-0.0 not preserved as zero");
        }
    }

    TEST("COMMON_EXPONENT mode compression") {
        /* Values with similar magnitudes */
        double values[10];
        for (int i = 0; i < 10; i++) {
            values[i] = 1000.0 + i; /* 1000-1009 */
        }

        uint8_t buffer1[512];
        uint8_t buffer2[512];

        size_t size_independent =
            varintFloatEncode(buffer1, values, 10, VARINT_FLOAT_PRECISION_HIGH,
                              VARINT_FLOAT_MODE_INDEPENDENT);

        size_t size_common =
            varintFloatEncode(buffer2, values, 10, VARINT_FLOAT_PRECISION_HIGH,
                              VARINT_FLOAT_MODE_COMMON_EXPONENT);

        /* COMMON_EXPONENT should be more efficient */
        if (size_common >= size_independent) {
            ERR("COMMON_EXPONENT (%zu) not more efficient than INDEPENDENT "
                "(%zu)",
                size_common, size_independent);
        }

        /* Verify correctness */
        double decoded[10];
        varintFloatDecode(buffer2, 10, decoded);

        for (int i = 0; i < 10; i++) {
            double error = fabs(decoded[i] - values[i]);
            if (error > 1e-4) {
                ERR("COMMON_EXPONENT value[%d] error: %.6f vs %.6f", i,
                    decoded[i], values[i]);
                break;
            }
        }
    }

    TEST("DELTA_EXPONENT mode for time series") {
        /* Time series with gradually changing magnitude */
        double values[20];
        for (int i = 0; i < 20; i++) {
            values[i] =
                100.0 * (1.0 + i * 0.01); /* 100.0, 101.0, 102.01, ... */
        }

        uint8_t buffer[512];
        size_t encoded =
            varintFloatEncode(buffer, values, 20, VARINT_FLOAT_PRECISION_HIGH,
                              VARINT_FLOAT_MODE_DELTA_EXPONENT);

        if (encoded == 0) {
            ERRR("Failed to encode DELTA_EXPONENT array");
        }

        /* Verify correctness */
        double decoded[20];
        varintFloatDecode(buffer, 20, decoded);

        for (int i = 0; i < 20; i++) {
            double error = fabs(decoded[i] - values[i]);
            if (error > 1e-4) {
                ERR("DELTA_EXPONENT value[%d] error: %.6f vs %.6f", i,
                    decoded[i], values[i]);
                break;
            }
        }
    }

    TEST("Precision error bounds") {
        double value = 123.456789;
        uint8_t buffer[128];

        /* Test each precision level */
        struct {
            varintFloatPrecision prec;
            double max_error;
        } tests[] = {{VARINT_FLOAT_PRECISION_FULL, 1e-15},
                     {VARINT_FLOAT_PRECISION_HIGH, 1e-4},
                     {VARINT_FLOAT_PRECISION_MEDIUM, 2e-1},
                     {VARINT_FLOAT_PRECISION_LOW, 10.0}};

        for (int i = 0; i < 4; i++) {
            varintFloatEncode(buffer, &value, 1, tests[i].prec,
                              VARINT_FLOAT_MODE_INDEPENDENT);
            double decoded;
            varintFloatDecode(buffer, 1, &decoded);

            double error = fabs(decoded - value);
            if (error > tests[i].max_error) {
                ERR("Precision %d: error %.2e exceeds bound %.2e",
                    tests[i].prec, error, tests[i].max_error);
            }
        }
    }

    TEST("Automatic precision selection") {
        /* Temperature sensor data: ±0.01°C absolute accuracy needed */
        const double values[] = {25.34, 25.35, 25.36, 25.33, 25.37};
        double absoluteError = 0.01;

        /* Calculate average value to determine relative error requirement */
        double avg = 0.0;
        for (int i = 0; i < 5; i++) {
            avg += values[i];
        }
        avg /= 5.0;

        /* Convert absolute error to relative error */
        double relativeError = absoluteError / avg; /* ~0.0004 for 25°C */

        uint8_t buffer[256];
        varintFloatPrecision selected;

        size_t encoded =
            varintFloatEncodeAuto(buffer, values, 5, relativeError,
                                  VARINT_FLOAT_MODE_INDEPENDENT, &selected);

        if (encoded == 0) {
            ERRR("Failed to encode with auto precision");
        }

        /* HIGH or better precision should be selected for 0.04% relative error
         */
        if (selected > VARINT_FLOAT_PRECISION_HIGH) {
            ERR("Selected precision %d too low for %.2e relative error",
                selected, relativeError);
        }

        /* Verify selected precision meets absolute error requirement */
        double decoded[5];
        varintFloatDecode(buffer, 5, decoded);

        for (int i = 0; i < 5; i++) {
            double error = fabs(decoded[i] - values[i]);
            if (error > absoluteError) {
                ERR("Auto-selected precision: error %.4f exceeds tolerance "
                    "%.2f",
                    error, absoluteError);
                break;
            }
        }
    }

    TEST("Compression ratio measurement") {
        /* Large dataset: 1000 sensor readings */
        double values[1000];
        for (int i = 0; i < 1000; i++) {
            values[i] = 25.0 + (i % 100) * 0.1; /* 25.0-34.9°C */
        }

        uint8_t *buffer = malloc(
            varintFloatMaxEncodedSize(1000, VARINT_FLOAT_PRECISION_FULL));
        if (!buffer) {
            ERRR("Failed to allocate buffer");
        }

        /* Compare different precision modes */
        size_t size_full =
            varintFloatEncode(buffer, values, 1000, VARINT_FLOAT_PRECISION_FULL,
                              VARINT_FLOAT_MODE_INDEPENDENT);

        size_t size_medium = varintFloatEncode(
            buffer, values, 1000, VARINT_FLOAT_PRECISION_MEDIUM,
            VARINT_FLOAT_MODE_COMMON_EXPONENT);

        /* MEDIUM + COMMON_EXPONENT should be much smaller than FULL */
        if (size_medium >= size_full) {
            ERR("MEDIUM compression (%zu) not better than FULL (%zu)",
                size_medium, size_full);
        }

        /* Should achieve at least 1.5x compression vs naive */
        size_t naive = 1000 * sizeof(double);
        float ratio = (float)naive / (float)size_medium;
        if (ratio < 1.5f) {
            ERR("Compression ratio %.2fx < 1.5x", ratio);
        }

        free(buffer);
    }

    TEST("Single value encode/decode") {
        double value = 3.14159265358979;
        uint8_t buffer[64];

        size_t encoded =
            varintFloatEncode(buffer, &value, 1, VARINT_FLOAT_PRECISION_FULL,
                              VARINT_FLOAT_MODE_INDEPENDENT);
        if (encoded == 0) {
            ERRR("Failed to encode single value");
        }

        double decoded;
        varintFloatDecode(buffer, 1, &decoded);

        double error = fabs(decoded - value);
        if (error > 1e-15) {
            ERR("Single value error: %.17f vs %.17f (error: %.2e)", decoded,
                value, error);
        }
    }

    TEST_FINAL_RESULT;
}

#ifdef VARINT_FLOAT_TEST_STANDALONE
int main(int argc, char *argv[]) {
    return varintFloatTest(argc, argv);
}
#endif
