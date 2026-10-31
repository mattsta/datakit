#include "multimapFull.h"
#include "cleaner.h"
#include "dkRandom.h"
#include "multimapFullInternal.h"
#include "multimapInternal.h"

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleanerName(const flex **, zfree, MapPtrs);
defcleanerName(flex **, zfree, Maps);
defcleanerName(multimapFullMiddle *, zfree, Middles);
defcleaner(multimapFull *, multimapFullFree);
defcleaner(flex *, flexFree);

/* ====================================================================
 * Management Macros
 * ==================================================================== */
/* Data structure size allocation maximums. */
/* Larger numbers == faster lookups but slower inserts.
 * You want a balance between size of reallocation required per insert
 * and size of contiguous blocks of data for lookup binary search. */
/* You can empirically discover correct numbers based on your workload
 * and platform by searching an exponential cache space then running
 * your workload for performance comparisons.
 * By default, we tune for 2016-era general purpose architectures
 * with per-CPU 32k L1 data caches, 256k L2 cache, and multi-MB
 * shared L3 caches (4 MB to 45 MB) */

#define NOISY_MAPPING 0

#if NOISY_MAPPING
#include <stdio.h>
#endif

static constexpr int CACHE_FILL = 65536;
#define MAP_STORAGE_MAX (CACHE_FILL / (sizeof(flex *)))
#define MIDDLE_STORAGE_MAX (CACHE_FILL / sizeof(multimapFullMiddle))
#define RANGEBOX_STORAGE_MAX (CACHE_FILL / (sizeof(databox)))

/* Create new array entries at 'idx' and populate them with values */
#define reallocIncrCount(m, idx, newMap, newMiddle, newRangeBox)               \
    do {                                                                       \
        uint32_t enterCount_ = (m)->count;                                     \
        multiarrayNativeInsert((m)->map, flex *, MAP_STORAGE_MAX, enterCount_, \
                               idx, &(newMap));                                \
        /* Hacky decrement because NativeInsert increments count for us, but   \
         * we only have one shared counter across all arrays. */               \
        enterCount_ = (m)->count;                                              \
        uint32_t middle_ = (newMiddle);                                        \
        multiarrayNativeInsert((m)->middle, multimapFullMiddle,                \
                               MIDDLE_STORAGE_MAX, enterCount_, idx,           \
                               &middle_);                                      \
        if ((idx) > 0) {                                                       \
            assert((m)->count > 0);                                            \
            /* Use (count - 1) because there's no rangeBox for map[0] */       \
            enterCount_ = (m)->count - 1;                                      \
            multiarrayNativeInsert((m)->rangeBox, databox,                     \
                                   RANGEBOX_STORAGE_MAX, enterCount_,          \
                                   (idx) - 1, &(newRangeBox));                 \
        }                                                                      \
                                                                               \
        (m)->count++;                                                          \
    } while (0)

/* Delete array entries at 'idx' and remove their values. */
#define reallocDecrCount(m, idx)                                               \
    do {                                                                       \
        size_t enterCount_ = (m)->count;                                       \
        multiarrayNativeDelete((m)->map, flex *, enterCount_, idx);            \
        enterCount_ = (m)->count;                                              \
        multiarrayNativeDelete((m)->middle, multimapFullMiddle, enterCount_,   \
                               idx);                                           \
        /* Yes, this is (m)->count > 1 and NOT (idx) > 0 because (idx) > 0     \
         * doesn't properly maintain the range boxes and we end up unable to   \
         * find elements in map[0] eventually (this error is not captured in   \
         * test cases: fix test case by testing with (idx) > 0 here and        \
         * getting a test case to fail with insert->delete->lookup against     \
         * various map configurations. */                                      \
        if ((m)->count > 1) {                                                  \
            /* Use (count - 1) because there's no rangeBox for map[0] */       \
            enterCount_ = (m)->count - 1;                                      \
                                                                               \
            /* The removed rangeBox may own a heap payload (a reference        \
             * map's copied BYTES key or a 96/128-bit key), so it is released  \
             * here for every removal.  Deleting map[0] removes rangeBox[0]    \
             * (the head of the old map[1], which becomes map[0] and needs no  \
             * rangeBox), so that branch releases it too. */                   \
            const size_t whichIdx = (idx) > 0 ? (idx) - 1 : 0;                 \
            databoxFreeData(getRangeBox(m, whichIdx));                         \
            multiarrayNativeDelete((m)->rangeBox, databox, enterCount_,        \
                                   whichIdx);                                  \
        }                                                                      \
                                                                               \
        (m)->count--;                                                          \
    } while (0)

static inline flex *getMapLowest(const multimapFull *m) {
    flex **zzl;
    multiarrayNativeGetHead(m->map, flex *, zzl);
    return *zzl;
}

static inline flex *getMapHighest(const multimapFull *m) {
    flex **zzl;
    multiarrayNativeGetTail(m->map, flex *, zzl, m->count);
    return *zzl;
}

static inline flex **getMapPtr(const multimapFull *m, multimapFullIdx idx) {
    flex **zzl;
    multiarrayNativeGet(m->map, flex *, zzl, idx);
    return zzl;
}

static inline flex *getMap(const multimapFull *m, multimapFullIdx idx) {
    flex **zzl = getMapPtr(m, idx);
    return *zzl;
}

static inline multimapFullMiddle getMiddle(const multimapFull *m,
                                           multimapFullIdx idx) {
    multimapFullMiddle *middle;
    multiarrayNativeGet(m->middle, multimapFullMiddle, middle, idx);
    return *middle;
}

/* Each rangeBox is a 16-byte databox holding the head key of a map after
 * map[0].  A narrow key is held by value or borrows its BYTES from the map
 * (a reference map holds a heap copy instead, allocated = 1).  A 96/128-bit
 * key points at a 16-byte heap payload the map owns (big = 1,
 * allocated = 1).  The multiarray moves the boxes when it inserts, deletes
 * or regroups them, but a heap payload doesn't move, so reading a rangeBox
 * never writes to it. */
static inline databox *getRangeBox(const multimapFull *m, multimapFullIdx idx) {
    databox *box;
    multiarrayNativeGet(m->rangeBox, databox, box, idx);
    return box;
}

DK_INLINE_ALWAYS bool rangeBoxIsWide_(const databox *box) {
    return box->type == DATABOX_SIGNED_128 || box->type == DATABOX_UNSIGNED_128;
}

/* Set rangeBox 'rb' to the decoded head key 'head'.  A wide payload the
 * rangeBox already owns is reused in place, so a refresh only allocates on
 * a narrow-to-wide change and only frees on a wide-to-narrow change.
 * 'ownBytes' copies a BYTES key to the heap (reference maps, whose head
 * decodes to bytes outside the map). */
static void rangeBoxAssign_(databox *const rb, const databox *const head,
                            const bool ownBytes) {
    void *payload = nullptr;
    if (rb->allocated) {
        if (rangeBoxIsWide_(rb)) {
            payload = rb->data.ptr;
        } else {
            databoxFreeData(rb);
        }
    }

    if (unlikely(rangeBoxIsWide_(head))) {
        databoxWidePayloadRequire(head);
        if (!payload) {
            /* @own: container; free=databoxFreeData */
            payload = zmalloc(sizeof(databoxWide));
        }

        memcpy(payload, head->data.ptr, sizeof(databoxWide));
        *rb = *head;
        rb->data.ptr = payload;
        rb->allocated = true;
        rb->big = true;
        return;
    }

    if (unlikely(payload != nullptr)) {
        zfree(payload);
    }

    *rb = *head;
    rb->allocated = false;
    rb->big = false;
    if (ownBytes) {
        (void)databoxAllocateIfNeeded(rb);
    }
}

/* Refresh rangeBox[idx - 1] from the head of 'map'.  A reference map
 * resolves its head through 'referenceContainer' and keeps a heap copy of
 * a BYTES key, because the resolved bytes live outside the map. */
static void rangeBoxRefresh_(multimapFull *m, const multimapFullIdx idx,
                             const flex *map, const bool useReference,
                             const multimapAtom *referenceContainer) {
    databox head;
    databoxWide headWide;
    multimapGetByTypeInto_(flexHead(map), &head, &headWide,
                           useReference ? referenceContainer : nullptr);
    rangeBoxAssign_(getRangeBox(m, idx - 1), &head, useReference);
}

static inline void setMiddle(multimapFull *m, multimapFullIdx idx,
                             multimapFullMiddle middle) {
    multimapFullMiddle *mid;
    multiarrayNativeGet(m->middle, multimapFullMiddle, mid, idx);
    *mid = middle;
}

#define CALCULATE_MIDDLE(middle, map) ((middle) - (map))
#define CALCULATE_MIDDLE_FORCE(m, map)                                         \
    CALCULATE_MIDDLE(flexMiddle(map, (m)->elementsPerEntry), map)

#define GET_MIDDLE(m, idx, map) (flexEntry *)((map) + getMiddle(m, idx))
#define SET_MIDDLE(m, idx, mid, map)                                           \
    do {                                                                       \
        setMiddle(m, idx, CALCULATE_MIDDLE(mid, map));                         \
    } while (0)

#define SET_MIDDLE_FORCE(m, idx, map)                                          \
    do {                                                                       \
        SET_MIDDLE(m, idx, flexMiddle(map, (m)->elementsPerEntry), map);       \
    } while (0)

/* Refreshed after every write that can move a map's head: the refresh
 * decodes one head entry and reuses the box's payload in place, so no
 * caller tracks whether the head changed */
#define updateRangeBoxForIdx(m, idx, map_)                                     \
    do {                                                                       \
        if ((idx) > 0 && flexCount(map_) > 0) {                                \
            /* rangeBox[idx - 1] is for map[idx]; a wide payload it owns is    \
             * reused in place */                                              \
            rangeBoxRefresh_(m, idx, map_, false, nullptr);                    \
        }                                                                      \
    } while (0)

/* Reference Range Boxes are different because they reference bytes NOT INSIDE
 * OUR OWN MAP, so we CAN'T just reference existing bytes, we have to copy bytes
 * for the range box to hold because the underlying data can go away at any */
#define updateRangeBoxForIdxWithReference(m, idx, map_, ref)                   \
    do {                                                                       \
        /* Only a map with rows: a map that reaches zero rows is deleted       \
         * after this update, so its range box goes with it. */                \
        if ((idx) > 0 && flexCount(map_) > 0) {                                \
            /* rangeBox[idx - 1] is for map[idx].  It keeps its own copy of    \
             * the resolved key (BYTES on the heap, a wide key in its owned    \
             * payload), since a reference names data outside 'map_'. */       \
            rangeBoxRefresh_(m, idx, map_, true, ref);                         \
        }                                                                      \
    } while (0)

/* Refresh a rangeBox after an in-map entry mutation (resize / replace /
 * incr of a VALUE field) that may have realloc'd the flex or shifted its
 * bytes (total-size varint header growth moves every entry).  The head
 * KEY entry is unchanged by a value mutation, but a non-surrogate
 * rangeBox holds a databox REFERENCING the head key's bytes inside the
 * flex allocation, so any realloc/shift strands it — re-read it from the
 * (possibly moved) head.
 *
 * Surrogate-keyed maps are the opposite case and are detected from the
 * data itself: their head entry decodes as a reference box, while their
 * rangeBox holds the RESOLVED key as an allocation-independent copy
 * (updateRangeBoxForIdxWithReference + databoxAllocateIfNeeded).  That
 * copy survives any flex realloc and CANNOT be re-derived without the
 * reference container, so it must be left alone — overwriting it with
 * the raw reference box would corrupt range binary search and leak the
 * copy's allocation.  rangeBoxAssign_ releases an owned BYTES copy and
 * reuses an owned wide payload, so the refresh is leak-free even for a
 * map whose rangeBox was once a copy. */
#define updateRangeBoxForIdxAfterInMapMutation(m, idx, map_)                   \
    do {                                                                       \
        if ((idx) > 0 && flexCount(map_) > 0) {                                \
            databox mutatedHead_;                                              \
            databoxWide mutatedHeadWide_;                                      \
            flexGetByTypeInto(flexHead(map_), &mutatedHead_,                   \
                              &mutatedHeadWide_);                              \
            if (mutatedHead_.type != DATABOX_CONTAINER_REFERENCE_EXTERNAL) {   \
                /* rangeBox[idx - 1] is for map[idx].  The assign sets every   \
                 * flag bit itself and copies a wide key into the payload the  \
                 * rangeBox owns. */                                           \
                rangeBoxAssign_(getRangeBox(m, (idx) - 1), &mutatedHead_,      \
                                false);                                        \
            }                                                                  \
        }                                                                      \
    } while (0)

#define nextMapIdxExists(m, mapIdx) ((mapIdx) < ((m)->count - 1))

/* ====================================================================
 * Creation
 * ==================================================================== */
static const multimapFull multimapFullNull = {};
static const databox rangeBoxNull = {};

multimapFull *multimapFullNew(multimapElements elementsPerEntry) {
    /* @own: caller; free=multimapFullFree */
    multimapFull *m = zcalloc(1, sizeof(*m));

    m->elementsPerEntry = elementsPerEntry; /* default: key->value map */
    m->maxSize = 2048;

    /* Create initial map as empty so we're ready to go
     * for new inserts. */
    /* @own: container; free=multimapFullFree */
    flex *newMap = flexNew();

    /* For an empty map, the "middle" is equivalent
     * to the head of an empty flex. */
    reallocIncrCount(m, (uint32_t)0, newMap, FLEX_EMPTY_SIZE, rangeBoxNull);
    assert(m->count == 1);

    /* We don't need to create rangeBox[0] because rangeBoxes
     * are offset by one (because we can assume anything less
     * than map[1] is in map[0], we don't need a rangeBox for
     * map[0] itself; plus, if we only have one map, we also don't
     * need any range boxes because there is no 'range'). */

    return m;
}

multimapFull *multimapFullSetNew(multimapElements elementsPerEntry,
                                 uint32_t maxSize) {
    /* @own: caller; free=multimapFullFree */
    multimapFull *m = multimapFullNew(elementsPerEntry);
    m->mapIsSet = true;
    m->maxSize = maxSize;
    return m;
}

DK_INLINE_ALWAYS void accountForNewMapAfterExistingMap_(
    multimapFull *m, const multimapFullIdx mapIdxMapBefore,
    const bool useReference, const multimapAtom *referenceContainer,
    flex *mapBefore, flex *mapAfter) {
    /* mapIdxMapB is the NEW index of MapB which depends on if
     * we are inserting BEFORE A or AFTER A */
    /* A is before B.
     * B is higher. */
    const multimapFullIdx mapIdxMapAfter = mapIdxMapBefore + 1;

    /* Open new position in our arrays for: map, middle, rangeBox */
    reallocIncrCount(m, mapIdxMapAfter, mapAfter,
                     CALCULATE_MIDDLE_FORCE(m, mapAfter), rangeBoxNull);

    /* Set previous position to our before map */
    *getMapPtr(m, mapIdxMapBefore) = mapBefore;

#if NOISY_MAPPING
    printf("[%p] Allocating new map...\n", (void *)m);
#endif

    /* Update middle for 'map' (we updated middle for 'higher' when we
     * inserted the 'higher' map above into the m->map multiarray). */
    SET_MIDDLE_FORCE(m, mapIdxMapBefore, mapBefore);

    /* Update range boxes */
    if (useReference) {
        /* NOTE: If this map is using references for keys, we store the
         *       *actual key* in the rangeBox (i.e. we *do not* store references
         *       in range boxes since we can't reliably compare references,
         *       only data itself). */
        updateRangeBoxForIdxWithReference(m, mapIdxMapAfter, mapAfter,
                                          referenceContainer);
        updateRangeBoxForIdxWithReference(m, mapIdxMapBefore, mapBefore,
                                          referenceContainer);
    } else {
        updateRangeBoxForIdx(m, mapIdxMapAfter, mapAfter);
        updateRangeBoxForIdx(m, mapIdxMapBefore, mapBefore);
    }
}

DK_INLINE_ALWAYS void splitMapLowHigh_(multimapFull *m, multimapFullIdx mapIdx,
                                       flex **map, const bool useReference,
                                       const multimapAtom *referenceContainer) {
    /* Split existing map; get [LOW, HIGH]. */
    flex *higher =
        flexSplitMiddle(map, m->elementsPerEntry, GET_MIDDLE(m, mapIdx, *map));

#if 0
    printf("Low, High after split:\n");
    flexRepr(*map);
    flexRepr(higher);
#endif

    accountForNewMapAfterExistingMap_(m, mapIdx, useReference,
                                      referenceContainer, *map, higher);
}

DK_INLINE_ALWAYS size_t mapNewBeforeExisting_(
    multimapFull *m, multimapFullIdx mapIdxHigher, flex *higher,
    const bool useReference, const multimapAtom *referenceContainer) {
    /* If mapIdxHigher == 0, we can't place 'before' 0, so we need to
     * just grow after Higher then move Higher up one, then replace
     * the low position with an empty map. */
    /* This is basically: (mapIdxHigher - 1) || 0 */
    size_t mapIdxLower = (mapIdxHigher ?: 1) - 1;

    /* Place new map 'lower' before existing map 'higher' */
    /* @own: container; free=multimapFullFree */
    flex *lower = flexNew();

    accountForNewMapAfterExistingMap_(m, mapIdxLower, useReference,
                                      referenceContainer, lower, higher);

    return mapIdxLower;
}

DK_INLINE_ALWAYS void
mapNewAfterExisting_(multimapFull *m, multimapFullIdx mapIdxLower, flex *lower,
                     const bool useReference,
                     const multimapAtom *referenceContainer) {
    /* Place new map 'higher' AFTER existing map 'lower' */
    /* @own: container; free=multimapFullFree */
    flex *higher = flexNew();

    accountForNewMapAfterExistingMap_(m, mapIdxLower, useReference,
                                      referenceContainer, lower, higher);
}

#if 0
/* Iterate over all maps and conform them under maxSize */
DK_FN_UNUSED DK_STATIC void conformMapsSplit(multimapFull *m) {
    /* Note: 'm->count' gets updated (grows) during this iteration, so when we
     * split '0' into '0, 1', we then iterate over what we just split
     * and the count increases. */
    multimapFullIdx i = 0;
    while (i < m->count) {
        flex **map = getMapPtr(m, i);
        /* Split map if both:
         *   - map has more than one entry (if map only has one entry we can't
         *     split it no matter how big it is)
         *   - map is above max map size. */
        if (flexCount(*map) > m->elementsPerEntry &&
            flexBytes(*map) > m->maxSize) {
            splitMap(m, i, map);

            /* Don't increment index here because we need to re-evaluate the
             * lowest split position to verify it is actually under the max.
             * We may need to split multiple times. */
        } else {
            /* We didn't split, so try the next map */
            i++;
        }
    }
}

#endif

DK_INLINE_ALWAYS void mergeMaps(multimapFull *m, multimapFullIdx mapIdx,
                                flex **map, flex **mapNext,
                                const bool useReference,
                                const multimapAtom *referenceContainer) {
    /* Merge 'map' and 'mapNext' */
    flexBulkAppendFlex(map, *mapNext);
    flexFree(*mapNext);

    /* Update middle */
    SET_MIDDLE_FORCE(m, mapIdx, *map);

    /* Update range box */
    if (useReference) {
        updateRangeBoxForIdxWithReference(m, mapIdx, *map, referenceContainer);
    } else {
        updateRangeBoxForIdx(m, mapIdx, *map);
    }

    /* Delete old slot metadata for mapIdx + 1 */
    reallocDecrCount(m, mapIdx + 1);
}

DK_INLINE_ALWAYS bool mergeSimple(multimapFull *m, multimapFullIdx mapIdx,
                                  const bool useReference,
                                  const multimapAtom *referenceContainer) {
    flex **map = getMapPtr(m, mapIdx);
    flex **mapNext = getMapPtr(m, mapIdx + 1);
    /* Merge maps if i and i+1 are, combined, below our max size. */
    if ((flexBytes(*map) + flexBytes(*mapNext)) <= m->maxSize) {
        mergeMaps(m, mapIdx, map, mapNext, useReference, referenceContainer);
        return true;
    }

    return false;
}

/* Bytes a refill leaves unused below maxSize: room for the flex header to
 * grow as its byte count does, so a refilled map is never over the limit */
static constexpr int MULTIMAP_FULL_COMPACT_SLACK = 8;

/* Fill every inner map, left to right, to its limit with the leading rows
 * of the map after it; a map whose rows all fit joins its neighbour.  The
 * rows and their order are unchanged, only where maps begin moves, so
 * every map but the last ends within one row of maxSize.  A map whose
 * head moved gets its range box refreshed (through the references for a
 * map of references). */
DK_INLINE_ALWAYS void
multimapFullCompact_(multimapFull *m, const bool useReference,
                     const multimapAtom *referenceContainer) {
    const multimapElements elements = m->elementsPerEntry;
    /* Map 'idx' took rows, or lost its first ones: its range box borrows
     * its head's bytes, and an append can move the map */
#define COMPACT_REFRESH_(idx, map_)                                            \
    do {                                                                       \
        if (useReference) {                                                    \
            updateRangeBoxForIdxWithReference(m, idx, map_,                    \
                                              referenceContainer);             \
        } else {                                                               \
            updateRangeBoxForIdx(m, idx, map_);                                \
        }                                                                      \
    } while (0)
    multimapFullIdx i = 0;
    while (i + 1 < m->count) {
        flex **map = getMapPtr(m, i);
        flex **next = getMapPtr(m, i + 1);
        const size_t have = flexBytes(*map);
        if (have + flexBytes(*next) <= m->maxSize) {
            flexBulkAppendFlex(map, *next);
            flexFree(*next);
            SET_MIDDLE_FORCE(m, i, *map);
            reallocDecrCount(m, i + 1);
            COMPACT_REFRESH_(i, *getMapPtr(m, i));
            continue;
        }

        /* The most leading rows of 'next' that fit in the room left (it
         * keeps at least one row: all of them fitting merged above) */
        const size_t room =
            m->maxSize > have + MULTIMAP_FULL_COMPACT_SLACK
                ? m->maxSize - have - MULTIMAP_FULL_COMPACT_SLACK
                : 0;
        const size_t total = flexCount(*next) / elements;
        const uint8_t *head = (const uint8_t *)flexHead(*next);
        flexEntry *fe = flexHead(*next);
        size_t rows = 0;
        while (rows + 1 < total) {
            flexEntry *after = fe;
            for (multimapElements e = 0; e < elements; e++) {
                after = flexNext(*next, after);
            }
            if ((size_t)((const uint8_t *)after - head) > room) {
                break;
            }
            fe = after;
            rows++;
        }

        if (rows) {
            flex *moved = flexSplitRange(next, 0, (uint32_t)(rows * elements));
            flexBulkAppendFlex(map, moved);
            flexFree(moved);
            SET_MIDDLE_FORCE(m, i, *map);
            SET_MIDDLE_FORCE(m, i + 1, *next);
            COMPACT_REFRESH_(i, *map);
            COMPACT_REFRESH_(i + 1, *next);
        }

        i++;
    }
#undef COMPACT_REFRESH_
}

void multimapFullCompact(multimapFull *m) {
    multimapFullCompact_(m, false, nullptr);
}

void multimapFullCompactWithReference(multimapFull *m,
                                      const multimapAtom *referenceContainer) {
    multimapFullCompact_(m, true, referenceContainer);
}

/* Iterate over all maps and conform them (pairwise) to <= maxSize */
#if 0
DK_FN_UNUSED DK_STATIC void conformMapsMerge(multimapFull *m) {
    /* Note: 'm->count' gets updated (shrinks) during this iteration, so when we
     * merge '0, 1' into '0', we then iterate over 0 again but m->count has
     * also shrunk by one. */
    multimapFullIdx i = 0;
    while (nextMapIdxExists(m, i)) {
        if (mergeSimple(m, i)) {
            /* Don't increment index here because we need to re-evaluate the
             * lowest merge position to check if it can merge again with
             * it's new next neighbor.
             * We may need to merge the same index multiple times. */
        } else {
            /* We didn't merge, so try the next map pair */
            i++;
        }
    }
}

#endif

/* If you pass in NULL for 'm_', then you get a new multimapFull.  Otherwise,
 * we realloc 'm_' to be the new multimapFull container.
 * Note: this means the pointers to *map[] and middle[] must NOT be pointers
 * inside of the struct 'm_' — if they are, you need to extract the inside
 * pointers and provide a clean array of inside pointers to the function.
 * (otherwise, the struct 'm_' will get rewritten and the array pointers it had
 * inside will be gone before we get a chance to use them!) */
static multimapFull *multimapFullNewFromManyGrow_(
    void *m_, flex *map[], const multimapFullMiddle middle[],
    const size_t count, const multimapElements elementsPerEntry,
    const bool mapIsSet, const bool useReference,
    const multimapAtom *referenceContainer) {
    /* @own: caller; free=multimapFullFree */
    multimapFull *m = zrealloc(m_, sizeof(*m));

    /* We expect calloc'd structs for initialization, so let's pretend... */
    *m = multimapFullNull;

    m->elementsPerEntry = elementsPerEntry;
    m->mapIsSet = mapIsSet;

    /* O(count) */
    for (multimapFullIdx i = 0; i < count; i++) {
        const uint32_t elements = flexCount(map[i]) / elementsPerEntry;
        if (elements) {
            m->values += elements;

            /* If we don't have an incoming middle, calculate the middle. */
            const multimapFullMiddle useMiddle =
                middle[i] > 2
                    ? middle[i]
                    : CALCULATE_MIDDLE(flexMiddle(map[i], elementsPerEntry),
                                       map[i]);

            /* Save some code duplication by setting a NULL rangeBox
             * then updating it after. */
            reallocIncrCount(m, i, map[i], useMiddle, rangeBoxNull);

            /* Range boxes **always** have full values, no references: a
             * reference map resolves its first key through the container */
            if (useReference) {
                updateRangeBoxForIdxWithReference(m, i, map[i],
                                                  referenceContainer);
            } else {
                updateRangeBoxForIdx(m, i, map[i]);
            }
        } else {
            assert(false && "Grew to full map with no contents?");
            unreachable();
        }
    }

    assert(m->count == count);

/* Conformity is commented out because there's actually an argument for
 * keeping too-big maps in the beginning: large maps are quicker to search.
 *
 * When we go to insert into a large map, we'll split it anyway, so for
 * now we can just retain the larger sizes and if the user inserts into them,
 * they will be split on-demand by the insert.
 *
 * (Future optimization: determine which inner maps are read-only and
 *  merge them together into larger "megamap" since we only split
 *  smaller maps for insert performance).
 *
 * This way, we retain higher initial read performance at the expense
 * of split-on-insert which happens normally anyway.
 *
 * The call below allows us to pre-split during the initial upgrade
 * from {small,medium} to Full, but it seems unnecessary *and*, since
 * we do the upgrade inline after a multimapInsert() call, it slows
 * down the general case of upgrading to Full.
 *
 * Current judgement: retain larger (but maxSize violating) initial
 * maps from the upgrade and they will be split on-demand during
 * any future insert operations. */
#if 0
    /* Do an O(count) to O(count^2) loop to verify and/or force
     * newly placed maps under our size limit. */
    /* We could try to combine the size conformity into the initial
     * map placement, but we already have a split functionality created
     * for inserting elements, so let's reuse that without cluttering
     * up the initial placement loop.  This may create an O(count^3)
     * iteration over our total maps, but our initial count is usually 2. */

    /* For the initial split, give us larger maps since we're growing
     * from a larger representation anyway. */
    m->maxSize = 4096;
    conformMapsSplit(m);
#endif

    /* We now return you to your regularly scheduled size classes */
    m->maxSize = 2048;

    return m;
}

multimapFull *multimapFullNewFromManyGrow(
    void *m_, flex *map[], const multimapFullMiddle middle[],
    const size_t count, const multimapElements elementsPerEntry,
    const bool mapIsSet) {
    return multimapFullNewFromManyGrow_(
        m_, map, middle, count, elementsPerEntry, mapIsSet, false, nullptr);
}

multimapFull *multimapFullNewFromManyGrowWithReference(
    void *m_, flex *map[], const multimapFullMiddle middle[],
    const size_t count, const multimapElements elementsPerEntry,
    const bool mapIsSet, const multimapAtom *referenceContainer) {
    return multimapFullNewFromManyGrow_(m_, map, middle, count,
                                        elementsPerEntry, mapIsSet, true,
                                        referenceContainer);
}

multimapFull *multimapFullNewFromTwoGrow(void *m, flex *map[2],
                                         multimapFullMiddle middle[2],
                                         multimapElements elementsPerEntry,
                                         bool mapIsSet) {
    return multimapFullNewFromManyGrow(m, map, middle, 2, elementsPerEntry,
                                       mapIsSet);
}

multimapFull *multimapFullNewFromTwoGrowWithReference(
    void *m, flex *map[2], multimapFullMiddle middle[2],
    multimapElements elementsPerEntry, bool mapIsSet,
    const multimapAtom *referenceContainer) {
    return multimapFullNewFromManyGrowWithReference(
        m, map, middle, 2, elementsPerEntry, mapIsSet, referenceContainer);
}

multimapFull *multimapFullNewFromOneGrow(void *m, flex *one,
                                         multimapFullMiddle mid,
                                         multimapElements elementsPerEntry,
                                         bool mapIsSet) {
    flex *map[1] = {one};
    multimapFullMiddle middle[1] = {mid};
    return multimapFullNewFromManyGrow(m, map, middle, 1, elementsPerEntry,
                                       mapIsSet);
}

size_t multimapFullCount(const multimapFull *m) {
    return m->values;
}

size_t multimapFullNodeCount(const multimapFull *m) {
    return m->count;
}

size_t multimapFullBytes(const multimapFull *m) {
    size_t Bytes = 0;
    for (multimapFullIdx q = 0; q < m->count; q++) {
        Bytes += flexBytes(getMap(m, q));
    }

    /* Note: Bytes is the size of all maps combined, but Bytes does
     * *not* include sizes of:
     *   - the map pointer array
     *   - the rangebox array
     *   - the middle offset array */
    return Bytes;
}

size_t multimapFullBytesFull(const multimapFull *m) {
    size_t Bytes = multimapFullBytes(m);

    /* The maps' bytes plus the struct and, per map, its range box, middle
     * and map pointer.  The per-map arrays are native multiarrays (no
     * container to measure), so their node overhead is not counted. */
    Bytes += sizeof(*m);
    Bytes += sizeof(databox) * (m->count - 1);
    Bytes += sizeof(multimapFullMiddle) * m->count;
    Bytes += sizeof(flex *) * m->count;

    return Bytes;
}

flex *multimapFullDump(const multimapFull *m) {
    const flex *localMaps[64] = {};
    const flex **useMaps;
    cleanerName(zfree, MapPtrs) const flex **heapMaps = nullptr;

    /* The sub-maps' pointers, gathered for one flexBulkMergeFlex: on the
     * stack up to 64 maps, past that on the heap */
    if (m->count <= countof(localMaps)) {
        useMaps = localMaps;
    } else {
        /* An array of m->count POINTERS — the element size must be
         * sizeof(*useMaps), not 1: allocating m->count BYTES here
         * under-allocated 8x and the pointer-fill loop below wrote
         * past the block for any multimapFull spanning more than
         * countof(localMaps) sub-maps. */
        heapMaps = zcalloc(m->count, sizeof(*useMaps));
        useMaps = heapMaps;
    }

    for (multimapFullIdx i = 0; i < m->count; i++) {
        useMaps[i] = getMap(m, i);
    }

    return flexBulkMergeFlex(useMaps, m->count);
}

multimapFull *multimapFullCopy(const multimapFull *restrict const m) {
    /* This is a slight ease of use hack for us.
     *
     * Technically we should just copy:
     *  - m->map
     *  - m->middle
     *  - m->rangeBox
     *      - NOTE: rangeBoxes could be ALLOCATED so need to DUPLICATE too!
     *
     * But, multiarray doesn't have copy functions and right now we don't want
     * to bother with writing them all, so let's copy our flexs and middles
     * into new arrays and just pretend we're "growing" this from an existing
     * map where the flexs are consumed.
     *
     * This method requires a little more processing because it does re-insert
     * everything everywhere instead of just copying existing arrays and
     * updating data inside of them, but it's much quicker for our immediate
     * development needs.
     *
     * It's still at least much quicker than _iterating_ all elements and
     * re-inserting them one-by-one into a new multimap. Here we're still
     * copying the flex extents and remembering their middle offsets natively.
     */
    const size_t howMany = m->count;

    static constexpr size_t stackAllocationDepth = 64;
    flex *maps_[stackAllocationDepth];
    flex **maps = maps_;
    cleanerName(zfree, Maps) flex **heapMapsSpill = nullptr;

    multimapFullMiddle middles_[stackAllocationDepth];
    multimapFullMiddle *middles = middles_;
    cleanerName(zfree, Middles) multimapFullMiddle *heapMiddlesSpill = nullptr;

    if (howMany > countof(maps_)) {
        heapMapsSpill = zcalloc(howMany, sizeof(*maps));
        maps = heapMapsSpill;
        heapMiddlesSpill = zcalloc(howMany, sizeof(*middles));
        middles = heapMiddlesSpill;
    }

    for (size_t i = 0; i < howMany; i++) {
        /* @own: container; free=multimapFullFree */
        maps[i] = flexDuplicate(getMap(m, i));
        middles[i] = getMiddle(m, i);
    }

    /* First arg of NULL will allocate a new multimap instead of reusing an
     * existing one.  The grow reads only middle[0, count), so a map with no
     * inner maps (DeleteNMapsIterate can empty one) passes no middles at
     * all, rather than a stack array nothing was written to. */
    multimapFull *const copy =
        multimapFullNewFromManyGrow(nullptr, maps, howMany ? middles : nullptr,
                                    howMany, m->elementsPerEntry, m->mapIsSet);
    /* The grow starts at the default inner-map size: the copy splits and
     * packs at the source's */
    copy->maxSize = m->maxSize;

    /* The grow decodes each rangeBox from its map's head.  In a
     * surrogate-keyed map that head is a reference id, not the resolved
     * key the source routes by, and there is no reference container here
     * to resolve it.  The source's rangeBoxes already hold the routing
     * keys, so the copy takes those, with its own copy of any BYTES or
     * wide payload.  A rangeBox that borrows bytes from a source map
     * equals the decoded head, so it keeps the box decoded from the
     * copy's own map. */
    for (multimapFullIdx i = 1; i < howMany; i++) {
        const databox *const src = getRangeBox(m, i - 1);
        databox *const dst = getRangeBox(copy, i - 1);
        if (!src->allocated && databoxCompare(dst, src) == 0) {
            continue;
        }

        rangeBoxAssign_(dst, src, true);
    }

    return copy;
}

/* ====================================================================
 * Range Box Searching
 * ==================================================================== */
/* A box's value as an unsigned 64-bit number, when it reads as one:
 * UNSIGNED_64, PTR, CONTAINER_REFERENCE_EXTERNAL or a non-negative
 * SIGNED_64 (databoxCompare orders all of these by that value) */
DK_INLINE_ALWAYS bool rangeBoxUnsigned_(const databox *box, uint64_t *value) {
    if (box->type == DATABOX_UNSIGNED_64 || box->type == DATABOX_PTR ||
        box->type == DATABOX_CONTAINER_REFERENCE_EXTERNAL ||
        (box->type == DATABOX_SIGNED_64 && box->data.i64 >= 0)) {
        *value = box->data.u64;
        return true;
    }

    return false;
}

/* Integer keys of a map often spread evenly (dense ids): a few probes
 * placed by the key's position between two known range boxes narrow the
 * search before halving takes over.  Only for maps, whose range boxes are
 * unique (so a box equal to the key answers exactly), and only while the
 * key and the boxes probed read as unsigned (any other box ends the
 * guessing; halving copes with every type).  The answer, the number of
 * range boxes at or below the key, is in [*min, *max] afterwards; true
 * with it in '*found' when a probe lands on the key. */
DK_INLINE_ALWAYS bool multimapFullInterpolate_(const multimapFull *m,
                                               const databox *key,
                                               multimapFullIdx *min,
                                               multimapFullIdx *max,
                                               multimapFullIdx *found) {
    uint64_t k;
    uint64_t vLo;
    uint64_t vHi;
    if (m->mapIsSet || m->count < 3 || !rangeBoxUnsigned_(key, &k)) {
        return false;
    }

    multimapFullIdx lo = 0;
    multimapFullIdx hi = m->count - 2; /* the last range box */
    if (!rangeBoxUnsigned_(getRangeBox(m, lo), &vLo) ||
        !rangeBoxUnsigned_(getRangeBox(m, hi), &vHi)) {
        return false;
    }

    if (k <= vLo || k >= vHi) {
        if (k == vLo || k == vHi) {
            *found = (k == vLo ? lo : hi) + 1;
            return true;
        }

        /* Below every range box: map 0; above every one: the last map */
        *min = *max = k < vLo ? 0 : hi + 1;
        return false;
    }

    /* Box lo is below the key and box hi above it, so a probe at
     * lo + 1 + (k - vLo) * (hi - lo - 1) / (vHi - vLo) falls strictly
     * between them */
    for (int probe = 0; probe < 4 && hi - lo > 1; probe++) {
        const multimapFullIdx g =
            lo + 1 +
            (multimapFullIdx)(((unsigned __int128)(k - vLo) * (hi - lo - 1)) /
                              (vHi - vLo));
        uint64_t v;
        if (!rangeBoxUnsigned_(getRangeBox(m, g), &v)) {
            break;
        }

        if (v == k) {
            *found = g + 1;
            return true;
        }

        if (v < k) {
            lo = g;
            vLo = v;
        } else {
            hi = g;
            vHi = v;
        }
    }

    *min = lo + 1;
    *max = hi;
    return false;
}

DK_STATIC multimapFullIdx multimapFullBinarySearch(const multimapFull *m,
                                                   const databox *key) {
    multimapFullIdx min = 0;

    /* 'count - 1' because we don't store a rangeBox for map[0], the
     * range of map[0] is implied to just be "less than the lowest value
     * in map[1]"
     * We always have m->count >= 1. */
    multimapFullIdx max = m->count - 1;

    multimapFullIdx found;
    if (multimapFullInterpolate_(m, key, &min, &max, &found)) {
        return found;
    }

    while (min < max) {
        const size_t mid = (min + max) >> 1;

        const databox *got = getRangeBox(m, mid);
        if (got->type >= DATABOX_BYTES) {
            /* The compare reads the bytes next */
            __builtin_prefetch(got->data.bytes.start, 0, 3);
        }

        const int compared = databoxCompareQuick(got, key);
#if 0
        printf("Current index: %zu\n", mid);
        databoxReprSay("Comparing databox A", got);
        databoxReprSay("Comparing databox B", key);
#endif

        if (compared < 0) {
            /* found value < search box */
            min = mid + 1;
        } else if (compared > 0) {
            /* found value > search box */
            max = mid;
        } else {
            /* found exact value */
            /* We do 'mid + 1' to undo the original 'count - 1'
             * used to start this search. */
#if 0
            printf("==========\n");
#endif
            return mid + 1;
        }
    }

#if 0
    printf("==========2\n");
#endif
    /* Note: we don't need corrective addition below because we're
     * at a lower range, and the lower range is captured correctly
     * by default (except for the 'min == count' case we handle) */

    /* If binary search tries to tell us our slot position is at
     * 'count', use the final map instead because the highest map
     * is where any values above the existing maximum should go. */
    if (min == m->count) {
        return min - 1;
    }

    return min;
}

/* The map holding the row 'key' addresses.  A set can hold several rows
 * with one key, running from the end of one map into the next, and the
 * search lands on the map that starts with the key: step back while the
 * map before ends with the key, so the key's FIRST row is addressed and
 * which row a lookup, delete or increment touches never depends on where
 * the maps happen to split. */
/* Whether a set's rows with 'key' continue from the map before 'idx' (its
 * last row has the key) */
DK_INLINE_ALWAYS bool multimapFullKeyContinuesBefore_(
    const multimapFull *m, const multimapFullIdx idx, const databox *key,
    const bool useReference, const multimapAtom *referenceContainer) {
    if (!m->mapIsSet || idx == 0 ||
        databoxCompare(getRangeBox(m, idx - 1), key) != 0) {
        return false;
    }
    databox last;
    databoxWide lastWide;
    multimapGetByTypeInto_(
        flexIndex(getMap(m, idx - 1), -(int32_t)m->elementsPerEntry), &last,
        &lastWide, useReference ? referenceContainer : nullptr);
    return databoxCompare(&last, key) == 0;
}

DK_INLINE_ALWAYS multimapFullIdx multimapFullKeyMap_(
    const multimapFull *m, const databox *key, const bool useReference,
    const multimapAtom *referenceContainer) {
    multimapFullIdx idx = multimapFullBinarySearch(m, key);
    while (multimapFullKeyContinuesBefore_(m, idx, key, useReference,
                                           referenceContainer)) {
        idx--;
    }
    return idx;
}

/* The row 'key' addresses within one map: a set's first row with it */
DK_INLINE_ALWAYS flexEntry *
multimapFullFindKeyInMap_(const multimapFull *m, const flex *map,
                          const flexEntry *middle, const databox *key,
                          const bool useReference,
                          const multimapAtom *referenceContainer) {
    if (useReference) {
        return m->mapIsSet ? flexFindByTypeSortedWithMiddleLowestWithReference(
                                 map, m->elementsPerEntry, key, middle,
                                 referenceContainer)
                           : flexFindByTypeSortedWithMiddleWithReference(
                                 map, m->elementsPerEntry, key, middle,
                                 referenceContainer);
    }
    return m->mapIsSet
               ? flexFindByTypeSortedWithMiddleLowest(map, m->elementsPerEntry,
                                                      key, middle)
               : flexFindByTypeSortedWithMiddle(map, m->elementsPerEntry, key,
                                                middle);
}

DK_STATIC multimapFullIdx
multimapFullBinarySearchFullWidth(multimapFull *m, const databox *elements[]) {
    multimapFullIdx min = 0;

    /* 'count - 1' because we always want 'current + 1' to be a valid map.
     * Also, it allows us to default to using 'max' as the highest value
     * without needing to clamp the value manually to 'count - 1' at the end. */
    multimapFullIdx max = m->count - 1;

    /* Is this optimized enough?  It's about half as fast as a
     * non-full-width search */

    while (min < max) {
        const size_t mid = (min + max) >> 1;
        const flex *map = getMap(m, mid);

        /* Prefetch the map data for comparison */
        __builtin_prefetch(map, 0, 1);

        const int compared =
            flexCompareEntries(map, elements, m->elementsPerEntry, 0);
        if (compared < 0) {
            /* Need to test for:
             *  [MID, SEARCH, NEXT).
             * If true, then value belongs in MID */

            /* We use the 'next head' ('mid + 1') instead of 'mid' tail because
             * if this check fails, we may need to check next head again anyway
             * at another iteration of the binary search, and by reading here,
             * it will already be in our cache hierarchy when (maybe) needed
             * again real soon now. */
            const flex *nextMap = getMap(m, mid + 1);
            __builtin_prefetch(nextMap, 0, 1);
            const int nextHeadCompared =
                flexCompareEntries(nextMap, elements, m->elementsPerEntry, 0);
            if (nextHeadCompared > 0) {
                /* (LOW, ELEMENT[i], HIGH) */
                /* Found map containing this element range */
                return mid;
            }

            /* [LOW, HIGH, ELEMENT[i]] */
            min = mid + 1;
        } else if (compared > 0) {
            /* found value > search box */
            max = mid;
        } else {
            /* else, element matches. */
            return mid;
        }
    }

    return min;
}

/* ====================================================================
 * Insert Helper
 * ==================================================================== */
DK_INLINE_ALWAYS bool
abstractFlexInsert(multimapFull *const m, const multimapFullIdx mapIdx,
                   flex **map, const databox *elements[],
                   const bool useSurrogateKey, const databox *const insertKey,
                   const multimapAtom *const referenceContainer,
                   const bool keysCanBecomePointers, void **keyAsPointer,
                   const bool forceFullWidthComparison,
                   databox *const replacedOut) {
    flexEntry *middle = GET_MIDDLE(m, mapIdx, *map);

    bool found = false;

    /* compareUsingKeyElementOnly: For maps (!mapIsSet), compare only key.
     * For sets (mapIsSet), compare all elements to check for full-width dups.
     * If forceFullWidthComparison is true, always compare all elements. */
    const bool compareUsingKeyElementOnly =
        forceFullWidthComparison ? false : !m->mapIsSet;

    if (useSurrogateKey) {
        assert(!keysCanBecomePointers);
        /* No replacing entry point reaches the surrogate-key insert. */
        assert(!replacedOut);
        found =
            flexInsertReplaceByTypeSortedWithMiddleMultiWithReferenceWithSurrogateKey(
                map, m->elementsPerEntry, elements, insertKey, &middle,
                compareUsingKeyElementOnly, referenceContainer);

        updateRangeBoxForIdxWithReference(m, mapIdx, *map, referenceContainer);
    } else {
        if (keysCanBecomePointers && m->mapIsSet && !forceFullWidthComparison) {
            /* Set semantics with externalized keys: the pointer insert has
             * no dedupe mode, so look for the whole row first and insert
             * (as a full-width row, which may externalize the key) only
             * when it is absent.  The probe uses the same full-width
             * comparison the insert positions with. */
            found = flexFindByTypeSortedWithMiddleFullWidth(
                        *map, m->elementsPerEntry, elements, middle) != nullptr;
            if (!found) {
                flexInsertReplaceByTypeSortedWithMiddleMultiDirectLongKeysBecomePointers(
                    map, m->elementsPerEntry, elements, &middle, false,
                    keyAsPointer);
            }

            multimapReplacedOutClear_(replacedOut, m->elementsPerEntry);
        } else if (keysCanBecomePointers) {
            found =
                flexInsertReplaceByTypeSortedWithMiddleMultiDirectLongKeysBecomePointersReplacing(
                    map, m->elementsPerEntry, elements, &middle,
                    compareUsingKeyElementOnly, keyAsPointer, replacedOut);
        } else if (m->mapIsSet && !forceFullWidthComparison) {
            /* TRUE set semantics: full-width compare AND dedupe — the
             * `values` accounting below already assumed found means
             * no-dup ("For sets: 'found' means exact match (no dup)"),
             * but the plain full-width insert appended a duplicate row
             * anyway, desyncing count from physical rows. */
            found = flexInsertReplaceByTypeSortedWithMiddleMultiDirectSet(
                map, m->elementsPerEntry, elements, &middle);
            multimapReplacedOutClear_(replacedOut, m->elementsPerEntry);
        } else {
            found = flexInsertReplaceByTypeSortedWithMiddleMultiDirectReplacing(
                map, m->elementsPerEntry, elements, &middle,
                compareUsingKeyElementOnly, replacedOut);
        }

        updateRangeBoxForIdx(m, mapIdx, *map);
    }

    SET_MIDDLE(m, mapIdx, middle, *map);

    /* Only update count if we inserted a NEW key (not replaced an existing).
     * For maps: 'found' means we found and replaced an existing key.
     * For sets: 'found' means we found exact (key+value) match (no dup). */
    if (!found) {
        m->values++;
    }

    return found;
}

DK_STATIC bool multimapFullFlexInsert(multimapFull *m,
                                      const multimapFullIdx mapIdx, flex **map,
                                      const databox *elements[],
                                      databox *replacedOut) {
    return abstractFlexInsert(m, mapIdx, map, elements, false, nullptr, nullptr,
                              false, nullptr, false, replacedOut);
}

DK_STATIC bool multimapFullFlexInsertFullWidth(multimapFull *m,
                                               const multimapFullIdx mapIdx,
                                               flex **map,
                                               const databox *elements[]) {
    /* Force full-width comparison (compare all elements, not just key) */
    return abstractFlexInsert(m, mapIdx, map, elements, false, nullptr, nullptr,
                              false, nullptr, true, nullptr);
}

DK_STATIC bool multimapFullFlexInsertExternalizeLargeKeys(
    multimapFull *m, const multimapFullIdx mapIdx, flex **map,
    const databox *elements[], void **keyCreated, databox *replacedOut) {
    return abstractFlexInsert(m, mapIdx, map, elements, false, nullptr, nullptr,
                              true, keyCreated, false, replacedOut);
}

DK_STATIC bool multimapFullFlexInsertWithSurrogateKey(
    multimapFull *m, const multimapFullIdx mapIdx, flex **map,
    const databox *elements[], const databox *insertKey,
    const multimapAtom *referenceContainer) {
    return abstractFlexInsert(m, mapIdx, map, elements, true, insertKey,
                              referenceContainer, false, nullptr, false,
                              nullptr);
}

/* ====================================================================
 * Delete Helper
 * ==================================================================== */
DK_INLINE_ALWAYS bool abstractFlexDelete(
    multimapFull *m, const multimapFullIdx mapIdx, const databox *elements[],
    const bool useFullWidth, const bool useReference,
    const multimapAtom *referenceContainer, databox *foundReference,
    databox *removedOut, const bool takeRemoved) {
    flex **map = getMapPtr(m, mapIdx);
    flexEntry *middle = GET_MIDDLE(m, mapIdx, *map);

    flexEntry *foundP;

    if (useFullWidth) {
        if (useReference) {
            foundP = flexFindByTypeSortedWithMiddleFullWidthWithReference(
                *map, m->elementsPerEntry, elements, middle,
                referenceContainer);

        } else {
            foundP = flexFindByTypeSortedWithMiddleFullWidth(
                *map, m->elementsPerEntry, elements, middle);
        }
    } else {
        foundP = multimapFullFindKeyInMap_(m, *map, middle, elements[0],
                                           useReference, referenceContainer);
    }

    if (foundP) {
        if (foundReference) {
            /* The row is being deleted, so a 96/128-bit key is reported
             * type-only (see multimapDeleteWithFound). */
            flexGetByTypeInto(foundP, foundReference, nullptr);
        }

        multimapRemovedOutFill_(*map, foundP, m->elementsPerEntry, removedOut,
                                takeRemoved);
        flexDeleteSortedValueWithMiddle(map, m->elementsPerEntry, foundP,
                                        &middle);
        m->values--;
        SET_MIDDLE(m, mapIdx, middle, *map);
        if (useReference) {
            updateRangeBoxForIdxWithReference(m, mapIdx, *map,
                                              referenceContainer);
        } else {
            updateRangeBoxForIdx(m, mapIdx, *map);
        }
    }

    return !!foundP;
}

DK_INLINE_ALWAYS bool multimapFullFlexDeleteWithReference(
    multimapFull *m, const multimapFullIdx mapIdx, const databox *key,
    const multimapAtom *referenceContainer, databox *foundReference) {
    const databox *elements[1] = {key};
    return abstractFlexDelete(m, mapIdx, elements, false, true,
                              referenceContainer, foundReference, nullptr,
                              false);
}

DK_INLINE_ALWAYS bool
multimapFullFlexDeleteWithFound(multimapFull *m, const multimapFullIdx mapIdx,
                                const databox *key, databox *foundReference,
                                databox *removedOut, const bool takeRemoved) {
    const databox *elements[1] = {key};
    return abstractFlexDelete(m, mapIdx, elements, false, false, nullptr,
                              foundReference, removedOut, takeRemoved);
}

DK_INLINE_ALWAYS bool multimapFullFlexDeleteFullWidthWithFound(
    multimapFull *m, const multimapFullIdx mapIdx, const databox *elements[],
    databox *foundReference) {
    return abstractFlexDelete(m, mapIdx, elements, true, false, nullptr,
                              foundReference, nullptr, false);
}

DK_INLINE_ALWAYS bool multimapFullFlexDeleteFullWidthWithReference(
    multimapFull *m, const multimapFullIdx mapIdx, const databox *elements[],
    const multimapAtom *referenceContainer, databox *foundReference) {
    return abstractFlexDelete(m, mapIdx, elements, true, true,
                              referenceContainer, foundReference, nullptr,
                              false);
}

/* Locate the row at position 'keyIndex' (0-based forward; negative =
 * 1-based from the tail): skip whole sub-maps by their row counts,
 * scanning from whichever END is nearer the target rank (O(maps / 2)
 * worst case), then walk the owning flex from the nearest of its
 * head/middle/tail anchors. */
DK_STATIC flexEntry *multimapFullRowByPosition_(multimapFull *m,
                                                int64_t keyIndex,
                                                multimapFullIdx *mapIdxOut,
                                                flex ***mapOut) {
    const int64_t rows = (int64_t)m->values;
    const int64_t row = keyIndex < 0 ? rows + keyIndex : keyIndex;
    if (row < 0 || row >= rows) {
        return nullptr;
    }

    multimapFullIdx mapIdx;
    flex **map;
    int64_t local;

    if (row <= rows / 2) {
        /* Forward scan: 'remain' counts rows before the current map */
        int64_t remain = row;
        mapIdx = 0;
        for (;; mapIdx++) {
            assert(mapIdx < m->count);
            map = getMapPtr(m, mapIdx);
            const int64_t mapRows = flexCount(*map) / m->elementsPerEntry;
            if (remain < mapRows) {
                local = remain;
                break;
            }

            remain -= mapRows;
        }
    } else {
        /* Backward scan: 'fromEnd' counts rows at and after the
         * current map's end */
        int64_t fromEnd = rows - row;
        mapIdx = m->count;
        for (;;) {
            assert(mapIdx > 0);
            mapIdx--;
            map = getMapPtr(m, mapIdx);
            const int64_t mapRows = flexCount(*map) / m->elementsPerEntry;
            if (fromEnd <= mapRows) {
                local = mapRows - fromEnd;
                break;
            }

            fromEnd -= mapRows;
        }
    }

    *mapIdxOut = mapIdx;
    *mapOut = map;
    return flexIndexWithMiddle(*map, m->elementsPerEntry,
                               local * m->elementsPerEntry,
                               GET_MIDDLE(m, mapIdx, *map));
}

bool multimapFullLookupByPosition(multimapFull *m, int64_t keyIndex,
                                  databox *elements[]) {
    multimapFullIdx mapIdx;
    flex **map;
    flexEntry *fe = multimapFullRowByPosition_(m, keyIndex, &mapIdx, &map);
    if (!fe) {
        return false;
    }

    for (multimapElements i = 0; i < m->elementsPerEntry; i++) {
        flexGetByType(fe, elements[i]);
        fe = flexNext(*map, fe);
    }

    return true;
}

bool multimapFullDeleteByPosition(multimapFull *m, int64_t keyIndex) {
    multimapFullIdx mapIdx;
    flex **map;
    flexEntry *fe = multimapFullRowByPosition_(m, keyIndex, &mapIdx, &map);
    if (!fe) {
        return false;
    }

    multimapEntry me = {.map = map, .mapIdx = mapIdx, .fe = fe};
    multimapFullDeleteEntry(m, &me);
    return true;
}

int64_t multimapFullFieldIncr(multimapFull *m, const databox *key,
                              uint32_t fieldOffset, int64_t incrBy) {
    /* Only a value of a row: 0 is the key, and past the last value the
     * walk would leave the row */
    if (fieldOffset == 0 || fieldOffset >= m->elementsPerEntry) {
        return 0;
    }
    uint32_t mapIdx = multimapFullKeyMap_(m, key, false, nullptr);

    flex **map = getMapPtr(m, mapIdx);
    flexEntry *middle = GET_MIDDLE(m, mapIdx, *map);

    flexEntry *current =
        multimapFullFindKeyInMap_(m, *map, middle, key, false, nullptr);
    if (!current) {
        /* No such key: return 0 rather than walking flexNext from NULL.
         * Matches the multimap-level FieldIncr miss contract. */
        return 0;
    }

    while (fieldOffset--) {
        current = flexNext(*map, current);
    }

    int64_t newVal = 0;
    const size_t bytesBefore = flexBytes(*map);
    if (flexIncrbySigned(map, current, incrBy, &newVal)) {
        /* An entry re-encoded at a different width moves the entries
         * after it and may realloc the flex: the middle is found again
         * and the rangeBox refreshed.  A map of the same size was written
         * in place, every entry where it was. */
        if (flexBytes(*map) != bytesBefore) {
            SET_MIDDLE_FORCE(m, mapIdx, *map);
            updateRangeBoxForIdxAfterInMapMutation(m, mapIdx, *map);
        }
        return newVal;
    }

    /* (unlikely) else, return current value; a 96/128-bit value can't be
     * read as int64_t, so it reports 0 (only its type is decoded) */
    databox curVal;
    flexGetByTypeInto(current, &curVal, nullptr);
    return databoxIsWideDeferred(&curVal) ? 0 : curVal.data.i;
}

/* ====================================================================
 * Debuggles API
 * ==================================================================== */
/* Consecutive range boxes differ (fails by design when duplicate keys are
 * allowed). Asserts only: nothing to do under NDEBUG. */
DK_FN_UNUSED static void
strictConsistencyCheck([[maybe_unused]] const multimapFull *m) {
#ifndef NDEBUG
    /* map i (i >= 1) owns range box i - 1 */
    for (size_t i = 1; i + 1 < m->count; i++) {
        assert(databoxCompare(getRangeBox(m, i - 1), getRangeBox(m, i)) != 0);
    }

    /* Walk only the key of each row, so any row width works. */
    /* 'prev' and 'key' decode into alternating slots, so 'prev' keeps its
     * wide payload while the next key is read. */
    bool isFirst = true;
    databox prev;
    databoxWide keyWide[2];
    uint32_t slot = 0;
    for (multimapFullIdx i = 0; i < m->count; i++) {
        const flex *map = getMap(m, i);
        flexEntry *fe = flexCount(map) ? flexHead(map) : nullptr;
        while (fe) {
            databox key;
            flexGetByTypeInto(fe, &key, &keyWide[slot]);
            slot ^= 1;
            for (multimapElements e = 0; fe && e < m->elementsPerEntry; e++) {
                fe = flexNext(map, fe);
            }

            if (isFirst) {
                prev = key;
                isFirst = false;
                continue;
            }

            /* Consistency means previous element is EITHER:
             *  - less than current element, OR
             *  - equals current element (we allow duplicate keys)
             */
            /* Also: 'reference' types don't sort based on their own
             * equality because they may be part of a self-sorting foreign
             * key list. */
            if (prev.type != DATABOX_CONTAINER_REFERENCE_EXTERNAL &&
                key.type != DATABOX_CONTAINER_REFERENCE_EXTERNAL) {
                assert(databoxCompare(&prev, &key) <= 0);
            }

            prev = key;
        }
    }
#endif
}

/* ====================================================================
 * Insert API
 * ==================================================================== */
DK_INLINE_ALWAYS bool
abstractInsert(multimapFull *m, const databox *elements[],
               multimapFullIdx mapIdx, const bool useSurrogateKey,
               const databox *insertKey, const multimapAtom *referenceContainer,
               const bool keysCanBecomePointers, void **keyAsPointer,
               const bool forceFullWidthComparison, databox *replacedOut) {
    assert(!useSurrogateKey || (useSurrogateKey && referenceContainer));
    /* 'replacedOut' rides only the key-compare map insert (the replacing
     * entry points); the full-width and surrogate inserts never carry it. */
    assert(!replacedOut || (!useSurrogateKey && !forceFullWidthComparison));

    /* Turn 'found' to true if this ends up being a REPLACE */
    bool found = false;

    /* Step 1: find matching map for range. */
    flex **map = getMapPtr(m, mapIdx);

    /* Step 2: check if map has room for this new entry. */
    const size_t mapBytes = flexBytes(*map);
    const size_t mapElementCount = flexCount(*map) / m->elementsPerEntry;

#if NOISY_MAPPING
    printf("====================================\n");
    printf("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n\n");
    //    multimapFullRepr(m);
    printf("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n");
    printf("====================================\n\n");
#endif

#if NOISY_MAPPING
    const size_t totalMapsAtStart = m->count;
    printf("[%p] Checking map %d with bytes %zu against max of %d (total maps: "
           "%d; elements per entry: %d)\n",
           (void *)m, mapIdx, mapBytes, m->maxSize, m->count,
           m->elementsPerEntry);
#endif

    /* Any REPLACE on an existing key MUST stay in the
     * map that already holds it.  If we instead route REPLACE
     * through the split-or-new-map path below, we end up with
     * two maps containing the same key, which corrupts
     * subsequent lookups (the binary search by range box
     * picks the WRONG map for keys equal to a range
     * boundary).  Symptom: SET on an existing large value
     * silently leaves the prior value behind.
     *
     * Detection: when the map exceeds maxSize but the
     * incoming key already exists in this map, we're doing a
     * REPLACE — flexInsertReplace handles in-place updates
     * (grow/shrink the flex by delta, no need to split). */
    bool wouldBeReplaceInThisMap = false;
    if ((mapBytes > m->maxSize) && mapElementCount > 0 && !useSurrogateKey) {
        /* useSurrogateKey path uses a different lookup (with
         * reference container) — leave it on the existing
         * code path; this check is for the common
         * full-key-comparison case. */
        flexEntry *foundEntry = flexFindByTypeSortedWithMiddle(
            *map, m->elementsPerEntry, elements[0],
            GET_MIDDLE(m, mapIdx, *map));
        if (foundEntry) {
            wouldBeReplaceInThisMap = true;
        }
    }

    if ((mapBytes <= m->maxSize) || mapElementCount == 0 ||
        wouldBeReplaceInThisMap) {
        /* A row goes in whole, whatever its size: a map under the limit
         * takes it, and a map it leaves over the limit splits on the next
         * insert (a map of one row is never split). */

        /* Step 3a: If map has room, insert directly. */
        if (useSurrogateKey) {
            found = multimapFullFlexInsertWithSurrogateKey(
                m, mapIdx, map, elements, insertKey, referenceContainer);
        } else {
            if (keysCanBecomePointers) {
                found = multimapFullFlexInsertExternalizeLargeKeys(
                    m, mapIdx, map, elements, keyAsPointer, replacedOut);
            } else if (forceFullWidthComparison) {
#if NOISY_MAPPING
                printf("Inserting into existing (full width)...\n");
#endif
                found =
                    multimapFullFlexInsertFullWidth(m, mapIdx, map, elements);
            } else {
#if NOISY_MAPPING
                printf("Inserting into existing...\n");
#endif
                found = multimapFullFlexInsert(m, mapIdx, map, elements,
                                               replacedOut);
            }
        }
    } else {
        /* else, target map is too big for more storage, so either:
         *  - split map in half
         *  - insert new map BEFORE mapIdx
         *  - insert new map AFTER mapIdx
         *
         *  ...then insert into correct map.
         */
        int currentPositionLargerThanNewKey;

        /* If accounting is functioning correctly, interior maps will never have
         * zero elements (a zero element map not at mapIdx 0 is illegal). */
        assert(mapIdx == 0 || (mapIdx > 0 && mapElementCount != 0));

        /* If map only has one element AND is already too big for this
         * insert, then just create a new map instead of doing a split. */
        if (mapElementCount == 1) {
#if NOISY_MAPPING
            printf("Creating new map...\n");
#endif
            /* Avoid split, just create ONE NEW MAP at the proper position! */
            /* (it doesn't make sense to split a map of one element
             * since that's a lot more work than just making a new clean map) */

            /* If new value is BIGGER than current value, new map AFTER. */
            /* If new value is SMALLER than current value, new map BEFORE. */
            /* (if same, who cares?) */

            /* If our multimapFull _only_ has one map (or if we
             * are insert-testing against map 0), we can't use
             * a range box, so special case this. */
            if (m->count == 1 || mapIdx == 0) {
#if NOISY_MAPPING
                printf("USING EXTANT BOX!\n");
#endif
                assert(mapIdx == 0);
                /* Now we have CONFIRMED:
                 *  - only one map exists
                 *  - map only has ONE element
                 *  - mapIdx is therefore 0
                 */
                /* if we *only* have one map, we can't compare using
                 * range boxes because map[0] has no range box,
                 * so just use the head value directly for insert
                 * comparisons. */
                databox got;
                databoxWide gotWide;
                const flexEntry *fe = flexHead(*map);
                multimapGetByTypeInto_(fe, &got, &gotWide,
                                       useSurrogateKey ? referenceContainer
                                                       : nullptr);

#if NOISY_MAPPING
                databoxReprSay("Deciding based on head[0] of:", &got);
#endif
                currentPositionLargerThanNewKey =
                    databoxCompare(&got, elements[0]) > 0;
            } else {
                /* else, this multimapFull has multiple maps,
                 * but the one we're trying to insert into ONLY
                 * has one element, so we need to either prepend or
                 * append a map then insert on the correct side. */
                assert(mapIdx > 0);
                const databox *const rangeBox = getRangeBox(m, mapIdx - 1);
#if NOISY_MAPPING
                assert(mapIdx > 0);
                printf("USING RANGE BOX (%d): %p!\n", mapIdx - 1, rangeBox);
                databoxReprSay("WITH VALUE:", rangeBox);
                databoxReprSay("AGAINST:", elements[0]);
#endif
                currentPositionLargerThanNewKey =
                    databoxCompare(rangeBox, elements[0]) > 0;
            }

            /* If current map is is greater than insert value,
             * we need to insert before current map. */
            if (currentPositionLargerThanNewKey) {
#if NOISY_MAPPING
                printf("Adding new map BEFORE EXISTING IDX: %d\n", mapIdx);
#endif
                mapIdx = mapNewBeforeExisting_(m, mapIdx, *map, useSurrogateKey,
                                               referenceContainer);
                /* remainder of calculations assume 'mapIdx' is index
                 * of the map we insert our new values into, and since
                 * we just split an empty map before this value,
                 * our returned mapIdx equals the original mapIdx - 1 */
                map = nullptr;
            } else {
#if NOISY_MAPPING
                printf("Adding new map AFTER EXISTING IDX: %d\n", mapIdx);
#endif
                /* else, current map is <= insert value, so insert after */
                /* This is the same as a normal SPLIT operation except
                 * we just add a new map after the existing map instead
                 * of splitting the current map (which makes no sense if
                 * we only have one map). */
                mapNewAfterExisting_(m, mapIdx, *map, useSurrogateKey,
                                     referenceContainer);
                map = nullptr;
            }
        } else {
            /* else, we have MORE than one map AND it has more than one
             * element, so we can just split it directly the insert into
             * the correct half after splitting. */
#if NOISY_MAPPING
            printf("Splitting...\n");
#endif
            /* Step 3b: Split current map then write into correct half */
            splitMapLowHigh_(m, mapIdx, map, useSurrogateKey,
                             referenceContainer);

            /* Step 3b1: Determine if the target map is LOW or HIGH after the
             *           split by comparing the smallest element of the HIGH
             *           map. If our insert element is smaller than HIGH,
             *           insert into LOW.
             *           Else, insert into HIGH. */
            /* The index arithmetic: map i (i > 0) has its head key in
             * rangeBox[i - 1], and map 0 has none (no key is below it).
             * The split made 'mapIdx' the low half and 'mapIdx + 1' the
             * high half, so the high half's head is
             * rangeBox[mapIdx], always present. */
            const size_t mapIdxHigh = mapIdx + 1;

#if NOISY_MAPPING
            assert(mapIdxHigh > 0);
            printf("USING RANGE BOX for Idx %zu: %p!\n", mapIdxHigh,
                   getRangeBox(m, mapIdxHigh - 1));
            databoxReprSay("Of value: ", getRangeBox(m, mapIdxHigh - 1));
#endif

            currentPositionLargerThanNewKey =
                databoxCompare(getRangeBox(m, mapIdxHigh - 1), elements[0]) > 0;
        }

        /* Step 3b2: Now all accounting is done and we can actually insert the
         *           new element into the appropriate map. */

        /* Now insert depending on whether current position is larger than
         * the insert key. */
        if (currentPositionLargerThanNewKey) {
#if NOISY_MAPPING
            printf("ADDING TO BEFORE!\n");
#endif
            /* Re-fetch 'map' because it could have been realloc'd away */
            map = getMapPtr(m, mapIdx);

            if (useSurrogateKey) {
                found = multimapFullFlexInsertWithSurrogateKey(
                    m, mapIdx, map, elements, insertKey, referenceContainer);
            } else {
                if (keysCanBecomePointers) {
                    found = multimapFullFlexInsertExternalizeLargeKeys(
                        m, mapIdx, map, elements, keyAsPointer, replacedOut);
                } else if (forceFullWidthComparison) {
                    found = multimapFullFlexInsertFullWidth(m, mapIdx, map,
                                                            elements);
                } else {
                    found = multimapFullFlexInsert(m, mapIdx, map, elements,
                                                   replacedOut);
                }
            }
        } else {
#if NOISY_MAPPING
            printf("ADDING TO AFTER!\n");
#endif
            /* ELSE, compare said USE HIGHER MAP FOR INSERT */
            const multimapFullIdx nextIdx = mapIdx + 1;
            flex **nextMap = getMapPtr(m, nextIdx);
            if (useSurrogateKey) {
                found = multimapFullFlexInsertWithSurrogateKey(
                    m, nextIdx, nextMap, elements, insertKey,
                    referenceContainer);
            } else {
                if (keysCanBecomePointers) {
                    found = multimapFullFlexInsertExternalizeLargeKeys(
                        m, nextIdx, nextMap, elements, keyAsPointer,
                        replacedOut);
                } else if (forceFullWidthComparison) {
                    found = multimapFullFlexInsertFullWidth(m, nextIdx, nextMap,
                                                            elements);
                } else {
                    found = multimapFullFlexInsert(m, nextIdx, nextMap,
                                                   elements, replacedOut);
                }
            }
        }
    }

    /* STRICT CONSISTENCY CHECK */
    /* Verifies elements are in sorted order across the entire multimapFull */
#if NOISY_MAPPING
    /* We either kept the SAME number of maps by inserting into an existing map
     * - OR -
     * We created ONE new map for an insert.
     * If we create more than one new map, something bad happened. */
    assert(m->count == totalMapsAtStart || m->count == totalMapsAtStart + 1);
    strictConsistencyCheck(m);
#endif

#if NOISY_MAPPING
    printf("====================================\n");
    printf("====================================\n\n");
#endif

    return found;
}

DK_STATIC bool multimapFullInsert_(multimapFull *m, const databox *elements[],
                                   const multimapFullIdx mapIdx,
                                   databox *replacedOut) {
    return abstractInsert(m, elements, mapIdx, false, nullptr, nullptr, false,
                          nullptr, false, replacedOut);
}

DK_STATIC bool multimapFullInsert_FullWidth_(multimapFull *m,
                                             const databox *elements[],
                                             const multimapFullIdx mapIdx) {
    /* Force full-width comparison (compare all elements, not just key) */
    return abstractInsert(m, elements, mapIdx, false, nullptr, nullptr, false,
                          nullptr, true, nullptr);
}

DK_STATIC bool multimapFullInsertWithSurrogateKey_(
    multimapFull *m, const databox *elements[], const databox *insertKey,
    const multimapFullIdx mapIdx, const multimapAtom *referenceContainer) {
    return abstractInsert(m, elements, mapIdx, true, insertKey,
                          referenceContainer, false, nullptr, false, nullptr);
}

bool multimapFullInsertWithSurrogateKey(
    multimapFull *m, const databox *elements[], const databox *insertKey,
    const multimapAtom *referenceContainer) {
    const multimapFullIdx mapIdx = multimapFullBinarySearch(m, elements[0]);
    return multimapFullInsertWithSurrogateKey_(m, elements, insertKey, mapIdx,
                                               referenceContainer);
}

/* As multimapSmallInsertWithSurrogateKeyIfAbsent: the map the key belongs
 * to is found once, and serves both the presence check and the insert */
bool multimapFullInsertWithSurrogateKeyIfAbsent(
    multimapFull *m, const databox *elements[], const databox *insertKey,
    const multimapAtom *referenceContainer, databox *foundKey,
    flexWhenAbsent *whenAbsent, void *whenAbsentCtx) {
    const multimapFullIdx mapIdx = multimapFullBinarySearch(m, elements[0]);
    flex **mapPtr = getMapPtr(m, mapIdx);

    /* Room in the map (abstractInsert's direct-insert case): one search
     * finds the key or inserts it, with abstractFlexInsert's upkeep */
    if (flexBytes(*mapPtr) <= m->maxSize || flexCount(*mapPtr) == 0) {
        flexEntry *middle = GET_MIDDLE(m, mapIdx, *mapPtr);
        flexEntry *present = nullptr;
        if (flexInsertByTypeSortedWithMiddleMultiWithReferenceWithSurrogateKeyIfAbsent(
                mapPtr, m->elementsPerEntry, elements, insertKey, &middle,
                referenceContainer, &present, whenAbsent, whenAbsentCtx)) {
            flexGetByType(present, foundKey);
            return false;
        }

        updateRangeBoxForIdxWithReference(m, mapIdx, *mapPtr,
                                          referenceContainer);
        SET_MIDDLE(m, mapIdx, middle, *mapPtr);
        m->values++;
        return true;
    }

    /* A full map splits before the insert: look first, then insert */
    const flex *map = *mapPtr;
    flexEntry *found =
        multimapFullFindKeyInMap_(m, map, GET_MIDDLE(m, mapIdx, map),
                                  elements[0], true, referenceContainer);
    if (found) {
        flexGetByType(found, foundKey);
        return false;
    }

    if (whenAbsent) {
        whenAbsent(whenAbsentCtx);
    }

    multimapFullInsertWithSurrogateKey_(m, elements, insertKey, mapIdx,
                                        referenceContainer);
    return true;
}

bool multimapFullInsert(multimapFull *m, const databox *elements[]) {
    return multimapFullInsertReplacing(m, elements, nullptr);
}

bool multimapFullInsertReplacing(multimapFull *m, const databox *elements[],
                                 databox *replacedOut) {
    /* Step 1: find matching map for range. */
    const multimapFullIdx mapIdx = multimapFullBinarySearch(m, elements[0]);
    return multimapFullInsert_(m, elements, mapIdx, replacedOut);
}

/* As multimapSmallInsertIfAbsent: the map the key belongs to is found
 * once, and serves both the presence check and the insert */
bool multimapFullInsertIfAbsent(multimapFull *m, const databox *elements[],
                                multimapEntry *me) {
    const multimapFullIdx mapIdx = multimapFullBinarySearch(m, elements[0]);
    flex **mapPtr = getMapPtr(m, mapIdx);
    flexEntry *found = nullptr;

    /* Room in the map (abstractInsert's direct-insert case): one search
     * finds the key or inserts the row, with abstractFlexInsert's upkeep */
    if (flexBytes(*mapPtr) <= m->maxSize || flexCount(*mapPtr) == 0) {
        flexEntry *middle = GET_MIDDLE(m, mapIdx, *mapPtr);
        if (!flexInsertByTypeSortedWithMiddleMultiDirectIfAbsent(
                mapPtr, m->elementsPerEntry, elements, &middle, &found,
                m->mapIsSet && me)) {
            updateRangeBoxForIdx(m, mapIdx, *mapPtr);
            SET_MIDDLE(m, mapIdx, middle, *mapPtr);
            m->values++;
            return true;
        }
    } else {
        /* A full map splits before the insert: look first, then insert */
        found = multimapFullFindKeyInMap_(m, *mapPtr,
                                          GET_MIDDLE(m, mapIdx, *mapPtr),
                                          elements[0], false, nullptr);
        if (!found) {
            multimapFullInsert_(m, elements, mapIdx, nullptr);
            return true;
        }
    }

    /* Present: the map is unchanged.  A set's lookup reports the key's
     * first row, found above within this map; only when the key's rows
     * start in an earlier map is it looked up there */
    if (me) {
        if (multimapFullKeyContinuesBefore_(m, mapIdx, elements[0], false,
                                            nullptr)) {
            multimapFullGetUnderlyingEntry(m, elements[0], me);
        } else {
            me->fe = found;
            me->mapIdx = mapIdx;
            me->map = mapPtr;
        }
    }
    return false;
}

bool multimapFullInsertAllowExternalizeKeys(multimapFull *m,
                                            const databox *elements[],
                                            void **keyAllocation) {
    return multimapFullInsertAllowExternalizeKeysReplacing(
        m, elements, keyAllocation, nullptr);
}

bool multimapFullInsertAllowExternalizeKeysReplacing(multimapFull *m,
                                                     const databox *elements[],
                                                     void **keyAllocation,
                                                     databox *replacedOut) {
    /* Step 1: find matching map for range. */
    const multimapFullIdx mapIdx = multimapFullBinarySearch(m, elements[0]);
    return abstractInsert(m, elements, mapIdx, false, nullptr, nullptr, true,
                          keyAllocation, false, replacedOut);
}

void multimapFullAppend(multimapFull *m, const databox *elements[]) {
    /* Straight into the highest map for a row at or past its last key;
     * any other row goes where an insert puts it, so the maps stay
     * ordered */
    const flex *highest = getMapHighest(m);
    if (flexCount(highest) == 0) {
        multimapFullInsert(m, elements);
        return;
    }
    databox last;
    databoxWide wide;
    flexGetByTypeInto(flexIndex(highest, -(int32_t)m->elementsPerEntry), &last,
                      &wide);
    if (databoxCompare(elements[0], &last) < 0) {
        multimapFullInsert(m, elements);
        return;
    }
    multimapFullInsert_(m, elements, m->count - 1, nullptr);
}

void multimapFullInsertFullWidth(multimapFull *m, const databox *elements[]) {
    /* Step 1: find matching map for range (full width search). */
    const multimapFullIdx mapIdx =
        multimapFullBinarySearchFullWidth(m, elements);

    /* InsertFullWidth: Always compare ALL elements (key+value) to allow
     * multiple entries with same key but different values (sorted set). */
    multimapFullInsert_FullWidth_(m, elements, mapIdx);
}

bool multimapFullGetUnderlyingEntry(multimapFull *m, const databox *key,
                                    multimapEntry *me) {
    me->mapIdx = multimapFullKeyMap_(m, key, false, nullptr);
    me->map = getMapPtr(m, me->mapIdx);
    me->fe = multimapFullFindKeyInMap_(
        m, *me->map, GET_MIDDLE(m, me->mapIdx, *me->map), key, false, nullptr);
    return !!me->fe;
}

bool multimapFullGetUnderlyingEntryFullWidth(multimapFull *m,
                                             const databox *elements[],
                                             multimapEntry *me) {
    me->mapIdx = multimapFullBinarySearchFullWidth(m, elements);
    me->map = getMapPtr(m, me->mapIdx);
    me->fe = flexFindByTypeSortedWithMiddleFullWidth(
        *me->map, m->elementsPerEntry, elements,
        GET_MIDDLE(m, me->mapIdx, *me->map));
    return !!me->fe;
}

static bool multimapFullGetUnderlyingEntryGetEntry(multimapFull *m,
                                                   const databox *key,
                                                   multimapEntry *me) {
    me->mapIdx = multimapFullBinarySearch(m, key);
    me->map = getMapPtr(m, me->mapIdx);
    me->fe = flexFindByTypeSortedWithMiddleGetEntry(
        *me->map, m->elementsPerEntry, key,
        GET_MIDDLE(m, me->mapIdx, *me->map));
    return !!me->fe;
}

bool multimapFullGetUnderlyingEntryWithReference(
    multimapFull *m, const databox *key, multimapEntry *me,
    const multimapAtom *referenceContainer) {
    me->mapIdx = multimapFullKeyMap_(m, key, true, referenceContainer);
    me->map = getMapPtr(m, me->mapIdx);
    me->fe = multimapFullFindKeyInMap_(m, *me->map,
                                       GET_MIDDLE(m, me->mapIdx, *me->map), key,
                                       true, referenceContainer);
    return !!me->fe;
}

/* Entry mutations keep the entry COUNT, so the middle ROW is unchanged —
 * but the middle BYTE offset must be rescanned (a total-size varint
 * header boundary crossing shifts every entry, so no O(1) adjustment
 * from a pre-mutation offset is sound), and the rangeBox must be
 * refreshed (the mutation may realloc the flex, stranding a rangeBox
 * that references head-key bytes inside the old allocation). */
/* After an entry of 'me's map was rewritten in place: a map that changed
 * size has moved entries (the middle is found again); the range box
 * (the map's first key, which the rewrite may have been) is refreshed
 * either way */
static void multimapFullRewrote_(multimapFull *m, const multimapEntry *me,
                                 const size_t bytesBefore) {
    if (flexBytes(*me->map) != bytesBefore) {
        SET_MIDDLE_FORCE(m, me->mapIdx, *me->map);
    }

    updateRangeBoxForIdxAfterInMapMutation(m, me->mapIdx, *me->map);
}

void multimapFullResizeEntry(multimapFull *m, multimapEntry *me,
                             size_t newLen) {
    const size_t bytesBefore = flexBytes(*me->map);
    flexResizeEntry(me->map, me->fe, newLen);
    multimapFullRewrote_(m, me, bytesBefore);
}

void multimapFullReplaceEntry(multimapFull *m, multimapEntry *me,
                              const databox *box) {
    const size_t bytesBefore = flexBytes(*me->map);
    flexReplaceByType(me->map, me->fe, box);
    multimapFullRewrote_(m, me, bytesBefore);
}

/* Split only when the map holds MORE THAN ONE ROW (flexCount counts
 * elements, so one k/v row == elementsPerEntry elements).  A single
 * oversized row can never be made smaller by splitting — flexMiddle of
 * a one-row map is its head, so "splitting" moves the whole row to the
 * high half and leaves an EMPTY low half whose skipped rangeBox refresh
 * keeps referencing the realloc-freed pre-split allocation (a
 * use-after-free on the next range search).  conformMapsSplit and the
 * insert path both already refuse one-row splits for the same reason. */
bool multimapFullRegularizeMap(multimapFull *m, multimapFullIdx mapIdx,
                               flex **map) {
    if (flexCount(*map) > m->elementsPerEntry && flexBytes(*map) > m->maxSize) {
        splitMapLowHigh_(m, mapIdx, map, false, nullptr);
        return true;
    }

    return false;
}

bool multimapFullRegularizeMapWithReference(
    multimapFull *m, multimapFullIdx mapIdx, flex **map,
    const multimapAtom *referenceContainer) {
    if (flexCount(*map) > m->elementsPerEntry && flexBytes(*map) > m->maxSize) {
        splitMapLowHigh_(m, mapIdx, map, true, referenceContainer);
        return true;
    }

    return false;
}

/* ====================================================================
 * Exists API
 * ==================================================================== */
#ifndef MULTIMAP_EXISTS_MISS_DUMP
#define MULTIMAP_EXISTS_MISS_DUMP 0
#endif
bool multimapFullExists(multimapFull *m, const databox *key) {
    /* Two searches: the sub-map by its range box, then the row in it */
    const multimapFullIdx mapIdx = multimapFullBinarySearch(m, key);
    const flex *map = getMap(m, mapIdx);
    bool found = !!flexFindByTypeSortedWithMiddle(map, m->elementsPerEntry, key,
                                                  GET_MIDDLE(m, mapIdx, map));
#if MULTIMAP_EXISTS_MISS_DUMP
    /* Diagnostic: BinarySearch picked submap A; flexFind inside A
     * said "not present"; but a key the caller just inserted might
     * actually live in a different submap B (range-box drift) OR be
     * in submap A but missed by the find-in-flex path itself.  Walk
     * every submap to locate the actual home; dump submap geometry +
     * range boxes so the caller can correlate insert sequences with
     * structural state. */
    if (!found) {
        multimapFullIdx actualIdx = (multimapFullIdx)-1;
        for (multimapFullIdx i = 0; i < m->count; i++) {
            const flex *mi = getMap(m, i);
            const flexEntry *mid = flexMiddle((flex *)mi, m->elementsPerEntry);
            if (flexFindByTypeSortedWithMiddle(mi, m->elementsPerEntry, key,
                                               mid)) {
                actualIdx = i;
                break;
            }
        }
        fprintf(stderr,
                "[MULTIMAP_EXISTS_MISS_DUMP] mapIdx_chosen=%u "
                "actualIdx=%d total_submaps=%u total_values=%zu "
                "key.type=%d key.len=%u\n",
                (unsigned)mapIdx,
                actualIdx == (multimapFullIdx)-1 ? -1 : (int)actualIdx,
                (unsigned)m->count, (size_t)m->values, (int)key->type,
                (unsigned)key->len);
        for (multimapFullIdx i = 0; i < (m->count > 1 ? m->count - 1 : 0);
             i++) {
            const databox *rb = getRangeBox(m, i);
            fprintf(stderr,
                    "[MULTIMAP_EXISTS_MISS_DUMP]   rangeBox[%u]: "
                    "type=%d len=%u\n",
                    (unsigned)i, (int)rb->type, (unsigned)rb->len);
        }
        /* Walk every submap and report its bounds + iteration of its
         * contents.  When actualIdx == -1 but iteration of the whole
         * multimap finds the key, the bug is in the per-submap
         * flexFindByTypeSortedWithMiddle path. */
        for (multimapFullIdx i = 0; i < m->count; i++) {
            const flex *mi = getMap(m, i);
            const size_t mi_count = flexCount(mi) / m->elementsPerEntry;
            fprintf(stderr,
                    "[MULTIMAP_EXISTS_MISS_DUMP]   submap[%u]: "
                    "values=%zu bytes=%zu\n",
                    (unsigned)i, mi_count, (size_t)flexBytes(mi));
        }
        fflush(stderr);
    }
#endif
    return found;
}

bool multimapFullExistsFullWidth(multimapFull *m, const databox *elements[]) {
    const multimapFullIdx mapIdx =
        multimapFullBinarySearchFullWidth(m, elements);
    assert(mapIdx < m->count);
    const flex *map = getMap(m, mapIdx);
    return !!flexFindByTypeSortedWithMiddleFullWidth(
        map, m->elementsPerEntry, elements, GET_MIDDLE(m, mapIdx, map));
}

bool multimapFullExistsWithReference(const multimapFull *m, const databox *key,
                                     databox *foundRef,
                                     const multimapAtom *referenceContainer) {
    /* This *works* because when we have REFERENCE / surrogateKey maps, we
     * store the actual surrogateKey values in our range boxes, so lookups
     * here are against ACTUAL values instead of REFERENCE values. */
    const multimapFullIdx mapIdx = multimapFullBinarySearch(m, key);
    const flex *map = getMap(m, mapIdx);
    const flexEntry *found = flexFindByTypeSortedWithMiddleWithReference(
        map, m->elementsPerEntry, key, GET_MIDDLE(m, mapIdx, map),
        referenceContainer);

#if 0
        printf("Picked map: %u\n", mapIdx);
        databoxReprSay("For element:", key);
        printf("Got map: %p\n", map);
        printf("Got entry: %p\n", found);
#endif

    if (found) {
        flexGetByType(found, foundRef);
        return true;
    }

    return false;
}

/* ====================================================================
 * Single-Key Lookup API
 * ==================================================================== */
DK_INLINE_ALWAYS bool abstractLookup(multimapFull *m, const databox *key,
                                     databox *elements[],
                                     const bool useReference,
                                     const multimapAtom *referenceContainer) {
    multimapFullIdx mapIdx =
        multimapFullKeyMap_(m, key, useReference, referenceContainer);
    flex *map = getMap(m, mapIdx);
    flexEntry *middle = GET_MIDDLE(m, mapIdx, map);

    flexEntry *foundP = multimapFullFindKeyInMap_(
        m, map, middle, key, useReference, referenceContainer);

    if (foundP) {
        flexEntry *nextFound = foundP;
        for (multimapElements i = 1; i < m->elementsPerEntry; i++) {
            nextFound = flexNext(map, nextFound);
            flexGetByType(nextFound, elements[i - 1]);
        }
    }

    return !!foundP;
}

bool multimapFullLookup(multimapFull *m, const databox *key,
                        databox *elements[]) {
    return abstractLookup(m, key, elements, false, nullptr);
}

bool multimapFullLookupWithReference(multimapFull *m, const databox *key,
                                     databox *elements[],
                                     const multimapAtom *referenceContainer) {
    return abstractLookup(m, key, elements, true, referenceContainer);
}

/* ====================================================================
 * Delete API
 * ==================================================================== */
/* After a delete left sub-map 'mapIdx' of 'm' (at '*map') with fewer
 * rows: once empty it is freed and its slots dropped (reallocDecrCount
 * frees the range boxes it drops), else it is merged into a neighbour,
 * the next preferred and the previous as the fallback, so a shrunken
 * tail sub-map is still reclaimed.  A lone sub-map stays, empty or not,
 * for the next insert.  'referenceContainer': NULL for plain keys. */
static void
multimapFullRepairAfterDelete_(multimapFull *m, const multimapFullIdx mapIdx,
                               flex **map,
                               const multimapAtom *referenceContainer) {
    if (m->count <= 1) {
        return;
    }
    if (flexCount(*map) == 0) {
        flexFree(*map);
        reallocDecrCount(m, mapIdx);
        return;
    }
    const bool useReference = referenceContainer != nullptr;
    bool merged = false;
    if (nextMapIdxExists(m, mapIdx)) {
        merged = mergeSimple(m, mapIdx, useReference, referenceContainer);
    }
    if (!merged && mapIdx > 0) {
        mergeSimple(m, mapIdx - 1, useReference, referenceContainer);
    }
}

DK_INLINE_ALWAYS bool
abstractDelete(multimapFull *m, const databox **elements, const bool fullWidth,
               const bool useReference, const multimapAtom *referenceContainer,
               databox *foundReference, databox *removedOut,
               const bool takeRemoved) {
    assert(!useReference || (useReference && referenceContainer));

    /* Step 1: find matching map for range. */
    multimapFullIdx mapIdx;
    const databox *key = elements[0];

    if (fullWidth) {
        mapIdx = multimapFullBinarySearchFullWidth(m, elements);
    } else {
        mapIdx = multimapFullKeyMap_(m, key, useReference, referenceContainer);
    }

    flex **map = getMapPtr(m, mapIdx);

    bool deleted;

    if (fullWidth) {
        if (useReference) {
            deleted = multimapFullFlexDeleteFullWidthWithReference(
                m, mapIdx, elements, referenceContainer, foundReference);
        } else {
            deleted = multimapFullFlexDeleteFullWidthWithFound(
                m, mapIdx, elements, foundReference);
        }
    } else {
        if (useReference) {
            deleted = multimapFullFlexDeleteWithReference(
                m, mapIdx, key, referenceContainer, foundReference);
        } else {
            deleted = multimapFullFlexDeleteWithFound(
                m, mapIdx, key, foundReference, removedOut, takeRemoved);
        }
    }

    /* Step 2: the sub-map after the delete */
    if (deleted) {
        multimapFullRepairAfterDelete_(
            m, mapIdx, map, useReference ? referenceContainer : nullptr);
    }

    return deleted;
}

void multimapFullDeleteEntry(multimapFull *m, multimapEntry *me) {
    multimapFullDeleteEntryWithReference(m, me, nullptr);
}

/* 'referenceContainer' (NULL for a map of plain keys) resolves the keys of
 * a reference map, whose range boxes hold copies of resolved keys */
void multimapFullDeleteEntryWithReference(
    multimapFull *m, multimapEntry *me,
    const multimapAtom *referenceContainer) {
    /* Step 1: pick victim map */
    const multimapFullIdx mapIdx = me->mapIdx;

    flex **map = me->map;
    flexEntry *middle = GET_MIDDLE(m, mapIdx, *map);

    /* Step 2: pick victim element */
    flexEntry *foundP = me->fe;

    flexDeleteSortedValueWithMiddle(map, m->elementsPerEntry, foundP, &middle);

    /* Step 3a: the counts, middle and range box after the row delete (as
     * abstractFlexDelete keeps them) */
    m->values--;
    SET_MIDDLE(m, mapIdx, middle, *map);

    if (referenceContainer) {
        updateRangeBoxForIdxWithReference(m, mapIdx, *map, referenceContainer);
    } else {
        updateRangeBoxForIdx(m, mapIdx, *map);
    }

    /* Step 3b: the sub-map after the delete */
    multimapFullRepairAfterDelete_(m, mapIdx, map, referenceContainer);
}

bool multimapFullDelete(multimapFull *m, const databox *key) {
    const databox *elements[1] = {key};
    return abstractDelete(m, elements, false, false, nullptr, nullptr, nullptr,
                          false);
}

bool multimapFullDeleteReporting(multimapFull *m, const databox *key,
                                 databox *removedOut) {
    const databox *elements[1] = {key};
    return abstractDelete(m, elements, false, false, nullptr, nullptr,
                          removedOut, false);
}

bool multimapFullDeleteTaking(multimapFull *m, const databox *key,
                              databox *takenOut) {
    const databox *elements[1] = {key};
    return abstractDelete(m, elements, false, false, nullptr, nullptr, takenOut,
                          true);
}

bool multimapFullDeleteNMapsIterate(multimapFull *m, const size_t n,
                                    multimapFullMapDeleter *mapIter,
                                    void *data) {
    for (size_t i = 0; i < n && m->values > 0; i++) {
        /* Always the lowest map: its rows leave the count with it */
        flex **map = getMapPtr(m, 0);
        if (mapIter) {
            mapIter(*map, data);
        }
        m->values -= flexCount(*map) / m->elementsPerEntry;

        /* The last map empties in place: a multimapFull always keeps one */
        if (m->count == 1) {
            flexReset(map);
            SET_MIDDLE_FORCE(m, 0, *map);
            break;
        }

        /* Free map 0, then its range box and middle */
        flexFree(*map);
        reallocDecrCount(m, 0);
    }

    /* True while rows remain for a later call to delete */
    return m->values > 0;
}

bool multimapFullDeleteNMaps(multimapFull *m, const size_t n) {
    return multimapFullDeleteNMapsIterate(m, n, nullptr, nullptr);
}

bool multimapFullDeleteFullWidth(multimapFull *m, const databox *elements[]) {
    return abstractDelete(m, elements, true, false, nullptr, nullptr, nullptr,
                          false);
}

bool multimapFullDeleteFullWidthWithFound(multimapFull *m,
                                          const databox *elements[],
                                          databox *foundReference) {
    return abstractDelete(m, elements, true, false, nullptr, foundReference,
                          nullptr, false);
}

bool multimapFullDeleteWithReference(multimapFull *m, const databox *key,
                                     const multimapAtom *referenceContainer,
                                     databox *foundReference) {
    const databox *elements[1] = {key};
    return abstractDelete(m, elements, false, true, referenceContainer,
                          foundReference, nullptr, false);
}

bool multimapFullDeleteWithFound(multimapFull *m, const databox *key,
                                 databox *foundReference) {
    const databox *elements[1] = {key};
    return abstractDelete(m, elements, false, false, nullptr, foundReference,
                          nullptr, false);
}

bool multimapFullRandomValue(multimapFull *m, const bool fromTail,
                             databox **foundBox, multimapEntry *me) {
    if (m->values == 0) {
        return false;
    }

    /* Uniform selection: every row is equally likely.  Pick a global row
     * with a per-thread uniform draw, then locate it through the
     * bidirectional map scan + middle-anchored flex walk.  fromTail always
     * addresses the container's last row (see multimapFull.h). */
    const int64_t row =
        fromTail ? (int64_t)m->values - 1 : (int64_t)dkRandomBounded(m->values);

    multimapFullIdx mapIdx;
    flex **map;
    flexEntry *foundP = multimapFullRowByPosition_(m, row, &mapIdx, &map);
    assert(foundP);

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

bool multimapFullDeleteRandomValue(multimapFull *m, const bool deleteFromTail,
                                   databox **deletedBox) {
    multimapEntry me;
    /* Pass NULL for deletedBox to multimapFullRandomValue, then manually
     * copy the data using flexGetByTypeDetached. This avoids use-after-free
     * since flexGetByType returns pointers to internal flex data which
     * become invalid after multimapFullDeleteEntry. */
    if (!multimapFullRandomValue(m, deleteFromTail, nullptr, &me)) {
        return false;
    }

    /* Copy the data before deletion so caller has valid data */
    if (deletedBox) {
        flexEntry *fe = me.fe;
        for (size_t i = 0; i < m->elementsPerEntry; i++) {
            flexGetByTypeDetached(fe, deletedBox[i]);
            fe = flexNext(*me.map, fe);
        }
    }

    multimapFullDeleteEntry(m, &me);
    return true;
}

/* ====================================================================
 * Reset API
 * ==================================================================== */
/* A reference map's rows hold their referents' ids without retaining
 * them (the referents' owner, as multimapAtom for its index, keeps their
 * counts), so a reset or free releases nothing */
void multimapFullReset(multimapFull *m) {
    /* Collapse back to a single empty map. Emptied maps cannot keep
     * their search metadata: range boxes reference bytes inside the
     * (now discarded) flex contents, so retaining multiple empty maps
     * would leave later lookups and inserts walking dangling pointers. */
    while (m->count > 1) {
        const multimapFullIdx idx = m->count - 1;
        flexFree(getMap(m, idx));
        reallocDecrCount(m, idx);
    }

    flex **map = getMapPtr(m, 0);
    flexReset(map);
    SET_MIDDLE_FORCE(m, 0, *map);

    /* We deleted all elements, so reset the cached
     * entry count back to zero. */
    m->values = 0;
}

/* ====================================================================
 * Free / Release API
 * ==================================================================== */
void multimapFullFree(multimapFull *const m) {
    if (m) {
        /* Free each map */
        for (multimapFullIdx i = 0; i < m->count; i++) {
            flexFree(getMap(m, i));
        }

        /* Free array of pointers to maps */
        multiarrayNativeFree(m->map);

        /* Free array of middle offsets */
        multiarrayNativeFree(m->middle);

        /* Free each range box (if necessary) */
        if (m->count > 0) {
            for (multimapFullIdx i = 0; i < m->count - 1; i++) {
                databoxFreeData(getRangeBox(m, i));
            }
        }

        /* Free array of range databoxes */
        multiarrayNativeFree(m->rangeBox);

        /* Free the multimapFull container itself */
        zfree(m);
    }
}

/* ====================================================================
 * First / Last
 * ==================================================================== */
bool multimapFullFirst(multimapFull *m, databox *elements[]) {
    if (m->values == 0) {
        return false;
    }

    flex *useMap = getMapLowest(m);
    flexEntry *current = flexHead(useMap);
    for (multimapElements i = 0; i < m->elementsPerEntry; i++) {
        /* Populate forward */
        flexGetByType(current, elements[i]);
        current = flexNext(useMap, current);
    }

    return true;
}

bool multimapFullLast(multimapFull *m, databox *elements[]) {
    if (m->values == 0) {
        return false;
    }

    flex *useMap = getMapHighest(m);
    flexEntry *current = flexTail(useMap);
    for (multimapElements i = 0; i < m->elementsPerEntry; i++) {
        /* Populate reverse */
        flexGetByType(current, elements[(m->elementsPerEntry - 1) - i]);
        current = flexPrev(useMap, current);
    }

    return true;
}

DK_INLINE_ALWAYS bool multimapFullIteratorInitAt_(multimapFull *m,
                                                  multimapIterator *iter,
                                                  bool forward,
                                                  const multimapEntry *me) {
    iter->mm = m;
    iter->forward = forward;
    iter->elementsPerEntry = m->elementsPerEntry;
    iter->type = MULTIMAP_TYPE_FULL;
    iter->mapIndex = me->mapIdx;
    iter->map = *me->map;
    iter->entry = me->fe;
    return true;
}

bool multimapFullIteratorInitAt(multimapFull *m, multimapIterator *iter,
                                bool forward, const databox *box) {
    multimapEntry me;
    multimapFullGetUnderlyingEntryGetEntry(m, box, &me);

    /* If we iterated past all elements, we can't iterate any more... */
    flex *next = nullptr;
    if (me.fe == *me.map + flexBytes(*me.map)) {
        /* ...unless the position merely ran off the end of one map while
         * a later map holds the keys above the search point (interior maps
         * are never empty) */
        if (forward && me.mapIdx + 1 < m->count) {
            me.mapIdx++;
            next = getMap(m, me.mapIdx);
            me.map = &next;
            me.fe = flexHead(next);
        } else {
            me.fe = nullptr;
        }
    }

    multimapFullIteratorInitAt_(m, iter, forward, &me);
    return !!me.fe;
}

bool multimapFullIteratorInit(multimapFull *m, multimapIterator *iter,
                              bool forward) {
    flex *tmp = nullptr;
    multimapEntry me;

    if (likely(m->values)) {
        if (likely(forward)) {
            tmp = getMapLowest(m);
            me.mapIdx = 0;
            me.fe = flexHead(tmp);
            me.map = &tmp;
        } else {
            tmp = getMapHighest(m);
            me.mapIdx = m->count - 1;
            me.fe = flexTail(tmp);
            me.map = &tmp;
        }
    } else {
        /* else, we have no values, so we can't iterate, so signal iterator to
         * abandon at the first Next() call. */
        me.fe = nullptr;
        me.mapIdx = m->count - 1;
        me.map = &tmp;
    }

    return multimapFullIteratorInitAt_(m, iter, forward, &me);
}

DK_INLINE_ALWAYS bool multimapFullIteratorNext_(multimapIterator *iter,
                                                databox *elements[],
                                                databoxWide *slots) {
    for (;;) {
        if (iter->entry) {
            flexEntry *current = iter->entry;
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
                    const multimapElements at =
                        (iter->elementsPerEntry - 1) - i;
                    multimapIteratorDecode_(current, elements[at], slots, at);
                    current = flexPrev(iter->map, current);
                }

                iter->entry = current;
            }

            return true;
        }

        /* If moving forward and reached end of map,
         * begin iterating over next map. */
        if (iter->forward) {
            multimapFull *local = (multimapFull *)iter->mm;
            iter->mapIndex++;
            if (iter->mapIndex < local->count) {
                iter->map = getMap(local, iter->mapIndex);
                iter->entry = flexHead(iter->map);

                /* We DO NOT allow empty maps, so we MUST have a head of a
                 * map. */
                assert(flexCount(iter->map) &&
                       "multimapFull interior map is empty?");
                continue;
            }

            /* ran out of maps to iterate! */
            return false;
        }

        /* If moving reverse and reached beginning of map,
         * begin iterating over previous map (if exists). */
        /* Note: we check BEFORE decrementing to ensure we process map 0.
         * If we decrement first, then check > 0, we would skip map 0. */
        if (iter->mapIndex > 0) {
            iter->mapIndex--;
            multimapFull *local = (multimapFull *)iter->mm;
            iter->map = getMap(local, iter->mapIndex);
            iter->entry = flexTail(iter->map);
            continue;
        }

        return false;
    }
}

bool multimapFullIteratorNext(multimapIterator *iter, databox *elements[]) {
    return multimapFullIteratorNext_(iter, elements, nullptr);
}

bool multimapFullIteratorNextWithSlots(multimapIterator *iter,
                                       databox *elements[],
                                       databoxWide *slots) {
    if (!slots) {
        return multimapFullIteratorNext_(iter, elements, nullptr);
    }

    return multimapFullIteratorNext_(iter, elements, slots);
}

bool multimapFullDeleteByPredicate(multimapFull *m,
                                   const multimapPredicate *p) {
    if (p->condition != MULTIMAP_CONDITION_LESS_THAN_EQUAL) {
        /* A tier trims a prefix (<=) only; multimapDeleteByPredicate
         * handles every other condition */
        assert(false && "Not Implemented!");
        return false;
    }

    multimapEntry me = {};
    /* Boundary (position) search, NOT exact-match: "delete all <= X"
     * must work for any X, not only when X is an existing key. */
    multimapFullGetUnderlyingEntryGetEntry(m, &p->compareAgainst, &me);
    if (!me.fe) {
        return false;
    }

    /* Freeing whole sub-maps only decrements the sub-map count, so
     * every removed row must be subtracted from m->values (the element
     * count multimapFullCount returns) explicitly. Every sub-map below
     * the boundary sub-map is entirely <= boundary; free them
     * wholesale ('0' each time, since deleting '0' promotes the next
     * map to '0'). */
    size_t removed = 0;
    for (multimapFullIdx i = 0; i < me.mapIdx; i++) {
        flex *map = getMap(m, 0);
        removed += flexCount(map) / m->elementsPerEntry;
        flexFree(map);
        reallocDecrCount(m, 0);
    }

    /* Re-find in the now-lowest sub-map (the wholesale deletes above
     * moved indices). Everything left to delete lives in sub-map 0. */
    multimapFullGetUnderlyingEntryGetEntry(m, &p->compareAgainst, &me);
    assert(me.mapIdx == 0);
    if (!me.fe) {
        m->values -= removed;
        return removed > 0;
    }

    /* Compare NOW, against the entry actually found post-coarse: an
     * exact match deletes up to AND including it; anything else (the
     * next-higher key, or the end-of-map sentinel when the boundary is
     * >= every remaining key) deletes strictly below it. Computing this
     * against a stale pre-coarse entry could pair 'exact' with an
     * end-sentinel position and walk past the buffer. */
    const size_t before = flexCount(*me.map) / m->elementsPerEntry;
    int compared = 1;
    if (flexEntryIsValid(*me.map, me.fe)) {
        databox value;
        databoxWide valueWide;
        flexGetByTypeInto(me.fe, &value, &valueWide);
        compared = databoxCompare(&value, &p->compareAgainst);
    }

    /* see multimapSmall for comments */
    if (compared == 0) {
        flexDeleteUpToInclusivePlusN(me.map, me.fe, m->elementsPerEntry - 1);
    } else {
        flexDeleteUpToInclusive(me.map, flexPrev(*me.map, me.fe));
    }

    removed += before - flexCount(*me.map) / m->elementsPerEntry;
    m->values -= removed;

    /* Trimming can empty the lowest sub-map entirely (boundary >= all
     * its keys). An empty LEADING sub-map is an invalid Full state -
     * First and the binary search assume sub-map 0 is populated - so
     * reclaim it and promote the next sub-map into its place. When it
     * is the only sub-map, an empty map is the legitimate empty-map
     * state and is kept. */
    if (flexCount(*me.map) == 0 && m->count > 1) {
        flexFree(*me.map);
        reallocDecrCount(m, 0);
    } else {
        SET_MIDDLE_FORCE(m, me.mapIdx, *me.map);
    }

    return true;
}

/* The key of the first (or, 'last', the last) row of 'map' compared with
 * 'against' */
static int multimapFullCompareRowKey_(const flex *map,
                                      uint32_t elementsPerEntry, bool last,
                                      const databox *against) {
    const flexEntry *fe =
        last ? flexIndex(map, -(int32_t)elementsPerEntry) : flexHead(map);
    databox key;
    databoxWide keyWide;
    flexGetByTypeInto(fe, &key, &keyWide);
    return databoxCompare(&key, against);
}

/* Rows with keys in [lo, hi]: the sub-map holding lo and those after it,
 * up to the first that reaches past hi.  A sub-map wholly inside is freed
 * whole; the edge sub-maps lose their rows in one move each; the edges
 * are then repaired as one delete leaves them.  O(log n + sub-maps
 * touched + rows in the edge sub-maps). */
size_t multimapFullDeleteRange(multimapFull *m, const databox *lo,
                               const databox *hi) {
    if (!m->values || (lo && hi && databoxCompare(lo, hi) > 0)) {
        return 0;
    }

    const uint32_t epe = m->elementsPerEntry;
    const multimapFullIdx first =
        lo ? multimapFullKeyMap_(m, lo, false, nullptr) : 0;
    multimapFullIdx lastTouched = first;
    size_t removed = 0;
    for (multimapFullIdx i = first; i < m->count;) {
        flex **map = getMapPtr(m, i);
        if (i > first) {
            if (hi && multimapFullCompareRowKey_(*map, epe, false, hi) > 0) {
                break; /* this sub-map starts past the range */
            }

            if (!hi || multimapFullCompareRowKey_(*map, epe, true, hi) <= 0) {
                removed += flexCount(*map) / epe;
                flexFree(*map);
                reallocDecrCount(m, i);
                continue; /* the next sub-map is now at i */
            }
        }

        const size_t rows =
            flexDeleteSortedKeyRange(map, epe, lo, hi, GET_MIDDLE(m, i, *map));
        if (rows) {
            removed += rows;
            SET_MIDDLE_FORCE(m, i, *map);
            updateRangeBoxForIdx(m, i, *map);
        }

        lastTouched = i;
        if (hi && flexCount(*map) &&
            multimapFullCompareRowKey_(*map, epe, true, hi) > 0) {
            break; /* rows past hi remain here: the range ends in it */
        }
        i++;
    }

    if (!removed) {
        return 0;
    }

    m->values -= removed;

    /* The far edge first: repairing it never moves the near one */
    if (lastTouched > first && lastTouched < m->count) {
        multimapFullRepairAfterDelete_(m, lastTouched,
                                       getMapPtr(m, lastTouched), nullptr);
    }
    if (first < m->count) {
        multimapFullRepairAfterDelete_(m, first, getMapPtr(m, first), nullptr);
    }

    return removed;
}

#ifdef DATAKIT_TEST
#define CTEST_INCLUDE_KVGEN
#include "ctest.h"

#include "multimap.h" /* the cross-tier sweeps and the allocation counter */

#define DOUBLE_NEWLINE 0
#include "perf.h"

#define multimapFullReport(m) multimapFullReport_(m, true)
#define multimapFullReportSizeOnly(m) multimapFullReport_(m, false)

static void multimapFullConforms(const multimapFull *m) {
    for (multimapFullIdx i = 0; i < m->count; i++) {
        if (i > 0 && flexCount(getMap(m, i)) == 0) {
            multimapFullRepr(m);
            assert(false && "Interior empty map? How'd we do that?");
            unreachable();
        }
    }
}

void multimapFullRepr(const multimapFull *m) {
    printf("MAPS {totalMaps %d} {totalCount %" PRIu64 "} {maxBytesPerMap %d}\n",
           m->count, m->values, m->maxSize);

    for (multimapFullIdx i = 0; i < m->count - 1; i++) {
        printf("rangebox %d ", i);
        const databox *got = getRangeBox(m, i);
        databoxReprSay("is", got);
    }

    printf("Map Counts: ");
    for (multimapFullIdx i = 0; i < m->count; i++) {
        const size_t elementsInMap =
            flexCount(getMap(m, i)) / m->elementsPerEntry;
        if (i != m->count - 1) {
            printf("[%zu] -> ", elementsInMap);
        } else {
            printf("[%zu]\n", elementsInMap);
        }
    }

    for (multimapFullIdx i = 0; i < m->count; i++) {
        printf("MAP: %d\n", i);
        flex *useMap = getMap(m, i);

        flexRepr(useMap);
#if 0
        if (i > 0 && flexCount(getMap(m, i)) == 0) {
            assert(false && "Interior empty map? How'd we do that?");
            unreachable();
        }

#endif
    }

    strictConsistencyCheck(m);
}

static size_t multimapFullReport_(const multimapFull *m, const bool print) {
    size_t Bytes = 0;
    size_t Count = 0;
    for (multimapFullIdx q = 0; q < m->count; q++) {
        Bytes += flexBytes(getMap(m, q));
        Count += flexCount(getMap(m, q));
    }

    size_t rangeBoxBytes = sizeof(databox) * (m->count - 1);
    size_t middleBytes = sizeof(multimapFullMiddle) * m->count;
    size_t mapPtrBytes = sizeof(flex *) * m->count;
    size_t containerBytes = sizeof(*m);
    size_t externalMetadataBytes =
        rangeBoxBytes + middleBytes + mapPtrBytes + containerBytes;
    size_t totalBytes = Bytes + externalMetadataBytes;
    double externalMetadataOverhead =
        (double)(externalMetadataBytes) / (totalBytes);
    if (print) {
        printf("[L] {bytes {total %zu} {data %zu}} {maps %d} {per map {%0.2f "
               "elements} {%0.2f bytes}}\n"
               "{overhead %0.2f%% {bytes %zu {%zu pointer} {%zu rangebox} "
               "{%zu middle} {%zu struct}}\n\n",
               totalBytes, Bytes, m->count,
               m->count ? (double)Count / m->count : 0,
               m->count ? (double)Bytes / m->count : 0,
               externalMetadataOverhead * 100, externalMetadataBytes,
               mapPtrBytes, rangeBoxBytes, middleBytes, containerBytes);
    }

    fflush(stdout);

    return totalBytes;
}

/* Iterate over entire multimapFull and verify keys are sorted correctly. */
void multimapFullVerify(const multimapFull *m) {
    if (m->count == 0) {
        return;
    }

    /* 'lowest' and 'next' decode into alternating slots, so 'lowest' keeps
     * its wide payload while the next key is read. */
    databoxWide keyWide[2];
    uint32_t slot = 0;
    databox lowest;
    size_t lowestMapIdx = 0;
    flexGetByTypeInto(getMapLowest(m), &lowest, &keyWide[slot]);
    slot ^= 1;

    for (multimapFullIdx i = 0; i < m->count; i++) {
        flex *zl = getMap(m, i);
        flexEntry *middle = GET_MIDDLE(m, i, zl);

        assert(middle == flexMiddle(zl, m->elementsPerEntry));

        /* rangeBox oracle: rangeBox[i - 1] routes searches into map[i],
         * so it must equal map[i]'s live head key, and when it holds a
         * borrowed bytes pointer, that pointer must live INSIDE the
         * current flex allocation (a stale pointer into a freed/moved
         * allocation is a use-after-free waiting for the next search).
         * A 96/128-bit key must live in a heap payload the rangeBox owns,
         * never inside the map. */
        if (i > 0 && flexCount(zl) > 0) {
            databox head;
            databoxWide headWide;
            flexGetByTypeInto(flexHead(zl), &head, &headWide);

            const databox *rangeBox = getRangeBox(m, i - 1);
            if (head.type != DATABOX_CONTAINER_REFERENCE_EXTERNAL) {
                assert(databoxCompare(&head, rangeBox) == 0);
                if (rangeBox->type == DATABOX_BYTES && !rangeBox->allocated &&
                    !DATABOX_IS_BYTES_EMBED(rangeBox)) {
                    const uint8_t *start = rangeBox->data.bytes.start;
                    assert(start >= zl && start < zl + flexBytes(zl));
                }
            }

            if (rangeBox->type == DATABOX_SIGNED_128 ||
                rangeBox->type == DATABOX_UNSIGNED_128) {
                const uint8_t *payload = rangeBox->data.ptr;
                assert(rangeBox->big && rangeBox->allocated && payload);
                assert(payload < zl || payload >= zl + flexBytes(zl));
            } else {
                assert(!rangeBox->big);
            }
        }

        for (multimapFullIdx j = 0; j < flexCount(zl);
             j += m->elementsPerEntry) {
            databox next;
            flexGetByTypeInto(flexIndex(zl, j), &next, &keyWide[slot]);
            slot ^= 1;

            if (i > 0 && databoxCompare(&lowest, &next) > 0) {
                printf("Lowest was previously %.*s from map %zu, but found "
                       "element %.*s at (%d, %d)\n",
                       (int)lowest.len, lowest.data.bytes.start, lowestMapIdx,
                       (int)next.len, next.data.bytes.start, i, j);
                assert(false);
                unreachable();
            }

            lowest = next;
            lowestMapIdx = i;
        }
    }
}

/* Every rangeBox equals its map's head key.  A wide one owns a heap payload
 * outside every map (big = 1, allocated = 1); a narrow one owns nothing.
 * Returns the number of failures. */
static int32_t rangeBoxOwnershipCheck_(const char *where,
                                       const multimapFull *m) {
    int32_t err = 0;
    for (multimapFullIdx i = 1; i < m->count; i++) {
        databox head;
        databoxWide headWide;
        flexGetByTypeInto(flexHead(getMap(m, i)), &head, &headWide);
        const databox *rb = getRangeBox(m, i - 1);
        if (databoxCompare(&head, rb) != 0) {
            ERR("%s: rangeBox[%" PRIu32 "] differs from its map's head", where,
                i - 1);
        }

        if (!rangeBoxIsWide_(rb)) {
            if (rb->big || rb->allocated) {
                ERR("%s: narrow rangeBox[%" PRIu32 "] claims a payload", where,
                    i - 1);
            }

            continue;
        }

        if (!rb->big || !rb->allocated || !rb->data.ptr) {
            ERR("%s: wide rangeBox[%" PRIu32 "] doesn't own its payload", where,
                i - 1);
            continue;
        }

        const uint8_t *payload = rb->data.ptr;
        for (multimapFullIdx j = 0; j < m->count; j++) {
            const flex *map = getMap(m, j);
            if (payload >= map && payload < map + flexBytes(map)) {
                ERR("%s: wide rangeBox[%" PRIu32 "] points into map %" PRIu32,
                    where, i - 1, j);
            }
        }
    }

    return err;
}

/* Check a surrogate-keyed 1-element map whose rows are atom ids
 * 0..count-1 naming vals[0..count-1] in 'ma', against the oracle
 * 'present':
 *   - every rangeBox equals its map's resolved head key;
 *   - the rows resolve to strictly ascending values, one per present id;
 *   - every id is found by its value exactly when present.
 * Returns the number of failures. */
static int32_t surrogateCopyCheck_(const char *where, const multimapFull *m,
                                   const multimapAtom *ma, const databox *vals,
                                   const bool *present, const size_t count) {
    int32_t err = 0;
    for (multimapFullIdx i = 1; i < m->count; i++) {
        databox head = {};
        databoxWide headWide;
        multimapGetByTypeInto_(flexHead(getMap(m, i)), &head, &headWide, ma);
        if (databoxCompare(&head, getRangeBox(m, i - 1)) != 0) {
            ERR("%s: rangeBox[%" PRIu32 "] is not map %" PRIu32
                "'s resolved head",
                where, i - 1, i);
        }
    }

    size_t want = 0;
    for (size_t id = 0; id < count; id++) {
        want += present[id];
    }

    databoxWide rowWide[2];
    uint32_t slot = 0;
    databox prev = {};
    size_t rows = 0;
    for (multimapFullIdx i = 0; i < m->count; i++) {
        const flex *map = getMap(m, i);
        for (flexEntry *fe = flexHead(map); fe; fe = flexNext(map, fe)) {
            databox row = {};
            multimapGetByTypeInto_(fe, &row, &rowWide[slot], ma);
            slot ^= 1;
            if (rows && databoxCompare(&prev, &row) >= 0) {
                ERR("%s: row %zu is not above the row before it", where, rows);
            }

            prev = row;
            rows++;
        }
    }

    if (rows != want || multimapFullCount(m) != want) {
        ERR("%s: %zu rows walked, %zu counted, %zu expected", where, rows,
            multimapFullCount(m), want);
    }

    for (size_t id = 0; id < count; id++) {
        databox found = {};
        const bool exists =
            multimapFullExistsWithReference(m, &vals[id], &found, ma);
        if (exists != present[id]) {
            ERR("%s: id %zu exists %d, expected %d", where, id, exists,
                present[id]);
        } else if (exists &&
                   (found.type != DATABOX_CONTAINER_REFERENCE_EXTERNAL ||
                    found.data.u != id)) {
            ERR("%s: value of id %zu found under another id", where, id);
        }
    }

    return err;
}

/* Sizes for the build-up sweep: every size to 256, then steps of about
 * 6%.  Building a map to size i crosses every internal split below i (and
 * deleting it crosses them back), so sampled sizes keep the coverage of
 * the old every-size sweep at about 1/80 of its work. */
static int32_t multimapFullTestNextSize_(int32_t i) {
    return i < 256 ? i + 1 : i + i / 16;
}

int multimapFullTest(int, char *[]) {
    int err = 0;

    TEST("16-byte rangeBoxes own wide boundary keys and reuse them in place") {
        const __uint128_t two64 = (__uint128_t)1 << 64;
        /* Ascending: Wn(9) < ... < Wn(2) < N(1) < N(5) < N(6) < Wp(1) <
         * ... < Wp(9).  Wn is below -2^64, Wp at or above 2^64. */
#define RB_WN(k)                                                               \
    ((databox){.type = DATABOX_SIGNED_128,                                     \
               .big = true,                                                    \
               .data.i128 = &(__int128_t){-(__int128_t)two64 - (k)}})
#define RB_WP(k)                                                               \
    ((databox){.type = DATABOX_UNSIGNED_128,                                   \
               .big = true,                                                    \
               .data.u128 = &(__uint128_t){two64 + (k)}})
#define RB_N(k) databoxNewUnsigned(k)

        multimapTestAllocCountBegin();
        {
            /* map0 [Wn9]  map1 [Wn3 Wn2 N1]  map2 [N5 N6 Wp1]
             * map3 [Wp5 Wp6 Wp7]  map4 [Wp9] */
            const databox keys[][3] = {
                {RB_WN(9)},
                {RB_WN(3), RB_WN(2), RB_N(1)},
                {RB_N(5), RB_N(6), RB_WP(1)},
                {RB_WP(5), RB_WP(6), RB_WP(7)},
                {RB_WP(9)},
            };
            const size_t rows[] = {1, 3, 3, 3, 1};
            /* @own: container; free=multimapFullFree */
            flex *maps[5];
            multimapFullMiddle middles[5] = {};
            const databox val = databoxNewUnsigned(7);
            for (size_t i = 0; i < 5; i++) {
                /* @own: container; free=multimapFullFree */
                maps[i] = flexNew();
                for (size_t r = 0; r < rows[i]; r++) {
                    flexPushByType(&maps[i], &keys[i][r], FLEX_ENDPOINT_TAIL);
                    flexPushByType(&maps[i], &val, FLEX_ENDPOINT_TAIL);
                }
            }

            /* @own: caller; free=multimapFullFree */
            multimapFull *m = multimapFullNewFromManyGrow(nullptr, maps,
                                                          middles, 5, 2, false);
            /* No two maps fit together, so deletes never merge them. */
            m->maxSize = 1;
            err += rangeBoxOwnershipCheck_("built", m);

            const void *p0 = getRangeBox(m, 0)->data.ptr;
            const void *p2 = getRangeBox(m, 2)->data.ptr;

            /* Deleting a head refreshes the rangeBox: wide to wide reuses
             * the payload, wide to narrow frees it. */
            databox k = RB_WN(3);
            multimapFullDelete(m, &k);
            err += rangeBoxOwnershipCheck_("delete Wn3", m);
            if (getRangeBox(m, 0)->data.ptr != p0) {
                ERR("wide to wide refresh moved the payload");
            }

            k = RB_WN(2);
            multimapFullDelete(m, &k);
            err += rangeBoxOwnershipCheck_("delete Wn2", m);
            if (getRangeBox(m, 0)->type != DATABOX_UNSIGNED_64) {
                ERR("wide to narrow refresh kept a wide rangeBox");
            }

            /* Narrow to narrow, then narrow to wide allocates. */
            k = RB_N(5);
            multimapFullDelete(m, &k);
            err += rangeBoxOwnershipCheck_("delete N5", m);
            k = RB_N(6);
            multimapFullDelete(m, &k);
            err += rangeBoxOwnershipCheck_("delete N6", m);
            if (getRangeBox(m, 1)->type != DATABOX_UNSIGNED_128) {
                ERR("narrow to wide refresh didn't take the wide key");
            }

            /* An insert above a wide head splits that map; the lower half
             * keeps its rangeBox, refreshed in place. */
            const databox k8 = RB_WP(8);
            const databox *row[2] = {&k8, &val};
            multimapFullInsert(m, row);
            err += rangeBoxOwnershipCheck_("insert Wp8", m);
            if (getRangeBox(m, 2)->data.ptr != p2) {
                ERR("insert refresh moved the wide payload");
            }

            /* A copy rebuilds its own payloads (freed at the block's end,
             * inside the counting window). */
            {
                cleaner(multimapFullFree) multimapFull *copy =
                    multimapFullCopy(m);
                err += rangeBoxOwnershipCheck_("copy", copy);
                if (copy->count != m->count) {
                    ERR("copy has %" PRIu32 " maps, original %" PRIu32,
                        copy->count, m->count);
                } else {
                    for (multimapFullIdx i = 0; i + 1 < m->count; i++) {
                        const databox *a = getRangeBox(m, i);
                        const databox *b = getRangeBox(copy, i);
                        if (rangeBoxIsWide_(a) && a->data.ptr == b->data.ptr) {
                            ERR("copy shares rangeBox[%" PRIu32 "]'s payload",
                                i);
                        }
                    }
                }
            }

            /* Deleting a whole map above map[0], then map[0] itself while
             * rangeBox[0] holds a wide key, then trimming whole leading
             * maps: each removal frees its rangeBox. */
            k = RB_N(1);
            multimapFullDelete(m, &k);
            err += rangeBoxOwnershipCheck_("delete map1", m);
            if (!rangeBoxIsWide_(getRangeBox(m, 0))) {
                ERR("rangeBox[0] should hold Wp1 before map 0 goes");
            }

            k = RB_WN(9);
            multimapFullDelete(m, &k);
            err += rangeBoxOwnershipCheck_("delete map0", m);

            const multimapPredicate trim = {
                .condition = MULTIMAP_CONDITION_LESS_THAN_EQUAL,
                .compareAgainst = RB_WP(6)};
            multimapFullDeleteByPredicate(m, &trim);
            err += rangeBoxOwnershipCheck_("trim", m);
            if (multimapFullCount(m) != 3) {
                ERR("trim left %zu rows, expected 3 (Wp7 Wp8 Wp9)",
                    multimapFullCount(m));
            }

            multimapFullFree(m);
        }

        const int64_t live = multimapTestAllocCountEnd();
        if (live != 0) {
            ERR("%" PRId64 " allocations outlived the map", live);
        }

#undef RB_WN
#undef RB_WP
#undef RB_N
    }

#if 1
    TEST("flex validators are total over every single-byte mutation") {
        /* Every sub-map is a flex, so this suite depends on the flex
         * validators and runs their sweep (defined in flex.c) as well. */
        err += flexValidateTotalitySweep();
    }

    TEST("replacing inserts report the replaced tuple at every layer") {
        /* The Full tier's replacing insert depends on the flex replacing
         * insert, so this suite runs the cross-layer sweep (defined in
         * multimap.c) as well. */
        err += multimapInsertReplacingSweep();
    }

    TEST("96/128-bit keys and values through every tier") {
        err += multimapWideSweep();
    }

    TEST("normal / extended / mixed matrix through every tier") {
        err += multimapWideMatrixSweep();
    }

    TEST("normal / extended / mixed persist round trips") {
        err += multimapWidePersistSweep();
    }

    TEST("create") {
        cleaner(multimapFullFree) multimapFull *m = multimapFullNew(2);
        if (multimapFullCount(m) != 0) {
            ERR("a new map holds %zu rows", multimapFullCount(m));
        }
    }

    TEST("binary-search-vs-iteration agreement on mixed-length numeric keys") {
        /* Regression: an earlier databoxCompareBytes_ natural sort
         * ordered mixed-length numeric-suffix strings ("g_0_..",
         * "g_1_..", ..., "g_10_..") non-transitively.  A flex sorted by
         * it was sorted locally but not globally, so binary search took
         * the wrong branch and missed keys that iteration still found.
         *
         * Build a map with 4 elements per entry, insert those keys, and
         * require that multimapFullExists finds every key and that a
         * full multimapFullIteratorNext walk visits every key. */
        cleaner(multimapFullFree) multimapFull *m = multimapFullNew(4);
        char buf[600][16];
        size_t n = 0;
        for (int it = 0; it < 12; it++) {
            for (int k = 0; k < 50; k++) {
                snprintf(buf[n], sizeof(buf[n]), "g_%d_%d", it, k);
                /* databoxNewBytesAllowEmbed: keys of 8 bytes or less
                 * take the embedded form. */
                const databox key =
                    databoxNewBytesAllowEmbed(buf[n], strlen(buf[n]));
                const databox val =
                    databoxNewBytesAllowEmbed(buf[n], strlen(buf[n]));
                const databox expire = databoxNewUnsigned(0);
                const databox type = databoxNewUnsigned(0);
                const databox *elements[4] = {&key, &expire, &type, &val};
                multimapFullInsert(m, elements);
                n++;
            }
        }
        /* Every inserted key must be findable via Exists. */
        size_t missing_via_exists = 0;
        for (size_t i = 0; i < n; i++) {
            const databox probe =
                databoxNewBytesAllowEmbed(buf[i], strlen(buf[i]));
            if (!multimapFullExists(m, &probe)) {
                printf("multimapFullExists missed inserted key '%s'\n", buf[i]);
                missing_via_exists++;
            }
        }
        if (missing_via_exists > 0) {
            printf("multimapFullExists missed %zu of %zu inserted keys\n",
                   missing_via_exists, n);
            ERR("multimapFullExists missed keys that were just inserted "
                "(natural-sort comparator non-transitive — BUG-A regressed)");
        }
        /* Iteration must visit at least n distinct keys. */
        multimapIterator iter;
        multimapFullIteratorInit(m, &iter, true);
        databox dk = {}, dv0 = {}, dv1 = {}, dv2 = {};
        databox *ele[4] = {&dk, &dv0, &dv1, &dv2};
        size_t walked = 0;
        while (multimapFullIteratorNext(&iter, ele)) {
            walked++;
        }
        if (walked != n) {
            printf("iteration walked %zu but inserted %zu\n", walked, n);
            ERR("iteration entry count != insertion count");
        }
    }

    TEST("dump with more than 64 sub-maps (heap-overflow regression)") {
        /* multimapFullDump's heap-spill arm allocated m->count BYTES
         * for an array of m->count POINTERS whenever the map spans
         * more than the 64-slot local array — an 8x under-allocation
         * the pointer-fill loop then wrote past the end of.  Force
         * more than 64 sub-maps with a small maxSize and dump: under
         * sanitizers the pre-fix code faults inside the fill loop,
         * and the merged flex must carry every inserted entry. */
        cleaner(multimapFullFree) multimapFull *m = multimapFullNew(2);
        m->maxSize = 512;
        char kb[32];
        size_t inserted = 0;
        for (size_t i = 0; i < 200'000 && m->count <= 80; i++) {
            snprintf(kb, sizeof(kb), "g13_%zu", i);
            const databox key = databoxNewBytesAllowEmbed(kb, strlen(kb));
            const databox val = databoxNewBytesAllowEmbed(kb, strlen(kb));
            const databox *elements[2] = {&key, &val};
            multimapFullInsert(m, elements);
            inserted++;
        }
        if (m->count <= 64) {
            printf("only %u sub-maps built from %zu inserts\n",
                   (unsigned)m->count, inserted);
            ERR("failed to construct >64 sub-maps — the heap-spill "
                "dump arm was never exercised");
        }
        cleaner(flexFree) flex *combined = multimapFullDump(m);
        if (!combined) {
            ERR("multimapFullDump returned NULL for a populated map");
        }
        const size_t dumped = flexCount(combined) / m->elementsPerEntry;
        if (dumped != inserted) {
            printf("dump carried %zu entries, inserted %zu\n", dumped,
                   inserted);
            ERR("dump lost entries across the >64-submap merge");
        }
    }

    TEST("range box search: guessed probes route as halving does") {
        /* Maps of integer keys spread evenly, with holes, skewed, and
         * mixed with reals, negatives, 128-bit values and bytes; every
         * probe on, beside and beyond each key must route to the number
         * of range boxes ordered at or below it (the oracle scans them
         * all with databoxCompare) */
        typedef enum mfTestSpread {
            MF_TEST_SPREAD_DENSE = 0,
            MF_TEST_SPREAD_HOLES,
            MF_TEST_SPREAD_CUBIC,
            MF_TEST_SPREAD_MIXED,
            MF_TEST_SPREAD_COUNT
        } mfTestSpread;
        constexpr int64_t keys = 6000;
        static __uint128_t wide[keys];
        static char text[keys][16];
        databox probes[6];
        for (int spread = 0; spread < MF_TEST_SPREAD_COUNT; spread++) {
            cleaner(multimapFullFree) multimapFull *m = multimapFullNew(2);
            m->maxSize = 256;
            for (int64_t i = 0; i < keys; i++) {
                databox key;
                switch ((mfTestSpread)spread) {
                case MF_TEST_SPREAD_DENSE:
                    key = databoxNewUnsigned((uint64_t)i);
                    break;
                case MF_TEST_SPREAD_HOLES:
                    key = databoxNewUnsigned((uint64_t)(i * 3 + (i % 7)));
                    break;
                case MF_TEST_SPREAD_CUBIC:
                    key = databoxNewUnsigned((uint64_t)(i * i * i));
                    break;
                default:
                    switch (i % 5) {
                    case 0:
                        key = DATABOX_DOUBLE((double)i + 0.5);
                        break;
                    case 1:
                        key = databoxNewSigned(-i);
                        break;
                    case 2:
                        wide[i] = ((__uint128_t)1 << 70) + (uint64_t)i;
                        key = (databox){.type = DATABOX_UNSIGNED_128,
                                        .big = true,
                                        .data.u128 = &wide[i]};
                        break;
                    case 3:
                        snprintf(text[i], sizeof(text[i]), "k%05" PRId64, i);
                        key = databoxNewBytesString(text[i]);
                        break;
                    default:
                        key = databoxNewUnsigned((uint64_t)i * 11);
                        break;
                    }
                    break;
                }

                const databox val = databoxNewUnsigned((uint64_t)i);
                const databox *row[2] = {&key, &val};
                multimapFullInsert(m, row);
            }

            if (m->count < 40) {
                ERR("spread %d built only %" PRIu32 " maps", spread, m->count);
            }

            for (multimapFullIdx b = 0; b + 1 < m->count; b++) {
                const databox *rb = getRangeBox(m, b);
                const uint64_t v = rb->data.u64;
                size_t n = 0;
                probes[n++] = *rb;
                if (rb->type == DATABOX_UNSIGNED_64) {
                    probes[n++] = databoxNewUnsigned(v + 1);
                    probes[n++] = databoxNewUnsigned(v ? v - 1 : 0);
                    probes[n++] = databoxNewSigned(v ? (int64_t)(v - 1) : 0);
                    probes[n++] =
                        (databox){.data.u64 = v + 2,
                                  .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};
                }
                probes[n++] = databoxNewUnsigned(UINT64_MAX);
                for (size_t p = 0; p < n; p++) {
                    multimapFullIdx want = 0;
                    for (multimapFullIdx r = 0; r + 1 < m->count; r++) {
                        want +=
                            databoxCompare(getRangeBox(m, r), &probes[p]) <= 0;
                    }

                    const multimapFullIdx got =
                        multimapFullBinarySearch(m, &probes[p]);
                    if (got != want) {
                        ERR("spread %d box %" PRIu32 " probe %zu (type %d): "
                            "map %" PRIu32 ", want %" PRIu32,
                            spread, b, p, (int)probes[p].type, got, want);
                    }
                }
            }
        }
    }

    TEST("test gaps") {
        cleaner(multimapFullFree) multimapFull *m = multimapFullNew(2);
        m->maxSize = 1024;

        const size_t howMany = 1024;
        /* Insert (with gaps) */
        for (size_t gap = 20; gap > 0; gap--) {
            for (size_t i = 0; i < howMany; i += gap) {
                const databox keybox = databoxNewUnsigned(i);
                const databox valbox = DATABOX_BOX_FALSE;
                const databox *elements[2] = {&keybox, &valbox};
                multimapFullInsert(m, elements);
            }
        }

#if 0
        multimapFullRepr(m);
#endif
        strictConsistencyCheck(m);

        /* Gap 1 reached every key; the larger gaps replaced rows of keys
         * already present */
        if (multimapFullCount(m) != howMany) {
            ERR("%zu rows, want %zu", multimapFullCount(m), howMany);
        }

        for (size_t i = 0; i < howMany; i++) {
            const databox keybox = databoxNewUnsigned(i);
            if (!multimapFullExists(m, &keybox)) {
                ERR("key %zu is missing", i);
                break;
            }
        }
    }

    TEST("speeds at different sizes") {
        int32_t maxMax = 1 << 16;
        [[maybe_unused]] uint64_t secs[20] = {};
        [[maybe_unused]] size_t size[20] = {};
        int i = 0;
        for (int32_t maxSize = 1; maxSize < maxMax; maxSize *= 2) {
            cleaner(multimapFullFree) multimapFull *m = multimapFullNew(2);
            m->maxSize = maxSize;
            /* About 160 KB of pairs: nine maps even at the largest
             * maxSize (32 KB); the timings printed are informational */
            int32_t pairs = 1 << 13;
            TEST_DESC("%d bytes max with %d k/v pairs...", maxSize, pairs) {
                TIME_INIT;
                for (int32_t j = 0; j < pairs; j++) {
                    char *key = genkey("key", j);
                    char *val = genval("val", j * 100);
                    const databox keybox = databoxNewBytesString(key);
                    const databox valbox = databoxNewBytesString(val);
                    const databox *elements[2] = {&keybox, &valbox};
                    multimapFullInsert(m, elements);
                }

                TIME_FINISH(pairs, "insert");
                assert(multimapFullCount(m) == (uint32_t)pairs);
                secs[i] = lps.global.us.duration;
                size[i] = multimapFullReportSizeOnly(m);
                i++;
            }

            multimapFullConforms(m);
            multimapFullReport(m);
            multimapFullVerify(m);
        }
    }

    printf("\n\n");
#endif

#if 1
    TEST_DESC("%d k/v pairs - inserting...", 300) {
        cleaner(multimapFullFree) multimapFull *m = multimapFullNew(2);
        TIME_INIT;
        for (int32_t j = 0; j < 300; j++) {
            char *key = genkey("1key", j);
            char *val = genval("1val", j * 100);
            const databox keybox = databoxNewBytesString(key);
            const databox valbox = databoxNewBytesString(val);
            const databox *elements[2] = {&keybox, &valbox};
            const bool alreadyExisted = multimapFullInsert(m, elements);
            assert(!alreadyExisted);
        }

        TIME_FINISH(300, "insert");
        multimapFullConforms(m);
        multimapFullReport(m);
        multimapFullVerify(m);
    }

    printf("\n\n");

    for (int32_t i = 0; i < 4096; i = multimapFullTestNextSize_(i)) {
        cleaner(multimapFullFree) multimapFull *m = multimapFullNew(2);
        TEST_DESC("%d k/v pairs - inserting...", i) {
            TIME_INIT;
            for (int32_t j = 0; j < i; j++) {
                char *key = genkey("key", j);
                char *val = genval("val", j * 100);
                const databox keybox = databoxNewBytesString(key);
                const databox valbox = databoxNewBytesString(val);
                const databox *elements[2] = {&keybox, &valbox};
                multimapFullInsert(m, elements);
            }

            TIME_FINISH(i, "insert");
        }

        multimapFullConforms(m);
        multimapFullReport(m);
        multimapFullVerify(m);

        TEST_DESC("%d k/v pairs - checking members...", i) {
            TIME_INIT;
            for (int32_t j = 0; j < i; j++) {
                char *key = genkey("key", j);
                const databox keybox = databoxNewBytesString(key);
                bool found = multimapFullExists(m, &keybox);
                if (!found) {
                    ERR("Didn't find %s at iteration (%d, %d)!", key, i, j);
                    assert(false);
                    unreachable();
                }
            }

            TIME_FINISH(i, "exists");
            assert(multimapFullCount(m) == (uint32_t)i);
        }

        printf("\n");
        TEST_DESC("%d k/v pairs - deleting...", i) {
            bool delLowToHigh = i % 2 == 0;
            multimapFullIdx start = delLowToHigh ? 0 : i - 1;
            int32_t incrBy = delLowToHigh ? 1 : -1;
            TIME_INIT;
            for (int32_t j = start; delLowToHigh ? j < i : j >= 0;
                 j += incrBy) {
                char *key = genkey("key", j);
                const databox keybox = databoxNewBytesString(key);
                bool deleted = multimapFullDelete(m, &keybox);
                if (!deleted) {
                    ERR("Didn't find %s at iteration (%d, %d)!", key, i, j);
                    multimapFullReport(m);
                    multimapFullVerify(m);
                    assert(false);
                }
            }

            TIME_FINISH(i, "delete");
            assert(multimapFullCount(m) == 0);
        }

        /* We don't check i == 0 because 0 has no elements, so we can't delete
         * the flex by finding elements to delete, so... just skip it. */
        if (i > 0) {
            if (m->count > 1) {
                ERR("After full delete, more than one map still exists!  We "
                    "have %u maps!\n",
                    m->count);
            }

            if (flexCount(getMapLowest(m)) > 0) {
                ERR("After full delete, map[0] has %zu elements!\n",
                    flexCount(getMapLowest(m)));
            }
        }

        multimapFullConforms(m);
        multimapFullReport(m);
        multimapFullVerify(m);

        TEST_DESC("%d k/v pairs - inserting again after full delete...", i) {
            TIME_INIT;
            for (int32_t j = 0; j < i; j++) {
                char *key = genkey("key", j);
                char *val = genval("val", j * 100);
                const databox keybox = databoxNewBytesString(key);
                const databox valbox = databoxNewBytesString(val);
                const databox *elements[2] = {&keybox, &valbox};
                multimapFullInsert(m, elements);
            }

            TIME_FINISH(i, "insert");
            assert(multimapFullCount(m) == (uint32_t)i);
        }

        multimapFullConforms(m);
        multimapFullReport(m);
        multimapFullVerify(m);

        printf("\n");
    }

#endif

    /* Every size to 256, then every 1/16 more: the same sizes as the
     * forward sweep above */
    for (int32_t i = 0; i < 4096; i = multimapFullTestNextSize_(i)) {
        cleaner(multimapFullFree) multimapFull *m = multimapFullNew(2);
        TEST_DESC("%d k/v pairs - inserting in reverse order...", i) {
            TIME_INIT;
            for (int32_t j = i; j > 0; j--) {
                char *key = genkey("key", j);
                char *val = genval("val", j * 100);
                const databox keybox = databoxNewBytesString(key);
                const databox valbox = databoxNewBytesString(val);
                const databox *elements[2] = {&keybox, &valbox};
                multimapFullInsert(m, elements);
            }

            TIME_FINISH(i, "insert");
        }

        multimapFullConforms(m);
        multimapFullReport(m);
        multimapFullVerify(m);

        TEST_DESC("%d k/v pairs - checking members...", i) {
            TIME_INIT;
            for (int32_t j = i; j > 0; j--) {
                char *key = genkey("key", j);
                const databox keybox = databoxNewBytesString(key);
                bool found = multimapFullExists(m, &keybox);
                if (!found) {
                    ERR("Didn't find %s at iteration (%d, %d)!", key, i, j);
                    assert(false);
                    unreachable();
                }
            }

            TIME_FINISH(i, "exists");
            assert(multimapFullCount(m) == (uint32_t)i);
        }

        printf("\n");
    }

    /* Regression: REPLACE on an existing key when the holding map has
     * already grown past maxSize must update IN PLACE.  Before the
     * fix, that path routed through the split-or-new-map branch, which
     * inserted a second copy into a different map.  Downstream lookups
     * then used the binary-search-by-range box to pick a map and got
     * the WRONG one for keys equal to a range boundary, silently
     * returning stale data.
     *
     * This test forces the failure shape:
     *   1. maxSize is small (256B) so the very first big-value insert
     *      pushes the map over maxSize without making it eligible for
     *      a clean split (we only have one element).
     *   2. We REPLACE that key with a different big value.
     *   3. Count must stay at 1; lookup must return the NEW value;
     *      multimapFullVerify must accept the resulting structure. */
    TEST("regression: REPLACE on existing key with map exceeding maxSize") {
        cleaner(multimapFullFree) multimapFull *m = multimapFullNew(2);
        m->maxSize = 256;

        /* Build a value larger than maxSize so a single insert is
         * already enough to trip mapBytes > maxSize. */
        char bigVal1[512];
        char bigVal2[512];
        memset(bigVal1, 'A', sizeof(bigVal1) - 1);
        bigVal1[sizeof(bigVal1) - 1] = '\0';
        memset(bigVal2, 'B', sizeof(bigVal2) - 1);
        bigVal2[sizeof(bigVal2) - 1] = '\0';

        const databox keybox = databoxNewBytesString("the-key");
        const databox v1box = databoxNewBytesString(bigVal1);
        const databox v2box = databoxNewBytesString(bigVal2);

        const databox *insert1[2] = {&keybox, &v1box};
        const bool existed1 = multimapFullInsert(m, insert1);
        assert(!existed1);
        assert(multimapFullCount(m) == 1);

        /* Now the map is over maxSize and contains exactly one key.
         * A REPLACE of that key must stay in this same map. */
        const databox *insert2[2] = {&keybox, &v2box};
        const bool existed2 = multimapFullInsert(m, insert2);
        if (!existed2) {
            ERR("REPLACE on existing key reported as new insert "
                "(returned !existed) — bug regressed!");
        }

        if (multimapFullCount(m) != 1) {
            ERR("After REPLACE, count is %zu but should be 1 — duplicate "
                "key was inserted in a second map (the regressed bug)!",
                multimapFullCount(m));
        }

        /* Verify the new value is what comes back, not the old one. */
        databox foundVal = {};
        databox *out[1] = {&foundVal};
        const bool ok = multimapFullLookup(m, &keybox, out);
        assert(ok);
        assert(foundVal.type == DATABOX_BYTES);
        if (foundVal.len != v2box.len ||
            memcmp(foundVal.data.bytes.start, v2box.data.bytes.start,
                   v2box.len) != 0) {
            ERR("Lookup after REPLACE returned the OLD value (binary "
                "search picked the wrong map) — bug regressed!");
        }

        /* Structural invariants — verify no duplicate keys leaked
         * into a second map. */
        multimapFullVerify(m);
    }

    TEST("exists after insert: small bytes-embed keys in 4-element map") {
        /* Mirror a 4-element-per-entry usage pattern (key + 3 value
         * boxes — the common shape downstream consumers use for
         * (key, type, expire, value) tuples).  Insert ~30 short
         * BYTES_EMBED keys, each followed immediately by
         * multimapFullExists(key) → assert true.
         *
         * Each insert should be findable on its own bucket while the
         * bucket is unsplit.  At ~30 entries of 6-byte keys (~20 B
         * per tuple, ~600 B total) the structure stays a single
         * submap (default maxSize 2048) — no split bookkeeping
         * involved.  If exists returns false here, the find-in-flex
         * path itself is dropping inserted keys, independent of any
         * range-box / split logic. */
        cleaner(multimapFullFree) multimapFull *m = multimapFullNew(4);

        const size_t howMany = 60;
        char keybuf[16];
        char valbuf[16];
        for (size_t i = 0; i < howMany; i++) {
            const int kn =
                snprintf(keybuf, sizeof(keybuf), "k_%zu_%zu", i / 10, i % 10);
            const int vn = snprintf(valbuf, sizeof(valbuf), "v_%zu", i);
            assert(kn > 0 && (size_t)kn < sizeof(keybuf));
            assert(vn > 0 && (size_t)vn < sizeof(valbuf));

            const databox keybox =
                databoxNewBytesAllocateOrEmbed(keybuf, (size_t)kn);
            const databox typebox = databoxNewUnsigned(1);
            const databox expirebox = DATABOX_BOX_FALSE;
            const databox valbox =
                databoxNewBytesAllocateOrEmbed(valbuf, (size_t)vn);
            const databox *elements[4] = {&keybox, &typebox, &expirebox,
                                          &valbox};

            const bool existed = multimapFullInsert(m, elements);
            if (existed) {
                ERR("entry %zu: insert reported REPLACE of an "
                    "already-present key during a fresh insert "
                    "sequence",
                    i);
            }

            if (!multimapFullExists(m, &keybox)) {
                ERR("entry %zu: multimapFullExists returned false "
                    "for key '%.*s' (len=%d, type=%d) immediately "
                    "after multimapFullInsert returned new-insert."
                    "  multimapFullCount=%zu m->count=%u",
                    i, kn, keybuf, kn, (int)keybox.type, multimapFullCount(m),
                    (unsigned)m->count);
            }
        }
    }

    TEST("exists after insert: shuffled small bytes-embed keys") {
        /* Sequential-order inserts are the easy case: every insert
         * appends at the tail and the binary search trivially walks
         * right.  Real-world consumers route by hash, so the insert
         * order to any single multimap is effectively random.  This
         * test inserts the SAME 60 short BYTES_EMBED keys but in a
         * shuffled order so the binary search hits every code path
         * (insert at head, insert in middle, growing prefix shifts
         * the underlying flex bytes around). */
        cleaner(multimapFullFree) multimapFull *m = multimapFullNew(4);

        const size_t howMany = 60;
        char keybuf[16];
        char valbuf[16];

        /* A seeded shuffle of [0, howMany): reproducible across runs,
         * and a non-sorted insert order */
        size_t order[60];
        for (size_t i = 0; i < howMany; i++) {
            order[i] = i;
        }
        uint64_t rng = 0xdeadbeefcafebabeULL;
        DK_SHUFFLE(order, howMany, &rng);

        for (size_t k = 0; k < howMany; k++) {
            const size_t i = order[k];
            const int kn =
                snprintf(keybuf, sizeof(keybuf), "k_%zu_%zu", i / 10, i % 10);
            const int vn = snprintf(valbuf, sizeof(valbuf), "v_%zu", i);
            assert(kn > 0 && (size_t)kn < sizeof(keybuf));
            assert(vn > 0 && (size_t)vn < sizeof(valbuf));

            const databox keybox =
                databoxNewBytesAllocateOrEmbed(keybuf, (size_t)kn);
            const databox typebox = databoxNewUnsigned(1);
            const databox expirebox = DATABOX_BOX_FALSE;
            const databox valbox =
                databoxNewBytesAllocateOrEmbed(valbuf, (size_t)vn);
            const databox *elements[4] = {&keybox, &typebox, &expirebox,
                                          &valbox};

            (void)multimapFullInsert(m, elements);
            if (!multimapFullExists(m, &keybox)) {
                ERR("shuffled-insert step %zu (key #%zu = '%.*s'): "
                    "exists missed key immediately after insert "
                    "(m->count=%u submaps, total values=%zu)",
                    k, i, kn, keybuf, (unsigned)m->count, multimapFullCount(m));
            }
        }

        /* Final sweep: re-verify every key after all inserts done. */
        for (size_t i = 0; i < howMany; i++) {
            const int kn =
                snprintf(keybuf, sizeof(keybuf), "k_%zu_%zu", i / 10, i % 10);
            const databox keybox =
                databoxNewBytesAllocateOrEmbed(keybuf, (size_t)kn);
            if (!multimapFullExists(m, &keybox)) {
                ERR("shuffled-insert final sweep: key %zu ('%.*s') "
                    "missing after full insert sequence",
                    i, kn, keybuf);
            }
        }

        multimapFullVerify(m);
    }

    TEST("exists after insert: 200 small bytes-embed keys + split") {
        /* Same shape but enough entries to force submap splits.
         * Default maxSize=2048; 200 entries × ~20 B/entry ≈ 4 KB,
         * so we should see at least 2 submaps form.  Same invariant:
         * every key must be findable immediately after insert. */
        cleaner(multimapFullFree) multimapFull *m = multimapFullNew(4);

        const size_t howMany = 200;
        char keybuf[16];
        char valbuf[16];
        for (size_t i = 0; i < howMany; i++) {
            const int kn =
                snprintf(keybuf, sizeof(keybuf), "k_%zu_%zu", i / 50, i % 50);
            const int vn = snprintf(valbuf, sizeof(valbuf), "v_%zu", i);
            assert(kn > 0 && (size_t)kn < sizeof(keybuf));
            assert(vn > 0 && (size_t)vn < sizeof(valbuf));

            const databox keybox =
                databoxNewBytesAllocateOrEmbed(keybuf, (size_t)kn);
            const databox typebox = databoxNewUnsigned(1);
            const databox expirebox = DATABOX_BOX_FALSE;
            const databox valbox =
                databoxNewBytesAllocateOrEmbed(valbuf, (size_t)vn);
            const databox *elements[4] = {&keybox, &typebox, &expirebox,
                                          &valbox};

            (void)multimapFullInsert(m, elements);
            if (!multimapFullExists(m, &keybox)) {
                ERR("entry %zu of %zu: multimapFullExists missed "
                    "key '%.*s' (m->count=%u submaps)",
                    i, howMany, kn, keybuf, (unsigned)m->count);
            }
        }

        /* Final whole-pass sweep — every inserted key must still be
         * findable after the entire insert sequence completes. */
        for (size_t i = 0; i < howMany; i++) {
            const int kn =
                snprintf(keybuf, sizeof(keybuf), "k_%zu_%zu", i / 50, i % 50);
            assert(kn > 0 && (size_t)kn < sizeof(keybuf));
            const databox keybox =
                databoxNewBytesAllocateOrEmbed(keybuf, (size_t)kn);
            if (!multimapFullExists(m, &keybox)) {
                ERR("final sweep: key %zu ('%.*s') missing after "
                    "all %zu inserts completed",
                    i, kn, keybuf, howMany);
            }
        }

        multimapFullVerify(m);
    }

    TEST("exists-after-insert: 80 keys with DATABOX_BYTES (aliased) value") {
        /* Some upstream consumers populate the value-box slot via a
         * decode macro that points `data.bytes.start` at an external
         * buffer (DATABOX_BYTES, not BYTES_EMBED).  The bytes are
         * copied INTO the flex during insert, so the alias is fine
         * for storage, but the test below exercises that input shape
         * to make sure no path in the multimap depends on the value
         * box being BYTES_EMBED specifically. */
        cleaner(multimapFullFree) multimapFull *m = multimapFullNew(4);

        const size_t howMany = 80;
        char keybuf[16];
        /* One backing buffer that every value-box aliases into.  This
         * is the closest match to how a streaming restore callback
         * hands values to consumers — a pointer into the row buffer,
         * not a freshly-allocated heap copy. */
        char valuepool[16];

        size_t order[80];
        for (size_t i = 0; i < howMany; i++) {
            order[i] = i;
        }
        uint64_t rng = 0xA5A5A5A5DEADBEEFULL;
        DK_SHUFFLE(order, howMany, &rng);

        for (size_t step = 0; step < howMany; step++) {
            const size_t i = order[step];
            const int kn =
                snprintf(keybuf, sizeof(keybuf), "k_%zu_%zu", i / 16, i % 16);
            const int vn = snprintf(valuepool, sizeof(valuepool), "v_%zu", i);

            const databox keybox =
                databoxNewBytesAllocateOrEmbed(keybuf, (size_t)kn);
            const databox typebox = databoxNewUnsigned(1);
            const databox expirebox = DATABOX_BOX_FALSE;
            /* DATABOX_BYTES with start pointing at our shared
             * backing buffer — re-aliased on every insert. */
            const databox valbox = {
                .type = DATABOX_BYTES,
                .len = (size_t)vn,
                .data.bytes.custart = (const uint8_t *)valuepool,
            };
            const databox *elements[4] = {&keybox, &typebox, &expirebox,
                                          &valbox};

            (void)multimapFullInsert(m, elements);
            if (!multimapFullExists(m, &keybox)) {
                ERR("aliased-value step %zu key #%zu '%.*s' missed", step, i,
                    kn, keybuf);
            }
        }

        for (size_t i = 0; i < howMany; i++) {
            const int kn =
                snprintf(keybuf, sizeof(keybuf), "k_%zu_%zu", i / 16, i % 16);
            const databox keybox =
                databoxNewBytesAllocateOrEmbed(keybuf, (size_t)kn);
            if (!multimapFullExists(m, &keybox)) {
                ERR("aliased-value final: key %zu '%.*s' missing", i, kn,
                    keybuf);
            }
        }

        multimapFullVerify(m);
    }

    TEST("exists-after-insert stress: 500 rounds × ~80 random keys") {
        /* Stress sweep: 500 rounds.  Each round creates a fresh
         * multimapFull(4), inserts ~80 short BYTES_EMBED keys in
         * fresh-shuffled order using a different LCG seed, and
         * verifies multimapFullExists after every insert AND in a
         * final whole-pass sweep.  If a specific insert order can
         * cause exists to miss a key inserted in the same scope,
         * one of the 500 rounds will surface it. */
        const size_t rounds = 500;
        const size_t keysPerRound = 80;
        size_t round_failures = 0;

        char keybuf[16];
        char valbuf[16];

        for (size_t r = 0; r < rounds; r++) {
            cleaner(multimapFullFree) multimapFull *m = multimapFullNew(4);

            /* Deterministic shuffle seeded by round number. */
            size_t order[80];
            for (size_t i = 0; i < keysPerRound; i++) {
                order[i] = i;
            }
            uint64_t rng = 0x9E3779B97F4A7C15ULL + r * 0xBF58476D1CE4E5B9ULL;
            DK_SHUFFLE(order, keysPerRound, &rng);

            bool round_ok = true;
            for (size_t step = 0; step < keysPerRound; step++) {
                const size_t i = order[step];
                const int kn = snprintf(keybuf, sizeof(keybuf), "k_%zu_%zu",
                                        i / 16, i % 16);
                const int vn = snprintf(valbuf, sizeof(valbuf), "v_%zu", i);

                const databox keybox =
                    databoxNewBytesAllocateOrEmbed(keybuf, (size_t)kn);
                const databox typebox = databoxNewUnsigned(1);
                const databox expirebox = DATABOX_BOX_FALSE;
                const databox valbox =
                    databoxNewBytesAllocateOrEmbed(valbuf, (size_t)vn);
                const databox *elements[4] = {&keybox, &typebox, &expirebox,
                                              &valbox};
                (void)multimapFullInsert(m, elements);
                if (!multimapFullExists(m, &keybox)) {
                    printf("\nround %zu step %zu key #%zu '%.*s' "
                           "missed (m->count=%u, values=%zu)\n",
                           r, step, i, kn, keybuf, (unsigned)m->count,
                           multimapFullCount(m));
                    round_ok = false;
                    break;
                }
            }

            if (round_ok) {
                /* Final whole-pass sweep. */
                for (size_t i = 0; i < keysPerRound; i++) {
                    const int kn = snprintf(keybuf, sizeof(keybuf), "k_%zu_%zu",
                                            i / 16, i % 16);
                    const databox keybox =
                        databoxNewBytesAllocateOrEmbed(keybuf, (size_t)kn);
                    if (!multimapFullExists(m, &keybox)) {
                        printf("\nround %zu final sweep: key #%zu "
                               "'%.*s' missing\n",
                               r, i, kn, keybuf);
                        round_ok = false;
                        break;
                    }
                }
            }

            if (!round_ok) {
                round_failures++;
            }
        }

        if (round_failures > 0) {
            ERR("%zu/%zu stress rounds failed exists-after-insert",
                round_failures, rounds);
        }
    }

    TEST("reset then reuse: inserts and lookups work after Reset") {
        /* Reset must collapse back to a single empty map — no leftover
         * empty maps, stale middle offsets, or range boxes referencing
         * discarded flex bytes. */
        cleaner(multimapFullFree) multimapFull *m = multimapFullNew(2);
        const uint64_t n = 5000;

        for (int cycle = 0; cycle < 2; cycle++) {
            /* Enough entries to split into many internal maps */
            for (uint64_t i = 0; i < n; i++) {
                const databox key = databoxNewUnsigned(i);
                const databox val = databoxNewUnsigned(i * 3);
                const databox *elements[2] = {&key, &val};
                multimapFullInsert(m, elements);
            }

            if (multimapFullCount(m) != n) {
                ERR("cycle %d: expected %" PRIu64 " entries, got %zu", cycle, n,
                    multimapFullCount(m));
            }

            multimapFullReset(m);

            if (multimapFullCount(m) != 0) {
                ERR("cycle %d: expected empty map after reset, got %zu", cycle,
                    multimapFullCount(m));
            }
        }

        /* Final round: verify lookups after reset-reuse are coherent */
        for (uint64_t i = 0; i < n; i++) {
            const databox key = databoxNewUnsigned(i);
            const databox val = databoxNewUnsigned(i + 7);
            const databox *elements[2] = {&key, &val};
            multimapFullInsert(m, elements);
        }

        size_t missing = 0;
        for (uint64_t i = 0; i < n; i++) {
            const databox probe = databoxNewUnsigned(i);
            if (!multimapFullExists(m, &probe)) {
                missing++;
            }
        }

        if (missing > 0) {
            ERR("%zu of %" PRIu64 " keys missing after reset-reuse", missing,
                n);
        }
    }

    TEST("wide entries: arity above the insert stack-array headroom") {
        /* The flex insert path keeps fixed stack scratch for entries up
         * to 8 elements and must spill wider entries to the heap;
         * arity-12 rows exercise the spill path. */
        const uint32_t arity = 12;
        cleaner(multimapFullFree) multimapFull *m = multimapFullNew(arity);

        for (uint64_t i = 0; i < 200; i++) {
            databox boxes[12];
            const databox *elements[12];
            for (uint32_t j = 0; j < arity; j++) {
                boxes[j] = databoxNewUnsigned(i * 100 + j);
                elements[j] = &boxes[j];
            }
            multimapFullInsert(m, elements);
        }

        if (multimapFullCount(m) != 200) {
            ERR("Expected 200 wide entries, got %zu", multimapFullCount(m));
        }

        size_t missing = 0;
        for (uint64_t i = 0; i < 200; i++) {
            const databox probe = databoxNewUnsigned(i * 100);
            if (!multimapFullExists(m, &probe)) {
                missing++;
            }
        }

        if (missing > 0) {
            ERR("%zu of 200 wide-entry keys missing", missing);
        }
    }

    TEST("Append keeps the maps ordered whatever order rows arrive in") {
        /* Ascending rows take the fast path; a row below the last key
         * must land where an insert puts it, not in the highest map */
        cleaner(multimapFullFree) multimapFull *m = multimapFullNew(2);
        char pad[64];
        memset(pad, 'p', sizeof(pad));
        const databox val = databoxNewBytes(pad, sizeof(pad));
        for (int64_t k = 0; k < 3000; k += 2) {
            const databox key = DATABOX_SIGNED(k);
            const databox *row[2] = {&key, &val};
            multimapFullAppend(m, row);
        }
        for (int64_t k = 2999; k > 0; k -= 2) { /* the odd keys, descending */
            const databox key = DATABOX_SIGNED(k);
            const databox *row[2] = {&key, &val};
            multimapFullAppend(m, row);
        }
        if (multimapFullNodeCount(m) < 2 || multimapFullCount(m) != 3000) {
            ERR("%zu maps, %zu rows", multimapFullNodeCount(m),
                multimapFullCount(m));
        }
        for (int64_t k = 0; k < 3000; k++) {
            const databox key = DATABOX_SIGNED(k);
            if (!multimapFullExists(m, &key)) {
                ERR("key %" PRId64 " not found after appends", k);
                break;
            }
        }
    }

    TEST("DeleteNMaps drops whole maps and their rows from the count") {
        cleaner(multimapFullFree) multimapFull *m = multimapFullNew(2);
        char pad[64];
        memset(pad, 'q', sizeof(pad));
        const databox val = databoxNewBytes(pad, sizeof(pad));
        for (int64_t k = 0; k < 3000; k++) {
            const databox key = DATABOX_SIGNED(k);
            const databox *row[2] = {&key, &val};
            multimapFullInsert(m, row);
        }
        const size_t maps = multimapFullNodeCount(m);
        const size_t lowest = flexCount(getMap(m, 0)) / 2;
        if (maps < 3 || multimapFullBytesFull(m) <= multimapFullBytes(m)) {
            ERR("%zu maps; BytesFull %zu, Bytes %zu", maps,
                multimapFullBytesFull(m), multimapFullBytes(m));
        }
        if (!multimapFullDeleteNMaps(m, 1) ||
            multimapFullCount(m) != 3000 - lowest ||
            multimapFullNodeCount(m) != maps - 1) {
            ERR("after one map: %zu rows, %zu maps", multimapFullCount(m),
                multimapFullNodeCount(m));
        }
        const databox gone = DATABOX_SIGNED(0);
        const databox kept = DATABOX_SIGNED(2999);
        if (multimapFullExists(m, &gone) || !multimapFullExists(m, &kept)) {
            ERR("the deleted map's rows or the rest are wrong");
        }

        /* Past the last map: empty, one map kept, usable again */
        if (multimapFullDeleteNMaps(m, maps + 5) || multimapFullCount(m) != 0 ||
            multimapFullNodeCount(m) != 1) {
            ERR("after all maps: %zu rows, %zu maps", multimapFullCount(m),
                multimapFullNodeCount(m));
        }
        const databox *row[2] = {&kept, &val};
        multimapFullInsert(m, row);
        if (multimapFullCount(m) != 1 || !multimapFullExists(m, &kept)) {
            ERR("an emptied map did not take a new row");
        }
    }

    TEST("a Full map grows from one sorted flex and deletes rows it found") {
        /* @own: container; free=multimapFullFree */
        flex *one = flexNew();
        flexEntry *middle = one + FLEX_EMPTY_SIZE;
        for (int64_t k = 0; k < 40; k++) {
            const databox key = DATABOX_SIGNED(k * 3);
            const databox val = DATABOX_SIGNED(k);
            const databox *row[2] = {&key, &val};
            flexInsertReplaceByTypeSortedWithMiddleMultiDirect(&one, 2, row,
                                                               &middle, true);
        }
        cleaner(multimapFullFree) multimapFull *m =
            multimapFullNewFromOneGrow(nullptr, one, 0, 2, false);
        const databox k30 = DATABOX_SIGNED(30);
        const databox v10 = DATABOX_SIGNED(10);
        const databox v11 = DATABOX_SIGNED(11);
        const databox *wrong[2] = {&k30, &v11};
        const databox *right[2] = {&k30, &v10};
        databox found = {};
        if (multimapFullCount(m) != 40 || !multimapFullExists(m, &k30) ||
            multimapFullDeleteFullWidthWithFound(m, wrong, &found) ||
            !multimapFullDeleteFullWidthWithFound(m, right, &found) ||
            found.data.i != 30 || multimapFullExists(m, &k30) ||
            multimapFullCount(m) != 39) {
            ERR("NewFromOneGrow / DeleteFullWidthWithFound misbehaved");
        }

        /* An oversize map splits the same with or without a (NULL)
         * reference container */
        flex **map = getMapPtr(m, 0);
        m->maxSize = 64;
        const databox k33 = DATABOX_SIGNED(33);
        const databox k117 = DATABOX_SIGNED(117);
        const databox k31 = DATABOX_SIGNED(31);
        if (!multimapFullRegularizeMapWithReference(m, 0, map, nullptr) ||
            multimapFullNodeCount(m) < 2 || multimapFullCount(m) != 39 ||
            !multimapFullExists(m, &k33) || !multimapFullExists(m, &k117) ||
            multimapFullExists(m, &k31)) {
            ERR("RegularizeMapWithReference did not split the map");
        }
    }

    TEST("wide entries: 128-bit element through heap-carved scratch") {
        /* Arity above the stack threshold forces the sorted-insert
         * heap scratch carve; a SIGNED_128 element routes its payload
         * through the carved bigBuffer. A previous carve overlapped
         * bigBuffer with content[], so tail descriptor builds stomped
         * the 128-bit payload before it was written into the flex.
         * The overlap is intra-allocation (ASan blind) - only value
         * verification catches it. */
        const uint32_t arity = 12;
        cleaner(multimapFullFree) multimapFull *m = multimapFullNew(arity);

        __int128_t bigVal = (((__int128_t)0x0123456789ABCDEFLL) << 64) |
                            (__int128_t)0x1122334455667788ULL;

        for (uint64_t i = 0; i < 50; i++) {
            databox boxes[12];
            const databox *elements[12];
            boxes[0] = databoxNewUnsigned(i);
            boxes[1] = (databox){
                .type = DATABOX_SIGNED_128, .big = true, .data.i128 = &bigVal};
            for (uint32_t j = 2; j < arity; j++) {
                boxes[j] = databoxNewUnsigned(0xAA00 + j);
            }
            for (uint32_t j = 0; j < arity; j++) {
                elements[j] = &boxes[j];
            }
            multimapFullInsert(m, elements);
        }

        /* Read the first entry back full width; the 128-bit slot needs
         * big-box storage */
        databox out[12];
        databoxBig out128;
        DATABOX_BIG_INIT(&out128);
        databox *ptrs[12];
        for (uint32_t j = 0; j < arity; j++) {
            ptrs[j] = &out[j];
        }
        ptrs[1] = (databox *)&out128;

        if (!multimapFullFirst(m, ptrs)) {
            ERR("multimapFullFirst failed on wide 128-bit map");
        }
        if (out[0].data.u64 != 0) {
            ERR("Wrong first key: %" PRIu64, out[0].data.u64);
        }
        if (*out128.data.i128 != bigVal) {
            ERR("128-bit payload corrupted (scratch carve overlap)");
        }
        for (uint32_t j = 2; j < arity; j++) {
            if (out[j].data.u64 != 0xAA00 + j) {
                ERR("Sentinel %u corrupted: %" PRIu64, j, out[j].data.u64);
            }
        }
    }

    TEST("rangeBoxes own 96/128-bit head keys across 4096+ sub-maps") {
        /* About two rows per sub-map (maxSize 64), so the rangeBox
         * multiarray grows past its native limit (4096 16-byte boxes) and
         * regroups, and inserts, splits, merges and trims keep moving
         * rangeBoxes.  A move copies the box but not its heap payload, so
         * every wide boundary key must still read back by value. */
        constexpr int fwRows = 9000;
        cleaner(multimapFullFree) multimapFull *m = multimapFullNew(2);
        m->maxSize = 64;

        __uint128_t keys[fwRows];
        bool neg[fwRows];
        bool present[fwRows];
        bool replaced[fwRows];
        uint64_t s = 0x2545F4914F6CDD1DULL;
        for (size_t i = 0; i < fwRows; i++) {
            /* A random 96- or 128-bit magnitude above 2^80 whose low 16
             * bits are the row index, so every key is distinct. */
            dkXorshift64Next(&s);
            const unsigned width = (s & 2) ? 95 : 127;
            const __uint128_t mask = ((__uint128_t)1 << width) - 1;
            __uint128_t mag = ((__uint128_t)s << 64) & mask;
            mag = (mag & ~(__uint128_t)0xFFFF) | ((__uint128_t)1 << 80) |
                  (__uint128_t)i;
            neg[i] = s & 1;
            keys[i] = neg[i] ? (__uint128_t)0 - mag : mag;
            replaced[i] = false;
        }

#define FW_KEY(i)                                                              \
    ((databox){.type = neg[i] ? DATABOX_SIGNED_128 : DATABOX_UNSIGNED_128,     \
               .big = true,                                                    \
               .data.u128 = &keys[i]})

        for (size_t i = 0; i < fwRows; i++) {
            const databox key = FW_KEY(i);
            const databox *row[2] = {&key, &key};
            multimapFullInsert(m, row);
            present[i] = true;
        }

        if (_multiarrayType(m->rangeBox) == MULTIARRAY_TYPE_NATIVE) {
            ERR("only %" PRIu32 " sub-maps: the rangeBoxes never left the "
                "native array",
                m->count);
        }

        for (int round = 0; round < 4; round++) {
            multimapFullVerify(m);
            strictConsistencyCheck(m);
            size_t missing = 0;
            for (size_t i = 0; i < fwRows; i++) {
                const databox key = FW_KEY(i);
                databoxBig out;
                memset(&out, 0xFF, sizeof(out));
                DATABOX_BIG_INIT(&out);
                databox *values[1] = {(databox *)&out};
                const bool found = multimapFullLookup(m, &key, values);
                bool right = found == present[i];
                if (found && replaced[i]) {
                    right = out.type == DATABOX_UNSIGNED_64 && out.data.u == i;
                } else if (found) {
                    right = out.type == key.type && *out.data.u128 == keys[i];
                }

                missing += !right;
            }

            if (missing) {
                ERR("round %d: %zu rows wrong", round, missing);
            }

            if (round == 0) {
                /* Delete every third row: sub-maps empty out and merge. */
                for (size_t i = 0; i < fwRows; i += 3) {
                    const databox key = FW_KEY(i);
                    multimapFullDelete(m, &key);
                    present[i] = false;
                }
            } else if (round == 1) {
                /* Shrink the value of every fifth row in place, which
                 * reallocates its sub-map and refreshes its rangeBox. */
                for (size_t i = 1; i < fwRows; i += 5) {
                    const databox key = FW_KEY(i);
                    multimapEntry me;
                    if (present[i] &&
                        multimapFullGetUnderlyingEntry(m, &key, &me)) {
                        const databox small = databoxNewUnsigned(i);
                        me.fe = flexNext(*me.map, me.fe);
                        multimapFullReplaceEntry(m, &me, &small);
                        replaced[i] = true;
                    }
                }
            } else if (round == 2) {
                /* Trim every negative key and the smallest positive one:
                 * whole leading sub-maps go. */
                size_t lowest = fwRows;
                for (size_t i = 0; i < fwRows; i++) {
                    if (present[i] && !neg[i] &&
                        (lowest == fwRows || keys[i] < keys[lowest])) {
                        lowest = i;
                    }
                }

                const multimapPredicate p = {
                    .condition = MULTIMAP_CONDITION_LESS_THAN_EQUAL,
                    .compareAgainst = FW_KEY(lowest)};
                multimapFullDeleteByPredicate(m, &p);
                for (size_t i = 0; i < fwRows; i++) {
                    if (neg[i] || i == lowest) {
                        present[i] = false;
                    }
                }
            }
        }

#undef FW_KEY
    }

    TEST("a surrogate-keyed rangeBox owns a 96/128-bit atom value") {
        /* The rangeBox of a surrogate-keyed map holds the resolved head
         * key.  The atom table holds narrow values and one wide value,
         * inserted last so building the table never resolves it. */
        /* @own: caller; free=multimapAtomFree */
        multimapAtom *ma = multimapAtomNew();
        for (int64_t v = 10; v <= 30; v += 10) {
            const databox atom = databoxNewSigned(v);
            multimapAtomInsert(ma, &atom);
        }

        __uint128_t wideAtom = (__uint128_t)1 << 70;
        const databox wide = {
            .type = DATABOX_UNSIGNED_128, .big = true, .data.u128 = &wideAtom};
        multimapAtomInsert(ma, &wide);

        /* map[0] holds ids 0-2 (10, 20, 30); map[1] holds id 3.  The new
         * multimapFull takes both flexes. */
        /* @own: container; free=multimapFullFree */
        flex *maps[2] = {flexNew(), flexNew()};
        for (uint64_t id = 0; id <= 3; id++) {
            const databox ref = {.type = DATABOX_CONTAINER_REFERENCE_EXTERNAL,
                                 .data.u = id};
            flexPushByType(&maps[id == 3], &ref, FLEX_ENDPOINT_TAIL);
        }

        multimapFullMiddle middles[2] = {0, 0};
        cleaner(multimapFullFree) multimapFull *m =
            multimapFullNewFromTwoGrowWithReference(nullptr, maps, middles, 1,
                                                    false, ma);

        const databox *rb = getRangeBox(m, 0);
        if (rb->type != DATABOX_UNSIGNED_128 || !rb->big || !rb->allocated ||
            !rb->data.u128 || *rb->data.u128 != wideAtom) {
            ERR("rangeBox does not own the wide atom value");
        }

        __uint128_t above = (__uint128_t)1 << 80;
        __uint128_t below = ((__uint128_t)1 << 70) - 1;
        const struct {
            databox probe;
            multimapFullIdx mapIdx;
        } cases[] = {
            {databoxNewSigned(20), 0},
            {{.type = DATABOX_UNSIGNED_128, .big = true, .data.u128 = &below},
             0},
            {wide, 1},
            {{.type = DATABOX_UNSIGNED_128, .big = true, .data.u128 = &above},
             1},
        };

        for (size_t i = 0; i < countof(cases); i++) {
            const multimapFullIdx got =
                multimapFullBinarySearch(m, &cases[i].probe);
            if (got != cases[i].mapIdx) {
                ERR("probe %zu routed to map %" PRIu32 ", expected %" PRIu32, i,
                    got, cases[i].mapIdx);
            }
        }

        multimapAtomFree(ma);
    }

    TEST("a copy of a surrogate-keyed map routes by resolved keys") {
        /* Rows are atom ids sorted by their atom values, as in
         * multimapAtom's reverse map.  The copy is checked against an
         * oracle of the ids it holds right after the copy and again after
         * inserts and deletes on it; the source must stay unchanged. */
        constexpr int copyAtoms = 2400;
        /* @own: caller; free=multimapAtomFree */
        multimapAtom *ma = multimapAtomNew();
        static __uint128_t wideVals[copyAtoms];
        static char strVals[copyAtoms][24];
        static databox vals[copyAtoms];
        for (size_t i = 0; i < copyAtoms; i++) {
            if (i % 4 == 0) {
                wideVals[i] = ((__uint128_t)1 << 70) + i * 3;
                vals[i] = (databox){.type = DATABOX_UNSIGNED_128,
                                    .big = true,
                                    .data.u128 = &wideVals[i]};
            } else if (i % 4 == 1) {
                vals[i] = databoxNewSigned((int64_t)i * 7 - 5000);
            } else {
                snprintf(strVals[i], sizeof(strVals[i]), "copy key %06zu", i);
                vals[i] = databoxNewBytesString(strVals[i]);
            }

            /* A fresh table hands out ids 0, 1, 2, ... */
            multimapAtomInsert(ma, &vals[i]);
        }

        bool inSource[copyAtoms];
        cleaner(multimapFullFree) multimapFull *m = multimapFullNew(1);
        for (size_t k = 0; k < copyAtoms; k++) {
            /* 7919 is prime, so this visits every id once, out of order. */
            const size_t i = (k * 7919) % copyAtoms;
            inSource[i] = i % 5 != 4;
            if (inSource[i]) {
                const databox ref = {
                    .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL, .data.u = i};
                const databox *elements[1] = {&vals[i]};
                multimapFullInsertWithSurrogateKey(m, elements, &ref, ma);
            }
        }

        if (m->count < 3) {
            ERR("source has %" PRIu32 " sub-maps, expected several", m->count);
        }

        cleaner(multimapFullFree) multimapFull *c = multimapFullCopy(m);
        err += surrogateCopyCheck_("source", m, ma, vals, inSource, copyAtoms);
        err +=
            surrogateCopyCheck_("fresh copy", c, ma, vals, inSource, copyAtoms);

        bool inCopy[copyAtoms];
        memcpy(inCopy, inSource, sizeof(inCopy));
        for (size_t i = 0; i < copyAtoms; i++) {
            if (!inCopy[i]) {
                const databox ref = {
                    .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL, .data.u = i};
                const databox *elements[1] = {&vals[i]};
                multimapFullInsertWithSurrogateKey(c, elements, &ref, ma);
                inCopy[i] = true;
            }

            if (i % 3 == 0) {
                databox found = {};
                if (!multimapFullDeleteWithReference(c, &vals[i], ma, &found) ||
                    found.type != DATABOX_CONTAINER_REFERENCE_EXTERNAL ||
                    found.data.u != i) {
                    ERR("copy: delete of id %zu failed", i);
                }

                inCopy[i] = false;
            }
        }

        err +=
            surrogateCopyCheck_("mutated copy", c, ma, vals, inCopy, copyAtoms);
        err += surrogateCopyCheck_("source after copy mutations", m, ma, vals,
                                   inSource, copyAtoms);

        multimapAtomFree(ma);
    }

    /* RandomValue's selection checks (uniformity, fromTail, the random()
     * stream) run over every tier from multimapTest. */

    TEST("Repr: an empty map and one split into inner maps") {
        multimapFull *m = multimapFullNew(2);
        multimapFullRepr(m);
        for (int64_t i = 0; i < 300; i++) {
            const databox k = databoxNewSigned(i);
            const databox v = databoxNewSigned(i * 3);
            const databox *row[2] = {&k, &v};
            multimapFullInsert(m, row);
        }

        if (multimapFullNodeCount(m) < 2) {
            ERR("300 rows stayed in %zu inner map", multimapFullNodeCount(m));
        }
        multimapFullRepr(m);
        multimapFullFree(m);
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
