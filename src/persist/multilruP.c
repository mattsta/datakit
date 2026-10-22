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

/* Log, then take, one eviction step; false when there is nothing to take
 * or the WAL write fails (then nothing changes) */
static bool multilruPEvictStep(multilruP *mlru, multilruPtr *victimOut) {
    multilruPtr victim = multilruPeekMinimum(mlru->mlru);
    if (!victim) {
        return false;
    }
    const persistOp op = multilruGetLevel(mlru->mlru, victim) > 0
                             ? PERSIST_OP_NUM_DECR_F0
                             : PERSIST_OP_DELETE;
    const void *args[1] = {&victim};
    if (!persistCtxLogOp(mlru->ctx, op, args, 1)) {
        return false;
    }
    multilruPtr taken;
    multilruRemoveMinimum(mlru->mlru, &taken);
    assert(taken == victim);
    if (victimOut) {
        *victimOut = victim;
    }
    return true;
}

static void multilruPEnforceLimits(multilruP *mlru) {
    while (multilruNeedsEviction(mlru->mlru) &&
           multilruPEvictStep(mlru, NULL)) {
    }
}

/* Insert with every step logged before it is taken: the handle the insert
 * will get, then the eviction steps the limits require.  0 when the WAL
 * write fails (nothing changes) or the new entry was itself evicted. */
static multilruPtr multilruPInsert_(multilruP *mlru, uint64_t weight) {
    if (!mlru || !mlru->mlru || !mlru->ctx) {
        return 0;
    }

    multilruPtr handle = multilruPeekNextHandle(mlru->mlru);
    const void *args[2] = {&handle, &weight};
    if (!persistCtxLogOp(mlru->ctx, PERSIST_OP_INSERT, args, 2)) {
        return 0;
    }
    const multilruPtr got = multilruInsertWeighted(mlru->mlru, weight);
    assert(got == handle);
    (void)got;

    multilruPEnforceLimits(mlru);
    persistCtxMaybeCompact(mlru->ctx, mlru->mlru, &persistOpsMultiLRU);
    return multilruIsPopulated(mlru->mlru, handle) ? handle : 0;
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
    return mlru;
}

void multilruCloseP(multilruP *mlru) {
    if (!mlru) {
        return;
    }

    /* Sync before closing */
    if (mlru->ctx) {
        persistCtxSync(mlru->ctx);
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

multilruPtr multilruInsertP(multilruP *mlru) {
    return multilruPInsert_(mlru, 0);
}

multilruPtr multilruInsertWeightedP(multilruP *mlru, uint64_t weight) {
    return multilruPInsert_(mlru, weight);
}

void multilruIncreaseP(multilruP *mlru, multilruPtr ptr) {
    if (!mlru || !mlru->mlru || !mlru->ctx || ptr == 0) {
        return;
    }

    /* Log to WAL first: [handle] using PERSIST_OP_CUSTOM for PROMOTE */
    const void *args[1] = {&ptr};
    if (!persistCtxLogOp(mlru->ctx, PERSIST_OP_CUSTOM, args, 1)) {
        return;
    }

    /* Update in-memory */
    multilruIncrease(mlru->mlru, ptr);

    /* Check compaction threshold */
    persistCtxMaybeCompact(mlru->ctx, mlru->mlru, &persistOpsMultiLRU);
}

void multilruUpdateWeightP(multilruP *mlru, multilruPtr ptr,
                           uint64_t newWeight) {
    if (!mlru || !mlru->mlru || !mlru->ctx || ptr == 0) {
        return;
    }

    /* Log to WAL first: [handle][newWeight] */
    const void *args[2] = {&ptr, &newWeight};
    if (!persistCtxLogOp(mlru->ctx, PERSIST_OP_UPDATE, args, 2)) {
        return;
    }

    /* Update in-memory */
    multilruUpdateWeight(mlru->mlru, ptr, newWeight);

    /* Check compaction threshold */
    persistCtxMaybeCompact(mlru->ctx, mlru->mlru, &persistOpsMultiLRU);
}

bool multilruRemoveMinimumP(multilruP *mlru, multilruPtr *out) {
    if (!mlru || !mlru->mlru || !mlru->ctx) {
        return false;
    }

    /* One step, logged first: a demotion above level 0, an eviction at
     * level 0 (multilruIsPopulated afterwards tells which) */
    if (!multilruPEvictStep(mlru, out)) {
        return false;
    }
    persistCtxMaybeCompact(mlru->ctx, mlru->mlru, &persistOpsMultiLRU);
    return true;
}

void multilruDeleteP(multilruP *mlru, multilruPtr ptr) {
    if (!mlru || !mlru->mlru || !mlru->ctx || ptr == 0) {
        return;
    }

    /* Log to WAL first */
    const void *args[1] = {&ptr};
    if (!persistCtxLogOp(mlru->ctx, PERSIST_OP_DELETE, args, 1)) {
        return;
    }

    /* Update in-memory */
    multilruDelete(mlru->mlru, ptr);

    /* Check compaction threshold */
    persistCtxMaybeCompact(mlru->ctx, mlru->mlru, &persistOpsMultiLRU);
}

/* ============================================================================
 * Configuration
 * ============================================================================
 */

void multilruSetMaxCountP(multilruP *mlru, uint64_t maxCount) {
    if (!mlru || !mlru->mlru) {
        return;
    }
    multilruSetMaxCount(mlru->mlru, maxCount);
    /* The limits live in the snapshot: write one now so the change is
     * durable (limits change rarely; eviction to them happens on the
     * next insert, logged step by step) */
    if (mlru->ctx) {
        persistCtxCompact(mlru->ctx, mlru->mlru, &persistOpsMultiLRU);
    }
}

uint64_t multilruGetMaxCountP(const multilruP *mlru) {
    return mlru && mlru->mlru ? multilruGetMaxCount(mlru->mlru) : 0;
}

void multilruSetMaxWeightP(multilruP *mlru, uint64_t maxWeight) {
    if (!mlru || !mlru->mlru) {
        return;
    }
    multilruSetMaxWeight(mlru->mlru, maxWeight);
    /* Durable at once, as for the count limit */
    if (mlru->ctx) {
        persistCtxCompact(mlru->ctx, mlru->mlru, &persistOpsMultiLRU);
    }
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
#include "persistTestCommon.h"

static int test_multilruP_basic(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {{0}};
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
    multilruPtr h1 = multilruInsertP(mlru);
    multilruPtr h2 = multilruInsertP(mlru);
    multilruPtr h3 = multilruInsertP(mlru);

    if (h1 == 0 || h2 == 0 || h3 == 0) {
        ERRR("Failed to insert entries");
    }

    /* Verify count */
    if (multilruCountP(mlru) != 3) {
        ERR("Count should be 3, got %zu", multilruCountP(mlru));
    }

    /* Promote h1 to level 1 */
    multilruIncreaseP(mlru, h1);
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
    multilruDeleteP(mlru, h2);
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
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {{0}};
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
    multilruPtr h1 = multilruInsertWeightedP(mlru, 100);
    multilruPtr h2 = multilruInsertWeightedP(mlru, 200);
    multilruInsertWeightedP(mlru, 150); /* h3 */

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
    multilruUpdateWeightP(mlru, h1, 300);
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
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {{0}};
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
        handles[i] = multilruInsertP(mlru);
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
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {{0}};
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
    multilruPtr h1 = multilruInsertP(mlru);
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
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {{0}};
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
        handles[i] = multilruInsertP(mlru);
        if (handles[i] == 0) {
            ERR("Failed to insert entry %zu", i);
        }
    }

    /* Promote random entries to various levels */
    for (size_t i = 0; i < numEntries; i += 7) {
        for (int promo = 0; promo < 3; promo++) {
            multilruIncreaseP(mlru, handles[i]);
        }
    }

    /* Delete some entries - save their handles for verification */
    cleanerName(zfree, MultilruPtrs) multilruPtr *deletedHandles =
        zmalloc(100 * sizeof(multilruPtr));
    for (size_t i = 100; i < 200; i++) {
        deletedHandles[i - 100] = handles[i];
        multilruDeleteP(mlru, handles[i]);
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
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {{0}};
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
        handles[i] = multilruInsertP(mlru);
    }

    /* Delete every other entry to create gaps */
    for (int i = 0; i < 10; i += 2) {
        multilruDeleteP(mlru, handles[i]);
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
    multilruPtr newHandle = multilruInsertP(mlru);
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
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {{0}};
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
            multilruPtr h = multilruInsertWeightedP(mlru, weight);
            if (h != 0) {
                handles[handleCount++] = h;
            }
        } else if (choice < 60 && handleCount > 0) {
            /* DELETE (20%) */
            size_t idx = xorshift % handleCount;
            multilruDeleteP(mlru, handles[idx]);
            handles[idx] = handles[--handleCount];
        } else if (choice < 80 && handleCount > 0) {
            /* PROMOTE (20%) */
            size_t idx = xorshift % handleCount;
            multilruIncreaseP(mlru, handles[idx]);
        } else if (choice < 95 && handleCount > 0) {
            /* UPDATE WEIGHT (15%) */
            size_t idx = xorshift % handleCount;
            uint64_t newWeight = (xorshift % 2000) + 1;
            multilruUpdateWeightP(mlru, handles[idx], newWeight);
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
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {{0}};
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
    multilruPtr h0 = multilruInsertWeightedP(mlru, 0);
    if (multilruGetWeightP(mlru, h0) != 0) {
        ERR("Zero weight should be preserved, got %" PRIu64,
            multilruGetWeightP(mlru, h0));
    }

    /* Test max weight */
    multilruPtr hMax = multilruInsertWeightedP(mlru, UINT64_MAX);
    if (multilruGetWeightP(mlru, hMax) != UINT64_MAX) {
        ERR("Max weight should be preserved, got %" PRIu64,
            multilruGetWeightP(mlru, hMax));
    }

    /* Test updating to extremes */
    multilruPtr h1 = multilruInsertWeightedP(mlru, 500);
    multilruUpdateWeightP(mlru, h1, 0);
    if (multilruGetWeightP(mlru, h1) != 0) {
        ERR("Updated weight should be 0, got %" PRIu64,
            multilruGetWeightP(mlru, h1));
    }

    multilruUpdateWeightP(mlru, h1, UINT64_MAX);
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
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {{0}};
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
        handles[i] = multilruInsertP(mlru);
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
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {{0}};
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
        handles[i] = multilruInsertP(mlru);
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
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {{0}};
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
    multilruPtr h1 = multilruInsertP(mlru);
    if (multilruGetLevelP(mlru, h1) != 0) {
        ERR("New entry should be at level 0, got %zu",
            multilruGetLevelP(mlru, h1));
    }

    /* Promote to each level */
    for (size_t targetLevel = 1; targetLevel <= 6; targetLevel++) {
        multilruIncreaseP(mlru, h1);
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
    multilruIncreaseP(mlru, h1);
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
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {{0}};
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
    multilruPtr live[64] = {0};
    size_t liveCount = 0;
    uint64_t rng = 0x1234;
    for (int i = 0; i < 60; i++) {
        const multilruPtr h = multilruInsertP(mlru);
        if (h && liveCount < 64) {
            live[liveCount++] = h;
        }
        for (int p = 0; p < 3 && liveCount; p++) {
            rng = rng * 6364136223846793005ULL + 1;
            multilruIncreaseP(mlru, live[(rng >> 33) % liveCount]);
        }
    }

    /* Empty level 0 by promoting every entry, so the next removals of the
     * minimum demote instead of evicting */
    for (size_t i = 0; i < liveCount; i++) {
        if (multilruIsPopulatedP(mlru, live[i]) &&
            multilruGetLevelP(mlru, live[i]) == 0) {
            multilruIncreaseP(mlru, live[i]);
        }
    }
    for (int i = 0; i < 4; i++) {
        multilruPtr victim;
        multilruRemoveMinimumP(mlru, &victim);
    }

    multilruStats stats;
    multilruGetStats(multilruGetRawP(mlru), &stats);
    if (stats.demotions == 0) {
        ERRR("The workload never demoted (test does not cover demotion)");
    }
    const uint64_t slots = stats.nextFresh;
    cleanerName(zfree, MultilruPtrs) multilruPtr *levelBefore =
        zcalloc(slots, sizeof(multilruPtr));
    for (uint64_t h = 1; h < slots; h++) {
        levelBefore[h] =
            multilruIsPopulatedP(mlru, h) ? 1 + multilruGetLevelP(mlru, h) : 0;
    }
    const multilruPtr nextBefore =
        multilruPeekNextHandle(multilruGetRawP(mlru));
    multilruSetMaxCountP(mlru, 5);
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

int multilruPTest(int argc, char *argv[]) {
    int err = 0;

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
