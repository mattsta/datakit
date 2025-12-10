/* multilruP.c - Persistent multilru wrapper
 *
 * See multilruP.h for API documentation.
 */

#include "multilruP.h"
#include "../datakit.h"
#include "../persist.h"

#include <string.h>
#include <unistd.h>

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

multilruP *multilruNewP(persistCtx *ctx) {
    if (!ctx) {
        return NULL;
    }

    multilruP *mlru = zcalloc(1, sizeof(*mlru));
    mlru->ctx = ctx;
    mlru->enableWeights = false; /* Default: no weights */

    /* Create underlying multilru */
    mlru->mlru = multilruNew();
    if (!mlru->mlru) {
        zfree(mlru);
        return NULL;
    }

    /* Initialize persistence context for multilru type */
    if (!persistCtxInitForType(ctx, &persistOpsMultiLRU)) {
        multilruFree(mlru->mlru);
        zfree(mlru);
        return NULL;
    }

    /* Don't save initial snapshot for empty multilru - the enableWeights
     * detection would fail since there are no entries yet. Instead, the first
     * mutation will trigger compaction which will save the snapshot with
     * correct config. */

    return mlru;
}

multilruP *multilruNewWithLevelsP(persistCtx *ctx, size_t maxLevels,
                                  uint64_t maxCount) {
    if (!ctx) {
        return NULL;
    }

    multilruP *mlru = zcalloc(1, sizeof(*mlru));
    mlru->ctx = ctx;
    mlru->enableWeights = false; /* No weights for this API */

    /* Create multilru with config */
    multilruConfig config = {
        .maxLevels = maxLevels,
        .startCapacity = 0,
        .maxWeight = 0,
        .maxCount = maxCount,
        .policy = MLRU_POLICY_COUNT,
        .evictStrategy = MLRU_EVICT_LRU,
        .enableWeights = false,
    };
    mlru->mlru = multilruNewWithConfig(&config);
    if (!mlru->mlru) {
        zfree(mlru);
        return NULL;
    }

    /* Initialize persistence */
    if (!persistCtxInitForType(ctx, &persistOpsMultiLRU)) {
        multilruFree(mlru->mlru);
        zfree(mlru);
        return NULL;
    }

    /* Save initial snapshot (now that we have proper enableWeights detection)
     */
    if (!persistCtxSaveSnapshot(ctx, mlru->mlru, &persistOpsMultiLRU)) {
        multilruFree(mlru->mlru);
        zfree(mlru);
        return NULL;
    }

    return mlru;
}

multilruP *multilruNewWithConfigP(persistCtx *ctx,
                                  const multilruConfig *config) {
    if (!ctx || !config) {
        return NULL;
    }

    multilruP *mlru = zcalloc(1, sizeof(*mlru));
    mlru->ctx = ctx;
    mlru->enableWeights = config->enableWeights; /* Store for snapshot */

    /* Create multilru with config */
    mlru->mlru = multilruNewWithConfig(config);
    if (!mlru->mlru) {
        zfree(mlru);
        return NULL;
    }

    /* Initialize persistence */
    if (!persistCtxInitForType(ctx, &persistOpsMultiLRU)) {
        multilruFree(mlru->mlru);
        zfree(mlru);
        return NULL;
    }

    /* Save initial snapshot (now that we have proper enableWeights detection)
     */
    if (!persistCtxSaveSnapshot(ctx, mlru->mlru, &persistOpsMultiLRU)) {
        multilruFree(mlru->mlru);
        zfree(mlru);
        return NULL;
    }

    return mlru;
}

multilruP *multilruOpenP(persistCtx *ctx) {
    if (!ctx) {
        return NULL;
    }

    /* Initialize persistence context for multilru type */
    if (!persistCtxInitForType(ctx, &persistOpsMultiLRU)) {
        return NULL;
    }

    /* Recover from snapshot + WAL */
    multilru *recovered = persistCtxRecover(ctx, &persistOpsMultiLRU);
    if (!recovered) {
        recovered = multilruNew();
    }

    multilruP *mlru = zcalloc(1, sizeof(*mlru));
    mlru->ctx = ctx;
    mlru->mlru = recovered;

    /* Detect if weights are enabled from the recovered multilru
     * We can check by seeing if totalWeight tracking works */
    multilruStats stats;
    multilruGetStats(recovered, &stats);
    mlru->enableWeights = (stats.totalWeight > 0);
    if (!mlru->enableWeights) {
        /* Check if any entry has weight */
        for (size_t h = 1; h < stats.nextFresh; h++) {
            if (multilruIsPopulated(recovered, h) &&
                multilruGetWeight(recovered, h) > 0) {
                mlru->enableWeights = true;
                break;
            }
        }
    }

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
    if (!mlru || !mlru->mlru || !mlru->ctx) {
        return 0;
    }

    /* Insert in-memory first to get handle */
    multilruPtr handle = multilruInsert(mlru->mlru);
    if (handle == 0) {
        return 0;
    }

    /* Log to WAL: [handle][weight=0] */
    uint64_t weight = 0;
    const void *args[2] = {&handle, &weight};
    if (!persistCtxLogOp(mlru->ctx, PERSIST_OP_INSERT, args, 2)) {
        /* WAL failed - remove the entry */
        multilruDelete(mlru->mlru, handle);
        return 0;
    }

    /* Check compaction threshold */
    persistCtxMaybeCompact(mlru->ctx, mlru->mlru, &persistOpsMultiLRU);

    return handle;
}

multilruPtr multilruInsertWeightedP(multilruP *mlru, uint64_t weight) {
    if (!mlru || !mlru->mlru || !mlru->ctx) {
        return 0;
    }

    /* Insert in-memory first to get handle */
    multilruPtr handle = multilruInsertWeighted(mlru->mlru, weight);
    if (handle == 0) {
        return 0;
    }

    /* Log to WAL: [handle][weight] */
    const void *args[2] = {&handle, &weight};
    if (!persistCtxLogOp(mlru->ctx, PERSIST_OP_INSERT, args, 2)) {
        /* WAL failed - remove the entry */
        multilruDelete(mlru->mlru, handle);
        return 0;
    }

    /* Check compaction threshold */
    persistCtxMaybeCompact(mlru->ctx, mlru->mlru, &persistOpsMultiLRU);

    return handle;
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

    /* Note: RemoveMinimum may demote or evict. Only log true evictions.
     * We'll check if the entry was at level 0 before removal */
    multilruPtr evicted;
    if (!multilruRemoveMinimum(mlru->mlru, &evicted)) {
        return false;
    }

    if (out) {
        *out = evicted;
    }

    /* Log deletion to WAL if this was a true eviction (not just demotion)
     * We can tell by checking if the entry still exists */
    if (!multilruIsPopulated(mlru->mlru, evicted)) {
        /* True eviction - log to WAL */
        const void *args[1] = {&evicted};
        persistCtxLogOp(mlru->ctx, PERSIST_OP_DELETE, args, 1);

        /* Check compaction threshold */
        persistCtxMaybeCompact(mlru->ctx, mlru->mlru, &persistOpsMultiLRU);
    }

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
    /* Config changes are persisted via next snapshot */
}

uint64_t multilruGetMaxCountP(const multilruP *mlru) {
    return mlru && mlru->mlru ? multilruGetMaxCount(mlru->mlru) : 0;
}

void multilruSetMaxWeightP(multilruP *mlru, uint64_t maxWeight) {
    if (!mlru || !mlru->mlru) {
        return;
    }
    multilruSetMaxWeight(mlru->mlru, maxWeight);
    /* Config changes are persisted via next snapshot */
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
    const char *basePath = "/tmp/multilruPTest_basic";

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
    const char *basePath = "/tmp/multilruPTest_weighted";

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
    const char *basePath = "/tmp/multilruPTest_compaction";

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
    const char *basePath = "/tmp/multilruPTest_empty";

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
    const char *basePath = "/tmp/multilruPTest_large";

    TEST("multilruP large dataset")

    ptestCleanupFiles(basePath);

    persistCtx *ctx = persistCtxNew(basePath, NULL);
    multilruP *mlru = multilruNewWithLevelsP(ctx, 7, 5000);
    if (!mlru) {
        ERRR("Failed to create multilruP");
    }

    /* Insert 2000 entries */
    const size_t numEntries = 2000;
    multilruPtr *handles = zmalloc(numEntries * sizeof(multilruPtr));
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
    multilruPtr *deletedHandles = zmalloc(100 * sizeof(multilruPtr));
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

    zfree(deletedHandles);

    /* Cleanup */
    zfree(handles);
    multilruCloseP(mlru);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

static int test_multilruP_handle_gaps(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    const char *basePath = "/tmp/multilruPTest_gaps";

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
    const char *basePath = "/tmp/multilruPTest_mixed";

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
    const char *basePath = "/tmp/multilruPTest_weights";

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
    const char *basePath = "/tmp/multilruPTest_evict";

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

static int test_multilruP_level_promotion(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    const char *basePath = "/tmp/multilruPTest_levels";

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

int multilruPTest(int argc, char *argv[]) {
    int err = 0;

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
    err += test_multilruP_level_promotion(argc, argv);

    /* Stress tests */
    err += test_multilruP_mixed_operations(argc, argv);

    return err;
}

#endif /* DATAKIT_TEST */
