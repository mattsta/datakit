# multimap Real-World Examples

## Overview

This guide demonstrates practical applications of the multimap family through real-world examples. Each example includes complete, working code that you can adapt to your own projects.

## Example 1: User Session Store

A session store that maps session IDs to user data with automatic expiration.

```c
#include "multimap.h"
#include <time.h>

typedef struct {
    uint64_t sessionId;
    char *username;
    time_t lastAccess;
    bool isActive;
} Session;

typedef struct {
    multimap *sessions;  /* sessionId → {username, lastAccess, isActive} */
} SessionStore;

SessionStore *sessionStoreNew(void) {
    SessionStore *store = malloc(sizeof(*store));
    /* 4 elements: sessionId (key) + username + lastAccess + isActive */
    store->sessions = multimapNew(4);
    return store;
}

void sessionStoreAdd(SessionStore *store, Session *session) {
    databox sid = databoxNewUnsigned(session->sessionId);
    databox user = databoxNewBytesString(session->username);
    databox access = databoxNewSigned(session->lastAccess);
    databox active = databoxBool(session->isActive);

    const databox *elements[4] = {&sid, &user, &access, &active};
    multimapInsert(&store->sessions, elements);
}

bool sessionStoreGet(SessionStore *store, uint64_t sessionId,
                    Session *outSession) {
    databox sid = databoxNewUnsigned(sessionId);
    databox user, access, active;
    databox *results[3] = {&user, &access, &active};

    if (multimapLookup(store->sessions, &sid, results)) {
        outSession->sessionId = sessionId;
        outSession->username = strndup(
            (char *)databoxCBytes(&user), databoxLen(&user)
        );
        outSession->lastAccess = access.data.i64;
        outSession->isActive = DATABOX_IS_TRUE(&active);
        return true;
    }

    return false;
}

void sessionStoreUpdate(SessionStore *store, uint64_t sessionId) {
    databox sid = databoxNewUnsigned(sessionId);
    multimapEntry me;

    if (multimapGetUnderlyingEntry(store->sessions, &sid, &me)) {
        /* Update lastAccess timestamp in-place */
        flexEntry *accessField = flexNext(*me.map, flexNext(*me.map, me.fe));
        databox newTime = databoxNewSigned(time(NULL));
        flexReplaceByType(me.map, accessField, &newTime);
    }
}

void sessionStoreExpireOld(SessionStore *store, time_t cutoffTime) {
    multimapIterator iter;
    multimapIteratorInit(store->sessions, &iter, true);

    databox sid, user, lastAccess, active;
    databox *elements[4] = {&sid, &user, &lastAccess, &active};

    /* Collect expired session IDs */
    databox expiredIds[1000];
    size_t expiredCount = 0;

    while (multimapIteratorNext(&iter, elements)) {
        if (lastAccess.data.i64 < cutoffTime) {
            expiredIds[expiredCount++] = sid;
            if (expiredCount >= 1000) break;
        }
    }

    /* Delete expired sessions */
    for (size_t i = 0; i < expiredCount; i++) {
        multimapDelete(&store->sessions, &expiredIds[i]);
    }

    printf("Expired %zu sessions\n", expiredCount);
}

void sessionStoreFree(SessionStore *store) {
    if (store) {
        multimapFree(store->sessions);
        free(store);
    }
}

/* Usage */
void exampleSessionStore(void) {
    SessionStore *store = sessionStoreNew();

    /* Add sessions */
    Session s1 = {.sessionId = 12345, .username = "alice",
                  .lastAccess = time(NULL), .isActive = true};
    sessionStoreAdd(store, &s1);

    Session s2 = {.sessionId = 67890, .username = "bob",
                  .lastAccess = time(NULL) - 3600, .isActive = true};
    sessionStoreAdd(store, &s2);

    /* Retrieve session */
    Session retrieved;
    if (sessionStoreGet(store, 12345, &retrieved)) {
        printf("Session 12345: user=%s, active=%s\n",
               retrieved.username, retrieved.isActive ? "yes" : "no");
        free(retrieved.username);
    }

    /* Update access time */
    sessionStoreUpdate(store, 12345);

    /* Expire old sessions (older than 30 minutes) */
    sessionStoreExpireOld(store, time(NULL) - 1800);

    sessionStoreFree(store);
}
```

## Example 2: Time-Series Metrics

Store and query time-series metrics with efficient range queries.

```c
#include "multimap.h"
#include <time.h>

typedef struct {
    multimap *metrics;  /* timestamp → {value, tags} */
    char *metricName;
} TimeSeries;

TimeSeries *timeSeriesNew(const char *name) {
    TimeSeries *ts = malloc(sizeof(*ts));
    /* 3 elements: timestamp (key) + value + tags */
    ts->metrics = multimapNew(3);
    ts->metricName = strdup(name);
    return ts;
}

void timeSeriesRecord(TimeSeries *ts, time_t timestamp,
                     double value, const char *tags) {
    databox ts_box = databoxNewSigned(timestamp);
    databox val_box = databoxNewReal(value);
    databox tag_box = databoxNewBytesString(tags);

    const databox *elements[3] = {&ts_box, &val_box, &tag_box};

    /* Use InsertFullWidth to allow multiple readings per timestamp */
    multimapInsertFullWidth(&ts->metrics, elements);
}

void timeSeriesRecordBatch(TimeSeries *ts, time_t *timestamps,
                          double *values, size_t count) {
    for (size_t i = 0; i < count; i++) {
        databox ts_box = databoxNewSigned(timestamps[i]);
        databox val_box = databoxNewReal(values[i]);
        databox tag_box = databoxNewBytesString("");

        const databox *elements[3] = {&ts_box, &val_box, &tag_box};

        /* Use Append for chronologically sorted data (much faster!) */
        multimapAppend(&ts->metrics, elements);
    }
}

typedef struct {
    time_t timestamp;
    double value;
    char *tags;
} Metric;

void timeSeriesQuery(TimeSeries *ts, time_t startTime, time_t endTime,
                    Metric **outMetrics, size_t *outCount) {
    multimapIterator iter;
    databox start = databoxNewSigned(startTime);

    /* Start iteration at startTime */
    if (!multimapIteratorInitAt(ts->metrics, &iter, true, &start)) {
        /* No data in range */
        *outMetrics = NULL;
        *outCount = 0;
        return;
    }

    /* Collect matching metrics */
    Metric *results = malloc(sizeof(Metric) * 10000);
    size_t count = 0;

    databox timestamp, value, tags;
    databox *elements[3] = {&timestamp, &value, &tags};

    while (multimapIteratorNext(&iter, elements)) {
        time_t ts = timestamp.data.i64;

        if (ts > endTime) {
            break;  /* Past our range */
        }

        results[count].timestamp = ts;
        results[count].value = value.data.d64;
        results[count].tags = strndup(
            (char *)databoxCBytes(&tags), databoxLen(&tags)
        );
        count++;

        if (count >= 10000) break;
    }

    *outMetrics = results;
    *outCount = count;
}

double timeSeriesAggregate(TimeSeries *ts, time_t startTime,
                          time_t endTime, const char *aggregation) {
    Metric *metrics;
    size_t count;
    timeSeriesQuery(ts, startTime, endTime, &metrics, &count);

    if (count == 0) {
        return 0.0;
    }

    double result = 0.0;

    if (strcmp(aggregation, "sum") == 0) {
        for (size_t i = 0; i < count; i++) {
            result += metrics[i].value;
        }
    } else if (strcmp(aggregation, "avg") == 0) {
        double sum = 0.0;
        for (size_t i = 0; i < count; i++) {
            sum += metrics[i].value;
        }
        result = sum / count;
    } else if (strcmp(aggregation, "max") == 0) {
        result = metrics[0].value;
        for (size_t i = 1; i < count; i++) {
            if (metrics[i].value > result) {
                result = metrics[i].value;
            }
        }
    } else if (strcmp(aggregation, "min") == 0) {
        result = metrics[0].value;
        for (size_t i = 1; i < count; i++) {
            if (metrics[i].value < result) {
                result = metrics[i].value;
            }
        }
    }

    /* Free allocated strings */
    for (size_t i = 0; i < count; i++) {
        free(metrics[i].tags);
    }
    free(metrics);

    return result;
}

void timeSeriesFree(TimeSeries *ts) {
    if (ts) {
        multimapFree(ts->metrics);
        free(ts->metricName);
        free(ts);
    }
}

/* Usage */
void exampleTimeSeries(void) {
    TimeSeries *cpu = timeSeriesNew("cpu.usage");

    /* Record individual measurements */
    time_t now = time(NULL);
    timeSeriesRecord(cpu, now, 45.2, "host=server1");
    timeSeriesRecord(cpu, now + 60, 52.1, "host=server1");
    timeSeriesRecord(cpu, now + 120, 48.9, "host=server1");

    /* Record batch (much faster for bulk inserts) */
    time_t timestamps[1000];
    double values[1000];
    for (int i = 0; i < 1000; i++) {
        timestamps[i] = now + (i * 60);  /* Every minute */
        values[i] = 40.0 + (rand() % 30);  /* 40-70% CPU */
    }
    timeSeriesRecordBatch(cpu, timestamps, values, 1000);

    /* Query last hour */
    time_t hourAgo = now - 3600;
    Metric *metrics;
    size_t count;
    timeSeriesQuery(cpu, hourAgo, now + 3600, &metrics, &count);

    printf("Found %zu metrics in last hour\n", count);

    /* Aggregate last hour */
    double avgCpu = timeSeriesAggregate(cpu, hourAgo, now + 3600, "avg");
    double maxCpu = timeSeriesAggregate(cpu, hourAgo, now + 3600, "max");

    printf("Average CPU: %.2f%%\n", avgCpu);
    printf("Max CPU: %.2f%%\n", maxCpu);

    /* Free */
    for (size_t i = 0; i < count; i++) {
        free(metrics[i].tags);
    }
    free(metrics);
    timeSeriesFree(cpu);
}
```

## Example 3: Inverted Index for Full-Text Search

Build an inverted index mapping words to document IDs.

```c
#include "multimap.h"
#include "multimapAtom.h"

typedef struct {
    multimapAtom *atoms;     /* Word interning for deduplication */
    multimap *index;         /* wordRef → {docId, position} */
} InvertedIndex;

InvertedIndex *invertedIndexNew(void) {
    InvertedIndex *idx = malloc(sizeof(*idx));
    idx->atoms = multimapAtomNew();
    /* 3 elements: wordRef (key) + docId + position */
    idx->index = multimapNew(3);
    return idx;
}

void invertedIndexAddDocument(InvertedIndex *idx, uint64_t docId,
                              const char *text) {
    /* Simple tokenization (split on spaces) */
    char *textCopy = strdup(text);
    char *word = strtok(textCopy, " \t\n.,;:!?");
    uint32_t position = 0;

    while (word != NULL) {
        /* Convert to lowercase */
        for (char *p = word; *p; p++) {
            *p = tolower(*p);
        }

        /* Intern the word to get a reference ID */
        databox wordBox = databoxNewBytesString(word);
        multimapAtomInsertIfNewConvert(idx->atoms, &wordBox);
        /* wordBox is now a small integer reference */

        /* Add to index: wordRef → {docId, position} */
        databox docBox = databoxNewUnsigned(docId);
        databox posBox = databoxNewUnsigned(position);
        const databox *entry[3] = {&wordBox, &docBox, &posBox};

        multimapInsertFullWidth(&idx->index, entry);

        position++;
        word = strtok(NULL, " \t\n.,;:!?");
    }

    free(textCopy);
}

typedef struct {
    uint64_t docId;
    uint32_t position;
} SearchResult;

void invertedIndexSearch(InvertedIndex *idx, const char *searchWord,
                        SearchResult **outResults, size_t *outCount) {
    /* Convert search word to lowercase */
    char *lower = strdup(searchWord);
    for (char *p = lower; *p; p++) {
        *p = tolower(*p);
    }

    /* Look up word reference */
    databox wordBox = databoxNewBytesString(lower);
    databox wordRef;

    if (!multimapAtomLookupReference(idx->atoms, &wordBox, &wordRef)) {
        /* Word not in index */
        free(lower);
        *outResults = NULL;
        *outCount = 0;
        return;
    }

    free(lower);

    /* Find all occurrences of this word */
    multimapIterator iter;
    if (!multimapIteratorInitAt(idx->index, &iter, true, &wordRef)) {
        *outResults = NULL;
        *outCount = 0;
        return;
    }

    SearchResult *results = malloc(sizeof(SearchResult) * 10000);
    size_t count = 0;

    databox keyRef, docId, position;
    databox *elements[3] = {&keyRef, &docId, &position};

    while (multimapIteratorNext(&iter, elements)) {
        /* Stop when we move past this word */
        if (keyRef.data.u != wordRef.data.u) {
            break;
        }

        results[count].docId = docId.data.u;
        results[count].position = position.data.u;
        count++;

        if (count >= 10000) break;
    }

    *outResults = results;
    *outCount = count;
}

void invertedIndexFree(InvertedIndex *idx) {
    if (idx) {
        multimapAtomFree(idx->atoms);
        multimapFree(idx->index);
        free(idx);
    }
}

/* Usage */
void exampleInvertedIndex(void) {
    InvertedIndex *idx = invertedIndexNew();

    /* Add documents */
    invertedIndexAddDocument(idx, 1, "the quick brown fox jumps over the lazy dog");
    invertedIndexAddDocument(idx, 2, "the lazy cat sleeps all day");
    invertedIndexAddDocument(idx, 3, "quick reflexes help the fox");

    /* Search for "lazy" */
    SearchResult *results;
    size_t count;
    invertedIndexSearch(idx, "lazy", &results, &count);

    printf("Found 'lazy' in %zu locations:\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("  Document %lu, position %u\n",
               results[i].docId, results[i].position);
    }
    free(results);

    /* Search for "fox" */
    invertedIndexSearch(idx, "fox", &results, &count);
    printf("Found 'fox' in %zu locations:\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("  Document %lu, position %u\n",
               results[i].docId, results[i].position);
    }
    free(results);

    invertedIndexFree(idx);
}
```

## Example 4: Leaderboard with Rankings

Implement a game leaderboard with efficient top-N queries.

```c
#include "multimap.h"

typedef struct {
    multimap *scores;  /* score (key) → {playerId, playerName} */
} Leaderboard;

Leaderboard *leaderboardNew(void) {
    Leaderboard *lb = malloc(sizeof(*lb));
    /* 3 elements: score (key) + playerId + playerName */
    /* Use SetNew to prevent duplicate score entries for same player */
    lb->scores = multimapNew(3);
    return lb;
}

void leaderboardAddScore(Leaderboard *lb, uint64_t playerId,
                        const char *playerName, int64_t score) {
    databox scoreBox = databoxNewSigned(score);
    databox idBox = databoxNewUnsigned(playerId);
    databox nameBox = databoxNewBytesString(playerName);

    const databox *entry[3] = {&scoreBox, &idBox, &nameBox};

    /* Insert or update score */
    multimapInsert(&lb->scores, entry);
}

void leaderboardUpdateScore(Leaderboard *lb, uint64_t playerId,
                           const char *playerName, int64_t scoreDelta) {
    /* In a real implementation, you'd need to:
     * 1. Find old score for playerId
     * 2. Delete old entry
     * 3. Insert new entry with updated score
     * For simplicity, we'll just add a new score */
    leaderboardAddScore(lb, playerId, playerName, scoreDelta);
}

typedef struct {
    uint64_t playerId;
    char *playerName;
    int64_t score;
} LeaderboardEntry;

void leaderboardGetTopN(Leaderboard *lb, size_t n,
                       LeaderboardEntry **outEntries, size_t *outCount) {
    LeaderboardEntry *entries = malloc(sizeof(LeaderboardEntry) * n);
    size_t count = 0;

    multimapIterator iter;
    multimapIteratorInit(lb->scores, &iter, false);  /* Reverse = high to low */

    databox score, playerId, playerName;
    databox *elements[3] = {&score, &playerId, &playerName};

    while (multimapIteratorNext(&iter, elements) && count < n) {
        entries[count].score = score.data.i64;
        entries[count].playerId = playerId.data.u;
        entries[count].playerName = strndup(
            (char *)databoxCBytes(&playerName), databoxLen(&playerName)
        );
        count++;
    }

    *outEntries = entries;
    *outCount = count;
}

int64_t leaderboardGetRank(Leaderboard *lb, int64_t targetScore) {
    /* Count how many scores are higher */
    int64_t rank = 1;

    multimapIterator iter;
    multimapIteratorInit(lb->scores, &iter, false);  /* High to low */

    databox score, playerId, playerName;
    databox *elements[3] = {&score, &playerId, &playerName};

    while (multimapIteratorNext(&iter, elements)) {
        if (score.data.i64 > targetScore) {
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
        free(lb);
    }
}

/* Usage */
void exampleLeaderboard(void) {
    Leaderboard *lb = leaderboardNew();

    /* Add scores */
    leaderboardAddScore(lb, 1, "Alice", 9500);
    leaderboardAddScore(lb, 2, "Bob", 8200);
    leaderboardAddScore(lb, 3, "Charlie", 7800);
    leaderboardAddScore(lb, 4, "Diana", 9100);
    leaderboardAddScore(lb, 5, "Eve", 8500);

    /* Get top 3 */
    LeaderboardEntry *top;
    size_t count;
    leaderboardGetTopN(lb, 3, &top, &count);

    printf("Top %zu players:\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("%zu. %s: %ld\n", i + 1, top[i].playerName, top[i].score);
        free(top[i].playerName);
    }
    free(top);

    /* Get rank of score 8500 */
    int64_t rank = leaderboardGetRank(lb, 8500);
    printf("Score 8500 is rank %ld\n", rank);

    leaderboardFree(lb);
}
```

## Example 5: LRU Cache with multimap

Implement a Least Recently Used (LRU) cache.

```c
#include "multimap.h"
#include <time.h>

typedef struct {
    multimap *cache;  /* key → {value, lastAccess} */
    size_t maxSize;
    size_t currentSize;
} LRUCache;

LRUCache *lruCacheNew(size_t maxSize) {
    LRUCache *cache = malloc(sizeof(*cache));
    /* 3 elements: key + value + lastAccess timestamp */
    cache->cache = multimapNew(3);
    cache->maxSize = maxSize;
    cache->currentSize = 0;
    return cache;
}

void lruCacheEvictOldest(LRUCache *cache) {
    /* Find entry with smallest lastAccess timestamp */
    multimapIterator iter;
    multimapIteratorInit(cache->cache, &iter, true);

    databox key, value, lastAccess;
    databox *elements[3] = {&key, &value, &lastAccess};

    databox oldestKey;
    time_t oldestTime = LONG_MAX;

    while (multimapIteratorNext(&iter, elements)) {
        if (lastAccess.data.i64 < oldestTime) {
            oldestTime = lastAccess.data.i64;
            oldestKey = key;
        }
    }

    /* Delete oldest entry */
    if (oldestTime != LONG_MAX) {
        multimapDelete(&cache->cache, &oldestKey);
        cache->currentSize--;
    }
}

void lruCachePut(LRUCache *cache, const databox *key, const databox *value) {
    /* Check if key exists */
    if (multimapExists(cache->cache, key)) {
        /* Update existing entry */
        multimapEntry me;
        if (multimapGetUnderlyingEntry(cache->cache, key, &me)) {
            /* Update value and timestamp */
            flexEntry *valueField = flexNext(*me.map, me.fe);
            flexReplaceByType(me.map, valueField, value);

            flexEntry *timeField = flexNext(*me.map, valueField);
            databox now = databoxNewSigned(time(NULL));
            flexReplaceByType(me.map, timeField, &now);
        }
    } else {
        /* New entry */
        if (cache->currentSize >= cache->maxSize) {
            lruCacheEvictOldest(cache);
        }

        databox now = databoxNewSigned(time(NULL));
        const databox *entry[3] = {key, value, &now};
        multimapInsert(&cache->cache, entry);
        cache->currentSize++;
    }
}

bool lruCacheGet(LRUCache *cache, const databox *key, databox *outValue) {
    databox value, lastAccess;
    databox *results[2] = {&value, &lastAccess};

    if (multimapLookup(cache->cache, key, results)) {
        *outValue = value;

        /* Update last access time */
        multimapEntry me;
        if (multimapGetUnderlyingEntry(cache->cache, key, &me)) {
            flexEntry *timeField = flexNext(*me.map,
                                           flexNext(*me.map, me.fe));
            databox now = databoxNewSigned(time(NULL));
            flexReplaceByType(me.map, timeField, &now);
        }

        return true;
    }

    return false;
}

void lruCacheFree(LRUCache *cache) {
    if (cache) {
        multimapFree(cache->cache);
        free(cache);
    }
}

/* Usage */
void exampleLRUCache(void) {
    LRUCache *cache = lruCacheNew(3);  /* Max 3 entries */

    /* Put entries */
    databox k1 = databoxNewBytesString("key1");
    databox v1 = databoxNewSigned(100);
    lruCachePut(cache, &k1, &v1);

    databox k2 = databoxNewBytesString("key2");
    databox v2 = databoxNewSigned(200);
    lruCachePut(cache, &k2, &v2);

    databox k3 = databoxNewBytesString("key3");
    databox v3 = databoxNewSigned(300);
    lruCachePut(cache, &k3, &v3);

    /* Cache is full (3/3) */

    /* Access key1 (updates its timestamp) */
    databox retrieved;
    if (lruCacheGet(cache, &k1, &retrieved)) {
        printf("Retrieved: %ld\n", retrieved.data.i64);
    }

    sleep(1);

    /* Add key4 - will evict key2 (oldest) */
    databox k4 = databoxNewBytesString("key4");
    databox v4 = databoxNewSigned(400);
    lruCachePut(cache, &k4, &v4);

    /* key2 should be evicted */
    if (!lruCacheGet(cache, &k2, &retrieved)) {
        printf("key2 was evicted\n");
    }

    /* key1 should still exist (was accessed recently) */
    if (lruCacheGet(cache, &k1, &retrieved)) {
        printf("key1 still in cache: %ld\n", retrieved.data.i64);
    }

    lruCacheFree(cache);
}
```

## Example 6: Database Query Result Cache

Cache database query results with automatic invalidation.

```c
#include "multimap.h"
#include "multimapAtom.h"

typedef struct {
    multimapAtom *queries;   /* Query string interning */
    multimap *results;       /* queryRef → {resultData, timestamp, hitCount} */
    time_t maxAge;
} QueryCache;

QueryCache *queryCacheNew(time_t maxAge) {
    QueryCache *qc = malloc(sizeof(*qc));
    qc->queries = multimapAtomNew();
    /* 4 elements: queryRef + resultData + timestamp + hitCount */
    qc->results = multimapNew(4);
    qc->maxAge = maxAge;
    return qc;
}

void queryCachePut(QueryCache *qc, const char *query, const databox *result) {
    /* Intern query string */
    databox queryBox = databoxNewBytesString(query);
    multimapAtomInsertIfNewConvert(qc->queries, &queryBox);

    /* Store result */
    databox timestamp = databoxNewSigned(time(NULL));
    databox hitCount = databoxNewSigned(0);
    const databox *entry[4] = {&queryBox, result, &timestamp, &hitCount};

    multimapInsert(&qc->results, entry);
}

bool queryCacheGet(QueryCache *qc, const char *query, databox *outResult) {
    /* Look up query reference */
    databox queryBox = databoxNewBytesString(query);
    databox queryRef;

    if (!multimapAtomLookupReference(qc->queries, &queryBox, &queryRef)) {
        /* Query not in cache */
        return false;
    }

    /* Look up cached result */
    databox result, timestamp, hitCount;
    databox *values[3] = {&result, &timestamp, &hitCount};

    if (!multimapLookup(qc->results, &queryRef, values)) {
        return false;
    }

    /* Check if result is too old */
    time_t age = time(NULL) - timestamp.data.i64;
    if (age > qc->maxAge) {
        /* Expired - delete it */
        multimapDelete(&qc->results, &queryRef);
        return false;
    }

    /* Increment hit count */
    multimapFieldIncr(&qc->results, &queryRef, 3, 1);

    *outResult = result;
    return true;
}

void queryCacheInvalidate(QueryCache *qc, const char *query) {
    databox queryBox = databoxNewBytesString(query);
    databox queryRef;

    if (multimapAtomLookupReference(qc->queries, &queryBox, &queryRef)) {
        multimapDelete(&qc->results, &queryRef);
    }
}

void queryCacheStats(QueryCache *qc) {
    printf("Query Cache Statistics:\n");
    printf("  Unique queries: %zu\n", multimapCount(qc->results));

    multimapIterator iter;
    multimapIteratorInit(qc->results, &iter, true);

    databox queryRef, result, timestamp, hitCount;
    databox *elements[4] = {&queryRef, &result, &timestamp, &hitCount};

    size_t totalHits = 0;
    while (multimapIteratorNext(&iter, elements)) {
        totalHits += hitCount.data.i64;
    }

    printf("  Total hits: %zu\n", totalHits);
}

void queryCacheFree(QueryCache *qc) {
    if (qc) {
        multimapAtomFree(qc->queries);
        multimapFree(qc->results);
        free(qc);
    }
}

/* Usage */
void exampleQueryCache(void) {
    QueryCache *qc = queryCacheNew(300);  /* 5 minute TTL */

    /* Simulate database query */
    databox result1 = databoxNewSigned(42);
    queryCachePut(qc, "SELECT COUNT(*) FROM users", &result1);

    /* Retrieve from cache */
    databox cached;
    if (queryCacheGet(qc, "SELECT COUNT(*) FROM users", &cached)) {
        printf("Cache hit! Result: %ld\n", cached.data.i64);
    }

    /* Stats */
    queryCacheStats(qc);

    /* Invalidate after data change */
    queryCacheInvalidate(qc, "SELECT COUNT(*) FROM users");

    queryCacheFree(qc);
}
```

## Performance Tips

### Tip 1: Use Append for Sorted Inserts

```c
/* BAD: Random inserts cause many memmoves */
for (int i = 0; i < 10000; i++) {
    int random = rand();
    databox k = databoxNewSigned(random);
    databox v = databoxNewSigned(random * 2);
    multimapInsert(&m, (const databox*[]){&k, &v});
}

/* GOOD: Sorted inserts with Append */
int sorted[10000];
/* ... fill and sort array ... */
for (int i = 0; i < 10000; i++) {
    databox k = databoxNewSigned(sorted[i]);
    databox v = databoxNewSigned(sorted[i] * 2);
    multimapAppend(&m, (const databox*[]){&k, &v});  /* Much faster! */
}
```

### Tip 2: Batch Operations

```c
/* BAD: Many small transactions */
for (int i = 0; i < 1000; i++) {
    /* fetch data */
    /* insert into multimap */
}

/* GOOD: Batch inserts */
/* Fetch all data first, then insert in sorted order */
```

### Tip 3: Choose Appropriate Size Limits

```c
/* For small, frequently updated maps */
multimap *hot = multimapNewLimit(2, FLEX_CAP_LEVEL_512);

/* For large, bulk-loaded maps */
multimap *bulk = multimapNewLimit(2, FLEX_CAP_LEVEL_4096);
```

### Tip 4: Use Reference Containers for Repeated Strings

```c
/* BAD: Store duplicate strings */
multimap *logs = multimapNew(3);  /* timestamp + level + message */

/* GOOD: Intern repeated log levels */
multimapAtom *atoms = multimapAtomNew();
multimap *logs = multimapNew(3);

databox level = databoxNewBytesString("ERROR");
multimapAtomInsertIfNewConvert(atoms, &level);
/* level is now a tiny integer reference */
```

## See Also

- [MULTIMAP.md](MULTIMAP.md) - Complete API reference
- [VARIANTS.md](VARIANTS.md) - Understanding Small/Medium/Full
- [databox](../core/DATABOX.md) - Value container
- [multimapAtom](MULTIMAP_ATOM.md) - String interning
