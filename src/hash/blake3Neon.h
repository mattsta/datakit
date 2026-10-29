#pragma once

/* BLAKE3 — ARM NEON accelerated compression.
 *
 * Provides 4-way SIMD compression primitives that are functionally
 * identical to the portable scalar versions in blake3.c. The single-
 * block path vectorizes the four G operations of each round across
 * NEON 128-bit lanes; the 4-way batched path hashes four independent
 * 64-byte blocks in parallel (one chunk per NEON lane).
 *
 * At runtime, blake3NeonAvailable() reports whether the NEON path is
 * compiled in. On non-NEON hosts the file still compiles but the
 * implementations are stubs. */

#include <stdint.h>

#include "blake3Internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Number of independent chunks processed per blake3NeonCompress4 call.
 * Tied at compile time to BLAKE3_BATCH_NEON_CHUNKS in blake3.c via
 * static_assert — if a future implementation widens the NEON batch,
 * bump this constant AND the matching one in blake3.c together. */
#define BLAKE3_NEON_COMPRESS4_LANES 4

/* True iff this build includes the NEON code path. */
bool blake3NeonAvailable(void);

/* Single-block in-place compress. Behaves identically to the portable
 * compress_in_place in blake3.c. */
void blake3NeonCompressInPlace(uint32_t cv[8],
                               const uint8_t block[BLAKE3_BLOCK_LEN],
                               uint8_t block_len, uint64_t counter,
                               uint8_t flags);

/* XOF variant: produces the 64-byte expanded compression output. */
void blake3NeonCompressXof(const uint32_t cv[8],
                           const uint8_t block[BLAKE3_BLOCK_LEN],
                           uint8_t block_len, uint64_t counter, uint8_t flags,
                           uint8_t out[64]);

/* 4-way batch compression: compress four independent (cv, block,
 * block_len, counter, flags) tuples in parallel and write four 32-byte
 * truncated output CVs.  Each cv_inputs[i] points to 8 uint32_t words;
 * each blocks[i] points to BLAKE3_BLOCK_LEN bytes. */
void blake3NeonCompress4(const uint32_t *const cv_inputs[4],
                         const uint8_t *const blocks[4],
                         const uint8_t block_lens[4],
                         const uint64_t counters[4], const uint8_t flag_set[4],
                         uint8_t out_cvs[4][32]);

#ifdef DATAKIT_TEST
int blake3NeonTest(int argc, char *argv[]);
#endif

#ifdef __cplusplus
}
#endif
