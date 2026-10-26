/* multilruP.c - Persistent multilru wrapper
 *
 * See multilruP.h for API documentation.
 */

#include "multilruP.h"
#include "../cleaner.h"
#include "../datakit.h"
#include "../persist.h"

#include <assert.h>
#include <string.h>
#include <unistd.h>

defcleanerName(multilruP *, zfree, MultilruP);
defcleanerName(multilruPtr *, zfree, MultilruPtrs);

/* ============================================================================
 * Eviction durability
 * ============================================================================
 * The wrapped multilru never evicts by itself (its auto-eviction is off).
 * After each insert the wrapper enforces the limits one step at a time,
 * logging each step before taking it: the step takes the LRU minimum,
 * demoting it (NUM_DECR_F0) above level 0 and evicting it (DELETE) at
 * level 0, exactly as multilruRemoveMinimum does.  Replay applies those
 * records and never runs the eviction policy itself, so it reaches the
 * same state whatever the restored LRU's recency order. */

/* The weaker durability of two logged results: a logged op is as durable
 * as the least durable of its steps */
static persistLogResult multilruPWorse(persistLogResult a, persistLogResult b) {
    if (a == PERSIST_LOG_NOT_DURABLE || b == PERSIST_LOG_NOT_DURABLE) {
        return PERSIST_LOG_NOT_DURABLE;
    }
    if (a == PERSIST_LOG_BUFFERED || b == PERSIST_LOG_BUFFERED) {
        return PERSIST_LOG_BUFFERED;
    }
    return a;
}

/* Log, then take, one eviction step: UNCHANGED when there is nothing to
 * take, REFUSED (nothing changes) when the WAL write fails */
static persistLogResult multilruPEvictStep(multilruP *mlru,
                                           multilruPtr *victimOut) {
    multilruPtr victim = multilruPeekMinimum(mlru->mlru);
    if (!victim) {
        return PERSIST_LOG_UNCHANGED;
    }
    const persistOp op = multilruGetLevel(mlru->mlru, victim) > 0
                             ? PERSIST_OP_NUM_DECR_F0
                             : PERSIST_OP_DELETE;
    const void *args[1] = {&victim};
    const persistLogResult logged = persistCtxLogOp(mlru->ctx, op, args, 1);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }
    multilruPtr taken;
    multilruRemoveMinimum(mlru->mlru, &taken);
    assert(taken == victim);
    if (victimOut) {
        *victimOut = victim;
    }
    return logged;
}

/* Evict, step by logged step, until the limits hold; folds each step's
 * durability into 'result'.  A refused step stops eviction (the LRU stays
 * over its limit, as the WAL records it; the next change retries). */
static persistLogResult multilruPEnforceLimits(multilruP *mlru,
                                               persistLogResult result) {
    while (multilruNeedsEviction(mlru->mlru)) {
        const persistLogResult step = multilruPEvictStep(mlru, NULL);
        if (step == PERSIST_LOG_REFUSED || step == PERSIST_LOG_UNCHANGED) {
            break;
        }
        result = multilruPWorse(result, step);
    }
    return result;
}

/* Insert with every step logged before it is taken: the handle the insert
 * will get, then the eviction steps the limits require.  '*handle' is the
 * new entry, or 0 when it was itself evicted. */
static persistLogResult multilruPInsert_(multilruP *mlru, uint64_t weight,
                                         multilruPtr *handleOut) {
    if (handleOut) {
        *handleOut = 0;
    }
    if (!mlru || !mlru->mlru || !mlru->ctx) {
        return PERSIST_LOG_REFUSED;
    }

    multilruPtr handle = multilruPeekNextHandle(mlru->mlru);
    const void *args[2] = {&handle, &weight};
    persistLogResult logged =
        persistCtxLogOp(mlru->ctx, PERSIST_OP_INSERT, args, 2);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }
    const multilruPtr got = multilruInsertWeighted(mlru->mlru, weight);
    assert(got == handle);
    (void)got;

    logged = multilruPEnforceLimits(mlru, logged);
    persistCtxMaybeCompact(mlru->ctx, mlru->mlru, &persistOpsMultiLRU);
    if (handleOut && multilruIsPopulated(mlru->mlru, handle)) {
        *handleOut = handle;
    }
    return logged;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/* A wrapper around a new multilru made from 'config': its auto-eviction
 * is off (the wrapper logs every eviction step) and its starting snapshot
 * (configuration included) is saved */
static multilruP *multilruPNew_(persistCtx *ctx, const multilruConfig *config) {
    if (!ctx || !config) {
        return NULL;
    }

    cleanerName(zfree, MultilruP) multilruP *mlru = zcalloc(1, sizeof(*mlru));
    mlru->ctx = ctx;
    /* @own: container; free=multilruFree */
    mlru->mlru = multilruNewWithConfig(config);
    if (!mlru->mlru) {
        return NULL;
    }
    multilruSetAutoEvict(mlru->mlru, false);

    if (!persistCtxInitForType(ctx, &persistOpsMultiLRU) ||
        !persistCtxSaveSnapshot(ctx, mlru->mlru, &persistOpsMultiLRU)) {
        multilruFree(mlru->mlru);
        return NULL;
    }

    persistCtxClaim(ctx);
    multilruP *out = mlru;
    mlru = NULL;
    return out;
}

multilruP *multilruNewP(persistCtx *ctx) {
    /* multilruNew's configuration */
    const multilruConfig config = {
        .maxLevels = 7,
        .policy = MLRU_POLICY_COUNT,
        .evictStrategy = MLRU_EVICT_LRU,
    };
    return multilruPNew_(ctx, &config);
}

multilruP *multilruNewWithLevelsP(persistCtx *ctx, size_t maxLevels,
                                  uint64_t maxCount) {
    const multilruConfig config = {
        .maxLevels = maxLevels,
        .maxCount = maxCount,
        .policy = MLRU_POLICY_COUNT,
        .evictStrategy = MLRU_EVICT_LRU,
    };
    return multilruPNew_(ctx, &config);
}

multilruP *multilruNewWithConfigP(persistCtx *ctx,
                                  const multilruConfig *config) {
    return multilruPNew_(ctx, config);
}

multilruP *multilruOpenP(persistCtx *ctx) {
    if (!ctx) {
        return NULL;
    }

    /* Initialize persistence context for multilru type */
    if (!persistCtxInitForType(ctx, &persistOpsMultiLRU)) {
        return NULL;
    }

    /* Recover from snapshot + WAL; every wrapper saves a starting
     * snapshot, so nothing recoverable means nothing to open */
    multilru *recovered = persistCtxRecover(ctx, &persistOpsMultiLRU);
    if (!recovered) {
        return NULL;
    }
    multilruSetAutoEvict(recovered, false);

    /* @own: caller; free=multilruCloseP */
    multilruP *mlru = zcalloc(1, sizeof(*mlru));
    mlru->ctx = ctx;
    mlru->mlru = recovered;
    persistCtxClaim(ctx);
    return mlru;
}

void multilruCloseP(multilruP *mlru) {
    if (!mlru) {
        return;
    }

    /* Sync before closing; the context is free for another wrapper */
    if (mlru->ctx) {
        persistCtxSync(mlru->ctx);
        persistCtxRelease(mlru->ctx);
    }

    /* Free underlying multilru */
    if (mlru->mlru) {
        multilruFree(mlru->mlru);
    }

    zfree(mlru);
}

multilru *multilruGetRawP(multilruP *mlru) {
    return mlru ? mlru->mlru : NULL;
}

/* ============================================================================
 * Metadata
 * ============================================================================
 */

size_t multilruCountP(const multilruP *mlru) {
    return mlru && mlru->mlru ? multilruCount(mlru->mlru) : 0;
}

size_t multilruBytesP(const multilruP *mlru) {
    return mlru && mlru->mlru ? multilruBytes(mlru->mlru) : 0;
}

uint64_t multilruTotalWeightP(const multilruP *mlru) {
    return mlru && mlru->mlru ? multilruTotalWeight(mlru->mlru) : 0;
}

size_t multilruLevelCountP(const multilruP *mlru, size_t level) {
    return mlru && mlru->mlru ? multilruLevelCount(mlru->mlru, level) : 0;
}

uint64_t multilruLevelWeightP(const multilruP *mlru, size_t level) {
    return mlru && mlru->mlru ? multilruLevelWeight(mlru->mlru, level) : 0;
}

uint64_t multilruGetWeightP(const multilruP *mlru, multilruPtr ptr) {
    return mlru && mlru->mlru ? multilruGetWeight(mlru->mlru, ptr) : 0;
}

size_t multilruGetLevelP(const multilruP *mlru, multilruPtr ptr) {
    return mlru && mlru->mlru ? multilruGetLevel(mlru->mlru, ptr) : 0;
}

bool multilruIsPopulatedP(const multilruP *mlru, multilruPtr ptr) {
    return mlru && mlru->mlru && multilruIsPopulated(mlru->mlru, ptr);
}

/* ============================================================================
 * Mutations
 * ============================================================================
 */

persistLogResult multilruInsertP(multilruP *mlru, multilruPtr *handle) {
    return multilruPInsert_(mlru, 0, handle);
}

persistLogResult multilruInsertWeightedP(multilruP *mlru, uint64_t weight,
                                         multilruPtr *handle) {
    return multilruPInsert_(mlru, weight, handle);
}

/* A live entry this wrapper can log a change of */
static bool multilruPEntry(const multilruP *mlru, multilruPtr ptr) {
    return mlru && mlru->mlru && mlru->ctx && ptr != 0 &&
           multilruIsPopulated(mlru->mlru, ptr);
}

persistLogResult multilruIncreaseP(multilruP *mlru, multilruPtr ptr) {
    if (!mlru || !mlru->mlru || !mlru->ctx) {
        return PERSIST_LOG_REFUSED;
    }
    if (!multilruPEntry(mlru, ptr)) {
        return PERSIST_LOG_UNCHANGED; /* as multilruIncrease: a no-op */
    }

    /* [handle], PERSIST_OP_CUSTOM for PROMOTE */
    const void *args[1] = {&ptr};
    const persistLogResult logged =
        persistCtxLogOp(mlru->ctx, PERSIST_OP_CUSTOM, args, 1);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }
    multilruIncrease(mlru->mlru, ptr);
    persistCtxMaybeCompact(mlru->ctx, mlru->mlru, &persistOpsMultiLRU);
    return logged;
}

persistLogResult multilruUpdateWeightP(multilruP *mlru, multilruPtr ptr,
                                       uint64_t newWeight) {
    if (!mlru || !mlru->mlru || !mlru->ctx) {
        return PERSIST_LOG_REFUSED;
    }
    if (!multilruPEntry(mlru, ptr) ||
        multilruGetWeight(mlru->mlru, ptr) == newWeight) {
        return PERSIST_LOG_UNCHANGED;
    }

    /* [handle][newWeight]; a heavier entry can put the LRU over its
     * weight limit, so the limits are enforced (logged) at once */
    const void *args[2] = {&ptr, &newWeight};
    persistLogResult logged =
        persistCtxLogOp(mlru->ctx, PERSIST_OP_UPDATE, args, 2);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }
    multilruUpdateWeight(mlru->mlru, ptr, newWeight);
    logged = multilruPEnforceLimits(mlru, logged);
    persistCtxMaybeCompact(mlru->ctx, mlru->mlru, &persistOpsMultiLRU);
    return logged;
}

persistLogResult multilruRemoveMinimumP(multilruP *mlru, multilruPtr *out) {
    if (out) {
        *out = 0;
    }
    if (!mlru || !mlru->mlru || !mlru->ctx) {
        return PERSIST_LOG_REFUSED;
    }

    /* One step, logged first: a demotion above level 0, an eviction at
     * level 0 (multilruIsPopulated afterwards tells which) */
    const persistLogResult logged = multilruPEvictStep(mlru, out);
    if (logged != PERSIST_LOG_REFUSED && logged != PERSIST_LOG_UNCHANGED) {
        persistCtxMaybeCompact(mlru->ctx, mlru->mlru, &persistOpsMultiLRU);
    }
    return logged;
}

persistLogResult multilruDeleteP(multilruP *mlru, multilruPtr ptr) {
    if (!mlru || !mlru->mlru || !mlru->ctx) {
        return PERSIST_LOG_REFUSED;
    }
    if (!multilruPEntry(mlru, ptr)) {
        return PERSIST_LOG_UNCHANGED;
    }

    const void *args[1] = {&ptr};
    const persistLogResult logged =
        persistCtxLogOp(mlru->ctx, PERSIST_OP_DELETE, args, 1);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }
    multilruDelete(mlru->mlru, ptr);
    persistCtxMaybeCompact(mlru->ctx, mlru->mlru, &persistOpsMultiLRU);
    return logged;
}

/* ============================================================================
 * Configuration
 * ============================================================================
 */

/* A new limit is applied, evicted to (each step logged) and made durable
 * by a snapshot, since the limits live in the snapshot.  NOT_DURABLE when
 * that snapshot fails (the limit holds in memory; the next compaction
 * records it). */
static persistLogResult multilruPSetLimit(multilruP *mlru, bool count,
                                          uint64_t limit) {
    /* A limit is persisted by the compaction below, not by a log entry,
     * but is refused when a logged change would be: past a bad WAL entry
     * that compaction would also drop the damaged tail, which only an
     * explicit compaction may do, and the evictions could not be logged */
    if (!mlru || !mlru->mlru || !persistCtxCanLog(mlru->ctx)) {
        return PERSIST_LOG_REFUSED;
    }
    if (count) {
        multilruSetMaxCount(mlru->mlru, limit);
    } else {
        multilruSetMaxWeight(mlru->mlru, limit);
    }
    const persistLogResult evicted =
        multilruPEnforceLimits(mlru, PERSIST_LOG_DURABLE);
    if (!persistCtxCompact(mlru->ctx, mlru->mlru, &persistOpsMultiLRU)) {
        return PERSIST_LOG_NOT_DURABLE;
    }
    return evicted;
}

persistLogResult multilruSetMaxCountP(multilruP *mlru, uint64_t maxCount) {
    return multilruPSetLimit(mlru, true, maxCount);
}

uint64_t multilruGetMaxCountP(const multilruP *mlru) {
    return mlru && mlru->mlru ? multilruGetMaxCount(mlru->mlru) : 0;
}

persistLogResult multilruSetMaxWeightP(multilruP *mlru, uint64_t maxWeight) {
    return multilruPSetLimit(mlru, false, maxWeight);
}

uint64_t multilruGetMaxWeightP(const multilruP *mlru) {
    return mlru && mlru->mlru ? multilruGetMaxWeight(mlru->mlru) : 0;
}

/* ============================================================================
 * Persistence Control
 * ============================================================================
 */

bool multilruSyncP(multilruP *mlru) {
    if (!mlru || !mlru->ctx) {
        return false;
    }
    return persistCtxSync(mlru->ctx);
}

bool multilruCompactP(multilruP *mlru) {
    if (!mlru || !mlru->ctx || !mlru->mlru) {
        return false;
    }
    return persistCtxCompact(mlru->ctx, mlru->mlru, &persistOpsMultiLRU);
}

void multilruGetStatsP(const multilruP *mlru, persistCtxStats *stats) {
    if (!mlru || !mlru->ctx || !stats) {
        return;
    }
    persistCtxGetStats(mlru->ctx, stats);
}

/* ============================================================================
 * Tests
 * ============================================================================
 */

#ifdef DATAKIT_TEST
#include "../str.h"
#include "persistTestCommon.h"

defcleaner(multilru *, multilruFree);

/* The inserted handle (0 when refused or evicted at once), for tests that
 * only follow handles */
static multilruPtr mlruTestInsert(multilruP *mlru, uint64_t weight) {
    multilruPtr handle = 0;
    (void)multilruInsertWeightedP(mlru, weight, &handle);
    return handle;
}
defcleanerName(uint8_t *, zfree, SnapBytes);

static int test_multilruP_basic(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multilruPTest_basic", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multilruP basic persistence")

    ptestCleanupFiles(basePath);

    /* Create new persistent multilru */
    persistCtx *ctx = persistCtxNew(basePath, NULL);
    multilruP *mlru = multilruNewWithLevelsP(ctx, 7, 100);
    if (!mlru) {
        ERRR("Failed to create multilruP");
    }

    /* Insert some entries */
    multilruPtr h1 = mlruTestInsert(mlru, 0);
    multilruPtr h2 = mlruTestInsert(mlru, 0);
    multilruPtr h3 = mlruTestInsert(mlru, 0);

    if (h1 == 0 || h2 == 0 || h3 == 0) {
        ERRR("Failed to insert entries");
    }

    /* Verify count */
    if (multilruCountP(mlru) != 3) {
        ERR("Count should be 3, got %zu", multilruCountP(mlru));
    }

    /* Promote h1 to level 1 */
    PTEST_LOGGED(multilruIncreaseP(mlru, h1));
    if (multilruGetLevelP(mlru, h1) != 1) {
        ERR("h1 should be at level 1, got %zu", multilruGetLevelP(mlru, h1));
    }

    /* Close and reopen */
    multilruCloseP(mlru);
    persistCtxFree(ctx);

    ctx = persistCtxNew(basePath, NULL);
    mlru = multilruOpenP(ctx);
    if (!mlru) {
        ERRR("Failed to reopen multilruP");
    }

    /* Verify entries after recovery */
    if (multilruCountP(mlru) != 3) {
        ERR("Count should be 3 after recovery, got %zu", multilruCountP(mlru));
    }

    if (!multilruIsPopulatedP(mlru, h1)) {
        ERRR("h1 should exist after recovery");
    }
    if (!multilruIsPopulatedP(mlru, h2)) {
        ERRR("h2 should exist after recovery");
    }
    if (!multilruIsPopulatedP(mlru, h3)) {
        ERRR("h3 should exist after recovery");
    }

    if (multilruGetLevelP(mlru, h1) != 1) {
        ERR("h1 should be at level 1 after recovery, got %zu",
            multilruGetLevelP(mlru, h1));
    }

    /* Delete an entry */
    PTEST_LOGGED(multilruDeleteP(mlru, h2));
    if (multilruIsPopulatedP(mlru, h2)) {
        ERRR("h2 should be deleted");
    }
    if (multilruCountP(mlru) != 2) {
        ERR("Count should be 2, got %zu", multilruCountP(mlru));
    }

    /* Close and reopen again */
    multilruCloseP(mlru);
    persistCtxFree(ctx);

    ctx = persistCtxNew(basePath, NULL);
    mlru = multilruOpenP(ctx);
    if (!mlru) {
        ERRR("Failed to reopen multilruP");
    }

    /* Verify after second recovery */
    if (multilruCountP(mlru) != 2) {
        ERR("Count should be 2 after second recovery, got %zu",
            multilruCountP(mlru));
    }
    if (multilruIsPopulatedP(mlru, h2)) {
        ERRR("h2 should still be deleted after recovery");
    }

    /* Cleanup */
    multilruCloseP(mlru);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

static int test_multilruP_weighted(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multilruPTest_weighted", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multilruP weighted entries")

    ptestCleanupFiles(basePath);

    /* Create multilru with weights enabled */
    persistCtx *ctx = persistCtxNew(basePath, NULL);
    multilruConfig config = {
        .maxLevels = 7,
        .startCapacity = 0,
        .maxWeight = 0,
        .maxCount = 0,
        .policy = MLRU_POLICY_COUNT,
        .evictStrategy = MLRU_EVICT_LRU,
        .enableWeights = true,
    };
    multilruP *mlru = multilruNewWithConfigP(ctx, &config);
    if (!mlru) {
        ERRR("Failed to create weighted multilruP");
    }

    /* Insert weighted entries */
    multilruPtr h1 = mlruTestInsert(mlru, 100);
    multilruPtr h2 = mlruTestInsert(mlru, 200);
    mlruTestInsert(mlru, 150); /* h3 */

    /* Verify weights */
    if (multilruGetWeightP(mlru, h1) != 100) {
        ERR("h1 weight should be 100, got %" PRIu64,
            multilruGetWeightP(mlru, h1));
    }
    if (multilruGetWeightP(mlru, h2) != 200) {
        ERR("h2 weight should be 200, got %" PRIu64,
            multilruGetWeightP(mlru, h2));
    }

    /* Total weight */
    if (multilruTotalWeightP(mlru) != 450) {
        ERR("Total weight should be 450, got %" PRIu64,
            multilruTotalWeightP(mlru));
    }

    /* Update weight */
    PTEST_LOGGED(multilruUpdateWeightP(mlru, h1, 300));
    if (multilruGetWeightP(mlru, h1) != 300) {
        ERR("h1 weight should be 300 after update, got %" PRIu64,
            multilruGetWeightP(mlru, h1));
    }

    /* Close and reopen */
    multilruCloseP(mlru);
    persistCtxFree(ctx);

    ctx = persistCtxNew(basePath, NULL);
    mlru = multilruOpenP(ctx);
    if (!mlru) {
        ERRR("Failed to reopen weighted multilruP");
    }

    /* Verify weights after recovery */
    if (multilruGetWeightP(mlru, h1) != 300) {
        ERR("h1 weight should be 300 after recovery, got %" PRIu64,
            multilruGetWeightP(mlru, h1));
    }
    if (multilruGetWeightP(mlru, h2) != 200) {
        ERR("h2 weight should be 200 after recovery, got %" PRIu64,
            multilruGetWeightP(mlru, h2));
    }

    /* Cleanup */
    multilruCloseP(mlru);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

static int test_multilruP_compaction(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multilruPTest_compaction", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multilruP compaction")

    ptestCleanupFiles(basePath);

    persistCtxConfig config = persistCtxDefaultConfig();
    config.compactAfterOps = 10;
    persistCtx *ctx = persistCtxNew(basePath, &config);
    if (!ctx) {
        ERRR("Failed to create persistCtx");
    }

    multilruP *mlru = multilruNewP(ctx);
    if (!mlru) {
        ERRR("Failed to create multilruP");
    }

    /* Insert many entries to trigger compaction */
    multilruPtr handles[20];
    for (int i = 0; i < 20; i++) {
        handles[i] = mlruTestInsert(mlru, 0);
    }

    /* Force compaction */
    bool compacted = multilruCompactP(mlru);
    if (!compacted) {
        ERRR("Compaction failed");
    }

    /* Verify all entries still exist */
    if (multilruCountP(mlru) != 20) {
        ERR("Count should be 20 after compaction, got %zu",
            multilruCountP(mlru));
    }

    for (int i = 0; i < 20; i++) {
        if (!multilruIsPopulatedP(mlru, handles[i])) {
            ERR("Handle %d should still be populated after compaction", i);
        }
    }

    /* Cleanup */
    multilruCloseP(mlru);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

static int test_multilruP_empty_recovery(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multilruPTest_empty", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multilruP empty cache recovery")

    ptestCleanupFiles(basePath);

    /* Create and immediately close empty cache */
    persistCtx *ctx = persistCtxNew(basePath, NULL);
    multilruP *mlru = multilruNewP(ctx);
    if (!mlru) {
        ERRR("Failed to create multilruP");
    }

    multilruCloseP(mlru);
    persistCtxFree(ctx);

    /* Reopen empty cache */
    ctx = persistCtxNew(basePath, NULL);
    mlru = multilruOpenP(ctx);
    if (!mlru) {
        ERRR("Failed to reopen empty multilruP");
    }

    /* Verify it's empty */
    if (multilruCountP(mlru) != 0) {
        ERR("Count should be 0, got %zu", multilruCountP(mlru));
    }

    /* Verify we can still use it */
    multilruPtr h1 = mlruTestInsert(mlru, 0);
    if (h1 == 0) {
        ERRR("Failed to insert into recovered empty cache");
    }

    if (multilruCountP(mlru) != 1) {
        ERR("Count should be 1, got %zu", multilruCountP(mlru));
    }

    /* Cleanup */
    multilruCloseP(mlru);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

static int test_multilruP_large_dataset(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multilruPTest_large", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multilruP large dataset")

    ptestCleanupFiles(basePath);

    persistCtx *ctx = persistCtxNew(basePath, NULL);
    multilruP *mlru = multilruNewWithLevelsP(ctx, 7, 5000);
    if (!mlru) {
        ERRR("Failed to create multilruP");
    }

    /* Insert 2000 entries */
    const size_t numEntries = 2000;
    cleanerName(zfree, MultilruPtrs) multilruPtr *handles =
        zmalloc(numEntries * sizeof(multilruPtr));
    for (size_t i = 0; i < numEntries; i++) {
        handles[i] = mlruTestInsert(mlru, 0);
        if (handles[i] == 0) {
            ERR("Failed to insert entry %zu", i);
        }
    }

    /* Promote random entries to various levels */
    for (size_t i = 0; i < numEntries; i += 7) {
        for (int promo = 0; promo < 3; promo++) {
            PTEST_LOGGED(multilruIncreaseP(mlru, handles[i]));
        }
    }

    /* Delete some entries - save their handles for verification */
    cleanerName(zfree, MultilruPtrs) multilruPtr *deletedHandles =
        zmalloc(100 * sizeof(multilruPtr));
    for (size_t i = 100; i < 200; i++) {
        deletedHandles[i - 100] = handles[i];
        PTEST_LOGGED(multilruDeleteP(mlru, handles[i]));
        handles[i] = 0; /* Mark as deleted */
    }

    /* Count remaining */
    size_t expectedCount = numEntries - 100;
    if (multilruCountP(mlru) != expectedCount) {
        ERR("Count should be %zu, got %zu", expectedCount,
            multilruCountP(mlru));
    }

    /* Force compaction to save snapshot with gaps */
    multilruCompactP(mlru);

    /* Close and reopen */
    multilruCloseP(mlru);
    persistCtxFree(ctx);

    ctx = persistCtxNew(basePath, NULL);
    mlru = multilruOpenP(ctx);
    if (!mlru) {
        ERRR("Failed to reopen multilruP");
    }

    /* Verify count after recovery */
    if (multilruCountP(mlru) != expectedCount) {
        ERR("Count should be %zu after recovery, got %zu", expectedCount,
            multilruCountP(mlru));
    }

    /* Verify all non-deleted entries exist */
    for (size_t i = 0; i < numEntries; i++) {
        if (handles[i] != 0) {
            /* Should exist */
            if (!multilruIsPopulatedP(mlru, handles[i])) {
                ERR("Entry %zu should exist", i);
            }
        }
    }

    /* Verify deleted entries don't exist */
    for (size_t i = 0; i < 100; i++) {
        if (multilruIsPopulatedP(mlru, deletedHandles[i])) {
            ERR("Entry %zu should be deleted", i + 100);
        }
    }

    /* Cleanup */
    multilruCloseP(mlru);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

static int test_multilruP_handle_gaps(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multilruPTest_gaps", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multilruP handle gaps and recycling")

    ptestCleanupFiles(basePath);

    persistCtx *ctx = persistCtxNew(basePath, NULL);
    multilruP *mlru = multilruNewP(ctx);
    if (!mlru) {
        ERRR("Failed to create multilruP");
    }

    /* Insert 10 entries */
    multilruPtr handles[10];
    for (int i = 0; i < 10; i++) {
        handles[i] = mlruTestInsert(mlru, 0);
    }

    /* Delete every other entry to create gaps */
    for (int i = 0; i < 10; i += 2) {
        PTEST_LOGGED(multilruDeleteP(mlru, handles[i]));
    }

    /* Verify count */
    if (multilruCountP(mlru) != 5) {
        ERR("Count should be 5, got %zu", multilruCountP(mlru));
    }

    /* Force compaction to save snapshot with gaps */
    multilruCompactP(mlru);

    /* Close and reopen */
    multilruCloseP(mlru);
    persistCtxFree(ctx);

    ctx = persistCtxNew(basePath, NULL);
    mlru = multilruOpenP(ctx);
    if (!mlru) {
        ERRR("Failed to reopen multilruP");
    }

    /* Verify gaps are preserved */
    for (int i = 0; i < 10; i++) {
        bool shouldExist = (i % 2 == 1);
        bool doesExist = multilruIsPopulatedP(mlru, handles[i]);
        if (shouldExist != doesExist) {
            ERR("Handle %d existence mismatch: expected=%d got=%d", i,
                shouldExist, doesExist);
        }
    }

    /* Insert more entries - they should fill gaps or continue */
    multilruPtr newHandle = mlruTestInsert(mlru, 0);
    if (newHandle == 0) {
        ERRR("Failed to insert new entry after recovery");
    }

    /* Cleanup */
    multilruCloseP(mlru);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

static int test_multilruP_mixed_operations(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multilruPTest_mixed", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multilruP mixed operations stress test")

    ptestCleanupFiles(basePath);

    persistCtxConfig config = persistCtxDefaultConfig();
    config.compactAfterOps = 50;
    persistCtx *ctx = persistCtxNew(basePath, &config);

    multilruConfig mlruCfg = {
        .maxLevels = 5,
        .enableWeights = true,
    };
    multilruP *mlru = multilruNewWithConfigP(ctx, &mlruCfg);
    if (!mlru) {
        ERRR("Failed to create multilruP");
    }

    /* Perform 500 random operations */
    multilruPtr handles[100];
    size_t handleCount = 0;
    uint64_t xorshift = 123456789;

    for (int op = 0; op < 500; op++) {
        /* Simple xorshift PRNG */
        xorshift ^= xorshift << 13;
        xorshift ^= xorshift >> 7;
        xorshift ^= xorshift << 17;
        int choice = xorshift % 100;

        if (choice < 40 && handleCount < 100) {
            /* INSERT (40%) */
            uint64_t weight = (xorshift % 1000) + 1;
            multilruPtr h = mlruTestInsert(mlru, weight);
            if (h != 0) {
                handles[handleCount++] = h;
            }
        } else if (choice < 60 && handleCount > 0) {
            /* DELETE (20%) */
            size_t idx = xorshift % handleCount;
            PTEST_LOGGED(multilruDeleteP(mlru, handles[idx]));
            handles[idx] = handles[--handleCount];
        } else if (choice < 80 && handleCount > 0) {
            /* PROMOTE (20%) */
            size_t idx = xorshift % handleCount;
            PTEST_LOGGED(multilruIncreaseP(mlru, handles[idx]));
        } else if (choice < 95 && handleCount > 0) {
            /* UPDATE WEIGHT (15%) */
            size_t idx = xorshift % handleCount;
            uint64_t newWeight = (xorshift % 2000) + 1;
            PTEST_LOGGED(multilruUpdateWeightP(mlru, handles[idx], newWeight));
        } else if (handleCount > 50) {
            /* REMOVE MINIMUM (5%) */
            multilruPtr evicted;
            if (multilruRemoveMinimumP(mlru, &evicted)) {
                /* Remove from handles array if present */
                for (size_t i = 0; i < handleCount; i++) {
                    if (handles[i] == evicted) {
                        handles[i] = handles[--handleCount];
                        break;
                    }
                }
            }
        }
    }

    /* Record state before recovery */
    size_t countBefore = multilruCountP(mlru);

    /* Close and reopen */
    multilruCloseP(mlru);
    persistCtxFree(ctx);

    ctx = persistCtxNew(basePath, NULL);
    mlru = multilruOpenP(ctx);
    if (!mlru) {
        ERRR("Failed to reopen multilruP");
    }

    /* Verify count matches */
    if (multilruCountP(mlru) != countBefore) {
        ERR("Count mismatch: before=%zu after=%zu", countBefore,
            multilruCountP(mlru));
    }

    /* Verify all tracked handles still exist */
    for (size_t i = 0; i < handleCount; i++) {
        if (!multilruIsPopulatedP(mlru, handles[i])) {
            ERR("Handle %zu should exist after recovery", i);
        }
    }

    /* Cleanup */
    multilruCloseP(mlru);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

static int test_multilruP_weight_extremes(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multilruPTest_weights", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multilruP weight extremes")

    ptestCleanupFiles(basePath);

    persistCtx *ctx = persistCtxNew(basePath, NULL);
    multilruConfig config = {
        .maxLevels = 7,
        .enableWeights = true,
    };
    multilruP *mlru = multilruNewWithConfigP(ctx, &config);
    if (!mlru) {
        ERRR("Failed to create weighted multilruP");
    }

    /* Test zero weight */
    multilruPtr h0 = mlruTestInsert(mlru, 0);
    if (multilruGetWeightP(mlru, h0) != 0) {
        ERR("Zero weight should be preserved, got %" PRIu64,
            multilruGetWeightP(mlru, h0));
    }

    /* Test max weight */
    multilruPtr hMax = mlruTestInsert(mlru, UINT64_MAX);
    if (multilruGetWeightP(mlru, hMax) != UINT64_MAX) {
        ERR("Max weight should be preserved, got %" PRIu64,
            multilruGetWeightP(mlru, hMax));
    }

    /* Test updating to extremes */
    multilruPtr h1 = mlruTestInsert(mlru, 500);
    PTEST_LOGGED(multilruUpdateWeightP(mlru, h1, 0));
    if (multilruGetWeightP(mlru, h1) != 0) {
        ERR("Updated weight should be 0, got %" PRIu64,
            multilruGetWeightP(mlru, h1));
    }

    PTEST_LOGGED(multilruUpdateWeightP(mlru, h1, UINT64_MAX));
    if (multilruGetWeightP(mlru, h1) != UINT64_MAX) {
        ERR("Updated weight should be UINT64_MAX, got %" PRIu64,
            multilruGetWeightP(mlru, h1));
    }

    /* Close and reopen */
    multilruCloseP(mlru);
    persistCtxFree(ctx);

    ctx = persistCtxNew(basePath, NULL);
    mlru = multilruOpenP(ctx);
    if (!mlru) {
        ERRR("Failed to reopen multilruP");
    }

    /* Verify extreme weights after recovery */
    if (multilruGetWeightP(mlru, h0) != 0) {
        ERR("Zero weight should be preserved after recovery, got %" PRIu64,
            multilruGetWeightP(mlru, h0));
    }
    if (multilruGetWeightP(mlru, hMax) != UINT64_MAX) {
        ERR("Max weight should be preserved after recovery, got %" PRIu64,
            multilruGetWeightP(mlru, hMax));
    }
    if (multilruGetWeightP(mlru, h1) != UINT64_MAX) {
        ERR("Updated max weight should be preserved after recovery, got "
            "%" PRIu64,
            multilruGetWeightP(mlru, h1));
    }

    /* Cleanup */
    multilruCloseP(mlru);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

static int test_multilruP_auto_evict(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multilruPTest_evict", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multilruP auto-eviction")

    ptestCleanupFiles(basePath);

    persistCtx *ctx = persistCtxNew(basePath, NULL);
    multilruP *mlru = multilruNewWithLevelsP(ctx, 5, 10);
    if (!mlru) {
        ERRR("Failed to create multilruP");
    }

    /* Insert 15 entries - should trigger evictions at maxCount=10 */
    multilruPtr handles[15];
    for (int i = 0; i < 15; i++) {
        handles[i] = mlruTestInsert(mlru, 0);
        if (handles[i] == 0) {
            ERR("Failed to insert entry %d", i);
        }
    }

    /* Count should be capped at 10 */
    size_t finalCount = multilruCountP(mlru);
    if (finalCount > 10) {
        ERR("Count should be <= 10, got %zu", finalCount);
    }

    /* Some entries should have been evicted */
    size_t evictedCount = 0;
    for (int i = 0; i < 15; i++) {
        if (!multilruIsPopulatedP(mlru, handles[i])) {
            evictedCount++;
        }
    }
    if (evictedCount == 0) {
        ERRR("Some entries should have been evicted");
    }

    /* Force compaction to save snapshot with evictions */
    multilruCompactP(mlru);

    /* Close and reopen */
    multilruCloseP(mlru);
    persistCtxFree(ctx);

    ctx = persistCtxNew(basePath, NULL);
    mlru = multilruOpenP(ctx);
    if (!mlru) {
        ERRR("Failed to reopen multilruP");
    }

    /* Verify count is still within limit */
    if (multilruCountP(mlru) > 10) {
        ERR("Count should be <= 10 after recovery, got %zu",
            multilruCountP(mlru));
    }

    /* Cleanup */
    multilruCloseP(mlru);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

static int test_multilruP_auto_evict_crash_before_compact(int argc,
                                                          char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multilruPTest_evict_crash", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multilruP auto-eviction survives reopen without compaction")

    ptestCleanupFiles(basePath);

    persistCtx *ctx = persistCtxNew(basePath, NULL);
    multilruP *mlru = multilruNewWithLevelsP(ctx, 5, 10);
    if (!mlru) {
        ERRR("Failed to create multilruP");
    }

    /* Overfill so inserts auto-evict; each eviction must be WAL-logged
     * as a DELETE or replay would resurrect it. */
    enum { INSERTED = 30 };
    multilruPtr handles[INSERTED];
    for (int i = 0; i < INSERTED; i++) {
        handles[i] = mlruTestInsert(mlru, 0);
        if (handles[i] == 0) {
            ERR("Failed to insert entry %d", i);
        }
    }

    bool evictedBefore[INSERTED];
    for (int i = 0; i < INSERTED; i++) {
        evictedBefore[i] = !multilruIsPopulatedP(mlru, handles[i]);
    }
    const size_t countBefore = multilruCountP(mlru);
    if (countBefore > 10) {
        ERR("Count should be <= 10, got %zu", countBefore);
    }

    /* Reopen WITHOUT compaction: recovery must replay the WAL,
     * including the auto-eviction DELETEs (models a crash before the
     * next compaction). */
    multilruCloseP(mlru);
    persistCtxFree(ctx);

    ctx = persistCtxNew(basePath, NULL);
    mlru = multilruOpenP(ctx);
    if (!mlru) {
        ERRR("Failed to reopen multilruP");
    }

    if (multilruCountP(mlru) != countBefore) {
        ERR("Count changed across WAL-only reopen: before %zu after %zu",
            countBefore, multilruCountP(mlru));
    }
    for (int i = 0; i < INSERTED; i++) {
        if (evictedBefore[i] && multilruIsPopulatedP(mlru, handles[i])) {
            ERR("Evicted entry %d resurrected after WAL replay", i);
        }
    }

    multilruCloseP(mlru);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

static int test_multilruP_level_promotion(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multilruPTest_levels", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multilruP level promotion persistence")

    ptestCleanupFiles(basePath);

    persistCtx *ctx = persistCtxNew(basePath, NULL);
    multilruP *mlru = multilruNewWithLevelsP(ctx, 7, 0);
    if (!mlru) {
        ERRR("Failed to create multilruP");
    }

    /* Insert entry and promote through all levels */
    multilruPtr h1 = mlruTestInsert(mlru, 0);
    if (multilruGetLevelP(mlru, h1) != 0) {
        ERR("New entry should be at level 0, got %zu",
            multilruGetLevelP(mlru, h1));
    }

    /* Promote to each level */
    for (size_t targetLevel = 1; targetLevel <= 6; targetLevel++) {
        PTEST_LOGGED(multilruIncreaseP(mlru, h1));
        size_t currentLevel = multilruGetLevelP(mlru, h1);
        if (currentLevel != targetLevel) {
            ERR("After promotion %zu, level should be %zu, got %zu",
                targetLevel, targetLevel, currentLevel);
        }
    }

    /* Close and reopen */
    multilruCloseP(mlru);
    persistCtxFree(ctx);

    ctx = persistCtxNew(basePath, NULL);
    mlru = multilruOpenP(ctx);
    if (!mlru) {
        ERRR("Failed to reopen multilruP");
    }

    /* Verify final level */
    if (multilruGetLevelP(mlru, h1) != 6) {
        ERR("Entry should be at level 6 after recovery, got %zu",
            multilruGetLevelP(mlru, h1));
    }

    /* Try to promote beyond max level (should stay at 6) */
    PTEST_LOGGED(multilruIncreaseP(mlru, h1));
    if (multilruGetLevelP(mlru, h1) != 6) {
        ERR("Entry should stay at level 6, got %zu",
            multilruGetLevelP(mlru, h1));
    }

    /* Cleanup */
    multilruCloseP(mlru);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

static int test_multilruP_demotions_replay(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multilruPTest_demotions", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multilruP demotions, handles and limits survive reopen")

    /* Count-limited, with promotions between inserts, so enforcing the
     * limit demotes entries as well as evicting them; the reopened LRU
     * must hold the same entries at the same levels and hand out the
     * same next handle, and a limit change must survive */
    ptestCleanupFiles(basePath);
    persistCtx *ctx = persistCtxNew(basePath, NULL);
    multilruP *mlru = multilruNewWithLevelsP(ctx, 4, 8);
    multilruPtr live[64] = {};
    size_t liveCount = 0;
    uint64_t rng = 0x1234;
    for (int i = 0; i < 60; i++) {
        const multilruPtr h = mlruTestInsert(mlru, 0);
        if (h && liveCount < 64) {
            live[liveCount++] = h;
        }
        for (int p = 0; p < 3 && liveCount; p++) {
            rng = rng * 6364136223846793005ULL + 1;
            PTEST_LOGGED(
                multilruIncreaseP(mlru, live[(rng >> 33) % liveCount]));
        }
    }

    /* Empty level 0 by promoting every entry, so the next removals of the
     * minimum demote instead of evicting */
    for (size_t i = 0; i < liveCount; i++) {
        if (multilruIsPopulatedP(mlru, live[i]) &&
            multilruGetLevelP(mlru, live[i]) == 0) {
            PTEST_LOGGED(multilruIncreaseP(mlru, live[i]));
        }
    }
    for (int i = 0; i < 4; i++) {
        multilruPtr victim;
        PTEST_LOGGED(multilruRemoveMinimumP(mlru, &victim));
    }

    multilruStats stats;
    multilruGetStats(multilruGetRawP(mlru), &stats);
    if (stats.demotions == 0) {
        ERRR("The workload never demoted (test does not cover demotion)");
    }

    /* A lower limit is evicted to at once, each step logged */
    if (!multilruSetMaxCountP(mlru, 5) ||
        multilruCount(multilruGetRawP(mlru)) > 5) {
        ERR("after lowering the limit to 5 the LRU holds %zu",
            multilruCount(multilruGetRawP(mlru)));
    }
    multilruGetStats(multilruGetRawP(mlru), &stats);
    const uint64_t slots = stats.nextFresh;
    cleanerName(zfree, MultilruPtrs) multilruPtr *levelBefore =
        zcalloc(slots, sizeof(multilruPtr));
    for (uint64_t h = 1; h < slots; h++) {
        levelBefore[h] =
            multilruIsPopulatedP(mlru, h) ? 1 + multilruGetLevelP(mlru, h) : 0;
    }
    const multilruPtr nextBefore =
        multilruPeekNextHandle(multilruGetRawP(mlru));
    multilruCloseP(mlru);
    persistCtxFree(ctx);

    ctx = persistCtxOpen(basePath, NULL);
    mlru = multilruOpenP(ctx);
    if (!mlru) {
        ERRR("Failed to reopen multilruP");
    } else {
        for (uint64_t h = 1; h < slots; h++) {
            const multilruPtr levelAfter = multilruIsPopulatedP(mlru, h)
                                               ? 1 + multilruGetLevelP(mlru, h)
                                               : 0;
            if (levelAfter != levelBefore[h]) {
                ERR("handle %" PRIu64 ": level %" PRIu64 " became %" PRIu64, h,
                    (uint64_t)levelBefore[h], (uint64_t)levelAfter);
            }
        }
        if (multilruPeekNextHandle(multilruGetRawP(mlru)) != nextBefore) {
            ERRR("The next handle changed across reopen");
        }
        if (multilruGetMaxCountP(mlru) != 5) {
            ERR("maxCount %" PRIu64 " after reopen, want 5",
                multilruGetMaxCountP(mlru));
        }
        multilruCloseP(mlru);
    }
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

static int test_multilruP_weight_limit(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multilruPTest_weightLimit", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multilruP evicts at once when a weight update passes the limit")

    ptestCleanupFiles(basePath);
    persistCtx *ctx = persistCtxNew(basePath, NULL);
    const multilruConfig config = {.maxLevels = 4,
                                   .maxWeight = 100,
                                   .policy = MLRU_POLICY_SIZE,
                                   .enableWeights = true};
    multilruP *mlru = multilruNewWithConfigP(ctx, &config);
    multilruPtr h[3];
    for (size_t i = 0; i < 3; i++) {
        h[i] = mlruTestInsert(mlru, 30);
    }

    /* The readers agree with the LRU they wrap */
    multilru *raw = multilruGetRawP(mlru);
    if (multilruGetMaxWeightP(mlru) != 100 ||
        multilruBytesP(mlru) != multilruBytes(raw) ||
        multilruLevelCountP(mlru, 0) != multilruLevelCount(raw, 0) ||
        multilruLevelCountP(mlru, 0) != 3 ||
        multilruLevelWeightP(mlru, 0) != 90 || !multilruSyncP(mlru)) {
        ERRR("a multilruP reader disagrees with the LRU");
    }
    persistCtxStats stats;
    multilruGetStatsP(mlru, &stats);
    if (stats.totalOps != 3) {
        ERR("stats report %" PRIu64 " ops for 3 inserts", stats.totalOps);
    }

    /* An unweighted insert, and a raised weight limit that evicts nothing */
    multilruPtr extra = 0;
    if (!multilruInsertP(mlru, &extra) || !extra ||
        multilruSetMaxWeightP(mlru, 1000) != PERSIST_LOG_DURABLE ||
        multilruGetMaxWeightP(mlru) != 1000 || multilruCountP(mlru) != 4) {
        ERRR("InsertP or SetMaxWeightP misbehaved");
    }
    PTEST_LOGGED(multilruDeleteP(mlru, extra));
    PTEST_LOGGED(multilruSetMaxWeightP(mlru, 100));
    if (multilruUpdateWeightP(mlru, h[2], 30) != PERSIST_LOG_UNCHANGED ||
        multilruIncreaseP(mlru, 9999) != PERSIST_LOG_UNCHANGED ||
        multilruDeleteP(mlru, 9999) != PERSIST_LOG_UNCHANGED) {
        ERRR("a change to nothing was logged");
    }

    /* 30 + 30 + 80 = 140 > 100: eviction runs now, not at the next insert */
    if (!multilruUpdateWeightP(mlru, h[2], 80) ||
        multilruTotalWeightP(mlru) > 100) {
        ERR("weight %" PRIu64 " left above the limit",
            multilruTotalWeightP(mlru));
    }
    const size_t count = multilruCountP(mlru);
    const uint64_t weight = multilruTotalWeightP(mlru);
    multilruCloseP(mlru);
    persistCtxFree(ctx);

    ctx = persistCtxOpen(basePath, NULL);
    mlru = multilruOpenP(ctx);
    if (!mlru || multilruCountP(mlru) != count ||
        multilruTotalWeightP(mlru) != weight) {
        ERRR("the eviction did not survive reopen");
    }
    multilruCloseP(mlru);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

static int test_multilruP_recency_order(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;

    TEST("a restored LRU demotes and evicts in the live one's order")

    for (size_t levels = 1; levels <= 4; levels++) {
        const multilruConfig config = {.maxLevels = levels,
                                       .policy = MLRU_POLICY_COUNT,
                                       .enableWeights = levels % 2 == 0};
        cleaner(multilruFree) multilru *live = multilruNewWithConfig(&config);

        /* Entries touched in a scrambled order, some deleted (holes) */
        multilruPtr handles[200];
        for (size_t i = 0; i < 200; i++) {
            handles[i] = config.enableWeights
                             ? multilruInsertWeighted(live, i * 3 + 1)
                             : multilruInsert(live);
        }
        uint64_t rng = 0xa11ce + levels;
        for (size_t i = 0; i < 600; i++) {
            const multilruPtr h = handles[splitmix64(&rng) % 200];
            if (multilruIsPopulated(live, h)) {
                multilruIncrease(live, h);
            }
        }
        for (size_t i = 0; i < 200; i += 7) {
            multilruDelete(live, handles[i]);
        }

        size_t len = 0;
        cleanerName(zfree, SnapBytes) uint8_t *snap =
            persistOpsMultiLRU.snapshot(live, &len);
        cleaner(multilruFree) multilru *back =
            snap ? persistOpsMultiLRU.restore(snap, len) : NULL;
        size_t againLen = 0;
        cleanerName(zfree, SnapBytes) uint8_t *again =
            back ? persistOpsMultiLRU.snapshot(back, &againLen) : NULL;
        if (!back || againLen != len || memcmp(snap, again, len) != 0) {
            ERR("levels %zu: restore did not reproduce the snapshot", levels);
        }

        /* Draining both gives the same steps: who, and demoted or gone */
        size_t steps = 0;
        multilruPtr a;
        multilruPtr b;
        while (back && multilruRemoveMinimum(live, &a)) {
            if (!multilruRemoveMinimum(back, &b) || a != b ||
                multilruIsPopulated(live, a) != multilruIsPopulated(back, b)) {
                ERR("levels %zu: step %zu took %zu, live took %zu", levels,
                    steps, b, a);
                break;
            }
            steps++;
        }
        if (back && multilruCount(back) != 0) {
            ERR("levels %zu: restored LRU outlived the live one", levels);
        }
    }

    TEST_FINAL_RESULT;
}

static int test_multilruP_damagedWal(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multilruPTest_damagedWal", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multilruP after a damaged WAL entry: healed, logging goes on")

    /* Three inserts with the last entry damaged: lenient recovery keeps
     * the first two (under their handles) and heals the WAL (the damaged
     * entry moves to the quarantine); an insert and a promote are then
     * logged with no compaction, and a reopen holds three entries */
    ptestCleanupFiles(basePath);
    persistCtx *ctx = persistCtxNew(basePath, NULL);
    multilruP *mlru = multilruNewWithLevelsP(ctx, 7, 100);
    const multilruPtr h1 = mlruTestInsert(mlru, 0);
    mlruTestInsert(mlru, 0);
    mlruTestInsert(mlru, 0);
    multilruCloseP(mlru);
    persistCtxFree(ctx);
    if (!ptestDamageLastWalEntry(basePath)) {
        ERRR("could not damage the WAL");
    }

    ctx = persistCtxOpen(basePath, NULL);
    mlru = multilruOpenP(ctx);
    if (!mlru || multilruCountP(mlru) != 2) {
        ERR("lenient recovery kept %zu entries, want 2",
            mlru ? multilruCountP(mlru) : (size_t)0);
    }
    if (!ptestExpectHealed(ctx, basePath)) {
        ERRR("the damaged entry was not healed into the quarantine");
    }
    multilruPtr handle = 0;
    if (mlru &&
        (multilruInsertP(mlru, &handle) == PERSIST_LOG_REFUSED || !handle ||
         multilruIncreaseP(mlru, h1) == PERSIST_LOG_REFUSED)) {
        ERRR("a change after the healed entry was refused");
    }
    multilruCloseP(mlru);
    persistCtxFree(ctx);

    ctx = persistCtxOpen(basePath, NULL);
    mlru = multilruOpenP(ctx);
    if (!mlru || multilruCountP(mlru) != 3) {
        ERR("reopen holds %zu entries, want 3",
            mlru ? multilruCountP(mlru) : (size_t)0);
    }
    multilruCloseP(mlru);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

/* Under a refusing WAL every change is refused and the LRU is as it was
 * (count, weights, each entry's level, the limits); calls missing their
 * wrapper are refused or answer empty */
static int test_multilruP_refusals(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multilruPTest_refusals", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multilruP refusals change nothing: a refused log, missing "
         "arguments")

    ptestCleanupFiles(basePath);
    persistCtx *ctx = persistCtxNew(basePath, NULL);
    const multilruConfig config = {.maxLevels = 4, .enableWeights = true};
    multilruP *mlru = multilruNewWithConfigP(ctx, &config);
    multilruPtr a = 0;
    multilruPtr b = 0;
    PTEST_LOGGED(multilruInsertWeightedP(mlru, 5, &a));
    PTEST_LOGGED(multilruInsertWeightedP(mlru, 7, &b));
    PTEST_LOGGED(multilruIncreaseP(mlru, b));
    const size_t levelA = multilruGetLevelP(mlru, a);
    const size_t levelB = multilruGetLevelP(mlru, b);
    const uint64_t maxCount = multilruGetMaxCountP(mlru);
    const uint64_t maxWeight = multilruGetMaxWeightP(mlru);

    multilruPtr fresh = 0;
    multilruPtr out = 0;
    PTEST_EXPECT_REFUSED(
        ctx, multilruInsertP(mlru, &fresh),
        multilruInsertWeightedP(mlru, 3, &fresh), multilruIncreaseP(mlru, a),
        multilruUpdateWeightP(mlru, a, 50), multilruRemoveMinimumP(mlru, &out),
        multilruDeleteP(mlru, b), multilruSetMaxCountP(mlru, 1),
        multilruSetMaxWeightP(mlru, 1));
    if (multilruCountP(mlru) != 2 || multilruTotalWeightP(mlru) != 12 ||
        multilruGetWeightP(mlru, a) != 5 || fresh != 0 || out != 0) {
        ERR("refused changes left count %zu, weight %" PRIu64
            ", a's weight %" PRIu64 ", handles %" PRIu64 " %" PRIu64,
            multilruCountP(mlru), multilruTotalWeightP(mlru),
            multilruGetWeightP(mlru, a), (uint64_t)fresh, (uint64_t)out);
    }
    if (multilruGetLevelP(mlru, a) != levelA ||
        multilruGetLevelP(mlru, b) != levelB) {
        ERR("refused changes moved levels %zu %zu to %zu %zu", levelA, levelB,
            multilruGetLevelP(mlru, a), multilruGetLevelP(mlru, b));
    }
    if (multilruGetMaxCountP(mlru) != maxCount ||
        multilruGetMaxWeightP(mlru) != maxWeight) {
        ERR("refused limit changes left limits %" PRIu64 " %" PRIu64,
            multilruGetMaxCountP(mlru), multilruGetMaxWeightP(mlru));
    }

    /* Missing arguments: refused or empty answers */
    if (multilruInsertP(NULL, &fresh) || multilruIncreaseP(NULL, a) ||
        multilruUpdateWeightP(NULL, a, 1) ||
        multilruRemoveMinimumP(NULL, &out) || multilruDeleteP(NULL, a) ||
        multilruSetMaxCountP(NULL, 1) || multilruSetMaxWeightP(NULL, 1) ||
        multilruNewP(NULL) || multilruOpenP(NULL) || multilruGetRawP(NULL) ||
        multilruCountP(NULL) || multilruBytesP(NULL) ||
        multilruTotalWeightP(NULL) || multilruIsPopulatedP(NULL, a) ||
        multilruSyncP(NULL) || multilruCompactP(NULL) ||
        multilruGetMaxCountP(NULL) || multilruGetMaxWeightP(NULL)) {
        ERRR("a call without its wrapper was not refused");
    }
    multilruGetStatsP(NULL, NULL);
    multilruCloseP(NULL);

    multilruCloseP(mlru);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

int multilruPTest(int argc, char *argv[]) {
    int err = 0;

    err += test_multilruP_refusals(argc, argv);

    err += test_multilruP_damagedWal(argc, argv);
    err += test_multilruP_recency_order(argc, argv);
    err += test_multilruP_weight_limit(argc, argv);
    err += test_multilruP_demotions_replay(argc, argv);

    /* Basic functionality tests */
    err += test_multilruP_basic(argc, argv);
    err += test_multilruP_weighted(argc, argv);
    err += test_multilruP_compaction(argc, argv);

    /* Edge case tests */
    err += test_multilruP_empty_recovery(argc, argv);
    err += test_multilruP_large_dataset(argc, argv);
    err += test_multilruP_handle_gaps(argc, argv);
    err += test_multilruP_weight_extremes(argc, argv);
    err += test_multilruP_auto_evict(argc, argv);
    err += test_multilruP_auto_evict_crash_before_compact(argc, argv);
    err += test_multilruP_level_promotion(argc, argv);

    /* Stress tests */
    err += test_multilruP_mixed_operations(argc, argv);

    return err;
}

#endif /* DATAKIT_TEST */
