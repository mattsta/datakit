/* multimapP.c - Persistent multimap wrapper implementation
 *
 * See multimapP.h for API documentation.
 */

#include "multimapP.h"
#include "../cleaner.h"
#include "../datakit.h"

#include <inttypes.h>
#include <string.h>
#include <unistd.h>

defcleanerName(multimapP *, zfree, MultimapP);

/* Forward declaration for internal use */
extern bool persistCtxInitForType(persistCtx *ctx, const persistOps *ops);
extern bool persistCtxSaveSnapshot(persistCtx *ctx, void *structure,
                                   const persistOps *ops);

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

multimapP *multimapNewP(persistCtx *ctx, uint32_t elementsPerEntry) {
    return multimapNewConfigureP(ctx, elementsPerEntry, false, false,
                                 FLEX_CAP_LEVEL_2048);
}

multimapP *multimapNewConfigureP(persistCtx *ctx, uint32_t elementsPerEntry,
                                 bool isSet, bool compress,
                                 flexCapSizeLimit limit) {
    if (!ctx) {
        return NULL;
    }

    cleanerName(zfree, MultimapP) multimapP *mp = zcalloc(1, sizeof(*mp));
    mp->ctx = ctx;
    mp->elementsPerEntry = elementsPerEntry;

    /* Create underlying multimap */
    mp->m = multimapNewConfigure(elementsPerEntry, isSet, compress, limit);
    if (!mp->m) {
        return NULL;
    }

    /* Initialize persistence for multimap type */
    if (!persistCtxInitForType(ctx, &persistOpsMultimap)) {
        multimapFree(mp->m);
        return NULL;
    }

    /* Take initial snapshot */
    if (!persistCtxSaveSnapshot(ctx, mp->m, &persistOpsMultimap)) {
        multimapFree(mp->m);
        return NULL;
    }

    persistCtxClaim(ctx);
    multimapP *out = mp;
    mp = NULL;
    return out;
}

multimapP *multimapOpenP(persistCtx *ctx) {
    if (!ctx) {
        return NULL;
    }

    /* Check if files exist; bind the context (refused for another type, or
     * while a wrapper holds it) */
    if (!persistCtxExists(ctx->basePath) ||
        !persistCtxInitForType(ctx, &persistOpsMultimap)) {
        return NULL;
    }

    cleanerName(zfree, MultimapP) multimapP *mp = zcalloc(1, sizeof(*mp));
    mp->ctx = ctx;

    /* Recover from snapshot + WAL */
    mp->m = persistCtxRecover(ctx, &persistOpsMultimap);
    if (!mp->m) {
        return NULL;
    }

    /* Get elementsPerEntry from recovered multimap */
    multimapIterator iter;
    multimapIteratorInit(mp->m, &iter, true);
    mp->elementsPerEntry = iter.elementsPerEntry;

    persistCtxClaim(ctx);
    multimapP *out = mp;
    mp = NULL;
    return out;
}

void multimapCloseP(multimapP *m) {
    if (!m) {
        return;
    }

    /* Sync before closing; the context is free for another wrapper */
    if (m->ctx && m->ctx->p) {
        persistSync(m->ctx->p);
    }
    persistCtxRelease(m->ctx);

    /* Free underlying multimap */
    if (m->m) {
        multimapFree(m->m);
    }

    zfree(m);
}

multimap *multimapGetP(multimapP *m) {
    return m ? m->m : NULL;
}

/* ============================================================================
 * Metadata
 * ============================================================================
 */

size_t multimapCountP(const multimapP *m) {
    return m && m->m ? multimapCount(m->m) : 0;
}

size_t multimapBytesP(const multimapP *m) {
    return m && m->m ? multimapBytes(m->m) : 0;
}

/* ============================================================================
 * Mutations (with persistence)
 * ============================================================================
 */

static bool multimapLoggableP(const multimapP *m) {
    return m && m->m && m->ctx;
}

persistLogResult multimapInsertP(multimapP *m, const databox *elements[],
                                 bool *replaced) {
    if (replaced) {
        *replaced = false;
    }
    if (!multimapLoggableP(m)) {
        return PERSIST_LOG_REFUSED;
    }

    const persistLogResult logged = persistCtxLogOp(
        m->ctx, PERSIST_OP_INSERT, elements, m->elementsPerEntry);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }
    const bool wasReplaced = multimapInsert(&m->m, elements);
    if (replaced) {
        *replaced = wasReplaced;
    }
    persistCtxMaybeCompact(m->ctx, m->m, &persistOpsMultimap);
    return logged;
}

persistLogResult multimapInsertIfNotExistsP(multimapP *m,
                                            const databox *elements[]) {
    if (!multimapLoggableP(m)) {
        return PERSIST_LOG_REFUSED;
    }
    /* Inserted by the one search that finds the key absent, then logged; a
     * refused log deletes the row again, which leaves the map as it was
     * (no row had the key) */
    if (!multimapInsertIfAbsent(&m->m, elements)) {
        return PERSIST_LOG_UNCHANGED;
    }

    const persistLogResult logged = persistCtxLogOp(
        m->ctx, PERSIST_OP_INSERT, elements, m->elementsPerEntry);
    if (logged == PERSIST_LOG_REFUSED) {
        multimapDelete(&m->m, elements[0]);
        return logged;
    }
    persistCtxMaybeCompact(m->ctx, m->m, &persistOpsMultimap);
    return logged;
}

persistLogResult multimapDeleteP(multimapP *m, const databox *key) {
    if (!multimapLoggableP(m)) {
        return PERSIST_LOG_REFUSED;
    }

    /* The row found here is deleted where it is once logged (logging
     * leaves the map as it was): one search */
    multimapEntry me = {};
    if (!multimapGetUnderlyingEntry(m->m, key, &me)) {
        return PERSIST_LOG_UNCHANGED;
    }

    const databox *keyArray[] = {key};
    const persistLogResult logged =
        persistCtxLogOp(m->ctx, PERSIST_OP_DELETE, keyArray, 1);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }
    multimapDeleteEntry(&m->m, &me);
    persistCtxMaybeCompact(m->ctx, m->m, &persistOpsMultimap);
    return logged;
}

persistLogResult multimapDeleteFullWidthP(multimapP *m,
                                          const databox *elements[]) {
    if (!multimapLoggableP(m)) {
        return PERSIST_LOG_REFUSED;
    }
    /* As multimapDeleteP: the row found here is deleted where it is once
     * logged, one search */
    multimapEntry me = {};
    if (!multimapGetUnderlyingEntryFullWidth(m->m, elements, &me)) {
        return PERSIST_LOG_UNCHANGED;
    }

    /* The full row, so replay removes the same row */
    const persistLogResult logged = persistCtxLogOp(
        m->ctx, PERSIST_OP_DELETE, elements, m->elementsPerEntry);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }
    multimapDeleteEntry(&m->m, &me);
    persistCtxMaybeCompact(m->ctx, m->m, &persistOpsMultimap);
    return logged;
}

persistLogResult multimapResetP(multimapP *m) {
    if (!multimapLoggableP(m)) {
        return PERSIST_LOG_REFUSED;
    }

    /* Reset only what the WAL records as reset */
    const persistLogResult logged =
        persistCtxLogOp(m->ctx, PERSIST_OP_CLEAR, NULL, 0);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }
    multimapReset(&m->m);

    /* The WAL is all stale now: compaction makes the empty map the
     * snapshot (best effort: on failure the WAL still replays to it) */
    persistCtxCompact(m->ctx, m->m, &persistOpsMultimap);
    return logged;
}

/* An increment's log, written when the map asks (the increment will
 * apply); a refused log declines it */
typedef struct mmpIncr_ {
    persistCtx *ctx;
    const databox *args[3];
    persistLogResult logged;
} mmpIncr_;

static bool mmpLogIncr_(void *ctx) {
    mmpIncr_ *incr = ctx;
    incr->logged =
        persistCtxLogOp(incr->ctx, PERSIST_OP_NUM_INCR, incr->args, 3);
    return incr->logged != PERSIST_LOG_REFUSED;
}

persistLogResult multimapFieldIncrP(multimapP *m, const databox *key,
                                    uint32_t fieldOffset, int64_t incrBy,
                                    int64_t *newValue) {
    if (newValue) {
        *newValue = 0;
    }
    if (!multimapLoggableP(m)) {
        return PERSIST_LOG_REFUSED;
    }

    /* The map decides and applies in one search and asks only once the
     * increment will change the field, so a refusal (the map's or the
     * log's) is never logged and leaves the field as it was.  Logged as
     * the increment, not the result. */
    const databox offsetBox = DATABOX_UNSIGNED(fieldOffset);
    const databox deltaBox = DATABOX_SIGNED(incrBy);
    mmpIncr_ incr = {.ctx = m->ctx,
                     .args = {key, &offsetBox, &deltaBox},
                     .logged = PERSIST_LOG_REFUSED};
    if (!multimapFieldIncrByIf(&m->m, key, fieldOffset, incrBy, newValue,
                               mmpLogIncr_, &incr)) {
        return PERSIST_LOG_REFUSED;
    }
    if (incrBy == 0) {
        return PERSIST_LOG_UNCHANGED;
    }
    persistCtxMaybeCompact(m->ctx, m->m, &persistOpsMultimap);
    return incr.logged;
}

/* ============================================================================
 * Lookups (read-only, delegate to underlying multimap)
 * ============================================================================
 */

bool multimapLookupP(multimapP *m, const databox *key, databox *elements[]) {
    return m && m->m ? multimapLookup(m->m, key, elements) : false;
}

bool multimapExistsP(multimapP *m, const databox *key) {
    return m && m->m ? multimapExists(m->m, key) : false;
}

bool multimapFirstP(multimapP *m, databox *elements[]) {
    return m && m->m ? multimapFirst(m->m, elements) : false;
}

bool multimapLastP(multimapP *m, databox *elements[]) {
    return m && m->m ? multimapLast(m->m, elements) : false;
}

bool multimapRandomValueP(multimapP *m, bool fromTail, databox **foundBox,
                          multimapEntry *me) {
    return m && m->m ? multimapRandomValue(m->m, fromTail, foundBox, me)
                     : false;
}

/* ============================================================================
 * Iteration
 * ============================================================================
 */

void multimapIteratorInitP(multimapP *m, multimapIterator *iter, bool forward) {
    if (m && m->m) {
        multimapIteratorInit(m->m, iter, forward);
    }
}

bool multimapIteratorInitAtP(multimapP *m, multimapIterator *iter, bool forward,
                             const databox *startAt) {
    return m && m->m ? multimapIteratorInitAt(m->m, iter, forward, startAt)
                     : false;
}

/* ============================================================================
 * Persistence Control
 * ============================================================================
 */

bool multimapSyncP(multimapP *m) {
    return m && m->ctx ? persistCtxSync(m->ctx) : false;
}

bool multimapCompactP(multimapP *m) {
    return m && m->ctx && m->m
               ? persistCtxCompact(m->ctx, m->m, &persistOpsMultimap)
               : false;
}

void multimapGetStatsP(const multimapP *m, persistCtxStats *stats) {
    if (m && m->ctx) {
        persistCtxGetStats(m->ctx, stats);
    }
}

/* ============================================================================
 * Tests
 * ============================================================================
 */

#ifdef DATAKIT_TEST
#include "../dkDiag.h"
#include "persistTestCommon.h"

/* Helper to verify multimap matches tracker */
static bool verifyMultimapMatchesTracker(multimapP *m,
                                         const ptestKVTracker *tracker) {
    if (multimapCountP(m) != tracker->count) {
        printf("  [verify] Count mismatch: multimap has %zu, tracker has %u\n",
               multimapCountP(m), tracker->count);
        return false;
    }

    /* Verify all tracker entries exist in multimap with correct values */
    for (uint32_t i = 0; i < tracker->count; i++) {
        databox foundVal;
        databox *found[] = {&foundVal};
        if (!multimapLookupP(m, &tracker->keys[i], found)) {
            printf("  [verify] Key at index %u not found\n", i);
            return false;
        }
        if (!ptestBoxesEqual(&foundVal, &tracker->values[i])) {
            printf("  [verify] Value mismatch at index %u\n", i);
            return false;
        }
    }

    return true;
}

int multimapPTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multimapPTest", basePath, sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multimapP keeps its size limit and compression across reopens") {
        /* Configured with the 512-byte limit and compression: a reopen
         * through the WAL, and one after a compaction (a Full snapshot),
         * both come back with that configuration and every row */
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multimapP *m =
            multimapNewConfigureP(ctx, 2, false, true, FLEX_CAP_LEVEL_512);
        for (int64_t k = 0; m && k < 1500; k++) {
            const databox key = databoxNewSigned(k);
            const databox val = databoxNewSigned(k);
            const databox *row[] = {&key, &val};
            PTEST_LOGGED(multimapInsertP(m, row, NULL));
        }
        multimapCloseP(m);
        persistCtxFree(ctx);
        for (int compacted = 0; compacted <= 1; compacted++) {
            ctx = persistCtxOpen(basePath, NULL);
            m = multimapOpenP(ctx);
            const multimap *raw = m ? multimapGetP(m) : NULL;
            if (!raw || multimapLimit(raw) != FLEX_CAP_LEVEL_512 ||
                !multimapCompresses(raw) || multimapCountP(m) != 1500) {
                ERR("reopen %d lost the configuration or rows", compacted);
            }
            if (m && !compacted && !multimapCompactP(m)) {
                ERR("compaction failed");
            }
            multimapCloseP(m);
            persistCtxFree(ctx);
        }
        ptestCleanupFiles(basePath);
    }

    TEST("multimapP create empty and close") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        if (!ctx) {
            ERR("Failed to create persistCtx");
        }

        multimapP *m = multimapNewP(ctx, 2);
        if (!m) {
            persistCtxFree(ctx);
            ERR("Failed to create multimapP");
        }

        if (multimapCountP(m) != 0) {
            ERR("New multimap should be empty");
        }

        multimapCloseP(m);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multimapP insert and lookup") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multimapP *m = multimapNewP(ctx, 2);
        ptestKVTracker tracker;
        ptestKVTrackerInit(&tracker);

        /* Insert entries */
        for (int i = 0; i < 20; i++) {
            databox key = DATABOX_SIGNED(i);
            databox val = DATABOX_SIGNED(i * 100);
            const databox *entry[] = {&key, &val};
            PTEST_LOGGED(multimapInsertP(m, entry, NULL));
            ptestKVTrackerInsert(&tracker, &key, &val);
        }

        if (!verifyMultimapMatchesTracker(m, &tracker)) {
            ERR("Multimap/tracker mismatch after inserts");
        }

        multimapCloseP(m);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multimapP update existing keys") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multimapP *m = multimapNewP(ctx, 2);
        ptestKVTracker tracker;
        ptestKVTrackerInit(&tracker);

        /* Insert initial values */
        for (int i = 0; i < 10; i++) {
            databox key = DATABOX_SIGNED(i);
            databox val = DATABOX_SIGNED(i * 10);
            const databox *entry[] = {&key, &val};
            PTEST_LOGGED(multimapInsertP(m, entry, NULL));
            ptestKVTrackerInsert(&tracker, &key, &val);
        }

        /* Update with new values */
        for (int i = 0; i < 10; i++) {
            databox key = DATABOX_SIGNED(i);
            databox val = DATABOX_SIGNED(i * 1000);
            const databox *entry[] = {&key, &val};
            PTEST_LOGGED(multimapInsertP(m, entry, NULL));
            ptestKVTrackerInsert(&tracker, &key, &val);
        }

        if (!verifyMultimapMatchesTracker(m, &tracker)) {
            ERR("Multimap/tracker mismatch after updates");
        }

        multimapCloseP(m);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multimapP recovery") {
        ptestCleanupFiles(basePath);
        ptestKVTracker tracker;
        ptestKVTrackerInit(&tracker);

        /* Create and populate */
        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            multimapP *m = multimapNewP(ctx, 2);

            for (int i = 0; i < 30; i++) {
                databox key = DATABOX_SIGNED(i);
                databox val = DATABOX_SIGNED(i * 50);
                const databox *entry[] = {&key, &val};
                PTEST_LOGGED(multimapInsertP(m, entry, NULL));
                ptestKVTrackerInsert(&tracker, &key, &val);
            }

            multimapCloseP(m);
            persistCtxFree(ctx);
        }

        /* Recover and verify */
        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multimapP *m = multimapOpenP(ctx);

            if (!m) {
                ERR("Failed to recover multimapP");
            }

            if (!verifyMultimapMatchesTracker(m, &tracker)) {
                ERR("Multimap/tracker mismatch after recovery");
            }

            multimapCloseP(m);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("multimapP delete operations") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multimapP *m = multimapNewP(ctx, 2);
        ptestKVTracker tracker;
        ptestKVTrackerInit(&tracker);

        /* Insert entries */
        for (int i = 0; i < 20; i++) {
            databox key = DATABOX_SIGNED(i);
            databox val = DATABOX_SIGNED(i * 100);
            const databox *entry[] = {&key, &val};
            PTEST_LOGGED(multimapInsertP(m, entry, NULL));
            ptestKVTrackerInsert(&tracker, &key, &val);
        }

        /* Delete every other entry */
        for (int i = 0; i < 20; i += 2) {
            databox key = DATABOX_SIGNED(i);
            PTEST_LOGGED(multimapDeleteP(m, &key));
            ptestKVTrackerDelete(&tracker, &key);
        }

        if (!verifyMultimapMatchesTracker(m, &tracker)) {
            ERR("Multimap/tracker mismatch after deletes");
        }

        /* Verify deleted keys don't exist */
        for (int i = 0; i < 20; i += 2) {
            databox key = DATABOX_SIGNED(i);
            if (multimapExistsP(m, &key)) {
                ERR("Deleted key %d should not exist", i);
            }
        }

        multimapCloseP(m);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multimapP delete recovery") {
        ptestCleanupFiles(basePath);
        ptestKVTracker tracker;
        ptestKVTrackerInit(&tracker);

        /* Create, populate, delete, close */
        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            multimapP *m = multimapNewP(ctx, 2);

            for (int i = 0; i < 15; i++) {
                databox key = DATABOX_SIGNED(i);
                databox val = DATABOX_SIGNED(i * 50);
                const databox *entry[] = {&key, &val};
                PTEST_LOGGED(multimapInsertP(m, entry, NULL));
                ptestKVTrackerInsert(&tracker, &key, &val);
            }

            /* Delete entries 3, 7, 11 */
            for (int i = 3; i < 15; i += 4) {
                databox key = DATABOX_SIGNED(i);
                PTEST_LOGGED(multimapDeleteP(m, &key));
                ptestKVTrackerDelete(&tracker, &key);
            }

            multimapCloseP(m);
            persistCtxFree(ctx);
        }

        /* Recover and verify */
        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multimapP *m = multimapOpenP(ctx);

            if (!m) {
                ERR("Failed to recover multimapP after delete");
            }

            if (!verifyMultimapMatchesTracker(m, &tracker)) {
                ERR("Multimap/tracker mismatch after delete recovery");
            }

            multimapCloseP(m);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("multimapP reset and continue") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multimapP *m = multimapNewP(ctx, 2);
        ptestKVTracker tracker;
        ptestKVTrackerInit(&tracker);

        /* Add entries */
        for (int i = 0; i < 10; i++) {
            databox key = DATABOX_SIGNED(i);
            databox val = DATABOX_SIGNED(i);
            const databox *entry[] = {&key, &val};
            PTEST_LOGGED(multimapInsertP(m, entry, NULL));
        }

        if (multimapCountP(m) != 10) {
            ERR("Count should be 10 before reset");
        }

        /* Reset the multimap */
        PTEST_LOGGED(multimapResetP(m));

        if (multimapCountP(m) != 0) {
            ERR("Count should be 0 after reset");
        }

        /* Add new entries */
        for (int i = 100; i < 115; i++) {
            databox key = DATABOX_SIGNED(i);
            databox val = DATABOX_SIGNED(i * 2);
            const databox *entry[] = {&key, &val};
            PTEST_LOGGED(multimapInsertP(m, entry, NULL));
            ptestKVTrackerInsert(&tracker, &key, &val);
        }

        if (!verifyMultimapMatchesTracker(m, &tracker)) {
            ERR("Multimap/tracker mismatch after reset and re-add");
        }

        multimapCloseP(m);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multimapP reset recovery") {
        ptestCleanupFiles(basePath);
        ptestKVTracker tracker;
        ptestKVTrackerInit(&tracker);

        /* Create, populate, reset, add more, close */
        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            multimapP *m = multimapNewP(ctx, 2);

            for (int i = 0; i < 10; i++) {
                databox key = DATABOX_SIGNED(i);
                databox val = DATABOX_SIGNED(i);
                const databox *entry[] = {&key, &val};
                PTEST_LOGGED(multimapInsertP(m, entry, NULL));
            }

            PTEST_LOGGED(multimapResetP(m));

            for (int i = 50; i < 60; i++) {
                databox key = DATABOX_SIGNED(i);
                databox val = DATABOX_SIGNED(i * 3);
                const databox *entry[] = {&key, &val};
                PTEST_LOGGED(multimapInsertP(m, entry, NULL));
                ptestKVTrackerInsert(&tracker, &key, &val);
            }

            multimapCloseP(m);
            persistCtxFree(ctx);
        }

        /* Recover and verify */
        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multimapP *m = multimapOpenP(ctx);

            if (!m) {
                ERR("Failed to recover multimapP after reset");
            }

            if (!verifyMultimapMatchesTracker(m, &tracker)) {
                ERR("Multimap/tracker mismatch after reset recovery");
            }

            multimapCloseP(m);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("multimapP mixed operations") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multimapP *m = multimapNewP(ctx, 2);
        ptestKVTracker tracker;
        ptestKVTrackerInit(&tracker);

        /* Interleave adds, updates, and deletes */
        for (int round = 0; round < 5; round++) {
            /* Add some entries */
            for (int i = round * 10; i < round * 10 + 8; i++) {
                databox key = DATABOX_SIGNED(i);
                databox val = DATABOX_SIGNED(i * round);
                const databox *entry[] = {&key, &val};
                PTEST_LOGGED(multimapInsertP(m, entry, NULL));
                ptestKVTrackerInsert(&tracker, &key, &val);
            }

            /* Update a few */
            for (int i = round * 10; i < round * 10 + 3; i++) {
                databox key = DATABOX_SIGNED(i);
                databox val = DATABOX_SIGNED(i * 9999);
                const databox *entry[] = {&key, &val};
                PTEST_LOGGED(multimapInsertP(m, entry, NULL));
                ptestKVTrackerInsert(&tracker, &key, &val);
            }

            /* Delete one */
            databox key = DATABOX_SIGNED(round * 10 + 5);
            PTEST_LOGGED(multimapDeleteP(m, &key));
            ptestKVTrackerDelete(&tracker, &key);
        }

        if (!verifyMultimapMatchesTracker(m, &tracker)) {
            ERR("Multimap/tracker mismatch after mixed ops");
        }

        multimapCloseP(m);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multimapP multi-cycle recovery") {
        ptestCleanupFiles(basePath);
        ptestKVTracker tracker;
        ptestKVTrackerInit(&tracker);

        for (int cycle = 0; cycle < PTEST_RECOVERY_CYCLES; cycle++) {
            persistCtx *ctx;
            multimapP *m;

            if (cycle == 0) {
                ctx = persistCtxNew(basePath, NULL);
                m = multimapNewP(ctx, 2);
            } else {
                ctx = persistCtxOpen(basePath, NULL);
                m = multimapOpenP(ctx);
            }

            if (!m) {
                ERR("Failed to open multimapP in cycle %d", cycle);
                continue;
            }

            /* Add new entries each cycle */
            for (int i = 0; i < 10; i++) {
                databox key = DATABOX_SIGNED(cycle * 100 + i);
                databox val = DATABOX_SIGNED(cycle * 1000 + i);
                const databox *entry[] = {&key, &val};
                PTEST_LOGGED(multimapInsertP(m, entry, NULL));
                ptestKVTrackerInsert(&tracker, &key, &val);
            }

            if (!verifyMultimapMatchesTracker(m, &tracker)) {
                ERR("Multimap/tracker mismatch in cycle %d", cycle);
            }

            multimapCloseP(m);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("multimapP large dataset") {
        /* Note: Limited to 200 entries due to WAL buffer constraints */
        ptestCleanupFiles(basePath);
        ptestKVTracker tracker;
        ptestKVTrackerInit(&tracker);

        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            multimapP *m = multimapNewP(ctx, 2);

            for (int i = 0; i < 200; i++) {
                databox key = DATABOX_SIGNED(i);
                databox val = DATABOX_SIGNED(i * 1000);
                const databox *entry[] = {&key, &val};
                PTEST_LOGGED(multimapInsertP(m, entry, NULL));
                ptestKVTrackerInsert(&tracker, &key, &val);
            }

            multimapCloseP(m);
            persistCtxFree(ctx);
        }

        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multimapP *m = multimapOpenP(ctx);

            if (!m) {
                ERR("Large dataset recovery failed");
            }

            if (!verifyMultimapMatchesTracker(m, &tracker)) {
                ERR("Large dataset verification failed");
            }

            multimapCloseP(m);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("multimapP empty recovery") {
        ptestCleanupFiles(basePath);

        /* Create empty and close */
        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            multimapP *m = multimapNewP(ctx, 2);
            multimapCloseP(m);
            persistCtxFree(ctx);
        }

        /* Recover and verify empty */
        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multimapP *m = multimapOpenP(ctx);

            if (!m) {
                ERR("Empty recovery failed");
            }

            if (multimapCountP(m) != 0) {
                ERR("Recovered empty multimap should have 0 entries");
            }

            multimapCloseP(m);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("multimapP single entry recovery") {
        ptestCleanupFiles(basePath);
        databox expectedKey = DATABOX_SIGNED(42);
        databox expectedVal = DATABOX_SIGNED(4200);

        /* Create with single entry */
        {
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            multimapP *m = multimapNewP(ctx, 2);
            const databox *entry[] = {&expectedKey, &expectedVal};
            PTEST_LOGGED(multimapInsertP(m, entry, NULL));
            multimapCloseP(m);
            persistCtxFree(ctx);
        }

        /* Recover and verify */
        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multimapP *m = multimapOpenP(ctx);

            if (!m) {
                ERR("Single entry recovery failed");
            }

            if (multimapCountP(m) != 1) {
                ERR("Should have exactly 1 entry");
            }

            databox foundVal;
            databox *found[] = {&foundVal};
            if (!multimapLookupP(m, &expectedKey, found)) {
                ERR("Expected key not found");
            }

            if (!ptestBoxesEqual(&foundVal, &expectedVal)) {
                ERR("Value mismatch after recovery");
            }

            multimapCloseP(m);
            persistCtxFree(ctx);
        }

        ptestCleanupFiles(basePath);
    }

    TEST("multimapP statistics tracking") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multimapP *m = multimapNewP(ctx, 2);

        /* Add entries */
        for (int i = 0; i < 15; i++) {
            databox key = DATABOX_SIGNED(i);
            databox val = DATABOX_SIGNED(i);
            const databox *entry[] = {&key, &val};
            PTEST_LOGGED(multimapInsertP(m, entry, NULL));
        }

        /* Delete a few */
        for (int i = 0; i < 5; i++) {
            databox key = DATABOX_SIGNED(i);
            PTEST_LOGGED(multimapDeleteP(m, &key));
        }

        persistCtxStats stats;
        multimapGetStatsP(m, &stats);

        /* 15 inserts + 5 deletes = 20 total ops */
        if (stats.totalOps != 20) {
            ERR("Total ops should be 20, got %" PRIu64, stats.totalOps);
        }

        multimapCloseP(m);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multimapP reset then reuse survives recovery") {
        /* Reset logs a CLEAR op; a post-reset refill must survive
         * close + WAL/snapshot recovery with no pre-reset ghosts */
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multimapP *m = multimapNewP(ctx, 2);

        for (int i = 0; i < 30; i++) {
            databox key = DATABOX_SIGNED(i);
            databox val = DATABOX_SIGNED(i * 10);
            const databox *entry[] = {&key, &val};
            PTEST_LOGGED(multimapInsertP(m, entry, NULL));
        }

        PTEST_LOGGED(multimapResetP(m));
        if (multimapCountP(m) != 0) {
            ERR("Expected empty after reset");
        }

        /* Refill with DIFFERENT keys so any ghost from before the
         * reset is detectable after recovery */
        for (int i = 100; i < 120; i++) {
            databox key = DATABOX_SIGNED(i);
            databox val = DATABOX_SIGNED(i * 7);
            const databox *entry[] = {&key, &val};
            PTEST_LOGGED(multimapInsertP(m, entry, NULL));
        }

        multimapCloseP(m);
        persistCtxFree(ctx);

        /* Recover and verify: exactly the post-reset contents */
        ctx = persistCtxNew(basePath, NULL);
        m = multimapOpenP(ctx);
        if (!m) {
            ERR("Recovery after reset failed");
        } else {
            if (multimapCountP(m) != 20) {
                ERR("Expected 20 post-recovery, got %zu", multimapCountP(m));
            }

            databox probeOld = DATABOX_SIGNED(3);
            if (multimapExistsP(m, &probeOld)) {
                ERR("Pre-reset ghost key survived recovery");
            }

            databox probeNew = DATABOX_SIGNED(110);
            if (!multimapExistsP(m, &probeNew)) {
                ERR("Post-reset key lost in recovery");
            }

            multimapCloseP(m);
        }
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multimapP field increments and insert-if-absent survive recovery") {
        /* key -> [counter, label]; increments replay as increments, a
         * refused one (missing key, a bytes field) changes nothing */
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multimapP *m = multimapNewP(ctx, 3);
        const databox label = databoxNewBytesString("label");
        for (int64_t i = 0; i < 10; i++) {
            const databox key = DATABOX_SIGNED(i);
            const databox counter = DATABOX_SIGNED(i);
            const databox *row[] = {&key, &counter, &label};
            PTEST_LOGGED(multimapInsertP(m, row, NULL));
        }

        const databox k3 = DATABOX_SIGNED(3);
        const databox k4 = DATABOX_SIGNED(4);
        const databox k5 = DATABOX_SIGNED(5);
        const databox k50 = DATABOX_SIGNED(50);
        const databox k99 = DATABOX_SIGNED(99);
        int64_t value = 0;
        if (!multimapFieldIncrP(m, &k3, 1, 5, &value) || value != 8) {
            ERR("3 + 5 should be 8");
        }
        if (!multimapFieldIncrP(m, &k4, 1, -10, &value) || value != -6) {
            ERR("4 - 10 should be -6");
        }

        /* Refused increments, an overflow included, log nothing */
        persistCtxStats before;
        persistCtxGetStats(ctx, &before);
        if (multimapFieldIncrP(m, &k99, 1, 1, &value) != PERSIST_LOG_REFUSED ||
            value != 0) {
            ERR("A missing key's increment was not refused with 0");
        }
        if (multimapFieldIncrP(m, &k3, 2, 1, &value) != PERSIST_LOG_REFUSED ||
            value != 0) {
            ERR("A bytes field's increment was not refused with 0");
        }
        if (multimapFieldIncrP(m, &k3, 1, INT64_MAX, &value) !=
                PERSIST_LOG_REFUSED ||
            value != 8) {
            ERR("An overflowing increment was not refused at 8");
        }
        if (multimapFieldIncrP(m, &k3, 1, 0, &value) != PERSIST_LOG_UNCHANGED ||
            value != 8) {
            ERR("An increment by 0 was logged");
        }
        const databox other = DATABOX_SIGNED(-1);
        const databox *dup[] = {&k5, &other, &label};
        if (multimapInsertIfNotExistsP(m, dup) != PERSIST_LOG_UNCHANGED) {
            ERR("Insert-if-absent replaced key 5");
        }
        persistCtxStats after;
        persistCtxGetStats(ctx, &after);
        if (after.totalOps != before.totalOps) {
            ERR("refused or unchanged ops wrote WAL entries");
        }
        const databox fifty = DATABOX_SIGNED(500);
        const databox *added[] = {&k50, &fifty, &label};
        if (!multimapInsertIfNotExistsP(m, added)) {
            ERR("Insert-if-absent refused absent key 50");
        }
        if (!multimapSyncP(m) || multimapBytesP(m) == 0) {
            ERR("Sync or byte count failed");
        }
        multimapCloseP(m);
        persistCtxFree(ctx);

        ctx = persistCtxOpen(basePath, NULL);
        m = multimapOpenP(ctx);
        if (!m) {
            ERR("Recovery failed");
        } else {
            static const struct {
                int64_t key;
                int64_t counter;
            } want[] = {{3, 8}, {4, -6}, {5, 5}, {50, 500}};
            for (size_t i = 0; i < sizeof(want) / sizeof(*want); i++) {
                const databox key = DATABOX_SIGNED(want[i].key);
                const databox expect = DATABOX_SIGNED(want[i].counter);
                databox vals[2];
                databox *out[] = {&vals[0], &vals[1]};
                if (!multimapLookupP(m, &key, out) ||
                    !databoxEqual(&vals[0], &expect)) {
                    ERR("key %" PRId64 ": counter not %" PRId64
                        " after recovery",
                        want[i].key, want[i].counter);
                }
            }
            if (multimapCountP(m) != 11) {
                ERR("Expected 11 rows, got %zu", multimapCountP(m));
            }
            multimapCloseP(m);
        }
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multimapP set: full-row delete removes that row after recovery") {
        /* One key with several rows: replay must remove the same row,
         * not the key's first row */
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multimapP *m =
            multimapNewConfigureP(ctx, 2, true, false, FLEX_CAP_LEVEL_2048);
        static constexpr int64_t rows[][2] = {
            {1, 10}, {1, 11}, {1, 12}, {2, 20}};
        for (size_t i = 0; i < 4; i++) {
            const databox key = DATABOX_SIGNED(rows[i][0]);
            const databox val = DATABOX_SIGNED(rows[i][1]);
            const databox *row[] = {&key, &val};
            PTEST_LOGGED(multimapInsertP(m, row, NULL));
        }
        const databox one = DATABOX_SIGNED(1);
        const databox eleven = DATABOX_SIGNED(11);
        const databox *gone[] = {&one, &eleven};
        if (!multimapDeleteFullWidthP(m, gone) ||
            multimapDeleteFullWidthP(m, gone) != PERSIST_LOG_UNCHANGED) {
            ERR("Full-row delete found no row, or deleted it twice");
        }
        /* An increment in a set is refused as misuse, and logs nothing */
        const dkDiagPrintMode priorPrint = dkDiagGetPrintMode();
        dkDiagSetPrintMode(DK_DIAG_PRINT_NEVER);
        persistLogResult incr = PERSIST_LOG_BUFFERED;
        DK_DIAG_EXPECT_REPORTS("a set", 1, DK_DIAG_MULTIMAP_SET_REWRITE,
                               incr = multimapFieldIncrP(m, &one, 1, 1, NULL));
        dkDiagSetPrintMode(priorPrint);
        if (incr != PERSIST_LOG_REFUSED) {
            ERR("an increment in a set was not refused");
        }
        multimapCloseP(m);
        persistCtxFree(ctx);

        ctx = persistCtxOpen(basePath, NULL);
        m = multimapOpenP(ctx);
        if (!m) {
            ERR("Recovery failed");
        } else {
            if (!multimapIsSet(multimapGetP(m))) {
                ERR("A set came back as a map");
            }
            static constexpr int64_t remain[][2] = {{1, 10}, {1, 12}, {2, 20}};
            multimapIterator iter;
            multimapIteratorInitP(m, &iter, true);
            databox got[2];
            databox *out[] = {&got[0], &got[1]};
            size_t n = 0;
            while (multimapIteratorNext(&iter, out)) {
                const databox k = DATABOX_SIGNED(remain[n < 3 ? n : 2][0]);
                const databox v = DATABOX_SIGNED(remain[n < 3 ? n : 2][1]);
                if (n >= 3 || !databoxEqual(&got[0], &k) ||
                    !databoxEqual(&got[1], &v)) {
                    ERR("Row %zu differs after recovery", n);
                }
                n++;
            }
            if (n != 3) {
                ERR("Expected 3 rows, got %zu", n);
            }

            /* The readers see the same rows */
            const databox two = DATABOX_SIGNED(2);
            if (!multimapFirstP(m, out) || !databoxEqual(&got[0], &one) ||
                !multimapLastP(m, out) || !databoxEqual(&got[0], &two) ||
                !multimapIteratorInitAtP(m, &iter, true, &two) ||
                !multimapIteratorNext(&iter, out) ||
                !databoxEqual(&got[0], &two)) {
                ERR("First/Last/IteratorInitAt disagree with the rows");
            }
            multimapEntry me;
            if (!multimapRandomValueP(m, false, out, &me)) {
                ERR("RandomValue found no row");
            }
            multimapCloseP(m);
        }
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multimapP a refused log changes nothing (insert-if-absent, "
         "increment, full-row delete)") {
        /* key -> counter, the counters a boolean and an unsigned: a
         * refused increment keeps the field's value and its type */
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multimapP *m = multimapNewP(ctx, 2);
        const databox k1 = DATABOX_SIGNED(1);
        const databox k2 = DATABOX_SIGNED(2);
        const databox k3 = DATABOX_SIGNED(3);
        const databox yes = DATABOX_BOX_TRUE;
        const databox nine = DATABOX_UNSIGNED(9);
        const databox thirty = DATABOX_SIGNED(30);
        const databox *row1[] = {&k1, &yes};
        const databox *row2[] = {&k2, &nine};
        const databox *row3[] = {&k3, &thirty};
        PTEST_LOGGED(multimapInsertP(m, row1, NULL));
        PTEST_LOGGED(multimapInsertP(m, row2, NULL));

        int64_t now1 = -1;
        int64_t now2 = -1;
        bool replaced = true;
        PTEST_EXPECT_REFUSED(ctx, multimapInsertIfNotExistsP(m, row3),
                             multimapFieldIncrP(m, &k1, 1, 1, &now1),
                             multimapFieldIncrP(m, &k2, 1, 1, &now2),
                             multimapDeleteFullWidthP(m, row2),
                             multimapInsertP(m, row3, &replaced),
                             multimapDeleteP(m, &k1), multimapResetP(m));
        if (replaced) {
            ERR("a refused insert reported a replace");
        }
        if (now1 != 1 || now2 != 9) {
            ERR("refused increments reported %" PRId64 " and %" PRId64
                ", not the current 1 and 9",
                now1, now2);
        }
        databox got = {};
        databox *out[] = {&got};
        if (multimapCountP(m) != 2 || multimapExistsP(m, &k3) ||
            !multimapExistsFullWidth(multimapGetP(m), row2)) {
            ERR("a refused insert or delete changed the rows");
        }
        if (!multimapLookupP(m, &k1, out) || got.type != DATABOX_TRUE ||
            !multimapLookupP(m, &k2, out) || got.type != DATABOX_UNSIGNED_64 ||
            got.data.u != 9) {
            ERR("a refused increment changed a field's value or type");
        }

        /* Missing arguments: refused or empty answers */
        multimapIterator iter;
        if (multimapInsertP(NULL, row1, NULL) ||
            multimapInsertIfNotExistsP(NULL, row1) ||
            multimapDeleteP(NULL, &k1) ||
            multimapDeleteFullWidthP(NULL, row1) || multimapResetP(NULL) ||
            multimapFieldIncrP(NULL, &k1, 1, 1, NULL) ||
            multimapNewP(NULL, 2) || multimapOpenP(NULL) ||
            multimapGetP(NULL) || multimapCountP(NULL) ||
            multimapBytesP(NULL) || multimapExistsP(NULL, &k1) ||
            multimapLookupP(NULL, &k1, out) || multimapSyncP(NULL) ||
            multimapCompactP(NULL)) {
            ERR("a call without its wrapper was not refused");
        }
        multimapIteratorInitP(NULL, &iter, true);
        multimapGetStatsP(NULL, NULL);
        multimapCloseP(NULL);

        /* Logging works again (a replace reports itself), and a reopen
         * holds only what was logged */
        if (multimapInsertP(m, row1, &replaced) == PERSIST_LOG_REFUSED ||
            !replaced) {
            ERR("a logged replace did not report itself");
        }
        if (multimapFieldIncrP(m, &k1, 1, 1, &now1) == PERSIST_LOG_REFUSED ||
            now1 != 2) {
            ERR("the context did not log again after the refusal");
        }
        multimapCloseP(m);
        persistCtxFree(ctx);
        ctx = persistCtxOpen(basePath, NULL);
        m = multimapOpenP(ctx);
        if (!m || multimapCountP(m) != 2 || multimapExistsP(m, &k3)) {
            ERR("a reopen holds %zu rows, key 3 %s", multimapCountP(m),
                m && multimapExistsP(m, &k3) ? "present" : "absent");
        }
        /* The boolean became the integer 2 (non-negative integers read
         * back unsigned) */
        if (m && (!multimapLookupP(m, &k1, out) ||
                  got.type != DATABOX_UNSIGNED_64 || got.data.u != 2)) {
            ERR("after a reopen key 1 holds type %d value %" PRIu64
                ", not the integer 2",
                (int)got.type, got.data.u64);
        }
        if (m && (!multimapLookupP(m, &k2, out) ||
                  got.type != DATABOX_UNSIGNED_64 || got.data.u != 9)) {
            ERR("after a reopen key 2 holds type %d value %" PRIu64
                ", not the unsigned 9",
                (int)got.type, got.data.u64);
        }
        multimapCloseP(m);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("multimapP after a damaged WAL entry: healed, logging goes on") {
        /* Rows 1, 2, 3 inserted with the last entry damaged: lenient
         * recovery keeps {1, 2} and heals the WAL (the damaged entry moves
         * to the quarantine); an insert of 4 is then logged with no
         * compaction, and a reopen holds {1, 2, 4} */
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multimapP *m = multimapNewP(ctx, 2);
        for (int64_t k = 1; k <= 3; k++) {
            const databox key = databoxNewSigned(k);
            const databox val = databoxNewSigned(k * 100);
            const databox *row[2] = {&key, &val};
            PTEST_LOGGED(multimapInsertP(m, row, NULL));
        }
        multimapCloseP(m);
        persistCtxFree(ctx);
        if (!ptestDamageLastWalEntry(basePath)) {
            ERR("could not damage the WAL");
        }

        const databox k1 = databoxNewSigned(1);
        const databox k3 = databoxNewSigned(3);
        const databox k4 = databoxNewSigned(4);
        const databox v9 = databoxNewSigned(900);
        const databox *row4[2] = {&k4, &v9};
        databox got = {};
        databox *out[1] = {&got};
        ctx = persistCtxOpen(basePath, NULL);
        m = multimapOpenP(ctx);
        if (!m || multimapCountP(m) != 2 || multimapExistsP(m, &k3)) {
            ERR("lenient recovery kept %zu rows, want 1, 2",
                m ? multimapCountP(m) : (size_t)0);
        }
        if (!ptestExpectHealed(ctx, basePath)) {
            ERR("the damaged entry was not healed into the quarantine");
        }
        if (m && (multimapInsertP(m, row4, NULL) == PERSIST_LOG_REFUSED ||
                  !multimapLookupP(m, &k1, out) || got.data.i != 100)) {
            ERR("a change after the healed entry was refused");
        }
        multimapCloseP(m);
        persistCtxFree(ctx);

        ctx = persistCtxOpen(basePath, NULL);
        m = multimapOpenP(ctx);
        if (!m || multimapCountP(m) != 3 || !multimapExistsP(m, &k1) ||
            !multimapLookupP(m, &k4, out) || got.data.i != 900) {
            ERR("reopen does not hold 1, 2, 4");
        }
        multimapCloseP(m);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
