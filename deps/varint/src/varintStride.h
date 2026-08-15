#pragma once

#include "varint.h"
#include "varintDelta.h"
#include "varintExternal.h"
#include "varintTagged.h"

__BEGIN_DECLS

/* ====================================================================
 * Stride Encoding — Arithmetic Progressions
 * ==================================================================== */
/* varint model Stride Encoding:
 *   Type encoded by: base + signed stride + count [+ exceptions]
 *   Size:
 *     - Exact stride: 3 small varints regardless of count
 *     - Fuzzy stride: above + (1 + 9) bytes per exception
 *   Layout (exact):
 *     [mode:1][base:tagged-zigzag][stride:tagged-zigzag][count:tagged]
 *   Layout (fuzzy):
 *     [mode:1][base:tagged-zigzag][stride:tagged-zigzag][count:tagged]
 *     [excCount:tagged]
 *     [idx1:tagged][actual1:tagged-zigzag]
 *     [idx2:tagged][actual2:tagged-zigzag] ...
 *   Meaning:
 *     Most sequences in real workloads (page offsets, paginated IDs, fixed
 *     polling intervals, sensor cadences) are arithmetic progressions or
 *     "near-progressions" with a handful of outliers. Stride encoding stores
 *     ~24 bytes total regardless of array length for exact runs.
 *   Pros:
 *     - Constant-size encoding for arithmetic progressions
 *     - O(1) random access via base + index*stride
 *     - Tolerates outliers via fuzzy mode (still pays only for exceptions)
 *   Cons:
 *     - Beats nothing if data isn't actually a progression
 *     - Fuzzy mode degrades back to ~varintDelta when >~20% are exceptions */

/* Mode byte distinguishes exact vs fuzzy on the wire. */
typedef enum varintStrideMode {
    VARINT_STRIDE_MODE_EXACT = 0,
    VARINT_STRIDE_MODE_FUZZY = 1,
} varintStrideMode;

/* Stride encoding metadata. Fields ordered by size to eliminate padding. */
typedef struct varintStrideMeta {
    int64_t base;          /* values[0] */
    int64_t stride;        /* values[1] - values[0] (signed) */
    size_t count;          /* Number of values represented */
    size_t exceptionCount; /* 0 for exact mode; >0 for fuzzy */
    size_t encodedSize;    /* Total bytes when encoded */
    varintStrideMode mode; /* Exact vs fuzzy */
} varintStrideMeta;

_Static_assert(sizeof(varintStrideMeta) == 48,
               "varintStrideMeta size changed! Expected 48 bytes "
               "(2x8-byte signed + 3x8-byte size_t + 4-byte enum + 4 pad).");
_Static_assert(sizeof(varintStrideMeta) <= 64,
               "varintStrideMeta exceeds single cache line!");

/* Fuzzy threshold heuristic: fuzzy stride is allowed if exceptions are
 * at most this fraction of count. Above this, plain Delta wins anyway. */
#define VARINT_STRIDE_FUZZY_MAX_FRACTION 0.20

/* ====================================================================
 * Analysis
 * ==================================================================== */

/* Analyze a signed array, detecting:
 *   - Exact stride: every consecutive delta equals values[1]-values[0]
 *   - Fuzzy stride: most deltas match; up to FUZZY_MAX_FRACTION outliers
 * Sets meta->mode accordingly and predicts encodedSize.
 * Returns true if encoding is beneficial vs raw int64 storage. */
bool varintStrideAnalyze(const int64_t *values, size_t count,
                         varintStrideMeta *meta);

/* Same but for unsigned input. Stride is computed in signed 2's-complement
 * arithmetic so it handles values that wrap modularly. */
bool varintStrideAnalyzeUnsigned(const uint64_t *values, size_t count,
                                 varintStrideMeta *meta);

/* Convenience: is stride encoding beneficial? */
static inline bool varintStrideIsBeneficial(const int64_t *values,
                                            size_t count) {
    varintStrideMeta m;
    return varintStrideAnalyze(values, count, &m);
}

/* ====================================================================
 * Encoding / Decoding
 * ==================================================================== */

/* Maximum possible encoded size for count values.
 * Worst case: fuzzy mode with FUZZY_MAX_FRACTION exceptions. */
static inline size_t varintStrideMaxEncodedSize(size_t count) {
    /* Mode(1) + base(9) + stride(9) + count(9) + excCount(9) +
     *   exceptions * (9 idx + 9 value) */
    size_t header = 1 + 9 + 9 + 9 + 9;
    size_t maxExc = (size_t)(count * VARINT_STRIDE_FUZZY_MAX_FRACTION) + 1;
    return header + maxExc * 18;
}

/* Encode array as exact or fuzzy stride. Analyzes internally if meta is
 * NULL or stale. Returns bytes written, or 0 if not beneficial. */
size_t varintStrideEncode(uint8_t *dst, const int64_t *values, size_t count,
                          varintStrideMeta *meta);

/* Force a specific mode (for testing or callers who already know).
 * Returns 0 if exact requested but data isn't an exact stride. */
size_t varintStrideEncodeWithMode(uint8_t *dst, const int64_t *values,
                                  size_t count, varintStrideMode mode,
                                  varintStrideMeta *meta);

/* Decode stride-encoded data. count must match the encoded count.
 * Returns bytes consumed. */
size_t varintStrideDecode(const uint8_t *src, size_t count, int64_t *output);

/* Unsigned encode/decode wrappers — same internal format, signed math. */
size_t varintStrideEncodeUnsigned(uint8_t *dst, const uint64_t *values,
                                  size_t count, varintStrideMeta *meta);
size_t varintStrideDecodeUnsigned(const uint8_t *src, size_t count,
                                  uint64_t *output);

/* ====================================================================
 * Random access + metadata extraction
 * ==================================================================== */

/* Read mode + base + stride + count without decoding exceptions.
 * Returns bytes consumed for the fixed header. */
size_t varintStrideReadMeta(const uint8_t *src, varintStrideMeta *meta);

/* Random access at index. For exact mode this is O(1); for fuzzy mode
 * O(exceptionCount) since exceptions must be scanned for index match. */
int64_t varintStrideGetAt(const uint8_t *src, size_t index);

/* Mode (first byte) — handy for dispatch / debugging. */
static inline varintStrideMode varintStrideGetMode(const uint8_t *src) {
    return (varintStrideMode)src[0];
}

#ifdef VARINT_STRIDE_TEST
int varintStrideTest(int argc, char *argv[]);
#endif

__END_DECLS
