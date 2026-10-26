#include "dkC23.h"

#ifdef DATAKIT_TEST
#include "ctest.h"

#include <inttypes.h>

/* Independent answers, found one bit at a time */
static unsigned int dkC23Leading_(uint64_t x, unsigned int width) {
    unsigned int n = 0;
    while (n < width && !((x >> (width - 1 - n)) & 1)) {
        n++;
    }
    return n;
}

static unsigned int dkC23Trailing_(uint64_t x, unsigned int width) {
    unsigned int n = 0;
    while (n < width && !((x >> n) & 1)) {
        n++;
    }
    return n;
}

static unsigned int dkC23Ones_(uint64_t x) {
    unsigned int n = 0;
    for (; x; x >>= 1) {
        n += (unsigned int)(x & 1);
    }
    return n;
}

static uint64_t dkC23Floor_(uint64_t x) {
    uint64_t p = 0;
    for (uint64_t b = 1; b != 0 && b <= x; b <<= 1) {
        p = b;
    }
    return p;
}

/* The ceiling, or 0 when it does not fit 'width' bits */
static uint64_t dkC23Ceil_(uint64_t x, unsigned int width) {
    uint64_t b = 1;
    while (b < x) {
        b <<= 1;
        if (b == 0 || (width < 64 && (b >> width) != 0)) {
            return 0;
        }
    }
    return b;
}

/* Every stdc_* result for 'v' as a T against the oracles (a ceiling that
 * does not fit is unspecified, so it is not compared) */
#define DKC23_CHECK(T, width, v)                                               \
    do {                                                                       \
        const T x_ = (T)(v);                                                   \
        const uint64_t u_ = (uint64_t)x_;                                      \
        const uint64_t ceil_ = dkC23Ceil_(u_, (width));                        \
        if (stdc_leading_zeros(x_) != dkC23Leading_(u_, (width)) ||            \
            stdc_trailing_zeros(x_) != dkC23Trailing_(u_, (width)) ||          \
            stdc_count_ones(x_) != dkC23Ones_(u_) ||                           \
            stdc_has_single_bit(x_) != (dkC23Ones_(u_) == 1) ||                \
            stdc_bit_width(x_) != (width) - dkC23Leading_(u_, (width)) ||      \
            (uint64_t)stdc_bit_floor(x_) != dkC23Floor_(u_) ||                 \
            (ceil_ != 0 && (uint64_t)stdc_bit_ceil(x_) != ceil_)) {            \
            ERR("%s 0x%" PRIx64 ": a stdbit result differs from its oracle",   \
                #T, u_);                                                       \
        }                                                                      \
    } while (0)

/* Zero, every single bit and its neighbours, every all-ones run and a
 * spread of mixed values, at T's width */
#define DKC23_SWEEP(T, width)                                                  \
    do {                                                                       \
        DKC23_CHECK(T, width, 0);                                              \
        for (unsigned int b = 0; b < (width); b++) {                           \
            const uint64_t one = UINT64_C(1) << b;                             \
            DKC23_CHECK(T, width, one);                                        \
            DKC23_CHECK(T, width, one - 1);                                    \
            DKC23_CHECK(T, width, one + 1);                                    \
            DKC23_CHECK(T, width, UINT64_MAX >> b);                            \
        }                                                                      \
        uint64_t s = 0x9E3779B97F4A7C15u;                                      \
        for (int i = 0; i < 2000; i++) {                                       \
            s = s * 6364136223846793005u + 1442695040888963407u;               \
            DKC23_CHECK(T, width, s >> (i % 64));                              \
        }                                                                      \
    } while (0)

int dkC23Test(int, char *[]) {
    int err = 0;

    TEST("stdbit: every function at every width against bit-by-bit "
         "answers") {
        DKC23_SWEEP(unsigned int, UINT_WIDTH);
        DKC23_SWEEP(unsigned long, ULONG_WIDTH);
        DKC23_SWEEP(unsigned long long, ULLONG_WIDTH);
    }

    TEST("stdbit: uint64_t and size_t pick their width") {
        if (stdc_bit_ceil((uint64_t)5) != 8 || stdc_bit_floor((size_t)5) != 4 ||
            stdc_bit_width((uint64_t)UINT64_MAX) != 64) {
            ERR("fixed-width types do not reach the right function");
        }
    }

    TEST_FINAL_RESULT;
}
#endif
