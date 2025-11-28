#pragma once

#include "varint.h"
#include "varintBitmap.h"
#include "varintDelta.h"
#include "varintDict.h"
#include "varintExternal.h"
#include "varintFOR.h"
#include "varintPFOR.h"
#include "varintTagged.h"

__BEGIN_DECLS

/* ====================================================================
 * Adaptive Varint Encoding - Intelligent Automatic Selection
 * ==================================================================== */
/* varint model Adaptive Container:
 *   Type encoded inside: automatically selects optimal encoding
 *   Size: variable based on selected encoding + 1-byte header
 *   Layout: [encoding_type:1byte][encoding_data...]
 *   Decision tree: analyzes data characteristics to choose:
 *     - DICT:   high repetition (unique ratio < 10%)
 *     - DELTA:  sorted/sequential with small deltas
 *     - FOR:    clustered values with small range
 *     - PFOR:   clustered with few outliers (<5%)
 *     - BITMAP: dense boolean/sparse sets in 0-65535 range
 *     - TAGGED: fallback for general purpose
 *
 *   Pros: No manual encoding selection needed
 *         Achieves near-optimal compression automatically
 *         Self-describing format (encoding type stored in header)
 *         Transparent to applications
 *   Cons: Analysis overhead (one-pass scan)
 *         Slightly larger header (1 byte for encoding type)
 *         Not suitable for streaming (needs full dataset)
 *
 *   Use cases: Log compression, database indexes, columnar storage,
 *              API responses, configuration files, time series */

/* Encoding types that adaptive can select */
typedef enum varintAdaptiveEncodingType {
    VARINT_ADAPTIVE_DELTA = 0,  /* Delta encoding for sorted/sequential */
    VARINT_ADAPTIVE_FOR = 1,    /* Frame-of-Reference for clustered */
    VARINT_ADAPTIVE_PFOR = 2,   /* Patched FOR with outliers */
    VARINT_ADAPTIVE_DICT = 3,   /* Dictionary for repetitive */
    VARINT_ADAPTIVE_BITMAP = 4, /* Bitmap for dense sets in 0-65535 */
    VARINT_ADAPTIVE_TAGGED = 5, /* Fallback general purpose */
    VARINT_ADAPTIVE_GROUP = 6,  /* Grouped encoding (future) */
} varintAdaptiveEncodingType;

/* Data characteristics computed during analysis
 * Fields ordered by size (8-byte → 4-byte → 1-byte) to eliminate padding */
typedef struct varintAdaptiveDataStats {
    size_t count;        /* Number of values */
    uint64_t minValue;   /* Minimum value */
    uint64_t maxValue;   /* Maximum value */
    uint64_t range;      /* maxValue - minValue */
    size_t uniqueCount;  /* Number of unique values */
    uint64_t avgDelta;   /* Average absolute delta between consecutive values */
    uint64_t maxDelta;   /* Maximum absolute delta */
    size_t outlierCount; /* Count of values beyond 95th percentile */
    float uniqueRatio;   /* uniqueCount / count */
    float outlierRatio;  /* outlierCount / count */
    bool isSorted;       /* True if array is sorted */
    bool isReverseSorted;   /* True if reverse sorted */
    bool fitsInBitmapRange; /* True if all values < 65536 */
} varintAdaptiveDataStats;

/* Compile-time size guarantees to prevent regressions */
_Static_assert(sizeof(varintAdaptiveDataStats) == 80,
               "varintAdaptiveDataStats size changed! Expected 80 bytes "
               "(8×8-byte + 2×4-byte + 3×1-byte + 5 padding). "
               "93.8% efficient - acceptable for analysis struct.");
_Static_assert(
    sizeof(varintAdaptiveDataStats) <= 128,
    "varintAdaptiveDataStats should not exceed 2 cache lines (128 bytes)! "
    "Hot fields (bytes 0-63) are in first cache line by design.");

/* Metadata for selected encoding
 * Fields ordered by size (8-byte/union → 4-byte) to eliminate padding */
typedef struct varintAdaptiveMeta {
    size_t originalCount; /* Number of values encoded */
    size_t encodedSize;   /* Total bytes including header */

    /* Encoding-specific metadata (only one used based on encodingType) */
    union {
        varintFORMeta forMeta;
        varintPFORMeta pforMeta;
        /* Dict and Delta don't need extra metadata (embedded in encoding) */
    } encodingMeta;

    varintAdaptiveEncodingType encodingType;
} varintAdaptiveMeta;

/* Compile-time size guarantees to prevent regressions */
_Static_assert(sizeof(varintAdaptiveMeta) == 72,
               "varintAdaptiveMeta size changed! Expected 72 bytes (2×8-byte + "
               "48-byte union + 4-byte enum + 4 padding). "
               "94.4% efficient - union dominates size.");
_Static_assert(
    sizeof(varintAdaptiveMeta) <= 128,
    "varintAdaptiveMeta should not exceed 2 cache lines (128 bytes)! "
    "Metadata union in first cache line, encodingType in second (read once "
    "during decode).");

/* ====================================================================
 * Core API
 * ==================================================================== */

/* Analyze data characteristics to determine best encoding.
 * This is called automatically by varintAdaptiveEncode, but can be
 * called separately to inspect data characteristics.
 *
 * values: array of values to analyze
 * count: number of values
 * stats: output statistics structure
 */
void varintAdaptiveAnalyze(const uint64_t *values, size_t count,
                           varintAdaptiveDataStats *stats);

/* Select optimal encoding based on data statistics.
 * Returns the recommended encoding type.
 *
 * Decision tree logic:
 * 1. If uniqueRatio < 0.1 (< 10% unique) → DICT
 * 2. If fitsInBitmapRange and count < 10000 → BITMAP
 * 3. If isSorted and avgDelta small → DELTA
 * 4. If outlierRatio < 0.05 (< 5% outliers) → PFOR
 * 5. If range < count * 100 → FOR
 * 6. Otherwise → TAGGED
 */
varintAdaptiveEncodingType
varintAdaptiveSelectEncoding(const varintAdaptiveDataStats *stats);

/* Encode array with automatic encoding selection.
 * Analyzes data, selects optimal encoding, and encodes with header.
 *
 * dst: output buffer (must be large enough - use varintAdaptiveMaxSize)
 * values: array of values to encode
 * count: number of values
 * meta: optional output metadata (can be NULL)
 * Returns: number of bytes written to dst
 *
 * Format: [encoding_type:1byte][encoded_data...]
 */
size_t varintAdaptiveEncode(uint8_t *dst, const uint64_t *values, size_t count,
                            varintAdaptiveMeta *meta);

/* Encode with a specific encoding (bypassing auto-selection).
 * Useful for testing or when you know the best encoding.
 *
 * dst: output buffer
 * values: array of values to encode
 * count: number of values
 * encodingType: specific encoding to use
 * meta: optional output metadata (can be NULL)
 * Returns: number of bytes written to dst
 */
size_t varintAdaptiveEncodeWith(uint8_t *dst, const uint64_t *values,
                                size_t count,
                                varintAdaptiveEncodingType encodingType,
                                varintAdaptiveMeta *meta);

/* Decode adaptively-encoded data.
 * Reads encoding type from header and delegates to appropriate decoder.
 *
 * src: encoded buffer (must start with encoding type byte)
 * values: output array (must be pre-allocated with maxCount capacity)
 * maxCount: maximum values to decode (size of values array)
 * meta: optional output metadata (can be NULL)
 * Returns: number of values decoded
 */
size_t varintAdaptiveDecode(const uint8_t *src, uint64_t *values,
                            size_t maxCount, varintAdaptiveMeta *meta);

/* Read metadata from encoded buffer without full decoding.
 * Useful for inspecting encoding type and size.
 *
 * src: encoded buffer
 * meta: output metadata structure
 * Returns: size of header in bytes
 */
size_t varintAdaptiveReadMeta(const uint8_t *src, varintAdaptiveMeta *meta);

/* Get encoding type from encoded buffer (just reads first byte).
 * Returns: encoding type
 */
static inline varintAdaptiveEncodingType
varintAdaptiveGetEncodingType(const uint8_t *src) {
    return (varintAdaptiveEncodingType)src[0];
}

/* Get human-readable name of encoding type.
 * Useful for debugging and logging.
 * Returns: static string name of encoding
 */
const char *varintAdaptiveEncodingName(varintAdaptiveEncodingType type);

/* Calculate maximum possible encoded size for count values.
 * This is a conservative upper bound - actual size usually much smaller.
 * Use for pre-allocating output buffers.
 *
 * count: number of values to encode
 * Returns: maximum bytes needed (worst-case scenario)
 */
static inline size_t varintAdaptiveMaxSize(size_t count) {
    if (count == 0) {
        return 1; /* Just header byte */
    }

    /* Worst case: TAGGED encoding with 1 byte header + 9 bytes per value
     * Header: 1 byte encoding type
     * Data: worst case is tagged (9 bytes per uint64_t) */
    return 1 + (count * 9);
}

/* Calculate compression ratio.
 * ratio > 1.0 means compression achieved (e.g., 5.0 = 80% reduction)
 * ratio < 1.0 means expansion (rare, only with tiny arrays)
 *
 * originalCount: number of values
 * encodedSize: size in bytes after encoding
 * Returns: compression ratio (originalBytes / encodedBytes)
 */
static inline float varintAdaptiveCompressionRatio(size_t originalCount,
                                                   size_t encodedSize) {
    if (encodedSize == 0) {
        return 0.0f;
    }
    size_t originalSize = originalCount * sizeof(uint64_t);
    return (float)originalSize / (float)encodedSize;
}

/* ====================================================================
 * Analysis Helpers
 * ==================================================================== */

/* Compute sortedness of array.
 * Returns: 1 if sorted ascending, -1 if sorted descending, 0 if unsorted
 */
int varintAdaptiveCheckSorted(const uint64_t *values, size_t count);

/* Count unique values in array (approximate for large arrays).
 * Uses simple hash-based counting for speed.
 * For exact count on small arrays, sorts and counts.
 *
 * values: array to analyze
 * count: number of values
 * Returns: approximate number of unique values
 */
size_t varintAdaptiveCountUnique(const uint64_t *values, size_t count);

/* Compute average absolute delta between consecutive values.
 * Useful for determining if DELTA encoding will be efficient.
 *
 * values: array to analyze
 * count: number of values
 * Returns: average absolute delta
 */
uint64_t varintAdaptiveAvgDelta(const uint64_t *values, size_t count);

__END_DECLS
