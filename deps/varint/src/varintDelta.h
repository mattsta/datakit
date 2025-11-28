#pragma once

#include "varint.h"
#include "varintExternal.h"

__BEGIN_DECLS

/* ====================================================================
 * Delta varints
 * ==================================================================== */
/* varint model Delta Encoding:
 *   Type encoded by: base value + ZigZag-encoded deltas
 *   Size: variable per delta (1-8 bytes each)
 *   Layout: [base_width][base_value][delta1_width][delta1]...
 *   Meaning: First value is base, subsequent values are deltas from previous
 *   Pros: Extremely efficient for sorted/sequential data (timestamps, IDs)
 *         Negative deltas supported via ZigZag encoding
 *         70-90% compression for typical sorted arrays
 *   Cons: Sequential encoding/decoding required for random access
 *         Best for monotonic or near-monotonic sequences */

/* ZigZag encoding: Maps signed integers to unsigned for efficient varint
 * storage Mapping: 0→0, -1→1, 1→2, -2→3, 2→4, -3→5, ... This ensures small
 * magnitude values (positive or negative) use fewer bytes */
static inline uint64_t varintDeltaZigZag(int64_t n) {
    /* Portable ZigZag: (n << 1) XOR with sign mask
     * For n >= 0: mask = 0, result = n << 1
     * For n < 0:  mask = -1 (all bits set), result = (n << 1) ^ -1 = ~(n << 1)
     * This avoids implementation-defined signed right shift behavior */
    uint64_t sign_mask = (uint64_t)(n < 0 ? -1 : 0);
    return ((uint64_t)n << 1) ^ sign_mask;
}

/* ZigZag decoding: Restores signed integer from unsigned ZigZag value */
static inline int64_t varintDeltaZigZagDecode(uint64_t zigzag) {
    /* If LSB is 0: positive number, just right shift
     * If LSB is 1: negative number, right shift and negate */
    return (int64_t)((zigzag >> 1) ^ (uint64_t)(-(int64_t)(zigzag & 1)));
}

/* Encode a single delta value into buffer
 * Returns number of bytes written (1 + varint bytes)
 * Format: [width_byte][delta_bytes...] */
varintWidth varintDeltaPut(uint8_t *p, const int64_t delta);

/* Decode a single delta value from buffer
 * Returns number of bytes read (1 + varint bytes)
 * Sets *pDelta to the decoded signed delta value */
varintWidth varintDeltaGet(const uint8_t *p, int64_t *pDelta);

/* Encode array of absolute values as base + deltas
 * output: buffer to write encoded data (caller must allocate sufficient space)
 * values: array of absolute values (sorted or sequential recommended)
 * count: number of values in array
 * Returns: total bytes written
 *
 * Format:
 * [base_width][base_value][delta1_width][delta1][delta2_width][delta2]...
 *
 * Maximum output size: 1 + 8 + (count-1) * 9 bytes
 * (1 byte base width, 8 bytes base value, up to 9 bytes per delta) */
size_t varintDeltaEncode(uint8_t *output, const int64_t *values, size_t count);

/* Decode delta-encoded array back to absolute values
 * input: delta-encoded buffer
 * count: number of values to decode
 * output: buffer to write absolute values (caller must allocate count *
 * sizeof(int64_t)) Returns: total bytes read from input
 *
 * Note: Decoding is sequential - each value depends on previous values */
size_t varintDeltaDecode(const uint8_t *input, size_t count, int64_t *output);

/* Calculate maximum output size needed for encoding count values
 * Useful for pre-allocating output buffer */
static inline size_t varintDeltaMaxEncodedSize(size_t count) {
    if (count == 0) {
        return 0;
    }
    /* Base: 1 byte width + 8 bytes value
     * Deltas: (count-1) * (1 byte width + 8 bytes value) */
    return 1 + 8 + (count - 1) * 9;
}

/* Encode array of unsigned absolute values as base + deltas
 * output: buffer to write encoded data
 * values: array of unsigned absolute values (sorted or sequential recommended)
 * count: number of values in array
 * Returns: total bytes written
 * Note: Deltas are still ZigZag encoded to handle value decreases */
size_t varintDeltaEncodeUnsigned(uint8_t *output, const uint64_t *values,
                                 size_t count);

/* Decode delta-encoded array back to unsigned absolute values
 * input: delta-encoded buffer
 * count: number of values to decode
 * output: buffer to write absolute values
 * Returns: total bytes read from input */
size_t varintDeltaDecodeUnsigned(const uint8_t *input, size_t count,
                                 uint64_t *output);

__END_DECLS
