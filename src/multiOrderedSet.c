#include "multiOrderedSet.h"

#include "multiOrderedSetSmall.h"
#include "multiOrderedSetSmallInternal.h"

#include "multiOrderedSetMedium.h"
#include "multiOrderedSetMediumInternal.h"

#include "multiOrderedSetFull.h"
#include "multiOrderedSetFullInternal.h"

/* ====================================================================
 * Databox Helpers
 * ==================================================================== */

/* Convert any numeric databox to double */
static inline double mosDataboxToDouble(const databox *box) {
    switch (box->type) {
    case DATABOX_DOUBLE_64:
        return box->data.d64;
    case DATABOX_FLOAT_32:
        return (double)box->data.f32;
    case DATABOX_SIGNED_64:
        return (double)box->data.i64;
    case DATABOX_UNSIGNED_64:
        return (double)box->data.u64;
    default:
        return 0.0;
    }
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
    static const void **jumper[4] = {NULL, &&mosSmall, &&mosMedium,            \
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
            assert(NULL);                                                      \
            __builtin_unreachable();                                           \
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
            assert(NULL);                                                      \
            __builtin_unreachable();                                           \
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

/* Threshold for Small → Medium promotion */
#define MOS_SMALL_MAX_BYTES 2048

/* Threshold for Medium → Full promotion (3x the small limit) */
#define MOS_MEDIUM_MAX_BYTES (MOS_SMALL_MAX_BYTES * 3)

/* Minimum entry count for promotion (must have 2 entries to split) */
#define MOS_MIN_ENTRIES_FOR_PROMOTION 2

DK_INLINE_ALWAYS void mosUpgradeIfNecessary(multiOrderedSet **m) {
    const uint32_t limit = COMPRESS_LIMIT(*m);
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
            *m = SET_COMPRESS_LIMIT(*m, limit);
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

            *m = MOS_TAG_(full, MOS_TYPE_FULL);
            *m = SET_COMPRESS_LIMIT(*m, limit);
        }
    }
    /* Full tier does not promote further */
}

/* ====================================================================
 * Creation / Destruction
 * ==================================================================== */

multiOrderedSet *multiOrderedSetNew(void) {
    multiOrderedSetSmall *created = multiOrderedSetSmallNew();
    multiOrderedSet *mos = MOS_TAG_(created, MOS_TYPE_SMALL);
    mos = SET_COMPRESS_DEPTH_LIMIT(mos, 0, FLEX_CAP_LEVEL_2048);
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

void multiOrderedSetReset(multiOrderedSet *mos) {
    MOS_SINGLE_NORETURN(mos, Reset);
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

bool multiOrderedSetIncrBy(multiOrderedSet **mos, const databox *delta,
                           const databox *member, databox *result) {
    bool ret;
    MOS_SETRESULT(ret, *mos, IncrBy, delta, member, result);
    mosUpgradeIfNecessary(mos);
    return ret;
}

/* ====================================================================
 * Deletion
 * ==================================================================== */

bool multiOrderedSetRemove(multiOrderedSet **mos, const databox *member) {
    /* TODO: auto-shrink behavior? */
    MOS_RETURN(*mos, Remove, member);
}

bool multiOrderedSetRemoveGetScore(multiOrderedSet **mos, const databox *member,
                                   databox *score) {
    MOS_RETURN(*mos, RemoveGetScore, member, score);
}

size_t multiOrderedSetRemoveRangeByScore(multiOrderedSet **mos,
                                         const mosRangeSpec *range) {
    MOS_RETURN(*mos, RemoveRangeByScore, range);
}

size_t multiOrderedSetRemoveRangeByRank(multiOrderedSet **mos, int64_t start,
                                        int64_t stop) {
    MOS_RETURN(*mos, RemoveRangeByRank, start, stop);
}

size_t multiOrderedSetPopMin(multiOrderedSet **mos, size_t count,
                             databox *members, databox *scores) {
    MOS_RETURN(*mos, PopMin, count, members, scores);
}

size_t multiOrderedSetPopMax(multiOrderedSet **mos, size_t count,
                             databox *members, databox *scores) {
    MOS_RETURN(*mos, PopMax, count, members, scores);
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
    static const void *jumper[] = {NULL, &&iterSmall, &&iterMedium, &&iterFull};
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
    (void)iter;
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
        return multiOrderedSetNew();
    }

    multiOrderedSet *result = multiOrderedSetNew();

    for (size_t i = 0; i < numSets; i++) {
        if (!sets[i]) {
            continue;
        }

        double weight = weights ? weights[i] : 1.0;

        mosIterator iter;
        multiOrderedSetIteratorInit(sets[i], &iter, true);

        databox member, score;
        while (multiOrderedSetIteratorNext(&iter, &member, &score)) {
            /* Check if member already exists in result */
            databox existingScore;
            if (multiOrderedSetGetScore(result, &member, &existingScore)) {
                /* Combine scores using aggregate */
                databox newScore;
                applyAggregate(&newScore, &existingScore, &score, 1.0, weight,
                               aggregate);
                multiOrderedSetAdd(&result, &newScore, &member);
            } else {
                /* New member, add with weighted score */
                databox weightedScore;
                double val = mosDataboxToDouble(&score) * weight;
                DATABOX_SET_DOUBLE(&weightedScore, val);
                multiOrderedSetAdd(&result, &weightedScore, &member);
            }
        }
    }

    return result;
}

multiOrderedSet *multiOrderedSetIntersect(const multiOrderedSet *sets[],
                                          const double *weights, size_t numSets,
                                          mosAggregate aggregate) {
    if (numSets < 2) {
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

    multiOrderedSet *result = multiOrderedSetNew();

    /* Iterate through smallest set */
    mosIterator iter;
    multiOrderedSetIteratorInit(sets[smallestIdx], &iter, true);

    databox member, score;
    while (multiOrderedSetIteratorNext(&iter, &member, &score)) {
        bool inAll = true;
        databox combinedScore = score;
        double weight0 = weights ? weights[smallestIdx] : 1.0;

        /* Apply weight to initial score */
        double val = mosDataboxToDouble(&combinedScore) * weight0;
        DATABOX_SET_DOUBLE(&combinedScore, val);

        /* Check if member exists in all other sets */
        for (size_t i = 0; i < numSets && inAll; i++) {
            if (i == smallestIdx) {
                continue;
            }

            databox otherScore;
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
        return multiOrderedSetNew();
    }

    multiOrderedSet *result = multiOrderedSetCopy(sets[0]);

    /* Remove members that exist in any other set */
    for (size_t i = 1; i < numSets; i++) {
        if (!sets[i]) {
            continue;
        }

        mosIterator iter;
        multiOrderedSetIteratorInit(sets[i], &iter, true);

        databox member, score;
        while (multiOrderedSetIteratorNext(&iter, &member, &score)) {
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

#define REPORT_TIME 1
#if REPORT_TIME
#define TIME_INIT PERF_TIMERS_SETUP
#define TIME_FINISH(i, what) PERF_TIMERS_FINISH_PRINT_RESULTS(i, what)
#else
#define TIME_INIT
#define TIME_FINISH(i, what)
#endif

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

int multiOrderedSetTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;

    TEST("multiOrderedSet: create and free") {
        multiOrderedSet *mos = multiOrderedSetNew();
        assert(mos != NULL);
        assert(multiOrderedSetCount(mos) == 0);
        multiOrderedSetFree(mos);
    }

    TEST("multiOrderedSet: add and count") {
        multiOrderedSet *mos = multiOrderedSetNew();

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

        multiOrderedSetFree(mos);
    }

    TEST("multiOrderedSet: NX and XX semantics") {
        multiOrderedSet *mos = multiOrderedSetNew();

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

        multiOrderedSetFree(mos);
    }

    TEST("multiOrderedSet: get score and rank") {
        multiOrderedSet *mos = multiOrderedSetNew();

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

        multiOrderedSetFree(mos);
    }

    TEST("multiOrderedSet: remove") {
        multiOrderedSet *mos = multiOrderedSetNew();

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

        multiOrderedSetFree(mos);
    }

    TEST("multiOrderedSet: iteration") {
        multiOrderedSet *mos = multiOrderedSetNew();

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

    TEST("multiOrderedSet: first and last") {
        multiOrderedSet *mos = multiOrderedSetNew();

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

        multiOrderedSetFree(mos);
    }

    TEST("multiOrderedSet: pop min/max") {
        multiOrderedSet *mos = multiOrderedSetNew();

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

        popped = multiOrderedSetPopMax(&mos, 1, poppedMembers, poppedScores);
        assert(popped == 1);
        assert(mosDataboxToDouble(&poppedScores[0]) == 50.0);
        assert(multiOrderedSetCount(mos) == 2);

        multiOrderedSetFree(mos);
    }

    TEST("multiOrderedSet: count by score range") {
        multiOrderedSet *mos = multiOrderedSetNew();

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

        multiOrderedSetFree(mos);
    }

    TEST("multiOrderedSet: union") {
        multiOrderedSet *set1 = multiOrderedSetNew();
        multiOrderedSet *set2 = multiOrderedSetNew();

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
        multiOrderedSet *result =
            multiOrderedSetUnion(sets, NULL, 2, MOS_AGGREGATE_SUM);

        assert(multiOrderedSetCount(result) == 3); /* a, b, c */

        databox gotScore;
        member = databoxNewBytes("b", 1);
        multiOrderedSetGetScore(result, &member, &gotScore);
        assert(mosDataboxToDouble(&gotScore) == 5.0); /* 2 + 3 */

        multiOrderedSetFree(set1);
        multiOrderedSetFree(set2);
        multiOrderedSetFree(result);
    }

    TEST("multiOrderedSet: intersection") {
        multiOrderedSet *set1 = multiOrderedSetNew();
        multiOrderedSet *set2 = multiOrderedSetNew();

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
        multiOrderedSet *result =
            multiOrderedSetIntersect(sets, NULL, 2, MOS_AGGREGATE_SUM);

        assert(multiOrderedSetCount(result) == 2); /* b, c */

        databox gotScore;
        member = databoxNewBytes("b", 1);
        multiOrderedSetGetScore(result, &member, &gotScore);
        assert(mosDataboxToDouble(&gotScore) == 12.0); /* 2 + 10 */

        multiOrderedSetFree(set1);
        multiOrderedSetFree(set2);
        multiOrderedSetFree(result);
    }

    TEST("multiOrderedSet: copy") {
        multiOrderedSet *mos = multiOrderedSetNew();

        databox score, member;
        for (int i = 0; i < 10; i++) {
            DATABOX_SET_DOUBLE(&score, (double)i);
            char buf[8];
            int len = snprintf(buf, sizeof(buf), "k%d", i);
            member = databoxNewBytes(buf, len);
            multiOrderedSetAdd(&mos, &score, &member);
        }

        multiOrderedSet *copy = multiOrderedSetCopy(mos);
        assert(multiOrderedSetCount(copy) == multiOrderedSetCount(mos));

        /* Modify original, verify copy unaffected */
        member = databoxNewBytes("k5", 2);
        multiOrderedSetRemove(&mos, &member);
        assert(multiOrderedSetCount(mos) == 9);
        assert(multiOrderedSetCount(copy) == 10);

        multiOrderedSetFree(mos);
        multiOrderedSetFree(copy);
    }

    TEST("multiOrderedSet: integer scores") {
        multiOrderedSet *mos = multiOrderedSetNew();

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

        multiOrderedSetFree(mos);
    }

    TEST("multiOrderedSet: getByRank") {
        multiOrderedSet *mos = multiOrderedSetNew();

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
            databox score = {.type = DATABOX_SIGNED_64, .data.i = i};
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
            databox score = {.type = DATABOX_SIGNED_64, .data.i = i};
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
            databox score = {.type = DATABOX_SIGNED_64, .data.i = i};
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
        multiOrderedSetType prevType = MOS_TYPE_SMALL;
        for (int i = 0; i < 30; i++) {
            int64_t scoreVal =
                (i * 997) % 10000; /* Pseudo-random distribution */
            databox score = {.type = DATABOX_SIGNED_64, .data.i = scoreVal};
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
                ERR("Entry m0 has wrong score %ld after inserting m%d!",
                    gotScore.data.i, i);
            }
            prevType = type;
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
        int64_t *oracleScores = zcalloc(1000, sizeof(int64_t));
        char (*oracleMembers)[32] = zcalloc(1000, 32);
        size_t oracleCount = 0;

        multiOrderedSetType prevType = MOS_TYPE_SMALL;
        int transitions = 0;

        for (int i = 0; i < 1000; i++) {
            /* Insert with random-ish score pattern */
            int64_t score = (i * 997) % 10000; /* Pseudo-random distribution */
            oracleScores[oracleCount] = score;
            snprintf(oracleMembers[oracleCount], 32, "member_%d", i);

            databox scoreBox = {.type = DATABOX_SIGNED_64, .data.i = score};
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
                            "%" PRId64 " got %" PRId64,
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

        zfree(oracleScores);
        zfree(oracleMembers);
        multiOrderedSetFree(mos);
    }

    TEST("FUZZ: Random access after tier promotion") {
        multiOrderedSet *mos = multiOrderedSetNewLimit(FLEX_CAP_LEVEL_64);

        int64_t *oracle = zcalloc(5000, sizeof(int64_t));
        size_t oracleCount = 0;

        /* Insert 5000 entries to ensure Full tier */
        for (int32_t i = 0; i < 5000; i++) {
            databox score = {.type = DATABOX_SIGNED_64, .data.i = i * 10};
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

        zfree(oracle);
        multiOrderedSetFree(mos);
    }

    TEST("FUZZ: Delete and re-insert at tier boundaries") {
        multiOrderedSet *mos = multiOrderedSetNewLimit(FLEX_CAP_LEVEL_64);

        /* Build up to near-transition */
        for (int i = 0; i < 200; i++) {
            databox score = {.type = DATABOX_SIGNED_64, .data.i = i};
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
            databox score = {.type = DATABOX_SIGNED_64, .data.i = i * 2 + 1000};
            char buf[32];
            snprintf(buf, sizeof(buf), "entry%d", i * 2);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            multiOrderedSetAdd(&mos, &score, &member);
        }

        assert(multiOrderedSetCount(mos) == initialCount);

        /* Verify sorted order */
        mosIterator iter;
        multiOrderedSetIteratorInit(mos, &iter, true);
        databox prevScore = {.type = DATABOX_SIGNED_64, .data.i = INT64_MIN};
        databox gotMember, gotScore;
        int count = 0;
        while (multiOrderedSetIteratorNext(&iter, &gotMember, &gotScore)) {
            if (databoxCompare(&gotScore, &prevScore) < 0) {
                ERRR("FUZZ FAIL: Sort order broken after delete/re-insert!");
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
        multiOrderedSet *mos = multiOrderedSetNew();

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
        size_t numValues = sizeof(testValues) / sizeof(testValues[0]);

        for (size_t i = 0; i < numValues; i++) {
            databox score = {.type = DATABOX_UNSIGNED_64,
                             .data.u = testValues[i]};
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
                           " (got %" PRIu64 ")\n",
                           testValues[i], gotScore.data.u);
                }
            }
        }

        /* Verify sorted order is correct by comparing adjacent pairs */
        int sortErrors = 0;
        mosIterator iter;
        multiOrderedSetIteratorInit(mos, &iter, true);
        databox prevMember = {0}, prevScore = {0};
        databox gotMember, gotScore;
        bool first = true;
        while (multiOrderedSetIteratorNext(&iter, &gotMember, &gotScore)) {
            if (!first) {
                /* Scores should be strictly increasing */
                int cmp = databoxCompare(&prevScore, &gotScore);
                if (cmp >= 0) {
                    sortErrors++;
                    if (sortErrors <= 3) {
                        printf("      SORT ERROR: %" PRIu64 " >= %" PRIu64 "\n",
                               prevScore.data.u, gotScore.data.u);
                    }
                }
            }
            first = false;
            prevScore = gotScore;
            prevMember = gotMember;
        }

        printf("      Large uint64 precision_errors=%d sort_errors=%d: %s\n",
               precisionErrors, sortErrors,
               (precisionErrors == 0 && sortErrors == 0) ? "OK" : "ISSUES");

        multiOrderedSetFree(mos);
    }

    TEST("PRECISION: Large negative int64_t scores") {
        multiOrderedSet *mos = multiOrderedSetNew();

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
        size_t numValues = sizeof(testValues) / sizeof(testValues[0]);

        for (size_t i = 0; i < numValues; i++) {
            databox score = {.type = DATABOX_SIGNED_64,
                             .data.i = testValues[i]};
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

        multiOrderedSetFree(mos);
    }

    TEST("PRECISION: Mixed score types (int64, uint64, double)") {
        multiOrderedSet *mos = multiOrderedSetNew();

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

        multiOrderedSetFree(mos);
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
                databox score = {.type = DATABOX_SIGNED_64,
                                 .data.i = (int64_t)i};
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
                databox score = {.type = DATABOX_SIGNED_64,
                                 .data.i = (int64_t)i};
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
                databox score = {.type = DATABOX_SIGNED_64,
                                 .data.i = (int64_t)i};
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
            databox score = {.type = DATABOX_SIGNED_64, .data.i = (int64_t)i};
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
            databox score = {.type = DATABOX_SIGNED_64, .data.i = (int64_t)i};
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
            int64_t score = (int64_t)(xoroshiro128plus(seed) % 1000000);
            databox scoreBox = {.type = DATABOX_SIGNED_64, .data.i = score};
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
        multiOrderedSet *mos = multiOrderedSetNew();

        databox score = {.type = DATABOX_SIGNED_64, .data.i = 42};
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
        multiOrderedSetFree(mos);
    }

    TEST("STRESS: Score update (replace) performance") {
        multiOrderedSet *mos = multiOrderedSetNewLimit(FLEX_CAP_LEVEL_64);

        /* Initial insert */
        for (int i = 0; i < 1000; i++) {
            databox score = {.type = DATABOX_SIGNED_64, .data.i = i};
            char buf[32];
            snprintf(buf, sizeof(buf), "upd%d", i);
            databox member = databoxNewBytesAllowEmbed(buf, strlen(buf));
            multiOrderedSetAdd(&mos, &score, &member);
        }

        /* Update all scores (should be faster than delete+insert) */
        TIME_INIT;
        for (int round = 0; round < 10; round++) {
            for (int i = 0; i < 1000; i++) {
                databox score = {.type = DATABOX_SIGNED_64,
                                 .data.i = i + round * 1000};
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
            databox score = {.type = DATABOX_SIGNED_64, .data.i = i * 5};
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
            databox score = {.type = DATABOX_SIGNED_64, .data.i = i * 3};
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
            databox score = {.type = DATABOX_SIGNED_64, .data.i = i * 7};
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
    static const size_t testCounts[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    static const size_t numTestCounts =
        sizeof(testCounts) / sizeof(testCounts[0]);

    TEST("(full width) insert / exists / lookup / delete / rank at various "
         "sizes") {
        for (size_t tc = 0; tc < numTestCounts; tc++) {
            size_t count = testCounts[tc];

            printf("test — %zu entries - inserting...\n", count);
            multiOrderedSet *mos = multiOrderedSetNew();

            /* Pre-allocate key buffers to avoid timing snprintf */
            char (*keys)[32] = NULL;
            if (count > 0) {
                keys = zcalloc(count, 32);
                for (size_t i = 0; i < count; i++) {
                    snprintf(keys[i], 32, "key%zu", i);
                }
            }

            {
                TIME_INIT;
                for (size_t i = 0; i < count; i++) {
                    databox score = {.type = DATABOX_SIGNED_64,
                                     .data.i = (int64_t)i};
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
                    printf("        score=%ld member='%.*s'\n", gotScore.data.i,
                           (int)gotMember.len,
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
                    databox score = {.type = DATABOX_SIGNED_64,
                                     .data.i = (int64_t)(count - i)};
                    databox member =
                        databoxNewBytesAllowEmbed(keys[i], strlen(keys[i]));
                    multiOrderedSetAdd(&mos, &score, &member);
                }
                TIME_FINISH(count, "insert (reverse order)");
            }
            MOS_PRINT_STATS(mos);

            if (keys) {
                zfree(keys);
            }
            multiOrderedSetFree(mos);
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
        const size_t NUM_COMPARE =
            sizeof(COMPARE_COUNTS) / sizeof(COMPARE_COUNTS[0]);

        for (size_t cc = 0; cc < NUM_COMPARE; cc++) {
            size_t count = COMPARE_COUNTS[cc];
            printf("--- %zu entries ---\n", count);

            /* Pre-allocate keys */
            char (*keys)[32] = zcalloc(count, 32);
            for (size_t i = 0; i < count; i++) {
                snprintf(keys[i], 32, "cmp%zu", i);
            }

            /* ---- multiOrderedSet ---- */
            multiOrderedSet *mos = multiOrderedSetNew();

            printf("  multiOrderedSet:\n");
            {
                TIME_INIT;
                for (size_t i = 0; i < count; i++) {
                    databox score = {.type = DATABOX_SIGNED_64,
                                     .data.i = (int64_t)i};
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
            multimap *mm = multimapNew(2);

            printf("  multimap (2-element, key=score):\n");
            {
                TIME_INIT;
                for (size_t i = 0; i < count; i++) {
                    databox score = {.type = DATABOX_SIGNED_64,
                                     .data.i = (int64_t)i};
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
                    databox score = {.type = DATABOX_SIGNED_64,
                                     .data.i = (int64_t)i};
                    multimapExists(mm, &score);
                }
                TIME_FINISH(count, "MM exists (by score/key)");
            }
            {
                TIME_INIT;
                for (size_t i = 0; i < count; i++) {
                    databox score = {.type = DATABOX_SIGNED_64,
                                     .data.i = (int64_t)i};
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

            multiOrderedSetFree(mos);
            multimapFree(mm);
            zfree(keys);
        }
    }

    TEST("COMPARE: Random access patterns") {
        const size_t count = 10000;
        const size_t accessCount = 50000;

        /* Pre-allocate keys and scores for random access */
        char (*keys)[32] = zcalloc(count, 32);
        int64_t *scores = zcalloc(count, sizeof(int64_t));
        for (size_t i = 0; i < count; i++) {
            snprintf(keys[i], 32, "rnd%zu", i);
            scores[i] = (int64_t)(i * 17 % count); /* Pseudo-random scores */
        }

        /* Build both structures */
        multiOrderedSet *mos = multiOrderedSetNew();
        multimap *mm = multimapNew(2);

        for (size_t i = 0; i < count; i++) {
            databox score = {.type = DATABOX_SIGNED_64, .data.i = scores[i]};
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
                databox score = {.type = DATABOX_SIGNED_64,
                                 .data.i = scores[idx]};
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

        multiOrderedSetFree(mos);
        multimapFree(mm);
        zfree(keys);
        zfree(scores);
    }

    TEST("COMPARE: Iteration throughput") {
        const size_t count = 10000;

        /* Pre-allocate keys */
        char (*keys)[32] = zcalloc(count, 32);
        for (size_t i = 0; i < count; i++) {
            snprintf(keys[i], 32, "itr%zu", i);
        }

        /* Build both structures */
        multiOrderedSet *mos = multiOrderedSetNew();
        multimap *mm = multimapNew(2);

        for (size_t i = 0; i < count; i++) {
            databox score = {.type = DATABOX_SIGNED_64, .data.i = (int64_t)i};
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

        multiOrderedSetFree(mos);
        multimapFree(mm);
        zfree(keys);
    }

    printf("\n=== All Extended multiOrderedSet Tests Completed ===\n\n");

    /* Run multiOrderedSetFull-specific tests (including stringPool mode) */
    err += multiOrderedSetFullTest(argc, argv);

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
