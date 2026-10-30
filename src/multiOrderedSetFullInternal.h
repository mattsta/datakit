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
 *   - scoreMap: multiarray of flex sub-maps of sorted (score, member) rows
 *   - middle: multiarray of middle offsets for binary search
 *   - rangeBox: multiarray of each sub-map's head score
 *   - memberIndex: multidict member -> score (members stored inline)
 *   - pool + internedScores: the atom table and id -> score (members
 *     stored once, in the table)
 *
 * This provides:
 *   - O(1) member existence check and score lookup
 *   - O(log n) rank queries and range operations
 *
 * MEMBERS, STORED ONCE
 *   A byte-string member longer than MOS_FULL_INTERN_MIN_LEN bytes lives
 *   in the set's atom table ('pool', made with the first such member): its
 *   row holds a reference (DATABOX_CONTAINER_REFERENCE_EXTERNAL, its id)
 *   and its score is in internedScores under the id.  Shorter members (and
 *   members of other types) are stored in the row and keyed in memberIndex
 *   as themselves.  Every comparison reads a reference as the member it
 *   stands for, so rows sort by member value in both forms.
 *
 *   A set made with a pool (multiOrderedSetFullNewWithPool, owned or
 *   shared) keeps every member in it (MOS_FLAG_INTERN_ALL).
 * ==================================================================== */

struct multiOrderedSetFull {
    multidict *memberIndex;    /* inline member -> score */
    multidictClass *mdClass;   /* class for memberIndex and internedScores */
    multiarray *scoreMap;      /* flex *; sorted (score, member or reference) */
    multiarray *middle;        /* uint32_t; middle offsets per sub-map */
    multiarray *rangeBox;      /* databox; head score per sub-map */
    multimapAtom *pool;        /* atom table of the interned members, or NULL */
    multidict *internedScores; /* interned member's id -> score, or NULL */
    uint32_t mapCount;         /* Number of sub-maps */
    uint64_t
        totalEntries; /* Total (score, member) pairs - supports >4B entries */
    uint32_t maxMapSize; /* Max bytes before splitting a sub-map */
    uint32_t flags;      /* MOS_FLAG_* */
};

/* Flag bits */
static constexpr int MOS_FLAG_POOL_OWNED =
    0x01; /* We own the pool and should free it */
static constexpr int MOS_FLAG_INTERN_ALL =
    0x02; /* Every member lives in the pool (a set made with one) */

/* A byte-string member longer than this lives in the atom table: measured
 * at the allocator (multiOrderedSetFullTest), a member costs about 13 + 2.2
 * bytes per member byte stored inline and 59 + 1.0 interned, which meet
 * near 40 bytes */
static constexpr uint32_t MOS_FULL_INTERN_MIN_LEN = 40;

/* Size limit for sub-maps before splitting */
static constexpr int MOS_FULL_DEFAULT_MAX_MAP_SIZE = 4096;
