#include "multimapMedium.h"
#include "multimapMediumInternal.h"

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
    multimapMedium *m = zcalloc(1, sizeof(*m));

    m->elementsPerEntry = elementsPerEntry;

    m->map[0] = flexNew();
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
    multimapMedium *copy = zcalloc(1, sizeof(*copy));
    *copy = *m;

    copy->map[0] = flexDuplicate(m->map[0]);
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
    databox got;
    if (useReference) {
        flexGetByTypeWithReference(head, &got, referenceContainer);
    } else {
        flexGetByType(head, &got);
    }

    const int compared = databoxCompare(&got, key);

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
    databox got;
    flexEntry *start = flexHead(m->map[1]);
    size_t mapIdx = 1;

    /* if head == tail, then map[1] has no elements and we use
     * map[0] for insert / search. */
    if (start == flexTail(m->map[1])) {
        return 0;
    }

    flexGetByType(start, &got);

    for (size_t i = 0; i < m->elementsPerEntry; i++) {
        const int compared = databoxCompare(&got, elements[i]);

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
            flexGetByType(start, &got);
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
    flexInsertReplaceByTypeSortedWithMiddleMultiWithReferenceWithSurrogateKey(
        &m->map[mapIdx], m->elementsPerEntry, elements, insertKey, &middle,
        m->mapIsSet, referenceContainer);
    SET_MIDDLE(m, mapIdx, middle);
}

bool multimapMediumInsert(multimapMedium *m, const databox *elements[]) {
    size_t mapIdx = multimapMediumBinarySearch(m, elements[0]);

    flexEntry *middle = GET_MIDDLE(m, mapIdx);
    const bool replaced = flexInsertReplaceByTypeSortedWithMiddleMultiDirect(
        &m->map[mapIdx], m->elementsPerEntry, elements, &middle, m->mapIsSet);
    SET_MIDDLE(m, mapIdx, middle);
    return replaced;
}

void multimapMediumInsertFullWidth(multimapMedium *m,
                                   const databox *elements[]) {
    size_t mapIdx = multimapMediumBinarySearchFullWidth(m, elements);

    flexEntry *middle = GET_MIDDLE(m, mapIdx);
    flexInsertReplaceByTypeSortedWithMiddleMultiDirect(
        &m->map[mapIdx], m->elementsPerEntry, elements, &middle, m->mapIsSet);
    SET_MIDDLE(m, mapIdx, middle);
}

void multimapMediumAppend(multimapMedium *m, const databox *elements[]) {
    static const size_t mapIdx = 1; /* always insert into highest map! */

    flexEntry *middle = GET_MIDDLE(m, mapIdx);
    flexInsertReplaceByTypeSortedWithMiddleMultiDirect(
        &m->map[mapIdx], m->elementsPerEntry, elements, &middle, m->mapIsSet);
    SET_MIDDLE(m, mapIdx, middle);
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
    /* copy/paste of the common case from abstractGetUnderlyingEntry() */
    me->mapIdx = multimapMediumBinarySearch_(m, key, false, NULL);
    me->fe =
        flexFindByTypeSortedWithMiddle(m->map[me->mapIdx], m->elementsPerEntry,
                                       key, GET_MIDDLE(m, me->mapIdx));
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

bool multimapMediumRandomValue(multimapMedium *m, const bool fromTail,
                               databox **foundBox, multimapEntry *me) {
    if (multimapMediumCount(m) == 0) {
        return false;
    }

    /* Step 1: pick victim map */
    const size_t mapIdx = random() % 2;
    flex **map = &m->map[mapIdx];

    /* Step 2: pick victim element */
    flexEntry *foundP = NULL;
    if (fromTail) {
        foundP = flexTailWithElements(*map, m->elementsPerEntry);
    } else {
        /* delete random entry in the map */
        const size_t totalWholeElements = flexCount(*map) / m->elementsPerEntry;
        const size_t randomElement = random() % totalWholeElements;

        /* Restore random element to an offset inside the list */
        foundP = flexIndex(*map, randomElement * m->elementsPerEntry);
    }

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
    if (!multimapMediumRandomValue(m, deleteFromTail, deletedBox, &me)) {
        return false;
    }

    multimapMediumDeleteEntry(m, &me);
    return true;
}

DK_INLINE_ALWAYS bool
abstractDelete(multimapMedium *m, const databox **elements,
               const bool fullWidth, const bool useReference,
               const struct multimapAtom *referenceContainer,
               databox *foundReference) {
    size_t mapIdx;

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

int64_t multimapMediumFieldIncr(multimapMedium *m, const databox *key,
                                uint32_t fieldOffset, int64_t incrBy) {
    size_t mapIdx = multimapMediumBinarySearch(m, key);

    flexEntry *current = flexFindByTypeSortedWithMiddle(
        m->map[mapIdx], m->elementsPerEntry, key, GET_MIDDLE(m, mapIdx));
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

    /* (unlikely) else, return current value */
    databox curVal;
    flexGetByType(current, &curVal);
    return curVal.data.i;
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
        me.fe = NULL;
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
            tmp = m->map[1];
            me.mapIdx = 1;
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
    multimapEntry me = {0};
    multimapMediumGetUnderlyingEntry(m, &p->compareAgainst, &me);

    if (!me.fe) {
        return false;
    }

    int compared = 1;
    if (!flexEntryIsValid(*me.map, me.fe)) {
        databox value;
        flexGetByType(me.fe, &value);

        compared = databoxCompare(&value, &p->compareAgainst);
    }

    switch (p->condition) {
    case MULTIMAP_CONDITION_LESS_THAN_EQUAL:
        if (me.mapIdx == 1) {
            /* If we found our entry in map 1, then all of
             * map 0 can be deleted with no traversal. */
            flexReset(&m->map[0]);
            SET_MIDDLE_FORCE(m, 0);
        }

        if (!me.fe) {
            return false;
        }

        /* see multimapSmall for comments */
        if (compared == 0) {
            flexDeleteUpToInclusivePlusN(me.map, me.fe,
                                         m->elementsPerEntry - 1);
        } else {
            flexDeleteUpToInclusive(me.map, flexPrev(*me.map, me.fe));
        }
        SET_MIDDLE_FORCE(m, me.mapIdx);
        return true;
    default:
        /* We don't support the other predicate types yet */
        assert(NULL && "Not Implemented!");
        return false;
    }
    return false;
}

#ifdef DATAKIT_TEST
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
#endif
