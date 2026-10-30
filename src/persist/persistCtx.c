/* persistCtx.c - Common infrastructure for persistent data structure wrappers
 *
 * See persistCtx.h for API documentation.
 */

#include "persistCtx.h"
#include "../cleaner.h"
#include "../datakit.h"
#include "../dkDiag.h"
#include "../timeUtil.h"

#include <errno.h>
#include <stdio.h> /* snprintf */
#include <string.h>

struct persistCtx {
    /* Core persistence layer */
    persist *p;

    /* File paths */
    char *basePath;     /* Base path for files */
    char *snapshotPath; /* path.snap */
    char *walPath;      /* path.wal */

    /* Configuration */
    persistCtxConfig config;

    /* State tracking */
    size_t opsSinceCompact; /* Operations since last compaction */
    uint64_t lastSyncTime;  /* Microseconds since epoch */

    /* Statistics */
    uint64_t totalOps;
    uint64_t totalSyncs;
    uint64_t notDurableOps;
    uint64_t totalCompactions;

    /* Bad-entry notification, kept until the structure's persist exists */
    persistBadEntryNotify *badEntryNotify;
    void *badEntryNotifyCtx;

    /* A live wrapper (made by a *NewP or *OpenP, not yet closed) holds the
     * context: one structure's snapshot and WAL belong to one live wrapper */
    bool claimed;
};

defcleanerName(char *, zfree, Chars);

/* ============================================================================
 * Time Utilities
 * ============================================================================
 */

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
        .compression = PERSIST_COMPRESS_NONE,
        .compressionLevel = 0,
        .compactRatio = 0,
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
        return nullptr;
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
    ctx->lastSyncTime = timeUtilUs();

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

    /* A live wrapper holds the context: no second structure on it */
    if (ctx->claimed) {
        DK_DIAG(DK_DIAG_PERSIST_CTX_CLAIMED,
                "%s is held by a live wrapper; close it before opening "
                "another",
                ctx->basePath);
        return false;
    }

    /* Already initialized: only for the same type.  A context holds one
     * structure's snapshot and WAL, which another type's codecs would
     * misread (or write over) */
    if (ctx->p) {
        const persistOps *bound = persistGetOps(ctx->p);
        if (bound != ops) {
            DK_DIAG(DK_DIAG_PERSIST_CTX_WRONG_TYPE, "%s is bound to %s, not %s",
                    ctx->basePath, bound->name, ops->name);
            return false;
        }
        return true;
    }

    /* Create persist config from ctx config */
    persistConfig pconfig = persistDefaultConfig();
    pconfig.walBufferSize = ctx->config.walBufferSize;
    pconfig.strictRecovery = ctx->config.strictRecovery;
    pconfig.compression = ctx->config.compression;
    pconfig.compressionLevel = ctx->config.compressionLevel;

    /* One compaction policy: the WAL size and ratio tests are the core's
     * (persistShouldCompact); the op count is the context's own */
    pconfig.walMaxSize = ctx->config.compactAfterBytes;
    pconfig.compactRatio = ctx->config.compactRatio;
    pconfig.autoCompact =
        ctx->config.compactAfterBytes > 0 || ctx->config.compactRatio > 0;

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
        persistStoreClose(snapStore);
        persistStoreClose(walStore);
        persistClose(ctx->p);
        ctx->p = nullptr;
        return false;
    }

    persistAttachSnapshot(ctx->p, snapStore);
    persistAttachWAL(ctx->p, walStore);

    /* Damaged WAL bytes heal into <base>.quarantine, created only then */
    const size_t qlen = strlen(ctx->basePath) + sizeof(".quarantine");
    cleanerName(zfree, Chars) char *qpath = zmalloc(qlen);
    snprintf(qpath, qlen, "%s.quarantine", ctx->basePath);
    persistAttachQuarantineFile(ctx->p, qpath);
    persistSetBadEntryNotify(ctx->p, ctx->badEntryNotify,
                             ctx->badEntryNotifyCtx);

    return true;
}

bool persistCtxClaim(persistCtx *ctx) {
    if (!ctx) {
        return false;
    }
    if (ctx->claimed) {
        DK_DIAG(DK_DIAG_PERSIST_CTX_CLAIMED,
                "%s is already held by a live wrapper", ctx->basePath);
        return false;
    }
    ctx->claimed = true;
    return true;
}

void persistCtxRelease(persistCtx *ctx) {
    if (ctx) {
        ctx->claimed = false;
    }
}

bool persistCtxStartNew(persistCtx *ctx, void *structure,
                        const persistOps *ops) {
    if (!ctx || !structure || !persistCtxInitForType(ctx, ops) ||
        !persistCtxSaveSnapshot(ctx, structure, ops)) {
        return false;
    }

    return persistCtxClaim(ctx);
}

void *persistCtxStartRecovered(persistCtx *ctx, const persistOps *ops) {
    /* Nothing on disk is nothing to open: the context stays unbound, so a
     * New of any type can still take it */
    if (!ctx || !persistCtxExists(ctx->basePath) ||
        !persistCtxInitForType(ctx, ops)) {
        return nullptr;
    }

    void *recovered = persistCtxRecover(ctx, ops);
    if (recovered) {
        persistCtxClaim(ctx);
    }

    return recovered;
}

void persistCtxFinish(persistCtx *ctx) {
    if (ctx) {
        persistCtxSync(ctx);
        persistCtxRelease(ctx);
    }
}

bool persistCtxLastBadEntry(const persistCtx *ctx, persistBadEntryReport *out) {
    return persistLastBadEntry(ctx ? ctx->p : nullptr, out);
}

void persistCtxSetBadEntryNotify(persistCtx *ctx, persistBadEntryNotify *fn,
                                 void *notifyCtx) {
    if (ctx) {
        ctx->badEntryNotify = fn;
        ctx->badEntryNotifyCtx = notifyCtx;
        persistSetBadEntryNotify(ctx->p, fn, notifyCtx);
    }
}

/* ============================================================================
 * Operations
 * ============================================================================
 */

bool persistCtxCanLog(persistCtx *ctx) {
    return ctx && ctx->p && persistWALReady(ctx->p);
}

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
        ctx->lastSyncTime = timeUtilUs();
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

    /* The WAL size and ratio tests: the core's policy */
    if (!shouldCompact && persistShouldCompact(ctx->p)) {
        shouldCompact = true;
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

    const uint64_t now = timeUtilUs();
    if (now - ctx->lastSyncTime >= 1'000'000) {
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
        ctx->lastSyncTime = timeUtilUs();
        ctx->totalSyncs++;
    }
    return result;
}

bool persistCtxCompact(persistCtx *ctx, void *structure,
                       const persistOps *ops) {
    if (!ctx || !ctx->p || !structure || !ops) {
        return false;
    }
    const persistOps *bound = persistGetOps(ctx->p);
    if (bound != ops) {
        DK_DIAG(DK_DIAG_PERSIST_CTX_WRONG_TYPE, "%s is bound to %s, not %s",
                ctx->basePath, bound->name, ops->name);
        return false;
    }

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
        return nullptr;
    }

    /* Initialize persist context if needed */
    if (!persistCtxInitForType(ctx, ops)) {
        return nullptr;
    }

    /* Recover from snapshot + WAL */
    return persistRecover(ctx->p);
}

persistCtxState persistCtxProbe(const char *basePath) {
    if (!basePath) {
        return PERSIST_CTX_ABSENT;
    }

    char *snapPath = makeSnapshotPath(basePath);
    const bool snapshot = access(snapPath, F_OK) == 0;
    zfree(snapPath);
    if (snapshot) {
        return PERSIST_CTX_PRESENT;
    }

    char *walPath = makeWalPath(basePath);
    const bool wal = access(walPath, F_OK) == 0;
    zfree(walPath);
    return wal ? PERSIST_CTX_ORPHAN_WAL : PERSIST_CTX_ABSENT;
}

bool persistCtxExists(const char *basePath) {
    const persistCtxState state = persistCtxProbe(basePath);
    if (state == PERSIST_CTX_ORPHAN_WAL) {
        /* There is data, but the Open paths treat the path as absent:
         * surface it rather than pass over the log in silence */
        DK_DIAG_CONDITION(DK_DIAG_PERSIST_ORPHAN_WAL,
                          "%s.wal exists without its snapshot; an Open "
                          "will not replay it",
                          basePath);
    }
    return state == PERSIST_CTX_PRESENT;
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
        /* Compaction starts a fresh WAL, so the live WAL (buffered
         * bytes counted) is what accumulated since the last compaction:
         * the byte-based compaction trigger uses the same measure */
        stats->bytesSinceCompact = persistWALBytes(ctx->p);
    }
}

/* ============================================================================
 * Tests
 * ============================================================================
 */

#ifdef DATAKIT_TEST
bool persistCtxTestRefuseLogsBegin(persistCtx *ctx, persistCtxTestRefusal *r) {
    *r = (persistCtxTestRefusal){};
    if (!ctx || !ctx->p) {
        return false;
    }

    /* A fresh store is readied on its first log by a header write, which
     * fails, so the very next log is refused whatever the buffering */
    r->original = persistWALStore(ctx->p);
    r->failing = persistStoreFaulty(persistStoreMemory(0), false);
    persistStoreFaultyArm(r->failing, PERSIST_FAULT_WRITE, 0);
    if (!persistAttachWAL(ctx->p, r->failing)) {
        persistStoreClose(r->failing);
        *r = (persistCtxTestRefusal){};
        return false;
    }
    return true;
}

void persistCtxTestRefuseLogsEnd(persistCtx *ctx, persistCtxTestRefusal *r) {
    if (r->failing) {
        persistStoreFaultyDisarm(r->failing);
        persistAttachWAL(ctx->p, r->original);
        persistStoreClose(r->failing);
    }
    *r = (persistCtxTestRefusal){};
}

#include "../ctest.h"
#include "../dkTestTmp.h"
#include "flexP.h"
#include "persistTestCommon.h"
#include <unistd.h>

/* Counts bad-entry notifications */
static void persistCtxTestNotify(persist *, const persistBadEntryReport *,
                                 void *ctx) {
    (*(int *)ctx)++;
}

/* A store sync that fails as a full device would */
static bool persistCtxTestFailSync(void *) {
    errno = ENOSPC;
    return false;
}

int persistCtxTest(int, char *[]) {
    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};

    TEST("persistCtx default config") {
        persistCtxConfig config = persistCtxDefaultConfig();
        if (config.syncMode != PERSIST_CTX_SYNC_EVERYSEC) {
            ERR("Default sync mode should be EVERYSEC");
        }
        if (config.compactAfterOps != 10000) {
            ERR("Default compact after ops should be 10000");
        }
    }

    TEST("persistCtx create and free") {
        char path[PATH_MAX];
        if (!dkTestTmpDirPath(&tmp, "persistCtxTest", path, sizeof(path))) {
            ERR("could not create a scratch directory: %s", strerror(errno));
        } else {
            persistCtx *ctx = persistCtxNew(path, nullptr);
            if (!ctx) {
                ERR("Failed to create persistCtx");
            } else {
                if (strcmp(ctx->basePath, path) != 0) {
                    ERR("Base path not set correctly");
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
                ERR("Context should not exist yet");
            }

            /* Create files manually */
            FILE *f = fopen(snapPath, "w");
            if (f) {
                fclose(f);
            }

            /* Now should exist */
            if (!persistCtxExists(path)) {
                ERR("Context should exist now");
            }
        }
    }

    TEST("a WAL without its snapshot probes as orphaned and is reported") {
        char path[PATH_MAX];
        char walPath[PATH_MAX];
        char snapPath[PATH_MAX];
        if (!dkTestTmpDirPath(&tmp, "persistCtxOrphan", path, sizeof(path)) ||
            !dkTestTmpDirPath(&tmp, "persistCtxOrphan.wal", walPath,
                              sizeof(walPath)) ||
            !dkTestTmpDirPath(&tmp, "persistCtxOrphan.snap", snapPath,
                              sizeof(snapPath))) {
            ERR("could not create a scratch directory: %s", strerror(errno));
        } else {
            const persistCtxState absent = persistCtxProbe(path);
            FILE *w = fopen(walPath, "w");
            if (w) {
                fclose(w);
            }

            /* Probing asks, so it reports nothing; Exists answers false
             * and records the condition */
            const dkDiagPrintMode prior = dkDiagGetPrintMode();
            dkDiagSetPrintMode(DK_DIAG_PRINT_NEVER);
            const uint64_t before = dkDiagCount();
            const persistCtxState orphan = persistCtxProbe(path);
            const bool probeQuiet = dkDiagCount() == before;
            const bool exists = persistCtxExists(path);
            dkDiagnostic d;
            const bool reported =
                dkDiagCount() == before + 1 && dkDiagLast(&d) &&
                d.code == DK_DIAG_PERSIST_ORPHAN_WAL &&
                d.severity == DK_DIAG_CONDITION && strstr(d.message, path);
            dkDiagSetPrintMode(prior);

            FILE *s = fopen(snapPath, "w");
            if (s) {
                fclose(s);
            }
            const uint64_t afterSnap = dkDiagCount();
            const persistCtxState present = persistCtxProbe(path);
            const bool presentExists = persistCtxExists(path);
            if (absent != PERSIST_CTX_ABSENT ||
                orphan != PERSIST_CTX_ORPHAN_WAL || !probeQuiet || exists ||
                !reported || present != PERSIST_CTX_PRESENT || !presentExists ||
                dkDiagCount() != afterSnap ||
                persistCtxProbe(nullptr) != PERSIST_CTX_ABSENT) {
                ERR("probe %d/%d/%d, exists %d/%d, reported %d", absent, orphan,
                    present, exists, presentExists, reported);
            }
        }
    }

    TEST("a context bound to one structure type refuses every other") {
        /* Bound to each type in turn: the same type initializes again;
         * every other type is refused by init, recover, snapshot and
         * compaction before its codecs see the structure */
        static const persistOps *const types[] = {
            &persistOpsFlex,        &persistOpsIntset,
            &persistOpsIntsetBig,   &persistOpsMultiroar,
            &persistOpsHyperloglog, &persistOpsMultilist,
            &persistOpsMultimap,    &persistOpsMultidict,
            &persistOpsMultiarray,  &persistOpsMultiOrderedSet,
            &persistOpsMultiLRU,
        };
        const size_t n = countof(types);
        uint64_t untouched = 0x5e17; /* never read by a refused call */
        /* Each refused call reports DK_DIAG_PERSIST_CTX_WRONG_TYPE once */
        const dkDiagPrintMode priorPrint = dkDiagGetPrintMode();
        dkDiagSetPrintMode(DK_DIAG_PRINT_NEVER);
        for (size_t bound = 0; bound < n; bound++) {
            char path[PATH_MAX];
            char name[32];
            snprintf(name, sizeof(name), "persistCtxBound%zu", bound);
            if (!dkTestTmpDirPath(&tmp, name, path, sizeof(path))) {
                ERR("could not create a scratch directory: %s",
                    strerror(errno));
                break;
            }
            persistCtx *ctx = persistCtxNew(path, nullptr);
            const uint64_t beforeBind = dkDiagCount();
            if (!persistCtxInitForType(ctx, types[bound]) ||
                !persistCtxInitForType(ctx, types[bound]) ||
                dkDiagCount() != beforeBind) {
                ERR("%s: could not bind, or binding reported",
                    types[bound]->name);
            }
            const uint64_t beforeOthers = dkDiagCount();
            size_t accepted = 0;
            for (size_t other = 0; other < n; other++) {
                if (other == bound) {
                    continue;
                }
                accepted += persistCtxInitForType(ctx, types[other]);
                accepted += persistCtxRecover(ctx, types[other]) != nullptr;
                accepted +=
                    persistCtxSaveSnapshot(ctx, &untouched, types[other]);
                accepted += persistCtxCompact(ctx, &untouched, types[other]);
            }
            if (accepted || untouched != 0x5e17) {
                ERR("%s: %zu calls of other types accepted", types[bound]->name,
                    accepted);
            }
            dkDiagnostic d;
            if (dkDiagCount() - beforeOthers != 4 * (n - 1) ||
                !dkDiagLast(&d) || d.code != DK_DIAG_PERSIST_CTX_WRONG_TYPE ||
                !strstr(d.message, types[bound]->name)) {
                ERR("%s: %" PRIu64 " wrong-type reports, want %zu",
                    types[bound]->name, dkDiagCount() - beforeOthers,
                    4 * (n - 1));
            }
            persistCtxFree(ctx);
        }
        dkDiagSetPrintMode(priorPrint);
    }

    TEST("an open with nothing on disk leaves the context unbound") {
        /* A flexP open of an empty path finds nothing and binds nothing:
         * a structure of another type then starts on the same context */
        char path[PATH_MAX];
        if (!dkTestTmpDirPath(&tmp, "persistCtxOpenNothing", path,
                              sizeof(path))) {
            ERR("could not create a scratch directory: %s", strerror(errno));
        } else {
            persistCtx *ctx = persistCtxNew(path, nullptr);
            flexP *opened = flexOpenP(ctx);
            const uint64_t before = dkDiagCount();
            const bool bound = persistCtxInitForType(ctx, &persistOpsIntset);
            if (opened || !bound || dkDiagCount() != before ||
                persistCtxStartRecovered(nullptr, &persistOpsFlex) ||
                persistCtxStartRecovered(ctx, &persistOpsFlex)) {
                ERR("open of nothing gave %p, another type bound %d (%" PRIu64
                    " reports)",
                    (void *)opened, bound, dkDiagCount() - before);
            }
            flexCloseP(opened);
            persistCtxFree(ctx);
        }
    }

    TEST("a live wrapper holds its context until it closes") {
        /* While a flexP is live, a second New or Open on its context is
         * refused and the first one's data stays its own; after the close
         * an Open takes the context and sees the first one's values */
        char path[PATH_MAX];
        if (!dkTestTmpDirPath(&tmp, "persistCtxClaim", path, sizeof(path))) {
            ERR("could not create a scratch directory: %s", strerror(errno));
        } else {
            persistCtx *ctx = persistCtxNew(path, nullptr);
            flexP *first = flexNewP(ctx);
            const databox one = DATABOX_SIGNED(1);
            PTEST_LOGGED(flexPushTailP(first, &one));
            /* Each refusal reports DK_DIAG_PERSIST_CTX_CLAIMED once */
            const dkDiagPrintMode priorPrint = dkDiagGetPrintMode();
            dkDiagSetPrintMode(DK_DIAG_PRINT_NEVER);
            const uint64_t beforeRefusals = dkDiagCount();
            flexP *second = flexNewP(ctx);
            flexP *opened = flexOpenP(ctx);
            dkDiagnostic d;
            const bool reported = dkDiagCount() - beforeRefusals == 2 &&
                                  dkDiagLast(&d) &&
                                  d.code == DK_DIAG_PERSIST_CTX_CLAIMED;
            dkDiagSetPrintMode(priorPrint);
            const bool refused = first && !second && !opened;
            flexCloseP(second);
            flexCloseP(opened);
            flexCloseP(first);

            const uint64_t beforeReopen = dkDiagCount();
            flexP *again = flexOpenP(ctx);
            if (!refused || !reported || !again || flexCountP(again) != 1 ||
                dkDiagCount() != beforeReopen) {
                ERR("live wrapper refused %d (reported %d), reopen %s with "
                    "%zu values",
                    refused, reported, again ? "made" : "refused",
                    again ? flexCountP(again) : (size_t)0);
            }
            flexCloseP(again);
            persistCtxFree(ctx);
        }
    }

    TEST("sync modes, byte-triggered compaction, a second claim, the "
         "bad-entry notifier, missing arguments") {
        char path[PATH_MAX];
        if (!dkTestTmpDirPath(&tmp, "persistCtxModes", path, sizeof(path))) {
            ERR("could not create a scratch directory: %s", strerror(errno));
        } else {
            /* NONE buffers each op; ALWAYS makes each durable */
            static const struct {
                persistCtxSyncMode mode;
                persistLogResult want;
            } modes[] = {{PERSIST_CTX_SYNC_NONE, PERSIST_LOG_BUFFERED},
                         {PERSIST_CTX_SYNC_ALWAYS, PERSIST_LOG_DURABLE}};
            const databox one = DATABOX_SIGNED(1);
            for (size_t m = 0; m < countof(modes); m++) {
                ptestCleanupFiles(path);
                persistCtxConfig config = persistCtxDefaultConfig();
                config.syncMode = modes[m].mode;
                persistCtx *ctx = persistCtxNew(path, &config);
                flexP *f = flexNewP(ctx);
                const persistLogResult got = flexPushTailP(f, &one);
                if (got != modes[m].want) {
                    ERR("sync mode %d logged as %d, not %d", (int)modes[m].mode,
                        (int)got, (int)modes[m].want);
                }
                flexCloseP(f);
                persistCtxFree(ctx);
            }

            /* A ratio alone compacts when the live WAL passes the snapshot
             * times the ratio: the WAL restarts each time, so 300 entries
             * against a growing snapshot compact several times, not on
             * every op */
            {
                ptestCleanupFiles(path);
                persistCtxConfig config = persistCtxDefaultConfig();
                config.compactAfterOps = 0;
                config.compactAfterBytes = 0;
                config.compactRatio = 1.0;
                persistCtx *rctx = persistCtxNew(path, &config);
                flexP *rf = flexNewP(rctx);
                for (int64_t i = 0; i < 300; i++) {
                    const databox v = DATABOX_SIGNED(i);
                    PTEST_LOGGED(flexPushTailP(rf, &v));
                }
                persistCtxStats rstats;
                persistCtxGetStats(rctx, &rstats);
                if (rstats.totalCompactions < 2 ||
                    rstats.totalCompactions > 150 || flexCountP(rf) != 300) {
                    ERR("a ratio of 1 compacted %" PRIu64 " times",
                        rstats.totalCompactions);
                }
                flexCloseP(rf);
                persistCtxFree(rctx);
            }

            /* A byte threshold alone compacts each time the live WAL (its
             * buffered bytes counted) passes it, and only then: 200 small
             * entries past a 256-byte threshold compact a few dozen times,
             * never on every op, buffered or not */
            persistCtxStats stats;
            persistCtx *ctx = nullptr;
            flexP *f = nullptr;
            for (int buffered = 0; buffered <= 1; buffered++) {
                ptestCleanupFiles(path);
                persistCtxConfig config = persistCtxDefaultConfig();
                config.compactAfterOps = 0;
                config.compactAfterBytes = 256;
                config.walBufferSize = buffered ? 64 * 1024 : 0;
                ctx = persistCtxNew(path, &config);
                f = flexNewP(ctx);
                for (int64_t i = 0; i < 200; i++) {
                    const databox v = DATABOX_SIGNED(i);
                    PTEST_LOGGED(flexPushTailP(f, &v));
                }
                persistCtxGetStats(ctx, &stats);
                if (stats.totalCompactions < 2 || stats.totalCompactions > 60 ||
                    stats.bytesSinceCompact >= 256 || flexCountP(f) != 200) {
                    ERR("buffered %d: a 256-byte threshold compacted %" PRIu64
                        " times, %zu bytes since",
                        buffered, stats.totalCompactions,
                        stats.bytesSinceCompact);
                }
                if (!buffered) {
                    flexCloseP(f);
                    persistCtxFree(ctx);
                }
            }

            /* A context its wrapper holds refuses a second claim, once */
            const dkDiagPrintMode priorPrint = dkDiagGetPrintMode();
            dkDiagSetPrintMode(DK_DIAG_PRINT_NEVER);
            bool claimed = true;
            DK_DIAG_EXPECT_REPORTS("a held context", 1,
                                   DK_DIAG_PERSIST_CTX_CLAIMED,
                                   claimed = persistCtxClaim(ctx));

            /* A second wrapper is refused before it touches the files:
             * the holder's snapshot and count stay */
            flexP *second = nullptr;
            DK_DIAG_EXPECT_REPORTS("a second wrapper", 1,
                                   DK_DIAG_PERSIST_CTX_CLAIMED,
                                   second = flexNewP(ctx));
            dkDiagSetPrintMode(priorPrint);
            if (claimed || second || flexCountP(f) != 200) {
                ERR("a held context was claimed again");
            }

            /* The close syncs (counted in the stats) and frees the context
             * for the next wrapper */
            persistCtxGetStats(ctx, &stats);
            const uint64_t syncsBefore = stats.totalSyncs;
            flexCloseP(f);
            persistCtxGetStats(ctx, &stats);
            if (stats.totalSyncs != syncsBefore + 1 || !persistCtxClaim(ctx)) {
                ERR("a close synced %" PRIu64 " times or kept the context",
                    stats.totalSyncs - syncsBefore);
            }
            persistCtxRelease(ctx);
            persistCtxFree(ctx);

            /* The notifier set on a context hears of a damaged entry its
             * open heals (a WAL of three entries, its last one damaged) */
            ptestCleanupFiles(path);
            ctx = persistCtxNew(path, nullptr);
            f = flexNewP(ctx);
            for (int64_t i = 0; i < 3; i++) {
                const databox v = DATABOX_SIGNED(i);
                PTEST_LOGGED(flexPushTailP(f, &v));
            }
            flexCloseP(f);
            persistCtxFree(ctx);
            if (!ptestDamageLastWalEntry(path)) {
                ERR("could not damage the WAL");
            }
            int notified = 0;
            ctx = persistCtxOpen(path, nullptr);
            persistCtxSetBadEntryNotify(ctx, persistCtxTestNotify, &notified);
            f = flexOpenP(ctx);
            persistBadEntryReport report;
            if (!f || notified != 1 || !persistCtxLastBadEntry(ctx, &report)) {
                ERR("a healed entry notified %d times", notified);
            }
            flexCloseP(f);
            persistCtxFree(ctx);

            /* Missing arguments: refused or empty answers */
            const int64_t v = 1;
            if (persistCtxNew(nullptr, nullptr) ||
                persistCtxOpen(nullptr, nullptr) || persistCtxCanLog(nullptr) ||
                persistCtxLogOp(nullptr, PERSIST_OP_INSERT, &v, 1) ||
                persistCtxMaybeCompact(nullptr, nullptr, &persistOpsFlex) ||
                persistCtxSync(nullptr) || persistCtxSyncIfDue(nullptr) ||
                persistCtxCompact(nullptr, nullptr, &persistOpsFlex) ||
                persistCtxRecover(nullptr, &persistOpsFlex) ||
                persistCtxInitForType(nullptr, &persistOpsFlex) ||
                persistCtxSaveSnapshot(nullptr, nullptr, &persistOpsFlex) ||
                persistCtxClaim(nullptr) ||
                persistCtxLastBadEntry(nullptr, &report)) {
                ERR("a call without its context was not refused");
            }
            persistCtxGetStats(nullptr, &stats);
            persistCtxSetBadEntryNotify(nullptr, persistCtxTestNotify,
                                        &notified);
            persistCtxRelease(nullptr);
            persistCtxFree(nullptr);
            ptestCleanupFiles(path);
        }
    }

    TEST("SyncIfDue syncs an EVERYSEC context once its second is up") {
        char path[PATH_MAX];
        if (!dkTestTmpDirPath(&tmp, "persistCtxSyncIfDue", path,
                              sizeof(path))) {
            ERR("could not create a scratch directory: %s", strerror(errno));
        } else {
            /* @own: caller; free=persistCtxFree */
            persistCtx *ctx = persistCtxNew(path, nullptr);
            /* @own: caller; free=flexCloseP */
            flexP *f = flexNewP(ctx);
            const databox one = DATABOX_SIGNED(1);
            PTEST_LOGGED(flexPushTailP(f, &one));

            const uint64_t syncs = ctx->totalSyncs;
            if (!persistCtxSyncIfDue(ctx) || ctx->totalSyncs != syncs) {
                ERR("synced before a second had passed");
            }
            ctx->lastSyncTime -= 2'000'000;
            if (!persistCtxSyncIfDue(ctx) || ctx->totalSyncs != syncs + 1) {
                ERR("did not sync once a second had passed");
            }

            /* Other modes leave syncing to their own policy */
            ctx->config.syncMode = PERSIST_CTX_SYNC_NONE;
            ctx->lastSyncTime -= 2'000'000;
            if (!persistCtxSyncIfDue(ctx) || ctx->totalSyncs != syncs + 1) {
                ERR("synced a SYNC_NONE context");
            }
            if (persistCtxSyncIfDue(nullptr)) {
                ERR("accepted a NULL context");
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
            PTEST_LOGGED(flexPushTailP(f, &one));

            /* The device fills for one op: the op is applied in memory
             * and in the WAL alike, reported and counted */
            persistStore *wal = persistWALStore(ctx->p);
            bool (*sync)(void *) = wal->sync;
            wal->sync = persistCtxTestFailSync;
            const databox two = DATABOX_SIGNED(2);
            if (persistCtxLogOp(ctx, PERSIST_OP_PUSH_TAIL, &two, 1) !=
                PERSIST_LOG_NOT_DURABLE) {
                ERR("a failed sync was not reported as NOT_DURABLE");
            }
            PTEST_LOGGED(flexPushTailP(f, &two));
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
                ERR("the wrapper dropped an op whose sync failed");
            }
            flexCloseP(f);
            persistCtxFree(ctx);

            /* Recovery replays all three logged pushes */
            /* @own: caller; free=persistCtxFree */
            ctx = persistCtxNew(path, &config);
            /* @own: caller; free=flexCloseP */
            f = flexOpenP(ctx);
            if (!f || flexCountP(f) != 3) {
                ERR("ops logged across a failed sync did not recover");
            }
            flexCloseP(f);
            persistCtxFree(ctx);
        }
    }

    if (!dkTestTmpDirRemove(&tmp)) {
        ERR("could not remove the scratch directory");
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
