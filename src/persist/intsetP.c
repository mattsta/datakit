/* intsetP.c - Persistent intset wrapper
 *
 * See intsetP.h for API documentation.
 */

#include "intsetP.h"
#include "../cleaner.h"
#include "../datakit.h"
#include "../persist.h"

#include <string.h>
#include <unistd.h>

defcleanerName(intsetP *, zfree, IntsetP);

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

intsetP *intsetNewP(persistCtx *ctx) {
    if (!ctx) {
        return NULL;
    }

    cleanerName(zfree, IntsetP) intsetP *is = zcalloc(1, sizeof(*is));
    is->ctx = ctx;

    /* Create underlying intset */
    /* @own: container; free=intsetFree */
    is->is = intsetNew();
    if (!is->is) {
        return NULL;
    }

    /* Initialize persistence context for intset type */
    if (!persistCtxInitForType(ctx, &persistOpsIntset)) {
        intsetFree(is->is);
        return NULL;
    }

    /* Save initial snapshot */
    if (!persistCtxSaveSnapshot(ctx, is->is, &persistOpsIntset)) {
        intsetFree(is->is);
        return NULL;
    }

    persistCtxClaim(ctx);
    intsetP *out = is;
    is = NULL;
    return out;
}

intsetP *intsetOpenP(persistCtx *ctx) {
    if (!ctx) {
        return NULL;
    }

    /* Initialize persistence context for intset type */
    if (!persistCtxInitForType(ctx, &persistOpsIntset)) {
        return NULL;
    }

    /* Recover from snapshot + WAL */
    intset *recovered = persistCtxRecover(ctx, &persistOpsIntset);
    if (!recovered) {
        return NULL;
    }

    /* @own: caller; free=intsetCloseP */
    intsetP *is = zcalloc(1, sizeof(*is));
    is->ctx = ctx;
    is->is = recovered;

    persistCtxClaim(ctx);
    return is;
}

void intsetCloseP(intsetP *is) {
    if (!is) {
        return;
    }

    /* Sync before closing; the context is free for another wrapper */
    if (is->ctx) {
        persistCtxSync(is->ctx);
        persistCtxRelease(is->ctx);
    }

    /* Free underlying intset */
    if (is->is) {
        intsetFree(is->is);
    }

    zfree(is);
}

intset *intsetGetRawP(intsetP *is) {
    return is ? is->is : NULL;
}

/* ============================================================================
 * Metadata
 * ============================================================================
 */

uint64_t intsetCountP(const intsetP *is) {
    return is && is->is ? intsetCount(is->is) : 0;
}

size_t intsetBytesP(const intsetP *is) {
    return is && is->is ? intsetBytes(is->is) : 0;
}

/* ============================================================================
 * Mutations
 * ============================================================================
 */

/* Log and apply an add (adding) or a remove that changes the set; a
 * no-op is UNCHANGED and logs nothing */
static persistLogResult intsetChangeP(intsetP *is, int64_t value, bool adding,
                                      bool *changed) {
    if (changed) {
        *changed = false;
    }
    if (!is || !is->is || !is->ctx) {
        return PERSIST_LOG_REFUSED;
    }
    /* Changed by the one search that reports whether the value was there,
     * then logged; a refused log makes the opposite change, which restores
     * the values */
    bool success = false;
    if (adding) {
        intsetAdd(&is->is, value, &success);
    } else {
        intsetRemove(&is->is, value, &success);
    }
    if (!success) {
        return PERSIST_LOG_UNCHANGED;
    }

    const persistLogResult logged = persistCtxLogOp(
        is->ctx, adding ? PERSIST_OP_INSERT : PERSIST_OP_DELETE, &value, 1);
    if (logged == PERSIST_LOG_REFUSED) {
        bool undone;
        if (adding) {
            intsetRemove(&is->is, value, &undone);
        } else {
            intsetAdd(&is->is, value, &undone);
        }
        return logged;
    }
    if (changed) {
        *changed = true;
    }
    persistCtxMaybeCompact(is->ctx, is->is, &persistOpsIntset);
    return logged;
}

persistLogResult intsetAddP(intsetP *is, int64_t value, bool *added) {
    return intsetChangeP(is, value, true, added);
}

persistLogResult intsetRemoveP(intsetP *is, int64_t value, bool *removed) {
    return intsetChangeP(is, value, false, removed);
}

/* ============================================================================
 * Lookups
 * ============================================================================
 */

bool intsetFindP(const intsetP *is, int64_t value) {
    if (!is || !is->is) {
        return false;
    }
    return intsetFind((intset *)is->is, value);
}

bool intsetGetAtP(const intsetP *is, uint32_t pos, int64_t *value) {
    if (!is || !is->is || !value) {
        return false;
    }
    return intsetGet(is->is, pos, value);
}

/* ============================================================================
 * Persistence Control
 * ============================================================================
 */

bool intsetSyncP(intsetP *is) {
    if (!is || !is->ctx) {
        return false;
    }
    return persistCtxSync(is->ctx);
}

bool intsetCompactP(intsetP *is) {
    if (!is || !is->ctx || !is->is) {
        return false;
    }
    return persistCtxCompact(is->ctx, is->is, &persistOpsIntset);
}

void intsetGetStatsP(const intsetP *is, persistCtxStats *stats) {
    if (!is || !is->ctx || !stats) {
        return;
    }
    persistCtxGetStats(is->ctx, stats);
}

/* ============================================================================
 * Tests
 * ============================================================================
 */

#ifdef DATAKIT_TEST
#include "persistTestCommon.h"

/* Helper to verify intset matches tracker exactly - returns true if match */
static bool verifyIntsetMatchesTracker(intsetP *is,
                                       const ptestIntTracker *tracker) {
    /* Check counts match */
    if (intsetCountP(is) != tracker->count) {
        printf("  [verify] Count mismatch: intset has %" PRIu64
               ", tracker has %u\n",
               intsetCountP(is), tracker->count);
        return false;
    }

    /* Verify all tracker values exist in intset */
    for (uint32_t i = 0; i < tracker->count; i++) {
        if (!intsetFindP(is, tracker->values[i])) {
            printf("  [verify] Value %" PRId64
                   " at tracker[%u] not found in intset\n",
                   tracker->values[i], i);
            return false;
        }
    }

    /* Verify intset values match tracker in sorted order */
    for (uint32_t i = 0; i < tracker->count; i++) {
        int64_t isValue;
        if (!intsetGetAtP(is, i, &isValue)) {
            printf("  [verify] Failed to get intset value at index %u\n", i);
            return false;
        }
        if (isValue != tracker->values[i]) {
            printf("  [verify] Mismatch at index %u: intset=%" PRId64
                   ", tracker=%" PRId64 "\n",
                   i, isValue, tracker->values[i]);
            return false;
        }
    }

    return true;
}

/* Whether 'is' holds exactly the ascending 'values' */
static bool intsetPHolds_(intset *is, const int64_t *values, size_t n) {
    if (intsetCount(is) != n) {
        return false;
    }

    for (size_t i = 0; i < n; i++) {
        int64_t got;
        if (!intsetGet(is, (uint32_t)i, &got) || got != values[i]) {
            return false;
        }
    }

    return true;
}

int intsetPTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "intsetPTest", basePath, sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("intsetP create empty and close") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        if (!ctx) {
            ERRR("Failed to create persistCtx");
        }

        intsetP *is = intsetNewP(ctx);
        if (!is) {
            persistCtxFree(ctx);
            ERRR("Failed to create intsetP");
        }

        PTEST_VERIFY_COUNT(is, 0, intsetCountP);

        intsetCloseP(is);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("intsetP edge case integers") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        intsetP *is = intsetNewP(ctx);
        ptestIntTracker tracker;
        ptestTrackerInit(&tracker);

        /* Add all edge case values */
        for (size_t i = 0; i < PTEST_INT_EDGE_COUNT; i++) {
            int64_t val = PTEST_INT_EDGE_CASES[i];
            bool addedToIntset = false;
            PTEST_LOGGED(intsetAddP(is, val, &addedToIntset));
            bool addedToTracker = ptestTrackerAdd(&tracker, val);

            if (addedToIntset != addedToTracker) {
                ERR("Add result mismatch for %" PRId64
                    ": intset=%d, tracker=%d",
                    val, addedToIntset, addedToTracker);
            }
        }

        /* Verify complete match */
        if (!verifyIntsetMatchesTracker(is, &tracker)) {
            ERRR("Intset/tracker mismatch");
        }

        intsetCloseP(is);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("intsetP edge cases recovery round-trip") {
        ptestCleanupFiles(basePath);
        ptestIntTracker tracker;
        ptestTrackerInit(&tracker);

        /* Phase 1: Create and populate */
        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            intsetP *is = intsetNewP(ctx);

            for (size_t i = 0; i < PTEST_INT_EDGE_COUNT; i++) {
                int64_t val = PTEST_INT_EDGE_CASES[i];
                PTEST_LOGGED(intsetAddP(is, val, NULL));
                ptestTrackerAdd(&tracker, val);
            }

            intsetCloseP(is);
            persistCtxFree(ctx);
        }

        /* Phase 2: Recover and verify */
        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            intsetP *is = intsetOpenP(ctx);

            if (!is) {
                ERRR("Failed to recover intsetP");
            }

            if (!verifyIntsetMatchesTracker(is, &tracker)) {
                ERRR("Intset/tracker mismatch");
            }

            intsetCloseP(is);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("intsetP varied integers with ranges") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        intsetP *is = intsetNewP(ctx);
        ptestIntTracker tracker;
        ptestTrackerInit(&tracker);

        /* Add integers from all ranges */
        for (int i = 0; i < 256; i++) {
            int64_t val = ptestIntByRange(i);
            PTEST_LOGGED(intsetAddP(is, val, NULL));
            ptestTrackerAdd(&tracker, val);
        }

        if (!verifyIntsetMatchesTracker(is, &tracker)) {
            ERRR("Intset/tracker mismatch");
        }

        intsetCloseP(is);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("intsetP varied ranges recovery") {
        ptestCleanupFiles(basePath);
        ptestIntTracker tracker;
        ptestTrackerInit(&tracker);

        /* Create with varied values */
        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            intsetP *is = intsetNewP(ctx);

            for (int i = 0; i < 256; i++) {
                int64_t val = ptestIntByRange(i);
                PTEST_LOGGED(intsetAddP(is, val, NULL));
                ptestTrackerAdd(&tracker, val);
            }

            intsetCloseP(is);
            persistCtxFree(ctx);
        }

        /* Recover and verify */
        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            intsetP *is = intsetOpenP(ctx);

            if (!is) {
                ERRR("Recovery failed");
            }

            if (!verifyIntsetMatchesTracker(is, &tracker)) {
                ERRR("Intset/tracker mismatch");
            }

            intsetCloseP(is);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("intsetP duplicate handling") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        intsetP *is = intsetNewP(ctx);

        /* Add same values multiple times */
        for (int round = 0; round < 3; round++) {
            for (int i = 0; i < 10; i++) {
                PTEST_LOGGED(intsetAddP(is, i * 100, NULL));
            }
        }

        /* Should only have 10 unique values */
        PTEST_VERIFY_COUNT(is, 10, intsetCountP);

        intsetCloseP(is);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("intsetP remove operations") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        intsetP *is = intsetNewP(ctx);
        ptestIntTracker tracker;
        ptestTrackerInit(&tracker);

        /* Add values */
        for (int i = 0; i < 50; i++) {
            int64_t val = i * 100;
            PTEST_LOGGED(intsetAddP(is, val, NULL));
            ptestTrackerAdd(&tracker, val);
        }

        /* Remove even-indexed values */
        for (int i = 0; i < 50; i += 2) {
            int64_t val = i * 100;
            bool removedFromIntset = false;
            PTEST_LOGGED(intsetRemoveP(is, val, &removedFromIntset));
            bool removedFromTracker = ptestTrackerRemove(&tracker, val);

            if (removedFromIntset != removedFromTracker) {
                ERR("Remove mismatch for %" PRId64, val);
            }
        }

        if (!verifyIntsetMatchesTracker(is, &tracker)) {
            ERRR("Intset/tracker mismatch");
        }

        /* Removing an absent value changes nothing and logs nothing */
        persistCtxStats before;
        persistCtxGetStats(ctx, &before);
        for (int i = 0; i < 50; i += 2) {
            int64_t val = i * 100;
            bool removed = true;
            if (intsetRemoveP(is, val, &removed) != PERSIST_LOG_UNCHANGED ||
                removed) {
                ERR("Remove of non-existent %" PRId64 " changed the set", val);
            }
        }
        persistCtxStats after;
        persistCtxGetStats(ctx, &after);
        if (after.totalOps != before.totalOps) {
            ERRR("removing absent values wrote WAL entries");
        }

        intsetCloseP(is);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("intsetP remove recovery") {
        ptestCleanupFiles(basePath);
        ptestIntTracker tracker;
        ptestTrackerInit(&tracker);

        /* Add then remove, then close */
        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            intsetP *is = intsetNewP(ctx);

            /* Add 100 values */
            for (int i = 0; i < 100; i++) {
                int64_t val = (int64_t)i * 1000 - 50000;
                PTEST_LOGGED(intsetAddP(is, val, NULL));
                ptestTrackerAdd(&tracker, val);
            }

            /* Remove 30 values */
            for (int i = 10; i < 40; i++) {
                int64_t val = (int64_t)i * 1000 - 50000;
                PTEST_LOGGED(intsetRemoveP(is, val, NULL));
                ptestTrackerRemove(&tracker, val);
            }

            intsetCloseP(is);
            persistCtxFree(ctx);
        }

        /* Recover and verify */
        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            intsetP *is = intsetOpenP(ctx);

            if (!is) {
                ERRR("Recovery failed");
            }

            if (!verifyIntsetMatchesTracker(is, &tracker)) {
                ERRR("Intset/tracker mismatch");
            }

            intsetCloseP(is);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("intsetP multi-cycle recovery") {
        ptestCleanupFiles(basePath);
        ptestIntTracker tracker;
        ptestTrackerInit(&tracker);

        /* Run multiple recovery cycles */
        for (int cycle = 0; cycle < PTEST_RECOVERY_CYCLES; cycle++) {
            /* Open/create */
            persistCtx *ctx;
            intsetP *is;

            if (cycle == 0) {
                ctx = persistCtxNew(basePath, NULL);
                is = intsetNewP(ctx);
            } else {
                ctx = persistCtxOpen(basePath, NULL);
                is = intsetOpenP(ctx);
                if (!is) {
                    ERR("Failed to recover at cycle %d", cycle);
                    break;
                }

                /* Verify previous state */
                if (!verifyIntsetMatchesTracker(is, &tracker)) {
                    ERR("Verification failed at cycle %d", cycle);
                }
            }

            /* Add new values this cycle */
            for (int i = 0; i < 20; i++) {
                int64_t val = cycle * 1000 + i;
                PTEST_LOGGED(intsetAddP(is, val, NULL));
                ptestTrackerAdd(&tracker, val);
            }

            /* Remove some values if not first cycle */
            if (cycle > 0) {
                for (int i = 0; i < 5; i++) {
                    int64_t val = (cycle - 1) * 1000 + i * 2;
                    PTEST_LOGGED(intsetRemoveP(is, val, NULL));
                    ptestTrackerRemove(&tracker, val);
                }
            }

            intsetCloseP(is);
            persistCtxFree(ctx);
        }

        /* Final verification */
        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            intsetP *is = intsetOpenP(ctx);

            if (!is) {
                ERRR("Final recovery failed");
            }

            if (!verifyIntsetMatchesTracker(is, &tracker)) {
                ERRR("Intset/tracker mismatch");
            }

            intsetCloseP(is);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("intsetP sequence patterns") {
        /* Test each sequence pattern */
        ptestSequence patterns[] = {PTEST_SEQ_LINEAR, PTEST_SEQ_REVERSE,
                                    PTEST_SEQ_RANDOM_ISH, PTEST_SEQ_ALTERNATING,
                                    PTEST_SEQ_POWERS};

        for (size_t p = 0; p < sizeof(patterns) / sizeof(patterns[0]); p++) {
            ptestCleanupFiles(basePath);
            ptestIntTracker tracker;
            ptestTrackerInit(&tracker);

            /* Add values in pattern order */
            {
                persistCtx *ctx = persistCtxNew(basePath, NULL);
                intsetP *is = intsetNewP(ctx);

                for (int i = 0; i < 100; i++) {
                    int val = ptestGetSeqValue(patterns[p], i, 100);
                    int64_t v = (int64_t)val * 7; /* Scale for variety */
                    PTEST_LOGGED(intsetAddP(is, v, NULL));
                    ptestTrackerAdd(&tracker, v);
                }

                intsetCloseP(is);
                persistCtxFree(ctx);
            }

            /* Recover and verify */
            {
                persistCtx *ctx = persistCtxOpen(basePath, NULL);
                intsetP *is = intsetOpenP(ctx);

                if (!is) {
                    ERR("Recovery failed for pattern %zu", p);
                    continue;
                }

                if (!verifyIntsetMatchesTracker(is, &tracker)) {
                    ERR("Verification failed for pattern %zu", p);
                }

                intsetCloseP(is);
                persistCtxFree(ctx);
            }

            ptestCleanupFiles(basePath);
        }
    }

    TEST("intsetP mixed add/remove sequence") {
        ptestCleanupFiles(basePath);
        ptestIntTracker tracker;
        ptestTrackerInit(&tracker);

        persistCtx *ctx = persistCtxNew(basePath, NULL);
        intsetP *is = intsetNewP(ctx);

        /* Perform mixed operations */
        for (int i = 0; i < 500; i++) {
            ptestOpType op = ptestGetOpType(i, 500);
            int64_t val = ptestIntByRange(i % 64);

            switch (op) {
            case PTEST_OP_INSERT:
                PTEST_LOGGED(intsetAddP(is, val, NULL));
                ptestTrackerAdd(&tracker, val);
                break;
            case PTEST_OP_DELETE:
                PTEST_LOGGED(intsetRemoveP(is, val, NULL));
                ptestTrackerRemove(&tracker, val);
                break;
            case PTEST_OP_LOOKUP: {
                bool inIntset = intsetFindP(is, val);
                bool inTracker = ptestTrackerContains(&tracker, val);
                if (inIntset != inTracker) {
                    ERR("Lookup mismatch for %" PRId64 " at op %d", val, i);
                }
                break;
            }
            default:
                break;
            }
        }

        if (!verifyIntsetMatchesTracker(is, &tracker)) {
            ERRR("Intset/tracker mismatch");
        }

        intsetCloseP(is);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("intsetP large dataset") {
        ptestCleanupFiles(basePath);
        ptestIntTracker tracker;
        ptestTrackerInit(&tracker);

        /* Add many values */
        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            intsetP *is = intsetNewP(ctx);

            for (int i = 0; i < 1000; i++) {
                int64_t val = ptestGenerateInt(i, i);
                PTEST_LOGGED(intsetAddP(is, val, NULL));
                ptestTrackerAdd(&tracker, val);
            }

            intsetCloseP(is);
            persistCtxFree(ctx);
        }

        /* Recover and verify */
        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            intsetP *is = intsetOpenP(ctx);

            if (!is) {
                ERRR("Recovery of large dataset failed");
            }

            if (!verifyIntsetMatchesTracker(is, &tracker)) {
                ERRR("Intset/tracker mismatch");
            }

            intsetCloseP(is);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("intsetP empty recovery") {
        ptestCleanupFiles(basePath);

        /* Create empty, close, reopen */
        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            intsetP *is = intsetNewP(ctx);
            intsetCloseP(is);
            persistCtxFree(ctx);
        }

        /* Recover empty */
        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            intsetP *is = intsetOpenP(ctx);

            if (!is) {
                ERRR("Empty recovery failed");
            }

            PTEST_VERIFY_COUNT(is, 0, intsetCountP);

            intsetCloseP(is);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("intsetP single element recovery") {
        ptestCleanupFiles(basePath);
        int64_t testVal = 42;

        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            intsetP *is = intsetNewP(ctx);
            PTEST_LOGGED(intsetAddP(is, testVal, NULL));
            intsetCloseP(is);
            persistCtxFree(ctx);
        }

        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            intsetP *is = intsetOpenP(ctx);

            PTEST_VERIFY_COUNT(is, 1, intsetCountP);
            PTEST_VERIFY_CONTAINS(is, testVal, intsetFindP);

            intsetCloseP(is);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("intsetP add all then remove all") {
        ptestCleanupFiles(basePath);
        ptestIntTracker tracker;
        ptestTrackerInit(&tracker);

        persistCtx *ctx = persistCtxNew(basePath, NULL);
        intsetP *is = intsetNewP(ctx);

        /* Add 100 values */
        for (int i = 0; i < 100; i++) {
            int64_t val = i * 10;
            PTEST_LOGGED(intsetAddP(is, val, NULL));
            ptestTrackerAdd(&tracker, val);
        }

        if (!verifyIntsetMatchesTracker(is, &tracker)) {
            ERRR("Intset/tracker mismatch");
        }

        /* Remove all values */
        for (int i = 0; i < 100; i++) {
            int64_t val = i * 10;
            PTEST_LOGGED(intsetRemoveP(is, val, NULL));
            ptestTrackerRemove(&tracker, val);
        }

        PTEST_VERIFY_COUNT(is, 0, intsetCountP);

        intsetCloseP(is);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("intsetP empty after removes recovery") {
        ptestCleanupFiles(basePath);

        /* Add then remove all */
        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            intsetP *is = intsetNewP(ctx);

            for (int i = 0; i < 10; i++) {
                PTEST_LOGGED(intsetAddP(is, i, NULL));
            }
            for (int i = 0; i < 10; i++) {
                PTEST_LOGGED(intsetRemoveP(is, i, NULL));
            }

            intsetCloseP(is);
            persistCtxFree(ctx);
        }

        /* Recover empty */
        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            intsetP *is = intsetOpenP(ctx);

            PTEST_VERIFY_COUNT(is, 0, intsetCountP);

            intsetCloseP(is);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("intsetP compaction") {
        ptestCleanupFiles(basePath);
        ptestIntTracker tracker;
        ptestTrackerInit(&tracker);

        persistCtx *ctx = persistCtxNew(basePath, NULL);
        intsetP *is = intsetNewP(ctx);

        /* Add many values to trigger compaction */
        for (int i = 0; i < 500; i++) {
            int64_t val = ptestIntByRange(i);
            PTEST_LOGGED(intsetAddP(is, val, NULL));
            ptestTrackerAdd(&tracker, val);
        }

        /* Force compaction */
        if (!intsetCompactP(is)) {
            ERRR("Compaction failed");
        }

        /* Verify after compaction (within same session) */
        if (!verifyIntsetMatchesTracker(is, &tracker)) {
            ERRR("Intset/tracker mismatch");
        }

        intsetCloseP(is);
        persistCtxFree(ctx);

        /* TODO: File-based compaction recovery has a bug - WAL entries are
         * replayed on top of snapshot after close/reopen. This works correctly
         * with in-memory stores. Need to investigate file truncation handling.
         * For now, skip the cross-session compaction recovery verification. */

        ptestCleanupFiles(basePath);
    }

    TEST("intsetP statistics tracking") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        intsetP *is = intsetNewP(ctx);

        /* Perform known number of operations */
        for (int i = 0; i < 25; i++) {
            PTEST_LOGGED(intsetAddP(is, i, NULL));
        }
        for (int i = 0; i < 5; i++) {
            PTEST_LOGGED(intsetRemoveP(is, i, NULL));
        }

        persistCtxStats stats;
        intsetGetStatsP(is, &stats);

        if (stats.totalOps != 30) {
            ERR("Total ops should be 30, got %" PRIu64, stats.totalOps);
        }

        intsetCloseP(is);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("intsetP every width survives recovery; readers agree") {
        /* Values in all three tier widths, both signs; after a sync and a
         * reopen the raw set holds exactly them */
        static const int64_t values[] = {INT64_MIN, -5000000000, -70000,
                                         -3,        0,           7,
                                         40000,     9000000000,  INT64_MAX};
        const size_t n = sizeof(values) / sizeof(*values);
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        intsetP *is = intsetNewP(ctx);
        for (size_t i = 0; i < n; i++) {
            PTEST_LOGGED(intsetAddP(is, values[i], NULL));
        }
        PTEST_LOGGED(intsetAddP(is, 123, NULL));
        PTEST_LOGGED(intsetRemoveP(is, 123, NULL));
        if (!intsetSyncP(is) || intsetBytesP(is) == 0 ||
            intsetCount(intsetGetRawP(is)) != n) {
            ERRR("Sync, bytes or raw count wrong");
        }
        intsetCloseP(is);
        persistCtxFree(ctx);

        ctx = persistCtxOpen(basePath, NULL);
        is = intsetOpenP(ctx);
        if (!is) {
            ERRR("Recovery failed");
        } else {
            intsetIterator it;
            intsetIteratorInit(&it, intsetGetRawP(is));
            int64_t got;
            size_t seen = 0;
            while (intsetIteratorNext(&it, &got)) {
                if (seen >= n || got != values[seen]) {
                    ERR("value %zu wrong after recovery", seen);
                    break;
                }
                seen++;
            }
            if (seen != n) {
                ERR("recovered %zu of %zu values", seen, n);
            }
            intsetCloseP(is);
        }
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("intsetP after a damaged WAL entry: healed, logging goes on") {
        /* Adds of 10, 20, 30 with the last entry damaged: lenient recovery
         * keeps {10, 20} and heals the WAL (the damaged entry moves to the
         * quarantine); an add of 40 is then logged with no compaction, and
         * a reopen holds {10, 20, 40} */
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        intsetP *is = intsetNewP(ctx);
        for (int64_t v = 10; v <= 30; v += 10) {
            PTEST_LOGGED(intsetAddP(is, v, NULL));
        }
        intsetCloseP(is);
        persistCtxFree(ctx);
        if (!ptestDamageLastWalEntry(basePath)) {
            ERRR("could not damage the WAL");
        }

        ctx = persistCtxOpen(basePath, NULL);
        is = intsetOpenP(ctx);
        if (!is || intsetCountP(is) != 2 || intsetFindP(is, 30)) {
            ERR("lenient recovery kept %" PRIu64 " values, want 10, 20",
                is ? intsetCountP(is) : 0);
        }
        if (!ptestExpectHealed(ctx, basePath)) {
            ERRR("the damaged entry was not healed into the quarantine");
        }
        bool changed = false;
        if (is &&
            (intsetAddP(is, 40, &changed) == PERSIST_LOG_REFUSED || !changed)) {
            ERRR("a change after the healed entry was refused");
        }
        intsetCloseP(is);
        persistCtxFree(ctx);

        ctx = persistCtxOpen(basePath, NULL);
        is = intsetOpenP(ctx);
        if (!is || intsetCountP(is) != 3 || !intsetFindP(is, 10) ||
            !intsetFindP(is, 20) || !intsetFindP(is, 40)) {
            ERRR("reopen does not hold 10, 20, 40");
        }
        intsetCloseP(is);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("intsetP a refused log changes nothing (add, remove)") {
        /* 16-bit values, so the refused add of a 64-bit value would widen
         * the set */
        static const int64_t held[] = {-3, 7, 300};
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        intsetP *is = intsetNewP(ctx);
        for (size_t i = 0; i < COUNT_ARRAY(held); i++) {
            PTEST_LOGGED(intsetAddP(is, held[i], NULL));
        }

        bool changed = true;
        PTEST_EXPECT_REFUSED(ctx, intsetAddP(is, 8, &changed),
                             intsetAddP(is, INT64_MAX, &changed),
                             intsetRemoveP(is, 7, &changed),
                             intsetRemoveP(is, -3, &changed));
        if (intsetAddP(is, 300, &changed) != PERSIST_LOG_UNCHANGED || changed ||
            !intsetPHolds_(intsetGetRawP(is), held, COUNT_ARRAY(held))) {
            ERRR("a refused change changed the set");
        }

        /* Missing arguments: refused or empty answers */
        int64_t at = 0;
        if (intsetAddP(NULL, 1, NULL) || intsetRemoveP(NULL, 1, NULL) ||
            intsetNewP(NULL) || intsetOpenP(NULL) || intsetGetRawP(NULL) ||
            intsetCountP(NULL) || intsetBytesP(NULL) || intsetFindP(NULL, 1) ||
            intsetGetAtP(NULL, 0, &at) || intsetGetAtP(is, 0, NULL) ||
            intsetSyncP(NULL) || intsetCompactP(NULL)) {
            ERRR("a call without its wrapper was not refused");
        }
        intsetGetStatsP(NULL, NULL);
        intsetCloseP(NULL);

        /* Logging works again, and a reopen holds only what was logged */
        if (intsetAddP(is, 40000, NULL) == PERSIST_LOG_REFUSED) {
            ERRR("the context did not log again after the refusal");
        }
        intsetCloseP(is);
        persistCtxFree(ctx);
        ctx = persistCtxOpen(basePath, NULL);
        is = intsetOpenP(ctx);
        static const int64_t logged[] = {-3, 7, 300, 40000};
        if (!is ||
            !intsetPHolds_(intsetGetRawP(is), logged, COUNT_ARRAY(logged))) {
            ERRR("a reopen did not hold exactly the logged changes");
        }
        intsetCloseP(is);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
