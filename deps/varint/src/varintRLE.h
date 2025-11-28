#pragma once

#include "varint.h"
#include "varintExternal.h"
#include "varintTagged.h"

__BEGIN_DECLS

/* ====================================================================
 * Run-Length Encoding (RLE) varints
 * ==================================================================== */
/* varint model Run-Length Encoding:
 *   Type encoded by: tagged varint for run length + value
 *   Size: variable (2-18 bytes per run)
 *   Layout: [run_length:tagged][value:tagged]...
 *   Meaning: Consecutive identical values stored as (count, value) pairs
 *   Pros: Extremely efficient for data with many repeated values
 *         Self-describing format, random access to run boundaries
 *         Can achieve 95%+ compression for sparse/repetitive data
 *   Cons: No benefit if all values are unique
 *         Worst case doubles storage size for unique sequences */

/* RLE encoding metadata structure */
typedef struct varintRLEMeta {
    size_t count;        /* Number of values in original data */
    size_t runCount;     /* Number of runs in encoded data */
    size_t encodedSize;  /* Total encoded size in bytes */
    size_t uniqueValues; /* Number of distinct values */
} varintRLEMeta;

/* Analyze array and fill metadata structure
 * Returns true if RLE encoding would be beneficial */
bool varintRLEAnalyze(const uint64_t *values, size_t count,
                      varintRLEMeta *meta);

/* Calculate size needed for RLE encoding */
size_t varintRLESize(const uint64_t *values, size_t count);

/* Maximum possible encoded size (worst case: all unique values) */
static inline size_t varintRLEMaxSize(size_t count) {
    /* Worst case: every value unique = count * (1 byte run + 9 bytes value) */
    return count * 10;
}

/* Encode array using Run-Length Encoding
 * dst: output buffer (must be at least varintRLEMaxSize bytes)
 * values: input array of values
 * count: number of values
 * meta: optional metadata output (can be NULL)
 * Returns: number of bytes written to dst */
size_t varintRLEEncode(uint8_t *dst, const uint64_t *values, size_t count,
                       varintRLEMeta *meta);

/* Decode RLE-encoded array
 * src: input buffer containing RLE-encoded data
 * values: output buffer for decoded values
 * maxCount: maximum values that fit in output buffer
 * Returns: number of values decoded, or 0 if buffer too small */
size_t varintRLEDecode(const uint8_t *src, uint64_t *values, size_t maxCount);

/* Get count of original values from encoded data header
 * Uses first run's metadata to determine total count */
size_t varintRLEGetCount(const uint8_t *src);

/* Get count of runs in encoded data */
size_t varintRLEGetRunCount(const uint8_t *src, size_t encodedSize);

/* Random access: get value at specific index without full decode
 * Requires scanning runs from start, but doesn't need output buffer */
uint64_t varintRLEGetAt(const uint8_t *src, size_t index);

/* Decode a single run from encoded data
 * src: pointer to start of run
 * runLength: output for number of repeated values in this run
 * value: output for the repeated value
 * Returns: bytes consumed for this run */
size_t varintRLEDecodeRun(const uint8_t *src, size_t *runLength,
                          uint64_t *value);

/* Check if RLE encoding would be beneficial
 * Returns true if compression ratio > 1.0 */
bool varintRLEIsBeneficial(const uint64_t *values, size_t count);

/* Encode with format that includes total count header
 * Format: [total_count:tagged][run1_len:tagged][run1_val:tagged]...
 * This variant stores total value count for easier decoding */
size_t varintRLEEncodeWithHeader(uint8_t *dst, const uint64_t *values,
                                 size_t count, varintRLEMeta *meta);

/* Decode RLE with header format
 * Reads count from header, doesn't need separate count parameter */
size_t varintRLEDecodeWithHeader(const uint8_t *src, uint64_t *values,
                                 size_t maxCount);

#ifdef VARINT_RLE_TEST
int varintRLETest(int argc, char *argv[]);
#endif

__END_DECLS
