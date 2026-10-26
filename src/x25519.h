#pragma once

/* X25519 — Curve25519 ECDH key agreement (RFC 7748).
 *
 * Self-contained constant-time implementation.  Two callable
 * primitives:
 *   - x25519Base(out, priv):   public key  = X25519(priv, base point 9)
 *   - x25519(out, priv, peer): shared key  = X25519(priv, peer_public)
 *
 * Both run the Montgomery ladder over GF(2^255 - 19) in constant time
 * with respect to the private key bits.  Returns true on success; the
 * only failure mode is `peer` being a low-order point (mapped to a
 * zero output, which the API surfaces by returning false from x25519()
 * so the caller can reject the agreement). */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define X25519_KEY_LEN ((size_t)32)

/* Derive the public key from a 32-byte private scalar.
 * Caller-supplied `priv` is used as-is; the implementation applies the
 * RFC 7748 clamping internally and does NOT mutate `priv`.  Always
 * succeeds. */
void x25519Base(uint8_t out_public[X25519_KEY_LEN],
                const uint8_t priv[X25519_KEY_LEN]);

/* Compute X25519(priv, peer_public) — the ECDH shared secret.
 * Returns false if the result is the all-zero point (low-order
 * peer key).  Implementations MUST treat false as a security
 * failure and refuse to proceed — RFC 7748 §6.1 specifies this
 * check. */
bool x25519(uint8_t out_shared[X25519_KEY_LEN],
            const uint8_t priv[X25519_KEY_LEN],
            const uint8_t peer_public[X25519_KEY_LEN]);

#ifdef DATAKIT_TEST
int x25519Test(int argc, char *argv[]);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif
