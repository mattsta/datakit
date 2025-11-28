#pragma once

#include "varint.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

__BEGIN_DECLS

/* ====================================================================
 * BP128 - Block-Packed 128 SIMD Encoding
 * ==================================================================== */
/* BP128 (Binary Packing 128) encodes integers in blocks of 128 values.
 * Each block stores values using the minimum bit-width needed.
 *
 * Structure:
 *   [1 byte: bit-width] [packed data: 128 * bitWidth bits]
 *
 * Features:
 * - Optimized for sorted/nearly-sorted integer sequences
 * - SIMD-accelerated encoding and decoding (AVX2/SSE/NEON)
 * - Excellent compression for clustered or sequential IDs
 * - Block-based random access (O(1) to any block)
 *
 * For sorted data, use BP128Delta which stores deltas instead:
 * - Stores first value + 127 deltas from previous
 * - Much better compression for sorted sequences
 * - Typical use: document IDs, timestamps, sorted lists
 */

/* Block size must be 128 for SIMD efficiency */
#define VARINT_BP128_BLOCK_SIZE 128

/* Maximum bytes per block: 1 byte header + 128 * 8 bytes data */
#define VARINT_BP128_MAX_BLOCK_BYTES (1 + 128 * 8)

/* Metadata for encoded data */
typedef struct varintBP128Meta {
    size_t count;         /* Total values encoded */
    size_t blockCount;    /* Number of 128-value blocks */
    size_t encodedBytes;  /* Total bytes in encoded output */
    size_t lastBlockSize; /* Values in last (partial) block */
    uint8_t maxBitWidth;  /* Maximum bit-width across blocks */
} varintBP128Meta;

/* ====================================================================
 * BP128 Encoding/Decoding (Raw Values)
 * ==================================================================== */

/* Calculate maximum encoded size for count values */
static inline size_t varintBP128MaxBytes(size_t count) {
    size_t fullBlocks = count / VARINT_BP128_BLOCK_SIZE;
    size_t remainder = count % VARINT_BP128_BLOCK_SIZE;
    /* Each full block: 1 byte header + up to 128*8 bytes data */
    /* Partial block: 1 byte header + 1 byte count + up to remainder*8 bytes */
    size_t bytes = fullBlocks * VARINT_BP128_MAX_BLOCK_BYTES;
    if (remainder > 0) {
        bytes += 2 + remainder * 8; /* header + count + data */
    }
    return bytes;
}

/* Encode array of uint32_t values in BP128 format
 * Returns bytes written, fills meta if provided */
size_t varintBP128Encode32(uint8_t *dst, const uint32_t *values, size_t count,
                           varintBP128Meta *meta);

/* Decode BP128 data to uint32_t array
 * Returns number of values decoded */
size_t varintBP128Decode32(const uint8_t *src, uint32_t *values,
                           size_t maxCount);

/* Encode array of uint64_t values in BP128 format */
size_t varintBP128Encode64(uint8_t *dst, const uint64_t *values, size_t count,
                           varintBP128Meta *meta);

/* Decode BP128 data to uint64_t array */
size_t varintBP128Decode64(const uint8_t *src, uint64_t *values,
                           size_t maxCount);

/* ====================================================================
 * BP128Delta Encoding/Decoding (For Sorted Sequences)
 * ==================================================================== */

/* BP128Delta stores deltas instead of raw values.
 * Block format: [bitWidth] [firstValue as varint] [127 packed deltas]
 * This achieves much better compression for sorted data. */

/* Encode sorted uint32_t values using delta encoding
 * Values MUST be in ascending order */
size_t varintBP128DeltaEncode32(uint8_t *dst, const uint32_t *values,
                                size_t count, varintBP128Meta *meta);

/* Decode BP128Delta data to uint32_t array */
size_t varintBP128DeltaDecode32(const uint8_t *src, uint32_t *values,
                                size_t maxCount);

/* Encode sorted uint64_t values using delta encoding */
size_t varintBP128DeltaEncode64(uint8_t *dst, const uint64_t *values,
                                size_t count, varintBP128Meta *meta);

/* Decode BP128Delta data to uint64_t array */
size_t varintBP128DeltaDecode64(const uint8_t *src, uint64_t *values,
                                size_t maxCount);

/* ====================================================================
 * Block-level Operations
 * ==================================================================== */

/* Encode a single block of 128 uint32_t values
 * Returns bytes written (including header) */
size_t varintBP128EncodeBlock32(uint8_t *dst, const uint32_t *values);

/* Decode a single block of 128 uint32_t values
 * Returns bytes consumed */
size_t varintBP128DecodeBlock32(const uint8_t *src, uint32_t *values);

/* Encode a single block of 128 uint32_t deltas
 * prevValue is the reference for delta calculation */
size_t varintBP128DeltaEncodeBlock32(uint8_t *dst, const uint32_t *values,
                                     uint32_t prevValue);

/* Decode a single block of 128 uint32_t values from deltas */
size_t varintBP128DeltaDecodeBlock32(const uint8_t *src, uint32_t *values,
                                     uint32_t prevValue);

/* ====================================================================
 * Utility Functions
 * ==================================================================== */

/* Calculate bits needed to represent value */
static inline uint8_t varintBP128BitsNeeded32(uint32_t value) {
    if (value == 0) {
        return 0;
    }
    uint8_t bits = 0;
    while (value) {
        value >>= 1;
        bits++;
    }
    return bits;
}

static inline uint8_t varintBP128BitsNeeded64(uint64_t value) {
    if (value == 0) {
        return 0;
    }
    uint8_t bits = 0;
    while (value) {
        value >>= 1;
        bits++;
    }
    return bits;
}

/* Calculate maximum bit-width needed for a block of values */
uint8_t varintBP128MaxBitWidth32(const uint32_t *values, size_t count);
uint8_t varintBP128MaxBitWidth64(const uint64_t *values, size_t count);

/* Check if BP128 encoding would be beneficial */
bool varintBP128IsBeneficial32(const uint32_t *values, size_t count);
bool varintBP128IsBeneficial64(const uint64_t *values, size_t count);

/* Check if data is sorted (for delta encoding) */
bool varintBP128IsSorted32(const uint32_t *values, size_t count);
bool varintBP128IsSorted64(const uint64_t *values, size_t count);

/* Get number of values in encoded data without decoding */
size_t varintBP128GetCount(const uint8_t *src, size_t srcBytes);

#ifdef VARINT_BP128_TEST
int varintBP128Test(int argc, char *argv[]);
#endif

__END_DECLS
