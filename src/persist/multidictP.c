/* multidictP.c - Persistent multidict wrapper
 *
 * See multidictP.h for API documentation.
 */

#include "multidictP.h"
#include "../cleaner.h"
#include "../datakit.h"
#include "../persist.h"

#include <string.h>
#include <unistd.h>

defcleanerName(multidictP *, zfree, MultidictP);
defcleaner(multidict *, multidictFree);
defcleaner(multidictClass *, multidictDefaultClassFree);

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

multidictP *multidictNewP(persistCtx *ctx) {
    return multidictNewPWithType(ctx, &multidictTypeExactKey);
}

multidictP *multidictNewPWithType(persistCtx *ctx, multidictType *type) {
    /* multidictOpenP recovers through persistOpsMultidict, which rebuilds
     * the built-in types only */
    if (!ctx ||
        (type != &multidictTypeExactKey && type != &multidictTypeCaseKey)) {
        return NULL;
    }

    /* Every failure below releases all three through their cleaners: the
     * dict first, then the class it borrows, then the wrapper */
    cleanerName(zfree, MultidictP) multidictP *d = zcalloc(1, sizeof(*d));
    cleaner(multidictDefaultClassFree) multidictClass *qdc =
        multidictDefaultClassNew();
    cleaner(multidictFree) multidict *dict =
        multidictNew(type, qdc, multidictRandomSeed());
    if (!d || !qdc || !dict) {
        return NULL;
    }

    /* Initialize persistence context for multidict type, and save the
     * initial snapshot, which records the key type */
    if (!persistCtxInitForType(ctx, &persistOpsMultidict) ||
        !persistCtxSaveSnapshot(ctx, dict, &persistOpsMultidict)) {
        return NULL;
    }

    /* The wrapper holds the dict, and the dict the class, from here on */
    d->ctx = ctx;
    d->d = dict;
    dict = NULL;
    qdc = NULL;

    persistCtxClaim(ctx);
    multidictP *out = d;
    d = NULL;
    return out;
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

    /* @own: caller; free=multidictCloseP */
    multidictP *d = zcalloc(1, sizeof(*d));
    if (!d) {
        /* The recovered dict owns its own default class */
        persistOpsMultidict.free(recovered);
        return NULL;
    }

    d->ctx = ctx;
    d->d = recovered;

    persistCtxClaim(ctx);
    return d;
}

void multidictCloseP(multidictP *d) {
    if (!d) {
        return;
    }

    /* Sync before closing; the context is free for another wrapper */
    if (d->ctx) {
        persistCtxSync(d->ctx);
        persistCtxRelease(d->ctx);
    }

    /* Free underlying multidict; it privately owns a default class
     * (created by NewP or by restore). The class must outlive the dict
     * because teardown runs class callbacks, so free it after. */
    if (d->d) {
        multidictClass *cls = multidictGetClass(d->d);
        multidictFree(d->d);
        multidictDefaultClassFree(cls);
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

persistLogResult multidictAddP(multidictP *d, const databox *key,
                               const databox *val, multidictResult *result) {
    if (result) {
        *result = MULTIDICT_ERR;
    }
    if (!d || !d->d || !d->ctx || !key || !val) {
        return PERSIST_LOG_REFUSED;
    }

    const databox *args[] = {key, val};
    const persistLogResult logged =
        persistCtxLogOp(d->ctx, PERSIST_OP_INSERT, args, 2);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }
    const multidictResult added = multidictAdd(d->d, key, val);
    if (result) {
        *result = added;
    }
    persistCtxMaybeCompact(d->ctx, d->d, &persistOpsMultidict);
    return logged;
}

typedef struct mdpReplace_ {
    persistCtx *ctx;
    const databox *args[2];
    persistLogResult logged;
} mdpReplace_;

/* Inside the dict's one search, with the key found: log the replace,
 * then rewrite the value, or decline when the log is refused (nothing
 * changes and nothing was logged) */
static bool mdpLogThenReplace_(void *ctx, const databox *,
                               databox *replacement) {
    mdpReplace_ *r = ctx;
    r->logged = persistCtxLogOp(r->ctx, PERSIST_OP_REPLACE, r->args, 2);
    if (r->logged == PERSIST_LOG_REFUSED) {
        return false;
    }

    *replacement = *r->args[1];
    return true;
}

persistLogResult multidictReplaceP(multidictP *d, const databox *key,
                                   const databox *val) {
    if (!d || !d->d || !d->ctx || !key || !val) {
        return PERSIST_LOG_REFUSED;
    }

    /* One search: an absent key logs nothing (replaying it would change
     * nothing either); a present one is logged and rewritten where it was
     * found, and that is the call's one LRU use */
    mdpReplace_ r = {.ctx = d->ctx, .args = {key, val}};
    const int32_t rewritten =
        multidictUpdateValue(d->d, key, mdpLogThenReplace_, &r);
    if (rewritten < 0) {
        return PERSIST_LOG_UNCHANGED;
    }
    if (rewritten == 0) {
        return PERSIST_LOG_REFUSED;
    }

    persistCtxMaybeCompact(d->ctx, d->d, &persistOpsMultidict);
    return r.logged;
}

persistLogResult multidictDeleteP(multidictP *d, const databox *key) {
    if (!d || !d->d || !d->ctx || !key) {
        return PERSIST_LOG_REFUSED;
    }
    if (!multidictExistsNoTouch(d->d, key)) {
        return PERSIST_LOG_UNCHANGED;
    }

    /* Delete takes a single databox*, not an array */
    const persistLogResult logged =
        persistCtxLogOp(d->ctx, PERSIST_OP_DELETE, key, 1);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }
    multidictDelete(d->d, key);
    persistCtxMaybeCompact(d->ctx, d->d, &persistOpsMultidict);
    return logged;
}

persistLogResult multidictEmptyP(multidictP *d) {
    if (!d || !d->d || !d->ctx) {
        return PERSIST_LOG_REFUSED;
    }

    const persistLogResult logged =
        persistCtxLogOp(d->ctx, PERSIST_OP_CLEAR, NULL, 0);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }
    multidictEmpty(d->d);
    persistCtxMaybeCompact(d->ctx, d->d, &persistOpsMultidict);
    return logged;
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
#include "../multimap.h" /* allocation counting seam */
#include "persistTestCommon.h"
#include <fcntl.h>
#include <inttypes.h>
#include <sys/stat.h>

/* The P mutations as their multidict counterparts report them, so a test
 * can compare a P dict with a plain one call for call */
static multidictResult mdpAdd(multidictP *d, const databox *key,
                              const databox *val) {
    multidictResult result = MULTIDICT_ERR;
    return multidictAddP(d, key, val, &result) ? result : MULTIDICT_ERR;
}

static multidictResult mdpReplace(multidictP *d, const databox *key,
                                  const databox *val) {
    const persistLogResult logged = multidictReplaceP(d, key, val);
    return logged && logged != PERSIST_LOG_UNCHANGED ? MULTIDICT_OK_REPLACED
                                                     : MULTIDICT_ERR;
}

static bool mdpDelete(multidictP *d, const databox *key) {
    const persistLogResult logged = multidictDeleteP(d, key);
    return logged && logged != PERSIST_LOG_UNCHANGED;
}

/* Helper to verify multidict matches tracker */
static bool verifyMultidictMatchesTracker(multidictP *d,
                                          const ptestKVTracker *tracker) {
    if (multidictCountP(d) != tracker->count) {
        printf("  [verify] Count mismatch: dict has %" PRIu64
               ", tracker has %u\n",
               multidictCountP(d), tracker->count);
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

int multidictPTest(int, char *[]) {
    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multidictPTest", basePath, sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multidictP create empty and close") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        if (!ctx) {
            ERR("Failed to create persistCtx");
        }

        multidictP *d = multidictNewP(ctx);
        if (!d) {
            persistCtxFree(ctx);
            ERR("Failed to create multidictP");
        }

        if (multidictCountP(d) != 0) {
            ERR("New dict should be empty");
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
            MULTIDICT_TEST_OK(mdpAdd(d, &key, &val));
            ptestKVTrackerInsert(&tracker, &key, &val);
        }

        if (!verifyMultidictMatchesTracker(d, &tracker)) {
            ERR("Multidict/tracker mismatch after adds");
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
            MULTIDICT_TEST_OK(mdpAdd(d, &key, &val));
            ptestKVTrackerInsert(&tracker, &key, &val);
        }

        /* Update with new values */
        for (int i = 0; i < 10; i++) {
            databox key = DATABOX_SIGNED(i);
            databox val = DATABOX_SIGNED(i * 1000);
            MULTIDICT_TEST_OK(mdpAdd(d, &key, &val));
            ptestKVTrackerInsert(&tracker, &key, &val);
        }

        if (!verifyMultidictMatchesTracker(d, &tracker)) {
            ERR("Multidict/tracker mismatch after updates");
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
                MULTIDICT_TEST_OK(mdpAdd(d, &key, &val));
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
                ERR("Failed to recover multidictP");
            }

            if (!verifyMultidictMatchesTracker(d, &tracker)) {
                ERR("Multidict/tracker mismatch after recovery");
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
            MULTIDICT_TEST_OK(mdpAdd(d, &key, &val));
            ptestKVTrackerInsert(&tracker, &key, &val);
        }

        /* Delete every other entry */
        for (int i = 0; i < 20; i += 2) {
            databox key = DATABOX_SIGNED(i);
            mdpDelete(d, &key);
            ptestKVTrackerDelete(&tracker, &key);
        }

        if (!verifyMultidictMatchesTracker(d, &tracker)) {
            ERR("Multidict/tracker mismatch after deletes");
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
                MULTIDICT_TEST_OK(mdpAdd(d, &key, &val));
                ptestKVTrackerInsert(&tracker, &key, &val);
            }

            /* Delete entries 3, 7, 11 */
            for (int i = 3; i < 15; i += 4) {
                databox key = DATABOX_SIGNED(i);
                mdpDelete(d, &key);
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
                ERR("Failed to recover multidictP after delete");
            }

            if (!verifyMultidictMatchesTracker(d, &tracker)) {
                ERR("Multidict/tracker mismatch after delete recovery");
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
            MULTIDICT_TEST_OK(mdpAdd(d, &key, &val));
        }

        if (multidictCountP(d) != 10) {
            ERR("Count should be 10 before empty");
        }

        /* Empty the dict */
        PTEST_LOGGED(multidictEmptyP(d));

        if (multidictCountP(d) != 0) {
            ERR("Count should be 0 after empty");
        }

        /* Add new entries */
        for (int i = 100; i < 115; i++) {
            databox key = DATABOX_SIGNED(i);
            databox val = DATABOX_SIGNED(i * 2);
            MULTIDICT_TEST_OK(mdpAdd(d, &key, &val));
            ptestKVTrackerInsert(&tracker, &key, &val);
        }

        if (!verifyMultidictMatchesTracker(d, &tracker)) {
            ERR("Multidict/tracker mismatch after empty and re-add");
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
                MULTIDICT_TEST_OK(mdpAdd(d, &key, &val));
            }

            PTEST_LOGGED(multidictEmptyP(d));

            for (int i = 50; i < 60; i++) {
                databox key = DATABOX_SIGNED(i);
                databox val = DATABOX_SIGNED(i * 3);
                MULTIDICT_TEST_OK(mdpAdd(d, &key, &val));
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
                ERR("Failed to recover multidictP after empty");
            }

            if (!verifyMultidictMatchesTracker(d, &tracker)) {
                ERR("Multidict/tracker mismatch after empty recovery");
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
                MULTIDICT_TEST_OK(mdpAdd(d, &key, &val));
                ptestKVTrackerInsert(&tracker, &key, &val);
            }

            /* Update a few */
            for (int i = round * 10; i < round * 10 + 3; i++) {
                databox key = DATABOX_SIGNED(i);
                databox val = DATABOX_SIGNED(i * 9999);
                MULTIDICT_TEST_OK(mdpAdd(d, &key, &val));
                ptestKVTrackerInsert(&tracker, &key, &val);
            }

            /* Delete one */
            databox key = DATABOX_SIGNED(round * 10 + 5);
            mdpDelete(d, &key);
            ptestKVTrackerDelete(&tracker, &key);
        }

        if (!verifyMultidictMatchesTracker(d, &tracker)) {
            ERR("Multidict/tracker mismatch after mixed ops");
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
                MULTIDICT_TEST_OK(mdpAdd(d, &key, &val));
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
                MULTIDICT_TEST_OK(mdpAdd(d, &key, &val));
                ptestKVTrackerInsert(&tracker, &key, &val);
            }

            multidictCloseP(d);
            persistCtxFree(ctx);
        }

        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multidictP *d = multidictOpenP(ctx);

            if (!d) {
                ERR("Large dataset recovery failed");
            }

            if (!verifyMultidictMatchesTracker(d, &tracker)) {
                ERR("Large dataset verification failed");
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
                ERR("Empty dict recovery failed");
            }

            if (multidictCountP(d) != 0) {
                ERR("Recovered empty dict should have 0 entries");
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
            MULTIDICT_TEST_OK(mdpAdd(d, &expectedKey, &expectedVal));
            multidictCloseP(d);
            persistCtxFree(ctx);
        }

        /* Recover and verify */
        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multidictP *d = multidictOpenP(ctx);

            if (!d) {
                ERR("Single entry recovery failed");
            }

            if (multidictCountP(d) != 1) {
                ERR("Should have exactly 1 entry");
            }

            databox val;
            if (!multidictFindP(d, &expectedKey, &val)) {
                ERR("Expected key not found");
            }

            if (!ptestBoxesEqual(&val, &expectedVal)) {
                ERR("Value mismatch after recovery");
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
            MULTIDICT_TEST_OK(mdpAdd(d, &key, &val));
        }

        /* Delete a few */
        for (int i = 0; i < 5; i++) {
            databox key = DATABOX_SIGNED(i);
            mdpDelete(d, &key);
        }

        persistCtxStats stats;
        multidictGetStatsP(d, &stats);

        /* 15 adds + 5 deletes = 20 total ops */
        if (stats.totalOps != 20) {
            ERR("Total ops should be 20, got %" PRIu64, stats.totalOps);
        }

        multidictCloseP(d);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multidictP case keys survive WAL replay and compaction") {
        ptestCleanupFiles(basePath);
        const databox alpha = databoxNewBytesString("Alpha");
        const databox ALPHA = databoxNewBytesString("ALPHA");
        const databox aLpHa = databoxNewBytesString("aLpHa");
        const databox beta = databoxNewBytesString("BETA");
        const databox betaLow = databoxNewBytesString("beta");
        const databox gamma = databoxNewBytesString("Gamma");
        const databox GAMMA = databoxNewBytesString("GAMMA");
        const databox one = DATABOX_SIGNED(1);
        const databox two = DATABOX_SIGNED(2);
        const databox three = DATABOX_SIGNED(3);

        /* Only built-in types can be reopened */
        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            static multidictType custom = {NULL, NULL};
            custom = multidictTypeCaseKey;
            if (multidictNewPWithType(ctx, &custom) ||
                multidictNewPWithType(ctx, NULL)) {
                ERR("multidictNewPWithType accepted a custom type");
            }
            persistCtxFree(ctx);
            ptestCleanupFiles(basePath);
        }

        /* Session 1: everything lives in the WAL after the empty, tagged
         * snapshot NewP writes */
        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            multidictP *d = multidictNewPWithType(ctx, &multidictTypeCaseKey);
            if (!d || mdpAdd(d, &alpha, &one) != MULTIDICT_OK_INSERTED ||
                mdpAdd(d, &beta, &two) != MULTIDICT_OK_INSERTED ||
                mdpAdd(d, &ALPHA, &three) != MULTIDICT_OK_REPLACED ||
                multidictCountP(d) != 2) {
                ERR("case multidictP session 1");
            }
            multidictCloseP(d);
            persistCtxFree(ctx);
        }

        /* Session 2: replayed from the WAL as a case dict */
        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multidictP *d = multidictOpenP(ctx);
            databox v = {};
            if (!d ||
                multidictGetType(multidictGetP(d)) != &multidictTypeCaseKey ||
                multidictCountP(d) != 2 || !multidictFindP(d, &aLpHa, &v) ||
                !ptestBoxesEqual(&v, &three) || !mdpDelete(d, &betaLow) ||
                multidictCountP(d) != 1) {
                ERR("case multidictP after WAL replay");
            }

            /* Compact, then keep logging on top of the new snapshot */
            if (!multidictCompactP(d) ||
                mdpAdd(d, &gamma, &one) != MULTIDICT_OK_INSERTED) {
                ERR("case multidictP compaction");
            }
            multidictCloseP(d);
            persistCtxFree(ctx);
        }

        /* Session 3: restored from the compacted snapshot plus the WAL;
         * keys differing only in case are still one entry, under the
         * spelling that first inserted it */
        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multidictP *d = multidictOpenP(ctx);
            databox v = {};
            if (!d ||
                multidictGetType(multidictGetP(d)) != &multidictTypeCaseKey ||
                multidictCountP(d) != 2 || !multidictExistsP(d, &GAMMA) ||
                !multidictFindP(d, &ALPHA, &v) ||
                !ptestBoxesEqual(&v, &three) ||
                mdpAdd(d, &aLpHa, &two) != MULTIDICT_OK_REPLACED ||
                multidictCountP(d) != 2) {
                ERR("case multidictP after compaction");
            }

            bool alphaKept = false;
            multidictIterator iter;
            multidictIteratorInitP(d, &iter);
            multidictEntry e;
            while (multidictIteratorNext(&iter, &e)) {
                alphaKept = alphaKept ||
                            (DATABOX_IS_BYTES(&e.key) && e.key.len == 5 &&
                             memcmp(databoxCBytes(&e.key), "Alpha", 5) == 0);
            }
            multidictIteratorRelease(&iter);
            if (!alphaKept) {
                ERR("case multidictP lost the stored spelling");
            }
            multidictCloseP(d);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("multidictP exact dicts reopen exact") {
        ptestCleanupFiles(basePath);
        const databox lower = databoxNewBytesString("key");
        const databox upper = databoxNewBytesString("KEY");
        const databox one = DATABOX_SIGNED(1);
        const databox two = DATABOX_SIGNED(2);
        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            multidictP *d = multidictNewP(ctx);
            MULTIDICT_TEST_OK(mdpAdd(d, &lower, &one));
            multidictCompactP(d);
            MULTIDICT_TEST_OK(mdpAdd(d, &upper, &two));
            multidictCloseP(d);
            persistCtxFree(ctx);
        }

        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multidictP *d = multidictOpenP(ctx);
            databox v = {};
            if (!d ||
                multidictGetType(multidictGetP(d)) != &multidictTypeExactKey ||
                multidictCountP(d) != 2 || !multidictFindP(d, &upper, &v) ||
                !ptestBoxesEqual(&v, &two)) {
                ERR("exact multidictP did not reopen exact");
            }
            multidictCloseP(d);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("multidictP wrappers return what multidict returns") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multidictP *d = multidictNewPWithType(ctx, &multidictTypeCaseKey);
        cleaner(multidictDefaultClassFree) multidictClass *qdc =
            multidictDefaultClassNew();
        cleaner(multidictFree) multidict *plain =
            multidictNew(&multidictTypeCaseKey, qdc, 0);

        /* A replace of an absent key fails and logs nothing */
        const databox absent = databoxNewBytesString("absent");
        const databox one = DATABOX_SIGNED(1);
        persistCtxStats before;
        persistCtxStats after;
        multidictGetStatsP(d, &before);
        if (mdpReplace(d, &absent, &one) != MULTIDICT_ERR ||
            multidictReplace(plain, &absent, &one) != MULTIDICT_ERR ||
            multidictCountP(d) != 0) {
            ERR("replace of an absent key did not fail");
        }
        multidictGetStatsP(d, &after);
        if (after.totalOps != before.totalOps) {
            ERR("failed replace was logged");
        }

        /* The same script through both, with mixed-case keys */
        uint32_t rng = 12345;
        for (int32_t step = 0; step < 400; step++) {
            rng = rng * 1103515245u + 12345u;
            const uint32_t r = rng >> 8;
            char buf[16];
            snprintf(buf, sizeof(buf), "%ckey%02" PRIu32, (r & 1) ? 'K' : 'k',
                     (r >> 1) % 24);
            const databox key = databoxNewBytesString(buf);
            const databox val = DATABOX_SIGNED(step);
            const uint32_t op = (r >> 8) % 100;
            bool same = true;
            if (op < 35) {
                same = mdpAdd(d, &key, &val) == multidictAdd(plain, &key, &val);
            } else if (op < 60) {
                same = mdpReplace(d, &key, &val) ==
                       multidictReplace(plain, &key, &val);
            } else if (op < 80) {
                same = mdpDelete(d, &key) == multidictDelete(plain, &key);
            } else if (op < 98) {
                databox pv = {};
                databox nv = {};
                const bool pf = multidictFindP(d, &key, &pv);
                const bool nf = multidictFind(plain, &key, &nv);
                same = pf == nf && multidictExistsP(d, &key) == nf &&
                       (!pf || ptestBoxesEqual(&pv, &nv));
            } else {
                PTEST_LOGGED(multidictEmptyP(d));
                multidictEmpty(plain);
            }

            if (!same || multidictCountP(d) != multidictCount(plain)) {
                ERR("multidictP diverged from multidict at step %d", step);
                break;
            }
        }
        multidictCloseP(d);
        persistCtxFree(ctx);

        /* Replaying the WAL gives the plain dict's contents */
        ctx = persistCtxOpen(basePath, NULL);
        d = multidictOpenP(ctx);
        if (!d || multidictCountP(d) != multidictCount(plain)) {
            ERR("replayed multidictP count differs from multidict");
        } else {
            multidictIterator iter;
            multidictIteratorInit(plain, &iter);
            multidictEntry e;
            while (multidictIteratorNext(&iter, &e)) {
                databox pv = {};
                if (!multidictFindP(d, &e.key, &pv) ||
                    !ptestBoxesEqual(&pv, &e.val)) {
                    ERR("replayed multidictP entry differs from multidict");
                    break;
                }
            }
            multidictIteratorRelease(&iter);
        }
        multidictCloseP(d);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multidictP creation frees everything when the snapshot fails") {
        /* The snapshot is published through a temp file beside it, so a
         * directory made read-only after the stores are open fails the
         * initial save */
        char dir[PATH_MAX];
        char path[PATH_MAX];
        char probe[PATH_MAX];
        if (!dkTestTmpDirPath(&tmp, "readOnly", dir, sizeof(dir)) ||
            !dkTestTmpDirPath(&tmp, "readOnly/dict", path, sizeof(path)) ||
            !dkTestTmpDirPath(&tmp, "readOnly/probe", probe, sizeof(probe)) ||
            mkdir(dir, 0700) != 0) {
            ERR("could not create the test directory");
        } else {
            persistCtx *ctx = persistCtxNew(path, NULL);
            if (!ctx || !persistCtxInitForType(ctx, &persistOpsMultidict)) {
                ERR("could not open the test stores");
            }

            chmod(dir, 0500);
            const int fd = open(probe, O_RDWR | O_CREAT, 0600);
            if (fd >= 0) {
                /* Permissions are not enforced (running as root) */
                close(fd);
                unlink(probe);
                printf("  skipped: directory permissions not enforced\n");
            } else if (ctx) {
                multidictType *const types[] = {&multidictTypeExactKey,
                                                &multidictTypeCaseKey};
                for (size_t t = 0; t < 2; t++) {
                    multimapTestAllocCountBegin();
                    multidictP *d = multidictNewPWithType(ctx, types[t]);
                    const int64_t live = multimapTestAllocCountEnd();
                    if (d) {
                        multidictCloseP(d);
                        ERR("multidictNewP succeeded without a snapshot");
                    } else if (live != 0) {
                        ERR("failed multidictNewP left %" PRId64
                            " blocks allocated",
                            live);
                    }
                }
            }

            chmod(dir, 0700);
            persistCtxFree(ctx);
            ptestCleanupFiles(path);
            rmdir(dir);
        }
    }

    TEST("multidictP mutations are one LRU use each and report no-ops") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multidictP *d = multidictNewP(ctx);
        multidictEnableLRU(multidictGetP(d), 4);
        const databox key = DATABOX_SIGNED(7);
        const databox one = DATABOX_SIGNED(1);
        const databox two = DATABOX_SIGNED(2);
        multidictResult added = MULTIDICT_ERR;
        if (!multidictAddP(d, &key, &one, &added) ||
            added != MULTIDICT_OK_INSERTED) {
            ERR("add was not an insert");
        }
        const int before = multidictGetLRULevel(multidictGetP(d), &key);
        if (!multidictReplaceP(d, &key, &two) ||
            multidictGetLRULevel(multidictGetP(d), &key) != before + 1) {
            ERR("replace moved the key from level %d to %d", before,
                multidictGetLRULevel(multidictGetP(d), &key));
        }

        const databox absent = DATABOX_SIGNED(8);
        persistCtxStats stats;
        persistCtxGetStats(ctx, &stats);
        const uint64_t ops = stats.totalOps;
        if (multidictReplaceP(d, &absent, &one) != PERSIST_LOG_UNCHANGED ||
            multidictDeleteP(d, &absent) != PERSIST_LOG_UNCHANGED) {
            ERR("an absent key's replace or delete was not UNCHANGED");
        }
        persistCtxGetStats(ctx, &stats);
        if (stats.totalOps != ops) {
            ERR("no-op mutations were logged");
        }
        multidictCloseP(d);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multidictP a refused log changes nothing (replace, delete)") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multidictP *d = multidictNewP(ctx);
        multidictEnableLRU(multidictGetP(d), 4);
        const databox key = databoxNewBytesString("the refused key");
        const databox kept =
            databoxNewBytesString("a value long enough to live on the heap");
        const databox other = databoxNewBytesString("other");
        const databox newer = DATABOX_SIGNED(99);
        PTEST_LOGGED(multidictAddP(d, &key, &kept, NULL));
        PTEST_LOGGED(multidictAddP(d, &other, &newer, NULL));
        const int level = multidictGetLRULevel(multidictGetP(d), &key);

        const databox fresh = databoxNewBytesString("fresh");
        multidictResult added = MULTIDICT_OK_REPLACED;
        PTEST_EXPECT_REFUSED(
            ctx, multidictReplaceP(d, &key, &newer), multidictDeleteP(d, &key),
            multidictAddP(d, &key, &newer, &added),
            multidictAddP(d, &fresh, &newer, &added), multidictEmptyP(d));

        databox got = {};
        if (!multidictFindP(d, &key, &got) ||
            databoxCompare(&got, &kept) != 0 || multidictCountP(d) != 2 ||
            multidictExistsP(d, &fresh) || added != MULTIDICT_ERR) {
            ERR("a refused change changed the dict");
        }
        if (multidictGetLRULevel(multidictGetP(d), &key) < level) {
            ERR("a refused change lost the key's LRU level");
        }

        /* Missing arguments: refused or empty answers */
        if (multidictAddP(NULL, &key, &newer, NULL) ||
            multidictAddP(d, NULL, &newer, NULL) ||
            multidictReplaceP(NULL, &key, &newer) ||
            multidictDeleteP(NULL, &key) || multidictEmptyP(NULL) ||
            multidictNewP(NULL) || multidictOpenP(NULL) ||
            multidictGetP(NULL) || multidictCountP(NULL) ||
            multidictBytesP(NULL) || multidictFindP(NULL, &key, &got) ||
            multidictExistsP(NULL, &key) || multidictSyncP(NULL) ||
            multidictCompactP(NULL)) {
            ERR("a call without its wrapper or key was not refused");
        }
        multidictGetStatsP(NULL, NULL);
        multidictCloseP(NULL);

        /* Logging works again, and a reopen holds only what was logged */
        if (multidictReplaceP(d, &key, &newer) == PERSIST_LOG_REFUSED) {
            ERR("the context did not log again after the refusal");
        }
        multidictCloseP(d);
        persistCtxFree(ctx);
        ctx = persistCtxOpen(basePath, NULL);
        d = multidictOpenP(ctx);
        if (!d || multidictCountP(d) != 2 || !multidictFindP(d, &key, &got) ||
            databoxCompare(&got, &newer) != 0) {
            ERR("a reopen did not hold exactly the logged changes");
        }
        multidictCloseP(d);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multidictP readers: CountP, BytesP and SyncP") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multidictP *d = multidictNewP(ctx);
        const uint64_t emptyBytes = multidictBytesP(d);
        for (int64_t i = 0; i < 50; i++) {
            const databox key = DATABOX_SIGNED(i);
            const databox val = DATABOX_SIGNED(i * i);
            MULTIDICT_TEST_OK(mdpAdd(d, &key, &val));
        }
        if (!multidictSyncP(d) || multidictCountP(d) != 50 ||
            multidictBytesP(d) <= emptyBytes) {
            ERR("CountP/BytesP/SyncP disagree with 50 adds");
        }
        multidictCloseP(d);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multidictP after a damaged WAL entry: healed, logging goes on") {
        /* Keys 1, 2, 3 added with the last entry damaged: lenient
         * recovery keeps {1, 2} and heals the WAL (the damaged entry moves
         * to the quarantine); an add of 4 is then logged with no
         * compaction, and a reopen holds {1, 2, 4} */
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multidictP *d = multidictNewP(ctx);
        for (int64_t k = 1; k <= 3; k++) {
            const databox key = databoxNewSigned(k);
            const databox val = databoxNewSigned(k * 100);
            PTEST_LOGGED(multidictAddP(d, &key, &val, NULL));
        }
        multidictCloseP(d);
        persistCtxFree(ctx);
        if (!ptestDamageLastWalEntry(basePath)) {
            ERR("could not damage the WAL");
        }

        const databox k1 = databoxNewSigned(1);
        const databox k3 = databoxNewSigned(3);
        const databox k4 = databoxNewSigned(4);
        const databox v9 = databoxNewSigned(900);
        ctx = persistCtxOpen(basePath, NULL);
        d = multidictOpenP(ctx);
        if (!d || multidictCountP(d) != 2 || multidictExistsP(d, &k3)) {
            ERR("lenient recovery kept %" PRIu64 " keys, want 1, 2",
                d ? multidictCountP(d) : 0);
        }
        databox got = {};
        if (!ptestExpectHealed(ctx, basePath)) {
            ERR("the damaged entry was not healed into the quarantine");
        }
        if (d && (multidictAddP(d, &k4, &v9, NULL) == PERSIST_LOG_REFUSED ||
                  !multidictFindP(d, &k1, &got) || got.data.i != 100)) {
            ERR("a change after the healed entry was refused");
        }
        multidictCloseP(d);
        persistCtxFree(ctx);

        ctx = persistCtxOpen(basePath, NULL);
        d = multidictOpenP(ctx);
        if (!d || multidictCountP(d) != 3 || !multidictExistsP(d, &k1) ||
            !multidictFindP(d, &k4, &got) || got.data.i != 900) {
            ERR("reopen does not hold 1, 2, 4");
        }
        multidictCloseP(d);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
