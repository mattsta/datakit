#pragma once

/* SHA-256 internals shared by sha256.c and its hardware backends
 * (src/hash/): the portable block compression the backends answer with
 * and are tested against, and the compressors' shapes */

#include "sha256.h"

#include <stddef.h>
#include <stdint.h>

/* Portable bulk-compress for SIMD variants to delegate to when the
 * SIMD path is unavailable.  Updates `state` in place by compressing
 * `num_blocks` consecutive 64-byte blocks from `blocks`.  Caller is
 * responsible for initialization, partial-block buffering, and length
 * padding — this is the inner primitive only. */
void sha256PortableCompressBlocks(uint32_t state[8], const uint8_t *blocks,
                                  size_t num_blocks);

/* FIPS 180-4's initial hash value H(0): the first 32 bits of the
 * fractional parts of the square roots of the first eight primes */
static constexpr uint32_t sha256InitialHash[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

/* A block compressor: folds `num_blocks` 64-byte blocks into `state`
 * (sha256PortableCompressBlocks, or a hardware backend's) */
typedef void sha256CompressBlocksFn(uint32_t state[8], const uint8_t *blocks,
                                    size_t num_blocks);

/* The SHA-256 of data[0, len) with every block compressed by `compress`:
 * one backend's whole-message digest, for comparing backends */
void sha256HashWith(sha256CompressBlocksFn *compress, const void *data,
                    size_t len, uint8_t out[SHA256_DIGEST_LEN]);

/* A four-lane block compressor: lane i folds `num_blocks` blocks from
 * blocks[i] into states[i] (sha256ShaNiCompressBlocksMulti4,
 * sha256ArmCryptoCompressBlocksMulti4) */
typedef void sha256CompressBlocksMulti4Fn(uint32_t states[4][8],
                                          const uint8_t *const blocks[4],
                                          size_t num_blocks);

#ifdef DATAKIT_TEST
/* For each backend's test: its one-lane and four-lane compressors
 * against the portable one over replayable random states and blocks,
 * and its whole-message digests (lengths 0..130, padding included)
 * against sha256Hash.  Runs whether the backend is native here or its
 * portable stub; returns the failures, each reported under 'name'. */
int sha256TestBackendAgainstPortable(const char *name,
                                     sha256CompressBlocksFn *one,
                                     sha256CompressBlocksMulti4Fn *four);
#endif
