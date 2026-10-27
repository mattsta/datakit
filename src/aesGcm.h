#pragma once

/* AES-GCM (NIST SP 800-38D) — authenticated encryption.
 *
 * Supports AES-128 and AES-256 (key length determines mode).  Nonce is
 * always 12 bytes (96 bits) per RFC 5116; tag is always 16 bytes.
 * AAD is authenticated but not encrypted.
 *
 * Implementation is portable scalar C.  AES round work uses S-box +
 * MixColumns tables; GHASH uses shift-and-XOR over GF(2^128).
 *
 * SECURITY NOTE — portable AES has CACHE-timing exposure: the T-table
 * approach reads memory at indices derived from key+round-state, so an
 * attacker co-resident on the same CPU can in principle recover round
 * keys via cache-side-channel.  This implementation is intended for
 * envelope-style data-at-rest / data-in-transit protection where the
 * threat model does NOT include a co-resident adversary.  For hostile
 * shared-tenant environments switch to a hardware AES path (AES-NI on
 * x86, AESE/AESMC on ARMv8) — a follow-up task. */

#ifdef __cplusplus
extern "C" {
#endif

#include "chacha20poly1305.h" /* shared FLEX_ENV_MAX_NONCE_LEN etc */

#include <stddef.h>
#include <stdint.h>

#define AES_GCM_NONCE_LEN ((size_t)12)
#define AES_GCM_TAG_LEN ((size_t)16)
#define AES128_KEY_LEN ((size_t)16)
#define AES256_KEY_LEN ((size_t)32)

/* Maximum AEAD payload per (key, nonce): NIST GCM caps at 2^36 - 32
 * bytes (~64 GiB).  Seal/Open refuse at this boundary. */
#define AES_GCM_MAX_PAYLOAD (((uint64_t)1 << 36) - 32u)

typedef struct aesGcmKey {
    /* Up to AES-256 = 14 rounds + 1, each round 4 x 32-bit words.  The
     * hardware backends read and write it as 16-byte vectors (AES-NI's
     * aligned stores fault on x86 otherwise), so the type is 16-byte
     * aligned: every aesGcmKey the compiler places (an array element, a
     * struct member) is, and heap memory from any allocator honouring
     * max_align_t is. */
    alignas(16) uint32_t round_keys[60];
    uint8_t h[16];      /* GHASH subkey = AES_K(0^128) */
    uint8_t key_size;   /* 16 (AES-128) or 32 (AES-256) */
    uint8_t num_rounds; /* 10 or 14 */
    uint8_t _pad[6];
} aesGcmKey;
static_assert(alignof(aesGcmKey) >= 16 && sizeof(aesGcmKey) % 16 == 0,
              "aesGcmKey must stay 16-byte aligned, element by element");

/* Initialise from raw key bytes.  Valid key lengths: 16 (AES-128) or
 * 32 (AES-256).  Returns false on any other length. */
bool aesGcmInit(aesGcmKey *out, const uint8_t *key, size_t key_len);

/* The AES key schedule (FIPS-197 5.2) in byte order: 11 round keys of 16
 * bytes for a 16-byte key, 15 for a 32-byte key (the caller checked the
 * length).  A backend whose instructions take round keys as byte vectors
 * fills them from here, so every backend expands keys one way. */
static constexpr size_t AES_GCM_ROUND_KEY_BYTES = 240;
void aesGcmKeyScheduleBytes(const uint8_t *key, size_t key_len,
                            uint8_t out[AES_GCM_ROUND_KEY_BYTES]);

/* Encrypt + authenticate.  Returns false if `plaintext_len` exceeds
 * AES_GCM_MAX_PAYLOAD.  `ciphertext` may alias `plaintext`. */
bool aesGcmSeal(const aesGcmKey *k, const uint8_t nonce[AES_GCM_NONCE_LEN],
                const uint8_t *aad, size_t aad_len, const uint8_t *plaintext,
                size_t plaintext_len, uint8_t *ciphertext,
                uint8_t tag[AES_GCM_TAG_LEN]);

/* Decrypt + verify.  Returns true iff `tag` matches the computed tag
 * (constant-time compare).  On failure plaintext is zeroed. */
bool aesGcmOpen(const aesGcmKey *k, const uint8_t nonce[AES_GCM_NONCE_LEN],
                const uint8_t *aad, size_t aad_len, const uint8_t *ciphertext,
                size_t ciphertext_len, const uint8_t tag[AES_GCM_TAG_LEN],
                uint8_t *plaintext);

/* Backend selected by the dispatcher (introspection for tests).
 *
 * Reports which path Init/Seal/Open will actually take on the
 * current host.  The dispatcher probes hardware capabilities once
 * on first call and pins the result for the process lifetime; the
 * answer reflects that pinned choice (plus the force-impl knob).
 *
 * SCALAR    : the portable AES + bit-by-bit GHASH path in aesGcm.c.
 * AESNI     : the AES-NI + PCLMUL backend in hash/aesGcmNi.c (x86).
 * ARMCRYPTO : the ARMv8 AES + PMULL backend in hash/aesGcmArmCrypto.c. */
typedef enum aesGcmBackend {
    AES_GCM_BACKEND_SCALAR = 0,
    AES_GCM_BACKEND_AESNI = 1,
    AES_GCM_BACKEND_ARMCRYPTO = 2,
} aesGcmBackend;

aesGcmBackend aesGcmActiveBackend(void);

/* Force a specific backend (testing / benchmarking).  Default AUTO
 * picks the fastest available; SCALAR / AESNI / ARMCRYPTO pin the
 * choice.  Pinning to a backend that is not available on the host
 * silently falls back to AUTO (the dispatcher will pick whatever it
 * can actually run). */
typedef enum aesGcmForceBackend {
    AES_GCM_FORCE_AUTO = 0,
    AES_GCM_FORCE_SCALAR = 1,
    AES_GCM_FORCE_AESNI = 2,
    AES_GCM_FORCE_ARMCRYPTO = 3,
} aesGcmForceBackend;

void aesGcmSetForceBackend(aesGcmForceBackend f);
aesGcmForceBackend aesGcmGetForceBackend(void);

#ifdef DATAKIT_TEST
int aesGcmTest(int argc, char *argv[]);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif
