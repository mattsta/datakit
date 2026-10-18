#include "multimapMedium.h"
#include "cleaner.h"
#include "dkRandom.h"
#include "multimapInternal.h"
#include "multimapMediumInternal.h"

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleaner(multimapMedium *, multimapMediumFree);

#define GET_MIDDLE(m, idx) (flexEntry *)((m)->map[idx] + (m)->middle[idx])
#define SET_MIDDLE(m, idx, mid)                                                \
    do {                                                                       \
        (m)->middle[idx] = (mid) - (m)->map[idx];                              \
    } while (0)

#define SET_MIDDLE_FORCE(m, idx)                                               \
    do {                                                                       \
        SET_MIDDLE(m, idx, flexMiddle((m)->map[idx], (m)->elementsPerEntry));  \
    } while (0)

/* Create empty multimapMedium */
multimapMedium *multimapMediumNew(multimapElements elementsPerEntry) {
    /* @own: caller; free=multimapMediumFree */
    multimapMedium *m = zcalloc(1, sizeof(*m));

    m->elementsPerEntry = elementsPerEntry;

    /* @own: container; free=multimapMediumFree */
    m->map[0] = flexNew();
    /* @own: container; free=multimapMediumFree */
    m->map[1] = flexNew();

    m->middle[0] = FLEX_EMPTY_SIZE;
    m->middle[1] = FLEX_EMPTY_SIZE;

    return m;
}

/* Create multimapMedium by splitting one sorted flex across two new slots */
multimapMedium *multimapMediumNewFromOneGrow(void *_m, flex *one,
                                             uint32_t middle,
                                             multimapElements elementsPerEntry,
                                             bool mapIsSet) {
    /* @own: caller; free=multimapMediumFree */
    multimapMedium *m = zrealloc(_m, sizeof(*m));

    m->elementsPerEntry = elementsPerEntry;
    m->mapIsSet = mapIsSet;

    flex *higher = flexSplitMiddle(&one, m->elementsPerEntry, one + middle);

#ifndef NDEBUG
    /* If one map has more than one element, the other map should exist... */
    if (flexCount(one) > 0) {
        assert(flexCount(higher) > 0);
    }
#endif

    m->map[0] = one;
    m->map[1] = higher;

    SET_MIDDLE_FORCE(m, 0);
    SET_MIDDLE_FORCE(m, 1);

    return m;
}

multimapMedium *multimapMediumCopy(const multimapMedium *m) {
    /* @own: caller; free=multimapMediumFree */
    multimapMedium *copy = zcalloc(1, sizeof(*copy));
    *copy = *m;

    /* @own: container; free=multimapMediumFree */
    copy->map[0] = flexDuplicate(m->map[0]);
    /* @own: container; free=multimapMediumFree */
    copy->map[1] = flexDuplicate(m->map[1]);

    /* middle entries are an array in the struct so they were duplicated at the
     * copy assignment. */
    return copy;
}

size_t multimapMediumCount(const multimapMedium *m) {
    return (flexCount(m->map[0]) + flexCount(m->map[1])) / m->elementsPerEntry;
}

size_t multimapMediumBytes(const multimapMedium *m) {
    return flexBytes(m->map[0]) + flexBytes(m->map[1]);
}

flex *multimapMediumDump(const multimapMedium *m) {
    /* @own: caller; free=flexFree */
    flex *all = flexDuplicate(m->map[0]);
    flexBulkAppendFlex(&all, m->map[1]);
    return all;
}

DK_INLINE_ALWAYS size_t multimapMediumBinarySearch_(
    multimapMedium *m, const databox *key, const bool useReference,
    const multimapAtom *referenceContainer) {
    /* if map[1] is empty, use map[0] directly */
    if (flexCount(m->map[1]) == 0) {
        return 0;
    }

    const flexEntry *restrict head = flexHead(m->map[1]);
    databoxBig got;
    multimapGetByTypeBig_(head, &got, useReference ? referenceContainer : NULL);

    const int compared = databoxCompare((databox *)&got, key);

    if (compared <= 0) {
        /* head of map 1 is <= key, so insert into [1] */
        return 1;
    }

    /* else, use map 0 */
    return 0;
}

DK_STATIC size_t multimapMediumBinarySearch(multimapMedium *m,
                                            const databox *key) {
    return multimapMediumBinarySearch_(m, key, false, NULL);
}

DK_STATIC size_t multimapMediumBinarySearchWithReference(
    multimapMedium *m, const databox *key,
    const multimapAtom *referenceContainer) {
    return multimapMediumBinarySearch_(m, key, true, referenceContainer);
}

DK_STATIC size_t multimapMediumBinarySearchFullWidth(
    multimapMedium *m, const databox *elements[]) {
    /* We split the search space based on the head element of map[1],
     * so if we're < element[x], use map[0], else use map[1] */
    databoxBig got;
    flexEntry *start = flexHead(m->map[1]);
    size_t mapIdx = 1;

    /* if head == tail, then map[1] has no elements and we use
     * map[0] for insert / search. */
    if (start == flexTail(m->map[1])) {
        return 0;
    }

    flexGetByTypeBig(start, &got);

    for (size_t i = 0; i < m->elementsPerEntry; i++) {
        const int compared = databoxCompare((databox *)&got, elements[i]);

        if (compared < 0) {
            /* head of map 1 is < key, so insert into [1] */
            /* [MAP 1 HEAD, NEW VALUE] */
            mapIdx = 1;
            break;
        } else if (compared > 0) {
            /* else, element of map 1 is > lookup key, so insert into [0] */
            /* [NEW VALUE, MAP 1 HEAD] */
            mapIdx = 0;
            break;
        } else { /* (compared == 0) */
            if (i == (size_t)m->elementsPerEntry - 1) {
                /* We found equal at max extent, so we are done. */
                break;
            }

            start = flexNext(m->map[1], start);
            assert(start);
            flexGetByTypeBig(start, &got);
            /* if same, loop again and check next element pair */
            /* If this is the last loop, we were equal against every
             * compare element, and we return mapIdx==1 as mapIdx
             * is initialized to 1 above. */
        }
    }

    return mapIdx;
}

void multimapMediumInsertWithSurrogateKey(
    multimapMedium *m, const databox *elements[], const databox *insertKey,
    const multimapAtom *referenceContainer) {
    const size_t mapIdx = multimapMediumBinarySearchWithReference(
        m, elements[0], referenceContainer);

    flexEntry *middle = GET_MIDDLE(m, mapIdx);
    /* compareUsingKeyElementOnly: For maps (!mapIsSet), compare only key.
     * For sets (mapIsSet), compare all elements to check for full-width dups.
     */
    flexInsertReplaceByTypeSortedWithMiddleMultiWithReferenceWithSurrogateKey(
        &m->map[mapIdx], m->elementsPerEntry, elements, insertKey, &middle,
        !m->mapIsSet, referenceContainer);
    SET_MIDDLE(m, mapIdx, middle);
}

bool multimapMediumInsert(multimapMedium *m, const databox *elements[]) {
    return multimapMediumInsertReplacing(m, elements, NULL);
}

/* 'replacedOut' as multimapSmallInsertReplacing: a map replace reports the
 * overwritten value entries, a set (which never overwrites) reports VOID. */
bool multimapMediumInsertReplacing(multimapMedium *m, const databox *elements[],
                                   databox *replacedOut) {
    size_t mapIdx = multimapMediumBinarySearch(m, elements[0]);

    flexEntry *middle = GET_MIDDLE(m, mapIdx);
    /* Maps: key-only compare, found => replace values.  Sets: the
     * dedupe variant — see multimapSmallInsert. */
    bool replaced;
    if (m->mapIsSet) {
        replaced = flexInsertReplaceByTypeSortedWithMiddleMultiDirectSet(
            &m->map[mapIdx], m->elementsPerEntry, elements, &middle);
        multimapReplacedOutClear_(replacedOut, m->elementsPerEntry);
    } else {
        replaced = flexInsertReplaceByTypeSortedWithMiddleMultiDirectReplacing(
            &m->map[mapIdx], m->elementsPerEntry, elements, &middle, true,
            replacedOut);
    }
    SET_MIDDLE(m, mapIdx, middle);
    return replaced;
}

void multimapMediumInsertFullWidth(multimapMedium *m,
                                   const databox *elements[]) {
    size_t mapIdx = multimapMediumBinarySearchFullWidth(m, elements);

    flexEntry *middle = GET_MIDDLE(m, mapIdx);
    /* InsertFullWidth: Always compare ALL elements (key+value) to allow
     * multiple entries with same key but different values (sorted set). */
    flexInsertReplaceByTypeSortedWithMiddleMultiDirect(
        &m->map[mapIdx], m->elementsPerEntry, elements, &middle, false);
    SET_MIDDLE(m, mapIdx, middle);
}

DK_INLINE_ALWAYS void multimapMediumConform_(multimapMedium *m, size_t mapIdx);

void multimapMediumAppend(multimapMedium *m, const databox *elements[]) {
    static const size_t mapIdx = 1; /* always insert into highest map! */

    flexEntry *middle = GET_MIDDLE(m, mapIdx);
    /* Maps: key-only compare.  Sets: the dedupe variant — see
     * multimapSmallInsert. */
    if (m->mapIsSet) {
        flexInsertReplaceByTypeSortedWithMiddleMultiDirectSet(
            &m->map[mapIdx], m->elementsPerEntry, elements, &middle);
    } else {
        flexInsertReplaceByTypeSortedWithMiddleMultiDirect(
            &m->map[mapIdx], m->elementsPerEntry, elements, &middle, true);
    }
    SET_MIDDLE(m, mapIdx, middle);

    /* Appending into map 1 while map 0 is empty (e.g. the first insert
     * after a full drain) would strand the data where searches, which
     * stop on an empty map 0, can't reach it. Rebalance into map 0. */
    multimapMediumConform_(m, 0);
}

DK_INLINE_ALWAYS bool
abstractGetUnderlyingEntry(multimapMedium *m, const databox *key,
                           multimapEntry *me, const bool useReference,
                           const multimapAtom *referenceContainer) {
    me->mapIdx =
        multimapMediumBinarySearch_(m, key, useReference, referenceContainer);

    if (useReference) {
        me->fe = flexFindByTypeSortedWithMiddleWithReference(
            m->map[me->mapIdx], m->elementsPerEntry, key,
            GET_MIDDLE(m, me->mapIdx), referenceContainer);
    } else {
        me->fe = flexFindByTypeSortedWithMiddle(m->map[me->mapIdx],
                                                m->elementsPerEntry, key,
                                                GET_MIDDLE(m, me->mapIdx));
    }

    me->map = &m->map[me->mapIdx];
    return !!me->fe;
}

bool multimapMediumGetUnderlyingEntry(multimapMedium *m, const databox *key,
                                      multimapEntry *me) {
    return abstractGetUnderlyingEntry(m, key, me, false, NULL);
}

bool multimapMediumGetUnderlyingEntryGetEntry(multimapMedium *m,
                                              const databox *key,
                                              multimapEntry *me) {
    /* Position variant: unlike the exact-match search used for direct
     * entry lookup, this returns the insertion point for any key, so
     * "start at or after K" works for keys that don't exist. */
    me->mapIdx = multimapMediumBinarySearch_(m, key, false, NULL);
    me->fe = flexFindByTypeSortedWithMiddleGetEntry(m->map[me->mapIdx],
                                                    m->elementsPerEntry, key,
                                                    GET_MIDDLE(m, me->mapIdx));
    me->map = &m->map[me->mapIdx];

    return !!me->fe;
}

bool multimapMediumGetUnderlyingEntryWithReference(
    multimapMedium *m, const databox *key, multimapEntry *me,
    const multimapAtom *referenceContainer) {
    return abstractGetUnderlyingEntry(m, key, me, true, referenceContainer);
}

void multimapMediumResizeEntry(multimapMedium *m, multimapEntry *me,
                               size_t newLen) {
    assert(m->map[me->mapIdx] == *me->map);
    flexResizeEntry(me->map, me->fe, newLen);
    SET_MIDDLE_FORCE(m, me->mapIdx);
}

void multimapMediumReplaceEntry(multimapMedium *m, multimapEntry *me,
                                const databox *box) {
    flexReplaceByType(me->map, me->fe, box);
    SET_MIDDLE_FORCE(m, me->mapIdx);
}

bool multimapMediumExists(multimapMedium *m, const databox *key) {
    size_t mapIdx = multimapMediumBinarySearch(m, key);

    return !!flexFindByTypeSortedWithMiddle(m->map[mapIdx], m->elementsPerEntry,
                                            key, GET_MIDDLE(m, mapIdx));
}

/* TODO: abstract x 4 */
bool multimapMediumExistsFullWidth(multimapMedium *m,
                                   const databox *elements[]) {
    size_t mapIdx = multimapMediumBinarySearchFullWidth(m, elements);

    return !!flexFindByTypeSortedWithMiddleFullWidth(
        m->map[mapIdx], m->elementsPerEntry, elements, GET_MIDDLE(m, mapIdx));
}

bool multimapMediumExistsWithReference(multimapMedium *m, const databox *key,
                                       databox *foundRef,
                                       const multimapAtom *referenceContainer) {
    size_t mapIdx =
        multimapMediumBinarySearchWithReference(m, key, referenceContainer);

    const flexEntry *found = flexFindByTypeSortedWithMiddleWithReference(
        m->map[mapIdx], m->elementsPerEntry, key, GET_MIDDLE(m, mapIdx),
        referenceContainer);

    if (found) {
        flexGetByType(found, foundRef);
        return true;
    }

    return false;
}

DK_INLINE_ALWAYS bool abstractLookup(multimapMedium *m, const databox *key,
                                     databox *elements[],
                                     const bool useReference,
                                     const multimapAtom *referenceContainer) {
    uint16_t mapIdx =
        multimapMediumBinarySearch_(m, key, useReference, referenceContainer);
    flexEntry *middle = GET_MIDDLE(m, mapIdx);

    flexEntry *foundP;
    if (useReference) {
        foundP = flexFindByTypeSortedWithMiddleWithReference(
            m->map[mapIdx], m->elementsPerEntry, key, middle,
            referenceContainer);
    } else {
        foundP = flexFindByTypeSortedWithMiddle(
            m->map[mapIdx], m->elementsPerEntry, key, middle);
    }

    if (foundP) {
        flexEntry *nextFound = foundP;
        for (multimapElements i = 1; i < m->elementsPerEntry; i++) {
            nextFound = flexNext(m->map[mapIdx], nextFound);
            flexGetByType(nextFound, elements[i - 1]);
        }
    }

    return !!foundP;
}

bool multimapMediumLookup(multimapMedium *m, const databox *key,
                          databox *elements[]) {
    return abstractLookup(m, key, elements, false, NULL);
}

bool multimapMediumLookupWithReference(multimapMedium *m, const databox *key,
                                       databox *elements[],
                                       const multimapAtom *referenceContainer) {
    return abstractLookup(m, key, elements, true, referenceContainer);
}

DK_STATIC flexEntry *multimapMediumRowByPosition_(multimapMedium *m,
                                                  int64_t keyIndex,
                                                  size_t *mapIdxOut);

bool multimapMediumRandomValue(multimapMedium *m, const bool fromTail,
                               databox **foundBox, multimapEntry *me) {
    const size_t rows = multimapMediumCount(m);
    if (rows == 0) {
        return false;
    }

    /* Uniform selection: pick a global row with a per-thread uniform
     * draw, then locate it; fromTail addresses the container's last
     * row. */
    const int64_t row =
        fromTail ? (int64_t)rows - 1 : (int64_t)dkRandomBounded(rows);

    size_t mapIdx;
    flexEntry *foundP = multimapMediumRowByPosition_(m, row, &mapIdx);
    assert(foundP);

    flex **map = &m->map[mapIdx];
    me->map = map;
    me->mapIdx = mapIdx;
    me->fe = foundP;

    if (foundBox) {
        flexEntry *ffoundP = foundP;
        for (size_t i = 0; i < m->elementsPerEntry; i++) {
            flexGetByType(ffoundP, foundBox[i]);
            ffoundP = flexNext(*map, ffoundP);
        }
    }

    return true;
}

DK_INLINE_ALWAYS void multimapMediumConform_(multimapMedium *m,
                                             const size_t mapIdx) {
    /* Now, with value deleted, we need ONE MORE set of checks:
     *  - if mapIdx is 0 AND
     *  - if mapIdx 0 is EMPTY AND
     *  - if mapIdx 1 is NOT EMPTY
     *
     * Then we MUST move mapIdx 1 to mapIdx 0 because otherwise
     * we'd have data inside map 1 while map 0 is empty, and that
     * means we wouldn't be able to find the data on map 1 again
     * because if map 0 is empty it means searches get cut off. */
    if (mapIdx == 0) {
        flex *restrict const map0Orig = m->map[0];
        flex *restrict const map1Orig = m->map[1];
        if (flexCount(map0Orig) == 0 && flexCount(map1Orig) > 0) {
            const size_t middleOrig = m->middle[0];
            m->map[0] = map1Orig;
            m->map[1] = map0Orig;
            m->middle[0] = m->middle[1];
            m->middle[1] = middleOrig;
        }
    }
}

void multimapMediumDeleteEntry(multimapMedium *m, multimapEntry *me) {
    /* Step 1: pick victim map */
    const size_t mapIdx = me->mapIdx;

    flex **map = me->map;
    flexEntry *middle = GET_MIDDLE(m, mapIdx);

    /* Step 2: pick victim element */
    flexEntry *foundP = me->fe;

    flexDeleteSortedValueWithMiddle(map, m->elementsPerEntry, foundP, &middle);
    SET_MIDDLE(m, mapIdx, middle);

    /* If we removed the final element of map[0], we need to move map[1] */
    multimapMediumConform_(m, mapIdx);
}

bool multimapMediumDeleteRandomValue(multimapMedium *m,
                                     const bool deleteFromTail,
                                     databox **deletedBox) {
    multimapEntry me;
    /* Pass NULL for deletedBox to multimapMediumRandomValue, then manually
     * copy the data using flexGetByTypeCopy. This avoids use-after-free
     * since flexGetByType returns pointers to internal flex data which
     * become invalid after multimapMediumDeleteEntry. */
    if (!multimapMediumRandomValue(m, deleteFromTail, NULL, &me)) {
        return false;
    }

    /* Copy the data before deletion so caller has valid data */
    if (deletedBox) {
        flexEntry *fe = me.fe;
        for (size_t i = 0; i < m->elementsPerEntry; i++) {
            flexGetByTypeCopy(fe, deletedBox[i]);
            fe = flexNext(*me.map, fe);
        }
    }

    multimapMediumDeleteEntry(m, &me);
    return true;
}

DK_INLINE_ALWAYS bool
abstractDelete(multimapMedium *m, const databox **elements,
               const bool fullWidth, const bool useReference,
               const struct multimapAtom *referenceContainer,
               databox *foundReference) {
    size_t mapIdx = 0;

    if (useReference) {
        if (fullWidth) {
            assert(NULL && "Not implemented!");
            __builtin_unreachable();
        } else {
            mapIdx = multimapMediumBinarySearchWithReference(
                m, *elements, referenceContainer);
        }
    } else {
        if (fullWidth) {
            mapIdx = multimapMediumBinarySearchFullWidth(m, elements);
        } else {
            mapIdx = multimapMediumBinarySearch(m, *elements);
        }
    }

    flexEntry *middle = GET_MIDDLE(m, mapIdx);

    flexEntry *foundP;

    if (useReference) {
        if (fullWidth) {
            foundP = flexFindByTypeSortedWithMiddleFullWidthWithReference(
                m->map[mapIdx], m->elementsPerEntry, elements, middle,
                referenceContainer);
        } else {
            foundP = flexFindByTypeSortedWithMiddleWithReference(
                m->map[mapIdx], m->elementsPerEntry, *elements, middle,
                referenceContainer);
        }
    } else {
        if (fullWidth) {
            foundP = flexFindByTypeSortedWithMiddleFullWidth(
                m->map[mapIdx], m->elementsPerEntry, elements, middle);
        } else {
            foundP = flexFindByTypeSortedWithMiddle(
                m->map[mapIdx], m->elementsPerEntry, *elements, middle);
        }
    }

    if (foundP) {
        if (foundReference) {
            flexGetByType(foundP, foundReference);
        }

        flexDeleteSortedValueWithMiddle(&m->map[mapIdx], m->elementsPerEntry,
                                        foundP, &middle);
        SET_MIDDLE(m, mapIdx, middle);

        /* If we removed the final element of map[0], we need to move map[1] */
        multimapMediumConform_(m, mapIdx);
    }

    return !!foundP;
}

bool multimapMediumDelete(multimapMedium *m, const databox *key) {
    return abstractDelete(m, &key, false, false, NULL, NULL);
}

bool multimapMediumDeleteWithReference(
    multimapMedium *m, const databox *key,
    const struct multimapAtom *referenceContainer, databox *foundReference) {
    return abstractDelete(m, &key, false, true, referenceContainer,
                          foundReference);
}

bool multimapMediumDeleteWithFound(multimapMedium *m, const databox *key,
                                   databox *foundReference) {
    return abstractDelete(m, &key, false, true, NULL, foundReference);
}

bool multimapMediumDeleteFullWidth(multimapMedium *m,
                                   const databox *elements[]) {
    return abstractDelete(m, elements, true, false, NULL, NULL);
}

/* Locate the row at position 'keyIndex' (0-based forward; negative =
 * 1-based from the tail): pick the owning map by row counts, then walk
 * that flex from the nearest of its head/middle/tail anchors. */
DK_STATIC flexEntry *multimapMediumRowByPosition_(multimapMedium *m,
                                                  int64_t keyIndex,
                                                  size_t *mapIdxOut) {
    const int64_t rows0 = flexCount(m->map[0]) / m->elementsPerEntry;
    const int64_t rows1 = flexCount(m->map[1]) / m->elementsPerEntry;
    const int64_t rows = rows0 + rows1;

    int64_t row = keyIndex < 0 ? rows + keyIndex : keyIndex;
    if (row < 0 || row >= rows) {
        return NULL;
    }

    const size_t mapIdx = row < rows0 ? 0 : 1;
    if (mapIdx == 1) {
        row -= rows0;
    }

    *mapIdxOut = mapIdx;
    return flexIndexWithMiddle(m->map[mapIdx], m->elementsPerEntry,
                               row * m->elementsPerEntry,
                               GET_MIDDLE(m, mapIdx));
}

bool multimapMediumLookupByPosition(multimapMedium *m, int64_t keyIndex,
                                    databox *elements[]) {
    size_t mapIdx;
    flexEntry *fe = multimapMediumRowByPosition_(m, keyIndex, &mapIdx);
    if (!fe) {
        return false;
    }

    for (multimapElements i = 0; i < m->elementsPerEntry; i++) {
        flexGetByType(fe, elements[i]);
        fe = flexNext(m->map[mapIdx], fe);
    }

    return true;
}

bool multimapMediumDeleteByPosition(multimapMedium *m, int64_t keyIndex) {
    size_t mapIdx;
    flexEntry *fe = multimapMediumRowByPosition_(m, keyIndex, &mapIdx);
    if (!fe) {
        return false;
    }

    multimapEntry me = {.map = &m->map[mapIdx], .mapIdx = mapIdx, .fe = fe};
    multimapMediumDeleteEntry(m, &me);
    return true;
}

int64_t multimapMediumFieldIncr(multimapMedium *m, const databox *key,
                                uint32_t fieldOffset, int64_t incrBy) {
    size_t mapIdx = multimapMediumBinarySearch(m, key);

    flexEntry *current = flexFindByTypeSortedWithMiddle(
        m->map[mapIdx], m->elementsPerEntry, key, GET_MIDDLE(m, mapIdx));
    if (!current) {
        /* No such key: return 0 rather than walking flexNext from NULL.
         * Matches the multimap-level FieldIncr miss contract. */
        return 0;
    }

    while (fieldOffset--) {
        current = flexNext(m->map[mapIdx], current);
    }

    int64_t newVal = 0;
    if (flexIncrbySigned(&m->map[mapIdx], current, incrBy, &newVal)) {
        /* if incremented, return new value */
        /* TODO: update flexIncrbySigned() to report if allocation changed
         *       so we only conditionally update the mapIdx */
        SET_MIDDLE_FORCE(m, mapIdx);
        return newVal;
    }

    /* (unlikely) else, return current value; a 96/128-bit value can't be
     * read as int64_t, so it reports 0 */
    databoxBig curVal;
    flexGetByTypeBig(current, &curVal);
    return curVal.big ? 0 : curVal.data.i;
}

void multimapMediumReset(multimapMedium *m) {
    flexReset(&m->map[0]);
    SET_MIDDLE_FORCE(m, 0);

    flexReset(&m->map[1]);
    SET_MIDDLE_FORCE(m, 1);
}

void multimapMediumFree(multimapMedium *m) {
    if (m) {
        flexFree(m->map[0]);
        flexFree(m->map[1]);
        zfree(m);
    }
}

bool multimapMediumFirst(multimapMedium *m, databox *elements[]) {
    if (flexCount(m->map[0]) == 0) {
        return false;
    }

    /* Populate forward */
    flexEntry *current = flexHead(m->map[0]);
    for (multimapElements i = 0; i < m->elementsPerEntry; i++) {
        flexGetByType(current, elements[i]);
        current = flexNext(m->map[0], current);
    }

    return true;
}

bool multimapMediumLast(multimapMedium *m, databox *elements[]) {
    /* Pick the map containing a tail entry */
    flex *useMap = m->map[1];
    if (flexCount(useMap) == 0) {
        useMap = m->map[0];
        if (flexCount(useMap) == 0) {
            /* If neither map has entries, we can't retrive elements. */
            return false;
        }
    }

    /* Populate reverse */
    flexEntry *current = flexTail(useMap);
    for (multimapElements i = 0; i < m->elementsPerEntry; i++) {
        flexGetByType(current, elements[(m->elementsPerEntry - 1) - i]);
        current = flexPrev(useMap, current);
    }

    return true;
}

DK_INLINE_ALWAYS bool multimapMediumIteratorInitAt_(multimapMedium *m,
                                                    multimapIterator *iter,
                                                    bool forward,
                                                    const multimapEntry *me) {
    iter->mm = m;
    iter->forward = forward;
    iter->elementsPerEntry = m->elementsPerEntry;
    iter->type = MULTIMAP_TYPE_MEDIUM;
    iter->mapIndex = me->mapIdx;
    iter->map = *me->map;
    iter->entry = me->fe;
    return true;
}

bool multimapMediumIteratorInitAt(multimapMedium *m, multimapIterator *iter,
                                  bool forward, const databox *box) {
    multimapEntry me;
    multimapMediumGetUnderlyingEntryGetEntry(m, box, &me);

    /* If we iterated past all elements, we can't iterate any more... */
    if (me.fe == *me.map + flexBytes(*me.map)) {
        /* ...unless the position merely ran off the end of map 0 while
         * map 1 still holds the keys at or above the search point. */
        if (forward && me.mapIdx == 0 && flexCount(m->map[1]) > 0) {
            me.mapIdx = 1;
            me.map = &m->map[1];
            me.fe = flexHead(m->map[1]);
        } else {
            me.fe = NULL;
        }
    }

    multimapMediumIteratorInitAt_(m, iter, forward, &me);
    return !!me.fe;
}

bool multimapMediumIteratorInit(multimapMedium *m, multimapIterator *iter,
                                bool forward) {
    flex *tmp = NULL;
    multimapEntry me;

    if (likely(flexCount(m->map[0]))) {
        if (likely(forward)) {
            tmp = m->map[0];
            me.mapIdx = 0;
            me.fe = flexHead(tmp);
            me.map = &tmp;
        } else {
            /* map[1] can be empty (deletes, or a conform swap), and an
             * empty flex has no tail entry: start in map[0] then. */
            const uint32_t last = flexCount(m->map[1]) ? 1 : 0;
            tmp = m->map[last];
            me.mapIdx = last;
            me.fe = flexTail(tmp);
            me.map = &tmp;
        }
    } else {
        /* If no entries in the lowest map (which must always have entries if
         * entries exist), then we can't iterate. */
        me.fe = NULL;
        me.mapIdx = 1;
        me.map = &tmp;
    }

    return multimapMediumIteratorInitAt_(m, iter, forward, &me);
}

bool multimapMediumIteratorNext(multimapIterator *iter, databox *elements[]) {
    if (iter->entry) {
        flexEntry *current = iter->entry;
        if (iter->forward) {
            /* Populate forward */
            for (multimapElements i = 0; i < iter->elementsPerEntry; i++) {
                flexGetByType(current, elements[i]);
                current = flexNext(iter->map, current);
            }

            iter->entry = current;
        } else {
            /* Populate reverse */
            for (multimapElements i = 0; i < iter->elementsPerEntry; i++) {
                flexGetByType(current,
                              elements[(iter->elementsPerEntry - 1) - i]);
                current = flexPrev(iter->map, current);
            }

            iter->entry = current;
        }

        return true;
    }

    /* If moving forward and reached end of first map,
     * begin iterating over second map. */
    if (iter->forward && iter->mapIndex == 0) {
        iter->mapIndex = 1;
        iter->map = ((multimapMedium *)iter->mm)->map[1];
        iter->entry = flexCount(iter->map) ? flexHead(iter->map) : NULL;
        return multimapMediumIteratorNext(iter, elements);
    }

    /* If moving reverse and reached beginning of second map,
     * begin iterating over first map. */
    if (!iter->forward && iter->mapIndex == 1) {
        iter->mapIndex = 0;
        iter->map = ((multimapMedium *)iter->mm)->map[0];
        iter->entry = flexCount(iter->map) ? flexTail(iter->map) : NULL;
        return multimapMediumIteratorNext(iter, elements);
    }

    return false;
}

bool multimapMediumDeleteByPredicate(multimapMedium *m,
                                     const multimapPredicate *p) {
    const size_t mapIdx =
        multimapMediumBinarySearch_(m, &p->compareAgainst, false, NULL);

    /* Boundary search, NOT an exact-match search: deleting everything
     * at or below a limit must work for any limit, not only for one
     * that happens to exist as a key. */
    flexEntry *startP = flexGetByTypeSortedWithMiddle(
        m->map[mapIdx], m->elementsPerEntry, &p->compareAgainst,
        GET_MIDDLE(m, mapIdx));

    /* If the found value is the END OF THE MAP, don't continue! */
    if (!startP) {
        return false;
    }

    /* By default, assume the value we found is at the end of the list. */
    int compared = 1;
    if (flexEntryIsValid(m->map[mapIdx], startP)) {
        databoxBig value;
        flexGetByTypeBig(startP, &value);

        compared = databoxCompare((databox *)&value, &p->compareAgainst);
    }

    switch (p->condition) {
    case MULTIMAP_CONDITION_LESS_THAN_EQUAL:
        if (mapIdx == 1) {
            /* The boundary lives in map 1, so all of map 0 sorts below
             * it and can be dropped with no traversal. */
            flexReset(&m->map[0]);
            SET_MIDDLE_FORCE(m, 0);
        }

        /* see multimapSmall for comments */
        if (compared == 0) {
            flexDeleteUpToInclusivePlusN(&m->map[mapIdx], startP,
                                         m->elementsPerEntry - 1);
        } else {
            flexDeleteUpToInclusive(&m->map[mapIdx],
                                    flexPrev(m->map[mapIdx], startP));
        }

        SET_MIDDLE_FORCE(m, mapIdx);

        /* Draining map 0 must never strand data in map 1, where
         * searches (which cut off on an empty map 0) can't reach it. */
        multimapMediumConform_(m, 0);
        return true;
    default:
        /* We don't support the other predicate types yet */
        assert(NULL && "Not Implemented!");
        return false;
    }
    return false;
}

#ifdef DATAKIT_TEST
#define CTEST_INCLUDE_KEYGEN
#define CTEST_INCLUDE_VALGEN
#include "ctest.h"

void multimapMediumRepr(const multimapMedium *m) {
    printf("MAPS {totalCount %zu}\n", multimapMediumCount(m));
    printf("Map Counts: ");
    for (size_t i = 0; i < 2; i++) {
        const size_t elementsInMap = flexCount(m->map[i]) / m->elementsPerEntry;
        if (i == 0) {
            printf("[%zu] -> ", elementsInMap);
        } else {
            printf("[%zu]\n", elementsInMap);
        }
    }

    flexRepr(m->map[0]);
    flexRepr(m->map[1]);
}

void multimapMediumVerify(const multimapMedium *m) {
    /* middle oracle: both cached byte offsets must equal fresh rescans;
     * sorted finds trust them blindly, so any divergence is corruption.
     * (Medium routes between maps by reading map[1]'s head LIVE on every
     * search, so there is no cached rangeBox to verify here.) */
    for (size_t i = 0; i < 2; i++) {
        assert(GET_MIDDLE(m, i) == flexMiddle(m->map[i], m->elementsPerEntry));
    }
}

/* Build a multimapMedium populated with `count` key/value pairs split
 * across the two maps at `splitAt` (logical entry index within the
 * combined sorted run).  Keys are integers k = 0..count-1.  Values
 * are bytes initialized to 'v'.  The test asserts the split lands on
 * a valid entry boundary. */
static multimapMedium *buildSplitMedium(size_t count, size_t splitAt) {
    assert(splitAt < count);
    /* multimapMediumNewFromOneGrow below consumes this flex */
    /* @own: container; free=multimapMediumFree */
    flex *f = flexNew();
    for (size_t i = 0; i < count; i++) {
        const databox keybox = databoxNewUnsigned(i);
        const databox valbox = databoxNewBytesString("v");
        flexPushByType(&f, &keybox, FLEX_ENDPOINT_TAIL);
        flexPushByType(&f, &valbox, FLEX_ENDPOINT_TAIL);
    }

    /* Find the byte offset of the split entry (splitAt-th key).  The
     * split sizes both halves from the row count, so it must be the
     * flex's middle row. */
    flexEntry *splitEntry = flexIndex(f, (int32_t)(splitAt * 2));
    assert(splitEntry != NULL);
    assert(splitEntry == flexMiddle(f, 2));
    const uint32_t middle = (uint32_t)(splitEntry - f);

    return multimapMediumNewFromOneGrow(NULL, f, middle, 2, false);
}

/* Count how many of the inserted keys live in each map. */
static void countByMap(multimapMedium *m, size_t expectedKeys, size_t *outMap0,
                       size_t *outMap1) {
    *outMap0 = 0;
    *outMap1 = 0;
    for (size_t i = 0; i < expectedKeys; i++) {
        const databox keybox = databoxNewUnsigned(i);
        multimapEntry me = {0};
        if (multimapMediumGetUnderlyingEntry(m, &keybox, &me)) {
            if (me.mapIdx == 0) {
                (*outMap0)++;
            } else {
                (*outMap1)++;
            }
        }
    }
}

int multimapMediumTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    /* multimapMedium has a fixed 2-map layout: map[0] holds keys
     * strictly less than head[map[1]], map[1] holds the rest.  Unlike
     * multimapFull, there is NO split-on-overflow path — every insert
     * lands in exactly one map chosen by binary search.  These tests
     * pin down the boundary behavior so that any future change to the
     * routing or insertion paths is caught.
     *
     * The risk shape we're checking for (the same shape as the
     * recently-fixed multimapFull bug): a REPLACE on an existing key
     * could be misrouted to the OTHER map, leaving two copies that
     * desynchronize the structure. */

    TEST("medium: REPLACE on key in map[0] stays in map[0]") {
        multimapMedium *m = buildSplitMedium(10, 5);
        /* Pre-state: keys 0..4 in map[0], 5..9 in map[1]. */
        const size_t startCount = multimapMediumCount(m);
        assert(startCount == 10);

        /* REPLACE key=2 with a much larger value to also force a
         * flex realloc — exercises the in-place grow path. */
        char bigVal[256];
        memset(bigVal, 'X', sizeof(bigVal) - 1);
        bigVal[sizeof(bigVal) - 1] = '\0';

        const databox keybox = databoxNewUnsigned(2);
        const databox valbox = databoxNewBytesString(bigVal);
        const databox *insert[2] = {&keybox, &valbox};
        const bool replaced = multimapMediumInsert(m, insert);
        if (!replaced) {
            ERRR("REPLACE on existing key in map[0] reported as new "
                 "insert — duplicate likely created!");
        }
        if (multimapMediumCount(m) != startCount) {
            ERR("Count changed from %zu to %zu after REPLACE — duplicate "
                "key created across maps!",
                startCount, multimapMediumCount(m));
        }

        /* Verify split invariant still holds: keys 0..4 in map[0],
         * keys 5..9 in map[1]. */
        size_t inMap0, inMap1;
        countByMap(m, 10, &inMap0, &inMap1);
        if (inMap0 != 5 || inMap1 != 5) {
            ERR("Split invariant broken: map[0]=%zu, map[1]=%zu after "
                "in-map[0] REPLACE",
                inMap0, inMap1);
        }
        multimapMediumFree(m);
    }

    TEST("medium: REPLACE on key in map[1] stays in map[1]") {
        multimapMedium *m = buildSplitMedium(10, 5);
        char bigVal[256];
        memset(bigVal, 'Y', sizeof(bigVal) - 1);
        bigVal[sizeof(bigVal) - 1] = '\0';

        const databox keybox = databoxNewUnsigned(7);
        const databox valbox = databoxNewBytesString(bigVal);
        const databox *insert[2] = {&keybox, &valbox};
        const bool replaced = multimapMediumInsert(m, insert);
        if (!replaced) {
            ERRR("REPLACE on existing key in map[1] reported as new "
                 "insert");
        }
        assert(multimapMediumCount(m) == 10);

        size_t inMap0, inMap1;
        countByMap(m, 10, &inMap0, &inMap1);
        if (inMap0 != 5 || inMap1 != 5) {
            ERR("Split invariant broken: map[0]=%zu, map[1]=%zu after "
                "in-map[1] REPLACE",
                inMap0, inMap1);
        }
        multimapMediumFree(m);
    }

    TEST("medium: REPLACE on boundary key (head of map[1])") {
        /* The most dangerous case: the key equals head[map[1]] exactly.
         * Binary search uses `compared <= 0 → map[1]`, so the REPLACE
         * routes to map[1] correctly.  But if the comparator ever
         * flipped to strict-less-than, the REPLACE would go to map[0],
         * leaving the original copy in map[1] orphaned and creating a
         * silent duplicate.  This test pins down current behavior. */
        multimapMedium *m = buildSplitMedium(10, 5);
        char bigVal[256];
        memset(bigVal, 'Z', sizeof(bigVal) - 1);
        bigVal[sizeof(bigVal) - 1] = '\0';

        /* key=5 is the head of map[1]. */
        const databox keybox = databoxNewUnsigned(5);
        const databox valbox = databoxNewBytesString(bigVal);
        const databox *insert[2] = {&keybox, &valbox};
        const bool replaced = multimapMediumInsert(m, insert);
        if (!replaced) {
            ERRR("REPLACE on boundary key (head of map[1]) reported as "
                 "new insert — routing must treat == as match!");
        }
        if (multimapMediumCount(m) != 10) {
            ERR("Boundary-key REPLACE changed count from 10 to %zu — "
                "key was inserted into map[0] as a duplicate!",
                multimapMediumCount(m));
        }

        /* Look up the boundary key and verify the new value (not the
         * stale "v") is returned. */
        databox foundVal = {0};
        databox *out[1] = {&foundVal};
        const bool ok = multimapMediumLookup(m, &keybox, out);
        assert(ok);
        if (foundVal.type != DATABOX_BYTES || foundVal.len != strlen(bigVal) ||
            memcmp(foundVal.data.bytes.start, bigVal, foundVal.len) != 0) {
            ERRR("Lookup on boundary key returned stale value — "
                 "binary search picked the wrong map!");
        }
        multimapMediumFree(m);
    }

    TEST("medium: many alternating REPLACEs across the boundary") {
        /* Stress: repeatedly REPLACE on both sides of the boundary
         * with varying value sizes.  If any insert is misrouted, the
         * count will diverge from the expected fixed total. */
        multimapMedium *m = buildSplitMedium(20, 10);
        assert(multimapMediumCount(m) == 20);

        for (int round = 0; round < 50; round++) {
            for (size_t k = 0; k < 20; k++) {
                /* Vary value size each round so the flex grows/shrinks. */
                char buf[64];
                const int sz = 8 + (int)((round + k) % 48);
                memset(buf, 'a' + (int)(k % 26), (size_t)sz);
                buf[sz] = '\0';
                const databox keybox = databoxNewUnsigned(k);
                const databox valbox = databoxNewBytesString(buf);
                const databox *insert[2] = {&keybox, &valbox};
                multimapMediumInsert(m, insert);
            }
            if (multimapMediumCount(m) != 20) {
                ERR("Round %d: count drifted to %zu (expected 20) — "
                    "REPLACEs misrouted!",
                    round, multimapMediumCount(m));
                break;
            }
        }
        multimapMediumFree(m);
    }

    TEST("medium: INSERT new key that equals current head[map[1]] "
         "is a REPLACE, not a duplicate") {
        /* If a caller hands us a key that happens to equal the
         * existing boundary key, that must be detected as a REPLACE
         * (since the key already exists).  This is the same property
         * as the boundary-REPLACE test above, expressed from the
         * INSERT direction. */
        multimapMedium *m = buildSplitMedium(10, 5);
        const databox keybox = databoxNewUnsigned(5);
        const databox valbox = databoxNewBytesString("brand-new");
        const databox *insert[2] = {&keybox, &valbox};
        const bool replaced = multimapMediumInsert(m, insert);
        assert(replaced);
        assert(multimapMediumCount(m) == 10);
        multimapMediumFree(m);
    }

    TEST("medium: empty map[1] routes all inserts to map[0]") {
        /* Construct a degenerate medium where map[1] is empty (this
         * can happen briefly during certain transitions).  Binary
         * search must short-circuit to map[0] without dereferencing
         * an empty map[1]. */
        cleaner(multimapMediumFree) multimapMedium *m = multimapMediumNew(2);
        for (size_t i = 0; i < 5; i++) {
            const databox keybox = databoxNewUnsigned(i);
            const databox valbox = databoxNewBytesString("v");
            const databox *insert[2] = {&keybox, &valbox};
            multimapMediumInsert(m, insert);
        }
        assert(multimapMediumCount(m) == 5);
        assert(flexCount(m->map[1]) == 0);

        /* REPLACE one of them. */
        const databox keybox = databoxNewUnsigned(2);
        const databox valbox = databoxNewBytesString("v2");
        const databox *insert[2] = {&keybox, &valbox};
        const bool replaced = multimapMediumInsert(m, insert);
        assert(replaced);
        assert(multimapMediumCount(m) == 5);
    }

    TEST("reset then reuse: both maps populated, two cycles") {
        /* Fill past the split boundary so BOTH internal maps hold
         * entries, then verify reset leaves a fully reusable map
         * (the Full-tier reset kept stale metadata; pin Medium too) */
        cleaner(multimapMediumFree) multimapMedium *m = multimapMediumNew(2);

        for (int cycle = 0; cycle < 2; cycle++) {
            for (uint64_t i = 0; i < 600; i++) {
                const databox keybox = databoxNewUnsigned(i);
                const databox valbox = databoxNewBytesString("reset-cycle-v");
                const databox *insert[2] = {&keybox, &valbox};
                multimapMediumInsert(m, insert);
            }

            if (multimapMediumCount(m) != 600) {
                ERR("cycle %d: expected 600, got %zu", cycle,
                    multimapMediumCount(m));
            }

            multimapMediumReset(m);

            if (multimapMediumCount(m) != 0) {
                ERR("cycle %d: expected 0 after reset, got %zu", cycle,
                    multimapMediumCount(m));
            }
        }

        /* Final refill: every key must be findable */
        size_t missing = 0;
        for (uint64_t i = 0; i < 600; i++) {
            const databox keybox = databoxNewUnsigned(i);
            const databox valbox = databoxNewBytesString("post-reset-v");
            const databox *insert[2] = {&keybox, &valbox};
            multimapMediumInsert(m, insert);
        }
        for (uint64_t i = 0; i < 600; i++) {
            const databox probe = databoxNewUnsigned(i);
            if (!multimapMediumExists(m, &probe)) {
                missing++;
            }
        }
        if (missing > 0) {
            ERR("%zu of 600 keys missing after reset-reuse", missing);
        }
    }

    TEST("medium: reverse iteration with an empty map[1]") {
        /* Deleting every key of map[1] leaves map[0] holding all rows.
         * Reverse iteration starts at the tail of map[1], which an empty
         * flex doesn't have, so it must start in map[0]. */
        cleaner(multimapMediumFree) multimapMedium *m = buildSplitMedium(10, 5);
        for (uint64_t i = 5; i < 10; i++) {
            const databox keybox = databoxNewUnsigned(i);
            if (!multimapMediumDelete(m, &keybox)) {
                ERR("delete of key %" PRIu64 " failed", i);
            }
        }

        if (flexCount(m->map[1]) != 0 || multimapMediumCount(m) != 5) {
            ERR("expected map[1] empty with 5 rows, got %zu rows",
                multimapMediumCount(m));
        }

        multimapIterator iter;
        multimapMediumIteratorInit(m, &iter, false);
        databox key = {{0}};
        databox val = {{0}};
        databox *elements[2] = {&key, &val};
        uint64_t expect = 5;
        while (multimapMediumIteratorNext(&iter, elements)) {
            if (expect == 0 || key.data.u != expect - 1) {
                ERR("reverse iteration gave key %" PRIu64 ", expected "
                    "%" PRIu64,
                    (uint64_t)key.data.u, expect - 1);
                break;
            }

            expect--;
        }

        if (expect != 0) {
            ERR("reverse iteration stopped with %" PRIu64 " rows left", expect);
        }
    }

    TEST("medium: 96/128-bit keys route between the two maps") {
        /* map[1]'s head key is read on every search to pick a map; with
         * wide keys that read needs wide storage.  Rows are {key, value}
         * with wide keys of both signs and wide values, split 5/5. */
        const __uint128_t base = (__uint128_t)1 << 100;
        __uint128_t keys[12];
        __uint128_t vals[12];
        databox keyBox[12];
        databox valBox[12];
        for (size_t i = 0; i < 12; i++) {
            /* Rows 0-3 are negative (-(2^100 + 4 - i)), the rest positive
             * (2^100 + i); rows 10 and 11 are inserted later. */
            const bool neg = i < 4;
            keys[i] = neg ? (__uint128_t)0 - (base + (4 - i)) : base + i;
            vals[i] = ((__uint128_t)1 << 90) + i * 7;
            keyBox[i] = (databox){.type = neg ? DATABOX_SIGNED_128
                                              : DATABOX_UNSIGNED_128,
                                  .big = true,
                                  .data.u128 = &keys[i]};
            valBox[i] = (databox){.type = DATABOX_UNSIGNED_128,
                                  .big = true,
                                  .data.u128 = &vals[i]};
        }

        /* @own: container; free=multimapMediumFree */
        flex *f = flexNew();
        for (size_t i = 0; i < 10; i++) {
            flexPushByType(&f, &keyBox[i], FLEX_ENDPOINT_TAIL);
            flexPushByType(&f, &valBox[i], FLEX_ENDPOINT_TAIL);
        }

        flexEntry *split = flexMiddle(f, 2);
        cleaner(multimapMediumFree) multimapMedium *m =
            multimapMediumNewFromOneGrow(NULL, f, (uint32_t)(split - f), 2,
                                         false);

        /* A key below every row, and one above, through Insert. */
        __uint128_t lowKey = (__uint128_t)0 - (base << 1);
        keyBox[10] = (databox){
            .type = DATABOX_SIGNED_128, .big = true, .data.u128 = &lowKey};
        keys[10] = lowKey;
        keys[11] = base << 1;
        for (size_t i = 10; i < 12; i++) {
            const databox *row[2] = {&keyBox[i], &valBox[i]};
            multimapMediumInsert(m, row);
        }

        for (size_t i = 0; i < 12; i++) {
            /* Rows 0-4 and the low key live in map[0]. */
            const size_t wantMap = (i < 5 || i == 10) ? 0 : 1;
            multimapEntry me = {0};
            if (!multimapMediumGetUnderlyingEntry(m, &keyBox[i], &me) ||
                me.mapIdx != wantMap) {
                ERR("row %zu: found in map %" PRIu32 ", expected map %zu", i,
                    me.mapIdx, wantMap);
            }

            databoxBig out;
            memset(&out, 0xFF, sizeof(out));
            DATABOX_BIG_INIT(&out);
            databox *values[1] = {(databox *)&out};
            if (!multimapMediumLookup(m, &keyBox[i], values) ||
                out.type != DATABOX_UNSIGNED_128 || *out.data.u128 != vals[i]) {
                ERR("row %zu: lookup gave a wrong value", i);
            }

            const databox *row[2] = {&keyBox[i], &valBox[i]};
            if (!multimapMediumExistsFullWidth(m, row)) {
                ERR("row %zu: full-width exists failed", i);
            }
        }

        /* A second row for key 7 with a smaller value: the full-width
         * route compares map[1]'s head row element by element. */
        __uint128_t smallerVal = vals[7] - 1;
        const databox smaller = {.type = DATABOX_UNSIGNED_128,
                                 .big = true,
                                 .data.u128 = &smallerVal};
        const databox *dupRow[2] = {&keyBox[7], &smaller};
        multimapMediumInsertFullWidth(m, dupRow);
        if (!multimapMediumExistsFullWidth(m, dupRow) ||
            multimapMediumCount(m) != 13) {
            ERRR("full-width duplicate of a wide key not stored");
        }

        /* A wide field can't be incremented: FieldIncr reports 0. */
        if (multimapMediumFieldIncr(m, &keyBox[2], 1, 1) != 0) {
            ERRR("FieldIncr of a wide value returned non-zero");
        }

        /* Trim every row at or below key 6, which lives in map[1]. */
        const multimapPredicate p = {.condition =
                                         MULTIMAP_CONDITION_LESS_THAN_EQUAL,
                                     .compareAgainst = keyBox[6]};
        multimapMediumDeleteByPredicate(m, &p);
        for (size_t i = 0; i < 12; i++) {
            const bool keep = i >= 7 && i != 10;
            if (multimapMediumExists(m, &keyBox[i]) != keep) {
                ERR("after trim: row %zu exists %d, expected %d", i, !keep,
                    keep);
            }
        }

        multimapMediumVerify(m);
    }

    TEST("medium: routing resolves a 96/128-bit atom at map[1]'s head") {
        /* Surrogate-keyed rows store atom references; routing resolves
         * map[1]'s head reference to its value.  The atom table holds a
         * narrow value and a wide one; the wide one is inserted last, so
         * building the table never resolves it. */
        multimapAtom *ma = multimapAtomNew();
        const databox narrow = databoxNewSigned(10);
        multimapAtomInsert(ma, &narrow);

        __uint128_t wideAtom = (__uint128_t)1 << 70;
        const databox wide = {
            .type = DATABOX_UNSIGNED_128, .big = true, .data.u128 = &wideAtom};
        multimapAtomInsert(ma, &wide);

        /* Atom ids follow insertion order: id 0 is 10 and id 1 is wide.
         * Splitting the two rows at the middle puts id 1 in map[1]. */
        /* @own: container; free=multimapMediumFree */
        flex *f = flexNew();
        for (uint64_t id = 0; id <= 1; id++) {
            const databox ref = {.type = DATABOX_CONTAINER_REFERENCE_EXTERNAL,
                                 .data.u = id};
            flexPushByType(&f, &ref, FLEX_ENDPOINT_TAIL);
        }

        flexEntry *split = flexMiddle(f, 1);
        cleaner(multimapMediumFree) multimapMedium *m =
            multimapMediumNewFromOneGrow(NULL, f, (uint32_t)(split - f), 1,
                                         false);

        __uint128_t above = (__uint128_t)1 << 80;
        __uint128_t below = ((__uint128_t)1 << 70) - 1;
        const struct {
            databox probe;
            size_t mapIdx;
        } cases[] = {
            {databoxNewSigned(30), 0},
            {databoxNewUnsigned(UINT64_MAX), 0},
            {{.type = DATABOX_UNSIGNED_128, .big = true, .data.u128 = &below},
             0},
            {wide, 1},
            {{.type = DATABOX_UNSIGNED_128, .big = true, .data.u128 = &above},
             1},
        };

        for (size_t i = 0; i < COUNT_ARRAY(cases); i++) {
            const size_t got =
                multimapMediumBinarySearchWithReference(m, &cases[i].probe, ma);
            if (got != cases[i].mapIdx) {
                ERR("probe %zu routed to map %zu, expected %zu", i, got,
                    cases[i].mapIdx);
            }
        }

        multimapAtomFree(ma);
    }

    TEST_FINAL_RESULT;
}
#endif
