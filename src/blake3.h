#pragma once

/* BLAKE3 cryptographic hash — portable C implementation.
 *
 * Public API mirrors the reference BLAKE3 library: hasher init (plain,
 * keyed, derive-key), incremental update, and (seekable) finalize.
 * Internally this is a single-threaded, scalar implementation; SIMD
 * variants build on the same struct/API. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLAKE3_KEY_LEN 32
#define BLAKE3_OUT_LEN 32
#define BLAKE3_BLOCK_LEN 64
#define BLAKE3_CHUNK_LEN 1024
#define BLAKE3_MAX_DEPTH 54
#define BLAKE3_MAX_SIMD_DEGREE 1

typedef struct blake3Hasher blake3Hasher;
struct blake3Hasher {
    /* Largest members first to minimize padding. */
    uint8_t cv_stack[(BLAKE3_MAX_DEPTH + 1) * 32];
    uint8_t buf[BLAKE3_BLOCK_LEN];
    /* When the most recent push folded all the way to cv_stack_len=1,
     * these hold the (left, right) CV pair that was the input to that
     * final fold.  Finalize uses them to re-derive the root output
     * with the ROOT flag applied — the eager-folded CV on the stack
     * has already discarded its compression inputs.  Valid only when
     * pending_root_valid is true. */
    uint8_t pending_root_left[32];
    uint8_t pending_root_right[32];
    uint64_t chunk_counter;
    uint32_t key[8];
    uint32_t cv[8];
    uint8_t buf_len;
    uint8_t blocks_compressed;
    uint8_t flags;
    uint8_t cv_stack_len;
    uint8_t pending_root_valid;
};

void blake3HasherInit(blake3Hasher *self);
void blake3HasherInitKeyed(blake3Hasher *self,
                           const uint8_t key[BLAKE3_KEY_LEN]);
void blake3HasherInitDeriveKey(blake3Hasher *self, const char *context);
void blake3HasherInitDeriveKeyRaw(blake3Hasher *self, const void *context,
                                  size_t context_len);
void blake3HasherUpdate(blake3Hasher *self, const void *input,
                        size_t input_len);
void blake3HasherFinalize(const blake3Hasher *self, uint8_t *out,
                          size_t out_len);
void blake3HasherFinalizeSeek(const blake3Hasher *self, uint64_t seek,
                              uint8_t *out, size_t out_len);

void blake3Hash(const void *input, size_t input_len,
                uint8_t out[BLAKE3_OUT_LEN]);
void blake3HashKeyed(const uint8_t key[BLAKE3_KEY_LEN], const void *input,
                     size_t input_len, uint8_t out[BLAKE3_OUT_LEN]);

/* One-shot BLAKE3 derive-key KDF.  Produces `out_len` bytes of pseudo-
 * random key material from `ikm` (input keying material — typically a
 * long-term root secret) bound to a globally unique `context` byte
 * sequence.
 *
 * The context is hashed under the DERIVE_KEY_CONTEXT flag; the result
 * keys the DERIVE_KEY_MATERIAL hash over `ikm`.  Calling this twice
 * with different contexts but the same `ikm` yields independent keys
 * — the canonical use case is "derive one sub-key per (component,
 * purpose) pair from a single high-entropy root".
 *
 * Context conventions (recommended):
 *   - Encode the application name, version, and purpose as bytes, e.g.
 *     b"datakit clusterMailbox 2026-05 origin-key".
 *   - Context bytes are NOT secret; they are protocol identifiers.
 *   - Reusing a context across protocol versions invites key reuse
 *     across incompatible code paths — include a version stamp.
 *
 * All inputs are opaque (ptr, len) byte ranges.  No NUL handling. */
void blake3DeriveKey(const void *context, size_t context_len, const void *ikm,
                     size_t ikm_len, uint8_t *out, size_t out_len);

/* Force a specific implementation for benchmarking and validation.
 * Default is AUTO — the dispatcher picks whatever is fastest based
 * on the measured crossover threshold.  Override only for direct A/B
 * measurement; production callers should leave the default in place. */
typedef enum blake3ForceImpl {
    BLAKE3_IMPL_AUTO = 0,   /* default: dispatch by size threshold */
    BLAKE3_IMPL_SCALAR = 1, /* always use portable compress, never SIMD */
    BLAKE3_IMPL_SIMD = 2,   /* always engage the SIMD batch path when
                             * input size makes it geometrically valid
                             * (>= 4 chunks); below that, falls back
                             * to scalar (no narrower SIMD primitive
                             * is available for 1-3 chunk inputs) */
} blake3ForceImpl;

void blake3SetForceImpl(blake3ForceImpl impl);
blake3ForceImpl blake3GetForceImpl(void);

/* Backend selected by the dispatcher (introspection for tests).
 * Reports the SIMD batch backend that would be chosen NOW given
 * the runtime CPU capabilities and the current force-impl knob.
 * Note: this answers "what would be picked for a large-enough input"
 * — for inputs below the batch threshold the dispatcher falls back
 * to scalar regardless of which backend is reported here. */
typedef enum blake3Backend {
    BLAKE3_BACKEND_SCALAR = 0,
    BLAKE3_BACKEND_NEON = 1,
    BLAKE3_BACKEND_AVX2 = 2,
    BLAKE3_BACKEND_SSE41 = 3,
} blake3Backend;

blake3Backend blake3DispatcherActiveBackend(void);

/* Portable compression primitives exposed for SIMD variants (NEON,
 * SSE4.1, AVX2) to differential-test against.  These are the same
 * scalar code paths the public API uses internally. */
void blake3PortableCompressInPlace(uint32_t cv[8],
                                   const uint8_t block[BLAKE3_BLOCK_LEN],
                                   uint8_t block_len, uint64_t counter,
                                   uint8_t flags);
void blake3PortableCompressXof(const uint32_t cv[8],
                               const uint8_t block[BLAKE3_BLOCK_LEN],
                               uint8_t block_len, uint64_t counter,
                               uint8_t flags, uint8_t out[64]);

#ifdef DATAKIT_TEST
int blake3Test(int argc, char *argv[]);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif
