#pragma once

/* Poly1305 — ARM NEON 2-block batched MAC accelerator.
 *
 * Speeds up the bulk-block phase of Poly1305 by processing 2 message
 * blocks per iteration with precomputed r^2.  The 5x26-bit limb
 * accumulator and r-key state are identical to the scalar reference
 * (chacha20poly1305.c `poly1305State`); only the `Block2x` entry point
 * is SIMD-accelerated.  The dispatcher in chacha20poly1305.c invokes
 * this entry point when the runtime probe poly1305NeonAvailable()
 * returns true AND at least 2 full 16-byte blocks of message remain.
 *
 * The function operates strictly on the 5x26-bit limb state already
 * held by the caller; the caller is responsible for the partial-block
 * tail (which goes through scalar Block).
 *
 * Output parity vs the scalar poly1305Block is enforced by
 * dkTestPoly1305SimdParity in the test driver.
 *
 * On non-aarch64 hosts the symbols are stubs: Available() returns
 * false; Block2x() asserts (callers must gate on the probe).  This
 * matches the pattern used by hash/aesGcmArmCrypto.c and
 * hash/chacha20Neon.c. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true iff the current CPU has ARM NEON available.  Apple
 * Silicon and all AArch64 always advertise NEON.  On non-aarch64 hosts
 * this always returns false. */
bool poly1305NeonAvailable(void);

/* Process pairs of 16-byte blocks of `msg` (length must be a multiple
 * of 32) into the 5x26-bit accumulator `h[0..4]` using key limbs
 * `r[0..4]`.  Each block adds the 2^128 hibit marker (1<<24 in limb 4)
 * since these are full blocks.
 *
 * `h` is the running accumulator state (5x 26-bit limbs in uint64_t
 * for headroom).  Modified in place.
 * `r` are the clamped 5x 26-bit limbs from poly1305Init.
 * `r2` are the precomputed 5x 26-bit limbs of r^2 mod p.  Caller is
 *      responsible for precomputing these once after poly1305Init via
 *      poly1305NeonPrecomputeR2.
 * `msg` is the message bytes (length 32, 64, ...).
 * `num_pairs` is the number of 2-block pairs to consume (i.e. msg has
 *      32 * num_pairs bytes).  Must be >= 1.
 *
 * Postcondition: h is fully updated to reflect 2*num_pairs message
 * blocks consumed; no internal buffer state changes. */
void poly1305NeonBlock2x(uint64_t h[5], const uint32_t r[5],
                         const uint32_t r2[5], const uint8_t *msg,
                         size_t num_pairs);

/* Precompute r^2 mod p as 5x 26-bit limbs given r.  Trivially done
 * scalar (one square) — provided here so the dispatcher and tests use
 * the exact same arithmetic. */
void poly1305NeonPrecomputeR2(const uint32_t r[5], uint32_t r2[5]);

#ifdef __cplusplus
} /* extern "C" */
#endif
