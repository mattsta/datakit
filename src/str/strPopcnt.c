#include "../str.h"

/* ====================================================================
 * non-blocked popcnt
 * ==================================================================== */
static uint8_t lookup8bit[256] = {
    /* 0 */ 0,  /* 1 */ 1,  /* 2 */ 1,  /* 3 */ 2,
    /* 4 */ 1,  /* 5 */ 2,  /* 6 */ 2,  /* 7 */ 3,
    /* 8 */ 1,  /* 9 */ 2,  /* a */ 2,  /* b */ 3,
    /* c */ 2,  /* d */ 3,  /* e */ 3,  /* f */ 4,
    /* 10 */ 1, /* 11 */ 2, /* 12 */ 2, /* 13 */ 3,
    /* 14 */ 2, /* 15 */ 3, /* 16 */ 3, /* 17 */ 4,
    /* 18 */ 2, /* 19 */ 3, /* 1a */ 3, /* 1b */ 4,
    /* 1c */ 3, /* 1d */ 4, /* 1e */ 4, /* 1f */ 5,
    /* 20 */ 1, /* 21 */ 2, /* 22 */ 2, /* 23 */ 3,
    /* 24 */ 2, /* 25 */ 3, /* 26 */ 3, /* 27 */ 4,
    /* 28 */ 2, /* 29 */ 3, /* 2a */ 3, /* 2b */ 4,
    /* 2c */ 3, /* 2d */ 4, /* 2e */ 4, /* 2f */ 5,
    /* 30 */ 2, /* 31 */ 3, /* 32 */ 3, /* 33 */ 4,
    /* 34 */ 3, /* 35 */ 4, /* 36 */ 4, /* 37 */ 5,
    /* 38 */ 3, /* 39 */ 4, /* 3a */ 4, /* 3b */ 5,
    /* 3c */ 4, /* 3d */ 5, /* 3e */ 5, /* 3f */ 6,
    /* 40 */ 1, /* 41 */ 2, /* 42 */ 2, /* 43 */ 3,
    /* 44 */ 2, /* 45 */ 3, /* 46 */ 3, /* 47 */ 4,
    /* 48 */ 2, /* 49 */ 3, /* 4a */ 3, /* 4b */ 4,
    /* 4c */ 3, /* 4d */ 4, /* 4e */ 4, /* 4f */ 5,
    /* 50 */ 2, /* 51 */ 3, /* 52 */ 3, /* 53 */ 4,
    /* 54 */ 3, /* 55 */ 4, /* 56 */ 4, /* 57 */ 5,
    /* 58 */ 3, /* 59 */ 4, /* 5a */ 4, /* 5b */ 5,
    /* 5c */ 4, /* 5d */ 5, /* 5e */ 5, /* 5f */ 6,
    /* 60 */ 2, /* 61 */ 3, /* 62 */ 3, /* 63 */ 4,
    /* 64 */ 3, /* 65 */ 4, /* 66 */ 4, /* 67 */ 5,
    /* 68 */ 3, /* 69 */ 4, /* 6a */ 4, /* 6b */ 5,
    /* 6c */ 4, /* 6d */ 5, /* 6e */ 5, /* 6f */ 6,
    /* 70 */ 3, /* 71 */ 4, /* 72 */ 4, /* 73 */ 5,
    /* 74 */ 4, /* 75 */ 5, /* 76 */ 5, /* 77 */ 6,
    /* 78 */ 4, /* 79 */ 5, /* 7a */ 5, /* 7b */ 6,
    /* 7c */ 5, /* 7d */ 6, /* 7e */ 6, /* 7f */ 7,
    /* 80 */ 1, /* 81 */ 2, /* 82 */ 2, /* 83 */ 3,
    /* 84 */ 2, /* 85 */ 3, /* 86 */ 3, /* 87 */ 4,
    /* 88 */ 2, /* 89 */ 3, /* 8a */ 3, /* 8b */ 4,
    /* 8c */ 3, /* 8d */ 4, /* 8e */ 4, /* 8f */ 5,
    /* 90 */ 2, /* 91 */ 3, /* 92 */ 3, /* 93 */ 4,
    /* 94 */ 3, /* 95 */ 4, /* 96 */ 4, /* 97 */ 5,
    /* 98 */ 3, /* 99 */ 4, /* 9a */ 4, /* 9b */ 5,
    /* 9c */ 4, /* 9d */ 5, /* 9e */ 5, /* 9f */ 6,
    /* a0 */ 2, /* a1 */ 3, /* a2 */ 3, /* a3 */ 4,
    /* a4 */ 3, /* a5 */ 4, /* a6 */ 4, /* a7 */ 5,
    /* a8 */ 3, /* a9 */ 4, /* aa */ 4, /* ab */ 5,
    /* ac */ 4, /* ad */ 5, /* ae */ 5, /* af */ 6,
    /* b0 */ 3, /* b1 */ 4, /* b2 */ 4, /* b3 */ 5,
    /* b4 */ 4, /* b5 */ 5, /* b6 */ 5, /* b7 */ 6,
    /* b8 */ 4, /* b9 */ 5, /* ba */ 5, /* bb */ 6,
    /* bc */ 5, /* bd */ 6, /* be */ 6, /* bf */ 7,
    /* c0 */ 2, /* c1 */ 3, /* c2 */ 3, /* c3 */ 4,
    /* c4 */ 3, /* c5 */ 4, /* c6 */ 4, /* c7 */ 5,
    /* c8 */ 3, /* c9 */ 4, /* ca */ 4, /* cb */ 5,
    /* cc */ 4, /* cd */ 5, /* ce */ 5, /* cf */ 6,
    /* d0 */ 3, /* d1 */ 4, /* d2 */ 4, /* d3 */ 5,
    /* d4 */ 4, /* d5 */ 5, /* d6 */ 5, /* d7 */ 6,
    /* d8 */ 4, /* d9 */ 5, /* da */ 5, /* db */ 6,
    /* dc */ 5, /* dd */ 6, /* de */ 6, /* df */ 7,
    /* e0 */ 3, /* e1 */ 4, /* e2 */ 4, /* e3 */ 5,
    /* e4 */ 4, /* e5 */ 5, /* e6 */ 5, /* e7 */ 6,
    /* e8 */ 4, /* e9 */ 5, /* ea */ 5, /* eb */ 6,
    /* ec */ 5, /* ed */ 6, /* ee */ 6, /* ef */ 7,
    /* f0 */ 4, /* f1 */ 5, /* f2 */ 5, /* f3 */ 6,
    /* f4 */ 5, /* f5 */ 6, /* f6 */ 6, /* f7 */ 7,
    /* f8 */ 5, /* f9 */ 6, /* fa */ 6, /* fb */ 7,
    /* fc */ 6, /* fd */ 7, /* fe */ 7, /* ff */ 8};

uint64_t StrPopCnt8Bit(const void *data_, const size_t len) {
    const uint8_t *restrict data = data_;

    uint64_t result = 0;

    for (size_t i = 0; i < len; i++) {
        result += lookup8bit[data[i]];
    }

    return result;
}

uint64_t StrPopCntAligned(const void *data_, size_t len) {
    const uint8_t *restrict data = data_;
    static const size_t multiStepSize = sizeof(uint64_t) * 4;

    /* Accumulator for pre and post alignment bytes */
    uint64_t singleAccum = 0;

    /* Accumulators across every iteration */
    uint64_t c0 = 0;
    uint64_t c1 = 0;
    uint64_t c2 = 0;
    uint64_t c3 = 0;

    /* Process initial unaligned bytes */
    const uint8_t readFor = DK_WORD_UNALIGNMENT(data);
    singleAccum += StrPopCnt8Bit(data, readFor);
    data += readFor;
    len -= readFor;

    /* Process aligned bytes */
    const uint64_t *v = (uint64_t *)data;
    while (len >= multiStepSize) {
#if __x86_64__
        /* manually still make up for intel having false dependencies in popcnt
         */
        const uint64_t r0 = v[0];
        const uint64_t r1 = v[1];
        const uint64_t r2 = v[2];
        const uint64_t r3 = v[3];
        __asm__("popcnt %4, %4  \n\t"
                "add %4, %0     \n\t"
                "popcnt %5, %5  \n\t"
                "add %5, %1     \n\t"
                "popcnt %6, %6  \n\t"
                "add %6, %2     \n\t"
                "popcnt %7, %7  \n\t"
                "add %7, %3     \n\t"
                : "+r"(c0), "+r"(c1), "+r"(c2), "+r"(c3)
                : "r"(r0), "r"(r1), "r"(r2), "r"(r3));
#else
        /* Trust these work correctly... */
        c0 += __builtin_popcountll(v[0]);
        c1 += __builtin_popcountll(v[1]);
        c2 += __builtin_popcountll(v[2]);
        c3 += __builtin_popcountll(v[3]);
#endif
        v += 4;
        len -= multiStepSize;
    }

    /* Count remaining unaligned bytes */
    singleAccum += StrPopCnt8Bit(v, len);

    return singleAccum + c0 + c1 + c2 + c3;
}

uint64_t StrPopCntExact(const void *data_, size_t len) {
    const uint8_t *restrict data = data_;
    static const size_t multiStepSize = sizeof(uint64_t) * 4;

    /* 'len' must be exactly divisible by 64 bytes or else we won't
     * process all bits. */
    assert(len % multiStepSize == 0);

    /* Reminder: popcnt has a false instruction-level dependency
     *           at the CPU microcode level, so no amount of compiler
     *           magic will fix the problem until compilers basically
     *           replicate the scheme below.
     *           Basically, popcnt must NOT use the same destination
     *           register across calls within the same pipeline
     *           because the instructions set falsely waits for the
     *           destination register unnecessarily.
     *
     *           The inline assembly version with manually sharded Accumulators
     *           is 2x faster than the compiler-provided intrinsic loop. */
#if __x86_64__
    /* Accumulators across every iteration */
    uint64_t c0 = 0;
    uint64_t c1 = 0;
    uint64_t c2 = 0;
    uint64_t c3 = 0;

    const uint64_t *v = (uint64_t *)data;
    while (len) {
        const uint64_t r0 = v[0];
        const uint64_t r1 = v[1];
        const uint64_t r2 = v[2];
        const uint64_t r3 = v[3];
        __asm__("popcnt %4, %4  \n\t"
                "add %4, %0     \n\t"
                "popcnt %5, %5  \n\t"
                "add %5, %1     \n\t"
                "popcnt %6, %6  \n\t"
                "add %6, %2     \n\t"
                "popcnt %7, %7  \n\t"
                "add %7, %3     \n\t"
                : "+r"(c0), "+r"(c1), "+r"(c2), "+r"(c3)
                : "r"(r0), "r"(r1), "r"(r2), "r"(r3));
        v += 4;
        len -= multiStepSize;
    }

    return c0 + c1 + c2 + c3;
#else
    /* If you want to benchmark the 2x slower intrinsic version where
     * even 2018-era gcc and clang can't get the register dependencies
     * sorted out correctly, enable this and benchmark.
     * (and, yes, even attempting to shard the results into c0-c3
     * doesn't work, and yes, even trying to specify them as register
     * variables doesn't work) */
    uint64_t result = 0;
    uint64_t *v = (uint64_t *)data;
    while (len) {
        result += __builtin_popcount(v[0]);
        result += __builtin_popcount(v[1]);
        result += __builtin_popcount(v[2]);
        result += __builtin_popcount(v[3]);
        v += 4;
        len -= multiStepSize;
    }
    return result;
#endif
}
