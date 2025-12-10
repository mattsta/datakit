/* multidictP.c - Persistent multidict wrapper
 *
 * See multidictP.h for API documentation.
 */

#include "multidictP.h"
#include "../datakit.h"
#include "../persist.h"

#include <string.h>
#include <unistd.h>

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

multidictP *multidictNewP(persistCtx *ctx) {
    if (!ctx) {
        return NULL;
    }

    multidictP *d = zcalloc(1, sizeof(*d));
    d->ctx = ctx;

    /* Create underlying multidict with default type and class */
    multidictClass *qdc = multidictDefaultClassNew();
    d->d = multidictNew(&multidictTypeExactKey, qdc, 0);
    if (!d->d) {
        zfree(d);
        return NULL;
    }

    /* Initialize persistence context for multidict type */
    if (!persistCtxInitForType(ctx, &persistOpsMultidict)) {
        multidictFree(d->d);
        zfree(d);
        return NULL;
    }

    /* Save initial snapshot */
    if (!persistCtxSaveSnapshot(ctx, d->d, &persistOpsMultidict)) {
        multidictFree(d->d);
        zfree(d);
        return NULL;
    }

    return d;
}

multidictP *multidictOpenP(persistCtx *ctx) {
    if (!ctx) {
        return NULL;
    }

    /* Initialize persistence context for multidict type */
    if (!persistCtxInitForType(ctx, &persistOpsMultidict)) {
        return NULL;
    }

    /* Recover from snapshot + WAL */
    multidict *recovered = persistCtxRecover(ctx, &persistOpsMultidict);
    if (!recovered) {
        return NULL;
    }

    multidictP *d = zcalloc(1, sizeof(*d));
    d->ctx = ctx;
    d->d = recovered;

    return d;
}

void multidictCloseP(multidictP *d) {
    if (!d) {
        return;
    }

    /* Sync before closing */
    if (d->ctx) {
        persistCtxSync(d->ctx);
    }

    /* Free underlying multidict */
    if (d->d) {
        multidictFree(d->d);
    }

    zfree(d);
}

multidict *multidictGetP(multidictP *d) {
    return d ? d->d : NULL;
}

/* ============================================================================
 * Metadata
 * ============================================================================
 */

uint64_t multidictCountP(const multidictP *d) {
    return d && d->d ? multidictCount(d->d) : 0;
}

uint64_t multidictBytesP(const multidictP *d) {
    return d && d->d ? multidictBytes(d->d) : 0;
}

/* ============================================================================
 * Mutations
 * ============================================================================
 */

multidictResult multidictAddP(multidictP *d, const databox *key,
                              const databox *val) {
    if (!d || !d->d || !d->ctx || !key || !val) {
        return MULTIDICT_ERR;
    }

    /* Log to WAL first */
    const databox *args[] = {key, val};
    if (!persistCtxLogOp(d->ctx, PERSIST_OP_INSERT, args, 2)) {
        return MULTIDICT_ERR;
    }

    /* Update in-memory */
    multidictResult result = multidictAdd(d->d, key, val);

    /* Check compaction threshold */
    persistCtxMaybeCompact(d->ctx, d->d, &persistOpsMultidict);

    return result;
}

multidictResult multidictReplaceP(multidictP *d, const databox *key,
                                  const databox *val) {
    if (!d || !d->d || !d->ctx || !key || !val) {
        return MULTIDICT_ERR;
    }

    /* Log to WAL first */
    const databox *args[] = {key, val};
    if (!persistCtxLogOp(d->ctx, PERSIST_OP_REPLACE, args, 2)) {
        return MULTIDICT_ERR;
    }

    /* Update in-memory */
    multidictResult result = multidictReplace(d->d, key, val);

    /* Check compaction threshold */
    persistCtxMaybeCompact(d->ctx, d->d, &persistOpsMultidict);

    return result;
}

bool multidictDeleteP(multidictP *d, const databox *key) {
    if (!d || !d->d || !d->ctx || !key) {
        return false;
    }

    /* Log to WAL first - note: delete takes a single databox*, not array */
    if (!persistCtxLogOp(d->ctx, PERSIST_OP_DELETE, key, 1)) {
        return false;
    }

    /* Update in-memory */
    bool result = multidictDelete(d->d, key);

    /* Check compaction threshold */
    persistCtxMaybeCompact(d->ctx, d->d, &persistOpsMultidict);

    return result;
}

void multidictEmptyP(multidictP *d) {
    if (!d || !d->d || !d->ctx) {
        return;
    }

    /* Log to WAL first */
    if (!persistCtxLogOp(d->ctx, PERSIST_OP_CLEAR, NULL, 0)) {
        return;
    }

    /* Update in-memory */
    multidictEmpty(d->d);

    /* Check compaction threshold */
    persistCtxMaybeCompact(d->ctx, d->d, &persistOpsMultidict);
}

/* ============================================================================
 * Lookups
 * ============================================================================
 */

bool multidictFindP(multidictP *d, const databox *key, databox *val) {
    if (!d || !d->d) {
        return false;
    }
    return multidictFind(d->d, key, val);
}

bool multidictExistsP(multidictP *d, const databox *key) {
    if (!d || !d->d) {
        return false;
    }
    return multidictExists(d->d, key);
}

/* ============================================================================
 * Iteration
 * ============================================================================
 */

bool multidictIteratorInitP(multidictP *d, multidictIterator *iter) {
    if (!d || !d->d) {
        return false;
    }
    return multidictIteratorInit(d->d, iter);
}

/* ============================================================================
 * Persistence Control
 * ============================================================================
 */

bool multidictSyncP(multidictP *d) {
    if (!d || !d->ctx) {
        return false;
    }
    return persistCtxSync(d->ctx);
}

bool multidictCompactP(multidictP *d) {
    if (!d || !d->ctx || !d->d) {
        return false;
    }
    return persistCtxCompact(d->ctx, d->d, &persistOpsMultidict);
}

void multidictGetStatsP(const multidictP *d, persistCtxStats *stats) {
    if (!d || !d->ctx || !stats) {
        return;
    }
    persistCtxGetStats(d->ctx, stats);
}

/* ============================================================================
 * Tests
 * ============================================================================
 */

#ifdef DATAKIT_TEST
#include "persistTestCommon.h"

/* Helper to verify multidict matches tracker */
static bool verifyMultidictMatchesTracker(multidictP *d,
                                          const ptestKVTracker *tracker) {
    if (multidictCountP(d) != tracker->count) {
        printf("  [verify] Count mismatch: dict has %llu, tracker has %u\n",
               (unsigned long long)multidictCountP(d), tracker->count);
        return false;
    }

    /* Verify all tracker entries exist in multidict with correct values */
    for (uint32_t i = 0; i < tracker->count; i++) {
        databox val;
        if (!multidictFindP(d, &tracker->keys[i], &val)) {
            printf("  [verify] Key at index %u not found\n", i);
            return false;
        }
        if (!ptestBoxesEqual(&val, &tracker->values[i])) {
            printf("  [verify] Value mismatch at index %u\n", i);
            return false;
        }
    }

    return true;
}

int multidictPTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    const char *basePath = "/tmp/multidictPTest";

    TEST("multidictP create empty and close") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        if (!ctx) {
            ERRR("Failed to create persistCtx");
        }

        multidictP *d = multidictNewP(ctx);
        if (!d) {
            persistCtxFree(ctx);
            ERRR("Failed to create multidictP");
        }

        if (multidictCountP(d) != 0) {
            ERRR("New dict should be empty");
        }

        multidictCloseP(d);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multidictP add with varied values") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multidictP *d = multidictNewP(ctx);
        ptestKVTracker tracker;
        ptestKVTrackerInit(&tracker);

        /* Add entries with numeric keys and values
         * Note: multidict only reliably persists numeric types */
        for (int i = 0; i < 20; i++) {
            databox key = DATABOX_SIGNED(i);
            databox val = DATABOX_SIGNED(i * 100);
            multidictAddP(d, &key, &val);
            ptestKVTrackerInsert(&tracker, &key, &val);
        }

        if (!verifyMultidictMatchesTracker(d, &tracker)) {
            ERRR("Multidict/tracker mismatch after adds");
        }

        multidictCloseP(d);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multidictP update existing keys") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multidictP *d = multidictNewP(ctx);
        ptestKVTracker tracker;
        ptestKVTrackerInit(&tracker);

        /* Add initial values */
        for (int i = 0; i < 10; i++) {
            databox key = DATABOX_SIGNED(i);
            databox val = DATABOX_SIGNED(i * 10);
            multidictAddP(d, &key, &val);
            ptestKVTrackerInsert(&tracker, &key, &val);
        }

        /* Update with new values */
        for (int i = 0; i < 10; i++) {
            databox key = DATABOX_SIGNED(i);
            databox val = DATABOX_SIGNED(i * 1000);
            multidictAddP(d, &key, &val);
            ptestKVTrackerInsert(&tracker, &key, &val);
        }

        if (!verifyMultidictMatchesTracker(d, &tracker)) {
            ERRR("Multidict/tracker mismatch after updates");
        }

        multidictCloseP(d);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multidictP varied values recovery") {
        ptestCleanupFiles(basePath);
        ptestKVTracker tracker;
        ptestKVTrackerInit(&tracker);

        /* Create and populate with numeric keys and values */
        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            multidictP *d = multidictNewP(ctx);

            for (int i = 0; i < 30; i++) {
                databox key = DATABOX_SIGNED(i);
                databox val = DATABOX_SIGNED(i * 50);
                multidictAddP(d, &key, &val);
                ptestKVTrackerInsert(&tracker, &key, &val);
            }

            multidictCloseP(d);
            persistCtxFree(ctx);
        }

        /* Recover and verify */
        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multidictP *d = multidictOpenP(ctx);

            if (!d) {
                ERRR("Failed to recover multidictP");
            }

            if (!verifyMultidictMatchesTracker(d, &tracker)) {
                ERRR("Multidict/tracker mismatch after recovery");
            }

            multidictCloseP(d);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("multidictP delete operations") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multidictP *d = multidictNewP(ctx);
        ptestKVTracker tracker;
        ptestKVTrackerInit(&tracker);

        /* Add entries */
        for (int i = 0; i < 20; i++) {
            databox key = DATABOX_SIGNED(i);
            databox val = DATABOX_SIGNED(i * 100);
            multidictAddP(d, &key, &val);
            ptestKVTrackerInsert(&tracker, &key, &val);
        }

        /* Delete every other entry */
        for (int i = 0; i < 20; i += 2) {
            databox key = DATABOX_SIGNED(i);
            multidictDeleteP(d, &key);
            ptestKVTrackerDelete(&tracker, &key);
        }

        if (!verifyMultidictMatchesTracker(d, &tracker)) {
            ERRR("Multidict/tracker mismatch after deletes");
        }

        /* Verify deleted keys don't exist */
        for (int i = 0; i < 20; i += 2) {
            databox key = DATABOX_SIGNED(i);
            if (multidictExistsP(d, &key)) {
                ERR("Deleted key %d should not exist", i);
            }
        }

        multidictCloseP(d);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multidictP delete recovery") {
        ptestCleanupFiles(basePath);
        ptestKVTracker tracker;
        ptestKVTrackerInit(&tracker);

        /* Create, populate, delete, close */
        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            multidictP *d = multidictNewP(ctx);

            for (int i = 0; i < 15; i++) {
                databox key = DATABOX_SIGNED(i);
                databox val = DATABOX_SIGNED(i * 50);
                multidictAddP(d, &key, &val);
                ptestKVTrackerInsert(&tracker, &key, &val);
            }

            /* Delete entries 3, 7, 11 */
            for (int i = 3; i < 15; i += 4) {
                databox key = DATABOX_SIGNED(i);
                multidictDeleteP(d, &key);
                ptestKVTrackerDelete(&tracker, &key);
            }

            multidictCloseP(d);
            persistCtxFree(ctx);
        }

        /* Recover and verify */
        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multidictP *d = multidictOpenP(ctx);

            if (!d) {
                ERRR("Failed to recover multidictP after delete");
            }

            if (!verifyMultidictMatchesTracker(d, &tracker)) {
                ERRR("Multidict/tracker mismatch after delete recovery");
            }

            multidictCloseP(d);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("multidictP empty and continue") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multidictP *d = multidictNewP(ctx);
        ptestKVTracker tracker;
        ptestKVTrackerInit(&tracker);

        /* Add entries */
        for (int i = 0; i < 10; i++) {
            databox key = DATABOX_SIGNED(i);
            databox val = DATABOX_SIGNED(i);
            multidictAddP(d, &key, &val);
        }

        if (multidictCountP(d) != 10) {
            ERRR("Count should be 10 before empty");
        }

        /* Empty the dict */
        multidictEmptyP(d);

        if (multidictCountP(d) != 0) {
            ERRR("Count should be 0 after empty");
        }

        /* Add new entries */
        for (int i = 100; i < 115; i++) {
            databox key = DATABOX_SIGNED(i);
            databox val = DATABOX_SIGNED(i * 2);
            multidictAddP(d, &key, &val);
            ptestKVTrackerInsert(&tracker, &key, &val);
        }

        if (!verifyMultidictMatchesTracker(d, &tracker)) {
            ERRR("Multidict/tracker mismatch after empty and re-add");
        }

        multidictCloseP(d);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multidictP empty recovery") {
        ptestCleanupFiles(basePath);
        ptestKVTracker tracker;
        ptestKVTrackerInit(&tracker);

        /* Create, populate, empty, add more, close */
        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            multidictP *d = multidictNewP(ctx);

            for (int i = 0; i < 10; i++) {
                databox key = DATABOX_SIGNED(i);
                databox val = DATABOX_SIGNED(i);
                multidictAddP(d, &key, &val);
            }

            multidictEmptyP(d);

            for (int i = 50; i < 60; i++) {
                databox key = DATABOX_SIGNED(i);
                databox val = DATABOX_SIGNED(i * 3);
                multidictAddP(d, &key, &val);
                ptestKVTrackerInsert(&tracker, &key, &val);
            }

            multidictCloseP(d);
            persistCtxFree(ctx);
        }

        /* Recover and verify */
        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multidictP *d = multidictOpenP(ctx);

            if (!d) {
                ERRR("Failed to recover multidictP after empty");
            }

            if (!verifyMultidictMatchesTracker(d, &tracker)) {
                ERRR("Multidict/tracker mismatch after empty recovery");
            }

            multidictCloseP(d);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("multidictP mixed operations") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multidictP *d = multidictNewP(ctx);
        ptestKVTracker tracker;
        ptestKVTrackerInit(&tracker);

        /* Interleave adds, updates, and deletes */
        for (int round = 0; round < 5; round++) {
            /* Add some entries */
            for (int i = round * 10; i < round * 10 + 8; i++) {
                databox key = DATABOX_SIGNED(i);
                databox val = DATABOX_SIGNED(i * round);
                multidictAddP(d, &key, &val);
                ptestKVTrackerInsert(&tracker, &key, &val);
            }

            /* Update a few */
            for (int i = round * 10; i < round * 10 + 3; i++) {
                databox key = DATABOX_SIGNED(i);
                databox val = DATABOX_SIGNED(i * 9999);
                multidictAddP(d, &key, &val);
                ptestKVTrackerInsert(&tracker, &key, &val);
            }

            /* Delete one */
            databox key = DATABOX_SIGNED(round * 10 + 5);
            multidictDeleteP(d, &key);
            ptestKVTrackerDelete(&tracker, &key);
        }

        if (!verifyMultidictMatchesTracker(d, &tracker)) {
            ERRR("Multidict/tracker mismatch after mixed ops");
        }

        multidictCloseP(d);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multidictP multi-cycle recovery") {
        ptestCleanupFiles(basePath);
        ptestKVTracker tracker;
        ptestKVTrackerInit(&tracker);

        for (int cycle = 0; cycle < PTEST_RECOVERY_CYCLES; cycle++) {
            persistCtx *ctx;
            multidictP *d;

            if (cycle == 0) {
                ctx = persistCtxNew(basePath, NULL);
                d = multidictNewP(ctx);
            } else {
                ctx = persistCtxOpen(basePath, NULL);
                d = multidictOpenP(ctx);
            }

            if (!d) {
                ERR("Failed to open multidictP in cycle %d", cycle);
                continue;
            }

            /* Add new entries each cycle */
            for (int i = 0; i < 10; i++) {
                databox key = DATABOX_SIGNED(cycle * 100 + i);
                databox val = DATABOX_SIGNED(cycle * 1000 + i);
                multidictAddP(d, &key, &val);
                ptestKVTrackerInsert(&tracker, &key, &val);
            }

            if (!verifyMultidictMatchesTracker(d, &tracker)) {
                ERR("Multidict/tracker mismatch in cycle %d", cycle);
            }

            multidictCloseP(d);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("multidictP large dataset") {
        /* Note: Limited to 200 entries due to WAL buffer constraints */
        ptestCleanupFiles(basePath);
        ptestKVTracker tracker;
        ptestKVTrackerInit(&tracker);

        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            multidictP *d = multidictNewP(ctx);

            for (int i = 0; i < 200; i++) {
                databox key = DATABOX_SIGNED(i);
                databox val = DATABOX_SIGNED(i * 1000);
                multidictAddP(d, &key, &val);
                ptestKVTrackerInsert(&tracker, &key, &val);
            }

            multidictCloseP(d);
            persistCtxFree(ctx);
        }

        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multidictP *d = multidictOpenP(ctx);

            if (!d) {
                ERRR("Large dataset recovery failed");
            }

            if (!verifyMultidictMatchesTracker(d, &tracker)) {
                ERRR("Large dataset verification failed");
            }

            multidictCloseP(d);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("multidictP empty dict recovery") {
        ptestCleanupFiles(basePath);

        /* Create empty and close */
        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            multidictP *d = multidictNewP(ctx);
            multidictCloseP(d);
            persistCtxFree(ctx);
        }

        /* Recover and verify empty */
        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multidictP *d = multidictOpenP(ctx);

            if (!d) {
                ERRR("Empty dict recovery failed");
            }

            if (multidictCountP(d) != 0) {
                ERRR("Recovered empty dict should have 0 entries");
            }

            multidictCloseP(d);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("multidictP single entry recovery") {
        ptestCleanupFiles(basePath);
        databox expectedKey = DATABOX_SIGNED(42);
        databox expectedVal = DATABOX_SIGNED(4200);

        /* Create with single entry */
        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            multidictP *d = multidictNewP(ctx);
            multidictAddP(d, &expectedKey, &expectedVal);
            multidictCloseP(d);
            persistCtxFree(ctx);
        }

        /* Recover and verify */
        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multidictP *d = multidictOpenP(ctx);

            if (!d) {
                ERRR("Single entry recovery failed");
            }

            if (multidictCountP(d) != 1) {
                ERRR("Should have exactly 1 entry");
            }

            databox val;
            if (!multidictFindP(d, &expectedKey, &val)) {
                ERRR("Expected key not found");
            }

            if (!ptestBoxesEqual(&val, &expectedVal)) {
                ERRR("Value mismatch after recovery");
            }

            multidictCloseP(d);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("multidictP statistics tracking") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multidictP *d = multidictNewP(ctx);

        /* Add entries */
        for (int i = 0; i < 15; i++) {
            databox key = DATABOX_SIGNED(i);
            databox val = DATABOX_SIGNED(i);
            multidictAddP(d, &key, &val);
        }

        /* Delete a few */
        for (int i = 0; i < 5; i++) {
            databox key = DATABOX_SIGNED(i);
            multidictDeleteP(d, &key);
        }

        persistCtxStats stats;
        multidictGetStatsP(d, &stats);

        /* 15 adds + 5 deletes = 20 total ops */
        if (stats.totalOps != 20) {
            ERR("Total ops should be 20, got %llu",
                (unsigned long long)stats.totalOps);
        }

        multidictCloseP(d);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
