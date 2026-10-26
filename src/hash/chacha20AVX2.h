#pragma once

/* ChaCha20 — x86 AVX2 8-block batched keystream generator.
 *
 * Produces 8 independent ChaCha20 keystream blocks (512 bytes total)
 * per call using the state-per-lane layout: each 256-bit AVX2 vector
 * holds the same state word across 8 parallel blocks (lane i = block i,
 * 32-bit slots 0..7).  Every quarter-round is therefore lane-parallel
 * across 8 blocks at once.
 *
 * The dispatcher in chacha20poly1305.c invokes this entry point when
 * the runtime probe chacha20AVX2Available() returns true AND at least
 * 512 bytes of keystream remain.  Otherwise it falls back to the NEON
 * (on aarch64) or scalar paths.
 *
 * On non-x86_64 hosts the symbols are stubs: Available() returns
 * false and Block8() aborts, so it is declared [[noreturn]] there
 * (callers must gate on the probe). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__x86_64__)
#define CHACHA20_AVX2_BUILD 1
#define CHACHA20_AVX2_STUB_NORETURN
#else
#define CHACHA20_AVX2_BUILD 0
#define CHACHA20_AVX2_STUB_NORETURN [[noreturn]]
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true iff the current CPU exposes AVX2 (via cpuid).  Always
 * false on non-x86_64 builds. */
bool chacha20AVX2Available(void);

/* Produce 8 ChaCha20 keystream blocks (512 bytes) given a 16-word
 * starting state.  `state` is the input state vector in the same form
 * as the scalar chacha20Block: 4 constant words, 8 key words, 1
 * counter word, 3 nonce words.  The eight generated blocks use
 * counter values state[12], state[12]+1, ..., state[12]+7.
 *
 * `out` MUST point to at least 512 bytes (8 x 64).  Blocks are laid
 * out contiguously: block i in bytes [i*64 .. i*64+63]. */
CHACHA20_AVX2_STUB_NORETURN void chacha20AVX2Block8(const uint32_t state[16],
                                                    uint8_t out[512]);

#ifdef __cplusplus
} /* extern "C" */
#endif
