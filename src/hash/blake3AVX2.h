#pragma once

/* BLAKE3 compression — AVX2 acceleration for x86_64.
 *
 * The portable scalar BLAKE3 lives in blake3.c. This module provides
 * drop-in AVX2 replacements for the compression core. The headline
 * win is Compress8, which processes 8 independent (cv, block) inputs
 * in parallel using 256-bit registers laid out as 16 transposed lanes
 * — one lane per state word, eight chunks across each lane. */

#include <stdbool.h>
#include <stdint.h>

#include "blake3.h"

/* Number of independent chunks processed per blake3AVX2Compress8 call.
 * Tied at compile time to BLAKE3_BATCH_AVX2_CHUNKS in blake3.c via
 * static_assert — if a future implementation widens the AVX2 batch,
 * bump this constant AND the matching one in blake3.c together. */
#define BLAKE3_AVX2_COMPRESS8_LANES 8

/* Returns true when the runtime CPU exposes AVX2. Always false on
 * non-x86 builds. */
bool blake3AVX2Available(void);

/* Single-block compression, 8-word output (chunk/parent CV). Mirrors
 * the portable compress_in_place but runs the column/diagonal G
 * mixers using 128-bit lanes. */
void blake3AVX2CompressInPlace(uint32_t cv[8],
                               const uint8_t block[BLAKE3_BLOCK_LEN],
                               uint8_t block_len, uint64_t counter,
                               uint8_t flags);

/* Single-block compression, full 64-byte XOF output (used for root
 * and seekable output paths). */
void blake3AVX2CompressXof(const uint32_t cv[8],
                           const uint8_t block[BLAKE3_BLOCK_LEN],
                           uint8_t block_len, uint64_t counter, uint8_t flags,
                           uint8_t out[64]);

/* 8-way batch — hash 8 independent (cv, block) inputs in parallel.
 * This is AVX2's main advantage; the single-block path mostly matches
 * SSE4.1. Each output is a 32-byte CV in out_cvs[i].
 *
 * Inputs are passed as arrays of pointers so the caller doesn't need
 * to gather them into a contiguous buffer. */
void blake3AVX2Compress8(const uint32_t *const cv_inputs[8],
                         const uint8_t *const blocks[8],
                         const uint8_t block_lens[8],
                         const uint64_t counters[8], const uint8_t flag_set[8],
                         uint8_t out_cvs[8][32]);

#ifdef DATAKIT_TEST
int blake3AVX2Test(int argc, char *argv[]);
#endif
