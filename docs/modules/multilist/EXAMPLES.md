# Multilist Examples - Real-World Usage

## Overview

This document provides practical, real-world examples of using the multilist family for common data structure patterns and algorithms.

## Basic Examples

### Example 1: Simple Queue (FIFO)

```c
#include "multilist.h"

/* Task queue processor */
typedef struct task {
    int id;
    char *description;
} task;

void processTaskQueue(void) {
    multilist *queue = multilistNew(FLEX_CAP_LEVEL_2048, 0);
    mflexState *state = mflexStateCreate();

    /* Producer: Add tasks to queue */
    for (int i = 0; i < 100; i++) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Process item %d", i);

        databox taskData = databoxNewBytesString(buf);
        multilistPushByTypeTail(&queue, state, &taskData);
    }

    printf("Queue has %zu tasks\n", multilistCount(queue));

    /* Consumer: Process tasks in order */
    databox result;
    while (multilistPopHead(&queue, state, &result)) {
        printf("Processing: %s\n", result.data.bytes.cstart);

        /* Do work here... */

        databoxFreeData(&result);
    }

    mflexStateFree(state);
    multilistFree(queue);
}
```

### Example 2: Stack (LIFO)

```c
#include "multilist.h"

/* Undo/Redo system */
typedef struct action {
    char *operation;
    void *data;
} action;

void undoRedoExample(void) {
    multilist *undoStack = multilistNew(FLEX_CAP_LEVEL_2048, 0);
    multilist *redoStack = multilistNew(FLEX_CAP_LEVEL_2048, 0);
    mflexState *state = mflexStateCreate();

    /* User performs actions */
    databox action1 = databoxNewBytesString("typed 'hello'");
    multilistPushByTypeHead(&undoStack, state, &action1);

    databox action2 = databoxNewBytesString("typed 'world'");
    multilistPushByTypeHead(&undoStack, state, &action2);

    databox action3 = databoxNewBytesString("deleted 'world'");
    multilistPushByTypeHead(&undoStack, state, &action3);

    printf("Undo stack has %zu actions\n", multilistCount(undoStack));

    /* User clicks 'Undo' */
    databox undone;
    if (multilistPopHead(&undoStack, state, &undone)) {
        printf("Undoing: %s\n", undone.data.bytes.cstart);

        /* Move to redo stack */
        multilistPushByTypeHead(&redoStack, state, &undone);
    }

    /* User clicks 'Redo' */
    databox redone;
    if (multilistPopHead(&redoStack, state, &redone)) {
        printf("Redoing: %s\n", redone.data.bytes.cstart);

        /* Move back to undo stack */
        multilistPushByTypeHead(&undoStack, state, &redone);
    }

    mflexStateFree(state);
    multilistFree(undoStack);
    multilistFree(redoStack);
}
```

### Example 3: Deque (Double-Ended Queue)

```c
#include "multilist.h"

/* Work stealing scheduler */
void workStealingScheduler(void) {
    multilist *localQueue = multilistNew(FLEX_CAP_LEVEL_2048, 0);
    mflexState *state = mflexStateCreate();

    /* Add work items */
    for (int i = 0; i < 10; i++) {
        databox work = databoxNewSigned(i);
        multilistPushByTypeTail(&localQueue, state, &work);
    }

    /* Local thread: Pop from head (LIFO for cache efficiency) */
    databox localWork;
    if (multilistPopHead(&localQueue, state, &localWork)) {
        printf("Local thread processing: %ld\n", localWork.data.i);
    }

    /* Stealing thread: Pop from tail (FIFO, least recently added) */
    databox stolenWork;
    if (multilistPopTail(&localQueue, state, &stolenWork)) {
        printf("Stealing thread got: %ld\n", stolenWork.data.i);
    }

    mflexStateFree(state);
    multilistFree(localQueue);
}
```

## Intermediate Examples

### Example 4: LRU Cache

```c
#include "multilist.h"

/* Least Recently Used cache using multilist */
typedef struct lruCache {
    multilist *list;    /* Stores keys in LRU order */
    size_t maxSize;
} lruCache;

lruCache *lruCacheCreate(size_t maxSize) {
    lruCache *cache = malloc(sizeof(lruCache));
    cache->list = multilistNew(FLEX_CAP_LEVEL_2048, 0);
    cache->maxSize = maxSize;
    return cache;
}

void lruCacheAccess(lruCache *cache, const char *key) {
    mflexState *s[2] = {mflexStateCreate(), mflexStateCreate()};

    /* Search for key in list */
    multilistIterator iter;
    multilistIteratorInitForward(cache->list, s, &iter);
    multilistEntry entry;
    bool found = false;

    while (multilistNext(&iter, &entry)) {
        if (strcmp(entry.box.data.bytes.cstart, key) == 0) {
            /* Found - move to front (most recently used) */
            databox keyBox = databoxNewBytesString(key);

            /* Remove from current position */
            multilistDelEntry(&iter, &entry);

            /* Add to front */
            multilistPushByTypeHead(&cache->list, s[0], &keyBox);

            found = true;
            break;
        }
    }

    if (!found) {
        /* Not in cache - add to front */
        databox keyBox = databoxNewBytesString(key);
        multilistPushByTypeHead(&cache->list, s[0], &keyBox);

        /* Evict if over capacity */
        if (multilistCount(cache->list) > cache->maxSize) {
            databox evicted;
            if (multilistPopTail(&cache->list, s[0], &evicted)) {
                printf("Evicting LRU item: %s\n", evicted.data.bytes.cstart);
                databoxFreeData(&evicted);
            }
        }
    }

    multilistIteratorRelease(&iter);
    mflexStateFree(s[0]);
    mflexStateFree(s[1]);
}

void lruCacheExample(void) {
    lruCache *cache = lruCacheCreate(5);

    /* Access pattern */
    lruCacheAccess(cache, "page1");
    lruCacheAccess(cache, "page2");
    lruCacheAccess(cache, "page3");
    lruCacheAccess(cache, "page4");
    lruCacheAccess(cache, "page5");
    lruCacheAccess(cache, "page6");  /* Evicts page1 */
    lruCacheAccess(cache, "page2");  /* Moves page2 to front */
    lruCacheAccess(cache, "page7");  /* Evicts page3 */

    multilistFree(cache->list);
    free(cache);
}
```

### Example 5: Circular Buffer

```c
#include "multilist.h"

/* Circular buffer for streaming data */
typedef struct circularBuffer {
    multilist *list;
    size_t maxSize;
    size_t writePos;
} circularBuffer;

circularBuffer *circularBufferCreate(size_t maxSize) {
    circularBuffer *cb = malloc(sizeof(circularBuffer));
    cb->list = multilistNew(FLEX_CAP_LEVEL_4096, 0);
    cb->maxSize = maxSize;
    cb->writePos = 0;
    return cb;
}

void circularBufferWrite(circularBuffer *cb, const void *data, size_t len) {
    mflexState *state = mflexStateCreate();
    databox item = databoxNewBytes(data, len);

    /* Add new item */
    multilistPushByTypeTail(&cb->list, state, &item);

    /* Remove oldest if over capacity */
    if (multilistCount(cb->list) > cb->maxSize) {
        databox old;
        multilistPopHead(&cb->list, state, &old);
        databoxFreeData(&old);
    }

    cb->writePos++;
    mflexStateFree(state);
}

void circularBufferRead(circularBuffer *cb, void (*callback)(const databox *)) {
    mflexState *s[2] = {mflexStateCreate(), mflexStateCreate()};

    multilistIterator iter;
    multilistIteratorInitForwardReadOnly(cb->list, s, &iter);
    multilistEntry entry;

    while (multilistNext(&iter, &entry)) {
        callback(&entry.box);
    }

    multilistIteratorRelease(&iter);
    mflexStateFree(s[0]);
    mflexStateFree(s[1]);
}

void printData(const databox *box) {
    printf("Data: %.*s\n", (int)box->len, box->data.bytes.cstart);
}

void circularBufferExample(void) {
    circularBuffer *cb = circularBufferCreate(5);

    /* Write sensor readings */
    for (int i = 0; i < 10; i++) {
        char reading[64];
        snprintf(reading, sizeof(reading), "Sensor reading %d: %.2f°C",
                 i, 20.0 + (i * 0.5));
        circularBufferWrite(cb, reading, strlen(reading));
    }

    printf("Buffer contains last 5 readings:\n");
    circularBufferRead(cb, printData);

    multilistFree(cb->list);
    free(cb);
}
```

### Example 6: Playlist Manager

```c
#include "multilist.h"

/* Music playlist with rotation */
typedef struct playlist {
    multilist *songs;
    char *name;
} playlist;

playlist *playlistCreate(const char *name) {
    playlist *pl = malloc(sizeof(playlist));
    pl->songs = multilistNew(FLEX_CAP_LEVEL_2048, 0);
    pl->name = strdup(name);
    return pl;
}

void playlistAdd(playlist *pl, const char *songName) {
    mflexState *state = mflexStateCreate();
    databox song = databoxNewBytesString(songName);
    multilistPushByTypeTail(&pl->songs, state, &song);
    mflexStateFree(state);
}

void playlistPlayNext(playlist *pl) {
    mflexState *s[2] = {mflexStateCreate(), mflexStateCreate()};
    multilistEntry entry;

    /* Get current song (at head) */
    if (multilistIndexGet(pl->songs, s[0], 0, &entry)) {
        printf("Now playing: %s\n", entry.box.data.bytes.cstart);

        /* Rotate to put current song at end (for repeat mode) */
        multilistRotate(pl->songs, s);
    } else {
        printf("Playlist is empty\n");
    }

    mflexStateFree(s[0]);
    mflexStateFree(s[1]);
}

void playlistExample(void) {
    playlist *pl = playlistCreate("My Favorites");

    /* Build playlist */
    playlistAdd(pl, "Song A");
    playlistAdd(pl, "Song B");
    playlistAdd(pl, "Song C");
    playlistAdd(pl, "Song D");

    /* Play through playlist (with rotation) */
    for (int i = 0; i < 8; i++) {
        printf("Track %d: ", i + 1);
        playlistPlayNext(pl);
    }

    multilistFree(pl->songs);
    free(pl->name);
    free(pl);
}
```

## Advanced Examples

### Example 7: Compressed Log Buffer

```c
#include "multilist.h"

/* High-performance log buffer with compression */
typedef struct logBuffer {
    multilist *logs;
    size_t totalLogs;
} logBuffer;

logBuffer *logBufferCreate(void) {
    logBuffer *lb = malloc(sizeof(logBuffer));
    /* Use Full variant with compression from the start */
    lb->logs = multilistNew(FLEX_CAP_LEVEL_8192, 5);
    /* compress=5 means first/last 5 nodes uncompressed */
    lb->totalLogs = 0;
    return lb;
}

void logBufferAppend(logBuffer *lb, const char *level, const char *message) {
    mflexState *state = mflexStateCreate();

    char logEntry[1024];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    snprintf(logEntry, sizeof(logEntry), "[%04d-%02d-%02d %02d:%02d:%02d] [%s] %s",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec,
             level, message);

    databox entry = databoxNewBytesString(logEntry);
    multilistPushByTypeTail(&lb->logs, state, &entry);

    lb->totalLogs++;
    mflexStateFree(state);
}

void logBufferDump(logBuffer *lb, const char *filename) {
    FILE *f = fopen(filename, "w");
    if (!f) return;

    mflexState *s[2] = {mflexStateCreate(), mflexStateCreate()};

    multilistIterator iter;
    multilistIteratorInitForwardReadOnly(lb->logs, s, &iter);
    multilistEntry entry;

    while (multilistNext(&iter, &entry)) {
        fprintf(f, "%s\n", entry.box.data.bytes.cstart);
    }

    multilistIteratorRelease(&iter);
    mflexStateFree(s[0]);
    mflexStateFree(s[1]);
    fclose(f);

    printf("Dumped %zu logs to %s\n", lb->totalLogs, filename);
}

void logBufferExample(void) {
    logBuffer *lb = logBufferCreate();

    /* Simulate application logging */
    logBufferAppend(lb, "INFO", "Application started");
    logBufferAppend(lb, "DEBUG", "Loading configuration");
    logBufferAppend(lb, "INFO", "Configuration loaded successfully");

    for (int i = 0; i < 10000; i++) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Processing request #%d", i);
        logBufferAppend(lb, "DEBUG", msg);
    }

    logBufferAppend(lb, "WARN", "High memory usage detected");
    logBufferAppend(lb, "ERROR", "Connection timeout");
    logBufferAppend(lb, "INFO", "Shutting down gracefully");

    printf("Total logs: %zu\n", lb->totalLogs);
    printf("Memory used: %zu bytes\n", multilistBytes(lb->logs));

    logBufferDump(lb, "application.log");

    multilistFree(lb->logs);
    free(lb);
}
```

### Example 8: Time Series Data

```c
#include "multilist.h"

/* Time series data storage */
typedef struct dataPoint {
    int64_t timestamp;
    double value;
} dataPoint;

typedef struct timeSeries {
    multilist *data;
    char *metric;
} timeSeries;

timeSeries *timeSeriesCreate(const char *metric) {
    timeSeries *ts = malloc(sizeof(timeSeries));
    ts->data = multilistNew(FLEX_CAP_LEVEL_4096, 2);
    ts->metric = strdup(metric);
    return ts;
}

void timeSeriesAdd(timeSeries *ts, int64_t timestamp, double value) {
    mflexState *state = mflexStateCreate();

    /* Pack timestamp and value into a single string for simplicity */
    char buf[128];
    snprintf(buf, sizeof(buf), "%ld:%.6f", timestamp, value);

    databox point = databoxNewBytesString(buf);
    multilistPushByTypeTail(&ts->data, state, &point);

    mflexStateFree(state);
}

void timeSeriesQuery(timeSeries *ts, int64_t startTime, int64_t endTime) {
    mflexState *s[2] = {mflexStateCreate(), mflexStateCreate()};

    multilistIterator iter;
    multilistIteratorInitForwardReadOnly(ts->data, s, &iter);
    multilistEntry entry;

    printf("Query results for %s from %ld to %ld:\n",
           ts->metric, startTime, endTime);

    while (multilistNext(&iter, &entry)) {
        int64_t ts;
        double val;

        /* Parse the stored data */
        sscanf(entry.box.data.bytes.cstart, "%ld:%lf", &ts, &val);

        if (ts >= startTime && ts <= endTime) {
            printf("  %ld: %.6f\n", ts, val);
        }
    }

    multilistIteratorRelease(&iter);
    mflexStateFree(s[0]);
    mflexStateFree(s[1]);
}

double timeSeriesAverage(timeSeries *ts) {
    mflexState *s[2] = {mflexStateCreate(), mflexStateCreate()};

    multilistIterator iter;
    multilistIteratorInitForwardReadOnly(ts->data, s, &iter);
    multilistEntry entry;

    double sum = 0.0;
    size_t count = 0;

    while (multilistNext(&iter, &entry)) {
        int64_t ts;
        double val;
        sscanf(entry.box.data.bytes.cstart, "%ld:%lf", &ts, &val);
        sum += val;
        count++;
    }

    multilistIteratorRelease(&iter);
    mflexStateFree(s[0]);
    mflexStateFree(s[1]);

    return count > 0 ? sum / count : 0.0;
}

void timeSeriesExample(void) {
    timeSeries *cpu = timeSeriesCreate("cpu.usage");

    /* Simulate CPU usage over 1 hour (1 sample per second) */
    int64_t baseTime = 1700000000;
    for (int i = 0; i < 3600; i++) {
        double usage = 30.0 + (rand() % 40);  /* 30-70% */
        timeSeriesAdd(cpu, baseTime + i, usage);
    }

    printf("Total samples: %zu\n", multilistCount(cpu->data));
    printf("Average CPU: %.2f%%\n", timeSeriesAverage(cpu));

    /* Query a 5-minute window */
    timeSeriesQuery(cpu, baseTime + 1000, baseTime + 1300);

    multilistFree(cpu->data);
    free(cpu->metric);
    free(cpu);
}
```

### Example 9: Message Queue with Priorities

```c
#include "multilist.h"

/* Priority message queue using multiple lists */
typedef struct priorityQueue {
    multilist *high;
    multilist *medium;
    multilist *low;
} priorityQueue;

priorityQueue *priorityQueueCreate(void) {
    priorityQueue *pq = malloc(sizeof(priorityQueue));
    pq->high = multilistNew(FLEX_CAP_LEVEL_2048, 0);
    pq->medium = multilistNew(FLEX_CAP_LEVEL_2048, 0);
    pq->low = multilistNew(FLEX_CAP_LEVEL_2048, 0);
    return pq;
}

typedef enum priority {
    PRIORITY_LOW,
    PRIORITY_MEDIUM,
    PRIORITY_HIGH
} priority;

void priorityQueueEnqueue(priorityQueue *pq, priority p, const char *message) {
    mflexState *state = mflexStateCreate();
    databox msg = databoxNewBytesString(message);

    switch (p) {
    case PRIORITY_HIGH:
        multilistPushByTypeTail(&pq->high, state, &msg);
        break;
    case PRIORITY_MEDIUM:
        multilistPushByTypeTail(&pq->medium, state, &msg);
        break;
    case PRIORITY_LOW:
        multilistPushByTypeTail(&pq->low, state, &msg);
        break;
    }

    mflexStateFree(state);
}

bool priorityQueueDequeue(priorityQueue *pq, databox *result) {
    mflexState *state = mflexStateCreate();
    bool found = false;

    /* Try high priority first */
    if (multilistPopHead(&pq->high, state, result)) {
        found = true;
    }
    /* Then medium */
    else if (multilistPopHead(&pq->medium, state, result)) {
        found = true;
    }
    /* Finally low */
    else if (multilistPopHead(&pq->low, state, result)) {
        found = true;
    }

    mflexStateFree(state);
    return found;
}

void priorityQueueExample(void) {
    priorityQueue *pq = priorityQueueCreate();

    /* Enqueue messages with different priorities */
    priorityQueueEnqueue(pq, PRIORITY_LOW, "Cleanup old logs");
    priorityQueueEnqueue(pq, PRIORITY_HIGH, "Critical: Database down!");
    priorityQueueEnqueue(pq, PRIORITY_MEDIUM, "Update user preferences");
    priorityQueueEnqueue(pq, PRIORITY_LOW, "Send weekly report");
    priorityQueueEnqueue(pq, PRIORITY_HIGH, "Alert: Security breach");
    priorityQueueEnqueue(pq, PRIORITY_MEDIUM, "Process new signups");

    /* Dequeue and process (high priority first) */
    databox msg;
    while (priorityQueueDequeue(pq, &msg)) {
        printf("Processing: %s\n", msg.data.bytes.cstart);
        databoxFreeData(&msg);
    }

    multilistFree(pq->high);
    multilistFree(pq->medium);
    multilistFree(pq->low);
    free(pq);
}
```

### Example 10: Browser History

```c
#include "multilist.h"

/* Browser history with back/forward navigation */
typedef struct browserHistory {
    multilist *history;
    int currentPosition;
} browserHistory;

browserHistory *browserHistoryCreate(const char *homepage) {
    browserHistory *bh = malloc(sizeof(browserHistory));
    bh->history = multilistNew(FLEX_CAP_LEVEL_2048, 0);
    bh->currentPosition = 0;

    mflexState *state = mflexStateCreate();
    databox page = databoxNewBytesString(homepage);
    multilistPushByTypeTail(&bh->history, state, &page);
    mflexStateFree(state);

    return bh;
}

void browserHistoryVisit(browserHistory *bh, const char *url) {
    mflexState *s[2] = {mflexStateCreate(), mflexStateCreate()};

    /* Delete all pages after current position (can't go forward anymore) */
    size_t total = multilistCount(bh->history);
    if (bh->currentPosition < total - 1) {
        size_t toDelete = total - bh->currentPosition - 1;
        multilistDelRange(&bh->history, s[0], bh->currentPosition + 1, toDelete);
    }

    /* Add new page */
    databox page = databoxNewBytesString(url);
    multilistPushByTypeTail(&bh->history, s[0], &page);

    bh->currentPosition++;

    mflexStateFree(s[0]);
    mflexStateFree(s[1]);
}

const char *browserHistoryBack(browserHistory *bh) {
    if (bh->currentPosition > 0) {
        bh->currentPosition--;

        mflexState *state = mflexStateCreate();
        multilistEntry entry;
        if (multilistIndexGet(bh->history, state, bh->currentPosition, &entry)) {
            mflexStateFree(state);
            return entry.box.data.bytes.cstart;
        }
        mflexStateFree(state);
    }

    return NULL;
}

const char *browserHistoryForward(browserHistory *bh) {
    size_t total = multilistCount(bh->history);
    if (bh->currentPosition < total - 1) {
        bh->currentPosition++;

        mflexState *state = mflexStateCreate();
        multilistEntry entry;
        if (multilistIndexGet(bh->history, state, bh->currentPosition, &entry)) {
            mflexStateFree(state);
            return entry.box.data.bytes.cstart;
        }
        mflexStateFree(state);
    }

    return NULL;
}

void browserHistoryExample(void) {
    browserHistory *bh = browserHistoryCreate("https://example.com");

    printf("Visiting pages...\n");
    browserHistoryVisit(bh, "https://news.example.com");
    browserHistoryVisit(bh, "https://blog.example.com");
    browserHistoryVisit(bh, "https://shop.example.com");

    printf("\nGoing back...\n");
    printf("Back to: %s\n", browserHistoryBack(bh));
    printf("Back to: %s\n", browserHistoryBack(bh));

    printf("\nGoing forward...\n");
    printf("Forward to: %s\n", browserHistoryForward(bh));

    printf("\nVisiting new page (clears forward history)...\n");
    browserHistoryVisit(bh, "https://store.example.com");

    printf("Can't go forward anymore: %s\n",
           browserHistoryForward(bh) ? "FALSE" : "TRUE");

    multilistFree(bh->history);
    free(bh);
}
```

## Performance Examples

### Example 11: Benchmarking Different Variants

```c
#include "multilist.h"
#include <time.h>

double measurePushPerformance(flexCapSizeLimit limit, size_t operations) {
    multilist *ml = multilistNew(limit, 0);
    mflexState *state = mflexStateCreate();

    clock_t start = clock();

    for (size_t i = 0; i < operations; i++) {
        databox item = databoxNewSigned(i);
        multilistPushByTypeTail(&ml, state, &item);
    }

    clock_t end = clock();
    double elapsed = ((double)(end - start)) / CLOCKS_PER_SEC;

    printf("Limit %d: %zu ops in %.3f sec (%.0f ops/sec)\n",
           limit, operations, elapsed, operations / elapsed);
    printf("  Final size: %zu elements, %zu bytes\n",
           multilistCount(ml), multilistBytes(ml));

    mflexStateFree(state);
    multilistFree(ml);

    return elapsed;
}

void benchmarkExample(void) {
    printf("=== Multilist Push Performance ===\n\n");

    size_t operations = 1000000;

    printf("Small nodes:\n");
    measurePushPerformance(FLEX_CAP_LEVEL_512, operations);

    printf("\nMedium nodes:\n");
    measurePushPerformance(FLEX_CAP_LEVEL_2048, operations);

    printf("\nLarge nodes:\n");
    measurePushPerformance(FLEX_CAP_LEVEL_8192, operations);
}
```

### Example 12: Compression Effectiveness

```c
#include "multilist.h"

void compressionExample(void) {
    printf("=== Compression Effectiveness ===\n\n");

    /* Without compression */
    multilist *uncompressed = multilistNew(FLEX_CAP_LEVEL_4096, 0);
    mflexState *s1 = mflexStateCreate();

    /* With compression */
    multilist *compressed = multilistNew(FLEX_CAP_LEVEL_4096, 2);
    mflexState *s2 = mflexStateCreate();

    /* Add repetitive text data (compresses well) */
    for (int i = 0; i < 100000; i++) {
        char buf[200];
        snprintf(buf, sizeof(buf),
                 "This is a log message with repeated text. "
                 "Entry number %d. Status: OK. "
                 "Server: web-server-01. Time: 2024-01-01 12:00:%02d",
                 i, i % 60);

        databox item1 = databoxNewBytesString(buf);
        databox item2 = databoxNewBytesString(buf);

        multilistPushByTypeTail(&uncompressed, s1, &item1);
        multilistPushByTypeTail(&compressed, s2, &item2);
    }

    size_t uncompressedSize = multilistBytes(uncompressed);
    size_t compressedSize = multilistBytes(compressed);
    double savings = 100.0 * (1.0 - ((double)compressedSize / uncompressedSize));

    printf("Uncompressed: %zu bytes\n", uncompressedSize);
    printf("Compressed:   %zu bytes\n", compressedSize);
    printf("Savings:      %.1f%%\n", savings);

    mflexStateFree(s1);
    mflexStateFree(s2);
    multilistFree(uncompressed);
    multilistFree(compressed);
}
```

## Common Patterns Summary

| Pattern | Operations | Use Case |
|---------|-----------|----------|
| Queue (FIFO) | PushTail + PopHead | Task processing, message queues |
| Stack (LIFO) | PushHead + PopHead | Undo/redo, call stack simulation |
| Deque | Push/Pop both ends | Work stealing, sliding windows |
| Circular Buffer | PushTail + PopHead with size limit | Streaming data, recent history |
| LRU Cache | Search + move to front + evict tail | Cache management |
| Priority Queue | Multiple lists | Task scheduling by importance |
| History | Insert at position + delete range | Browser history, document editing |

## Best Practices from Examples

1. **Always create and free mflexState:**
   ```c
   mflexState *state = mflexStateCreate();
   /* ... use state ... */
   mflexStateFree(state);
   ```

2. **Use appropriate size limits:**
   - Small data: `FLEX_CAP_LEVEL_512` or `FLEX_CAP_LEVEL_1024`
   - Medium data: `FLEX_CAP_LEVEL_2048` (default)
   - Large data: `FLEX_CAP_LEVEL_4096` or `FLEX_CAP_LEVEL_8192`

3. **Free popped data:**
   ```c
   databox result;
   if (multilistPopHead(&ml, state, &result)) {
       /* Use result... */
       databoxFreeData(&result);  /* Important! */
   }
   ```

4. **Release iterators:**
   ```c
   multilistIterator iter;
   multilistIteratorInitForward(ml, s, &iter);
   /* ... iterate ... */
   multilistIteratorRelease(&iter);  /* Important for Full variant! */
   ```

5. **Choose compression wisely:**
   - No compression (`depth=0`): Maximum speed
   - Light compression (`depth=1-2`): Good balance
   - Heavy compression (`depth=5+`): Maximum space savings

## See Also

- [MULTILIST.md](MULTILIST.md) - Complete API reference
- [VARIANTS.md](VARIANTS.md) - Understanding Small/Medium/Full variants
- [databox](../core/DATABOX.md) - Value container documentation
- [flex](../flex/FLEX.md) - Underlying storage format
