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

**Headers**: `multidict.h`

**Source**: `multidict.c`

## Architecture

multidict uses a **two-table incremental rehashing** design similar to Redis:

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

## Data Structures

### Core Types

```c
/* Forward declaration */
typedef struct multidict multidict;

/* Hash table configuration */
typedef struct multidictType {
    uint32_t (*hashFunction)(struct multidict *d, const void *key,
                             uint32_t len);
    int32_t (*keyCompare)(void *privdata, const void *key1,
                          const uint32_t key1_sz, const void *key2,
                          const uint32_t key2_sz);
} multidictType;

/* Pluggable storage class (124 bytes) */
typedef struct multidictClass {
    void *privdata;  /* Private data for this class */

    /* Core operations */
    int64_t (*insertByType)(struct multidictClass *qdc, multidictSlot *slot,
                            const databox *keybox, const databox *valbox);
    void *(*operationSlotGetOrCreate)(struct multidictClass *qdc,
                                      multidictSlot **slot,
                                      const databox *keybox);
    void *(*operationRemove)(struct multidictClass *qdc, multidictSlot **slot,
                             const databox *keybox);
    bool (*findValueByKey)(struct multidictClass *qdc, multidictSlot *slot,
                           const databox *keybox, databox *valbox);
    void *(*createSlot)(void);
    uint32_t (*freeSlot)(struct multidictClass *qdc, multidictSlot *slot);
    uint32_t (*sizeBytes)(multidictSlot *slot);

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

    /* Rehashing support */
    bool (*lastKey)(multidictSlot *slot, databox *keybox);
    bool (*migrateLast)(void *dst, void *src);

    /* Cleanup */
    void (*free)(struct multidictClass *qdc);
    uint32_t (*freeClass)(struct multidictClass *qdc);

    uint8_t disableResize;  /* Global resize control */
} multidictClass;
```

### Iterator Types

```c
/* Slot-level iterator (64 bytes + slot reference) */
typedef struct multidictSlotIterator {
    uint8_t iterspace[64];   /* Stack-allocated iterator state */
    multidictSlot *slot;
    void *entry;
    uint32_t index;
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

/* Entry returned during iteration (60 bytes) */
typedef struct multidictEntry {
    databox key;
    databox val;
    databox extra;  /* Optional third field */
} multidictEntry;
```

## Creation Functions

### Basic Creation

```c
/* Create new multidict with hash type and storage class */
multidict *multidictNew(multidictType *type, multidictClass *qdc, int32_t seed);

/* Get the class from an existing multidict (for creating siblings) */
multidictClass *multidictGetClass(multidict *d);

/* Example: Create with exact key matching */
multidictClass *myClass = /* ... create custom class ... */;
multidict *d = multidictNew(&multidictTypeExactKey, myClass, 42);

/* Example: Create with case-insensitive keys */
multidict *ci = multidictNew(&multidictTypeCaseKey, myClass, 42);
```

### Hash Function Seeding

```c
/* Set hash seed (max 2^20) */
bool multidictSetHashFunctionSeed(multidict *d, uint32_t seed);

/* Get current seed */
uint32_t multidictGetHashFunctionSeed(multidict *d);

/* Example */
multidictSetHashFunctionSeed(d, 12345);
```

## Core Operations

### Insertion

```c
/* Add key-value pair to dictionary */
int64_t multidictAdd(multidict *d, const databox *keybox,
                     const databox *valbox);

/* Example: Insert string key-value */
databox key = databoxNewBytesString("username");
databox val = databoxNewBytesString("alice");

int64_t result = multidictAdd(d, &key, &val);
/* result indicates slot-specific insertion result */
```

### Lookup

```c
/* Find value by key */
bool multidictFind(multidict *d, const databox *keybox, databox *valbox);

/* Convenience function for string keys */
bool multidictFindByString(multidict *d, char *key, uint8_t **val);

/* Example: Lookup */
databox searchKey = databoxNewBytesString("username");
databox foundValue = {{0}};

if (multidictFind(d, &searchKey, &foundValue)) {
    printf("Found: %.*s\n",
           (int)foundValue.len,
           foundValue.data.bytes.start);
}
```

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

**Unsafe Iterator**:
- Cannot call `multidictAdd`, `multidictDelete`, etc. during iteration
- Detects modifications via fingerprint
- Faster (no overhead tracking)

**Safe Iterator**:
- Can modify dictionary during iteration
- Tracks active safe iterators
- Prevents rehashing while iterators are active

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

/* Example */
databox randomKey;
if (multidictGetRandomKey(d, &randomKey)) {
    printf("Random key: ");
    databoxRepr(&randomKey);
    printf("\n");
}
```

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
printf("Rehashed %ld slots\n", rehashed);
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

```c
/* Get element count across all slots */
#define multidictSize(d) ((d)->ht[0].count + (d)->ht[1].count)

/* Get total slot count */
#define multidictSlots(d) ((d)->ht[0].size + (d)->ht[1].size)

/* Check if rehashing */
#define multidictIsRehashing(d) ((d)->rehashing)

/* Print statistics (debug builds only) */
void multidictPrintStats(multidict *d);

/* Example */
printf("Dictionary has %lu elements in %lu slots\n",
       multidictSize(d), multidictSlots(d));

if (multidictIsRehashing(d)) {
    printf("Currently rehashing...\n");
}
```

## Hash Functions

### Built-in Hash Functions

```c
/* 32-bit integer hash (Thomas Wang's Mix Function) */
uint32_t multidictIntHashFunction(uint32_t key);

/* 64-bit integer hash (Thomas Wang's Mix Function) */
uint32_t multidictLongLongHashFunction(uint64_t key);

/* Generic byte hash (XXHash) */
uint32_t multidictGenHashFunction(multidict *d, const void *key, int32_t len);

/* Case-insensitive ASCII hash (djb hash variant) */
uint32_t multidictGenCaseHashFunction(multidict *d, const uint8_t *buf,
                                      int32_t len);
```

### Predefined Types

```c
/* Exact key matching (case-sensitive) */
extern multidictType multidictTypeExactKey;

/* Case-insensitive key matching */
extern multidictType multidictTypeCaseKey;

/* Example: Use predefined types */
multidict *exact = multidictNew(&multidictTypeExactKey, myClass, 0);
multidict *caseInsensitive = multidictNew(&multidictTypeCaseKey, myClass, 0);
```

## Creating a Custom multidictClass

A `multidictClass` defines how data is stored in each hash slot. Here's a minimal example using flex arrays:

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

uint32_t sizeBytesImpl(multidictSlot *slot) {
    return flexBytes((flex *)slot);
}

int64_t insertByTypeImpl(multidictClass *qdc, multidictSlot *slot,
                         const databox *keybox, const databox *valbox) {
    flex *f = slot;

    /* Search for existing key */
    flexEntry *fe = flexHead(f);
    while (fe) {
        databox existingKey;
        flexGetByType(f, &fe, &existingKey);

        if (databoxCompare(&existingKey, keybox) == 0) {
            /* Replace value */
            flexGetByType(f, &fe, NULL);  /* skip old value */
            /* Delete old value and insert new one */
            /* ... implementation ... */
            return 1;  /* replaced */
        }

        flexGetByType(f, &fe, NULL);  /* skip value */
    }

    /* Not found, append */
    flexPushByType(&f, keybox, FLEX_ENDPOINT_TAIL);
    flexPushByType(&f, valbox, FLEX_ENDPOINT_TAIL);
    return 0;  /* inserted new */
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

| Operation | Average | Worst Case | Notes |
|-----------|---------|------------|-------|
| Insert | O(1) | O(n) | Amortized O(1) with incremental rehash |
| Lookup | O(1) | O(n) | Depends on slot implementation |
| Delete | O(1) | O(n) | Must find key in slot |
| Iteration | O(n) | O(n) | Iterates all slots and entries |
| SCAN | O(1) per call | O(n) | Full scan is O(n) |
| Rehash Step | O(1) | O(k) | k = entries in single slot |

### Space Complexity

**Per Dictionary**:
- 96 bytes (multidict struct)
- 32 bytes × 2 (ht[0] and ht[1])
- 8 bytes × slot_count (pointer array)
- Variable (slot contents via multidictClass)

**Rehashing Overhead**:
- During rehash: ~2x memory (two hash tables)
- After rehash: back to normal

### Rehashing Strategy

multidict performs **incremental rehashing**:

1. On first operation after expansion, `ht[1]` is allocated
2. Each subsequent operation rehashes 1 slot from `ht[0]` to `ht[1]`
3. Lookups check both tables during rehashing
4. Inserts always go to `ht[1]` during rehashing
5. Once `ht[0]` is empty, it's freed and `ht[1]` becomes `ht[0]`

This spreads the rehashing cost across many operations instead of blocking.

## Best Practices

### 1. Choose Appropriate Hash Function

```c
/* For integer keys */
multidict *intDict = multidictNew(&multidictTypeExactKey, class, 0);
/* Keys will be hashed using generic hash on bytes */

/* For case-insensitive string keys */
multidict *ciDict = multidictNew(&multidictTypeCaseKey, class, 0);
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
multidictClass *flexClass = /* ... flex-based implementation ... */;
multidict *d = multidictNew(&multidictTypeExactKey, flexClass, 0);

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
/* Using multidict to count string occurrences */
databox key = databoxNewBytesString(word);
databox count = {{0}};

if (multidictFind(d, &key, &count)) {
    /* Increment existing count */
    int64_t current = count.data.i64;
    count = databoxNewSigned(current + 1);
    /* Replace with new count... */
} else {
    /* First occurrence */
    count = databoxNewSigned(1);
    multidictAdd(d, &key, &count);
}
```

### Pattern 3: SCAN with Filtering

```c
void filterPrint(void *privdata, const databox *key, const databox *val) {
    const char *prefix = privdata;

    if (databoxStartsWith(key, prefix)) {
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
printf("Size: %lu, Slots: %lu\n", multidictSize(d), multidictSlots(d));
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

The test suite in `multidict.c` includes:
- Basic insertion and lookup
- Deletion operations
- Iterator functionality
- SCAN operations
- Rehashing behavior
- Slot migration during rehash
- Statistics and introspection

## See Also

- [multimap](../multimap/MULTIMAP.md) - Higher-level sorted key-value container built on multidict concepts
- [databox](../core/DATABOX.md) - Universal value container used for keys/values
- [flex](../flex/FLEX.md) - Packed array structure useful for slot implementations

## Implementation Notes

**Origin**: Based on Redis dict.c by Salvatore Sanfilippo, enhanced by Matt Stancliff

**Key Enhancements**:
- Pluggable storage via multidictClass
- Integration with databox type system
- Incremental rehashing optimizations
- Support for arbitrary slot implementations

The multidict serves as a foundational building block, providing hash table semantics with complete control over how data is stored and accessed within each hash slot.
