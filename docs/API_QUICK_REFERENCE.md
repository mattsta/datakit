# API Quick Reference

A task-oriented quick reference for common operations across all datakit modules.

**Purpose**: Find code snippets quickly when you know what you want to do.

**Organization**: By task, then by module. Use your editor's search (Ctrl+F) to find operations.

---

## Table of Contents

1. [Container Creation](#container-creation)
2. [Container Destruction](#container-destruction)
3. [Insertion Operations](#insertion-operations)
4. [Deletion Operations](#deletion-operations)
5. [Lookup & Search](#lookup--search)
6. [Iteration](#iteration)
7. [Indexing & Random Access](#indexing--random-access)
8. [Metadata & Size](#metadata--size)
9. [Memory Management](#memory-management)
10. [Type Conversion](#type-conversion)
11. [String Operations](#string-operations)
12. [Compression](#compression)
13. [Set Operations](#set-operations)

---

## Container Creation

### "I want to create a map/dictionary"

**multimap** (sorted key-value, auto-scaling):
```c
multimap *m = multimapNew(2);  // 2 = key+value
```

**multidict** (hash table with custom storage):
```c
multidictClass *class = /* your storage class */;
multidict *d = multidictNew(&multidictTypeExactKey, class, 0);
```

**multiroar** (sparse bitmap/set):
```c
multiroar *r = multiroarBitNew();  // For set membership
```

### "I want to create a list"

**multilist** (linked list of packed arrays):
```c
multilist *ml = multilistNew(FLEX_CAP_LEVEL_2048, 0);
```

**flex** (compact variable-length array):
```c
flex *f = flexNew();
```

**mflex** (auto-compressed flex):
```c
mflex *m = mflexNew();
mflexState *state = mflexStateCreate();
```

### "I want to create an array"

**multiarray** (dynamic array, auto-scaling):
```c
multiarray *arr = multiarrayNew(sizeof(int64_t), 1024);
```

**Native inline array**:
```c
MyStruct *arr = multiarrayNativeNew(arr[1024]);
```

### "I want to create a set"

**intset** (variable-width integer set):
```c
intset *is = intsetNew();  // Starts with 16-bit encoding
```

**multimap as set** (sorted, unique keys):
```c
multimap *set = multimapSetNew(1);  // 1 element per entry
```

**multiroar** (sparse integer bitmap):
```c
multiroar *r = multiroarBitNew();
```

### "I want to create a cache"

**multilru** (multi-level LRU):
```c
multilru *lru = multilruNew();  // Default 7 levels
```

### "I want to create a string"

**databox** (small strings, embedded):
```c
databox str = databoxNewBytesString("hello");
```

**DKS** (dynamic string buffer):
```c
dksstr *s = dksstr_new();
```

---

## Container Destruction

### "I want to free a container"

```c
multimapFree(m);        // multimap
multidictFree(d);       // multidict
multilistFree(ml);      // multilist
multiarrayFree(arr);    // multiarray (not in API, free by variant)
flexFree(f);            // flex
mflexFree(mf);          // mflex
intsetFree(is);         // intset
multilruFree(lru);      // multilru
multiroarFree(r);       // multiroar

databoxFree(&box);      // databox (if allocated)
dksstr_free(s);         // DKS string
mflexStateFree(state);  // mflex state
```

### "I want to clear but keep allocated"

```c
multimapReset(m);       // multimap
flexReset(&f);          // flex
mflexReset(&mf);        // mflex
```

---

## Insertion Operations

### "I want to add to a map"

**multimap**:
```c
databox key = databoxNewBytesString("username");
databox val = databoxNewBytesString("alice");
const databox *elements[2] = {&key, &val};
multimapInsert(&m, elements);
```

**multidict**:
```c
databox key = databoxNewBytesString("key");
databox val = databoxNewSigned(42);
multidictAdd(d, &key, &val);
```

### "I want to add to a list"

**multilist (queue - push tail)**:
```c
databox item = databoxNewBytesString("task");
mflexState *state = mflexStateCreate();
multilistPushByTypeTail(&ml, state, &item);
```

**multilist (stack - push head)**:
```c
databox item = databoxNewSigned(42);
multilistPushByTypeHead(&ml, state, &item);
```

**flex (append)**:
```c
flexPushBytes(&f, "hello", 5, FLEX_ENDPOINT_TAIL);
flexPushSigned(&f, 42, FLEX_ENDPOINT_TAIL);
```

**mflex (compressed)**:
```c
mflexPushDouble(&mf, state, 3.14, FLEX_ENDPOINT_TAIL);
```

### "I want to add to an array"

**multiarray**:
```c
double value = 3.14;
multiarrayInsertAfter(&arr, 0, &value);
```

### "I want to add to a set"

**intset**:
```c
bool added;
intsetAdd(&is, 42, &added);  // added = true if new
```

**multiroar**:
```c
multiroarBitSet(r, 12345);
```

### "I want to insert in sorted order"

**multimap** (auto-sorted):
```c
const databox *elements[2] = {&key, &val};
multimapInsert(&m, elements);  // Maintains sort order
```

**flex** (manual sorted insert):
```c
flexEntry *middle = NULL;
databox box = databoxNewSigned(42);
flexInsertByTypeSortedWithMiddle(&f, &box, &middle);
```

---

## Deletion Operations

### "I want to remove from a map"

**multimap**:
```c
databox key = databoxNewBytesString("username");
multimapDelete(&m, &key);
```

**multidict**:
```c
multidictDelete(d, &key);
```

### "I want to remove from a list"

**multilist (pop from head - queue)**:
```c
databox result;
if (multilistPopHead(&ml, state, &result)) {
    // Use result
    databoxFreeData(&result);
}
```

**multilist (pop from tail - stack)**:
```c
databox result;
if (multilistPopTail(&ml, state, &result)) {
    // Use result
}
```

**flex (delete at position)**:
```c
flexEntry *fe = flexIndex(f, 5);
flexDelete(&f, &fe);
```

**flex (delete range)**:
```c
flexDeleteRange(&f, 0, 10);  // Delete first 10 elements
```

### "I want to remove from an array"

**multiarray**:
```c
multiarrayDelete(&arr, 5);  // Delete at index 5
```

### "I want to remove from a set"

**intset**:
```c
bool removed;
intsetRemove(&is, 42, &removed);
```

**multiroar**:
```c
multiroarRemove(r, 12345);
```

### "I want to evict LRU item"

**multilru**:
```c
multilruPtr evicted;
if (multilruRemoveMinimum(lru, &evicted)) {
    // evicted contains the LRU entry pointer
}
```

---

## Lookup & Search

### "I want to find by key"

**multimap**:
```c
databox searchKey = databoxNewBytesString("username");
databox value;
databox *results[1] = {&value};
if (multimapLookup(m, &searchKey, results)) {
    // Found!
}
```

**multidict**:
```c
databox foundValue;
if (multidictFind(d, &key, &foundValue)) {
    // Found!
}
```

**flex (linear search)**:
```c
databox search = databoxNewBytesString("needle");
flexEntry *found = flexFindByType(f, flexHead(f), &search, 0);
```

**flex (binary search - sorted)**:
```c
flexEntry *middle = flexMiddle(f, 1);
databox search = databoxNewSigned(42);
flexEntry *found = flexFindByTypeSortedWithMiddle(f, 1, &search, middle);
```

### "I want to check membership"

**intset**:
```c
if (intsetFind(is, 42)) {
    // 42 is in set
}
```

**multiroar**:
```c
if (multiroarBitGet(r, 12345)) {
    // Bit is set
}
```

**multimap**:
```c
if (multimapExists(m, &key)) {
    // Key exists
}
```

### "I want to check if exists"

```c
// multimap
bool exists = multimapExists(m, &key);

// multidict
bool exists = multidictFind(d, &key, NULL);

// intset
bool exists = intsetFind(is, value);

// multiroar
bool exists = multiroarBitGet(r, position);
```

---

## Iteration

### "I want to iterate a map"

**multimap**:
```c
multimapIterator iter;
multimapIteratorInit(m, &iter, true);  // true = forward

databox key, value;
databox *elements[2] = {&key, &value};
while (multimapIteratorNext(&iter, elements)) {
    // Process key and value
}
```

**multidict (safe - can modify)**:
```c
multidictIterator iter;
multidictIteratorGetSafe(d, &iter);

multidictEntry entry;
while (multidictIteratorNext(&iter, &entry)) {
    // Can call multidictDelete() here
}
multidictIteratorRelease(&iter);
```

**multidict (SCAN - stateless)**:
```c
void callback(void *privdata, const databox *key, const databox *val) {
    printf("Key: %s\n", key->data.bytes.start);
}

uint64_t cursor = 0;
do {
    cursor = multidictScan(d, cursor, callback, NULL);
} while (cursor != 0);
```

### "I want to iterate a list"

**multilist (forward)**:
```c
multimapIterator iter;
multilistIteratorInitForward(ml, state, &iter);

multilistEntry entry;
while (multilistNext(&iter, &entry)) {
    // Process entry.box
}
multilistIteratorRelease(&iter);
```

**multilist (reverse)**:
```c
multilistIteratorInitReverse(ml, state, &iter);
while (multilistNext(&iter, &entry)) {
    // Process in reverse
}
```

**flex (forward)**:
```c
flexEntry *fe = flexHead(f);
while (fe && flexEntryIsValid(f, fe)) {
    databox box;
    flexGetByType(fe, &box);
    // Process box
    fe = flexNext(f, fe);
}
```

**flex (reverse)**:
```c
flexEntry *fe = flexTail(f);
while (fe && flexEntryIsValid(f, fe)) {
    databox box;
    flexGetByType(fe, &box);
    // Process box
    fe = flexPrev(f, fe);
}
```

### "I want to iterate an array"

**multiarray (all elements)**:
```c
size_t count = multiarrayCount(arr);
for (size_t i = 0; i < count; i++) {
    int64_t *elem = (int64_t *)multiarrayGet(arr, i);
    // Process elem
}
```

### "I want to iterate a set"

**intset**:
```c
for (uint32_t i = 0; i < intsetCount(is); i++) {
    int64_t value;
    if (intsetGet(is, i, &value)) {
        // Process value (in sorted order)
    }
}
```

---

## Indexing & Random Access

### "I want to get by index"

**multiarray**:
```c
int64_t *value = (int64_t *)multiarrayGet(arr, 42);
int64_t *first = (int64_t *)multiarrayGetHead(arr);
int64_t *last = (int64_t *)multiarrayGetTail(arr);
```

**flex**:
```c
flexEntry *fe = flexIndex(f, 5);      // Positive index
flexEntry *last = flexIndex(f, -1);   // Negative index from end
flexEntry *head = flexHead(f);
flexEntry *tail = flexTail(f);
```

**multilist**:
```c
multilistEntry entry;
if (multilistIndexGet(ml, state, 5, &entry)) {
    // entry contains element at index 5
}
```

**intset** (sorted position):
```c
int64_t value;
intsetGet(is, 10, &value);  // 10th smallest value
```

### "I want first/last element"

```c
// multimap
databox first[2], last[2];
multimapFirst(m, first);
multimapLast(m, last);

// multilist
multilistEntry entry;
multilistIndexGet(ml, state, 0, &entry);   // First
multilistIndexGet(ml, state, -1, &entry);  // Last

// flex
flexEntry *head = flexHead(f);
flexEntry *tail = flexTail(f);
```

### "I want a random element"

```c
// multimap
databox randomKey;
multimapGetRandomKey(m, &randomKey);

// multidict
databox randomKey;
multidictGetRandomKey(d, &randomKey);

// intset
int64_t random = intsetRandom(is);

// multilru (random from LRU)
multilruPtr candidates[10];
multilruGetNLowest(lru, candidates, 10);  // 10 LRU candidates
```

---

## Metadata & Size

### "I want to know the size"

```c
// Count elements
size_t count = multimapCount(m);
size_t count = multidictSize(d);
size_t count = multilistCount(ml);
size_t count = multiarrayCount(arr);
size_t count = flexCount(f);
size_t count = mflexCount(mf);
size_t count = intsetCount(is);
size_t count = multilruCount(lru);

// Memory bytes
size_t bytes = multimapBytes(m);
size_t bytes = multilistBytes(ml);
size_t bytes = multiarrayBytes(arr);
size_t bytes = flexBytes(f);
size_t bytes = intsetBytes(is);
size_t bytes = multilruBytes(lru);
```

### "I want to check if empty"

```c
bool empty = (multimapCount(m) == 0);
bool empty = flexIsEmpty(f);
bool empty = mflexIsEmpty(mf);
bool empty = (intsetCount(is) == 0);
```

### "I want to know compression status"

**mflex**:
```c
bool compressed = mflexIsCompressed(mf);
size_t uncompressed = mflexBytesUncompressed(mf);
size_t compressed_size = mflexBytesCompressed(mf);
size_t actual = mflexBytesActual(mf);

if (compressed) {
    double ratio = (double)uncompressed / compressed_size;
    printf("Compression: %.2fx\n", ratio);
}
```

---

## Memory Management

### "I want to copy a container"

```c
multimap *copy = multimapCopy(m);
flex *copy = flexDuplicate(f);
mflex *copy = mflexDuplicate(mf);
intset *copy = /* no copy function, recreate manually */;
multiroar *copy = multiroarDuplicate(r);
```

### "I want to copy a databox"

```c
databox original = databoxNewBytesString("hello");
databox copy = databoxCopy(&original);
// copy has its own memory
databoxFree(&copy);
```

### "I want to grow a buffer efficiently"

**Using fibbuf (Fibonacci growth)**:
```c
size_t newSize = fibbufNextSizeBuffer(currentSize);
```

**Using jebuf (jemalloc size class)**:
```c
size_t allocSize = jebufSizeAllocation(requestedSize);
void *ptr = malloc(allocSize);  // No wasted memory!
```

**Combined (optimal)**:
```c
size_t fibSize = fibbufNextSizeBuffer(currentSize);
size_t allocSize = jebufSizeAllocation(fibSize);
ptr = realloc(ptr, allocSize);
```

### "I want to check if reallocation is worth it"

```c
if (jebufUseNewAllocation(currentSize, newSize)) {
    // newSize uses different jemalloc size class
    ptr = realloc(ptr, jebufSizeAllocation(newSize));
} else {
    // Same size class, keep current allocation
}
```

### "I want to preallocate"

```c
// multimap
multimapExpand(m, 10000);  // Rounds to power of 2

// multiarray
multiarray *arr = multiarrayNew(sizeof(int64_t), 10000);

// multilru
multilru *lru = multilruNewWithLevelsCapacity(7, 10000);
```

---

## Type Conversion

### "I want to convert string to number"

**Reliable round-trip conversion**:
```c
databox result;
if (StrScanScanReliable("3.14", 4, &result)) {
    // result contains optimal type (int64, uint64, float, or double)
    if (result.type == DATABOX_DOUBLE_64) {
        printf("Double: %f\n", result.data.d64);
    }
}
```

**Fast integer parsing**:
```c
uint64_t value;
if (StrBufToUInt64("12345", 5, &value)) {
    printf("Value: %lu\n", value);
}
```

**LuaJIT-style scanning**:
```c
databox result;
StrScanFmt fmt = StrScanScan("42", &result, STRSCAN_OPT_TOINT, false, false);
if (fmt == STRSCAN_INT) {
    printf("Integer: %ld\n", result.data.i64);
}
```

### "I want to convert number to string"

**Integer to string**:
```c
char buf[32];
size_t len = StrInt64ToBuf(buf, sizeof(buf), -12345);
printf("%.*s\n", (int)len, buf);  // "-12345"
```

**Double to string**:
```c
char buf[64];
size_t len = StrDoubleFormatToBufNice(buf, sizeof(buf), 3.14159);
printf("%.*s\n", (int)len, buf);  // "3.14159"
```

### "I want to create a databox from value"

```c
// Integers
databox i64 = databoxNewSigned(-42);
databox u64 = databoxNewUnsigned(12345);

// Floats
databox f = databoxNewReal(3.14159);  // double

// Bytes/Strings
databox str = databoxNewBytesString("hello");
databox bytes = databoxNewBytes(data, len);

// Embedded (≤8 bytes, no allocation)
databox small = databoxNewBytesAllowEmbed("hi", 2);

// Booleans
databox t = databoxBool(true);
databox f = DATABOX_BOX_FALSE;

// NULL
databox n = databoxNull();
```

### "I want to extract value from databox"

```c
if (DATABOX_IS_SIGNED_INTEGER(&box)) {
    int64_t value = box.data.i64;
}

if (DATABOX_IS_UNSIGNED_INTEGER(&box)) {
    uint64_t value = box.data.u64;
}

if (DATABOX_IS_FLOAT(&box)) {
    double value = box.data.d64;
}

if (DATABOX_IS_BYTES(&box)) {
    const uint8_t *data = databoxCBytes(&box);
    size_t len = databoxLen(&box);
}

if (DATABOX_IS_TRUE(&box)) {
    // Is true
}
```

---

## String Operations

### "I want to count UTF-8 characters"

```c
const char *str = "Hello 💛";  // 10 bytes
size_t chars = StrLenUtf8(str, strlen(str));  // 7 characters
size_t bytes = strlen(str);                    // 10 bytes
```

### "I want to get bytes for N characters"

```c
const char *str = "ab💛cd";
size_t bytes = StrLenUtf8CountBytes(str, strlen(str), 3);  // First 3 chars

char sub[bytes + 1];
memcpy(sub, str, bytes);
sub[bytes] = '\0';
// sub = "ab💛"
```

### "I want case-insensitive comparison"

```c
if (StrICmp("Hello", "HELLO") == 0) {
    // Equal (case-insensitive)
}

if (StrNICmp("Hello", "HELP", 3) == 0) {
    // First 3 chars match
}
```

### "I want to build a dynamic string"

```c
dksstr *s = dksstr_new();

dksstr_cat(s, "Hello");
dksstr_cat(s, " ");
dksstr_cat(s, "World");

printf("%s\n", s->buf);  // "Hello World"

dksstr_free(s);
```

---

## Compression

### "I want automatic compression"

**mflex (transparent)**:
```c
mflexState *state = mflexStateCreate();
mflex *m = mflexNew();

// Add data (auto-compresses when beneficial)
for (int i = 0; i < 10000; i++) {
    mflexPushDouble(&m, state, (double)i, FLEX_ENDPOINT_TAIL);
}

printf("Compressed: %s\n", mflexIsCompressed(m) ? "YES" : "NO");
printf("Ratio: %.2fx\n",
       (double)mflexBytesUncompressed(m) / mflexBytesActual(m));
```

### "I want manual compression control"

**Disable compression**:
```c
mflex *m = mflexNewNoCompress();  // Never compress
// OR
mflexSetCompressNever(&m, state);
```

**Enable compression**:
```c
mflexSetCompressAuto(&m, state);  // Try to compress
```

### "I want to work with compressed data"

**Open/close pattern**:
```c
// Open (decompresses if needed)
flex *f = mflexOpen(m, state);

// Work with uncompressed flex
flexPushSigned(&f, 42, FLEX_ENDPOINT_TAIL);

// Close (recompresses if beneficial)
mflexCloseGrow(&m, state, f);
```

**Read-only access** (no recompression):
```c
const flex *f = mflexOpenReadOnly(m, state);
// Read operations...
// No close needed
```

---

## Set Operations

### "I want union/intersection/difference"

**multiroar (bitmaps)**:
```c
multiroar *a = multiroarBitNew();
multiroar *b = multiroarBitNew();

// Union
multiroar *u = multiroarNewOr(a, b);

// Intersection
multiroar *i = multiroarNewAnd(a, b);

// Difference (a - b)
multiroar *notB = multiroarNewNot(b);
multiroar *diff = multiroarNewAnd(a, notB);

// XOR (symmetric difference)
multiroar *xor = multiroarNewXor(a, b);
```

**multimap (sorted sets)**:
```c
multimap *result = multimapNew(1);

multimapIterator iter1, iter2;
multimapIteratorInit(a, &iter1, true);
multimapIteratorInit(b, &iter2, true);

// Intersection
multimapIntersectKeys(&result, &iter1, &iter2);

// Difference
multimapIteratorInit(a, &iter1, true);
multimapIteratorInit(b, &iter2, true);
multimapDifferenceKeys(&result, &iter1, &iter2, false);
```

### "I want to set a range of bits"

**multiroar**:
```c
multiroarBitSetRange(r, 1000, 100);  // Set bits 1000-1099
```

### "I want to count set bits"

```c
// multiroar - count all set bits
size_t setBits = /* iterate and count */;

// intset - count unique values
size_t count = intsetCount(is);
```

---

## Common Patterns

### Pattern: LRU Cache

```c
#define MAX_SIZE 1000

typedef struct cache {
    multilru *lru;
    multimap *data;
} cache;

cache *cacheNew() {
    cache *c = malloc(sizeof(*c));
    c->lru = multilruNew();
    c->data = multimapNew(2);
    return c;
}

void cacheSet(cache *c, databox *key, databox *val) {
    // Evict if full
    if (multilruCount(c->lru) >= MAX_SIZE) {
        multilruPtr evicted;
        multilruRemoveMinimum(c->lru, &evicted);
        // Remove from data using evicted as key...
    }

    // Insert into LRU and data
    multilruPtr ptr = multilruInsert(c->lru);
    const databox *kv[2] = {key, val};
    multimapInsert(&c->data, kv);
}

void cacheGet(cache *c, databox *key, multilruPtr ptr) {
    // Mark as accessed
    multilruIncrease(c->lru, ptr);

    // Lookup value
    databox val;
    databox *results[1] = {&val};
    multimapLookup(c->data, key, results);
}
```

### Pattern: Time-Series Data

```c
multimap *timeseries = multimapNew(2);  // timestamp + value

// Insert
databox ts = databoxNewUnsigned(time(NULL));
databox temp = databoxNewReal(25.3);
const databox *reading[2] = {&ts, &temp};
multimapAppend(&timeseries, reading);  // Append (sorted by time)

// Query range
databox start = databoxNewUnsigned(startTime);
multimapIterator iter;
multimapIteratorInitAt(timeseries, &iter, true, &start);

databox timestamp, temperature;
databox *row[2] = {&timestamp, &temperature};
while (multimapIteratorNext(&iter, row)) {
    printf("%lu: %.2f\n", timestamp.data.u64, temperature.data.d64);
}
```

### Pattern: Sparse Integer Set

```c
// For range 0 to billions
multiroar *activeUsers = multiroarBitNew();

// Add
multiroarBitSet(activeUsers, 12345678);
multiroarBitSet(activeUsers, 98765432);

// Check
if (multiroarBitGet(activeUsers, 12345678)) {
    printf("User is active\n");
}

// Remove
multiroarRemove(activeUsers, 12345678);
```

### Pattern: Configuration Storage

```c
flex *config = flexNew();

// Store mixed-type config
typedef struct {
    const char *key;
    databox value;
} ConfigEntry;

ConfigEntry settings[] = {
    {"port", databoxNewUnsigned(8080)},
    {"host", databoxNewBytesString("localhost")},
    {"timeout", databoxNewReal(30.5)},
    {"debug", databoxBool(true)},
};

for (size_t i = 0; i < 4; i++) {
    databox keyBox = databoxNewBytesString(settings[i].key);
    const databox *pair[2] = {&keyBox, &settings[i].value};
    flexAppendMultiple(&config, 2, pair);
}

// Lookup
flexEntry *fe = flexHead(config);
while (fe) {
    databox key, val;
    flexGetByType(fe, &key);
    fe = flexNext(config, fe);
    flexGetByType(fe, &val);

    printf("%.*s = ", (int)key.len, databoxBytes(&key));
    databoxRepr(&val);
    printf("\n");

    fe = flexNext(config, fe);
}
```

---

## Performance Tips

1. **Use appropriate container for access pattern**
   - Random access → multiarray, flex with index
   - Sorted iteration → multimap, intset
   - LRU eviction → multilru
   - Sparse sets → multiroar

2. **Batch operations when possible**
   ```c
   // Open once, many ops, close once (mflex)
   flex *f = mflexOpen(m, state);
   for (int i = 0; i < 1000; i++) {
       flexPushSigned(&f, i, FLEX_ENDPOINT_TAIL);
   }
   mflexCloseGrow(&m, state, f);
   ```

3. **Use middle hints for sorted operations**
   ```c
   flexEntry *middle = NULL;
   for (int i = 0; i < 1000; i++) {
       databox box = databoxNewSigned(i);
       flexInsertByTypeSortedWithMiddle(&f, &box, &middle);
   }
   ```

4. **Disable resizing during bulk inserts**
   ```c
   multimapResizeDisable(m);
   // ... bulk inserts ...
   multimapResizeEnable(m);
   multimapResize(m);
   ```

5. **Use jebuf to avoid wasted memory**
   ```c
   size_t allocSize = jebufSizeAllocation(requestedSize);
   void *ptr = malloc(allocSize);  // Uses full jemalloc allocation
   ```

6. **Use fibbuf for efficient growth**
   ```c
   size_t newSize = fibbufNextSizeBuffer(currentSize);
   ```

7. **Combine fibbuf + jebuf for optimal allocation**
   ```c
   size_t fibSize = fibbufNextSizeBuffer(currentSize);
   size_t allocSize = jebufSizeAllocation(fibSize);
   ptr = realloc(ptr, allocSize);
   ```

---

## Module Reference

- [databox](modules/core/DATABOX.md) - Universal 16-byte container
- [multimap](modules/multimap/MULTIMAP.md) - Sorted key-value store
- [multilist](modules/multilist/MULTILIST.md) - Linked list with packed nodes
- [multiarray](modules/multiarray/MULTIARRAY.md) - Dynamic array
- [flex](modules/flex/FLEX.md) - Compact variable-length array
- [mflex](modules/flex/MFLEX.md) - Auto-compressed flex
- [intset](modules/intset/INTSET.md) - Variable-width integer set
- [multidict](modules/multi/MULTIDICT.md) - Generic hash table
- [multilru](modules/multi/MULTILRU.md) - Multi-level LRU cache
- [multiroar](modules/multi/MULTIROAR.md) - Roaring bitmap
- [str](modules/string/STR.md) - String utilities
- [fibbuf](modules/memory/FIBBUF.md) - Fibonacci growth
- [jebuf](modules/memory/JEBUF.md) - Jemalloc size classes
