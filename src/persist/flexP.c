/* flexP.c - Persistent flex wrapper
 *
 * See flexP.h for API documentation.
 */

#include "flexP.h"
#include "../cleaner.h"
#include "../datakit.h"
#include "../persist.h"

#include <string.h>
#include <unistd.h>

defcleanerName(flexP *, zfree, FlexP);

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

flexP *flexNewP(persistCtx *ctx) {
    if (!ctx) {
        return nullptr;
    }

    cleanerName(zfree, FlexP) flexP *f = zcalloc(1, sizeof(*f));
    f->ctx = ctx;

    /* Create underlying flex */
    /* @own: container; free=flexFree */
    f->f = flexNew();
    if (!f->f) {
        return nullptr;
    }

    /* Initialize persistence context for flex type */
    if (!persistCtxInitForType(ctx, &persistOpsFlex)) {
        flexFree(f->f);
        return nullptr;
    }

    /* Save initial snapshot */
    if (!persistCtxSaveSnapshot(ctx, f->f, &persistOpsFlex)) {
        flexFree(f->f);
        return nullptr;
    }

    persistCtxClaim(ctx);
    flexP *out = f;
    f = nullptr;
    return out;
}

flexP *flexOpenP(persistCtx *ctx) {
    if (!ctx) {
        return nullptr;
    }

    /* Initialize persistence context for flex type */
    if (!persistCtxInitForType(ctx, &persistOpsFlex)) {
        return nullptr;
    }

    /* Recover from snapshot + WAL */
    flex *recovered = persistCtxRecover(ctx, &persistOpsFlex);
    if (!recovered) {
        return nullptr;
    }

    /* @own: caller; free=flexCloseP */
    flexP *f = zcalloc(1, sizeof(*f));
    f->ctx = ctx;
    f->f = recovered;

    persistCtxClaim(ctx);
    return f;
}

void flexCloseP(flexP *f) {
    if (!f) {
        return;
    }

    /* Sync before closing; the context is free for another wrapper */
    if (f->ctx) {
        persistCtxSync(f->ctx);
        persistCtxRelease(f->ctx);
    }

    /* Free underlying flex */
    if (f->f) {
        flexFree(f->f);
    }

    zfree(f);
}

flex *flexGetP(flexP *f) {
    return f ? f->f : nullptr;
}

/* ============================================================================
 * Metadata
 * ============================================================================
 */

size_t flexCountP(const flexP *f) {
    return f && f->f ? flexCount(f->f) : 0;
}

size_t flexBytesP(const flexP *f) {
    return f && f->f ? flexBytes(f->f) : 0;
}

/* ============================================================================
 * Mutations
 * ============================================================================
 */

/* Log a push, then store the value exactly as replay will */
static persistLogResult flexPushP(flexP *f, const databox *box, persistOp op,
                                  flexEndpoint where) {
    if (!f || !f->f || !f->ctx || !box) {
        return PERSIST_LOG_REFUSED;
    }

    const persistLogResult logged = persistCtxLogOp(f->ctx, op, box, 1);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }

    flexPushByTypeExact(&f->f, box, where);
    persistCtxMaybeCompact(f->ctx, f->f, &persistOpsFlex);
    return logged;
}

persistLogResult flexPushHeadP(flexP *f, const databox *box) {
    return flexPushP(f, box, PERSIST_OP_PUSH_HEAD, FLEX_ENDPOINT_HEAD);
}

persistLogResult flexPushTailP(flexP *f, const databox *box) {
    return flexPushP(f, box, PERSIST_OP_PUSH_TAIL, FLEX_ENDPOINT_TAIL);
}

persistLogResult flexResetP(flexP *f) {
    if (!f || !f->f || !f->ctx) {
        return PERSIST_LOG_REFUSED;
    }

    const persistLogResult logged =
        persistCtxLogOp(f->ctx, PERSIST_OP_CLEAR, nullptr, 0);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }

    flexReset(&f->f);
    persistCtxMaybeCompact(f->ctx, f->f, &persistOpsFlex);
    return logged;
}

/* ============================================================================
 * Lookups
 * ============================================================================
 */

flexEntry *flexIndexP(const flexP *f, int32_t index) {
    if (!f || !f->f) {
        return nullptr;
    }
    return flexIndex(f->f, index);
}

void flexGetByTypeP(const flexP *f, flexEntry *fe, databox *box) {
    if (!f || !f->f || !fe || !box) {
        return;
    }
    flexGetByType(fe, box);
}

/* ============================================================================
 * Persistence Control
 * ============================================================================
 */

bool flexSyncP(flexP *f) {
    if (!f || !f->ctx) {
        return false;
    }
    return persistCtxSync(f->ctx);
}

bool flexCompactP(flexP *f) {
    if (!f || !f->ctx || !f->f) {
        return false;
    }
    return persistCtxCompact(f->ctx, f->f, &persistOpsFlex);
}

void flexGetStatsP(const flexP *f, persistCtxStats *stats) {
    if (!f || !f->ctx || !stats) {
        return;
    }
    persistCtxGetStats(f->ctx, stats);
}

/* ============================================================================
 * Tests
 * ============================================================================
 */

#ifdef DATAKIT_TEST
#include "../dkDiag.h"
#include "intsetP.h"
#include "persistTestCommon.h"

/* Helper to verify flex matches tracker exactly - returns true if match */
static bool verifyFlexMatchesTracker(flexP *f, const ptestBoxTracker *tracker) {
    /* Check counts match */
    if (flexCountP(f) != tracker->count) {
        printf("  [verify] Count mismatch: flex has %zu, tracker has %u\n",
               flexCountP(f), tracker->count);
        return false;
    }

    /* Verify all values match in order */
    for (uint32_t i = 0; i < tracker->count; i++) {
        flexEntry *fe = flexIndexP(f, i);
        if (!fe) {
            printf("  [verify] Failed to get flex entry at index %u\n", i);
            return false;
        }

        databox actualBox;
        flexGetByTypeP(f, fe, &actualBox);

        if (!ptestBoxesEqual(&actualBox, &tracker->boxes[i])) {
            printf("  [verify] Mismatch at index %u\n", i);
            return false;
        }
    }

    return true;
}

int flexPTest(int, char *[]) {
    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "flexPTest", basePath, sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("flexP create empty and close") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, nullptr);
        if (!ctx) {
            ERR("Failed to create persistCtx");
        }

        flexP *f = flexNewP(ctx);
        if (!f) {
            persistCtxFree(ctx);
            ERR("Failed to create flexP");
        }

        if (flexCountP(f) != 0) {
            ERR("New flex should be empty");
        }

        flexCloseP(f);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("flexP refusals change nothing: a refused log, missing arguments, "
         "a held or other-type context") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, nullptr);
        flexP *f = flexNewP(ctx);
        const databox one = DATABOX_SIGNED(1);
        PTEST_LOGGED(flexPushTailP(f, &one));
        PTEST_EXPECT_REFUSED(ctx, flexPushTailP(f, &one),
                             flexPushHeadP(f, &one), flexResetP(f));
        if (flexCountP(f) != 1) {
            ERR("refused changes left %zu entries, not 1", flexCountP(f));
        }

        /* Missing arguments: refused or empty answers */
        databox got = {};
        if (flexPushTailP(nullptr, &one) || flexPushHeadP(f, nullptr) ||
            flexResetP(nullptr) || flexNewP(nullptr) || flexOpenP(nullptr) ||
            flexGetP(nullptr) || flexCountP(nullptr) || flexBytesP(nullptr) ||
            flexIndexP(nullptr, 0) || flexSyncP(nullptr) ||
            flexCompactP(nullptr)) {
            ERR("a call without its wrapper or value was not refused");
        }
        flexGetByTypeP(nullptr, nullptr, &got);
        flexGetStatsP(nullptr, nullptr);
        flexCloseP(nullptr);

        /* A held context takes no second wrapper; once it is let go, it
         * stays bound to flex, so another type's wrapper is refused */
        const dkDiagPrintMode priorPrint = dkDiagGetPrintMode();
        dkDiagSetPrintMode(DK_DIAG_PRINT_NEVER);
        flexP *second = nullptr;
        DK_DIAG_EXPECT_REPORTS("a held context", 1, DK_DIAG_PERSIST_CTX_CLAIMED,
                               second = flexNewP(ctx));
        flexCloseP(f);
        intsetP *other = nullptr;
        DK_DIAG_EXPECT_REPORTS("a flex context", 1,
                               DK_DIAG_PERSIST_CTX_WRONG_TYPE,
                               other = intsetNewP(ctx));
        dkDiagSetPrintMode(priorPrint);
        if (second || other) {
            ERR("a held or other-type context took a wrapper");
        }
        intsetCloseP(other);
        flexCloseP(second);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("flexP push tail with all data types") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, nullptr);
        flexP *f = flexNewP(ctx);
        ptestBoxTracker tracker;
        ptestBoxTrackerInit(&tracker);

        /* Push each data type */
        for (int type = 0; type < PTEST_TYPE_COUNT; type++) {
            databox box;
            ptestGenerateBox(&box, type, type);
            PTEST_LOGGED(flexPushTailP(f, &box));
            ptestBoxTrackerPushTail(&tracker, &box);
        }

        if (!verifyFlexMatchesTracker(f, &tracker)) {
            ERR("Flex/tracker mismatch");
        }

        flexCloseP(f);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("flexP push head with all data types") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, nullptr);
        flexP *f = flexNewP(ctx);
        ptestBoxTracker tracker;
        ptestBoxTrackerInit(&tracker);

        /* Push each data type to head */
        for (int type = 0; type < PTEST_TYPE_COUNT; type++) {
            databox box;
            ptestGenerateBox(&box, type, type);
            PTEST_LOGGED(flexPushHeadP(f, &box));
            ptestBoxTrackerPushHead(&tracker, &box);
        }

        if (!verifyFlexMatchesTracker(f, &tracker)) {
            ERR("Flex/tracker mismatch");
        }

        flexCloseP(f);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("flexP stores values exactly, live and recovered, and reports "
         "each log") {
        ptestCleanupFiles(basePath);
        persistCtxConfig config = persistCtxDefaultConfig();
        config.syncMode = PERSIST_CTX_SYNC_ALWAYS;
        persistCtx *ctx = persistCtxNew(basePath, &config);
        flexP *f = flexNewP(ctx);
        static const char *const numerals[] = {"22", "007", "1.5", "-3"};
        for (size_t i = 0; i < 4; i++) {
            const databox s = databoxNewBytesString(numerals[i]);
            if (flexPushTailP(f, &s) != PERSIST_LOG_DURABLE) {
                ERR("push %zu was not durable", i);
            }
        }
        if (flexPushTailP(f, nullptr) != PERSIST_LOG_REFUSED ||
            flexCountP(f) != 4) {
            ERR("a NULL push was not refused");
        }

        /* The readers agree with the flex they wrap */
        if (flexBytesP(f) != flexBytes(flexGetP(f)) || flexBytesP(f) == 0 ||
            !flexSyncP(f) || flexBytesP(nullptr) != 0 || flexGetP(nullptr) ||
            flexSyncP(nullptr)) {
            ERR("flexBytesP / flexGetP / flexSyncP disagree");
        }

        for (int pass = 0; pass < 2; pass++) {
            for (size_t i = 0; i < 4; i++) {
                databox got;
                flexGetByTypeP(f, flexIndexP(f, (int32_t)i), &got);
                if (got.type != DATABOX_BYTES ||
                    got.len != strlen(numerals[i]) ||
                    memcmp(databoxBytes(&got), numerals[i], got.len) != 0) {
                    ERR("%s: \"%s\" changed type or bytes",
                        pass ? "recovered" : "live", numerals[i]);
                }
            }
            flexCloseP(f);
            persistCtxFree(ctx);
            if (pass == 0) {
                ctx = persistCtxOpen(basePath, &config);
                f = flexOpenP(ctx);
                if (!f) {
                    ERR("recovery failed");
                    persistCtxFree(ctx);
                    break;
                }
            }
        }
        ptestCleanupFiles(basePath);
    }

    TEST("flexP after a damaged WAL entry: healed, logging goes on") {
        /* Three pushes; the last WAL entry's digest damaged on disk.
         * Lenient recovery keeps two and heals the WAL (the damaged entry
         * moves to the quarantine); a push is then logged with no
         * compaction, and a reopen holds the two kept values and the new
         * one. */
        ptestCleanupFiles(basePath);
        {
            persistCtx *ctx = persistCtxNew(basePath, nullptr);
            flexP *f = flexNewP(ctx);
            for (int64_t v = 1; v <= 3; v++) {
                const databox box = DATABOX_SIGNED(v * 11);
                PTEST_LOGGED(flexPushTailP(f, &box));
            }
            flexCloseP(f);
            persistCtxFree(ctx);
        }
        if (!ptestDamageLastWalEntry(basePath)) {
            ERR("could not damage the WAL");
        }

        {
            persistCtx *ctx = persistCtxOpen(basePath, nullptr);
            flexP *f = flexOpenP(ctx);
            if (!f || flexCountP(f) != 2) {
                ERR("lenient recovery kept %zu values, want 2",
                    f ? flexCountP(f) : (size_t)0);
            }
            if (!ptestExpectHealed(ctx, basePath)) {
                ERR("the damaged entry was not healed into the quarantine");
            }
            const databox later = DATABOX_SIGNED(99);
            if (f && (flexPushTailP(f, &later) == PERSIST_LOG_REFUSED ||
                      flexCountP(f) != 3)) {
                ERR("a push after the healed entry was refused");
            }
            if (f) {
                flexCloseP(f);
            }
            persistCtxFree(ctx);
        }

        {
            persistCtx *ctx = persistCtxOpen(basePath, nullptr);
            flexP *f = flexOpenP(ctx);
            databox got = {};
            if (f && flexCountP(f) == 3) {
                flexGetByTypeP(f, flexIndexP(f, -1), &got);
            }
            if (!f || flexCountP(f) != 3 || got.data.i64 != 99) {
                ERR("reopen holds %zu values, want 11, 22, 99",
                    f ? flexCountP(f) : (size_t)0);
            }
            if (f) {
                flexCloseP(f);
            }
            persistCtxFree(ctx);
        }
        ptestCleanupFiles(basePath);
    }

    TEST("flexP all data types recovery round-trip") {
        ptestCleanupFiles(basePath);
        ptestBoxTracker tracker;
        ptestBoxTrackerInit(&tracker);

        /* Phase 1: Create and populate with all types */
        {
            persistCtx *ctx = persistCtxNew(basePath, nullptr);
            flexP *f = flexNewP(ctx);

            for (int type = 0; type < PTEST_TYPE_COUNT; type++) {
                for (int seed = 0; seed < 5; seed++) {
                    databox box;
                    ptestGenerateBox(&box, type, seed);
                    PTEST_LOGGED(flexPushTailP(f, &box));
                    ptestBoxTrackerPushTail(&tracker, &box);
                }
            }

            flexCloseP(f);
            persistCtxFree(ctx);
        }

        /* Phase 2: Recover and verify */
        {
            persistCtx *ctx = persistCtxOpen(basePath, nullptr);
            flexP *f = flexOpenP(ctx);

            if (!f) {
                ERR("Failed to recover flexP");
            }

            if (!verifyFlexMatchesTracker(f, &tracker)) {
                ERR("Flex/tracker mismatch");
            }

            flexCloseP(f);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("flexP mixed head/tail push") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, nullptr);
        flexP *f = flexNewP(ctx);
        ptestBoxTracker tracker;
        ptestBoxTrackerInit(&tracker);

        /* Alternate between push head and push tail */
        for (int i = 0; i < 50; i++) {
            databox box = DATABOX_SIGNED(i);
            if (i % 2 == 0) {
                PTEST_LOGGED(flexPushTailP(f, &box));
                ptestBoxTrackerPushTail(&tracker, &box);
            } else {
                PTEST_LOGGED(flexPushHeadP(f, &box));
                ptestBoxTrackerPushHead(&tracker, &box);
            }
        }

        if (!verifyFlexMatchesTracker(f, &tracker)) {
            ERR("Flex/tracker mismatch");
        }

        flexCloseP(f);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("flexP mixed push recovery") {
        ptestCleanupFiles(basePath);
        ptestBoxTracker tracker;
        ptestBoxTrackerInit(&tracker);

        /* Create with mixed pushes */
        {
            persistCtx *ctx = persistCtxNew(basePath, nullptr);
            flexP *f = flexNewP(ctx);

            for (int i = 0; i < 50; i++) {
                databox box = DATABOX_SIGNED(i * 100);
                if (i % 2 == 0) {
                    PTEST_LOGGED(flexPushTailP(f, &box));
                    ptestBoxTrackerPushTail(&tracker, &box);
                } else {
                    PTEST_LOGGED(flexPushHeadP(f, &box));
                    ptestBoxTrackerPushHead(&tracker, &box);
                }
            }

            flexCloseP(f);
            persistCtxFree(ctx);
        }

        /* Recover and verify */
        {
            persistCtx *ctx = persistCtxOpen(basePath, nullptr);
            flexP *f = flexOpenP(ctx);

            if (!f) {
                ERR("Recovery failed");
            }

            if (!verifyFlexMatchesTracker(f, &tracker)) {
                ERR("Flex/tracker mismatch");
            }

            flexCloseP(f);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("flexP reset and continue") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, nullptr);
        flexP *f = flexNewP(ctx);
        ptestBoxTracker tracker;
        ptestBoxTrackerInit(&tracker);

        /* Push some values */
        for (int i = 0; i < 20; i++) {
            databox box = DATABOX_SIGNED(i);
            PTEST_LOGGED(flexPushTailP(f, &box));
        }

        /* Reset */
        PTEST_LOGGED(flexResetP(f));
        ptestBoxTrackerInit(&tracker); /* Clear tracker */

        if (flexCountP(f) != 0) {
            ERR("Count should be 0 after reset");
        }

        /* Push more values after reset */
        for (int i = 100; i < 110; i++) {
            databox box = DATABOX_SIGNED(i);
            PTEST_LOGGED(flexPushTailP(f, &box));
            ptestBoxTrackerPushTail(&tracker, &box);
        }

        if (!verifyFlexMatchesTracker(f, &tracker)) {
            ERR("Flex/tracker mismatch");
        }

        flexCloseP(f);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("flexP reset recovery") {
        ptestCleanupFiles(basePath);
        ptestBoxTracker tracker;
        ptestBoxTrackerInit(&tracker);

        /* Create, populate, reset, repopulate, close */
        {
            persistCtx *ctx = persistCtxNew(basePath, nullptr);
            flexP *f = flexNewP(ctx);

            for (int i = 0; i < 10; i++) {
                databox box = DATABOX_SIGNED(i);
                PTEST_LOGGED(flexPushTailP(f, &box));
            }

            PTEST_LOGGED(flexResetP(f));

            for (int i = 100; i < 115; i++) {
                databox box = DATABOX_SIGNED(i);
                PTEST_LOGGED(flexPushTailP(f, &box));
                ptestBoxTrackerPushTail(&tracker, &box);
            }

            flexCloseP(f);
            persistCtxFree(ctx);
        }

        /* Recover and verify only post-reset data */
        {
            persistCtx *ctx = persistCtxOpen(basePath, nullptr);
            flexP *f = flexOpenP(ctx);

            if (!f) {
                ERR("Recovery failed");
            }

            if (!verifyFlexMatchesTracker(f, &tracker)) {
                ERR("Flex/tracker mismatch");
            }

            flexCloseP(f);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("flexP multi-cycle recovery") {
        ptestCleanupFiles(basePath);
        ptestBoxTracker tracker;
        ptestBoxTrackerInit(&tracker);

        /* Multiple recovery cycles */
        for (int cycle = 0; cycle < PTEST_RECOVERY_CYCLES; cycle++) {
            persistCtx *ctx;
            flexP *f;

            if (cycle == 0) {
                ctx = persistCtxNew(basePath, nullptr);
                f = flexNewP(ctx);
            } else {
                ctx = persistCtxOpen(basePath, nullptr);
                f = flexOpenP(ctx);
                if (!f) {
                    ERR("Failed to recover at cycle %d", cycle);
                    break;
                }

                /* Verify previous state */
                if (!verifyFlexMatchesTracker(f, &tracker)) {
                    ERR("Verification failed at cycle %d", cycle);
                }
            }

            /* Add new values this cycle */
            for (int i = 0; i < 10; i++) {
                databox box = DATABOX_SIGNED(cycle * 1000 + i);
                PTEST_LOGGED(flexPushTailP(f, &box));
                ptestBoxTrackerPushTail(&tracker, &box);
            }

            flexCloseP(f);
            persistCtxFree(ctx);
        }

        /* Final verification */
        {
            persistCtx *ctx = persistCtxOpen(basePath, nullptr);
            flexP *f = flexOpenP(ctx);

            if (!f) {
                ERR("Final recovery failed");
            }

            if (!verifyFlexMatchesTracker(f, &tracker)) {
                ERR("Flex/tracker mismatch");
            }

            flexCloseP(f);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("flexP string data") {
        ptestCleanupFiles(basePath);
        ptestBoxTracker tracker;
        ptestBoxTrackerInit(&tracker);

        {
            persistCtx *ctx = persistCtxNew(basePath, nullptr);
            flexP *f = flexNewP(ctx);

            /* Push various strings */
            for (int i = 0; i < (int)PTEST_STRING_COUNT; i++) {
                databox box;
                ptestGenerateStringBox(&box, i);
                PTEST_LOGGED(flexPushTailP(f, &box));
                ptestBoxTrackerPushTail(&tracker, &box);
            }

            flexCloseP(f);
            persistCtxFree(ctx);
        }

        /* Recover and verify strings */
        {
            persistCtx *ctx = persistCtxOpen(basePath, nullptr);
            flexP *f = flexOpenP(ctx);

            if (!f) {
                ERR("String recovery failed");
            }

            if (flexCountP(f) != PTEST_STRING_COUNT) {
                ERR("String count mismatch: got %zu, expected %zu",
                    flexCountP(f), PTEST_STRING_COUNT);
            }

            flexCloseP(f);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("flexP large dataset") {
        ptestCleanupFiles(basePath);
        ptestBoxTracker tracker;
        ptestBoxTrackerInit(&tracker);

        /* Add many values */
        {
            persistCtx *ctx = persistCtxNew(basePath, nullptr);
            flexP *f = flexNewP(ctx);

            for (int i = 0; i < 500; i++) {
                databox box;
                ptestGenerateBox(&box, i % PTEST_TYPE_COUNT, i);
                PTEST_LOGGED(flexPushTailP(f, &box));
                ptestBoxTrackerPushTail(&tracker, &box);
            }

            flexCloseP(f);
            persistCtxFree(ctx);
        }

        /* Recover and verify */
        {
            persistCtx *ctx = persistCtxOpen(basePath, nullptr);
            flexP *f = flexOpenP(ctx);

            if (!f) {
                ERR("Large dataset recovery failed");
            }

            if (!verifyFlexMatchesTracker(f, &tracker)) {
                ERR("Flex/tracker mismatch");
            }

            flexCloseP(f);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("flexP empty recovery") {
        ptestCleanupFiles(basePath);

        /* Create empty, close, reopen */
        {
            persistCtx *ctx = persistCtxNew(basePath, nullptr);
            flexP *f = flexNewP(ctx);
            flexCloseP(f);
            persistCtxFree(ctx);
        }

        /* Recover empty */
        {
            persistCtx *ctx = persistCtxOpen(basePath, nullptr);
            flexP *f = flexOpenP(ctx);

            if (!f) {
                ERR("Empty recovery failed");
            }

            if (flexCountP(f) != 0) {
                ERR("Empty flex should have count 0");
            }

            flexCloseP(f);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("flexP single element recovery") {
        ptestCleanupFiles(basePath);
        databox testBox = DATABOX_SIGNED(42);

        {
            persistCtx *ctx = persistCtxNew(basePath, nullptr);
            flexP *f = flexNewP(ctx);
            PTEST_LOGGED(flexPushTailP(f, &testBox));
            flexCloseP(f);
            persistCtxFree(ctx);
        }

        {
            persistCtx *ctx = persistCtxOpen(basePath, nullptr);
            flexP *f = flexOpenP(ctx);

            if (flexCountP(f) != 1) {
                ERR("Should have 1 element");
            }

            flexEntry *fe = flexIndexP(f, 0);
            databox actualBox;
            flexGetByTypeP(f, fe, &actualBox);

            if (!ptestBoxesEqual(&actualBox, &testBox)) {
                ERR("Single element mismatch");
            }

            flexCloseP(f);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("flexP sequence patterns") {
        ptestSequence patterns[] = {PTEST_SEQ_LINEAR, PTEST_SEQ_REVERSE,
                                    PTEST_SEQ_RANDOM_ISH, PTEST_SEQ_ALTERNATING,
                                    PTEST_SEQ_POWERS};

        for (size_t p = 0; p < countof(patterns); p++) {
            ptestCleanupFiles(basePath);
            ptestBoxTracker tracker;
            ptestBoxTrackerInit(&tracker);

            /* Add values in pattern order */
            {
                persistCtx *ctx = persistCtxNew(basePath, nullptr);
                flexP *f = flexNewP(ctx);

                for (int i = 0; i < 100; i++) {
                    int val = ptestGetSeqValue(patterns[p], i, 100);
                    databox box = DATABOX_SIGNED(val);
                    PTEST_LOGGED(flexPushTailP(f, &box));
                    ptestBoxTrackerPushTail(&tracker, &box);
                }

                flexCloseP(f);
                persistCtxFree(ctx);
            }

            /* Recover and verify */
            {
                persistCtx *ctx = persistCtxOpen(basePath, nullptr);
                flexP *f = flexOpenP(ctx);

                if (!f) {
                    ERR("Recovery failed for pattern %zu", p);
                    continue;
                }

                if (!verifyFlexMatchesTracker(f, &tracker)) {
                    ERR("Verification failed for pattern %zu", p);
                }

                flexCloseP(f);
                persistCtxFree(ctx);
            }

            ptestCleanupFiles(basePath);
        }
    }

    TEST("flexP compaction") {
        ptestCleanupFiles(basePath);
        ptestBoxTracker tracker;
        ptestBoxTrackerInit(&tracker);

        persistCtx *ctx = persistCtxNew(basePath, nullptr);
        flexP *f = flexNewP(ctx);

        /* Add many values */
        for (int i = 0; i < 300; i++) {
            databox box = DATABOX_SIGNED(i);
            PTEST_LOGGED(flexPushTailP(f, &box));
            ptestBoxTrackerPushTail(&tracker, &box);
        }

        /* Force compaction */
        if (!flexCompactP(f)) {
            ERR("Compaction failed");
        }

        /* Verify after compaction (within same session) */
        if (!verifyFlexMatchesTracker(f, &tracker)) {
            ERR("Flex/tracker mismatch");
        }

        /* Writes after the snapshot go to the WAL alone */
        for (int i = 300; i < 305; i++) {
            databox box = DATABOX_SIGNED(i);
            PTEST_LOGGED(flexPushTailP(f, &box));
            ptestBoxTrackerPushTail(&tracker, &box);
        }

        flexCloseP(f);
        persistCtxFree(ctx);

        /* A reopen is the snapshot plus only the WAL written after it: the
         * 300 compacted values are not replayed a second time */
        {
            persistCtx *again = persistCtxOpen(basePath, nullptr);
            flexP *reopened = flexOpenP(again);
            if (!reopened) {
                ERR("reopen after compaction failed");
            } else {
                if (!verifyFlexMatchesTracker(reopened, &tracker)) {
                    ERR("reopen after compaction does not match: %zu values",
                        flexCountP(reopened));
                }
                flexCloseP(reopened);
            }
            persistCtxFree(again);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("flexP statistics tracking") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, nullptr);
        flexP *f = flexNewP(ctx);

        /* Perform known number of operations */
        for (int i = 0; i < 20; i++) {
            databox box = DATABOX_SIGNED(i);
            PTEST_LOGGED(flexPushTailP(f, &box));
        }
        for (int i = 0; i < 5; i++) {
            databox box = DATABOX_SIGNED(i + 100);
            PTEST_LOGGED(flexPushHeadP(f, &box));
        }

        persistCtxStats stats;
        flexGetStatsP(f, &stats);

        if (stats.totalOps != 25) {
            ERR("Total ops should be 25, got %" PRIu64, stats.totalOps);
        }

        flexCloseP(f);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("flexP negative index access") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, nullptr);
        flexP *f = flexNewP(ctx);

        /* Push 10 values */
        for (int i = 0; i < 10; i++) {
            databox box = DATABOX_SIGNED(i);
            PTEST_LOGGED(flexPushTailP(f, &box));
        }

        /* Access with negative index */
        flexEntry *fe = flexIndexP(f, -1);
        if (!fe) {
            ERR("Negative index access failed");
        }

        databox lastBox;
        flexGetByTypeP(f, fe, &lastBox);

        if (lastBox.data.i != 9) {
            ERR("Last element should be 9, got %" PRIdMAX, lastBox.data.i);
        }

        fe = flexIndexP(f, -10);
        if (!fe) {
            ERR("First element via negative index failed");
        }

        databox firstBox;
        flexGetByTypeP(f, fe, &firstBox);

        if (firstBox.data.i != 0) {
            ERR("First element should be 0, got %" PRIdMAX, firstBox.data.i);
        }

        flexCloseP(f);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("flexP integer variations") {
        ptestCleanupFiles(basePath);
        ptestBoxTracker tracker;
        ptestBoxTrackerInit(&tracker);

        {
            persistCtx *ctx = persistCtxNew(basePath, nullptr);
            flexP *f = flexNewP(ctx);

            /* Test all edge case integers */
            for (size_t i = 0; i < PTEST_INT_EDGE_COUNT; i++) {
                databox box = DATABOX_SIGNED(PTEST_INT_EDGE_CASES[i]);
                PTEST_LOGGED(flexPushTailP(f, &box));
                ptestBoxTrackerPushTail(&tracker, &box);
            }

            flexCloseP(f);
            persistCtxFree(ctx);
        }

        {
            persistCtx *ctx = persistCtxOpen(basePath, nullptr);
            flexP *f = flexOpenP(ctx);

            if (!verifyFlexMatchesTracker(f, &tracker)) {
                ERR("Flex/tracker mismatch");
            }

            flexCloseP(f);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
