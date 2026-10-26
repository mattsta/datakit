/* SPDX-License-Identifier: BSD-2-Clause */
/* ctmem.h — constant-time memory primitives + secure-zero helper.
 *
 * The compiler is free to rewrite straightforward byte loops into early-exit
 * branches when it can prove a difference. We defeat that by:
 *   1. Funneling the running diff through a volatile slot (compiler must
 *      assume side effects on every store);
 *   2. Routing the diff through an inline-asm barrier with "+r" constraint
 *      and a "memory" clobber so the compiler treats the value as opaque
 *      after the loop.
 *
 * `dkSecureZero` is the DCE-resistant memset(0) for key material: a volatile
 * byte loop followed by an asm "memory" barrier. Use it instead of `memset`
 * any time the buffer being cleared holds a secret that the next statement
 * is about to free / let go out of scope; vanilla memset on such a buffer is
 * legal for the optimizer to elide entirely under -O3+LTO.
 *
 * Usage: include in any module that compares MAC tags or zeroes secrets.
 * Header-only and self-contained on purpose: a stray link-time dependency
 * here would defeat the value of inlining for the 16-byte common case.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Opaque the value so the compiler cannot reason about it after the
 * barrier. On non-GNU compilers we fall back to volatile-only semantics
 * (still correct, just slightly weaker against an aggressive PGO pass). */
static inline uint8_t dkCtBarrierU8(uint8_t v) {
#if defined(__GNUC__) || defined(__clang__)
    __asm__ volatile("" : "+r"(v) : : "memory");
#endif
    return v;
}

/* Return true iff a and b match over n bytes. Constant in n for any pair
 * of inputs of equal length. */
static inline bool dkCtMemEq(const uint8_t *a, const uint8_t *b, size_t n) {
    const volatile uint8_t *va = (const volatile uint8_t *)a;
    const volatile uint8_t *vb = (const volatile uint8_t *)b;
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff = (uint8_t)(diff | (uint8_t)(va[i] ^ vb[i]));
    }
    return dkCtBarrierU8(diff) == 0;
}

/* Specialised 16-byte tag compare. Unrolled so the compiler can issue
 * independent loads; still constant-time because every byte contributes. */
static inline bool dkCtMemEq16(const uint8_t *a, const uint8_t *b) {
    const volatile uint8_t *va = (const volatile uint8_t *)a;
    const volatile uint8_t *vb = (const volatile uint8_t *)b;
    uint8_t d0 = (uint8_t)(va[0] ^ vb[0]);
    uint8_t d1 = (uint8_t)(va[1] ^ vb[1]);
    uint8_t d2 = (uint8_t)(va[2] ^ vb[2]);
    uint8_t d3 = (uint8_t)(va[3] ^ vb[3]);
    uint8_t d4 = (uint8_t)(va[4] ^ vb[4]);
    uint8_t d5 = (uint8_t)(va[5] ^ vb[5]);
    uint8_t d6 = (uint8_t)(va[6] ^ vb[6]);
    uint8_t d7 = (uint8_t)(va[7] ^ vb[7]);
    uint8_t d8 = (uint8_t)(va[8] ^ vb[8]);
    uint8_t d9 = (uint8_t)(va[9] ^ vb[9]);
    uint8_t da = (uint8_t)(va[10] ^ vb[10]);
    uint8_t db = (uint8_t)(va[11] ^ vb[11]);
    uint8_t dc = (uint8_t)(va[12] ^ vb[12]);
    uint8_t dd = (uint8_t)(va[13] ^ vb[13]);
    uint8_t de = (uint8_t)(va[14] ^ vb[14]);
    uint8_t df = (uint8_t)(va[15] ^ vb[15]);
    uint8_t diff = (uint8_t)(d0 | d1 | d2 | d3 | d4 | d5 | d6 | d7 | d8 | d9 |
                             da | db | dc | dd | de | df);
    return dkCtBarrierU8(diff) == 0;
}

/* Zero `n` bytes at `p` such that the compiler cannot eliminate the writes
 * even if `p` is about to be freed or go out of scope. Use this for key
 * material, AEAD nonce-derivation buffers, MAC tags, and any other secret
 * intermediate state. For plain initialization of a fresh struct (no secret
 * has been written into it yet) ordinary `memset` is fine. */
static inline void dkSecureZero(void *p, size_t n) {
    if (p == NULL || n == 0) {
        return;
    }
#if defined(DK_HAVE_MEMSET_EXPLICIT)
    memset_explicit(p, 0, n);
#elif defined(DK_HAVE_MEMSET_S)
    memset_s(p, n, 0, n);
#elif defined(DK_HAVE_EXPLICIT_BZERO)
    explicit_bzero(p, n);
#else
    /* No libc secure zero (or built without datakit's CMake checks):
     * volatile stores the compiler must keep, then a barrier */
    volatile uint8_t *vp = (volatile uint8_t *)p;
    while (n--) {
        *vp++ = 0;
    }
    __asm__ volatile("" ::: "memory");
#endif
}
