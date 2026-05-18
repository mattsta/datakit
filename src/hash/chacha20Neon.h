#pragma once

/* ChaCha20 — ARM NEON 4-block batched keystream generator.
 *
 * Produces 4 independent ChaCha20 keystream blocks (256 bytes total)
 * per call using the state-per-lane layout: each 128-bit NEON vector
 * holds the same state word across 4 parallel blocks. This is the
 * standard floodyberry/sodium-style horizontal SIMD pattern.
 *
 * The dispatcher in chacha20poly1305.c invokes this entry point when
 * the runtime probe chacha20NeonAvailable() returns true AND at least
 * 256 bytes of keystream remain to be produced.  Otherwise it falls
 * back to the scalar chacha20Block per-block.
 *
 * On non-aarch64 hosts the symbols are stubs: Available() returns
 * false; Block4() asserts (callers must gate on the probe).  This
 * matches the pattern used by hash/aesGcmArmCrypto.c and
 * hash/blake3Neon.c. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true iff the current CPU has ARM NEON available.  Apple
 * Silicon and all AArch64 always advertise NEON.  On non-aarch64 hosts
 * this always returns false. */
bool chacha20NeonAvailable(void);

/* Produce 4 ChaCha20 keystream blocks (256 bytes) given a 16-word
 * starting state.  `state` is the input state vector in the same form
 * as the scalar chacha20Block: 4 constant words, 8 key words, 1
 * counter word, 3 nonce words (16 x uint32_t = 64 bytes).  The four
 * generated blocks use counter values state[12], state[12]+1, +2, +3.
 *
 * `out` MUST point to at least 256 bytes (4 x 64).  The 4 blocks are
 * laid out contiguously: block 0 in bytes [0..63], block 1 in [64..127],
 * block 2 in [128..191], block 3 in [192..255]. */
void chacha20NeonBlock4(const uint32_t state[16], uint8_t out[256]);

#ifdef __cplusplus
} /* extern "C" */
#endif
