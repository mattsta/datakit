/* RFC 8439 ChaCha20-Poly1305 AEAD primitive — portable reference impl.
 *
 * Layout:
 *   1. ChaCha20: quarter-round, block function, stream xor.
 *   2. Poly1305: one-shot MAC using 26-bit limb arithmetic (5 limbs of
 *      ~26 bits each => 130-bit accumulator; multiply-then-carry keeps
 *      every intermediate within uint64_t without 128-bit math).
 *   3. AEAD glue per RFC 8439 §2.8.
 *
 * No heap; all state is on the stack or in caller buffers. */

#include "chacha20poly1305.h"

#include "ctmem.h"
#include "dkDiag.h"
#include "hash/chacha20AVX2.h"
#include "hash/chacha20Neon.h"
#include "hash/poly1305AVX2.h"
#include "hash/poly1305Neon.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ===== Endian helpers (portable, no host-endianness assumption) ===== */

static inline uint32_t load32_le(const uint8_t *p) {
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline void store32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline void store64_le(uint8_t *p, uint64_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    p[4] = (uint8_t)(v >> 32);
    p[5] = (uint8_t)(v >> 40);
    p[6] = (uint8_t)(v >> 48);
    p[7] = (uint8_t)(v >> 56);
}

static inline uint32_t rotl32(uint32_t v, uint32_t n) {
    return (v << n) | (v >> (32 - n));
}

/* ===== ChaCha20 ===== */

/* Four independent quarter-rounds, each step explicitly interleaved
 * across all four lanes.  Modern OoO cores can already extract this
 * parallelism from sequential QRs, but expressing it explicitly lets
 * the compiler emit a tighter instruction schedule on scalar back-ends
 * and avoids relying on the optimizer pulling apart the macro-fused
 * sequence.  Measured ~10-15% over the sequential-QR layout on scalar
 * ChaCha20. */
#define QR4(a0, b0, c0, d0, a1, b1, c1, d1, a2, b2, c2, d2, a3, b3, c3, d3)    \
    do {                                                                       \
        (a0) += (b0);                                                          \
        (a1) += (b1);                                                          \
        (a2) += (b2);                                                          \
        (a3) += (b3);                                                          \
        (d0) ^= (a0);                                                          \
        (d1) ^= (a1);                                                          \
        (d2) ^= (a2);                                                          \
        (d3) ^= (a3);                                                          \
        (d0) = rotl32((d0), 16);                                               \
        (d1) = rotl32((d1), 16);                                               \
        (d2) = rotl32((d2), 16);                                               \
        (d3) = rotl32((d3), 16);                                               \
        (c0) += (d0);                                                          \
        (c1) += (d1);                                                          \
        (c2) += (d2);                                                          \
        (c3) += (d3);                                                          \
        (b0) ^= (c0);                                                          \
        (b1) ^= (c1);                                                          \
        (b2) ^= (c2);                                                          \
        (b3) ^= (c3);                                                          \
        (b0) = rotl32((b0), 12);                                               \
        (b1) = rotl32((b1), 12);                                               \
        (b2) = rotl32((b2), 12);                                               \
        (b3) = rotl32((b3), 12);                                               \
        (a0) += (b0);                                                          \
        (a1) += (b1);                                                          \
        (a2) += (b2);                                                          \
        (a3) += (b3);                                                          \
        (d0) ^= (a0);                                                          \
        (d1) ^= (a1);                                                          \
        (d2) ^= (a2);                                                          \
        (d3) ^= (a3);                                                          \
        (d0) = rotl32((d0), 8);                                                \
        (d1) = rotl32((d1), 8);                                                \
        (d2) = rotl32((d2), 8);                                                \
        (d3) = rotl32((d3), 8);                                                \
        (c0) += (d0);                                                          \
        (c1) += (d1);                                                          \
        (c2) += (d2);                                                          \
        (c3) += (d3);                                                          \
        (b0) ^= (c0);                                                          \
        (b1) ^= (c1);                                                          \
        (b2) ^= (c2);                                                          \
        (b3) ^= (c3);                                                          \
        (b0) = rotl32((b0), 7);                                                \
        (b1) = rotl32((b1), 7);                                                \
        (b2) = rotl32((b2), 7);                                                \
        (b3) = rotl32((b3), 7);                                                \
    } while (0)

/* ChaCha20 constants — "expand 32-byte k" per RFC 8439 §2.3. */
static constexpr unsigned CHACHA20_CONST_0 = 0x61707865u; /* "expa" */
static constexpr unsigned CHACHA20_CONST_1 = 0x3320646eu; /* "nd 3" */
static constexpr unsigned CHACHA20_CONST_2 = 0x79622d32u; /* "2-by" */
static constexpr unsigned CHACHA20_CONST_3 = 0x6b206574u; /* "te k" */

/* Build the 16-word ChaCha20 state from key, counter, nonce.  Used by
 * the SIMD batched paths which take a pre-built state and produce
 * multiple blocks with sequential counters. */
static inline void chacha20StateInit(uint32_t state[16], const uint8_t key[32],
                                     uint32_t counter,
                                     const uint8_t nonce[12]) {
    state[0] = CHACHA20_CONST_0;
    state[1] = CHACHA20_CONST_1;
    state[2] = CHACHA20_CONST_2;
    state[3] = CHACHA20_CONST_3;
    state[4] = load32_le(key + 0);
    state[5] = load32_le(key + 4);
    state[6] = load32_le(key + 8);
    state[7] = load32_le(key + 12);
    state[8] = load32_le(key + 16);
    state[9] = load32_le(key + 20);
    state[10] = load32_le(key + 24);
    state[11] = load32_le(key + 28);
    state[12] = counter;
    state[13] = load32_le(nonce + 0);
    state[14] = load32_le(nonce + 4);
    state[15] = load32_le(nonce + 8);
}

/* Backend identification — public declarations live in
 * chacha20poly1305.h.  These dispatcher fields control which SIMD
 * Block-batch backend chacha20XorStream uses. */
static chacha20ForceBackend g_chacha20_force = CHACHA20_FORCE_AUTO;

void chacha20SetForceBackend(chacha20ForceBackend f) {
    g_chacha20_force = f;
}

chacha20ForceBackend chacha20GetForceBackend(void) {
    return g_chacha20_force;
}

/* Resolve which backend the dispatcher will use for the bulk-block
 * path.  The Block4/Block8 entry points themselves are still gated on
 * the AVAILABLE probes; this just controls the per-call selection
 * relative to the force-knob and the per-call bytes-remaining count. */
chacha20Backend chacha20ActiveBackend(void) {
    if (g_chacha20_force == CHACHA20_FORCE_SCALAR) {
        return CHACHA20_BACKEND_SCALAR;
    }
    if (g_chacha20_force == CHACHA20_FORCE_NEON && chacha20NeonAvailable()) {
        return CHACHA20_BACKEND_NEON;
    }
    if (g_chacha20_force == CHACHA20_FORCE_AVX2 && chacha20AVX2Available()) {
        return CHACHA20_BACKEND_AVX2;
    }
    if (g_chacha20_force == CHACHA20_FORCE_AUTO) {
        if (chacha20AVX2Available()) {
            return CHACHA20_BACKEND_AVX2;
        }
        if (chacha20NeonAvailable()) {
            return CHACHA20_BACKEND_NEON;
        }
    }
    return CHACHA20_BACKEND_SCALAR;
}

/* Poly1305 SIMD backend dispatch — independent of chacha20 (different
 * SIMD shape, different cost model).  Default AUTO picks the widest
 * available batch (AVX2 > NEON > scalar). */
static poly1305ForceBackend g_poly1305_force = POLY1305_FORCE_AUTO;

void poly1305SetForceBackend(poly1305ForceBackend f) {
    g_poly1305_force = f;
}

poly1305ForceBackend poly1305GetForceBackend(void) {
    return g_poly1305_force;
}

poly1305Backend poly1305ActiveBackend(void) {
    if (g_poly1305_force == POLY1305_FORCE_SCALAR) {
        return POLY1305_BACKEND_SCALAR;
    }
    if (g_poly1305_force == POLY1305_FORCE_NEON && poly1305NeonAvailable()) {
        return POLY1305_BACKEND_NEON;
    }
    if (g_poly1305_force == POLY1305_FORCE_AVX2 && poly1305AVX2Available()) {
        return POLY1305_BACKEND_AVX2;
    }
    if (g_poly1305_force == POLY1305_FORCE_AUTO) {
        if (poly1305AVX2Available()) {
            return POLY1305_BACKEND_AVX2;
        }
        if (poly1305NeonAvailable()) {
            return POLY1305_BACKEND_NEON;
        }
    }
    return POLY1305_BACKEND_SCALAR;
}

/* Produce one 64-byte ChaCha20 keystream block.  out must be 64 bytes. */
static void chacha20Block(const uint8_t key[32], uint32_t counter,
                          const uint8_t nonce[12], uint8_t out[64]) {
    uint32_t s0 = CHACHA20_CONST_0, s1 = CHACHA20_CONST_1,
             s2 = CHACHA20_CONST_2, s3 = CHACHA20_CONST_3;
    uint32_t s4 = load32_le(key + 0);
    uint32_t s5 = load32_le(key + 4);
    uint32_t s6 = load32_le(key + 8);
    uint32_t s7 = load32_le(key + 12);
    uint32_t s8 = load32_le(key + 16);
    uint32_t s9 = load32_le(key + 20);
    uint32_t s10 = load32_le(key + 24);
    uint32_t s11 = load32_le(key + 28);
    uint32_t s12 = counter;
    uint32_t s13 = load32_le(nonce + 0);
    uint32_t s14 = load32_le(nonce + 4);
    uint32_t s15 = load32_le(nonce + 8);

    uint32_t x0 = s0, x1 = s1, x2 = s2, x3 = s3, x4 = s4, x5 = s5, x6 = s6,
             x7 = s7, x8 = s8, x9 = s9, x10 = s10, x11 = s11, x12 = s12,
             x13 = s13, x14 = s14, x15 = s15;

    for (uint32_t i = 0; i < 10; i++) { /* 10 doublerounds = 20 rounds */
        /* Column rounds — four independent QRs interleaved. */
        QR4(x0, x4, x8, x12, x1, x5, x9, x13, x2, x6, x10, x14, x3, x7, x11,
            x15);
        /* Diagonal rounds — four independent QRs interleaved. */
        QR4(x0, x5, x10, x15, x1, x6, x11, x12, x2, x7, x8, x13, x3, x4, x9,
            x14);
    }

    store32_le(out + 0, x0 + s0);
    store32_le(out + 4, x1 + s1);
    store32_le(out + 8, x2 + s2);
    store32_le(out + 12, x3 + s3);
    store32_le(out + 16, x4 + s4);
    store32_le(out + 20, x5 + s5);
    store32_le(out + 24, x6 + s6);
    store32_le(out + 28, x7 + s7);
    store32_le(out + 32, x8 + s8);
    store32_le(out + 36, x9 + s9);
    store32_le(out + 40, x10 + s10);
    store32_le(out + 44, x11 + s11);
    store32_le(out + 48, x12 + s12);
    store32_le(out + 52, x13 + s13);
    store32_le(out + 56, x14 + s14);
    store32_le(out + 60, x15 + s15);
}

/* Stream-xor `len` bytes from `in` into `out` using ChaCha20 starting at
 * `initial_counter`.  in == out is allowed for in-place.
 *
 * Dispatch policy: when at least 512 bytes remain AND AVX2 is
 * available, generate 8 keystream blocks per call (chacha20AVX2Block8).
 * Else when at least 256 bytes remain AND NEON is available, generate
 * 4 blocks per call (chacha20NeonBlock4).  The tail (< 64 bytes per
 * iteration) falls through to the scalar chacha20Block path, which
 * also services hosts with neither SIMD backend available.  Bit-for-bit
 * output parity vs the pure-scalar path is verified by
 * dkTestChaChaSimdParity. */
static void chacha20XorStream(const uint8_t key[32], uint32_t initial_counter,
                              const uint8_t nonce[12], const uint8_t *in,
                              uint8_t *out, size_t len) {
    const chacha20Backend backend = chacha20ActiveBackend();
    uint32_t counter = initial_counter;

    /* AVX2 8-block path: 512 bytes per call.  Counter is incremented
     * by 8 per iteration; the SIMD routine reads state[12] as the
     * base counter and produces blocks for c+0..c+7. */
    if (CHACHA20_AVX2_BUILD && backend == CHACHA20_BACKEND_AVX2) {
        uint8_t ks[512];
        uint32_t state[16];
        while (len >= 512u) {
            chacha20StateInit(state, key, counter, nonce);
            chacha20AVX2Block8(state, ks);
            for (size_t i = 0; i < 512u; i++) {
                out[i] = (uint8_t)(in[i] ^ ks[i]);
            }
            in += 512u;
            out += 512u;
            len -= 512u;
            counter += 8u;
        }
        dkSecureZero(ks, sizeof(ks));
        /* state[] holds the key bytes (positions 4..11) plus nonce and
         * counter; scrub before frame teardown so a stack-leaking
         * primitive cannot read the key from the released frame. */
        dkSecureZero(state, sizeof(state));
    }

    /* NEON 4-block path: 256 bytes per call.  Used on aarch64 and also
     * as a step-down on x86 when fewer than 512 bytes remain (NEON
     * backend is unavailable on x86 so this branch is only reached on
     * AArch64 hosts or if AVX2 ran but left < 512 bytes — see below). */
    if (backend == CHACHA20_BACKEND_NEON || backend == CHACHA20_BACKEND_AVX2) {
        /* AVX2 backend can also use NEON here on platforms where both
         * report available (none today), but the more important branch
         * is: AArch64 host, post-AVX2 fallthrough is impossible because
         * AVX2 probe returns false on AArch64 — backend == NEON. */
        if (backend == CHACHA20_BACKEND_NEON && len >= 256u) {
            uint8_t ks[256];
            uint32_t state[16];
            while (len >= 256u) {
                chacha20StateInit(state, key, counter, nonce);
                chacha20NeonBlock4(state, ks);
                for (size_t i = 0; i < 256u; i++) {
                    out[i] = (uint8_t)(in[i] ^ ks[i]);
                }
                in += 256u;
                out += 256u;
                len -= 256u;
                counter += 4u;
            }
            dkSecureZero(ks, sizeof(ks));
            /* state[] holds the key bytes (positions 4..11) plus nonce
             * and counter; scrub before frame teardown so a stack-
             * leaking primitive cannot read the key from the released
             * frame. */
            dkSecureZero(state, sizeof(state));
        }
    }

    /* Tail / fallback: scalar one-block-at-a-time. */
    uint8_t block[64];
    while (len > 0) {
        chacha20Block(key, counter, nonce, block);
        size_t n = len < 64u ? len : 64u;
        for (size_t i = 0; i < n; i++) {
            out[i] = (uint8_t)(in[i] ^ block[i]);
        }
        in += n;
        out += n;
        len -= n;
        counter++;
    }
    /* Don't leave keystream sitting on the stack. */
    dkSecureZero(block, sizeof(block));
}

/* ===== Poly1305 =====
 *
 * 130-bit field arithmetic over GF(2^130 - 5) using 5x 26-bit limbs.
 * Multiplication produces 5x ~52-bit products which we carry-reduce
 * (mod 2^130 - 5: the top limb's overflow beyond 2^130 multiplies by 5
 * and folds into the low limb).  This is the canonical Bernstein
 * 32-bit-friendly representation and fits in uint64_t throughout. */

typedef struct poly1305State {
    /* 32-byte accumulator + key area; field order: largest first. */
    uint64_t h[5];   /* 130-bit accumulator (5 x 26-bit limbs) */
    uint32_t r[5];   /* clamped r as 5 x 26-bit limbs */
    uint32_t s[4];   /* "s" half of the 256-bit one-time key */
    uint8_t buf[16]; /* partial block */
    size_t buf_len;  /* bytes currently in buf (0..15) */
} poly1305State;

static void poly1305Init(poly1305State *st, const uint8_t key[32]) {
    /* Clamp r per RFC 8439 §2.5: clear the four reserved bits in each of
     * r[3], r[7], r[11], r[15] and the low two bits of r[4], r[8], r[12]. */
    uint32_t r0 = load32_le(key + 0) & 0x0fffffffu;
    uint32_t r1 = load32_le(key + 4) & 0x0ffffffcu;
    uint32_t r2 = load32_le(key + 8) & 0x0ffffffcu;
    uint32_t r3 = load32_le(key + 12) & 0x0ffffffcu;

    /* Repack 4 x 32-bit r into 5 x 26-bit r. */
    st->r[0] = r0 & 0x3ffffffu;
    st->r[1] = ((r0 >> 26) | (r1 << 6)) & 0x3ffffffu;
    st->r[2] = ((r1 >> 20) | (r2 << 12)) & 0x3ffffffu;
    st->r[3] = ((r2 >> 14) | (r3 << 18)) & 0x3ffffffu;
    st->r[4] = (r3 >> 8) & 0x3ffffffu;

    st->h[0] = 0;
    st->h[1] = 0;
    st->h[2] = 0;
    st->h[3] = 0;
    st->h[4] = 0;

    st->s[0] = load32_le(key + 16);
    st->s[1] = load32_le(key + 20);
    st->s[2] = load32_le(key + 24);
    st->s[3] = load32_le(key + 28);

    st->buf_len = 0;
}

/* Consume one full 16-byte block (or final short block with hibit=0).
 * `hibit_set` tells whether to add the 2^128 marker (always 1 except
 * for the very last short block). */
static void poly1305Block(poly1305State *st, const uint8_t block[16], size_t,
                          bool hibit_set) {
    /* Build 5 x 26-bit input limbs from a (possibly short) block.  For a
     * short block we copy into a zero-padded scratch and add the hibit
     * marker at position `block_len` (8 bits) — but per RFC the marker
     * is "append a 1 bit then pad to 17 bytes".  For the partial-block
     * path, the caller zero-pads buf and inserts the 0x01 byte
     * explicitly, then calls us with block_len=16, hibit_set=false.
     * Full blocks pass block_len=16, hibit_set=true => add 1<<24 to
     * limb 4. */

    uint32_t t0 = load32_le(block + 0);
    uint32_t t1 = load32_le(block + 4);
    uint32_t t2 = load32_le(block + 8);
    uint32_t t3 = load32_le(block + 12);

    /* h += m */
    uint64_t h0 = st->h[0] + (t0 & 0x3ffffffu);
    uint64_t h1 = st->h[1] + (((t0 >> 26) | (t1 << 6)) & 0x3ffffffu);
    uint64_t h2 = st->h[2] + (((t1 >> 20) | (t2 << 12)) & 0x3ffffffu);
    uint64_t h3 = st->h[3] + (((t2 >> 14) | (t3 << 18)) & 0x3ffffffu);
    uint64_t h4 = st->h[4] + (t3 >> 8) + (hibit_set ? (1u << 24) : 0u);

    uint64_t r0 = st->r[0];
    uint64_t r1 = st->r[1];
    uint64_t r2 = st->r[2];
    uint64_t r3 = st->r[3];
    uint64_t r4 = st->r[4];

    /* Precompute 5*r1..5*r4 (used by the wrap-around terms). */
    uint64_t s1 = r1 * 5;
    uint64_t s2 = r2 * 5;
    uint64_t s3 = r3 * 5;
    uint64_t s4 = r4 * 5;

    /* h *= r (mod 2^130 - 5).  Each d_i is a 64-bit product accumulator;
     * with limbs ~26 bits and r limbs ~26 bits, products are ~52 bits;
     * summing 5 of them stays under 2^55 — fits comfortably. */
    uint64_t d0 = h0 * r0 + h1 * s4 + h2 * s3 + h3 * s2 + h4 * s1;
    uint64_t d1 = h0 * r1 + h1 * r0 + h2 * s4 + h3 * s3 + h4 * s2;
    uint64_t d2 = h0 * r2 + h1 * r1 + h2 * r0 + h3 * s4 + h4 * s3;
    uint64_t d3 = h0 * r3 + h1 * r2 + h2 * r1 + h3 * r0 + h4 * s4;
    uint64_t d4 = h0 * r4 + h1 * r3 + h2 * r2 + h3 * r1 + h4 * r0;

    /* Carry-reduce. */
    uint64_t c;
    c = d0 >> 26;
    h0 = d0 & 0x3ffffffu;
    d1 += c;
    c = d1 >> 26;
    h1 = d1 & 0x3ffffffu;
    d2 += c;
    c = d2 >> 26;
    h2 = d2 & 0x3ffffffu;
    d3 += c;
    c = d3 >> 26;
    h3 = d3 & 0x3ffffffu;
    d4 += c;
    c = d4 >> 26;
    h4 = d4 & 0x3ffffffu;
    /* Top overflow folds back as *5 to limb 0 (mod 2^130 - 5). */
    h0 += c * 5;
    c = h0 >> 26;
    h0 &= 0x3ffffffu;
    h1 += c;

    st->h[0] = h0;
    st->h[1] = h1;
    st->h[2] = h2;
    st->h[3] = h3;
    st->h[4] = h4;
}

static void poly1305Update(poly1305State *st, const uint8_t *data, size_t len) {
    /* Drain buffered partial block. */
    if (st->buf_len > 0) {
        size_t want = 16 - st->buf_len;
        size_t take = len < want ? len : want;
        memcpy(st->buf + st->buf_len, data, take);
        st->buf_len += take;
        data += take;
        len -= take;
        if (st->buf_len == 16) {
            poly1305Block(st, st->buf, 16, true);
            st->buf_len = 0;
        }
    }

    /* SIMD bulk path: process N-block batches with precomputed r^N.
     * Cost model: AVX2 4-block requires 3 modular squares to precompute
     * r^2..r^4 (~tens of cycles each); only pays off when many batches
     * follow.  NEON 2-block requires 1 square.  Threshold of 4 batches
     * is a conservative break-even for the precompute amortisation.
     * Output is bit-identical to scalar — enforced by
     * dkTestPoly1305SimdParity. */
    const poly1305Backend pbk = poly1305ActiveBackend();
    if (POLY1305_AVX2_BUILD && pbk == POLY1305_BACKEND_AVX2 &&
        len >= 64u * 4u) {
        uint32_t r2[5], r3[5], r4[5];
        poly1305AVX2PrecomputePowers(st->r, r2, r3, r4);
        size_t groups = len / 64u;
        poly1305AVX2Block4x(st->h, st->r, r2, r3, r4, data, groups);
        const size_t bulk = groups * 64u;
        data += bulk;
        len -= bulk;
        dkSecureZero(r2, sizeof(r2));
        dkSecureZero(r3, sizeof(r3));
        dkSecureZero(r4, sizeof(r4));
    } else if (pbk == POLY1305_BACKEND_NEON && len >= 32u * 4u) {
        uint32_t r2[5];
        poly1305NeonPrecomputeR2(st->r, r2);
        size_t pairs = len / 32u;
        poly1305NeonBlock2x(st->h, st->r, r2, data, pairs);
        const size_t bulk = pairs * 32u;
        data += bulk;
        len -= bulk;
        dkSecureZero(r2, sizeof(r2));
    }

    /* Scalar bulk for the tail (and the entire payload when SIMD is
     * unavailable or below the threshold). */
    while (len >= 16) {
        poly1305Block(st, data, 16, true);
        data += 16;
        len -= 16;
    }
    /* Stash partial-block tail. */
    if (len > 0) {
        memcpy(st->buf, data, len);
        st->buf_len = len;
    }
}

static void poly1305Final(poly1305State *st, uint8_t tag[16]) {
    /* Final partial block: zero-pad, set the 0x01 marker byte at
     * position buf_len, then process with hibit_set=false. */
    if (st->buf_len > 0) {
        st->buf[st->buf_len] = 1;
        for (size_t i = st->buf_len + 1; i < 16; i++) {
            st->buf[i] = 0;
        }
        poly1305Block(st, st->buf, st->buf_len, false);
        st->buf_len = 0;
    }

    /* Full carry propagation. */
    uint64_t h0 = st->h[0];
    uint64_t h1 = st->h[1];
    uint64_t h2 = st->h[2];
    uint64_t h3 = st->h[3];
    uint64_t h4 = st->h[4];
    uint64_t c;

    c = h1 >> 26;
    h1 &= 0x3ffffffu;
    h2 += c;
    c = h2 >> 26;
    h2 &= 0x3ffffffu;
    h3 += c;
    c = h3 >> 26;
    h3 &= 0x3ffffffu;
    h4 += c;
    c = h4 >> 26;
    h4 &= 0x3ffffffu;
    h0 += c * 5;
    c = h0 >> 26;
    h0 &= 0x3ffffffu;
    h1 += c;

    /* Compute h + -p = h - (2^130 - 5) = h + 5 - 2^130, and select the
     * larger of (h, h + 5 mod 2^130) as the canonical result. */
    uint64_t g0 = h0 + 5;
    c = g0 >> 26;
    g0 &= 0x3ffffffu;
    uint64_t g1 = h1 + c;
    c = g1 >> 26;
    g1 &= 0x3ffffffu;
    uint64_t g2 = h2 + c;
    c = g2 >> 26;
    g2 &= 0x3ffffffu;
    uint64_t g3 = h3 + c;
    c = g3 >> 26;
    g3 &= 0x3ffffffu;
    uint64_t g4 = h4 + c - (1ULL << 26);

    /* If g4 underflowed (negative when interpreted signed), h was < p,
     * so keep h.  Else use g. */
    uint64_t mask = (g4 >> 63) - 1; /* all-ones if g4>=0, else 0 */
    g0 &= mask;
    g1 &= mask;
    g2 &= mask;
    g3 &= mask;
    g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;

    /* Pack into 4 x 32-bit. */
    uint32_t f0 = (uint32_t)((h0) | (h1 << 26));
    uint32_t f1 = (uint32_t)((h1 >> 6) | (h2 << 20));
    uint32_t f2 = (uint32_t)((h2 >> 12) | (h3 << 14));
    uint32_t f3 = (uint32_t)((h3 >> 18) | (h4 << 8));

    /* tag = (h + s) mod 2^128 — note: the per-limb s already lives in
     * little-endian uint32 in st->s. */
    uint64_t t = (uint64_t)f0 + st->s[0];
    f0 = (uint32_t)t;
    t = (uint64_t)f1 + st->s[1] + (t >> 32);
    f1 = (uint32_t)t;
    t = (uint64_t)f2 + st->s[2] + (t >> 32);
    f2 = (uint32_t)t;
    t = (uint64_t)f3 + st->s[3] + (t >> 32);
    f3 = (uint32_t)t;

    store32_le(tag + 0, f0);
    store32_le(tag + 4, f1);
    store32_le(tag + 8, f2);
    store32_le(tag + 12, f3);

    /* Wipe sensitive state. */
    dkSecureZero(st, sizeof(*st));
}

/* ===== AEAD glue (RFC 8439 §2.8) ===== */

/* Derive the Poly1305 one-time key by running ChaCha20 with counter=0
 * and taking the first 32 bytes of keystream.  RFC 8439 §2.6. */
static void deriveOnetimeKey(const uint8_t key[32], const uint8_t nonce[12],
                             uint8_t out_otk[32]) {
    uint8_t block[64];
    chacha20Block(key, 0, nonce, block);
    memcpy(out_otk, block, 32);
    dkSecureZero(block, sizeof(block));
}

/* MAC = Poly1305(AAD || pad16 || ciphertext || pad16 || aad_len_LE64 ||
 * ct_len_LE64) — RFC 8439 §2.8. */
static void computeAeadTag(const uint8_t otk[32], const uint8_t *aad,
                           size_t aad_len, const uint8_t *ct, size_t ct_len,
                           uint8_t tag[16]) {
    poly1305State st;
    poly1305Init(&st, otk);

    static constexpr uint8_t zeros[16] = {};

    if (aad_len > 0) {
        poly1305Update(&st, aad, aad_len);
        size_t pad = (16 - (aad_len % 16)) % 16;
        if (pad > 0) {
            poly1305Update(&st, zeros, pad);
        }
    }
    if (ct_len > 0) {
        poly1305Update(&st, ct, ct_len);
        size_t pad = (16 - (ct_len % 16)) % 16;
        if (pad > 0) {
            poly1305Update(&st, zeros, pad);
        }
    }

    uint8_t lens[16];
    store64_le(lens + 0, (uint64_t)aad_len);
    store64_le(lens + 8, (uint64_t)ct_len);
    poly1305Update(&st, lens, sizeof(lens));

    poly1305Final(&st, tag);
}

/* An AEAD call made wrongly (NULL buffers for a nonzero length, a stream
 * call out of order or on a finished context): refused by the caller of
 * this, and reported as misuse */
__attribute__((cold, noinline)) static void
aeadMisuse_(const char *function, int line, const char *what) {
    dkDiagReport_(DK_DIAG_MISUSE, DK_DIAG_AEAD_MISUSE, function, __FILE__, line,
                  "%s", what);
}
#define AEAD_MISUSE_(what) aeadMisuse_(__func__, __LINE__, (what))

/* Arguments a one-shot Seal or Open cannot use: a NULL key, nonce or tag,
 * or a NULL buffer for a nonzero length */
#define AEAD_ARGS_MISUSED_(key, nonce, tag, aad, aadLen, in, out, len)         \
    ((!(key) || !(nonce) || !(tag) || ((len) > 0 && (!(in) || !(out))) ||      \
      ((aadLen) > 0 && !(aad))) &&                                             \
     (AEAD_MISUSE_("a NULL key, nonce or tag, or a NULL buffer for a "         \
                   "nonzero length"),                                          \
      true))

bool chacha20poly1305Seal(const uint8_t key[CHACHA20_POLY1305_KEY_LEN],
                          const uint8_t nonce[CHACHA20_POLY1305_NONCE_LEN],
                          const uint8_t *aad, size_t aad_len,
                          const uint8_t *plaintext, size_t plaintext_len,
                          uint8_t *ciphertext,
                          uint8_t tag[CHACHA20_POLY1305_TAG_LEN]) {
    if (AEAD_ARGS_MISUSED_(key, nonce, tag, aad, aad_len, plaintext, ciphertext,
                           plaintext_len)) {
        return false;
    }
    if ((uint64_t)plaintext_len > CHACHA20_POLY1305_MAX_PAYLOAD) {
        return false;
    }

    uint8_t otk[32];
    deriveOnetimeKey(key, nonce, otk);

    /* Encrypt at counter=1. */
    if (plaintext_len > 0) {
        chacha20XorStream(key, 1, nonce, plaintext, ciphertext, plaintext_len);
    }

    /* Authenticate aad + ciphertext. */
    computeAeadTag(otk, aad, aad_len, ciphertext, plaintext_len, tag);

    dkSecureZero(otk, sizeof(otk));
    return true;
}

/* Constant-time 16-byte compare: returns 0 iff equal, nonzero otherwise.
 * Asm-barriered helper from ctmem.h prevents short-circuit codegen. */
static uint32_t constTimeNeq16(const uint8_t *a, const uint8_t *b) {
    return dkCtMemEq16(a, b) ? 0u : 1u;
}

bool chacha20poly1305Open(const uint8_t key[CHACHA20_POLY1305_KEY_LEN],
                          const uint8_t nonce[CHACHA20_POLY1305_NONCE_LEN],
                          const uint8_t *aad, size_t aad_len,
                          const uint8_t *ciphertext, size_t ciphertext_len,
                          const uint8_t tag[CHACHA20_POLY1305_TAG_LEN],
                          uint8_t *plaintext) {
    if ((uint64_t)ciphertext_len > CHACHA20_POLY1305_MAX_PAYLOAD) {
        /* Refuse symmetrically with Seal — an envelope claiming a
         * payload past the counter-wrap boundary cannot have been
         * produced by a compliant sealer, so treat it as malformed.
         * Do NOT wipe the plaintext buffer here: the caller's buffer
         * might be smaller than ciphertext_len (e.g. they passed a
         * length that obviously can't fit in their buffer to probe
         * the bound).  No keystream was derived, no sensitive bytes
         * were written. */
        return false;
    }
    /* NULL guard parity with aesGcmOpen: a non-zero length paired with a
     * NULL buffer is a caller bug; refuse cleanly (and report it) instead
     * of segfaulting through the MAC / XOR loops. */
    if (AEAD_ARGS_MISUSED_(key, nonce, tag, aad, aad_len, ciphertext, plaintext,
                           ciphertext_len)) {
        return false;
    }

    uint8_t otk[32];
    deriveOnetimeKey(key, nonce, otk);

    uint8_t computed_tag[CHACHA20_POLY1305_TAG_LEN];
    computeAeadTag(otk, aad, aad_len, ciphertext, ciphertext_len, computed_tag);

    uint32_t neq = constTimeNeq16(computed_tag, tag);
    dkSecureZero(otk, sizeof(otk));
    dkSecureZero(computed_tag, sizeof(computed_tag));

    if (neq != 0) {
        /* Authentication failed: zero plaintext output as defense in
         * depth against callers that ignore the return. */
        if (ciphertext_len > 0 && plaintext != NULL) {
            dkSecureZero(plaintext, ciphertext_len);
        }
        return false;
    }

    if (ciphertext_len > 0) {
        chacha20XorStream(key, 1, nonce, ciphertext, plaintext, ciphertext_len);
    }
    return true;
}

/* ===== Streaming AEAD ================================================
 *
 * The streaming Seal/Open ctx exposes its limb state as the prefix of
 * a chacha20poly1305StreamCtx; locked by static_assert below so the
 * reinterpret-cast to (poly1305State *) is always valid. */

static_assert(offsetof(chacha20poly1305StreamCtx, poly_h) ==
                  offsetof(poly1305State, h),
              "chacha20poly1305StreamCtx::poly_h must alias poly1305State::h");
static_assert(offsetof(chacha20poly1305StreamCtx, poly_r) ==
                  offsetof(poly1305State, r),
              "chacha20poly1305StreamCtx::poly_r must alias poly1305State::r");
static_assert(offsetof(chacha20poly1305StreamCtx, poly_s) ==
                  offsetof(poly1305State, s),
              "chacha20poly1305StreamCtx::poly_s must alias poly1305State::s");
static_assert(
    offsetof(chacha20poly1305StreamCtx, poly_buf) ==
        offsetof(poly1305State, buf),
    "chacha20poly1305StreamCtx::poly_buf must alias poly1305State::buf");
static_assert(offsetof(chacha20poly1305StreamCtx, poly_buf_len) ==
                  offsetof(poly1305State, buf_len),
              "chacha20poly1305StreamCtx::poly_buf_len must alias buf_len");

static inline poly1305State *streamPoly(chacha20poly1305StreamCtx *ctx) {
    return (poly1305State *)ctx;
}

/* Initialise common state for both Seal and Open streams. */
static void streamInitCommon(chacha20poly1305StreamCtx *ctx,
                             const uint8_t key[32], const uint8_t nonce[12]) {
    memset(ctx, 0, sizeof(*ctx));
    memcpy(ctx->key, key, 32);
    memcpy(ctx->nonce, nonce, 12);
    /* Derive one-time Poly1305 key from ChaCha20 block 0; initialise
     * the running Poly1305 state with it. */
    uint8_t otk[32];
    deriveOnetimeKey(key, nonce, otk);
    poly1305Init(streamPoly(ctx), otk);
    dkSecureZero(otk, sizeof(otk));
    ctx->chacha_counter = 1;  /* block 0 was the OTK; payload starts at 1 */
    ctx->keystream_used = 64; /* force a fresh block on first Update */
    ctx->phase = CHACHA20_POLY1305_STREAM_PHASE_AAD;
}

void chacha20poly1305SealStreamInit(chacha20poly1305StreamCtx *ctx,
                                    const uint8_t key[32],
                                    const uint8_t nonce[12]) {
    if (ctx == NULL || key == NULL || nonce == NULL) {
        /* No return value says so: the diagnostic is the only signal */
        AEAD_MISUSE_("stream init with no context, key or nonce");
        if (ctx != NULL) {
            /* Wipe the ctx (any prior key/nonce from a previous Init)
             * before leaving in the rejection state; mirrors
             * streamFinalise's contract that every failure path
             * produces a bytewise-zero ctx. */
            dkSecureZero(ctx, sizeof(*ctx));
        }
        return;
    }
    streamInitCommon(ctx, key, nonce);
}

void chacha20poly1305OpenStreamInit(chacha20poly1305StreamCtx *ctx,
                                    const uint8_t key[32],
                                    const uint8_t nonce[12]) {
    if (ctx == NULL || key == NULL || nonce == NULL) {
        /* No return value says so: the diagnostic is the only signal */
        AEAD_MISUSE_("stream init with no context, key or nonce");
        if (ctx != NULL) {
            /* Wipe the ctx (any prior key/nonce from a previous Init)
             * before leaving in the rejection state; mirrors
             * streamFinalise's contract that every failure path
             * produces a bytewise-zero ctx. */
            dkSecureZero(ctx, sizeof(*ctx));
        }
        return;
    }
    streamInitCommon(ctx, key, nonce);
}

/* Common AAD feed: legal during AAD phase only.  Returns false (and
 * wipes ctx) if called after Update or Final, or with NULL aad and
 * non-zero len.  Wipe matches streamFinalise's failure-path contract
 * so key/nonce do not linger on the caller's stack frame. */
static bool streamAadCommon(chacha20poly1305StreamCtx *ctx, const uint8_t *aad,
                            size_t len) {
    if (ctx == NULL || ctx->phase != CHACHA20_POLY1305_STREAM_PHASE_AAD) {
        AEAD_MISUSE_("stream AAD with no context, after the body began, or "
                     "on a finished context");
        if (ctx != NULL) {
            dkSecureZero(ctx, sizeof(*ctx));
        }
        return false;
    }
    if (len == 0) {
        return true;
    }
    if (aad == NULL) {
        AEAD_MISUSE_("stream AAD: a NULL buffer for a nonzero length");
        dkSecureZero(ctx, sizeof(*ctx));
        return false;
    }
    poly1305Update(streamPoly(ctx), aad, len);
    ctx->aad_total += (uint64_t)len;
    return true;
}

bool chacha20poly1305SealStreamAad(chacha20poly1305StreamCtx *ctx,
                                   const uint8_t *aad, size_t len) {
    return streamAadCommon(ctx, aad, len);
}

bool chacha20poly1305OpenStreamAad(chacha20poly1305StreamCtx *ctx,
                                   const uint8_t *aad, size_t len) {
    return streamAadCommon(ctx, aad, len);
}

/* Close the AAD phase: pad the running Poly1305 to a 16-byte boundary
 * and transition into BODY.  Idempotent within BODY (no-op). */
static void streamCloseAad(chacha20poly1305StreamCtx *ctx) {
    if (ctx->phase != CHACHA20_POLY1305_STREAM_PHASE_AAD) {
        return;
    }
    const size_t pad = (16u - (size_t)(ctx->aad_total % 16u)) % 16u;
    if (pad > 0) {
        static constexpr uint8_t zeros[16] = {};
        poly1305Update(streamPoly(ctx), zeros, pad);
    }
    ctx->phase = CHACHA20_POLY1305_STREAM_PHASE_BODY;
}

/* Refresh the keystream block when the prior one is exhausted.
 * Returns false (and wipes ctx) if the counter would wrap.  The wipe
 * mirrors streamFinalise's failure-path contract: every poison
 * transition leaves the caller's storage bytewise zero rather than
 * retaining key/nonce/keystream_buf.  Centralising the wipe here
 * covers BOTH Seal-side and Open-side counter-wrap callers; their
 * `return false` after a failed refill thus needs no additional wipe. */
static bool streamRefillKeystream(chacha20poly1305StreamCtx *ctx) {
    if (ctx->chacha_counter == 0u) {
        /* Already wrapped — would silently reuse OTK keystream. */
        dkSecureZero(ctx, sizeof(*ctx));
        return false;
    }
    chacha20Block(ctx->key, ctx->chacha_counter, ctx->nonce,
                  ctx->keystream_buf);
    ctx->chacha_counter++;
    ctx->keystream_used = 0;
    return true;
}

/* Streaming Update for Seal: encrypts plaintext into ciphertext_out
 * and feeds the resulting ciphertext into the running Poly1305.
 *
 * Symmetric with OpenStreamUpdate: every poison transition wipes the
 * entire ctx (key, nonce, keystream_buf, etc.) so the caller's stack
 * frame does not retain secrets.  Post-wipe phase is UNINIT (0), which
 * the wrong-phase guard rejects, so any follow-on Update/Final
 * remains fail-closed. */
bool chacha20poly1305SealStreamUpdate(chacha20poly1305StreamCtx *ctx,
                                      const uint8_t *plaintext, size_t len,
                                      uint8_t *ciphertext_out) {
    if (ctx == NULL || (ctx->phase != CHACHA20_POLY1305_STREAM_PHASE_AAD &&
                        ctx->phase != CHACHA20_POLY1305_STREAM_PHASE_BODY)) {
        AEAD_MISUSE_("stream update with no context or on a finished "
                     "context");
        if (ctx != NULL) {
            dkSecureZero(ctx, sizeof(*ctx));
        }
        return false;
    }
    if (len == 0) {
        streamCloseAad(ctx);
        return true;
    }
    if (plaintext == NULL || ciphertext_out == NULL) {
        AEAD_MISUSE_("stream update: a NULL buffer for a nonzero length");
        dkSecureZero(ctx, sizeof(*ctx));
        return false;
    }
    if (ctx->ct_total > CHACHA20_POLY1305_MAX_PAYLOAD - (uint64_t)len) {
        dkSecureZero(ctx, sizeof(*ctx));
        return false;
    }
    streamCloseAad(ctx);

    const uint8_t *in = plaintext;
    uint8_t *out = ciphertext_out;

    /* SIMD bulk fast-path: when the per-block keystream is exhausted
     * AND we have >= 256 bytes to process, dispatch to chacha20XorStream
     * which uses NEON/AVX2 batched block generation.  Advances counter
     * by floor(bulk/64); the tail (< 256 bytes) falls through to the
     * per-block loop.  Preserves all invariants: keystream_buf stays
     * untouched here (the bulk path generates its own SIMD keystream
     * on the fly), so we must not have buffered keystream bytes to
     * consume first. */
    if (ctx->keystream_used >= 64u && len >= 256u &&
        ctx->chacha_counter != 0u) {
        size_t bulk_blocks = len / 64u;
        /* Don't cross the counter-wrap boundary in one SIMD call.
         * Remaining blocks before wrap: UINT32_MAX - counter + 1 (here
         * `counter != 0` guarantees the +1 cannot overflow). */
        const uint64_t blocks_before_wrap =
            (uint64_t)UINT32_MAX - (uint64_t)ctx->chacha_counter + 1u;
        if ((uint64_t)bulk_blocks > blocks_before_wrap) {
            bulk_blocks = (size_t)blocks_before_wrap;
        }
        if (bulk_blocks >= 4u) {
            const size_t bulk = bulk_blocks * 64u;
            chacha20XorStream(ctx->key, ctx->chacha_counter, ctx->nonce, in,
                              out, bulk);
            poly1305Update(streamPoly(ctx), out, bulk);
            ctx->chacha_counter += (uint32_t)bulk_blocks;
            ctx->ct_total += (uint64_t)bulk;
            in += bulk;
            out += bulk;
            len -= bulk;
        }
    }

    while (len > 0) {
        if (ctx->keystream_used >= 64u) {
            if (!streamRefillKeystream(ctx)) {
                return false;
            }
        }
        const size_t avail = 64u - ctx->keystream_used;
        const size_t n = (len < avail) ? len : avail;
        for (size_t i = 0; i < n; i++) {
            out[i] =
                (uint8_t)(in[i] ^ ctx->keystream_buf[ctx->keystream_used + i]);
        }
        poly1305Update(streamPoly(ctx), out, n);
        ctx->keystream_used += (uint32_t)n;
        ctx->ct_total += (uint64_t)n;
        in += n;
        out += n;
        len -= n;
    }
    return true;
}

bool chacha20poly1305OpenStreamUpdate(chacha20poly1305StreamCtx *ctx,
                                      const uint8_t *ciphertext, size_t len,
                                      uint8_t *plaintext_out) {
    if (ctx == NULL || (ctx->phase != CHACHA20_POLY1305_STREAM_PHASE_AAD &&
                        ctx->phase != CHACHA20_POLY1305_STREAM_PHASE_BODY)) {
        AEAD_MISUSE_("stream update with no context or on a finished "
                     "context");
        if (ctx != NULL) {
            /* Wipe the ctx (key, nonce, keystream_buf, etc.) before
             * marking poisoned: streamFinalise's failure path does the
             * same, so any poison transition leaves the caller's
             * storage bytewise zero rather than retaining secrets. */
            dkSecureZero(ctx, sizeof(*ctx));
        }
        return false;
    }
    if (len == 0) {
        streamCloseAad(ctx);
        return true;
    }
    if (ciphertext == NULL || plaintext_out == NULL) {
        AEAD_MISUSE_("stream update: a NULL buffer for a nonzero length");
        dkSecureZero(ctx, sizeof(*ctx));
        return false;
    }
    if (ctx->ct_total > CHACHA20_POLY1305_MAX_PAYLOAD - (uint64_t)len) {
        dkSecureZero(ctx, sizeof(*ctx));
        return false;
    }
    streamCloseAad(ctx);

    const uint8_t *in = ciphertext;
    uint8_t *out = plaintext_out;

    /* SIMD bulk fast-path: same shape as SealStreamUpdate.  Note the
     * Poly1305 input order is REVERSED — Open feeds ciphertext to the
     * MAC, then decrypts.  When in == out (in-place Open), the order
     * matters: Poly1305 must see the ciphertext before XOR overwrites
     * it.  chacha20XorStream supports in == out by reading then
     * writing each byte in order; we Poly1305-feed `in` BEFORE calling
     * it so the bytes are still ciphertext at MAC time. */
    if (ctx->keystream_used >= 64u && len >= 256u &&
        ctx->chacha_counter != 0u) {
        size_t bulk_blocks = len / 64u;
        const uint64_t blocks_before_wrap =
            (uint64_t)UINT32_MAX - (uint64_t)ctx->chacha_counter + 1u;
        if ((uint64_t)bulk_blocks > blocks_before_wrap) {
            bulk_blocks = (size_t)blocks_before_wrap;
        }
        if (bulk_blocks >= 4u) {
            const size_t bulk = bulk_blocks * 64u;
            /* MAC the ciphertext bytes first (Open semantic). */
            poly1305Update(streamPoly(ctx), in, bulk);
            chacha20XorStream(ctx->key, ctx->chacha_counter, ctx->nonce, in,
                              out, bulk);
            ctx->chacha_counter += (uint32_t)bulk_blocks;
            ctx->ct_total += (uint64_t)bulk;
            in += bulk;
            out += bulk;
            len -= bulk;
        }
    }

    while (len > 0) {
        if (ctx->keystream_used >= 64u) {
            if (!streamRefillKeystream(ctx)) {
                return false;
            }
        }
        const size_t avail = 64u - ctx->keystream_used;
        const size_t n = (len < avail) ? len : avail;
        /* Tag input is ciphertext (the same bytes we received). */
        poly1305Update(streamPoly(ctx), in, n);
        for (size_t i = 0; i < n; i++) {
            out[i] =
                (uint8_t)(in[i] ^ ctx->keystream_buf[ctx->keystream_used + i]);
        }
        ctx->keystream_used += (uint32_t)n;
        ctx->ct_total += (uint64_t)n;
        in += n;
        out += n;
        len -= n;
    }
    return true;
}

/* Finalise: pad ciphertext to 16, append AAD len + CT len, emit tag.
 * Returns false on out-of-order use.
 *
 * After the tag is computed (success) OR on out-of-order rejection
 * (failure), the entire ctx is wiped with `dkSecureZero` so that
 * `key`, `nonce`, and the residual `keystream_buf` cannot leak via
 * the caller's stack frame.  `poly1305Final` only wipes the aliased
 * poly1305State prefix; this wipe covers the chacha-specific tail.
 *
 * Post-wipe phase is left at UNINIT (the all-zero byte): UNINIT does
 * not match AAD or BODY, so every subsequent Aad/Update/Final call
 * correctly returns false and re-poisons (fail-closed). The terminal-
 * state contract is "ctx is bytewise zero after Final"; callers must
 * treat the ctx as discarded. */
static bool streamFinalise(chacha20poly1305StreamCtx *ctx, uint8_t tag[16]) {
    streamCloseAad(ctx);
    if (ctx->phase != CHACHA20_POLY1305_STREAM_PHASE_BODY) {
        /* Out-of-order use: caller must not see partial state.  Wipe
         * everything (including key/nonce) before returning. */
        AEAD_MISUSE_("stream final on a finished context");
        dkSecureZero(ctx, sizeof(*ctx));
        return false;
    }
    const size_t pad = (16u - (size_t)(ctx->ct_total % 16u)) % 16u;
    if (pad > 0) {
        static constexpr uint8_t zeros[16] = {};
        poly1305Update(streamPoly(ctx), zeros, pad);
    }
    uint8_t lens[16];
    store64_le(lens + 0, ctx->aad_total);
    store64_le(lens + 8, ctx->ct_total);
    poly1305Update(streamPoly(ctx), lens, sizeof(lens));
    poly1305Final(streamPoly(ctx), tag);
    /* Tag is now in caller's `tag` buffer; the ctx tail (key, nonce,
     * keystream_buf, counters, totals) is no longer needed.  Wipe it
     * all. */
    dkSecureZero(ctx, sizeof(*ctx));
    return true;
}

bool chacha20poly1305SealStreamFinal(chacha20poly1305StreamCtx *ctx,
                                     uint8_t tag[16]) {
    if (ctx == NULL || tag == NULL) {
        AEAD_MISUSE_("stream final with no context or no tag buffer");
        if (ctx != NULL) {
            /* Wipe everything; per the streamFinalise contract the
             * post-failure ctx must be bytewise zero so key/nonce
             * material does not linger on the stack frame. */
            dkSecureZero(ctx, sizeof(*ctx));
        }
        return false;
    }
    return streamFinalise(ctx, tag);
}

bool chacha20poly1305OpenStreamFinal(chacha20poly1305StreamCtx *ctx,
                                     const uint8_t expected_tag[16]) {
    if (ctx == NULL || expected_tag == NULL) {
        AEAD_MISUSE_("stream final with no context or no tag");
        if (ctx != NULL) {
            /* Wipe everything; per the streamFinalise contract the
             * post-failure ctx must be bytewise zero so key/nonce
             * material does not linger on the stack frame. */
            dkSecureZero(ctx, sizeof(*ctx));
        }
        return false;
    }
    uint8_t computed[16];
    if (!streamFinalise(ctx, computed)) {
        /* streamFinalise already wiped ctx on its failure path. */
        dkSecureZero(computed, sizeof(computed));
        return false;
    }
    const bool ok = dkCtMemEq16(computed, expected_tag);
    dkSecureZero(computed, sizeof(computed));
    /* streamFinalise already wiped ctx on its success path; whether
     * the tag matches or not, the ctx is bytewise zero and the caller
     * must treat it as discarded. */
    return ok;
}

/* ===== Test driver =================================================== */

#ifdef DATAKIT_TEST

#include "cleaner.h"
#include "ctest.h"
#include "datakit.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Scope-exit cleaners for test scratch buffers (see cleaner.h idioms). */
defcleanerName(uint8_t *, zfree, Bytes);
defcleanerName(uint32_t *, zfree, Words);

static int bytesEqual(const uint8_t *a, const uint8_t *b, size_t n) {
    return memcmp(a, b, n) == 0;
}

static void printHexLine(const char *label, const uint8_t *p, size_t n) {
    printf("%s", label);
    for (size_t i = 0; i < n; i++) {
        printf("%02x", p[i]);
    }
    printf("\n");
}

static int reportMismatch(const char *name, const uint8_t *got,
                          const uint8_t *expected, size_t n) {
    int err = 0;
    ERR("'%s' mismatch (%zu bytes)", name, n);
    printHexLine("  got: ", got, n);
    printHexLine("  exp: ", expected, n);
    fflush(stdout);
    return err;
}

/* ---- RFC 8439 §2.4.2 ChaCha20 encryption test vector ---------------- */

static constexpr uint8_t rfc_2_4_2_key[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
    0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
    0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
static constexpr uint8_t rfc_2_4_2_nonce[12] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4a, 0x00, 0x00, 0x00, 0x00};
/* String literals below intentionally elide the trailing NUL — the RFC
 * vectors store the exact byte count and tests compare against fixed-
 * length ciphertext arrays.  Suppress -Wunterminated-string-init
 * (clang 16+ AND GCC 15+) only for these declarations; reinstate at
 * end of block. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wunterminated-string-initialization"
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wunterminated-string-initialization"
#endif
static constexpr uint8_t rfc_2_4_2_plaintext[114] =
    "Ladies and Gentlemen of the class of '99: If I could offer you only "
    "one tip for the future, sunscreen would be it.";
static constexpr uint8_t rfc_2_4_2_ciphertext[114] = {
    0x6e, 0x2e, 0x35, 0x9a, 0x25, 0x68, 0xf9, 0x80, 0x41, 0xba, 0x07, 0x28,
    0xdd, 0x0d, 0x69, 0x81, 0xe9, 0x7e, 0x7a, 0xec, 0x1d, 0x43, 0x60, 0xc2,
    0x0a, 0x27, 0xaf, 0xcc, 0xfd, 0x9f, 0xae, 0x0b, 0xf9, 0x1b, 0x65, 0xc5,
    0x52, 0x47, 0x33, 0xab, 0x8f, 0x59, 0x3d, 0xab, 0xcd, 0x62, 0xb3, 0x57,
    0x16, 0x39, 0xd6, 0x24, 0xe6, 0x51, 0x52, 0xab, 0x8f, 0x53, 0x0c, 0x35,
    0x9f, 0x08, 0x61, 0xd8, 0x07, 0xca, 0x0d, 0xbf, 0x50, 0x0d, 0x6a, 0x61,
    0x56, 0xa3, 0x8e, 0x08, 0x8a, 0x22, 0xb6, 0x5e, 0x52, 0xbc, 0x51, 0x4d,
    0x16, 0xcc, 0xf8, 0x06, 0x81, 0x8c, 0xe9, 0x1a, 0xb7, 0x79, 0x37, 0x36,
    0x5a, 0xf9, 0x0b, 0xbf, 0x74, 0xa3, 0x5b, 0xe6, 0xb4, 0x0b, 0x8e, 0xed,
    0xf2, 0x78, 0x5e, 0x42, 0x87, 0x4d};

/* §2.4.2 starts the ChaCha20 stream at counter=1. */

/* ---- RFC 8439 §2.5.2 Poly1305 MAC test vector ----------------------- */

static constexpr uint8_t rfc_2_5_2_key[32] = {
    0x85, 0xd6, 0xbe, 0x78, 0x57, 0x55, 0x6d, 0x33, 0x7f, 0x44, 0x52,
    0xfe, 0x42, 0xd5, 0x06, 0xa8, 0x01, 0x03, 0x80, 0x8a, 0xfb, 0x0d,
    0xb2, 0xfd, 0x4a, 0xbf, 0xf6, 0xaf, 0x41, 0x49, 0xf5, 0x1b};
static constexpr uint8_t rfc_2_5_2_msg[34] =
    "Cryptographic Forum Research Group";
static constexpr uint8_t rfc_2_5_2_tag[16] = {
    0xa8, 0x06, 0x1d, 0xc1, 0x30, 0x51, 0x36, 0xc6,
    0xc2, 0x2b, 0x8b, 0xaf, 0x0c, 0x01, 0x27, 0xa9};

/* ---- RFC 8439 §2.6.2 Poly1305 key generation vector ----------------- */

static constexpr uint8_t rfc_2_6_2_key[32] = {
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a,
    0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95,
    0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f};
static constexpr uint8_t rfc_2_6_2_nonce[12] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
static constexpr uint8_t rfc_2_6_2_expected_otk[32] = {
    0x8a, 0xd5, 0xa0, 0x8b, 0x90, 0x5f, 0x81, 0xcc, 0x81, 0x50, 0x40,
    0x27, 0x4a, 0xb2, 0x94, 0x71, 0xa8, 0x33, 0xb6, 0x37, 0xe3, 0xfd,
    0x0d, 0xa5, 0x08, 0xdb, 0xb8, 0xe2, 0xfd, 0xd1, 0xa6, 0x46};

/* ---- RFC 8439 §2.8.2 AEAD test vector ------------------------------- */

static constexpr uint8_t rfc_2_8_2_key[32] = {
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a,
    0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95,
    0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f};
static constexpr uint8_t rfc_2_8_2_nonce[12] = {
    0x07, 0x00, 0x00, 0x00, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47};
static constexpr uint8_t rfc_2_8_2_aad[12] = {
    0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7};
static constexpr uint8_t rfc_2_8_2_plaintext[114] =
    "Ladies and Gentlemen of the class of '99: If I could offer you only "
    "one tip for the future, sunscreen would be it.";
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
static constexpr uint8_t rfc_2_8_2_ciphertext[114] = {
    0xd3, 0x1a, 0x8d, 0x34, 0x64, 0x8e, 0x60, 0xdb, 0x7b, 0x86, 0xaf, 0xbc,
    0x53, 0xef, 0x7e, 0xc2, 0xa4, 0xad, 0xed, 0x51, 0x29, 0x6e, 0x08, 0xfe,
    0xa9, 0xe2, 0xb5, 0xa7, 0x36, 0xee, 0x62, 0xd6, 0x3d, 0xbe, 0xa4, 0x5e,
    0x8c, 0xa9, 0x67, 0x12, 0x82, 0xfa, 0xfb, 0x69, 0xda, 0x92, 0x72, 0x8b,
    0x1a, 0x71, 0xde, 0x0a, 0x9e, 0x06, 0x0b, 0x29, 0x05, 0xd6, 0xa5, 0xb6,
    0x7e, 0xcd, 0x3b, 0x36, 0x92, 0xdd, 0xbd, 0x7f, 0x2d, 0x77, 0x8b, 0x8c,
    0x98, 0x03, 0xae, 0xe3, 0x28, 0x09, 0x1b, 0x58, 0xfa, 0xb3, 0x24, 0xe4,
    0xfa, 0xd6, 0x75, 0x94, 0x55, 0x85, 0x80, 0x8b, 0x48, 0x31, 0xd7, 0xbc,
    0x3f, 0xf4, 0xde, 0xf0, 0x8e, 0x4b, 0x7a, 0x9d, 0xe5, 0x76, 0xd2, 0x65,
    0x86, 0xce, 0xc6, 0x4b, 0x61, 0x16};
static constexpr uint8_t rfc_2_8_2_tag[16] = {
    0x1a, 0xe1, 0x0b, 0x59, 0x4f, 0x09, 0xe2, 0x6a,
    0x7e, 0x90, 0x2e, 0xcb, 0xd0, 0x60, 0x06, 0x91};

/* ---- RFC 8439 Appendix A.5 real-world AEAD example ------------------ */

static constexpr uint8_t rfc_a5_key[32] = {
    0x1c, 0x92, 0x40, 0xa5, 0xeb, 0x55, 0xd3, 0x8a, 0xf3, 0x33, 0x88,
    0x86, 0x04, 0xf6, 0xb5, 0xf0, 0x47, 0x39, 0x17, 0xc1, 0x40, 0x2b,
    0x80, 0x09, 0x9d, 0xca, 0x5c, 0xbc, 0x20, 0x70, 0x75, 0xc0};
static constexpr uint8_t rfc_a5_nonce[12] = {
    0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
static constexpr uint8_t rfc_a5_aad[12] = {0xf3, 0x33, 0x88, 0x86, 0x00, 0x00,
                                           0x00, 0x00, 0x00, 0x00, 0x4e, 0x91};
static constexpr uint8_t rfc_a5_ciphertext[265] = {
    0x64, 0xa0, 0x86, 0x15, 0x75, 0x86, 0x1a, 0xf4, 0x60, 0xf0, 0x62, 0xc7,
    0x9b, 0xe6, 0x43, 0xbd, 0x5e, 0x80, 0x5c, 0xfd, 0x34, 0x5c, 0xf3, 0x89,
    0xf1, 0x08, 0x67, 0x0a, 0xc7, 0x6c, 0x8c, 0xb2, 0x4c, 0x6c, 0xfc, 0x18,
    0x75, 0x5d, 0x43, 0xee, 0xa0, 0x9e, 0xe9, 0x4e, 0x38, 0x2d, 0x26, 0xb0,
    0xbd, 0xb7, 0xb7, 0x3c, 0x32, 0x1b, 0x01, 0x00, 0xd4, 0xf0, 0x3b, 0x7f,
    0x35, 0x58, 0x94, 0xcf, 0x33, 0x2f, 0x83, 0x0e, 0x71, 0x0b, 0x97, 0xce,
    0x98, 0xc8, 0xa8, 0x4a, 0xbd, 0x0b, 0x94, 0x81, 0x14, 0xad, 0x17, 0x6e,
    0x00, 0x8d, 0x33, 0xbd, 0x60, 0xf9, 0x82, 0xb1, 0xff, 0x37, 0xc8, 0x55,
    0x97, 0x97, 0xa0, 0x6e, 0xf4, 0xf0, 0xef, 0x61, 0xc1, 0x86, 0x32, 0x4e,
    0x2b, 0x35, 0x06, 0x38, 0x36, 0x06, 0x90, 0x7b, 0x6a, 0x7c, 0x02, 0xb0,
    0xf9, 0xf6, 0x15, 0x7b, 0x53, 0xc8, 0x67, 0xe4, 0xb9, 0x16, 0x6c, 0x76,
    0x7b, 0x80, 0x4d, 0x46, 0xa5, 0x9b, 0x52, 0x16, 0xcd, 0xe7, 0xa4, 0xe9,
    0x90, 0x40, 0xc5, 0xa4, 0x04, 0x33, 0x22, 0x5e, 0xe2, 0x82, 0xa1, 0xb0,
    0xa0, 0x6c, 0x52, 0x3e, 0xaf, 0x45, 0x34, 0xd7, 0xf8, 0x3f, 0xa1, 0x15,
    0x5b, 0x00, 0x47, 0x71, 0x8c, 0xbc, 0x54, 0x6a, 0x0d, 0x07, 0x2b, 0x04,
    0xb3, 0x56, 0x4e, 0xea, 0x1b, 0x42, 0x22, 0x73, 0xf5, 0x48, 0x27, 0x1a,
    0x0b, 0xb2, 0x31, 0x60, 0x53, 0xfa, 0x76, 0x99, 0x19, 0x55, 0xeb, 0xd6,
    0x31, 0x59, 0x43, 0x4e, 0xce, 0xbb, 0x4e, 0x46, 0x6d, 0xae, 0x5a, 0x10,
    0x73, 0xa6, 0x72, 0x76, 0x27, 0x09, 0x7a, 0x10, 0x49, 0xe6, 0x17, 0xd9,
    0x1d, 0x36, 0x10, 0x94, 0xfa, 0x68, 0xf0, 0xff, 0x77, 0x98, 0x71, 0x30,
    0x30, 0x5b, 0xea, 0xba, 0x2e, 0xda, 0x04, 0xdf, 0x99, 0x7b, 0x71, 0x4d,
    0x6c, 0x6f, 0x2c, 0x29, 0xa6, 0xad, 0x5c, 0xb4, 0x02, 0x2b, 0x02, 0x70,
    0x9b};
static constexpr uint8_t rfc_a5_tag[16] = {0xee, 0xad, 0x9d, 0x67, 0x89, 0x0c,
                                           0xbb, 0x22, 0x39, 0x23, 0x36, 0xfe,
                                           0xa1, 0x85, 0x1f, 0x38};
/* The plaintext from A.5 is 265 bytes of a fake email body — we encode
 * by literal bytes rather than relying on string escaping for safety. */
static constexpr uint8_t rfc_a5_plaintext[265] = {
    0x49, 0x6e, 0x74, 0x65, 0x72, 0x6e, 0x65, 0x74, 0x2d, 0x44, 0x72, 0x61,
    0x66, 0x74, 0x73, 0x20, 0x61, 0x72, 0x65, 0x20, 0x64, 0x72, 0x61, 0x66,
    0x74, 0x20, 0x64, 0x6f, 0x63, 0x75, 0x6d, 0x65, 0x6e, 0x74, 0x73, 0x20,
    0x76, 0x61, 0x6c, 0x69, 0x64, 0x20, 0x66, 0x6f, 0x72, 0x20, 0x61, 0x20,
    0x6d, 0x61, 0x78, 0x69, 0x6d, 0x75, 0x6d, 0x20, 0x6f, 0x66, 0x20, 0x73,
    0x69, 0x78, 0x20, 0x6d, 0x6f, 0x6e, 0x74, 0x68, 0x73, 0x20, 0x61, 0x6e,
    0x64, 0x20, 0x6d, 0x61, 0x79, 0x20, 0x62, 0x65, 0x20, 0x75, 0x70, 0x64,
    0x61, 0x74, 0x65, 0x64, 0x2c, 0x20, 0x72, 0x65, 0x70, 0x6c, 0x61, 0x63,
    0x65, 0x64, 0x2c, 0x20, 0x6f, 0x72, 0x20, 0x6f, 0x62, 0x73, 0x6f, 0x6c,
    0x65, 0x74, 0x65, 0x64, 0x20, 0x62, 0x79, 0x20, 0x6f, 0x74, 0x68, 0x65,
    0x72, 0x20, 0x64, 0x6f, 0x63, 0x75, 0x6d, 0x65, 0x6e, 0x74, 0x73, 0x20,
    0x61, 0x74, 0x20, 0x61, 0x6e, 0x79, 0x20, 0x74, 0x69, 0x6d, 0x65, 0x2e,
    0x20, 0x49, 0x74, 0x20, 0x69, 0x73, 0x20, 0x69, 0x6e, 0x61, 0x70, 0x70,
    0x72, 0x6f, 0x70, 0x72, 0x69, 0x61, 0x74, 0x65, 0x20, 0x74, 0x6f, 0x20,
    0x75, 0x73, 0x65, 0x20, 0x49, 0x6e, 0x74, 0x65, 0x72, 0x6e, 0x65, 0x74,
    0x2d, 0x44, 0x72, 0x61, 0x66, 0x74, 0x73, 0x20, 0x61, 0x73, 0x20, 0x72,
    0x65, 0x66, 0x65, 0x72, 0x65, 0x6e, 0x63, 0x65, 0x20, 0x6d, 0x61, 0x74,
    0x65, 0x72, 0x69, 0x61, 0x6c, 0x20, 0x6f, 0x72, 0x20, 0x74, 0x6f, 0x20,
    0x63, 0x69, 0x74, 0x65, 0x20, 0x74, 0x68, 0x65, 0x6d, 0x20, 0x6f, 0x74,
    0x68, 0x65, 0x72, 0x20, 0x74, 0x68, 0x61, 0x6e, 0x20, 0x61, 0x73, 0x20,
    0x2f, 0xe2, 0x80, 0x9c, 0x77, 0x6f, 0x72, 0x6b, 0x20, 0x69, 0x6e, 0x20,
    0x70, 0x72, 0x6f, 0x67, 0x72, 0x65, 0x73, 0x73, 0x2e, 0x2f, 0xe2, 0x80,
    0x9d};

/* Filling helper: deterministic pseudo-random fill from a 32-bit seed. */
static void fillPseudoRandom(uint8_t *buf, size_t n, uint32_t seed) {
    uint32_t s = seed ? seed : 1;
    for (size_t i = 0; i < n; i++) {
        s = s * 1664525u + 1013904223u;
        buf[i] = (uint8_t)(s >> 24);
    }
}

/* Pthread worker for the concurrency test. */
typedef struct mtArgs {
    /* Field order: largest -> smallest. */
    int64_t errors;
    uint32_t thread_id;
    uint32_t iters;
} mtArgs;

static void *mtWorker(void *p) {
    mtArgs *a = (mtArgs *)p;
    a->errors = 0;
    for (uint32_t i = 0; i < a->iters; i++) {
        uint8_t key[32], nonce[12], aad[16], tag[16];
        size_t pt_len = (size_t)((a->thread_id * 17u + i * 31u) % 257u);
        uint8_t pt[260], ct[260], out[260];

        uint32_t seed = (a->thread_id << 16) ^ i ^ 0xdeadbeefu;
        fillPseudoRandom(key, sizeof(key), seed + 1);
        fillPseudoRandom(nonce, sizeof(nonce), seed + 2);
        fillPseudoRandom(aad, sizeof(aad), seed + 3);
        fillPseudoRandom(pt, pt_len, seed + 4);

        (void)chacha20poly1305Seal(key, nonce, aad, sizeof(aad), pt, pt_len, ct,
                                   tag);
        bool ok = chacha20poly1305Open(key, nonce, aad, sizeof(aad), ct, pt_len,
                                       tag, out);
        if (!ok || memcmp(pt, out, pt_len) != 0) {
            a->errors++;
        }
    }
    return NULL;
}

static void runBench(size_t payload, uint32_t iters) {
    uint8_t key[32], nonce[12], tag[16];
    memset(key, 0xa5, sizeof(key));
    memset(nonce, 0x5a, sizeof(nonce));
    cleanerName(zfree, Bytes) uint8_t *pt = zmalloc(payload);
    cleanerName(zfree, Bytes) uint8_t *ct = zmalloc(payload);
    cleanerName(zfree, Bytes) uint8_t *out = zmalloc(payload);
    memset(pt, 0x42, payload);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (uint32_t i = 0; i < iters; i++) {
        (void)chacha20poly1305Seal(key, nonce, NULL, 0, pt, payload, ct, tag);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double seal_secs = (double)(t1.tv_sec - t0.tv_sec) +
                       (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    double mb = (double)((uint64_t)payload * iters) / (1000.0 * 1000.0);
    double seal_ns_op = seal_secs * 1e9 / (double)iters;
    double seal_mbs = seal_secs > 0 ? mb / seal_secs : 0.0;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (uint32_t i = 0; i < iters; i++) {
        (void)chacha20poly1305Open(key, nonce, NULL, 0, ct, payload, tag, out);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double open_secs = (double)(t1.tv_sec - t0.tv_sec) +
                       (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    double open_ns_op = open_secs * 1e9 / (double)iters;
    double open_mbs = open_secs > 0 ? mb / open_secs : 0.0;

    printf("    bench payload=%zuB iters=%u: seal %.1f ns/op (%.1f MB/s)   "
           "open %.1f ns/op (%.1f MB/s)\n",
           payload, iters, seal_ns_op, seal_mbs, open_ns_op, open_mbs);
}

int chacha20poly1305Test(int, char *[]) {
    int err = 0;

    TEST("RFC 8439 §2.4.2: ChaCha20 stream encryption") {
        uint8_t ct[114];
        chacha20XorStream(rfc_2_4_2_key, 1, rfc_2_4_2_nonce,
                          rfc_2_4_2_plaintext, ct, sizeof(ct));
        if (!bytesEqual(ct, rfc_2_4_2_ciphertext, sizeof(ct))) {
            err += reportMismatch("2.4.2 ciphertext", ct, rfc_2_4_2_ciphertext,
                                  sizeof(ct));
        }
    }

    TEST("RFC 8439 §2.5.2: Poly1305 MAC") {
        poly1305State st;
        poly1305Init(&st, rfc_2_5_2_key);
        poly1305Update(&st, rfc_2_5_2_msg, sizeof(rfc_2_5_2_msg));
        uint8_t tag[16];
        poly1305Final(&st, tag);
        if (!bytesEqual(tag, rfc_2_5_2_tag, 16)) {
            err += reportMismatch("2.5.2 tag", tag, rfc_2_5_2_tag, 16);
        }
    }

    TEST("RFC 8439 §2.6.2: Poly1305 key generation from ChaCha20") {
        uint8_t otk[32];
        deriveOnetimeKey(rfc_2_6_2_key, rfc_2_6_2_nonce, otk);
        if (!bytesEqual(otk, rfc_2_6_2_expected_otk, 32)) {
            err += reportMismatch("2.6.2 otk", otk, rfc_2_6_2_expected_otk, 32);
        }
    }

    TEST("RFC 8439 §2.8.2: AEAD seal vector") {
        uint8_t ct[114], tag[16];
        (void)chacha20poly1305Seal(rfc_2_8_2_key, rfc_2_8_2_nonce,
                                   rfc_2_8_2_aad, sizeof(rfc_2_8_2_aad),
                                   rfc_2_8_2_plaintext,
                                   sizeof(rfc_2_8_2_plaintext), ct, tag);
        if (!bytesEqual(ct, rfc_2_8_2_ciphertext, sizeof(ct))) {
            err += reportMismatch("2.8.2 ciphertext", ct, rfc_2_8_2_ciphertext,
                                  sizeof(ct));
        }
        if (!bytesEqual(tag, rfc_2_8_2_tag, 16)) {
            err += reportMismatch("2.8.2 tag", tag, rfc_2_8_2_tag, 16);
        }

        /* Open round-trip on the same vector. */
        uint8_t pt[114];
        bool ok = chacha20poly1305Open(rfc_2_8_2_key, rfc_2_8_2_nonce,
                                       rfc_2_8_2_aad, sizeof(rfc_2_8_2_aad), ct,
                                       sizeof(ct), tag, pt);
        if (!ok) {
            ERR("2.8.2 Open returned false on its own seal output");
        }
        if (!bytesEqual(pt, rfc_2_8_2_plaintext, sizeof(pt))) {
            err += reportMismatch("2.8.2 plaintext", pt, rfc_2_8_2_plaintext,
                                  sizeof(pt));
        }
    }

    TEST("RFC 8439 Appendix A.5: AEAD vector") {
        uint8_t ct[265], tag[16];
        (void)chacha20poly1305Seal(rfc_a5_key, rfc_a5_nonce, rfc_a5_aad,
                                   sizeof(rfc_a5_aad), rfc_a5_plaintext,
                                   sizeof(rfc_a5_plaintext), ct, tag);
        if (!bytesEqual(ct, rfc_a5_ciphertext, sizeof(ct))) {
            err += reportMismatch("A.5 ciphertext", ct, rfc_a5_ciphertext,
                                  sizeof(ct));
        }
        if (!bytesEqual(tag, rfc_a5_tag, 16)) {
            err += reportMismatch("A.5 tag", tag, rfc_a5_tag, 16);
        }

        uint8_t pt[265];
        bool ok =
            chacha20poly1305Open(rfc_a5_key, rfc_a5_nonce, rfc_a5_aad,
                                 sizeof(rfc_a5_aad), ct, sizeof(ct), tag, pt);
        if (!ok) {
            ERR("A.5 Open returned false");
        }
        if (!bytesEqual(pt, rfc_a5_plaintext, sizeof(pt))) {
            err += reportMismatch("A.5 plaintext", pt, rfc_a5_plaintext,
                                  sizeof(pt));
        }
    }

    TEST("Round-trip (no AAD) across size spread") {
        static constexpr size_t sizes[] = {0,   1,   63,   64,   65,
                                           127, 128, 1024, 16384};
        uint8_t key[32], nonce[12], tag[16];
        fillPseudoRandom(key, sizeof(key), 0xC0FFEE);
        fillPseudoRandom(nonce, sizeof(nonce), 0xDEADBEEF);

        for (size_t si = 0; si < countof(sizes); si++) {
            size_t n = sizes[si];
            cleanerName(zfree, Bytes) uint8_t *pt = (n > 0) ? zmalloc(n) : NULL;
            cleanerName(zfree, Bytes) uint8_t *ct = (n > 0) ? zmalloc(n) : NULL;
            cleanerName(zfree, Bytes) uint8_t *out =
                (n > 0) ? zmalloc(n) : NULL;
            if (n > 0) {
                fillPseudoRandom(pt, n, (uint32_t)(0x100 + n));
            }
            (void)chacha20poly1305Seal(key, nonce, NULL, 0, pt, n, ct, tag);
            bool ok =
                chacha20poly1305Open(key, nonce, NULL, 0, ct, n, tag, out);
            if (!ok) {
                ERR("round-trip Open failed size=%zu", n);
            }
            if (n > 0 && memcmp(pt, out, n) != 0) {
                ERR("round-trip mismatch size=%zu", n);
            }
        }
    }

    TEST("Round-trip with AAD across size + AAD spread") {
        static constexpr size_t sizes[] = {0,   1,   63,   64,   65,
                                           127, 128, 1024, 16384};
        static constexpr size_t aad_sizes[] = {0, 1, 15, 16, 17, 64, 257};
        uint8_t key[32], nonce[12], tag[16];
        fillPseudoRandom(key, sizeof(key), 0xCAFEBABE);
        fillPseudoRandom(nonce, sizeof(nonce), 0xFACEFEED);

        for (size_t si = 0; si < countof(sizes); si++) {
            for (size_t ai = 0; ai < countof(aad_sizes); ai++) {
                size_t n = sizes[si];
                size_t alen = aad_sizes[ai];
                cleanerName(zfree, Bytes) uint8_t *pt =
                    (n > 0) ? zmalloc(n) : NULL;
                cleanerName(zfree, Bytes) uint8_t *ct =
                    (n > 0) ? zmalloc(n) : NULL;
                cleanerName(zfree, Bytes) uint8_t *out =
                    (n > 0) ? zmalloc(n) : NULL;
                cleanerName(zfree, Bytes) uint8_t *aad =
                    (alen > 0) ? zmalloc(alen) : NULL;
                if (n > 0) {
                    fillPseudoRandom(pt, n, (uint32_t)(0x200 + n + alen));
                }
                if (alen > 0) {
                    fillPseudoRandom(aad, alen, (uint32_t)(0x300 + alen));
                }
                (void)chacha20poly1305Seal(key, nonce, aad, alen, pt, n, ct,
                                           tag);
                bool ok = chacha20poly1305Open(key, nonce, aad, alen, ct, n,
                                               tag, out);
                if (!ok) {
                    ERR("aad round-trip Open failed n=%zu aad=%zu", n, alen);
                }
                if (n > 0 && memcmp(pt, out, n) != 0) {
                    ERR("aad round-trip mismatch n=%zu aad=%zu", n, alen);
                }
            }
        }
    }

    TEST("In-place encryption (ciphertext aliases plaintext)") {
        uint8_t key[32], nonce[12], tag[16];
        fillPseudoRandom(key, sizeof(key), 0x11111111);
        fillPseudoRandom(nonce, sizeof(nonce), 0x22222222);
        const size_t n = 1500;
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(n);
        cleanerName(zfree, Bytes) uint8_t *ref = zmalloc(n);
        fillPseudoRandom(buf, n, 0x33333333);
        memcpy(ref, buf, n);

        (void)chacha20poly1305Seal(key, nonce, NULL, 0, buf, n, buf, tag);
        /* buf now holds ciphertext.  Decrypt in place. */
        bool ok = chacha20poly1305Open(key, nonce, NULL, 0, buf, n, tag, buf);
        if (!ok) {
            ERR("in-place Open returned false");
        }
        if (memcmp(buf, ref, n) != 0) {
            ERR("in-place mismatch after Seal/Open");
        }
    }

    TEST("Tamper detection: flip each ciphertext byte") {
        uint8_t key[32], nonce[12], aad[8], tag[16], saved_tag[16];
        const size_t n = 96;
        uint8_t pt[96], ct[96], out[96];
        fillPseudoRandom(key, sizeof(key), 0x44);
        fillPseudoRandom(nonce, sizeof(nonce), 0x55);
        fillPseudoRandom(aad, sizeof(aad), 0x66);
        fillPseudoRandom(pt, n, 0x77);
        (void)chacha20poly1305Seal(key, nonce, aad, sizeof(aad), pt, n, ct,
                                   tag);
        memcpy(saved_tag, tag, 16);

        for (size_t i = 0; i < n; i++) {
            uint8_t saved = ct[i];
            ct[i] ^= 0x55;
            memcpy(tag, saved_tag, 16);
            bool ok = chacha20poly1305Open(key, nonce, aad, sizeof(aad), ct, n,
                                           tag, out);
            if (ok) {
                ERR("tamper ct[%zu] accepted", i);
            }
            ct[i] = saved;
        }
    }

    TEST("Tamper detection: flip each tag byte") {
        uint8_t key[32], nonce[12], aad[8], tag[16];
        const size_t n = 64;
        uint8_t pt[64], ct[64], out[64];
        fillPseudoRandom(key, sizeof(key), 0xAA);
        fillPseudoRandom(nonce, sizeof(nonce), 0xBB);
        fillPseudoRandom(aad, sizeof(aad), 0xCC);
        fillPseudoRandom(pt, n, 0xDD);
        (void)chacha20poly1305Seal(key, nonce, aad, sizeof(aad), pt, n, ct,
                                   tag);

        for (size_t i = 0; i < 16; i++) {
            uint8_t bad[16];
            memcpy(bad, tag, 16);
            bad[i] ^= 0xff;
            bool ok = chacha20poly1305Open(key, nonce, aad, sizeof(aad), ct, n,
                                           bad, out);
            if (ok) {
                ERR("tamper tag[%zu] accepted", i);
            }
        }
    }

    TEST("Tamper detection: flip each AAD byte") {
        uint8_t key[32], nonce[12], aad[32], tag[16];
        const size_t n = 100;
        uint8_t pt[100], ct[100], out[100];
        fillPseudoRandom(key, sizeof(key), 0xE1);
        fillPseudoRandom(nonce, sizeof(nonce), 0xE2);
        fillPseudoRandom(aad, sizeof(aad), 0xE3);
        fillPseudoRandom(pt, n, 0xE4);
        (void)chacha20poly1305Seal(key, nonce, aad, sizeof(aad), pt, n, ct,
                                   tag);

        for (size_t i = 0; i < sizeof(aad); i++) {
            uint8_t saved = aad[i];
            aad[i] ^= 0x01;
            bool ok = chacha20poly1305Open(key, nonce, aad, sizeof(aad), ct, n,
                                           tag, out);
            if (ok) {
                ERR("tamper aad[%zu] accepted", i);
            }
            aad[i] = saved;
        }
    }

    TEST("Tamper detection: nonce / key modification") {
        uint8_t key[32], nonce[12], tag[16];
        const size_t n = 80;
        uint8_t pt[80], ct[80], out[80];
        fillPseudoRandom(key, sizeof(key), 0xF1);
        fillPseudoRandom(nonce, sizeof(nonce), 0xF2);
        fillPseudoRandom(pt, n, 0xF3);
        (void)chacha20poly1305Seal(key, nonce, NULL, 0, pt, n, ct, tag);

        uint8_t bad_nonce[12];
        memcpy(bad_nonce, nonce, 12);
        bad_nonce[0] ^= 0x01;
        if (chacha20poly1305Open(key, bad_nonce, NULL, 0, ct, n, tag, out)) {
            ERR("bad nonce accepted");
        }

        uint8_t bad_key[32];
        memcpy(bad_key, key, 32);
        bad_key[31] ^= 0x80;
        if (chacha20poly1305Open(bad_key, nonce, NULL, 0, ct, n, tag, out)) {
            ERR("bad key accepted");
        }
    }

    TEST("Plaintext zeroed on failed Open") {
        uint8_t key[32], nonce[12], tag[16];
        const size_t n = 200;
        uint8_t pt[200], ct[200], out[200];
        fillPseudoRandom(key, sizeof(key), 0x77);
        fillPseudoRandom(nonce, sizeof(nonce), 0x88);
        fillPseudoRandom(pt, n, 0x99);
        (void)chacha20poly1305Seal(key, nonce, NULL, 0, pt, n, ct, tag);

        memset(out, 0xab, sizeof(out));
        uint8_t bad_tag[16];
        memcpy(bad_tag, tag, 16);
        bad_tag[0] ^= 0xff;

        bool ok =
            chacha20poly1305Open(key, nonce, NULL, 0, ct, n, bad_tag, out);
        if (ok) {
            ERR("bad-tag Open returned true");
        }
        for (size_t i = 0; i < n; i++) {
            if (out[i] != 0) {
                ERR("plaintext[%zu]=0x%02x not zeroed after a failed Open", i,
                    out[i]);
                break;
            }
        }
    }

    TEST("Streaming Seal/Open: matches one-shot for varied chunk sizes") {
        const uint8_t key[32] = {0x55, 0x66, 0x77, 0x88};
        const uint8_t nonce[12] = {0x01, 0x02, 0x03, 0x04, 0x05,
                                   0x06, 0x07, 0x08, 0x09, 0x0a};
        const uint8_t aad[] = "header bytes";
        const size_t aad_len = sizeof(aad) - 1u;
        const size_t test_sizes[] = {0u,    1u,    63u,   64u,   65u,
                                     127u,  128u,  129u,  255u,  256u,
                                     1023u, 1024u, 1025u, 4096u, 8192u};
        const size_t num_sizes = countof(test_sizes);
        const size_t chunk_sizes[] = {1u,   7u,   16u,   31u,     32u,
                                      33u,  63u,  64u,   65u,     127u,
                                      128u, 256u, 1024u, SIZE_MAX};
        const size_t num_chunks = countof(chunk_sizes);
        cleanerName(zfree, Bytes) uint8_t *plain = zmalloc(8192u);
        for (size_t i = 0; i < 8192u; i++) {
            plain[i] = (uint8_t)(i * 17u + 3u);
        }

        for (size_t s = 0; s < num_sizes; s++) {
            const size_t pl = test_sizes[s];
            /* One-shot baseline. */
            uint8_t one_ct[8192], one_tag[16];
            chacha20poly1305Seal(key, nonce, aad, aad_len, plain, pl, one_ct,
                                 one_tag);
            for (size_t c = 0; c < num_chunks; c++) {
                size_t chunk = chunk_sizes[c];
                /* Stream Seal. */
                chacha20poly1305StreamCtx ctx;
                chacha20poly1305SealStreamInit(&ctx, key, nonce);
                /* Feed AAD in two chunks if it has length > 1. */
                if (aad_len > 0) {
                    chacha20poly1305SealStreamAad(&ctx, aad, aad_len / 2);
                    chacha20poly1305SealStreamAad(&ctx, aad + aad_len / 2,
                                                  aad_len - aad_len / 2);
                }
                uint8_t stream_ct[8192];
                size_t pos = 0;
                while (pos < pl) {
                    size_t take = chunk < (pl - pos) ? chunk : (pl - pos);
                    if (!chacha20poly1305SealStreamUpdate(
                            &ctx, plain + pos, take, stream_ct + pos)) {
                        ERR("stream seal update failed at pl=%zu chunk=%zu "
                            "pos=%zu",
                            pl, chunk, pos);
                        break;
                    }
                    pos += take;
                }
                uint8_t stream_tag[16];
                if (!chacha20poly1305SealStreamFinal(&ctx, stream_tag)) {
                    ERR("stream seal final failed at pl=%zu chunk=%zu", pl,
                        chunk);
                }
                if (memcmp(one_ct, stream_ct, pl) != 0) {
                    ERR("stream ct mismatch pl=%zu chunk=%zu", pl, chunk);
                }
                if (memcmp(one_tag, stream_tag, 16) != 0) {
                    ERR("stream tag mismatch pl=%zu chunk=%zu", pl, chunk);
                }
                /* Stream Open round-trip from the streaming output. */
                chacha20poly1305OpenStreamInit(&ctx, key, nonce);
                if (aad_len > 0) {
                    chacha20poly1305OpenStreamAad(&ctx, aad, aad_len);
                }
                uint8_t recovered[8192];
                pos = 0;
                while (pos < pl) {
                    size_t take = chunk < (pl - pos) ? chunk : (pl - pos);
                    if (!chacha20poly1305OpenStreamUpdate(
                            &ctx, stream_ct + pos, take, recovered + pos)) {
                        ERR("stream open update failed pl=%zu chunk=%zu", pl,
                            chunk);
                        break;
                    }
                    pos += take;
                }
                if (!chacha20poly1305OpenStreamFinal(&ctx, stream_tag)) {
                    ERR("stream open final failed pl=%zu chunk=%zu", pl, chunk);
                }
                if (pl > 0 && memcmp(plain, recovered, pl) != 0) {
                    ERR("stream open plaintext mismatch pl=%zu chunk=%zu", pl,
                        chunk);
                }
            }
        }
    }

    TEST("Streaming Open: tampered tag is rejected, ctx poisoned") {
        const uint8_t key[32] = {0xCC};
        const uint8_t nonce[12] = {0x11};
        const uint8_t pt[100] = {0x42};
        uint8_t ct[100], tag[16];
        chacha20poly1305Seal(key, nonce, NULL, 0, pt, sizeof(pt), ct, tag);

        /* Flip a tag byte. */
        tag[7] ^= 0x01;
        chacha20poly1305StreamCtx ctx;
        chacha20poly1305OpenStreamInit(&ctx, key, nonce);
        uint8_t recovered[100];
        chacha20poly1305OpenStreamUpdate(&ctx, ct, sizeof(ct), recovered);
        if (chacha20poly1305OpenStreamFinal(&ctx, tag)) {
            ERR("Tampered tag must be rejected");
        }
        /* Ctx is poisoned; further Final calls also fail. */
        if (chacha20poly1305OpenStreamFinal(&ctx, tag)) {
            ERR("Poisoned ctx must keep rejecting");
        }
    }

    TEST("Streaming: AAD after Update poisons ctx") {
        const uint8_t key[32] = {0xAA};
        const uint8_t nonce[12] = {0xBB};
        chacha20poly1305StreamCtx ctx;
        chacha20poly1305SealStreamInit(&ctx, key, nonce);
        uint8_t out[4];
        const uint8_t pt[4] = {1, 2, 3, 4};
        if (!chacha20poly1305SealStreamUpdate(&ctx, pt, 4, out)) {
            ERR("first update must succeed");
        }
        /* AAD now is out of order. */
        const uint64_t diagBefore = dkDiagCount();
        if (chacha20poly1305SealStreamAad(&ctx, pt, 4)) {
            ERR("AAD after update must be refused");
        }
        /* Subsequent ops on poisoned ctx all fail. */
        if (chacha20poly1305SealStreamUpdate(&ctx, pt, 4, out)) {
            ERR("update on poisoned ctx must fail");
        }
        uint8_t tag[16];
        if (chacha20poly1305SealStreamFinal(&ctx, tag)) {
            ERR("final on poisoned ctx must fail");
        }
        /* Each out-of-order call is reported once */
        if (dkDiagCount() != diagBefore + 3) {
            ERR("three out-of-order stream calls reported %" PRIu64
                " times, want 3",
                dkDiagCount() - diagBefore);
        }
    }

    TEST("Multi-thread round-trip (8 threads x 256 iters)") {
        static constexpr uint32_t kThreads = 8u;
        pthread_t threads[8];
        mtArgs args[8];
        for (uint32_t i = 0; i < kThreads; i++) {
            args[i].errors = 0;
            args[i].thread_id = i + 1;
            args[i].iters = 256;
            int rc = pthread_create(&threads[i], NULL, mtWorker, &args[i]);
            if (rc != 0) {
                ERR("pthread_create rc=%d", rc);
            }
        }
        for (uint32_t i = 0; i < kThreads; i++) {
            pthread_join(threads[i], NULL);
            if (args[i].errors != 0) {
                /* one report for the thread, counting all its errors */
                ERR("thread %u: %" PRId64 " errors", i, args[i].errors);
                err += (int)args[i].errors - 1;
            }
        }
    }

    TEST("Counter-wrap guard refuses oversized payloads") {
        /* CHACHA20_POLY1305_MAX_PAYLOAD = (2^32 - 1) * 64 bytes ≈ 256
         * GiB.  Triggering the boundary by actual allocation is not
         * feasible, so test the guard logic with NULL pointers and a
         * size value past the bound.  The check is the FIRST statement
         * in Seal/Open so we never dereference plaintext/ciphertext on
         * the rejection path.
         *
         * On 32-bit platforms SIZE_MAX < MAX_PAYLOAD and this size
         * cannot be expressed; skip the test there. */
        if ((uint64_t)SIZE_MAX <= CHACHA20_POLY1305_MAX_PAYLOAD) {
            printf("    (skipped on 32-bit: size_t cannot exceed bound)\n");
        } else {
            const size_t over = (size_t)(CHACHA20_POLY1305_MAX_PAYLOAD + 1u);
            /* Real (small) buffers, so the NULL-buffer misuse check
             * cannot be what refuses: the bound must, before either
             * buffer is read, and with no misuse reported */
            uint8_t key[32] = {};
            uint8_t nonce[12] = {};
            uint8_t tag[16] = {};
            uint8_t in_small[32] = {};
            uint8_t ct_sentinel[32];
            memset(ct_sentinel, 0xAA, sizeof(ct_sentinel));
            const uint64_t diagBefore = dkDiagCount();
            if (chacha20poly1305Seal(key, nonce, NULL, 0u, in_small, over,
                                     ct_sentinel, tag)) {
                ERR("Seal must refuse plaintext_len past 256 GiB bound");
            }
            for (uint32_t i = 0; i < 32u; i++) {
                if (ct_sentinel[i] != 0xAAu) {
                    ERR("Seal wrote ciphertext during bound rejection "
                        "(offset %u)",
                        i);
                    break;
                }
            }
            uint8_t pt_sentinel[32];
            memset(pt_sentinel, 0xAA, sizeof(pt_sentinel));
            if (chacha20poly1305Open(key, nonce, NULL, 0u, in_small, over, tag,
                                     pt_sentinel)) {
                ERR("Open must refuse ciphertext_len past 256 GiB bound");
            }
            if (dkDiagCount() != diagBefore) {
                ERR("a bound rejection was refused as misuse instead");
            }
            /* Open is documented to zero the plaintext buffer on
             * failure — verify the first 32 bytes were touched.  (The
             * full `over`-sized buffer would not fit in memory; we
             * pass a pointer to 32 bytes and rely on Open's memset to
             * write all `ciphertext_len` bytes there — except Open
             * sees `ciphertext_len > MAX` and rejects FIRST, before
             * the memset.  So the sentinel should be UNCHANGED.) */
            for (uint32_t i = 0; i < 32u; i++) {
                if (pt_sentinel[i] != 0xAAu) {
                    ERR("Open touched plaintext buffer during bound "
                        "rejection (offset %u)",
                        i);
                    break;
                }
            }
        }
    }

    TEST("Bulk-path correctness parity across SIMD length sweep "
         "(testChachaBulkWipesState)") {
        /* The bulk-path state[] wipe in chacha20XorStream is a stack-
         * residue defense.  Stack residue is not directly observable
         * from outside the function (the frame is released).  Instead
         * we assert the only thing we CAN observe: that adding the
         * wipe did not break correctness — bit-exact parity between
         * the SIMD bulk path and the scalar block path across a length
         * sweep that includes every transition boundary (1-block tail,
         * partial AVX2 batch, partial NEON batch, plus exact multiples
         * of 256 and 512 to exercise the bulk loops repeatedly). */
        const size_t sizes[] = {0u,    1u,    63u,   64u,   65u,   128u,
                                255u,  256u,  257u,  384u,  511u,  512u,
                                513u,  768u,  1023u, 1024u, 1025u, 1536u,
                                2047u, 2048u, 2049u, 4096u, 8192u, 16384u};
        const size_t n_sizes = countof(sizes);

        uint8_t key[32], nonce[12];
        fillPseudoRandom(key, sizeof(key), 0xBEEFCAFE);
        fillPseudoRandom(nonce, sizeof(nonce), 0x0DD0F00D);

        chacha20ForceBackend saved = chacha20GetForceBackend();
        for (size_t i = 0; i < n_sizes; i++) {
            const size_t n = sizes[i];
            cleanerName(zfree, Bytes) uint8_t *in = (n > 0) ? zmalloc(n) : NULL;
            cleanerName(zfree, Bytes) uint8_t *out_simd =
                (n > 0) ? zmalloc(n) : NULL;
            cleanerName(zfree, Bytes) uint8_t *out_scalar =
                (n > 0) ? zmalloc(n) : NULL;
            if (n > 0) {
                fillPseudoRandom(in, n, (uint32_t)(0xABCD + n));
            }
            /* Scalar baseline. */
            chacha20SetForceBackend(CHACHA20_FORCE_SCALAR);
            chacha20XorStream(key, 1, nonce, in, out_scalar, n);
            /* AUTO path (real SIMD on hosts that have it). */
            chacha20SetForceBackend(CHACHA20_FORCE_AUTO);
            chacha20XorStream(key, 1, nonce, in, out_simd, n);
            if (n > 0 && memcmp(out_simd, out_scalar, n) != 0) {
                ERR("simd vs scalar mismatch at len=%zu", n);
            }
        }
        chacha20SetForceBackend(saved);
    }

    TEST("Bulk-path state[16] wipe abstraction "
         "(testChachaBulkStateWipeProof)") {
        /* Limitation: chacha20XorStream's `uint32_t state[16]` (per
         * src/chacha20poly1305.c:259, 289) is a stack-local in the SIMD
         * keystream-batch generators.  After the function returns the
         * stack frame is released and the residue is unreachable from
         * outside — no caller can directly observe whether
         * dkSecureZero(state, sizeof(state)) actually ran.
         *
         * Workaround: mirror the production wipe pattern on a heap-
         * allocated 16-word state buffer pre-filled with 0xA5, apply
         * dkSecureZero, and assert the full region is zero.  This pins
         * the abstraction the bulk-path wipe relies on.  It does NOT
         * prove the production code invokes dkSecureZero — that
         * property is enforced by code review of the bulk-path
         * epilogue.  Pairs with testChachaBulkWipesState which proves
         * the wipe did not perturb output. */
        cleanerName(zfree, Words) uint32_t *state =
            (uint32_t *)zmalloc(16u * sizeof(uint32_t));
        memset(state, 0xA5, 16u * sizeof(uint32_t));
        dkSecureZero(state, 16u * sizeof(uint32_t));
        const uint8_t *p = (const uint8_t *)state;
        for (size_t i = 0; i < 16u * sizeof(uint32_t); i++) {
            if (p[i] != 0) {
                ERR("state[16] dkSecureZero left nonzero byte at offset "
                    "%zu (=0x%02x)",
                    i, p[i]);
                break;
            }
        }
    }

    TEST("Open NULL guards parity (testChachaOpenNullParity)") {
        /* Parity with aesGcmPortableOpen: NULL ciphertext or NULL
         * plaintext with non-zero len must return false instead of
         * dereferencing the NULL. */
        uint8_t key[32] = {};
        uint8_t nonce[12] = {};
        uint8_t tag[16] = {};
        uint8_t pt_buf[16];
        memset(pt_buf, 0xCC, sizeof(pt_buf));
        uint8_t ct_buf[16];
        memset(ct_buf, 0xDD, sizeof(ct_buf));

        const uint64_t diagBefore = dkDiagCount();
        if (chacha20poly1305Open(key, nonce, NULL, 0u, NULL, 16u, tag,
                                 pt_buf)) {
            ERR("Open with NULL ciphertext (len>0) must return false");
        }
        if (chacha20poly1305Open(key, nonce, NULL, 0u, ct_buf, 16u, tag,
                                 NULL)) {
            ERR("Open with NULL plaintext (len>0) must return false");
        }
        if (chacha20poly1305Seal(key, nonce, NULL, 0u, NULL, 16u, ct_buf,
                                 tag)) {
            ERR("Seal with NULL plaintext (len>0) must return false");
        }
        /* Each misuse is reported once */
        dkDiagnostic d;
        if (dkDiagCount() != diagBefore + 3 || !dkDiagLast(&d) ||
            d.code != DK_DIAG_AEAD_MISUSE) {
            ERR("three NULL-buffer calls reported %" PRIu64 " times, want 3",
                dkDiagCount() - diagBefore);
        }
        /* len == 0 with NULL pointers must remain valid (no read, no
         * write).  It's effectively an authenticate-AAD-only call. */
        const uint64_t diagValid = dkDiagCount();
        if (chacha20poly1305Open(key, nonce, NULL, 0u, NULL, 0u, tag, NULL)) {
            /* Tag is all-zero and won't match the real MAC of empty
             * input under this key/nonce, so a false is expected — but
             * the caller-supplied NULLs must not crash before that. */
        }
        /* A tag that does not verify is data, not misuse: nothing is
         * reported */
        if (dkDiagCount() != diagValid) {
            ERR("a failed tag check must not report");
        }
    }

    TEST("Streaming Open: poison transitions wipe ctx "
         "(testChachaOpenPoisonWipes)") {
        /* OpenStreamUpdate's three poison transitions are: (1) wrong
         * phase / NULL ctx, (2) NULL in/out with non-zero len, (3)
         * counter overflow.  Each must now leave the entire ctx
         * bytewise zero (mirrors streamFinalise's failure path).
         *
         * For (1) we exercise the wrong-phase branch — call Update
         * without ever calling Init, with a ctx pre-filled with a
         * recognizable secret pattern, then assert the ctx region is
         * zero.  For (2) we Init properly, then call Update with NULL
         * inputs and non-zero len.  In both cases we also confirm the
         * post-wipe ctx cannot be revived. */
        chacha20poly1305StreamCtx ctx;

        /* (1) wrong-phase poison: pre-fill ctx with a recognizable
         * pattern, then call Update without Init.  Update sees
         * phase != AAD && != BODY and must wipe. */
        memset(&ctx, 0x5A, sizeof(ctx));
        uint8_t out_buf[8];
        uint8_t in_buf[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        if (chacha20poly1305OpenStreamUpdate(&ctx, in_buf, sizeof(in_buf),
                                             out_buf)) {
            ERR("Update on un-initialised ctx must return false");
        }
        const uint8_t *p = (const uint8_t *)&ctx;
        for (size_t i = 0; i < sizeof(ctx); i++) {
            if (p[i] != 0) {
                ERR("wrong-phase poison left nonzero byte at offset %zu "
                    "(=0x%02x)",
                    i, p[i]);
                break;
            }
        }

        /* (2) NULL-input poison: properly init then trigger the
         * NULL-input branch.  The Init writes key+nonce; the poison
         * wipe must clear them. */
        uint8_t key[32] = {0x11};
        uint8_t nonce[12] = {0x22};
        chacha20poly1305OpenStreamInit(&ctx, key, nonce);
        if (chacha20poly1305OpenStreamUpdate(&ctx, NULL, 16u, out_buf)) {
            ERR("Update with NULL ciphertext (len>0) must return false");
        }
        for (size_t i = 0; i < sizeof(ctx); i++) {
            if (p[i] != 0) {
                ERR("null-input poison left nonzero byte at offset %zu "
                    "(=0x%02x)",
                    i, p[i]);
                break;
            }
        }

        /* (3) ct_total overflow poison: re-init, force ct_total to
         * MAX_PAYLOAD so the next byte would pass it, then Update.  On
         * 32-bit (SIZE_MAX < MAX_PAYLOAD) skip, as the Seal test does. */
        chacha20poly1305OpenStreamInit(&ctx, key, nonce);
        if ((uint64_t)SIZE_MAX > CHACHA20_POLY1305_MAX_PAYLOAD) {
            ctx.ct_total = CHACHA20_POLY1305_MAX_PAYLOAD;
            if (chacha20poly1305OpenStreamUpdate(&ctx, in_buf, 1u, out_buf)) {
                ERR("Open Update past MAX_PAYLOAD must return false");
            }
            for (size_t i = 0; i < sizeof(ctx); i++) {
                if (p[i] != 0) {
                    ERR("Open ct_total-overflow poison left nonzero byte at "
                        "offset %zu (=0x%02x)",
                        i, p[i]);
                    break;
                }
            }
        }

        /* Post-wipe: ctx phase is 0 (UNINIT); subsequent Update must
         * fail again (and re-wipe, no-op since already zero). */
        if (chacha20poly1305OpenStreamUpdate(&ctx, in_buf, sizeof(in_buf),
                                             out_buf)) {
            ERR("Update on post-poison wiped ctx must remain failing");
        }
    }

    TEST("Streaming Seal: poison transitions wipe ctx "
         "(testSealStreamUpdatePoisonWipes)") {
        /* Symmetric companion to testChachaOpenPoisonWipes for
         * SealStreamUpdate.  The three poison transitions are: (1)
         * wrong phase (Update before Init or after Final), (2) NULL
         * plaintext/ciphertext_out with non-zero len, (3) ct_total
         * overflow past CHACHA20_POLY1305_MAX_PAYLOAD.  Each must
         * leave the ctx bytewise zero. */
        chacha20poly1305StreamCtx ctx;
        const uint8_t *p = (const uint8_t *)&ctx;
        uint8_t out_buf[8];
        uint8_t in_buf[8] = {1, 2, 3, 4, 5, 6, 7, 8};

        /* (1) wrong-phase poison: pre-fill ctx with 0x5A pattern, then
         * call Update without Init. */
        memset(&ctx, 0x5A, sizeof(ctx));
        if (chacha20poly1305SealStreamUpdate(&ctx, in_buf, sizeof(in_buf),
                                             out_buf)) {
            ERR("Seal Update on un-initialised ctx must return false");
        }
        for (size_t i = 0; i < sizeof(ctx); i++) {
            if (p[i] != 0) {
                ERR("Seal wrong-phase poison left nonzero byte at "
                    "offset %zu (=0x%02x)",
                    i, p[i]);
                break;
            }
        }

        /* (2) NULL-input poison: properly init then trigger the
         * NULL-input branch.  Init writes key+nonce; wipe must clear. */
        uint8_t key[32] = {0x33};
        uint8_t nonce[12] = {0x44};
        chacha20poly1305SealStreamInit(&ctx, key, nonce);
        if (chacha20poly1305SealStreamUpdate(&ctx, NULL, 16u, out_buf)) {
            ERR("Seal Update with NULL plaintext (len>0) must return false");
        }
        for (size_t i = 0; i < sizeof(ctx); i++) {
            if (p[i] != 0) {
                ERR("Seal null-input poison left nonzero byte at "
                    "offset %zu (=0x%02x)",
                    i, p[i]);
                break;
            }
        }

        /* (3) ct_total overflow poison: re-init, force ct_total close to
         * MAX_PAYLOAD so the next chunk would push past, then call
         * Update.  On 32-bit (SIZE_MAX < MAX_PAYLOAD) skip — the size
         * value cannot be expressed. */
        chacha20poly1305SealStreamInit(&ctx, key, nonce);
        if ((uint64_t)SIZE_MAX > CHACHA20_POLY1305_MAX_PAYLOAD) {
            ctx.ct_total = CHACHA20_POLY1305_MAX_PAYLOAD;
            const size_t over_chunk = 1u;
            if (chacha20poly1305SealStreamUpdate(&ctx, in_buf, over_chunk,
                                                 out_buf)) {
                ERR("Seal Update past MAX_PAYLOAD must return false");
            }
            for (size_t i = 0; i < sizeof(ctx); i++) {
                if (p[i] != 0) {
                    ERR("Seal ct_total-overflow poison left nonzero "
                        "byte at offset %zu (=0x%02x)",
                        i, p[i]);
                    break;
                }
            }
        }

        /* Post-wipe: phase is UNINIT; subsequent ops must remain
         * fail-closed. */
        uint8_t tag[16];
        if (chacha20poly1305SealStreamUpdate(&ctx, in_buf, sizeof(in_buf),
                                             out_buf)) {
            ERR("Seal Update on post-poison wiped ctx must remain failing");
        }
        if (chacha20poly1305SealStreamFinal(&ctx, tag)) {
            ERR("Seal Final on post-poison wiped ctx must remain failing");
        }
    }

    TEST("Streaming Init: NULL-arg paths wipe ctx "
         "(testStreamInitNullArgWipes)") {
        /* SealStreamInit and OpenStreamInit re-entered with NULL key
         * or NULL nonce on a ctx that was previously initialised with
         * real secrets: the Init's rejection path must wipe the prior
         * key/nonce out of the caller's storage rather than just
         * marking phase=POISONED. */
        chacha20poly1305StreamCtx ctx;
        const uint8_t *p = (const uint8_t *)&ctx;
        uint8_t key[32] = {0x55};
        uint8_t nonce[12] = {0x66};

        /* Seal side. */
        chacha20poly1305SealStreamInit(&ctx, key, nonce);
        /* Now Init again with NULL key — prior key/nonce must be wiped. */
        chacha20poly1305SealStreamInit(&ctx, NULL, nonce);
        for (size_t i = 0; i < sizeof(ctx); i++) {
            if (p[i] != 0) {
                ERR("SealStreamInit NULL-key path left nonzero byte at "
                    "offset %zu (=0x%02x)",
                    i, p[i]);
                break;
            }
        }

        /* Re-init for the NULL-nonce variant. */
        chacha20poly1305SealStreamInit(&ctx, key, nonce);
        chacha20poly1305SealStreamInit(&ctx, key, NULL);
        for (size_t i = 0; i < sizeof(ctx); i++) {
            if (p[i] != 0) {
                ERR("SealStreamInit NULL-nonce path left nonzero byte "
                    "at offset %zu (=0x%02x)",
                    i, p[i]);
                break;
            }
        }

        /* Open side. */
        chacha20poly1305OpenStreamInit(&ctx, key, nonce);
        chacha20poly1305OpenStreamInit(&ctx, NULL, nonce);
        for (size_t i = 0; i < sizeof(ctx); i++) {
            if (p[i] != 0) {
                ERR("OpenStreamInit NULL-key path left nonzero byte at "
                    "offset %zu (=0x%02x)",
                    i, p[i]);
                break;
            }
        }

        chacha20poly1305OpenStreamInit(&ctx, key, nonce);
        chacha20poly1305OpenStreamInit(&ctx, key, NULL);
        for (size_t i = 0; i < sizeof(ctx); i++) {
            if (p[i] != 0) {
                ERR("OpenStreamInit NULL-nonce path left nonzero byte "
                    "at offset %zu (=0x%02x)",
                    i, p[i]);
                break;
            }
        }
    }

    TEST("Streaming Aad: poison transitions wipe ctx "
         "(testStreamAadCommonPoisonWipes)") {
        /* streamAadCommon has two poison paths: (1) wrong phase (Aad
         * after Update has advanced phase to BODY), (2) NULL aad with
         * non-zero len.  Both must wipe the ctx in addition to the
         * phase transition. */
        chacha20poly1305StreamCtx ctx;
        const uint8_t *p = (const uint8_t *)&ctx;
        uint8_t key[32] = {0x77};
        uint8_t nonce[12] = {0x88};
        uint8_t out_buf[4];
        const uint8_t pt[4] = {1, 2, 3, 4};
        const uint8_t aad[8] = {0xA, 0xB, 0xC, 0xD, 0xE, 0xF, 0x10, 0x11};

        /* (1) Wrong-phase Aad on Seal side: Init → Update → Aad. */
        chacha20poly1305SealStreamInit(&ctx, key, nonce);
        if (!chacha20poly1305SealStreamUpdate(&ctx, pt, sizeof(pt), out_buf)) {
            ERR("Seal Update during AAD->BODY transition must succeed");
        }
        if (chacha20poly1305SealStreamAad(&ctx, aad, sizeof(aad))) {
            ERR("Seal Aad after Update must be refused");
        }
        for (size_t i = 0; i < sizeof(ctx); i++) {
            if (p[i] != 0) {
                ERR("Seal Aad wrong-phase poison left nonzero byte at "
                    "offset %zu (=0x%02x)",
                    i, p[i]);
                break;
            }
        }

        /* (2) NULL-aad with non-zero len on Open side. */
        chacha20poly1305OpenStreamInit(&ctx, key, nonce);
        if (chacha20poly1305OpenStreamAad(&ctx, NULL, sizeof(aad))) {
            ERR("Open Aad with NULL aad (len>0) must return false");
        }
        for (size_t i = 0; i < sizeof(ctx); i++) {
            if (p[i] != 0) {
                ERR("Open Aad null-input poison left nonzero byte at "
                    "offset %zu (=0x%02x)",
                    i, p[i]);
                break;
            }
        }
    }

    TEST("Streaming: counter-wrap during Update wipes ctx "
         "(testStreamUpdateCounterWrapWipes)") {
        /* streamRefillKeystream poisons when the 32-bit ChaCha counter
         * has wrapped (counter == 0).  Reaching the wrap naturally
         * requires processing 256 GiB through one stream; instead we
         * directly stage the ctx into the wrap state by zeroing
         * chacha_counter AND marking keystream_used full so the next
         * Update byte forces a refill.  Both Seal and Open paths
         * route through streamRefillKeystream, so both should wipe. */
        chacha20poly1305StreamCtx ctx;
        const uint8_t *p = (const uint8_t *)&ctx;
        uint8_t key[32] = {0x99};
        uint8_t nonce[12] = {0xAA};
        uint8_t out_buf[16];
        uint8_t in_buf[16] = {1, 2,  3,  4,  5,  6,  7,  8,
                              9, 10, 11, 12, 13, 14, 15, 16};

        /* Seal side. */
        chacha20poly1305SealStreamInit(&ctx, key, nonce);
        /* Advance to BODY (zero-len Update transitions AAD -> BODY). */
        if (!chacha20poly1305SealStreamUpdate(&ctx, NULL, 0u, NULL)) {
            ERR("Seal Update len=0 must transition to BODY");
        }
        /* Force wrap state: counter == 0, keystream exhausted. */
        ctx.chacha_counter = 0u;
        ctx.keystream_used = 64u;
        if (chacha20poly1305SealStreamUpdate(&ctx, in_buf, sizeof(in_buf),
                                             out_buf)) {
            ERR("Seal Update on wrapped counter must return false");
        }
        for (size_t i = 0; i < sizeof(ctx); i++) {
            if (p[i] != 0) {
                ERR("Seal counter-wrap poison left nonzero byte at "
                    "offset %zu (=0x%02x)",
                    i, p[i]);
                break;
            }
        }

        /* Open side. */
        chacha20poly1305OpenStreamInit(&ctx, key, nonce);
        if (!chacha20poly1305OpenStreamUpdate(&ctx, NULL, 0u, NULL)) {
            ERR("Open Update len=0 must transition to BODY");
        }
        ctx.chacha_counter = 0u;
        ctx.keystream_used = 64u;
        if (chacha20poly1305OpenStreamUpdate(&ctx, in_buf, sizeof(in_buf),
                                             out_buf)) {
            ERR("Open Update on wrapped counter must return false");
        }
        for (size_t i = 0; i < sizeof(ctx); i++) {
            if (p[i] != 0) {
                ERR("Open counter-wrap poison left nonzero byte at "
                    "offset %zu (=0x%02x)",
                    i, p[i]);
                break;
            }
        }
    }

    TEST("Streaming Final success wipes ctx "
         "(testStreamFinalSuccessWipes)") {
        /* Wipe-proof variant of the streamFinalise success path: pre-
         * fill the ctx with a recognizable 0xA5 pattern BEFORE Init,
         * run a complete Seal/Open round-trip, and assert the ctx is
         * bytewise zero after each Final.  The earlier parity tests
         * relied on Init+Final to produce a zero ctx; this verifies the
         * wipe ran independently of which sub-paths the operation
         * touched.  If dkSecureZero in streamFinalise were elided, the
         * pre-fill 0xA5 bytes in regions that the algorithm never
         * writes (e.g. the poly_buf trailing bytes when ct_total is not
         * a multiple of 16) would survive and this test would catch it. */
        chacha20poly1305StreamCtx ctx;
        const uint8_t *p = (const uint8_t *)&ctx;
        uint8_t key[32];
        memset(key, 0xCC, sizeof(key));
        uint8_t nonce[12];
        memset(nonce, 0xDD, sizeof(nonce));
        const uint8_t aad[5] = {1, 2, 3, 4, 5};
        const uint8_t pt[37] = {0xAB};
        uint8_t ct[37];
        uint8_t recovered[37];
        uint8_t tag[16];

        /* Seal-side success path. */
        memset(&ctx, 0xA5, sizeof(ctx));
        chacha20poly1305SealStreamInit(&ctx, key, nonce);
        if (!chacha20poly1305SealStreamAad(&ctx, aad, sizeof(aad))) {
            ERR("Seal Aad failed");
        }
        if (!chacha20poly1305SealStreamUpdate(&ctx, pt, sizeof(pt), ct)) {
            ERR("Seal Update failed");
        }
        if (!chacha20poly1305SealStreamFinal(&ctx, tag)) {
            ERR("Seal Final failed on legit stream");
        }
        for (size_t i = 0; i < sizeof(ctx); i++) {
            if (p[i] != 0) {
                ERR("Seal Final success left nonzero byte at offset %zu "
                    "(=0x%02x); pre-fill 0xA5 was not scrubbed",
                    i, p[i]);
                break;
            }
        }

        /* Open-side success path (re-fill 0xA5, then open the Seal ct). */
        memset(&ctx, 0xA5, sizeof(ctx));
        chacha20poly1305OpenStreamInit(&ctx, key, nonce);
        if (!chacha20poly1305OpenStreamAad(&ctx, aad, sizeof(aad))) {
            ERR("Open Aad failed");
        }
        if (!chacha20poly1305OpenStreamUpdate(&ctx, ct, sizeof(ct),
                                              recovered)) {
            ERR("Open Update failed");
        }
        if (!chacha20poly1305OpenStreamFinal(&ctx, tag)) {
            ERR("Open Final failed on legit stream");
        }
        if (memcmp(recovered, pt, sizeof(pt)) != 0) {
            ERR("Open recovered != original plaintext");
        }
        for (size_t i = 0; i < sizeof(ctx); i++) {
            if (p[i] != 0) {
                ERR("Open Final success left nonzero byte at offset %zu "
                    "(=0x%02x); pre-fill 0xA5 was not scrubbed",
                    i, p[i]);
                break;
            }
        }

        /* Open-side failure path: tamper the ciphertext, run Final, must
         * return false AND wipe ctx. */
        memset(&ctx, 0xA5, sizeof(ctx));
        ct[0] ^= 0xFFu;
        chacha20poly1305OpenStreamInit(&ctx, key, nonce);
        if (!chacha20poly1305OpenStreamAad(&ctx, aad, sizeof(aad))) {
            ERR("Open Aad (tamper) failed");
        }
        if (!chacha20poly1305OpenStreamUpdate(&ctx, ct, sizeof(ct),
                                              recovered)) {
            ERR("Open Update (tamper) failed");
        }
        if (chacha20poly1305OpenStreamFinal(&ctx, tag)) {
            ERR("Open Final must reject tampered ciphertext");
        }
        for (size_t i = 0; i < sizeof(ctx); i++) {
            if (p[i] != 0) {
                ERR("Open Final tamper-reject left nonzero byte at "
                    "offset %zu (=0x%02x); pre-fill 0xA5 was not scrubbed",
                    i, p[i]);
                break;
            }
        }
    }

    TEST("Performance bench") {
        runBench(64, 200000);
        runBench(1024, 50000);
        runBench(65536, 2000);
        runBench(1048576, 200);
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
