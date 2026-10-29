#pragma once

/* The Full tier's layout, for the intset family's own files */

#include "intsetFull.h"
#include "intsetMediumInternal.h"

/* Memory layout:
 * [count16][count32][count64][values16...][values32...][values64...] */
struct intsetFull {
    uint64_t count16;  /* Number of int16_t values */
    uint64_t count32;  /* Number of int32_t values */
    uint64_t count64;  /* Number of int64_t values */
    int16_t *values16; /* Sorted array of int16_t values */
    int32_t *values32; /* Sorted array of int32_t values */
    int64_t *values64; /* Sorted array of int64_t values */
};

/* Get total count */
static inline uint64_t intsetFullCount(const intsetFull *f) {
    return f ? f->count16 + f->count32 + f->count64 : 0;
}
