#pragma once

#include "varint.h"
#include "varintDelta.h"
#include "varintExternal.h"

__BEGIN_DECLS

/* ====================================================================
 * Delta-of-Delta (Second-Order Delta) Varints
 * ==================================================================== */
/* varint model Delta-of-Delta Encoding (Gorilla-style, second-order):
 *   Type encoded by: base + first delta + (count-2) ZigZag delta-of-deltas
 *   Size: variable per element (1-9 bytes each via varintExternal width tag)
 *   Layout:
 *     [base_width:1][base][delta1_width:1][delta1]
 *     [dod1_width:1][dod1] ... [dodN_width:1][dodN]
 *   Meaning:
 *     - dod[i] = (values[i+2] - values[i+1]) - (values[i+1] - values[i])
 *     - For monotonic-stride series (timestamps at fixed intervals, smooth
 *       sensor curves), dod values are near-zero so most encode in 1 byte.
 *   Pros:
 *     - 7x+ compression for regular-interval time series (Gorilla pattern)
 *     - Self-describing format (no external width tracking needed)
 *     - Signed-clean via ZigZag at every level
 *     - Promotion of examples/integration/delta_compression.c to first-class
 *   Cons:
 *     - Strictly sequential decode (each dod adds to running delta)
 *     - Pays a 1-byte width tag per element; if values are erratic enough
 *       that dod doesn't shrink, plain varintDelta is smaller. */

/* Metadata describing a delta-of-delta encoding result.
 * Fields ordered by size (8-byte > 4-byte > 1-byte) to minimize padding. */
typedef struct varintDeltaDeltaMeta {
    size_t count;       /* Number of values represented */
    size_t encodedSize; /* Total bytes in encoded output */
    size_t zeroDoD;     /* Count of dod entries that were exactly zero */
    size_t oneByteDoD;  /* Count of dod entries that fit in 1 byte */
} varintDeltaDeltaMeta;

/* Compile-time size guarantee — stays cache-friendly. */
_Static_assert(sizeof(varintDeltaDeltaMeta) == 32,
               "varintDeltaDeltaMeta size changed! Expected 32 bytes "
               "(4x8-byte, ZERO padding). 100% efficient.");
_Static_assert(sizeof(varintDeltaDeltaMeta) <= 64,
               "varintDeltaDeltaMeta exceeds single cache line!");

/* ====================================================================
 * Single value encode/decode (advanced use)
 * ==================================================================== */

/* Encode a single ZigZag-tagged signed value into buffer.
 * Same wire format as varintDeltaPut (shared so DoD and Delta interoperate).
 * Returns bytes written: 1 (width) + width (data). */
static inline varintWidth varintDeltaDeltaPut(uint8_t *p, int64_t signedValue) {
    return varintDeltaPut(p, signedValue);
}

/* Decode a single ZigZag-tagged signed value from buffer.
 * Sets *pValue to the decoded signed value.
 * Returns bytes read: 1 (width) + width (data). */
static inline varintWidth varintDeltaDeltaGet(const uint8_t *p,
                                              int64_t *pValue) {
    return varintDeltaGet(p, pValue);
}

/* ====================================================================
 * Array encode/decode (primary API)
 * ==================================================================== */

/* Maximum possible encoded size for count values.
 * Worst case: every element needs a 1-byte width tag + 9-byte payload. */
static inline size_t varintDeltaDeltaMaxEncodedSize(size_t count) {
    if (count == 0) {
        return 0;
    }
    /* count * (1 byte width + up to 8 bytes value) */
    return count * 9;
}

/* Encode array of signed values using second-order delta encoding.
 * output: caller-allocated buffer (use varintDeltaDeltaMaxEncodedSize)
 * values: input signed array (any int64_t)
 * count:  number of values
 * meta:   optional output (may be NULL)
 * Returns: total bytes written. */
size_t varintDeltaDeltaEncode(uint8_t *output, const int64_t *values,
                              size_t count, varintDeltaDeltaMeta *meta);

/* Decode a delta-of-delta-encoded array back to absolute signed values.
 * input:  encoded buffer
 * count:  number of values to decode (must match original count)
 * output: caller-allocated buffer (count * sizeof(int64_t))
 * Returns: total bytes read. */
size_t varintDeltaDeltaDecode(const uint8_t *input, size_t count,
                              int64_t *output);

/* Unsigned-value variant. Internally still ZigZag-encodes the underlying
 * signed deltas (since deltas can be negative even with unsigned inputs).
 * Returns: total bytes written. */
size_t varintDeltaDeltaEncodeUnsigned(uint8_t *output, const uint64_t *values,
                                      size_t count, varintDeltaDeltaMeta *meta);

/* Decode unsigned-value variant.
 * Returns: total bytes read. */
size_t varintDeltaDeltaDecodeUnsigned(const uint8_t *input, size_t count,
                                      uint64_t *output);

/* ====================================================================
 * Analysis / Beneficial check
 * ==================================================================== */

/* Analyze without writing output; fills meta with predicted stats.
 * Cheap O(n) two-pass over values.
 * Returns true if predicted encoded size < count*sizeof(int64_t). */
bool varintDeltaDeltaAnalyze(const int64_t *values, size_t count,
                             varintDeltaDeltaMeta *meta);

/* Convenience: would DoD be smaller than raw int64 storage? */
static inline bool varintDeltaDeltaIsBeneficial(const int64_t *values,
                                                size_t count) {
    varintDeltaDeltaMeta meta;
    return varintDeltaDeltaAnalyze(values, count, &meta);
}

#ifdef VARINT_DELTA_DELTA_TEST
int varintDeltaDeltaTest(int argc, char *argv[]);
#endif

__END_DECLS
