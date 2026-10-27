#pragma once

/* Poly1305's arithmetic in five 26-bit limbs: a 16-byte block as limbs,
 * and one multiply mod 2^130 - 5.  The scalar MAC absorbs every block
 * through both; the vector backends load their blocks and precompute
 * their powers of r with the same two, so all paths agree bit for bit. */

#include "../dkEndian.h" /* dkLoadLe32 */

#include <stddef.h>
#include <stdint.h>

static constexpr uint32_t POLY1305_LIMB_MASK = 0x3ffffff;

/* The 2^128 marker as it lands in the top limb: a full block adds it,
 * a padded last block (which carries its own 0x01 byte) does not */
static constexpr uint32_t POLY1305_HIBIT = UINT32_C(1) << 24;

/* The 16 bytes at 'm' (little-endian) as five 26-bit limbs, with 'hibit'
 * added to the top one */
static inline void poly1305LimbsLoad(const uint8_t m[16], uint32_t hibit,
                                     uint64_t out[5]) {
    const uint32_t t0 = dkLoadLe32(m + 0);
    const uint32_t t1 = dkLoadLe32(m + 4);
    const uint32_t t2 = dkLoadLe32(m + 8);
    const uint32_t t3 = dkLoadLe32(m + 12);
    out[0] = t0 & POLY1305_LIMB_MASK;
    out[1] = ((t0 >> 26) | (t1 << 6)) & POLY1305_LIMB_MASK;
    out[2] = ((t1 >> 20) | (t2 << 12)) & POLY1305_LIMB_MASK;
    out[3] = ((t2 >> 14) | (t3 << 18)) & POLY1305_LIMB_MASK;
    out[4] = (t3 >> 8) + hibit;
}

/* a * b mod 2^130 - 5, partially reduced: every limb of the result is
 * below 2^26 except limb 1, below 2^26 + 2^6.  'a' may be an
 * accumulator plus a block (limbs below 2^27) and 'b' a clamped r or a
 * power of one (limbs below 2^27): a column is five products each below
 * 2^27 * 5 * 2^27 < 2^57, so its sum stays below 2^60 and no 64-bit
 * sum overflows.  The wrap-around terms use 5 * b_i because 2^130 is
 * 5 mod p. */
static inline void poly1305LimbsMulMod(const uint64_t a[5], const uint32_t b[5],
                                       uint64_t out[5]) {
    const uint64_t b0 = b[0], b1 = b[1], b2 = b[2], b3 = b[3], b4 = b[4];
    const uint64_t s1 = b1 * 5, s2 = b2 * 5, s3 = b3 * 5, s4 = b4 * 5;

    uint64_t d0 = a[0] * b0 + a[1] * s4 + a[2] * s3 + a[3] * s2 + a[4] * s1;
    uint64_t d1 = a[0] * b1 + a[1] * b0 + a[2] * s4 + a[3] * s3 + a[4] * s2;
    uint64_t d2 = a[0] * b2 + a[1] * b1 + a[2] * b0 + a[3] * s4 + a[4] * s3;
    uint64_t d3 = a[0] * b3 + a[1] * b2 + a[2] * b1 + a[3] * b0 + a[4] * s4;
    uint64_t d4 = a[0] * b4 + a[1] * b3 + a[2] * b2 + a[3] * b1 + a[4] * b0;

    /* Carry up the limbs; the top limb's overflow folds back as * 5 */
    d1 += d0 >> 26;
    d2 += d1 >> 26;
    d3 += d2 >> 26;
    d4 += d3 >> 26;
    uint64_t h0 = (d0 & POLY1305_LIMB_MASK) + (d4 >> 26) * 5;
    out[1] = (d1 & POLY1305_LIMB_MASK) + (h0 >> 26);
    out[0] = h0 & POLY1305_LIMB_MASK;
    out[2] = d2 & POLY1305_LIMB_MASK;
    out[3] = d3 & POLY1305_LIMB_MASK;
    out[4] = d4 & POLY1305_LIMB_MASK;
}

/* poly1305LimbsMulMod for 32-bit limb arrays: the vector backends'
 * powers of r (r^2, r^3, r^4), each below 2^26 + 2^6 per limb, which
 * fits 32 bits */
static inline void poly1305LimbsMulMod32(const uint32_t a[5],
                                         const uint32_t b[5], uint32_t out[5]) {
    const uint64_t wide[5] = {a[0], a[1], a[2], a[3], a[4]};
    uint64_t product[5];
    poly1305LimbsMulMod(wide, b, product);
    for (size_t i = 0; i < 5; i++) {
        out[i] = (uint32_t)product[i];
    }
}
