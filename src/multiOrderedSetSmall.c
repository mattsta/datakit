#include "multiOrderedSetSmall.h"
#include "multiOrderedSetSmallInternal.h"

#include "str.h" /* for random functions */

/* ====================================================================
 * Random Number Generator
 * ==================================================================== */
static uint64_t mosRandomState = 0x12345678ABCDEF01ULL;

static inline uint64_t mosRandom(void) {
    return xorshift64star(&mosRandomState);
}

/* ====================================================================
 * Databox Arithmetic Helper
 * ==================================================================== */

/* Add delta to base, store result in out. Returns true on success. */
static bool mosDataboxAdd(const databox *base, const databox *delta,
                          databox *out) {
    /* Convert both to double and add */
    double baseVal, deltaVal;

    switch (base->type) {
    case DATABOX_DOUBLE_64:
        baseVal = base->data.d64;
        break;
    case DATABOX_FLOAT_32:
        baseVal = (double)base->data.f32;
        break;
    case DATABOX_SIGNED_64:
        baseVal = (double)base->data.i64;
        break;
    case DATABOX_UNSIGNED_64:
        baseVal = (double)base->data.u64;
        break;
    default:
        return false;
    }

    switch (delta->type) {
    case DATABOX_DOUBLE_64:
        deltaVal = delta->data.d64;
        break;
    case DATABOX_FLOAT_32:
        deltaVal = (double)delta->data.f32;
        break;
    case DATABOX_SIGNED_64:
        deltaVal = (double)delta->data.i64;
        break;
    case DATABOX_UNSIGNED_64:
        deltaVal = (double)delta->data.u64;
        break;
    default:
        return false;
    }

    DATABOX_SET_DOUBLE(out, baseVal + deltaVal);
    return true;
}

/* ====================================================================
 * Macros for middle management
 * ==================================================================== */
#define GET_MIDDLE(m) (flexEntry *)((m)->map + (m)->middle)
#define SET_MIDDLE(m, mid)                                                     \
    do {                                                                       \
        (m)->middle = (uint32_t)((mid) - (m)->map);                            \
    } while (0)
#define SET_MIDDLE_FORCE(m)                                                    \
    do {                                                                       \
        SET_MIDDLE(m, flexMiddle((m)->map, MOS_ELEMENTS_PER_ENTRY));           \
    } while (0)

/* ====================================================================
 * Internal Helpers
 * ==================================================================== */

/* Find entry position for a score (binary search).
 * Returns the entry where we should insert, or the entry with matching score.
 * 'found' is set to true if exact score match found. */
static flexEntry *mosFindScorePosition(const multiOrderedSetSmall *m,
                                       const databox *score,
                                       const flexEntry *middle, bool *found) {
    *found = false;
    flexEntry *entry = flexFindByTypeSortedWithMiddle(
        m->map, MOS_ELEMENTS_PER_ENTRY, score, (flexEntry *)middle);
    if (entry) {
        *found = true;
    }
    return entry;
}

/* Insert (score, member) pair in sorted position */
static void mosInsertSorted(multiOrderedSetSmall *m, const databox *score,
                            const databox *member) {
    const databox *elements[2] = {score, member};
    flexEntry *middle = GET_MIDDLE(m);

    /* Insert sorted by score, comparing full width for positioning
     * (score first, then member for equal scores) */
    flexInsertByTypeSortedWithMiddleMultiDirect(&m->map, MOS_ELEMENTS_PER_ENTRY,
                                                elements, &middle);
    SET_MIDDLE(m, middle);
}

/* Remove entry at position (entry points to score) */
static void mosRemoveEntry(multiOrderedSetSmall *m, flexEntry *entry) {
    /* Delete 2 elements: score and member */
    flexDeleteCount(&m->map, &entry, MOS_ELEMENTS_PER_ENTRY);
    SET_MIDDLE_FORCE(m);
}

/* ====================================================================
 * Creation / Destruction
 * ==================================================================== */

multiOrderedSetSmall *multiOrderedSetSmallNew(void) {
    multiOrderedSetSmall *m = zcalloc(1, sizeof(*m));
    m->map = flexNew();
    m->middle = FLEX_EMPTY_SIZE;
    m->flags = 0;
    return m;
}

multiOrderedSetSmall *multiOrderedSetSmallCopy(const multiOrderedSetSmall *m) {
    multiOrderedSetSmall *copy = zcalloc(1, sizeof(*copy));
    copy->map = flexDuplicate(m->map);
    copy->middle = m->middle;
    copy->flags = m->flags;
    return copy;
}

void multiOrderedSetSmallFree(multiOrderedSetSmall *m) {
    if (m) {
        flexFree(m->map);
        zfree(m);
    }
}

void multiOrderedSetSmallReset(multiOrderedSetSmall *m) {
    flexFree(m->map);
    m->map = flexNew();
    m->middle = FLEX_EMPTY_SIZE;
}

/* ====================================================================
 * Statistics
 * ==================================================================== */

size_t multiOrderedSetSmallCount(const multiOrderedSetSmall *m) {
    return flexCount(m->map) / MOS_ELEMENTS_PER_ENTRY;
}

size_t multiOrderedSetSmallBytes(const multiOrderedSetSmall *m) {
    return flexBytes(m->map);
}

/* ====================================================================
 * Insertion / Update
 * ==================================================================== */

bool multiOrderedSetSmallAdd(multiOrderedSetSmall *m, const databox *score,
                             const databox *member) {
    /* Check if member already exists (linear scan) */
    flexEntry *existing = mosFindMemberLinear(m->map, member);

    if (existing) {
        /* Member exists: remove old entry and insert with new score */
        mosRemoveEntry(m, existing);
        mosInsertSorted(m, score, member);
        return true; /* Replaced */
    }

    /* Member doesn't exist: insert new entry */
    mosInsertSorted(m, score, member);
    return false; /* New insert */
}

bool multiOrderedSetSmallAddNX(multiOrderedSetSmall *m, const databox *score,
                               const databox *member) {
    /* Only add if member doesn't exist */
    flexEntry *existing = mosFindMemberLinear(m->map, member);
    if (existing) {
        return false; /* Already exists, not added */
    }

    mosInsertSorted(m, score, member);
    return true; /* Added */
}

bool multiOrderedSetSmallAddXX(multiOrderedSetSmall *m, const databox *score,
                               const databox *member) {
    /* Only update if member exists */
    flexEntry *existing = mosFindMemberLinear(m->map, member);
    if (!existing) {
        return false; /* Doesn't exist, not updated */
    }

    mosRemoveEntry(m, existing);
    mosInsertSorted(m, score, member);
    return true; /* Updated */
}

bool multiOrderedSetSmallAddGetPrevious(multiOrderedSetSmall *m,
                                        const databox *score,
                                        const databox *member,
                                        databox *prevScore) {
    flexEntry *existing = mosFindMemberLinear(m->map, member);

    if (existing) {
        /* Get the previous score before removing */
        flexGetByType(existing, prevScore);
        mosRemoveEntry(m, existing);
        mosInsertSorted(m, score, member);
        return true; /* Existed, prevScore filled */
    }

    mosInsertSorted(m, score, member);
    return false; /* New insert */
}

bool multiOrderedSetSmallIncrBy(multiOrderedSetSmall *m, const databox *delta,
                                const databox *member, databox *result) {
    flexEntry *existing = mosFindMemberLinear(m->map, member);

    if (existing) {
        /* Get current score */
        databox currentScore;
        flexGetByType(existing, &currentScore);

        /* Add delta to current score */
        if (!mosDataboxAdd(&currentScore, delta, result)) {
            return false; /* Type mismatch or overflow */
        }

        /* Remove old, insert with new score */
        mosRemoveEntry(m, existing);
        mosInsertSorted(m, result, member);
        return true;
    }

    /* Member doesn't exist: use delta as initial score */
    *result = *delta;
    mosInsertSorted(m, delta, member);
    return true;
}

/* ====================================================================
 * Deletion
 * ==================================================================== */

bool multiOrderedSetSmallRemove(multiOrderedSetSmall *m,
                                const databox *member) {
    flexEntry *existing = mosFindMemberLinear(m->map, member);
    if (!existing) {
        return false;
    }

    mosRemoveEntry(m, existing);
    return true;
}

bool multiOrderedSetSmallRemoveGetScore(multiOrderedSetSmall *m,
                                        const databox *member, databox *score) {
    flexEntry *existing = mosFindMemberLinear(m->map, member);
    if (!existing) {
        return false;
    }

    flexGetByType(existing, score);
    mosRemoveEntry(m, existing);
    return true;
}

size_t multiOrderedSetSmallRemoveRangeByScore(multiOrderedSetSmall *m,
                                              const mosRangeSpec *range) {
    size_t removed = 0;
    flexEntry *entry = flexHead(m->map);

    while (entry) {
        databox score, member;
        flexEntry *memberEntry = flexNext(m->map, entry);
        if (!memberEntry) {
            break;
        }

        flexGetByType(entry, &score);
        flexGetByType(memberEntry, &member);

        /* Check if in range */
        if (mosScoreInRange(&score, &range->min, range->minExclusive,
                            &range->max, range->maxExclusive)) {
            /* Get next entry before removing */
            flexEntry *nextEntry = flexNext(m->map, memberEntry);
            mosRemoveEntry(m, entry);
            removed++;
            entry = nextEntry;
        } else {
            /* If score > max, we can stop (sorted order) */
            int cmp = databoxCompare(&score, &range->max);
            if (cmp > 0 || (cmp == 0 && range->maxExclusive)) {
                break;
            }
            entry = flexNext(m->map, memberEntry);
        }
    }

    return removed;
}

size_t multiOrderedSetSmallRemoveRangeByRank(multiOrderedSetSmall *m,
                                             int64_t start, int64_t stop) {
    size_t count = multiOrderedSetSmallCount(m);
    start = mosNormalizeRank(start, count);
    stop = mosNormalizeRank(stop, count);

    if (start < 0 || stop < 0 || start > stop) {
        return 0;
    }

    /* Delete entries from start to stop (inclusive) */
    size_t toRemove = (size_t)(stop - start + 1);
    size_t offset = (size_t)start * MOS_ELEMENTS_PER_ENTRY;

    flexEntry *entry = flexIndex(m->map, offset);
    if (entry) {
        flexDeleteCount(&m->map, &entry, toRemove * MOS_ELEMENTS_PER_ENTRY);
        SET_MIDDLE_FORCE(m);
    }

    return toRemove;
}

size_t multiOrderedSetSmallPopMin(multiOrderedSetSmall *m, size_t count,
                                  databox *members, databox *scores) {
    size_t total = multiOrderedSetSmallCount(m);
    if (count > total) {
        count = total;
    }

    for (size_t i = 0; i < count; i++) {
        flexEntry *head = flexHead(m->map);
        if (!head) {
            return i;
        }

        flexEntry *memberEntry = flexNext(m->map, head);
        if (!memberEntry) {
            return i;
        }

        flexGetByType(head, &scores[i]);
        flexGetByType(memberEntry, &members[i]);
        mosRemoveEntry(m, head);
    }

    return count;
}

size_t multiOrderedSetSmallPopMax(multiOrderedSetSmall *m, size_t count,
                                  databox *members, databox *scores) {
    size_t total = multiOrderedSetSmallCount(m);
    if (count > total) {
        count = total;
    }

    for (size_t i = 0; i < count; i++) {
        /* Get the last entry (2 elements from end) */
        size_t idx = flexCount(m->map);
        if (idx < MOS_ELEMENTS_PER_ENTRY) {
            return i;
        }

        flexEntry *scoreEntry = flexIndex(m->map, idx - MOS_ELEMENTS_PER_ENTRY);
        if (!scoreEntry) {
            return i;
        }

        flexEntry *memberEntry = flexNext(m->map, scoreEntry);
        if (!memberEntry) {
            return i;
        }

        flexGetByType(scoreEntry, &scores[i]);
        flexGetByType(memberEntry, &members[i]);
        mosRemoveEntry(m, scoreEntry);
    }

    return count;
}

/* ====================================================================
 * Lookup
 * ==================================================================== */

bool multiOrderedSetSmallExists(const multiOrderedSetSmall *m,
                                const databox *member) {
    return mosFindMemberLinear(m->map, member) != NULL;
}

bool multiOrderedSetSmallGetScore(const multiOrderedSetSmall *m,
                                  const databox *member, databox *score) {
    flexEntry *entry = mosFindMemberLinear(m->map, member);
    if (!entry) {
        return false;
    }

    flexGetByType(entry, score);
    return true;
}

int64_t multiOrderedSetSmallGetRank(const multiOrderedSetSmall *m,
                                    const databox *member) {
    int64_t rank = 0;
    flexEntry *entry = flexHead(m->map);

    while (entry) {
        flexEntry *memberEntry = flexNext(m->map, entry);
        if (!memberEntry) {
            break;
        }

        databox currentMember;
        flexGetByType(memberEntry, &currentMember);

        if (databoxCompare(&currentMember, member) == 0) {
            return rank;
        }

        rank++;
        entry = flexNext(m->map, memberEntry);
    }

    return -1; /* Not found */
}

int64_t multiOrderedSetSmallGetReverseRank(const multiOrderedSetSmall *m,
                                           const databox *member) {
    int64_t rank = multiOrderedSetSmallGetRank(m, member);
    if (rank < 0) {
        return -1;
    }

    size_t count = multiOrderedSetSmallCount(m);
    return (int64_t)(count - 1) - rank;
}

bool multiOrderedSetSmallGetByRank(const multiOrderedSetSmall *m, int64_t rank,
                                   databox *member, databox *score) {
    size_t count = multiOrderedSetSmallCount(m);
    rank = mosNormalizeRank(rank, count);
    if (rank < 0) {
        return false;
    }

    size_t offset = (size_t)rank * MOS_ELEMENTS_PER_ENTRY;
    flexEntry *entry = flexIndex(m->map, offset);
    if (!entry) {
        return false;
    }

    flexEntry *memberEntry = flexNext(m->map, entry);
    if (!memberEntry) {
        return false;
    }

    flexGetByType(entry, score);
    flexGetByType(memberEntry, member);
    return true;
}

/* ====================================================================
 * Range Queries
 * ==================================================================== */

size_t multiOrderedSetSmallCountByScore(const multiOrderedSetSmall *m,
                                        const mosRangeSpec *range) {
    size_t count = 0;
    flexEntry *entry = flexHead(m->map);

    while (entry) {
        databox score;
        flexGetByType(entry, &score);

        /* If score > max, we can stop */
        int cmp = databoxCompare(&score, &range->max);
        if (cmp > 0 || (cmp == 0 && range->maxExclusive)) {
            break;
        }

        if (mosScoreInRange(&score, &range->min, range->minExclusive,
                            &range->max, range->maxExclusive)) {
            count++;
        }

        flexEntry *memberEntry = flexNext(m->map, entry);
        if (!memberEntry) {
            break;
        }
        entry = flexNext(m->map, memberEntry);
    }

    return count;
}

/* ====================================================================
 * Iteration
 * ==================================================================== */

void multiOrderedSetSmallIteratorInit(const multiOrderedSetSmall *m,
                                      mosIterator *iter, bool forward) {
    iter->mos = (void *)m;
    iter->type = MOS_TYPE_SMALL;
    iter->forward = forward;
    iter->mapIndex = 0;

    if (forward) {
        iter->current = flexHead(m->map);
    } else {
        /* Start at last entry (last score position) */
        size_t count = flexCount(m->map);
        if (count >= MOS_ELEMENTS_PER_ENTRY) {
            iter->current = flexIndex(m->map, count - MOS_ELEMENTS_PER_ENTRY);
        } else {
            iter->current = NULL;
        }
    }

    iter->valid = (iter->current != NULL);
}

bool multiOrderedSetSmallIteratorInitAtScore(const multiOrderedSetSmall *m,
                                             mosIterator *iter,
                                             const databox *score,
                                             bool forward) {
    iter->mos = (void *)m;
    iter->type = MOS_TYPE_SMALL;
    iter->forward = forward;
    iter->mapIndex = 0;

    /* Find first entry with score >= given score */
    flexEntry *entry = flexHead(m->map);
    while (entry) {
        databox currentScore;
        flexGetByType(entry, &currentScore);

        if (databoxCompare(&currentScore, score) >= 0) {
            iter->current = entry;
            iter->valid = true;
            return true;
        }

        flexEntry *memberEntry = flexNext(m->map, entry);
        if (!memberEntry) {
            break;
        }
        entry = flexNext(m->map, memberEntry);
    }

    iter->current = NULL;
    iter->valid = false;
    return false;
}

bool multiOrderedSetSmallIteratorInitAtRank(const multiOrderedSetSmall *m,
                                            mosIterator *iter, int64_t rank,
                                            bool forward) {
    iter->mos = (void *)m;
    iter->type = MOS_TYPE_SMALL;
    iter->forward = forward;
    iter->mapIndex = 0;

    size_t count = multiOrderedSetSmallCount(m);
    rank = mosNormalizeRank(rank, count);
    if (rank < 0) {
        iter->current = NULL;
        iter->valid = false;
        return false;
    }

    size_t offset = (size_t)rank * MOS_ELEMENTS_PER_ENTRY;
    iter->current = flexIndex(m->map, offset);
    iter->valid = (iter->current != NULL);
    return iter->valid;
}

bool multiOrderedSetSmallIteratorNext(mosIterator *iter, databox *member,
                                      databox *score) {
    if (!iter->valid || !iter->current) {
        return false;
    }

    multiOrderedSetSmall *m = (multiOrderedSetSmall *)iter->mos;
    flexEntry *entry = (flexEntry *)iter->current;

    flexEntry *memberEntry = flexNext(m->map, entry);
    if (!memberEntry) {
        iter->valid = false;
        return false;
    }

    flexGetByType(entry, score);
    flexGetByType(memberEntry, member);

    /* Advance iterator */
    if (iter->forward) {
        iter->current = flexNext(m->map, memberEntry);
    } else {
        /* Move backwards: need to find previous score entry */
        size_t currentOffset = entry - m->map;
        if (currentOffset >= MOS_ELEMENTS_PER_ENTRY * 2) {
            /* There's a previous entry */
            iter->current =
                flexIndex(m->map, currentOffset - MOS_ELEMENTS_PER_ENTRY);
        } else {
            iter->current = NULL;
        }
    }

    iter->valid = (iter->current != NULL);
    return true;
}

/* ====================================================================
 * First / Last
 * ==================================================================== */

bool multiOrderedSetSmallFirst(const multiOrderedSetSmall *m, databox *member,
                               databox *score) {
    flexEntry *entry = flexHead(m->map);
    if (!entry) {
        return false;
    }

    flexEntry *memberEntry = flexNext(m->map, entry);
    if (!memberEntry) {
        return false;
    }

    flexGetByType(entry, score);
    flexGetByType(memberEntry, member);
    return true;
}

bool multiOrderedSetSmallLast(const multiOrderedSetSmall *m, databox *member,
                              databox *score) {
    size_t count = flexCount(m->map);
    if (count < MOS_ELEMENTS_PER_ENTRY) {
        return false;
    }

    flexEntry *entry = flexIndex(m->map, count - MOS_ELEMENTS_PER_ENTRY);
    if (!entry) {
        return false;
    }

    flexEntry *memberEntry = flexNext(m->map, entry);
    if (!memberEntry) {
        return false;
    }

    flexGetByType(entry, score);
    flexGetByType(memberEntry, member);
    return true;
}

/* ====================================================================
 * Random
 * ==================================================================== */

size_t multiOrderedSetSmallRandomMembers(const multiOrderedSetSmall *m,
                                         int64_t count, databox *members,
                                         databox *scores) {
    size_t total = multiOrderedSetSmallCount(m);
    if (total == 0) {
        return 0;
    }

    bool allowDuplicates = (count < 0);
    size_t absCount = (size_t)(count < 0 ? -count : count);
    size_t retrieved = 0;

    if (allowDuplicates) {
        /* With duplicates: simple random selection */
        for (size_t i = 0; i < absCount; i++) {
            size_t idx = mosRandom() % total;
            if (multiOrderedSetSmallGetByRank(m, (int64_t)idx, &members[i],
                                              &scores[i])) {
                retrieved++;
            }
        }
    } else {
        /* Without duplicates: Fisher-Yates on indices */
        if (absCount > total) {
            absCount = total;
        }

        /* For small counts, use simple rejection sampling */
        if (absCount <= total / 4) {
            /* Use a simple set to track selected indices */
            uint8_t *selected = zcalloc(1, (total + 7) / 8);
            while (retrieved < absCount) {
                size_t idx = mosRandom() % total;
                size_t byte = idx / 8;
                size_t bit = idx % 8;
                if (!(selected[byte] & (1 << bit))) {
                    selected[byte] |= (1 << bit);
                    if (multiOrderedSetSmallGetByRank(m, (int64_t)idx,
                                                      &members[retrieved],
                                                      &scores[retrieved])) {
                        retrieved++;
                    }
                }
            }
            zfree(selected);
        } else {
            /* For larger counts, iterate and select */
            for (size_t i = 0; i < total && retrieved < absCount; i++) {
                /* Probability of selection */
                size_t remaining = total - i;
                size_t needed = absCount - retrieved;
                if ((mosRandom() % remaining) < needed) {
                    if (multiOrderedSetSmallGetByRank(m, (int64_t)i,
                                                      &members[retrieved],
                                                      &scores[retrieved])) {
                        retrieved++;
                    }
                }
            }
        }
    }

    return retrieved;
}

/* ====================================================================
 * Debugging
 * ==================================================================== */

#ifdef DATAKIT_TEST
#include "ctest.h"

void multiOrderedSetSmallRepr(const multiOrderedSetSmall *m) {
    printf("multiOrderedSetSmall {\n");
    printf("  count: %zu\n", multiOrderedSetSmallCount(m));
    printf("  bytes: %zu\n", multiOrderedSetSmallBytes(m));
    printf("  entries:\n");

    flexEntry *entry = flexHead(m->map);
    int idx = 0;
    while (entry) {
        flexEntry *memberEntry = flexNext(m->map, entry);
        if (!memberEntry) {
            break;
        }

        databox score, member;
        flexGetByType(entry, &score);
        flexGetByType(memberEntry, &member);

        printf("    [%d] ", idx);
        databoxReprSay("score=", &score);
        databoxReprSay(" member=", &member);
        printf("\n");

        idx++;
        entry = flexNext(m->map, memberEntry);
    }
    printf("}\n");
}

int multiOrderedSetSmallTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    TEST("multiOrderedSetSmall — create and free") {
        multiOrderedSetSmall *mos = multiOrderedSetSmallNew();
        if (!mos) {
            ERRR("Failed to create multiOrderedSetSmall");
        }
        if (multiOrderedSetSmallCount(mos) != 0) {
            ERRR("New set should be empty");
        }
        multiOrderedSetSmallFree(mos);
    }

    TEST("multiOrderedSetSmall — add and lookup") {
        multiOrderedSetSmall *mos = multiOrderedSetSmallNew();

        databox score1 = {.type = DATABOX_SIGNED_64, .data.i = 100};
        databox member1 = databoxNewBytesAllowEmbed("member1", 7);

        databox score2 = {.type = DATABOX_SIGNED_64, .data.i = 50};
        databox member2 = databoxNewBytesAllowEmbed("member2", 7);

        databox score3 = {.type = DATABOX_SIGNED_64, .data.i = 75};
        databox member3 = databoxNewBytesAllowEmbed("member3", 7);

        /* Add entries */
        if (multiOrderedSetSmallAdd(mos, &score1, &member1)) {
            ERRR("First add should return false (new)");
        }
        if (multiOrderedSetSmallAdd(mos, &score2, &member2)) {
            ERRR("Second add should return false (new)");
        }
        if (multiOrderedSetSmallAdd(mos, &score3, &member3)) {
            ERRR("Third add should return false (new)");
        }

        if (multiOrderedSetSmallCount(mos) != 3) {
            ERR("Count should be 3, got %zu", multiOrderedSetSmallCount(mos));
        }

        /* Check ordering (should be: member2=50, member3=75, member1=100) */
        databox m, s;
        if (!multiOrderedSetSmallGetByRank(mos, 0, &m, &s)) {
            ERRR("GetByRank(0) failed");
        }
        if (s.data.i != 50) {
            ERR("Rank 0 should have score 50, got %ld", s.data.i);
        }

        if (!multiOrderedSetSmallGetByRank(mos, 1, &m, &s)) {
            ERRR("GetByRank(1) failed");
        }
        if (s.data.i != 75) {
            ERR("Rank 1 should have score 75, got %ld", s.data.i);
        }

        if (!multiOrderedSetSmallGetByRank(mos, 2, &m, &s)) {
            ERRR("GetByRank(2) failed");
        }
        if (s.data.i != 100) {
            ERR("Rank 2 should have score 100, got %ld", s.data.i);
        }

        /* Test GetScore */
        databox foundScore;
        if (!multiOrderedSetSmallGetScore(mos, &member1, &foundScore)) {
            ERRR("GetScore for member1 failed");
        }
        if (foundScore.data.i != 100) {
            ERR("member1 score should be 100, got %ld", foundScore.data.i);
        }

        /* Test GetRank */
        int64_t rank = multiOrderedSetSmallGetRank(mos, &member2);
        if (rank != 0) {
            ERR("member2 rank should be 0, got %ld", rank);
        }

        rank = multiOrderedSetSmallGetRank(mos, &member1);
        if (rank != 2) {
            ERR("member1 rank should be 2, got %ld", rank);
        }

        multiOrderedSetSmallFree(mos);
    }

    TEST("multiOrderedSetSmall — update score") {
        multiOrderedSetSmall *mos = multiOrderedSetSmallNew();

        databox score1 = {.type = DATABOX_SIGNED_64, .data.i = 100};
        databox member1 = databoxNewBytesAllowEmbed("member1", 7);

        multiOrderedSetSmallAdd(mos, &score1, &member1);

        /* Update to new score */
        databox newScore = {.type = DATABOX_SIGNED_64, .data.i = 200};
        if (!multiOrderedSetSmallAdd(mos, &newScore, &member1)) {
            ERRR("Update should return true (existed)");
        }

        if (multiOrderedSetSmallCount(mos) != 1) {
            ERRR("Count should still be 1");
        }

        databox foundScore;
        multiOrderedSetSmallGetScore(mos, &member1, &foundScore);
        if (foundScore.data.i != 200) {
            ERRR("Score should be updated to 200");
        }

        multiOrderedSetSmallFree(mos);
    }

    TEST("multiOrderedSetSmall — remove") {
        multiOrderedSetSmall *mos = multiOrderedSetSmallNew();

        databox score1 = {.type = DATABOX_SIGNED_64, .data.i = 100};
        databox member1 = databoxNewBytesAllowEmbed("member1", 7);

        databox score2 = {.type = DATABOX_SIGNED_64, .data.i = 200};
        databox member2 = databoxNewBytesAllowEmbed("member2", 7);

        multiOrderedSetSmallAdd(mos, &score1, &member1);
        multiOrderedSetSmallAdd(mos, &score2, &member2);

        if (!multiOrderedSetSmallRemove(mos, &member1)) {
            ERRR("Remove should return true");
        }

        if (multiOrderedSetSmallCount(mos) != 1) {
            ERRR("Count should be 1 after remove");
        }

        if (multiOrderedSetSmallExists(mos, &member1)) {
            ERRR("member1 should not exist after remove");
        }

        if (!multiOrderedSetSmallExists(mos, &member2)) {
            ERRR("member2 should still exist");
        }

        multiOrderedSetSmallFree(mos);
    }

    TEST("multiOrderedSetSmall — pop min/max") {
        multiOrderedSetSmall *mos = multiOrderedSetSmallNew();

        for (int i = 0; i < 5; i++) {
            databox score = {.type = DATABOX_SIGNED_64, .data.i = i * 10};
            char buf[16];
            snprintf(buf, sizeof(buf), "m%d", i);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            multiOrderedSetSmallAdd(mos, &score, &member);
        }

        if (multiOrderedSetSmallCount(mos) != 5) {
            ERRR("Count should be 5");
        }

        /* Pop min */
        databox members[2], scores[2];
        size_t popped = multiOrderedSetSmallPopMin(mos, 2, members, scores);
        if (popped != 2) {
            ERR("Should pop 2, got %zu", popped);
        }
        if (scores[0].data.i != 0) {
            ERRR("First popped score should be 0");
        }
        if (scores[1].data.i != 10) {
            ERRR("Second popped score should be 10");
        }

        if (multiOrderedSetSmallCount(mos) != 3) {
            ERRR("Count should be 3 after pop min");
        }

        /* Pop max */
        popped = multiOrderedSetSmallPopMax(mos, 1, members, scores);
        if (popped != 1) {
            ERR("Should pop 1, got %zu", popped);
        }
        if (scores[0].data.i != 40) {
            ERR("Popped score should be 40 (max), got %ld", scores[0].data.i);
        }

        if (multiOrderedSetSmallCount(mos) != 2) {
            ERRR("Count should be 2 after pop max");
        }

        multiOrderedSetSmallFree(mos);
    }

    TEST("multiOrderedSetSmall — iteration") {
        multiOrderedSetSmall *mos = multiOrderedSetSmallNew();

        for (int i = 0; i < 5; i++) {
            databox score = {.type = DATABOX_SIGNED_64, .data.i = i * 10};
            char buf[16];
            snprintf(buf, sizeof(buf), "m%d", i);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            multiOrderedSetSmallAdd(mos, &score, &member);
        }

        /* Forward iteration */
        mosIterator iter;
        multiOrderedSetSmallIteratorInit(mos, &iter, true);

        int idx = 0;
        databox m, s;
        while (multiOrderedSetSmallIteratorNext(&iter, &m, &s)) {
            if (s.data.i != idx * 10) {
                ERR("Forward iter[%d] score mismatch: expected %d, got %ld",
                    idx, idx * 10, s.data.i);
            }
            idx++;
        }

        if (idx != 5) {
            ERR("Forward iteration should visit 5 entries, got %d", idx);
        }

        multiOrderedSetSmallFree(mos);
    }

    TEST("multiOrderedSetSmall — copy") {
        multiOrderedSetSmall *mos = multiOrderedSetSmallNew();

        databox score = {.type = DATABOX_SIGNED_64, .data.i = 42};
        databox member = databoxNewBytesAllowEmbed("test", 4);
        multiOrderedSetSmallAdd(mos, &score, &member);

        multiOrderedSetSmall *copy = multiOrderedSetSmallCopy(mos);
        if (multiOrderedSetSmallCount(copy) != 1) {
            ERRR("Copy should have count 1");
        }

        databox foundScore;
        if (!multiOrderedSetSmallGetScore(copy, &member, &foundScore)) {
            ERRR("Copy should contain member");
        }

        /* Modify original */
        databox newScore = {.type = DATABOX_SIGNED_64, .data.i = 100};
        multiOrderedSetSmallAdd(mos, &newScore, &member);

        /* Copy should be unchanged */
        multiOrderedSetSmallGetScore(copy, &member, &foundScore);
        if (foundScore.data.i != 42) {
            ERRR("Copy should be independent of original");
        }

        multiOrderedSetSmallFree(mos);
        multiOrderedSetSmallFree(copy);
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
