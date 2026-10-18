# Multilist Examples - Module Walkthroughs

## Overview

This document holds **multilist-specific** walkthroughs: patterns where the
interesting part is how multilist behaves (variant upgrades, `mflexState`
lifetime, `databox` ownership on pop, compression, iterator invalidation).

Generic cross-module recipes - iteration skeletons, memory-management rules,
benchmark harnesses shared with flex/multimap/multiarray - live in
[docs/examples/PATTERNS.md](../../examples/PATTERNS.md),
[docs/examples/USE_CASES.md](../../examples/USE_CASES.md), and
[docs/examples/BENCHMARKS.md](../../examples/BENCHMARKS.md). They are not
repeated here.

Every example below assumes these headers:

```c
#include "multilist.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
```

## Ground Rules These Examples Follow

Four rules explain most of the code shape in this file:

1. **`mflexState` is expensive scratch memory.** `mflexStateCreate()` allocates
   two working buffers. Create it once per owner (or per thread), not per
   operation. Small and Medium lists ignore it, but any list can become Full on
   the next push, so it must still be valid.
2. **Push takes one state, insert/iterate/rotate take `mflexState *state[2]`.**
   Keep both around: `mflexState *s[2]` plus `s[0]` for the single-state calls.
3. **Pop copies; index/iterate borrow.** `multilistPop()` fills a box you must
   release with `databoxFreeData()`. `multilistIndex()` and `multilistNext()`
   fill a box pointing into list memory that you must _not_ free and that dies
   at the next mutation.
4. **Read popped bytes with `databoxCBytes()`.** Popped byte values of 8 bytes
   or fewer are embedded in the box (`DATABOX_BYTES_EMBED`), so
   `.data.bytes.cstart` is wrong for them.

## Basic Examples

### Example 1: Task Queue (FIFO) with a Shared State

The point of this example is state lifetime: one `mflexState` for the whole
queue, not one per push.

```c
typedef struct taskQueue {
    multilist *ml;
    mflexState *s[2];
} taskQueue;

void taskQueueInit(taskQueue *q) {
    q->ml = multilistNew(FLEX_CAP_LEVEL_2048, 0);
    q->s[0] = mflexStateCreate();
    q->s[1] = mflexStateCreate();
}

void taskQueueDestroy(taskQueue *q) {
    multilistFree(q->ml);
    mflexStateFree(q->s[0]);
    mflexStateFree(q->s[1]);
}

void taskQueueSubmit(taskQueue *q, const char *description) {
    /* databoxNewBytesString only points at 'description';
     * the push copies the bytes into the flex node. */
    databox task = databoxNewBytesString(description);
    multilistPushByTypeTail(&q->ml, q->s[0], &task);
}

bool taskQueueRun(taskQueue *q) {
    databox got;
    if (!multilistPopHead(&q->ml, q->s[0], &got)) {
        return false;
    }

    printf("Processing: %.*s\n", (int)got.len, databoxCBytes(&got));

    /* pop allocated (or embedded) a copy - release it */
    databoxFreeData(&got);
    return true;
}

void taskQueueExample(void) {
    taskQueue q;
    taskQueueInit(&q);

    for (uint32_t i = 0; i < 100; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Process item %" PRIu32, i);
        taskQueueSubmit(&q, buf);
    }

    printf("Queue has %zu tasks\n", multilistCount(q.ml));

    while (taskQueueRun(&q)) {
        /* keep draining */
    }

    taskQueueDestroy(&q);
}
```

By the 100th push this list has already crossed 2 KB and become Medium. Nothing
in the code above changed - only the pointer written back through `&q->ml`.

### Example 2: Undo/Redo Stacks and Box Ownership

Moving a value from one list to another is where pop-copy ownership matters: the
push copies the bytes again, so the popped box still has to be freed.

```c
typedef struct editor {
    multilist *undo;
    multilist *redo;
    mflexState *state;
} editor;

void editorInit(editor *e) {
    e->undo = multilistNew(FLEX_CAP_LEVEL_2048, 0);
    e->redo = multilistNew(FLEX_CAP_LEVEL_2048, 0);
    e->state = mflexStateCreate();
}

void editorRecord(editor *e, const char *action) {
    databox box = databoxNewBytesString(action);
    multilistPushByTypeHead(&e->undo, e->state, &box);
}

bool editorUndo(editor *e) {
    databox undone;
    if (!multilistPopHead(&e->undo, e->state, &undone)) {
        return false;
    }

    printf("Undoing: %.*s\n", (int)undone.len, databoxCBytes(&undone));

    /* The push copies the bytes into the redo list... */
    multilistPushByTypeHead(&e->redo, e->state, &undone);

    /* ...so the popped copy is still ours to release. */
    databoxFreeData(&undone);
    return true;
}

bool editorRedo(editor *e) {
    databox redone;
    if (!multilistPopHead(&e->redo, e->state, &redone)) {
        return false;
    }

    printf("Redoing: %.*s\n", (int)redone.len, databoxCBytes(&redone));

    multilistPushByTypeHead(&e->undo, e->state, &redone);
    databoxFreeData(&redone);
    return true;
}

void editorDestroy(editor *e) {
    multilistFree(e->undo);
    multilistFree(e->redo);
    mflexStateFree(e->state);
}
```

Forgetting the `databoxFreeData()` after re-pushing is the single most common
multilist leak, because the code reads as if the push took ownership. It does
not.

### Example 3: Deque for Work Stealing

Both ends are O(1), which is what makes a work-stealing deque viable. Note the
direction convention: the owner takes from the end it pushes to (LIFO, hot in
cache), and thieves take from the far end (FIFO, oldest work).

```c
typedef struct workDeque {
    multilist *ml;
    mflexState *state;
} workDeque;

void workDequePush(workDeque *d, int64_t item) {
    databox work = databoxNewSigned(item);
    multilistPushByTypeTail(&d->ml, d->state, &work);
}

/* Owner: take from the tail - the most recently pushed item. */
bool workDequeTakeOwn(workDeque *d, int64_t *out) {
    databox got;
    if (!multilistPopTail(&d->ml, d->state, &got)) {
        return false;
    }

    *out = got.data.i64;
    printf("Owner took: %" PRId64 "\n", got.data.i64);
    databoxFreeData(&got);
    return true;
}

/* Thief: take from the head - the oldest, coldest item. */
bool workDequeSteal(workDeque *d, int64_t *out) {
    databox got;
    if (!multilistPopHead(&d->ml, d->state, &got)) {
        return false;
    }

    *out = got.data.i64;
    printf("Thief stole: %" PRId64 "\n", got.data.i64);
    databoxFreeData(&got);
    return true;
}
```

Integers pushed with `databoxNewSigned()` come back as `DATABOX_SIGNED_64`;
read `got.data.i64` and print with `PRId64`. `databoxFreeData()` on an integer
box is a safe no-op, so the release call can stay unconditional.

Real work stealing needs external synchronization - multilist itself is not
thread-safe and `mflexState` cannot be shared across threads.

## Intermediate Examples

### Example 4: Search-and-Promote (Iterator Invalidation)

This is the canonical demonstration of what you may and may not do while an
iterator is live: **delete is supported, insert/push is not**.

```c
typedef struct recentList {
    multilist *ml;
    mflexState *s[2];
    size_t maxEntries;
} recentList;

/* Move 'key' to the front if present; otherwise insert it and trim. */
void recentListTouch(recentList *r, const char *key) {
    const size_t keyLen = strlen(key);
    bool found = false;

    multilistIterator iter = {0};
    multilistIteratorInitForward(r->ml, r->s, &iter);

    multilistEntry entry;
    while (multilistNext(&iter, &entry)) {
        if (entry.box.len == keyLen &&
            !memcmp(entry.box.data.bytes.cstart, key, keyLen)) {
            /* Deleting during iteration is fine: multilistDelEntry()
             * repositions the iterator for the current direction. */
            multilistDelEntry(&iter, &entry);
            found = true;

            /* But we are about to PUSH, which may split a node or
             * upgrade the variant, so stop iterating first. */
            break;
        }
    }

    multilistIteratorRelease(&iter);

    databox keyBox = databoxNewBytesString(key);
    multilistPushByTypeHead(&r->ml, r->s[0], &keyBox);

    if (!found && multilistCount(r->ml) > r->maxEntries) {
        databox evicted;
        if (multilistPopTail(&r->ml, r->s[0], &evicted)) {
            printf("Evicting: %.*s\n", (int)evicted.len,
                   databoxCBytes(&evicted));
            databoxFreeData(&evicted);
        }
    }
}
```

Two multilist-specific details:

- The push happens **after** `multilistIteratorRelease()`. Pushing while the
  iterator is live is undefined: the push can reallocate the flex the iterator's
  `fe` points into, or replace the whole variant.
- The comparison uses `entry.box.len` + `memcmp`, not `strcmp`. Entry bytes are
  not NUL-terminated.

This is a linear scan per touch, so it is a teaching example, not a real cache.
For an actual LRU use [multilru](../multi/MULTILRU.md).

### Example 5: Bounded Buffer with Count-Based Eviction

The generic bounded-buffer recipe is in
[docs/examples/PATTERNS.md](../../examples/PATTERNS.md); what is worth saying
here is the multilist-specific caveat.

```c
void boundedAppend(multilist **ml, mflexState *state, const void *data,
                   size_t len, size_t maxEntries) {
    databox item = databoxNewBytes(data, len);
    multilistPushByTypeTail(ml, state, &item);

    while (multilistCount(*ml) > maxEntries) {
        databox old = {{0}};
        if (!multilistPopHead(ml, state, &old)) {
            break;
        }

        databoxFreeData(&old);
    }
}
```

**Caveat:** a cap expressed in _elements_ does not cap memory, because node
count and node occupancy follow byte size, not element count. If what you
actually need to bound is memory, test `multilistBytes()` instead - but remember
that `multilistBytes()` reports uncompressed logical bytes, so with a
compression depth set it will not track real footprint either.

Also note the eviction loop uses `while`, not `if`: a single push can only add
one element, but reusing this helper after a bulk load needs to drain more than
one.

### Example 6: Round-Robin with `multilistRotate()`

`multilistRotate()` moves the **tail** element to the **head**. That direction
is the whole content of this example - reading index 0 and then rotating gives
you the same element forever.

```c
typedef struct rotation {
    multilist *ml;
    mflexState *s[2];
} rotation;

void rotationStep(rotation *r) {
    multilistEntry entry;

    /* Read the TAIL, because that is what rotate is about to move. */
    if (!multilistIndexGet(r->ml, r->s[0], -1, &entry)) {
        printf("Nothing to rotate\n");
        return;
    }

    printf("Now current: %.*s\n", (int)entry.box.len,
           entry.box.data.bytes.cstart);

    /* Tail becomes the new head; the previous tail-1 becomes the tail. */
    multilistRotate(r->ml, r->s);
}

void rotationExample(void) {
    rotation r = {.ml = multilistNew(FLEX_CAP_LEVEL_2048, 0),
                  .s = {mflexStateCreate(), mflexStateCreate()}};

    const char *tracks[] = {"Track A", "Track B", "Track C", "Track D"};
    for (size_t i = 0; i < sizeof(tracks) / sizeof(*tracks); i++) {
        databox box = databoxNewBytesString(tracks[i]);
        multilistPushByTypeTail(&r.ml, r.s[0], &box);
    }

    /* Visits D, C, B, A, D, C, B, A */
    for (uint32_t step = 0; step < 8; step++) {
        rotationStep(&r);
    }

    multilistFree(r.ml);
    mflexStateFree(r.s[0]);
    mflexStateFree(r.s[1]);
}
```

`multilistRotate()` takes `multilist *`, not `multilist **`: rotation cannot
change the element count, so it can never trigger a variant upgrade. Rotating a
list of 0 or 1 elements is a no-op.

## Advanced Examples

### Example 7: Append-Only Log with Interior Compression

The compression depth only becomes meaningful once a list is Full and has more
than `depth * 2` nodes, which makes an append-heavy log the natural fit: the
recent end stays hot and uncompressed, and the cold interior compresses.

```c
typedef struct logBuffer {
    multilist *ml;
    mflexState *s[2];
    size_t totalLogs;
} logBuffer;

void logBufferInit(logBuffer *lb) {
    /* 8 KB nodes; once Full, keep 5 nodes hot at each end. */
    lb->ml = multilistNew(FLEX_CAP_LEVEL_8192, 5);
    lb->s[0] = mflexStateCreate();
    lb->s[1] = mflexStateCreate();
    lb->totalLogs = 0;
}

void logBufferAppend(logBuffer *lb, const char *level, const char *message) {
    char line[1024];
    const int len = snprintf(line, sizeof(line), "[%s] %s", level, message);
    if (len < 0) {
        return;
    }

    databox entry = databoxNewBytes(line, (size_t)len);
    multilistPushByTypeTail(&lb->ml, lb->s[0], &entry);
    lb->totalLogs++;
}

void logBufferDump(logBuffer *lb, FILE *out) {
    multilistIterator iter = {0};

    /* Read-only iteration: nodes decompressed into the state buffers
     * are not marked dirty, so nothing needs recompressing afterwards. */
    multilistIteratorInitForwardReadOnly(lb->ml, lb->s, &iter);

    multilistEntry entry;
    while (multilistNext(&iter, &entry)) {
        fprintf(out, "%.*s\n", (int)entry.box.len,
                entry.box.data.bytes.cstart);
    }

    multilistIteratorRelease(&iter);
}

void logBufferDestroy(logBuffer *lb) {
    multilistFree(lb->ml);
    mflexStateFree(lb->s[0]);
    mflexStateFree(lb->s[1]);
}
```

Points specific to compressed multilists:

- Use the **read-only** iterator for dumps. The read-write iterator exists so
  `multilistDelEntry()` can work, and it forces the touched nodes back through
  the compression path.
- `multilistIteratorRelease()` matters here: for Full lists it is what releases
  the iterator's hold on the decompression scratch buffers. For Small and
  Medium it is a no-op, but writing it unconditionally is what makes the code
  survive an upgrade.
- Appending never decompresses an interior node, because the tail node is
  always uncompressed.

### Example 8: Typed Time-Series Entries

Instead of `snprintf`-ing a record into a string, push each field as its own
typed `databox`. flex encodes integers and floats in their compact numeric
forms, so this is both smaller and faster than string packing - and it removes
the `sscanf` round-trip entirely.

```c
typedef struct timeSeries {
    multilist *ml;
    mflexState *s[2];
} timeSeries;

/* Each sample occupies two consecutive elements: timestamp, then value. */
void timeSeriesAdd(timeSeries *ts, int64_t timestamp, double value) {
    databox stampBox = databoxNewSigned(timestamp);
    multilistPushByTypeTail(&ts->ml, ts->s[0], &stampBox);

    databox valueBox = databoxNewReal(value);
    multilistPushByTypeTail(&ts->ml, ts->s[0], &valueBox);
}

static double sampleValue(const databox *box) {
    /* databoxNewReal() narrows to DATABOX_FLOAT_32 when the double
     * round-trips exactly, so handle both float widths. */
    if (box->type == DATABOX_FLOAT_32) {
        return (double)box->data.f32;
    }

    return box->data.d64;
}

void timeSeriesQuery(timeSeries *ts, int64_t startTime, int64_t endTime) {
    multilistIterator iter = {0};
    multilistIteratorInitForwardReadOnly(ts->ml, ts->s, &iter);

    multilistEntry entry;
    while (multilistNext(&iter, &entry)) {
        const int64_t stamp = entry.box.data.i64;

        if (!multilistNext(&iter, &entry)) {
            break; /* trailing timestamp with no value */
        }

        const double value = sampleValue(&entry.box);

        if (stamp >= startTime && stamp <= endTime) {
            printf("  %" PRId64 ": %.6f\n", stamp, value);
        }
    }

    multilistIteratorRelease(&iter);
}

double timeSeriesAverage(timeSeries *ts) {
    multilistIterator iter = {0};
    multilistIteratorInitForwardReadOnly(ts->ml, ts->s, &iter);

    double sum = 0.0;
    size_t count = 0;

    multilistEntry entry;
    while (multilistNext(&iter, &entry)) {
        if (!multilistNext(&iter, &entry)) {
            break;
        }

        sum += sampleValue(&entry.box);
        count++;
    }

    multilistIteratorRelease(&iter);

    return count > 0 ? sum / (double)count : 0.0;
}
```

Because a pair is two elements, `multilistCount()` is twice the sample count and
`multilistIndexGet(ml, s[0], 2 * n, &entry)` addresses sample `n`'s timestamp.

Watch the type discipline: a timestamp pushed with `databoxNewSigned()` comes
back as `DATABOX_SIGNED_64`, but a _string_ of digits pushed with
`databoxNewBytesString()` also comes back as an integer box, because flex stores
it in its integer encoding. If you need "these bytes stay bytes", do not feed
the container a bare numeric string.

### Example 9: Several Lists Sharing One State

Three priority lanes, one `mflexState`. Creating a state per call - the obvious
first draft - allocates and frees two buffers on every enqueue.

```c
typedef enum priority {
    PRIORITY_LOW = 0,
    PRIORITY_MEDIUM = 1,
    PRIORITY_HIGH = 2,
    PRIORITY_COUNT = 3
} priority;

typedef struct priorityQueue {
    multilist *lane[PRIORITY_COUNT];
    mflexState *state;
} priorityQueue;

void priorityQueueInit(priorityQueue *pq) {
    for (size_t i = 0; i < PRIORITY_COUNT; i++) {
        pq->lane[i] = multilistNew(FLEX_CAP_LEVEL_2048, 0);
    }

    pq->state = mflexStateCreate();
}

void priorityQueueEnqueue(priorityQueue *pq, priority p, const char *message) {
    databox msg = databoxNewBytesString(message);
    multilistPushByTypeTail(&pq->lane[p], pq->state, &msg);
}

/* On success the caller owns 'result' and must databoxFreeData() it. */
bool priorityQueueDequeue(priorityQueue *pq, databox *result) {
    for (int32_t p = PRIORITY_HIGH; p >= PRIORITY_LOW; p--) {
        if (multilistPopHead(&pq->lane[p], pq->state, result)) {
            return true;
        }
    }

    return false;
}

void priorityQueueDestroy(priorityQueue *pq) {
    for (size_t i = 0; i < PRIORITY_COUNT; i++) {
        multilistFree(pq->lane[i]);
    }

    mflexStateFree(pq->state);
}
```

Sharing one state across lists is safe because the state is scratch space used
only for the duration of a single call. Sharing it across _threads_ is not.

### Example 10: Cursor Navigation and Borrowed Entry Lifetime

A back/forward cursor over a list. The trap this example exists to show:
`multilistIndexGet()` hands you a pointer into list memory, so returning
`entry.box.data.bytes.cstart` to a caller who then mutates the list is a
dangling read.

```c
typedef struct history {
    multilist *ml;
    mflexState *s[2];
    size_t cursor; /* index of the current entry */
} history;

void historyVisit(history *h, const char *url) {
    /* Drop everything after the cursor - the forward branch is dead. */
    const size_t total = multilistCount(h->ml);
    if (total > h->cursor + 1) {
        const int64_t toDelete = (int64_t)(total - h->cursor - 1);
        multilistDelRange(&h->ml, h->s[0], (mlOffsetId)(h->cursor + 1),
                          toDelete);
    }

    databox page = databoxNewBytesString(url);
    multilistPushByTypeTail(&h->ml, h->s[0], &page);

    h->cursor = multilistCount(h->ml) - 1;
}

/* Copies the current entry into 'out' (caller-provided buffer) because the
 * entry box borrows list memory and dies on the next mutation. */
bool historyCurrent(history *h, char *out, size_t outLen) {
    multilistEntry entry;
    if (!multilistIndexGet(h->ml, h->s[0], (mlOffsetId)h->cursor, &entry)) {
        return false;
    }

    const size_t copyLen =
        entry.box.len < outLen - 1 ? entry.box.len : outLen - 1;
    memcpy(out, entry.box.data.bytes.cstart, copyLen);
    out[copyLen] = '\0';
    return true;
}

bool historyBack(history *h) {
    if (h->cursor == 0) {
        return false;
    }

    h->cursor--;
    return true;
}

bool historyForward(history *h) {
    const size_t total = multilistCount(h->ml);
    if (total == 0 || h->cursor + 1 >= total) {
        return false;
    }

    h->cursor++;
    return true;
}
```

Two fixes worth copying:

- `h->cursor + 1 >= total` instead of `cursor < total - 1`. With `size_t`,
  `total - 1` on an empty list wraps to `SIZE_MAX` and the bound check passes.
- `multilistDelRange()` takes a _count_, not an end index, and its `start`
  accepts negative values counting from the tail.

## Performance Examples

### Example 11: Measuring the Effect of the Size Limit

All of these end up as Full lists; what differs is node size, and therefore how
often a node splits and how large each memmove is.

```c
#include <time.h>

double measurePushPerformance(flexCapSizeLimit limit, size_t operations) {
    multilist *ml = multilistNew(limit, 0);
    mflexState *state = mflexStateCreate();

    const clock_t start = clock();

    for (size_t i = 0; i < operations; i++) {
        databox item = databoxNewSigned((int64_t)i);
        multilistPushByTypeTail(&ml, state, &item);
    }

    const clock_t end = clock();
    const double elapsed = ((double)(end - start)) / CLOCKS_PER_SEC;

    printf("Limit level %d: %zu ops in %.3f sec (%.0f ops/sec)\n", (int)limit,
           operations, elapsed, (double)operations / elapsed);
    printf("  Final: %zu elements, %zu logical bytes\n", multilistCount(ml),
           multilistBytes(ml));

    mflexStateFree(state);
    multilistFree(ml);

    return elapsed;
}

void benchmarkExample(void) {
    const size_t operations = 1000000;

    measurePushPerformance(FLEX_CAP_LEVEL_512, operations);
    measurePushPerformance(FLEX_CAP_LEVEL_2048, operations);
    measurePushPerformance(FLEX_CAP_LEVEL_8192, operations);
}
```

Build with the cmake `perf` preset before believing any of these numbers -
sanitizer builds add 3-10x overhead and change the ranking.

### Example 12: What Compression Does and Does Not Show Up In

A common mistake is to compare `multilistBytes()` between a compressed and an
uncompressed list and conclude compression is broken. It is not: for the Full
variant `multilistBytes()` sums `mflexBytesUncompressed()` over the nodes, so it
deliberately reports **logical** size and is identical either way.

```c
void compressionExample(void) {
    multilist *plain = multilistNew(FLEX_CAP_LEVEL_4096, 0);
    multilist *packed = multilistNew(FLEX_CAP_LEVEL_4096, 2);

    mflexState *s1 = mflexStateCreate();
    mflexState *s2 = mflexStateCreate();

    for (uint32_t i = 0; i < 100000; i++) {
        char buf[200];
        const int len =
            snprintf(buf, sizeof(buf),
                     "This is a log message with repeated text. "
                     "Entry number %" PRIu32 ". Status: OK. Server: web-01",
                     i);
        if (len < 0) {
            continue;
        }

        databox item = databoxNewBytes(buf, (size_t)len);
        multilistPushByTypeTail(&plain, s1, &item);
        multilistPushByTypeTail(&packed, s2, &item);
    }

    /* These two print the SAME number. That is correct behavior. */
    printf("plain  logical bytes: %zu\n", multilistBytes(plain));
    printf("packed logical bytes: %zu\n", multilistBytes(packed));

    mflexStateFree(s1);
    mflexStateFree(s2);
    multilistFree(plain);
    multilistFree(packed);
}
```

To observe the actual saving you have to look outside the multilist API - at
process RSS, or at allocator statistics around the two lists - because no public
function exposes the compressed footprint of a multilist. The per-node
primitives `mflexBytesCompressed()` and `mflexBytesActual()` exist in
[mflex](../flex/MFLEX.md), but multilist does not aggregate them for you.

## Pattern Index

| Pattern               | Operations                          | Example |
| --------------------- | ----------------------------------- | ------- |
| Queue (FIFO)          | PushTail + PopHead                  | 1       |
| Stack / undo-redo     | PushHead + PopHead across two lists | 2       |
| Deque                 | Push/Pop at both ends               | 3       |
| Search-and-promote    | Iterate + DelEntry + PushHead       | 4       |
| Bounded buffer        | PushTail + PopHead under a cap      | 5       |
| Round-robin           | IndexGet(-1) + Rotate               | 6       |
| Compressed append log | PushTail + read-only iteration      | 7       |
| Typed records         | Paired typed databox elements       | 8       |
| Multi-lane priority   | Several lists, one shared state     | 9       |
| Cursor navigation     | IndexGet + DelRange + PushTail      | 10      |

## See Also

- [MULTILIST.md](MULTILIST.md) - Complete API reference
- [VARIANTS.md](VARIANTS.md) - Small/Medium/Full internals and upgrade rules
- [databox](../core/DATABOX.md) - Value container and ownership rules
- [flex](../flex/FLEX.md) - Underlying storage format
- [mflex](../flex/MFLEX.md) - Managed flex and compression primitives
- [Generic patterns](../../examples/PATTERNS.md) - Cross-module recipes
- [Benchmarks](../../examples/BENCHMARKS.md) - Cross-module benchmark harnesses
