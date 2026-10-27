#include "dkSort.h"

#ifdef DATAKIT_TEST
#include "ctest.h"

#include <inttypes.h>
#include <string.h>

/* An order-free fingerprint of the 'n' elements of size 'size' at 'v':
 * the wrapping sum of each element's mixed bits, kept by any permutation */
static uint64_t dkSortFingerprint_(const void *v, size_t n, size_t size) {
    uint64_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t bits = 0;
        memcpy(&bits, (const uint8_t *)v + i * size, size);
        sum += dkSplitmix64Mix(bits);
    }

    return sum;
}

/* 'n' replayable draws of type T sorted by dkSortArray: each element is
 * >= the one before it, and the elements are the ones drawn */
#define DKSORT_CHECK(T, n, draw)                                               \
    do {                                                                       \
        T v[n];                                                                \
        uint64_t s = 0x5047;                                                   \
        for (size_t i = 0; i < (n); i++) {                                     \
            v[i] = (draw);                                                     \
        }                                                                      \
        const uint64_t before = dkSortFingerprint_(v, (n), sizeof(T));         \
        dkSortArray(v, (n));                                                   \
        for (size_t i = 1; i < (n); i++) {                                     \
            if (v[i] < v[i - 1]) {                                             \
                ERR(#T ": element %zu sorts before element %zu", i, i - 1);    \
                break;                                                         \
            }                                                                  \
        }                                                                      \
        if (dkSortFingerprint_(v, (n), sizeof(T)) != before) {                 \
            ERR(#T ": sorting changed the elements");                          \
        }                                                                      \
    } while (0)

int dkSortTest(int, char *[]) {
    int err = 0;

    TEST("dkSortArray sorts each element type ascending, full range") {
        /* Full-width draws reach both signs and the top bit, where a
         * comparator built on subtraction would wrap */
        DKSORT_CHECK(uint32_t, 999, (uint32_t)(dkSplitmix64Next(&s) >> 32));
        DKSORT_CHECK(uint64_t, 999, dkSplitmix64Next(&s));
        DKSORT_CHECK(int64_t, 999, (int64_t)dkSplitmix64Next(&s));
        DKSORT_CHECK(double, 999, (double)(int64_t)dkSplitmix64Next(&s) / 1e9);
    }

    TEST("the comparators order the extremes") {
        const uint64_t u[] = {0, UINT64_MAX};
        const int64_t i[] = {INT64_MIN, INT64_MAX};
        const uint32_t w[] = {0, UINT32_MAX};
        const double d[] = {-1e308, 1e308};
        if (dkCompareU64(&u[0], &u[1]) >= 0 ||
            dkCompareU64(&u[1], &u[0]) <= 0 ||
            dkCompareU64(&u[1], &u[1]) != 0 ||
            dkCompareI64(&i[0], &i[1]) >= 0 ||
            dkCompareI64(&i[1], &i[0]) <= 0 ||
            dkCompareU32(&w[0], &w[1]) >= 0 ||
            dkCompareU32(&w[1], &w[0]) <= 0 ||
            dkCompareDouble(&d[0], &d[1]) >= 0 ||
            dkCompareDouble(&d[1], &d[0]) <= 0) {
            ERR("a comparator misorders its type's extremes");
        }
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
