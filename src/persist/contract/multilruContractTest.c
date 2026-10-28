/* multilruContractTest.c - persistOpsMultiLRU in the persistence contract
 *
 * The ops and argument shapes are multilruP's: INSERT [&handle, &weight],
 * CUSTOM (promote) [&handle], UPDATE [&handle, &weight], DELETE [&handle];
 * an eviction step is the DELETE of the evicted handle or, for an entry
 * above level 0, NUM_DECR_F0 (its demotion). */

#include "persistContract.h"

#ifdef DATAKIT_TEST

#include "../../cleaner.h"
#include "../../datakit.h"
#include "../../multilru.h"
#include "../../str.h"

defcleanerName(multilruPtr *, zfree, LruHandles);

static void pcLruDelete(multilru *lru, const persistContractBuild *b,
                        multilruPtr handle) {
    const void *args[1] = {&handle};
    multilruDelete(lru, handle);
    persistContractRecord(b->log, PERSIST_OP_DELETE, args, 1);
}

static void *pcLruBuild(const persistContractBuild *b) {
    uint64_t rng = b->seed;
    /* Weighted and unweighted LRUs of four or seven levels; limits far
     * above what a build holds, so evictions stay explicit (as multilruP
     * logs them) while the snapshot still carries every setting */
    const bool weights = splitmix64(&rng) & 1;
    const bool other = splitmix64(&rng) & 1;
    const multilruConfig config = {
        .maxLevels = other ? 7 : 4,
        .maxCount = other ? UINT64_C(1) << 40 : 0,
        .maxWeight = other && weights ? UINT64_C(1) << 50 : 0,
        .policy = other && weights ? MLRU_POLICY_HYBRID : MLRU_POLICY_COUNT,
        .evictStrategy = other ? MLRU_EVICT_SIZE_LRU : MLRU_EVICT_LRU,
        .enableWeights = weights,
    };
    /* @own: caller; free=multilruFree */
    multilru *lru = multilruNewWithConfig(&config);
    persistContractStart(b->log, lru);

    /* Live handles, so promotes, updates and deletes name real entries */
    cleanerName(zfree, LruHandles) multilruPtr *live =
        zcalloc(b->size + 1, sizeof(multilruPtr));
    size_t liveCount = 0;
    for (size_t i = 0; i < b->size; i++) {
        const uint64_t roll = splitmix64(&rng) % 16;
        const size_t pick =
            liveCount ? (size_t)(splitmix64(&rng) % liveCount) : 0;
        if (roll < 7 || liveCount == 0) {
            uint64_t weight = weights ? splitmix64(&rng) % 1000 : 0;
            multilruPtr handle = weights ? multilruInsertWeighted(lru, weight)
                                         : multilruInsert(lru);
            const void *args[2] = {&handle, &weight};
            persistContractRecord(b->log, PERSIST_OP_INSERT, args, 2);
            live[liveCount++] = handle;
        } else if (roll < 11) {
            multilruPtr handle = live[pick];
            const void *args[1] = {&handle};
            multilruIncrease(lru, handle);
            persistContractRecord(b->log, PERSIST_OP_CUSTOM, args, 1);
        } else if (roll < 13 && weights) {
            multilruPtr handle = live[pick];
            uint64_t weight = splitmix64(&rng) % 1000;
            const void *args[2] = {&handle, &weight};
            multilruUpdateWeight(lru, handle, weight);
            persistContractRecord(b->log, PERSIST_OP_UPDATE, args, 2);
        } else if (roll < 15) {
            pcLruDelete(lru, b, live[pick]);
            live[pick] = live[--liveCount];
        } else {
            /* One eviction step: a demotion above level 0, a true
             * eviction at it */
            multilruPtr victim;
            if (multilruRemoveMinimum(lru, &victim)) {
                const void *args[1] = {&victim};
                if (multilruIsPopulated(lru, victim)) {
                    persistContractRecord(b->log, PERSIST_OP_NUM_DECR_F0, args,
                                          1);
                } else {
                    persistContractRecord(b->log, PERSIST_OP_DELETE, args, 1);
                    for (size_t j = 0; j < liveCount; j++) {
                        if (live[j] == victim) {
                            live[j] = live[--liveCount];
                            break;
                        }
                    }
                }
            }
        }
    }
    return lru;
}

static bool pcLruSameSettings(const void *original, const void *restored) {
    multilruStats a;
    multilruStats b;
    multilruGetStats(original, &a);
    multilruGetStats(restored, &b);
    return a.maxLevels == b.maxLevels && a.maxCount == b.maxCount &&
           a.maxWeight == b.maxWeight &&
           multilruGetPolicy(original) == multilruGetPolicy(restored) &&
           multilruGetEvictStrategy(original) ==
               multilruGetEvictStrategy(restored) &&
           multilruHasWeights(original) == multilruHasWeights(restored);
}

const persistContractType persistContractMultiLRU = {
    .ops = &persistOpsMultiLRU,
    .build = pcLruBuild,
    .sameSettings = pcLruSameSettings,
};

#endif /* DATAKIT_TEST */
