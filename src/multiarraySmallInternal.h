#pragma once

#include <stdint.h>

/* multiarraySmall is a 16 byte struct.
 * 'data' is an array of elements.
 * 'len' is the width of each element in the array 'data'.
 * 'count' is the number of elements in 'data'
 * 'rowMax' is the number of elements to store before 'multiarray' grows
 *          this multiarraySmall into a multiarrayMedium. */
struct multiarraySmall {
    uint8_t *data;
    uint16_t len;    /* width of each entry in 'data' */
    uint16_t count;  /* number of entries (i.e. size of data is len * count) */
    uint16_t rowMax; /* not used here, but used for upgrading. */
    /* 2 bytes padding */
};

/* An index (or a range of 'count' from it, when 'count' is not 0) outside
 * the array: reports DK_DIAG_MULTIARRAY_INDEX for 'function' and answers
 * NULL.  Cold and not variadic, so a tier's hot path reaches it with a
 * tail call and keeps no frame for it.  Sites use the two macros. */
__attribute__((cold, noinline)) void *
multiarrayIndexRefused_(const char *function, const char *file, int line,
                        int64_t idx, uint64_t count);
#define MA_INDEX_REFUSED_(idx)                                                 \
    multiarrayIndexRefused_(__func__, __FILE__, __LINE__, (int64_t)(idx), 0)
#define MA_RANGE_REFUSED_(idx, count)                                          \
    multiarrayIndexRefused_(__func__, __FILE__, __LINE__, (int64_t)(idx),      \
                            (count))
