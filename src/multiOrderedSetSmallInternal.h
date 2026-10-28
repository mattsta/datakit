#pragma once

#include "multiOrderedSetCommon.h"

/* ====================================================================
 * multiOrderedSetSmall - Single flex tier
 * ====================================================================
 *
 * Storage: Single flex containing (score, member) pairs sorted by score.
 * Member lookup: Linear scan (O(n), acceptable for small sets).
 * Score lookup: Binary search via flex sorted functions (O(log n)).
 *
 * A Small set has no header: the set's pointer is its flex, with the
 * middle row's offset in the pointer's top bits (multiOrderedSet.c).
 * This struct is the view each call builds on the stack from the
 * pointer; a call that changes the rows leaves the new flex and middle
 * here for the caller to pack back into the pointer.
 * ==================================================================== */

struct multiOrderedSetSmall {
    flex *map;       /* Single flex: [score0, member0, score1, member1, ...] */
    uint32_t middle; /* Offset to middle entry for binary search */
};
