#pragma once

#include "cleaner.h"
#include "databox.h"
#include "flex.h"
#include "flexCapacityManagement.h"
#include "str.h"  /* xorshift64star */
#include <math.h> /* isfinite */
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

/* base + delta for IncrBy, following the platform increment policy
 * (DATABOX.md "Numeric updates").  Two 64-bit integers add exactly; the
 * result is SIGNED_64 or UNSIGNED_64, whichever holds it, and a result
 * outside [INT64_MIN, UINT64_MAX] is refused.  Any real operand makes the
 * sum a double, and an inf or NaN result is refused.  A 96/128-bit operand
 * is refused.  Returns false, leaving '*out' unwritten, on any refusal. */
DK_INLINE_ALWAYS bool mosScoreAdd(const databox *base, const databox *delta,
                                  databox *out) {
    const bool baseInt =
        base->type == DATABOX_SIGNED_64 || base->type == DATABOX_UNSIGNED_64;
    const bool deltaInt =
        delta->type == DATABOX_SIGNED_64 || delta->type == DATABOX_UNSIGNED_64;
    if (baseInt && deltaInt) {
        const __int128_t a = base->type == DATABOX_SIGNED_64
                                 ? (__int128_t)base->data.i64
                                 : (__int128_t)base->data.u64;
        const __int128_t b = delta->type == DATABOX_SIGNED_64
                                 ? (__int128_t)delta->data.i64
                                 : (__int128_t)delta->data.u64;
        const __int128_t sum = a + b; /* cannot overflow 128 bits */
        if (sum >= INT64_MIN && sum <= INT64_MAX) {
            *out = databoxNewSigned((int64_t)sum);
            return true;
        }

        if (sum >= 0 && sum <= (__int128_t)UINT64_MAX) {
            *out = databoxNewUnsigned((uint64_t)sum);
            return true;
        }

        return false;
    }

    const bool wide = base->type == DATABOX_SIGNED_128 ||
                      base->type == DATABOX_UNSIGNED_128 ||
                      delta->type == DATABOX_SIGNED_128 ||
                      delta->type == DATABOX_UNSIGNED_128;
    double a;
    double b;
    if (wide || !mosScoreToDouble(base, &a) || !mosScoreToDouble(delta, &b)) {
        return false;
    }

    const double sum = a + b;
    if (!isfinite(sum)) {
        return false;
    }

    DATABOX_SET_DOUBLE(out, sum);
    return true;
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

/* Get entry for member (linear scan) - returns entry pointing to score.
 * Byte strings compare equal only when their bytes are identical (the
 * natural-order compare never equates different bytes), so a stored
 * string member is matched by length and memcmp alone.  'rowOut', when
 * not NULL, receives the found row's index (its rank in 'f'). */
DK_INLINE_ALWAYS flexEntry *
mosFindMemberLinearRow(const flex *f, const databox *member, size_t *rowOut) {
    size_t row = 0;
    const bool wantBytes = DATABOX_IS_BYTES(member);
    const void *want = wantBytes ? databoxCBytes(member) : NULL;
    const size_t wantLen = wantBytes ? databoxLen(member) : 0;
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

        const bool match =
            (wantBytes && currentMember.type == DATABOX_BYTES)
                ? currentMember.len == wantLen &&
                      memcmp(currentMember.data.bytes.start, want, wantLen) == 0
                : databoxCompare(&currentMember, member) == 0;
        if (match) {
            if (rowOut) {
                *rowOut = row;
            }

            return entry; /* Found: return pointer to score */
        }

        /* Move to next entry (skip member, go to next score) */
        entry = flexNext(f, memberEntry);
        row++;
    }
    return NULL;
}

DK_INLINE_ALWAYS flexEntry *mosFindMemberLinear(const flex *f,
                                                const databox *member) {
    return mosFindMemberLinearRow(f, member, NULL);
}

/* Whether row (aScore, aMember) sorts before row (bScore, bMember): rows
 * order by score, then member */
DK_INLINE_ALWAYS bool mosRowBefore(const databox *aScore,
                                   const databox *aMember,
                                   const databox *bScore,
                                   const databox *bMember) {
    const int compared = databoxCompare(aScore, bScore);
    return compared < 0 ||
           (compared == 0 && databoxCompare(aMember, bMember) < 0);
}

/* Rewrites the score of the row whose score entry is 'entry' (in '*f') to
 * 'score' in place, when the row keeps its place: still after the row
 * before it and before the row after it.  Returns false, changing
 * nothing, when the row would move (the caller removes and reinserts it).
 * The update that found the row reuses its position instead of a delete
 * and a sorted insert.  The flex may be reallocated and resized: a caller
 * keeping a middle offset refreshes it when flexBytes changed. */
DK_INLINE_ALWAYS bool mosRewriteScoreInPlace(flex **f, flexEntry *entry,
                                             const databox *score,
                                             const databox *member) {
    databox otherScore;
    databox otherMember;
    databoxWide otherScoreWide;
    databoxWide otherMemberWide;
    flexEntry *prevMember = flexPrev(*f, entry);
    if (prevMember) {
        flexGetByTypeInto(flexPrev(*f, prevMember), &otherScore,
                          &otherScoreWide);
        flexGetByTypeInto(prevMember, &otherMember, &otherMemberWide);
        if (!mosRowBefore(&otherScore, &otherMember, score, member)) {
            return false;
        }
    }

    flexEntry *nextScore = flexNext(*f, flexNext(*f, entry));
    if (nextScore) {
        flexGetByTypeInto(nextScore, &otherScore, &otherScoreWide);
        flexGetByTypeInto(flexNext(*f, nextScore), &otherMember,
                          &otherMemberWide);
        if (!mosRowBefore(score, member, &otherScore, &otherMember)) {
            return false;
        }
    }

    flexReplaceByType(f, entry, score);
    return true;
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

/* Resolve the inclusive rank range [*start, *stop] against 'count' rows:
 * negative ranks count from the end, a start
 * below the first row starts at it and a stop past the last row stops at
 * it.  False (nothing in range) when the range is empty or entirely
 * outside the rows. */
DK_INLINE_ALWAYS bool mosClampRankRange(int64_t *start, int64_t *stop,
                                        size_t count) {
    const int64_t n = (int64_t)count;
    int64_t s = *start < 0 ? *start + n : *start;
    int64_t e = *stop < 0 ? *stop + n : *stop;
    if (s < 0) {
        s = 0;
    }
    if (e >= n) {
        e = n - 1;
    }
    /* With e clamped below n, a start past the end, a stop before the
     * first row and an empty set (e == -1) all leave s > e */
    if (s > e) {
        return false;
    }
    *start = s;
    *stop = e;
    return true;
}

/* ====================================================================
 * Random members
 * ==================================================================== */

/* Fill members[slot] and scores[slot] with the pair at 'rank' (each tier
 * clears the slots first, so a 96/128-bit value comes back deferred) */
typedef bool mosRankPick(const void *set, size_t rank, databox *member,
                         databox *score);

defcleanerName(uint64_t *, zfree, MosRankSet);

/* RandomMembers for any tier over a set of 'total' > 0 pairs, drawing
 * from the tier's xorshift64star state 'rng':
 *   - count < 0: exactly -count picks, each rank drawn independently, so
 *     members may repeat;
 *   - count >= 0: min(count, total) distinct ranks.  When they are a
 *     large share of the set, one selection-sampling pass picks them
 *     (ascending); otherwise ranks are drawn and rejected against a hash
 *     set sized to the request, so a small sample of a big set costs
 *     O(count), not O(total).
 * Returns the number of slot pairs filled. */
static inline size_t mosRandomMembersFill(const void *set, mosRankPick *pick,
                                          uint64_t *rng, size_t total,
                                          int64_t count, databox *members,
                                          databox *scores) {
    size_t retrieved = 0;
    if (count < 0) {
        const uint64_t want = (uint64_t)0 - (uint64_t)count;
        for (uint64_t i = 0; i < want; i++) {
            const size_t rank = xorshift64star(rng) % total;
            if (pick(set, rank, &members[retrieved], &scores[retrieved])) {
                retrieved++;
            }
        }

        return retrieved;
    }

    const size_t want = (uint64_t)count < total ? (size_t)count : total;
    if (want == 0) {
        return 0;
    }

    if (want > total / 4) {
        /* Selection sampling: rank i is taken with probability
         * needed / remaining, which reaches 1 once every remaining rank is
         * needed, so exactly 'want' ranks are taken */
        for (size_t i = 0; i < total && retrieved < want; i++) {
            const size_t remaining = total - i;
            const size_t needed = want - retrieved;
            if (xorshift64star(rng) % remaining < needed &&
                pick(set, i, &members[retrieved], &scores[retrieved])) {
                retrieved++;
            }
        }

        return retrieved;
    }

    /* Rejection sampling against an open-addressing set of rank + 1
     * (0 = empty slot), at most half full.  At most a quarter of the ranks
     * are taken, so each draw is new with probability >= 3/4. */
    size_t slots = 16;
    while (slots < want * 2) {
        slots <<= 1;
    }

    const size_t mask = slots - 1;
    cleanerName(zfree, MosRankSet) uint64_t *seen =
        zcalloc(slots, sizeof(*seen));
    size_t draws = 0;
    while (retrieved < want && draws < want * 64) {
        draws++;
        const size_t rank = xorshift64star(rng) % total;
        size_t at = rank & mask;
        while (seen[at] != 0 && seen[at] != (uint64_t)rank + 1) {
            at = (at + 1) & mask;
        }

        if (seen[at] != 0) {
            continue; /* already taken */
        }

        seen[at] = (uint64_t)rank + 1;
        if (pick(set, rank, &members[retrieved], &scores[retrieved])) {
            retrieved++;
        }
    }

    return retrieved;
}
