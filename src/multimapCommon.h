#pragma once

#include <stdint.h>

#include "databox.h"
#include "flex.h"

struct multimapAtom;

/* multimapElements means you have 65535 available "columns" per row.
 * If you (for some strange reason) need more than 64k columns, you can
 * increase the storage class of multimapElements, but note: increasing the
 * storage class will increase all struct alignment causing additional
 * padding. */
/* Note: multimapSmall and multimapMedium use 16 bits for multimapElements. */
typedef uint32_t multimapElements;

/* 32 bit multimapFullIdx gives you a max multimapFull size of:
 *   - 2.1 TB with 512 byte maps
 *   - 4.3 TB with 1024 byte maps
 *   - 8.7 TB with 2048 byte maps
 *   - 17.5 TB with 4096 byte maps
 * 64 bit multimapFullIdx gives you a max multimapFull size of:
 *   - 9.4 ZB with 512 byte maps
 *   - 18.8 ZB with 1024 byte maps
 *   - 37.7 ZB with 2048 byte maps
 *   - 75.5 ZB with 4096 byte maps */

/* 'multimapFullIdx' is the storage class for
 * the total number of maps in a multimapFull */
typedef uint32_t multimapFullIdx;

/* multimapFullMiddle is the storage type for the middle offsets
 * of each of our maps.  Maps should always be less than 64k total,
 * so a 2 byte size class for finding the *middle* of a map should
 * be enough.
 *
 * If a map is larger than 64k, then it only holds *one* entry and
 * the middle of a one-entry map is the head of the map, which is
 * between 2 and 6 bytes in (and a uint16_t can easily hold the offsets
 * numbers '2' and '6').
 *
 * All that being said, 4 byte widths are *faster* than 2 byte widths
 * for our array storage, so we trade a little space for speed here. */
typedef uint32_t multimapFullMiddle;

/* 'multimapFullValues' is the storage class for:
 *   - the total number of key-value pairs in the multimapFull.
 *
 * The two extreme cases for 'multimapFullValues' are:
 *   - one isolated key-value pair per map
 *     - then 'count' == 'values'
 *   - 1024 key-value pairs per map
 *     - then 'values' need to be larger than 'count' if
 *       'count' approaches 'count'/'values' */
typedef uint32_t multimapFullValues;

typedef struct multimapIterator {
    void *mm; /* pointer back to the multimap (untagged) instance itself */
    flexEntry *entry;
    flex *map;
    uint32_t mapIndex; /* for Medium and Full: which index 'mm' came from */
    uint32_t elementsPerEntry : 16; /* cached value mm->elementsPerEntry */
    uint32_t type : 2;    /* original tagged type of 'mm'; values: 1, 2, 3 */
    uint32_t forward : 1; /* are we iterating forward? */
    uint32_t unused : 13;
} multimapIterator;

#if DK_C11
_Static_assert(sizeof(multimapIterator) == 32,
               "multimapIterator grew bigger than we expected?");
#endif

typedef struct multimapEntry {
    flex **map;
    flexEntry *fe;
    multimapFullIdx mapIdx;
} multimapEntry;

typedef enum multimapCondition {
    MULTIMAP_CONDITION_NONE = 0,
    MULTIMAP_CONDITION_ALL, /* foreach */
    MULTIMAP_CONDITION_LESS_THAN,
    MULTIMAP_CONDITION_LESS_THAN_EQUAL,
    MULTIMAP_CONDITION_EQUAL,
    MULTIMAP_CONDITION_GREATER_THAN,
    MULTIMAP_CONDITION_GREATER_THAN_EQUAL
} multimapCondition;

/* Meaning: [MAP CONTENTS] [CONDITION] [COMPARE AGAINST]
 *          e.g. "Is map entry <= 5.5?" */
typedef struct multimapPredicate {
    multimapCondition condition;
    databox compareAgainst;
} multimapPredicate;

typedef enum multimapType {
    MULTIMAP_TYPE_SMALL = 1,  /* 16 bytes, fixed. */
    MULTIMAP_TYPE_MEDIUM = 2, /* 28 bytes, fixed. */
    MULTIMAP_TYPE_FULL = 3,   /* 52 bytes, grows as necessary */
} multimapType;
