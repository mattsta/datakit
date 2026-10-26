/* multiroarP.c - Persistent multiroar wrapper
 *
 * See multiroarP.h for API documentation.
 */

#include "multiroarP.h"
#include "../cleaner.h"
#include "../datakit.h"
#include "../persist.h"

#include <assert.h>
#include <string.h>
#include <unistd.h>

defcleanerName(multiroarP *, zfree, MultiroarP);

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/* Wrap `roar` (taken over) as a new persistent multiroar: its starting
 * snapshot records its mode, so recovery restores the same kind */
static multiroarP *multiroarStartP(persistCtx *ctx, multiroar *roar) {
    if (!ctx || !roar) {
        multiroarFree(roar);
        return NULL;
    }

    cleanerName(zfree, MultiroarP) multiroarP *r = zcalloc(1, sizeof(*r));
    r->ctx = ctx;
    r->r = roar;

    /* Initialize persistence context for multiroar type */
    if (!persistCtxInitForType(ctx, &persistOpsMultiroar)) {
        multiroarFree(r->r);
        return NULL;
    }

    /* Save initial snapshot */
    if (!persistCtxSaveSnapshot(ctx, r->r, &persistOpsMultiroar)) {
        multiroarFree(r->r);
        return NULL;
    }

    persistCtxClaim(ctx);
    multiroarP *out = r;
    r = NULL;
    return out;
}

multiroarP *multiroarNewP(persistCtx *ctx) {
    /* @own: container; free=multiroarFree */
    return multiroarStartP(ctx, ctx ? multiroarBitNew() : NULL);
}

multiroarP *multiroarValueNewP(persistCtx *ctx, uint8_t bitWidth, uint64_t rows,
                               uint64_t cols) {
    /* @own: container; free=multiroarFree */
    return multiroarStartP(ctx, ctx ? multiroarValueNew(bitWidth, rows, cols)
                                    : NULL);
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

    /* @own: caller; free=multiroarCloseP */
    multiroarP *r = zcalloc(1, sizeof(*r));
    r->ctx = ctx;
    r->r = recovered;

    persistCtxClaim(ctx);
    return r;
}

void multiroarCloseP(multiroarP *r) {
    if (!r) {
        return;
    }

    /* Sync before closing; the context is free for another wrapper */
    if (r->ctx) {
        persistCtxSync(r->ctx);
        persistCtxRelease(r->ctx);
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

/* A bit-mode roar this wrapper can log for */
static bool multiroarBitModeP(const multiroarP *r) {
    return r && r->r && r->ctx && !multiroarValueWidth(r->r);
}

/* Log and apply a set (adding) or clear of one bit; a no-op logs
 * nothing */
static persistLogResult multiroarBitChangeP(multiroarP *r, uint64_t position,
                                            bool adding, bool *changed) {
    if (changed) {
        *changed = false;
    }
    if (!multiroarBitModeP(r)) {
        return PERSIST_LOG_REFUSED;
    }

    /* Changed by the one search that reports the bit's previous state,
     * then logged; a refused log flips the bit back, which restores it */
    const bool wasSet = adding ? multiroarBitSet(r->r, position)
                               : multiroarRemove(r->r, position);
    if (wasSet == adding) {
        return PERSIST_LOG_UNCHANGED;
    }

    const persistLogResult logged = persistCtxLogOp(
        r->ctx, adding ? PERSIST_OP_INSERT : PERSIST_OP_DELETE, &position, 1);
    if (logged == PERSIST_LOG_REFUSED) {
        if (adding) {
            multiroarRemove(r->r, position);
        } else {
            multiroarBitSet(r->r, position);
        }
        return logged;
    }
    if (changed) {
        *changed = true;
    }
    persistCtxMaybeCompact(r->ctx, r->r, &persistOpsMultiroar);
    return logged;
}

persistLogResult multiroarBitSetP(multiroarP *r, uint64_t position,
                                  bool *added) {
    return multiroarBitChangeP(r, position, true, added);
}

persistLogResult multiroarRemoveP(multiroarP *r, uint64_t position,
                                  bool *removed) {
    return multiroarBitChangeP(r, position, false, removed);
}

/* Log and apply a range op as one row: the whole range or none of it */
static persistLogResult multiroarRangeP(multiroarP *r, persistOp op,
                                        uint64_t start, uint64_t extent) {
    if (!multiroarBitModeP(r)) {
        return PERSIST_LOG_REFUSED;
    }
    if (extent == 0) {
        return PERSIST_LOG_UNCHANGED;
    }

    const uint64_t range[2] = {start, extent};
    const persistLogResult logged = persistCtxLogOp(r->ctx, op, range, 2);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }
    if (op == PERSIST_OP_SET_RANGE) {
        multiroarBitSetRange(r->r, start, extent);
    } else {
        multiroarBitClearRange(r->r, start, extent);
    }
    persistCtxMaybeCompact(r->ctx, r->r, &persistOpsMultiroar);
    return logged;
}

persistLogResult multiroarBitSetRangeP(multiroarP *r, uint64_t start,
                                       uint64_t extent) {
    return multiroarRangeP(r, PERSIST_OP_SET_RANGE, start, extent);
}

persistLogResult multiroarBitClearRangeP(multiroarP *r, uint64_t start,
                                         uint64_t extent) {
    return multiroarRangeP(r, PERSIST_OP_CLEAR_RANGE, start, extent);
}

persistLogResult multiroarValueSetP(multiroarP *r, uint64_t position,
                                    const databox *value) {
    if (!r || !r->r || !r->ctx || !value) {
        return PERSIST_LOG_REFUSED;
    }
    const uint8_t width = multiroarValueWidth(r->r);
    if (!width || !multiroarValueInBounds(r->r, position)) {
        return PERSIST_LOG_REFUSED;
    }

    /* The integer kinds multiroarValueSet stores, within the width */
    uint64_t v;
    switch (value->type) {
    case DATABOX_UNSIGNED_64:
        v = value->data.u;
        break;
    case DATABOX_SIGNED_64:
        if (value->data.i < 0) {
            return PERSIST_LOG_REFUSED;
        }
        v = (uint64_t)value->data.i;
        break;
    case DATABOX_TRUE:
        v = 1;
        break;
    case DATABOX_FALSE:
        v = 0;
        break;
    default:
        return PERSIST_LOG_REFUSED;
    }
    if (width < 64 && v >> width) {
        return PERSIST_LOG_REFUSED;
    }

    const uint64_t words[2] = {position, v};
    const persistLogResult logged =
        persistCtxLogOp(r->ctx, PERSIST_OP_INSERT, words, 2);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }
    /* Checked above: a value roar, in bounds, a value within the width */
    databox stored = databoxNewUnsigned(v);
    const multiroarValueSetResult set =
        multiroarValueSet(r->r, position, &stored);
    assert(set != MULTIROAR_VALUE_REFUSED);
    (void)set;
    persistCtxMaybeCompact(r->ctx, r->r, &persistOpsMultiroar);
    return logged;
}

persistLogResult multiroarValueRemoveP(multiroarP *r, uint64_t position,
                                       bool *removed) {
    if (removed) {
        *removed = false;
    }
    if (!r || !r->r || !r->ctx || !multiroarValueWidth(r->r)) {
        return PERSIST_LOG_REFUSED;
    }
    /* Removed by the one search that finds the value, then logged; a
     * refused log stores the removed value again, which restores it */
    databox was = {{0}};
    if (!multiroarValueRemoveGetRemoved(r->r, position, &was)) {
        return PERSIST_LOG_UNCHANGED; /* nothing there: nothing to log */
    }

    const persistLogResult logged =
        persistCtxLogOp(r->ctx, PERSIST_OP_DELETE, &position, 1);
    if (logged == PERSIST_LOG_REFUSED) {
        /* The position just held `was`, so it takes it back */
        const multiroarValueSetResult restored =
            multiroarValueSet(r->r, position, &was);
        assert(restored == MULTIROAR_VALUE_ADDED);
        (void)restored;
        return logged;
    }
    if (removed) {
        *removed = true;
    }
    persistCtxMaybeCompact(r->ctx, r->r, &persistOpsMultiroar);
    return logged;
}

/* ============================================================================
 * Lookups
 * ============================================================================
 */

bool multiroarValueGetP(const multiroarP *r, uint64_t position,
                        databox *value) {
    if (!r || !r->r || !multiroarValueWidth(r->r) ||
        !multiroarValueInBounds(r->r, position)) {
        return false;
    }
    return multiroarValueGet(r->r, position, value);
}

uint64_t multiroarValueCountP(const multiroarP *r) {
    return r && r->r ? multiroarValueCount(r->r) : 0;
}

bool multiroarBitGetP(const multiroarP *r, uint64_t position) {
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
#include "intsetP.h"
#include "persistTestCommon.h"

static int test_multiroarP_basic(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {{0}};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multiroarPTest_basic", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multiroarP basic persistence")

    ptestCleanupFiles(basePath);

    /* Create new persistent multiroar */
    persistCtx *ctx = persistCtxNew(basePath, NULL);
    multiroarP *r = multiroarNewP(ctx);
    if (!r) {
        ERRR("Failed to create multiroarP");
    }

    /* Add some bits */
    bool added = false;
    if (!multiroarBitSetP(r, 100, &added) || !added) {
        ERRR("Failed to set bit 100");
    }
    if (!multiroarBitSetP(r, 200, &added) || !added) {
        ERRR("Failed to set bit 200");
    }
    if (!multiroarBitSetP(r, 8192, &added) || !added) {
        ERRR("Failed to set bit 8192");
    }

    /* Setting a set bit changes nothing and logs nothing */
    if (multiroarBitSetP(r, 100, &added) != PERSIST_LOG_UNCHANGED || added) {
        ERRR("re-setting bit 100 was logged");
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
    bool removed = false;
    if (!multiroarRemoveP(r, 200, &removed) || !removed) {
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
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {{0}};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multiroarPTest_recovery", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multiroarP recovery from crash")

    ptestCleanupFiles(basePath);

    /* Create multiroar and add bits */
    persistCtx *ctx = persistCtxNew(basePath, NULL);
    multiroarP *r = multiroarNewP(ctx);

    for (uint64_t i = 0; i < 100; i++) {
        PTEST_LOGGED(multiroarBitSetP(r, i * 10, NULL));
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
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {{0}};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multiroarPTest_compaction", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

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
        PTEST_LOGGED(multiroarBitSetP(r, i, NULL));
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
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {{0}};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multiroarPTest_fuzzing", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

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
            PTEST_LOGGED(multiroarBitSetP(r, pos, NULL));
        } else {
            /* Delete */
            PTEST_LOGGED(multiroarRemoveP(r, pos, NULL));
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

defcleaner(multiroar *, multiroarFree);

static int test_multiroarP_ranges(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {{0}};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multiroarPTest_ranges", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multiroarP range set/clear and the readers survive recovery")

    ptestCleanupFiles(basePath);
    persistCtx *ctx = persistCtxNew(basePath, NULL);
    multiroarP *r = multiroarNewP(ctx);
    if (!multiroarIsEmptyP(r)) {
        ERRR("A new roar is not empty");
    }

    /* [100, 400) set, [150, 200) cleared, 120 removed, 9000 set: 250 bits */
    PTEST_LOGGED(multiroarBitSetRangeP(r, 100, 300));
    PTEST_LOGGED(multiroarBitClearRangeP(r, 150, 50));
    PTEST_LOGGED(multiroarRemoveP(r, 120, NULL));
    PTEST_LOGGED(multiroarBitSetP(r, 9000, NULL));
    cleaner(multiroarFree) multiroar *want =
        multiroarDuplicate(multiroarGetRawP(r));
    if (!multiroarSyncP(r) || multiroarMemoryUsageP(r) == 0) {
        ERRR("Sync or memory usage failed");
    }
    /* Each call is one logged op, however wide its range */
    persistCtxStats stats;
    multiroarGetStatsP(r, &stats);
    if (stats.totalOps != 4) {
        ERR("logged %" PRIu64 " ops for 4 calls", stats.totalOps);
    }
    multiroarCloseP(r);
    persistCtxFree(ctx);

    ctx = persistCtxOpen(basePath, NULL);
    r = multiroarOpenP(ctx);
    if (!r) {
        ERRR("Recovery failed");
    } else {
        uint64_t min = 0;
        uint64_t max = 0;
        uint64_t sel = 0;
        if (!multiroarEquals(multiroarGetRawP(r), want)) {
            ERRR("Recovered bits differ");
        }
        if (multiroarBitCountP(r) != 250 || multiroarIsEmptyP(r)) {
            ERR("Expected 250 bits, got %zu", multiroarBitCountP(r));
        }
        if (!multiroarMinP(r, &min) || min != 100 || !multiroarMaxP(r, &max) ||
            max != 9000) {
            ERR("min/max %" PRIu64 "/%" PRIu64 ", want 100/9000", min, max);
        }
        /* Below 200: 100..149 except 120 */
        if (multiroarRankP(r, 200) != 49) {
            ERR("rank(200) = %zu, want 49", multiroarRankP(r, 200));
        }
        /* The 50th bit: 100..149 holds 49, so the 50th is 200 */
        if (!multiroarSelectP(r, 50, &sel) || sel != 200) {
            ERR("select(50) = %" PRIu64 ", want 200", sel);
        }
        multiroarCloseP(r);
    }
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

static int test_multiroarP_values(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {{0}};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multiroarPTest_values", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multiroarP value mode survives a crash and refuses what it cannot "
         "store")

    ptestCleanupFiles(basePath);

    /* 12-bit values in a 50 x 40 grid */
    persistCtx *ctx = persistCtxNew(basePath, NULL);
    multiroarP *r = multiroarValueNewP(ctx, 12, 50, 40);
    if (!r) {
        ERRR("value-mode multiroarP not created");
        persistCtxFree(ctx);
        return err;
    }
    for (uint64_t i = 0; i < 200; i++) {
        const databox v = databoxNewUnsigned((i * 37) & 0xfff);
        if (!multiroarValueSetP(r, i * 9, &v)) {
            ERR("set at %" PRIu64 " refused", i * 9);
        }
    }
    for (uint64_t i = 0; i < 200; i += 5) {
        if (!multiroarValueRemoveP(r, i * 9, NULL)) {
            ERR("remove at %" PRIu64 " refused", i * 9);
        }
    }

    /* Refused without a change: too wide, outside the grid, not an
     * integer, negative */
    const databox wide = databoxNewUnsigned(0x1000);
    const databox fits = databoxNewUnsigned(1);
    const databox text = databoxNewBytesString("7");
    const databox negative = databoxNewSigned(-1);
    if (multiroarValueSetP(r, 1, &wide) || multiroarValueSetP(r, 2000, &fits) ||
        multiroarValueSetP(r, 1, &text) ||
        multiroarValueSetP(r, 1, &negative) || multiroarValueCountP(r) != 160) {
        ERRR("an unstorable value was accepted");
    }

    /* Crash: no close */
    persistCtxFree(ctx);
    multiroarFree(r->r);
    zfree(r);

    ctx = persistCtxNew(basePath, NULL);
    r = multiroarOpenP(ctx);
    if (!r) {
        ERRR("value-mode multiroarP did not recover");
        persistCtxFree(ctx);
        return err;
    }
    uint64_t rows;
    uint64_t cols;
    multiroarValueDimensions(multiroarGetRawP(r), &rows, &cols);
    if (multiroarValueWidth(multiroarGetRawP(r)) != 12 || rows != 50 ||
        cols != 40 || multiroarValueCountP(r) != 160) {
        ERRR("recovery lost the width, dimensions or values");
    }
    for (uint64_t i = 0; i < 200; i++) {
        databox got = {{0}};
        const bool present = multiroarValueGetP(r, i * 9, &got);
        if (present != (i % 5 != 0) ||
            (present && got.data.u != ((i * 37) & 0xfff))) {
            ERR("position %" PRIu64 " recovered wrongly", i * 9);
        }
    }

    /* Bit ops are not value ops */
    cleaner(multiroarFree) multiroar *bits = multiroarBitNew();
    const multiroarP bitWrapper = {.r = bits, .ctx = ctx};
    if (multiroarValueSetP((multiroarP *)&bitWrapper, 1, &fits)) {
        ERRR("a bit-mode roar took a value");
    }

    multiroarCloseP(r);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

static int test_multiroarP_lifecycleRefusals(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {{0}};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multiroarPTest_refusals", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multiroarP refuses to start or open what it cannot persist")

    ptestCleanupFiles(basePath);

    /* No context */
    if (multiroarNewP(NULL) || multiroarValueNewP(NULL, 8, 4, 4) ||
        multiroarOpenP(NULL)) {
        ERRR("a NULL context was accepted");
    }

    /* Value widths the roar cannot store: nothing starts, and the context
     * stays free for a roar that can */
    persistCtx *widthCtx = persistCtxNew(basePath, NULL);
    if (multiroarValueNewP(widthCtx, 0, 4, 4) ||
        multiroarValueNewP(widthCtx, 65, 4, 4)) {
        ERRR("a value width of 0 or 65 was accepted");
    }
    multiroarP *fits = multiroarValueNewP(widthCtx, 64, 4, 4);
    if (!fits) {
        ERRR("a 64-bit value roar was refused after the bad widths");
    } else {
        multiroarCloseP(fits);
    }
    persistCtxFree(widthCtx);
    ptestCleanupFiles(basePath);

    /* Files in a directory that does not exist: the stores cannot open,
     * so neither start nor open succeeds (the roar is freed) */
    char missingDir[PATH_MAX + sizeof("-missing/sub/roar")];
    snprintf(missingDir, sizeof(missingDir), "%s-missing/sub/roar", basePath);
    persistCtx *ctx = persistCtxNew(missingDir, NULL);
    if (ctx) {
        if (multiroarNewP(ctx) || multiroarOpenP(ctx)) {
            ERRR("a roar started where its files cannot be written");
        }
        persistCtxFree(ctx);
    }

    /* Nothing saved yet: open finds no snapshot to recover */
    ctx = persistCtxNew(basePath, NULL);
    if (multiroarOpenP(ctx)) {
        ERRR("a roar opened with no snapshot");
    }
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    /* Another structure's files are not a roar */
    ctx = persistCtxNew(basePath, NULL);
    intsetP *is = intsetNewP(ctx);
    if (!is || !intsetAddP(is, 42, NULL)) {
        ERRR("intsetP setup failed");
    }
    /* A context bound to the intset cannot take a roar: its snapshot and
     * WAL codecs are the intset's */
    if (multiroarNewP(ctx) || multiroarOpenP(ctx)) {
        ERRR("a roar started on a context bound to an intset");
    }
    intsetCloseP(is);
    persistCtxFree(ctx);
    ctx = persistCtxNew(basePath, NULL);
    multiroarP *foreign = multiroarOpenP(ctx);
    if (foreign) {
        ERRR("an intset's files opened as a roar");
        multiroarCloseP(foreign);
    }
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

static int test_multiroarP_damagedWal(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {{0}};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multiroarPTest_damagedWal", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multiroarP after a damaged WAL entry: healed, logging goes on")

    /* Bits 3, 5, 7 set with the last entry damaged: lenient recovery
     * keeps {3, 5}, the damaged entry moves to the quarantine, and
     * logging goes on with no compaction: a set of 9 and a clear of 3 are
     * logged, and a reopen holds {5, 9} */
    ptestCleanupFiles(basePath);
    persistCtx *ctx = persistCtxNew(basePath, NULL);
    multiroarP *r = multiroarNewP(ctx);
    for (uint64_t bit = 3; bit <= 7; bit += 2) {
        PTEST_LOGGED(multiroarBitSetP(r, bit, NULL));
    }
    multiroarCloseP(r);
    persistCtxFree(ctx);
    if (!ptestDamageLastWalEntry(basePath)) {
        ERRR("could not damage the WAL");
    }

    ctx = persistCtxOpen(basePath, NULL);
    r = multiroarOpenP(ctx);
    if (!r || multiroarBitCountP(r) != 2 || multiroarBitGetP(r, 7)) {
        ERR("lenient recovery kept %zu bits, want 3, 5",
            r ? multiroarBitCountP(r) : (size_t)0);
    }
    if (!ptestExpectHealed(ctx, basePath)) {
        ERRR("the damaged entry was not healed into the quarantine");
    }
    bool changed = false;
    if (r &&
        (multiroarBitSetP(r, 9, &changed) == PERSIST_LOG_REFUSED || !changed ||
         multiroarRemoveP(r, 3, &changed) == PERSIST_LOG_REFUSED || !changed)) {
        ERRR("a change after the healed entry was refused");
    }
    multiroarCloseP(r);
    persistCtxFree(ctx);

    ctx = persistCtxOpen(basePath, NULL);
    r = multiroarOpenP(ctx);
    if (!r || multiroarBitCountP(r) != 2 || multiroarBitGetP(r, 3) ||
        !multiroarBitGetP(r, 5) || !multiroarBitGetP(r, 9)) {
        ERRR("reopen does not hold 5, 9");
    }
    multiroarCloseP(r);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

static int test_multiroarP_valueKindsAndModes(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {{0}};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multiroarPTest_kinds", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multiroarP value kinds, mode refusals and readers without a roar")

    /* A non-negative signed value, true and false store as 5, 1 and 0
     * (0 is a stored value, not an absence) and survive a crash; removing
     * an empty position logs nothing and reports nothing removed; bit ops
     * on a value roar and value ops without a value are refused */
    ptestCleanupFiles(basePath);
    persistCtx *ctx = persistCtxNew(basePath, NULL);
    multiroarP *r = multiroarValueNewP(ctx, 8, 0, 0);
    if (!r) {
        ERRR("value-mode multiroarP not created");
        persistCtxFree(ctx);
        return err;
    }
    const databox five = databoxNewSigned(5);
    const databox yes = DATABOX_BOX_TRUE;
    const databox no = DATABOX_BOX_FALSE;
    if (!multiroarValueSetP(r, 10, &five) || !multiroarValueSetP(r, 11, &yes) ||
        !multiroarValueSetP(r, 12, &no)) {
        ERRR("a signed or boolean value was refused");
    }
    bool removed = true;
    if (multiroarValueRemoveP(r, 13, &removed) != PERSIST_LOG_UNCHANGED ||
        removed) {
        ERRR("removing an empty position was logged or reported");
    }
    bool changed = true;
    if (multiroarValueSetP(r, 14, NULL) != PERSIST_LOG_REFUSED ||
        multiroarBitSetP(r, 1, &changed) != PERSIST_LOG_REFUSED || changed ||
        multiroarRemoveP(r, 10, &changed) != PERSIST_LOG_REFUSED || changed ||
        multiroarBitSetRangeP(r, 0, 4) != PERSIST_LOG_REFUSED ||
        multiroarBitClearRangeP(r, 0, 4) != PERSIST_LOG_REFUSED ||
        multiroarValueCountP(r) != 3) {
        ERRR("a bit op or an absent value changed a value roar");
    }

    /* Crash: no close */
    persistCtxFree(ctx);
    multiroarFree(r->r);
    zfree(r);

    ctx = persistCtxNew(basePath, NULL);
    r = multiroarOpenP(ctx);
    static const uint64_t want[3] = {5, 1, 0};
    for (uint64_t i = 0; r && i < 3; i++) {
        databox got = {{0}};
        if (!multiroarValueGetP(r, 10 + i, &got) ||
            got.type != DATABOX_UNSIGNED_64 || got.data.u != want[i]) {
            ERR("position %" PRIu64 " did not recover as %" PRIu64, 10 + i,
                want[i]);
        }
    }
    if (!r || multiroarValueCountP(r) != 3) {
        ERRR("recovery lost a value");
    }
    multiroarCloseP(r);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    /* A bit roar: an empty range changes and logs nothing, and value
     * removal is refused */
    ctx = persistCtxNew(basePath, NULL);
    r = multiroarNewP(ctx);
    persistCtxStats before = {0};
    persistCtxStats after = {0};
    multiroarGetStatsP(r, &before);
    if (multiroarBitSetRangeP(r, 7, 0) != PERSIST_LOG_UNCHANGED ||
        multiroarBitClearRangeP(r, 7, 0) != PERSIST_LOG_UNCHANGED ||
        multiroarValueRemoveP(r, 7, &removed) != PERSIST_LOG_REFUSED ||
        removed || !multiroarIsEmptyP(r)) {
        ERRR("an empty range or a value removal changed a bit roar");
    }
    multiroarGetStatsP(r, &after);
    if (after.totalOps != before.totalOps) {
        ERRR("an empty range was logged");
    }
    multiroarCloseP(r);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    /* Every reader answers "nothing" for no roar; controls refuse */
    uint64_t pos = 99;
    persistCtxStats untouched = {.totalOps = 77};
    multiroarCloseP(NULL);
    multiroarGetStatsP(NULL, &untouched);
    if (multiroarGetRawP(NULL) || multiroarBitCountP(NULL) ||
        multiroarMemoryUsageP(NULL) || !multiroarIsEmptyP(NULL) ||
        multiroarValueGetP(NULL, 0, NULL) || multiroarValueCountP(NULL) ||
        multiroarBitGetP(NULL, 0) || multiroarMinP(NULL, &pos) ||
        multiroarMaxP(NULL, &pos) || multiroarRankP(NULL, 5) ||
        multiroarSelectP(NULL, 0, &pos) || pos != 99 || multiroarSyncP(NULL) ||
        multiroarCompactP(NULL) ||
        multiroarValueRemoveP(NULL, 0, NULL) != PERSIST_LOG_REFUSED ||
        untouched.totalOps != 77) {
        ERRR("a reader or control answered for no roar");
    }

    TEST_FINAL_RESULT;
}

/* Under a refusing WAL every change is refused and the roar equals its
 * copy from before; logging then works again and a reopen holds only the
 * logged change.  The bits cover a sparse chunk, a dense range, a chunk
 * holding one bit (removed whole by a remove) and a chunk not yet there
 * (made by a set). */
static int test_multiroarP_refusedLogs(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {{0}};
    char bitPath[PATH_MAX];
    char valuePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multiroarPTest_refusedBits", bitPath,
                          sizeof(bitPath)) ||
        !dkTestTmpDirPath(&tmp, "multiroarPTest_refusedValues", valuePath,
                          sizeof(valuePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multiroarP a refused log changes nothing (bit set, bit remove, "
         "value remove)")

    persistCtx *ctx = persistCtxNew(bitPath, NULL);
    multiroarP *r = multiroarNewP(ctx);
    PTEST_LOGGED(multiroarBitSetP(r, 5, NULL));
    PTEST_LOGGED(multiroarBitSetP(r, 70000, NULL));
    PTEST_LOGGED(multiroarBitSetRangeP(r, 200000, 20000));
    cleaner(multiroarFree) multiroar *want =
        multiroarDuplicate(multiroarGetRawP(r));

    bool changed = true;
    PTEST_EXPECT_REFUSED(ctx, multiroarBitSetP(r, 6, &changed),
                         multiroarBitSetP(r, UINT64_C(1) << 40, &changed),
                         multiroarRemoveP(r, 5, &changed),
                         multiroarRemoveP(r, 70000, &changed),
                         multiroarRemoveP(r, 210000, &changed),
                         multiroarBitSetRangeP(r, 300000, 9000),
                         multiroarBitClearRangeP(r, 200000, 20000));
    if (changed || !multiroarEquals(multiroarGetRawP(r), want)) {
        ERRR("a refused bit change changed the roar");
    }

    /* Missing arguments, and value calls on a bit roar: refused or empty
     * answers */
    uint64_t pos = 0;
    const databox one = databoxNewUnsigned(1);
    if (multiroarBitSetP(NULL, 1, NULL) || multiroarRemoveP(NULL, 1, NULL) ||
        multiroarBitSetRangeP(NULL, 0, 1) ||
        multiroarBitSetRangeP(r, 0, 0) != PERSIST_LOG_UNCHANGED ||
        multiroarValueSetP(r, 1, &one) || multiroarValueRemoveP(r, 1, NULL) ||
        multiroarNewP(NULL) || multiroarOpenP(NULL) || multiroarGetRawP(NULL) ||
        multiroarBitCountP(NULL) || multiroarMemoryUsageP(NULL) ||
        !multiroarIsEmptyP(NULL) || multiroarBitGetP(NULL, 5) ||
        multiroarMinP(NULL, &pos) || multiroarMaxP(NULL, &pos) ||
        multiroarRankP(NULL, 5) || multiroarSelectP(NULL, 0, &pos) ||
        multiroarSyncP(NULL) || multiroarCompactP(NULL)) {
        ERRR("a call without its wrapper was not refused");
    }
    multiroarGetStatsP(NULL, NULL);
    multiroarCloseP(NULL);
    if (multiroarBitSetP(r, 6, NULL) == PERSIST_LOG_REFUSED) {
        ERRR("the context did not log again after the refusal");
    }
    multiroarBitSet(want, 6);
    multiroarCloseP(r);
    persistCtxFree(ctx);
    ctx = persistCtxOpen(bitPath, NULL);
    r = multiroarOpenP(ctx);
    if (!r || !multiroarEquals(multiroarGetRawP(r), want)) {
        ERRR("a reopen did not hold exactly the logged bits");
    }
    multiroarCloseP(r);
    persistCtxFree(ctx);

    ctx = persistCtxNew(valuePath, NULL);
    r = multiroarValueNewP(ctx, 8, 1000, 1);
    const databox v200 = databoxNewUnsigned(200);
    const databox v0 = databoxNewUnsigned(0);
    PTEST_LOGGED(multiroarValueSetP(r, 7, &v200));
    PTEST_LOGGED(multiroarValueSetP(r, 9, &v0));
    cleaner(multiroarFree) multiroar *wantValues =
        multiroarDuplicate(multiroarGetRawP(r));

    const databox v50 = databoxNewUnsigned(50);
    PTEST_EXPECT_REFUSED(ctx, multiroarValueRemoveP(r, 7, &changed),
                         multiroarValueRemoveP(r, 9, &changed),
                         multiroarValueSetP(r, 7, &v50),
                         multiroarValueSetP(r, 11, &v50));
    if (multiroarValueRemoveP(r, 500, &changed) != PERSIST_LOG_UNCHANGED ||
        changed || !multiroarEquals(multiroarGetRawP(r), wantValues)) {
        ERRR("refused value changes changed the roar");
    }

    /* Bit calls on a value roar, and values it cannot store, are refused */
    const databox wide = databoxNewUnsigned(256);
    const databox negative = databoxNewSigned(-1);
    const databox text = databoxNewBytesString("x");
    if (multiroarBitSetP(r, 1, NULL) || multiroarBitSetRangeP(r, 0, 4) ||
        multiroarValueSetP(r, 1, &wide) ||
        multiroarValueSetP(r, 1, &negative) ||
        multiroarValueSetP(r, 1, &text) || multiroarValueSetP(r, 1000, &v50) ||
        multiroarValueSetP(r, 1, NULL) || multiroarValueSetP(NULL, 1, &v50) ||
        multiroarValueRemoveP(NULL, 1, NULL) ||
        multiroarValueNewP(NULL, 8, 1, 1) ||
        multiroarValueGetP(NULL, 7, NULL) || multiroarValueCountP(NULL)) {
        ERRR("a value call the roar cannot take was not refused");
    }

    /* Logging works again, and a remove reports itself */
    if (multiroarValueRemoveP(r, 7, &changed) == PERSIST_LOG_REFUSED ||
        !changed) {
        ERRR("the context did not log again after the refusal");
    }
    multiroarValueRemoveGetRemoved(wantValues, 7, NULL);
    multiroarCloseP(r);
    persistCtxFree(ctx);
    ctx = persistCtxOpen(valuePath, NULL);
    r = multiroarOpenP(ctx);
    if (!r || !multiroarEquals(multiroarGetRawP(r), wantValues)) {
        ERRR("a reopen did not hold exactly the logged values");
    }
    multiroarCloseP(r);
    persistCtxFree(ctx);

    TEST_FINAL_RESULT;
}

int multiroarPTest(int argc, char *argv[]) {
    int err = 0;

    err += test_multiroarP_refusedLogs(argc, argv);
    err += test_multiroarP_valueKindsAndModes(argc, argv);
    err += test_multiroarP_damagedWal(argc, argv);
    err += test_multiroarP_lifecycleRefusals(argc, argv);
    err += test_multiroarP_values(argc, argv);
    err += test_multiroarP_ranges(argc, argv);
    err += test_multiroarP_basic(argc, argv);
    err += test_multiroarP_recovery(argc, argv);
    err += test_multiroarP_compaction(argc, argv);
    err += test_multiroarP_fuzzing(argc, argv);

    return err;
}

#endif /* DATAKIT_TEST */
