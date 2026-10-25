# multiarray Worked Examples

## Overview

These walkthroughs go deep on multiarray specifics: which entry point to use,
what each tier's struct fields actually mean, and how to iterate a chunked tier
without paying O(K) per element. For generic container recipes that are not
multiarray-specific, see the cross-module guides under `docs/examples/`.

Two module facts shape almost every example below:

- **The container API counts; the Native macro API does not.**
  `multiarrayCount()` and `multiarrayBytes()` answer for a `multiarrayNew()`
  array (Count is O(nodes) on Medium/Large); a Native array's caller keeps the
  count, as the examples below do.
- **Struct fields need the internal headers.** `multiarray.h` transitively
  exposes `multiarraySmall` and `multiarrayMedium`; reaching into a
  `multiarrayLarge` needs an explicit `#include "multiarrayLargeInternal.h"`.

## Example 1: Bounded Time-Series Buffer (container API)

A capped append-and-evict buffer over the tag-dispatching container API. The
buffer owns the element count because the container does not.

```c
#include "multiarray.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct Measurement {
    int64_t timestamp;
    double value;
    uint32_t sensorId;
} Measurement;

typedef struct TimeSeriesBuffer {
    multiarray *data;
    uint32_t count;   /* multiarray does not track this for us */
    uint32_t maxSize;
} TimeSeriesBuffer;

TimeSeriesBuffer *tsBufferNew(uint32_t maxSize) {
    TimeSeriesBuffer *buf = zcalloc(1, sizeof(*buf));

    /* rowMax is the per-node element capacity, not a total capacity. */
    buf->data = multiarrayNew(sizeof(Measurement), 512);
    buf->maxSize = maxSize;
    return buf;
}

void tsBufferAppend(TimeSeriesBuffer *buf, uint32_t sensorId, double value) {
    /* Evict the oldest element first so we stay at maxSize. */
    if (buf->count >= buf->maxSize) {
        multiarrayDelete(&buf->data, 0);
        buf->count--;
    }

    /* Not const: multiarrayInsert() takes a non-const void *. */
    Measurement m = {.timestamp = (int64_t)time(NULL),
                     .value = value,
                     .sensorId = sensorId};

    /* idx is the position the new element will occupy, so count == append. */
    multiarrayInsert(&buf->data, buf->count, &m);
    buf->count++;
}

const Measurement *tsBufferRecent(const TimeSeriesBuffer *buf, uint32_t back) {
    if (back >= buf->count) {
        return NULL;
    }

    return (const Measurement *)multiarrayGet(buf->data,
                                              buf->count - 1 - back);
}

void tsBufferFree(TimeSeriesBuffer *buf) {
    if (buf) {
        multiarrayFree(buf->data); /* handles Small, Medium, and Large */
        zfree(buf);
    }
}

void exampleTimeSeriesBuffer(void) {
    TimeSeriesBuffer *buf = tsBufferNew(10000);

    for (uint32_t i = 0; i < 50000; i++) {
        tsBufferAppend(buf, i % 5, 20.0 + (double)(i % 100) / 10.0);
    }

    const Measurement *recent = tsBufferRecent(buf, 0);
    printf("most recent: sensor=%" PRIu32 " value=%.2f ts=%" PRId64 "\n",
           recent->sensorId, recent->value, recent->timestamp);

    tsBufferFree(buf);
}
```

**What to notice.** `multiarrayInsert()` and `multiarrayDelete()` take
`multiarray **` because the insert path may replace the pointer with a promoted
container. Storing the `multiarray *` inside a struct and passing
`&buf->data` is exactly right; passing a local copy would lose the promotion.

Head deletion is the expensive direction in every tier (Small memmoves the whole
array; Medium and Large memmove within the head node and eventually free it).
Evicting in batches rather than one per append cuts that cost.

## Example 2: Vertex Buffer (Native macro API)

The Native API has no container struct: it is a raw typed array plus your own
count, promoted to Medium at `rowMax`.

```c
#include "multiarray.h"

#include <inttypes.h>
#include <stdio.h>

typedef struct Vertex {
    float position[3];
    float normal[3];
    float texcoord[2];
    uint32_t color;
} Vertex;

typedef struct VertexBuffer {
    /* Must be multiarray *, NOT Vertex *: multiarrayNativeInsert() assigns a
     * promoted container back into this variable once it outgrows Native. */
    multiarray *vertices;
    int32_t count;
    int32_t rowMax;
} VertexBuffer;

VertexBuffer *vertexBufferNew(void) {
    VertexBuffer *vb = zcalloc(1, sizeof(*vb));

    /* Takes the element TYPE and allocates room for exactly one element. */
    vb->vertices = multiarrayNativeNew(Vertex);
    vb->count = 0;
    vb->rowMax = 1024;
    return vb;
}

void vertexBufferAdd(VertexBuffer *vb, const Vertex *v) {
    /* Appends at index vb->count and increments vb->count for us. */
    multiarrayNativeInsert(vb->vertices, Vertex, vb->rowMax, vb->count,
                           vb->count, v);
}

void vertexBufferAddTriangle(VertexBuffer *vb, const Vertex *v0,
                             const Vertex *v1, const Vertex *v2) {
    vertexBufferAdd(vb, v0);
    vertexBufferAdd(vb, v1);
    vertexBufferAdd(vb, v2);
}

Vertex *vertexBufferGet(VertexBuffer *vb, int32_t index) {
    /* The Get macros are statements that write into a result variable. */
    Vertex *v = NULL;
    multiarrayNativeGet(vb->vertices, Vertex, v, index);
    return v;
}

void vertexBufferFree(VertexBuffer *vb) {
    if (vb) {
        /* Dispatches on the tag: Native, Medium, or Large. */
        multiarrayNativeFree(vb->vertices);
        zfree(vb);
    }
}

void exampleVertexBuffer(void) {
    VertexBuffer *vb = vertexBufferNew();

    const Vertex v0 = {{-1, -1, 1}, {0, 0, 1}, {0, 0}, 0xFFFFFFFF};
    const Vertex v1 = {{1, -1, 1}, {0, 0, 1}, {1, 0}, 0xFFFFFFFF};
    const Vertex v2 = {{1, 1, 1}, {0, 0, 1}, {1, 1}, 0xFFFFFFFF};

    for (int32_t i = 0; i < 2000; i++) {
        vertexBufferAddTriangle(vb, &v0, &v1, &v2);
    }

    const Vertex *v = vertexBufferGet(vb, 3000);
    printf("vertex 3000 color: 0x%08" PRIx32 "\n", v->color);
    printf("total vertices: %" PRId32 "\n", vb->count);

    vertexBufferFree(vb);
}
```

**Native cost model.** While the array is still Native, every insert is a
`zrealloc()` to exactly `count + 1` elements — there is no geometric
overallocation. For an append-heavy loop that is one allocator round trip per
element. If that matters, either pick a small `rowMax` so the array reaches
Medium quickly, or use the container API, whose Small tier has the same
exact-fit behavior but whose Medium tier appends inside a node.

**Do not** write `multiarrayNativeNew(arr[1024])` expecting 1024 elements of
capacity. The macro is `zcalloc(1, sizeof(what))`, so an array-subscript
expression just yields one element's width.

## Example 3: Event Log (Small tier directly)

When the working set genuinely stays small, use `multiarraySmall` directly and
keep true O(1) indexing. A standalone Small never promotes — only the container
API's `multiarrayInsert()` runs the upgrade check — so this is a deliberate
"stay flat" choice.

```c
#include "multiarraySmall.h"
#include "multiarraySmallInternal.h" /* needed for ->count and ->data */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef enum EventType {
    EVENT_INFO,
    EVENT_WARNING,
    EVENT_ERROR,
    EVENT_DEBUG
} EventType;

typedef struct Event {
    int64_t timestamp;
    uint32_t type;
    uint32_t sourceId;
    char message[128];
} Event;

typedef struct EventLog {
    multiarraySmall *events;
} EventLog;

EventLog *eventLogNew(void) {
    EventLog *log = zcalloc(1, sizeof(*log));

    /* rowMax is unused by a standalone Small; it only matters when the
     * container API upgrades one. */
    log->events = multiarraySmallNew(sizeof(Event), 2048);
    return log;
}

void eventLogAdd(EventLog *log, EventType type, uint32_t sourceId,
                 const char *message) {
    Event e = {.timestamp = (int64_t)time(NULL),
               .type = (uint32_t)type,
               .sourceId = sourceId};
    strncpy(e.message, message, sizeof(e.message) - 1);

    /* Appending at ->count is the cheap direction: no memmove. */
    multiarraySmallInsert(log->events, log->events->count, &e);
}

void eventLogPrint(const EventLog *log) {
    static const char *typeNames[] = {"INFO", "WARN", "ERROR", "DEBUG"};

    for (uint16_t i = 0; i < log->events->count; i++) {
        /* multiarraySmallGet() returns uint8_t *; cast it. */
        const Event *e = (const Event *)multiarraySmallGet(log->events, i);
        printf("[%s] source %" PRIu32 " @ %" PRId64 ": %s\n",
               typeNames[e->type], e->sourceId, e->timestamp, e->message);
    }
}

uint32_t eventLogCountOfType(const EventLog *log, EventType type) {
    uint32_t found = 0;

    for (uint16_t i = 0; i < log->events->count; i++) {
        const Event *e = (const Event *)multiarraySmallGet(log->events, i);
        if (e->type == (uint32_t)type) {
            found++;
        }
    }

    return found;
}

void eventLogFree(EventLog *log) {
    if (log) {
        multiarraySmallFree(log->events);
        zfree(log);
    }
}

void exampleEventLog(void) {
    EventLog *log = eventLogNew();

    eventLogAdd(log, EVENT_INFO, 1, "system started");
    eventLogAdd(log, EVENT_INFO, 2, "connection established");
    eventLogAdd(log, EVENT_WARNING, 1, "high memory usage");
    eventLogAdd(log, EVENT_ERROR, 3, "connection lost");

    eventLogPrint(log);
    printf("errors: %" PRIu32 "\n", eventLogCountOfType(log, EVENT_ERROR));

    eventLogFree(log);
}
```

**Bounds.** `multiarraySmall.count` is a `uint16_t` and
`multiarraySmallInsert()` takes a `uint16_t idx`, so a standalone Small holds at
most 65535 elements. It also asserts `idx <= mar->count` on insert and
`idx < mar->count` with `count > 0` on delete — deleting from an empty Small is
a caller bug, not a no-op.

## Example 4: Particle System (Medium tier, node-aware)

This is the case where reading the Medium struct pays off: physics runs over
every element, so iterate node by node instead of calling `Get()` per index.

```c
#include "multiarrayMedium.h"
#include "multiarrayMediumInternal.h" /* needed for ->node, ->count, ->len */

#include <inttypes.h>
#include <stdio.h>

typedef struct Particle {
    float position[3];
    float velocity[3];
    float life;
    uint32_t color;
} Particle;

typedef struct ParticleSystem {
    multiarrayMedium *particles;
    uint32_t count;  /* ELEMENTS; particles->count is NODES */
    float gravity[3];
} ParticleSystem;

ParticleSystem *particleSystemNew(void) {
    ParticleSystem *ps = zcalloc(1, sizeof(*ps));

    ps->particles = multiarrayMediumNew(sizeof(Particle), 512);
    ps->gravity[1] = -9.8f;
    return ps;
}

void particleSystemEmit(ParticleSystem *ps, const float pos[3],
                        const float vel[3]) {
    Particle p = {.life = 1.0f, .color = 0xFFFFFFFF};

    for (uint32_t i = 0; i < 3; i++) {
        p.position[i] = pos[i];
        p.velocity[i] = vel[i];
    }

    /* Appending at the element count is the cheap position. */
    multiarrayMediumInsert(ps->particles, (int32_t)ps->count, &p);
    ps->count++;
}

/* Advance physics with a single linear pass over the node arrays.
 * Dead particles are collected as global indexes and deleted afterwards,
 * highest index first, so earlier deletions never shift a pending index and
 * node-array reallocation never invalidates a live node pointer. */
void particleSystemUpdate(ParticleSystem *ps, float deltaTime) {
    uint32_t *dead = zmalloc(ps->count * sizeof(*dead));
    uint32_t deadCount = 0;
    uint32_t globalIdx = 0;

    for (uint32_t nodeIdx = 0; nodeIdx < ps->particles->count; nodeIdx++) {
        multiarrayMediumNode *node = &ps->particles->node[nodeIdx];

        for (uint16_t i = 0; i < node->count; i++) {
            Particle *p =
                (Particle *)(node->data + (size_t)i * ps->particles->len);

            for (uint32_t axis = 0; axis < 3; axis++) {
                p->velocity[axis] += ps->gravity[axis] * deltaTime;
                p->position[axis] += p->velocity[axis] * deltaTime;
            }

            p->life -= deltaTime;
            if (p->life <= 0.0f) {
                dead[deadCount++] = globalIdx;
            }

            globalIdx++;
        }
    }

    while (deadCount > 0) {
        deadCount--;
        multiarrayMediumDelete(ps->particles, (int32_t)dead[deadCount]);
        ps->count--;
    }

    zfree(dead);
}

void particleSystemFree(ParticleSystem *ps) {
    if (ps) {
        multiarrayMediumFree(ps->particles);
        zfree(ps);
    }
}

void exampleParticleSystem(void) {
    ParticleSystem *ps = particleSystemNew();

    for (int32_t i = 0; i < 1000; i++) {
        const float pos[3] = {0.0f, 10.0f, 0.0f};
        const float vel[3] = {(float)(i % 7) - 3.0f, (float)(i % 5),
                              (float)(i % 3) - 1.0f};
        particleSystemEmit(ps, pos, vel);
    }

    for (int32_t frame = 0; frame < 100; frame++) {
        particleSystemUpdate(ps, 1.0f / 60.0f);
        printf("frame %" PRId32 ": %" PRIu32 " particles (%" PRIu32
               " nodes)\n",
               frame, ps->count, ps->particles->count);
    }

    particleSystemFree(ps);
}
```

**Why the deferred deletion.** `multiarrayMediumDelete()` can free a node whose
element count reaches zero, which calls `multiarraySmallDirectDelete()` on
`mar->node` — that reallocs the node array and memmoves the nodes after it. Any
`multiarrayMediumNode *` you were holding is invalid afterwards. Collecting
indexes during the read pass and deleting in descending order after it avoids
both the dangling node pointer and the index-shifting problem.

**Why `ps->count` exists separately.** `ps->particles->count` is the number of
**nodes**, not particles. Confusing the two is the most common Medium bug.

## Example 5: Sorted Index with Binary Search (Small tier)

Binary search needs O(1) indexing, which only Native and Small provide — in
Medium and Large every probe costs an O(K) node walk, turning a
`log(n)`-probe search into `log(n) * K` work.

```c
#include "multiarraySmall.h"
#include "multiarraySmallInternal.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

typedef struct IndexEntry {
    uint64_t key;
    uint32_t value;
} IndexEntry;

typedef struct SortedIndex {
    multiarraySmall *entries;
} SortedIndex;

SortedIndex *sortedIndexNew(void) {
    SortedIndex *idx = zcalloc(1, sizeof(*idx));
    idx->entries = multiarraySmallNew(sizeof(IndexEntry), 1024);
    return idx;
}

/* Lower bound: first position whose key is >= 'key'. */
static uint16_t sortedIndexLowerBound(const SortedIndex *idx, uint64_t key) {
    uint16_t left = 0;
    uint16_t right = idx->entries->count;

    while (left < right) {
        const uint16_t mid = left + ((right - left) / 2);
        const IndexEntry *e =
            (const IndexEntry *)multiarraySmallGet(idx->entries, mid);

        if (e->key < key) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    return left;
}

void sortedIndexInsert(SortedIndex *idx, uint64_t key, uint32_t value) {
    const uint16_t pos = sortedIndexLowerBound(idx, key);

    if (pos < idx->entries->count) {
        IndexEntry *e = (IndexEntry *)multiarraySmallGet(idx->entries, pos);
        if (e->key == key) {
            e->value = value; /* in-place update, no reallocation */
            return;
        }
    }

    /* pos <= count, which is exactly the contract multiarraySmallInsert()
     * asserts (count itself means "append"). */
    IndexEntry entry = {.key = key, .value = value};
    multiarraySmallInsert(idx->entries, pos, &entry);
}

bool sortedIndexLookup(const SortedIndex *idx, uint64_t key, uint32_t *value) {
    const uint16_t pos = sortedIndexLowerBound(idx, key);

    if (pos < idx->entries->count) {
        const IndexEntry *e =
            (const IndexEntry *)multiarraySmallGet(idx->entries, pos);
        if (e->key == key) {
            *value = e->value;
            return true;
        }
    }

    return false;
}

void sortedIndexFree(SortedIndex *idx) {
    if (idx) {
        multiarraySmallFree(idx->entries);
        zfree(idx);
    }
}

void exampleSortedIndex(void) {
    SortedIndex *idx = sortedIndexNew();

    const uint64_t keys[] = {50, 10, 90, 30, 70, 20, 80, 40, 60, 100};
    for (uint32_t i = 0; i < 10; i++) {
        sortedIndexInsert(idx, keys[i], i);
    }

    uint32_t value = 0;
    if (sortedIndexLookup(idx, 70, &value)) {
        printf("key 70 -> value %" PRIu32 "\n", value);
    }

    /* Range [30, 80) via two lower-bound probes. */
    const uint16_t start = sortedIndexLowerBound(idx, 30);
    const uint16_t end = sortedIndexLowerBound(idx, 80);
    for (uint16_t i = start; i < end; i++) {
        const IndexEntry *e =
            (const IndexEntry *)multiarraySmallGet(idx->entries, i);
        printf("  key %" PRIu64 " -> %" PRIu32 "\n", e->key, e->value);
    }

    sortedIndexFree(idx);
}
```

Every insert here is an O(n) memmove plus a `zrealloc()`, so this pattern suits
read-heavy indexes. It also caps out at 65535 entries with the `uint16_t`
counters.

## Example 6: Large Dataset Node Walk

The right way to consume a Large: never index it. Walk the XOR list once and
process each node's contiguous data array.

```c
#include "multiarrayLarge.h"
#include "multiarrayLargeInternal.h" /* head, tail, prevNext, count */

#include <inttypes.h>
#include <stdio.h>

typedef struct Record {
    uint64_t id;
    uint32_t category;
    double score;
} Record;

void processLargeDataset(void) {
    multiarrayLarge *records = multiarrayLargeNew(sizeof(Record), 2048);

    for (int32_t i = 0; i < 10000000; i++) {
        const Record r = {.id = (uint64_t)i,
                          .category = (uint32_t)(i % 100),
                          .score = (double)(i % 1000) / 10.0};

        multiarrayLargeInsert(records, i, &r); /* append */
    }

    /* XOR traversal: you must carry 'prev' with you.
     * next = prev ^ current->prevNext, starting from head with prev == NULL. */
    multiarrayLargeNode *prev = NULL;
    multiarrayLargeNode *current = records->head;

    double totalScore = 0.0;
    uint64_t count = 0;
    uint64_t nodes = 0;

    while (current) {
        for (uint64_t i = 0; i < current->count; i++) {
            const Record *r =
                (const Record *)(current->data + i * records->len);
            totalScore += r->score;
            count++;
        }

        nodes++;

        multiarrayLargeNode *next = (multiarrayLargeNode *)((uintptr_t)prev ^
                                                            current->prevNext);
        prev = current;
        current = next;
    }

    printf("processed %" PRIu64 " records across %" PRIu64 " nodes\n", count,
           nodes);
    printf("average score: %.2f\n", totalScore / (double)count);

    /* Ends are always O(1), regardless of size. */
    const Record *first = (const Record *)multiarrayLargeGetHead(records);
    const Record *last = (const Record *)multiarrayLargeGetTail(records);
    printf("first id %" PRIu64 ", last id %" PRIu64 "\n", first->id, last->id);

    multiarrayLargeFree(records);
}
```

**The XOR contract.** `prevNext` is a 48-bit bitfield sharing its 8-byte word
with the node's 16-bit `count`, so the whole node is 16 bytes. Reading it back
as a pointer works because the stored value is `prev XOR next` and both fit in
48 bits on supported 64-bit targets. The consequence for callers: you cannot
resume a walk from an arbitrary node — only from `head` (with `prev == NULL`) or
`tail` (with `next == NULL`), carrying the neighbour along.

## Performance Notes Specific to multiarray

### Never index a chunked tier in a loop

`multiarrayMediumGetForward()` and `multiarrayLargeGetForward()` keep **no state
between calls**. They are the forward-only entry points — no negative-index
handling, plus a single-node fast path in Large — not stateful iterators.

```c
/* O(E * K): every call rescans from the first node. */
for (uint32_t i = 0; i < count; i++) {
    void *elem = multiarrayLargeGetForward(large, i);
}

/* O(E): one pass over the node list, contiguous within each node.  A
 * container array does this with its iterator; a Native Large array walks
 * its nodes as Example 6 does. */
multiarrayIter it;
multiarrayIteratorInit(arr, &it, true);
for (void *elem; (elem = multiarrayIteratorNext(&it));) {
}
```

Medium has the same property, with the node scan running over a contiguous node
array instead of a pointer chain.

### rowMax controls node size, not total capacity

`rowMax` is the per-node element capacity. It also decides when promotion
happens, and the two ladders test it differently:

- Container ladder: Small -> Medium at `count == rowMax` **elements**; Medium ->
  Large at `count == rowMax` **nodes**.
- Native ladder: Native -> Medium at `count == rowMax` elements; Medium -> Large
  when `sizeof(void *) * nodeCount > sizeof(element) * rowMax`, which also
  depends on the element width.

Aiming for roughly 8 KB - 16 KB per node (`rowMax * sizeof(element)`) matches
the sizing guidance in `multiarraySmall.h`.

### Append is the cheap direction everywhere

Head insertion memmoves a whole Small array. In Medium and Large it memmoves the
whole head node while that node has room, and once the head node is full at
`rowMax` every further head insert allocates a fresh single-element node in
front of it — plus, in Medium, a realloc and memmove of the node array.
Appending writes into the tail node instead, and a full tail node splits off a
single-element node with nothing to copy.

### A drained array moves back down

`multiarrayDelete()`, `multiarrayPop()` and `multiarrayDeleteRange()` free
emptied nodes, merge sparse neighbours, and move the array back down its
tiers well below where it grew: Large at `rowMax / 2` nodes or fewer becomes
Medium, and Medium holding `rowMax / 2` elements or fewer in at most two nodes
becomes Small. A container that spiked to ten million elements and drained
back to ten ends as one Small array. The gap between the growth and shrink
points means one insert after a shrink never grows the array back.

```c
multiarray *m = multiarrayNew(sizeof(uint64_t), 64);
/* ... grows to Large ... */
multiarrayDeleteRange(&m, 0, multiarrayCount(m) - 10); /* may rewrite m */
/* m is Small again */
```

The Native macro API keeps its own ladder (Native -> Medium -> Large) and
never demotes; keep a native array on the macros.

## See Also

- [MULTIARRAY.md](MULTIARRAY.md) - Complete API reference
- [VARIANTS.md](VARIANTS.md) - Native, Small, Medium, and Large in detail
- [databox](../core/DATABOX.md) - Universal container type
