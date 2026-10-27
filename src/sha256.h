#pragma once

/* SHA-256 (FIPS 180-4) — portable C implementation.
 *
 * Public API: incremental hashing via init/update/final, plus a one-shot
 * convenience wrapper. Output is the 32-byte big-endian message digest.
 * Implementation is scalar, endian-safe, and uses no heap allocation in
 * the hashing path. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHA256_DIGEST_LEN 32
#define SHA256_BLOCK_LEN 64

typedef struct sha256Ctx sha256Ctx;
struct sha256Ctx {
    /* Fields ordered largest -> smallest to minimize padding. */
    uint64_t bit_count;            /* total message length in bits */
    uint32_t state[8];             /* H0..H7 working hash state */
    uint8_t buf[SHA256_BLOCK_LEN]; /* partial-block buffer */
    uint32_t buf_len;              /* bytes currently in buf */
};

void sha256Init(sha256Ctx *ctx);
void sha256Update(sha256Ctx *ctx, const void *data, size_t len);
void sha256Final(sha256Ctx *ctx, uint8_t out[SHA256_DIGEST_LEN]);

/* One-shot convenience: equivalent to init + update + final. */
void sha256Hash(const void *data, size_t len, uint8_t out[SHA256_DIGEST_LEN]);

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

#ifdef DATAKIT_TEST
int sha256Test(int argc, char *argv[]);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif
