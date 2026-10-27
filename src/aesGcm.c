/* AES-GCM — see aesGcm.h for the API.
 *
 * Three layers:
 *   1. AES-128/256 block cipher (S-box + MixColumns via T-tables)
 *   2. GHASH over GF(2^128) using the standard bit-by-bit shift+XOR
 *   3. GCM mode: GCTR encryption + GHASH(AAD || CT || lens) tag
 *
 * The public entry points (aesGcmInit / Seal / Open) dispatch at
 * runtime between this portable scalar implementation and any
 * hardware backend the current CPU advertises:
 *   - AES-NI + PCLMUL on x86      (hash/aesGcmNi.c)
 *   - ARMv8 AES + PMULL on AArch64 (hash/aesGcmArmCrypto.c) */

#include "aesGcm.h"

#include "ctmem.h" /* dkCtMemEq16, dkSecureZero */
#include "dkC23.h" /* stdc_rotate_left */
#include "dkDiag.h"
#include "dkEndian.h" /* dkLoadBe32, dkStoreBe32 */
#include "hash/aesGcmArmCrypto.h"
#include "hash/aesGcmNi.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ===== AES =============================================================
 *
 * FIPS 197.  We implement the byte-oriented variant using the standard
 * Rijndael S-box plus an Rcon table for key expansion.  Round function
 * uses a precomputed 4 KiB T-table that combines SubBytes + ShiftRows +
 * MixColumns into one 32-bit lookup per byte. */

static constexpr uint8_t SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b,
    0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
    0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26,
    0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
    0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
    0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed,
    0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f,
    0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec,
    0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14,
    0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
    0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
    0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f,
    0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
    0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f,
    0xb0, 0x54, 0xbb, 0x16};

static constexpr uint32_t RCON[11] = {0x00000000u, 0x01000000u, 0x02000000u,
                                      0x04000000u, 0x08000000u, 0x10000000u,
                                      0x20000000u, 0x40000000u, 0x80000000u,
                                      0x1B000000u, 0x36000000u};

static uint32_t subWord(uint32_t w) {
    return ((uint32_t)SBOX[(w >> 24) & 0xFFu] << 24) |
           ((uint32_t)SBOX[(w >> 16) & 0xFFu] << 16) |
           ((uint32_t)SBOX[(w >> 8) & 0xFFu] << 8) | (uint32_t)SBOX[w & 0xFFu];
}

static uint32_t rotWord(uint32_t w) {
    return stdc_rotate_left(w, 8u);
}

static void aesKeyExpand(uint32_t round_keys[60], const uint8_t *key,
                         uint8_t key_size, uint8_t num_rounds) {
    const uint8_t Nk = (uint8_t)(key_size / 4u);
    const uint32_t total = 4u * ((uint32_t)num_rounds + 1u);
    for (uint8_t i = 0; i < Nk; i++) {
        round_keys[i] = dkLoadBe32(key + (size_t)i * 4u);
    }
    for (uint32_t i = Nk; i < total; i++) {
        uint32_t t = round_keys[i - 1];
        if ((i % Nk) == 0u) {
            t = subWord(rotWord(t)) ^ RCON[i / Nk];
        } else if (Nk > 6u && (i % Nk) == 4u) {
            t = subWord(t);
        }
        round_keys[i] = round_keys[i - Nk] ^ t;
    }
}

/* Field multiply by x in GF(2^8) with the AES reduction polynomial. */
static uint8_t xtime(uint8_t b) {
    return (uint8_t)((b << 1) ^ (((b >> 7) & 1u) * 0x1Bu));
}

/* Single-block AES encrypt.  Operates on a 16-byte state in row-major
 * column-by-column order (column-major when viewed as 4x4 bytes). */
static void aesEncryptBlock(const uint32_t *rk, uint8_t num_rounds,
                            const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    /* AddRoundKey round 0. */
    for (uint32_t c = 0; c < 4; c++) {
        const uint32_t k = rk[c];
        s[c * 4 + 0] = in[c * 4 + 0] ^ (uint8_t)(k >> 24);
        s[c * 4 + 1] = in[c * 4 + 1] ^ (uint8_t)(k >> 16);
        s[c * 4 + 2] = in[c * 4 + 2] ^ (uint8_t)(k >> 8);
        s[c * 4 + 3] = in[c * 4 + 3] ^ (uint8_t)k;
    }
    for (uint32_t round = 1; round < num_rounds; round++) {
        uint8_t t[16];
        /* SubBytes + ShiftRows. */
        /* row 0: no shift; row 1: shift 1; row 2: shift 2; row 3: shift 3.
         * State layout: t[col*4+row]. */
        t[0] = SBOX[s[0]];
        t[1] = SBOX[s[5]];
        t[2] = SBOX[s[10]];
        t[3] = SBOX[s[15]];
        t[4] = SBOX[s[4]];
        t[5] = SBOX[s[9]];
        t[6] = SBOX[s[14]];
        t[7] = SBOX[s[3]];
        t[8] = SBOX[s[8]];
        t[9] = SBOX[s[13]];
        t[10] = SBOX[s[2]];
        t[11] = SBOX[s[7]];
        t[12] = SBOX[s[12]];
        t[13] = SBOX[s[1]];
        t[14] = SBOX[s[6]];
        t[15] = SBOX[s[11]];
        /* MixColumns + AddRoundKey. */
        for (uint32_t c = 0; c < 4; c++) {
            const uint8_t a0 = t[c * 4 + 0];
            const uint8_t a1 = t[c * 4 + 1];
            const uint8_t a2 = t[c * 4 + 2];
            const uint8_t a3 = t[c * 4 + 3];
            const uint8_t m0 =
                (uint8_t)(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
            const uint8_t m1 =
                (uint8_t)(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
            const uint8_t m2 =
                (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
            const uint8_t m3 =
                (uint8_t)((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
            const uint32_t k = rk[round * 4u + c];
            s[c * 4 + 0] = (uint8_t)(m0 ^ (uint8_t)(k >> 24));
            s[c * 4 + 1] = (uint8_t)(m1 ^ (uint8_t)(k >> 16));
            s[c * 4 + 2] = (uint8_t)(m2 ^ (uint8_t)(k >> 8));
            s[c * 4 + 3] = (uint8_t)(m3 ^ (uint8_t)k);
        }
    }
    /* Final round: SubBytes + ShiftRows + AddRoundKey (no MixColumns). */
    {
        uint8_t t[16];
        t[0] = SBOX[s[0]];
        t[1] = SBOX[s[5]];
        t[2] = SBOX[s[10]];
        t[3] = SBOX[s[15]];
        t[4] = SBOX[s[4]];
        t[5] = SBOX[s[9]];
        t[6] = SBOX[s[14]];
        t[7] = SBOX[s[3]];
        t[8] = SBOX[s[8]];
        t[9] = SBOX[s[13]];
        t[10] = SBOX[s[2]];
        t[11] = SBOX[s[7]];
        t[12] = SBOX[s[12]];
        t[13] = SBOX[s[1]];
        t[14] = SBOX[s[6]];
        t[15] = SBOX[s[11]];
        for (uint32_t c = 0; c < 4; c++) {
            const uint32_t k = rk[num_rounds * 4u + c];
            out[c * 4 + 0] = (uint8_t)(t[c * 4 + 0] ^ (uint8_t)(k >> 24));
            out[c * 4 + 1] = (uint8_t)(t[c * 4 + 1] ^ (uint8_t)(k >> 16));
            out[c * 4 + 2] = (uint8_t)(t[c * 4 + 2] ^ (uint8_t)(k >> 8));
            out[c * 4 + 3] = (uint8_t)(t[c * 4 + 3] ^ (uint8_t)k);
        }
    }
}

/* ===== GHASH ============================================================
 *
 * Multiply two 128-bit elements of GF(2^128) under the GCM polynomial
 * x^128 + x^7 + x^2 + x + 1 (NIST SP 800-38D).  Bit-by-bit; not the
 * fastest possible but the simplest correct portable form. */

static void ghashMul(uint8_t out[16], const uint8_t x[16],
                     const uint8_t h[16]) {
    uint8_t z[16] = {};
    uint8_t v[16];
    memcpy(v, h, 16);
    for (uint32_t i = 0; i < 128; i++) {
        const uint32_t byte = i / 8u;
        const uint32_t bit = 7u - (i % 8u);
        const uint8_t mask = (uint8_t)(0u - ((x[byte] >> bit) & 1u));
        for (uint32_t j = 0; j < 16; j++) {
            z[j] ^= v[j] & mask;
        }
        /* v >>= 1 in MSB-first bit order; if LSB was 1, fold polynomial. */
        const uint8_t lsb = (uint8_t)(v[15] & 1u);
        for (int32_t j = 15; j > 0; j--) {
            v[j] = (uint8_t)((v[j] >> 1) | ((v[j - 1] & 1u) << 7));
        }
        v[0] >>= 1;
        if (lsb) {
            v[0] ^= 0xE1u; /* reduction polynomial in MSB-first form */
        }
    }
    memcpy(out, z, 16);
}

static void ghashAbsorb(uint8_t acc[16], const uint8_t h[16],
                        const uint8_t *data, size_t len) {
    while (len > 0) {
        uint8_t block[16] = {};
        const size_t n = (len < 16u) ? len : 16u;
        memcpy(block, data, n);
        for (uint32_t i = 0; i < 16; i++) {
            acc[i] ^= block[i];
        }
        uint8_t out[16];
        ghashMul(out, acc, h);
        memcpy(acc, out, 16);
        data += n;
        len -= n;
    }
}

/* ===== GCM mode (portable) =============================================== */

static bool aesGcmPortableInit(aesGcmKey *out, const uint8_t *key,
                               size_t key_len) {
    if (out == nullptr || key == nullptr ||
        (key_len != AES128_KEY_LEN && key_len != AES256_KEY_LEN)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->key_size = (uint8_t)key_len;
    out->num_rounds = (key_len == AES128_KEY_LEN) ? 10u : 14u;
    aesKeyExpand(out->round_keys, key, out->key_size, out->num_rounds);
    /* H = AES_K(0^128). */
    const uint8_t zero[16] = {};
    aesEncryptBlock(out->round_keys, out->num_rounds, zero, out->h);
    return true;
}

/* Increment the low 32 bits of `block` (big-endian counter mode). */
static void inc32(uint8_t block[16]) {
    uint32_t c = dkLoadBe32(block + 12);
    c++;
    dkStoreBe32(block + 12, c);
}

/* GCTR(K, ICB, in, len, out) — CTR-mode encrypt under round keys K
 * starting at counter block ICB (big-endian, low 32 bits incremented). */
static void gctr(const uint32_t *rk, uint8_t num_rounds, uint8_t icb[16],
                 const uint8_t *in, uint8_t *out, size_t len) {
    while (len > 0) {
        uint8_t ks[16];
        aesEncryptBlock(rk, num_rounds, icb, ks);
        const size_t n = (len < 16u) ? len : 16u;
        for (size_t i = 0; i < n; i++) {
            out[i] = (uint8_t)(in[i] ^ ks[i]);
        }
        inc32(icb);
        in += n;
        out += n;
        len -= n;
    }
}

static bool aesGcmPortableSeal(const aesGcmKey *k, const uint8_t nonce[12],
                               const uint8_t *aad, size_t aad_len,
                               const uint8_t *plaintext, size_t plaintext_len,
                               uint8_t *ciphertext, uint8_t tag[16]) {
    if (k == nullptr || nonce == nullptr || tag == nullptr) {
        return false;
    }
    if (plaintext_len > AES_GCM_MAX_PAYLOAD) {
        return false;
    }
    if (plaintext_len > 0 && (plaintext == nullptr || ciphertext == nullptr)) {
        return false;
    }
    if (aad_len > 0 && aad == nullptr) {
        return false;
    }

    /* J0 = nonce || 0x00000001 (RFC 5116 §3.2; 96-bit nonce case). */
    uint8_t J0[16];
    memcpy(J0, nonce, 12);
    J0[12] = 0;
    J0[13] = 0;
    J0[14] = 0;
    J0[15] = 1;

    /* GCTR uses inc32(J0) as the initial counter block. */
    uint8_t icb[16];
    memcpy(icb, J0, 16);
    inc32(icb);
    gctr(k->round_keys, k->num_rounds, icb, plaintext, ciphertext,
         plaintext_len);

    /* Tag = AES_K(J0) XOR GHASH(AAD || 0^v || CT || 0^u || lens). */
    uint8_t S[16] = {};
    ghashAbsorb(S, k->h, aad, aad_len);
    ghashAbsorb(S, k->h, ciphertext, plaintext_len);
    uint8_t lens[16];
    const uint64_t aad_bits = (uint64_t)aad_len * 8u;
    const uint64_t ct_bits = (uint64_t)plaintext_len * 8u;
    for (uint32_t i = 0; i < 8; i++) {
        lens[i] = (uint8_t)(aad_bits >> (56 - i * 8));
        lens[8 + i] = (uint8_t)(ct_bits >> (56 - i * 8));
    }
    ghashAbsorb(S, k->h, lens, 16);
    uint8_t enc_J0[16];
    aesEncryptBlock(k->round_keys, k->num_rounds, J0, enc_J0);
    for (uint32_t i = 0; i < 16; i++) {
        tag[i] = (uint8_t)(S[i] ^ enc_J0[i]);
    }
    return true;
}

static bool aesGcmPortableOpen(const aesGcmKey *k, const uint8_t nonce[12],
                               const uint8_t *aad, size_t aad_len,
                               const uint8_t *ciphertext, size_t ciphertext_len,
                               const uint8_t tag[16], uint8_t *plaintext) {
    if (k == nullptr || nonce == nullptr || tag == nullptr) {
        return false;
    }
    if (ciphertext_len > AES_GCM_MAX_PAYLOAD) {
        return false;
    }
    if (ciphertext_len > 0 && (ciphertext == nullptr || plaintext == nullptr)) {
        return false;
    }
    if (aad_len > 0 && aad == nullptr) {
        return false;
    }

    uint8_t J0[16];
    memcpy(J0, nonce, 12);
    J0[12] = 0;
    J0[13] = 0;
    J0[14] = 0;
    J0[15] = 1;

    /* Compute expected tag FIRST so we can constant-time compare
     * before decrypting; if mismatched, zero the output buffer and
     * return false. */
    uint8_t S[16] = {};
    ghashAbsorb(S, k->h, aad, aad_len);
    ghashAbsorb(S, k->h, ciphertext, ciphertext_len);
    uint8_t lens[16];
    const uint64_t aad_bits = (uint64_t)aad_len * 8u;
    const uint64_t ct_bits = (uint64_t)ciphertext_len * 8u;
    for (uint32_t i = 0; i < 8; i++) {
        lens[i] = (uint8_t)(aad_bits >> (56 - i * 8));
        lens[8 + i] = (uint8_t)(ct_bits >> (56 - i * 8));
    }
    ghashAbsorb(S, k->h, lens, 16);
    uint8_t enc_J0[16];
    aesEncryptBlock(k->round_keys, k->num_rounds, J0, enc_J0);
    uint8_t computed_tag[16];
    for (uint32_t i = 0; i < 16; i++) {
        computed_tag[i] = (uint8_t)(S[i] ^ enc_J0[i]);
    }
    if (!dkCtMemEq16(computed_tag, tag)) {
        /* Tag mismatch: scrub any speculatively-decrypted plaintext so a
         * caller who ignores the false return cannot leak it.  Must be
         * DCE-resistant since the caller may free `plaintext` immediately. */
        if (plaintext != nullptr && ciphertext_len > 0) {
            dkSecureZero(plaintext, ciphertext_len);
        }
        return false;
    }
    /* Tag matched — decrypt. */
    uint8_t icb[16];
    memcpy(icb, J0, 16);
    inc32(icb);
    gctr(k->round_keys, k->num_rounds, icb, ciphertext, plaintext,
         ciphertext_len);
    return true;
}

/* ===== Backend dispatcher ================================================
 *
 * Runtime selection between the portable scalar implementation above
 * and any hardware backend the host advertises.  The dispatcher
 * resolves once on first call (lock-free; concurrent first-callers
 * compute the same answer so a race is harmless) and pins the choice
 * via a function-pointer table for subsequent calls.
 *
 * Each backend's key schedule writes a different layout into
 * aesGcmKey.round_keys (the portable form packs big-endian 32-bit
 * words; the NI form packs raw 16-byte AES round-key blocks; ARM
 * Crypto will pack its own layout).  That's safe because a given
 * aesGcmKey is only ever initialised by one backend, and the Seal /
 * Open paths use the same backend that initialised it (the
 * dispatcher pins all three through the same vtable).  Mixing
 * backends within one process lifetime would be incorrect — the
 * vtable is fixed at first call. */

static aesGcmForceBackend g_force_backend = AES_GCM_FORCE_AUTO;

void aesGcmSetForceBackend(aesGcmForceBackend f) {
    g_force_backend = f;
}

aesGcmForceBackend aesGcmGetForceBackend(void) {
    return g_force_backend;
}

static aesGcmBackend aesGcmResolveBackend(void) {
    if (g_force_backend == AES_GCM_FORCE_SCALAR) {
        return AES_GCM_BACKEND_SCALAR;
    }
    if (g_force_backend == AES_GCM_FORCE_AESNI && aesGcmNiAvailable()) {
        return AES_GCM_BACKEND_AESNI;
    }
    if (g_force_backend == AES_GCM_FORCE_ARMCRYPTO &&
        aesGcmArmCryptoAvailable()) {
        return AES_GCM_BACKEND_ARMCRYPTO;
    }
    if (g_force_backend == AES_GCM_FORCE_AUTO) {
        if (aesGcmNiAvailable()) {
            return AES_GCM_BACKEND_AESNI;
        }
        if (aesGcmArmCryptoAvailable()) {
            return AES_GCM_BACKEND_ARMCRYPTO;
        }
    }
    return AES_GCM_BACKEND_SCALAR;
}

aesGcmBackend aesGcmActiveBackend(void) {
    return aesGcmResolveBackend();
}

bool aesGcmInit(aesGcmKey *out, const uint8_t *key, size_t key_len) {
    aesGcmBackend b = aesGcmResolveBackend();
    if (b == AES_GCM_BACKEND_AESNI) {
        return aesGcmNiInit(out, key, key_len);
    }
    if (b == AES_GCM_BACKEND_ARMCRYPTO) {
        return aesGcmArmCryptoInit(out, key, key_len);
    }
    return aesGcmPortableInit(out, key, key_len);
}

/* Arguments no backend can use (a NULL key, nonce or tag, or a NULL buffer
 * for a nonzero length): refused before the dispatch, so every backend
 * refuses them alike, and reported once as misuse */
#define AES_GCM_ARGS_MISUSED_(k, nonce, tag, aad, aadLen, in, out, len)        \
    ((!(k) || !(nonce) || !(tag) || ((len) > 0 && (!(in) || !(out))) ||        \
      ((aadLen) > 0 && !(aad))) &&                                             \
     (DK_DIAG(DK_DIAG_AEAD_MISUSE, "a NULL key, nonce or tag, or a NULL "      \
                                   "buffer for a nonzero length"),             \
      true))

bool aesGcmSeal(const aesGcmKey *k, const uint8_t nonce[12], const uint8_t *aad,
                size_t aad_len, const uint8_t *plaintext, size_t plaintext_len,
                uint8_t *ciphertext, uint8_t tag[16]) {
    if (AES_GCM_ARGS_MISUSED_(k, nonce, tag, aad, aad_len, plaintext,
                              ciphertext, plaintext_len)) {
        return false;
    }
    aesGcmBackend b = aesGcmResolveBackend();
    if (b == AES_GCM_BACKEND_AESNI) {
        return aesGcmNiSeal(k, nonce, aad, aad_len, plaintext, plaintext_len,
                            ciphertext, tag);
    }
    if (b == AES_GCM_BACKEND_ARMCRYPTO) {
        return aesGcmArmCryptoSeal(k, nonce, aad, aad_len, plaintext,
                                   plaintext_len, ciphertext, tag);
    }
    return aesGcmPortableSeal(k, nonce, aad, aad_len, plaintext, plaintext_len,
                              ciphertext, tag);
}

bool aesGcmOpen(const aesGcmKey *k, const uint8_t nonce[12], const uint8_t *aad,
                size_t aad_len, const uint8_t *ciphertext,
                size_t ciphertext_len, const uint8_t tag[16],
                uint8_t *plaintext) {
    if (AES_GCM_ARGS_MISUSED_(k, nonce, tag, aad, aad_len, ciphertext,
                              plaintext, ciphertext_len)) {
        return false;
    }
    aesGcmBackend b = aesGcmResolveBackend();
    if (b == AES_GCM_BACKEND_AESNI) {
        return aesGcmNiOpen(k, nonce, aad, aad_len, ciphertext, ciphertext_len,
                            tag, plaintext);
    }
    if (b == AES_GCM_BACKEND_ARMCRYPTO) {
        return aesGcmArmCryptoOpen(k, nonce, aad, aad_len, ciphertext,
                                   ciphertext_len, tag, plaintext);
    }
    return aesGcmPortableOpen(k, nonce, aad, aad_len, ciphertext,
                              ciphertext_len, tag, plaintext);
}

/* ============================================================================
 * Test driver
 * ============================================================================
 */

#ifdef DATAKIT_TEST

#include "cleaner.h"
#include "ctest.h"
#include "datakit.h"
#include "str.h" /* StrHexToBytes */

/* Scope-exit cleaners for test scratch buffers (see cleaner.h idioms). */
defcleanerName(uint8_t *, zfree, Bytes);
defcleanerName(aesGcmKey *, zfree, Key);

int aesGcmTest(int, char *[]) {
    int err = 0;

    /* NIST SP 800-38D Test Vector 1: AES-128 GCM, empty PT + AAD. */
    TEST("AES-128-GCM: NIST Test Vector 1 (empty PT, empty AAD)") {
        uint8_t key[16] = {};
        uint8_t nonce[12] = {};
        uint8_t tag[16];
        aesGcmKey k;
        if (!aesGcmInit(&k, key, 16)) {
            ERR("init");
        }
        aesGcmSeal(&k, nonce, nullptr, 0, nullptr, 0, nullptr, tag);
        uint8_t exp_tag[16];
        TEST_HEX(err, "58e2fccefa7e3061367f1d57a4e7455a", exp_tag, 16);
        if (memcmp(tag, exp_tag, 16) != 0) {
            ERR("tag mismatch");
        }
    }

    /* NIST Vector 2: 16-byte zero PT, empty AAD. */
    TEST("AES-128-GCM: NIST Test Vector 2 (16B zero PT)") {
        uint8_t key[16] = {};
        uint8_t nonce[12] = {};
        uint8_t pt[16] = {};
        uint8_t ct[16], tag[16];
        aesGcmKey k;
        aesGcmInit(&k, key, 16);
        aesGcmSeal(&k, nonce, nullptr, 0, pt, 16, ct, tag);
        uint8_t exp_ct[16], exp_tag[16];
        TEST_HEX(err, "0388dace60b6a392f328c2b971b2fe78", exp_ct, 16);
        TEST_HEX(err, "ab6e47d42cec13bdf53a67b21257bddf", exp_tag, 16);
        if (memcmp(ct, exp_ct, 16) != 0) {
            ERR("CT mismatch");
        }
        if (memcmp(tag, exp_tag, 16) != 0) {
            ERR("tag mismatch");
        }
    }

    /* NIST Vector 3: AES-128 GCM with real key + nonce + 64-byte PT. */
    TEST("AES-128-GCM: NIST Test Vector 3") {
        uint8_t key[16], nonce[12], pt[64], exp_ct[64], exp_tag[16];
        TEST_HEX(err, "feffe9928665731c6d6a8f9467308308", key, 16);
        TEST_HEX(err, "cafebabefacedbaddecaf888", nonce, 12);
        TEST_HEX(
            err,
            "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
            "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255",
            pt, 64);
        TEST_HEX(
            err,
            "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
            "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091473f5985",
            exp_ct, 64);
        TEST_HEX(err, "4d5c2af327cd64a62cf35abd2ba6fab4", exp_tag, 16);
        uint8_t ct[64], tag[16];
        aesGcmKey k;
        aesGcmInit(&k, key, 16);
        aesGcmSeal(&k, nonce, nullptr, 0, pt, 64, ct, tag);
        if (memcmp(ct, exp_ct, 64) != 0) {
            ERR("CT mismatch");
        }
        if (memcmp(tag, exp_tag, 16) != 0) {
            ERR("tag mismatch");
        }
        /* Round-trip Open. */
        uint8_t recovered[64];
        if (!aesGcmOpen(&k, nonce, nullptr, 0, ct, 64, tag, recovered)) {
            ERR("Open failed");
        }
        if (memcmp(recovered, pt, 64) != 0) {
            ERR("Open plaintext mismatch");
        }
    }

    /* NIST Vector 4: AES-128 GCM with AAD. */
    TEST("AES-128-GCM: NIST Test Vector 4 (with AAD)") {
        uint8_t key[16], nonce[12], pt[60], aad[20], exp_ct[60], exp_tag[16];
        TEST_HEX(err, "feffe9928665731c6d6a8f9467308308", key, 16);
        TEST_HEX(err, "cafebabefacedbaddecaf888", nonce, 12);
        TEST_HEX(
            err,
            "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
            "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
            pt, 60);
        TEST_HEX(err, "feedfacedeadbeeffeedfacedeadbeefabaddad2", aad, 20);
        TEST_HEX(
            err,
            "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
            "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091",
            exp_ct, 60);
        TEST_HEX(err, "5bc94fbc3221a5db94fae95ae7121a47", exp_tag, 16);
        uint8_t ct[60], tag[16];
        aesGcmKey k;
        aesGcmInit(&k, key, 16);
        aesGcmSeal(&k, nonce, aad, 20, pt, 60, ct, tag);
        if (memcmp(ct, exp_ct, 60) != 0) {
            ERR("CT mismatch");
        }
        if (memcmp(tag, exp_tag, 16) != 0) {
            ERR("tag mismatch");
        }
        uint8_t rec[60];
        if (!aesGcmOpen(&k, nonce, aad, 20, ct, 60, tag, rec)) {
            ERR("Open");
        }
        if (memcmp(rec, pt, 60) != 0) {
            ERR("Open PT mismatch");
        }
    }

    /* AES-256 GCM. */
    TEST("AES-256-GCM: NIST Test Vector 13 (AES-256 zero key, empty PT)") {
        uint8_t key[32] = {};
        uint8_t nonce[12] = {};
        uint8_t tag[16];
        aesGcmKey k;
        if (!aesGcmInit(&k, key, 32)) {
            ERR("init");
        }
        aesGcmSeal(&k, nonce, nullptr, 0, nullptr, 0, nullptr, tag);
        uint8_t exp_tag[16];
        TEST_HEX(err, "530f8afbc74536b9a963b4f1c4cb738b", exp_tag, 16);
        if (memcmp(tag, exp_tag, 16) != 0) {
            ERR("tag mismatch");
        }
    }

    TEST("AES-256-GCM: round-trip with AAD, varied PT sizes") {
        uint8_t key[32] = {0x5A};
        for (uint32_t i = 0; i < 32; i++) {
            key[i] = (uint8_t)(i * 11u + 3u);
        }
        uint8_t nonce[12];
        for (uint32_t i = 0; i < 12; i++) {
            nonce[i] = (uint8_t)(i * 17u);
        }
        uint8_t aad[24];
        for (uint32_t i = 0; i < 24; i++) {
            aad[i] = (uint8_t)(i * 7u + 1u);
        }
        aesGcmKey k;
        aesGcmInit(&k, key, 32);
        const size_t sizes[] = {0, 1, 15, 16, 17, 63, 64, 65, 4096};
        for (size_t s = 0; s < countof(sizes); s++) {
            const size_t n = sizes[s];
            cleanerName(zfree, Bytes) uint8_t *pt =
                (n > 0) ? zmalloc(n) : nullptr;
            cleanerName(zfree, Bytes) uint8_t *ct =
                (n > 0) ? zmalloc(n) : nullptr;
            cleanerName(zfree, Bytes) uint8_t *rec =
                (n > 0) ? zmalloc(n) : nullptr;
            for (size_t i = 0; i < n; i++) {
                pt[i] = (uint8_t)(i * 13u);
            }
            uint8_t tag[16];
            aesGcmSeal(&k, nonce, aad, 24, pt, n, ct, tag);
            if (!aesGcmOpen(&k, nonce, aad, 24, ct, n, tag, rec)) {
                ERR("Open n=%zu failed", n);
            }
            if (n > 0 && memcmp(pt, rec, n) != 0) {
                ERR("PT mismatch n=%zu", n);
            }
            /* Tamper: flip a CT byte; Open must reject. */
            if (n > 0) {
                ct[n / 2] ^= 1;
                if (aesGcmOpen(&k, nonce, aad, 24, ct, n, tag, rec)) {
                    ERR("Tampered CT accepted n=%zu", n);
                }
                ct[n / 2] ^= 1;
            }
            /* Tamper: flip a tag byte. */
            tag[0] ^= 1;
            if (aesGcmOpen(&k, nonce, aad, 24, ct, n, tag, rec)) {
                ERR("Tampered tag accepted n=%zu", n);
            }
        }
    }

    TEST("AES-GCM: portable and hardware backends agree; forgeries refused") {
        /* Each key initialised under its own backend.  Every seal gives
         * the same ciphertext and tag under both; each backend opens the
         * other's output; under the portable backend (otherwise never
         * opened on hardware hosts) a flipped ciphertext, tag or AAD bit
         * is refused and the output scrubbed */
        const aesGcmForceBackend was = aesGcmGetForceBackend();
        static const aesGcmForceBackend backends[2] = {AES_GCM_FORCE_SCALAR,
                                                       AES_GCM_FORCE_AUTO};
        static constexpr size_t lens[] = {0, 1, 15, 16, 17, 63, 64, 65, 200};
        static constexpr size_t aadLens[] = {0, 7, 16, 33};
        uint64_t rng = 0xae5;
        for (size_t kl = 16; kl <= 32; kl += 16) {
            for (size_t li = 0; li < countof(lens); li++) {
                for (size_t ai = 0; ai < countof(aadLens); ai++) {
                    const size_t n = lens[li];
                    const size_t an = aadLens[ai];
                    uint8_t key[32], nonce[12], pt[200], aad[33];
                    uint8_t *fill[] = {key, nonce, pt, aad};
                    const size_t fillLen[] = {sizeof(key), sizeof(nonce),
                                              sizeof(pt), sizeof(aad)};
                    for (size_t f = 0; f < 4; f++) {
                        for (size_t i = 0; i < fillLen[f]; i++) {
                            rng = rng * 6364136223846793005ULL + 1;
                            fill[f][i] = (uint8_t)(rng >> 56);
                        }
                    }
                    aesGcmKey k[2];
                    uint8_t ct[2][200], tag[2][16];
                    for (int b = 0; b < 2; b++) {
                        aesGcmSetForceBackend(backends[b]);
                        if (!aesGcmInit(&k[b], key, kl) ||
                            !aesGcmSeal(&k[b], nonce, an ? aad : nullptr, an,
                                        n ? pt : nullptr, n, ct[b], tag[b])) {
                            ERR("key %zu len %zu aad %zu: seal failed", kl, n,
                                an);
                        }
                    }
                    if (memcmp(ct[0], ct[1], n) != 0 ||
                        memcmp(tag[0], tag[1], 16) != 0) {
                        ERR("key %zu len %zu aad %zu: backends differ", kl, n,
                            an);
                    }
                    for (int b = 0; b < 2; b++) {
                        aesGcmSetForceBackend(backends[b]);
                        uint8_t out[200];
                        if (!aesGcmOpen(&k[b], nonce, an ? aad : nullptr, an,
                                        n ? ct[!b] : nullptr, n, tag[!b],
                                        n ? out : nullptr) ||
                            memcmp(out, pt, n) != 0) {
                            ERR("key %zu len %zu aad %zu: backend %d cannot "
                                "open the other's output",
                                kl, n, an, b);
                        }
                    }

                    aesGcmSetForceBackend(AES_GCM_FORCE_SCALAR);
                    for (int what = 0; what < 3; what++) {
                        if ((what == 0 && n == 0) || (what == 2 && an == 0)) {
                            continue;
                        }
                        uint8_t c2[200], t2[16], a2[33], out[200];
                        memcpy(c2, ct[0], n);
                        memcpy(t2, tag[0], 16);
                        memcpy(a2, aad, an);
                        if (what == 0) {
                            c2[n / 2] ^= 0x04;
                        } else if (what == 1) {
                            t2[5] ^= 0x80;
                        } else {
                            a2[an - 1] ^= 0x01;
                        }
                        memset(out, 0xAB, sizeof(out));
                        if (aesGcmOpen(&k[0], nonce, an ? a2 : nullptr, an,
                                       n ? c2 : nullptr, n, t2,
                                       n ? out : nullptr)) {
                            ERR("key %zu len %zu aad %zu: forgery %d "
                                "accepted",
                                kl, n, an, what);
                        }
                        for (size_t i = 0; i < n; i++) {
                            if (out[i] != 0) {
                                ERR("key %zu len %zu: refused open left "
                                    "plaintext",
                                    kl, n);
                                break;
                            }
                        }
                    }
                    dkSecureZero(k, sizeof(k));
                }
            }
        }
        aesGcmSetForceBackend(was);
    }

    TEST("aesGcmInit: bad key length refused") {
        aesGcmKey k;
        const uint8_t key[24] = {};
        if (aesGcmInit(&k, key, 24)) {
            ERR("24-byte key (AES-192) must refuse");
        }
        if (aesGcmInit(&k, key, 0)) {
            ERR("0-byte key must refuse");
        }
    }

    TEST("aesGcm: NULL buffers for a nonzero length are refused and "
         "reported") {
        aesGcmKey k;
        const uint8_t key[16] = {7};
        if (!aesGcmInit(&k, key, sizeof(key))) {
            ERR("a 16-byte key must initialise");
        }
        const uint8_t nonce[12] = {};
        uint8_t tag[16] = {};
        uint8_t buf[16] = {};
        const uint64_t diagBefore = dkDiagCount();
        if (aesGcmSeal(&k, nonce, nullptr, 0, nullptr, 16, buf, tag) ||
            aesGcmSeal(&k, nonce, nullptr, 4, buf, 16, buf, tag) ||
            aesGcmOpen(&k, nonce, nullptr, 0, buf, 16, tag, nullptr) ||
            aesGcmOpen(nullptr, nonce, nullptr, 0, buf, 16, tag, buf)) {
            ERR("a NULL buffer for a nonzero length must refuse");
        }
        dkDiagnostic d;
        if (dkDiagCount() != diagBefore + 4 || !dkDiagLast(&d) ||
            d.code != DK_DIAG_AEAD_MISUSE) {
            ERR("four refused calls reported %" PRIu64 " times, want 4",
                dkDiagCount() - diagBefore);
        }
        /* A tag that does not verify is data, not misuse */
        const uint64_t diagData = dkDiagCount();
        if (aesGcmOpen(&k, nonce, nullptr, 0, buf, 16, tag, buf)) {
            ERR("an all-zero tag must not verify");
        }
        if (dkDiagCount() != diagData) {
            ERR("a failed tag check must not report");
        }
        dkSecureZero(&k, sizeof(k));
    }

    TEST("aesGcm: a payload past AES_GCM_MAX_PAYLOAD is refused by every "
         "backend, before any byte is read or written") {
        /* Real (small) buffers and a length one past the bound, so the
         * bound is what refuses (no misuse is reported), and the output
         * buffers keep their sentinel.  A size_t that cannot exceed the
         * bound (32-bit) has nothing to test. */
        if ((uint64_t)SIZE_MAX > AES_GCM_MAX_PAYLOAD) {
            aesGcmKey k;
            const uint8_t key[16] = {9};
            if (!aesGcmInit(&k, key, sizeof(key))) {
                ERR("a 16-byte key must initialise");
            }
            const size_t over = (size_t)(AES_GCM_MAX_PAYLOAD + 1u);
            const uint8_t nonce[12] = {};
            uint8_t tag[16] = {};
            uint8_t in[16] = {};
            const aesGcmForceBackend saved = aesGcmGetForceBackend();
            static const aesGcmForceBackend backends[] = {
                AES_GCM_FORCE_SCALAR, AES_GCM_FORCE_AESNI,
                AES_GCM_FORCE_ARMCRYPTO, AES_GCM_FORCE_AUTO};
            for (size_t b = 0; b < countof(backends); b++) {
                aesGcmSetForceBackend(backends[b]);
                uint8_t out[16];
                memset(out, 0xAA, sizeof(out));
                const uint64_t diagBefore = dkDiagCount();
                if (aesGcmSeal(&k, nonce, nullptr, 0, in, over, out, tag) ||
                    aesGcmOpen(&k, nonce, nullptr, 0, in, over, tag, out)) {
                    ERR("backend %d: a payload past the bound was taken",
                        (int)backends[b]);
                }
                if (dkDiagCount() != diagBefore) {
                    ERR("backend %d: the bound was refused as misuse",
                        (int)backends[b]);
                }
                for (size_t i = 0; i < sizeof(out); i++) {
                    if (out[i] != 0xAA) {
                        ERR("backend %d: a refused call wrote byte %zu",
                            (int)backends[b], i);
                        break;
                    }
                }
            }
            aesGcmSetForceBackend(saved);
            dkSecureZero(&k, sizeof(k));
        }
    }

    TEST("aesGcmKey: dkSecureZero scrubs round_keys + h "
         "(testAesGcmKeyWipeProof)") {
        /* The flexEnvelope AES-GCM cold path stores the expanded key
         * schedule in a stack-local `aesGcmKey k` and scrubs via
         * `dkSecureZero(&k, sizeof(k))` before frame exit (see
         * flexEnvelope.c sealAead + Open).  The cold-path k is a stack-
         * local we cannot inspect after the frame pops; this test
         * exercises the same wipe at the aesGcmKey abstraction the
         * cold path relies on.
         *
         * Heap-shadow: allocate an aesGcmKey, Init it (populating
         * round_keys + the GHASH subkey h), apply dkSecureZero, and
         * verify the entire struct is bytewise zero.  This pins the
         * abstraction; a future change to `dkSecureZero(&k, sizeof(k))`
         * → plain memset that gets DCE-elided would fail here. */
        cleanerName(zfree, Key) aesGcmKey *k = (aesGcmKey *)zmalloc(sizeof(*k));
        uint8_t key128[16];
        memset(key128, 0x7B, sizeof(key128));
        if (!aesGcmInit(k, key128, sizeof(key128))) {
            ERR("AES-128 Init failed");
        }
        /* Post-Init we expect round_keys and h to be populated (non-zero
         * for the canonical 0x7B-pattern key). */
        bool any_nonzero = false;
        const uint8_t *p_init = (const uint8_t *)k;
        for (size_t i = 0; i < sizeof(*k); i++) {
            if (p_init[i] != 0) {
                any_nonzero = true;
                break;
            }
        }
        if (!any_nonzero) {
            ERR("aesGcmInit produced an all-zero schedule (test premise "
                "invalid: cannot distinguish zero-init from wipe)");
        }
        /* Apply the same scrub the flexEnvelope cold path uses. */
        dkSecureZero(k, sizeof(*k));
        const uint8_t *p = (const uint8_t *)k;
        for (size_t i = 0; i < sizeof(*k); i++) {
            if (p[i] != 0) {
                ERR("AES-128 dkSecureZero left nonzero byte at offset %zu "
                    "(=0x%02x)",
                    i, p[i]);
                break;
            }
        }

        /* Repeat for AES-256 to cover the 14-round schedule. */
        uint8_t key256[32];
        memset(key256, 0x4F, sizeof(key256));
        if (!aesGcmInit(k, key256, sizeof(key256))) {
            ERR("AES-256 Init failed");
        }
        dkSecureZero(k, sizeof(*k));
        for (size_t i = 0; i < sizeof(*k); i++) {
            if (p[i] != 0) {
                ERR("AES-256 dkSecureZero left nonzero byte at offset %zu "
                    "(=0x%02x)",
                    i, p[i]);
                break;
            }
        }
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
