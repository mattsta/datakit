#pragma once

#include "varint.h"
#include "varintExternal.h"

__BEGIN_DECLS

/* ====================================================================
 * PFOR - Patched Frame-of-Reference Encoding
 * ==================================================================== */
/* varint model PFOR (Patched Frame-of-Reference):
 *   Type encoded by: frame min + width + exception list
 *   Size: variable (depends on range and exception count)
 *   Layout: [min][width][count][values...][exception_count][exceptions...]
 *   Meaning: values stored as offsets from min with configurable bit width.
 *            Outliers beyond width stored as exceptions.
 *   Pro: Excellent compression for clustered data with few outliers (<5%).
 *        Random access supported. Optimal for stock prices, latencies, etc.
 *   Con: Less efficient with many outliers or uniform distribution.
 *        Requires two-pass encoding (analyze then encode). */

/* Exception threshold percentiles for PFOR encoding */
#define VARINT_PFOR_THRESHOLD_90 90 /* 90th percentile - more exceptions */
#define VARINT_PFOR_THRESHOLD_95 95 /* 95th percentile - balanced (default) */
#define VARINT_PFOR_THRESHOLD_99 99 /* 99th percentile - fewer exceptions */

/* PFOR metadata structure for encoding/decoding state
 * Fields ordered by size (8-byte → 4-byte) to eliminate padding */
typedef struct varintPFORMeta {
    uint64_t min;             /* Minimum value in frame */
    uint64_t exceptionMarker; /* Marker value for exceptions (all 1s) */
    uint64_t thresholdValue;  /* Actual threshold value from percentile */
    varintWidth width;        /* Width in bytes for regular values */
    uint32_t count;           /* Total number of values */
    uint32_t exceptionCount;  /* Number of exception values */
    uint32_t threshold;       /* Percentile threshold (90, 95, 99) */
} varintPFORMeta;

/* Compile-time size guarantees to prevent regressions */
_Static_assert(sizeof(varintPFORMeta) == 40,
               "varintPFORMeta size changed! Expected 40 bytes (3×8-byte + "
               "4×4-byte, ZERO padding). "
               "This struct achieved 100% efficiency - do not break it!");
_Static_assert(sizeof(varintPFORMeta) <= 64,
               "varintPFORMeta exceeds single cache line (64 bytes)! "
               "Keep this struct cache-friendly for hot encoding paths.");

/* Compute optimal threshold and metadata for encoding.
 * Returns width needed for regular values (non-exceptions).
 * exceptionCount will be set to number of values exceeding threshold. */
varintWidth varintPFORComputeThreshold(const uint64_t *values, uint32_t count,
                                       uint32_t threshold,
                                       varintPFORMeta *meta);

/* Encode array of values using PFOR.
 * dst must have enough space (use varintPFORSize() to calculate).
 * Returns number of bytes written. */
size_t varintPFOREncode(uint8_t *dst, const uint64_t *values, uint32_t count,
                        uint32_t threshold, varintPFORMeta *meta);

/* Decode PFOR-encoded data into values array.
 * values must have space for meta->count elements.
 * Returns number of values decoded. */
size_t varintPFORDecode(const uint8_t *src, uint64_t *values,
                        varintPFORMeta *meta);

/* Random access: get value at specific index.
 * More efficient than full decode for single values.
 * Returns the value at the given index. */
uint64_t varintPFORGetAt(const uint8_t *src, uint32_t index,
                         const varintPFORMeta *meta);

/* Calculate size needed for encoding.
 * Call after varintPFORComputeThreshold() to get accurate size.
 * Returns total bytes needed for encoded output. */
size_t varintPFORSize(const varintPFORMeta *meta);

/* Read metadata from encoded buffer.
 * Parses header to extract encoding parameters.
 * Returns number of header bytes consumed. */
size_t varintPFORReadMeta(const uint8_t *src, varintPFORMeta *meta);

__END_DECLS
