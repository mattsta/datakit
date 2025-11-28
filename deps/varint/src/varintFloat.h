#pragma once

#include "varint.h"
#include "varintExternal.h"
#include <math.h>

__BEGIN_DECLS

/* ====================================================================
 * Float varints
 * ==================================================================== */
/* varint model Float Compression:
 *   Type encoded by: precision mode + IEEE 754 component compression
 *   Size: variable based on precision and data characteristics
 *   Layout: [precision:1][exp_bits:1][mant_bits:1][common_exp?][data...]
 *   Meaning: Lossy or lossless compression of floating point arrays
 *   Pros: High compression for scientific/sensor data with known precision
 *         Adjustable precision (FULL/HIGH/MEDIUM/LOW)
 *         Common exponent optimization for similar-magnitude values
 *         Delta-of-exponents for sequential sensor readings
 *   Cons: Lossy compression (except FULL mode)
 *         Sequential decoding for delta modes
 *         Reconstruction error depends on precision mode */

/* Precision modes for floating point compression
 * Each mode trades precision for space efficiency */
typedef enum varintFloatPrecision {
    /* FULL: No precision loss - full IEEE 754 double precision
     * 52-bit mantissa, 11-bit exponent
     * Max error: 0.0 (lossless)
     * Typical use: Critical calculations requiring exact values */
    VARINT_FLOAT_PRECISION_FULL = 0,

    /* HIGH: ~7 decimal digits of precision
     * 23-bit mantissa (IEEE 754 single precision)
     * Max relative error: ~1.2e-7
     * Typical use: GPS coordinates, financial data */
    VARINT_FLOAT_PRECISION_HIGH = 1,

    /* MEDIUM: ~3 decimal digits of precision
     * 10-bit mantissa
     * Max relative error: ~9.8e-4
     * Typical use: Temperature sensors (±0.1°C), pressure readings */
    VARINT_FLOAT_PRECISION_MEDIUM = 2,

    /* LOW: ~1 decimal digit of precision
     * 4-bit mantissa
     * Max relative error: ~6.3e-2
     * Typical use: Coarse approximations, data visualization */
    VARINT_FLOAT_PRECISION_LOW = 3,
} varintFloatPrecision;

/* Get mantissa bits for precision mode */
static inline uint8_t
varintFloatPrecisionMantissaBits(varintFloatPrecision precision) {
    switch (precision) {
    case VARINT_FLOAT_PRECISION_FULL:
        return 52;
    case VARINT_FLOAT_PRECISION_HIGH:
        return 23;
    case VARINT_FLOAT_PRECISION_MEDIUM:
        return 10;
    case VARINT_FLOAT_PRECISION_LOW:
        return 4;
    default:
        return 52;
    }
}

/* Get exponent bits for precision mode */
static inline uint8_t
varintFloatPrecisionExponentBits(varintFloatPrecision precision) {
    switch (precision) {
    case VARINT_FLOAT_PRECISION_FULL:
        return 11;
    case VARINT_FLOAT_PRECISION_HIGH:
        return 8;
    case VARINT_FLOAT_PRECISION_MEDIUM:
        return 8;
    case VARINT_FLOAT_PRECISION_LOW:
        return 5;
    default:
        return 11;
    }
}

/* Calculate maximum absolute error for a precision mode
 * This is the maximum error introduced by mantissa truncation
 * relative_error = 2^(-mantissa_bits) */
static inline double
varintFloatPrecisionMaxRelativeError(varintFloatPrecision precision) {
    uint8_t mantissa_bits = varintFloatPrecisionMantissaBits(precision);
    return ldexp(1.0, -(int)mantissa_bits);
}

/* Encoding options for floating point compression */
typedef enum varintFloatEncodingMode {
    /* INDEPENDENT: Each float encoded independently
     * No assumptions about data relationship
     * Format: [signs][exponents][mantissas] */
    VARINT_FLOAT_MODE_INDEPENDENT = 0,

    /* COMMON_EXPONENT: All values share similar magnitude
     * Store base exponent + small deltas
     * Best for: sensor readings of same physical quantity
     * Format: [signs][base_exp][exp_deltas][mantissas] */
    VARINT_FLOAT_MODE_COMMON_EXPONENT = 1,

    /* DELTA_EXPONENT: Sequential exponents (time series)
     * Store first exponent + deltas
     * Best for: slowly varying sensor data
     * Format: [signs][first_exp][exp_deltas][mantissas] */
    VARINT_FLOAT_MODE_DELTA_EXPONENT = 2,
} varintFloatEncodingMode;

/* Float compression metadata structure
 * Fields ordered by size (8-byte → 4-byte → 1-byte) to eliminate padding */
typedef struct varintFloatMeta {
    size_t count;            /* Number of values */
    size_t encodedSize;      /* Total encoded size in bytes */
    size_t specialCount;     /* Number of special values (NaN/Inf/zero) */
    double maxRelativeError; /* Maximum relative error for this precision */
    varintFloatPrecision precision; /* Precision mode used */
    varintFloatEncodingMode mode;   /* Encoding mode used */
    uint8_t exponentBits;           /* Bits per exponent */
    uint8_t mantissaBits;           /* Bits per mantissa */
} varintFloatMeta;

/* Compile-time size guarantees to prevent regressions */
_Static_assert(sizeof(varintFloatMeta) == 48,
               "varintFloatMeta size changed! Expected 48 bytes (4×8-byte + "
               "2×4-byte + 2×1-byte + 6 padding). "
               "87.5% efficient - padding after uint8_t fields is acceptable.");
_Static_assert(sizeof(varintFloatMeta) <= 64,
               "varintFloatMeta exceeds single cache line (64 bytes)! "
               "Keep float metadata cache-friendly.");

/* Encode array of doubles with specified precision
 * output: buffer to write encoded data (caller must allocate)
 * values: array of double values to encode
 * count: number of values in array
 * precision: precision mode (FULL/HIGH/MEDIUM/LOW)
 * mode: encoding mode (INDEPENDENT/COMMON_EXPONENT/DELTA_EXPONENT)
 * Returns: total bytes written
 *
 * Format: [precision:1][exp_bits:1][mant_bits:1][mode:1][data...]
 * Maximum output size: varintFloatMaxEncodedSize(count, precision) */
size_t varintFloatEncode(uint8_t *output, const double *values,
                         const size_t count,
                         const varintFloatPrecision precision,
                         const varintFloatEncodingMode mode);

/* Decode floating point array
 * input: encoded buffer
 * count: number of values to decode
 * output: buffer to write decoded doubles (caller must allocate count *
 * sizeof(double)) Returns: total bytes read from input
 *
 * Note: Precision information is stored in the encoded data */
size_t varintFloatDecode(const uint8_t *input, const size_t count,
                         double *output);

/* Encode with automatic precision selection based on data range
 * Analyzes the data and selects optimal precision mode
 * output: buffer to write encoded data (caller must allocate)
 * values: array of double values to encode
 * count: number of values in array
 * max_relative_error: maximum acceptable relative error (e.g., 1e-6)
 * mode: encoding mode (INDEPENDENT/COMMON_EXPONENT/DELTA_EXPONENT)
 * selected_precision: output parameter for selected precision mode
 * Returns: total bytes written */
size_t varintFloatEncodeAuto(uint8_t *output, const double *values,
                             const size_t count,
                             const double max_relative_error,
                             const varintFloatEncodingMode mode,
                             varintFloatPrecision *selected_precision);

/* Calculate maximum output size needed for encoding
 * Useful for pre-allocating output buffer */
static inline size_t varintFloatMaxEncodedSize(size_t count,
                                               varintFloatPrecision precision) {
    if (count == 0) {
        return 0;
    }

    /* Header: 4 bytes (precision, exp_bits, mant_bits, mode) */
    size_t header = 4;

    /* Signs: packed in bits, ceil(count/8) bytes */
    size_t signs = (count + 7) / 8;

    /* Exponents: worst case 2 bytes each (1 byte width + up to 8 bytes value,
     * but typically ~2) */
    size_t exponents = count * 9;

    /* Mantissas: depends on precision */
    uint8_t mant_bits = varintFloatPrecisionMantissaBits(precision);
    size_t mantissas = ((size_t)mant_bits * count + 7) / 8;

    /* Special values bitmap: ceil(count/8) bytes */
    size_t special_bitmap = (count + 7) / 8;

    /* Special values storage: worst case all values are special (8 bytes each)
     */
    size_t special_values = count * 8;

    return header + signs + exponents + mantissas + special_bitmap +
           special_values;
}

/* Calculate compression ratio achieved
 * encoded_size: size of encoded data in bytes
 * count: number of values
 * Returns: compression ratio (original_size / encoded_size) */
static inline double varintFloatCompressionRatio(size_t encoded_size,
                                                 size_t count) {
    if (encoded_size == 0) {
        return 0.0;
    }
    size_t original_size = count * sizeof(double);
    return (double)original_size / (double)encoded_size;
}

/* Calculate maximum absolute error for a given value and precision
 * Returns the maximum reconstruction error */
static inline double
varintFloatMaxAbsoluteError(double value, varintFloatPrecision precision) {
    double rel_error = varintFloatPrecisionMaxRelativeError(precision);
    return fabs(value) * rel_error;
}

/* IEEE 754 double decomposition helper
 * Extracts sign, exponent, and mantissa from a double value
 * Returns true if value is normal, false if special (NaN, Inf, denormal, zero)
 */
bool varintFloatDecompose(double value, uint64_t *sign, int16_t *exponent,
                          uint64_t *mantissa);

/* IEEE 754 double composition helper
 * Reconstructs a double from sign, exponent, and mantissa components */
double varintFloatCompose(uint64_t sign, int16_t exponent, uint64_t mantissa);

/* Check if value is special (NaN, Infinity, denormal, or zero) */
static inline bool varintFloatIsSpecial(double value) {
    return !isfinite(value) || value == 0.0 ||
           fabs(value) < 2.2250738585072014e-308;
}

/* Extract metadata from encoded buffer
 * Reads the header information without decoding the entire array */
void varintFloatReadMeta(const uint8_t *input, varintFloatMeta *meta);

/* Analyze data and fill metadata structure
 * Useful for estimating compression ratio before encoding */
void varintFloatAnalyze(const double *values, size_t count,
                        varintFloatPrecision precision,
                        varintFloatEncodingMode mode, varintFloatMeta *meta);

__END_DECLS
