/* coldmapP.c - a coldmap whose changes are logged (see coldmapP.h) */

#include "coldmapP.h"
#include "../cleaner.h"
#include "../datakit.h"
#include "../persist.h"

struct coldmapP {
    coldmap *cm;
    persistCtx *ctx;
    mflexState *state; /* for the checks before a log */
};

defcleanerName(coldmapP *, zfree, ColdmapP);

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/* Wrap 'cm' (taken over): its starting snapshot records its config */
static coldmapP *coldmapStartP(persistCtx *ctx, coldmap *cm) {
    if (!ctx || !cm) {
        coldmapFree(cm);
        return nullptr;
    }

    if (!persistCtxStartNew(ctx, cm, &persistOpsColdmap)) {
        coldmapFree(cm);
        return nullptr;
    }

    /* @own: caller; free=coldmapCloseP */
    coldmapP *c = zcalloc(1, sizeof(*c));
    c->ctx = ctx;
    c->cm = cm;
    /* @own: container; free=coldmapCloseP */
    c->state = mflexStateCreate();
    return c;
}

coldmapP *coldmapNewP(persistCtx *ctx, multimapElements elementsPerEntry,
                      const coldmapConfig *config) {
    /* @own: container; free=coldmapFree */
    return coldmapStartP(ctx,
                         ctx ? coldmapNew(elementsPerEntry, config) : nullptr);
}

coldmapP *coldmapNewFromDumpP(persistCtx *ctx,
                              multimapElements elementsPerEntry,
                              const coldmapConfig *config, flex *rows) {
    if (!ctx) {
        flexFree(rows);
        return nullptr;
    }
    /* @own: container; free=coldmapFree */
    return coldmapStartP(ctx,
                         coldmapNewFromDump(elementsPerEntry, config, rows));
}

coldmapP *coldmapOpenP(persistCtx *ctx) {
    coldmap *recovered = persistCtxStartRecovered(ctx, &persistOpsColdmap);
    if (!recovered) {
        return nullptr;
    }

    /* @own: caller; free=coldmapCloseP */
    coldmapP *c = zcalloc(1, sizeof(*c));
    c->ctx = ctx;
    c->cm = recovered;
    /* @own: container; free=coldmapCloseP */
    c->state = mflexStateCreate();
    return c;
}

void coldmapCloseP(coldmapP *c) {
    if (!c) {
        return;
    }

    /* Sync before closing; the context is free for another wrapper */
    persistCtxFinish(c->ctx);
    coldmapFree(c->cm);
    mflexStateFree(c->state);
    zfree(c);
}

const coldmap *coldmapGetRawP(const coldmapP *c) {
    return c ? c->cm : nullptr;
}

size_t coldmapCountP(const coldmapP *c) {
    return c && c->cm ? coldmapCount(c->cm) : 0;
}

/* ============================================================================
 * Changes: each logged first, so a refused log changes nothing
 * ============================================================================
 */

/* A change logged: compaction may follow */
static persistLogResult coldmapLoggedP(coldmapP *c, persistLogResult logged) {
    if (logged != PERSIST_LOG_REFUSED) {
        persistCtxMaybeCompact(c->ctx, c->cm, &persistOpsColdmap);
    }
    return logged;
}

persistLogResult coldmapInsertP(coldmapP *c, const databox *elements[],
                                bool *replaced) {
    if (replaced) {
        *replaced = false;
    }
    if (!c || !c->cm || !elements) {
        return PERSIST_LOG_REFUSED;
    }

    const persistLogResult logged = persistCtxLogOp(
        c->ctx, PERSIST_OP_INSERT, elements, coldmapElementsPerEntry(c->cm));
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }

    const bool was = coldmapInsert(c->cm, elements);
    if (replaced) {
        *replaced = was;
    }
    return coldmapLoggedP(c, logged);
}

persistLogResult coldmapDeleteP(coldmapP *c, const databox *key) {
    if (!c || !c->cm || !key) {
        return PERSIST_LOG_REFUSED;
    }
    if (!coldmapExists(c->cm, c->state, key)) {
        return PERSIST_LOG_UNCHANGED;
    }

    const databox *args[1] = {key};
    const persistLogResult logged =
        persistCtxLogOp(c->ctx, PERSIST_OP_DELETE, args, 1);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }

    coldmapDelete(c->cm, key);
    return coldmapLoggedP(c, logged);
}

persistLogResult coldmapInsertManyP(coldmapP *c, const flex *rows) {
    if (!c || !c->cm || !rows ||
        !coldmapRowsAscend(rows, coldmapElementsPerEntry(c->cm))) {
        return PERSIST_LOG_REFUSED;
    }
    if (!flexCount(rows)) {
        return PERSIST_LOG_UNCHANGED;
    }

    const persistLogResult logged =
        persistCtxLogOp(c->ctx, PERSIST_OP_BULK_INSERT, rows, 1);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }

    coldmapInsertMany(c->cm, rows, nullptr);
    return coldmapLoggedP(c, logged);
}

persistLogResult coldmapDeleteRangeP(coldmapP *c, const databox *lo,
                                     const databox *hi, size_t *deleted) {
    if (deleted) {
        *deleted = 0;
    }
    if (!c || !c->cm) {
        return PERSIST_LOG_REFUSED;
    }
    if (!coldmapCount(c->cm) || (lo && hi && databoxCompare(lo, hi) > 0)) {
        return PERSIST_LOG_UNCHANGED;
    }

    const databox *bounds[2] = {lo, hi};
    const persistLogResult logged =
        persistCtxLogOp(c->ctx, PERSIST_OP_BULK_DELETE, bounds, 2);
    if (logged == PERSIST_LOG_REFUSED) {
        return logged;
    }

    const size_t n = coldmapDeleteRange(c->cm, lo, hi);
    if (deleted) {
        *deleted = n;
    }
    return coldmapLoggedP(c, logged);
}

/* ============================================================================
 * Persistence control
 * ============================================================================
 */

bool coldmapSyncP(coldmapP *c) {
    return c && c->ctx && persistCtxSync(c->ctx);
}

bool coldmapCompactP(coldmapP *c) {
    return c && c->ctx && c->cm &&
           persistCtxCompact(c->ctx, c->cm, &persistOpsColdmap);
}

void coldmapGetStatsP(const coldmapP *c, persistCtxStats *stats) {
    if (c && c->ctx && stats) {
        persistCtxGetStats(c->ctx, stats);
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
defcleaner(flex *, flexFree);
defcleaner(mflexState *, mflexStateFree);

/* Key number 'k' as "key:NNNNNN" into 'buf' */
static databox coldmapPTestKey(char *buf, size_t len, uint64_t k) {
    const int n = snprintf(buf, len, "key:%06" PRIu64, k);
    return databoxNewBytes(buf, (size_t)n);
}

/* Whether the persistent coldmap holds exactly the multimap's rows */
static bool coldmapPTestSame(const coldmapP *c, const multimap *m) {
    cleaner(mflexStateFree) mflexState *state = mflexStateCreate();
    cleaner(flexFree) flex *want = multimapDump(m);
    cleaner(flexFree) flex *got = coldmapDump(coldmapGetRawP(c), state);
    return flexEqual(want, got);
}

int coldmapPTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;
    cleaner(dkTestTmpDirRemove) dkTestTmpDir tmp = {};
    char basePath[PATH_MAX];
    if (!dkTestTmpDirPath(&tmp, "coldmapPTest", basePath, sizeof(basePath))) {
        ERR("could not create a scratch directory: %s", strerror(errno));
        return err;
    }

    TEST("every change survives a crash (no close), a reopen and a "
         "compaction, agreeing with a multimap") {
        ptestCleanupFiles(basePath);
        const coldmapConfig small = {.rawLimit = FLEX_CAP_LEVEL_512,
                                     .storedTarget = 128};
        persistCtx *ctx = persistCtxNew(basePath, nullptr);
        coldmapP *c = coldmapNewP(ctx, 2, &small);
        cleaner(multimapFree) multimap *m = multimapNew(2);
        char key[32];
        char hiKey[32];
        char value[48];
        uint64_t rng = 0xC01D;
        for (size_t step = 0; step < 3000 && c; step++) {
            const uint64_t r = splitmix64(&rng);
            const databox k = coldmapPTestKey(key, sizeof(key), r % 900);
            if (r % 10 < 7) {
                const int vn =
                    snprintf(value, sizeof(value), "{\"v\":%zu}", step);
                const databox v = databoxNewBytes(value, (size_t)vn);
                const databox *row[2] = {&k, &v};
                multimapInsert(&m, row);
                PTEST_LOGGED(coldmapInsertP(c, row, nullptr));
            } else if (r % 10 < 9) {
                const bool had = multimapDelete(&m, &k);
                const persistLogResult got = coldmapDeleteP(c, &k);
                const bool logged =
                    got != PERSIST_LOG_REFUSED && got != PERSIST_LOG_UNCHANGED;
                if (logged != had || (!had && got != PERSIST_LOG_UNCHANGED)) {
                    ERR("step %zu: delete logged %d", step, got);
                }
            } else {
                const databox hi = coldmapPTestKey(hiKey, sizeof(hiKey),
                                                   r % 900 + (r >> 40) % 30);
                const size_t want = multimapDeleteRange(&m, &k, &hi);
                size_t deleted = 0;
                PTEST_LOGGED(coldmapDeleteRangeP(c, &k, &hi, &deleted));
                if (deleted != want) {
                    ERR("step %zu: range deleted %zu, want %zu", step, deleted,
                        want);
                }
            }

            if (step == 1500) {
                /* A sorted batch, then a snapshot mid-stream */
                cleaner(multimapFree) multimap *batch = multimapNew(2);
                for (uint64_t i = 0; i < 40; i++) {
                    const databox bk =
                        coldmapPTestKey(key, sizeof(key), 2000 + i * 3);
                    const databox *row[2] = {&bk, &bk};
                    multimapInsert(&batch, row);
                    multimapInsert(&m, row);
                }
                cleaner(flexFree) flex *dump = multimapDump(batch);
                PTEST_LOGGED(coldmapInsertManyP(c, dump));
                if (!coldmapCompactP(c)) {
                    ERR("%s", "compaction failed");
                }
            }
        }

        if (!c || !coldmapPTestSame(c, m)) {
            ERR("%s", "the live coldmap differs from the multimap");
        }

        /* A crash: the context goes without a close */
        persistCtxFree(ctx);
        if (c) {
            coldmapFree((coldmap *)coldmapGetRawP(c));
            mflexStateFree(c->state);
            zfree(c);
        }

        ctx = persistCtxNew(basePath, nullptr);
        c = coldmapOpenP(ctx);
        coldmapConfig config = {};
        if (c) {
            coldmapGetConfig(coldmapGetRawP(c), &config);
        }
        if (!c || !coldmapPTestSame(c, m) ||
            config.rawLimit != FLEX_CAP_LEVEL_512 ||
            config.storedTarget != 128) {
            ERR("%s", "the recovered coldmap differs");
        }

        /* Changes that change nothing log nothing; unsorted batches are
         * refused */
        const databox absent = coldmapPTestKey(key, sizeof(key), 999'999);
        const databox one = databoxNewSigned(1);
        cleaner(flexFree) flex *unsorted = flexNew();
        for (int i = 0; i < 2; i++) {
            flexPushByTypeExact(&unsorted, &absent, FLEX_ENDPOINT_TAIL);
            flexPushByTypeExact(&unsorted, &one, FLEX_ENDPOINT_TAIL);
        }
        if (coldmapDeleteP(c, &absent) != PERSIST_LOG_UNCHANGED ||
            coldmapInsertManyP(c, unsorted) != PERSIST_LOG_REFUSED) {
            ERR("%s", "a no-op was logged or unsorted rows were taken");
        }

        coldmapCloseP(c);
        persistCtxFree(ctx);
        ptestCleanupFiles(basePath);
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
