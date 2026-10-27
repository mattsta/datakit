/* AES-GCM — ARMv8 AES + PMULL accelerated backend (AArch64).
 *
 * Public API mirrors aesGcm.c (Init/Seal/Open) plus an
 * aesGcmArmCryptoAvailable() probe.  The dispatcher in aesGcm.c routes
 * to this backend when the probe returns true (typically Apple Silicon
 * and modern Cortex-A cores).
 *
 * Reference: ARM "ARMv8-A AArch64 Cryptographic Extension" + RFC 5288.
 *
 * Layout:
 *   - aarch64-only block guarded by `#if defined(__aarch64__) &&
 *     defined(__ARM_FEATURE_CRYPTO) && defined(__ARM_NEON)`
 *   - Soft stubs on every other platform so the symbol set is stable
 *     and direct callers crash if (and only if) they ignore the probe.
 *
 * Storage model: we piggyback on the existing `aesGcmKey` struct from
 * aesGcm.h.  `round_keys` is `uint32_t[60]` = 240 bytes = 15 * 16-byte
 * AES round keys (enough for AES-256's 14+1).  The ARM key schedule
 * writes its expanded keys directly into this region as raw 16-byte
 * blocks (uint8x16_t).  `h[16]` holds the GHASH subkey H = AES_K(0).
 * `key_size` and `num_rounds` keep their portable meanings.  The
 * portable round_keys layout (big-endian 32-bit words) and the ARM
 * layout (raw little-endian 16-byte blocks) are NOT compatible — but a
 * key struct only ever sees one backend's writes followed by reads
 * from the same backend (the dispatcher pins both calls to one path),
 * so this is safe in practice.
 *
 * GHASH model: GCM specifies its multiplications in a "reflected" form
 * (bit-reversed within each byte relative to natural register order).
 * To make `vmull_p64` agree with the NIST test vectors we byte-reverse
 * each block once on load and again on store; the multiplication
 * itself proceeds in the reflected representation with the same
 * Montgomery-style reduction used by the AES-NI backend.  The
 * pre-shifted H optimization is identical to the NI backend.
 *
 * Correctness is verified by `dkTestAesArmCrypto`: every existing
 * aesGcm test vector is run through both the portable path and the
 * ARM-crypto path and the ciphertext+tag bytes are compared. */

#include "aesGcmArmCrypto.h"

#include "../ctmem.h"    /* dkCtMemEq16, dkSecureZero */
#include "../dkEndian.h" /* dkLoadBe32, dkStoreBe32 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Build / runtime capability                                               */
/* ------------------------------------------------------------------------ */

#if defined(__aarch64__) && defined(__ARM_FEATURE_CRYPTO) && defined(__ARM_NEON)
#define AES_GCM_ARM_CRYPTO_BUILD 1
#else
#define AES_GCM_ARM_CRYPTO_BUILD 0
#endif

#if AES_GCM_ARM_CRYPTO_BUILD

#include <arm_neon.h>

#if defined(__APPLE__)
/* On Apple Silicon the AES + PMULL crypto extensions are mandatory on
 * every shipping core, so we can shortcut runtime probing.  This
 * matches the sha256ArmCrypto.c precedent and avoids dragging in
 * sysctl just to discover a static answer. */
#define AES_GCM_ARM_CRYPTO_RUNTIME_OK() (true)
#elifdef __linux__
#include <sys/auxv.h>
#ifndef HWCAP_AES
#define HWCAP_AES (1UL << 3)
#endif
#ifndef HWCAP_PMULL
#define HWCAP_PMULL (1UL << 4)
#endif
#define AES_GCM_ARM_CRYPTO_RUNTIME_OK()                                        \
    ((getauxval(AT_HWCAP) & (HWCAP_AES | HWCAP_PMULL)) ==                      \
     (HWCAP_AES | HWCAP_PMULL))
#else
/* Conservative: assume the compiler enabled crypto deliberately. */
#define AES_GCM_ARM_CRYPTO_RUNTIME_OK() (true)
#endif

bool aesGcmArmCryptoAvailable(void) {
    return AES_GCM_ARM_CRYPTO_RUNTIME_OK();
}

/* ====================================================================== */
/* Block encrypt                                                          */
/* ====================================================================== */

/* AES round on AArch64 with crypto:
 *   vaeseq_u8(state, key) = SubBytes(ShiftRows(state ^ key))
 *   vaesmcq_u8(state)     = MixColumns(state)
 *
 * The standard pattern for full AES encryption is:
 *   for r in 1..Nr-1: state = MC( SB( SR( state XOR RK[r-1] ) ) )
 *                            = vaesmcq_u8(vaeseq_u8(state, RK[r-1]))
 *   final:           state = SB( SR( state XOR RK[Nr-1] ) ) XOR RK[Nr]
 *                            = vaeseq_u8(state, RK[Nr-1]) XOR RK[Nr]
 *
 * The vaeseq_u8 instruction already folds in the AddRoundKey by XOR-ing
 * the second operand before SubBytes.  So we feed it RK[r-1] in each
 * round, and the final round XORs with RK[Nr] manually after the last
 * SB+SR.
 */
[[gnu::target("crypto"), gnu::always_inline]] static inline uint8x16_t
aesArmEncBlock(const uint8x16_t *rk, size_t num_rounds, uint8x16_t state) {
    /* Rounds 1..Nr-1: aese (SR+SB+AddRoundKey RK[r-1]) + aesmc. */
    for (size_t r = 0; r < num_rounds - 1u; r++) {
        state = vaeseq_u8(state, rk[r]);
        state = vaesmcq_u8(state);
    }
    /* Final round: aese with RK[Nr-1] then XOR with RK[Nr]. */
    state = vaeseq_u8(state, rk[num_rounds - 1u]);
    state = veorq_u8(state, rk[num_rounds]);
    return state;
}

/* ====================================================================== */
/* GHASH (PMULL + reflection)                                             */
/* ====================================================================== */

/* Byte-reverse mask (lane-wise) used to swap MSB-first (GCM wire) <->
 * LSB-first (xmm-style natural) within a 128-bit lane.  Same pattern
 * as `bswapMask` in hash/aesGcmNi.c. */
[[gnu::target("crypto"), gnu::always_inline]] static inline uint8x16_t
armBswap128(uint8x16_t v) {
    return vrev64q_u8(vextq_u8(v, v, 8));
}

/* Helpers: extract the low/high 64 bits as a poly64_t. */
[[gnu::target("crypto"), gnu::always_inline]] static inline poly64_t
lo64(uint8x16_t v) {
    return (poly64_t)vgetq_lane_u64(vreinterpretq_u64_u8(v), 0);
}
[[gnu::target("crypto"), gnu::always_inline]] static inline poly64_t
hi64(uint8x16_t v) {
    return (poly64_t)vgetq_lane_u64(vreinterpretq_u64_u8(v), 1);
}

/* Cast a poly128_t result of vmull_p64 back to a uint8x16_t. */
[[gnu::target("crypto"), gnu::always_inline]] static inline uint8x16_t
p128ToU8(poly128_t p) {
    return vreinterpretq_u8_p128(p);
}

/* Schoolbook 128-bit carry-less product into (hi, mid, lo) decomposed
 * 256-bit result, then fold the middle term and Montgomery-reduce.
 * Pattern mirrors the AES-NI backend (libsodium gcm_reduce) with
 * NEON-style vshl_n / vsri_n byte-shift equivalents.
 *
 * Both inputs are in the "reflected" representation (each block
 * byte-reversed on load); `h` has been pre-shifted by one bit during
 * key setup so we avoid paying the shift cost per-multiply. */
[[gnu::target("crypto"), gnu::always_inline]] static inline uint8x16_t
armGhashMul(uint8x16_t x, uint8x16_t h) {
    /* Schoolbook 4-clmul: form lo, hi, mid.
     *   r_lo  = x.lo * h.lo
     *   r_hi  = x.hi * h.hi
     *   r_mid = (x.lo * h.hi) ^ (x.hi * h.lo)  -- the cross-product
     */
    const poly64_t x_lo = lo64(x);
    const poly64_t x_hi = hi64(x);
    const poly64_t h_lo = lo64(h);
    const poly64_t h_hi = hi64(h);

    const uint8x16_t r_lo = p128ToU8(vmull_p64(x_lo, h_lo));
    const uint8x16_t r_hi = p128ToU8(vmull_p64(x_hi, h_hi));
    const uint8x16_t r_mid = veorq_u8(p128ToU8(vmull_p64(x_lo, h_hi)),
                                      p128ToU8(vmull_p64(x_hi, h_lo)));

    /* Fold mid (128 bits centered on bit 64..191) into hi:lo (256 bits).
     * Low 64 bits of mid join the high half of lo; high 64 bits of
     * mid join the low half of hi.  We synthesize 8-byte left/right
     * shifts via vextq_u8 with a zero vector — same operation as
     * _mm_slli_si128/_mm_srli_si128 by 8 in the AES-NI backend. */
    const uint8x16_t zero = vdupq_n_u8(0);
    const uint8x16_t mid_lo_shifted_left = vextq_u8(zero, r_mid, 8);
    const uint8x16_t mid_hi_shifted_right = vextq_u8(r_mid, zero, 8);
    const uint8x16_t lo = veorq_u8(r_lo, mid_lo_shifted_left);
    const uint8x16_t hi = veorq_u8(r_hi, mid_hi_shifted_right);

    /* Two-stage Montgomery-style reduction.
     *   p64 = 0xc200000000000000 in the low 64 bits, 0 in the high.
     *   a = clmul(lo.lo, p64.lo)
     *   b = swap_hi_lo(lo) XOR a
     *   c = clmul(b.lo, p64.lo)
     *   d = swap_hi_lo(b) XOR c
     *   out = d XOR hi
     */
    const poly64_t p64 = (poly64_t)0xc200000000000000ULL;
    const uint8x16_t a = p128ToU8(vmull_p64(lo64(lo), p64));
    /* swap_hi_lo: rotate by 8 bytes. */
    const uint8x16_t lo_swapped = vextq_u8(lo, lo, 8);
    const uint8x16_t b = veorq_u8(lo_swapped, a);
    const uint8x16_t c = p128ToU8(vmull_p64(lo64(b), p64));
    const uint8x16_t b_swapped = vextq_u8(b, b, 8);
    const uint8x16_t d = veorq_u8(b_swapped, c);
    return veorq_u8(d, hi);
}

/* GHASH absorb: XOR `data` (one 16-byte block) into `acc` then
 * multiply by H.  Caller already has `acc` and `h` in reflected
 * representation.  `block` is in wire (MSB-first) form. */
[[gnu::target("crypto"), gnu::always_inline]] static inline uint8x16_t
armGhashAbsorbBlock(uint8x16_t acc, uint8x16_t h, const uint8_t block[16]) {
    uint8x16_t b = vld1q_u8(block);
    b = armBswap128(b);
    acc = veorq_u8(acc, b);
    return armGhashMul(acc, h);
}

[[gnu::target("crypto")]] static uint8x16_t
armGhashAbsorb(uint8x16_t acc, uint8x16_t h, const uint8_t *data, size_t len) {
    while (len >= 16) {
        acc = armGhashAbsorbBlock(acc, h, data);
        data += 16;
        len -= 16;
    }
    if (len > 0) {
        uint8_t tail[16] = {};
        memcpy(tail, data, len);
        acc = armGhashAbsorbBlock(acc, h, tail);
    }
    return acc;
}

/* ====================================================================== */
/* GCTR (CTR-mode encrypt under inc32 counter)                            */
/* ====================================================================== */

[[gnu::target("crypto"), gnu::always_inline]] static inline uint8x16_t
armInc32Vec(uint8x16_t ctr) {
    /* Increment the big-endian 32-bit counter occupying the low 4
     * bytes of the second 64-bit lane (memory layout: bytes [12..15]).
     * Round-trip through scalar; the alternative is a careful byte-
     * swap + addition, but a stored-and-incremented uint32 keeps the
     * code obvious and is not on the critical path. */
    uint8_t tmp[16];
    vst1q_u8(tmp, ctr);
    dkStoreBe32(tmp + 12, dkLoadBe32(tmp + 12) + 1);
    return vld1q_u8(tmp);
}

[[gnu::target("crypto")]] static void armGctr(const uint8x16_t *rk,
                                              size_t num_rounds, uint8x16_t icb,
                                              const uint8_t *in, uint8_t *out,
                                              size_t len) {
    while (len >= 16) {
        uint8x16_t ks = aesArmEncBlock(rk, num_rounds, icb);
        uint8x16_t p = vld1q_u8(in);
        vst1q_u8(out, veorq_u8(p, ks));
        icb = armInc32Vec(icb);
        in += 16;
        out += 16;
        len -= 16;
    }
    if (len > 0) {
        uint8x16_t ks = aesArmEncBlock(rk, num_rounds, icb);
        uint8_t ks_bytes[16];
        vst1q_u8(ks_bytes, ks);
        for (size_t i = 0; i < len; i++) {
            out[i] = (uint8_t)(in[i] ^ ks_bytes[i]);
        }
    }
}

/* ====================================================================== */
/* Init / Seal / Open                                                     */
/* ====================================================================== */

[[gnu::target("crypto")]] bool
aesGcmArmCryptoInit(aesGcmKey *out, const uint8_t *key, size_t key_len) {
    if (out == nullptr || key == nullptr ||
        (key_len != AES128_KEY_LEN && key_len != AES256_KEY_LEN)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->key_size = (uint8_t)key_len;
    out->num_rounds = (key_len == AES128_KEY_LEN) ? 10u : 14u;

    /* The schedule in FIPS-197 byte order is each round key's 16 bytes
     * as the vector instructions load them; encryption is hardware */
    aesGcmKeyScheduleBytes(key, key_len, (uint8_t *)out->round_keys);
    const uint8x16_t *rk = (const uint8x16_t *)out->round_keys;

    /* Compute the GHASH subkey H = AES_K(0) and pre-shift it by 1 bit
     * in the bit-reflected representation so subsequent armGhashMul
     * calls don't have to pay the shift cost per-multiply.  Mirrors
     * the AES-NI backend's pre-shift exactly.
     *
     * Steps:
     *   1. h_wire = AES_K(0)                       // wire form (MSB-first)
     *   2. h0 = REV128(h_wire)                     // byte-reverse load
     *   3. carry = h0_hi.msb ? {0xc200..., 1} : 0  // GCM poly carry
     *   4. h_shifted = (h0 << 1) | carry           // 128-bit left shift */
    const uint8x16_t zero = vdupq_n_u8(0);
    const uint8x16_t h_wire = aesArmEncBlock(rk, out->num_rounds, zero);
    const uint8x16_t h0 = armBswap128(h_wire);

    /* h0 << 1 over the full 128 bits with inter-lane carry. */
    const uint64x2_t h0_u64 = vreinterpretq_u64_u8(h0);
    const uint64x2_t h0_shl_lane = vshlq_n_u64(h0_u64, 1);
    /* Inter-lane carry: high bit of low lane must carry into low bit
     * of high lane.  Build a vector where bit 63 of low lane is moved
     * to bit 0 of high lane.  We do this by computing (h0 >> 63)
     * per-lane (gives a 1 if MSB set), then sliding the low lane up
     * into the high lane via vextq_u8 (8-byte shift left). */
    const uint64x2_t h0_msb_per_lane = vshrq_n_u64(h0_u64, 63);
    const uint8x16_t carry_low =
        vextq_u8(vreinterpretq_u8_u64(vdupq_n_u64(0)),
                 vreinterpretq_u8_u64(h0_msb_per_lane), 8);
    const uint8x16_t h_shifted =
        veorq_u8(vreinterpretq_u8_u64(h0_shl_lane), carry_low);

    /* Build the GCM-polynomial carry: 0xc200000000000000:0x1 if h0's
     * high-lane MSB was 1, else zero.  We broadcast the high-lane MSB
     * across both lanes as a 0/-1 mask and AND with the polynomial
     * constant. */
    const uint64_t h0_hi_msb_u64 = vgetq_lane_u64(h0_msb_per_lane, 1);
    /* mask is 0xFFFF...FFFF if h0_hi_msb was 1, else 0. */
    const uint64_t mask_u64 = (uint64_t)0 - h0_hi_msb_u64;
    const uint64x2_t mask = vsetq_lane_u64(mask_u64, vdupq_n_u64(mask_u64), 0);
    /* Polynomial constant: low lane = 1, high lane = 0xc200000000000000. */
    const uint64x2_t poly_const_low_lane_one = vsetq_lane_u64(
        (uint64_t)1, vdupq_n_u64((uint64_t)0xc200000000000000ULL), 0);
    const uint8x16_t carry_poly =
        vreinterpretq_u8_u64(vandq_u64(poly_const_low_lane_one, mask));

    const uint8x16_t h = veorq_u8(h_shifted, carry_poly);
    vst1q_u8(out->h, h);
    return true;
}

[[gnu::target("crypto")]] bool
aesGcmArmCryptoSeal(const aesGcmKey *k, const uint8_t nonce[AES_GCM_NONCE_LEN],
                    const uint8_t *aad, size_t aad_len,
                    const uint8_t *plaintext, size_t plaintext_len,
                    uint8_t *ciphertext, uint8_t tag[AES_GCM_TAG_LEN]) {
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

    const uint8x16_t *rk = (const uint8x16_t *)k->round_keys;

    /* J0 = nonce || 0x00000001 */
    uint8_t J0_bytes[16];
    memcpy(J0_bytes, nonce, 12);
    J0_bytes[12] = 0;
    J0_bytes[13] = 0;
    J0_bytes[14] = 0;
    J0_bytes[15] = 1;
    uint8x16_t J0 = vld1q_u8(J0_bytes);

    /* Encrypt with inc32(J0) as initial counter. */
    uint8x16_t icb = armInc32Vec(J0);
    armGctr(rk, k->num_rounds, icb, plaintext, ciphertext, plaintext_len);

    /* Tag = AES_K(J0) XOR GHASH(AAD || 0^v || CT || 0^u || lens).
     * H was pre-shifted + bit-reflected during Init; load as-is. */
    const uint8x16_t h = vld1q_u8(k->h);
    uint8x16_t acc = vdupq_n_u8(0);
    acc = armGhashAbsorb(acc, h, aad, aad_len);
    acc = armGhashAbsorb(acc, h, ciphertext, plaintext_len);
    uint8_t lens[16];
    const uint64_t aad_bits = (uint64_t)aad_len * 8u;
    const uint64_t ct_bits = (uint64_t)plaintext_len * 8u;
    for (uint32_t i = 0; i < 8; i++) {
        lens[i] = (uint8_t)(aad_bits >> (56 - i * 8));
        lens[8 + i] = (uint8_t)(ct_bits >> (56 - i * 8));
    }
    acc = armGhashAbsorb(acc, h, lens, 16);

    /* Convert acc back to wire form and XOR with AES_K(J0). */
    uint8x16_t acc_wire = armBswap128(acc);
    uint8x16_t enc_J0 = aesArmEncBlock(rk, k->num_rounds, J0);
    uint8x16_t tag_v = veorq_u8(acc_wire, enc_J0);
    vst1q_u8(tag, tag_v);
    return true;
}

[[gnu::target("crypto")]] bool
aesGcmArmCryptoOpen(const aesGcmKey *k, const uint8_t nonce[AES_GCM_NONCE_LEN],
                    const uint8_t *aad, size_t aad_len,
                    const uint8_t *ciphertext, size_t ciphertext_len,
                    const uint8_t tag[AES_GCM_TAG_LEN], uint8_t *plaintext) {
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

    const uint8x16_t *rk = (const uint8x16_t *)k->round_keys;

    uint8_t J0_bytes[16];
    memcpy(J0_bytes, nonce, 12);
    J0_bytes[12] = 0;
    J0_bytes[13] = 0;
    J0_bytes[14] = 0;
    J0_bytes[15] = 1;
    uint8x16_t J0 = vld1q_u8(J0_bytes);

    /* Compute expected tag first so we can constant-time compare
     * before decrypting; on mismatch we DCE-resistantly zero the
     * caller's plaintext buffer.  H was pre-shifted + bit-reflected
     * during Init; load as-is. */
    const uint8x16_t h = vld1q_u8(k->h);
    uint8x16_t acc = vdupq_n_u8(0);
    acc = armGhashAbsorb(acc, h, aad, aad_len);
    acc = armGhashAbsorb(acc, h, ciphertext, ciphertext_len);
    uint8_t lens[16];
    const uint64_t aad_bits = (uint64_t)aad_len * 8u;
    const uint64_t ct_bits = (uint64_t)ciphertext_len * 8u;
    for (uint32_t i = 0; i < 8; i++) {
        lens[i] = (uint8_t)(aad_bits >> (56 - i * 8));
        lens[8 + i] = (uint8_t)(ct_bits >> (56 - i * 8));
    }
    acc = armGhashAbsorb(acc, h, lens, 16);

    uint8x16_t acc_wire = armBswap128(acc);
    uint8x16_t enc_J0 = aesArmEncBlock(rk, k->num_rounds, J0);
    uint8_t computed_tag[16];
    vst1q_u8(computed_tag, veorq_u8(acc_wire, enc_J0));
    if (!dkCtMemEq16(computed_tag, tag)) {
        if (plaintext != nullptr && ciphertext_len > 0) {
            dkSecureZero(plaintext, ciphertext_len);
        }
        return false;
    }

    /* Tag matched; decrypt. */
    uint8x16_t icb = armInc32Vec(J0);
    armGctr(rk, k->num_rounds, icb, ciphertext, plaintext, ciphertext_len);
    return true;
}

#else /* !aarch64-with-crypto — soft stubs; Available() must always link. */

bool aesGcmArmCryptoAvailable(void) {
    return false;
}

/* Stubs return false; reaching them means the caller ignored the
 * Available() probe.  Defensive: don't claim success on platforms
 * where we can't actually run. */
bool aesGcmArmCryptoInit(aesGcmKey *, const uint8_t *, size_t) {
    return false;
}

bool aesGcmArmCryptoSeal(const aesGcmKey *, const uint8_t[AES_GCM_NONCE_LEN],
                         const uint8_t *, size_t, const uint8_t *, size_t,
                         uint8_t *, uint8_t[AES_GCM_TAG_LEN]) {
    return false;
}

bool aesGcmArmCryptoOpen(const aesGcmKey *, const uint8_t[AES_GCM_NONCE_LEN],
                         const uint8_t *, size_t, const uint8_t *, size_t,
                         const uint8_t[AES_GCM_TAG_LEN], uint8_t *) {
    return false;
}

#endif /* aarch64-with-crypto */
