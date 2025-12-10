#include "varintFloat.h"
#include "varintDelta.h"
#include <float.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Check for overflow in size_t multiplication */
static inline bool size_mul_overflow(size_t a, size_t b, size_t *result) {
    if (a == 0 || b == 0) {
        *result = 0;
        return false;
    }
    *result = a * b;
    return (*result / a) != b;
}

/* IEEE 754 double precision format:
 * Sign: 1 bit (bit 63)
 * Exponent: 11 bits (bits 62-52), biased by 1023
 * Mantissa: 52 bits (bits 51-0), implicit leading 1
 */

#define IEEE754_DOUBLE_EXPONENT_BIAS 1023

/* Decompose IEEE 754 double into components */
bool varintFloatDecompose(double value, uint64_t *sign, int16_t *exponent,
                          uint64_t *mantissa) {
    /* Use union to access bit representation */
    union {
        double d;
        uint64_t u;
    } bits;
    bits.d = value;

    /* Extract sign bit (bit 63) */
    *sign = (bits.u >> 63) & 1;

    /* Extract exponent (bits 62-52) */
    uint64_t exp_bits = (bits.u >> 52) & 0x7FF;

    /* Extract mantissa (bits 51-0) */
    *mantissa = bits.u & 0xFFFFFFFFFFFFFULL;

    /* Check for special values */
    if (exp_bits == 0x7FF) {
        /* NaN or Infinity */
        *exponent = 0x7FF;
        return false;
    }

    if (exp_bits == 0) {
        if (*mantissa == 0) {
            /* Zero */
            *exponent = 0;
            return false;
        } else {
            /* Denormal number */
            *exponent = 1 - IEEE754_DOUBLE_EXPONENT_BIAS;
            return false;
        }
    }

    /* Normal number: remove bias and add implicit leading 1 to mantissa */
    *exponent = (int16_t)((int)exp_bits - IEEE754_DOUBLE_EXPONENT_BIAS);
    *mantissa |= (1ULL << 52); /* Add implicit leading 1 */

    return true;
}

/* Compose IEEE 754 double from components */
double varintFloatCompose(uint64_t sign, int16_t exponent, uint64_t mantissa) {
    union {
        double d;
        uint64_t u;
    } bits;

    /* Handle special cases */
    if (exponent == 0 && mantissa == 0) {
        /* Zero */
        bits.u = sign << 63;
        return bits.d;
    }

    /* Add bias back to exponent */
    int biased_exp = exponent + IEEE754_DOUBLE_EXPONENT_BIAS;

    /* Clamp exponent to valid range */
    if (biased_exp <= 0) {
        /* Underflow to zero */
        bits.u = sign << 63;
        return bits.d;
    }
    if (biased_exp >= 0x7FF) {
        /* Overflow to infinity */
        bits.u = (sign << 63) | (0x7FFULL << 52);
        return bits.d;
    }

    /* Remove implicit leading 1 from mantissa */
    mantissa &= 0xFFFFFFFFFFFFFULL;

    /* Assemble IEEE 754 format */
    bits.u = (sign << 63) | ((uint64_t)biased_exp << 52) | mantissa;

    return bits.d;
}

/* Truncate mantissa to specified number of bits with rounding */
static inline uint64_t truncateMantissa(const uint64_t mantissa,
                                        const uint8_t from_bits,
                                        const uint8_t to_bits) {
    if (to_bits >= from_bits) {
        return mantissa;
    }

    /* Number of bits to remove */
    const uint8_t shift = from_bits - to_bits;

    /* Round to nearest: add 0.5 in the LSB of the result */
    const uint64_t rounding = 1ULL << (shift - 1);
    const uint64_t rounded = mantissa + rounding;

    /* Shift down */
    return rounded >> shift;
}

/* Expand mantissa from reduced precision back to full precision */
static inline uint64_t expandMantissa(const uint64_t mantissa,
                                      const uint8_t from_bits,
                                      const uint8_t to_bits) {
    if (from_bits >= to_bits) {
        return mantissa;
    }

    /* Shift up */
    const uint8_t shift = to_bits - from_bits;
    return mantissa << shift;
}

/* Pack bits into byte array */
static void packBits(const uint64_t *values, const size_t count,
                     const uint8_t bits_per_value, uint8_t *output) {
    size_t bit_offset = 0;
    memset(output, 0, (count * bits_per_value + 7) / 8);

    for (size_t i = 0; i < count; i++) {
        const uint64_t value = values[i];
        const size_t byte_offset = bit_offset / 8;
        const size_t bit_in_byte = bit_offset % 8;

        /* Write value bit by bit */
        for (uint8_t bit = 0; bit < bits_per_value; bit++) {
            if (value & (1ULL << bit)) {
                output[byte_offset + (bit + bit_in_byte) / 8] |=
                    (1 << ((bit + bit_in_byte) % 8));
            }
        }

        bit_offset += bits_per_value;
    }
}

/* Unpack bits from byte array */
static void unpackBits(const uint8_t *input, const size_t count,
                       const uint8_t bits_per_value, uint64_t *output) {
    size_t bit_offset = 0;

    for (size_t i = 0; i < count; i++) {
        uint64_t value = 0;
        const size_t byte_offset = bit_offset / 8;
        const size_t bit_in_byte = bit_offset % 8;

        /* Read value bit by bit */
        for (uint8_t bit = 0; bit < bits_per_value; bit++) {
            const size_t src_byte = byte_offset + (bit + bit_in_byte) / 8;
            const size_t src_bit = (bit + bit_in_byte) % 8;
            if (input[src_byte] & (1 << src_bit)) {
                value |= (1ULL << bit);
            }
        }

        output[i] = value;
        bit_offset += bits_per_value;
    }
}

/* Encode floating point array */
size_t varintFloatEncode(uint8_t *output, const double *values,
                         const size_t count,
                         const varintFloatPrecision precision,
                         const varintFloatEncodingMode mode) {
    if (count == 0) {
        return 0;
    }

    uint8_t *p = output;

    /* Write header */
    *p++ = (uint8_t)precision;
    const uint8_t exp_bits = varintFloatPrecisionExponentBits(precision);
    const uint8_t mant_bits = varintFloatPrecisionMantissaBits(precision);
    *p++ = exp_bits;
    *p++ = mant_bits;
    *p++ = (uint8_t)mode;

    /* Check for integer overflow in allocation sizes */
    size_t allocSize1, allocSize2, allocSize3;
    if (size_mul_overflow(count, sizeof(uint64_t), &allocSize1) ||
        size_mul_overflow(count, sizeof(int16_t), &allocSize2) ||
        size_mul_overflow(count, sizeof(uint64_t), &allocSize3)) {
        return 0; /* Integer overflow */
    }

    /* Allocate temporary arrays for components */
    uint64_t *signs = malloc(allocSize1);
    int16_t *exponents = malloc(allocSize2);
    uint64_t *mantissas = malloc(allocSize1);
    uint64_t *special_flags = malloc(allocSize1);

    if (!signs || !exponents || !mantissas || !special_flags) {
        free(signs);
        free(exponents);
        free(mantissas);
        free(special_flags);
        return 0;
    }

    /* Decompose all values */
    for (size_t i = 0; i < count; i++) {
        const bool is_normal = varintFloatDecompose(
            values[i], &signs[i], &exponents[i], &mantissas[i]);
        special_flags[i] = is_normal ? 0 : 1;

        /* Truncate mantissa to desired precision
         * Note: decompose returns 53-bit mantissa (52 + implicit 1) */
        if (is_normal) {
            if (mant_bits == 52) {
                /* FULL precision: remove implicit 1 (bit 52) to get 52-bit
                 * field */
                mantissas[i] &= 0xFFFFFFFFFFFFFULL; /* Keep bits 51-0 */
            } else {
                /* Reduced precision: truncate from 53 bits to target */
                mantissas[i] = truncateMantissa(mantissas[i], 53, mant_bits);
            }
        }
    }

    /* Write special values bitmap */
    const size_t special_bitmap_size = (count + 7) / 8;
    packBits(special_flags, count, 1, p);
    p += special_bitmap_size;

    /* Write signs bitmap */
    packBits(signs, count, 1, p);
    p += (count + 7) / 8;

    /* Write exponents based on mode */
    if (mode == VARINT_FLOAT_MODE_INDEPENDENT) {
        /* Each exponent independently */
        for (size_t i = 0; i < count; i++) {
            if (!special_flags[i]) {
                /* Encode exponent as signed value */
                varintWidth width;
                uint64_t zigzag = varintDeltaZigZag(exponents[i]);
                varintExternalUnsignedEncoding(zigzag, width);
                *p++ = (uint8_t)width;
                varintExternalPutFixedWidth(p, zigzag, width);
                p += width;
            }
        }
    } else if (mode == VARINT_FLOAT_MODE_COMMON_EXPONENT) {
        /* Find min/max exponents for non-special values */
        int16_t min_exp = INT16_MAX;
        int16_t max_exp = INT16_MIN;
        size_t normal_exp_count = 0;
        for (size_t i = 0; i < count; i++) {
            if (!special_flags[i]) {
                if (exponents[i] < min_exp) {
                    min_exp = exponents[i];
                }
                if (exponents[i] > max_exp) {
                    max_exp = exponents[i];
                }
                normal_exp_count++;
            }
        }

        /* Write base exponent (min) only if there are normal values */
        if (normal_exp_count > 0) {
            const uint64_t zigzag = varintDeltaZigZag(min_exp);
            varintWidth width;
            varintExternalUnsignedEncoding(zigzag, width);
            *p++ = (uint8_t)width;
            varintExternalPutFixedWidth(p, zigzag, width);
            p += width;

            /* Write exponent deltas */
            for (size_t i = 0; i < count; i++) {
                if (!special_flags[i]) {
                    const uint8_t delta = (uint8_t)(exponents[i] - min_exp);
                    *p++ = delta;
                }
            }
        }
    } else { /* DELTA_EXPONENT */
        /* Write first exponent */
        size_t first_normal = 0;
        while (first_normal < count && special_flags[first_normal]) {
            first_normal++;
        }

        if (first_normal < count) {
            const uint64_t zigzag = varintDeltaZigZag(exponents[first_normal]);
            varintWidth width;
            varintExternalUnsignedEncoding(zigzag, width);
            *p++ = (uint8_t)width;
            varintExternalPutFixedWidth(p, zigzag, width);
            p += width;

            /* Write exponent deltas */
            int16_t prev_exp = exponents[first_normal];
            for (size_t i = first_normal + 1; i < count; i++) {
                if (!special_flags[i]) {
                    const int16_t delta = exponents[i] - prev_exp;
                    const uint64_t delta_zigzag = varintDeltaZigZag(delta);
                    varintWidth dwidth;
                    varintExternalUnsignedEncoding(delta_zigzag, dwidth);
                    *p++ = (uint8_t)dwidth;
                    varintExternalPutFixedWidth(p, delta_zigzag, dwidth);
                    p += dwidth;
                    prev_exp = exponents[i];
                }
            }
        }
    }

    /* Write mantissas (packed bits) */
    size_t normal_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (!special_flags[i]) {
            mantissas[normal_count++] = mantissas[i];
        }
    }
    if (normal_count > 0) {
        const size_t mantissa_bytes = (normal_count * mant_bits + 7) / 8;
        packBits(mantissas, normal_count, mant_bits, p);
        p += mantissa_bytes;
    }

    /* Write special values (for NaN, Inf, etc.) */
    for (size_t i = 0; i < count; i++) {
        if (special_flags[i]) {
            /* Store the full 64-bit representation */
            union {
                double d;
                uint64_t u;
            } bits;
            bits.d = values[i];
            memcpy(p, &bits.u, sizeof(uint64_t));
            p += sizeof(uint64_t);
        }
    }

    free(signs);
    free(exponents);
    free(mantissas);
    free(special_flags);

    return (size_t)(p - output);
}

/* Decode floating point array */
size_t varintFloatDecode(const uint8_t *input, const size_t count,
                         double *output) {
    if (count == 0) {
        return 0;
    }

    const uint8_t *p = input;

    /* Read header */
    const varintFloatPrecision precision = (varintFloatPrecision)(*p++);
    const uint8_t exp_bits = *p++;
    const uint8_t mant_bits = *p++;
    (void)precision; /* Precision info embedded in exp/mant_bits */
    (void)exp_bits;  /* Bits info used implicitly in unpacking */
    const varintFloatEncodingMode mode = (varintFloatEncodingMode)(*p++);

    /* Check for integer overflow in allocation sizes */
    size_t allocSize1, allocSize2;
    if (size_mul_overflow(count, sizeof(uint64_t), &allocSize1) ||
        size_mul_overflow(count, sizeof(int16_t), &allocSize2)) {
        return 0; /* Integer overflow */
    }

    /* Allocate temporary arrays */
    uint64_t *signs = malloc(allocSize1);
    int16_t *exponents = malloc(allocSize2);
    uint64_t *mantissas = malloc(allocSize1);
    uint64_t *special_flags = malloc(allocSize1);

    if (!signs || !exponents || !mantissas || !special_flags) {
        free(signs);
        free(exponents);
        free(mantissas);
        free(special_flags);
        return 0;
    }

    /* Read special values bitmap */
    const size_t special_bitmap_size = (count + 7) / 8;
    unpackBits(p, count, 1, special_flags);
    p += special_bitmap_size;

    /* Read signs bitmap */
    unpackBits(p, count, 1, signs);
    p += (count + 7) / 8;

    /* Read exponents based on mode */
    if (mode == VARINT_FLOAT_MODE_INDEPENDENT) {
        for (size_t i = 0; i < count; i++) {
            if (!special_flags[i]) {
                varintWidth width = (varintWidth)(*p++);
                uint64_t zigzag = varintExternalGet(p, width);
                exponents[i] = (int16_t)varintDeltaZigZagDecode(zigzag);
                p += width;
            }
        }
    } else if (mode == VARINT_FLOAT_MODE_COMMON_EXPONENT) {
        /* Check if there are any normal values */
        size_t normal_exp_count = 0;
        for (size_t i = 0; i < count; i++) {
            if (!special_flags[i]) {
                normal_exp_count++;
            }
        }

        /* Read base exponent only if there are normal values */
        if (normal_exp_count > 0) {
            const varintWidth width = (varintWidth)(*p++);
            const uint64_t zigzag = varintExternalGet(p, width);
            const int16_t base_exp = (int16_t)varintDeltaZigZagDecode(zigzag);
            p += width;

            /* Read exponent deltas */
            for (size_t i = 0; i < count; i++) {
                if (!special_flags[i]) {
                    const uint8_t delta = *p++;
                    exponents[i] = base_exp + delta;
                }
            }
        }
    } else { /* DELTA_EXPONENT */
        /* Find first normal value */
        size_t first_normal = 0;
        while (first_normal < count && special_flags[first_normal]) {
            first_normal++;
        }

        if (first_normal < count) {
            /* Read first exponent */
            const varintWidth width = (varintWidth)(*p++);
            const uint64_t zigzag = varintExternalGet(p, width);
            exponents[first_normal] = (int16_t)varintDeltaZigZagDecode(zigzag);
            p += width;

            /* Read exponent deltas */
            int16_t prev_exp = exponents[first_normal];
            for (size_t i = first_normal + 1; i < count; i++) {
                if (!special_flags[i]) {
                    const varintWidth dwidth = (varintWidth)(*p++);
                    const uint64_t delta_zigzag = varintExternalGet(p, dwidth);
                    p += dwidth; // IMPORTANT: advance pointer after reading
                                 // value
                    const int16_t delta =
                        (int16_t)varintDeltaZigZagDecode(delta_zigzag);
                    exponents[i] = prev_exp + delta;
                    prev_exp = exponents[i];
                }
            }
        }
    }

    /* Read mantissas */
    size_t normal_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (!special_flags[i]) {
            normal_count++;
        }
    }

    if (normal_count > 0) {
        /* Check for overflow in allocation size */
        size_t mantAllocSize;
        if (size_mul_overflow(normal_count, sizeof(uint64_t), &mantAllocSize)) {
            free(signs);
            free(exponents);
            free(mantissas);
            free(special_flags);
            return 0; /* Integer overflow */
        }

        uint64_t *packed_mantissas = malloc(mantAllocSize);
        if (!packed_mantissas) {
            free(signs);
            free(exponents);
            free(mantissas);
            free(special_flags);
            return 0;
        }

        unpackBits(p, normal_count, mant_bits, packed_mantissas);
        p += (normal_count * mant_bits + 7) / 8;

        /* Expand mantissas back to full precision
         * For FULL precision (52-bit): add implicit 1 back
         * For reduced precision: expand to 53 bits (52 + implicit 1) */
        size_t mant_idx = 0;
        for (size_t i = 0; i < count; i++) {
            if (!special_flags[i]) {
                if (mant_bits == 52) {
                    /* FULL precision: add implicit 1 (bit 52) back */
                    mantissas[i] = packed_mantissas[mant_idx++] | (1ULL << 52);
                } else {
                    /* Reduced precision: expand to 53 bits */
                    mantissas[i] = expandMantissa(packed_mantissas[mant_idx++],
                                                  mant_bits, 53);
                }
            }
        }

        free(packed_mantissas);
    }

    /* Read special values */
    for (size_t i = 0; i < count; i++) {
        if (special_flags[i]) {
            union {
                double d;
                uint64_t u;
            } bits;
            memcpy(&bits.u, p, sizeof(uint64_t));
            output[i] = bits.d;
            p += sizeof(uint64_t);
        }
    }

    /* Reconstruct normal values */
    for (size_t i = 0; i < count; i++) {
        if (!special_flags[i]) {
            output[i] =
                varintFloatCompose(signs[i], exponents[i], mantissas[i]);
        }
    }

    free(signs);
    free(exponents);
    free(mantissas);
    free(special_flags);

    return (size_t)(p - input);
}

/* Encode with automatic precision selection */
size_t varintFloatEncodeAuto(uint8_t *output, const double *values,
                             const size_t count,
                             const double max_relative_error,
                             const varintFloatEncodingMode mode,
                             varintFloatPrecision *selected_precision) {
    /* Select precision based on maximum allowable relative error
     * Thresholds based on mantissa bit counts with safety margins:
     * FULL:   52-bit → 2^-52 ≈ 2e-16 (lossless)
     * HIGH:   23-bit → 2^-23 ≈ 1.2e-7 (use for < 5e-4)
     * MEDIUM: 10-bit → 2^-10 ≈ 9.8e-4 (use for < 3e-2)
     * LOW:     4-bit → 2^-4  ≈ 6.3e-2 (use for >= 3e-2) */
    varintFloatPrecision precision = VARINT_FLOAT_PRECISION_LOW;

    if (max_relative_error < 1e-10) {
        precision = VARINT_FLOAT_PRECISION_FULL;
    } else if (max_relative_error < 5e-4) { /* 0.05% threshold */
        precision = VARINT_FLOAT_PRECISION_HIGH;
    } else if (max_relative_error < 0.03) { /* 3% threshold */
        precision = VARINT_FLOAT_PRECISION_MEDIUM;
    } else {
        precision = VARINT_FLOAT_PRECISION_LOW;
    }

    if (selected_precision) {
        *selected_precision = precision;
    }

    return varintFloatEncode(output, values, count, precision, mode);
}
