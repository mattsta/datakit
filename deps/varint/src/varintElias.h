#pragma once

#include "varint.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

__BEGIN_DECLS

/* ====================================================================
 * Elias Gamma and Delta Universal Codes
 * ==================================================================== */
/* Elias Gamma Code:
 *   Encodes positive integer N as:
 *   1. floor(log2(N)) zeros followed by
 *   2. N in binary (including leading 1 bit)
 *   Example: 1=1, 2=010, 3=011, 4=00100, 5=00101, 9=0001001
 *   Pros: Optimal for geometric distribution P(n)=2^(-n)
 *         Prefix-free (self-delimiting)
 *         Very compact for small integers
 *   Cons: Not efficient for larger integers (use Delta instead)
 *
 * Elias Delta Code:
 *   Encodes positive integer N as:
 *   1. Encode floor(log2(N))+1 using Gamma code
 *   2. Write remaining log2(N) bits of N (without leading 1)
 *   Example: 1=1, 2=0100, 3=0101, 4=01100, 8=00100000, 9=00100001
 *   Pros: More efficient than Gamma for larger integers
 *         Still prefix-free (self-delimiting)
 *         Optimal for different distributions
 *   Cons: Slight overhead for very small integers (1-3)
 *
 * Note: These codes work for positive integers only (N >= 1).
 *       For zero and negative values, use ZigZag encoding first. */

/* Encoding statistics */
typedef struct varintEliasMeta {
    size_t count;        /* Number of values encoded */
    size_t totalBits;    /* Total bits used in encoding */
    size_t encodedBytes; /* Ceiling of totalBits/8 */
} varintEliasMeta;

/* ====================================================================
 * Bit-level I/O helpers (internal, but exposed for advanced use)
 * ==================================================================== */

/* Bit writer state for encoding */
typedef struct varintBitWriter {
    uint8_t *buffer; /* Output buffer */
    size_t bitPos;   /* Current bit position */
    size_t capacity; /* Buffer capacity in bytes */
} varintBitWriter;

/* Bit reader state for decoding */
typedef struct varintBitReader {
    const uint8_t *buffer; /* Input buffer */
    size_t bitPos;         /* Current bit position */
    size_t totalBits;      /* Total bits available */
} varintBitReader;

/* Initialize bit writer */
void varintBitWriterInit(varintBitWriter *w, uint8_t *buffer, size_t capacity);

/* Write n bits to output */
void varintBitWriterWrite(varintBitWriter *w, uint64_t value, size_t nBits);

/* Get number of bytes written (rounded up) */
size_t varintBitWriterBytes(const varintBitWriter *w);

/* Initialize bit reader */
void varintBitReaderInit(varintBitReader *r, const uint8_t *buffer,
                         size_t totalBits);

/* Read n bits from input */
uint64_t varintBitReaderRead(varintBitReader *r, size_t nBits);

/* Check if more bits available */
bool varintBitReaderHasMore(const varintBitReader *r, size_t nBits);

/* ====================================================================
 * Elias Gamma Encoding/Decoding
 * ==================================================================== */

/* Calculate bits needed for Gamma encoding of single value */
size_t varintEliasGammaBits(uint64_t value);

/* Encode single value using Gamma code (value must be >= 1)
 * Writes to bit writer, returns bits written */
size_t varintEliasGammaEncode(varintBitWriter *w, uint64_t value);

/* Decode single value using Gamma code
 * Reads from bit reader, returns decoded value (>= 1) */
uint64_t varintEliasGammaDecode(varintBitReader *r);

/* Encode array of values using Gamma code
 * dst: output buffer (must be large enough)
 * values: array of positive integers (>= 1)
 * count: number of values
 * meta: optional output statistics
 * Returns: bytes written */
size_t varintEliasGammaEncodeArray(uint8_t *dst, const uint64_t *values,
                                   size_t count, varintEliasMeta *meta);

/* Decode Gamma-encoded array
 * src: input buffer
 * srcBits: total bits in input
 * values: output buffer
 * maxCount: max values to decode
 * Returns: number of values decoded */
size_t varintEliasGammaDecodeArray(const uint8_t *src, size_t srcBits,
                                   uint64_t *values, size_t maxCount);

/* ====================================================================
 * Elias Delta Encoding/Decoding
 * ==================================================================== */

/* Calculate bits needed for Delta encoding of single value */
size_t varintEliasDeltaBits(uint64_t value);

/* Encode single value using Delta code (value must be >= 1)
 * Writes to bit writer, returns bits written */
size_t varintEliasDeltaEncode(varintBitWriter *w, uint64_t value);

/* Decode single value using Delta code
 * Reads from bit reader, returns decoded value (>= 1) */
uint64_t varintEliasDeltaDecode(varintBitReader *r);

/* Encode array of values using Delta code
 * dst: output buffer (must be large enough)
 * values: array of positive integers (>= 1)
 * count: number of values
 * meta: optional output statistics
 * Returns: bytes written */
size_t varintEliasDeltaEncodeArray(uint8_t *dst, const uint64_t *values,
                                   size_t count, varintEliasMeta *meta);

/* Decode Delta-encoded array
 * src: input buffer
 * srcBits: total bits in input
 * values: output buffer
 * maxCount: max values to decode
 * Returns: number of values decoded */
size_t varintEliasDeltaDecodeArray(const uint8_t *src, size_t srcBits,
                                   uint64_t *values, size_t maxCount);

/* ====================================================================
 * Utility Functions
 * ==================================================================== */

/* Calculate max bytes needed for Gamma encoding count values (worst case) */
static inline size_t varintEliasGammaMaxBytes(size_t count) {
    /* Worst case: 64-bit values need 127 bits each (63 zeros + 64 bits) */
    return (count * 127 + 7) / 8;
}

/* Calculate max bytes needed for Delta encoding count values (worst case) */
static inline size_t varintEliasDeltaMaxBytes(size_t count) {
    /* Worst case: 64-bit values need ~76 bits each */
    return (count * 76 + 7) / 8;
}

/* Check if Gamma encoding would be more efficient than raw storage */
bool varintEliasGammaIsBeneficial(const uint64_t *values, size_t count);

/* Check if Delta encoding would be more efficient than raw storage */
bool varintEliasDeltaIsBeneficial(const uint64_t *values, size_t count);

#ifdef VARINT_ELIAS_TEST
int varintEliasTest(int argc, char *argv[]);
#endif

__END_DECLS
