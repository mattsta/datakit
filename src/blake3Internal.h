#pragma once

/* BLAKE3 internals shared by blake3.c and its SIMD backends (src/hash/):
 * the portable compression the backends answer with and are tested
 * against, and the backends' entry-point shapes */

#include "blake3.h"

#include <stddef.h>
#include <stdint.h>

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

/* The 'n' compresses a batched SIMD compress does, one at a time through
 * the portable path: pair i's CV as 32 little-endian bytes in
 * out_cvs[i].  The batch entry points of a backend this build lacks
 * answer with it. */
void blake3PortableCompressBatch(size_t n, const uint32_t *const cv_inputs[],
                                 const uint8_t *const blocks[],
                                 const uint8_t block_lens[],
                                 const uint64_t counters[],
                                 const uint8_t flag_set[],
                                 uint8_t out_cvs[][32]);

/* A backend's three compress entry points, as each SIMD file exports
 * them: in place, XOF, and a batch of lanes whose CVs come back as 32
 * little-endian bytes each (blake3NeonCompress4, blake3SSE41Compress4,
 * blake3AVX2Compress8) */
typedef void blake3CompressInPlaceFn(uint32_t cv[8],
                                     const uint8_t block[BLAKE3_BLOCK_LEN],
                                     uint8_t block_len, uint64_t counter,
                                     uint8_t flags);
typedef void blake3CompressXofFn(const uint32_t cv[8],
                                 const uint8_t block[BLAKE3_BLOCK_LEN],
                                 uint8_t block_len, uint64_t counter,
                                 uint8_t flags, uint8_t out[64]);
typedef void blake3BatchCompressFn(const uint32_t *const cv_inputs[],
                                   const uint8_t *const blocks[],
                                   const uint8_t block_lens[],
                                   const uint64_t counters[],
                                   const uint8_t flag_set[],
                                   uint8_t out_cvs[][32]);

#ifdef DATAKIT_TEST
/* For each backend's test: its three entry points against the portable
 * path over replayable random CVs, blocks (every length 0..64),
 * counters and flags, 'lanes' wide for the batch.  Runs whether the
 * backend is native here or its portable stub; returns the failures,
 * each reported under 'name'. */
int blake3TestBackendAgainstPortable(const char *name,
                                     blake3CompressInPlaceFn *inPlace,
                                     blake3CompressXofFn *xof, size_t lanes,
                                     blake3BatchCompressFn *batch);

/* One compress input drawn from the replayable stream at '*state': a
 * chaining value, a block, a counter, a block length of 1..64 and flags
 * below 0x80.  The backend tests' shared source of inputs. */
void blake3TestDrawInput(uint64_t *state, uint32_t cv[8],
                         uint8_t block[BLAKE3_BLOCK_LEN], uint64_t *counter,
                         uint8_t *blockLen, uint8_t *flags);
#endif
