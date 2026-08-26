#include "multiOrderedSetFull.h"
#include "multiOrderedSetFullInternal.h"
#include "multiOrderedSetMediumInternal.h"
/* atomPool.h already included via multiOrderedSetFullInternal.h */

#include "str.h" /* for random functions */

/* ====================================================================
 * Random Number Generator
 * ==================================================================== */
static uint64_t mosRandomState = 0xABCDEF0123456789ULL;

static inline uint64_t mosRandom(void) {
    return xorshift64star(&mosRandomState);
}

/* ====================================================================
 * Databox Arithmetic Helper
 * ==================================================================== */

/* Add delta to base, store result in out. Returns true on success. */
static bool mosDataboxAdd(const databox *base, const databox *delta,
                          databox *out) {
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

/* Convert pool ID back to member (for iteration/retrieval) */
static inline bool poolIdToMember(const multiOrderedSetFull *m,
                                  const databox *idBox, databox *member) {
    if (m->pool && idBox->type == DATABOX_UNSIGNED_64) {
        return atomPoolLookup(m->pool, idBox->data.u64, member);
    }
    /* No pool - idBox IS the member */
    *member = *idBox;
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

/* Get rangeBox pointer by index */
static inline databox *getRangeBox(const multiOrderedSetFull *m, uint32_t idx) {
    databox *box;
    multiarrayNativeGet(m->rangeBox, databox, box, idx);
    return box;
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
 * order) and member lookups (findEntryByMember scans only the returned
 * sub-map). Masked while mapCount==1; sub-map splitting makes it mandatory.
 * Relies on rangeBox being monotonic, which empty-sub-map reclaim guarantees
 * (no interior INT64_MAX sentinels). */
static int32_t findSubMapForScore(const multiOrderedSetFull *m,
                                  const databox *score) {
    int32_t left = 0;
    int32_t right = (int32_t)m->mapCount - 1;

    while (left < right) {
        const int32_t mid = (left + right + 1) / 2; /* upper mid: bias right */
        databox *rangeScorePtr;
        multiarrayNativeGet(m->rangeBox, databox, rangeScorePtr, mid);

        if (rangeScorePtr && databoxCompare(rangeScorePtr, score) <= 0) {
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

    /* Snap the cut to a score boundary at/around the geometric middle. k is a
     * PAIR index in [1, pairs-1] with score[k] != score[k-1]. */
    const size_t mid = pairs / 2;
    size_t k = 0;
    databox prevScore;
    databox curScore;

    for (size_t i = (mid < 1 ? 1 : mid); i < pairs; i++) {
        flexGetByType(
            flexIndex(map, (int32_t)((i - 1) * MOS_ELEMENTS_PER_ENTRY)),
            &prevScore);
        flexGetByType(flexIndex(map, (int32_t)(i * MOS_ELEMENTS_PER_ENTRY)),
                      &curScore);
        if (databoxCompare(&curScore, &prevScore) != 0) {
            k = i;
            break;
        }
    }

    if (k == 0) {
        for (size_t i = (mid < 1 ? 1 : mid); i >= 1; i--) {
            flexGetByType(
                flexIndex(map, (int32_t)((i - 1) * MOS_ELEMENTS_PER_ENTRY)),
                &prevScore);
            flexGetByType(flexIndex(map, (int32_t)(i * MOS_ELEMENTS_PER_ENTRY)),
                          &curScore);
            if (databoxCompare(&curScore, &prevScore) != 0) {
                k = i;
                break;
            }
            if (i == 1) {
                break; /* avoid size_t underflow */
            }
        }
    }

    if (k == 0) {
        return; /* one giant equal-score run: cannot split safely */
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
    databox upperRange;
    flexGetByType(flexHead(upper), &upperRange);

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
            flexGetByType(head, rangeScore);
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
            flexGetByType(memberEntry, &memberOrId);
            releasePoolMember(m, &memberOrId);
        }
    }

    flexDeleteCount(&map, &entry, MOS_ELEMENTS_PER_ENTRY);
    setSubMap(m, mapIdx, map);
    updateMiddle(m, mapIdx);
    m->totalEntries--;

    /* Update rangeBox - check if map has entries before reading head.
     * flexHead() returns non-NULL even for empty flex (points past header),
     * so we must check flexCount() to avoid reading invalid memory. */
    databox *rangeScore = getRangeBox(m, mapIdx);
    if (rangeScore) {
        if (flexCount(map) > 0) {
            flexEntry *head = flexHead(map);
            flexGetByType(head, rangeScore);
        } else {
            /* Map is empty, set to max value */
            rangeScore->type = DATABOX_SIGNED_64;
            rangeScore->data.i = INT64_MAX;
        }
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

/* Find entry in score maps by member (using member index for score lookup) */
static flexEntry *findEntryByMember(const multiOrderedSetFull *m,
                                    const databox *member, uint32_t *mapIdx) {
    /* First, get the score from member index */
    databox score;
    if (!multidictFind((multidict *)m->memberIndex, member, &score)) {
        return NULL;
    }

    /* Find the sub-map containing this score */
    *mapIdx = findSubMapForScore(m, &score);
    flex *map = getSubMap(m, *mapIdx);
    if (!map) {
        return NULL;
    }

    /* In pool mode, get the pool ID for comparison */
    databox memberToMatch;
    if (m->pool) {
        uint64_t id = atomPoolGetId(m->pool, member);
        if (id == 0) {
            return NULL; /* Member not in pool */
        }
        memberToMatch = databoxNewUnsigned(id);
    } else {
        memberToMatch = *member;
    }

    /* Search for the (score, member/memberID) entry */
    flexEntry *entry = flexHead(map);
    while (entry) {
        flexEntry *memberEntry = flexNext(map, entry);
        if (!memberEntry) {
            break;
        }

        databox currentScore, currentMemberOrId;
        flexGetByType(entry, &currentScore);
        flexGetByType(memberEntry, &currentMemberOrId);

        if (databoxCompare(&currentScore, &score) == 0 &&
            databoxCompare(&currentMemberOrId, &memberToMatch) == 0) {
            return entry;
        }

        /* If we've passed the score, it's not in this map */
        if (databoxCompare(&currentScore, &score) > 0) {
            break;
        }

        entry = flexNext(map, memberEntry);
    }

    return NULL;
}

/* ====================================================================
 * Creation / Destruction
 * ==================================================================== */

multiOrderedSetFull *multiOrderedSetFullNew(void) {
    multiOrderedSetFull *m = zcalloc(1, sizeof(*m));

    /* Create member index (member → score) */
    m->mdClass = multidictDefaultClassNew();
    m->memberIndex = multidictNew(&multidictTypeExactKey, m->mdClass, 0);

    /* Create initial sub-map */
    m->scoreMap = multiarrayNativeNew(flex *);
    m->middle = multiarrayNativeNew(uint32_t);
    m->rangeBox = multiarrayNativeNew(databox);

    flex *initialMap = flexNew();
    uint32_t initialMiddle = FLEX_EMPTY_SIZE;
    databox initialRange = databoxNewSigned(INT64_MAX);

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
    m->pool = atomPoolNewDefault(); /* Uses HASH backend for speed */
    m->flags |= MOS_FLAG_POOL_OWNED;
    return m;
}

multiOrderedSetFull *multiOrderedSetFullNewWithPoolType(atomPoolType type) {
    multiOrderedSetFull *m = multiOrderedSetFullNew();
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

            databox score, member;
            flexGetByType(entry, &score);
            flexGetByType(memberEntry, &member);

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
    multiOrderedSetFull *copy = zcalloc(1, sizeof(*copy));

    /* Create own class for the copy (classes shouldn't be shared for ownership)
     */
    copy->mdClass = multidictDefaultClassNew();
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

    uint32_t copyCount = 0;
    for (uint32_t i = 0; i < m->mapCount; i++) {
        flex *origMap = getSubMap(m, i);
        flex *mapCopy = origMap ? flexDuplicate(origMap) : flexNew();
        uint32_t mid = getMiddle(m, i);
        databox *range = getRangeBox(m, i);
        databox rangeCopy = range ? *range : (databox){0};

        multiarrayNativeInsert(copy->scoreMap, flex *, 64, copyCount, i,
                               &mapCopy);
        multiarrayNativeInsert(copy->middle, uint32_t, 64, copyCount, i, &mid);
        multiarrayNativeInsert(copy->rangeBox, databox, 64, copyCount, i,
                               &rangeCopy);
    }

    copy->mapCount = m->mapCount;
    copy->totalEntries = m->totalEntries;
    copy->maxMapSize = m->maxMapSize;

    /* Handle pool mode for copy */
    if (m->pool) {
        if (m->flags & MOS_FLAG_POOL_OWNED) {
            /* Source owns pool - copy creates its own pool and re-interns */
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
                    flexGetByType(memberEntry, &oldId);
                    databox member;
                    if (atomPoolLookup(m->pool, oldId.data.u64, &member)) {
                        /* Intern in new pool and update entry */
                        uint64_t newId = atomPoolIntern(copy->pool, &member);
                        databox newIdBox = databoxNewUnsigned(newId);
                        flexReplaceByType(&map, memberEntry, &newIdBox);
                        setSubMap(copy, i, map);
                    }
                    ent = flexNext(map, memberEntry);
                }
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
                    flexGetByType(memberEntry, &idBox);
                    if (idBox.type == DATABOX_UNSIGNED_64) {
                        /* Re-intern to increment refcount */
                        databox member;
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
                    flexGetByType(memberEntry, &memberOrId);
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
                    flexGetByType(memberEntry, &memberOrId);
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
    multiarrayNativeFree(m->scoreMap);
    multiarrayNativeFree(m->middle);
    multiarrayNativeFree(m->rangeBox);

    m->scoreMap = multiarrayNativeNew(flex *);
    m->middle = multiarrayNativeNew(uint32_t);
    m->rangeBox = multiarrayNativeNew(databox);

    flex *initialMap = flexNew();
    uint32_t initialMiddle = FLEX_EMPTY_SIZE;
    databox initialRange = databoxNewSigned(INT64_MAX);

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

bool multiOrderedSetFullAdd(multiOrderedSetFull *m, const databox *score,
                            const databox *member) {
    databox existingScore;
    bool existed = multidictFind(m->memberIndex, member, &existingScore);

    if (existed) {
        /* Remove old entry from score maps */
        uint32_t mapIdx;
        flexEntry *entry = findEntryByMember(m, member, &mapIdx);
        if (entry) {
            removeFromSubMap(m, mapIdx, entry);
            reclaimEmptySubMap(m, mapIdx);
        }
    }

    /* Add to member index (multidictAdd does upsert - add or replace) */
    multidictResult res = multidictAdd(m->memberIndex, member, score);
    if (res == MULTIDICT_ERR) {
        /* Failed to add to member index - don't add to scoreMap either */
        return existed;
    }

    /* Add to score map */
    uint32_t mapIdx = findSubMapForScore(m, score);
    insertIntoSubMap(m, mapIdx, score, member);

    return existed;
}

bool multiOrderedSetFullAddNX(multiOrderedSetFull *m, const databox *score,
                              const databox *member) {
    if (multidictExists(m->memberIndex, member)) {
        return false;
    }

    multidictAdd(m->memberIndex, member, score);
    uint32_t mapIdx = findSubMapForScore(m, score);
    insertIntoSubMap(m, mapIdx, score, member);

    return true;
}

bool multiOrderedSetFullAddXX(multiOrderedSetFull *m, const databox *score,
                              const databox *member) {
    databox existingScore;
    if (!multidictFind(m->memberIndex, member, &existingScore)) {
        return false;
    }

    /* Remove old entry */
    uint32_t mapIdx;
    flexEntry *entry = findEntryByMember(m, member, &mapIdx);
    if (entry) {
        removeFromSubMap(m, mapIdx, entry);
        reclaimEmptySubMap(m, mapIdx);
    }

    /* Update with new score (entry must exist since XX checked) */
    multidictAdd(m->memberIndex, member, score);
    mapIdx = findSubMapForScore(m, score);
    insertIntoSubMap(m, mapIdx, score, member);

    return true;
}

bool multiOrderedSetFullAddGetPrevious(multiOrderedSetFull *m,
                                       const databox *score,
                                       const databox *member,
                                       databox *prevScore) {
    bool existed = multidictFind(m->memberIndex, member, prevScore);

    if (existed) {
        uint32_t mapIdx;
        flexEntry *entry = findEntryByMember(m, member, &mapIdx);
        if (entry) {
            removeFromSubMap(m, mapIdx, entry);
            reclaimEmptySubMap(m, mapIdx);
        }
    }

    /* multidictAdd does upsert - add or replace */
    multidictAdd(m->memberIndex, member, score);
    uint32_t mapIdx = findSubMapForScore(m, score);
    insertIntoSubMap(m, mapIdx, score, member);

    return existed;
}

bool multiOrderedSetFullIncrBy(multiOrderedSetFull *m, const databox *delta,
                               const databox *member, databox *result) {
    databox existingScore;
    bool existed = multidictFind(m->memberIndex, member, &existingScore);

    if (existed) {
        if (!mosDataboxAdd(&existingScore, delta, result)) {
            return false;
        }

        uint32_t mapIdx;
        flexEntry *entry = findEntryByMember(m, member, &mapIdx);
        if (entry) {
            removeFromSubMap(m, mapIdx, entry);
            reclaimEmptySubMap(m, mapIdx);
        }
    } else {
        *result = *delta;
    }

    /* multidictAdd does upsert - add or replace */
    multidictAdd(m->memberIndex, member, result);
    uint32_t mapIdx = findSubMapForScore(m, result);
    insertIntoSubMap(m, mapIdx, result, member);

    return true;
}

/* ====================================================================
 * Deletion
 * ==================================================================== */

bool multiOrderedSetFullRemove(multiOrderedSetFull *m, const databox *member) {
    databox score;
    if (!multidictFind(m->memberIndex, member, &score)) {
        return false;
    }

    uint32_t mapIdx;
    flexEntry *entry = findEntryByMember(m, member, &mapIdx);
    if (entry) {
        removeFromSubMap(m, mapIdx, entry);
        reclaimEmptySubMap(m, mapIdx);
    }

    multidictDelete(m->memberIndex, member);
    return true;
}

bool multiOrderedSetFullRemoveGetScore(multiOrderedSetFull *m,
                                       const databox *member, databox *score) {
    if (!multidictFind(m->memberIndex, member, score)) {
        return false;
    }

    uint32_t mapIdx;
    flexEntry *entry = findEntryByMember(m, member, &mapIdx);
    if (entry) {
        removeFromSubMap(m, mapIdx, entry);
        reclaimEmptySubMap(m, mapIdx);
    }

    multidictDelete(m->memberIndex, member);
    return true;
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

            databox score, memberOrId;
            flexGetByType(entry, &score);
            flexGetByType(memberEntry, &memberOrId);

            if (mosScoreInRange(&score, &range->min, range->minExclusive,
                                &range->max, range->maxExclusive)) {
                flexEntry *nextEntry = flexNext(map, memberEntry);

                /* In pool mode, convert pool ID to member for memberIndex */
                databox member;
                if (!poolIdToMember(m, &memberOrId, &member)) {
                    entry = nextEntry;
                    continue;
                }

                /* Remove from member index */
                multidictDelete(m->memberIndex, &member);

                /* Remove from score map */
                removeFromSubMap(m, mapIdx, entry);
                removed++;

                entry = nextEntry;
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
    start = mosNormalizeRank(start, count);
    stop = mosNormalizeRank(stop, count);

    if (start < 0 || stop < 0 || start > stop) {
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
                databox memberOrId;
                flexGetByType(memberEntry, &memberOrId);

                /* In pool mode, convert pool ID to member for memberIndex */
                databox member;
                if (!poolIdToMember(m, &memberOrId, &member)) {
                    currentRank++;
                    entry = flexNext(map, memberEntry);
                    continue;
                }

                flexEntry *nextEntry = flexNext(map, memberEntry);

                multidictDelete(m->memberIndex, &member);
                removeFromSubMap(m, mapIdx, entry);
                removed++;

                entry = nextEntry;
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

            flexGetByType(head, &scores[popped]);

            /* In pool mode, convert pool ID back to member string */
            databox memberOrId;
            flexGetByType(memberEntry, &memberOrId);
            if (!poolIdToMember(m, &memberOrId, &members[popped])) {
                break;
            }

            multidictDelete(m->memberIndex, &members[popped]);
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

            flexGetByType(entry, &scores[popped]);

            /* In pool mode, convert pool ID back to member string */
            databox memberOrId;
            flexGetByType(memberEntry, &memberOrId);
            if (!poolIdToMember(m, &memberOrId, &members[popped])) {
                break;
            }

            multidictDelete(m->memberIndex, &members[popped]);
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
    databox score;
    if (!multidictFind((multidict *)m->memberIndex, member, &score)) {
        return -1;
    }

    /* In pool mode, get the member's pool ID for comparison */
    databox memberToMatch;
    if (m->pool) {
        uint64_t id = atomPoolGetId(m->pool, member);
        if (id == 0) {
            return -1; /* Member not in pool */
        }
        memberToMatch = databoxNewUnsigned(id);
    } else {
        memberToMatch = *member;
    }

    int64_t rank = 0;

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

            databox currentScore, currentMemberOrId;
            flexGetByType(entry, &currentScore);
            flexGetByType(memberEntry, &currentMemberOrId);

            if (databoxCompare(&currentMemberOrId, &memberToMatch) == 0) {
                return rank;
            }

            rank++;
            entry = flexNext(map, memberEntry);
        }
    }

    return -1;
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
        databox memberOrId;
        flexGetByType(memberEntry, &memberOrId);
        return poolIdToMember(m, &memberOrId, member);
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
            flexGetByType(entry, &score);

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
            flexGetByType(entry, &currentScore);

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
    databox memberOrId;
    flexGetByType(memberEntry, &memberOrId);
    if (!poolIdToMember(m, &memberOrId, member)) {
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
        size_t currentOffset = entry - map;
        if (currentOffset >= MOS_ELEMENTS_PER_ENTRY * 2) {
            iter->current =
                flexIndex(map, currentOffset - MOS_ELEMENTS_PER_ENTRY);
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
                databox memberOrId;
                flexGetByType(memberEntry, &memberOrId);
                return poolIdToMember(m, &memberOrId, member);
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
                    databox memberOrId;
                    flexGetByType(memberEntry, &memberOrId);
                    return poolIdToMember(m, &memberOrId, member);
                }
            }
        }
    }
    return false;
}

/* ====================================================================
 * Random
 * ==================================================================== */

size_t multiOrderedSetFullRandomMembers(const multiOrderedSetFull *m,
                                        int64_t count, databox *members,
                                        databox *scores) {
    if (m->totalEntries == 0) {
        return 0;
    }

    bool allowDuplicates = (count < 0);
    size_t absCount = (size_t)(count < 0 ? -count : count);
    size_t retrieved = 0;

    if (absCount > m->totalEntries && !allowDuplicates) {
        absCount = m->totalEntries;
    }

    for (size_t i = 0; i < absCount; i++) {
        size_t idx = mosRandom() % m->totalEntries;
        if (multiOrderedSetFullGetByRank(m, (int64_t)idx, &members[retrieved],
                                         &scores[retrieved])) {
            retrieved++;
        }
    }

    return retrieved;
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

                databox score, member;
                flexGetByType(entry, &score);
                flexGetByType(memberEntry, &member);

                printf("    [%d] ", idx);
                databoxReprSay("score", &score);
                printf(" ");
                databoxReprSay("member", &member);
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
    databox prevLast = {0};
    bool havePrev = false;
    for (uint32_t i = 0; i < m->mapCount; i++) {
        flex *map = getSubMap(m, i);
        const size_t cnt = map ? subMapCount(map) : 0;
        sum += cnt;
        if (cnt == 0) {
            continue; /* empty sub-map holds the INT64_MAX rangeBox sentinel */
        }
        databox headScore;
        flexGetByType(flexHead(map), &headScore);
        const databox *rb = getRangeBox(m, i);
        if (rb && databoxCompare(rb, &headScore) != 0) {
            printf("  INVARIANT FAIL: submap %u rangeBox != head score\n", i);
            bad++;
        }
        if (havePrev && databoxCompare(&prevLast, &headScore) > 0) {
            printf("  INVARIANT FAIL: submap %u breaks global score order\n",
                   i);
            bad++;
        }
        flexGetByType(flexIndex(map, (cnt - 1) * MOS_ELEMENTS_PER_ENTRY),
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
                ERR("member%03d score should be %d, got %ld", i, i * 10,
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
                ERR("Rank %d should have score %d, got %ld", i, i * 10,
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
                        "got %ld",
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
            uint64_t inlineInsertStart = _perfTSC();
            for (size_t i = 0; i < N; i++) {
                databox score = databoxNewUnsigned(i);
                /* Generate string of exact length */
                memset(buf, 'A' + (i % 26), strLen);
                snprintf(buf, strLen + 1, "m%0*zu", (int)(strLen - 2), i);
                buf[strLen] = '\0';
                databox member = databoxNewBytesAllowEmbed(buf, strLen);
                multiOrderedSetFullAdd(mosInline, &score, &member);
            }
            uint64_t inlineInsertEnd = _perfTSC();

            /* Insert - Pool HASH */
            uint64_t hashInsertStart = _perfTSC();
            for (size_t i = 0; i < N; i++) {
                databox score = databoxNewUnsigned(i);
                memset(buf, 'A' + (i % 26), strLen);
                snprintf(buf, strLen + 1, "m%0*zu", (int)(strLen - 2), i);
                buf[strLen] = '\0';
                databox member = databoxNewBytesAllowEmbed(buf, strLen);
                multiOrderedSetFullAdd(mosHash, &score, &member);
            }
            uint64_t hashInsertEnd = _perfTSC();

            /* Insert - Pool TREE */
            uint64_t treeInsertStart = _perfTSC();
            for (size_t i = 0; i < N; i++) {
                databox score = databoxNewUnsigned(i);
                memset(buf, 'A' + (i % 26), strLen);
                snprintf(buf, strLen + 1, "m%0*zu", (int)(strLen - 2), i);
                buf[strLen] = '\0';
                databox member = databoxNewBytesAllowEmbed(buf, strLen);
                multiOrderedSetFullAdd(mosTree, &score, &member);
            }
            uint64_t treeInsertEnd = _perfTSC();

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

            printf("  Insert (cycles/op):\n");
            printf("    Inline:     %.1f\n",
                   (double)(inlineInsertEnd - inlineInsertStart) / N);
            printf("    Pool HASH:  %.1f\n",
                   (double)(hashInsertEnd - hashInsertStart) / N);
            printf("    Pool TREE:  %.1f\n",
                   (double)(treeInsertEnd - treeInsertStart) / N);

            /* Lookup benchmark */
            uint64_t inlineLookupStart = _perfTSC();
            for (size_t i = 0; i < N; i++) {
                memset(buf, 'A' + (i % 26), strLen);
                snprintf(buf, strLen + 1, "m%0*zu", (int)(strLen - 2), i);
                buf[strLen] = '\0';
                databox member = databoxNewBytesAllowEmbed(buf, strLen);
                databox score;
                multiOrderedSetFullGetScore(mosInline, &member, &score);
            }
            uint64_t inlineLookupEnd = _perfTSC();

            uint64_t hashLookupStart = _perfTSC();
            for (size_t i = 0; i < N; i++) {
                memset(buf, 'A' + (i % 26), strLen);
                snprintf(buf, strLen + 1, "m%0*zu", (int)(strLen - 2), i);
                buf[strLen] = '\0';
                databox member = databoxNewBytesAllowEmbed(buf, strLen);
                databox score;
                multiOrderedSetFullGetScore(mosHash, &member, &score);
            }
            uint64_t hashLookupEnd = _perfTSC();

            uint64_t treeLookupStart = _perfTSC();
            for (size_t i = 0; i < N; i++) {
                memset(buf, 'A' + (i % 26), strLen);
                snprintf(buf, strLen + 1, "m%0*zu", (int)(strLen - 2), i);
                buf[strLen] = '\0';
                databox member = databoxNewBytesAllowEmbed(buf, strLen);
                databox score;
                multiOrderedSetFullGetScore(mosTree, &member, &score);
            }
            uint64_t treeLookupEnd = _perfTSC();

            printf("  Lookup (cycles/op):\n");
            printf("    Inline:     %.1f\n",
                   (double)(inlineLookupEnd - inlineLookupStart) / N);
            printf("    Pool HASH:  %.1f\n",
                   (double)(hashLookupEnd - hashLookupStart) / N);
            printf("    Pool TREE:  %.1f\n",
                   (double)(treeLookupEnd - treeLookupStart) / N);

            /* Iteration benchmark */
            mosIterator iter;
            databox m, s;

            uint64_t inlineIterStart = _perfTSC();
            multiOrderedSetFullIteratorInit(mosInline, &iter, true);
            while (multiOrderedSetFullIteratorNext(&iter, &m, &s)) {
            }
            uint64_t inlineIterEnd = _perfTSC();

            uint64_t hashIterStart = _perfTSC();
            multiOrderedSetFullIteratorInit(mosHash, &iter, true);
            while (multiOrderedSetFullIteratorNext(&iter, &m, &s)) {
            }
            uint64_t hashIterEnd = _perfTSC();

            uint64_t treeIterStart = _perfTSC();
            multiOrderedSetFullIteratorInit(mosTree, &iter, true);
            while (multiOrderedSetFullIteratorNext(&iter, &m, &s)) {
            }
            uint64_t treeIterEnd = _perfTSC();

            printf("  Iteration (cycles/op):\n");
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
        printf("│   Speed:   O(1) lookup, ~2 cycles/op iteration              "
               "│\n");
        printf("│   Best for: Read-heavy workloads, iteration-intensive apps  "
               "│\n");
        printf("├─────────────────────────────────────────────────────────────┤"
               "\n");
        printf("│ ATOM_POOL_TREE (multimapAtom):                              "
               "│\n");
        printf("│   Memory:  ~22 bytes/entry overhead (3-4x less than HASH)   "
               "│\n");
        printf("│   Speed:   O(log n) lookup, ~10 cycles/op iteration (5-6x)  "
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

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
