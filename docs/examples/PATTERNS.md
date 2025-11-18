# Common Coding Patterns

This guide demonstrates frequently-used coding patterns across datakit modules, providing copy-paste-ready examples for common scenarios.

## Table of Contents

1. [Container Creation Patterns](#container-creation-patterns)
2. [Iteration Patterns](#iteration-patterns)
3. [Memory Management Patterns](#memory-management-patterns)
4. [Error Handling Patterns](#error-handling-patterns)
5. [Data Transformation Patterns](#data-transformation-patterns)
6. [Caching Patterns](#caching-patterns)
7. [Time-Series Patterns](#time-series-patterns)
8. [String Building Patterns](#string-building-patterns)
9. [Set Operations Patterns](#set-operations-patterns)
10. [Compression Patterns](#compression-patterns)

---

## Container Creation Patterns

### Pattern: Initialize to NULL Before Creation

Always initialize container pointers to NULL before creating them, especially for containers using double-pointer APIs.

```c
/* WRONG - uninitialized pointer */
multimap *m;
multimapNew(&m);  /* m contains garbage - undefined behavior! */

/* RIGHT - initialize to NULL */
multimap *m = NULL;
multimapNew(&m);  /* Safe initialization */
```

### Pattern: Choose Right Variant for Data Size

Select the appropriate size variant based on expected data volume:

```c
/* Small dataset (< 1KB) */
multimapSmall *sm = multimapSmallNew();

/* Medium dataset (1KB - 100KB) */
multimapMedium *mm = multimapMediumNew();

/* Large/unbounded dataset (> 100KB) */
multimap *lm = NULL;
multimapNew(&lm);  /* Uses multimapFull internally */

/* Small, fixed-size array */
MyStruct *arr = multiarrayNativeNew(arr[1024]);

/* Dynamic array with expected size */
multiarray *dyn = multiarrayNew(sizeof(Record), 2048);
```

### Pattern: Pre-allocate for Known Sizes

Avoid multiple reallocations by pre-sizing containers:

```c
/* multimap with expected capacity */
multimap *m = multimapNewLimit(2, FLEX_CAP_LEVEL_4096);
/* Or expand after creation */
multimapExpand(m, 10000);

/* multiarray with initial capacity */
multiarray *arr = multiarrayNew(sizeof(int64_t), 10000);

/* flex with reserved space */
flex *f = flexNew();
flexReserve(&f, 1000);  /* Reserve space for 1000 elements */
```

---

## Iteration Patterns

### Pattern: Forward Iteration with Iterator

Use iterators for efficient sequential access:

```c
/* multimap iteration */
multimap *m = /* ... */;
multimapIterator iter;
multimapIteratorInit(m, &iter, true);  /* true = forward */

databox key, value;
databox *elements[2] = {&key, &value};

while (multimapIteratorNext(&iter, elements)) {
    printf("Key: %.*s, Value: %.*s\n",
           (int)databoxLen(&key), databoxCBytes(&key),
           (int)databoxLen(&value), databoxCBytes(&value));
}

/* multilist iteration */
multilist *ml = /* ... */;
mflexState *state = mflexStateCreate();
multimapIterator mlIter;
multilistIteratorInitForward(ml, state, &mlIter);

multilistEntry entry;
while (multilistNext(&mlIter, &entry)) {
    /* Process entry.box */
}

multilistIteratorRelease(&mlIter);
mflexStateFree(state);

/* flex iteration */
flex *f = /* ... */;
flexEntry *fe = flexHead(f);
while (fe && flexEntryIsValid(f, fe)) {
    databox box;
    flexGetByType(fe, &box);
    /* Process box */
    fe = flexNext(f, fe);
}
```

### Pattern: Reverse Iteration

Iterate in reverse order:

```c
/* multimap reverse */
multimapIterator iter;
multimapIteratorInit(m, &iter, false);  /* false = reverse */

databox key, value;
databox *elements[2] = {&key, &value};
while (multimapIteratorNext(&iter, elements)) {
    /* Process in reverse order */
}

/* multilist reverse */
multilistIteratorInitReverse(ml, state, &iter);
while (multilistNext(&iter, &entry)) {
    /* Process in reverse */
}

/* flex reverse */
flexEntry *fe = flexTail(f);
while (fe && flexEntryIsValid(f, fe)) {
    databox box;
    flexGetByType(fe, &box);
    /* Process in reverse */
    fe = flexPrev(f, fe);
}
```

### Pattern: Iteration with Modification

Safely modify containers during iteration:

```c
/* multidict safe iteration (can delete during iteration) */
multidict *d = /* ... */;
multidictIterator iter;
multidictIteratorGetSafe(d, &iter);

multidictEntry entry;
while (multidictIteratorNext(&iter, &entry)) {
    /* Can safely call multidictDelete here */
    if (shouldDelete(&entry.key)) {
        multidictDelete(d, &entry.key);
    }
}
multidictIteratorRelease(&iter);

/* Collect-then-delete pattern for other containers */
databox toDelete[1000];
size_t deleteCount = 0;

multimapIterator iter;
multimapIteratorInit(m, &iter, true);
databox key, value;
databox *elements[2] = {&key, &value};

while (multimapIteratorNext(&iter, elements)) {
    if (shouldDelete(&value)) {
        toDelete[deleteCount++] = key;
        if (deleteCount >= 1000) break;
    }
}

for (size_t i = 0; i < deleteCount; i++) {
    multimapDelete(&m, &toDelete[i]);
}
```

### Pattern: Indexed Iteration for Arrays

Use index-based iteration for random access containers:

```c
/* multiarray */
size_t count = multiarrayCount(arr);
for (size_t i = 0; i < count; i++) {
    MyStruct *elem = (MyStruct *)multiarrayGet(arr, i);
    /* Process elem */
}

/* intset (sorted) */
for (uint32_t i = 0; i < intsetCount(is); i++) {
    int64_t value;
    if (intsetGet(is, i, &value)) {
        printf("%ld ", value);
    }
}

/* flex with index */
for (size_t i = 0; i < flexCount(f); i++) {
    flexEntry *fe = flexIndex(f, i);
    databox box;
    flexGetByType(fe, &box);
    /* Process box */
}
```

---

## Memory Management Patterns

### Pattern: Proper Cleanup

Always free resources in reverse order of allocation:

```c
/* Create */
multimap *m = NULL;
multimapNew(&m);
mflexState *state = mflexStateCreate();

/* Use */
/* ... */

/* Cleanup in reverse order */
mflexStateFree(state);
multimapFree(m);  /* Also sets m = NULL */
```

### Pattern: Free Popped Data

When popping data that contains allocated memory, free it:

```c
/* multilist pop */
databox result;
if (multilistPopHead(&ml, state, &result)) {
    /* Use result */
    printf("Got: %s\n", result.data.bytes.cstart);

    /* Free if it contains allocated data */
    databoxFreeData(&result);
}

/* Always free iterator data */
databox box;
flexGetByType(fe, &box);
/* Use box */
databoxFreeData(&box);  /* Free if allocated */
```

### Pattern: Efficient Growth with fibbuf + jebuf

Combine Fibonacci growth with jemalloc alignment:

```c
size_t currentSize = 1024;
void *ptr = malloc(currentSize);

/* Need to grow */
size_t fibSize = fibbufNextSizeBuffer(currentSize);
size_t allocSize = jebufSizeAllocation(fibSize);

if (jebufUseNewAllocation(currentSize, allocSize)) {
    ptr = realloc(ptr, allocSize);
    currentSize = allocSize;
}
```

### Pattern: Memory Bounds

Set global memory limits for safety:

```c
/* Set 100MB limit */
memboundSet(100 * 1024 * 1024);

/* Allocations will fail if limit exceeded */
void *ptr = memboundMalloc(1024 * 1024);
if (!ptr) {
    fprintf(stderr, "Memory limit exceeded\n");
    return ERROR_OUT_OF_MEMORY;
}

/* Use regular malloc behavior */
/* ... */

/* Free as normal */
memboundFree(ptr);

/* Check current usage */
size_t used = memboundUsed();
printf("Memory used: %zu bytes\n", used);
```

---

## Error Handling Patterns

### Pattern: Check Return Values

Always check function return values:

```c
/* Boolean returns */
databox key = databoxNewBytesString("username");
databox value;
databox *results[1] = {&value};

if (multimapLookup(m, &key, results)) {
    printf("Found: %.*s\n", (int)databoxLen(&value),
           databoxCBytes(&value));
} else {
    fprintf(stderr, "Key not found\n");
}

/* Pointer returns */
flex *f = flexNew();
if (!f) {
    fprintf(stderr, "Failed to allocate flex\n");
    return NULL;
}

/* Success output parameters */
bool added;
intsetAdd(&is, 42, &added);
if (added) {
    printf("Successfully added\n");
} else {
    printf("Already exists\n");
}
```

### Pattern: Graceful Degradation

Handle edge cases gracefully:

```c
/* Empty container checks */
if (multimapCount(m) == 0) {
    printf("Map is empty\n");
    return;
}

if (flexIsEmpty(f)) {
    printf("Flex is empty\n");
    return;
}

/* Bounds checking */
int64_t value;
if (intsetGet(is, index, &value)) {
    /* Valid index */
} else {
    fprintf(stderr, "Index %u out of bounds (count: %zu)\n",
            index, intsetCount(is));
}

/* NULL checks before operations */
if (m == NULL) {
    fprintf(stderr, "NULL multimap\n");
    return ERROR_NULL_POINTER;
}
```

---

## Data Transformation Patterns

### Pattern: String to Numeric Conversion

Use reliable type detection for parsing:

```c
/* Automatic type detection */
const char *input = "42.5";
databox result;

if (StrScanScanReliable(input, strlen(input), &result)) {
    switch (result.type) {
        case DATABOX_SIGNED_64:
            printf("Integer: %ld\n", result.data.i64);
            break;
        case DATABOX_UNSIGNED_64:
            printf("Unsigned: %lu\n", result.data.u64);
            break;
        case DATABOX_DOUBLE_64:
            printf("Double: %f\n", result.data.d64);
            break;
        default:
            printf("Other type\n");
    }
}

/* Fast integer parsing */
uint64_t value;
if (StrBufToUInt64("12345", 5, &value)) {
    printf("Value: %lu\n", value);
}
```

### Pattern: Numeric to String Conversion

Convert numbers to strings efficiently:

```c
/* Integer to string */
char buf[32];
size_t len = StrInt64ToBuf(buf, sizeof(buf), -12345);
printf("%.*s\n", (int)len, buf);  /* "-12345" */

/* Double to string (nice formatting) */
char dbuf[64];
len = StrDoubleFormatToBufNice(dbuf, sizeof(dbuf), 3.14159);
printf("%.*s\n", (int)len, dbuf);  /* "3.14159" */

/* Double to string (exact) */
len = StrDoubleFormatToBufExact(dbuf, sizeof(dbuf), 3.14159265359);
printf("%.*s\n", (int)len, dbuf);
```

### Pattern: databox Type Checking

Check and extract values from databox:

```c
databox box = /* ... */;

if (DATABOX_IS_SIGNED_INTEGER(&box)) {
    int64_t value = box.data.i64;
    printf("Signed: %ld\n", value);
}

if (DATABOX_IS_UNSIGNED_INTEGER(&box)) {
    uint64_t value = box.data.u64;
    printf("Unsigned: %lu\n", value);
}

if (DATABOX_IS_FLOAT(&box)) {
    double value = box.data.d64;
    printf("Float: %f\n", value);
}

if (DATABOX_IS_BYTES(&box)) {
    const uint8_t *data = databoxCBytes(&box);
    size_t len = databoxLen(&box);
    printf("Bytes: %.*s\n", (int)len, data);
}

if (DATABOX_IS_TRUE(&box)) {
    printf("Boolean: true\n");
}

if (DATABOX_IS_NULL(&box)) {
    printf("NULL value\n");
}
```

---

## Caching Patterns

### Pattern: Simple LRU Cache

Implement basic LRU caching:

```c
typedef struct {
    multimap *data;    /* key → value */
    multilru *lru;     /* LRU tracking */
    size_t maxSize;
} SimpleCache;

SimpleCache *cacheNew(size_t maxSize) {
    SimpleCache *c = malloc(sizeof(*c));
    c->data = multimapNew(2);  /* key + value */
    c->lru = multilruNew();
    c->maxSize = maxSize;
    return c;
}

void cachePut(SimpleCache *c, const databox *key, const databox *value) {
    /* Evict if at capacity */
    while (multilruCount(c->lru) >= c->maxSize) {
        multilruPtr evicted;
        if (multilruRemoveMinimum(c->lru, &evicted)) {
            /* Find and delete corresponding entry from data */
            /* (In real impl, need bidirectional mapping) */
        }
    }

    /* Insert */
    const databox *kv[2] = {key, value};
    multimapInsert(&c->data, kv);
    multilruInsert(c->lru);
}

bool cacheGet(SimpleCache *c, const databox *key, databox *outValue) {
    databox *results[1] = {outValue};
    if (multimapLookup(c->data, key, results)) {
        /* Update LRU (in real impl, track ptr) */
        return true;
    }
    return false;
}

void cacheFree(SimpleCache *c) {
    multimapFree(c->data);
    multilruFree(c->lru);
    free(c);
}
```

### Pattern: TTL Cache

Cache with time-based expiration:

```c
typedef struct {
    multimap *cache;  /* key → {value, timestamp} */
    time_t ttlSeconds;
} TTLCache;

TTLCache *ttlCacheNew(time_t ttl) {
    TTLCache *c = malloc(sizeof(*c));
    c->cache = multimapNew(3);  /* key + value + timestamp */
    c->ttlSeconds = ttl;
    return c;
}

void ttlCachePut(TTLCache *c, const databox *key, const databox *value) {
    databox timestamp = databoxNewSigned(time(NULL));
    const databox *elements[3] = {key, value, &timestamp};
    multimapInsert(&c->cache, elements);
}

bool ttlCacheGet(TTLCache *c, const databox *key, databox *outValue) {
    databox value, timestamp;
    databox *results[2] = {&value, &timestamp};

    if (multimapLookup(c->cache, key, results)) {
        time_t age = time(NULL) - timestamp.data.i64;
        if (age <= c->ttlSeconds) {
            *outValue = value;
            return true;
        } else {
            /* Expired - delete */
            multimapDelete(&c->cache, key);
        }
    }
    return false;
}

void ttlCacheExpireOld(TTLCache *c) {
    time_t now = time(NULL);
    time_t cutoff = now - c->ttlSeconds;

    /* Collect expired keys */
    databox expired[1000];
    size_t count = 0;

    multimapIterator iter;
    multimapIteratorInit(c->cache, &iter, true);
    databox key, value, timestamp;
    databox *elements[3] = {&key, &value, &timestamp};

    while (multimapIteratorNext(&iter, elements)) {
        if (timestamp.data.i64 < cutoff) {
            expired[count++] = key;
            if (count >= 1000) break;
        }
    }

    /* Delete expired */
    for (size_t i = 0; i < count; i++) {
        multimapDelete(&c->cache, &expired[i]);
    }
}
```

---

## Time-Series Patterns

### Pattern: Sorted Time-Series Storage

Store time-series data with efficient range queries:

```c
typedef struct {
    multimap *series;  /* timestamp → value */
} TimeSeries;

TimeSeries *tsNew(void) {
    TimeSeries *ts = malloc(sizeof(*ts));
    ts->series = multimapNew(2);
    return ts;
}

void tsAppend(TimeSeries *ts, time_t timestamp, double value) {
    databox ts_box = databoxNewSigned(timestamp);
    databox val_box = databoxNewReal(value);
    const databox *entry[2] = {&ts_box, &val_box};

    /* Use Append for chronological data (faster!) */
    multimapAppend(&ts->series, entry);
}

void tsQuery(TimeSeries *ts, time_t start, time_t end,
             void (*callback)(time_t, double)) {
    databox startBox = databoxNewSigned(start);

    multimapIterator iter;
    if (!multimapIteratorInitAt(ts->series, &iter, true, &startBox)) {
        return;  /* No data */
    }

    databox timestamp, value;
    databox *elements[2] = {&timestamp, &value};

    while (multimapIteratorNext(&iter, elements)) {
        time_t ts = timestamp.data.i64;
        if (ts > end) break;

        callback(ts, value.data.d64);
    }
}

double tsAggregate(TimeSeries *ts, time_t start, time_t end,
                   const char *op) {
    double result = 0.0;
    size_t count = 0;
    double min = DBL_MAX;
    double max = -DBL_MAX;

    databox startBox = databoxNewSigned(start);
    multimapIterator iter;
    if (!multimapIteratorInitAt(ts->series, &iter, true, &startBox)) {
        return 0.0;
    }

    databox timestamp, value;
    databox *elements[2] = {&timestamp, &value};

    while (multimapIteratorNext(&iter, elements)) {
        time_t ts = timestamp.data.i64;
        if (ts > end) break;

        double val = value.data.d64;
        result += val;
        count++;
        if (val < min) min = val;
        if (val > max) max = val;
    }

    if (strcmp(op, "sum") == 0) return result;
    if (strcmp(op, "avg") == 0) return count > 0 ? result / count : 0;
    if (strcmp(op, "min") == 0) return min;
    if (strcmp(op, "max") == 0) return max;
    if (strcmp(op, "count") == 0) return count;

    return 0.0;
}
```

### Pattern: Circular Buffer for Recent Data

Keep only the most recent N data points:

```c
typedef struct {
    multilist *buffer;
    size_t maxSize;
} CircularBuffer;

CircularBuffer *cbNew(size_t maxSize) {
    CircularBuffer *cb = malloc(sizeof(*cb));
    cb->buffer = multilistNew(FLEX_CAP_LEVEL_2048, 0);
    cb->maxSize = maxSize;
    return cb;
}

void cbAppend(CircularBuffer *cb, const databox *item) {
    mflexState *state = mflexStateCreate();

    /* Add to tail */
    multilistPushByTypeTail(&cb->buffer, state, item);

    /* Evict oldest if over capacity */
    if (multilistCount(cb->buffer) > cb->maxSize) {
        databox evicted;
        multilistPopHead(&cb->buffer, state, &evicted);
        databoxFreeData(&evicted);
    }

    mflexStateFree(state);
}

void cbIterate(CircularBuffer *cb,
               void (*callback)(const databox *)) {
    mflexState *state = mflexStateCreate();

    multimapIterator iter;
    multilistIteratorInitForwardReadOnly(cb->buffer, state, &iter);

    multilistEntry entry;
    while (multilistNext(&iter, &entry)) {
        callback(&entry.box);
    }

    multilistIteratorRelease(&iter);
    mflexStateFree(state);
}
```

---

## String Building Patterns

### Pattern: Dynamic String Builder

Build strings efficiently:

```c
/* Using dks string buffer */
dksstr *s = dksstr_new();

dksstr_cat(s, "Hello");
dksstr_cat(s, " ");
dksstr_cat(s, "World");
dksstr_cat_int(s, 42);

printf("Result: %s\n", s->buf);  /* "Hello World42" */

dksstr_free(s);

/* Using flex for mixed data */
flex *f = NULL;

databox str1 = databoxNewBytesString("Hello");
databox str2 = databoxNewBytesString("World");
databox num = databoxNewSigned(42);

flexPushByType(&f, &str1, FLEX_ENDPOINT_TAIL);
flexPushByType(&f, &str2, FLEX_ENDPOINT_TAIL);
flexPushByType(&f, &num, FLEX_ENDPOINT_TAIL);

/* Iterate and build result */
char result[256];
size_t pos = 0;

flexEntry *fe = flexHead(f);
while (fe && flexEntryIsValid(f, fe)) {
    databox box;
    flexGetByType(fe, &box);

    if (DATABOX_IS_BYTES(&box)) {
        pos += snprintf(result + pos, sizeof(result) - pos,
                       "%.*s ", (int)databoxLen(&box),
                       databoxCBytes(&box));
    } else if (DATABOX_IS_SIGNED_INTEGER(&box)) {
        pos += snprintf(result + pos, sizeof(result) - pos,
                       "%ld ", box.data.i64);
    }

    fe = flexNext(f, fe);
}

printf("Result: %s\n", result);

flexFree(f);
```

---

## Set Operations Patterns

### Pattern: Union of Integer Sets

Combine multiple integer sets:

```c
intset *setUnion(intset *a, intset *b) {
    intset *result = intsetNew();

    /* Add all from a */
    for (uint32_t i = 0; i < intsetCount(a); i++) {
        int64_t val;
        intsetGet(a, i, &val);
        intsetAdd(&result, val, NULL);
    }

    /* Add all from b (duplicates ignored) */
    for (uint32_t i = 0; i < intsetCount(b); i++) {
        int64_t val;
        intsetGet(b, i, &val);
        intsetAdd(&result, val, NULL);
    }

    return result;
}

intset *setIntersect(intset *a, intset *b) {
    intset *result = intsetNew();

    /* For each in a, check if in b */
    for (uint32_t i = 0; i < intsetCount(a); i++) {
        int64_t val;
        intsetGet(a, i, &val);
        if (intsetFind(b, val)) {
            intsetAdd(&result, val, NULL);
        }
    }

    return result;
}

intset *setDifference(intset *a, intset *b) {
    intset *result = intsetNew();

    /* For each in a, add if NOT in b */
    for (uint32_t i = 0; i < intsetCount(a); i++) {
        int64_t val;
        intsetGet(a, i, &val);
        if (!intsetFind(b, val)) {
            intsetAdd(&result, val, NULL);
        }
    }

    return result;
}
```

### Pattern: Bitmap Set Operations

Use multiroar for efficient bitmap operations:

```c
/* Create bitmaps */
multiroar *active = multiroarBitNew();
multiroar *premium = multiroarBitNew();

/* Set user IDs */
multiroarBitSet(active, 1001);
multiroarBitSet(active, 1002);
multiroarBitSet(active, 1003);

multiroarBitSet(premium, 1002);
multiroarBitSet(premium, 1003);
multiroarBitSet(premium, 1004);

/* Union: active OR premium users */
multiroar *all = multiroarNewOr(active, premium);

/* Intersection: active AND premium users */
multiroar *activePremium = multiroarNewAnd(active, premium);

/* Difference: active but not premium */
multiroar *notPremium = multiroarNewNot(premium);
multiroar *activeFree = multiroarNewAnd(active, notPremium);

/* XOR: exclusive (active or premium, but not both) */
multiroar *exclusive = multiroarNewXor(active, premium);

/* Cleanup */
multiroarFree(active);
multiroarFree(premium);
multiroarFree(all);
multiroarFree(activePremium);
multiroarFree(notPremium);
multiroarFree(activeFree);
multiroarFree(exclusive);
```

---

## Compression Patterns

### Pattern: Automatic Compression with mflex

Use mflex for transparent compression:

```c
mflexState *state = mflexStateCreate();
mflex *m = mflexNew();

/* Add data - auto-compresses when beneficial */
for (int i = 0; i < 100000; i++) {
    mflexPushDouble(&m, state, (double)i, FLEX_ENDPOINT_TAIL);
}

/* Check compression status */
if (mflexIsCompressed(m)) {
    size_t uncompressed = mflexBytesUncompressed(m);
    size_t compressed = mflexBytesCompressed(m);
    double ratio = (double)uncompressed / compressed;
    printf("Compressed: %.2fx ratio\n", ratio);
}

/* Access data (transparently decompressed) */
flex *f = mflexOpen(m, state);
/* Work with uncompressed data */
mflexCloseGrow(&m, state, f);

mflexStateFree(state);
mflexFree(m);
```

### Pattern: Delta-of-Delta for Timestamps

Compress monotonically increasing integers:

```c
dod *timestamps = dodNew();

/* Add increasing timestamps */
time_t baseTime = 1700000000;
for (int i = 0; i < 10000; i++) {
    dodAdd(timestamps, baseTime + (i * 60));  /* Every minute */
}

/* Retrieve values */
int64_t retrieved;
if (dodGet(timestamps, 5000, &retrieved)) {
    printf("Timestamp at 5000: %ld\n", retrieved);
}

dodFree(timestamps);
```

### Pattern: Float Compression with xof

Compress floating-point time series:

```c
xof *temperatures = xofNew();

/* Add temperature readings */
for (int i = 0; i < 10000; i++) {
    double temp = 20.0 + (rand() % 100) / 10.0;  /* 20.0 - 30.0 */
    xofAdd(temperatures, temp);
}

/* Similar values compress very well */
printf("Compressed size: %zu bytes\n", xofBytes(temperatures));

/* Retrieve */
double temp;
if (xofGet(temperatures, 1000, &temp)) {
    printf("Temperature at 1000: %.2f\n", temp);
}

xofFree(temperatures);
```

---

## Summary

These patterns cover the most common use cases across datakit modules:

1. **Always initialize pointers to NULL** before container creation
2. **Choose the right size variant** for your data volume
3. **Use iterators** for sequential access, not index-based loops
4. **Free resources** in reverse order of allocation
5. **Check return values** for all operations
6. **Pre-allocate** when you know the size to avoid reallocations
7. **Use appropriate compression** for repetitive or sequential data
8. **Leverage specialized containers** (intset for integers, multiroar for sparse bitmaps)

For more detailed examples, see:
- [USE_CASES.md](USE_CASES.md) - Real-world application examples
- [MIGRATION.md](MIGRATION.md) - Migrating from other libraries
- [BENCHMARKS.md](BENCHMARKS.md) - Performance comparisons
