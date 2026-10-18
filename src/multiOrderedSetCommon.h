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
 * 96/128-bit values
 * ====================================================================
 * Scores and members may be 96/128-bit integers.  Such a value does not
 * fit a databox, so it decodes only into storage that is set up for it
 * (see databox.h, "the destination owner sets the wide capability"):
 *
 *   - Caller boxes (GetScore, GetByRank, First, Last, IteratorNext) are
 *     filled in place, as flexGetByType fills them.
 *     Pass a DATABOX_BIG_INIT'd databoxBig, re-initialized before each
 *     call, to receive a wide value; a zeroed box receives the type only.
 *     A BYTES result borrows from the set until the set changes.
 *   - Removing reads (AddGetPrevious, RemoveGetScore, PopMin, PopMax)
 *     never read the destination and return self-contained values:
 *     BYTES and 96/128-bit values are heap copies.  Release each one
 *     with databoxFreeData.
 *   - Array outputs (RandomMembers) ignore the destination's flags: each
 *     slot is cleared before it is filled, so a 96/128-bit value comes
 *     back deferred (type only, data.ptr == NULL; databoxIsWideDeferred).
 *   - Every value the set decodes for its own use is a plain databox with
 *     a stack databoxWide beside it: flexGetByTypeInto decodes into the
 *     pair directly, and for a decoder that fills a box in place
 *     (multidictFind, the tier iterators) mosBoxAim points the box at
 *     the slot first.  Narrow values never touch the slot.
 * ==================================================================== */

DK_INLINE_ALWAYS bool mosBoxIsWide(const databox *box) {
    return box->type == DATABOX_SIGNED_128 || box->type == DATABOX_UNSIGNED_128;
}

/* Point 'box' at 'slot' for a decoder that fills a box in place: the
 * header is cleared and big = 1, so a 96/128-bit value is written into
 * 'slot'.  A narrow value overwrites the pointer and leaves big = 1,
 * which nothing reads on a narrow value; re-aim before each decode. */
DK_INLINE_ALWAYS void mosBoxAim(databox *box, databoxWide *slot) {
    memset(box, 0, sizeof(*box));
    box->big = true;
    box->data.u128 = (__uint128_t *)slot->b;
}

/* Clear one slot of an array output before it is filled in place, so a
 * 96/128-bit value is reported deferred instead of being written through
 * whatever the slot held. */
DK_INLINE_ALWAYS void mosBoxClear(databox *box) {
    memset(box, 0, sizeof(*box));
}

/* Self-contained copy of the entry at 'fe' for a removing read; the
 * prior contents of 'out' are ignored. */
DK_INLINE_ALWAYS void mosReadOwned(const flexEntry *fe, databox *out) {
    memset(out, 0, sizeof(*out));
    flexGetByTypeCopy(fe, out);
}

/* Numeric score as a double.  Returns false for a non-numeric box. */
DK_INLINE_ALWAYS bool mosScoreToDouble(const databox *box, double *out) {
    switch (box->type) {
    case DATABOX_DOUBLE_64:
        *out = box->data.d64;
        return true;
    case DATABOX_FLOAT_32:
        *out = (double)box->data.f32;
        return true;
    case DATABOX_SIGNED_64:
        *out = (double)box->data.i64;
        return true;
    case DATABOX_UNSIGNED_64:
        *out = (double)box->data.u64;
        return true;
    case DATABOX_SIGNED_128:
        if (!box->data.i128) {
            return false;
        }
        *out = (double)*box->data.i128;
        return true;
    case DATABOX_UNSIGNED_128:
        if (!box->data.u128) {
            return false;
        }
        *out = (double)*box->data.u128;
        return true;
    default:
        return false;
    }
}

/* ====================================================================
 * Entry Reading/Writing Helpers
 * ====================================================================
 * Each entry is [score, member] in the flex.
 * ==================================================================== */

/* Read entry at position into caller boxes (filled in place, see above) */
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
        databoxWide currentMemberWide;
        flexGetByTypeInto(memberEntry, &currentMember, &currentMemberWide);

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
