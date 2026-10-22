#include "multilist.h"

#include "multilistSmall.h"
#include "multilistSmallInternal.h"

#include "multilistMedium.h"
#include "multilistMediumInternal.h"

#include "multilistFull.h"

#include "flexCapacityManagement.h"

#if MULTILIST_INLINE
#include "multilistFull.c"
#include "multilistMedium.c"
#include "multilistSmall.c"
#endif

#define PTRLIB_BITS_LOW 2
#define PTRLIB_BITS_HIGH 16
#include "ptrlib.h"
#include "ptrlibCompress.h"

/* multilistFull has different arguments than Small/Medium, so we need to make
 * fake prototypes for each Small/Medium function to throw away the unused
 * compression state argument. */
#include "multilistAdapter.h"

#include "cleaner.h"

defcleanerName(flex **, zfree, Flexes);

typedef enum multilistType {
    MULTILIST_TYPE_INVALID = 0,
    MULTILIST_TYPE_SMALL = 1,  /* 8 bytes, fixed. */
    MULTILIST_TYPE_MEDIUM = 2, /* 16 bytes, fixed. */
    MULTILIST_TYPE_FULL = 3,   /* 24 bytes, grows as necessary. */
} multilistType;

#define _multilistType(list) _PTRLIB_TYPE(list)
#define _MULTILIST_USE(list) _PTRLIB_TOP_USE_ALL(list)
#define _MULTILIST_TAG(list, type) ((multilist *)_PTRLIB_TAG(list, type))

#define mls(m) ((multilistSmall *)_MULTILIST_USE(m))
#define mlm(m) ((multilistMedium *)_MULTILIST_USE(m))
#define mlf(m) ((multilistFull *)_MULTILIST_USE(m))

#define _MULTILIST(ret, m, func, ...)                                          \
    do {                                                                       \
        switch (_multilistType(m)) {                                           \
        case MULTILIST_TYPE_SMALL:                                             \
            ret multilistSmallAdapter##func(mls(m), __VA_ARGS__);              \
            break;                                                             \
        case MULTILIST_TYPE_MEDIUM:                                            \
            ret multilistMediumAdapter##func(mlm(m), __VA_ARGS__);             \
            break;                                                             \
        case MULTILIST_TYPE_FULL:                                              \
            ret multilistFull##func(mlf(m), __VA_ARGS__);                      \
            break;                                                             \
        default:                                                               \
            assert(NULL);                                                      \
            __builtin_unreachable();                                           \
        }                                                                      \
    } while (0)

#define MULTILIST_NORETURN(m, func, ...) _MULTILIST(, m, func, __VA_ARGS__)
#define MULTILIST_RETURN(m, func, ...) _MULTILIST(return, m, func, __VA_ARGS__)

#define _MULTILIST_SINGLE(ret, m, func)                                        \
    do {                                                                       \
        switch (_multilistType(m)) {                                           \
        case MULTILIST_TYPE_SMALL:                                             \
            ret multilistSmall##func(mls(m));                                  \
            break;                                                             \
        case MULTILIST_TYPE_MEDIUM:                                            \
            ret multilistMedium##func(mlm(m));                                 \
            break;                                                             \
        case MULTILIST_TYPE_FULL:                                              \
            ret multilistFull##func(mlf(m));                                   \
            break;                                                             \
        default:                                                               \
            assert(NULL);                                                      \
            __builtin_unreachable();                                           \
        }                                                                      \
    } while (0)

#define MULTILIST_SINGLE_NORETURN(m, func) _MULTILIST_SINGLE(, m, func)
#define MULTILIST_SINGLE_RETURN(m, func) _MULTILIST_SINGLE(return, m, func)

/* ====================================================================
 * Manage
 * ==================================================================== */
multilist *multilistNew(const flexCapSizeLimit limit, const uint32_t depth) {
    multilist *ml = (multilist *)multilistSmallCreate();
    ml = SET_COMPRESS_DEPTH_LIMIT(ml, depth, limit);
    return _MULTILIST_TAG(ml, MULTILIST_TYPE_SMALL);
}

multilist *multilistNewFromFlex(const flexCapSizeLimit limit,
                                const uint32_t depth, flex *fl) {
    /* Retain 'fl' inside our new multilistSmall.
     * Note: we don't auto-upgrade here if the flex is too big.
     * During the next data write operation, the multilistUpgradeIfNecessary()
     * call will be triggered to become MEDIUM or FULL as required. */
    multilist *ml = (multilist *)multilistSmallNewFromFlexConsume(fl);
    ml = SET_COMPRESS_DEPTH_LIMIT(ml, depth, limit);
    return _MULTILIST_TAG(ml, MULTILIST_TYPE_SMALL);
}

multilist *multilistDuplicate(const multilist *mlOrig) {
    static const size_t **dupers[4] = {NULL, &&sm, &&md, &&full};

    const multilistType type = _multilistType(mlOrig);
    goto *dupers[type];

    void *ml;
sm:
    ml = multilistSmallDuplicate(mls(mlOrig));
    goto goodbye;

md:
    ml = multilistMediumDuplicate(mlm(mlOrig));
    goto goodbye;

full:
    ml = multilistFullDuplicate(mlf(mlOrig));
    /* fallthrough */

goodbye:
    ml = SET_COMPRESS_DEPTH_LIMIT(ml, COMPRESS_DEPTH(mlOrig),
                                  COMPRESS_LIMIT(mlOrig));
    return _MULTILIST_TAG(ml, type);
}

void multilistFree(multilist *m) {
    if (m) {
        MULTILIST_SINGLE_NORETURN(m, Free);
    }
}

flexCapSizeLimit multilistLimit(const multilist *ml) {
    return (flexCapSizeLimit)COMPRESS_LIMIT(ml);
}

uint32_t multilistDepth(const multilist *ml) {
    return COMPRESS_DEPTH(ml);
}

void multilistReset(multilist **ml) {
    const flexCapSizeLimit limit = COMPRESS_LIMIT(*ml);
    const uint32_t depth = COMPRESS_DEPTH(*ml);
    multilistFree(*ml);
    /* @own: caller; free=multilistFree */
    *ml = multilistNew(limit, depth);
}

size_t multilistCount(const multilist *m) {
    MULTILIST_SINGLE_RETURN(m, Count);
}

size_t multilistBytes(const multilist *m) {
    MULTILIST_SINGLE_RETURN(m, Bytes);
}

static void multilistUpgradeIfNecessary(multilist **m, mflexState *state,
                                        const uint_fast32_t depth,
                                        const uint_fast32_t limit) {
    const multilistType type = _multilistType(*m);
    if (type == MULTILIST_TYPE_SMALL) {
        multilistSmall *small = mls(*m);
        /* Statically defined growth cases: fix?  parameterize?  modularize? */
        if (multilistSmallBytes(small) > flexOptimizationSizeLimit[limit]) {
            multilistMedium *medium =
                multilistMediumNewFromFlexConsumeGrow(small, small->fl);
            /* Tag the metadata-carrying pointer (*m holds the
             * depth/limit top bits; tagging the raw pointer would
             * discard them and break the Medium->Full threshold) */
            *m = SET_COMPRESS_DEPTH_LIMIT(medium, depth, limit);
            *m = _MULTILIST_TAG(*m, MULTILIST_TYPE_MEDIUM);
        }
    } else if (type == MULTILIST_TYPE_MEDIUM) {
        multilistMedium *medium = mlm(*m);
        if (multilistMediumBytes(medium) >
            (flexOptimizationSizeLimit[limit] * 3)) {
            flex *f[2] = {medium->fl[0], medium->fl[1]};
            multilistFull *full = multilistFullNewFromFlexConsumeGrow(
                medium, state, f, 2, depth, limit);
            *m = SET_COMPRESS_DEPTH_LIMIT(full, depth, limit);
            *m = _MULTILIST_TAG(*m, MULTILIST_TYPE_FULL);
        }
    } /* else, is FULL and no grow necessary. */
}

multilist *multilistNewFromDump(flexCapSizeLimit limit, uint32_t depth,
                                mflexState *state, flex *elements) {
    const size_t bytes = flexBytes(elements);
    const size_t nodeBytes = flexOptimizationSizeLimit[limit];

    /* What Small or Medium holds: adopt it, then grow as a write would */
    if (bytes <= nodeBytes * 3) {
        multilist *ml = multilistNewFromFlex(limit, depth, elements);
        multilistUpgradeIfNecessary(&ml, state, depth, limit);
        return ml;
    }

    /* Larger: cut whole elements into chunks of about two nodes (tail
     * first, so each cut only shortens the dump); Full splits each chunk
     * into two nodes and applies the compression depth once */
    const size_t count = flexCount(elements);
    size_t perChunk = (count * 2 * nodeBytes) / bytes;
    perChunk = perChunk ? perChunk : 1;
    const size_t chunks = (count + perChunk - 1) / perChunk;
    cleanerName(zfree, Flexes) flex **chunk = zcalloc(chunks, sizeof(flex *));
    for (size_t i = chunks - 1; i > 0; i--) {
        const size_t end =
            (i + 1) * perChunk < count ? (i + 1) * perChunk : count;
        const size_t entries = end - i * perChunk;
        /* @own: container; free=flexFree */
        chunk[i] =
            flexSplitRange(&elements, -(int32_t)entries, (uint32_t)entries);
    }
    chunk[0] = elements;

    /* @own: caller; free=multilistFree */
    multilistFull *full = multilistFullNewFromFlexConsumeGrow(
        NULL, state, chunk, chunks, depth, limit);
    multilist *ml = SET_COMPRESS_DEPTH_LIMIT(full, depth, limit);
    return _MULTILIST_TAG(ml, MULTILIST_TYPE_FULL);
}

/* ====================================================================
 * Insert
 * ==================================================================== */
void multilistPushByTypeHead(multilist **ml, mflexState *state,
                             const databox *box) {
    MULTILIST_NORETURN(*ml, PushByTypeHead, state, box);
    const uint32_t depth = COMPRESS_DEPTH(*ml);
    const uint32_t limit = COMPRESS_LIMIT(*ml);
    multilistUpgradeIfNecessary(ml, state, depth, limit);
}

void multilistPushByTypeTail(multilist **ml, mflexState *state,
                             const databox *box) {
    MULTILIST_NORETURN(*ml, PushByTypeTail, state, box);
    const uint32_t depth = COMPRESS_DEPTH(*ml);
    const uint32_t limit = COMPRESS_LIMIT(*ml);
    multilistUpgradeIfNecessary(ml, state, depth, limit);
}

void multilistInsertByTypeAfter(multilist **ml, mflexState *state[2],
                                multilistEntry *node, const databox *box) {
    MULTILIST_NORETURN(*ml, InsertByTypeAfter, state, node, box);
    const uint32_t depth = COMPRESS_DEPTH(*ml);
    const uint32_t limit = COMPRESS_LIMIT(*ml);
    multilistUpgradeIfNecessary(ml, state[0], depth, limit);
}

void multilistInsertByTypeBefore(multilist **ml, mflexState *state[2],
                                 multilistEntry *node, const databox *box) {
    MULTILIST_NORETURN(*ml, InsertByTypeBefore, state, node, box);
    const uint32_t depth = COMPRESS_DEPTH(*ml);
    const uint32_t limit = COMPRESS_LIMIT(*ml);
    multilistUpgradeIfNecessary(ml, state[0], depth, limit);
}

/* ====================================================================
 * Remove
 * ==================================================================== */
void multilistDelEntry(multilistIterator *iter, multilistEntry *entry) {
    static const size_t **dupers[4] = {NULL, &&sm, &&md, &&full};

    goto *dupers[iter->type];

sm:
    multilistSmallDelEntry(iter, entry);
    return;

md:
    multilistMediumDelEntry(iter, entry);
    return;

full:
    multilistFullDelEntry(iter, entry);
    return;
}

bool multilistDelRange(multilist **m, mflexState *state, mlOffsetId start,
                       int64_t values) {
    MULTILIST_RETURN(*m, DelRange, state, start, values);
}

bool multilistReplaceByTypeAtIndex(multilist **m, mflexState *state,
                                   mlOffsetId index, const databox *box) {
    MULTILIST_RETURN(*m, ReplaceByTypeAtIndex, state, index, box);
}

/* ====================================================================
 * Iterate
 * ==================================================================== */
bool multilistIndex(const multilist *ml, mflexState *state,
                    const mlOffsetId index, multilistEntry *entry,
                    const bool openNode) {
    MULTILIST_RETURN(ml, Index, state, index, entry, openNode);
}

/* Index starting from a caller-held anchor (a previously located entry
 * plus its known global index) when the anchor is closer to the target
 * than either endpoint: "grab an existing reference and walk to the
 * target" in either direction.  On the Full tier whole nodes are
 * skipped by their counts from the anchor's node; Small/Medium have a
 * single flex pair where the two-way endpoint walk is already optimal.
 * The anchor must have been obtained after the most recent mutation.
 * Never slower than multilistIndex. */
bool multilistIndexNear(const multilist *ml, mflexState *state,
                        const mlOffsetId index, const multilistEntry *anchor,
                        const mlOffsetId anchorIdx, multilistEntry *entry,
                        const bool openNode) {
    MULTILIST_RETURN(ml, IndexNear, state, index, anchor, anchorIdx, entry,
                     openNode);
}

/* The Wide forms run the plain lookup, which reports a wide element
 * deferred, and decode that one element again into 'slot'.  entry->fe is
 * the element's live position (an opened node on the Full tier), so the
 * second decode is a cold arm that narrow elements never reach. */
static bool multilistIndexWideFinish_(multilistEntry *entry,
                                      databoxWide *slot) {
    if (unlikely(databoxIsWideDeferred(&entry->box))) {
        flexGetByTypeInto(entry->fe, &entry->box, slot);
    }

    return true;
}

bool multilistIndexWide(const multilist *ml, mflexState *state,
                        const mlOffsetId index, multilistEntry *entry,
                        databoxWide *slot) {
    return multilistIndex(ml, state, index, entry, true) &&
           multilistIndexWideFinish_(entry, slot);
}

bool multilistIndexNearWide(const multilist *ml, mflexState *state,
                            const mlOffsetId index,
                            const multilistEntry *anchor,
                            const mlOffsetId anchorIdx, multilistEntry *entry,
                            databoxWide *slot) {
    return multilistIndexNear(ml, state, index, anchor, anchorIdx, entry,
                              true) &&
           multilistIndexWideFinish_(entry, slot);
}

void multilistIteratorInit(multilist *ml, mflexState *state[2],
                           multilistIterator *iter, bool forward,
                           bool readOnly) {
    iter->type = _multilistType(ml);
    MULTILIST_NORETURN(ml, IteratorInit, state, iter, forward, readOnly);
}

bool multilistIteratorInitAtIdx(const multilist *ml, mflexState *state[2],
                                multilistIterator *iter, mlOffsetId idx,
                                bool forward, bool readOnly) {
    iter->type = _multilistType(ml);
    MULTILIST_RETURN(ml, IteratorInitAtIdx, state, iter, idx, forward,
                     readOnly);
}

void multilistIteratorRelease(multilistIterator *iter) {
    /* Small and Medium have no iterator state to release... */
    if (iter->type == MULTILIST_TYPE_FULL) {
        multilistFullIteratorRelease(iter);
    }
}

/* Restart a PREVIOUSLY INITIALIZED iterator from the head (Rewind) or
 * tail (RewindTail), reusing the iterator's retained mflex states and
 * readOnly mode.  Any open node is released first, so rewinding is
 * always safe mid-iteration. */
void multilistRewind(multilist *ml, multilistIterator *iter) {
    multilistIteratorRelease(iter);
    multilistIteratorInit(ml, iter->state, iter, true, iter->readOnly);
}

void multilistRewindTail(multilist *ml, multilistIterator *iter) {
    multilistIteratorRelease(iter);
    multilistIteratorInit(ml, iter->state, iter, false, iter->readOnly);
}

bool multilistNext(multilistIterator *iter, multilistEntry *entry) {
    static const size_t **dupers[4] = {NULL, &&sm, &&md, &&full};

    goto *dupers[iter->type];

sm:
    return multilistSmallNext(iter, entry);

md:
    return multilistMediumNext(iter, entry);

full:
    return multilistFullNext(iter, entry);
}

bool multilistPop(multilist **m, mflexState *state, databox *got,
                  bool fromTail) {
    MULTILIST_RETURN(*m, Pop, state, got, fromTail);
}

void multilistRotate(multilist *m, mflexState *state[2]) {
    MULTILIST_NORETURN(m, Rotate, state);
}

flex *multilistDump(const multilist *m, mflexState *state) {
    MULTILIST_RETURN(m, Dump, state);
}

/* ====================================================================
 * Testing
 * ==================================================================== */
#ifdef DATAKIT_TEST

#include "cleaner.h"
#include "str.h" /* for StrInt64ToBuf */
#include "timeUtil.h"
#include <inttypes.h>

#include "ctest.h"

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleanerName(int64_t *, zfree, I64);
defcleaner(multilist *, multilistFree);
defcleaner(mflexState *, mflexStateFree);
defcleaner(flex *, flexFree);
defcleaner(multilistSmall *, multilistSmallFree);
defcleaner(multilistMedium *, multilistMediumFree);
defcleaner(multilistFull *, multilistFullFree);

CTEST_INCLUDE_GEN(str)

#if 0
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
#endif
void multilistRepr(const multilist *m) {
    const multilistType type = _multilistType(m);
    printf("multilist type: %s\n", type == MULTILIST_TYPE_SMALL    ? "SMALL"
                                   : type == MULTILIST_TYPE_MEDIUM ? "MEDIUM"
                                                                   : "FULL");
    MULTILIST_SINGLE_NORETURN(m, Repr);
}

#define multilistPush_(WHAT, a, b, c, d)                                       \
    do {                                                                       \
        databox pushBox_ = databoxNewBytesString(c);                           \
        pushBox_.len = d;                                                      \
        multilistPushByType##WHAT(&a, b, &pushBox_);                           \
    } while (0)

#define multilistPushHead(a, b, c, d) multilistPush_(Head, a, b, c, d)
#define multilistPushTail(a, b, c, d) multilistPush_(Tail, a, b, c, d)

static void multilistInsertBefore(multilist **ml, mflexState *state[2],
                                  multilistEntry *entry, void *data,
                                  size_t len) {
    multilistInsertByTypeBefore(ml, state, entry,
                                &DATABOX_WITH_BYTES(data, len));
}

static void multilistInsertAfter(multilist **ml, mflexState *state[2],
                                 multilistEntry *entry, void *data,
                                 size_t len) {
    multilistInsertByTypeAfter(ml, state, entry,
                               &DATABOX_WITH_BYTES(data, len));
}

static bool multilistReplaceAtIndex(multilist **ml, mflexState *state,
                                    mlOffsetId index, void *data, size_t len) {
    return multilistReplaceByTypeAtIndex(ml, state, index,
                                         &DATABOX_WITH_BYTES(data, len));
}

/* ====================================================================
 * 96/128-bit element test support
 * ==================================================================== */
/* Independent oracle for one element: an integer is its two's-complement
 * bits plus its sign, and a string is the id it was generated from. */
typedef struct mlwVal {
    __uint128_t bits;
    bool negative;
    bool isStr;
    uint32_t strId;
} mlwVal;

#define MLW_MAX 1024

static bool mlwIsWide(const mlwVal *v) {
    if (v->isStr) {
        return false;
    }

    if (v->negative) {
        return (__int128_t)v->bits < (__int128_t)INT64_MIN;
    }

    return v->bits > UINT64_MAX;
}

/* The values just past each 64/96/128-bit limit, both signs. */
static mlwVal mlwBoundary(uint32_t which) {
    const __uint128_t one = 1;
    mlwVal v = {0};
    switch (which % 9) {
    case 0:
        v.bits = one << 64;
        break;
    case 1:
        v.bits = (one << 95) - 1;
        break;
    case 2:
        v.bits = one << 95;
        break;
    case 3:
        v.bits = one << 127;
        break;
    case 4:
        v.bits = ~(__uint128_t)0;
        break;
    case 5:
        v.bits = (__uint128_t)(-((__int128_t)1 << 63) - 1);
        v.negative = true;
        break;
    case 6:
        v.bits = (__uint128_t)(-((__int128_t)1 << 95));
        v.negative = true;
        break;
    case 7:
        v.bits = one << 127; /* INT128_MIN */
        v.negative = true;
        break;
    default:
        v.bits = (one << 127) - 1; /* INT128_MAX */
        break;
    }

    return v;
}

/* Element 'i' of a test list: boundary wide values, generated wide values
 * of 65..126 significant bits, 64-bit values (including the 64-bit
 * limits) and short and long strings, interleaved. */
static mlwVal mlwGen(uint32_t i) {
    mlwVal v = {0};
    const uint64_t mix = (uint64_t)(i + 1) * UINT64_C(0x9E3779B97F4A7C15);
    switch (i % 5) {
    case 0:
        return mlwBoundary(i / 5);
    case 1:
        v.negative = (int64_t)mix < 0;
        v.bits = (__uint128_t)(__int128_t)(int64_t)mix;
        return v;
    case 2:
        v.isStr = true;
        v.strId = i;
        return v;
    case 3: {
        const uint64_t hi = (mix >> (2 + i % 40)) | 1;
        const __uint128_t mag = ((__uint128_t)hi << 64) | i;
        v.negative = (i & 8) != 0;
        v.bits = v.negative ? (__uint128_t)(-(__int128_t)mag) : mag;
        return v;
    }
    default:
        switch ((i / 5) % 3) {
        case 0:
            v.bits = UINT64_MAX - i;
            break;
        case 1:
            v.bits = (__uint128_t)(__int128_t)(INT64_MIN + (int64_t)i);
            v.negative = true;
            break;
        default:
            v.bits = i;
            break;
        }
        return v;
    }
}

/* The three data modes of the read matrix: normal holds only 64-bit
 * values and strings, extended only 96/128-bit values, and mixed
 * interleaves both (mlwGen itself). */
typedef enum mlwData {
    MLW_DATA_NORMAL = 0,
    MLW_DATA_EXTENDED,
    MLW_DATA_MIXED,
    MLW_DATA_COUNT
} mlwData;

static const char *const mlwDataName[MLW_DATA_COUNT] = {"normal", "extended",
                                                        "mixed"};

/* Element 'i' of a list in data mode 'data'.  Normal and extended pick
 * the matching mlwGen classes, so every id stays unique. */
static mlwVal mlwGenIn(mlwData data, uint32_t i) {
    static const uint32_t normalClass[3] = {1, 2, 4};
    static const uint32_t extendedClass[2] = {0, 3};
    switch (data) {
    case MLW_DATA_NORMAL:
        return mlwGen(5 * (i / 3) + normalClass[i % 3]);
    case MLW_DATA_EXTENDED:
        return mlwGen(5 * (i / 2) + extendedClass[i % 2]);
    default:
        return mlwGen(i);
    }
}

static size_t mlwStr(uint32_t id, char *buf, size_t cap) {
    /* Every third string is too long to embed in a databox. */
    const int n =
        id % 3 == 0
            ? snprintf(buf, cap, "wide-str-%" PRIu32 "-padpadpadpadpad", id)
            : snprintf(buf, cap, "ws%" PRIu32, id);
    return (size_t)n;
}

/* Build the box to push for 'v'.  A wide value lives in storage->extra
 * and a string in 'buf'.  Positive wide values that fit __int128 are
 * boxed signed or unsigned by turns; both must read back UNSIGNED_128. */
static const databox *mlwBox(const mlwVal *v, databoxBig *storage, char *buf,
                             size_t cap) {
    memset(storage, 0, sizeof(*storage));
    if (v->isStr) {
        const size_t len = mlwStr(v->strId, buf, cap);
        *(databox *)storage = databoxNewBytes(buf, len);
    } else if (mlwIsWide(v)) {
        if (v->negative ||
            (v->bits < ((__uint128_t)1 << 127) && (v->bits & 2))) {
            DATABOX_BIG_SIGNED_128(storage, (__int128_t)v->bits);
        } else {
            DATABOX_BIG_UNSIGNED_128(storage, v->bits);
        }
    } else if (v->negative) {
        *(databox *)storage = databoxNewSigned((int64_t)(__int128_t)v->bits);
    } else {
        *(databox *)storage = databoxNewUnsigned((uint64_t)v->bits);
    }

    return (const databox *)storage;
}

/* How a result was read, which decides what mlwCheck demands of it:
 *   - ENTRY: an entry from Next or a Wide read.  A wide value carries a
 *     live payload (big = 1, allocated = 0).  Narrow values have their
 *     flag bits left as they were, so those bits are not checked.
 *   - DEFERRED: an entry from Index / IndexNear.  A wide value is type
 *     only (databoxIsWideDeferred); narrow values are as for ENTRY.
 *   - OWNED: a Pop result, whose flags are all defined: a narrow value
 *     has big = 0. */
typedef enum mlwRead {
    MLW_READ_ENTRY = 0,
    MLW_READ_DEFERRED,
    MLW_READ_OWNED
} mlwRead;

/* NULL when 'box' holds exactly 'want', else what differs.  When
 * 'inlineAt' is set a wide payload must be there.  Expected types follow
 * the canonical representation: past 64 bits a negative value is
 * SIGNED_128 and a positive one UNSIGNED_128. */
static const char *mlwCheckRead(const databox *box, const mlwVal *want,
                                const void *inlineAt, mlwRead how) {
    if (want->isStr) {
        char buf[64];
        const size_t len = mlwStr(want->strId, buf, sizeof(buf));
        if (box->type != DATABOX_BYTES && box->type != DATABOX_BYTES_EMBED) {
            return "string read back as a non-string type";
        }

        if (databoxLen(box) != len || memcmp(databoxBytes(box), buf, len)) {
            return "string bytes differ";
        }

        return how == MLW_READ_OWNED && box->big ? "string has big set" : NULL;
    }

    if (mlwIsWide(want)) {
        if (box->type !=
            (want->negative ? DATABOX_SIGNED_128 : DATABOX_UNSIGNED_128)) {
            return "wide value read back with the wrong type";
        }

        if (how == MLW_READ_DEFERRED) {
            return databoxIsWideDeferred(box) && !box->big && !box->allocated
                       ? NULL
                       : "single read of a wide value is not deferred";
        }

        if (!box->big || !box->data.ptr) {
            return "wide value has no payload";
        }

        if (how == MLW_READ_ENTRY && box->allocated) {
            return "borrowed wide value claims to own its payload";
        }

        if (inlineAt && box->data.ptr != inlineAt) {
            return "wide payload is not in the destination's own storage";
        }

        __uint128_t got;
        memcpy(&got, box->data.ptr, sizeof(got));
        return got == want->bits ? NULL : "wide value differs";
    }

    if (how == MLW_READ_OWNED && box->big) {
        return "narrow value has big set";
    }

    if (box->type == DATABOX_SIGNED_64) {
        return (__uint128_t)(__int128_t)box->data.i == want->bits
                   ? NULL
                   : "signed 64-bit value differs";
    }

    if (box->type == DATABOX_UNSIGNED_64) {
        return !want->negative && (__uint128_t)box->data.u == want->bits
                   ? NULL
                   : "unsigned 64-bit value differs";
    }

    return "narrow value read back as a non-integer type";
}

static const char *mlwCheck(const databox *box, const mlwVal *want,
                            const void *inlineAt) {
    return mlwCheckRead(box, want, inlineAt, MLW_READ_ENTRY);
}

static void mlwInsertAt(mlwVal *want, size_t *n, size_t at, mlwVal v) {
    assert(*n < MLW_MAX && at <= *n);
    memmove(&want[at + 1], &want[at], (*n - at) * sizeof(*want));
    want[at] = v;
    (*n)++;
}

static void mlwRemoveAt(mlwVal *want, size_t *n, size_t at) {
    assert(at < *n);
    memmove(&want[at], &want[at + 1], (*n - at - 1) * sizeof(*want));
    (*n)--;
}

static void mlwPush(multilist **ml, mflexState *state, const mlwVal *v,
                    bool head) {
    databoxBig storage;
    char buf[64];
    const databox *box = mlwBox(v, &storage, buf, sizeof(buf));
    if (head) {
        multilistPushByTypeHead(ml, state, box);
    } else {
        multilistPushByTypeTail(ml, state, box);
    }
}

/* Iterate from 'start' (normalized like an index) to the end in the given
 * direction, decoding every element into a 0xFF-poisoned entry. */
static uint32_t mlwVerifyFrom(multilist *ml, mflexState *s[2],
                              const mlwVal *want, size_t n, mlOffsetId start,
                              bool forward, bool atIdx, const char *what) {
    uint32_t err = 0;
    multilistIterator iter;
    multilistEntry entry;
    memset(&iter, 0xFF, sizeof(iter));
    size_t idx = start < 0 ? (size_t)((mlOffsetId)n + start) : (size_t)start;
    if (atIdx) {
        if (!multilistIteratorInitAtIdx(ml, s, &iter, start, forward,
                                        forward)) {
            ERR("%s: IteratorInitAtIdx(%" PRId64 ") failed", what, start);
            return err;
        }
    } else {
        multilistIteratorInit(ml, s, &iter, forward, forward);
    }

    size_t seen = 0;
    const size_t expect = forward ? n - idx : idx + 1;
    for (;;) {
        memset(&entry, 0xFF, sizeof(entry));
        if (!multilistNext(&iter, &entry)) {
            break;
        }

        const char *why = seen < expect
                              ? mlwCheck(&entry.box, &want[idx], &iter.wide)
                              : "iterated past the end";
        if (why) {
            ERR("%s: Next %s from %" PRId64 " at %zu: %s", what,
                forward ? "forward" : "reverse", start, idx, why);
            break;
        }

        seen++;
        idx = forward ? idx + 1 : idx - 1;
    }

    multilistIteratorRelease(&iter);
    if (!err && seen != expect) {
        ERR("%s: Next %s from %" PRId64 " saw %zu of %zu", what,
            forward ? "forward" : "reverse", start, seen, expect);
    }

    return err;
}

/* Every read path over the whole list: Next both ways, then Index
 * (deferred for wide values) and IndexWide (whole, in the caller's slot)
 * at both signs, all into 0xFF-poisoned entries and slots. */
static uint32_t mlwVerify(multilist *ml, mflexState *s[2], const mlwVal *want,
                          size_t n, const char *what) {
    uint32_t err = 0;
    if (multilistCount(ml) != n) {
        ERR("%s: count %zu, want %zu", what, multilistCount(ml), n);
        return err;
    }

    if (n == 0) {
        return err;
    }

    err += mlwVerifyFrom(ml, s, want, n, 0, true, false, what);
    err += mlwVerifyFrom(ml, s, want, n, -1, false, false, what);
    for (size_t i = 0; i < n && !err; i++) {
        const mlOffsetId at[2] = {(mlOffsetId)i, -(mlOffsetId)(n - i)};
        for (size_t k = 0; k < 2 && !err; k++) {
            multilistEntry entry;
            memset(&entry, 0xFF, sizeof(entry));
            if (!multilistIndexGet(ml, s[0], at[k], &entry)) {
                ERR("%s: Index(%" PRId64 ") failed", what, at[k]);
                break;
            }

            const char *why =
                mlwCheckRead(&entry.box, &want[i], NULL, MLW_READ_DEFERRED);
            if (why) {
                ERR("%s: Index(%" PRId64 "): %s", what, at[k], why);
            }

            databoxWide slot;
            memset(&slot, 0xFF, sizeof(slot));
            memset(&entry, 0xFF, sizeof(entry));
            if (!multilistIndexWide(ml, s[0], at[k], &entry, &slot)) {
                ERR("%s: IndexWide(%" PRId64 ") failed", what, at[k]);
                break;
            }

            why = mlwCheck(&entry.box, &want[i], &slot);
            if (why) {
                ERR("%s: IndexWide(%" PRId64 "): %s", what, at[k], why);
            }
        }
    }

    return err;
}

#define OK printf("\tOK\n")
/* Tier Rewind test elements: fixed-width strings, one per id. */
#define MLR_LEN 24
static databox mlrBox(uint32_t id, char buf[MLR_LEN + 1]) {
    /* id % 1000000 keeps the id at six digits, so the string is exactly
     * MLR_LEN bytes and GCC can prove the buffer is large enough. */
    snprintf(buf, MLR_LEN + 1, "rw-%06" PRIu32 "-xxxxxxxxxxxxxx", id % 1000000);
    return databoxNewBytes(buf, MLR_LEN);
}

static bool mlrIs(const databox *box, uint32_t id) {
    char buf[MLR_LEN + 1];
    const databox want = mlrBox(id, buf);
    return box->type == DATABOX_BYTES && box->len == MLR_LEN &&
           memcmp(box->data.bytes.start, want.data.bytes.start, MLR_LEN) == 0;
}

/* Call the tier's own Rewind or RewindTail for 'ml'. */
static void mlrTierRewind(multilist *ml, multilistIterator *iter, bool tail) {
    switch (_multilistType(ml)) {
    case MULTILIST_TYPE_SMALL:
        if (tail) {
            multilistSmallRewindTail(mls(ml), iter);
        } else {
            multilistSmallRewind(mls(ml), iter);
        }
        break;
    case MULTILIST_TYPE_MEDIUM:
        if (tail) {
            multilistMediumRewindTail(mlm(ml), iter);
        } else {
            multilistMediumRewind(mlm(ml), iter);
        }
        break;
    case MULTILIST_TYPE_FULL:
        if (tail) {
            multilistFullRewindTail(mlf(ml), iter);
        } else {
            multilistFullRewind(mlf(ml), iter);
        }
        break;
    default:
        assert(NULL);
    }
}

/* Call the tier's own iterator release for 'ml'. */
static void mlrTierRelease(multilist *ml, multilistIterator *iter) {
    switch (_multilistType(ml)) {
    case MULTILIST_TYPE_SMALL:
        multilistSmallReleaseIterator(iter);
        break;
    case MULTILIST_TYPE_MEDIUM:
        multilistMediumReleaseIterator(iter);
        break;
    case MULTILIST_TYPE_FULL:
        multilistFullIteratorRelease(iter);
        break;
    default:
        assert(NULL);
    }
}

/* Compare 'ml' with the oracle 'want' (ids, head to tail) by its count
 * and by walking it forward and in reverse.  Returns NULL on a match,
 * else what differed. */
static const char *mlrListIs(multilist *ml, mflexState *s[2],
                             const uint32_t *want, uint32_t n) {
    if (multilistCount(ml) != n) {
        return "wrong count";
    }

    for (int dir = 0; dir < 2; dir++) {
        const bool forward = dir == 0;
        multilistIterator iter;
        multilistEntry entry;
        multilistIteratorInitReadOnly(ml, s, &iter, forward);
        uint32_t seen = 0;
        const char *why = NULL;
        while (!why && multilistNext(&iter, &entry)) {
            if (seen >= n ||
                !mlrIs(&entry.box, want[forward ? seen : n - 1 - seen])) {
                why = forward ? "forward walk differs" : "reverse walk differs";
            }

            seen++;
        }

        multilistIteratorRelease(&iter);
        if (!why && seen != n) {
            why = "walk ended early";
        }

        if (why) {
            return why;
        }
    }

    return NULL;
}

/* Whether flex 'f' holds exactly the ids 'want', in order. */
static bool mlaFlexIs(const flex *f, const uint32_t *want, uint32_t n) {
    if (flexCount(f) != n) {
        return false;
    }

    flexEntry *fe = flexHeadIfAny(f);
    for (uint32_t i = 0; i < n; i++, fe = flexNext(f, fe)) {
        if (!fe) {
            return false;
        }

        databox box = {{0}};
        flexGetByTypeInto(fe, &box, NULL);
        if (!mlrIs(&box, want[i])) {
            return false;
        }
    }

    return true;
}

/* Drive one tier through every multilist{Small,Medium}Adapter* macro
 * (and the tier functions they name), checking each result against the
 * id oracle 'want'.  PFX is Small or Medium; 'why' is set on the first
 * failure. */
#define MLA_EXERCISE(PFX, why)                                                 \
    do {                                                                       \
        char buf_[MLR_LEN + 1];                                                \
        uint32_t want_[16];                                                    \
        uint32_t n_ = 0;                                                       \
        databox box_;                                                          \
        cleaner(multilist##PFX##Free) multilist##PFX *ml_ =                    \
            multilist##PFX##AdapterNew(7, 3);                                  \
        multilist##PFX##AdapterSetCompressDepth(ml_, 2);                       \
        multilist##PFX##AdapterSetFill(ml_, 5);                                \
        multilist##PFX##AdapterSetOptions(ml_, 5, 2);                          \
        if (multilist##PFX##AdapterCount(ml_) != 0) {                          \
            why = "New is not empty";                                          \
            break;                                                             \
        }                                                                      \
                                                                               \
        /* [0, 1, 2] by pushes */                                              \
        box_ = mlrBox(1, buf_);                                                \
        multilist##PFX##AdapterPushByTypeTail(ml_, s0, &box_);                 \
        box_ = mlrBox(2, buf_);                                                \
        multilist##PFX##AdapterPushByTypeTail(ml_, s0, &box_);                 \
        box_ = mlrBox(0, buf_);                                                \
        multilist##PFX##AdapterPushByTypeHead(ml_, s0, &box_);                 \
        want_[n_++] = 0;                                                       \
        want_[n_++] = 1;                                                       \
        want_[n_++] = 2;                                                       \
                                                                               \
        /* AppendFlex [3, 4], AppendValuesFromFlex [5, 6] */                   \
        cleaner(flexFree) flex *f34_ = flexNew();                              \
        cleaner(flexFree) flex *f56_ = flexNew();                              \
        for (uint32_t i_ = 3; i_ <= 6; i_++) {                                 \
            box_ = mlrBox(i_, buf_);                                           \
            flexPushByType(i_ <= 4 ? &f34_ : &f56_, &box_,                     \
                           FLEX_ENDPOINT_TAIL);                                \
            want_[n_++] = i_;                                                  \
        }                                                                      \
        multilist##PFX##AdapterAppendFlex(ml_, f34_);                          \
        multilist##PFX##AdapterAppendValuesFromFlex(ml_, f56_);                \
                                                                               \
        /* Insert 100 before id 3 and 101 after it, through Index */           \
        multilistEntry e_;                                                     \
        if (!multilist##PFX##AdapterIndex(ml_, s0, 3, &e_, true)) {            \
            why = "Index(3) failed";                                           \
            break;                                                             \
        }                                                                      \
        box_ = mlrBox(100, buf_);                                              \
        multilist##PFX##AdapterInsertByTypeBefore(ml_, s, &e_, &box_);         \
        if (!multilist##PFX##AdapterIndex(ml_, s0, 4, &e_, true)) {            \
            why = "Index(4) failed";                                           \
            break;                                                             \
        }                                                                      \
        box_ = mlrBox(101, buf_);                                              \
        multilist##PFX##AdapterInsertByTypeAfter(ml_, s, &e_, &box_);          \
        memmove(&want_[4], &want_[3], (n_ - 3) * sizeof(*want_));              \
        want_[3] = 100;                                                        \
        n_++;                                                                  \
        memmove(&want_[6], &want_[5], (n_ - 5) * sizeof(*want_));              \
        want_[5] = 101;                                                        \
        n_++;                                                                  \
                                                                               \
        /* Replace the tail with 102; delete the head */                       \
        box_ = mlrBox(102, buf_);                                              \
        if (!multilist##PFX##AdapterReplaceByTypeAtIndex(ml_, s0, -1,          \
                                                         &box_) ||             \
            !multilist##PFX##AdapterDelRange(ml_, s0, 0, 1)) {                 \
            why = "Replace or DelRange failed";                                \
            break;                                                             \
        }                                                                      \
        want_[n_ - 1] = 102;                                                   \
        memmove(&want_[0], &want_[1], (n_ - 1) * sizeof(*want_));              \
        n_--;                                                                  \
                                                                               \
        /* IndexNear from an anchor at index 3 */                              \
        multilistEntry anchor_, near_;                                         \
        if (!multilist##PFX##AdapterIndex(ml_, s0, 3, &anchor_, true) ||       \
            !multilist##PFX##AdapterIndexNear(ml_, s0, 4, &anchor_, 3, &near_, \
                                              true) ||                         \
            !mlrIs(&near_.box, want_[4])) {                                    \
            why = "IndexNear misread";                                         \
            break;                                                             \
        }                                                                      \
                                                                               \
        /* Walk: DelEntry the head, Release, Rewind, RewindTail, AtIdx */      \
        multilistIterator it_;                                                 \
        memset(&it_, 0, sizeof(it_));                                          \
        multilist##PFX##AdapterIteratorInit(ml_, s, &it_, true, false);        \
        if (!multilist##PFX##AdapterNext(&it_, &e_) ||                         \
            !mlrIs(&e_.box, want_[0])) {                                       \
            why = "Next misread the head";                                     \
            break;                                                             \
        }                                                                      \
        multilist##PFX##AdapterDelEntry(&it_, &e_);                            \
        memmove(&want_[0], &want_[1], (n_ - 1) * sizeof(*want_));              \
        n_--;                                                                  \
        if (!multilist##PFX##AdapterNext(&it_, &e_) ||                         \
            !mlrIs(&e_.box, want_[0])) {                                       \
            why = "Next after DelEntry misread";                               \
            break;                                                             \
        }                                                                      \
        multilist##PFX##AdapterReleaseIterator(&it_);                          \
        multilist##PFX##AdapterRewind(ml_, &it_);                              \
        if (!multilist##PFX##AdapterNext(&it_, &e_) ||                         \
            !mlrIs(&e_.box, want_[0])) {                                       \
            why = "Rewind misread";                                            \
            break;                                                             \
        }                                                                      \
        multilist##PFX##AdapterRewindTail(ml_, &it_);                          \
        if (!multilist##PFX##AdapterNext(&it_, &e_) ||                         \
            !mlrIs(&e_.box, want_[n_ - 1])) {                                  \
            why = "RewindTail misread";                                        \
            break;                                                             \
        }                                                                      \
        if (!multilist##PFX##AdapterIteratorInitAtIdx(ml_, s, &it_, 2, true,   \
                                                      true) ||                 \
            !multilist##PFX##AdapterNext(&it_, &e_) ||                         \
            !mlrIs(&e_.box, want_[2])) {                                       \
            why = "IteratorInitAtIdx misread";                                 \
            break;                                                             \
        }                                                                      \
                                                                               \
        /* Duplicate and Dump */                                               \
        {                                                                      \
            cleaner(multilist##PFX##Free) multilist##PFX *dup_ =               \
                multilist##PFX##AdapterDuplicate(ml_);                         \
            cleaner(flexFree) flex *dump_ =                                    \
                multilist##PFX##AdapterDump(dup_, s0);                         \
            if (!mlaFlexIs(dump_, want_, n_)) {                                \
                why = "Duplicate or Dump differs";                             \
                break;                                                         \
            }                                                                  \
        }                                                                      \
                                                                               \
        /* Rotate, then Pop, PopFromTail and PopFromHead */                    \
        multilist##PFX##AdapterRotate(ml_, s);                                 \
        const uint32_t last_ = want_[n_ - 1];                                  \
        memmove(&want_[1], &want_[0], (n_ - 1) * sizeof(*want_));              \
        want_[0] = last_;                                                      \
        databox got_ = {{0}};                                                  \
        bool popped_ = multilist##PFX##AdapterPop(ml_, s0, &got_, true) &&     \
                       mlrIs(&got_, want_[n_ - 1]);                            \
        databoxFreeData(&got_);                                                \
        memset(&got_, 0, sizeof(got_));                                        \
        popped_ = popped_ && multilist##PFX##AdapterPopFromTail(ml_, &got_) && \
                  mlrIs(&got_, want_[n_ - 2]);                                 \
        databoxFreeData(&got_);                                                \
        memset(&got_, 0, sizeof(got_));                                        \
        popped_ = popped_ && multilist##PFX##AdapterPopFromHead(ml_, &got_) && \
                  mlrIs(&got_, want_[0]);                                      \
        databoxFreeData(&got_);                                                \
        n_ -= 2;                                                               \
        memmove(&want_[0], &want_[1], (n_ - 1) * sizeof(*want_));              \
        n_--;                                                                  \
        if (!popped_ || multilist##PFX##AdapterCount(ml_) != n_ ||             \
            multilist##PFX##AdapterBytes(ml_) == 0) {                          \
            why = "Rotate, Pop or Count went wrong";                           \
            break;                                                             \
        }                                                                      \
        {                                                                      \
            cleaner(flexFree) flex *dump_ =                                    \
                multilist##PFX##AdapterDump(ml_, s0);                          \
            if (!mlaFlexIs(dump_, want_, n_)) {                                \
                why = "the list differs after the pops";                       \
                break;                                                         \
            }                                                                  \
        }                                                                      \
                                                                               \
        /* The constructors: Create, NewFromFlex, NewFromFlexConsume */        \
        {                                                                      \
            const uint32_t w34_[2] = {3, 4};                                   \
            cleaner(multilist##PFX##Free) multilist##PFX *c_ =                 \
                multilist##PFX##AdapterCreate();                               \
            cleaner(multilist##PFX##Free) multilist##PFX *nf_ =                \
                multilist##PFX##AdapterNewFromFlex(f34_);                      \
            cleaner(multilist##PFX##Free) multilist##PFX *nc_ =                \
                multilist##PFX##AdapterNewFromFlexConsume(                     \
                    flexDuplicate(f34_));                                      \
            cleaner(flexFree) flex *d1_ =                                      \
                multilist##PFX##AdapterDump(nf_, s0);                          \
            cleaner(flexFree) flex *d2_ =                                      \
                multilist##PFX##AdapterDump(nc_, s0);                          \
            if (multilist##PFX##AdapterCount(c_) != 0 ||                       \
                !mlaFlexIs(d1_, w34_, 2) || !mlaFlexIs(d2_, w34_, 2)) {        \
                why = "Create, NewFromFlex or NewFromFlexConsume differs";     \
                break;                                                         \
            }                                                                  \
        }                                                                      \
    } while (0)

/* The flex an entry's element lives in, on each tier. */
static const flex *mloFlex(const multilist *ml, const multilistEntry *entry) {
    switch (_multilistType(ml)) {
    case MULTILIST_TYPE_SMALL:
        return mls(ml)->fl;
    case MULTILIST_TYPE_MEDIUM:
        return mlm(ml)->fl[entry->nodeIdx];
    default:
        return entry->f;
    }
}

/* Take 'steps' elements from 'iter', which must start at the head
 * (forward) or the tail (reverse) of ids 0..n-1.  When steps >= n the
 * walk must then be exhausted.  Returns NULL, or what went wrong. */
static const char *mlrWalk(multilistIterator *iter, uint32_t n, bool forward,
                           uint32_t steps) {
    multilistEntry entry;
    for (uint32_t k = 0; k < steps && k < n; k++) {
        if (!multilistNext(iter, &entry)) {
            return "walk ended early";
        }

        if (!mlrIs(&entry.box, forward ? k : n - 1 - k)) {
            return forward ? "wrong element walking forward"
                           : "wrong element walking in reverse";
        }
    }

    if (steps >= n && multilistNext(iter, &entry)) {
        return "walked past the end";
    }

    return NULL;
}

int multilistTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    uint32_t err = 0;
    cleaner(mflexStateFree) mflexState *s0 = mflexStateCreate();
    cleaner(mflexStateFree) mflexState *s1 = mflexStateCreate();
    mflexState *s[2] = {s0, s1};

    const int depth[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    const size_t depthCount = sizeof(depth) / sizeof(*depth);
    int64_t runtime[depthCount];
    const int defaultCompressSizeLimit = 1;

    for (size_t _i = 0; _i < depthCount; _i++) {
        printf("Testing Option %d\n", depth[_i]);
        int64_t start = timeUtilMs();

        TEST("create list") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            (void)ml;
        }

        TEST("add to tail of empty list") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            const databox pushBox = databoxNewBytesString("hello");
            multilistPushByTypeTail(&ml, s0, &pushBox);
            /* 1 for head and 1 for tail beacuse 1 node = head = tail */
        }

        TEST("add to head of empty list") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            const databox pushBox = databoxNewBytesString("hello");
            multilistPushByTypeHead(&ml, s0, &pushBox);
            /* 1 for head and 1 for tail beacuse 1 node = head = tail */
        }

        TEST("IndexNear: anchored lookup matches Index across all tiers") {
            /* Three configurations pinning each concrete tier: a large
             * fill limit keeps the list Small, a mid-size limit with
             * more data lands Medium, and limit 0 fragments a longer
             * list into many Full-tier nodes (exercising the
             * node-count skip from the anchor in both directions). */
            const struct {
                flexCapSizeLimit limit;
                int total;
                multilistType wantType;
            } configs[] = {
                {flexOptimizationSizeLimits - 1, 80, MULTILIST_TYPE_SMALL},
                /* ~60 small ints (~182 bytes) sit between limit (128)
                 * and the Medium->Full threshold (limit * 3 = 384) */
                {FLEX_CAP_LEVEL_128, 60, MULTILIST_TYPE_MEDIUM},
                {0, 500, MULTILIST_TYPE_FULL},
            };

            for (size_t c = 0; c < sizeof(configs) / sizeof(*configs); c++) {
                const int total = configs[c].total;
                cleaner(multilistFree) multilist *ml =
                    multilistNew(configs[c].limit, depth[_i]);
                for (int i = 0; i < total; i++) {
                    const databox pushBox = databoxNewSigned(i);
                    multilistPushByTypeTail(&ml, s0, &pushBox);
                }

                if (_multilistType(ml) != configs[c].wantType) {
                    /* Tier thresholds are size-based; skip silently
                     * only if the config no longer lands its tier —
                     * but loudly, because the test then lost tier
                     * coverage. */
                    ERR("config %zu landed tier %d, wanted %d!", c,
                        (int)_multilistType(ml), (int)configs[c].wantType);
                }

                const int anchors[] = {0, total / 4, total / 2, 3 * total / 4,
                                       total - 1};
                for (size_t ai = 0; ai < sizeof(anchors) / sizeof(*anchors);
                     ai++) {
                    multilistEntry anchor = {0};
                    assert(multilistIndexCheck(ml, s0, anchors[ai], &anchor));

                    for (int t = -total; t < total; t += 3) {
                        multilistEntry got = {0};
                        const bool foundNear = multilistIndexNearGet(
                            ml, s0, t, &anchor, anchors[ai], &got);
                        assert(foundNear);
                        const int64_t expect = t < 0 ? total + t : t;
                        if (got.box.data.i != expect) {
                            ERR("IndexNear(tier %d, anchor %d, target %d): "
                                "got %" PRId64 " expected %" PRId64 "!",
                                (int)_multilistType(ml), anchors[ai], t,
                                (int64_t)got.box.data.i, expect);
                            assert(NULL);
                        }
                    }

                    /* Out of range stays out of range */
                    multilistEntry miss = {0};
                    assert(!multilistIndexNearGet(ml, s0, total, &anchor,
                                                  anchors[ai], &miss));
                    assert(!multilistIndexNearGet(ml, s0, -total - 1, &anchor,
                                                  anchors[ai], &miss));
                }
            }
        }

        TEST("iterator rewind: head and tail restarts mid-iteration") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 40; i++) {
                const databox pushBox = databoxNewSigned(i);
                multilistPushByTypeTail(&ml, s0, &pushBox);
            }

            multilistIterator iter;
            multilistIteratorInitForward(ml, s, &iter);

            /* consume a few entries, then rewind mid-iteration */
            multilistEntry entry;
            for (int i = 0; i < 7; i++) {
                assert(multilistNext(&iter, &entry));
            }

            multilistRewind(ml, &iter);
            assert(multilistNext(&iter, &entry));
            assert(entry.box.data.i == 0);

            /* full pass after rewind sees every element in order */
            int64_t expect = 1;
            while (multilistNext(&iter, &entry)) {
                assert(entry.box.data.i == expect++);
            }
            assert(expect == 40);

            /* rewind to the tail flips direction from the same iter */
            multilistRewindTail(ml, &iter);
            assert(multilistNext(&iter, &entry));
            assert(entry.box.data.i == 39);
            assert(multilistNext(&iter, &entry));
            assert(entry.box.data.i == 38);

            multilistIteratorRelease(&iter);
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("add to tail 5x at fill %zu at compress %d", f,
                      depth[_i]) {
                cleaner(multilistFree) multilist *ml =
                    multilistNew(f, depth[_i]);
                for (int i = 0; i < 5; i++) {
                    databox pushBox = databoxNewBytesString(genstr("hello", i));
                    pushBox.len = 32;
                    multilistPushByTypeTail(&ml, s0, &pushBox);
                }

                if (multilistCount(ml) != 5) {
                    ERROR;
                }

                if (f == 32) {
                }
            }
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("add to head 5x at fill %zu at compress %d", f,
                      depth[_i]) {
                cleaner(multilistFree) multilist *ml =
                    multilistNew(f, depth[_i]);
                for (int i = 0; i < 5; i++) {
                    databox pushBox = databoxNewBytesString(genstr("hello", i));
                    pushBox.len = 32;
                    multilistPushByTypeHead(&ml, s0, &pushBox);
                }

                if (multilistCount(ml) != 5) {
                    ERROR;
                }

                if (f == 32) {
                }
            }
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("add to tail 500x at fill %zu at compress %d", f,
                      depth[_i]) {
                cleaner(multilistFree) multilist *ml =
                    multilistNew(f, depth[_i]);
                for (int i = 0; i < 500; i++) {
                    databox pushBox = databoxNewBytesString(genstr("hello", i));
                    pushBox.len = 64;
                    multilistPushByTypeTail(&ml, s0, &pushBox);
                }

                if (multilistCount(ml) != 500) {
                    ERROR;
                }

                if (f == 32) {
                }
            }
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("add to head 500x at fill %zu at compress %d", f,
                      depth[_i]) {
                cleaner(multilistFree) multilist *ml =
                    multilistNew(f, depth[_i]);
                for (int i = 0; i < 500; i++) {
                    multilistPushHead(ml, s0, genstr("hello", i), 32);
                }

                if (multilistCount(ml) != 500) {
                    ERROR;
                }

                if (f == 32) {
                }
            }
        }

        TEST("rotate empty") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            multilistRotate(ml, s);
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("rotate one val once at fill %zu", f) {
                cleaner(multilistFree) multilist *ml =
                    multilistNew(f, depth[_i]);
                multilistPushHead(ml, s0, "hello", 6);

                multilistRotate(ml, s);

                /* Ignore compression verify because flex is
                 * too small to compress. */
            }
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("rotate 504 val 5000 times at fill %zu at compress %d", f,
                      depth[_i]) {
                cleaner(multilistFree) multilist *ml =
                    multilistNew(f, depth[_i]);
                multilistRepr(ml);
                printf("============\n");
                multilistPushHead(ml, s0, "900", 3);
                multilistRepr(ml);
                printf("============\n");
                multilistPushHead(ml, s0, "7000", 4);
                multilistRepr(ml);
                printf("============\n");
                multilistPushHead(ml, s0, "-1200", 5);
                multilistRepr(ml);
                printf("============\n");
                multilistPushHead(ml, s0, "42", 2);
                for (int i = 0; i < 500; i++) {
                    multilistPushHead(ml, s0, genstr("hello", i), 64);
                }

                assert(multilistCount(ml) == 504);

                for (int i = 0; i < 5000; i++) {
                    assert(multilistCount(ml) == 504);
                    multilistRotate(ml, s);
                    assert(multilistCount(ml) == 504);
                }

                if (f == 1) {
                } else if (f == 2) {
                } else if (f == 32) {
                }
            }
        }

        TEST("pop empty") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            databox box = {{0}};
            bool found = multilistPopHead(&ml, s0, &box);
            assert(!found);
            (void)found;
        }

        TEST("pop 1 string from 1") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            char *populate = genstr("hello", 331);
            multilistPushHead(ml, s0, populate, 32);
            uint8_t *data;
            uint32_t bytes;
            databox box = {{0}};
            multilistPopHead(&ml, s0, &box);
            bytes = box.len;
            data = box.data.bytes.start;
            assert(data != NULL);
            assert(bytes == 32);
            if (strcmp(populate, (char *)data)) {
                ERR("Pop'd value (%.*s) didn't equal original value (%s)",
                    bytes, data, populate);
            }

            databoxFreeData(&box);
        }

        TEST("pop head 1 number from 1") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            /* An integer pops as that integer; a numeral string pops as the
             * same string (multilist stores exactly what it is given). */
            const databox num = databoxNewSigned(55513);
            multilistPushByTypeHead(&ml, s0, &num);
            multilistPushHead(ml, s0, "55513", 5);
            databox box = {{0}};
            multilistPopHead(&ml, s0, &box);
            if (!DATABOX_IS_BYTES(&box) || databoxLen(&box) != 5 ||
                memcmp(databoxBytes(&box), "55513", 5) != 0) {
                ERRR("numeral string did not pop as the same string");
            }
            databoxFreeData(&box);
            box = (databox){{0}};
            multilistPopHead(&ml, s0, &box);
            if (!DATABOX_IS_INTEGER(&box) || box.data.i != 55513) {
                ERRR("integer did not pop as 55513");
            }
            databoxFreeData(&box);
        }

        TEST("pop head 500 from 500") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 500; i++) {
                multilistPushHead(ml, s0, genstr("hello", i), 32);
            }

            for (int i = 0; i < 500; i++) {
                uint8_t *data;
                uint32_t bytes;
                databox box = {{0}};
                bool found = multilistPopHead(&ml, s0, &box);
                data = box.data.bytes.start;
                bytes = box.len;
                assert(found);
                (void)found;
                assert(data != NULL);
                assert(bytes == 32);
                if (strcmp(genstr("hello", 499 - i), (char *)data)) {
                    ERR("Pop'd value (%.*s) didn't equal original value (%s)",
                        bytes, data, genstr("hello", 499 - i));
                }
                databoxFreeData(&box);
            }
        }

        TEST("pop head 5000 from 500") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 500; i++) {
                multilistPushHead(ml, s0, genstr("hello", i), 32);
            }

            for (int i = 0; i < 5000; i++) {
                uint8_t *data;
                uint32_t bytes;
                databox box = {{0}};
                bool found = multilistPopHead(&ml, s0, &box);
                data = box.data.bytes.start;
                bytes = box.len;
                if (i < 500) {
                    assert(found);
                    assert(data);
                    assert(bytes == 32);
                    if (strcmp(genstr("hello", 499 - i), (char *)data)) {
                        ERR("Pop'd value (%.*s) didn't equal original value "
                            "(%s)",
                            bytes, data, genstr("hello", 499 - i));
                        assert(NULL);
                    }
                } else {
                    assert(!found);
                }
                (void)found;
                databoxFreeData(&box);
            }
        }

        TEST("iterate forward over 500 list") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 500; i++) {
                multilistPushHead(ml, s0, genstr("hello", i), 32);
            }

            multilistIterator iter = {0};
            multilistIteratorInitForwardReadOnly(ml, s, &iter);
            multilistEntry entry;
            int i = 499;
            int values = 0;
            while (multilistNext(&iter, &entry)) {
                char *h = genstr("hello", i);
                if (strcmp(entry.box.data.bytes.cstart, h)) {
                    ERR("value [%s] didn't match [%s] at position %d",
                        entry.box.data.bytes.start, h, i);
                }

                i--;
                values++;
            }

            if (values != 500) {
                ERR("Didn't iterate over exactly 500 elements (%d)", i);
            }
        }

        TEST("iterate reverse over 500 list") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 500; i++) {
                multilistPushHead(ml, s0, genstr("hello", i), 32);
            }

            multilistIterator iter = {0};
            multilistIteratorInitReverseReadOnly(ml, s, &iter);
            multilistEntry entry;
            int i = 0;
            while (multilistNext(&iter, &entry)) {
                char *h = genstr("hello", i);
                if (strcmp(entry.box.data.bytes.cstart, h)) {
                    ERR("value [%s] didn't match [%s] at position %d",
                        entry.box.data.bytes.start, h, i);
                }

                i++;
            }

            if (i != 500) {
                ERR("Didn't iterate over exactly 500 elements (%d)", i);
            }
        }

        TEST("insert before with 0 elements") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            multilistEntry entry;
            multilistIndexGet(ml, s0, 0, &entry);
            printf("WHAT? %p, %p, %d\n", entry.ml, entry.fe, entry.offset);
            multilistInsertBefore(&ml, s, &entry, "abc", 4);
        }

        TEST("insert after with 0 elements") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            multilistEntry entry;
            multilistIndexGet(ml, s0, 0, &entry);
            multilistInsertAfter(&ml, s, &entry, "abc", 4);
        }

        TEST("insert after 1 element") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            multilistPushHead(ml, s0, "hello", 6);
            multilistEntry entry;
            multilistIndexGet(ml, s0, 0, &entry);
            multilistInsertAfter(&ml, s, &entry, "abc", 4);
        }

        TEST("insert before 1 element") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            multilistPushHead(ml, s0, "hello", 6);
            multilistEntry entry;
            multilistIndexGet(ml, s0, 0, &entry);
            multilistInsertAfter(&ml, s, &entry, "abc", 4);
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("insert once in elements while iterating at fill %zu at "
                      "compress %d\n",
                      f, depth[_i]) {
                cleaner(multilistFree) multilist *ml =
                    multilistNew(f, depth[_i]);
                multilistPushTail(ml, s0, "abc", 3);
                multilistPushTail(ml, s0, "def", 3); /* unique node */

                multilistPushTail(ml, s0, "bob", 3); /* reset for +3 */
                multilistPushTail(ml, s0, "foo", 3);
                multilistPushTail(ml, s0, "zoo", 3);

                /* insert "bar" before "bob" while iterating over list. */
                multilistIterator iter = {0};
                multilistIteratorInitForwardReadOnly(ml, s, &iter);
                multilistEntry entry;
                while (multilistNext(&iter, &entry)) {
                    if (!strncmp(entry.box.data.bytes.cstart, "bob", 3)) {
                        /* Insert as fill = 1 so it spills into new node. */
                        multilistInsertBefore(&ml, s, &entry, "bar", 3);
                        /* note: we DO NOT support insert while iterating,
                         * meaning if you insert during an iteration, you
                         * must immediately exit the iteration.
                         *
                         * if you need more generic insert-while-iterating
                         * behavior, create a series of
                         * IteratorInsert{Before,After}Entry, etc. */
                        break;
                    }
                }

                multilistRepr(ml);

                /* verify results */
                bool got = multilistIndexGet(ml, s0, 0, &entry);
                assert(got);
                if (strncmp(entry.box.data.bytes.cstart, "abc", 3)) {
                    ERR("Value 0 didn't match, instead got: %.*s",
                        (int32_t)entry.box.len, entry.box.data.bytes.start);
                }

                got = multilistIndexGet(ml, s0, 1, &entry);
                assert(got);
                if (strncmp(entry.box.data.bytes.cstart, "def", 3)) {
                    ERR("Value 1 didn't match, instead got: %.*s",
                        (int32_t)entry.box.len, entry.box.data.bytes.start);
                    assert(NULL);
                }

                got = multilistIndexGet(ml, s0, 2, &entry);
                assert(got);
                if (strncmp(entry.box.data.bytes.cstart, "bar", 3)) {
                    ERR("Value 2 didn't match, instead got: %.*s",
                        (int32_t)entry.box.len, entry.box.data.bytes.start);
                }

                got = multilistIndexGet(ml, s0, 3, &entry);
                assert(got);
                if (strncmp(entry.box.data.bytes.cstart, "bob", 3)) {
                    ERR("Value 3 didn't match, instead got: %.*s",
                        (int32_t)entry.box.len, entry.box.data.bytes.start);
                }

                got = multilistIndexGet(ml, s0, 4, &entry);
                assert(got);
                if (strncmp(entry.box.data.bytes.cstart, "foo", 3)) {
                    ERR("Value 4 didn't match, instead got: %.*s",
                        (int32_t)entry.box.len, entry.box.data.bytes.start);
                }

                got = multilistIndexGet(ml, s0, 5, &entry);
                assert(got);
                if (strncmp(entry.box.data.bytes.cstart, "zoo", 3)) {
                    ERR("Value 5 didn't match, instead got: %.*s",
                        (int32_t)entry.box.len, entry.box.data.bytes.start);
                }
                (void)got;
            }
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC(
                "insert [before] 250 new in middle of 500 elements at fill"
                " %zu at compress %d",
                f, depth[_i]) {
                cleaner(multilistFree) multilist *ml =
                    multilistNew(f, depth[_i]);
                for (int i = 0; i < 500; i++) {
                    multilistPushTail(ml, s0, genstr("hello", i), 32);
                }

                for (int i = 0; i < 250; i++) {
                    multilistEntry entry;
                    multilistIndexGet(ml, s0, 250, &entry);
                    multilistInsertBefore(&ml, s, &entry, genstr("abc", i), 32);
                }

                if (f == 32) {
                }
            }
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("insert [after] 250 new in middle of 500 elements at "
                      "fill %zu at compress %d",
                      f, depth[_i]) {
                cleaner(multilistFree) multilist *ml =
                    multilistNew(f, depth[_i]);
                for (int i = 0; i < 500; i++) {
                    multilistPushHead(ml, s0, genstr("hello", i), 32);
                }

                for (int i = 0; i < 250; i++) {
                    multilistEntry entry;
                    multilistIndexGet(ml, s0, 250, &entry);
                    multilistInsertAfter(&ml, s, &entry, genstr("abc", i), 32);
                }

                if (multilistCount(ml) != 750) {
                    ERR("List size not 750, but rather %zu",
                        multilistCount(ml));
                }

                if (f == 32) {
                }
            }
        }

        TEST("duplicate empty list") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            cleaner(multilistFree) multilist *copy = multilistDuplicate(ml);
            (void)copy;
        }

        TEST("duplicate list of 1 element") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            multilistPushHead(ml, s0, genstr("hello", 3), 32);
            cleaner(multilistFree) multilist *copy = multilistDuplicate(ml);
            (void)copy;
        }

        TEST("duplicate list of 500") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 500; i++) {
                multilistPushHead(ml, s0, genstr("hello", i), 32);
            }

            cleaner(multilistFree) multilist *copy = multilistDuplicate(ml);
            (void)copy;
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("index 1,200 from 500 list at fill %zu at compress %d", f,
                      depth[_i]) {
                cleaner(multilistFree) multilist *ml =
                    multilistNew(f, depth[_i]);
                for (int i = 0; i < 500; i++) {
                    multilistPushTail(ml, s0, genstr("hello", i + 1), 32);
                }

                multilistEntry entry;
                multilistIndexGet(ml, s0, 1, &entry);
                if (!strcmp(entry.box.data.bytes.cstart, "hello2")) {
                    OK;
                } else {
                    ERR("Value: %s", entry.box.data.bytes.start);
                }
                multilistIndexGet(ml, s0, 200, &entry);
                if (!strcmp(entry.box.data.bytes.cstart, "hello201")) {
                    OK;
                } else {
                    ERR("Value: %s", entry.box.data.bytes.start);
                }
            }

            TEST_DESC("index -1,-2 from 500 list at fill %zu at compress %d", f,
                      depth[_i]) {
                cleaner(multilistFree) multilist *ml =
                    multilistNew(f, depth[_i]);
                for (int i = 0; i < 500; i++) {
                    multilistPushTail(ml, s0, genstr("hello", i + 1), 32);
                }

                multilistEntry entry;
                multilistIndexGet(ml, s0, -1, &entry);
                if (!strcmp(entry.box.data.bytes.cstart, "hello500")) {
                    OK;
                } else {
                    ERR("Value: %s", entry.box.data.bytes.start);
                }

                multilistIndexGet(ml, s0, -2, &entry);
                if (!strcmp(entry.box.data.bytes.cstart, "hello499")) {
                    OK;
                } else {
                    ERR("Value: %s", entry.box.data.bytes.start);
                }
            }

            TEST_DESC("index -100 from 500 list at fill %zu at compress %d", f,
                      depth[_i]) {
                cleaner(multilistFree) multilist *ml =
                    multilistNew(f, depth[_i]);
                for (int i = 0; i < 500; i++) {
                    multilistPushTail(ml, s0, genstr("hello", i + 1), 32);
                }

                multilistEntry entry;
                multilistIndexGet(ml, s0, -100, &entry);
                if (!strcmp(entry.box.data.bytes.cstart, "hello401")) {
                    OK;
                } else {
                    ERR("Value: %s", entry.box.data.bytes.start);
                }
            }

            TEST_DESC(
                "index too big +1 from 50 list at fill %zu at compress %d", f,
                depth[_i]) {
                cleaner(multilistFree) multilist *ml =
                    multilistNew(f, depth[_i]);
                for (int i = 0; i < 50; i++) {
                    multilistPushTail(ml, s0, genstr("hello", i + 1), 32);
                }

                multilistEntry entry;
                if (multilistIndexCheck(ml, s0, 50, &entry)) {
                    ERR("Index found at 50 with 50 list: %.*s",
                        (int32_t)entry.box.len, entry.box.data.bytes.start);
                } else {
                    OK;
                }
            }
        }

        TEST("delete range empty list") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            multilistDelRange(&ml, s0, 5, 20);
        }

        TEST("delete range of entire node in list of one node") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 32; i++) {
                multilistPushHead(ml, s0, genstr("hello", i), 32);
            }

            multilistDelRange(&ml, s0, 0, 32);
        }

        TEST("delete range of entire node with overflow valuess") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 32; i++) {
                multilistPushHead(ml, s0, genstr("hello", i), 32);
            }

            multilistDelRange(&ml, s0, 0, 128);
        }

        TEST("delete middle 100 of 500 list") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 500; i++) {
                multilistPushTail(ml, s0, genstr("hello", i + 1), 32);
            }

            multilistDelRange(&ml, s0, 200, 100);
        }

        TEST("delete negative 1 from 500 list") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 500; i++) {
                multilistPushTail(ml, s0, genstr("hello", i + 1), 32);
            }

            multilistDelRange(&ml, s0, -1, 1);
        }

        TEST("delete negative 1 from 500 list with overflow valuess") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 500; i++) {
                multilistPushTail(ml, s0, genstr("hello", i + 1), 32);
            }

            multilistDelRange(&ml, s0, -1, 128);
        }

        TEST("delete negative 100 from 500 list") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 500; i++) {
                multilistPushTail(ml, s0, genstr("hello", i + 1), 32);
            }

            multilistDelRange(&ml, s0, -100, 100);
        }

        TEST("delete -10 values 5 from 50 list") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 50; i++) {
                multilistPushTail(ml, s0, genstr("hello", i + 1), 32);
            }

            multilistDelRange(&ml, s0, -10, 5);
        }

        TEST("numbers only list read") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            for (int64_t v = 1111; v <= 4444; v += 1111) {
                const databox num = databoxNewSigned(v);
                multilistPushByTypeTail(&ml, s0, &num);
            }
            multilistEntry entry;
            multilistIndexGet(ml, s0, 0, &entry);
            if (entry.box.data.i64 != 1111) {
                ERR("Not 1111, %" PRIi64 "", entry.box.data.i64);
            }

            multilistIndexGet(ml, s0, 1, &entry);
            if (entry.box.data.i64 != 2222) {
                ERR("Not 2222, %" PRIi64 "", entry.box.data.i64);
            }

            multilistIndexGet(ml, s0, 2, &entry);
            if (entry.box.data.i64 != 3333) {
                ERR("Not 3333, %" PRIi64 "", entry.box.data.i64);
            }

            multilistIndexGet(ml, s0, 3, &entry);
            if (entry.box.data.i64 != 4444) {
                ERR("Not 4444, %" PRIi64 "", entry.box.data.i64);
            }

            if (multilistIndexGet(ml, s0, 4, &entry)) {
                ERR("Index past elements: %" PRIi64 "", entry.box.data.i64);
            }

            multilistIndexGet(ml, s0, -1, &entry);
            if (entry.box.data.i64 != 4444) {
                ERR("Not 4444 (reverse), %" PRIi64 "", entry.box.data.i64);
            }

            multilistIndexGet(ml, s0, -2, &entry);
            if (entry.box.data.i64 != 3333) {
                ERR("Not 3333 (reverse), %" PRIi64 "", entry.box.data.i64);
            }

            multilistIndexGet(ml, s0, -3, &entry);
            if (entry.box.data.i64 != 2222) {
                ERR("Not 2222 (reverse), %" PRIi64 "", entry.box.data.i64);
            }

            multilistIndexGet(ml, s0, -4, &entry);
            if (entry.box.data.i64 != 1111) {
                ERR("Not 1111 (reverse), %" PRIi64 "", entry.box.data.i64);
            }

            if (multilistIndexGet(ml, s0, -5, &entry)) {
                ERR("Index past elements (reverse), %" PRIi64 "",
                    entry.box.data.i64);
            }
        }

        TEST("numbers larger list read") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            int64_t nums[5000];
            for (int i = 0; i < 5000; i++) {
                nums[i] = -5157318210846258176 + i;
                const databox pushBox = databoxNewSigned(nums[i]);
                multilistPushByTypeTail(&ml, s0, &pushBox);
            }

            multilistPushTail(ml, s0, "xxxxxxxxxxxxxxxxxxxx", 20);
            multilistEntry entry;
            for (int i = 0; i < 5000; i++) {
                multilistIndexGet(ml, s0, i, &entry);
                if (entry.box.data.i64 != nums[i]) {
                    ERR("[%d] Not longval %" PRIi64 " but rather %" PRIi64 "",
                        i, nums[i], entry.box.data.i64);
                }

                entry.box.data.i64 = 0xdeadbeef;
            }

            multilistIndexGet(ml, s0, 5000, &entry);
            if (strncmp(entry.box.data.bytes.cstart, "xxxxxxxxxxxxxxxxxxxx",
                        20)) {
                ERR("String val not match: %s", entry.box.data.bytes.start);
            }
        }

        TEST("numbers larger list read B") {
            cleaner(multilistFree) multilist *ml =
                multilistNew(defaultCompressSizeLimit, depth[_i]);
            multilistPushTail(ml, s0, "99", 2);
            multilistPushTail(ml, s0, "98", 2);
            multilistPushTail(ml, s0, "xxxxxxxxxxxxxxxxxxxx", 20);
            multilistPushTail(ml, s0, "96", 2);
            multilistPushTail(ml, s0, "95", 2);
            multilistReplaceAtIndex(&ml, s0, 1, "foo", 3);
            multilistReplaceAtIndex(&ml, s0, -1, "bar", 3);
            OK;
        }

        mflexStateReset(s0);
        mflexStateReset(s1);

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("lrem test at fill %zu at compress %d", f, depth[_i]) {
                cleaner(multilistFree) multilist *ml =
                    multilistNew(f, depth[_i]);
                char *words[] = {"abc", "foo", "bar",  "foobar", "foobared",
                                 "zap", "bar", "test", "foo"};
                char *result[] = {"abc", "foo",  "foobar", "foobared",
                                  "zap", "test", "foo"};
                char *resultB[] = {"abc",      "foo", "foobar",
                                   "foobared", "zap", "test"};
                for (int i = 0; i < 9; i++) {
                    multilistPushTail(ml, s0, words[i], strlen(words[i]));
                }

                multilistRepr(ml);

                /* lrem 0 bar */
                multilistIterator iter = {0};
                multilistIteratorInitForward(ml, s, &iter);
                multilistEntry entry;
                int i = 0;
                while (multilistNext(&iter, &entry)) {
                    if (flexCompareBytes(entry.fe, "bar", 3)) {
                        multilistDelEntry(&iter, &entry);
                    }

                    i++;
                }

                /* check result of lrem 0 bar */
                multilistIteratorInitForwardReadOnly(ml, s, &iter);
                multilistRepr(ml);
                i = 0;
                int ok = 1;
                while (multilistNext(&iter, &entry)) {
                    /* Result must be: abc, foo, foobar, foobared, zap, test,
                     * foo */
                    if (strncmp(entry.box.data.bytes.cstart, result[i],
                                (int32_t)entry.box.len)) {
                        ERR("No match at position %d, got %.*s instead of %s",
                            i, (int32_t)entry.box.len,
                            entry.box.data.bytes.start, result[i]);
                        ok = 0;
                    }

                    i++;
                }

                multilistPushTail(ml, s0, "foo", 3);

                /* lrem -2 foo */
                multilistIteratorInitReverse(ml, s, &iter);
                multilistRepr(ml);
                i = 0;
                int del = 2;
                while (multilistNext(&iter, &entry)) {
                    if (flexCompareBytes(entry.fe, "foo", 3)) {
                        multilistDelEntry(&iter, &entry);
                        del--;
                    }

                    if (!del) {
                        break;
                    }

                    i++;
                }

                multilistIteratorRelease(&iter);

                /* check result of lrem -2 foo */
                /* (we're ignoring the '2' part and still deleting all foo
                 * because we only have two foo) */
                multilistIteratorInitReverseReadOnly(ml, s, &iter);
                multilistRepr(ml);
                i = 0;
                const size_t resB = sizeof(resultB) / sizeof(*resultB);
                while (multilistNext(&iter, &entry)) {
                    /* Result must be: abc, foo, foobar, foobared, zap, test,
                     * foo */
                    if (strncmp(entry.box.data.bytes.cstart,
                                resultB[resB - 1 - i],
                                (int32_t)entry.box.len)) {
                        ERR("No match at position %d, got %.*s instead of %s",
                            i, (int32_t)entry.box.len,
                            entry.box.data.bytes.start, resultB[resB - 1 - i]);
                        ok = 0;
                    }

                    i++;
                }

                /* final result of all tests */
                if (ok) {
                    OK;
                }
            }
        }

        mflexStateReset(s0);
        mflexStateReset(s1);

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("iterate reverse + delete at fill %zu at compress %d", f,
                      depth[_i]) {
                cleaner(multilistFree) multilist *ml =
                    multilistNew(f, depth[_i]);
                multilistPushTail(ml, s0, "abc", 3);
                multilistPushTail(ml, s0, "def", 3);
                multilistPushTail(ml, s0, "hij", 3);
                multilistPushTail(ml, s0, "jkl", 3);
                multilistPushTail(ml, s0, "oop", 3);

                multilistEntry entry;
                multilistIterator iter = {0};
                multilistIteratorInitReverse(ml, s, &iter);
                int i = 0;
                while (multilistNext(&iter, &entry)) {
                    printf("Entry fe: %p (%.*s)\n", entry.fe, 3, entry.fe + 1);
                    if (flexCompareBytes(entry.fe, "hij", 3)) {
                        multilistDelEntry(&iter, &entry);
                    }

                    i++;
                }

                if (i != 5) {
                    ERR("Didn't iterate 5 times, iterated %d times.", i);
                    multilistRepr(ml);
                }

                /* Check results after deletion of "hij" */
                multilistIteratorInitForward(ml, s, &iter);
                i = 0;
                char *vals[] = {"abc", "def", "jkl", "oop"};
                while (multilistNext(&iter, &entry)) {
                    if (!flexCompareBytes(entry.fe, vals[i], 3)) {
                        ERR("Value at %d didn't match %s\n", i, vals[i]);
                    }

                    i++;
                }
            }
        }

        mflexStateReset(s0);
        mflexStateReset(s1);

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("iterator at index test at fill %zu at compress %d", f,
                      depth[_i]) {
                cleaner(multilistFree) multilist *ml =
                    multilistNew(f, depth[_i]);
                int64_t nums[5000];
                for (int i = 0; i < 760; i++) {
                    nums[i] = -5157318210846258176 + i;
                    const databox pushBox = databoxNewSigned(nums[i]);
                    multilistPushByTypeTail(&ml, s0, &pushBox);
                }

                multilistEntry entry;
                multilistIterator iter = {0};
                multilistIteratorInitAtIdxForwardReadOnly(ml, s, &iter, 437);
                int i = 437;
                while (multilistNext(&iter, &entry)) {
                    if (entry.box.data.i64 != nums[i]) {
                        ERR("Expected %" PRIi64 ", but got %" PRIi64 "",
                            entry.box.data.i64, nums[i]);
                    }

                    i++;
                }
            }
        }

        mflexStateReset(s0);
        mflexStateReset(s1);

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("ltrim test A at fill %zu at compress %d", f, depth[_i]) {
                cleaner(multilistFree) multilist *ml =
                    multilistNew(f, depth[_i]);
                int64_t nums[5000];
                for (int i = 0; i < 32; i++) {
                    nums[i] = -5157318210846258176 + i;
                    const databox pushBox = DATABOX_SIGNED(nums[i]);
                    multilistPushByTypeTail(&ml, s0, &pushBox);
                }

                if (f == 32) {
                }

                /* ltrim 25 53 (keep [25,32] inclusive = 7 remaining) */
                multilistDelRange(&ml, s0, 0, 25);
                multilistDelRange(&ml, s0, 0, 0);
                multilistEntry entry;
                for (int i = 0; i < 7; i++) {
                    multilistIndexGet(ml, s0, i, &entry);
                    if (entry.box.data.i64 != nums[25 + i]) {
                        ERR("Deleted invalid range!  Expected %" PRIi64
                            " but got "
                            "%" PRIi64 "",
                            entry.box.data.i64, nums[25 + i]);
                    }
                }
                if (f == 32) {
                }
            }
        }

        mflexStateReset(s0);
        mflexStateReset(s1);

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("ltrim test B at fill %zu at compress %d", f, depth[_i]) {
                /* Force-disable compression because our 33 sequential
                 * integers don't compress and the check always fails. */
                cleaner(multilistFree) multilist *ml = multilistNew(f, 0);
                int64_t nums[5000];
                for (int i = 0; i < 33; i++) {
                    nums[i] = i;
                    const databox numBox = databoxNewSigned(nums[i]);
                    multilistPushByTypeTail(&ml, s0, &numBox);
                }

                if (f == 32) {
                }

                /* ltrim 5 16 (keep [5,16] inclusive = 12 remaining) */
                multilistDelRange(&ml, s0, 0, 5);
                multilistDelRange(&ml, s0, -16, 16);
                if (f == 32) {
                }

                multilistEntry entry;
                multilistIndexGet(ml, s0, 0, &entry);
                if (entry.box.data.i64 != 5) {
                    ERR("A: longval not 5, but %" PRIi64 "",
                        entry.box.data.i64);
                } else {
                    OK;
                }
                multilistIndexGet(ml, s0, -1, &entry);
                if (entry.box.data.i64 != 16) {
                    ERR("B! got instead: %" PRIi64 "", entry.box.data.i64);
                } else {
                    OK;
                }
                multilistPushTail(ml, s0, "bobobob", 7);
                multilistIndexGet(ml, s0, -1, &entry);
                if (strncmp(entry.box.data.bytes.cstart, "bobobob", 7)) {
                    ERR("Tail doesn't match bobobob, it's %.*s instead",
                        (int32_t)entry.box.len, entry.box.data.bytes.start);
                }

                for (int i = 0; i < 12; i++) {
                    multilistIndexGet(ml, s0, i, &entry);
                    if (entry.box.data.i64 != nums[5 + i]) {
                        ERR("Deleted invalid range!  Expected %" PRIi64
                            " but got "
                            "%" PRIi64 "",
                            entry.box.data.i64, nums[5 + i]);
                    }
                }
            }
        }

        mflexStateReset(s0);
        mflexStateReset(s1);

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("ltrim test C at fill %zu at compress %d", f, depth[_i]) {
                cleaner(multilistFree) multilist *ml =
                    multilistNew(f, depth[_i]);
                int64_t nums[5000];
                for (int i = 0; i < 33; i++) {
                    nums[i] = -5157318210846258176 + i;
                    const databox pushBox = DATABOX_SIGNED(nums[i]);
                    multilistPushByTypeTail(&ml, s0, &pushBox);
                }

                if (f == 32) {
                }

                /* ltrim 3 3 (keep [3,3] inclusive = 1 remaining) */
                multilistDelRange(&ml, s0, 0, 3);
                multilistDelRange(&ml, s0, -29,
                                  4000); /* make sure not loop forever */
                if (f == 32) {
                }

                multilistEntry entry;
                multilistIndexGet(ml, s0, 0, &entry);
                if (entry.box.data.i64 != -5157318210846258173) {
                    ERROR;
                } else {
                    OK;
                }
            }
        }

        mflexStateReset(s0);
        mflexStateReset(s1);

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("ltrim test D at fill %zu at compress %d", f, depth[_i]) {
                cleaner(multilistFree) multilist *ml =
                    multilistNew(f, depth[_i]);
                int64_t nums[5000];
                for (int i = 0; i < 33; i++) {
                    nums[i] = -5157318210846258176 + i;
                    const databox numBox = databoxNewSigned(nums[i]);
                    multilistPushByTypeTail(&ml, s0, &numBox);
                }

                if (f == 32) {
                }

                multilistDelRange(&ml, s0, -12, 3);
                if (multilistCount(ml) != 30) {
                    ERR("Didn't delete exactly three elements!  values is: %zu",
                        multilistCount(ml));
                }
            }
        }

        mflexStateReset(s0);
        mflexStateReset(s1);

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("create multilist from flex at fill %zu at compress %d",
                      f, depth[_i]) {
                /* @own: container; free=multilistFree */
                flex *fl = flexNew();
                int64_t nums[64];
                char num[64];
                for (int i = 0; i < 33; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int bytes = StrInt64ToBuf(num, sizeof(num), nums[i]);
                    flexPushBytes(&fl, num, bytes, FLEX_ENDPOINT_TAIL);
                }

                for (int i = 0; i < 33; i++) {
                    flexPushBytes(&fl, genstr("hello", i), 32,
                                  FLEX_ENDPOINT_TAIL);
                }

                cleaner(multilistFree) multilist *ml =
                    multilistNewFromFlex(f, depth[_i], fl);
                (void)ml;

                if (f == 1) {
                } else if (f == 32) {
                } else if (f == 66) {
                }
            }
        }

        int64_t stop = timeUtilMs();
        runtime[_i] = stop - start;
    }

    mflexStateReset(s0);
    mflexStateReset(s1);

    /* Compression depth against list length, at every fill: lists from a
     * few nodes to thousands, depths from 1 to past the node count.  Every
     * element is read back, so every compressed interior node is
     * decompressed and checked. */
    const int listSizes[] = {30, 251, 1000, 5000};
    const size_t depths[] = {1, 2, 3, 5, 8, 13, 21, 39};
    int64_t start = timeUtilMs();
    for (size_t list = 0; list < (sizeof(listSizes) / sizeof(*listSizes));
         list++) {
        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            for (size_t d = 0; d < sizeof(depths) / sizeof(*depths); d++) {
                const size_t _depth = depths[d];
                const int n = listSizes[list];
                TEST_DESC("verify specific compression of interior nodes with "
                          "%d list "
                          "at fill %zu at compress depth %zu",
                          n, f, _depth) {
                    cleaner(multilistFree) multilist *ml =
                        multilistNew(f, _depth);
                    for (int i = 0; i < n; i++) {
                        multilistPushTail(ml, s0, genstr("hello TAIL", i + 1),
                                          64);
                        multilistPushHead(ml, s0, genstr("hello HEAD", i + 1),
                                          64);
                    }

                    /* HEAD n .. HEAD 1, then TAIL 1 .. TAIL n */
                    multilistIterator iter;
                    multilistEntry entry;
                    multilistIteratorInitReadOnly(ml, s, &iter, true);
                    int seen = 0;
                    while (multilistNext(&iter, &entry)) {
                        const char *want =
                            seen < n ? genstr("hello HEAD", n - seen)
                                     : genstr("hello TAIL", seen - n + 1);
                        if (entry.box.len != 64 ||
                            memcmp(entry.box.data.bytes.start, want, 64) != 0) {
                            ERR("list %d fill %zu depth %zu: element %d is "
                                "not %s",
                                n, f, _depth, seen, want);
                            break;
                        }
                        seen++;
                    }

                    multilistIteratorRelease(&iter);
                    if (seen != 2 * n || multilistCount(ml) != (size_t)2 * n) {
                        ERR("list %d fill %zu depth %zu: read %d of %d", n, f,
                            _depth, seen, 2 * n);
                    }
                }
            }
        }
    }
    int64_t stop = timeUtilMs();

    printf("\n");
    for (size_t i = 0; i < depthCount; i++) {
        fprintf(stderr, "Compress Depth %02d: %0.3f seconds.\n", depth[i],
                (float)runtime[i] / 1000);
    }

    fprintf(stderr, "Final Stress Loop: %0.2f seconds.\n",
            (float)(stop - start) / 1000);
    printf("\n");

    /* ================================================================
     * COMPREHENSIVE FUZZ TESTS
     * ================================================================ */

    printf("\n=== MULTILIST FUZZ TESTING ===\n\n");

    TEST("FUZZ: push/pop operations with oracle verification") {
        cleaner(multilistFree) multilist *ml = multilistNew(4, 2);
        const size_t maxSize = 1000;
        cleanerName(zfree, I64) int64_t *oracle =
            zcalloc(maxSize, sizeof(int64_t));
        size_t oracleCount = 0;

        srand(12345);
        size_t pushOps = 0, popOps = 0;

        for (size_t round = 0; round < 5000; round++) {
            int op = rand() % 10;

            if (op < 6 && oracleCount < maxSize) {
                /* Push (60%) */
                int64_t val = (int64_t)(rand() % 100000) - 50000;
                int where = rand() % 2;
                databox box = databoxNewSigned(val);

                if (where == 0) {
                    /* Push head */
                    multilistPushByTypeHead(&ml, s0, &box);
                    memmove(&oracle[1], &oracle[0],
                            oracleCount * sizeof(int64_t));
                    oracle[0] = val;
                } else {
                    /* Push tail */
                    multilistPushByTypeTail(&ml, s0, &box);
                    oracle[oracleCount] = val;
                }
                oracleCount++;
                pushOps++;
            } else if (oracleCount > 0) {
                /* Pop (40%) */
                int where = rand() % 2;
                databox got = {{0}};

                if (where == 0) {
                    /* Pop head */
                    multilistPopHead(&ml, s0, &got);
                    int64_t expected = oracle[0];
                    int64_t actual = (got.type == DATABOX_SIGNED_64)
                                         ? got.data.i
                                         : (int64_t)got.data.u;
                    if (actual != expected) {
                        ERR("head mismatch: got %" PRId64 " expected %" PRId64,
                            actual, expected);
                    }
                    memmove(&oracle[0], &oracle[1],
                            (oracleCount - 1) * sizeof(int64_t));
                } else {
                    /* Pop tail */
                    multilistPopTail(&ml, s0, &got);
                    int64_t expected = oracle[oracleCount - 1];
                    int64_t actual = (got.type == DATABOX_SIGNED_64)
                                         ? got.data.i
                                         : (int64_t)got.data.u;
                    if (actual != expected) {
                        ERR("tail mismatch: got %" PRId64 " expected %" PRId64,
                            actual, expected);
                    }
                }
                databoxFreeData(&got);
                oracleCount--;
                popOps++;
            }

            /* Periodic verification */
            if (round % 500 == 0) {
                if (multilistCount(ml) != oracleCount) {
                    ERR("count mismatch at round %zu: ml=%zu oracle=%zu", round,
                        multilistCount(ml), oracleCount);
                }
            }
        }

        /* Final verification */
        if (multilistCount(ml) != oracleCount) {
            ERR("final count mismatch: ml=%zu oracle=%zu", multilistCount(ml),
                oracleCount);
        }

        printf("  push=%zu pop=%zu final=%zu\n", pushOps, popOps, oracleCount);
    }

    TEST("FUZZ: index access verification") {
        cleaner(multilistFree) multilist *ml = multilistNew(4, 2);
        const size_t count = 500;

        /* Populate list */
        for (size_t i = 0; i < count; i++) {
            databox box = databoxNewSigned((int64_t)i);
            multilistPushByTypeTail(&ml, s0, &box);
        }

        /* Verify positive index access */
        for (size_t i = 0; i < count; i++) {
            multilistEntry entry;
            if (!multilistIndexGet(ml, s0, i, &entry)) {
                ERR("index %zu failed", i);
            }
            int64_t val = (entry.box.type == DATABOX_SIGNED_64)
                              ? entry.box.data.i
                              : (int64_t)entry.box.data.u;
            if ((size_t)val != i) {
                ERR("index %zu returned %" PRId64, i, val);
            }
        }

        /* Verify negative index access */
        for (size_t i = 0; i < count; i++) {
            multilistEntry entry;
            int64_t negIdx = -(int64_t)(i + 1);
            if (!multilistIndexGet(ml, s0, negIdx, &entry)) {
                ERR("negative index %" PRId64 " failed", negIdx);
            }
            int64_t val = (entry.box.type == DATABOX_SIGNED_64)
                              ? entry.box.data.i
                              : (int64_t)entry.box.data.u;
            int64_t expected = (int64_t)(count - 1 - i);
            if (val != expected) {
                ERR("negative index %" PRId64 " returned %" PRId64
                    ", expected %" PRId64,
                    negIdx, val, expected);
            }
        }

        printf("  verified %zu positive and %zu negative indices\n", count,
               count);
    }

    TEST("FUZZ: iterator forward/backward consistency") {
        cleaner(multilistFree) multilist *ml = multilistNew(4, 2);
        const size_t count = 200;

        /* Populate list */
        for (size_t i = 0; i < count; i++) {
            databox box = databoxNewSigned((int64_t)i);
            multilistPushByTypeTail(&ml, s0, &box);
        }

        /* Forward iteration */
        multilistIterator iter;
        multilistIteratorInitForwardReadOnly(ml, s, &iter);
        multilistEntry entry;
        size_t idx = 0;
        while (multilistNext(&iter, &entry)) {
            int64_t val = (entry.box.type == DATABOX_SIGNED_64)
                              ? entry.box.data.i
                              : (int64_t)entry.box.data.u;
            if ((size_t)val != idx) {
                ERR("forward iter at %zu: got %" PRId64, idx, val);
            }
            idx++;
        }
        multilistIteratorRelease(&iter);

        if (idx != count) {
            ERR("forward iteration count: got %zu expected %zu", idx, count);
        }

        /* Backward iteration */
        multilistIteratorInitReverseReadOnly(ml, s, &iter);
        idx = 0;
        while (multilistNext(&iter, &entry)) {
            int64_t val = (entry.box.type == DATABOX_SIGNED_64)
                              ? entry.box.data.i
                              : (int64_t)entry.box.data.u;
            int64_t expected = (int64_t)(count - 1 - idx);
            if (val != expected) {
                ERR("backward iter at %zu: got %" PRId64 " expected %" PRId64,
                    idx, val, expected);
            }
            idx++;
        }
        multilistIteratorRelease(&iter);

        if (idx != count) {
            ERR("backward iteration count: got %zu expected %zu", idx, count);
        }

        printf("  verified forward and backward iteration of %zu elements\n",
               count);
    }

    TEST("FUZZ: mixed types in list") {
        cleaner(multilistFree) multilist *ml = multilistNew(4, 2);
        const size_t count = 300;

        srand(77777);
        for (size_t i = 0; i < count; i++) {
            int type = rand() % 4;
            databox box;
            char buf[64]; /* Declare outside switch to keep in scope */
            switch (type) {
            case 0:
                box = databoxNewSigned((int64_t)(rand() % 100000) - 50000);
                break;
            case 1:
                box = databoxNewUnsigned(rand() % 100000);
                break;
            case 2:
                snprintf(buf, sizeof(buf), "str_%d", rand());
                box = databoxNewBytesString(buf);
                break;
            case 3:
                box.type = DATABOX_DOUBLE_64;
                box.data.d64 = (double)(rand() % 10000) / 100.0;
                break;
            }
            multilistPushByTypeTail(&ml, s0, &box);
        }

        if (multilistCount(ml) != count) {
            ERR("mixed type count: got %zu expected %zu", multilistCount(ml),
                count);
        }

        /* Verify we can iterate all elements */
        multilistIterator iter;
        multilistIteratorInitForwardReadOnly(ml, s, &iter);
        multilistEntry entry;
        size_t iterCount = 0;
        while (multilistNext(&iter, &entry)) {
            iterCount++;
        }
        multilistIteratorRelease(&iter);

        if (iterCount != count) {
            ERR("mixed type iter count: got %zu expected %zu", iterCount,
                count);
        }

        printf("  verified %zu mixed-type elements\n", count);
    }

    TEST("FUZZ: tier transitions - small to medium to full") {
        /* Test that lists correctly transition between tiers */
        for (size_t fill = 1; fill <= 8; fill++) {
            cleaner(multilistFree) multilist *ml = multilistNew(fill, 0);

            /* Keep pushing until we force tier transitions */
            const size_t targetCount = 1000;
            for (size_t i = 0; i < targetCount; i++) {
                databox box = databoxNewSigned((int64_t)i);
                multilistPushByTypeTail(&ml, s0, &box);

                /* Periodically verify count */
                if ((i + 1) % 100 == 0) {
                    if (multilistCount(ml) != i + 1) {
                        ERR("fill %zu at i=%zu: count=%zu expected=%zu", fill,
                            i, multilistCount(ml), i + 1);
                    }
                }
            }

            /* Verify final state */
            if (multilistCount(ml) != targetCount) {
                ERR("fill %zu final: count=%zu expected=%zu", fill,
                    multilistCount(ml), targetCount);
            }

            /* Pop half and verify */
            for (size_t i = 0; i < targetCount / 2; i++) {
                databox got = {{0}};
                if (!multilistPopTail(&ml, s0, &got)) {
                    ERR("fill %zu: pop failed at %zu", fill, i);
                }
                databoxFreeData(&got);
            }

            if (multilistCount(ml) != targetCount / 2) {
                ERR("fill %zu after pops: count=%zu expected=%zu", fill,
                    multilistCount(ml), targetCount / 2);
            }
        }
        printf("  verified tier transitions across fill levels 1-8\n");
    }

    TEST("MEDIUM: unbalanced head-only push through tier") {
        /* Push only to head - F0 grows large, F1 stays empty.
         * This tests the unbalanced scenario where rebalancing TODO
         * is not yet implemented (multilistMedium.c lines 143, 156, 479).
         * Use limit=6 (2048 bytes) so Medium→Full is at 6144 bytes,
         * giving room to stay in Medium with ~100 small integers. */
        cleaner(multilistFree) multilist *ml = multilistNew(6, 0);

        /* Push enough to get into Medium tier but not Full tier */
        const size_t targetCount = 100;
        for (size_t i = 0; i < targetCount; i++) {
            databox box = databoxNewSigned((int64_t)i);
            multilistPushByTypeHead(&ml, s0, &box);
        }

        /* Verify count is correct */
        if (multilistCount(ml) != targetCount) {
            ERR("head-only count: got %zu expected %zu", multilistCount(ml),
                targetCount);
        }

        /* Verify index access works across the unbalanced structure */
        for (size_t i = 0; i < targetCount; i++) {
            multilistEntry entry;
            if (!multilistIndex(ml, s0, (int64_t)i, &entry, false)) {
                ERR("head-only index failed at %zu", i);
            }
            /* Head push inverts order: index 0 = value (targetCount-1) */
            int64_t expected = (int64_t)(targetCount - 1 - i);
            int64_t got;
            if (DATABOX_IS_SIGNED_INTEGER(&entry.box)) {
                got = entry.box.data.i64;
            } else if (DATABOX_IS_UNSIGNED_INTEGER(&entry.box)) {
                got = (int64_t)entry.box.data.u64;
            } else {
                got = -9999;
            }
            if (got != expected) {
                ERR("head-only index %zu: got %" PRId64 " expected %" PRId64, i,
                    got, expected);
            }
        }

        /* Verify forward iteration */
        multilistIterator iter;
        multilistIteratorInitForwardReadOnly(ml, s, &iter);
        multilistEntry entry;
        size_t iterCount = 0;
        int64_t expectedVal = (int64_t)(targetCount - 1);
        while (multilistNext(&iter, &entry)) {
            int64_t got = (entry.box.type == DATABOX_SIGNED_64)
                              ? entry.box.data.i
                              : (int64_t)entry.box.data.u;
            if (got != expectedVal) {
                ERR("head-only iter at %zu: got %" PRId64 " expected %" PRId64,
                    iterCount, got, expectedVal);
            }
            expectedVal--;
            iterCount++;
        }
        multilistIteratorRelease(&iter);

        if (iterCount != targetCount) {
            ERR("head-only iter count: got %zu expected %zu", iterCount,
                targetCount);
        }

        /* Verify backward iteration - reverse iter still uses multilistNext */
        multilistIteratorInitReverseReadOnly(ml, s, &iter);
        iterCount = 0;
        expectedVal = 0;
        while (multilistNext(&iter, &entry)) {
            int64_t got = (entry.box.type == DATABOX_SIGNED_64)
                              ? entry.box.data.i
                              : (int64_t)entry.box.data.u;
            if (got != expectedVal) {
                ERR("head-only reverse iter at %zu: got %" PRId64
                    " expected %" PRId64,
                    iterCount, got, expectedVal);
            }
            expectedVal++;
            iterCount++;
        }
        multilistIteratorRelease(&iter);

        if (iterCount != targetCount) {
            ERR("head-only reverse iter count: got %zu expected %zu", iterCount,
                targetCount);
        }

        printf("  verified unbalanced head-only push (%zu elements)\n",
               targetCount);
    }

    TEST("MEDIUM: unbalanced tail-only push through tier") {
        /* Push only to tail - F1 grows large, F0 starts minimal.
         * After NewFromFlex, F0 has half the initial flex, F1 has other half,
         * then all new pushes go to F1 only.
         * Use limit=6 (2048 bytes) to stay in Medium tier. */
        cleaner(multilistFree) multilist *ml = multilistNew(6, 0);

        const size_t targetCount = 100;
        for (size_t i = 0; i < targetCount; i++) {
            databox box = databoxNewSigned((int64_t)i);
            multilistPushByTypeTail(&ml, s0, &box);
        }

        if (multilistCount(ml) != targetCount) {
            ERR("tail-only count: got %zu expected %zu", multilistCount(ml),
                targetCount);
        }

        /* Verify index access - tail push maintains order */
        for (size_t i = 0; i < targetCount; i++) {
            multilistEntry entry;
            if (!multilistIndex(ml, s0, (int64_t)i, &entry, false)) {
                ERR("tail-only index failed at %zu", i);
            }
            int64_t expected = (int64_t)i;
            int64_t got = (entry.box.type == DATABOX_SIGNED_64)
                              ? entry.box.data.i
                              : (int64_t)entry.box.data.u;
            if (got != expected) {
                ERR("tail-only index %zu: got %" PRId64 " expected %" PRId64, i,
                    got, expected);
            }
        }

        /* Verify forward iteration */
        multilistIterator iter;
        multilistIteratorInitForwardReadOnly(ml, s, &iter);
        multilistEntry entry;
        size_t iterCount = 0;
        while (multilistNext(&iter, &entry)) {
            int64_t expected = (int64_t)iterCount;
            int64_t got = (entry.box.type == DATABOX_SIGNED_64)
                              ? entry.box.data.i
                              : (int64_t)entry.box.data.u;
            if (got != expected) {
                ERR("tail-only iter at %zu: got %" PRId64 " expected %" PRId64,
                    iterCount, got, expected);
            }
            iterCount++;
        }
        multilistIteratorRelease(&iter);

        if (iterCount != targetCount) {
            ERR("tail-only iter count: got %zu expected %zu", iterCount,
                targetCount);
        }

        printf("  verified unbalanced tail-only push (%zu elements)\n",
               targetCount);
    }

    TEST("MEDIUM: alternating head/tail maintains balance") {
        /* Alternating pushes should keep F0 and F1 roughly balanced.
         * Use limit=6 (2048 bytes) to stay in Medium tier. */
        cleaner(multilistFree) multilist *ml = multilistNew(6, 0);

        const size_t targetCount = 100;
        for (size_t i = 0; i < targetCount; i++) {
            databox box = databoxNewSigned((int64_t)i);
            if (i % 2 == 0) {
                multilistPushByTypeHead(&ml, s0, &box);
            } else {
                multilistPushByTypeTail(&ml, s0, &box);
            }
        }

        if (multilistCount(ml) != targetCount) {
            ERR("alternating count: got %zu expected %zu", multilistCount(ml),
                targetCount);
        }

        /* Verify we can iterate all elements */
        multilistIterator iter;
        multilistIteratorInitForwardReadOnly(ml, s, &iter);
        multilistEntry entry;
        size_t iterCount = 0;
        while (multilistNext(&iter, &entry)) {
            iterCount++;
        }
        multilistIteratorRelease(&iter);

        if (iterCount != targetCount) {
            ERR("alternating iter count: got %zu expected %zu", iterCount,
                targetCount);
        }

        printf("  verified alternating head/tail push (%zu elements)\n",
               targetCount);
    }

    TEST("MEDIUM: unbalanced delete operations") {
        /* Build a balanced list, then delete from one end only.
         * Use limit=6 (2048 bytes) to stay in Medium tier. */
        cleaner(multilistFree) multilist *ml = multilistNew(6, 0);

        const size_t buildCount = 80;
        for (size_t i = 0; i < buildCount; i++) {
            databox box = databoxNewSigned((int64_t)i);
            if (i % 2 == 0) {
                multilistPushByTypeHead(&ml, s0, &box);
            } else {
                multilistPushByTypeTail(&ml, s0, &box);
            }
        }

        /* Pop only from head until half remain */
        size_t remaining = buildCount;
        while (remaining > buildCount / 2) {
            databox got = {{0}};
            if (!multilistPopHead(&ml, s0, &got)) {
                ERR("unbalanced delete pop failed at remaining=%zu", remaining);
            }
            databoxFreeData(&got);
            remaining--;
        }

        if (multilistCount(ml) != remaining) {
            ERR("unbalanced delete count: got %zu expected %zu",
                multilistCount(ml), remaining);
        }

        /* Verify iteration still works after unbalanced deletions */
        multilistIterator iter;
        multilistIteratorInitForwardReadOnly(ml, s, &iter);
        multilistEntry entry;
        size_t iterCount = 0;
        while (multilistNext(&iter, &entry)) {
            iterCount++;
        }
        multilistIteratorRelease(&iter);

        if (iterCount != remaining) {
            ERR("unbalanced delete iter: got %zu expected %zu", iterCount,
                remaining);
        }

        printf("  verified unbalanced delete operations (%zu remaining)\n",
               remaining);
    }

    TEST("MEDIUM: negative index access with unbalanced structure") {
        /* Test negative indexing when F0/F1 are unbalanced.
         * Use limit=6 (2048 bytes) to stay in Medium tier. */
        cleaner(multilistFree) multilist *ml = multilistNew(6, 0);

        const size_t targetCount = 100;
        /* Push all to head - creates unbalanced F0 */
        for (size_t i = 0; i < targetCount; i++) {
            databox box = databoxNewSigned((int64_t)i);
            multilistPushByTypeHead(&ml, s0, &box);
        }

        /* Test negative indices */
        for (size_t i = 1; i <= targetCount; i++) {
            multilistEntry entry;
            int64_t negIdx = -(int64_t)i;
            if (!multilistIndex(ml, s0, negIdx, &entry, false)) {
                ERR("negative index failed at %" PRId64, negIdx);
            }
            /* -1 should be value 0, -2 should be value 1, etc. */
            int64_t expected = (int64_t)(i - 1);
            int64_t got = (entry.box.type == DATABOX_SIGNED_64)
                              ? entry.box.data.i
                              : (int64_t)entry.box.data.u;
            if (got != expected) {
                ERR("negative index %" PRId64 ": got %" PRId64
                    " expected %" PRId64,
                    negIdx, got, expected);
            }
        }

        printf("  verified negative index access with unbalanced structure\n");
    }

    TEST("MEDIUM: DelEntry while iterating visits every element") {
        /* Each delete reallocs the flex being iterated, and deleting F0's
         * last element moves F1 into F0; iteration must survive both and
         * still visit every element exactly once, in both directions. */
        const int total = 60;
        for (int pass = 0; pass < 4; pass++) {
            const bool forward = pass < 2;
            const int every = pass % 2 == 0 ? 1 : 2; /* delete all, or half */
            cleaner(multilistFree) multilist *ml =
                multilistNew(FLEX_CAP_LEVEL_512, 0);
            char buf[64];
            for (int i = 0; i < total; i++) {
                snprintf(buf, sizeof(buf), "medium-del-%02d-padding", i);
                const databox box = databoxNewBytesString(buf);
                multilistPushByTypeTail(&ml, s0, &box);
            }

            if (_multilistType(ml) != MULTILIST_TYPE_MEDIUM) {
                ERR("pass %d: landed tier %d, wanted Medium", pass,
                    (int)_multilistType(ml));
            }

            bool kept[60];
            multilistIterator iter;
            multilistEntry entry;
            multilistIteratorInit(ml, s, &iter, forward, false);
            int visited = 0;
            while (multilistNext(&iter, &entry)) {
                const int i = forward ? visited : total - 1 - visited;
                snprintf(buf, sizeof(buf), "medium-del-%02d-padding", i);
                if (visited >= total || entry.box.len != strlen(buf) ||
                    memcmp(entry.box.data.bytes.start, buf, entry.box.len)) {
                    ERR("pass %d: element %d read back wrong", pass, i);
                    break;
                }

                kept[i] = visited % every != 0;
                if (!kept[i]) {
                    multilistDelEntry(&iter, &entry);
                }

                visited++;
            }

            multilistIteratorRelease(&iter);
            if (visited != total) {
                ERR("pass %d: visited %d of %d elements", pass, visited, total);
                continue;
            }

            int remaining = 0;
            multilistIteratorInitForwardReadOnly(ml, s, &iter);
            for (int i = 0; i < total; i++) {
                if (!kept[i]) {
                    continue;
                }

                snprintf(buf, sizeof(buf), "medium-del-%02d-padding", i);
                if (!multilistNext(&iter, &entry) ||
                    entry.box.len != strlen(buf) ||
                    memcmp(entry.box.data.bytes.start, buf, entry.box.len)) {
                    ERR("pass %d: kept element %d missing", pass, i);
                    break;
                }

                remaining++;
            }

            multilistIteratorRelease(&iter);
            if ((int)multilistCount(ml) != remaining) {
                ERR("pass %d: %zu elements left, want %d", pass,
                    multilistCount(ml), remaining);
            }
        }
    }

    TEST("DelEntry keeps every entry and iterator offset naming its element, "
         "every tier") {
        /* Walks forward and in reverse, from either end or from an index
         * (positive or negative, so Full entries carry head- and
         * tail-relative offsets), deleting by pattern: every element,
         * every other one (both phases), the first or the last element of
         * each node, whole nodes, and a mix.  After every Next the
         * entry's offset must name its element, and after every
         * DelEntry the iterator's offset must name the element it
         * returns next.  The walk must visit exactly the expected
         * elements, and the list must then match an array oracle. */
        enum { DO_MAX = 600, DO_PATTERNS = 7, DO_STARTS = 6 };
        const struct {
            const char *name;
            flexCapSizeLimit limit;
            uint32_t depth;
            uint32_t count;
            multilistType wantType;
        } configs[] = {
            {"Small", FLEX_CAP_LEVEL_65536, 0, 60, MULTILIST_TYPE_SMALL},
            {"Medium", FLEX_CAP_LEVEL_512, 0, 40, MULTILIST_TYPE_MEDIUM},
            {"Full depth 0", FLEX_CAP_LEVEL_512, 0, 300, MULTILIST_TYPE_FULL},
            {"Full depth 1", FLEX_CAP_LEVEL_512, 1, 600, MULTILIST_TYPE_FULL},
        };
        static bool firstIn[DO_MAX];
        static bool lastIn[DO_MAX];
        static mlNodeId nodeOf[DO_MAX];
        static bool gone[DO_MAX];
        uint32_t negativeOffsets = 0;
        for (size_t c = 0; c < sizeof(configs) / sizeof(*configs); c++) {
            const uint32_t n = configs[c].count;
            for (int from = 0; from < DO_STARTS; from++) {
                /* forward from the head, reverse from the tail, then
                 * forward / reverse from indexes 2n/3, n/3, -n/2, -n/4 */
                const bool forward = from % 2 == 0;
                const mlOffsetId at[DO_STARTS] = {
                    0,     (mlOffsetId)n - 1,  2 * (mlOffsetId)n / 3,
                    n / 3, -(mlOffsetId)n / 2, -(mlOffsetId)n / 4};
                const uint32_t first =
                    (uint32_t)(at[from] < 0 ? n + at[from] : at[from]);
                for (int pattern = 0; pattern < DO_PATTERNS; pattern++) {
                    cleaner(multilistFree) multilist *ml =
                        multilistNew(configs[c].limit, configs[c].depth);
                    char buf[MLR_LEN + 1];
                    for (uint32_t i = 0; i < n; i++) {
                        const databox box = mlrBox(i, buf);
                        multilistPushByTypeTail(&ml, s0, &box);
                    }

                    if (_multilistType(ml) != configs[c].wantType) {
                        ERR("%s: landed tier %d", configs[c].name,
                            (int)_multilistType(ml));
                        break;
                    }

                    /* The node layout, from a read-only walk */
                    multilistIterator iter;
                    multilistEntry entry;
                    multilistIteratorInitForwardReadOnly(ml, s, &iter);
                    for (uint32_t i = 0; multilistNext(&iter, &entry); i++) {
                        const flex *fl = mloFlex(ml, &entry);
                        firstIn[i] = entry.fe == flexHead(fl);
                        lastIn[i] = flexNext(fl, entry.fe) == NULL;
                        nodeOf[i] = entry.nodeIdx;
                        gone[i] = false;
                    }
                    multilistIteratorRelease(&iter);

                    const char *why = NULL;
                    if (from < 2) {
                        multilistIteratorInit(ml, s, &iter, forward, false);
                    } else if (!multilistIteratorInitAtIdx(
                                   ml, s, &iter, at[from], forward, false)) {
                        why = "InitAtIdx refused a valid index";
                    }

                    const uint32_t steps = forward ? n - first : first + 1;
                    uint32_t k = 0;
                    for (; !why && multilistNext(&iter, &entry); k++) {
                        const uint32_t id = forward ? first + k : first - k;
                        if (k >= steps || !mlrIs(&entry.box, id)) {
                            why = "the walk read the wrong element";
                            break;
                        }

                        if (flexIndex(mloFlex(ml, &entry), entry.offset) !=
                            entry.fe) {
                            why = "an entry offset does not name its element";
                            break;
                        }

                        negativeOffsets += entry.offset < 0;
                        bool del;
                        switch (pattern) {
                        case 0:
                            del = true;
                            break;
                        case 1:
                            del = k % 2 == 0;
                            break;
                        case 2:
                            del = k % 2 == 1;
                            break;
                        case 3:
                            del = firstIn[id];
                            break;
                        case 4:
                            del = lastIn[id];
                            break;
                        case 5:
                            del = nodeOf[id] % 2 == 0;
                            break;
                        default:
                            del = id % 7 == 3 || lastIn[id];
                            break;
                        }

                        if (!del) {
                            continue;
                        }

                        multilistDelEntry(&iter, &entry);
                        gone[id] = true;
                        if (iter.fe) {
                            const multilistEntry next = {
                                .nodeIdx = iter.nodeIdx, .f = iter.f};
                            if (flexIndex(mloFlex(ml, &next), iter.offset) !=
                                iter.fe) {
                                why = "after DelEntry the iterator offset does "
                                      "not name its next element";
                            }
                        }
                    }

                    multilistIteratorRelease(&iter);
                    if (!why && k != steps) {
                        why = "the walk did not visit every element";
                    }

                    /* What is left, against the oracle */
                    uint32_t left = 0;
                    if (!why) {
                        multilistIteratorInitForwardReadOnly(ml, s, &iter);
                        for (uint32_t i = 0; i < n && !why; i++) {
                            if (gone[i]) {
                                continue;
                            }

                            if (!multilistNext(&iter, &entry) ||
                                !mlrIs(&entry.box, i)) {
                                why = "a kept element is missing";
                            }

                            left++;
                        }

                        if (!why && multilistNext(&iter, &entry)) {
                            why = "a deleted element is still there";
                        }

                        multilistIteratorRelease(&iter);
                    }

                    if (!why && multilistCount(ml) != left) {
                        why = "the count does not match";
                    }

                    if (why) {
                        ERR("%s, %s from %" PRId64 ", pattern %d, step %u: %s",
                            configs[c].name, forward ? "forward" : "reverse",
                            at[from], pattern, k, why);
                    }
                }
            }
        }

        if (!negativeOffsets) {
            ERRR("no walk produced a tail-relative offset");
        }
    }

    TEST("edge inputs on every tier: out-of-range indexes, empty lists, "
         "no-op deletes and invalid replaces") {
        /* On a populated list of each tier: the first and last valid
         * indexes of both signs read the right element; every index past
         * either end (by one, by two, at the int64 limits and past 32
         * bits) is refused by IndexGet, IndexCheck, InitAtIdx and
         * Replace, and DelRange refuses zero and negative counts and
         * starts outside the list; refused calls change nothing.  Then
         * the same list, emptied by pops, refuses everything, iterates
         * nothing, and still takes a first insert through IndexGet's
         * entry. */
        enum { EI_MAX = 600 };
        const struct {
            const char *name;
            flexCapSizeLimit limit;
            uint32_t depth;
            uint32_t count;
            multilistType wantType;
        } configs[] = {
            {"Small", FLEX_CAP_LEVEL_65536, 0, 60, MULTILIST_TYPE_SMALL},
            {"Medium", FLEX_CAP_LEVEL_512, 0, 40, MULTILIST_TYPE_MEDIUM},
            {"Full depth 0", FLEX_CAP_LEVEL_512, 0, 300, MULTILIST_TYPE_FULL},
            {"Full depth 1", FLEX_CAP_LEVEL_512, 1, 600, MULTILIST_TYPE_FULL},
        };
        static uint32_t want[EI_MAX];
        const mlOffsetId big = (mlOffsetId)1 << 32;
        for (size_t c = 0; c < sizeof(configs) / sizeof(*configs); c++) {
            const char *name = configs[c].name;
            uint32_t n = configs[c].count;
            cleaner(multilistFree) multilist *ml =
                multilistNew(configs[c].limit, configs[c].depth);
            char buf[MLR_LEN + 1];
            for (uint32_t i = 0; i < n; i++) {
                const databox box = mlrBox(i, buf);
                multilistPushByTypeTail(&ml, s0, &box);
                want[i] = i;
            }

            if (_multilistType(ml) != configs[c].wantType) {
                ERR("%s: landed tier %d", name, (int)_multilistType(ml));
                continue;
            }

            const mlOffsetId sn = n;
            multilistEntry entry;
            multilistIterator iter;
            const mlOffsetId good[] = {0, sn - 1, -1, -sn};
            for (size_t k = 0; k < sizeof(good) / sizeof(*good); k++) {
                const uint32_t pos =
                    (uint32_t)(good[k] < 0 ? sn + good[k] : good[k]);
                if (!multilistIndexGet(ml, s0, good[k], &entry) ||
                    !mlrIs(&entry.box, want[pos]) ||
                    !multilistIndexCheck(ml, s0, good[k], &entry)) {
                    ERR("%s: valid index %" PRId64 " misread", name, good[k]);
                }
            }

            const mlOffsetId bad[] = {sn,      sn + 1,    -sn - 1,
                                      -sn - 2, INT64_MAX, INT64_MIN,
                                      big,     big + 1,   -big};
            for (size_t k = 0; k < sizeof(bad) / sizeof(*bad); k++) {
                if (multilistIndexGet(ml, s0, bad[k], &entry) ||
                    multilistIndexCheck(ml, s0, bad[k], &entry)) {
                    ERR("%s: index %" PRId64 " was accepted", name, bad[k]);
                }

                memset(&iter, 0, sizeof(iter));
                if (multilistIteratorInitAtIdx(ml, s, &iter, bad[k], true,
                                               true)) {
                    ERR("%s: InitAtIdx(%" PRId64 ") was accepted", name,
                        bad[k]);
                }
            }

            const databox rep = mlrBox(900000, buf);
            /* Past 32 bits too: each of the last five would land on a
             * real element (1, n - 1, 0, 1, n - 1) if it were narrowed to
             * 32 bits on the way in. */
            const mlOffsetId badRep[] = {sn,
                                         sn + 1,
                                         -sn - 1,
                                         INT32_MAX,
                                         INT32_MIN,
                                         (mlOffsetId)INT32_MAX + 1,
                                         (mlOffsetId)INT32_MIN - 1,
                                         INT64_MAX,
                                         INT64_MIN,
                                         big + 1,
                                         big + sn - 1,
                                         -big,
                                         -big + 1,
                                         -big - 1};
            for (size_t k = 0; k < sizeof(badRep) / sizeof(*badRep); k++) {
                if (multilistReplaceByTypeAtIndex(&ml, s0, badRep[k], &rep)) {
                    ERR("%s: Replace(%" PRId64 ") was accepted", name,
                        badRep[k]);
                }
            }

            const struct {
                mlOffsetId start;
                int64_t values;
            } noop[] = {{0, 0},   {sn - 1, 0},  {0, -1},        {0, INT64_MIN},
                        {sn, 1},  {-sn - 1, 1}, {INT64_MAX, 1}, {INT64_MIN, 3},
                        {big, 1}, {-big, 1}};
            for (size_t k = 0; k < sizeof(noop) / sizeof(*noop); k++) {
                if (multilistDelRange(&ml, s0, noop[k].start, noop[k].values)) {
                    ERR("%s: DelRange(%" PRId64 ", %" PRId64 ") was accepted",
                        name, noop[k].start, noop[k].values);
                }
            }

            const char *why = mlrListIs(ml, s, want, n);
            if (why) {
                ERR("%s: after the refused calls: %s", name, why);
            }

            /* The edges are accepted: replace both ends, delete the last
             * element with an oversized count and the first exactly. */
            const databox repHead = mlrBox(900001, buf);
            if (!multilistReplaceByTypeAtIndex(&ml, s0, -sn, &repHead)) {
                ERR("%s: Replace(-count) was refused", name);
            }
            want[0] = 900001;
            const databox repTail = mlrBox(900002, buf);
            if (!multilistReplaceByTypeAtIndex(&ml, s0, sn - 1, &repTail)) {
                ERR("%s: Replace(count - 1) was refused", name);
            }
            want[n - 1] = 900002;
            if (!multilistDelRange(&ml, s0, -1, INT64_MAX) ||
                !multilistDelRange(&ml, s0, -(mlOffsetId)(n - 1), 1)) {
                ERR("%s: an edge DelRange was refused", name);
            }
            n -= 2;
            memmove(&want[0], &want[1], n * sizeof(*want));
            why = mlrListIs(ml, s, want, n);
            if (why) {
                ERR("%s: after the edge replaces and deletes: %s", name, why);
            }

            /* Empty the list, keeping its tier. */
            databox got = {{0}};
            while (multilistPopTail(&ml, s0, &got)) {
                databoxFreeData(&got);
                memset(&got, 0, sizeof(got));
            }

            if (_multilistType(ml) != configs[c].wantType ||
                multilistCount(ml) != 0) {
                ERR("%s: popping everything left tier %d with %zu elements",
                    name, (int)_multilistType(ml), multilistCount(ml));
                continue;
            }

            const mlOffsetId emptyIdx[] = {0, -1, 1, INT64_MIN};
            for (size_t k = 0; k < sizeof(emptyIdx) / sizeof(*emptyIdx); k++) {
                if (multilistIndexGet(ml, s0, emptyIdx[k], &entry) ||
                    multilistIndexCheck(ml, s0, emptyIdx[k], &entry) ||
                    multilistIteratorInitAtIdx(ml, s, &iter, emptyIdx[k], true,
                                               true) ||
                    multilistReplaceByTypeAtIndex(&ml, s0, emptyIdx[k], &rep) ||
                    multilistDelRange(&ml, s0, emptyIdx[k], 1)) {
                    ERR("%s: the empty list accepted index %" PRId64, name,
                        emptyIdx[k]);
                }
            }

            if (multilistPopHead(&ml, s0, &got)) {
                ERR("%s: the empty list popped something", name);
            }

            multilistRotate(ml, s);
            for (int dir = 0; dir < 2; dir++) {
                multilistIteratorInit(ml, s, &iter, dir == 0, true);
                if (multilistNext(&iter, &entry)) {
                    ERR("%s: the empty list iterated an element", name);
                }
                multilistIteratorRelease(&iter);
            }

            /* A first insert through IndexGet's entry on the empty list,
             * once before and (after emptying again) once after. */
            for (int after = 0; after < 2; after++) {
                const databox first = mlrBox(800000 + (uint32_t)after, buf);
                memset(&entry, 0xFF, sizeof(entry));
                multilistIndexGet(ml, s0, 0, &entry);
                if (after) {
                    multilistInsertByTypeAfter(&ml, s, &entry, &first);
                } else {
                    multilistInsertByTypeBefore(&ml, s, &entry, &first);
                }

                want[0] = 800000 + (uint32_t)after;
                why = mlrListIs(ml, s, want, 1);
                if (why) {
                    ERR("%s: the first insert %s: %s", name,
                        after ? "after" : "before", why);
                }

                if (multilistPopHead(&ml, s0, &got)) {
                    databoxFreeData(&got);
                    memset(&got, 0, sizeof(got));
                }
            }
        }
    }

    TEST("every tier declaration links, and every Adapter macro resolves to "
         "working code") {
        /* Small and Medium through every multilist{Small,Medium}Adapter*
         * macro, including New and the Set* settings (accepted and
         * ignored), checked against an id oracle; Medium's
         * NewFromFlexConsumeGrow from a Small; and Full's SetOptions,
         * SetDepth, AppendValuesFlex, AppendValuesFromFlex, NewFromFlex
         * and multilistFulllCreateFlex. */
        const char *why = NULL;
        MLA_EXERCISE(Small, why);
        if (why) {
            ERR("Small: %s", why);
        }

        why = NULL;
        MLA_EXERCISE(Medium, why);
        if (why) {
            ERR("Medium: %s", why);
        }

        char buf[MLR_LEN + 1];
        enum { LK_N = 200 };
        static uint32_t want[2 * LK_N];
        cleaner(flexFree) flex *src = flexNew();
        for (uint32_t i = 0; i < LK_N; i++) {
            const databox box = mlrBox(i, buf);
            flexPushByType(&src, &box, FLEX_ENDPOINT_TAIL);
            want[i] = i;
            want[LK_N + i] = i;
        }

        {
            /* Small -> Medium the way the upgrade does it */
            flex *fl = flexDuplicate(src);
            cleaner(multilistMediumFree) multilistMedium *md =
                multilistMediumAdapterNewFromFlexConsumeGrow(
                    multilistSmallAdapterNewFromFlexConsume(fl), fl);
            cleaner(flexFree) flex *dump = multilistMediumAdapterDump(md, s0);
            if (!mlaFlexIs(dump, want, LK_N)) {
                ERRR("Medium NewFromFlexConsumeGrow differs");
            }
        }

        {
            cleaner(multilistFullFree) multilistFull *a =
                multilistFullNewFromFlex(FLEX_CAP_LEVEL_512, 0, s0, src);
            cleaner(multilistFullFree) multilistFull *b =
                multilistFulllCreateFlex(FLEX_CAP_LEVEL_512, 0, s0, src);
            multilistFullAppendValuesFromFlex(a, s0, src);
            multilistFullAppendValuesFlex(b, s0, src);
            cleaner(flexFree) flex *da = multilistFullDump(a, s0);
            cleaner(flexFree) flex *db = multilistFullDump(b, s0);
            if (!mlaFlexIs(da, want, 2 * LK_N) ||
                !mlaFlexIs(db, want, 2 * LK_N)) {
                ERRR("Full NewFromFlex / FulllCreateFlex / AppendValues "
                     "differ");
            }

            /* Depth 1 compresses interior nodes, depth 0 decompresses */
            multilistFullSetOptions(a, FLEX_CAP_LEVEL_512, 1);
            multilistFullSetDepth(b, FLEX_CAP_LEVEL_512, 1);
            const bool compressed =
                multilistFullBytesActual(a) < multilistFullBytes(a) &&
                multilistFullBytesActual(b) < multilistFullBytes(b);
            multilistFullSetOptions(a, FLEX_CAP_LEVEL_512, 0);
            multilistFullSetDepth(b, FLEX_CAP_LEVEL_512, 0);
            if (!compressed ||
                multilistFullBytesActual(a) != multilistFullBytes(a) ||
                multilistFullBytesActual(b) != multilistFullBytes(b)) {
                ERRR("Full SetOptions / SetDepth did not apply the depth");
            }

            cleaner(flexFree) flex *da2 = multilistFullDump(a, s0);
            if (!mlaFlexIs(da2, want, 2 * LK_N)) {
                ERRR("Full SetOptions changed the contents");
            }
        }
    }

    TEST("FUZZ: stress random operations") {
        cleaner(multilistFree) multilist *ml = multilistNew(4, 2);
        size_t count = 0;

        srand(99999);
        for (size_t round = 0; round < 10000; round++) {
            int op = rand() % 10;

            if (op < 4) {
                /* Push head (40%) */
                databox box = databoxNewSigned((int64_t)rand());
                multilistPushByTypeHead(&ml, s0, &box);
                count++;
            } else if (op < 8) {
                /* Push tail (40%) */
                databox box = databoxNewSigned((int64_t)rand());
                multilistPushByTypeTail(&ml, s0, &box);
                count++;
            } else if (count > 0) {
                /* Pop (20%) */
                databox got = {{0}};
                if (rand() % 2) {
                    multilistPopHead(&ml, s0, &got);
                } else {
                    multilistPopTail(&ml, s0, &got);
                }
                databoxFreeData(&got);
                count--;
            }

            /* Periodic verification */
            if (round % 1000 == 0) {
                if (multilistCount(ml) != count) {
                    ERR("round %zu: ml=%zu oracle=%zu", round,
                        multilistCount(ml), count);
                }
            }
        }

        if (multilistCount(ml) != count) {
            ERR("final: ml=%zu oracle=%zu", multilistCount(ml), count);
        }

        printf("  completed 10K random operations, final count=%zu\n", count);
    }

    printf("\n=== All multilist fuzz tests completed! ===\n\n");

    TEST("DelRange span sweep vs shadow array (all tiers)") {
        /* Shadow-verified DelRange over every (start,count) combo on a
         * list sized with ~100-byte payloads so growth crosses the
         * Small->Medium->Full tiers; catches per-flex split math bugs
         * (Medium's mid-F0 spans silently deleted the wrong elements) */
        char payload[128];
        int32_t sweepFailures = 0;
        bool sawMedium = false;

        for (int32_t n = 20; n <= 60 && sweepFailures == 0; n += 20) {
            for (int32_t delStart = 0; delStart < n && sweepFailures == 0;
                 delStart += 3) {
                for (int32_t del = 1; del <= n - delStart && sweepFailures == 0;
                     del += 5) {
                    /* 2048-byte cap: ~110B payloads put n=20..60 right
                     * across the Small->Medium->Full transitions */
                    cleaner(multilistFree) multilist *ml =
                        multilistNew(FLEX_CAP_LEVEL_2048, 2);

                    for (int32_t i = 0; i < n; i++) {
                        snprintf(payload, sizeof(payload),
                                 "element-%04d-"
                                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                 "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                                 i);
                        const databox box = databoxNewBytesString(payload);
                        multilistPushByTypeTail(&ml, s0, &box);
                    }

                    if (_multilistType(ml) == MULTILIST_TYPE_MEDIUM) {
                        sawMedium = true;
                    }

                    multilistDelRange(&ml, s0, delStart, del);

                    /* Survivors: [0,delStart) then [delStart+del, n) */
                    const int32_t expected = n - del;
                    bool bad = false;
                    if ((int32_t)multilistCount(ml) != expected) {
                        bad = true;
                    }

                    for (int32_t i = 0; !bad && i < expected; i++) {
                        const int32_t orig = i < delStart ? i : i + del;
                        snprintf(payload, sizeof(payload),
                                 "element-%04d-"
                                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                 "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                                 orig);
                        multilistEntry entry = {0};
                        if (!multilistIndexGet(ml, s0, i, &entry) ||
                            entry.box.len != strlen(payload) ||
                            memcmp(entry.box.data.bytes.start, payload,
                                   entry.box.len) != 0) {
                            bad = true;
                        }
                    }

                    if (bad) {
                        ERR("DelRange(n=%d, start=%d, del=%d) corrupted "
                            "survivors",
                            n, delStart, del);
                        sweepFailures++;
                    }
                }
            }
        }

        if (!sawMedium) {
            ERRR("Sweep never reached Medium tier - sizing is wrong");
        }
    }

    TEST("FUZZ/SOAK: differential ops vs shadow model across tiers") {
        /* Deterministic random op soak: push head/tail, DelRange, and
         * Duplicate against a flat shadow array, verifying full list
         * contents periodically. Payload sizes chosen so lists wander
         * across Small/Medium/Full; tier coverage is asserted so the
         * harness can never silently regress into single-tier testing. */
        uint64_t rng = 0x9E3779B97F4A7C15ULL;
#define FUZZ_NEXT() (rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17, rng)
        enum { SHADOW_MAX = 512 };
        static int64_t shadow[SHADOW_MAX];
        int32_t shadowCount = 0;
        int64_t nextId = 0;
        char payload[96];
        bool tierSeen[4] = {false, false, false, false};
        int32_t soakFailures = 0;
        int32_t soakMaxCount = 0;

#define FUZZ_PAYLOAD(id)                                                       \
    snprintf(payload, sizeof(payload),                                         \
             "id-%08" PRId64                                                   \
             "-pppppppppppppppppppppppppppppppppppppppppppppppp",              \
             (int64_t)(id))

        const uint32_t wantLimit = FLEX_CAP_LEVEL_2048;
        const uint32_t wantDepth = 2;
        cleaner(multilistFree) multilist *ml =
            multilistNew(wantLimit, wantDepth);

        for (int32_t op = 0; op < 20000 && soakFailures == 0; op++) {
            const uint64_t roll = FUZZ_NEXT() % 100;

            if (roll < 45 && shadowCount < SHADOW_MAX) {
                /* push tail */
                FUZZ_PAYLOAD(nextId);
                const databox box = databoxNewBytesString(payload);
                multilistPushByTypeTail(&ml, s0, &box);
                shadow[shadowCount++] = nextId++;
            } else if (roll < 70 && shadowCount < SHADOW_MAX) {
                /* push head */
                FUZZ_PAYLOAD(nextId);
                const databox box = databoxNewBytesString(payload);
                multilistPushByTypeHead(&ml, s0, &box);
                memmove(shadow + 1, shadow,
                        (size_t)shadowCount * sizeof(*shadow));
                shadow[0] = nextId++;
                shadowCount++;
            } else if (roll < 90 && shadowCount > 0) {
                /* DelRange at random start/len */
                const int32_t dStart = (int32_t)(FUZZ_NEXT() % shadowCount);
                int32_t maxDel = shadowCount - dStart;
                /* Cap spans so pushes outpace deletes on average and
                 * the population climbs through Medium into Full
                 * (uncapped spans held equilibrium below the Full
                 * threshold and the tier assert caught it) */
                if (maxDel > 3) {
                    maxDel = 3;
                }
                const int32_t del = 1 + (int32_t)(FUZZ_NEXT() % maxDel);
                multilistDelRange(&ml, s0, dStart, del);
                memmove(shadow + dStart, shadow + dStart + del,
                        (size_t)(shadowCount - dStart - del) * sizeof(*shadow));
                shadowCount -= del;
            } else if (roll < 96 && shadowCount > 0) {
                /* pop head or tail */
                const bool fromTail = (FUZZ_NEXT() & 1) != 0;
                databox popped = {{0}};
                if (!multilistPop(&ml, s0, &popped, fromTail)) {
                    ERR("op %d: pop(%d) failed with %d elements", op,
                        (int)fromTail, shadowCount);
                    soakFailures++;
                } else {
                    const int64_t wantId =
                        fromTail ? shadow[shadowCount - 1] : shadow[0];
                    FUZZ_PAYLOAD(wantId);
                    if (popped.len != strlen(payload) ||
                        memcmp(popped.data.bytes.start, payload, popped.len) !=
                            0) {
                        ERR("op %d: pop(%d) wrong element (want id %" PRId64
                            ")",
                            op, (int)fromTail, wantId);
                        soakFailures++;
                    }
                    databoxFreeData(&popped);
                    if (!fromTail) {
                        memmove(shadow, shadow + 1,
                                (size_t)(shadowCount - 1) * sizeof(*shadow));
                    }
                    shadowCount--;
                }
            } else {
                /* duplicate + free (leak/aliasing exercise); contents
                 * must match the shadow element-for-element, not just
                 * by count */
                cleaner(multilistFree) multilist *dup = multilistDuplicate(ml);
                if ((int32_t)multilistCount(dup) != shadowCount) {
                    ERR("op %d: duplicate count %zu != shadow %d", op,
                        multilistCount(dup), shadowCount);
                    soakFailures++;
                }
                for (int32_t i = 0; i < shadowCount && soakFailures == 0; i++) {
                    FUZZ_PAYLOAD(shadow[i]);
                    multilistEntry entry = {0};
                    if (!multilistIndexGet(dup, s0, i, &entry) ||
                        entry.box.len != strlen(payload) ||
                        memcmp(entry.box.data.bytes.start, payload,
                               entry.box.len) != 0) {
                        ERR("op %d: duplicate element %d mismatch "
                            "(want id %" PRId64 ")",
                            op, i, shadow[i]);
                        soakFailures++;
                    }
                }
            }

            tierSeen[_multilistType(ml)] = true;
            if (shadowCount > soakMaxCount) {
                soakMaxCount = shadowCount;
            }

            /* Compression metadata rides the tagged pointer's top bits
             * and must survive every tier promotion */
            if (COMPRESS_DEPTH(ml) != wantDepth ||
                COMPRESS_LIMIT(ml) != wantLimit) {
                ERR("op %d: compress metadata lost (depth %u limit %u, "
                    "want %u/%u, tier %d)",
                    op, (unsigned)COMPRESS_DEPTH(ml),
                    (unsigned)COMPRESS_LIMIT(ml), (unsigned)wantDepth,
                    (unsigned)wantLimit, (int)_multilistType(ml));
                soakFailures++;
            }

            /* Per-op count check plus the Medium structural invariant
             * (F0 nonempty whenever the list is nonempty) so the op
             * that breaks state is named, not a later victim op */
            if ((int32_t)multilistCount(ml) != shadowCount) {
                ERR("op %d roll %d: count %zu != shadow %d", op, (int)roll,
                    multilistCount(ml), shadowCount);
                soakFailures++;
            }
            if (_multilistType(ml) == MULTILIST_TYPE_MEDIUM) {
                multilistMedium *mm = mlm(ml);
                if (flexCount(mm->fl[0]) == 0 && flexCount(mm->fl[1]) > 0) {
                    ERR("op %d roll %d: Medium invariant broken "
                        "(F0 empty, F1 has %zu)",
                        op, (int)roll, flexCount(mm->fl[1]));
                    soakFailures++;
                }
            }

            /* Full-content verification every 250 ops and near tier
             * boundaries (cheap enough at this scale) */
            if (op % 250 == 0 || soakFailures) {
                if ((int32_t)multilistCount(ml) != shadowCount) {
                    ERR("op %d: count %zu != shadow %d", op, multilistCount(ml),
                        shadowCount);
                    soakFailures++;
                }
                for (int32_t i = 0; i < shadowCount && soakFailures == 0; i++) {
                    FUZZ_PAYLOAD(shadow[i]);
                    multilistEntry entry = {0};
                    if (!multilistIndexGet(ml, s0, i, &entry) ||
                        entry.box.len != strlen(payload) ||
                        memcmp(entry.box.data.bytes.start, payload,
                               entry.box.len) != 0) {
                        ERR("op %d: element %d mismatch (want id %" PRId64 ")",
                            op, i, shadow[i]);
                        soakFailures++;
                    }
                }
            }
        }

        printf("    soak: maxCount=%d finalCount=%d finalTier=%d "
               "finalBytes=%zu limit=%u\n",
               soakMaxCount, shadowCount, (int)_multilistType(ml),
               multilistBytes(ml), (unsigned)COMPRESS_LIMIT(ml));

        if (!tierSeen[MULTILIST_TYPE_SMALL] ||
            !tierSeen[MULTILIST_TYPE_MEDIUM] ||
            !tierSeen[MULTILIST_TYPE_FULL]) {
            ERR("Soak tier coverage incomplete: small=%d medium=%d full=%d",
                tierSeen[MULTILIST_TYPE_SMALL], tierSeen[MULTILIST_TYPE_MEDIUM],
                tierSeen[MULTILIST_TYPE_FULL]);
        }
#undef FUZZ_PAYLOAD
#undef FUZZ_NEXT
    }

    TEST("tier Rewind and RewindTail restart every tier's iterator") {
        /* Each tier's own Rewind / RewindTail, called mid-walk and after
         * an exhausted walk, restarts at the head / tail and then yields
         * every element in order.  On Full the iterator keeps its state
         * pair and readOnly mode, and a writable iterator's open node is
         * released (the list must read back intact afterwards). */
        const struct {
            const char *name;
            flexCapSizeLimit limit;
            uint32_t depth;
            uint32_t count;
            multilistType wantType;
        } configs[] = {
            {"Small", FLEX_CAP_LEVEL_65536, 0, 60, MULTILIST_TYPE_SMALL},
            {"Medium", FLEX_CAP_LEVEL_512, 0, 40, MULTILIST_TYPE_MEDIUM},
            {"Full depth 0", FLEX_CAP_LEVEL_512, 0, 300, MULTILIST_TYPE_FULL},
            {"Full depth 1", FLEX_CAP_LEVEL_512, 1, 600, MULTILIST_TYPE_FULL},
        };

        for (size_t c = 0; c < sizeof(configs) / sizeof(*configs); c++) {
            for (int ro = 0; ro < 2; ro++) {
                const uint32_t n = configs[c].count;
                cleaner(multilistFree) multilist *ml =
                    multilistNew(configs[c].limit, configs[c].depth);
                char buf[MLR_LEN + 1];
                for (uint32_t i = 0; i < n; i++) {
                    const databox box = mlrBox(i, buf);
                    multilistPushByTypeTail(&ml, s0, &box);
                }

                if (_multilistType(ml) != configs[c].wantType ||
                    (configs[c].depth > 0 &&
                     multilistFullBytesActual(mlf(ml)) >= multilistBytes(ml))) {
                    ERR("%s: landed tier %d, or no node was compressed",
                        configs[c].name, (int)_multilistType(ml));
                    continue;
                }

                multilistIterator iter;
                memset(&iter, 0, sizeof(iter));
                multilistIteratorInit(ml, s, &iter, true, ro);
                const char *why = mlrWalk(&iter, n, true, n / 3);
                if (!why) {
                    mlrTierRewind(ml, &iter, false);
                    why = mlrWalk(&iter, n, true, n);
                }

                if (!why) {
                    mlrTierRewind(ml, &iter, true);
                    why = mlrWalk(&iter, n, false, n / 2);
                }

                if (!why) {
                    mlrTierRewind(ml, &iter, true);
                    why = mlrWalk(&iter, n, false, n);
                }

                if (!why) {
                    mlrTierRewind(ml, &iter, false);
                    why = mlrWalk(&iter, n, true, n);
                }

                if (!why && configs[c].wantType == MULTILIST_TYPE_FULL &&
                    (iter.readOnly != (bool)ro || iter.state[0] != s0 ||
                     iter.state[1] != s1)) {
                    why = "the iterator lost its state pair or readOnly mode";
                }

                multilistIteratorRelease(&iter);

                if (!why) {
                    multilistIteratorInit(ml, s, &iter, true, true);
                    why = mlrWalk(&iter, n, true, n);
                    multilistIteratorRelease(&iter);
                    if (!why && multilistCount(ml) != n) {
                        why = "the count changed";
                    }
                }

                if (why) {
                    ERR("%s, %s: %s", configs[c].name,
                        ro ? "read-only" : "writable", why);
                }
            }
        }
    }

    TEST("tier iterator release, mid-walk and after the walk, every tier") {
        /* Each tier's own release, mid-walk and after an exhausted walk.
         * Small and Medium hold nothing open, so release is a no-op: the
         * iterator is byte-for-byte unchanged and keeps walking.  Full
         * closes a writable iterator's open node.  On every tier the list
         * must read back intact and the iterator must rewind cleanly. */
        const struct {
            const char *name;
            flexCapSizeLimit limit;
            uint32_t depth;
            uint32_t count;
            multilistType wantType;
        } configs[] = {
            {"Small", FLEX_CAP_LEVEL_65536, 0, 60, MULTILIST_TYPE_SMALL},
            {"Medium", FLEX_CAP_LEVEL_512, 0, 40, MULTILIST_TYPE_MEDIUM},
            {"Full depth 0", FLEX_CAP_LEVEL_512, 0, 300, MULTILIST_TYPE_FULL},
            {"Full depth 1", FLEX_CAP_LEVEL_512, 1, 600, MULTILIST_TYPE_FULL},
        };

        for (size_t c = 0; c < sizeof(configs) / sizeof(*configs); c++) {
            for (int ro = 0; ro < 2; ro++) {
                const uint32_t n = configs[c].count;
                const bool full = configs[c].wantType == MULTILIST_TYPE_FULL;
                cleaner(multilistFree) multilist *ml =
                    multilistNew(configs[c].limit, configs[c].depth);
                char buf[MLR_LEN + 1];
                for (uint32_t i = 0; i < n; i++) {
                    const databox box = mlrBox(i, buf);
                    multilistPushByTypeTail(&ml, s0, &box);
                }

                if (_multilistType(ml) != configs[c].wantType) {
                    ERR("%s: landed tier %d", configs[c].name,
                        (int)_multilistType(ml));
                    continue;
                }

                multilistIterator iter;
                memset(&iter, 0, sizeof(iter));
                multilistIteratorInit(ml, s, &iter, true, ro);
                const char *why = mlrWalk(&iter, n, true, n / 2);
                if (!why) {
                    multilistIterator before;
                    memcpy(&before, &iter, sizeof(iter));
                    mlrTierRelease(ml, &iter);
                    if (!full) {
                        /* a no-op: unchanged, and the walk goes on */
                        multilistEntry entry;
                        if (memcmp(&before, &iter, sizeof(iter)) != 0) {
                            why = "release changed the iterator";
                        } else if (!multilistNext(&iter, &entry) ||
                                   !mlrIs(&entry.box, n / 2)) {
                            why = "the walk did not continue after release";
                        }
                    }
                }

                if (!why) {
                    mlrTierRewind(ml, &iter, false);
                    why = mlrWalk(&iter, n, true, n);
                }

                if (!why) {
                    /* release after an exhausted walk */
                    mlrTierRelease(ml, &iter);
                    mlrTierRewind(ml, &iter, true);
                    why = mlrWalk(&iter, n, false, n);
                    mlrTierRelease(ml, &iter);
                }

                if (!why) {
                    multilistIteratorInit(ml, s, &iter, true, true);
                    why = mlrWalk(&iter, n, true, n);
                    mlrTierRelease(ml, &iter);
                    if (!why && multilistCount(ml) != n) {
                        why = "the count changed";
                    }
                }

                if (why) {
                    ERR("%s, %s: %s", configs[c].name,
                        ro ? "read-only" : "writable", why);
                }
            }
        }
    }

    TEST("wide: normal, extended and mixed data through every read path, "
         "all tiers") {
        /* Every destination entry, slot and iterator is filled with 0xFF
         * before each read, so a decode that trusts the destination's
         * stale 'big' bit or pointer writes through garbage (ASAN) and one
         * that skips the payload reads back type-only.  Next and the Wide
         * reads must give wide values whole, in the iterator's or the
         * caller's slot; Index and IndexNear must give them deferred.
         * Pops go into a zeroed plain box (heap payload) or a poisoned
         * DATABOX_BIG_INIT'd databoxBig (inline payload) by turns. */
        const struct {
            const char *name;
            flexCapSizeLimit limit;
            uint32_t depth;
            uint32_t count;
            multilistType wantType;
        } configs[] = {
            {"Small", FLEX_CAP_LEVEL_65536, 0, 60, MULTILIST_TYPE_SMALL},
            {"Medium", FLEX_CAP_LEVEL_512, 0, 60, MULTILIST_TYPE_MEDIUM},
            {"Full depth 0", FLEX_CAP_LEVEL_128, 0, 400, MULTILIST_TYPE_FULL},
            {"Full depth 1", FLEX_CAP_LEVEL_512, 1, 600, MULTILIST_TYPE_FULL},
        };

        for (size_t cd = 0;
             cd < MLW_DATA_COUNT * (sizeof(configs) / sizeof(*configs)); cd++) {
            const mlwData data = (mlwData)(cd % MLW_DATA_COUNT);
            const size_t c = cd / MLW_DATA_COUNT;
            char name[64];
            snprintf(name, sizeof(name), "%s %s", mlwDataName[data],
                     configs[c].name);
            mlwVal want[MLW_MAX];
            size_t n = 0;
            uint32_t nextId = 0;
            cleaner(multilistFree) multilist *ml =
                multilistNew(configs[c].limit, configs[c].depth);

/* The value for a step that wants a wide element: 'wide' itself, except
 * in normal data, which takes the next normal element instead. */
#define MLW_PICK(wide)                                                         \
    (data == MLW_DATA_NORMAL ? mlwGenIn(data, nextId++) : (wide))

            /* Mostly tail pushes, every seventh at the head, then a wide
             * tail so Rotate moves a wide element first. */
            for (uint32_t i = 0; i < configs[c].count; i++) {
                const mlwVal v = mlwGenIn(data, nextId++);
                const bool head = i % 7 == 6;
                mlwPush(&ml, s0, &v, head);
                mlwInsertAt(want, &n, head ? 0 : n, v);
            }

            const mlwVal wideTail = MLW_PICK(mlwBoundary(7));
            mlwPush(&ml, s0, &wideTail, false);
            mlwInsertAt(want, &n, n, wideTail);

            if (_multilistType(ml) != configs[c].wantType) {
                ERR("%s: landed tier %d, wanted %d", name,
                    (int)_multilistType(ml), (int)configs[c].wantType);
            }

            if (configs[c].depth > 0 &&
                multilistFullBytesActual(mlf(ml)) >= multilistBytes(ml)) {
                ERR("%s: no node was compressed", name);
            }

            err += mlwVerify(ml, s, want, n, name);

            /* IteratorInitAtIdx, forward and reverse. */
            const mlOffsetId starts[] = {0, (mlOffsetId)n / 3, -1,
                                         -(mlOffsetId)n / 2};
            for (size_t k = 0; k < sizeof(starts) / sizeof(*starts); k++) {
                err +=
                    mlwVerifyFrom(ml, s, want, n, starts[k], true, true, name);
                err +=
                    mlwVerifyFrom(ml, s, want, n, starts[k], false, true, name);
            }

            /* A struct copy of an iterator has a slot of its own: reading
             * through the copy leaves a box read through the original
             * intact, and each box points into its own iterator. */
            for (size_t at = 0; at < n; at += 3) {
                multilistIterator iter;
                memset(&iter, 0xFF, sizeof(iter));
                if (!multilistIteratorInitAtIdx(ml, s, &iter, (mlOffsetId)at,
                                                true, true)) {
                    ERR("%s: IteratorInitAtIdx(%zu) failed", name, at);
                    break;
                }

                multilistIterator copy = iter;
                multilistEntry a;
                multilistEntry b;
                memset(&a, 0xFF, sizeof(a));
                memset(&b, 0xFF, sizeof(b));
                const bool okA = multilistNext(&iter, &a);
                const bool okB = multilistNext(&copy, &b);
                const char *why = okA && okB
                                      ? mlwCheck(&b.box, &want[at], &copy.wide)
                                      : "Next failed";
                if (!why) {
                    why = mlwCheck(&a.box, &want[at], &iter.wide);
                }

                if (why) {
                    ERR("%s: iterator copy at %zu: %s", name, at, why);
                    break;
                }

                multilistIteratorRelease(&iter);
            }

            /* Out of range: the Wide reads fail like the plain ones. */
            {
                multilistEntry entry;
                databoxWide slot;
                if (multilistIndexWide(ml, s0, (mlOffsetId)n, &entry, &slot) ||
                    multilistIndexWide(ml, s0, -(mlOffsetId)n - 1, &entry,
                                       &slot)) {
                    ERR("%s: IndexWide found an index out of range", name);
                }
            }

            /* IndexNear from anchors on both sides of every target.  The
             * anchors open their nodes with s1 so the targets' s0 decodes
             * cannot overwrite an anchor's open view. */
            const size_t anchorAt[] = {0, n / 4, n / 2, 3 * n / 4, n - 1};
            for (size_t a = 0; a < sizeof(anchorAt) / sizeof(*anchorAt); a++) {
                multilistEntry anchor;
                memset(&anchor, 0xFF, sizeof(anchor));
                if (!multilistIndexGet(ml, s1, (mlOffsetId)anchorAt[a],
                                       &anchor)) {
                    ERR("%s: anchor %zu not found", name, anchorAt[a]);
                    continue;
                }

                for (mlOffsetId t = -(mlOffsetId)n; t < (mlOffsetId)n; t += 7) {
                    const size_t idx =
                        t < 0 ? (size_t)((mlOffsetId)n + t) : (size_t)t;
                    multilistEntry got;
                    memset(&got, 0xFF, sizeof(got));
                    const char *why =
                        multilistIndexNearGet(ml, s0, t, &anchor,
                                              (mlOffsetId)anchorAt[a], &got)
                            ? mlwCheckRead(&got.box, &want[idx], NULL,
                                           MLW_READ_DEFERRED)
                            : "not found";
                    if (why) {
                        ERR("%s: IndexNear(%" PRId64 ") from %zu: %s", name, t,
                            anchorAt[a], why);
                        break;
                    }

                    databoxWide slot;
                    memset(&slot, 0xFF, sizeof(slot));
                    memset(&got, 0xFF, sizeof(got));
                    why = multilistIndexNearWide(ml, s0, t, &anchor,
                                                 (mlOffsetId)anchorAt[a], &got,
                                                 &slot)
                              ? mlwCheck(&got.box, &want[idx], &slot)
                              : "not found";
                    if (why) {
                        ERR("%s: IndexNearWide(%" PRId64 ") from %zu: %s", name,
                            t, anchorAt[a], why);
                        break;
                    }
                }

                const char *why = mlwCheckRead(&anchor.box, &want[anchorAt[a]],
                                               NULL, MLW_READ_DEFERRED);
                if (why) {
                    ERR("%s: anchor %zu after IndexNear: %s", name, anchorAt[a],
                        why);
                }
            }

            /* Duplicate. */
            {
                cleaner(multilistFree) multilist *dup = multilistDuplicate(ml);
                err += mlwVerify(dup, s, want, n, name);
            }

            /* Rotate: the first move is the wide tail. */
            for (int r = 0; r < 5; r++) {
                const mlwVal moved = want[n - 1];
                multilistRotate(ml, s);
                mlwRemoveAt(want, &n, n - 1);
                mlwInsertAt(want, &n, 0, moved);
            }

            err += mlwVerify(ml, s, want, n, name);

            /* Replace with wide values (normal values in normal data). */
            const size_t replaceAt[] = {0, n / 2, n - 1};
            for (size_t k = 0; k < sizeof(replaceAt) / sizeof(*replaceAt);
                 k++) {
                const mlwVal v = MLW_PICK(mlwBoundary((uint32_t)k + 4));
                databoxBig storage;
                char buf[64];
                multilistReplaceByTypeAtIndex(
                    &ml, s0, (mlOffsetId)replaceAt[k],
                    mlwBox(&v, &storage, buf, sizeof(buf)));
                want[replaceAt[k]] = v;
            }

            err += mlwVerify(ml, s, want, n, name);

            /* InsertBefore / InsertAfter wide values (normal values in
             * normal data) at both ends and the middle. */
            for (size_t k = 0; k < 6; k++) {
                const size_t at = (k / 2) == 0   ? 0
                                  : (k / 2) == 1 ? n / 2
                                                 : n - 1;
                const bool after = k % 2;
                const uint32_t wideId = 5 * nextId + 3;
                const mlwVal v = MLW_PICK(mlwGen(wideId));
                nextId++;
                multilistEntry entry;
                memset(&entry, 0xFF, sizeof(entry));
                if (!multilistIndexGet(ml, s0, (mlOffsetId)at, &entry)) {
                    ERR("%s: insert anchor %zu not found", name, at);
                    continue;
                }

                databoxBig storage;
                char buf[64];
                const databox *box = mlwBox(&v, &storage, buf, sizeof(buf));
                if (after) {
                    multilistInsertByTypeAfter(&ml, s, &entry, box);
                } else {
                    multilistInsertByTypeBefore(&ml, s, &entry, box);
                }

                mlwInsertAt(want, &n, after ? at + 1 : at, v);
            }

            err += mlwVerify(ml, s, want, n, name);

            /* DelEntry mid-iteration: forward, then reverse. */
            for (int pass = 0; pass < 2; pass++) {
                const bool forward = pass == 0;
                multilistIterator iter;
                multilistIteratorInit(ml, s, &iter, forward, false);
                size_t idx = forward ? 0 : n - 1;
                size_t visited = 0;
                const size_t total = n;
                multilistEntry entry;
                for (;;) {
                    memset(&entry, 0xFF, sizeof(entry));
                    if (!multilistNext(&iter, &entry)) {
                        break;
                    }

                    const char *why =
                        visited < total
                            ? mlwCheck(&entry.box, &want[idx], &iter.wide)
                            : "iterated past the end";
                    if (why) {
                        ERR("%s: DelEntry pass %d at %zu: %s", name, pass, idx,
                            why);
                        break;
                    }

                    if (visited % (forward ? 4 : 3) == 1) {
                        multilistDelEntry(&iter, &entry);
                        mlwRemoveAt(want, &n, idx);
                        if (forward) {
                            idx--;
                        }
                    }

                    visited++;
                    idx = forward ? idx + 1 : idx - 1;
                }

                multilistIteratorRelease(&iter);
                if (visited != total) {
                    ERR("%s: DelEntry pass %d visited %zu of %zu", name, pass,
                        visited, total);
                }

                err += mlwVerify(ml, s, want, n, name);
            }

            /* NewFromFlex over the same elements, then pushes that make it
             * re-tier. */
            {
                cleaner(flexFree) flex *fl = flexNew();
                for (size_t i = 0; i < n; i++) {
                    databoxBig storage;
                    char buf[64];
                    flexPushByType(&fl,
                                   mlwBox(&want[i], &storage, buf, sizeof(buf)),
                                   FLEX_ENDPOINT_TAIL);
                }

                mlwVal copy[MLW_MAX];
                size_t copyN = n;
                memcpy(copy, want, n * sizeof(*want));
                cleaner(multilistFree) multilist *fromFlex =
                    multilistNewFromFlex(configs[c].limit, configs[c].depth,
                                         fl);
                fl = NULL; /* the list owns it now */
                err += mlwVerify(fromFlex, s, copy, copyN, name);

                const mlwVal head = MLW_PICK(mlwBoundary(8));
                const mlwVal tail = MLW_PICK(mlwBoundary(6));
                mlwPush(&fromFlex, s0, &head, true);
                mlwPush(&fromFlex, s0, &tail, false);
                mlwInsertAt(copy, &copyN, 0, head);
                mlwInsertAt(copy, &copyN, copyN, tail);
                err += mlwVerify(fromFlex, s, copy, copyN, name);
            }

            /* Pop from both ends into both destination kinds until empty;
             * verify the remainder halfway. */
            const size_t half = n / 2;
            for (size_t k = 0; n > 0; k++) {
                const bool fromTail = k % 3 != 0;
                const bool inlineDst = k % 2 == 0;
                const mlwVal expect = fromTail ? want[n - 1] : want[0];
                databoxBig dst;
                if (inlineDst) {
                    memset(&dst, 0xFF, sizeof(dst));
                    DATABOX_BIG_INIT(&dst);
                } else {
                    memset(&dst, 0, sizeof(dst));
                }

                if (!multilistPop(&ml, s0, (databox *)&dst, fromTail)) {
                    ERR("%s: pop %zu failed with %zu left", name, k, n);
                    break;
                }

                const char *why =
                    mlwCheckRead((const databox *)&dst, &expect,
                                 inlineDst ? dst.extra : NULL, MLW_READ_OWNED);
                if (!why && !inlineDst && mlwIsWide(&expect) &&
                    !dst.allocated) {
                    why = "wide pop into a plain box is not heap-owned";
                }

                if (!why && expect.isStr) {
                    /* Up to 8 bytes embedded with nothing to free, longer
                     * ones a heap copy. */
                    const bool embed = databoxLen((const databox *)&dst) <=
                                       sizeof(dst.data.bytes.embed8);
                    if (embed &&
                        (dst.type != DATABOX_BYTES_EMBED || dst.allocated)) {
                        why = "popped short string is not embedded";
                    } else if (!embed &&
                               (dst.type != DATABOX_BYTES || !dst.allocated)) {
                        why = "popped long string is not an owned copy";
                    }
                }

                if (!why && !mlwIsWide(&expect) && !expect.isStr &&
                    dst.allocated) {
                    why = "popped integer claims to own memory";
                }

                if (why) {
                    ERR("%s: pop %zu (%s, %s): %s", name, k,
                        fromTail ? "tail" : "head",
                        inlineDst ? "databoxBig" : "plain box", why);
                }

                databoxFreeData((databox *)&dst);
                mlwRemoveAt(want, &n, fromTail ? n - 1 : 0);
                if (n == half) {
                    err += mlwVerify(ml, s, want, n, name);
                }
            }

            if (multilistCount(ml) != 0) {
                ERR("%s: %zu elements left after popping all", name,
                    multilistCount(ml));
            }
#undef MLW_PICK
        }
    }

    TEST("multilistDump: byte-identical to decode and re-encode, all tiers") {
        const struct {
            const char *name;
            flexCapSizeLimit limit;
            uint32_t depth;
            uint32_t count;
            multilistType wantType;
        } configs[] = {
            {"Small", FLEX_CAP_LEVEL_65536, 0, 60, MULTILIST_TYPE_SMALL},
            {"Medium", FLEX_CAP_LEVEL_512, 0, 60, MULTILIST_TYPE_MEDIUM},
            {"Full depth 0", FLEX_CAP_LEVEL_128, 0, 400, MULTILIST_TYPE_FULL},
            {"Full depth 1", FLEX_CAP_LEVEL_512, 1, 600, MULTILIST_TYPE_FULL},
        };

        for (size_t cd = 0;
             cd < MLW_DATA_COUNT * (sizeof(configs) / sizeof(*configs)); cd++) {
            const mlwData data = (mlwData)(cd % MLW_DATA_COUNT);
            const size_t c = cd / MLW_DATA_COUNT;
            char name[64];
            snprintf(name, sizeof(name), "%s %s", mlwDataName[data],
                     configs[c].name);
            mlwVal want[MLW_MAX];
            size_t n = 0;
            cleaner(multilistFree) multilist *ml =
                multilistNew(configs[c].limit, configs[c].depth);

            {
                /* An empty list dumps to an empty flex. */
                cleaner(flexFree) flex *dump = multilistDump(ml, s0);
                cleaner(flexFree) flex *empty = flexNew();
                if (flexBytes(dump) != flexBytes(empty) ||
                    memcmp(dump, empty, flexBytes(empty))) {
                    ERR("%s: empty list did not dump to an empty flex", name);
                }
            }

            for (uint32_t i = 0; i < configs[c].count; i++) {
                const mlwVal v = mlwGenIn(data, i);
                const bool head = i % 7 == 6;
                mlwPush(&ml, s0, &v, head);
                mlwInsertAt(want, &n, head ? 0 : n, v);
            }

            /* Mutate the way a live list would be: rotate, replace, insert,
             * and delete a range. */
            for (int r = 0; r < 3; r++) {
                const mlwVal moved = want[n - 1];
                multilistRotate(ml, s);
                mlwRemoveAt(want, &n, n - 1);
                mlwInsertAt(want, &n, 0, moved);
            }

            /* Replace and insert wide values, or normal ones in normal
             * data. */
            {
                const mlwVal v = data == MLW_DATA_NORMAL
                                     ? mlwGenIn(data, configs[c].count)
                                     : mlwBoundary(3);
                databoxBig storage;
                char buf[64];
                multilistReplaceByTypeAtIndex(
                    &ml, s0, (mlOffsetId)(n / 3),
                    mlwBox(&v, &storage, buf, sizeof(buf)));
                want[n / 3] = v;
            }

            {
                const mlwVal v = data == MLW_DATA_NORMAL
                                     ? mlwGenIn(data, configs[c].count + 1)
                                     : mlwBoundary(5);
                multilistEntry entry;
                memset(&entry, 0xFF, sizeof(entry));
                if (multilistIndexGet(ml, s0, (mlOffsetId)(n / 2), &entry)) {
                    databoxBig storage;
                    char buf[64];
                    multilistInsertByTypeAfter(
                        &ml, s, &entry, mlwBox(&v, &storage, buf, sizeof(buf)));
                    mlwInsertAt(want, &n, n / 2 + 1, v);
                } else {
                    ERR("%s: insert anchor not found", name);
                }
            }

            const size_t delAt = n / 4;
            multilistDelRange(&ml, s0, (mlOffsetId)delAt, 3);
            for (int k = 0; k < 3; k++) {
                mlwRemoveAt(want, &n, delAt);
            }

            if (_multilistType(ml) != configs[c].wantType) {
                ERR("%s: landed tier %d, wanted %d", name,
                    (int)_multilistType(ml), (int)configs[c].wantType);
            }

            /* The reference is the element-at-a-time decode and re-encode. */
            cleaner(flexFree) flex *ref = flexNew();
            {
                multilistIterator iter;
                multilistEntry entry;
                multilistIteratorInitForwardReadOnly(ml, s, &iter);
                while (multilistNext(&iter, &entry)) {
                    flexPushByType(&ref, &entry.box, FLEX_ENDPOINT_TAIL);
                }

                multilistIteratorRelease(&iter);
            }

            cleaner(flexFree) flex *dump = multilistDump(ml, s0);
            if (flexBytes(dump) != flexBytes(ref) ||
                memcmp(dump, ref, flexBytes(ref))) {
                ERR("%s: dump (%zu bytes) differs from decode and re-encode "
                    "(%zu bytes)",
                    name, flexBytes(dump), flexBytes(ref));
            }

            /* Check the dump's elements against the oracle too. */
            if (flexCount(dump) != n) {
                ERR("%s: dump holds %zu elements, want %zu", name,
                    flexCount(dump), n);
            } else {
                flexEntry *fe = flexHead(dump);
                for (size_t i = 0; i < n; i++) {
                    databox got;
                    databoxWide slot;
                    memset(&got, 0xFF, sizeof(got));
                    memset(&slot, 0xFF, sizeof(slot));
                    flexGetByTypeInto(fe, &got, &slot);
                    const char *why = mlwCheck(&got, &want[i], &slot);
                    if (why) {
                        ERR("%s: dump element %zu: %s", name, i, why);
                        break;
                    }

                    fe = flexNext(dump, fe);
                }
            }

            /* Dumping reads only: the list is unchanged and a second dump
             * is identical. */
            err += mlwVerify(ml, s, want, n, name);
            cleaner(flexFree) flex *again = multilistDump(ml, s0);
            if (flexBytes(again) != flexBytes(dump) ||
                memcmp(again, dump, flexBytes(dump))) {
                ERR("%s: a second dump differs from the first", name);
            }

            /* A list emptied in place (it keeps its tier) dumps empty. */
            multilistDelRange(&ml, s0, 0, (int64_t)n);
            cleaner(flexFree) flex *emptied = multilistDump(ml, s0);
            if (flexCount(emptied) != 0 || multilistCount(ml) != 0) {
                ERR("%s: emptied list dumped %zu elements", name,
                    flexCount(emptied));
            }
        }
    }

    TEST("NewFromDump rebuilds every tier; later edits agree") {
        /* Sizes reaching Small, Medium and Full, with and without
         * compression: the rebuild dumps the same bytes, and the same
         * pops and pushes give the same results on both */
        static const int64_t dumpSizes[] = {0, 5, 120, 400, 5000};
        static const uint32_t compressDepths[] = {0, 2};
        cleaner(mflexStateFree) mflexState *scratch = mflexStateCreate();
        for (size_t d = 0; d < COUNT_ARRAY(compressDepths); d++) {
            const uint32_t compressDepth = compressDepths[d];
            for (size_t z = 0; z < COUNT_ARRAY(dumpSizes); z++) {
                cleaner(multilistFree) multilist *orig =
                    multilistNew(FLEX_CAP_LEVEL_512, compressDepth);
                for (int64_t i = 0; i < dumpSizes[z]; i++) {
                    const databox v = DATABOX_SIGNED(i * 7 - 3000);
                    multilistPushByTypeTail(&orig, scratch, &v);
                }
                cleaner(multilistFree) multilist *rebuilt =
                    multilistNewFromDump(FLEX_CAP_LEVEL_512, compressDepth,
                                         scratch, multilistDump(orig, scratch));
                for (int round = 0; round < 2; round++) {
                    cleaner(flexFree) flex *a = multilistDump(orig, scratch);
                    cleaner(flexFree) flex *b = multilistDump(rebuilt, scratch);
                    if (flexBytes(a) != flexBytes(b) ||
                        memcmp(a, b, flexBytes(a)) != 0) {
                        ERR("size %" PRId64 " depth %u round %d: dumps "
                            "differ",
                            dumpSizes[z], compressDepth, round);
                        break;
                    }
                    for (int64_t k = 0; k < 40; k++) {
                        databox x = {{0}};
                        databox y = {{0}};
                        const bool tail = k & 1;
                        const bool px = multilistPop(&orig, scratch, &x, tail);
                        const bool py =
                            multilistPop(&rebuilt, scratch, &y, tail);
                        if (px != py || (px && !databoxEqual(&x, &y))) {
                            ERR("size %" PRId64 ": pop %" PRId64 " differs",
                                dumpSizes[z], k);
                        }
                        databoxFreeData(&x);
                        databoxFreeData(&y);
                        const databox v = DATABOX_SIGNED(k * 1000);
                        multilistPushByTypeHead(&orig, scratch, &v);
                        multilistPushByTypeHead(&rebuilt, scratch, &v);
                    }
                }
            }
        }
    }

    if (!err) {
        printf("ALL TESTS PASSED!\n");
    } else {
        ERR("Sorry, not all tests passed!  In fact, %d tests failed.", err);
    }

    return err;
}
#endif /* DATAKIT_TEST */
