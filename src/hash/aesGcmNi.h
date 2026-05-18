#pragma once

/* AES-GCM — AES-NI + PCLMUL accelerated backend (x86-64).
 *
 * Mirrors the public surface of aesGcm.{c,h} so the dispatcher in
 * aesGcm.c can substitute this backend transparently.  The probe
 * aesGcmNiAvailable() reports whether the host CPU supports both
 * AES-NI (`aes`) and carry-less-multiply (`pclmul`) — both are
 * required.  PCLMUL implies SSE4.1 so we don't probe for that
 * separately.
 *
 * On non-x86 hosts the file defines stubs so the Available() probe is
 * always linkable; callers gate execution on the boolean and never
 * reach the entry points unless the probe returns true.  This matches
 * the structure used by hash/sha256ShaNi.c and hash/blake3AVX2.c.
 *
 * No dynamic allocation: callers supply an `aesGcmKey` buffer (the
 * same struct used by the portable backend, defined in aesGcm.h) and
 * the NI key schedule is laid out into the existing `round_keys`
 * array as raw 16-byte AES round-key blocks (10 or 14 of them plus
 * the initial round key) packed contiguously.  GHASH subkey `H` is
 * computed by encrypting the zero block under the freshly expanded
 * key and stored in the existing `h` field.
 *
 * The dispatcher checks aesGcmNiAvailable() once on first call (see
 * aesGcm.c) and from then on routes Seal/Open through the NI entry
 * points when available. */

#include "../aesGcm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true iff the current CPU advertises both AES-NI and PCLMUL.
 * Safe to call from any thread; result is computed via
 * __builtin_cpu_supports() which caches its CPUID probe. */
bool aesGcmNiAvailable(void);

/* Mirror of aesGcmInit (aesGcm.h:51).  Expands the key into the NI
 * round-key layout and computes the GHASH subkey H = AES_K(0^128).
 * Returns false on bad key length, exactly like the portable variant. */
bool aesGcmNiInit(aesGcmKey *out, const uint8_t *key, size_t key_len);

/* Mirror of aesGcmSeal (aesGcm.h:55).  Produces bit-identical
 * ciphertext+tag to the portable backend for the same (key, nonce,
 * aad, plaintext) tuple.  `ciphertext` may alias `plaintext`. */
bool aesGcmNiSeal(const aesGcmKey *k, const uint8_t nonce[AES_GCM_NONCE_LEN],
                  const uint8_t *aad, size_t aad_len, const uint8_t *plaintext,
                  size_t plaintext_len, uint8_t *ciphertext,
                  uint8_t tag[AES_GCM_TAG_LEN]);

/* Mirror of aesGcmOpen (aesGcm.h:62).  Bit-identical decrypt+verify;
 * constant-time tag compare; plaintext zeroed on tag mismatch. */
bool aesGcmNiOpen(const aesGcmKey *k, const uint8_t nonce[AES_GCM_NONCE_LEN],
                  const uint8_t *aad, size_t aad_len, const uint8_t *ciphertext,
                  size_t ciphertext_len, const uint8_t tag[AES_GCM_TAG_LEN],
                  uint8_t *plaintext);

#ifdef __cplusplus
} /* extern "C" */
#endif
