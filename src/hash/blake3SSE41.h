#pragma once

/* BLAKE3 - x86 SSE4.1 accelerated compression.
 *
 * Provides 4-way SIMD single-block compression primitives that are
 * functionally identical to the portable scalar versions in blake3.c.
 * The implementation vectorizes the four G operations of each round
 * across SSE 128-bit lanes (4x uint32_t per __m128i), with diagonal-step
 * lane rotations performed via _mm_shuffle_epi32.
 *
 * At runtime, blake3SSE41Available() reports whether the SSE4.1 path is
 * compiled in AND supported by the current CPU. On non-x86 hosts the
 * file still compiles cleanly but the implementations are stubs that
 * fall back to the portable scalar path. */

#include <stdint.h>

#include "blake3Internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Number of independent chunks processed per blake3SSE41Compress4 call.
 * Tied at compile time to BLAKE3_BATCH_SSE41_CHUNKS in blake3.c via
 * static_assert — if a future implementation widens the SSE4.1 batch,
 * bump this constant AND the matching one in blake3.c together. */
#define BLAKE3_SSE41_COMPRESS4_LANES 4

/* True iff this build includes the SSE4.1 code path AND the running CPU
 * advertises SSE4.1 support. Safe to call on any platform. */
bool blake3SSE41Available(void);

/* Single-block in-place compress. Behaves identically to the portable
 * compress_in_place in blake3.c. */
void blake3SSE41CompressInPlace(uint32_t cv[8],
                                const uint8_t block[BLAKE3_BLOCK_LEN],
                                uint8_t block_len, uint64_t counter,
                                uint8_t flags);

/* XOF variant: produces the 64-byte expanded compression output. */
void blake3SSE41CompressXof(const uint32_t cv[8],
                            const uint8_t block[BLAKE3_BLOCK_LEN],
                            uint8_t block_len, uint64_t counter, uint8_t flags,
                            uint8_t out[64]);

/* 4-way batch compression: compress four independent (cv, block,
 * block_len, counter, flags) tuples in parallel and write four 32-byte
 * truncated output CVs.  Mirrors blake3NeonCompress4 — each
 * cv_inputs[i] points to 8 uint32_t words; each blocks[i] points to
 * BLAKE3_BLOCK_LEN bytes. State words live one-per-vector across 4
 * chunks (transposed layout). */
void blake3SSE41Compress4(const uint32_t *const cv_inputs[4],
                          const uint8_t *const blocks[4],
                          const uint8_t block_lens[4],
                          const uint64_t counters[4], const uint8_t flag_set[4],
                          uint8_t out_cvs[4][32]);

#ifdef DATAKIT_TEST
int blake3SSE41Test(int argc, char *argv[]);
#endif

#ifdef __cplusplus
}
#endif
