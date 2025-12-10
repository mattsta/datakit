/* persistCtx.h - Common infrastructure for persistent data structure wrappers
 *
 * This provides the shared "persistent context" used by all *P() wrappers.
 * Each wrapper (multimapP, multilistP, etc.) uses a persistCtx to manage:
 *   - Automatic WAL logging on mutations
 *   - Configurable sync policies
 *   - Automatic compaction based on thresholds
 *   - Clean recovery on open
 *
 * Usage:
 *   persistCtx *ctx = persistCtxNew("/path/to/data", &config);
 *   multimapP *m = multimapNewP(ctx, 2);
 *   multimapInsertP(m, entries);   // Automatically logged to WAL
 *   multimapCloseP(m);
 *   persistCtxFree(ctx);
 */

#pragma once

#include "../persist.h"
#include <stdbool.h>
#include <stdint.h>

/* ============================================================================
 * Configuration
 * ============================================================================
 */

typedef enum persistCtxSyncMode {
    PERSIST_CTX_SYNC_NONE = 0, /* No auto-sync, manual only */
    PERSIST_CTX_SYNC_EVERYSEC, /* Sync at most once per second */
    PERSIST_CTX_SYNC_ALWAYS,   /* Sync after every operation */
} persistCtxSyncMode;

typedef struct persistCtxConfig {
    /* Sync policy */
    persistCtxSyncMode syncMode;

    /* Compaction thresholds (compact when EITHER is exceeded) */
    size_t compactAfterOps;   /* Compact after N operations (0 = disabled) */
    size_t compactAfterBytes; /* Compact after N WAL bytes (0 = disabled) */

    /* WAL buffer size */
    size_t walBufferSize; /* Buffer size for WAL writes */

    /* Recovery mode */
    bool strictRecovery; /* Fail on any WAL corruption vs skip bad entries */
} persistCtxConfig;

/* Default configuration */
persistCtxConfig persistCtxDefaultConfig(void);

/* ============================================================================
 * Persistent Context
 * ============================================================================
 */

typedef struct persistCtx {
    /* Core persistence layer */
    persist *p;

    /* File paths */
    char *basePath;     /* Base path for files */
    char *snapshotPath; /* path.snap */
    char *walPath;      /* path.wal */

    /* Configuration */
    persistCtxConfig config;

    /* State tracking */
    size_t opsSinceCompact;   /* Operations since last compaction */
    size_t bytesSinceCompact; /* WAL bytes since last compaction */
    uint64_t lastSyncTime;    /* Microseconds since epoch */

    /* Statistics */
    uint64_t totalOps;
    uint64_t totalSyncs;
    uint64_t totalCompactions;
} persistCtx;

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/* Create a new persistent context at the given path
 * Files created: <basePath>.snap, <basePath>.wal */
persistCtx *persistCtxNew(const char *basePath, const persistCtxConfig *config);

/* Open existing persistent context (for recovery) */
persistCtx *persistCtxOpen(const char *basePath,
                           const persistCtxConfig *config);

/* Close and free persistent context (syncs first) */
void persistCtxFree(persistCtx *ctx);

/* ============================================================================
 * Operations (called by wrappers)
 * ============================================================================
 */

/* Log an operation to WAL - returns false on failure */
bool persistCtxLogOp(persistCtx *ctx, persistOp op, const void *args,
                     size_t argc);

/* Check and perform compaction if thresholds exceeded
 * structurePtr: pointer to the in-memory structure for snapshotting */
bool persistCtxMaybeCompact(persistCtx *ctx, void *structure,
                            const persistOps *ops);

/* Force sync WAL to disk */
bool persistCtxSync(persistCtx *ctx);

/* Force compaction now */
bool persistCtxCompact(persistCtx *ctx, void *structure, const persistOps *ops);

/* ============================================================================
 * Recovery
 * ============================================================================
 */

/* Recover structure from snapshot + WAL replay
 * Returns NULL on failure (check errno) */
void *persistCtxRecover(persistCtx *ctx, const persistOps *ops);

/* Check if persistent context exists at path */
bool persistCtxExists(const char *basePath);

/* ============================================================================
 * Internal (used by wrappers)
 * ============================================================================
 */

/* Initialize persist context for a specific structure type
 * Must be called before logging operations */
bool persistCtxInitForType(persistCtx *ctx, const persistOps *ops);

/* Save initial snapshot when creating a new persistent structure */
bool persistCtxSaveSnapshot(persistCtx *ctx, void *structure,
                            const persistOps *ops);

/* ============================================================================
 * Statistics
 * ============================================================================
 */

typedef struct persistCtxStats {
    uint64_t totalOps;
    uint64_t totalSyncs;
    uint64_t totalCompactions;
    size_t opsSinceCompact;
    size_t bytesSinceCompact;
    int64_t snapshotBytes;
    int64_t walBytes;
} persistCtxStats;

void persistCtxGetStats(const persistCtx *ctx, persistCtxStats *stats);

#ifdef DATAKIT_TEST
int persistCtxTest(int argc, char *argv[]);
#endif
