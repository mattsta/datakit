#pragma once

/* Poly1305 — x86 AVX2 4-block batched MAC accelerator.
 *
 * Speeds up the bulk-block phase of Poly1305 by processing 4 message
 * blocks per iteration with precomputed r, r^2, r^3, r^4.  The
 * 5x26-bit limb accumulator and r-key state are identical to the
 * scalar reference (chacha20poly1305.c `poly1305State`); only the
 * `Block4x` entry point is SIMD-accelerated.  The dispatcher in
 * chacha20poly1305.c invokes this entry point when the runtime probe
 * poly1305AVX2Available() returns true AND at least 4 full 16-byte
 * blocks of message remain.
 *
 * Per-function __attribute__((target("avx2"))) lets us build on hosts
 * whose baseline ISA is below AVX2; the dispatcher will only invoke
 * Block4x after a positive runtime probe.
 *
 * Output parity vs the scalar poly1305Block is enforced by
 * dkTestPoly1305SimdParity in the test driver.
 *
 * On non-x86_64 hosts the symbols are stubs: Available() returns
 * false; Block4x() asserts (callers must gate on the probe).  This
 * matches the pattern used by hash/aesGcmNi.c and hash/chacha20AVX2.c. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true iff the current CPU exposes AVX2 (via cpuid).  Always
 * false on non-x86_64 builds. */
bool poly1305AVX2Available(void);

/* Process groups of 4x 16-byte blocks of `msg` (length must be a
 * multiple of 64) into the 5x26-bit accumulator `h[0..4]` using key
 * limbs `r[0..4]` and precomputed powers `r2`, `r3`, `r4` (all 5x
 * 26-bit limbs of r^k mod p).  Each block adds the 2^128 hibit marker
 * (1<<24 in limb 4) since these are full blocks.
 *
 * `h` is the running accumulator state (5x 26-bit limbs in uint64_t
 * for headroom).  Modified in place.
 * `r`, `r2`, `r3`, `r4` are the clamped key power limbs.  Caller is
 *      responsible for precomputing r2..r4 once after poly1305Init via
 *      poly1305AVX2PrecomputePowers.
 * `msg` is the message bytes (length 64, 128, ...).
 * `num_groups` is the number of 4-block groups to consume (i.e. msg
 *      has 64 * num_groups bytes).  Must be >= 1.
 *
 * Postcondition: h is fully updated to reflect 4*num_groups message
 * blocks consumed; no internal buffer state changes. */
void poly1305AVX2Block4x(uint64_t h[5], const uint32_t r[5],
                         const uint32_t r2[5], const uint32_t r3[5],
                         const uint32_t r4[5], const uint8_t *msg,
                         size_t num_groups);

/* Precompute r^2, r^3, r^4 mod p as 5x 26-bit limbs given r.  Trivially
 * done scalar (three multiplies) — provided here so the dispatcher and
 * tests use the exact same arithmetic. */
void poly1305AVX2PrecomputePowers(const uint32_t r[5], uint32_t r2[5],
                                  uint32_t r3[5], uint32_t r4[5]);

#ifdef __cplusplus
} /* extern "C" */
#endif
