#pragma once

#include "intsetCommon.h"
#include <stddef.h>
#include <stdint.h>

/* Small tier: int16_t values only, single contiguous array.
 * Its layout, count and size: intsetSmallInternal.h.
 * @handle: returns: a call that can move it returns the new address */
typedef struct intsetSmall intsetSmall;

/* Create new small intset */
intsetSmall *intsetSmallNew(void);

/* Create small intset from sorted int16 array */
intsetSmall *intsetSmallFromArray(const int16_t *values, uint32_t count);

/* Free small intset */
void intsetSmallFree(intsetSmall *is);

/* Find value: INTSET_FOUND or INTSET_NOT_FOUND, and in '*pos' (may be
 * NULL) the value's index, or where it would go (0 to the count) */
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
