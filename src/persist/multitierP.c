/* multitierP.c - a multitier whose changes are logged (see multitierP.h) */

#include "multitierP.h"
#include "../cleaner.h"
#include "../datakit.h"
#include "../persist.h"

struct multitierP {
    multitier *t;
    persistCtx *ctx;
    multitierSinkFn *sink; /* the caller's, before the log */
    void *sinkCtx;
    uint64_t sinkLogRefused; /* sink records the WAL refused */
};

defcleanerName(void *, zfree, Void);

/* Rows leaving for the sink: their keys logged, then the caller's sink */
static void multitierLogSinkP(void *ctx, const flex *rows) {
    multitierP *p = ctx;
    const size_t n = flexCount(rows) / 2;
    cleanerName(zfree, Void) void *storage =
        zcalloc(n ? n : 1, sizeof(databoxBig) + sizeof(databox *));
    databoxBig *keys = storage;
    const databox **args = (const databox **)(keys + (n ? n : 1));
    flexEntry *fe = n ? flexHead(rows) : nullptr;
    for (size_t r = 0; r < n; r++) {
        flexGetByTypeBig(fe, &keys[r]);
        args[r] = (const databox *)&keys[r];
        fe = flexNext(rows, fe);
        fe = flexNext(rows, fe);
    }

    /* The caller's sink has the rows before the log says they left: a
     * crash between the two, or a refused log, leaves them in the files
     * as well, so they come back on the next open (held twice, never
     * lost); refusals are counted */
    if (p->sink) {
        p->sink(p->sinkCtx, rows);
    }
    if (n && persistCtxLogOp(p->ctx, PERSIST_OP_BULK_DELETE, args, n) ==
                 PERSIST_LOG_REFUSED) {
        p->sinkLogRefused++;
    }
}

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

static multitierP *multitierWrapP(persistCtx *ctx, multitier *t,
                                  multitierSinkFn *sink, void *sinkCtx) {
    /* @own: caller; free=multitierCloseP */
    multitierP *p = zcalloc(1, sizeof(*p));
    p->ctx = ctx;
    p->t = t;
    p->sink = sink;
    p->sinkCtx = sinkCtx;
    multitierSetSink(t, multitierLogSinkP, p);
    return p;
}

multitierP *multitierNewP(persistCtx *ctx, const multitierConfig *config) {
    if (!ctx) {
        return nullptr;
    }

    /* @own: container; free=multitierFree */
    multitier *t = multitierNew(config);
    if (!t || !persistCtxStartNew(ctx, t, &persistOpsMultitier)) {
        multitierFree(t);
        return nullptr;
    }

    return multitierWrapP(ctx, t, config ? config->sink : nullptr,
                          config ? config->sinkCtx : nullptr);
}

multitierP *multitierOpenP(persistCtx *ctx, multitierSinkFn *sink,
                           void *sinkCtx) {
    /* Replay's sweeps (with logged uses) give their rows to the caller's
     * own sink: logging them now would write into the WAL being read */
    multitierSetRecoverySink(sink, sinkCtx);
    multitier *t = persistCtxStartRecovered(ctx, &persistOpsMultitier);
    multitierSetRecoverySink(nullptr, nullptr);
    if (!t) {
        return nullptr;
    }

    return multitierWrapP(ctx, t, sink, sinkCtx);
}

void multitierCloseP(multitierP *p) {
    if (!p) {
        return;
    }

    /* A last snapshot holds the whole state (use history included), so a
     * clean restart is exact; then sync, and the context is free for
     * another wrapper */
    persistCtxCompact(p->ctx, p->t, &persistOpsMultitier);
    persistCtxFinish(p->ctx);
    multitierFree(p->t);
    zfree(p);
}

const multitier *multitierGetRawP(const multitierP *p) {
    return p ? p->t : nullptr;
}

size_t multitierCountP(const multitierP *p) {
    return p && p->t ? multitierCount(p->t) : 0;
}

/* ============================================================================
 * Changes
 * ============================================================================
 */

persistLogResult multitierSetP(multitierP *p, const databox *key,
                               const databox *value, bool *isNew) {
    if (isNew) {
        *isNew = false;
    }
    if (!p || !p->t || !key || !value) {
        return PERSIST_LOG_REFUSED;
    }

    const databox *row[2] = {key, value};
    const persistLogResult logged =
        persistCtxLogOp(p->ctx, PERSIST_OP_INSERT, row, 2);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }

    const bool added = multitierSet(p->t, key, value);
    if (isNew) {
        *isNew = added;
    }
    persistCtxMaybeCompact(p->ctx, p->t, &persistOpsMultitier);
    return logged;
}

persistLogResult multitierDeleteP(multitierP *p, const databox *key) {
    if (!p || !p->t || !key) {
        return PERSIST_LOG_REFUSED;
    }

    databox unused = {};
    if (!multitierPeek(p->t, key, &unused)) {
        return PERSIST_LOG_UNCHANGED;
    }

    const databox *args[1] = {key};
    const persistLogResult logged =
        persistCtxLogOp(p->ctx, PERSIST_OP_DELETE, args, 1);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }

    multitierDelete(p->t, key);
    persistCtxMaybeCompact(p->ctx, p->t, &persistOpsMultitier);
    return logged;
}

bool multitierGetP(multitierP *p, const databox *key, databox *value) {
    if (!p || !p->t || !key) {
        return false;
    }

    /* With logged uses a Get is logged first: a refused record reads the
     * value where it is (a Peek), moving nothing the log does not hold */
    multitierConfig config;
    multitierGetConfig(p->t, &config);
    if (config.logUses) {
        const databox *args[1] = {key};
        if (persistCtxLogOp(p->ctx, PERSIST_OP_USE, args, 1) ==
            PERSIST_LOG_REFUSED) {
            return multitierPeek(p->t, key, value);
        }
    }

    const bool found = multitierGet(p->t, key, value);
    /* A Get can push rows to the sink, which logs them */
    persistCtxMaybeCompact(p->ctx, p->t, &persistOpsMultitier);
    return found && multitierPeek(p->t, key, value); /* after compaction */
}

void multitierSetSinkP(multitierP *p, multitierSinkFn *sink, void *sinkCtx) {
    if (p) {
        p->sink = sink;
        p->sinkCtx = sinkCtx;
    }
}

uint64_t multitierSinkLogRefusedP(const multitierP *p) {
    return p ? p->sinkLogRefused : 0;
}

bool multitierPeekP(multitierP *p, const databox *key, databox *value) {
    return p && p->t && multitierPeek(p->t, key, value);
}

void multitierFlushP(multitierP *p) {
    if (p && p->t) {
        multitierConfig config;
        multitierGetConfig(p->t, &config);
        if (config.logUses && persistCtxLogOp(p->ctx, PERSIST_OP_FLUSH, nullptr,
                                              0) == PERSIST_LOG_REFUSED) {
            return; /* a refused record moves nothing */
        }
        multitierFlush(p->t);
        persistCtxMaybeCompact(p->ctx, p->t, &persistOpsMultitier);
    }
}

/* ============================================================================
 * Persistence control
 * ============================================================================
 */

bool multitierSyncP(multitierP *p) {
    return p && p->ctx && persistCtxSync(p->ctx);
}

bool multitierCompactP(multitierP *p) {
    return p && p->ctx && p->t &&
           persistCtxCompact(p->ctx, p->t, &persistOpsMultitier);
}

void multitierGetStatsP(const multitierP *p, persistCtxStats *stats) {
    if (p && p->ctx && stats) {
        persistCtxGetStats(p->ctx, stats);
    }
}

/* ============================================================================
 * Tests
 * ============================================================================
 */

#ifdef DATAKIT_TEST
#include "../multimap.h"
#include "../str.h"
#include "persistTestCommon.h"

defcleaner(multimap *, multimapFree);
defcleanerName(uint8_t *, zfree, TierSnap);

/* The model of the live rows: a sink takes its rows out of it */
static void multitierPTestSink(void *ctx, const flex *rows) {
    multimap **model = ctx;
    flexEntry *fe = flexHead(rows);
    for (size_t r = 0; r < flexCount(rows) / 2; r++) {
        databoxBig key;
        flexGetByTypeBig(fe, &key);
        multimapDelete(model, (const databox *)&key);
        fe = flexNext(rows, fe);
        fe = flexNext(rows, fe);
    }
}

/* A sink that, once armed, "dies" inside its call: it keeps the WAL file
 * as it stands (the op that pushed the rows out is logged, the rows' own
 * BULK_DELETE is not) and leaves the model as it was (the rows never
 * reached it).  Unarmed it takes the rows out of the model. */
typedef struct multitierPCrashSink {
    multimap **model;
    const char *walPath;
    bool armed;
    bool died;
    uint8_t *wal;
    size_t walLen;
} multitierPCrashSink;

static void multitierPCrashSinkFn(void *ctx, const flex *rows) {
    multitierPCrashSink *c = ctx;
    if (c->armed && !c->died) {
        c->died = true;
        FILE *fp = fopen(c->walPath, "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            const long size = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            c->walLen = size > 0 ? (size_t)size : 0;
            /* @own: container; free=zfree */
            c->wal = zmalloc(c->walLen ? c->walLen : 1);
            c->walLen = fread(c->wal, 1, c->walLen, fp);
            fclose(fp);
        }
        return;
    }
    if (!c->died) {
        multitierPTestSink(c->model, rows);
    }
}
/* Counts the rows a sink receives */
static void multitierPTestCountSink(void *ctx, const flex *rows) {
    *(size_t *)ctx += flexCount(rows) / 2;
}

static databox multitierPTestKey(char *buf, size_t len, uint64_t k) {
    const int n = snprintf(buf, len, "key:%06" PRIu64, k);
    return databoxNewBytes(buf, (size_t)n);
}

/* Every model row read back by Peek, and the counts agree */
static bool multitierPTestSame(multitierP *p, const multimap *model) {
    if (multitierCountP(p) != multimapCount(model)) {
        return false;
    }

    multimapIterator it;
    multimapIteratorInit(model, &it, true);
    databox k = {};
    databox v = {};
    databox *row[2] = {&k, &v};
    while (multimapIteratorNext(&it, row)) {
        databox got = {};
        if (!multitierPeekP(p, &k, &got) || databoxCompare(&got, &v)) {
            return false;
        }
    }
    return true;
}

int multitierPTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "multitierPTest", basePath, sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("every row survives a crash (no close) and a reopen, sunk rows "
         "stay gone, at every level") {
        ptestCleanupFiles(basePath);
        cleaner(multimapFree) multimap *model = multimapNew(2);
        const multitierConfig config = {
            .hotMaxBytes = 2048,
            .coldMaxBytes = 2048,
            .demoteBatch = 8,
            .cold = {.rawLimit = FLEX_CAP_LEVEL_512, .storedTarget = 128},
            .sink = multitierPTestSink,
            .sinkCtx = &model,
        };
        persistCtx *ctx = persistCtxNew(basePath, nullptr);
        multitierP *p = multitierNewP(ctx, &config);
        char key[32];
        char value[48];
        uint64_t rng = 0x71E5;
        for (size_t step = 0; step < 4000 && p; step++) {
            const uint64_t r = splitmix64(&rng);
            const databox k = multitierPTestKey(key, sizeof(key), r % 800);
            if (r % 10 < 6) {
                const int vn =
                    snprintf(value, sizeof(value), "{\"v\":%zu}", step);
                const databox v = databoxNewBytes(value, (size_t)vn);
                const databox *row[2] = {&k, &v};
                multimapInsert(&model, row);
                PTEST_LOGGED(multitierSetP(p, &k, &v, nullptr));
            } else if (r % 10 < 8) {
                databox got = {};
                (void)multitierGetP(p, &k, &got);
            } else {
                const bool had = multimapDelete(&model, &k);
                const persistLogResult got = multitierDeleteP(p, &k);
                if ((got == PERSIST_LOG_UNCHANGED) == had) {
                    ERR("step %zu: delete logged %d", step, got);
                }
            }
            if (step == 2000 && !multitierCompactP(p)) {
                ERR("%s", "compaction failed");
            }
        }

        multitierStats st = {};
        multitierGetStats(p->t, &st);
        if (!p || !multitierPTestSame(p, model)) {
            ERR("%s", "the live rows differ from the model");
        }
        if (!st.sunkRows || !st.demotions) {
            ERR("%" PRIu64 " rows sank, %" PRIu64
                " demoted; cold holds %" PRIu64 " stored bytes",
                st.sunkRows, st.demotions, st.coldStoredBytes);
        }

        /* A crash: the context goes without a close */
        persistCtxFree(ctx);
        if (p) {
            multitierFree(p->t);
            zfree(p);
        }

        ctx = persistCtxNew(basePath, nullptr);
        p = multitierOpenP(ctx, multitierPTestSink, &model);
        if (!p || !multitierPTestSame(p, model)) {
            ERR("%s", "the recovered multitier differs");
        }

        /* Writes after recovery log as before */
        const databox k = multitierPTestKey(key, sizeof(key), 999'999);
        const databox v = databoxNewBytes("after", 5);
        const databox *row[2] = {&k, &v};
        multimapInsert(&model, row);
        PTEST_LOGGED(multitierSetP(p, &k, &v, nullptr));
        multitierCloseP(p);
        persistCtxFree(ctx);

        ctx = persistCtxNew(basePath, nullptr);
        p = multitierOpenP(ctx, multitierPTestSink, &model);
        if (!p || !multitierPTestSame(p, model)) {
            ERR("%s", "the reopened multitier differs");
        }
        multitierCloseP(p);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("a sink set later receives the rows leaving; the counts, stats and "
         "a sync agree") {
        /* With no sink, rows leaving the cold level are dropped; after
         * SetSinkP the sink receives every row leaving, so the rows held
         * plus the rows received are the keys written since.  No WAL
         * record is refused.  No multitier: nothing to read or sync */
        ptestCleanupFiles(basePath);
        const multitierConfig config = {
            .hotMaxBytes = 1024,
            .coldMaxBytes = 1024,
            .demoteBatch = 8,
            .cold = {.rawLimit = FLEX_CAP_LEVEL_512, .storedTarget = 128},
        };
        persistCtx *ctx = persistCtxNew(basePath, nullptr);
        multitierP *p = multitierNewP(ctx, &config);
        char key[32];
        char value[64];
        memset(value, 'v', sizeof(value));
        const databox v = databoxNewBytes(value, sizeof(value));
        for (uint64_t i = 0; p && i < 400; i++) {
            const databox k = multitierPTestKey(key, sizeof(key), i);
            PTEST_LOGGED(multitierSetP(p, &k, &v, nullptr));
        }
        const size_t heldBefore = multitierCountP(p);
        size_t received = 0;
        multitierSetSinkP(p, multitierPTestCountSink, &received);
        for (uint64_t i = 400; p && i < 800; i++) {
            const databox k = multitierPTestKey(key, sizeof(key), i);
            PTEST_LOGGED(multitierSetP(p, &k, &v, nullptr));
        }
        persistCtxStats stats = {};
        multitierGetStatsP(p, &stats);
        const multitier *raw = multitierGetRawP(p);
        if (!p || heldBefore >= 400 || !received ||
            multitierCountP(p) + received != heldBefore + 400 || !raw ||
            multitierCount(raw) != multitierCountP(p) ||
            multitierSinkLogRefusedP(p) != 0 || stats.totalOps < 800 ||
            !multitierSyncP(p)) {
            ERR("held %zu then %zu, %zu received, %" PRIu64 " ops", heldBefore,
                multitierCountP(p), received, stats.totalOps);
        }
        persistCtxStats untouched = {.totalOps = 77};
        multitierGetStatsP(nullptr, &untouched);
        if (multitierGetRawP(nullptr) || multitierCountP(nullptr) ||
            multitierSinkLogRefusedP(nullptr) || multitierSyncP(nullptr) ||
            untouched.totalOps != 77) {
            ERR("%s", "a call on no multitier answered");
        }
        multitierCloseP(p);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("with logged uses, rows a crash kept from the sink reach the "
         "recovery sink: never lost") {
        ptestCleanupFiles(basePath);
        cleaner(multimapFree) multimap *model = multimapNew(2);
        char walPath[PATH_MAX + sizeof(".wal")];
        snprintf(walPath, sizeof(walPath), "%s.wal", basePath);
        multitierPCrashSink crash = {.model = &model, .walPath = walPath};
        const multitierConfig config = {
            .hotMaxBytes = 2048,
            .coldMaxBytes = 4096,
            .demoteBatch = 8,
            .logUses = true,
            .cold = {.rawLimit = FLEX_CAP_LEVEL_512, .storedTarget = 128},
            .sink = multitierPCrashSinkFn,
            .sinkCtx = &crash,
        };
        persistCtxConfig cc = persistCtxDefaultConfig();
        cc.syncMode = PERSIST_CTX_SYNC_ALWAYS;
        cc.walBufferSize = 0;
        cc.compactAfterOps = 0;
        cc.compactAfterBytes = 0;
        cc.compactRatio = 0;
        persistCtx *ctx = persistCtxNew(basePath, &cc);
        multitierP *p = multitierNewP(ctx, &config);
        char key[32];
        char value[48];
        for (uint64_t i = 0; i < 4000 && p && !crash.died; i++) {
            /* Arm once the cold level has started giving rows up */
            crash.armed = i >= 1500;
            const databox k = multitierPTestKey(key, sizeof(key), i);
            const int vn =
                snprintf(value, sizeof(value), "{\"v\":%" PRIu64 "}", i);
            const databox v = databoxNewBytes(value, (size_t)vn);
            const databox *row[2] = {&k, &v};
            multimapInsert(&model, row);
            PTEST_LOGGED(multitierSetP(p, &k, &v, nullptr));
        }
        if (!crash.died || !crash.wal) {
            ERR("%s", "the sink never died");
        }

        /* The crash: the files as they stood inside the sink */
        persistCtxFree(ctx);
        multitierFree(p->t);
        zfree(p);
        FILE *fp = fopen(walPath, "wb");
        if (fp) {
            (void)fwrite(crash.wal, 1, crash.walLen, fp);
            fclose(fp);
        }
        zfree(crash.wal);

        crash.armed = false;
        crash.died = false;
        ctx = persistCtxNew(basePath, &cc);
        p = multitierOpenP(ctx, multitierPCrashSinkFn, &crash);
        size_t lost = 0;
        multimapIterator it;
        databox mk = {};
        databox mv = {};
        databox *pair[2] = {&mk, &mv};
        multimapIteratorInit(model, &it, true);
        while (p && multimapIteratorNext(&it, pair)) {
            databox got = {};
            lost += !multitierPeekP(p, &mk, &got);
        }
        if (!p || lost) {
            ERR("%zu rows neither held nor delivered after the crash", lost);
        }
        multitierCloseP(p);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST("with logged uses a crash restores the exact state; without, a "
         "clean close does") {
        for (int logUses = 1; logUses >= 0; logUses--) {
            ptestCleanupFiles(basePath);
            cleaner(multimapFree) multimap *model = multimapNew(2);
            const multitierConfig config = {
                .hotMaxBytes = 2048,
                .coldMaxBytes = 4096,
                .demoteBatch = 8,
                .logUses = logUses,
                .cold = {.rawLimit = FLEX_CAP_LEVEL_512, .storedTarget = 128},
                .sink = multitierPTestSink,
                .sinkCtx = &model,
            };
            persistCtx *ctx = persistCtxNew(basePath, nullptr);
            multitierP *p = multitierNewP(ctx, &config);
            char key[32];
            char value[48];
            uint64_t rng = 0xE8AC7 + (uint64_t)logUses;
            for (size_t step = 0; step < 3000 && p; step++) {
                const uint64_t r = splitmix64(&rng);
                const databox k = multitierPTestKey(key, sizeof(key), r % 600);
                if (r % 10 < 5) {
                    const int vn =
                        snprintf(value, sizeof(value), "{\"v\":%zu}", step);
                    const databox v = databoxNewBytes(value, (size_t)vn);
                    PTEST_LOGGED(multitierSetP(p, &k, &v, nullptr));
                } else if (r % 10 < 9) {
                    databox got = {};
                    (void)multitierGetP(p, &k, &got);
                } else {
                    (void)multitierDeleteP(p, &k);
                }
                if (step == 1500) {
                    multitierFlushP(p);
                }
            }

            size_t wantLen = 0;
            cleanerName(zfree, TierSnap) uint8_t *want =
                persistOpsMultitier.snapshot(p->t, &wantLen);
            if (logUses) {
                /* A crash: the context goes without a close */
                persistCtxFree(ctx);
                multitierFree(p->t);
                zfree(p);
            } else {
                multitierCloseP(p);
                persistCtxFree(ctx);
            }

            ctx = persistCtxNew(basePath, nullptr);
            p = multitierOpenP(ctx, multitierPTestSink, &model);
            size_t gotLen = 0;
            cleanerName(zfree, TierSnap) uint8_t *got =
                p ? persistOpsMultitier.snapshot(p->t, &gotLen) : nullptr;
            if (!got || gotLen != wantLen || memcmp(got, want, wantLen) != 0) {
                ERR("logUses %d: the restored state differs (%zu bytes, live "
                    "%zu)",
                    logUses, gotLen, wantLen);
            }
            multitierCloseP(p);
            persistCtxFree(ctx);
        }
        ptestCleanupFiles(basePath);
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
