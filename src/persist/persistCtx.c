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
    char *path = zmalloc(len);
    snprintf(path, len, "%s.snap", basePath);
    return path;
}

static char *makeWalPath(const char *basePath) {
    size_t len = strlen(basePath) + 5; /* ".wal\0" */
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
    char *dup = zmalloc(len);
    memcpy(dup, s, len);
    return dup;
}

persistCtx *persistCtxNew(const char *basePath,
                          const persistCtxConfig *config) {
    if (!basePath) {
        return NULL;
    }

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

bool persistCtxLogOp(persistCtx *ctx, persistOp op, const void *args,
                     size_t argc) {
    if (!ctx || !ctx->p) {
        return false;
    }

    /* Log to WAL */
    if (!persistLogOp(ctx->p, op, args, argc)) {
        return false;
    }

    /* Update counters */
    ctx->opsSinceCompact++;
    ctx->totalOps++;

    /* Handle sync policy */
    if (ctx->config.syncMode == PERSIST_CTX_SYNC_ALWAYS) {
        persistSync(ctx->p);
        ctx->totalSyncs++;
    } else if (ctx->config.syncMode == PERSIST_CTX_SYNC_EVERYSEC) {
        uint64_t now = getTimeMicroseconds();
        if (now - ctx->lastSyncTime >= 1000000) { /* 1 second */
            persistSync(ctx->p);
            ctx->lastSyncTime = now;
            ctx->totalSyncs++;
        }
    }

    return true;
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
        ctx->bytesSinceCompact = 0;
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
    stats->totalCompactions = ctx->totalCompactions;
    stats->opsSinceCompact = ctx->opsSinceCompact;
    stats->bytesSinceCompact = ctx->bytesSinceCompact;

    if (ctx->p) {
        persistStats pstats;
        persistGetStats(ctx->p, &pstats);
        stats->snapshotBytes = pstats.snapshotBytes;
        stats->walBytes = pstats.walBytes;
    }
}

/* ============================================================================
 * Tests
 * ============================================================================
 */

#ifdef DATAKIT_TEST
#include "../ctest.h"
#include <unistd.h>

int persistCtxTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;

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
        const char *path = "/tmp/persistCtxTest";
        persistCtx *ctx = persistCtxNew(path, NULL);
        if (!ctx) {
            ERRR("Failed to create persistCtx");
        }
        if (strcmp(ctx->basePath, path) != 0) {
            ERRR("Base path not set correctly");
        }
        persistCtxFree(ctx);

        /* Clean up */
        char snapPath[256], walPath[256];
        snprintf(snapPath, sizeof(snapPath), "%s.snap", path);
        snprintf(walPath, sizeof(walPath), "%s.wal", path);
        unlink(snapPath);
        unlink(walPath);
    }

    TEST("persistCtx exists check") {
        const char *path = "/tmp/persistCtxExistsTest";

        /* Should not exist initially */
        if (persistCtxExists(path)) {
            ERRR("Context should not exist yet");
        }

        /* Create files manually */
        char snapPath[256];
        snprintf(snapPath, sizeof(snapPath), "%s.snap", path);
        FILE *f = fopen(snapPath, "w");
        if (f) {
            fclose(f);
        }

        /* Now should exist */
        if (!persistCtxExists(path)) {
            ERRR("Context should exist now");
        }

        /* Clean up */
        unlink(snapPath);
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
