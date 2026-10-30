#include "multimapSmall.h"
#include "dkRandom.h"
#include "multimapInternal.h"
#include "multimapSmallInternal.h"

multimapSmall *multimapSmallNew(multimapElements elementsPerEntry,
                                bool mapIsSet) {
    /* @own: caller; free=multimapSmallFree */
    multimapSmall *m = zcalloc(1, sizeof(*m));

    m->elementsPerEntry = elementsPerEntry;
    m->mapIsSet = mapIsSet;

    /* @own: container; free=multimapSmallFree */
    m->map = flexNew();
    m->middle = FLEX_EMPTY_SIZE;

    return m;
}

multimapSmall *multimapSmallCopy(const multimapSmall *const m) {
    /* @own: caller; free=multimapSmallFree */
    multimapSmall *copy = zcalloc(1, sizeof(*copy));
    *copy = *m;

    /* @own: container; free=multimapSmallFree */
    copy->map = flexDuplicate(m->map);

    return copy;
}

size_t multimapSmallCount(const multimapSmall *m) {
    return flexCount(m->map) / m->elementsPerEntry;
}

size_t multimapSmallBytes(const multimapSmall *m) {
    return flexBytes(m->map);
}

flex *multimapSmallDump(const multimapSmall *m) {
    /* @own: caller; free=flexFree */
    return flexDuplicate(m->map);
}

#define GET_MIDDLE(m) (flexEntry *)((m)->map + (m)->middle)
#define SET_MIDDLE(m, mid)                                                     \
    do {                                                                       \
        (m)->middle = (mid) - (m)->map;                                        \
    } while (0)

#define SET_MIDDLE_FORCE(m)                                                    \
    do {                                                                       \
        SET_MIDDLE(m, flexMiddle((m)->map, (m)->elementsPerEntry));            \
    } while (0)

void multimapSmallInsertWithSurrogateKey(
    multimapSmall *m, const databox *elements[], const databox *insertKey,
    const struct multimapAtom *referenceContainer) {
    flexEntry *middle = GET_MIDDLE(m);
    /* compareUsingKeyElementOnly: For maps (!mapIsSet), compare only key.
     * For sets (mapIsSet), compare all elements to check for full-width dups.
     */
    flexInsertReplaceByTypeSortedWithMiddleMultiWithReferenceWithSurrogateKey(
        &m->map, m->elementsPerEntry, elements, insertKey, &middle,
        !m->mapIsSet, referenceContainer);
    SET_MIDDLE(m, middle);
}

/* As multimapInsertWithSurrogateKeyIfAbsent (multimap.h) */
bool multimapSmallInsertWithSurrogateKeyIfAbsent(
    multimapSmall *m, const databox *elements[], const databox *insertKey,
    const struct multimapAtom *referenceContainer, databox *foundKey,
    flexWhenAbsent *whenAbsent, void *whenAbsentCtx) {
    flexEntry *middle = GET_MIDDLE(m);
    flexEntry *found = nullptr;
    if (flexInsertByTypeSortedWithMiddleMultiWithReferenceWithSurrogateKeyIfAbsent(
            &m->map, m->elementsPerEntry, elements, insertKey, &middle,
            referenceContainer, &found, whenAbsent, whenAbsentCtx)) {
        flexGetByType(found, foundKey);
        return false;
    }

    SET_MIDDLE(m, middle);
    return true;
}

bool multimapSmallInsert(multimapSmall *m, const databox *elements[]) {
    return multimapSmallInsertReplacing(m, elements, nullptr);
}

/* 'replacedOut' (NULL, or 'elementsPerEntry - 1' slots) reports the value
 * entries a map replace overwrote; a set never overwrites a value, so a set
 * reports every slot VOID. */
bool multimapSmallInsertReplacing(multimapSmall *m, const databox *elements[],
                                  databox *replacedOut) {
    flexEntry *middle = GET_MIDDLE(m);
    /* Maps (!mapIsSet): key-only compare, found => replace values.
     * Sets (mapIsSet): full-width compare AND dedupe — the Set variant
     * makes a found full-width-equal entry a no-op.  The plain Direct
     * call with compareUsingKeyElementOnly=false is the
     * duplicates-ALLOWED sorted bag (InsertFullWidth), which silently
     * duplicated every re-inserted set member here for years. */
    bool replaced;
    if (m->mapIsSet) {
        replaced = flexInsertReplaceByTypeSortedWithMiddleMultiDirectSet(
            &m->map, m->elementsPerEntry, elements, &middle);
        multimapReplacedOutClear_(replacedOut, m->elementsPerEntry);
    } else {
        replaced = flexInsertReplaceByTypeSortedWithMiddleMultiDirectReplacing(
            &m->map, m->elementsPerEntry, elements, &middle, true, replacedOut);
    }
    SET_MIDDLE(m, middle);
    return replaced;
}

/* Inserts the row only when no row has its key, in one search: true when
 * inserted, false when the key was present (the map unchanged, and 'me',
 * when non-NULL, its row; a set's first row with the key). */
bool multimapSmallInsertIfAbsent(multimapSmall *m, const databox *elements[],
                                 multimapEntry *me) {
    flexEntry *middle = GET_MIDDLE(m);
    flexEntry *found = nullptr;
    /* A set's present key comes back at its first row, as its lookups
     * report it: no second search */
    if (flexInsertByTypeSortedWithMiddleMultiDirectIfAbsent(
            &m->map, m->elementsPerEntry, elements, &middle, &found,
            m->mapIsSet && me)) {
        if (me) {
            me->fe = found;
            me->map = &m->map;
        }
        return false;
    }

    SET_MIDDLE(m, middle);
    return true;
}

void multimapSmallInsertFullWidth(multimapSmall *m, const databox *elements[]) {
    flexEntry *middle = GET_MIDDLE(m);
    /* InsertFullWidth: Always compare ALL elements (key+value) to allow
     * multiple entries with same key but different values (sorted set). */
    flexInsertReplaceByTypeSortedWithMiddleMultiDirect(
        &m->map, m->elementsPerEntry, elements, &middle, false);
    SET_MIDDLE(m, middle);
}

void multimapSmallAppend(multimapSmall *m, const databox *elements[]) {
    /* Same implementation because we only have one map. */
    multimapSmallInsert(m, elements);
}

/* The row 'key' addresses.  A set can hold several rows with one key: its
 * first row, so which row a lookup, delete or increment touches never
 * depends on where the binary search happened to land. */
DK_INLINE_ALWAYS flexEntry *
multimapSmallFindKey_(multimapSmall *m, const databox *key,
                      const bool useReference,
                      const multimapAtom *referenceContainer) {
    flexEntry *middle = GET_MIDDLE(m);
    if (useReference) {
        return m->mapIsSet ? flexFindByTypeSortedWithMiddleLowestWithReference(
                                 m->map, m->elementsPerEntry, key, middle,
                                 referenceContainer)
                           : flexFindByTypeSortedWithMiddleWithReference(
                                 m->map, m->elementsPerEntry, key, middle,
                                 referenceContainer);
    }
    return m->mapIsSet ? flexFindByTypeSortedWithMiddleLowest(
                             m->map, m->elementsPerEntry, key, middle)
                       : flexFindByTypeSortedWithMiddle(
                             m->map, m->elementsPerEntry, key, middle);
}

bool multimapSmallGetUnderlyingEntry(multimapSmall *m, const databox *key,
                                     multimapEntry *me) {
    me->fe = multimapSmallFindKey_(m, key, false, nullptr);
    me->map = &m->map;
    return !!me->fe;
}

bool multimapSmallGetUnderlyingEntryWithReference(
    multimapSmall *m, const databox *key, multimapEntry *me,
    const multimapAtom *referenceContainer) {
    me->fe = multimapSmallFindKey_(m, key, true, referenceContainer);
    me->map = &m->map;
    return !!me->fe;
}

bool multimapSmallGetUnderlyingEntryFullWidth(multimapSmall *m,
                                              const databox *elements[],
                                              multimapEntry *me) {
    me->fe = flexFindByTypeSortedWithMiddleFullWidth(
        m->map, m->elementsPerEntry, elements, GET_MIDDLE(m));
    me->map = &m->map;
    return !!me->fe;
}

/* Delete the row 'me' found (by GetUnderlyingEntry*), without a search.
 * A Small map keeps no range boxes, so 'referenceContainer' is unused. */
void multimapSmallDeleteEntryWithReference(
    multimapSmall *m, multimapEntry *me,
    [[maybe_unused]] const multimapAtom *referenceContainer) {
    flexEntry *middle = GET_MIDDLE(m);
    flexDeleteSortedValueWithMiddle(&m->map, m->elementsPerEntry, me->fe,
                                    &middle);
    SET_MIDDLE(m, middle);
}

/* Resizing/replacing one entry keeps the entry COUNT, so the middle ROW
 * is unchanged — but its BYTE offset is not recoverable from the entry's
 * own size delta alone: when the map's total size crosses a varint width
 * boundary, the flex container header itself grows or shrinks and every
 * entry shifts, so any O(1) adjustment computed from a pre-mutation
 * entry offset reads a misaligned position and poisons the cached
 * middle for the map's remaining lifetime (sorted finds trust it
 * blindly and insert/delete only ever maintain it incrementally).
 * The full rescan is the only computation correct in all cases where the
 * map's size changed; a map of the same size kept every entry in place. */
void multimapSmallResizeEntry(multimapSmall *m, multimapEntry *me,
                              size_t newLen) {
    const size_t bytesBefore = flexBytes(*me->map);
    flexResizeEntry(me->map, me->fe, newLen);
    if (flexBytes(*me->map) != bytesBefore) {
        SET_MIDDLE_FORCE(m);
    }
}

void multimapSmallReplaceEntry(multimapSmall *m, multimapEntry *me,
                               const databox *box) {
    const size_t bytesBefore = flexBytes(*me->map);
    flexReplaceByType(me->map, me->fe, box);
    if (flexBytes(*me->map) != bytesBefore) {
        SET_MIDDLE_FORCE(m);
    }
}

bool multimapSmallExists(multimapSmall *m, const databox *key) {
    return !!flexFindByTypeSortedWithMiddle(m->map, m->elementsPerEntry, key,
                                            GET_MIDDLE(m));
}

bool multimapSmallExistsFullWidth(multimapSmall *m, const databox *elements[]) {
    return !!flexFindByTypeSortedWithMiddleFullWidth(
        m->map, m->elementsPerEntry, elements, GET_MIDDLE(m));
}

bool multimapSmallExistsWithReference(multimapSmall *m, const databox *key,
                                      databox *foundRef,
                                      const multimapAtom *referenceContainer) {
    const flexEntry *found = flexFindByTypeSortedWithMiddleWithReference(
        m->map, m->elementsPerEntry, key, GET_MIDDLE(m), referenceContainer);

    if (found) {
        flexGetByType(found, foundRef);
        return true;
    }

    return false;
}

DK_INLINE_ALWAYS bool abstractLookup(multimapSmall *m, const databox *key,
                                     databox *elements[],
                                     const bool useReference,
                                     const multimapAtom *referenceContainer) {
    flexEntry *foundP =
        multimapSmallFindKey_(m, key, useReference, referenceContainer);

    if (foundP) {
        flexEntry *nextFound = foundP;
        for (multimapElements i = 1; i < m->elementsPerEntry; i++) {
            /* We do Next() *before* because for the initial loop
             * we want to skip over the key.
             * We don't need to return the key to users since they
             * *provided* the key we used to lookup.  So, we skip
             * over the key for returning values to users.
             * Also, putting the lookup first saves us from a
             * "dangling next" at the end of the traversal
             * so we don't need to run any Next() calls if
             * we aren't going to use them. */
            nextFound = flexNext(m->map, nextFound);
            flexGetByType(nextFound, elements[i - 1]);
        }
    }
    return !!foundP;
}

bool multimapSmallLookup(multimapSmall *m, const databox *key,
                         databox *elements[]) {
    return abstractLookup(m, key, elements, false, nullptr);
}

bool multimapSmallLookupWithReference(multimapSmall *m, const databox *key,
                                      databox *elements[],
                                      const multimapAtom *referenceContainer) {
    return abstractLookup(m, key, elements, true, referenceContainer);
}

bool multimapSmallRandomValue(multimapSmall *m, const bool fromTail,
                              databox **foundBox, multimapEntry *me) {
    const size_t count = flexCount(m->map);
    if (count == 0) {
        return false;
    }

    /* Step 2: pick victim element */
    flexEntry *foundP = nullptr;
    if (fromTail) {
        foundP = flexTailWithElements(m->map, m->elementsPerEntry);
    } else {
        /* Pick a uniformly random entry with a per-thread draw. */
        const uint64_t totalWholeElements =
            (uint64_t)count / m->elementsPerEntry;
        const uint64_t randomElement = dkRandomBounded(totalWholeElements);

        /* Restore random element to an offset inside the list, walking
         * from the nearest of head/middle/tail.  The offset is below
         * 'count', and the int32_t narrowing follows flex's own index
         * contract: flexIndex and flexIndexWithMiddle take int32_t, so a
         * flex addressed through them holds at most INT32_MAX entries.  A
         * standalone Small map never upgrades, so this, not the generic
         * multimap's tier limit, is what bounds it. */
        assert(count <= INT32_MAX);
        foundP = flexIndexWithMiddle(
            m->map, m->elementsPerEntry,
            (int32_t)(randomElement * m->elementsPerEntry), GET_MIDDLE(m));
    }

    me->map = &m->map;
    me->mapIdx = 0;
    me->fe = foundP;

    if (foundBox) {
        flexEntry *ffoundP = foundP;
        for (size_t i = 0; i < m->elementsPerEntry; i++) {
            flexGetByType(ffoundP, foundBox[i]);
            ffoundP = flexNext(m->map, ffoundP);
        }
    }

    return true;
}

static void multimapSmallDeleteEntry(multimapSmall *m, multimapEntry *me) {
    flex **map = me->map;
    flexEntry *middle = GET_MIDDLE(m);

    flexEntry *foundP = me->fe;

    flexDeleteSortedValueWithMiddle(map, m->elementsPerEntry, foundP, &middle);
    SET_MIDDLE(m, middle);
}

bool multimapSmallDeleteRandomValue(multimapSmall *m, const bool deleteFromTail,
                                    databox **deletedBox) {
    multimapEntry me;
    /* Pass NULL for deletedBox to multimapSmallRandomValue, then manually
     * copy the data using flexGetByTypeDetached. This avoids use-after-free
     * since flexGetByType returns pointers to internal flex data which
     * become invalid after multimapSmallDeleteEntry. */
    if (!multimapSmallRandomValue(m, deleteFromTail, nullptr, &me)) {
        return false;
    }

    /* Copy the data before deletion so caller has valid data */
    if (deletedBox) {
        flexEntry *fe = me.fe;
        for (size_t i = 0; i < m->elementsPerEntry; i++) {
            flexGetByTypeDetached(fe, deletedBox[i]);
            fe = flexNext(m->map, fe);
        }
    }

    multimapSmallDeleteEntry(m, &me);
    return true;
}

DK_INLINE_ALWAYS bool
abstractDelete(multimapSmall *m, const databox **elements, const bool fullWidth,
               const bool useReference, const multimapAtom *referenceContainer,
               databox *foundReference, databox *removedOut,
               const bool takeRemoved) {
    flexEntry *middle = GET_MIDDLE(m);
    const databox *key = elements[0];

    flexEntry *foundP;

    if (fullWidth) {
        if (useReference) {
            foundP = flexFindByTypeSortedWithMiddleFullWidthWithReference(
                m->map, m->elementsPerEntry, elements, middle,
                referenceContainer);
        } else {
            foundP = flexFindByTypeSortedWithMiddleFullWidth(
                m->map, m->elementsPerEntry, elements, middle);
        }
    } else {
        foundP =
            multimapSmallFindKey_(m, key, useReference, referenceContainer);
    }

    if (foundP) {
        if (foundReference) {
            /* The row is being deleted, so a 96/128-bit key is reported
             * type-only (see multimapDeleteWithFound). */
            flexGetByTypeInto(foundP, foundReference, nullptr);
        }

        multimapRemovedOutFill_(m->map, foundP, m->elementsPerEntry, removedOut,
                                takeRemoved);
        flexDeleteSortedValueWithMiddle(&m->map, m->elementsPerEntry, foundP,
                                        &middle);
        SET_MIDDLE(m, middle);
    }

    return !!foundP;
}

bool multimapSmallDelete(multimapSmall *m, const databox *key) {
    const databox *elements[1] = {key};
    return abstractDelete(m, elements, false, false, nullptr, nullptr, nullptr,
                          false);
}

bool multimapSmallDeleteReporting(multimapSmall *m, const databox *key,
                                  databox *removedOut) {
    const databox *elements[1] = {key};
    return abstractDelete(m, elements, false, false, nullptr, nullptr,
                          removedOut, false);
}

bool multimapSmallDeleteTaking(multimapSmall *m, const databox *key,
                               databox *takenOut) {
    const databox *elements[1] = {key};
    return abstractDelete(m, elements, false, false, nullptr, nullptr, takenOut,
                          true);
}

bool multimapSmallDeleteFullWidth(multimapSmall *m, const databox *elements[]) {
    return abstractDelete(m, elements, true, false, nullptr, nullptr, nullptr,
                          false);
}

bool multimapSmallDeleteWithReference(multimapSmall *m, const databox *key,
                                      const multimapAtom *referenceContainer,
                                      databox *foundReference) {
    const databox *elements[1] = {key};
    return abstractDelete(m, elements, false, true, referenceContainer,
                          foundReference, nullptr, false);
}

bool multimapSmallDeleteWithFound(multimapSmall *m, const databox *key,
                                  databox *foundReference) {
    const databox *elements[1] = {key};
    return abstractDelete(m, elements, false, true, nullptr, foundReference,
                          nullptr, false);
}

/* Row entry at position 'keyIndex' (0-based forward; negative =
 * 1-based from the tail), walking from the nearest of the maintained
 * head/middle/tail anchors.  NULL when out of range. */
DK_STATIC flexEntry *multimapSmallRowByPosition_(multimapSmall *m,
                                                 int64_t keyIndex) {
    const int64_t rows = flexCount(m->map) / m->elementsPerEntry;
    const int64_t row = keyIndex < 0 ? rows + keyIndex : keyIndex;
    if (row < 0 || row >= rows) {
        return nullptr;
    }

    return flexIndexWithMiddle(m->map, m->elementsPerEntry,
                               row * m->elementsPerEntry, GET_MIDDLE(m));
}

bool multimapSmallLookupByPosition(multimapSmall *m, int64_t keyIndex,
                                   databox *elements[]) {
    flexEntry *fe = multimapSmallRowByPosition_(m, keyIndex);
    if (!fe) {
        return false;
    }

    for (multimapElements i = 0; i < m->elementsPerEntry; i++) {
        flexGetByType(fe, elements[i]);
        fe = flexNext(m->map, fe);
    }

    return true;
}

bool multimapSmallDeleteByPosition(multimapSmall *m, int64_t keyIndex) {
    flexEntry *fe = multimapSmallRowByPosition_(m, keyIndex);
    if (!fe) {
        return false;
    }

    multimapEntry me = {.map = &m->map, .mapIdx = 0, .fe = fe};
    multimapSmallDeleteEntry(m, &me);
    return true;
}

int64_t multimapSmallFieldIncr(multimapSmall *m, const databox *key,
                               uint32_t fieldOffset, int64_t incrBy) {
    /* Only a value of a row: 0 is the key, and past the last value the
     * walk would leave the row */
    if (fieldOffset == 0 || fieldOffset >= m->elementsPerEntry) {
        return 0;
    }
    flexEntry *current = multimapSmallFindKey_(m, key, false, nullptr);
    if (!current) {
        /* No such key: return 0 rather than walking flexNext from NULL.
         * Matches the multimap-level FieldIncr miss contract. */
        return 0;
    }

    while (fieldOffset--) {
        current = flexNext(m->map, current);
    }

    int64_t newVal = 0;
    const size_t bytesBefore = flexBytes(m->map);
    if (flexIncrbySigned(&m->map, current, incrBy, &newVal)) {
        /* An entry re-encoded at a different width shifts every entry
         * after it (and a total-size varint crossing shifts them all), so
         * the middle is found again; a map of the same size kept every
         * entry where it was, as nearly every increment does. */
        if (flexBytes(m->map) != bytesBefore) {
            SET_MIDDLE_FORCE(m);
        }
        /* if incremented, return new value */
        return newVal;
    }

    /* (unlikely) else, return current value; a 96/128-bit value can't be
     * read as int64_t, so it reports 0 (only its type is decoded) */
    databox curVal;
    flexGetByTypeInto(current, &curVal, nullptr);
    return databoxIsWideDeferred(&curVal) ? 0 : curVal.data.i;
}

void multimapSmallReset(multimapSmall *m) {
    flexReset(&m->map);
    SET_MIDDLE_FORCE(m);
}

void multimapSmallFree(multimapSmall *m) {
    if (m) {
        flexFree(m->map);
        zfree(m);
    }
}

bool multimapSmallFirst(multimapSmall *m, databox *elements[]) {
    if (flexCount(m->map) == 0) {
        return false;
    }

    /* Populate forward */
    flexEntry *current = flexHead(m->map);
    for (multimapElements i = 0; i < m->elementsPerEntry; i++) {
        flexGetByType(current, elements[i]);
        current = flexNext(m->map, current);
    }

    return true;
}

bool multimapSmallLast(multimapSmall *m, databox *elements[]) {
    if (flexCount(m->map) == 0) {
        return false;
    }

    /* Populate reverse */
    flexEntry *current = flexTail(m->map);
    for (multimapElements i = 0; i < m->elementsPerEntry; i++) {
        flexGetByType(current, elements[(m->elementsPerEntry - 1) - i]);
        current = flexPrev(m->map, current);
    }

    return true;
}

DK_INLINE_ALWAYS bool multimapSmallIteratorInitAt_(multimapSmall *m,
                                                   multimapIterator *iter,
                                                   bool forward,
                                                   flexEntry *startAt) {
    flex *const map = m->map;
    iter->mm = map; /* the flex: 'm' may be a view on the caller's stack */
    iter->map = map;
    iter->mapIndex = 0; /* irrelevant for multimapSmall */
    iter->forward = forward;
    iter->entry = startAt;
    iter->elementsPerEntry = m->elementsPerEntry;
    iter->type = MULTIMAP_TYPE_SMALL;
    return true;
}

bool multimapSmallIteratorInitAt(multimapSmall *m, multimapIterator *iter,
                                 bool forward, const databox *box) {
    flexEntry *middle = GET_MIDDLE(m);
    flexEntry *startAt = flexFindByTypeSortedWithMiddleGetEntry(
        m->map, m->elementsPerEntry, box, middle);

    /* If we iterated past all elements, we can't iterate any more... */
    if (startAt == m->map + flexBytes(m->map)) {
        startAt = nullptr;
    }

    multimapSmallIteratorInitAt_(m, iter, forward, startAt);
    return !!startAt;
}

bool multimapSmallIteratorInit(multimapSmall *m, multimapIterator *iter,
                               bool forward) {
    flex *const map = m->map;
    flexEntry *startAt = likely(flexCount(map))
                             ? (likely(forward) ? flexHead(map) : flexTail(map))
                             : nullptr;
    return multimapSmallIteratorInitAt_(m, iter, forward, startAt);
}

DK_INLINE_ALWAYS bool multimapSmallIteratorNext_(multimapIterator *iter,
                                                 databox *elements[],
                                                 databoxWide *slots) {
    if (iter->entry) {
        flexEntry *current = iter->entry;
        assert(current < iter->map + flexBytes(iter->map));

        if (iter->forward) {
            /* Populate forward */
            for (multimapElements i = 0; i < iter->elementsPerEntry; i++) {
                multimapIteratorDecode_(current, elements[i], slots, i);
                current = flexNext(iter->map, current);
            }

            iter->entry = current;
        } else {
            /* Populate reverse */
            for (multimapElements i = 0; i < iter->elementsPerEntry; i++) {
                const multimapElements at = (iter->elementsPerEntry - 1) - i;
                multimapIteratorDecode_(current, elements[at], slots, at);
                current = flexPrev(iter->map, current);
            }

            iter->entry = current;
        }

        return true;
    }

    return false;
}

bool multimapSmallIteratorNext(multimapIterator *iter, databox *elements[]) {
    return multimapSmallIteratorNext_(iter, elements, nullptr);
}

bool multimapSmallIteratorNextWithSlots(multimapIterator *iter,
                                        databox *elements[],
                                        databoxWide *slots) {
    if (!slots) {
        return multimapSmallIteratorNext_(iter, elements, nullptr);
    }

    return multimapSmallIteratorNext_(iter, elements, slots);
}

bool multimapSmallDeleteByPredicate(multimapSmall *m,
                                    const multimapPredicate *p) {
    flexEntry *startP = flexGetByTypeSortedWithMiddle(
        m->map, m->elementsPerEntry, &p->compareAgainst, GET_MIDDLE(m));

    /* If the found value is the END OF THE MAP, don't continue! */
    if (!startP) {
        return false;
    }

    /* RESUME:
     *   - getbytype is returning the next HIGHEST match, but we don't
     *     want to delete that, we want to delete UP TO THAT, NOT inclusive.
     *     Unless it's an exact match, then we delete up to the EXACT MATCH,
     *     inclusive.
     */

    /* By default, assume the value we found is at the end of the list. */
    int compared = 1;
    if (flexEntryIsValid(m->map, startP)) {
        databox value;
        databoxWide valueWide;
        flexGetByTypeInto(startP, &value, &valueWide);

        compared = databoxCompare(&value, &p->compareAgainst);
    }

    switch (p->condition) {
    case MULTIMAP_CONDITION_LESS_THAN_EQUAL:
        if (compared == 0) {
            /* The element we found is an EXACT MATCH for LTE, so
             * delete everything in the map up to AND INCLUDING this
             * element. */
            flexDeleteUpToInclusivePlusN(&m->map, startP,
                                         m->elementsPerEntry - 1);

        } else {
            /* else, delete everything BELOW the current element because
             * our find will return the next highest above the search limit */
            flexDeleteUpToInclusive(&m->map, flexPrev(m->map, startP));
        }
        SET_MIDDLE_FORCE(m);
        return true;
    default:
        /* A tier trims a prefix (<=) only; multimapDeleteByPredicate
         * handles every other condition */
        assert(false && "Not Implemented!");
        return false;
    }

    return false;
}

size_t multimapSmallDeleteRange(multimapSmall *m, const databox *lo,
                                const databox *hi) {
    const size_t rows = flexDeleteSortedKeyRange(&m->map, m->elementsPerEntry,
                                                 lo, hi, GET_MIDDLE(m));
    if (rows) {
        SET_MIDDLE_FORCE(m);
    }
    return rows;
}

#ifdef DATAKIT_TEST
void multimapSmallRepr(const multimapSmall *m) {
    flexRepr(m->map);
}

void multimapSmallVerify(const multimapSmall *m) {
    /* middle oracle: the cached byte offset must equal a fresh rescan;
     * sorted finds trust it blindly, so any divergence is corruption. */
    assert(GET_MIDDLE(m) == flexMiddle(m->map, m->elementsPerEntry));
}
#endif
