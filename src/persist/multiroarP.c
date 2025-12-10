/* multiroarP.c - Persistent multiroar wrapper
 *
 * See multiroarP.h for API documentation.
 */

#include "multiroarP.h"
#include "../datakit.h"
#include "../persist.h"

#include <string.h>
#include <unistd.h>

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

multiroarP *multiroarNewP(persistCtx *ctx) {
    if (!ctx) {
        return NULL;
    }

    multiroarP *r = zcalloc(1, sizeof(*r));
    r->ctx = ctx;

    /* Create underlying multiroar */
    r->r = multiroarBitNew();
    if (!r->r) {
        zfree(r);
        return NULL;
    }

    /* Initialize persistence context for multiroar type */
    if (!persistCtxInitForType(ctx, &persistOpsMultiroar)) {
        multiroarFree(r->r);
        zfree(r);
        return NULL;
    }

    /* Save initial snapshot */
    if (!persistCtxSaveSnapshot(ctx, r->r, &persistOpsMultiroar)) {
        multiroarFree(r->r);
        zfree(r);
        return NULL;
    }

    return r;
}

multiroarP *multiroarOpenP(persistCtx *ctx) {
    if (!ctx) {
        return NULL;
    }

    /* Initialize persistence context for multiroar type */
    if (!persistCtxInitForType(ctx, &persistOpsMultiroar)) {
        return NULL;
    }

    /* Recover from snapshot + WAL */
    multiroar *recovered = persistCtxRecover(ctx, &persistOpsMultiroar);
    if (!recovered) {
        return NULL;
    }

    multiroarP *r = zcalloc(1, sizeof(*r));
    r->ctx = ctx;
    r->r = recovered;

    return r;
}

void multiroarCloseP(multiroarP *r) {
    if (!r) {
        return;
    }

    /* Sync before closing */
    if (r->ctx) {
        persistCtxSync(r->ctx);
    }

    /* Free underlying multiroar */
    if (r->r) {
        multiroarFree(r->r);
    }

    zfree(r);
}

multiroar *multiroarGetRawP(multiroarP *r) {
    return r ? r->r : NULL;
}

/* ============================================================================
 * Metadata
 * ============================================================================
 */

size_t multiroarBitCountP(const multiroarP *r) {
    return r && r->r ? multiroarBitCount(r->r) : 0;
}

size_t multiroarMemoryUsageP(const multiroarP *r) {
    return r && r->r ? multiroarMemoryUsage(r->r) : 0;
}

bool multiroarIsEmptyP(const multiroarP *r) {
    return !r || !r->r || multiroarIsEmpty(r->r);
}

/* ============================================================================
 * Mutations
 * ============================================================================
 */

bool multiroarBitSetP(multiroarP *r, size_t position) {
    if (!r || !r->r || !r->ctx) {
        return false;
    }

    /* Check if already set */
    bool wasSet = multiroarBitGet(r->r, position);

    /* Log to WAL first */
    uint64_t pos64 = position;
    if (!persistCtxLogOp(r->ctx, PERSIST_OP_INSERT, &pos64, 1)) {
        return false;
    }

    /* Update in-memory */
    multiroarBitSet(r->r, position);

    /* Check compaction threshold */
    persistCtxMaybeCompact(r->ctx, r->r, &persistOpsMultiroar);

    return !wasSet; /* Return true if bit was not previously set */
}

bool multiroarRemoveP(multiroarP *r, size_t position) {
    if (!r || !r->r || !r->ctx) {
        return false;
    }

    /* Check if currently set */
    bool wasSet = multiroarBitGet(r->r, position);

    /* Log to WAL first */
    uint64_t pos64 = position;
    if (!persistCtxLogOp(r->ctx, PERSIST_OP_DELETE, &pos64, 1)) {
        return false;
    }

    /* Update in-memory */
    multiroarRemove(r->r, position);

    /* Check compaction threshold */
    persistCtxMaybeCompact(r->ctx, r->r, &persistOpsMultiroar);

    return wasSet; /* Return true if bit was previously set */
}

void multiroarBitSetRangeP(multiroarP *r, size_t start, size_t extent) {
    if (!r || !r->r || !r->ctx) {
        return;
    }

    /* Log each bit individually (could be optimized with batch WAL operations)
     */
    for (size_t i = 0; i < extent; i++) {
        uint64_t pos = start + i;
        if (!persistCtxLogOp(r->ctx, PERSIST_OP_INSERT, &pos, 1)) {
            return;
        }
    }

    /* Update in-memory */
    multiroarBitSetRange(r->r, start, extent);

    /* Check compaction threshold */
    persistCtxMaybeCompact(r->ctx, r->r, &persistOpsMultiroar);
}

void multiroarBitClearRangeP(multiroarP *r, uint64_t start, uint64_t extent) {
    if (!r || !r->r || !r->ctx) {
        return;
    }

    /* Log each bit individually */
    for (uint64_t i = 0; i < extent; i++) {
        uint64_t pos = start + i;
        if (!persistCtxLogOp(r->ctx, PERSIST_OP_DELETE, &pos, 1)) {
            return;
        }
    }

    /* Update in-memory */
    multiroarBitClearRange(r->r, start, extent);

    /* Check compaction threshold */
    persistCtxMaybeCompact(r->ctx, r->r, &persistOpsMultiroar);
}

/* ============================================================================
 * Lookups
 * ============================================================================
 */

bool multiroarBitGetP(const multiroarP *r, size_t position) {
    if (!r || !r->r) {
        return false;
    }
    return multiroarBitGet(r->r, position);
}

bool multiroarMinP(const multiroarP *r, uint64_t *position) {
    if (!r || !r->r || !position) {
        return false;
    }
    return multiroarMin(r->r, position);
}

bool multiroarMaxP(const multiroarP *r, uint64_t *position) {
    if (!r || !r->r || !position) {
        return false;
    }
    return multiroarMax(r->r, position);
}

size_t multiroarRankP(const multiroarP *r, uint64_t position) {
    if (!r || !r->r) {
        return 0;
    }
    return multiroarRank(r->r, position);
}

bool multiroarSelectP(const multiroarP *r, size_t k, uint64_t *position) {
    if (!r || !r->r || !position) {
        return false;
    }
    return multiroarSelect(r->r, k, position);
}

/* ============================================================================
 * Persistence Control
 * ============================================================================
 */

bool multiroarSyncP(multiroarP *r) {
    if (!r || !r->ctx) {
        return false;
    }
    return persistCtxSync(r->ctx);
}

bool multiroarCompactP(multiroarP *r) {
    if (!r || !r->ctx || !r->r) {
        return false;
    }
    return persistCtxCompact(r->ctx, r->r, &persistOpsMultiroar);
}

void multiroarGetStatsP(const multiroarP *r, persistCtxStats *stats) {
    if (!r || !r->ctx || !stats) {
        return;
    }
    persistCtxGetStats(r->ctx, stats);
}

/* ============================================================================
 * Tests
 * ============================================================================
 */

#ifdef DATAKIT_TEST
#include "persistTestCommon.h"

static int test_multiroarP_basic(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    const char *basePath = "/tmp/multiroarPTest_basic";

    TEST("multiroarP basic persistence")

    ptestCleanupFiles(basePath);

    /* Create new persistent multiroar */
    persistCtx *ctx = persistCtxNew(basePath, NULL);
    multiroarP *r = multiroarNewP(ctx);
    if (!r) {
        ERRR("Failed to create multiroarP");
    }

    /* Add some bits */
    if (!multiroarBitSetP(r, 100)) {
        ERRR("Failed to set bit 100");
    }
    if (!multiroarBitSetP(r, 200)) {
        ERRR("Failed to set bit 200");
    }
    if (!multiroarBitSetP(r, 8192)) {
        ERRR("Failed to set bit 8192");
    }

    /* Verify bits */
    if (!multiroarBitGetP(r, 100)) {
        ERRR("Bit 100 not set");
    }
    if (!multiroarBitGetP(r, 200)) {
        ERRR("Bit 200 not set");
    }
    if (!multiroarBitGetP(r, 8192)) {
        ERRR("Bit 8192 not set");
    }
    if (multiroarBitGetP(r, 150)) {
        ERRR("Bit 150 should not be set");
    }

    /* Check count */
    if (multiroarBitCountP(r) != 3) {
        ERR("Count should be 3, got %zu", multiroarBitCountP(r));
    }

    /* Close and reopen */
    multiroarCloseP(r);
    persistCtxFree(ctx);

    ctx = persistCtxNew(basePath, NULL);
    r = multiroarOpenP(ctx);
    if (!r) {
        ERRR("Failed to reopen multiroarP");
    }

    /* Verify bits after recovery */
    if (!multiroarBitGetP(r, 100)) {
        ERRR("Bit 100 not set after recovery");
    }
    if (!multiroarBitGetP(r, 200)) {
        ERRR("Bit 200 not set after recovery");
    }
    if (!multiroarBitGetP(r, 8192)) {
        ERRR("Bit 8192 not set after recovery");
    }
    if (multiroarBitCountP(r) != 3) {
        ERR("Count should be 3 after recovery, got %zu", multiroarBitCountP(r));
    }

    /* Remove a bit */
    if (!multiroarRemoveP(r, 200)) {
        ERRR("Failed to remove bit 200");
    }
    if (multiroarBitGetP(r, 200)) {
        ERRR("Bit 200 should be removed");
    }
    if (multiroarBitCountP(r) != 2) {
        ERR("Count should be 2, got %zu", multiroarBitCountP(r));
    }

    /* Close and reopen again */
    multiroarCloseP(r);
    persistCtxFree(ctx);

    ctx = persistCtxNew(basePath, NULL);
    r = multiroarOpenP(ctx);
    if (!r) {
        ERRR("Failed to reopen multiroarP");
    }

    /* Verify bits after second recovery */
    if (!multiroarBitGetP(r, 100)) {
        ERRR("Bit 100 not set after second recovery");
    }
    if (multiroarBitGetP(r, 200)) {
        ERRR("Bit 200 should be removed after recovery");
    }
    if (!multiroarBitGetP(r, 8192)) {
        ERRR("Bit 8192 not set after second recovery");
    }
    if (multiroarBitCountP(r) != 2) {
        ERR("Count should be 2 after second recovery, got %zu",
            multiroarBitCountP(r));
    }

    /* Cleanup */
    multiroarCloseP(r);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

static int test_multiroarP_recovery(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    const char *basePath = "/tmp/multiroarPTest_recovery";

    TEST("multiroarP recovery from crash")

    ptestCleanupFiles(basePath);

    /* Create multiroar and add bits */
    persistCtx *ctx = persistCtxNew(basePath, NULL);
    multiroarP *r = multiroarNewP(ctx);

    for (uint64_t i = 0; i < 100; i++) {
        multiroarBitSetP(r, i * 10);
    }

    /* Simulate crash - don't call multiroarCloseP */
    persistCtxFree(ctx);
    zfree(r);

    /* Recover */
    ctx = persistCtxNew(basePath, NULL);
    r = multiroarOpenP(ctx);
    if (!r) {
        ERRR("Failed to recover after crash");
    }

    /* Verify all bits */
    if (multiroarBitCountP(r) != 100) {
        ERR("Count should be 100 after recovery, got %zu",
            multiroarBitCountP(r));
    }

    for (uint64_t i = 0; i < 100; i++) {
        if (!multiroarBitGetP(r, i * 10)) {
            ERR("Bit %" PRIu64 " not set after recovery", i * 10);
        }
    }

    /* Cleanup */
    multiroarCloseP(r);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

static int test_multiroarP_compaction(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    const char *basePath = "/tmp/multiroarPTest_compaction";

    TEST("multiroarP compaction")

    ptestCleanupFiles(basePath);

    persistCtx *ctx = persistCtxNew(basePath, NULL);
    if (!ctx) {
        ERRR("Failed to create persistCtx");
    }

    multiroarP *r = multiroarNewP(ctx);
    if (!r) {
        ERRR("Failed to create multiroarP");
    }

    /* Add many bits */
    for (uint64_t i = 0; i < 50; i++) {
        multiroarBitSetP(r, i);
    }

    /* Attempt compaction */
    bool compacted = multiroarCompactP(r);

    if (!compacted) {
        ERRR("Compaction failed");
    }

    /* Verify data is intact */
    if (multiroarBitCountP(r) != 50) {
        ERR("Count should be 50, got %zu", multiroarBitCountP(r));
    }

    /* Verify all bits still set */
    for (uint64_t i = 0; i < 50; i++) {
        if (!multiroarBitGetP(r, i)) {
            ERR("Bit %" PRIu64 " should still be set", i);
        }
    }

    /* Cleanup */
    multiroarCloseP(r);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

static int test_multiroarP_fuzzing(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    const char *basePath = "/tmp/multiroarPTest_fuzzing";

    TEST("multiroarP fuzzing with random operations")

    ptestCleanupFiles(basePath);

    persistCtxConfig config = persistCtxDefaultConfig();
    config.compactAfterOps = 50;
    persistCtx *ctx = persistCtxNew(basePath, &config);
    multiroarP *r = multiroarNewP(ctx);

    /* Perform random operations using simple bit positions */
    for (int op = 0; op < 200; op++) {
        uint64_t pos = (uint64_t)op % 100;

        if (op % 2 == 0) {
            /* Insert */
            multiroarBitSetP(r, pos);
        } else {
            /* Delete */
            multiroarRemoveP(r, pos);
        }
    }

    /* Verify non-zero count */
    if (multiroarBitCountP(r) == 0) {
        ERRR("Expected non-zero count after operations");
    }

    /* Close and reopen */
    size_t beforeCount = multiroarBitCountP(r);
    multiroarCloseP(r);
    persistCtxFree(ctx);

    ctx = persistCtxNew(basePath, NULL);
    r = multiroarOpenP(ctx);

    /* Verify count matches after recovery */
    if (multiroarBitCountP(r) != beforeCount) {
        ERR("Count mismatch after recovery: before=%zu, after=%zu", beforeCount,
            multiroarBitCountP(r));
    }

    /* Cleanup */
    multiroarCloseP(r);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

int multiroarPTest(int argc, char *argv[]) {
    int err = 0;

    err += test_multiroarP_basic(argc, argv);
    err += test_multiroarP_recovery(argc, argv);
    err += test_multiroarP_compaction(argc, argv);
    err += test_multiroarP_fuzzing(argc, argv);

    return err;
}

#endif /* DATAKIT_TEST */
