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
 *   if (multimapInsertP(m, entries, NULL) == PERSIST_LOG_NOT_DURABLE) {
 *       // applied and logged; the sync failed (counted in the stats)
 *   }
 *   multimapCloseP(m);
 *   persistCtxFree(ctx);
 */

#pragma once

#include "../persist.h"
#include <stdint.h>

/* ============================================================================
 * Configuration
 * ============================================================================
 */

typedef enum persistCtxSyncMode {
    PERSIST_CTX_SYNC_NONE = 0, /* No auto-sync, manual only */
    PERSIST_CTX_SYNC_EVERYSEC, /* Sync at most once per second WHILE OPS
                                * ARRIVE: syncing is op-driven, so an op
                                * followed by idle silence stays unsynced
                                * until the next op, an explicit
                                * persistCtxSync/SyncIfDue, or close. Call
                                * persistCtxSyncIfDue from an event loop to
                                * bound the idle-tail loss window. */
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

    /* Snapshot body compression (persistConfig.compression and
     * compressionLevel; off by default) */
    persistCompression compression;
    int compressionLevel;
} persistCtxConfig;

/* Default configuration */
persistCtxConfig persistCtxDefaultConfig(void);

/* ============================================================================
 * Persistent Context
 * ============================================================================
 */

/* @handle: fixed: its address holds from creation to free */
typedef struct persistCtx persistCtx;

/* How a context binds: the first *NewP / *OpenP fixes the structure type
 * (persistCtxInitForType; any other type is refused from then on), and the
 * wrapper it returns holds the context until its *CloseP.  While a wrapper
 * holds it, another *NewP / *OpenP on it is refused (two live structures
 * would interleave their changes in one WAL, and the second's snapshot
 * would replace the first's).  After the close, a new wrapper of the same
 * type may take it.  Wrappers call these; an application need not. */
bool persistCtxClaim(persistCtx *ctx);
void persistCtxRelease(persistCtx *ctx);

/* A wrapper's lifecycle, one call per step:
 *   persistCtxStartNew: bind the context to 'ops', save 'structure' as its
 *     first snapshot and hold the context.  False, with nothing held and
 *     'structure' still the caller's, when the context is held, bound to
 *     another type, or the snapshot cannot be written.
 *   persistCtxStartRecovered: bind, recover the structure from snapshot
 *     plus WAL, and hold the context; nullptr (nothing held) on the same
 *     refusals or a failed recovery.  With nothing on disk
 *     (persistCtxExists false) it is nullptr and the context stays unbound.
 *   persistCtxFinish: the close: sync, then let the context go (a null
 *     context is a no-op). */
bool persistCtxStartNew(persistCtx *ctx, void *structure,
                        const persistOps *ops);
void *persistCtxStartRecovered(persistCtx *ctx, const persistOps *ops);
void persistCtxFinish(persistCtx *ctx);

/* Bad WAL entries (persist.h, "Bad WAL entries"): a context heals damaged
 * WAL bytes into <basePath>.quarantine (created only when needed) and holds
 * entries it cannot heal.  The last one met, and a callback for each new
 * one (set it before *OpenP to hear about recovery). */
bool persistCtxLastBadEntry(const persistCtx *ctx, persistBadEntryReport *out);
void persistCtxSetBadEntryNotify(persistCtx *ctx, persistBadEntryNotify *fn,
                                 void *notifyCtx);

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

/* Whether the WAL takes an append now (persistWALReady): a change a
 * wrapper persists by snapshot instead of by log is refused exactly when
 * this is false, as a logged change would be */
bool persistCtxCanLog(persistCtx *ctx);

/* Log an operation to the WAL and sync as the context's syncMode asks.
 * Apply the change exactly when the result is not PERSIST_LOG_REFUSED;
 * a failed sync reports PERSIST_LOG_NOT_DURABLE (never undoing the op)
 * and the next sync retries (see persistLogResult). */
persistLogResult persistCtxLogOp(persistCtx *ctx, persistOp op,
                                 const void *args, size_t argc);

/* Check and perform compaction if thresholds exceeded
 * structurePtr: pointer to the in-memory structure for snapshotting */
bool persistCtxMaybeCompact(persistCtx *ctx, void *structure,
                            const persistOps *ops);

/* Force sync WAL to disk; false (counted in syncFailures) when it fails */
bool persistCtxSync(persistCtx *ctx);

/* EVERYSEC helper for event loops: syncs only when a full interval has
 * passed since the last sync (no-op under other sync modes). Bounds
 * the idle-tail loss window that op-driven syncing leaves open. */
bool persistCtxSyncIfDue(persistCtx *ctx);

/* Force compaction now */
bool persistCtxCompact(persistCtx *ctx, void *structure, const persistOps *ops);

/* ============================================================================
 * Recovery
 * ============================================================================
 */

/* Recover structure from snapshot + WAL replay
 * Returns NULL on failure (check errno) */
void *persistCtxRecover(persistCtx *ctx, const persistOps *ops);

/* What is on disk at 'basePath' */
typedef enum persistCtxState {
    PERSIST_CTX_ABSENT = 0,  /* no snapshot and no WAL */
    PERSIST_CTX_PRESENT = 1, /* a snapshot (an Open can recover it) */
    /* a WAL without its snapshot: there is data, but every Open treats
     * the path as absent and would not replay it */
    PERSIST_CTX_ORPHAN_WAL = 2,
} persistCtxState;
persistCtxState persistCtxProbe(const char *basePath);

/* Whether a context exists at 'basePath' (persistCtxProbe says PRESENT).
 * An orphaned WAL answers false and is also reported as a CONDITION
 * diagnostic, DK_DIAG_PERSIST_ORPHAN_WAL (dkDiag.h), so the log is never
 * passed over unnoticed. */
bool persistCtxExists(const char *basePath);

/* ============================================================================
 * Internal (used by wrappers)
 * ============================================================================
 */

/* Initialize persist context for a specific structure type
 * Must be called before logging operations.  A context holds one
 * structure's files: once bound to a type, it refuses (false) any other
 * type, and so does every call taking 'ops' (start, open, recover,
 * snapshot, compaction) */
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
    uint64_t syncFailures;  /* flushes or syncs that failed */
    uint64_t notDurableOps; /* ops logged as PERSIST_LOG_NOT_DURABLE */
    int32_t lastSyncError;  /* errno of the latest sync when it failed; 0
                               once a later sync succeeds */
    uint64_t totalCompactions;
    size_t opsSinceCompact;
    size_t bytesSinceCompact; /* the live WAL's entry bytes, buffered ones
                                 counted: what compactAfterBytes measures */
    int64_t snapshotBytes;
    int64_t walBytes; /* WAL bytes ever written to the store */
} persistCtxStats;

void persistCtxGetStats(const persistCtx *ctx, persistCtxStats *stats);

#ifdef DATAKIT_TEST
int persistCtxTest(int argc, char *argv[]);

/* Tests of a wrapper's refused log: between Begin and End every log
 * through 'ctx' is refused (its WAL is a store whose writes fail);
 * End puts the context's own WAL store back. */
typedef struct persistCtxTestRefusal {
    persistStore *original;
    persistStore *failing;
} persistCtxTestRefusal;
bool persistCtxTestRefuseLogsBegin(persistCtx *ctx, persistCtxTestRefusal *r);
void persistCtxTestRefuseLogsEnd(persistCtx *ctx, persistCtxTestRefusal *r);
#endif
