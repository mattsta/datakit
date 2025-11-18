# multiarray Real-World Examples

## Overview

This guide demonstrates practical applications of the multiarray family through real-world examples. Each example includes complete, working code that you can adapt to your own projects.

## Example 1: Time-Series Data Buffer

A circular-style buffer for storing recent time-series measurements with automatic growth.

```c
#include "multiarray.h"
#include <time.h>

typedef struct Measurement {
    time_t timestamp;
    double value;
    uint32_t sensor_id;
} Measurement;

typedef struct TimeSeriesBuffer {
    multiarray *data;
    size_t maxSize;
} TimeSeriesBuffer;

TimeSeriesBuffer *tsBufferNew(size_t maxSize) {
    TimeSeriesBuffer *buf = malloc(sizeof(*buf));
    buf->data = multiarrayNew(sizeof(Measurement), 1024);
    buf->maxSize = maxSize;
    return buf;
}

void tsBufferAppend(TimeSeriesBuffer *buf, uint32_t sensor_id, double value) {
    size_t count = multiarrayCount(buf->data);

    /* Evict oldest if at capacity */
    if (count >= buf->maxSize) {
        multiarrayDelete(&buf->data, 0);
        count--;
    }

    /* Append new measurement */
    Measurement m = {
        .timestamp = time(NULL),
        .value = value,
        .sensor_id = sensor_id
    };

    multiarrayInsertAfter(&buf->data, count > 0 ? count - 1 : 0, &m);
}

Measurement *tsBufferGetRecent(TimeSeriesBuffer *buf, size_t n) {
    size_t count = multiarrayCount(buf->data);
    if (n >= count) {
        return NULL;
    }

    return (Measurement *)multiarrayGet(buf->data, count - 1 - n);
}

void tsBufferQueryRange(TimeSeriesBuffer *buf, time_t start, time_t end,
                        Measurement **results, size_t *count) {
    size_t total = multiarrayCount(buf->data);
    Measurement *tmp = malloc(sizeof(Measurement) * total);
    size_t found = 0;

    for (size_t i = 0; i < total; i++) {
        Measurement *m = (Measurement *)multiarrayGet(buf->data, i);
        if (m->timestamp >= start && m->timestamp <= end) {
            tmp[found++] = *m;
        }
    }

    *results = realloc(tmp, sizeof(Measurement) * found);
    *count = found;
}

void tsBufferFree(TimeSeriesBuffer *buf) {
    if (buf) {
        /* Note: multiarray variants must be freed based on type */
        free(buf);
    }
}

/* Usage */
void exampleTimeSeriesBuffer(void) {
    TimeSeriesBuffer *buf = tsBufferNew(10000);

    /* Record measurements */
    for (int i = 0; i < 50000; i++) {
        double value = 20.0 + (rand() % 100) / 10.0;
        tsBufferAppend(buf, i % 5, value);
    }

    /* Get most recent */
    Measurement *recent = tsBufferGetRecent(buf, 0);
    printf("Most recent: sensor=%u, value=%.2f\n",
           recent->sensor_id, recent->value);

    /* Query time range */
    time_t now = time(NULL);
    Measurement *results;
    size_t count;
    tsBufferQueryRange(buf, now - 3600, now, &results, &count);
    printf("Last hour: %zu measurements\n", count);
    free(results);

    tsBufferFree(buf);
}
```

## Example 2: Dynamic Vertex Buffer for Graphics

A growing vertex buffer that automatically scales for 3D graphics applications.

```c
#include "multiarray.h"

typedef struct Vertex {
    float position[3];
    float normal[3];
    float texcoord[2];
    uint32_t color;
} Vertex;

typedef struct VertexBuffer {
    Vertex *vertices;
    int count;
    int capacity;
} VertexBuffer;

VertexBuffer *vertexBufferNew(void) {
    VertexBuffer *vb = malloc(sizeof(*vb));
    vb->vertices = multiarrayNativeNew(vb->vertices[1024]);
    vb->count = 0;
    vb->capacity = 1024;
    return vb;
}

void vertexBufferAddVertex(VertexBuffer *vb, const Vertex *v) {
    multiarrayNativeInsert(vb->vertices, Vertex, vb->capacity,
                          vb->count, vb->count, v);
    /* Automatically upgrades to Medium if capacity exceeded */
}

void vertexBufferAddTriangle(VertexBuffer *vb,
                            const Vertex *v0,
                            const Vertex *v1,
                            const Vertex *v2) {
    vertexBufferAddVertex(vb, v0);
    vertexBufferAddVertex(vb, v1);
    vertexBufferAddVertex(vb, v2);
}

void vertexBufferAddQuad(VertexBuffer *vb,
                        const Vertex *v0,
                        const Vertex *v1,
                        const Vertex *v2,
                        const Vertex *v3) {
    /* Two triangles */
    vertexBufferAddTriangle(vb, v0, v1, v2);
    vertexBufferAddTriangle(vb, v0, v2, v3);
}

Vertex *vertexBufferGetVertex(VertexBuffer *vb, int index) {
    Vertex *v;
    multiarrayNativeGet(vb->vertices, Vertex, v, index);
    return v;
}

void vertexBufferGenerateCube(VertexBuffer *vb, float size) {
    float h = size / 2.0f;

    /* Front face */
    Vertex v0 = {{-h, -h,  h}, {0, 0, 1}, {0, 0}, 0xFFFFFFFF};
    Vertex v1 = {{ h, -h,  h}, {0, 0, 1}, {1, 0}, 0xFFFFFFFF};
    Vertex v2 = {{ h,  h,  h}, {0, 0, 1}, {1, 1}, 0xFFFFFFFF};
    Vertex v3 = {{-h,  h,  h}, {0, 0, 1}, {0, 1}, 0xFFFFFFFF};
    vertexBufferAddQuad(vb, &v0, &v1, &v2, &v3);

    /* Add remaining 5 faces... */
    /* (back, left, right, top, bottom) */
}

void vertexBufferFree(VertexBuffer *vb) {
    if (vb) {
        multiarrayNativeFree(vb->vertices);
        free(vb);
    }
}

/* Usage */
void exampleVertexBuffer(void) {
    VertexBuffer *vb = vertexBufferNew();

    /* Generate geometry */
    for (int i = 0; i < 100; i++) {
        vertexBufferGenerateCube(vb, 1.0f + i * 0.1f);
    }

    printf("Total vertices: %d\n", vb->count);
    printf("Bytes used: %zu\n", multiarrayBytes((multiarray *)vb->vertices));

    vertexBufferFree(vb);
}
```

## Example 3: Event Log with Filtering

An event logging system that stores events and allows filtering by type and time.

```c
#include "multiarray.h"
#include <string.h>

typedef enum EventType {
    EVENT_INFO,
    EVENT_WARNING,
    EVENT_ERROR,
    EVENT_DEBUG
} EventType;

typedef struct Event {
    time_t timestamp;
    EventType type;
    uint32_t source_id;
    char message[128];
} Event;

typedef struct EventLog {
    multiarraySmall *events;
} EventLog;

EventLog *eventLogNew(void) {
    EventLog *log = malloc(sizeof(*log));
    log->events = multiarraySmallNew(sizeof(Event), 2048);
    return log;
}

void eventLogAdd(EventLog *log, EventType type, uint32_t source_id,
                const char *message) {
    Event e = {
        .timestamp = time(NULL),
        .type = type,
        .source_id = source_id
    };
    strncpy(e.message, message, sizeof(e.message) - 1);

    multiarraySmallInsert(log->events, log->events->count, &e);
}

void eventLogInfo(EventLog *log, uint32_t source_id, const char *msg) {
    eventLogAdd(log, EVENT_INFO, source_id, msg);
}

void eventLogWarning(EventLog *log, uint32_t source_id, const char *msg) {
    eventLogAdd(log, EVENT_WARNING, source_id, msg);
}

void eventLogError(EventLog *log, uint32_t source_id, const char *msg) {
    eventLogAdd(log, EVENT_ERROR, source_id, msg);
}

void eventLogFilter(EventLog *log, EventType type, Event **results, size_t *count) {
    Event *tmp = malloc(sizeof(Event) * log->events->count);
    size_t found = 0;

    for (size_t i = 0; i < log->events->count; i++) {
        Event *e = (Event *)multiarraySmallGet(log->events, i);
        if (e->type == type) {
            tmp[found++] = *e;
        }
    }

    *results = realloc(tmp, sizeof(Event) * found);
    *count = found;
}

void eventLogPrint(EventLog *log) {
    const char *typeNames[] = {"INFO", "WARN", "ERROR", "DEBUG"};

    for (size_t i = 0; i < log->events->count; i++) {
        Event *e = (Event *)multiarraySmallGet(log->events, i);
        printf("[%s] Source %u: %s\n", typeNames[e->type],
               e->source_id, e->message);
    }
}

void eventLogFree(EventLog *log) {
    if (log) {
        multiarraySmallFree(log->events);
        free(log);
    }
}

/* Usage */
void exampleEventLog(void) {
    EventLog *log = eventLogNew();

    /* Log events */
    eventLogInfo(log, 1, "System started");
    eventLogInfo(log, 2, "Connection established");
    eventLogWarning(log, 1, "High memory usage");
    eventLogError(log, 3, "Connection lost");
    eventLogInfo(log, 2, "Reconnecting...");

    /* Print all events */
    printf("All events:\n");
    eventLogPrint(log);

    /* Filter errors */
    Event *errors;
    size_t errorCount;
    eventLogFilter(log, EVENT_ERROR, &errors, &errorCount);
    printf("\nErrors: %zu\n", errorCount);
    for (size_t i = 0; i < errorCount; i++) {
        printf("  %s\n", errors[i].message);
    }
    free(errors);

    eventLogFree(log);
}
```

## Example 4: Particle System

A dynamic particle system that efficiently manages thousands of particles.

```c
#include "multiarray.h"

typedef struct Particle {
    float position[3];
    float velocity[3];
    float life;
    uint32_t color;
} Particle;

typedef struct ParticleSystem {
    multiarrayMedium *particles;
    float gravity[3];
} ParticleSystem;

ParticleSystem *particleSystemNew(void) {
    ParticleSystem *ps = malloc(sizeof(*ps));
    ps->particles = multiarrayMediumNew(sizeof(Particle), 512);
    ps->gravity[0] = 0.0f;
    ps->gravity[1] = -9.8f;
    ps->gravity[2] = 0.0f;
    return ps;
}

void particleSystemEmit(ParticleSystem *ps, float pos[3], float vel[3]) {
    Particle p = {
        .position = {pos[0], pos[1], pos[2]},
        .velocity = {vel[0], vel[1], vel[2]},
        .life = 1.0f,
        .color = 0xFFFFFFFF
    };

    /* Insert at end */
    size_t count = 0;
    for (uint32_t i = 0; i < ps->particles->count; i++) {
        count += ps->particles->node[i].count;
    }

    multiarrayMediumInsert(ps->particles, count, &p);
}

void particleSystemUpdate(ParticleSystem *ps, float deltaTime) {
    /* Iterate through all nodes */
    for (uint32_t nodeIdx = 0; nodeIdx < ps->particles->count; nodeIdx++) {
        multiarrayMediumNode *node = &ps->particles->node[nodeIdx];

        /* Process particles in this node */
        for (uint16_t i = 0; i < node->count; ) {
            Particle *p = (Particle *)(node->data + i * ps->particles->len);

            /* Update physics */
            p->velocity[0] += ps->gravity[0] * deltaTime;
            p->velocity[1] += ps->gravity[1] * deltaTime;
            p->velocity[2] += ps->gravity[2] * deltaTime;

            p->position[0] += p->velocity[0] * deltaTime;
            p->position[1] += p->velocity[1] * deltaTime;
            p->position[2] += p->velocity[2] * deltaTime;

            /* Age particle */
            p->life -= deltaTime;

            /* Remove dead particles */
            if (p->life <= 0.0f) {
                /* Calculate global index */
                size_t globalIdx = 0;
                for (uint32_t j = 0; j < nodeIdx; j++) {
                    globalIdx += ps->particles->node[j].count;
                }
                globalIdx += i;

                multiarrayMediumDelete(ps->particles, globalIdx);
                /* Don't increment i - next particle shifted down */
            } else {
                i++;
            }
        }
    }
}

size_t particleSystemCount(ParticleSystem *ps) {
    size_t count = 0;
    for (uint32_t i = 0; i < ps->particles->count; i++) {
        count += ps->particles->node[i].count;
    }
    return count;
}

void particleSystemFree(ParticleSystem *ps) {
    if (ps) {
        multiarrayMediumFree(ps->particles);
        free(ps);
    }
}

/* Usage */
void exampleParticleSystem(void) {
    ParticleSystem *ps = particleSystemNew();

    /* Emit particles */
    for (int i = 0; i < 1000; i++) {
        float pos[3] = {0.0f, 10.0f, 0.0f};
        float vel[3] = {
            (rand() / (float)RAND_MAX - 0.5f) * 2.0f,
            (rand() / (float)RAND_MAX) * 5.0f,
            (rand() / (float)RAND_MAX - 0.5f) * 2.0f
        };
        particleSystemEmit(ps, pos, vel);
    }

    /* Simulate */
    for (int frame = 0; frame < 100; frame++) {
        particleSystemUpdate(ps, 1.0f / 60.0f);
        printf("Frame %d: %zu particles\n", frame, particleSystemCount(ps));
    }

    particleSystemFree(ps);
}
```

## Example 5: Sorted Index with Binary Search

A sorted array that maintains order and supports binary search.

```c
#include "multiarray.h"

typedef struct IndexEntry {
    uint64_t key;
    uint32_t value;
} IndexEntry;

typedef struct SortedIndex {
    multiarraySmall *entries;
} SortedIndex;

SortedIndex *sortedIndexNew(void) {
    SortedIndex *idx = malloc(sizeof(*idx));
    idx->entries = multiarraySmallNew(sizeof(IndexEntry), 1024);
    return idx;
}

/* Binary search to find insertion point */
static int findInsertPosition(SortedIndex *idx, uint64_t key) {
    int left = 0;
    int right = idx->entries->count;

    while (left < right) {
        int mid = (left + right) / 2;
        IndexEntry *e = (IndexEntry *)multiarraySmallGet(idx->entries, mid);

        if (e->key < key) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    return left;
}

void sortedIndexInsert(SortedIndex *idx, uint64_t key, uint32_t value) {
    int pos = findInsertPosition(idx, key);

    /* Check for duplicate */
    if (pos < idx->entries->count) {
        IndexEntry *e = (IndexEntry *)multiarraySmallGet(idx->entries, pos);
        if (e->key == key) {
            /* Update existing */
            e->value = value;
            return;
        }
    }

    /* Insert at position */
    IndexEntry entry = {.key = key, .value = value};
    multiarraySmallInsert(idx->entries, pos, &entry);
}

bool sortedIndexLookup(SortedIndex *idx, uint64_t key, uint32_t *value) {
    int pos = findInsertPosition(idx, key);

    if (pos < idx->entries->count) {
        IndexEntry *e = (IndexEntry *)multiarraySmallGet(idx->entries, pos);
        if (e->key == key) {
            *value = e->value;
            return true;
        }
    }

    return false;
}

void sortedIndexRange(SortedIndex *idx, uint64_t start, uint64_t end,
                     IndexEntry **results, size_t *count) {
    int startPos = findInsertPosition(idx, start);
    int endPos = findInsertPosition(idx, end);

    *count = endPos - startPos;
    *results = malloc(sizeof(IndexEntry) * (*count));

    for (int i = startPos; i < endPos; i++) {
        IndexEntry *e = (IndexEntry *)multiarraySmallGet(idx->entries, i);
        (*results)[i - startPos] = *e;
    }
}

void sortedIndexFree(SortedIndex *idx) {
    if (idx) {
        multiarraySmallFree(idx->entries);
        free(idx);
    }
}

/* Usage */
void exampleSortedIndex(void) {
    SortedIndex *idx = sortedIndexNew();

    /* Insert in random order */
    uint64_t keys[] = {50, 10, 90, 30, 70, 20, 80, 40, 60, 100};
    for (int i = 0; i < 10; i++) {
        sortedIndexInsert(idx, keys[i], i);
    }

    /* Lookup */
    uint32_t value;
    if (sortedIndexLookup(idx, 70, &value)) {
        printf("Key 70 -> Value %u\n", value);
    }

    /* Range query */
    IndexEntry *range;
    size_t count;
    sortedIndexRange(idx, 30, 80, &range, &count);
    printf("Range [30, 80): %zu entries\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("  Key: %lu, Value: %u\n", range[i].key, range[i].value);
    }
    free(range);

    sortedIndexFree(idx);
}
```

## Example 6: Large Dataset Processing

Processing millions of records with Large variant for memory efficiency.

```c
#include "multiarray.h"

typedef struct Record {
    uint64_t id;
    uint32_t category;
    double score;
} Record;

void processLargeDataset(const char *filename) {
    /* Use Large variant for millions of records */
    multiarrayLarge *records = multiarrayLargeNew(sizeof(Record), 2048);

    /* Load data (simulated) */
    for (int i = 0; i < 10000000; i++) {
        Record r = {
            .id = i,
            .category = i % 100,
            .score = (rand() % 1000) / 10.0
        };

        multiarrayLargeInsert(records, i, &r);

        if (i % 1000000 == 0) {
            printf("Loaded %d million records\n", i / 1000000);
        }
    }

    /* Process by iterating through nodes */
    multiarrayLargeNode *prev = NULL;
    multiarrayLargeNode *current = records->head;

    double totalScore = 0.0;
    uint64_t count = 0;

    while (current) {
        /* Process entire node */
        for (uint16_t i = 0; i < current->count; i++) {
            Record *r = (Record *)(current->data + i * records->len);
            totalScore += r->score;
            count++;
        }

        /* Next node via XOR */
        multiarrayLargeNode *next = (multiarrayLargeNode *)(
            (uintptr_t)prev ^ current->prevNext
        );
        prev = current;
        current = next;
    }

    printf("Processed %lu records\n", count);
    printf("Average score: %.2f\n", totalScore / count);

    multiarrayLargeFree(records);
}
```

## Performance Tips

### Tip 1: Use GetForward for Sequential Access

```c
/* SLOW: Random access in Large */
for (int i = 0; i < count; i++) {
    void *elem = multiarrayLargeGet(large, i);  /* O(n) each time! */
}

/* FAST: Forward iteration */
for (int i = 0; i < count; i++) {
    void *elem = multiarrayLargeGetForward(large, i);  /* Maintains state */
}
```

### Tip 2: Choose Appropriate rowMax

```c
/* Small rowMax - more nodes, less wasted space, more splits */
multiarray *compact = multiarrayNew(sizeof(int), 128);

/* Large rowMax - fewer nodes, more wasted space, fewer splits */
multiarray *spacious = multiarrayNew(sizeof(int), 4096);
```

### Tip 3: Batch Inserts at End

```c
/* SLOW: Insert at random positions */
for (int i = 0; i < 10000; i++) {
    multiarraySmallInsert(small, rand() % small->count, &data);
}

/* FAST: Append to end */
for (int i = 0; i < 10000; i++) {
    multiarraySmallInsert(small, small->count, &data);
}
```

### Tip 4: Use Native for Fixed Size

```c
/* If you know the size won't exceed 1000 */
MyStruct *arr = multiarrayNativeNew(arr[1000]);
/* Zero overhead, direct access */
```

## See Also

- [MULTIARRAY.md](MULTIARRAY.md) - Complete API reference
- [VARIANTS.md](VARIANTS.md) - Understanding Native/Small/Medium/Large
- [databox](../core/DATABOX.md) - Universal container type
