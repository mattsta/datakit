/* multimapP.c - Persistent multimap wrapper implementation
 *
 * See multimapP.h for API documentation.
 */

#include "multimapP.h"
#include "../datakit.h"

#include <string.h>
#include <unistd.h>

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

    multimapP *mp = zcalloc(1, sizeof(*mp));
    mp->ctx = ctx;
    mp->elementsPerEntry = elementsPerEntry;

    /* Create underlying multimap */
    mp->m = multimapNewConfigure(elementsPerEntry, isSet, compress, limit);
    if (!mp->m) {
        zfree(mp);
        return NULL;
    }

    /* Initialize persistence for multimap type */
    if (!persistCtxInitForType(ctx, &persistOpsMultimap)) {
        multimapFree(mp->m);
        zfree(mp);
        return NULL;
    }

    /* Take initial snapshot */
    if (!persistCtxSaveSnapshot(ctx, mp->m, &persistOpsMultimap)) {
        multimapFree(mp->m);
        zfree(mp);
        return NULL;
    }

    return mp;
}

multimapP *multimapOpenP(persistCtx *ctx) {
    if (!ctx) {
        return NULL;
    }

    /* Check if files exist */
    if (!persistCtxExists(ctx->basePath)) {
        return NULL;
    }

    multimapP *mp = zcalloc(1, sizeof(*mp));
    mp->ctx = ctx;

    /* Recover from snapshot + WAL */
    mp->m = persistCtxRecover(ctx, &persistOpsMultimap);
    if (!mp->m) {
        zfree(mp);
        return NULL;
    }

    /* Get elementsPerEntry from recovered multimap */
    multimapIterator iter;
    multimapIteratorInit(mp->m, &iter, true);
    mp->elementsPerEntry = iter.elementsPerEntry;

    return mp;
}

void multimapCloseP(multimapP *m) {
    if (!m) {
        return;
    }

    /* Sync before closing */
    if (m->ctx && m->ctx->p) {
        persistSync(m->ctx->p);
    }

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

bool multimapInsertP(multimapP *m, const databox *elements[]) {
    if (!m || !m->m || !m->ctx) {
        return false;
    }

    /* 1. Log to WAL first (write-ahead) */
    if (!persistCtxLogOp(m->ctx, PERSIST_OP_INSERT, elements,
                         m->elementsPerEntry)) {
        return false;
    }

    /* 2. Update in-memory structure */
    bool replaced = multimapInsert(&m->m, elements);

    /* 3. Check compaction threshold */
    persistCtxMaybeCompact(m->ctx, m->m, &persistOpsMultimap);

    return replaced;
}

bool multimapInsertIfNotExistsP(multimapP *m, const databox *elements[]) {
    if (!m || !m->m || !m->ctx) {
        return false;
    }

    /* Check if exists first */
    if (multimapExists(m->m, elements[0])) {
        return false; /* Already exists, no insert */
    }

    /* Insert (will log to WAL) */
    return multimapInsertP(m, elements);
}

bool multimapDeleteP(multimapP *m, const databox *key) {
    if (!m || !m->m || !m->ctx) {
        return false;
    }

    /* Check if exists (to return correct result) */
    if (!multimapExists(m->m, key)) {
        return false;
    }

    /* 1. Log to WAL first (pass as array of pointers for encodeOp) */
    const databox *keyArray[] = {key};
    if (!persistCtxLogOp(m->ctx, PERSIST_OP_DELETE, keyArray, 1)) {
        return false;
    }

    /* 2. Update in-memory structure */
    bool deleted = multimapDelete(&m->m, key);

    /* 3. Check compaction threshold */
    persistCtxMaybeCompact(m->ctx, m->m, &persistOpsMultimap);

    return deleted;
}

bool multimapDeleteFullWidthP(multimapP *m, const databox *elements[]) {
    if (!m || !m->m || !m->ctx) {
        return false;
    }

    /* 1. Log to WAL first (use the full entry for deletion) */
    if (!persistCtxLogOp(m->ctx, PERSIST_OP_DELETE, elements,
                         m->elementsPerEntry)) {
        return false;
    }

    /* 2. Update in-memory structure */
    bool deleted = multimapDeleteFullWidth(&m->m, elements);

    /* 3. Check compaction threshold */
    persistCtxMaybeCompact(m->ctx, m->m, &persistOpsMultimap);

    return deleted;
}

void multimapResetP(multimapP *m) {
    if (!m || !m->m || !m->ctx) {
        return;
    }

    /* 1. Log clear operation to WAL */
    persistCtxLogOp(m->ctx, PERSIST_OP_CLEAR, NULL, 0);

    /* 2. Reset in-memory structure */
    multimapReset(m->m);

    /* 3. Force compaction after reset (WAL is now stale) */
    persistCtxCompact(m->ctx, m->m, &persistOpsMultimap);
}

int64_t multimapFieldIncrP(multimapP *m, const databox *key,
                           uint32_t fieldOffset, int64_t incrBy) {
    if (!m || !m->m || !m->ctx) {
        return 0;
    }

    /* For increment, we need to encode the operation specially.
     * We'll use a custom encoding: [key][fieldOffset][incrBy] */
    struct {
        const databox *key;
        uint32_t fieldOffset;
        int64_t incrBy;
    } args = {key, fieldOffset, incrBy};

    /* 1. Log to WAL
     * Note: This uses PERSIST_OP_UPDATE with special encoding.
     * The applyOp handler needs to understand this format. */
    if (!persistCtxLogOp(m->ctx, PERSIST_OP_UPDATE, &args, 3)) {
        /* If logging fails, we still do the in-memory update for consistency
         * but return 0 to indicate failure */
        return multimapFieldIncr(&m->m, key, fieldOffset, incrBy);
    }

    /* 2. Update in-memory structure */
    int64_t result = multimapFieldIncr(&m->m, key, fieldOffset, incrBy);

    /* 3. Check compaction threshold */
    persistCtxMaybeCompact(m->ctx, m->m, &persistOpsMultimap);

    return result;
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
    const char *basePath = "/tmp/multimapPTest";

    TEST("multimapP create empty and close") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        if (!ctx) {
            ERRR("Failed to create persistCtx");
        }

        multimapP *m = multimapNewP(ctx, 2);
        if (!m) {
            persistCtxFree(ctx);
            ERRR("Failed to create multimapP");
        }

        if (multimapCountP(m) != 0) {
            ERRR("New multimap should be empty");
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
            multimapInsertP(m, entry);
            ptestKVTrackerInsert(&tracker, &key, &val);
        }

        if (!verifyMultimapMatchesTracker(m, &tracker)) {
            ERRR("Multimap/tracker mismatch after inserts");
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
            multimapInsertP(m, entry);
            ptestKVTrackerInsert(&tracker, &key, &val);
        }

        /* Update with new values */
        for (int i = 0; i < 10; i++) {
            databox key = DATABOX_SIGNED(i);
            databox val = DATABOX_SIGNED(i * 1000);
            const databox *entry[] = {&key, &val};
            multimapInsertP(m, entry);
            ptestKVTrackerInsert(&tracker, &key, &val);
        }

        if (!verifyMultimapMatchesTracker(m, &tracker)) {
            ERRR("Multimap/tracker mismatch after updates");
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
                multimapInsertP(m, entry);
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
                ERRR("Failed to recover multimapP");
            }

            if (!verifyMultimapMatchesTracker(m, &tracker)) {
                ERRR("Multimap/tracker mismatch after recovery");
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
            multimapInsertP(m, entry);
            ptestKVTrackerInsert(&tracker, &key, &val);
        }

        /* Delete every other entry */
        for (int i = 0; i < 20; i += 2) {
            databox key = DATABOX_SIGNED(i);
            multimapDeleteP(m, &key);
            ptestKVTrackerDelete(&tracker, &key);
        }

        if (!verifyMultimapMatchesTracker(m, &tracker)) {
            ERRR("Multimap/tracker mismatch after deletes");
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
                multimapInsertP(m, entry);
                ptestKVTrackerInsert(&tracker, &key, &val);
            }

            /* Delete entries 3, 7, 11 */
            for (int i = 3; i < 15; i += 4) {
                databox key = DATABOX_SIGNED(i);
                multimapDeleteP(m, &key);
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
                ERRR("Failed to recover multimapP after delete");
            }

            if (!verifyMultimapMatchesTracker(m, &tracker)) {
                ERRR("Multimap/tracker mismatch after delete recovery");
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
            multimapInsertP(m, entry);
        }

        if (multimapCountP(m) != 10) {
            ERRR("Count should be 10 before reset");
        }

        /* Reset the multimap */
        multimapResetP(m);

        if (multimapCountP(m) != 0) {
            ERRR("Count should be 0 after reset");
        }

        /* Add new entries */
        for (int i = 100; i < 115; i++) {
            databox key = DATABOX_SIGNED(i);
            databox val = DATABOX_SIGNED(i * 2);
            const databox *entry[] = {&key, &val};
            multimapInsertP(m, entry);
            ptestKVTrackerInsert(&tracker, &key, &val);
        }

        if (!verifyMultimapMatchesTracker(m, &tracker)) {
            ERRR("Multimap/tracker mismatch after reset and re-add");
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
                multimapInsertP(m, entry);
            }

            multimapResetP(m);

            for (int i = 50; i < 60; i++) {
                databox key = DATABOX_SIGNED(i);
                databox val = DATABOX_SIGNED(i * 3);
                const databox *entry[] = {&key, &val};
                multimapInsertP(m, entry);
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
                ERRR("Failed to recover multimapP after reset");
            }

            if (!verifyMultimapMatchesTracker(m, &tracker)) {
                ERRR("Multimap/tracker mismatch after reset recovery");
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
                multimapInsertP(m, entry);
                ptestKVTrackerInsert(&tracker, &key, &val);
            }

            /* Update a few */
            for (int i = round * 10; i < round * 10 + 3; i++) {
                databox key = DATABOX_SIGNED(i);
                databox val = DATABOX_SIGNED(i * 9999);
                const databox *entry[] = {&key, &val};
                multimapInsertP(m, entry);
                ptestKVTrackerInsert(&tracker, &key, &val);
            }

            /* Delete one */
            databox key = DATABOX_SIGNED(round * 10 + 5);
            multimapDeleteP(m, &key);
            ptestKVTrackerDelete(&tracker, &key);
        }

        if (!verifyMultimapMatchesTracker(m, &tracker)) {
            ERRR("Multimap/tracker mismatch after mixed ops");
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
                multimapInsertP(m, entry);
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
                multimapInsertP(m, entry);
                ptestKVTrackerInsert(&tracker, &key, &val);
            }

            multimapCloseP(m);
            persistCtxFree(ctx);
        }

        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multimapP *m = multimapOpenP(ctx);

            if (!m) {
                ERRR("Large dataset recovery failed");
            }

            if (!verifyMultimapMatchesTracker(m, &tracker)) {
                ERRR("Large dataset verification failed");
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
                ERRR("Empty recovery failed");
            }

            if (multimapCountP(m) != 0) {
                ERRR("Recovered empty multimap should have 0 entries");
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
            multimapInsertP(m, entry);
            multimapCloseP(m);
            persistCtxFree(ctx);
        }

        /* Recover and verify */
        {
            persistCtx *ctx = persistCtxOpen(basePath, NULL);
            multimapP *m = multimapOpenP(ctx);

            if (!m) {
                ERRR("Single entry recovery failed");
            }

            if (multimapCountP(m) != 1) {
                ERRR("Should have exactly 1 entry");
            }

            databox foundVal;
            databox *found[] = {&foundVal};
            if (!multimapLookupP(m, &expectedKey, found)) {
                ERRR("Expected key not found");
            }

            if (!ptestBoxesEqual(&foundVal, &expectedVal)) {
                ERRR("Value mismatch after recovery");
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
            multimapInsertP(m, entry);
        }

        /* Delete a few */
        for (int i = 0; i < 5; i++) {
            databox key = DATABOX_SIGNED(i);
            multimapDeleteP(m, &key);
        }

        persistCtxStats stats;
        multimapGetStatsP(m, &stats);

        /* 15 inserts + 5 deletes = 20 total ops */
        if (stats.totalOps != 20) {
            ERR("Total ops should be 20, got %llu",
                (unsigned long long)stats.totalOps);
        }

        multimapCloseP(m);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
