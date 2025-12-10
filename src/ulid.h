#pragma once

#include "datakit.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ====================================================================
 * ULID - Universally Unique Lexicographically Sortable Identifier
 * ==================================================================== */
/* A modern ULID implementation with Base36 encoding.
 *
 * Format: 128-bit value with:
 * - 64 bits for nanosecond timestamp (lexicographically sortable)
 * - 64 bits for randomness (collision-resistant)
 *
 * Encoded as 25-character Base36 string (0-9A-Z).
 * Base36 is more compact than Base32 (25 vs 32 characters).
 *
 * Features:
 * - Multiple encoding implementations: scalar, SWAR, SSE2, AVX2, NEON
 * - Runtime SIMD capability detection
 * - Thread-safe generation with monotonic counter
 * - Full encode/decode roundtrip support
 */

/* Base36 encoded length: ceil(128 / log2(36)) = 25 characters */
#define ULID_ENCODED_LENGTH 25
#define ULID_BINARY_LENGTH 16

/* Base36 encoded length for 64-bit: 13 characters (36^13 > 2^64) */
#define ULID64_ENCODED_LENGTH 13
#define ULID64_BINARY_LENGTH 8

/* Base36 encoded length for 32-bit: 7 characters */
#define ULID32_ENCODED_LENGTH 7
#define ULID32_BINARY_LENGTH 4

/* ULID binary representation */
typedef struct ulid {
    uint8_t data[ULID_BINARY_LENGTH]; /* 128-bit identifier in big-endian */
} ulid_t;

/* 64-bit ULID variant */
typedef struct ulid64 {
    uint64_t data;
} ulid64_t;

/* 32-bit ULID variant */
typedef struct ulid32 {
    uint32_t data;
} ulid32_t;

/* Encoding implementation types for benchmarking */
typedef enum ulidEncodeImpl {
    ULID_ENCODE_SCALAR = 0, /* Pure scalar implementation */
    ULID_ENCODE_SWAR,       /* SWAR (SIMD Within A Register) */
    ULID_ENCODE_SSE2,       /* SSE2 intrinsics */
    ULID_ENCODE_AVX2,       /* AVX2 intrinsics */
    ULID_ENCODE_NEON,       /* ARM NEON intrinsics */
    ULID_ENCODE_AUTO,       /* Auto-select best available */
    ULID_ENCODE_COUNT
} ulidEncodeImpl;

/* ====================================================================
 * Core API
 * ==================================================================== */

/* Generate a new ULID with nanosecond timestamp + randomness */
void ulidGenerate(ulid_t *out);

/* Encode ULID to Base36 string (25 chars + null terminator)
 * Returns length written (25) on success, 0 on error */
size_t ulidEncode(const ulid_t *ulid, char *out, size_t outLen);

/* Decode Base36 string to ULID
 * Returns true on success, false on invalid input */
bool ulidDecode(const char *str, ulid_t *out);

/* Extract nanosecond timestamp from ULID */
uint64_t ulidGetTimestampNs(const ulid_t *ulid);

/* Extract randomness component from ULID */
uint64_t ulidGetRandom(const ulid_t *ulid);

/* Convenience: generate and encode in one call */
size_t ulidGenerateAndEncode(char *out, size_t outLen);

/* ====================================================================
 * Implementation Selection API
 * ==================================================================== */

/* Get the currently selected encoding implementation */
ulidEncodeImpl ulidGetEncodeImpl(void);

/* Set the encoding implementation (for benchmarking)
 * Returns false if the requested implementation is not available */
bool ulidSetEncodeImpl(ulidEncodeImpl impl);

/* Check if an implementation is available on this platform */
bool ulidIsImplAvailable(ulidEncodeImpl impl);

/* Get human-readable name for implementation */
const char *ulidGetImplName(ulidEncodeImpl impl);

/* ====================================================================
 * Specific Implementation APIs (for benchmarking)
 * ==================================================================== */

/* Scalar (portable) implementation */
size_t ulidEncodeScalar(const ulid_t *ulid, char *out, size_t outLen);
bool ulidDecodeScalar(const char *str, ulid_t *out);

/* SWAR (SIMD Within A Register) implementation */
size_t ulidEncodeSWAR(const ulid_t *ulid, char *out, size_t outLen);
bool ulidDecodeSWAR(const char *str, ulid_t *out);

/* SSE2 implementation (x86/x64) */
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#define ULID_HAS_SSE2 1
size_t ulidEncodeSSE2(const ulid_t *ulid, char *out, size_t outLen);
bool ulidDecodeSSE2(const char *str, ulid_t *out);
#else
#define ULID_HAS_SSE2 0
#endif

/* AVX2 implementation (x86/x64) */
#if defined(__AVX2__)
#define ULID_HAS_AVX2 1
size_t ulidEncodeAVX2(const ulid_t *ulid, char *out, size_t outLen);
bool ulidDecodeAVX2(const char *str, ulid_t *out);
#else
#define ULID_HAS_AVX2 0
#endif

/* NEON implementation (ARM) */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#define ULID_HAS_NEON 1
size_t ulidEncodeNEON(const ulid_t *ulid, char *out, size_t outLen);
bool ulidDecodeNEON(const char *str, ulid_t *out);
#else
#define ULID_HAS_NEON 0
#endif

/* ====================================================================
 * Comparison and Utility
 * ==================================================================== */

/* Compare two ULIDs (returns -1, 0, or 1 like memcmp) */
int ulidCompare(const ulid_t *a, const ulid_t *b);

/* Check if ULID is zero (null ULID) */
bool ulidIsZero(const ulid_t *ulid);

/* Clear/zero a ULID */
void ulidClear(ulid_t *ulid);

/* Copy a ULID */
void ulidCopy(ulid_t *dst, const ulid_t *src);

/* ====================================================================
 * ULID Variants - Compact Time-Ordered IDs
 * ==================================================================== */

/* Variant type enumeration */
typedef enum ulidVariantType {
    /* Epoch-offset variants (extended range) */
    ULID_VARIANT_EPOCH2020,   /* 48-bit ns offset from 2020 + 16-bit random */
    ULID_VARIANT_EPOCH2024,   /* 48-bit ns offset from 2024 + 16-bit random */
    ULID_VARIANT_EPOCHCUSTOM, /* 48-bit ns offset from custom + 16-bit random */

    /* Precision trade-off variants */
    ULID_VARIANT_NS, /* 52-bit ns + 12-bit random */
    ULID_VARIANT_US, /* 46-bit us + 18-bit random */
    ULID_VARIANT_MS, /* 42-bit ms + 22-bit random */

    /* Creative/specialized variants */
    ULID_VARIANT_DUALNS, /* 64-bit ns + 64-bit ns (128-bit dual timestamp) */
    ULID_VARIANT_DUALNS_INTERLEAVED, /* 128-bit bit-interleaved dual ns
                                        timestamps */
    ULID_VARIANT_NSCOUNT,            /* 40-bit ns + 24-bit counter */
    ULID_VARIANT_HYBRID,             /* 44-bit us + 20-bit ns delta */
    ULID_VARIANT_SNOWFLAKE, /* 41-bit ms + 10-bit machine + 13-bit seq */

    /* 32-bit compact variants */
    ULID_VARIANT_32MS, /* 26-bit ms + 6-bit random */
    ULID_VARIANT_32S,  /* 22-bit s + 10-bit random */

    ULID_VARIANT_COUNT
} ulidVariantType;

/* Variant configuration */
typedef struct ulidVariantConfig {
    ulidVariantType type;
    uint64_t customEpochNs; /* For EPOCHCUSTOM variant */
    uint16_t machineId;     /* For SNOWFLAKE variant (0-1023) */
} ulidVariantConfig;

/* Epoch offsets in nanoseconds since Unix epoch */
#define ULID_EPOCH_2020_NS                                                     \
    1577836800000000000ULL /* 2020-01-01 00:00:00 UTC                          \
                            */
#define ULID_EPOCH_2024_NS                                                     \
    1704067200000000000ULL /* 2024-01-01 00:00:00 UTC                          \
                            */

/* ====================================================================
 * 64-bit Variant API
 * ==================================================================== */

/* Generate ULID64 with specific variant type (uses defaults) */
void ulid64Generate(ulid64_t *out, ulidVariantType type);

/* Generate ULID64 with custom configuration */
void ulid64GenerateWithConfig(ulid64_t *out, const ulidVariantConfig *config);

/* Encode ULID64 to Base36 string (13 chars + null terminator) */
size_t ulid64Encode(const ulid64_t *id, char *out, size_t outLen);

/* Decode Base36 string to ULID64 */
bool ulid64Decode(const char *str, ulid64_t *out);

/* Extract timestamp from ULID64 (returns nanoseconds since Unix epoch) */
uint64_t ulid64GetTimestampNs(const ulid64_t *id, ulidVariantType type);

/* Extract random/sequence component from ULID64 */
uint64_t ulid64GetRandom(const ulid64_t *id, ulidVariantType type);

/* Convenience: generate and encode in one call */
size_t ulid64GenerateAndEncode(char *out, size_t outLen, ulidVariantType type);

/* ====================================================================
 * 128-bit DUALNS Variant API
 * ==================================================================== */

/* Generate DUALNS (128-bit: two sequential 64-bit nanosecond timestamps) */
void ulidGenerateDualNs(ulid_t *out);

/* Generate DUALNS and return as __uint128_t (high 64 = ts1, low 64 = ts2) */
__uint128_t ulidGenerateDualNsU128(void);

/* Create DUALNS from __uint128_t value */
void ulidFromDualNsU128(ulid_t *out, __uint128_t value);

/* Extract DUALNS as __uint128_t (high 64 = ts1, low 64 = ts2) */
__uint128_t ulidToDualNsU128(const ulid_t *id);

/* Extract first timestamp from DUALNS variant (high 64 bits) */
uint64_t ulidGetFirstTimestampNs(const ulid_t *id);

/* Extract second timestamp from DUALNS variant (low 64 bits) */
uint64_t ulidGetSecondTimestampNs(const ulid_t *id);

/* ====================================================================
 * 128-bit DUALNS_INTERLEAVED Variant API
 * ==================================================================== */

/* Generate DUALNS_INTERLEAVED (128-bit: bit-interleaved dual timestamps) */
void ulidGenerateDualNsInterleaved(ulid_t *out);

/* Generate DUALNS_INTERLEAVED and return as __uint128_t */
__uint128_t ulidGenerateDualNsInterleavedU128(void);

/* Create DUALNS_INTERLEAVED from two timestamps */
void ulidFromDualNsInterleaved(ulid_t *out, uint64_t ts1, uint64_t ts2);

/* Create DUALNS_INTERLEAVED from __uint128_t interleaved value */
void ulidFromDualNsInterleavedU128(ulid_t *out, __uint128_t interleaved);

/* Extract as __uint128_t interleaved value */
__uint128_t ulidToDualNsInterleavedU128(const ulid_t *id);

/* Deinterleave and extract first timestamp */
uint64_t ulidGetFirstTimestampNsInterleaved(const ulid_t *id);

/* Deinterleave and extract second timestamp */
uint64_t ulidGetSecondTimestampNsInterleaved(const ulid_t *id);

/* ====================================================================
 * Variant-Specific Extraction APIs (64-bit variants)
 * ==================================================================== */

/* Extract counter from NSCOUNT variant */
uint32_t ulid64GetCounter(const ulid64_t *id);

/* Extract machine ID from SNOWFLAKE variant */
uint16_t ulid64GetSnowflakeMachineId(const ulid64_t *id);

/* Extract sequence number from SNOWFLAKE variant */
uint16_t ulid64GetSnowflakeSequence(const ulid64_t *id);

/* Extract nanosecond delta from HYBRID variant */
uint32_t ulid64GetNsDelta(const ulid64_t *id);

/* ====================================================================
 * Configuration Helpers
 * ==================================================================== */

/* Initialize configuration with defaults for a given variant type */
void ulidVariantConfigInit(ulidVariantConfig *config, ulidVariantType type);

/* Validate configuration (checks epochs, machine IDs, etc.) */
bool ulidVariantConfigValidate(const ulidVariantConfig *config);

/* ====================================================================
 * 64-bit Utility Functions
 * ==================================================================== */

/* Compare two ULID64s (returns -1, 0, or 1 like memcmp) */
int ulid64Compare(const ulid64_t *a, const ulid64_t *b);

/* Check if ULID64 is zero (null ULID) */
bool ulid64IsZero(const ulid64_t *id);

/* Clear/zero a ULID64 */
void ulid64Clear(ulid64_t *id);

/* Copy a ULID64 */
void ulid64Copy(ulid64_t *dst, const ulid64_t *src);

/* ====================================================================
 * Variant Metadata API
 * ==================================================================== */

/* Get human-readable name for variant type */
const char *ulid64GetVariantName(ulidVariantType type);

/* Get detailed description of variant type */
const char *ulid64GetVariantDescription(ulidVariantType type);

/* Get maximum timestamp range in years for variant */
double ulid64GetVariantRangeYears(ulidVariantType type);

/* Get number of random bits for variant */
uint8_t ulid64GetVariantRandomBits(ulidVariantType type);

/* Get timestamp precision for variant (ns, us, ms, s) */
const char *ulid64GetVariantPrecision(ulidVariantType type);

/* ====================================================================
 * 32-bit Compact Variant API
 * ==================================================================== */

/* Generate ULID32 with specific variant type */
void ulid32Generate(ulid32_t *out, ulidVariantType type);

/* Encode ULID32 to Base36 string (7 chars + null terminator) */
size_t ulid32Encode(const ulid32_t *id, char *out, size_t outLen);

/* Decode Base36 string to ULID32 */
bool ulid32Decode(const char *str, ulid32_t *out);

/* Extract timestamp from ULID32 (returns nanoseconds since Unix epoch) */
uint64_t ulid32GetTimestampNs(const ulid32_t *id, ulidVariantType type);

/* Extract random component from ULID32 */
uint32_t ulid32GetRandom(const ulid32_t *id, ulidVariantType type);

/* Compare two ULID32s */
int ulid32Compare(const ulid32_t *a, const ulid32_t *b);

/* Check if ULID32 is zero */
bool ulid32IsZero(const ulid32_t *id);

/* Clear ULID32 */
void ulid32Clear(ulid32_t *id);

/* Copy ULID32 */
void ulid32Copy(ulid32_t *dst, const ulid32_t *src);

/* ====================================================================
 * Validation API
 * ==================================================================== */

/* Check if variant type is valid */
bool ulid64IsValidVariantType(ulidVariantType type);

/* Check if timestamp would overflow for given variant type */
bool ulid64ValidateTimestamp(uint64_t timestampNs, ulidVariantType type);

/* Get maximum safe timestamp for variant (nanoseconds since Unix epoch) */
uint64_t ulid64GetMaxTimestamp(ulidVariantType type);

#ifdef DATAKIT_TEST
int ulidTest(int argc, char *argv[]);
#endif
