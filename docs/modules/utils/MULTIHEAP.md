# multiheap - Reference-Based Object Storage

## Overview

`multiheap` is a **specialized wrapper around multimap** that provides a simple interface for storing arbitrary objects indexed by integer references. It functions as a "heap" of objects that can be stored, retrieved, and resized by reference ID.

**Key Features:**

- Store arbitrary C objects by integer reference
- Direct pointer access to stored objects
- In-place object resizing
- Object insertion and retrieval via macros that infer `sizeof`
- Built on multimap's efficient storage

**Header**: `multiheap.h` (header-only — there is no `multiheap.c`)

**Dependencies**: `multimap.h`, `databox.h`, `flex.h`

> **Status:** no other datakit module calls multiheap yet; its behavior is
> pinned by its own test suite (`datakit-test test multiheap`). Read the
> [Resize contract](#resize-contract) before adopting it.

## Architecture

`multiheap` is implemented as a thin macro layer over multimap, using:

- **Keys**: 64-bit unsigned integer references (`DATABOX_UNSIGNED_64`)
- **Values**: Raw byte arrays containing the object image (`DATABOX_BYTES`)
- **Storage**: multimap in **map mode**, 2 elements per entry

### Map mode is load-bearing, not a style choice

A multiheap slot must behave as "save = overwrite this ref's value" (last write
wins), because every reader looks the slot up by **key only**. Map mode gives
exactly that: key-only compare, and on a hit the value is replaced in place.

The header used to say `multimapSetNew` — SET mode — which compares the **full
width** `{key, value}` pair. Re-saving a ref with a _changed_ value therefore
appended a **second** row for that ref instead of replacing it. The key-only
reader then returned whichever duplicate the sorted search happened to land on,
and because these values are raw little-endian integers stored as
`DATABOX_BYTES`, "sorted" means memcmp order — numerically scrambled, so
`extent = 1500` sorts _before_ `extent = 500`. A caller that saved state more
than once would restore a stale value and never converge.

The constructor is `multimapNew(2)` (map mode); `multimapSetNew(2)` would
break the last-write-wins contract described above.

```
┌─────────────────────────────────────────┐
│          multiheap (typedef)            │
│        (actually a multimap*)           │
└────────────────┬────────────────────────┘
                 │
                 v
         ┌───────────────┐
         │   multimap    │
         │   (2 fields)  │
         └───────┬───────┘
                 │
    ┌────────────┴────────────┐
    │  Key (ref ID)  │ Value  │
    │  UNSIGNED_64   │ BYTES  │
    └────────────────┴────────┘
```

## Data Structure

```c
/* multiheap is just a typedef - it's actually a multimap */
typedef multimap multiheap;
```

**Storage Format:**

Each entry in the multiheap contains:

- **Key**: uint64_t reference ID (stored as DATABOX_UNSIGNED_64)
- **Value**: Raw bytes of the object (stored as DATABOX_BYTES)

## API Reference

### Creation and Destruction

```c
/* Create new empty multiheap */
#define multiheapNew() (multiheap *)multimapNew(2)

/* Free multiheap and all stored objects */
#define multiheapFree(mh) multimapFree(mh)

/* Example */
multiheap *heap = multiheapNew();
/* ... use heap ... */
multiheapFree(heap);
```

**Note**: `multiheapNew()` creates a multimap in **map mode** with 2 elements
per entry (key + value). Map mode is what makes a repeat insert of the same ref
_overwrite_ rather than append — see
[Map mode is load-bearing](#map-mode-is-load-bearing-not-a-style-choice).

### Storing Objects

> **Insert takes `multiheap **`, Read takes `multiheap _`.** This is not a
stylistic inconsistency you can ignore: `multiheapInsertObj`expands to`multimapInsert((multimap \*\*)heap, ...)`, which must be able to update your
handle when the multimap reallocates or upgrades variant. `multiheapRead`expands to`multimapGetUnderlyingEntry((multimap _)heap, ...)` and takes the
> handle by value. Passing the wrong one compiles (the macros cast) and then
> corrupts memory. See [Pointer conventions](#pointer-conventions).

```c
/* Insert object into heap by reference
 * heap: ADDRESS of the heap handle (multiheap **)
 * ref: integer reference ID (uint64_t)
 * obj: pointer to object to store
 * Automatically uses sizeof(*obj) for size
 *
 * In map mode, inserting an existing ref REPLACES its value.
 */
#define multiheapInsert(heap, ref, obj)

/* Low-level insert with explicit size
 * heap: ADDRESS of the heap handle (multiheap **)
 * ref: integer reference ID
 * obj: pointer to object data
 * objSize: size in bytes to store
 */
#define multiheapInsertObj(heap, ref, obj, objSize)

/* Example: Store various object types */
typedef struct user {
    uint64_t id;
    char name[32];
    uint32_t age;
} user;

typedef struct config {
    bool enabled;
    uint32_t timeout;
    char path[256];
} config;

multiheap *heap = multiheapNew();

/* Store a user struct - note &heap, not heap */
user u = {.id = 1001, .name = "Alice", .age = 30};
multiheapInsert(&heap, 1001, &u);

/* Store a config struct */
config cfg = {.enabled = true, .timeout = 5000};
multiheapInsert(&heap, 2000, &cfg);

/* Store raw bytes with explicit size */
char buffer[1024] = {0};
multiheapInsertObj(&heap, 3000, buffer, 512);  // Only store 512 bytes

/* Same ref again REPLACES the previous value (map mode) */
user u2 = {.id = 1001, .name = "Alice II", .age = 31};
multiheapInsert(&heap, 1001, &u2);   // one row for 1001, not two
```

### Pointer conventions

| Macro / function        | `heap` argument | Why                                         |
| ----------------------- | --------------- | ------------------------------------------- |
| `multiheapNew`          | —               | returns the handle                          |
| `multiheapFree`         | `multiheap *`   | `multimapFree` frees by value               |
| `multiheapInsert`       | `multiheap **`  | `multimapInsert` may move the multimap      |
| `multiheapInsertObj`    | `multiheap **`  | same                                        |
| `multiheapRead`         | `multiheap *`   | lookup only, never resizes                  |
| `multiheapReadByKey`    | `multiheap *`   | same                                        |
| `multiheapRestore`      | `multiheap *`   | wraps `multiheapRead`                       |
| `multiheapReallocByKey` | `multiheap **`  | `multimapResizeEntry` may move the multimap |

### Retrieving Objects

```c
/* Get pointer to stored object by reference
 * heap: the heap handle (multiheap *)
 * ref: integer reference ID
 * Returns: void* pointer to object data
 * Note: Pointer is valid until the heap is modified
 * ASSERTS if ref is not present - see below
 */
#define multiheapRead(heap, ref)

/* Get pointer to stored object by databox key
 * heap: the heap handle (multiheap *)
 * key: databox containing reference ID
 * Returns: void* pointer to object data
 */
DK_FN_UNUSED static void *
multiheapReadByKey(multiheap *restrict const heap,
                   const databox *restrict const key);

/* Example: Read and use stored objects */
multiheap *heap = multiheapNew();

user u = {.id = 1001, .name = "Bob", .age = 25};
multiheapInsert(&heap, 1001, &u);          /* &heap to insert */

/* Get pointer to stored user - plain heap to read */
user *stored = multiheapRead(heap, 1001);
printf("Name: %s, Age: %u\n", stored->name, stored->age);

/* Modify in-place. Safe: the sort key is the ref, not the value,
 * so rewriting the value cannot disturb ordering. */
stored->age = 26;
```

> **There is no "not found" result.** `multiheapReadByKey` does
> `assert(found)` and then `assert(start)`. A missing ref aborts in a build with
> assertions enabled, and reads through an uninitialized `multimapEntry` in a
> build with `NDEBUG`. There is no NULL return to test, so **never write
> `if (!multiheapRead(...))`** — that branch is unreachable in a debug build and
> already-undefined behavior in a release build. Track which refs you have
> inserted yourself, or use `multimapExists()` on the underlying multimap first.

### Copying Objects

```c
/* Copy object from heap to local variable
 * heap: heap to read from
 * ref: integer reference ID
 * copyTo: pointer to destination variable
 * Uses sizeof(*copyTo) for size
 */
#define multiheapRestore(heap, ref, copyTo)

/* Example: Safe object retrieval */
multiheap *heap = multiheapNew();

user u = {.id = 1001, .name = "Charlie", .age = 35};
multiheapInsert(&heap, 1001, &u);

/* Later: restore to local copy */
user restored;
multiheapRestore(heap, 1001, &restored);
printf("Restored: %s, %u\n", restored.name, restored.age);

/* 'restored' is an independent copy - safe even if heap is modified */
```

`multiheapRestore` copies exactly `sizeof(*copyTo)` bytes with `memcpy`. It does
not know or check how many bytes were actually stored, so restoring into a type
larger than what was inserted reads past the stored value. Keep the insert type
and the restore type identical, and do not use it with flexible-array structs
(where `sizeof` excludes the tail).

### Resizing Objects

```c
/* Resize stored object by reference (or by databox key with ByKey)
 * heap: ADDRESS of the heap handle (multiheap **)
 * newSize: new size in bytes
 * Returns: void* pointer to the resized region
 * WARNING: pointer may change, and CONTENTS are unspecified after a
 * resize — repopulate through the returned pointer. */
#define multiheapRealloc(heap, ref, newSize)
#define multiheapReallocByKey(heap, key, newSize)

/* Example: dynamic object resizing */
multiheap *heap = multiheapNew();

/* Start with small buffer */
char initial[64] = "Hello";
multiheapInsertObj(&heap, 100, initial, 64);

/* Expand, then REPOPULATE (contents are unspecified after resize) */
char *expanded = multiheapRealloc(&heap, 100, 256);
snprintf(expanded, 256, "Hello World!");

/* IMPORTANT: always use the returned pointer after realloc */
char *ptr = multiheapRealloc(&heap, 100, 512);
snprintf(ptr, 512, "Hello Bigger World!");
// Don't use 'expanded' anymore - use 'ptr'
```

**Critical Notes on Resizing:**

1. The heap handle itself may change, which is why the realloc macros take its
   address and cast to `multimap **`
2. The object pointer will change if expansion requires relocation
3. Always use the returned pointer after a realloc
4. Every pointer previously returned by `multiheapRead()` is invalidated
5. `multiheapReallocByKey` asserts the key exists — it cannot grow a ref that
   was never inserted

### Resize contract

`multiheapRealloc(&heap, ref, newSize)` / `multiheapReallocByKey(&heap, key,
newSize)` resize a ref's VALUE region and return the live pointer to the
resized region. Two rules:

1. **Contents are unspecified after a resize** (flex entry resizing does not
   guarantee prefix preservation). Repopulate through the returned pointer
   before reading.
2. Both macros assert the key exists — they cannot grow a ref that was never
   inserted.

### Limitations

**There is no delete.** multiheap exposes no way to remove a single ref.
Entries live until `multiheapFree()` destroys the whole heap. If you need
removal, call `multimapDelete()` on the underlying multimap directly.

## Real-World Examples

### Example 1: Object Pool with Dynamic Sizing

```c
typedef struct buffer {
    size_t capacity;
    size_t length;
    char data[];  /* Flexible array member */
} buffer;

multiheap *bufferPool = multiheapNew();
uint64_t nextId = 1;

/* Allocate new buffer */
uint64_t createBuffer(size_t initialSize) {
    buffer *buf = malloc(sizeof(*buf) + initialSize);
    buf->capacity = initialSize;
    buf->length = 0;

    uint64_t id = nextId++;
    multiheapInsertObj(&bufferPool, id, buf, sizeof(*buf) + initialSize);
    free(buf);  /* Copied into heap */

    return id;
}

/* Append to buffer, resizing if needed */
void appendToBuffer(uint64_t id, const char *data, size_t len) {
    buffer *buf = multiheapRead(bufferPool, id);

    if (buf->length + len > buf->capacity) {
        /* Need to expand. multiheapRealloc does not compile, so go
         * through the ByKey form with an explicit key. */
        size_t newCap = (buf->length + len) * 2;
        databox key = {.data.u = id, .type = DATABOX_UNSIGNED_64};
        multiheapReallocByKey(&bufferPool, &key, sizeof(*buf) + newCap);
        buf = multiheapRead(bufferPool, id);  /* re-read after resize */
        buf->capacity = newCap;
    }

    memcpy(buf->data + buf->length, data, len);
    buf->length += len;
}

/* Get buffer contents */
const char *getBuffer(uint64_t id, size_t *outLen) {
    buffer *buf = multiheapRead(bufferPool, id);
    *outLen = buf->length;
    return buf->data;
}

/* Usage */
uint64_t buf = createBuffer(64);
appendToBuffer(buf, "Hello", 5);
appendToBuffer(buf, " World", 6);

size_t len;
const char *contents = getBuffer(buf, &len);
printf("Buffer: %.*s\n", (int)len, contents);
```

### Example 2: State Machine with Variable State Data

```c
typedef enum {
    STATE_IDLE,
    STATE_CONNECTING,
    STATE_CONNECTED,
    STATE_ERROR
} connectionState;

typedef struct stateData {
    connectionState state;
    uint32_t dataSize;
    char data[];  /* State-specific data */
} stateData;

multiheap *connections = multiheapNew();

void setConnectionState(uint64_t connId, connectionState newState,
                        const void *stateInfo, size_t infoSize) {
    stateData *sd = malloc(sizeof(*sd) + infoSize);
    sd->state = newState;
    sd->dataSize = infoSize;
    memcpy(sd->data, stateInfo, infoSize);

    /* Map mode: re-inserting the same connId replaces the old state
     * rather than appending a second row for it. */
    multiheapInsertObj(&connections, connId, sd, sizeof(*sd) + infoSize);
    free(sd);
}

connectionState getConnectionState(uint64_t connId) {
    stateData *sd = multiheapRead(connections, connId);
    return sd->state;
}

void transitionToConnected(uint64_t connId, const char *serverAddr) {
    setConnectionState(connId, STATE_CONNECTED,
                       serverAddr, strlen(serverAddr) + 1);
}

void transitionToError(uint64_t connId, const char *errorMsg) {
    setConnectionState(connId, STATE_ERROR,
                       errorMsg, strlen(errorMsg) + 1);
}
```

### Example 3: Cache with Object Versioning

```c
typedef struct cachedObject {
    uint64_t version;
    uint64_t timestamp;
    uint32_t size;
    char data[];
} cachedObject;

multiheap *cache = multiheapNew();

void cacheStore(uint64_t key, const void *data, size_t size) {
    cachedObject *obj = malloc(sizeof(*obj) + size);
    obj->version = 1;
    obj->timestamp = time(NULL);
    obj->size = size;
    memcpy(obj->data, data, size);

    multiheapInsertObj(&cache, key, obj, sizeof(*obj) + size);
    free(obj);
}

bool cacheUpdate(uint64_t key, const void *data, size_t size) {
    /* multiheapRead cannot report "missing" - it asserts. Check first
     * on the underlying multimap. */
    databox keyBox = {.data.u = key, .type = DATABOX_UNSIGNED_64};
    if (!multimapExists((multimap *)cache, &keyBox)) {
        return false;  /* Not in cache */
    }

    /* Resize, then re-read: the resize may have moved everything */
    size_t newTotalSize = sizeof(cachedObject) + size;
    multiheapReallocByKey(&cache, &keyBox, newTotalSize);

    cachedObject *existing = multiheapRead(cache, key);

    /* Update metadata */
    existing->version++;
    existing->timestamp = time(NULL);
    existing->size = size;
    memcpy(existing->data, data, size);

    return true;
}

const void *cacheRetrieve(uint64_t key, size_t *outSize,
                          uint64_t *outVersion) {
    databox keyBox = {.data.u = key, .type = DATABOX_UNSIGNED_64};
    if (!multimapExists((multimap *)cache, &keyBox)) {
        return NULL;   /* must test BEFORE reading - Read asserts */
    }

    cachedObject *obj = multiheapRead(cache, key);

    *outSize = obj->size;
    *outVersion = obj->version;
    return obj->data;
}
```

### Example 4: Serialization Buffer Manager

```c
/* Manage serialization buffers that grow as needed */
multiheap *serBuffers = multiheapNew();

typedef struct serBuffer {
    size_t used;
    size_t capacity;
    uint8_t bytes[];
} serBuffer;

uint64_t createSerializer(void) {
    static uint64_t nextId = 1;
    serBuffer sb = {.used = 0, .capacity = 256};

    uint64_t id = nextId++;
    multiheapInsertObj(&serBuffers, id, &sb, sizeof(sb) + 256);
    return id;
}

void serializeUint32(uint64_t serId, uint32_t value) {
    serBuffer *sb = multiheapRead(serBuffers, serId);

    if (sb->used + 4 > sb->capacity) {
        size_t newCap = sb->capacity * 2;
        databox key = {.data.u = serId, .type = DATABOX_UNSIGNED_64};
        multiheapReallocByKey(&serBuffers, &key, sizeof(*sb) + newCap);
        sb = multiheapRead(serBuffers, serId);
        sb->capacity = newCap;
    }

    memcpy(sb->bytes + sb->used, &value, 4);
    sb->used += 4;
}

void serializeString(uint64_t serId, const char *str) {
    size_t len = strlen(str) + 1;
    serBuffer *sb = multiheapRead(serBuffers, serId);

    if (sb->used + len > sb->capacity) {
        size_t newCap = sb->capacity;
        while (newCap < sb->used + len) {
            newCap *= 2;
        }
        databox key = {.data.u = serId, .type = DATABOX_UNSIGNED_64};
        multiheapReallocByKey(&serBuffers, &key, sizeof(*sb) + newCap);
        sb = multiheapRead(serBuffers, serId);
        sb->capacity = newCap;
    }

    memcpy(sb->bytes + sb->used, str, len);
    sb->used += len;
}

const uint8_t *getSerializedData(uint64_t serId, size_t *outSize) {
    serBuffer *sb = multiheapRead(serBuffers, serId);
    *outSize = sb->used;
    return sb->bytes;
}
```

## Algorithm Explanation

`multiheap` operations map directly to underlying multimap operations:

### Insert Algorithm

```c
multiheapInsert(&heap, ref, obj)
  → Create databox key: {type = UNSIGNED_64, data.u = ref}
  → Create databox value: {type = BYTES, len = sizeof(*obj), data.ptr = obj}
  → Call multimapInsert((multimap **)heap, [key, value])
  → Map mode: key-only compare; on a hit the value is replaced in place,
    otherwise a new entry is inserted in sorted position
```

The object is **copied** into the multimap's flex storage. The pointer you pass
is not retained, so a stack object or a temporarily-malloc'd staging buffer is
fine — the examples above free theirs immediately after inserting.

### Read Algorithm

```c
multiheapRead(heap, ref)
  → Create databox key: {type = UNSIGNED_64, data.u = ref}
  → Call multimapGetUnderlyingEntry((multimap *)heap, key, &entry)
  → assert(found)                    /* no NULL return path */
  → The entry points AT the key, so step forward one element to reach
    the value: flexNext(*entry.map, entry.fe)
  → Strip flex metadata: flexGetByType() → val.data.bytes.start
  → assert(start)
  → Return pointer to object data
```

### Realloc Algorithm

```c
multiheapReallocByKey(&heap, &key, newSize)
  → multimapGetUnderlyingEntry((multimap *)*heap, key, &entry)
  → assert(found)
  → multimapResizeEntry((multimap **)heap, &entry, newSize)
    → May trigger flex reallocation
    → May trigger multimap variant upgrade (Small→Medium→Full)
    → May move the heap handle itself, which is why it takes &heap
  → Re-fetch and return the object pointer
    (but see defect 2 above - re-read with multiheapRead instead)
```

Resizing changes only the value's byte length; the key is untouched, so the
entry keeps its sorted position and no other ref is disturbed.

## Performance Characteristics

| Operation    | Complexity   | Notes                                         |
| ------------ | ------------ | --------------------------------------------- |
| New          | O(1)         | Creates a map-mode multimap with 2 elements   |
| Insert       | O(log n)     | Binary search; replaces in place on a key hit |
| Read         | O(log n)     | Binary search to find entry                   |
| Restore      | O(log n + k) | Search + memcpy of `sizeof(*copyTo)` bytes    |
| ReallocByKey | O(log n + k) | Search + potential flex/variant reallocation  |
| Free         | O(n)         | Releases the multimap and all stored bytes    |
| Delete       | —            | Not provided; see defect 3                    |

### Memory Overhead

```
Base overhead (multiheap itself):
- multimapSmall:  0 bytes (a bare map: its 1 flex alone)
- multimapMedium: 28 bytes (2 flex arrays)
- multimapFull:   40+ bytes (N flex arrays)

Per stored object:
- flex metadata: ~2-4 bytes per field
- Key storage:   5-9 bytes (varint encoded uint64_t)
- Value header:  2-4 bytes (size prefix)
- Object data:   sizeof(object) bytes

Example: Store 100 user structs (64 bytes each):
- Multimap overhead: 28 bytes (medium)
- Flex metadata:     ~800 bytes (8 bytes × 100 entries)
- Object data:       6,400 bytes
- Total:             ~7,228 bytes (~1.13x object size)
```

### Variant Transitions

The underlying multimap automatically upgrades storage:

```
multimapSmall
  ↓ (exceeds 1 flex capacity)
multimapMedium
  ↓ (exceeds 2 flex capacity)
multimapFull
```

An upgrade can move the multimap, which is exactly why the mutating macros take
the handle's address. See [multimap](../multimap/MULTIMAP.md) for the current
thresholds and per-variant sizes — do not treat the byte figures above this
section as authoritative, they are illustrative only.

## Best Practices

### 1. Use Type-Safe Insertion

```c
/* GOOD - automatic sizeof */
user u = {0};
multiheapInsert(&heap, 1, &u);

/* AVOID - manual sizeof (error-prone) */
multiheapInsertObj(&heap, 1, &u, sizeof(user));
```

`multiheapInsert` expands to `multiheapInsertObj(heap, ref, obj, sizeof(*obj))`,
so it is only correct when `obj` is a pointer to a complete object. For a struct
with a flexible array member, `sizeof(*obj)` excludes the tail — use
`multiheapInsertObj` with the real total size, as every flexible-array example
in this document does.

### 2. Always Use Returned Pointers After Realloc

```c
databox key = {.data.u = 1, .type = DATABOX_UNSIGNED_64};

/* WRONG - stale pointer */
char *ptr = multiheapRead(heap, 1);
multiheapReallocByKey(&heap, &key, 1024);
strcpy(ptr, "data");  // MAY CRASH - ptr is invalid

/* RIGHT - re-read after the resize */
multiheapReallocByKey(&heap, &key, 1024);
char *ptr = multiheapRead(heap, 1);
strcpy(ptr, "data");  // Safe
```

### 3. Use multiheapRestore for Long-Lived Access

```c
/* BAD - pointer may become invalid */
user *ptr = multiheapRead(heap, 1);
// ... many operations that might modify heap ...
printf("%s\n", ptr->name);  // Risky

/* GOOD - copy to local */
user local;
multiheapRestore(heap, 1, &local);
// ... any operations ...
printf("%s\n", local.name);  // Safe
```

### 4. Re-inserting a Ref Overwrites It

In map mode a repeat insert is an update, not a failure and not a duplicate.
That is the intended way to save new state for an existing ref.

```c
/* Distinct refs - two entries */
static uint64_t nextRef = 1;
multiheapInsert(&heap, nextRef++, &obj1);
multiheapInsert(&heap, nextRef++, &obj2);

/* Same ref twice - ONE entry, holding obj2 */
multiheapInsert(&heap, 1, &obj1);
multiheapInsert(&heap, 1, &obj2);  // replaces obj1
```

This is the whole reason multiheap must not be built with `multimapSetNew`: in
set mode the second call compares the full `{key, value}` pair, sees a different
value, and appends a second row for ref 1. The key-only reader would then return
whichever of the two the sorted search landed on.

## Common Pitfalls

### 1. Using Stale Pointers

```c
/* WRONG */
data *p1 = multiheapRead(heap, 1);
data *p2 = multiheapRead(heap, 2);
multiheapInsert(&heap, 3, &newData);  // May invalidate p1, p2
printf("%d\n", p1->value);  // DANGER

/* RIGHT */
data *p1 = multiheapRead(heap, 1);
printf("%d\n", p1->value);  // Use immediately
```

Any insert or realloc can move the flex storage, and can move the heap handle
itself. Treat every pointer from `multiheapRead()` as valid only until the next
mutating call.

### 2. Ignoring Realloc Return Value

Because of the pointer-depth defect described above, the safest pattern today is
the _opposite_ of what you would normally write: perform the resize, then take
a fresh `multiheapRead` rather than trusting the macro's return value.

```c
databox key = {.data.u = 1, .type = DATABOX_UNSIGNED_64};

/* SAFE today - resize, then re-read */
multiheapReallocByKey(&heap, &key, 2048);
char *ptr = multiheapRead(heap, 1);

/* The natural form, once defect 2 is fixed */
char *ptr = multiheapReallocByKey(&heap, &key, 2048);
```

### 3. Assuming Pointer Stability

```c
/* WRONG - assumes pointer won't move */
typedef struct node {
    struct node *next;
    int value;
} node;

node n1 = {.value = 1};
multiheapInsert(&heap, 1, &n1);
node *p1 = multiheapRead(heap, 1);

node n2 = {.next = p1, .value = 2};  // Store pointer to heap object
multiheapInsert(&heap, 2, &n2);  // p1 may now be invalid!

/* RIGHT - use references instead of pointers */
typedef struct node {
    uint64_t nextRef;  /* Reference ID instead of pointer */
    int value;
} node;
```

## Comparison with Alternatives

| Feature    | multiheap    | hash table | array          | malloc pool |
| ---------- | ------------ | ---------- | -------------- | ----------- |
| Lookup     | O(log n)     | O(1) avg   | O(1)           | O(1)        |
| Insert     | O(log n)     | O(1) avg   | O(1) append    | O(1)        |
| Resize     | O(log n)     | N/A        | O(n)           | Complex     |
| Ordered    | Yes (by ref) | No         | Yes (by index) | No          |
| Memory     | Medium       | High       | Low            | Variable    |
| Sparse IDs | Efficient    | Efficient  | Wasteful       | Wasteful    |

**Choose multiheap when:**

- Need to store objects by integer ID
- IDs may be sparse or non-sequential
- Need to resize objects in-place
- Want automatic memory management
- Prefer sorted storage

**Choose alternatives when:**

- Need O(1) lookup (use hash table)
- IDs are dense/sequential (use [offsetArray](../memory/OFFSET_ARRAY.md))
- Need pointer stability (use a custom allocator)
- Need to delete individual entries
- Need a "missing key" result rather than an assert

## Thread Safety

`multiheap` is **not thread-safe**. Concurrent access requires external synchronization:

```c
pthread_mutex_t heapMutex = PTHREAD_MUTEX_INITIALIZER;

void threadSafeInsert(uint64_t ref, void *obj, size_t size) {
    pthread_mutex_lock(&heapMutex);
    multiheapInsertObj(&heap, ref, obj, size);
    pthread_mutex_unlock(&heapMutex);
}

void *threadSafeRead(uint64_t ref, size_t size) {
    pthread_mutex_lock(&heapMutex);
    void *ptr = multiheapRead(heap, ref);
    /* Must copy before unlock - the pointer is only valid while no
     * other thread can mutate the heap */
    void *copy = malloc(size);
    memcpy(copy, ptr, size);
    pthread_mutex_unlock(&heapMutex);
    return copy;
}
```

Note that the caller has to supply `size` in both directions: multiheap stores
the value length internally but exposes no way to query it.

## See Also

- [multimap](../multimap/MULTIMAP.md) - Underlying sorted key-value store
- [flex](../flex/FLEX.md) - Low-level flexible array used by multimap
- [databox](../core/DATABOX.md) - Type-safe value container system

## Implementation Notes

`multiheap` is entirely implemented as macros in `multiheap.h` - there is no `.c` file. All functionality delegates to the multimap implementation.

The key design decisions:

1. **Map mode**: uses `multimapNew(2)` so a re-save of a ref overwrites its
   value. Set mode would append a duplicate row and break the key-only reader.
2. **Convenience sizing**: `multiheapInsert` / `multiheapRestore` infer
   `sizeof(*obj)`; the `Obj` and `ByKey` forms take explicit sizes for
   flexible-array types.
3. **Copy-in storage**: the object image is copied into flex storage, so the
   source pointer need not outlive the call.
4. **Minimal overhead**: a macro layer over multimap with no state of its own.

Statement-expression macros (`({ ... })`) are a GNU extension; this header is
clang/gcc-only, which matches the rest of datakit.

### If you adopt this module

It is unused and untested today. Before relying on it, fix the two header
defects listed above and add a test that at minimum covers: insert/read
round-trip, re-insert overwriting rather than duplicating, resize followed by
read, and a value large enough to force a multimap variant upgrade.
