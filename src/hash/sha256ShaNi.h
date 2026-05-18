#pragma once

/* SHA-256 (FIPS 180-4) — Intel SHA Extensions (SHA-NI) accelerated compressor.
 *
 * Exposes a block-oriented compression primitive that processes N contiguous
 * 64-byte input blocks using the sha256rnds2/sha256msg1/sha256msg2 instructions
 * (available on Goldmont, Cannon Lake+, Ice Lake+, and AMD Zen+).
 *
 * Padding/finalization is the caller's responsibility (see sha256.c for the
 * portable reference flow). Runtime CPUID detection lets callers cleanly fall
 * back to the portable scalar path on unsupported hardware. */

#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Returns true iff the current CPU advertises SHA-NI support. Safe to call
 * from any thread; result is computed via __builtin_cpu_supports("sha"). */
bool sha256ShaNiAvailable(void);

/* Compress `num_blocks` consecutive 512-bit blocks of `blocks` into `state`.
 * `state` holds H0..H7 in sequential big-endian-32 logical order, matching
 * sha256Ctx::state. No padding is added; pass only fully-formed blocks. */
void sha256ShaNiCompressBlocks(uint32_t state[8], const uint8_t *blocks,
                               size_t num_blocks);

/* Four-lane multi-buffer compressor: compresses `num_blocks` independent
 * 64-byte blocks from each of `blocks[0..3]` into the corresponding
 * `states[0..3]`.  Mirrors the ARM `sha256ArmCryptoCompressBlocksMulti4`
 * API so the HMAC batch-of-four path can dispatch by platform.
 *
 * On x86 with SHA-NI: issues each lane's sha256rnds2 chain into the OoO
 * core in round-robin order — Skylake-X+ and Zen3+ can retire 1 rnds2
 * per cycle and have enough issue width to overlap 2-4 independent
 * lanes, so the multi-buffer path nets a measurable speedup over four
 * serial calls.  On hosts without SHA-NI (or non-x86): delegates to
 * four serial sha256PortableCompressBlocks calls so the symbol is
 * stable and correctness is preserved. */
void sha256ShaNiCompressBlocksMulti4(uint32_t states[4][8],
                                     const uint8_t *const blocks[4],
                                     size_t num_blocks);

#ifdef DATAKIT_TEST
int sha256ShaNiTest(int argc, char *argv[]);
#endif
