#pragma once

#include "intsetCommon.h"
#include "intsetSmall.h"
#include <stddef.h>
#include <stdint.h>

/* Medium tier: int16_t and int32_t values in separate arrays.
 * Its layout and count: intsetMediumInternal.h.
 * @handle: returns: a call that can move it returns the new address */
typedef struct intsetMedium intsetMedium;

/* Create new medium intset */
intsetMedium *intsetMediumNew(void);

/* Create medium intset by upgrading from small */
intsetMedium *intsetMediumFromSmall(const intsetSmall *small);

/* Free medium intset */
void intsetMediumFree(intsetMedium *m);

/* Get total bytes */
size_t intsetMediumBytes(const intsetMedium *m);

/* Find value: INTSET_FOUND or INTSET_NOT_FOUND, and in '*pos' (may be
 * NULL; computing it costs one more search) the value's index in sorted
 * order across both widths, or where it would go (0 to the count) */
intsetSearchResult intsetMediumFind(const intsetMedium *m, int64_t value,
                                    uint64_t *pos);

/* Get value at position in merged view (0-indexed) */
bool intsetMediumGet(const intsetMedium *m, uint64_t pos, int64_t *value);

/* Add value - returns new intset (may be reallocated) and whether added */
intsetMedium *intsetMediumAdd(intsetMedium *m, int64_t value, bool *added);

/* Remove value - returns new intset (may be reallocated) and whether removed */
intsetMedium *intsetMediumRemove(intsetMedium *m, int64_t value, bool *removed);

/* Check if should upgrade to full tier */
bool intsetMediumShouldUpgrade(const intsetMedium *m, int64_t nextValue);

/* Iterator */
typedef struct intsetMediumIterator {
    const intsetMedium *m;
    uint32_t pos16; /* Current position in values16 */
    uint32_t pos32; /* Current position in values32 */
} intsetMediumIterator;

void intsetMediumIteratorInit(intsetMediumIterator *it, const intsetMedium *m);
bool intsetMediumIteratorNext(intsetMediumIterator *it, int64_t *value);
