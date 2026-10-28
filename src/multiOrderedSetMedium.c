#include "multiOrderedSetMedium.h"
#include "multiOrderedSetMediumInternal.h"
#include "multiOrderedSetSmallInternal.h"

#include "str.h" /* for random functions */

/* ====================================================================
 * Random Number Generator
 * ==================================================================== */
/* xorshift64star state for RandomMembers */
static uint64_t mosRandomState = 0x98765432FEDCBA01ULL;

/* ====================================================================
 * Databox Arithmetic Helper
 * ==================================================================== */

/* ====================================================================
 * Macros for middle management
 * ==================================================================== */
#define GET_MIDDLE(m, idx) (flexEntry *)((m)->map[idx] + (m)->middle[idx])
#define SET_MIDDLE(m, idx, mid)                                                \
    do {                                                                       \
        (m)->middle[idx] = (uint32_t)((mid) - (m)->map[idx]);                  \
    } while (0)
#define SET_MIDDLE_FORCE(m, idx)                                               \
    do {                                                                       \
        SET_MIDDLE(m, idx, flexMiddle((m)->map[idx], MOS_ELEMENTS_PER_ENTRY)); \
    } while (0)

/* ====================================================================
 * Internal Helpers
 * ==================================================================== */

/* Get count for a single map */
static size_t mapCount(const flex *map) {
    return flexCount(map) / MOS_ELEMENTS_PER_ENTRY;
}

/* Determine which map a score belongs to based on split point */
static int getMapIndexForScore(const multiOrderedSetMedium *m,
                               const databox *score) {
    /* Get the maximum score in map[0] to determine split */
    size_t count0 = mapCount(m->map[0]);
    if (count0 == 0) {
        /* map[0] is empty (pops or removals drained it): a score below
         * map[1]'s lowest goes there, any other to map[1], so every
         * map[1] score stays above map[0]'s highest */
        if (mapCount(m->map[1]) == 0) {
            return 0;
        }

        databox lowest1;
        databoxWide lowest1Wide;
        flexGetByTypeInto(flexHead(m->map[1]), &lowest1, &lowest1Wide);
        return databoxCompare(score, &lowest1) < 0 ? 0 : 1;
    }

    /* Get last entry in map[0] */
    size_t lastIdx = (count0 - 1) * MOS_ELEMENTS_PER_ENTRY;
    flexEntry *lastEntry = flexIndex(m->map[0], lastIdx);
    if (!lastEntry) {
        return 0;
    }

    databox maxScore0;
    databoxWide maxScore0Wide;
    flexGetByTypeInto(lastEntry, &maxScore0, &maxScore0Wide);

    /* If score <= max of map[0], it goes in map[0], else map[1] */
    return (databoxCompare(score, &maxScore0) <= 0) ? 0 : 1;
}

/* Find member in a specific map (linear scan), returns entry pointing to score
 */
static flexEntry *findMemberInMap(const flex *map, const databox *member) {
    return mosFindMemberLinear(map, member);
}

/* Find member across all maps, returns map index and entry */
static flexEntry *findMemberAnyMap(const multiOrderedSetMedium *m,
                                   const databox *member, int *mapIdx) {
    for (int i = 0; i < 2; i++) {
        flexEntry *entry = findMemberInMap(m->map[i], member);
        if (entry) {
            *mapIdx = i;
            return entry;
        }
    }
    *mapIdx = -1;
    return nullptr;
}

/* Insert (score, member) into appropriate map */
static void insertIntoMap(multiOrderedSetMedium *m, int mapIdx,
                          const databox *score, const databox *member) {
    const databox *elements[2] = {score, member};
    flexEntry *middle = GET_MIDDLE(m, mapIdx);

    flexInsertByTypeSortedWithMiddleMultiDirect(
        &m->map[mapIdx], MOS_ELEMENTS_PER_ENTRY, elements, &middle);
    SET_MIDDLE(m, mapIdx, middle);
}

/* Remove entry from specific map */
static void removeFromMap(multiOrderedSetMedium *m, int mapIdx,
                          flexEntry *entry) {
    flexDeleteCount(&m->map[mapIdx], &entry, MOS_ELEMENTS_PER_ENTRY);
    SET_MIDDLE_FORCE(m, mapIdx);
}

/* Whether the row at 'entry' in map 'mapIdx' may take 'score' and stay in
 * that map: every map[1] score stays above map[0]'s highest */
static bool scoreKeepsMap(const multiOrderedSetMedium *m, int mapIdx,
                          flexEntry *entry, const databox *score) {
    if (mapIdx == 1) {
        return getMapIndexForScore(m, score) == 1;
    }

    /* In map[0] only the last row can reach map[1]'s lowest score (the
     * in-map order check bounds every other row by the row after it) */
    if (flexNext(m->map[0], flexNext(m->map[0], entry)) ||
        mapCount(m->map[1]) == 0) {
        return true;
    }

    databox lowest1;
    databoxWide lowest1Wide;
    flexGetByTypeInto(flexHead(m->map[1]), &lowest1, &lowest1Wide);
    return databoxCompare(score, &lowest1) < 0;
}

/* Gives the found row at 'entry' in map 'mapIdx' the score 'score':
 * rewritten in place when the row keeps its place and its map, else
 * removed and reinserted where the score routes it */
static void setRowScore(multiOrderedSetMedium *m, int mapIdx, flexEntry *entry,
                        const databox *score, const databox *member) {
    const size_t bytes = flexBytes(m->map[mapIdx]);
    if (scoreKeepsMap(m, mapIdx, entry, score) &&
        mosRewriteScoreInPlace(&m->map[mapIdx], entry, score, member)) {
        if (flexBytes(m->map[mapIdx]) != bytes) {
            SET_MIDDLE_FORCE(m, mapIdx);
        }

        return;
    }

    removeFromMap(m, mapIdx, entry);
    insertIntoMap(m, getMapIndexForScore(m, score), score, member);
}

/* ====================================================================
 * Creation / Destruction
 * ==================================================================== */

multiOrderedSetMedium *multiOrderedSetMediumNew(void) {
    /* @own: caller; free=multiOrderedSetMediumFree */
    multiOrderedSetMedium *m = zcalloc(1, sizeof(*m));
    /* @own: container; free=multiOrderedSetMediumFree */
    m->map[0] = flexNew();
    /* @own: container; free=multiOrderedSetMediumFree */
    m->map[1] = flexNew();
    m->middle[0] = FLEX_EMPTY_SIZE;
    m->middle[1] = FLEX_EMPTY_SIZE;
    m->flags = 0;
    return m;
}

multiOrderedSetMedium *multiOrderedSetMediumNewFromSmall(flex *map,
                                                         uint32_t middle) {
    /* @own: caller; free=multiOrderedSetMediumFree */
    multiOrderedSetMedium *m = zcalloc(1, sizeof(*m));

    /* Split the small map into two halves */
    size_t count = flexCount(map) / MOS_ELEMENTS_PER_ENTRY;
    size_t splitPoint = count / 2;

    if (splitPoint == 0) {
        /* Too few entries to split meaningfully */
        m->map[0] = map;
        m->middle[0] = middle;
        /* @own: container; free=multiOrderedSetMediumFree */
        m->map[1] = flexNew();
        m->middle[1] = FLEX_EMPTY_SIZE;
    } else {
        /* Create two new maps */
        /* @own: container; free=multiOrderedSetMediumFree */
        m->map[0] = flexNew();
        /* @own: container; free=multiOrderedSetMediumFree */
        m->map[1] = flexNew();

        /* Initialize middles to proper head position before first insert */
        SET_MIDDLE_FORCE(m, 0);
        SET_MIDDLE_FORCE(m, 1);

        /* Copy entries to appropriate maps: the first half to map[0], and
         * past it any row with the score of the row before (map[1] must
         * hold only scores above map[0]'s: routing by score alone puts a
         * later member of a shared score in map[0], out of member order
         * with the ones in map[1]) */
        flexEntry *entry = flexHead(map);
        flexEntry *prevScoreEntry = nullptr;
        bool upper = false;
        size_t idx = 0;
        while (entry) {
            flexEntry *memberEntry = flexNext(map, entry);
            if (!memberEntry) {
                break;
            }

            databox score = {};
            databox member = {};
            databoxWide scoreWide, memberWide;
            flexGetByTypeInto(entry, &score, &scoreWide);
            flexGetByTypeInto(memberEntry, &member, &memberWide);

            if (!upper && idx >= splitPoint) {
                databox prev = {};
                databoxWide prevWide;
                flexGetByTypeInto(prevScoreEntry, &prev, &prevWide);
                upper = databoxCompare(&score, &prev) != 0;
            }

            insertIntoMap(m, upper ? 1 : 0, &score, &member);

            idx++;
            prevScoreEntry = entry;
            entry = flexNext(map, memberEntry);
        }

        /* Free the original map */
        flexFree(map);
    }

    return m;
}

multiOrderedSetMedium *
multiOrderedSetMediumCopy(const multiOrderedSetMedium *m) {
    /* @own: caller; free=multiOrderedSetMediumFree */
    multiOrderedSetMedium *copy = zcalloc(1, sizeof(*copy));
    /* @own: container; free=multiOrderedSetMediumFree */
    copy->map[0] = flexDuplicate(m->map[0]);
    /* @own: container; free=multiOrderedSetMediumFree */
    copy->map[1] = flexDuplicate(m->map[1]);
    copy->middle[0] = m->middle[0];
    copy->middle[1] = m->middle[1];
    copy->flags = m->flags;
    return copy;
}

void multiOrderedSetMediumFree(multiOrderedSetMedium *m) {
    if (m) {
        flexFree(m->map[0]);
        flexFree(m->map[1]);
        zfree(m);
    }
}

void multiOrderedSetMediumReset(multiOrderedSetMedium *m) {
    flexFree(m->map[0]);
    flexFree(m->map[1]);
    /* @own: container; free=multiOrderedSetMediumFree */
    m->map[0] = flexNew();
    /* @own: container; free=multiOrderedSetMediumFree */
    m->map[1] = flexNew();
    m->middle[0] = FLEX_EMPTY_SIZE;
    m->middle[1] = FLEX_EMPTY_SIZE;
}

/* ====================================================================
 * Statistics
 * ==================================================================== */

size_t multiOrderedSetMediumCount(const multiOrderedSetMedium *m) {
    return mapCount(m->map[0]) + mapCount(m->map[1]);
}

size_t multiOrderedSetMediumBytes(const multiOrderedSetMedium *m) {
    return flexBytes(m->map[0]) + flexBytes(m->map[1]);
}

/* ====================================================================
 * Insertion / Update
 * ==================================================================== */

bool multiOrderedSetMediumAdd(multiOrderedSetMedium *m, const databox *score,
                              const databox *member) {
    int existingMap;
    flexEntry *existing = findMemberAnyMap(m, member, &existingMap);

    if (existing) {
        setRowScore(m, existingMap, existing, score, member);
        return true;
    }

    /* Insert into appropriate map based on score */
    int targetMap = getMapIndexForScore(m, score);
    insertIntoMap(m, targetMap, score, member);
    return false;
}

bool multiOrderedSetMediumAddNX(multiOrderedSetMedium *m, const databox *score,
                                const databox *member) {
    int existingMap;
    if (findMemberAnyMap(m, member, &existingMap)) {
        return false;
    }

    int targetMap = getMapIndexForScore(m, score);
    insertIntoMap(m, targetMap, score, member);
    return true;
}

bool multiOrderedSetMediumAddXX(multiOrderedSetMedium *m, const databox *score,
                                const databox *member) {
    int existingMap;
    flexEntry *existing = findMemberAnyMap(m, member, &existingMap);

    if (!existing) {
        return false;
    }

    setRowScore(m, existingMap, existing, score, member);
    return true;
}

bool multiOrderedSetMediumAddGetPrevious(multiOrderedSetMedium *m,
                                         const databox *score,
                                         const databox *member,
                                         databox *prevScore) {
    int existingMap;
    flexEntry *existing = findMemberAnyMap(m, member, &existingMap);

    if (existing) {
        mosReadOwned(existing, prevScore);
        setRowScore(m, existingMap, existing, score, member);
        return true;
    }

    int targetMap = getMapIndexForScore(m, score);
    insertIntoMap(m, targetMap, score, member);
    return false;
}

bool multiOrderedSetMediumCombine(multiOrderedSetMedium *m,
                                  const databox *operand, const databox *member,
                                  mosCombineOp op, databox *result) {
    int existingMap;
    flexEntry *existing = findMemberAnyMap(m, member, &existingMap);

    if (existing) {
        databox currentScore;
        databoxWide currentScoreWide;
        flexGetByTypeInto(existing, &currentScore, &currentScoreWide);

        if (!mosCombine(&currentScore, operand, op, result)) {
            return false;
        }

        setRowScore(m, existingMap, existing, result, member);
        return true;
    }

    *result = *operand;
    int targetMap = getMapIndexForScore(m, operand);
    insertIntoMap(m, targetMap, operand, member);
    return true;
}

/* ====================================================================
 * Deletion
 * ==================================================================== */

bool multiOrderedSetMediumRemoveIf(multiOrderedSetMedium *m,
                                   const databox *member, flexProceed *proceed,
                                   void *ctx) {
    int existingMap;
    flexEntry *existing = findMemberAnyMap(m, member, &existingMap);
    if (!existing || (proceed && !proceed(ctx))) {
        return false;
    }

    removeFromMap(m, existingMap, existing);
    return true;
}

bool multiOrderedSetMediumRemove(multiOrderedSetMedium *m,
                                 const databox *member) {
    return multiOrderedSetMediumRemoveIf(m, member, nullptr, nullptr);
}

bool multiOrderedSetMediumRemoveGetScore(multiOrderedSetMedium *m,
                                         const databox *member,
                                         databox *score) {
    int existingMap;
    flexEntry *existing = findMemberAnyMap(m, member, &existingMap);

    if (!existing) {
        return false;
    }

    mosReadOwned(existing, score);
    removeFromMap(m, existingMap, existing);
    return true;
}

size_t multiOrderedSetMediumRemoveRangeByScore(multiOrderedSetMedium *m,
                                               const mosRangeSpec *range) {
    size_t removed = 0;

    /* Process both maps */
    for (int mapIdx = 0; mapIdx < 2; mapIdx++) {
        flexEntry *entry = flexHeadIfAny(m->map[mapIdx]);

        while (entry) {
            flexEntry *memberEntry = flexNext(m->map[mapIdx], entry);
            if (!memberEntry) {
                break;
            }

            databox score;
            databoxWide scoreWide;
            flexGetByTypeInto(entry, &score, &scoreWide);

            if (mosScoreInRange(&score, &range->min, range->minExclusive,
                                &range->max, range->maxExclusive)) {
                /* Removing shifts the following pairs down, may move the
                 * flex and may shrink its header: the next pair then
                 * starts where this one did, measured from the head */
                const size_t offset =
                    (size_t)(entry - flexHead(m->map[mapIdx]));
                const bool hasNext =
                    flexNext(m->map[mapIdx], memberEntry) != nullptr;
                removeFromMap(m, mapIdx, entry);
                removed++;
                entry = hasNext ? flexHead(m->map[mapIdx]) + offset : nullptr;
            } else {
                int cmp = databoxCompare(&score, &range->max);
                if (cmp > 0 || (cmp == 0 && range->maxExclusive)) {
                    break;
                }
                entry = flexNext(m->map[mapIdx], memberEntry);
            }
        }
    }

    return removed;
}

size_t multiOrderedSetMediumRemoveRangeByRank(multiOrderedSetMedium *m,
                                              int64_t start, int64_t stop) {
    size_t count = multiOrderedSetMediumCount(m);
    if (!mosClampRankRange(&start, &stop, count)) {
        return 0;
    }

    size_t removed = 0;
    size_t count0 = mapCount(m->map[0]);

    /* Calculate ranges for each map */
    int64_t start0 = start;
    int64_t stop0 = (stop < (int64_t)count0) ? stop : (int64_t)count0 - 1;

    int64_t start1 = start - (int64_t)count0;
    int64_t stop1 = stop - (int64_t)count0;

    /* Remove from map[0] if applicable */
    if (start0 <= stop0 && start0 >= 0 && start0 < (int64_t)count0) {
        size_t toRemove = (size_t)(stop0 - start0 + 1);
        size_t offset = (size_t)start0 * MOS_ELEMENTS_PER_ENTRY;
        flexEntry *entry = flexIndex(m->map[0], offset);
        if (entry) {
            flexDeleteCount(&m->map[0], &entry,
                            toRemove * MOS_ELEMENTS_PER_ENTRY);
            SET_MIDDLE_FORCE(m, 0);
            removed += toRemove;
        }
    }

    /* Remove from map[1] if applicable */
    size_t count1 = mapCount(m->map[1]);
    if (start1 < 0) {
        start1 = 0;
    }
    if (stop1 >= 0 && start1 < (int64_t)count1) {
        if (stop1 >= (int64_t)count1) {
            stop1 = (int64_t)count1 - 1;
        }
        size_t toRemove = (size_t)(stop1 - start1 + 1);
        size_t offset = (size_t)start1 * MOS_ELEMENTS_PER_ENTRY;
        flexEntry *entry = flexIndex(m->map[1], offset);
        if (entry) {
            flexDeleteCount(&m->map[1], &entry,
                            toRemove * MOS_ELEMENTS_PER_ENTRY);
            SET_MIDDLE_FORCE(m, 1);
            removed += toRemove;
        }
    }

    return removed;
}

size_t multiOrderedSetMediumPopMin(multiOrderedSetMedium *m, size_t count,
                                   databox *members, databox *scores) {
    size_t popped = 0;

    while (popped < count) {
        /* Pop from map[0] first (lower scores) */
        flexEntry *head = flexHeadIfAny(m->map[0]);
        if (head) {
            flexEntry *memberEntry = flexNext(m->map[0], head);
            if (memberEntry) {
                mosReadOwned(head, &scores[popped]);
                mosReadOwned(memberEntry, &members[popped]);
                removeFromMap(m, 0, head);
                popped++;
                continue;
            }
        }

        /* map[0] empty, try map[1] */
        head = flexHeadIfAny(m->map[1]);
        if (head) {
            flexEntry *memberEntry = flexNext(m->map[1], head);
            if (memberEntry) {
                mosReadOwned(head, &scores[popped]);
                mosReadOwned(memberEntry, &members[popped]);
                removeFromMap(m, 1, head);
                popped++;
                continue;
            }
        }

        break; /* Both maps empty */
    }

    return popped;
}

size_t multiOrderedSetMediumPopMax(multiOrderedSetMedium *m, size_t count,
                                   databox *members, databox *scores) {
    size_t popped = 0;

    while (popped < count) {
        /* Pop from map[1] first (higher scores) */
        size_t count1 = flexCount(m->map[1]);
        if (count1 >= MOS_ELEMENTS_PER_ENTRY) {
            flexEntry *entry =
                flexIndex(m->map[1], count1 - MOS_ELEMENTS_PER_ENTRY);
            if (entry) {
                flexEntry *memberEntry = flexNext(m->map[1], entry);
                if (memberEntry) {
                    mosReadOwned(entry, &scores[popped]);
                    mosReadOwned(memberEntry, &members[popped]);
                    removeFromMap(m, 1, entry);
                    popped++;
                    continue;
                }
            }
        }

        /* map[1] empty, try map[0] */
        size_t count0 = flexCount(m->map[0]);
        if (count0 >= MOS_ELEMENTS_PER_ENTRY) {
            flexEntry *entry =
                flexIndex(m->map[0], count0 - MOS_ELEMENTS_PER_ENTRY);
            if (entry) {
                flexEntry *memberEntry = flexNext(m->map[0], entry);
                if (memberEntry) {
                    mosReadOwned(entry, &scores[popped]);
                    mosReadOwned(memberEntry, &members[popped]);
                    removeFromMap(m, 0, entry);
                    popped++;
                    continue;
                }
            }
        }

        break;
    }

    return popped;
}

/* ====================================================================
 * Lookup
 * ==================================================================== */

bool multiOrderedSetMediumExists(const multiOrderedSetMedium *m,
                                 const databox *member) {
    int mapIdx;
    return findMemberAnyMap(m, member, &mapIdx) != nullptr;
}

bool multiOrderedSetMediumGetScore(const multiOrderedSetMedium *m,
                                   const databox *member, databox *score) {
    int mapIdx;
    flexEntry *entry = findMemberAnyMap(m, member, &mapIdx);

    if (!entry) {
        return false;
    }

    flexGetByType(entry, score);
    return true;
}

int64_t multiOrderedSetMediumGetRank(const multiOrderedSetMedium *m,
                                     const databox *member) {
    /* map[0]'s rows rank first */
    size_t row;
    if (mosFindMemberLinearRow(m->map[0], member, &row)) {
        return (int64_t)row;
    }

    if (mosFindMemberLinearRow(m->map[1], member, &row)) {
        return (int64_t)(mapCount(m->map[0]) + row);
    }

    return -1;
}

int64_t multiOrderedSetMediumGetReverseRank(const multiOrderedSetMedium *m,
                                            const databox *member) {
    int64_t rank = multiOrderedSetMediumGetRank(m, member);
    if (rank < 0) {
        return -1;
    }

    size_t count = multiOrderedSetMediumCount(m);
    return (int64_t)(count - 1) - rank;
}

bool multiOrderedSetMediumGetByRank(const multiOrderedSetMedium *m,
                                    int64_t rank, databox *member,
                                    databox *score) {
    size_t count = multiOrderedSetMediumCount(m);
    rank = mosNormalizeRank(rank, count);
    if (rank < 0) {
        return false;
    }

    size_t count0 = mapCount(m->map[0]);

    int mapIdx;
    size_t localRank;
    if ((size_t)rank < count0) {
        mapIdx = 0;
        localRank = (size_t)rank;
    } else {
        mapIdx = 1;
        localRank = (size_t)rank - count0;
    }

    size_t offset = localRank * MOS_ELEMENTS_PER_ENTRY;
    flexEntry *entry = flexIndex(m->map[mapIdx], offset);
    if (!entry) {
        return false;
    }

    flexEntry *memberEntry = flexNext(m->map[mapIdx], entry);
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

size_t multiOrderedSetMediumCountByScore(const multiOrderedSetMedium *m,
                                         const mosRangeSpec *range) {
    size_t count = 0;

    for (int mapIdx = 0; mapIdx < 2; mapIdx++) {
        flexEntry *entry = flexHeadIfAny(m->map[mapIdx]);

        while (entry) {
            databox score;
            databoxWide scoreWide;
            flexGetByTypeInto(entry, &score, &scoreWide);

            int cmp = databoxCompare(&score, &range->max);
            if (cmp > 0 || (cmp == 0 && range->maxExclusive)) {
                break;
            }

            if (mosScoreInRange(&score, &range->min, range->minExclusive,
                                &range->max, range->maxExclusive)) {
                count++;
            }

            flexEntry *memberEntry = flexNext(m->map[mapIdx], entry);
            if (!memberEntry) {
                break;
            }
            entry = flexNext(m->map[mapIdx], memberEntry);
        }
    }

    return count;
}

/* ====================================================================
 * Iteration
 * ==================================================================== */

void multiOrderedSetMediumIteratorInit(const multiOrderedSetMedium *m,
                                       mosIterator *iter, bool forward) {
    iter->mos = (void *)m;
    iter->type = MOS_TYPE_MEDIUM;
    iter->forward = forward;

    if (forward) {
        iter->mapIndex = 0;
        iter->current = flexHeadIfAny(m->map[0]);
        if (!iter->current) {
            iter->mapIndex = 1;
            iter->current = flexHeadIfAny(m->map[1]);
        }
    } else {
        iter->mapIndex = 1;
        size_t count1 = flexCount(m->map[1]);
        if (count1 >= MOS_ELEMENTS_PER_ENTRY) {
            iter->current =
                flexIndex(m->map[1], count1 - MOS_ELEMENTS_PER_ENTRY);
        } else {
            iter->mapIndex = 0;
            size_t count0 = flexCount(m->map[0]);
            if (count0 >= MOS_ELEMENTS_PER_ENTRY) {
                iter->current =
                    flexIndex(m->map[0], count0 - MOS_ELEMENTS_PER_ENTRY);
            } else {
                iter->current = nullptr;
            }
        }
    }

    iter->valid = (iter->current != nullptr);
}

bool multiOrderedSetMediumIteratorInitAtScore(const multiOrderedSetMedium *m,
                                              mosIterator *iter,
                                              const databox *score,
                                              bool forward) {
    iter->mos = (void *)m;
    iter->type = MOS_TYPE_MEDIUM;
    iter->forward = forward;

    /* Determine which map to start in based on score */
    int targetMap = getMapIndexForScore(m, score);

    /* Find first entry with score >= given score */
    for (int mapIdx = targetMap; mapIdx < 2; mapIdx++) {
        flexEntry *entry = flexHeadIfAny(m->map[mapIdx]);
        while (entry) {
            databox currentScore;
            databoxWide currentScoreWide;
            flexGetByTypeInto(entry, &currentScore, &currentScoreWide);

            if (databoxCompare(&currentScore, score) >= 0) {
                iter->mapIndex = mapIdx;
                iter->current = entry;
                iter->valid = true;
                return true;
            }

            flexEntry *memberEntry = flexNext(m->map[mapIdx], entry);
            if (!memberEntry) {
                break;
            }
            entry = flexNext(m->map[mapIdx], memberEntry);
        }
    }

    iter->current = nullptr;
    iter->valid = false;
    return false;
}

bool multiOrderedSetMediumIteratorInitAtRank(const multiOrderedSetMedium *m,
                                             mosIterator *iter, int64_t rank,
                                             bool forward) {
    iter->mos = (void *)m;
    iter->type = MOS_TYPE_MEDIUM;
    iter->forward = forward;

    size_t count = multiOrderedSetMediumCount(m);
    rank = mosNormalizeRank(rank, count);
    if (rank < 0) {
        iter->current = nullptr;
        iter->valid = false;
        return false;
    }

    size_t count0 = mapCount(m->map[0]);
    int mapIdx;
    size_t localRank;

    if ((size_t)rank < count0) {
        mapIdx = 0;
        localRank = (size_t)rank;
    } else {
        mapIdx = 1;
        localRank = (size_t)rank - count0;
    }

    size_t offset = localRank * MOS_ELEMENTS_PER_ENTRY;
    iter->mapIndex = mapIdx;
    iter->current = flexIndex(m->map[mapIdx], offset);
    iter->valid = (iter->current != nullptr);
    return iter->valid;
}

bool multiOrderedSetMediumIteratorNext(mosIterator *iter, databox *member,
                                       databox *score) {
    if (!iter->valid || !iter->current) {
        return false;
    }

    multiOrderedSetMedium *m = (multiOrderedSetMedium *)iter->mos;
    flexEntry *entry = (flexEntry *)iter->current;
    flex *currentMap = m->map[iter->mapIndex];

    flexEntry *memberEntry = flexNext(currentMap, entry);
    if (!memberEntry) {
        iter->valid = false;
        return false;
    }

    flexGetByType(entry, score);
    flexGetByType(memberEntry, member);

    /* Advance iterator */
    if (iter->forward) {
        flexEntry *next = flexNext(currentMap, memberEntry);
        if (!next && iter->mapIndex == 0) {
            /* Move to map[1] */
            iter->mapIndex = 1;
            next = flexHeadIfAny(m->map[1]);
        }
        iter->current = next;
    } else {
        /* Move backwards: the previous pair's score is two elements back */
        flexEntry *prevMember = flexPrev(currentMap, entry);
        if (prevMember) {
            iter->current = flexPrev(currentMap, prevMember);
        } else if (iter->mapIndex == 1) {
            /* Move to map[0] */
            iter->mapIndex = 0;
            size_t count0 = flexCount(m->map[0]);
            if (count0 >= MOS_ELEMENTS_PER_ENTRY) {
                iter->current =
                    flexIndex(m->map[0], count0 - MOS_ELEMENTS_PER_ENTRY);
            } else {
                iter->current = nullptr;
            }
        } else {
            iter->current = nullptr;
        }
    }

    iter->valid = (iter->current != nullptr);
    return true;
}

/* ====================================================================
 * First / Last
 * ==================================================================== */

bool multiOrderedSetMediumFirst(const multiOrderedSetMedium *m, databox *member,
                                databox *score) {
    /* First entry is in map[0] */
    int mapIdx = 0;
    flexEntry *entry = flexHeadIfAny(m->map[0]);
    if (!entry) {
        mapIdx = 1;
        entry = flexHeadIfAny(m->map[1]);
    }
    if (!entry) {
        return false;
    }

    flexEntry *memberEntry = flexNext(m->map[mapIdx], entry);
    if (!memberEntry) {
        return false;
    }

    flexGetByType(entry, score);
    flexGetByType(memberEntry, member);
    return true;
}

bool multiOrderedSetMediumLast(const multiOrderedSetMedium *m, databox *member,
                               databox *score) {
    /* Last entry is in map[1] if not empty, else map[0] */
    int mapIdx = 1;
    size_t count = flexCount(m->map[1]);
    if (count < MOS_ELEMENTS_PER_ENTRY) {
        mapIdx = 0;
        count = flexCount(m->map[0]);
    }
    if (count < MOS_ELEMENTS_PER_ENTRY) {
        return false;
    }

    flexEntry *entry =
        flexIndex(m->map[mapIdx], count - MOS_ELEMENTS_PER_ENTRY);
    if (!entry) {
        return false;
    }

    flexEntry *memberEntry = flexNext(m->map[mapIdx], entry);
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

/* Fill one array slot pair with the pair at 'rank', ignoring what the
 * slots held: a 96/128-bit value comes back deferred (type only) */
static bool mosMediumRandomPick_(const void *set, size_t rank, databox *member,
                                 databox *score) {
    mosBoxClear(member);
    mosBoxClear(score);
    return multiOrderedSetMediumGetByRank(set, (int64_t)rank, member, score);
}

size_t multiOrderedSetMediumRandomMembers(const multiOrderedSetMedium *m,
                                          int64_t count, databox *members,
                                          databox *scores) {
    const size_t total = multiOrderedSetMediumCount(m);
    if (total == 0) {
        return 0;
    }

    return mosRandomMembersFill(m, mosMediumRandomPick_, &mosRandomState, total,
                                count, members, scores);
}

/* ====================================================================
 * Debugging
 * ==================================================================== */

#ifdef DATAKIT_TEST
#include "ctest.h"

void multiOrderedSetMediumRepr(const multiOrderedSetMedium *m) {
    printf("multiOrderedSetMedium {\n");
    printf("  total count: %zu\n", multiOrderedSetMediumCount(m));
    printf("  total bytes: %zu\n", multiOrderedSetMediumBytes(m));

    for (int mapIdx = 0; mapIdx < 2; mapIdx++) {
        printf("  map[%d]: count=%zu bytes=%zu\n", mapIdx,
               mapCount(m->map[mapIdx]), flexBytes(m->map[mapIdx]));

        flexEntry *entry = flexHead(m->map[mapIdx]);
        int idx = 0;
        while (entry) {
            flexEntry *memberEntry = flexNext(m->map[mapIdx], entry);
            if (!memberEntry) {
                break;
            }

            databoxBig score, member;
            flexGetByTypeBig(entry, &score);
            flexGetByTypeBig(memberEntry, &member);

            printf("    [%d] ", idx);
            databoxReprSay("score=", (const databox *)&score);
            databoxReprSay(" member=", (const databox *)&member);
            printf("\n");

            idx++;
            entry = flexNext(m->map[mapIdx], memberEntry);
        }
    }
    printf("}\n");
}

/* The differential's members: member s holds score s, s < MOSM_N */
static constexpr int MOSM_N = 120;
static bool mosmIs_(const databox *member, const databox *score, int64_t s) {
    char want[16];
    snprintf(want, sizeof(want), "m%03" PRId64, s);
    const bool scoreOk = score->type == DATABOX_SIGNED_64 ? score->data.i64 == s
                         : score->type == DATABOX_UNSIGNED_64
                             ? s >= 0 && score->data.u64 == (uint64_t)s
                             : false;
    return scoreOk && databoxLen(member) == strlen(want) &&
           memcmp(databoxBytes(member), want, strlen(want)) == 0;
}

/* Every read of 'm' against 'present'; returns the mismatches */
static int mosmCheck_(const multiOrderedSetMedium *m, const bool *present,
                      int step) {
    int err = 0;
    int64_t order[MOSM_N];
    int64_t n = 0;
    for (int64_t s = 0; s < MOSM_N; s++) {
        if (present[s]) {
            order[n++] = s;
        }
    }
    if ((int64_t)multiOrderedSetMediumCount(m) != n) {
        ERR("step %d: count %zu, want %" PRId64, step,
            multiOrderedSetMediumCount(m), n);
        return err;
    }
    databox mb;
    databox sb;
    if (n ? (!multiOrderedSetMediumFirst(m, &mb, &sb) ||
             !mosmIs_(&mb, &sb, order[0]) ||
             !multiOrderedSetMediumLast(m, &mb, &sb) ||
             !mosmIs_(&mb, &sb, order[n - 1]))
          : (multiOrderedSetMediumFirst(m, &mb, &sb) ||
             multiOrderedSetMediumLast(m, &mb, &sb))) {
        databox fm = {};
        databox fs = {};
        databox lm = {};
        databox ls = {};
        const bool hf = multiOrderedSetMediumFirst(m, &fm, &fs);
        const bool hl = multiOrderedSetMediumLast(m, &lm, &ls);
        ERR("step %d: first %d (score %" PRId64 "), last %d (score %" PRId64
            "); want %" PRId64 " entries, %" PRId64 "..%" PRId64
            "; map sizes %zu/%zu",
            step, hf, fs.data.i64, hl, ls.data.i64, n, n ? order[0] : -1,
            n ? order[n - 1] : -1, mapCount(m->map[0]), mapCount(m->map[1]));
        return err;
    }
    for (int dir = 0; dir < 2; dir++) {
        mosIterator it;
        multiOrderedSetMediumIteratorInit(m, &it, dir == 0);
        int64_t k = 0;
        while (multiOrderedSetMediumIteratorNext(&it, &mb, &sb)) {
            if (k >= n ||
                !mosmIs_(&mb, &sb, dir == 0 ? order[k] : order[n - 1 - k])) {
                ERR("step %d: iteration %d at %" PRId64, step, dir, k);
                return err;
            }
            k++;
        }
        if (k != n) {
            ERR("step %d: iteration %d gave %" PRId64 " of %" PRId64, step, dir,
                k, n);
            return err;
        }
    }
    for (int64_t r = 0; r < n; r++) {
        char name[16];
        snprintf(name, sizeof(name), "m%03" PRId64, order[r]);
        const databox key = databoxNewBytesString(name);
        mosIterator it;
        if (!multiOrderedSetMediumGetByRank(m, r, &mb, &sb) ||
            !mosmIs_(&mb, &sb, order[r]) ||
            multiOrderedSetMediumGetRank(m, &key) != r ||
            multiOrderedSetMediumGetReverseRank(m, &key) != n - 1 - r ||
            !multiOrderedSetMediumIteratorInitAtRank(m, &it, r, r % 2 == 0) ||
            !multiOrderedSetMediumIteratorNext(&it, &mb, &sb) ||
            !mosmIs_(&mb, &sb, order[r])) {
            ERR("step %d: rank %" PRId64, step, r);
            return err;
        }
    }
    return err;
}

int multiOrderedSetMediumTest(int, char *[]) {
    int err = 0;

    TEST("multiOrderedSetMedium — random ops against a sorted oracle") {
        /* Adds, removes, pops from both ends and rank/score range removals
         * empty one map or the other again and again; after every step
         * every read agrees with the oracle */
        multiOrderedSetMedium *m = multiOrderedSetMediumNew();
        bool present[MOSM_N] = {};
        uint64_t rng = 0x3d1u;
        for (int step = 0; step < 1500 && !err; step++) {
            const uint64_t op = splitmix64(&rng) % 10;
            if (op < 4) {
                const int64_t s = (int64_t)(splitmix64(&rng) % MOSM_N);
                char name[16];
                snprintf(name, sizeof(name), "m%03" PRId64, s);
                const databox member = databoxNewBytesString(name);
                const databox score = databoxNewSigned(s);
                multiOrderedSetMediumAdd(m, &score, &member);
                present[s] = true;
            } else if (op == 4) {
                const int64_t s = (int64_t)(splitmix64(&rng) % MOSM_N);
                char name[16];
                snprintf(name, sizeof(name), "m%03" PRId64, s);
                const databox member = databoxNewBytesString(name);
                if (multiOrderedSetMediumRemove(m, &member) != present[s]) {
                    ERR("step %d: remove of %" PRId64 " wrong", step, s);
                }
                present[s] = false;
            } else if (op == 5 || op == 6) {
                const size_t want = 1 + splitmix64(&rng) % 4;
                databox pm[4];
                databox ps[4];
                const size_t got =
                    op == 5 ? multiOrderedSetMediumPopMin(m, want, pm, ps)
                            : multiOrderedSetMediumPopMax(m, want, pm, ps);
                for (size_t i = 0; i < got; i++) {
                    int64_t s = -1;
                    for (int64_t k = 0; k < MOSM_N; k++) {
                        const int64_t c = op == 5 ? k : MOSM_N - 1 - k;
                        if (present[c]) {
                            s = c;
                            break;
                        }
                    }
                    if (s < 0 || !mosmIs_(&pm[i], &ps[i], s)) {
                        ERR("step %d: pop %zu wrong", step, i);
                    }
                    if (s >= 0) {
                        present[s] = false;
                    }
                    databoxFreeData(&pm[i]);
                    databoxFreeData(&ps[i]);
                }
            } else if (op == 7) {
                int64_t n = 0;
                for (int64_t s = 0; s < MOSM_N; s++) {
                    n += present[s];
                }
                if (n) {
                    const int64_t a = (int64_t)(splitmix64(&rng) % (uint64_t)n);
                    const int64_t b = a + (int64_t)(splitmix64(&rng) % 5);
                    int64_t rank = 0;
                    size_t want = 0;
                    /* A stop past the end stops at the last rank */
                    for (int64_t s = 0; s < MOSM_N; s++) {
                        if (present[s]) {
                            if (rank >= a && rank <= b) {
                                present[s] = false;
                                want++;
                            }
                            rank++;
                        }
                    }
                    if (multiOrderedSetMediumRemoveRangeByRank(m, a, b) !=
                        want) {
                        ERR("step %d: rank range [%" PRId64 ", %" PRId64 "]",
                            step, a, b);
                    }
                }
            } else if (op == 8) {
                const int64_t lo = (int64_t)(splitmix64(&rng) % MOSM_N);
                const int64_t hi = lo + (int64_t)(splitmix64(&rng) % 8);
                const mosRangeSpec range = {.min = databoxNewSigned(lo),
                                            .max = databoxNewSigned(hi)};
                size_t want = 0;
                for (int64_t s = lo; s <= hi && s < MOSM_N; s++) {
                    want += present[s];
                    present[s] = false;
                }
                if (multiOrderedSetMediumRemoveRangeByScore(m, &range) !=
                    want) {
                    ERR("step %d: score range [%" PRId64 ", %" PRId64 "]", step,
                        lo, hi);
                }
            } else {
                /* Drain the low end, then add above everything */
                databox pm[4];
                databox ps[4];
                const size_t got = multiOrderedSetMediumPopMin(m, 4, pm, ps);
                for (size_t i = 0; i < got; i++) {
                    for (int64_t k = 0; k < MOSM_N; k++) {
                        if (present[k]) {
                            present[k] = false;
                            break;
                        }
                    }
                    databoxFreeData(&pm[i]);
                    databoxFreeData(&ps[i]);
                }
                const int64_t s = MOSM_N - 1 - (int64_t)(splitmix64(&rng) % 10);
                char name[16];
                snprintf(name, sizeof(name), "m%03" PRId64, s);
                const databox member = databoxNewBytesString(name);
                const databox score = databoxNewSigned(s);
                multiOrderedSetMediumAdd(m, &score, &member);
                present[s] = true;
            }
            err += mosmCheck_(m, present, step);
        }
        multiOrderedSetMediumFree(m);
    }

    TEST("multiOrderedSetMedium — create and free") {
        multiOrderedSetMedium *mos = multiOrderedSetMediumNew();
        if (!mos) {
            ERR("Failed to create multiOrderedSetMedium");
        }
        if (multiOrderedSetMediumCount(mos) != 0) {
            ERR("New set should be empty");
        }
        multiOrderedSetMediumFree(mos);
    }

    TEST("multiOrderedSetMedium — resized in-place scores, misses, edges") {
        /* Scores i * 2^40 for m0..m9.  m3's 6-byte integer becomes the
         * double 3.5e12: still between its neighbours, so it is rewritten
         * in place, but 8 bytes wide.  Ranks, scores and order hold. */
        multiOrderedSetMedium *mos = multiOrderedSetMediumNew();
        for (int64_t i = 0; i < 10; i++) {
            const databox score = databoxNewSigned(i << 40);
            char buf[8];
            snprintf(buf, sizeof(buf), "m%" PRId64, i);
            const databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            multiOrderedSetMediumAdd(mos, &score, &member);
        }
        const databox m3 = databoxNewBytesAllowEmbed("m3", 2);
        const databox wider = DATABOX_DOUBLE(3.5e12);
        databox prev = {};
        if (!multiOrderedSetMediumAddGetPrevious(mos, &wider, &m3, &prev) ||
            prev.data.i != (INT64_C(3) << 40)) {
            ERR("the rewrite did not report m3's old score");
        }
        databox got = {};
        if (!multiOrderedSetMediumGetScore(mos, &m3, &got) ||
            got.type != DATABOX_DOUBLE_64 || got.data.d64 != 3.5e12 ||
            multiOrderedSetMediumGetRank(mos, &m3) != 3) {
            ERR("m3 lost its new score or its rank");
        }

        /* A new member through AddGetPrevious; misses rank -1 */
        const databox mNew = databoxNewBytesAllowEmbed("mN", 2);
        const databox mid = databoxNewSigned((INT64_C(5) << 40) + 1);
        const databox ghost = databoxNewBytesAllowEmbed("ghost", 5);
        if (multiOrderedSetMediumAddGetPrevious(mos, &mid, &mNew, &prev) ||
            multiOrderedSetMediumGetRank(mos, &mNew) != 6 ||
            multiOrderedSetMediumGetRank(mos, &ghost) != -1 ||
            multiOrderedSetMediumGetReverseRank(mos, &ghost) != -1) {
            ERR("a new member was misplaced or a miss was ranked");
        }

        /* Every row in ascending order */
        mosIterator iter;
        databox m;
        databox s;
        multiOrderedSetMediumIteratorInit(mos, &iter, true);
        databox last = databoxNewSigned(INT64_MIN);
        size_t n = 0;
        bool ordered = true;
        while (multiOrderedSetMediumIteratorNext(&iter, &m, &s)) {
            ordered = ordered && databoxCompare(&last, &s) < 0;
            last = s;
            n++;
        }
        if (!ordered || n != 11) {
            ERR("iterated %zu rows, ordered %d", n, ordered);
        }

        /* Iterators past the end, or at a rank out of range, start
         * nowhere and step nothing */
        const databox above = databoxNewSigned(INT64_C(20) << 40);
        if (multiOrderedSetMediumIteratorInitAtScore(mos, &iter, &above,
                                                     true) ||
            multiOrderedSetMediumIteratorNext(&iter, &m, &s) ||
            multiOrderedSetMediumIteratorInitAtRank(mos, &iter, 11, true) ||
            multiOrderedSetMediumIteratorNext(&iter, &m, &s)) {
            ERR("an iterator started past the end");
        }
        multiOrderedSetMediumFree(mos);

        multiOrderedSetMedium *empty = multiOrderedSetMediumNew();
        databox members[2];
        databox scores[2];
        if (multiOrderedSetMediumRandomMembers(empty, 2, members, scores) !=
            0) {
            ERR("an empty set gave random members");
        }
        multiOrderedSetMediumFree(empty);
    }

    TEST("multiOrderedSetMedium — add and lookup") {
        multiOrderedSetMedium *mos = multiOrderedSetMediumNew();

        /* Add several entries */
        for (int i = 0; i < 10; i++) {
            databox score = databoxNewSigned(i * 10);
            char buf[16];
            snprintf(buf, sizeof(buf), "m%d", i);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            multiOrderedSetMediumAdd(mos, &score, &member);
        }

        if (multiOrderedSetMediumCount(mos) != 10) {
            ERR("Count should be 10, got %zu", multiOrderedSetMediumCount(mos));
        }

        /* Verify ordering */
        for (int i = 0; i < 10; i++) {
            databox m, s;
            if (!multiOrderedSetMediumGetByRank(mos, i, &m, &s)) {
                ERR("GetByRank(%d) failed", i);
            }
            if (s.data.i != i * 10) {
                ERR("Rank %d should have score %d, got %" PRIdMAX, i, i * 10,
                    s.data.i);
            }
        }

        multiOrderedSetMediumFree(mos);
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
