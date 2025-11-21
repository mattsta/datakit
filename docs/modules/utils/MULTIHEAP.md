# multiheap - Reference-Based Object Storage

## Overview

`multiheap` is a **specialized wrapper around multimap** that provides a simple interface for storing arbitrary objects indexed by integer references. It functions as a "heap" of objects that can be stored, retrieved, and resized by reference ID.

**Key Features:**

- Store arbitrary C objects by integer reference
- Direct pointer access to stored objects
- In-place object resizing with automatic reallocation
- Type-safe object insertion and retrieval via macros
- Zero-copy object restoration
- Built on multimap's efficient storage

**Header**: `multiheap.h`

**Dependencies**: `multimap.h`, `databox.h`

## Architecture

`multiheap` is implemented as a thin macro layer over multimap, using:

- **Keys**: 64-bit unsigned integer references
- **Values**: Raw byte arrays containing serialized objects
- **Storage**: Leverages multimap's scale-aware storage optimization

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
#define multiheapNew() (multiheap *)multimapSetNew(2)

/* Free multiheap and all stored objects */
#define multiheapFree(mh) multimapFree(mh)

/* Example */
multiheap *heap = multiheapNew();
/* ... use heap ... */
multiheapFree(heap);
```

**Note**: `multiheapNew()` creates a multimap in **set mode** with 2 elements per entry (key + value), ensuring unique reference IDs.

### Storing Objects

```c
/* Insert object into heap by reference
 * heap: heap to insert into
 * ref: integer reference ID (uint64_t)
 * obj: pointer to object to store
 * Automatically uses sizeof(*obj) for size
 */
#define multiheapInsert(heap, ref, obj)

/* Low-level insert with explicit size
 * heap: heap to insert into
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

/* Store a user struct */
user u = {.id = 1001, .name = "Alice", .age = 30};
multiheapInsert(heap, 1001, &u);

/* Store a config struct */
config cfg = {.enabled = true, .timeout = 5000};
multiheapInsert(heap, 2000, &cfg);

/* Store raw bytes with explicit size */
char buffer[1024];
multiheapInsertObj(heap, 3000, buffer, 512);  // Only store 512 bytes
```

### Retrieving Objects

```c
/* Get pointer to stored object by reference
 * heap: heap to read from
 * ref: integer reference ID
 * Returns: void* pointer to object data
 * Note: Pointer is valid until heap is modified
 */
#define multiheapRead(heap, ref)

/* Get pointer to stored object by databox key
 * heap: heap to read from
 * key: databox containing reference ID
 * Returns: void* pointer to object data
 */
static void *multiheapReadByKey(multiheap *restrict const heap,
                                const databox *restrict const key);

/* Example: Read and use stored objects */
multiheap *heap = multiheapNew();

user u = {.id = 1001, .name = "Bob", .age = 25};
multiheapInsert(heap, 1001, &u);

/* Get pointer to stored user */
user *stored = multiheapRead(heap, 1001);
printf("Name: %s, Age: %u\n", stored->name, stored->age);

/* Modify in-place (careful - may affect sorted order) */
stored->age = 26;
```

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
multiheapInsert(heap, 1001, &u);

/* Later: restore to local copy */
user restored;
multiheapRestore(heap, 1001, &restored);
printf("Restored: %s, %u\n", restored.name, restored.age);

/* 'restored' is independent copy - safe even if heap is modified */
```

### Resizing Objects

```c
/* Resize stored object by reference
 * heap: pointer to heap (may be updated if reallocation occurs)
 * ref: integer reference ID
 * newSize: new size in bytes
 * Returns: void* pointer to resized object data
 * WARNING: Pointer may change after resize!
 */
#define multiheapRealloc(heap, ref, newSize)

/* Resize stored object by databox key
 * heap: pointer to heap (may be updated if reallocation occurs)
 * key: databox containing reference ID
 * newSize: new size in bytes
 * Returns: void* pointer to resized object data
 */
#define multiheapReallocByKey(heap, key, newSize)

/* Example: Dynamic object resizing */
multiheap *heap = multiheapNew();

/* Start with small buffer */
char initial[64] = "Hello";
multiheapInsertObj(heap, 100, initial, 64);

/* Expand to larger buffer */
char *expanded = multiheapRealloc(heap, 100, 256);
strcat(expanded, " World!");  // Now have room for more data

/* IMPORTANT: Always use returned pointer after realloc */
char *ptr = multiheapRealloc(heap, 100, 512);
// Don't use 'expanded' anymore - use 'ptr'
```

**Critical Notes on Resizing:**

1. The heap pointer itself may change (underlying multimap reallocation)
2. The object pointer will change if expansion requires relocation
3. Always use the returned pointer after `multiheapRealloc()`
4. Original pointers from `multiheapRead()` are invalidated

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
    multiheapInsertObj(bufferPool, id, buf, sizeof(*buf) + initialSize);
    free(buf);  /* Copied into heap */

    return id;
}

/* Append to buffer, resizing if needed */
void appendToBuffer(uint64_t id, const char *data, size_t len) {
    buffer *buf = multiheapRead(bufferPool, id);

    if (buf->length + len > buf->capacity) {
        /* Need to expand */
        size_t newCap = (buf->length + len) * 2;
        buf = multiheapRealloc(bufferPool, id, sizeof(*buf) + newCap);
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

    multiheapInsertObj(connections, connId, sd, sizeof(*sd) + infoSize);
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

    multiheapInsertObj(cache, key, obj, sizeof(*obj) + size);
    free(obj);
}

bool cacheUpdate(uint64_t key, const void *data, size_t size) {
    cachedObject *existing = multiheapRead(cache, key);
    if (!existing) {
        return false;  /* Not in cache */
    }

    /* Resize if needed */
    size_t newTotalSize = sizeof(*existing) + size;
    existing = multiheapRealloc(cache, key, newTotalSize);

    /* Update metadata */
    existing->version++;
    existing->timestamp = time(NULL);
    existing->size = size;
    memcpy(existing->data, data, size);

    return true;
}

const void *cacheRetrieve(uint64_t key, size_t *outSize,
                          uint64_t *outVersion) {
    cachedObject *obj = multiheapRead(cache, key);
    if (!obj) {
        return NULL;
    }

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
    multiheapInsertObj(serBuffers, id, &sb, sizeof(sb) + 256);
    return id;
}

void serializeUint32(uint64_t serId, uint32_t value) {
    serBuffer *sb = multiheapRead(serBuffers, serId);

    if (sb->used + 4 > sb->capacity) {
        size_t newCap = sb->capacity * 2;
        sb = multiheapRealloc(serBuffers, serId, sizeof(*sb) + newCap);
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
        sb = multiheapRealloc(serBuffers, serId, sizeof(*sb) + newCap);
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
multiheapInsert(heap, ref, obj)
  → Create databox key: {type = UNSIGNED_64, data.u = ref}
  → Create databox value: {type = BYTES, len = sizeof(*obj), data = obj}
  → Call multimapInsert(heap, [key, value])
  → Returns immediately (O(log n) binary search + insertion)
```

### Read Algorithm

```c
multiheapRead(heap, ref)
  → Create databox key: {type = UNSIGNED_64, data.u = ref}
  → Call multimapGetUnderlyingEntry(heap, key, &entry)
  → Navigate to value in flex: flexNext(entry.map, entry.fe)
  → Extract bytes pointer: flexGetByType() → val.data.bytes.start
  → Return pointer to object data
```

### Realloc Algorithm

```c
multiheapRealloc(heap, ref, newSize)
  → Get underlying entry (contains current object)
  → Call multimapResizeEntry(heap, entry, newSize)
    → May trigger flex reallocation
    → May trigger multimap variant upgrade (Small→Medium→Full)
  → Re-fetch entry (pointers may have changed)
  → Return new pointer to resized object
```

## Performance Characteristics

| Operation | Complexity   | Notes                            |
| --------- | ------------ | -------------------------------- |
| New       | O(1)         | Creates multimap with 2 fields   |
| Insert    | O(log n)     | Binary search in sorted multimap |
| Read      | O(log n)     | Binary search to find entry      |
| Restore   | O(log n + k) | Search + memcpy of k bytes       |
| Realloc   | O(log n + k) | Search + potential reallocation  |
| Free      | O(n)         | Must free all stored objects     |

### Memory Overhead

```
Base overhead (multiheap itself):
- multimapSmall:  16 bytes (1 flex)
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
multiheapSmall (16 B)
  ↓ (exceeds 1 flex capacity)
multimapMedium (28 B)
  ↓ (exceeds 2 flex capacity)
multimapFull (40+ B)
```

## Best Practices

### 1. Use Type-Safe Insertion

```c
/* GOOD - automatic sizeof */
user u = {...};
multiheapInsert(heap, 1, &u);

/* AVOID - manual sizeof (error-prone) */
multiheapInsertObj(heap, 1, &u, sizeof(user));
```

### 2. Always Use Returned Pointers After Realloc

```c
/* WRONG - stale pointer */
char *ptr = multiheapRead(heap, 1);
multiheapRealloc(heap, 1, 1024);
strcpy(ptr, "data");  // MAY CRASH - ptr is invalid

/* RIGHT - use returned pointer */
char *ptr = multiheapRealloc(heap, 1, 1024);
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

### 4. Prefer Unique Reference IDs

```c
/* GOOD - unique IDs */
static uint64_t nextRef = 1;
multiheapInsert(heap, nextRef++, &obj1);
multiheapInsert(heap, nextRef++, &obj2);

/* BAD - duplicate IDs (second insert will fail in set mode) */
multiheapInsert(heap, 1, &obj1);
multiheapInsert(heap, 1, &obj2);  // Fails - duplicate key
```

## Common Pitfalls

### 1. Using Stale Pointers

```c
/* WRONG */
data *p1 = multiheapRead(heap, 1);
data *p2 = multiheapRead(heap, 2);
multiheapInsert(heap, 3, &newData);  // May invalidate p1, p2
printf("%d\n", p1->value);  // DANGER

/* RIGHT */
data *p1 = multiheapRead(heap, 1);
printf("%d\n", p1->value);  // Use immediately
```

### 2. Ignoring Realloc Return Value

```c
/* WRONG */
multiheapRealloc(heap, 1, 2048);
char *ptr = multiheapRead(heap, 1);  // Extra lookup

/* RIGHT */
char *ptr = multiheapRealloc(heap, 1, 2048);  // Use return value
```

### 3. Assuming Pointer Stability

```c
/* WRONG - assumes pointer won't move */
typedef struct node {
    struct node *next;
    int value;
} node;

node n1 = {.value = 1};
multiheapInsert(heap, 1, &n1);
node *p1 = multiheapRead(heap, 1);

node n2 = {.next = p1, .value = 2};  // Store pointer to heap object
multiheapInsert(heap, 2, &n2);  // p1 may now be invalid!

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
- IDs are dense/sequential (use array)
- Need pointer stability (use custom allocator)

## Thread Safety

`multiheap` is **not thread-safe**. Concurrent access requires external synchronization:

```c
pthread_mutex_t heapMutex = PTHREAD_MUTEX_INITIALIZER;

void threadSafeInsert(uint64_t ref, void *obj) {
    pthread_mutex_lock(&heapMutex);
    multiheapInsert(heap, ref, obj);
    pthread_mutex_unlock(&heapMutex);
}

void *threadSafeRead(uint64_t ref) {
    pthread_mutex_lock(&heapMutex);
    void *ptr = multiheapRead(heap, ref);
    /* Must copy before unlock or pointer may become invalid */
    void *copy = malloc(size);
    memcpy(copy, ptr, size);
    pthread_mutex_unlock(&heapMutex);
    return copy;
}
```

## See Also

- [multimap](../multimap/MULTIMAP.md) - Underlying sorted key-value store
- [flex](../flex/FLEX.md) - Low-level flexible array used by multimap
- [databox](../core/DATABOX.md) - Type-safe value container system

## Implementation Notes

`multiheap` is entirely implemented as macros in `multiheap.h` - there is no `.c` file. All functionality delegates to the multimap implementation.

The key design decisions:

1. **Set mode**: Uses `multimapSetNew(2)` to ensure unique reference IDs
2. **Type safety**: Macros use `sizeof(*obj)` to avoid size errors
3. **Pointer returns**: All operations return updated pointers for safety
4. **Minimal overhead**: Zero-cost abstraction over multimap
