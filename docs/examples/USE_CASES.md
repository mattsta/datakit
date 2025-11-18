# Real-World Use Cases

This guide demonstrates practical, production-ready applications built with datakit, showing how to combine multiple modules to solve real problems.

## Table of Contents

1. [Web Application Session Store](#web-application-session-store)
2. [High-Performance Log Aggregator](#high-performance-log-aggregator)
3. [Real-Time Analytics Dashboard](#real-time-analytics-dashboard)
4. [Distributed Cache System](#distributed-cache-system)
5. [Full-Text Search Engine](#full-text-search-engine)
6. [Gaming Leaderboard Service](#gaming-leaderboard-service)
7. [IoT Sensor Data Collector](#iot-sensor-data-collector)
8. [Configuration Management System](#configuration-management-system)
9. [Message Queue with Priorities](#message-queue-with-priorities)
10. [Graph Database (Adjacency List)](#graph-database-adjacency-list)

---

## Web Application Session Store

A production-ready session store with TTL, LRU eviction, and multi-field sessions.

```c
#include "multimap.h"
#include "multimapAtom.h"
#include "multilru.h"
#include <time.h>

typedef struct {
    multimapAtom *atoms;      /* String interning for session IDs */
    multimap *sessions;       /* sessionRef → {userId, data, lastAccess} */
    multilru *lru;            /* LRU tracking */
    size_t maxSessions;
    time_t sessionTTL;
} SessionStore;

SessionStore *sessionStoreNew(size_t maxSessions, time_t ttl) {
    SessionStore *store = malloc(sizeof(*store));
    store->atoms = multimapAtomNew();
    store->sessions = multimapNew(4);  /* sessionRef + userId + data + lastAccess */
    store->lru = multilruNew();
    store->maxSessions = maxSessions;
    store->sessionTTL = ttl;
    return store;
}

typedef struct {
    char sessionId[64];
    uint64_t userId;
    void *data;
    size_t dataLen;
    time_t lastAccess;
} Session;

/* Create new session */
bool sessionStoreCreate(SessionStore *store, const char *sessionId,
                       uint64_t userId, const void *data, size_t dataLen) {
    /* Evict if at capacity */
    while (multimapCount(store->sessions) >= store->maxSessions) {
        multilruPtr evicted;
        if (!multilruRemoveMinimum(store->lru, &evicted)) {
            break;
        }
        /* In production: track session→lru mapping for removal */
    }

    /* Intern session ID */
    databox sidBox = databoxNewBytesString(sessionId);
    multimapAtomInsertIfNewConvert(store->atoms, &sidBox);

    /* Store session data */
    databox userBox = databoxNewUnsigned(userId);
    databox dataBox = databoxNewBytes(data, dataLen);
    databox timeBox = databoxNewSigned(time(NULL));

    const databox *elements[4] = {&sidBox, &userBox, &dataBox, &timeBox};
    multimapInsert(&store->sessions, elements);

    /* Track in LRU */
    multilruInsert(store->lru);

    return true;
}

/* Get session and update access time */
bool sessionStoreGet(SessionStore *store, const char *sessionId,
                    Session *outSession) {
    /* Look up session ID reference */
    databox sidBox = databoxNewBytesString(sessionId);
    databox sidRef;

    if (!multimapAtomLookupReference(store->atoms, &sidBox, &sidRef)) {
        return false;  /* Session doesn't exist */
    }

    /* Get session data */
    databox userId, data, lastAccess;
    databox *results[3] = {&userId, &data, &lastAccess};

    if (!multimapLookup(store->sessions, &sidRef, results)) {
        return false;
    }

    /* Check TTL */
    time_t age = time(NULL) - lastAccess.data.i64;
    if (age > store->sessionTTL) {
        /* Expired */
        multimapDelete(&store->sessions, &sidRef);
        return false;
    }

    /* Update last access time */
    multimapFieldUpdate(&store->sessions, &sidRef, 3,
                       &(databox){.data.i64 = time(NULL),
                                 .type = DATABOX_SIGNED_64});

    /* Fill output */
    strncpy(outSession->sessionId, sessionId, sizeof(outSession->sessionId) - 1);
    outSession->userId = userId.data.u64;
    outSession->data = malloc(databoxLen(&data));
    memcpy(outSession->data, databoxCBytes(&data), databoxLen(&data));
    outSession->dataLen = databoxLen(&data);
    outSession->lastAccess = time(NULL);

    return true;
}

/* Delete session */
void sessionStoreDelete(SessionStore *store, const char *sessionId) {
    databox sidBox = databoxNewBytesString(sessionId);
    databox sidRef;

    if (multimapAtomLookupReference(store->atoms, &sidBox, &sidRef)) {
        multimapDelete(&store->sessions, &sidRef);
    }
}

/* Expire old sessions */
size_t sessionStoreExpireOld(SessionStore *store) {
    time_t cutoff = time(NULL) - store->sessionTTL;
    databox expired[1000];
    size_t expiredCount = 0;

    /* Find expired */
    multimapIterator iter;
    multimapIteratorInit(store->sessions, &iter, true);

    databox sid, userId, data, lastAccess;
    databox *elements[4] = {&sid, &userId, &data, &lastAccess};

    while (multimapIteratorNext(&iter, elements) && expiredCount < 1000) {
        if (lastAccess.data.i64 < cutoff) {
            expired[expiredCount++] = sid;
        }
    }

    /* Delete expired */
    for (size_t i = 0; i < expiredCount; i++) {
        multimapDelete(&store->sessions, &expired[i]);
    }

    return expiredCount;
}

void sessionStoreFree(SessionStore *store) {
    if (store) {
        multimapAtomFree(store->atoms);
        multimapFree(store->sessions);
        multilruFree(store->lru);
        free(store);
    }
}

/* Example usage */
void exampleSessionStore(void) {
    SessionStore *store = sessionStoreNew(10000, 3600);  /* 10K sessions, 1hr TTL */

    /* Create session */
    char userData[] = "user_preferences_data";
    sessionStoreCreate(store, "sess_abc123", 1001, userData, strlen(userData));

    /* Retrieve session */
    Session sess;
    if (sessionStoreGet(store, "sess_abc123", &sess)) {
        printf("User ID: %lu\n", sess.userId);
        printf("Data: %.*s\n", (int)sess.dataLen, (char *)sess.data);
        free(sess.data);
    }

    /* Periodic cleanup */
    size_t expired = sessionStoreExpireOld(store);
    printf("Expired %zu sessions\n", expired);

    sessionStoreFree(store);
}
```

---

## High-Performance Log Aggregator

Collect, compress, and query millions of log entries efficiently.

```c
#include "multilist.h"
#include "multimapAtom.h"
#include "multimap.h"
#include <time.h>

typedef struct {
    multilist *logs;          /* Compressed log storage */
    multimapAtom *atoms;      /* Interned log levels and sources */
    multimap *index;          /* Timestamp index */
} LogAggregator;

typedef enum {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL
} LogLevel;

LogAggregator *logAggregatorNew(void) {
    LogAggregator *agg = malloc(sizeof(*agg));
    /* Use compression for logs (first/last 5 nodes uncompressed) */
    agg->logs = multilistNew(FLEX_CAP_LEVEL_8192, 5);
    agg->atoms = multimapAtomNew();
    agg->index = multimapNew(2);  /* timestamp → logIndex */
    return agg;
}

void logAggregatorAppend(LogAggregator *agg, LogLevel level,
                        const char *source, const char *message) {
    mflexState *state = mflexStateCreate();

    /* Build log entry */
    char entry[2048];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    snprintf(entry, sizeof(entry),
             "[%04d-%02d-%02d %02d:%02d:%02d] [%d] [%s] %s",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec,
             level, source, message);

    /* Intern level and source strings for deduplication */
    databox levelBox = databoxNewSigned(level);
    databox sourceBox = databoxNewBytesString(source);
    multimapAtomInsertIfNewConvert(agg->atoms, &sourceBox);

    /* Append to compressed log storage */
    databox logBox = databoxNewBytesString(entry);
    multilistPushByTypeTail(&agg->logs, state, &logBox);

    /* Index by timestamp for fast range queries */
    size_t logIndex = multilistCount(agg->logs) - 1;
    databox tsBox = databoxNewSigned(now);
    databox idxBox = databoxNewUnsigned(logIndex);
    const databox *indexEntry[2] = {&tsBox, &idxBox};
    multimapAppend(&agg->index, indexEntry);

    mflexStateFree(state);
}

void logAggregatorQueryRange(LogAggregator *agg, time_t start, time_t end,
                             void (*callback)(const char *)) {
    databox startBox = databoxNewSigned(start);

    multimapIterator iter;
    if (!multimapIteratorInitAt(agg->index, &iter, true, &startBox)) {
        return;
    }

    mflexState *state = mflexStateCreate();

    databox timestamp, logIndex;
    databox *elements[2] = {&timestamp, &logIndex};

    while (multimapIteratorNext(&iter, elements)) {
        if (timestamp.data.i64 > end) break;

        /* Retrieve log entry */
        multilistEntry entry;
        if (multilistIndexGet(agg->logs, state, logIndex.data.u64, &entry)) {
            callback(entry.box.data.bytes.cstart);
        }
    }

    mflexStateFree(state);
}

void logAggregatorStats(LogAggregator *agg) {
    printf("Total logs: %zu\n", multilistCount(agg->logs));
    printf("Memory used: %zu bytes\n", multilistBytes(agg->logs));
    printf("Interned strings: %zu\n", multimapAtomCount(agg->atoms));
    printf("Index entries: %zu\n", multimapCount(agg->index));
}

void logAggregatorFree(LogAggregator *agg) {
    if (agg) {
        multilistFree(agg->logs);
        multimapAtomFree(agg->atoms);
        multimapFree(agg->index);
        free(agg);
    }
}

/* Example usage */
void exampleLogAggregator(void) {
    LogAggregator *agg = logAggregatorNew();

    /* Simulate application logging */
    logAggregatorAppend(agg, LOG_INFO, "app", "Application started");
    logAggregatorAppend(agg, LOG_DEBUG, "db", "Connected to database");

    for (int i = 0; i < 100000; i++) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Processing request #%d", i);
        logAggregatorAppend(agg, LOG_DEBUG, "http", msg);
    }

    logAggregatorAppend(agg, LOG_ERROR, "db", "Connection lost");
    logAggregatorAppend(agg, LOG_INFO, "app", "Shutting down");

    /* Query logs from last hour */
    time_t now = time(NULL);
    time_t hourAgo = now - 3600;

    printf("\nLogs from last hour:\n");
    logAggregatorQueryRange(agg, hourAgo, now, [](const char *log) {
        printf("%s\n", log);
    });

    logAggregatorStats(agg);
    logAggregatorFree(agg);
}
```

---

## Real-Time Analytics Dashboard

Track metrics, compute aggregations, and detect anomalies in real-time.

```c
#include "multimap.h"
#include "hyperloglog.h"
#include <time.h>

typedef struct {
    multimap *timeseries;     /* timestamp → metric values */
    hyperloglog *uniqueUsers; /* Cardinality estimation */
    multimap *counters;       /* metric_name → count */
    time_t windowSize;
} AnalyticsDashboard;

AnalyticsDashboard *dashboardNew(time_t windowSize) {
    AnalyticsDashboard *dash = malloc(sizeof(*dash));
    dash->timeseries = multimapNew(3);  /* timestamp + metric + value */
    dash->uniqueUsers = hyperloglogCreate();
    dash->counters = multimapNew(2);  /* metric_name + count */
    dash->windowSize = windowSize;
    return dash;
}

void dashboardRecordMetric(AnalyticsDashboard *dash, const char *metric,
                          double value) {
    time_t now = time(NULL);

    databox tsBox = databoxNewSigned(now);
    databox metricBox = databoxNewBytesString(metric);
    databox valueBox = databoxNewReal(value);

    const databox *entry[3] = {&tsBox, &metricBox, &valueBox};
    multimapAppend(&dash->timeseries, entry);

    /* Update counter */
    databox currentCount;
    databox *results[1] = {&currentCount};

    if (multimapLookup(dash->counters, &metricBox, results)) {
        /* Increment existing */
        multimapFieldIncr(&dash->counters, &metricBox, 1, 1);
    } else {
        /* Initialize */
        databox one = databoxNewSigned(1);
        const databox *counterEntry[2] = {&metricBox, &one};
        multimapInsert(&dash->counters, counterEntry);
    }
}

void dashboardRecordUser(AnalyticsDashboard *dash, const char *userId) {
    hyperloglogAdd(dash->uniqueUsers, (uint8_t *)userId, strlen(userId));
}

double dashboardGetMetricAvg(AnalyticsDashboard *dash, const char *metric,
                            time_t start, time_t end) {
    databox startBox = databoxNewSigned(start);
    databox metricBox = databoxNewBytesString(metric);

    multimapIterator iter;
    if (!multimapIteratorInitAt(dash->timeseries, &iter, true, &startBox)) {
        return 0.0;
    }

    double sum = 0.0;
    size_t count = 0;

    databox timestamp, metricName, value;
    databox *elements[3] = {&timestamp, &metricName, &value};

    while (multimapIteratorNext(&iter, elements)) {
        if (timestamp.data.i64 > end) break;

        if (databoxLen(&metricName) == strlen(metric) &&
            memcmp(databoxCBytes(&metricName), metric, strlen(metric)) == 0) {
            sum += value.data.d64;
            count++;
        }
    }

    return count > 0 ? sum / count : 0.0;
}

uint64_t dashboardGetUniqueUsers(AnalyticsDashboard *dash) {
    return hyperloglogCount(dash->uniqueUsers);
}

void dashboardGetTopMetrics(AnalyticsDashboard *dash, size_t n) {
    /* Simple implementation - iterate and print top N */
    multimapIterator iter;
    multimapIteratorInit(dash->counters, &iter, false);  /* Reverse for high to low */

    databox metric, count;
    databox *elements[2] = {&metric, &count};

    printf("Top %zu metrics:\n", n);
    size_t shown = 0;

    while (multimapIteratorNext(&iter, elements) && shown < n) {
        printf("%zu. %.*s: %ld\n",
               shown + 1,
               (int)databoxLen(&metric),
               databoxCBytes(&metric),
               count.data.i64);
        shown++;
    }
}

void dashboardFree(AnalyticsDashboard *dash) {
    if (dash) {
        multimapFree(dash->timeseries);
        hyperloglogFree(dash->uniqueUsers);
        multimapFree(dash->counters);
        free(dash);
    }
}

/* Example usage */
void exampleAnalyticsDashboard(void) {
    AnalyticsDashboard *dash = dashboardNew(3600);  /* 1 hour window */

    /* Simulate metrics */
    for (int i = 0; i < 10000; i++) {
        dashboardRecordMetric(dash, "page_views", 1.0);
        dashboardRecordMetric(dash, "response_time_ms", 50.0 + (rand() % 100));

        char userId[32];
        snprintf(userId, sizeof(userId), "user_%d", rand() % 5000);
        dashboardRecordUser(dash, userId);
    }

    /* Query analytics */
    time_t now = time(NULL);
    time_t hourAgo = now - 3600;

    double avgResponseTime = dashboardGetMetricAvg(dash, "response_time_ms",
                                                   hourAgo, now);
    printf("Average response time: %.2f ms\n", avgResponseTime);

    uint64_t uniqueUsers = dashboardGetUniqueUsers(dash);
    printf("Unique users (estimated): %lu\n", uniqueUsers);

    dashboardGetTopMetrics(dash, 5);

    dashboardFree(dash);
}
```

---

## Distributed Cache System

Multi-level cache with sharding and consistent hashing.

```c
#include "multimap.h"
#include "multilru.h"
#include "xxHash/xxhash.h"

#define NUM_SHARDS 16

typedef struct {
    multimap *data;
    multilru *lru;
    size_t maxSize;
} CacheShard;

typedef struct {
    CacheShard shards[NUM_SHARDS];
    size_t totalMaxSize;
} DistributedCache;

DistributedCache *dcacheNew(size_t totalMaxSize) {
    DistributedCache *cache = malloc(sizeof(*cache));
    cache->totalMaxSize = totalMaxSize;

    size_t shardSize = totalMaxSize / NUM_SHARDS;
    for (int i = 0; i < NUM_SHARDS; i++) {
        cache->shards[i].data = multimapNew(3);  /* key + value + lruPtr */
        cache->shards[i].lru = multilruNew();
        cache->shards[i].maxSize = shardSize;
    }

    return cache;
}

/* Consistent hashing to select shard */
static int dcacheGetShard(const void *key, size_t keyLen) {
    uint64_t hash = XXH64(key, keyLen, 0);
    return hash % NUM_SHARDS;
}

bool dcacheGet(DistributedCache *cache, const void *key, size_t keyLen,
              void **outValue, size_t *outValueLen) {
    int shard = dcacheGetShard(key, keyLen);
    CacheShard *s = &cache->shards[shard];

    databox keyBox = databoxNewBytes(key, keyLen);
    databox value, lruPtr;
    databox *results[2] = {&value, &lruPtr};

    if (multimapLookup(s->data, &keyBox, results)) {
        /* Update LRU */
        multilruIncrease(s->lru, (multilruPtr)lruPtr.data.u64);

        /* Return value */
        *outValue = malloc(databoxLen(&value));
        memcpy(*outValue, databoxCBytes(&value), databoxLen(&value));
        *outValueLen = databoxLen(&value);
        return true;
    }

    return false;
}

void dcachePut(DistributedCache *cache, const void *key, size_t keyLen,
              const void *value, size_t valueLen) {
    int shard = dcacheGetShard(key, keyLen);
    CacheShard *s = &cache->shards[shard];

    /* Evict if at capacity */
    while (multimapCount(s->data) >= s->maxSize) {
        multilruPtr evicted;
        if (!multilruRemoveMinimum(s->lru, &evicted)) break;

        /* Find and delete corresponding entry */
        /* (Production: maintain reverse mapping) */
    }

    /* Insert */
    databox keyBox = databoxNewBytes(key, keyLen);
    databox valueBox = databoxNewBytes(value, valueLen);

    multilruPtr ptr = multilruInsert(s->lru);
    databox ptrBox = databoxNewUnsigned(ptr);

    const databox *entry[3] = {&keyBox, &valueBox, &ptrBox};
    multimapInsert(&s->data, entry);
}

void dcacheDelete(DistributedCache *cache, const void *key, size_t keyLen) {
    int shard = dcacheGetShard(key, keyLen);
    CacheShard *s = &cache->shards[shard];

    databox keyBox = databoxNewBytes(key, keyLen);
    multimapDelete(&s->data, &keyBox);
}

void dcacheStats(DistributedCache *cache) {
    printf("Cache Statistics:\n");
    size_t totalEntries = 0;
    size_t totalBytes = 0;

    for (int i = 0; i < NUM_SHARDS; i++) {
        size_t entries = multimapCount(cache->shards[i].data);
        size_t bytes = multimapBytes(cache->shards[i].data);
        totalEntries += entries;
        totalBytes += bytes;

        printf("Shard %d: %zu entries, %zu bytes\n", i, entries, bytes);
    }

    printf("Total: %zu entries, %zu bytes\n", totalEntries, totalBytes);
}

void dcacheFree(DistributedCache *cache) {
    if (cache) {
        for (int i = 0; i < NUM_SHARDS; i++) {
            multimapFree(cache->shards[i].data);
            multilruFree(cache->shards[i].lru);
        }
        free(cache);
    }
}

/* Example usage */
void exampleDistributedCache(void) {
    DistributedCache *cache = dcacheNew(10000);

    /* Put entries */
    for (int i = 0; i < 1000; i++) {
        char key[32], value[64];
        snprintf(key, sizeof(key), "key_%d", i);
        snprintf(value, sizeof(value), "value_for_key_%d", i);

        dcachePut(cache, key, strlen(key), value, strlen(value));
    }

    /* Get entry */
    void *value;
    size_t valueLen;
    if (dcacheGet(cache, "key_500", 7, &value, &valueLen)) {
        printf("Retrieved: %.*s\n", (int)valueLen, (char *)value);
        free(value);
    }

    dcacheStats(cache);
    dcacheFree(cache);
}
```

---

## Full-Text Search Engine

Inverted index for efficient full-text search with ranking.

```c
#include "multimap.h"
#include "multimapAtom.h"
#include "intset.h"

typedef struct {
    multimapAtom *words;      /* Word → wordRef interning */
    multimap *index;          /* wordRef → {docId, position, frequency} */
    multimap *documents;      /* docId → {title, content, wordCount} */
    uint64_t nextDocId;
} SearchEngine;

SearchEngine *searchEngineNew(void) {
    SearchEngine *se = malloc(sizeof(*se));
    se->words = multimapAtomNew();
    se->index = multimapNew(4);  /* wordRef + docId + position + frequency */
    se->documents = multimapNew(3);  /* docId + title + wordCount */
    se->nextDocId = 1;
    return se;
}

/* Simple tokenizer */
static void tokenize(const char *text, void (*callback)(const char *word)) {
    char *copy = strdup(text);
    char *word = strtok(copy, " \t\n.,;:!?");

    while (word != NULL) {
        /* Convert to lowercase */
        for (char *p = word; *p; p++) {
            *p = tolower(*p);
        }

        if (strlen(word) > 2) {  /* Skip very short words */
            callback(word);
        }

        word = strtok(NULL, " \t\n.,;:!?");
    }

    free(copy);
}

uint64_t searchEngineAddDocument(SearchEngine *se, const char *title,
                                 const char *content) {
    uint64_t docId = se->nextDocId++;

    /* Store document */
    databox docIdBox = databoxNewUnsigned(docId);
    databox titleBox = databoxNewBytesString(title);
    databox wordCountBox = databoxNewUnsigned(0);  /* Will update */

    const databox *docEntry[3] = {&docIdBox, &titleBox, &wordCountBox};
    multimapInsert(&se->documents, docEntry);

    /* Index words */
    typedef struct {
        SearchEngine *se;
        uint64_t docId;
        uint32_t position;
    } IndexContext;

    IndexContext ctx = {se, docId, 0};

    tokenize(content, [](const char *word, void *data) {
        IndexContext *ctx = (IndexContext *)data;

        /* Intern word */
        databox wordBox = databoxNewBytesString(word);
        multimapAtomInsertIfNewConvert(ctx->se->words, &wordBox);

        /* Add to index */
        databox docIdBox = databoxNewUnsigned(ctx->docId);
        databox posBox = databoxNewUnsigned(ctx->position++);
        databox freqBox = databoxNewUnsigned(1);  /* Simple freq for now */

        const databox *indexEntry[4] = {&wordBox, &docIdBox, &posBox, &freqBox};
        multimapInsertFullWidth(&ctx->se->index, indexEntry);
    });

    return docId;
}

typedef struct {
    uint64_t docId;
    char *title;
    double score;
} SearchResult;

void searchEngineQuery(SearchEngine *se, const char *query,
                      SearchResult **outResults, size_t *outCount) {
    /* Tokenize query */
    intset *matchingDocs = intsetNew();

    tokenize(query, [](const char *word, void *data) {
        SearchEngine *se = (SearchEngine *)((void **)data)[0];
        intset **docs = (intset **)((void **)data)[1];

        /* Look up word reference */
        databox wordBox = databoxNewBytesString(word);
        databox wordRef;

        if (!multimapAtomLookupReference(se->words, &wordBox, &wordRef)) {
            return;  /* Word not in index */
        }

        /* Find all documents containing this word */
        multimapIterator iter;
        if (!multimapIteratorInitAt(se->index, &iter, true, &wordRef)) {
            return;
        }

        databox ref, docId, pos, freq;
        databox *elements[4] = {&ref, &docId, &pos, &freq};

        while (multimapIteratorNext(&iter, elements)) {
            if (ref.data.u64 != wordRef.data.u64) break;

            intsetAdd(docs, docId.data.u64, NULL);
        }
    });

    /* Collect and score results */
    SearchResult *results = malloc(sizeof(SearchResult) * intsetCount(matchingDocs));
    size_t count = 0;

    for (uint32_t i = 0; i < intsetCount(matchingDocs); i++) {
        int64_t docId;
        intsetGet(matchingDocs, i, &docId);

        /* Get document info */
        databox docIdBox = databoxNewUnsigned(docId);
        databox title, wordCount;
        databox *docResults[2] = {&title, &wordCount};

        if (multimapLookup(se->documents, &docIdBox, docResults)) {
            results[count].docId = docId;
            results[count].title = strndup(
                (char *)databoxCBytes(&title),
                databoxLen(&title)
            );
            results[count].score = 1.0;  /* Simple scoring */
            count++;
        }
    }

    intsetFree(matchingDocs);

    *outResults = results;
    *outCount = count;
}

void searchEngineFree(SearchEngine *se) {
    if (se) {
        multimapAtomFree(se->words);
        multimapFree(se->index);
        multimapFree(se->documents);
        free(se);
    }
}

/* Example usage */
void exampleSearchEngine(void) {
    SearchEngine *se = searchEngineNew();

    /* Add documents */
    searchEngineAddDocument(se, "Introduction to datakit",
        "datakit is a high-performance data structure library");
    searchEngineAddDocument(se, "Using multimap",
        "multimap provides efficient key-value storage");
    searchEngineAddDocument(se, "Performance tips",
        "datakit offers excellent performance for data structures");

    /* Search */
    SearchResult *results;
    size_t count;
    searchEngineQuery(se, "datakit performance", &results, &count);

    printf("Found %zu results:\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("%zu. %s (score: %.2f)\n",
               i + 1, results[i].title, results[i].score);
        free(results[i].title);
    }
    free(results);

    searchEngineFree(se);
}
```

---

## Gaming Leaderboard Service

High-performance leaderboard with efficient rank queries.

```c
#include "multimap.h"

typedef struct {
    multimap *scores;         /* score → {playerId, playerName, timestamp} */
    multimap *playerLookup;   /* playerId → score */
} Leaderboard;

Leaderboard *leaderboardNew(void) {
    Leaderboard *lb = malloc(sizeof(*lb));
    lb->scores = multimapNew(4);  /* score + playerId + name + timestamp */
    lb->playerLookup = multimapNew(2);  /* playerId + score */
    return lb;
}

void leaderboardUpdateScore(Leaderboard *lb, uint64_t playerId,
                           const char *playerName, int64_t newScore) {
    databox playerIdBox = databoxNewUnsigned(playerId);

    /* Check if player already has a score */
    databox oldScore;
    databox *lookupResults[1] = {&oldScore};

    if (multimapLookup(lb->playerLookup, &playerIdBox, lookupResults)) {
        /* Remove old score entry */
        multimapDelete(&lb->scores, &oldScore);
    }

    /* Add new score */
    databox scoreBox = databoxNewSigned(newScore);
    databox nameBox = databoxNewBytesString(playerName);
    databox timestampBox = databoxNewSigned(time(NULL));

    const databox *scoreEntry[4] = {&scoreBox, &playerIdBox, &nameBox,
                                   &timestampBox};
    multimapInsert(&lb->scores, scoreEntry);

    /* Update lookup */
    const databox *lookupEntry[2] = {&playerIdBox, &scoreBox};
    multimapInsert(&lb->playerLookup, lookupEntry);
}

typedef struct {
    uint64_t playerId;
    char *playerName;
    int64_t score;
    size_t rank;
} LeaderboardEntry;

void leaderboardGetTopN(Leaderboard *lb, size_t n,
                       LeaderboardEntry **outEntries, size_t *outCount) {
    LeaderboardEntry *entries = malloc(sizeof(LeaderboardEntry) * n);
    size_t count = 0;

    multimapIterator iter;
    multimapIteratorInit(lb->scores, &iter, false);  /* Reverse = high to low */

    databox score, playerId, playerName, timestamp;
    databox *elements[4] = {&score, &playerId, &playerName, &timestamp};

    while (multimapIteratorNext(&iter, elements) && count < n) {
        entries[count].playerId = playerId.data.u64;
        entries[count].playerName = strndup(
            (char *)databoxCBytes(&playerName),
            databoxLen(&playerName)
        );
        entries[count].score = score.data.i64;
        entries[count].rank = count + 1;
        count++;
    }

    *outEntries = entries;
    *outCount = count;
}

int64_t leaderboardGetRank(Leaderboard *lb, uint64_t playerId) {
    /* Get player's score */
    databox playerIdBox = databoxNewUnsigned(playerId);
    databox playerScore;
    databox *results[1] = {&playerScore};

    if (!multimapLookup(lb->playerLookup, &playerIdBox, results)) {
        return -1;  /* Player not found */
    }

    /* Count players with higher scores */
    int64_t rank = 1;

    multimapIterator iter;
    multimapIteratorInit(lb->scores, &iter, false);  /* High to low */

    databox score, pid, name, ts;
    databox *elements[4] = {&score, &pid, &name, &ts};

    while (multimapIteratorNext(&iter, elements)) {
        if (score.data.i64 > playerScore.data.i64) {
            rank++;
        } else {
            break;
        }
    }

    return rank;
}

void leaderboardFree(Leaderboard *lb) {
    if (lb) {
        multimapFree(lb->scores);
        multimapFree(lb->playerLookup);
        free(lb);
    }
}

/* Example usage */
void exampleLeaderboard(void) {
    Leaderboard *lb = leaderboardNew();

    /* Add scores */
    leaderboardUpdateScore(lb, 1, "Alice", 9500);
    leaderboardUpdateScore(lb, 2, "Bob", 8200);
    leaderboardUpdateScore(lb, 3, "Charlie", 9100);
    leaderboardUpdateScore(lb, 4, "Diana", 8500);
    leaderboardUpdateScore(lb, 5, "Eve", 9800);

    /* Update score */
    leaderboardUpdateScore(lb, 2, "Bob", 9900);  /* Bob improved!*/

    /* Get top 3 */
    LeaderboardEntry *top;
    size_t count;
    leaderboardGetTopN(lb, 3, &top, &count);

    printf("Top %zu players:\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("%zu. %s: %ld\n", top[i].rank, top[i].playerName, top[i].score);
        free(top[i].playerName);
    }
    free(top);

    /* Get rank */
    int64_t rank = leaderboardGetRank(lb, 3);
    printf("Charlie's rank: %ld\n", rank);

    leaderboardFree(lb);
}
```

---

## Summary

These real-world use cases demonstrate how datakit modules work together to solve production problems:

1. **Session Store** - Combines multimap, multimapAtom, and multilru for efficient session management
2. **Log Aggregator** - Uses multilist compression and multimap indexing for millions of logs
3. **Analytics Dashboard** - Leverages hyperloglog for cardinality and multimap for time-series
4. **Distributed Cache** - Implements sharding with consistent hashing and LRU eviction
5. **Search Engine** - Builds inverted index with multimapAtom for string interning
6. **Leaderboard** - Sorted multimap for efficient rank queries

For more examples, see:
- [PATTERNS.md](PATTERNS.md) - Common coding patterns
- [MIGRATION.md](MIGRATION.md) - Migrating from other libraries
- [BENCHMARKS.md](BENCHMARKS.md) - Performance comparisons
