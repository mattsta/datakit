#include "dkC23.h"

#ifdef DATAKIT_TEST
#include "ctest.h"

#include <inttypes.h>
#include <string.h>

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
            dkLcg64Next(&s);                                                   \
            DKC23_CHECK(T, width, s >> (i % 64));                              \
        }                                                                      \
    } while (0)

/* 'x' rotated left by 'n' in 'width' bits, moving one bit at a time */
static uint64_t dkC23RotateLeft_(uint64_t x, unsigned int n,
                                 unsigned int width) {
    uint64_t r = 0;
    for (unsigned int b = 0; b < width; b++) {
        if ((x >> b) & 1) {
            r |= UINT64_C(1) << ((b + n) % width);
        }
    }
    return r;
}

/* Both rotates of a spread of T values by every count from 0 to twice the
 * width, against the oracle; right by n is left by width - n mod width */
#define DKC23_ROTATE_SWEEP(T, width)                                           \
    do {                                                                       \
        uint64_t s = 0xD1B54A32D192ED03u;                                      \
        for (int i = 0; i < 64; i++) {                                         \
            dkLcg64Next(&s);                                                   \
            const T x_ = (T)s;                                                 \
            for (unsigned int n = 0; n <= 2 * (width); n++) {                  \
                const uint64_t left =                                          \
                    dkC23RotateLeft_((uint64_t)x_, n % (width), (width));      \
                const uint64_t right = dkC23RotateLeft_(                       \
                    (uint64_t)x_, ((width) - n % (width)) % (width), (width)); \
                const T gotLeft = stdc_rotate_left(x_, n);                     \
                const T gotRight = stdc_rotate_right(x_, n);                   \
                if ((uint64_t)gotLeft != left ||                               \
                    (uint64_t)gotRight != right) {                             \
                    ERR("%s 0x%" PRIx64 " rotated by %u differs from its "     \
                        "oracle",                                              \
                        #T, (uint64_t)x_, n);                                  \
                }                                                              \
            }                                                                  \
        }                                                                      \
    } while (0)

int dkC23Test(int, char *[]) {
    int err = 0;

    TEST("stdbit: rotates at every width and count against bit-by-bit "
         "answers") {
        DKC23_ROTATE_SWEEP(uint8_t, 8u);
        DKC23_ROTATE_SWEEP(uint16_t, 16u);
        DKC23_ROTATE_SWEEP(uint32_t, 32u);
        DKC23_ROTATE_SWEEP(uint64_t, 64u);
        /* the result keeps the value's type: no promotion to int */
        static_assert(
            _Generic(stdc_rotate_left((uint8_t)1, 1u), uint8_t: 1, default: 0));
        static_assert(_Generic(stdc_rotate_right((uint64_t)1, 1u),
                          uint64_t: 1,
                          default: 0));
    }

    TEST("stdbit: every function at every width against bit-by-bit "
         "answers") {
        DKC23_SWEEP(unsigned char, UCHAR_WIDTH);
        DKC23_SWEEP(unsigned short, USHRT_WIDTH);
        DKC23_SWEEP(unsigned int, UINT_WIDTH);
        DKC23_SWEEP(unsigned long, ULONG_WIDTH);
        DKC23_SWEEP(unsigned long long, ULLONG_WIDTH);
    }

    TEST("stdbit: __STDC_ENDIAN_NATIVE__ names the order a word's bytes "
         "are stored in") {
        static_assert(__STDC_ENDIAN_LITTLE__ != __STDC_ENDIAN_BIG__);
        const uint32_t word = 0x01020304u;
        uint8_t bytes[4];
        memcpy(bytes, &word, sizeof(bytes));
        const bool little = bytes[0] == 0x04 && bytes[3] == 0x01;
        const bool big = bytes[0] == 0x01 && bytes[3] == 0x04;
        if ((__STDC_ENDIAN_NATIVE__ == __STDC_ENDIAN_LITTLE__) != little ||
            (__STDC_ENDIAN_NATIVE__ == __STDC_ENDIAN_BIG__) != big) {
            ERR("native order %d disagrees with the stored bytes %02x..%02x",
                (int)__STDC_ENDIAN_NATIVE__, bytes[0], bytes[3]);
        }
    }

    TEST("countof: the elements of an array, as a constant too") {
        int ints[7] = {};
        double grid[3][5] = {};
        struct {
            char c;
            int64_t i;
        } recs[4] = {};
        static_assert(countof(ints) == 7);
        static_assert(countof(grid) == 3 && countof(grid[0]) == 5);
        static_assert(countof(recs) == 4);
        static_assert(countof("abc") == 4); /* the terminator counts */
        /* a constexpr object's initializer may not define a type (C23
         * 6.7.2): the fallback must not either */
        static constexpr size_t constCount = countof(ints);
        static_assert(constCount == 7);
        size_t seen = 0;
        for (size_t i = 0; i < countof(ints); i++) {
            seen += (size_t)ints[i] + 1;
        }
        if (seen != 7 || countof(grid[2]) != 5 || recs[3].i != 0) {
            ERR("countof walked %zu of 7", seen);
        }
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
