/* persistCtx.c - Common infrastructure for persistent data structure wrappers
 *
 * See persistCtx.h for API documentation.
 */

#include "persistCtx.h"
#include "../datakit.h"

#include <errno.h>
#include <string.h>
#include <sys/time.h>

/* ============================================================================
 * Time Utilities
 * ============================================================================
 */

static uint64_t getTimeMicroseconds(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + (uint64_t)tv.tv_usec;
}

/* ============================================================================
 * Default Configuration
 * ============================================================================
 */

persistCtxConfig persistCtxDefaultConfig(void) {
    return (persistCtxConfig){
        .syncMode = PERSIST_CTX_SYNC_EVERYSEC,
        .compactAfterOps = 10000, /* Compact after 10k operations */
        .compactAfterBytes = 64 * 1024 * 1024, /* Or after 64MB WAL */
        .walBufferSize = 64 * 1024,            /* 64KB buffer */
        .strictRecovery = false,
    };
}

/* ============================================================================
 * Path Utilities
 * ============================================================================
 */

static char *makeSnapshotPath(const char *basePath) {
    size_t len = strlen(basePath) + 6; /* ".snap\0" */
    /* @own: caller; free=zfree */
    char *path = zmalloc(len);
    snprintf(path, len, "%s.snap", basePath);
    return path;
}

static char *makeWalPath(const char *basePath) {
    size_t len = strlen(basePath) + 5; /* ".wal\0" */
    /* @own: caller; free=zfree */
    char *path = zmalloc(len);
    snprintf(path, len, "%s.wal", basePath);
    return path;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/* Helper to duplicate a string using zmalloc */
static char *zstrdupInternal(const char *s) {
    size_t len = strlen(s) + 1;
    /* @own: caller; free=zfree */
    char *dup = zmalloc(len);
    memcpy(dup, s, len);
    return dup;
}

persistCtx *persistCtxNew(const char *basePath,
                          const persistCtxConfig *config) {
    if (!basePath) {
        return NULL;
    }

    /* @own: caller; free=persistCtxFree */
    persistCtx *ctx = zcalloc(1, sizeof(*ctx));

    /* Store paths */
    ctx->basePath = zstrdupInternal(basePath);
    ctx->snapshotPath = makeSnapshotPath(basePath);
    ctx->walPath = makeWalPath(basePath);

    /* Configuration */
    ctx->config = config ? *config : persistCtxDefaultConfig();

    /* Initialize timing */
    ctx->lastSyncTime = getTimeMicroseconds();

    /* Note: The actual persist context (ctx->p) is created when a structure
     * is attached via the wrapper's New or Open functions, since we need
     * to know the persistOps type. */

    return ctx;
}

persistCtx *persistCtxOpen(const char *basePath,
                           const persistCtxConfig *config) {
    /* For now, same as New - the wrapper's Open function handles recovery */
    return persistCtxNew(basePath, config);
}

void persistCtxFree(persistCtx *ctx) {
    if (!ctx) {
        return;
    }

    /* Sync before closing */
    if (ctx->p) {
        persistSync(ctx->p);
        persistClose(ctx->p);
    }

    /* Free paths */
    zfree(ctx->basePath);
    zfree(ctx->snapshotPath);
    zfree(ctx->walPath);

    zfree(ctx);
}

/* ============================================================================
 * Internal: Initialize persist context for a structure type
 * ============================================================================
 */

bool persistCtxInitForType(persistCtx *ctx, const persistOps *ops) {
    if (!ctx || !ops) {
        return false;
    }

    /* Already initialized? */
    if (ctx->p) {
        return true;
    }

    /* Create persist config from ctx config */
    persistConfig pconfig = persistDefaultConfig();
    pconfig.walBufferSize = ctx->config.walBufferSize;
    pconfig.strictRecovery = ctx->config.strictRecovery;

    switch (ctx->config.syncMode) {
    case PERSIST_CTX_SYNC_NONE:
        pconfig.syncMode = PERSIST_SYNC_NONE;
        break;
    case PERSIST_CTX_SYNC_EVERYSEC:
        pconfig.syncMode = PERSIST_SYNC_EVERYSEC;
        break;
    case PERSIST_CTX_SYNC_ALWAYS:
        pconfig.syncMode = PERSIST_SYNC_ALWAYS;
        break;
    }

    /* Create persist context */
    ctx->p = persistCreate(ops, &pconfig);
    if (!ctx->p) {
        return false;
    }

    /* Attach file stores (persistStore here is the storage backend from
     * persist.h) */
    persistStore *snapStore = persistStoreFile(ctx->snapshotPath, true);
    persistStore *walStore = persistStoreFile(ctx->walPath, true);

    if (!snapStore || !walStore) {
        if (snapStore) {
            snapStore->close(snapStore->ctx);
            zfree(snapStore);
        }
        if (walStore) {
            walStore->close(walStore->ctx);
            zfree(walStore);
        }
        persistClose(ctx->p);
        ctx->p = NULL;
        return false;
    }

    persistAttachSnapshot(ctx->p, snapStore);
    persistAttachWAL(ctx->p, walStore);

    return true;
}

/* ============================================================================
 * Operations
 * ============================================================================
 */

persistLogResult persistCtxLogOp(persistCtx *ctx, persistOp op,
                                 const void *args, size_t argc) {
    if (!ctx || !ctx->p) {
        return PERSIST_LOG_REFUSED;
    }

    /* Log to WAL; a refused op is not in it, so the caller applies
     * nothing */
    persistLogResult result = persistLogOp(ctx->p, op, args, argc);
    if (result == PERSIST_LOG_REFUSED) {
        return result;
    }

    /* Update counters */
    ctx->opsSinceCompact++;
    ctx->totalOps++;

    /* The persist carries this context's sync mode, so persistLogOp made
     * the sync the policy asks for.  The entry is logged either way: a
     * failed sync (NOT_DURABLE) never fails the op, or the caller would
     * leave memory behind the WAL; the next sync retries */
    if (result == PERSIST_LOG_DURABLE) {
        ctx->totalSyncs++;
        ctx->lastSyncTime = getTimeMicroseconds();
    } else if (result == PERSIST_LOG_NOT_DURABLE) {
        ctx->notDurableOps++;
    }
    return result;
}

bool persistCtxMaybeCompact(persistCtx *ctx, void *structure,
                            const persistOps *ops) {
    if (!ctx || !ctx->p || !structure) {
        return false;
    }

    bool shouldCompact = false;

    /* Check operation threshold */
    if (ctx->config.compactAfterOps > 0 &&
        ctx->opsSinceCompact >= ctx->config.compactAfterOps) {
        shouldCompact = true;
    }

    /* Check byte threshold */
    if (!shouldCompact && ctx->config.compactAfterBytes > 0) {
        persistStats stats;
        persistGetStats(ctx->p, &stats);
        if (stats.walBytes >= ctx->config.compactAfterBytes) {
            shouldCompact = true;
        }
    }

    if (shouldCompact) {
        return persistCtxCompact(ctx, structure, ops);
    }

    return true;
}

bool persistCtxSyncIfDue(persistCtx *ctx) {
    if (!ctx || !ctx->p) {
        return false;
    }

    if (ctx->config.syncMode != PERSIST_CTX_SYNC_EVERYSEC) {
        return true;
    }

    const uint64_t now = getTimeMicroseconds();
    if (now - ctx->lastSyncTime >= 1000000) {
        if (!persistSync(ctx->p)) {
            return false;
        }
        ctx->lastSyncTime = now;
        ctx->totalSyncs++;
    }

    return true;
}

bool persistCtxSync(persistCtx *ctx) {
    if (!ctx || !ctx->p) {
        return false;
    }

    bool result = persistSync(ctx->p);
    if (result) {
        ctx->lastSyncTime = getTimeMicroseconds();
        ctx->totalSyncs++;
    }
    return result;
}

bool persistCtxCompact(persistCtx *ctx, void *structure,
                       const persistOps *ops) {
    if (!ctx || !ctx->p || !structure) {
        return false;
    }

    (void)ops; /* Used by persistCompact internally via ctx->p */

    if (persistCompact(ctx->p, structure)) {
        ctx->opsSinceCompact = 0;
        ctx->totalCompactions++;
        return true;
    }

    return false;
}

/* ============================================================================
 * Recovery
 * ============================================================================
 */

void *persistCtxRecover(persistCtx *ctx, const persistOps *ops) {
    if (!ctx) {
        return NULL;
    }

    /* Initialize persist context if needed */
    if (!persistCtxInitForType(ctx, ops)) {
        return NULL;
    }

    /* Recover from snapshot + WAL */
    return persistRecover(ctx->p);
}

bool persistCtxExists(const char *basePath) {
    if (!basePath) {
        return false;
    }

    char *snapPath = makeSnapshotPath(basePath);
    bool exists = (access(snapPath, F_OK) == 0);
    zfree(snapPath);

    if (!exists) {
        /* An orphaned WAL (log present, snapshot missing) means data
         * exists but the Open paths will treat it as absent - surface
         * the condition instead of silently losing the log */
        char *walPath = makeWalPath(basePath);
        if (access(walPath, F_OK) == 0) {
            fprintf(stderr,
                    "persistCtx: WAL exists without snapshot at %s.wal - "
                    "recovery will ignore it\n",
                    basePath);
        }
        zfree(walPath);
    }

    return exists;
}

/* ============================================================================
 * Snapshot (for initial save)
 * ============================================================================
 */

bool persistCtxSaveSnapshot(persistCtx *ctx, void *structure,
                            const persistOps *ops) {
    if (!ctx || !structure) {
        return false;
    }

    /* Initialize persist context if needed */
    if (!persistCtxInitForType(ctx, ops)) {
        return false;
    }

    return persistSnapshot(ctx->p, structure);
}

/* ============================================================================
 * Statistics
 * ============================================================================
 */

void persistCtxGetStats(const persistCtx *ctx, persistCtxStats *stats) {
    if (!ctx || !stats) {
        return;
    }

    memset(stats, 0, sizeof(*stats));

    stats->totalOps = ctx->totalOps;
    stats->totalSyncs = ctx->totalSyncs;
    stats->notDurableOps = ctx->notDurableOps;
    stats->totalCompactions = ctx->totalCompactions;
    stats->opsSinceCompact = ctx->opsSinceCompact;

    if (ctx->p) {
        persistStats pstats;
        persistGetStats(ctx->p, &pstats);
        stats->syncFailures = pstats.syncFailures;
        stats->lastSyncError = pstats.lastSyncError;
        stats->snapshotBytes = pstats.snapshotBytes;
        stats->walBytes = pstats.walBytes;
        /* Compaction starts a fresh WAL, so the live WAL size IS the
         * bytes accumulated since the last compaction (the byte-based
         * compaction trigger uses the same measure) */
        stats->bytesSinceCompact = pstats.walBytes;
    }
}

/* ============================================================================
 * Tests
 * ============================================================================
 */

#ifdef DATAKIT_TEST
#include "../ctest.h"
#include "../dkTestTmp.h"
#include "flexP.h"
#include <unistd.h>

/* A store sync that fails as a full device would */
static bool persistCtxTestFailSync(void *ctx) {
    (void)ctx;
    errno = ENOSPC;
    return false;
}

int persistCtxTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {{0}};

    TEST("persistCtx default config") {
        persistCtxConfig config = persistCtxDefaultConfig();
        if (config.syncMode != PERSIST_CTX_SYNC_EVERYSEC) {
            ERRR("Default sync mode should be EVERYSEC");
        }
        if (config.compactAfterOps != 10000) {
            ERRR("Default compact after ops should be 10000");
        }
    }

    TEST("persistCtx create and free") {
        char path[PATH_MAX];
        if (!dkTestTmpDirPath(&tmp, "persistCtxTest", path, sizeof(path))) {
            ERR("could not create a scratch directory: %s", strerror(errno));
        } else {
            persistCtx *ctx = persistCtxNew(path, NULL);
            if (!ctx) {
                ERRR("Failed to create persistCtx");
            } else {
                if (strcmp(ctx->basePath, path) != 0) {
                    ERRR("Base path not set correctly");
                }
                persistCtxFree(ctx);
            }
        }
    }

    TEST("persistCtx exists check") {
        char path[PATH_MAX];
        char snapPath[PATH_MAX];
        if (!dkTestTmpDirPath(&tmp, "persistCtxExistsTest", path,
                              sizeof(path)) ||
            !dkTestTmpDirPath(&tmp, "persistCtxExistsTest.snap", snapPath,
                              sizeof(snapPath))) {
            ERR("could not create a scratch directory: %s", strerror(errno));
        } else {
            /* Should not exist initially */
            if (persistCtxExists(path)) {
                ERRR("Context should not exist yet");
            }

            /* Create files manually */
            FILE *f = fopen(snapPath, "w");
            if (f) {
                fclose(f);
            }

            /* Now should exist */
            if (!persistCtxExists(path)) {
                ERRR("Context should exist now");
            }
        }
    }

    TEST("SyncIfDue syncs an EVERYSEC context once its second is up") {
        char path[PATH_MAX];
        if (!dkTestTmpDirPath(&tmp, "persistCtxSyncIfDue", path,
                              sizeof(path))) {
            ERR("could not create a scratch directory: %s", strerror(errno));
        } else {
            /* @own: caller; free=persistCtxFree */
            persistCtx *ctx = persistCtxNew(path, NULL);
            /* @own: caller; free=flexCloseP */
            flexP *f = flexNewP(ctx);
            const databox one = DATABOX_SIGNED(1);
            flexPushTailP(f, &one);

            const uint64_t syncs = ctx->totalSyncs;
            if (!persistCtxSyncIfDue(ctx) || ctx->totalSyncs != syncs) {
                ERRR("synced before a second had passed");
            }
            ctx->lastSyncTime -= 2000000;
            if (!persistCtxSyncIfDue(ctx) || ctx->totalSyncs != syncs + 1) {
                ERRR("did not sync once a second had passed");
            }

            /* Other modes leave syncing to their own policy */
            ctx->config.syncMode = PERSIST_CTX_SYNC_NONE;
            ctx->lastSyncTime -= 2000000;
            if (!persistCtxSyncIfDue(ctx) || ctx->totalSyncs != syncs + 1) {
                ERRR("synced a SYNC_NONE context");
            }
            if (persistCtxSyncIfDue(NULL)) {
                ERRR("accepted a NULL context");
            }

            flexCloseP(f);
            persistCtxFree(ctx);
        }
    }

    TEST("a failed sync keeps the op applied, logged and counted") {
        char path[PATH_MAX];
        if (!dkTestTmpDirPath(&tmp, "persistCtxSyncFail", path, sizeof(path))) {
            ERR("could not create a scratch directory: %s", strerror(errno));
        } else {
            persistCtxConfig config = persistCtxDefaultConfig();
            config.syncMode = PERSIST_CTX_SYNC_ALWAYS;
            /* @own: caller; free=persistCtxFree */
            persistCtx *ctx = persistCtxNew(path, &config);
            /* @own: caller; free=flexCloseP */
            flexP *f = flexNewP(ctx);
            const databox one = DATABOX_SIGNED(1);
            flexPushTailP(f, &one);

            /* The device fills for one op: the op is applied in memory
             * and in the WAL alike, reported and counted */
            persistStore *wal = persistWALStore(ctx->p);
            bool (*sync)(void *) = wal->sync;
            wal->sync = persistCtxTestFailSync;
            const databox two = DATABOX_SIGNED(2);
            if (persistCtxLogOp(ctx, PERSIST_OP_PUSH_TAIL, &two, 1) !=
                PERSIST_LOG_NOT_DURABLE) {
                ERRR("a failed sync was not reported as NOT_DURABLE");
            }
            flexPushTailP(f, &two);
            wal->sync = sync;

            persistCtxStats stats;
            persistCtxGetStats(ctx, &stats);
            if (stats.notDurableOps != 2 || stats.syncFailures != 2 ||
                stats.lastSyncError != ENOSPC) {
                ERR("not durable %" PRIu64 ", failures %" PRIu64 ", error %d",
                    stats.notDurableOps, stats.syncFailures,
                    stats.lastSyncError);
            }
            if (flexCountP(f) != 2) {
                ERRR("the wrapper dropped an op whose sync failed");
            }
            flexCloseP(f);
            persistCtxFree(ctx);

            /* Recovery replays all three logged pushes */
            /* @own: caller; free=persistCtxFree */
            ctx = persistCtxNew(path, &config);
            /* @own: caller; free=flexCloseP */
            f = flexOpenP(ctx);
            if (!f || flexCountP(f) != 3) {
                ERRR("ops logged across a failed sync did not recover");
            }
            flexCloseP(f);
            persistCtxFree(ctx);
        }
    }

    if (!dkTestTmpDirRemove(&tmp)) {
        ERRR("could not remove the scratch directory");
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
