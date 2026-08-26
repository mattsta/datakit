# Real-World Use Cases

Complete, self-contained scenarios that combine several datakit modules
to solve one production problem. Every API call is checked against the
current headers in `src/`.

Scope: this file holds **whole applications**. Small copy-paste idioms
live in [PATTERNS.md](PATTERNS.md), measurement recipes live in
[BENCHMARKS.md](BENCHMARKS.md), and single-module walkthroughs live in
`docs/modules/<module>/EXAMPLES.md`.

## Table of Contents

1. [Sharded LRU Cache](#sharded-lru-cache)
2. [Compressed Log Aggregator](#compressed-log-aggregator)
3. [Real-Time Analytics Dashboard](#real-time-analytics-dashboard)
4. [Single-Module Scenarios](#single-module-scenarios)

---

## Sharded LRU Cache

Combines `rapidhash` for shard selection, `multimap` for storage, and
`multilru` for eviction. The interesting part is wiring `multilru`'s
eviction callback to the map so entries actually disappear from storage
when the LRU evicts them — an LRU that only tracks recency while the map
grows forever is the classic way to get this wrong.

```c
#include "multimap.h"
#include "multilru.h"
#include "rapidhash.h"
#include "timeUtil.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum { CACHE_SHARDS = 16 };

typedef struct cacheShard {
    multimap *data;    /* key -> {value, lruPtr}          */
    multimap *byLru;   /* lruPtr -> key (reverse index)   */
    multilru *lru;     /* recency ordering                */
    uint64_t hits;
    uint64_t misses;
} cacheShard;

typedef struct shardedCache {
    cacheShard shards[CACHE_SHARDS];
} shardedCache;

/* multilru calls this immediately BEFORE it frees an evicted entry, so
 * this is where the storage side of the eviction happens. */
static void cacheOnEvict(size_t evictedPtr, void *userData) {
    cacheShard *s = userData;

    const databox lruBox = databoxNewUnsigned(evictedPtr);

    databox key;
    if (multimapLookupInto(s->byLru, &lruBox, &key, 1)) {
        /* Copy the key: deleting from byLru invalidates the reference
         * the lookup handed back. */
        databox owned = databoxCopy(&key);
        multimapDelete(&s->byLru, &lruBox);
        multimapDelete(&s->data, &owned);
        databoxFreeData(&owned);
    }
}

shardedCache *cacheNew(uint64_t maxEntries) {
    shardedCache *c = zcalloc(1, sizeof(*c));
    const uint64_t perShard = maxEntries / CACHE_SHARDS;

    for (size_t i = 0; i < CACHE_SHARDS; i++) {
        cacheShard *s = &c->shards[i];
        s->data = multimapNew(3);   /* key + value + lruPtr */
        s->byLru = multimapNew(2);  /* lruPtr + key         */
        s->lru = multilruNew();

        /* Automatic eviction: multilru enforces the cap on insert and
         * tells us which entry left so we can drop its storage. */
        multilruSetMaxCount(s->lru, perShard);
        multilruSetEvictCallback(s->lru, cacheOnEvict, s);
    }

    return c;
}

void cacheFree(shardedCache *c) {
    if (!c) {
        return;
    }

    for (size_t i = 0; i < CACHE_SHARDS; i++) {
        multilruFree(c->shards[i].lru);
        multimapFree(c->shards[i].data);
        multimapFree(c->shards[i].byLru);
    }

    zfree(c);
}

static cacheShard *cacheShardFor(shardedCache *c, const void *key,
                                 size_t keyLen) {
    return &c->shards[rapidhash(key, keyLen) % CACHE_SHARDS];
}

void cachePut(shardedCache *c, const void *key, size_t keyLen,
              const void *value, size_t valueLen) {
    cacheShard *s = cacheShardFor(c, key, keyLen);

    const databox keyBox = databoxNewBytes(key, keyLen);

    /* Replacing an existing key: retire its old LRU handle first so the
     * reverse index never points at a stale entry. */
    databox existing[2];
    if (multimapLookupInto(s->data, &keyBox, existing, 2)) {
        const databox oldLru = databoxNewUnsigned(existing[1].data.u64);
        multimapDelete(&s->byLru, &oldLru);
        multilruDelete(s->lru, (multilruPtr)existing[1].data.u64);
    }

    /* Insert can trigger eviction, which calls cacheOnEvict. */
    const multilruPtr ptr = multilruInsert(s->lru);

    const databox valueBox = databoxNewBytes(value, valueLen);
    const databox lruBox = databoxNewUnsigned(ptr);

    const databox *entry[3] = {&keyBox, &valueBox, &lruBox};
    multimapInsert(&s->data, entry);

    const databox *reverse[2] = {&lruBox, &keyBox};
    multimapInsert(&s->byLru, reverse);
}

/* Returns a REFERENCE into the shard's storage: copy it before the next
 * mutation of this shard if you need it to outlive the call. */
bool cacheGet(shardedCache *c, const void *key, size_t keyLen,
              databox *outValue) {
    cacheShard *s = cacheShardFor(c, key, keyLen);

    const databox keyBox = databoxNewBytes(key, keyLen);

    databox found[2];  /* value + lruPtr */
    if (!multimapLookupInto(s->data, &keyBox, found, 2)) {
        s->misses++;
        return false;
    }

    multilruIncrease(s->lru, (multilruPtr)found[1].data.u64);
    s->hits++;
    *outValue = found[0];
    return true;
}

bool cacheDelete(shardedCache *c, const void *key, size_t keyLen) {
    cacheShard *s = cacheShardFor(c, key, keyLen);

    const databox keyBox = databoxNewBytes(key, keyLen);

    databox found[2];
    if (!multimapLookupInto(s->data, &keyBox, found, 2)) {
        return false;
    }

    const databox lruBox = databoxNewUnsigned(found[1].data.u64);
    multilruDelete(s->lru, (multilruPtr)found[1].data.u64);
    multimapDelete(&s->byLru, &lruBox);
    multimapDelete(&s->data, &keyBox);
    return true;
}

void cacheReport(const shardedCache *c) {
    size_t entries = 0;
    size_t bytes = 0;
    uint64_t hits = 0;
    uint64_t misses = 0;

    for (size_t i = 0; i < CACHE_SHARDS; i++) {
        const cacheShard *s = &c->shards[i];
        entries += multimapCount(s->data);
        bytes += multimapBytes(s->data) + multimapBytes(s->byLru) +
                 multilruBytes(s->lru);
        hits += s->hits;
        misses += s->misses;
    }

    const uint64_t total = hits + misses;
    printf("entries %zu, bytes %zu, hit rate %.1f%%\n", entries, bytes,
           total ? 100.0 * (double)hits / (double)total : 0.0);
}

void exampleShardedCache(void) {
    shardedCache *c = cacheNew(10000);

    char key[32];
    char value[64];
    for (uint64_t i = 0; i < 50000; i++) {
        const size_t keyLen =
            (size_t)snprintf(key, sizeof(key), "key_%" PRIu64, i);
        const size_t valueLen =
            (size_t)snprintf(value, sizeof(value), "value_%" PRIu64, i);
        cachePut(c, key, keyLen, value, valueLen);
    }

    /* 50K inserts against a 10K cap: eviction kept it bounded. */
    databox got;
    const size_t probeLen =
        (size_t)snprintf(key, sizeof(key), "key_%" PRIu64, UINT64_C(49999));
    if (cacheGet(c, key, probeLen, &got)) {
        printf("hit: %.*s\n", (int)databoxLen(&got),
               (const char *)databoxCBytes(&got));
    }

    cacheReport(c);
    cacheFree(c);
}
```

Notes on the shape of this:

- `multilruSetMaxCount` plus `multilruSetEvictCallback` is the intended
  eviction workflow. The alternative — `multilruSetAutoEvict(lru,
false)` and a `while (multilruNeedsEviction(lru))` drain loop — is
  documented in `multilru.h` and is the right choice when you want to
  batch evictions rather than pay them on the insert path.
- The reverse index (`byLru`) exists because eviction hands you an LRU
  handle, not a key. Without it the callback has nothing to delete.
- `multimapLookupInto` fills `elementsPerEntry - 1` boxes — values only,
  since you already hold the key.

---

## Compressed Log Aggregator

Collects log lines into a compressed `multilist`, interns repeated source
names with `multimapAtom`, and keeps a sorted `multimap` index for range
queries by time.

```c
#include "multilist.h"
#include "multimap.h"
#include "multimapAtom.h"
#include "timeUtil.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef enum logLevel {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL
} logLevel;

typedef struct logAggregator {
    multilist *lines;     /* compressed message storage           */
    multimapAtom *names;  /* interned source names                */
    multimap *index;      /* timestampMs + sourceRef + lineOffset */
    mflexState *state;    /* scratch for single-state list calls  */
} logAggregator;

logAggregator *logAggregatorNew(void) {
    logAggregator *agg = zcalloc(1, sizeof(*agg));

    /* 8 KB nodes; keep the newest 5 nodes at each end uncompressed so
     * appends and tail reads never pay decompression. */
    agg->lines = multilistNew(FLEX_CAP_LEVEL_8192, 5);
    agg->names = multimapAtomNew();
    agg->index = multimapNew(3);
    agg->state = mflexStateCreate();

    return agg;
}

void logAggregatorFree(logAggregator *agg) {
    if (!agg) {
        return;
    }

    mflexStateFree(agg->state);
    multilistFree(agg->lines);
    multimapAtomFree(agg->names);
    multimapFree(agg->index);
    zfree(agg);
}

void logAggregatorAppend(logAggregator *agg, logLevel level,
                         const char *source, const char *message) {
    const uint64_t nowMs = timeUtilMs();

    char line[2048];
    const int lineLen =
        snprintf(line, sizeof(line), "%" PRIu64 " [%d] %s: %s", nowMs,
                 (int)level, source, message);
    if (lineLen <= 0) {
        return;
    }

    const databox lineBox = databoxNewBytes(line, (size_t)lineLen);
    multilistPushByTypeTail(&agg->lines, agg->state, &lineBox);

    /* Intern the source name. InsertIfNewConvert rewrites sourceBox
     * IN PLACE into the atom reference, so after this call sourceBox is
     * a small integer reference, not the string. */
    databox sourceBox = databoxNewBytesString(source);
    multimapAtomInsertIfNewConvert(agg->names, &sourceBox);

    const databox tsBox = databoxNewUnsigned(nowMs);
    const databox offsetBox =
        databoxNewUnsigned(multilistCount(agg->lines) - 1);

    /* Append, not Insert: timestamps arrive non-decreasing, and Append
     * skips the sorted-position search entirely. */
    const databox *entry[3] = {&tsBox, &sourceBox, &offsetBox};
    multimapAppend(&agg->index, entry);
}

/* Walk every line logged in [startMs, endMs]. When sourceFilter is
 * non-NULL, only lines from that source are reported. */
void logAggregatorQuery(logAggregator *agg, uint64_t startMs, uint64_t endMs,
                        const char *sourceFilter,
                        void (*onLine)(const databox *line, void *userData),
                        void *userData) {
    databox filterRef;
    bool haveFilter = false;

    if (sourceFilter) {
        const databox filterBox = databoxNewBytesString(sourceFilter);
        if (!multimapAtomLookupReference(agg->names, &filterBox, &filterRef)) {
            return;  /* that source never logged anything */
        }
        haveFilter = true;
    }

    const databox startBox = databoxNewUnsigned(startMs);

    multimapIterator iter;
    if (!multimapIteratorInitAt(agg->index, &iter, true, &startBox)) {
        return;
    }

    databox entry[3];
    while (multimapIteratorNextInto(&iter, entry, 3)) {
        if (entry[0].data.u64 > endMs) {
            break;
        }

        if (haveFilter && entry[1].data.u64 != filterRef.data.u64) {
            continue;
        }

        multilistEntry line;
        if (multilistIndexGet(agg->lines, agg->state, (mlOffsetId)entry[2].data.u64,
                              &line)) {
            onLine(&line.box, userData);
        }
    }
}

void logAggregatorReport(const logAggregator *agg) {
    printf("lines          %zu\n", multilistCount(agg->lines));
    printf("line bytes     %zu\n", multilistBytes(agg->lines));
    printf("interned names %zu (%zu bytes)\n", multimapAtomCount(agg->names),
           multimapAtomBytes(agg->names));
    printf("index entries  %zu (%zu bytes)\n", multimapCount(agg->index),
           multimapBytes(agg->index));
}

static void printLine(const databox *line, void *userData) {
    (void)userData;
    printf("%.*s\n", (int)databoxLen(line), (const char *)databoxCBytes(line));
}

void exampleLogAggregator(void) {
    logAggregator *agg = logAggregatorNew();

    const uint64_t startMs = timeUtilMs();

    logAggregatorAppend(agg, LOG_INFO, "app", "starting up");

    char message[256];
    for (uint64_t i = 0; i < 100000; i++) {
        snprintf(message, sizeof(message), "handled request %" PRIu64, i);
        logAggregatorAppend(agg, LOG_DEBUG, "http", message);
    }

    logAggregatorAppend(agg, LOG_ERROR, "db", "connection lost");
    logAggregatorAppend(agg, LOG_INFO, "app", "shutting down");

    /* Only the "db" lines from this run. */
    logAggregatorQuery(agg, startMs, timeUtilMs(), "db", printLine, NULL);

    logAggregatorReport(agg);
    logAggregatorFree(agg);
}
```

Notes:

- The interned source reference is what lands in the index, so filtering
  by source is an integer comparison rather than a string compare — and
  100,000 lines from `"http"` store the name exactly once.
- `multilistIndexGet` takes a **single** `mflexState *`. The multilist
  _iterator_ entry points take a two-element `mflexState *state[2]`
  instead; see [PATTERNS.md](PATTERNS.md#iteration).
- Compression only pays for the nodes behind the uncompressed depth
  (5 here). Recent lines stay hot and uncompressed; old ones shrink.

---

## Real-Time Analytics Dashboard

Combines a sorted `multimap` time series, `multimapAtom` for metric
names, `hyperloglog` for unique-visitor cardinality, and `multiroar` for
an exact set of currently-active user IDs.

```c
#include "hyperloglog.h"
#include "multimap.h"
#include "multimapAtom.h"
#include "multiroar.h"
#include "str.h" /* splitmix64 / xoroshiro128plus */
#include "timeUtil.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct dashboard {
    multimap *series;         /* timestampS + metricRef + value  */
    multimapAtom *metrics;    /* interned metric names           */
    multimap *counters;       /* metricRef + observationCount    */
    hyperloglog *uniqueUsers; /* approximate distinct users       */
    multiroar *activeUsers;   /* exact set of active user IDs     */
} dashboard;

dashboard *dashboardNew(void) {
    dashboard *d = zcalloc(1, sizeof(*d));
    d->series = multimapNew(3);
    d->metrics = multimapAtomNew();
    d->counters = multimapNew(2);
    d->uniqueUsers = hyperloglogNew();
    d->activeUsers = multiroarBitNew();
    return d;
}

void dashboardFree(dashboard *d) {
    if (!d) {
        return;
    }

    multimapFree(d->series);
    multimapAtomFree(d->metrics);
    multimapFree(d->counters);
    hyperloglogFree(d->uniqueUsers);
    multiroarFree(d->activeUsers);
    zfree(d);
}

void dashboardRecord(dashboard *d, const char *metric, double value) {
    databox metricRef = databoxNewBytesString(metric);
    multimapAtomInsertIfNewConvert(d->metrics, &metricRef);

    const databox tsBox = databoxNewUnsigned(timeUtilS());
    const databox valueBox = databoxNewReal(value);

    /* Samples arrive in time order, so Append beats Insert here. The
     * series is range-scanned, never keyed by timestamp, so repeated
     * timestamps are fine. */
    const databox *sample[3] = {&tsBox, &metricRef, &valueBox};
    multimapAppend(&d->series, sample);

    /* Per-metric observation count: increment field 1 of the entry, or
     * seed it when this metric is new. */
    if (multimapExists(d->counters, &metricRef)) {
        multimapFieldIncr(&d->counters, &metricRef, 1, 1);
    } else {
        const databox one = databoxNewSigned(1);
        const databox *counter[2] = {&metricRef, &one};
        multimapInsert(&d->counters, counter);
    }
}

void dashboardRecordUser(dashboard *d, uint64_t userId) {
    /* hyperloglogAdd takes hyperloglog ** because a sparse HLL is
     * reallocated (and eventually promoted to dense) as it fills. */
    hyperloglogAdd(&d->uniqueUsers, &userId, sizeof(userId));

    /* Exact membership for the bounded, dense "currently active" set. */
    multiroarBitSet(d->activeUsers, userId);
}

typedef struct metricWindow {
    double sum;
    double min;
    double max;
    uint64_t count;
} metricWindow;

bool dashboardWindow(dashboard *d, const char *metric, uint64_t startS,
                     uint64_t endS, metricWindow *out) {
    const databox metricBox = databoxNewBytesString(metric);

    databox metricRef;
    if (!multimapAtomLookupReference(d->metrics, &metricBox, &metricRef)) {
        return false;
    }

    const databox startBox = databoxNewUnsigned(startS);

    multimapIterator iter;
    if (!multimapIteratorInitAt(d->series, &iter, true, &startBox)) {
        return false;
    }

    *out = (metricWindow){.sum = 0.0, .min = INFINITY, .max = -INFINITY};

    databox sample[3];
    while (multimapIteratorNextInto(&iter, sample, 3)) {
        if (sample[0].data.u64 > endS) {
            break;
        }

        if (sample[1].data.u64 != metricRef.data.u64) {
            continue;
        }

        const double v = sample[2].data.d64;
        out->sum += v;
        out->count++;
        if (v < out->min) {
            out->min = v;
        }
        if (v > out->max) {
            out->max = v;
        }
    }

    return out->count > 0;
}

void dashboardReport(dashboard *d) {
    bool invalid = false;
    const uint64_t approxUsers = hyperloglogCount(d->uniqueUsers, &invalid);

    printf("unique users (approx) %" PRIu64 "%s\n", approxUsers,
           invalid ? " [HLL corrupt]" : "");
    printf("active users (exact)  %" PRIu64 " in %" PRIu64 " bitmap bytes\n",
           multiroarBitCount(d->activeUsers),
           multiroarMemoryUsage(d->activeUsers));
    printf("samples               %zu (%zu bytes)\n", multimapCount(d->series),
           multimapBytes(d->series));

    /* Counters are keyed by atom reference; resolve names for display. */
    multimapIterator iter;
    multimapIteratorInit(d->counters, &iter, true);

    databox counter[2];
    while (multimapIteratorNextInto(&iter, counter, 2)) {
        databox name;
        if (multimapAtomLookup(d->metrics, &counter[0], &name)) {
            printf("  %.*s: %" PRId64 "\n", (int)databoxLen(&name),
                   (const char *)databoxCBytes(&name), counter[1].data.i64);
        }
    }
}

void exampleDashboard(void) {
    dashboard *d = dashboardNew();

    /* Deterministic pseudo-random traffic; see PATTERNS.md for seeding. */
    uint64_t seed = 0x9e3779b97f4a7c15ULL;
    uint64_t rng[2] = {splitmix64(&seed), splitmix64(&seed)};

    const uint64_t startS = timeUtilS();

    for (uint64_t i = 0; i < 10000; i++) {
        dashboardRecord(d, "page_views", 1.0);
        dashboardRecord(d, "response_time_ms",
                        50.0 + (double)(xoroshiro128plus(rng) % 100));
        dashboardRecordUser(d, xoroshiro128plus(rng) % 5000);
    }

    metricWindow w;
    if (dashboardWindow(d, "response_time_ms", startS, timeUtilS(), &w)) {
        printf("response_time_ms: avg %.2f, min %.2f, max %.2f over %" PRIu64
               " samples\n",
               w.sum / (double)w.count, w.min, w.max, w.count);
    }

    dashboardReport(d);
    dashboardFree(d);
}
```

Notes:

- `hyperloglog` answers "how many distinct?" in a fixed ~12 KB with
  roughly 0.8% error, and cannot answer "is this ID present?".
  `multiroar` answers membership exactly but scales with the ID space.
  Using both is not redundancy — they answer different questions.
- `hyperloglogCount`'s second argument reports a corrupt sparse
  encoding; pass `NULL` when you do not want to check.
- `multimapFieldIncr(&map, key, fieldOffset, incrBy)` mutates one field
  of an existing entry in place, so a counter update does not rewrite
  the whole entry.

---

## Single-Module Scenarios

These scenarios exercise one module deeply and are maintained in that
module's own examples file rather than duplicated here:

| Scenario                     | Location                              |
| ---------------------------- | ------------------------------------- |
| User session store with TTL  | `docs/modules/multimap/EXAMPLES.md`   |
| Time-series metric storage   | `docs/modules/multimap/EXAMPLES.md`   |
| Inverted index / text search | `docs/modules/multimap/EXAMPLES.md`   |
| Leaderboard with rankings    | `docs/modules/multimap/EXAMPLES.md`   |
| Query result cache           | `docs/modules/multimap/EXAMPLES.md`   |
| Queue, stack, deque          | `docs/modules/multilist/EXAMPLES.md`  |
| Circular buffer              | `docs/modules/multilist/EXAMPLES.md`  |
| Priority message queue       | `docs/modules/multilist/EXAMPLES.md`  |
| Browser history              | `docs/modules/multilist/EXAMPLES.md`  |
| Vertex / particle buffers    | `docs/modules/multiarray/EXAMPLES.md` |
| Sorted index + binary search | `docs/modules/multiarray/EXAMPLES.md` |

---

## Summary

| Use case            | Modules combined                                            |
| ------------------- | ----------------------------------------------------------- |
| Sharded LRU cache   | rapidhash + multimap + multilru                             |
| Log aggregator      | multilist (compressed) + multimapAtom + multimap + timeUtil |
| Analytics dashboard | multimap + multimapAtom + hyperloglog + multiroar + str     |

Three habits carry across all of them:

1. **Intern repeated strings once** with `multimapAtom` and store the
   integer reference everywhere else.
2. **Append instead of insert** when keys arrive in order.
3. **Wire eviction to storage**, so a recency structure and a data
   structure cannot drift apart.

Next:

- [PATTERNS.md](PATTERNS.md) — the idioms these examples are built from
- [BENCHMARKS.md](BENCHMARKS.md) — how to measure what you built
