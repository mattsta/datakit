#include "multimap.h"

#include "multimapSmall.h"
#include "multimapSmallInternal.h"

#include "multimapMedium.h"
#include "multimapMediumInternal.h"

#include "multimapFull.h"
#include "multimapFullInternal.h"

#include "cleaner.h"
#include "timeUtil.h"

defcleanerName(databox *, zfree, Databoxes);
defcleanerName(databox **, zfree, DataboxPtrs);
defcleanerName(int64_t *, zfree, Int64);
defcleanerName(int32_t *, zfree, Int32);
defcleanerName(uint8_t *, zfree, Bytes);
defcleanerName(char *, zfree, Chars);
defcleanerName(char **, zfree, CharPtrs);
defcleaner(multimap *, multimapFree);
defcleaner(multimapSmall *, multimapSmallFree);

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

#define multimapType_(map) ((multimapType)_PTRLIB_TYPE(map))
#define MULTIMAP_USE_(map) _PTRLIB_TOP_USE_ALL(map)
#define MULTIMAP_TAG_(map, type) ((multimap *)_PTRLIB_TAG(map, type))

/* Stack allocation threshold for iterator element arrays to avoid VLA blowup */
#define MULTIMAP_STACK_THRESHOLD 8

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
    /* @own: caller; free=multimapFree */
    multimapSmall *created = multimapSmallNew(elementsPerEntry, false);
    created = SET_COMPRESS_DEPTH_LIMIT(created, false, FLEX_CAP_LEVEL_2048);
    return MULTIMAP_TAG_(created, MULTIMAP_TYPE_SMALL);
}

multimap *multimapNewLimit(multimapElements elementsPerEntry,
                           const flexCapSizeLimit limit) {
    /* @own: caller; free=multimapFree */
    multimapSmall *created = multimapSmallNew(elementsPerEntry, false);
    created = SET_COMPRESS_DEPTH_LIMIT(created, false, limit);
    return MULTIMAP_TAG_(created, MULTIMAP_TYPE_SMALL);
}

multimap *multimapNewCompress(multimapElements elementsPerEntry,
                              const flexCapSizeLimit limit) {
    /* @own: caller; free=multimapFree */
    multimap *created = (multimap *)multimapSmallNew(elementsPerEntry, false);
    created = SET_COMPRESS_DEPTH_LIMIT(created, true, limit);
    return MULTIMAP_TAG_(created, MULTIMAP_TYPE_SMALL);
}

multimap *multimapSetNew(multimapElements elementsPerEntry) {
    /* @own: caller; free=multimapFree */
    multimap *created = (multimap *)multimapSmallNew(elementsPerEntry, true);
    created = SET_COMPRESS_DEPTH_LIMIT(created, false, FLEX_CAP_LEVEL_2048);
    return MULTIMAP_TAG_(created, MULTIMAP_TYPE_SMALL);
}

multimap *multimapNewConfigure(multimapElements elementsPerEntry,
                               const bool isSet, const bool compress,
                               const flexCapSizeLimit sizeLimit) {
    /* @own: caller; free=multimapFree */
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
        /* @own: caller; free=multimapFree */
        copy = (multimap *)multimapSmallCopy(mms(m));
    } else if (type == MULTIMAP_TYPE_MEDIUM) {
        /* @own: caller; free=multimapFree */
        copy = (multimap *)multimapMediumCopy(mmm(m));
    } else {
        /* @own: caller; free=multimapFree */
        copy = (multimap *)multimapFullCopy(mmf(m));
    }

    copy = SET_COMPRESS_DEPTH_LIMIT(copy, depth, limit);
    copy = MULTIMAP_TAG_(copy, type);
    return copy;
}

bool multimapInsert(multimap **m, const databox *elements[]) {
    return multimapInsertReplacing(m, elements, NULL);
}

/* As multimapInsert, and on a replace also reports the overwritten value
 * entries through 'replacedOut' (NULL, or elementsPerEntry - 1 slots; VOID
 * when nothing was replaced).  The tier that holds the key fills the report
 * before any upgrade; inline integer and pointer payloads survive the
 * upgrade, BYTES payloads do not. */
bool multimapInsertReplacing(multimap **m, const databox *elements[],
                             databox *replacedOut) {
    /* Insert new elements */
    bool replaced;
    MULTIMAP_SETRESULT(replaced, *m, InsertReplacing, elements, replacedOut);

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
    MULTIMAP_NORETURN(*m, Append, elements);

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

static multimapElements multimapElementsPerEntry_(const multimap *m) {
    switch (multimapType_(m)) {
    case MULTIMAP_TYPE_SMALL:
        return mms(m)->elementsPerEntry;
    case MULTIMAP_TYPE_MEDIUM:
        return mmm(m)->elementsPerEntry;
    case MULTIMAP_TYPE_FULL:
        return mmf(m)->elementsPerEntry;
    default:
        assert(NULL);
        __builtin_unreachable();
    }
}

/* Resize the FIRST VALUE field of the row with 'key' to 'newSize' bytes
 * (rows wider than key+value: use multimapGetUnderlyingEntry +
 * multimapResizeEntry to target other fields).  The key must exist
 * (asserted).  Contents of the resized field are unspecified after the
 * resize; repopulate through a fresh lookup. */
void multimapEntryResize(multimap **m, const databox *key, size_t newSize) {
    multimapEntry me;
    const bool found = multimapGetUnderlyingEntry(*m, key, &me);
    assert(found);
    (void)found;

    /* advance from the key entry to its first value field */
    me.fe = flexNext(*me.map, me.fe);
    multimapResizeEntry(m, &me, newSize);
}

/* Replace the VALUE fields of the row with 'key': elements[] holds
 * elementsPerEntry - 1 replacement boxes, applied in field order.  The
 * key stays in place (replacing a key would break sort order); the key
 * must exist (asserted).  Each field replacement may reallocate the
 * underlying storage, so the row is re-found per field. */
void multimapEntryReplace(multimap **m, const databox *key,
                          const databox *elements[]) {
    const multimapElements epe = multimapElementsPerEntry_(*m);
    for (multimapElements i = 0; i + 1 < epe; i++) {
        multimapEntry me;
        const bool found = multimapGetUnderlyingEntry(*m, key, &me);
        assert(found);
        (void)found;

        for (multimapElements skip = 0; skip <= i; skip++) {
            me.fe = flexNext(*me.map, me.fe);
        }

        multimapReplaceEntry(m, &me, elements[i]);
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
    if (!multimapGetUnderlyingEntry(*m, key, &me)) {
        /* No such key: a miss leaves me.fe NULL, so return 0 rather
         * than dereferencing it. Reachable via a stale or forged atom
         * reference handed to Retain/Release. */
        return 0;
    }

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
/* Populate 'elements' (elementsPerEntry boxes, FULL width including the
 * key) with the row at 'keyIndex' (0-based from the head; negative from
 * the tail, -1 = last row).  Returns false when the position is out of
 * range.  Each tier locates the row through its maintained anchors:
 * sub-map row counts route to the owning flex, then the walk starts
 * from the nearest of that flex's head, cached middle, or tail. */
bool multimapLookupByPosition(multimap *m, int64_t keyIndex,
                              databox *elements[]) {
    MULTIMAP_RETURN(m, LookupByPosition, keyIndex, elements);
}

/* Delete the row at 'keyIndex' (position semantics as above).  Returns
 * false when the position is out of range.  Deletes the EXACT addressed
 * row (safe for duplicate keys and surrogate-keyed maps). */
bool multimapDeleteByPosition(multimap **m, int64_t keyIndex) {
    MULTIMAP_RETURN(*m, DeleteByPosition, keyIndex);
}

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

    cleanerName(zfree, Databoxes) databox *underlyingElements = NULL;
    cleanerName(zfree, DataboxPtrs) databox **heapElementPtrs = NULL;
    if (useHeapElements) {
        /* The iterator populates through elements[0..N-1], so the heap
         * path needs BOTH an array of N boxes AND an array of N
         * pointers into it. */
        underlyingElements =
            zcalloc(iter.elementsPerEntry, sizeof(*underlyingElements));
        heapElementPtrs =
            zcalloc(iter.elementsPerEntry, sizeof(*heapElementPtrs));
        for (multimapElements i = 0; i < iter.elementsPerEntry; i++) {
            heapElementPtrs[i] = &underlyingElements[i];
        }

        elements = heapElementPtrs;
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

    return processed;
}

/* Use element[0] of each sorted multimap as the itersection key. */
void multimapIntersectKeys(multimap **restrict dst,
                           multimapIterator *restrict const a,
                           multimapIterator *restrict const b) {
    const size_t epeA = a->elementsPerEntry;
    const size_t epeB = b->elementsPerEntry;

    /* Use stack for small element counts, heap for larger to avoid overflow */
    databox eaStack_[MULTIMAP_STACK_THRESHOLD];
    databox ebStack_[MULTIMAP_STACK_THRESHOLD];
    databox *eaStack[MULTIMAP_STACK_THRESHOLD];
    databox *ebStack[MULTIMAP_STACK_THRESHOLD];

    cleanerName(zfree, Databoxes) databox *eaHeap_ = NULL;
    cleanerName(zfree, Databoxes) databox *ebHeap_ = NULL;
    cleanerName(zfree, DataboxPtrs) databox **eaHeap = NULL;
    cleanerName(zfree, DataboxPtrs) databox **ebHeap = NULL;

    databox *ea_, *eb_;
    databox **ea, **eb;

    if (epeA <= MULTIMAP_STACK_THRESHOLD) {
        ea_ = eaStack_;
        ea = eaStack;
    } else {
        eaHeap_ = zmalloc(epeA * sizeof(databox));
        eaHeap = zmalloc(epeA * sizeof(databox *));
        ea_ = eaHeap_;
        ea = eaHeap;
    }

    if (epeB <= MULTIMAP_STACK_THRESHOLD) {
        eb_ = ebStack_;
        eb = ebStack;
    } else {
        ebHeap_ = zmalloc(epeB * sizeof(databox));
        ebHeap = zmalloc(epeB * sizeof(databox *));
        eb_ = ebHeap_;
        eb = ebHeap;
    }

    if (epeA == epeB) {
        /* If maps have the same element count, assign both at once */
        for (size_t i = 0; i < epeA; i++) {
            ea[i] = &ea_[i];
            eb[i] = &eb_[i];
        }
    } else {
        /* else, different element accounts so we need two loops */
        for (size_t i = 0; i < epeA; i++) {
            ea[i] = &ea_[i];
        }

        for (size_t i = 0; i < epeB; i++) {
            eb[i] = &eb_[i];
        }
    }

    bool foundA = multimapIteratorNext(a, ea);
    bool foundB = multimapIteratorNext(b, eb);

    /* element-by-element zipper algorithm for intersecting two sorted lists. */
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
        if (compared < 0) {
            /* ea < eb: element in A is smaller than current B element.
             * Since B is sorted and we haven't found ea yet, ea is NOT in B.
             * Add it to the difference and advance A. */
            multimapInsert(dst, (const databox **)ea);
            foundA = multimapIteratorNext(a, ea);
        } else if (compared > 0) {
            /* ea > eb: element in B is smaller than current A element.
             * Just advance B to catch up - ea might still be in B. */
            foundB = multimapIteratorNext(b, eb);
        } else {
            /* ea == eb: element exists in both A and B.
             * Don't add to difference, advance both. */
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

    const size_t epe = msrc.elementsPerEntry;

    /* Use stack for small element counts, heap for larger to avoid overflow */
    databox bsrcStack_[MULTIMAP_STACK_THRESHOLD];
    databox *bsrcStack[MULTIMAP_STACK_THRESHOLD];
    cleanerName(zfree, Databoxes) databox *bsrcHeap_ = NULL;
    cleanerName(zfree, DataboxPtrs) databox **bsrcHeap = NULL;

    databox *bsrc_;
    databox **bsrc;

    if (epe <= MULTIMAP_STACK_THRESHOLD) {
        bsrc_ = bsrcStack_;
        bsrc = bsrcStack;
    } else {
        bsrcHeap_ = zmalloc(epe * sizeof(databox));
        bsrcHeap = zmalloc(epe * sizeof(databox *));
        bsrc_ = bsrcHeap_;
        bsrc = bsrcHeap;
    }

    for (size_t i = 0; i < epe; i++) {
        bsrc[i] = &bsrc_[i];
    }

    while (multimapIteratorNext(&msrc, bsrc)) {
        multimapInsert(dst, (const databox **)&bsrc[0]);
    }
}

/* ====================================================================
 * Testing
 * ==================================================================== */
#ifdef DATAKIT_TEST

/* Assert internal invariants of any tier: every cached middle byte
 * offset equals a fresh flexMiddle() rescan, and (Full tier) every
 * rangeBox matches its map's live head key without dangling into a
 * stale allocation.  These caches are trusted blindly by sorted finds
 * and range routing, so tests call this after every mutation. */
void multimapVerify(const multimap *m) {
    MULTIMAP_SINGLE_NORETURN(m, Verify);
}

#define CTEST_INCLUDE_KVGEN
#include "ctest.h"

#define DOUBLE_NEWLINE 0
#include "perf.h"

#include "str.h" /* for xoroshiro128plus */

#include "mdsc.h" /* mdscfree for the replacing sweep's externalized keys */
#include <inttypes.h>

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

/* Walker for the ProcessUntil wide-row regression: counts rows and sums
 * every value field so a mispopulated element pointer shows up as a
 * wrong sum (or a sanitizer trap) rather than passing silently. */
typedef struct processUntilWideCheck {
    size_t rows;
    int64_t sum;
    multimapElements elementsPerEntry;
} processUntilWideCheck;

static bool processUntilWideWalker(void *userData, const databox *elements[]) {
    processUntilWideCheck *c = userData;
    c->rows++;
    for (multimapElements i = 1; i < c->elementsPerEntry; i++) {
        c->sum += elements[i]->data.i;
    }

    return true;
}

defcleanerName(flex *, flexFree, Sweep);
defcleanerName(multimapMedium *, multimapMediumFree, Sweep);
defcleanerName(multimapFull *, multimapFullFree, Sweep);

/* Replacing-insert sweep: every tuple is [key][counter][pointer]; the
 * pointers are synthetic tags and are never dereferenced. */
#define RP_WIDTH 3
#define RP_VALUES (RP_WIDTH - 1)
#define RP_PTR_TAG(i) ((void *)(uintptr_t)(0x10000u + (uint64_t)(i) * 16u))

/* The report of one replacing insert against what it must say. */
static int32_t rpJudge(const char *where, const uint64_t key,
                       const bool replaced, const databox out[RP_VALUES],
                       const bool expectReplaced, const uint64_t counter,
                       const uint64_t ptrIdx) {
    int err = 0;
    if (replaced != expectReplaced) {
        ERR("%s: key %" PRIu64 " replaced=%d, expected %d", where, key,
            (int)replaced, (int)expectReplaced);
    }

    if (!expectReplaced) {
        for (size_t i = 0; i < RP_VALUES; i++) {
            if (out[i].type != DATABOX_VOID) {
                ERR("%s: key %" PRIu64 " fresh insert reported slot %zu "
                    "type %d, expected VOID",
                    where, key, i, (int)out[i].type);
            }
        }
        return err;
    }

    if (out[0].type == DATABOX_VOID || out[0].data.u != counter) {
        ERR("%s: key %" PRIu64 " replaced counter reported %" PRIu64
            " (type %d), expected %" PRIu64,
            where, key, (uint64_t)out[0].data.u, (int)out[0].type, counter);
    }

    if (out[1].type == DATABOX_VOID ||
        out[1].data.u != (uintptr_t)RP_PTR_TAG(ptrIdx)) {
        ERR("%s: key %" PRIu64 " replaced pointer reported 0x%" PRIx64
            " (type %d), expected 0x%" PRIx64,
            where, key, (uint64_t)out[1].data.u, (int)out[1].type,
            (uint64_t)(uintptr_t)RP_PTR_TAG(ptrIdx));
    }

    return err;
}

/* One replacing insert of [key][counter][RP_PTR_TAG(ptrIdx)] through a
 * layer, judged against the expected report. */
typedef bool (*rpInsertFn)(void *m, const databox *elements[],
                           databox *replacedOut);

static bool rpFlex(void *m, const databox *elements[], databox *out) {
    flexEntry *middle = NULL;
    return flexInsertReplaceByTypeSortedWithMiddleMultiDirectReplacing(
        (flex **)m, RP_WIDTH, elements, &middle, true, out);
}

static bool rpSmall(void *m, const databox *elements[], databox *out) {
    return multimapSmallInsertReplacing(m, elements, out);
}

static bool rpMedium(void *m, const databox *elements[], databox *out) {
    return multimapMediumInsertReplacing(m, elements, out);
}

static bool rpFull(void *m, const databox *elements[], databox *out) {
    return multimapFullInsertReplacing(m, elements, out);
}

static bool rpGeneric(void *m, const databox *elements[], databox *out) {
    return multimapInsertReplacing((multimap **)m, elements, out);
}

static int32_t rpOne(const char *where, rpInsertFn fn, void *m,
                     const uint64_t key, const databox *counter,
                     const uint64_t ptrIdx, const bool expectReplaced,
                     const uint64_t expectCounter,
                     const uint64_t expectPtrIdx) {
    const databox k = databoxNewUnsigned(key);
    const databox p = databoxNewPtr(RP_PTR_TAG(ptrIdx));
    const databox *elements[RP_WIDTH] = {&k, counter, &p};
    databox out[RP_VALUES];
    /* Poison the report so a slot the call never wrote reads as an error. */
    for (size_t i = 0; i < RP_VALUES; i++) {
        out[i] = databoxNewUnsigned(UINT64_MAX);
    }

    const bool replaced = fn(m, elements, out);
    return rpJudge(where, key, replaced, out, expectReplaced, expectCounter,
                   expectPtrIdx);
}

/* Fresh inserts of keys [0, n) report VOID; replacing each then reports the
 * counter and pointer it held. */
static int32_t rpExercise(const char *where, rpInsertFn fn, void *m,
                          const uint64_t n) {
    int32_t err = 0;
    for (uint64_t i = 0; i < n; i++) {
        const databox c = databoxNewUnsigned(i);
        err += rpOne(where, fn, m, i, &c, i, false, 0, 0);
    }

    for (uint64_t i = 0; i < n; i++) {
        const databox c = databoxNewUnsigned(i + 1000);
        err += rpOne(where, fn, m, i, &c, i + 1000, true, i, i);
    }

    return err;
}

/* Insert [key][wideValue][tag], then replace it with an inline counter.  A
 * 96/128-bit integer does not fit a databox, so its slot reports only the
 * type with a NULL payload pointer (see flex.h); the pointer slot reports
 * normally, and the next replace of the key reports the counter. */
static int32_t rpWide(const char *where, rpInsertFn fn, void *m,
                      const uint64_t key, const databox *wideValue,
                      const databoxType expectType) {
    int32_t err = 0;
    err += rpOne(where, fn, m, key, wideValue, key, false, 0, 0);

    const databox k = databoxNewUnsigned(key);
    const databox c = databoxNewUnsigned(key + 1000);
    const databox tag = databoxNewPtr(RP_PTR_TAG(key + 1000));
    const databox *elements[RP_WIDTH] = {&k, &c, &tag};
    databox out[RP_VALUES];
    for (size_t i = 0; i < RP_VALUES; i++) {
        out[i] = databoxNewUnsigned(UINT64_MAX);
    }

    if (!fn(m, elements, out)) {
        ERR("%s: key %" PRIu64 " replace of a wide value was not a replace",
            where, key);
    }

    if (out[0].type != expectType || out[0].data.u128 != NULL) {
        ERR("%s: key %" PRIu64 " replaced wide value reported type %d "
            "payload %p, expected type %d payload NULL",
            where, key, (int)out[0].type, (void *)out[0].data.u128,
            (int)expectType);
    }

    if (out[1].type == DATABOX_VOID ||
        out[1].data.u != (uintptr_t)RP_PTR_TAG(key)) {
        ERR("%s: key %" PRIu64 " replaced pointer reported 0x%" PRIx64
            " (type %d), expected 0x%" PRIx64,
            where, key, (uint64_t)out[1].data.u, (int)out[1].type,
            (uint64_t)(uintptr_t)RP_PTR_TAG(key));
    }

    const databox again = databoxNewUnsigned(key + 2000);
    err += rpOne(where, fn, m, key, &again, key + 2000, true, key + 1000,
                 key + 1000);
    return err;
}

int32_t multimapInsertReplacingSweep(void) {
    int err = 0;

    TEST("replacing insert: flex layer reports the replaced tuple") {
        cleanerName(flexFree, Sweep) flex *f = flexNew();
        err += rpExercise("flex", rpFlex, &f, 64);
    }

    TEST("replacing insert: flex layer with a key that becomes a pointer") {
        cleanerName(flexFree, Sweep) flex *f = flexNew();
        flexEntry *middle = NULL;
        static const char longKey[] = "a-key-long-enough-to-externalize";
        const databox k = databoxNewBytesString(longKey);
        const databox c0 = databoxNewUnsigned(7);
        const databox tag0 = databoxNewPtr(RP_PTR_TAG(7));
        const databox *e0[RP_WIDTH] = {&k, &c0, &tag0};
        databox out[RP_VALUES];
        void *keyAlloc = NULL;
        bool replaced =
            flexInsertReplaceByTypeSortedWithMiddleMultiDirectLongKeysBecomePointersReplacing(
                &f, RP_WIDTH, e0, &middle, true, &keyAlloc, out);
        err += rpJudge("flex-long-key insert", 0, replaced, out, false, 0, 0);
        if (!keyAlloc) {
            ERRR("flex-long-key: the long key was not externalized");
        }

        const databox c1 = databoxNewUnsigned(8);
        const databox tag1 = databoxNewPtr(RP_PTR_TAG(8));
        const databox *e1[RP_WIDTH] = {&k, &c1, &tag1};
        void *keyAllocReplace = NULL;
        replaced =
            flexInsertReplaceByTypeSortedWithMiddleMultiDirectLongKeysBecomePointersReplacing(
                &f, RP_WIDTH, e1, &middle, true, &keyAllocReplace, out);
        err += rpJudge("flex-long-key replace", 0, replaced, out, true, 7, 7);
        if (keyAllocReplace) {
            ERRR("flex-long-key: a replace minted a second key allocation");
            mdscfree(keyAllocReplace);
        }

        if (keyAlloc) {
            mdscfree(keyAlloc);
        }
    }

    TEST("replacing insert: multimapSmall reports the replaced tuple") {
        cleaner(multimapSmallFree) multimapSmall *m =
            multimapSmallNew(RP_WIDTH, false);
        err += rpExercise("small", rpSmall, m, 32);
    }

    TEST("replacing insert: multimapMedium reports the replaced tuple") {
        cleanerName(multimapMediumFree, Sweep) multimapMedium *m =
            multimapMediumNew(RP_WIDTH);
        err += rpExercise("medium", rpMedium, m, 256);
    }

    TEST("replacing insert: multimapFull reports the replaced tuple") {
        cleanerName(multimapFullFree, Sweep) multimapFull *m =
            multimapFullNew(RP_WIDTH);
        err += rpExercise("full", rpFull, m, 4096);
    }

    TEST("replacing insert: multimapFull externalized (MDSC) key") {
        cleanerName(multimapFullFree, Sweep) multimapFull *m =
            multimapFullNew(RP_WIDTH);
        static const char longKey[] = "another-key-long-enough-to-externalize";
        const databox k = databoxNewBytesString(longKey);
        const databox c0 = databoxNewUnsigned(11);
        const databox tag0 = databoxNewPtr(RP_PTR_TAG(11));
        const databox *e0[RP_WIDTH] = {&k, &c0, &tag0};
        databox out[RP_VALUES];
        void *keyAlloc = NULL;
        bool replaced = multimapFullInsertAllowExternalizeKeysReplacing(
            m, e0, &keyAlloc, out);
        err += rpJudge("full-mdsc insert", 0, replaced, out, false, 0, 0);
        if (!keyAlloc) {
            ERRR("full-mdsc: the long key was not externalized");
        }

        const databox c1 = databoxNewUnsigned(12);
        const databox tag1 = databoxNewPtr(RP_PTR_TAG(12));
        const databox *e1[RP_WIDTH] = {&k, &c1, &tag1};
        void *keyAllocReplace = NULL;
        replaced = multimapFullInsertAllowExternalizeKeysReplacing(
            m, e1, &keyAllocReplace, out);
        err += rpJudge("full-mdsc replace", 0, replaced, out, true, 11, 11);
        if (keyAllocReplace) {
            ERRR("full-mdsc: a replace minted a second key allocation");
            mdscfree(keyAllocReplace);
        }

        if (keyAlloc) {
            mdscfree(keyAlloc);
        }
    }

    TEST("externalized inserts that split a multimapFull still externalize") {
        /* 35-byte keys with 100-byte values overflow the default sub-map
         * size every ~19 rows, so most of these inserts land on the
         * split / new-map path.  Every fresh long key must come back as an
         * allocation, whichever path stored it. */
        enum { splitRows = 200 };
        cleanerName(multimapFullFree, Sweep) multimapFull *m =
            multimapFullNew(2);
        void *allocs[splitRows] = {0};
        char val[100];
        memset(val, 'v', sizeof(val));
        uint64_t missed = 0;
        for (uint64_t i = 0; i < splitRows; i++) {
            char key[64];
            snprintf(key, sizeof(key),
                     "long-key-needs-externalizing-%06" PRIu64, i);
            const databox k = databoxNewBytesString(key);
            const databox v = databoxNewBytes(val, sizeof(val));
            const databox *e[2] = {&k, &v};
            if (multimapFullInsertAllowExternalizeKeys(m, e, &allocs[i])) {
                ERR("split-externalize: fresh key %" PRIu64 " was reported "
                    "as a replace",
                    i);
            }

            if (!allocs[i]) {
                missed++;
            }
        }

        if (missed) {
            ERR("split-externalize: %" PRIu64 " of %d fresh long keys were "
                "stored inline instead of externalized (%zu sub-maps)",
                missed, splitRows, multimapFullNodeCount(m));
        }

        if (multimapFullNodeCount(m) < 2) {
            ERR("split-externalize: expected several sub-maps, got %zu",
                multimapFullNodeCount(m));
        }

        if (multimapFullCount(m) != splitRows) {
            ERR("split-externalize: count %zu, expected %d",
                multimapFullCount(m), splitRows);
        }

        for (uint64_t i = 0; i < splitRows; i++) {
            char key[64];
            snprintf(key, sizeof(key),
                     "long-key-needs-externalizing-%06" PRIu64, i);
            const databox k = databoxNewBytesString(key);
            if (!multimapFullExists(m, &k)) {
                ERR("split-externalize: key %" PRIu64 " is not found", i);
            }
        }

        for (uint64_t i = 0; i < splitRows; i++) {
            if (allocs[i]) {
                mdscfree(allocs[i]);
            }
        }
    }

    TEST("a set inserted with externalized keys stores each member once") {
        /* Short members (never externalized) and long members (always
         * externalized), each inserted twice, in enough volume to split
         * sub-maps.  The second insert of a member is a no-op that reports
         * found, so count and the physical rows agree. */
        enum { setRows = 120 };
        cleanerName(multimapFullFree, Sweep) multimapFull *m =
            multimapFullSetNew(2, 2048);
        /* A wrongly minted second allocation stays referenced by the map,
         * so it is freed with the first ones, after the last lookup. */
        void *allocs[setRows] = {0};
        void *extraAllocs[setRows] = {0};
        char val[64];
        memset(val, 's', sizeof(val));
        for (uint32_t pass = 0; pass < 2; pass++) {
            for (uint64_t i = 0; i < setRows; i++) {
                char key[64];
                snprintf(key, sizeof(key), "a-long-set-member-key-%06" PRIu64,
                         i);
                const databox kLong = databoxNewBytesString(key);
                const databox kShort = databoxNewUnsigned(i);
                const databox v = databoxNewBytes(val, sizeof(val));
                const databox *e[2] = {(i & 1) ? &kLong : &kShort, &v};
                void *alloc = NULL;
                databox out[1] = {databoxNewUnsigned(UINT64_MAX)};
                const bool found =
                    multimapFullInsertAllowExternalizeKeysReplacing(
                        m, e, &alloc, out);
                if (found != (pass == 1)) {
                    ERR("set-externalize: pass %u member %" PRIu64 " found=%d",
                        pass, i, found);
                }

                if (out[0].type != DATABOX_VOID) {
                    ERR("set-externalize: pass %u member %" PRIu64
                        " reported type %d, expected VOID",
                        pass, i, (int)out[0].type);
                }

                if (pass == 0) {
                    allocs[i] = alloc;
                    if ((i & 1) && !alloc) {
                        ERR("set-externalize: long member %" PRIu64
                            " was not externalized",
                            i);
                    }
                } else if (alloc) {
                    ERR("set-externalize: re-inserting member %" PRIu64
                        " minted a second key allocation",
                        i);
                    extraAllocs[i] = alloc;
                }
            }
        }

        /* The same key with a different value is a different set member. */
        const databox k0 = databoxNewUnsigned(0);
        const databox other = databoxNewUnsigned(1);
        const databox *e0[2] = {&k0, &other};
        void *alloc0 = NULL;
        if (multimapFullInsertAllowExternalizeKeys(m, e0, &alloc0)) {
            ERRR("set-externalize: a new (key, value) pair reported found");
        }

        multimapIterator it;
        multimapFullIteratorInit(m, &it, true);
        databox a;
        databox b;
        databox *ab[2] = {&a, &b};
        size_t physical = 0;
        while (multimapFullIteratorNext(&it, ab)) {
            physical++;
        }

        if (multimapFullCount(m) != setRows + 1 || physical != setRows + 1) {
            ERR("set-externalize: count %zu, iterated rows %zu, expected %d",
                multimapFullCount(m), physical, setRows + 1);
        }

        for (uint64_t i = 0; i < setRows; i++) {
            if (allocs[i]) {
                mdscfree(allocs[i]);
            }

            if (extraAllocs[i]) {
                mdscfree(extraAllocs[i]);
            }
        }
    }

    TEST("replacing insert: a set reports VOID on a duplicate member") {
        cleaner(multimapFree) multimap *m = multimapSetNew(RP_WIDTH);
        const databox c = databoxNewUnsigned(5);
        err += rpOne("set first", rpGeneric, &m, 5, &c, 5, false, 0, 0);
        databox out[RP_VALUES];
        const databox k = databoxNewUnsigned(5);
        const databox p = databoxNewPtr(RP_PTR_TAG(5));
        const databox *e[RP_WIDTH] = {&k, &c, &p};
        const bool found = multimapInsertReplacing(&m, e, out);
        err += rpJudge("set duplicate", 5, false, out, false, 0, 0);
        if (!found) {
            ERRR("set duplicate: the member was not found");
        }
    }

    TEST("replacing insert: a replaced 96/128-bit value reports its type") {
        __uint128_t u128 = ((__uint128_t)UINT64_C(0x0123456789ABCDEF) << 64) |
                           UINT64_C(0x1122334455667788);
        __uint128_t u96 = (__uint128_t)1 << 80;
        __int128_t s128 = -(__int128_t)u128;
        __int128_t s96 = -(__int128_t)u96;
        const databox wide[] = {
            {.type = DATABOX_UNSIGNED_128, .big = true, .data.u128 = &u128},
            {.type = DATABOX_UNSIGNED_128, .big = true, .data.u128 = &u96},
            {.type = DATABOX_SIGNED_128, .big = true, .data.i128 = &s128},
            {.type = DATABOX_SIGNED_128, .big = true, .data.i128 = &s96},
        };

        cleanerName(flexFree, Sweep) flex *f = flexNew();
        cleaner(multimapSmallFree) multimapSmall *small =
            multimapSmallNew(RP_WIDTH, false);
        cleanerName(multimapMediumFree, Sweep) multimapMedium *medium =
            multimapMediumNew(RP_WIDTH);
        cleanerName(multimapFullFree, Sweep) multimapFull *full =
            multimapFullNew(RP_WIDTH);
        cleaner(multimapFree) multimap *generic = multimapNew(RP_WIDTH);
        for (uint64_t w = 0; w < COUNT_ARRAY(wide); w++) {
            const databoxType t = wide[w].type;
            err += rpWide("flex wide", rpFlex, &f, w, &wide[w], t);
            err += rpWide("small wide", rpSmall, small, w, &wide[w], t);
            err += rpWide("medium wide", rpMedium, medium, w, &wide[w], t);
            err += rpWide("full wide", rpFull, full, w, &wide[w], t);
            err += rpWide("generic wide", rpGeneric, &generic, w, &wide[w], t);
        }
    }

    TEST("replacing insert: a replace that upgrades Small->Medium->Full") {
        /* A replace that grows its value past the tier's byte limit is the
         * call that upgrades the tier: its report is taken by the tier that
         * held the key, before the upgrade moves the flex bytes. */
        const flexCapSizeLimit limit = FLEX_CAP_LEVEL_2048;
        const size_t smallCap = flexOptimizationSizeLimit[limit];
        const size_t mediumCap = smallCap * 3;
        cleaner(multimapFree) multimap *m = multimapNewLimit(RP_WIDTH, limit);
        cleanerName(zfree, Bytes) uint8_t *wide = zmalloc(mediumCap);
        memset(wide, 'w', mediumCap);

        uint64_t next = 0;
        while (multimapType_(m) == MULTIMAP_TYPE_SMALL &&
               multimapBytes(m) + 1024 < smallCap) {
            const databox c = databoxNewUnsigned(next);
            err += rpOne("upgrade fill small", rpGeneric, &m, next, &c, next,
                         false, 0, 0);
            next++;
        }

        if (multimapType_(m) != MULTIMAP_TYPE_SMALL || next < 3) {
            ERR("upgrade: fill left type %d after %" PRIu64 " keys, expected "
                "SMALL with at least three entries",
                (int)multimapType_(m), next);
        }

        const databox grow1 = databoxNewBytes(wide, smallCap);
        err += rpOne("upgrade small->medium", rpGeneric, &m, 0, &grow1, 5000,
                     true, 0, 0);
        if (multimapType_(m) != MULTIMAP_TYPE_MEDIUM) {
            ERR("upgrade: the growing replace left type %d, expected MEDIUM",
                (int)multimapType_(m));
        }

        while (multimapType_(m) == MULTIMAP_TYPE_MEDIUM &&
               multimapBytes(m) + 2048 < mediumCap) {
            const databox c = databoxNewUnsigned(next);
            err += rpOne("upgrade fill medium", rpGeneric, &m, next, &c, next,
                         false, 0, 0);
            next++;
        }

        const databox grow2 = databoxNewBytes(wide, mediumCap);
        err += rpOne("upgrade medium->full", rpGeneric, &m, 1, &grow2, 6000,
                     true, 1, 1);
        if (multimapType_(m) != MULTIMAP_TYPE_FULL) {
            ERR("upgrade: the growing replace left type %d, expected FULL",
                (int)multimapType_(m));
        }

        /* Every key still reports what it held, now from the Full tier. */
        for (uint64_t i = 2; i < next; i++) {
            const databox c = databoxNewUnsigned(i + 1000);
            err += rpOne("upgrade after full", rpGeneric, &m, i, &c, i + 1000,
                         true, i, i);
        }
    }

    return err;
}

/* RandomValue and LookupByPosition through one tier, for the per-tier
 * selection checks below. */
typedef bool (*rvDrawFn)(void *m, bool fromTail, databox **boxes,
                         multimapEntry *me);
typedef bool (*rvPositionFn)(void *m, int64_t row, databox **boxes);
typedef bool (*rvInsertFn)(void *m, const databox *elements[]);

static bool rvSmallDraw(void *m, bool fromTail, databox **boxes,
                        multimapEntry *me) {
    return multimapSmallRandomValue(m, fromTail, boxes, me);
}

static bool rvSmallPosition(void *m, int64_t row, databox **boxes) {
    return multimapSmallLookupByPosition(m, row, boxes);
}

static bool rvSmallInsert(void *m, const databox *elements[]) {
    return multimapSmallInsert(m, elements);
}

static bool rvMediumDraw(void *m, bool fromTail, databox **boxes,
                         multimapEntry *me) {
    return multimapMediumRandomValue(m, fromTail, boxes, me);
}

static bool rvMediumPosition(void *m, int64_t row, databox **boxes) {
    return multimapMediumLookupByPosition(m, row, boxes);
}

static bool rvMediumInsert(void *m, const databox *elements[]) {
    return multimapMediumInsert(m, elements);
}

static bool rvFullDraw(void *m, bool fromTail, databox **boxes,
                       multimapEntry *me) {
    return multimapFullRandomValue(m, fromTail, boxes, me);
}

static bool rvFullPosition(void *m, int64_t row, databox **boxes) {
    return multimapFullLookupByPosition(m, row, boxes);
}

static bool rvFullInsert(void *m, const databox *elements[]) {
    return multimapFullInsert(m, elements);
}

/* Fill keys [0, rows) with a 64-byte value (enough to split a Full
 * container into several sub-maps), then check that RandomValue draws rows
 * uniformly, that fromTail always returns the last row, and that drawing
 * leaves the process-wide random() stream alone.
 *
 * rows = 768 is 3 * 256, chosen so the common broken draws all show up: a
 * dead low bit leaves every odd row unreached, a draw capped below 'rows'
 * (say a 9-bit draw) leaves rows 512..767 unreached, and a modulo of a
 * 10-bit draw gives rows 0..255 double weight.
 *
 * Why this cannot flake: with 64 expected hits per row, the Pearson
 * statistic of a uniform draw is close to chi-square with 767 degrees of
 * freedom (mean 767, sd about 39).  The ceiling of 2 * 767 is about 19 sd
 * out; the Laurent-Massart bound puts the chance of a uniform draw passing
 * it below e^-100.  A row is never drawn with probability e^-64 per row.
 * The double-weight modulo above scores about 6900, a dead low bit about
 * 49000, so both fail by a wide margin.
 *
 * A bias of order n / 2^31 (what random() % n has) is below anything a
 * sample of this size can see.  What a test can pin is that RandomValue
 * draws from its own per-thread generator: a caller's seeded random()
 * sequence must come out the same with RandomValue calls in between.  The
 * last check pins that; a draw through random() fails it on the first
 * step. */
static int32_t rvSelectionCheck(const char *where, void *m, rvInsertFn insert,
                                rvDrawFn draw, rvPositionFn position) {
    int32_t err = 0;
    enum { rows = 768, drawsPerRow = 64, draws = rows * drawsPerRow };

    char payload[64];
    memset(payload, 'p', sizeof(payload));
    for (uint64_t i = 0; i < rows; i++) {
        const databox key = databoxNewUnsigned(i);
        const databox val = databoxNewBytes(payload, sizeof(payload));
        const databox *elements[2] = {&key, &val};
        insert(m, elements);
    }

    uint32_t hits[rows] = {0};
    for (uint64_t d = 0; d < draws; d++) {
        databox k = {{0}};
        databox v = {{0}};
        databox *boxes[2] = {&k, &v};
        multimapEntry me;
        if (!draw(m, false, boxes, &me)) {
            ERR("%s: a draw failed on a non-empty container", where);
            return err;
        }

        if (k.type != DATABOX_UNSIGNED_64 || k.data.u >= rows) {
            ERR("%s: a draw returned key %" PRIu64 " (type %d), outside "
                "[0, %d)",
                where, (uint64_t)k.data.u, (int)k.type, rows);
            return err;
        }

        hits[k.data.u]++;
    }

    uint32_t unreached = 0;
    double chiSquare = 0;
    for (uint32_t i = 0; i < rows; i++) {
        if (hits[i] == 0) {
            unreached++;
        }

        const double delta = (double)hits[i] - drawsPerRow;
        chiSquare += delta * delta / drawsPerRow;
    }

    if (unreached) {
        ERR("%s: %" PRIu32 " of %d rows were never drawn in %d draws", where,
            unreached, rows, draws);
    }

    const double chiSquareCeiling = 2.0 * (rows - 1);
    if (chiSquare > chiSquareCeiling) {
        ERR("%s: chi-square %.1f over %d rows exceeds %.1f; the draw is not "
            "uniform",
            where, chiSquare, rows, chiSquareCeiling);
    }

    /* fromTail: the same row on every call, the container's last. */
    databox lastKey = {{0}};
    databox lastVal = {{0}};
    databox *lastBoxes[2] = {&lastKey, &lastVal};
    if (!position(m, rows - 1, lastBoxes) || lastKey.data.u != rows - 1) {
        ERR("%s: could not read the last row by position", where);
    }

    for (uint32_t d = 0; d < drawsPerRow; d++) {
        databox k = {{0}};
        databox v = {{0}};
        databox *boxes[2] = {&k, &v};
        multimapEntry me;
        if (!draw(m, true, boxes, &me) || k.data.u != lastKey.data.u) {
            ERR("%s: fromTail returned key %" PRIu64 ", not the last row "
                "%" PRIu64,
                where, (uint64_t)k.data.u, (uint64_t)lastKey.data.u);
            break;
        }
    }

    /* A seeded random() sequence is the same with draws interleaved. */
    enum { streamLen = 16 };
    const uint32_t seed = 0x5eed;
    uint64_t expected[streamLen];
    srandom(seed);
    for (uint32_t i = 0; i < streamLen; i++) {
        expected[i] = (uint64_t)random();
    }

    srandom(seed);
    for (uint32_t i = 0; i < streamLen; i++) {
        databox k = {{0}};
        databox v = {{0}};
        databox *boxes[2] = {&k, &v};
        multimapEntry me;
        draw(m, false, boxes, &me);
        const uint64_t got = (uint64_t)random();
        if (got != expected[i]) {
            ERR("%s: RandomValue advanced the random() stream (step %" PRIu32
                ": %" PRIu64 " != %" PRIu64 ")",
                where, i, got, expected[i]);
            break;
        }
    }

    return err;
}

int multimapTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;

    TEST("replacing insert sweep (every layer)") {
        err += multimapInsertReplacingSweep();
    }

    TEST("small: create...") {
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_2048);
        multimapReport(m);
    }

    TEST("small: insert / lookup / exists / delete / type check...") {
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_2048);
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
        (void)found;
        assert(databoxEqual(&val, &value));

        multimapDelete(&m, &key);
        exists = multimapExists(m, &key);
        assert(!exists);
        (void)exists;

        assert(multimapType_(m) == MULTIMAP_TYPE_SMALL);
        multimapReport(m);
    }

    /* Regression test for mapIsSet inversion bug: inserting same key with
     * different value should replace, not create a duplicate entry. */
    TEST("key replacement regression (mapIsSet fix)...") {
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_2048);

        /* Insert key=100, val=200 */
        databox key1 = databoxNewSigned(100);
        databox val1 = databoxNewSigned(200);
        const databox *elements1[2] = {&key1, &val1};
        bool replaced = multimapInsert(&m, elements1);
        assert(!replaced); /* First insert is not a replacement */
        assert(multimapCount(m) == 1);

        /* Verify initial value */
        databox result = {{0}};
        databox *results[1] = {&result};
        bool found = multimapLookup(m, &key1, results);
        assert(found);
        (void)found;
        assert(databoxEqual(&val1, &result));

        /* Insert same key with different value - should replace */
        databox val2 = databoxNewSigned(999);
        const databox *elements2[2] = {&key1, &val2};
        replaced = multimapInsert(&m, elements2);
        assert(replaced); /* This MUST return true - key was replaced */
        assert(multimapCount(m) == 1); /* Size must stay at 1, not grow to 2 */

        /* Verify value was updated */
        result = (databox){{0}};
        found = multimapLookup(m, &key1, results);
        assert(found);
        assert(databoxEqual(&val2, &result)); /* Value must be 999, not 200 */

        /* Insert another different key - should NOT replace */
        databox key2 = databoxNewSigned(101);
        databox val3 = databoxNewSigned(300);
        const databox *elements3[2] = {&key2, &val3};
        replaced = multimapInsert(&m, elements3);
        assert(!replaced);
        (void)replaced;
        assert(multimapCount(m) == 2); /* Now we have 2 entries */
    }

    /* Test key replacement works across all multimap representations */
    TEST("key replacement across Small/Medium/Full representations...") {
        /* Use a small max size to force transitions between representations */
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_64);

        /* Insert many keys to transition through Small->Medium->Full */
        for (int i = 0; i < 1000; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "key%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            const databox *elements[2] = {&key, &val};
            bool replaced = multimapInsert(&m, elements);
            assert(!replaced);
            (void)replaced;
        }
        assert(multimapCount(m) == 1000);

        /* Now update all keys - each should report replaced=true */
        for (int i = 0; i < 1000; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "key%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i + 10000); /* Different value */
            const databox *elements[2] = {&key, &val};
            bool replaced = multimapInsert(&m, elements);
            if (!replaced) {
                ERR("Key %s was not recognized as duplicate at iteration %d!",
                    keyBuf, i);
                assert(replaced);
            }
        }
        /* Size must still be 1000, not 2000 */
        assert(multimapCount(m) == 1000);

        /* Verify all values were updated */
        for (int i = 0; i < 1000; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "key%d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox result = {{0}};
            databox *results[1] = {&result};
            bool found = multimapLookup(m, &key, results);
            assert(found);
            (void)found;
            databox expected = databoxNewSigned(i + 10000);
            if (!databoxEqual(&expected, &result)) {
                ERR("Key %s has wrong value at iteration %d!", keyBuf, i);
                assert(databoxEqual(&expected, &result));
            }
        }
    }

    TEST("integrity: FieldIncr width sweep re-verifies middles + rangeBoxes") {
        /* String keys force byte-typed rangeBoxes (the kind a realloc
         * strands); signed values sweep every integer encoding width in
         * both directions so single-entry mutations re-encode entries
         * and drag map totals across varint header width boundaries.
         * After EVERY operation the cached middles (all tiers) and
         * rangeBoxes (Full) are checked against fresh rescans, and at
         * the end every key must round-trip its exact shadow value. */
        const int64_t deltas[] = {
            1, 100, 30000, 8000000, INT64_C(3000000000), INT64_MAX / 4};
        const size_t nDeltas = sizeof(deltas) / sizeof(*deltas);

        for (int count = 4; count <= 1024; count *= 4) {
            cleaner(multimapFree) multimap *m =
                multimapNewLimit(2, FLEX_CAP_LEVEL_64);
            cleanerName(zfree, Int64) int64_t *shadow =
                zcalloc(count, sizeof(*shadow));

            for (int i = 0; i < count; i++) {
                char keyBuf[32];
                snprintf(keyBuf, sizeof(keyBuf), "key%05d", i);
                databox key = databoxNewBytesString(keyBuf);
                databox val = databoxNewSigned(0);
                const databox *elements[2] = {&key, &val};
                multimapInsert(&m, elements);
                multimapVerify(m);
            }

            /* Sweep encoding widths UP... */
            for (size_t d = 0; d < nDeltas; d++) {
                for (int i = 0; i < count; i++) {
                    char keyBuf[32];
                    snprintf(keyBuf, sizeof(keyBuf), "key%05d", i);
                    databox key = databoxNewBytesString(keyBuf);
                    shadow[i] += deltas[d];
                    const int64_t got =
                        multimapFieldIncr(&m, &key, 1, deltas[d]);
                    if (got != shadow[i]) {
                        ERR("FieldIncr(+%" PRId64 ") on %s returned %" PRId64
                            " expected %" PRId64 "!",
                            deltas[d], keyBuf, got, shadow[i]);
                        assert(got == shadow[i]);
                    }

                    multimapVerify(m);
                }
            }

            /* ...then back DOWN (shrinking re-encodes + header shrink) */
            for (size_t d = nDeltas; d-- > 0;) {
                for (int i = 0; i < count; i++) {
                    char keyBuf[32];
                    snprintf(keyBuf, sizeof(keyBuf), "key%05d", i);
                    databox key = databoxNewBytesString(keyBuf);
                    shadow[i] -= deltas[d];
                    const int64_t got =
                        multimapFieldIncr(&m, &key, 1, -deltas[d]);
                    if (got != shadow[i]) {
                        ERR("FieldIncr(-%" PRId64 ") on %s returned %" PRId64
                            " expected %" PRId64 "!",
                            deltas[d], keyBuf, got, shadow[i]);
                        assert(got == shadow[i]);
                    }

                    multimapVerify(m);
                }
            }

            /* Full round-trip: every key still findable, exact value. */
            for (int i = 0; i < count; i++) {
                char keyBuf[32];
                snprintf(keyBuf, sizeof(keyBuf), "key%05d", i);
                databox key = databoxNewBytesString(keyBuf);
                databox result = {{0}};
                databox *results[1] = {&result};
                const bool found = multimapLookup(m, &key, results);
                assert(found);
                (void)found;
                databox expected = databoxNewSigned(shadow[i]);
                assert(databoxEqual(&expected, &result));
            }

            /* The largest sweep must have exercised the Full tier or
             * this test silently stopped covering rangeBoxes. */
            if (count == 1024) {
                assert(multimapType_(m) == MULTIMAP_TYPE_FULL);
            }
        }
    }

    TEST("integrity: Replace/ResizeEntry byte sweep re-verifies "
         "middles + rangeBoxes") {
        /* Byte values grown and shrunk across two orders of magnitude
         * realloc the flexes constantly and cross varint header widths;
         * every mutation must leave middles and rangeBoxes coherent. */
        static const size_t lens[] = {1, 8, 63, 64, 65, 250, 1024, 5, 4096, 2};
        const size_t nLens = sizeof(lens) / sizeof(*lens);
        const int count = 256;

        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_64);
        for (int i = 0; i < count; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "key%05d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewBytesString("v");
            const databox *elements[2] = {&key, &val};
            multimapInsert(&m, elements);
        }

        assert(multimapType_(m) == MULTIMAP_TYPE_FULL);

        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(4096);
        for (size_t L = 0; L < nLens; L++) {
            const size_t len = lens[L];
            memset(buf, (int)('a' + (len % 26)), len);

            for (int i = 0; i < count; i++) {
                char keyBuf[32];
                snprintf(keyBuf, sizeof(keyBuf), "key%05d", i);
                databox key = databoxNewBytesString(keyBuf);

                multimapEntry me = {0};
                const bool found = multimapGetUnderlyingEntry(m, &key, &me);
                assert(found);
                (void)found;

                /* advance from the key entry to its value entry */
                me.fe = flexNext(*me.map, me.fe);
                databox val = databoxNewBytes(buf, len);
                multimapReplaceEntry(&m, &me, &val);
                multimapVerify(m);
            }

            /* Spot round-trip: content AND length must match. */
            for (int i = 0; i < count; i += 51) {
                char keyBuf[32];
                snprintf(keyBuf, sizeof(keyBuf), "key%05d", i);
                databox key = databoxNewBytesString(keyBuf);
                databox result = {{0}};
                databox *results[1] = {&result};
                const bool found = multimapLookup(m, &key, results);
                assert(found);
                (void)found;
                assert(result.len == len);
                assert(memcmp(result.data.bytes.start, buf, len) == 0);
            }
        }

        /* ResizeEntry pass: length-only contract (content past the old
         * prefix is unspecified), exercising grow, shrink, and the
         * regularize/split path for oversized maps. */
        static const size_t resizeTo[] = {9, 2048, 3};
        for (size_t L = 0; L < sizeof(resizeTo) / sizeof(*resizeTo); L++) {
            for (int i = 0; i < count; i++) {
                char keyBuf[32];
                snprintf(keyBuf, sizeof(keyBuf), "key%05d", i);
                databox key = databoxNewBytesString(keyBuf);

                multimapEntry me = {0};
                const bool found = multimapGetUnderlyingEntry(m, &key, &me);
                assert(found);
                (void)found;

                me.fe = flexNext(*me.map, me.fe);
                multimapResizeEntry(&m, &me, resizeTo[L]);
                multimapVerify(m);

                databox result = {{0}};
                databox *results[1] = {&result};
                const bool stillThere = multimapLookup(m, &key, results);
                assert(stillThere);
                (void)stillThere;
                assert(result.len == resizeTo[L]);
            }
        }
    }

    TEST("ProcessUntil: wide rows (> 16 elements) use a real heap "
         "pointer array") {
        /* elementsPerEntry > 16 forces the heap-elements path; every
         * element pointer must be populated and readable. */
        const multimapElements epe = 20;
        cleaner(multimapFree) multimap *m = multimapNew(epe);

        int64_t expectSum = 0;
        for (int row = 0; row < 3; row++) {
            databox boxes[20];
            const databox *elements[20];
            boxes[0] = databoxNewSigned(row);
            elements[0] = &boxes[0];
            for (int i = 1; i < 20; i++) {
                boxes[i] = databoxNewSigned((row * 100) + i);
                elements[i] = &boxes[i];
                expectSum += (row * 100) + i;
            }

            multimapInsert(&m, elements);
        }

        const multimapPredicate all = {.condition = MULTIMAP_CONDITION_ALL};
        processUntilWideCheck check = {.elementsPerEntry = epe};
        const size_t processed =
            multimapProcessUntil(m, &all, true, processUntilWideWalker, &check);
        assert(processed == 3);
        assert(check.rows == 3);
        if (check.sum != expectSum) {
            ERR("wide ProcessUntil sum %" PRId64 " expected %" PRId64 "!",
                check.sum, expectSum);
            assert(check.sum == expectSum);
        }
    }

    TEST("EntryResize/EntryReplace + ByPosition: key-level and "
         "positional row access") {
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_64);
        for (int i = 0; i < 300; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "key%05d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            const databox *elements[2] = {&key, &val};
            multimapInsert(&m, elements);
        }

        assert(multimapType_(m) == MULTIMAP_TYPE_FULL);

        /* LookupByPosition: full-width rows, forward and negative */
        databox rk = {{0}};
        databox rv = {{0}};
        databox *row[2] = {&rk, &rv};
        assert(multimapLookupByPosition(m, 0, row));
        assert(rv.data.i == 0);
        assert(multimapLookupByPosition(m, 137, row));
        assert(rv.data.i == 137);
        assert(multimapLookupByPosition(m, -1, row));
        assert(rv.data.i == 299);
        assert(multimapLookupByPosition(m, -300, row));
        assert(rv.data.i == 0);
        assert(!multimapLookupByPosition(m, 300, row));
        assert(!multimapLookupByPosition(m, -301, row));

        /* EntryReplace: values swap in place, key untouched */
        databox findKey = databoxNewBytesString("key00137");
        databox newVal = databoxNewSigned(-9137);
        const databox *reps[1] = {&newVal};
        multimapEntryReplace(&m, &findKey, reps);
        multimapVerify(m);
        databox got = {{0}};
        databox *gotP[1] = {&got};
        assert(multimapLookup(m, &findKey, gotP));
        assert(got.data.i == -9137);

        /* EntryResize: value becomes a byte region of the new size */
        databox bytesVal = databoxNewBytesString("resize-me-payload");
        const databox *reps2[1] = {&bytesVal};
        multimapEntryReplace(&m, &findKey, reps2);
        multimapEntryResize(&m, &findKey, 64);
        multimapVerify(m);
        assert(multimapLookup(m, &findKey, gotP));
        assert(databoxLen(&got) == 64);

        /* DeleteByPosition: forward and negative addressing */
        assert(multimapDeleteByPosition(&m, 0)); /* key00000 */
        assert(multimapCount(m) == 299);
        databox gone = databoxNewBytesString("key00000");
        assert(!multimapExists(m, &gone));

        assert(multimapDeleteByPosition(&m, -1)); /* key00299 */
        assert(multimapCount(m) == 298);
        databox gone2 = databoxNewBytesString("key00299");
        assert(!multimapExists(m, &gone2));

        assert(!multimapDeleteByPosition(&m, 500));
        multimapVerify(m);

        /* Interior deletes land near the middle anchor; verify the
         * neighborhood stays coherent afterwards. */
        assert(multimapLookupByPosition(m, 150, row));
        const int64_t midVal = rv.data.i;
        assert(multimapDeleteByPosition(&m, 150));
        multimapVerify(m);
        assert(multimapLookupByPosition(m, 150, row));
        assert(rv.data.i != midVal);
        assert(multimapCount(m) == 297);
    }

    TEST("ByPosition: every position addressable across all tiers") {
        /* Grow one map through Small -> Medium -> Full; at each tier
         * assert every position (both sign conventions) returns the
         * exact expected row.  Values are keyed to insertion order so a
         * positional miss or misroute is a value mismatch. */
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_64);
        int inserted = 0;
        multimapType lastType = MULTIMAP_TYPE_SMALL;
        bool sawSmall = false;
        bool sawMedium = false;
        bool sawFull = false;

        databox rk = {{0}};
        databox rv = {{0}};
        databox *row[2] = {&rk, &rv};

        while (inserted < 600) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "key%05d", inserted);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(inserted);
            const databox *elements[2] = {&key, &val};
            multimapInsert(&m, elements);
            inserted++;

            const multimapType t = multimapType_(m);
            const bool tierChanged = t != lastType;
            lastType = t;

            /* Full positional sweep at tier entry and periodically */
            if (tierChanged || (inserted % 149) == 0 || inserted <= 4) {
                switch (t) {
                case MULTIMAP_TYPE_SMALL:
                    sawSmall = true;
                    break;
                case MULTIMAP_TYPE_MEDIUM:
                    sawMedium = true;
                    break;
                case MULTIMAP_TYPE_FULL:
                    sawFull = true;
                    break;
                default:
                    assert(NULL);
                }

                for (int i = 0; i < inserted; i++) {
                    assert(multimapLookupByPosition(m, i, row));
                    assert(rv.data.i == i);
                    assert(multimapLookupByPosition(m, i - inserted, row));
                    assert(rv.data.i == i);
                }

                assert(!multimapLookupByPosition(m, inserted, row));
                assert(!multimapLookupByPosition(m, -inserted - 1, row));
            }
        }

        assert(sawSmall && sawMedium && sawFull);

        /* Delete every row positionally (alternating ends and middle)
         * until empty; count and verifier stay exact throughout. */
        int64_t remaining = 600;
        int which = 0;
        while (remaining > 0) {
            const int64_t pos = (which % 3 == 0)   ? 0
                                : (which % 3 == 1) ? -1
                                                   : remaining / 2;
            assert(multimapDeleteByPosition(&m, pos));
            remaining--;
            which++;
            assert((int64_t)multimapCount(m) == remaining);
            if ((remaining % 97) == 0) {
                multimapVerify(m);
            }
        }

        assert(!multimapDeleteByPosition(&m, 0));
    }

    TEST("FieldIncr: tier-direct calls hit + miss (no crash, miss == 0)") {
        databox missing = databoxNewBytesString("zzz-no-such-key");

        /* Small tier: direct call */
        {
            cleaner(multimapFree) multimap *m =
                multimapNewLimit(2, FLEX_CAP_LEVEL_64);
            for (int i = 0; i < 4; i++) {
                char keyBuf[32];
                snprintf(keyBuf, sizeof(keyBuf), "key%05d", i);
                databox key = databoxNewBytesString(keyBuf);
                databox val = databoxNewSigned(10);
                const databox *elements[2] = {&key, &val};
                multimapInsert(&m, elements);
            }

            assert(multimapType_(m) == MULTIMAP_TYPE_SMALL);

            databox key = databoxNewBytesString("key00000");
            assert(multimapSmallFieldIncr(mms(m), &key, 1, 5) == 15);
            multimapVerify(m);
            assert(multimapSmallFieldIncr(mms(m), &missing, 1, 5) == 0);
            assert(multimapFieldIncr(&m, &missing, 1, 5) == 0);
            multimapVerify(m);
        }

        /* Medium then Full tiers: force upgrades, direct call each */
        {
            cleaner(multimapFree) multimap *m =
                multimapNewLimit(2, FLEX_CAP_LEVEL_64);
            int i = 0;
            while (multimapType_(m) == MULTIMAP_TYPE_SMALL && i < 5000) {
                char keyBuf[32];
                snprintf(keyBuf, sizeof(keyBuf), "key%05d", i++);
                databox key = databoxNewBytesString(keyBuf);
                databox val = databoxNewSigned(10);
                const databox *elements[2] = {&key, &val};
                multimapInsert(&m, elements);
            }

            assert(multimapType_(m) == MULTIMAP_TYPE_MEDIUM);

            databox key = databoxNewBytesString("key00000");
            assert(multimapMediumFieldIncr(mmm(m), &key, 1, 5) == 15);
            multimapVerify(m);
            assert(multimapMediumFieldIncr(mmm(m), &missing, 1, 5) == 0);
            multimapVerify(m);

            while (multimapType_(m) == MULTIMAP_TYPE_MEDIUM && i < 50000) {
                char keyBuf[32];
                snprintf(keyBuf, sizeof(keyBuf), "key%05d", i++);
                databox key2 = databoxNewBytesString(keyBuf);
                databox val = databoxNewSigned(10);
                const databox *elements[2] = {&key2, &val};
                multimapInsert(&m, elements);
            }

            assert(multimapType_(m) == MULTIMAP_TYPE_FULL);

            /* This path previously terminated in
             * assert(NULL && "Not correctly implemented yet!") */
            assert(multimapFullFieldIncr(mmf(m), &key, 1, 5) == 20);
            multimapVerify(m);
            assert(multimapFullFieldIncr(mmf(m), &missing, 1, 5) == 0);
            multimapVerify(m);
        }
    }

#if 1
    TEST("speeds at different sizes") {
        uint64_t secs[20] = {0};
        size_t size[20] = {0};
        int i = 0;
        for (size_t maxIdx = 0; maxIdx < flexOptimizationSizeLimits; maxIdx++) {
            flexCapSizeLimit maxSize = flexOptimizationSizeLimit[maxIdx];
            cleaner(multimapFree) multimap *m = multimapNewLimit(2, maxIdx);
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
        }
    }

    printf("\n\n");
#endif

    TEST("(full width) small->medium->full: insert / exists / lookup / delete "
         "/ type "
         "check...") {
        for (int32_t i = 0; i < 4096; i++) {
            cleaner(multimapFree) multimap *m =
                multimapNewLimit(2, FLEX_CAP_LEVEL_2048);
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

            printf("\n");
        }
    }

    TEST("(key only) small->medium->full: insert / exists / lookup / delete / "
         "type "
         "check...") {
        for (int32_t i = 0; i < 4096; i++) {
            cleaner(multimapFree) multimap *m =
                multimapNewLimit(2, FLEX_CAP_LEVEL_2048);
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

            printf("\n");
        }
    }

    /* ================================================================
     * Edge Case and Boundary Tests
     * ================================================================ */

    TEST("duplicate key insertion behavior") {
        /* Test that duplicate keys work correctly across all map types */
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_512);

        /* Insert same key multiple times with different values */
        for (int32_t i = 0; i < 100; i++) {
            databox key = databoxNewSigned(42); /* Same key */
            databox val = databoxNewSigned(i);
            const databox *elements[2] = {&key, &val};
            multimapInsertFullWidth(&m, elements);
        }

        assert(multimapCount(m) == 100);

        /* Verify all entries exist */
        for (int32_t i = 0; i < 100; i++) {
            databox key = databoxNewSigned(42);
            databox val = databoxNewSigned(i);
            const databox *elements[2] = {&key, &val};
            bool found = multimapExistsFullWidth(m, elements);
            if (!found) {
                ERR("Duplicate key entry %d not found!", i);
            }
        }

        /* Delete all entries one by one */
        for (int32_t i = 0; i < 100; i++) {
            databox key = databoxNewSigned(42);
            databox val = databoxNewSigned(i);
            const databox *elements[2] = {&key, &val};
            bool deleted = multimapDeleteFullWidth(&m, elements);
            if (!deleted) {
                ERR("Failed to delete duplicate key entry %d!", i);
            }
        }

        assert(multimapCount(m) == 0);
    }

    TEST("boundary values (INT64_MIN, INT64_MAX, 0)") {
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_512);

        /* Test extreme integer values */
        int64_t extremeKeys[] = {INT64_MIN, INT64_MIN + 1, -1,       0,
                                 1,         INT64_MAX - 1, INT64_MAX};
        size_t numKeys = sizeof(extremeKeys) / sizeof(extremeKeys[0]);

        /* Insert all extreme values */
        for (size_t i = 0; i < numKeys; i++) {
            databox key = databoxNewSigned(extremeKeys[i]);
            databox val = databoxNewSigned((int64_t)i);
            const databox *elements[2] = {&key, &val};
            multimapInsert(&m, elements);
        }

        assert(multimapCount(m) == numKeys);

        /* Verify all exist and can be looked up */
        for (size_t i = 0; i < numKeys; i++) {
            databox key = databoxNewSigned(extremeKeys[i]);
            bool exists = multimapExists(m, &key);
            if (!exists) {
                ERR("Extreme key %" PRId64 " not found!", extremeKeys[i]);
            }

            databox foundVal = {{0}};
            databox *values[1] = {&foundVal};
            bool found = multimapLookup(m, &key, values);
            if (!found) {
                ERR("Extreme key %" PRId64 " lookup failed!", extremeKeys[i]);
            }
            if (foundVal.data.i != (int64_t)i) {
                ERR("Extreme key %" PRId64 " has wrong value!", extremeKeys[i]);
            }
        }

        /* Delete in random order */
        size_t order[] = {3, 0, 6, 2, 5, 1, 4};
        for (size_t i = 0; i < numKeys; i++) {
            databox key = databoxNewSigned(extremeKeys[order[i]]);
            bool deleted = multimapDelete(&m, &key);
            if (!deleted) {
                ERR("Failed to delete extreme key %" PRId64 "!",
                    extremeKeys[order[i]]);
            }
        }

        assert(multimapCount(m) == 0);
    }

    TEST("map split boundary correctness") {
        /* Force map splits and verify boundary lookups */
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_256);

        /* Insert enough to trigger multiple splits */
        const int32_t numEntries = 500;
        for (int32_t i = 0; i < numEntries; i++) {
            databox key = databoxNewSigned(i * 10); /* Spread keys */
            databox val = databoxNewSigned(i);
            const databox *elements[2] = {&key, &val};
            multimapInsert(&m, elements);
        }

        /* Verify ALL entries are findable after splits */
        for (int32_t i = 0; i < numEntries; i++) {
            databox key = databoxNewSigned(i * 10);
            bool exists = multimapExists(m, &key);
            if (!exists) {
                ERR("Key %d lost after splits!", i * 10);
            }
        }

        /* Verify non-existent keys between entries */
        for (int32_t i = 0; i < numEntries - 1; i++) {
            databox key = databoxNewSigned(i * 10 + 5); /* Between entries */
            bool exists = multimapExists(m, &key);
            if (exists) {
                ERR("Non-existent key %d incorrectly found!", i * 10 + 5);
            }
        }

        multimapVerify(m);
    }

    TEST("interleaved insert/delete across boundaries") {
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_256);

        /* Insert many entries */
        for (int32_t i = 0; i < 300; i++) {
            databox key = databoxNewSigned(i);
            databox val = databoxNewSigned(i * 100);
            const databox *elements[2] = {&key, &val};
            multimapInsert(&m, elements);
        }

        /* Delete every other entry */
        for (int32_t i = 0; i < 300; i += 2) {
            databox key = databoxNewSigned(i);
            bool deleted = multimapDelete(&m, &key);
            if (!deleted) {
                ERR("Failed to delete key %d!", i);
            }
        }

        /* Verify remaining entries */
        for (int32_t i = 0; i < 300; i++) {
            databox key = databoxNewSigned(i);
            bool exists = multimapExists(m, &key);
            if (i % 2 == 0 && exists) {
                ERR("Deleted key %d still exists!", i);
            }
            if (i % 2 == 1 && !exists) {
                ERR("Remaining key %d not found!", i);
            }
        }

        /* Re-insert deleted entries */
        for (int32_t i = 0; i < 300; i += 2) {
            databox key = databoxNewSigned(i);
            databox val = databoxNewSigned(i * 100 + 1);
            const databox *elements[2] = {&key, &val};
            multimapInsert(&m, elements);
        }

        /* Verify all entries */
        assert(multimapCount(m) == 300);
        for (int32_t i = 0; i < 300; i++) {
            databox key = databoxNewSigned(i);
            bool exists = multimapExists(m, &key);
            if (!exists) {
                ERR("Re-inserted key %d not found!", i);
            }
        }

        multimapVerify(m);
    }

    TEST("reverse order insertion") {
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_256);

        /* Insert in reverse order to stress binary search */
        for (int32_t i = 500; i >= 0; i--) {
            databox key = databoxNewSigned(i);
            databox val = databoxNewSigned(i);
            const databox *elements[2] = {&key, &val};
            multimapInsert(&m, elements);

            /* Verify immediately */
            bool exists = multimapExists(m, &key);
            if (!exists) {
                ERR("Reverse insert: key %d not found immediately!", i);
            }
        }

        /* Verify final state */
        assert(multimapCount(m) == 501);
        for (int32_t i = 0; i <= 500; i++) {
            databox key = databoxNewSigned(i);
            bool exists = multimapExists(m, &key);
            if (!exists) {
                ERR("Reverse insert final: key %d not found!", i);
            }
        }

        multimapVerify(m);
    }

    TEST("random order insertion with verification") {
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_256);

        /* Generate pseudo-random keys using LCG */
        const int32_t numEntries = 1000;
        cleanerName(zfree, Int32) int32_t *keys =
            zcalloc(numEntries, sizeof(int32_t));
        uint32_t seed = 54321;

        for (int32_t i = 0; i < numEntries; i++) {
            seed = seed * 1103515245 + 12345;
            keys[i] = (int32_t)(seed % 100000);
        }

        /* Insert all keys */
        for (int32_t i = 0; i < numEntries; i++) {
            databox key = databoxNewSigned(keys[i]);
            databox val = databoxNewSigned(i);
            const databox *elements[2] = {&key, &val};
            multimapInsertFullWidth(&m, elements);
        }

        /* Verify all entries exist */
        for (int32_t i = 0; i < numEntries; i++) {
            databox key = databoxNewSigned(keys[i]);
            databox val = databoxNewSigned(i);
            const databox *elements[2] = {&key, &val};
            bool found = multimapExistsFullWidth(m, elements);
            if (!found) {
                ERR("Random insert: entry [%d, %d] not found!", keys[i], i);
            }
        }

        multimapVerify(m);
    }

    TEST("string key boundary handling") {
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_256);

        /* Insert string keys that stress binary search */
        const char *stringKeys[] = {
            "",
            "a",
            "aa",
            "aaa",
            "ab",
            "b",
            "ba",
            "bb",
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
            "\x00\x01\x02",
            "\xFF\xFE\xFD"};
        const size_t numKeys = sizeof(stringKeys) / sizeof(stringKeys[0]);

        for (size_t i = 0; i < numKeys; i++) {
            databox key = databoxNewBytesString(stringKeys[i]);
            databox val = databoxNewSigned((int64_t)i);
            const databox *elements[2] = {&key, &val};
            multimapInsert(&m, elements);
        }

        /* Verify all exist */
        for (size_t i = 0; i < numKeys; i++) {
            databox key = databoxNewBytesString(stringKeys[i]);
            bool exists = multimapExists(m, &key);
            if (!exists) {
                ERR("String key '%s' not found!", stringKeys[i]);
            }
        }

        multimapVerify(m);
    }

    TEST("upgrade path Small -> Medium -> Full") {
        /* Use very small limits to force upgrades */
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_64);

        multimapType prevType = multimapType_(m);
        int32_t smallToMediumTransition = -1;
        int32_t mediumToFullTransition = -1;

        /* Insert until we've seen all transitions */
        for (int32_t i = 0; i < 10000 && mediumToFullTransition < 0; i++) {
            databox key = databoxNewSigned(i);
            databox val = databoxNewSigned(i);
            const databox *elements[2] = {&key, &val};
            multimapInsert(&m, elements);

            multimapType newType = multimapType_(m);
            if (prevType == MULTIMAP_TYPE_SMALL &&
                newType == MULTIMAP_TYPE_MEDIUM) {
                smallToMediumTransition = i;
                printf("    Small->Medium at entry %d\n", i);
            } else if (prevType == MULTIMAP_TYPE_MEDIUM &&
                       newType == MULTIMAP_TYPE_FULL) {
                mediumToFullTransition = i;
                printf("    Medium->Full at entry %d\n", i);
            }
            prevType = newType;
        }

        /* smallToMediumTransition is for debug info only */
        (void)smallToMediumTransition;

        /* Verify all entries still exist after transitions */
        size_t count = multimapCount(m);
        for (size_t i = 0; i < count; i++) {
            databox key = databoxNewSigned((int64_t)i);
            bool exists = multimapExists(m, &key);
            if (!exists) {
                ERR("Entry %zu lost after type transitions!", i);
            }
        }

        multimapVerify(m);
    }

    TEST("first/last element retrieval across boundaries") {
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_256);

        /* Insert entries spanning multiple internal maps */
        for (int32_t i = 1000; i >= 0; i--) {
            databox key = databoxNewSigned(i);
            databox val = databoxNewSigned(i * 10);
            const databox *elements[2] = {&key, &val};
            multimapInsert(&m, elements);
        }

        /* Verify first element */
        databox firstKey = {{0}};
        databox firstVal = {{0}};
        databox *firstElements[2] = {&firstKey, &firstVal};
        bool gotFirst = multimapFirst(m, firstElements);
        if (!gotFirst || firstKey.data.i != 0) {
            ERR("First element wrong: got %" PRIdMAX ", expected 0",
                firstKey.data.i);
        }

        /* Verify last element */
        databox lastKey = {{0}};
        databox lastVal = {{0}};
        databox *lastElements[2] = {&lastKey, &lastVal};
        bool gotLast = multimapLast(m, lastElements);
        if (!gotLast || lastKey.data.i != 1000) {
            ERR("Last element wrong: got %" PRIdMAX ", expected 1000",
                lastKey.data.i);
        }

        multimapVerify(m);
    }

    TEST("iterator across all map boundaries") {
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_256);

        /* Insert entries */
        for (int32_t i = 0; i < 500; i++) {
            databox key = databoxNewSigned(i);
            databox val = databoxNewSigned(i * 2);
            const databox *elements[2] = {&key, &val};
            multimapInsert(&m, elements);
        }

        /* Forward iteration */
        multimapIterator iter;
        multimapIteratorInit(m, &iter, true);
        databox key = {{0}};
        databox val = {{0}};
        databox *elements[2] = {&key, &val};

        int64_t expected = 0;
        int64_t iterCount = 0;
        while (multimapIteratorNext(&iter, elements)) {
            if (key.data.i != expected) {
                ERR("Forward iter: expected %" PRId64 ", got %" PRIdMAX,
                    expected, key.data.i);
            }
            expected++;
            iterCount++;
        }

        if (iterCount != 500) {
            ERR("Forward iter: expected 500 entries, got %" PRId64, iterCount);
        }

        /* Backward iteration */
        multimapIteratorInit(m, &iter, false);
        expected = 499;
        iterCount = 0;
        while (multimapIteratorNext(&iter, elements)) {
            if (key.data.i != expected) {
                ERR("Backward iter: expected %" PRId64 ", got %" PRIdMAX,
                    expected, key.data.i);
            }
            expected--;
            iterCount++;
        }

        if (iterCount != 500) {
            ERR("Backward iter: expected 500 entries, got %" PRId64, iterCount);
        }

        multimapVerify(m);
    }

    TEST("delete causing map merge") {
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_256);

        /* Insert to create multiple maps */
        for (int32_t i = 0; i < 500; i++) {
            databox key = databoxNewSigned(i);
            databox val = databoxNewSigned(i);
            const databox *elements[2] = {&key, &val};
            multimapInsert(&m, elements);
        }

        /* Delete most entries to trigger potential merges */
        for (int32_t i = 0; i < 490; i++) {
            databox key = databoxNewSigned(i);
            bool deleted = multimapDelete(&m, &key);
            if (!deleted) {
                ERR("Failed to delete key %d!", i);
            }
        }

        assert(multimapCount(m) == 10);

        /* Verify remaining entries */
        for (int32_t i = 490; i < 500; i++) {
            databox key = databoxNewSigned(i);
            bool exists = multimapExists(m, &key);
            if (!exists) {
                ERR("Remaining key %d not found after mass delete!", i);
            }
        }

        multimapVerify(m);
    }

    TEST("mixed type keys (integers and strings)") {
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_256);

        /* Insert integer keys */
        for (int32_t i = 0; i < 100; i++) {
            databox key = databoxNewSigned(i);
            databox val = databoxNewSigned(i);
            const databox *elements[2] = {&key, &val};
            multimapInsert(&m, elements);
        }

        /* Insert string keys */
        for (int32_t i = 0; i < 100; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "key_%03d", i);
            databox key = databoxNewBytesString(buf);
            databox val = databoxNewSigned(i + 1000);
            const databox *elements[2] = {&key, &val};
            multimapInsert(&m, elements);
        }

        assert(multimapCount(m) == 200);

        /* Verify integer keys */
        for (int32_t i = 0; i < 100; i++) {
            databox key = databoxNewSigned(i);
            bool exists = multimapExists(m, &key);
            if (!exists) {
                ERR("Integer key %d not found!", i);
            }
        }

        /* Verify string keys */
        for (int32_t i = 0; i < 100; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "key_%03d", i);
            databox key = databoxNewBytesString(buf);
            bool exists = multimapExists(m, &key);
            if (!exists) {
                ERR("String key '%s' not found!", buf);
            }
        }

        multimapVerify(m);
    }

    TEST("PERF: lookup performance across many maps") {
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_256);

        /* Insert enough to create many internal maps */
        const int32_t numEntries = 10000;
        for (int32_t i = 0; i < numEntries; i++) {
            databox key = databoxNewSigned(i);
            databox val = databoxNewSigned(i);
            const databox *elements[2] = {&key, &val};
            multimapInsert(&m, elements);
        }

        printf("    Entry count: %zu\n", multimapCount(m));

        /* Benchmark lookups */
        int64_t startNs = timeUtilMonotonicNs();
        for (int32_t round = 0; round < 10; round++) {
            for (int32_t i = 0; i < numEntries; i++) {
                databox key = databoxNewSigned(i);
                multimapExists(m, &key);
            }
        }
        int64_t elapsed = timeUtilMonotonicNs() - startNs;
        int64_t totalOps = numEntries * 10;
        printf("    Lookup: %.1f ns/op, %.0f ops/sec\n",
               (double)elapsed / totalOps, totalOps / (elapsed / 1e9));
    }

    TEST("stress test: insert/delete/lookup random mix") {
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_256);
        uint32_t seed = 98765;
        cleanerName(zfree, Int32) int32_t *inserted =
            zcalloc(10000, sizeof(int32_t));
        size_t insertedCount = 0;

        /* Random mix of operations */
        for (int32_t i = 0; i < 5000; i++) {
            seed = seed * 1103515245 + 12345;
            int32_t op = seed % 10; /* 0-6: insert, 7-8: delete, 9: verify */
            int32_t key = seed % 10000;

            if (op < 7) {
                /* Insert */
                databox k = databoxNewSigned(key);
                databox v = databoxNewSigned(i);
                const databox *elements[2] = {&k, &v};
                multimapInsertFullWidth(&m, elements);
                inserted[insertedCount++] = key;
            } else if (op < 9 && insertedCount > 0) {
                /* Delete random existing */
                size_t idx = seed % insertedCount;
                databox k = databoxNewSigned(inserted[idx]);
                multimapDelete(&m, &k);
            } else if (insertedCount > 0) {
                /* Verify random existing */
                size_t idx = seed % insertedCount;
                databox k = databoxNewSigned(inserted[idx]);
                /* Key might have been deleted, so don't assert on exists */
                multimapExists(m, &k);
            }

            /* Periodic verification */
            if (i % 1000 == 0) {
                multimapVerify(m);
            }
        }

        multimapVerify(m);
    }

    /* ================================================================
     * COMPREHENSIVE BINARY SEARCH FUZZ TESTS
     * ================================================================
     * These tests use an oracle (shadow data structure) to verify that
     * every inserted element can ALWAYS be found via binary search,
     * regardless of key distribution, map type, or insertion order.
     * ================================================================ */

    TEST("FUZZ: binary search correctness - sequential keys") {
        /* Test sequential key insertion - stresses in-order binary search */
        for (int limit = 0; limit < 4; limit++) {
            flexCapSizeLimit capLimit = (flexCapSizeLimit[]){
                FLEX_CAP_LEVEL_64, FLEX_CAP_LEVEL_256, FLEX_CAP_LEVEL_512,
                FLEX_CAP_LEVEL_2048}[limit];

            cleaner(multimapFree) multimap *m = multimapNewLimit(2, capLimit);
            const int32_t numKeys = 2000;

            /* Insert sequential keys */
            for (int32_t i = 0; i < numKeys; i++) {
                databox key = databoxNewSigned(i);
                databox val = databoxNewSigned(i * 100);
                const databox *elements[2] = {&key, &val};
                multimapInsert(&m, elements);

                /* Verify ALL previously inserted keys can still be found */
                for (int32_t j = 0; j <= i; j++) {
                    databox checkKey = databoxNewSigned(j);
                    if (!multimapExists(m, &checkKey)) {
                        ERR("FUZZ FAIL: Sequential key %d not found after "
                            "inserting key %d (limit=%d, type=%d)!",
                            j, i, capLimit, multimapType_(m));
                        assert(NULL);
                    }
                }
            }

            /* Final comprehensive check */
            for (int32_t i = 0; i < numKeys; i++) {
                databox key = databoxNewSigned(i);
                databox gotVal = {{0}};
                databox *vals[1] = {&gotVal};
                bool found = multimapLookup(m, &key, vals);
                if (!found || gotVal.data.i != i * 100) {
                    ERR("FUZZ FAIL: Sequential lookup failed for key %d!", i);
                    assert(NULL);
                }
            }

            printf("    limit=%d type=%d count=%zu: OK\n", capLimit,
                   multimapType_(m), multimapCount(m));
        }
    }

    TEST("FUZZ: binary search correctness - reverse sequential keys") {
        /* Reverse insertion stresses insertions at the beginning */
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_256);
        const int32_t numKeys = 1500;

        /* Insert in reverse order */
        for (int32_t i = numKeys - 1; i >= 0; i--) {
            databox key = databoxNewSigned(i);
            databox val = databoxNewSigned(i);
            const databox *elements[2] = {&key, &val};
            multimapInsert(&m, elements);

            /* Verify this key and a sample of others */
            if (!multimapExists(m, &key)) {
                ERR("FUZZ FAIL: Reverse key %d not found immediately!", i);
                assert(NULL);
            }

            /* Check first 10 keys inserted (highest values) */
            for (int32_t j = numKeys - 1; j > numKeys - 11 && j >= i; j--) {
                databox checkKey = databoxNewSigned(j);
                if (!multimapExists(m, &checkKey)) {
                    ERR("FUZZ FAIL: Reverse key %d lost after inserting %d!", j,
                        i);
                    assert(NULL);
                }
            }
        }

        /* Final comprehensive verification */
        for (int32_t i = 0; i < numKeys; i++) {
            databox key = databoxNewSigned(i);
            if (!multimapExists(m, &key)) {
                ERR("FUZZ FAIL: Reverse final check failed for key %d!", i);
                assert(NULL);
            }
        }

        printf("    type=%d count=%zu: OK\n", multimapType_(m),
               multimapCount(m));
    }

    TEST("FUZZ: binary search correctness - random keys with oracle") {
        /* Use oracle (sorted array) to track what should exist */
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_256);
        const int32_t numKeys = 3000;

        /* Oracle: simple sorted array of inserted keys */
        cleanerName(zfree, Int64) int64_t *oracle =
            zcalloc(numKeys, sizeof(int64_t));
        size_t oracleCount = 0;

        uint64_t seed[2] = {0xDEADBEEF12345678ULL, 0xCAFEBABE87654321ULL};

        for (int32_t i = 0; i < numKeys; i++) {
            /* Generate random key */
            int64_t keyVal =
                (int64_t)(xoroshiro128plus(seed) % 1000000) - 500000;

            databox key = databoxNewSigned(keyVal);
            databox val = databoxNewSigned(i);
            const databox *elements[2] = {&key, &val};

            /* Check if key already exists (duplicate) */
            bool alreadyExists = multimapExists(m, &key);

            multimapInsert(&m, elements);

            /* Add to oracle if new */
            if (!alreadyExists) {
                /* Insert sorted */
                size_t insertPos = oracleCount;
                for (size_t j = 0; j < oracleCount; j++) {
                    if (oracle[j] > keyVal) {
                        insertPos = j;
                        break;
                    }
                }
                memmove(&oracle[insertPos + 1], &oracle[insertPos],
                        (oracleCount - insertPos) * sizeof(int64_t));
                oracle[insertPos] = keyVal;
                oracleCount++;
            }

            /* Periodic full oracle verification */
            if (i % 500 == 0 || i == numKeys - 1) {
                for (size_t j = 0; j < oracleCount; j++) {
                    databox checkKey = databoxNewSigned(oracle[j]);
                    if (!multimapExists(m, &checkKey)) {
                        ERR("FUZZ FAIL: Oracle key %" PRId64
                            " (idx %zu) not found "
                            "after %d inserts!",
                            oracle[j], j, i);
                        assert(NULL);
                    }
                }
            }
        }

        printf("    type=%d count=%zu oracle=%zu: OK\n", multimapType_(m),
               multimapCount(m), oracleCount);
    }

    TEST("FUZZ: binary search correctness - clustered keys") {
        /* Keys clustered in narrow ranges stress map splitting */
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_256);
        const int32_t numClusters = 10;
        const int32_t keysPerCluster = 200;

        cleanerName(zfree, Int64) int64_t *allKeys =
            zcalloc(numClusters * keysPerCluster, sizeof(int64_t));
        size_t keyCount = 0;

        uint64_t seed[2] = {42, 123};

        /* Create clusters at widely spaced base values */
        for (int32_t c = 0; c < numClusters; c++) {
            int64_t clusterBase = c * 100000;

            for (int32_t k = 0; k < keysPerCluster; k++) {
                /* Keys within ±50 of cluster base */
                int64_t offset = (int64_t)(xoroshiro128plus(seed) % 100) - 50;
                int64_t keyVal = clusterBase + offset;

                databox key = databoxNewSigned(keyVal);
                databox val = databoxNewSigned(c * 1000 + k);
                const databox *elements[2] = {&key, &val};

                if (!multimapExists(m, &key)) {
                    multimapInsert(&m, elements);
                    allKeys[keyCount++] = keyVal;
                }
            }

            /* Verify all keys in this and previous clusters */
            for (size_t j = 0; j < keyCount; j++) {
                databox checkKey = databoxNewSigned(allKeys[j]);
                if (!multimapExists(m, &checkKey)) {
                    ERR("FUZZ FAIL: Clustered key %" PRId64
                        " not found after cluster "
                        "%d!",
                        allKeys[j], c);
                    assert(NULL);
                }
            }
        }

        printf("    type=%d count=%zu clusters=%d: OK\n", multimapType_(m),
               multimapCount(m), numClusters);
    }

    TEST("FUZZ: binary search correctness - interleaved insert/delete") {
        /* Interleaved operations stress binary search with changing data */
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_256);

        /* Oracle tracks current state */
        cleanerName(zfree, Int64) int64_t *oracle =
            zcalloc(10000, sizeof(int64_t));
        size_t oracleCount = 0;

        uint64_t seed[2] = {99999, 11111};

        for (int32_t round = 0; round < 5000; round++) {
            uint64_t op = xoroshiro128plus(seed);

            if (op % 3 != 0 || oracleCount == 0) {
                /* Insert (2/3 probability) */
                int64_t keyVal = (int64_t)(xoroshiro128plus(seed) % 50000);
                databox key = databoxNewSigned(keyVal);
                databox val = databoxNewSigned(round);
                const databox *elements[2] = {&key, &val};

                if (!multimapExists(m, &key)) {
                    multimapInsert(&m, elements);

                    /* Add to oracle (keep sorted) */
                    size_t insertPos = oracleCount;
                    for (size_t j = 0; j < oracleCount; j++) {
                        if (oracle[j] > keyVal) {
                            insertPos = j;
                            break;
                        }
                    }
                    memmove(&oracle[insertPos + 1], &oracle[insertPos],
                            (oracleCount - insertPos) * sizeof(int64_t));
                    oracle[insertPos] = keyVal;
                    oracleCount++;
                }
            } else {
                /* Delete (1/3 probability) */
                size_t delIdx = xoroshiro128plus(seed) % oracleCount;
                int64_t keyVal = oracle[delIdx];
                databox key = databoxNewSigned(keyVal);

                bool deleted = multimapDelete(&m, &key);
                if (!deleted) {
                    ERR("FUZZ FAIL: Delete of oracle key %" PRId64 " failed!",
                        keyVal);
                    assert(NULL);
                }

                /* Remove from oracle */
                memmove(&oracle[delIdx], &oracle[delIdx + 1],
                        (oracleCount - delIdx - 1) * sizeof(int64_t));
                oracleCount--;
            }

            /* Periodic full verification */
            if (round % 500 == 0) {
                for (size_t j = 0; j < oracleCount; j++) {
                    databox checkKey = databoxNewSigned(oracle[j]);
                    if (!multimapExists(m, &checkKey)) {
                        ERR("FUZZ FAIL: Oracle key %" PRId64
                            " missing at round %d!",
                            oracle[j], round);
                        assert(NULL);
                    }
                }
                assert(multimapCount(m) == oracleCount);
            }
        }

        printf("    type=%d final_count=%zu: OK\n", multimapType_(m),
               oracleCount);
    }

    TEST("FUZZ: binary search correctness - boundary values") {
        /* Test extreme and boundary integer values */
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_256);

        int64_t boundaryKeys[] = {
            INT64_MIN,
            INT64_MIN + 1,
            INT64_MIN + 2,
            -1000000000000LL,
            -1000000LL,
            -1000,
            -100,
            -10,
            -2,
            -1,
            0,
            1,
            2,
            10,
            100,
            1000,
            1000000LL,
            1000000000000LL,
            INT64_MAX - 2,
            INT64_MAX - 1,
            INT64_MAX,
        };
        size_t numBoundary = sizeof(boundaryKeys) / sizeof(boundaryKeys[0]);

        /* Insert all boundary values */
        for (size_t i = 0; i < numBoundary; i++) {
            databox key = databoxNewSigned(boundaryKeys[i]);
            databox val = databoxNewSigned((int64_t)i);
            const databox *elements[2] = {&key, &val};
            multimapInsert(&m, elements);
        }

        /* Verify all can be found */
        for (size_t i = 0; i < numBoundary; i++) {
            databox key = databoxNewSigned(boundaryKeys[i]);
            if (!multimapExists(m, &key)) {
                ERR("FUZZ FAIL: Boundary key %" PRId64 " not found!",
                    boundaryKeys[i]);
                assert(NULL);
            }

            databox gotVal = {{0}};
            databox *vals[1] = {&gotVal};
            bool found = multimapLookup(m, &key, vals);
            if (!found || gotVal.data.i != (int64_t)i) {
                ERR("FUZZ FAIL: Boundary lookup wrong for %" PRId64 "!",
                    boundaryKeys[i]);
                assert(NULL);
            }
        }

        /* Now interleave with normal values */
        for (int32_t i = -1000; i <= 1000; i++) {
            databox key = databoxNewSigned(i);
            databox val = databoxNewSigned(i + 10000);
            const databox *elements[2] = {&key, &val};
            if (!multimapExists(m, &key)) {
                multimapInsert(&m, elements);
            }
        }

        /* Re-verify boundaries still work */
        for (size_t i = 0; i < numBoundary; i++) {
            databox key = databoxNewSigned(boundaryKeys[i]);
            if (!multimapExists(m, &key)) {
                ERR("FUZZ FAIL: Boundary key %" PRId64
                    " lost after interleave!",
                    boundaryKeys[i]);
                assert(NULL);
            }
        }

        printf("    type=%d count=%zu boundaries=%zu: OK\n", multimapType_(m),
               multimapCount(m), numBoundary);
    }

    TEST("FUZZ: binary search correctness - mixed types (int/string)") {
        /* Mix integer and string keys - tests databoxCompare across types */
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_256);

        const int32_t numInts = 500;
        const int32_t numStrings = 500;

        /* Insert integers */
        for (int32_t i = 0; i < numInts; i++) {
            databox key = databoxNewSigned(i * 7 - 1000);
            databox val = databoxNewSigned(i);
            const databox *elements[2] = {&key, &val};
            multimapInsert(&m, elements);
        }

        /* Insert strings */
        for (int32_t i = 0; i < numStrings; i++) {
            char buf[64];
            snprintf(buf, sizeof(buf), "key_%05d", i);
            databox key = databoxNewBytesString(buf);
            databox val = databoxNewSigned(i + 10000);
            const databox *elements[2] = {&key, &val};
            multimapInsert(&m, elements);
        }

        /* Verify all integers */
        for (int32_t i = 0; i < numInts; i++) {
            databox key = databoxNewSigned(i * 7 - 1000);
            if (!multimapExists(m, &key)) {
                ERR("FUZZ FAIL: Mixed-type int key %d not found!",
                    i * 7 - 1000);
                assert(NULL);
            }
        }

        /* Verify all strings */
        for (int32_t i = 0; i < numStrings; i++) {
            char buf[64];
            snprintf(buf, sizeof(buf), "key_%05d", i);
            databox key = databoxNewBytesString(buf);
            if (!multimapExists(m, &key)) {
                ERR("FUZZ FAIL: Mixed-type string key %s not found!", buf);
                assert(NULL);
            }
        }

        /* Verify non-existent keys return false */
        databox noKey1 = databoxNewSigned(999999);
        databox noKey2 = databoxNewBytesString("nonexistent_key");
        if (multimapExists(m, &noKey1) || multimapExists(m, &noKey2)) {
            ERR("FUZZ FAIL: Non-existent key found! %s", "");
            assert(NULL);
        }

        printf("    type=%d count=%zu ints=%d strings=%d: OK\n",
               multimapType_(m), multimapCount(m), numInts, numStrings);
    }

    TEST("FUZZ: binary search correctness - type transitions") {
        /* Verify binary search works correctly during Small→Medium→Full */
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_64);

        cleanerName(zfree, Int64) int64_t *oracle =
            zcalloc(5000, sizeof(int64_t));
        size_t oracleCount = 0;
        uint64_t seed[2] = {777, 888};

        multimapType lastType = MULTIMAP_TYPE_SMALL;

        for (int32_t i = 0; i < 5000; i++) {
            int64_t keyVal = (int64_t)(xoroshiro128plus(seed) % 100000);
            databox key = databoxNewSigned(keyVal);
            databox val = databoxNewSigned(i);
            const databox *elements[2] = {&key, &val};

            if (!multimapExists(m, &key)) {
                multimapInsert(&m, elements);
                oracle[oracleCount++] = keyVal;
            }

            multimapType curType = multimapType_(m);
            if (curType != lastType) {
                /* Type transition occurred - full verification */
                printf("      Transition at %d: %d -> %d (count=%zu)\n", i,
                       lastType, curType, oracleCount);

                for (size_t j = 0; j < oracleCount; j++) {
                    databox checkKey = databoxNewSigned(oracle[j]);
                    if (!multimapExists(m, &checkKey)) {
                        ERR("FUZZ FAIL: Key %" PRId64 " lost during %d->%d "
                            "transition!",
                            oracle[j], lastType, curType);
                        assert(NULL);
                    }
                }
                lastType = curType;
            }
        }

        /* Final verification */
        for (size_t j = 0; j < oracleCount; j++) {
            databox checkKey = databoxNewSigned(oracle[j]);
            if (!multimapExists(m, &checkKey)) {
                ERR("FUZZ FAIL: Final check - key %" PRId64 " missing!",
                    oracle[j]);
                assert(NULL);
            }
        }

        printf("    final_type=%d count=%zu: OK\n", multimapType_(m),
               oracleCount);
    }

    TEST("FUZZ: binary search correctness - duplicate keys (full width)") {
        /* Same key with different values - stresses full-width search */
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_256);

        const int32_t numUniqueKeys = 50;
        const int32_t valsPerKey = 100;

        /* Insert many values for each key */
        for (int32_t k = 0; k < numUniqueKeys; k++) {
            for (int32_t v = 0; v < valsPerKey; v++) {
                databox key = databoxNewSigned(k * 1000);
                databox val = databoxNewSigned(k * 10000 + v);
                const databox *elements[2] = {&key, &val};
                multimapInsertFullWidth(&m, elements);
            }

            /* Verify all values for this key exist */
            for (int32_t v = 0; v < valsPerKey; v++) {
                databox key = databoxNewSigned(k * 1000);
                databox val = databoxNewSigned(k * 10000 + v);
                const databox *elements[2] = {&key, &val};
                if (!multimapExistsFullWidth(m, elements)) {
                    ERR("FUZZ FAIL: FullWidth key=%d val=%d not found!",
                        k * 1000, k * 10000 + v);
                    assert(NULL);
                }
            }
        }

        /* Verify total count */
        size_t expected = (size_t)numUniqueKeys * valsPerKey;
        if (multimapCount(m) != expected) {
            ERR("FUZZ FAIL: FullWidth count %zu != expected %zu!",
                multimapCount(m), expected);
            assert(NULL);
        }

        printf("    type=%d count=%zu keys=%d vals_per=%d: OK\n",
               multimapType_(m), multimapCount(m), numUniqueKeys, valsPerKey);
    }

    TEST("FUZZ: binary search correctness - string keys comprehensive") {
        /* String keys with various lengths and content */
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_256);

        /* Store all inserted strings for verification */
        cleanerName(zfree, CharPtrs) char **oracle =
            zcalloc(2000, sizeof(char *));
        size_t oracleCount = 0;

        uint64_t seed[2] = {12345, 67890};

        /* Various string patterns */
        const char *prefixes[] = {"", "a", "aa", "aaa", "b", "ab", "ba", "z"};
        size_t numPrefixes = sizeof(prefixes) / sizeof(prefixes[0]);

        for (int32_t i = 0; i < 1000; i++) {
            char buf[128];
            size_t prefixIdx = xoroshiro128plus(seed) % numPrefixes;
            int32_t suffix = (int32_t)(xoroshiro128plus(seed) % 10000);
            snprintf(buf, sizeof(buf), "%s%05d", prefixes[prefixIdx], suffix);

            databox key = databoxNewBytesString(buf);
            databox val = databoxNewSigned(i);
            const databox *elements[2] = {&key, &val};

            if (!multimapExists(m, &key)) {
                multimapInsert(&m, elements);
                size_t len = strlen(buf) + 1;
                /* @own: container; free=zfree */
                oracle[oracleCount] = zmalloc(len);
                memcpy(oracle[oracleCount], buf, len);
                oracleCount++;
            }
        }

        /* Verify all strings */
        for (size_t j = 0; j < oracleCount; j++) {
            databox key = databoxNewBytesString(oracle[j]);
            if (!multimapExists(m, &key)) {
                ERR("FUZZ FAIL: String key '%s' not found!", oracle[j]);
                assert(NULL);
            }
        }

        /* Free the element strings; the array itself is scope-cleaned. */
        for (size_t j = 0; j < oracleCount; j++) {
            zfree(oracle[j]);
        }

        printf("    type=%d count=%zu: OK\n", multimapType_(m), oracleCount);
    }

    TEST("FUZZ: rangeBox consistency in Full maps") {
        /* Verify rangeBox correctly reflects first key of each map */
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_64);

        /* Insert enough to create Full map with multiple internal maps */
        for (int32_t i = 0; i < 3000; i++) {
            databox key = databoxNewSigned(i);
            databox val = databoxNewSigned(i * 10);
            const databox *elements[2] = {&key, &val};
            multimapInsert(&m, elements);
        }

        if (multimapType_(m) != MULTIMAP_TYPE_FULL) {
            printf("    Skipped (didn't reach Full type)\n");
        } else {
            /* Verify all entries can be found via iteration AND lookup */
            multimapIterator iter;
            multimapIteratorInit(m, &iter, true);
            databox iterKey = {{0}};
            databox iterVal = {{0}};
            databox *iterElements[2] = {&iterKey, &iterVal};
            int32_t count = 0;
            intmax_t prevKey = INTMAX_MIN;

            while (multimapIteratorNext(&iter, iterElements)) {
                /* Verify sorted order */
                if (iterKey.data.i < prevKey) {
                    ERR("FUZZ FAIL: Iterator out of order! %" PRIdMAX
                        " < %" PRIdMAX,
                        iterKey.data.i, prevKey);
                    assert(NULL);
                }
                prevKey = iterKey.data.i;

                /* Verify lookup works for this key */
                if (!multimapExists(m, &iterKey)) {
                    ERR("FUZZ FAIL: Iterator key %" PRIdMAX
                        " not found via lookup!",
                        iterKey.data.i);
                    assert(NULL);
                }
                count++;
            }

            if (count != 3000) {
                ERR("FUZZ FAIL: Iterator count %d != 3000!", count);
                assert(NULL);
            }

            printf("    type=FULL count=%d verified_order=OK: OK\n", count);
        }
    }

    TEST("FUZZ: stress test - massive random operations") {
        /* High-volume random insert/delete/lookup */
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_256);

        /* Use bitmap as lightweight oracle for existence */
        const size_t keySpace = 100000;
        cleanerName(zfree, Bytes) uint8_t *exists =
            zcalloc((keySpace + 7) / 8, 1);
        size_t existCount = 0;

#define BIT_SET(arr, idx) ((arr)[(idx) / 8] |= (1 << ((idx) % 8)))
#define BIT_CLR(arr, idx) ((arr)[(idx) / 8] &= ~(1 << ((idx) % 8)))
#define BIT_GET(arr, idx) (((arr)[(idx) / 8] >> ((idx) % 8)) & 1)

        uint64_t seed[2] = {0xABCDEF, 0x123456};
        int insertOps = 0, deleteOps = 0, lookupOps = 0;

        for (int32_t round = 0; round < 50000; round++) {
            uint64_t op = xoroshiro128plus(seed) % 10;
            size_t keyIdx = xoroshiro128plus(seed) % keySpace;
            databox key = databoxNewSigned((int64_t)keyIdx);

            if (op < 5) {
                /* Insert (50%) */
                databox val = databoxNewSigned((int64_t)round);
                const databox *elements[2] = {&key, &val};
                if (!BIT_GET(exists, keyIdx)) {
                    multimapInsert(&m, elements);
                    BIT_SET(exists, keyIdx);
                    existCount++;
                }
                insertOps++;
            } else if (op < 8) {
                /* Lookup (30%) */
                bool found = multimapExists(m, &key);
                bool shouldExist = BIT_GET(exists, keyIdx);
                if (found != shouldExist) {
                    ERR("FUZZ FAIL: Lookup mismatch for key %zu! found=%d "
                        "should=%d",
                        keyIdx, found, shouldExist);
                    assert(NULL);
                }
                lookupOps++;
            } else {
                /* Delete (20%) */
                if (BIT_GET(exists, keyIdx)) {
                    bool deleted = multimapDelete(&m, &key);
                    if (!deleted) {
                        ERR("FUZZ FAIL: Delete of existing key %zu failed!",
                            keyIdx);
                        assert(NULL);
                    }
                    BIT_CLR(exists, keyIdx);
                    existCount--;
                }
                deleteOps++;
            }

            /* Periodic count verification */
            if (round % 5000 == 0) {
                if (multimapCount(m) != existCount) {
                    ERR("FUZZ FAIL: Count mismatch! map=%zu oracle=%zu",
                        multimapCount(m), existCount);
                    assert(NULL);
                }
            }
        }

#undef BIT_SET
#undef BIT_CLR
#undef BIT_GET

        printf("    type=%d count=%zu ops(I/D/L)=%d/%d/%d: OK\n",
               multimapType_(m), existCount, insertOps, deleteOps, lookupOps);
    }

    TEST("FUZZ: explicit Small map verification") {
        /* Stay in Small map and verify binary search works correctly */
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_2048);
        assert(multimapType_(m) == MULTIMAP_TYPE_SMALL);

        /* Insert few enough entries to stay in Small */
        const int32_t numKeys = 20;
        uint64_t seed[2] = {111, 222};

        for (int32_t i = 0; i < numKeys; i++) {
            int64_t keyVal = (int64_t)(xoroshiro128plus(seed) % 1000);
            databox key = databoxNewSigned(keyVal);
            databox val = databoxNewSigned(i);
            const databox *elements[2] = {&key, &val};
            if (!multimapExists(m, &key)) {
                multimapInsert(&m, elements);
            }
        }

        /* Verify still Small */
        if (multimapType_(m) != MULTIMAP_TYPE_SMALL) {
            ERR("FUZZ FAIL: Expected Small map, got type %d!",
                multimapType_(m));
            assert(NULL);
        }

        /* Verify all entries findable */
        seed[0] = 111;
        seed[1] = 222;
        for (int32_t i = 0; i < numKeys; i++) {
            int64_t keyVal = (int64_t)(xoroshiro128plus(seed) % 1000);
            databox key = databoxNewSigned(keyVal);
            /* Just verify lookup doesn't crash and returns consistent result */
            multimapExists(m, &key);
        }

        printf("    type=SMALL count=%zu: OK\n", multimapCount(m));
    }

    TEST("FUZZ: explicit Medium map verification") {
        /* Stay in Medium map and verify binary search works correctly */
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_64);

        cleanerName(zfree, Int64) int64_t *oracle =
            zcalloc(100, sizeof(int64_t));
        size_t oracleCount = 0;

        /* Insert enough to transition Small->Medium but not to Full */
        for (int32_t i = 0; i < 50; i++) {
            databox key = databoxNewSigned(i * 2);
            databox val = databoxNewSigned(i);
            const databox *elements[2] = {&key, &val};
            multimapInsert(&m, elements);
            oracle[oracleCount++] = i * 2;

            /* Check if we've reached Medium */
            if (multimapType_(m) == MULTIMAP_TYPE_MEDIUM) {
                /* Verify all keys still findable in Medium */
                for (size_t j = 0; j < oracleCount; j++) {
                    databox checkKey = databoxNewSigned(oracle[j]);
                    if (!multimapExists(m, &checkKey)) {
                        ERR("FUZZ FAIL: Key %" PRId64 " lost in Medium!",
                            oracle[j]);
                        assert(NULL);
                    }
                }
                printf("    Reached Medium at count=%zu: verified\n",
                       oracleCount);
            }

            /* Stop before reaching Full */
            if (multimapType_(m) == MULTIMAP_TYPE_FULL) {
                break;
            }
        }

        /* Final verification if still Medium */
        if (multimapType_(m) == MULTIMAP_TYPE_MEDIUM) {
            for (size_t j = 0; j < oracleCount; j++) {
                databox checkKey = databoxNewSigned(oracle[j]);
                if (!multimapExists(m, &checkKey)) {
                    ERR("FUZZ FAIL: Final Medium check - key %" PRId64
                        " missing!",
                        oracle[j]);
                    assert(NULL);
                }
            }
        }

        printf("    type=%d count=%zu: OK\n", multimapType_(m), oracleCount);
    }

    TEST("FUZZ: explicit Full map with many submaps") {
        /* Create Full map with many internal submaps */
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_64);

        cleanerName(zfree, Int64) int64_t *oracle =
            zcalloc(5000, sizeof(int64_t));
        size_t oracleCount = 0;

        /* Insert to create Full map with splits */
        for (int32_t i = 0; i < 5000; i++) {
            databox key = databoxNewSigned(i);
            databox val = databoxNewSigned(i * 10);
            const databox *elements[2] = {&key, &val};
            multimapInsert(&m, elements);
            oracle[oracleCount++] = i;
        }

        assert(multimapType_(m) == MULTIMAP_TYPE_FULL);

        /* Random access pattern to stress binary search across submaps */
        uint64_t seed[2] = {333, 444};
        for (int32_t round = 0; round < 10000; round++) {
            size_t idx = xoroshiro128plus(seed) % oracleCount;
            databox key = databoxNewSigned(oracle[idx]);
            if (!multimapExists(m, &key)) {
                ERR("FUZZ FAIL: Full random access - key %" PRId64
                    " not found!",
                    oracle[idx]);
                assert(NULL);
            }
        }

        printf("    type=FULL count=%zu random_accesses=10000: OK\n",
               oracleCount);
    }

    TEST("FUZZ: binary search at exact transition boundaries") {
        /* Test binary search works correctly AT the exact transition points */
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_64);

        cleanerName(zfree, Int64) int64_t *oracle =
            zcalloc(200, sizeof(int64_t));
        size_t oracleCount = 0;
        int transitions = 0;
        multimapType prevType = MULTIMAP_TYPE_SMALL;

        for (int32_t i = 0; i < 200; i++) {
            databox key = databoxNewSigned(i);
            databox val = databoxNewSigned(i);
            const databox *elements[2] = {&key, &val};
            multimapInsert(&m, elements);
            oracle[oracleCount++] = i;

            multimapType curType = multimapType_(m);
            if (curType != prevType) {
                transitions++;
                printf("      Transition %d: %s->%s at i=%d\n", transitions,
                       prevType == 1 ? "Small"
                                     : (prevType == 2 ? "Medium" : "Full"),
                       curType == 1 ? "Small"
                                    : (curType == 2 ? "Medium" : "Full"),
                       i);

                /* At transition, verify EVERY key */
                for (size_t j = 0; j < oracleCount; j++) {
                    databox checkKey = databoxNewSigned(oracle[j]);
                    if (!multimapExists(m, &checkKey)) {
                        ERR("FUZZ FAIL: Transition boundary - key %" PRId64
                            " lost!",
                            oracle[j]);
                        assert(NULL);
                    }
                }

                /* Also delete and re-insert at boundary */
                databox boundaryKey = databoxNewSigned(i);
                bool deleted = multimapDelete(&m, &boundaryKey);
                assert(deleted);
                (void)deleted;
                assert(!multimapExists(m, &boundaryKey));
                multimapInsert(&m, elements);
                assert(multimapExists(m, &boundaryKey));

                prevType = curType;
            }
        }

        assert(transitions >=
               2); /* Should see Small->Medium and Medium->Full */
        printf("    transitions=%d final_type=%d count=%zu: OK\n", transitions,
               multimapType_(m), oracleCount);
    }

    TEST("FUZZ: lookup nonexistent keys at all sizes") {
        /* Verify lookups for keys that DON'T exist work at all map sizes */
        for (int limit = 0; limit < 3; limit++) {
            flexCapSizeLimit capLimit = (flexCapSizeLimit[]){
                FLEX_CAP_LEVEL_2048, /* Stay Small longer */
                FLEX_CAP_LEVEL_256,  /* Transition to Medium/Full */
                FLEX_CAP_LEVEL_64    /* Quick Full transition */
            }[limit];

            cleaner(multimapFree) multimap *m = multimapNewLimit(2, capLimit);

            /* Insert even keys only */
            for (int32_t i = 0; i < 500; i += 2) {
                databox key = databoxNewSigned(i);
                databox val = databoxNewSigned(i);
                const databox *elements[2] = {&key, &val};
                multimapInsert(&m, elements);
            }

            /* Lookup odd keys - should all return false */
            for (int32_t i = 1; i < 500; i += 2) {
                databox key = databoxNewSigned(i);
                if (multimapExists(m, &key)) {
                    ERR("FUZZ FAIL: Non-existent key %d found! type=%d", i,
                        multimapType_(m));
                    assert(NULL);
                }
            }

            /* Verify even keys exist */
            for (int32_t i = 0; i < 500; i += 2) {
                databox key = databoxNewSigned(i);
                if (!multimapExists(m, &key)) {
                    ERR("FUZZ FAIL: Existing key %d not found! type=%d", i,
                        multimapType_(m));
                    assert(NULL);
                }
            }

            printf("    capLimit=%d type=%d: OK\n", capLimit, multimapType_(m));
        }
    }

    printf("\n=== All multimap binary search fuzz tests passed! ===\n\n");

    /* ================================================================
     * DIRECT IMPLEMENTATION TESTS - Key Replacement Regression
     * Test each implementation (Small, Medium, Full) directly
     * ================================================================ */

    TEST("DIRECT: multimapSmall key replacement") {
        /* Test Small implementation directly */
        cleaner(multimapSmallFree) multimapSmall *s =
            multimapSmallNew(2, false); /* mapIsSet=false = MAP */

        /* Insert 10 key/value pairs */
        for (int i = 0; i < 10; i++) {
            databox key = databoxNewSigned(i);
            databox val = databoxNewSigned(i * 100);
            const databox *elements[2] = {&key, &val};
            bool replaced = multimapSmallInsert(s, elements);
            assert(!replaced); /* First insert is not replacement */
            (void)replaced;
        }
        assert(multimapSmallCount(s) == 10);

        /* Update all keys - should replace, count stays at 10 */
        for (int i = 0; i < 10; i++) {
            databox key = databoxNewSigned(i);
            databox val = databoxNewSigned(i * 1000); /* Different value */
            const databox *elements[2] = {&key, &val};
            bool replaced = multimapSmallInsert(s, elements);
            if (!replaced) {
                ERR("multimapSmallInsert: key %d not recognized as duplicate!",
                    i);
                assert(replaced);
            }
        }
        assert(multimapSmallCount(s) == 10); /* Count must stay at 10 */

        /* Verify values were updated */
        for (int i = 0; i < 10; i++) {
            databox key = databoxNewSigned(i);
            databox result = {{0}};
            databox *results[1] = {&result};
            bool found = multimapSmallLookup(s, &key, results);
            assert(found);
            (void)found;
            databox expected = databoxNewSigned(i * 1000);
            assert(databoxEqual(&expected, &result));
            (void)expected;
        }
    }

    TEST("DIRECT: multimapMedium key replacement") {
        /* Create Medium by splitting Small */
        /* multimapMediumNewFromOneGrow below consumes this small */
        /* @own: container; free=multimapMediumFree */
        multimapSmall *small = multimapSmallNew(2, false);

        /* Insert enough to make meaningful split */
        for (int i = 0; i < 20; i++) {
            databox key = databoxNewSigned(i);
            databox val = databoxNewSigned(i * 100);
            const databox *elements[2] = {&key, &val};
            multimapSmallInsert(small, elements);
        }

        /* Create Medium from Small */
        multimapMedium *m = multimapMediumNewFromOneGrow(
            small, small->map, small->middle, 2, false);

        /* Update all keys in Medium - should replace */
        for (int i = 0; i < 20; i++) {
            databox key = databoxNewSigned(i);
            databox val = databoxNewSigned(i * 1000); /* Different value */
            const databox *elements[2] = {&key, &val};
            bool replaced = multimapMediumInsert(m, elements);
            if (!replaced) {
                ERR("multimapMediumInsert: key %d not recognized as duplicate!",
                    i);
                assert(replaced);
            }
        }
        assert(multimapMediumCount(m) == 20); /* Count must stay at 20 */

        /* Verify values were updated */
        for (int i = 0; i < 20; i++) {
            databox key = databoxNewSigned(i);
            databox result = {{0}};
            databox *results[1] = {&result};
            bool found = multimapMediumLookup(m, &key, results);
            assert(found);
            (void)found;
            databox expected = databoxNewSigned(i * 1000);
            assert(databoxEqual(&expected, &result));
            (void)expected;
        }

        multimapMediumFree(m);
    }

    TEST("DIRECT: multimapFull key replacement") {
        /* Create Full by creating a Small and growing it */
        /* the Medium then Full conversions below consume this small */
        /* @own: container; free=multimapFullFree */
        multimapSmall *small = multimapSmallNew(2, false);

        /* Insert enough to grow to Full */
        for (int i = 0; i < 100; i++) {
            databox key = databoxNewSigned(i);
            databox val = databoxNewSigned(i * 100);
            const databox *elements[2] = {&key, &val};
            multimapSmallInsert(small, elements);
        }

        /* Convert to Full (via Medium) */
        multimapMedium *medium = multimapMediumNewFromOneGrow(
            small, small->map, small->middle, 2, false);

        flex *maps[2] = {medium->map[0], medium->map[1]};
        multimapFullMiddle middles[2] = {medium->middle[0], medium->middle[1]};
        multimapFull *f =
            multimapFullNewFromTwoGrow(medium, maps, middles, 2, false);

        assert(multimapFullCount(f) == 100);

        /* Update all keys - should replace, count stays at 100 */
        for (int i = 0; i < 100; i++) {
            databox key = databoxNewSigned(i);
            databox val = databoxNewSigned(i * 1000); /* Different value */
            const databox *elements[2] = {&key, &val};
            bool replaced = multimapFullInsert(f, elements);
            if (!replaced) {
                ERR("multimapFullInsert: key %d not recognized as duplicate!",
                    i);
                assert(replaced);
            }
        }
        assert(multimapFullCount(f) == 100); /* Count must stay at 100 */

        /* Verify values were updated */
        for (int i = 0; i < 100; i++) {
            databox key = databoxNewSigned(i);
            databox result = {{0}};
            databox *results[1] = {&result};
            bool found = multimapFullLookup(f, &key, results);
            assert(found);
            (void)found;
            databox expected = databoxNewSigned(i * 1000);
            assert(databoxEqual(&expected, &result));
            (void)expected;
        }

        multimapFullFree(f);
    }

    TEST("DIRECT: multimapFull key replacement with many splits") {
        /* Use multimap wrapper to create Full with many splits */
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_64);

        /* Insert 500 keys - will force Full representation with many splits */
        for (int i = 0; i < 500; i++) {
            databox key = databoxNewSigned(i);
            databox val = databoxNewSigned(i * 100);
            const databox *elements[2] = {&key, &val};
            bool replaced = multimapInsert(&m, elements);
            assert(!replaced);
            (void)replaced;
        }

        /* Verify we're in Full representation */
        assert(multimapType_(m) == MULTIMAP_TYPE_FULL);
        assert(multimapCount(m) == 500);

        /* Update all keys - count must stay at 500 */
        for (int i = 0; i < 500; i++) {
            databox key = databoxNewSigned(i);
            databox val = databoxNewSigned(i * 1000);
            const databox *elements[2] = {&key, &val};
            bool replaced = multimapInsert(&m, elements);
            if (!replaced) {
                ERR("multimapFull (wrapper): key %d not recognized!", i);
                assert(replaced);
            }
        }
        assert(multimapCount(m) == 500);
    }

    TEST("DIRECT: multimapFull key replacement - string keys") {
        /* Use multimap wrapper to create Full with string keys */
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_128);

        /* Insert with string keys - will grow to Full */
        for (int i = 0; i < 200; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "key_%04d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i);
            const databox *elements[2] = {&key, &val};
            bool replaced = multimapInsert(&m, elements);
            assert(!replaced);
            (void)replaced;
        }

        /* Verify in Full and count is correct */
        assert(multimapType_(m) == MULTIMAP_TYPE_FULL);
        assert(multimapCount(m) == 200);

        /* Update all string keys */
        for (int i = 0; i < 200; i++) {
            char keyBuf[32];
            snprintf(keyBuf, sizeof(keyBuf), "key_%04d", i);
            databox key = databoxNewBytesString(keyBuf);
            databox val = databoxNewSigned(i + 10000);
            const databox *elements[2] = {&key, &val};
            bool replaced = multimapInsert(&m, elements);
            if (!replaced) {
                ERR("multimapFull (string): key %s not recognized!", keyBuf);
                assert(replaced);
            }
        }
        assert(multimapCount(m) == 200);
    }

    printf(
        "\n=== All DIRECT implementation key replacement tests passed! ===\n");

    /* ====================================================================
     * Cross-Tier Set Operations Tests
     * These test multimapIntersectKeys, multimapDifferenceKeys, and
     * multimapCopyKeys across Small/Medium/Full tier combinations.
     * Previously these functions had ZERO test coverage!
     * ==================================================================== */
    printf("\n=== Testing Cross-Tier Set Operations ===\n");

    /* Helper to verify a map contains expected value */
#define VERIFY_MAP_CONTAINS(m, val)                                            \
    do {                                                                       \
        databox searchKey = databoxNewSigned(val);                             \
        if (!multimapExists(m, &searchKey)) {                                  \
            ERR("Expected value %" PRId64 " not found in result map!",         \
                (int64_t)(val));                                               \
            assert(false);                                                     \
        }                                                                      \
    } while (0)

#define VERIFY_MAP_NOT_CONTAINS(m, val)                                        \
    do {                                                                       \
        databox searchKey = databoxNewSigned(val);                             \
        if (multimapExists(m, &searchKey)) {                                   \
            ERR("Value %" PRId64 " should NOT be in result map!",              \
                (int64_t)(val));                                               \
            assert(false);                                                     \
        }                                                                      \
    } while (0)

    TEST("multimapIntersectKeys - Small ∩ Small") {
        cleaner(multimapFree) multimap *a =
            multimapNewLimit(1, FLEX_CAP_LEVEL_64);
        cleaner(multimapFree) multimap *b =
            multimapNewLimit(1, FLEX_CAP_LEVEL_64);

        /* A = {1, 2, 3, 4, 5} */
        for (int64_t i = 1; i <= 5; i++) {
            databox v = databoxNewSigned(i);
            const databox *elems[1] = {&v};
            multimapInsert(&a, elems);
        }

        /* B = {3, 4, 5, 6, 7} */
        for (int64_t i = 3; i <= 7; i++) {
            databox v = databoxNewSigned(i);
            const databox *elems[1] = {&v};
            multimapInsert(&b, elems);
        }

        assert(multimapType_(a) == MULTIMAP_TYPE_SMALL);
        assert(multimapType_(b) == MULTIMAP_TYPE_SMALL);

        /* Intersect: A ∩ B should be {3, 4, 5} */
        cleaner(multimapFree) multimap *result =
            multimapNewLimit(1, FLEX_CAP_LEVEL_64);
        multimapIterator ia, ib;
        multimapIteratorInit(a, &ia, true);
        multimapIteratorInit(b, &ib, true);
        multimapIntersectKeys(&result, &ia, &ib);

        assert(multimapCount(result) == 3);
        VERIFY_MAP_CONTAINS(result, 3);
        VERIFY_MAP_CONTAINS(result, 4);
        VERIFY_MAP_CONTAINS(result, 5);
        VERIFY_MAP_NOT_CONTAINS(result, 1);
        VERIFY_MAP_NOT_CONTAINS(result, 2);
        VERIFY_MAP_NOT_CONTAINS(result, 6);
        VERIFY_MAP_NOT_CONTAINS(result, 7);
    }

    TEST("multimapIntersectKeys - Small ∩ Full (cross-tier)") {
        cleaner(multimapFree) multimap *a =
            multimapNewLimit(1, FLEX_CAP_LEVEL_64);
        cleaner(multimapFree) multimap *b =
            multimapNewLimit(1, FLEX_CAP_LEVEL_64);

        /* A = {10, 20, 30, 40, 50} - stays Small */
        int64_t aVals[] = {10, 20, 30, 40, 50};
        for (int i = 0; i < 5; i++) {
            databox v = databoxNewSigned(aVals[i]);
            const databox *elems[1] = {&v};
            multimapInsert(&a, elems);
        }

        /* B = many elements to force Full tier, including 20, 30, 40 */
        for (int64_t i = 0; i < 600; i++) {
            databox v = databoxNewSigned(i);
            const databox *elems[1] = {&v};
            multimapInsert(&b, elems);
        }

        assert(multimapType_(a) == MULTIMAP_TYPE_SMALL);
        assert(multimapType_(b) == MULTIMAP_TYPE_FULL);

        /* Intersect: Should find {10, 20, 30, 40, 50} since all are in B */
        cleaner(multimapFree) multimap *result =
            multimapNewLimit(1, FLEX_CAP_LEVEL_64);
        multimapIterator ia, ib;
        multimapIteratorInit(a, &ia, true);
        multimapIteratorInit(b, &ib, true);
        multimapIntersectKeys(&result, &ia, &ib);

        assert(multimapCount(result) == 5);
        for (int i = 0; i < 5; i++) {
            VERIFY_MAP_CONTAINS(result, aVals[i]);
        }
    }

    TEST("multimapIntersectKeys - Full ∩ Full (both large, partial overlap)") {
        cleaner(multimapFree) multimap *a =
            multimapNewLimit(1, FLEX_CAP_LEVEL_64);
        cleaner(multimapFree) multimap *b =
            multimapNewLimit(1, FLEX_CAP_LEVEL_64);

        /* A = {0..599} */
        for (int64_t i = 0; i < 600; i++) {
            databox v = databoxNewSigned(i);
            const databox *elems[1] = {&v};
            multimapInsert(&a, elems);
        }

        /* B = {300..899} */
        for (int64_t i = 300; i < 900; i++) {
            databox v = databoxNewSigned(i);
            const databox *elems[1] = {&v};
            multimapInsert(&b, elems);
        }

        assert(multimapType_(a) == MULTIMAP_TYPE_FULL);
        assert(multimapType_(b) == MULTIMAP_TYPE_FULL);

        /* Intersect: Should be {300..599} = 300 elements */
        cleaner(multimapFree) multimap *result =
            multimapNewLimit(1, FLEX_CAP_LEVEL_64);
        multimapIterator ia, ib;
        multimapIteratorInit(a, &ia, true);
        multimapIteratorInit(b, &ib, true);
        multimapIntersectKeys(&result, &ia, &ib);

        assert(multimapCount(result) == 300);
        VERIFY_MAP_CONTAINS(result, 300);
        VERIFY_MAP_CONTAINS(result, 450);
        VERIFY_MAP_CONTAINS(result, 599);
        VERIFY_MAP_NOT_CONTAINS(result, 299);
        VERIFY_MAP_NOT_CONTAINS(result, 600);
    }

    TEST("multimapIntersectKeys - disjoint sets (no overlap)") {
        cleaner(multimapFree) multimap *a =
            multimapNewLimit(1, FLEX_CAP_LEVEL_64);
        cleaner(multimapFree) multimap *b =
            multimapNewLimit(1, FLEX_CAP_LEVEL_64);

        /* A = {0..99} */
        for (int64_t i = 0; i < 100; i++) {
            databox v = databoxNewSigned(i);
            const databox *elems[1] = {&v};
            multimapInsert(&a, elems);
        }

        /* B = {1000..1099} */
        for (int64_t i = 1000; i < 1100; i++) {
            databox v = databoxNewSigned(i);
            const databox *elems[1] = {&v};
            multimapInsert(&b, elems);
        }

        /* Intersect: Should be empty */
        cleaner(multimapFree) multimap *result =
            multimapNewLimit(1, FLEX_CAP_LEVEL_64);
        multimapIterator ia, ib;
        multimapIteratorInit(a, &ia, true);
        multimapIteratorInit(b, &ib, true);
        multimapIntersectKeys(&result, &ia, &ib);

        assert(multimapCount(result) == 0);
    }

    TEST("multimapDifferenceKeys - Small \\ Small (basic difference)") {
        cleaner(multimapFree) multimap *a =
            multimapNewLimit(1, FLEX_CAP_LEVEL_64);
        cleaner(multimapFree) multimap *b =
            multimapNewLimit(1, FLEX_CAP_LEVEL_64);

        /* A = {1, 2, 3, 4, 5} */
        for (int64_t i = 1; i <= 5; i++) {
            databox v = databoxNewSigned(i);
            const databox *elems[1] = {&v};
            multimapInsert(&a, elems);
        }

        /* B = {3, 4, 5, 6, 7} */
        for (int64_t i = 3; i <= 7; i++) {
            databox v = databoxNewSigned(i);
            const databox *elems[1] = {&v};
            multimapInsert(&b, elems);
        }

        /* Difference A \ B should be {1, 2} (in A but not B) */
        cleaner(multimapFree) multimap *result =
            multimapNewLimit(1, FLEX_CAP_LEVEL_64);
        multimapIterator ia, ib;
        multimapIteratorInit(a, &ia, true);
        multimapIteratorInit(b, &ib, true);
        multimapDifferenceKeys(&result, &ia, &ib, false);

        assert(multimapCount(result) == 2);
        VERIFY_MAP_CONTAINS(result, 1);
        VERIFY_MAP_CONTAINS(result, 2);
        VERIFY_MAP_NOT_CONTAINS(result, 3);
        VERIFY_MAP_NOT_CONTAINS(result, 6);
    }

    TEST("multimapDifferenceKeys - Full \\ Small (cross-tier, A longer)") {
        cleaner(multimapFree) multimap *a =
            multimapNewLimit(1, FLEX_CAP_LEVEL_64);
        cleaner(multimapFree) multimap *b =
            multimapNewLimit(1, FLEX_CAP_LEVEL_64);

        /* A = {0..599} (Full tier) */
        for (int64_t i = 0; i < 600; i++) {
            databox v = databoxNewSigned(i);
            const databox *elems[1] = {&v};
            multimapInsert(&a, elems);
        }

        /* B = {100, 200, 300} (Small tier) */
        int64_t bVals[] = {100, 200, 300};
        for (int i = 0; i < 3; i++) {
            databox v = databoxNewSigned(bVals[i]);
            const databox *elems[1] = {&v};
            multimapInsert(&b, elems);
        }

        assert(multimapType_(a) == MULTIMAP_TYPE_FULL);
        assert(multimapType_(b) == MULTIMAP_TYPE_SMALL);

        /* Difference A \ B should be 597 elements (600 - 3 shared) */
        cleaner(multimapFree) multimap *result =
            multimapNewLimit(1, FLEX_CAP_LEVEL_64);
        multimapIterator ia, ib;
        multimapIteratorInit(a, &ia, true);
        multimapIteratorInit(b, &ib, true);
        multimapDifferenceKeys(&result, &ia, &ib, false);

        assert(multimapCount(result) == 597);
        VERIFY_MAP_CONTAINS(result, 0);
        VERIFY_MAP_CONTAINS(result, 99);
        VERIFY_MAP_CONTAINS(result, 101);
        VERIFY_MAP_CONTAINS(result, 599);
        VERIFY_MAP_NOT_CONTAINS(result, 100);
        VERIFY_MAP_NOT_CONTAINS(result, 200);
        VERIFY_MAP_NOT_CONTAINS(result, 300);
    }

    TEST("multimapDifferenceKeys - A exhausts before B (remainder handling)") {
        cleaner(multimapFree) multimap *a =
            multimapNewLimit(1, FLEX_CAP_LEVEL_64);
        cleaner(multimapFree) multimap *b =
            multimapNewLimit(1, FLEX_CAP_LEVEL_64);

        /* A = {1, 2, 3} */
        for (int64_t i = 1; i <= 3; i++) {
            databox v = databoxNewSigned(i);
            const databox *elems[1] = {&v};
            multimapInsert(&a, elems);
        }

        /* B = {1, 2, 3, 100, 200, 300, 400} */
        int64_t bVals[] = {1, 2, 3, 100, 200, 300, 400};
        for (int i = 0; i < 7; i++) {
            databox v = databoxNewSigned(bVals[i]);
            const databox *elems[1] = {&v};
            multimapInsert(&b, elems);
        }

        /* A \ B = {} since all of A is in B */
        cleaner(multimapFree) multimap *result =
            multimapNewLimit(1, FLEX_CAP_LEVEL_64);
        multimapIterator ia, ib;
        multimapIteratorInit(a, &ia, true);
        multimapIteratorInit(b, &ib, true);
        multimapDifferenceKeys(&result, &ia, &ib, false);

        assert(multimapCount(result) == 0);
    }

    TEST("multimapCopyKeys - Small into Full (union across tiers)") {
        cleaner(multimapFree) multimap *dst =
            multimapNewLimit(1, FLEX_CAP_LEVEL_64);
        cleaner(multimapFree) multimap *src =
            multimapNewLimit(1, FLEX_CAP_LEVEL_64);

        /* dst = {0..599} (Full tier) */
        for (int64_t i = 0; i < 600; i++) {
            databox v = databoxNewSigned(i);
            const databox *elems[1] = {&v};
            multimapInsert(&dst, elems);
        }

        /* src = {500, 600, 700, 800} - overlaps on 500, new: 600,700,800 */
        int64_t srcVals[] = {500, 600, 700, 800};
        for (int i = 0; i < 4; i++) {
            databox v = databoxNewSigned(srcVals[i]);
            const databox *elems[1] = {&v};
            multimapInsert(&src, elems);
        }

        assert(multimapType_(dst) == MULTIMAP_TYPE_FULL);
        assert(multimapType_(src) == MULTIMAP_TYPE_SMALL);

        size_t countBefore = multimapCount(dst);
        multimapCopyKeys(&dst, src);
        size_t countAfter = multimapCount(dst);

        /* Should add 3 new elements (600, 700, 800) */
        assert(countAfter == countBefore + 3);
        VERIFY_MAP_CONTAINS(dst, 0);
        VERIFY_MAP_CONTAINS(dst, 599);
        VERIFY_MAP_CONTAINS(dst, 600);
        VERIFY_MAP_CONTAINS(dst, 700);
        VERIFY_MAP_CONTAINS(dst, 800);
    }

    TEST("multimapCopyKeys - Full into empty (full copy)") {
        cleaner(multimapFree) multimap *dst =
            multimapNewLimit(1, FLEX_CAP_LEVEL_64);
        cleaner(multimapFree) multimap *src =
            multimapNewLimit(1, FLEX_CAP_LEVEL_64);

        /* src = {0..599} */
        for (int64_t i = 0; i < 600; i++) {
            databox v = databoxNewSigned(i);
            const databox *elems[1] = {&v};
            multimapInsert(&src, elems);
        }

        assert(multimapType_(src) == MULTIMAP_TYPE_FULL);
        assert(multimapCount(dst) == 0);

        multimapCopyKeys(&dst, src);

        assert(multimapCount(dst) == 600);
        VERIFY_MAP_CONTAINS(dst, 0);
        VERIFY_MAP_CONTAINS(dst, 299);
        VERIFY_MAP_CONTAINS(dst, 599);
    }

    TEST("FUZZ: multimapIntersectKeys across random tier combinations") {
        uint64_t seed[2] = {0x1234567890ABCDEFULL, 0xFEDCBA0987654321ULL};

        for (int trial = 0; trial < 20; trial++) {
            cleaner(multimapFree) multimap *a =
                multimapNewLimit(1, FLEX_CAP_LEVEL_64);
            cleaner(multimapFree) multimap *b =
                multimapNewLimit(1, FLEX_CAP_LEVEL_64);

            /* Random size for A: 5-700 elements */
            int64_t sizeA = 5 + (int64_t)(xoroshiro128plus(seed) % 696);
            int64_t startA = (int64_t)(xoroshiro128plus(seed) % 1000);
            for (int64_t i = 0; i < sizeA; i++) {
                databox v = databoxNewSigned(startA + i);
                const databox *elems[1] = {&v};
                multimapInsert(&a, elems);
            }

            /* Random size for B: 5-700 elements */
            int64_t sizeB = 5 + (int64_t)(xoroshiro128plus(seed) % 696);
            int64_t startB = (int64_t)(xoroshiro128plus(seed) % 1000);
            for (int64_t i = 0; i < sizeB; i++) {
                databox v = databoxNewSigned(startB + i);
                const databox *elems[1] = {&v};
                multimapInsert(&b, elems);
            }

            /* Calculate expected intersection */
            int64_t overlapStart = (startA > startB) ? startA : startB;
            int64_t endA = startA + sizeA;
            int64_t endB = startB + sizeB;
            int64_t overlapEnd = (endA < endB) ? endA : endB;
            size_t expectedCount = (overlapEnd > overlapStart)
                                       ? (size_t)(overlapEnd - overlapStart)
                                       : 0;

            cleaner(multimapFree) multimap *result =
                multimapNewLimit(1, FLEX_CAP_LEVEL_64);
            multimapIterator ia, ib;
            multimapIteratorInit(a, &ia, true);
            multimapIteratorInit(b, &ib, true);
            multimapIntersectKeys(&result, &ia, &ib);

            if (multimapCount(result) != expectedCount) {
                ERR("Trial %d: Expected intersection count %zu, got %zu "
                    "(A: [%" PRId64 "..%" PRId64 "], B: [%" PRId64 "..%" PRId64
                    "], overlap: [%" PRId64 "..%" PRId64 "])",
                    trial, expectedCount, multimapCount(result),
                    (int64_t)startA, (int64_t)(startA + sizeA - 1),
                    (int64_t)startB, (int64_t)(startB + sizeB - 1),
                    (int64_t)overlapStart, (int64_t)(overlapEnd - 1));
                assert(false);
            }
        }
    }

    TEST("FUZZ: multimapDifferenceKeys across random tier combinations") {
        uint64_t seed[2] = {0xFEDCBA0987654321ULL, 0x1234567890ABCDEFULL};

        for (int trial = 0; trial < 20; trial++) {
            cleaner(multimapFree) multimap *a =
                multimapNewLimit(1, FLEX_CAP_LEVEL_64);
            cleaner(multimapFree) multimap *b =
                multimapNewLimit(1, FLEX_CAP_LEVEL_64);

            /* Random size for A: 5-700 elements */
            int64_t sizeA = 5 + (int64_t)(xoroshiro128plus(seed) % 696);
            int64_t startA = (int64_t)(xoroshiro128plus(seed) % 1000);
            for (int64_t i = 0; i < sizeA; i++) {
                databox v = databoxNewSigned(startA + i);
                const databox *elems[1] = {&v};
                multimapInsert(&a, elems);
            }

            /* Random size for B: 5-700 elements */
            int64_t sizeB = 5 + (int64_t)(xoroshiro128plus(seed) % 696);
            int64_t startB = (int64_t)(xoroshiro128plus(seed) % 1000);
            for (int64_t i = 0; i < sizeB; i++) {
                databox v = databoxNewSigned(startB + i);
                const databox *elems[1] = {&v};
                multimapInsert(&b, elems);
            }

            /* Calculate expected difference (A \ B) */
            int64_t endA = startA + sizeA;
            int64_t endB = startB + sizeB;
            int64_t overlapStart = (startA > startB) ? startA : startB;
            int64_t overlapEnd = (endA < endB) ? endA : endB;
            size_t overlapCount = (overlapEnd > overlapStart)
                                      ? (size_t)(overlapEnd - overlapStart)
                                      : 0;
            size_t expectedCount = (size_t)sizeA - overlapCount;

            cleaner(multimapFree) multimap *result =
                multimapNewLimit(1, FLEX_CAP_LEVEL_64);
            multimapIterator ia, ib;
            multimapIteratorInit(a, &ia, true);
            multimapIteratorInit(b, &ib, true);
            multimapDifferenceKeys(&result, &ia, &ib, false);

            if (multimapCount(result) != expectedCount) {
                ERR("Trial %d: Expected difference count %zu, got %zu", trial,
                    expectedCount, multimapCount(result));
                assert(false);
            }
        }
    }

    printf("\n=== Cross-Tier Set Operations Tests Passed! ===\n");

#undef VERIFY_MAP_CONTAINS
#undef VERIFY_MAP_NOT_CONTAINS

    TEST("reset then reuse: small tier stays coherent across cycles") {
        /* Dispatcher-level pin for the Small tier (Full and Medium have
         * their own reset-reuse tests in their suites) */
        cleaner(multimapFree) multimap *m = multimapNew(2);

        for (int cycle = 0; cycle < 3; cycle++) {
            for (uint64_t i = 0; i < 40; i++) {
                const databox keybox = databoxNewUnsigned(i);
                const databox valbox = databoxNewUnsigned(i * 7);
                const databox *insert[2] = {&keybox, &valbox};
                multimapInsert(&m, insert);
            }
            if (multimapCount(m) != 40) {
                ERR("cycle %d: expected 40, got %zu", cycle, multimapCount(m));
            }

            multimapReset(m);
            if (multimapCount(m) != 0) {
                ERR("cycle %d: expected 0 after reset, got %zu", cycle,
                    multimapCount(m));
            }
        }

        for (uint64_t i = 0; i < 40; i++) {
            const databox keybox = databoxNewUnsigned(i);
            const databox valbox = databoxNewUnsigned(i + 1);
            const databox *insert[2] = {&keybox, &valbox};
            multimapInsert(&m, insert);
        }
        size_t missing = 0;
        for (uint64_t i = 0; i < 40; i++) {
            const databox probe = databoxNewUnsigned(i);
            if (!multimapExists(m, &probe)) {
                missing++;
            }
        }
        if (missing > 0) {
            ERR("%zu of 40 keys missing after reset-reuse", missing);
        }
    }

    TEST("FUZZ/SOAK: differential ops vs sorted shadow across tiers") {
        /* Deterministic op soak against a sorted unique-key shadow.
         * All checks are behavioral (count, full-iteration order,
         * exists, first/last, predicate-delete correctness,
         * RandomValue liveness) so they catch tier bugs - Medium map0
         * stranding, iterator-at-key gaps, predicate no-ops, Full
         * count desync - without reaching into any tier's internals.
         * A small submap cap makes the map climb Small->Medium->Full
         * quickly; tier coverage is asserted so this can't silently
         * regress into single-tier testing. */
        uint64_t rng = 0x243F6A8885A308D3ULL;
#define SOAK_NEXT() (rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17, rng)
        enum { SHADOW_MAX = 4200, KEY_RANGE = 4000, FILL = 3000 };
        static int64_t shKey[SHADOW_MAX];
        static int64_t shVal[SHADOW_MAX];
        int32_t shCount = 0;
        int64_t valSeq = 1;
        bool tierSeen[4] = {false, false, false, false};
        int32_t soakFail = 0;

        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_64);

        /* Deterministic ascending fill to force promotion through
         * Small -> Medium -> Full before the random churn (churn alone
         * balances population too low to reach Full). */
        for (int32_t i = 0; i < FILL; i++) {
            const databox kb = databoxNewSigned(i);
            const databox vb = databoxNewSigned(valSeq);
            const databox *ins[2] = {&kb, &vb};
            multimapInsert(&m, ins);
            shKey[i] = i;
            shVal[i] = valSeq++;
            tierSeen[multimapType_(m)] = true;
        }
        shCount = FILL;

        for (int32_t op = 0; op < 20000 && soakFail == 0; op++) {
            const uint64_t roll = SOAK_NEXT() % 100;
            const int64_t k = (int64_t)(SOAK_NEXT() % KEY_RANGE);

            /* binary search the shadow for key k */
            int32_t lo = 0;
            int32_t hi = shCount;
            while (lo < hi) {
                const int32_t mid = (lo + hi) / 2;
                if (shKey[mid] < k) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            const bool present = lo < shCount && shKey[lo] == k;

            if (roll < 60 && (present || shCount < SHADOW_MAX)) {
                /* insert or replace */
                const int64_t v = valSeq++;
                const databox kb = databoxNewSigned(k);
                const databox vb = databoxNewSigned(v);
                const databox *ins[2] = {&kb, &vb};
                multimapInsert(&m, ins);
                if (present) {
                    shVal[lo] = v;
                } else {
                    memmove(&shKey[lo + 1], &shKey[lo],
                            (size_t)(shCount - lo) * sizeof(shKey[0]));
                    memmove(&shVal[lo + 1], &shVal[lo],
                            (size_t)(shCount - lo) * sizeof(shVal[0]));
                    shKey[lo] = k;
                    shVal[lo] = v;
                    shCount++;
                }
            } else if (roll < 70 && present) {
                /* delete by key */
                const databox kb = databoxNewSigned(k);
                multimapDelete(&m, &kb);
                memmove(&shKey[lo], &shKey[lo + 1],
                        (size_t)(shCount - lo - 1) * sizeof(shKey[0]));
                memmove(&shVal[lo], &shVal[lo + 1],
                        (size_t)(shCount - lo - 1) * sizeof(shVal[0]));
                shCount--;
            } else if (roll < 76 && shCount > 0) {
                /* delete-by-predicate LESS_THAN_EQUAL; boundary sits in
                 * the low prefix (so population still climbs to Full)
                 * and is either an existing key (exact) or a value
                 * strictly between keys (non-exact - the case the
                 * Medium/Full predicate bug missed) */
                const int32_t j =
                    (int32_t)(SOAK_NEXT() %
                              (uint64_t)(shCount < 6 ? shCount : 6));
                const int64_t bound = shKey[j] - (int64_t)(SOAK_NEXT() & 1);
                const multimapPredicate pred = {
                    .condition = MULTIMAP_CONDITION_LESS_THAN_EQUAL,
                    .compareAgainst = databoxNewSigned(bound)};
                multimapDeleteByPredicate(&m, &pred);
                int32_t keep = 0;
                for (int32_t i = 0; i < shCount; i++) {
                    if (shKey[i] > bound) {
                        shKey[keep] = shKey[i];
                        shVal[keep] = shVal[i];
                        keep++;
                    }
                }
                shCount = keep;
            } else if (roll < 86 && shCount > 0) {
                /* RandomValue must not crash on a single-sided-empty
                 * Medium and must return a live key */
                databox got[2];
                databox *gp[2] = {&got[0], &got[1]};
                multimapEntry rme;
                if (multimapRandomValue(m, (SOAK_NEXT() & 1) != 0, gp, &rme)) {
                    const int64_t rk = got[0].data.i;
                    int32_t a = 0;
                    int32_t b = shCount;
                    while (a < b) {
                        const int32_t mid = (a + b) / 2;
                        if (shKey[mid] < rk) {
                            a = mid + 1;
                        } else {
                            b = mid;
                        }
                    }
                    if (!(a < shCount && shKey[a] == rk)) {
                        ERR("op %d: RandomValue returned absent key %" PRId64,
                            op, (int64_t)rk);
                        soakFail++;
                    }
                }
            } else {
                /* exists probe (present and absent) */
                const databox kb = databoxNewSigned(k);
                if (multimapExists(m, &kb) != present) {
                    ERR("op %d: exists(%" PRId64 ")=%d != shadow %d", op,
                        (int64_t)k, !present, present);
                    soakFail++;
                }
            }

            tierSeen[multimapType_(m)] = true;

            /* per-op: count matches shadow */
            if ((int32_t)multimapCount(m) != shCount) {
                ERR("op %d roll %d: count %zu != shadow %d", op, (int)roll,
                    multimapCount(m), shCount);
                soakFail++;
            }

            /* per-op: first/last match shadow ends (catches Medium
             * map0-stranding, which makes First report empty) */
            if (shCount > 0 && !soakFail) {
                databox fe[2];
                databox le[2];
                if (!multimapFirstInto(m, fe, 2) || fe[0].data.i != shKey[0]) {
                    ERR("op %d: first key %" PRId64 " != shadow %" PRId64, op,
                        (int64_t)fe[0].data.i, (int64_t)shKey[0]);
                    soakFail++;
                }
                if (!multimapLastInto(m, le, 2) ||
                    le[0].data.i != shKey[shCount - 1]) {
                    ERR("op %d: last key %" PRId64 " != shadow %" PRId64, op,
                        (int64_t)le[0].data.i, (int64_t)shKey[shCount - 1]);
                    soakFail++;
                }
            }

            /* full forward-iteration order + value check every 200 ops
             * (also catches stranded/unreachable data) */
            if ((op % 200 == 0 || soakFail) && !soakFail) {
                multimapIterator iter;
                multimapIteratorInit(m, &iter, true);
                databox row[2];
                int32_t seen = 0;
                while (multimapIteratorNextInto(&iter, row, 2)) {
                    if (seen >= shCount) {
                        ERR("op %d: iterator yielded > %d rows", op, shCount);
                        soakFail++;
                        break;
                    }
                    if (row[0].data.i != shKey[seen] ||
                        row[1].data.i != shVal[seen]) {
                        ERR("op %d: row %d (%" PRId64 ",%" PRId64
                            ") != shadow (%" PRId64 ",%" PRId64 ")",
                            op, seen, (int64_t)row[0].data.i,
                            (int64_t)row[1].data.i, (int64_t)shKey[seen],
                            (int64_t)shVal[seen]);
                        soakFail++;
                        break;
                    }
                    seen++;
                }
                if (!soakFail && seen != shCount) {
                    ERR("op %d: iterator yielded %d rows, shadow %d", op, seen,
                        shCount);
                    soakFail++;
                }
            }
        }

        printf("    soak: finalCount=%d finalTier=%d\n", shCount,
               (int)multimapType_(m));

        if (!tierSeen[MULTIMAP_TYPE_MEDIUM] || !tierSeen[MULTIMAP_TYPE_FULL]) {
            ERR("Soak tier coverage incomplete: medium=%d full=%d",
                tierSeen[MULTIMAP_TYPE_MEDIUM], tierSeen[MULTIMAP_TYPE_FULL]);
        }

#undef SOAK_NEXT
    }

    TEST("random row selection per tier: uniform, fromTail, random() kept") {
        cleaner(multimapSmallFree) multimapSmall *small =
            multimapSmallNew(2, false);
        err += rvSelectionCheck("small", small, rvSmallInsert, rvSmallDraw,
                                rvSmallPosition);

        cleanerName(multimapMediumFree, Sweep) multimapMedium *medium =
            multimapMediumNew(2);
        err += rvSelectionCheck("medium", medium, rvMediumInsert, rvMediumDraw,
                                rvMediumPosition);

        cleanerName(multimapFullFree, Sweep) multimapFull *full =
            multimapFullNew(2);
        err += rvSelectionCheck("full", full, rvFullInsert, rvFullDraw,
                                rvFullPosition);
        if (multimapFullNodeCount(full) < 2) {
            ERR("full: the container did not split (%zu sub-map)",
                multimapFullNodeCount(full));
        }
    }

    TEST("replacing insert: replacedOut may alias the new value boxes") {
        /* The exchange idiom: new values in, old values out, same boxes. */
        cleaner(multimapSmallFree) multimapSmall *small =
            multimapSmallNew(RP_WIDTH, false);
        cleanerName(multimapMediumFree, Sweep) multimapMedium *medium =
            multimapMediumNew(RP_WIDTH);
        cleanerName(multimapFullFree, Sweep) multimapFull *full =
            multimapFullNew(RP_WIDTH);
        cleaner(multimapFree) multimap *generic = multimapNew(RP_WIDTH);
        struct {
            const char *where;
            rpInsertFn fn;
            void *m;
        } layers[] = {
            {"small", rpSmall, small},
            {"medium", rpMedium, medium},
            {"full", rpFull, full},
            {"generic", rpGeneric, &generic},
        };

        for (size_t l = 0; l < COUNT_ARRAY(layers); l++) {
            const char *where = layers[l].where;
            for (uint64_t key = 0; key < 8; key++) {
                const databox k = databoxNewUnsigned(key);
                databox v[RP_VALUES] = {databoxNewUnsigned(100 + key),
                                        databoxNewPtr(RP_PTR_TAG(key))};
                const databox *e[RP_WIDTH] = {&k, &v[0], &v[1]};
                if (layers[l].fn(layers[l].m, e, v)) {
                    ERR("%s alias: fresh key %" PRIu64 " reported a replace",
                        where, key);
                }

                v[0] = databoxNewUnsigned(200 + key);
                v[1] = databoxNewPtr(RP_PTR_TAG(key + 1000));
                if (!layers[l].fn(layers[l].m, e, v)) {
                    ERR("%s alias: key %" PRIu64 " was not replaced", where,
                        key);
                }

                if (v[0].type != DATABOX_UNSIGNED_64 ||
                    v[0].data.u != 100 + key ||
                    v[1].data.u != (uintptr_t)RP_PTR_TAG(key)) {
                    ERR("%s alias: key %" PRIu64 " reported (%" PRIu64
                        ", 0x%" PRIx64 "), expected (%" PRIu64 ", 0x%" PRIx64
                        ")",
                        where, key, (uint64_t)v[0].data.u,
                        (uint64_t)v[1].data.u, 100 + key,
                        (uint64_t)(uintptr_t)RP_PTR_TAG(key));
                }

                /* Read back what the exchange stored. */
                const databox again = databoxNewUnsigned(300 + key);
                const databox tag = databoxNewPtr(RP_PTR_TAG(key + 2000));
                const databox *probe[RP_WIDTH] = {&k, &again, &tag};
                databox stored[RP_VALUES];
                layers[l].fn(layers[l].m, probe, stored);
                if (stored[0].type != DATABOX_UNSIGNED_64 ||
                    stored[0].data.u != 200 + key ||
                    stored[1].data.u != (uintptr_t)RP_PTR_TAG(key + 1000)) {
                    ERR("%s alias: key %" PRIu64 " stored (%" PRIu64
                        ", 0x%" PRIx64 "), expected (%" PRIu64 ", 0x%" PRIx64
                        ")",
                        where, key, (uint64_t)stored[0].data.u,
                        (uint64_t)stored[1].data.u, 200 + key,
                        (uint64_t)(uintptr_t)RP_PTR_TAG(key + 1000));
                }
            }
        }
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
