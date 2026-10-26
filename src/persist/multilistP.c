/* multilistP.c - Persistent multilist wrapper
 *
 * See multilistP.h for API documentation.
 */

#include "multilistP.h"
#include "../cleaner.h"
#include "../datakit.h"
#include "../persist.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

defcleanerName(multilistP *, zfree, MultilistP);

/* Scope-exit cleaner for the fixed 2-slot mflex state array the read-only
 * iterator requires: frees whichever slots were created. */
DK_FN_UNUSED static void freeMflexStates2_cleanup(mflexState *(*arr)[2]) {
    for (size_t i = 0; i < 2; i++) {
        if ((*arr)[i]) {
            mflexStateFree((*arr)[i]);
        }
    }
}

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

multilistP *multilistNewP(persistCtx *ctx, flexCapSizeLimit limit,
                          uint32_t depth) {
    if (!ctx) {
        return NULL;
    }

    cleanerName(zfree, MultilistP) multilistP *ml = zcalloc(1, sizeof(*ml));
    ml->ctx = ctx;

    /* Create mflex state for compressed node operations */
    /* @own: container; free=mflexStateFree */
    ml->state = mflexStateCreate();
    if (!ml->state) {
        return NULL;
    }

    /* Create underlying multilist */
    /* @own: container; free=multilistFree */
    ml->ml = multilistNewLimit(limit, depth);
    if (!ml->ml) {
        mflexStateFree(ml->state);
        return NULL;
    }

    /* Initialize persistence context for multilist type */
    if (!persistCtxInitForType(ctx, &persistOpsMultilist)) {
        multilistFree(ml->ml);
        mflexStateFree(ml->state);
        return NULL;
    }

    /* Save initial snapshot */
    if (!persistCtxSaveSnapshot(ctx, ml->ml, &persistOpsMultilist)) {
        multilistFree(ml->ml);
        mflexStateFree(ml->state);
        return NULL;
    }

    persistCtxClaim(ctx);
    multilistP *out = ml;
    ml = NULL;
    return out;
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

    cleanerName(zfree, MultilistP) multilistP *ml = zcalloc(1, sizeof(*ml));
    ml->ctx = ctx;
    ml->ml = recovered;

    /* Create mflex state for compressed node operations */
    /* @own: container; free=mflexStateFree */
    ml->state = mflexStateCreate();
    if (!ml->state) {
        multilistFree(ml->ml);
        return NULL;
    }

    persistCtxClaim(ctx);
    multilistP *out = ml;
    ml = NULL;
    return out;
}

void multilistCloseP(multilistP *ml) {
    if (!ml) {
        return;
    }

    /* Sync before closing; the context is free for another wrapper */
    if (ml->ctx) {
        persistCtxSync(ml->ctx);
        persistCtxRelease(ml->ctx);
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

static bool multilistLoggableP(const multilistP *ml) {
    return ml && ml->ml && ml->ctx;
}

static persistLogResult multilistPushP(multilistP *ml, const databox *box,
                                       bool tail) {
    if (!multilistLoggableP(ml) || !box) {
        return PERSIST_LOG_REFUSED;
    }

    const persistLogResult logged = persistCtxLogOp(
        ml->ctx, tail ? PERSIST_OP_PUSH_TAIL : PERSIST_OP_PUSH_HEAD, box, 1);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }
    if (tail) {
        multilistPushByTypeTail(&ml->ml, ml->state, box);
    } else {
        multilistPushByTypeHead(&ml->ml, ml->state, box);
    }
    persistCtxMaybeCompact(ml->ctx, ml->ml, &persistOpsMultilist);
    return logged;
}

persistLogResult multilistPushHeadP(multilistP *ml, const databox *box) {
    return multilistPushP(ml, box, false);
}

persistLogResult multilistPushTailP(multilistP *ml, const databox *box) {
    return multilistPushP(ml, box, true);
}

static persistLogResult multilistPopP(multilistP *ml, databox *got, bool tail) {
    if (!multilistLoggableP(ml)) {
        return PERSIST_LOG_REFUSED;
    }
    if (multilistCount(ml->ml) == 0) {
        return PERSIST_LOG_UNCHANGED;
    }

    const persistLogResult logged = persistCtxLogOp(
        ml->ctx, tail ? PERSIST_OP_POP_TAIL : PERSIST_OP_POP_HEAD, NULL, 0);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }
    multilistPop(&ml->ml, ml->state, got, tail);
    persistCtxMaybeCompact(ml->ctx, ml->ml, &persistOpsMultilist);
    return logged;
}

persistLogResult multilistPopHeadP(multilistP *ml, databox *got) {
    return multilistPopP(ml, got, false);
}

persistLogResult multilistPopTailP(multilistP *ml, databox *got) {
    return multilistPopP(ml, got, true);
}

persistLogResult multilistDelRangeP(multilistP *ml, mlOffsetId start,
                                    int64_t values) {
    if (!multilistLoggableP(ml)) {
        return PERSIST_LOG_REFUSED;
    }

    /* A range that deletes nothing logs nothing, so the WAL only holds
     * operations that change the list */
    const mlOffsetId count = (mlOffsetId)multilistCount(ml->ml);
    if (values <= 0 || start >= count || start < -count) {
        return PERSIST_LOG_UNCHANGED;
    }

    const databox startBox = DATABOX_SIGNED(start);
    const databox valuesBox = DATABOX_SIGNED(values);
    const databox *args[] = {&startBox, &valuesBox};
    const persistLogResult logged =
        persistCtxLogOp(ml->ctx, PERSIST_OP_DELETE_AT, args, 2);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }
    multilistDelRange(&ml->ml, ml->state, start, values);
    persistCtxMaybeCompact(ml->ctx, ml->ml, &persistOpsMultilist);
    return logged;
}

persistLogResult multilistReplaceAtIndexP(multilistP *ml, mlOffsetId index,
                                          const databox *box) {
    if (!multilistLoggableP(ml) || !box) {
        return PERSIST_LOG_REFUSED;
    }

    /* An index outside the list is refused before it is logged */
    const mlOffsetId count = (mlOffsetId)multilistCount(ml->ml);
    if (index >= count || index < -count) {
        return PERSIST_LOG_REFUSED;
    }

    const databox indexBox = DATABOX_SIGNED(index);
    const databox *args[] = {&indexBox, box};
    const persistLogResult logged =
        persistCtxLogOp(ml->ctx, PERSIST_OP_REPLACE, args, 2);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }
    multilistReplaceByTypeAtIndex(&ml->ml, ml->state, index, box);
    persistCtxMaybeCompact(ml->ctx, ml->ml, &persistOpsMultilist);
    return logged;
}

persistLogResult multilistResetP(multilistP *ml) {
    if (!multilistLoggableP(ml)) {
        return PERSIST_LOG_REFUSED;
    }

    const persistLogResult logged =
        persistCtxLogOp(ml->ctx, PERSIST_OP_CLEAR, NULL, 0);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }
    /* Empty, keeping the configuration */
    multilistReset(&ml->ml);
    persistCtxMaybeCompact(ml->ctx, ml->ml, &persistOpsMultilist);
    return logged;
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

defcleaner(mflexState *, mflexStateFree);

/* Helper to verify multilist matches tracker */
static bool verifyMultilistMatchesTracker(multilistP *ml,
                                          const ptestBoxTracker *tracker) {
    uint64_t mlCount = multilistCountP(ml);
    if (mlCount != tracker->count) {
        printf("  [verify] Count mismatch: multilist has %" PRIu64
               ", tracker has %u\n",
               mlCount, tracker->count);
        return false;
    }

    /* The Wide read, so a 96/128-bit element compares by value. */
    multilistEntry entry;
    databoxWide wide;
    for (uint32_t i = 0; i < tracker->count; i++) {
        if (!multilistIndexWide(ml->ml, ml->state, i, &entry, &wide)) {
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

/* Element 'i' of the wide-element test, built in 'storage' (and 'str' for
 * a string), which must outlive every use of the returned box: positive
 * and negative 96/128-bit values around each limit, strings, and 64-bit
 * values. */
static const databox *mlpWideValue(uint32_t i, databoxBig *storage, char *str,
                                   size_t cap) {
    const __uint128_t one = 1;
    memset(storage, 0, sizeof(*storage));
    switch (i % 4) {
    case 0: {
        const __uint128_t positive[] = {one << 64, (one << 95) - 1, one << 95,
                                        one << 127, ~(__uint128_t)0};
        DATABOX_BIG_UNSIGNED_128(storage, positive[(i / 4) % 5]);
        break;
    }
    case 1: {
        const __int128_t negative[] = {-((__int128_t)1 << 63) - 1,
                                       -((__int128_t)1 << 95),
                                       (__int128_t)(one << 127)};
        DATABOX_BIG_SIGNED_128(storage, negative[(i / 4) % 3]);
        break;
    }
    case 2: {
        const int n = snprintf(str, cap, "mlp-wide-%" PRIu32 "-padding", i);
        *(databox *)storage = databoxNewBytes(str, (size_t)n);
        break;
    }
    default:
        *(databox *)storage = (i / 4) % 2 ? databoxNewSigned(-(int64_t)i * 7919)
                                          : databoxNewUnsigned(UINT64_MAX - i);
        break;
    }

    return (const databox *)storage;
}

/* Iterate 'ml' forward through 0xFF-poisoned entries and compare it, in
 * order, with the tracker: every wide element must come back whole. */
static bool verifyMultilistIterMatchesTracker(multilistP *ml,
                                              const ptestBoxTracker *tracker) {
    cleaner(freeMflexStates2)
        mflexState *states[2] = {mflexStateCreate(), mflexStateCreate()};
    multilistIterator iter;
    memset(&iter, 0xFF, sizeof(iter));
    multilistIteratorInitForwardReadOnlyP(ml, states, &iter);
    uint32_t seen = 0;
    multilistEntry entry;
    for (;;) {
        memset(&entry, 0xFF, sizeof(entry));
        if (!multilistNext(&iter, &entry)) {
            break;
        }

        if (seen >= tracker->count ||
            !ptestBoxesEqual(&entry.box, &tracker->boxes[seen])) {
            printf("  [verify] Next mismatch at %u\n", seen);
            multilistIteratorRelease(&iter);
            return false;
        }

        seen++;
    }

    multilistIteratorRelease(&iter);
    if (seen != tracker->count) {
        printf("  [verify] Next saw %u of %u\n", seen, tracker->count);
        return false;
    }

    return true;
}

/* The three data modes of the matrix test. */
typedef enum mlpData {
    MLP_DATA_NORMAL = 0, /* 64-bit values, short and long strings */
    MLP_DATA_EXTENDED,   /* 96/128-bit values only */
    MLP_DATA_MIXED,      /* all of the above, interleaved */
    MLP_DATA_COUNT
} mlpData;

/* Element 'i' of data mode 'data', built like mlpWideValue.  Short
 * strings (8 bytes or less) are boxed as BYTES, as a caller would. */
static const databox *mlpDataValue(mlpData data, uint32_t i,
                                   databoxBig *storage, char *str, size_t cap) {
    const bool shortStr = data == MLP_DATA_NORMAL  ? i % 4 == 0
                          : data == MLP_DATA_MIXED ? i % 5 == 4
                                                   : false;
    if (shortStr) {
        memset(storage, 0, sizeof(*storage));
        const int n = snprintf(str, cap, "s%" PRIu32, i % 1'000'000);
        *(databox *)storage = databoxNewBytes(str, (size_t)n);
        return (const databox *)storage;
    }

    switch (data) {
    case MLP_DATA_NORMAL:
        /* mlpWideValue classes 2 (string) and 3 (64-bit) */
        return mlpWideValue(4 * i + 2 + (i % 2), storage, str, cap);
    case MLP_DATA_EXTENDED:
        /* mlpWideValue classes 0 and 1 (positive and negative wide) */
        return mlpWideValue(4 * i + (i % 2), storage, str, cap);
    default:
        return mlpWideValue(i, storage, str, cap);
    }
}

int multilistPTest(int, char *[]) {
    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multilistPTest", basePath, sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multilistP create empty and close") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        if (!ctx) {
            ERR("Failed to create persistCtx");
        }

        multilistP *ml = multilistNewP(ctx, FLEX_CAP_LEVEL_2048, 0);
        if (!ml) {
            persistCtxFree(ctx);
            ERR("Failed to create multilistP");
        }

        if (multilistCountP(ml) != 0) {
            ERR("New list should be empty");
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
            ERR("Failed to create persistCtx");
        }

        multilistP *ml = multilistNewP(ctx, FLEX_CAP_LEVEL_2048, 0);
        if (!ml) {
            persistCtxFree(ctx);
            ERR("Failed to create multilistP");
        }

        /* Push 100 elements */
        for (int i = 0; i < 100; i++) {
            databox box = DATABOX_SIGNED((int64_t)i);
            PTEST_LOGGED(multilistPushTailP(ml, &box));
        }
        if (multilistCountP(ml) != 100) {
            ERR("Should have 100 elements, got %zu", multilistCountP(ml));
        }

        /* Force compaction */
        if (!multilistCompactP(ml)) {
            ERR("Compaction failed");
        }

        /* Push 50 more elements AFTER compaction */
        for (int i = 100; i < 150; i++) {
            databox box = DATABOX_SIGNED((int64_t)i);
            PTEST_LOGGED(multilistPushTailP(ml, &box));
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
            ERR("Failed to open persistCtx");
        }

        ml = multilistOpenP(ctx);
        if (!ml) {
            persistCtxFree(ctx);
            ERR("Failed to recover multilistP");
        }

        size_t recoveredCount = multilistCountP(ml);
        if (recoveredCount != 150) {
            ERR("Expected 150 elements, got %zu (double elements bug)",
                recoveredCount);
        }

        /* Verify all elements have correct sequential values */
        cleaner(freeMflexStates2)
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
                ERR("Element %d should be %d, got %" PRId64, count, count, val);
            }
            count++;
        }
        multilistIteratorRelease(&iter);

        if (count != 150) {
            ERR("Iterator returned %d elements, expected 150", count);
        }

        multilistCloseP(ml);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multilistP refusals change nothing: a refused log, missing "
         "arguments") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multilistP *ml = multilistNewP(ctx, FLEX_CAP_LEVEL_2048, 0);
        const databox ten = DATABOX_SIGNED(10);
        const databox twenty = DATABOX_SIGNED(20);
        PTEST_LOGGED(multilistPushTailP(ml, &ten));
        PTEST_LOGGED(multilistPushTailP(ml, &twenty));
        databox popped = {};
        PTEST_EXPECT_REFUSED(
            ctx, multilistPushHeadP(ml, &ten), multilistPushTailP(ml, &ten),
            multilistPopHeadP(ml, &popped), multilistPopTailP(ml, &popped),
            multilistDelRangeP(ml, 0, 1),
            multilistReplaceAtIndexP(ml, 0, &twenty), multilistResetP(ml));
        cleaner(mflexStateFree) mflexState *state = mflexStateCreate();
        multilistEntry head = {};
        multilistEntry tail = {};
        if (multilistCountP(ml) != 2 ||
            !multilistIndexGetP(ml, state, 0, &head) ||
            !databoxEqual(&head.box, &ten) ||
            !multilistIndexGetP(ml, state, 1, &tail) ||
            !databoxEqual(&tail.box, &twenty)) {
            ERR("a refused change changed the list");
        }

        /* Missing arguments: refused or empty answers; an empty list's
         * pop changes nothing */
        persistCtxStats stats;
        multilistGetStatsP(ml, &stats);
        const uint64_t ops = stats.totalOps;
        if (multilistPushTailP(NULL, &ten) || multilistPushHeadP(ml, NULL) ||
            multilistPopHeadP(NULL, &popped) ||
            multilistDelRangeP(NULL, 0, 1) ||
            multilistReplaceAtIndexP(NULL, 0, &ten) ||
            multilistReplaceAtIndexP(ml, 0, NULL) || multilistResetP(NULL) ||
            multilistNewP(NULL, FLEX_CAP_LEVEL_2048, 0) ||
            multilistOpenP(NULL) || multilistGetP(NULL) ||
            multilistCountP(NULL) || multilistBytesP(NULL) ||
            multilistIndexGetP(NULL, state, 0, &head) || multilistSyncP(NULL) ||
            multilistCompactP(NULL)) {
            ERR("a call without its wrapper or value was not refused");
        }
        multilistIterator iter;
        mflexState *states[2] = {state, state};
        multilistIteratorInitP(NULL, states, &iter, true, true);
        multilistGetStatsP(NULL, NULL);
        multilistCloseP(NULL);
        PTEST_LOGGED(multilistResetP(ml));
        if (multilistPopTailP(ml, &popped) != PERSIST_LOG_UNCHANGED) {
            ERR("an empty list's pop was logged");
        }
        multilistGetStatsP(ml, &stats);
        if (stats.totalOps != ops + 1) {
            ERR("%" PRIu64 " ops logged for one reset", stats.totalOps - ops);
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
            PTEST_LOGGED(multilistPushTailP(ml, &box));
            ptestBoxTrackerPushTail(&tracker, &box);
        }

        if (!verifyMultilistMatchesTracker(ml, &tracker)) {
            ERR("Multilist/tracker mismatch");
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
            PTEST_LOGGED(multilistPushHeadP(ml, &box));
            ptestBoxTrackerPushHead(&tracker, &box);
        }

        if (!verifyMultilistMatchesTracker(ml, &tracker)) {
            ERR("Multilist/tracker mismatch");
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
                    PTEST_LOGGED(multilistPushTailP(ml, &box));
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
                ERR("Recovery failed");
            }

            if (!verifyMultilistMatchesTracker(ml, &tracker)) {
                ERR("Recovery verification failed");
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
            PTEST_LOGGED(multilistPushTailP(ml, &box));
            ptestBoxTrackerPushTail(&tracker, &box);
        }

        /* Pop from tail */
        databox got = {};
        if (!multilistPopTailP(ml, &got)) {
            ERR("Pop tail failed");
        }
        databox expected;
        ptestBoxTrackerPopTail(&tracker, &expected);
        if (!ptestBoxesEqual(&got, &expected)) {
            ERR("Pop tail value mismatch");
        }
        databoxFreeData(&got);

        /* Pop from head */
        if (!multilistPopHeadP(ml, &got)) {
            ERR("Pop head failed");
        }
        ptestBoxTrackerPopHead(&tracker, &expected);
        if (!ptestBoxesEqual(&got, &expected)) {
            ERR("Pop head value mismatch");
        }
        databoxFreeData(&got);

        if (!verifyMultilistMatchesTracker(ml, &tracker)) {
            ERR("Post-pop verification failed");
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
                PTEST_LOGGED(multilistPushTailP(ml, &box));
                ptestBoxTrackerPushTail(&tracker, &box);
            }

            /* Pop some */
            for (int i = 0; i < 5; i++) {
                databox got = {};
                databox expected;
                PTEST_LOGGED(multilistPopTailP(ml, &got));
                databoxFreeData(&got);
                ptestBoxTrackerPopTail(&tracker, &expected);
            }
            for (int i = 0; i < 3; i++) {
                databox got = {};
                databox expected;
                PTEST_LOGGED(multilistPopHeadP(ml, &got));
                databoxFreeData(&got);
                ptestBoxTrackerPopHead(&tracker, &expected);
            }

            multilistCloseP(ml);
            persistCtxFree(ctx);
        }

        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multilistP *ml = multilistOpenP(ctx);

            if (!ml) {
                ERR("Recovery failed");
            }

            if (!verifyMultilistMatchesTracker(ml, &tracker)) {
                ERR("Pop recovery verification failed");
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
                PTEST_LOGGED(multilistPushHeadP(ml, &box));
                ptestBoxTrackerPushHead(&tracker, &box);
            } else if (i % 3 == 1) {
                PTEST_LOGGED(multilistPushTailP(ml, &box));
                ptestBoxTrackerPushTail(&tracker, &box);
            } else {
                /* Pop if not empty */
                if (tracker.count > 0) {
                    databox got = {};
                    databox expected;
                    if (i % 2 == 0) {
                        PTEST_LOGGED(multilistPopHeadP(ml, &got));
                        ptestBoxTrackerPopHead(&tracker, &expected);
                    } else {
                        PTEST_LOGGED(multilistPopTailP(ml, &got));
                        ptestBoxTrackerPopTail(&tracker, &expected);
                    }
                    databoxFreeData(&got);
                }
            }
        }

        if (!verifyMultilistMatchesTracker(ml, &tracker)) {
            ERR("Mixed operations verification failed");
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
            PTEST_LOGGED(multilistPushTailP(ml, &box));
        }

        PTEST_LOGGED(multilistResetP(ml));
        ptestBoxTrackerInit(&tracker);

        if (multilistCountP(ml) != 0) {
            ERR("Count should be 0 after reset");
        }

        for (int i = 100; i < 110; i++) {
            databox box = DATABOX_SIGNED(i);
            PTEST_LOGGED(multilistPushTailP(ml, &box));
            ptestBoxTrackerPushTail(&tracker, &box);
        }

        if (!verifyMultilistMatchesTracker(ml, &tracker)) {
            ERR("Post-reset verification failed");
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
                PTEST_LOGGED(multilistPushTailP(ml, &box));
            }

            PTEST_LOGGED(multilistResetP(ml));

            for (int i = 100; i < 115; i++) {
                databox box = DATABOX_SIGNED(i);
                PTEST_LOGGED(multilistPushTailP(ml, &box));
                ptestBoxTrackerPushTail(&tracker, &box);
            }

            multilistCloseP(ml);
            persistCtxFree(ctx);
        }

        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multilistP *ml = multilistOpenP(ctx);

            if (!ml) {
                ERR("Reset recovery failed");
            }

            if (!verifyMultilistMatchesTracker(ml, &tracker)) {
                ERR("Reset recovery verification failed");
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
                PTEST_LOGGED(multilistPushTailP(ml, &box));
                ptestBoxTrackerPushTail(&tracker, &box);
            }

            multilistCloseP(ml);
            persistCtxFree(ctx);
        }

        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multilistP *ml = multilistOpenP(ctx);

            if (!ml) {
                ERR("Final recovery failed");
            }

            if (!verifyMultilistMatchesTracker(ml, &tracker)) {
                ERR("Final verification failed");
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
                PTEST_LOGGED(multilistPushTailP(ml, &box));
                ptestBoxTrackerPushTail(&tracker, &box);
            }

            multilistCloseP(ml);
            persistCtxFree(ctx);
        }

        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multilistP *ml = multilistOpenP(ctx);

            if (!ml) {
                ERR("Large dataset recovery failed");
            }

            if (!verifyMultilistMatchesTracker(ml, &tracker)) {
                ERR("Large dataset verification failed");
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
                ERR("Empty recovery failed");
            }

            if (multilistCountP(ml) != 0) {
                ERR("Empty multilist should have count 0");
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
            PTEST_LOGGED(multilistPushTailP(ml, &testBox));
            multilistCloseP(ml);
            persistCtxFree(ctx);
        }

        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multilistP *ml = multilistOpenP(ctx);

            if (multilistCountP(ml) != 1) {
                ERR("Should have 1 element");
            }

            multilistEntry entry;
            if (!multilistIndex(ml->ml, ml->state, 0, &entry, false)) {
                ERR("Failed to get index 0");
            }

            if (!ptestBoxesEqual(&entry.box, &testBox)) {
                ERR("Single element mismatch");
            }

            multilistCloseP(ml);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("multilistP refuses no-op deletes and out-of-range replaces "
         "without logging them") {
        /* Zero and negative counts, starts outside the list (both signs,
         * and far beyond 32 bits), and replace indexes outside the list
         * must return false, log nothing (totalOps unchanged) and leave
         * the list as it was, before and after recovery.  An empty list
         * refuses every range and index. */
        ptestCleanupFiles(basePath);
        ptestBoxTracker tracker;
        ptestBoxTrackerInit(&tracker);
        const mlOffsetId n = 12;
        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            multilistP *ml = multilistNewP(ctx, FLEX_CAP_LEVEL_2048, 0);
            const databox rep = DATABOX_SIGNED(-77);

            /* Empty list first: deletes change nothing, replaces have no
             * element to replace */
            if (multilistDelRangeP(ml, 0, 1) != PERSIST_LOG_UNCHANGED ||
                multilistDelRangeP(ml, -1, 1) != PERSIST_LOG_UNCHANGED ||
                multilistReplaceAtIndexP(ml, 0, &rep) ||
                multilistReplaceAtIndexP(ml, -1, &rep)) {
                ERR("an empty list accepted a delete or a replace");
            }

            for (mlOffsetId i = 0; i < n; i++) {
                databox box = DATABOX_SIGNED(i * 3);
                PTEST_LOGGED(multilistPushTailP(ml, &box));
                ptestBoxTrackerPushTail(&tracker, &box);
            }

            persistCtxStats before;
            multilistGetStatsP(ml, &before);
            const struct {
                mlOffsetId start;
                int64_t values;
            } noops[] = {{0, 0},
                         {0, -1},
                         {n, 1},
                         {-n - 1, 1},
                         {INT64_MAX, 1},
                         {INT64_MIN, 5},
                         {(mlOffsetId)1 << 32, 1},
                         {-1, 0}};
            for (size_t k = 0; k < countof(noops); k++) {
                if (multilistDelRangeP(ml, noops[k].start, noops[k].values) !=
                    PERSIST_LOG_UNCHANGED) {
                    ERR("DelRangeP(%" PRId64 ", %" PRId64 ") was accepted",
                        noops[k].start, noops[k].values);
                }
            }

            /* The last four would land on elements 1, n - 1, 0 and n - 1
             * if the index were narrowed to 32 bits. */
            const mlOffsetId big = (mlOffsetId)1 << 32;
            const mlOffsetId badIdx[] = {
                n,         -n - 1,  INT32_MAX,   INT32_MIN, INT64_MAX,
                INT64_MIN, big + 1, big + n - 1, -big,      -big - 1};
            for (size_t k = 0; k < countof(badIdx); k++) {
                if (multilistReplaceAtIndexP(ml, badIdx[k], &rep)) {
                    ERR("ReplaceAtIndexP(%" PRId64 ") was accepted", badIdx[k]);
                }
            }

            persistCtxStats after;
            multilistGetStatsP(ml, &after);
            if (after.totalOps != before.totalOps) {
                ERR("refused calls logged %" PRIu64 " operations",
                    after.totalOps - before.totalOps);
            }

            if (!verifyMultilistMatchesTracker(ml, &tracker)) {
                ERR("a refused call changed the list");
            }

            /* The edges are still accepted. */
            if (!multilistReplaceAtIndexP(ml, -1, &rep) ||
                !multilistDelRangeP(ml, -n, 1)) {
                ERR("an edge replace or delete was refused");
            }
            tracker.boxes[n - 1] = rep;
            memmove(&tracker.boxes[0], &tracker.boxes[1],
                    (tracker.count - 1) * sizeof(*tracker.boxes));
            tracker.count--;

            multilistCloseP(ml);
            persistCtxFree(ctx);
        }

        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multilistP *ml = multilistOpenP(ctx);
            if (!ml) {
                ERR("Recovery failed");
            } else {
                if (!verifyMultilistMatchesTracker(ml, &tracker)) {
                    ERR("recovered list differs");
                }

                multilistCloseP(ml);
            }

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
            PTEST_LOGGED(multilistPushTailP(ml, &box));
        }

        for (int i = 0; i < 5; i++) {
            databox got = {};
            PTEST_LOGGED(multilistPopTailP(ml, &got));
            databoxFreeData(&got);
        }

        persistCtxStats stats;
        multilistGetStatsP(ml, &stats);

        if (stats.totalOps != 25) {
            ERR("Total ops should be 25, got %" PRIu64, stats.totalOps);
        }

        multilistCloseP(ml);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multilistP 96/128-bit elements survive WAL replay and compaction") {
        /* strictRecovery, so a WAL row that replay refuses fails the reopen
         * instead of being skipped.  The tracker's boxes point into
         * 'storage' and 'strs', which outlive every check. */
        enum { MLP_WIDE_COUNT = 160, MLP_WIDE_SLOTS = MLP_WIDE_COUNT + 4 };
        static databoxBig storage[MLP_WIDE_SLOTS];
        static char strs[MLP_WIDE_SLOTS][48];
        ptestCleanupFiles(basePath);
        persistCtxConfig cfg = persistCtxDefaultConfig();
        cfg.strictRecovery = true;
        ptestBoxTracker tracker;
        ptestBoxTrackerInit(&tracker);

        persistCtx *ctx = persistCtxNew(basePath, &cfg);
        multilistP *ml = ctx ? multilistNewP(ctx, FLEX_CAP_LEVEL_128, 0) : NULL;
        if (!ml) {
            ERR("Failed to create multilistP");
        }

        for (uint32_t i = 0; ml && i < MLP_WIDE_COUNT; i++) {
            const databox *box =
                mlpWideValue(i, &storage[i], strs[i], sizeof(strs[i]));
            if (i % 5 == 4) {
                PTEST_LOGGED(multilistPushHeadP(ml, box));
                ptestBoxTrackerPushHead(&tracker, box);
            } else {
                PTEST_LOGGED(multilistPushTailP(ml, box));
                ptestBoxTrackerPushTail(&tracker, box);
            }
        }

        if (ml) {
            /* Replace with a wide value (logged as REPLACE). */
            const databox *rep =
                mlpWideValue(1, &storage[MLP_WIDE_COUNT], strs[MLP_WIDE_COUNT],
                             sizeof(strs[MLP_WIDE_COUNT]));
            if (!multilistReplaceAtIndexP(ml, 7, rep)) {
                ERR("wide replace failed");
            }
            tracker.boxes[7] = *rep;

            /* Pop both ends: into a zeroed box, then a databoxBig. */
            databox got = {};
            databox expected;
            if (!multilistPopTailP(ml, &got) ||
                !ptestBoxTrackerPopTail(&tracker, &expected) ||
                !ptestBoxesEqual(&got, &expected)) {
                ERR("tail pop of the wide list read back wrong");
            }
            databoxFreeData(&got);

            databoxBig gotBig;
            DATABOX_BIG_INIT(&gotBig);
            if (!multilistPopHeadP(ml, (databox *)&gotBig) ||
                !ptestBoxTrackerPopHead(&tracker, &expected) ||
                !ptestBoxesEqual((databox *)&gotBig, &expected)) {
                ERR("head pop of the wide list read back wrong");
            }
            databoxFreeData((databox *)&gotBig);

            /* Past Medium's 3x limit, so the Full tier is covered. */
            if (multilistBytesP(ml) <= 3 * 128) {
                ERR("list is only %zu bytes; it never reached Full",
                    multilistBytesP(ml));
            }

            if (!verifyMultilistMatchesTracker(ml, &tracker)) {
                ERR("live wide list does not match the tracker");
            }

            multilistCloseP(ml);
        }
        persistCtxFree(ctx);

        /* Reopen: snapshot of the empty list plus WAL replay. */
        ctx = persistCtxOpen(basePath, &cfg);
        ml = ctx ? multilistOpenP(ctx) : NULL;
        if (!ml) {
            ERR("WAL replay of the wide list failed");
        } else {
            if (!verifyMultilistMatchesTracker(ml, &tracker)) {
                ERR("WAL-replayed wide list does not match the tracker");
            }

            /* Compact (snapshot via the dump), then log more wide values on
             * top. */
            if (!multilistCompactP(ml)) {
                ERR("compaction of the wide list failed");
            }

            for (uint32_t k = 0; k < 3; k++) {
                const uint32_t slot = MLP_WIDE_COUNT + 1 + k;
                const databox *box =
                    mlpWideValue(4 * k + (k & 1), &storage[slot], strs[slot],
                                 sizeof(strs[slot]));
                PTEST_LOGGED(multilistPushTailP(ml, box));
                ptestBoxTrackerPushTail(&tracker, box);
            }

            multilistCloseP(ml);
        }
        persistCtxFree(ctx);

        /* Reopen: the compacted snapshot plus the newer WAL rows. */
        ctx = persistCtxOpen(basePath, &cfg);
        ml = ctx ? multilistOpenP(ctx) : NULL;
        if (!ml) {
            ERR("snapshot + WAL recovery of the wide list failed");
        } else {
            if (!verifyMultilistMatchesTracker(ml, &tracker)) {
                ERR("snapshot-recovered wide list does not match the "
                    "tracker");
            }

            multilistCloseP(ml);
        }
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multilistP normal, extended and mixed data on every tier survive "
         "WAL replay and compaction") {
        /* Each data mode on Small, Medium, Full and compressed Full: push,
         * replace, delete a range and pop (logged), then reopen from the
         * WAL, compact (snapshot via the dump), log more on top and reopen
         * from the snapshot plus the WAL.  Each state is read back through
         * IndexWide and Next against the tracker, under strictRecovery so
         * a refused WAL row fails the reopen. */
        constexpr int MLP_MATRIX_MAX = 420;
        static databoxBig storage[MLP_MATRIX_MAX];
        static char strs[MLP_MATRIX_MAX][48];
        static const char *const dataName[MLP_DATA_COUNT] = {
            "normal", "extended", "mixed"};
        const struct {
            const char *name;
            flexCapSizeLimit limit;
            uint32_t depth;
            uint32_t count;
            size_t minBytes; /* exclusive */
            size_t maxBytes; /* inclusive */
        } tiers[] = {
            {"Small", FLEX_CAP_LEVEL_65536, 0, 40, 0, 65536},
            {"Medium", FLEX_CAP_LEVEL_512, 0, 70, 512, 3 * 512},
            {"Full", FLEX_CAP_LEVEL_128, 0, 160, 3 * 128, SIZE_MAX},
            {"Full compressed", FLEX_CAP_LEVEL_128, 1, 400, 3 * 128, SIZE_MAX},
        };

        for (uint32_t d = 0; d < MLP_DATA_COUNT; d++) {
            for (size_t t = 0; t < countof(tiers); t++) {
                const mlpData data = (mlpData)d;
                const uint32_t count = tiers[t].count;
                ptestCleanupFiles(basePath);
                persistCtxConfig cfg = persistCtxDefaultConfig();
                cfg.strictRecovery = true;
                ptestBoxTracker tracker;
                ptestBoxTrackerInit(&tracker);

                persistCtx *ctx = persistCtxNew(basePath, &cfg);
                multilistP *ml =
                    ctx ? multilistNewP(ctx, tiers[t].limit, tiers[t].depth)
                        : NULL;
                if (!ml) {
                    ERR("%s %s: failed to create multilistP", dataName[d],
                        tiers[t].name);
                    persistCtxFree(ctx);
                    continue;
                }

                for (uint32_t i = 0; i < count; i++) {
                    const databox *box = mlpDataValue(data, i, &storage[i],
                                                      strs[i], sizeof(strs[i]));
                    if (i % 5 == 4) {
                        PTEST_LOGGED(multilistPushHeadP(ml, box));
                        ptestBoxTrackerPushHead(&tracker, box);
                    } else {
                        PTEST_LOGGED(multilistPushTailP(ml, box));
                        ptestBoxTrackerPushTail(&tracker, box);
                    }
                }

                const size_t bytes = multilistBytesP(ml);
                if (bytes <= tiers[t].minBytes || bytes > tiers[t].maxBytes) {
                    ERR("%s %s: %zu bytes is outside the tier's range",
                        dataName[d], tiers[t].name, bytes);
                }

                /* REPLACE, then DELETE_AT over two elements. */
                const databox *rep =
                    mlpDataValue(data, count, &storage[count], strs[count],
                                 sizeof(strs[count]));
                if (!multilistReplaceAtIndexP(ml, 3, rep)) {
                    ERR("%s %s: replace failed", dataName[d], tiers[t].name);
                }
                tracker.boxes[3] = *rep;

                if (!multilistDelRangeP(ml, 5, 2)) {
                    ERR("%s %s: delete range failed", dataName[d],
                        tiers[t].name);
                }
                memmove(&tracker.boxes[5], &tracker.boxes[7],
                        (tracker.count - 7) * sizeof(*tracker.boxes));
                tracker.count -= 2;

                /* POP_TAIL into a zeroed box, POP_HEAD into a databoxBig. */
                databox got = {};
                databox expected;
                if (!multilistPopTailP(ml, &got) ||
                    !ptestBoxTrackerPopTail(&tracker, &expected) ||
                    !ptestBoxesEqual(&got, &expected)) {
                    ERR("%s %s: tail pop read back wrong", dataName[d],
                        tiers[t].name);
                }
                databoxFreeData(&got);

                databoxBig gotBig;
                memset(&gotBig, 0, sizeof(gotBig));
                DATABOX_BIG_INIT(&gotBig);
                if (!multilistPopHeadP(ml, (databox *)&gotBig) ||
                    !ptestBoxTrackerPopHead(&tracker, &expected) ||
                    !ptestBoxesEqual((databox *)&gotBig, &expected)) {
                    ERR("%s %s: head pop read back wrong", dataName[d],
                        tiers[t].name);
                }
                databoxFreeData((databox *)&gotBig);

                if (!verifyMultilistMatchesTracker(ml, &tracker) ||
                    !verifyMultilistIterMatchesTracker(ml, &tracker)) {
                    ERR("%s %s: live list does not match the tracker",
                        dataName[d], tiers[t].name);
                }

                multilistCloseP(ml);
                persistCtxFree(ctx);

                /* Reopen: the empty snapshot plus WAL replay. */
                ctx = persistCtxOpen(basePath, &cfg);
                ml = ctx ? multilistOpenP(ctx) : NULL;
                if (!ml) {
                    ERR("%s %s: WAL replay failed", dataName[d], tiers[t].name);
                    persistCtxFree(ctx);
                    ptestCleanupFiles(basePath);
                    continue;
                }

                if (!verifyMultilistMatchesTracker(ml, &tracker) ||
                    !verifyMultilistIterMatchesTracker(ml, &tracker)) {
                    ERR("%s %s: WAL-replayed list does not match the tracker",
                        dataName[d], tiers[t].name);
                }

                if (!multilistCompactP(ml)) {
                    ERR("%s %s: compaction failed", dataName[d], tiers[t].name);
                }

                for (uint32_t k = 1; k <= 3; k++) {
                    const uint32_t slot = count + k;
                    const databox *box =
                        mlpDataValue(data, slot, &storage[slot], strs[slot],
                                     sizeof(strs[slot]));
                    PTEST_LOGGED(multilistPushTailP(ml, box));
                    ptestBoxTrackerPushTail(&tracker, box);
                }

                multilistCloseP(ml);
                persistCtxFree(ctx);

                /* Reopen: the compacted snapshot plus the newer WAL rows. */
                ctx = persistCtxOpen(basePath, &cfg);
                ml = ctx ? multilistOpenP(ctx) : NULL;
                if (!ml) {
                    ERR("%s %s: snapshot + WAL recovery failed", dataName[d],
                        tiers[t].name);
                } else {
                    if (!verifyMultilistMatchesTracker(ml, &tracker) ||
                        !verifyMultilistIterMatchesTracker(ml, &tracker)) {
                        ERR("%s %s: snapshot-recovered list does not match "
                            "the tracker",
                            dataName[d], tiers[t].name);
                    }

                    multilistCloseP(ml);
                }

                persistCtxFree(ctx);
                ptestCleanupFiles(basePath);
            }
        }
    }

    TEST("multilistP readers: GetP, IndexP and SyncP") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multilistP *ml = multilistNewP(ctx, FLEX_CAP_LEVEL_2048, 0);
        for (int64_t i = 0; i < 10; i++) {
            const databox v = DATABOX_SIGNED(i * 11);
            PTEST_LOGGED(multilistPushTailP(ml, &v));
        }
        cleaner(mflexStateFree) mflexState *state = mflexStateCreate();
        multilistEntry first = {};
        multilistEntry last = {};
        const databox want0 = DATABOX_SIGNED(0);
        const databox want9 = DATABOX_SIGNED(99);
        if (!multilistSyncP(ml) || multilistCount(multilistGetP(ml)) != 10 ||
            !multilistIndexGetP(ml, state, 0, &first) ||
            !databoxEqual(&first.box, &want0) ||
            !multilistIndexGetP(ml, state, -1, &last) ||
            !databoxEqual(&last.box, &want9) ||
            multilistIndexCheckP(ml, state, 10, &last)) {
            ERR("GetP/IndexP/SyncP disagree with the pushes");
        }
        multilistCloseP(ml);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multilistP keeps its node size and compression across reopen") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multilistP *ml = multilistNewP(ctx, FLEX_CAP_LEVEL_512, 2);
        for (int64_t i = 0; i < 300; i++) {
            const databox v = DATABOX_SIGNED(i);
            PTEST_LOGGED(multilistPushTailP(ml, &v));
        }
        multilistCompactP(ml);
        multilistCloseP(ml);
        persistCtxFree(ctx);

        ctx = persistCtxOpen(basePath, NULL);
        ml = multilistOpenP(ctx);
        if (!ml || multilistLimit(multilistGetP(ml)) != FLEX_CAP_LEVEL_512 ||
            multilistDepth(multilistGetP(ml)) != 2 ||
            multilistCount(multilistGetP(ml)) != 300) {
            ERR("Reopened list lost its configuration or elements");
        }
        multilistCloseP(ml);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multilistP after a damaged WAL entry: healed, logging goes on") {
        /* Tail pushes of 1, 2, 3 with the last entry damaged: lenient
         * recovery keeps [1, 2] and heals the WAL (the damaged entry moves
         * to the quarantine); a push of 4 is then logged with no
         * compaction, and a reopen holds [1, 2, 4] */
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multilistP *ml = multilistNewP(ctx, FLEX_CAP_LEVEL_2048, 0);
        for (int64_t v = 1; v <= 3; v++) {
            const databox box = databoxNewSigned(v);
            PTEST_LOGGED(multilistPushTailP(ml, &box));
        }
        multilistCloseP(ml);
        persistCtxFree(ctx);
        if (!ptestDamageLastWalEntry(basePath)) {
            ERR("could not damage the WAL");
        }

        const databox four = databoxNewSigned(4);
        ctx = persistCtxOpen(basePath, NULL);
        ml = multilistOpenP(ctx);
        if (!ml || multilistCountP(ml) != 2) {
            ERR("lenient recovery kept %zu values, want 1, 2",
                ml ? multilistCountP(ml) : (size_t)0);
        }
        if (!ptestExpectHealed(ctx, basePath)) {
            ERR("the damaged entry was not healed into the quarantine");
        }
        if (ml && multilistPushTailP(ml, &four) == PERSIST_LOG_REFUSED) {
            ERR("a change after the healed entry was refused");
        }
        multilistCloseP(ml);
        persistCtxFree(ctx);

        ctx = persistCtxOpen(basePath, NULL);
        ml = multilistOpenP(ctx);
        cleaner(mflexStateFree) mflexState *state = mflexStateCreate();
        const int64_t want[3] = {1, 2, 4};
        bool same = ml && multilistCountP(ml) == 3;
        for (int i = 0; same && i < 3; i++) {
            multilistEntry at = {};
            same = multilistIndexGetP(ml, state, i, &at) &&
                   at.box.data.i == want[i];
        }
        if (!same) {
            ERR("reopen does not hold 1, 2, 4");
        }
        multilistCloseP(ml);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
