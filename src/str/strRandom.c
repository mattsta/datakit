#include "../str.h"

/* ====================================================================
 * Randos
 * ==================================================================== */
/*  Written in 2015 by Sebastiano Vigna (vigna@acm.org)
    From http://xoroshiro.di.unimi.it/splitmix64.c
    (public domain / CC0) */

/* This is a very fast generator passing BigCrush, and it can be useful if for
 * some reason you absolutely want 64 bits of state; otherwise, we rather
 * suggest to use a xoroshiro128+ (for moderately parallel computations) or
 * xorshift1024* (for massively parallel computations) generator. */

uint64_t splitmix64(uint64_t *x) {
    uint64_t z = (*x += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

/*  Written in 2016 by David Blackman and Sebastiano Vigna (vigna@acm.org)
    From http://xoroshiro.di.unimi.it/xoroshiro128plus.c
    (public domain / CC0) */

/* This is the successor to xorshift128+. It is the fastest full-period
   generator passing BigCrush without systematic failures, but due to the
   relatively short period it is acceptable only for applications with a mild
   amount of parallelism; otherwise, use a xorshift1024* generator.

   Beside passing BigCrush, this generator passes the PractRand test suite up to
   (and included) 16TB, with the exception of binary rank tests, which fail due
   to the lowest bit being an LFSR; all other bits pass all tests. We suggest to
   use a sign test to extract a random Boolean value.  Note that the generator
   uses a simulated rotate operation, which most C compilers will turn into a
   single instruction. In Java, you can use Long.rotateLeft(). In languages that
   do not make low-level rotation instructions accessible xorshift128+ could be
   faster.

   The state must be seeded so that it is not everywhere zero. If you have a
   64-bit seed, we suggest to seed a splitmix64 generator and use its output to
   fill s. */

static inline uint64_t rotl(const uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

uint64_t xoroshiro128plus(uint64_t s[2]) {
    const uint64_t s0 = s[0];
    uint64_t s1 = s[1];
    const uint64_t result = s0 + s1;

    s1 ^= s0;
    s[0] = rotl(s0, 55) ^ s1 ^ (s1 << 14); // a, b
    s[1] = rotl(s1, 36);                   // c

    return result;
}

#if 0
/* This is the jump function for the generator. It is equivalent
   to 2^64 calls to next(); it can be used to generate 2^64
   non-overlapping subsequences for parallel computations. */

void xoroshiro128plusNext(uint64_t s[2]) {
    static const uint64_t JUMP[] = {0xbeac0467eba5facb, 0xd86b048b86aa9922};

    uint64_t s0 = 0;
    uint64_t s1 = 0;
    for (int i = 0; i < COUNT_ARRAY(JUMP); i++)
        for (int b = 0; b < 64; b++) {
            if (JUMP[i] & UINT64_C(1) << b) {
                s0 ^= s[0];
                s1 ^= s[1];
            }
            (void)xoroshiro128plus(s);
        }

    s[0] = s0;
    s[1] = s1;
}
#endif

/* Implementations and comments below taken from:
 *   https://en.wikipedia.org/wiki/Xorshift */

/* ref:
 * This algorithm has a maximal period of 2^128 − 1
 * and passes the diehard tests.
 * However, it fails the MatrixRank and LinearComp tests of the BigCrush
 * test suite from the TestU01 framework.
 */

/* State variables must be initialized so they are not all zero. */
/* Result of xorshift128 is in 'w' after the function returns. */
/* SPEED RANK: FASTEST (1st) */
void xorshift128(uint32_t *restrict x, uint32_t *restrict y,
                 uint32_t *restrict z, uint32_t *restrict w) {
    uint32_t t = *x;
    t ^= t << 11;
    t ^= t >> 8;
    *x = *y;
    *y = *z;
    *z = *w;
    *w ^= *w >> 19;
    *w ^= t;
}

/* The following 64-bit generator with 64 bits of state has a maximal period of
 * 2^64 − 1 and fails only the MatrixRank test of BigCrush: */

/* SPEED RANK: SLOWEST (4th) */
uint64_t xorshift64star(uint64_t *x) {
    *x ^= *x >> 12; // a
    *x ^= *x << 25; // b
    *x ^= *x >> 27; // c
    return *x * UINT64_C(2685821657736338717);
}

/* xorshift1024* generator with 1024 bits of state and a maximal period of
 * 2^1024 − 1; it passes BigCrush, even when reversed: */

/* SPEED RANK: SECOND SLOWEST (3rd) */
uint64_t xorshift1024star(uint64_t s[16], uint8_t *restrict sIndex) {
    const uint64_t s0 = s[*sIndex];
    uint64_t s1 = s[ *sIndex = (*sIndex + 1) % 16];
    s1 ^= s1 << 31;                                 // a
    s[*sIndex] = s1 ^ s0 ^ (s1 >> 11) ^ (s0 >> 30); // b, c
    return s[*sIndex] * UINT64_C(1181783497276652981);
}

/* Both generators, as it happens with all xorshift* generators, emit a
 * sequence of 64-bit values that is equidistributed in the maximum possible
 * dimension (except that they will never output zero 16 times in a row). */

/* Faster with no multipication:
 * xorshift+ family, based on 64-bit shifts:
 * the following xorshift128+ generator uses 128 bits of state and has a
 * maximal period of 2^128 − 1. It passes BigCrush, even when reversed. */

/* SPEED RANK: SECOND FASTEST (2nd) */
uint64_t xorshift128plus(uint64_t s[2]) {
    uint64_t x = s[0];
    const uint64_t y = s[1];
    s[0] = y;
    x ^= x << 23;                         // a
    s[1] = x ^ y ^ (x >> 17) ^ (y >> 26); // b, c
    return s[1] + y;
}

/* xorshift128plus is one of the fastest generators passing BigCrush.
 * One disadvantage of adding consecutive outputs is while the underlying
 * xorshift128 generator is 2-dimensionally equidistributed, the associated
 * xorshift128+ generator is just 1-dimensionally equidistributed. */
