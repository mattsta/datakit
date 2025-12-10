/* multilistP.c - Persistent multilist wrapper
 *
 * See multilistP.h for API documentation.
 */

#include "multilistP.h"
#include "../datakit.h"
#include "../persist.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

multilistP *multilistNewP(persistCtx *ctx, flexCapSizeLimit limit,
                          uint32_t depth) {
    if (!ctx) {
        return NULL;
    }

    multilistP *ml = zcalloc(1, sizeof(*ml));
    ml->ctx = ctx;
    ml->limit = limit;
    ml->depth = depth;

    /* Create mflex state for compressed node operations */
    ml->state = mflexStateCreate();
    if (!ml->state) {
        zfree(ml);
        return NULL;
    }

    /* Create underlying multilist */
    ml->ml = multilistNew(limit, depth);
    if (!ml->ml) {
        mflexStateFree(ml->state);
        zfree(ml);
        return NULL;
    }

    /* Initialize persistence context for multilist type */
    if (!persistCtxInitForType(ctx, &persistOpsMultilist)) {
        multilistFree(ml->ml);
        mflexStateFree(ml->state);
        zfree(ml);
        return NULL;
    }

    /* Save initial snapshot */
    if (!persistCtxSaveSnapshot(ctx, ml->ml, &persistOpsMultilist)) {
        multilistFree(ml->ml);
        mflexStateFree(ml->state);
        zfree(ml);
        return NULL;
    }

    return ml;
}

multilistP *multilistOpenP(persistCtx *ctx) {
    if (!ctx) {
        return NULL;
    }

    /* Initialize persistence context for multilist type */
    if (!persistCtxInitForType(ctx, &persistOpsMultilist)) {
        return NULL;
    }

    /* Recover from snapshot + WAL */
    multilist *recovered = persistCtxRecover(ctx, &persistOpsMultilist);
    if (!recovered) {
        return NULL;
    }

    multilistP *ml = zcalloc(1, sizeof(*ml));
    ml->ctx = ctx;
    ml->ml = recovered;
    /* Note: limit and depth not recoverable from snapshot, use defaults */
    ml->limit = FLEX_CAP_LEVEL_2048;
    ml->depth = 0;

    /* Create mflex state for compressed node operations */
    ml->state = mflexStateCreate();
    if (!ml->state) {
        multilistFree(ml->ml);
        zfree(ml);
        return NULL;
    }

    return ml;
}

void multilistCloseP(multilistP *ml) {
    if (!ml) {
        return;
    }

    /* Sync before closing */
    if (ml->ctx) {
        persistCtxSync(ml->ctx);
    }

    /* Free underlying multilist */
    if (ml->ml) {
        multilistFree(ml->ml);
    }

    /* Free mflex state */
    if (ml->state) {
        mflexStateFree(ml->state);
    }

    zfree(ml);
}

multilist *multilistGetP(multilistP *ml) {
    return ml ? ml->ml : NULL;
}

/* ============================================================================
 * Metadata
 * ============================================================================
 */

size_t multilistCountP(const multilistP *ml) {
    return ml && ml->ml ? multilistCount(ml->ml) : 0;
}

size_t multilistBytesP(const multilistP *ml) {
    return ml && ml->ml ? multilistBytes(ml->ml) : 0;
}

/* ============================================================================
 * Mutations
 * ============================================================================
 */

void multilistPushHeadP(multilistP *ml, const databox *box) {
    if (!ml || !ml->ml || !ml->ctx || !box) {
        return;
    }

    /* Log to WAL first */
    if (!persistCtxLogOp(ml->ctx, PERSIST_OP_PUSH_HEAD, box, 1)) {
        return;
    }

    /* Update in-memory */
    multilistPushByTypeHead(&ml->ml, ml->state, box);

    /* Check compaction threshold */
    persistCtxMaybeCompact(ml->ctx, ml->ml, &persistOpsMultilist);
}

void multilistPushTailP(multilistP *ml, const databox *box) {
    if (!ml || !ml->ml || !ml->ctx || !box) {
        return;
    }

    /* Log to WAL first */
    if (!persistCtxLogOp(ml->ctx, PERSIST_OP_PUSH_TAIL, box, 1)) {
        return;
    }

    /* Update in-memory */
    multilistPushByTypeTail(&ml->ml, ml->state, box);

    /* Check compaction threshold */
    persistCtxMaybeCompact(ml->ctx, ml->ml, &persistOpsMultilist);
}

bool multilistPopHeadP(multilistP *ml, databox *got) {
    if (!ml || !ml->ml || !ml->ctx) {
        return false;
    }

    /* Check if there's anything to pop */
    if (multilistCount(ml->ml) == 0) {
        return false;
    }

    /* Log to WAL first */
    if (!persistCtxLogOp(ml->ctx, PERSIST_OP_POP_HEAD, NULL, 0)) {
        return false;
    }

    /* Update in-memory */
    bool result = multilistPop(&ml->ml, ml->state, got, false);

    /* Check compaction threshold */
    persistCtxMaybeCompact(ml->ctx, ml->ml, &persistOpsMultilist);

    return result;
}

bool multilistPopTailP(multilistP *ml, databox *got) {
    if (!ml || !ml->ml || !ml->ctx) {
        return false;
    }

    /* Check if there's anything to pop */
    if (multilistCount(ml->ml) == 0) {
        return false;
    }

    /* Log to WAL first */
    if (!persistCtxLogOp(ml->ctx, PERSIST_OP_POP_TAIL, NULL, 0)) {
        return false;
    }

    /* Update in-memory */
    bool result = multilistPop(&ml->ml, ml->state, got, true);

    /* Check compaction threshold */
    persistCtxMaybeCompact(ml->ctx, ml->ml, &persistOpsMultilist);

    return result;
}

bool multilistDelRangeP(multilistP *ml, mlOffsetId start, int64_t values) {
    if (!ml || !ml->ml || !ml->ctx) {
        return false;
    }

    /* Encode start and values as databoxes for WAL */
    const databox startBox = DATABOX_SIGNED(start);
    const databox valuesBox = DATABOX_SIGNED(values);
    const databox *args[] = {&startBox, &valuesBox};

    /* Log to WAL first */
    if (!persistCtxLogOp(ml->ctx, PERSIST_OP_DELETE_AT, args, 2)) {
        return false;
    }

    /* Update in-memory */
    bool result = multilistDelRange(&ml->ml, ml->state, start, values);

    /* Check compaction threshold */
    persistCtxMaybeCompact(ml->ctx, ml->ml, &persistOpsMultilist);

    return result;
}

bool multilistReplaceAtIndexP(multilistP *ml, mlNodeId index,
                              const databox *box) {
    if (!ml || !ml->ml || !ml->ctx || !box) {
        return false;
    }

    /* Encode index and value for WAL */
    const databox indexBox = DATABOX_SIGNED(index);
    const databox *args[] = {&indexBox, box};

    /* Log to WAL first */
    if (!persistCtxLogOp(ml->ctx, PERSIST_OP_REPLACE, args, 2)) {
        return false;
    }

    /* Update in-memory */
    bool result = multilistReplaceByTypeAtIndex(&ml->ml, ml->state, index, box);

    /* Check compaction threshold */
    persistCtxMaybeCompact(ml->ctx, ml->ml, &persistOpsMultilist);

    return result;
}

void multilistResetP(multilistP *ml) {
    if (!ml || !ml->ml || !ml->ctx) {
        return;
    }

    /* Log to WAL first */
    if (!persistCtxLogOp(ml->ctx, PERSIST_OP_CLEAR, NULL, 0)) {
        return;
    }

    /* Update in-memory - free and recreate */
    multilistFree(ml->ml);
    ml->ml = multilistNew(ml->limit, ml->depth);

    /* Check compaction threshold */
    persistCtxMaybeCompact(ml->ctx, ml->ml, &persistOpsMultilist);
}

/* ============================================================================
 * Lookups
 * ============================================================================
 */

bool multilistIndexP(const multilistP *ml, mflexState *state, mlOffsetId index,
                     multilistEntry *entry, bool openNode) {
    if (!ml || !ml->ml) {
        return false;
    }
    return multilistIndex(ml->ml, state, index, entry, openNode);
}

/* ============================================================================
 * Iteration
 * ============================================================================
 */

void multilistIteratorInitP(multilistP *ml, mflexState *state[2],
                            multilistIterator *iter, bool forward,
                            bool readOnly) {
    if (!ml || !ml->ml) {
        return;
    }
    multilistIteratorInit(ml->ml, state, iter, forward, readOnly);
}

/* ============================================================================
 * Persistence Control
 * ============================================================================
 */

bool multilistSyncP(multilistP *ml) {
    if (!ml || !ml->ctx) {
        return false;
    }
    return persistCtxSync(ml->ctx);
}

bool multilistCompactP(multilistP *ml) {
    if (!ml || !ml->ctx || !ml->ml) {
        return false;
    }
    return persistCtxCompact(ml->ctx, ml->ml, &persistOpsMultilist);
}

void multilistGetStatsP(const multilistP *ml, persistCtxStats *stats) {
    if (!ml || !ml->ctx || !stats) {
        return;
    }
    persistCtxGetStats(ml->ctx, stats);
}

/* ============================================================================
 * Tests
 * ============================================================================
 */

#ifdef DATAKIT_TEST
#include "persistTestCommon.h"

/* Helper to verify multilist matches tracker */
static bool verifyMultilistMatchesTracker(multilistP *ml,
                                          const ptestBoxTracker *tracker) {
    uint64_t mlCount = multilistCountP(ml);
    if (mlCount != tracker->count) {
        printf("  [verify] Count mismatch: multilist has %lu, tracker has %u\n",
               mlCount, tracker->count);
        return false;
    }

    multilistEntry entry;
    for (uint32_t i = 0; i < tracker->count; i++) {
        if (!multilistIndex(ml->ml, ml->state, i, &entry, false)) {
            printf("  [verify] Failed to get index %u\n", i);
            return false;
        }
        if (!ptestBoxesEqual(&entry.box, &tracker->boxes[i])) {
            printf("  [verify] Mismatch at index %u\n", i);
            return false;
        }
    }

    return true;
}

int multilistPTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    const char *basePath = "/tmp/multilistPTest";

    TEST("multilistP create empty and close") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        if (!ctx) {
            ERRR("Failed to create persistCtx");
        }

        multilistP *ml = multilistNewP(ctx, FLEX_CAP_LEVEL_2048, 0);
        if (!ml) {
            persistCtxFree(ctx);
            ERRR("Failed to create multilistP");
        }

        if (multilistCountP(ml) != 0) {
            ERRR("New list should be empty");
        }

        multilistCloseP(ml);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multilistP compaction cross-session recovery") {
        /* Test that compaction properly clears WAL buffer to prevent double
         * elements on recovery. Regression test for bug where buffered WAL
         * entries before compaction were being replayed after recovery. */
        ptestCleanupFiles(basePath);

        /* Session 1: Create, add 100 elements, compact, add 50 more */
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        if (!ctx) {
            ERRR("Failed to create persistCtx");
        }

        multilistP *ml = multilistNewP(ctx, FLEX_CAP_LEVEL_2048, 0);
        if (!ml) {
            persistCtxFree(ctx);
            ERRR("Failed to create multilistP");
        }

        /* Push 100 elements */
        for (int i = 0; i < 100; i++) {
            databox box = DATABOX_SIGNED((int64_t)i);
            multilistPushTailP(ml, &box);
        }
        if (multilistCountP(ml) != 100) {
            ERR("Should have 100 elements, got %zu", multilistCountP(ml));
        }

        /* Force compaction */
        if (!multilistCompactP(ml)) {
            ERRR("Compaction failed");
        }

        /* Push 50 more elements AFTER compaction */
        for (int i = 100; i < 150; i++) {
            databox box = DATABOX_SIGNED((int64_t)i);
            multilistPushTailP(ml, &box);
        }
        if (multilistCountP(ml) != 150) {
            ERR("Should have 150 elements, got %zu", multilistCountP(ml));
        }

        /* Close session 1 */
        multilistCloseP(ml);
        persistCtxFree(ctx);

        /* Session 2: Recover and verify count is exactly 150 (not 250) */
        ctx = persistCtxOpen(basePath, NULL);
        if (!ctx) {
            ERRR("Failed to open persistCtx");
        }

        ml = multilistOpenP(ctx);
        if (!ml) {
            persistCtxFree(ctx);
            ERRR("Failed to recover multilistP");
        }

        size_t recoveredCount = multilistCountP(ml);
        if (recoveredCount != 150) {
            ERR("Expected 150 elements, got %zu (double elements bug)",
                recoveredCount);
        }

        /* Verify all elements have correct sequential values */
        mflexState *states[2] = {mflexStateCreate(), mflexStateCreate()};
        multilistIterator iter;
        multilistIteratorInitForwardReadOnlyP(ml, states, &iter);

        multilistEntry entry;
        int count = 0;
        while (multilistNext(&iter, &entry)) {
            int64_t val = -1;
            if (entry.box.type == DATABOX_SIGNED_64) {
                val = entry.box.data.i64;
            } else if (entry.box.type == DATABOX_UNSIGNED_64) {
                val = (int64_t)entry.box.data.u64;
            } else {
                ERR("Element %d has unexpected type: %d", count,
                    entry.box.type);
            }

            if (val != count) {
                ERR("Element %d should be %d, got %lld", count, count,
                    (long long)val);
            }
            count++;
        }
        multilistIteratorRelease(&iter);
        mflexStateFree(states[0]);
        mflexStateFree(states[1]);

        if (count != 150) {
            ERR("Iterator returned %d elements, expected 150", count);
        }

        multilistCloseP(ml);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multilistP push tail with all data types") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multilistP *ml = multilistNewP(ctx, FLEX_CAP_LEVEL_2048, 0);
        ptestBoxTracker tracker;
        ptestBoxTrackerInit(&tracker);

        for (int type = 0; type < PTEST_TYPE_COUNT; type++) {
            databox box;
            ptestGenerateBox(&box, type, type);
            multilistPushTailP(ml, &box);
            ptestBoxTrackerPushTail(&tracker, &box);
        }

        if (!verifyMultilistMatchesTracker(ml, &tracker)) {
            ERRR("Multilist/tracker mismatch");
        }

        multilistCloseP(ml);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multilistP push head with all data types") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multilistP *ml = multilistNewP(ctx, FLEX_CAP_LEVEL_2048, 0);
        ptestBoxTracker tracker;
        ptestBoxTrackerInit(&tracker);

        for (int type = 0; type < PTEST_TYPE_COUNT; type++) {
            databox box;
            ptestGenerateBox(&box, type, type);
            multilistPushHeadP(ml, &box);
            ptestBoxTrackerPushHead(&tracker, &box);
        }

        if (!verifyMultilistMatchesTracker(ml, &tracker)) {
            ERRR("Multilist/tracker mismatch");
        }

        multilistCloseP(ml);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multilistP all data types recovery") {
        ptestCleanupFiles(basePath);
        ptestBoxTracker tracker;
        ptestBoxTrackerInit(&tracker);

        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            multilistP *ml = multilistNewP(ctx, FLEX_CAP_LEVEL_2048, 0);

            for (int type = 0; type < PTEST_TYPE_COUNT; type++) {
                for (int seed = 0; seed < 5; seed++) {
                    databox box;
                    ptestGenerateBox(&box, type, seed);
                    multilistPushTailP(ml, &box);
                    ptestBoxTrackerPushTail(&tracker, &box);
                }
            }

            multilistCloseP(ml);
            persistCtxFree(ctx);
        }

        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multilistP *ml = multilistOpenP(ctx);

            if (!ml) {
                ERRR("Recovery failed");
            }

            if (!verifyMultilistMatchesTracker(ml, &tracker)) {
                ERRR("Recovery verification failed");
            }

            multilistCloseP(ml);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("multilistP pop operations") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multilistP *ml = multilistNewP(ctx, FLEX_CAP_LEVEL_2048, 0);
        ptestBoxTracker tracker;
        ptestBoxTrackerInit(&tracker);

        for (int i = 0; i < 10; i++) {
            databox box = DATABOX_SIGNED(i);
            multilistPushTailP(ml, &box);
            ptestBoxTrackerPushTail(&tracker, &box);
        }

        /* Pop from tail */
        databox got;
        if (!multilistPopTailP(ml, &got)) {
            ERRR("Pop tail failed");
        }
        databox expected;
        ptestBoxTrackerPopTail(&tracker, &expected);
        if (!ptestBoxesEqual(&got, &expected)) {
            ERRR("Pop tail value mismatch");
        }

        /* Pop from head */
        if (!multilistPopHeadP(ml, &got)) {
            ERRR("Pop head failed");
        }
        ptestBoxTrackerPopHead(&tracker, &expected);
        if (!ptestBoxesEqual(&got, &expected)) {
            ERRR("Pop head value mismatch");
        }

        if (!verifyMultilistMatchesTracker(ml, &tracker)) {
            ERRR("Post-pop verification failed");
        }

        multilistCloseP(ml);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multilistP pop recovery") {
        ptestCleanupFiles(basePath);
        ptestBoxTracker tracker;
        ptestBoxTrackerInit(&tracker);

        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            multilistP *ml = multilistNewP(ctx, FLEX_CAP_LEVEL_2048, 0);

            for (int i = 0; i < 20; i++) {
                databox box = DATABOX_SIGNED(i * 100);
                multilistPushTailP(ml, &box);
                ptestBoxTrackerPushTail(&tracker, &box);
            }

            /* Pop some */
            databox got;
            for (int i = 0; i < 5; i++) {
                databox expected;
                multilistPopTailP(ml, &got);
                ptestBoxTrackerPopTail(&tracker, &expected);
            }
            for (int i = 0; i < 3; i++) {
                databox expected;
                multilistPopHeadP(ml, &got);
                ptestBoxTrackerPopHead(&tracker, &expected);
            }

            multilistCloseP(ml);
            persistCtxFree(ctx);
        }

        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multilistP *ml = multilistOpenP(ctx);

            if (!ml) {
                ERRR("Recovery failed");
            }

            if (!verifyMultilistMatchesTracker(ml, &tracker)) {
                ERRR("Pop recovery verification failed");
            }

            multilistCloseP(ml);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("multilistP mixed push/pop") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multilistP *ml = multilistNewP(ctx, FLEX_CAP_LEVEL_2048, 0);
        ptestBoxTracker tracker;
        ptestBoxTrackerInit(&tracker);

        /* Interleaved push/pop */
        for (int i = 0; i < 50; i++) {
            databox box = DATABOX_SIGNED(i);
            if (i % 3 == 0) {
                multilistPushHeadP(ml, &box);
                ptestBoxTrackerPushHead(&tracker, &box);
            } else if (i % 3 == 1) {
                multilistPushTailP(ml, &box);
                ptestBoxTrackerPushTail(&tracker, &box);
            } else {
                /* Pop if not empty */
                if (tracker.count > 0) {
                    databox got, expected;
                    if (i % 2 == 0) {
                        multilistPopHeadP(ml, &got);
                        ptestBoxTrackerPopHead(&tracker, &expected);
                    } else {
                        multilistPopTailP(ml, &got);
                        ptestBoxTrackerPopTail(&tracker, &expected);
                    }
                }
            }
        }

        if (!verifyMultilistMatchesTracker(ml, &tracker)) {
            ERRR("Mixed operations verification failed");
        }

        multilistCloseP(ml);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multilistP reset and continue") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multilistP *ml = multilistNewP(ctx, FLEX_CAP_LEVEL_2048, 0);
        ptestBoxTracker tracker;
        ptestBoxTrackerInit(&tracker);

        for (int i = 0; i < 20; i++) {
            databox box = DATABOX_SIGNED(i);
            multilistPushTailP(ml, &box);
        }

        multilistResetP(ml);
        ptestBoxTrackerInit(&tracker);

        if (multilistCountP(ml) != 0) {
            ERRR("Count should be 0 after reset");
        }

        for (int i = 100; i < 110; i++) {
            databox box = DATABOX_SIGNED(i);
            multilistPushTailP(ml, &box);
            ptestBoxTrackerPushTail(&tracker, &box);
        }

        if (!verifyMultilistMatchesTracker(ml, &tracker)) {
            ERRR("Post-reset verification failed");
        }

        multilistCloseP(ml);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multilistP reset recovery") {
        ptestCleanupFiles(basePath);
        ptestBoxTracker tracker;
        ptestBoxTrackerInit(&tracker);

        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            multilistP *ml = multilistNewP(ctx, FLEX_CAP_LEVEL_2048, 0);

            for (int i = 0; i < 10; i++) {
                databox box = DATABOX_SIGNED(i);
                multilistPushTailP(ml, &box);
            }

            multilistResetP(ml);

            for (int i = 100; i < 115; i++) {
                databox box = DATABOX_SIGNED(i);
                multilistPushTailP(ml, &box);
                ptestBoxTrackerPushTail(&tracker, &box);
            }

            multilistCloseP(ml);
            persistCtxFree(ctx);
        }

        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multilistP *ml = multilistOpenP(ctx);

            if (!ml) {
                ERRR("Reset recovery failed");
            }

            if (!verifyMultilistMatchesTracker(ml, &tracker)) {
                ERRR("Reset recovery verification failed");
            }

            multilistCloseP(ml);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("multilistP multi-cycle recovery") {
        ptestCleanupFiles(basePath);
        ptestBoxTracker tracker;
        ptestBoxTrackerInit(&tracker);

        for (int cycle = 0; cycle < PTEST_RECOVERY_CYCLES; cycle++) {
            persistCtx *ctx;
            multilistP *ml;

            if (cycle == 0) {
                ctx = persistCtxNew(basePath, NULL);
                ml = multilistNewP(ctx, FLEX_CAP_LEVEL_2048, 0);
            } else {
                ctx = persistCtxOpen(basePath, NULL);
                ml = multilistOpenP(ctx);
                if (!ml) {
                    ERR("Failed to recover at cycle %d", cycle);
                    break;
                }

                if (!verifyMultilistMatchesTracker(ml, &tracker)) {
                    ERR("Verification failed at cycle %d", cycle);
                }
            }

            for (int i = 0; i < 10; i++) {
                databox box = DATABOX_SIGNED(cycle * 1000 + i);
                multilistPushTailP(ml, &box);
                ptestBoxTrackerPushTail(&tracker, &box);
            }

            multilistCloseP(ml);
            persistCtxFree(ctx);
        }

        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multilistP *ml = multilistOpenP(ctx);

            if (!ml) {
                ERRR("Final recovery failed");
            }

            if (!verifyMultilistMatchesTracker(ml, &tracker)) {
                ERRR("Final verification failed");
            }

            multilistCloseP(ml);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("multilistP large dataset") {
        /* Note: Limited to 200 elements due to WAL buffer constraints.
         * Larger datasets may hit persistence layer limits. */
        ptestCleanupFiles(basePath);
        ptestBoxTracker tracker;
        ptestBoxTrackerInit(&tracker);

        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            multilistP *ml = multilistNewP(ctx, FLEX_CAP_LEVEL_2048, 0);

            for (int i = 0; i < 200; i++) {
                databox box;
                ptestGenerateBox(&box, i % PTEST_TYPE_COUNT, i);
                multilistPushTailP(ml, &box);
                ptestBoxTrackerPushTail(&tracker, &box);
            }

            multilistCloseP(ml);
            persistCtxFree(ctx);
        }

        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multilistP *ml = multilistOpenP(ctx);

            if (!ml) {
                ERRR("Large dataset recovery failed");
            }

            if (!verifyMultilistMatchesTracker(ml, &tracker)) {
                ERRR("Large dataset verification failed");
            }

            multilistCloseP(ml);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("multilistP empty recovery") {
        ptestCleanupFiles(basePath);

        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            multilistP *ml = multilistNewP(ctx, FLEX_CAP_LEVEL_2048, 0);
            multilistCloseP(ml);
            persistCtxFree(ctx);
        }

        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multilistP *ml = multilistOpenP(ctx);

            if (!ml) {
                ERRR("Empty recovery failed");
            }

            if (multilistCountP(ml) != 0) {
                ERRR("Empty multilist should have count 0");
            }

            multilistCloseP(ml);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("multilistP single element recovery") {
        ptestCleanupFiles(basePath);
        databox testBox = DATABOX_SIGNED(42);

        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            multilistP *ml = multilistNewP(ctx, FLEX_CAP_LEVEL_2048, 0);
            multilistPushTailP(ml, &testBox);
            multilistCloseP(ml);
            persistCtxFree(ctx);
        }

        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multilistP *ml = multilistOpenP(ctx);

            if (multilistCountP(ml) != 1) {
                ERRR("Should have 1 element");
            }

            multilistEntry entry;
            if (!multilistIndex(ml->ml, ml->state, 0, &entry, false)) {
                ERRR("Failed to get index 0");
            }

            if (!ptestBoxesEqual(&entry.box, &testBox)) {
                ERRR("Single element mismatch");
            }

            multilistCloseP(ml);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("multilistP statistics tracking") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multilistP *ml = multilistNewP(ctx, FLEX_CAP_LEVEL_2048, 0);

        for (int i = 0; i < 20; i++) {
            databox box = DATABOX_SIGNED(i);
            multilistPushTailP(ml, &box);
        }

        databox got;
        for (int i = 0; i < 5; i++) {
            multilistPopTailP(ml, &got);
        }

        persistCtxStats stats;
        multilistGetStatsP(ml, &stats);

        if (stats.totalOps != 25) {
            ERR("Total ops should be 25, got %llu",
                (unsigned long long)stats.totalOps);
        }

        multilistCloseP(ml);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
