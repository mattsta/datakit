#pragma once

/* Sorting arrays of plain numbers ascending.  dkSortArray(a, n) takes
 * the element size from the array's type and picks the comparator that
 * matches it, so a site cannot pair a uint32_t size with a uint64_t
 * comparator.  The comparators stand alone too, for a qsort or bsearch
 * a site writes itself. */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* qsort comparators: negative, zero or positive as *a sorts before, with
 * or after *b.  A NaN compares equal to everything, so arrays holding
 * NaN have no defined order. */
static inline int dkCompareU32(const void *a, const void *b) {
    const uint32_t x = *(const uint32_t *)a;
    const uint32_t y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static inline int dkCompareU64(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *)a;
    const uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static inline int dkCompareI64(const void *a, const void *b) {
    const int64_t x = *(const int64_t *)a;
    const int64_t y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static inline int dkCompareDouble(const void *a, const void *b) {
    const double x = *(const double *)a;
    const double y = *(const double *)b;
    return (x > y) - (x < y);
}

/* The 'n' elements of array 'a' (uint32_t, uint64_t, int64_t or double)
 * sorted ascending; another element type does not compile */
#define dkSortArray(a, n)                                                      \
    qsort((a), (n), sizeof(*(a)),                                              \
          _Generic(*(a),                                                       \
              uint32_t: dkCompareU32,                                          \
              uint64_t: dkCompareU64,                                          \
              int64_t: dkCompareI64,                                           \
              double: dkCompareDouble))

#ifdef DATAKIT_TEST
int dkSortTest(int argc, char *argv[]);
#endif
