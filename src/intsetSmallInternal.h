#pragma once

/* The Small tier's layout, for the intset family's own files */

#include "intsetSmall.h"

/* Memory layout: [count16][values16...] */
struct intsetSmall {
    uint32_t count16;   /* Number of int16_t values */
    int16_t values16[]; /* Sorted array of int16_t values */
};

/* Get total count */
static inline uint32_t intsetSmallCount(const intsetSmall *is) {
    return is ? is->count16 : 0;
}

/* Get total bytes */
static inline size_t intsetSmallBytes(const intsetSmall *is) {
    if (!is) {
        return 0;
    }
    return sizeof(intsetSmall) + (is->count16 * sizeof(int16_t));
}
