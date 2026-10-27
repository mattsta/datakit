/* X25519 — see x25519.h.
 *
 * Field representation: 5 x 51-bit limbs in uint64_t, little-endian.
 * Reduces to the canonical 32-byte form only at encode/decode time.
 * Multiplication uses the schoolbook 5-limb routine with explicit
 * carry chains; squaring uses the standard doubled-cross-term shortcut.
 * The Montgomery ladder runs in constant time wrt private-key bits via
 * conditional-swap (cswap) implemented as a mask XOR. */

#include "x25519.h"

#include "ctmem.h" /* dkCtMemEq for constant-time small-order pre-reject. */
#include "dkBits.h"

#include <stdint.h>
#include <string.h>

#define LIMB_MASK ((uint64_t)0x7FFFFFFFFFFFFULL) /* 2^51 - 1 */

typedef struct fe25519 {
    uint64_t l[5];
} fe25519;

/* ----- Encode / decode --------------------------------------------------
 *
 * decode: load 32 LE bytes, mask the high bit, repack into 5 x 51-bit
 *         limbs (one trailing carry, no further reduction).
 * encode: full reduction modulo 2^255 - 19, then write 32 LE bytes. */

static void feDecode(fe25519 *out, const uint8_t in[32]) {
    uint64_t t[4];
    for (uint32_t i = 0; i < 4; i++) {
        t[i] = 0;
        for (uint32_t j = 0; j < 8; j++) {
            t[i] |= ((uint64_t)in[i * 8 + j]) << (j * 8);
        }
    }
    /* Strip the high bit of the most-significant byte per RFC 7748
     * §5: the u-coordinate is encoded modulo 2^255. */
    t[3] &= 0x7FFFFFFFFFFFFFFFULL;

    out->l[0] = t[0] & LIMB_MASK;
    out->l[1] = ((t[0] >> 51) | (t[1] << 13)) & LIMB_MASK;
    out->l[2] = ((t[1] >> 38) | (t[2] << 26)) & LIMB_MASK;
    out->l[3] = ((t[2] >> 25) | (t[3] << 39)) & LIMB_MASK;
    out->l[4] = (t[3] >> 12) & LIMB_MASK;
}

static void feCarry(fe25519 *f) {
    uint64_t c;
    c = f->l[0] >> 51;
    f->l[0] &= LIMB_MASK;
    f->l[1] += c;
    c = f->l[1] >> 51;
    f->l[1] &= LIMB_MASK;
    f->l[2] += c;
    c = f->l[2] >> 51;
    f->l[2] &= LIMB_MASK;
    f->l[3] += c;
    c = f->l[3] >> 51;
    f->l[3] &= LIMB_MASK;
    f->l[4] += c;
    c = f->l[4] >> 51;
    f->l[4] &= LIMB_MASK;
    f->l[0] += c * 19u;
    c = f->l[0] >> 51;
    f->l[0] &= LIMB_MASK;
    f->l[1] += c;
}

static void feEncode(uint8_t out[32], const fe25519 *in) {
    fe25519 t = *in;
    feCarry(&t);
    feCarry(&t);
    feCarry(&t);
    /* Conditional subtract 2^255 - 19 if t >= p.  Compute t + 19 and
     * see if it carries past 2^255. */
    fe25519 m;
    m.l[0] = t.l[0] + 19u;
    m.l[1] = t.l[1] + (m.l[0] >> 51);
    m.l[0] &= LIMB_MASK;
    m.l[2] = t.l[2] + (m.l[1] >> 51);
    m.l[1] &= LIMB_MASK;
    m.l[3] = t.l[3] + (m.l[2] >> 51);
    m.l[2] &= LIMB_MASK;
    m.l[4] = t.l[4] + (m.l[3] >> 51);
    m.l[3] &= LIMB_MASK;
    /* If bit 51 of m.l[4] is set, t was >= p; pick m (subtracts p). */
    const uint64_t pick_m = (m.l[4] >> 51) & 1u;
    m.l[4] &= LIMB_MASK;
    const uint64_t mask = (uint64_t)0 - pick_m;
    for (uint32_t i = 0; i < 5; i++) {
        t.l[i] = (t.l[i] & ~mask) | (m.l[i] & mask);
    }
    /* Re-pack into 4 x 64-bit then to bytes. */
    uint64_t w0 = t.l[0] | (t.l[1] << 51);
    uint64_t w1 = (t.l[1] >> 13) | (t.l[2] << 38);
    uint64_t w2 = (t.l[2] >> 26) | (t.l[3] << 25);
    uint64_t w3 = (t.l[3] >> 39) | (t.l[4] << 12);
    uint64_t words[4] = {w0, w1, w2, w3};
    for (uint32_t i = 0; i < 4; i++) {
        for (uint32_t j = 0; j < 8; j++) {
            out[i * 8 + j] = (uint8_t)(words[i] >> (j * 8));
        }
    }
}

/* ----- Field arithmetic ------------------------------------------------- */

static void feAdd(fe25519 *out, const fe25519 *a, const fe25519 *b) {
    for (uint32_t i = 0; i < 5; i++) {
        out->l[i] = a->l[i] + b->l[i];
    }
}

/* Subtract: avoid negatives by adding 2*p first.  4*p per limb keeps
 * everything well below 2^64. */
static void feSub(fe25519 *out, const fe25519 *a, const fe25519 *b) {
    out->l[0] = a->l[0] + 0xFFFFFFFFFFFDAULL - b->l[0];
    out->l[1] = a->l[1] + 0xFFFFFFFFFFFFEULL - b->l[1];
    out->l[2] = a->l[2] + 0xFFFFFFFFFFFFEULL - b->l[2];
    out->l[3] = a->l[3] + 0xFFFFFFFFFFFFEULL - b->l[3];
    out->l[4] = a->l[4] + 0xFFFFFFFFFFFFEULL - b->l[4];
}

/* 5x5 schoolbook mul with 19* fold for limbs beyond 4.  Uses
 * __uint128_t — available on every supported compiler. */
static void feMul(fe25519 *out, const fe25519 *a, const fe25519 *b) {
    __uint128_t r0, r1, r2, r3, r4;
    const uint64_t a0 = a->l[0], a1 = a->l[1], a2 = a->l[2], a3 = a->l[3],
                   a4 = a->l[4];
    const uint64_t b0 = b->l[0], b1 = b->l[1], b2 = b->l[2], b3 = b->l[3],
                   b4 = b->l[4];
    const uint64_t b1_19 = b1 * 19u;
    const uint64_t b2_19 = b2 * 19u;
    const uint64_t b3_19 = b3 * 19u;
    const uint64_t b4_19 = b4 * 19u;

    r0 = (__uint128_t)a0 * b0 + (__uint128_t)a1 * b4_19 +
         (__uint128_t)a2 * b3_19 + (__uint128_t)a3 * b2_19 +
         (__uint128_t)a4 * b1_19;
    r1 = (__uint128_t)a0 * b1 + (__uint128_t)a1 * b0 + (__uint128_t)a2 * b4_19 +
         (__uint128_t)a3 * b3_19 + (__uint128_t)a4 * b2_19;
    r2 = (__uint128_t)a0 * b2 + (__uint128_t)a1 * b1 + (__uint128_t)a2 * b0 +
         (__uint128_t)a3 * b4_19 + (__uint128_t)a4 * b3_19;
    r3 = (__uint128_t)a0 * b3 + (__uint128_t)a1 * b2 + (__uint128_t)a2 * b1 +
         (__uint128_t)a3 * b0 + (__uint128_t)a4 * b4_19;
    r4 = (__uint128_t)a0 * b4 + (__uint128_t)a1 * b3 + (__uint128_t)a2 * b2 +
         (__uint128_t)a3 * b1 + (__uint128_t)a4 * b0;

    /* Two carry passes to fit back into 51-bit limbs. */
    uint64_t c;
    c = (uint64_t)(r0 >> 51);
    out->l[0] = (uint64_t)r0 & LIMB_MASK;
    r1 += c;
    c = (uint64_t)(r1 >> 51);
    out->l[1] = (uint64_t)r1 & LIMB_MASK;
    r2 += c;
    c = (uint64_t)(r2 >> 51);
    out->l[2] = (uint64_t)r2 & LIMB_MASK;
    r3 += c;
    c = (uint64_t)(r3 >> 51);
    out->l[3] = (uint64_t)r3 & LIMB_MASK;
    r4 += c;
    c = (uint64_t)(r4 >> 51);
    out->l[4] = (uint64_t)r4 & LIMB_MASK;
    out->l[0] += c * 19u;
    c = out->l[0] >> 51;
    out->l[0] &= LIMB_MASK;
    out->l[1] += c;
}

static void feSquare(fe25519 *out, const fe25519 *a) {
    feMul(out, a, a);
}

/* Multiply by a small (< 2^16) scalar. */
static void feMul121665(fe25519 *out, const fe25519 *a) {
    __uint128_t r0 = (__uint128_t)a->l[0] * 121665u;
    __uint128_t r1 = (__uint128_t)a->l[1] * 121665u;
    __uint128_t r2 = (__uint128_t)a->l[2] * 121665u;
    __uint128_t r3 = (__uint128_t)a->l[3] * 121665u;
    __uint128_t r4 = (__uint128_t)a->l[4] * 121665u;
    uint64_t c;
    c = (uint64_t)(r0 >> 51);
    out->l[0] = (uint64_t)r0 & LIMB_MASK;
    r1 += c;
    c = (uint64_t)(r1 >> 51);
    out->l[1] = (uint64_t)r1 & LIMB_MASK;
    r2 += c;
    c = (uint64_t)(r2 >> 51);
    out->l[2] = (uint64_t)r2 & LIMB_MASK;
    r3 += c;
    c = (uint64_t)(r3 >> 51);
    out->l[3] = (uint64_t)r3 & LIMB_MASK;
    r4 += c;
    c = (uint64_t)(r4 >> 51);
    out->l[4] = (uint64_t)r4 & LIMB_MASK;
    out->l[0] += c * 19u;
}

/* Constant-time conditional swap of a and b iff `c` is 1; c MUST be 0
 * or 1. */
static void feCswap(fe25519 *a, fe25519 *b, uint64_t c) {
    const uint64_t mask = (uint64_t)0 - c;
    for (uint32_t i = 0; i < 5; i++) {
        const uint64_t t = (a->l[i] ^ b->l[i]) & mask;
        a->l[i] ^= t;
        b->l[i] ^= t;
    }
}

/* Inversion via Fermat's little theorem: a^(p-2) where p = 2^255 - 19,
 * so the exponent is 2^255 - 21.  Pruned addition chain identical to
 * the djb reference. */
static void feInvert(fe25519 *out, const fe25519 *z) {
    fe25519 z2, z9, z11, z2_5_0, z2_10_0, z2_20_0, z2_50_0, z2_100_0, t;
    feSquare(&z2, z); /* 2 */
    feSquare(&t, &z2);
    feSquare(&t, &t);        /* 8 */
    feMul(&z9, &t, z);       /* 9 */
    feMul(&z11, &z9, &z2);   /* 11 */
    feSquare(&t, &z11);      /* 22 */
    feMul(&z2_5_0, &t, &z9); /* 2^5 - 2^0 = 31 */

    feSquare(&t, &z2_5_0);
    for (uint32_t i = 1; i < 5; i++) {
        feSquare(&t, &t);
    }
    feMul(&z2_10_0, &t, &z2_5_0); /* 2^10 - 2^0 */

    feSquare(&t, &z2_10_0);
    for (uint32_t i = 1; i < 10; i++) {
        feSquare(&t, &t);
    }
    feMul(&z2_20_0, &t, &z2_10_0); /* 2^20 - 2^0 */

    feSquare(&t, &z2_20_0);
    for (uint32_t i = 1; i < 20; i++) {
        feSquare(&t, &t);
    }
    feMul(&t, &t, &z2_20_0); /* 2^40 - 2^0 */

    for (uint32_t i = 0; i < 10; i++) {
        feSquare(&t, &t);
    }
    feMul(&z2_50_0, &t, &z2_10_0); /* 2^50 - 2^0 */

    feSquare(&t, &z2_50_0);
    for (uint32_t i = 1; i < 50; i++) {
        feSquare(&t, &t);
    }
    feMul(&z2_100_0, &t, &z2_50_0); /* 2^100 - 2^0 */

    feSquare(&t, &z2_100_0);
    for (uint32_t i = 1; i < 100; i++) {
        feSquare(&t, &t);
    }
    feMul(&t, &t, &z2_100_0); /* 2^200 - 2^0 */

    feSquare(&t, &t);
    for (uint32_t i = 1; i < 50; i++) {
        feSquare(&t, &t);
    }
    feMul(&t, &t, &z2_50_0); /* 2^250 - 2^0 */

    for (uint32_t i = 0; i < 5; i++) {
        feSquare(&t, &t);
    }
    feMul(out, &t, &z11); /* 2^255 - 21 */
}

/* ----- Montgomery ladder ----------------------------------------------- */

static void clampScalar(uint8_t s[32]) {
    s[0] &= 0xF8;
    s[31] &= 0x7F;
    s[31] |= 0x40;
}

static void x25519ScalarMult(uint8_t out[32], const uint8_t scalar[32],
                             const uint8_t u_in[32]) {
    uint8_t k[32];
    memcpy(k, scalar, 32);
    clampScalar(k);

    fe25519 x1;
    feDecode(&x1, u_in);

    fe25519 x2 = {{1, 0, 0, 0, 0}}; /* identity */
    fe25519 z2 = {{0, 0, 0, 0, 0}};
    fe25519 x3 = x1;
    fe25519 z3 = {{1, 0, 0, 0, 0}};
    uint64_t swap = 0;

    /* Loop-scope intermediates: declared here so the function epilogue
     * can wipe them after the ladder completes. Several (notably
     * A, AA, B, BB, DA, CB, E) are derived from the secret-scalar-
     * conditioned ladder state, so we treat all of them as secret. */
    fe25519 A, AA, B, BB, E, C, D, DA, CB, t1, t2;

    for (int32_t pos = 254; pos >= 0; pos--) {
        const uint64_t bit = dkBitGet(k, (size_t)pos);
        swap ^= bit;
        feCswap(&x2, &x3, swap);
        feCswap(&z2, &z3, swap);
        swap = bit;

        feAdd(&A, &x2, &z2);
        feSquare(&AA, &A);
        feSub(&B, &x2, &z2);
        feSquare(&BB, &B);
        feSub(&E, &AA, &BB);
        feAdd(&C, &x3, &z3);
        feSub(&D, &x3, &z3);
        feMul(&DA, &D, &A);
        feMul(&CB, &C, &B);
        feAdd(&t1, &DA, &CB);
        feSquare(&x3, &t1);
        feSub(&t2, &DA, &CB);
        feSquare(&t1, &t2);
        feMul(&z3, &t1, &x1);
        feMul(&x2, &AA, &BB);
        feMul121665(&t1, &E);
        feAdd(&t1, &t1, &AA);
        feMul(&z2, &E, &t1);
    }
    feCswap(&x2, &x3, swap);
    feCswap(&z2, &z3, swap);

    fe25519 zinv, result;
    feInvert(&zinv, &z2);
    feMul(&result, &x2, &zinv);
    feEncode(out, &result);

    /* Defense-in-depth: wipe every secret-derived stack-resident value
     * before frame teardown.  memset on a soon-to-be-dead buffer can be
     * elided under -O3 + LTO; dkSecureZero defeats that. The scalar
     * `k` is the primary secret; ladder intermediates derive from it
     * via the cswapped (x2/z2, x3/z3) pair, so a stack-leaking
     * primitive could otherwise reconstruct bit information from the
     * post-ladder state.  `swap` is the most recently sampled scalar
     * bit; explicitly clear it too. */
    dkSecureZero(k, sizeof(k));
    dkSecureZero(&x1, sizeof(x1));
    dkSecureZero(&x2, sizeof(x2));
    dkSecureZero(&z2, sizeof(z2));
    dkSecureZero(&x3, sizeof(x3));
    dkSecureZero(&z3, sizeof(z3));
    dkSecureZero(&A, sizeof(A));
    dkSecureZero(&AA, sizeof(AA));
    dkSecureZero(&B, sizeof(B));
    dkSecureZero(&BB, sizeof(BB));
    dkSecureZero(&E, sizeof(E));
    dkSecureZero(&C, sizeof(C));
    dkSecureZero(&D, sizeof(D));
    dkSecureZero(&DA, sizeof(DA));
    dkSecureZero(&CB, sizeof(CB));
    dkSecureZero(&t1, sizeof(t1));
    dkSecureZero(&t2, sizeof(t2));
    dkSecureZero(&zinv, sizeof(zinv));
    dkSecureZero(&result, sizeof(result));
    dkSecureZero(&swap, sizeof(swap));
}

void x25519Base(uint8_t out_public[32], const uint8_t priv[32]) {
    static constexpr uint8_t base[32] = {9, 0};
    x25519ScalarMult(out_public, priv, base);
}

/* Canonical small-order u-coordinate blacklist for Curve25519.
 *
 * RFC 7748 §6.1 documents that scalar multiplication by a small-order
 * peer key yields the all-zero shared secret and that implementations
 * MAY (and SHOULD, defense-in-depth) pre-reject those peer keys before
 * running the ladder.  The list below enumerates the 12 distinct
 * 32-byte little-endian serializations that decode to a small-order
 * u-coordinate on Curve25519.  Sources:
 *   - RFC 7748 §6.1
 *   - "May the Fourth Be With You: A Microarchitectural Side Channel
 *      Attack on Several Real-World Applications of Curve25519" —
 *      Genkin/Valenta/Yarom (CCS '17), Appendix A
 *   - libsodium crypto_scalarmult/curve25519/ref10/x25519_ref10.c
 *      (`has_small_order` blacklist, 7 unique post-mask entries)
 *
 * Entries 0..6 are the 7 unique u-coordinates of small order on the
 * curve and its quadratic twist:
 *   [0] 0                              (order-4 point at infinity image)
 *   [1] 1                              (order-1, fixed point of doubling)
 *   [2] 32560625091655743179... (0xe0..0x00)  (order-8 generator on twist)
 *   [3] 39382357235489614581... (0x5f..0x57)  (order-8 generator on curve)
 *   [4] p - 1                          (order-2)
 *   [5] p                              (order-4)
 *   [6] p + 1                          (order-1)
 *
 * Entries 7..11 are the 5 high-bit-set serialization "twins" of
 * entries 2..6.  These decode to the same u-coordinate after the
 * mandatory RFC 7748 §5 high-bit mask, but they are distinct
 * attacker-supplied byte patterns and we reject them pre-mask so the
 * attacker cannot disguise a small-order key by toggling bit 255.
 * (Entries 0 and 1 with bit 255 set decode to entries with the same
 * post-mask value; we do not include those twins because that would
 * also reject legitimate keys whose high bit happens to be set —
 * those are vanishingly rare random points, but for symmetry we
 * exclude only the trivially small ones.)
 *
 * The pre-reject is a constant-time OR-reduce of `dkCtMemEq` results
 * across all 12 entries.  The post-multiply zero check remains as a
 * defense-in-depth backstop. */
static constexpr int X25519_SMALL_ORDER_COUNT = 12;
static constexpr uint8_t x25519SmallOrderTable[X25519_SMALL_ORDER_COUNT][32] = {
    /* [0] 0 */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    /* [1] 1 */
    {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    /* [2]
       325606250916557431795983626356110631294008115727848805560023387167927233504
     */
    {0xe0, 0xeb, 0x7a, 0x7c, 0x3b, 0x41, 0xb8, 0xae, 0x16, 0x56, 0xe3,
     0xfa, 0xf1, 0x9f, 0xc4, 0x6a, 0xda, 0x09, 0x8d, 0xeb, 0x9c, 0x32,
     0xb1, 0xfd, 0x86, 0x62, 0x05, 0x16, 0x5f, 0x49, 0xb8, 0x00},
    /* [3]
       39382357235489614581723060781553021112529911719440698176882885853963445705823
     */
    {0x5f, 0x9c, 0x95, 0xbc, 0xa3, 0x50, 0x8c, 0x24, 0xb1, 0xd0, 0xb1,
     0x55, 0x9c, 0x83, 0xef, 0x5b, 0x04, 0x44, 0x5c, 0xc4, 0x58, 0x1c,
     0x8e, 0x86, 0xd8, 0x22, 0x4e, 0xdd, 0xd0, 0x9f, 0x11, 0x57},
    /* [4] p - 1 = 2^255 - 20 */
    {0xec, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
    /* [5] p = 2^255 - 19 */
    {0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
    /* [6] p + 1 = 2^255 - 18 */
    {0xee, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
    /* [7] entry [2] with bit 255 set (decodes to same u after mask) */
    {0xe0, 0xeb, 0x7a, 0x7c, 0x3b, 0x41, 0xb8, 0xae, 0x16, 0x56, 0xe3,
     0xfa, 0xf1, 0x9f, 0xc4, 0x6a, 0xda, 0x09, 0x8d, 0xeb, 0x9c, 0x32,
     0xb1, 0xfd, 0x86, 0x62, 0x05, 0x16, 0x5f, 0x49, 0xb8, 0x80},
    /* [8] entry [3] with bit 255 set */
    {0x5f, 0x9c, 0x95, 0xbc, 0xa3, 0x50, 0x8c, 0x24, 0xb1, 0xd0, 0xb1,
     0x55, 0x9c, 0x83, 0xef, 0x5b, 0x04, 0x44, 0x5c, 0xc4, 0x58, 0x1c,
     0x8e, 0x86, 0xd8, 0x22, 0x4e, 0xdd, 0xd0, 0x9f, 0x11, 0xd7},
    /* [9] entry [4] with bit 255 set */
    {0xec, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
    /* [10] entry [5] with bit 255 set */
    {0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
    /* [11] entry [6] with bit 255 set */
    {0xee, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
};

/* Constant-time pre-reject: returns true iff `peer` matches any entry
 * in the small-order table.  Implemented as an OR-reduction so the
 * comparison time is independent of which entry (if any) matched. */
static bool x25519IsSmallOrder(const uint8_t peer[32]) {
    uint8_t acc = 0;
    for (uint32_t i = 0; i < X25519_SMALL_ORDER_COUNT; i++) {
        /* Cast bool to uint8_t and OR into accumulator — no early exit. */
        acc = (uint8_t)(acc |
                        (uint8_t)dkCtMemEq(peer, x25519SmallOrderTable[i], 32));
    }
    return acc != 0;
}

bool x25519(uint8_t out_shared[32], const uint8_t priv[32],
            const uint8_t peer_public[32]) {
    /* RFC 7748 §6.1 pre-reject: bail before the ladder if the caller
     * handed us a documented small-order u-coordinate.  Stronger than
     * the post-multiply zero check alone because (a) we avoid running
     * the ladder under attacker-chosen inputs, and (b) we catch the
     * full canonical list rather than only the cases that happen to
     * produce a zero output. */
    if (x25519IsSmallOrder(peer_public)) {
        /* Zero the output so callers that ignore the return value still
         * see a clearly-distinct sentinel.  Constant-time (memset of a
         * fixed 32 bytes; not gated on any secret). */
        memset(out_shared, 0, 32);
        return false;
    }
    x25519ScalarMult(out_shared, priv, peer_public);
    /* Defense-in-depth: even after the pre-reject above, keep the
     * post-multiply zero check.  RFC 7748 §6.1 — the all-zero output
     * indicates the contributory-pair invariant was broken, i.e. peer
     * fed us a low-order point that wasn't on our blacklist.
     * Constant-time OR-reduce the 32 bytes to one byte. */
    uint8_t acc = 0;
    for (uint32_t i = 0; i < 32; i++) {
        acc |= out_shared[i];
    }
    return acc != 0;
}

/* ============================================================================
 * Test driver
 * ============================================================================
 */

#ifdef DATAKIT_TEST

#include "cleaner.h"
#include "ctest.h"
#include "datakit.h" /* zmalloc / zfree */
#include "str.h"     /* StrHexToBytes, StrBytesToHex */

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleanerName(uint8_t *, zfree, U8);

int x25519Test(int, char *[]) {
    int err = 0;

    /* RFC 7748 §5.2 test vectors. */
    TEST("RFC 7748 §5.2: scalar multiplication, vector 1") {
        uint8_t scalar[32], u[32], expected[32], got[32];
        TEST_HEX(
            err,
            "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
            scalar, 32);
        TEST_HEX(
            err,
            "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
            u, 32);
        TEST_HEX(
            err,
            "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552",
            expected, 32);
        x25519(got, scalar, u);
        if (memcmp(got, expected, 32) != 0) {
            char gh[65], eh[65];
            StrBytesToHex(got, 32, gh);
            StrBytesToHex(expected, 32, eh);
            ERR("vec1: got %s\nexp %s", gh, eh);
        }
    }

    TEST("RFC 7748 §5.2: scalar multiplication, vector 2") {
        uint8_t scalar[32], u[32], expected[32], got[32];
        TEST_HEX(
            err,
            "4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d",
            scalar, 32);
        TEST_HEX(
            err,
            "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493",
            u, 32);
        TEST_HEX(
            err,
            "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957",
            expected, 32);
        x25519(got, scalar, u);
        if (memcmp(got, expected, 32) != 0) {
            char gh[65], eh[65];
            StrBytesToHex(got, 32, gh);
            StrBytesToHex(expected, 32, eh);
            ERR("vec2: got %s\nexp %s", gh, eh);
        }
    }

    TEST("RFC 7748 §6.1: full ECDH round-trip with the canonical keypair") {
        /* Alice and Bob from §6.1. */
        uint8_t alice_priv[32], bob_priv[32];
        uint8_t alice_pub[32], bob_pub[32];
        uint8_t alice_pub_expect[32], bob_pub_expect[32];
        uint8_t shared_expect[32];
        uint8_t shared_a[32], shared_b[32];

        TEST_HEX(
            err,
            "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a",
            alice_priv, 32);
        TEST_HEX(
            err,
            "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a",
            alice_pub_expect, 32);
        TEST_HEX(
            err,
            "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb",
            bob_priv, 32);
        TEST_HEX(
            err,
            "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f",
            bob_pub_expect, 32);
        TEST_HEX(
            err,
            "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742",
            shared_expect, 32);

        x25519Base(alice_pub, alice_priv);
        x25519Base(bob_pub, bob_priv);
        TEST_BYTES(err, alice_pub, alice_pub_expect, 32,
                   "Alice's derived public key does not match §6.1");
        TEST_BYTES(err, bob_pub, bob_pub_expect, 32,
                   "Bob's derived public key does not match §6.1");

        if (!x25519(shared_a, alice_priv, bob_pub)) {
            ERR("Alice's ECDH should not flag low-order");
        }
        if (!x25519(shared_b, bob_priv, alice_pub)) {
            ERR("Bob's ECDH should not flag low-order");
        }
        TEST_BYTES(err, shared_a, shared_b, 32,
                   "Alice and Bob computed different shared secrets");
        TEST_BYTES(err, shared_a, shared_expect, 32,
                   "Shared secret does not match §6.1");
    }

    TEST("low-order point: x25519 returns false") {
        /* The all-zero public key is a low-order point; ECDH with it
         * produces a zero shared output and the API surfaces that
         * as false (RFC 7748 §6.1). */
        uint8_t priv[32];
        for (uint32_t i = 0; i < 32; i++) {
            priv[i] = (uint8_t)i;
        }
        uint8_t peer_zero[32] = {};
        uint8_t out[32];
        if (x25519(out, priv, peer_zero)) {
            ERR("Low-order peer key must surface as false");
        }
    }

    TEST("public-key derivation is deterministic") {
        uint8_t priv[32];
        for (uint32_t i = 0; i < 32; i++) {
            priv[i] = (uint8_t)(i * 7u);
        }
        uint8_t pub1[32], pub2[32];
        x25519Base(pub1, priv);
        x25519Base(pub2, priv);
        TEST_BYTES(err, pub1, pub2, 32,
                   "Base derivation must be deterministic");
    }

    TEST("ECDH agreement on random keypairs") {
        uint64_t rng = 0xDEADBEEFCAFEBABEULL;
        for (uint32_t t = 0; t < 32; t++) {
            uint8_t a_priv[32], b_priv[32];
            for (uint32_t i = 0; i < 32; i++) {
                dkLcg64Next(&rng);
                a_priv[i] = (uint8_t)(rng >> 56);
            }
            for (uint32_t i = 0; i < 32; i++) {
                dkLcg64Next(&rng);
                b_priv[i] = (uint8_t)(rng >> 56);
            }
            uint8_t a_pub[32], b_pub[32];
            x25519Base(a_pub, a_priv);
            x25519Base(b_pub, b_priv);
            uint8_t s_a[32], s_b[32];
            bool ok_a = x25519(s_a, a_priv, b_pub);
            bool ok_b = x25519(s_b, b_priv, a_pub);
            if (!ok_a || !ok_b) {
                ERR("trial %u: x25519 surfaced low-order falsely", t);
            }
            TEST_BYTES(err, s_a, s_b, 32, "trial %u: shared secrets diverge",
                       t);
        }
    }

    /* Every entry in the canonical small-order table must be
     * pre-rejected at entry to x25519().  Also verify the output
     * buffer is zeroed on rejection so callers that ignore the
     * return value see a sentinel rather than uninitialized memory. */
    TEST("canonical small-order list pre-reject") {
        uint8_t priv[32];
        for (uint32_t i = 0; i < 32; i++) {
            priv[i] = (uint8_t)(0x40u | (i & 0x1Fu));
        }
        for (uint32_t e = 0; e < X25519_SMALL_ORDER_COUNT; e++) {
            uint8_t out[32];
            memset(out, 0xCC, sizeof(out)); /* poison */
            if (x25519(out, priv, x25519SmallOrderTable[e])) {
                ERR("entry %u: small-order peer must reject", e);
            }
            for (uint32_t i = 0; i < 32; i++) {
                if (out[i] != 0) {
                    ERR("entry %u: out[%u] = 0x%02x; reject must zero output",
                        e, i, out[i]);
                    break;
                }
            }
        }
    }

    /* 100 random peers should never trigger the pre-reject (the
     * blacklist is 12 entries out of 2^256 possible byte strings — a
     * random hit is statistically impossible). */
    TEST("random peers do not false-positive") {
        uint64_t rng = 0xA9CAFEBABEDEADBELL;
        uint8_t priv[32];
        for (uint32_t i = 0; i < 32; i++) {
            priv[i] = (uint8_t)(i ^ 0x55u);
        }
        for (uint32_t t = 0; t < 100; t++) {
            uint8_t peer[32];
            for (uint32_t i = 0; i < 32; i++) {
                dkLcg64Next(&rng);
                peer[i] = (uint8_t)(rng >> 56);
            }
            uint8_t out[32];
            if (!x25519(out, priv, peer)) {
                ERR("trial %u: random peer pre-rejected (false positive)", t);
            }
        }
    }

    /* Correctness-parity check that the ladder's stack-wipe epilogue
     * did not perturb output.  Stack residue itself is not observable
     * from outside the function frame; instead we assert that adding
     * dkSecureZero over the scalar + intermediate `fe25519` locals
     * preserved bit-exact agreement between random keypairs across
     * many trials.  If the wipe accidentally clobbered a live value
     * before the result was encoded, ECDH agreement would fail here. */
    TEST("testX25519WipesScalar: ECDH stays bit-exact after wipe epilogue") {
        uint64_t rng = 0x1234ABCD5678EF01ULL;
        for (uint32_t t = 0; t < 256u; t++) {
            uint8_t a_priv[32], b_priv[32];
            for (uint32_t i = 0; i < 32; i++) {
                dkLcg64Next(&rng);
                a_priv[i] = (uint8_t)(rng >> 56);
            }
            for (uint32_t i = 0; i < 32; i++) {
                dkLcg64Next(&rng);
                b_priv[i] = (uint8_t)(rng >> 56);
            }
            uint8_t a_pub[32], b_pub[32];
            x25519Base(a_pub, a_priv);
            x25519Base(b_pub, b_priv);
            uint8_t s_a[32], s_b[32];
            if (!x25519(s_a, a_priv, b_pub) || !x25519(s_b, b_priv, a_pub)) {
                ERR("trial %u: x25519 returned false on random keypair", t);
                continue;
            }
            TEST_BYTES(err, s_a, s_b, 32,
                       "trial %u: shared secrets diverge after wipe epilogue",
                       t);
        }
    }

    TEST("testX25519WipeProof: dkSecureZero scrubs scalar + fe25519 "
         "pattern (mirror of ladder epilogue)") {
        /* Limitation: x25519ScalarMult's `uint8_t k[32]` scalar copy and
         * its `fe25519` ladder intermediates (x1..t2, zinv, result,
         * swap) live on the function's stack frame.  After
         * x25519ScalarMult returns, the frame is released and those
         * bytes are unreachable from outside — no caller can directly
         * observe whether dkSecureZero actually ran.
         *
         * Workaround: mirror the production wipe pattern on a heap-
         * allocated scalar + fe25519 array, pre-fill with the
         * recognizable 0xA5 byte, apply dkSecureZero, and assert the
         * full region is zero.  This pins the abstraction the ladder
         * epilogue relies on (and would catch a regression where
         * dkSecureZero degenerates to a no-op or memset that LTO can
         * elide).  It does NOT prove the production code actually
         * invokes dkSecureZero for each local — that property is
         * enforced by code review of the ladder epilogue.
         *
         * The production wipe scope (per x25519.c:292-311) is: scalar
         * k[32], plus ladder intermediates as fe25519 (each = 5 limbs
         * of uint64_t).  We exercise dkSecureZero over both shapes. */
        cleanerName(zfree, U8) uint8_t *scalar = (uint8_t *)zmalloc(32);
        memset(scalar, 0xA5, 32);
        dkSecureZero(scalar, 32);
        for (size_t i = 0; i < 32; i++) {
            if (scalar[i] != 0) {
                ERR("scalar dkSecureZero left nonzero byte at offset %zu "
                    "(=0x%02x)",
                    i, scalar[i]);
                break;
            }
        }

        /* Mirror an fe25519 limb array (5 uint64_t).  The actual fe25519
         * is a stack-local struct in the ladder; the test buffer below
         * matches its byte size and exercises the same dkSecureZero
         * call shape used in the epilogue. */
        const size_t fe_bytes = 5u * sizeof(uint64_t);
        cleanerName(zfree, U8) uint8_t *fe = (uint8_t *)zmalloc(fe_bytes);
        memset(fe, 0xA5, fe_bytes);
        dkSecureZero(fe, fe_bytes);
        for (size_t i = 0; i < fe_bytes; i++) {
            if (fe[i] != 0) {
                ERR("fe25519 dkSecureZero left nonzero byte at offset %zu "
                    "(=0x%02x)",
                    i, fe[i]);
                break;
            }
        }
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
