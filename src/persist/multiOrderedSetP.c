/* multiOrderedSetP.c - Persistent multiOrderedSet wrapper
 *
 * See multiOrderedSetP.h for API documentation.
 */

#include "multiOrderedSetP.h"
#include "../databox.h"
#include "../datakit.h"
#include "../persist.h"

#include <string.h>
#include <unistd.h>

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

multiOrderedSetP *multiOrderedSetNewP(persistCtx *ctx) {
    if (!ctx) {
        printf("DEBUG: multiOrderedSetNewP: ctx is NULL\n");
        return NULL;
    }

    multiOrderedSetP *mos = zcalloc(1, sizeof(*mos));
    mos->ctx = ctx;

    /* Create underlying multiOrderedSet (starts as Small tier) */
    mos->mos = multiOrderedSetNew();

    /* Initialize persistence context for multiOrderedSet type */
    if (!persistCtxInitForType(ctx, &persistOpsMultiOrderedSet)) {
        zfree(mos);
        return NULL;
    }

    /* Save initial snapshot (empty set) */
    if (!persistCtxSaveSnapshot(ctx, mos->mos, &persistOpsMultiOrderedSet)) {
        if (mos->mos) {
            multiOrderedSetFree(mos->mos);
        }
        zfree(mos);
        return NULL;
    }

    return mos;
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
    multiOrderedSet *recovered =
        persistCtxRecover(ctx, &persistOpsMultiOrderedSet);
    /* Note: recovered can be NULL for empty set, but we need non-NULL for
     * proper operation */
    if (!recovered) {
        recovered = multiOrderedSetNew();
    }

    multiOrderedSetP *mos = zcalloc(1, sizeof(*mos));
    mos->ctx = ctx;
    mos->mos = recovered;

    return mos;
}

void multiOrderedSetCloseP(multiOrderedSetP *mos) {
    if (!mos) {
        return;
    }

    /* Sync before closing */
    if (mos->ctx) {
        persistCtxSync(mos->ctx);
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

bool multiOrderedSetAddP(multiOrderedSetP *mos, const databox *score,
                         const databox *member) {
    if (!mos || !mos->ctx || !score || !member) {
        return false;
    }

    /* Log to WAL first */
    const databox *args[2] = {score, member};
    if (!persistCtxLogOp(mos->ctx, PERSIST_OP_INSERT, args, 2)) {
        return false;
    }

    /* Update in-memory */
    bool added = multiOrderedSetAdd(&mos->mos, score, member);

    /* Check compaction threshold */
    persistCtxMaybeCompact(mos->ctx, mos->mos, &persistOpsMultiOrderedSet);

    return added;
}

bool multiOrderedSetRemoveP(multiOrderedSetP *mos, const databox *member) {
    if (!mos || !mos->ctx || !member) {
        return false;
    }

    /* Empty set - nothing to remove */
    if (!mos->mos) {
        return false;
    }

    /* Log to WAL first */
    const databox *args[1] = {member};
    if (!persistCtxLogOp(mos->ctx, PERSIST_OP_DELETE, args, 1)) {
        return false;
    }

    /* Update in-memory */
    bool removed = multiOrderedSetRemove(&mos->mos, member);

    /* Check compaction threshold */
    persistCtxMaybeCompact(mos->ctx, mos->mos, &persistOpsMultiOrderedSet);

    return removed;
}

void multiOrderedSetResetP(multiOrderedSetP *mos) {
    if (!mos || !mos->ctx) {
        return;
    }

    /* Log to WAL first */
    if (!persistCtxLogOp(mos->ctx, PERSIST_OP_CLEAR, NULL, 0)) {
        return;
    }

    /* Update in-memory */
    if (mos->mos) {
        multiOrderedSetReset(mos->mos);
    }

    /* Check compaction threshold */
    persistCtxMaybeCompact(mos->ctx, mos->mos, &persistOpsMultiOrderedSet);
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

static int test_multiOrderedSetP_basic(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    const char *basePath = "/tmp/multiOrderedSetPTest_basic";

    TEST("multiOrderedSetP basic persistence")

    ptestCleanupFiles(basePath);

    /* Create new persistent multiOrderedSet */
    persistCtx *ctx = persistCtxNew(basePath, NULL);
    multiOrderedSetP *mos = multiOrderedSetNewP(ctx);
    if (!mos) {
        ERRR("Failed to create multiOrderedSetP");
    }

    /* Add some entries (return value false = new insert, true = replaced) */
    databox score1 = databoxNewSigned(100);
    databox member1 = databoxNewBytesAllowEmbed("alice", 5);
    multiOrderedSetAddP(mos, &score1, &member1);

    databox score2 = databoxNewSigned(200);
    databox member2 = databoxNewBytesAllowEmbed("bob", 3);
    multiOrderedSetAddP(mos, &score2, &member2);

    databox score3 = databoxNewSigned(50);
    databox member3 = databoxNewBytesAllowEmbed("charlie", 7);
    multiOrderedSetAddP(mos, &score3, &member3);

    /* Verify entries */
    if (!multiOrderedSetExistsP(mos, &member1)) {
        ERRR("alice should exist");
    }
    if (!multiOrderedSetExistsP(mos, &member2)) {
        ERRR("bob should exist");
    }
    if (!multiOrderedSetExistsP(mos, &member3)) {
        ERRR("charlie should exist");
    }

    /* Check count */
    if (multiOrderedSetCountP(mos) != 3) {
        ERR("Count should be 3, got %zu", multiOrderedSetCountP(mos));
    }

    /* Verify scores */
    databox retrievedScore;
    if (!multiOrderedSetGetScoreP(mos, &member1, &retrievedScore)) {
        ERRR("Failed to get alice's score");
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
        ERRR("Failed to reopen multiOrderedSetP");
    }

    /* Verify entries after recovery */
    if (multiOrderedSetCountP(mos) != 3) {
        ERR("Count should be 3 after recovery, got %zu",
            multiOrderedSetCountP(mos));
    }

    if (!multiOrderedSetExistsP(mos, &member1)) {
        ERRR("alice should exist after recovery");
    }
    if (!multiOrderedSetGetScoreP(mos, &member1, &retrievedScore)) {
        ERRR("Failed to get alice's score after recovery");
    }
    if (retrievedScore.data.i != 100) {
        ERR("alice's score should be 100 after recovery, got %" PRIdMAX,
            retrievedScore.data.i);
    }

    /* Remove an entry */
    if (!multiOrderedSetRemoveP(mos, &member2)) {
        ERRR("Failed to remove bob");
    }
    if (multiOrderedSetExistsP(mos, &member2)) {
        ERRR("bob should be removed");
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
        ERRR("Failed to reopen multiOrderedSetP");
    }

    /* Verify after second recovery */
    if (multiOrderedSetCountP(mos) != 2) {
        ERR("Count should be 2 after second recovery, got %zu",
            multiOrderedSetCountP(mos));
    }
    if (multiOrderedSetExistsP(mos, &member2)) {
        ERRR("bob should still be removed after recovery");
    }
    if (!multiOrderedSetExistsP(mos, &member1)) {
        ERRR("alice should exist after second recovery");
    }

    /* Cleanup */
    multiOrderedSetCloseP(mos);
    persistCtxFree(ctx);
    ptestCleanupFiles(basePath);

    TEST_FINAL_RESULT;
}

static int test_multiOrderedSetP_iteration(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    const char *basePath = "/tmp/multiOrderedSetPTest_iteration";

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
        multiOrderedSetAddP(mos, &score, &member);
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
            ERRR("Too many entries in iteration");
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

static int test_multiOrderedSetP_compaction(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    const char *basePath = "/tmp/multiOrderedSetPTest_compaction";

    TEST("multiOrderedSetP compaction")

    ptestCleanupFiles(basePath);

    persistCtx *ctx = persistCtxNew(basePath, NULL);
    if (!ctx) {
        ERRR("Failed to create persistCtx");
    }

    multiOrderedSetP *mos = multiOrderedSetNewP(ctx);
    if (!mos) {
        ERRR("Failed to create multiOrderedSetP");
    }

    /* Add many entries */
    char memberBuf[32];
    for (int64_t i = 0; i < 50; i++) {
        databox score = databoxNewSigned(i * 10);
        snprintf(memberBuf, sizeof(memberBuf), "member_%02" PRId64, i);
        databox member =
            databoxNewBytesAllowEmbed(memberBuf, strlen(memberBuf));
        multiOrderedSetAddP(mos, &score, &member);
    }

    /* Attempt compaction */
    bool compacted = multiOrderedSetCompactP(mos);
    if (!compacted) {
        ERRR("Compaction failed");
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

int multiOrderedSetPTest(int argc, char *argv[]) {
    int err = 0;

    err += test_multiOrderedSetP_basic(argc, argv);
    err += test_multiOrderedSetP_iteration(argc, argv);
    err += test_multiOrderedSetP_compaction(argc, argv);

    return err;
}

#endif /* DATAKIT_TEST */
