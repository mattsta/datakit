#include "multiOrderedSetFull.h"
#include "multiOrderedSetFullInternal.h"
#include "multiOrderedSetMediumInternal.h"
/* atomPool.h already included via multiOrderedSetFullInternal.h */

#include "str.h" /* for random functions */

/* ====================================================================
 * Random Number Generator
 * ==================================================================== */
/* xorshift64star state for RandomMembers */
static uint64_t mosRandomState = 0xABCDEF0123456789ULL;

/* ====================================================================
 * Databox Arithmetic Helper
 * ==================================================================== */

/* ====================================================================
 * Atom Pool Helpers
 * ==================================================================== */

/* Convert member to pool ID (for storing in scoreMap) */
static inline databox memberToPoolId(multiOrderedSetFull *m,
                                     const databox *member) {
    if (m->pool) {
        uint64_t id = atomPoolIntern(m->pool, member);
        return databoxNewUnsigned(id);
    }
    /* No pool - return member as-is */
    return *member;
}

/* Read the member stored at 'memberEntry' into 'member'.  In pool mode
 * the entry is a pool ID and the member comes from the pool; otherwise
 * the entry is decoded straight into 'member'.  With a NULL 'slot' the
 * caller's box is filled in place (the public read contract); with a
 * slot, a 96/128-bit member lands in 'slot' (the set's own temporaries). */
static inline bool readMember(const multiOrderedSetFull *m,
                              const flexEntry *memberEntry, databox *member,
                              databoxWide *slot) {
    if (m->pool) {
        databox idBox;
        flexGetByTypeInto(memberEntry, &idBox, NULL);
        if (idBox.type == DATABOX_UNSIGNED_64) {
            if (slot) {
                mosBoxAim(member, slot);
            }
            return atomPoolLookup(m->pool, idBox.data.u64, member);
        }
    }

    if (slot) {
        flexGetByTypeInto(memberEntry, member, slot);
    } else {
        flexGetByType(memberEntry, member);
    }
    return true;
}

/* Release member from pool (when removing from set) */
static inline void releasePoolMember(multiOrderedSetFull *m,
                                     const databox *idBox) {
    if (m->pool && idBox->type == DATABOX_UNSIGNED_64) {
        atomPoolRelease(m->pool, idBox->data.u64);
    }
}

/* ====================================================================
 * Internal Helpers
 * ==================================================================== */

/* Get count for a single sub-map */
static size_t subMapCount(const flex *map) {
    return flexCount(map) / MOS_ELEMENTS_PER_ENTRY;
}

/* Get a sub-map by index */
static flex *getSubMap(const multiOrderedSetFull *m, uint32_t idx) {
    flex **ptr;
    multiarrayNativeGet(m->scoreMap, flex *, ptr, idx);
    return ptr ? *ptr : NULL;
}

/* Set a sub-map by index - we need to get the pointer to modify it */
static void setSubMap(multiOrderedSetFull *m, uint32_t idx, flex *newMap) {
    flex **ptr;
    multiarrayNativeGet(m->scoreMap, flex *, ptr, idx);
    if (ptr) {
        *ptr = newMap;
    }
}

/* Get middle offset for a sub-map */
static uint32_t getMiddle(const multiOrderedSetFull *m, uint32_t idx) {
    uint32_t *ptr;
    multiarrayNativeGet(m->middle, uint32_t, ptr, idx);
    return ptr ? *ptr : 0;
}

/* Set middle offset for a sub-map */
static void setMiddle(multiOrderedSetFull *m, uint32_t idx, uint32_t mid) {
    uint32_t *ptr;
    multiarrayNativeGet(m->middle, uint32_t, ptr, idx);
    if (ptr) {
        *ptr = mid;
    }
}

/* rangeBox holds each sub-map's head (minimum) score as a 16-byte box,
 * read in place.  A 96/128-bit head keeps its value in a 16-byte heap
 * payload the set owns (big = 1, allocated = 1).  The multiarray moves
 * boxes but never their payloads, so the pointer stays valid and a read
 * never writes.  rangeBoxRefresh reuses a payload in place, allocates
 * one only when a head turns wide and frees it when the head turns
 * narrow; every path that drops a box releases it with databoxFreeData.
 * Every other box has allocated = 0 and big = 0. */
static inline databox *getRangeBox(const multiOrderedSetFull *m, uint32_t idx) {
    databox *box;
    multiarrayNativeGet(m->rangeBox, databox, box, idx);
    return box;
}

/* Decode 'head' into rangeBox 'rb', or the INT64_MAX sentinel an empty
 * sub-map holds when 'head' is NULL.  A new box starts zeroed. */
static void rangeBoxRefresh(databox *rb, const flexEntry *head) {
    databoxWide *payload = (rb->allocated && mosBoxIsWide(rb))
                               ? (databoxWide *)rb->data.ptr
                               : NULL;
    if (head) {
        flexGetByTypeInto(head, rb, payload);
    } else {
        rb->type = DATABOX_SIGNED_64;
        rb->data.i = INT64_MAX;
    }

    if (likely(!mosBoxIsWide(rb))) {
        if (unlikely(payload)) {
            zfree(payload);
            rb->allocated = false;
            rb->big = false;
        }

        return;
    }

    if (!payload) {
        /* @own: container; free=databoxFreeData */
        payload = zmalloc(sizeof(*payload));
        flexGetByTypeInto(head, rb, payload);
    }

    rb->allocated = true; /* the decode borrows; the set owns the payload */
}

/* Release every rangeBox payload before the boxes are dropped */
static void rangeBoxReleaseAll(multiOrderedSetFull *m) {
    for (uint32_t i = 0; i < m->mapCount; i++) {
        databoxFreeData(getRangeBox(m, i));
    }
}

/* Update middle from flex */
static void updateMiddle(multiOrderedSetFull *m, uint32_t idx) {
    flex *map = getSubMap(m, idx);
    if (map) {
        flexEntry *mid = flexMiddle(map, MOS_ELEMENTS_PER_ENTRY);
        setMiddle(m, idx, mid ? (uint32_t)(mid - map) : FLEX_EMPTY_SIZE);
    }
}

/* Find which sub-map a score belongs to: the LAST sub-map whose min score
 * (rangeBox[i] = head/min of sub-map i) is <= score — i.e. the sub-map that
 * should contain `score`. If score is below every sub-map's min, returns 0 (it
 * belongs at the front and becomes the new min there).
 *
 * Must be "last min <= score", NOT "first min >= score": the latter overshoots
 * interior scores to the next sub-map, mis-routing inserts (breaking global
 * order) and member lookups (findEntryForScore searches only the returned
 * sub-map). Masked while mapCount==1; sub-map splitting makes it mandatory.
 * Relies on rangeBox being monotonic, which empty-sub-map reclaim guarantees
 * (no interior INT64_MAX sentinels). */
static int32_t findSubMapForScore(const multiOrderedSetFull *m,
                                  const databox *score) {
    int32_t left = 0;
    int32_t right = (int32_t)m->mapCount - 1;

    while (left < right) {
        const int32_t mid = (left + right + 1) / 2; /* upper mid: bias right */
        const databox *rangeScore = getRangeBox(m, (uint32_t)mid);

        if (rangeScore && databoxCompare(rangeScore, score) <= 0) {
            left =
                mid; /* sub-map mid's min <= score: candidate, search right */
        } else {
            right = mid - 1;
        }
    }

    return left;
}

/* Split an oversized sub-map at a score boundary so member lookups (which route
 * by a single sub-map via findSubMapForScore) never have an equal-score run
 * straddling two sub-maps. Lower [0,k) stays; upper [k,end) becomes a new
 * sub-map at mapIdx+1, where k is a pair index with score[k] != score[k-1]. If
 * the whole sub-map is one score value (no interior boundary), it is left
 * oversized — it cannot be split safely. */
static void splitSubMap(multiOrderedSetFull *m, uint32_t mapIdx) {
    flex *map = getSubMap(m, mapIdx);
    if (!map) {
        return;
    }

    const size_t pairs = subMapCount(map);
    if (pairs < 2) {
        return;
    }

    /* Pairs are in score order, so equal first and last scores mean one
     * equal-score run with no boundary to cut at.  Checked in O(1): an
     * oversized run is offered a split on every insert into it. */
    databox s[2];
    databoxWide sw[2];
    flexGetByTypeInto(flexHead(map), &s[0], &sw[0]);
    flexGetByTypeInto(flexIndex(map, -(int32_t)MOS_ELEMENTS_PER_ENTRY), &s[1],
                      &sw[1]);
    if (databoxCompare(&s[0], &s[1]) == 0) {
        return; /* one giant equal-score run: cannot split safely */
    }

    /* Snap the cut to the score boundary nearest the geometric middle: k is
     * a PAIR index in [1, pairs-1] with score[k] != score[k-1].  One walk
     * (each box keeps its own wide storage, so the previous score stays
     * valid while the next is read). */
    const size_t mid = pairs / 2;
    size_t k = 0;
    size_t bestDistance = SIZE_MAX;
    flexEntry *fe = flexHead(map);
    int cur = 0;
    flexGetByTypeInto(fe, &s[cur], &sw[cur]);
    for (size_t i = 1; i < pairs; i++) {
        fe = flexNext(map, flexNext(map, fe)); /* past the member */
        cur ^= 1;
        flexGetByTypeInto(fe, &s[cur], &sw[cur]);
        if (databoxCompare(&s[cur], &s[cur ^ 1]) != 0) {
            const size_t distance = i > mid ? i - mid : mid - i;
            if (distance < bestDistance) {
                bestDistance = distance;
                k = i;
            }
            if (i >= mid) {
                break; /* later boundaries are only further away */
            }
        }
    }

    /* Lower [0,k) stays in 'map' (reallocated); upper [k,pairs) returns new.
     * flexSplitRange is ELEMENT-indexed. */
    const uint32_t splitElem = (uint32_t)(k * MOS_ELEMENTS_PER_ENTRY);
    const uint32_t moveElems = (uint32_t)((pairs - k) * MOS_ELEMENTS_PER_ENTRY);
    flex *upper = flexSplitRange(&map, (int32_t)splitElem, moveElems);

    /* 'map' was reallocated by the split; store the lower half back. */
    setSubMap(m, mapIdx, map);

    /* Compute the upper half's middle + range BEFORE the inserts (the inserts
     * can relocate the multiarrays, but 'upper' itself is not moved). */
    flexEntry *upMid = flexMiddle(upper, MOS_ELEMENTS_PER_ENTRY);
    uint32_t upperMiddle = upMid ? (uint32_t)(upMid - upper) : FLEX_EMPTY_SIZE;
    databox upperRange = {{0}};
    rangeBoxRefresh(&upperRange, flexHead(upper));

    /* Insert the upper half at mapIdx+1 across all 3 parallel arrays. Each
     * insert needs that array's current element count (== m->mapCount); use a
     * separate scratch per call (the macro does localCount++ and uses it for
     * the native->medium promotion trigger). Bump m->mapCount once after. */
    const uint32_t insertAt = mapIdx + 1;
    uint32_t cScore = m->mapCount;
    uint32_t cMid = m->mapCount;
    uint32_t cRange = m->mapCount;
    multiarrayNativeInsert(m->scoreMap, flex *, 64, cScore, insertAt, &upper);
    multiarrayNativeInsert(m->middle, uint32_t, 64, cMid, insertAt,
                           &upperMiddle);
    multiarrayNativeInsert(m->rangeBox, databox, 64, cRange, insertAt,
                           &upperRange);
    m->mapCount++;

    /* Lower half shrank: recompute its middle. rangeBox[mapIdx] is unchanged
     * (its head/min score is the same after removing the tail). totalEntries
     * is unchanged (split moves pairs, never adds/removes). */
    updateMiddle(m, mapIdx);
}

/* Insert into specific sub-map */
static void insertIntoSubMap(multiOrderedSetFull *m, uint32_t mapIdx,
                             const databox *score, const databox *member) {
    flex *map = getSubMap(m, mapIdx);
    if (!map) {
        return;
    }

    /* In pool mode, convert member to pool ID for storage in scoreMap */
    databox memberOrId = memberToPoolId(m, member);
    const databox *elements[2] = {score, &memberOrId};
    uint32_t mid = getMiddle(m, mapIdx);
    flexEntry *middle = (flexEntry *)(map + mid);

    flexInsertByTypeSortedWithMiddleMultiDirect(&map, MOS_ELEMENTS_PER_ENTRY,
                                                elements, &middle);

    setSubMap(m, mapIdx, map);
    setMiddle(m, mapIdx, (uint32_t)(middle - map));
    m->totalEntries++;

    /* Update rangeBox if needed */
    flexEntry *head = flexHead(map);
    if (head) {
        databox *rangeScore = getRangeBox(m, mapIdx);
        if (rangeScore) {
            rangeBoxRefresh(rangeScore, head);
        }
    }

    /* Split oversized sub-maps so the structure scales. Requires >=2 pairs;
     * splitSubMap is a no-op when no interior score boundary exists (a single
     * giant equal-score run stays oversized so member routing stays correct).
     */
    if (flexBytes(map) > m->maxMapSize && m->mapCount < 1024 &&
        subMapCount(map) >= 2) {
        splitSubMap(m, mapIdx);
    }
}

/* Remove entry from specific sub-map */
static void removeFromSubMap(multiOrderedSetFull *m, uint32_t mapIdx,
                             flexEntry *entry) {
    flex *map = getSubMap(m, mapIdx);
    if (!map) {
        return;
    }

    /* In pool mode, release the member before deleting */
    if (m->pool) {
        flexEntry *memberEntry = flexNext(map, entry);
        if (memberEntry) {
            databox memberOrId;
            databoxWide memberOrIdWide;
            flexGetByTypeInto(memberEntry, &memberOrId, &memberOrIdWide);
            releasePoolMember(m, &memberOrId);
        }
    }

    flexDeleteCount(&map, &entry, MOS_ELEMENTS_PER_ENTRY);
    setSubMap(m, mapIdx, map);
    updateMiddle(m, mapIdx);
    m->totalEntries--;

    /* Update rangeBox - check if map has entries before reading head.
     * flexHead() returns non-NULL even for empty flex (points past header),
     * so we must check flexCount() to avoid reading invalid memory.  An
     * empty map holds the INT64_MAX sentinel. */
    databox *rangeScore = getRangeBox(m, mapIdx);
    if (rangeScore) {
        rangeBoxRefresh(rangeScore, flexCount(map) > 0 ? flexHead(map) : NULL);
    }
}

/* Reclaim an emptied sub-map slot so rangeBox stays monotonic for
 * findSubMapForScore's binary search. Score updates can empty a *middle*
 * sub-map; an empty middle keeps the INT64_MAX rangeBox sentinel, breaking the
 * non-decreasing order the search relies on -> mis-routing -> duplicates.
 * Never reclaims the last sub-map (mapCount must stay >= 1; a lone empty map
 * with rangeBox==INT64_MAX is harmless, search returns 0 trivially).
 * WARNING: shifts all sub-map indices > mapIdx down by one and frees the
 * slot's flex — callers must NOT use a held mapIdx/flex* afterward. */
static void reclaimEmptySubMap(multiOrderedSetFull *m, uint32_t mapIdx) {
    if (m->mapCount <= 1) {
        return;
    }
    flex *map = getSubMap(m, mapIdx);
    if (!map || subMapCount(map) != 0) {
        return; /* only reclaim genuinely empty sub-maps */
    }

    /* multiarray stores the flex pointer by value and won't free it on delete
     * (mirrors Free/Reset), so free it here first. */
    flexFree(map);

    /* Separate scratch per array (the macro mutates its count lvalue). */
    uint32_t cScore = m->mapCount;
    multiarrayNativeDelete(m->scoreMap, flex *, cScore, mapIdx);
    uint32_t cMiddle = m->mapCount;
    multiarrayNativeDelete(m->middle, uint32_t, cMiddle, mapIdx);
    databoxFreeData(getRangeBox(m, mapIdx));
    uint32_t cRange = m->mapCount;
    multiarrayNativeDelete(m->rangeBox, databox, cRange, mapIdx);

    m->mapCount--;
}

/* Sweep all empty sub-maps (backward so each delete only shifts already-visited
 * higher indices). For bulk remove/pop loops that hold a live mapIdx and so
 * cannot reclaim inline — call this once after the loop. */
static void reclaimAllEmptySubMaps(multiOrderedSetFull *m) {
    for (int32_t i = (int32_t)m->mapCount - 1; i >= 0 && m->mapCount > 1; i--) {
        reclaimEmptySubMap(m, (uint32_t)i);
    }
}

/* Look up 'member's score in the member index; a 96/128-bit score lands
 * in 'slot' */
static bool findScore(const multiOrderedSetFull *m, const databox *member,
                      databox *score, databoxWide *slot) {
    mosBoxAim(score, slot);
    return multidictFind((multidict *)m->memberIndex, member, score);
}

/* The score-map row of 'member' whose score is 'score' (the member
 * index's value for it): the sub-map that holds the score, then a binary
 * search for the (score, member or pool id) row, as the sub-maps are kept
 * sorted by that pair.  NULL when there is no such row. */
static flexEntry *findEntryForScore(const multiOrderedSetFull *m,
                                    const databox *member, const databox *score,
                                    uint32_t *mapIdx) {
    *mapIdx = findSubMapForScore(m, score);
    flex *map = getSubMap(m, *mapIdx);
    if (!map || flexCount(map) == 0) {
        return NULL;
    }

    /* In pool mode the row holds the member's pool ID */
    databox memberToMatch;
    if (m->pool) {
        const uint64_t id = atomPoolGetId(m->pool, member);
        if (id == 0) {
            return NULL; /* Member not in pool */
        }
        memberToMatch = databoxNewUnsigned(id);
    } else {
        memberToMatch = *member;
    }

    const databox *row[MOS_ELEMENTS_PER_ENTRY] = {score, &memberToMatch};
    const flexEntry *middle = map + getMiddle(m, *mapIdx);
    return flexFindByTypeSortedWithMiddleFullWidth(map, MOS_ELEMENTS_PER_ENTRY,
                                                   row, middle);
}

/* Find entry in score maps by member (using member index for score lookup) */
static flexEntry *findEntryByMember(const multiOrderedSetFull *m,
                                    const databox *member, uint32_t *mapIdx) {
    databox score;
    databoxWide scoreWide;
    if (!findScore(m, member, &score, &scoreWide)) {
        return NULL;
    }

    return findEntryForScore(m, member, &score, mapIdx);
}

/* ====================================================================
 * Creation / Destruction
 * ==================================================================== */

multiOrderedSetFull *multiOrderedSetFullNew(void) {
    /* @own: caller; free=multiOrderedSetFullFree */
    multiOrderedSetFull *m = zcalloc(1, sizeof(*m));

    /* Create member index (member → score) */
    /* @own: container; free=multiOrderedSetFullFree */
    m->mdClass = multidictDefaultClassNew();
    /* @own: container; free=multiOrderedSetFullFree */
    m->memberIndex = multidictNew(&multidictTypeExactKey, m->mdClass, 0);

    /* Create initial sub-map */
    m->scoreMap = multiarrayNativeNew(flex *);
    m->middle = multiarrayNativeNew(uint32_t);
    m->rangeBox = multiarrayNativeNew(databox);

    /* @own: container; free=multiOrderedSetFullFree */
    flex *initialMap = flexNew();
    uint32_t initialMiddle = FLEX_EMPTY_SIZE;
    databox initialRange = {{0}};
    rangeBoxRefresh(&initialRange, NULL);

    /* Use multiarrayNativeInsert instead of non-existent Append */
    uint32_t mapCount = 0;
    multiarrayNativeInsert(m->scoreMap, flex *, 64, mapCount, 0, &initialMap);
    multiarrayNativeInsert(m->middle, uint32_t, 64, mapCount, 0,
                           &initialMiddle);
    multiarrayNativeInsert(m->rangeBox, databox, 64, mapCount, 0,
                           &initialRange);

    m->mapCount = 1;
    m->totalEntries = 0;
    m->maxMapSize = MOS_FULL_DEFAULT_MAX_MAP_SIZE;
    m->flags = 0;
    m->pool = NULL;

    return m;
}

multiOrderedSetFull *multiOrderedSetFullNewWithPool(atomPool *pool) {
    multiOrderedSetFull *m = multiOrderedSetFullNew();
    m->pool = pool;
    return m;
}

multiOrderedSetFull *multiOrderedSetFullNewWithOwnedPool(void) {
    multiOrderedSetFull *m = multiOrderedSetFullNew();
    /* @own: container; free=multiOrderedSetFullFree */
    m->pool = atomPoolNewDefault(); /* Uses HASH backend for speed */
    m->flags |= MOS_FLAG_POOL_OWNED;
    return m;
}

multiOrderedSetFull *multiOrderedSetFullNewWithPoolType(atomPoolType type) {
    multiOrderedSetFull *m = multiOrderedSetFullNew();
    /* @own: container; free=multiOrderedSetFullFree */
    m->pool = atomPoolNew(type);
    m->flags |= MOS_FLAG_POOL_OWNED;
    return m;
}

multiOrderedSetFull *
multiOrderedSetFullNewFromMedium(struct multiOrderedSetMedium *medium,
                                 flex *maps[2], uint32_t middles[2]) {
    multiOrderedSetFull *m = multiOrderedSetFullNew();

    /* Reset to clear the initial empty map and prepare for new data */
    multiOrderedSetFullReset(m);

    (void)middles; /* Currently unused, reserved for future optimization */

    /* Iterate through both medium maps and add entries */
    for (int mapIdx = 0; mapIdx < 2; mapIdx++) {
        flexEntry *entry = flexHead(maps[mapIdx]);
        while (entry) {
            flexEntry *memberEntry = flexNext(maps[mapIdx], entry);
            if (!memberEntry) {
                break;
            }

            databox score = {{0}};
            databox member = {{0}};
            databoxWide scoreWide, memberWide;
            flexGetByTypeInto(entry, &score, &scoreWide);
            flexGetByTypeInto(memberEntry, &member, &memberWide);

            multiOrderedSetFullAdd(m, &score, &member);

            entry = flexNext(maps[mapIdx], memberEntry);
        }
        flexFree(maps[mapIdx]);
    }

    /* Free the medium struct */
    zfree(medium);

    return m;
}

multiOrderedSetFull *multiOrderedSetFullCopy(const multiOrderedSetFull *m) {
    /* @own: caller; free=multiOrderedSetFullFree */
    multiOrderedSetFull *copy = zcalloc(1, sizeof(*copy));

    /* Create own class for the copy (classes shouldn't be shared for ownership)
     */
    /* @own: container; free=multiOrderedSetFullFree */
    copy->mdClass = multidictDefaultClassNew();
    /* @own: container; free=multiOrderedSetFullFree */
    copy->memberIndex = multidictNew(&multidictTypeExactKey, copy->mdClass, 0);

    /* Copy all entries from original memberIndex */
    multidictIterator iter;
    multidictIteratorInit((multidict *)m->memberIndex, &iter);
    multidictEntry entry;
    while (multidictIteratorNext(&iter, &entry)) {
        multidictAdd(copy->memberIndex, &entry.key, &entry.val);
    }
    multidictIteratorRelease(&iter);

    copy->scoreMap = multiarrayNativeNew(flex *);
    copy->middle = multiarrayNativeNew(uint32_t);
    copy->rangeBox = multiarrayNativeNew(databox);

    /* One count per array: each insert advances its own count, and the
     * native-to-medium promotion keys off it */
    uint32_t cScore = 0;
    uint32_t cMid = 0;
    uint32_t cRange = 0;
    for (uint32_t i = 0; i < m->mapCount; i++) {
        flex *origMap = getSubMap(m, i);
        /* @own: container; free=multiOrderedSetFullFree */
        flex *mapCopy = origMap ? flexDuplicate(origMap) : flexNew();
        uint32_t mid = getMiddle(m, i);

        /* Rebuilt from the copy's own head, so the copy never shares a
         * payload (or borrowed bytes) with the original */
        databox rangeCopy = {{0}};
        rangeBoxRefresh(&rangeCopy,
                        flexCount(mapCopy) > 0 ? flexHead(mapCopy) : NULL);

        multiarrayNativeInsert(copy->scoreMap, flex *, 64, cScore, i, &mapCopy);
        multiarrayNativeInsert(copy->middle, uint32_t, 64, cMid, i, &mid);
        multiarrayNativeInsert(copy->rangeBox, databox, 64, cRange, i,
                               &rangeCopy);
    }

    copy->mapCount = m->mapCount;
    copy->totalEntries = m->totalEntries;
    copy->maxMapSize = m->maxMapSize;

    /* Handle pool mode for copy */
    if (m->pool) {
        if (m->flags & MOS_FLAG_POOL_OWNED) {
            /* Source owns pool - copy creates its own pool and re-interns */
            /* @own: container; free=multiOrderedSetFullFree */
            copy->pool = atomPoolNewDefault();
            copy->flags = MOS_FLAG_POOL_OWNED;

            /* Re-intern all members from scoreMaps into new pool */
            for (uint32_t i = 0; i < copy->mapCount; i++) {
                flex *map = getSubMap(copy, i);
                if (!map) {
                    continue;
                }
                flexEntry *ent = flexHead(map);
                while (ent) {
                    flexEntry *memberEntry = flexNext(map, ent);
                    if (!memberEntry) {
                        break;
                    }
                    /* Get old pool ID, look up member in source pool */
                    databox oldId;
                    databoxWide oldIdWide;
                    flexGetByTypeInto(memberEntry, &oldId, &oldIdWide);
                    databox member = {{0}};
                    if (atomPoolLookup(m->pool, oldId.data.u64, &member)) {
                        /* Intern in new pool and update entry.  A new id
                         * of another width reallocates the map: the entry
                         * keeps its offset, so find it again from there */
                        const size_t at = (size_t)(memberEntry - map);
                        uint64_t newId = atomPoolIntern(copy->pool, &member);
                        databox newIdBox = databoxNewUnsigned(newId);
                        flexReplaceByType(&map, memberEntry, &newIdBox);
                        setSubMap(copy, i, map);
                        memberEntry = map + at;
                    }
                    ent = flexNext(map, memberEntry);
                }

                /* Ids of other widths moved the entries: the middle copied
                 * from the source no longer points at an entry */
                updateMiddle(copy, i);
            }
        } else {
            /* Source uses external pool - copy shares it and bumps refcounts */
            copy->pool = m->pool;
            copy->flags = 0; /* Not owned */

            /* Increment refcounts for all pool IDs */
            for (uint32_t i = 0; i < copy->mapCount; i++) {
                flex *map = getSubMap(copy, i);
                if (!map) {
                    continue;
                }
                flexEntry *ent = flexHead(map);
                while (ent) {
                    flexEntry *memberEntry = flexNext(map, ent);
                    if (!memberEntry) {
                        break;
                    }
                    databox idBox;
                    databoxWide idBoxWide;
                    flexGetByTypeInto(memberEntry, &idBox, &idBoxWide);
                    if (idBox.type == DATABOX_UNSIGNED_64) {
                        /* Re-intern to increment refcount */
                        databox member = {{0}};
                        if (atomPoolLookup(copy->pool, idBox.data.u64,
                                           &member)) {
                            atomPoolIntern(copy->pool, &member);
                        }
                    }
                    ent = flexNext(map, memberEntry);
                }
            }
        }
    } else {
        copy->pool = NULL;
        copy->flags = m->flags;
    }

    return copy;
}

void multiOrderedSetFullFree(multiOrderedSetFull *m) {
    if (!m) {
        return;
    }

    /* If using pool, release all member IDs from scoreMaps */
    if (m->pool) {
        for (uint32_t i = 0; i < m->mapCount; i++) {
            flex *map = getSubMap(m, i);
            if (map) {
                flexEntry *entry = flexHead(map);
                while (entry) {
                    flexEntry *memberEntry = flexNext(map, entry);
                    if (!memberEntry) {
                        break;
                    }
                    databox memberOrId;
                    databoxWide memberOrIdWide;
                    flexGetByTypeInto(memberEntry, &memberOrId,
                                      &memberOrIdWide);
                    releasePoolMember(m, &memberOrId);
                    entry = flexNext(map, memberEntry);
                }
            }
        }
        /* Free pool if we own it */
        if (m->flags & MOS_FLAG_POOL_OWNED) {
            atomPoolFree(m->pool);
        }
    }

    multidictFree(m->memberIndex);
    if (m->mdClass) {
        multidictDefaultClassFree(m->mdClass);
    }

    for (uint32_t i = 0; i < m->mapCount; i++) {
        flex *map = getSubMap(m, i);
        if (map) {
            flexFree(map);
        }
    }

    rangeBoxReleaseAll(m);
    multiarrayNativeFree(m->scoreMap);
    multiarrayNativeFree(m->middle);
    multiarrayNativeFree(m->rangeBox);

    zfree(m);
}

void multiOrderedSetFullReset(multiOrderedSetFull *m) {
    /* If using pool, release all member IDs from scoreMaps */
    if (m->pool) {
        for (uint32_t i = 0; i < m->mapCount; i++) {
            flex *map = getSubMap(m, i);
            if (map) {
                flexEntry *entry = flexHead(map);
                while (entry) {
                    flexEntry *memberEntry = flexNext(map, entry);
                    if (!memberEntry) {
                        break;
                    }
                    databox memberOrId;
                    databoxWide memberOrIdWide;
                    flexGetByTypeInto(memberEntry, &memberOrId,
                                      &memberOrIdWide);
                    releasePoolMember(m, &memberOrId);
                    entry = flexNext(map, memberEntry);
                }
            }
        }
        /* If we own the pool, reset it too */
        if (m->flags & MOS_FLAG_POOL_OWNED) {
            atomPoolReset(m->pool);
        }
    }

    /* Clear member index */
    multidictEmpty(m->memberIndex);

    /* Clear all sub-maps */
    for (uint32_t i = 0; i < m->mapCount; i++) {
        flex *map = getSubMap(m, i);
        if (map) {
            flexFree(map);
        }
    }

    /* Reset to single empty map */
    rangeBoxReleaseAll(m);
    multiarrayNativeFree(m->scoreMap);
    multiarrayNativeFree(m->middle);
    multiarrayNativeFree(m->rangeBox);

    m->scoreMap = multiarrayNativeNew(flex *);
    m->middle = multiarrayNativeNew(uint32_t);
    m->rangeBox = multiarrayNativeNew(databox);

    /* @own: container; free=multiOrderedSetFullFree */
    flex *initialMap = flexNew();
    uint32_t initialMiddle = FLEX_EMPTY_SIZE;
    databox initialRange = {{0}};
    rangeBoxRefresh(&initialRange, NULL);

    uint32_t resetCount = 0;
    multiarrayNativeInsert(m->scoreMap, flex *, 64, resetCount, 0, &initialMap);
    multiarrayNativeInsert(m->middle, uint32_t, 64, resetCount, 0,
                           &initialMiddle);
    multiarrayNativeInsert(m->rangeBox, databox, 64, resetCount, 0,
                           &initialRange);

    m->mapCount = 1;
    m->totalEntries = 0;
}

/* ====================================================================
 * Statistics
 * ==================================================================== */

size_t multiOrderedSetFullCount(const multiOrderedSetFull *m) {
    return m->totalEntries;
}

size_t multiOrderedSetFullBytes(const multiOrderedSetFull *m) {
    size_t bytes = sizeof(*m);
    bytes += multidictBytes((multidict *)m->memberIndex);

    for (uint32_t i = 0; i < m->mapCount; i++) {
        flex *map = getSubMap(m, i);
        if (map) {
            bytes += flexBytes(map);
        }
    }

    /* Include owned pool bytes in total */
    if (m->pool && (m->flags & MOS_FLAG_POOL_OWNED)) {
        bytes += atomPoolBytes(m->pool);
    }

    return bytes;
}

/* ====================================================================
 * Insertion / Update
 * ==================================================================== */

/* Take the score-map row of 'member', whose stored score is 'old' */
static void removeMemberRow(multiOrderedSetFull *m, const databox *member,
                            const databox *old) {
    uint32_t mapIdx;
    flexEntry *entry = findEntryForScore(m, member, old, &mapIdx);
    if (entry) {
        removeFromSubMap(m, mapIdx, entry);
        reclaimEmptySubMap(m, mapIdx);
    }
}

/* Give 'member' the score 'score' in the member index and the score maps:
 * an existing member ('existed', its row already taken out) has its index
 * value rewritten in place, in one search */
static bool setMemberScore(multiOrderedSetFull *m, const databox *member,
                           const databox *score, bool existed) {
    const multidictResult res =
        existed ? multidictReplace(m->memberIndex, member, score)
                : multidictAdd(m->memberIndex, member, score);
    if (res == MULTIDICT_ERR) {
        return false;
    }

    insertIntoSubMap(m, findSubMapForScore(m, score), score, member);
    return true;
}

bool multiOrderedSetFullAdd(multiOrderedSetFull *m, const databox *score,
                            const databox *member) {
    /* One member-index read gives both whether the member exists and the
     * score that locates its row */
    databox old;
    databoxWide oldWide;
    const bool existed = findScore(m, member, &old, &oldWide);
    if (existed) {
        removeMemberRow(m, member, &old);
    }

    setMemberScore(m, member, score, existed);
    return existed;
}

bool multiOrderedSetFullAddNX(multiOrderedSetFull *m, const databox *score,
                              const databox *member) {
    /* One member-index search refuses a present member or indexes it */
    if (multidictAddNX(m->memberIndex, member, score) !=
        MULTIDICT_OK_INSERTED) {
        return false;
    }

    uint32_t mapIdx = findSubMapForScore(m, score);
    insertIntoSubMap(m, mapIdx, score, member);

    return true;
}

bool multiOrderedSetFullAddXX(multiOrderedSetFull *m, const databox *score,
                              const databox *member) {
    databox old;
    databoxWide oldWide;
    if (!findScore(m, member, &old, &oldWide)) {
        return false;
    }

    removeMemberRow(m, member, &old);
    setMemberScore(m, member, score, true);
    return true;
}

bool multiOrderedSetFullAddGetPrevious(multiOrderedSetFull *m,
                                       const databox *score,
                                       const databox *member,
                                       databox *prevScore) {
    databox existingScore;
    databoxWide existingScoreWide;
    const bool existed =
        findScore(m, member, &existingScore, &existingScoreWide);

    if (existed) {
        /* Self-contained: the stored score is replaced below */
        *prevScore = databoxCopy(&existingScore);
        removeMemberRow(m, member, &existingScore);
    }

    setMemberScore(m, member, score, existed);
    return existed;
}

bool multiOrderedSetFullIncrBy(multiOrderedSetFull *m, const databox *delta,
                               const databox *member, databox *result) {
    databox existingScore;
    databoxWide existingScoreWide;
    const bool existed =
        findScore(m, member, &existingScore, &existingScoreWide);

    if (existed) {
        if (!mosScoreAdd(&existingScore, delta, result)) {
            return false;
        }

        removeMemberRow(m, member, &existingScore);
    } else {
        *result = *delta;
    }

    setMemberScore(m, member, result, existed);
    return true;
}

/* ====================================================================
 * Deletion
 * ==================================================================== */

/* Remove 'member' with one member-index search, which hands back the
 * score (owned) that locates its row */
static bool removeMember(multiOrderedSetFull *m, const databox *member,
                         databox *score) {
    if (!multidictGetAndDelete(m->memberIndex, member, score)) {
        return false;
    }

    removeMemberRow(m, member, score);
    return true;
}

bool multiOrderedSetFullRemove(multiOrderedSetFull *m, const databox *member) {
    databox score;
    if (!removeMember(m, member, &score)) {
        return false;
    }

    databoxFreeData(&score);
    return true;
}

bool multiOrderedSetFullRemoveGetScore(multiOrderedSetFull *m,
                                       const databox *member, databox *score) {
    /* The removal's owned copy is the caller's */
    return removeMember(m, member, score);
}

size_t multiOrderedSetFullRemoveRangeByScore(multiOrderedSetFull *m,
                                             const mosRangeSpec *range) {
    size_t removed = 0;

    for (uint32_t mapIdx = 0; mapIdx < m->mapCount; mapIdx++) {
        flex *map = getSubMap(m, mapIdx);
        if (!map) {
            continue;
        }

        flexEntry *entry = flexHead(map);
        while (entry) {
            flexEntry *memberEntry = flexNext(map, entry);
            if (!memberEntry) {
                break;
            }

            databox score;
            databoxWide scoreWide;
            flexGetByTypeInto(entry, &score, &scoreWide);

            if (mosScoreInRange(&score, &range->min, range->minExclusive,
                                &range->max, range->maxExclusive)) {
                flexEntry *nextEntry = flexNext(map, memberEntry);

                /* In pool mode, convert pool ID to member for memberIndex */
                databox member = {{0}};
                databoxWide memberWide;
                if (!readMember(m, memberEntry, &member, &memberWide)) {
                    entry = nextEntry;
                    continue;
                }

                /* Remove from member index */
                multidictDelete(m->memberIndex, &member);

                /* Remove from score map.  Removing shifts the following
                 * pairs down, may move the flex and may shrink its
                 * header: the next pair then starts where this one did,
                 * measured from the head */
                const size_t offset = (size_t)(entry - flexHead(map));
                removeFromSubMap(m, mapIdx, entry);
                removed++;

                map = getSubMap(m, mapIdx);
                entry = nextEntry ? flexHead(map) + offset : NULL;
            } else {
                int cmp = databoxCompare(&score, &range->max);
                if (cmp > 0 || (cmp == 0 && range->maxExclusive)) {
                    break;
                }
                entry = flexNext(map, memberEntry);
            }
        }
    }

    /* Bulk loop held live mapIdx/map, so reclaim emptied sub-maps now (backward
     * sweep) rather than inline, to restore monotonic rangeBox. */
    reclaimAllEmptySubMaps(m);
    return removed;
}

size_t multiOrderedSetFullRemoveRangeByRank(multiOrderedSetFull *m,
                                            int64_t start, int64_t stop) {
    size_t count = m->totalEntries;
    if (!mosClampRankRange(&start, &stop, count)) {
        return 0;
    }

    /* Collect entries to remove */
    size_t removed = 0;
    int64_t currentRank = 0;

    for (uint32_t mapIdx = 0; mapIdx < m->mapCount && currentRank <= stop;
         mapIdx++) {
        flex *map = getSubMap(m, mapIdx);
        if (!map) {
            continue;
        }

        flexEntry *entry = flexHead(map);
        while (entry && currentRank <= stop) {
            flexEntry *memberEntry = flexNext(map, entry);
            if (!memberEntry) {
                break;
            }

            if (currentRank >= start) {
                /* In pool mode, convert pool ID to member for memberIndex */
                databox member = {{0}};
                databoxWide memberWide;
                if (!readMember(m, memberEntry, &member, &memberWide)) {
                    currentRank++;
                    entry = flexNext(map, memberEntry);
                    continue;
                }

                flexEntry *nextEntry = flexNext(map, memberEntry);

                /* Removing shifts the following pairs down, may move the
                 * flex and may shrink its header: the next pair then
                 * starts where this one did, measured from the head */
                const size_t offset = (size_t)(entry - flexHead(map));
                multidictDelete(m->memberIndex, &member);
                removeFromSubMap(m, mapIdx, entry);
                removed++;

                map = getSubMap(m, mapIdx);
                entry = nextEntry ? flexHead(map) + offset : NULL;
            } else {
                entry = flexNext(map, memberEntry);
            }

            currentRank++;
        }
    }

    /* Bulk loop held live mapIdx/map, so reclaim emptied sub-maps now (backward
     * sweep) rather than inline, to restore monotonic rangeBox. */
    reclaimAllEmptySubMaps(m);
    return removed;
}

size_t multiOrderedSetFullPopMin(multiOrderedSetFull *m, size_t count,
                                 databox *members, databox *scores) {
    size_t popped = 0;

    for (uint32_t mapIdx = 0; mapIdx < m->mapCount && popped < count;
         mapIdx++) {
        flex *map = getSubMap(m, mapIdx);
        if (!map) {
            continue;
        }

        while (popped < count) {
            flexEntry *head = flexHead(map);
            if (!head) {
                break;
            }

            flexEntry *memberEntry = flexNext(map, head);
            if (!memberEntry) {
                break;
            }

            /* In pool mode, convert pool ID back to member string */
            databox member = {{0}};
            databoxWide memberWide;
            if (!readMember(m, memberEntry, &member, &memberWide)) {
                break;
            }

            /* Self-contained results: copy before the entry goes away */
            mosReadOwned(head, &scores[popped]);
            members[popped] = databoxCopy(&member);

            multidictDelete(m->memberIndex, &member);
            removeFromSubMap(m, mapIdx, head);

            /* Refresh map pointer after removal */
            map = getSubMap(m, mapIdx);
            popped++;
        }
    }

    reclaimAllEmptySubMaps(m); /* backward sweep: bulk loop held live mapIdx */
    return popped;
}

size_t multiOrderedSetFullPopMax(multiOrderedSetFull *m, size_t count,
                                 databox *members, databox *scores) {
    size_t popped = 0;

    for (int32_t mapIdx = (int32_t)m->mapCount - 1;
         mapIdx >= 0 && popped < count; mapIdx--) {
        flex *map = getSubMap(m, mapIdx);
        if (!map) {
            continue;
        }

        while (popped < count) {
            size_t mapEntries = flexCount(map);
            if (mapEntries < MOS_ELEMENTS_PER_ENTRY) {
                break;
            }

            flexEntry *entry =
                flexIndex(map, mapEntries - MOS_ELEMENTS_PER_ENTRY);
            if (!entry) {
                break;
            }

            flexEntry *memberEntry = flexNext(map, entry);
            if (!memberEntry) {
                break;
            }

            /* In pool mode, convert pool ID back to member string */
            databox member = {{0}};
            databoxWide memberWide;
            if (!readMember(m, memberEntry, &member, &memberWide)) {
                break;
            }

            /* Self-contained results: copy before the entry goes away */
            mosReadOwned(entry, &scores[popped]);
            members[popped] = databoxCopy(&member);

            multidictDelete(m->memberIndex, &member);
            removeFromSubMap(m, mapIdx, entry);

            map = getSubMap(m, mapIdx);
            popped++;
        }
    }

    reclaimAllEmptySubMaps(m); /* backward sweep: bulk loop held live mapIdx */
    return popped;
}

/* ====================================================================
 * Lookup
 * ==================================================================== */

bool multiOrderedSetFullExists(const multiOrderedSetFull *m,
                               const databox *member) {
    return multidictExists((multidict *)m->memberIndex, member);
}

bool multiOrderedSetFullGetScore(const multiOrderedSetFull *m,
                                 const databox *member, databox *score) {
    return multidictFind((multidict *)m->memberIndex, member, score);
}

int64_t multiOrderedSetFullGetRank(const multiOrderedSetFull *m,
                                   const databox *member) {
    /* The member's score locates its row; every row of the sub-maps
     * before its sub-map ranks below it, and within the sub-map the rows
     * before it */
    uint32_t mapIdx;
    const flexEntry *row = findEntryByMember(m, member, &mapIdx);
    if (!row) {
        return -1;
    }

    int64_t rank = 0;
    for (uint32_t i = 0; i < mapIdx; i++) {
        const flex *map = getSubMap(m, i);
        if (map) {
            rank += (int64_t)(flexCount(map) / MOS_ELEMENTS_PER_ENTRY);
        }
    }

    flex *map = getSubMap(m, mapIdx);
    for (flexEntry *e = flexHead(map); e && e != row;
         e = flexNext(map, flexNext(map, e))) {
        rank++;
    }

    return rank;
}

int64_t multiOrderedSetFullGetReverseRank(const multiOrderedSetFull *m,
                                          const databox *member) {
    int64_t rank = multiOrderedSetFullGetRank(m, member);
    if (rank < 0) {
        return -1;
    }
    return (int64_t)(m->totalEntries - 1) - rank;
}

bool multiOrderedSetFullGetByRank(const multiOrderedSetFull *m, int64_t rank,
                                  databox *member, databox *score) {
    rank = mosNormalizeRank(rank, m->totalEntries);
    if (rank < 0) {
        return false;
    }

    int64_t currentRank = 0;

    for (uint32_t mapIdx = 0; mapIdx < m->mapCount; mapIdx++) {
        flex *map = getSubMap(m, mapIdx);
        if (!map) {
            continue;
        }

        size_t mapEntries = subMapCount(map);
        if (currentRank + (int64_t)mapEntries <= rank) {
            currentRank += (int64_t)mapEntries;
            continue;
        }

        /* Target is in this map */
        size_t localRank = (size_t)(rank - currentRank);
        size_t offset = localRank * MOS_ELEMENTS_PER_ENTRY;

        flexEntry *entry = flexIndex(map, offset);
        if (!entry) {
            return false;
        }

        flexEntry *memberEntry = flexNext(map, entry);
        if (!memberEntry) {
            return false;
        }

        flexGetByType(entry, score);

        /* In pool mode, convert pool ID back to member string */
        return readMember(m, memberEntry, member, NULL);
    }

    return false;
}

/* ====================================================================
 * Range Queries
 * ==================================================================== */

size_t multiOrderedSetFullCountByScore(const multiOrderedSetFull *m,
                                       const mosRangeSpec *range) {
    size_t count = 0;

    for (uint32_t mapIdx = 0; mapIdx < m->mapCount; mapIdx++) {
        flex *map = getSubMap(m, mapIdx);
        if (!map) {
            continue;
        }

        flexEntry *entry = flexHead(map);
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

            flexEntry *memberEntry = flexNext(map, entry);
            if (!memberEntry) {
                break;
            }
            entry = flexNext(map, memberEntry);
        }
    }

    return count;
}

/* ====================================================================
 * Iteration
 * ==================================================================== */

void multiOrderedSetFullIteratorInit(const multiOrderedSetFull *m,
                                     mosIterator *iter, bool forward) {
    iter->mos = (void *)m;
    iter->type = MOS_TYPE_FULL;
    iter->forward = forward;

    if (forward) {
        iter->mapIndex = 0;
        while (iter->mapIndex < m->mapCount) {
            flex *map = getSubMap(m, iter->mapIndex);
            if (map && flexCount(map) > 0) {
                iter->current = flexHead(map);
                break;
            }
            iter->mapIndex++;
        }
    } else {
        iter->mapIndex = m->mapCount - 1;
        while (iter->mapIndex < m->mapCount) { /* unsigned wrap check */
            flex *map = getSubMap(m, iter->mapIndex);
            size_t count = map ? flexCount(map) : 0;
            if (count >= MOS_ELEMENTS_PER_ENTRY) {
                iter->current = flexIndex(map, count - MOS_ELEMENTS_PER_ENTRY);
                break;
            }
            iter->mapIndex--;
        }
    }

    iter->valid = (iter->current != NULL);
}

bool multiOrderedSetFullIteratorInitAtScore(const multiOrderedSetFull *m,
                                            mosIterator *iter,
                                            const databox *score,
                                            bool forward) {
    iter->mos = (void *)m;
    iter->type = MOS_TYPE_FULL;
    iter->forward = forward;

    uint32_t mapIdx = findSubMapForScore(m, score);

    for (; mapIdx < m->mapCount; mapIdx++) {
        flex *map = getSubMap(m, mapIdx);
        if (!map) {
            continue;
        }

        flexEntry *entry = flexHead(map);
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

            flexEntry *memberEntry = flexNext(map, entry);
            if (!memberEntry) {
                break;
            }
            entry = flexNext(map, memberEntry);
        }
    }

    iter->current = NULL;
    iter->valid = false;
    return false;
}

bool multiOrderedSetFullIteratorInitAtRank(const multiOrderedSetFull *m,
                                           mosIterator *iter, int64_t rank,
                                           bool forward) {
    iter->mos = (void *)m;
    iter->type = MOS_TYPE_FULL;
    iter->forward = forward;

    rank = mosNormalizeRank(rank, m->totalEntries);
    if (rank < 0) {
        iter->current = NULL;
        iter->valid = false;
        return false;
    }

    int64_t currentRank = 0;

    for (uint32_t mapIdx = 0; mapIdx < m->mapCount; mapIdx++) {
        flex *map = getSubMap(m, mapIdx);
        if (!map) {
            continue;
        }

        size_t mapEntries = subMapCount(map);
        if (currentRank + (int64_t)mapEntries <= rank) {
            currentRank += (int64_t)mapEntries;
            continue;
        }

        size_t localRank = (size_t)(rank - currentRank);
        size_t offset = localRank * MOS_ELEMENTS_PER_ENTRY;

        iter->mapIndex = mapIdx;
        iter->current = flexIndex(map, offset);
        iter->valid = (iter->current != NULL);
        return iter->valid;
    }

    iter->current = NULL;
    iter->valid = false;
    return false;
}

bool multiOrderedSetFullIteratorNext(mosIterator *iter, databox *member,
                                     databox *score) {
    if (!iter->valid || !iter->current) {
        return false;
    }

    multiOrderedSetFull *m = (multiOrderedSetFull *)iter->mos;
    flexEntry *entry = (flexEntry *)iter->current;
    flex *map = getSubMap(m, iter->mapIndex);

    flexEntry *memberEntry = flexNext(map, entry);
    if (!memberEntry) {
        iter->valid = false;
        return false;
    }

    flexGetByType(entry, score);

    /* In pool mode, convert pool ID back to member string */
    if (!readMember(m, memberEntry, member, NULL)) {
        iter->valid = false;
        return false;
    }

    /* Advance iterator */
    if (iter->forward) {
        flexEntry *next = flexNext(map, memberEntry);
        if (!next) {
            /* Move to next map */
            iter->mapIndex++;
            while (iter->mapIndex < m->mapCount) {
                map = getSubMap(m, iter->mapIndex);
                if (map && flexCount(map) > 0) {
                    next = flexHead(map);
                    break;
                }
                iter->mapIndex++;
            }
        }
        iter->current = next;
    } else {
        /* The previous pair's score is two elements back */
        flexEntry *prevMember = flexPrev(map, entry);
        if (prevMember) {
            iter->current = flexPrev(map, prevMember);
        } else {
            /* Move to previous map */
            iter->current = NULL;
            while (iter->mapIndex > 0) {
                iter->mapIndex--;
                map = getSubMap(m, iter->mapIndex);
                size_t count = map ? flexCount(map) : 0;
                if (count >= MOS_ELEMENTS_PER_ENTRY) {
                    iter->current =
                        flexIndex(map, count - MOS_ELEMENTS_PER_ENTRY);
                    break;
                }
            }
        }
    }

    iter->valid = (iter->current != NULL);
    return true;
}

/* ====================================================================
 * First / Last
 * ==================================================================== */

bool multiOrderedSetFullFirst(const multiOrderedSetFull *m, databox *member,
                              databox *score) {
    for (uint32_t mapIdx = 0; mapIdx < m->mapCount; mapIdx++) {
        flex *map = getSubMap(m, mapIdx);
        if (!map) {
            continue;
        }

        flexEntry *entry = flexHead(map);
        if (entry) {
            flexEntry *memberEntry = flexNext(map, entry);
            if (memberEntry) {
                flexGetByType(entry, score);

                /* In pool mode, convert pool ID back to member string */
                return readMember(m, memberEntry, member, NULL);
            }
        }
    }
    return false;
}

bool multiOrderedSetFullLast(const multiOrderedSetFull *m, databox *member,
                             databox *score) {
    for (int32_t mapIdx = (int32_t)m->mapCount - 1; mapIdx >= 0; mapIdx--) {
        flex *map = getSubMap(m, mapIdx);
        if (!map) {
            continue;
        }

        size_t count = flexCount(map);
        if (count >= MOS_ELEMENTS_PER_ENTRY) {
            flexEntry *entry = flexIndex(map, count - MOS_ELEMENTS_PER_ENTRY);
            if (entry) {
                flexEntry *memberEntry = flexNext(map, entry);
                if (memberEntry) {
                    flexGetByType(entry, score);

                    /* In pool mode, convert pool ID back to member string */
                    return readMember(m, memberEntry, member, NULL);
                }
            }
        }
    }
    return false;
}

/* ====================================================================
 * Random
 * ==================================================================== */

/* Fill one array slot pair with the pair at 'rank', ignoring what the
 * slots held: a 96/128-bit value comes back deferred (type only) */
static bool mosFullRandomPick_(const void *set, size_t rank, databox *member,
                               databox *score) {
    mosBoxClear(member);
    mosBoxClear(score);
    return multiOrderedSetFullGetByRank(set, (int64_t)rank, member, score);
}

size_t multiOrderedSetFullRandomMembers(const multiOrderedSetFull *m,
                                        int64_t count, databox *members,
                                        databox *scores) {
    if (m->totalEntries == 0) {
        return 0;
    }

    return mosRandomMembersFill(m, mosFullRandomPick_, &mosRandomState,
                                (size_t)m->totalEntries, count, members,
                                scores);
}

/* ====================================================================
 * Debugging
 * ==================================================================== */

#ifdef DATAKIT_TEST
#include "ctest.h"
#include "perf.h"

void multiOrderedSetFullRepr(const multiOrderedSetFull *m) {
    printf("multiOrderedSetFull {\n");
    printf("  totalEntries: %" PRIu64 "\n", m->totalEntries);
    printf("  mapCount: %u\n", m->mapCount);
    printf("  bytes: %zu\n", multiOrderedSetFullBytes(m));

    for (uint32_t mapIdx = 0; mapIdx < m->mapCount; mapIdx++) {
        flex *map = getSubMap(m, mapIdx);
        printf("  map[%u]: count=%zu bytes=%zu\n", mapIdx,
               map ? subMapCount(map) : 0, map ? flexBytes(map) : 0);

        if (map) {
            flexEntry *entry = flexHead(map);
            int idx = 0;
            while (entry) {
                flexEntry *memberEntry = flexNext(map, entry);
                if (!memberEntry) {
                    break;
                }

                databoxBig score, member;
                flexGetByTypeBig(entry, &score);
                flexGetByTypeBig(memberEntry, &member);

                printf("    [%d] ", idx);
                databoxReprSay("score", (const databox *)&score);
                printf(" ");
                databoxReprSay("member", (const databox *)&member);
                printf("\n");

                idx++;
                entry = flexNext(map, memberEntry);
            }
        }
    }
    printf("}\n");
}

/* White-box invariant oracle for the sub-map structure (test only). Returns
 * the number of violations (0 = healthy). Sub-map splitting and every
 * mutation must preserve all of these; this is the fuzz oracle for the split
 * path. Checks: (1) rangeBox[i] == head/min score of sub-map i;
 * (2) global non-decreasing score order across sub-maps; (3) totalEntries ==
 * sum of per-sub-map pair counts. Empty sub-maps (rangeBox == INT64_MAX
 * sentinel) are skipped for (1)/(2). */
static int mosFullCheckInvariants(const multiOrderedSetFull *m) {
    int bad = 0;
    uint64_t sum = 0;
    databoxBig prevLast = {{0}};
    bool havePrev = false;
    for (uint32_t i = 0; i < m->mapCount; i++) {
        flex *map = getSubMap(m, i);
        const size_t cnt = map ? subMapCount(map) : 0;
        sum += cnt;
        if (cnt == 0) {
            continue; /* empty sub-map holds the INT64_MAX rangeBox sentinel */
        }
        databoxBig headScore;
        flexGetByTypeBig(flexHead(map), &headScore);
        const databox *rb = getRangeBox(m, i);
        if (rb && databoxCompare(rb, (const databox *)&headScore) != 0) {
            printf("  INVARIANT FAIL: submap %u rangeBox != head score\n", i);
            bad++;
        }

        /* A wide head owns a heap payload; every other head owns nothing */
        const bool rbWide = rb && mosBoxIsWide(rb);
        if (rb && (rbWide ? !(rb->big && rb->allocated && rb->data.ptr)
                          : (rb->big || rb->allocated))) {
            printf("  INVARIANT FAIL: submap %u rangeBox flags wrong\n", i);
            bad++;
        }
        if (havePrev && databoxCompare((const databox *)&prevLast,
                                       (const databox *)&headScore) > 0) {
            printf("  INVARIANT FAIL: submap %u breaks global score order\n",
                   i);
            bad++;
        }
        flexGetByTypeBig(flexIndex(map, (cnt - 1) * MOS_ELEMENTS_PER_ENTRY),
                         &prevLast);
        havePrev = true;
    }
    if (sum != m->totalEntries) {
        printf("  INVARIANT FAIL: totalEntries %" PRIu64
               " != sum of sub-map counts %" PRIu64 "\n",
               m->totalEntries, sum);
        bad++;
    }
    return bad;
}

/* The copy test's members: member s holds score s */
#define MOS_COPY_N 3000
static void mosCopyName_(char *buf, size_t len, int64_t s) {
    snprintf(buf, len, "copied-member-%05" PRId64, s);
}

/* 'member' and 'score' are member s with score s */
static bool mosCopyIs_(const databox *member, const databox *score, int64_t s) {
    char want[32];
    mosCopyName_(want, sizeof(want), s);
    const bool scoreOk = score->type == DATABOX_SIGNED_64 ? score->data.i64 == s
                         : score->type == DATABOX_UNSIGNED_64
                             ? s >= 0 && score->data.u64 == (uint64_t)s
                             : false;
    return scoreOk && databoxLen(member) == strlen(want) &&
           memcmp(databoxBytes(member), want, strlen(want)) == 0;
}

/* Every read of 'm' against 'present' (score s is in the set as member
 * s); returns the mismatches */
static int mosCopyCheck_(const multiOrderedSetFull *m, const bool *present,
                         const char *what) {
    int bad = 0;
    int64_t order[MOS_COPY_N];
    int64_t n = 0;
    for (int64_t s = 0; s < MOS_COPY_N; s++) {
        if (present[s]) {
            order[n++] = s;
        }
    }

    if ((int64_t)multiOrderedSetFullCount(m) != n ||
        mosFullCheckInvariants(m) != 0) {
        printf("%s: count %zu, want %" PRId64 " (or invariants)\n", what,
               multiOrderedSetFullCount(m), n);
        return 1;
    }

    /* Both directions, member names included */
    for (int dir = 0; dir < 2; dir++) {
        mosIterator it;
        multiOrderedSetFullIteratorInit(m, &it, dir == 0);
        databox member;
        databox score;
        int64_t k = 0;
        while (multiOrderedSetFullIteratorNext(&it, &member, &score)) {
            if (k >= n || !mosCopyIs_(&member, &score,
                                      dir == 0 ? order[k] : order[n - 1 - k])) {
                printf("%s: iteration %d step %" PRId64 " wrong\n", what, dir,
                       k);
                bad++;
                break;
            }
            k++;
        }
        if (k != n) {
            printf("%s: iteration %d gave %" PRId64 " of %" PRId64 "\n", what,
                   dir, k, n);
            bad++;
        }
    }

    /* Ranks, positioned iterators, score lookups */
    for (int64_t r = 0; r < n; r += 37) {
        databox member;
        databox score;
        char name[32];
        mosCopyName_(name, sizeof(name), order[r]);
        const databox key = databoxNewBytesString(name);
        mosIterator it;
        databox im;
        databox is;
        const char *wrong = NULL;
        if (!multiOrderedSetFullGetByRank(m, r, &member, &score) ||
            !mosCopyIs_(&member, &score, order[r])) {
            wrong = "GetByRank";
        } else if (multiOrderedSetFullGetRank(m, &key) != r) {
            wrong = "GetRank";
        } else if (multiOrderedSetFullGetReverseRank(m, &key) != n - 1 - r) {
            wrong = "GetReverseRank";
        } else if (!multiOrderedSetFullIteratorInitAtRank(m, &it, r, true) ||
                   !multiOrderedSetFullIteratorNext(&it, &im, &is) ||
                   !mosCopyIs_(&im, &is, order[r])) {
            wrong = "IteratorInitAtRank forward";
        } else if (!multiOrderedSetFullIteratorInitAtRank(m, &it, r, false) ||
                   !multiOrderedSetFullIteratorNext(&it, &im, &is) ||
                   !mosCopyIs_(&im, &is, order[r])) {
            wrong = "IteratorInitAtRank backward";
        }
        if (wrong) {
            printf("%s: %s at rank %" PRId64 " (score %" PRId64 ") wrong\n",
                   what, wrong, r, order[r]);
            bad++;
            break;
        }
        /* Positioned at a score that is absent lands on the next one */
        const databox at = databoxNewSigned(order[r] - (r ? 0 : 1));
        if (!multiOrderedSetFullIteratorInitAtScore(m, &it, &at, true) ||
            !multiOrderedSetFullIteratorNext(&it, &im, &is) ||
            !mosCopyIs_(&im, &is, order[r])) {
            printf("%s: at score %" PRId64 " wrong\n", what, order[r]);
            bad++;
            break;
        }
    }

    databox fm;
    databox fs;
    databox lm;
    databox ls;
    const mosRangeSpec range = {.min = databoxNewSigned(500),
                                .max = databoxNewSigned(2500),
                                .minExclusive = true};
    int64_t inRange = 0;
    for (int64_t k = 0; k < n; k++) {
        inRange += order[k] > 500 && order[k] <= 2500;
    }
    if (n && (!multiOrderedSetFullFirst(m, &fm, &fs) ||
              !mosCopyIs_(&fm, &fs, order[0]) ||
              !multiOrderedSetFullLast(m, &lm, &ls) ||
              !mosCopyIs_(&lm, &ls, order[n - 1]) ||
              (int64_t)multiOrderedSetFullCountByScore(m, &range) != inRange)) {
        printf("%s: first/last/count-by-score wrong\n", what);
        bad++;
    }
    return bad;
}

/* A Full set spanning several sub-maps against a presence oracle over
 * scores 0..MOSF_N-1 (member "f<score>") */
#define MOSF_N 2400
static void mosfName_(char *buf, size_t len, int64_t s) {
    snprintf(buf, len, "f%05" PRId64, s);
}

static bool mosfIs_(const databox *member, const databox *score, int64_t s) {
    char want[24]; /* "f" and any int64 */
    mosfName_(want, sizeof(want), s);
    const bool scoreOk = score->type == DATABOX_SIGNED_64 ? score->data.i64 == s
                         : score->type == DATABOX_UNSIGNED_64
                             ? s >= 0 && score->data.u64 == (uint64_t)s
                             : false;
    return scoreOk && databoxLen(member) == strlen(want) &&
           memcmp(databoxBytes(member), want, strlen(want)) == 0;
}

/* The present scores in rank order into 'list'; returns how many */
static int64_t mosfList_(const bool *present, int64_t *list) {
    int64_t n = 0;
    for (int64_t s = 0; s < MOSF_N; s++) {
        if (present[s]) {
            list[n++] = s;
        }
    }
    return n;
}

static int mosfCheck_(const multiOrderedSetFull *m, const bool *present,
                      int step, uint64_t *rng) {
    static int64_t list[MOSF_N];
    const int64_t n = mosfList_(present, list);
    if (multiOrderedSetFullCount(m) != (size_t)n) {
        printf("ERROR: step %d: count %zu, oracle %" PRId64 "\n", step,
               multiOrderedSetFullCount(m), n);
        return 1;
    }
    int bad = mosFullCheckInvariants(m) != 0;

    /* Ranks past either end refuse */
    databox member;
    databox score;
    mosIterator it;
    if (multiOrderedSetFullGetByRank(m, n, &member, &score) ||
        multiOrderedSetFullGetByRank(m, -n - 1, &member, &score) ||
        multiOrderedSetFullIteratorInitAtRank(m, &it, n, true)) {
        printf("ERROR: step %d: a rank past the end answered\n", step);
        bad++;
    }

    /* Sampled ranks, from the front and the back, and a short walk */
    for (int k = 0; k < 6 && n > 0; k++) {
        const int64_t r = (int64_t)(splitmix64(rng) % (uint64_t)n);
        const int64_t s = list[r];
        if (!multiOrderedSetFullGetByRank(m, r, &member, &score) ||
            !mosfIs_(&member, &score, s) ||
            !multiOrderedSetFullGetByRank(m, r - n, &member, &score) ||
            !mosfIs_(&member, &score, s)) {
            printf("ERROR: step %d: rank %" PRId64 " is not %" PRId64 "\n",
                   step, r, s);
            bad++;
            continue;
        }
        const bool forward = k % 2 == 0;
        if (!multiOrderedSetFullIteratorInitAtRank(m, &it, r, forward)) {
            printf("ERROR: step %d: iterator at rank %" PRId64 "\n", step, r);
            bad++;
            continue;
        }
        for (int64_t w = 0; w < 4; w++) {
            const int64_t at = forward ? r + w : r - w;
            const bool more =
                multiOrderedSetFullIteratorNext(&it, &member, &score);
            if (at < 0 || at >= n) {
                if (more) {
                    printf("ERROR: step %d: walk ran past the end\n", step);
                    bad++;
                }
                break;
            }
            if (!more || !mosfIs_(&member, &score, list[at])) {
                printf("ERROR: step %d: walk from %" PRId64 " step %" PRId64
                       "\n",
                       step, r, w);
                bad++;
                break;
            }
        }
    }
    return bad;
}

int multiOrderedSetFullTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    TEST("multiOrderedSetFull — create and free") {
        multiOrderedSetFull *mos = multiOrderedSetFullNew();
        if (!mos) {
            ERRR("Failed to create multiOrderedSetFull");
        }
        if (multiOrderedSetFullCount(mos) != 0) {
            ERRR("New set should be empty");
        }
        multiOrderedSetFullFree(mos);
    }

    TEST(
        "multiOrderedSetFull — pooled members: an external and an owned pool") {
        /* @own: caller; free=atomPoolFree */
        atomPool *shared = atomPoolNew(ATOM_POOL_HASH);
        multiOrderedSetFull *a = multiOrderedSetFullNewWithPool(shared);
        multiOrderedSetFull *b = multiOrderedSetFullNewWithOwnedPool();
        for (int i = 0; i < 50; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "a-long-shared-member-%03d", i);
            const databox member = databoxNewBytesString(buf);
            const databox score = databoxNewSigned(i);
            multiOrderedSetFullAdd(a, &score, &member);
            multiOrderedSetFullAdd(b, &score, &member);
        }
        const databox m7 = databoxNewBytesString("a-long-shared-member-007");
        databox sa;
        databox sb;
        if (multiOrderedSetFullCount(a) != 50 ||
            multiOrderedSetFullCount(b) != 50 ||
            !multiOrderedSetFullGetScore(a, &m7, &sa) || sa.data.i64 != 7 ||
            !multiOrderedSetFullGetScore(b, &m7, &sb) || sb.data.i64 != 7 ||
            atomPoolCount(shared) != 50) {
            ERRR("a pooled set lost members or scores");
        }

        /* Freeing the set leaves the external pool to its owner */
        multiOrderedSetFullFree(a);
        multiOrderedSetFullFree(b);
        if (atomPoolCount(shared) > 50) {
            ERRR("the external pool grew after its set was freed");
        }
        atomPoolFree(shared);
    }

    TEST("MOS-Full: one score over many sub-maps stays in member order") {
        /* 2000 members at one score: one sub-map past its size limit that
         * has no score boundary to split at (and must not cost a scan per
         * insert to find that out); members joining at that score
         * afterwards (below, between and above the rest in member order)
         * iterate in member order and rank where iteration puts them */
        multiOrderedSetFull *m = multiOrderedSetFullNew();
        const databox seven = databoxNewSigned(7);
        for (int i = 0; i < 2000; i++) {
            char buf[24];
            snprintf(buf, sizeof(buf), "same-%05d", i * 2);
            const databox member = databoxNewBytesString(buf);
            multiOrderedSetFullAdd(m, &seven, &member);
        }
        static const char *later[] = {"a", "same-00001", "same-01999",
                                      "same-03999", "zzz"};
        for (size_t i = 0; i < 5; i++) {
            const databox member = databoxNewBytesString(later[i]);
            multiOrderedSetFullAdd(m, &seven, &member);
        }
        mosIterator it;
        multiOrderedSetFullIteratorInit(m, &it, true);
        databox member;
        databox score;
        char prev[32] = "";
        size_t prevLen = 0;
        int64_t rank = 0;
        bool ordered = true;
        bool ranked = true;
        while (multiOrderedSetFullIteratorNext(&it, &member, &score)) {
            const databox before = databoxNewBytes(prev, prevLen);
            if (rank && databoxCompare(&before, &member) >= 0) {
                ordered = false;
            }
            prevLen = databoxLen(&member) < sizeof(prev) ? databoxLen(&member)
                                                         : sizeof(prev);
            memcpy(prev, databoxBytes(&member), prevLen);
            const databox key = databoxNewBytes(prev, prevLen);
            if (rank % 97 == 0 || prevLen < 6 || prev[0] == 'z' ||
                prev[prevLen - 1] == '9') {
                ranked = ranked && multiOrderedSetFullGetRank(m, &key) == rank;
            }
            rank++;
        }
        if (!ordered || !ranked || rank != 2005 ||
            mosFullCheckInvariants(m) != 0) {
            ERR("one score: %" PRId64 " iterated, ordered %d, ranked %d", rank,
                ordered, ranked);
        }
        multiOrderedSetFullFree(m);
    }

    TEST("MOS-Full: copies of plain, owned-pool and shared-pool sets") {
        /* 3000 members over many sub-maps, then a score band, a rank band
         * and both ends removed (emptying middle sub-maps); the copy,
         * checked after its source is freed, answers every read as the
         * oracle does, and keeps working when written to */
        /* @own: caller; free=atomPoolFree */
        atomPool *shared = atomPoolNew(ATOM_POOL_HASH);
        static const char *kinds[] = {"plain", "owned pool", "shared pool"};
        for (int kind = 0; kind < 3; kind++) {
            /* @own: caller; free=multiOrderedSetFullFree */
            multiOrderedSetFull *m =
                kind == 0   ? multiOrderedSetFullNew()
                : kind == 1 ? multiOrderedSetFullNewWithOwnedPool()
                            : multiOrderedSetFullNewWithPool(shared);
            bool present[MOS_COPY_N] = {0};
            for (int64_t i = 0; i < MOS_COPY_N; i++) {
                const int64_t s = (i * 1237) % MOS_COPY_N;
                char name[32];
                mosCopyName_(name, sizeof(name), s);
                const databox member = databoxNewBytesString(name);
                const databox score = databoxNewSigned(s);
                multiOrderedSetFullAdd(m, &score, &member);
                present[s] = true;
            }

            const mosRangeSpec band = {.min = databoxNewSigned(1000),
                                       .max = databoxNewSigned(1599)};
            const size_t byScore =
                multiOrderedSetFullRemoveRangeByScore(m, &band);
            for (int64_t s = 1000; s < 1600; s++) {
                present[s] = false;
            }
            /* Ranks 100-199 are scores 100-199: nothing below is gone */
            const size_t byRank =
                multiOrderedSetFullRemoveRangeByRank(m, 100, 199);
            for (int64_t s = 100; s < 200; s++) {
                present[s] = false;
            }
            databox pm[5];
            databox ps[5];
            const size_t lows = multiOrderedSetFullPopMin(m, 5, pm, ps);
            for (size_t i = 0; i < lows; i++) {
                if (!mosCopyIs_(&pm[i], &ps[i], (int64_t)i)) {
                    ERR("%s: PopMin %zu wrong", kinds[kind], i);
                }
                present[i] = false;
                databoxFreeData(&pm[i]);
                databoxFreeData(&ps[i]);
            }
            const size_t highs = multiOrderedSetFullPopMax(m, 5, pm, ps);
            for (size_t i = 0; i < highs; i++) {
                const int64_t s = MOS_COPY_N - 1 - (int64_t)i;
                if (!mosCopyIs_(&pm[i], &ps[i], s)) {
                    ERR("%s: PopMax %zu wrong", kinds[kind], i);
                }
                present[s] = false;
                databoxFreeData(&pm[i]);
                databoxFreeData(&ps[i]);
            }
            if (byScore != 600 || byRank != 100 || lows != 5 || highs != 5) {
                ERR("%s: removed %zu by score, %zu by rank, popped %zu/%zu",
                    kinds[kind], byScore, byRank, lows, highs);
            }
            char label[48];
            snprintf(label, sizeof(label), "%s source", kinds[kind]);
            err += mosCopyCheck_(m, present, label);

            /* @own: caller; free=multiOrderedSetFullFree */
            multiOrderedSetFull *copy = multiOrderedSetFullCopy(m);
            multiOrderedSetFullFree(m);
            snprintf(label, sizeof(label), "%s copy", kinds[kind]);
            err += mosCopyCheck_(copy, present, label);

            for (int64_t s = 1000; s < 1100; s++) {
                char name[32];
                mosCopyName_(name, sizeof(name), s);
                const databox member = databoxNewBytesString(name);
                const databox score = databoxNewSigned(s);
                multiOrderedSetFullAdd(copy, &score, &member);
                present[s] = true;
            }
            snprintf(label, sizeof(label), "%s copy after adds", kinds[kind]);
            err += mosCopyCheck_(copy, present, label);
            multiOrderedSetFullFree(copy);
        }
        atomPoolFree(shared);
    }

    TEST("multiOrderedSetFull — add and lookup") {
        multiOrderedSetFull *mos = multiOrderedSetFullNew();

        for (int i = 0; i < 100; i++) {
            databox score = databoxNewSigned(i * 10);
            char buf[16];
            snprintf(buf, sizeof(buf), "member%03d", i);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            multiOrderedSetFullAdd(mos, &score, &member);
        }

        if (multiOrderedSetFullCount(mos) != 100) {
            ERR("Count should be 100, got %zu", multiOrderedSetFullCount(mos));
        }

        /* Test O(1) member lookup */
        for (int i = 0; i < 100; i++) {
            char buf[16];
            snprintf(buf, sizeof(buf), "member%03d", i);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            databox score;

            if (!multiOrderedSetFullGetScore(mos, &member, &score)) {
                ERR("GetScore for member%03d failed", i);
            }
            if (score.data.i != i * 10) {
                ERR("member%03d score should be %d, got %" PRIdMAX, i, i * 10,
                    score.data.i);
            }
        }

        /* Test rank queries */
        for (int i = 0; i < 100; i++) {
            databox m, s;
            if (!multiOrderedSetFullGetByRank(mos, i, &m, &s)) {
                ERR("GetByRank(%d) failed", i);
            }
            if (s.data.i != i * 10) {
                ERR("Rank %d should have score %d, got %" PRIdMAX, i, i * 10,
                    s.data.i);
            }
        }

        multiOrderedSetFullFree(mos);
    }

    TEST("multiOrderedSetFull — atomPool mode basic (both backends)") {
        /* Test both atomPool backends */
        atomPoolType types[] = {ATOM_POOL_HASH, ATOM_POOL_TREE};
        const char *typeNames[] = {"HASH", "TREE"};

        for (int t = 0; t < 2; t++) {
            /* Test basic operations with owned pool */
            multiOrderedSetFull *mos =
                multiOrderedSetFullNewWithPoolType(types[t]);
            if (!mos) {
                ERR("[%s] Failed to create multiOrderedSetFull with pool",
                    typeNames[t]);
                continue;
            }

            /* Add entries */
            for (int i = 0; i < 100; i++) {
                databox score = databoxNewSigned(i * 10);
                char buf[32];
                snprintf(buf, sizeof(buf), "poolmember%03d", i);
                databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
                multiOrderedSetFullAdd(mos, &score, &member);
            }

            if (multiOrderedSetFullCount(mos) != 100) {
                ERR("[%s] Pool mode: Count should be 100, got %zu",
                    typeNames[t], multiOrderedSetFullCount(mos));
            }

            /* Verify lookups work correctly */
            for (int i = 0; i < 100; i++) {
                char buf[32];
                snprintf(buf, sizeof(buf), "poolmember%03d", i);
                databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
                databox score;

                if (!multiOrderedSetFullGetScore(mos, &member, &score)) {
                    ERR("[%s] Pool mode: GetScore for poolmember%03d failed",
                        typeNames[t], i);
                }
                if (score.data.i != i * 10) {
                    ERR("[%s] Pool mode: poolmember%03d score should be %d, "
                        "got %" PRIdMAX,
                        typeNames[t], i, i * 10, score.data.i);
                }
            }

            /* Verify iteration returns correct members */
            mosIterator iter;
            multiOrderedSetFullIteratorInit(mos, &iter, true);
            int count = 0;
            databox m, s;
            while (multiOrderedSetFullIteratorNext(&iter, &m, &s)) {
                if (m.type != DATABOX_BYTES) {
                    ERR("[%s] Pool mode iteration: member should be BYTES, got "
                        "%d",
                        typeNames[t], m.type);
                }
                count++;
            }
            if (count != 100) {
                ERR("[%s] Pool mode iteration: should iterate 100, got %d",
                    typeNames[t], count);
            }

            /* Verify GetByRank returns correct members */
            for (int i = 0; i < 100; i++) {
                if (!multiOrderedSetFullGetByRank(mos, i, &m, &s)) {
                    ERR("[%s] Pool mode: GetByRank(%d) failed", typeNames[t],
                        i);
                }
                if (m.type != DATABOX_BYTES) {
                    ERR("[%s] Pool mode GetByRank: member should be BYTES, got "
                        "%d",
                        typeNames[t], m.type);
                }
            }

            multiOrderedSetFullFree(mos);
        }
    }

    TEST("multiOrderedSetFull — inline vs pool memory comparison") {
        printf(
            "\n=== Inline vs Pool Comparison (varying string lengths) ===\n");
        printf("=== Testing both HASH and TREE atomPool backends ===\n\n");

        /* Test with different string lengths to show where pool mode helps.
         * Pool overhead is ~84 bytes/entry (HASH) or ~22 bytes/entry (TREE).
         * With short strings, overhead dominates. With long strings, pool wins.
         */
        struct {
            size_t strLen;
            const char *name;
        } strTests[] = {
            {16, "Short (16 bytes)"},
            {36, "Medium (36 bytes)"},
            {48, "Realistic (48 bytes)"},
            {64, "Long (64 bytes)"},
        };

        const size_t N = 5000; /* entries per test */

        for (size_t st = 0; st < 4; st++) {
            size_t strLen = strTests[st].strLen;
            printf("--- %s strings, %zu entries ---\n", strTests[st].name, N);

            /* Create test sets: inline, pool HASH, pool TREE */
            multiOrderedSetFull *mosInline = multiOrderedSetFullNew();
            multiOrderedSetFull *mosHash =
                multiOrderedSetFullNewWithPoolType(ATOM_POOL_HASH);
            multiOrderedSetFull *mosTree =
                multiOrderedSetFullNewWithPoolType(ATOM_POOL_TREE);

            /* Generate fixed-length member strings */
            char buf[128];

            /* Insert - Inline */
            uint64_t inlineInsertStart = perfNowNs();
            for (size_t i = 0; i < N; i++) {
                databox score = databoxNewUnsigned(i);
                /* Generate string of exact length */
                memset(buf, 'A' + (i % 26), strLen);
                snprintf(buf, strLen + 1, "m%0*zu", (int)(strLen - 2), i);
                buf[strLen] = '\0';
                databox member = databoxNewBytesAllowEmbed(buf, strLen);
                multiOrderedSetFullAdd(mosInline, &score, &member);
            }
            uint64_t inlineInsertEnd = perfNowNs();

            /* Insert - Pool HASH */
            uint64_t hashInsertStart = perfNowNs();
            for (size_t i = 0; i < N; i++) {
                databox score = databoxNewUnsigned(i);
                memset(buf, 'A' + (i % 26), strLen);
                snprintf(buf, strLen + 1, "m%0*zu", (int)(strLen - 2), i);
                buf[strLen] = '\0';
                databox member = databoxNewBytesAllowEmbed(buf, strLen);
                multiOrderedSetFullAdd(mosHash, &score, &member);
            }
            uint64_t hashInsertEnd = perfNowNs();

            /* Insert - Pool TREE */
            uint64_t treeInsertStart = perfNowNs();
            for (size_t i = 0; i < N; i++) {
                databox score = databoxNewUnsigned(i);
                memset(buf, 'A' + (i % 26), strLen);
                snprintf(buf, strLen + 1, "m%0*zu", (int)(strLen - 2), i);
                buf[strLen] = '\0';
                databox member = databoxNewBytesAllowEmbed(buf, strLen);
                multiOrderedSetFullAdd(mosTree, &score, &member);
            }
            uint64_t treeInsertEnd = perfNowNs();

            size_t inlineBytes = multiOrderedSetFullBytes(mosInline);
            size_t hashBytes = multiOrderedSetFullBytes(mosHash);
            size_t treeBytes = multiOrderedSetFullBytes(mosTree);

            printf("  Memory (bytes/entry):\n");
            printf("    Inline:     %zu total (%.1f bytes/entry)\n",
                   inlineBytes, (double)inlineBytes / N);
            printf("    Pool HASH:  %zu total (%.1f bytes/entry) [%+.1f%%]\n",
                   hashBytes, (double)hashBytes / N,
                   100.0 * ((double)hashBytes / inlineBytes - 1.0));
            printf("    Pool TREE:  %zu total (%.1f bytes/entry) [%+.1f%%]\n",
                   treeBytes, (double)treeBytes / N,
                   100.0 * ((double)treeBytes / inlineBytes - 1.0));

            printf("  Insert (ns/op):\n");
            printf("    Inline:     %.1f\n",
                   (double)(inlineInsertEnd - inlineInsertStart) / N);
            printf("    Pool HASH:  %.1f\n",
                   (double)(hashInsertEnd - hashInsertStart) / N);
            printf("    Pool TREE:  %.1f\n",
                   (double)(treeInsertEnd - treeInsertStart) / N);

            /* Lookup benchmark */
            uint64_t inlineLookupStart = perfNowNs();
            for (size_t i = 0; i < N; i++) {
                memset(buf, 'A' + (i % 26), strLen);
                snprintf(buf, strLen + 1, "m%0*zu", (int)(strLen - 2), i);
                buf[strLen] = '\0';
                databox member = databoxNewBytesAllowEmbed(buf, strLen);
                databox score;
                multiOrderedSetFullGetScore(mosInline, &member, &score);
            }
            uint64_t inlineLookupEnd = perfNowNs();

            uint64_t hashLookupStart = perfNowNs();
            for (size_t i = 0; i < N; i++) {
                memset(buf, 'A' + (i % 26), strLen);
                snprintf(buf, strLen + 1, "m%0*zu", (int)(strLen - 2), i);
                buf[strLen] = '\0';
                databox member = databoxNewBytesAllowEmbed(buf, strLen);
                databox score;
                multiOrderedSetFullGetScore(mosHash, &member, &score);
            }
            uint64_t hashLookupEnd = perfNowNs();

            uint64_t treeLookupStart = perfNowNs();
            for (size_t i = 0; i < N; i++) {
                memset(buf, 'A' + (i % 26), strLen);
                snprintf(buf, strLen + 1, "m%0*zu", (int)(strLen - 2), i);
                buf[strLen] = '\0';
                databox member = databoxNewBytesAllowEmbed(buf, strLen);
                databox score;
                multiOrderedSetFullGetScore(mosTree, &member, &score);
            }
            uint64_t treeLookupEnd = perfNowNs();

            printf("  Lookup (ns/op):\n");
            printf("    Inline:     %.1f\n",
                   (double)(inlineLookupEnd - inlineLookupStart) / N);
            printf("    Pool HASH:  %.1f\n",
                   (double)(hashLookupEnd - hashLookupStart) / N);
            printf("    Pool TREE:  %.1f\n",
                   (double)(treeLookupEnd - treeLookupStart) / N);

            /* Iteration benchmark */
            mosIterator iter;
            databox m, s;

            uint64_t inlineIterStart = perfNowNs();
            multiOrderedSetFullIteratorInit(mosInline, &iter, true);
            while (multiOrderedSetFullIteratorNext(&iter, &m, &s)) {
            }
            uint64_t inlineIterEnd = perfNowNs();

            uint64_t hashIterStart = perfNowNs();
            multiOrderedSetFullIteratorInit(mosHash, &iter, true);
            while (multiOrderedSetFullIteratorNext(&iter, &m, &s)) {
            }
            uint64_t hashIterEnd = perfNowNs();

            uint64_t treeIterStart = perfNowNs();
            multiOrderedSetFullIteratorInit(mosTree, &iter, true);
            while (multiOrderedSetFullIteratorNext(&iter, &m, &s)) {
            }
            uint64_t treeIterEnd = perfNowNs();

            printf("  Iteration (ns/op):\n");
            printf("    Inline:     %.2f\n",
                   (double)(inlineIterEnd - inlineIterStart) / N);
            printf("    Pool HASH:  %.2f\n",
                   (double)(hashIterEnd - hashIterStart) / N);
            printf("    Pool TREE:  %.2f\n",
                   (double)(treeIterEnd - treeIterStart) / N);

            printf("\n");

            multiOrderedSetFullFree(mosInline);
            multiOrderedSetFullFree(mosHash);
            multiOrderedSetFullFree(mosTree);
        }

        /* Additional test: Demonstrate deduplication savings.
         * Pool mode really shines when the same member is used multiple times
         * (e.g., updating scores for existing members). */
        printf("--- Deduplication Test (5000 unique from 50000 inserts) ---\n");
        {
            const size_t totalOps = 50000;
            const size_t uniqueMembers = 5000;
            const size_t strLen = 48;

            multiOrderedSetFull *mosInline = multiOrderedSetFullNew();
            multiOrderedSetFull *mosHash =
                multiOrderedSetFullNewWithPoolType(ATOM_POOL_HASH);
            multiOrderedSetFull *mosTree =
                multiOrderedSetFullNewWithPoolType(ATOM_POOL_TREE);

            char buf[128];

            /* Insert with heavy reuse (10 score updates per member on avg) */
            for (size_t i = 0; i < totalOps; i++) {
                size_t memberIdx = i % uniqueMembers;
                databox score = databoxNewUnsigned(i);
                memset(buf, 'A' + (memberIdx % 26), strLen);
                snprintf(buf, strLen + 1, "m%0*zu", (int)(strLen - 2),
                         memberIdx);
                buf[strLen] = '\0';
                databox member = databoxNewBytesAllowEmbed(buf, strLen);
                multiOrderedSetFullAdd(mosInline, &score, &member);
                multiOrderedSetFullAdd(mosHash, &score, &member);
                multiOrderedSetFullAdd(mosTree, &score, &member);
            }

            size_t inlineBytes = multiOrderedSetFullBytes(mosInline);
            size_t hashBytes = multiOrderedSetFullBytes(mosHash);
            size_t treeBytes = multiOrderedSetFullBytes(mosTree);

            printf("  Memory after %zu ops (%zu unique 48-byte members):\n",
                   totalOps, uniqueMembers);
            printf("    Inline:     %zu bytes (%.1f bytes/member)\n",
                   inlineBytes, (double)inlineBytes / uniqueMembers);
            printf("    Pool HASH:  %zu bytes (%.1f bytes/member) [%+.1f%%]\n",
                   hashBytes, (double)hashBytes / uniqueMembers,
                   100.0 * ((double)hashBytes / inlineBytes - 1.0));
            printf("    Pool TREE:  %zu bytes (%.1f bytes/member) [%+.1f%%]\n",
                   treeBytes, (double)treeBytes / uniqueMembers,
                   100.0 * ((double)treeBytes / inlineBytes - 1.0));

            multiOrderedSetFullFree(mosInline);
            multiOrderedSetFullFree(mosHash);
            multiOrderedSetFullFree(mosTree);
        }

        /* Summary: Highlight the key tradeoffs */
        printf("=== BACKEND SELECTION SUMMARY ===\n");
        printf("┌─────────────────────────────────────────────────────────────┐"
               "\n");
        printf("│ ATOM_POOL_HASH (stringPool):                                "
               "│\n");
        printf("│   Memory:  ~84 bytes/entry overhead (2-3x more than TREE)   "
               "│\n");
        printf("│   Speed:   O(1) lookup, faster iteration                    "
               "│\n");
        printf("│   Best for: Read-heavy workloads, iteration-intensive apps  "
               "│\n");
        printf("├─────────────────────────────────────────────────────────────┤"
               "\n");
        printf("│ ATOM_POOL_TREE (multimapAtom):                              "
               "│\n");
        printf("│   Memory:  ~22 bytes/entry overhead (3-4x less than HASH)   "
               "│\n");
        printf("│   Speed:   O(log n) lookup, iteration 5-6x slower           "
               "│\n");
        printf("│   Best for: Memory-constrained, write-heavy, small pools    "
               "│\n");
        printf("├─────────────────────────────────────────────────────────────┤"
               "\n");
        printf("│ CRITICAL: TREE is 5-10x SLOWER for iteration than HASH!     "
               "│\n");
        printf("│ If you iterate frequently, use HASH despite memory cost.    "
               "│\n");
        printf("└─────────────────────────────────────────────────────────────┘"
               "\n");
        printf("\n");
    }

    /* ================================================================
     * Sub-map invariant oracle — validates mosFullCheckInvariants itself on
     * healthy sets, both single sub-map and split (mapCount > 1).
     * ================================================================ */
    TEST("MOS-Full: random ops across sub-maps against a sorted oracle") {
        /* Pops and range removals large enough to empty whole sub-maps
         * and cross into the next, mixed with adds; after every step the
         * count, invariants, sampled ranks from both ends, short walks
         * both ways and out-of-range refusals agree with the oracle */
        multiOrderedSetFull *m = multiOrderedSetFullNew();
        static bool present[MOSF_N];
        memset(present, 0, sizeof(present));
        uint64_t rng = 0xf011u;
        char name[24]; /* "f" and any int64 */
        for (int64_t s = 0; s < MOSF_N;
             s += 1 + (int64_t)(splitmix64(&rng) % 2)) {
            mosfName_(name, sizeof(name), s);
            const databox member = databoxNewBytesString(name);
            const databox score = databoxNewSigned(s);
            multiOrderedSetFullAdd(m, &score, &member);
            present[s] = true;
        }
        static databox pm[700];
        static databox ps[700];
        static int64_t list[MOSF_N];
        for (int step = 0; step < 160 && !err; step++) {
            const uint64_t op = splitmix64(&rng) % 6;
            const int64_t n = mosfList_(present, list);
            if (op <= 1 || n < 200) {
                for (int i = 0; i < 120; i++) {
                    const int64_t s = (int64_t)(splitmix64(&rng) % MOSF_N);
                    mosfName_(name, sizeof(name), s);
                    const databox member = databoxNewBytesString(name);
                    const databox score = databoxNewSigned(s);
                    multiOrderedSetFullAdd(m, &score, &member);
                    present[s] = true;
                }
            } else if (op == 2 || op == 3) {
                const size_t want = 1 + splitmix64(&rng) % 700;
                const size_t got =
                    op == 2 ? multiOrderedSetFullPopMin(m, want, pm, ps)
                            : multiOrderedSetFullPopMax(m, want, pm, ps);
                if (got != (want < (size_t)n ? want : (size_t)n)) {
                    ERR("step %d: popped %zu of %zu", step, got, want);
                }
                for (size_t i = 0; i < got && (int64_t)i < n; i++) {
                    const int64_t s =
                        op == 2 ? list[i] : list[n - 1 - (int64_t)i];
                    if (!mosfIs_(&pm[i], &ps[i], s)) {
                        ERR("step %d: pop %zu wrong", step, i);
                    }
                    present[s] = false;
                    databoxFreeData(&pm[i]);
                    databoxFreeData(&ps[i]);
                }
            } else if (op == 4) {
                const int64_t a = (int64_t)(splitmix64(&rng) % (uint64_t)n);
                const int64_t b = a + (int64_t)(splitmix64(&rng) % 600);
                const int64_t last = b < n ? b : n - 1;
                size_t want = 0;
                for (int64_t r = a; r <= last; r++) {
                    present[list[r]] = false;
                    want++;
                }
                if (multiOrderedSetFullRemoveRangeByRank(m, a, b) != want) {
                    ERR("step %d: rank range [%" PRId64 ", %" PRId64 "]", step,
                        a, b);
                }
            } else {
                const int64_t lo = (int64_t)(splitmix64(&rng) % MOSF_N);
                const int64_t hi = lo + (int64_t)(splitmix64(&rng) % 600);
                const mosRangeSpec range = {.min = databoxNewSigned(lo),
                                            .max = databoxNewSigned(hi)};
                size_t want = 0;
                for (int64_t s = lo; s <= hi && s < MOSF_N; s++) {
                    want += present[s];
                }
                if (multiOrderedSetFullCountByScore(m, &range) != want) {
                    ERR("step %d: count by score [%" PRId64 ", %" PRId64 "]",
                        step, lo, hi);
                }
                for (int64_t s = lo; s <= hi && s < MOSF_N; s++) {
                    present[s] = false;
                }
                if (multiOrderedSetFullRemoveRangeByScore(m, &range) != want) {
                    ERR("step %d: score range [%" PRId64 ", %" PRId64 "]", step,
                        lo, hi);
                }
            }
            err += mosfCheck_(m, present, step, &rng);
        }
        multiOrderedSetFullFree(m);
    }

    TEST("MOS-Full: invariant oracle holds (ascending insert)") {
        multiOrderedSetFull *m = multiOrderedSetFullNew();
        for (int i = 0; i < 1000; i++) {
            databox score = databoxNewSigned(i);
            char buf[24];
            int n = snprintf(buf, sizeof(buf), "m%06d", i);
            databox member = databoxNewBytesAllowEmbed(buf, (size_t)n);
            multiOrderedSetFullAdd(m, &score, &member);
        }
        if (mosFullCheckInvariants(m) != 0) {
            ERRR("invariant oracle reported violations (ascending)");
        }
        if (multiOrderedSetFullCount(m) != 1000) {
            ERR("count expected 1000, got %zu", multiOrderedSetFullCount(m));
        }
        multiOrderedSetFullFree(m);
    }

    TEST("MOS-Full: invariant oracle holds (random order + duplicate scores)") {
        multiOrderedSetFull *m = multiOrderedSetFullNew();
        /* Pseudo-random scrambled order with many equal scores (score = i%50),
         * which is exactly the equal-score-run case the split gating
         * must handle. */
        uint64_t s = 0x9e3779b97f4a7c15ULL;
        for (int i = 0; i < 1000; i++) {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            int key = (int)(s >> 40) % 4096;
            databox score = databoxNewSigned(key % 50);
            char buf[24];
            int n = snprintf(buf, sizeof(buf), "k%08d", key);
            databox member = databoxNewBytesAllowEmbed(buf, (size_t)n);
            multiOrderedSetFullAdd(m, &score, &member);
        }
        if (mosFullCheckInvariants(m) != 0) {
            ERRR("invariant oracle reported violations (random/dup scores)");
        }
        multiOrderedSetFullFree(m);
    }

    TEST("MOS-Full: forced splits keep invariants + lookups (boundary split)") {
        multiOrderedSetFull *m = multiOrderedSetFullNew();
        m->maxMapSize = 256; /* force many splits */

        /* 600 ascending-score entries with a 20-wide equal-score run (score
         * 250 repeated) to exercise the boundary-snap gating. */
        const int N = 600;
        for (int i = 0; i < N; i++) {
            int sc = (i >= 300 && i < 320) ? 250 : i;
            databox score = databoxNewSigned(sc);
            char buf[24];
            int n = snprintf(buf, sizeof(buf), "m%06d", i);
            databox member = databoxNewBytesAllowEmbed(buf, (size_t)n);
            multiOrderedSetFullAdd(m, &score, &member);
        }
        if (m->mapCount <= 1) {
            ERR("expected splits, mapCount=%u", m->mapCount);
        }
        if (mosFullCheckInvariants(m) != 0) {
            ERRR("invariant oracle reported violations after splits");
        }
        if (multiOrderedSetFullCount(m) != (size_t)N) {
            ERR("count expected %d got %zu", N, multiOrderedSetFullCount(m));
        }
        for (int i = 0; i < N; i++) {
            char buf[24];
            int n = snprintf(buf, sizeof(buf), "m%06d", i);
            databox member = databoxNewBytesAllowEmbed(buf, (size_t)n);
            databox got;
            int want = (i >= 300 && i < 320) ? 250 : i;
            if (!multiOrderedSetFullGetScore(m, &member, &got)) {
                ERR("GetScore missing m%06d after split", i);
            } else if (got.data.i != want) {
                ERR("GetScore m%06d expected %d got %" PRId64, i, want,
                    (int64_t)got.data.i);
            }
        }
        int64_t prevScore = INT64_MIN;
        for (int r = 0; r < N; r++) {
            databox member;
            databox score;
            if (!multiOrderedSetFullGetByRank(m, r, &member, &score)) {
                ERR("GetByRank failed at rank %d", r);
                continue;
            }
            if (score.data.i < prevScore) {
                ERR("GetByRank not ordered at rank %d", r);
            }
            prevScore = score.data.i;
        }
        multiOrderedSetFullFree(m);
    }

    TEST("MOS-Full: score-update churn empties middle sub-maps (reclaim)") {
        multiOrderedSetFull *m = multiOrderedSetFullNew();
        m->maxMapSize = 256; /* force splits so middles can empty */
        const int N = 1000;

        for (int i = 0; i < N; i++) {
            databox score = databoxNewSigned(i);
            char buf[24];
            int n = snprintf(buf, sizeof(buf), "m%06d", i);
            databox member = databoxNewBytesAllowEmbed(buf, (size_t)n);
            multiOrderedSetFullAdd(m, &score, &member);
        }

        /* Push every member's score upward each round; this drains low
         * sub-maps into high ones, emptying middles — the exact pattern that
         * exposed the rangeBox-sentinel bug. */
        for (int round = 1; round <= 10; round++) {
            for (int i = 0; i < N; i++) {
                databox score =
                    databoxNewSigned((int64_t)i + (int64_t)round * 1000);
                char buf[24];
                int n = snprintf(buf, sizeof(buf), "m%06d", i);
                databox member = databoxNewBytesAllowEmbed(buf, (size_t)n);
                multiOrderedSetFullAdd(m, &score, &member);
            }
            if (multiOrderedSetFullCount(m) != (size_t)N) {
                ERR("round %d: count drifted to %zu (expected %d)", round,
                    multiOrderedSetFullCount(m), N);
            }
            if (mosFullCheckInvariants(m) != 0) {
                ERR("round %d: invariant oracle violations", round);
            }
            /* No empty sub-map may survive in the interior (reclaim check). */
            for (uint32_t s = 0; s < m->mapCount; s++) {
                flex *sm = getSubMap(m, s);
                if ((sm ? subMapCount(sm) : 0) == 0 && s != m->mapCount - 1) {
                    ERR("round %d: empty interior sub-map %u (reclaim failed)",
                        round, s);
                }
            }
        }
        multiOrderedSetFullFree(m);
    }

    TEST("MOS-Full: delete-to-empty collapses to one sub-map, re-add works") {
        multiOrderedSetFull *m = multiOrderedSetFullNew();
        m->maxMapSize = 256;
        const int N = 1000;
        char buf[24];

        for (int i = 0; i < N; i++) {
            databox score = databoxNewSigned(i);
            int n = snprintf(buf, sizeof(buf), "d%06d", i);
            databox member = databoxNewBytesAllowEmbed(buf, (size_t)n);
            multiOrderedSetFullAdd(m, &score, &member);
        }
        for (int i = 0; i < N; i++) {
            int n = snprintf(buf, sizeof(buf), "d%06d", i);
            databox member = databoxNewBytesAllowEmbed(buf, (size_t)n);
            if (!multiOrderedSetFullRemove(m, &member)) {
                ERR("remove of d%06d failed", i);
            }
            if (mosFullCheckInvariants(m) != 0) {
                ERR("invariants violated mid-delete at i=%d", i);
            }
        }
        if (multiOrderedSetFullCount(m) != 0) {
            ERR("after full delete count expected 0, got %zu",
                multiOrderedSetFullCount(m));
        }
        if (m->mapCount != 1) {
            ERR("after full delete mapCount expected 1, got %u", m->mapCount);
        }
        for (int i = 0; i < 100; i++) {
            databox score = databoxNewSigned(i);
            int n = snprintf(buf, sizeof(buf), "r%06d", i);
            databox member = databoxNewBytesAllowEmbed(buf, (size_t)n);
            multiOrderedSetFullAdd(m, &score, &member);
        }
        if (multiOrderedSetFullCount(m) != 100) {
            ERR("after re-add count expected 100, got %zu",
                multiOrderedSetFullCount(m));
        }
        if (mosFullCheckInvariants(m) != 0) {
            ERRR("invariants violated after re-add");
        }
        multiOrderedSetFullFree(m);
    }

    TEST("MOS-Full: 96/128-bit sub-map heads route, split, copy and remove") {
        /* Score of member i: +-(2^70 + i * 2^40), sign alternating, so
         * every score is wide and sub-map heads are wide on both sides */
        multiOrderedSetFull *m = multiOrderedSetFullNew();
        m->maxMapSize = 256;
        const int N = 800;
        const __int128_t base = (__int128_t)1 << 70;
        const __int128_t step = (__int128_t)1 << 40;

        for (int i = 0; i < N; i++) {
            const __int128_t v = base + (__int128_t)i * step;
            databoxBig score;
            memset(&score, 0, sizeof(score));
            DATABOX_BIG_SIGNED_128(&score, (i & 1) ? -v : v);
            const databox member = databoxNewSigned(i);
            multiOrderedSetFullAdd(m, (databox *)&score, &member);
        }

        if (m->mapCount <= 1) {
            ERR("expected splits, mapCount=%u", m->mapCount);
        }
        if (mosFullCheckInvariants(m) != 0) {
            ERRR("invariants violated with 96/128-bit heads");
        }

        /* A positive wide value reads back UNSIGNED_128 (canonical form) */
        size_t wideHeads = 0;
        for (uint32_t s = 0; s < m->mapCount; s++) {
            const databox *head = getRangeBox(m, s);
            if (head && mosBoxIsWide(head) && head->big && head->allocated &&
                head->data.ptr) {
                wideHeads++;
            }
        }
        if (wideHeads != m->mapCount) {
            ERR("%zu of %u sub-map heads read back wide", wideHeads,
                m->mapCount);
        }

        multiOrderedSetFull *copy = multiOrderedSetFullCopy(m);
        if (copy->mapCount != m->mapCount) {
            ERR("copy has %u sub-maps, original %u", copy->mapCount,
                m->mapCount);
        }
        for (uint32_t s = 0; s < m->mapCount && s < copy->mapCount; s++) {
            if (getRangeBox(copy, s)->data.ptr == getRangeBox(m, s)->data.ptr) {
                ERR("copy shares the rangeBox payload of sub-map %u", s);
            }
        }
        const multiOrderedSetFull *sets[2] = {m, copy};
        for (int which = 0; which < 2; which++) {
            if (mosFullCheckInvariants(sets[which]) != 0) {
                ERR("set %d invariants violated", which);
            }

            for (int i = 0; i < N; i++) {
                const __int128_t v = base + (__int128_t)i * step;
                const databox member = databoxNewSigned(i);
                databoxBig got;
                DATABOX_BIG_INIT(&got);
                const databoxType wantType =
                    (i & 1) ? DATABOX_SIGNED_128 : DATABOX_UNSIGNED_128;
                if (!multiOrderedSetFullGetScore(sets[which], &member,
                                                 (databox *)&got) ||
                    got.type != wantType ||
                    *got.data.i128 != ((i & 1) ? -v : v)) {
                    ERR("set %d: GetScore wrong for member %d", which, i);
                }
            }
        }
        multiOrderedSetFullFree(copy);

        /* Remove the scores in [-(base + 200 step), base + 200 step), a
         * range that spans the sign change and several sub-maps */
        databoxBig lo;
        databoxBig hi;
        memset(&lo, 0, sizeof(lo));
        memset(&hi, 0, sizeof(hi));
        DATABOX_BIG_SIGNED_128(&lo, -(base + 200 * step));
        DATABOX_BIG_SIGNED_128(&hi, base + 200 * step);
        const mosRangeSpec range = {.min = *(databox *)&lo,
                                    .max = *(databox *)&hi,
                                    .minExclusive = false,
                                    .maxExclusive = true};
        const size_t removed = multiOrderedSetFullRemoveRangeByScore(m, &range);
        size_t want = 0;
        for (int i = 0; i < N; i++) {
            const __int128_t v = base + (__int128_t)i * step;
            const __int128_t s = (i & 1) ? -v : v;
            if (s >= -(base + 200 * step) && s < base + 200 * step) {
                want++;
            }
        }
        if (removed != want || multiOrderedSetFullCount(m) != N - want) {
            ERR("range removal removed %zu (expected %zu), count %zu", removed,
                want, multiOrderedSetFullCount(m));
        }
        if (mosFullCheckInvariants(m) != 0) {
            ERRR("invariants violated after wide range removal");
        }
        multiOrderedSetFullFree(m);
    }

    TEST("MOS-Full: rangeBox payload is reused in place and freed on narrow") {
        multiOrderedSetFull *m = multiOrderedSetFullNew();
        const __int128_t w1 = -((__int128_t)1 << 70);
        const __int128_t w2 = -((__int128_t)1 << 71);

        const databox narrowScore = databoxNewSigned(10);
        const databox m1 = databoxNewSigned(1);
        multiOrderedSetFullAdd(m, &narrowScore, &m1);
        const databox *rb = getRangeBox(m, 0);
        if (mosBoxIsWide(rb) || rb->allocated || rb->big) {
            ERRR("narrow head should own nothing");
        }

        /* narrow -> wide: the head gets a payload */
        databoxBig s2;
        memset(&s2, 0, sizeof(s2));
        DATABOX_BIG_SIGNED_128(&s2, w1);
        const databox m2 = databoxNewSigned(2);
        multiOrderedSetFullAdd(m, (const databox *)&s2, &m2);
        rb = getRangeBox(m, 0);
        const void *payload = rb->data.ptr;
        if (rb->type != DATABOX_SIGNED_128 || !rb->allocated || !rb->big ||
            !payload || *rb->data.i128 != w1) {
            ERRR("wide head did not get an owned payload");
        }

        /* wide -> wide: the same payload is rewritten in place */
        databoxBig s3;
        memset(&s3, 0, sizeof(s3));
        DATABOX_BIG_SIGNED_128(&s3, w2);
        const databox m3 = databoxNewSigned(3);
        multiOrderedSetFullAdd(m, (const databox *)&s3, &m3);
        rb = getRangeBox(m, 0);
        if (rb->data.ptr != payload || *rb->data.i128 != w2) {
            ERRR("wide head refresh did not reuse its payload");
        }

        multiOrderedSetFullRemove(m, &m3);
        rb = getRangeBox(m, 0);
        if (rb->data.ptr != payload || *rb->data.i128 != w1) {
            ERRR("wide head after removal did not reuse its payload");
        }

        /* wide -> narrow: the payload is freed (the leak checker sees it
         * otherwise) and the flags are cleared */
        multiOrderedSetFullRemove(m, &m2);
        rb = getRangeBox(m, 0);
        if (mosBoxIsWide(rb) || rb->allocated || rb->big ||
            rb->data.i64 != 10) {
            ERRR("narrow head after a wide one kept a payload");
        }

        /* wide again, then drop it through the empty-map sentinel and Free */
        multiOrderedSetFullAdd(m, (const databox *)&s2, &m2);
        multiOrderedSetFullRemove(m, &m1);
        if (mosFullCheckInvariants(m) != 0) {
            ERRR("invariants violated across head transitions");
        }
        multiOrderedSetFullRemove(m, &m2);
        rb = getRangeBox(m, 0);
        if (rb->type != DATABOX_SIGNED_64 || rb->data.i64 != INT64_MAX ||
            rb->allocated) {
            ERRR("empty sub-map should hold the narrow sentinel");
        }
        multiOrderedSetFullAdd(m, (const databox *)&s3, &m3);
        multiOrderedSetFullReset(m);
        multiOrderedSetFullAdd(m, (const databox *)&s3, &m3);
        multiOrderedSetFullFree(m);
    }

    TEST("MOS-Full: mixed narrow and wide heads across splits and reclaim") {
        /* Scores alternate narrow and wide in sorted runs, so sub-map heads
         * flip between the two as pairs are added, popped and reclaimed */
        multiOrderedSetFull *m = multiOrderedSetFullNew();
        m->maxMapSize = 192;
        const int N = 600;
        for (int i = 0; i < N; i++) {
            databoxBig score;
            memset(&score, 0, sizeof(score));
            if ((i / 7) & 1) {
                DATABOX_BIG_SIGNED_128(&score, ((__int128_t)i << 66) * -1);
            } else {
                *(databox *)&score = databoxNewSigned((int64_t)i * 1000);
            }
            const databox member = databoxNewSigned(i);
            multiOrderedSetFullAdd(m, (const databox *)&score, &member);
        }
        if (m->mapCount <= 1 || mosFullCheckInvariants(m) != 0) {
            ERR("mixed build: mapCount %u or invariants wrong", m->mapCount);
        }

        size_t left = (size_t)N;
        for (int round = 0; round < 6 && left > 0; round++) {
            databox members[40];
            databox scores[40];
            memset(members, 0xFF, sizeof(members));
            memset(scores, 0xFF, sizeof(scores));
            const size_t popped =
                (round & 1) ? multiOrderedSetFullPopMax(m, 40, members, scores)
                            : multiOrderedSetFullPopMin(m, 40, members, scores);
            for (size_t i = 0; i < popped; i++) {
                if (mosBoxIsWide(&scores[i]) &&
                    !(scores[i].allocated && scores[i].big &&
                      scores[i].data.ptr)) {
                    ERR("popped wide score %zu is not heap-owned", i);
                }
                databoxFreeData(&members[i]);
                databoxFreeData(&scores[i]);
            }
            left -= popped;
            if (multiOrderedSetFullCount(m) != left ||
                mosFullCheckInvariants(m) != 0) {
                ERR("round %d: count %zu (want %zu) or invariants wrong", round,
                    multiOrderedSetFullCount(m), left);
            }
        }

        /* RandomMembers ignores poisoned destinations: wide values come
         * back deferred, narrow ones in full */
        databox members[16];
        databox scores[16];
        memset(members, 0xFF, sizeof(members));
        memset(scores, 0xFF, sizeof(scores));
        const size_t got =
            multiOrderedSetFullRandomMembers(m, -16, members, scores);
        for (size_t i = 0; i < got; i++) {
            if (mosBoxIsWide(&scores[i])
                    ? !databoxIsWideDeferred(&scores[i])
                    : (scores[i].big || scores[i].allocated)) {
                ERR("random member %zu: score not deferred or flags stale", i);
            }
        }
        multiOrderedSetFullFree(m);
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
