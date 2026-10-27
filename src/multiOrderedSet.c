#include "multiOrderedSet.h"

#include "multiOrderedSetSmall.h"
#include "multiOrderedSetSmallInternal.h"

#include "multiOrderedSetMedium.h"
#include "multiOrderedSetMediumInternal.h"

#include "multiOrderedSetFull.h"
#include "multiOrderedSetFullInternal.h"

#include "cleaner.h"

#include <float.h> /* DBL_MAX (tests) */
#include <math.h>  /* isfinite */

/* Fixed-width key/member buffer used by test scratch allocations below. */
typedef char mosKeyBuf32[32];
defcleanerName(mosKeyBuf32 *, zfree, KeyBuf32);
defcleanerName(int64_t *, zfree, Int64);
defcleaner(multiOrderedSet *, multiOrderedSetFree);

/* ====================================================================
 * Databox Helpers
 * ==================================================================== */

/* Convert any numeric databox to double (0.0 for a non-numeric box) */
static inline double mosDataboxToDouble(const databox *box) {
    double value;
    return mosScoreToDouble(box, &value) ? value : 0.0;
}

/* ====================================================================
 * Pointer Tagging Setup
 * ==================================================================== */
#define PTRLIB_BITS_LOW 2
#define PTRLIB_BITS_HIGH 16
#include "ptrlib.h"
#include "ptrlibCompress.h"

#define mosType_(mos) ((multiOrderedSetType)_PTRLIB_TYPE(mos))
#define MOS_USE_(mos) _PTRLIB_TOP_USE_ALL(mos)
#define MOS_TAG_(mos, type) ((multiOrderedSet *)_PTRLIB_TAG(mos, type))

#define moss(m) ((multiOrderedSetSmall *)MOS_USE_(m))
#define mosm(m) ((multiOrderedSetMedium *)MOS_USE_(m))
#define mosf(m) ((multiOrderedSetFull *)MOS_USE_(m))

/* ====================================================================
 * Computed Goto Dispatch Macros
 * ==================================================================== */
#define MOS_USE_COMPUTED_GOTO_JUMPING 1

#if MOS_USE_COMPUTED_GOTO_JUMPING
#define MOS_SETUP_LABELS(m)                                                    \
    __label__ mosSmall;                                                        \
    __label__ mosMedium;                                                       \
    __label__ mosFull;                                                         \
    static const void **jumper[4] = {nullptr, &&mosSmall, &&mosMedium,         \
                                     &&mosFull};                               \
    goto *jumper[mosType_(m)]
#endif

#if !MOS_USE_COMPUTED_GOTO_JUMPING
#define MOS_(ret, m, func, ...)                                                \
    do {                                                                       \
        switch (mosType_(m)) {                                                 \
        case MOS_TYPE_SMALL:                                                   \
            ret multiOrderedSetSmall##func(moss(m), __VA_ARGS__);              \
            break;                                                             \
        case MOS_TYPE_MEDIUM:                                                  \
            ret multiOrderedSetMedium##func(mosm(m), __VA_ARGS__);             \
            break;                                                             \
        case MOS_TYPE_FULL:                                                    \
            ret multiOrderedSetFull##func(mosf(m), __VA_ARGS__);               \
            break;                                                             \
        default:                                                               \
            assert(false);                                                     \
            unreachable();                                                     \
        }                                                                      \
    } while (0)
#else
#define MOS_(ret, m, func, ...)                                                \
    do {                                                                       \
        MOS_SETUP_LABELS(m);                                                   \
        do {                                                                   \
        mosSmall:                                                              \
            ret multiOrderedSetSmall##func(moss(m), __VA_ARGS__);              \
            break;                                                             \
        mosMedium:                                                             \
            ret multiOrderedSetMedium##func(mosm(m), __VA_ARGS__);             \
            break;                                                             \
        mosFull:                                                               \
            ret multiOrderedSetFull##func(mosf(m), __VA_ARGS__);               \
            break;                                                             \
        } while (0);                                                           \
    } while (0)
#endif

#define MOS_NORETURN(m, func, ...) MOS_(, m, func, __VA_ARGS__)
#define MOS_RETURN(m, func, ...) MOS_(return, m, func, __VA_ARGS__)
#define MOS_SETRESULT(var, m, func, ...) MOS_(var =, m, func, __VA_ARGS__)

/* Single-argument dispatch macros */
#if !MOS_USE_COMPUTED_GOTO_JUMPING
#define MOS_SINGLE_(ret, m, func)                                              \
    do {                                                                       \
        switch (mosType_(m)) {                                                 \
        case MOS_TYPE_SMALL:                                                   \
            ret multiOrderedSetSmall##func(moss(m));                           \
            break;                                                             \
        case MOS_TYPE_MEDIUM:                                                  \
            ret multiOrderedSetMedium##func(mosm(m));                          \
            break;                                                             \
        case MOS_TYPE_FULL:                                                    \
            ret multiOrderedSetFull##func(mosf(m));                            \
            break;                                                             \
        default:                                                               \
            assert(false);                                                     \
            unreachable();                                                     \
        }                                                                      \
    } while (0)
#else
#define MOS_SINGLE_(ret, m, func)                                              \
    do {                                                                       \
        MOS_SETUP_LABELS(m);                                                   \
        do {                                                                   \
        mosSmall:                                                              \
            ret multiOrderedSetSmall##func(moss(m));                           \
            break;                                                             \
        mosMedium:                                                             \
            ret multiOrderedSetMedium##func(mosm(m));                          \
            break;                                                             \
        mosFull:                                                               \
            ret multiOrderedSetFull##func(mosf(m));                            \
            break;                                                             \
        } while (0);                                                           \
    } while (0)
#endif

#define MOS_SINGLE_NORETURN(m, func) MOS_SINGLE_(, m, func)
#define MOS_SINGLE_RETURN(m, func) MOS_SINGLE_(return, m, func)

/* ====================================================================
 * Tier Promotion
 * ==================================================================== */

/* Minimum entry count for promotion (must have 2 entries to split) */
static constexpr int MOS_MIN_ENTRIES_FOR_PROMOTION = 2;

DK_INLINE_ALWAYS void mosUpgradeIfNecessary(multiOrderedSet **m) {
    const uint32_t limit = COMPRESS_LIMIT(*m);
    const uint32_t depth = COMPRESS_DEPTH(*m); /* carried into every tier */
    const multiOrderedSetType type = mosType_(*m);

    if (type == MOS_TYPE_SMALL) {
        multiOrderedSetSmall *small = moss(*m);
        size_t bytes = multiOrderedSetSmallBytes(small);
        size_t count = multiOrderedSetSmallCount(small);

        /* Need at least 2 entries to split for Medium tier */
        if (bytes > flexOptimizationSizeLimit[limit] &&
            count >= MOS_MIN_ENTRIES_FOR_PROMOTION) {
            /* Promote to Medium */
            multiOrderedSetMedium *medium = multiOrderedSetMediumNewFromSmall(
                small, small->map, small->middle);
            /* Small struct freed by NewFromSmall, just update pointer */
            *m = MOS_TAG_(medium, MOS_TYPE_MEDIUM);
            *m = SET_COMPRESS_DEPTH_LIMIT(*m, depth, limit);
        }
    } else if (type == MOS_TYPE_MEDIUM) {
        multiOrderedSetMedium *medium = mosm(*m);
        size_t bytes = multiOrderedSetMediumBytes(medium);
        size_t count = multiOrderedSetMediumCount(medium);

        /* Promote to Full when significantly larger */
        if (bytes > (flexOptimizationSizeLimit[limit] * 3) &&
            count >= MOS_MIN_ENTRIES_FOR_PROMOTION) {
            flex *maps[2] = {medium->map[0], medium->map[1]};
            uint32_t middles[2] = {medium->middle[0], medium->middle[1]};

            multiOrderedSetFull *full =
                multiOrderedSetFullNewFromMedium(medium, maps, middles);
            /* Sub-maps split at the set's own size limit, as multimap's
             * inner maps do (a fixed size would leave a small-limit set's
             * Full in one sub-map, and a large-limit set's sub-maps short) */
            full->maxMapSize = flexOptimizationSizeLimit[limit];

            *m = MOS_TAG_(full, MOS_TYPE_FULL);
            *m = SET_COMPRESS_DEPTH_LIMIT(*m, depth, limit);
        }
    }
    /* Full tier does not promote further */
}

/* After a removal: move a drained Full or Medium down to the tier its
 * rows fit with room to grow, as multimap does.  L is the size limit's
 * cutoff; tiers grow past L (Small) and 3L (Medium) and shrink well
 * below: Full (three sub-maps at most) -> Medium at <= 1.5L, or straight
 * to Small at <= L/2; Medium -> Small at <= L/2.  O(1) unless it moves;
 * rows move once, bounded by 1.5L.  A pooled Full (rows hold pool ids)
 * stays Full. */
/* The move itself: a Full of at most 'fullMaps' sub-maps and 'fullBytes'
 * bytes, or a Medium of at most 'smallBytes', becomes a Small; rows past
 * 'smallBytes' then split into a Medium.  A shrink passes its hysteresis
 * thresholds, multiOrderedSetCompact the growth points (3L, L). */
DK_INLINE_ALWAYS void mosMoveDown_(multiOrderedSet **m, uint32_t fullMaps,
                                   size_t fullBytes, size_t smallBytes) {
    const uint32_t limit = COMPRESS_LIMIT(*m);
    const uint32_t depth = COMPRESS_DEPTH(*m);
    flex *rows = nullptr;

    switch (mosType_(*m)) {
    case MOS_TYPE_FULL: {
        multiOrderedSetFull *full = mosf(*m);
        dkTierStats s;
        if (full->mapCount > fullMaps) {
            return;
        }
        multiOrderedSetFullStats(full, &s);
        if (s.dataBytes > fullBytes ||
            !(rows = multiOrderedSetFullTakeRows(full))) {
            return;
        }
        break;
    }
    case MOS_TYPE_MEDIUM: {
        multiOrderedSetMedium *medium = mosm(*m);
        if (multiOrderedSetMediumBytes(medium) > smallBytes) {
            return;
        }
        /* map[0] holds only scores below map[1]'s: appending keeps order */
        rows = medium->map[0];
        flexBulkAppendFlex(&rows, medium->map[1]);
        flexFree(medium->map[1]);
        zfree(medium);
        break;
    }
    default:
        return;
    }

    /* @own: caller; free=multiOrderedSetFree */
    multiOrderedSetSmall *small = multiOrderedSetSmallNew();
    flexFree(small->map);
    small->map = rows;
    small->middle =
        flexCount(rows)
            ? (uint32_t)(flexMiddle(rows, MOS_ELEMENTS_PER_ENTRY) - rows)
            : FLEX_EMPTY_SIZE;
    *m = MOS_TAG_(small, MOS_TYPE_SMALL);

    /* Rows above 'smallBytes' (from a Full) belong in Medium, split as an
     * upgrade splits them */
    if (flexBytes(rows) > smallBytes &&
        multiOrderedSetSmallCount(small) >= MOS_MIN_ENTRIES_FOR_PROMOTION) {
        multiOrderedSetMedium *medium =
            multiOrderedSetMediumNewFromSmall(small, small->map, small->middle);
        *m = MOS_TAG_(medium, MOS_TYPE_MEDIUM);
    }
    *m = SET_COMPRESS_DEPTH_LIMIT(*m, depth, limit);
}

static void mosShrinkIfNecessary(multiOrderedSet **m) {
    const size_t L = flexOptimizationSizeLimit[COMPRESS_LIMIT(*m)];
    mosMoveDown_(m, 3, L + L / 2, L / 2);
}

void multiOrderedSetCompact(multiOrderedSet **mos) {
    const size_t L = flexOptimizationSizeLimit[COMPRESS_LIMIT(*mos)];
    if (mosType_(*mos) == MOS_TYPE_FULL) {
        multiOrderedSetFullCompact(mosf(*mos));
    }
    mosMoveDown_(mos, UINT32_MAX, 3 * L, L);
}

/* A removal entry point: the tier's call, then a shrink when it removed
 * ('type' is the call's result: bool, or a count of rows removed) */
#define MOS_REMOVE_(type, m, func, ...)                                        \
    do {                                                                       \
        type removed_;                                                         \
        MOS_SETRESULT(removed_, *(m), func, __VA_ARGS__);                      \
        if (removed_) {                                                        \
            mosShrinkIfNecessary(m);                                           \
        }                                                                      \
        return removed_;                                                       \
    } while (0)

/* ====================================================================
 * Creation / Destruction
 * ==================================================================== */

multiOrderedSet *multiOrderedSetNew(void) {
    multiOrderedSetSmall *created = multiOrderedSetSmallNew();
    multiOrderedSet *mos = MOS_TAG_(created, MOS_TYPE_SMALL);
    mos = SET_COMPRESS_DEPTH_LIMIT(mos, 0,
                                   dkTierDefaultLimit(DK_TIER_MULTIORDEREDSET));
    return mos;
}

multiOrderedSet *multiOrderedSetNewLimit(flexCapSizeLimit limit) {
    multiOrderedSetSmall *created = multiOrderedSetSmallNew();
    multiOrderedSet *mos = MOS_TAG_(created, MOS_TYPE_SMALL);
    mos = SET_COMPRESS_DEPTH_LIMIT(mos, 0, limit);
    return mos;
}

multiOrderedSet *multiOrderedSetNewCompress(flexCapSizeLimit limit) {
    multiOrderedSetSmall *created = multiOrderedSetSmallNew();
    multiOrderedSet *mos = MOS_TAG_(created, MOS_TYPE_SMALL);
    mos = SET_COMPRESS_DEPTH_LIMIT(mos, 1, limit);
    return mos;
}

multiOrderedSet *multiOrderedSetCopy(const multiOrderedSet *mos) {
    const uint32_t limit = COMPRESS_LIMIT(mos);
    const uint32_t depth = COMPRESS_DEPTH(mos);
    multiOrderedSet *copy;

    const multiOrderedSetType type = mosType_(mos);
    if (type == MOS_TYPE_SMALL) {
        copy = (multiOrderedSet *)multiOrderedSetSmallCopy(moss(mos));
    } else if (type == MOS_TYPE_MEDIUM) {
        copy = (multiOrderedSet *)multiOrderedSetMediumCopy(mosm(mos));
    } else {
        copy = (multiOrderedSet *)multiOrderedSetFullCopy(mosf(mos));
    }

    copy = SET_COMPRESS_DEPTH_LIMIT(copy, depth, limit);
    copy = MOS_TAG_(copy, type);
    return copy;
}

void multiOrderedSetFree(multiOrderedSet *mos) {
    if (mos) {
        MOS_SINGLE_NORETURN(mos, Free);
    }
}

void multiOrderedSetReset(multiOrderedSet **mos) {
    if (mosType_(*mos) == MOS_TYPE_SMALL) {
        multiOrderedSetSmallReset(moss(*mos));
        return;
    }

    const flexCapSizeLimit limit = COMPRESS_LIMIT(*mos);
    const uint32_t depth = COMPRESS_DEPTH(*mos);
    multiOrderedSetFree(*mos);
    /* @own: caller; free=multiOrderedSetFree */
    multiOrderedSet *created =
        MOS_TAG_(multiOrderedSetSmallNew(), MOS_TYPE_SMALL);
    *mos = SET_COMPRESS_DEPTH_LIMIT(created, depth, limit);
}

/* ====================================================================
 * Basic Statistics
 * ==================================================================== */

size_t multiOrderedSetCount(const multiOrderedSet *mos) {
    MOS_SINGLE_RETURN(mos, Count);
}

size_t multiOrderedSetBytes(const multiOrderedSet *mos) {
    MOS_SINGLE_RETURN(mos, Bytes);
}

flexCapSizeLimit multiOrderedSetLimit(const multiOrderedSet *mos) {
    return (flexCapSizeLimit)COMPRESS_LIMIT(mos);
}

void multiOrderedSetStats(const multiOrderedSet *mos, dkTierStats *out) {
    const uint32_t limit = flexOptimizationSizeLimit[COMPRESS_LIMIT(mos)];
    switch (mosType_(mos)) {
    case MOS_TYPE_SMALL: {
        const multiOrderedSetSmall *s = moss(mos);
        const uint64_t data = multiOrderedSetSmallBytes(s);
        *out = (dkTierStats){.count = multiOrderedSetSmallCount(s),
                             .dataBytes = data,
                             .totalBytes = data + sizeof(*s),
                             .units = 1,
                             .unitLimit = limit,
                             .tier = MOS_TYPE_SMALL};
        break;
    }
    case MOS_TYPE_MEDIUM: {
        const multiOrderedSetMedium *md = mosm(mos);
        const uint64_t data = multiOrderedSetMediumBytes(md);
        *out = (dkTierStats){.count = multiOrderedSetMediumCount(md),
                             .dataBytes = data,
                             .totalBytes = data + sizeof(*md),
                             .units = 2,
                             .unitLimit = limit,
                             .tier = MOS_TYPE_MEDIUM};
        break;
    }
    case MOS_TYPE_FULL:
        multiOrderedSetFullStats(mosf(mos), out);
        break;
    }
}

/* ====================================================================
 * Insertion / Update
 * ==================================================================== */

bool multiOrderedSetAdd(multiOrderedSet **mos, const databox *score,
                        const databox *member) {
    bool result;
    MOS_SETRESULT(result, *mos, Add, score, member);
    mosUpgradeIfNecessary(mos);
    return result;
}

bool multiOrderedSetAddNX(multiOrderedSet **mos, const databox *score,
                          const databox *member) {
    bool result;
    MOS_SETRESULT(result, *mos, AddNX, score, member);
    mosUpgradeIfNecessary(mos);
    return result;
}

bool multiOrderedSetAddXX(multiOrderedSet **mos, const databox *score,
                          const databox *member) {
    bool result;
    MOS_SETRESULT(result, *mos, AddXX, score, member);
    /* No upgrade needed for XX (update-only) */
    return result;
}

bool multiOrderedSetAddGetPrevious(multiOrderedSet **mos, const databox *score,
                                   const databox *member, databox *prevScore) {
    bool result;
    MOS_SETRESULT(result, *mos, AddGetPrevious, score, member, prevScore);
    mosUpgradeIfNecessary(mos);
    return result;
}

/* One member search: the tier's Combine makes the member's new score
 * from its present one and 'operand' by 'op' (an absent member gets
 * 'operand'), then the set upgrades if it grew past its tier */
static bool mosCombineAt_(multiOrderedSet **mos, const databox *operand,
                          const databox *member, mosCombineOp op,
                          databox *result) {
    bool ret;
    MOS_SETRESULT(ret, *mos, Combine, operand, member, op, result);
    mosUpgradeIfNecessary(mos);
    return ret;
}

bool multiOrderedSetIncrBy(multiOrderedSet **mos, const databox *delta,
                           const databox *member, databox *result) {
    /* Increment policy: the delta must be a 64-bit integer or a finite real
     * (it becomes a new member's score as is); sums are checked by
     * mosScoreAdd. */
    double asReal;
    const bool deltaOk = delta->type == DATABOX_SIGNED_64 ||
                         delta->type == DATABOX_UNSIGNED_64 ||
                         ((delta->type == DATABOX_FLOAT_32 ||
                           delta->type == DATABOX_DOUBLE_64) &&
                          mosScoreToDouble(delta, &asReal) && isfinite(asReal));
    if (!deltaOk) {
        return false;
    }

    return mosCombineAt_(mos, delta, member, MOS_COMBINE_INCR, result);
}

/* ====================================================================
 * Deletion
 * ==================================================================== */

bool multiOrderedSetRemove(multiOrderedSet **mos, const databox *member) {
    MOS_REMOVE_(bool, mos, Remove, member);
}

bool multiOrderedSetRemoveGetScore(multiOrderedSet **mos, const databox *member,
                                   databox *score) {
    MOS_REMOVE_(bool, mos, RemoveGetScore, member, score);
}

bool multiOrderedSetRemoveIf(multiOrderedSet **mos, const databox *member,
                             flexProceed *proceed, void *ctx) {
    MOS_REMOVE_(bool, mos, RemoveIf, member, proceed, ctx);
}

size_t multiOrderedSetRemoveRangeByScore(multiOrderedSet **mos,
                                         const mosRangeSpec *range) {
    MOS_REMOVE_(size_t, mos, RemoveRangeByScore, range);
}

size_t multiOrderedSetRemoveRangeByRank(multiOrderedSet **mos, int64_t start,
                                        int64_t stop) {
    MOS_REMOVE_(size_t, mos, RemoveRangeByRank, start, stop);
}

size_t multiOrderedSetPopMin(multiOrderedSet **mos, size_t count,
                             databox *members, databox *scores) {
    MOS_REMOVE_(size_t, mos, PopMin, count, members, scores);
}

size_t multiOrderedSetPopMax(multiOrderedSet **mos, size_t count,
                             databox *members, databox *scores) {
    MOS_REMOVE_(size_t, mos, PopMax, count, members, scores);
}

/* ====================================================================
 * Lookup
 * ==================================================================== */

bool multiOrderedSetExists(const multiOrderedSet *mos, const databox *member) {
    MOS_RETURN(mos, Exists, member);
}

bool multiOrderedSetGetScore(const multiOrderedSet *mos, const databox *member,
                             databox *score) {
    MOS_RETURN(mos, GetScore, member, score);
}

int64_t multiOrderedSetGetRank(const multiOrderedSet *mos,
                               const databox *member) {
    MOS_RETURN(mos, GetRank, member);
}

int64_t multiOrderedSetGetReverseRank(const multiOrderedSet *mos,
                                      const databox *member) {
    MOS_RETURN(mos, GetReverseRank, member);
}

bool multiOrderedSetGetByRank(const multiOrderedSet *mos, int64_t rank,
                              databox *member, databox *score) {
    MOS_RETURN(mos, GetByRank, rank, member, score);
}

/* ====================================================================
 * Range Queries
 * ==================================================================== */

size_t multiOrderedSetCountByScore(const multiOrderedSet *mos,
                                   const mosRangeSpec *range) {
    MOS_RETURN(mos, CountByScore, range);
}

/* ====================================================================
 * Iteration
 * ==================================================================== */

void multiOrderedSetIteratorInit(const multiOrderedSet *mos, mosIterator *iter,
                                 bool forward) {
    iter->type = mosType_(mos);
    MOS_NORETURN(mos, IteratorInit, iter, forward);
}

bool multiOrderedSetIteratorInitAtScore(const multiOrderedSet *mos,
                                        mosIterator *iter, const databox *score,
                                        bool forward) {
    iter->type = mosType_(mos);
    MOS_RETURN(mos, IteratorInitAtScore, iter, score, forward);
}

bool multiOrderedSetIteratorInitAtRank(const multiOrderedSet *mos,
                                       mosIterator *iter, int64_t rank,
                                       bool forward) {
    iter->type = mosType_(mos);
    MOS_RETURN(mos, IteratorInitAtRank, iter, rank, forward);
}

bool multiOrderedSetIteratorNext(mosIterator *iter, databox *member,
                                 databox *score) {
    /* Direct dispatch based on stored type in iterator */
    static const void *jumper[] = {nullptr, &&iterSmall, &&iterMedium,
                                   &&iterFull};
    goto *jumper[iter->type];

iterSmall:
    return multiOrderedSetSmallIteratorNext(iter, member, score);

iterMedium:
    return multiOrderedSetMediumIteratorNext(iter, member, score);

iterFull:
    return multiOrderedSetFullIteratorNext(iter, member, score);
}

void multiOrderedSetIteratorRelease(mosIterator *iter) {
    /* Currently no resources to release, but keep for API consistency */
    iter->valid = 0;
}

/* ====================================================================
 * First / Last Access
 * ==================================================================== */

bool multiOrderedSetFirst(const multiOrderedSet *mos, databox *member,
                          databox *score) {
    MOS_RETURN(mos, First, member, score);
}

bool multiOrderedSetLast(const multiOrderedSet *mos, databox *member,
                         databox *score) {
    MOS_RETURN(mos, Last, member, score);
}

/* ====================================================================
 * Random Access
 * ==================================================================== */

size_t multiOrderedSetRandomMembers(const multiOrderedSet *mos, int64_t count,
                                    databox *members, databox *scores) {
    MOS_RETURN(mos, RandomMembers, count, members, scores);
}

/* ====================================================================
 * Set Operations
 * ==================================================================== */

/* Helper: apply aggregate function to combine scores */
static void applyAggregate(databox *result, const databox *a, const databox *b,
                           double weightA, double weightB,
                           mosAggregate aggregate) {
    /* Convert scores to double for weighted operations */
    double valA = mosDataboxToDouble(a) * weightA;
    double valB = mosDataboxToDouble(b) * weightB;
    double combined;

    switch (aggregate) {
    case MOS_AGGREGATE_SUM:
        combined = valA + valB;
        break;
    case MOS_AGGREGATE_MIN:
        combined = (valA < valB) ? valA : valB;
        break;
    case MOS_AGGREGATE_MAX:
        combined = (valA > valB) ? valA : valB;
        break;
    default:
        combined = valA + valB;
        break;
    }

    DATABOX_SET_DOUBLE(result, combined);
}

multiOrderedSet *multiOrderedSetUnion(const multiOrderedSet *sets[],
                                      const double *weights, size_t numSets,
                                      mosAggregate aggregate) {
    if (numSets == 0) {
        /* @own: caller; free=multiOrderedSetFree */
        return multiOrderedSetNew();
    }

    /* @own: caller; free=multiOrderedSetFree */
    multiOrderedSet *result = multiOrderedSetNew();

    /* Any other aggregate value sums, as applyAggregate does */
    const mosCombineOp op = aggregate == MOS_AGGREGATE_MIN   ? MOS_COMBINE_MIN
                            : aggregate == MOS_AGGREGATE_MAX ? MOS_COMBINE_MAX
                                                             : MOS_COMBINE_SUM;

    for (size_t i = 0; i < numSets; i++) {
        if (!sets[i]) {
            continue;
        }

        double weight = weights ? weights[i] : 1.0;

        mosIterator iter;
        multiOrderedSetIteratorInit(sets[i], &iter, true);

        databox member, score;
        databoxWide memberWide, scoreWide;
        for (;;) {
            mosBoxAim(&member, &memberWide);
            mosBoxAim(&score, &scoreWide);
            if (!multiOrderedSetIteratorNext(&iter, &member, &score)) {
                break;
            }

            /* One search: a new member gets its weighted score, a present
             * one the aggregate of its score and the weighted one */
            databox weightedScore;
            DATABOX_SET_DOUBLE(&weightedScore,
                               mosDataboxToDouble(&score) * weight);
            databox combined;
            (void)mosCombineAt_(&result, &weightedScore, &member, op,
                                &combined);
        }
    }

    return result;
}

multiOrderedSet *multiOrderedSetIntersect(const multiOrderedSet *sets[],
                                          const double *weights, size_t numSets,
                                          mosAggregate aggregate) {
    if (numSets < 2) {
        /* @own: caller; free=multiOrderedSetFree */
        return multiOrderedSetNew();
    }

    /* Start with smallest set for efficiency */
    size_t smallestIdx = 0;
    size_t smallestCount = multiOrderedSetCount(sets[0]);
    for (size_t i = 1; i < numSets; i++) {
        size_t count = multiOrderedSetCount(sets[i]);
        if (count < smallestCount) {
            smallestCount = count;
            smallestIdx = i;
        }
    }

    /* @own: caller; free=multiOrderedSetFree */
    multiOrderedSet *result = multiOrderedSetNew();

    /* Iterate through smallest set */
    mosIterator iter;
    multiOrderedSetIteratorInit(sets[smallestIdx], &iter, true);

    databox member, score;
    databoxWide memberWide, scoreWide;
    for (;;) {
        mosBoxAim(&member, &memberWide);
        mosBoxAim(&score, &scoreWide);
        if (!multiOrderedSetIteratorNext(&iter, &member, &score)) {
            break;
        }

        bool inAll = true;
        databox combinedScore;
        double weight0 = weights ? weights[smallestIdx] : 1.0;

        /* Apply weight to initial score */
        double val = mosDataboxToDouble(&score) * weight0;
        DATABOX_SET_DOUBLE(&combinedScore, val);

        /* Check if member exists in all other sets */
        for (size_t i = 0; i < numSets && inAll; i++) {
            if (i == smallestIdx) {
                continue;
            }

            databox otherScore;
            databoxWide otherScoreWide;
            mosBoxAim(&otherScore, &otherScoreWide);
            if (multiOrderedSetGetScore(sets[i], &member, &otherScore)) {
                double weight = weights ? weights[i] : 1.0;
                applyAggregate(&combinedScore, &combinedScore, &otherScore, 1.0,
                               weight, aggregate);
            } else {
                inAll = false;
            }
        }

        if (inAll) {
            multiOrderedSetAdd(&result, &combinedScore, &member);
        }
    }

    return result;
}

multiOrderedSet *multiOrderedSetDifference(const multiOrderedSet *sets[],
                                           size_t numSets) {
    if (numSets == 0) {
        /* @own: caller; free=multiOrderedSetFree */
        return multiOrderedSetNew();
    }

    /* @own: caller; free=multiOrderedSetFree */
    multiOrderedSet *result = multiOrderedSetCopy(sets[0]);

    /* Remove members that exist in any other set */
    for (size_t i = 1; i < numSets; i++) {
        if (!sets[i]) {
            continue;
        }

        mosIterator iter;
        multiOrderedSetIteratorInit(sets[i], &iter, true);

        /* Only the member is needed: the score is read type-only */
        databox member, score;
        databoxWide memberWide;
        for (;;) {
            mosBoxAim(&member, &memberWide);
            mosBoxClear(&score);
            if (!multiOrderedSetIteratorNext(&iter, &member, &score)) {
                break;
            }
            multiOrderedSetRemove(&result, &member);
        }
    }

    return result;
}

/* ====================================================================
 * Debugging / Testing
 * ==================================================================== */

#ifdef DATAKIT_TEST
#include "ctest.h"

#define DOUBLE_NEWLINE 0
#include "perf.h"

#include "multimap.h" /* For comparison benchmarks */

#include "str.h" /* for xoroshiro128plus */

defcleaner(multimap *, multimapFree);
typedef struct mosWidePair mosWidePair;
defcleanerName(mosWidePair *, zfree, WidePairs);
defcleanerName(size_t *, zfree, Sizes);

#define REPORT_TIME 1
#if REPORT_TIME
#define TIME_INIT PERF_TIMERS_SETUP
#define TIME_FINISH(i, what) PERF_TIMERS_FINISH_PRINT_RESULTS(i, what)
#else
#define TIME_INIT
#define TIME_FINISH(i, what)
#endif

/* A non-empty map's stored middle offset is its exact middle row */
static void mosVerifyMiddle_(const flex *map, uint32_t middle) {
    if (flexCount(map)) {
        assert(middle ==
               (uint32_t)(flexMiddle(map, MOS_ELEMENTS_PER_ENTRY) - map));
    }
}

void multiOrderedSetVerify(const multiOrderedSet *mos) {
    switch (mosType_(mos)) {
    case MOS_TYPE_SMALL:
        mosVerifyMiddle_(moss(mos)->map, moss(mos)->middle);
        break;
    case MOS_TYPE_MEDIUM: {
        const multiOrderedSetMedium *md = mosm(mos);
        mosVerifyMiddle_(md->map[0], md->middle[0]);
        mosVerifyMiddle_(md->map[1], md->middle[1]);
        if (flexCount(md->map[0]) && flexCount(md->map[1])) {
            databox last;
            databox head;
            databoxWide lastWide;
            databoxWide headWide;
            flexGetByTypeInto(
                flexIndex(md->map[0], -(int32_t)MOS_ELEMENTS_PER_ENTRY), &last,
                &lastWide);
            flexGetByTypeInto(flexHead(md->map[1]), &head, &headWide);
            assert(databoxCompare(&last, &head) <= 0);
        }
        break;
    }
    case MOS_TYPE_FULL:
        multiOrderedSetFullVerify(mosf(mos));
        break;
    }
}

void multiOrderedSetRepr(const multiOrderedSet *mos) {
    printf("multiOrderedSet (%p) type=%d count=%zu bytes=%zu\n", (void *)mos,
           mosType_(mos), multiOrderedSetCount(mos), multiOrderedSetBytes(mos));

    switch (mosType_(mos)) {
    case MOS_TYPE_SMALL:
        multiOrderedSetSmallRepr(moss(mos));
        break;
    case MOS_TYPE_MEDIUM:
        multiOrderedSetMediumRepr(mosm(mos));
        break;
    case MOS_TYPE_FULL:
        multiOrderedSetFullRepr(mosf(mos));
        break;
    default:
        printf("  Unknown type!\n");
        break;
    }
}

/* ====================================================================
 * 96/128-bit scores and members: oracle helpers
 * ====================================================================
 * The oracle keeps every integer as two's-complement bits plus a sign,
 * which covers [INT128_MIN, UINT128_MAX] without going through databox. */
typedef struct mosWideNum {
    __uint128_t bits;
    bool neg;
} mosWideNum;

struct mosWidePair {
    mosWideNum score;
    mosWideNum memberNum;
    bool memberIsBytes;
    char memberStr[16];
};

static mosWideNum mosWideU_(__uint128_t v) {
    return (mosWideNum){.bits = v, .neg = false};
}

static mosWideNum mosWideS_(__int128_t v) {
    return (mosWideNum){.bits = (__uint128_t)v, .neg = v < 0};
}

static int mosWideNumCmp_(mosWideNum a, mosWideNum b) {
    if (a.neg != b.neg) {
        return a.neg ? -1 : 1;
    }

    if (a.neg) {
        const __int128_t x = (__int128_t)a.bits;
        const __int128_t y = (__int128_t)b.bits;
        return (x > y) - (x < y);
    }

    return (a.bits > b.bits) - (a.bits < b.bits);
}

static bool mosWideNumIsWide_(mosWideNum n) {
    return n.neg ? (__int128_t)n.bits < INT64_MIN : n.bits > UINT64_MAX;
}

/* Box 'n' in its canonical type: 64-bit when it fits, else 128-bit with
 * the payload in b->extra. */
static void mosWideNumBox_(databoxBig *b, mosWideNum n) {
    memset(b, 0, sizeof(*b));
    if (n.neg) {
        const __int128_t v = (__int128_t)n.bits;
        if (v >= INT64_MIN) {
            b->type = DATABOX_SIGNED_64;
            b->data.i64 = (int64_t)v;
        } else {
            DATABOX_BIG_SIGNED_128(b, v);
        }
    } else if (n.bits <= UINT64_MAX) {
        b->type = DATABOX_UNSIGNED_64;
        b->data.u64 = (uint64_t)n.bits;
    } else {
        DATABOX_BIG_UNSIGNED_128(b, n.bits);
    }
}

/* Read an integer box back into the oracle form.  A 96/128-bit box with
 * no payload (type-only) is not a value and reads as false. */
static bool mosWideNumFromBox_(const databox *b, mosWideNum *n) {
    switch (b->type) {
    case DATABOX_SIGNED_64:
        *n = mosWideS_(b->data.i64);
        return true;
    case DATABOX_UNSIGNED_64:
        *n = mosWideU_(b->data.u64);
        return true;
    case DATABOX_SIGNED_128:
        if (!b->data.i128) {
            return false;
        }
        *n = mosWideS_(*b->data.i128);
        return true;
    case DATABOX_UNSIGNED_128:
        if (!b->data.u128) {
            return false;
        }
        *n = mosWideU_(*b->data.u128);
        return true;
    default:
        return false;
    }
}

static void mosWideMemberBox_(databoxBig *b, const mosWidePair *p) {
    if (p->memberIsBytes) {
        memset(b, 0, sizeof(*b));
        *(databox *)b = databoxNewBytesString(p->memberStr);
    } else {
        mosWideNumBox_(b, p->memberNum);
    }
}

/* A decoded score matches the oracle in value and in canonical type. */
static bool mosWideScoreMatches_(const databox *got, const mosWidePair *p) {
    mosWideNum n;
    if (!mosWideNumFromBox_(got, &n) || mosWideNumCmp_(n, p->score) != 0) {
        return false;
    }

    const bool gotWide =
        got->type == DATABOX_SIGNED_128 || got->type == DATABOX_UNSIGNED_128;
    return gotWide == mosWideNumIsWide_(p->score);
}

static bool mosWideMemberMatches_(const databox *got, const mosWidePair *p) {
    if (p->memberIsBytes) {
        const databox want = databoxNewBytesString(p->memberStr);
        return (got->type == DATABOX_BYTES ||
                got->type == DATABOX_BYTES_EMBED) &&
               databoxCompare(got, &want) == 0;
    }

    mosWideNum n;
    if (!mosWideNumFromBox_(got, &n) || mosWideNumCmp_(n, p->memberNum) != 0) {
        return false;
    }

    const bool gotWide =
        got->type == DATABOX_SIGNED_128 || got->type == DATABOX_UNSIGNED_128;
    return gotWide == mosWideNumIsWide_(p->memberNum);
}

/* Set order: score, then member.  Pairs that share a score always have
 * integer members, so the tie-break never compares across types. */
static int mosWidePairCmp_(const void *a, const void *b) {
    const mosWidePair *x = a;
    const mosWidePair *y = b;
    const int cmp = mosWideNumCmp_(x->score, y->score);
    if (cmp != 0) {
        return cmp;
    }

    return mosWideNumCmp_(x->memberNum, y->memberNum);
}

/* A removed value is self-contained: a wide one owns a heap payload and
 * BYTES own a heap copy (or are embedded). */
static bool mosWideOwned_(const databox *got) {
    switch (got->type) {
    case DATABOX_SIGNED_128:
    case DATABOX_UNSIGNED_128:
        return got->big && got->allocated && got->data.u128;
    case DATABOX_BYTES:
        return !got->big && got->allocated;
    default:
        return !got->big;
    }
}

static uint64_t mosWideRng_ = UINT64_C(0x9E3779B97F4A7C15);

static uint64_t mosWideMix_(uint64_t x) {
    return dkSplitmix64Next(&x);
}

/* Value 'i' of a fixed sequence: the wide set W (INT128 extremes, 2^64,
 * 2^95 +- 1, 2^127, UINT128_MAX, -2^63-1, -2^95), then 96/128-bit
 * values of both signs derived from 'i'. */
static mosWideNum mosWideValue_(size_t i) {
    const __uint128_t one = 1;
    switch (i) {
    case 0:
        return mosWideU_(one << 64);
    case 1:
        return mosWideU_((one << 95) - 1);
    case 2:
        return mosWideU_(one << 95);
    case 3:
        return mosWideU_(one << 127);
    case 4:
        return mosWideU_(~(__uint128_t)0);
    case 5:
        return mosWideS_((__int128_t)INT64_MIN - 1);
    case 6:
        return mosWideS_(-(__int128_t)(one << 95));
    case 7:
        return mosWideS_((__int128_t)(one << 127));
    case 8:
        return mosWideS_((__int128_t)((one << 127) - 1));
    default:
        break;
    }

    const uint64_t hi = mosWideMix_(i);
    const uint64_t lo = mosWideMix_(hi);
    /* About half 96-bit, half full 128-bit; both signs */
    const __uint128_t mag = (i & 1) ? ((__uint128_t)(hi >> 33) << 64) | lo
                                    : ((__uint128_t)(hi >> 1) << 64) | lo;
    const __uint128_t wideMag = mag | (one << 66);
    return (i & 2) ? mosWideS_(-(__int128_t)wideMag) : mosWideU_(wideMag);
}

static bool mosWideSeen_(const mosWidePair *p, size_t n, mosWideNum v,
                         bool member) {
    for (size_t i = 0; i < n; i++) {
        const mosWideNum have = member ? p[i].memberNum : p[i].score;
        if ((!member || !p[i].memberIsBytes) && mosWideNumCmp_(have, v) == 0) {
            return true;
        }
    }

    return false;
}

/* Which values a scenario uses: only 64-bit and BYTES (normal), only
 * 96/128-bit integers (extended), or both (mixed). */
typedef enum mosWideMode {
    MOS_WIDE_MIXED = 0,
    MOS_WIDE_NORMAL,
    MOS_WIDE_EXTENDED,
} mosWideMode;

static mosWideMode mosWideMode_ = MOS_WIDE_MIXED;

/* The score every 50th pair shares */
static mosWideNum mosWideShared_(void) {
    return mosWideMode_ == MOS_WIDE_NORMAL ? mosWideU_((__uint128_t)1 << 62)
                                           : mosWideU_((__uint128_t)1 << 64);
}

/* Score 'i' of a fixed sequence in the current mode */
static mosWideNum mosWideScoreAt_(size_t i) {
    if (mosWideMode_ == MOS_WIDE_NORMAL) {
        return mosWideS_((int64_t)mosWideMix_(i) >> 2);
    }

    return mosWideValue_(i);
}

/* Fill 'n' pairs: in mixed mode a third wide scores, the rest 64-bit,
 * with every 50th pair sharing one score (integer members only); members
 * rotate through BYTES, 64-bit and wide integers.  Normal mode keeps
 * every value 64-bit or BYTES; extended mode makes every score and
 * member a 96/128-bit integer. */
static void mosWideFill_(mosWidePair *p, size_t n) {
    size_t wideScores = 0;
    size_t wideMembers = 0;
    const mosWideNum shared = mosWideShared_();
    for (size_t i = 0; i < n; i++) {
        mosWidePair *pair = &p[i];
        memset(pair, 0, sizeof(*pair));

        const bool sharedScore = (i % 50) == 1;
        if (sharedScore) {
            pair->score = shared;
        } else {
            do {
                const bool wide =
                    mosWideMode_ == MOS_WIDE_EXTENDED ||
                    (mosWideMode_ == MOS_WIDE_MIXED && i % 3 == 0);
                if (wide) {
                    pair->score = mosWideValue_(wideScores++);
                } else {
                    const int64_t v =
                        (int64_t)xorshift64star(&mosWideRng_) >> (i % 40);
                    pair->score = mosWideS_(v);
                }
            } while (mosWideSeen_(p, i, pair->score, false) ||
                     mosWideNumCmp_(pair->score, shared) == 0);
        }

        size_t kind = sharedScore ? 2 : i % 3;
        if (mosWideMode_ == MOS_WIDE_NORMAL && kind == 2) {
            kind = 1;
        } else if (mosWideMode_ == MOS_WIDE_EXTENDED) {
            kind = 2;
        }

        if (kind == 0) {
            pair->memberIsBytes = true;
            snprintf(pair->memberStr, sizeof(pair->memberStr), "wm-%05zu", i);
        } else {
            do {
                pair->memberNum =
                    kind == 1 ? mosWideS_((int64_t)xorshift64star(&mosWideRng_))
                              : mosWideValue_(wideMembers++);
            } while (mosWideSeen_(p, i, pair->memberNum, true));
        }
    }
}

static void mosWideAdd_(multiOrderedSet **mos, const mosWidePair *p) {
    databoxBig score;
    databoxBig member;
    mosWideNumBox_(&score, p->score);
    mosWideMemberBox_(&member, p);
    ctestPoisonStack();
    multiOrderedSetAdd(mos, (databox *)&score, (databox *)&member);
}

/* Checks every read path against 'sorted' (the oracle, in set order).
 * Returns the number of mismatches. */
static int mosWideVerify_(const multiOrderedSet *mos, const mosWidePair *sorted,
                          size_t n) {
    int err = 0;

    if (multiOrderedSetCount(mos) != n) {
        ERR("count %zu, expected %zu", multiOrderedSetCount(mos), n);
        return err;
    }

    /* Iterate both directions into caller-provided wide storage */
    for (int dir = 0; dir < 2; dir++) {
        const bool forward = dir == 0;
        mosIterator iter;
        ctestPoisonStack();
        multiOrderedSetIteratorInit(mos, &iter, forward);
        size_t k = 0;
        databoxBig member;
        databoxBig score;
        for (;;) {
            DATABOX_BIG_INIT(&member);
            DATABOX_BIG_INIT(&score);
            ctestPoisonStack();
            if (!multiOrderedSetIteratorNext(&iter, (databox *)&member,
                                             (databox *)&score)) {
                break;
            }

            const mosWidePair *p = &sorted[forward ? k : n - 1 - k];
            if (k >= n || !mosWideScoreMatches_((databox *)&score, p) ||
                !mosWideMemberMatches_((databox *)&member, p)) {
                ERR("%s iteration mismatch at %zu",
                    forward ? "forward" : "reverse", k);
                break;
            }
            k++;
        }
        if (k != n) {
            ERR("%s iteration visited %zu of %zu",
                forward ? "forward" : "reverse", k, n);
        }
    }

    for (size_t k = 0; k < n; k++) {
        const mosWidePair *p = &sorted[k];
        databoxBig member;
        mosWideMemberBox_(&member, p);

        ctestPoisonStack();
        if (!multiOrderedSetExists(mos, (databox *)&member)) {
            ERR("member %zu not found", k);
            continue;
        }

        databoxBig score;
        DATABOX_BIG_INIT(&score);
        ctestPoisonStack();
        if (!multiOrderedSetGetScore(mos, (databox *)&member,
                                     (databox *)&score) ||
            !mosWideScoreMatches_((databox *)&score, p)) {
            ERR("GetScore mismatch at %zu", k);
        }

        ctestPoisonStack();
        const int64_t rank = multiOrderedSetGetRank(mos, (databox *)&member);
        if (rank != (int64_t)k) {
            ERR("GetRank %" PRId64 ", expected %zu", rank, k);
        }

        ctestPoisonStack();
        const int64_t rev =
            multiOrderedSetGetReverseRank(mos, (databox *)&member);
        if (rev != (int64_t)(n - 1 - k)) {
            ERR("GetReverseRank %" PRId64 ", expected %zu", rev, n - 1 - k);
        }

        databoxBig gotMember;
        DATABOX_BIG_INIT(&gotMember);
        DATABOX_BIG_INIT(&score);
        ctestPoisonStack();
        if (!multiOrderedSetGetByRank(mos, (int64_t)k, (databox *)&gotMember,
                                      (databox *)&score) ||
            !mosWideScoreMatches_((databox *)&score, p) ||
            !mosWideMemberMatches_((databox *)&gotMember, p)) {
            ERR("GetByRank mismatch at %zu", k);
        }

        /* Positioned iteration: the first pair at or above this score */
        size_t first = k;
        while (first > 0 &&
               mosWideNumCmp_(sorted[first - 1].score, p->score) == 0) {
            first--;
        }
        databoxBig at;
        mosWideNumBox_(&at, p->score);
        mosIterator iter;
        ctestPoisonStack();
        if (!multiOrderedSetIteratorInitAtScore(mos, &iter, (databox *)&at,
                                                true)) {
            ERR("IteratorInitAtScore failed at %zu", k);
        } else {
            DATABOX_BIG_INIT(&gotMember);
            DATABOX_BIG_INIT(&score);
            if (!multiOrderedSetIteratorNext(&iter, (databox *)&gotMember,
                                             (databox *)&score) ||
                !mosWideScoreMatches_((databox *)&score, &sorted[first]) ||
                !mosWideMemberMatches_((databox *)&gotMember, &sorted[first])) {
                ERR("IteratorInitAtScore landed wrong at %zu", k);
            }
        }
    }

    if (n > 0) {
        databoxBig member;
        databoxBig score;
        DATABOX_BIG_INIT(&member);
        DATABOX_BIG_INIT(&score);
        if (!multiOrderedSetFirst(mos, (databox *)&member, (databox *)&score) ||
            !mosWideScoreMatches_((databox *)&score, &sorted[0]) ||
            !mosWideMemberMatches_((databox *)&member, &sorted[0])) {
            ERR("First mismatch");
        }

        DATABOX_BIG_INIT(&member);
        DATABOX_BIG_INIT(&score);
        if (!multiOrderedSetLast(mos, (databox *)&member, (databox *)&score) ||
            !mosWideScoreMatches_((databox *)&score, &sorted[n - 1]) ||
            !mosWideMemberMatches_((databox *)&member, &sorted[n - 1])) {
            ERR("Last mismatch");
        }

        /* RandomMembers ignores poisoned destinations: a wide value comes
         * back deferred, any other value whole with clean flags */
        databox rMembers[8];
        databox rScores[8];
        memset(rMembers, 0xFF, sizeof(rMembers));
        memset(rScores, 0xFF, sizeof(rScores));
        ctestPoisonStack();
        const size_t picked =
            multiOrderedSetRandomMembers(mos, -8, rMembers, rScores);
        if (picked == 0) {
            ERR("RandomMembers returned nothing");
        }
        for (size_t i = 0; i < picked; i++) {
            const databox *boxes[2] = {&rMembers[i], &rScores[i]};
            for (size_t j = 0; j < 2; j++) {
                const databox *b = boxes[j];
                if (mosBoxIsWide(b) ? !databoxIsWideDeferred(b)
                                    : (b->big || b->allocated)) {
                    ERR("RandomMembers slot %zu.%zu not deferred or stale", i,
                        j);
                }
            }

            /* A narrow score finds its pair in the oracle */
            mosWideNum s;
            if (mosWideNumFromBox_(&rScores[i], &s)) {
                bool found = false;
                for (size_t k = 0; k < n && !found; k++) {
                    found = mosWideNumCmp_(sorted[k].score, s) == 0;
                }
                if (!found) {
                    ERR("RandomMembers score %zu is not in the set", i);
                }
            }
        }

        /* Range counts with wide bounds, inclusive and exclusive */
        for (size_t a = 0; a < n; a += 1 + n / 7) {
            const size_t b = n - 1 - (a / 2);
            databoxBig lo;
            databoxBig hi;
            mosWideNumBox_(&lo, sorted[a].score);
            mosWideNumBox_(&hi, sorted[b].score);
            for (int ex = 0; ex < 4; ex++) {
                mosRangeSpec range = {.min = *(databox *)&lo,
                                      .max = *(databox *)&hi,
                                      .minExclusive = ex & 1,
                                      .maxExclusive = (ex & 2) != 0};
                size_t want = 0;
                for (size_t i = 0; i < n; i++) {
                    const int cmin =
                        mosWideNumCmp_(sorted[i].score, sorted[a].score);
                    const int cmax =
                        mosWideNumCmp_(sorted[i].score, sorted[b].score);
                    if ((range.minExclusive ? cmin > 0 : cmin >= 0) &&
                        (range.maxExclusive ? cmax < 0 : cmax <= 0)) {
                        want++;
                    }
                }

                ctestPoisonStack();
                const size_t got = multiOrderedSetCountByScore(mos, &range);
                if (got != want) {
                    ERR("CountByScore [%zu..%zu] ex=%d: %zu, expected %zu", a,
                        b, ex, got, want);
                }
            }
        }
    }

    return err;
}

/* Removes the pair at 'idx' from the oracle. */
static void mosWideDrop_(mosWidePair *sorted, size_t *n, size_t idx) {
    memmove(&sorted[idx], &sorted[idx + 1], (*n - idx - 1) * sizeof(*sorted));
    (*n)--;
}

static size_t mosWideFindMember_(const mosWidePair *sorted, size_t n,
                                 const mosWidePair *p) {
    for (size_t i = 0; i < n; i++) {
        if (sorted[i].memberIsBytes == p->memberIsBytes &&
            (p->memberIsBytes
                 ? strcmp(sorted[i].memberStr, p->memberStr) == 0
                 : mosWideNumCmp_(sorted[i].memberNum, p->memberNum) == 0)) {
            return i;
        }
    }

    return SIZE_MAX;
}

/* Builds a set of 'n' pairs, checks it reaches 'tier', then drives the
 * read paths, the removing reads and the deletes against the oracle. */
static int mosWideScenario_(size_t n, multiOrderedSetType tier) {
    int err = 0;
    cleanerName(zfree, WidePairs) mosWidePair *pairs =
        zcalloc(n, sizeof(*pairs));
    cleanerName(zfree, WidePairs) mosWidePair *sorted =
        zcalloc(n, sizeof(*sorted));
    cleanerName(zfree, Sizes) size_t *order = zcalloc(n, sizeof(*order));
    mosWideFill_(pairs, n);

    /* Insert in shuffled order */
    for (size_t i = 0; i < n; i++) {
        order[i] = i;
    }
    for (size_t i = n; i > 1; i--) {
        const size_t j = xorshift64star(&mosWideRng_) % i;
        const size_t t = order[i - 1];
        order[i - 1] = order[j];
        order[j] = t;
    }

    cleaner(multiOrderedSetFree) multiOrderedSet *mos = multiOrderedSetNew();
    for (size_t i = 0; i < n; i++) {
        mosWideAdd_(&mos, &pairs[order[i]]);
    }

    if (mosType_(mos) != tier) {
        ERR("%zu pairs built tier %d, expected %d", n, mosType_(mos), tier);
    }

    memcpy(sorted, pairs, n * sizeof(*sorted));
    qsort(sorted, n, sizeof(*sorted), mosWidePairCmp_);
    size_t live = n;
    err += mosWideVerify_(mos, sorted, live);

    /* A copy reads back the same pairs */
    {
        cleaner(multiOrderedSetFree) multiOrderedSet *copy =
            multiOrderedSetCopy(mos);
        err += mosWideVerify_(copy, sorted, live);
    }

    /* Re-adding an existing member with a new score moves it */
    for (size_t i = 0; i < n; i += 5) {
        const size_t at = mosWideFindMember_(sorted, live, &pairs[i]);
        mosWidePair moved = sorted[at];
        mosWideDrop_(sorted, &live, at);
        size_t gen = 5000 + i * 3;
        do {
            moved.score = mosWideScoreAt_(gen++);
        } while (mosWideSeen_(sorted, live, moved.score, false));

        databoxBig score;
        databoxBig member;
        mosWideNumBox_(&score, moved.score);
        mosWideMemberBox_(&member, &moved);

        /* The previous score is self-contained: the destination's prior
         * state is never read */
        databoxBig prev;
        memset(&prev, 0xFF, sizeof(prev));
        ctestPoisonStack();
        if (!multiOrderedSetAddGetPrevious(&mos, (databox *)&score,
                                           (databox *)&member,
                                           (databox *)&prev)) {
            ERR("AddGetPrevious did not find member %zu", i);
        } else {
            if (!mosWideScoreMatches_((databox *)&prev, &pairs[i]) ||
                !mosWideOwned_((databox *)&prev)) {
                ERR("AddGetPrevious previous score wrong for %zu", i);
            }
            databoxFreeData((databox *)&prev);
        }

        pairs[i] = moved;
        sorted[live++] = moved;
        qsort(sorted, live, sizeof(*sorted), mosWidePairCmp_);
    }
    err += mosWideVerify_(mos, sorted, live);

    /* RemoveGetScore into a poisoned plain box, then Remove */
    for (size_t i = 1; i < n; i += 7) {
        const size_t at = mosWideFindMember_(sorted, live, &pairs[i]);
        if (at == SIZE_MAX) {
            continue;
        }

        databoxBig member;
        mosWideMemberBox_(&member, &sorted[at]);
        if (i % 2) {
            databox score;
            memset(&score, 0xFF, sizeof(score));
            ctestPoisonStack();
            if (!multiOrderedSetRemoveGetScore(&mos, (databox *)&member,
                                               &score)) {
                ERR("RemoveGetScore did not find member %zu", i);
            } else {
                if (!mosWideScoreMatches_(&score, &sorted[at]) ||
                    !mosWideOwned_(&score)) {
                    ERR("RemoveGetScore wrong for %zu", i);
                }
                databoxFreeData(&score);
            }
        } else {
            ctestPoisonStack();
            if (!multiOrderedSetRemove(&mos, (databox *)&member)) {
                ERR("Remove did not find member %zu", i);
            }
        }

        if (multiOrderedSetExists(mos, (databox *)&member)) {
            ERR("member %zu still exists after removal", i);
        }
        mosWideDrop_(sorted, &live, at);
    }
    err += mosWideVerify_(mos, sorted, live);

    /* Pops fill poisoned plain arrays with self-contained values */
    for (int side = 0; side < 2 && live >= 3; side++) {
        databox members[3];
        databox scores[3];
        memset(members, 0xFF, sizeof(members));
        memset(scores, 0xFF, sizeof(scores));
        ctestPoisonStack();
        const size_t popped =
            side == 0 ? multiOrderedSetPopMin(&mos, 3, members, scores)
                      : multiOrderedSetPopMax(&mos, 3, members, scores);
        if (popped != 3) {
            ERR("pop %d returned %zu", side, popped);
        }

        for (size_t j = 0; j < popped; j++) {
            const mosWidePair *p =
                side == 0 ? &sorted[j] : &sorted[live - 1 - j];
            if (!mosWideScoreMatches_(&scores[j], p) ||
                !mosWideMemberMatches_(&members[j], p) ||
                !mosWideOwned_(&scores[j]) || !mosWideOwned_(&members[j])) {
                ERR("pop %d entry %zu wrong", side, j);
            }
            databoxFreeData(&scores[j]);
            databoxFreeData(&members[j]);
        }

        if (side == 0) {
            memmove(sorted, sorted + popped, (live - popped) * sizeof(*sorted));
        }
        live -= popped;
    }
    err += mosWideVerify_(mos, sorted, live);

    /* Range removal with wide bounds */
    if (live > 8) {
        const size_t a = live / 4;
        const size_t b = live / 2;
        databoxBig lo;
        databoxBig hi;
        mosWideNumBox_(&lo, sorted[a].score);
        mosWideNumBox_(&hi, sorted[b].score);
        const mosRangeSpec range = {.min = *(databox *)&lo,
                                    .max = *(databox *)&hi,
                                    .minExclusive = false,
                                    .maxExclusive = true};
        size_t first = a;
        while (first > 0 &&
               mosWideNumCmp_(sorted[first - 1].score, sorted[a].score) == 0) {
            first--;
        }
        size_t end = b;
        while (end > 0 &&
               mosWideNumCmp_(sorted[end - 1].score, sorted[b].score) == 0) {
            end--;
        }

        ctestPoisonStack();
        const size_t removed = multiOrderedSetRemoveRangeByScore(&mos, &range);
        if (removed != end - first) {
            ERR("RemoveRangeByScore removed %zu, expected %zu", removed,
                end - first);
        }
        memmove(&sorted[first], &sorted[end], (live - end) * sizeof(*sorted));
        live -= end - first;
    }
    err += mosWideVerify_(mos, sorted, live);

    /* Rank range removal */
    if (live > 4) {
        ctestPoisonStack();
        const size_t removed = multiOrderedSetRemoveRangeByRank(&mos, 1, 2);
        if (removed != 2) {
            ERR("RemoveRangeByRank removed %zu, expected 2", removed);
        }
        mosWideDrop_(sorted, &live, 1);
        mosWideDrop_(sorted, &live, 1);
    }
    err += mosWideVerify_(mos, sorted, live);
    return err;
}

defcleanerName(databox *, zfree, Boxes);
defcleanerName(uint8_t *, zfree, Seen);
defcleanerName(uint32_t *, zfree, U32);
defcleanerName(double *, zfree, Doubles);
defcleanerName(bool *, zfree, Bools);

/* Pair 'i' of the RandomMembers sets: member "rm-NNNNNN", score 3 * i */
static void mosRandomPair_(size_t i, char *buf, size_t bufLen, databox *member,
                           databox *score) {
    const int len = snprintf(buf, bufLen, "rm-%06zu", i);
    *member = databoxNewBytes(buf, (size_t)len);
    *score = databoxNewSigned((int64_t)i * 3);
}

/* The pair index a RandomMembers result names, or SIZE_MAX when the
 * member is not one of the set's or its score is not that member's */
static size_t mosRandomPairIndex_(const databox *member, const databox *score,
                                  size_t total) {
    if (!DATABOX_IS_BYTES(member) || member->len != 9) {
        return SIZE_MAX;
    }

    const char *b = databoxCBytes(member);
    if (memcmp(b, "rm-", 3) != 0) {
        return SIZE_MAX;
    }

    size_t i = 0;
    for (size_t d = 3; d < 9; d++) {
        if (b[d] < '0' || b[d] > '9') {
            return SIZE_MAX;
        }
        i = i * 10 + (size_t)(b[d] - '0');
    }

    if (i >= total || mosDataboxToDouble(score) != (double)i * 3.0) {
        return SIZE_MAX;
    }

    return i;
}

/* RandomMembers on a set of 'total' pairs: count >= 0 returns exactly
 * min(count, total) distinct pairs of the set, count < 0 exactly -count
 * pairs of the set.  Returns failures. */
static int mosRandomMembersCheck_(const multiOrderedSet *mos, size_t total) {
    int err = 0;
    const int64_t counts[] = {0,
                              1,
                              2,
                              (int64_t)(total / 8) + 1,
                              (int64_t)(total / 4),
                              (int64_t)(total / 4) + 1,
                              (int64_t)(total / 2),
                              (int64_t)total - 1,
                              (int64_t)total,
                              (int64_t)total + 5,
                              -1,
                              -3,
                              -(int64_t)total,
                              -(int64_t)(total * 2 + 3)};
    const size_t cap = total * 2 + 8;
    cleanerName(zfree, Boxes) databox *members = zcalloc(cap, sizeof(*members));
    cleanerName(zfree, Boxes) databox *scores = zcalloc(cap, sizeof(*scores));
    cleanerName(zfree, Seen) uint8_t *seen = zcalloc(total, 1);

    for (size_t c = 0; c < countof(counts); c++) {
        const int64_t count = counts[c];
        const bool distinct = count >= 0;
        const size_t want =
            distinct ? ((size_t)count < total ? (size_t)count : total)
                     : (size_t)(-count);
        for (int round = 0; round < 3; round++) {
            memset(seen, 0, total);
            const size_t got =
                multiOrderedSetRandomMembers(mos, count, members, scores);
            if (got != want) {
                ERR("RandomMembers(%" PRId64 ") of %zu returned %zu, want %zu",
                    count, total, got, want);
                continue;
            }

            for (size_t i = 0; i < got; i++) {
                const size_t at =
                    mosRandomPairIndex_(&members[i], &scores[i], total);
                if (at == SIZE_MAX) {
                    ERR("RandomMembers(%" PRId64 ") of %zu: slot %zu is not "
                        "a pair of the set",
                        count, total, i);
                    continue;
                }

                if (distinct && seen[at]) {
                    ERR("RandomMembers(%" PRId64 ") of %zu repeated pair %zu",
                        count, total, at);
                }
                seen[at] = 1;
            }
        }
    }

    return err;
}

/* Score updates against an oracle: members "u0000".."u<n-1>", scores
 * drawn so that many tie, then Add / AddXX / AddGetPrevious / IncrBy with
 * near moves (the row usually keeps its place) and far ones.  After every
 * update the set iterates in (score, member) order exactly as the oracle
 * sorts, and the tier never changes. */
static int64_t *mosUpdateOracleScores_;

static int mosUpdateOracleCompare_(const void *a, const void *b) {
    const uint32_t ia = *(const uint32_t *)a;
    const uint32_t ib = *(const uint32_t *)b;
    const int64_t sa = mosUpdateOracleScores_[ia];
    const int64_t sb = mosUpdateOracleScores_[ib];
    if (sa != sb) {
        return sa < sb ? -1 : 1;
    }

    /* Fixed-width names sort as their indexes */
    return ia < ib ? -1 : (ia > ib);
}

static int mosUpdateOracleCheck_(const multiOrderedSet *mos, uint32_t n,
                                 uint32_t *order, uint32_t step) {
    int err = 0;
    for (uint32_t i = 0; i < n; i++) {
        order[i] = i;
    }
    qsort(order, n, sizeof(*order), mosUpdateOracleCompare_);

    mosIterator iter;
    multiOrderedSetIteratorInit(mos, &iter, true);
    databox member;
    databox score;
    uint32_t at = 0;
    while (multiOrderedSetIteratorNext(&iter, &member, &score)) {
        char want[16];
        snprintf(want, sizeof(want), "u%04" PRIu32, at < n ? order[at] : 0);
        if (at >= n || databoxLen(&member) != strlen(want) ||
            memcmp(databoxCBytes(&member), want, strlen(want)) != 0 ||
            score.data.i != mosUpdateOracleScores_[order[at]]) {
            ERR("update oracle: step %" PRIu32 " row %" PRIu32
                " is not %s with score %" PRId64,
                step, at, want, at < n ? mosUpdateOracleScores_[order[at]] : 0);
            break;
        }
        at++;
    }
    multiOrderedSetIteratorRelease(&iter);

    if (at != n) {
        ERR("update oracle: step %" PRIu32 " iterated %" PRIu32
            " rows, expected %" PRIu32,
            step, at, n);
    }

    /* One member's rank per step, walking every rank over the steps */
    const uint32_t rankAt = step % n;
    char name[16];
    snprintf(name, sizeof(name), "u%04" PRIu32, order[rankAt]);
    const databox m = databoxNewBytesString(name);
    if (multiOrderedSetGetRank(mos, &m) != (int64_t)rankAt) {
        ERR("update oracle: step %" PRIu32 " rank of %s is %" PRId64
            ", expected %" PRIu32,
            step, name, multiOrderedSetGetRank(mos, &m), rankAt);
    }

    return err;
}

static int mosUpdateOracle_(uint32_t n, multiOrderedSetType tier) {
    int err = 0;
    cleanerName(zfree, Int64) int64_t *scores = zcalloc(n, sizeof(*scores));
    cleanerName(zfree, U32) uint32_t *order = zcalloc(n, sizeof(*order));
    mosUpdateOracleScores_ = scores;
    cleaner(multiOrderedSetFree) multiOrderedSet *mos = multiOrderedSetNew();
    uint64_t rng = 0x5DEECE66DULL + n;
    const int64_t span = (int64_t)n * 2; /* about two members per score */

    for (uint32_t i = 0; i < n; i++) {
        char name[16];
        snprintf(name, sizeof(name), "u%04" PRIu32, i);
        scores[i] = (int64_t)(xorshift64star(&rng) % (uint64_t)span);
        const databox s = databoxNewSigned(scores[i]);
        const databox m = databoxNewBytesString(name);
        multiOrderedSetAdd(&mos, &s, &m);
    }

    if (mosType_(mos) != tier) {
        ERR("update oracle: %" PRIu32 " members built tier %d, expected %d", n,
            mosType_(mos), tier);
        return err;
    }

    for (uint32_t step = 0; step < 600 && !err; step++) {
        const uint32_t i = (uint32_t)(xorshift64star(&rng) % n);
        char name[16];
        snprintf(name, sizeof(name), "u%04" PRIu32, i);
        const databox m = databoxNewBytesString(name);
        const uint64_t r = xorshift64star(&rng);
        /* Near moves mostly keep the row's place; far ones move it */
        const int64_t move = (r & 1) ? (int64_t)(r >> 8) % 3 - 1
                                     : (int64_t)(r >> 8) % span - span / 2;
        const int64_t next = scores[i] + move;
        switch ((r >> 4) % 4) {
        case 0: {
            const databox s = databoxNewSigned(next);
            if (!multiOrderedSetAdd(&mos, &s, &m)) {
                ERR("update oracle: Add of present %s reported new", name);
            }
            break;
        }
        case 1: {
            const databox s = databoxNewSigned(next);
            if (!multiOrderedSetAddXX(&mos, &s, &m)) {
                ERR("update oracle: AddXX of present %s refused", name);
            }
            break;
        }
        case 2: {
            const databox s = databoxNewSigned(next);
            databox prev = {};
            if (!multiOrderedSetAddGetPrevious(&mos, &s, &m, &prev) ||
                prev.data.i != scores[i]) {
                ERR("update oracle: AddGetPrevious of %s gave %" PRId64
                    ", expected %" PRId64,
                    name, (int64_t)prev.data.i, scores[i]);
            }
            break;
        }
        default: {
            const databox delta = databoxNewSigned(move);
            databox result = {};
            if (!multiOrderedSetIncrBy(&mos, &delta, &m, &result) ||
                result.data.i != next) {
                ERR("update oracle: IncrBy of %s gave %" PRId64
                    ", expected %" PRId64,
                    name, (int64_t)result.data.i, next);
            }
            break;
        }
        }

        scores[i] = next;
        err += mosUpdateOracleCheck_(mos, n, order, step);
        if (mosType_(mos) != tier) {
            ERR("update oracle: step %" PRIu32 " moved to tier %d", step,
                mosType_(mos));
        }
    }

    /* A string member and an integer member that spell the same number
     * are two members: each lookup finds its own */
    const databox asString = databoxNewBytesString("22");
    const databox asInteger = databoxNewSigned(22);
    const databox scoreA = databoxNewSigned(-5);
    const databox scoreB = databoxNewSigned(-6);
    multiOrderedSetAdd(&mos, &scoreA, &asString);
    multiOrderedSetAdd(&mos, &scoreB, &asInteger);
    databox gotA = {};
    databox gotB = {};
    if (multiOrderedSetCount(mos) != n + 2 ||
        !multiOrderedSetGetScore(mos, &asString, &gotA) ||
        !multiOrderedSetGetScore(mos, &asInteger, &gotB) || gotA.data.i != -5 ||
        gotB.data.i != -6) {
        ERR("update oracle: \"22\" and 22 are not two members (count %zu, "
            "scores %" PRId64 " %" PRId64 ")",
            multiOrderedSetCount(mos), (int64_t)gotA.data.i,
            (int64_t)gotB.data.i);
    }

    return err;
}

/* Counts the times multiOrderedSetRemoveIf asked, answering 'answer' */
typedef struct mosProceedProbe_ {
    bool answer;
    uint32_t asked;
} mosProceedProbe_;

static bool mosProceedProbe(void *ctx) {
    mosProceedProbe_ *probe = ctx;
    probe->asked++;
    return probe->answer;
}

/* RemoveIf on 'n' members (scores 0..n-1) built to 'tier': every third
 * member is declined (count, score and rank stay), then approved (gone),
 * then asked for again (absent: no question) */
static int mosRemoveIfTier_(int64_t n, multiOrderedSetType tier) {
    int err = 0;
    cleaner(multiOrderedSetFree) multiOrderedSet *mos = multiOrderedSetNew();
    for (int64_t i = 0; i < n; i++) {
        char buf[24];
        snprintf(buf, sizeof(buf), "i%05" PRId64, i);
        const databox member = databoxNewBytesString(buf);
        const databox score = databoxNewSigned(i);
        multiOrderedSetAdd(&mos, &score, &member);
    }
    if (mosType_(mos) != tier) {
        ERR("%" PRId64 " members built tier %d", n, mosType_(mos));
        return err;
    }

    size_t live = (size_t)n;
    for (int64_t i = 0; i < n; i += 3) {
        char buf[24];
        snprintf(buf, sizeof(buf), "i%05" PRId64, i);
        const databox member = databoxNewBytesString(buf);
        const int64_t rank = multiOrderedSetGetRank(mos, &member);
        mosProceedProbe_ probe = {.answer = false};
        databox score = {};
        if (multiOrderedSetRemoveIf(&mos, &member, mosProceedProbe, &probe) ||
            probe.asked != 1 || multiOrderedSetCount(mos) != live ||
            !multiOrderedSetGetScore(mos, &member, &score) ||
            score.data.i64 != i ||
            multiOrderedSetGetRank(mos, &member) != rank) {
            ERR("tier %d member %" PRId64 ": a declined remove changed it",
                tier, i);
            break;
        }
        probe.answer = true;
        if (!multiOrderedSetRemoveIf(&mos, &member, mosProceedProbe, &probe) ||
            probe.asked != 2 || multiOrderedSetExists(mos, &member) ||
            multiOrderedSetCount(mos) != --live ||
            multiOrderedSetRemoveIf(&mos, &member, mosProceedProbe, &probe) ||
            probe.asked != 2) {
            ERR("tier %d member %" PRId64 ": approved or absent misread", tier,
                i);
            break;
        }
    }

    return err;
}

/* The set-algebra oracles' three sets over members "m0".."m<n-1>": set i
 * holds the members j not divisible by i + 2, scored j * (i + 1) + 0.25
 * (all exact in a double, so every combination order agrees) */
static void mosAlgebraSets_(uint32_t n, multiOrderedSet *in[3]) {
    for (uint32_t i = 0; i < 3; i++) {
        /* @own: caller; free=multiOrderedSetFree */
        in[i] = multiOrderedSetNew();
        for (uint32_t j = 0; j < n; j++) {
            if (j % (i + 2) != 0) {
                char key[16];
                const int klen = snprintf(key, sizeof(key), "m%u", j);
                const databox member = databoxNewBytes(key, klen);
                const databox score =
                    databoxNewReal((double)j * (i + 1) + 0.25);
                multiOrderedSetAdd(&in[i], &score, &member);
            }
        }
    }
}

/* The oracle of combining those sets weighted by 'weights' with 'how': a
 * member of any set (union) or of all three (intersect) gets its first
 * weighted score, then each later one combined.  Fills want/seen; returns
 * the member count. */
static uint32_t mosAlgebraOracle_(uint32_t n, const double weights[3],
                                  mosAggregate how, bool intersect,
                                  double *want, bool *seen) {
    uint32_t members = 0;
    for (uint32_t j = 0; j < n; j++) {
        const bool inAll = j % 2 != 0 && j % 3 != 0 && j % 4 != 0;
        for (uint32_t i = 0; i < 3; i++) {
            if (j % (i + 2) == 0 || (intersect && !inAll)) {
                continue;
            }
            const double w = ((double)j * (i + 1) + 0.25) * weights[i];
            if (!seen[j]) {
                seen[j] = true;
                want[j] = w;
                members++;
            } else if (how == MOS_AGGREGATE_MIN) {
                want[j] = want[j] < w ? want[j] : w;
            } else if (how == MOS_AGGREGATE_MAX) {
                want[j] = want[j] > w ? want[j] : w;
            } else {
                want[j] = want[j] + w;
            }
        }
    }
    return members;
}

/* 'got' holds exactly the oracle's members and scores */
static int mosAlgebraCheck_(const multiOrderedSet *got, uint32_t n,
                            const double *want, const bool *seen,
                            uint32_t members, const char *what, int how) {
    int err = 0;
    if (multiOrderedSetCount(got) != members) {
        ERR("%s size %u aggregate %d: %zu members, want %u", what, n, how,
            multiOrderedSetCount(got), members);
        return err;
    }
    for (uint32_t j = 0; j < n; j++) {
        char key[16];
        const int klen = snprintf(key, sizeof(key), "m%u", j);
        const databox member = databoxNewBytes(key, klen);
        databox score;
        databoxWide scoreWide;
        mosBoxAim(&score, &scoreWide);
        const bool present = multiOrderedSetGetScore(got, &member, &score);
        if (present != seen[j] ||
            (present && mosDataboxToDouble(&score) != want[j])) {
            ERR("%s size %u aggregate %d member %u: present %d score %g "
                "(want %d, %g)",
                what, n, how, j, present,
                present ? mosDataboxToDouble(&score) : 0.0, seen[j], want[j]);
            break;
        }
    }
    return err;
}

int multiOrderedSetTest(int argc, char *argv[]) {
    int err = 0;

    TEST("multiOrderedSet: create and free") {
        cleaner(multiOrderedSetFree) multiOrderedSet *mos =
            multiOrderedSetNew();
        assert(mos != nullptr);
        assert(multiOrderedSetCount(mos) == 0);
    }

    TEST("multiOrderedSet: add and count") {
        cleaner(multiOrderedSetFree) multiOrderedSet *mos =
            multiOrderedSetNew();

        databox score, member;
        DATABOX_SET_DOUBLE(&score, 1.0);
        member = databoxNewBytes("member1", 7);

        bool existed = multiOrderedSetAdd(&mos, &score, &member);
        assert(!existed); /* New member */
        assert(multiOrderedSetCount(mos) == 1);

        /* Add same member with different score */
        DATABOX_SET_DOUBLE(&score, 2.0);
        existed = multiOrderedSetAdd(&mos, &score, &member);
        assert(existed); /* Member existed, score updated */
        assert(multiOrderedSetCount(mos) == 1);
    }

    TEST("multiOrderedSet: NX and XX semantics") {
        cleaner(multiOrderedSetFree) multiOrderedSet *mos =
            multiOrderedSetNew();

        databox score, member;
        DATABOX_SET_DOUBLE(&score, 1.0);
        member = databoxNewBytes("test", 4);

        /* NX: add only if not exists */
        bool added = multiOrderedSetAddNX(&mos, &score, &member);
        assert(added);
        assert(multiOrderedSetCount(mos) == 1);

        /* NX again: should fail */
        DATABOX_SET_DOUBLE(&score, 2.0);
        added = multiOrderedSetAddNX(&mos, &score, &member);
        assert(!added);

        /* Verify score unchanged */
        databox gotScore;
        multiOrderedSetGetScore(mos, &member, &gotScore);
        assert(mosDataboxToDouble(&gotScore) == 1.0);

        /* XX: update only if exists */
        DATABOX_SET_DOUBLE(&score, 3.0);
        bool updated = multiOrderedSetAddXX(&mos, &score, &member);
        assert(updated);
        multiOrderedSetGetScore(mos, &member, &gotScore);
        assert(mosDataboxToDouble(&gotScore) == 3.0);

        /* XX on non-existent: should fail */
        databox member2;
        member2 = databoxNewBytes("noexist", 7);
        updated = multiOrderedSetAddXX(&mos, &score, &member2);
        assert(!updated);
        assert(multiOrderedSetCount(mos) == 1);
    }

    TEST("multiOrderedSet: get score and rank") {
        cleaner(multiOrderedSetFree) multiOrderedSet *mos =
            multiOrderedSetNew();

        /* Add members with different scores */
        databox scores[3], members[3];
        DATABOX_SET_DOUBLE(&scores[0], 10.0);
        DATABOX_SET_DOUBLE(&scores[1], 20.0);
        DATABOX_SET_DOUBLE(&scores[2], 30.0);
        members[0] = databoxNewBytes("a", 1);
        members[1] = databoxNewBytes("b", 1);
        members[2] = databoxNewBytes("c", 1);

        for (int i = 0; i < 3; i++) {
            multiOrderedSetAdd(&mos, &scores[i], &members[i]);
        }

        /* Test getScore */
        databox gotScore;
        bool found = multiOrderedSetGetScore(mos, &members[1], &gotScore);
        assert(found);
        assert(mosDataboxToDouble(&gotScore) == 20.0);

        /* Test getRank */
        int64_t rank = multiOrderedSetGetRank(mos, &members[0]);
        assert(rank == 0);
        rank = multiOrderedSetGetRank(mos, &members[1]);
        assert(rank == 1);
        rank = multiOrderedSetGetRank(mos, &members[2]);
        assert(rank == 2);

        /* Test getReverseRank */
        rank = multiOrderedSetGetReverseRank(mos, &members[0]);
        assert(rank == 2);
        rank = multiOrderedSetGetReverseRank(mos, &members[2]);
        assert(rank == 0);
    }

    TEST("multiOrderedSet: remove") {
        cleaner(multiOrderedSetFree) multiOrderedSet *mos =
            multiOrderedSetNew();

        databox score, member;
        DATABOX_SET_DOUBLE(&score, 5.0);
        member = databoxNewBytes("target", 6);
        multiOrderedSetAdd(&mos, &score, &member);
        assert(multiOrderedSetCount(mos) == 1);

        bool removed = multiOrderedSetRemove(&mos, &member);
        assert(removed);
        assert(multiOrderedSetCount(mos) == 0);

        /* Remove again: should fail */
        removed = multiOrderedSetRemove(&mos, &member);
        assert(!removed);
    }

    TEST("multiOrderedSet: iteration") {
        cleaner(multiOrderedSetFree) multiOrderedSet *mos =
            multiOrderedSetNew();

        /* Add members */
        databox score, member;
        for (int i = 0; i < 5; i++) {
            DATABOX_SET_DOUBLE(&score, (double)(i * 10));
            char buf[16];
            int len = snprintf(buf, sizeof(buf), "m%d", i);
            member = databoxNewBytes(buf, len);
            multiOrderedSetAdd(&mos, &score, &member);
        }

        /* Forward iteration */
        mosIterator iter;
        multiOrderedSetIteratorInit(mos, &iter, true);

        int count = 0;
        double prevScore = -1.0;
        databox gotMember, gotScore;
        while (multiOrderedSetIteratorNext(&iter, &gotMember, &gotScore)) {
            double s = mosDataboxToDouble(&gotScore);
            assert(s > prevScore);
            prevScore = s;
            count++;
        }
        assert(count == 5);
    }

    TEST("multiOrderedSet: RemoveIf asks once found; a no keeps the member") {
        err += mosRemoveIfTier_(60, MOS_TYPE_SMALL);
        err += mosRemoveIfTier_(400, MOS_TYPE_MEDIUM);
        err += mosRemoveIfTier_(1200, MOS_TYPE_FULL);
    }

    TEST("multiOrderedSet: IncrBy inserts, updates and refuses in every tier") {
        static const struct {
            int64_t n;
            multiOrderedSetType tier;
        } tiers[] = {{60, MOS_TYPE_SMALL},
                     {400, MOS_TYPE_MEDIUM},
                     {1200, MOS_TYPE_FULL}};
        for (size_t t = 0; t < countof(tiers); t++) {
            const int64_t n = tiers[t].n;
            cleaner(multiOrderedSetFree) multiOrderedSet *mos =
                multiOrderedSetNew();
            for (int64_t i = 0; i < n; i++) {
                char buf[24];
                snprintf(buf, sizeof(buf), "i%05" PRId64, i);
                const databox member = databoxNewBytesString(buf);
                const databox score = databoxNewSigned(i);
                multiOrderedSetAdd(&mos, &score, &member);
            }
            if (mosType_(mos) != tiers[t].tier) {
                ERR("%" PRId64 " members built tier %d", n, mosType_(mos));
                continue;
            }

            /* A missing member joins at the delta: after the 6 members
             * scored 0..5 ("i00005" < "newbie" at the tie) */
            databox result = {};
            const databox five = databoxNewSigned(5);
            const databox newbie = databoxNewBytesString("newbie");
            if (!multiOrderedSetIncrBy(&mos, &five, &newbie, &result) ||
                result.data.i64 != 5 ||
                multiOrderedSetCount(mos) != (size_t)n + 1 ||
                multiOrderedSetGetRank(mos, &newbie) != 6) {
                ERR("tier %d: IncrBy of a missing member", tiers[t].tier);
            }

            /* An existing member moves */
            const databox three = databoxNewSigned(3);
            const databox ten = databoxNewBytesString("i00010");
            databox got;
            if (!multiOrderedSetIncrBy(&mos, &three, &ten, &result) ||
                !multiOrderedSetGetScore(mos, &ten, &got) ||
                got.data.i64 != 13) {
                ERR("tier %d: IncrBy of an existing member", tiers[t].tier);
            }

            /* Up to UINT64_MAX fits; one past it, or a 96/128-bit delta,
             * or an inf / NaN result is refused and changes nothing */
            const databox zero = databoxNewBytesString("i00000");
            const databox top = databoxNewUnsigned(UINT64_MAX);
            const databox one = databoxNewSigned(1);
            databoxBig wide;
            DATABOX_BIG_SIGNED_128(&wide, (__int128_t)1 << 100);
            const databox inf = DATABOX_DOUBLE(INFINITY);
            const databox nan = DATABOX_DOUBLE(NAN);
            const databox fresh = databoxNewBytesString("fresh");
            const size_t before = (size_t)n + 1;
            if (!multiOrderedSetIncrBy(&mos, &top, &zero, &result) ||
                result.type != DATABOX_UNSIGNED_64 ||
                result.data.u64 != UINT64_MAX ||
                multiOrderedSetIncrBy(&mos, &one, &zero, &result) ||
                multiOrderedSetIncrBy(&mos, (const databox *)&wide, &fresh,
                                      &result) ||
                multiOrderedSetIncrBy(&mos, &inf, &ten, &result) ||
                multiOrderedSetIncrBy(&mos, &nan, &ten, &result) ||
                multiOrderedSetCount(mos) != before ||
                multiOrderedSetExists(mos, &fresh) ||
                !multiOrderedSetGetScore(mos, &zero, &got) ||
                got.type != DATABOX_UNSIGNED_64 || got.data.u64 != UINT64_MAX ||
                !multiOrderedSetGetScore(mos, &ten, &got) ||
                got.data.i64 != 13) {
                ERR("tier %d: a refused IncrBy changed the set", tiers[t].tier);
            }
        }
    }

    TEST("multiOrderedSet: score updates keep oracle order in every tier") {
        err += mosUpdateOracle_(60, MOS_TYPE_SMALL);
        err += mosUpdateOracle_(400, MOS_TYPE_MEDIUM);
        err += mosUpdateOracle_(1200, MOS_TYPE_FULL);
    }

    TEST("multiOrderedSet: RemoveRangeByRank clamps its range in every tier") {
        /* Each range on a fresh set of n members (score i, member "r%05d")
         * removes exactly the oracle's ranks: negative ranks from the end,
         * start and stop clamped to the set, empty when outside it */
        static constexpr int64_t ns[] = {5, 10, 14, 20, 40, 3000};
        unsigned tiersSeen = 0;
        static const struct {
            int64_t start;
            int64_t stop;
        } ranges[] = {
            {0, -1},         {0, INT64_MAX}, {-1000, 1},       {5, 2},
            {-2, 100},       {3, 3},         {-1, -1},         {1, -2},
            {INT64_MIN, -1}, {10000, 20000}, {-20000, -10000},
        };
        for (size_t t = 0; t < countof(ns); t++) {
            const int64_t n = ns[t];
            for (size_t r = 0; r < countof(ranges); r++) {
                multiOrderedSet *mos =
                    multiOrderedSetNewLimit(FLEX_CAP_LEVEL_128);
                for (int64_t i = 0; i < n; i++) {
                    char buf[24];
                    snprintf(buf, sizeof(buf), "r%05" PRId64, i);
                    const databox member = databoxNewBytesString(buf);
                    const databox score = databoxNewSigned(i);
                    multiOrderedSetAdd(&mos, &score, &member);
                }
                const int tier = (int)mosType_(mos);
                tiersSeen |= 1u << tier;
                /* Oracle */
                int64_t s = ranges[r].start;
                int64_t e = ranges[r].stop;
                if (s < 0) {
                    s = s < -n ? 0 : s + n;
                }
                if (e < 0) {
                    e = e < -n ? -1 : e + n;
                }
                if (e >= n) {
                    e = n - 1;
                }
                const size_t want = s <= e && s < n ? (size_t)(e - s + 1) : 0;
                const size_t got = multiOrderedSetRemoveRangeByRank(
                    &mos, ranges[r].start, ranges[r].stop);
                /* The survivors are the ranks outside [s, e] */
                bool rest = multiOrderedSetCount(mos) == (size_t)n - got;
                mosIterator it;
                multiOrderedSetIteratorInit(mos, &it, true);
                databox member;
                databox score;
                int64_t expect = 0;
                while (rest &&
                       multiOrderedSetIteratorNext(&it, &member, &score)) {
                    if (want && expect == s) {
                        expect = e + 1;
                    }
                    rest = score.data.i64 == expect;
                    expect++;
                }
                if (got != want || !rest) {
                    ERR("tier %d, n %" PRId64 ", range (%" PRId64 ", %" PRId64
                        "): removed %zu, want %zu",
                        tier, n, ranges[r].start, ranges[r].stop, got, want);
                }
                multiOrderedSetFree(mos);
            }
        }
        const unsigned all = (1u << MOS_TYPE_SMALL) | (1u << MOS_TYPE_MEDIUM) |
                             (1u << MOS_TYPE_FULL);
        if ((tiersSeen & all) != all) {
            ERR("rank clamp: tiers seen 0x%x, want 0x%x", tiersSeen, all);
        }
    }

    TEST("multiOrderedSet: equal scores stay in member order across a "
         "Small->Medium split") {
        /* Promotion splits Small's rows in half whatever their scores, so
         * equal scores can land in both of Medium's maps; members added
         * at that score afterwards must still iterate in member order */
        multiOrderedSet *mos = multiOrderedSetNewLimit(FLEX_CAP_LEVEL_128);
        const databox five = databoxNewSigned(5);
        int added = 0;
        while (mosType_(mos) == MOS_TYPE_SMALL && added < 500) {
            char buf[16];
            snprintf(buf, sizeof(buf), "k%03d", added++);
            const databox member = databoxNewBytesString(buf);
            multiOrderedSetAdd(&mos, &five, &member);
        }
        if (mosType_(mos) != MOS_TYPE_MEDIUM) {
            ERR("equal-score split: tier %d after %d adds", (int)mosType_(mos),
                added);
        }
        static const char *later[] = {"zzz", "a", "k0005x", "k9"};
        for (size_t i = 0; i < 4; i++) {
            const databox member = databoxNewBytesString(later[i]);
            multiOrderedSetAdd(&mos, &five, &member);
        }
        mosIterator iter;
        multiOrderedSetIteratorInit(mos, &iter, true);
        databox gotMember;
        databox gotScore;
        /* Member order is databoxCompare's (natural sort: "k9" < "k000") */
        char prev[32] = "";
        size_t prevLen = 0;
        size_t seen = 0;
        bool ordered = true;
        while (multiOrderedSetIteratorNext(&iter, &gotMember, &gotScore)) {
            const size_t len = databoxLen(&gotMember);
            const databox before = databoxNewBytes(prev, prevLen);
            if (seen && databoxCompare(&before, &gotMember) >= 0) {
                ordered = false;
            }
            prevLen = len < sizeof(prev) ? len : sizeof(prev);
            memcpy(prev, databoxBytes(&gotMember), prevLen);
            seen++;
        }
        if (!ordered || seen != (size_t)added + 4 ||
            multiOrderedSetCount(mos) != (size_t)added + 4) {
            ERR("equal-score split: %zu iterated of %d, ordered %d", seen,
                added + 4, ordered);
        }
        multiOrderedSetFree(mos);
    }

    TEST("multiOrderedSet: tier promotion") {
        /* Use small size limit to force promotion */
        multiOrderedSet *mos = multiOrderedSetNewLimit(FLEX_CAP_LEVEL_128);
        assert(mosType_(mos) == MOS_TYPE_SMALL);

        /* Add enough entries to trigger promotion */
        databox score, member;
        for (int i = 0; i < 100; i++) {
            DATABOX_SET_DOUBLE(&score, (double)i);
            char buf[32];
            int len = snprintf(buf, sizeof(buf), "member_%d", i);
            member = databoxNewBytes(buf, len);
            multiOrderedSetAdd(&mos, &score, &member);
        }

        /* Should have been promoted at least to Medium */
        assert(mosType_(mos) != MOS_TYPE_SMALL);

        /* Verify all entries still accessible */
        assert(multiOrderedSetCount(mos) == 100);

        /* Verify sorted order maintained */
        mosIterator iter;
        multiOrderedSetIteratorInit(mos, &iter, true);
        databox gotMember, gotScore;
        double prevScore = -1.0;
        while (multiOrderedSetIteratorNext(&iter, &gotMember, &gotScore)) {
            double s = mosDataboxToDouble(&gotScore);
            assert(s > prevScore);
            prevScore = s;
        }

        multiOrderedSetFree(mos);
    }

    TEST("multiOrderedSet: Compact keeps members and order, picks the "
         "smallest tier, packs sub-maps") {
        cleaner(multiOrderedSetFree) multiOrderedSet *mos =
            multiOrderedSetNewLimit(FLEX_CAP_LEVEL_512);
        const int64_t members = 3000;
        for (int64_t i = 0; i < members; i++) {
            databox score;
            /* Scores out of member order, so the walk order is the scores' */
            DATABOX_SET_DOUBLE(&score, (double)((i * 7919) % members));
            const databox member = databoxNewSigned(i);
            multiOrderedSetAdd(&mos, &score, &member);
        }

        cleanerName(zfree, Int64) int64_t *before =
            zcalloc((size_t)members, sizeof(int64_t));
        cleanerName(zfree, Int64) int64_t *after =
            zcalloc((size_t)members, sizeof(int64_t));
        const int64_t keepEvery[] = {1, 4, 40, 400, 3000};
        for (size_t p = 0; p < countof(keepEvery); p++) {
            for (int64_t i = 0; i < members; i++) {
                if (i % keepEvery[p]) {
                    const databox member = databoxNewSigned(i);
                    multiOrderedSetRemove(&mos, &member);
                }
            }

            const size_t n = multiOrderedSetCount(mos);
            dkTierStats was;
            multiOrderedSetStats(mos, &was);
            for (int32_t side = 0; side < 2; side++) {
                int64_t *walk = side ? after : before;
                mosIterator it;
                multiOrderedSetIteratorInit(mos, &it, true);
                databox member;
                databox score;
                size_t at = 0;
                while (at < n &&
                       multiOrderedSetIteratorNext(&it, &member, &score)) {
                    walk[at++] = member.data.i64;
                }
                multiOrderedSetIteratorRelease(&it);
                if (side == 0) {
                    multiOrderedSetCompact(&mos);
                }
            }

            dkTierStats s;
            multiOrderedSetStats(mos, &s);
            const uint64_t L = s.unitLimit;
            bool tierOk;
            switch (s.tier) {
            case MOS_TYPE_SMALL:
                tierOk =
                    s.dataBytes <= L || s.count < MOS_MIN_ENTRIES_FOR_PROMOTION;
                break;
            case MOS_TYPE_MEDIUM:
                tierOk = s.dataBytes > L;
                break;
            default:
                /* Packed: every sub-map but the last within a row (under 16
                 * bytes here) and the refill slack of the limit */
                tierOk = s.dataBytes > 3 * L &&
                         s.units <= s.dataBytes / (L - 16 - 8) + 1;
                break;
            }
            printf("  keep 1 in %4" PRId64 ": tier %u, %4u sub-maps, %6" PRIu64
                   " bytes -> tier %u, %4u, %6" PRIu64 "\n",
                   keepEvery[p], was.tier, was.units, was.totalBytes, s.tier,
                   s.units, s.totalBytes);
            if (multiOrderedSetCount(mos) != n ||
                memcmp(before, after, n * sizeof(int64_t)) != 0 || !tierOk ||
                s.units > was.units) {
                ERR("keep 1 in %" PRId64 ": compacted tier %u units %u (was "
                    "%u), %" PRIu64 " data bytes, %zu members",
                    keepEvery[p], s.tier, s.units, was.units, s.dataBytes,
                    multiOrderedSetCount(mos));
            }
            multiOrderedSetVerify(mos);

            /* Ranks and adds work on the compacted set */
            const databox first = databoxNewSigned(before[0]);
            databox score;
            DATABOX_SET_DOUBLE(&score, -1.0);
            const databox extra = databoxNewSigned(members + (int64_t)p);
            multiOrderedSetAdd(&mos, &score, &extra);
            if (multiOrderedSetGetRank(mos, &extra) != 0 ||
                multiOrderedSetGetRank(mos, &first) != 1) {
                ERR("keep 1 in %" PRId64 ": ranks after Compact are wrong",
                    keepEvery[p]);
            }
            multiOrderedSetRemove(&mos, &extra);
        }
    }

    TEST("multiOrderedSet: Reset from every tier gives an empty Small with "
         "the same limit and compression") {
        for (int32_t compress = 0; compress < 2; compress++) {
            for (int32_t tier = MOS_TYPE_SMALL; tier <= MOS_TYPE_FULL; tier++) {
                cleaner(multiOrderedSetFree) multiOrderedSet *mos =
                    compress ? multiOrderedSetNewCompress(FLEX_CAP_LEVEL_64)
                             : multiOrderedSetNewLimit(FLEX_CAP_LEVEL_64);
                const uint32_t depth = COMPRESS_DEPTH(mos);
                int64_t next = 0;
                do {
                    databox score;
                    DATABOX_SET_DOUBLE(&score, (double)next);
                    const databox member = databoxNewSigned(next);
                    multiOrderedSetAdd(&mos, &score, &member);
                    next++;
                } while (mosType_(mos) != (multiOrderedSetType)tier);

                multiOrderedSetReset(&mos);
                const databox member = databoxNewSigned(5);
                if (mosType_(mos) != MOS_TYPE_SMALL ||
                    multiOrderedSetCount(mos) != 0 ||
                    multiOrderedSetExists(mos, &member) ||
                    multiOrderedSetLimit(mos) != FLEX_CAP_LEVEL_64 ||
                    COMPRESS_DEPTH(mos) != depth) {
                    ERR("compress %d tier %d: reset to tier %d, count %zu",
                        compress, tier, mosType_(mos),
                        multiOrderedSetCount(mos));
                }

                databox score;
                DATABOX_SET_DOUBLE(&score, 1.5);
                multiOrderedSetAdd(&mos, &score, &member);
                if (multiOrderedSetCount(mos) != 1 ||
                    !multiOrderedSetExists(mos, &member)) {
                    ERR("compress %d tier %d: a member after Reset is missing",
                        compress, tier);
                }
            }
        }
    }

    TEST("multiOrderedSet: stats and limit in every tier") {
        cleaner(multiOrderedSetFree) multiOrderedSet *mos =
            multiOrderedSetNewLimit(FLEX_CAP_LEVEL_64);
        const uint32_t limit = flexOptimizationSizeLimit[FLEX_CAP_LEVEL_64];
        dkTierStats s;
        int64_t next = 0;
        for (int32_t want = MOS_TYPE_SMALL; want <= MOS_TYPE_FULL; want++) {
            while (mosType_(mos) != (multiOrderedSetType)want) {
                databox score;
                DATABOX_SET_DOUBLE(&score, (double)next);
                const databox member = databoxNewSigned(next);
                multiOrderedSetAdd(&mos, &score, &member);
                next++;
            }
            multiOrderedSetStats(mos, &s);
            bool shape;
            if (want == MOS_TYPE_SMALL) {
                shape = s.units == 1 && s.unitLimit == limit &&
                        s.totalBytes ==
                            s.dataBytes + sizeof(multiOrderedSetSmall) &&
                        s.dataBytes == multiOrderedSetBytes(mos);
            } else if (want == MOS_TYPE_MEDIUM) {
                shape = s.units == 2 && s.unitLimit == limit &&
                        s.totalBytes ==
                            s.dataBytes + sizeof(multiOrderedSetMedium) &&
                        s.dataBytes == multiOrderedSetBytes(mos);
            } else {
                /* Full's sub-maps split at the set's own size limit */
                const multiOrderedSetFull *f = mosf(mos);
                shape = s.units == f->mapCount && s.units >= 1 &&
                        s.unitLimit == limit && f->maxMapSize == limit &&
                        s.totalBytes > multiOrderedSetBytes(mos) &&
                        s.dataBytes < multiOrderedSetBytes(mos);
            }
            cleaner(multiOrderedSetFree) multiOrderedSet *copy =
                multiOrderedSetCopy(mos);
            if (multiOrderedSetLimit(mos) != FLEX_CAP_LEVEL_64 ||
                multiOrderedSetLimit(copy) != FLEX_CAP_LEVEL_64) {
                ERR("tier %d: limit %d, copy's %d", want,
                    multiOrderedSetLimit(mos), multiOrderedSetLimit(copy));
            }
            if (s.tier != want || s.count != (uint64_t)next || !shape ||
                s.totalBytes <= s.dataBytes) {
                ERR("tier %d: stats tier %u units %u count %" PRIu64
                    " limit %u data %" PRIu64 " total %" PRIu64,
                    want, s.tier, s.units, s.count, s.unitLimit, s.dataBytes,
                    s.totalBytes);
            }
        }
    }

    TEST("multiOrderedSet: the compress setting survives growth, shrink and "
         "copy") {
        cleaner(multiOrderedSetFree) multiOrderedSet *mos =
            multiOrderedSetNewCompress(FLEX_CAP_LEVEL_64);
        int64_t next = 0;
        while (mosType_(mos) != MOS_TYPE_FULL) {
            const databox b = databoxNewSigned(next++);
            multiOrderedSetAdd(&mos, &b, &b);
        }
        if (COMPRESS_DEPTH(mos) != 1 ||
            COMPRESS_LIMIT(mos) != FLEX_CAP_LEVEL_64) {
            ERR("Full kept depth %u limit %u", (uint32_t)COMPRESS_DEPTH(mos),
                (uint32_t)COMPRESS_LIMIT(mos));
        }
        while (next > 1) {
            const databox b = databoxNewSigned(--next);
            multiOrderedSetRemove(&mos, &b);
        }
        if (mosType_(mos) != MOS_TYPE_SMALL || COMPRESS_DEPTH(mos) != 1 ||
            COMPRESS_LIMIT(mos) != FLEX_CAP_LEVEL_64) {
            ERR("drained to tier %d with depth %u limit %u", mosType_(mos),
                (uint32_t)COMPRESS_DEPTH(mos), (uint32_t)COMPRESS_LIMIT(mos));
        }
        cleaner(multiOrderedSetFree) multiOrderedSet *copy =
            multiOrderedSetCopy(mos);
        if (COMPRESS_DEPTH(copy) != 1) {
            ERR("a copy lost the compress setting");
        }
    }

    TEST("multiOrderedSet: first and last") {
        cleaner(multiOrderedSetFree) multiOrderedSet *mos =
            multiOrderedSetNew();

        databox score, member;
        DATABOX_SET_DOUBLE(&score, 50.0);
        member = databoxNewBytes("middle", 6);
        multiOrderedSetAdd(&mos, &score, &member);

        DATABOX_SET_DOUBLE(&score, 10.0);
        member = databoxNewBytes("first", 5);
        multiOrderedSetAdd(&mos, &score, &member);

        DATABOX_SET_DOUBLE(&score, 90.0);
        member = databoxNewBytes("last", 4);
        multiOrderedSetAdd(&mos, &score, &member);

        databox gotMember, gotScore;
        bool found = multiOrderedSetFirst(mos, &gotMember, &gotScore);
        assert(found);
        assert(mosDataboxToDouble(&gotScore) == 10.0);

        found = multiOrderedSetLast(mos, &gotMember, &gotScore);
        assert(found);
        assert(mosDataboxToDouble(&gotScore) == 90.0);
    }

    TEST("multiOrderedSet: pop min/max") {
        cleaner(multiOrderedSetFree) multiOrderedSet *mos =
            multiOrderedSetNew();

        databox score, member;
        for (int i = 0; i < 5; i++) {
            DATABOX_SET_DOUBLE(&score, (double)(i + 1) * 10.0);
            char buf[16];
            int len = snprintf(buf, sizeof(buf), "m%d", i);
            member = databoxNewBytes(buf, len);
            multiOrderedSetAdd(&mos, &score, &member);
        }

        assert(multiOrderedSetCount(mos) == 5);

        databox poppedMembers[2], poppedScores[2];
        size_t popped =
            multiOrderedSetPopMin(&mos, 2, poppedMembers, poppedScores);
        assert(popped == 2);
        assert(mosDataboxToDouble(&poppedScores[0]) == 10.0);
        assert(mosDataboxToDouble(&poppedScores[1]) == 20.0);
        assert(multiOrderedSetCount(mos) == 3);
        /* Popped values are self-contained: release them */
        databoxFreeData(&poppedMembers[0]);
        databoxFreeData(&poppedMembers[1]);

        popped = multiOrderedSetPopMax(&mos, 1, poppedMembers, poppedScores);
        assert(popped == 1);
        assert(mosDataboxToDouble(&poppedScores[0]) == 50.0);
        assert(multiOrderedSetCount(mos) == 2);
        databoxFreeData(&poppedMembers[0]);
    }

    TEST("multiOrderedSet: count by score range") {
        cleaner(multiOrderedSetFree) multiOrderedSet *mos =
            multiOrderedSetNew();

        databox score, member;
        for (int i = 0; i < 10; i++) {
            DATABOX_SET_DOUBLE(&score, (double)i * 10.0);
            char buf[16];
            int len = snprintf(buf, sizeof(buf), "m%d", i);
            member = databoxNewBytes(buf, len);
            multiOrderedSetAdd(&mos, &score, &member);
        }

        /* Count entries with score in [20, 50] */
        mosRangeSpec range;
        DATABOX_SET_DOUBLE(&range.min, 20.0);
        DATABOX_SET_DOUBLE(&range.max, 50.0);
        range.minExclusive = false;
        range.maxExclusive = false;

        size_t count = multiOrderedSetCountByScore(mos, &range);
        assert(count == 4); /* 20, 30, 40, 50 */

        /* Count with exclusive bounds (20, 50) */
        range.minExclusive = true;
        range.maxExclusive = true;
        count = multiOrderedSetCountByScore(mos, &range);
        assert(count == 2); /* 30, 40 */
    }

    TEST("multiOrderedSet: union") {
        cleaner(multiOrderedSetFree) multiOrderedSet *set1 =
            multiOrderedSetNew();
        cleaner(multiOrderedSetFree) multiOrderedSet *set2 =
            multiOrderedSetNew();

        databox score, member;

        /* set1: a=1, b=2 */
        DATABOX_SET_DOUBLE(&score, 1.0);
        member = databoxNewBytes("a", 1);
        multiOrderedSetAdd(&set1, &score, &member);
        DATABOX_SET_DOUBLE(&score, 2.0);
        member = databoxNewBytes("b", 1);
        multiOrderedSetAdd(&set1, &score, &member);

        /* set2: b=3, c=4 */
        DATABOX_SET_DOUBLE(&score, 3.0);
        member = databoxNewBytes("b", 1);
        multiOrderedSetAdd(&set2, &score, &member);
        DATABOX_SET_DOUBLE(&score, 4.0);
        member = databoxNewBytes("c", 1);
        multiOrderedSetAdd(&set2, &score, &member);

        const multiOrderedSet *sets[] = {set1, set2};
        cleaner(multiOrderedSetFree) multiOrderedSet *result =
            multiOrderedSetUnion(sets, nullptr, 2, MOS_AGGREGATE_SUM);

        assert(multiOrderedSetCount(result) == 3); /* a, b, c */

        databox gotScore;
        member = databoxNewBytes("b", 1);
        multiOrderedSetGetScore(result, &member, &gotScore);
        assert(mosDataboxToDouble(&gotScore) == 5.0); /* 2 + 3 */
    }

    TEST("multiOrderedSet: weighted union and intersection in every tier "
         "match an oracle, per aggregate") {
        /* The three oracle sets weighted 1, 0.5, 3, combined by union and
         * by intersection with each aggregate; the union of each size
         * lands in its tier */
        static constexpr uint32_t sizes[] = {20, 300, 4000};
        static const multiOrderedSetType tiers[] = {
            MOS_TYPE_SMALL, MOS_TYPE_MEDIUM, MOS_TYPE_FULL};
        static constexpr double weights[3] = {1.0, 0.5, 3.0};
        static const mosAggregate hows[] = {
            MOS_AGGREGATE_SUM, MOS_AGGREGATE_MIN, MOS_AGGREGATE_MAX};
        for (size_t s = 0; s < countof(sizes); s++) {
            const uint32_t n = sizes[s];
            multiOrderedSet *in[3] = {nullptr, nullptr, nullptr};
            mosAlgebraSets_(n, in);
            const multiOrderedSet *sets[3] = {in[0], in[1], in[2]};
            for (size_t h = 0; h < countof(hows); h++) {
                for (int intersect = 0; intersect <= 1; intersect++) {
                    cleanerName(zfree, Doubles) double *want =
                        zcalloc(n, sizeof(*want));
                    cleanerName(zfree, Bools) bool *seen =
                        zcalloc(n, sizeof(*seen));
                    const uint32_t members = mosAlgebraOracle_(
                        n, weights, hows[h], intersect, want, seen);
                    cleaner(multiOrderedSetFree) multiOrderedSet *got =
                        intersect
                            ? multiOrderedSetIntersect(sets, weights, 3,
                                                       hows[h])
                            : multiOrderedSetUnion(sets, weights, 3, hows[h]);
                    if (!intersect && mosType_(got) != tiers[s]) {
                        ERR("size %u: union built tier %d, want %d", n,
                            (int)mosType_(got), (int)tiers[s]);
                    }
                    err += mosAlgebraCheck_(got, n, want, seen, members,
                                            intersect ? "intersect" : "union",
                                            (int)hows[h]);
                }
            }

            for (uint32_t i = 0; i < 3; i++) {
                multiOrderedSetFree(in[i]);
            }
        }
    }

    TEST("multiOrderedSet: intersection") {
        cleaner(multiOrderedSetFree) multiOrderedSet *set1 =
            multiOrderedSetNew();
        cleaner(multiOrderedSetFree) multiOrderedSet *set2 =
            multiOrderedSetNew();

        databox score, member;

        /* set1: a=1, b=2, c=3 */
        DATABOX_SET_DOUBLE(&score, 1.0);
        member = databoxNewBytes("a", 1);
        multiOrderedSetAdd(&set1, &score, &member);
        DATABOX_SET_DOUBLE(&score, 2.0);
        member = databoxNewBytes("b", 1);
        multiOrderedSetAdd(&set1, &score, &member);
        DATABOX_SET_DOUBLE(&score, 3.0);
        member = databoxNewBytes("c", 1);
        multiOrderedSetAdd(&set1, &score, &member);

        /* set2: b=10, c=20 */
        DATABOX_SET_DOUBLE(&score, 10.0);
        member = databoxNewBytes("b", 1);
        multiOrderedSetAdd(&set2, &score, &member);
        DATABOX_SET_DOUBLE(&score, 20.0);
        member = databoxNewBytes("c", 1);
        multiOrderedSetAdd(&set2, &score, &member);

        const multiOrderedSet *sets[] = {set1, set2};
        cleaner(multiOrderedSetFree) multiOrderedSet *result =
            multiOrderedSetIntersect(sets, nullptr, 2, MOS_AGGREGATE_SUM);

        assert(multiOrderedSetCount(result) == 2); /* b, c */

        databox gotScore;
        member = databoxNewBytes("b", 1);
        multiOrderedSetGetScore(result, &member, &gotScore);
        assert(mosDataboxToDouble(&gotScore) == 12.0); /* 2 + 10 */
    }

    TEST("multiOrderedSet: copy") {
        cleaner(multiOrderedSetFree) multiOrderedSet *mos =
            multiOrderedSetNew();

        databox score, member;
        for (int i = 0; i < 10; i++) {
            DATABOX_SET_DOUBLE(&score, (double)i);
            char buf[8];
            int len = snprintf(buf, sizeof(buf), "k%d", i);
            member = databoxNewBytes(buf, len);
            multiOrderedSetAdd(&mos, &score, &member);
        }

        cleaner(multiOrderedSetFree) multiOrderedSet *copy =
            multiOrderedSetCopy(mos);
        assert(multiOrderedSetCount(copy) == multiOrderedSetCount(mos));

        /* Modify original, verify copy unaffected */
        member = databoxNewBytes("k5", 2);
        multiOrderedSetRemove(&mos, &member);
        assert(multiOrderedSetCount(mos) == 9);
        assert(multiOrderedSetCount(copy) == 10);
    }

    TEST("multiOrderedSet: integer scores") {
        cleaner(multiOrderedSetFree) multiOrderedSet *mos =
            multiOrderedSetNew();

        /* Test with various integer score types */
        databox score, member;

        /* Signed 64-bit */
        DATABOX_SET_SIGNED(&score, -100);
        member = databoxNewBytes("neg", 3);
        multiOrderedSetAdd(&mos, &score, &member);

        /* Unsigned 64-bit */
        DATABOX_SET_UNSIGNED(&score, 1000);
        member = databoxNewBytes("pos", 3);
        multiOrderedSetAdd(&mos, &score, &member);

        assert(multiOrderedSetCount(mos) == 2);

        /* Verify ordering: -100 < 1000 */
        databox gotMember, gotScore;
        multiOrderedSetFirst(mos, &gotMember, &gotScore);
        assert(gotScore.data.i64 == -100);

        multiOrderedSetLast(mos, &gotMember, &gotScore);
        assert(gotScore.data.u64 == 1000);
    }

    TEST("multiOrderedSet: getByRank") {
        cleaner(multiOrderedSetFree) multiOrderedSet *mos =
            multiOrderedSetNew();

        databox score, member;
        for (int i = 0; i < 5; i++) {
            DATABOX_SET_DOUBLE(&score, (double)(i * 10));
            char buf[8];
            int len = snprintf(buf, sizeof(buf), "m%d", i);
            member = databoxNewBytes(buf, len);
            multiOrderedSetAdd(&mos, &score, &member);
        }

        databox gotMember, gotScore;

        /* Positive rank */
        bool found = multiOrderedSetGetByRank(mos, 2, &gotMember, &gotScore);
        assert(found);
        assert(mosDataboxToDouble(&gotScore) == 20.0);

        /* Negative rank (-1 = last) */
        found = multiOrderedSetGetByRank(mos, -1, &gotMember, &gotScore);
        assert(found);
        assert(mosDataboxToDouble(&gotScore) == 40.0);

        /* Out of bounds */
        found = multiOrderedSetGetByRank(mos, 10, &gotMember, &gotScore);
        assert(!found);
    }

    TEST("multiOrderedSet: RandomMembers counts and distinctness, all tiers") {
        /* Grow one set through every tier, checking the RandomMembers
         * contract at sizes on each side of the sampler's thresholds */
        const size_t sizes[] = {1, 2, 3, 5, 8, 17, 40, 100, 300, 1000, 3000};
        multiOrderedSet *mos = multiOrderedSetNewLimit(FLEX_CAP_LEVEL_64);
        bool tierSeen[3] = {false, false, false};
        size_t have = 0;
        char buf[16];
        for (size_t s = 0; s < countof(sizes); s++) {
            for (; have < sizes[s]; have++) {
                databox member;
                databox score;
                mosRandomPair_(have, buf, sizeof(buf), &member, &score);
                multiOrderedSetAdd(&mos, &score, &member);
            }

            if (multiOrderedSetCount(mos) != have) {
                ERR("RandomMembers set holds %zu, want %zu",
                    multiOrderedSetCount(mos), have);
            }

            const int type = mosType_(mos);
            if (type == MOS_TYPE_SMALL) {
                tierSeen[0] = true;
            } else if (type == MOS_TYPE_MEDIUM) {
                tierSeen[1] = true;
            } else if (type == MOS_TYPE_FULL) {
                tierSeen[2] = true;
            }

            err += mosRandomMembersCheck_(mos, have);
        }

        if (!tierSeen[0] || !tierSeen[1] || !tierSeen[2]) {
            ERR("RandomMembers sizes covered tiers small=%d medium=%d full=%d",
                tierSeen[0], tierSeen[1], tierSeen[2]);
        }

        multiOrderedSetFree(mos);
    }

    /* ================================================================
     * EXTENDED TESTS: Reporting and Statistics
     * ================================================================ */

    printf("\n=== Extended multiOrderedSet Tests ===\n\n");

    TEST("REPORT: Show tier statistics") {
        printf("    Testing statistics at various sizes:\n");

        /* Small tier stats */
        multiOrderedSet *mos = multiOrderedSetNewLimit(FLEX_CAP_LEVEL_2048);
        for (int i = 0; i < 10; i++) {
            databox score = databoxNewSigned(i);
            char buf[16];
            snprintf(buf, sizeof(buf), "s%d", i);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            multiOrderedSetAdd(&mos, &score, &member);
        }
        printf("      [S] count=%zu bytes=%zu type=Small\n",
               multiOrderedSetCount(mos), multiOrderedSetBytes(mos));
        assert(mosType_(mos) == MOS_TYPE_SMALL);
        multiOrderedSetFree(mos);

        /* Medium tier stats */
        mos = multiOrderedSetNewLimit(FLEX_CAP_LEVEL_64);
        for (int i = 0; i < 50; i++) {
            databox score = databoxNewSigned(i);
            char buf[16];
            snprintf(buf, sizeof(buf), "m%d", i);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            multiOrderedSetAdd(&mos, &score, &member);
        }
        printf("      [M] count=%zu bytes=%zu type=%s\n",
               multiOrderedSetCount(mos), multiOrderedSetBytes(mos),
               mosType_(mos) == MOS_TYPE_MEDIUM ? "Medium" : "Full");
        multiOrderedSetFree(mos);

        /* Full tier stats */
        mos = multiOrderedSetNewLimit(FLEX_CAP_LEVEL_64);
        for (int i = 0; i < 500; i++) {
            databox score = databoxNewSigned(i);
            char buf[16];
            snprintf(buf, sizeof(buf), "f%d", i);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            multiOrderedSetAdd(&mos, &score, &member);
        }
        printf("      [F] count=%zu bytes=%zu type=%s\n",
               multiOrderedSetCount(mos), multiOrderedSetBytes(mos),
               mosType_(mos) == MOS_TYPE_FULL ? "Full" : "NOT FULL!");
        assert(mosType_(mos) == MOS_TYPE_FULL);
        multiOrderedSetFree(mos);
    }

    /* ================================================================
     * Tier Promotion Regression Test
     * ================================================================ */

    TEST("REGRESSION: Small->Medium->Full promotion preserves entries") {
        multiOrderedSet *mos = multiOrderedSetNewLimit(FLEX_CAP_LEVEL_64);

        /* Add entries and track tier transitions - use pseudo-random scores */
        for (int i = 0; i < 30; i++) {
            int64_t scoreVal =
                (i * 997) % 10000; /* Pseudo-random distribution */
            databox score = databoxNewSigned(scoreVal);
            char buf[32];
            snprintf(buf, sizeof(buf), "m%d", i);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            multiOrderedSetAdd(&mos, &score, &member);

            multiOrderedSetType type = mosType_(mos);

            /* Check if first entry is still accessible after each insert */
            databox checkMem = databoxNewBytesAllowEmbed("m0", 2);
            databox gotScore;
            bool found = multiOrderedSetGetScore(mos, &checkMem, &gotScore);

            if (!found && i >= 1) {
                ERR("Entry m0 lost after inserting m%d! (type=%s)", i,
                    type == MOS_TYPE_SMALL    ? "Small"
                    : type == MOS_TYPE_MEDIUM ? "Medium"
                                              : "Full");
            }
            if (found && gotScore.data.i != 0) {
                ERR("Entry m0 has wrong score %" PRIdMAX
                    " after inserting m%d!",
                    gotScore.data.i, i);
            }
        }

        /* Verify we reached Full tier */
        assert(mosType_(mos) == MOS_TYPE_FULL);
        multiOrderedSetFree(mos);
    }

    /* ================================================================
     * FUZZ TESTS: Tier Promotion with Oracle Tracking
     * ================================================================ */

    TEST("FUZZ: Tier promotion preserves all entries (oracle tracking)") {
        multiOrderedSet *mos = multiOrderedSetNewLimit(FLEX_CAP_LEVEL_64);

        /* Oracle arrays to track what we've inserted */
        cleanerName(zfree, Int64) int64_t *oracleScores =
            zcalloc(1000, sizeof(int64_t));
        cleanerName(zfree, KeyBuf32) mosKeyBuf32 *oracleMembers =
            zcalloc(1000, 32);
        size_t oracleCount = 0;

        multiOrderedSetType prevType = MOS_TYPE_SMALL;
        int transitions = 0;

        for (int i = 0; i < 1000; i++) {
            /* Insert with random-ish score pattern */
            int64_t score = (i * 997) % 10000; /* Pseudo-random distribution */
            oracleScores[oracleCount] = score;
            snprintf(oracleMembers[oracleCount], 32, "member_%d", i);

            databox scoreBox = databoxNewSigned(score);
            databox memberBox = databoxNewBytesAllowEmbed(
                oracleMembers[oracleCount], strlen(oracleMembers[oracleCount]));
            multiOrderedSetAdd(&mos, &scoreBox, &memberBox);
            oracleCount++;

            /* Check for tier transition */
            multiOrderedSetType curType = mosType_(mos);
            if (curType != prevType) {
                transitions++;
                printf("      Transition %d: %s->%s at count=%zu\n",
                       transitions,
                       prevType == MOS_TYPE_SMALL    ? "Small"
                       : prevType == MOS_TYPE_MEDIUM ? "Medium"
                                                     : "Full",
                       curType == MOS_TYPE_SMALL    ? "Small"
                       : curType == MOS_TYPE_MEDIUM ? "Medium"
                                                    : "Full",
                       oracleCount);

                /* At transition, verify ALL entries are accessible */
                for (size_t j = 0; j < oracleCount; j++) {
                    databox checkMember = databoxNewBytesAllowEmbed(
                        oracleMembers[j], strlen(oracleMembers[j]));
                    databox gotScore;
                    if (!multiOrderedSetGetScore(mos, &checkMember,
                                                 &gotScore)) {
                        ERR("FUZZ FAIL: Entry '%s' lost at transition!",
                            oracleMembers[j]);
                    }
                    if (gotScore.data.i != oracleScores[j]) {
                        ERR("FUZZ FAIL: Score mismatch for '%s': expected "
                            "%" PRId64 " got %" PRIdMAX,
                            oracleMembers[j], oracleScores[j], gotScore.data.i);
                    }
                }

                prevType = curType;
            }
        }

        assert(transitions >=
               2); /* Should see Small->Medium and Medium->Full */
        printf("      Final: count=%zu transitions=%d type=%s: OK\n",
               multiOrderedSetCount(mos), transitions,
               mosType_(mos) == MOS_TYPE_FULL ? "Full" : "NOT_FULL");

        multiOrderedSetFree(mos);
    }

    TEST("FUZZ: Random access after tier promotion") {
        multiOrderedSet *mos = multiOrderedSetNewLimit(FLEX_CAP_LEVEL_64);

        cleanerName(zfree, Int64) int64_t *oracle =
            zcalloc(5000, sizeof(int64_t));
        size_t oracleCount = 0;

        /* Insert 5000 entries to ensure Full tier */
        for (int32_t i = 0; i < 5000; i++) {
            databox score = databoxNewSigned(i * 10);
            char buf[32];
            snprintf(buf, sizeof(buf), "key%d", i);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            multiOrderedSetAdd(&mos, &score, &member);
            oracle[oracleCount++] = i;
        }

        assert(mosType_(mos) == MOS_TYPE_FULL);

        /* Random access pattern using xorshift */
        uint64_t seed[2] = {12345, 67890};
        int errors = 0;
        for (int32_t round = 0; round < 10000; round++) {
            size_t idx = xoroshiro128plus(seed) % oracleCount;
            char buf[32];
            snprintf(buf, sizeof(buf), "key%" PRId64, oracle[idx]);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            databox gotScore;

            if (!multiOrderedSetGetScore(mos, &member, &gotScore)) {
                errors++;
                if (errors <= 5) {
                    ERR("FUZZ FAIL: Key '%s' not found!", buf);
                }
            }
        }

        printf(
            "      type=FULL count=%zu random_accesses=10000 errors=%d: %s\n",
            oracleCount, errors, errors == 0 ? "OK" : "FAIL");
        assert(errors == 0);

        multiOrderedSetFree(mos);
    }

    TEST("FUZZ: Delete and re-insert at tier boundaries") {
        multiOrderedSet *mos = multiOrderedSetNewLimit(FLEX_CAP_LEVEL_64);

        /* Build up to near-transition */
        for (int i = 0; i < 200; i++) {
            databox score = databoxNewSigned(i);
            char buf[32];
            snprintf(buf, sizeof(buf), "entry%d", i);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            multiOrderedSetAdd(&mos, &score, &member);
        }

        multiOrderedSetType initialType = mosType_(mos);
        size_t initialCount = multiOrderedSetCount(mos);

        /* Delete half and re-insert */
        for (int i = 0; i < 100; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "entry%d", i * 2); /* Delete evens */
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            multiOrderedSetRemove(&mos, &member);
        }

        assert(multiOrderedSetCount(mos) == initialCount - 100);

        /* Re-insert with different scores */
        for (int i = 0; i < 100; i++) {
            databox score = databoxNewSigned(i * 2 + 1000);
            char buf[32];
            snprintf(buf, sizeof(buf), "entry%d", i * 2);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            multiOrderedSetAdd(&mos, &score, &member);
        }

        assert(multiOrderedSetCount(mos) == initialCount);

        /* Verify sorted order */
        mosIterator iter;
        multiOrderedSetIteratorInit(mos, &iter, true);
        databox prevScore = databoxNewSigned(INT64_MIN);
        databox gotMember, gotScore;
        int count = 0;
        while (multiOrderedSetIteratorNext(&iter, &gotMember, &gotScore)) {
            if (databoxCompare(&gotScore, &prevScore) < 0) {
                ERR("FUZZ FAIL: Sort order broken after delete/re-insert!");
            }
            prevScore = gotScore;
            count++;
        }
        assert((size_t)count == initialCount);

        printf("      type=%s->%s count=%zu delete_reinsert=100: OK\n",
               initialType == MOS_TYPE_SMALL    ? "Small"
               : initialType == MOS_TYPE_MEDIUM ? "Medium"
                                                : "Full",
               mosType_(mos) == MOS_TYPE_SMALL    ? "Small"
               : mosType_(mos) == MOS_TYPE_MEDIUM ? "Medium"
                                                  : "Full",
               (size_t)count);

        multiOrderedSetFree(mos);
    }

    /* ================================================================
     * PRECISION TESTS: Large Integer Scores
     * ================================================================ */

    TEST("PRECISION: Large uint64_t scores (> 2^53)") {
        /* Test precision for values that can't be exactly represented in double
         */
        cleaner(multiOrderedSetFree) multiOrderedSet *mos =
            multiOrderedSetNew();

        /* Values near 2^53 where double loses precision */
        uint64_t testValues[] = {
            (1ULL << 53) - 1, /* Largest exactly representable */
            (1ULL << 53),     /* First imprecise */
            (1ULL << 53) + 1, /* Should differ from above */
            (1ULL << 53) + 2, /* Should differ from above */
            (1ULL << 60),     /* Much larger */
            (1ULL << 60) + 1, /* Differ by 1 */
            UINT64_MAX - 1,   /* Near max */
            UINT64_MAX        /* Max value */
        };
        size_t numValues = countof(testValues);

        for (size_t i = 0; i < numValues; i++) {
            databox score = databoxNewUnsigned(testValues[i]);
            char buf[32];
            snprintf(buf, sizeof(buf), "u%" PRIu64, testValues[i]);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            multiOrderedSetAdd(&mos, &score, &member);
        }

        assert(multiOrderedSetCount(mos) == numValues);

        /* Verify each value can be retrieved exactly */
        int precisionErrors = 0;
        for (size_t i = 0; i < numValues; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "u%" PRIu64, testValues[i]);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            databox gotScore;

            if (!multiOrderedSetGetScore(mos, &member, &gotScore)) {
                ERR("PRECISION FAIL: Value %" PRIu64 " not found!",
                    testValues[i]);
                precisionErrors++;
                continue;
            }

            if (gotScore.data.u != testValues[i]) {
                precisionErrors++;
                if (precisionErrors <= 3) {
                    printf("      WARNING: Score precision issue at %" PRIu64
                           " (got %" PRIuMAX ")\n",
                           testValues[i], gotScore.data.u);
                }
            }
        }

        /* Verify sorted order is correct by comparing adjacent pairs */
        int sortErrors = 0;
        mosIterator iter;
        multiOrderedSetIteratorInit(mos, &iter, true);
        databox prevScore = {};
        databox gotMember, gotScore;
        bool first = true;
        while (multiOrderedSetIteratorNext(&iter, &gotMember, &gotScore)) {
            if (!first) {
                /* Scores should be strictly increasing */
                int cmp = databoxCompare(&prevScore, &gotScore);
                if (cmp >= 0) {
                    sortErrors++;
                    if (sortErrors <= 3) {
                        printf("      SORT ERROR: %" PRIuMAX " >= %" PRIuMAX
                               "\n",
                               prevScore.data.u, gotScore.data.u);
                    }
                }
            }
            first = false;
            prevScore = gotScore;
        }

        printf("      Large uint64 precision_errors=%d sort_errors=%d: %s\n",
               precisionErrors, sortErrors,
               (precisionErrors == 0 && sortErrors == 0) ? "OK" : "ISSUES");
    }

    TEST("PRECISION: Large negative int64_t scores") {
        cleaner(multiOrderedSetFree) multiOrderedSet *mos =
            multiOrderedSetNew();

        int64_t testValues[] = {INT64_MIN,
                                INT64_MIN + 1,
                                INT64_MIN + 2,
                                -(1LL << 53),
                                -(1LL << 53) + 1,
                                -1000000000000LL,
                                -1,
                                0,
                                1,
                                1000000000000LL,
                                (1LL << 53) - 1,
                                INT64_MAX - 1,
                                INT64_MAX};
        size_t numValues = countof(testValues);

        for (size_t i = 0; i < numValues; i++) {
            databox score = databoxNewSigned(testValues[i]);
            char buf[32];
            snprintf(buf, sizeof(buf), "i%zu", i);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            multiOrderedSetAdd(&mos, &score, &member);
        }

        /* Verify sorted order */
        mosIterator iter;
        multiOrderedSetIteratorInit(mos, &iter, true);
        int64_t prevScore = INT64_MIN;
        databox gotMember, gotScore;
        bool first = true;
        int sortErrors = 0;

        while (multiOrderedSetIteratorNext(&iter, &gotMember, &gotScore)) {
            int64_t curScore = gotScore.data.i;
            if (!first && curScore < prevScore) {
                sortErrors++;
                if (sortErrors <= 3) {
                    printf("      SORT ERROR: %" PRId64 " < %" PRId64 "\n",
                           curScore, prevScore);
                }
            }
            first = false;
            prevScore = curScore;
        }

        printf("      Large int64 count=%zu sort_errors=%d: %s\n",
               multiOrderedSetCount(mos), sortErrors,
               sortErrors == 0 ? "OK" : "FAIL");
    }

    TEST("PRECISION: Mixed score types (int64, uint64, double)") {
        cleaner(multiOrderedSetFree) multiOrderedSet *mos =
            multiOrderedSetNew();

        /* Add with different numeric types */
        databox score, member;

        /* int64 values */
        DATABOX_SET_SIGNED(&score, -1000);
        member = databoxNewBytes("neg_int", 7);
        multiOrderedSetAdd(&mos, &score, &member);

        DATABOX_SET_SIGNED(&score, 0);
        member = databoxNewBytes("zero_int", 8);
        multiOrderedSetAdd(&mos, &score, &member);

        DATABOX_SET_SIGNED(&score, 1000);
        member = databoxNewBytes("pos_int", 7);
        multiOrderedSetAdd(&mos, &score, &member);

        /* uint64 values */
        DATABOX_SET_UNSIGNED(&score, 500);
        member = databoxNewBytes("mid_uint", 8);
        multiOrderedSetAdd(&mos, &score, &member);

        DATABOX_SET_UNSIGNED(&score, 2000);
        member = databoxNewBytes("big_uint", 8);
        multiOrderedSetAdd(&mos, &score, &member);

        /* double values */
        DATABOX_SET_DOUBLE(&score, -500.5);
        member = databoxNewBytes("neg_dbl", 7);
        multiOrderedSetAdd(&mos, &score, &member);

        DATABOX_SET_DOUBLE(&score, 500.5);
        member = databoxNewBytes("pos_dbl", 7);
        multiOrderedSetAdd(&mos, &score, &member);

        assert(multiOrderedSetCount(mos) == 7);

        /* Verify order is: -1000, -500.5, 0, 500, 500.5, 1000, 2000 */
        const char *expectedOrder[] = {"neg_int",  "neg_dbl", "zero_int",
                                       "mid_uint", "pos_dbl", "pos_int",
                                       "big_uint"};
        mosIterator iter;
        multiOrderedSetIteratorInit(mos, &iter, true);
        databox gotMember, gotScore;
        int idx = 0;
        int orderErrors = 0;

        while (multiOrderedSetIteratorNext(&iter, &gotMember, &gotScore)) {
            if (idx < 7) {
                /* Compare member names to expected order */
                const char *got = (const char *)databoxBytes(&gotMember);
                if (strncmp(got, expectedOrder[idx], gotMember.len) != 0) {
                    orderErrors++;
                    printf(
                        "      ORDER ERROR at %d: expected '%s' got '%.*s'\n",
                        idx, expectedOrder[idx], (int)gotMember.len, got);
                }
            }
            idx++;
        }

        printf("      Mixed types count=%d order_errors=%d: %s\n", idx,
               orderErrors, orderErrors == 0 ? "OK" : "FAIL");
    }

    /* ================================================================
     * PERFORMANCE BENCHMARKS
     * ================================================================ */

    printf("\n--- Performance Benchmarks ---\n");

    TEST("PERF: Insert throughput by tier") {
        const size_t SMALL_COUNT = 50;
        const size_t MEDIUM_COUNT = 200;
        const size_t FULL_COUNT = 5000;

        /* Small tier benchmark */
        {
            multiOrderedSet *mos = multiOrderedSetNewLimit(FLEX_CAP_LEVEL_2048);
            TIME_INIT;
            for (size_t i = 0; i < SMALL_COUNT; i++) {
                databox score = databoxNewSigned((int64_t)i);
                char buf[32];
                snprintf(buf, sizeof(buf), "s%zu", i);
                databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
                multiOrderedSetAdd(&mos, &score, &member);
            }
            TIME_FINISH(SMALL_COUNT, "Small inserts");
            multiOrderedSetFree(mos);
        }

        /* Medium tier benchmark */
        {
            multiOrderedSet *mos = multiOrderedSetNewLimit(FLEX_CAP_LEVEL_64);
            TIME_INIT;
            for (size_t i = 0; i < MEDIUM_COUNT; i++) {
                databox score = databoxNewSigned((int64_t)i);
                char buf[32];
                snprintf(buf, sizeof(buf), "m%zu", i);
                databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
                multiOrderedSetAdd(&mos, &score, &member);
            }
            TIME_FINISH(MEDIUM_COUNT, "Medium inserts");
            assert(mosType_(mos) == MOS_TYPE_MEDIUM ||
                   mosType_(mos) == MOS_TYPE_FULL);
            multiOrderedSetFree(mos);
        }

        /* Full tier benchmark */
        {
            multiOrderedSet *mos = multiOrderedSetNewLimit(FLEX_CAP_LEVEL_64);
            TIME_INIT;
            for (size_t i = 0; i < FULL_COUNT; i++) {
                databox score = databoxNewSigned((int64_t)i);
                char buf[32];
                snprintf(buf, sizeof(buf), "f%zu", i);
                databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
                multiOrderedSetAdd(&mos, &score, &member);
            }
            TIME_FINISH(FULL_COUNT, "Full inserts");
            assert(mosType_(mos) == MOS_TYPE_FULL);
            multiOrderedSetFree(mos);
        }
    }

    TEST("PERF: Lookup throughput by tier") {
        const size_t LOOKUP_COUNT = 10000;

        /* Build Full tier set */
        multiOrderedSet *mos = multiOrderedSetNewLimit(FLEX_CAP_LEVEL_64);
        for (size_t i = 0; i < 5000; i++) {
            databox score = databoxNewSigned((int64_t)i);
            char buf[32];
            snprintf(buf, sizeof(buf), "key%zu", i);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            multiOrderedSetAdd(&mos, &score, &member);
        }

        /* Score lookup benchmark */
        {
            TIME_INIT;
            uint64_t seed[2] = {11111, 22222};
            for (size_t i = 0; i < LOOKUP_COUNT; i++) {
                size_t idx = xoroshiro128plus(seed) % 5000;
                char buf[32];
                snprintf(buf, sizeof(buf), "key%zu", idx);
                databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
                databox gotScore;
                multiOrderedSetGetScore(mos, &member, &gotScore);
            }
            TIME_FINISH(LOOKUP_COUNT, "GetScore lookups");
        }

        /* Rank lookup benchmark */
        {
            TIME_INIT;
            uint64_t seed[2] = {33333, 44444};
            for (size_t i = 0; i < LOOKUP_COUNT; i++) {
                size_t idx = xoroshiro128plus(seed) % 5000;
                char buf[32];
                snprintf(buf, sizeof(buf), "key%zu", idx);
                databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
                multiOrderedSetGetRank(mos, &member);
            }
            TIME_FINISH(LOOKUP_COUNT, "GetRank lookups");
        }

        /* Exists check benchmark */
        {
            TIME_INIT;
            uint64_t seed[2] = {55555, 66666};
            for (size_t i = 0; i < LOOKUP_COUNT; i++) {
                size_t idx = xoroshiro128plus(seed) % 5000;
                char buf[32];
                snprintf(buf, sizeof(buf), "key%zu", idx);
                databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
                multiOrderedSetExists(mos, &member);
            }
            TIME_FINISH(LOOKUP_COUNT, "Exists checks");
        }

        multiOrderedSetFree(mos);
    }

    TEST("PERF: Iteration throughput") {
        multiOrderedSet *mos = multiOrderedSetNewLimit(FLEX_CAP_LEVEL_64);

        /* Build set with 5000 entries */
        for (size_t i = 0; i < 5000; i++) {
            databox score = databoxNewSigned((int64_t)i);
            char buf[32];
            snprintf(buf, sizeof(buf), "iter%zu", i);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            multiOrderedSetAdd(&mos, &score, &member);
        }

        /* Forward iteration benchmark */
        {
            TIME_INIT;
            for (int round = 0; round < 10; round++) {
                mosIterator iter;
                multiOrderedSetIteratorInit(mos, &iter, true);
                databox gotMember, gotScore;
                while (
                    multiOrderedSetIteratorNext(&iter, &gotMember, &gotScore)) {
                    /* Just iterate */
                }
            }
            TIME_FINISH(50000, "Forward iteration (10 full passes)");
        }

        multiOrderedSetFree(mos);
    }

    /* ================================================================
     * STRESS TESTS: Large Datasets
     * ================================================================ */

    TEST("STRESS: 10K random entries") {
        multiOrderedSet *mos = multiOrderedSetNewLimit(FLEX_CAP_LEVEL_64);
        uint64_t seed[2] = {77777, 88888};

        TIME_INIT;
        for (size_t i = 0; i < 10000; i++) {
            int64_t score = (int64_t)(xoroshiro128plus(seed) % 1'000'000);
            databox scoreBox = databoxNewSigned(score);
            char buf[32];
            snprintf(buf, sizeof(buf), "stress%zu", i);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            multiOrderedSetAdd(&mos, &scoreBox, &member);
        }
        TIME_FINISH(10000, "10K random inserts");

        printf("      10K entries: count=%zu bytes=%zu type=%s\n",
               multiOrderedSetCount(mos), multiOrderedSetBytes(mos),
               mosType_(mos) == MOS_TYPE_FULL ? "Full" : "OTHER");

        /* Verify all entries present */
        for (size_t i = 0; i < 10000; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "stress%zu", i);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            if (!multiOrderedSetExists(mos, &member)) {
                ERR("STRESS FAIL: Entry %zu missing!", i);
            }
        }

        multiOrderedSetFree(mos);
    }

    TEST("STRESS: Duplicate score handling") {
        /* Many entries with same score, different members */
        cleaner(multiOrderedSetFree) multiOrderedSet *mos =
            multiOrderedSetNew();

        databox score = databoxNewSigned(42);
        for (int i = 0; i < 100; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "same_score_%d", i);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            multiOrderedSetAdd(&mos, &score, &member);
        }

        assert(multiOrderedSetCount(mos) == 100);

        /* All should have same score */
        for (int i = 0; i < 100; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "same_score_%d", i);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            databox gotScore;
            bool found = multiOrderedSetGetScore(mos, &member, &gotScore);
            assert(found);
            assert(gotScore.data.i == 42);
        }

        printf("      100 same-score entries: OK\n");
    }

    TEST("STRESS: Score update (replace) performance") {
        multiOrderedSet *mos = multiOrderedSetNewLimit(FLEX_CAP_LEVEL_64);

        /* Initial insert */
        for (int i = 0; i < 1000; i++) {
            databox score = databoxNewSigned(i);
            char buf[32];
            snprintf(buf, sizeof(buf), "upd%d", i);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            multiOrderedSetAdd(&mos, &score, &member);
        }

        /* Update all scores (should be faster than delete+insert) */
        TIME_INIT;
        for (int round = 0; round < 10; round++) {
            for (int i = 0; i < 1000; i++) {
                databox score = databoxNewSigned(i + round * 1000);
                char buf[32];
                snprintf(buf, sizeof(buf), "upd%d", i);
                databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
                multiOrderedSetAdd(&mos, &score, &member);
            }
        }
        TIME_FINISH(10000, "Score updates");

        assert(multiOrderedSetCount(mos) == 1000);
        multiOrderedSetFree(mos);
    }

    /* ================================================================
     * DIRECT TIER IMPLEMENTATION TESTS
     * ================================================================ */

    printf("\n--- Direct Tier Implementation Tests ---\n");

    TEST("DIRECT: multiOrderedSetSmall basic ops") {
        multiOrderedSetSmall *small = multiOrderedSetSmallNew();

        /* Insert 20 entries */
        for (int i = 0; i < 20; i++) {
            databox score = databoxNewSigned(i * 5);
            char buf[16];
            snprintf(buf, sizeof(buf), "small%d", i);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            multiOrderedSetSmallAdd(small, &score, &member);
        }
        assert(multiOrderedSetSmallCount(small) == 20);

        /* Lookup */
        for (int i = 0; i < 20; i++) {
            char buf[16];
            snprintf(buf, sizeof(buf), "small%d", i);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            databox gotScore;
            bool found =
                multiOrderedSetSmallGetScore(small, &member, &gotScore);
            assert(found);
            assert(gotScore.data.i == i * 5);
        }

        /* Delete half */
        for (int i = 0; i < 10; i++) {
            char buf[16];
            snprintf(buf, sizeof(buf), "small%d", i * 2);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            bool removed = multiOrderedSetSmallRemove(small, &member);
            assert(removed);
        }
        assert(multiOrderedSetSmallCount(small) == 10);

        printf("      Small tier: insert=20 delete=10 final=%zu: OK\n",
               multiOrderedSetSmallCount(small));
        multiOrderedSetSmallFree(small);
    }

    TEST("DIRECT: multiOrderedSetMedium basic ops") {
        multiOrderedSetMedium *medium = multiOrderedSetMediumNew();

        /* Insert 50 entries */
        for (int i = 0; i < 50; i++) {
            databox score = databoxNewSigned(i * 3);
            char buf[16];
            snprintf(buf, sizeof(buf), "med%d", i);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            multiOrderedSetMediumAdd(medium, &score, &member);
        }
        assert(multiOrderedSetMediumCount(medium) == 50);

        /* Verify all present */
        for (int i = 0; i < 50; i++) {
            char buf[16];
            snprintf(buf, sizeof(buf), "med%d", i);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            if (!multiOrderedSetMediumExists(medium, &member)) {
                ERR("DIRECT FAIL: Medium tier missing entry %d", i);
            }
        }

        printf("      Medium tier: insert=50 count=%zu: OK\n",
               multiOrderedSetMediumCount(medium));
        multiOrderedSetMediumFree(medium);
    }

    TEST("DIRECT: multiOrderedSetFull basic ops") {
        multiOrderedSetFull *full = multiOrderedSetFullNew();

        /* Insert 200 entries */
        for (int i = 0; i < 200; i++) {
            databox score = databoxNewSigned(i * 7);
            char buf[16];
            snprintf(buf, sizeof(buf), "full%d", i);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            multiOrderedSetFullAdd(full, &score, &member);
        }
        assert(multiOrderedSetFullCount(full) == 200);

        /* Rank queries */
        for (int i = 0; i < 200; i++) {
            char buf[16];
            snprintf(buf, sizeof(buf), "full%d", i);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            int64_t rank = multiOrderedSetFullGetRank(full, &member);
            if (rank != i) {
                ERR("DIRECT FAIL: Full tier rank mismatch for entry %d: got "
                    "%" PRId64,
                    i, rank);
            }
        }

        printf("      Full tier: insert=200 count=%zu rank_verified=200: OK\n",
               multiOrderedSetFullCount(full));
        multiOrderedSetFullFree(full);
    }

    /* ================================================================
     * COMPREHENSIVE PERFORMANCE TESTS (multimap-style)
     * ================================================================ */

    printf("\n=== Comprehensive Performance Tests ===\n\n");

    /* Helper macro to print statistics like multimap does */
#define MOS_PRINT_STATS(mos)                                                   \
    do {                                                                       \
        const char *typeStr = mosType_(mos) == MOS_TYPE_SMALL    ? "S"         \
                              : mosType_(mos) == MOS_TYPE_MEDIUM ? "M"         \
                                                                 : "F";        \
        printf("[%s] {bytes {total %zu}} {count %zu} {type %s}\n", typeStr,    \
               multiOrderedSetBytes(mos), multiOrderedSetCount(mos),           \
               mosType_(mos) == MOS_TYPE_SMALL    ? "Small"                    \
               : mosType_(mos) == MOS_TYPE_MEDIUM ? "Medium"                   \
                                                  : "Full");                   \
    } while (0)

    /* Test at various sizes like multimap does */
    static constexpr size_t testCounts[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    static constexpr size_t numTestCounts = countof(testCounts);

    TEST("(full width) insert / exists / lookup / delete / rank at various "
         "sizes") {
        for (size_t tc = 0; tc < numTestCounts; tc++) {
            size_t count = testCounts[tc];

            printf("test — %zu entries - inserting...\n", count);
            cleaner(multiOrderedSetFree) multiOrderedSet *mos =
                multiOrderedSetNew();

            /* Pre-allocate key buffers to avoid timing snprintf */
            cleanerName(zfree, KeyBuf32) mosKeyBuf32 *keys = nullptr;
            if (count > 0) {
                keys = zcalloc(count, 32);
                for (size_t i = 0; i < count; i++) {
                    snprintf(keys[i], 32, "key%zu", i);
                }
            }

            {
                TIME_INIT;
                for (size_t i = 0; i < count; i++) {
                    databox score = databoxNewSigned((int64_t)i);
                    databox member =
                        databoxNewBytesAllowEmbed(keys[i], strlen(keys[i]));
                    multiOrderedSetAdd(&mos, &score, &member);
                }
                TIME_FINISH(count, "insert");
            }
            MOS_PRINT_STATS(mos);

            printf("test — %zu entries - checking members (sequential)...\n",
                   count);
            {
                TIME_INIT;
                for (size_t i = 0; i < count; i++) {
                    databox member =
                        databoxNewBytesAllowEmbed(keys[i], strlen(keys[i]));
                    if (!multiOrderedSetExists(mos, &member)) {
                        ERR("FAIL: entry %zu not found", i);
                    }
                }
                TIME_FINISH(count, "exists (sequential)");
            }

            if (count > 0) {
                printf("test — %zu entries - GetScore (sequential)...\n",
                       count);
                {
                    TIME_INIT;
                    for (size_t i = 0; i < count; i++) {
                        databox member =
                            databoxNewBytesAllowEmbed(keys[i], strlen(keys[i]));
                        databox gotScore;
                        multiOrderedSetGetScore(mos, &member, &gotScore);
                    }
                    TIME_FINISH(count, "GetScore (sequential)");
                }

                printf("test — %zu entries - GetRank (sequential)...\n", count);
                {
                    TIME_INIT;
                    for (size_t i = 0; i < count; i++) {
                        databox member =
                            databoxNewBytesAllowEmbed(keys[i], strlen(keys[i]));
                        multiOrderedSetGetRank(mos, &member);
                    }
                    TIME_FINISH(count, "GetRank (sequential)");
                }
            }

            printf("test — %zu entries - deleting...\n", count);
            {
                size_t deleteErrors = 0;
                TIME_INIT;
                for (size_t i = 0; i < count; i++) {
                    databox member =
                        databoxNewBytesAllowEmbed(keys[i], strlen(keys[i]));
                    if (!multiOrderedSetRemove(&mos, &member)) {
                        deleteErrors++;
                        if (deleteErrors <= 10) {
                            printf(
                                "      DELETE FAIL: entry %zu '%s' not found\n",
                                i, keys[i]);
                        }
                    }
                }
                TIME_FINISH(count, "delete");
                if (deleteErrors > 0) {
                    printf("      DELETE ERRORS: %zu entries not found!\n",
                           deleteErrors);
                }
            }
            MOS_PRINT_STATS(mos);
            if (multiOrderedSetCount(mos) != 0) {
                /* List remaining entries */
                printf("      REMAINING ENTRIES:\n");
                mosIterator iter;
                multiOrderedSetIteratorInit(mos, &iter, true);
                databox gotMember, gotScore;
                int shown = 0;
                while (
                    multiOrderedSetIteratorNext(&iter, &gotMember, &gotScore) &&
                    shown < 10) {
                    printf("        score=%" PRIdMAX " member='%.*s'\n",
                           gotScore.data.i, (int)gotMember.len,
                           (char *)databoxBytes(&gotMember));
                    shown++;
                }
                ERR("After deleting %zu entries, count=%zu (expected 0)!",
                    count, multiOrderedSetCount(mos));
            }

            printf(
                "test — %zu entries - inserting again after full delete...\n",
                count);
            {
                TIME_INIT;
                for (size_t i = 0; i < count; i++) {
                    databox score = databoxNewSigned((int64_t)(count - i));
                    databox member =
                        databoxNewBytesAllowEmbed(keys[i], strlen(keys[i]));
                    multiOrderedSetAdd(&mos, &score, &member);
                }
                TIME_FINISH(count, "insert (reverse order)");
            }
            MOS_PRINT_STATS(mos);

            printf("\n");
        }
    }

    /* ================================================================
     * COMPARISON: multiOrderedSet vs raw multimap
     * ================================================================ */

    printf("\n=== multiOrderedSet vs multimap Comparison ===\n\n");

    TEST("COMPARE: multiOrderedSet vs multimap insert/iterate") {
        /* Note: multimap and multiOrderedSet have different key semantics:
         * - multiOrderedSet: lookup by member, sorted by score
         * - multimap: lookup by first element (key), sorted by key
         * We can fairly compare: insert performance and iteration performance
         */

        const size_t COMPARE_COUNTS[] = {100, 1000, 10000};
        const size_t NUM_COMPARE = countof(COMPARE_COUNTS);

        for (size_t cc = 0; cc < NUM_COMPARE; cc++) {
            size_t count = COMPARE_COUNTS[cc];
            printf("--- %zu entries ---\n", count);

            /* Pre-allocate keys */
            cleanerName(zfree, KeyBuf32) mosKeyBuf32 *keys = zcalloc(count, 32);
            for (size_t i = 0; i < count; i++) {
                snprintf(keys[i], 32, "cmp%zu", i);
            }

            /* ---- multiOrderedSet ---- */
            cleaner(multiOrderedSetFree) multiOrderedSet *mos =
                multiOrderedSetNew();

            printf("  multiOrderedSet:\n");
            {
                TIME_INIT;
                for (size_t i = 0; i < count; i++) {
                    databox score = databoxNewSigned((int64_t)i);
                    databox member =
                        databoxNewBytesAllowEmbed(keys[i], strlen(keys[i]));
                    multiOrderedSetAdd(&mos, &score, &member);
                }
                TIME_FINISH(count, "MOS insert");
            }
            {
                TIME_INIT;
                for (size_t i = 0; i < count; i++) {
                    databox member =
                        databoxNewBytesAllowEmbed(keys[i], strlen(keys[i]));
                    multiOrderedSetExists(mos, &member);
                }
                TIME_FINISH(count, "MOS exists (by member)");
            }
            {
                TIME_INIT;
                for (size_t i = 0; i < count; i++) {
                    databox member =
                        databoxNewBytesAllowEmbed(keys[i], strlen(keys[i]));
                    databox gotScore;
                    multiOrderedSetGetScore(mos, &member, &gotScore);
                }
                TIME_FINISH(count, "MOS GetScore (by member)");
            }
            printf("    bytes=%zu type=%s\n", multiOrderedSetBytes(mos),
                   mosType_(mos) == MOS_TYPE_SMALL    ? "Small"
                   : mosType_(mos) == MOS_TYPE_MEDIUM ? "Medium"
                                                      : "Full");

            /* ---- multimap (2-element: score, member) sorted by score ---- */
            cleaner(multimapFree) multimap *mm = multimapNew(2);

            printf("  multimap (2-element, key=score):\n");
            {
                TIME_INIT;
                for (size_t i = 0; i < count; i++) {
                    databox score = databoxNewSigned((int64_t)i);
                    databox member =
                        databoxNewBytesAllowEmbed(keys[i], strlen(keys[i]));
                    const databox *elements[2] = {&score, &member};
                    multimapInsert(&mm, elements);
                }
                TIME_FINISH(count, "MM insert");
            }
            {
                /* multimap lookup is by score (first element), not by member */
                TIME_INIT;
                for (size_t i = 0; i < count; i++) {
                    databox score = databoxNewSigned((int64_t)i);
                    multimapExists(mm, &score);
                }
                TIME_FINISH(count, "MM exists (by score/key)");
            }
            {
                TIME_INIT;
                for (size_t i = 0; i < count; i++) {
                    databox score = databoxNewSigned((int64_t)i);
                    databox foundElements[2];
                    databox *elemPtrs[2] = {&foundElements[0],
                                            &foundElements[1]};
                    multimapLookup(mm, &score, elemPtrs);
                }
                TIME_FINISH(count, "MM lookup (by score/key)");
            }
            printf("    bytes=%zu count=%zu\n", multimapBytes(mm),
                   multimapCount(mm));
            printf("\n");
        }
    }

    TEST("COMPARE: Random access patterns") {
        const size_t count = 10000;
        const size_t accessCount = 50000;

        /* Pre-allocate keys and scores for random access */
        cleanerName(zfree, KeyBuf32) mosKeyBuf32 *keys = zcalloc(count, 32);
        cleanerName(zfree, Int64) int64_t *scores =
            zcalloc(count, sizeof(int64_t));
        for (size_t i = 0; i < count; i++) {
            snprintf(keys[i], 32, "rnd%zu", i);
            scores[i] = (int64_t)(i * 17 % count); /* Pseudo-random scores */
        }

        /* Build both structures */
        cleaner(multiOrderedSetFree) multiOrderedSet *mos =
            multiOrderedSetNew();
        cleaner(multimapFree) multimap *mm = multimapNew(2);

        for (size_t i = 0; i < count; i++) {
            databox score = databoxNewSigned(scores[i]);
            databox member =
                databoxNewBytesAllowEmbed(keys[i], strlen(keys[i]));
            multiOrderedSetAdd(&mos, &score, &member);
            const databox *elements[2] = {&score, &member};
            multimapInsert(&mm, elements);
        }

        printf("Random access benchmark (%zu accesses on %zu entries):\n",
               accessCount, count);

        /* Random access - multiOrderedSet (lookup by member) */
        {
            uint64_t seed[2] = {98765, 43210};
            TIME_INIT;
            for (size_t i = 0; i < accessCount; i++) {
                size_t idx = xoroshiro128plus(seed) % count;
                databox member =
                    databoxNewBytesAllowEmbed(keys[idx], strlen(keys[idx]));
                databox gotScore;
                multiOrderedSetGetScore(mos, &member, &gotScore);
            }
            TIME_FINISH(accessCount, "MOS random GetScore (by member)");
        }

        /* Random access - multimap (lookup by score/key) */
        {
            uint64_t seed[2] = {98765,
                                43210}; /* Same seed for fair comparison */
            TIME_INIT;
            for (size_t i = 0; i < accessCount; i++) {
                size_t idx = xoroshiro128plus(seed) % count;
                databox score = databoxNewSigned(scores[idx]);
                databox foundElements[2];
                databox *elemPtrs[2] = {&foundElements[0], &foundElements[1]};
                multimapLookup(mm, &score, elemPtrs);
            }
            TIME_FINISH(accessCount, "MM random lookup (by score/key)");
        }

        printf("  MOS bytes=%zu type=%s\n", multiOrderedSetBytes(mos),
               mosType_(mos) == MOS_TYPE_FULL ? "Full" : "Other");
        printf("  MM  bytes=%zu count=%zu\n\n", multimapBytes(mm),
               multimapCount(mm));
    }

    TEST("COMPARE: Iteration throughput") {
        const size_t count = 10000;

        /* Pre-allocate keys */
        cleanerName(zfree, KeyBuf32) mosKeyBuf32 *keys = zcalloc(count, 32);
        for (size_t i = 0; i < count; i++) {
            snprintf(keys[i], 32, "itr%zu", i);
        }

        /* Build both structures */
        cleaner(multiOrderedSetFree) multiOrderedSet *mos =
            multiOrderedSetNew();
        cleaner(multimapFree) multimap *mm = multimapNew(2);

        for (size_t i = 0; i < count; i++) {
            databox score = databoxNewSigned((int64_t)i);
            databox member =
                databoxNewBytesAllowEmbed(keys[i], strlen(keys[i]));
            multiOrderedSetAdd(&mos, &score, &member);
            const databox *elements[2] = {&score, &member};
            multimapInsert(&mm, elements);
        }

        printf("Iteration benchmark (10 full passes over %zu entries):\n",
               count);

        /* multiOrderedSet iteration */
        {
            TIME_INIT;
            for (int round = 0; round < 10; round++) {
                mosIterator iter;
                multiOrderedSetIteratorInit(mos, &iter, true);
                databox gotMember, gotScore;
                while (
                    multiOrderedSetIteratorNext(&iter, &gotMember, &gotScore)) {
                    /* Just iterate */
                }
            }
            TIME_FINISH(count * 10, "MOS iteration");
        }

        /* multimap iteration */
        {
            TIME_INIT;
            for (int round = 0; round < 10; round++) {
                multimapIterator iter;
                multimapIteratorInit(mm, &iter, true);
                databox elements[2];
                databox *elemPtrs[2] = {&elements[0], &elements[1]};
                while (multimapIteratorNext(&iter, elemPtrs)) {
                    /* Just iterate */
                }
            }
            TIME_FINISH(count * 10, "MM iteration");
        }

        printf("  MOS bytes=%zu\n", multiOrderedSetBytes(mos));
        printf("  MM  bytes=%zu\n\n", multimapBytes(mm));
    }

    printf("\n=== All Extended multiOrderedSet Tests Completed ===\n\n");

    TEST("reset then reuse: dispatcher across growth, two cycles") {
        cleaner(multiOrderedSetFree) multiOrderedSet *mos =
            multiOrderedSetNew();
        char member[32];

        for (int cycle = 0; cycle < 2; cycle++) {
            for (uint64_t i = 0; i < 400; i++) {
                snprintf(member, sizeof(member), "rr-member-%" PRIu64, i);
                const databox score = databoxNewUnsigned(i);
                const databox mem = databoxNewBytesString(member);
                multiOrderedSetAdd(&mos, &score, &mem);
            }
            if (multiOrderedSetCount(mos) != 400) {
                ERR("cycle %d: expected 400, got %zu", cycle,
                    multiOrderedSetCount(mos));
            }

            multiOrderedSetReset(&mos);
            if (multiOrderedSetCount(mos) != 0) {
                ERR("cycle %d: expected 0 after reset, got %zu", cycle,
                    multiOrderedSetCount(mos));
            }
        }

        size_t missing = 0;
        for (uint64_t i = 0; i < 400; i++) {
            snprintf(member, sizeof(member), "rr-member-%" PRIu64, i);
            const databox score = databoxNewUnsigned(i + 5);
            const databox mem = databoxNewBytesString(member);
            multiOrderedSetAdd(&mos, &score, &mem);
        }
        for (uint64_t i = 0; i < 400; i++) {
            snprintf(member, sizeof(member), "rr-member-%" PRIu64, i);
            const databox mem = databoxNewBytesString(member);
            if (!multiOrderedSetExists(mos, &mem)) {
                missing++;
            }
        }
        if (missing > 0) {
            ERR("%zu of 400 members missing after reset-reuse", missing);
        }
    }

    TEST("reset then reuse: Full tier with TREE atom pool") {
        /* End-to-end pin for the atomPool treeReset fix: the TREE
         * backend used to no-op on reset, leaving atoms interned */
        multiOrderedSetFull *m =
            multiOrderedSetFullNewWithPoolType(ATOM_POOL_TREE);
        char member[32];

        for (int cycle = 0; cycle < 2; cycle++) {
            for (uint64_t i = 0; i < 100; i++) {
                snprintf(member, sizeof(member), "tree-rr-%" PRIu64, i);
                const databox score = databoxNewUnsigned(i);
                const databox mem = databoxNewBytesString(member);
                multiOrderedSetFullAdd(m, &score, &mem);
            }
            if (multiOrderedSetFullCount(m) != 100) {
                ERR("cycle %d: expected 100, got %zu", cycle,
                    multiOrderedSetFullCount(m));
            }

            multiOrderedSetFullReset(m);
            if (multiOrderedSetFullCount(m) != 0) {
                ERR("cycle %d: expected 0 after reset, got %zu", cycle,
                    multiOrderedSetFullCount(m));
            }
        }

        const databox score = databoxNewUnsigned(1);
        const databox mem = databoxNewBytesString("tree-post-reset");
        multiOrderedSetFullAdd(m, &score, &mem);
        if (multiOrderedSetFullCount(m) != 1 ||
            !multiOrderedSetFullExists(m, &mem)) {
            ERR("TREE-pool set unusable after reset");
        }

        multiOrderedSetFullFree(m);
    }

    TEST("NX/XX/IncrBy, rank iterators and range removals agree with an "
         "oracle in every tier") {
        static constexpr size_t sizes[3] = {12, 400, 900};
        static const multiOrderedSetType tiers[3] = {
            MOS_TYPE_SMALL, MOS_TYPE_MEDIUM, MOS_TYPE_FULL};
        for (size_t t = 0; t < 3; t++) {
            for (int compress = 0; compress <= 1; compress++) {
                cleaner(multiOrderedSetFree) multiOrderedSet *s =
                    compress ? multiOrderedSetNewCompress(FLEX_CAP_LEVEL_2048)
                             : multiOrderedSetNew();
                const size_t n = sizes[t];
                /* The oracle: score per member id; -1 marks removed */
                int64_t oracle[900];
                char name[16];
                for (size_t i = 0; i < n; i++) {
                    oracle[i] = (int64_t)((i * 7919) % 1000);
                    snprintf(name, sizeof(name), "m%04zu", i);
                    const databox member = databoxNewBytesString(name);
                    const databox score = DATABOX_SIGNED(oracle[i]);
                    if (!multiOrderedSetAddNX(&s, &score, &member)) {
                        ERR("tier %zu: AddNX refused new member %zu", t, i);
                    }
                }
                if (!compress && mosType_(s) != tiers[t]) {
                    ERR("tier %zu built type %d", t, (int)mosType_(s));
                }

                /* NX refuses a present member; XX refuses an absent one */
                const databox m0 = databoxNewBytesString("m0000");
                const databox absent = databoxNewBytesString("zzzz");
                const databox high = DATABOX_SIGNED(5000);
                if (multiOrderedSetAddNX(&s, &high, &m0) ||
                    multiOrderedSetAddXX(&s, &high, &absent) ||
                    !multiOrderedSetAddXX(&s, &high, &m0)) {
                    ERR("tier %zu: NX/XX accepted the wrong member", t);
                }
                oracle[0] = 5000;
                const databox five = DATABOX_SIGNED(5);
                databox incr = {};
                if (!multiOrderedSetIncrBy(&s, &five, &m0, &incr) ||
                    incr.data.i64 != 5005) {
                    ERR("tier %zu: IncrBy gave %" PRId64, t, incr.data.i64);
                }
                oracle[0] = 5005;

                /* Rank order of the oracle: by score, then member name */
                size_t order[900];
                size_t live = 0;
#define MOS_ORACLE_ORDER_()                                                    \
    do {                                                                       \
        live = 0;                                                              \
        for (size_t i = 0; i < n; i++) {                                       \
            if (oracle[i] >= 0) {                                              \
                size_t at = live++;                                            \
                while (at > 0 && (oracle[order[at - 1]] > oracle[i] ||         \
                                  (oracle[order[at - 1]] == oracle[i] &&       \
                                   order[at - 1] > i))) {                      \
                    order[at] = order[at - 1];                                 \
                    at--;                                                      \
                }                                                              \
                order[at] = i;                                                 \
            }                                                                  \
        }                                                                      \
    } while (0)
                MOS_ORACLE_ORDER_();

                /* An iterator at a rank yields that rank's member, forward
                 * and backward */
                const int64_t ranks[3] = {0, (int64_t)live / 2,
                                          (int64_t)live - 1};
                for (size_t r = 0; r < 3; r++) {
                    for (int forward = 0; forward <= 1; forward++) {
                        mosIterator it;
                        databox member;
                        databox score;
                        if (!multiOrderedSetIteratorInitAtRank(s, &it, ranks[r],
                                                               forward) ||
                            !multiOrderedSetIteratorNext(&it, &member,
                                                         &score)) {
                            ERR("tier %zu: no iterator at rank %" PRId64, t,
                                ranks[r]);
                            continue;
                        }
                        snprintf(name, sizeof(name), "m%04zu", order[ranks[r]]);
                        if (member.len != strlen(name) ||
                            memcmp(databoxBytes(&member), name, member.len)) {
                            ERR("tier %zu: rank %" PRId64 " (%s) gave the "
                                "wrong member",
                                t, ranks[r], forward ? "fwd" : "rev");
                        }
                        multiOrderedSetIteratorRelease(&it);
                    }
                }

                /* Removing ranks 1..3, then scores in [100, 300] */
                const size_t byRank =
                    multiOrderedSetRemoveRangeByRank(&s, 1, 3);
                for (size_t k = 1; k <= 3 && k < live; k++) {
                    oracle[order[k]] = -1;
                }
                const mosRangeSpec range = {.min = DATABOX_SIGNED(100),
                                            .max = DATABOX_SIGNED(300)};
                const size_t byScore =
                    multiOrderedSetRemoveRangeByScore(&s, &range);
                size_t wantByScore = 0;
                for (size_t i = 0; i < n; i++) {
                    if (oracle[i] >= 100 && oracle[i] <= 300) {
                        oracle[i] = -1;
                        wantByScore++;
                    }
                }
                MOS_ORACLE_ORDER_();
                if (byRank != (live + 3 + wantByScore > 3 ? 3 : 0) ||
                    byScore != wantByScore || multiOrderedSetCount(s) != live) {
                    ERR("tier %zu: removed %zu by rank, %zu by score (want "
                        "%zu); %zu left, want %zu",
                        t, byRank, byScore, wantByScore,
                        multiOrderedSetCount(s), live);
                }

                /* The whole set, in order */
                mosIterator it;
                multiOrderedSetIteratorInit(s, &it, true);
                databox member;
                databox score;
                for (size_t k = 0; k < live; k++) {
                    snprintf(name, sizeof(name), "m%04zu", order[k]);
                    if (!multiOrderedSetIteratorNext(&it, &member, &score) ||
                        member.len != strlen(name) ||
                        memcmp(databoxBytes(&member), name, member.len) ||
                        score.data.i64 != oracle[order[k]]) {
                        ERR("tier %zu compress %d: rank %zu differs", t,
                            compress, k);
                        break;
                    }
                }
                multiOrderedSetIteratorRelease(&it);
#undef MOS_ORACLE_ORDER_
            }
        }
    }

    TEST("multiOrderedSet: 96/128-bit scores and members in every tier") {
        const mosWideMode modes[3] = {MOS_WIDE_NORMAL, MOS_WIDE_EXTENDED,
                                      MOS_WIDE_MIXED};
        for (size_t mode = 0; mode < 3; mode++) {
            mosWideMode_ = modes[mode];
            err += mosWideScenario_(12, MOS_TYPE_SMALL);
            err += mosWideScenario_(120, MOS_TYPE_MEDIUM);
            err += mosWideScenario_(900, MOS_TYPE_FULL);
        }
        mosWideMode_ = MOS_WIDE_MIXED;
    }

    for (int mode = 0; mode < 3; mode++) {
        TEST_DESC("multiOrderedSet: set operations keep members (mode %d)",
                  mode) {
            /* A holds members 0..39, B holds 20..59: |A u B| = 60,
             * |A n B| = 20, |A - B| = 20.  Mode 0 uses only 64-bit values,
             * mode 1 only 96/128-bit values, mode 2 alternates them. */
            cleaner(multiOrderedSetFree) multiOrderedSet *a =
                multiOrderedSetNew();
            cleaner(multiOrderedSetFree) multiOrderedSet *b =
                multiOrderedSetNew();
            mosWideNum members[60];
            mosWideNum scores[60];
            for (size_t i = 0; i < 60; i++) {
                const bool wide = mode == 1 || (mode == 2 && (i & 1));
                members[i] = wide ? mosWideValue_(i < 9 ? i : 100 + i)
                                  : mosWideS_((int64_t)i * 7919 - 100'000);
                scores[i] = wide ? mosWideValue_(200 + i)
                                 : mosWideS_((int64_t)i * 31 - 500);
                databoxBig member;
                databoxBig score;
                mosWideNumBox_(&member, members[i]);
                mosWideNumBox_(&score, scores[i]);
                if (i < 40) {
                    multiOrderedSetAdd(&a, (databox *)&score,
                                       (databox *)&member);
                }
                if (i >= 20) {
                    multiOrderedSetAdd(&b, (databox *)&score,
                                       (databox *)&member);
                }
            }

            const multiOrderedSet *sets[2] = {a, b};
            ctestPoisonStack();
            cleaner(multiOrderedSetFree) multiOrderedSet *u =
                multiOrderedSetUnion(sets, nullptr, 2, MOS_AGGREGATE_MAX);
            ctestPoisonStack();
            cleaner(multiOrderedSetFree) multiOrderedSet *x =
                multiOrderedSetIntersect(sets, nullptr, 2, MOS_AGGREGATE_MIN);
            ctestPoisonStack();
            cleaner(multiOrderedSetFree) multiOrderedSet *d =
                multiOrderedSetDifference(sets, 2);

            if (multiOrderedSetCount(u) != 60 ||
                multiOrderedSetCount(x) != 20 ||
                multiOrderedSetCount(d) != 20) {
                ERR("set sizes union %zu, intersect %zu, difference %zu",
                    multiOrderedSetCount(u), multiOrderedSetCount(x),
                    multiOrderedSetCount(d));
            }

            for (size_t i = 0; i < 60; i++) {
                databoxBig member;
                mosWideNumBox_(&member, members[i]);
                const bool inA = i < 40;
                const bool inB = i >= 20;
                if (!multiOrderedSetExists(u, (databox *)&member) ||
                    multiOrderedSetExists(x, (databox *)&member) !=
                        (inA && inB) ||
                    multiOrderedSetExists(d, (databox *)&member) !=
                        (inA && !inB)) {
                    ERR("set membership wrong for member %zu", i);
                }

                /* Union scores are doubles of the inputs, stored in their
                 * canonical form: an integral double that fits 64 bits
                 * reads back as an integer */
                databox score = {};
                const mosWideNum want = scores[i];
                const double wantD = want.neg ? (double)(__int128_t)want.bits
                                              : (double)want.bits;
                double gotD = 0;
                if (!multiOrderedSetGetScore(u, (databox *)&member, &score) ||
                    !mosScoreToDouble(&score, &gotD) || gotD != wantD) {
                    ERR("union score wrong for member %zu", i);
                }
            }
        }
    }

    TEST("IncrBy: exact integer sums, refused overflow and non-finite") {
        cleaner(multiOrderedSetFree) multiOrderedSet *mos =
            multiOrderedSetNew();
        const databox mInt = databoxNewBytesString("int");
        const databox mBig = databoxNewBytesString("big");
        const databox mReal = databoxNewBytesString("real");
        const databox mNew = databoxNewBytesString("new");
        const databox two53 = databoxNewSigned((int64_t)1 << 53);
        const databox one = databoxNewSigned(1);
        const databox maxU = databoxNewUnsigned(UINT64_MAX);
        const databox half = databoxNewReal(0.5);
        const databox dmax = databoxNewReal(DBL_MAX);
        const databox nan = databoxNewReal(NAN);
        databox r = {};

        /* 2^53 + 1 is exact as an integer (a double would round it) */
        if (!multiOrderedSetIncrBy(&mos, &two53, &mInt, &r) ||
            !multiOrderedSetIncrBy(&mos, &one, &mInt, &r) ||
            r.type != DATABOX_SIGNED_64 ||
            r.data.i64 != ((int64_t)1 << 53) + 1) {
            ERR("2^53 + 1 was not exact");
        }

        /* UINT64_MAX + 1 is refused and the score stays */
        if (!multiOrderedSetIncrBy(&mos, &maxU, &mBig, &r) ||
            multiOrderedSetIncrBy(&mos, &one, &mBig, &r)) {
            ERR("UINT64_MAX + 1 was not refused");
        }

        databox s = {};
        if (!multiOrderedSetGetScore(mos, &mBig, &s) ||
            s.type != DATABOX_UNSIGNED_64 || s.data.u64 != UINT64_MAX) {
            ERR("a refused IncrBy changed the score");
        }

        /* A real operand sums in double; inf and NaN are refused */
        if (!multiOrderedSetIncrBy(&mos, &dmax, &mReal, &r) ||
            !multiOrderedSetIncrBy(&mos, &half, &mInt, &r) ||
            multiOrderedSetIncrBy(&mos, &dmax, &mReal, &r) ||
            multiOrderedSetIncrBy(&mos, &nan, &mNew, &r) ||
            multiOrderedSetExists(mos, &mNew)) {
            ERR("real IncrBy did not follow the policy");
        }
    }

    TEST("Repr: once in each tier, members listed") {
        /* A 128-byte limit reaches Medium and Full within a few dozen
         * members (each tier's printer lists every member) */
        cleaner(multiOrderedSetFree) multiOrderedSet *mos =
            multiOrderedSetNewLimit(FLEX_CAP_LEVEL_128);
        bool printed[MOS_TYPE_FULL + 1] = {false};
        for (int64_t i = 0; i < 200 && !printed[MOS_TYPE_FULL]; i++) {
            char name[16];
            snprintf(name, sizeof(name), "m%03" PRId64, i);
            const databox s = databoxNewSigned(i);
            const databox m = databoxNewBytesString(name);
            multiOrderedSetAdd(&mos, &s, &m);
            if (!printed[mosType_(mos)]) {
                multiOrderedSetRepr(mos);
                printed[mosType_(mos)] = true;
            }
        }

        if (!printed[MOS_TYPE_SMALL] || !printed[MOS_TYPE_MEDIUM] ||
            !printed[MOS_TYPE_FULL]) {
            ERR("did not reach every tier to print it");
        }
    }

    /* Run per-tier test suites */
    err += multiOrderedSetSmallTest(argc, argv);
    err += multiOrderedSetMediumTest(argc, argv);

    /* Run multiOrderedSetFull-specific tests (including stringPool mode) */
    err += multiOrderedSetFullTest(argc, argv);

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
