/* multiOrderedSetP.c - Persistent multiOrderedSet wrapper
 *
 * See multiOrderedSetP.h for API documentation.
 */

#include "multiOrderedSetP.h"
#include "../cleaner.h"
#include "../databox.h"
#include "../datakit.h"
#include "../persist.h"

#include <string.h>
#include <unistd.h>

defcleanerName(multiOrderedSetP *, zfree, MultiOrderedSetP);

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

multiOrderedSetP *multiOrderedSetNewP(persistCtx *ctx) {
    if (!ctx) {
        return NULL;
    }

    cleanerName(zfree, MultiOrderedSetP) multiOrderedSetP *mos =
        zcalloc(1, sizeof(*mos));
    mos->ctx = ctx;

    /* Create underlying multiOrderedSet (starts as Small tier) */
    /* @own: container; free=multiOrderedSetFree */
    mos->mos = multiOrderedSetNew();

    /* Initialize persistence context for multiOrderedSet type, and save
     * the initial snapshot (empty set) */
    if (!persistCtxInitForType(ctx, &persistOpsMultiOrderedSet) ||
        !persistCtxSaveSnapshot(ctx, mos->mos, &persistOpsMultiOrderedSet)) {
        multiOrderedSetFree(mos->mos);
        return NULL;
    }

    persistCtxClaim(ctx);
    multiOrderedSetP *out = mos;
    mos = NULL;
    return out;
}

multiOrderedSetP *multiOrderedSetOpenP(persistCtx *ctx) {
    if (!ctx) {
        return NULL;
    }

    /* Initialize persistence context for multiOrderedSet type */
    if (!persistCtxInitForType(ctx, &persistOpsMultiOrderedSet)) {
        return NULL;
    }

    /* Recover from snapshot + WAL */
    /* An empty set restores as an empty set: NULL means no snapshot, or one
     * that cannot be read, which must not open as an empty set (its next
     * compaction would replace the files) */
    multiOrderedSet *recovered =
        persistCtxRecover(ctx, &persistOpsMultiOrderedSet);
    if (!recovered) {
        return NULL;
    }

    /* @own: caller; free=multiOrderedSetCloseP */
    multiOrderedSetP *mos = zcalloc(1, sizeof(*mos));
    mos->ctx = ctx;
    mos->mos = recovered;

    persistCtxClaim(ctx);
    return mos;
}

void multiOrderedSetCloseP(multiOrderedSetP *mos) {
    if (!mos) {
        return;
    }

    /* Sync before closing; the context is free for another wrapper */
    if (mos->ctx) {
        persistCtxSync(mos->ctx);
        persistCtxRelease(mos->ctx);
    }

    /* Free underlying multiOrderedSet */
    if (mos->mos) {
        multiOrderedSetFree(mos->mos);
    }

    zfree(mos);
}

multiOrderedSet *multiOrderedSetGetRawP(multiOrderedSetP *mos) {
    return mos ? mos->mos : NULL;
}

/* ============================================================================
 * Metadata
 * ============================================================================
 */

size_t multiOrderedSetCountP(const multiOrderedSetP *mos) {
    return mos && mos->mos ? multiOrderedSetCount(mos->mos) : 0;
}

size_t multiOrderedSetBytesP(const multiOrderedSetP *mos) {
    return mos && mos->mos ? multiOrderedSetBytes(mos->mos) : 0;
}

/* ============================================================================
 * Mutations
 * ============================================================================
 */

persistLogResult multiOrderedSetAddP(multiOrderedSetP *mos,
                                     const databox *score,
                                     const databox *member, bool *updated) {
    if (updated) {
        *updated = false;
    }
    if (!mos || !mos->ctx || !score || !member) {
        return PERSIST_LOG_REFUSED;
    }

    const databox *args[2] = {score, member};
    const persistLogResult logged =
        persistCtxLogOp(mos->ctx, PERSIST_OP_INSERT, args, 2);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }
    const bool existed = multiOrderedSetAdd(&mos->mos, score, member);
    if (updated) {
        *updated = existed;
    }
    persistCtxMaybeCompact(mos->ctx, mos->mos, &persistOpsMultiOrderedSet);
    return logged;
}

/* A removal's log, written when the set asks (the member was found); a
 * refused log declines the removal.  'asked' tells a decline from an
 * absent member. */
typedef struct mospRemove_ {
    persistCtx *ctx;
    const databox *args[1];
    persistLogResult logged;
    bool asked;
} mospRemove_;

static bool mospLogRemove_(void *ctx) {
    mospRemove_ *r = ctx;
    r->asked = true;
    r->logged = persistCtxLogOp(r->ctx, PERSIST_OP_DELETE, r->args, 1);
    return r->logged != PERSIST_LOG_REFUSED;
}

persistLogResult multiOrderedSetRemoveP(multiOrderedSetP *mos,
                                        const databox *member) {
    if (!mos || !mos->ctx || !member) {
        return PERSIST_LOG_REFUSED;
    }
    if (!mos->mos) {
        return PERSIST_LOG_UNCHANGED;
    }

    /* Logged from inside the removal's one search, once the member is
     * found; a refused log leaves the set as it was */
    mospRemove_ r = {.ctx = mos->ctx,
                     .args = {member},
                     .logged = PERSIST_LOG_REFUSED,
                     .asked = false};
    if (!multiOrderedSetRemoveIf(&mos->mos, member, mospLogRemove_, &r)) {
        return r.asked ? PERSIST_LOG_REFUSED : PERSIST_LOG_UNCHANGED;
    }
    persistCtxMaybeCompact(mos->ctx, mos->mos, &persistOpsMultiOrderedSet);
    return r.logged;
}

persistLogResult multiOrderedSetResetP(multiOrderedSetP *mos) {
    if (!mos || !mos->ctx) {
        return PERSIST_LOG_REFUSED;
    }

    const persistLogResult logged =
        persistCtxLogOp(mos->ctx, PERSIST_OP_CLEAR, NULL, 0);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }
    if (mos->mos) {
        multiOrderedSetReset(&mos->mos);
    }
    persistCtxMaybeCompact(mos->ctx, mos->mos, &persistOpsMultiOrderedSet);
    return logged;
}

/* ============================================================================
 * Lookups
 * ============================================================================
 */

bool multiOrderedSetExistsP(const multiOrderedSetP *mos,
                            const databox *member) {
    if (!mos || !mos->mos || !member) {
        return false;
    }
    return multiOrderedSetExists(mos->mos, member);
}

bool multiOrderedSetGetScoreP(const multiOrderedSetP *mos,
                              const databox *member, databox *score) {
    if (!mos || !mos->mos || !member || !score) {
        return false;
    }
    return multiOrderedSetGetScore(mos->mos, member, score);
}

bool multiOrderedSetGetByRankP(const multiOrderedSetP *mos, int64_t rank,
                               databox *member, databox *score) {
    if (!mos || !mos->mos || !member || !score) {
        return false;
    }
    return multiOrderedSetGetByRank(mos->mos, rank, member, score);
}

bool multiOrderedSetFirstP(const multiOrderedSetP *mos, databox *member,
                           databox *score) {
    if (!mos || !mos->mos || !member || !score) {
        return false;
    }
    return multiOrderedSetFirst(mos->mos, member, score);
}

bool multiOrderedSetLastP(const multiOrderedSetP *mos, databox *member,
                          databox *score) {
    if (!mos || !mos->mos || !member || !score) {
        return false;
    }
    return multiOrderedSetLast(mos->mos, member, score);
}

size_t multiOrderedSetCountByScoreP(const multiOrderedSetP *mos,
                                    const mosRangeSpec *range) {
    if (!mos || !mos->mos || !range) {
        return 0;
    }
    return multiOrderedSetCountByScore(mos->mos, range);
}

/* ============================================================================
 * Iteration
 * ============================================================================
 */

void multiOrderedSetIteratorInitP(const multiOrderedSetP *mos,
                                  mosIterator *iter, bool forward) {
    if (!mos || !mos->mos || !iter) {
        return;
    }
    multiOrderedSetIteratorInit(mos->mos, iter, forward);
}

bool multiOrderedSetIteratorInitAtScoreP(const multiOrderedSetP *mos,
                                         mosIterator *iter,
                                         const databox *score, bool forward) {
    if (!mos || !mos->mos || !iter || !score) {
        return false;
    }
    return multiOrderedSetIteratorInitAtScore(mos->mos, iter, score, forward);
}

bool multiOrderedSetIteratorNextP(mosIterator *iter, databox *member,
                                  databox *score) {
    return multiOrderedSetIteratorNext(iter, member, score);
}

void multiOrderedSetIteratorReleaseP(mosIterator *iter) {
    multiOrderedSetIteratorRelease(iter);
}

/* ============================================================================
 * Persistence Control
 * ============================================================================
 */

bool multiOrderedSetSyncP(multiOrderedSetP *mos) {
    if (!mos || !mos->ctx) {
        return false;
    }
    return persistCtxSync(mos->ctx);
}

bool multiOrderedSetCompactP(multiOrderedSetP *mos) {
    if (!mos || !mos->ctx) {
        return false;
    }
    return persistCtxCompact(mos->ctx, mos->mos, &persistOpsMultiOrderedSet);
}

void multiOrderedSetGetStatsP(const multiOrderedSetP *mos,
                              persistCtxStats *stats) {
    if (!mos || !mos->ctx || !stats) {
        return;
    }
    persistCtxGetStats(mos->ctx, stats);
}

/* ============================================================================
 * Tests
 * ============================================================================
 */

#ifdef DATAKIT_TEST
#include "persistTestCommon.h"

static int test_multiOrderedSetP_basic(int, char *[]) {
    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multiOrderedSetPTest_basic", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multiOrderedSetP basic persistence")

    ptestCleanupFiles(basePath);

    /* Create new persistent multiOrderedSet */
    persistCtx *ctx = persistCtxNew(basePath, NULL);
    multiOrderedSetP *mos = multiOrderedSetNewP(ctx);
    if (!mos) {
        ERR("Failed to create multiOrderedSetP");
    }

    /* Add some entries ('updated': the member was already present) */
    bool updated = true;
    databox score1 = databoxNewSigned(100);
    databox member1 = databoxNewBytesAllowEmbed("alice", 5);
    if (!multiOrderedSetAddP(mos, &score1, &member1, &updated) || updated) {
        ERR("alice was not added as a new member");
    }

    databox score2 = databoxNewSigned(200);
    databox member2 = databoxNewBytesAllowEmbed("bob", 3);
    PTEST_LOGGED(multiOrderedSetAddP(mos, &score2, &member2, NULL));

    databox score3 = databoxNewSigned(50);
    databox member3 = databoxNewBytesAllowEmbed("charlie", 7);
    PTEST_LOGGED(multiOrderedSetAddP(mos, &score3, &member3, NULL));
    if (!multiOrderedSetAddP(mos, &score3, &member3, &updated) || !updated) {
        ERR("re-adding charlie was not an update");
    }

    /* Verify entries */
    if (!multiOrderedSetExistsP(mos, &member1)) {
        ERR("alice should exist");
    }
    if (!multiOrderedSetExistsP(mos, &member2)) {
        ERR("bob should exist");
    }
    if (!multiOrderedSetExistsP(mos, &member3)) {
        ERR("charlie should exist");
    }

    /* Check count */
    if (multiOrderedSetCountP(mos) != 3) {
        ERR("Count should be 3, got %zu", multiOrderedSetCountP(mos));
    }

    /* Verify scores */
    databox retrievedScore;
    if (!multiOrderedSetGetScoreP(mos, &member1, &retrievedScore)) {
        ERR("Failed to get alice's score");
    }
    if (retrievedScore.data.i != 100) {
        ERR("alice's score should be 100, got %" PRIdMAX,
            retrievedScore.data.i);
    }

    /* Close and reopen */
    multiOrderedSetCloseP(mos);
    persistCtxFree(ctx);

    ctx = persistCtxNew(basePath, NULL);
    mos = multiOrderedSetOpenP(ctx);
    if (!mos) {
        ERR("Failed to reopen multiOrderedSetP");
    }

    /* Verify entries after recovery */
    if (multiOrderedSetCountP(mos) != 3) {
        ERR("Count should be 3 after recovery, got %zu",
            multiOrderedSetCountP(mos));
    }

    if (!multiOrderedSetExistsP(mos, &member1)) {
        ERR("alice should exist after recovery");
    }
    if (!multiOrderedSetGetScoreP(mos, &member1, &retrievedScore)) {
        ERR("Failed to get alice's score after recovery");
    }
    if (retrievedScore.data.i != 100) {
        ERR("alice's score should be 100 after recovery, got %" PRIdMAX,
            retrievedScore.data.i);
    }

    /* Remove an entry */
    if (!multiOrderedSetRemoveP(mos, &member2)) {
        ERR("Failed to remove bob");
    }
    if (multiOrderedSetRemoveP(mos, &member2) != PERSIST_LOG_UNCHANGED) {
        ERR("removing absent bob was logged");
    }
    if (multiOrderedSetExistsP(mos, &member2)) {
        ERR("bob should be removed");
    }
    if (multiOrderedSetCountP(mos) != 2) {
        ERR("Count should be 2, got %zu", multiOrderedSetCountP(mos));
    }

    /* Close and reopen again */
    multiOrderedSetCloseP(mos);
    persistCtxFree(ctx);

    ctx = persistCtxNew(basePath, NULL);
    mos = multiOrderedSetOpenP(ctx);
    if (!mos) {
        ERR("Failed to reopen multiOrderedSetP");
    }

    /* Verify after second recovery */
    if (multiOrderedSetCountP(mos) != 2) {
        ERR("Count should be 2 after second recovery, got %zu",
            multiOrderedSetCountP(mos));
    }
    if (multiOrderedSetExistsP(mos, &member2)) {
        ERR("bob should still be removed after recovery");
    }
    if (!multiOrderedSetExistsP(mos, &member1)) {
        ERR("alice should exist after second recovery");
    }

    /* Cleanup */
    multiOrderedSetCloseP(mos);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

static int test_multiOrderedSetP_iteration(int, char *[]) {
    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multiOrderedSetPTest_iteration", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multiOrderedSetP iteration order")

    ptestCleanupFiles(basePath);

    persistCtx *ctx = persistCtxNew(basePath, NULL);
    multiOrderedSetP *mos = multiOrderedSetNewP(ctx);

    /* Add entries in non-sorted order */
    int64_t scores[] = {300, 100, 200, 50, 150};
    const char *members[] = {"e", "b", "d", "a", "c"};

    for (size_t i = 0; i < 5; i++) {
        databox score = databoxNewSigned(scores[i]);
        databox member =
            databoxNewBytesAllowEmbed(members[i], strlen(members[i]));
        PTEST_LOGGED(multiOrderedSetAddP(mos, &score, &member, NULL));
    }

    /* Close and reopen */
    multiOrderedSetCloseP(mos);
    persistCtxFree(ctx);

    ctx = persistCtxNew(basePath, NULL);
    mos = multiOrderedSetOpenP(ctx);

    /* Iterate forward - should be in score order: 50, 100, 150, 200, 300 */
    mosIterator iter;
    multiOrderedSetIteratorInitP(mos, &iter, true);

    const char *expectedOrder[] = {"a", "b", "c", "d", "e"};
    int64_t expectedScores[] = {50, 100, 150, 200, 300};
    size_t idx = 0;

    databox iterMember, iterScore;
    while (multiOrderedSetIteratorNextP(&iter, &iterMember, &iterScore)) {
        if (idx >= 5) {
            ERR("Too many entries in iteration");
        }

        int64_t actualScore = iterScore.data.i;
        if (actualScore != expectedScores[idx]) {
            ERR("Score at position %zu should be %" PRId64 ", got %" PRId64,
                idx, expectedScores[idx], actualScore);
        }

        const uint8_t *actualMemberBytes = iterMember.data.bytes.start;
        const char *actualMember = (const char *)actualMemberBytes;
        size_t actualLen = iterMember.len;
        if (strncmp(actualMember, expectedOrder[idx], actualLen) != 0) {
            ERR("Member at position %zu should be %s", idx, expectedOrder[idx]);
        }

        idx++;
    }

    multiOrderedSetIteratorReleaseP(&iter);

    if (idx != 5) {
        ERR("Should have iterated 5 entries, got %zu", idx);
    }

    /* Cleanup */
    multiOrderedSetCloseP(mos);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

static int test_multiOrderedSetP_compaction(int, char *[]) {
    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multiOrderedSetPTest_compaction", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multiOrderedSetP compaction")

    ptestCleanupFiles(basePath);

    persistCtx *ctx = persistCtxNew(basePath, NULL);
    if (!ctx) {
        ERR("Failed to create persistCtx");
    }

    multiOrderedSetP *mos = multiOrderedSetNewP(ctx);
    if (!mos) {
        ERR("Failed to create multiOrderedSetP");
    }

    /* Add many entries */
    char memberBuf[32];
    for (int64_t i = 0; i < 50; i++) {
        databox score = databoxNewSigned(i * 10);
        snprintf(memberBuf, sizeof(memberBuf), "member_%02" PRId64, i);
        databox member =
            databoxNewBytesAllowEmbed(memberBuf, strlen(memberBuf));
        PTEST_LOGGED(multiOrderedSetAddP(mos, &score, &member, NULL));
    }

    /* Attempt compaction */
    bool compacted = multiOrderedSetCompactP(mos);
    if (!compacted) {
        ERR("Compaction failed");
    }

    /* Verify data is intact */
    if (multiOrderedSetCountP(mos) != 50) {
        ERR("Count should be 50, got %zu", multiOrderedSetCountP(mos));
    }

    /* Verify all entries still accessible */
    for (int64_t i = 0; i < 50; i++) {
        snprintf(memberBuf, sizeof(memberBuf), "member_%02" PRId64, i);
        databox member =
            databoxNewBytesAllowEmbed(memberBuf, strlen(memberBuf));
        if (!multiOrderedSetExistsP(mos, &member)) {
            ERR("member_%02" PRId64 " should still exist", i);
        }
    }

    /* Cleanup */
    multiOrderedSetCloseP(mos);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

static int test_multiOrderedSetP_resetReuse(int, char *[]) {
    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multiOrderedSetPTest_resetReuse", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multiOrderedSetP reset then reuse survives recovery") {
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multiOrderedSetP *mos = multiOrderedSetNewP(ctx);

        char buf[32];
        for (int i = 0; i < 30; i++) {
            snprintf(buf, sizeof(buf), "pre-%d", i);
            const databox score = databoxNewUnsigned((uint64_t)i);
            const databox member = databoxNewBytesString(buf);
            PTEST_LOGGED(multiOrderedSetAddP(mos, &score, &member, NULL));
        }

        PTEST_LOGGED(multiOrderedSetResetP(mos));
        if (multiOrderedSetCountP(mos) != 0) {
            ERR("Expected empty after reset");
        }

        /* Refill with DIFFERENT members so pre-reset ghosts are
         * detectable after recovery */
        for (int i = 0; i < 20; i++) {
            snprintf(buf, sizeof(buf), "post-%d", i);
            const databox score = databoxNewUnsigned((uint64_t)(i + 100));
            const databox member = databoxNewBytesString(buf);
            PTEST_LOGGED(multiOrderedSetAddP(mos, &score, &member, NULL));
        }

        multiOrderedSetCloseP(mos);
        persistCtxFree(ctx);

        ctx = persistCtxNew(basePath, NULL);
        mos = multiOrderedSetOpenP(ctx);
        if (!mos) {
            ERR("Recovery after reset failed");
        } else {
            if (multiOrderedSetCountP(mos) != 20) {
                ERR("Expected 20 post-recovery, got %zu",
                    multiOrderedSetCountP(mos));
            }

            const databox ghost = databoxNewBytesString("pre-3");
            if (multiOrderedSetExistsP(mos, &ghost)) {
                ERR("Pre-reset ghost member survived recovery");
            }

            const databox kept = databoxNewBytesString("post-7");
            if (!multiOrderedSetExistsP(mos, &kept)) {
                ERR("Post-reset member lost in recovery");
            }

            multiOrderedSetCloseP(mos);
        }
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST_FINAL_RESULT;
}

static int test_multiOrderedSetP_readers(int, char *[]) {
    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multiOrderedSetPTest_readers", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multiOrderedSetP readers agree with ten scored members") {
        /* m0..m9 scored 0, 10, ..., 90 */
        ptestCleanupFiles(basePath);
        persistCtx *ctx = persistCtxNew(basePath, NULL);
        multiOrderedSetP *mos = multiOrderedSetNewP(ctx);
        char name[8];
        for (int i = 0; i < 10; i++) {
            snprintf(name, sizeof(name), "m%d", i);
            const databox score = DATABOX_SIGNED(i * 10);
            const databox member = databoxNewBytesString(name);
            PTEST_LOGGED(multiOrderedSetAddP(mos, &score, &member, NULL));
        }

        databox member = {};
        databox score = {};
        const databox m0 = databoxNewBytesString("m0");
        const databox m3 = databoxNewBytesString("m3");
        const databox m5 = databoxNewBytesString("m5");
        const databox m9 = databoxNewBytesString("m9");
        if (!multiOrderedSetFirstP(mos, &member, &score) ||
            !databoxEqual(&member, &m0)) {
            ERR("FirstP is not m0");
        }
        if (!multiOrderedSetLastP(mos, &member, &score) ||
            !databoxEqual(&member, &m9)) {
            ERR("LastP is not m9");
        }
        if (!multiOrderedSetGetByRankP(mos, 3, &member, &score) ||
            !databoxEqual(&member, &m3)) {
            ERR("GetByRankP(3) is not m3");
        }
        const mosRangeSpec range = {.min = DATABOX_SIGNED(20),
                                    .max = DATABOX_SIGNED(50)};
        if (multiOrderedSetCountByScoreP(mos, &range) != 4) {
            ERR("CountByScoreP [20, 50] = %zu, want 4",
                multiOrderedSetCountByScoreP(mos, &range));
        }
        mosIterator iter;
        const databox at = DATABOX_SIGNED(45);
        if (!multiOrderedSetIteratorInitAtScoreP(mos, &iter, &at, true) ||
            !multiOrderedSetIteratorNextP(&iter, &member, &score) ||
            !databoxEqual(&member, &m5)) {
            ERR("IteratorInitAtScoreP(45) does not start at m5");
        }
        multiOrderedSetIteratorReleaseP(&iter);

        persistCtxStats stats;
        multiOrderedSetGetStatsP(mos, &stats);
        if (!multiOrderedSetSyncP(mos) || multiOrderedSetBytesP(mos) == 0 ||
            multiOrderedSetCount(multiOrderedSetGetRawP(mos)) != 10 ||
            stats.totalOps < 10) {
            ERR("SyncP/BytesP/GetRawP/GetStatsP disagree");
        }

        multiOrderedSetCloseP(mos);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST_FINAL_RESULT;
}

/* The whole file at 'path' into 'buf' (up to 'cap'); its length, or 0 */
static size_t test_mosPReadFile(const char *path, uint8_t *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    const size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

static int test_multiOrderedSetP_openRefusals(int, char *[]) {
    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multiOrderedSetPTest_open", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }
    char snapPath[PATH_MAX + sizeof(".snap")];
    snprintf(snapPath, sizeof(snapPath), "%s.snap", basePath);

    TEST("multiOrderedSetP opens only what it saved, and heals a damaged "
         "WAL entry")

    /* An empty set reopens as an empty set */
    persistCtx *ctx = persistCtxNew(basePath, NULL);
    multiOrderedSetP *mos = multiOrderedSetNewP(ctx);
    multiOrderedSetCloseP(mos);
    persistCtxFree(ctx);
    ctx = persistCtxNew(basePath, NULL);
    mos = multiOrderedSetOpenP(ctx);
    if (!mos || multiOrderedSetCountP(mos) != 0) {
        ERR("an empty set did not reopen empty");
    }
    multiOrderedSetCloseP(mos);
    persistCtxFree(ctx);

    /* A damaged snapshot does not open, and stays as it was */
    ctx = persistCtxNew(basePath, NULL);
    mos = multiOrderedSetNewP(ctx);
    for (int64_t i = 0; i < 20; i++) {
        const databox score = databoxNewSigned(i);
        const databox member = databoxNewSigned(i * 7);
        PTEST_LOGGED(multiOrderedSetAddP(mos, &score, &member, NULL));
    }
    multiOrderedSetCompactP(mos);
    multiOrderedSetCloseP(mos);
    persistCtxFree(ctx);
    static uint8_t before[1 << 16];
    static uint8_t after[1 << 16];
    const size_t len = test_mosPReadFile(snapPath, before, sizeof(before));
    FILE *f = fopen(snapPath, "r+b");
    if (!f || len < 16 || fseek(f, (long)(len / 2), SEEK_SET) != 0 ||
        fputc(before[len / 2] ^ 0x5a, f) == EOF) {
        ERR("could not damage the snapshot");
    }
    if (f) {
        fclose(f);
    }
    before[len / 2] ^= 0x5a;
    ctx = persistCtxNew(basePath, NULL);
    mos = multiOrderedSetOpenP(ctx);
    if (mos) {
        ERR("a damaged snapshot opened");
        multiOrderedSetCloseP(mos);
    }
    persistCtxFree(ctx);
    if (test_mosPReadFile(snapPath, after, sizeof(after)) != len ||
        memcmp(before, after, len) != 0) {
        ERR("a refused open changed the snapshot");
    }

    /* No snapshot at all does not open (it used to open as empty) */
    ptestCleanupFiles(basePath);
    ctx = persistCtxNew(basePath, NULL);
    mos = multiOrderedSetOpenP(ctx);
    if (mos) {
        ERR("a set opened with no snapshot");
        multiOrderedSetCloseP(mos);
    }
    persistCtxFree(ctx);

    /* Members 10, 20, 30 added with the last entry damaged: lenient
     * recovery keeps {10, 20} and heals the WAL (the damaged entry moves
     * to the quarantine); an add of 40 is then logged with no compaction,
     * and a reopen holds {10, 20, 40} */
    ptestCleanupFiles(basePath);
    ctx = persistCtxNew(basePath, NULL);
    mos = multiOrderedSetNewP(ctx);
    for (int64_t m = 10; m <= 30; m += 10) {
        const databox score = databoxNewSigned(m / 10);
        const databox member = databoxNewSigned(m);
        PTEST_LOGGED(multiOrderedSetAddP(mos, &score, &member, NULL));
    }
    multiOrderedSetCloseP(mos);
    persistCtxFree(ctx);
    if (!ptestDamageLastWalEntry(basePath)) {
        ERR("could not damage the WAL");
    }
    const databox m10 = databoxNewSigned(10);
    const databox m30 = databoxNewSigned(30);
    const databox m40 = databoxNewSigned(40);
    const databox s4 = databoxNewSigned(4);
    ctx = persistCtxOpen(basePath, NULL);
    mos = multiOrderedSetOpenP(ctx);
    if (!mos || multiOrderedSetCountP(mos) != 2 ||
        multiOrderedSetExistsP(mos, &m30)) {
        ERR("lenient recovery kept %zu members, want 10, 20",
            mos ? multiOrderedSetCountP(mos) : (size_t)0);
    }
    if (!ptestExpectHealed(ctx, basePath)) {
        ERR("the damaged entry was not healed into the quarantine");
    }
    if (mos &&
        multiOrderedSetAddP(mos, &s4, &m40, NULL) == PERSIST_LOG_REFUSED) {
        ERR("a change after the healed entry was refused");
    }
    multiOrderedSetCloseP(mos);
    persistCtxFree(ctx);
    ctx = persistCtxOpen(basePath, NULL);
    mos = multiOrderedSetOpenP(ctx);
    if (!mos || multiOrderedSetCountP(mos) != 3 ||
        !multiOrderedSetExistsP(mos, &m10) ||
        !multiOrderedSetExistsP(mos, &m40)) {
        ERR("reopen does not hold 10, 20, 40");
    }
    multiOrderedSetCloseP(mos);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    /* A context bound to another type takes no set */
    ctx = persistCtxNew(basePath, NULL);
    if (!persistCtxInitForType(ctx, &persistOpsIntset) ||
        multiOrderedSetNewP(ctx) || multiOrderedSetOpenP(ctx)) {
        ERR("a set started on a context bound to an intset");
    }
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

/* A refused log changes nothing: in a small set and in one large enough
 * for its member index (the Full tier), a remove under a refusing WAL is
 * REFUSED with the member kept, an absent member's is UNCHANGED, logging
 * works again, and a reopen holds only the logged removal */
static int test_multiOrderedSetP_refusedLog(int, char *[]) {
    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multiOrderedSetPTest_refusedLog", basePath,
                          sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("multiOrderedSetP a refused log changes nothing (remove)") {
        static constexpr int sizes[] = {20, 1200};
        for (size_t s = 0; s < COUNT_ARRAY(sizes); s++) {
            ptestCleanupFiles(basePath);
            persistCtx *ctx = persistCtxNew(basePath, NULL);
            multiOrderedSetP *mos = multiOrderedSetNewP(ctx);
            char buf[32];
            for (int i = 0; i < sizes[s]; i++) {
                snprintf(buf, sizeof(buf), "m%05d", i);
                const databox score = databoxNewSigned(i);
                const databox member = databoxNewBytesString(buf);
                PTEST_LOGGED(multiOrderedSetAddP(mos, &score, &member, NULL));
            }
            const databox kept = databoxNewBytesString("m00005");
            const databox ghost = databoxNewBytesString("absent");

            const databox high = databoxNewSigned(99);
            bool updated = true;
            PTEST_EXPECT_REFUSED(
                ctx, multiOrderedSetRemoveP(mos, &kept),
                multiOrderedSetAddP(mos, &high, &kept, &updated),
                multiOrderedSetAddP(mos, &high, &ghost, &updated),
                multiOrderedSetResetP(mos));

            databox score = {};
            if (multiOrderedSetRemoveP(mos, &ghost) != PERSIST_LOG_UNCHANGED ||
                multiOrderedSetCountP(mos) != (size_t)sizes[s] ||
                !multiOrderedSetGetScoreP(mos, &kept, &score) ||
                score.data.i64 != 5 || multiOrderedSetExistsP(mos, &ghost) ||
                updated) {
                ERR("%d members: a refused change changed the set", sizes[s]);
            }

            /* Missing arguments: refused or empty answers */
            if (multiOrderedSetAddP(NULL, &high, &kept, NULL) ||
                multiOrderedSetAddP(mos, NULL, &kept, NULL) ||
                multiOrderedSetRemoveP(NULL, &kept) ||
                multiOrderedSetRemoveP(mos, NULL) ||
                multiOrderedSetResetP(NULL) || multiOrderedSetNewP(NULL) ||
                multiOrderedSetOpenP(NULL) || multiOrderedSetGetRawP(NULL) ||
                multiOrderedSetCountP(NULL) || multiOrderedSetBytesP(NULL) ||
                multiOrderedSetExistsP(NULL, &kept) ||
                multiOrderedSetGetScoreP(NULL, &kept, &score) ||
                multiOrderedSetSyncP(NULL) || multiOrderedSetCompactP(NULL)) {
                ERR("a call without its wrapper or member was not refused");
            }
            multiOrderedSetGetStatsP(NULL, NULL);
            multiOrderedSetCloseP(NULL);

            /* Logging works again (an update reports itself) */
            const databox six = databoxNewSigned(6);
            const databox next6 = databoxNewBytesString("m00006");
            if (multiOrderedSetRemoveP(mos, &kept) == PERSIST_LOG_REFUSED ||
                multiOrderedSetAddP(mos, &six, &next6, &updated) ==
                    PERSIST_LOG_REFUSED ||
                !updated) {
                ERR("the context did not log again after the refusal");
            }
            multiOrderedSetCloseP(mos);
            persistCtxFree(ctx);

            ctx = persistCtxOpen(basePath, NULL);
            mos = multiOrderedSetOpenP(ctx);
            const databox next = databoxNewBytesString("m00006");
            if (!mos || multiOrderedSetCountP(mos) != (size_t)sizes[s] - 1 ||
                multiOrderedSetExistsP(mos, &kept) ||
                !multiOrderedSetGetScoreP(mos, &next, &score) ||
                score.data.i64 != 6) {
                ERR("%d members: a reopen did not hold exactly the logged "
                    "removal",
                    sizes[s]);
            }
            multiOrderedSetCloseP(mos);
            persistCtxFree(ctx);
        }
        ptestCleanupFiles(basePath);
    }

    TEST_FINAL_RESULT;
}

int multiOrderedSetPTest(int argc, char *argv[]) {
    int err = 0;

    err += test_multiOrderedSetP_refusedLog(argc, argv);
    err += test_multiOrderedSetP_openRefusals(argc, argv);
    err += test_multiOrderedSetP_readers(argc, argv);
    err += test_multiOrderedSetP_basic(argc, argv);
    err += test_multiOrderedSetP_iteration(argc, argv);
    err += test_multiOrderedSetP_compaction(argc, argv);
    err += test_multiOrderedSetP_resetReuse(argc, argv);

    return err;
}

#endif /* DATAKIT_TEST */
