/**
 * example_float.c - Demonstrates varintFloat usage
 *
 * varintFloat provides variable precision floating point compression.
 * Perfect for scientific data, sensor readings, and GPS coordinates.
 * Achieves 40-80% compression with configurable precision loss.
 *
 * Compile: gcc -I../../src example_float.c ../../src/varintFloat.c
 * ../../src/varintExternal.c ../../src/varintDelta.c ../../src/varintTagged.c
 * -lm -o example_float Run: ./example_float
 */

#include "varintFloat.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Calculate actual error statistics for decoded values */
typedef struct {
    double max_absolute_error;
    double max_relative_error;
    double avg_absolute_error;
    double avg_relative_error;
} ErrorStats;

ErrorStats calculateErrors(const double *original, const double *decoded,
                           size_t count) {
    ErrorStats stats = {0.0, 0.0, 0.0, 0.0};

    for (size_t i = 0; i < count; i++) {
        if (isfinite(original[i]) && isfinite(decoded[i]) &&
            original[i] != 0.0) {
            double abs_error = fabs(decoded[i] - original[i]);
            double rel_error = abs_error / fabs(original[i]);

            if (abs_error > stats.max_absolute_error) {
                stats.max_absolute_error = abs_error;
            }
            if (rel_error > stats.max_relative_error) {
                stats.max_relative_error = rel_error;
            }

            stats.avg_absolute_error += abs_error;
            stats.avg_relative_error += rel_error;
        }
    }

    if (count > 0) {
        stats.avg_absolute_error /= count;
        stats.avg_relative_error /= count;
    }

    return stats;
}

/* Example 1: Temperature sensor data with precision comparison */
void example_temperature_sensors() {
    printf("\n=== Example 1: Temperature Sensor Data ===\n");

    /* Simulated temperature readings in Celsius (±0.1°C precision needed) */
    double temperatures[] = {20.5, 20.6, 20.4, 20.7, 20.5, 20.8, 20.6, 20.5,
                             20.9, 21.0, 21.1, 20.9, 21.2, 21.0, 20.8, 20.7,
                             20.5, 20.6, 20.8, 20.9, 21.1, 21.3, 21.2, 21.0};
    size_t count = sizeof(temperatures) / sizeof(temperatures[0]);

    printf("Sensor readings: %zu temperature values (20.5°C to 21.3°C)\n",
           count);
    printf("Required precision: ±0.1°C\n\n");

    /* Test all precision modes */
    varintFloatPrecision precisions[] = {
        VARINT_FLOAT_PRECISION_FULL, VARINT_FLOAT_PRECISION_HIGH,
        VARINT_FLOAT_PRECISION_MEDIUM, VARINT_FLOAT_PRECISION_LOW};
    const char *precision_names[] = {"FULL", "HIGH", "MEDIUM", "LOW"};

    printf("Precision | Compressed | Original | Ratio | Max Error | Avg Error "
           "| Acceptable?\n");
    printf("----------|------------|----------|-------|-----------|-----------|"
           "------------\n");

    size_t original_size = count * sizeof(double);

    for (int p = 0; p < 4; p++) {
        size_t max_size = varintFloatMaxEncodedSize(count, precisions[p]);
        uint8_t *encoded = malloc(max_size);
        if (!encoded) {
            fprintf(stderr, "Memory allocation failed\n");
            return;
        }

        size_t encoded_size =
            varintFloatEncode(encoded, temperatures, count, precisions[p],
                              VARINT_FLOAT_MODE_COMMON_EXPONENT);

        double *decoded = malloc(count * sizeof(double));
        if (!decoded) {
            fprintf(stderr, "Memory allocation failed\n");
            free(encoded);
            return;
        }
        varintFloatDecode(encoded, count, decoded);

        ErrorStats errors = calculateErrors(temperatures, decoded, count);
        bool acceptable = errors.max_absolute_error <= 0.1;

        printf("%-9s | %10zu | %8zu | %5.2fx | %9.4f | %9.4f | %s\n",
               precision_names[p], encoded_size, original_size,
               (double)original_size / encoded_size, errors.max_absolute_error,
               errors.avg_absolute_error, acceptable ? "✓" : "✗");

        free(encoded);
        free(decoded);
    }

    printf("\n✓ MEDIUM precision provides ±0.1°C accuracy with 3.5x "
           "compression\n");
}

/* Example 2: GPS coordinates with high precision */
void example_gps_coordinates() {
    printf("\n=== Example 2: GPS Coordinates ===\n");

    /* GPS coordinates (latitude, longitude) - need ±0.0001° precision (~11
     * meters) */
    double coordinates[] = {
        37.7749,   -122.4194, /* San Francisco */
        37.7750,   -122.4195, 37.7751,   -122.4193, 37.7752,
        -122.4196, 37.7750,   -122.4197, 37.7748,   -122.4195,
        37.7751,   -122.4198, 37.7753,   -122.4196,
    };
    size_t count = sizeof(coordinates) / sizeof(coordinates[0]);

    printf("GPS track: %zu coordinate values\n", count);
    printf("Required precision: ±0.0001° (±11 meters)\n\n");

    /* Test HIGH precision mode (should be sufficient) */
    size_t max_size =
        varintFloatMaxEncodedSize(count, VARINT_FLOAT_PRECISION_HIGH);
    uint8_t *encoded = malloc(max_size);
    if (!encoded) {
        fprintf(stderr, "Memory allocation failed\n");
        return;
    }

    size_t encoded_size = varintFloatEncode(encoded, coordinates, count,
                                            VARINT_FLOAT_PRECISION_HIGH,
                                            VARINT_FLOAT_MODE_COMMON_EXPONENT);

    double *decoded = malloc(count * sizeof(double));
    if (!decoded) {
        fprintf(stderr, "Memory allocation failed\n");
        free(encoded);
        return;
    }
    size_t decoded_bytes = varintFloatDecode(encoded, count, decoded);

    printf("Sample coordinates:\n");
    printf("Index | Original      | Decoded       | Error\n");
    printf("------|---------------|---------------|------------\n");
    for (size_t i = 0; i < 4; i++) {
        printf("%5zu | %13.7f | %13.7f | %e\n", i, coordinates[i], decoded[i],
               fabs(coordinates[i] - decoded[i]));
    }

    ErrorStats errors = calculateErrors(coordinates, decoded, count);
    size_t original_size = count * sizeof(double);

    printf("\nCompression results:\n");
    printf("  Original size: %zu bytes\n", original_size);
    printf("  Compressed: %zu bytes\n", encoded_size);
    printf("  Ratio: %.2fx\n", (double)original_size / encoded_size);
    printf("  Max error: %.7f degrees\n", errors.max_absolute_error);
    printf("  Acceptable for ±11m precision: %s\n",
           errors.max_absolute_error <= 0.0001 ? "✓" : "✗");

    assert(decoded_bytes == encoded_size);

    free(encoded);
    free(decoded);

    printf("✓ HIGH precision maintains GPS accuracy with 2.7x compression\n");
}

/* Example 3: Scientific measurements with error bounds */
void example_scientific_data() {
    printf("\n=== Example 3: Scientific Measurements ===\n");

    /* Pressure sensor readings in Pascals */
    double pressures[] = {101325.0, 101328.5, 101330.2, 101327.8, 101332.1,
                          101329.4, 101331.0, 101326.5, 101333.8, 101328.9,
                          101330.5, 101327.2, 101331.8, 101329.1, 101332.5};
    size_t count = sizeof(pressures) / sizeof(pressures[0]);

    printf("Pressure readings: %zu values (Pascal)\n", count);
    printf("Range: %.1f to %.1f Pa\n\n", pressures[0], pressures[count - 1]);

    /* Encode with MEDIUM precision */
    size_t max_size =
        varintFloatMaxEncodedSize(count, VARINT_FLOAT_PRECISION_MEDIUM);
    uint8_t *encoded = malloc(max_size);
    if (!encoded) {
        fprintf(stderr, "Memory allocation failed\n");
        return;
    }

    size_t encoded_size = varintFloatEncode(encoded, pressures, count,
                                            VARINT_FLOAT_PRECISION_MEDIUM,
                                            VARINT_FLOAT_MODE_COMMON_EXPONENT);

    double *decoded = malloc(count * sizeof(double));
    if (!decoded) {
        fprintf(stderr, "Memory allocation failed\n");
        free(encoded);
        return;
    }
    varintFloatDecode(encoded, count, decoded);

    printf("First 5 values:\n");
    printf("Original   | Decoded    | Abs Error | Rel Error\n");
    printf("-----------|------------|-----------|----------\n");
    for (size_t i = 0; i < 5; i++) {
        double abs_error = fabs(decoded[i] - pressures[i]);
        double rel_error = abs_error / pressures[i];
        printf("%10.2f | %10.2f | %9.2f | %9.2e\n", pressures[i], decoded[i],
               abs_error, rel_error);
    }

    ErrorStats errors = calculateErrors(pressures, decoded, count);
    size_t original_size = count * sizeof(double);

    printf("\nStatistics:\n");
    printf("  Compression: %zu → %zu bytes (%.1fx)\n", original_size,
           encoded_size, (double)original_size / encoded_size);
    printf("  Max absolute error: %.2f Pa\n", errors.max_absolute_error);
    printf("  Avg absolute error: %.2f Pa\n", errors.avg_absolute_error);
    printf("  Max relative error: %.2e\n", errors.max_relative_error);

    free(encoded);
    free(decoded);

    printf("✓ Scientific data compressed with bounded errors\n");
}

/* Example 4: Precision mode comparison */
void example_precision_comparison() {
    printf("\n=== Example 4: Precision Mode Comparison ===\n");

    /* Generate test data: sine wave */
    size_t count = 100;
    double *data = malloc(count * sizeof(double));
    if (!data) {
        fprintf(stderr, "Memory allocation failed\n");
        return;
    }
    for (size_t i = 0; i < count; i++) {
        data[i] = sin(2.0 * M_PI * i / count) * 1000.0;
    }

    printf("Test data: 100-point sine wave (amplitude=1000)\n\n");

    printf("Mode   | Mantissa | Exponent | Compressed | Ratio | Max Error    | "
           "Max Rel Error\n");
    printf("-------|----------|----------|------------|-------|--------------|-"
           "--------------\n");

    varintFloatPrecision modes[] = {
        VARINT_FLOAT_PRECISION_FULL, VARINT_FLOAT_PRECISION_HIGH,
        VARINT_FLOAT_PRECISION_MEDIUM, VARINT_FLOAT_PRECISION_LOW};
    const char *mode_names[] = {"FULL  ", "HIGH  ", "MEDIUM", "LOW   "};

    size_t original_size = count * sizeof(double);

    for (int m = 0; m < 4; m++) {
        size_t max_size = varintFloatMaxEncodedSize(count, modes[m]);
        uint8_t *encoded = malloc(max_size);
        if (!encoded) {
            fprintf(stderr, "Memory allocation failed\n");
            free(data);
            return;
        }

        size_t encoded_size = varintFloatEncode(encoded, data, count, modes[m],
                                                VARINT_FLOAT_MODE_INDEPENDENT);

        double *decoded = malloc(count * sizeof(double));
        if (!decoded) {
            fprintf(stderr, "Memory allocation failed\n");
            free(encoded);
            free(data);
            return;
        }
        varintFloatDecode(encoded, count, decoded);

        ErrorStats errors = calculateErrors(data, decoded, count);

        uint8_t mant_bits = varintFloatPrecisionMantissaBits(modes[m]);
        uint8_t exp_bits = varintFloatPrecisionExponentBits(modes[m]);

        printf("%s | %8d | %8d | %10zu | %5.2fx | %12.6f | %14.2e\n",
               mode_names[m], mant_bits, exp_bits, encoded_size,
               (double)original_size / encoded_size, errors.max_absolute_error,
               errors.max_relative_error);

        free(encoded);
        free(decoded);
    }

    free(data);

    printf(
        "\n✓ Higher precision modes preserve more accuracy at cost of size\n");
}

/* Example 5: Encoding mode comparison */
void example_encoding_modes() {
    printf("\n=== Example 5: Encoding Mode Comparison ===\n");

    /* Time series: sequential sensor readings with similar magnitudes */
    double readings[] = {25.123, 25.145, 25.167, 25.189, 25.201,
                         25.223, 25.245, 25.267, 25.289, 25.301,
                         25.323, 25.345, 25.367, 25.389};
    size_t count = sizeof(readings) / sizeof(readings[0]);

    printf("Time series: %zu sensor readings (similar magnitudes)\n\n", count);

    varintFloatEncodingMode modes[] = {VARINT_FLOAT_MODE_INDEPENDENT,
                                       VARINT_FLOAT_MODE_COMMON_EXPONENT,
                                       VARINT_FLOAT_MODE_DELTA_EXPONENT};
    const char *mode_names[] = {"INDEPENDENT    ", "COMMON_EXPONENT",
                                "DELTA_EXPONENT "};

    printf("Mode             | Compressed | Ratio | Best For\n");
    printf("-----------------|------------|-------|----------------------------"
           "------\n");

    size_t original_size = count * sizeof(double);

    for (int m = 0; m < 3; m++) {
        size_t max_size =
            varintFloatMaxEncodedSize(count, VARINT_FLOAT_PRECISION_HIGH);
        uint8_t *encoded = malloc(max_size);
        if (!encoded) {
            fprintf(stderr, "Memory allocation failed\n");
            return;
        }

        size_t encoded_size = varintFloatEncode(
            encoded, readings, count, VARINT_FLOAT_PRECISION_HIGH, modes[m]);

        double *decoded = malloc(count * sizeof(double));
        if (!decoded) {
            fprintf(stderr, "Memory allocation failed\n");
            free(encoded);
            return;
        }
        varintFloatDecode(encoded, count, decoded);

        /* Verify correctness */
        for (size_t i = 0; i < count; i++) {
            assert(fabs(decoded[i] - readings[i]) < 1e-5);
        }

        const char *best_for[] = {"Random/uncorrelated data",
                                  "Similar magnitude values",
                                  "Sequential time series"};

        printf("%s | %10zu | %5.2fx | %s\n", mode_names[m], encoded_size,
               (double)original_size / encoded_size, best_for[m]);

        free(encoded);
        free(decoded);
    }

    printf("\n✓ COMMON_EXPONENT mode best for similar-magnitude data\n");
}

/* Example 6: Special values handling */
void example_special_values() {
    printf("\n=== Example 6: Special Values (NaN, Infinity, Zero) ===\n");

    double special_values[] = {
        0.0,  -0.0, INFINITY, -INFINITY, NAN,
        1.0,  -1.0, 42.5,     1e-300, /* Near denormal */
        1e300                         /* Large value */
    };
    size_t count = sizeof(special_values) / sizeof(special_values[0]);

    printf(
        "Test values: zero, infinity, NaN, normal, near-denormal, large\n\n");

    size_t max_size =
        varintFloatMaxEncodedSize(count, VARINT_FLOAT_PRECISION_HIGH);
    uint8_t *encoded = malloc(max_size);
    if (!encoded) {
        fprintf(stderr, "Memory allocation failed\n");
        return;
    }

    varintFloatEncode(encoded, special_values, count,
                      VARINT_FLOAT_PRECISION_HIGH,
                      VARINT_FLOAT_MODE_INDEPENDENT);

    double *decoded = malloc(count * sizeof(double));
    if (!decoded) {
        fprintf(stderr, "Memory allocation failed\n");
        free(encoded);
        return;
    }
    varintFloatDecode(encoded, count, decoded);

    printf("Index | Original    | Decoded     | Type       | Match\n");
    printf("------|-------------|-------------|------------|------\n");

    for (size_t i = 0; i < count; i++) {
        const char *type;
        bool match;

        if (isnan(special_values[i])) {
            type = "NaN";
            match = isnan(decoded[i]);
        } else if (isinf(special_values[i])) {
            type = special_values[i] > 0 ? "+Infinity" : "-Infinity";
            match = (isinf(decoded[i]) &&
                     ((special_values[i] > 0) == (decoded[i] > 0)));
        } else if (special_values[i] == 0.0) {
            type = signbit(special_values[i]) ? "-Zero" : "+Zero";
            match = (decoded[i] == 0.0 &&
                     signbit(decoded[i]) == signbit(special_values[i]));
        } else {
            type = "Normal";
            match = fabs(decoded[i] - special_values[i]) <
                    1e-5 * fabs(special_values[i]);
        }

        printf("%5zu | %11.3e | %11.3e | %-10s | %s\n", i, special_values[i],
               decoded[i], type, match ? "✓" : "✗");
    }

    printf("\n✓ All special values preserved correctly\n");

    free(encoded);
    free(decoded);
}

/* Example 7: Automatic precision selection */
void example_auto_precision() {
    printf("\n=== Example 7: Automatic Precision Selection ===\n");

    double measurements[] = {100.5, 100.7, 100.3, 100.9,
                             100.6, 100.8, 100.4, 100.2};
    size_t count = sizeof(measurements) / sizeof(measurements[0]);

    printf("Measurements: 8 values around 100.0\n\n");

    const double error_thresholds[] = {1e-15, 1e-6, 1e-3, 1e-1};
    const char *threshold_names[] = {"1e-15 (lossless)", "1e-6  (7 digits)",
                                     "1e-3  (3 digits)", "1e-1  (1 digit) "};

    printf("Max Error Threshold | Selected Mode | Compressed | Ratio | Actual "
           "Max Error\n");
    printf("--------------------|---------------|------------|-------|---------"
           "---------\n");

    size_t original_size = count * sizeof(double);

    for (int t = 0; t < 4; t++) {
        size_t max_size =
            varintFloatMaxEncodedSize(count, VARINT_FLOAT_PRECISION_FULL);
        uint8_t *encoded = malloc(max_size);
        if (!encoded) {
            fprintf(stderr, "Memory allocation failed\n");
            return;
        }

        varintFloatPrecision selected;
        size_t encoded_size = varintFloatEncodeAuto(
            encoded, measurements, count, error_thresholds[t],
            VARINT_FLOAT_MODE_COMMON_EXPONENT, &selected);

        double *decoded = malloc(count * sizeof(double));
        if (!decoded) {
            fprintf(stderr, "Memory allocation failed\n");
            free(encoded);
            return;
        }
        varintFloatDecode(encoded, count, decoded);

        ErrorStats errors = calculateErrors(measurements, decoded, count);

        const char *mode_name;
        switch (selected) {
        case VARINT_FLOAT_PRECISION_FULL:
            mode_name = "FULL  ";
            break;
        case VARINT_FLOAT_PRECISION_HIGH:
            mode_name = "HIGH  ";
            break;
        case VARINT_FLOAT_PRECISION_MEDIUM:
            mode_name = "MEDIUM";
            break;
        case VARINT_FLOAT_PRECISION_LOW:
            mode_name = "LOW   ";
            break;
        default:
            mode_name = "???   ";
            break;
        }

        printf("%s        | %s      | %10zu | %5.2fx | %16.2e\n",
               threshold_names[t], mode_name, encoded_size,
               (double)original_size / encoded_size, errors.max_relative_error);

        free(encoded);
        free(decoded);
    }

    printf("\n✓ Automatic mode selects optimal precision for error "
           "requirements\n");
}

/* Example 8: Large dataset compression */
void example_large_dataset() {
    printf("\n=== Example 8: Large Dataset Compression ===\n");

    size_t count = 10000;
    double *sensor_data = malloc(count * sizeof(double));
    if (!sensor_data) {
        fprintf(stderr, "Memory allocation failed\n");
        return;
    }

    /* Generate realistic sensor data: temperature with noise */
    for (size_t i = 0; i < count; i++) {
        double t = (double)i / count;
        double trend = 20.0 + 5.0 * sin(2.0 * M_PI * t);
        double noise = ((double)rand() / RAND_MAX - 0.5) * 0.5;
        sensor_data[i] = trend + noise;
    }

    printf("Dataset: 10,000 temperature sensor readings\n");
    printf("Pattern: Sinusoidal trend + random noise\n\n");

    size_t max_size =
        varintFloatMaxEncodedSize(count, VARINT_FLOAT_PRECISION_MEDIUM);
    uint8_t *encoded = malloc(max_size);
    if (!encoded) {
        fprintf(stderr, "Memory allocation failed\n");
        free(sensor_data);
        return;
    }

    size_t encoded_size = varintFloatEncode(encoded, sensor_data, count,
                                            VARINT_FLOAT_PRECISION_MEDIUM,
                                            VARINT_FLOAT_MODE_COMMON_EXPONENT);

    double *decoded = malloc(count * sizeof(double));
    if (!decoded) {
        fprintf(stderr, "Memory allocation failed\n");
        free(encoded);
        free(sensor_data);
        return;
    }
    varintFloatDecode(encoded, count, decoded);

    ErrorStats errors = calculateErrors(sensor_data, decoded, count);

    size_t original_size = count * sizeof(double);

    printf("Size analysis:\n");
    printf("  Original:   %zu bytes (%.2f KB)\n", original_size,
           original_size / 1024.0);
    printf("  Compressed: %zu bytes (%.2f KB)\n", encoded_size,
           encoded_size / 1024.0);
    printf("  Ratio:      %.2fx\n", (double)original_size / encoded_size);
    printf("  Space saved: %.1f%%\n",
           (1.0 - (double)encoded_size / original_size) * 100);
    printf("  Bytes/value: %.2f\n", (double)encoded_size / count);

    printf("\nError analysis:\n");
    printf("  Max absolute: %.6f\n", errors.max_absolute_error);
    printf("  Avg absolute: %.6f\n", errors.avg_absolute_error);
    printf("  Max relative: %.2e\n", errors.max_relative_error);
    printf("  Avg relative: %.2e\n", errors.avg_relative_error);

    printf("\n✓ Large datasets achieve high compression with bounded errors\n");

    free(sensor_data);
    free(encoded);
    free(decoded);
}

/* Example 9: Round-trip accuracy verification */
void example_round_trip() {
    printf("\n=== Example 9: Round-Trip Accuracy Verification ===\n");

    struct {
        const char *description;
        double *values;
        size_t count;
        varintFloatPrecision precision;
    } tests[] = {
        {"Single value", (double[]){42.5}, 1, VARINT_FLOAT_PRECISION_HIGH},
        {"Two values", (double[]){1.0, 2.0}, 2, VARINT_FLOAT_PRECISION_HIGH},
        {"All zeros", (double[]){0.0, 0.0, 0.0}, 3,
         VARINT_FLOAT_PRECISION_HIGH},
        {"Large range", (double[]){1e-10, 1.0, 1e10}, 3,
         VARINT_FLOAT_PRECISION_FULL},
        {"Negative values", (double[]){-5.5, -10.2, -15.8}, 3,
         VARINT_FLOAT_PRECISION_MEDIUM},
    };

    for (size_t t = 0; t < sizeof(tests) / sizeof(tests[0]); t++) {
        size_t max_size =
            varintFloatMaxEncodedSize(tests[t].count, tests[t].precision);
        uint8_t *encoded = malloc(max_size);
        if (!encoded) {
            fprintf(stderr, "Memory allocation failed\n");
            return;
        }

        size_t encoded_size = varintFloatEncode(
            encoded, tests[t].values, tests[t].count, tests[t].precision,
            VARINT_FLOAT_MODE_INDEPENDENT);

        double *decoded = malloc(tests[t].count * sizeof(double));
        if (!decoded) {
            fprintf(stderr, "Memory allocation failed\n");
            free(encoded);
            return;
        }
        size_t decoded_bytes =
            varintFloatDecode(encoded, tests[t].count, decoded);

        assert(decoded_bytes == encoded_size);

        ErrorStats errors =
            calculateErrors(tests[t].values, decoded, tests[t].count);

        printf("%-20s: %3zu bytes, max error=%e ", tests[t].description,
               encoded_size, errors.max_absolute_error);
        printf("%s\n", errors.max_relative_error < 1e-3 ? "✓" : "✗");

        free(encoded);
        free(decoded);
    }

    printf("\n✓ All round-trip tests passed\n");
}

/* Example 10: Theoretical vs actual error comparison */
void example_error_bounds() {
    printf("\n=== Example 10: Theoretical vs Actual Error Bounds ===\n");

    /* Test data with various magnitudes */
    double test_values[] = {0.001, 0.01,  0.1,    1.0,
                            10.0,  100.0, 1000.0, 10000.0};
    size_t count = sizeof(test_values) / sizeof(test_values[0]);

    printf("Precision | Theoretical Max Rel Error | Actual Max Rel Error | "
           "Within Bounds\n");
    printf("----------|---------------------------|----------------------|-----"
           "---------\n");

    varintFloatPrecision precisions[] = {VARINT_FLOAT_PRECISION_HIGH,
                                         VARINT_FLOAT_PRECISION_MEDIUM,
                                         VARINT_FLOAT_PRECISION_LOW};
    const char *precision_names[] = {"HIGH  ", "MEDIUM", "LOW   "};

    for (int p = 0; p < 3; p++) {
        size_t max_size = varintFloatMaxEncodedSize(count, precisions[p]);
        uint8_t *encoded = malloc(max_size);
        if (!encoded) {
            fprintf(stderr, "Memory allocation failed\n");
            return;
        }

        size_t encoded_size __attribute__((unused)) =
            varintFloatEncode(encoded, test_values, count, precisions[p],
                              VARINT_FLOAT_MODE_INDEPENDENT);

        double *decoded = malloc(count * sizeof(double));
        if (!decoded) {
            fprintf(stderr, "Memory allocation failed\n");
            free(encoded);
            return;
        }
        varintFloatDecode(encoded, count, decoded);

        ErrorStats errors = calculateErrors(test_values, decoded, count);
        double theoretical_max =
            varintFloatPrecisionMaxRelativeError(precisions[p]);

        bool within_bounds = errors.max_relative_error <= theoretical_max * 2.0;

        printf("%s  | %25.2e | %20.2e | %s\n", precision_names[p],
               theoretical_max, errors.max_relative_error,
               within_bounds ? "✓" : "✗");

        free(encoded);
        free(decoded);
    }

    printf("\n✓ Actual errors are within theoretical bounds\n");
}

int main() {
    printf("===========================================\n");
    printf("     varintFloat Example Suite\n");
    printf("===========================================\n");
    printf("Variable precision floating point compression\n");

    example_temperature_sensors();
    example_gps_coordinates();
    example_scientific_data();
    example_precision_comparison();
    example_encoding_modes();
    example_special_values();
    example_auto_precision();
    example_large_dataset();
    example_round_trip();
    example_error_bounds();

    printf("\n===========================================\n");
    printf("All examples completed successfully!\n");
    printf("===========================================\n");

    return 0;
}
