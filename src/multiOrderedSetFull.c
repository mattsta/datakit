#include "multiOrderedSetFull.h"
#include "multiOrderedSetFullInternal.h"
#include "multiOrderedSetMediumInternal.h"

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
        return databoxNewUnsigned(multimapAtomIntern(m->pool, member));
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
        flexGetByTypeInto(memberEntry, &idBox, nullptr);
        if (idBox.type == DATABOX_UNSIGNED_64) {
            if (slot) {
                mosBoxAim(member, slot);
            }
            return multimapAtomLookupById(m->pool, idBox.data.u64, member);
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
        multimapAtomReleaseById(m->pool, idBox->data.u64);
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
    return ptr ? *ptr : nullptr;
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
                               : nullptr;
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

/* Sub-maps hold rows in (score, member or pool id) order, and so do the
 * sub-maps among themselves: an equal-score run may span several.  A
 * sub-map's first row compares with (score, memberOrId) by its rangeBox
 * score, and on a tie by its first member, read from the flex then (so a
 * lookup decodes at most one member per tied step).  With a NULL
 * 'memberOrId' only the score counts.  An empty sub-map (only ever the
 * lone one) sorts after everything. */
static int mosFullHeadCompare_(const multiOrderedSetFull *m, uint32_t i,
                               const databox *score,
                               const databox *memberOrId) {
    const int byScore = databoxCompare(getRangeBox(m, i), score);
    if (byScore != 0 || !memberOrId) {
        return byScore;
    }

    const flex *map = getSubMap(m, i);
    if (!map || flexCount(map) == 0) {
        return 1;
    }

    databox head;
    databoxWide headWide;
    flexGetByTypeInto(flexNext(map, flexHead(map)), &head, &headWide);
    return databoxCompare(&head, memberOrId);
}

/* The LAST sub-map whose first row is at or below the row (score,
 * memberOrId): the sub-map that holds that row, or should.  Below every
 * first row it is sub-map 0, where the row becomes the new first.  With a
 * NULL 'memberOrId' it is the last sub-map whose first score is strictly
 * below 'score' (or 0): where a scan for the first row at 'score' or above
 * starts, since an equal-score run may begin in that sub-map's tail.
 * Binary search over the first rows, which stay strictly ascending because
 * empty sub-maps are reclaimed (no interior INT64_MAX sentinels). */
static uint32_t findSubMap_(const multiOrderedSetFull *m, const databox *score,
                            const databox *memberOrId) {
    int32_t left = 0;
    int32_t right = (int32_t)m->mapCount - 1;
    while (left < right) {
        const int32_t mid = (left + right + 1) / 2; /* upper mid: bias right */
        const int cmp =
            mosFullHeadCompare_(m, (uint32_t)mid, score, memberOrId);
        if (memberOrId ? cmp <= 0 : cmp < 0) {
            left = mid; /* sub-map mid starts at or below: search right */
        } else {
            right = mid - 1;
        }
    }

    return (uint32_t)left;
}

/* Rows per node of the three parallel per-sub-map arrays.  Large enough
 * that they stay in the native (one contiguous array) or medium (a short
 * node array) tiers for any realistic set: a Get by sub-map index then
 * costs O(1) or a walk over a few nodes, never the large tier's list walk
 * from its head, which routing (O(log sub-maps) Gets per call) and rank
 * (a Get per earlier sub-map) would pay on every call. */
static constexpr int MOS_FULL_ARRAY_ROWS = 1024;

/* Insert one sub-map slot (its flex, middle offset and rangeBox) at 'at'
 * into the three parallel arrays, which hold 'count' slots each now.  The
 * caller then sets m->mapCount. */
static void mosFullInsertSlot_(multiOrderedSetFull *m, uint32_t at,
                               uint32_t count, flex *map, uint32_t middle,
                               databox range) {
    /* Each insert advances its own count (the native-to-medium promotion
     * keys off it) */
    uint32_t c = count;
    multiarrayNativeInsert(m->scoreMap, flex *, MOS_FULL_ARRAY_ROWS, c, at,
                           &map);
    c = count;
    multiarrayNativeInsert(m->middle, uint32_t, MOS_FULL_ARRAY_ROWS, c, at,
                           &middle);
    c = count;
    multiarrayNativeInsert(m->rangeBox, databox, MOS_FULL_ARRAY_ROWS, c, at,
                           &range);
}

/* Split an oversized sub-map at its middle row.  Routing compares whole
 * rows (findSubMap_), so a cut may fall inside an equal-score run.  Lower
 * [0,k) stays; upper [k,pairs) becomes a new sub-map at mapIdx+1. */
static void splitSubMap(multiOrderedSetFull *m, uint32_t mapIdx) {
    flex *map = getSubMap(m, mapIdx);
    if (!map) {
        return;
    }

    const size_t pairs = subMapCount(map);
    if (pairs < 2) {
        return;
    }

    /* Lower [0,k) stays in 'map' (reallocated); upper [k,pairs) returns new.
     * flexSplitRange is ELEMENT-indexed. */
    const size_t k = pairs / 2;
    const uint32_t splitElem = (uint32_t)(k * MOS_ELEMENTS_PER_ENTRY);
    const uint32_t moveElems = (uint32_t)((pairs - k) * MOS_ELEMENTS_PER_ENTRY);
    flex *upper = flexSplitRange(&map, (int32_t)splitElem, moveElems);

    /* 'map' was reallocated by the split; store the lower half back. */
    setSubMap(m, mapIdx, map);

    /* Compute the upper half's middle + range BEFORE the inserts (the inserts
     * can relocate the multiarrays, but 'upper' itself is not moved). */
    flexEntry *upMid = flexMiddle(upper, MOS_ELEMENTS_PER_ENTRY);
    uint32_t upperMiddle = upMid ? (uint32_t)(upMid - upper) : FLEX_EMPTY_SIZE;
    databox upperRange = {};
    rangeBoxRefresh(&upperRange, flexHead(upper));

    /* The upper half becomes the slot after the lower one */
    mosFullInsertSlot_(m, mapIdx + 1, m->mapCount, upper, upperMiddle,
                       upperRange);
    m->mapCount++;

    /* Lower half shrank: recompute its middle. rangeBox[mapIdx] is unchanged
     * (its head/min score is the same after removing the tail). totalEntries
     * is unchanged (split moves pairs, never adds/removes). */
    updateMiddle(m, mapIdx);
}

/* Insert the row (score, member) into the sub-map that holds its place.
 * In pool mode the row holds the member's pool id, which is also what
 * routes it. */
static void insertRow(multiOrderedSetFull *m, const databox *score,
                      const databox *member) {
    databox memberOrId = memberToPoolId(m, member);
    const uint32_t mapIdx = findSubMap_(m, score, &memberOrId);
    flex *map = getSubMap(m, mapIdx);
    if (!map) {
        return;
    }

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

    /* Split oversized sub-maps so the structure scales (>= 2 rows); the
     * sub-map count grows with the set, so no sub-map outgrows its limit */
    if (flexBytes(map) > m->maxMapSize && subMapCount(map) >= 2) {
        splitSubMap(m, mapIdx);
    }
}

/* Delete the 'rows' rows of sub-map 'mapIdx' that start at 'entry', in
 * one flex delete (their pool ids released first; the member index is the
 * caller's).  The middle, total and rangeBox are updated once. */
static void deleteRows_(multiOrderedSetFull *m, uint32_t mapIdx,
                        flexEntry *entry, size_t rows) {
    flex *map = getSubMap(m, mapIdx);
    if (!map || rows == 0) {
        return;
    }

    if (m->pool) {
        flexEntry *e = entry;
        for (size_t i = 0; i < rows && e; i++) {
            flexEntry *memberEntry = flexNext(map, e);
            if (!memberEntry) {
                break;
            }
            databox memberOrId;
            databoxWide memberOrIdWide;
            flexGetByTypeInto(memberEntry, &memberOrId, &memberOrIdWide);
            releasePoolMember(m, &memberOrId);
            e = flexNext(map, memberEntry);
        }
    }

    if (rows == 1 && subMapCount(map) > 1) {
        /* One row: move the stored (exact) middle by at most a row in
         * O(1), as multimapFull's deletes do, instead of walking half the
         * sub-map to find it again */
        flexEntry *middle = map + getMiddle(m, mapIdx);
        flexDeleteSortedValueWithMiddle(&map, MOS_ELEMENTS_PER_ENTRY, entry,
                                        &middle);
        setSubMap(m, mapIdx, map);
        setMiddle(m, mapIdx, (uint32_t)(middle - map));
    } else {
        flexDeleteCount(&map, &entry,
                        (uint32_t)(rows * MOS_ELEMENTS_PER_ENTRY));
        setSubMap(m, mapIdx, map);
        updateMiddle(m, mapIdx);
    }
    m->totalEntries -= rows;

    /* Update rangeBox - check if map has entries before reading head.
     * flexHead() returns non-NULL even for empty flex (points past header),
     * so we must check flexCount() to avoid reading invalid memory.  An
     * empty map holds the INT64_MAX sentinel. */
    databox *rangeScore = getRangeBox(m, mapIdx);
    if (rangeScore) {
        rangeBoxRefresh(rangeScore,
                        flexCount(map) > 0 ? flexHead(map) : nullptr);
    }
}

/* Take the members of the 'rows' rows from 'first' out of the member
 * index, copying each member and score out (owned) into 'members' and
 * 'scores' when they are not NULL.  Stops before a row whose member
 * cannot be read (only a damaged pool), which stays.  Returns the rows
 * taken, which the caller then deletes with deleteRows_. */
static size_t mosFullDropMembers_(multiOrderedSetFull *m, const flex *map,
                                  flexEntry *first, size_t rows,
                                  databox *members, databox *scores) {
    size_t taken = 0;
    for (flexEntry *e = first; taken < rows && e; taken++) {
        flexEntry *memberEntry = flexNext(map, e);
        databox member = {};
        databoxWide memberWide;
        if (!memberEntry || !readMember(m, memberEntry, &member, &memberWide)) {
            break;
        }

        if (members) {
            mosReadOwned(e, &scores[taken]);
            members[taken] = databoxCopy(&member);
        }
        multidictDelete(m->memberIndex, &member);
        e = flexNext(map, memberEntry);
    }

    return taken;
}

/* Reclaim an emptied sub-map slot so the first rows stay ascending for
 * findSubMap_'s binary search. Score updates can empty a *middle* sub-map;
 * an empty middle keeps the INT64_MAX rangeBox sentinel, breaking the
 * ascending order the search relies on -> mis-routing -> duplicates.
 * Never reclaims the last sub-map (mapCount must stay >= 1; a lone empty map
 * with rangeBox==INT64_MAX is harmless, search returns 0 trivially).
 * WARNING: shifts all sub-map indices > mapIdx down by one and frees the
 * slot's flex — callers must NOT use a held mapIdx/flex* afterward. */
/* Drop sub-map slot 'mapIdx' from the three per-sub-map arrays (its flex
 * is the caller's: freed or merged elsewhere) and release its range box */
static void dropSubMapSlot_(multiOrderedSetFull *m, uint32_t mapIdx) {
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
    dropSubMapSlot_(m, mapIdx);
}

/* Merge sub-map 'mapIdx + 1' into 'mapIdx' when both fit in half a
 * sub-map: every row of a sub-map sorts before every row of the next, so
 * appending keeps the order.  The headroom keeps a score update (a
 * remove, then an insert) from merging a pair that its insert splits
 * again: a split leaves halves summing past maxMapSize, which deletes
 * must shrink by half before they merge. */
static bool mergeSubMapWithNext_(multiOrderedSetFull *m, uint32_t mapIdx) {
    if (mapIdx + 1 >= m->mapCount) {
        return false;
    }
    flex *map = getSubMap(m, mapIdx);
    flex *next = getSubMap(m, mapIdx + 1);
    const size_t fits = m->maxMapSize / 2;
    if (!map || !next || flexBytes(map) + flexBytes(next) > fits) {
        return false;
    }

    flexBulkAppendFlex(&map, next);
    flexFree(next);
    setSubMap(m, mapIdx, map);
    updateMiddle(m, mapIdx);
    rangeBoxRefresh(getRangeBox(m, mapIdx),
                    flexCount(map) ? flexHead(map) : nullptr);
    dropSubMapSlot_(m, mapIdx + 1);
    return true;
}

/* After a delete left sub-map 'mapIdx' holding rows: merge it with its next
 * neighbour, or else its previous one, when the pair fits (as multimapFull
 * does), so deletes scattered through a set don't leave it spread over
 * many nearly empty sub-maps */
static void coalesceAround_(multiOrderedSetFull *m, uint32_t mapIdx) {
    if (mapIdx >= m->mapCount) {
        return;
    }
    if (!mergeSubMapWithNext_(m, mapIdx) && mapIdx > 0) {
        mergeSubMapWithNext_(m, mapIdx - 1);
    }
}

/* Bytes a refill leaves unused below maxMapSize, for the flex header to
 * grow as its byte count does */
static constexpr int MOS_FULL_COMPACT_SLACK = 8;

/* Fill every sub-map, left to right, to maxMapSize with the leading rows
 * of the one after it; a sub-map whose rows all fit (or an empty one)
 * joins its neighbour.  Rows keep their order, so the sub-maps stay
 * sorted; only where each begins moves, and its range box follows. */
void multiOrderedSetFullCompact(multiOrderedSetFull *m) {
    uint32_t i = 0;
    while (i + 1 < m->mapCount) {
        flex *map = getSubMap(m, i);
        flex *next = getSubMap(m, i + 1);
        const size_t have = flexBytes(map);
        if (have + flexBytes(next) <= m->maxMapSize || subMapCount(next) == 0) {
            flexBulkAppendFlex(&map, next);
            flexFree(next);
            setSubMap(m, i, map);
            updateMiddle(m, i);
            rangeBoxRefresh(getRangeBox(m, i),
                            flexCount(map) ? flexHead(map) : nullptr);
            dropSubMapSlot_(m, i + 1);
            continue;
        }

        /* The most leading rows of 'next' that fit in the room left; it
         * keeps at least one (all of them fitting merged above) */
        const size_t room = m->maxMapSize > have + MOS_FULL_COMPACT_SLACK
                                ? m->maxMapSize - have - MOS_FULL_COMPACT_SLACK
                                : 0;
        const size_t total = flexCount(next) / MOS_ELEMENTS_PER_ENTRY;
        const uint8_t *head = (const uint8_t *)flexHead(next);
        flexEntry *fe = flexHead(next);
        size_t rows = 0;
        while (rows + 1 < total) {
            flexEntry *after = flexNext(next, flexNext(next, fe));
            if ((size_t)((const uint8_t *)after - head) > room) {
                break;
            }
            fe = after;
            rows++;
        }

        if (rows) {
            flex *moved = flexSplitRange(
                &next, 0, (uint32_t)(rows * MOS_ELEMENTS_PER_ENTRY));
            flexBulkAppendFlex(&map, moved);
            flexFree(moved);
            setSubMap(m, i, map);
            setSubMap(m, i + 1, next);
            updateMiddle(m, i);
            updateMiddle(m, i + 1);
            rangeBoxRefresh(getRangeBox(m, i), flexHead(map));
            rangeBoxRefresh(getRangeBox(m, i + 1), flexHead(next));
        }

        i++;
    }
}

/* After a bulk remove or pop (whose loop held live indices): reclaim
 * emptied sub-maps (backward, so each delete only shifts visited indices),
 * then merge every neighbouring pair that fits */
static void reclaimAllEmptySubMaps(multiOrderedSetFull *m) {
    for (int32_t i = (int32_t)m->mapCount - 1; i >= 0 && m->mapCount > 1; i--) {
        reclaimEmptySubMap(m, (uint32_t)i);
    }
    for (uint32_t i = 0; i + 1 < m->mapCount;) {
        if (!mergeSubMapWithNext_(m, i)) {
            i++; /* merged: the grown map meets its new neighbour next */
        }
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
 * index's value for it): the sub-map that holds the (score, member or pool
 * id) row, then a binary search for it there, as rows are kept sorted by
 * that pair.  NULL when there is no such row. */
static flexEntry *findEntryForScore(const multiOrderedSetFull *m,
                                    const databox *member, const databox *score,
                                    uint32_t *mapIdx) {
    /* In pool mode the row holds the member's pool ID */
    *mapIdx = 0;
    databox memberToMatch;
    if (m->pool) {
        uint64_t id;
        if (!multimapAtomGetId(m->pool, member, &id)) {
            return nullptr; /* Member not in pool */
        }
        memberToMatch = databoxNewUnsigned(id);
    } else {
        memberToMatch = *member;
    }

    *mapIdx = findSubMap_(m, score, &memberToMatch);
    flex *map = getSubMap(m, *mapIdx);
    if (!map || flexCount(map) == 0) {
        return nullptr;
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
        return nullptr;
    }

    return findEntryForScore(m, member, &score, mapIdx);
}

/* Rows in sub-map 'i' */
static uint64_t mosFullCountAt_(const multiOrderedSetFull *m, uint32_t i) {
    const flex *map = getSubMap(m, i);
    return map ? subMapCount(map) : 0;
}

/* The sub-map holding rank 'rank' (below totalEntries) and, in '*local',
 * that row's index inside it: whole sub-maps are skipped by their counts,
 * from whichever end is nearer.  mapCount when the rank is past the end. */
static uint32_t mosFullLocateRank_(const multiOrderedSetFull *m, uint64_t rank,
                                   uint64_t *local) {
    if (rank < m->totalEntries / 2) {
        uint64_t before = 0; /* rows in the sub-maps before 'i' */
        for (uint32_t i = 0; i < m->mapCount; i++) {
            const uint64_t n = mosFullCountAt_(m, i);
            if (rank < before + n) {
                *local = rank - before;
                return i;
            }
            before += n;
        }
    } else {
        uint64_t from = m->totalEntries; /* rows in sub-map 'i' and after */
        for (uint32_t i = m->mapCount; i-- > 0;) {
            from -= mosFullCountAt_(m, i);
            if (rank >= from) {
                *local = rank - from;
                return i;
            }
        }
    }

    *local = 0;
    return m->mapCount;
}

/* The first row whose score is at or above 'score', in sub-map '*mapIdx';
 * NULL (with '*mapIdx' == mapCount) when every score is below.  Starts in
 * the last sub-map whose first score is strictly below 'score', since an
 * equal-score run may begin in its tail; a sub-map holds at most
 * maxMapSize bytes, so the walk there is short. */
static flexEntry *mosFullSeekScore_(const multiOrderedSetFull *m,
                                    const databox *score, uint32_t *mapIdx) {
    for (uint32_t i = findSubMap_(m, score, nullptr); i < m->mapCount; i++) {
        flex *map = getSubMap(m, i);
        flexEntry *entry = map ? flexHeadIfAny(map) : nullptr;
        while (entry) {
            databox current;
            databoxWide currentWide;
            flexGetByTypeInto(entry, &current, &currentWide);
            if (databoxCompare(&current, score) >= 0) {
                *mapIdx = i;
                return entry;
            }

            flexEntry *memberEntry = flexNext(map, entry);
            entry = memberEntry ? flexNext(map, memberEntry) : nullptr;
        }
    }

    *mapIdx = m->mapCount;
    return nullptr;
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
    m->memberIndex =
        multidictNew(&multidictTypeExactKey, m->mdClass, multidictRandomSeed());

    /* Create initial sub-map */
    m->scoreMap = multiarrayNativeNew(flex *);
    m->middle = multiarrayNativeNew(uint32_t);
    m->rangeBox = multiarrayNativeNew(databox);

    /* @own: container; free=multiOrderedSetFullFree */
    flex *initialMap = flexNew();
    databox initialRange = {};
    rangeBoxRefresh(&initialRange, nullptr);
    mosFullInsertSlot_(m, 0, 0, initialMap, FLEX_EMPTY_SIZE, initialRange);

    m->mapCount = 1;
    m->totalEntries = 0;
    m->maxMapSize = MOS_FULL_DEFAULT_MAX_MAP_SIZE;
    m->flags = 0;
    m->pool = nullptr;

    return m;
}

multiOrderedSetFull *multiOrderedSetFullNewWithPool(multimapAtom *pool) {
    multiOrderedSetFull *m = multiOrderedSetFullNew();
    m->pool = pool;
    return m;
}

/* The atom table an owned pool gets by default: found and read by id in
 * constant time */
static const multimapAtomConfig mosOwnedPoolConfig_ = {
    .index = MULTIMAP_ATOM_INDEX_SLOTS, .store = MULTIMAP_ATOM_STORE_DIRECT};

multiOrderedSetFull *
multiOrderedSetFullNewWithOwnedPoolConfig(const multimapAtomConfig *config) {
    /* @own: container; free=multiOrderedSetFullFree */
    multimapAtom *pool = multimapAtomNewConfig(config);
    if (!pool) {
        return nullptr;
    }

    multiOrderedSetFull *m = multiOrderedSetFullNew();
    m->pool = pool;
    m->flags |= MOS_FLAG_POOL_OWNED;
    return m;
}

multiOrderedSetFull *multiOrderedSetFullNewWithOwnedPool(void) {
    return multiOrderedSetFullNewWithOwnedPoolConfig(&mosOwnedPoolConfig_);
}

multiOrderedSetFull *
multiOrderedSetFullNewFromMedium(struct multiOrderedSetMedium *medium,
                                 flex *maps[2],
                                 [[maybe_unused]] uint32_t middles[2]) {
    multiOrderedSetFull *m = multiOrderedSetFullNew();

    /* Reset to clear the initial empty map and prepare for new data */
    multiOrderedSetFullReset(m);

    /* Iterate through both medium maps and add entries */
    for (int mapIdx = 0; mapIdx < 2; mapIdx++) {
        flexEntry *entry = flexHead(maps[mapIdx]);
        while (entry) {
            flexEntry *memberEntry = flexNext(maps[mapIdx], entry);
            if (!memberEntry) {
                break;
            }

            databox score = {};
            databox member = {};
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
    copy->memberIndex = multidictNew(&multidictTypeExactKey, copy->mdClass,
                                     multidictRandomSeed());

    /* Copy all entries from original memberIndex */
    multidictIterator iter;
    multidictIteratorInit((multidict *)m->memberIndex, &iter);
    multidictEntry entry;
    while (multidictIteratorNext(&iter, &entry)) {
        multidictPut(copy->memberIndex, &entry.key, &entry.val);
    }
    multidictIteratorRelease(&iter);

    copy->scoreMap = multiarrayNativeNew(flex *);
    copy->middle = multiarrayNativeNew(uint32_t);
    copy->rangeBox = multiarrayNativeNew(databox);

    for (uint32_t i = 0; i < m->mapCount; i++) {
        flex *origMap = getSubMap(m, i);
        /* @own: container; free=multiOrderedSetFullFree */
        flex *mapCopy = origMap ? flexDuplicate(origMap) : flexNew();
        uint32_t mid = getMiddle(m, i);

        /* Rebuilt from the copy's own head, so the copy never shares a
         * payload (or borrowed bytes) with the original */
        databox rangeCopy = {};
        rangeBoxRefresh(&rangeCopy,
                        flexCount(mapCopy) > 0 ? flexHead(mapCopy) : nullptr);

        mosFullInsertSlot_(copy, i, i, mapCopy, mid, rangeCopy);
    }

    copy->mapCount = m->mapCount;
    copy->totalEntries = m->totalEntries;
    copy->maxMapSize = m->maxMapSize;

    /* Handle pool mode for copy */
    if (m->pool) {
        if (m->flags & MOS_FLAG_POOL_OWNED) {
            /* Source owns pool - copy creates its own pool (the same
             * settings) and re-interns */
            const multimapAtomConfig settings = {
                .index = multimapAtomGetIndex(m->pool),
                .ids = multimapAtomGetIds(m->pool),
                .store = multimapAtomGetStore(m->pool)};
            /* @own: container; free=multiOrderedSetFullFree */
            copy->pool = multimapAtomNewConfig(&settings);
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
                    databox member = {};
                    if (multimapAtomLookupById(m->pool, oldId.data.u64,
                                               &member)) {
                        /* Intern in new pool and update entry.  A new id
                         * of another width reallocates the map: the entry
                         * keeps its offset, so find it again from there */
                        const size_t at = (size_t)(memberEntry - map);
                        uint64_t newId =
                            multimapAtomIntern(copy->pool, &member);
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
                        /* One more reference: the copy holds the id too */
                        multimapAtomRetainById(copy->pool, idBox.data.u64);
                    }
                    ent = flexNext(map, memberEntry);
                }
            }
        }
    } else {
        copy->pool = nullptr;
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
            multimapAtomFree(m->pool);
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

/* Hand a pool-free Full's rows over as one flex (its sub-maps appended in
 * order: every row of a sub-map sorts before the next's) and free the
 * rest of it: member index, per-sub-map arrays and range boxes, struct.
 * NULL, with nothing freed, for a pooled set (its rows hold pool ids,
 * which only Full resolves). */
flex *multiOrderedSetFullTakeRows(multiOrderedSetFull *m) {
    if (m->pool) {
        return nullptr;
    }

    flex *rows = getSubMap(m, 0);
    for (uint32_t i = 1; i < m->mapCount; i++) {
        flex *next = getSubMap(m, i);
        flexBulkAppendFlex(&rows, next);
        flexFree(next);
    }

    multidictFree(m->memberIndex);
    if (m->mdClass) {
        multidictDefaultClassFree(m->mdClass);
    }
    rangeBoxReleaseAll(m);
    multiarrayNativeFree(m->scoreMap);
    multiarrayNativeFree(m->middle);
    multiarrayNativeFree(m->rangeBox);
    zfree(m);
    return rows;
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
            multimapAtomReset(m->pool);
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
    databox initialRange = {};
    rangeBoxRefresh(&initialRange, nullptr);
    mosFullInsertSlot_(m, 0, 0, initialMap, FLEX_EMPTY_SIZE, initialRange);

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
        bytes += multimapAtomBytes(m->pool);
    }

    return bytes;
}

#ifdef DATAKIT_TEST
/* The caches sorted finds trust blindly: every sub-map's stored middle is
 * its exact middle row, every range box holds its sub-map's head score,
 * rows ascend across sub-maps, only a lone sub-map is empty, and the
 * total matches */
void multiOrderedSetFullVerify(const multiOrderedSetFull *m) {
    assert(m->mapCount >= 1);
    uint64_t rows = 0;
    databox prevLast = {};
    databoxWide prevLastWide;
    bool havePrev = false;
    for (uint32_t i = 0; i < m->mapCount; i++) {
        flex *map = getSubMap(m, i);
        assert(map);
        const size_t n = subMapCount(map);
        assert(n > 0 || m->mapCount == 1);
        rows += n;

        const flexEntry *mid = flexMiddle(map, MOS_ELEMENTS_PER_ENTRY);
        assert(getMiddle(m, i) ==
               (mid ? (uint32_t)(mid - map) : (uint32_t)FLEX_EMPTY_SIZE));
        if (!n) {
            continue;
        }

        databox head;
        databoxWide headWide;
        flexGetByTypeInto(flexHead(map), &head, &headWide);
        assert(databoxCompare(getRangeBox(m, i), &head) == 0);
        if (havePrev) {
            assert(databoxCompare(&prevLast, &head) <= 0);
        }
        flexGetByTypeInto(flexIndex(map, -(int32_t)MOS_ELEMENTS_PER_ENTRY),
                          &prevLast, &prevLastWide);
        havePrev = true;
    }
    assert(rows == m->totalEntries);
}
#endif

void multiOrderedSetFullStats(const multiOrderedSetFull *m, dkTierStats *out) {
    uint64_t data = 0;
    for (uint32_t i = 0; i < m->mapCount; i++) {
        const flex *map = getSubMap(m, i);
        data += map ? flexBytes(map) : 0;
    }

    /* The struct, member index and owned pool (multiOrderedSetFullBytes),
     * plus each sub-map's pointer, middle offset and range box */
    *out = (dkTierStats){
        .count = m->totalEntries,
        .dataBytes = data,
        .totalBytes = multiOrderedSetFullBytes(m) +
                      (uint64_t)m->mapCount *
                          (sizeof(flex *) + sizeof(uint32_t) + sizeof(databox)),
        .units = m->mapCount,
        .unitLimit = m->maxMapSize,
        .tier = 3};
}

/* ====================================================================
 * Insertion / Update
 * ==================================================================== */

/* -1, 0 or 1 as the row (score, memberOrId) at 'row' of 'map' compares
 * with ('score', the member or id entry at 'memberEntry') */
static int mosFullRowCompare_(const flex *map, flexEntry *row,
                              const databox *score, flexEntry *memberEntry) {
    databox s;
    databoxWide sWide;
    flexGetByTypeInto(row, &s, &sWide);
    const int byScore = databoxCompare(&s, score);
    if (byScore) {
        return byScore;
    }

    databox a;
    databoxWide aWide;
    databox b;
    databoxWide bWide;
    flexGetByTypeInto(flexNext(map, row), &a, &aWide);
    flexGetByTypeInto(memberEntry, &b, &bWide);
    return databoxCompare(&a, &b);
}

/* Give the row at 'entry' (sub-map 'mapIdx') the score 'score' where it
 * is, when the rows stay in order: a row before it and a row after it in
 * the same sub-map, below and above the new (score, member).  False,
 * changing nothing, otherwise (the row must move, or it is a sub-map's
 * first or last row, which routing and the next sub-map's order read).
 * A score update that keeps its place (small deltas on spread scores)
 * then costs one rewrite instead of a delete and an insert. */
static bool mosFullRescoreInPlace_(multiOrderedSetFull *m,
                                   const uint32_t mapIdx, flexEntry *entry,
                                   const databox *score) {
    flex *map = getSubMap(m, mapIdx);
    if (!map || entry == flexHead(map)) {
        return false;
    }

    flexEntry *member = flexNext(map, entry);
    flexEntry *next = member ? flexNext(map, member) : nullptr;
    flexEntry *prevMember = flexPrev(map, entry);
    flexEntry *prev = prevMember ? flexPrev(map, prevMember) : nullptr;
    if (!next || !prev || mosFullRowCompare_(map, prev, score, member) >= 0 ||
        mosFullRowCompare_(map, next, score, member) <= 0) {
        return false;
    }

    /* The middle is an offset: an entry before it that changes size moves
     * it by the change */
    const size_t at = (size_t)(entry - map);
    size_t middle = getMiddle(m, mapIdx);
    const size_t bytesBefore = flexBytes(map);
    flexReplaceByType(&map, map + at, score);
    if (at < middle) {
        middle = (size_t)((int64_t)middle + (int64_t)flexBytes(map) -
                          (int64_t)bytesBefore);
    }

    setSubMap(m, mapIdx, map);
    setMiddle(m, mapIdx, (uint32_t)middle);
    if (flexBytes(map) > m->maxMapSize && subMapCount(map) >= 2) {
        splitSubMap(m, mapIdx);
    }

    return true;
}

/* 'member', now scored 'old', given 'score' in place when its row keeps
 * its place (mosFullRescoreInPlace_), its member index value rewritten
 * too; false, changing nothing, when the row must move */
/* Take the row at 'entry' of sub-map 'mapIdx' out of the score maps */
static void removeRowAt_(multiOrderedSetFull *m, const uint32_t mapIdx,
                         flexEntry *entry) {
    deleteRows_(m, mapIdx, entry, 1);
    const uint32_t before = m->mapCount;
    reclaimEmptySubMap(m, mapIdx);
    if (m->mapCount == before) {
        coalesceAround_(m, mapIdx);
    }
}

/* 'member', now scored 'old', is to take 'score': rescored in place when
 * its row keeps its place (mosFullRescoreInPlace_; the member index then
 * takes the new score too, and the result is true), else its row is taken
 * out of the score maps, found by the same one search, for the caller to
 * insert again (false) */
static bool mosFullRescoreOrRemove_(multiOrderedSetFull *m,
                                    const databox *member, const databox *old,
                                    const databox *score) {
    uint32_t mapIdx;
    flexEntry *entry = findEntryForScore(m, member, old, &mapIdx);
    if (!entry) {
        return false;
    }

    if (!mosFullRescoreInPlace_(m, mapIdx, entry, score)) {
        removeRowAt_(m, mapIdx, entry);
        return false;
    }

    /* The row holds the new score now: the index follows it (multidict
     * fails fast on allocation, so the replace completes) */
    (void)multidictReplace(m->memberIndex, member, score);
    return true;
}

/* Take the score-map row of 'member', whose stored score is 'old' */
static void removeMemberRow(multiOrderedSetFull *m, const databox *member,
                            const databox *old) {
    uint32_t mapIdx;
    flexEntry *entry = findEntryForScore(m, member, old, &mapIdx);
    if (entry) {
        removeRowAt_(m, mapIdx, entry);
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

    insertRow(m, score, member);
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
        if (mosFullRescoreOrRemove_(m, member, &old, score)) {
            return true;
        }
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

    insertRow(m, score, member);
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

bool multiOrderedSetFullCombine(multiOrderedSetFull *m, const databox *operand,
                                const databox *member, mosCombineOp op,
                                databox *result) {
    databox existingScore;
    databoxWide existingScoreWide;
    const bool existed =
        findScore(m, member, &existingScore, &existingScoreWide);

    if (existed) {
        if (!mosCombine(&existingScore, operand, op, result)) {
            return false;
        }

        if (mosFullRescoreOrRemove_(m, member, &existingScore, result)) {
            return true;
        }
    } else {
        *result = *operand;
    }

    setMemberScore(m, member, result, existed);
    return true;
}

/* ====================================================================
 * Deletion
 * ==================================================================== */

/* RemoveIf's question, asked by the member index once the member is found
 * (its score, shown too, is not needed for the answer) */
typedef struct mosFullAsk_ {
    flexProceed *proceed;
    void *ctx;
} mosFullAsk_;

static bool mosFullAskGate_(void *ctx, const databox *) {
    const mosFullAsk_ *ask = ctx;
    return ask->proceed(ask->ctx);
}

/* Remove 'member' with one member-index search, which hands back the
 * score (owned) that locates its row; with 'proceed', asked inside that
 * search before anything changes */
static bool removeMember(multiOrderedSetFull *m, const databox *member,
                         databox *score, flexProceed *proceed, void *ctx) {
    mosFullAsk_ ask = {.proceed = proceed, .ctx = ctx};
    if (!(proceed ? multidictGetAndDeleteIf(m->memberIndex, member, score,
                                            mosFullAskGate_, &ask)
                  : multidictGetAndDelete(m->memberIndex, member, score))) {
        return false;
    }

    removeMemberRow(m, member, score);
    return true;
}

bool multiOrderedSetFullRemoveIf(multiOrderedSetFull *m, const databox *member,
                                 flexProceed *proceed, void *ctx) {
    databox score;
    if (!removeMember(m, member, &score, proceed, ctx)) {
        return false;
    }

    databoxFreeData(&score);
    return true;
}

bool multiOrderedSetFullRemove(multiOrderedSetFull *m, const databox *member) {
    return multiOrderedSetFullRemoveIf(m, member, nullptr, nullptr);
}

bool multiOrderedSetFullRemoveGetScore(multiOrderedSetFull *m,
                                       const databox *member, databox *score) {
    /* The removal's owned copy is the caller's */
    return removeMember(m, member, score, nullptr, nullptr);
}

size_t multiOrderedSetFullRemoveRangeByScore(multiOrderedSetFull *m,
                                             const mosRangeSpec *range) {
    size_t removed = 0;

    /* From the first row at or above the minimum to past the maximum */
    uint32_t mapIdx;
    flexEntry *entry = mosFullSeekScore_(m, &range->min, &mapIdx);
    bool pastMax = false;
    for (; mapIdx < m->mapCount && !pastMax; mapIdx++, entry = nullptr) {
        flex *map = getSubMap(m, mapIdx);
        if (!map) {
            continue;
        }

        if (!entry) {
            entry = flexHeadIfAny(map);
        }

        /* Rows are in score order, so this sub-map's rows in the range are
         * one span: after any rows equal to an exclusive minimum, up to the
         * first past the maximum */
        flexEntry *spanStart = nullptr;
        size_t spanRows = 0;
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
                spanStart = spanStart ? spanStart : entry;
                spanRows++;
            } else {
                const int cmp = databoxCompare(&score, &range->max);
                if (cmp > 0 || (cmp == 0 && range->maxExclusive)) {
                    pastMax = true;
                    break;
                }
            }
            entry = flexNext(map, memberEntry);
        }

        const size_t taken =
            mosFullDropMembers_(m, map, spanStart, spanRows, nullptr, nullptr);
        deleteRows_(m, mapIdx, spanStart, taken);
        removed += taken;
        pastMax = pastMax || taken < spanRows; /* a damaged pool stops it */
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

    /* From the row at rank 'start' (whole sub-maps skipped by count) to
     * rank 'stop' */
    size_t removed = 0;
    uint64_t left = (uint64_t)(stop - start) + 1; /* rows still to remove */
    uint64_t local;
    uint32_t mapIdx = mosFullLocateRank_(m, (uint64_t)start, &local);
    for (; mapIdx < m->mapCount && left > 0; mapIdx++, local = 0) {
        flex *map = getSubMap(m, mapIdx);
        if (!map) {
            continue;
        }

        /* This sub-map's share: one span from row 'local' */
        const uint64_t inMap = subMapCount(map) - local;
        const size_t rows = (size_t)(inMap < left ? inMap : left);
        flexEntry *spanStart =
            rows ? flexIndex(map, (int32_t)(local * MOS_ELEMENTS_PER_ENTRY))
                 : nullptr;
        const size_t taken =
            mosFullDropMembers_(m, map, spanStart, rows, nullptr, nullptr);
        deleteRows_(m, mapIdx, spanStart, taken);
        removed += taken;
        left -= rows;
        if (taken < rows) {
            break; /* a damaged pool stops it */
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

        /* This sub-map's share: one span from its head, copied out
         * (self-contained) before it goes */
        const size_t inMap = subMapCount(map);
        const size_t rows = inMap < count - popped ? inMap : count - popped;
        flexEntry *head = rows ? flexHead(map) : nullptr;
        const size_t taken = mosFullDropMembers_(
            m, map, head, rows, members + popped, scores + popped);
        deleteRows_(m, mapIdx, head, taken);
        popped += taken;
        if (taken < rows) {
            break; /* a damaged pool stops it */
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
            databox member = {};
            databoxWide memberWide;
            if (!readMember(m, memberEntry, &member, &memberWide)) {
                break;
            }

            /* Self-contained results: copy before the entry goes away */
            mosReadOwned(entry, &scores[popped]);
            members[popped] = databoxCopy(&member);

            multidictDelete(m->memberIndex, &member);
            deleteRows_(m, mapIdx, entry, 1);

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

    /* The rows before the member's sub-map, counted from the nearer end:
     * the sub-maps before it, or the total less the sub-maps from it on */
    int64_t rank = 0;
    if (mapIdx <= m->mapCount / 2) {
        for (uint32_t i = 0; i < mapIdx; i++) {
            rank += (int64_t)mosFullCountAt_(m, i);
        }
    } else {
        rank = (int64_t)m->totalEntries;
        for (uint32_t i = mapIdx; i < m->mapCount; i++) {
            rank -= (int64_t)mosFullCountAt_(m, i);
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

    uint64_t local;
    const uint32_t mapIdx = mosFullLocateRank_(m, (uint64_t)rank, &local);
    flex *map = mapIdx < m->mapCount ? getSubMap(m, mapIdx) : nullptr;
    flexEntry *entry =
        map ? flexIndex(map, (int32_t)(local * MOS_ELEMENTS_PER_ENTRY))
            : nullptr;
    flexEntry *memberEntry = entry ? flexNext(map, entry) : nullptr;
    if (!memberEntry) {
        return false;
    }

    flexGetByType(entry, score);

    /* In pool mode, convert pool ID back to member string */
    return readMember(m, memberEntry, member, nullptr);
}

/* ====================================================================
 * Range Queries
 * ==================================================================== */

size_t multiOrderedSetFullCountByScore(const multiOrderedSetFull *m,
                                       const mosRangeSpec *range) {
    size_t count = 0;

    /* From the first row at or above the minimum to past the maximum */
    uint32_t mapIdx;
    flexEntry *entry = mosFullSeekScore_(m, &range->min, &mapIdx);
    for (; mapIdx < m->mapCount; mapIdx++, entry = nullptr) {
        flex *map = getSubMap(m, mapIdx);
        if (!map) {
            continue;
        }

        if (!entry) {
            entry = flexHeadIfAny(map);
        }
        while (entry) {
            databox score;
            databoxWide scoreWide;
            flexGetByTypeInto(entry, &score, &scoreWide);

            int cmp = databoxCompare(&score, &range->max);
            if (cmp > 0 || (cmp == 0 && range->maxExclusive)) {
                return count;
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
    iter->current = nullptr; /* stays NULL when every sub-map is empty */

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

    iter->valid = (iter->current != nullptr);
}

bool multiOrderedSetFullIteratorInitAtScore(const multiOrderedSetFull *m,
                                            mosIterator *iter,
                                            const databox *score,
                                            bool forward) {
    iter->mos = (void *)m;
    iter->type = MOS_TYPE_FULL;
    iter->forward = forward;

    uint32_t mapIdx;
    iter->current = mosFullSeekScore_(m, score, &mapIdx);
    iter->mapIndex = mapIdx;
    iter->valid = iter->current != nullptr;
    return iter->valid;
}

bool multiOrderedSetFullIteratorInitAtRank(const multiOrderedSetFull *m,
                                           mosIterator *iter, int64_t rank,
                                           bool forward) {
    iter->mos = (void *)m;
    iter->type = MOS_TYPE_FULL;
    iter->forward = forward;

    rank = mosNormalizeRank(rank, m->totalEntries);
    uint64_t local = 0;
    const uint32_t mapIdx =
        rank < 0 ? m->mapCount : mosFullLocateRank_(m, (uint64_t)rank, &local);
    flex *map = mapIdx < m->mapCount ? getSubMap(m, mapIdx) : nullptr;
    iter->mapIndex = mapIdx;
    iter->current =
        map ? flexIndex(map, (int32_t)(local * MOS_ELEMENTS_PER_ENTRY))
            : nullptr;
    iter->valid = iter->current != nullptr;
    return iter->valid;
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
    if (!readMember(m, memberEntry, member, nullptr)) {
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
            iter->current = nullptr;
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

    iter->valid = (iter->current != nullptr);
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
                return readMember(m, memberEntry, member, nullptr);
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
                    return readMember(m, memberEntry, member, nullptr);
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
 * (2) rows strictly ascend across sub-maps: each sub-map's last
 * (score, member or id) row is below the next one's first, equal-score
 * runs included; (3) totalEntries == sum of per-sub-map pair counts.
 * Empty sub-maps (rangeBox == INT64_MAX sentinel) are skipped for
 * (1)/(2). */
static int mosFullCheckInvariants(const multiOrderedSetFull *m) {
    int bad = 0;
    uint64_t sum = 0;
    databoxBig prevLast = {};
    databoxBig prevLastMember = {};
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
        databoxBig headMember;
        flexGetByTypeBig(flexNext(map, flexHead(map)), &headMember);
        if (havePrev) {
            int order = databoxCompare((const databox *)&prevLast,
                                       (const databox *)&headScore);
            if (order == 0) {
                order = databoxCompare((const databox *)&prevLastMember,
                                       (const databox *)&headMember);
            }
            if (order >= 0) {
                printf("  INVARIANT FAIL: submap %u breaks global row order\n",
                       i);
                bad++;
            }
        }
        flexEntry *last =
            flexIndex(map, (int32_t)((cnt - 1) * MOS_ELEMENTS_PER_ENTRY));
        flexGetByTypeBig(last, &prevLast);
        flexGetByTypeBig(flexNext(map, last), &prevLastMember);
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
static constexpr int MOS_COPY_N = 3000;
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
        const char *wrong = nullptr;
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
static constexpr int MOSF_N = 2400;
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
    int err = 0;
    if (multiOrderedSetFullCount(m) != (size_t)n) {
        ERR("step %d: count %zu, oracle %" PRId64, step,
            multiOrderedSetFullCount(m), n);
        return err;
    }
    if (mosFullCheckInvariants(m) != 0) {
        ERR("step %d: invariants broken", step);
    }

    /* Ranks past either end refuse */
    databox member;
    databox score;
    mosIterator it;
    if (multiOrderedSetFullGetByRank(m, n, &member, &score) ||
        multiOrderedSetFullGetByRank(m, -n - 1, &member, &score) ||
        multiOrderedSetFullIteratorInitAtRank(m, &it, n, true)) {
        ERR("step %d: a rank past the end answered", step);
    }

    /* Sampled ranks, from the front and the back, and a short walk */
    for (int k = 0; k < 6 && n > 0; k++) {
        const int64_t r = (int64_t)(splitmix64(rng) % (uint64_t)n);
        const int64_t s = list[r];
        if (!multiOrderedSetFullGetByRank(m, r, &member, &score) ||
            !mosfIs_(&member, &score, s) ||
            !multiOrderedSetFullGetByRank(m, r - n, &member, &score) ||
            !mosfIs_(&member, &score, s)) {
            ERR("step %d: rank %" PRId64 " is not %" PRId64, step, r, s);
            continue;
        }
        const bool forward = k % 2 == 0;
        if (!multiOrderedSetFullIteratorInitAtRank(m, &it, r, forward)) {
            ERR("step %d: iterator at rank %" PRId64, step, r);
            continue;
        }
        for (int64_t w = 0; w < 4; w++) {
            const int64_t at = forward ? r + w : r - w;
            const bool more =
                multiOrderedSetFullIteratorNext(&it, &member, &score);
            if (at < 0 || at >= n) {
                if (more) {
                    ERR("step %d: walk ran past the end", step);
                }
                break;
            }
            if (!more || !mosfIs_(&member, &score, list[at])) {
                ERR("step %d: walk from %" PRId64 " step %" PRId64, step, r, w);
                break;
            }
        }
    }
    return err;
}

/* The owned atom tables the pool tests run: the fewest bytes and the
 * fastest */
static const struct {
    const char *name;
    multimapAtomConfig config;
} mosTestPoolConfigs_[] = {
    {"compact", {}},
    {"fast",
     {.index = MULTIMAP_ATOM_INDEX_SLOTS, .store = MULTIMAP_ATOM_STORE_DIRECT}},
};

int multiOrderedSetFullTest(int, char *[]) {
    int err = 0;

    TEST("multiOrderedSetFull — create and free") {
        multiOrderedSetFull *mos = multiOrderedSetFullNew();
        if (!mos) {
            ERR("Failed to create multiOrderedSetFull");
        }
        if (multiOrderedSetFullCount(mos) != 0) {
            ERR("New set should be empty");
        }
        multiOrderedSetFullFree(mos);
    }

    TEST("multiOrderedSetFull — empty and emptied sets have nothing to read") {
        /* A new set and one emptied after holding rows: counting a score
         * range and starting an iterator at a score find nothing, and
         * neither reads past a sub-map */
        const mosRangeSpec all = {.min = databoxNewSigned(INT64_MIN),
                                  .max = databoxNewSigned(INT64_MAX)};
        const databox zero = databoxNewSigned(0);
        for (int emptied = 0; emptied <= 1; emptied++) {
            multiOrderedSetFull *mos = multiOrderedSetFullNew();
            if (emptied) {
                for (int i = 0; i < 300; i++) {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "m%03d", i);
                    const databox member = databoxNewBytesString(buf);
                    const databox score = databoxNewSigned(i);
                    multiOrderedSetFullAdd(mos, &score, &member);
                }
                for (int i = 0; i < 300; i++) {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "m%03d", i);
                    const databox member = databoxNewBytesString(buf);
                    multiOrderedSetFullRemove(mos, &member);
                }
            }
            /* The iterator starts as junk, as an uninitialized one would */
            mosIterator iter;
            memset(&iter, 0xA5, sizeof(iter));
            multiOrderedSetFullIteratorInit(mos, &iter, true);
            const bool forwardValid = iter.valid;
            memset(&iter, 0xA5, sizeof(iter));
            multiOrderedSetFullIteratorInit(mos, &iter, false);
            const bool reverseValid = iter.valid;
            databox m;
            databox s;
            const bool stepped = multiOrderedSetFullIteratorNext(&iter, &m, &s);
            const databox m007 = databoxNewBytesString("m007");
            databox members[2];
            databox scores[2];
            const size_t picked =
                multiOrderedSetFullRandomMembers(mos, 2, members, scores);
            if (multiOrderedSetFullCount(mos) != 0 ||
                multiOrderedSetFullCountByScore(mos, &all) != 0 ||
                multiOrderedSetFullIteratorInitAtScore(mos, &iter, &zero,
                                                       true) ||
                iter.valid || forwardValid || reverseValid || stepped ||
                multiOrderedSetFullFirst(mos, &m, &s) ||
                multiOrderedSetFullLast(mos, &m, &s) ||
                multiOrderedSetFullGetRank(mos, &m007) != -1 ||
                multiOrderedSetFullGetReverseRank(mos, &m007) != -1 ||
                picked != 0) {
                ERR("%s set: found a row", emptied ? "an emptied" : "a new");
            }
            multiOrderedSetFullFree(mos);
        }
    }

    TEST(
        "multiOrderedSetFull — pooled members: an external and an owned pool") {
        /* @own: caller; free=multimapAtomFree */
        multimapAtom *shared = multimapAtomNew();
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
            multimapAtomCount(shared) != 50) {
            ERR("a pooled set lost members or scores");
        }

        /* Freeing the set leaves the external pool to its owner */
        multiOrderedSetFullFree(a);
        multiOrderedSetFullFree(b);
        if (multimapAtomCount(shared) > 50) {
            ERR("the external pool grew after its set was freed");
        }
        multimapAtomFree(shared);
    }

    TEST("MOS-Full: one score over many sub-maps stays in member order") {
        /* 2000 members at one score: the run splits over many sub-maps
         * (routing compares whole rows, so a cut may fall inside it);
         * members joining at that score afterwards (below, between and
         * above the rest in member order) iterate in member order and rank
         * where iteration puts them */
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
        if (!ordered || !ranked || rank != 2005 || m->mapCount < 2 ||
            mosFullCheckInvariants(m) != 0) {
            ERR("one score: %" PRId64 " iterated, ordered %d, ranked %d, %u "
                "sub-maps",
                rank, ordered, ranked, m->mapCount);
        }
        multiOrderedSetFullFree(m);
    }

    TEST("MOS-Full: equal-score runs across sub-maps, plain and pool mode") {
        /* Ten scores with 400 members each, added in a scattered order, so
         * every run spans sub-maps and cuts land inside runs.  Plain mode
         * orders a run by member, pool mode by pool id; both must keep
         * rows strictly ascending, iterate in score order, start a score
         * scan at the run's first row, find and rank every member, and
         * remove every member (scattered) back to empty */
        for (int pooled = 0; pooled < 2; pooled++) {
            multiOrderedSetFull *m = pooled
                                         ? multiOrderedSetFullNewWithOwnedPool()
                                         : multiOrderedSetFullNew();
            const uint32_t n = 4000;
            for (uint32_t i = 0; i < n; i++) {
                /* scattered: 2654435761 is coprime with n */
                const uint32_t j = (uint32_t)(((uint64_t)i * 2654435761u) % n);
                char buf[24];
                snprintf(buf, sizeof(buf), "run-%05u", j);
                const databox member = databoxNewBytesString(buf);
                const databox score = databoxNewSigned(j % 10);
                multiOrderedSetFullAdd(m, &score, &member);
            }
            if (multiOrderedSetFullCount(m) != n || m->mapCount < 10 ||
                mosFullCheckInvariants(m) != 0) {
                ERR("pooled %d: %zu members over %u sub-maps after adds",
                    pooled, multiOrderedSetFullCount(m), m->mapCount);
            }

            /* A scan at each score starts at its run's first row: the
             * smallest member (plain) or a member of that score (pool) */
            for (int64_t s = 0; s < 10; s++) {
                const databox at = databoxNewSigned(s);
                mosIterator it;
                databox member;
                databox score;
                if (!multiOrderedSetFullIteratorInitAtScore(m, &it, &at,
                                                            true) ||
                    !multiOrderedSetFullIteratorNext(&it, &member, &score) ||
                    score.data.i64 != s) {
                    ERR("pooled %d: score scan at %" PRId64 " missed", pooled,
                        s);
                    continue;
                }
                const int64_t rank = multiOrderedSetFullGetRank(m, &member);
                if (rank != s * 400) {
                    ERR("pooled %d: the run at %" PRId64 " starts at rank "
                        "%" PRId64 ", want %" PRId64,
                        pooled, s, rank, s * 400);
                }
            }

            for (uint32_t i = 0; i < n; i++) {
                const uint32_t j = (i * 40503u + 7) % n; /* scattered again */
                char buf[24];
                snprintf(buf, sizeof(buf), "run-%05u", j);
                const databox member = databoxNewBytesString(buf);
                databox score;
                if (!multiOrderedSetFullGetScore(m, &member, &score) ||
                    score.data.i64 != (int64_t)(j % 10) ||
                    multiOrderedSetFullGetRank(m, &member) < 0 ||
                    !multiOrderedSetFullRemove(m, &member)) {
                    ERR("pooled %d: member %u not found or not removed", pooled,
                        j);
                    break;
                }
                if (i % 500 == 0 && mosFullCheckInvariants(m) != 0) {
                    ERR("pooled %d: invariants broken after %u removals",
                        pooled, i);
                    break;
                }
            }
            if (multiOrderedSetFullCount(m) != 0 ||
                mosFullCheckInvariants(m) != 0) {
                ERR("pooled %d: %zu members left", pooled,
                    multiOrderedSetFullCount(m));
            }

            /* Refilled: counts and a range removal over runs that span
             * sub-maps, with both bounds inclusive and exclusive */
            for (uint32_t j = 0; j < n; j++) {
                char buf[24];
                snprintf(buf, sizeof(buf), "run-%05u", j);
                const databox member = databoxNewBytesString(buf);
                const databox score = databoxNewSigned(j % 10);
                multiOrderedSetFullAdd(m, &score, &member);
            }
            for (int64_t s = 0; s < 10; s++) {
                const mosRangeSpec one = {.min = databoxNewSigned(s),
                                          .max = databoxNewSigned(s)};
                const mosRangeSpec openLow = {.min = databoxNewSigned(s),
                                              .max = databoxNewSigned(s + 1),
                                              .minExclusive = true};
                const mosRangeSpec openHigh = {.min = databoxNewSigned(s),
                                               .max = databoxNewSigned(s + 1),
                                               .maxExclusive = true};
                const size_t wantLow = s < 9 ? 400 : 0;
                if (multiOrderedSetFullCountByScore(m, &one) != 400 ||
                    multiOrderedSetFullCountByScore(m, &openLow) != wantLow ||
                    multiOrderedSetFullCountByScore(m, &openHigh) != 400) {
                    ERR("pooled %d: counts at score %" PRId64 " are %zu, %zu, "
                        "%zu",
                        pooled, s, multiOrderedSetFullCountByScore(m, &one),
                        multiOrderedSetFullCountByScore(m, &openLow),
                        multiOrderedSetFullCountByScore(m, &openHigh));
                }
            }
            const mosRangeSpec middle = {.min = databoxNewSigned(3),
                                         .max = databoxNewSigned(5)};
            const mosRangeSpec all = {.min = databoxNewSigned(0),
                                      .max = databoxNewSigned(9)};
            if (multiOrderedSetFullRemoveRangeByScore(m, &middle) != 1200 ||
                multiOrderedSetFullCountByScore(m, &middle) != 0 ||
                multiOrderedSetFullCountByScore(m, &all) != n - 1200 ||
                multiOrderedSetFullCount(m) != n - 1200 ||
                mosFullCheckInvariants(m) != 0) {
                ERR("pooled %d: removing scores 3..5 left %zu members", pooled,
                    multiOrderedSetFullCount(m));
            }

            /* Left: scores 0-2 then 6-9, 400 rows each, so ranks
             * 2000..2399 are exactly the score-8 run */
            const mosRangeSpec eight = {.min = databoxNewSigned(8),
                                        .max = databoxNewSigned(8)};
            static const struct {
                int64_t rank;
                int64_t score;
            } around[] = {{0, 0},    {1199, 2}, {1200, 6}, {1999, 7},
                          {2000, 9}, {2399, 9}, {-1, 9},   {-400, 9},
                          {-401, 7}, {-2400, 0}};
            if (multiOrderedSetFullRemoveRangeByRank(m, 2000, 2399) != 400 ||
                multiOrderedSetFullCountByScore(m, &eight) != 0 ||
                multiOrderedSetFullCount(m) != n - 1600 ||
                mosFullCheckInvariants(m) != 0) {
                ERR("pooled %d: removing ranks 2000..2399 left %zu members",
                    pooled, multiOrderedSetFullCount(m));
            }
            for (size_t a = 0; a < countof(around); a++) {
                databox member;
                databox score;
                if (!multiOrderedSetFullGetByRank(m, around[a].rank, &member,
                                                  &score) ||
                    score.data.i64 != around[a].score) {
                    ERR("pooled %d: rank %" PRId64 " has score %" PRId64
                        ", want %" PRId64,
                        pooled, around[a].rank, score.data.i64,
                        around[a].score);
                }
            }

            /* Popping 500 from the bottom: the score-0 run (400 rows over
             * several sub-maps), then 100 of score 1, each owned */
            databox popMembers[500];
            databox popScores[500];
            const size_t popped =
                multiOrderedSetFullPopMin(m, 500, popMembers, popScores);
            bool popOrder = popped == 500;
            for (size_t p = 0; p < popped; p++) {
                const int64_t want = p < 400 ? 0 : 1;
                popOrder = popOrder && popScores[p].data.i64 == want &&
                           databoxLen(&popMembers[p]) > 0 &&
                           !multiOrderedSetFullExists(m, &popMembers[p]);
                databoxFreeData(&popMembers[p]);
                databoxFreeData(&popScores[p]);
            }
            if (!popOrder || multiOrderedSetFullCount(m) != n - 2100 ||
                mosFullCheckInvariants(m) != 0) {
                ERR("pooled %d: PopMin gave %zu in the wrong order or left "
                    "%zu",
                    pooled, popped, multiOrderedSetFullCount(m));
            }
            multiOrderedSetFullFree(m);
        }
    }

    TEST("MOS-Full: copies of plain, owned-pool and shared-pool sets") {
        /* 3000 members over many sub-maps, then a score band, a rank band
         * and both ends removed (emptying middle sub-maps); the copy,
         * checked after its source is freed, answers every read as the
         * oracle does, and keeps working when written to */
        /* @own: caller; free=multimapAtomFree */
        multimapAtom *shared = multimapAtomNew();
        static const char *kinds[] = {"plain", "owned pool", "shared pool"};
        for (int kind = 0; kind < 3; kind++) {
            /* @own: caller; free=multiOrderedSetFullFree */
            multiOrderedSetFull *m =
                kind == 0   ? multiOrderedSetFullNew()
                : kind == 1 ? multiOrderedSetFullNewWithOwnedPool()
                            : multiOrderedSetFullNewWithPool(shared);
            bool present[MOS_COPY_N] = {};
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
        multimapAtomFree(shared);
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

    TEST("multiOrderedSetFull — atom table mode, compact and fast tables") {
        for (size_t t = 0; t < countof(mosTestPoolConfigs_); t++) {
            const char *name = mosTestPoolConfigs_[t].name;
            multiOrderedSetFull *mos =
                multiOrderedSetFullNewWithOwnedPoolConfig(
                    &mosTestPoolConfigs_[t].config);
            if (!mos) {
                ERR("[%s] Failed to create multiOrderedSetFull with pool",
                    name);
                continue;
            }

            for (int i = 0; i < 100; i++) {
                databox score = databoxNewSigned(i * 10);
                char buf[32];
                snprintf(buf, sizeof(buf), "poolmember%03d", i);
                databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
                multiOrderedSetFullAdd(mos, &score, &member);
            }

            if (multiOrderedSetFullCount(mos) != 100) {
                ERR("[%s] Pool mode: Count should be 100, got %zu", name,
                    multiOrderedSetFullCount(mos));
            }

            for (int i = 0; i < 100; i++) {
                char buf[32];
                snprintf(buf, sizeof(buf), "poolmember%03d", i);
                databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
                databox score;
                if (!multiOrderedSetFullGetScore(mos, &member, &score) ||
                    score.data.i != i * 10) {
                    ERR("[%s] Pool mode: poolmember%03d score wrong", name, i);
                }
            }

            /* Iteration and ranks hand back the members themselves */
            mosIterator iter;
            multiOrderedSetFullIteratorInit(mos, &iter, true);
            int count = 0;
            databox m, s;
            while (multiOrderedSetFullIteratorNext(&iter, &m, &s)) {
                if (m.type != DATABOX_BYTES) {
                    ERR("[%s] Pool mode iteration: member should be BYTES, got "
                        "%d",
                        name, m.type);
                }
                count++;
            }
            if (count != 100) {
                ERR("[%s] Pool mode iteration: should iterate 100, got %d",
                    name, count);
            }

            for (int i = 0; i < 100; i++) {
                if (!multiOrderedSetFullGetByRank(mos, i, &m, &s) ||
                    m.type != DATABOX_BYTES) {
                    ERR("[%s] Pool mode: GetByRank(%d) failed", name, i);
                }
            }

            multiOrderedSetFullFree(mos);
        }
    }

    TEST("multiOrderedSetFull — inline and atom table members: bytes and "
         "times") {
        /* The same members inline and in each atom table, every length:
         * each set holds them all, and the bytes and times are reported */
        static const size_t lengths[] = {16, 36, 48, 64};
        constexpr size_t N = 5000;
        for (size_t st = 0; st < countof(lengths); st++) {
            const size_t strLen = lengths[st];
            printf("--- %zu-byte members, %zu entries ---\n", strLen, N);
            multiOrderedSetFull *sets[1 + countof(mosTestPoolConfigs_)];
            const char *names[1 + countof(mosTestPoolConfigs_)];
            sets[0] = multiOrderedSetFullNew();
            names[0] = "inline";
            for (size_t t = 0; t < countof(mosTestPoolConfigs_); t++) {
                sets[1 + t] = multiOrderedSetFullNewWithOwnedPoolConfig(
                    &mosTestPoolConfigs_[t].config);
                names[1 + t] = mosTestPoolConfigs_[t].name;
            }

            for (size_t v = 0; v < countof(sets); v++) {
                char buf[128];
                const uint64_t addStart = perfNowNs();
                for (size_t i = 0; i < N; i++) {
                    databox score = databoxNewUnsigned(i);
                    memset(buf, 'A' + (i % 26), strLen);
                    snprintf(buf, strLen + 1, "m%0*zu", (int)(strLen - 2), i);
                    databox member = databoxNewBytesAllowEmbed(buf, strLen);
                    multiOrderedSetFullAdd(sets[v], &score, &member);
                }
                const uint64_t addNs = perfNowNs() - addStart;

                const uint64_t findStart = perfNowNs();
                size_t found = 0;
                for (size_t i = 0; i < N; i++) {
                    memset(buf, 'A' + (i % 26), strLen);
                    snprintf(buf, strLen + 1, "m%0*zu", (int)(strLen - 2), i);
                    databox member = databoxNewBytesAllowEmbed(buf, strLen);
                    databox score;
                    found +=
                        multiOrderedSetFullGetScore(sets[v], &member, &score) &&
                        score.data.u == i;
                }
                const uint64_t findNs = perfNowNs() - findStart;

                printf("    %-8s %6.1f bytes/entry  add %6.1f ns  find %6.1f "
                       "ns\n",
                       names[v],
                       (double)multiOrderedSetFullBytes(sets[v]) / (double)N,
                       (double)addNs / (double)N, (double)findNs / (double)N);
                if (found != N || multiOrderedSetFullCount(sets[v]) != N) {
                    ERR("%s set of %zu-byte members: %zu of %zu found",
                        names[v], strLen, found, N);
                }

                multiOrderedSetFullFree(sets[v]);
            }
        }
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

    TEST("MOS-Full: score updates in place and moved agree with an oracle") {
        /* 3000 members on spread scores (100 apart, so small deltas keep
         * a row in place) take 20000 updates: mostly small IncrBy and Add
         * of a nearby score, now and then a far jump or a score equal to
         * another member's.  Every 2000 updates: every score, the order
         * (strictly ascending (score, member)), the count and the sub-map
         * invariants */
        constexpr int64_t members = 3000;
        multiOrderedSetFull *m = multiOrderedSetFullNew();
        static int64_t oracle[3000];
        char name[24];
        for (int64_t i = 0; i < members; i++) {
            mosfName_(name, sizeof(name), i);
            oracle[i] = i * 100;
            const databox member = databoxNewBytesString(name);
            const databox score = databoxNewSigned(oracle[i]);
            multiOrderedSetFullAdd(m, &score, &member);
        }

        uint64_t rng = 0x5C0E;
        for (int step = 0; step < 20000 && !err; step++) {
            const int64_t i = (int64_t)(splitmix64(&rng) % members);
            mosfName_(name, sizeof(name), i);
            const databox member = databoxNewBytesString(name);
            const uint64_t roll = splitmix64(&rng) % 20;
            if (roll < 12) {
                const databox delta =
                    databoxNewSigned((int64_t)(splitmix64(&rng) % 61) - 30);
                databox result = {};
                multiOrderedSetFullCombine(m, &delta, &member, MOS_COMBINE_INCR,
                                           &result);
                oracle[i] += delta.data.i64;
            } else {
                const int64_t to =
                    roll < 17
                        ? oracle[i] + (int64_t)(splitmix64(&rng) % 41) - 20
                    : roll < 19
                        ? (int64_t)(splitmix64(&rng) % (members * 100))
                        : oracle[splitmix64(&rng) % members]; /* a tie */
                const databox score = databoxNewSigned(to);
                multiOrderedSetFullAdd(m, &score, &member);
                oracle[i] = to;
            }

            if (step % 2000 != 1999) {
                continue;
            }

            for (int64_t k = 0; k < members && !err; k++) {
                mosfName_(name, sizeof(name), k);
                const databox member2 = databoxNewBytesString(name);
                databox score = {};
                if (!multiOrderedSetFullGetScore(m, &member2, &score) ||
                    score.data.i64 != oracle[k]) {
                    ERR("step %d: member %" PRId64 " scores %" PRId64
                        ", oracle %" PRId64,
                        step, k, score.data.i64, oracle[k]);
                }
            }

            mosIterator iter;
            multiOrderedSetFullIteratorInit(m, &iter, true);
            databox prevMember = {};
            databox prevScore = {};
            size_t seen = 0;
            databox gotMember;
            databox gotScore;
            while (
                multiOrderedSetFullIteratorNext(&iter, &gotMember, &gotScore)) {
                if (seen && (databoxCompare(&prevScore, &gotScore) > 0 ||
                             (databoxCompare(&prevScore, &gotScore) == 0 &&
                              databoxCompare(&prevMember, &gotMember) >= 0))) {
                    ERR("step %d: rows out of order at %zu", step, seen);
                    break;
                }
                prevMember = gotMember;
                prevScore = gotScore;
                seen++;
            }

            if (seen != (size_t)members ||
                multiOrderedSetFullCount(m) != (size_t)members ||
                mosFullCheckInvariants(m)) {
                ERR("step %d: %zu rows walked, count %zu, invariants %d", step,
                    seen, multiOrderedSetFullCount(m),
                    mosFullCheckInvariants(m));
            }
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
            ERR("invariant oracle reported violations (ascending)");
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
            dkLcg64Next(&s);
            int key = (int)(s >> 40) % 4096;
            databox score = databoxNewSigned(key % 50);
            char buf[24];
            int n = snprintf(buf, sizeof(buf), "k%08d", key);
            databox member = databoxNewBytesAllowEmbed(buf, (size_t)n);
            multiOrderedSetFullAdd(m, &score, &member);
        }
        if (mosFullCheckInvariants(m) != 0) {
            ERR("invariant oracle reported violations (random/dup scores)");
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
            ERR("invariant oracle reported violations after splits");
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
                ERR("GetScore m%06d expected %d got %" PRIdMAX, i, want,
                    got.data.i);
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
            ERR("invariants violated after re-add");
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
            ERR("invariants violated with 96/128-bit heads");
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
            ERR("invariants violated after wide range removal");
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
            ERR("narrow head should own nothing");
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
            ERR("wide head did not get an owned payload");
        }

        /* wide -> wide: the same payload is rewritten in place */
        databoxBig s3;
        memset(&s3, 0, sizeof(s3));
        DATABOX_BIG_SIGNED_128(&s3, w2);
        const databox m3 = databoxNewSigned(3);
        multiOrderedSetFullAdd(m, (const databox *)&s3, &m3);
        rb = getRangeBox(m, 0);
        if (rb->data.ptr != payload || *rb->data.i128 != w2) {
            ERR("wide head refresh did not reuse its payload");
        }

        multiOrderedSetFullRemove(m, &m3);
        rb = getRangeBox(m, 0);
        if (rb->data.ptr != payload || *rb->data.i128 != w1) {
            ERR("wide head after removal did not reuse its payload");
        }

        /* wide -> narrow: the payload is freed (the leak checker sees it
         * otherwise) and the flags are cleared */
        multiOrderedSetFullRemove(m, &m2);
        rb = getRangeBox(m, 0);
        if (mosBoxIsWide(rb) || rb->allocated || rb->big ||
            rb->data.i64 != 10) {
            ERR("narrow head after a wide one kept a payload");
        }

        /* wide again, then drop it through the empty-map sentinel and Free */
        multiOrderedSetFullAdd(m, (const databox *)&s2, &m2);
        multiOrderedSetFullRemove(m, &m1);
        if (mosFullCheckInvariants(m) != 0) {
            ERR("invariants violated across head transitions");
        }
        multiOrderedSetFullRemove(m, &m2);
        rb = getRangeBox(m, 0);
        if (rb->type != DATABOX_SIGNED_64 || rb->data.i64 != INT64_MAX ||
            rb->allocated) {
            ERR("empty sub-map should hold the narrow sentinel");
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

    TEST("MOS-Full: an unknown pool setting is refused; a pooled set keeps "
         "its rows") {
        const multimapAtomConfig bad = {.store = (multimapAtomStore)9};
        if (multiOrderedSetFullNewWithOwnedPoolConfig(&bad)) {
            ERR("a pool with an unknown store was made");
        }

        /* TakeRows refuses a pooled set (its rows hold pool ids), which
         * stays whole */
        multiOrderedSetFull *m = multiOrderedSetFullNewWithOwnedPool();
        for (int i = 0; i < 20; i++) {
            const databox score = databoxNewSigned(i);
            char name[16];
            snprintf(name, sizeof(name), "member-%d", i);
            const databox member = databoxNewBytesString(name);
            multiOrderedSetFullAdd(m, &score, &member);
        }
        if (multiOrderedSetFullTakeRows(m) ||
            multiOrderedSetFullCount(m) != 20) {
            ERR("TakeRows took a pooled set's rows");
        }
        multiOrderedSetFullFree(m);
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
