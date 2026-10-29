#pragma once

/* The Medium tier's layout, for the intset family's own files */

#include "intsetMedium.h"
#include "intsetSmallInternal.h"

/* Memory layout: [count16][count32][values16...][values32...] */
struct intsetMedium {
    uint32_t count16;  /* Number of int16_t values */
    uint32_t count32;  /* Number of int32_t values */
    int16_t *values16; /* Sorted array of int16_t values */
    int32_t *values32; /* Sorted array of int32_t values */
};

/* Get total count */
static inline uint64_t intsetMediumCount(const intsetMedium *m) {
    return m ? (uint64_t)m->count16 + (uint64_t)m->count32 : 0;
}
