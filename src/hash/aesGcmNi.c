/* AES-GCM — AES-NI + PCLMUL accelerated backend (x86-64).
 *
 * Public API mirrors aesGcm.c (Init/Seal/Open) plus an aesGcmNiAvailable()
 * probe.  The dispatcher in aesGcm.c routes to this backend when the
 * probe returns true.
 *
 * Reference: Intel "Carry-Less Multiplication Instruction and its
 * Usage for Computing the GCM Mode" white paper (Gueron + Kounavis).
 *
 * Layout:
 *   - x86-only block guarded by `#if defined(__x86_64__)`
 *   - Soft stubs on every other platform so the symbol set is stable
 *     and direct callers crash if (and only if) they ignore the probe.
 *
 * Storage model: we piggyback on the existing `aesGcmKey` struct from
 * aesGcm.h.  `round_keys` is `uint32_t[60]` = 240 bytes = 15 * 16-byte
 * AES round keys (enough for AES-256's 14+1).  The NI key schedule
 * writes its expanded keys directly into this region as raw 16-byte
 * blocks.  `h[16]` holds the GHASH subkey H = AES_K(0).  `key_size`
 * and `num_rounds` keep their portable meanings.  The portable
 * round_keys layout (big-endian 32-bit words) and the NI layout (raw
 * little-endian 16-byte blocks) are NOT compatible — but a key
 * struct only ever sees one backend's writes followed by reads from
 * the same backend (the dispatcher pins both calls to one path), so
 * this is safe in practice.
 *
 * GHASH model: bits in GCM are processed MSB-first ("reflected" with
 * respect to xmm's natural LSB-first order).  We byte-swap input
 * blocks once on load and once on store, then do the carry-less
 * multiply in the natural representation with a single-bit
 * left-shift at the end to compensate for the bit-reflection
 * mismatch.  Reduction folds the high 128 bits down through the GCM
 * polynomial x^128 + x^7 + x^2 + x + 1 using two more clmul + xor
 * + shift sequences (the standard "Algorithm 5" reduction).
 *
 * Correctness is verified by `dkTestAesNi`: every existing aesGcm
 * test vector is run through both the portable path and the NI path
 * and the ciphertext+tag bytes are compared. */

#include "aesGcmNi.h"

#include "../ctmem.h" /* dkCtMemEq16, dkSecureZero */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__x86_64__)

#include <emmintrin.h> /* SSE2 base */
#include <tmmintrin.h> /* SSSE3: _mm_shuffle_epi8 */
#include <wmmintrin.h> /* AES-NI + PCLMUL */

/* ====================================================================== */
/* CPU probe                                                              */
/* ====================================================================== */

bool aesGcmNiAvailable(void) {
    __builtin_cpu_init();
    return __builtin_cpu_supports("aes") != 0 &&
           __builtin_cpu_supports("pclmul") != 0;
}

/* ====================================================================== */
/* Key schedule (AES-128 + AES-256)                                       */
/* ====================================================================== */

/* Per AES-128 the standard "assist" pattern: rot+sub+rcon a copy of
 * the previous round key's word3, fold the temp into each of the four
 * words of the new round key. */
__attribute__((target("aes,pclmul,sse4.1"),
               always_inline)) static inline __m128i
aes128KeyExpandAssist(__m128i prev, __m128i temp) {
    temp = _mm_shuffle_epi32(temp, 0xff);
    prev = _mm_xor_si128(prev, _mm_slli_si128(prev, 4));
    prev = _mm_xor_si128(prev, _mm_slli_si128(prev, 4));
    prev = _mm_xor_si128(prev, _mm_slli_si128(prev, 4));
    return _mm_xor_si128(prev, temp);
}

__attribute__((target("aes,pclmul,sse4.1"))) static void
aes128KeyExpand(__m128i rk[11], const uint8_t key[16]) {
    rk[0] = _mm_loadu_si128((const __m128i *)key);
    rk[1] =
        aes128KeyExpandAssist(rk[0], _mm_aeskeygenassist_si128(rk[0], 0x01));
    rk[2] =
        aes128KeyExpandAssist(rk[1], _mm_aeskeygenassist_si128(rk[1], 0x02));
    rk[3] =
        aes128KeyExpandAssist(rk[2], _mm_aeskeygenassist_si128(rk[2], 0x04));
    rk[4] =
        aes128KeyExpandAssist(rk[3], _mm_aeskeygenassist_si128(rk[3], 0x08));
    rk[5] =
        aes128KeyExpandAssist(rk[4], _mm_aeskeygenassist_si128(rk[4], 0x10));
    rk[6] =
        aes128KeyExpandAssist(rk[5], _mm_aeskeygenassist_si128(rk[5], 0x20));
    rk[7] =
        aes128KeyExpandAssist(rk[6], _mm_aeskeygenassist_si128(rk[6], 0x40));
    rk[8] =
        aes128KeyExpandAssist(rk[7], _mm_aeskeygenassist_si128(rk[7], 0x80));
    rk[9] =
        aes128KeyExpandAssist(rk[8], _mm_aeskeygenassist_si128(rk[8], 0x1b));
    rk[10] =
        aes128KeyExpandAssist(rk[9], _mm_aeskeygenassist_si128(rk[9], 0x36));
}

/* AES-256 key expansion.  The schedule alternates between two patterns:
 *   "round A": rot+sub+rcon, fold into the (i-8)th round key
 *   "round B": sub only,     fold into the (i-8)th round key
 * Note that aeskeygenassist's second arg MUST be a compile-time
 * constant integer, so the AES-256 schedule is fully unrolled below
 * (no loop indexing into a runtime rcons[] table).  The schedule
 * produces 15 round keys total (rk[0..14]) — 14 rounds + initial. */
__attribute__((target("aes,pclmul,sse4.1"),
               always_inline)) static inline __m128i
aes256ExpandA(__m128i prev_minus_one, __m128i assisted) {
    /* assisted = _mm_aeskeygenassist_si128(prev, RCON) with the relevant
     * RCON.  Caller produces it because RCON must be an immediate. */
    assisted = _mm_shuffle_epi32(assisted, 0xff);
    __m128i v = prev_minus_one;
    v = _mm_xor_si128(v, _mm_slli_si128(v, 4));
    v = _mm_xor_si128(v, _mm_slli_si128(v, 4));
    v = _mm_xor_si128(v, _mm_slli_si128(v, 4));
    return _mm_xor_si128(v, assisted);
}

__attribute__((target("aes,pclmul,sse4.1"),
               always_inline)) static inline __m128i
aes256ExpandB(__m128i prev_minus_one, __m128i prev) {
    __m128i assisted = _mm_aeskeygenassist_si128(prev, 0x00);
    assisted = _mm_shuffle_epi32(assisted, 0xaa);
    __m128i v = prev_minus_one;
    v = _mm_xor_si128(v, _mm_slli_si128(v, 4));
    v = _mm_xor_si128(v, _mm_slli_si128(v, 4));
    v = _mm_xor_si128(v, _mm_slli_si128(v, 4));
    return _mm_xor_si128(v, assisted);
}

__attribute__((target("aes,pclmul,sse4.1"))) static void
aes256KeyExpand(__m128i rk[15], const uint8_t key[32]) {
    rk[0] = _mm_loadu_si128((const __m128i *)(key + 0));
    rk[1] = _mm_loadu_si128((const __m128i *)(key + 16));
    rk[2] = aes256ExpandA(rk[0], _mm_aeskeygenassist_si128(rk[1], 0x01));
    rk[3] = aes256ExpandB(rk[1], rk[2]);
    rk[4] = aes256ExpandA(rk[2], _mm_aeskeygenassist_si128(rk[3], 0x02));
    rk[5] = aes256ExpandB(rk[3], rk[4]);
    rk[6] = aes256ExpandA(rk[4], _mm_aeskeygenassist_si128(rk[5], 0x04));
    rk[7] = aes256ExpandB(rk[5], rk[6]);
    rk[8] = aes256ExpandA(rk[6], _mm_aeskeygenassist_si128(rk[7], 0x08));
    rk[9] = aes256ExpandB(rk[7], rk[8]);
    rk[10] = aes256ExpandA(rk[8], _mm_aeskeygenassist_si128(rk[9], 0x10));
    rk[11] = aes256ExpandB(rk[9], rk[10]);
    rk[12] = aes256ExpandA(rk[10], _mm_aeskeygenassist_si128(rk[11], 0x20));
    rk[13] = aes256ExpandB(rk[11], rk[12]);
    rk[14] = aes256ExpandA(rk[12], _mm_aeskeygenassist_si128(rk[13], 0x40));
}

/* ====================================================================== */
/* Block encrypt                                                          */
/* ====================================================================== */

/* AES-128 single block: XOR with round-0, aesenc rounds 1..9,
 * aesenclast round 10. */
__attribute__((target("aes,pclmul,sse4.1"),
               always_inline)) static inline __m128i
aes128EncBlock(const __m128i rk[11], __m128i state) {
    state = _mm_xor_si128(state, rk[0]);
    state = _mm_aesenc_si128(state, rk[1]);
    state = _mm_aesenc_si128(state, rk[2]);
    state = _mm_aesenc_si128(state, rk[3]);
    state = _mm_aesenc_si128(state, rk[4]);
    state = _mm_aesenc_si128(state, rk[5]);
    state = _mm_aesenc_si128(state, rk[6]);
    state = _mm_aesenc_si128(state, rk[7]);
    state = _mm_aesenc_si128(state, rk[8]);
    state = _mm_aesenc_si128(state, rk[9]);
    state = _mm_aesenclast_si128(state, rk[10]);
    return state;
}

/* AES-256 single block: XOR with round-0, aesenc rounds 1..13,
 * aesenclast round 14. */
__attribute__((target("aes,pclmul,sse4.1"),
               always_inline)) static inline __m128i
aes256EncBlock(const __m128i rk[15], __m128i state) {
    state = _mm_xor_si128(state, rk[0]);
    state = _mm_aesenc_si128(state, rk[1]);
    state = _mm_aesenc_si128(state, rk[2]);
    state = _mm_aesenc_si128(state, rk[3]);
    state = _mm_aesenc_si128(state, rk[4]);
    state = _mm_aesenc_si128(state, rk[5]);
    state = _mm_aesenc_si128(state, rk[6]);
    state = _mm_aesenc_si128(state, rk[7]);
    state = _mm_aesenc_si128(state, rk[8]);
    state = _mm_aesenc_si128(state, rk[9]);
    state = _mm_aesenc_si128(state, rk[10]);
    state = _mm_aesenc_si128(state, rk[11]);
    state = _mm_aesenc_si128(state, rk[12]);
    state = _mm_aesenc_si128(state, rk[13]);
    state = _mm_aesenclast_si128(state, rk[14]);
    return state;
}

/* Wrapper that picks 128 vs 256 by num_rounds. */
__attribute__((target("aes,pclmul,sse4.1"),
               always_inline)) static inline __m128i
aesEncBlock(const __m128i *rk, uint8_t num_rounds, __m128i state) {
    if (num_rounds == 10) {
        return aes128EncBlock(rk, state);
    }
    return aes256EncBlock(rk, state);
}

/* ====================================================================== */
/* GHASH (PCLMUL + reflection)                                            */
/* ====================================================================== */

/* Byte-reverse mask used to swap MSB-first (GCM wire) <-> LSB-first
 * (xmm natural) within a 128-bit lane. */
__attribute__((target("aes,pclmul,sse4.1"),
               always_inline)) static inline __m128i
bswapMask(void) {
    return _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
}

/* Multiply two 128-bit GCM elements.  Both inputs are in the
 * "reflected" xmm representation (each block has been byte-reversed
 * via bswapMask on load).  The `h` argument MUST already have been
 * pre-shifted by one bit during key setup (see aesGcmNiInit) so we
 * avoid an expensive bit-shift on every multiply.
 *
 * Reference: libsodium's aead_aes256gcm_aesni.c (clmul128 +
 * gcm_reduce).  Pattern:
 *   1. Schoolbook 4-clmul -> (hi, mid, lo) decomposed product.
 *   2. Fold the 128-bit middle term into hi:lo via byte shifts.
 *   3. Two-pass Montgomery-style reduction using
 *      p64 = 0xc200000000000000:0x0000000000000000 (the upper half
 *      of the bit-reflected GCM polynomial). */
__attribute__((target("aes,pclmul,sse4.1"),
               always_inline)) static inline __m128i
ghashMul(__m128i x, __m128i h) {
    /* Schoolbook 4-clmul (textbook is competitive with Karatsuba on
     * modern CPUs and simpler to verify). */
    const __m128i r_lo = _mm_clmulepi64_si128(x, h, 0x00);
    const __m128i r_hi = _mm_clmulepi64_si128(x, h, 0x11);
    const __m128i r_mid = _mm_xor_si128(_mm_clmulepi64_si128(x, h, 0x10),
                                        _mm_clmulepi64_si128(x, h, 0x01));

    /* Fold mid (128 bits centered on bit 64..191) into hi:lo (256
     * bits).  Low half of mid joins the high half of lo; high half
     * of mid joins the low half of hi.  Byte shifts only. */
    const __m128i hi = _mm_xor_si128(r_hi, _mm_srli_si128(r_mid, 8));
    const __m128i lo = _mm_xor_si128(r_lo, _mm_slli_si128(r_mid, 8));

    /* Reduction (libsodium gcm_reduce).  p64 holds 0xc2... in the
     * LOW 64 bits and zero in the HIGH 64 bits — important: the
     * polynomial constant lives in the low qword, not the high. */
    const __m128i p64 =
        _mm_set_epi64x((long long)0, (long long)0xc200000000000000ULL);
    const __m128i a = _mm_clmulepi64_si128(lo, p64, 0x00);
    const __m128i b = _mm_xor_si128(_mm_shuffle_epi32(lo, 0x4e), a);
    const __m128i c = _mm_clmulepi64_si128(b, p64, 0x00);
    const __m128i d = _mm_xor_si128(_mm_shuffle_epi32(b, 0x4e), c);
    return _mm_xor_si128(d, hi);
}

/* GHASH absorb: XOR `data` (one 16-byte block) into `acc` then
 * multiply by H.  Caller already has `acc` and `h` in reflected
 * representation.  `block` is in wire (MSB-first) form. */
__attribute__((target("aes,pclmul,sse4.1"),
               always_inline)) static inline __m128i
ghashAbsorbBlock(__m128i acc, __m128i h, const uint8_t block[16]) {
    __m128i b = _mm_loadu_si128((const __m128i *)block);
    b = _mm_shuffle_epi8(b, bswapMask());
    acc = _mm_xor_si128(acc, b);
    return ghashMul(acc, h);
}

__attribute__((target("aes,pclmul,sse4.1"))) static __m128i
ghashAbsorb(__m128i acc, __m128i h, const uint8_t *data, size_t len) {
    while (len >= 16) {
        acc = ghashAbsorbBlock(acc, h, data);
        data += 16;
        len -= 16;
    }
    if (len > 0) {
        uint8_t tail[16] = {0};
        memcpy(tail, data, len);
        acc = ghashAbsorbBlock(acc, h, tail);
    }
    return acc;
}

/* ====================================================================== */
/* GCTR (CTR-mode encrypt under inc32 counter)                            */
/* ====================================================================== */

__attribute__((target("aes,pclmul,sse4.1"),
               always_inline)) static inline __m128i
inc32Vec(__m128i ctr) {
    /* Increment the big-endian 32-bit counter occupying the high 32
     * bits of the second 64-bit lane (memory layout: bytes [12..15]). */
    /* Easiest: round-trip through scalar; the alternative is a careful
     * byte-swap + addition, but a stored-and-incremented uint32 keeps
     * the code obvious and is not on the critical path (the encrypt
     * dwarfs it). */
    uint8_t tmp[16];
    _mm_storeu_si128((__m128i *)tmp, ctr);
    uint32_t c = ((uint32_t)tmp[12] << 24) | ((uint32_t)tmp[13] << 16) |
                 ((uint32_t)tmp[14] << 8) | (uint32_t)tmp[15];
    c++;
    tmp[12] = (uint8_t)(c >> 24);
    tmp[13] = (uint8_t)(c >> 16);
    tmp[14] = (uint8_t)(c >> 8);
    tmp[15] = (uint8_t)c;
    return _mm_loadu_si128((const __m128i *)tmp);
}

__attribute__((target("aes,pclmul,sse4.1"))) static void
gctr(const __m128i *rk, uint8_t num_rounds, __m128i icb, const uint8_t *in,
     uint8_t *out, size_t len) {
    while (len >= 16) {
        __m128i ks = aesEncBlock(rk, num_rounds, icb);
        __m128i p = _mm_loadu_si128((const __m128i *)in);
        _mm_storeu_si128((__m128i *)out, _mm_xor_si128(p, ks));
        icb = inc32Vec(icb);
        in += 16;
        out += 16;
        len -= 16;
    }
    if (len > 0) {
        __m128i ks = aesEncBlock(rk, num_rounds, icb);
        uint8_t ks_bytes[16];
        _mm_storeu_si128((__m128i *)ks_bytes, ks);
        for (size_t i = 0; i < len; i++) {
            out[i] = (uint8_t)(in[i] ^ ks_bytes[i]);
        }
    }
}

/* ====================================================================== */
/* Init / Seal / Open                                                     */
/* ====================================================================== */

__attribute__((target("aes,pclmul,sse4.1"))) bool
aesGcmNiInit(aesGcmKey *out, const uint8_t *key, size_t key_len) {
    if (out == NULL || key == NULL ||
        (key_len != AES128_KEY_LEN && key_len != AES256_KEY_LEN)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->key_size = (uint8_t)key_len;
    out->num_rounds = (key_len == AES128_KEY_LEN) ? 10u : 14u;

    __m128i *rk = (__m128i *)out->round_keys;
    if (key_len == AES128_KEY_LEN) {
        aes128KeyExpand(rk, key);
    } else {
        aes256KeyExpand(rk, key);
    }

    /* Compute the GHASH subkey H = AES_K(0) and pre-shift it by 1 bit
     * in the bit-reflected representation so subsequent ghashMul calls
     * don't have to pay the shift cost per-multiply.  Reference:
     * libsodium aead_aes256gcm_aesni.c precomp_for_block_count.
     *
     * Steps:
     *   1. h0 = REV128(AES_K(0))                  // byte-reverse load
     *   2. carry = h0_hi.msb ? {0xc200..., 1} : 0  // GCM poly carry
     *   3. h_shifted = (h0 << 1) | carry           // bit-shift over 128 bits
     *
     * We store the pre-shifted, byte-reversed H in `out->h` for use by
     * Seal/Open's ghashMul calls.  This means out->h is NOT in wire
     * form — it's the precomputed multiplier, distinct from the
     * portable backend's storage convention.  That's safe because
     * the dispatcher pins all calls through the NI backend once it
     * has initialised the key. */
    const __m128i zero = _mm_setzero_si128();
    const __m128i h_wire = aesEncBlock(rk, out->num_rounds, zero);
    const __m128i h0 = _mm_shuffle_epi8(h_wire, bswapMask());

    /* Build the GCM-polynomial carry: 0xc200000000000000:0x1 if h0's
     * high-lane MSB was 1, else zero.  Use lane-3 broadcast of the
     * "all bits set if MSB=1" mask to drive both halves of carry. */
    const __m128i poly_carry_const =
        _mm_set_epi64x((long long)0xc200000000000000LL, (long long)1LL);
    const __m128i h0_msb = _mm_srli_epi64(h0, 63); /* MSB per lane */
    __m128i mask = _mm_sub_epi64(_mm_setzero_si128(), h0_msb);
    mask = _mm_shuffle_epi32(mask, 0xff); /* broadcast lane-3 (h0_hi MSB) */
    const __m128i carry = _mm_and_si128(poly_carry_const, mask);

    /* h0 << 1 over the full 128 bits (inter-lane carry). */
    const __m128i h0_shl_lane = _mm_slli_epi64(h0, 1);
    const __m128i h0_carry = _mm_srli_epi64(_mm_slli_si128(h0, 8), 63);
    const __m128i h_shifted = _mm_or_si128(h0_shl_lane, h0_carry);

    const __m128i h = _mm_xor_si128(h_shifted, carry);
    _mm_storeu_si128((__m128i *)out->h, h);
    return true;
}

__attribute__((target("aes,pclmul,sse4.1"))) bool
aesGcmNiSeal(const aesGcmKey *k, const uint8_t nonce[AES_GCM_NONCE_LEN],
             const uint8_t *aad, size_t aad_len, const uint8_t *plaintext,
             size_t plaintext_len, uint8_t *ciphertext,
             uint8_t tag[AES_GCM_TAG_LEN]) {
    if (k == NULL || nonce == NULL || tag == NULL) {
        return false;
    }
    if (plaintext_len > AES_GCM_MAX_PAYLOAD) {
        return false;
    }
    if (plaintext_len > 0 && (plaintext == NULL || ciphertext == NULL)) {
        return false;
    }
    if (aad_len > 0 && aad == NULL) {
        return false;
    }

    const __m128i *rk = (const __m128i *)k->round_keys;

    /* J0 = nonce || 0x00000001 */
    uint8_t J0_bytes[16];
    memcpy(J0_bytes, nonce, 12);
    J0_bytes[12] = 0;
    J0_bytes[13] = 0;
    J0_bytes[14] = 0;
    J0_bytes[15] = 1;
    __m128i J0 = _mm_loadu_si128((const __m128i *)J0_bytes);

    /* Encrypt with inc32(J0) as initial counter. */
    __m128i icb = inc32Vec(J0);
    gctr(rk, k->num_rounds, icb, plaintext, ciphertext, plaintext_len);

    /* Tag = AES_K(J0) XOR GHASH(AAD || 0^v || CT || 0^u || lens).
     * H was pre-shifted + bit-reflected during Init; load as-is. */
    const __m128i h = _mm_loadu_si128((const __m128i *)k->h);
    __m128i acc = _mm_setzero_si128();
    acc = ghashAbsorb(acc, h, aad, aad_len);
    acc = ghashAbsorb(acc, h, ciphertext, plaintext_len);
    uint8_t lens[16];
    const uint64_t aad_bits = (uint64_t)aad_len * 8u;
    const uint64_t ct_bits = (uint64_t)plaintext_len * 8u;
    for (uint32_t i = 0; i < 8; i++) {
        lens[i] = (uint8_t)(aad_bits >> (56 - i * 8));
        lens[8 + i] = (uint8_t)(ct_bits >> (56 - i * 8));
    }
    acc = ghashAbsorb(acc, h, lens, 16);

    /* Convert acc back to wire form and XOR with AES_K(J0). */
    __m128i acc_wire = _mm_shuffle_epi8(acc, bswapMask());
    __m128i enc_J0 = aesEncBlock(rk, k->num_rounds, J0);
    __m128i tag_v = _mm_xor_si128(acc_wire, enc_J0);
    _mm_storeu_si128((__m128i *)tag, tag_v);
    return true;
}

__attribute__((target("aes,pclmul,sse4.1"))) bool
aesGcmNiOpen(const aesGcmKey *k, const uint8_t nonce[AES_GCM_NONCE_LEN],
             const uint8_t *aad, size_t aad_len, const uint8_t *ciphertext,
             size_t ciphertext_len, const uint8_t tag[AES_GCM_TAG_LEN],
             uint8_t *plaintext) {
    if (k == NULL || nonce == NULL || tag == NULL) {
        return false;
    }
    if (ciphertext_len > AES_GCM_MAX_PAYLOAD) {
        return false;
    }
    if (ciphertext_len > 0 && (ciphertext == NULL || plaintext == NULL)) {
        return false;
    }
    if (aad_len > 0 && aad == NULL) {
        return false;
    }

    const __m128i *rk = (const __m128i *)k->round_keys;

    uint8_t J0_bytes[16];
    memcpy(J0_bytes, nonce, 12);
    J0_bytes[12] = 0;
    J0_bytes[13] = 0;
    J0_bytes[14] = 0;
    J0_bytes[15] = 1;
    __m128i J0 = _mm_loadu_si128((const __m128i *)J0_bytes);

    /* Compute expected tag first so we can constant-time compare
     * before decrypting; on mismatch we DCE-resistantly zero the
     * caller's plaintext buffer.  H was pre-shifted + bit-reflected
     * during Init; load as-is. */
    const __m128i h = _mm_loadu_si128((const __m128i *)k->h);
    __m128i acc = _mm_setzero_si128();
    acc = ghashAbsorb(acc, h, aad, aad_len);
    acc = ghashAbsorb(acc, h, ciphertext, ciphertext_len);
    uint8_t lens[16];
    const uint64_t aad_bits = (uint64_t)aad_len * 8u;
    const uint64_t ct_bits = (uint64_t)ciphertext_len * 8u;
    for (uint32_t i = 0; i < 8; i++) {
        lens[i] = (uint8_t)(aad_bits >> (56 - i * 8));
        lens[8 + i] = (uint8_t)(ct_bits >> (56 - i * 8));
    }
    acc = ghashAbsorb(acc, h, lens, 16);

    __m128i acc_wire = _mm_shuffle_epi8(acc, bswapMask());
    __m128i enc_J0 = aesEncBlock(rk, k->num_rounds, J0);
    uint8_t computed_tag[16];
    _mm_storeu_si128((__m128i *)computed_tag, _mm_xor_si128(acc_wire, enc_J0));
    if (!dkCtMemEq16(computed_tag, tag)) {
        if (plaintext != NULL && ciphertext_len > 0) {
            dkSecureZero(plaintext, ciphertext_len);
        }
        return false;
    }

    /* Tag matched; decrypt. */
    __m128i icb = inc32Vec(J0);
    gctr(rk, k->num_rounds, icb, ciphertext, plaintext, ciphertext_len);
    return true;
}

#else /* !x86_64 — soft stubs; aesGcmNiAvailable() must always link. */

bool aesGcmNiAvailable(void) {
    return false;
}

/* Stubs return false; reaching them means the caller ignored the
 * Available() probe.  Defensive: don't claim success on platforms
 * where we can't actually run. */
bool aesGcmNiInit(aesGcmKey *out, const uint8_t *key, size_t key_len) {
    (void)out;
    (void)key;
    (void)key_len;
    return false;
}

bool aesGcmNiSeal(const aesGcmKey *k, const uint8_t nonce[AES_GCM_NONCE_LEN],
                  const uint8_t *aad, size_t aad_len, const uint8_t *plaintext,
                  size_t plaintext_len, uint8_t *ciphertext,
                  uint8_t tag[AES_GCM_TAG_LEN]) {
    (void)k;
    (void)nonce;
    (void)aad;
    (void)aad_len;
    (void)plaintext;
    (void)plaintext_len;
    (void)ciphertext;
    (void)tag;
    return false;
}

bool aesGcmNiOpen(const aesGcmKey *k, const uint8_t nonce[AES_GCM_NONCE_LEN],
                  const uint8_t *aad, size_t aad_len, const uint8_t *ciphertext,
                  size_t ciphertext_len, const uint8_t tag[AES_GCM_TAG_LEN],
                  uint8_t *plaintext) {
    (void)k;
    (void)nonce;
    (void)aad;
    (void)aad_len;
    (void)ciphertext;
    (void)ciphertext_len;
    (void)tag;
    (void)plaintext;
    return false;
}

#endif /* x86_64 */
