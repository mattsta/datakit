#pragma once

#include "varint.h"
#include "varintExternal.h"
#include "varintTagged.h"

__BEGIN_DECLS

/* ====================================================================
 * Dictionary Encoding for Repetitive Values
 * ==================================================================== */
/* varint model Dictionary Container:
 *   Type encoded by: dictionary of unique values + indices
 *   Layout: [dict_size][dict_entries...][count][indices...]
 *   Size: Optimal for highly repetitive data (10 unique values in 1M entries)
 *   Encoding: dict_entries use varintTagged, indices use varintExternal
 *   Pro: 99%+ compression for repetitive data (logs, enums, status codes)
 *   Con: Only efficient when cardinality << count (< 10% unique values) */

/* Dictionary structure for building and using dictionaries */
typedef struct varintDict {
    uint64_t *values;       /* Dictionary values (sorted) */
    uint32_t size;          /* Number of unique values */
    uint32_t capacity;      /* Allocated capacity */
    varintWidth indexWidth; /* Width needed for indices */
} varintDict;

/* Compile-time size guarantees to prevent regressions */
_Static_assert(
    sizeof(varintDict) == 24,
    "varintDict size changed! Expected 24 bytes (8-byte pointer + 3×4-byte + 4 "
    "padding). "
    "83.3% efficient - padding required for 8-byte alignment of pointer.");
_Static_assert(sizeof(varintDict) <= 64,
               "varintDict exceeds single cache line (64 bytes)! "
               "Keep dictionary struct cache-friendly.");

/* ====================================================================
 * Dictionary Management
 * ==================================================================== */

/* Initialize a new dictionary structure.
 * Returns pointer to dictionary or NULL on allocation failure. */
varintDict *varintDictCreate(void);

/* Free dictionary and all associated memory. */
void varintDictFree(varintDict *dict);

/* Build dictionary from an array of values.
 * Finds unique values and assigns indices.
 * Returns 0 on success, -1 on failure. */
int varintDictBuild(varintDict *dict, const uint64_t *values, size_t count);

/* Lookup a value's index in the dictionary.
 * Returns index or -1 if value not found (use signed return for error). */
int32_t varintDictFind(const varintDict *dict, const uint64_t value);

/* Get value at given index in dictionary.
 * Returns value or 0 if index out of bounds. */
uint64_t varintDictLookup(const varintDict *dict, const uint32_t index);

/* ====================================================================
 * Encoding and Decoding
 * ==================================================================== */

/* Encode values using dictionary compression.
 * Format: [dict_size][dict_entries...][count][indices...]
 * Returns number of bytes written, or 0 on error.
 * Buffer must be large enough (use varintDictEncodedSize to calculate). */
size_t varintDictEncode(uint8_t *buffer, const uint64_t *values, size_t count);

/* Encode values using pre-built dictionary.
 * Faster if reusing dictionary across multiple arrays.
 * Returns number of bytes written, or 0 on error. */
size_t varintDictEncodeWithDict(uint8_t *buffer, const varintDict *dict,
                                const uint64_t *values, size_t count);

/* Decode dictionary-encoded data.
 * Allocates output array (caller must free).
 * Returns decoded array or NULL on error.
 * *outCount is set to number of decoded values. */
uint64_t *varintDictDecode(const uint8_t *buffer, size_t bufferLen,
                           size_t *outCount);

/* Decode dictionary-encoded data into pre-allocated array.
 * Returns number of values decoded, or 0 on error.
 * maxValues is the capacity of the output array. */
size_t varintDictDecodeInto(const uint8_t *buffer, size_t bufferLen,
                            uint64_t *output, size_t maxValues);

/* ====================================================================
 * Size Calculation and Analysis
 * ==================================================================== */

/* Calculate size needed for dictionary encoding.
 * Returns size in bytes, or 0 if encoding not beneficial. */
size_t varintDictEncodedSize(const uint64_t *values, size_t count);

/* Calculate size with pre-built dictionary.
 * Useful for estimating shared dictionary compression. */
size_t varintDictEncodedSizeWithDict(const varintDict *dict,
                                     const size_t count);

/* Calculate compression ratio.
 * Returns ratio > 1.0 for savings, < 1.0 for expansion.
 * Example: 10.0 means 10x compression (90% space savings). */
float varintDictCompressionRatio(const uint64_t *values, size_t count);

/* Get statistics about the dictionary encoding.
 * Returns 0 on success, -1 on failure. */
typedef struct varintDictStats {
    size_t uniqueCount;     /* Number of unique values */
    size_t totalCount;      /* Total number of values */
    size_t dictBytes;       /* Bytes for dictionary */
    size_t indexBytes;      /* Bytes for indices */
    size_t totalBytes;      /* Total encoded size */
    size_t originalBytes;   /* Size with uint64_t */
    float compressionRatio; /* originalBytes / totalBytes */
    float spaceReduction;   /* (1 - totalBytes/originalBytes) * 100 */
} varintDictStats;

/* Compile-time size guarantees to prevent regressions */
_Static_assert(sizeof(varintDictStats) == 56,
               "varintDictStats size changed! Expected 56 bytes (6×8-byte + "
               "2×4-byte, ZERO padding). "
               "This struct achieved 100% efficiency - do not break it!");
_Static_assert(sizeof(varintDictStats) <= 64,
               "varintDictStats exceeds single cache line (64 bytes)! "
               "Keep statistics struct cache-friendly.");

int varintDictGetStats(const uint64_t *values, size_t count,
                       varintDictStats *stats);

__END_DECLS
