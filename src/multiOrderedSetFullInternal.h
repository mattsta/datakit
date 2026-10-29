#pragma once

#include "multiOrderedSetCommon.h"
#include "multiarray.h"
#include "multidict.h"
#include "multimapAtom.h"

/* ====================================================================
 * multiOrderedSetFull - Dual structure tier
 * ====================================================================
 *
 * Storage:
 *   - memberIndex: multidict for O(1) member → score lookups
 *   - scoreMap: multiarray of flex for O(log n) sorted score operations
 *   - middle: multiarray of middle offsets for binary search
 *   - rangeBox: multiarray of score bounds for each sub-map
 *
 * This provides:
 *   - O(1) member existence check and score lookup
 *   - O(log n) rank queries and range operations
 *   - Efficient memory usage via split sub-maps
 *
 * Memory: ~64-80 bytes fixed overhead + actual data
 *
 * ATOM TABLE MODE (opt-in):
 *   When pool is non-NULL, scoreMap stores (score, memberID) instead of
 *   (score, member): members are interned in the atom table (multimapAtom,
 *   any index, ids and store) and only their ids are stored in scoreMap.
 *   The memberIndex still stores member strings as keys for O(1) lookup.
 * ==================================================================== */

struct multiOrderedSetFull {
    multidict *memberIndex;  /* member → score mapping (O(1) lookup) */
    multidictClass *mdClass; /* class for memberIndex (owned by us) */
    multiarray *scoreMap;    /* flex *; sorted (score,member/memberID) */
    multiarray *middle;      /* uint32_t; middle offsets per sub-map */
    multiarray *rangeBox;    /* databox; head score per sub-map */
    multimapAtom *pool;      /* Optional: atom table interning members */
    uint32_t mapCount;       /* Number of sub-maps */
    uint64_t
        totalEntries; /* Total (score, member) pairs - supports >4B entries */
    uint32_t maxMapSize; /* Max bytes before splitting a sub-map */
    uint32_t flags;      /* Bit 0: poolOwned (we free it on destroy) */
};

/* Flag bits */
static constexpr int MOS_FLAG_POOL_OWNED =
    0x01; /* We own the pool and should free it */

/* Size limit for sub-maps before splitting */
static constexpr int MOS_FULL_DEFAULT_MAX_MAP_SIZE = 4096;
