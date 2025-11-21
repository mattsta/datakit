# Migration Guide

This guide helps you migrate from other data structure libraries to datakit, with side-by-side comparisons and conversion examples.

## Table of Contents

1. [From Redis Data Structures](#from-redis-data-structures)
2. [From C++ STL](#from-c-stl)
3. [From glib](#from-glib)
4. [From Berkeley DB](#from-berkeley-db)
5. [From SQLite](#from-sqlite)
6. [From Protocol Buffers](#from-protocol-buffers)
7. [Common Migration Patterns](#common-migration-patterns)
8. [API Translation Table](#api-translation-table)

---

## From Redis Data Structures

datakit originated from Redis internals and shares similar design philosophies.

### Redis String → dks String Buffer

**Redis (C API):**

```c
/* Redis SDS (Simple Dynamic String) */
sds s = sdsnew("hello");
s = sdscat(s, " world");
printf("%s (len=%zu)\n", s, sdslen(s));
sdsfree(s);
```

**datakit:**

```c
/* dks string buffer */
dksstr *s = dksstr_new();
dksstr_cat(s, "hello");
dksstr_cat(s, " world");
printf("%s (len=%zu)\n", s->buf, dksstr_len(s));
dksstr_free(s);
```

### Redis List → multilist

**Redis:**

```c
/* Redis listpack-based list */
list *l = listCreate();
listAddNodeTail(l, mydata);
listAddNodeHead(l, mydata);

listIter *iter = listGetIterator(l, AL_START_HEAD);
listNode *node;
while ((node = listNext(iter)) != NULL) {
    void *value = listNodeValue(node);
    /* process value */
}
listReleaseIterator(iter);
listRelease(l);
```

**datakit:**

```c
/* multilist (similar to Redis's list implementation) */
multilist *ml = multilistNew(FLEX_CAP_LEVEL_2048, 0);
mflexState *state = mflexStateCreate();

databox item1 = databoxNewBytesString("data");
multilistPushByTypeTail(&ml, state, &item1);

databox item2 = databoxNewBytesString("data");
multilistPushByTypeHead(&ml, state, &item2);

multimapIterator iter;
multilistIteratorInitForward(ml, state, &iter);

multilistEntry entry;
while (multilistNext(&iter, &entry)) {
    /* Process entry.box */
}

multilistIteratorRelease(&iter);
mflexStateFree(state);
multilistFree(ml);
```

### Redis Hash → multimap

**Redis:**

```c
/* Redis hash */
dict *d = dictCreate(&hashDictType, NULL);
dictAdd(d, "key1", "value1");
dictAdd(d, "key2", "value2");

dictEntry *de = dictFind(d, "key1");
if (de) {
    char *value = dictGetVal(de);
    printf("Found: %s\n", value);
}

dictIterator *iter = dictGetIterator(d);
while ((de = dictNext(iter)) != NULL) {
    char *key = dictGetKey(de);
    char *val = dictGetVal(de);
    printf("%s: %s\n", key, val);
}
dictReleaseIterator(iter);
dictRelease(d);
```

**datakit:**

```c
/* multimap */
multimap *m = multimapNew(2);  /* key + value */

databox k1 = databoxNewBytesString("key1");
databox v1 = databoxNewBytesString("value1");
const databox *e1[2] = {&k1, &v1};
multimapInsert(&m, e1);

databox k2 = databoxNewBytesString("key2");
databox v2 = databoxNewBytesString("value2");
const databox *e2[2] = {&k2, &v2};
multimapInsert(&m, e2);

/* Lookup */
databox searchKey = databoxNewBytesString("key1");
databox value;
databox *results[1] = {&value};

if (multimapLookup(m, &searchKey, results)) {
    printf("Found: %.*s\n", (int)databoxLen(&value),
           databoxCBytes(&value));
}

/* Iterate */
multimapIterator iter;
multimapIteratorInit(m, &iter, true);

databox key, val;
databox *elements[2] = {&key, &val};

while (multimapIteratorNext(&iter, elements)) {
    printf("%.*s: %.*s\n",
           (int)databoxLen(&key), databoxCBytes(&key),
           (int)databoxLen(&val), databoxCBytes(&val));
}

multimapFree(m);
```

### Redis Sorted Set → multimap with Score Key

**Redis:**

```c
/* Redis sorted set (zset) */
zset *zs = zsetCreate();
zsetAdd(zs, 95.5, "alice");
zsetAdd(zs, 87.3, "bob");
zsetAdd(zs, 92.1, "charlie");

/* Get by score range */
zrangespec range = {.min = 90.0, .max = 100.0};
unsigned long count;
dictEntry **entries = zsetRange(zs, &range, &count);

for (unsigned long i = 0; i < count; i++) {
    char *member = dictGetKey(entries[i]);
    double score = *(double *)dictGetVal(entries[i]);
    printf("%s: %.2f\n", member, score);
}
```

**datakit:**

```c
/* multimap with score as key (automatically sorted) */
multimap *scores = multimapNew(2);  /* score + name */

databox s1 = databoxNewReal(95.5);
databox n1 = databoxNewBytesString("alice");
const databox *e1[2] = {&s1, &n1};
multimapInsert(&scores, e1);

databox s2 = databoxNewReal(87.3);
databox n2 = databoxNewBytesString("bob");
const databox *e2[2] = {&s2, &n2};
multimapInsert(&scores, e2);

databox s3 = databoxNewReal(92.1);
databox n3 = databoxNewBytesString("charlie");
const databox *e3[2] = {&s3, &n3};
multimapInsert(&scores, e3);

/* Range query [90.0, 100.0] */
databox startScore = databoxNewReal(90.0);

multimapIterator iter;
multimapIteratorInitAt(scores, &iter, true, &startScore);

databox score, name;
databox *elements[2] = {&score, &name};

while (multimapIteratorNext(&iter, elements)) {
    if (score.data.d64 > 100.0) break;

    printf("%.*s: %.2f\n",
           (int)databoxLen(&name), databoxCBytes(&name),
           score.data.d64);
}

multimapFree(scores);
```

### Redis HyperLogLog → hyperloglog

**Redis:**

```c
/* Redis HyperLogLog */
struct hllhdr *hll = createHLLObject();
hllAdd(hll, "user_123", 8);
hllAdd(hll, "user_456", 8);
hllAdd(hll, "user_789", 8);

uint64_t cardinality = hllCount(hll);
printf("Estimated users: %lu\n", cardinality);

freeHLLObject(hll);
```

**datakit:**

```c
/* hyperloglog (same algorithm as Redis) */
hyperloglog *hll = hyperloglogCreate();

hyperloglogAdd(hll, (uint8_t *)"user_123", 8);
hyperloglogAdd(hll, (uint8_t *)"user_456", 8);
hyperloglogAdd(hll, (uint8_t *)"user_789", 8);

uint64_t cardinality = hyperloglogCount(hll);
printf("Estimated users: %lu\n", cardinality);

hyperloglogFree(hll);
```

### Redis Intset → intset

**Redis:**

```c
/* Redis intset */
intset *is = intsetNew();
uint8_t success;

is = intsetAdd(is, 42, &success);
is = intsetAdd(is, 100, &success);
is = intsetAdd(is, -50, &success);

if (intsetFind(is, 42)) {
    printf("Found 42\n");
}

printf("Count: %u\n", intsetLen(is));

zfree(is);
```

**datakit:**

```c
/* intset (compatible API) */
intset *is = intsetNew();
bool success;

intsetAdd(&is, 42, &success);
intsetAdd(&is, 100, &success);
intsetAdd(&is, -50, &success);

if (intsetFind(is, 42)) {
    printf("Found 42\n");
}

printf("Count: %zu\n", intsetCount(is));

intsetFree(is);
```

---

## From C++ STL

### std::vector → multiarray

**C++ STL:**

```cpp
/* std::vector */
std::vector<int> vec;
vec.reserve(1000);

for (int i = 0; i < 1000; i++) {
    vec.push_back(i);
}

for (int i = 0; i < vec.size(); i++) {
    std::cout << vec[i] << " ";
}

vec.clear();
```

**datakit:**

```c
/* multiarray */
multiarray *arr = multiarrayNew(sizeof(int), 1000);

for (int i = 0; i < 1000; i++) {
    multiarrayInsertAfter(&arr, i > 0 ? i - 1 : 0, &i);
}

for (size_t i = 0; i < multiarrayCount(arr); i++) {
    int *val = (int *)multiarrayGet(arr, i);
    printf("%d ", *val);
}

/* multiarray freed by variant-specific free */
```

### std::list → multilist

**C++ STL:**

```cpp
/* std::list */
std::list<std::string> lst;

lst.push_back("first");
lst.push_front("zeroth");
lst.push_back("second");

for (const auto& item : lst) {
    std::cout << item << std::endl;
}

std::string front = lst.front();
lst.pop_front();

lst.clear();
```

**datakit:**

```c
/* multilist */
multilist *ml = multilistNew(FLEX_CAP_LEVEL_2048, 0);
mflexState *state = mflexStateCreate();

databox first = databoxNewBytesString("first");
multilistPushByTypeTail(&ml, state, &first);

databox zeroth = databoxNewBytesString("zeroth");
multilistPushByTypeHead(&ml, state, &zeroth);

databox second = databoxNewBytesString("second");
multilistPushByTypeTail(&ml, state, &second);

/* Iterate */
multimapIterator iter;
multilistIteratorInitForward(ml, state, &iter);

multilistEntry entry;
while (multilistNext(&iter, &entry)) {
    printf("%.*s\n", (int)entry.box.len,
           entry.box.data.bytes.cstart);
}

multilistIteratorRelease(&iter);

/* Pop front */
databox front;
if (multilistPopHead(&ml, state, &front)) {
    printf("Front: %.*s\n", (int)front.len,
           front.data.bytes.cstart);
    databoxFreeData(&front);
}

mflexStateFree(state);
multilistFree(ml);
```

### std::map → multimap

**C++ STL:**

```cpp
/* std::map (sorted) */
std::map<std::string, int> m;

m["alice"] = 95;
m["bob"] = 87;
m["charlie"] = 92;

auto it = m.find("alice");
if (it != m.end()) {
    std::cout << "alice: " << it->second << std::endl;
}

for (const auto& [key, value] : m) {
    std::cout << key << ": " << value << std::endl;
}

m.clear();
```

**datakit:**

```c
/* multimap (sorted) */
multimap *m = multimapNew(2);

databox k1 = databoxNewBytesString("alice");
databox v1 = databoxNewSigned(95);
const databox *e1[2] = {&k1, &v1};
multimapInsert(&m, e1);

databox k2 = databoxNewBytesString("bob");
databox v2 = databoxNewSigned(87);
const databox *e2[2] = {&k2, &v2};
multimapInsert(&m, e2);

databox k3 = databoxNewBytesString("charlie");
databox v3 = databoxNewSigned(92);
const databox *e3[2] = {&k3, &v3};
multimapInsert(&m, e3);

/* Find */
databox searchKey = databoxNewBytesString("alice");
databox value;
databox *results[1] = {&value};

if (multimapLookup(m, &searchKey, results)) {
    printf("alice: %ld\n", value.data.i64);
}

/* Iterate (sorted by key) */
multimapIterator iter;
multimapIteratorInit(m, &iter, true);

databox key, val;
databox *elements[2] = {&key, &val};

while (multimapIteratorNext(&iter, elements)) {
    printf("%.*s: %ld\n",
           (int)databoxLen(&key), databoxCBytes(&key),
           val.data.i64);
}

multimapFree(m);
```

### std::unordered_map → multidict

**C++ STL:**

```cpp
/* std::unordered_map (hash table) */
std::unordered_map<std::string, int> um;

um["key1"] = 100;
um["key2"] = 200;
um["key3"] = 300;

auto it = um.find("key2");
if (it != um.end()) {
    std::cout << "Found: " << it->second << std::endl;
}

um.erase("key1");

for (const auto& [key, value] : um) {
    std::cout << key << ": " << value << std::endl;
}

um.clear();
```

**datakit:**

```c
/* multidict (hash table) */
multidictClass *class = /* your class */;
multidict *d = multidictNew(&multidictTypeExactKey, class, 0);

databox k1 = databoxNewBytesString("key1");
databox v1 = databoxNewSigned(100);
multidictAdd(d, &k1, &v1);

databox k2 = databoxNewBytesString("key2");
databox v2 = databoxNewSigned(200);
multidictAdd(d, &k2, &v2);

databox k3 = databoxNewBytesString("key3");
databox v3 = databoxNewSigned(300);
multidictAdd(d, &k3, &v3);

/* Find */
databox searchKey = databoxNewBytesString("key2");
databox foundValue;

if (multidictFind(d, &searchKey, &foundValue)) {
    printf("Found: %ld\n", foundValue.data.i64);
}

/* Delete */
databox delKey = databoxNewBytesString("key1");
multidictDelete(d, &delKey);

/* Iterate */
multidictIterator iter;
multidictIteratorGetSafe(d, &iter);

multidictEntry entry;
while (multidictIteratorNext(&iter, &entry)) {
    printf("%.*s: %ld\n",
           (int)databoxLen(&entry.key), databoxCBytes(&entry.key),
           entry.val.data.i64);
}

multidictIteratorRelease(&iter);
multidictFree(d);
```

### std::set → intset or multimap

**C++ STL:**

```cpp
/* std::set<int> */
std::set<int> s;

s.insert(42);
s.insert(100);
s.insert(7);

if (s.find(42) != s.end()) {
    std::cout << "Found 42" << std::endl;
}

for (int val : s) {
    std::cout << val << " ";  /* Sorted: 7 42 100 */
}

s.erase(42);
s.clear();
```

**datakit (using intset):**

```c
/* intset (for integers) */
intset *is = intsetNew();

intsetAdd(&is, 42, NULL);
intsetAdd(&is, 100, NULL);
intsetAdd(&is, 7, NULL);

if (intsetFind(is, 42)) {
    printf("Found 42\n");
}

/* Iterate (sorted) */
for (uint32_t i = 0; i < intsetCount(is); i++) {
    int64_t val;
    intsetGet(is, i, &val);
    printf("%ld ", val);  /* Sorted: 7 42 100 */
}

bool removed;
intsetRemove(&is, 42, &removed);

intsetFree(is);
```

**datakit (using multimap for non-integers):**

```c
/* multimap as set (single element) */
multimap *set = multimapSetNew(1);

databox v1 = databoxNewBytesString("apple");
const databox *e1[1] = {&v1};
multimapInsert(&set, e1);

databox v2 = databoxNewBytesString("banana");
const databox *e2[1] = {&v2};
multimapInsert(&set, e2);

databox searchVal = databoxNewBytesString("apple");
if (multimapExists(set, &searchVal)) {
    printf("Found apple\n");
}

multimapFree(set);
```

---

## From glib

### GHashTable → multimap or multidict

**glib:**

```c
/* GHashTable */
GHashTable *hash = g_hash_table_new(g_str_hash, g_str_equal);

g_hash_table_insert(hash, "key1", "value1");
g_hash_table_insert(hash, "key2", "value2");

gpointer value = g_hash_table_lookup(hash, "key1");
if (value) {
    printf("Found: %s\n", (char *)value);
}

g_hash_table_destroy(hash);
```

**datakit:**

```c
/* multimap */
multimap *m = multimapNew(2);

databox k1 = databoxNewBytesString("key1");
databox v1 = databoxNewBytesString("value1");
const databox *e1[2] = {&k1, &v1};
multimapInsert(&m, e1);

databox k2 = databoxNewBytesString("key2");
databox v2 = databoxNewBytesString("value2");
const databox *e2[2] = {&k2, &v2};
multimapInsert(&m, e2);

databox searchKey = databoxNewBytesString("key1");
databox value;
databox *results[1] = {&value};

if (multimapLookup(m, &searchKey, results)) {
    printf("Found: %.*s\n", (int)databoxLen(&value),
           databoxCBytes(&value));
}

multimapFree(m);
```

### GList → multilist

**glib:**

```c
/* GList */
GList *list = NULL;

list = g_list_append(list, "first");
list = g_list_append(list, "second");
list = g_list_prepend(list, "zeroth");

for (GList *l = list; l != NULL; l = l->next) {
    printf("%s\n", (char *)l->data);
}

g_list_free(list);
```

**datakit:**

```c
/* multilist */
multilist *ml = multilistNew(FLEX_CAP_LEVEL_2048, 0);
mflexState *state = mflexStateCreate();

databox first = databoxNewBytesString("first");
multilistPushByTypeTail(&ml, state, &first);

databox second = databoxNewBytesString("second");
multilistPushByTypeTail(&ml, state, &second);

databox zeroth = databoxNewBytesString("zeroth");
multilistPushByTypeHead(&ml, state, &zeroth);

multimapIterator iter;
multilistIteratorInitForward(ml, state, &iter);

multilistEntry entry;
while (multilistNext(&iter, &entry)) {
    printf("%.*s\n", (int)entry.box.len,
           entry.box.data.bytes.cstart);
}

multilistIteratorRelease(&iter);
mflexStateFree(state);
multilistFree(ml);
```

### GArray → multiarray

**glib:**

```c
/* GArray */
GArray *arr = g_array_new(FALSE, FALSE, sizeof(int));

for (int i = 0; i < 100; i++) {
    g_array_append_val(arr, i);
}

for (int i = 0; i < arr->len; i++) {
    int val = g_array_index(arr, int, i);
    printf("%d ", val);
}

g_array_free(arr, TRUE);
```

**datakit:**

```c
/* multiarray */
multiarray *arr = multiarrayNew(sizeof(int), 100);

for (int i = 0; i < 100; i++) {
    multiarrayInsertAfter(&arr, i > 0 ? i - 1 : 0, &i);
}

for (size_t i = 0; i < multiarrayCount(arr); i++) {
    int *val = (int *)multiarrayGet(arr, i);
    printf("%d ", *val);
}

/* Free based on variant */
```

---

## From Berkeley DB

### DB Hash → multimap

**Berkeley DB:**

```c
/* Berkeley DB Hash */
DB *dbp;
db_create(&dbp, NULL, 0);
dbp->open(dbp, NULL, "mydb.db", NULL, DB_HASH, DB_CREATE, 0664);

DBT key, data;
memset(&key, 0, sizeof(DBT));
memset(&data, 0, sizeof(DBT));

key.data = "key1";
key.size = strlen("key1") + 1;
data.data = "value1";
data.size = strlen("value1") + 1;

dbp->put(dbp, NULL, &key, &data, 0);

/* Retrieve */
dbp->get(dbp, NULL, &key, &data, 0);
printf("Retrieved: %s\n", (char *)data.data);

dbp->close(dbp, 0);
```

**datakit:**

```c
/* multimap (in-memory, but can be serialized) */
multimap *m = multimapNew(2);

databox k = databoxNewBytesString("key1");
databox v = databoxNewBytesString("value1");
const databox *e[2] = {&k, &v};
multimapInsert(&m, e);

/* Retrieve */
databox searchKey = databoxNewBytesString("key1");
databox value;
databox *results[1] = {&value};

if (multimapLookup(m, &searchKey, results)) {
    printf("Retrieved: %.*s\n", (int)databoxLen(&value),
           databoxCBytes(&value));
}

multimapFree(m);
```

---

## From SQLite

### Simple Table → multimap

**SQLite:**

```c
/* SQLite */
sqlite3 *db;
sqlite3_open("test.db", &db);

char *sql = "CREATE TABLE users (id INT, name TEXT, score REAL);";
sqlite3_exec(db, sql, NULL, NULL, NULL);

sql = "INSERT INTO users VALUES (1, 'alice', 95.5);";
sqlite3_exec(db, sql, NULL, NULL, NULL);

sql = "INSERT INTO users VALUES (2, 'bob', 87.3);";
sqlite3_exec(db, sql, NULL, NULL, NULL);

/* Query */
sqlite3_stmt *stmt;
sql = "SELECT name, score FROM users WHERE id = 1;";
sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);

if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(stmt, 0);
    double score = sqlite3_column_double(stmt, 1);
    printf("%s: %.2f\n", name, score);
}

sqlite3_finalize(stmt);
sqlite3_close(db);
```

**datakit:**

```c
/* multimap (for in-memory structured data) */
multimap *users = multimapNew(4);  /* id + name + score */

/* Insert */
databox id1 = databoxNewSigned(1);
databox name1 = databoxNewBytesString("alice");
databox score1 = databoxNewReal(95.5);
const databox *user1[3] = {&id1, &name1, &score1};
multimapInsert(&users, user1);

databox id2 = databoxNewSigned(2);
databox name2 = databoxNewBytesString("bob");
databox score2 = databoxNewReal(87.3);
const databox *user2[3] = {&id2, &name2, &score2};
multimapInsert(&users, user2);

/* Query by id */
databox searchId = databoxNewSigned(1);
databox name, score;
databox *results[2] = {&name, &score};

if (multimapLookup(users, &searchId, results)) {
    printf("%.*s: %.2f\n",
           (int)databoxLen(&name), databoxCBytes(&name),
           score.data.d64);
}

multimapFree(users);
```

---

## From Protocol Buffers

### Message Fields → multimap or flex

**Protocol Buffers:**

```protobuf
message User {
  int64 id = 1;
  string name = 2;
  repeated string tags = 3;
  map<string, int32> scores = 4;
}
```

**datakit:**

```c
/* Option 1: multimap for structured data */
typedef struct {
    multimap *users;  /* id → {name, tags_flex, scores_map} */
} UserStore;

UserStore *userStoreNew(void) {
    UserStore *store = malloc(sizeof(*store));
    store->users = multimapNew(4);
    return store;
}

void userStoreAdd(UserStore *store, int64_t id, const char *name,
                 const char **tags, size_t tagCount,
                 multimap *scores) {
    /* Pack tags into flex */
    flex *tagsFlex = NULL;
    for (size_t i = 0; i < tagCount; i++) {
        flexPushBytes(&tagsFlex, tags[i], strlen(tags[i]),
                     FLEX_ENDPOINT_TAIL);
    }

    /* Create entry */
    databox idBox = databoxNewSigned(id);
    databox nameBox = databoxNewBytesString(name);
    databox tagsBox = databoxNewPtr(tagsFlex);
    databox scoresBox = databoxNewPtr(scores);

    const databox *entry[4] = {&idBox, &nameBox, &tagsBox, &scoresBox};
    multimapInsert(&store->users, entry);
}

/* Option 2: Use databox for variant data */
databox message = /* ... */;
flex *fields = flexNew();

/* Encode fields */
databox fieldId = databoxNewSigned(1);
databox fieldValue = databoxNewSigned(12345);
const databox *field[2] = {&fieldId, &fieldValue};
flexAppendMultiple(&fields, 2, field);
```

---

## Common Migration Patterns

### Pattern 1: Error Handling

**Before (errno-based):**

```c
void *ptr = malloc(size);
if (!ptr) {
    perror("malloc failed");
    return -1;
}
```

**After (datakit):**

```c
multimap *m = multimapNew(2);
if (!m) {
    fprintf(stderr, "Failed to create multimap\n");
    return NULL;
}

/* Or check return values */
bool success;
intsetAdd(&is, value, &success);
if (!success) {
    fprintf(stderr, "Failed to add value\n");
}
```

### Pattern 2: Iterator Patterns

**Before (callback-based):**

```c
void processItem(void *item, void *userdata) {
    printf("Item: %s\n", (char *)item);
}

listForEach(list, processItem, NULL);
```

**After (datakit iterators):**

```c
multimapIterator iter;
multilistIteratorInitForward(ml, state, &iter);

multilistEntry entry;
while (multilistNext(&iter, &entry)) {
    printf("Item: %.*s\n", (int)entry.box.len,
           entry.box.data.bytes.cstart);
}

multilistIteratorRelease(&iter);
```

### Pattern 3: Memory Management

**Before (manual ref counting):**

```c
typedef struct {
    int refcount;
    void *data;
} RefCounted;

RefCounted *rc = malloc(sizeof(RefCounted));
rc->refcount = 1;
rc->data = strdup("hello");

/* Increment */
rc->refcount++;

/* Decrement */
rc->refcount--;
if (rc->refcount == 0) {
    free(rc->data);
    free(rc);
}
```

**After (datakit - container owns data):**

```c
/* Containers manage their own memory */
multimap *m = multimapNew(2);

databox key = databoxNewBytesString("key");
databox value = databoxNewBytesString("value");
const databox *entry[2] = {&key, &value};

multimapInsert(&m, entry);  /* Makes internal copy */

/* No need to track references */
multimapFree(m);  /* Frees all data */
```

---

## API Translation Table

### Container Creation

| Other Library         | datakit                              |
| --------------------- | ------------------------------------ |
| `sdsnew()`            | `dksstr_new()`                       |
| `listCreate()`        | `multilistNew()`                     |
| `dictCreate()`        | `multimapNew()` or `multidictNew()`  |
| `std::vector<T>()`    | `multiarrayNew(sizeof(T), capacity)` |
| `std::map<K,V>()`     | `multimapNew(2)`                     |
| `std::set<T>()`       | `intsetNew()` or `multimapSetNew(1)` |
| `g_hash_table_new()`  | `multimapNew(2)`                     |
| `intsetNew()` (Redis) | `intsetNew()`                        |

### Container Operations

| Operation  | Other Library               | datakit                                         |
| ---------- | --------------------------- | ----------------------------------------------- |
| Insert     | `dictAdd(d, k, v)`          | `multimapInsert(&m, elements)`                  |
| Lookup     | `dictFind(d, k)`            | `multimapLookup(m, &key, results)`              |
| Delete     | `dictDelete(d, k)`          | `multimapDelete(&m, &key)`                      |
| Count      | `dictSize(d)`               | `multimapCount(m)`                              |
| Push back  | `vec.push_back(x)`          | `multilistPushByTypeTail(&ml, state, &x)`       |
| Push front | `list.push_front(x)`        | `multilistPushByTypeHead(&ml, state, &x)`       |
| Pop        | `list.pop_front()`          | `multilistPopHead(&ml, state, &result)`         |
| Iterate    | `for (auto& x : container)` | `while (multimapIteratorNext(&iter, elements))` |

### Memory Management

| Operation | Other Library       | datakit                          |
| --------- | ------------------- | -------------------------------- |
| Free      | `sdsfree(s)`        | `dksstr_free(s)`                 |
| Free      | `listRelease(l)`    | `multilistFree(ml)`              |
| Free      | `dictRelease(d)`    | `multimapFree(m)`                |
| Free      | `container.clear()` | Container-specific free function |

---

## Summary

### Key Differences

1. **Type System**: datakit uses `databox` for polymorphic storage instead of `void*`
2. **Memory Model**: Containers own their data; use pass-by-pointer for reallocation
3. **Iterators**: Explicit iterator structs instead of callbacks or C++ iterators
4. **Encoding**: Many structures use variable-width encoding for efficiency
5. **Compression**: Optional transparent compression (mflex, multilist)

### Migration Checklist

- [ ] Replace container types with datakit equivalents
- [ ] Convert data to `databox` format where needed
- [ ] Update iteration patterns to use datakit iterators
- [ ] Change memory management to use datakit free functions
- [ ] Test with existing data to verify correctness
- [ ] Measure performance and memory usage improvements
- [ ] Update documentation and comments

### Getting Help

- Check [PATTERNS.md](PATTERNS.md) for common coding patterns
- See [USE_CASES.md](USE_CASES.md) for complete examples
- Read [BENCHMARKS.md](BENCHMARKS.md) for performance comparisons
- Consult module-specific documentation for detailed APIs

For migration questions, refer to the [API Quick Reference](../API_QUICK_REFERENCE.md) for task-oriented code snippets.
