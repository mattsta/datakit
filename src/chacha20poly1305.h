#pragma once

/* RFC 8439 ChaCha20-Poly1305 AEAD primitive.
 *
 * Portable, self-contained, caller-owns-storage; no heap allocations on
 * the hot path.  Intended as a building block for higher-level envelope
 * formats (flexEnvelope etc.).
 *
 * All buffers are caller-owned.  ciphertext may alias plaintext for
 * in-place encryption; plaintext may alias ciphertext on Open. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHACHA20_POLY1305_KEY_LEN ((size_t)32)
#define CHACHA20_POLY1305_NONCE_LEN ((size_t)12)
#define CHACHA20_POLY1305_TAG_LEN ((size_t)16)

/* Upper bound on plaintext length for a single (key, nonce) pair.  RFC
 * 8439's 32-bit block counter starts at 1 for encryption (block 0 is
 * reserved for the Poly1305 one-time-key) so the maximum encryptable
 * length before the counter would wrap into the OTK block is
 * (2^32 - 1) * 64 bytes.  Sealing past this point would silently
 * collide the keystream with the OTK and destroy both confidentiality
 * and authenticity, so Seal/Open refuse at this boundary. */
#define CHACHA20_POLY1305_MAX_PAYLOAD                                          \
    (((uint64_t)0xFFFFFFFFu - 1u) * (uint64_t)64u)

/* Encrypt + authenticate.  Writes `plaintext_len` bytes to `ciphertext`
 * and CHACHA20_POLY1305_TAG_LEN bytes to `tag`.  `aad` (additional
 * authenticated data) may be NULL/0 — it is authenticated but not
 * encrypted, intended for envelope framing bytes.  ciphertext may
 * alias plaintext for in-place encryption.
 *
 * Returns true on success, false iff `plaintext_len` exceeds
 * CHACHA20_POLY1305_MAX_PAYLOAD — in which case NEITHER ciphertext NOR
 * tag is written and the caller must not transmit anything for this
 * (key, nonce, plaintext) triple. */
bool chacha20poly1305Seal(const uint8_t key[CHACHA20_POLY1305_KEY_LEN],
                          const uint8_t nonce[CHACHA20_POLY1305_NONCE_LEN],
                          const uint8_t *aad, size_t aad_len,
                          const uint8_t *plaintext, size_t plaintext_len,
                          uint8_t *ciphertext,
                          uint8_t tag[CHACHA20_POLY1305_TAG_LEN]);

/* Decrypt + verify.  Returns true iff `tag` matches the computed tag
 * (constant-time compare).  On success writes `ciphertext_len` bytes
 * to `plaintext` (may alias ciphertext).  On failure the caller MUST
 * NOT use the plaintext output — implementations zero it. */
bool chacha20poly1305Open(const uint8_t key[CHACHA20_POLY1305_KEY_LEN],
                          const uint8_t nonce[CHACHA20_POLY1305_NONCE_LEN],
                          const uint8_t *aad, size_t aad_len,
                          const uint8_t *ciphertext, size_t ciphertext_len,
                          const uint8_t tag[CHACHA20_POLY1305_TAG_LEN],
                          uint8_t *plaintext);

/* ===== Streaming Seal / Open =========================================
 *
 * Chunked variants of Seal/Open producing the same RFC 8439 wire
 * output, so callers can encrypt or decrypt payloads larger than RAM.
 * The ctx is caller-owned, fixed size, and never allocates on the hot
 * path.
 *
 * Lifecycle (Seal):
 *   1. SealStreamInit(ctx, key, nonce)
 *   2. SealStreamAad(ctx, aad_chunk, len) — zero or more.  All AAD
 *      MUST be supplied BEFORE the first SealStreamUpdate call.
 *   3. SealStreamUpdate(ctx, plaintext_chunk, len, ciphertext_out)
 *      zero or more times.  `ciphertext_out` may alias the input.
 *   4. SealStreamFinal(ctx, tag)  — writes the 16-byte tag.
 *
 * Open mirrors Seal: OpenStreamInit / Aad / Update (decrypts) / Final
 * (constant-time tag verify).
 *
 * Any function returning bool returns false on out-of-order calls,
 * NULL inputs where required, or when total ciphertext would exceed
 * CHACHA20_POLY1305_MAX_PAYLOAD.  Failure POISONS the ctx — every
 * subsequent call also returns false (fail-closed; resuming a
 * corrupted state would mask the violation).  Out-of-order calls and
 * NULL inputs (Init's included: it returns nothing) are reported as
 * DK_DIAG_AEAD_MISUSE (dkDiag.h), as are the one-shot Seal/Open's NULL
 * buffers for a nonzero length; a tag that does not verify is not.
 *
 * CAUTION — Streaming Open plaintext-before-tag exposure:
 *   The Poly1305 tag is only verified inside OpenStreamFinal.  Until
 *   OpenStreamFinal returns true, treat every byte written by
 *   OpenStreamUpdate as adversary-controlled.  Callers MUST NOT
 *   release these bytes to a downstream consumer (network socket,
 *   parser, file flush, replay pipeline, etc.) until Final has
 *   succeeded.  A pipeline that emits decrypted chunks before tag-
 *   check surfaces ships unauthenticated plaintext; the tag failure
 *   only arrives at end-of-stream.  For incremental release of
 *   plaintext under authentication, frame each chunk with its own
 *   AEAD invocation (one-shot Seal/Open per chunk) so each piece is
 *   independently verified before it is consumed. */

typedef enum chacha20poly1305StreamPhase {
    CHACHA20_POLY1305_STREAM_PHASE_UNINIT = 0,
    CHACHA20_POLY1305_STREAM_PHASE_AAD = 1,
    CHACHA20_POLY1305_STREAM_PHASE_BODY = 2,
    CHACHA20_POLY1305_STREAM_PHASE_FINALIZED = 3,
    CHACHA20_POLY1305_STREAM_PHASE_POISONED = 4
} chacha20poly1305StreamPhase;

/* Internal fields are exposed so callers can stack-allocate the ctx;
 * do NOT read or write them directly — use the API.  Layout matches
 * the poly1305 limb state prefix so the implementation can reinterpret
 * the ctx as a poly1305State pointer; a static_assert in
 * chacha20poly1305.c locks the offsets. */
typedef struct chacha20poly1305StreamCtx {
    uint64_t poly_h[5];
    uint32_t poly_r[5];
    uint32_t poly_s[4];
    uint8_t poly_buf[16];
    size_t poly_buf_len;
    uint8_t key[CHACHA20_POLY1305_KEY_LEN];
    uint8_t nonce[CHACHA20_POLY1305_NONCE_LEN];
    uint8_t keystream_buf[64];
    uint64_t aad_total;
    uint64_t ct_total;
    uint32_t chacha_counter;
    uint32_t keystream_used;
    uint8_t phase;
    uint8_t _pad[7];
} chacha20poly1305StreamCtx;

void chacha20poly1305SealStreamInit(
    chacha20poly1305StreamCtx *ctx,
    const uint8_t key[CHACHA20_POLY1305_KEY_LEN],
    const uint8_t nonce[CHACHA20_POLY1305_NONCE_LEN]);

bool chacha20poly1305SealStreamAad(chacha20poly1305StreamCtx *ctx,
                                   const uint8_t *aad, size_t len);

bool chacha20poly1305SealStreamUpdate(chacha20poly1305StreamCtx *ctx,
                                      const uint8_t *plaintext, size_t len,
                                      uint8_t *ciphertext_out);

bool chacha20poly1305SealStreamFinal(chacha20poly1305StreamCtx *ctx,
                                     uint8_t tag[CHACHA20_POLY1305_TAG_LEN]);

void chacha20poly1305OpenStreamInit(
    chacha20poly1305StreamCtx *ctx,
    const uint8_t key[CHACHA20_POLY1305_KEY_LEN],
    const uint8_t nonce[CHACHA20_POLY1305_NONCE_LEN]);

bool chacha20poly1305OpenStreamAad(chacha20poly1305StreamCtx *ctx,
                                   const uint8_t *aad, size_t len);

bool chacha20poly1305OpenStreamUpdate(chacha20poly1305StreamCtx *ctx,
                                      const uint8_t *ciphertext, size_t len,
                                      uint8_t *plaintext_out);

bool chacha20poly1305OpenStreamFinal(
    chacha20poly1305StreamCtx *ctx,
    const uint8_t expected_tag[CHACHA20_POLY1305_TAG_LEN]);

/* ===== SIMD backend dispatch (testing / telemetry) ===================
 *
 * chacha20XorStream uses runtime SIMD dispatch: AVX2 8-block on x86
 * when available, NEON 4-block on AArch64 when available, scalar
 * one-block-at-a-time otherwise.  These accessors expose the choice
 * for tests and for backend-parity assertions.  Forcing a specific
 * backend pins the dispatcher to it; pinning to a backend unavailable
 * on the host silently falls back to AUTO. */
typedef enum chacha20Backend {
    CHACHA20_BACKEND_SCALAR = 0,
    CHACHA20_BACKEND_NEON = 1,
    CHACHA20_BACKEND_AVX2 = 2,
} chacha20Backend;

typedef enum chacha20ForceBackend {
    CHACHA20_FORCE_AUTO = 0,
    CHACHA20_FORCE_SCALAR = 1,
    CHACHA20_FORCE_NEON = 2,
    CHACHA20_FORCE_AVX2 = 3,
} chacha20ForceBackend;

chacha20Backend chacha20ActiveBackend(void);
void chacha20SetForceBackend(chacha20ForceBackend f);
chacha20ForceBackend chacha20GetForceBackend(void);

/* ===== Poly1305 SIMD backend dispatch (testing / telemetry) ==========
 *
 * poly1305Update uses runtime SIMD dispatch for the bulk-block phase:
 * AVX2 4-block batch on x86 when available, NEON 2-block batch on
 * AArch64 when available, scalar one-block-at-a-time otherwise.  These
 * accessors expose the choice for tests and for backend-parity
 * assertions.  Forcing a specific backend pins the dispatcher to it;
 * pinning to a backend unavailable on the host silently falls back to
 * AUTO. */
typedef enum poly1305Backend {
    POLY1305_BACKEND_SCALAR = 0,
    POLY1305_BACKEND_NEON = 1,
    POLY1305_BACKEND_AVX2 = 2,
} poly1305Backend;

typedef enum poly1305ForceBackend {
    POLY1305_FORCE_AUTO = 0,
    POLY1305_FORCE_SCALAR = 1,
    POLY1305_FORCE_NEON = 2,
    POLY1305_FORCE_AVX2 = 3,
} poly1305ForceBackend;

poly1305Backend poly1305ActiveBackend(void);
void poly1305SetForceBackend(poly1305ForceBackend f);
poly1305ForceBackend poly1305GetForceBackend(void);

#ifdef DATAKIT_TEST
int chacha20poly1305Test(int argc, char *argv[]);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif
