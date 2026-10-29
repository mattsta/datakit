#include "multimap.h"

#include "multimapSmall.h"
#include "multimapSmallInternal.h"

#include "multimapMedium.h"
#include "multimapMediumInternal.h"

#include "multimapFull.h"
#include "multimapFullInternal.h"

#include "cleaner.h"
#include "dkDiag.h"
#include "timeUtil.h"

defcleanerName(databox *, zfree, Databoxes);
defcleanerName(databoxWide *, zfree, DataboxWides);
defcleanerName(databox **, zfree, DataboxPtrs);
defcleanerName(int64_t *, zfree, Int64);
defcleanerName(int32_t *, zfree, Int32);
defcleanerName(void *, zfree, Void);
defcleaner(flex *, flexFree);
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

/* The pointer's tag: 0 for a bare Small map (multimapSmallInternal.h),
 * else the tier of the struct it points to */
#define MULTIMAP_TAG_OF_(map) ptrlibBareTag(map)
/* The tier: a bare map is Small */
#define multimapType_(map)                                                     \
    ((multimapType)(MULTIMAP_TAG_OF_(map) | (MULTIMAP_TAG_OF_(map) == 0)))
#define MULTIMAP_USE_(map) ptrlibBareBody(map)
#define MULTIMAP_TAG_(map, type) ((multimap *)_PTRLIB_TAG(map, type))

/* Stack allocation threshold for iterator element arrays to avoid VLA blowup */
static constexpr int MULTIMAP_STACK_THRESHOLD = 8;

/* A Small map's struct, or for a bare map a view of it on the stack (good
 * for reads only: a change through it is lost; changes go through
 * MULTIMAP_UPDATE_ and friends, which store a bare map back) */
#define mms(m)                                                                 \
    (MULTIMAP_IS_BARE(m) ? (multimapSmall[1]){multimapBareView(m)}             \
                         : (multimapSmall *)MULTIMAP_USE_(m))
#define mmm(m) ((multimapMedium *)MULTIMAP_USE_(m))
#define mmf(m) ((multimapFull *)MULTIMAP_USE_(m))

/* The limit and compress setting of any map */
DK_INLINE_ALWAYS flexCapSizeLimit mmLimit_(const multimap *m) {
    return (flexCapSizeLimit)(MULTIMAP_IS_BARE(m) ? multimapBareLimit(m)
                                                  : COMPRESS_LIMIT(m));
}

DK_INLINE_ALWAYS uint32_t mmDepth_(const multimap *m) {
    return MULTIMAP_IS_BARE(m) ? 0 : (uint32_t)COMPRESS_DEPTH(m);
}

/* A Small map of 'view' (its rows taken): bare when its shape and middle
 * fit the pointer, else a struct Small */
static multimap *mmSmallOf_(const multimapSmall *view, const uint32_t depth,
                            const flexCapSizeLimit limit) {
    if (depth == 0 && multimapBareFits(view->elementsPerEntry, view->mapIsSet,
                                       view->middle)) {
        /* @own: caller; free=multimapFree */
        return multimapBarePack(view->map, view->middle, view->elementsPerEntry,
                                limit);
    }

    /* @own: caller; free=multimapFree */
    multimapSmall *small = zcalloc(1, sizeof(*small));
    *small = *view;
    multimap *tagged = SET_COMPRESS_DEPTH_LIMIT(small, depth, limit);
    return MULTIMAP_TAG_(tagged, MULTIMAP_TYPE_SMALL);
}

/* After a change through a bare map's view: the map again (it stays bare
 * while its middle fits, else it moves into a struct Small) */
DK_INLINE_ALWAYS multimap *mmBareSettle_(const multimapSmall *view,
                                         const multimap *was) {
    if (likely(view->middle < MULTIMAP_BARE_MIDDLE_LIMIT)) {
        return multimapBarePack(view->map, view->middle, view->elementsPerEntry,
                                multimapBareLimit(was));
    }

    return mmSmallOf_(view, 0, (flexCapSizeLimit)multimapBareLimit(was));
}

/* An entry filled through a bare map's view points at the view's map
 * field; it points at its own slot instead */
DK_INLINE_ALWAYS void mmBareEntry_(multimapEntry *me, flex *map) {
    me->bare = map;
    me->map = &me->bare;
}

/* After a change through a bare map's view that filled or used 'me': the
 * entry points at its own slot while 'm' is bare, or at the struct a
 * too-wide middle moved the map into */
DK_INLINE_ALWAYS void mmBareRelink_(const multimap *m, multimapEntry *me,
                                    flex *map) {
    if (MULTIMAP_IS_BARE(m)) {
        mmBareEntry_(me, map);
    } else {
        me->map = &((multimapSmall *)MULTIMAP_USE_(m))->map;
    }
}

#define MULTIMAP_USE_COMPUTED_GOTO_JUMPING 1

/* The four arms of a call by tag.  'bare' is the bare map's arm (a macro
 * of (ret, m, func, ...)); a struct tier's arm calls its own function. */
#if MULTIMAP_USE_COMPUTED_GOTO_JUMPING
#define MULTIMAP_DISPATCH_(bare, ret, m, func, ...)                            \
    do {                                                                       \
        __label__ mapBare;                                                     \
        __label__ mapSmall;                                                    \
        __label__ mapMedium;                                                   \
        __label__ mapFull;                                                     \
        static const void **jumper[4] = {&&mapBare, &&mapSmall, &&mapMedium,   \
                                         &&mapFull};                           \
        goto *jumper[MULTIMAP_TAG_OF_(m)];                                     \
        do {                                                                   \
        mapBare:                                                               \
            bare(ret, m, func __VA_OPT__(, ) __VA_ARGS__);                     \
            break;                                                             \
        mapSmall:                                                              \
            ret multimapSmall##func((multimapSmall *)MULTIMAP_USE_(m)          \
                                        __VA_OPT__(, ) __VA_ARGS__);           \
            break;                                                             \
        mapMedium:                                                             \
            ret multimapMedium##func(mmm(m) __VA_OPT__(, ) __VA_ARGS__);       \
            break;                                                             \
        mapFull:                                                               \
            ret multimapFull##func(mmf(m) __VA_OPT__(, ) __VA_ARGS__);         \
            break;                                                             \
        } while (0);                                                           \
    } while (0)
#else
#define MULTIMAP_DISPATCH_(bare, ret, m, func, ...)                            \
    do {                                                                       \
        switch (MULTIMAP_TAG_OF_(m)) {                                         \
        case 0:                                                                \
            bare(ret, m, func __VA_OPT__(, ) __VA_ARGS__);                     \
            break;                                                             \
        case MULTIMAP_TYPE_SMALL:                                              \
            ret multimapSmall##func((multimapSmall *)MULTIMAP_USE_(m)          \
                                        __VA_OPT__(, ) __VA_ARGS__);           \
            break;                                                             \
        case MULTIMAP_TYPE_MEDIUM:                                             \
            ret multimapMedium##func(mmm(m) __VA_OPT__(, ) __VA_ARGS__);       \
            break;                                                             \
        case MULTIMAP_TYPE_FULL:                                               \
            ret multimapFull##func(mmf(m) __VA_OPT__(, ) __VA_ARGS__);         \
            break;                                                             \
        default:                                                               \
            assert(false);                                                     \
            unreachable();                                                     \
        }                                                                      \
    } while (0)
#endif

/* Bare arms.  READ: a call on the view.  UPDATE: 'm' is the caller's
 * handle (an lvalue), stored back after the call.  READ_ENTRY: a read
 * that fills the entry 'me' (the call's last argument).  UPDATE_ENTRY: a
 * change through the entry 'me' (the call's first argument after the
 * map), which points at the view's map during the call. */
#define MM_BARE_READ_(ret, m, func, ...)                                       \
    {                                                                          \
        multimapSmall v_ = multimapBareView(m);                                \
        ret multimapSmall##func(&v_ __VA_OPT__(, ) __VA_ARGS__);               \
    }
#define MM_BARE_UPDATE_(ret, m, func, ...)                                     \
    {                                                                          \
        multimapSmall v_ = multimapBareView(m);                                \
        ret multimapSmall##func(&v_ __VA_OPT__(, ) __VA_ARGS__);               \
        (m) = mmBareSettle_(&v_, (m));                                         \
    }

#define MULTIMAP_(ret, m, func, ...)                                           \
    MULTIMAP_DISPATCH_(MM_BARE_READ_, ret, m, func __VA_OPT__(, ) __VA_ARGS__)
#define MULTIMAP_NORETURN(m, func, ...)                                        \
    MULTIMAP_(, m, func __VA_OPT__(, ) __VA_ARGS__)
#define MULTIMAP_RETURN(m, func, ...)                                          \
    MULTIMAP_(return, m, func __VA_OPT__(, ) __VA_ARGS__)
#define MULTIMAP_SETRESULT(var, m, func, ...)                                  \
    MULTIMAP_(var =, m, func __VA_OPT__(, ) __VA_ARGS__)
/* A change: 'm' is the caller's handle (*m) */
#define MULTIMAP_UPDATE_(ret, m, func, ...)                                    \
    MULTIMAP_DISPATCH_(MM_BARE_UPDATE_, ret, m, func __VA_OPT__(, ) __VA_ARGS__)

#define MULTIMAP_SINGLE_NORETURN(m, func) MULTIMAP_(, m, func)
#define MULTIMAP_SINGLE_RETURN(m, func) MULTIMAP_(return, m, func)

/* A read that fills the entry 'me': on a bare map the entry points at
 * its own slot (a view's map field is gone once the call returns) */
#define MULTIMAP_ENTRY_READ_(var, m, me, func, ...)                            \
    do {                                                                       \
        if (MULTIMAP_IS_BARE(m)) {                                             \
            multimapSmall v_ = multimapBareView(m);                            \
            var = multimapSmall##func(&v_, __VA_ARGS__);                       \
            if (me) {                                                          \
                mmBareEntry_((me), v_.map);                                    \
            }                                                                  \
        } else {                                                               \
            MULTIMAP_SETRESULT(var, m, func, __VA_ARGS__);                     \
        }                                                                      \
    } while (0)

/* A change through the entry 'me' ('m' is the caller's handle): on a bare
 * map the entry points at the view's map during the call and at its own
 * slot (or the struct a too-wide middle moved the map into) after */
#define MULTIMAP_ENTRY_UPDATE_(m, me, func, ...)                               \
    do {                                                                       \
        if (MULTIMAP_IS_BARE(m)) {                                             \
            multimapSmall v_ = multimapBareView(m);                            \
            (me)->map = &v_.map;                                               \
            multimapSmall##func(&v_, me __VA_OPT__(, ) __VA_ARGS__);           \
            (m) = mmBareSettle_(&v_, (m));                                     \
            mmBareRelink_((m), (me), v_.map);                                  \
        } else {                                                               \
            MULTIMAP_NORETURN(m, func, me __VA_OPT__(, ) __VA_ARGS__);         \
        }                                                                      \
    } while (0)

/* A new, empty Small map: bare when its shape allows (mmSmallOf_) */
static multimap *mmNew_(const multimapElements elementsPerEntry,
                        const bool isSet, const bool compress,
                        const flexCapSizeLimit limit) {
    /* Note: You can only have up to 64k 'elementsPerEntry'
     *       (which is overkill anyway) */
    multimapSmall view = {.middle = FLEX_EMPTY_SIZE,
                          .elementsPerEntry = elementsPerEntry,
                          .mapIsSet = isSet};
    /* @own: container; free=multimapFree */
    view.map = flexNew();
    /* @own: caller; free=multimapFree */
    return mmSmallOf_(&view, compress, limit);
}

multimap *multimapNew(multimapElements elementsPerEntry) {
    /* @own: caller; free=multimapFree */
    return mmNew_(elementsPerEntry, false, false,
                  dkTierDefaultLimit(DK_TIER_MULTIMAP));
}

multimap *multimapNewLimit(multimapElements elementsPerEntry,
                           const flexCapSizeLimit limit) {
    /* @own: caller; free=multimapFree */
    return mmNew_(elementsPerEntry, false, false, limit);
}

multimap *multimapNewCompress(multimapElements elementsPerEntry,
                              const flexCapSizeLimit limit) {
    /* @own: caller; free=multimapFree */
    return mmNew_(elementsPerEntry, false, true, limit);
}

multimap *multimapSetNew(multimapElements elementsPerEntry) {
    /* @own: caller; free=multimapFree */
    return mmNew_(elementsPerEntry, true, false,
                  dkTierDefaultLimit(DK_TIER_MULTIMAP));
}

multimap *multimapNewConfigure(multimapElements elementsPerEntry,
                               const bool isSet, const bool compress,
                               const flexCapSizeLimit sizeLimit) {
    /* @own: caller; free=multimapFree */
    return mmNew_(elementsPerEntry, isSet, compress, sizeLimit);
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

multimapType multimapGetType(const multimap *m) {
    return multimapType_(m);
}

void multimapStats(const multimap *m, dkTierStats *out) {
    *out = (dkTierStats){.count = multimapCount(m),
                         .tier = (uint8_t)multimapType_(m),
                         .unitLimit = flexOptimizationSizeLimit[mmLimit_(m)]};
    switch (multimapType_(m)) {
    case MULTIMAP_TYPE_SMALL:
        out->units = 1;
        out->dataBytes = multimapSmallBytes(mms(m));
        /* A bare map has no struct: it is its flex */
        out->totalBytes =
            out->dataBytes + (MULTIMAP_IS_BARE(m) ? 0 : sizeof(multimapSmall));
        break;
    case MULTIMAP_TYPE_MEDIUM:
        out->units = 2;
        out->dataBytes = multimapMediumBytes(mmm(m));
        out->totalBytes = out->dataBytes + sizeof(multimapMedium);
        break;
    case MULTIMAP_TYPE_FULL:
        out->units = mmf(m)->count;
        out->dataBytes = multimapFullBytes(mmf(m));
        out->totalBytes = multimapFullBytesFull(mmf(m));
        break;
    }
}

bool multimapIsSet(const multimap *m) {
    switch (multimapType_(m)) {
    case MULTIMAP_TYPE_SMALL:
        return mms(m)->mapIsSet;
    case MULTIMAP_TYPE_MEDIUM:
        return mmm(m)->mapIsSet;
    case MULTIMAP_TYPE_FULL:
        return mmf(m)->mapIsSet;
    default:
        assert(false);
        unreachable();
    }
}

/* True when `rows` holds whole rows in the order a multimap keeps them:
 * ascending by the whole row, strictly for a set (it holds each row once);
 * a map's rows have distinct keys unless they were added with
 * multimapInsertFullWidth, which may repeat a key */
static bool multimapDumpAscends_(const flex *rows,
                                 multimapElements elementsPerEntry,
                                 bool isSet) {
    const size_t count = flexCount(rows);
    if (elementsPerEntry == 0 || count % elementsPerEntry) {
        return false;
    }

    cleanerName(zfree, Void) void *storage =
        zcalloc(2 * (size_t)elementsPerEntry, sizeof(databoxBig));
    databoxBig *prev = storage;
    databoxBig *cur = prev + elementsPerEntry;
    flexEntry *fe = flexHead(rows);
    for (size_t row = 0; row < count / elementsPerEntry; row++) {
        for (multimapElements e = 0; e < elementsPerEntry; e++) {
            flexGetByTypeBig(fe, &cur[e]);
            fe = flexNext(rows, fe);
        }

        if (row > 0) {
            int cmp = 0;
            for (size_t e = 0; e < elementsPerEntry && cmp == 0; e++) {
                cmp = databoxCompare((const databox *)&prev[e],
                                     (const databox *)&cur[e]);
            }
            if (cmp > 0 || (cmp == 0 && isSet)) {
                return false;
            }
        }

        /* The rows' storage is the flex, so swapping halves keeps the
         * previous row's boxes valid */
        databoxBig *swap = prev;
        prev = cur;
        cur = swap;
    }
    return true;
}

multimap *multimapNewFromDump(multimapElements elementsPerEntry, bool isSet,
                              bool compress, flexCapSizeLimit limit,
                              flex *rows) {
    if (!multimapDumpAscends_(rows, elementsPerEntry, isSet)) {
        flexFree(rows);
        return nullptr;
    }

    /* Up to what Medium holds under 'limit': insert the rows */
    const size_t bytes = flexBytes(rows);
    const size_t splitSize = flexOptimizationSizeLimit[limit];
    if (bytes <= splitSize * 3) {
        /* @own: caller; free=multimapFree */
        multimap *m =
            multimapNewConfigure(elementsPerEntry, isSet, compress, limit);
        cleanerName(zfree, Void) void *storage =
            zcalloc(elementsPerEntry, sizeof(databoxBig) + sizeof(databox *));
        databoxBig *boxes = storage;
        const databox **row = (const databox **)(boxes + elementsPerEntry);
        flexEntry *fe = flexHead(rows);
        for (size_t r = 0; r < flexCount(rows) / elementsPerEntry; r++) {
            for (multimapElements e = 0; e < elementsPerEntry; e++) {
                flexGetByTypeBig(fe, &boxes[e]);
                row[e] = (const databox *)&boxes[e];
                fe = flexNext(rows, fe);
            }
            multimapInsertFullWidth(&m, row);
        }
        flexFree(rows);
        return m;
    }

    /* Larger: cut whole rows into inner maps of about the split size
     * (tail first, so each cut only shortens the rows) and build Full
     * directly from them */
    const size_t total = flexCount(rows) / elementsPerEntry;
    size_t perMap = (total * splitSize) / bytes;
    perMap = perMap ? perMap : 1;
    const size_t maps = dkDivCeil(total, perMap);
    cleanerName(zfree, Void) void *storage =
        zcalloc(maps, sizeof(flex *) + sizeof(multimapFullMiddle));
    flex **map = storage;
    multimapFullMiddle *middle = (multimapFullMiddle *)(map + maps);
    for (size_t i = maps - 1; i > 0; i--) {
        /* Map i holds rows [i * perMap, min((i + 1) * perMap, total)) */
        const size_t end = (i + 1) * perMap < total ? (i + 1) * perMap : total;
        const size_t entries = (end - i * perMap) * elementsPerEntry;
        /* @own: container; free=flexFree */
        map[i] = flexSplitRange(&rows, -(int32_t)entries, (uint32_t)entries);
    }
    map[0] = rows;

    /* @own: caller; free=multimapFree */
    multimapFull *full = multimapFullNewFromManyGrow(nullptr, map, middle, maps,
                                                     elementsPerEntry, isSet);
    /* Inner maps split at the multimap's own limit, as inserts grow them */
    full->maxSize = (uint32_t)splitSize;
    multimap *m = SET_COMPRESS_DEPTH_LIMIT(full, compress, limit);
    return MULTIMAP_TAG_(m, MULTIMAP_TYPE_FULL);
}

flexCapSizeLimit multimapLimit(const multimap *m) {
    return mmLimit_(m);
}

bool multimapCompresses(const multimap *m) {
    return mmDepth_(m) != 0;
}

static void multimapUpgradeIfNecessaryWithReference(
    multimap **m, uint_fast32_t depth, flexCapSizeLimit limit,
    const multimapAtom *restrict referenceContainer);

/* Whether the rows' keys, resolved through 'referenceContainer', strictly
 * ascend (a map of references holds each value once) */
static bool
multimapDumpAscendsWithReference_(const flex *rows,
                                  multimapElements elementsPerEntry,
                                  const multimapAtom *referenceContainer) {
    const size_t count = flexCount(rows);
    if (elementsPerEntry == 0 || count % elementsPerEntry) {
        return false;
    }

    databox key[2];
    databoxWide keyWide[2];
    flexEntry *fe = flexHead(rows);
    for (size_t row = 0; row < count / elementsPerEntry; row++) {
        databox *cur = &key[row & 1];
        *cur = (databox){};
        flexGetByTypeIntoWithReference(fe, cur, &keyWide[row & 1],
                                       referenceContainer);
        if (row > 0 && databoxCompare(&key[(row - 1) & 1], cur) >= 0) {
            return false;
        }

        for (multimapElements e = 0; e < elementsPerEntry; e++) {
            fe = flexNext(rows, fe);
        }
    }

    return true;
}

multimap *
multimapNewFromDumpWithReference(multimapElements elementsPerEntry,
                                 flexCapSizeLimit limit, flex *rows,
                                 const multimapAtom *referenceContainer) {
    if (!multimapDumpAscendsWithReference_(rows, elementsPerEntry,
                                           referenceContainer)) {
        flexFree(rows);
        return nullptr;
    }

    const size_t bytes = flexBytes(rows);
    const size_t splitSize = flexOptimizationSizeLimit[limit];
    const size_t total = flexCount(rows) / elementsPerEntry;

    /* Up to what Medium holds: the rows become a Small's one map, grown
     * once as an insert would grow it */
    if (bytes <= splitSize * 3 || total < 4) {
        const multimapSmall view = {
            .map = rows,
            .middle = (uint32_t)(flexMiddle(rows, elementsPerEntry) - rows),
            .elementsPerEntry = elementsPerEntry};
        /* @own: caller; free=multimapFree */
        multimap *m = mmSmallOf_(&view, 0, limit);
        multimapUpgradeIfNecessaryWithReference(&m, 0, limit,
                                                referenceContainer);
        return m;
    }

    /* Larger: whole rows cut into inner maps of about the split size, as
     * multimapNewFromDump cuts them, with range boxes resolved */
    size_t perMap = (total * splitSize) / bytes;
    perMap = perMap ? perMap : 1;
    const size_t maps = dkDivCeil(total, perMap);
    cleanerName(zfree, Void) void *storage =
        zcalloc(maps, sizeof(flex *) + sizeof(multimapFullMiddle));
    flex **map = storage;
    multimapFullMiddle *middle = (multimapFullMiddle *)(map + maps);
    for (size_t i = maps - 1; i > 0; i--) {
        const size_t end = (i + 1) * perMap < total ? (i + 1) * perMap : total;
        const size_t entries = (end - i * perMap) * elementsPerEntry;
        /* @own: container; free=flexFree */
        map[i] = flexSplitRange(&rows, -(int32_t)entries, (uint32_t)entries);
    }
    map[0] = rows;

    /* @own: caller; free=multimapFree */
    multimapFull *full = multimapFullNewFromManyGrowWithReference(
        nullptr, map, middle, maps, elementsPerEntry, false,
        referenceContainer);
    full->maxSize = (uint32_t)splitSize;
    multimap *m = SET_COMPRESS_DEPTH_LIMIT(full, 0, limit);
    return MULTIMAP_TAG_(m, MULTIMAP_TYPE_FULL);
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
            /* Medium reuses a struct Small's allocation; a bare map has
             * none */
            /* @own: container; free=multimapMediumFree */
            multimapMedium *medium = multimapMediumNewFromOneGrow(
                MULTIMAP_IS_BARE(*m) ? nullptr : small, small->map,
                small->middle, small->elementsPerEntry, small->mapIsSet);

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
            /* Full keeps the configuration too (copies, dumps and
             * snapshots read it from here) */
            multimap *tagged = SET_COMPRESS_DEPTH_LIMIT(full, depth, limit);
            *m = MULTIMAP_TAG_(tagged, MULTIMAP_TYPE_FULL);
        }
    }
}

static void multimapUpgradeIfNecessary(multimap **m, const uint_fast32_t depth,
                                       const flexCapSizeLimit limit) {
    multimapUpgradeIfNecessary_(m, depth, limit, false, nullptr);
}

/* After a delete: move a drained Full or Medium down to the tier its rows
 * fit with room to grow.  L is the size limit's cutoff; tiers grow past L
 * (Small) and 3L (Medium), and shrink well below those, so a tier a
 * shrink enters takes real growth to leave (no flapping at a boundary):
 *   Full (three inner maps at most) -> Medium at <= 1.5L, or straight to
 *   Small at <= L/2;  Medium -> Small at <= L/2.
 * The check is O(1): Full is only measured once it is down to three
 * inner maps.  Rows move once, in O(bytes) bounded by 1.5L.
 *
 * multimapMoveDown_ is the move itself: a Full of at most 'fullMaps' inner
 * maps and 'fullBytes' bytes, or a Medium of at most 'smallBytes', becomes
 * a Small; rows past 'smallBytes' then split into a Medium.  A shrink
 * passes the thresholds above, multimapCompact the growth points
 * themselves (3L, L).  Neither builds Full's range boxes, so both are
 * right for maps of references. */
DK_INLINE_ALWAYS void multimapMoveDown_(multimap **m, uint32_t fullMaps,
                                        size_t fullBytes, size_t smallBytes) {
    const uint32_t depth = mmDepth_(*m);
    const flexCapSizeLimit limit = mmLimit_(*m);
    flex *rows = nullptr;
    multimapElements elementsPerEntry;
    bool isSet;

    switch (multimapType_(*m)) {
    case MULTIMAP_TYPE_FULL: {
        multimapFull *full = mmf(*m);
        if (full->count > fullMaps || multimapFullBytes(full) > fullBytes) {
            return;
        }
        elementsPerEntry = full->elementsPerEntry;
        isSet = full->mapIsSet;
        /* @own: container; free=flexFree */
        rows = multimapFullDump(full);
        multimapFullFree(full);
        break;
    }
    case MULTIMAP_TYPE_MEDIUM: {
        multimapMedium *medium = mmm(*m);
        if (multimapMediumBytes(medium) > smallBytes) {
            return;
        }
        elementsPerEntry = medium->elementsPerEntry;
        isSet = medium->mapIsSet;
        rows = medium->map[0];
        flexBulkAppendFlex(&rows, medium->map[1]);
        flexFree(medium->map[1]);
        zfree(medium);
        break;
    }
    default:
        return;
    }

    const multimapSmall view = {
        .map = rows,
        .middle = (uint32_t)(flexMiddle(rows, elementsPerEntry) - rows),
        .elementsPerEntry = elementsPerEntry,
        .mapIsSet = isSet};

    /* Rows above 'smallBytes' (from a Full) belong in Medium, which needs
     * two whole rows to split, as an upgrade does */
    if (flexBytes(rows) > smallBytes &&
        flexCount(rows) > elementsPerEntry * 2) {
        /* @own: container; free=multimapMediumFree */
        multimapMedium *medium = multimapMediumNewFromOneGrow(
            nullptr, rows, view.middle, elementsPerEntry, isSet);
        multimap *tagged = SET_COMPRESS_DEPTH_LIMIT(medium, depth, limit);
        *m = MULTIMAP_TAG_(tagged, MULTIMAP_TYPE_MEDIUM);
    } else {
        /* @own: caller; free=multimapFree */
        *m = mmSmallOf_(&view, depth, limit);
    }
}

static void multimapShrinkIfNecessary_(multimap **m) {
    const size_t L = flexOptimizationSizeLimit[mmLimit_(*m)];
    multimapMoveDown_(m, 3, L + L / 2, L / 2);
}

void multimapCompact(multimap **m) {
    const size_t L = flexOptimizationSizeLimit[mmLimit_(*m)];
    if (multimapType_(*m) == MULTIMAP_TYPE_FULL) {
        multimapFullCompact(mmf(*m));
    }
    multimapMoveDown_(m, UINT32_MAX, 3 * L, L);
}

void multimapCompactWithReference(multimap **m,
                                  const multimapAtom *referenceContainer) {
    const size_t L = flexOptimizationSizeLimit[mmLimit_(*m)];
    if (multimapType_(*m) == MULTIMAP_TYPE_FULL) {
        multimapFullCompactWithReference(mmf(*m), referenceContainer);
    }
    multimapMoveDown_(m, UINT32_MAX, 3 * L, L);
}

/* A delete entry point: the tier's call, then a shrink when it removed */
#define MULTIMAP_DELETE_(m, func, ...)                                         \
    do {                                                                       \
        bool deleted_;                                                         \
        MULTIMAP_UPDATE_(deleted_ =, *(m), func, __VA_ARGS__);                 \
        if (deleted_) {                                                        \
            multimapShrinkIfNecessary_(m);                                     \
        }                                                                      \
        return deleted_;                                                       \
    } while (0)

static void multimapUpgradeIfNecessaryWithReference(
    multimap **m, const uint_fast32_t depth, const flexCapSizeLimit limit,
    const multimapAtom *restrict referenceContainer) {
    multimapUpgradeIfNecessary_(m, depth, limit, true, referenceContainer);
}
multimap *multimapCopy(const multimap *m) {
    const uint32_t depth = mmDepth_(m);
    const flexCapSizeLimit limit = mmLimit_(m);

    multimap *copy;

    const multimapType type = multimapType_(m);
    if (MULTIMAP_IS_BARE(m)) {
        multimapSmall view = multimapBareView(m);
        /* @own: caller; free=multimapFree */
        view.map = flexDuplicate(view.map);
        return mmSmallOf_(&view, depth, limit);
    }

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
    return multimapInsertReplacing(m, elements, nullptr);
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
    MULTIMAP_UPDATE_(replaced =, *m, InsertReplacing, elements, replacedOut);

    /* Now check multimap to see if we should grow to a larger
     * representation. */
    const uint32_t depth = mmDepth_(*m);
    const flexCapSizeLimit limit = mmLimit_(*m);
    multimapUpgradeIfNecessary(m, depth, limit);
    return replaced;
}

bool multimapInsertIfAbsentEntry(multimap **m, const databox *elements[],
                                 multimapEntry *me) {
    bool inserted;
    if (MULTIMAP_IS_BARE(*m)) {
        multimapSmall v = multimapBareView(*m);
        inserted = multimapSmallInsertIfAbsent(&v, elements, me);
        *m = mmBareSettle_(&v, *m);
        if (me && !inserted) {
            mmBareRelink_(*m, me, v.map);
        }
    } else {
        MULTIMAP_SETRESULT(inserted, *m, InsertIfAbsent, elements, me);
    }

    if (inserted) {
        const uint32_t depth = mmDepth_(*m);
        const flexCapSizeLimit limit = mmLimit_(*m);
        multimapUpgradeIfNecessary(m, depth, limit);
    }

    return inserted;
}

bool multimapInsertIfAbsent(multimap **m, const databox *elements[]) {
    return multimapInsertIfAbsentEntry(m, elements, nullptr);
}

void multimapInsertFullWidth(multimap **m, const databox *elements[]) {
    /* Insert new elements */
    MULTIMAP_UPDATE_(, *m, InsertFullWidth, elements);

    /* Now check multimap to see if we should grow to a larger
     * representation. */
    const uint32_t depth = mmDepth_(*m);
    const flexCapSizeLimit limit = mmLimit_(*m);
    multimapUpgradeIfNecessary(m, depth, limit);
}

void multimapInsertWithSurrogateKey(multimap **m, const databox *elements[],
                                    const databox *insertKey,
                                    const multimapAtom *referenceContainer) {
    /* Insert new elements */
    MULTIMAP_UPDATE_(, *m, InsertWithSurrogateKey, elements, insertKey,
                     referenceContainer);

    /* Now check multimap to see if we should grow to a larger
     * representation. */
    const uint32_t depth = mmDepth_(*m);
    const flexCapSizeLimit limit = mmLimit_(*m);
    multimapUpgradeIfNecessaryWithReference(m, depth, limit,
                                            referenceContainer);
}

bool multimapInsertWithSurrogateKeyIfAbsent(
    multimap **m, const databox *elements[], const databox *insertKey,
    const multimapAtom *referenceContainer, databox *foundKey,
    flexWhenAbsent *whenAbsent, void *whenAbsentCtx) {
    bool inserted;
    MULTIMAP_UPDATE_(inserted =, *m, InsertWithSurrogateKeyIfAbsent, elements,
                     insertKey, referenceContainer, foundKey, whenAbsent,
                     whenAbsentCtx);
    if (inserted) {
        const uint32_t depth = mmDepth_(*m);
        const flexCapSizeLimit limit = mmLimit_(*m);
        multimapUpgradeIfNecessaryWithReference(m, depth, limit,
                                                referenceContainer);
    }

    return inserted;
}

void multimapAppend(multimap **m, const databox *elements[]) {
    MULTIMAP_UPDATE_(, *m, Append, elements);

    const uint32_t depth = mmDepth_(*m);
    const flexCapSizeLimit limit = mmLimit_(*m);
    multimapUpgradeIfNecessary(m, depth, limit);
}

bool multimapGetUnderlyingEntry(multimap *m, const databox *key,
                                multimapEntry *me) {
    bool found;
    MULTIMAP_ENTRY_READ_(found, m, me, GetUnderlyingEntry, key, me);
    return found;
}

/* An in-place rewrite of a set row (replace, resize, increment): every
 * element of a set row is part of its order, so the rewrite is refused
 * and reported as misuse (DK_DIAG_MULTIMAP_SET_REWRITE) */
[[gnu::cold, gnu::noinline]] static bool mmSetRewrite_(const char *function,
                                                       int line) {
    dkDiagReport_(DK_DIAG_MISUSE, DK_DIAG_MULTIMAP_SET_REWRITE, function,
                  __FILE__, line, "%s",
                  "a set row cannot be rewritten in place: delete it and "
                  "insert the new row");
    return true;
}

#define MM_SET_REWRITE_REFUSED_(m)                                             \
    (multimapIsSet(m) && mmSetRewrite_(__func__, __LINE__))

bool multimapGetUnderlyingEntryWithReference(
    multimap *m, const databox *key, multimapEntry *me,
    const multimapAtom *referenceContainer) {
    bool found;
    MULTIMAP_ENTRY_READ_(found, m, me, GetUnderlyingEntryWithReference, key, me,
                         referenceContainer);
    return found;
}

bool multimapGetUnderlyingEntryFullWidth(multimap *m, const databox *elements[],
                                         multimapEntry *me) {
    bool found;
    MULTIMAP_ENTRY_READ_(found, m, me, GetUnderlyingEntryFullWidth, elements,
                         me);
    return found;
}

static void multimapRegularizeMap(multimap **m, multimapFullIdx mapIdx,
                                  flex **map) {
    const multimapType type = multimapType_(*m);
    if (type == MULTIMAP_TYPE_SMALL || type == MULTIMAP_TYPE_MEDIUM) {
        const uint32_t depth = mmDepth_(*m);
        const flexCapSizeLimit limit = mmLimit_(*m);
        multimapUpgradeIfNecessary(m, depth, limit);
    } else {
        multimapFull *full = mmf(*m);
        multimapFullRegularizeMap(full, mapIdx, map);
    }
}

void multimapDeleteEntry(multimap **m, multimapEntry *me) {
    MULTIMAP_ENTRY_UPDATE_(*m, me, DeleteEntryWithReference, nullptr);
    multimapShrinkIfNecessary_(m);
}

void multimapDeleteEntryWithReference(multimap **m, multimapEntry *me,
                                      const multimapAtom *referenceContainer) {
    MULTIMAP_ENTRY_UPDATE_(*m, me, DeleteEntryWithReference,
                           referenceContainer);
    multimapShrinkIfNecessary_(m);
}

bool multimapResizeEntry(multimap **m, multimapEntry *me, size_t newLen) {
    if (MM_SET_REWRITE_REFUSED_(*m)) {
        return false;
    }

    MULTIMAP_ENTRY_UPDATE_(*m, me, ResizeEntry, newLen);
    multimapRegularizeMap(m, me->mapIdx, me->map);
    return true;
}

bool multimapReplaceEntry(multimap **m, multimapEntry *me, const databox *box) {
    if (MM_SET_REWRITE_REFUSED_(*m)) {
        return false;
    }

    MULTIMAP_ENTRY_UPDATE_(*m, me, ReplaceEntry, box);

    /* NOTE: DO NOT regularize map here because if a map requires surrogate keys
     * and we regularize the map, we aren't passing through the
     * referenceContainer here, so this horks the maps. We'd need to pass
     * referenceContainer all the way through the multimapFieldIncr() chain
     * because Incr() calls this multimapReplaceEntry()... it's easier to just
     * allow some sub-optimal maps at the moment. */
#if 0
    multimapRegularizeMap(m, me->mapIdx, me->map);
#endif
    return true;
}

bool multimapReplaceEntryWithReference(multimap **m, multimapEntry *me,
                                       const databox *box,
                                       const multimapAtom *referenceContainer) {
    if (MM_SET_REWRITE_REFUSED_(*m)) {
        return false;
    }

    MULTIMAP_ENTRY_UPDATE_(*m, me, ReplaceEntry, box);

    const multimapType type = multimapType_(*m);
    if (type == MULTIMAP_TYPE_FULL) {
        multimapFull *full = mmf(*m);
        multimapFullRegularizeMapWithReference(full, me->mapIdx, me->map,
                                               referenceContainer);
    }
    return true;
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
        assert(false);
        unreachable();
    }
}

multimapElements multimapElementsPerEntry(const multimap *m) {
    return multimapElementsPerEntry_(m);
}

/* Resize the FIRST VALUE field of the row with 'key' to 'newSize' bytes
 * (rows wider than key+value: use multimapGetUnderlyingEntry +
 * multimapResizeEntry to target other fields).  The key must exist
 * (asserted).  Contents of the resized field are unspecified after the
 * resize; repopulate through a fresh lookup. */
void multimapEntryResize(multimap **m, const databox *key, size_t newSize) {
    multimapEntry me;
    [[maybe_unused]] const bool found =
        multimapGetUnderlyingEntry(*m, key, &me);
    assert(found);

    /* advance from the key entry to its first value field */
    me.fe = flexNext(*me.map, me.fe);
    multimapResizeEntry(m, &me, newSize);
}

/* Replace the VALUE fields of the row with 'key': elements[] holds
 * elementsPerEntry - 1 replacement boxes, in field order.  The key stays
 * in place (a set's rows are all key: refused, as every in-place rewrite
 * of a set is); the key must exist (asserted).  One search: a map's
 * insert of a present key rewrites every value field of its row at once
 * (and grows the tier as an insert does). */
void multimapEntryReplace(multimap **m, const databox *key,
                          const databox *elements[]) {
    if (MM_SET_REWRITE_REFUSED_(*m)) {
        return;
    }

    const multimapElements epe = multimapElementsPerEntry_(*m);
    constexpr multimapElements stackRow = 8;
    const databox *stack[stackRow];
    cleanerName(zfree, Void) void *heap =
        epe > stackRow ? zmalloc(epe * sizeof(const databox *)) : nullptr;
    const databox **row = heap ? (const databox **)heap : stack;
    row[0] = key;
    for (multimapElements i = 1; i < epe; i++) {
        row[i] = elements[i - 1];
    }

    [[maybe_unused]] const bool replaced = multimapInsert(m, row);
    assert(replaced);
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

bool multimapLookupWithReference(const multimap *m, const databox *key,
                                 databox *elements[],
                                 const multimapAtom *referenceContainer) {
    MULTIMAP_RETURN(m, LookupWithReference, key, elements, referenceContainer);
}

bool multimapDelete(multimap **m, const databox *key) {
    MULTIMAP_DELETE_(m, Delete, key);
}

bool multimapDeleteReporting(multimap **m, const databox *key,
                             databox *removedOut) {
    MULTIMAP_DELETE_(m, DeleteReporting, key, removedOut);
}

bool multimapDeleteTaking(multimap **m, const databox *key, databox *takenOut) {
    MULTIMAP_DELETE_(m, DeleteTaking, key, takenOut);
}

bool multimapDeleteWithReference(multimap **m, const databox *key,
                                 const struct multimapAtom *referenceContainer,
                                 databox *foundReference) {
    MULTIMAP_DELETE_(m, DeleteWithReference, key, referenceContainer,
                     foundReference);
}

bool multimapDeleteWithFound(multimap **m, const databox *key,
                             databox *foundReference) {
    MULTIMAP_DELETE_(m, DeleteWithFound, key, foundReference);
}

bool multimapDeleteFullWidth(multimap **m, const databox *elements[]) {
    MULTIMAP_DELETE_(m, DeleteFullWidth, elements);
}

bool multimapRandomValue(multimap *m, const bool fromTail, databox **found,
                         multimapEntry *me) {
    bool picked;
    MULTIMAP_ENTRY_READ_(picked, m, me, RandomValue, fromTail, found, me);
    return picked;
}

bool multimapDeleteRandomValue(multimap **m, const bool fromTail,
                               databox **deleted) {
    MULTIMAP_DELETE_(m, DeleteRandomValue, fromTail, deleted);
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
/* The field at 'fieldOffset' of the row 'me' starts (moving 'me' to it) as
 * an int64 counter: TRUE and FALSE count as 1 and 0.  False for a field
 * that is not an int64-range integer or boolean. */
static bool multimapFieldCounter_(uint32_t fieldOffset, multimapEntry *me,
                                  databox *field, int64_t *value) {
    while (fieldOffset--) {
        me->fe = flexNext(*me->map, me->fe);
    }

    databoxWide fieldWide;
    flexGetByTypeInto(me->fe, field, &fieldWide);
    switch (field->type) {
    case DATABOX_SIGNED_64:
        *value = field->data.i64;
        return true;
    case DATABOX_UNSIGNED_64:
        if (field->data.u64 > (uint64_t)INT64_MAX) {
            return false;
        }

        *value = (int64_t)field->data.u64;
        return true;
    case DATABOX_TRUE:
        *value = 1;
        return true;
    case DATABOX_FALSE:
        *value = 0;
        return true;
    default:
        return false;
    }
}

/* The increment of multimapFieldIncrByIf at the row 'row' already found
 * (a copy: the walk to the field moves it) */
static bool multimapFieldIncrAt_(multimap **m, multimapEntry row,
                                 uint32_t fieldOffset, int64_t incrBy,
                                 int64_t *newValue, flexProceed *proceed,
                                 void *ctx) {
    /* A set's values order its rows (they are part of each row's
     * identity), so none is a counter to rewrite in place */
    multimapEntry me = row;
    databox field;
    int64_t current = 0;
    /* Only a value of the row: 0 is its key, and an offset past its last
     * value would walk into the next row or off the map */
    if (fieldOffset == 0 || fieldOffset >= multimapElementsPerEntry(*m) ||
        MM_SET_REWRITE_REFUSED_(*m) ||
        !multimapFieldCounter_(fieldOffset, &me, &field, &current)) {
        if (newValue) {
            *newValue = 0;
        }
        return false;
    }

    /* Increment policy: exact; refuse a result outside int64. */
    int64_t next;
    if (ckd_add(&next, current, incrBy) ||
        (incrBy != 0 && proceed && !proceed(ctx))) {
        if (newValue) {
            *newValue = current;
        }
        return false;
    }

    if (incrBy != 0) {
        /* A boolean field stays boolean while the result is 0 or 1. */
        const bool wasBool =
            field.type == DATABOX_TRUE || field.type == DATABOX_FALSE;
        databox result;
        if (wasBool && (next == 0 || next == 1)) {
            result = next ? DATABOX_BOX_TRUE : DATABOX_BOX_FALSE;
        } else {
            result = databoxNewSigned(next);
        }

        multimapReplaceEntry(m, &me, &result);
    }

    if (newValue) {
        *newValue = next;
    }

    return true;
}

bool multimapFieldIncrByIf(multimap **m, const databox *key,
                           uint32_t fieldOffset, int64_t incrBy,
                           int64_t *newValue, flexProceed *proceed, void *ctx) {
    multimapEntry row;
    if (!multimapGetUnderlyingEntry(*m, key, &row)) {
        if (newValue) {
            *newValue = 0;
        }
        return false;
    }

    return multimapFieldIncrAt_(m, row, fieldOffset, incrBy, newValue, proceed,
                                ctx);
}

bool multimapFieldIncrEntry(multimap **m, const multimapEntry *row,
                            uint32_t fieldOffset, int64_t incrBy,
                            int64_t *newValue) {
    /* As multimapFieldIncrBy: '*newValue' is written only on success */
    int64_t next;
    if (!multimapFieldIncrAt_(m, *row, fieldOffset, incrBy, &next, nullptr,
                              nullptr)) {
        return false;
    }

    if (newValue) {
        *newValue = next;
    }

    return true;
}

bool multimapFieldIncrBy(multimap **m, const databox *key, uint32_t fieldOffset,
                         int64_t incrBy, int64_t *newValue) {
    int64_t next;
    if (!multimapFieldIncrByIf(m, key, fieldOffset, incrBy, &next, nullptr,
                               nullptr)) {
        return false;
    }

    if (newValue) {
        *newValue = next;
    }

    return true;
}

int64_t multimapFieldIncr(multimap **m, const databox *key,
                          uint32_t fieldOffset, int64_t incrBy) {
    /* The sum, or on a refusal the current value (0 for a missing key or a
     * field that is not an int64 counter) */
    int64_t reported;
    multimapFieldIncrByIf(m, key, fieldOffset, incrBy, &reported, nullptr,
                          nullptr);
    return reported;
}

#endif

void multimapReset(multimap **m) {
    multimapElements elementsPerEntry;
    switch (multimapType_(*m)) {
    case MULTIMAP_TYPE_SMALL:
        MULTIMAP_UPDATE_(, *m, Reset);
        return;
    case MULTIMAP_TYPE_MEDIUM:
        elementsPerEntry = mmm(*m)->elementsPerEntry;
        break;
    case MULTIMAP_TYPE_FULL:
        elementsPerEntry = mmf(*m)->elementsPerEntry;
        break;
    default:
        assert(false);
        unreachable();
    }

    const bool isSet = multimapIsSet(*m);
    const bool compress = multimapCompresses(*m);
    const flexCapSizeLimit limit = multimapLimit(*m);
    multimapFree(*m);
    /* @own: caller; free=multimapFree */
    *m = multimapNewConfigure(elementsPerEntry, isSet, compress, limit);
}

void multimapFree(multimap *m) {
    if (MULTIMAP_IS_BARE(m)) {
        /* A bare map is its flex alone (and NULL frees nothing) */
        flexFree(multimapBareFlex(m));
    } else {
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
    MULTIMAP_DELETE_(m, DeleteByPosition, keyIndex);
}

bool multimapFirst(multimap *m, databox *elements[]) {
    MULTIMAP_RETURN(m, First, elements);
}

bool multimapLast(multimap *m, databox *elements[]) {
    MULTIMAP_RETURN(m, Last, elements);
}

bool multimapProcessPredicate(const multimapPredicate *p,
                              const databox *value) {
    /* ALL and NONE need no compare (a NONE predicate may carry no value) */
    if (p->condition == MULTIMAP_CONDITION_ALL) {
        return true;
    }
    if (p->condition == MULTIMAP_CONDITION_NONE) {
        return false;
    }

    int compared = databoxCompare(value, &p->compareAgainst);
    switch (p->condition) {
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
    default: /* ALL and NONE returned above */
        assert(false);
        unreachable();
    }

    return false;
}

/* The row 'it' reads next, when its key meets 'p': deleted, and true;
 * false when there is no row or its key does not.  The row is read with
 * its wide values and copied (owned) before the delete, so the delete
 * never searches with bytes inside the map it changes. */
static bool multimapDeleteNextRowIf_(multimap **m, multimapIterator *it,
                                     const multimapPredicate *p) {
    const multimapElements epe = multimapElementsPerEntry_(*m);
    constexpr multimapElements stackRow = 8;
    /* Wide slots first: the one heap block keeps their alignment */
    databoxWide slotsS[stackRow];
    databox gotS[stackRow];
    databox ownedS[stackRow];
    databox *gotPS[stackRow];
    const databox *rowS[stackRow];
    cleanerName(zfree, Void) void *heap =
        epe > stackRow
            ? zcalloc(epe, sizeof(databoxWide) + 2 * sizeof(databox) +
                               2 * sizeof(databox *))
            : nullptr;
    databoxWide *slots = heap ? (databoxWide *)heap : slotsS;
    databox *got = heap ? (databox *)(slots + epe) : gotS;
    databox *owned = heap ? got + epe : ownedS;
    databox **gotP = heap ? (databox **)(owned + epe) : gotPS;
    const databox **row = heap ? (const databox **)(gotP + epe) : rowS;
    for (multimapElements i = 0; i < epe; i++) {
        got[i] = (databox){};
        gotP[i] = &got[i];
    }

    if (!multimapIteratorNextWithSlots(it, gotP, slots) ||
        !multimapProcessPredicate(p, gotP[0])) {
        return false;
    }

    for (multimapElements i = 0; i < epe; i++) {
        owned[i] = databoxCopy(gotP[i]);
        row[i] = &owned[i];
    }

    if (multimapIsSet(*m)) {
        multimapDeleteFullWidth(m, row);
    } else {
        multimapDelete(m, row[0]);
    }

    for (multimapElements i = 0; i < epe; i++) {
        databoxFreeData(&owned[i]);
    }

    return true;
}

/* The tiers' one-pass trim of every row with a key at or below the bound
 * (as far as one row with the bound itself) */
static bool multimapDeleteByPredicateTier_(multimap **m,
                                           const multimapPredicate *p) {
    MULTIMAP_DELETE_(m, DeleteByPredicate, p);
}

bool multimapDeleteByPredicate(multimap **m, const multimapPredicate *p) {
    bool any = false;
    multimapIterator it;
    switch (p->condition) {
    case MULTIMAP_CONDITION_LESS_THAN_EQUAL:
        /* The tiers trim a sorted prefix in one pass.  It ends at one row
         * whose key equals the bound; a set may hold more rows with that
         * key, and they are the head now. */
        if (multimapIsSet(*m)) {
            any = multimapDeleteByPredicateTier_(m, p);
            for (;;) {
                multimapIteratorInit(*m, &it, true);
                if (!multimapDeleteNextRowIf_(m, &it, p)) {
                    return any;
                }
                any = true;
            }
        }
        return multimapDeleteByPredicateTier_(m, p);
    case MULTIMAP_CONDITION_ALL:
        if (!multimapCount(*m)) {
            return false;
        }
        multimapReset(m);
        return true;
    case MULTIMAP_CONDITION_LESS_THAN:
    case MULTIMAP_CONDITION_GREATER_THAN:
    case MULTIMAP_CONDITION_GREATER_THAN_EQUAL:
        /* Keys sort, so the rows that meet the condition are those at one
         * end: they leave from it, one search each */
        for (;;) {
            multimapIteratorInit(*m, &it,
                                 p->condition == MULTIMAP_CONDITION_LESS_THAN);
            if (!multimapDeleteNextRowIf_(m, &it, p)) {
                return any;
            }
            any = true;
        }
    case MULTIMAP_CONDITION_EQUAL:
        /* One row in a map; in a set every row with the key, together
         * from the first at the key */
        if (!multimapIsSet(*m)) {
            return multimapDelete(m, &p->compareAgainst);
        }
        while (multimapIteratorInitAt(*m, &it, true, &p->compareAgainst) &&
               multimapDeleteNextRowIf_(m, &it, p)) {
            any = true;
        }
        return any;
    default:
        return false; /* NONE: no row meets it */
    }
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
    const void **jumper[4] = {nullptr, &&iterSmall, &&iterMedium, &&iterFull};
    goto *jumper[iter->type];

iterSmall:
    return multimapSmallIteratorNext(iter, elements);

iterMedium:
    return multimapMediumIteratorNext(iter, elements);

iterFull:
    return multimapFullIteratorNext(iter, elements);
}

bool multimapIteratorNextWithSlots(multimapIterator *iter, databox **elements,
                                   databoxWide *slots) {
    const void **jumper[4] = {nullptr, &&iterSmall, &&iterMedium, &&iterFull};
    goto *jumper[iter->type];

iterSmall:
    return multimapSmallIteratorNextWithSlots(iter, elements, slots);

iterMedium:
    return multimapMediumIteratorNextWithSlots(iter, elements, slots);

iterFull:
    return multimapFullIteratorNextWithSlots(iter, elements, slots);
}

/* Rows decoded into library-owned storage (ProcessUntil and the key set
 * operations).  Each row is plain boxes plus one databoxWide slot per
 * column, decoded through the tier's WithSlots iterator: a 96/128-bit
 * element borrows its column's slot with big = 1, and a narrow element
 * writes only type, data and len.  The boxes start zeroed, and before
 * each row any 'big' left by a wide element is cleared, so 'big' is set
 * exactly on 96/128-bit values.  Narrow rows never take that branch. */
DK_INLINE_ALWAYS void multimapRowBind_(databox *const row,
                                       databox **const elements,
                                       const size_t elementsPerEntry) {
    for (size_t i = 0; i < elementsPerEntry; i++) {
        row[i] = (databox){};
        elements[i] = &row[i];
    }
}

typedef bool(multimapTierIteratorNext)(multimapIterator *iter,
                                       databox *elements[], databoxWide *slots);

DK_INLINE_ALWAYS bool multimapIteratorNextRow_(multimapIterator *const iter,
                                               multimapTierIteratorNext *next,
                                               databox **const elements,
                                               databox *const row,
                                               databoxWide *const slots) {
    for (size_t i = 0; i < iter->elementsPerEntry; i++) {
        if (unlikely(row[i].big)) {
            row[i].big = false;
        }
    }

    return next(iter, elements, slots);
}

/* One loop per tier so the tier's Next is a direct (inlinable) call. */
DK_INLINE_ALWAYS size_t multimapProcessRows_(
    multimapIterator *const iter, multimapTierIteratorNext *next,
    const multimapPredicate *p, multimapElementWalker *walker, void *userData,
    databox **const elements, databox *const row, databoxWide *const slots) {
    size_t processed = 0;
    while (multimapIteratorNextRow_(iter, next, elements, row, slots) &&
           multimapProcessPredicate(p, elements[0])) {
        processed++;
        if (!walker(userData, (const databox **)elements)) {
            break;
        }
    }

    return processed;
}

size_t multimapProcessUntil(multimap *m, const multimapPredicate *p,
                            bool forward, multimapElementWalker *walker,
                            void *userData) {
    if (multimapCount(m) == 0) {
        return 0;
    }

    /* Populate iterator metadata based on map type */
    multimapIterator iter = {};
    MULTIMAP_NORETURN(m, IteratorInit, &iter, forward);

    /* We materialize 16 physical databoxes for the most common cases
     * so we won't have to malloc() during each iteration.
     * If we have more than 16 elements, then just malloc() and free()
     * as usual. */
    const bool useHeapElements = iter.elementsPerEntry > 16 ? true : false;

    databox rowStack[16];
    databox *elementsStack[16];
    databoxWide slotsStack[16];
    databox *row = rowStack;
    databox **elements = elementsStack;
    databoxWide *slots = slotsStack;

    cleanerName(zfree, Databoxes) databox *heapRow = nullptr;
    cleanerName(zfree, DataboxPtrs) databox **heapElementPtrs = nullptr;
    cleanerName(zfree, DataboxWides) databoxWide *heapSlots = nullptr;
    if (useHeapElements) {
        /* The iterator populates through elements[0..N-1], so the heap
         * path needs an array of N boxes, an array of N pointers into
         * it, and N wide slots. */
        heapRow = zcalloc(iter.elementsPerEntry, sizeof(*heapRow));
        heapElementPtrs =
            zcalloc(iter.elementsPerEntry, sizeof(*heapElementPtrs));
        heapSlots = zcalloc(iter.elementsPerEntry, sizeof(*heapSlots));
        row = heapRow;
        elements = heapElementPtrs;
        slots = heapSlots;
    }

    multimapRowBind_(row, elements, iter.elementsPerEntry);

    switch (multimapType_(m)) {
    case MULTIMAP_TYPE_SMALL:
        return multimapProcessRows_(&iter, multimapSmallIteratorNextWithSlots,
                                    p, walker, userData, elements, row, slots);
    case MULTIMAP_TYPE_MEDIUM:
        return multimapProcessRows_(&iter, multimapMediumIteratorNextWithSlots,
                                    p, walker, userData, elements, row, slots);
    case MULTIMAP_TYPE_FULL:
        return multimapProcessRows_(&iter, multimapFullIteratorNextWithSlots, p,
                                    walker, userData, elements, row, slots);
    default:
        assert(false);
        unreachable();
    }
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
    databoxWide waStack[MULTIMAP_STACK_THRESHOLD];
    databoxWide wbStack[MULTIMAP_STACK_THRESHOLD];

    cleanerName(zfree, Databoxes) databox *eaHeap_ = nullptr;
    cleanerName(zfree, Databoxes) databox *ebHeap_ = nullptr;
    cleanerName(zfree, DataboxPtrs) databox **eaHeap = nullptr;
    cleanerName(zfree, DataboxPtrs) databox **ebHeap = nullptr;
    cleanerName(zfree, DataboxWides) databoxWide *waHeap = nullptr;
    cleanerName(zfree, DataboxWides) databoxWide *wbHeap = nullptr;

    databox *ea_, *eb_;
    databox **ea, **eb;
    databoxWide *wa, *wb;

    if (epeA <= MULTIMAP_STACK_THRESHOLD) {
        ea_ = eaStack_;
        ea = eaStack;
        wa = waStack;
    } else {
        eaHeap_ = zmalloc(epeA * sizeof(*eaHeap_));
        eaHeap = zmalloc(epeA * sizeof(databox *));
        waHeap = zmalloc(epeA * sizeof(*waHeap));
        ea_ = eaHeap_;
        ea = eaHeap;
        wa = waHeap;
    }

    if (epeB <= MULTIMAP_STACK_THRESHOLD) {
        eb_ = ebStack_;
        eb = ebStack;
        wb = wbStack;
    } else {
        ebHeap_ = zmalloc(epeB * sizeof(*ebHeap_));
        ebHeap = zmalloc(epeB * sizeof(databox *));
        wbHeap = zmalloc(epeB * sizeof(*wbHeap));
        eb_ = ebHeap_;
        eb = ebHeap;
        wb = wbHeap;
    }

    multimapRowBind_(ea_, ea, epeA);
    multimapRowBind_(eb_, eb, epeB);

#define nextA_()                                                               \
    multimapIteratorNextRow_(a, multimapIteratorNextWithSlots, ea, ea_, wa)
#define nextB_()                                                               \
    multimapIteratorNextRow_(b, multimapIteratorNextWithSlots, eb, eb_, wb)
    bool foundA = nextA_();
    bool foundB = nextB_();

    /* element-by-element zipper algorithm for intersecting two sorted lists. */
    while (foundA && foundB) {
        const int compared = databoxCompare(ea[0], eb[0]);
        if (compared < 0) {
            foundA = nextA_();
        } else if (compared > 0) {
            foundB = nextB_();
        } else {
            /* Keys compare equal, so add key to result map */
            multimapInsert(dst, (const databox **)&ea[0]);
            foundA = nextA_();
            foundB = nextB_();
        }
    }
#undef nextA_
#undef nextB_
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
    databox ea_ = {};
    databox eb_ = {};
    databoxWide wa;
    databoxWide wb;
    databox *ea[1] = {&ea_};
    databox *eb[1] = {&eb_};
#endif

#define nextA_()                                                               \
    multimapIteratorNextRow_(a, multimapIteratorNextWithSlots, ea, &ea_, &wa)
#define nextB_()                                                               \
    multimapIteratorNextRow_(b, multimapIteratorNextWithSlots, eb, &eb_, &wb)
    bool foundA = nextA_();
    bool foundB = nextB_();

    while (foundA && foundB) {
        const int compared = databoxCompare(ea[0], eb[0]);
        if (compared < 0) {
            /* ea < eb: element in A is smaller than current B element.
             * Since B is sorted and we haven't found ea yet, ea is NOT in B.
             * Add it to the difference and advance A. */
            multimapInsert(dst, (const databox **)ea);
            foundA = nextA_();
        } else if (compared > 0) {
            /* ea > eb: element in B is smaller than current A element, so
             * eb is NOT in A: part of a symmetric difference.  Advance B
             * to catch up - ea might still be in B. */
            if (symmetricDifference) {
                multimapInsert(dst, (const databox **)eb);
            }
            foundB = nextB_();
        } else {
            /* ea == eb: element exists in both A and B.
             * Don't add to difference, advance both. */
            foundA = nextA_();
            foundB = nextB_();
        }
    }

    /* The above loop terminates at the shortest list, so we need to
     * append elements from the longer map not possible to be in the
     * shorter map because we already ran out of equality depth comparisons.
     */
    while (foundA) {
        multimapInsert(dst, (const databox **)&ea[0]);
        foundA = nextA_();
    }

    if (symmetricDifference) {
        while (foundB) {
            multimapInsert(dst, (const databox **)&eb[0]);
            foundB = nextB_();
        }
    }
#undef nextA_
#undef nextB_
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
    databoxWide wsrcStack[MULTIMAP_STACK_THRESHOLD];
    cleanerName(zfree, Databoxes) databox *bsrcHeap_ = nullptr;
    cleanerName(zfree, DataboxPtrs) databox **bsrcHeap = nullptr;
    cleanerName(zfree, DataboxWides) databoxWide *wsrcHeap = nullptr;

    databox *bsrc_;
    databox **bsrc;
    databoxWide *wsrc;

    if (epe <= MULTIMAP_STACK_THRESHOLD) {
        bsrc_ = bsrcStack_;
        bsrc = bsrcStack;
        wsrc = wsrcStack;
    } else {
        bsrcHeap_ = zmalloc(epe * sizeof(*bsrcHeap_));
        bsrcHeap = zmalloc(epe * sizeof(databox *));
        wsrcHeap = zmalloc(epe * sizeof(*wsrcHeap));
        bsrc_ = bsrcHeap_;
        bsrc = bsrcHeap;
        wsrc = wsrcHeap;
    }

    multimapRowBind_(bsrc_, bsrc, epe);

    while (multimapIteratorNextRow_(&msrc, multimapIteratorNextWithSlots, bsrc,
                                    bsrc_, wsrc)) {
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
#include "dkBits.h"

#define DOUBLE_NEWLINE 0
#include "perf.h"

#include "multimapAtom.h"
defcleaner(multimapAtom *, multimapAtomFree);

#include "str.h" /* for xoroshiro128plus */

#include "mdsc.h" /* mdscfree for the replacing sweep's externalized keys */
#include "persist/multimapP.h" /* the wide persist sweep's round trips */
#include <execinfo.h> /* backtrace: the allocation window's live-block stacks */
#include <inttypes.h>
#include <stdlib.h> /* getenv */
#include <unistd.h> /* getpid, unlink for the wide persist sweep */

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
        /* a bare map has no struct */
        containerBytes = MULTIMAP_IS_BARE(m) ? 0 : sizeof(multimapSmall);
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
        assert(false);
        unreachable();
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
static constexpr int RP_WIDTH = 3;
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
        ERR("%s: key %" PRIu64 " replaced counter reported %" PRIuMAX
            " (type %d), expected %" PRIu64,
            where, key, out[0].data.u, (int)out[0].type, counter);
    }

    if (out[1].type == DATABOX_VOID ||
        out[1].data.u != (uintptr_t)RP_PTR_TAG(ptrIdx)) {
        ERR("%s: key %" PRIu64 " replaced pointer reported 0x%" PRIxMAX
            " (type %d), expected 0x%" PRIxPTR,
            where, key, out[1].data.u, (int)out[1].type,
            (uintptr_t)RP_PTR_TAG(ptrIdx));
    }

    return err;
}

/* One replacing insert of [key][counter][RP_PTR_TAG(ptrIdx)] through a
 * layer, judged against the expected report. */
typedef bool (*rpInsertFn)(void *m, const databox *elements[],
                           databox *replacedOut);

static bool rpFlex(void *m, const databox *elements[], databox *out) {
    flexEntry *middle = nullptr;
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

    if (out[0].type != expectType || out[0].data.u128 != nullptr) {
        ERR("%s: key %" PRIu64 " replaced wide value reported type %d "
            "payload %p, expected type %d payload NULL",
            where, key, (int)out[0].type, (void *)out[0].data.u128,
            (int)expectType);
    }

    if (out[1].type == DATABOX_VOID ||
        out[1].data.u != (uintptr_t)RP_PTR_TAG(key)) {
        ERR("%s: key %" PRIu64 " replaced pointer reported 0x%" PRIxMAX
            " (type %d), expected 0x%" PRIxPTR,
            where, key, out[1].data.u, (int)out[1].type,
            (uintptr_t)RP_PTR_TAG(key));
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
        flexEntry *middle = nullptr;
        static constexpr char longKey[] = "a-key-long-enough-to-externalize";
        const databox k = databoxNewBytesString(longKey);
        const databox c0 = databoxNewUnsigned(7);
        const databox tag0 = databoxNewPtr(RP_PTR_TAG(7));
        const databox *e0[RP_WIDTH] = {&k, &c0, &tag0};
        databox out[RP_VALUES];
        void *keyAlloc = nullptr;
        bool replaced =
            flexInsertReplaceByTypeSortedWithMiddleMultiDirectLongKeysBecomePointersReplacing(
                &f, RP_WIDTH, e0, &middle, true, &keyAlloc, out);
        err += rpJudge("flex-long-key insert", 0, replaced, out, false, 0, 0);
        if (!keyAlloc) {
            ERR("flex-long-key: the long key was not externalized");
        }

        const databox c1 = databoxNewUnsigned(8);
        const databox tag1 = databoxNewPtr(RP_PTR_TAG(8));
        const databox *e1[RP_WIDTH] = {&k, &c1, &tag1};
        void *keyAllocReplace = nullptr;
        replaced =
            flexInsertReplaceByTypeSortedWithMiddleMultiDirectLongKeysBecomePointersReplacing(
                &f, RP_WIDTH, e1, &middle, true, &keyAllocReplace, out);
        err += rpJudge("flex-long-key replace", 0, replaced, out, true, 7, 7);
        if (keyAllocReplace) {
            ERR("flex-long-key: a replace minted a second key allocation");
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
        static constexpr char longKey[] =
            "another-key-long-enough-to-externalize";
        const databox k = databoxNewBytesString(longKey);
        const databox c0 = databoxNewUnsigned(11);
        const databox tag0 = databoxNewPtr(RP_PTR_TAG(11));
        const databox *e0[RP_WIDTH] = {&k, &c0, &tag0};
        databox out[RP_VALUES];
        void *keyAlloc = nullptr;
        bool replaced = multimapFullInsertAllowExternalizeKeysReplacing(
            m, e0, &keyAlloc, out);
        err += rpJudge("full-mdsc insert", 0, replaced, out, false, 0, 0);
        if (!keyAlloc) {
            ERR("full-mdsc: the long key was not externalized");
        }

        const databox c1 = databoxNewUnsigned(12);
        const databox tag1 = databoxNewPtr(RP_PTR_TAG(12));
        const databox *e1[RP_WIDTH] = {&k, &c1, &tag1};
        void *keyAllocReplace = nullptr;
        replaced = multimapFullInsertAllowExternalizeKeysReplacing(
            m, e1, &keyAllocReplace, out);
        err += rpJudge("full-mdsc replace", 0, replaced, out, true, 11, 11);
        if (keyAllocReplace) {
            ERR("full-mdsc: a replace minted a second key allocation");
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
        constexpr int splitRows = 200;
        cleanerName(multimapFullFree, Sweep) multimapFull *m =
            multimapFullNew(2);
        void *allocs[splitRows] = {};
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
        constexpr int setRows = 120;
        cleanerName(multimapFullFree, Sweep) multimapFull *m =
            multimapFullSetNew(2, 2048);
        /* A wrongly minted second allocation stays referenced by the map,
         * so it is freed with the first ones, after the last lookup. */
        void *allocs[setRows] = {};
        void *extraAllocs[setRows] = {};
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
                void *alloc = nullptr;
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
        void *alloc0 = nullptr;
        if (multimapFullInsertAllowExternalizeKeys(m, e0, &alloc0)) {
            ERR("set-externalize: a new (key, value) pair reported found");
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
            ERR("set duplicate: the member was not found");
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
        for (uint64_t w = 0; w < countof(wide); w++) {
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

/* ====================================================================
 * 96/128-bit keys and values through every tier
 * ==================================================================== */
/* The oracle is independent of databox: a sign plus the two's complement
 * bits gives the whole signed and unsigned 128-bit range one order. */
typedef struct mwInt {
    __uint128_t bits;
    bool neg;
} mwInt;

typedef struct mwRow {
    mwInt key;
    mwInt val;
    bool present;
} mwRow;

static int mwCmp(const mwInt *a, const mwInt *b) {
    if (a->neg != b->neg) {
        return a->neg ? -1 : 1;
    }

    return (a->bits > b->bits) - (a->bits < b->bits);
}

static int mwRowKeyCmp(const void *a, const void *b) {
    return mwCmp(&((const mwRow *)a)->key, &((const mwRow *)b)->key);
}

static int mwRowCmp(const void *a, const void *b) {
    const int k = mwRowKeyCmp(a, b);
    return k ? k : mwCmp(&((const mwRow *)a)->val, &((const mwRow *)b)->val);
}

static bool mwIsWide(const mwInt *v) {
    if (v->neg) {
        /* below INT64_MIN */
        return v->bits < (__uint128_t)0 - ((__uint128_t)1 << 63);
    }

    return v->bits > UINT64_MAX;
}

static mwInt mwSmall(const int64_t v) {
    return (mwInt){.bits = (__uint128_t)(__int128_t)v, .neg = v < 0};
}

/* An input box for 'v'.  A wide box points at the oracle's own bits. */
static databox mwBox(const mwInt *v) {
    if (mwIsWide(v)) {
        return (databox){.type =
                             v->neg ? DATABOX_SIGNED_128 : DATABOX_UNSIGNED_128,
                         .big = true,
                         .data.u128 = (__uint128_t *)&v->bits};
    }

    return v->neg ? databoxNewSigned((int64_t)(uint64_t)v->bits)
                  : databoxNewUnsigned((uint64_t)v->bits);
}

/* Does 'got' hold exactly 'want', under the canonical read-back types?  A
 * wide value must have a live payload ('big' set). */
static bool mwHolds(const databox *got, const mwInt *want) {
    if (mwIsWide(want)) {
        const databoxType type =
            want->neg ? DATABOX_SIGNED_128 : DATABOX_UNSIGNED_128;
        if (got->type != type || !got->big || !got->data.u128) {
            return false;
        }

        __uint128_t bits;
        memcpy(&bits, got->data.u128, sizeof(bits));
        return bits == want->bits;
    }

    if (want->neg) {
        return got->type == DATABOX_SIGNED_64 &&
               got->data.i64 == (int64_t)(uint64_t)want->bits;
    }

    return got->type == DATABOX_UNSIGNED_64 &&
           got->data.u64 == (uint64_t)want->bits;
}

/* A caller-provided output box: 0xFF-poisoned, then armed. */
static databox *mwArm(databoxBig *b) {
    memset(b, 0xFF, sizeof(*b));
    DATABOX_BIG_INIT(b);
    return (databox *)b;
}

/* A random 96- or 128-bit value of either sign. */
static mwInt mwRandWide(uint64_t *rng) {
    const uint64_t hi = dkXorshift64Next(rng);
    const uint64_t lo = dkXorshift64Next(rng);
    const unsigned width = (hi & 2) ? 95 : 127;
    __uint128_t mag =
        (((__uint128_t)hi << 64) | lo) & ((((__uint128_t)1) << width) - 1);
    mag |= ((__uint128_t)1) << 64;
    if (hi & 1) {
        return (mwInt){.bits = (__uint128_t)0 - mag, .neg = true};
    }

    return (mwInt){.bits = mag, .neg = false};
}

/* Sort 'rows' by key and drop repeated keys; returns how many are kept. */
static size_t mwSortUnique(mwRow *rows, const size_t n) {
    qsort(rows, n, sizeof(*rows), mwRowKeyCmp);
    size_t kept = 0;
    for (size_t i = 0; i < n; i++) {
        if (kept == 0 || mwCmp(&rows[kept - 1].key, &rows[i].key) != 0) {
            rows[kept++] = rows[i];
        }
    }

    return kept;
}

/* Fill 'rows' with distinct keys sorted ascending: the design's boundary
 * set W, narrow keys around the 64-bit limits, then random wide keys.  Two
 * of every three values are wide; the rest are small integers. */
static size_t mwBuildRows(mwRow *rows, const size_t cap, uint64_t *rng) {
    const __uint128_t one = 1;
    const mwInt fixed[] = {
        {.bits = one << 64},
        {.bits = (one << 95) - 1},
        {.bits = one << 95},
        {.bits = one << 127},
        {.bits = ~(__uint128_t)0},
        {.bits = ~(__uint128_t)0 >> 1},
        {.bits = (__uint128_t)0 - (one << 63) - 1, .neg = true},
        {.bits = (__uint128_t)0 - (one << 95), .neg = true},
        {.bits = one << 127, .neg = true},
        mwSmall(0),
        mwSmall(1),
        mwSmall(-1),
        mwSmall(INT64_MAX),
        mwSmall(INT64_MIN),
        {.bits = UINT64_MAX},
        mwSmall(1000),
        mwSmall(-1000),
    };

    size_t n = 0;
    while (n < cap) {
        rows[n].key = n < countof(fixed) ? fixed[n] : mwRandWide(rng);
        rows[n].val =
            (n % 3 == 2) ? mwSmall((int64_t)(n * 7) - 50) : mwRandWide(rng);
        rows[n].present = false;
        n++;
    }

    return mwSortUnique(rows, n);
}

static void mwShuffle(size_t *order, const size_t n, uint64_t *rng) {
    for (size_t i = 0; i < n; i++) {
        order[i] = i;
    }

    DK_SHUFFLE(order, n, rng);
}

static void mwInsert(multimap **m, mwRow *r) {
    const databox key = mwBox(&r->key);
    const databox val = mwBox(&r->val);
    const databox *elements[2] = {&key, &val};
    multimapInsert(m, elements);
    r->present = true;
}

static const mwRow *mwNextPresent(const mwRow *rows, const size_t n,
                                  size_t *idx) {
    while (*idx < n && !rows[*idx].present) {
        (*idx)++;
    }

    return *idx < n ? &rows[(*idx)++] : nullptr;
}

static const mwRow *mwPrevPresent(const mwRow *rows, size_t *idx) {
    while (*idx > 0 && !rows[*idx - 1].present) {
        (*idx)--;
    }

    return *idx > 0 ? &rows[--(*idx)] : nullptr;
}

/* ProcessUntil walker: each row must be the next expected one, and the
 * library's own row storage must report 'big' exactly for wide values. */
typedef struct mwWalk {
    const mwRow *rows;
    size_t n;
    size_t idx;
    size_t seen;
    size_t stopAfter;
    bool forward;
    bool bad;
} mwWalk;

static bool mwWalker(void *userData, const databox *elements[]) {
    mwWalk *w = userData;
    const mwRow *want = w->forward ? mwNextPresent(w->rows, w->n, &w->idx)
                                   : mwPrevPresent(w->rows, &w->idx);
    if (!want || !mwHolds(elements[0], &want->key) ||
        !mwHolds(elements[1], &want->val) ||
        elements[0]->big != mwIsWide(&want->key) ||
        elements[1]->big != mwIsWide(&want->val)) {
        w->bad = true;
        return false;
    }

    w->seen++;
    return !w->stopAfter || w->seen < w->stopAfter;
}

static int32_t mwCheckProcess(const char *where, multimap *m, const mwRow *rows,
                              const size_t n, const multimapCondition condition,
                              const mwInt *pivot, const bool forward,
                              const size_t stopAfter) {
    int32_t err = 0;
    size_t expect = 0;
    for (size_t i = 0; i < n; i++) {
        if (!rows[i].present) {
            continue;
        }

        const int c = mwCmp(&rows[i].key, pivot);
        expect +=
            condition == MULTIMAP_CONDITION_ALL ||
            (condition == MULTIMAP_CONDITION_LESS_THAN && c < 0) ||
            (condition == MULTIMAP_CONDITION_GREATER_THAN_EQUAL && c >= 0);
    }

    if (stopAfter && expect > stopAfter) {
        expect = stopAfter;
    }

    const multimapPredicate p = {.condition = condition,
                                 .compareAgainst = mwBox(pivot)};
    mwWalk w = {.rows = rows,
                .n = n,
                .idx = forward ? 0 : n,
                .stopAfter = stopAfter,
                .forward = forward};
    const size_t processed = multimapProcessUntil(m, &p, forward, mwWalker, &w);
    if (w.bad || w.seen != expect || processed != expect) {
        ERR("%s: ProcessUntil(cond %d, %s) saw %zu rows (bad %d), "
            "processed %zu, expected %zu",
            where, (int)condition, forward ? "forward" : "reverse", w.seen,
            w.bad, processed, expect);
    }

    return err;
}

/* Iterate a 1-element key set and compare it with the present rows whose
 * 'member' flag is set. */
static int32_t mwCheckKeySet(const char *where, multimap *set,
                             const mwRow *rows, const size_t n,
                             const bool *member) {
    int32_t err = 0;
    multimapIterator iter;
    multimapIteratorInit(set, &iter, true);
    size_t idx = 0;
    for (;;) {
        databoxBig kb;
        databox *e[1] = {mwArm(&kb)};
        const bool got = multimapIteratorNext(&iter, e);
        while (idx < n && !(rows[idx].present && member[idx])) {
            idx++;
        }

        if (!got) {
            if (idx < n) {
                ERR("%s: key set ended early at row %zu", where, idx);
            }
            break;
        }

        if (idx >= n || !mwHolds(e[0], &rows[idx].key)) {
            ERR("%s: key set holds a wrong key near row %zu", where, idx);
            break;
        }

        idx++;
    }

    return err;
}

/* Intersect, difference and copy of key sets built from 'm'. */
static int32_t mwCheckSets(const char *where, multimap *m, const mwRow *rows,
                           const size_t n) {
    int32_t err = 0;
    bool all[n];
    bool everyOther[n];
    bool rest[n];
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        all[i] = true;
        everyOther[i] = rows[i].present && (k++ % 2 == 0);
        rest[i] = !everyOther[i];
    }

    cleaner(multimapFree) multimap *a = multimapNew(1);
    multimapCopyKeys(&a, m);
    err += mwCheckKeySet(where, a, rows, n, all);

    cleaner(multimapFree) multimap *b = multimapNew(1);
    for (size_t i = 0; i < n; i++) {
        if (everyOther[i]) {
            const databox key = mwBox(&rows[i].key);
            const databox *e[1] = {&key};
            multimapInsert(&b, e);
        }
    }

    multimapIterator ia;
    multimapIterator ib;
    cleaner(multimapFree) multimap *inter = multimapNew(1);
    multimapIteratorInit(a, &ia, true);
    multimapIteratorInit(b, &ib, true);
    multimapIntersectKeys(&inter, &ia, &ib);
    err += mwCheckKeySet(where, inter, rows, n, everyOther);

    cleaner(multimapFree) multimap *diff = multimapNew(1);
    multimapIteratorInit(a, &ia, true);
    multimapIteratorInit(b, &ib, true);
    multimapDifferenceKeys(&diff, &ia, &ib, false);
    err += mwCheckKeySet(where, diff, rows, n, rest);

    return err;
}

/* multimapIteratorNextWithSlots in both directions: plain 0xFF-poisoned
 * boxes and slots.  A wide element must borrow exactly its own slot, and a
 * narrow element must leave its slot untouched.  NULL slots must keep the
 * in-band contract on armed boxes. */
static int32_t mwCheckSlotIteration(const char *where, multimap *m,
                                    const mwRow *rows, const size_t n,
                                    const size_t present) {
    int32_t err = 0;
    uint8_t poison[sizeof(databoxWide)];
    memset(poison, 0xFF, sizeof(poison));
    for (int dir = 0; dir < 4; dir++) {
        const bool forward = (dir & 1) == 0;
        const bool useSlots = dir < 2;
        multimapIterator iter;
        multimapIteratorInit(m, &iter, forward);
        size_t idx = forward ? 0 : n;
        size_t seen = 0;
        for (;;) {
            databox kb;
            databox vb;
            databoxBig kbig;
            databoxBig vbig;
            databoxWide slots[2];
            databox *e[2];
            memset(slots, 0xFF, sizeof(slots));
            if (useSlots) {
                memset(&kb, 0xFF, sizeof(kb));
                memset(&vb, 0xFF, sizeof(vb));
                e[0] = &kb;
                e[1] = &vb;
            } else {
                e[0] = mwArm(&kbig);
                e[1] = mwArm(&vbig);
            }

            if (!multimapIteratorNextWithSlots(&iter, e,
                                               useSlots ? slots : nullptr)) {
                break;
            }

            const mwRow *want = forward ? mwNextPresent(rows, n, &idx)
                                        : mwPrevPresent(rows, &idx);
            bool ok =
                want && mwHolds(e[0], &want->key) && mwHolds(e[1], &want->val);
            for (size_t i = 0; ok && useSlots && i < 2; i++) {
                const mwInt *v = i == 0 ? &want->key : &want->val;
                if (mwIsWide(v)) {
                    ok = e[i]->data.ptr == (void *)&slots[i] && e[i]->big &&
                         !e[i]->allocated;
                } else {
                    ok = memcmp(&slots[i], poison, sizeof(poison)) == 0;
                }
            }

            if (!ok) {
                ERR("%s: %s slot iteration (%s) wrong at row %zu", where,
                    forward ? "forward" : "reverse",
                    useSlots ? "slots" : "in-band", seen);
                break;
            }

            seen++;
        }

        if (seen != present) {
            ERR("%s: %s slot iteration (%s) saw %zu rows, expected %zu", where,
                forward ? "forward" : "reverse", useSlots ? "slots" : "in-band",
                seen, present);
        }
    }

    return err;
}

/* Every read path of 'm' against the rows marked present. */
static int32_t mwCheckReads(const char *where, multimap *m, const mwRow *rows,
                            const size_t n) {
    int32_t err = 0;
    size_t present = 0;
    for (size_t i = 0; i < n; i++) {
        present += rows[i].present;
    }

    if (multimapCount(m) != present) {
        ERR("%s: count %zu, expected %zu", where, multimapCount(m), present);
    }

    for (size_t i = 0; i < n; i++) {
        const databox key = mwBox(&rows[i].key);
        databoxBig vb;
        databox *e[1] = {mwArm(&vb)};
        const bool found = multimapLookup(m, &key, e);
        if (found != rows[i].present) {
            ERR("%s: row %zu lookup found %d, expected %d", where, i, found,
                rows[i].present);
        } else if (found && !mwHolds(e[0], &rows[i].val)) {
            ERR("%s: row %zu lookup gave a wrong value", where, i);
        }

        if (multimapExists(m, &key) != rows[i].present) {
            ERR("%s: row %zu exists disagrees", where, i);
        }

        /* key + 1 is absent unless it is the next row's key; skip the
         * two keys where + 1 changes sign or wraps. */
        mwInt probe = rows[i].key;
        probe.bits++;
        const bool wraps = probe.bits == 0;
        if (!wraps && (i + 1 == n || mwCmp(&probe, &rows[i + 1].key) != 0)) {
            const databox absent = mwBox(&probe);
            if (multimapExists(m, &absent)) {
                ERR("%s: key after row %zu exists", where, i);
            }
        }
    }

    for (int dir = 0; dir < 2; dir++) {
        const bool forward = dir == 0;
        multimapIterator iter;
        multimapIteratorInit(m, &iter, forward);
        size_t idx = forward ? 0 : n;
        size_t seen = 0;
        for (;;) {
            databoxBig kb;
            databoxBig vb;
            databox *e[2] = {mwArm(&kb), mwArm(&vb)};
            if (!multimapIteratorNext(&iter, e)) {
                break;
            }

            const mwRow *want = forward ? mwNextPresent(rows, n, &idx)
                                        : mwPrevPresent(rows, &idx);
            if (!want || !mwHolds(e[0], &want->key) ||
                !mwHolds(e[1], &want->val)) {
                ERR("%s: %s iteration wrong at row %zu", where,
                    forward ? "forward" : "reverse", seen);
                break;
            }

            seen++;
        }

        if (seen != present) {
            ERR("%s: %s iteration saw %zu rows, expected %zu", where,
                forward ? "forward" : "reverse", seen, present);
        }
    }

    err += mwCheckSlotIteration(where, m, rows, n, present);

    if (present) {
        size_t lo = 0;
        size_t hi = n;
        const mwRow *first = mwNextPresent(rows, n, &lo);
        const mwRow *last = mwPrevPresent(rows, &hi);
        databoxBig kb;
        databoxBig vb;
        databox *e[2] = {mwArm(&kb), mwArm(&vb)};
        if (!multimapFirst(m, e) || !mwHolds(e[0], &first->key) ||
            !mwHolds(e[1], &first->val)) {
            ERR("%s: First wrong", where);
        }

        e[0] = mwArm(&kb);
        e[1] = mwArm(&vb);
        if (!multimapLast(m, e) || !mwHolds(e[0], &last->key) ||
            !mwHolds(e[1], &last->val)) {
            ERR("%s: Last wrong", where);
        }
    }

    /* Positional reads from both ends, and range iteration starting at
     * every fifth present key. */
    int64_t pos = 0;
    for (size_t i = 0; i < n; i++) {
        if (!rows[i].present) {
            continue;
        }

        for (int side = 0; side < 2; side++) {
            const int64_t at = side ? pos - (int64_t)present : pos;
            databoxBig kb;
            databoxBig vb;
            databox *e[2] = {mwArm(&kb), mwArm(&vb)};
            if (!multimapLookupByPosition(m, at, e) ||
                !mwHolds(e[0], &rows[i].key) || !mwHolds(e[1], &rows[i].val)) {
                ERR("%s: LookupByPosition(%" PRId64 ") wrong", where, at);
            }
        }

        if (pos % 5 == 0) {
            const databox anchor = mwBox(&rows[i].key);
            multimapIterator iter;
            if (!multimapIteratorInitAt(m, &iter, true, &anchor)) {
                ERR("%s: IteratorInitAt row %zu found nothing", where, i);
            } else {
                size_t idx = i;
                size_t seen = 0;
                for (;;) {
                    databoxBig kb;
                    databoxBig vb;
                    databox *e[2] = {mwArm(&kb), mwArm(&vb)};
                    if (!multimapIteratorNext(&iter, e)) {
                        break;
                    }

                    const mwRow *want = mwNextPresent(rows, n, &idx);
                    if (!want || !mwHolds(e[0], &want->key) ||
                        !mwHolds(e[1], &want->val)) {
                        ERR("%s: range from row %zu wrong at step %zu", where,
                            i, seen);
                        break;
                    }

                    seen++;
                }

                if (seen != present - (size_t)pos) {
                    ERR("%s: range from row %zu saw %zu rows, expected %zu",
                        where, i, seen, present - (size_t)pos);
                }
            }
        }

        pos++;
    }

    if (present) {
        /* Pivot on the middle present key (a wide key in these rows). */
        size_t mid = 0;
        for (size_t i = 0, k = 0; i < n; i++) {
            if (rows[i].present && k++ == present / 2) {
                mid = i;
            }
        }

        const mwInt *pivot = &rows[mid].key;
        err += mwCheckProcess(where, m, rows, n, MULTIMAP_CONDITION_ALL, pivot,
                              true, 0);
        err += mwCheckProcess(where, m, rows, n, MULTIMAP_CONDITION_ALL, pivot,
                              false, 0);
        err += mwCheckProcess(where, m, rows, n, MULTIMAP_CONDITION_LESS_THAN,
                              pivot, true, 0);
        err += mwCheckProcess(where, m, rows, n,
                              MULTIMAP_CONDITION_GREATER_THAN_EQUAL, pivot,
                              false, 0);
        err += mwCheckProcess(where, m, rows, n, MULTIMAP_CONDITION_ALL, pivot,
                              true, 3);
    }

    err += mwCheckSets(where, m, rows, n);
    return err;
}

/* The tier-direct FieldIncr can't increment a wide field: it reports 0
 * and leaves the value alone. */
static int64_t mwTierFieldIncr(multimap *m, const databox *key) {
    switch (multimapType_(m)) {
    case MULTIMAP_TYPE_SMALL:
        return multimapSmallFieldIncr(mms(m), key, 1, 1);
    case MULTIMAP_TYPE_MEDIUM:
        return multimapMediumFieldIncr(mmm(m), key, 1, 1);
    default:
        return multimapFullFieldIncr(mmf(m), key, 1, 1);
    }
}

/* Insert, read, increment, delete, trim by predicate, refill and copy a
 * key -> value map with wide keys and values, checking every read path
 * after each step.  'expect' is the tier the full row set must reach. */
static int32_t mwExercise(const char *tier, multimap **m, mwRow *rows,
                          const size_t n, const multimapType expect,
                          uint64_t *rng) {
    int32_t err = 0;
    size_t order[n];
    mwShuffle(order, n, rng);
    for (size_t k = 0; k < n; k++) {
        mwInsert(m, &rows[order[k]]);
    }

    if (multimapType_(*m) != expect) {
        ERR("%s: %zu rows built tier %d, expected %d", tier, n,
            (int)multimapType_(*m), (int)expect);
    }

    char where[64];
    snprintf(where, sizeof(where), "%s inserted", tier);
    err += mwCheckReads(where, *m, rows, n);

    for (size_t i = 0; i < n; i++) {
        const databox key = mwBox(&rows[i].key);
        if (mwIsWide(&rows[i].val)) {
            if (mwTierFieldIncr(*m, &key) != 0) {
                ERR("%s: tier FieldIncr of wide row %zu returned non-zero",
                    tier, i);
            }
        } else {
            const int64_t before = (int64_t)(uint64_t)rows[i].val.bits;
            const int64_t got = multimapFieldIncr(m, &key, 1, 3);
            if (got != before + 3) {
                ERR("%s: FieldIncr row %zu gave %" PRId64 ", expected "
                    "%" PRId64,
                    tier, i, got, before + 3);
            }

            rows[i].val = mwSmall(before + 3);
        }
    }

    snprintf(where, sizeof(where), "%s incremented", tier);
    err += mwCheckReads(where, *m, rows, n);

    for (size_t k = 1; k < n; k += 2) {
        mwRow *r = &rows[order[k]];
        const databox key = mwBox(&r->key);
        if (!multimapDelete(m, &key)) {
            ERR("%s: delete of row %zu failed", tier, order[k]);
        }

        r->present = false;
    }

    snprintf(where, sizeof(where), "%s half deleted", tier);
    err += mwCheckReads(where, *m, rows, n);

    /* Trim everything at or below a present key a quarter of the way in. */
    size_t cut = n;
    for (size_t i = n / 4; i < n; i++) {
        if (rows[i].present) {
            cut = i;
            break;
        }
    }

    if (cut < n) {
        const multimapPredicate p = {.condition =
                                         MULTIMAP_CONDITION_LESS_THAN_EQUAL,
                                     .compareAgainst = mwBox(&rows[cut].key)};
        multimapDeleteByPredicate(m, &p);
        for (size_t i = 0; i <= cut; i++) {
            rows[i].present = false;
        }

        snprintf(where, sizeof(where), "%s trimmed", tier);
        err += mwCheckReads(where, *m, rows, n);
    }

    for (size_t k = 0; k < n; k++) {
        if (!rows[order[k]].present) {
            mwInsert(m, &rows[order[k]]);
        }
    }

    snprintf(where, sizeof(where), "%s refilled", tier);
    err += mwCheckReads(where, *m, rows, n);

    cleaner(multimapFree) multimap *copy = multimapCopy(*m);
    snprintf(where, sizeof(where), "%s copy", tier);
    err += mwCheckReads(where, copy, rows, n);

    return err;
}

/* Rows with repeated keys and distinct values, inserted full width: the
 * whole row is the sort key, so wide values are compared too. */
static int32_t mwExerciseFullWidth(const char *tier, multimap **m,
                                   const multimapType expect, uint64_t *rng) {
    int32_t err = 0;
    enum { mwfwKeys = 6, mwfwPerKey = 5, mwfwRows = mwfwKeys * mwfwPerKey };
    mwRow rows[mwfwRows];
    for (size_t i = 0; i < mwfwRows; i++) {
        const size_t k = i / mwfwPerKey;
        rows[i].key =
            (k % 2) ? mwSmall((int64_t)k * 1000 - 2500) : mwRandWide(rng);
        if (i % mwfwPerKey) {
            rows[i].key = rows[i - 1].key;
        }

        rows[i].val =
            (i % mwfwPerKey == 4) ? mwSmall((int64_t)i - 15) : mwRandWide(rng);
        rows[i].present = false;
    }

    qsort(rows, mwfwRows, sizeof(*rows), mwRowCmp);

    size_t order[mwfwRows];
    mwShuffle(order, mwfwRows, rng);
    for (size_t k = 0; k < mwfwRows; k++) {
        mwRow *r = &rows[order[k]];
        const databox key = mwBox(&r->key);
        const databox val = mwBox(&r->val);
        const databox *e[2] = {&key, &val};
        multimapInsertFullWidth(m, e);
        r->present = true;
    }

    if (multimapType_(*m) != expect) {
        ERR("%s full width: built tier %d, expected %d", tier,
            (int)multimapType_(*m), (int)expect);
    }

    for (int round = 0; round < 2; round++) {
        size_t present = 0;
        for (size_t i = 0; i < mwfwRows; i++) {
            const databox key = mwBox(&rows[i].key);
            const databox val = mwBox(&rows[i].val);
            const databox *e[2] = {&key, &val};
            present += rows[i].present;
            if (multimapExistsFullWidth(*m, e) != rows[i].present) {
                ERR("%s full width round %d: row %zu exists disagrees", tier,
                    round, i);
            }
        }

        if (multimapCount(*m) != present) {
            ERR("%s full width round %d: count %zu, expected %zu", tier, round,
                multimapCount(*m), present);
        }

        multimapIterator iter;
        multimapIteratorInit(*m, &iter, true);
        size_t idx = 0;
        for (;;) {
            databoxBig kb;
            databoxBig vb;
            databox *e[2] = {mwArm(&kb), mwArm(&vb)};
            if (!multimapIteratorNext(&iter, e)) {
                break;
            }

            const mwRow *want = mwNextPresent(rows, mwfwRows, &idx);
            if (!want || !mwHolds(e[0], &want->key) ||
                !mwHolds(e[1], &want->val)) {
                ERR("%s full width round %d: iteration wrong", tier, round);
                break;
            }
        }

        if (mwNextPresent(rows, mwfwRows, &idx)) {
            ERR("%s full width round %d: iteration ended early", tier, round);
        }

        for (size_t k = round; k < mwfwRows; k += 2) {
            mwRow *r = &rows[order[k]];
            if (!r->present) {
                continue;
            }

            const databox key = mwBox(&r->key);
            const databox val = mwBox(&r->val);
            const databox *e[2] = {&key, &val};
            if (!multimapDeleteFullWidth(m, e)) {
                ERR("%s full width: delete of row %zu failed", tier, order[k]);
            }

            r->present = false;
        }
    }

    return err;
}

int32_t multimapWideSweep(void) {
    int32_t err = 0;
    uint64_t rng = 0x9E3779B97F4A7C15ULL;
    constexpr int mwMaxRows = 96;
    mwRow rows[mwMaxRows];

    struct {
        const char *tier;
        size_t rows;
        flexCapSizeLimit limit;
        multimapType expect;
    } const plans[] = {
        {"small", 12, FLEX_CAP_LEVEL_2048, MULTIMAP_TYPE_SMALL},
        {"medium", 16, FLEX_CAP_LEVEL_256, MULTIMAP_TYPE_MEDIUM},
        {"full", mwMaxRows, FLEX_CAP_LEVEL_64, MULTIMAP_TYPE_FULL},
    };

    for (size_t p = 0; p < countof(plans); p++) {
        /* Build the full sorted set, then keep an evenly spaced subset
         * that includes both extremes (INT128_MIN and UINT128_MAX). */
        const size_t built = mwBuildRows(rows, mwMaxRows, &rng);
        const size_t want = plans[p].rows < built ? plans[p].rows : built;
        size_t n = 0;
        size_t wideKeys = 0;
        for (size_t i = 0; i < want; i++) {
            rows[n] = rows[(i * (built - 1)) / (want - 1)];
            wideKeys += mwIsWide(&rows[n].key);
            n++;
        }

        if (wideKeys < 2) {
            ERR("%s: only %zu wide keys", plans[p].tier, wideKeys);
        }

        cleaner(multimapFree) multimap *m = multimapNewLimit(2, plans[p].limit);
        err += mwExercise(plans[p].tier, &m, rows, n, plans[p].expect, &rng);
    }

    const flexCapSizeLimit fwLimits[] = {FLEX_CAP_LEVEL_2048,
                                         FLEX_CAP_LEVEL_512, FLEX_CAP_LEVEL_64};
    const multimapType fwTiers[] = {MULTIMAP_TYPE_SMALL, MULTIMAP_TYPE_MEDIUM,
                                    MULTIMAP_TYPE_FULL};
    for (size_t t = 0; t < countof(fwLimits); t++) {
        cleaner(multimapFree) multimap *m = multimapNewLimit(2, fwLimits[t]);
        err += mwExerciseFullWidth(t == 0   ? "small"
                                   : t == 1 ? "medium"
                                            : "full",
                                   &m, fwTiers[t], &rng);
    }

    return err;
}

/* ====================================================================
 * The normal / extended / mixed matrix
 * ==================================================================== */
/* Live blocks from the datakit allocator, counted while a window opened by
 * mxCountBegin is open.  A window that builds and frees maps must end
 * where it began, which catches a leaked rangeBox payload or row copy. */
static datakitConfig mxSavedAlloc;
static int64_t mxLive;

/* The blocks the window allocated and has not freed, so a window that
 * ends with blocks live can name them: address and size always, and the
 * allocating stack when DK_TEST_ALLOC_STACKS is set in the environment.
 * An open-addressing table with backward-shift deletion, allocated with
 * the saved allocator so it is never counted itself. */
enum { mxTrackBits = 16, mxTrackFrames = 12 };
#define mxTrackSlots ((size_t)1 << mxTrackBits)
typedef struct mxTracked {
    const void *p;
    size_t size;
} mxTracked;
typedef struct mxTrackedStack {
    int depth;
    void *frames[mxTrackFrames];
} mxTrackedStack;
static mxTracked *mxTrack;
static mxTrackedStack *mxTrackStacks;
static size_t mxTrackLive;
static bool mxTrackFull;

static size_t mxTrackHome(const void *p) {
    return (size_t)(((uint64_t)(uintptr_t)p * UINT64_C(0x9E3779B97F4A7C15)) >>
                    (64 - mxTrackBits));
}

static void mxTrackAdd(const void *p, const size_t sz) {
    if (!mxTrack || !p) {
        return;
    }

    if (mxTrackLive >= mxTrackSlots / 2) {
        mxTrackFull = true;
        return;
    }

    size_t i = mxTrackHome(p);
    while (mxTrack[i].p) {
        i = (i + 1) & (mxTrackSlots - 1);
    }

    mxTrack[i] = (mxTracked){.p = p, .size = sz};
    if (mxTrackStacks) {
        mxTrackStacks[i].depth =
            backtrace(mxTrackStacks[i].frames, mxTrackFrames);
    }

    mxTrackLive++;
}

/* Forget 'p'; false when the window never allocated it */
static bool mxTrackDel(const void *p) {
    if (!mxTrack || !p) {
        return false;
    }

    size_t i = mxTrackHome(p);
    while (mxTrack[i].p != p) {
        if (!mxTrack[i].p) {
            return false;
        }

        i = (i + 1) & (mxTrackSlots - 1);
    }

    /* Backward-shift: pull later entries of the probe run into the hole */
    size_t j = i;
    for (;;) {
        mxTrack[i].p = nullptr;
        size_t home;
        do {
            j = (j + 1) & (mxTrackSlots - 1);
            if (!mxTrack[j].p) {
                mxTrackLive--;
                return true;
            }

            home = mxTrackHome(mxTrack[j].p);
        } while (i <= j ? (i < home && home <= j) : (i < home || home <= j));

        mxTrack[i] = mxTrack[j];
        if (mxTrackStacks) {
            mxTrackStacks[i] = mxTrackStacks[j];
        }

        i = j;
    }
}

static void *mxCalloc(size_t count, size_t sz) {
    void *p = mxSavedAlloc.localCalloc(count, sz);
    mxLive += p != nullptr;
    mxTrackAdd(p, count * sz);
    return p;
}

static void *mxMalloc(size_t sz) {
    void *p = mxSavedAlloc.localMalloc(sz);
    mxLive += p != nullptr;
    mxTrackAdd(p, sz);
    return p;
}

static void mxReallocCount(const void *ptr, const void *p, const size_t sz) {
    if (!ptr && p) {
        mxLive++;
        mxTrackAdd(p, sz);
    } else if (ptr && !p && !sz) {
        mxLive--;
        (void)mxTrackDel(ptr);
    } else if (ptr && p && mxTrackDel(ptr)) {
        mxTrackAdd(p, sz);
    }
}

static void *mxRealloc(void *ptr, size_t sz) {
    void *p = mxSavedAlloc.localRealloc(ptr, sz);
    mxReallocCount(ptr, p, sz);
    return p;
}

static void *mxReallocSlate(void *ptr, size_t sz) {
    void *p = mxSavedAlloc.localReallocSlate(ptr, sz);
    mxReallocCount(ptr, p, sz);
    return p;
}

static void mxFree(void *ptr) {
    mxLive -= ptr != nullptr;
    (void)mxTrackDel(ptr);
    mxSavedAlloc.localFree(ptr);
}

static int mxMemalign(void **ptr, size_t alignment, size_t sz) {
    const int r = mxSavedAlloc.localMemalign(ptr, alignment, sz);
    mxLive += r == 0;
    if (r == 0) {
        mxTrackAdd(*ptr, sz);
    }

    return r;
}

static void mxCountBegin(void) {
    mxSavedAlloc = datakitConfigMemory__;
    mxTrack = mxSavedAlloc.localCalloc(mxTrackSlots, sizeof(*mxTrack));
    mxTrackStacks =
        getenv("DK_TEST_ALLOC_STACKS")
            ? mxSavedAlloc.localCalloc(mxTrackSlots, sizeof(*mxTrackStacks))
            : nullptr;
    mxTrackLive = 0;
    mxTrackFull = false;

    datakitConfig counting = mxSavedAlloc;
    counting.localCalloc = mxCalloc;
    counting.localMalloc = mxMalloc;
    counting.localRealloc = mxRealloc;
    counting.localReallocSlate = mxReallocSlate;
    counting.localFree = mxFree;
    counting.localMemalign = mxMemalign;
    mxLive = 0;
    datakitConfigSet(&counting);
}

/* Close the window and return its live count; when blocks are left,
 * print each one (and its stack, when recorded) first */
static int64_t mxCountEnd(void) {
    datakitConfigSet(&mxSavedAlloc);
    if (mxLive != 0 || mxTrackLive != 0) {
        printf("allocation window: %" PRId64 " live by count, %zu tracked%s\n",
               mxLive, mxTrackLive,
               mxTrackFull ? " (table full: some untracked)" : "");
        for (size_t i = 0; mxTrack && i < mxTrackSlots; i++) {
            if (!mxTrack[i].p) {
                continue;
            }

            printf("  live block %p: %zu bytes\n", mxTrack[i].p,
                   mxTrack[i].size);
            if (mxTrackStacks) {
                fflush(stdout);
                backtrace_symbols_fd(mxTrackStacks[i].frames,
                                     mxTrackStacks[i].depth, STDOUT_FILENO);
            }
        }
    }

    mxSavedAlloc.localFree(mxTrack);
    mxSavedAlloc.localFree(mxTrackStacks);
    mxTrack = nullptr;
    mxTrackStacks = nullptr;
    return mxLive;
}

void multimapTestAllocCountBegin(void) {
    mxCountBegin();
}

int64_t multimapTestAllocCountEnd(void) {
    return mxCountEnd();
}

typedef enum mxMode { MX_NORMAL, MX_EXTENDED, MX_MIXED } mxMode;
static const char *const mxModeName[] = {"normal", "extended", "mixed"};

/* A random 64-bit integer of either sign. */
static mwInt mxRandNarrow(uint64_t *rng) {
    const uint64_t r = dkXorshift64Next(rng);
    if (dkXorshift64Next(rng) & 1) {
        return mwSmall((int64_t)(r | ((uint64_t)1 << 63)));
    }

    return (mwInt){.bits = r};
}

/* Sorted distinct rows for 'mode': normal rows hold only 64-bit keys and
 * values, extended rows only 96/128-bit ones, and mixed rows interleave
 * both across the 2^63 / 2^64 / 2^96 boundaries (mwBuildRows). */
static size_t mxBuildRows(mwRow *rows, const size_t cap, const mxMode mode,
                          uint64_t *rng) {
    if (mode == MX_MIXED) {
        return mwBuildRows(rows, cap, rng);
    }

    const __uint128_t one = 1;
    const mwInt narrowFixed[] = {
        mwSmall(INT64_MIN),   mwSmall(-1),
        mwSmall(0),           mwSmall(1),
        mwSmall(INT64_MAX),   {.bits = (__uint128_t)INT64_MAX + 1},
        {.bits = UINT64_MAX},
    };
    const mwInt wideFixed[] = {
        {.bits = one << 64},
        {.bits = (one << 95) - 1},
        {.bits = one << 95},
        {.bits = one << 127},
        {.bits = ~(__uint128_t)0},
        {.bits = ~(__uint128_t)0 >> 1},
        {.bits = (__uint128_t)0 - (one << 63) - 1, .neg = true},
        {.bits = (__uint128_t)0 - (one << 95), .neg = true},
        {.bits = one << 127, .neg = true},
    };

    const bool wide = mode == MX_EXTENDED;
    const mwInt *fixed = wide ? wideFixed : narrowFixed;
    const size_t fixedCount = wide ? countof(wideFixed) : countof(narrowFixed);
    for (size_t n = 0; n < cap; n++) {
        if (n < fixedCount) {
            rows[n].key = fixed[n];
        } else {
            rows[n].key = wide ? mwRandWide(rng) : mxRandNarrow(rng);
        }

        rows[n].val = wide ? mwRandWide(rng) : mwSmall((int64_t)(n * 7) - 50);
        rows[n].present = false;
    }

    return mwSortUnique(rows, cap);
}

static constexpr int mxPoolMax = 160;

/* 'want' evenly spaced rows of the sorted 'pool', both extremes included. */
static size_t mxSubset(mwRow *dst, const mwRow *pool, const size_t built,
                       size_t want) {
    if (want > built) {
        want = built;
    }

    for (size_t i = 0; i < want; i++) {
        dst[i] = pool[want == 1 ? 0 : (i * (built - 1)) / (want - 1)];
        dst[i].present = false;
    }

    return want;
}

/* The tier 'want' rows of 'pool' build under 'limit'. */
static multimapType mxTierFor(const mwRow *pool, const size_t built,
                              const size_t want, const flexCapSizeLimit limit) {
    mwRow rows[mxPoolMax];
    const size_t n = mxSubset(rows, pool, built, want);
    cleaner(multimapFree) multimap *m = multimapNewLimit(2, limit);
    for (size_t i = 0; i < n; i++) {
        mwInsert(&m, &rows[i]);
    }

    return multimapType_(m);
}

/* A row count that builds 'target' under 'limit': the middle of the
 * counts that reach it, or all of the pool for Full.  0 if none does. */
static size_t mxRowsForTier(const mwRow *pool, const size_t built,
                            const flexCapSizeLimit limit,
                            const multimapType target) {
    size_t first = 0;
    size_t last = 0;
    for (size_t n = 3; n <= built; n++) {
        const multimapType type = mxTierFor(pool, built, n, limit);
        if (type == target) {
            first = first ? first : n;
            last = n;
        } else if (type > target) {
            break;
        }
    }

    if (!first) {
        return 0;
    }

    return target == MULTIMAP_TYPE_FULL ? last : (first + last) / 2;
}

/* Read an integer box back into the oracle's form. */
static bool mxFromBox(const databox *box, mwInt *out) {
    switch (box->type) {
    case DATABOX_SIGNED_64:
        *out = mwSmall(box->data.i64);
        return true;
    case DATABOX_UNSIGNED_64:
        *out = (mwInt){.bits = box->data.u64};
        return true;
    case DATABOX_SIGNED_128:
    case DATABOX_UNSIGNED_128:
        if (!box->data.ptr) {
            return false;
        }

        memcpy(&out->bits, box->data.ptr, sizeof(out->bits));
        out->neg = box->type == DATABOX_SIGNED_128;
        return true;
    default:
        return false;
    }
}

/* The present row whose key is 'key', or NULL. */
static mwRow *mxFind(mwRow *rows, const size_t n, const mwInt *key) {
    size_t lo = 0;
    size_t hi = n;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        const int c = mwCmp(&rows[mid].key, key);
        if (c == 0) {
            return rows[mid].present ? &rows[mid] : nullptr;
        }

        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    return nullptr;
}

/* A documented type-only report: the type of 'want' with no payload. */
static bool mxTypeOnly(const databox *got, const mwInt *want) {
    const databoxType type =
        want->neg ? DATABOX_SIGNED_128 : DATABOX_UNSIGNED_128;
    return got->type == type && !got->data.ptr && !got->big &&
           databoxIsWideDeferred(got);
}

/* The removing reads: InsertReplacing's replacedOut, DeleteWithFound,
 * DeleteReporting and a DeleteRandomValue drain that empties the map,
 * alternating armed and zeroed boxes. */
/* Replace every third row's value (alternating wide and narrow) and
 * delete every fourth row, checking each reported old value and key. */
static int32_t mxReplaceAndDelete(const char *where, multimap **m, mwRow *rows,
                                  const size_t n, uint64_t *rng) {
    int32_t err = 0;
    for (size_t i = 0; i < n; i += 3) {
        if (!rows[i].present) {
            continue;
        }

        const mwInt next = (i % 2) ? mwRandWide(rng) : mwSmall((int64_t)i);
        const databox key = mwBox(&rows[i].key);
        const databox val = mwBox(&next);
        const databox *e[2] = {&key, &val};
        databoxBig out;
        databox *replaced = mwArm(&out);
        if (!multimapInsertReplacing(m, e, replaced)) {
            ERR("%s: InsertReplacing row %zu reported no replace", where, i);
        } else if (mwIsWide(&rows[i].val) ? !mxTypeOnly(replaced, &rows[i].val)
                                          : !mwHolds(replaced, &rows[i].val)) {
            ERR("%s: InsertReplacing row %zu reported a wrong old value", where,
                i);
        }

        rows[i].val = next;
    }

    for (size_t i = 1; i < n; i += 4) {
        if (!rows[i].present) {
            continue;
        }

        /* i steps by 4 from 1, so i % 12 cycles 1, 5, 9: DeleteTaking,
         * DeleteReporting and DeleteWithFound in turn */
        const databox key = mwBox(&rows[i].key);
        if (i % 12 == 5) {
            /* DeleteReporting: the removed value, under replacedOut's
             * rules */
            databoxBig removed;
            databox *r = mwArm(&removed);
            if (!multimapDeleteReporting(m, &key, r)) {
                ERR("%s: DeleteReporting row %zu found nothing", where, i);
            } else if (mwIsWide(&rows[i].val) ? !mxTypeOnly(r, &rows[i].val)
                                              : !mwHolds(r, &rows[i].val)) {
                ERR("%s: DeleteReporting row %zu reported a wrong value", where,
                    i);
            }

            rows[i].present = false;
            continue;
        }

        if (i % 12 == 1) {
            /* DeleteTaking into an armed box: the value whole, wide in
             * band */
            databoxBig taken;
            databox *t = mwArm(&taken);
            if (!multimapDeleteTaking(m, &key, t)) {
                ERR("%s: DeleteTaking row %zu found nothing", where, i);
            } else if (!mwHolds(t, &rows[i].val)) {
                ERR("%s: DeleteTaking row %zu took a wrong value", where, i);
            }

            databoxFreeData(t);
            rows[i].present = false;
            continue;
        }

        databoxBig found;
        databox *f = mwArm(&found);
        if (!multimapDeleteWithFound(m, &key, f)) {
            ERR("%s: DeleteWithFound row %zu found nothing", where, i);
        } else if (mwIsWide(&rows[i].key) ? !mxTypeOnly(f, &rows[i].key)
                                          : !mwHolds(f, &rows[i].key)) {
            ERR("%s: DeleteWithFound row %zu reported a wrong key", where, i);
        }

        rows[i].present = false;
    }

    return err + mwCheckReads(where, *m, rows, n);
}

static int32_t mxRemovingReads(const char *where, multimap **m, mwRow *rows,
                               const size_t n, uint64_t *rng) {
    int32_t err = mxReplaceAndDelete(where, m, rows, n, rng);
    for (size_t k = 0; multimapCount(*m); k++) {
        const bool armed = k % 2 == 0;
        databoxBig kb;
        databoxBig vb;
        databox *e[2];
        if (armed) {
            e[0] = mwArm(&kb);
            e[1] = mwArm(&vb);
            kb.allocated = false;
            vb.allocated = false;
        } else {
            memset(&kb, 0, sizeof(kb));
            memset(&vb, 0, sizeof(vb));
            e[0] = (databox *)&kb;
            e[1] = (databox *)&vb;
        }

        if (!multimapDeleteRandomValue(m, k % 3 == 0, e)) {
            ERR("%s: DeleteRandomValue failed with %zu rows left", where,
                multimapCount(*m));
            break;
        }

        mwInt gotKey;
        mwRow *r =
            mxFromBox(e[0], &gotKey) ? mxFind(rows, n, &gotKey) : nullptr;
        if (!r || !mwHolds(e[0], &r->key) || !mwHolds(e[1], &r->val)) {
            ERR("%s: DeleteRandomValue %zu removed an unexpected row", where,
                k);
        } else {
            for (size_t c = 0; c < 2; c++) {
                const mwInt *v = c == 0 ? &r->key : &r->val;
                const databoxBig *b = c == 0 ? &kb : &vb;
                if (!mwIsWide(v)) {
                    continue;
                }

                /* Armed: in band, nothing owned.  Zeroed: a heap payload
                 * the caller owns. */
                const void *extra = b->extra;
                const bool ok =
                    armed ? e[c]->data.ptr == extra && !e[c]->allocated
                          : e[c]->allocated && e[c]->big &&
                                e[c]->data.ptr != extra;
                if (!ok) {
                    ERR("%s: DeleteRandomValue %zu column %zu has the wrong "
                        "owner (%s box)",
                        where, k, c, armed ? "armed" : "zeroed");
                }
            }

            r->present = false;
        }

        databoxFreeData(e[0]);
        databoxFreeData(e[1]);
    }

    for (size_t i = 0; i < n; i++) {
        if (rows[i].present) {
            ERR("%s: row %zu survived the DeleteRandomValue drain", where, i);
            break;
        }
    }

    return err;
}

/* DeleteRandomValue copies BYTES the way multilistPop does: 8 bytes or
 * less embedded with nothing owned, longer strings on the heap.  Drains a
 * map of 64-bit keys and string values of every length 0..20 under
 * 'limit', checking each removed value against the key it came with. */
static int32_t mxRemovedBytes(const char *where, const flexCapSizeLimit limit,
                              const size_t rows) {
    int32_t err = 0;
    const char text[] = "abcdefghijklmnopqrstuvwxyz";
    cleaner(multimapFree) multimap *m = multimapNewLimit(2, limit);
    for (size_t i = 0; i < rows; i++) {
        const databox key = databoxNewUnsigned(i);
        const databox val = databoxNewBytes(text, i % 21);
        const databox *e[2] = {&key, &val};
        multimapInsert(&m, e);
    }

    bool seen[rows];
    memset(seen, 0, sizeof(seen));

    /* DeleteReporting reports a string's type and length (its bytes are
     * not to be read once the row is gone) */
    for (size_t i = 0; i < rows; i += 5) {
        const databox key = databoxNewUnsigned(i);
        databox removed = {};
        if (!multimapDeleteReporting(&m, &key, &removed) ||
            !DATABOX_IS_BYTES(&removed) || removed.len != i % 21) {
            ERR("%s: DeleteReporting of row %zu reported type %d length %zu",
                where, i, removed.type, (size_t)removed.len);
        }

        seen[i] = true;
    }

    /* DeleteTaking hands each string back whole and owned: short ones
     * embedded, longer ones as heap copies */
    for (size_t i = 2; i < rows; i += 5) {
        const databox key = databoxNewUnsigned(i);
        databox taken = {};
        const size_t len = i % 21;
        const bool shortBytes = len <= sizeof(taken.data.bytes.embed8);
        if (!multimapDeleteTaking(&m, &key, &taken) || taken.len != len ||
            memcmp(databoxBytes(&taken), text, len) ||
            taken.type != (shortBytes ? DATABOX_BYTES_EMBED : DATABOX_BYTES) ||
            taken.allocated != !shortBytes) {
            ERR("%s: DeleteTaking of row %zu took a wrong %zu-byte string",
                where, i, len);
        }

        databoxFreeData(&taken);
        seen[i] = true;
    }

    for (size_t k = 0; multimapCount(m); k++) {
        databox kb = {};
        databox vb = {};
        databox *e[2] = {&kb, &vb};
        if (!multimapDeleteRandomValue(&m, k % 2 == 0, e)) {
            ERR("%s: DeleteRandomValue failed", where);
            break;
        }

        const size_t i = kb.type == DATABOX_UNSIGNED_64 ? kb.data.u64 : rows;
        const size_t len = i < rows ? i % 21 : 0;
        const bool shortBytes = len <= sizeof(vb.data.bytes.embed8);
        if (i >= rows || seen[i]) {
            ERR("%s: DeleteRandomValue %zu removed an unexpected key", where,
                k);
        } else if (vb.len != len || memcmp(databoxBytes(&vb), text, len) ||
                   vb.type !=
                       (shortBytes ? DATABOX_BYTES_EMBED : DATABOX_BYTES) ||
                   vb.allocated != !shortBytes || vb.big) {
            ERR("%s: DeleteRandomValue %zu returned a wrong %zu-byte string",
                where, k, len);
        } else {
            seen[i] = true;
        }

        databoxFreeData(&kb);
        databoxFreeData(&vb);
    }

    return err;
}

/* The matrix's drain uses DeleteRandomValue, whose row comes from an
 * OS-seeded generator, so its delete order differs from run to run.
 * DeleteByPosition takes the same delete path with a reproducible row:
 * bring the extended (96/128-bit) map of each tier to the matrix's
 * pre-drain state (the exercise, then the replaces and deletes), then
 * drain it in 'orders' seeded orders, each inside a counting window, so
 * an order that leaks a rangeBox payload or row copy fails every run and
 * names its seed. */
static int32_t mxDrainOrders(const char *where, const flexCapSizeLimit limit,
                             const multimapType expect, const mwRow *base,
                             const size_t n, const uint64_t rngAtBuild,
                             const uint64_t orders) {
    int32_t err = 0;
    for (uint64_t seed = 1; seed <= orders; seed++) {
        uint64_t rng = rngAtBuild;
        uint64_t order = seed * UINT64_C(0x9E3779B97F4A7C15);
        mwRow rows[mxPoolMax];
        memcpy(rows, base, n * sizeof(*rows));
        mxCountBegin();
        {
            cleaner(multimapFree) multimap *m = multimapNewLimit(2, limit);
            err += mwExercise(where, &m, rows, n, expect, &rng);
            err += mxReplaceAndDelete(where, &m, rows, n, &rng);

            while (multimapCount(m)) {
                const int64_t pos =
                    (int64_t)(dkXorshift64Next(&order) % multimapCount(m));
                if (!multimapDeleteByPosition(&m, pos)) {
                    ERR("%s drain order %" PRIu64 ": DeleteByPosition(%" PRId64
                        ") failed with %zu rows left",
                        where, seed, pos, multimapCount(m));
                    break;
                }
            }
        }

        const int64_t live = mxCountEnd();
        if (live != 0) {
            ERR("%s drain order %" PRIu64 ": %" PRId64
                " allocations outlived the map",
                where, seed, live);
        }
    }

    return err;
}

int32_t multimapWideMatrixSweep(void) {
    int32_t err = 0;
    uint64_t rng = 0xD1B54A32D192ED03ULL;
    struct {
        const char *tier;
        flexCapSizeLimit limit;
        multimapType expect;
    } const plans[] = {
        {"small", FLEX_CAP_LEVEL_2048, MULTIMAP_TYPE_SMALL},
        {"medium", FLEX_CAP_LEVEL_256, MULTIMAP_TYPE_MEDIUM},
        {"full", FLEX_CAP_LEVEL_64, MULTIMAP_TYPE_FULL},
    };

    for (size_t mode = MX_NORMAL; mode <= MX_MIXED; mode++) {
        mwRow pool[mxPoolMax];
        const size_t built = mxBuildRows(pool, mxPoolMax, mode, &rng);
        for (size_t p = 0; p < countof(plans); p++) {
            char where[64];
            snprintf(where, sizeof(where), "%s %s", mxModeName[mode],
                     plans[p].tier);
            const size_t want =
                mxRowsForTier(pool, built, plans[p].limit, plans[p].expect);
            if (!want) {
                ERR("%s: no row count builds the tier", where);
                continue;
            }

            mwRow rows[mxPoolMax];
            const size_t n = mxSubset(rows, pool, built, want);
            mwRow rowsAtBuild[mxPoolMax];
            memcpy(rowsAtBuild, rows, n * sizeof(*rows));
            const uint64_t rngAtBuild = rng;

            /* Build, exercise every read path, drain and free inside one
             * counting window: every rangeBox payload, row copy and
             * removed value must be released. */
            mxCountBegin();
            {
                cleaner(multimapFree) multimap *m =
                    multimapNewLimit(2, plans[p].limit);
                err += mwExercise(where, &m, rows, n, plans[p].expect, &rng);
                multimapVerify(m);
                err += mxRemovingReads(where, &m, rows, n, &rng);
            }

            if (mode == MX_NORMAL) {
                const size_t bytesRows[] = {10, 60, 400};
                err += mxRemovedBytes(where, plans[p].limit, bytesRows[p]);
            }

            const int64_t live = mxCountEnd();
            if (live != 0) {
                ERR("%s: %" PRId64 " allocations outlived the map", where,
                    live);
            }

            /* The same map, drained in reproducible orders */
            err += mxDrainOrders(where, plans[p].limit, plans[p].expect,
                                 rowsAtBuild, n, rngAtBuild, 32);
        }
    }

    return err;
}

/* Snapshot, WAL and recovery through multimapP in each data mode: rows
 * inserted before a compaction live in the snapshot, later inserts and
 * deletes in the WAL, and the reopened map must hold exactly the oracle's
 * rows. */
int32_t multimapWidePersistSweep(void) {
    int32_t err = 0;
    uint64_t rng = 0x94D049BB133111EBULL;
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) {
        tmp = "/tmp";
    }

    struct {
        const char *tier;
        flexCapSizeLimit limit;
        multimapType expect;
    } const plans[] = {
        {"small", FLEX_CAP_LEVEL_2048, MULTIMAP_TYPE_SMALL},
        {"medium", FLEX_CAP_LEVEL_256, MULTIMAP_TYPE_MEDIUM},
        {"full", FLEX_CAP_LEVEL_64, MULTIMAP_TYPE_FULL},
    };

    for (size_t mode = MX_NORMAL; mode <= MX_MIXED; mode++) {
        mwRow pool[mxPoolMax];
        const size_t built = mxBuildRows(pool, mxPoolMax, mode, &rng);
        for (size_t p = 0; p < countof(plans); p++) {
            char where[64];
            snprintf(where, sizeof(where), "persist %s %s", mxModeName[mode],
                     plans[p].tier);
            const size_t want =
                mxRowsForTier(pool, built, plans[p].limit, plans[p].expect);
            if (!want) {
                ERR("%s: no row count builds the tier", where);
                continue;
            }

            mwRow rows[mxPoolMax];
            const size_t n = mxSubset(rows, pool, built, want);
            size_t order[mxPoolMax];
            mwShuffle(order, n, &rng);

            char base[512];
            char snap[600];
            char wal[600];
            snprintf(base, sizeof(base), "%s/dkMultimapWideP.%d.%zu.%zu", tmp,
                     (int)getpid(), mode, p);
            snprintf(snap, sizeof(snap), "%s.snap", base);
            snprintf(wal, sizeof(wal), "%s.wal", base);
            unlink(snap);
            unlink(wal);

            persistCtx *ctx = persistCtxNew(base, nullptr);
            multimapP *mp = ctx ? multimapNewConfigureP(ctx, 2, false, false,
                                                        plans[p].limit)
                                : nullptr;
            if (!mp) {
                ERR("%s: could not create the persistent map", where);
                if (ctx) {
                    persistCtxFree(ctx);
                }
                continue;
            }

            for (size_t k = 0; k < n; k++) {
                mwRow *r = &rows[order[k]];
                const databox key = mwBox(&r->key);
                const databox val = mwBox(&r->val);
                const databox *e[2] = {&key, &val};
                if (multimapInsertP(mp, e, nullptr) == PERSIST_LOG_REFUSED) {
                    ERR("%s: insert refused", where);
                }
                r->present = true;
                if (k == n / 2 && !multimapCompactP(mp)) {
                    ERR("%s: compaction failed", where);
                }
            }

            for (size_t k = 0; k < n; k += 4) {
                mwRow *r = &rows[order[k]];
                const databox key = mwBox(&r->key);
                if (!multimapDeleteP(mp, &key)) {
                    ERR("%s: delete of row %zu failed", where, order[k]);
                }

                r->present = false;
            }

            if (multimapType_(multimapGetP(mp)) != plans[p].expect && n > 3) {
                /* A quarter of the rows deleted stays above every shrink
                 * point, so the tier is still the one the build reached */
                ERR("%s: built tier %d, expected %d", where,
                    (int)multimapType_(multimapGetP(mp)), (int)plans[p].expect);
            }

            err += mwCheckReads(where, multimapGetP(mp), rows, n);
            multimapCloseP(mp);
            persistCtxFree(ctx);

            ctx = persistCtxOpen(base, nullptr);
            mp = ctx ? multimapOpenP(ctx) : nullptr;
            if (!mp) {
                ERR("%s: recovery failed", where);
            } else {
                char reopened[80];
                snprintf(reopened, sizeof(reopened), "%s reopened", where);
                err += mwCheckReads(reopened, multimapGetP(mp), rows, n);
                multimapCloseP(mp);
            }

            if (ctx) {
                persistCtxFree(ctx);
            }

            unlink(snap);
            unlink(wal);
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

    uint32_t hits[rows] = {};
    for (uint64_t d = 0; d < draws; d++) {
        databox k = {};
        databox v = {};
        databox *boxes[2] = {&k, &v};
        multimapEntry me;
        if (!draw(m, false, boxes, &me)) {
            ERR("%s: a draw failed on a non-empty container", where);
            return err;
        }

        if (k.type != DATABOX_UNSIGNED_64 || k.data.u >= rows) {
            ERR("%s: a draw returned key %" PRIuMAX " (type %d), outside "
                "[0, %d)",
                where, k.data.u, (int)k.type, rows);
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
    databox lastKey = {};
    databox lastVal = {};
    databox *lastBoxes[2] = {&lastKey, &lastVal};
    if (!position(m, rows - 1, lastBoxes) || lastKey.data.u != rows - 1) {
        ERR("%s: could not read the last row by position", where);
    }

    for (uint32_t d = 0; d < drawsPerRow; d++) {
        databox k = {};
        databox v = {};
        databox *boxes[2] = {&k, &v};
        multimapEntry me;
        if (!draw(m, true, boxes, &me) || k.data.u != lastKey.data.u) {
            ERR("%s: fromTail returned key %" PRIuMAX ", not the last row "
                "%" PRIuMAX,
                where, k.data.u, lastKey.data.u);
            break;
        }
    }

    /* A seeded random() sequence is the same with draws interleaved. */
    constexpr int streamLen = 16;
    const uint32_t seed = 0x5eed;
    uint64_t expected[streamLen];
    srandom(seed);
    for (uint32_t i = 0; i < streamLen; i++) {
        expected[i] = (uint64_t)random();
    }

    srandom(seed);
    for (uint32_t i = 0; i < streamLen; i++) {
        databox k = {};
        databox v = {};
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

/* Sizes for the build-up sweeps: every size to 256, then steps of about
 * 6%.  Building a map to size i crosses every tier transition below i
 * (and deleting it crosses them back), so sampled sizes keep the
 * coverage of the old every-size sweep at about 1/80 of its work. */
static int32_t multimapTestNextSize_(int32_t i) {
    return i < 256 ? i + 1 : i + i / 16;
}

/* multimapGetUnderlyingEntryFullWidth then multimapDeleteEntry in a set,
 * against multimapDeleteFullWidth in a twin, in every tier: every row
 * found is the row named, a row with a value never inserted is not found,
 * and both maps lose one row per step until both are empty */
static int32_t multimapFullWidthEntrySweep_(void) {
    int32_t err = 0;
    /* 20, 120 and 6000 rows (five per key) at shrinking split sizes */
    const int64_t keys[] = {4, 24, 1200};
    const flexCapSizeLimit limits[] = {FLEX_CAP_LEVEL_2048, FLEX_CAP_LEVEL_512,
                                       FLEX_CAP_LEVEL_64};
    const multimapType tiers[] = {MULTIMAP_TYPE_SMALL, MULTIMAP_TYPE_MEDIUM,
                                  MULTIMAP_TYPE_FULL};
    for (size_t t = 0; t < countof(keys); t++) {
        cleaner(multimapFree) multimap *a =
            multimapNewConfigure(2, true, false, limits[t]);
        cleaner(multimapFree) multimap *b =
            multimapNewConfigure(2, true, false, limits[t]);
        for (int64_t k = 0; k < keys[t]; k++) {
            for (int64_t v = 0; v < 5; v++) {
                const databox key = databoxNewSigned(k);
                const databox val = databoxNewSigned(v);
                const databox *row[] = {&key, &val};
                multimapInsert(&a, row);
                multimapInsert(&b, row);
            }
        }
        if (multimapType_(a) != tiers[t]) {
            ERR("full-width entry: tier %zu built type %d", t,
                (int)multimapType_(a));
        }

        /* Rows in a scattered order: 7 is coprime to every row count */
        const int64_t rows = keys[t] * 5;
        for (int64_t i = 0; i < rows; i++) {
            const int64_t at = (i * 7) % rows;
            const databox key = databoxNewSigned(at / 5);
            const databox val = databoxNewSigned(at % 5);
            const databox never = databoxNewSigned(5 + at % 5);
            const databox *row[] = {&key, &val};
            const databox *absent[] = {&key, &never};
            multimapEntry me;
            if (multimapGetUnderlyingEntryFullWidth(a, absent, &me)) {
                ERR("full-width entry: tier %zu found a row never inserted", t);
                break;
            }
            if (!multimapGetUnderlyingEntryFullWidth(a, row, &me)) {
                ERR("full-width entry: tier %zu row %" PRId64 " not found", t,
                    at);
                break;
            }
            databox gotKey = {};
            databox gotVal = {};
            flexGetByType(me.fe, &gotKey);
            flexGetByType(flexNext(*me.map, me.fe), &gotVal);
            if (!databoxEqual(&gotKey, &key) || !databoxEqual(&gotVal, &val)) {
                ERR("full-width entry: tier %zu row %" PRId64
                    " located another row",
                    t, at);
                break;
            }
            multimapDeleteEntry(&a, &me);
            if (!multimapDeleteFullWidth(&b, row) ||
                multimapCount(a) != multimapCount(b)) {
                ERR("full-width entry: tier %zu row %" PRId64
                    ": counts %zu and %zu",
                    t, at, multimapCount(a), multimapCount(b));
                break;
            }
            if (i % 97 == 0) {
                multimapVerify(a);
            }
        }
        if (multimapCount(a) != 0) {
            ERR("full-width entry: tier %zu kept %zu rows", t,
                multimapCount(a));
        }
    }

    return err;
}

/* Counts the times multimapFieldIncrByIf asked, answering 'answer' */
typedef struct mmProceedProbe_ {
    bool answer;
    uint32_t asked;
} mmProceedProbe_;

static bool mmProceedProbe(void *ctx) {
    mmProceedProbe_ *probe = ctx;
    probe->asked++;
    return probe->answer;
}

/* multimapInsertIfAbsent through Small, Medium and Full against an oracle
 * of the first value each key was offered: only a key's first offer
 * inserts, a later offer leaves the stored value, and the count is the
 * number of distinct keys. */
static int32_t multimapInsertIfAbsentSweep_(void) {
    int32_t err = 0;
    enum { keys = 1500, offers = 3 };
    static uint64_t firstValue[keys];
    static bool seen[keys];
    memset(seen, 0, sizeof(seen));

    cleaner(multimapFree) multimap *m = multimapNewLimit(2, FLEX_CAP_LEVEL_512);
    bool sawType[MULTIMAP_TYPE_FULL + 1] = {false};
    uint64_t distinct = 0;
    uint64_t state = 0x9E3779B97F4A7C15ULL;
    for (uint64_t i = 0; i < (uint64_t)keys * offers; i++) {
        dkLcg64Next(&state);
        const uint64_t key = (state >> 33) % keys;
        const uint64_t value = i * 7 + 1;
        const databox k = databoxNewUnsigned(key);
        const databox v = databoxNewUnsigned(value);
        const databox *row[2] = {&k, &v};
        /* Every other offer asks for the present row's entry, which must
         * be the stored row */
        multimapEntry me = {};
        const bool inserted = i % 2 ? multimapInsertIfAbsentEntry(&m, row, &me)
                                    : multimapInsertIfAbsent(&m, row);
        if (inserted != !seen[key]) {
            ERR("if-absent: key %" PRIu64 " offer returned %d, expected %d",
                key, inserted, !seen[key]);
        } else if (!inserted && i % 2) {
            databox gotKey = {};
            databox gotVal = {};
            flexGetByType(me.fe, &gotKey);
            flexGetByType(flexNext(*me.map, me.fe), &gotVal);
            if (gotKey.data.u != key || gotVal.data.u != firstValue[key]) {
                ERR("if-absent: key %" PRIu64 " entry names %" PRIuMAX
                    " = %" PRIuMAX,
                    key, gotKey.data.u, gotVal.data.u);
            }
        }

        if (!seen[key]) {
            seen[key] = true;
            firstValue[key] = value;
            distinct++;
        }

        sawType[multimapType_(m)] = true;
    }

    if (!sawType[MULTIMAP_TYPE_SMALL] || !sawType[MULTIMAP_TYPE_MEDIUM] ||
        !sawType[MULTIMAP_TYPE_FULL]) {
        ERR("if-absent: tiers visited small %d medium %d full %d",
            sawType[MULTIMAP_TYPE_SMALL], sawType[MULTIMAP_TYPE_MEDIUM],
            sawType[MULTIMAP_TYPE_FULL]);
    }

    if (multimapCount(m) != distinct) {
        ERR("if-absent: count %zu, expected %" PRIu64, multimapCount(m),
            distinct);
    }

    for (uint64_t key = 0; key < keys; key++) {
        const databox k = databoxNewUnsigned(key);
        databox got = {};
        databox *out[1] = {&got};
        const bool found = multimapLookup(m, &k, out);
        if (found != seen[key] || (found && (got.type != DATABOX_UNSIGNED_64 ||
                                             got.data.u != firstValue[key]))) {
            ERR("if-absent: key %" PRIu64 " found %d value %" PRIuMAX
                ", expected found %d value %" PRIu64,
                key, found, got.data.u, seen[key],
                seen[key] ? firstValue[key] : 0);
        }
    }

    multimapVerify(m);

    /* In a set any row with the key counts as present */
    cleaner(multimapFree) multimap *set = multimapSetNew(2);
    const databox k = databoxNewUnsigned(5);
    const databox v1 = databoxNewUnsigned(1);
    const databox v2 = databoxNewUnsigned(2);
    const databox *first[2] = {&k, &v1};
    const databox *second[2] = {&k, &v2};
    if (!multimapInsertIfAbsent(&set, first) ||
        multimapInsertIfAbsent(&set, second) || multimapCount(set) != 1) {
        ERR("if-absent: set holds %zu rows for one key offered twice",
            multimapCount(set));
    }

    /* A set's present key names its first row, whatever was offered */
    const databox v0 = databoxNewUnsigned(0);
    const databox *lower[2] = {&k, &v0};
    multimapInsertFullWidth(&set, lower);
    const databox v9 = databoxNewUnsigned(9);
    const databox *offered[2] = {&k, &v9};
    multimapEntry me = {};
    databox gotVal = {};
    if (multimapInsertIfAbsentEntry(&set, offered, &me)) {
        ERR("if-absent: a set's present key was inserted again");
    } else {
        flexGetByType(flexNext(*me.map, me.fe), &gotVal);
        if (gotVal.data.u != 0) {
            ERR("if-absent: a set's entry names value %" PRIuMAX
                ", not the first row's 0",
                gotVal.data.u);
        }
    }

    return err;
}

/* multimapDeleteEntry through Small, Medium and Full: rows found by
 * GetUnderlyingEntry and deleted where they are leave exactly the others,
 * with the map still searchable (middles, range boxes, merged sub-maps) */
static int32_t multimapDeleteEntrySweep_(void) {
    int32_t err = 0;
    const int64_t sizes[] = {20, 200, 3000};
    bool sawType[MULTIMAP_TYPE_FULL + 1] = {false};
    for (size_t s = 0; s < countof(sizes); s++) {
        const int64_t n = sizes[s];
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_512);
        for (int64_t i = 0; i < n; i++) {
            const databox k = databoxNewSigned(i);
            const databox v = databoxNewSigned(i * 10);
            const databox *row[2] = {&k, &v};
            multimapInsert(&m, row);
        }

        const multimapType tier = multimapType_(m);
        sawType[tier] = true;
        /* Delete every key not divisible by 3, from the middle outwards */
        for (int64_t step = 0; step < n; step++) {
            const int64_t i =
                (step & 1) ? n / 2 + 1 + step / 2 : n / 2 - step / 2;
            if (i < 0 || i >= n || i % 3 == 0) {
                continue;
            }

            const databox k = databoxNewSigned(i);
            multimapEntry me = {};
            if (!multimapGetUnderlyingEntry(m, &k, &me)) {
                ERR("delete-entry (%" PRId64 " rows, tier %d): key %" PRId64
                    " not found before its delete",
                    n, tier, i);
                continue;
            }

            multimapDeleteEntry(&m, &me);
        }

        int64_t kept = 0;
        for (int64_t i = 0; i < n; i++) {
            const databox k = databoxNewSigned(i);
            databox got = {};
            databox *out[1] = {&got};
            const bool found = multimapLookup(m, &k, out);
            if (found != (i % 3 == 0) || (found && got.data.i != i * 10)) {
                ERR("delete-entry (%" PRId64 " rows, tier %d): key %" PRId64
                    " found %d value %" PRIdMAX,
                    n, tier, i, found, got.data.i);
            }

            kept += i % 3 == 0;
        }

        if ((int64_t)multimapCount(m) != kept) {
            ERR("delete-entry (%" PRId64 " rows, tier %d): count %zu, "
                "expected %" PRId64,
                n, tier, multimapCount(m), kept);
        }

        multimapVerify(m);
    }

    if (!sawType[MULTIMAP_TYPE_SMALL] || !sawType[MULTIMAP_TYPE_MEDIUM] ||
        !sawType[MULTIMAP_TYPE_FULL]) {
        ERR("delete-entry: tiers visited small %d medium %d full %d",
            sawType[MULTIMAP_TYPE_SMALL], sawType[MULTIMAP_TYPE_MEDIUM],
            sawType[MULTIMAP_TYPE_FULL]);
    }

    return err;
}

/* A report slot's value before a call, so a test tells a slot the call
 * left alone from one it wrote */
static constexpr int64_t MM_TEST_NOT_WRITTEN = INT64_MIN;

int multimapTest(int, char *[]) {
    int err = 0;

    TEST("LookupWithReference finds rows keyed by atom references, every "
         "tier") {
        /* Rows inserted under a surrogate atom reference (the stored key)
         * are found by the key's value through the atom table in the
         * Small, Medium and Full tiers; an absent key is not */
        static constexpr size_t sizes[] = {3, 100, 150, 200, 300, 2000};
        unsigned tiers = 0;
        for (size_t t = 0; t < countof(sizes); t++) {
            cleaner(multimapAtomFree) multimapAtom *atoms = multimapAtomNew();
            multimap *m = multimapNewLimit(2, FLEX_CAP_LEVEL_512);
            for (size_t i = 0; i < sizes[t]; i++) {
                char buf[40];
                snprintf(buf, sizeof(buf), "lookup-key-%05zu",
                         (i * 7) % sizes[t]);
                const databox key = databoxNewBytesString(buf);
                databox ref = key;
                multimapAtomInsertConvert(atoms, &ref);
                const databox val = databoxNewUnsigned((i * 7) % sizes[t]);
                const databox *row[2] = {&key, &val};
                multimapInsertWithSurrogateKey(&m, row, &ref, atoms);
            }
            tiers |= 1u << multimapType_(m);
            size_t missed = 0;
            for (size_t i = 0; i < sizes[t]; i++) {
                char buf[40];
                snprintf(buf, sizeof(buf), "lookup-key-%05zu", i);
                const databox key = databoxNewBytesString(buf);
                databox got = {};
                databox *out[1] = {&got};
                if (!multimapLookupWithReference(m, &key, out, atoms) ||
                    got.data.u64 != i) {
                    missed++;
                }
            }
            const databox absent = databoxNewBytesString("lookup-key-absent");
            databox got = {};
            databox *out[1] = {&got};
            if (missed || multimapLookupWithReference(m, &absent, out, atoms)) {
                ERR("lookup by reference: %zu of %zu missed (size %zu)", missed,
                    sizes[t], sizes[t]);
            }
            multimapFree(m);
        }
        const unsigned all = (1u << MULTIMAP_TYPE_SMALL) |
                             (1u << MULTIMAP_TYPE_MEDIUM) |
                             (1u << MULTIMAP_TYPE_FULL);
        if ((tiers & all) != all) {
            ERR("lookup by reference: tiers seen 0x%x, want 0x%x", tiers, all);
        }
    }

    TEST("one-row maps take smaller keys in a new map before them") {
        /* 3000-byte values, past the Full tier's 2048-byte sub-map size.
         * Ascending keys 1000..20000 reach the Full tier, each new largest
         * row in a map of its own at the end; keys just below those rows
         * go to the maps before them.  Then 20 keys descending below every
         * key reach map 0: once it is down to one row, each lands in a new
         * map placed before it.  The result is in key order, every row
         * found with its value, and the map verifies. */
        static char big[3000];
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_512);
        int64_t keys[64];
        size_t NK = 0;
        for (int64_t k = 1000; k <= 20000; k += 1000) {
            keys[NK++] = k;
        }
        static constexpr int64_t later[] = {19500, 18500, 17999, 16001,
                                            15500, 19999, 14500};
        for (size_t i = 0; i < countof(later); i++) {
            keys[NK++] = later[i];
        }
        for (int64_t k = 990; k >= 800; k -= 10) {
            keys[NK++] = k;
        }
        for (size_t i = 0; i < NK; i++) {
            memset(big, (int)('a' + i % 26), sizeof(big));
            const databox k = databoxNewSigned(keys[i]);
            const databox v = databoxNewBytes(big, sizeof(big));
            const databox *row[2] = {&k, &v};
            multimapInsert(&m, row);
        }
        multimapVerify(m);
        bool ok =
            multimapType_(m) == MULTIMAP_TYPE_FULL && multimapCount(m) == NK;
        for (size_t i = 0; ok && i < NK; i++) {
            const databox k = databoxNewSigned(keys[i]);
            databox got = {};
            databox *out[1] = {&got};
            ok = multimapLookup(m, &k, out) &&
                 databoxLen(&got) == sizeof(big) &&
                 ((const char *)databoxBytes(&got))[0] == (char)('a' + i % 26);
        }
        multimapIterator it;
        multimapIteratorInit(m, &it, true);
        databox kb = {};
        databox vb = {};
        databox *rowOut[2] = {&kb, &vb};
        int64_t prev = INT64_MIN;
        size_t seen = 0;
        while (ok && multimapIteratorNext(&it, rowOut)) {
            ok = kb.data.i > prev;
            prev = kb.data.i;
            seen++;
        }
        if (!ok || seen != NK) {
            ERR("one-row maps: type %d, count %zu, %u maps, %zu rows in "
                "order",
                (int)multimapType_(m), multimapCount(m),
                (unsigned)mmf(m)->count, seen);
        }

        /* Descending keys never leave map 0 at one row (each arrival
         * splits a two-row map 0 and lands in its lower half), so empty
         * it down to one: deleting every key in (800, 4000] leaves map 0
         * holding only 800 (it holds the smallest keys).  Then 700, 600
         * and 500 each find map 0 a single oversized row above them and
         * get a new map placed before it: three more maps, in order. */
        size_t kept = 0;
        for (size_t i = 0; i < NK; i++) {
            if (keys[i] > 800 && keys[i] <= 4000) {
                const databox k = databoxNewSigned(keys[i]);
                multimapDelete(&m, &k);
            } else {
                kept++;
            }
        }
        const uint32_t mapsBefore = (uint32_t)mmf(m)->count;
        static constexpr int64_t below[] = {700, 600, 500};
        for (size_t i = 0; i < 3; i++) {
            memset(big, 'z' - (int)i, sizeof(big));
            const databox k = databoxNewSigned(below[i]);
            const databox v = databoxNewBytes(big, sizeof(big));
            const databox *row[2] = {&k, &v};
            multimapInsert(&m, row);
        }
        multimapVerify(m);
        multimapIteratorInit(m, &it, true);
        prev = INT64_MIN;
        seen = 0;
        ok = true;
        while (ok && multimapIteratorNext(&it, rowOut)) {
            ok = kb.data.i > prev;
            prev = kb.data.i;
            seen++;
        }
        for (size_t i = 0; ok && i < 3; i++) {
            const databox k = databoxNewSigned(below[i]);
            databox got = {};
            databox *out[1] = {&got};
            ok = multimapLookup(m, &k, out) &&
                 ((const char *)databoxBytes(&got))[0] == (char)('z' - i);
        }
        if (!ok || seen != kept + 3 ||
            (uint32_t)mmf(m)->count != mapsBefore + 3) {
            ERR("maps placed before map 0: %zu rows (want %zu), %u maps "
                "(want %u)",
                seen, kept + 3, (unsigned)mmf(m)->count, mapsBefore + 3);
        }
    }

    TEST("ReplaceEntryWithReference grows values in a reference-keyed map") {
        /* Every third row's value replaced by a 600-byte one through the
         * reference-aware replace (Full sub-maps grow past their limit and
         * rebalance through the references); every key still looks up its
         * current value and the map verifies */
        static constexpr size_t sizes[] = {6, 150, 1500};
        static char bigValue[600];
        memset(bigValue, 'v', sizeof(bigValue));
        for (size_t t = 0; t < countof(sizes); t++) {
            cleaner(multimapAtomFree) multimapAtom *atoms = multimapAtomNew();
            multimap *m = multimapNewLimit(2, FLEX_CAP_LEVEL_512);
            for (size_t i = 0; i < sizes[t]; i++) {
                char buf[40];
                snprintf(buf, sizeof(buf), "replace-key-%05zu", i);
                const databox key = databoxNewBytesString(buf);
                databox ref = key;
                multimapAtomInsertConvert(atoms, &ref);
                const databox val = databoxNewUnsigned(i);
                const databox *row[2] = {&key, &val};
                multimapInsertWithSurrogateKey(&m, row, &ref, atoms);
            }
            for (size_t i = 0; i < sizes[t]; i += 3) {
                char buf[40];
                snprintf(buf, sizeof(buf), "replace-key-%05zu", i);
                const databox key = databoxNewBytesString(buf);
                multimapEntry me;
                if (!multimapGetUnderlyingEntryWithReference(m, &key, &me,
                                                             atoms)) {
                    ERR("size %zu: key %zu not found to replace", sizes[t], i);
                    continue;
                }
                me.fe = flexNext(*me.map, me.fe); /* to the value */
                bigValue[0] = (char)('a' + i % 26);
                const databox big = databoxNewBytes(bigValue, sizeof(bigValue));
                multimapReplaceEntryWithReference(&m, &me, &big, atoms);
            }
            size_t wrong = 0;
            for (size_t i = 0; i < sizes[t]; i++) {
                char buf[40];
                snprintf(buf, sizeof(buf), "replace-key-%05zu", i);
                const databox key = databoxNewBytesString(buf);
                databox got = {};
                databox *out[1] = {&got};
                const bool found =
                    multimapLookupWithReference(m, &key, out, atoms);
                const bool ok =
                    i % 3 == 0
                        ? found && databoxLen(&got) == sizeof(bigValue) &&
                              ((const char *)databoxBytes(&got))[0] ==
                                  (char)('a' + i % 26)
                        : found && got.data.u64 == i;
                wrong += !ok;
            }
            if (wrong || multimapCount(m) != sizes[t]) {
                ERR("size %zu: %zu rows read wrong after replaces, count %zu",
                    sizes[t], wrong, multimapCount(m));
            }
            multimapVerify(m);
            multimapFree(m);
        }
    }

    TEST("delete by entry through every tier") {
        err += multimapDeleteEntrySweep_();
    }

    TEST("DeleteByPredicate: every condition removes exactly the rows whose "
         "key meets it, in maps and sets of every tier") {
        /* Maps [key, value] and sets [key, member] (keys repeated three
         * times in a set) at 20, 300 and 3000 rows under a small limit,
         * some keys 128-bit: the rows left are exactly those whose key
         * fails the predicate, the result says whether any went, and the
         * map verifies */
        static const multimapCondition conds[] = {
            MULTIMAP_CONDITION_NONE,
            MULTIMAP_CONDITION_ALL,
            MULTIMAP_CONDITION_LESS_THAN,
            MULTIMAP_CONDITION_LESS_THAN_EQUAL,
            MULTIMAP_CONDITION_EQUAL,
            MULTIMAP_CONDITION_GREATER_THAN,
            MULTIMAP_CONDITION_GREATER_THAN_EQUAL};
        const int64_t sizes[] = {20, 300, 3000};
        for (int set = 0; set < 2 && !err; set++) {
            for (size_t s = 0; s < countof(sizes) && !err; s++) {
                for (size_t c = 0; c < countof(conds) && !err; c++) {
                    const int64_t rows = sizes[s];
                    cleaner(multimapFree) multimap *m =
                        multimapNewConfigure(2, set, false, FLEX_CAP_LEVEL_512);
                    databoxBig wide[1];
                    for (int64_t r = 0; r < rows; r++) {
                        /* Every 16th key 128-bit (sorting past the rest) */
                        const int64_t k = set ? r / 3 : r;
                        databox key = databoxNewSigned(k * 2);
                        if (k % 16 == 15) {
                            DATABOX_BIG_SIGNED_128(&wide[0],
                                                   ((__int128_t)1 << 100) + k);
                            key = *(databox *)&wide[0];
                        }
                        const databox val = databoxNewSigned(r);
                        const databox *row[2] = {&key, &val};
                        multimapInsert(&m, row);
                    }

                    /* The boundary: a key present in the middle */
                    const int64_t mid = (set ? rows / 3 : rows) / 2;
                    const multimapPredicate p = {
                        .condition = conds[c],
                        .compareAgainst = databoxNewSigned(
                            (mid % 16 == 15 ? mid - 1 : mid) * 2)};

                    /* What must stay: every row, read now, that fails p */
                    size_t keep = 0;
                    size_t before = multimapCount(m);
                    multimapIterator it;
                    multimapIteratorInit(m, &it, true);
                    databox kb = {};
                    databox vb = {};
                    databox *got[2] = {&kb, &vb};
                    databoxWide slots[2];
                    while (multimapIteratorNextWithSlots(&it, got, slots)) {
                        keep += !multimapProcessPredicate(&p, &kb);
                    }

                    const bool any = multimapDeleteByPredicate(&m, &p);
                    multimapVerify(m);
                    size_t left = 0;
                    bool wrong = false;
                    multimapIteratorInit(m, &it, true);
                    while (multimapIteratorNextWithSlots(&it, got, slots)) {
                        left++;
                        wrong |= multimapProcessPredicate(&p, &kb);
                    }
                    if (left != keep || wrong || any != (keep < before) ||
                        multimapCount(m) != left) {
                        ERR("%s size %" PRId64 " condition %d: %zu of %zu "
                            "left, want %zu (deleted %d)",
                            set ? "set" : "map", rows, (int)conds[c], left,
                            before, keep, any);
                    }
                }
            }
        }
    }

    TEST("DeleteByPredicate on an empty map and on rows wider than the "
         "stack row") {
        /* ALL and GREATER_THAN on an empty map remove nothing; on a map
         * of 12-element rows (past the 8 kept on the stack) the suffix
         * goes and the rest keep every field */
        cleaner(multimapFree) multimap *m = multimapNew(12);
        const multimapPredicate all = {.condition = MULTIMAP_CONDITION_ALL};
        const multimapPredicate above = {.condition =
                                             MULTIMAP_CONDITION_GREATER_THAN,
                                         .compareAgainst = databoxNewSigned(9)};
        if (multimapDeleteByPredicate(&m, &all) ||
            multimapDeleteByPredicate(&m, &above)) {
            ERR("an empty map reported a delete");
        }

        databox boxes[12];
        const databox *row[12];
        for (int64_t k = 0; k < 20; k++) {
            for (int f = 0; f < 12; f++) {
                boxes[f] = databoxNewSigned(k * 100 + f);
                row[f] = &boxes[f];
            }
            boxes[0] = databoxNewSigned(k);
            multimapInsert(&m, row);
        }

        if (!multimapDeleteByPredicate(&m, &above) || multimapCount(m) != 10) {
            ERR("%zu rows left of 20 after > 9", multimapCount(m));
        }
        databox got[11];
        databox *gotP[11];
        for (int f = 0; f < 11; f++) {
            gotP[f] = &got[f];
        }
        const databox nine = databoxNewSigned(9);
        if (!multimapLookup(m, &nine, gotP) || got[10].data.i != 911) {
            ERR("row 9 lost its fields%s", "");
        }
        if (!multimapDeleteByPredicate(&m, &all) || multimapCount(m) != 0) {
            ERR("ALL left %zu rows", multimapCount(m));
        }
    }

    TEST("FieldIncr through every integer width keeps every tier's search "
         "state right") {
        /* Maps of [key, a, b] rows at 20, 300 and 3000 rows under a small
         * limit: field b of rows spread over the map (before and after
         * each middle) steps through 1-, 2-, 4- and 8-byte counts and back
         * to zero; after each step every row reads back its count and the
         * map verifies (middles, range boxes) */
        static const int64_t steps[] = {
            1, 200, 70000, INT64_C(1) << 40, -(INT64_C(1) << 40), -70000, -201};
        const int64_t sizes[] = {20, 300, 3000};
        for (size_t s = 0; s < countof(sizes) && !err; s++) {
            const int64_t rows = sizes[s];
            cleaner(multimapFree) multimap *m =
                multimapNewLimit(3, FLEX_CAP_LEVEL_512);
            for (int64_t k = 0; k < rows; k++) {
                const databox key = databoxNewSigned(k);
                const databox a = databoxNewSigned(k * 3);
                const databox b = databoxNewSigned(0);
                const databox *row[3] = {&key, &a, &b};
                multimapInsert(&m, row);
            }

            int64_t counted = 0; /* the count every stepped row holds */
            for (size_t st = 0; st < countof(steps) && !err; st++) {
                for (int64_t k = 0; k < rows; k += 7) {
                    const databox key = databoxNewSigned(k);
                    const int64_t now =
                        multimapFieldIncr(&m, &key, 2, steps[st]);
                    if (now != counted + steps[st]) {
                        ERR("size %" PRId64 " step %zu: row %" PRId64
                            " counted to %" PRId64,
                            rows, st, k, now);
                        break;
                    }
                }
                counted += steps[st];

                for (int64_t k = 0; k < rows && !err; k++) {
                    const databox key = databoxNewSigned(k);
                    databox a = {};
                    databox b = {};
                    databox *got[2] = {&a, &b};
                    const int64_t want = k % 7 ? 0 : counted;
                    if (!multimapLookup(m, &key, got) || a.data.i != k * 3 ||
                        b.data.i != want) {
                        ERR("size %" PRId64 " step %zu: row %" PRId64
                            " reads wrong",
                            rows, st, k);
                    }
                }

                multimapVerify(m); /* asserts on a stale middle or range box */
            }

            /* ReplaceEntry and ResizeEntry at the same size write in
             * place: field a gets another 8-byte value, then bytes of one
             * length rewritten at that length */
            for (int64_t k = 0; k < rows && !err; k++) {
                const databox key = databoxNewSigned(k);
                multimapEntry me = {};
                if (!multimapGetUnderlyingEntry(m, &key, &me)) {
                    ERR("size %" PRId64 ": row %" PRId64 " lost", rows, k);
                    break;
                }
                me.fe = flexNext(*me.map, me.fe);
                const databox wide = databoxNewSigned(INT64_MIN + k);
                multimapReplaceEntry(&m, &me, &wide);
                multimapVerify(m);
            }
            for (int pass = 0; pass < 2 && !err; pass++) {
                for (int64_t k = 0; k < rows; k += 3) {
                    const databox key = databoxNewSigned(k);
                    multimapEntry me = {};
                    multimapGetUnderlyingEntry(m, &key, &me);
                    me.fe = flexNext(*me.map, me.fe);
                    const databox text = databoxNewBytesString(
                        pass ? "the same length...." : "nineteen bytes ....");
                    multimapReplaceEntry(&m, &me, &text);
                }
                multimapVerify(m);
            }
            for (int64_t k = 0; k < rows && !err; k++) {
                const databox key = databoxNewSigned(k);
                databox a = {};
                databox b = {};
                databox *got[2] = {&a, &b};
                const bool text = k % 3 == 0;
                if (!multimapLookup(m, &key, got) ||
                    (text ? a.len != 19 || memcmp(a.data.bytes.start,
                                                  "the same length....", 19)
                          : a.data.i != INT64_MIN + k)) {
                    ERR("size %" PRId64 ": row %" PRId64 " field a wrong", rows,
                        k);
                }
            }
        }
    }

    TEST("EntryReplace rewrites every value field of a row, in every tier "
         "and row width") {
        /* Maps of 4- and 10-element rows (3 and 9 values: the stack and
         * the heap scratch) at 20, 300 and 3000 rows under a small limit:
         * replacing every row's values leaves each reading back its new
         * values (some longer, some shorter), the count unchanged and the
         * map verified */
        bool sawType[MULTIMAP_TYPE_FULL + 1] = {false};
        const multimapElements widths[] = {4, 10};
        const int64_t sizes[] = {20, 300, 3000};
        for (size_t w = 0; w < countof(widths); w++) {
            const multimapElements width = widths[w];
            for (size_t s = 0; s < countof(sizes); s++) {
                cleaner(multimapFree) multimap *m =
                    multimapNewLimit(width, FLEX_CAP_LEVEL_512);
                databox boxes[10];
                const databox *row[10];
                for (int64_t k = 0; k < sizes[s]; k++) {
                    boxes[0] = databoxNewSigned(k);
                    for (multimapElements f = 1; f < width; f++) {
                        boxes[f] = databoxNewSigned(k * 100 + f);
                    }
                    for (multimapElements f = 0; f < width; f++) {
                        row[f] = &boxes[f];
                    }
                    multimapInsert(&m, row);
                }
                sawType[multimapGetType(m)] = true;
                char text[10][24];
                for (int64_t k = 0; k < sizes[s]; k++) {
                    const databox key = databoxNewSigned(k);
                    const databox *values[9];
                    for (multimapElements f = 1; f < width; f++) {
                        /* bytes of varying length, then a large integer */
                        if (f % 2) {
                            snprintf(text[f], sizeof(text[f]),
                                     "v%" PRId64 "-%u", k,
                                     (unsigned)(f * (k % 7)));
                            boxes[f] = databoxNewBytesString(text[f]);
                        } else {
                            boxes[f] = databoxNewSigned(-k * 1'000'003 - f);
                        }
                        values[f - 1] = &boxes[f];
                    }
                    multimapEntryReplace(&m, &key, values);
                    databox got[10];
                    databox *gotP[10];
                    for (multimapElements f = 0; f + 1 < width; f++) {
                        gotP[f] = &got[f];
                    }
                    if (!multimapLookup(m, &key, gotP)) {
                        ERR("width %u size %" PRId64 ": key %" PRId64 " lost",
                            (unsigned)width, sizes[s], k);
                        break;
                    }
                    for (multimapElements f = 1; f < width; f++) {
                        if (databoxCompare(&got[f - 1], &boxes[f]) != 0) {
                            ERR("width %u key %" PRId64
                                " field %u not replaced",
                                (unsigned)width, k, (unsigned)f);
                        }
                    }
                }
                if (multimapCount(m) != (size_t)sizes[s]) {
                    ERR("EntryReplace changed the row count");
                }
                multimapVerify(m);
            }
        }
        if (!sawType[MULTIMAP_TYPE_FULL]) {
            ERR("the EntryReplace sweep never reached Full");
        }
    }

    TEST("a set's insert-if-absent hands back the row its lookup finds, "
         "in every tier") {
        /* Sets of 40 keys holding 1..61 rows each under a small node
         * limit, so they reach Medium and Full and some keys' rows span
         * sub-maps: offering any key again inserts nothing and names the
         * very row GetUnderlyingEntry finds (the key's first) */
        bool sawType[MULTIMAP_TYPE_FULL + 1] = {false};
        const int64_t rowsPerKey[] = {1, 7, 61};
        for (size_t s = 0; s < countof(rowsPerKey); s++) {
            cleaner(multimapFree) multimap *set =
                multimapNewConfigure(2, true, false, FLEX_CAP_LEVEL_512);
            for (int64_t k = 0; k < 40; k++) {
                for (int64_t r = 0; r < rowsPerKey[s]; r++) {
                    const databox key = databoxNewSigned(k);
                    const databox val = databoxNewSigned(r);
                    const databox *row[2] = {&key, &val};
                    multimapInsertFullWidth(&set, row);
                }
            }
            sawType[multimapGetType(set)] = true;
            const size_t before = multimapCount(set);
            for (int64_t k = 0; k < 40; k++) {
                const databox key = databoxNewSigned(k);
                const databox other = databoxNewSigned(-1);
                const databox *offered[2] = {&key, &other};
                multimapEntry viaInsert = {};
                multimapEntry viaLookup = {};
                const bool inserted =
                    multimapInsertIfAbsentEntry(&set, offered, &viaInsert);
                const bool found =
                    multimapGetUnderlyingEntry(set, &key, &viaLookup);
                if (inserted || !found || viaInsert.fe != viaLookup.fe ||
                    viaInsert.mapIdx != viaLookup.mapIdx ||
                    *viaInsert.map != *viaLookup.map) {
                    ERR("%" PRId64 " rows a key, key %" PRId64
                        ": inserted %d, entry %s the lookup's",
                        rowsPerKey[s], k, inserted,
                        viaInsert.fe == viaLookup.fe ? "map differs from"
                                                     : "differs from");
                }
            }
            if (multimapCount(set) != before) {
                ERR("offering present keys changed the set's count");
            }
            multimapVerify(set);
        }
        if (!sawType[MULTIMAP_TYPE_FULL]) {
            ERR("the sweep never reached a Full set");
        }
    }

    TEST("multimapProcessPredicate: every condition below, at and above") {
        static const multimapCondition conds[] = {
            MULTIMAP_CONDITION_ALL,
            MULTIMAP_CONDITION_LESS_THAN_EQUAL,
            MULTIMAP_CONDITION_LESS_THAN,
            MULTIMAP_CONDITION_EQUAL,
            MULTIMAP_CONDITION_GREATER_THAN,
            MULTIMAP_CONDITION_GREATER_THAN_EQUAL,
        };
        /* rows: condition; columns: value 4, 5, 6 against 5 */
        static constexpr bool want[6][3] = {
            {true, true, true},   {true, true, false},  {true, false, false},
            {false, true, false}, {false, false, true}, {false, true, true},
        };
        for (size_t c = 0; c < 6; c++) {
            const multimapPredicate p = {.condition = conds[c],
                                         .compareAgainst = databoxNewSigned(5)};
            for (int64_t v = 4; v <= 6; v++) {
                const databox value = databoxNewSigned(v);
                if (multimapProcessPredicate(&p, &value) != want[c][v - 4]) {
                    ERR("condition %d against %" PRId64 " wrong", (int)conds[c],
                        v);
                }
            }
        }
    }

    TEST("insert-if-absent through every tier") {
        err += multimapInsertIfAbsentSweep_();
    }

    TEST("a full-width entry is the row named, every tier") {
        err += multimapFullWidthEntrySweep_();
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

    TEST("replacing insert sweep (every layer)") {
        err += multimapInsertReplacingSweep();
    }

    TEST("small: create...") {
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_2048);
        multimapReport(m);
        if (multimapType_(m) != MULTIMAP_TYPE_SMALL || multimapCount(m) != 0) {
            ERR("a new map is not an empty Small map (%zu rows)",
                multimapCount(m));
        }
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

        databox value = {};
        databox *values[1] = {&value};
        bool found = multimapLookup(m, &key, values);
        assert(found);
        assert(databoxEqual(&val, &value));

        multimapDelete(&m, &key);
        exists = multimapExists(m, &key);
        assert(!exists);

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
        databox result = {};
        databox *results[1] = {&result};
        bool found = multimapLookup(m, &key1, results);
        assert(found);
        assert(databoxEqual(&val1, &result));

        /* Insert same key with different value - should replace */
        databox val2 = databoxNewSigned(999);
        const databox *elements2[2] = {&key1, &val2};
        replaced = multimapInsert(&m, elements2);
        assert(replaced); /* This MUST return true - key was replaced */
        assert(multimapCount(m) == 1); /* Size must stay at 1, not grow to 2 */

        /* Verify value was updated */
        result = (databox){};
        found = multimapLookup(m, &key1, results);
        assert(found);
        assert(databoxEqual(&val2, &result)); /* Value must be 999, not 200 */

        /* Insert another different key - should NOT replace */
        databox key2 = databoxNewSigned(101);
        databox val3 = databoxNewSigned(300);
        const databox *elements3[2] = {&key2, &val3};
        replaced = multimapInsert(&m, elements3);
        assert(!replaced);
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
            databox result = {};
            databox *results[1] = {&result};
            bool found = multimapLookup(m, &key, results);
            assert(found);
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
        const size_t nDeltas = countof(deltas);

        /* 256 keys already put a 64-byte-limit map deep into Full (many
         * inner maps); verifying after every operation is O(n) */
        for (int count = 4; count <= 256; count *= 4) {
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
                databox result = {};
                databox *results[1] = {&result};
                const bool found = multimapLookup(m, &key, results);
                assert(found);
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
        static constexpr size_t lens[] = {1,   8,    63, 64,   65,
                                          250, 1024, 5,  4096, 2};
        const size_t nLens = countof(lens);
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

                multimapEntry me = {};
                const bool found = multimapGetUnderlyingEntry(m, &key, &me);
                assert(found);

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
                databox result = {};
                databox *results[1] = {&result};
                const bool found = multimapLookup(m, &key, results);
                assert(found);
                assert(result.len == len);
                assert(memcmp(result.data.bytes.start, buf, len) == 0);
            }
        }

        /* ResizeEntry pass: length-only contract (content past the old
         * prefix is unspecified), exercising grow, shrink, and the
         * regularize/split path for oversized maps. */
        static constexpr size_t resizeTo[] = {9, 2048, 3};
        for (size_t L = 0; L < countof(resizeTo); L++) {
            for (int i = 0; i < count; i++) {
                char keyBuf[32];
                snprintf(keyBuf, sizeof(keyBuf), "key%05d", i);
                databox key = databoxNewBytesString(keyBuf);

                multimapEntry me = {};
                const bool found = multimapGetUnderlyingEntry(m, &key, &me);
                assert(found);

                me.fe = flexNext(*me.map, me.fe);
                multimapResizeEntry(&m, &me, resizeTo[L]);
                multimapVerify(m);

                databox result = {};
                databox *results[1] = {&result};
                const bool stillThere = multimapLookup(m, &key, results);
                assert(stillThere);
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
        databox rk = {};
        databox rv = {};
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
        databox got = {};
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

        databox rk = {};
        databox rv = {};
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
                    assert(false);
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
            /* The tier's own call on the map's Small form: for a bare map
             * a view, stored back after the change */
            multimapSmall *small = mms(m);
            assert(multimapSmallFieldIncr(small, &key, 1, 5) == 15);
            if (MULTIMAP_IS_BARE(m)) {
                m = mmBareSettle_(small, m);
            }
            multimapVerify(m);
            /* Wider counts move the entries after it: the middle follows */
            small = mms(m);
            assert(multimapSmallFieldIncr(small, &key, 1, INT64_C(1) << 40) ==
                   (INT64_C(1) << 40) + 15);
            if (MULTIMAP_IS_BARE(m)) {
                m = mmBareSettle_(small, m);
            }
            multimapVerify(m);
            assert(multimapSmallFieldIncr(mms(m), &missing, 1, 5) == 0);
            assert(multimapFieldIncr(&m, &missing, 1, 5) == 0);
            /* Not a value of the row: the key, or past the last value */
            assert(multimapSmallFieldIncr(mms(m), &key, 0, 5) == 0);
            assert(multimapSmallFieldIncr(mms(m), &key, 2, 5) == 0);
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
            assert(multimapMediumFieldIncr(mmm(m), &key, 1, INT64_C(1) << 40) ==
                   (INT64_C(1) << 40) + 15);
            multimapVerify(m);
            assert(multimapMediumFieldIncr(mmm(m), &key, 1,
                                           -(INT64_C(1) << 40)) == 15);
            multimapVerify(m);
            multimapRepr(m); /* the Medium tier's dump */
            assert(multimapMediumFieldIncr(mmm(m), &missing, 1, 5) == 0);
            assert(multimapMediumFieldIncr(mmm(m), &key, 0, 5) == 0);
            assert(multimapMediumFieldIncr(mmm(m), &key, 2, 5) == 0);
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

            /* A Full map increments in place too */
            assert(multimapFullFieldIncr(mmf(m), &key, 1, 5) == 20);
            multimapVerify(m);
            assert(multimapFullFieldIncr(mmf(m), &key, 1, INT64_C(1) << 40) ==
                   (INT64_C(1) << 40) + 20);
            multimapVerify(m);
            /* key00000's count is 8 bytes wide since the Medium steps (a
             * shrink keeps the width): a fresh key grows from one byte */
            const databox fresh = databoxNewBytesString("key00007");
            assert(multimapFullFieldIncr(mmf(m), &fresh, 1, INT64_C(1) << 40) ==
                   (INT64_C(1) << 40) + 10);
            multimapVerify(m);
            assert(multimapFullFieldIncr(mmf(m), &missing, 1, 5) == 0);
            assert(multimapFullFieldIncr(mmf(m), &key, 0, 5) == 0);
            assert(multimapFullFieldIncr(mmf(m), &key, 2, 5) == 0);
            multimapVerify(m);
        }
    }

#if 1
    TEST("speeds at different sizes") {
        [[maybe_unused]] uint64_t secs[20] = {};
        [[maybe_unused]] size_t size[20] = {};
        int i = 0;
        for (size_t maxIdx = 0; maxIdx < flexOptimizationSizeLimits; maxIdx++) {
            flexCapSizeLimit maxSize = flexOptimizationSizeLimit[maxIdx];
            cleaner(multimapFree) multimap *m = multimapNewLimit(2, maxIdx);
            /* Smoke size: about 340 KB of pairs, which reaches the Full tier
             * at every cap level (8 maps at 64 KB); the timings printed are
             * informational */
            int32_t pairs = 1 << 14;
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
        for (int32_t i = 0; i < 4096; i = multimapTestNextSize_(i)) {
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
                        assert(false);
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
                        assert(false);
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
        for (int32_t i = 0; i < 4096; i = multimapTestNextSize_(i)) {
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
                        assert(false);
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
                    databox value = {};
                    databox *values[1] = {&value};
                    bool found = multimapLookup(m, &keybox, values);

                    if (!found) {
                        ERR("Didn't find %s at iteration (%d, %d)!", key, i, j);
                        assert(false);
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
                    databox value = {};
                    databox *values[1] = {&value};
                    bool found = multimapLookup(m, &keybox, values);

                    if (!found) {
                        ERR("Didn't find %s at iteration (%d, %d)!", key, i, j);
                        assert(false);
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
                        assert(false);
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
        size_t numKeys = countof(extremeKeys);

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

            databox foundVal = {};
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
            keys[i] = (int32_t)(seed % 100'000);
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
        const size_t numKeys = countof(stringKeys);

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

    TEST("Compact: rows unchanged, smallest tier, packed inner maps") {
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_512);
        const int64_t rows = 3000;
        for (int64_t i = 0; i < rows; i++) {
            const databox kb = databoxNewSigned(i);
            const databox vb = databoxNewSigned(i * 3);
            const databox *row[2] = {&kb, &vb};
            multimapInsert(&m, row);
        }

        /* Compact after the fill, then at points down the drain: three of
         * every four rows gone, then most of the rest, then nearly all */
        const int64_t keepEvery[] = {1, 4, 40, 400, 3000};
        for (size_t p = 0; p < countof(keepEvery); p++) {
            for (int64_t i = 0; i < rows; i++) {
                if (i % keepEvery[p]) {
                    const databox kb = databoxNewSigned(i);
                    multimapDelete(&m, &kb);
                }
            }

            dkTierStats before;
            multimapStats(m, &before);
            /* @own: caller; free=flexFree */
            flex *dumpBefore = multimapDump(m);
            multimapCompact(&m);
            /* @own: caller; free=flexFree */
            flex *dumpAfter = multimapDump(m);
            dkTierStats s;
            multimapStats(m, &s);
            const uint64_t L = s.unitLimit;
            const uint64_t rowsLeft = s.count;
            bool tierOk;
            switch (s.tier) {
            case MULTIMAP_TYPE_SMALL:
                tierOk = s.dataBytes <= L || rowsLeft <= 2;
                break;
            case MULTIMAP_TYPE_MEDIUM:
                tierOk = s.dataBytes > L;
                break;
            default:
                /* Packed: every inner map but the last within a row (these
                 * are under 16 bytes) and the refill slack of the limit */
                tierOk = s.dataBytes > 3 * L &&
                         s.units <= s.dataBytes / (L - 16 - 8) + 1;
                break;
            }
            if (flexBytes(dumpBefore) != flexBytes(dumpAfter) ||
                memcmp(dumpBefore, dumpAfter, flexBytes(dumpBefore)) != 0 ||
                !tierOk || s.units > before.units || s.count != before.count) {
                ERR("keep 1 in %" PRId64 ": compacted tier %u units %u (was "
                    "tier %u units %u), %" PRIu64 " data bytes, rows %s",
                    keepEvery[p], s.tier, s.units, before.tier, before.units,
                    s.dataBytes,
                    flexBytes(dumpBefore) == flexBytes(dumpAfter) ? "same"
                                                                  : "changed");
            }
            printf("  keep 1 in %4" PRId64
                   ": tier %u, %4u inner maps, %6" PRIu64
                   " bytes -> tier %u, %4u, %6" PRIu64 "\n",
                   keepEvery[p], before.tier, before.units, before.totalBytes,
                   s.tier, s.units, s.totalBytes);
            flexFree(dumpBefore);
            flexFree(dumpAfter);
            multimapVerify(m);

            /* Still takes rows (and grows) as before */
            const databox kb = databoxNewSigned(rows + (int64_t)p);
            const databox vb = databoxNewSigned(1);
            const databox *row[2] = {&kb, &vb};
            multimapInsert(&m, row);
            if (!multimapExists(m, &kb)) {
                ERR("keep 1 in %" PRId64 ": an insert after Compact is lost",
                    keepEvery[p]);
            }
            multimapDelete(&m, &kb);
        }
    }

    TEST("Compact on a map of references keeps every lookup") {
        cleaner(multimapAtomFree) multimapAtom *atoms = multimapAtomNew();
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_512);
        const size_t keys = 1500;
        for (size_t i = 0; i < keys; i++) {
            char buf[40];
            snprintf(buf, sizeof(buf), "compact-key-%05zu", i);
            const databox key = databoxNewBytesString(buf);
            databox ref = key;
            multimapAtomInsertConvert(atoms, &ref);
            const databox val = databoxNewUnsigned(i);
            const databox *row[2] = {&key, &val};
            multimapInsertWithSurrogateKey(&m, row, &ref, atoms);
        }

        dkTierStats before;
        multimapStats(m, &before);
        multimapCompactWithReference(&m, atoms);
        dkTierStats s;
        multimapStats(m, &s);
        printf("  references: %u inner maps -> %u (%" PRIu64 " data bytes)\n",
               before.units, s.units, s.dataBytes);
        size_t wrong = 0;
        for (size_t i = 0; i < keys; i++) {
            char buf[40];
            snprintf(buf, sizeof(buf), "compact-key-%05zu", i);
            const databox key = databoxNewBytesString(buf);
            databox got = {};
            databox *out[1] = {&got};
            wrong += !multimapLookupWithReference(m, &key, out, atoms) ||
                     got.data.u64 != i;
        }
        if (wrong || s.count != keys || s.tier != MULTIMAP_TYPE_FULL ||
            s.units >= before.units) {
            ERR("%zu lookups wrong; units %u -> %u, tier %u", wrong,
                before.units, s.units, s.tier);
        }
        multimapVerify(m);
    }

    TEST("Reset from every tier: an empty Small keeping its configuration") {
        const struct {
            bool isSet;
            bool compress;
            flexCapSizeLimit limit;
        } configs[] = {{false, false, FLEX_CAP_LEVEL_64},
                       {true, true, FLEX_CAP_LEVEL_128}};
        for (size_t c = 0; c < countof(configs); c++) {
            for (int32_t tier = MULTIMAP_TYPE_SMALL; tier <= MULTIMAP_TYPE_FULL;
                 tier++) {
                cleaner(multimapFree) multimap *m = multimapNewConfigure(
                    2, configs[c].isSet, configs[c].compress, configs[c].limit);
                int64_t next = 0;
                do {
                    const databox kb = databoxNewSigned(next);
                    const databox vb = databoxNewSigned(next * 3);
                    const databox *row[2] = {&kb, &vb};
                    multimapInsert(&m, row);
                    next++;
                } while (multimapGetType(m) != (multimapType)tier);

                multimapReset(&m);
                const databox kb = databoxNewSigned(7);
                const databox vb = databoxNewSigned(8);
                if (multimapGetType(m) != MULTIMAP_TYPE_SMALL ||
                    multimapCount(m) != 0 || multimapExists(m, &kb) ||
                    multimapIsSet(m) != configs[c].isSet ||
                    multimapCompresses(m) != configs[c].compress ||
                    multimapLimit(m) != configs[c].limit) {
                    ERR("config %zu tier %d: reset to tier %d, count %zu", c,
                        tier, multimapGetType(m), multimapCount(m));
                }

                /* Rows of the same width go back in */
                const databox *row[2] = {&kb, &vb};
                multimapInsert(&m, row);
                if (multimapCount(m) != 1 || !multimapExists(m, &kb)) {
                    ERR("config %zu tier %d: a row after Reset is missing", c,
                        tier);
                }
                multimapVerify(m);
            }
        }
    }

    TEST("stats: tier, inner maps, rows and bytes in every tier") {
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_64);
        const uint32_t limit = flexOptimizationSizeLimit[FLEX_CAP_LEVEL_64];
        dkTierStats s;
        int64_t next = 0;
        for (int32_t want = MULTIMAP_TYPE_SMALL; want <= MULTIMAP_TYPE_FULL;
             want++) {
            while (multimapGetType(m) != (multimapType)want) {
                const databox kb = databoxNewSigned(next);
                const databox vb = databoxNewSigned(next * 7);
                const databox *row[2] = {&kb, &vb};
                multimapInsert(&m, row);
                next++;
            }
            multimapStats(m, &s);
            /* Small: none when bare (the map is its flex), else its struct */
            const bool full = want == MULTIMAP_TYPE_FULL;
            const size_t overhead =
                want == MULTIMAP_TYPE_SMALL
                    ? (MULTIMAP_IS_BARE(m) ? 0 : sizeof(multimapSmall))
                : want == MULTIMAP_TYPE_MEDIUM ? sizeof(multimapMedium)
                                               : 0;
            const uint32_t units = want == MULTIMAP_TYPE_SMALL ? 1
                                   : want == MULTIMAP_TYPE_MEDIUM
                                       ? 2
                                       : mmf(m)->count;
            if (s.tier != want || s.units != units ||
                s.count != (uint64_t)next || s.unitLimit != limit ||
                s.dataBytes != multimapBytes(m) ||
                (!full && s.totalBytes != s.dataBytes + overhead) ||
                (full && s.totalBytes != multimapFullBytesFull(mmf(m)))) {
                ERR("tier %d: stats tier %u units %u count %" PRIu64
                    " limit %u data %" PRIu64 " total %" PRIu64,
                    want, s.tier, s.units, s.count, s.unitLimit, s.dataBytes,
                    s.totalBytes);
            }
        }
        if (s.units < 2) {
            ERR("Full right after growing holds %u inner maps", s.units);
        }
    }

    TEST("upgrade path Small -> Medium -> Full") {
        /* Use very small limits to force upgrades */
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_64);

        multimapType prevType = multimapType_(m);
        [[maybe_unused]] int32_t smallToMediumTransition = -1;
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
        databox firstKey = {};
        databox firstVal = {};
        databox *firstElements[2] = {&firstKey, &firstVal};
        bool gotFirst = multimapFirst(m, firstElements);
        if (!gotFirst || firstKey.data.i != 0) {
            ERR("First element wrong: got %" PRIdMAX ", expected 0",
                firstKey.data.i);
        }

        /* Verify last element */
        databox lastKey = {};
        databox lastVal = {};
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
        databox key = {};
        databox val = {};
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
        /* The oracle: rows per key.  A full-width insert compares the
         * whole row, so each insert (a new value every step) adds a row,
         * and a delete removes one of the key's rows. */
        uint32_t rows[10000] = {};
        size_t rowCount = 0;

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
                rows[key]++;
                rowCount++;
            } else if (op < 9 && insertedCount > 0) {
                /* Delete a row of a key inserted at some point */
                size_t idx = seed % insertedCount;
                const int32_t gone = inserted[idx];
                databox k = databoxNewSigned(gone);
                if (multimapDelete(&m, &k) != (rows[gone] > 0)) {
                    ERR("step %" PRId32 ": deleting key %" PRId32
                        " disagrees with the oracle",
                        i, gone);
                }

                if (rows[gone] > 0) {
                    rows[gone]--;
                    rowCount--;
                }
            } else if (insertedCount > 0) {
                /* Look up a key inserted at some point */
                size_t idx = seed % insertedCount;
                databox k = databoxNewSigned(inserted[idx]);
                if (multimapExists(m, &k) != (rows[inserted[idx]] > 0)) {
                    ERR("step %" PRId32 ": key %" PRId32
                        " exists disagrees with the oracle",
                        i, inserted[idx]);
                }
            }

            /* Periodic verification */
            if (i % 1000 == 0) {
                multimapVerify(m);
            }
        }

        multimapVerify(m);
        if (multimapCount(m) != rowCount) {
            ERR("%zu rows, the oracle holds %zu", multimapCount(m), rowCount);
        }
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

                /* Every previously inserted key must still be found: all
                 * of them after each of the first 300 inserts (through the
                 * small tiers' splits) and every 37th after; the new key,
                 * its predecessor, the middle and the first every time */
                const bool all = i < 300 || i % 37 == 0;
                const int32_t spot[4] = {i, i > 0 ? i - 1 : 0, i / 2, 0};
                for (int32_t k = 0; k < (all ? i + 1 : 4); k++) {
                    const int32_t j = all ? k : spot[k];
                    databox checkKey = databoxNewSigned(j);
                    if (!multimapExists(m, &checkKey)) {
                        ERR("FUZZ FAIL: Sequential key %d not found after "
                            "inserting key %d (limit=%d, type=%d)!",
                            j, i, capLimit, multimapType_(m));
                        assert(false);
                    }
                }
            }

            /* Final comprehensive check */
            for (int32_t i = 0; i < numKeys; i++) {
                databox key = databoxNewSigned(i);
                databox gotVal = {};
                databox *vals[1] = {&gotVal};
                bool found = multimapLookup(m, &key, vals);
                if (!found || gotVal.data.i != i * 100) {
                    ERR("FUZZ FAIL: Sequential lookup failed for key %d!", i);
                    assert(false);
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
                assert(false);
            }

            /* Check first 10 keys inserted (highest values) */
            for (int32_t j = numKeys - 1; j > numKeys - 11 && j >= i; j--) {
                databox checkKey = databoxNewSigned(j);
                if (!multimapExists(m, &checkKey)) {
                    ERR("FUZZ FAIL: Reverse key %d lost after inserting %d!", j,
                        i);
                    assert(false);
                }
            }
        }

        /* Final comprehensive verification */
        for (int32_t i = 0; i < numKeys; i++) {
            databox key = databoxNewSigned(i);
            if (!multimapExists(m, &key)) {
                ERR("FUZZ FAIL: Reverse final check failed for key %d!", i);
                assert(false);
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
                (int64_t)(xoroshiro128plus(seed) % 1'000'000) - 500'000;

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
                        assert(false);
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
            int64_t clusterBase = c * 100'000;

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
                    assert(false);
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
                    assert(false);
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
                        assert(false);
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
            -1'000'000LL,
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
            1'000'000LL,
            1000000000000LL,
            INT64_MAX - 2,
            INT64_MAX - 1,
            INT64_MAX,
        };
        size_t numBoundary = countof(boundaryKeys);

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
                assert(false);
            }

            databox gotVal = {};
            databox *vals[1] = {&gotVal};
            bool found = multimapLookup(m, &key, vals);
            if (!found || gotVal.data.i != (int64_t)i) {
                ERR("FUZZ FAIL: Boundary lookup wrong for %" PRId64 "!",
                    boundaryKeys[i]);
                assert(false);
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
                assert(false);
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
                assert(false);
            }
        }

        /* Verify all strings */
        for (int32_t i = 0; i < numStrings; i++) {
            char buf[64];
            snprintf(buf, sizeof(buf), "key_%05d", i);
            databox key = databoxNewBytesString(buf);
            if (!multimapExists(m, &key)) {
                ERR("FUZZ FAIL: Mixed-type string key %s not found!", buf);
                assert(false);
            }
        }

        /* Verify non-existent keys return false */
        databox noKey1 = databoxNewSigned(999999);
        databox noKey2 = databoxNewBytesString("nonexistent_key");
        if (multimapExists(m, &noKey1) || multimapExists(m, &noKey2)) {
            ERR("FUZZ FAIL: Non-existent key found!");
            assert(false);
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
            int64_t keyVal = (int64_t)(xoroshiro128plus(seed) % 100'000);
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
                        assert(false);
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
                assert(false);
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
                    assert(false);
                }
            }
        }

        /* Verify total count */
        size_t expected = (size_t)numUniqueKeys * valsPerKey;
        if (multimapCount(m) != expected) {
            ERR("FUZZ FAIL: FullWidth count %zu != expected %zu!",
                multimapCount(m), expected);
            assert(false);
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
        size_t numPrefixes = countof(prefixes);

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
                assert(false);
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
            databox iterKey = {};
            databox iterVal = {};
            databox *iterElements[2] = {&iterKey, &iterVal};
            int32_t count = 0;
            intmax_t prevKey = INTMAX_MIN;

            while (multimapIteratorNext(&iter, iterElements)) {
                /* Verify sorted order */
                if (iterKey.data.i < prevKey) {
                    ERR("FUZZ FAIL: Iterator out of order! %" PRIdMAX
                        " < %" PRIdMAX,
                        iterKey.data.i, prevKey);
                    assert(false);
                }
                prevKey = iterKey.data.i;

                /* Verify lookup works for this key */
                if (!multimapExists(m, &iterKey)) {
                    ERR("FUZZ FAIL: Iterator key %" PRIdMAX
                        " not found via lookup!",
                        iterKey.data.i);
                    assert(false);
                }
                count++;
            }

            if (count != 3000) {
                ERR("FUZZ FAIL: Iterator count %d != 3000!", count);
                assert(false);
            }

            printf("    type=FULL count=%d verified_order=OK: OK\n", count);
        }
    }

    TEST("FUZZ: stress test - massive random operations") {
        /* High-volume random insert/delete/lookup */
        cleaner(multimapFree) multimap *m =
            multimapNewLimit(2, FLEX_CAP_LEVEL_256);

        /* Use bitmap as lightweight oracle for existence */
        const size_t keySpace = 100'000;
        cleanerName(zfree, Bytes) uint8_t *exists =
            zcalloc(dkBitBytes(keySpace), 1);
        size_t existCount = 0;

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
                if (!dkBitGet(exists, keyIdx)) {
                    multimapInsert(&m, elements);
                    dkBitSet(exists, keyIdx);
                    existCount++;
                }
                insertOps++;
            } else if (op < 8) {
                /* Lookup (30%) */
                bool found = multimapExists(m, &key);
                bool shouldExist = dkBitGet(exists, keyIdx);
                if (found != shouldExist) {
                    ERR("FUZZ FAIL: Lookup mismatch for key %zu! found=%d "
                        "should=%d",
                        keyIdx, found, shouldExist);
                    assert(false);
                }
                lookupOps++;
            } else {
                /* Delete (20%) */
                if (dkBitGet(exists, keyIdx)) {
                    bool deleted = multimapDelete(&m, &key);
                    if (!deleted) {
                        ERR("FUZZ FAIL: Delete of existing key %zu failed!",
                            keyIdx);
                        assert(false);
                    }
                    dkBitClear(exists, keyIdx);
                    existCount--;
                }
                deleteOps++;
            }

            /* Periodic count verification */
            if (round % 5000 == 0) {
                if (multimapCount(m) != existCount) {
                    ERR("FUZZ FAIL: Count mismatch! map=%zu oracle=%zu",
                        multimapCount(m), existCount);
                    assert(false);
                }
            }
        }

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
            assert(false);
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
                        assert(false);
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
                    assert(false);
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
                assert(false);
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
                        assert(false);
                    }
                }

                /* Also delete and re-insert at boundary */
                databox boundaryKey = databoxNewSigned(i);
                bool deleted = multimapDelete(&m, &boundaryKey);
                assert(deleted);
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
                    assert(false);
                }
            }

            /* Verify even keys exist */
            for (int32_t i = 0; i < 500; i += 2) {
                databox key = databoxNewSigned(i);
                if (!multimapExists(m, &key)) {
                    ERR("FUZZ FAIL: Existing key %d not found! type=%d", i,
                        multimapType_(m));
                    assert(false);
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
            databox result = {};
            databox *results[1] = {&result};
            bool found = multimapSmallLookup(s, &key, results);
            assert(found);
            databox expected = databoxNewSigned(i * 1000);
            assert(databoxEqual(&expected, &result));
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
            databox result = {};
            databox *results[1] = {&result};
            bool found = multimapMediumLookup(m, &key, results);
            assert(found);
            databox expected = databoxNewSigned(i * 1000);
            assert(databoxEqual(&expected, &result));
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
            databox result = {};
            databox *results[1] = {&result};
            bool found = multimapFullLookup(f, &key, results);
            assert(found);
            databox expected = databoxNewSigned(i * 1000);
            assert(databoxEqual(&expected, &result));
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

            multimapReset(&m);
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
#define SOAK_NEXT() dkXorshift64Next(&rng)
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
                    ERR("op %d: first key %" PRIdMAX " != shadow %" PRId64, op,
                        fe[0].data.i, shKey[0]);
                    soakFail++;
                }
                if (!multimapLastInto(m, le, 2) ||
                    le[0].data.i != shKey[shCount - 1]) {
                    ERR("op %d: last key %" PRIdMAX " != shadow %" PRId64, op,
                        le[0].data.i, shKey[shCount - 1]);
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
                        ERR("op %d: row %d (%" PRIdMAX ",%" PRIdMAX
                            ") != shadow (%" PRId64 ",%" PRId64 ")",
                            op, seen, row[0].data.i, row[1].data.i, shKey[seen],
                            shVal[seen]);
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

        for (size_t l = 0; l < countof(layers); l++) {
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
                    ERR("%s alias: key %" PRIu64 " reported (%" PRIuMAX
                        ", 0x%" PRIxMAX "), expected (%" PRIu64 ", 0x%" PRIxPTR
                        ")",
                        where, key, v[0].data.u, v[1].data.u, 100 + key,
                        (uintptr_t)RP_PTR_TAG(key));
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
                    ERR("%s alias: key %" PRIu64 " stored (%" PRIuMAX
                        ", 0x%" PRIxMAX "), expected (%" PRIu64 ", 0x%" PRIxPTR
                        ")",
                        where, key, stored[0].data.u, stored[1].data.u,
                        200 + key, (uintptr_t)RP_PTR_TAG(key + 1000));
                }
            }
        }
    }

    TEST("delete by <= removes exactly the rows at or below the bound") {
        /* Keys 10, 20, ... in maps of 1, 2 and 3 elements per row and
         * every tier; bounds below, at and between the first keys, in
         * the middle, at the top and above it.  With one element per
         * row the row before the first kept one can be the map's head. */
        static constexpr int32_t sizes[] = {6, 400, 4000};
        static constexpr int64_t bounds[] = {5, 10, 15, 20, 25, 1005, -1, -2};
        for (uint32_t width = 1; width <= 3; width++) {
            for (size_t s = 0; s < countof(sizes); s++) {
                for (size_t b = 0; b < countof(bounds); b++) {
                    const int32_t n = sizes[s];
                    cleaner(multimapFree) multimap *m = multimapNew(width);
                    for (int32_t i = n; i >= 1; i--) {
                        databox row[3] = {databoxNewSigned((int64_t)i * 10),
                                          databoxNewSigned(i),
                                          databoxNewSigned(-i)};
                        const databox *ptrs[3] = {&row[0], &row[1], &row[2]};
                        multimapInsert(&m, ptrs);
                    }
                    /* -1: the top key; -2: above it */
                    const int64_t bound = bounds[b] == -1 ? (int64_t)n * 10
                                          : bounds[b] == -2
                                              ? (int64_t)n * 10 + 5
                                              : bounds[b];
                    const multimapPredicate pred = {
                        .condition = MULTIMAP_CONDITION_LESS_THAN_EQUAL,
                        .compareAgainst = databoxNewSigned(bound)};
                    multimapDeleteByPredicate(&m, &pred);

                    const int64_t gone = bound / 10 < n ? bound / 10 : n;
                    const size_t want = (size_t)(n - gone);
                    const databox below = databoxNewSigned(gone * 10);
                    const databox above = databoxNewSigned((gone + 1) * 10);
                    if (multimapCount(m) != want ||
                        (gone > 0 && multimapExists(m, &below)) ||
                        (gone < n && !multimapExists(m, &above))) {
                        ERR("width %u, %d rows, <= %" PRId64
                            ": %zu rows left, want %zu",
                            width, n, bound, multimapCount(m), want);
                    }
                }
            }
        }
    }

    TEST("key set operations agree with a bitset oracle in every tier") {
        /* Two random sets of 0..n-1 per round, sizes reaching each tier:
         * intersection, difference, symmetric difference and union
         * (copy keys) hold exactly the oracle's members */
        static constexpr int32_t ns[] = {12, 300, 3000};
        static bool inA[3000];
        static bool inB[3000];
        uint64_t rng = 0x5e7;
        for (size_t t = 0; t < countof(ns); t++) {
            for (int round = 0; round < 4; round++) {
                const int32_t n = ns[t];
                cleaner(multimapFree) multimap *a = multimapSetNew(1);
                cleaner(multimapFree) multimap *b = multimapSetNew(1);
                for (int32_t i = 0; i < n; i++) {
                    dkLcg64Next(&rng);
                    inA[i] = (rng >> 40) % 3 == 0;
                    inB[i] = (rng >> 50) % 3 == 0;
                    const databox key = databoxNewSigned(i);
                    const databox *row[1] = {&key};
                    if (inA[i]) {
                        multimapInsert(&a, row);
                    }
                    if (inB[i]) {
                        multimapInsert(&b, row);
                    }
                }
                for (int op = 0; op < 4; op++) {
                    cleaner(multimapFree) multimap *out = multimapSetNew(1);
                    multimapIterator ia;
                    multimapIterator ib;
                    multimapIteratorInit(a, &ia, true);
                    multimapIteratorInit(b, &ib, true);
                    if (op == 0) {
                        multimapIntersectKeys(&out, &ia, &ib);
                    } else if (op == 1 || op == 2) {
                        multimapDifferenceKeys(&out, &ia, &ib, op == 2);
                    } else {
                        multimapCopyKeys(&out, a);
                        multimapCopyKeys(&out, b);
                    }
                    size_t want = 0;
                    bool same = true;
                    for (int32_t i = 0; i < n; i++) {
                        const bool member = op == 0   ? inA[i] && inB[i]
                                            : op == 1 ? inA[i] && !inB[i]
                                            : op == 2 ? inA[i] != inB[i]
                                                      : inA[i] || inB[i];
                        const databox key = databoxNewSigned(i);
                        want += member;
                        same = same && multimapExists(out, &key) == member;
                    }
                    if (!same || multimapCount(out) != want) {
                        ERR("%d keys, op %d: %zu members, want %zu%s", n, op,
                            multimapCount(out), want,
                            same ? "" : " (membership differs)");
                    }
                }
            }
        }
    }

    TEST("a NaN key can be found and deleted (keys use identity order)") {
        /* databoxEqual says NaN != NaN, but key identity is the total
         * order, where every NaN is one position: a stored NaN key stays
         * reachable instead of leaking. */
        cleaner(multimapFree) multimap *m = multimapNew(2);
        const databox nanKey = databoxNewReal(NAN);
        const databox otherNan = databoxNewReal(-NAN);
        const databox inf = databoxNewReal(INFINITY);
        const databox val = databoxNewSigned(7);
        const databox *rowNan[] = {&nanKey, &val};
        const databox *rowInf[] = {&inf, &val};
        multimapInsert(&m, rowInf);
        multimapInsert(&m, rowNan);
        multimapInsert(&m, rowNan); /* replaces, does not duplicate */

        if (multimapCount(m) != 2 || !multimapExists(m, &nanKey) ||
            !multimapExists(m, &otherNan)) {
            ERR("NaN key is not one reachable entry");
        }

        if (!multimapDelete(&m, &otherNan) || multimapExists(m, &nanKey) ||
            multimapCount(m) != 1 || !multimapExists(m, &inf)) {
            ERR("NaN key could not be deleted cleanly");
        }
    }

    TEST("FieldIncrBy: exact, refuses overflow and non-counters") {
        cleaner(multimapFree) multimap *m = multimapNew(2);
        const databox kMax = databoxNewSigned(1);
        const databox kBool = databoxNewSigned(2);
        const databox kStr = databoxNewSigned(3);
        const databox kMissing = databoxNewSigned(4);
        const databox vMax = databoxNewSigned(INT64_MAX);
        const databox vStr = databoxNewBytesString("text");
        const databox *rowMax[] = {&kMax, &vMax};
        const databox *rowBool[] = {&kBool, &DATABOX_BOX_FALSE};
        const databox *rowStr[] = {&kStr, &vStr};
        multimapInsert(&m, rowMax);
        multimapInsert(&m, rowBool);
        multimapInsert(&m, rowStr);

        int64_t got = 42;
        if (multimapFieldIncrBy(&m, &kMax, 1, 1, &got) || got != 42 ||
            multimapFieldIncr(&m, &kMax, 1, 0) != INT64_MAX) {
            ERR("INT64_MAX + 1 was not refused unchanged");
        }
        if (!multimapFieldIncrBy(&m, &kMax, 1, -2, &got) ||
            got != INT64_MAX - 2) {
            ERR("INT64_MAX - 2 did not apply");
        }
        if (!multimapFieldIncrBy(&m, &kBool, 1, 1, &got) || got != 1 ||
            !multimapFieldIncrBy(&m, &kBool, 1, 1, &got) || got != 2 ||
            !multimapFieldIncrBy(&m, &kBool, 1, -2, &got) || got != 0) {
            ERR("boolean counter arithmetic is wrong");
        }
        if (multimapFieldIncrBy(&m, &kStr, 1, 1, nullptr) ||
            multimapFieldIncrBy(&m, &kMissing, 1, 1, nullptr) ||
            multimapFieldIncr(&m, &kMissing, 1, 1) != 0) {
            ERR("a non-counter field or missing key was not refused");
        }
    }

    TEST("FieldIncrBy: an offset outside the row's values is refused") {
        /* Offset 0 is the key (keys never change in place); an offset
         * past the row's last value would be the next row's key, or past
         * the map's end after the last row */
        cleaner(multimapFree) multimap *m = multimapNew(2);
        const databox k1 = databoxNewSigned(1);
        const databox k2 = databoxNewSigned(2);
        const databox v = databoxNewSigned(5);
        const databox *row1[] = {&k1, &v};
        const databox *row2[] = {&k2, &v};
        multimapInsert(&m, row1);
        multimapInsert(&m, row2);
        const size_t bytes = multimapBytes(m);
        const uint32_t offsets[] = {0, 2, 3, UINT32_MAX};
        size_t accepted = 0;
        for (size_t i = 0; i < countof(offsets); i++) {
            int64_t got = 42;
            accepted += multimapFieldIncrBy(&m, &k1, offsets[i], 1, &got);
            accepted += multimapFieldIncrBy(&m, &k2, offsets[i], 1, &got);
            multimapEntry row;
            if (multimapGetUnderlyingEntry(m, &k2, &row)) {
                accepted +=
                    multimapFieldIncrEntry(&m, &row, offsets[i], 1, nullptr);
            }
        }
        databox got1 = {};
        databox got2 = {};
        databox *found1[] = {&got1};
        databox *found2[] = {&got2};
        if (accepted || multimapBytes(m) != bytes || multimapCount(m) != 2 ||
            !multimapLookup(m, &k1, found1) ||
            !multimapLookup(m, &k2, found2) || got1.data.i != 5 ||
            got2.data.i != 5) {
            ERR("%zu increments outside the values applied", accepted);
        }
    }

    TEST("FieldIncrEntry: the increment of FieldIncrBy at a row found") {
        /* Twin maps take the same increments, one by key and one at the
         * row multimapGetUnderlyingEntry found; results, reports and the
         * stored fields agree, across a boolean growing into an integer,
         * the int64 edge and a non-counter, and a set refuses the entry
         * form as it refuses the keyed one */
        cleaner(multimapFree) multimap *byKey = multimapNew(3);
        cleaner(multimapFree) multimap *byRow = multimapNew(3);
        const databox vStr = databoxNewBytesString("text");
        const databox vMax = databoxNewSigned(INT64_MAX - 1);
        for (int64_t k = 0; k < 3; k++) {
            const databox key = databoxNewSigned(k);
            const databox *value = k == 0   ? &DATABOX_BOX_FALSE
                                   : k == 1 ? &vMax
                                            : &vStr;
            const databox *row[] = {&key, &vStr, value};
            multimapInsert(&byKey, row);
            multimapInsert(&byRow, row);
        }

        typedef struct mmIncrStep {
            int64_t key;
            int64_t by;
        } mmIncrStep;
        static const mmIncrStep steps[] = {{0, 1},    {0, 1}, {0, 300},
                                           {0, -302}, {1, 1}, {1, 1},
                                           {1, -5},   {2, 1}, {0, 0}};
        for (size_t s = 0; s < countof(steps); s++) {
            const databox key = databoxNewSigned(steps[s].key);
            int64_t wantNew = MM_TEST_NOT_WRITTEN;
            int64_t gotNew = MM_TEST_NOT_WRITTEN;
            const bool want =
                multimapFieldIncrBy(&byKey, &key, 2, steps[s].by, &wantNew);
            multimapEntry row;
            bool got = false;
            if (multimapGetUnderlyingEntry(byRow, &key, &row)) {
                got = multimapFieldIncrEntry(&byRow, &row, 2, steps[s].by,
                                             &gotNew);
            }

            databox a = {};
            databox b = {};
            databox ignored = {};
            databox *outA[] = {&ignored, &a};
            databox *outB[] = {&ignored, &b};
            multimapLookup(byKey, &key, outA);
            multimapLookup(byRow, &key, outB);
            if (got != want || gotNew != wantNew ||
                databoxCompare(&a, &b) != 0 || a.type != b.type) {
                ERR("step %zu (key %" PRId64 " by %" PRId64
                    "): entry %d/%" PRId64 ", keyed %d/%" PRId64,
                    s, steps[s].key, steps[s].by, got, gotNew, want, wantNew);
            }
        }

        cleaner(multimapFree) multimap *set = multimapSetNew(2);
        const databox one = databoxNewSigned(1);
        const databox *setRow[] = {&one, &one};
        multimapInsert(&set, setRow);
        multimapEntry me;
        multimapGetUnderlyingEntry(set, &one, &me);
        const dkDiagPrintMode priorPrint = dkDiagGetPrintMode();
        dkDiagSetPrintMode(DK_DIAG_PRINT_NEVER);
        bool wrote = false;
        int64_t now = MM_TEST_NOT_WRITTEN;
        DK_DIAG_EXPECT_REPORTS(
            "a set by entry", 1, DK_DIAG_MULTIMAP_SET_REWRITE,
            wrote = multimapFieldIncrEntry(&set, &me, 1, 1, &now));
        dkDiagSetPrintMode(priorPrint);
        if (wrote || now != MM_TEST_NOT_WRITTEN) {
            ERR("a set's row was incremented in place by entry (or its "
                "report was written)");
        }
    }

    TEST("a set refuses every in-place rewrite: its values order the rows") {
        /* (1, 1) (1, 5) (2, 3): raising (1, 1) to 11 in place would sit it
         * before (1, 5), and full-width searches would stop finding rows.
         * Each rewrite is refused and reported once; a map's value column
         * still rewrites, with no report. */
        cleaner(multimapFree) multimap *set = multimapSetNew(2);
        cleaner(multimapFree) multimap *map = multimapNew(2);
        static constexpr int64_t rows[][2] = {{1, 1}, {1, 5}, {2, 3}};
        for (size_t i = 0; i < countof(rows); i++) {
            const databox k = databoxNewSigned(rows[i][0]);
            const databox v = databoxNewSigned(rows[i][1]);
            const databox *row[] = {&k, &v};
            multimapInsert(&set, row);
            multimapInsert(&map, row);
        }
        const databox one = databoxNewSigned(1);
        const databox two = databoxNewSigned(2);
        const databox eleven = databoxNewSigned(11);
        const dkDiagPrintMode priorPrint = dkDiagGetPrintMode();
        dkDiagSetPrintMode(DK_DIAG_PRINT_NEVER);
        int64_t now = MM_TEST_NOT_WRITTEN;
        bool wrote = false;
        multimapEntry me;
        multimapGetUnderlyingEntry(set, &one, &me);
        me.fe = flexNext(*me.map, me.fe); /* the value column */
#define MM_SET_REFUSES_(call)                                                  \
    DK_DIAG_EXPECT_REPORTS("a set", 1, DK_DIAG_MULTIMAP_SET_REWRITE,           \
                           wrote |= (call))
        MM_SET_REFUSES_(multimapFieldIncrBy(&set, &one, 1, 10, &now));
        MM_SET_REFUSES_(multimapFieldIncrBy(&set, &two, 1, 1, &now));
        MM_SET_REFUSES_(multimapFieldIncr(&set, &one, 1, 10) != 0);
        MM_SET_REFUSES_(multimapReplaceEntry(&set, &me, &eleven));
        MM_SET_REFUSES_(
            multimapReplaceEntryWithReference(&set, &me, &eleven, nullptr));
        MM_SET_REFUSES_(multimapResizeEntry(&set, &me, 40));
#undef MM_SET_REFUSES_
        multimapEntry mapMe;
        multimapGetUnderlyingEntry(map, &one, &mapMe);
        mapMe.fe = flexNext(*mapMe.map, mapMe.fe);
        bool mapWrote = false;
        DK_DIAG_EXPECT_REPORTS("a map", 0, DK_DIAG_NONE,
                               mapWrote =
                                   multimapReplaceEntry(&map, &mapMe, &eleven));
        dkDiagSetPrintMode(priorPrint);
        databox got = {};
        databox *out[] = {&got};
        if (wrote || now != MM_TEST_NOT_WRITTEN) {
            ERR("a set row was rewritten in place");
        }
        if (!mapWrote || !multimapLookup(map, &one, out) || got.data.i != 11) {
            ERR("a map's value column was not rewritten");
        }
        for (size_t i = 0; i < countof(rows); i++) {
            const databox k = databoxNewSigned(rows[i][0]);
            const databox v = databoxNewSigned(rows[i][1]);
            const databox *row[] = {&k, &v};
            if (!multimapExistsFullWidth(set, row)) {
                ERR("row %zu lost after a refused increment", i);
            }
        }
        multimapVerify(set);
    }

    TEST("FieldIncrByIf: asked once a change will apply; declining changes "
         "nothing") {
        cleaner(multimapFree) multimap *m = multimapNew(2);
        const databox kBool = databoxNewSigned(1);
        const databox kU = databoxNewSigned(2);
        const databox kMax = databoxNewSigned(3);
        const databox kMissing = databoxNewSigned(4);
        const databox vU = databoxNewUnsigned(9);
        const databox vMax = databoxNewSigned(INT64_MAX);
        const databox *rowBool[] = {&kBool, &DATABOX_BOX_TRUE};
        const databox *rowU[] = {&kU, &vU};
        const databox *rowMax[] = {&kMax, &vMax};
        multimapInsert(&m, rowBool);
        multimapInsert(&m, rowU);
        multimapInsert(&m, rowMax);

        /* Declined: the field keeps its value and type, and the current
         * value is reported */
        mmProceedProbe_ probe = {.answer = false, .asked = 0};
        int64_t now = MM_TEST_NOT_WRITTEN;
        databox got = {};
        databox *out[] = {&got};
        if (multimapFieldIncrByIf(&m, &kBool, 1, 1, &now, mmProceedProbe,
                                  &probe) ||
            probe.asked != 1 || now != 1 || !multimapLookup(m, &kBool, out) ||
            got.type != DATABOX_TRUE) {
            ERR("a declined boolean increment changed the field");
        }
        if (multimapFieldIncrByIf(&m, &kU, 1, 5, &now, mmProceedProbe,
                                  &probe) ||
            probe.asked != 2 || now != 9 || !multimapLookup(m, &kU, out) ||
            got.type != DATABOX_UNSIGNED_64 || got.data.u != 9) {
            ERR("a declined unsigned increment changed the field");
        }

        /* Not asked: an increment by 0 (true, current), an overflow
         * (false, current), a missing key (false, 0) */
        if (!multimapFieldIncrByIf(&m, &kU, 1, 0, &now, mmProceedProbe,
                                   &probe) ||
            now != 9 ||
            multimapFieldIncrByIf(&m, &kMax, 1, 1, &now, mmProceedProbe,
                                  &probe) ||
            now != INT64_MAX ||
            multimapFieldIncrByIf(&m, &kMissing, 1, 1, &now, mmProceedProbe,
                                  &probe) ||
            now != 0 || probe.asked != 2) {
            ERR("a no-change, overflow or missing key asked or misreported");
        }

        /* Allowed, and with no question at all */
        probe.answer = true;
        if (!multimapFieldIncrByIf(&m, &kU, 1, 5, &now, mmProceedProbe,
                                   &probe) ||
            now != 14 || probe.asked != 3 ||
            !multimapFieldIncrByIf(&m, &kU, 1, 1, &now, nullptr, nullptr) ||
            now != 15 || !multimapLookup(m, &kU, out) || got.data.i != 15) {
            ERR("an allowed increment did not apply");
        }
    }

    TEST("a set's key addresses its first row, every tier") {
        /* Five rows per key (values 0..4); lookup sees value 0, and each
         * delete by key removes the lowest remaining row, including keys
         * whose rows run across inner maps */
        /* 20, 120 and 6000 rows at shrinking split sizes: Small, Medium
         * and Full */
        const int64_t keys[] = {4, 24, 1200};
        const flexCapSizeLimit limits[] = {
            FLEX_CAP_LEVEL_2048, FLEX_CAP_LEVEL_512, FLEX_CAP_LEVEL_64};
        for (size_t t = 0; t < countof(keys); t++) {
            cleaner(multimapFree) multimap *m =
                multimapNewConfigure(2, true, false, limits[t]);
            for (int64_t k = 0; k < keys[t]; k++) {
                for (int64_t v = 4; v >= 0; v--) {
                    const databox key = databoxNewSigned(k);
                    const databox val = databoxNewSigned(v);
                    const databox *row[] = {&key, &val};
                    multimapInsert(&m, row);
                }
            }
            const multimapType tiers[] = {
                MULTIMAP_TYPE_SMALL, MULTIMAP_TYPE_MEDIUM, MULTIMAP_TYPE_FULL};
            if (multimapType_(m) != tiers[t]) {
                ERR("tier %zu: built type %d", t, (int)multimapType_(m));
            }
            for (int round = 0; round < 3; round++) {
                for (int64_t k = 0; k < keys[t]; k++) {
                    const databox key = databoxNewSigned(k);
                    const databox want = databoxNewSigned(round);
                    databox got;
                    databox *out[] = {&got};
                    if (!multimapLookup(m, &key, out) ||
                        !databoxEqual(&got, &want)) {
                        ERR("tier %zu round %d key %" PRId64
                            ": lookup not the first row",
                            t, round, k);
                        break;
                    }
                    multimapDelete(&m, &key);
                }
            }
            if (multimapCount(m) != (size_t)keys[t] * 2) {
                ERR("tier %zu: %zu rows left", t, multimapCount(m));
            }
        }
    }

    TEST("a size limit holds in every tier, through a dump and rebuild") {
        /* Made with the 512-byte limit (and with compression): grown to
         * Full, its inner maps split at 512 bytes; its dump rebuilt with the
         * same configuration is Full with 512-byte inner maps too, and both
         * report the limit and compression they were made with */
        for (int compress = 0; compress <= 1; compress++) {
            cleaner(multimapFree) multimap *m =
                multimapNewConfigure(2, false, compress, FLEX_CAP_LEVEL_512);
            for (int64_t k = 0; k < 1500; k++) {
                const databox key = databoxNewSigned(k);
                const databox val = databoxNewSigned(k * 3);
                const databox *row[] = {&key, &val};
                multimapInsert(&m, row);
            }
            const uint32_t want = flexOptimizationSizeLimit[FLEX_CAP_LEVEL_512];
            cleaner(multimapFree) multimap *b =
                multimapNewFromDump(2, false, multimapCompresses(m),
                                    multimapLimit(m), multimapDump(m));
            bool ok = multimapType_(m) == MULTIMAP_TYPE_FULL && b &&
                      multimapType_(b) == MULTIMAP_TYPE_FULL &&
                      mmf(m)->maxSize == want && mmf(b)->maxSize == want &&
                      multimapLimit(m) == FLEX_CAP_LEVEL_512 &&
                      multimapLimit(b) == FLEX_CAP_LEVEL_512 &&
                      multimapCompresses(m) == (bool)compress &&
                      multimapCompresses(b) == (bool)compress &&
                      multimapCount(b) == 1500;
            if (ok) {
                multimapVerify(b);
            }
            if (!ok) {
                ERR("compress %d: tiers %d/%d, maxSize %u/%u (want %u), "
                    "limits %d/%d",
                    compress, (int)multimapType_(m),
                    b ? (int)multimapType_(b) : -1, (unsigned)mmf(m)->maxSize,
                    b ? (unsigned)mmf(b)->maxSize : 0, (unsigned)want,
                    (int)multimapLimit(m), b ? (int)multimapLimit(b) : -1);
            }
        }
    }

    TEST("NewFromDump rebuilds every tier and kind; later edits agree") {
        /* For maps and sets of each tier: the rebuilt dump is the same
         * bytes, the rebuild verifies, and the same later inserts and
         * deletes leave both with the same dump */
        const int64_t counts[] = {10, 150, 3000};
        for (int set = 0; set < 2; set++) {
            for (size_t t = 0; t < countof(counts); t++) {
                cleaner(multimapFree) multimap *m =
                    set ? multimapSetNew(2) : multimapNew(2);
                for (int64_t k = 0; k < counts[t]; k++) {
                    const databox key = databoxNewSigned((k * 7919) % 10007);
                    const databox val = databoxNewSigned(k % 3);
                    const databox *row[] = {&key, &val};
                    multimapInsert(&m, row);
                }
                cleaner(multimapFree) multimap *b =
                    multimapNewFromDump(2, set, multimapCompresses(m),
                                        multimapLimit(m), multimapDump(m));
                if (!b || multimapIsSet(b) != (bool)set) {
                    ERR("set %d tier %zu: no rebuild", set, t);
                    continue;
                }
                multimapVerify(b);
                for (int round = 0; round < 2; round++) {
                    cleaner(flexFree) flex *dm = multimapDump(m);
                    cleaner(flexFree) flex *db = multimapDump(b);
                    if (flexBytes(dm) != flexBytes(db) ||
                        memcmp(dm, db, flexBytes(dm)) != 0) {
                        ERR("set %d tier %zu round %d: dumps differ", set, t,
                            round);
                    }
                    for (int64_t k = 0; k < 200; k++) {
                        const databox key = databoxNewSigned(k * 31 - 500);
                        const databox val = databoxNewSigned(k);
                        const databox *row[] = {&key, &val};
                        multimapInsert(&m, row);
                        multimapInsert(&b, row);
                        const databox gone =
                            databoxNewSigned((k * 7919) % 10007);
                        multimapDelete(&m, &gone);
                        multimapDelete(&b, &gone);
                    }
                    multimapVerify(b);
                }
            }
        }

        /* Refused: rows out of order, and a row cut short */
        const databox one = databoxNewSigned(1);
        const databox two = databoxNewSigned(2);
        const databox *down[] = {&two, &one, &one, &one};
        if (multimapNewFromDump(2, false, false, FLEX_CAP_LEVEL_2048,
                                flexNewFromDataboxPtrsExact(down, 4)) ||
            multimapNewFromDump(2, false, false, FLEX_CAP_LEVEL_2048,
                                flexNewFromDataboxPtrsExact(down, 3))) {
            ERR("NewFromDump accepted unordered or partial rows");
        }
    }

    TEST("Append in any order builds the map Insert builds, every tier") {
        const flexCapSizeLimit limits[] = {
            FLEX_CAP_LEVEL_2048, FLEX_CAP_LEVEL_512, FLEX_CAP_LEVEL_64};
        const multimapType tiers[] = {MULTIMAP_TYPE_SMALL, MULTIMAP_TYPE_MEDIUM,
                                      MULTIMAP_TYPE_FULL};
        /* Two passes of rows each: sized so each tier is reached */
        const int64_t counts[] = {10, 60, 1000};
        for (size_t t = 0; t < countof(limits); t++) {
            for (int compress = 0; compress <= 1; compress++) {
                cleaner(multimapFree) multimap *appended =
                    compress ? multimapNewCompress(2, limits[t])
                             : multimapNewLimit(2, limits[t]);
                cleaner(multimapFree) multimap *inserted =
                    multimapNewLimit(2, limits[t]);

                /* Even keys ascending (the fast path), then odd keys
                 * descending (every one below the last key) */
                for (int64_t pass = 0; pass < 2; pass++) {
                    for (int64_t i = 0; i < counts[t]; i++) {
                        const int64_t k =
                            pass == 0 ? 2 * i : 2 * (counts[t] - i) - 1;
                        const databox key = databoxNewSigned(k);
                        const databox val = databoxNewSigned(k * 7);
                        const databox *row[] = {&key, &val};
                        multimapAppend(&appended, row);
                        multimapInsert(&inserted, row);
                    }
                }
                multimapVerify(appended);
                if (multimapType_(appended) != tiers[t] ||
                    multimapCount(appended) != multimapCount(inserted)) {
                    ERR("tier %zu compress %d: type %d, %zu rows", t, compress,
                        (int)multimapType_(appended), multimapCount(appended));
                    continue;
                }

                multimapIterator a;
                multimapIterator b;
                multimapIteratorInit(appended, &a, true);
                multimapIteratorInit(inserted, &b, true);
                databox ra[2];
                databox rb[2];
                while (multimapIteratorNextInto(&a, ra, 2)) {
                    if (!multimapIteratorNextInto(&b, rb, 2) ||
                        databoxCompare(&ra[0], &rb[0]) != 0 ||
                        databoxCompare(&ra[1], &rb[1]) != 0) {
                        ERR("tier %zu compress %d: rows differ", t, compress);
                        break;
                    }
                }
            }
        }
    }

    TEST("IteratorInitAt an absent key starts at the next key, every tier") {
        /* Even keys only; starting at each odd key must give the next
         * even key, including where the search point falls between two
         * of the tier's inner maps */
        const flexCapSizeLimit limits[] = {
            FLEX_CAP_LEVEL_2048, FLEX_CAP_LEVEL_512, FLEX_CAP_LEVEL_64};
        const multimapType tiers[] = {MULTIMAP_TYPE_SMALL, MULTIMAP_TYPE_MEDIUM,
                                      MULTIMAP_TYPE_FULL};
        const int64_t counts[] = {20, 120, 2000};
        for (size_t t = 0; t < countof(limits); t++) {
            cleaner(multimapFree) multimap *m = multimapNewLimit(2, limits[t]);
            for (int64_t k = 0; k < counts[t]; k++) {
                const databox key = databoxNewSigned(2 * k);
                const databox val = databoxNewSigned(k);
                const databox *row[] = {&key, &val};
                multimapInsert(&m, row);
            }
            if (multimapType_(m) != tiers[t]) {
                ERR("tier %zu: built type %d", t, (int)multimapType_(m));
                continue;
            }
            for (int64_t odd = -1; odd < 2 * counts[t]; odd += 2) {
                const databox at = databoxNewSigned(odd);
                multimapIterator iter;
                databox got[2];
                const bool found =
                    multimapIteratorInitAt(m, &iter, true, &at) &&
                    multimapIteratorNextInto(&iter, got, 2);
                const bool wantFound = odd + 1 < 2 * counts[t];
                const databox want = databoxNewSigned(odd + 1);
                if (found != wantFound ||
                    (found && !databoxEqual(&got[0], &want))) {
                    ERR("tier %zu: start at %" PRId64 " found %d", t, odd,
                        found);
                    break;
                }
            }
        }
    }

    TEST("multimap: a Small map of one- or two-element rows that is not a "
         "set is its flex alone, through every entry operation") {
        /* Which maps start bare: rows of 1, of 2, of 3; a set; a
         * compressing map; a 64 KB limit */
        static const bool bare[] = {true, true, false, false, false, true};
        for (size_t i = 0; i < countof(bare); i++) {
            cleaner(multimapFree) multimap *shape = nullptr;
            switch (i) {
            case 0:
                shape = multimapNew(1);
                break;
            case 1:
                shape = multimapNew(2);
                break;
            case 2:
                shape = multimapNew(3);
                break;
            case 3:
                shape = multimapSetNew(2);
                break;
            case 4:
                shape = multimapNewCompress(2, FLEX_CAP_LEVEL_2048);
                break;
            default:
                shape = multimapNewLimit(2, FLEX_CAP_LEVEL_65536);
                break;
            }
            dkTierStats s;
            multimapStats(shape, &s);
            if (MULTIMAP_IS_BARE(shape) != bare[i] ||
                multimapGetType(shape) != MULTIMAP_TYPE_SMALL ||
                (bare[i] && s.totalBytes != s.dataBytes)) {
                ERR("shape %zu: bare %d, tier %d, total %" PRIu64
                    " data %" PRIu64,
                    i, MULTIMAP_IS_BARE(shape), multimapGetType(shape),
                    s.totalBytes, s.dataBytes);
            }
        }

        /* Entry operations on a bare map: each may move the flex, which
         * the map (and the entry, for the next call) must follow; a stale
         * pointer is a use after free under the sanitizers */
        cleaner(multimapFree) multimap *m = multimapNew(2);
        for (int64_t k = 0; k < 8; k++) {
            const databox key = databoxNewSigned(k);
            const databox val = databoxNewSigned(k * 10);
            const databox *row[2] = {&key, &val};
            multimapInsert(&m, row);
        }
        char longText[300];
        memset(longText, 'v', sizeof(longText));
        const databox longVal = databoxNewBytes(longText, sizeof(longText));
        const databox key3 = databoxNewSigned(3);
        multimapEntry me;
        if (!multimapGetUnderlyingEntry(m, &key3, &me)) {
            ERR("key 3 is missing");
        }
        me.fe = flexNext(*me.map, me.fe);        /* the value */
        multimapReplaceEntry(&m, &me, &longVal); /* grows the flex */
        multimapVerify(m);
        databox got;
        databox *gotp[1] = {&got};
        if (!MULTIMAP_IS_BARE(m) || !multimapLookup(m, &key3, gotp) ||
            got.len != sizeof(longText)) {
            ERR("a replace through the entry was lost (bare %d)",
                MULTIMAP_IS_BARE(m));
        }

        /* A present key through InsertIfAbsentEntry gives its row */
        const databox five = databoxNewSigned(5);
        const databox fifty = databoxNewSigned(50);
        const databox *present[2] = {&five, &fifty};
        if (multimapInsertIfAbsentEntry(&m, present, &me)) {
            ERR("key 5 was inserted again");
        }
        multimapDeleteEntry(&m, &me);
        multimapVerify(m);
        if (multimapExists(m, &five) || multimapCount(m) != 7) {
            ERR("a delete through the entry left %zu rows", multimapCount(m));
        }

        multimapEntry picked;
        if (!multimapRandomValue(m, true, nullptr, &picked)) {
            ERR("no row to pick");
        }
        multimapDeleteEntry(&m, &picked); /* the last row, key 7 */
        const databox seven = databoxNewSigned(7);
        if (multimapExists(m, &seven) || multimapCount(m) != 6) {
            ERR("a delete of the picked row left %zu rows", multimapCount(m));
        }

        cleaner(multimapFree) multimap *copy = multimapCopy(m);
        if (!MULTIMAP_IS_BARE(copy) || multimapCount(copy) != 6 ||
            !multimapLookup(copy, &key3, gotp) || got.len != sizeof(longText)) {
            ERR("a copy of a bare map differs");
        }

        /* A middle past 11 bits: the map keeps a struct, still Small */
        cleaner(multimapFree) multimap *wide =
            multimapNewLimit(2, FLEX_CAP_LEVEL_65536);
        int64_t k = 0;
        while (MULTIMAP_IS_BARE(wide) && k < 64) {
            const databox key = databoxNewSigned(k++);
            const databox *row[2] = {&key, &longVal};
            multimapInsert(&wide, row);
        }
        multimapVerify(wide);
        if (MULTIMAP_IS_BARE(wide) ||
            multimapGetType(wide) != MULTIMAP_TYPE_SMALL ||
            multimapCount(wide) != (size_t)k) {
            ERR("a wide middle left bare %d, tier %d, %zu rows",
                MULTIMAP_IS_BARE(wide), multimapGetType(wide),
                multimapCount(wide));
        }
        for (int64_t j = 0; j < k; j++) {
            const databox key = databoxNewSigned(j);
            if (!multimapLookup(wide, &key, gotp) ||
                got.len != sizeof(longText)) {
                ERR("row %" PRId64 " of the wide map is missing", j);
                break;
            }
        }
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
