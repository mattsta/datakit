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

#ifdef DATAKIT_TEST
int sha256Test(int argc, char *argv[]);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif
