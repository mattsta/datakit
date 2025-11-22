#include "multimap.h"

#include "multimapSmall.h"
#include "multimapSmallInternal.h"

#include "multimapMedium.h"
#include "multimapMediumInternal.h"

#include "multimapFull.h"
#include "multimapFullInternal.h"

#if MULTIMAP_INLINE
#include "multimapFull.c"
#include "multimapMedium.c"
#include "multimapSmall.c"
#endif

#define PTRLIB_BITS_LOW 2
#define PTRLIB_BITS_HIGH 16
#include "ptrlib.h"
#include "ptrlibCompress.h"

#include "flexCapacityManagement.h"

#define multimapType_(map) _PTRLIB_TYPE(map)
#define MULTIMAP_USE_(map) _PTRLIB_TOP_USE_ALL(map)
#define MULTIMAP_TAG_(map, type) ((multimap *)_PTRLIB_TAG(map, type))

#define mms(m) ((multimapSmall *)MULTIMAP_USE_(m))
#define mmm(m) ((multimapMedium *)MULTIMAP_USE_(m))
#define mmf(m) ((multimapFull *)MULTIMAP_USE_(m))

#define MULTIMAP_USE_COMPUTED_GOTO_JUMPING 1

#if MULTIMAP_USE_COMPUTED_GOTO_JUMPING
#define MULTIMAP_SETUP_LABELS(m)                                               \
    __label__ mapSmall;                                                        \
    __label__ mapMedium;                                                       \
    __label__ mapFull;                                                         \
    static const void **jumper[4] = {NULL, &&mapSmall, &&mapMedium,            \
                                     &&mapFull};                               \
    goto *jumper[multimapType_(m)]
#endif

#if !MULTIMAP_USE_COMPUTED_GOTO_JUMPING
#define MULTIMAP_(ret, m, func, ...)                                           \
    do {                                                                       \
        switch (multimapType_(m)) {                                            \
        case MULTIMAP_TYPE_SMALL:                                              \
            ret multimapSmall##func(mms(m), __VA_ARGS__);                      \
            break;                                                             \
        case MULTIMAP_TYPE_MEDIUM:                                             \
            ret multimapMedium##func(mmm(m), __VA_ARGS__);                     \
            break;                                                             \
        case MULTIMAP_TYPE_FULL:                                               \
            ret multimapFull##func(mmf(m), __VA_ARGS__);                       \
            break;                                                             \
        default:                                                               \
            assert(NULL);                                                      \
            __builtin_unreachable();                                           \
        }                                                                      \
    } while (0)
#else
#define MULTIMAP_(ret, m, func, ...)                                           \
    do {                                                                       \
        MULTIMAP_SETUP_LABELS(m);                                              \
        do {                                                                   \
        mapSmall:                                                              \
            ret multimapSmall##func(mms(m), __VA_ARGS__);                      \
            break;                                                             \
        mapMedium:                                                             \
            ret multimapMedium##func(mmm(m), __VA_ARGS__);                     \
            break;                                                             \
        mapFull:                                                               \
            ret multimapFull##func(mmf(m), __VA_ARGS__);                       \
            break;                                                             \
        } while (0);                                                           \
    } while (0)
#endif

#define MULTIMAP_NORETURN(m, func, ...) MULTIMAP_(, m, func, __VA_ARGS__)
#define MULTIMAP_RETURN(m, func, ...) MULTIMAP_(return, m, func, __VA_ARGS__)
#define MULTIMAP_SETRESULT(var, m, func, ...)                                  \
    MULTIMAP_(var =, m, func, __VA_ARGS__)

#if !MULTIMAP_USE_COMPUTED_GOTO_JUMPING
#define MULTIMAP_SINGLE_(ret, m, func)                                         \
    do {                                                                       \
        switch (multimapType_(m)) {                                            \
        case MULTIMAP_TYPE_SMALL:                                              \
            ret multimapSmall##func(mms(m));                                   \
            break;                                                             \
        case MULTIMAP_TYPE_MEDIUM:                                             \
            ret multimapMedium##func(mmm(m));                                  \
            break;                                                             \
        case MULTIMAP_TYPE_FULL:                                               \
            ret multimapFull##func(mmf(m));                                    \
            break;                                                             \
        default:                                                               \
            assert(NULL);                                                      \
            __builtin_unreachable();                                           \
        }                                                                      \
    } while (0)
#else
#define MULTIMAP_SINGLE_(ret, m, func)                                         \
    do {                                                                       \
        MULTIMAP_SETUP_LABELS(m);                                              \
        do {                                                                   \
        mapSmall:                                                              \
            ret multimapSmall##func(mms(m));                                   \
            break;                                                             \
        mapMedium:                                                             \
            ret multimapMedium##func(mmm(m));                                  \
            break;                                                             \
        mapFull:                                                               \
            ret multimapFull##func(mmf(m));                                    \
            break;                                                             \
        } while (0);                                                           \
    } while (0)
#endif

#define MULTIMAP_SINGLE_NORETURN(m, func) MULTIMAP_SINGLE_(, m, func)
#define MULTIMAP_SINGLE_RETURN(m, func) MULTIMAP_SINGLE_(return, m, func)

multimap *multimapNew(multimapElements elementsPerEntry) {
    /* Note: You can only have up to 64k 'elementsPerEntry'
     *       (which is overkill anyway) */
    multimapSmall *created = multimapSmallNew(elementsPerEntry, false);
    created = SET_COMPRESS_DEPTH_LIMIT(created, false, FLEX_CAP_LEVEL_2048);
    return MULTIMAP_TAG_(created, MULTIMAP_TYPE_SMALL);
}

multimap *multimapNewLimit(multimapElements elementsPerEntry,
                           const flexCapSizeLimit limit) {
    multimapSmall *created = multimapSmallNew(elementsPerEntry, false);
    created = SET_COMPRESS_DEPTH_LIMIT(created, false, limit);
    return MULTIMAP_TAG_(created, MULTIMAP_TYPE_SMALL);
}

multimap *multimapNewCompress(multimapElements elementsPerEntry,
                              const flexCapSizeLimit limit) {
    multimap *created = (multimap *)multimapSmallNew(elementsPerEntry, false);
    created = SET_COMPRESS_DEPTH_LIMIT(created, true, limit);
    return MULTIMAP_TAG_(created, MULTIMAP_TYPE_SMALL);
}

multimap *multimapSetNew(multimapElements elementsPerEntry) {
    multimap *created = (multimap *)multimapSmallNew(elementsPerEntry, true);
    created = SET_COMPRESS_DEPTH_LIMIT(created, false, FLEX_CAP_LEVEL_2048);
    return MULTIMAP_TAG_(created, MULTIMAP_TYPE_SMALL);
}

multimap *multimapNewConfigure(multimapElements elementsPerEntry,
                               const bool isSet, const bool compress,
                               const flexCapSizeLimit sizeLimit) {
    multimap *created = (multimap *)multimapSmallNew(elementsPerEntry, isSet);
    created = SET_COMPRESS_DEPTH_LIMIT(created, compress, sizeLimit);
    return MULTIMAP_TAG_(created, MULTIMAP_TYPE_SMALL);
}

size_t multimapCount(const multimap *m) {
    MULTIMAP_SINGLE_RETURN(m, Count);
}

size_t multimapBytes(const multimap *m) {
    MULTIMAP_SINGLE_RETURN(m, Bytes);
}

flex *multimapDump(const multimap *m) {
    MULTIMAP_SINGLE_RETURN(m, Dump);
}

DK_INLINE_ALWAYS void multimapUpgradeIfNecessary_(
    multimap **m, const uint_fast32_t depth, const flexCapSizeLimit limit,
    const bool useReference, const multimapAtom *restrict referenceContainer) {
    const multimapType type = multimapType_(*m);
    if (type == MULTIMAP_TYPE_SMALL) {
        multimapSmall *small = mms(*m);
        /* Note: the '* 2' below is because the multimapSmall **MUST**
         *       have at least two complete entries in order to be split
         *       into a medium.
         *       Without 2 complete entries, the medium will break because it
         *       will split [ELEMENT, <NOTHING>] then the medium map will
         *       insert *all* contents only into the first split map, never
         *       into the second split map. */
        if (multimapSmallBytes(small) > flexOptimizationSizeLimit[limit] &&
            multimapSmallCount(small) > (small->elementsPerEntry * 2)) {
            /* Medium self-manages reference lookups, so we do not need an
             * independent multimapMediumNewFromOneGrowWithReference() */
            multimapMedium *medium = multimapMediumNewFromOneGrow(
                small, small->map, small->middle, small->elementsPerEntry,
                small->mapIsSet);

            medium = SET_COMPRESS_DEPTH_LIMIT(medium, depth, limit);
            *m = MULTIMAP_TAG_(medium, MULTIMAP_TYPE_MEDIUM);
        }
    } else if (type == MULTIMAP_TYPE_MEDIUM) {
        multimapMedium *medium = mmm(*m);
        if (multimapMediumBytes(medium) >
                (flexOptimizationSizeLimit[limit] * 3) &&
            flexCount(medium->map[0]) && flexCount(medium->map[1])) {
            multimapFullMiddle middles[2] = {medium->middle[0],
                                             medium->middle[1]};
            /* Note: we manually set 'maps' here instead of passing in
             * 'middle->maps' because *Grow() reallocs the struct and will
             * kill the pointers inside the struct before it has a chance
             * to use the data. */
            flex *maps[2] = {medium->map[0], medium->map[1]};

            /* multimapFull stores an array of external databoxes to determine
             * which map has which elements, so if we are using references, we
             * must tell multimapFull those boxes should be underlying reference
             * values, not direct values themselves. */
            multimapFull *full;
            if (useReference) {
                full = multimapFullNewFromTwoGrowWithReference(
                    medium, maps, middles, medium->elementsPerEntry,
                    medium->mapIsSet, referenceContainer);
            } else {
                full = multimapFullNewFromTwoGrow(medium, maps, middles,
                                                  medium->elementsPerEntry,
                                                  medium->mapIsSet);
            }

            full->maxSize = flexOptimizationSizeLimit[limit];
            *m = MULTIMAP_TAG_(full, MULTIMAP_TYPE_FULL);
        }
    }
}

static void multimapUpgradeIfNecessary(multimap **m, const uint_fast32_t depth,
                                       const flexCapSizeLimit limit) {
    multimapUpgradeIfNecessary_(m, depth, limit, false, NULL);
}

static void multimapUpgradeIfNecessaryWithReference(
    multimap **m, const uint_fast32_t depth, const flexCapSizeLimit limit,
    const multimapAtom *restrict referenceContainer) {
    multimapUpgradeIfNecessary_(m, depth, limit, true, referenceContainer);
}
multimap *multimapCopy(const multimap *m) {
    const uint32_t depth = COMPRESS_DEPTH(m);
    const flexCapSizeLimit limit = COMPRESS_LIMIT(m);

    multimap *copy;

    const multimapType type = multimapType_(m);
    if (type == MULTIMAP_TYPE_SMALL) {
        copy = (multimap *)multimapSmallCopy(mms(m));
    } else if (type == MULTIMAP_TYPE_MEDIUM) {
        copy = (multimap *)multimapMediumCopy(mmm(m));
    } else {
        copy = (multimap *)multimapFullCopy(mmf(m));
    }

    copy = SET_COMPRESS_DEPTH_LIMIT(copy, depth, limit);
    copy = MULTIMAP_TAG_(copy, type);
    return copy;
}

bool multimapInsert(multimap **m, const databox *elements[]) {
    /* Insert new elements */
    bool replaced;
    MULTIMAP_SETRESULT(replaced, *m, Insert, elements);

    /* Now check multimap to see if we should grow to a larger
     * representation. */
    const uint32_t depth = COMPRESS_DEPTH(*m);
    const flexCapSizeLimit limit = COMPRESS_LIMIT(*m);
    multimapUpgradeIfNecessary(m, depth, limit);
    return replaced;
}

void multimapInsertFullWidth(multimap **m, const databox *elements[]) {
    /* Insert new elements */
    MULTIMAP_NORETURN(*m, InsertFullWidth, elements);

    /* Now check multimap to see if we should grow to a larger
     * representation. */
    const uint32_t depth = COMPRESS_DEPTH(*m);
    const flexCapSizeLimit limit = COMPRESS_LIMIT(*m);
    multimapUpgradeIfNecessary(m, depth, limit);
}

void multimapInsertWithSurrogateKey(multimap **m, const databox *elements[],
                                    const databox *insertKey,
                                    const multimapAtom *referenceContainer) {
    /* Insert new elements */
    MULTIMAP_NORETURN(*m, InsertWithSurrogateKey, elements, insertKey,
                      referenceContainer);

    /* Now check multimap to see if we should grow to a larger
     * representation. */
    const uint32_t depth = COMPRESS_DEPTH(*m);
    const flexCapSizeLimit limit = COMPRESS_LIMIT(*m);
    multimapUpgradeIfNecessaryWithReference(m, depth, limit,
                                            referenceContainer);
}

void multimapAppend(multimap **m, const databox *elements[]) {
    MULTIMAP_NORETURN(m, Append, elements);

    const uint32_t depth = COMPRESS_DEPTH(*m);
    const flexCapSizeLimit limit = COMPRESS_LIMIT(*m);
    multimapUpgradeIfNecessary(m, depth, limit);
}

bool multimapGetUnderlyingEntry(multimap *m, const databox *key,
                                multimapEntry *me) {
    MULTIMAP_RETURN(m, GetUnderlyingEntry, key, me);
}

bool multimapGetUnderlyingEntryWithReference(
    multimap *m, const databox *key, multimapEntry *me,
    const multimapAtom *referenceContainer) {
    MULTIMAP_RETURN(m, GetUnderlyingEntryWithReference, key, me,
                    referenceContainer);
}

void multimapRegularizeMap(multimap **m, multimapFullIdx mapIdx, flex **map) {
    const multimapType type = multimapType_(*m);
    if (type == MULTIMAP_TYPE_SMALL || type == MULTIMAP_TYPE_MEDIUM) {
        const uint32_t depth = COMPRESS_DEPTH(*m);
        const flexCapSizeLimit limit = COMPRESS_LIMIT(*m);
        multimapUpgradeIfNecessary(m, depth, limit);
    } else {
        multimapFull *full = mmf(*m);
        multimapFullRegularizeMap(full, mapIdx, map);
    }
}

void multimapResizeEntry(multimap **m, multimapEntry *me, size_t newLen) {
    MULTIMAP_NORETURN(*m, ResizeEntry, me, newLen);
    multimapRegularizeMap(m, me->mapIdx, me->map);
}

void multimapReplaceEntry(multimap **m, multimapEntry *me, const databox *box) {
    MULTIMAP_NORETURN(*m, ReplaceEntry, me, box);

    /* NOTE: DO NOT regularize map here because if a map requires surrogate keys
     * and we regularize the map, we aren't passing through the
     * referenceContainer here, so this horks the maps. We'd need to pass
     * referenceContainer all the way through the multimapFieldIncr() chain
     * because Incr() calls this multimapReplaceEntry()... it's easier to just
     * allow some sub-optimal maps at the moment. */
#if 0
    multimapRegularizeMap(m, me->mapIdx, me->map);
#endif
}

void multimapReplaceEntryWithReference(multimap **m, multimapEntry *me,
                                       const databox *box,
                                       const multimapAtom *referenceContainer) {
    MULTIMAP_NORETURN(*m, ReplaceEntry, me, box);

    const multimapType type = multimapType_(*m);
    if (type == MULTIMAP_TYPE_FULL) {
        multimapFull *full = mmf(*m);
        multimapFullRegularizeMapWithReference(full, me->mapIdx, me->map,
                                               referenceContainer);
    }
}

bool multimapExists(const multimap *m, const databox *key) {
    MULTIMAP_RETURN(m, Exists, key);
}

bool multimapExistsFullWidth(const multimap *m, const databox *elements[]) {
    MULTIMAP_RETURN(m, ExistsFullWidth, elements);
}

bool multimapExistsWithReference(
    const multimap *m, const databox *key, databox *foundRef,
    const struct multimapAtom *referenceContainer) {
    MULTIMAP_RETURN(m, ExistsWithReference, key, foundRef, referenceContainer);
}

bool multimapLookup(const multimap *m, const databox *key,
                    databox *elements[]) {
    MULTIMAP_RETURN(m, Lookup, key, elements);
}

bool multimapDelete(multimap **m, const databox *key) {
    /* TODO: auto-shrink behavior?  How to decide when to shrink
     * from Full -> Medium -> Small? */
    MULTIMAP_RETURN(*m, Delete, key);
}

bool multimapDeleteWithReference(multimap **m, const databox *key,
                                 const struct multimapAtom *referenceContainer,
                                 databox *foundReference) {
    /* TODO: auto-shrink behavior?  How to decide when to shrink
     * from Full -> Medium -> Small? */
    MULTIMAP_RETURN(*m, DeleteWithReference, key, referenceContainer,
                    foundReference);
}

bool multimapDeleteWithFound(multimap **m, const databox *key,
                             databox *foundReference) {
    /* TODO: auto-shrink behavior?  How to decide when to shrink
     * from Full -> Medium -> Small? */
    MULTIMAP_RETURN(*m, DeleteWithFound, key, foundReference);
}

bool multimapDeleteFullWidth(multimap **m, const databox *elements[]) {
    /* TODO: auto-shrink behavior?  How to decide when to shrink
     * from Full -> Medium -> Small? */
    MULTIMAP_RETURN(*m, DeleteFullWidth, elements);
}

bool multimapRandomValue(multimap *m, const bool fromTail, databox **found,
                         multimapEntry *me) {
    MULTIMAP_RETURN(m, RandomValue, fromTail, found, me);
}

bool multimapDeleteRandomValue(multimap **m, const bool fromTail,
                               databox **deleted) {
    MULTIMAP_RETURN(*m, DeleteRandomValue, fromTail, deleted);
}

#if 0
/* This interface is unstable because flex doesn't return offsets
 * of movement if field grows, so middles can't be updated. */
int64_t multimapFieldIncr(multimap **m, const databox *key,
                          uint32_t fieldOffset, int64_t incrBy) {
    MULTIMAP_RETURN(*m, FieldIncr, key, fieldOffset, incrBy);

    /* Note: incrby could create a bigger number allocation,
     *       but we aren't upgrading maps here. */
}
#else
int64_t multimapFieldIncr(multimap **m, const databox *key,
                          uint32_t fieldOffset, int64_t incrBy) {
    /* Step 1: fetch entry for key */
    multimapEntry me;
    multimapGetUnderlyingEntry(*m, key, &me);

    /* Step 2: iterate to offset field */
    while (fieldOffset--) {
        me.fe = flexNext(*me.map, me.fe);
    }

    /* Step 3: read offset field */
    databox current;
    flexGetByType(me.fe, &current);

    /* Nothing to change... */
    if (incrBy == 0) {
        return current.data.i;
    }

    /* Step 4: run increment */
    /* We're safe using 'i' because we only incr/decr by int64_t,
     * but this also implies you shouldn't FieldIncr anything
     * with value > INT64_MAX */
    assert(/* This isn't fully inclusive of conditions, but enough for now */
           (current.type == DATABOX_UNSIGNED_64 &&
            current.data.u <= INT64_MAX) ||
           (current.type == DATABOX_SIGNED_64) ||
           (current.type == DATABOX_TRUE) || (current.type == DATABOX_FALSE));

    /* This is a bit weird because we allow FALSE to be an implicit 0 and
     * we allow TRUE to be an implicit 1, and we allow math operations against
     * bool types. */
    if (incrBy < 0) {
        /* If incrementing negative, FALSE becomes signed; TRUE becomes FALSE */
        if (incrBy == -1 && current.type == DATABOX_TRUE) {
            current.type = DATABOX_FALSE;
        } else {
            current.type = DATABOX_SIGNED_64;
        }
    } else {
        /* If incrementing positive, FALSE becomes TRUE; TRUE becomes signed. */
        if (incrBy == 1 && current.type == DATABOX_FALSE) {
            current.type = DATABOX_TRUE;
        } else {
            current.type = DATABOX_SIGNED_64;
        }
    }

    /* Also note: we aren't checking for overflow here... */
    current.data.i += incrBy;

#if 0
    databoxReprSay("Setting value to", &current);
#endif

    /* Step 5: replace entry */
    multimapReplaceEntry(m, &me, &current);

    /* Step 6: return updated count */
    return current.data.i;
}

#endif

void multimapReset(multimap *m) {
    MULTIMAP_SINGLE_NORETURN(m, Reset);
}

void multimapFree(multimap *m) {
    if (m) {
        MULTIMAP_SINGLE_NORETURN(m, Free);
    }
}

/* Positional Operations */
bool multimapFirst(multimap *m, databox *elements[]) {
    MULTIMAP_RETURN(m, First, elements);
}

bool multimapLast(multimap *m, databox *elements[]) {
    MULTIMAP_RETURN(m, Last, elements);
}

bool multimapProcessPredicate(const multimapPredicate *p,
                              const databox *value) {
    /* Short circuit ALL condition so we don't waste time in databoxCompare() */
    if (p->condition == MULTIMAP_CONDITION_ALL) {
        return true;
    }

    int compared = databoxCompare(value, &p->compareAgainst);
    switch (p->condition) {
    case MULTIMAP_CONDITION_ALL:
        return true;
        break;
    case MULTIMAP_CONDITION_LESS_THAN_EQUAL:
        if (compared <= 0) {
            return true;
        }
        break;
    case MULTIMAP_CONDITION_LESS_THAN:
        if (compared < 0) {
            return true;
        }
        break;
    case MULTIMAP_CONDITION_EQUAL:
        if (compared == 0) {
            return true;
        }
        break;
    case MULTIMAP_CONDITION_GREATER_THAN:
        if (compared > 0) {
            return true;
        }
        break;
    case MULTIMAP_CONDITION_GREATER_THAN_EQUAL:
        if (compared >= 0) {
            return true;
        }
        break;
    default:
        assert(NULL);
        __builtin_unreachable();
    }

    return false;
}

bool multimapDeleteByPredicate(multimap **m, const multimapPredicate *p) {
    MULTIMAP_RETURN(*m, DeleteByPredicate, p);
}

bool multimapIteratorInitAt(const multimap *m, multimapIterator *iter,
                            const bool forward, const databox *box) {
    MULTIMAP_RETURN(m, IteratorInitAt, iter, forward, box);
}

void multimapIteratorInit(const multimap *m, multimapIterator *iter,
                          const bool forward) {
    MULTIMAP_NORETURN(m, IteratorInit, iter, forward);
}

bool multimapIteratorNext(multimapIterator *iter, databox **elements) {
    const void **jumper[4] = {NULL, &&iterSmall, &&iterMedium, &&iterFull};
    goto *jumper[iter->type];

iterSmall:
    return multimapSmallIteratorNext(iter, elements);

iterMedium:
    return multimapMediumIteratorNext(iter, elements);

iterFull:
    return multimapFullIteratorNext(iter, elements);
}

size_t multimapProcessUntil(multimap *m, const multimapPredicate *p,
                            bool forward, multimapElementWalker *walker,
                            void *userData) {
    if (multimapCount(m) == 0) {
        return 0;
    }

    /* Populate iterator metadata based on map type */
    multimapIterator iter = {0};
    MULTIMAP_NORETURN(m, IteratorInit, &iter, forward);

    /* We materialize 16 physical databoxes for the most common cases
     * so we won't have to malloc() during each iteration.
     * If we have more than 16 elements, then just malloc() and free()
     * as usual. */
    const bool useHeapElements = iter.elementsPerEntry > 16 ? true : false;

    databox e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15,
        e16;
    databox *ee16[16] = {&e1, &e2,  &e3,  &e4,  &e5,  &e6,  &e7,  &e8,
                         &e9, &e10, &e11, &e12, &e13, &e14, &e15, &e16};

    databox **elements = ee16;

    databox *underlyingElements = NULL;
    if (useHeapElements) {
        underlyingElements =
            zcalloc(iter.elementsPerEntry, sizeof(*underlyingElements));
        elements = &underlyingElements;
    }

    size_t processed = 0;
    switch (multimapType_(m)) {
    case MULTIMAP_TYPE_SMALL:
        while (multimapSmallIteratorNext(&iter, elements) &&
               multimapProcessPredicate(p, elements[0])) {
            processed++;
            if (!walker(userData, (const databox **)elements)) {
                break;
            }
        }
        break;
    case MULTIMAP_TYPE_MEDIUM:
        while (multimapMediumIteratorNext(&iter, elements) &&
               multimapProcessPredicate(p, elements[0])) {
            processed++;
            if (!walker(userData, (const databox **)elements)) {
                break;
            }
        }
        break;
    case MULTIMAP_TYPE_FULL:
        while (multimapFullIteratorNext(&iter, elements) &&
               multimapProcessPredicate(p, elements[0])) {
            processed++;
            if (!walker(userData, (const databox **)elements)) {
                break;
            }
        }
        break;
    default:
        assert(NULL);
        __builtin_unreachable();
    }

    if (useHeapElements) {
        zfree(underlyingElements);
    }

    return processed;
}

/* Use element[0] of each sorted multimap as the itersection key. */
void multimapIntersectKeys(multimap **restrict dst,
                           multimapIterator *restrict const a,
                           multimapIterator *restrict const b) {
    /* TODO: remove the VLAs here */
    databox ea_[a->elementsPerEntry];
    databox eb_[b->elementsPerEntry];
    databox *ea[a->elementsPerEntry];
    databox *eb[b->elementsPerEntry];

    if (a->elementsPerEntry == b->elementsPerEntry) {
        /* If maps have the same element count, assign both at once */
        for (size_t i = 0; i < a->elementsPerEntry; i++) {
            ea[i] = &ea_[i];
            eb[i] = &eb_[i];
        }
    } else {
        /* else, different element accounts so we need two loops */
        for (size_t i = 0; i < a->elementsPerEntry; i++) {
            ea[i] = &ea_[i];
        }

        /* We could combine these if we check element counts are the same... */
        for (size_t i = 0; i < b->elementsPerEntry; i++) {
            eb[i] = &eb_[i];
        }
    }

    bool foundA = multimapIteratorNext(a, ea);
    bool foundB = multimapIteratorNext(b, eb);

    /* element-by-element zipper algoirthm for intersecting two sorted lists. */
    while (foundA && foundB) {
        const int compared = databoxCompare(ea[0], eb[0]);
        if (compared < 0) {
            foundA = multimapIteratorNext(a, ea);
        } else if (compared > 0) {
            foundB = multimapIteratorNext(b, eb);
        } else {
            /* Keys compare equal, so add key to result map */
            multimapInsert(dst, (const databox **)&ea[0]);
            foundA = multimapIteratorNext(a, ea);
            foundB = multimapIteratorNext(b, eb);
        }
    }
}

void multimapDifferenceKeys(multimap **restrict dst,
                            multimapIterator *restrict const a,
                            multimapIterator *restrict const b,
                            const bool symmetricDifference) {
#if 0
    databox ea_[a->elementsPerEntry];
    databox eb_[b->elementsPerEntry];
    databox *ea[a->elementsPerEntry];
    databox *eb[b->elementsPerEntry];
#else
    assert(a->elementsPerEntry == 1);
    assert(a->elementsPerEntry == b->elementsPerEntry);
    databox ea_;
    databox eb_;
    databox *ea[1] = {&ea_};
    databox *eb[1] = {&eb_};
#endif

    bool foundA = multimapIteratorNext(a, ea);
    bool foundB = multimapIteratorNext(b, eb);

    while (foundA && foundB) {
        const int compared = databoxCompare(ea[0], eb[0]);
        if (compared) {
            /* elements not equal, meaning they are different!
             * Only add the first map element to the result because of how later
             * usage expects just [A] - [B], not the complete symmetric set
             * difference of A and B. */
            multimapInsert(dst, (const databox **)ea);

            if (compared < 0) {
                foundA = multimapIteratorNext(a, ea);
            } else {
                foundB = multimapIteratorNext(b, eb);
            }
        } else {
            /* else, elements are equal so they definitely aren't different. */
            foundA = multimapIteratorNext(a, ea);
            foundB = multimapIteratorNext(b, eb);
        }
    }

    /* The above loop terminates at the shortest list, so we need to
     * append elements from the longer map not possible to be in the
     * shorter map because we already ran out of equality depth comparisons.
     */
    while (foundA) {
        multimapInsert(dst, (const databox **)&ea[0]);
        foundA = multimapIteratorNext(a, ea);
    }

    if (symmetricDifference) {
        while (foundB) {
            multimapInsert(dst, (const databox **)&eb[0]);
            foundB = multimapIteratorNext(b, eb);
        }
    }
}

/* This is basically a 'union' function.
 * Just loop all your input keys into 'dst' and they'll get added if they
 * don't exist, and if they do exist, nothing changes. */
void multimapCopyKeys(multimap **restrict dst, const multimap *restrict src) {
    multimapIterator msrc;
    multimapIteratorInit(src, &msrc, true);

    databox *bsrc[msrc.elementsPerEntry];

    while (multimapIteratorNext(&msrc, bsrc)) {
        multimapInsert(dst, (const databox **)&bsrc[0]);
    }
}

/* ====================================================================
 * Testing
 * ==================================================================== */
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

#define multimapVerify(m)

#define multimapReport(m) multimapReport_(m, true)
#define multimapReportSizeOnly(m) multimapReport_(m, false)

void multimapRepr(const multimap *m) {
    const multimapType whatItIs = multimapType_(m);
    printf("Type: %s\n", whatItIs == MULTIMAP_TYPE_SMALL    ? "SMALL"
                         : whatItIs == MULTIMAP_TYPE_MEDIUM ? "MEDIUM"
                                                            : "FULL");
    MULTIMAP_SINGLE_NORETURN(m, Repr);
}

static size_t multimapReport_(const multimap *m, const bool print) {
    size_t Bytes = multimapBytes(m);
    size_t values = multimapCount(m);
    int count = 0;
    size_t rangeBoxBytes = 0;
    size_t middleBytes = 0;
    size_t mapPtrBytes = 0;
    size_t containerBytes = 0;
    char *type = "INVALID";

    switch (multimapType_(m)) {
    case MULTIMAP_TYPE_SMALL:
        count = 1;
        containerBytes = sizeof(multimapSmall);
        mapPtrBytes = 0; /* included in container */
        middleBytes = 0; /* included in container */
        type = "S";
        break;
    case MULTIMAP_TYPE_MEDIUM:
        count = 2;
        containerBytes = sizeof(multimapMedium);
        mapPtrBytes = 0; /* included in container */
        middleBytes = 0; /* included in container */
        type = "M";
        break;
    case MULTIMAP_TYPE_FULL:
        count = mmf(m)->count;
        rangeBoxBytes = sizeof(databox) * (count - 1);
        middleBytes = sizeof(multimapFullMiddle) * count;
        mapPtrBytes = sizeof(flex *) * count;
        containerBytes = sizeof(*mmf(m));
        type = "L";
        break;
    default:
        assert(NULL);
        __builtin_unreachable();
    }

    size_t externalMetadataBytes =
        rangeBoxBytes + middleBytes + mapPtrBytes + containerBytes;
    size_t totalBytes = Bytes + externalMetadataBytes;
    double externalMetadataOverhead =
        (double)(externalMetadataBytes) / (totalBytes);

    if (print) {
        printf("[%s] {bytes {total %zu} {data %zu}} {maps %d} {per map {%0.2f "
               "elements} {%0.2f bytes}}\n"
               "{overhead %0.2f%% {bytes %zu {%zu pointer} {%zu rangebox} "
               "{%zu middle} {%zu struct}}\n\n",
               type, totalBytes, Bytes, count,
               count ? (double)values / count : 0,
               count ? (double)Bytes / count : 0,
               externalMetadataOverhead * 100, externalMetadataBytes,
               mapPtrBytes, rangeBoxBytes, middleBytes, containerBytes);
    }

    fflush(stdout);

    return totalBytes;
}

int multimapTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;

    TEST("small: create...") {
        multimap *m = multimapNewLimit(2, FLEX_CAP_LEVEL_2048);
        multimapReport(m);
        multimapFree(m);
    }

    TEST("small: insert / lookup / exists / delete / type check...") {
        multimap *m = multimapNewLimit(2, FLEX_CAP_LEVEL_2048);
        assert(multimapType_(m) == MULTIMAP_TYPE_SMALL);

        databox key = databoxNewSigned(3);
        databox val = databoxNewSigned(4);
        const databox *elements[2] = {&key, &val};

        bool exists = multimapExists(m, &key);
        assert(!exists);

        multimapInsert(&m, elements);
        exists = multimapExists(m, &key);
        assert(exists);

        databox value = {{0}};
        databox *values[1] = {&value};
        bool found = multimapLookup(m, &key, values);
        assert(found);
        assert(databoxEqual(&val, &value));

        multimapDelete(&m, &key);
        exists = multimapExists(m, &key);
        assert(!exists);

        assert(multimapType_(m) == MULTIMAP_TYPE_SMALL);
        multimapReport(m);
        multimapFree(m);
    }

#if 1
    TEST("speeds at different sizes") {
        uint64_t secs[20] = {0};
        size_t size[20] = {0};
        int i = 0;
        for (size_t maxIdx = 0; maxIdx < flexOptimizationSizeLimits; maxIdx++) {
            flexCapSizeLimit maxSize = flexOptimizationSizeLimit[maxIdx];
            multimap *m = multimapNewLimit(2, maxIdx);
            int32_t pairs = 1 << 17;
            TEST_DESC("%d bytes max with %d k/v pairs...", maxSize, pairs) {
                TIME_INIT;
                for (int32_t j = 0; j < pairs; j++) {
                    char *key = genkey("key", j);
                    char *val = genval("val", j * 100);
                    const databox keybox = databoxNewBytesString(key);
                    const databox valbox = databoxNewBytesString(val);
                    const databox *elements[2] = {&keybox, &valbox};
                    multimapInsert(&m, elements);
                }

                TIME_FINISH(pairs, "insert");
                secs[i] = lps.global.us.duration;
                size[i] = multimapReportSizeOnly(m);
                (void)secs;
                (void)size;
                i++;
            }

            multimapReport(m);
            multimapVerify(m);
            multimapFree(m);
        }
    }

    printf("\n\n");
#endif

    TEST("(full width) small->medium->full: insert / exists / lookup / delete "
         "/ type "
         "check...") {
        for (int32_t i = 0; i < 4096; i++) {
            multimap *m = multimapNewLimit(2, FLEX_CAP_LEVEL_2048);
            TEST_DESC("%d k/v pairs - inserting...", i) {
                TIME_INIT;
                for (int32_t j = 0; j < i; j++) {
                    char *key = genkey("key", 0); /* same key for everything */
                    char *val = genval("val", j * 100);
                    const databox keybox = databoxNewBytesString(key);
                    const databox valbox = databoxNewBytesString(val);
                    const databox *elements[2] = {&keybox, &valbox};
                    multimapInsertFullWidth(&m, elements);
                }

                TIME_FINISH(i, "insert");
            }
            assert(multimapCount(m) == (uint32_t)i);

            multimapReport(m);
            multimapVerify(m);

            TEST_DESC("%d k/v pairs - checking members (sequential)...", i) {
                TIME_INIT;
                for (int32_t j = 0; j < i; j++) {
                    char *key = genkey("key", 0);
                    char *val = genval("val", j * 100);
                    const databox keybox = databoxNewBytesString(key);
                    const databox valbox = databoxNewBytesString(val);
                    const databox *elements[2] = {&keybox, &valbox};
                    bool found = multimapExistsFullWidth(m, elements);
                    if (!found) {
                        ERR("Didn't find [%s, %s] at iteration (%d, %d)!", key,
                            val, i, j);
                        assert(NULL);
                    }
                }
                TIME_FINISH(i, "exists (sequential)");
            }
            assert(multimapCount(m) == (uint32_t)i);

            printf("\n");
            TEST_DESC("%d k/v pairs - deleting...", i) {
                bool delLowToHigh = i % 2 == 0;
                multimapFullIdx start = delLowToHigh ? 0 : i - 1;
                int32_t incrBy = delLowToHigh ? 1 : -1;
                TIME_INIT;
                for (int32_t j = start; delLowToHigh ? j < i : j >= 0;
                     j += incrBy) {
                    char *key = genkey("key", 0);
                    char *val = genval("val", j * 100);
                    const databox keybox = databoxNewBytesString(key);
                    const databox valbox = databoxNewBytesString(val);
                    const databox *elements[2] = {&keybox, &valbox};
                    bool deleted = multimapDeleteFullWidth(&m, elements);
                    if (!deleted) {
                        ERR("Didn't find %s at iteration (%d, %d)!", key, i, j);
                        multimapReport(m);
                        multimapVerify(m);
                        assert(NULL);
                    }
                }
                TIME_FINISH(i, "delete");
            }
            assert(multimapCount(m) == 0);
            multimapReport(m);
            multimapVerify(m);

            TEST_DESC("%d k/v pairs - inserting again after full delete...",
                      i) {
                TIME_INIT;
                for (int32_t j = 0; j < i; j++) {
                    char *key = genkey("key", 0);
                    char *val = genval("val", j * 100);
                    const databox keybox = databoxNewBytesString(key);
                    const databox valbox = databoxNewBytesString(val);
                    const databox *elements[2] = {&keybox, &valbox};
                    multimapInsertFullWidth(&m, elements);
                }

                TIME_FINISH(i, "insert");
                assert(multimapCount(m) == (uint32_t)i);
            }

            multimapReport(m);
            multimapVerify(m);

            multimapFree(m);
            printf("\n");
        }
    }

    TEST("(key only) small->medium->full: insert / exists / lookup / delete / "
         "type "
         "check...") {
        for (int32_t i = 0; i < 4096; i++) {
            multimap *m = multimapNewLimit(2, FLEX_CAP_LEVEL_2048);
            TEST_DESC("%d k/v pairs - inserting...", i) {
                TIME_INIT;
                for (int32_t j = 0; j < i; j++) {
                    char *key = genkey("key", j);
                    char *val = genval("val", j * 100);
                    const databox keybox = databoxNewBytesString(key);
                    const databox valbox = databoxNewBytesString(val);
                    const databox *elements[2] = {&keybox, &valbox};
                    multimapInsert(&m, elements);
                }

                TIME_FINISH(i, "insert");
            }
            assert(multimapCount(m) == (uint32_t)i);

            multimapReport(m);
            multimapVerify(m);

            TEST_DESC("%d k/v pairs - checking members (sequential)...", i) {
                TIME_INIT;
                for (int32_t j = 0; j < i; j++) {
                    char *key = genkey("key", j);
                    const databox keybox = databoxNewBytesString(key);
                    bool found = multimapExists(m, &keybox);
                    if (!found) {
                        ERR("Didn't find %s at iteration (%d, %d)!", key, i, j);
                        assert(NULL);
                    }
                }
                TIME_FINISH(i, "exists (sequential)");
            }
            assert(multimapCount(m) == (uint32_t)i);

            TEST_DESC("%d k/v pairs - checking lookup (sequential)...", i) {
                TIME_INIT;
                for (int32_t j = 0; j < i; j++) {
                    char *key = genkey("key", j);
                    char *val = genval("val", j * 100);
                    const databox keybox = databoxNewBytesString(key);
                    const databox valbox = databoxNewBytesString(val);
                    databox value = {{0}};
                    databox *values[1] = {&value};
                    bool found = multimapLookup(m, &keybox, values);

                    if (!found) {
                        ERR("Didn't find %s at iteration (%d, %d)!", key, i, j);
                        assert(NULL);
                    }

                    if (!databoxEqual(&valbox, &value)) {
                        ERR("Didn't find value!  Expected %s but got %s!",
                            valbox.data.bytes.start, value.data.bytes.start);
                    }
                }
                TIME_FINISH(i, "lookup (sequential)");
            }
            assert(multimapCount(m) == (uint32_t)i);

            TEST_DESC("%d k/v pairs - checking lookup (random)...", i) {
                TIME_INIT;
                for (int32_t _j = 0; _j < i; _j++) {
                    int32_t j = rand() % i;
                    char *key = genkey("key", j);
                    char *val = genval("val", j * 100);
                    const databox keybox = databoxNewBytesString(key);
                    const databox valbox = databoxNewBytesString(val);
                    databox value = {{0}};
                    databox *values[1] = {&value};
                    bool found = multimapLookup(m, &keybox, values);

                    if (!found) {
                        ERR("Didn't find %s at iteration (%d, %d)!", key, i, j);
                        assert(NULL);
                    }
                    if (!databoxEqual(&valbox, &value)) {
                        ERR("Didn't find value!  Expected %s but got %s!",
                            valbox.data.bytes.start, value.data.bytes.start);
                    }
                }
                TIME_FINISH(i, "lookup (random)");
            }
            assert(multimapCount(m) == (uint32_t)i);

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
                    bool deleted = multimapDelete(&m, &keybox);
                    if (!deleted) {
                        ERR("Didn't find %s at iteration (%d, %d)!", key, i, j);
                        multimapReport(m);
                        multimapVerify(m);
                        assert(NULL);
                    }
                }
                TIME_FINISH(i, "delete");
            }
            assert(multimapCount(m) == 0);
            multimapReport(m);
            multimapVerify(m);

#if 0
            /* We don't check i == 0 because 0 has no elements, so we can't delete
             * the flex by finding elements to delete, so... just skip it. */
            if (i > 0) {
                if (count > 1) {
                    ERR("After full delete, more than one map still exists!  We "
                            "have %u maps!\n",
                            count);
                }

                if (flexCount(m->map[0]) > 0) {
                    ERR("After full delete, map[0] has %zu elements!\n",
                            flexCount(m->map[0]));
                }
            }
            multimapReport(m);
            multimapVerify(m);
#endif

            TEST_DESC("%d k/v pairs - inserting again after full delete...",
                      i) {
                TIME_INIT;
                for (int32_t j = 0; j < i; j++) {
                    char *key = genkey("key", j);
                    char *val = genval("val", j * 100);
                    const databox keybox = databoxNewBytesString(key);
                    const databox valbox = databoxNewBytesString(val);
                    const databox *elements[2] = {&keybox, &valbox};
                    multimapInsert(&m, elements);
                }

                TIME_FINISH(i, "insert");
                assert(multimapCount(m) == (uint32_t)i);
            }

            multimapReport(m);
            multimapVerify(m);

            multimapFree(m);
            printf("\n");
        }
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
