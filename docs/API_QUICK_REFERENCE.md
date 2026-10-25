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
14. [Prefix Sums & Range Queries](#prefix-sums--range-queries)
15. [Errors & Diagnostics](#errors--diagnostics)

---

## Container Creation

### "I want to create a map/dictionary"

**multimap** (sorted key-value, auto-scaling):

```c
multimap *m = multimapNew(2);  // 2 = key+value
```

**multidict** (hash table with custom storage):

```c
/* multidictNew(multidictType *type, multidictClass *qdc, int32_t seed) */
multidictClass *storage = /* your storage class, or NULL for default */;
multidict *d = multidictNew(&multidictTypeExactKey, storage, 0);
```

**multiroar** (sparse bitmap/set):

```c
multiroar *r = multiroarBitNew();  // For set membership
```

### "I want to create a list"

**multilist** (linked list of packed arrays):

```c
multilist *ml = multilistNewLimit(FLEX_CAP_LEVEL_2048, 0);
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
/* multiarrayNew(uint16_t elementLen, uint16_t rowMax):
 * elementLen is the size of one element, rowMax the elements per row
 * before the container splits. Neither is a total capacity. */
multiarray *arr = multiarrayNew(sizeof(int64_t), 512);
```

**Native inline array** (macro over a caller-owned holder):

```c
MyStruct *holder = NULL;
multiarray *mar = multiarrayNativeNew(holder);
```

### "I want to create a set"

**intset** (variable-width integer set):

```c
intset *is = intsetNew();  // Starts with 16-bit encoding
```

**multimap as set** (sorted, deduplicated):

```c
multimap *set = multimapSetNew(1);  // 1 element per entry
```

Set mode dedupes on the WHOLE ROW, not on the key: with
`elementsPerEntry > 1` two entries sharing a key but differing in any
later element both survive. Normal (non-set) maps compare the key only
and replace the values on a match.

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

**dks** (dynamic string buffer): two variants generated from the same
template. `mds` is the full form, `mdsc` the compact one. Both are
`char *` typedefs, so they print with `%s`:

```c
mds *s = mdsnew("hello");   /* or mdsempty() */
mdsc *c = mdscnew("hello"); /* compact twin, same call shape */
```

---

## Container Destruction

### "I want to free a container"

```c
multimapFree(m);        // multimap
multidictFree(d);       // multidict
multilistFree(ml);      // multilist
multiarrayFree(arr);    // multiarray
flexFree(f);            // flex
mflexFree(mf);          // mflex
intsetFree(is);         // intset
multilruFree(lru);      // multilru
multiroarFree(r);       // multiroar

databoxFree(&box);      // databox (if allocated)
mdsfree(s);             // mds string  (mdscfree for mdsc)
mflexStateFree(state);  // mflex state
```

### "I want to clear and reuse"

```c
multimapReset(&m);      // multimap (back to an empty Small)
multiOrderedSetReset(&mos);
multilistReset(&ml);
multiarrayReset(&ma);
intsetReset(&is);
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
multiarrayInsert(&arr, 0, &value);  // copies elementLen bytes from 'what'
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

**multilru**: `multilruRemoveMinimum` is S4LRU, so it does NOT always
evict. An entry above level 0 is DEMOTED to level-1 as a second chance;
only a level-0 entry is truly evicted. Ask
`multilruIsPopulated(lru, out)` which happened before freeing anything
the entry owns.

```c
multilruPtr out;
if (multilruRemoveMinimum(lru, &out)) {
    if (multilruIsPopulated(lru, out)) {
        // demoted — the entry is still in the cache; do NOT free it
    } else {
        // truly evicted — the handle is dead, release your resource
    }
}
```

For unconditional bulk eviction, use the bulk entry points:

```c
multilruPtr gone[64];
size_t n = multilruEvictN(lru, gone, 64);            // count actually evicted
size_t m = multilruEvictToSize(lru, target, gone, 64); // evict to a weight
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

**multilist (forward)**: the iterator init macros take an array of TWO
`mflexState *`, not a single state.

```c
mflexState *states[2] = {mflexStateCreate(), mflexStateCreate()};

multilistIterator iter;
multilistIteratorInitForward(ml, states, &iter);

multilistEntry entry;
while (multilistNext(&iter, &entry)) {
    // Process entry
}
multilistIteratorRelease(&iter);
```

**multilist (reverse)**:

```c
multilistIteratorInitReverse(ml, states, &iter);
while (multilistNext(&iter, &entry)) {
    // Process in reverse
}
multilistIteratorRelease(&iter);
```

**flex (forward)**: seed the loop with `flexHeadIfAny`, never
`flexHead`. `flexHead` returns an insertion POSITION and stays non-NULL
on an empty flex, so a `flexHead`-seeded loop reads one entry past the
buffer before `flexNext` can apply its end check.

```c
for (flexEntry *fe = flexHeadIfAny(f); fe; fe = flexNext(f, fe)) {
    databox box;
    flexGetByType(fe, &box);
    // Process box
}
```

**flex (reverse)**:

```c
for (flexEntry *fe = flexTail(f); fe; fe = flexPrev(f, fe)) {
    databox box;
    flexGetByType(fe, &box);
    // Process box
}
```

### "I want to iterate an array"

**multiarray**: walk node by node with the iterator (a `multiarrayGet`
loop re-walks nodes from the front on Medium/Large arrays):

```c
multiarrayIter it;
multiarrayIteratorInit(arr, &it, true); /* false: last to first */
int64_t *elem;
while ((elem = multiarrayIteratorNext(&it))) {
    // Process elem (in place; don't change the array during the walk)
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

**multilist** (`multilistIndexGet` takes a single `mflexState *`,
unlike the iterator macros):

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
// multimap — fills a FULL-WIDTH box array, plus the entry handle
databox key, val;
databox *found[2] = {&key, &val};
multimapEntry me;
multimapRandomValue(m, false /* fromTail */, found, &me);

// multidict
databox randomKey;
multidictGetRandomKey(d, &randomKey);

// intset
int64_t random = intsetRandom(is);

// multilru (random from LRU)
multilruPtr candidates[10];
size_t got = multilruCollectLowest(lru, candidates, 10);  // up to 10 LRU candidates
```

---

## Metadata & Size

### "I want to know the size"

```c
// Count elements
size_t count = multimapCount(m);
size_t count = multidictSize(d);      // macro for multidictCount()
size_t count = multilistCount(ml);
size_t count = flexCount(f);
size_t count = mflexCount(mf);
size_t count = intsetCount(is);
size_t count = multilruCount(lru);

// Memory bytes
size_t bytes = multimapBytes(m);
size_t bytes = multilistBytes(ml);
size_t bytes = flexBytes(f);
size_t bytes = intsetBytes(is);
size_t bytes = multilruBytes(lru);
```

`multiarrayCount(arr)` and `multiarrayBytes(arr)` cover the container API;
the Native macro API tracks its count externally.

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
// multimap — choose the per-map flex size limit up front rather than
// preallocating a count; the container retiers itself as it grows
multimap *m = multimapNewLimit(2, FLEX_CAP_LEVEL_2048);

// multiarray — rowMax is the per-row element budget before splitting
multiarray *arr = multiarrayNew(sizeof(int64_t), 512);

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
    printf("Value: %" PRIu64 "\n", value);  // <inttypes.h>
}
```

**LuaJIT-style scanning**:

```c
databox result;
StrScanFmt fmt = StrScanScan("42", &result, STRSCAN_OPT_TOINT, false, false);
if (fmt == STRSCAN_INT) {
    printf("Integer: %" PRId64 "\n", result.data.i64);
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

Every `mds` mutator returns a possibly-new pointer, so always reassign.
The buffer IS the string — no `.buf` member, no wrapper struct.

```c
mds *s = mdsempty();

s = mdscat(s, "Hello");
s = mdscat(s, " ");
s = mdscatlen(s, "World", 5);

printf("%s (%zu bytes)\n", s, mdslen(s));  // "Hello World (11 bytes)"

mdsfree(s);
```

Swap the `mds` prefix for `mdsc` to use the compact variant
(`mdscempty`, `mdsccat`, `mdsclen`, `mdscfree`).

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
multiroar *diff = multiroarNewAndNot(a, b);

// XOR (symmetric difference)
multiroar *sym = multiroarNewXor(a, b);
```

Use `multiroarNewAndNot` for difference. `multiroarNewNot` complements
only the chunks a bitmap has already materialized, so
NOT-then-AND is not set difference — the answer depends on which
chunks happen to exist.

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
uint64_t setBits = multiroarBitCount(r);

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
    // Evict if full. RemoveMinimum demotes above level 0, so check
    // whether the handle actually died before dropping the payload.
    if (multilruCount(c->lru) >= MAX_SIZE) {
        multilruPtr out;
        if (multilruRemoveMinimum(c->lru, &out) &&
            !multilruIsPopulated(c->lru, out)) {
            // Truly evicted: remove the matching entry from c->data
        }
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
    printf("%" PRIu64 ": %.2f\n", timestamp.data.u64, temperature.data.d64);
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
flexEntry *fe = flexHeadIfAny(config);
while (fe) {
    databox key, val;
    flexGetByType(fe, &key);

    fe = flexNext(config, fe);
    if (!fe) {
        break;  // odd element count; nothing to pair with
    }
    flexGetByType(fe, &val);

    printf("%.*s = ", (int)databoxLen(&key), (const char *)databoxCBytes(&key));
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

4. **Append instead of insert when the input is already sorted**

   ```c
   /* multimapAppend skips the sorted-position search that
    * multimapInsert performs on every entry. */
   multimapAppend(&m, entry);
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

## Prefix Sums & Range Queries

### "I want running totals or counts by position"

```c
void *fw = NULL;                        /* fenwickI64; also I16..U128, Double */
fenwickI64Update(&fw, idx, delta);      /* e[idx] += delta */
int64_t upTo = fenwickI64Query(fw, idx);          /* e[0..idx] */
int64_t span = fenwickI64RangeQuery(fw, lo, hi);  /* e[lo..hi] */
fenwickI64Free(fw);
```

### "I want rank / select (first position reaching a total)"

```c
size_t pos = fenwickI64LowerBound(fw, target);  /* SIZE_MAX: none */
```

### "I want exact totals over mixed ints and doubles"

```c
multiFenwickFlat *mf = NULL;
const databox amount = DATABOX_DOUBLE(2.5);   /* or DATABOX_SIGNED(n), ... */
multiFenwickFlatUpdate(&mf, idx, &amount);
databox total;
multiFenwickFlatQuery(mf, idx, &total);  /* typed by value */
```

### "I want rectangle sums on a grid"

```c
fenwick2dI64 *g = NULL;
fenwick2dI64Update(&g, row, col, 1);
int64_t inBox = fenwick2dI64RangeQuery(g, r1, c1, r2, c2);
```

### "I want range minimum / maximum"

```c
void *seg = segmentI64New(SEGMENT_OP_MIN);
segmentI64Update(&seg, idx, value);              /* assigns */
int64_t lo = segmentI64Query(seg, left, right);
```

Which tree for which job, and measured speed and memory:
[FENWICK.md](modules/fenwick/FENWICK.md), [SEGMENT.md](modules/fenwick/SEGMENT.md).

---

## Errors & Diagnostics

datakit never prints. A refused misuse, a runtime condition or a fatal misuse is recorded per thread
(details: [DIAGNOSTICS.md](modules/core/DIAGNOSTICS.md)).

### "A call returned false / NULL and I want to know why"

```c
const uint64_t before = dkDiagCount();
multiroar *u = multiroarNewOr(a, b);
dkDiagnostic d;
if (!u && dkDiagCount() != before && dkDiagLast(&d)) {
    fprintf(stderr, "%s: %s\n", dkDiagCodeName(d.code), d.message);
}
```

### "I want datakit's diagnostics in my logs"

```c
static void onDiag(const dkDiagnostic *d, void *ctx) { /* log d */ }
static const dkDiagHook hook = {.fn = onDiag, .ctx = NULL};
dkDiagSetHook(&hook);
```

### "I want them on stderr while debugging (or never)"

```c
dkDiagSetPrintMode(DK_DIAG_PRINT_ALWAYS); /* or DK_DIAG_PRINT_NEVER */
```

### "Is there data at this persist path?"

```c
switch (persistCtxProbe(basePath)) {
case PERSIST_CTX_PRESENT:    /* an Open recovers it */ break;
case PERSIST_CTX_ORPHAN_WAL: /* a WAL without its snapshot */ break;
case PERSIST_CTX_ABSENT:     break;
}
```

---

## Module Reference

- [databox](modules/core/DATABOX.md) - Universal 16-byte container
- [diagnostics](modules/core/DIAGNOSTICS.md) - Misuse and condition reporting (dkDiag)
- [multimap](modules/multimap/MULTIMAP.md) - Sorted key-value store
- [multilist](modules/multilist/MULTILIST.md) - Linked list with packed nodes
- [multiarray](modules/multiarray/MULTIARRAY.md) - Dynamic array
- [flex](modules/flex/FLEX.md) - Compact variable-length array
- [mflex](modules/flex/MFLEX.md) - Auto-compressed flex
- [intset](modules/intset/INTSET.md) - Variable-width integer set
- [multidict](modules/multi/MULTIDICT.md) - Generic hash table
- [multilru](modules/multi/MULTILRU.md) - Multi-level LRU cache
- [multiroar](modules/multi/MULTIROAR.md) - Roaring bitmap
- [Fenwick trees](modules/fenwick/FENWICK.md) - Prefix sums, rank, 2D grids
- [Segment trees](modules/fenwick/SEGMENT.md) - Range sum/min/max
- [str](modules/string/STR.md) - String utilities
- [fibbuf](modules/memory/FIBBUF.md) - Fibonacci growth
- [jebuf](modules/memory/JEBUF.md) - Jemalloc size classes
