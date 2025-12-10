#pragma once

#include "intsetCommon.h"
#include "intsetMedium.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Full tier: int16_t, int32_t, and int64_t values in separate arrays
 * Memory layout:
 * [count16][count32][count64][values16...][values32...][values64...] */

typedef struct intsetFull {
    uint64_t count16;  /* Number of int16_t values */
    uint64_t count32;  /* Number of int32_t values */
    uint64_t count64;  /* Number of int64_t values */
    int16_t *values16; /* Sorted array of int16_t values */
    int32_t *values32; /* Sorted array of int32_t values */
    int64_t *values64; /* Sorted array of int64_t values */
} intsetFull;

/* Create new full intset */
intsetFull *intsetFullNew(void);

/* Create full intset by upgrading from medium */
intsetFull *intsetFullFromMedium(const intsetMedium *medium);

/* Free full intset */
void intsetFullFree(intsetFull *f);

/* Get total count */
static inline uint64_t intsetFullCount(const intsetFull *f) {
    return f ? f->count16 + f->count32 + f->count64 : 0;
}

/* Get total bytes */
size_t intsetFullBytes(const intsetFull *f);

/* Find value - returns position and search result
 * Position is in the merged virtual view (0 to count16+count32+count64-1) */
intsetSearchResult intsetFullFind(const intsetFull *f, int64_t value,
                                  uint64_t *pos);

/* Get value at position in merged view (0-indexed) */
bool intsetFullGet(const intsetFull *f, uint64_t pos, int64_t *value);

/* Add value - returns new intset (may be reallocated) and whether added */
intsetFull *intsetFullAdd(intsetFull *f, int64_t value, bool *added);

/* Remove value - returns new intset (may be reallocated) and whether removed */
intsetFull *intsetFullRemove(intsetFull *f, int64_t value, bool *removed);

/* Iterator */
typedef struct intsetFullIterator {
    const intsetFull *f;
    uint64_t pos16; /* Current position in values16 */
    uint64_t pos32; /* Current position in values32 */
    uint64_t pos64; /* Current position in values64 */
} intsetFullIterator;

void intsetFullIteratorInit(intsetFullIterator *it, const intsetFull *f);
bool intsetFullIteratorNext(intsetFullIterator *it, int64_t *value);
