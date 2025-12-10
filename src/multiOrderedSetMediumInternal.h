#pragma once

#include "multiOrderedSetCommon.h"

/* ====================================================================
 * multiOrderedSetMedium - Dual flex tier
 * ====================================================================
 *
 * Storage: Two flex arrays for better parallelism and cache behavior.
 * Split point: Median score divides entries between the two maps.
 * Member lookup: Linear scan across both maps (still O(n) but parallelizable).
 * Score lookup: Binary search in appropriate map (O(log n)).
 *
 * Memory: 8*2 + 4*2 + 4 = 28 bytes fixed overhead + flex contents
 * ==================================================================== */

struct multiOrderedSetMedium {
    flex *map[2];       /* Two maps: map[0] = lower scores, map[1] = higher */
    uint32_t middle[2]; /* Offset to middle of each sorted map */
    uint32_t flags;     /* Reserved for future use */
};

/* Note: struct may have padding for alignment */
