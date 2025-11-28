#pragma once

#include "varint.h"
#include "varintExternal.h"
#include "varintTagged.h"

__BEGIN_DECLS

/* ====================================================================
 * Frame-of-Reference (FOR) varints
 * ==================================================================== */
/* varint model Frame-of-Reference:
 *   Type encoded by: tagged varint header + offset width byte
 *   Size: 3-19 bytes header + (count * offset_width) bytes
 *   Layout:
 * [min_value:tagged][offset_width:1byte][count:tagged][offset1]...[offsetN]
 *   Meaning: All values stored as fixed-width offsets from minimum value
 *   Pros: Extremely efficient for clustered values (timestamps, IDs, prices)
 *         SIMD-friendly (all offsets same width), supports random access
 *         Can achieve 67%+ compression for clustered data
 *         Self-describing header (uses tagged varints for min and count)
 *   Cons: Requires computing min/max first, entire array must fit in memory
 *         Less efficient if values have large range relative to count */

/* FOR encoding metadata structure
 * Fields ordered by size (8-byte → 4-byte) to eliminate padding */
typedef struct varintFORMeta {
    uint64_t minValue;       /* Minimum value in the dataset */
    uint64_t maxValue;       /* Maximum value in the dataset */
    uint64_t range;          /* maxValue - minValue */
    size_t count;            /* Number of values encoded */
    size_t encodedSize;      /* Total encoded size in bytes */
    varintWidth offsetWidth; /* Bytes per offset (1-8) */
} varintFORMeta;

/* Compile-time size guarantees to prevent regressions */
_Static_assert(sizeof(varintFORMeta) == 48,
               "varintFORMeta size changed! Expected 48 bytes (5×8-byte + "
               "1×4-byte + 4 padding). "
               "Review field ordering if this fails.");
_Static_assert(
    sizeof(varintFORMeta) <= 64,
    "varintFORMeta exceeds single cache line (64 bytes)! "
    "This will hurt performance - keep metadata structs cache-friendly.");

/* Compute optimal offset width for a range of values */
varintWidth varintFORComputeWidth(const uint64_t range);

/* Analyze array and fill metadata structure */
void varintFORAnalyze(const uint64_t *values, const size_t count,
                      varintFORMeta *meta);

/* Calculate size needed for FOR encoding */
size_t varintFORSize(const varintFORMeta *meta);

/* Encode array using Frame-of-Reference
 * Returns number of bytes written to 'dst' */
size_t varintFOREncode(uint8_t *dst, const uint64_t *values, const size_t count,
                       varintFORMeta *meta);

/* Decode entire FOR-encoded array
 * Returns number of values decoded */
size_t varintFORDecode(const uint8_t *src, uint64_t *values,
                       const size_t maxCount);

/* Random access: get value at specific index without full decode */
uint64_t varintFORGetAt(const uint8_t *src, const size_t index);

/* Extract metadata from encoded FOR data */
void varintFORReadMetadata(const uint8_t *src, varintFORMeta *meta);

/* Get minimum value from encoded data */
uint64_t varintFORGetMinValue(const uint8_t *src);

/* Get count of values from encoded data */
size_t varintFORGetCount(const uint8_t *src);

/* Get offset width from encoded data */
varintWidth varintFORGetOffsetWidth(const uint8_t *src);

/* ====================================================================
 * SIMD-Accelerated Batch Operations
 * ==================================================================== */

/* Check if SIMD batch operations are available at runtime */
bool varintFORHasSIMD(void);

/* SIMD-accelerated batch decode (up to 8x faster for large arrays)
 * Falls back to scalar if SIMD unavailable or count too small.
 * Returns number of values decoded. */
size_t varintFORBatchDecode(const uint8_t *src, uint64_t *values,
                            const size_t maxCount);

/* SIMD-accelerated min/max analysis (up to 4x faster)
 * Falls back to scalar if SIMD unavailable or count too small. */
void varintFORBatchAnalyze(const uint64_t *values, const size_t count,
                           varintFORMeta *meta);

/* SIMD-accelerated batch encode (fixed-width writes)
 * Falls back to scalar if SIMD unavailable or count too small.
 * Returns number of bytes written. */
size_t varintFORBatchEncode(uint8_t *dst, const uint64_t *values,
                            const size_t count, varintFORMeta *meta);

/* Decode a block of values starting at given index
 * Useful for streaming/chunked processing */
size_t varintFORDecodeBlock(const uint8_t *src, uint64_t *values,
                            const size_t startIndex, const size_t blockSize);

__END_DECLS
