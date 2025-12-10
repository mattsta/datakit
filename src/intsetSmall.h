#pragma once

#include "intsetCommon.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Small tier: int16_t values only, single contiguous array
 * Memory layout: [count16][values16...] */

typedef struct intsetSmall {
    uint32_t count16;   /* Number of int16_t values */
    int16_t values16[]; /* Sorted array of int16_t values */
} intsetSmall;

/* Create new small intset */
intsetSmall *intsetSmallNew(void);

/* Create small intset from sorted int16 array */
intsetSmall *intsetSmallFromArray(const int16_t *values, uint32_t count);

/* Free small intset */
void intsetSmallFree(intsetSmall *is);

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

/* Find value - returns position and search result */
intsetSearchResult intsetSmallFind(const intsetSmall *is, int64_t value,
                                   uint32_t *pos);

/* Get value at position (0-indexed) */
bool intsetSmallGet(const intsetSmall *is, uint32_t pos, int64_t *value);

/* Add value - returns new intset (may be reallocated) and whether added */
intsetSmall *intsetSmallAdd(intsetSmall *is, int64_t value, bool *added);

/* Remove value - returns new intset (may be reallocated) and whether removed */
intsetSmall *intsetSmallRemove(intsetSmall *is, int64_t value, bool *removed);

/* Check if should upgrade to medium tier */
bool intsetSmallShouldUpgrade(const intsetSmall *is, int64_t nextValue);

/* Iterator */
typedef struct intsetSmallIterator {
    const intsetSmall *is;
    uint32_t pos;
} intsetSmallIterator;

void intsetSmallIteratorInit(intsetSmallIterator *it, const intsetSmall *is);
bool intsetSmallIteratorNext(intsetSmallIterator *it, int64_t *value);
