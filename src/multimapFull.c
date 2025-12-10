#include "multimapFull.h"
#include "multimapFullInternal.h"

/* ====================================================================
 * Management Macros
 * ==================================================================== */
/* Data structure size allocation maximums. */
/* Larger numbers == faster lookups but slower inserts.
 * You want a balance between size of reallocation required per insert
 * and size of contiguous blocks of data for lookup binary search. */
/* You can empirically discover correct numbers based on your workload
 * and platform by searching an exponential cache space then running
 * your workload for performance comparisions.
 * By default, we tune for 2016-era general purpose architectures
 * with per-CPU 32k L1 data caches, 256k L2 cache, and multi-MB
 * shared L3 caches (4 MB to 45 MB) */

#define NOISY_MAPPING 0

#if NOISY_MAPPING
#include <stdio.h>
#endif

#define CACHE_FILL 65536
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
            /* This extra retrieve/free should be annotated throughout         \
             * our code where referenceContainer is performing                 \
             * reallocDecrCount operations, but we seem to be missing some     \
             * annotations because we end up with memory leaks in all cases    \
             * unless we centralize the range box free operation to here. */   \
            /* If removing this (idx) > 0 branch, remember to update whichIdx  \
             * to be a ternary again: whichIdx = idx ? idx - 1 : 0 */          \
            size_t whichIdx = 0;                                               \
            if ((idx) > 0) {                                                   \
                whichIdx = (idx) - 1;                                          \
                databox *rangeBox_ = getRangeBox(m, whichIdx);                 \
                databoxFreeData(rangeBox_);                                    \
            }                                                                  \
                                                                               \
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

static inline databox *getRangeBox(const multimapFull *m, multimapFullIdx idx) {
    databox *box;
    multiarrayNativeGet(m->rangeBox, databox, box, idx);
    return box;
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

#define updateRangeBoxForIdx(m, idx, map_)                                     \
    do {                                                                       \
        /* TODO: optimize rangeBox to only update if head changed? */          \
        if ((idx) > 0 && flexCount(map_) > 0) {                                \
            /* Minus one becase rangeBox[0] is for map[1], etc */              \
            flexGetByType(flexHead(map_), getRangeBox(m, (idx) - 1));          \
        }                                                                      \
    } while (0)

/* Reference Range Boxes are different because they reference bytes NOT INSIDE
 * OUR OWN MAP, so we CAN'T just reference existing bytes, we have to copy bytes
 * for the range box to hold because the underlying data can go away at any */
#define updateRangeBoxForIdxWithReference(m, idx, map_, ref)                   \
    do {                                                                       \
        /* TODO: optimize rangeBox to only update if head changed? */          \
        /* We also only update if map has elements BECAUSE if this map reaches \
         * zero elements, we automatically delete the map after this update,   \
         * so don't bother updating a rangebox we are just going to elete      \
         * after this update. */                                               \
        if ((idx) > 0 && flexCount(map_) > 0) {                                \
            /* Minus one becase rangeBox[0] is for map[1], etc */              \
            databox *rangeBox = getRangeBox(m, (idx) - 1);                     \
            databoxFreeData(rangeBox);                                         \
                                                                               \
            /* This looks weird, but we want to COPY the value inside rangeBox \
             * if it is referencing data inside 'map_' */                      \
            flexGetByTypeWithReference(flexHead(map_), rangeBox, ref);         \
            (void)databoxAllocateIfNeeded(rangeBox);                           \
        }                                                                      \
    } while (0)

#define nextMapIdxExists(m, mapIdx) ((mapIdx) < ((m)->count - 1))

/* ====================================================================
 * Creation
 * ==================================================================== */
static const multimapFull multimapFullNull = {0};
static const databox rangeBoxNull = {{0}};

multimapFull *multimapFullNew(multimapElements elementsPerEntry) {
    multimapFull *m = zcalloc(1, sizeof(*m));

    m->elementsPerEntry = elementsPerEntry; /* default: key->value map */
    m->maxSize = 2048;

    /* Create initial map as empty so we're ready to go
     * for new inserts. */
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
                                 uint16_t maxSize) {
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
    flex *higher = flexNew();

    accountForNewMapAfterExistingMap_(m, mapIdxLower, useReference,
                                      referenceContainer, lower, higher);
}

#if 0
/* Iterate over all maps and conform them under maxSize */
DK_STATIC DK_FN_UNUSED void conformMapsSplit(multimapFull *m) {
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

/* Iterate over all maps and conform them (pairwise) to <= maxSize */
#if 0
DK_STATIC DK_FN_UNUSED void conformMapsMerge(multimapFull *m) {
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
multimapFull *multimapFullNewFromManyGrow_(
    void *m_, flex *map[], const multimapFullMiddle middle[],
    const size_t count, const multimapElements elementsPerEntry,
    const bool mapIsSet, const bool useReference,
    const multimapAtom *referenceContainer) {
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

            /* TODO: need reference container for creating range boxes */
            /* Range boxes **always** have full values, no references. */
            if (useReference) {
                updateRangeBoxForIdxWithReference(m, i, map[i],
                                                  referenceContainer);
            } else {
                updateRangeBoxForIdx(m, i, map[i]);
            }
        } else {
            assert(NULL && "Grew to full map with no contents?");
            __builtin_unreachable();
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
     * iteration over our total maps, but our initial count is ususally 2. */

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
        m_, map, middle, count, elementsPerEntry, mapIsSet, false, NULL);
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

    /* This is a rough estimate taken from multimap.c testing, but
     * still doesn't account for 100% (like when multiarrays
     * have grown to individual types, etc, but we don't have
     * multiarrayBytes() implemented yet! */
    Bytes += sizeof(*m);
    Bytes += sizeof(databox) * (m->count - 1);
    Bytes += sizeof(multimapFullMiddle) * m->count;
    Bytes += sizeof(flex *) * m->count;

    return Bytes;
}

flex *multimapFullDump(const multimapFull *m) {
    const flex *localMaps[64] = {0};
    const flex **useMaps;

    /* TODO:
     *   could do abstraction breakage here and check if
     *   multiarray for maps is native (max 8192 instead of 64),
     *   then just pass it directly to flexBulkMergeFlex()
     *   (else, allocate local) */
    if (m->count <= COUNT_ARRAY(localMaps)) {
        useMaps = localMaps;
    } else {
        useMaps = zcalloc(1, m->count);
    }

    for (multimapFullIdx i = 0; i < m->count; i++) {
        useMaps[i] = getMap(m, i);
    }

    flex *combined = flexBulkMergeFlex(useMaps, m->count);

    if (m->count <= COUNT_ARRAY(localMaps)) {
        /* ok, we used a local array above */
    } else {
        /* else, free allocated array */
        zfree(useMaps);
    }

    return combined;
}

multimapFull *multimapFullCopy(const multimapFull *restrict const m) {
    /* This is a slight ease of use hack for us.
     *
     * Technically we should just copy:
     *  - m->map
     *  - m->middle
     *  - m->rangeBox
     *      - NOTE: rangeBoxes could be ALLOCATED so need to DUPLCIATE too!
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

    static const size_t stackAllocationDepth = 64;
    flex *maps_[stackAllocationDepth];
    flex **maps = maps_;

    multimapFullMiddle middles_[stackAllocationDepth];
    multimapFullMiddle *middles = middles_;

    if (howMany > COUNT_ARRAY(maps_)) {
        maps = zcalloc(howMany, sizeof(*maps));
        middles = zcalloc(howMany, sizeof(*middles));
    }

    for (size_t i = 0; i < howMany; i++) {
        maps[i] = flexDuplicate(getMap(m, i));
        middles[i] = getMiddle(m, i);
    }

    /* First arg of NULL will allocate a new multimap instead of re-using an
     * existing one. */
    multimapFull *const copied = multimapFullNewFromManyGrow(
        NULL, maps, middles, howMany, m->elementsPerEntry, m->mapIsSet);

    if (maps != maps_) {
        /* We only need one check because they are both sized the same, so if
         * one failed to catch the small size, the other obviously would too. */
        assert(middles != middles_);
        zfree(maps);
        zfree(middles);
    }

    return copied;
}

/* ====================================================================
 * Range Box Searching
 * ==================================================================== */
DK_STATIC multimapFullIdx multimapFullBinarySearch(const multimapFull *m,
                                                   const databox *key) {
    multimapFullIdx min = 0;

    /* 'count - 1' because we don't store a rangeBox for map[0], the
     * range of map[0] is implied to just be "less than the lowest value
     * in map[1]"
     * We always have m->count >= 1. */
    multimapFullIdx max = m->count - 1;

    while (min < max) {
        const size_t mid = (min + max) >> 1;

        const databox *got = getRangeBox(m, mid);
#if 1
        __builtin_prefetch(got->data.bytes.start, 0, 3);
#endif

        const int compared = databoxCompare(got, key);
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
                   const bool useHighestInsertPosition,
                   const bool keysCanBecomePointers, void **keyAsPointer,
                   const bool forceFullWidthComparison) {
    flexEntry *middle = GET_MIDDLE(m, mapIdx, *map);

    bool found = false;

    /* compareUsingKeyElementOnly: For maps (!mapIsSet), compare only key.
     * For sets (mapIsSet), compare all elements to check for full-width dups.
     * If forceFullWidthComparison is true, always compare all elements. */
    const bool compareUsingKeyElementOnly =
        forceFullWidthComparison ? false : !m->mapIsSet;

    if (useSurrogateKey) {
        assert(!keysCanBecomePointers);
        if (useHighestInsertPosition) {
            assert(NULL && "Not implemented!");
            __builtin_unreachable();
        } else {
            found =
                flexInsertReplaceByTypeSortedWithMiddleMultiWithReferenceWithSurrogateKey(
                    map, m->elementsPerEntry, elements, insertKey, &middle,
                    compareUsingKeyElementOnly, referenceContainer);
        }

        updateRangeBoxForIdxWithReference(m, mapIdx, *map, referenceContainer);
    } else {
        if (useHighestInsertPosition) {
            assert(NULL && "Not implemented!");
            __builtin_unreachable();
        } else if (keysCanBecomePointers) {
            found =
                flexInsertReplaceByTypeSortedWithMiddleMultiDirectLongKeysBecomePointers(
                    map, m->elementsPerEntry, elements, &middle,
                    compareUsingKeyElementOnly, keyAsPointer);
        } else {
            found = flexInsertReplaceByTypeSortedWithMiddleMultiDirect(
                map, m->elementsPerEntry, elements, &middle,
                compareUsingKeyElementOnly);
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
                                      const databox *elements[]) {
    return abstractFlexInsert(m, mapIdx, map, elements, false, NULL, NULL,
                              false, false, NULL, false);
}

DK_STATIC bool multimapFullFlexInsertFullWidth(multimapFull *m,
                                               const multimapFullIdx mapIdx,
                                               flex **map,
                                               const databox *elements[]) {
    /* Force full-width comparison (compare all elements, not just key) */
    return abstractFlexInsert(m, mapIdx, map, elements, false, NULL, NULL,
                              false, false, NULL, true);
}

DK_STATIC bool multimapFullFlexInsertExternalizeLargeKeys(
    multimapFull *m, const multimapFullIdx mapIdx, flex **map,
    const databox *elements[], void **keyCreated) {
    return abstractFlexInsert(m, mapIdx, map, elements, false, NULL, NULL,
                              false, true, keyCreated, false);
}

DK_STATIC bool multimapFullFlexInsertWithSurrogateKey(
    multimapFull *m, const multimapFullIdx mapIdx, flex **map,
    const databox *elements[], const databox *insertKey,
    const multimapAtom *referenceContainer) {
    return abstractFlexInsert(m, mapIdx, map, elements, true, insertKey,
                              referenceContainer, false, false, NULL, false);
}

/* ====================================================================
 * Delete Helper
 * ==================================================================== */
DK_INLINE_ALWAYS bool abstractFlexDelete(
    multimapFull *m, const multimapFullIdx mapIdx, const databox *elements[],
    const bool useFullWidth, const bool useReference,
    const multimapAtom *referenceContainer, databox *foundReference) {
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
        if (useReference) {
            foundP = flexFindByTypeSortedWithMiddleWithReference(
                *map, m->elementsPerEntry, elements[0], middle,
                referenceContainer);
        } else {
            foundP = flexFindByTypeSortedWithMiddle(*map, m->elementsPerEntry,
                                                    elements[0], middle);
        }
    }

    if (foundP) {
        if (foundReference) {
            flexGetByType(foundP, foundReference);
        }

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
                              referenceContainer, foundReference);
}

DK_INLINE_ALWAYS bool
multimapFullFlexDeleteWithFound(multimapFull *m, const multimapFullIdx mapIdx,
                                const databox *key, databox *foundReference) {
    const databox *elements[1] = {key};
    return abstractFlexDelete(m, mapIdx, elements, false, false, NULL,
                              foundReference);
}

DK_INLINE_ALWAYS bool multimapFullFlexDeleteFullWidthWithFound(
    multimapFull *m, const multimapFullIdx mapIdx, const databox *elements[],
    databox *foundReference) {
    return abstractFlexDelete(m, mapIdx, elements, true, false, NULL,
                              foundReference);
}

DK_INLINE_ALWAYS bool multimapFullFlexDeleteFullWidthWithReference(
    multimapFull *m, const multimapFullIdx mapIdx, const databox *elements[],
    const multimapAtom *referenceContainer, databox *foundReference) {
    return abstractFlexDelete(m, mapIdx, elements, true, true,
                              referenceContainer, foundReference);
}

int64_t multimapFullFieldIncr(multimapFull *m, const databox *key,
                              uint32_t fieldOffset, int64_t incrBy) {
    uint32_t mapIdx = multimapFullBinarySearch(m, key);

    flex **map = getMapPtr(m, mapIdx);
    flexEntry *middle = GET_MIDDLE(m, mapIdx, *map);

    flexEntry *current =
        flexFindByTypeSortedWithMiddle(*map, m->elementsPerEntry, key, middle);
    while (fieldOffset--) {
        current = flexNext(*map, current);
    }

    int64_t newVal = 0;
    if (flexIncrbySigned(map, current, incrBy, &newVal)) {
        /* if incremented, return new value */
        /* TODO: update flexIncrbySigned() to report if allocation changed
         *       so we only conditionally update the mapIdx */
        /* TODO: BEFORE SHIPPING: ALSO NEED TO UPDATE updateRangeBoxForIdx */
        assert(NULL && "Not correctly implemented yet!");
        SET_MIDDLE_FORCE(m, mapIdx, *map);
        return newVal;
    }

    /* (unlikely) else, return current value */
    databox curVal = {{0}};
    flexGetByType(current, &curVal);
    return curVal.data.i;
}

/* ====================================================================
 * Debuggles API
 * ==================================================================== */
DK_FN_UNUSED static void strictConsistencyCheck(const multimapFull *m) {
    databox key;
    databox b;
    databox c;
    databox d;
    multimapIterator iter;
    databox *ele[4] = {&key, &b, &c, &d};

    /* Verify all range boxes are not sequentially equal... */
    /* Note: will fail if duplicate keys are allowed, kinda obviously. */
    databox *rangeBoxPrev;
    for (size_t i = 0; i < m->count - 1; i++) {
        databox *const box = getRangeBox(m, i);
        if (i > 1) {
            assert(databoxCompare(rangeBoxPrev, box) != 0);
        }

        rangeBoxPrev = box;
    }

    bool isFirst = true;
    multimapFullIteratorInit((multimapFull *)m, &iter, true);
    databox prev;
    while (multimapFullIteratorNext(&iter, ele)) {
        if (isFirst) {
            prev = key;
            isFirst = false;
            continue;
        }

        /* Consistency means previous element is EITHER:
         *  - less than current element, OR
         *  - equals current element (we allow duplicate keys)
         */
        /* Also: 'reference' types don't sort based on their own equality
         * because they may be part of a self-sorting foreign key list. */
        if (prev.type != DATABOX_CONTAINER_REFERENCE_EXTERNAL &&
            key.type != DATABOX_CONTAINER_REFERENCE_EXTERNAL) {
            assert(databoxCompare(&prev, &key) <= 0);
        }

        prev = key;
    }
}

/* ====================================================================
 * Insert API
 * ==================================================================== */
DK_INLINE_ALWAYS bool
abstractInsert(multimapFull *m, const databox *elements[],
               multimapFullIdx mapIdx, const bool useSurrogateKey,
               const databox *insertKey, const multimapAtom *referenceContainer,
               const bool useHighestInsertPosition,
               const bool keysCanBecomePointers, void **keyAsPointer,
               const bool forceFullWidthComparison) {
    assert(!useSurrogateKey || (useSurrogateKey && referenceContainer));

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

    if ((mapBytes <= m->maxSize) || mapElementCount == 0) {
        /* TODO: better (any) size checking on the incoming elements.
         * We could be inserting 1 GB of elements
         * and we don't seem to care at the moment. */

        /* Step 3a: If map has room, insert directly. */
        if (useSurrogateKey) {
            if (useHighestInsertPosition) {
                assert(NULL && "Not implemented!");
                __builtin_unreachable();
            } else {
                found = multimapFullFlexInsertWithSurrogateKey(
                    m, mapIdx, map, elements, insertKey, referenceContainer);
            }
        } else {
            if (useHighestInsertPosition) {
                assert(NULL && "Not implemented!");
                __builtin_unreachable();
            } else if (keysCanBecomePointers) {
                found = multimapFullFlexInsertExternalizeLargeKeys(
                    m, mapIdx, map, elements, keyAsPointer);
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
                found = multimapFullFlexInsert(m, mapIdx, map, elements);
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
                const flexEntry *fe = flexHead(*map);
                if (useSurrogateKey) {
                    flexGetByTypeWithReference(fe, &got, referenceContainer);
                } else {
                    flexGetByType(fe, &got);
                }

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
                map = NULL;
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
                map = NULL;
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
            /* TODO: formalize all this getRangeBox() math justification */
            /* if mapIdx is 0, then the rangebox is INVALID! */

            /* This weird addition is because we just split 'mapIdx' into
             * 'mapIdx' and 'mapIdx + 1' and we need to discover
             * 'currentPositionLargerThanNewKey' based on the HIGHER map,
             * so use higher map index for range box compare. */
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
                if (useHighestInsertPosition) {
                    assert(NULL && "Not implemented!");
                    __builtin_unreachable();
                } else {
                    found = multimapFullFlexInsertWithSurrogateKey(
                        m, mapIdx, map, elements, insertKey,
                        referenceContainer);
                }
            } else {
                if (useHighestInsertPosition) {
                    assert(NULL && "Not implemented!");
                    __builtin_unreachable();
                } else if (forceFullWidthComparison) {
                    found = multimapFullFlexInsertFullWidth(m, mapIdx, map,
                                                            elements);
                } else {
                    found = multimapFullFlexInsert(m, mapIdx, map, elements);
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
                if (useHighestInsertPosition) {
                    assert(NULL && "Not implemented!");
                    __builtin_unreachable();
                } else {
                    found = multimapFullFlexInsertWithSurrogateKey(
                        m, nextIdx, nextMap, elements, insertKey,
                        referenceContainer);
                }
            } else {
                if (useHighestInsertPosition) {
                    assert(NULL && "Not implemented!");
                    __builtin_unreachable();
                } else if (forceFullWidthComparison) {
                    found = multimapFullFlexInsertFullWidth(m, nextIdx, nextMap,
                                                            elements);
                } else {
                    found =
                        multimapFullFlexInsert(m, nextIdx, nextMap, elements);
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
                                   const multimapFullIdx mapIdx) {
    return abstractInsert(m, elements, mapIdx, false, NULL, NULL, false, false,
                          NULL, false);
}

DK_STATIC bool multimapFullInsert_FullWidth_(multimapFull *m,
                                             const databox *elements[],
                                             const multimapFullIdx mapIdx) {
    /* Force full-width comparison (compare all elements, not just key) */
    return abstractInsert(m, elements, mapIdx, false, NULL, NULL, false, false,
                          NULL, true);
}

DK_STATIC bool multimapFullInsertWithSurrogateKey_(
    multimapFull *m, const databox *elements[], const databox *insertKey,
    const multimapFullIdx mapIdx, const multimapAtom *referenceContainer) {
    return abstractInsert(m, elements, mapIdx, true, insertKey,
                          referenceContainer, false, false, NULL, false);
}

bool multimapFullInsertWithSurrogateKey(
    multimapFull *m, const databox *elements[], const databox *insertKey,
    const multimapAtom *referenceContainer) {
    const multimapFullIdx mapIdx = multimapFullBinarySearch(m, elements[0]);
    return multimapFullInsertWithSurrogateKey_(m, elements, insertKey, mapIdx,
                                               referenceContainer);
}

bool multimapFullInsert(multimapFull *m, const databox *elements[]) {
    /* Step 1: find matching map for range. */
    const multimapFullIdx mapIdx = multimapFullBinarySearch(m, elements[0]);
    return multimapFullInsert_(m, elements, mapIdx);
}

bool multimapFullInsertAllowExternalizeKeys(multimapFull *m,
                                            const databox *elements[],
                                            void **keyAllocation) {
    /* Step 1: find matching map for range. */
    const multimapFullIdx mapIdx = multimapFullBinarySearch(m, elements[0]);
    return abstractInsert(m, elements, mapIdx, false, NULL, NULL, false, true,
                          keyAllocation, false);
}

void multimapFullAppend(multimapFull *m, const databox *elements[]) {
    /* directly insert into highest map */
    multimapFullInsert_(m, elements, m->count - 1);
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
    me->mapIdx = multimapFullBinarySearch(m, key);
    me->map = getMapPtr(m, me->mapIdx);
    me->fe =
        flexFindByTypeSortedWithMiddle(*me->map, m->elementsPerEntry, key,
                                       GET_MIDDLE(m, me->mapIdx, *me->map));
    return !!me->fe;
}

bool multimapFullGetUnderlyingEntryGetEntry(multimapFull *m, const databox *key,
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
    me->mapIdx = multimapFullBinarySearch(m, key);
    me->map = getMapPtr(m, me->mapIdx);
    me->fe = flexFindByTypeSortedWithMiddleWithReference(
        *me->map, m->elementsPerEntry, key, GET_MIDDLE(m, me->mapIdx, *me->map),
        referenceContainer);
    return !!me->fe;
}

void multimapFullResizeEntry(multimapFull *m, multimapEntry *me,
                             size_t newLen) {
    flexResizeEntry(me->map, me->fe, newLen);
    SET_MIDDLE_FORCE(m, me->mapIdx, *me->map);
}

void multimapFullReplaceEntry(multimapFull *m, multimapEntry *me,
                              const databox *box) {
    flexReplaceByType(me->map, me->fe, box);
    SET_MIDDLE_FORCE(m, me->mapIdx, *me->map);
}

bool multimapFullRegularizeMap(multimapFull *m, multimapFullIdx mapIdx,
                               flex **map) {
    if (flexCount(*map) > 1 && flexBytes(*map) > m->maxSize) {
        splitMapLowHigh_(m, mapIdx, map, false, NULL);
        return true;
    }

    return false;
}

bool multimapFullRegularizeMapWithReference(
    multimapFull *m, multimapFullIdx mapIdx, flex **map,
    const multimapAtom *referenceContainer) {
    if (flexCount(*map) > 1 && flexBytes(*map) > m->maxSize) {
        splitMapLowHigh_(m, mapIdx, map, true, referenceContainer);
        return true;
    }

    return false;
}

/* ====================================================================
 * Exists API
 * ==================================================================== */
bool multimapFullExists(multimapFull *m, const databox *key) {
    /* TODO: bloom filter */
    const multimapFullIdx mapIdx = multimapFullBinarySearch(m, key);
    const flex *map = getMap(m, mapIdx);
    return !!flexFindByTypeSortedWithMiddle(map, m->elementsPerEntry, key,
                                            GET_MIDDLE(m, mapIdx, map));
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

bool multimapFullExistsFullWidthWithReference(
    multimapFull *m, const databox *elements[],
    const multimapAtom *referenceContainer) {
    const multimapFullIdx mapIdx =
        multimapFullBinarySearchFullWidth(m, elements);
    assert(mapIdx < m->count);
    const flex *map = getMap(m, mapIdx);
    return !!flexFindByTypeSortedWithMiddleFullWidthWithReference(
        map, m->elementsPerEntry, elements, GET_MIDDLE(m, mapIdx, map),
        referenceContainer);
}

/* ====================================================================
 * Single-Key Lookup API
 * ==================================================================== */
DK_INLINE_ALWAYS bool abstractLookup(multimapFull *m, const databox *key,
                                     databox *elements[],
                                     const bool useReference,
                                     const multimapAtom *referenceContainer) {
    /* TODO: bloom filter */
    multimapFullIdx mapIdx = multimapFullBinarySearch(m, key);
    flex *map = getMap(m, mapIdx);
    flexEntry *middle = GET_MIDDLE(m, mapIdx, map);

    flexEntry *foundP;
    if (useReference) {
        foundP = flexFindByTypeSortedWithMiddleWithReference(
            map, m->elementsPerEntry, key, middle, referenceContainer);
    } else {
        foundP = flexFindByTypeSortedWithMiddle(map, m->elementsPerEntry, key,
                                                middle);
    }

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
    return abstractLookup(m, key, elements, false, NULL);
}

bool multimapFullLookupWithReference(multimapFull *m, const databox *key,
                                     databox *elements[],
                                     const multimapAtom *referenceContainer) {
    return abstractLookup(m, key, elements, true, referenceContainer);
}

/* ====================================================================
 * Delete API
 * ==================================================================== */
DK_INLINE_ALWAYS bool abstractDelete(multimapFull *m, const databox **elements,
                                     const bool fullWidth,
                                     const bool useReference,
                                     const multimapAtom *referenceContainer,
                                     databox *foundReference) {
    assert(!useReference || (useReference && referenceContainer));

    /* Step 1: find matching map for range. */
    multimapFullIdx mapIdx;
    const databox *key = elements[0];

    if (fullWidth) {
        mapIdx = multimapFullBinarySearchFullWidth(m, elements);
    } else {
        mapIdx = multimapFullBinarySearch(m, key);
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
            deleted =
                multimapFullFlexDeleteWithFound(m, mapIdx, key, foundReference);
        }
    }

    /* Step 2a: Clean up after successful delete only if we have more
     *          than one map.  If we only have one map, we don't delete it
     *          for ease of adding more data next time. */
    if (deleted && m->count > 1) {
        /* Step 2b: if we have more than one map and this map is now empty,
         *          delete the map and shrink all accounting arrays. */
        if (flexCount(*map) == 0) {
            /* Step 2b1: Free empty map, delete all slots for it. */
            flexFree(*map);

            /* We integrated this check/free into every use of
             * 'reallocDecrCount' itself, but for efficiency we should really
             * only be doing databoxFreeData() when we are using reference
             * actions. (But, the reference actions we had annotated already
             * were missing a few databoxFreeData() calls somewhere? So, for now
             * it's easier to have verified-free by running
             * getRangeBox->databoxFreeData a little more often than required to
             * not leave leaks anywhere. */
#if 0
            if (useReference) {
                /* When using references in a Full, the range boxes may contain
                 * allocated data which needs to be free'd when range boxes
                 * are deleted or updated or changed */

                /* See reallocDecrCount() for why this math is the way it is */
                if (mapIdx > 0) {
                    databox *rangeBox = getRangeBox(m, mapIdx - 1);
                    databoxFreeData(rangeBox);
                }
            }
#endif

            reallocDecrCount(m, mapIdx);
        } else if (nextMapIdxExists(m, mapIdx)) {
            /* Step 2b2: else, map still has contents, so attempt to
             *           merge mapIdx with (mapIdx + 1).
             *
             * Future optimization: could also try to merge
             *                      prev->current, current->next, next->next */
            mergeSimple(m, mapIdx, useReference, referenceContainer);
        }
    }

    return deleted;
}

void multimapFullDeleteEntry(multimapFull *m, multimapEntry *me) {
    /* Step 1: pick victim map */
    const multimapFullIdx mapIdx = me->mapIdx;

    flex **map = me->map;
    flexEntry *middle = GET_MIDDLE(m, mapIdx, *map);

    /* Step 2: pick victim element */
    flexEntry *foundP = me->fe;

    flexDeleteSortedValueWithMiddle(map, m->elementsPerEntry, foundP, &middle);

    /* This is code copied from:
     *   - abstractFlexDelete
     *      - AND FROM -
     *   - abstractDelete */

    /* Step 3a: Repair multimapFull state from entry delete */
    m->values--;
    SET_MIDDLE(m, mapIdx, middle, *map);

    /* TODO: this doesn't work for reference maps since we can leak the
     * reference rangebox */
    updateRangeBoxForIdx(m, mapIdx, *map);

    /* Step 3b: Repair state if map is now empty (from 'abstractDelete') */
    if (m->count > 1) {
        if (flexCount(*map) == 0) {
            flexFree(*map);
            reallocDecrCount(m, mapIdx);
        } else if (nextMapIdxExists(m, mapIdx)) {
            mergeSimple(m, mapIdx, false, NULL);
        }
    }
}

bool multimapFullDelete(multimapFull *m, const databox *key) {
    const databox *elements[1] = {key};
    return abstractDelete(m, elements, false, false, NULL, NULL);
}

bool multimapFullDeleteNMapsIterate(multimapFull *m, const size_t n,
                                    multimapFullMapDeleter *mapIter,
                                    void *data) {
    for (size_t i = 0; i < n; i++) {
        /* If we run out of maps, return failure because there's
         * nothing more to process. */
        if (m->count == 0) {
            break;
        }

        /* Always delete zero map... */
        const size_t mapIdx = 0;

        /* Get map 0 */
        flex **map = getMapPtr(m, mapIdx);

        if (mapIter) {
            mapIter(*map, data);
        }

        /* Free map 0 */
        flexFree(*map);

#if 0
        if (nextMapIdxExists(m, mapIdx)) {
            databox *rangeBox = getRangeBox(m, mapIdx + 1);
            databoxFreeData(rangeBox);
        }
#endif

        /* Remove range box and middle for map 0 */
        reallocDecrCount(m, mapIdx);
    }

    /* Return true if more maps can be deleted next time. */
    /* Return false if no more maps exist. */
    return !!m->count;
}

bool multimapFullDeleteNMaps(multimapFull *m, const size_t n) {
    return multimapFullDeleteNMapsIterate(m, n, NULL, NULL);
}

bool multimapFullDeleteFullWidth(multimapFull *m, const databox *elements[]) {
    return abstractDelete(m, elements, true, false, NULL, NULL);
}

bool multimapFullDeleteFullWidthWithFound(multimapFull *m,
                                          const databox *elements[],
                                          databox *foundReference) {
    return abstractDelete(m, elements, true, false, NULL, foundReference);
}

bool multimapFullDeleteWithReference(multimapFull *m, const databox *key,
                                     const multimapAtom *referenceContainer,
                                     databox *foundReference) {
    const databox *elements[1] = {key};
    return abstractDelete(m, elements, false, true, referenceContainer,
                          foundReference);
}

bool multimapFullDeleteWithFound(multimapFull *m, const databox *key,
                                 databox *foundReference) {
    const databox *elements[1] = {key};
    return abstractDelete(m, elements, false, false, NULL, foundReference);
}

bool multimapFullDeleteFullWidthWithReference(
    multimapFull *m, const databox *elements[],
    const multimapAtom *referenceContainer, databox *foundReference) {
    return abstractDelete(m, elements, true, true, referenceContainer,
                          foundReference);
}

bool multimapFullRandomValue(multimapFull *m, const bool fromTail,
                             databox **foundBox, multimapEntry *me) {
    if (m->values == 0) {
        return false;
    }

    /* Step 1: pick victim map */
    const multimapFullIdx mapIdx = random() % m->count;
    flex **map = getMapPtr(m, mapIdx);

    /* Step 2: pick victim element */
    flexEntry *foundP = NULL;
    if (fromTail) {
        foundP = flexTailWithElements(*map, m->elementsPerEntry);
    } else {
        /* delete random entry in the map */
        const uint_fast32_t totalWholeElements =
            flexCount(*map) / m->elementsPerEntry;
        const uint_fast32_t randomElement = random() % totalWholeElements;

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

bool multimapFullDeleteRandomValue(multimapFull *m, const bool deleteFromTail,
                                   databox **deletedBox) {
    multimapEntry me;
    /* Pass NULL for deletedBox to multimapFullRandomValue, then manually
     * copy the data using flexGetByTypeCopy. This avoids use-after-free
     * since flexGetByType returns pointers to internal flex data which
     * become invalid after multimapFullDeleteEntry. */
    if (!multimapFullRandomValue(m, deleteFromTail, NULL, &me)) {
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

    multimapFullDeleteEntry(m, &me);
    return true;
}

/* ====================================================================
 * Reset API
 * ==================================================================== */
/* TODO: reset-with-reference to release all retained atoms */
void multimapFullReset(multimapFull *m) {
    for (multimapFullIdx idx = 0; idx < m->count; idx++) {
        flex **map = getMapPtr(m, idx);
        flexReset(map);
        SET_MIDDLE_FORCE(m, idx, *map);
        updateRangeBoxForIdx(m, idx, *map);
    }

    /* We deleted all elements, so reset the cached
     * entry count back to zero. */
    m->values = 0;
}

/* ====================================================================
 * Free / Release API
 * ==================================================================== */
/* TODO: free-with-reference to release all retained atoms */
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
    if (me.fe == *me.map + flexBytes(*me.map)) {
        me.fe = NULL;
    }

    multimapFullIteratorInitAt_(m, iter, forward, &me);
    return !!me.fe;
}

bool multimapFullIteratorInit(multimapFull *m, multimapIterator *iter,
                              bool forward) {
    flex *tmp = NULL;
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
        me.fe = NULL;
        me.mapIdx = m->count - 1;
        me.map = &tmp;
    }

    return multimapFullIteratorInitAt_(m, iter, forward, &me);
}

bool multimapFullIteratorNext(multimapIterator *iter, databox *elements[]) {
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

    /* If moving forward and reached end of map,
     * begin iterating over next map. */
    if (iter->forward) {
        multimapFull *local = (multimapFull *)iter->mm;
        iter->mapIndex++;
        if (iter->mapIndex < local->count) {
            iter->map = getMap(local, iter->mapIndex);
            iter->entry = flexHead(iter->map);

            /* We DO NOT allow empty maps, so we MUST have a head of a map. */
            assert(flexCount(iter->map) &&
                   "multimapFull interior map is empty?");

            return multimapFullIteratorNext(iter, elements);
        }

        /* ran out of maps to iterate! */
        return false;
    }

    /* If moving reverse and reached beginning of map,
     * begin iterating over previous map (if exists). */
    /* Note: we check BEFORE decrementing to ensure we process map 0.
     * If we decrement first, then check > 0, we would skip map 0. */
    if (!iter->forward && iter->mapIndex > 0) {
        iter->mapIndex--;
        multimapFull *local = (multimapFull *)iter->mm;
        iter->map = getMap(local, iter->mapIndex);
        iter->entry = flexTail(iter->map);
        return multimapFullIteratorNext(iter, elements);
    }

    return false;
}

bool multimapFullDeleteByPredicate(multimapFull *m,
                                   const multimapPredicate *p) {
    multimapEntry me = {0};
    multimapFullGetUnderlyingEntry(m, &p->compareAgainst, &me);

    if (!me.fe) {
        return false;
    }

    int compared = 1;
    if (!flexEntryIsValid(*me.map, me.fe)) {
        databox value = {{0}};
        flexGetByType(me.fe, &value);

        compared = databoxCompare(&value, &p->compareAgainst);
    }

    switch (p->condition) {
    case MULTIMAP_CONDITION_LESS_THAN_EQUAL:
        for (multimapFullIdx i = 0; i < me.mapIdx; i++) {
            /* If we found our entry in a map greater than zero,
             * we can delete all lower maps with no element traversal.
             *
             * We always delete '0' here because everytime we delete '0',
             * the next highest map becomes the new '0' */
            flex *map = getMap(m, 0);
            flexFree(map);
            reallocDecrCount(m, 0);
        }

        /* We need to get the entry again because the previous
         * deletes could have moved our index value */
        multimapFullGetUnderlyingEntry(m, &p->compareAgainst, &me);

        /* Since this is a "delete all <=," at this point we should
         * **only** ever be deleting from map 0 (because we deleted
         * all lower maps in the 'for' loop immediately above */
        assert(me.mapIdx == 0);

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

        SET_MIDDLE_FORCE(m, me.mapIdx, *me.map);
        return true;
    default:
        /* We don't support the other predicate types yet */
        assert(NULL && "Not Implemented!");
        return false;
    }

    return false;
}

#ifdef DATAKIT_TEST
#define CTEST_INCLUDE_KVGEN
#include "ctest.h"

#define DOUBLE_NEWLINE 0
#include "perf.h"

#define REPORT_TIME 1
#if REPORT_TIME
#define TIME_INIT PERF_TIMERS_SETUP
#define TIME_FINISH(i, what) PERF_TIMERS_FINISH_PRINT_RESULTS(i, what)
#else
#define TIME_INIT
#define TIME_FINISH(i, what)
#endif

#define multimapFullReport(m) multimapFullReport_(m, true)
#define multimapFullReportSizeOnly(m) multimapFullReport_(m, false)

void multimapFullConforms(const multimapFull *m) {
    for (multimapFullIdx i = 0; i < m->count; i++) {
        if (i > 0 && flexCount(getMap(m, i)) == 0) {
            multimapFullRepr(m);
            assert(NULL && "Interior empty map? How'd we do that?");
            __builtin_unreachable();
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
            assert(NULL && "Interior empty map? How'd we do that?");
            __builtin_unreachable();
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

    databox lowest = {{0}};
    size_t lowestMapIdx = 0;
    flexGetByType(getMapLowest(m), &lowest);

    for (multimapFullIdx i = 0; i < m->count; i++) {
        flex *zl = getMap(m, i);
        flexEntry *middle = GET_MIDDLE(m, i, zl);

        assert(middle == flexMiddle(zl, m->elementsPerEntry));

        for (multimapFullIdx j = 0; j < flexCount(zl);
             j += m->elementsPerEntry) {
            databox next = {{0}};
            flexGetByType(flexIndex(zl, j), &next);

            if (i > 0 && databoxCompare(&lowest, &next) > 0) {
                printf("Lowest was previously %.*s from map %zu, but found "
                       "element %.*s at (%d, %d)\n",
                       (int)lowest.len, lowest.data.bytes.start, lowestMapIdx,
                       (int)next.len, next.data.bytes.start, i, j);
                assert(NULL);
                __builtin_unreachable();
            }

            lowest = next;
            lowestMapIdx = i;
        }
    }
}

int multimapFullTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

#if 1
    TEST("create") {
        multimapFull *m = multimapFullNew(2);
        multimapFullFree(m);
    }

    TEST("test gaps") {
        multimapFull *m = multimapFullNew(2);
        m->maxSize = 1024;

        const size_t howMany = 1024;
        /* Insert (with gaps) */
        for (size_t gap = 20; gap > 0; gap--) {
            for (size_t i = 0; i < howMany; i += gap) {
                const databox keybox = {.data.u = i,
                                        .type = DATABOX_UNSIGNED_64};
                const databox valbox = DATABOX_BOX_FALSE;
                const databox *elements[2] = {&keybox, &valbox};
                multimapFullInsert(m, elements);
            }
        }

#if 0
        multimapFullRepr(m);
#endif
        strictConsistencyCheck(m);
        multimapFullFree(m);
    }

    TEST("speeds at different sizes") {
        int32_t maxMax = 1 << 16;
        uint64_t secs[20] = {0};
        size_t size[20] = {0};
        int i = 0;
        for (int32_t maxSize = 1; maxSize < maxMax; maxSize *= 2) {
            multimapFull *m = multimapFullNew(2);
            m->maxSize = maxSize;
            int32_t pairs = 1 << 17;
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
                (void)secs;
                (void)size;
                i++;
            }

            multimapFullConforms(m);
            multimapFullReport(m);
            multimapFullVerify(m);
            multimapFullFree(m);
        }
    }

    printf("\n\n");
#endif

#if 1
    TEST_DESC("%d k/v pairs - inserting...", 300) {
        multimapFull *m = multimapFullNew(2);
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
        multimapFullFree(m);
    }

    printf("\n\n");

    for (int32_t i = 0; i < 4096; i++) {
        multimapFull *m = multimapFullNew(2);
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
                    assert(NULL);
                    __builtin_unreachable();
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
                    assert(NULL);
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

        multimapFullFree(m);
        printf("\n");
    }

#endif

    for (int32_t i = 0; i < 4096; i++) {
        multimapFull *m = multimapFullNew(2);
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
                    assert(NULL);
                    __builtin_unreachable();
                }
            }

            TIME_FINISH(i, "exists");
            assert(multimapFullCount(m) == (uint32_t)i);
        }

        multimapFullFree(m);
        printf("\n");
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
