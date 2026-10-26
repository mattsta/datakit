/* Tier simulation: one driver for every tiered container family.
 *
 * Each family plugs in through a small adapter (simOps) that maps an
 * ordered key -> value view onto its API.  The driver runs the same
 * scripted phases against every adapter, checks every operation against
 * a shadow model (ascending unique keys with their values), and records
 * every tier transition the container makes:
 *
 *   grow      ascending fill through Small -> Medium -> Full
 *   churn     random puts, deletes and lookups at a steady population
 *   walks     chunked ascending walks that resume by key after other
 *             mutations, and lookups that start in the gaps between keys
 *   drains    scattered deletes, then deletes from the front, then from
 *             the back, each down to a handful of rows
 *   oscillate inserts and deletes just across each tier's growth point
 *   bulk      range removals (where the family has them)
 *   regrow    growth again after each drain
 *
 * Checked after every operation: count, the touched key's lookup, and the
 * container's dkTierStats (count, units, bytes).  Every 97 operations: a
 * full ascending walk (and descending, where the family has one) equals
 * the shadow, and the family's own verifier runs.  In Full, the inner
 * units must stay within a density bound of the data they hold (for
 * families that coalesce sparse units).  At the end, the transition
 * histogram must show every growth transition, and shrink transitions
 * exactly when the family shrinks.
 *
 * Keys are even numbers, so every odd number is a gap. */

#include "tierSim.h"

#ifdef DATAKIT_TEST
#include "ctest.h"
#include "datakit.h"
#include "intset.h"
#include "mflex.h"
#include "multiOrderedSet.h"
#include "multiarray.h"
#include "multilist.h"
#include "multimap.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum { SIM_CAP = 8192, SIM_KEYSPACE = 2 * SIM_CAP + 64, SIM_CHUNK = 16 };

/* ====================================================================
 * Adapters
 * ==================================================================== */
typedef struct simOps {
    const char *name;
    bool valueIsKey; /* a set: the value read back is the key itself */
    bool shrinks;    /* tiers move down as deletes empty them */
    bool coalesces;  /* Full merges sparse units (density bound applies) */
    int32_t growTo;  /* rows the growth phase reaches */
    void *(*make)(void);
    void (*destroy)(void *c);
    void (*put)(void **c, int64_t k, int64_t v); /* insert or replace */
    bool (*del)(void **c, int64_t k);
    bool (*get)(void *c, int64_t k, int64_t *v);
    void (*stats)(void *c, dkTierStats *s);
    /* Ascending rows from the first key >= from; returns rows written */
    size_t (*scan)(void *c, int64_t from, int64_t *k, int64_t *v, size_t max);
    /* Every row descending; NULL: no reverse walk */
    size_t (*scanBack)(void *c, int64_t *k, int64_t *v, size_t max);
    /* Remove every key <= bound; returns rows removed; NULL: none */
    size_t (*delUpTo)(void **c, int64_t bound);
    void (*verify)(void *c); /* the family's own checker; NULL: none */
} simOps;

/* ---- multimap: a map of two-element rows ---- */
static void *mmMake(void) {
    /* @own: caller; free=multimapFree */
    return multimapNewLimit(2, FLEX_CAP_LEVEL_64);
}

static void mmDestroy(void *c) {
    multimapFree(c);
}

static void mmPut(void **c, int64_t k, int64_t v) {
    const databox kb = databoxNewSigned(k);
    const databox vb = databoxNewSigned(v);
    const databox *row[2] = {&kb, &vb};
    multimapInsert((multimap **)c, row);
}

static bool mmDel(void **c, int64_t k) {
    const databox kb = databoxNewSigned(k);
    return multimapDelete((multimap **)c, &kb);
}

static bool mmGet(void *c, int64_t k, int64_t *v) {
    const databox kb = databoxNewSigned(k);
    databox out[1];
    if (!multimapLookupInto(c, &kb, out, 1)) {
        return false;
    }
    *v = out[0].data.i;
    return true;
}

static void mmStats(void *c, dkTierStats *s) {
    multimapStats(c, s);
}

static size_t mmWalk(multimapIterator *it, int64_t *k, int64_t *v, size_t max) {
    size_t n = 0;
    databox row[2];
    while (n < max && multimapIteratorNextInto(it, row, 2)) {
        k[n] = row[0].data.i;
        v[n] = row[1].data.i;
        n++;
    }
    return n;
}

static size_t mmScan(void *c, int64_t from, int64_t *k, int64_t *v,
                     size_t max) {
    multimapIterator it;
    const databox fb = databoxNewSigned(from);
    if (!multimapIteratorInitAt(c, &it, true, &fb)) {
        return 0;
    }
    return mmWalk(&it, k, v, max);
}

static size_t mmScanBack(void *c, int64_t *k, int64_t *v, size_t max) {
    multimapIterator it;
    multimapIteratorInit(c, &it, false);
    return mmWalk(&it, k, v, max);
}

static size_t mmDelUpTo(void **c, int64_t bound) {
    const size_t before = multimapCount(*c);
    const multimapPredicate p = {.condition =
                                     MULTIMAP_CONDITION_LESS_THAN_EQUAL,
                                 .compareAgainst = databoxNewSigned(bound)};
    multimapDeleteByPredicate((multimap **)c, &p);
    return before - multimapCount(*c);
}

static void mmVerify(void *c) {
    multimapVerify(c);
}

/* ---- multiOrderedSet: member and score are both the key ---- */
static int64_t simI64(const databox *b) {
    return b->type == DATABOX_DOUBLE_64 ? (int64_t)b->data.d64 : b->data.i;
}

static void *mosMake(void) {
    /* @own: caller; free=multiOrderedSetFree */
    return multiOrderedSetNewLimit(FLEX_CAP_LEVEL_64);
}

static void mosDestroy(void *c) {
    multiOrderedSetFree(c);
}

static void mosPut(void **c, int64_t k, int64_t) {
    const databox b = databoxNewSigned(k);
    multiOrderedSetAdd((multiOrderedSet **)c, &b, &b);
}

static bool mosDel(void **c, int64_t k) {
    const databox b = databoxNewSigned(k);
    return multiOrderedSetRemove((multiOrderedSet **)c, &b);
}

static bool mosGet(void *c, int64_t k, int64_t *v) {
    const databox b = databoxNewSigned(k);
    databox score = {};
    if (!multiOrderedSetGetScore(c, &b, &score)) {
        return false;
    }
    *v = simI64(&score);
    return true;
}

static void mosStats(void *c, dkTierStats *s) {
    multiOrderedSetStats(c, s);
}

static size_t mosWalk(mosIterator *it, int64_t *k, int64_t *v, size_t max) {
    size_t n = 0;
    databox member = {};
    databox score = {};
    while (n < max && multiOrderedSetIteratorNext(it, &member, &score)) {
        k[n] = simI64(&member);
        v[n] = simI64(&score);
        n++;
    }
    multiOrderedSetIteratorRelease(it);
    return n;
}

static size_t mosScan(void *c, int64_t from, int64_t *k, int64_t *v,
                      size_t max) {
    mosIterator it;
    const databox fb = databoxNewSigned(from);
    if (!multiOrderedSetIteratorInitAtScore(c, &it, &fb, true)) {
        return 0;
    }
    return mosWalk(&it, k, v, max);
}

static size_t mosScanBack(void *c, int64_t *k, int64_t *v, size_t max) {
    mosIterator it;
    multiOrderedSetIteratorInit(c, &it, false);
    return mosWalk(&it, k, v, max);
}

static void mosVerify(void *c) {
    multiOrderedSetVerify(c);
}

static size_t mosDelUpTo(void **c, int64_t bound) {
    const mosRangeSpec range = {.min = databoxNewSigned(INT64_MIN),
                                .max = databoxNewSigned(bound)};
    return multiOrderedSetRemoveRangeByScore((multiOrderedSet **)c, &range);
}

/* ---- intset: keys spread over 16, 32 and 64-bit values in order, so
 * growth crosses both width boundaries ---- */
static int64_t isEncode(int64_t k) {
    if (k < 4096) {
        return k;
    }
    if (k < 8192) {
        return k * 16; /* 65536..131056: int32 */
    }
    return k * ((int64_t)1 << 40); /* int64 */
}

static int64_t isDecode(int64_t v) {
    if (v < 4096) {
        return v;
    }
    if (v < 8192 * 16) {
        return v / 16;
    }
    return v / ((int64_t)1 << 40);
}

static void *isMake(void) {
    /* @own: caller; free=intsetFree */
    return intsetNew();
}

static void isDestroy(void *c) {
    intsetFree(c);
}

static void isPut(void **c, int64_t k, int64_t) {
    intsetAdd((intset **)c, isEncode(k), NULL);
}

static bool isDel(void **c, int64_t k) {
    bool removed = false;
    intsetRemove((intset **)c, isEncode(k), &removed);
    return removed;
}

static bool isGet(void *c, int64_t k, int64_t *v) {
    *v = k;
    return intsetFind(c, isEncode(k));
}

static void isStats(void *c, dkTierStats *s) {
    intsetStats(c, s);
}

static size_t isScan(void *c, int64_t from, int64_t *k, int64_t *v,
                     size_t max) {
    intsetIterator it;
    intsetIteratorInit(&it, c);
    size_t n = 0;
    int64_t value;
    while (n < max && intsetIteratorNext(&it, &value)) {
        const int64_t key = isDecode(value);
        if (key >= from) {
            k[n] = key;
            v[n] = key;
            n++;
        }
    }
    return n;
}

static const simOps simFamilies[] = {
    {.name = "multimap",
     .shrinks = true,
     .coalesces = true,
     .growTo = 2000,
     .make = mmMake,
     .destroy = mmDestroy,
     .put = mmPut,
     .del = mmDel,
     .get = mmGet,
     .stats = mmStats,
     .scan = mmScan,
     .scanBack = mmScanBack,
     .delUpTo = mmDelUpTo,
     .verify = mmVerify},
    {.name = "multiOrderedSet",
     .valueIsKey = true,
     .shrinks = true,
     .coalesces = true,
     .growTo = 2000,
     .make = mosMake,
     .destroy = mosDestroy,
     .put = mosPut,
     .del = mosDel,
     .get = mosGet,
     .stats = mosStats,
     .scan = mosScan,
     .scanBack = mosScanBack,
     .delUpTo = mosDelUpTo,
     .verify = mosVerify},
    {.name = "intset",
     .valueIsKey = true,
     .growTo = 4400,
     .make = isMake,
     .destroy = isDestroy,
     .put = isPut,
     .del = isDel,
     .get = isGet,
     .stats = isStats,
     .scan = isScan},
};

/* ====================================================================
 * The run: the container, its shadow, and what the run has seen
 * ==================================================================== */
typedef struct simRun {
    const simOps *ops; /* ordered families (NULL for a sequence run) */
    const char *name;
    bool shrinks;
    bool coalesces;
    void *c;
    uint64_t rng;
    uint64_t opsDone;
    int64_t key[SIM_CAP]; /* the shadow: ascending unique keys */
    int64_t val[SIM_CAP];
    int32_t n;
    uint8_t tier;
    uint64_t trans[4][4]; /* [from][to] tier transitions */
    int32_t growPoint[4]; /* rows when growth first reached each tier */
    uint32_t maxUnits;
    double worstDensity; /* Full units per ideal unit count */
    uint32_t worstFlap;  /* unit-count changes in one probe's 20 ops */
    int32_t err;
    /* scratch for walks */
    int64_t wk[SIM_CAP];
    int64_t wv[SIM_CAP];
    uint8_t startPresent[SIM_KEYSPACE];
    uint8_t touched[SIM_KEYSPACE];
    uint8_t everPresent[SIM_KEYSPACE];
    uint8_t seen[SIM_KEYSPACE];
} simRun;

static simRun simState;

#define SIM_ERR(r, ...)                                                        \
    do {                                                                       \
        if ((r)->err < 8) {                                                    \
            printf("    %s op %" PRIu64 ": ", (r)->name, (r)->opsDone);        \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
        (r)->err++;                                                            \
    } while (0)

static uint64_t simNext(simRun *r) {
    r->rng ^= r->rng << 13;
    r->rng ^= r->rng >> 7;
    r->rng ^= r->rng << 17;
    return r->rng;
}

/* Lower bound: the index of the first shadow key >= k */
static int32_t simLower(const simRun *r, int64_t k) {
    int32_t lo = 0;
    int32_t hi = r->n;
    while (lo < hi) {
        const int32_t mid = lo + (hi - lo) / 2;
        if (r->key[mid] < k) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static bool simHas(const simRun *r, int64_t k, int32_t *at) {
    *at = simLower(r, k);
    return *at < r->n && r->key[*at] == k;
}

/* ---- checks ---- */
static void simCheckAll(simRun *r) {
    const size_t got = r->ops->scan(r->c, INT64_MIN, r->wk, r->wv, SIM_CAP);
    if (got != (size_t)r->n) {
        SIM_ERR(r, "full walk %zu rows, shadow %d", got, r->n);
    }
    for (size_t i = 0; i < got && i < (size_t)r->n; i++) {
        if (r->wk[i] != r->key[i] || r->wv[i] != r->val[i]) {
            SIM_ERR(r,
                    "walk row %zu (%" PRId64 ", %" PRId64
                    ") != shadow (%" PRId64 ", %" PRId64 ")",
                    i, r->wk[i], r->wv[i], r->key[i], r->val[i]);
            break;
        }
    }
    if (r->ops->scanBack) {
        const size_t back = r->ops->scanBack(r->c, r->wk, r->wv, SIM_CAP);
        if (back != (size_t)r->n) {
            SIM_ERR(r, "reverse walk %zu rows, shadow %d", back, r->n);
        }
        for (size_t i = 0; i < back && i < (size_t)r->n; i++) {
            if (r->wk[i] != r->key[r->n - 1 - (int32_t)i]) {
                SIM_ERR(r, "reverse row %zu key %" PRId64 " != shadow %" PRId64,
                        i, r->wk[i], r->key[r->n - 1 - (int32_t)i]);
                break;
            }
        }
    }
    if (r->ops->verify) {
        r->ops->verify(r->c);
    }
}

/* What every operation checks of a container's stats, ordered or
 * sequence: the count against the shadow, sane units and bytes, the tier
 * transition it made, and Full's units against the fewest that could hold
 * its data (merging neighbours that fit keeps every pair above a unit) */
static void simTrackStats(simRun *r, const dkTierStats *s) {
    if (s->count != (uint64_t)r->n) {
        SIM_ERR(r, "stats count %" PRIu64 ", shadow %d", s->count, r->n);
    }
    if (s->totalBytes < s->dataBytes || (r->n && s->units == 0)) {
        SIM_ERR(r, "stats units %u data %" PRIu64 " total %" PRIu64, s->units,
                s->dataBytes, s->totalBytes);
    }
    if (s->tier != r->tier) {
        r->trans[r->tier & 3][s->tier & 3]++;
        if (s->tier > r->tier && !r->growPoint[s->tier & 3]) {
            r->growPoint[s->tier & 3] = r->n;
        }
        r->tier = s->tier;
    }
    if (s->units > r->maxUnits) {
        r->maxUnits = s->units;
    }

    if (s->tier == 3 && s->units > 1 && s->unitLimit) {
        const uint64_t ideal = (s->dataBytes + s->unitLimit - 1) / s->unitLimit;
        const double ratio = (double)s->units / (double)(ideal ? ideal : 1);
        if (ratio > r->worstDensity) {
            r->worstDensity = ratio;
        }
        if (r->coalesces && s->units > 4 * ideal + 2) {
            SIM_ERR(r,
                    "Full holds %u units for %" PRIu64 " data bytes (limit %u)",
                    s->units, s->dataBytes, s->unitLimit);
        }
    }
}

static void simCheckOp(simRun *r, int64_t k) {
    dkTierStats s;
    r->ops->stats(r->c, &s);
    simTrackStats(r, &s);

    int32_t at;
    int64_t v = 0;
    const bool want = simHas(r, k, &at);
    const bool got = r->ops->get(r->c, k, &v);
    if (got != want || (got && v != r->val[at])) {
        SIM_ERR(r, "get %" PRId64 ": %d (%" PRId64 "), shadow %d (%" PRId64 ")",
                k, got, v, want, want ? r->val[at] : 0);
    }

    if (++r->opsDone % 97 == 0) {
        simCheckAll(r);
    }
}

/* ---- operations: container and shadow together ---- */
static void simPut(simRun *r, int64_t k) {
    const int64_t v = r->ops->valueIsKey ? k : (int64_t)(simNext(r) >> 20);
    int32_t at;
    if (simHas(r, k, &at)) {
        r->val[at] = v;
    } else {
        if (r->n == SIM_CAP) {
            return;
        }
        memmove(&r->key[at + 1], &r->key[at],
                (size_t)(r->n - at) * sizeof(r->key[0]));
        memmove(&r->val[at + 1], &r->val[at],
                (size_t)(r->n - at) * sizeof(r->val[0]));
        r->key[at] = k;
        r->val[at] = v;
        r->n++;
    }
    r->ops->put(&r->c, k, v);
    simCheckOp(r, k);
}

static void simDelAt(simRun *r, int32_t at) {
    const int64_t k = r->key[at];
    memmove(&r->key[at], &r->key[at + 1],
            (size_t)(r->n - at - 1) * sizeof(r->key[0]));
    memmove(&r->val[at], &r->val[at + 1],
            (size_t)(r->n - at - 1) * sizeof(r->val[0]));
    r->n--;
    if (!r->ops->del(&r->c, k)) {
        SIM_ERR(r, "delete %" PRId64 " found nothing", k);
    }
    simCheckOp(r, k);
}

static void simDel(simRun *r, int64_t k) {
    int32_t at;
    if (simHas(r, k, &at)) {
        simDelAt(r, at);
    } else {
        if (r->ops->del(&r->c, k)) {
            SIM_ERR(r, "delete of absent %" PRId64 " removed something", k);
        }
        simCheckOp(r, k);
    }
}

static int64_t simRandomKey(simRun *r) {
    return (int64_t)(simNext(r) % SIM_CAP) * 2;
}

/* A random mutation during a walk, remembered by key */
static void simWalkMutate(simRun *r) {
    const int64_t k = simRandomKey(r);
    r->touched[k] = 1;
    if (simNext(r) & 1) {
        r->everPresent[k] = 1;
        simPut(r, k);
    } else {
        simDel(r, k);
    }
}

/* Walk ascending SIM_CHUNK rows at a time, resuming from the key after
 * the last one seen (an odd key: a gap) after a few mutations.  Rows
 * present at the start and never touched must each appear exactly once
 * with their values; nothing appears twice, out of order, or that was
 * never present during the walk. */
static void simWalkWhileChanging(simRun *r) {
    memset(r->startPresent, 0, sizeof(r->startPresent));
    memset(r->touched, 0, sizeof(r->touched));
    memset(r->seen, 0, sizeof(r->seen));
    for (int32_t i = 0; i < r->n; i++) {
        r->startPresent[r->key[i]] = 1;
    }
    memcpy(r->everPresent, r->startPresent, sizeof(r->everPresent));

    int64_t from = INT64_MIN;
    int64_t last = INT64_MIN;
    int64_t k[SIM_CHUNK];
    int64_t v[SIM_CHUNK];
    for (;;) {
        const size_t got = r->ops->scan(r->c, from, k, v, SIM_CHUNK);
        for (size_t i = 0; i < got; i++) {
            if (k[i] <= last || k[i] < 0 || k[i] >= SIM_KEYSPACE) {
                SIM_ERR(r, "walk key %" PRId64 " after %" PRId64, k[i], last);
                return;
            }
            last = k[i];
            if (!r->everPresent[k[i]] || r->seen[k[i]]++) {
                SIM_ERR(r, "walk saw %" PRId64 " (present %d, seen %d)", k[i],
                        r->everPresent[k[i]], r->seen[k[i]]);
            }
            int32_t at;
            if (!r->touched[k[i]] &&
                (!simHas(r, k[i], &at) || v[i] != r->val[at])) {
                SIM_ERR(r, "walk value of untouched %" PRId64, k[i]);
            }
        }
        if (got < SIM_CHUNK) {
            break;
        }
        from = last + 1;
        for (int32_t m = 0; m < 4; m++) {
            simWalkMutate(r);
        }
    }

    for (int64_t key = 0; key < SIM_KEYSPACE; key++) {
        if (r->startPresent[key] && !r->touched[key] && r->seen[key] != 1) {
            SIM_ERR(r, "walk missed untouched %" PRId64, key);
            break;
        }
    }
}

/* Walks that start in gaps, before the first key and past the last */
static void simGapProbes(simRun *r) {
    for (int32_t p = 0; p < 8; p++) {
        int64_t from = (int64_t)(simNext(r) % (SIM_KEYSPACE + 4)) - 2;
        from |= 1; /* odd: never a key */
        int64_t k;
        int64_t v;
        const size_t got = r->ops->scan(r->c, from, &k, &v, 1);
        const int32_t at = simLower(r, from);
        if (got != (at < r->n ? 1u : 0u) || (got && k != r->key[at])) {
            SIM_ERR(r,
                    "walk from gap %" PRId64 ": got %zu (%" PRId64
                    "), shadow %" PRId64,
                    from, got, got ? k : 0, at < r->n ? r->key[at] : -1);
        }
    }
}

/* ====================================================================
 * Phases
 * ==================================================================== */
static void simGrow(simRun *r, int32_t rows) {
    for (int32_t i = r->n; i < rows; i++) {
        simPut(r, (int64_t)i * 2);
    }
}

/* Delete one key and put it back, ten times, for 32 keys.  A merge on the
 * first delete and a split on its insert may happen once; a unit count
 * that changes on most of the 20 operations is a merge every delete
 * undoes with a split on the next insert (each moving a unit's bytes). */
static void simFlapProbe(simRun *r) {
    if (r->tier != 3 || r->n < 2) {
        return;
    }
    for (int32_t p = 0; p < 32; p++) {
        const int64_t k = r->key[(int32_t)(simNext(r) % (uint64_t)r->n)];
        dkTierStats s;
        r->ops->stats(r->c, &s);
        uint32_t units = s.units;
        uint32_t changes = 0;
        for (int32_t cycle = 0; cycle < 10; cycle++) {
            simDel(r, k);
            r->ops->stats(r->c, &s);
            changes += s.units != units;
            units = s.units;
            simPut(r, k);
            r->ops->stats(r->c, &s);
            changes += s.units != units;
            units = s.units;
        }
        if (changes > r->worstFlap) {
            r->worstFlap = changes;
        }
    }
    if (r->coalesces && r->worstFlap > 4) {
        SIM_ERR(r, "units changed %u times in 20 delete/insert ops of one key",
                r->worstFlap);
    }
}

static void simChurn(simRun *r, int32_t ops) {
    for (int32_t op = 0; op < ops; op++) {
        const uint64_t roll = simNext(r) % 100;
        if (roll < 55) {
            simPut(r, simRandomKey(r));
        } else if (roll < 90 && r->n) {
            simDelAt(r, (int32_t)(simNext(r) % (uint64_t)r->n));
        } else {
            simDel(r, simRandomKey(r) | 1); /* a gap: never present */
        }
        if (op % 250 == 0) {
            simWalkWhileChanging(r);
        }
        if (op % 300 == 0) {
            simGapProbes(r);
        }
    }
}

typedef enum simDrainFrom {
    SIM_DRAIN_SCATTERED,
    SIM_DRAIN_FRONT,
    SIM_DRAIN_BACK
} simDrainFrom;

static void simDrain(simRun *r, simDrainFrom from, int32_t floor) {
    while (r->n > floor) {
        const int32_t at = from == SIM_DRAIN_FRONT ? 0
                           : from == SIM_DRAIN_BACK
                               ? r->n - 1
                               : (int32_t)(simNext(r) % (uint64_t)r->n);
        simDelAt(r, at);
    }
    simCheckAll(r);
    if (r->shrinks && r->tier != 1) {
        SIM_ERR(r, "drained to %d rows, still tier %u", r->n, r->tier);
    }
}

/* Insert and delete two keys just across each growth point, many times.
 * Growth must happen once; a family that shrinks must not flip back and
 * forth (its shrink point sits well below its growth point). */
static void simOscillate(simRun *r) {
    for (int32_t t = 2; t <= 3; t++) {
        const int32_t point = r->growPoint[t];
        if (point < 2 || point + 2 > SIM_CAP) {
            continue;
        }
        while (r->n > point - 1) {
            simDelAt(r, r->n - 1);
        }
        simGrow(r, point - 1);
        uint64_t before = 0;
        for (int32_t a = 0; a < 4; a++) {
            for (int32_t b = 0; b < 4; b++) {
                before += r->trans[a][b];
            }
        }
        for (int32_t cycle = 0; cycle < 40; cycle++) {
            const int64_t top = r->key[r->n - 1];
            simPut(r, top + 2);
            simPut(r, top + 4);
            simDel(r, top + 4);
            simDel(r, top + 2);
        }
        uint64_t after = 0;
        for (int32_t a = 0; a < 4; a++) {
            for (int32_t b = 0; b < 4; b++) {
                after += r->trans[a][b];
            }
        }
        if (after - before > 2) {
            SIM_ERR(r, "%" PRIu64 " tier changes in 40 cycles at row %d",
                    after - before, point);
        }
    }
}

static void simBulk(simRun *r, int32_t floor) {
    if (!r->ops->delUpTo) {
        return;
    }
    while (r->n > floor) {
        const int64_t bound = r->key[r->n / 4] - (int64_t)(simNext(r) & 1);
        const int32_t keep = simLower(r, bound + 1);
        const size_t removed = r->ops->delUpTo(&r->c, bound);
        if (removed != (size_t)keep) {
            SIM_ERR(r, "range delete to %" PRId64 " removed %zu, shadow %d",
                    bound, removed, keep);
        }
        memmove(&r->key[0], &r->key[keep],
                (size_t)(r->n - keep) * sizeof(r->key[0]));
        memmove(&r->val[0], &r->val[keep],
                (size_t)(r->n - keep) * sizeof(r->val[0]));
        r->n -= keep;
        simCheckOp(r, bound);
    }
    simCheckAll(r);
}

/* The end of every run, ordered or sequence: growth must reach Full; a
 * family that shrinks must leave Full and reach Small again, one that
 * does not must never move down; then one report line */
static void simVerdict(simRun *r) {
    const uint64_t up = r->trans[1][2] && r->trans[2][3];
    const uint64_t outOfFull = r->trans[3][2] + r->trans[3][1];
    const uint64_t intoSmall = r->trans[2][1] + r->trans[3][1];
    if (!up) {
        SIM_ERR(r,
                "growth never reached Full (S>M %" PRIu64 ", M>F %" PRIu64 ")",
                r->trans[1][2], r->trans[2][3]);
    }
    if (r->shrinks && (!outOfFull || !intoSmall)) {
        SIM_ERR(r, "shrinks, but F>M %" PRIu64 " F>S %" PRIu64 " M>S %" PRIu64,
                r->trans[3][2], r->trans[3][1], r->trans[2][1]);
    }
    if (!r->shrinks && (outOfFull || r->trans[2][1])) {
        SIM_ERR(r, "does not shrink, yet moved down");
    }

    printf("    %-16s %6" PRIu64 " ops; up S>M %" PRIu64 " M>F %" PRIu64
           "; down F>M %" PRIu64 " F>S %" PRIu64 " M>S %" PRIu64
           "; max units %u; worst Full density %.1fx; worst flap %u/20\n",
           r->name, r->opsDone, r->trans[1][2], r->trans[2][3], r->trans[3][2],
           r->trans[3][1], r->trans[2][1], r->maxUnits, r->worstDensity,
           r->worstFlap);
}

static int32_t simRunFamily(const simOps *ops, uint64_t seed) {
    simRun *r = &simState;
    memset(r, 0, sizeof(*r));
    r->ops = ops;
    r->name = ops->name;
    r->shrinks = ops->shrinks;
    r->coalesces = ops->coalesces;
    r->rng = seed;
    r->c = ops->make();
    dkTierStats s;
    ops->stats(r->c, &s);
    r->tier = s.tier;

    /* Drains end at two rows: under any family's Small re-entry point */
    simGrow(r, ops->growTo);
    simFlapProbe(r); /* fresh splits: neighbours sit at the merge point */
    simChurn(r, 4000);
    simFlapProbe(r);
    simDrain(r, SIM_DRAIN_SCATTERED, 2);
    simGrow(r, ops->growTo);
    simDrain(r, SIM_DRAIN_FRONT, 2);
    simGrow(r, ops->growTo);
    simDrain(r, SIM_DRAIN_BACK, 2);
    simGrow(r, ops->growTo);
    simOscillate(r);
    simGrow(r, ops->growTo);
    simBulk(r, 2);
    simGrow(r, ops->growTo);
    simCheckAll(r);
    simVerdict(r);
    ops->destroy(r->c);
    return r->err;
}

/* ====================================================================
 * Sequence families: values in order, by position (multilist,
 * multiarray).  The run's 'key' array holds the shadow sequence.
 * ==================================================================== */
typedef struct simSeqOps {
    const char *name;
    bool shrinks;
    bool coalesces;
    int32_t growTo;
    void *(*make)(void);
    void (*destroy)(void *c);
    void (*push)(void **c, bool tail, int64_t v);
    bool (*pop)(void **c, bool tail, int64_t *v);
    void (*insertAt)(void **c, int64_t idx, int64_t v); /* before idx */
    bool (*get)(void *c, int64_t idx, int64_t *v);
    void (*delRange)(void **c, int64_t start, int64_t count);
    void (*stats)(void *c, dkTierStats *s);
    /* From position 'from', toward the tail (forward) or the head */
    size_t (*walk)(void *c, int64_t from, bool forward, int64_t *out,
                   size_t max);
} simSeqOps;

/* ---- multilist ---- */
static mflexState *mlState[2];

static void *mlMake(void) {
    /* @own: caller; free=mflexStateFree */
    mlState[0] = mflexStateCreate();
    /* @own: caller; free=mflexStateFree */
    mlState[1] = mflexStateCreate();
    /* @own: caller; free=multilistFree */
    return multilistNewLimit(FLEX_CAP_LEVEL_64, 0);
}

static void mlDestroy(void *c) {
    multilistFree(c);
    mflexStateFree(mlState[0]);
    mflexStateFree(mlState[1]);
}

static void mlPush(void **c, bool tail, int64_t v) {
    const databox b = databoxNewSigned(v);
    if (tail) {
        multilistPushByTypeTail((multilist **)c, mlState[0], &b);
    } else {
        multilistPushByTypeHead((multilist **)c, mlState[0], &b);
    }
}

static bool mlPop(void **c, bool tail, int64_t *v) {
    databox got = {};
    if (!multilistPop((multilist **)c, mlState[0], &got, tail)) {
        return false;
    }
    *v = got.data.i;
    databoxFreeData(&got);
    return true;
}

static void mlInsertAt(void **c, int64_t idx, int64_t v) {
    if (idx == (int64_t)multilistCount(*c)) {
        mlPush(c, true, v);
        return;
    }
    multilistEntry at;
    const databox b = databoxNewSigned(v);
    multilistIndexGet(*c, mlState[0], idx, &at);
    multilistInsertByTypeBefore((multilist **)c, mlState, &at, &b);
}

static bool mlGet(void *c, int64_t idx, int64_t *v) {
    multilistEntry at;
    if (!multilistIndexGet(c, mlState[0], idx, &at)) {
        return false;
    }
    *v = at.box.data.i;
    return true;
}

static void mlDelRange(void **c, int64_t start, int64_t count) {
    multilistDelRange((multilist **)c, mlState[0], start, count);
}

static void mlStats(void *c, dkTierStats *s) {
    multilistStats(c, s);
}

static size_t mlWalk(void *c, int64_t from, bool forward, int64_t *out,
                     size_t max) {
    multilistIterator it;
    if (!multilistIteratorInitAtIdx(c, mlState, &it, from, forward, true)) {
        return 0;
    }
    size_t n = 0;
    multilistEntry e;
    while (n < max && multilistNext(&it, &e)) {
        out[n++] = e.box.data.i;
    }
    multilistIteratorRelease(&it);
    return n;
}

/* ---- multiarray: int64 elements, 16 per node ---- */
static void *maMake(void) {
    /* @own: caller; free=multiarrayFree */
    return multiarrayNew(sizeof(int64_t), 16);
}

static void maDestroy(void *c) {
    multiarrayFree(c);
}

static void maPush(void **c, bool tail, int64_t v) {
    multiarrayInsert((multiarray **)c,
                     tail ? (multiarrayIdx)multiarrayCount(*c) : 0, &v);
}

static bool maPop(void **c, bool tail, int64_t *v) {
    const size_t n = multiarrayCount(*c);
    if (!n) {
        return false;
    }
    multiarrayPop((multiarray **)c, tail ? (multiarrayIdx)(n - 1) : 0, v);
    return true;
}

static void maInsertAt(void **c, int64_t idx, int64_t v) {
    multiarrayInsert((multiarray **)c, (multiarrayIdx)idx, &v);
}

static bool maGet(void *c, int64_t idx, int64_t *v) {
    const void *at = multiarrayGet(c, (multiarrayIdx)idx);
    if (!at) {
        return false;
    }
    memcpy(v, at, sizeof(*v));
    return true;
}

static void maDelRange(void **c, int64_t start, int64_t count) {
    multiarrayDeleteRange((multiarray **)c, (multiarrayIdx)start,
                          (uint32_t)count);
}

static void maStats(void *c, dkTierStats *s) {
    multiarrayStats(c, s);
}

static size_t maWalk(void *c, int64_t from, bool forward, int64_t *out,
                     size_t max) {
    if ((size_t)from >= multiarrayCount(c)) {
        return 0;
    }
    multiarrayIter it;
    multiarrayIteratorInitAt(c, &it, (multiarrayIdx)from, forward);
    size_t n = 0;
    const void *at;
    while (n < max && (at = multiarrayIteratorNext(&it))) {
        memcpy(&out[n++], at, sizeof(*out));
    }
    return n;
}

static const simSeqOps simSequences[] = {
    {.name = "multilist",
     .shrinks = true,
     .coalesces = true,
     .growTo = 2000,
     .make = mlMake,
     .destroy = mlDestroy,
     .push = mlPush,
     .pop = mlPop,
     .insertAt = mlInsertAt,
     .get = mlGet,
     .delRange = mlDelRange,
     .stats = mlStats,
     .walk = mlWalk},
    {.name = "multiarray",
     .shrinks = true,
     .coalesces = true,
     .growTo = 3000,
     .make = maMake,
     .destroy = maDestroy,
     .push = maPush,
     .pop = maPop,
     .insertAt = maInsertAt,
     .get = maGet,
     .delRange = maDelRange,
     .stats = maStats,
     .walk = maWalk},
};

static const simSeqOps *simSeq;

/* Every 97 operations: both full walks and a walk from a random position
 * equal the shadow */
static void simSeqCheckAll(simRun *r) {
    const size_t fwd = simSeq->walk(r->c, 0, true, r->wk, SIM_CAP);
    bool same = fwd == (size_t)r->n;
    for (size_t i = 0; same && i < fwd; i++) {
        same = r->wk[i] == r->key[i];
    }
    const size_t back =
        r->n ? simSeq->walk(r->c, r->n - 1, false, r->wk, SIM_CAP) : 0;
    same = same && back == (size_t)r->n;
    for (size_t i = 0; same && i < back; i++) {
        same = r->wk[i] == r->key[r->n - 1 - (int32_t)i];
    }
    if (r->n) {
        const int32_t from = (int32_t)(simNext(r) % (uint64_t)r->n);
        const size_t tail = simSeq->walk(r->c, from, true, r->wk, SIM_CAP);
        same = same && tail == (size_t)(r->n - from);
        for (size_t i = 0; same && i < tail; i++) {
            same = r->wk[i] == r->key[from + (int32_t)i];
        }
    }
    if (!same) {
        SIM_ERR(r, "walks (%zu forward, %zu back) differ from the shadow (%d)",
                fwd, back, r->n);
    }
}

static void simSeqCheckOp(simRun *r) {
    dkTierStats s;
    simSeq->stats(r->c, &s);
    simTrackStats(r, &s);
    if (r->n) {
        const int32_t at = (int32_t)(simNext(r) % (uint64_t)r->n);
        int64_t v = 0;
        if (!simSeq->get(r->c, at, &v) || v != r->key[at]) {
            SIM_ERR(r, "get %d: %" PRId64 ", shadow %" PRId64, at, v,
                    r->key[at]);
        }
    }
    if (++r->opsDone % 97 == 0) {
        simSeqCheckAll(r);
    }
}

static void simSeqInsertAt(simRun *r, int32_t at) {
    if (r->n == SIM_CAP) {
        return;
    }
    const int64_t v = (int64_t)(simNext(r) >> 22);
    memmove(&r->key[at + 1], &r->key[at],
            (size_t)(r->n - at) * sizeof(r->key[0]));
    r->key[at] = v;
    r->n++;
    if (at == r->n - 1) {
        simSeq->push(&r->c, true, v);
    } else if (at == 0) {
        simSeq->push(&r->c, false, v);
    } else {
        simSeq->insertAt(&r->c, at, v);
    }
    simSeqCheckOp(r);
}

static void simSeqPop(simRun *r, bool tail) {
    int64_t v = 0;
    const bool got = simSeq->pop(&r->c, tail, &v);
    if (!r->n) {
        if (got) {
            SIM_ERR(r, "pop from an empty sequence returned a value");
        }
    } else {
        const int64_t want = tail ? r->key[r->n - 1] : r->key[0];
        if (!got || v != want) {
            SIM_ERR(r, "pop %s: %" PRId64 ", shadow %" PRId64,
                    tail ? "tail" : "head", v, want);
        }
        if (!tail) {
            memmove(&r->key[0], &r->key[1],
                    (size_t)(r->n - 1) * sizeof(r->key[0]));
        }
        r->n--;
    }
    simSeqCheckOp(r);
}

static void simSeqDelRange(simRun *r, int32_t start, int32_t count) {
    if (start >= r->n) {
        return;
    }
    if (count > r->n - start) {
        count = r->n - start;
    }
    memmove(&r->key[start], &r->key[start + count],
            (size_t)(r->n - start - count) * sizeof(r->key[0]));
    r->n -= count;
    simSeq->delRange(&r->c, start, count);
    simSeqCheckOp(r);
}

static void simSeqGrow(simRun *r, int32_t rows) {
    while (r->n < rows) {
        simSeqInsertAt(r, r->n);
    }
}

static void simSeqChurn(simRun *r, int32_t ops) {
    for (int32_t op = 0; op < ops; op++) {
        const uint64_t roll = simNext(r) % 100;
        if (roll < 25) {
            simSeqInsertAt(r, r->n);
        } else if (roll < 40) {
            simSeqInsertAt(r, 0);
        } else if (roll < 55) {
            simSeqPop(r, true);
        } else if (roll < 70) {
            simSeqPop(r, false);
        } else if (roll < 82) {
            simSeqInsertAt(r, (int32_t)(simNext(r) % (uint64_t)(r->n + 1)));
        } else if (roll < 94 && r->n) {
            simSeqDelRange(r, (int32_t)(simNext(r) % (uint64_t)r->n),
                           1 + (int32_t)(simNext(r) % 3));
        } else {
            simSeqCheckOp(r);
        }
    }
}

static void simSeqDrain(simRun *r, simDrainFrom from, int32_t floor) {
    while (r->n > floor) {
        if (from == SIM_DRAIN_FRONT) {
            simSeqPop(r, false);
        } else if (from == SIM_DRAIN_BACK) {
            simSeqPop(r, true);
        } else {
            simSeqDelRange(r, (int32_t)(simNext(r) % (uint64_t)r->n), 1);
        }
    }
    simSeqCheckAll(r);
    if (r->shrinks && r->tier != 1) {
        SIM_ERR(r, "drained to %d values, still tier %u", r->n, r->tier);
    }
}

/* Two pushes and two pops across each growth point, many times */
static void simSeqOscillate(simRun *r) {
    for (int32_t t = 2; t <= 3; t++) {
        const int32_t point = r->growPoint[t];
        if (point < 2 || point + 2 > SIM_CAP) {
            continue;
        }
        while (r->n > point - 1) {
            simSeqPop(r, true);
        }
        simSeqGrow(r, point - 1);
        uint64_t before = 0;
        for (int32_t a = 0; a < 16; a++) {
            before += r->trans[a / 4][a % 4];
        }
        for (int32_t cycle = 0; cycle < 40; cycle++) {
            simSeqInsertAt(r, r->n);
            simSeqInsertAt(r, r->n);
            simSeqPop(r, true);
            simSeqPop(r, true);
        }
        uint64_t after = 0;
        for (int32_t a = 0; a < 16; a++) {
            after += r->trans[a / 4][a % 4];
        }
        if (after - before > 2) {
            SIM_ERR(r, "%" PRIu64 " tier changes in 40 cycles at %d values",
                    after - before, point);
        }
    }
}

/* Delete one value and insert one at the same position, ten times, at 32
 * positions: Full's units must not merge and split on every operation */
static void simSeqFlapProbe(simRun *r) {
    if (r->tier != 3 || r->n < 2) {
        return;
    }
    for (int32_t p = 0; p < 32; p++) {
        const int32_t at = (int32_t)(simNext(r) % (uint64_t)r->n);
        dkTierStats s;
        simSeq->stats(r->c, &s);
        uint32_t units = s.units;
        uint32_t changes = 0;
        for (int32_t cycle = 0; cycle < 10 && at < r->n; cycle++) {
            simSeqDelRange(r, at, 1);
            simSeq->stats(r->c, &s);
            changes += s.units != units;
            units = s.units;
            simSeqInsertAt(r, at);
            simSeq->stats(r->c, &s);
            changes += s.units != units;
            units = s.units;
        }
        if (changes > r->worstFlap) {
            r->worstFlap = changes;
        }
    }
    if (r->coalesces && r->worstFlap > 4) {
        SIM_ERR(r, "units changed %u times in 20 delete/insert ops at one spot",
                r->worstFlap);
    }
}

static void simSeqBulk(simRun *r, int32_t floor) {
    while (r->n > floor) {
        const int32_t start = (int32_t)(simNext(r) % (uint64_t)r->n);
        simSeqDelRange(r, start / 2, r->n / 4 + 1);
    }
    simSeqCheckAll(r);
}

static int32_t simRunSequence(const simSeqOps *ops, uint64_t seed) {
    simRun *r = &simState;
    memset(r, 0, sizeof(*r));
    simSeq = ops;
    r->name = ops->name;
    r->shrinks = ops->shrinks;
    r->coalesces = ops->coalesces;
    r->rng = seed;
    r->c = ops->make();
    dkTierStats s;
    ops->stats(r->c, &s);
    r->tier = s.tier;

    simSeqGrow(r, ops->growTo);
    simSeqFlapProbe(r);
    simSeqChurn(r, 4000);
    simSeqFlapProbe(r);
    simSeqDrain(r, SIM_DRAIN_SCATTERED, 2);
    simSeqGrow(r, ops->growTo);
    simSeqDrain(r, SIM_DRAIN_FRONT, 2);
    simSeqGrow(r, ops->growTo);
    simSeqDrain(r, SIM_DRAIN_BACK, 2);
    simSeqGrow(r, ops->growTo);
    simSeqOscillate(r);
    simSeqGrow(r, ops->growTo);
    simSeqBulk(r, 2);
    simSeqGrow(r, ops->growTo);
    simSeqCheckAll(r);
    simVerdict(r);
    ops->destroy(r->c);
    return r->err;
}

int tierSimTest(int, char *[]) {
    int err = 0;

    TEST("tier simulation: every family through every phase") {
        for (size_t f = 0; f < countof(simFamilies); f++) {
            if (simRunFamily(&simFamilies[f], 0x243F6A8885A308D3ULL + f)) {
                ERR("%s: simulation failed", simFamilies[f].name);
            }
        }
    }

    TEST("tier simulation: every sequence family through every phase") {
        for (size_t f = 0; f < countof(simSequences); f++) {
            if (simRunSequence(&simSequences[f], 0x13198A2E03707344ULL + f)) {
                ERR("%s: simulation failed", simSequences[f].name);
            }
        }
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
