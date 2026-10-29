#pragma once

/* SHA-256 compression accelerated via the ARMv8 Crypto Extensions
 * (sha256h / sha256h2 / sha256su0 / sha256su1).
 *
 * This is a drop-in alternative compressor for sha256.c.  Same state
 * representation (eight 32-bit chaining words in host byte order); the
 * caller is responsible for buffering and length-padding.  Targets
 * Apple Silicon and other aarch64 cores exposing FEAT_SHA256. */

#include "sha256Internal.h"

#include <stddef.h>
#include <stdint.h>

/* True at runtime iff this build has the ARMv8 crypto path AND the
 * host CPU supports it. */
bool sha256ArmCryptoAvailable(void);

/* Compress N full 64-byte blocks into ctx->state.  Caller must have
 * filled state[] with H0..H7 initial values (or chained from a
 * previous compress).  Updates state[] in place.  Does NOT touch
 * bit_count or buf — the wrapping sha256.c functions manage those. */
void sha256ArmCryptoCompressBlocks(uint32_t state[8], const uint8_t *blocks,
                                   size_t num_blocks);

/* Compress `num_blocks` full 64-byte blocks for each of four independent
 * SHA-256 messages in parallel.  Each lane has its own chaining state
 * (states[0..3]) and its own block pointer (blocks[0..3]).  `num_blocks`
 * is shared across all four lanes — callers must split work so every
 * lane supplies the same number of blocks per call.  The four streams
 * are interleaved through the round loop so the CPU can hide
 * vsha256h/vsha256h2 latency across all of them; expect ~2-3x
 * throughput vs. four serial single-buffer calls when four messages are
 * available concurrently.
 *
 * On platforms without the ARMv8 crypto extensions this falls back to
 * four serial sha256PortableCompressBlocks calls so the symbol is safe
 * to call everywhere. */
void sha256ArmCryptoCompressBlocksMulti4(uint32_t states[4][8],
                                         const uint8_t *const blocks[4],
                                         size_t num_blocks);

#ifdef DATAKIT_TEST
int sha256ArmCryptoTest(int argc, char *argv[]);
#endif
