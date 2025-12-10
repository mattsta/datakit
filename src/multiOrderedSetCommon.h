#pragma once

#include "databox.h"
#include "flex.h"
#include "flexCapacityManagement.h"
#include <stdbool.h>
#include <stdint.h>

/* ====================================================================
 * Type Definitions
 * ====================================================================
 *
 * multiOrderedSet stores (score, member) pairs where:
 *   - Pairs are sorted by score first, then by member (for equal scores)
 *   - Members must be unique
 *   - elementsPerEntry is always 2: [score, member]
 * ==================================================================== */

#define MOS_ELEMENTS_PER_ENTRY 2

typedef enum multiOrderedSetType {
    MOS_TYPE_SMALL = 1,  /* Single flex, 16 bytes fixed */
    MOS_TYPE_MEDIUM = 2, /* Array of flex, ~32 bytes fixed */
    MOS_TYPE_FULL = 3    /* Dual structure, ~64 bytes */
} multiOrderedSetType;

/* Index types for consistency with multimap */
typedef uint32_t mosMapIdx;
typedef uint32_t mosMiddle;

/* ====================================================================
 * Score Comparison
 * ====================================================================
 * Scores are databoxes - we compare them using databox comparison
 * which handles all numeric types correctly.
 *
 * Comparison order:
 *   1. First by score (databox numeric comparison)
 *   2. Then by member (databox comparison) when scores equal
 * ==================================================================== */

/* Compare two entries (score, member) for sorting.
 * Returns: <0 if a < b, 0 if a == b, >0 if a > b */
DK_INLINE_ALWAYS int mosCompareEntries(const databox *scoreA,
                                       const databox *memberA,
                                       const databox *scoreB,
                                       const databox *memberB) {
    /* First compare scores */
    int cmp = databoxCompare(scoreA, scoreB);
    if (cmp != 0) {
        return cmp;
    }
    /* Equal scores: compare members */
    return databoxCompare(memberA, memberB);
}

/* Check if score is within range spec */
DK_INLINE_ALWAYS bool mosScoreInRange(const databox *score, const databox *min,
                                      bool minEx, const databox *max,
                                      bool maxEx) {
    int cmpMin = databoxCompare(score, min);
    int cmpMax = databoxCompare(score, max);

    /* Check min boundary */
    if (minEx) {
        if (cmpMin <= 0) {
            return false; /* score <= min, but we need score > min */
        }
    } else {
        if (cmpMin < 0) {
            return false; /* score < min */
        }
    }

    /* Check max boundary */
    if (maxEx) {
        if (cmpMax >= 0) {
            return false; /* score >= max, but we need score < max */
        }
    } else {
        if (cmpMax > 0) {
            return false; /* score > max */
        }
    }

    return true;
}

/* ====================================================================
 * Entry Reading/Writing Helpers
 * ====================================================================
 * Each entry is [score, member] in the flex.
 * ==================================================================== */

/* Read entry at position: returns score and member databoxes */
DK_INLINE_ALWAYS void mosReadEntry(const flex *f, const flexEntry *entry,
                                   databox *score, databox *member) {
    flexGetByType(entry, score);
    flexEntry *memberEntry = flexNext(f, (flexEntry *)entry);
    flexGetByType(memberEntry, member);
}

/* Get entry for member (linear scan) - returns entry pointing to score */
DK_INLINE_ALWAYS flexEntry *mosFindMemberLinear(const flex *f,
                                                const databox *member) {
    flexEntry *entry = flexHead(f);
    while (entry) {
        /* entry points to score, next is member */
        flexEntry *memberEntry = flexNext(f, entry);
        if (!memberEntry) {
            break;
        }

        databox currentMember;
        flexGetByType(memberEntry, &currentMember);

        if (databoxCompare(&currentMember, member) == 0) {
            return entry; /* Found: return pointer to score */
        }

        /* Move to next entry (skip member, go to next score) */
        entry = flexNext(f, memberEntry);
    }
    return NULL;
}

/* Helper: normalize negative rank to positive index */
DK_INLINE_ALWAYS int64_t mosNormalizeRank(int64_t rank, size_t count) {
    if (rank < 0) {
        rank = (int64_t)count + rank;
    }
    if (rank < 0 || (size_t)rank >= count) {
        return -1; /* Out of bounds */
    }
    return rank;
}
