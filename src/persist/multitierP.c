/* multitierP.c - a multitier whose changes are logged (see multitierP.h) */

#include "multitierP.h"
#include "../cleaner.h"
#include "../datakit.h"
#include "../persist.h"

struct multitierP {
    multitier *t;
    persistCtx *ctx;
    multitierSinkFn *sink; /* the caller's, after the log */
    void *sinkCtx;
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

    /* The rows are gone either way: a refused log leaves them in the
     * files, and they come back on the next open */
    if (n) {
        (void)persistCtxLogOp(p->ctx, PERSIST_OP_BULK_DELETE, args, n);
    }
    if (p->sink) {
        p->sink(p->sinkCtx, rows);
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
    multitier *t = persistCtxStartRecovered(ctx, &persistOpsMultitier);
    if (!t) {
        return nullptr;
    }

    return multitierWrapP(ctx, t, sink, sinkCtx);
}

void multitierCloseP(multitierP *p) {
    if (!p) {
        return;
    }

    /* Sync before closing; the context is free for another wrapper */
    persistCtxFinish(p->ctx);
    multitierFree(p->t);
    zfree(p);
}

multitier *multitierGetRawP(multitierP *p) {
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
                               const databox *value) {
    if (!p || !p->t || !key || !value) {
        return PERSIST_LOG_REFUSED;
    }

    const databox *row[2] = {key, value};
    const persistLogResult logged =
        persistCtxLogOp(p->ctx, PERSIST_OP_INSERT, row, 2);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }

    multitierSet(p->t, key, value);
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
    return p && p->t && multitierGet(p->t, key, value);
}

bool multitierPeekP(multitierP *p, const databox *key, databox *value) {
    return p && p->t && multitierPeek(p->t, key, value);
}

void multitierFlushP(multitierP *p) {
    if (p && p->t) {
        multitierFlush(p->t);
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
                PTEST_LOGGED(multitierSetP(p, &k, &v));
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
        multitierGetStats(multitierGetRawP(p), &st);
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
            multitierFree(multitierGetRawP(p));
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
        PTEST_LOGGED(multitierSetP(p, &k, &v));
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

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
