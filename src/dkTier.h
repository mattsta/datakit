#pragma once

#include <stdint.h>

/* The shape of one tiered container, filled the same way by every family
 * that grows through tiers (multimap, multiOrderedSet, multilist,
 * multiarray, intset): which tier holds the data, how many inner units
 * hold it, and what it costs in memory.  Operators read it to see where
 * memory goes; tests read it to drive and check tier changes. */
typedef struct dkTierStats {
    uint64_t count;      /* rows, members or elements held */
    uint64_t dataBytes;  /* bytes of the stored data itself */
    uint64_t totalBytes; /* dataBytes plus the tier's own structs and arrays */
    uint32_t units;      /* inner maps, nodes or arrays holding the data */
    uint32_t unitLimit;  /* bytes a unit grows to before it splits (0: none) */
    uint8_t tier;        /* 1 Small, 2 Medium, 3 Full (Large for multiarray) */
} dkTierStats;
