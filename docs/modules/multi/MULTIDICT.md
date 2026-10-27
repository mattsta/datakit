# multidict - Generic Hash Table Dictionary

## Overview

`multidict` is a **generic hash table implementation** that provides a flexible foundation for building custom dictionary types with configurable storage strategies. Unlike traditional hash tables, multidict uses a **slot-based architecture** where each hash slot can contain arbitrary data structures through a pluggable class system.

**Key Features:**

- Pluggable storage classes via `multidictClass` interface
- Incremental rehashing for smooth performance
- Dual hash table design (for rehashing)
- Support for both exact and case-insensitive keys
- Customizable hash functions (XXHash by default)
- Safe and unsafe iteration modes
- SCAN operation for stateless traversal
- Conditional inserts (`AddNX` / `AddXX` / `Replace`) and atomic
  get-and-delete
- Numeric increment helpers for integer and float values
- Whole-dictionary copy and merge
- Structured statistics, including per-chain distribution and byte-based
  load metrics
- Optional self-management: memory ceiling, eviction callback, and
  LRU/LFU eviction policies
- Optional byte-based expansion driven by average slot size instead of
  element count

**Headers**: `multidict.h`

**Source**: `multidict.c`

## Architecture

multidict uses a **two-table incremental rehashing** design:

```
┌─────────────┐
│  multidict  │
└──────┬──────┘
       │
       ├──> ht[0] ──> [slot][slot][slot]...
       │              Each slot is a multidictSlot*
       │              managed by multidictClass
       │
       └──> ht[1] ──> [slot][slot][slot]... (active during rehash)
```

When the hash table needs to grow:

1. `ht[1]` is allocated with new size
2. Elements are incrementally moved from `ht[0]` to `ht[1]`
3. Once complete, `ht[1]` becomes `ht[0]`
4. `ht[1]` is reset

This incremental approach avoids blocking operations on large dictionaries.

Load factors are **percentages of slot count**, and the defaults are tuned
for slots that hold many entries rather than one:

| Setting              | Default | Meaning                                       |
| -------------------- | ------- | --------------------------------------------- |
| Expand load factor   | 200     | Expand when `count * 100 / slots` exceeds 200 |
| Shrink load factor   | 10      | Shrink when the same ratio drops below 10     |
| Minimum shrink slots | 8       | Never shrink below 8 slots                    |
| Initial slot count   | 1       | `MULTIDICT_HT_INITIAL_SIZE`                   |

## Data Structures

### Core Types

```c
/* Forward declaration */
typedef struct multidict multidict;

/* Insert / update outcome */
typedef enum {
    MULTIDICT_ERR = -1,        /* Operation failed */
    MULTIDICT_OK_REPLACED = 0, /* Key existed, value was replaced */
    MULTIDICT_OK_INSERTED = 1  /* New key was inserted */
} multidictResult;

/* Hash table configuration: how keys hash and match (see How Keys Match) */
typedef struct multidictType {
    uint32_t (*hashFunction)(struct multidict *d, const void *key,
                             uint32_t len);
    int32_t (*keyCompare)(void *privdata, const void *key1,
                          const uint32_t key1_sz, const void *key2,
                          const uint32_t key2_sz);
} multidictType;

/* Pluggable storage class: 17 pointer-sized fields plus one flag byte.
 * A slot is whatever the class stores in a table cell; hooks that may
 * reallocate it take the cell (multidictSlot **) and update it. */
typedef struct multidictClass {
    void *privdata;  /* Private data for this class */

    /* Core operations.  insertByType returns 1 (new key), 0 (replaced; the
     * overwritten value goes to 'replaced' when not NULL) or -1. */
    int64_t (*insertByType)(struct multidictClass *qdc, multidictSlot **slot,
                            const databox *keybox, const databox *valbox,
                            databox *replaced);
    void *(*operationSlotGetOrCreate)(struct multidictClass *qdc,
                                      multidictSlot **slot,
                                      const databox *keybox);
    /* One search; 'removedVal' (may be NULL) gets the removed value:
     * type and length only, or an owned copy with 'take' */
    bool (*operationRemove)(struct multidictClass *qdc, multidictSlot **slot,
                            const databox *keybox, databox *removedVal,
                            bool take);
    bool (*findValueByKey)(struct multidictClass *qdc, multidictSlot *slot,
                           const databox *keybox, databox *valbox);
    void *(*createSlot)(void);
    uint32_t (*freeSlot)(struct multidictClass *qdc, multidictSlot *slot);
    size_t (*sizeBytes)(multidictSlot *slot);

    /* Iteration */
    bool (*getIter)(struct multidictSlotIterator *iter, multidictSlot *slot);
    bool (*iterNext)(struct multidictSlotIterator *iter,
                     struct multidictEntry *entry);

    /* Random access */
    uint32_t (*countSlot)(multidictSlot *slot);
    bool (*findKeyByPosition)(struct multidictClass *qdc, multidictSlot *slot,
                              uint32_t pos, databox *keybox);

    /* Scanning */
    void (*iterateAll)(struct multidictClass *qdc, multidictSlot *slot,
                       multidictIterProcess process, void *privdata);

    /* Rewrite a present key's value in one search: 'update' sees the
     * stored value and supplies the new one (or refuses); 1 rewritten,
     * 0 refused, -1 absent.  IncrBy, IncrByFloat, AddXX, Replace. */
    int32_t (*updateByType)(struct multidictClass *qdc, multidictSlot **slot,
                            const databox *keybox,
                            multidictValueUpdate *update, void *ctx);

    /* Rehash reads each old slot with getIter/iterNext, inserts every row
     * into its new slot with insertByType (which copies what the row
     * borrows) and frees the old slot whole with freeSlot. */

    /* Cleanup */
    void (*free)(struct multidictClass *qdc);
    uint32_t (*freeClass)(struct multidictClass *qdc);

    uint8_t disableResize;  /* Global resize control */
} multidictClass;
```

### Iterator Types

```c
/* Slot-level iterator: 144 bytes, 16-byte aligned (statically asserted) */
typedef struct multidictSlotIterator {
    uint8_t iterspace[64];   /* Stack-allocated iterator state */
    multidictSlot *slot;
    void *entry;
    uint32_t index;
    databoxWide wide[3];     /* Wide key, val, extra of the current entry */
} multidictSlotIterator;

/* Dictionary-level iterator */
typedef struct multidictIterator {
    multidict *d;
    multidictSlotIterator iter;
    multidictSlot *current;
    uint64_t fingerprint;    /* Detect modifications during iteration */
    int64_t index;           /* Current slot index */
    int32_t table;           /* 0 or 1 (which hash table) */
    bool safe;               /* Safe to modify during iteration? */
} multidictIterator;

/* Entry returned during iteration: 48 bytes (statically asserted) */
typedef struct multidictEntry {
    databox key;
    databox val;
    databox extra; /* Optional third field */
} multidictEntry;
```

The library sets the key and value flags up before it fills the entry,
so an entry needs no initialization: a 96/128-bit value has `big = 1`,
every other value `big = 0`, and `allocated = 0` throughout. The fields
borrow. BYTES point into the dict until it changes. A 96/128-bit value
points into the iterator's `wide` slots (`wide[0]` for the key,
`wide[1]` for the value, `wide[2]` for `extra`) and is valid until the
next `multidictIteratorNext` on that iterator. A struct copy of an
iterator has its own slots, and a field read from the original still
points into the original. Keep a value longer with `databoxCopy` (owned;
release with `databoxFreeData`) or `databoxBigCopyFrom` (re-homed into a
`databoxBig`).

A slot class's `iterNext` fills entries the same way: it decodes a wide
field into `iter->wide[i]` (with `flexGetByTypeInto`, or
`multimapIteratorNextWithSlots` for a multimap-backed slot) and sets up
the key and value flags.

### Statistics Types

```c
/* Lightweight, always available */
typedef struct multidictStats {
    uint64_t count;      /* Total number of key-value pairs */
    uint64_t slots;      /* Total number of hash slots */
    uint32_t loadFactor; /* count * 100 / slots */
    uint64_t usedBytes;  /* Bytes of the slots: the sum of their sizes */
    uint64_t keyBytes;   /* Keys: a 64-bit number or real counts 8, a
                            96/128-bit one 16, bytes their length */
    uint64_t valBytes;   /* Values, counted as keys are */
    uint64_t totalBytes; /* Total memory usage */
    bool isRehashing;
} multidictStats;

/* Adds chain-length distribution and ht[1] stats during rehash */
#define MULTIDICT_STATS_VECTLEN 50
typedef struct multidictDetailedStats {
    multidictStats basic;
    uint64_t usedSlots;
    uint64_t maxChainLen;
    float avgChainLen;
    uint64_t chainDistribution[MULTIDICT_STATS_VECTLEN];
    bool hasRehashTable;
    multidictStats rehashTable;
} multidictDetailedStats;

/* Count-based and byte-based load, side by side */
typedef struct multidictLoadMetrics {
    uint32_t countLoadFactor; /* (count * 100) / slots */
    uint64_t avgSlotBytes;    /* usedBytes / usedSlots (0 if none) */
    uint32_t byteLoadFactor;  /* (avgSlotBytes * 100) / targetSlotBytes */
    uint64_t usedSlots;
    uint64_t totalUsedBytes;
    uint64_t maxSlotBytes;
    uint64_t targetSlotBytes;
} multidictLoadMetrics;
```

## Creation Functions

### Basic Creation

```c
/* Create new multidict with hash type and storage class */
multidict *multidictNew(multidictType *type, multidictClass *qdc, int32_t seed);

/* Get the class from an existing multidict (for creating siblings) */
multidictClass *multidictGetClass(multidict *d);

/* Ready-made multimap-backed slot class - use this unless you need
 * custom slot storage */
multidictClass *multidictDefaultClassNew(void);
void multidictDefaultClassFree(multidictClass *qdc);

/* Example: Create with exact key matching using the built-in class */
multidictClass *cls = multidictDefaultClassNew();
multidict *d = multidictNew(&multidictTypeExactKey, cls, 42);

/* Example: Create with case-insensitive keys */
multidict *ci = multidictNew(&multidictTypeCaseKey, cls, 42);

/* The class is shared, so free it only after every dict using it is gone */
multidictFree(ci);
multidictFree(d);
multidictDefaultClassFree(cls);
```

### Hash Function Seeding

```c
/* Set hash seed. The seed is applied unconditionally; the return value
 * reports whether it was inside the recommended bound of 2^20. */
bool multidictSetHashFunctionSeed(multidict *d, uint32_t seed);

/* Get current seed */
uint32_t multidictGetHashFunctionSeed(multidict *d);

/* Example */
if (!multidictSetHashFunctionSeed(d, 12345)) {
    /* seed applied anyway, but it exceeded 2^20 */
}
```

## Core Operations

### Insertion

```c
/* Add or replace a key-value pair */
multidictResult multidictAdd(multidict *d, const databox *keybox,
                             const databox *valbox);

/* Example: Insert string key-value */
databox key = databoxNewBytesString("username");
databox val = databoxNewBytesString("alice");

switch (multidictAdd(d, &key, &val)) {
case MULTIDICT_OK_INSERTED: /* new key */ break;
case MULTIDICT_OK_REPLACED: /* existing key, new value */ break;
case MULTIDICT_ERR: /* slot class rejected the insert */ break;
}
```

`multidictAdd` is upsert semantics. While a rehash is in flight it first
removes any copy of the key still living in `ht[0]`, so a key can never be
duplicated across the two tables and the per-table counts stay exact.

### Conditional Insert

```c
/* Insert only when the key does NOT already exist */
multidictResult multidictAddNX(multidict *d, const databox *keybox,
                               const databox *valbox);

/* Update only when the key DOES already exist */
multidictResult multidictAddXX(multidict *d, const databox *keybox,
                               const databox *valbox);

/* Explicit replace (same shape as Add, named for intent) */
multidictResult multidictReplace(multidict *d, const databox *keybox,
                                 const databox *valbox);

/* Example: claim a name only if unclaimed */
if (multidictAddNX(d, &key, &val) == MULTIDICT_OK_INSERTED) {
    /* we won the race */
}
```

`AddNX` returns `MULTIDICT_ERR` when the key already exists; `AddXX` and
`multidictReplace` return `MULTIDICT_ERR` when it does not. `ERR` here means
"precondition not met", not "something broke".

### Read-Modify-Write in One Search

```c
/* 'update' sees the stored value and fills the new one, or returns false
 * to change nothing: MULTIDICT_UPDATE_REWRITTEN (one LRU use),
 * MULTIDICT_UPDATE_DECLINED, MULTIDICT_UPDATE_ABSENT */
multidictUpdateResult multidictUpdateValue(multidict *d, const databox *keybox,
                                           multidictValueUpdate *update,
                                           void *ctx);
```

The key is found once and the value is rewritten where it was found. The
callback may do work of its own before deciding (it must not change the same
dict): the persisted wrapper `multidictReplaceP` logs the change from inside
it and declines when the log is refused, so a replace logs and applies in one
search with nothing to undo. `AddXX` and `Replace` are this call with a
callback that always takes the new value.

### Bulk Insert and Delete

```c
/* Returns how many operations succeeded */
uint32_t multidictAddMultiple(multidict *d, const databox *keys,
                              const databox *vals, uint32_t count);
uint32_t multidictDeleteMultiple(multidict *d, const databox *keys,
                                 uint32_t count);
```

### Numeric Updates

```c
/* Increment an integer value in place; *result gets the new value */
multidictResult multidictIncrBy(multidict *d, const databox *keybox,
                                int64_t increment, int64_t *result);

/* Increment a floating point value in place */
multidictResult multidictIncrByFloat(multidict *d, const databox *keybox,
                                     double increment, double *result);

/* Example: counter that creates itself on first use */
int64_t hits = 0;
multidictIncrBy(d, &key, 1, &hits);
printf("hits: %" PRId64 "\n", hits);
```

Both increment functions:

- create the key with the increment as its value when the key is missing;
- return `MULTIDICT_ERR` without touching anything when the key exists but
  holds a non-numeric value;
- write the result through `*result` before storing, so `result` is valid
  whenever the call did not return `MULTIDICT_ERR`;
- store the new value with `multidictAdd`, so they return
  `MULTIDICT_OK_INSERTED` on first use and `MULTIDICT_OK_REPLACED` after.

### Lookup

```c
/* Find value by key */
bool multidictFind(multidict *d, const databox *keybox, databox *valbox);

/* Convenience function for string keys: true only when the key is present
 * and its value is a byte string; *val borrows the value's bytes (length
 * not reported, not NUL-terminated) */
bool multidictFindByString(multidict *d, const char *key, uint8_t **val);

/* Existence checks that skip materializing the value */
bool multidictExists(multidict *d, const databox *keybox);
bool multidictExistsByString(multidict *d, const char *key);

/* Example: Lookup */
databox searchKey = databoxNewBytesString("username");
databox foundValue = {{0}};

if (multidictFind(d, &searchKey, &foundValue)) {
    printf("Found: %.*s\n",
           (int)foundValue.len,
           foundValue.data.bytes.start);
}

/* Example: a value that may be 96/128-bit.  The caller owns the
 * destination, so it sets up the wide capability (re-run
 * DATABOX_BIG_INIT before each call). */
databoxBig wideValue;
DATABOX_BIG_INIT(&wideValue);
if (multidictFind(d, &searchKey, (databox *)&wideValue)) {
    /* wideValue.data.u128 points at wideValue.extra if it is wide */
}
```

`valbox` is filled in place, as `flexGetByType` fills a box: a zeroed
`databox` receives a 96/128-bit value's type only (`data.ptr == NULL`).

### Deletion

```c
/* Delete entry by key */
bool multidictDelete(multidict *ht, const databox *keybox);

/* Example */
databox key = databoxNewBytesString("username");
bool deleted = multidictDelete(d, &key);

if (deleted) {
    printf("Entry deleted successfully\n");
}

/* Remove every entry but keep the dictionary alive */
void multidictEmpty(multidict *d);
```

### Atomic Get-and-Delete

```c
/* Fetch the value and remove the key in one pass */
bool multidictGetAndDelete(multidict *d, const databox *keybox,
                           databox *valbox);

/* The same, first showing the stored value to 'gate' within the same hash
 * and table lookup: a false answer keeps the entry, its value and its LRU
 * level.  'gate' runs inside the dict's operation and must not use the
 * dict (a persisted wrapper writes its log there). */
typedef bool(multidictRemoveGate)(void *ctx, const databox *value);
bool multidictGetAndDeleteIf(multidict *d, const databox *keybox,
                             databox *valbox, multidictRemoveGate *gate,
                             void *ctx);

/* Pop an arbitrary entry (useful for draining a work set) */
bool multidictPopRandom(multidict *d, databox *keybox, databox *valbox);

/* Example: drain the dictionary */
databox k, v;
while (multidictPopRandom(d, &k, &v)) {
    process(&k, &v);
    databoxFreeData(&k);
    databoxFreeData(&v);
}
```

The entry is gone once these return, so they never read their
destination boxes and hand back self-contained values: BYTES and
96/128-bit values are heap copies (`allocated = 1`). Release every
returned box with `databoxFreeData`.

### 96/128-bit Keys and Values

Keys and values may be 96/128-bit integers. Where such a value lands
follows the rule in `databox.h`: whoever owns the destination sets up
its wide capability.

| Destination                                                | Who sets it up | Wide value                                                                   |
| ---------------------------------------------------------- | -------------- | ---------------------------------------------------------------------------- |
| `multidictFind` `valbox`, `multidictGetRandomKey` `keybox` | caller         | written to a `DATABOX_BIG_INIT`'d `databoxBig`; type only into a zeroed box  |
| `multidictGetSomeKeys` `keys[]`                            | library        | deferred (type only, `databoxIsWideDeferred`); prior contents ignored        |
| `multidictGetAndDelete`, `multidictPopRandom` outputs      | library        | heap copy, prior contents ignored                                            |
| `multidictEntry` fields                                    | library        | in the iterator's `wide` slots, valid until the next `multidictIteratorNext` |
| scan and eviction callback arguments                       | library        | in stack slots, valid for the call; `big = 1` exactly for a wide value       |

- A key hashes by value: a 128-bit box holding a value that fits 64 bits
  finds the entry stored under the 64-bit form.
- The byte statistics count a 96/128-bit key or value as 16 bytes.
- `multidictIncrBy` and `multidictIncrByFloat` treat a 96/128-bit value
  as non-numeric and return `MULTIDICT_ERR`.

## Copy and Merge

```c
/* Deep copy, including the dictionary's configuration */
multidict *multidictCopy(const multidict *src);

typedef enum {
    MULTIDICT_MERGE_REPLACE, /* src wins on collision */
    MULTIDICT_MERGE_KEEP     /* dst wins on collision */
} multidictMergeMode;

/* Fold src into dst; returns the number of src entries actually applied.
 * With MULTIDICT_MERGE_KEEP, collisions are skipped and not counted. */
uint64_t multidictMerge(multidict *dst, const multidict *src,
                        multidictMergeMode mode);

/* Example: layer defaults under user overrides */
uint64_t added = multidictMerge(userConfig, defaults, MULTIDICT_MERGE_KEEP);
printf("pulled in %" PRIu64 " defaults\n", added);
```

## Iteration

### Basic Iteration

```c
/* Initialize iterator (non-safe) */
bool multidictIteratorInit(multidict *d, multidictIterator *iter);

/* Initialize safe iterator (allows modifications during iteration) */
bool multidictIteratorGetSafe(multidict *d, multidictIterator *iter);

/* Get next entry */
bool multidictIteratorNext(multidictIterator *iter, multidictEntry *e);

/* Release iterator */
void multidictIteratorRelease(multidictIterator *iter);

/* Example: Iterate all entries */
multidictIterator iter;
multidictIteratorInit(d, &iter);

multidictEntry entry;
while (multidictIteratorNext(&iter, &entry)) {
    printf("Key: ");
    databoxRepr(&entry.key);
    printf("Value: ");
    databoxRepr(&entry.val);
    printf("\n");
}

multidictIteratorRelease(&iter);
```

### Safe vs Unsafe Iteration

**Unsafe Iterator** (`multidictIteratorInit`):

- Cannot call `multidictAdd`, `multidictDelete`, or anything that mutates the
  dictionary during iteration
- Captures a 64-bit fingerprint of the dictionary state and re-checks it in
  `multidictIteratorRelease`
- The fingerprint check is an `assert`, so it catches misuse in debug builds
  and vanishes under `NDEBUG` — it is a development guard, not a runtime
  safety net
- Faster: no per-dictionary tracking

**Safe Iterator** (`multidictIteratorGetSafe`):

- Can modify the dictionary during iteration
- Increments the dictionary's active-iterator count
- That count suppresses automatic rehashing for as long as any safe iterator
  is outstanding

Both flavors set up their bookkeeping on the **first** `multidictIteratorNext`
call, not at init time. An iterator that is initialized and released without
ever calling `Next` is a no-op on both paths, which is why the abandoned-loop
case is harmless. Always pair a loop that did iterate with a `Release`,
otherwise a safe iterator leaves rehashing pinned off forever.

```c
/* Safe iteration with modifications */
multidictIterator iter;
multidictIteratorGetSafe(d, &iter);

multidictEntry entry;
while (multidictIteratorNext(&iter, &entry)) {
    /* Safe to call multidictAdd or multidictDelete here */
    if (shouldDelete(&entry)) {
        multidictDelete(d, &entry.key);
    }
}

multidictIteratorRelease(&iter);
```

## SCAN Operation

```c
/* Stateless iteration callback */
typedef void(multidictScanFunction)(void *privdata, const databox *key,
                                   const databox *val);

/* Perform SCAN operation */
uint64_t multidictScan(multidict *d, uint64_t cursor,
                       multidictScanFunction *fn, void *privdata);

/* Example: SCAN all entries */
void printEntry(void *privdata, const databox *key, const databox *val) {
    int *count = privdata;
    printf("Entry %d: ", (*count)++);
    databoxRepr(key);
    printf(" -> ");
    databoxRepr(val);
    printf("\n");
}

int count = 0;
uint64_t cursor = 0;

do {
    cursor = multidictScan(d, cursor, printEntry, &count);
} while (cursor != 0);
```

The SCAN algorithm:

- **Stateless**: No iterator to maintain between calls
- **Consistent**: All elements present throughout scan are returned
- **Resilient**: Handles rehashing gracefully
- **May return duplicates**: If rehashing occurs during scan
- Uses **reverse binary iteration** of hash bits for rehash safety

## Random Access

```c
/* Get random key from dictionary */
bool multidictGetRandomKey(multidict *d, databox *keybox);

/* Sample up to 'count' keys; returns how many were written.
 * The count is clamped to the dictionary size, and the sampler also
 * advances any in-flight rehash while it walks. */
uint32_t multidictGetSomeKeys(multidict *d, databox *keys, uint32_t count);

/* Example */
databox randomKey = {{0}}; /* zeroed: a wide key arrives type-only */
if (multidictGetRandomKey(d, &randomKey)) {
    printf("Random key: ");
    databoxRepr(&randomKey);
    printf("\n");
}

/* Example: sample 16 keys for an approximate-eviction pass */
databox sample[16];
const uint32_t got = multidictGetSomeKeys(d, sample, 16);
for (uint32_t i = 0; i < got; i++) {
    consider(&sample[i]);
}
```

`multidictGetSomeKeys` ignores what `keys[]` held: each slot is cleared
before it is filled, so a 96/128-bit key comes back deferred (type only,
`data.ptr == NULL`; test with `databoxIsWideDeferred`).

`multidictGetRandomKey` and `multidictPopRandom` fail only on an empty
dict, rehash in flight or not. Deletes leave an emptied slot allocated, so
a random probe can land on one: after a bounded number of probes the call
walks the live slots from a random start and takes the first entry it
finds.

A BYTES key from either sampler borrows from its slot. Any later call on
the dict, a lookup included, can run a rehash step that moves the entry and
frees that slot, so copy the key (`databoxCopy`) before using it with the
dict again, for example to find or delete it. `multidictEvictToLimit` and
`multidictPopRandom` do this themselves.

## Resize Operations

### Manual Resize

```c
/* Expand to specific size (rounds up to power of 2) */
bool multidictExpand(multidict *d, uint64_t size);

/* Resize to minimal size containing all elements */
bool multidictResize(multidict *d);

/* Example: Expand to hold at least 10000 elements */
multidictExpand(d, 10000);
```

`multidictExpand` returns **false** without doing anything when a rehash is
already in flight, or when the rounded-up power of two equals the current
slot count. A false return is a normal outcome, not an error to abort on.

### Resize Control

```c
/* Enable automatic resizing */
void multidictResizeEnable(multidict *d);

/* Disable automatic resizing */
void multidictResizeDisable(multidict *d);

/* Example: Disable resizing during bulk operations */
multidictResizeDisable(d);
for (int i = 0; i < 1000000; i++) {
    /* ... bulk inserts ... */
}
multidictResizeEnable(d);
multidictResize(d);  /* Resize once when done */
```

### Incremental Rehashing

```c
/* Perform N steps of rehashing */
bool multidictRehash(multidict *d, int32_t n);

/* Rehash for specified milliseconds */
int64_t multidictRehashMilliseconds(multidict *d, int64_t ms);

/* Example: Rehash for 100ms */
int64_t rehashed = multidictRehashMilliseconds(d, 100);
printf("Rehashed %" PRId64 " slots\n", rehashed);
```

## Memory Management

```c
/* Free entire dictionary */
void multidictFree(multidict *d);

/* Example */
multidictFree(d);
d = NULL;
```

## Metadata and Statistics

`struct multidict` is **opaque** — it is defined in `multidict.c`, not in
the header. Use the accessor functions; the old macro names remain only as
thin aliases:

```c
/* Accessors (the real API) */
uint64_t multidictCount(const multidict *d);     /* Total key-value pairs */
uint64_t multidictSlotCount(const multidict *d); /* Total hash slots */
bool multidictIsRehashing_(const multidict *d);  /* Rehash in progress? */

/* Backwards compatibility aliases */
#define multidictSize(d) multidictCount(d)
#define multidictSlots(d) multidictSlotCount(d)
#define multidictIsRehashing(d) multidictIsRehashing_(d)

/* Example */
printf("Dictionary has %" PRIu64 " elements in %" PRIu64 " slots\n",
       multidictCount(d), multidictSlotCount(d));

if (multidictIsRehashing(d)) {
    printf("Currently rehashing...\n");
}
```

### Structured Statistics

```c
void multidictGetStats(multidict *d, multidictStats *stats);
void multidictGetDetailedStats(multidict *d, multidictDetailedStats *stats);
void multidictGetLoadMetrics(multidict *d, multidictLoadMetrics *metrics);

uint32_t multidictLoadFactor(multidict *d);
uint64_t multidictBytes(multidict *d);
uint64_t multidictKeyBytes(multidict *d);
uint64_t multidictValBytes(multidict *d);

/* Example: report the worst chain in the table */
multidictDetailedStats detailed;
multidictGetDetailedStats(d, &detailed);
printf("slots used %" PRIu64 ", longest chain %" PRIu64 ", avg %.2f\n",
       detailed.usedSlots, detailed.maxChainLen,
       (double)detailed.avgChainLen);
```

`multidictPrintStats()` is a human-readable dump of the same data, and is
compiled **only when `DATAKIT_TEST` is defined**:

```c
#ifdef DATAKIT_TEST
void multidictPrintStats(multidict *d);
#endif
```

## Self-Management

### Memory Ceiling and Eviction

```c
/* Called before an entry is evicted; return false to veto the eviction */
typedef bool(multidictEvictionCallback)(void *privdata, const databox *key,
                                        const databox *val);

void multidictSetMaxMemory(multidict *d, uint64_t maxBytes); /* 0 = no limit */
uint64_t multidictGetMaxMemory(multidict *d);
void multidictSetEvictionCallback(multidict *d, multidictEvictionCallback *cb,
                                  void *privdata);

bool multidictIsOverLimit(multidict *d);
uint32_t multidictEvictToLimit(multidict *d); /* returns entries evicted */

/* Example: 64MB cap with cleanup notification */
static bool onEvict(void *privdata, const databox *key, const databox *val) {
    releaseExternalResource(key, val);
    return true; /* allow */
}

multidictSetMaxMemory(d, 64ULL * 1024 * 1024);
multidictSetEvictionCallback(d, onEvict, NULL);
```

The eviction callback runs between the dict's operations, never inside one:
the victim's value is read, the callback answers, and only then is the entry
deleted. So the callback may read the dict. This costs a second lookup per
eviction when a callback is set, which keeps user code out of a half-done
operation (unlike `multidictGetAndDeleteIf`'s gate, which is library code that
leaves the dict alone).

```c

if (multidictIsOverLimit(d)) {
    printf("evicted %" PRIu32 " entries\n", multidictEvictToLimit(d));
}
```

### Eviction Policy and LRU Tracking

LRU tracking is off by default and costs nothing while disabled.

```c
typedef enum multidictEvictPolicy {
    MULTIDICT_EVICT_NONE = 0, /* No eviction (default) */
    MULTIDICT_EVICT_RANDOM,   /* Random victim */
    MULTIDICT_EVICT_LRU,      /* Least Recently Used */
    MULTIDICT_EVICT_LFU,      /* Least Frequently Used (approximate) */
    MULTIDICT_EVICT_SIZE_LRU, /* LRU weighted by entry size */
} multidictEvictPolicy;

bool multidictEnableLRU(multidict *d, size_t levels);
void multidictDisableLRU(multidict *d);
bool multidictHasLRU(multidict *d);

void multidictSetEvictPolicy(multidict *d, multidictEvictPolicy policy);
multidictEvictPolicy multidictGetEvictPolicy(multidict *d);

void multidictTouch(multidict *d, const databox *keybox);
int multidictGetLRULevel(multidict *d, const databox *keybox);

/* Example: scan-resistant cache */
multidictEnableLRU(d, 7);
multidictSetEvictPolicy(d, MULTIDICT_EVICT_LRU);
multidictSetMaxMemory(d, 256ULL * 1024 * 1024);
```

The LRU levels are backed by [multilru](MULTILRU.md), so the level count
here means the same thing it does there.

A new key is tracked at the coldest level, and a delete stops tracking it.
Usage follows the **named-key rule**: every call that names an existing key
is one use of it and promotes it exactly one level, whatever the outcome:

- `multidictFind`, `multidictFindByString`, `multidictTouch`;
- `multidictExists`, `multidictExistsByString`;
- `multidictAdd` of a present key, `multidictAddNX` (also when it refuses
  because the key exists), `multidictAddXX`, `multidictReplace`;
- `multidictIncrBy` / `multidictIncrByFloat` of a present key, also when the
  increment policy refuses the result.

Calls that name no existing key never promote: a miss, iteration, scans,
random keys, persistence, rehashing, and `multidictGetLRULevel` (an
inspector).

Under `MULTIDICT_EVICT_LRU`, `multidictEvictToLimit` evicts the coldest
entry first: the lowest level, oldest within it. A victim leaves LRU
tracking only when its delete commits. Reading its value for the eviction
callback is not an access, so an entry the callback vetoes stays tracked
with its level and rank unchanged and is evictable again later. Within one
call a vetoed entry is not offered again: eviction moves on to the
next-coldest entry, and once 64 entries have been passed over, the rest of
the call picks random victims.

### Byte-Based Expansion

By default expansion is driven by element count against slot count. Byte-based
expansion instead watches the **average slot size**, which is the right signal
when a slot class packs many entries into one allocation.

```c
void multidictEnableByteBasedExpansion(multidict *d, uint64_t targetSlotBytes,
                                       uint64_t maxSlotBytes);
void multidictDisableByteBasedExpansion(multidict *d);
bool multidictIsByteBasedExpansion(multidict *d);

/* Passing 0 for either bound selects the default: 2MB target, 8MB max */
multidictEnableByteBasedExpansion(d, 0, 0);

multidictLoadMetrics metrics;
multidictGetLoadMetrics(d, &metrics);
printf("avg slot %" PRIu64 " bytes (%" PRIu32 "%% of target)\n",
       metrics.avgSlotBytes, metrics.byteLoadFactor);
```

## Hash Functions

### Built-in Hash Functions

```c
/* 32-bit integer hash (Thomas Wang's Mix Function) */
uint32_t multidictIntHashFunction(uint32_t key);

/* 64-bit integer hash (Thomas Wang's Mix Function) */
uint32_t multidictLongLongHashFunction(uint64_t key);

/* Generic byte hash: XXH64 on 64-bit builds, XXH32 otherwise, both
 * seeded from the dictionary's seed */
uint32_t multidictGenHashFunction(multidict *d, const void *key, int32_t len);

/* Case-insensitive ASCII hash: multidictGenHashFunction (same seed) over
 * the key with exactly 'A'-'Z' folded, as multidictTypeCaseKey's compare
 * folds; folds in stack chunks, never allocates */
uint32_t multidictGenCaseHashFunction(multidict *d, const uint8_t *buf,
                                      int32_t len);
```

### Predefined Types

```c
/* Exact key matching (case-sensitive, binary-safe) */
extern multidictType multidictTypeExactKey;

/* ASCII case-insensitive key matching (see How Keys Match) */
extern multidictType multidictTypeCaseKey;

/* Example: Use predefined types with the built-in slot class */
multidictClass *cls = multidictDefaultClassNew();
multidict *exact = multidictNew(&multidictTypeExactKey, cls, 0);
multidict *caseInsensitive = multidictNew(&multidictTypeCaseKey, cls, 0);
```

### How Keys Match

A type's `keyCompare` decides when two byte-string keys (`DATABOX_BYTES`,
`DATABOX_BYTES_EMBED`) are the same key. The dict reads it once, in
`multidictNew`:

- `multidictTypeExactKey` (or a `NULL` compare): keys match by their exact
  bytes. Keys are binary, so `"a\0b"` and `"a\0c"` are different keys. This
  path goes straight to the slot layer and pays nothing for the other kind.
- `multidictTypeCaseKey`: keys match ASCII case-insensitively. Only `'A'`-`'Z'`
  and `'a'`-`'z'` fold together; NUL, punctuation and bytes above `0x7F` must
  match exactly, and the C locale has no effect. Its hash folds the same
  letters, so keys that compare equal always hash equal.
- Any other compare: byte-string keys it reports equal are one key. Keys it
  reports equal must hash equal under the type's `hashFunction`. The compare
  gets the class's `privdata`.

Integer, float and other non-byte-string keys match by value, exactly, in
every type: signed and unsigned 5, `5.0f`, `5.0` and a 128-bit 5 are one key,
as are `-0.0` and 0 (they hash by value, through `databoxNumericHashKey`).
The dict stores the key box exactly as given; for minimal storage pass it
through `databoxCanonical` first (see DATABOX.md, "Canonical Numerals and
Minimal Storage").

Matching applies to every operation that looks a key up: add, the
conditional adds and replace, find, exists, delete, get-and-delete,
increments, touch and LRU level, eviction, copy and merge.

A dict holds one entry per key under the spelling that first inserted it:

```c
multidict *ci = multidictNew(&multidictTypeCaseKey, cls, 0);
databox foo = databoxNewBytesString("Foo");
databox FOO = databoxNewBytesString("FOO");
databox one = DATABOX_SIGNED(1);
databox two = DATABOX_SIGNED(2);
multidictAdd(ci, &foo, &one); /* MULTIDICT_OK_INSERTED, stored as "Foo" */
multidictAdd(ci, &FOO, &two); /* MULTIDICT_OK_REPLACED, still "Foo" = 2 */
/* multidictFind(ci, "foo") finds 2; iteration reports the key "Foo" */
```

Iteration, scans, random keys and eviction callbacks report the stored
spelling. A lookup that misses a key's exact bytes walks that key's slot,
comparing each byte-string key there, so a case dict pays per slot entry on
those lookups (two entries a slot on average at the default load factor).

### Persisting the Key Type

A `persistOpsMultidict` snapshot records how the dict's keys match, so a
restore rebuilds the same kind of dict:

- An exact-key dict writes its key/value pairs only; a snapshot without a
  record restores as an exact-key dict.
- A case dict, or a dict with any other type, writes one leading unsigned
  element first (case or custom), which makes the element count odd.
- The WAL carries no record: its operations replay onto the dict the snapshot
  restored, and every recovery starts from a snapshot, so replayed adds,
  replaces and deletes match keys the way the restored dict does. Compaction
  writes a new snapshot with the record.

```c
/* multidictP: create with a built-in type; open restores it */
multidictP *pd = multidictNewPWithType(ctx, &multidictTypeCaseKey);
multidictP *again = multidictOpenP(ctx2); /* a case dict again */
```

`persistOpsMultidict.restore` refuses a snapshot written by a dict with a
custom type, because restoring it under other key matching would merge or split
keys. To persist such a dict, give persist a copy of `persistOpsMultidict` whose
`.restore` calls `multidictPersistRestoreAs(data, len, &myType)`. That call
refuses any snapshot written by a dict of another kind (exact, case or custom),
so a mismatch fails the recovery instead of changing the keys.
`multidictNewPWithType` accepts only the built-in types.

## Creating a Custom multidictClass

Most callers should start with `multidictDefaultClassNew()`, which supplies a
complete multimap-backed slot class. Write your own only when you need a
different slot representation.

A `multidictClass` defines how data is stored in each hash slot. Here's a
minimal sketch using flex arrays:

```c
/* Simple flex-based slot implementation */
typedef flex multidictSlot;

void *createSlotFlex(void) {
    return flexNew();
}

uint32_t freeSlotFlex(multidictClass *qdc, multidictSlot *slot) {
    flex *f = slot;
    uint32_t count = flexCount(f) / 2;  /* key-value pairs */
    flexFree(f);
    return count;
}

size_t sizeBytesImpl(multidictSlot *slot) {
    return flexBytes((flex *)slot);
}

/* insertByType return contract (multidict.c relies on it):
 *     1  -> a NEW key was inserted   (maps to MULTIDICT_OK_INSERTED)
 *     0  -> an existing key's value was replaced (MULTIDICT_OK_REPLACED)
 *   < 0  -> failure                  (maps to MULTIDICT_ERR)
 * Getting this backwards corrupts the dictionary's count and byte
 * accounting, because multidict trusts it to decide whether to bump
 * count and keyBytes. */
int64_t insertByTypeImpl(multidictClass *qdc, multidictSlot **slot,
                         const databox *keybox, const databox *valbox,
                         databox *replaced) {
    (void)qdc;
    flex **f = (flex **)slot; /* writes may move the flex: update the cell */

    /* Rows are [key, value] pairs: look for the key */
    for (flexEntry *fe = flexHead(*f); fe; fe = flexNext(*f, flexNext(*f, fe))) {
        databox existingKey;
        flexGetByType(fe, &existingKey);
        if (databoxCompare(&existingKey, keybox) == 0) {
            flexEntry *valueEntry = flexNext(*f, fe);
            if (replaced) {
                flexGetByType(valueEntry, replaced); /* measured only */
            }
            flexReplaceByType(f, valueEntry, valbox);
            return 0; /* replaced */
        }
    }

    /* Not found: append the pair */
    flexPushByType(f, keybox, FLEX_ENDPOINT_TAIL);
    flexPushByType(f, valbox, FLEX_ENDPOINT_TAIL);
    return 1; /* inserted new */
}

/* ... implement other required functions ... */

multidictClass myFlexClass = {
    .privdata = NULL,
    .insertByType = insertByTypeImpl,
    .createSlot = createSlotFlex,
    .freeSlot = freeSlotFlex,
    .sizeBytes = sizeBytesImpl,
    /* ... set all other function pointers ... */
};
```

## Performance Characteristics

### Time Complexity

Every per-key cost here is "hash + slot dispatch + whatever the slot class
charges". With the default expand load factor of 200, slots hold roughly two
entries on average, so `k` below is small but never guaranteed to be 1.

| Operation          | Average       | Worst Case | Notes                                                   |
| ------------------ | ------------- | ---------- | ------------------------------------------------------- |
| Insert             | O(1) + O(k)   | O(n)       | Amortized; also removes any `ht[0]` twin during rehash  |
| Lookup             | O(1) + O(k)   | O(n)       | k depends on the slot implementation                    |
| Delete             | O(1) + O(k)   | O(n)       | One slot search per table (GetAndDelete, PopRandom too) |
| AddNX              | O(1) + O(k)   | O(n)       | Existence probe followed by an Add                      |
| AddXX/Replace      | O(1) + O(k)   | O(n)       | One slot search: the value is rewritten in place        |
| IncrBy/IncrByFloat | O(1) + O(k)   | O(n)       | One slot search for an existing key (an Add if absent)  |
| Iteration          | O(n)          | O(n)       | Iterates all slots and entries                          |
| SCAN               | O(1) per call | O(n)       | Full scan is O(n)                                       |
| Rehash Step        | O(1)          | O(k)       | k = entries in a single slot                            |
| Copy/Merge         | O(n)          | O(n)       | Merge is one Add per source entry                       |
| GetStats           | O(1)          | O(1)       | Counters are maintained incrementally                   |
| GetDetailedStats   | O(slots)      | O(slots)   | Walks every slot to build the histogram                 |

### Space Complexity

**Per Dictionary**:

- One `multidict` struct, which is considerably larger than the "140 bytes"
  comment in `multidict.c` suggests: on top of the two hash tables it carries
  memory-limit, eviction-callback, byte-expansion, and LRU fields
- Two `multidictHT` structs, each holding a table pointer plus `size`,
  `oversize`, `count`, `usedBytes`, `keyBytes`, `valBytes`, and `totalBytes`
- 8 bytes x slot_count (pointer array), per live table
- Variable (slot contents via multidictClass)
- When LRU tracking is enabled: a `multilru`, a pointer-to-key side array,
  and an auxiliary key-to-pointer dictionary

**Rehashing Overhead**:

- During rehash: ~2x memory (two hash tables)
- After rehash: back to normal

### Rehashing Strategy

multidict performs **incremental rehashing**:

1. On first operation after expansion, `ht[1]` is allocated
2. Each subsequent operation rehashes 1 slot from `ht[0]` to `ht[1]`
3. Lookups check both tables during rehashing
4. Inserts go to `ht[1]`, and `multidictAdd` explicitly removes any copy of
   the key still sitting in `ht[0]` first, so no key is ever present in both
   tables and each table's `count` stays exact
5. Once `ht[0]` is empty, it's freed and `ht[1]` becomes `ht[0]`

Safe iterators pin the dictionary: while any are outstanding, automatic
rehashing is suppressed so entries do not move under the iterator.

This spreads the rehashing cost across many operations instead of blocking.

## Best Practices

### 1. Choose Appropriate Hash Function

```c
multidictClass *cls = multidictDefaultClassNew();

/* Exact (case-sensitive) keys: XXH64/XXH32 over the key bytes */
multidict *exactDict = multidictNew(&multidictTypeExactKey, cls, 0);

/* Case-insensitive string keys: the same seeded hash over ASCII-folded
 * bytes */
multidict *ciDict = multidictNew(&multidictTypeCaseKey, cls, 0);
```

### 2. Disable Resizing During Bulk Operations

```c
multidictResizeDisable(d);

for (int i = 0; i < MANY; i++) {
    multidictAdd(d, &keys[i], &vals[i]);
}

multidictResizeEnable(d);
multidictResize(d);
```

### 3. Use Safe Iterators Only When Necessary

```c
/* Read-only: use unsafe iterator (faster) */
multidictIterator iter;
multidictIteratorInit(d, &iter);

/* Modifying during iteration: use safe iterator */
multidictIterator safeIter;
multidictIteratorGetSafe(d, &safeIter);
```

### 4. Implement Efficient Slot Classes

Your `multidictClass` should:

- Use memory-efficient data structures
- Implement fast key comparison
- Minimize allocations per operation
- Support efficient iteration

## Common Patterns

### Pattern 1: String Dictionary

```c
multidictClass *cls = multidictDefaultClassNew();
multidict *d = multidictNew(&multidictTypeExactKey, cls, 0);

/* Insert */
databox key = databoxNewBytesString("name");
databox val = databoxNewBytesString("Alice");
multidictAdd(d, &key, &val);

/* Lookup */
databox result;
if (multidictFind(d, &key, &result)) {
    printf("Name: %.*s\n", (int)result.len, result.data.bytes.start);
}
```

### Pattern 2: Counting Occurrences

```c
/* Using multidict to count string occurrences.
 * multidictIncrBy already handles the create-on-first-use case, so there
 * is no find/branch/replace dance to write by hand. */
databox key = databoxNewBytesString(word);

int64_t occurrences = 0;
if (multidictIncrBy(d, &key, 1, &occurrences) != MULTIDICT_ERR) {
    printf("%s seen %" PRId64 " times\n", word, occurrences);
}
```

### Pattern 3: SCAN with Filtering

```c
void filterPrint(void *privdata, const databox *key, const databox *val) {
    const char *prefix = privdata;
    const size_t prefixLen = strlen(prefix);

    /* Keys arrive as raw databoxes; resolve the bytes (which may be
     * embedded in the box itself) before comparing. */
    uint8_t *bytes = NULL;
    size_t len = 0;
    databoxGetBytes((databox *)key, &bytes, &len);

    if (bytes && len >= prefixLen && memcmp(bytes, prefix, prefixLen) == 0) {
        printf("Matched: ");
        databoxRepr(key);
        printf("\n");
    }
}

const char *prefix = "user:";
uint64_t cursor = 0;

do {
    cursor = multidictScan(d, cursor, filterPrint, (void *)prefix);
} while (cursor != 0);
```

## Debugging

```c
/* Print internal representation */
void multidictRepr(const multidict *d);

/* Print operation details */
void multidictOpRepr(const multidictOp *op);

/* Example */
#ifndef NDEBUG
multidictRepr(d);
printf("Size: %" PRIu64 ", Slots: %" PRIu64 "\n",
       multidictCount(d), multidictSlotCount(d));
#endif
```

## Thread Safety

multidict is **not thread-safe** by default. For concurrent access:

```c
pthread_rwlock_t lock = PTHREAD_RWLOCK_INITIALIZER;

/* Readers */
pthread_rwlock_rdlock(&lock);
multidictFind(d, &key, &val);
pthread_rwlock_unlock(&lock);

/* Writers */
pthread_rwlock_wrlock(&lock);
multidictAdd(d, &key, &val);
pthread_rwlock_unlock(&lock);
```

## Testing

`multidict.c` carries its own suite behind `DATAKIT_TEST`, entered through
`int multidictTest(int argc, char *argv[])`. It covers:

- Basic insertion, lookup, and deletion
- Conditional inserts and numeric increments
- Copy and merge
- Iterator functionality (safe and unsafe)
- SCAN operations
- Rehashing behavior and slot migration during rehash
- Statistics, load metrics, and introspection
- Memory limits, eviction, and LRU tracking

## See Also

- [multimap](../multimap/MULTIMAP.md) - Sorted key-value container; the
  default slot class is built on it
- [multilru](MULTILRU.md) - Backs the optional LRU eviction levels
- [databox](../core/DATABOX.md) - Universal value container used for keys/values
- [flex](../flex/FLEX.md) - Packed array structure useful for slot implementations

## Implementation Notes

**Design points**:

- Pluggable storage via `multidictClass`, so the hash layer never assumes a
  slot representation
- Integration with the databox type system for keys and values
- Incremental rehashing with explicit cross-table deduplication on insert
- Byte-aware expansion for slot classes that pack many entries per slot
- Optional self-management (memory ceiling, eviction policy, LRU levels)
  that costs nothing while unused

The multidict serves as a foundational building block, providing hash table
semantics with complete control over how data is stored and accessed within
each hash slot.
