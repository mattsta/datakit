# multimap - Scale-Aware Sorted Key-Value Store

## Overview

`multimap` is a **self-optimizing sorted key-value container** that automatically transitions between three internal representations based on data size. It provides O(log n) lookups with minimal memory overhead by using the most efficient storage format for your current dataset size.

**Key Features:**
- Automatic scaling from 16 bytes (small) → 28 bytes (medium) → variable size (full)
- Multi-column support: store N values per key (up to 65,535 columns!)
- Sorted storage with binary search for O(log n) operations
- Set mode (unique keys) or Map mode (duplicate keys allowed)
- Reference container support for string deduplication
- Forward/reverse iteration with flexible starting points
- Predicate-based filtering and deletion
- Optional LZ4 compression

**Headers**: `multimap.h`, `multimapCommon.h`

**Source**: `multimap.c`, `multimapSmall.c`, `multimapMedium.c`, `multimapFull.c`

## Architecture

The multimap family uses **pointer tagging** to transparently manage three underlying implementations:

```
┌─────────────┐
│  multimap*  │  Opaque pointer with 2-bit type tag
└──────┬──────┘
       │
       ├──[tag=1]──> multimapSmall  (16 bytes, 1 flex)
       ├──[tag=2]──> multimapMedium (28 bytes, 2 flex arrays)
       └──[tag=3]──> multimapFull   (40+ bytes, N flex arrays)
```

When you call `multimapInsert()`, the implementation:
1. Checks the current variant via pointer tag
2. Routes to the appropriate function (Small/Medium/Full)
3. Automatically upgrades to the next variant if size threshold exceeded
4. Transparently updates your pointer to the new variant

**You never need to know which variant you're using** - the API is identical.

## Data Structures

### Common Types

```c
/* Element count type - up to 65,535 "columns" per row */
typedef uint32_t multimapElements;

/* Iterator for traversing multimap contents */
typedef struct multimapIterator {
    void *mm;                    /* Back pointer to multimap */
    flexEntry *entry;            /* Current position */
    flex *map;                   /* Current map being iterated */
    uint32_t mapIndex;           /* For Medium/Full: which map index */
    uint32_t elementsPerEntry : 16;
    uint32_t type : 2;           /* Variant type: 1=Small, 2=Medium, 3=Full */
    uint32_t forward : 1;        /* Direction of iteration */
} multimapIterator;

/* Entry reference for direct manipulation */
typedef struct multimapEntry {
    flex **map;                  /* Pointer to the flex map */
    flexEntry *fe;               /* Specific entry within map */
    multimapFullIdx mapIdx;      /* For Full: which map contains entry */
} multimapEntry;

/* Predicate for conditional operations */
typedef enum multimapCondition {
    MULTIMAP_CONDITION_NONE = 0,
    MULTIMAP_CONDITION_ALL,
    MULTIMAP_CONDITION_LESS_THAN,
    MULTIMAP_CONDITION_LESS_THAN_EQUAL,
    MULTIMAP_CONDITION_EQUAL,
    MULTIMAP_CONDITION_GREATER_THAN,
    MULTIMAP_CONDITION_GREATER_THAN_EQUAL
} multimapCondition;

typedef struct multimapPredicate {
    multimapCondition condition;
    databox compareAgainst;
} multimapPredicate;
```

## Creation Functions

### Basic Creation

```c
/* Create new multimap with default 2KB max per map */
multimap *multimapNew(multimapElements elementsPerEntry);

/* Create with explicit size limit */
multimap *multimapNewLimit(multimapElements elementsPerEntry,
                           flexCapSizeLimit limit);

/* Create with LZ4 compression enabled */
multimap *multimapNewCompress(multimapElements elementsPerEntry,
                              flexCapSizeLimit limit);

/* Create in "set" mode (unique keys only) */
multimap *multimapSetNew(multimapElements elementsPerEntry);

/* Create with full configuration */
multimap *multimapNewConfigure(multimapElements elementsPerEntry,
                               bool isSet, bool compress,
                               flexCapSizeLimit sizeLimit);

/* Example usage */
multimap *m = multimapNew(2);  /* Key + Value = 2 elements */

/* Multi-column example: User ID, Name, Age, Score */
multimap *users = multimapNew(4);

/* Set mode: no duplicate keys allowed */
multimap *uniqueIds = multimapSetNew(1);

/* Compressed map for large text data */
multimap *textIndex = multimapNewCompress(2, FLEX_CAP_LEVEL_4096);
```

### Size Limits

Available `flexCapSizeLimit` values control when maps split:

```c
FLEX_CAP_LEVEL_128     /* Split at 128 bytes */
FLEX_CAP_LEVEL_256     /* Split at 256 bytes */
FLEX_CAP_LEVEL_512     /* Split at 512 bytes (recommended for small data) */
FLEX_CAP_LEVEL_1024    /* Split at 1KB */
FLEX_CAP_LEVEL_2048    /* Split at 2KB (default) */
FLEX_CAP_LEVEL_4096    /* Split at 4KB (recommended for large data) */
FLEX_CAP_LEVEL_8192    /* Split at 8KB */
```

### Copying and Metadata

```c
/* Deep copy entire multimap */
multimap *multimapCopy(const multimap *m);

/* Get element count (total rows) */
size_t multimapCount(const multimap *m);

/* Get total bytes used by map data */
size_t multimapBytes(const multimap *m);

/* Dump all contents to a single flex */
flex *multimapDump(const multimap *m);

/* Example usage */
multimap *original = multimapNew(2);
/* ... populate ... */
multimap *backup = multimapCopy(original);

printf("Contains %zu entries\n", multimapCount(original));
printf("Using %zu bytes\n", multimapBytes(original));
```

## Insertion Operations

### Basic Insertion

```c
/* Insert or replace entry by key (first element) */
bool multimapInsert(multimap **m, const databox *elements[]);

/* Insert using all elements as compound key */
void multimapInsertFullWidth(multimap **m, const databox *elements[]);

/* Append to end (assumes key > all existing keys) */
void multimapAppend(multimap **m, const databox *elements[]);

/* Example: Key-Value insertion */
multimap *m = multimapNew(2);

databox key = databoxNewBytesString("username");
databox val = databoxNewBytesString("alice");
const databox *elements[2] = {&key, &val};

bool replaced = multimapInsert(&m, elements);
/* replaced is true if key existed and value was updated */

/* Example: Multi-column insertion */
multimap *users = multimapNew(4);

databox uid = databoxNewSigned(1001);
databox name = databoxNewBytesString("Bob");
databox age = databoxNewSigned(25);
databox score = databoxNewReal(98.5);
const databox *row[4] = {&uid, &name, &age, &score};

multimapInsert(&users, row);
```

**Important**: Note that insertion takes `multimap **m` (pointer to pointer). This allows automatic upgrading between variants:

```c
multimap *m = multimapNew(2);  /* Starts as Small */

/* After many inserts, 'm' may become Medium or Full */
for (int i = 0; i < 10000; i++) {
    databox k = databoxNewSigned(i);
    databox v = databoxNewSigned(i * 100);
    const databox *e[2] = {&k, &v};
    multimapInsert(&m, e);  /* Pass &m to allow upgrade */
}

/* 'm' now points to a different variant (Medium or Full) */
```

### Full-Width Operations

Full-width operations use **all elements** as the key, allowing duplicate first elements:

```c
/* Time-series example: timestamp + measurement */
multimap *timeseries = multimapNew(2);

databox ts1 = databoxNewSigned(1000);
databox temp1 = databoxNewReal(20.5);
const databox *row1[2] = {&ts1, &temp1};
multimapInsertFullWidth(&timeseries, row1);

/* Same timestamp, different measurement - both stored! */
databox ts2 = databoxNewSigned(1000);  /* Same! */
databox temp2 = databoxNewReal(21.0);   /* Different! */
const databox *row2[2] = {&ts2, &temp2};
multimapInsertFullWidth(&timeseries, row2);

/* Now contains 2 entries, both with timestamp=1000 */
assert(multimapCount(timeseries) == 2);
```

### Surrogate Key Insertion (Advanced)

For use with `multimapAtom` reference containers:

```c
void multimapInsertWithSurrogateKey(
    multimap **m, const databox *elements[],
    const databox *insertKey,
    const struct multimapAtom *referenceContainer);

/* Inserts with 'insertKey' instead of elements[0] for ordering */
/* See multimapAtom.h for reference container usage */
```

## Lookup Operations

### Basic Lookup

```c
/* Check if key exists */
bool multimapExists(const multimap *m, const databox *key);

/* Check if full-width entry exists */
bool multimapExistsFullWidth(const multimap *m,
                             const databox *elements[]);

/* Lookup values by key */
bool multimapLookup(const multimap *m, const databox *key,
                    databox *elements[]);

/* Example: Simple lookup */
multimap *m = /* ... populated ... */;

databox searchKey = databoxNewBytesString("username");

if (multimapExists(m, &searchKey)) {
    databox value;
    databox *results[1] = {&value};

    if (multimapLookup(m, &searchKey, results)) {
        printf("Found: ");
        databoxRepr(&value);
    }
}

/* Example: Multi-column lookup */
multimap *users = /* ... populated with 4 columns ... */;

databox uid = databoxNewSigned(1001);
databox name, age, score;
databox *results[3] = {&name, &age, &score};

if (multimapLookup(users, &uid, results)) {
    printf("Name: ");
    databoxRepr(&name);
    printf("Age: ");
    databoxRepr(&age);
    printf("Score: ");
    databoxRepr(&score);
}
```

**Important**: `multimapLookup()` returns N-1 values (skips the key you searched for).

### First and Last

```c
/* Get first entry (lowest key) */
bool multimapFirst(multimap *m, databox *elements[]);

/* Get last entry (highest key) */
bool multimapLast(multimap *m, databox *elements[]);

/* Example */
databox firstKey, firstName;
databox *first[2] = {&firstKey, &firstName};

if (multimapFirst(m, first)) {
    printf("Lowest key: ");
    databoxRepr(&firstKey);
}
```

### Random Access

```c
/* Get random entry */
bool multimapRandomValue(multimap *m, bool fromTail,
                         databox **found, multimapEntry *me);

/* Example: Random sampling */
databox *foundElements[2];
multimapEntry me;

if (multimapRandomValue(m, false, foundElements, &me)) {
    printf("Random entry found!\n");
}
```

### Underlying Entry Access (Advanced)

```c
/* Get direct reference to entry for in-place modification */
bool multimapGetUnderlyingEntry(multimap *m, const databox *key,
                                multimapEntry *me);

/* Resize entry in place */
void multimapResizeEntry(multimap **m, multimapEntry *me,
                         size_t newLen);

/* Replace entry value in place */
void multimapReplaceEntry(multimap **m, multimapEntry *me,
                          const databox *box);

/* Example: In-place modification */
multimapEntry me;
databox searchKey = databoxNewBytesString("counter");

if (multimapGetUnderlyingEntry(m, &searchKey, &me)) {
    /* Directly modify the value */
    databox newValue = databoxNewSigned(999);
    multimapReplaceEntry(&m, &me, &newValue);
}
```

## Deletion Operations

### Basic Deletion

```c
/* Delete by key (first element) */
bool multimapDelete(multimap **m, const databox *key);

/* Delete by full-width match */
bool multimapDeleteFullWidth(multimap **m,
                             const databox *elements[]);

/* Delete and retrieve deleted value */
bool multimapDeleteWithFound(multimap **m, const databox *key,
                             databox *foundReference);

/* Delete random entry */
bool multimapDeleteRandomValue(multimap **m, bool fromTail,
                               databox **deleted);

/* Example: Simple delete */
databox key = databoxNewBytesString("username");
bool deleted = multimapDelete(&m, &key);

if (deleted) {
    printf("Entry deleted\n");
}

/* Example: Delete with retrieval */
databox deletedValue;
if (multimapDeleteWithFound(&m, &key, &deletedValue)) {
    printf("Deleted value was: ");
    databoxRepr(&deletedValue);
}
```

### Predicate-Based Deletion

```c
/* Delete all entries matching predicate */
bool multimapDeleteByPredicate(multimap **m,
                               const multimapPredicate *p);

/* Example: Delete all entries where key <= 100 */
databox limit = databoxNewSigned(100);
multimapPredicate pred = {
    .condition = MULTIMAP_CONDITION_LESS_THAN_EQUAL,
    .compareAgainst = limit
};

bool deleted = multimapDeleteByPredicate(&m, &pred);
if (deleted) {
    printf("Deleted all entries with key <= 100\n");
}
```

## Iteration

### Basic Iteration

```c
/* Initialize iterator */
void multimapIteratorInit(const multimap *m, multimapIterator *iter,
                          bool forward);

/* Initialize at specific key */
bool multimapIteratorInitAt(const multimap *m, multimapIterator *iter,
                            bool forward, const databox *startAt);

/* Advance iterator and get next entry */
bool multimapIteratorNext(multimapIterator *iter,
                          databox **elements);

/* Example: Forward iteration */
multimapIterator iter;
multimapIteratorInit(m, &iter, true);  /* true = forward */

databox key, value;
databox *elements[2] = {&key, &value};

while (multimapIteratorNext(&iter, elements)) {
    printf("Key: ");
    databoxRepr(&key);
    printf("Value: ");
    databoxRepr(&value);
    printf("\n");
}

/* Example: Reverse iteration */
multimapIteratorInit(m, &iter, false);  /* false = reverse */

while (multimapIteratorNext(&iter, elements)) {
    /* Process in reverse order */
}

/* Example: Start at specific key */
databox startKey = databoxNewSigned(1000);
multimapIteratorInitAt(m, &iter, true, &startKey);

while (multimapIteratorNext(&iter, elements)) {
    /* Iterate from key=1000 onwards */
}
```

### Iteration Pattern

```c
typedef bool(multimapElementWalker)(void *userData,
                                    const databox *elements[]);

size_t multimapProcessUntil(multimap *m,
                           const multimapPredicate *p,
                           bool forward,
                           multimapElementWalker *walker,
                           void *userData);

/* Example: Custom processing */
bool printElement(void *userData, const databox *elements[]) {
    int *count = userData;
    printf("Entry %d: ", (*count)++);
    databoxRepr(elements[0]);
    return true;  /* Continue iteration */
}

int count = 0;
multimapPredicate pred = {.condition = MULTIMAP_CONDITION_ALL};
multimapProcessUntil(m, &pred, true, printElement, &count);
```

## Field Operations

### Field Increment

```c
/* Increment a numeric field at offset */
int64_t multimapFieldIncr(multimap **m, const databox *key,
                          uint32_t fieldOffset, int64_t incrBy);

/* Example: Counter implementation */
multimap *counters = multimapNew(2);  /* key + count */

databox key = databoxNewBytesString("pageviews");
databox count = databoxNewSigned(0);
const databox *init[2] = {&key, &count};
multimapInsert(&counters, init);

/* Increment counter by 1 */
int64_t newCount = multimapFieldIncr(&counters, &key, 1, 1);
printf("New count: %ld\n", newCount);

/* Increment by 10 */
newCount = multimapFieldIncr(&counters, &key, 1, 10);
printf("New count: %ld\n", newCount);

/* Example: Multi-column field increment */
/* Row: [UserID, LoginCount, PointsEarned] */
multimap *stats = multimapNew(3);

databox uid = databoxNewSigned(1001);
databox logins = databoxNewSigned(0);
databox points = databoxNewSigned(0);
const databox *row[3] = {&uid, &logins, &points};
multimapInsert(&stats, row);

/* Increment login count (offset 1) */
multimapFieldIncr(&stats, &uid, 1, 1);

/* Increment points (offset 2) */
multimapFieldIncr(&stats, &uid, 2, 100);
```

## Set Operations

### Intersection

```c
/* Compute key intersection of two multimaps */
void multimapIntersectKeys(multimap **restrict dst,
                          multimapIterator *restrict const a,
                          multimapIterator *restrict const b);

/* Example */
multimap *map1 = /* ... */;
multimap *map2 = /* ... */;
multimap *result = multimapNew(1);

multimapIterator iter1, iter2;
multimapIteratorInit(map1, &iter1, true);
multimapIteratorInit(map2, &iter2, true);

multimapIntersectKeys(&result, &iter1, &iter2);
/* result now contains keys present in both map1 and map2 */
```

### Difference

```c
/* Compute key difference */
void multimapDifferenceKeys(multimap **restrict dst,
                           multimapIterator *restrict const a,
                           multimapIterator *restrict const b,
                           bool symmetricDifference);

/* Example: A - B (keys in A but not in B) */
multimap *difference = multimapNew(1);
multimapIteratorInit(map1, &iter1, true);
multimapIteratorInit(map2, &iter2, true);

multimapDifferenceKeys(&difference, &iter1, &iter2, false);
```

### Copy Keys

```c
/* Copy just the keys from source to destination */
void multimapCopyKeys(multimap **restrict dst,
                     const multimap *restrict src);

/* Example */
multimap *keysOnly = multimapNew(1);
multimapCopyKeys(&keysOnly, sourceMap);
```

## Memory Management

### Reset and Free

```c
/* Clear all entries but keep structure */
void multimapReset(multimap *m);

/* Free entire multimap */
void multimapFree(multimap *m);

/* Example */
multimap *m = multimapNew(2);
/* ... use it ... */

multimapReset(m);  /* Clear all data, keep allocated */
/* ... reuse it ... */

multimapFree(m);   /* Destroy completely */
```

### Reference Containers (Advanced)

The multimap family supports **reference containers** (`multimapAtom`) for automatic string deduplication and reference counting. See `MULTIMAP_ATOM.md` for details.

## Performance Characteristics

### Time Complexity

| Operation | Small | Medium | Full |
|-----------|-------|--------|------|
| Insert | O(n) | O(n) | O(n) in largest map |
| Lookup | O(log n) | O(log n) | O(log n) with map binary search |
| Delete | O(n) | O(n) | O(n) in specific map |
| Iteration | O(n) | O(n) | O(n) |
| First/Last | O(1) | O(1) | O(1) |

### Space Complexity

| Variant | Fixed Overhead | Per Entry | Max Recommended Size |
|---------|---------------|-----------|---------------------|
| Small | 16 bytes | ~varies~ | < 2KB total |
| Medium | 28 bytes | ~varies~ | 2KB - 6KB total |
| Full | 40+ bytes + N*(8+4+8) | ~varies~ | Unlimited (TBs) |

### Automatic Upgrade Thresholds

```c
Small → Medium:  when bytes > limit AND count > elementsPerEntry * 2
Medium → Full:   when bytes > limit * 3 AND count > elementsPerEntry * 2
```

## Best Practices

### 1. Choose Appropriate Size Limits

```c
/* Small, frequent updates - use smaller splits */
multimap *hotCache = multimapNewLimit(2, FLEX_CAP_LEVEL_512);

/* Large, bulk operations - use larger splits */
multimap *bulkData = multimapNewLimit(2, FLEX_CAP_LEVEL_4096);
```

### 2. Use elementsPerEntry Wisely

```c
/* Good: Natural column grouping */
multimap *users = multimapNew(4);  /* ID, Name, Age, Email */

/* Bad: Too many columns makes iteration slow */
multimap *bad = multimapNew(100);  /* Hard to manage */

/* Better: Split into multiple maps or use nested structures */
```

### 3. Leverage Set Mode for Uniqueness

```c
/* Automatically prevents duplicate keys */
multimap *uniqueUsers = multimapSetNew(2);

databox key = databoxNewBytesString("alice");
databox val1 = databoxNewSigned(1);
const databox *e1[2] = {&key, &val1};
multimapInsert(&uniqueUsers, e1);  /* Inserts */

databox val2 = databoxNewSigned(2);
const databox *e2[2] = {&key, &val2};
bool replaced = multimapInsert(&uniqueUsers, e2);  /* Replaces */
assert(replaced == true);
```

### 4. Use Append When Possible

```c
/* If inserting in sorted order, append is much faster */
for (int i = 0; i < 10000; i++) {
    databox k = databoxNewSigned(i);  /* Monotonically increasing */
    databox v = databoxNewSigned(i * 2);
    const databox *e[2] = {&k, &v};
    multimapAppend(&m, e);  /* O(1) append vs O(n) insert */
}
```

## Common Patterns

### Pattern 1: Dictionary/Hash Table Replacement

```c
multimap *dict = multimapNew(2);

/* Insert */
databox key = databoxNewBytesString("color");
databox val = databoxNewBytesString("blue");
const databox *kv[2] = {&key, &val};
multimapInsert(&dict, kv);

/* Lookup */
databox result;
databox *results[1] = {&result};
if (multimapLookup(dict, &key, results)) {
    /* Found! */
}

/* Delete */
multimapDelete(&dict, &key);
```

### Pattern 2: Time Series Storage

```c
multimap *timeseries = multimapNew(2);  /* timestamp + value */

/* Insert readings */
for (int i = 0; i < 1000; i++) {
    databox ts = databoxNewSigned(time(NULL) + i);
    databox temp = databoxNewReal(20.0 + (rand() % 10));
    const databox *reading[2] = {&ts, &temp};
    multimapAppend(&timeseries, reading);
}

/* Query range: all readings from timestamp X onwards */
databox startTime = databoxNewSigned(targetTimestamp);
multimapIterator iter;
multimapIteratorInitAt(timeseries, &iter, true, &startTime);

databox ts, temp;
databox *row[2] = {&ts, &temp};
while (multimapIteratorNext(&iter, row)) {
    printf("%ld: %.2f\n", ts.data.i64, temp.data.d64);
}
```

### Pattern 3: Leaderboard/Ranking

```c
/* Store: Score (key) + PlayerName (value) */
/* Sorted automatically by score! */
multimap *leaderboard = multimapNew(2);

databox score1 = databoxNewSigned(9500);
databox player1 = databoxNewBytesString("Alice");
const databox *entry1[2] = {&score1, &player1};
multimapInsert(&leaderboard, entry1);

databox score2 = databoxNewSigned(8200);
databox player2 = databoxNewBytesString("Bob");
const databox *entry2[2] = {&score2, &player2};
multimapInsert(&leaderboard, entry2);

/* Get top scorer */
databox topScore, topPlayer;
databox *top[2] = {&topScore, &topPlayer};
if (multimapLast(leaderboard, top)) {
    printf("Top player: %s with %ld points\n",
           databoxCBytes(&topPlayer), topScore.data.i64);
}

/* Iterate from highest to lowest */
multimapIterator iter;
multimapIteratorInit(leaderboard, &iter, false);  /* Reverse */
while (multimapIteratorNext(&iter, top)) {
    printf("%ld: %s\n", topScore.data.i64,
           databoxCBytes(&topPlayer));
}
```

### Pattern 4: Multi-Column Database Table

```c
/* Table: UserID | Name | Age | Status */
multimap *users = multimapNew(4);

/* Insert row */
databox uid = databoxNewSigned(1001);
databox name = databoxNewBytesString("Charlie");
databox age = databoxNewSigned(30);
databox status = databoxBool(true);
const databox *row[4] = {&uid, &name, &age, &status};
multimapInsert(&users, row);

/* Query row */
databox foundName, foundAge, foundStatus;
databox *cols[3] = {&foundName, &foundAge, &foundStatus};
if (multimapLookup(users, &uid, cols)) {
    printf("User %ld: %s, age %ld, active: %s\n",
           uid.data.i64,
           databoxCBytes(&foundName),
           foundAge.data.i64,
           DATABOX_IS_TRUE(&foundStatus) ? "yes" : "no");
}
```

## Thread Safety

multimap is **not thread-safe** by default. For concurrent access, use external synchronization:

```c
pthread_rwlock_t lock = PTHREAD_RWLOCK_INITIALIZER;
multimap *shared = multimapNew(2);

/* Reader */
pthread_rwlock_rdlock(&lock);
bool exists = multimapExists(shared, &key);
pthread_rwlock_unlock(&lock);

/* Writer */
pthread_rwlock_wrlock(&lock);
multimapInsert(&shared, elements);
pthread_rwlock_unlock(&lock);
```

## See Also

- [VARIANTS.md](VARIANTS.md) - Detailed comparison of Small/Medium/Full variants
- [EXAMPLES.md](EXAMPLES.md) - Real-world usage examples
- [databox](../core/DATABOX.md) - Universal container used for all values
- [flex](../flex/FLEX.md) - Underlying storage for multimap variants
- [multimapAtom](MULTIMAP_ATOM.md) - Reference counting and string interning

## Testing

Run the multimap test suite:

```bash
./src/datakit-test test multimap
```

The test suite includes:
- Small → Medium → Full automatic transitions
- Insertion, lookup, deletion across all variants
- Full-width operations
- Iteration forward and reverse
- Field increment operations
- Predicate-based operations
- Performance benchmarks at different scale limits
