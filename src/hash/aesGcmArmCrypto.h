#pragma once

/* AES-GCM — ARMv8 AES + PMULL accelerated backend (AArch64).
 *
 * Mirrors the public surface of aesGcm.{c,h} (and hash/aesGcmNi.h) so
 * the dispatcher in aesGcm.c can substitute this backend transparently
 * on Apple Silicon / Cortex-A cores that expose FEAT_AES and FEAT_PMULL.
 * The probe aesGcmArmCryptoAvailable() reports whether the host CPU
 * advertises the relevant crypto extensions; both AES (for the round
 * intrinsics) and PMULL (for the 64x64 carry-less multiply used by
 * GHASH) are required.
 *
 * On non-aarch64 hosts the file defines stubs so the Available() probe
 * is always linkable; callers gate execution on the boolean and never
 * reach the entry points unless the probe returns true.  This matches
 * the structure used by hash/aesGcmNi.c on x86 and hash/sha256ArmCrypto.c
 * for the SHA-256 case.
 *
 * No dynamic allocation: callers supply an `aesGcmKey` buffer (the
 * same struct used by the portable backend, defined in aesGcm.h) and
 * the ARM key schedule is laid out into the existing `round_keys`
 * array as raw 16-byte AES round-key blocks (10 or 14 of them plus
 * the initial round key) packed contiguously.  GHASH subkey `H` is
 * computed by encrypting the zero block under the freshly expanded
 * key and stored in the existing `h` field, pre-shifted by one bit in
 * the bit-reflected representation (matching the NI backend).
 *
 * The dispatcher checks aesGcmArmCryptoAvailable() once on first call
 * (see aesGcm.c) and from then on routes Seal/Open through the ARM
 * crypto entry points when available. */

#include "../aesGcm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true iff the current CPU advertises both ARMv8 AES and PMULL
 * extensions.  Safe to call from any thread.  On Apple Silicon the
 * extensions are mandatory on every shipping core, so this is a static
 * true on that platform.  On Linux/aarch64 we consult HWCAP. */
bool aesGcmArmCryptoAvailable(void);

/* Mirror of aesGcmInit (aesGcm.h:51).  Expands the key into the ARM
 * round-key layout and computes the GHASH subkey H = AES_K(0^128).
 * Returns false on bad key length, exactly like the portable variant. */
bool aesGcmArmCryptoInit(aesGcmKey *out, const uint8_t *key, size_t key_len);

/* Mirror of aesGcmSeal (aesGcm.h:55).  Produces bit-identical
 * ciphertext+tag to the portable backend for the same (key, nonce,
 * aad, plaintext) tuple.  `ciphertext` may alias `plaintext`. */
bool aesGcmArmCryptoSeal(const aesGcmKey *k,
                         const uint8_t nonce[AES_GCM_NONCE_LEN],
                         const uint8_t *aad, size_t aad_len,
                         const uint8_t *plaintext, size_t plaintext_len,
                         uint8_t *ciphertext, uint8_t tag[AES_GCM_TAG_LEN]);

/* Mirror of aesGcmOpen (aesGcm.h:62).  Bit-identical decrypt+verify;
 * constant-time tag compare; plaintext zeroed on tag mismatch. */
bool aesGcmArmCryptoOpen(const aesGcmKey *k,
                         const uint8_t nonce[AES_GCM_NONCE_LEN],
                         const uint8_t *aad, size_t aad_len,
                         const uint8_t *ciphertext, size_t ciphertext_len,
                         const uint8_t tag[AES_GCM_TAG_LEN],
                         uint8_t *plaintext);

#ifdef __cplusplus
} /* extern "C" */
#endif
