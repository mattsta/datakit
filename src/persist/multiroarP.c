/* multiroarP.c - Persistent multiroar wrapper
 *
 * See multiroarP.h for API documentation.
 */

#include "multiroarP.h"
#include "../cleaner.h"
#include "../datakit.h"
#include "../persist.h"

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

    /* One INSERT per bit (there is no set-range op).  If a write fails,
     * apply exactly the bits already logged, so memory and WAL agree. */
    size_t logged = 0;
    while (logged < extent) {
        uint64_t pos = start + logged;
        if (!persistCtxLogOp(r->ctx, PERSIST_OP_INSERT, &pos, 1)) {
            break;
        }
        logged++;
    }

    /* Update in-memory */
    multiroarBitSetRange(r->r, start, logged);

    /* Check compaction threshold */
    persistCtxMaybeCompact(r->ctx, r->r, &persistOpsMultiroar);
}

void multiroarBitClearRangeP(multiroarP *r, uint64_t start, uint64_t extent) {
    if (!r || !r->r || !r->ctx) {
        return;
    }

    /* One CLEAR_RANGE row: the whole range is logged or none of it */
    const uint64_t range[2] = {start, extent};
    if (!persistCtxLogOp(r->ctx, PERSIST_OP_CLEAR_RANGE, range, 2)) {
        return;
    }

    /* Update in-memory */
    multiroarBitClearRange(r->r, start, extent);

    /* Check compaction threshold */
    persistCtxMaybeCompact(r->ctx, r->r, &persistOpsMultiroar);
}

bool multiroarValueSetP(multiroarP *r, uint64_t position,
                        const databox *value) {
    if (!r || !r->r || !r->ctx || !value) {
        return false;
    }
    const uint8_t width = multiroarValueWidth(r->r);
    if (!width || !multiroarValueInBounds(r->r, position)) {
        return false;
    }

    /* The integer kinds multiroarValueSet stores, within the width */
    uint64_t v;
    switch (value->type) {
    case DATABOX_UNSIGNED_64:
        v = value->data.u;
        break;
    case DATABOX_SIGNED_64:
        if (value->data.i < 0) {
            return false;
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
        return false;
    }
    if (width < 64 && v >> width) {
        return false;
    }

    const uint64_t words[2] = {position, v};
    if (!persistCtxLogOp(r->ctx, PERSIST_OP_INSERT, words, 2)) {
        return false;
    }
    databox stored = databoxNewUnsigned(v);
    multiroarValueSet(r->r, position, &stored);
    persistCtxMaybeCompact(r->ctx, r->r, &persistOpsMultiroar);
    return true;
}

bool multiroarValueRemoveP(multiroarP *r, uint64_t position) {
    if (!r || !r->r || !r->ctx || !multiroarValueWidth(r->r)) {
        return false;
    }
    if (!multiroarValueGetP(r, position, NULL)) {
        return true; /* nothing there: nothing to log */
    }

    if (!persistCtxLogOp(r->ctx, PERSIST_OP_DELETE, &position, 1)) {
        return false;
    }
    multiroarValueRemoveGetRemoved(r->r, position, NULL);
    persistCtxMaybeCompact(r->ctx, r->r, &persistOpsMultiroar);
    return true;
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
    multiroarBitSetRangeP(r, 100, 300);
    multiroarBitClearRangeP(r, 150, 50);
    multiroarRemoveP(r, 120);
    multiroarBitSetP(r, 9000);
    cleaner(multiroarFree) multiroar *want =
        multiroarDuplicate(multiroarGetRawP(r));
    if (!multiroarSyncP(r) || multiroarMemoryUsageP(r) == 0) {
        ERRR("Sync or memory usage failed");
    }
    persistCtxStats stats;
    multiroarGetStatsP(r, &stats);
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
        if (!multiroarValueRemoveP(r, i * 9)) {
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

int multiroarPTest(int argc, char *argv[]) {
    int err = 0;

    err += test_multiroarP_values(argc, argv);
    err += test_multiroarP_ranges(argc, argv);
    err += test_multiroarP_basic(argc, argv);
    err += test_multiroarP_recovery(argc, argv);
    err += test_multiroarP_compaction(argc, argv);
    err += test_multiroarP_fuzzing(argc, argv);

    return err;
}

#endif /* DATAKIT_TEST */
