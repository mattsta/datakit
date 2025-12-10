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
 * Memory: 8 + 4 + 4 = 16 bytes fixed overhead + flex contents
 * ==================================================================== */

struct multiOrderedSetSmall {
    flex *map;       /* Single flex: [score0, member0, score1, member1, ...] */
    uint32_t middle; /* Offset to middle entry for binary search */
    uint32_t flags;  /* Reserved for future use (compression, etc.) */
};

#if DK_C11
_Static_assert(sizeof(struct multiOrderedSetSmall) ==
                   (sizeof(flex *) + sizeof(uint32_t) * 2),
               "multiOrderedSetSmall struct size unexpected!");
#endif
