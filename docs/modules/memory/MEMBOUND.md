# membound - Bounded Memory Allocator

## Overview

`membound` is a **memory allocator with bounded allocation from a pre-allocated pool**. It provides malloc/free semantics within a fixed memory region, using a buddy allocation algorithm for efficient space management without relying on the system allocator.

**Key Features:**

- Fixed-size memory pool (no dynamic growth beyond initial size)
- Power-of-two buddy allocation algorithm
- Thread-safe with mutex protection
- Process-shared memory support (via mmap with MAP_SHARED)
- Zero external malloc dependencies after initialization
- Automatic coalescing of adjacent free blocks
- Maximum single allocation: 1 GiB

**Header**: `membound.h`

**Source**: `membound.c`

**Origin**: Adapted from SQLite's mem5.c allocator

## What is Bounded Memory Allocation?

Bounded memory allocators solve the problem of **unpredictable memory usage** by pre-allocating a fixed pool and serving all allocations from that pool. This provides:

1. **Guaranteed memory limits** - Never exceed the pool size
2. **No system malloc calls** - All allocations come from the pool
3. **Predictable fragmentation** - Mathematically bounded by Robson's theorem
4. **Zero overhead per allocation** - No malloc metadata per block
5. **Process isolation** - Memory can be shared between processes via mmap

### When to Use membound

**Use membound when:**

- You need strict memory limits (e.g., embedded systems, containers)
- Predictable memory usage is critical
- You want to isolate allocations from the system heap
- Sharing memory between processes (fork-safe allocations)
- Testing memory pressure scenarios

**Don't use membound when:**

- You need unlimited dynamic allocation
- Single allocations exceed 1 GiB
- You need very fine-grained allocation sizes (< 256 bytes wastes space)
- Standard malloc performance is sufficient

## Data Structure

```c
typedef struct membound {
    /* Memory pool */
    uint8_t *zPool;         /* Memory available for allocation */
    int64_t szAtom;         /* Smallest possible allocation (typically 256 bytes) */
    uint64_t size;          /* Total byte extent of 'zPool' */
    int64_t nBlock;         /* Number of szAtom sized blocks in zPool */

    /* Thread safety */
    pthread_mutex_t mutex;  /* Mutex for thread-safe access */

    /* Performance statistics */
    uint64_t nAlloc;        /* Total number of malloc calls */
    uint64_t totalAlloc;    /* Total of all malloc calls (including internal frag) */
    uint64_t totalExcess;   /* Total internal fragmentation */
    uint32_t currentOut;    /* Current bytes checked out (including frag) */
    uint32_t currentCount;  /* Current number of distinct allocations */
    uint32_t maxOut;        /* Maximum instantaneous currentOut */
    uint32_t maxCount;      /* Maximum instantaneous currentCount */

    /* Free lists - one per power-of-two size class */
    int32_t aiFreelist[31]; /* Free block lists by size class */

    /* Control metadata - one byte per block */
    uint8_t *aCtrl;         /* Tracks allocated/free status and size */
} membound;
```

### Memory Layout

```
Total Memory = Pool + Control Array

┌─────────────────────────────────────────┐
│         Memory Pool (zPool)              │  User allocations
│    nBlock * szAtom bytes                 │
├─────────────────────────────────────────┤
│      Control Array (aCtrl)               │  Metadata (1 byte per block)
│         nBlock bytes                     │
└─────────────────────────────────────────┘

Example with 1 MB pool, 256-byte atoms:
- nBlock = 1,048,576 / 257 = 4,079 blocks
- Pool size = 4,079 * 256 = 1,044,224 bytes
- Control size = 4,079 bytes
- Total = 1,048,303 bytes
```

### Control Byte Format

```c
typedef enum memboundCtrl {
    MEMBOUND_CTRL_LOGSIZE = 0x1f,  /* Bits 0-4: Log2(size in atoms) */
    MEMBOUND_CTRL_FREE = 0x20       /* Bit 5: Free flag */
} memboundCtrl;

Examples:
0x00 = allocated, size = 2^0 = 1 atom
0x03 = allocated, size = 2^3 = 8 atoms
0x25 = free, size = 2^5 = 32 atoms
```

## Buddy Allocation Algorithm

The allocator uses the **binary buddy system**:

1. **Power-of-two sizes**: All allocations are rounded up to powers of 2
2. **Block splitting**: Large blocks are split into smaller buddies when needed
3. **Block coalescing**: Adjacent free buddies are merged back together
4. **Free lists**: One list per size class (2^0 atoms, 2^1 atoms, ..., 2^30 atoms)

### Allocation Example

```
Initial state: One 1024-byte block free (assuming 256-byte atoms = 4 atoms)

aiFreelist[2] → Block 0 (4 atoms = 1024 bytes)

Request 256 bytes (1 atom):
1. Need size class 0 (2^0 = 1 atom)
2. No blocks in freelist[0], split from freelist[2]
3. Split: [0-3] → [0-1] and [2-3]
4. Split: [0-1] → [0] and [1]
5. Allocate block 0, return blocks 1, 2-3 to freelists

aiFreelist[0] → Block 1
aiFreelist[1] → Block 2-3
Block 0: allocated
```

### Deallocation and Coalescing

```c
Free block 0 (1 atom):
1. Check buddy block 1 - is it free and same size?
   Yes → Merge [0] + [1] = [0-1]
2. Check buddy [2-3] - is it free and same size?
   Yes → Merge [0-1] + [2-3] = [0-3]
3. Add [0-3] back to freelist[2]

Result: Back to initial state!
```

## API Reference

### Creation and Destruction

```c
/* Create new membound allocator with specified pool size
 * Returns: membound pointer on success, NULL on failure
 * Note: Uses mmap for process-shared memory
 */
membound *memboundCreate(size_t size);

/* Shutdown allocator (unsafe - may have outstanding allocations)
 * Returns: true on success, false on error
 */
bool memboundShutdown(membound *m);

/* Shutdown allocator only if no outstanding allocations
 * Returns: true on success, false if allocations still exist
 */
bool memboundShutdownSafe(membound *m);

/* Example: Create 10 MB allocator */
membound *m = memboundCreate(10 * 1024 * 1024);
if (!m) {
    fprintf(stderr, "Failed to create allocator\n");
    exit(1);
}

/* ... use allocator ... */

/* Safe shutdown - only succeeds if all memory freed */
if (!memboundShutdownSafe(m)) {
    fprintf(stderr, "Memory leak detected!\n");
}
```

### Growing the Pool

```c
/* Increase pool size (creates new pool, copies data, releases old)
 * m: membound allocator
 * size: new total size (must be larger than current)
 * Returns: true on success, false on failure
 */
bool memboundIncreaseSize(membound *m, size_t size);

/* Example: Grow from 10 MB to 20 MB */
if (!memboundIncreaseSize(m, 20 * 1024 * 1024)) {
    fprintf(stderr, "Failed to grow allocator\n");
}
```

**Warning**: Growing is expensive - copies entire pool!

### Memory Allocation

```c
/* Allocate memory from pool
 * size: bytes to allocate (max 1 GiB)
 * Returns: pointer to memory, or NULL if insufficient space
 */
void *memboundAlloc(membound *m, size_t size);

/* Free previously allocated memory
 * p: pointer returned by memboundAlloc (must not be NULL)
 */
void memboundFree(membound *m, void *p);

/* Reallocate memory (resize existing allocation)
 * p: existing allocation (must not be NULL)
 * newlen: new size (must be power of 2)
 * Returns: pointer to resized memory, or NULL on failure
 */
void *memboundRealloc(membound *m, void *p, int32_t newlen);

/* Example: Basic allocation */
void *data = memboundAlloc(m, 1024);
if (!data) {
    fprintf(stderr, "Out of memory\n");
    return;
}

/* Use the memory */
memset(data, 0, 1024);

/* Free when done */
memboundFree(m, data);
```

### Statistics and Monitoring

```c
/* Get current number of outstanding allocations
 * Returns: count of allocations not yet freed
 */
size_t memboundCurrentAllocationCount(const membound *m);

/* Reset allocator to initial state (frees all memory)
 * WARNING: Invalidates all outstanding pointers!
 */
void memboundReset(membound *m);

/* Example: Check for leaks */
size_t outstanding = memboundCurrentAllocationCount(m);
if (outstanding > 0) {
    fprintf(stderr, "Memory leak: %zu allocations not freed\n", outstanding);
}
```

## Real-World Examples

### Example 1: Fixed-Memory Cache

```c
/* Image cache with strict memory limit */
typedef struct imageCache {
    membound *allocator;
    struct {
        char *key;
        void *imageData;
        size_t size;
    } entries[1000];
    size_t count;
} imageCache;

imageCache *imageCacheCreate(size_t maxMemoryBytes) {
    imageCache *cache = malloc(sizeof(*cache));

    /* Create bounded allocator for cache data */
    cache->allocator = memboundCreate(maxMemoryBytes);
    if (!cache->allocator) {
        free(cache);
        return NULL;
    }

    cache->count = 0;
    return cache;
}

bool imageCacheAdd(imageCache *cache, const char *key,
                   const void *imageData, size_t size) {
    /* Try to allocate from bounded pool */
    void *data = memboundAlloc(cache->allocator, size);
    if (!data) {
        /* Out of cache memory - eviction needed */
        printf("Cache full, need to evict\n");
        return false;
    }

    /* Copy image into cache */
    memcpy(data, imageData, size);
    cache->entries[cache->count].key = strdup(key);
    cache->entries[cache->count].imageData = data;
    cache->entries[cache->count].size = size;
    cache->count++;

    return true;
}

void imageCacheFree(imageCache *cache) {
    for (size_t i = 0; i < cache->count; i++) {
        free(cache->entries[i].key);
        memboundFree(cache->allocator, cache->entries[i].imageData);
    }

    memboundShutdownSafe(cache->allocator);
    free(cache);
}

/* Usage */
imageCache *cache = imageCacheCreate(50 * 1024 * 1024); // 50 MB limit

uint8_t imageData[1024 * 1024]; // 1 MB image
if (!imageCacheAdd(cache, "photo.jpg", imageData, sizeof(imageData))) {
    printf("Failed to add image\n");
}

imageCacheFree(cache);
```

### Example 2: Process-Shared Pool

```c
/* Shared memory pool between parent and child */
membound *sharedPool = memboundCreate(10 * 1024 * 1024);

/* Allocate some data */
int *sharedCounter = memboundAlloc(sharedPool, sizeof(int));
*sharedCounter = 0;

pid_t pid = fork();
if (pid == 0) {
    /* Child process */
    for (int i = 0; i < 1000; i++) {
        __sync_fetch_and_add(sharedCounter, 1);
    }
    exit(0);
} else {
    /* Parent process */
    wait(NULL);
    printf("Counter value: %d\n", *sharedCounter); // Should be 1000

    memboundFree(sharedPool, sharedCounter);
    memboundShutdown(sharedPool);
}
```

### Example 3: Preventing Memory Exhaustion

```c
/* Server with memory limit per connection */
typedef struct connection {
    int fd;
    membound *allocator;
} connection;

connection *connectionCreate(int fd) {
    connection *conn = malloc(sizeof(*conn));
    conn->fd = fd;

    /* Each connection limited to 1 MB */
    conn->allocator = memboundCreate(1 * 1024 * 1024);

    return conn;
}

void *connectionAllocate(connection *conn, size_t size) {
    void *mem = memboundAlloc(conn->allocator, size);
    if (!mem) {
        /* Connection exceeded its memory limit */
        fprintf(stderr, "Connection %d exceeded memory limit\n", conn->fd);
        close(conn->fd);
    }
    return mem;
}

void connectionClose(connection *conn) {
    /* Verify no leaks */
    if (memboundCurrentAllocationCount(conn->allocator) > 0) {
        fprintf(stderr, "Warning: connection leaked memory\n");
    }

    memboundShutdown(conn->allocator);
    close(conn->fd);
    free(conn);
}
```

### Example 4: Testing Memory Pressure

```c
/* Simulate OOM conditions for testing */
void testOOM(void) {
    membound *limited = memboundCreate(1024); // Only 1 KB!

    /* This should succeed */
    void *small = memboundAlloc(limited, 256);
    assert(small != NULL);

    /* This should succeed */
    void *small2 = memboundAlloc(limited, 256);
    assert(small2 != NULL);

    /* This should fail - not enough space */
    void *tooBig = memboundAlloc(limited, 1024);
    assert(tooBig == NULL);
    printf("OOM handled correctly\n");

    memboundFree(limited, small);
    memboundFree(limited, small2);
    memboundShutdown(limited);
}
```

## Performance Characteristics

| Operation        | Complexity | Notes                                        |
| ---------------- | ---------- | -------------------------------------------- |
| Create           | O(n)       | n = pool size, must initialize control array |
| Alloc (best)     | O(1)       | When exact size available in freelist        |
| Alloc (worst)    | O(log k)   | k = size classes, must split blocks          |
| Free (best)      | O(1)       | No coalescing possible                       |
| Free (worst)     | O(log k)   | Full coalescing up to largest block          |
| Realloc (shrink) | O(1)       | Returns same pointer                         |
| Realloc (grow)   | O(n)       | Allocates new, copies n bytes, frees old     |
| Shutdown         | O(1)       | Just unmaps memory                           |

### Memory Overhead

```
Per-allocation overhead: 1 byte control + rounding to power-of-2

Example: 300-byte allocation
- Rounded to: 512 bytes (next power of 2)
- Control: 1 byte
- Waste: 212 bytes (41% internal fragmentation!)
- Actual overhead: 213 bytes

Best case: Requesting exactly power-of-2
- 256-byte request → 256 bytes allocated
- Overhead: 0 bytes

Worst case: Just over power-of-2
- 257-byte request → 512 bytes allocated
- Overhead: 255 bytes (99% waste!)
```

### Fragmentation Bounds

Robson's theorem guarantees bounded fragmentation:

```
N >= M * (1 + log2(n)/2) - n + 1

Where:
- N = total pool size
- M = maximum outstanding memory
- n = largest_alloc / smallest_alloc

Example:
- Pool: 10 MB (N = 10,485,760)
- Atom: 256 bytes (smallest)
- Max single alloc: 1 MB (n = 1,048,576/256 = 4,096)
- Required: M * (1 + log2(4096)/2) - 4096 + 1 = M * 7 - 4095

Maximum guaranteed M ≈ 1.5 MB of useful allocations
```

## Thread Safety

membound is **fully thread-safe** via mutex protection:

```c
/* All operations are atomic and thread-safe */
void workerThread(void *arg) {
    membound *shared = (membound *)arg;

    /* Safe: mutex protects allocation */
    void *data = memboundAlloc(shared, 1024);

    /* ... do work ... */

    /* Safe: mutex protects deallocation */
    memboundFree(shared, data);
}

/* Create shared allocator */
membound *shared = memboundCreate(10 * 1024 * 1024);

/* Launch multiple threads */
pthread_t threads[10];
for (int i = 0; i < 10; i++) {
    pthread_create(&threads[i], NULL, workerThread, shared);
}

/* Wait for completion */
for (int i = 0; i < 10; i++) {
    pthread_join(threads[i], NULL);
}

memboundShutdownSafe(shared);
```

**Mutex type**: `PTHREAD_PROCESS_SHARED` - works across fork()!

## Best Practices

### 1. Size Pool Appropriately

```c
/* BAD - pool too small, frequent failures */
membound *tiny = memboundCreate(1024);
for (int i = 0; i < 100; i++) {
    void *data = memboundAlloc(tiny, 512); // Fails after 2 iterations!
}

/* GOOD - size based on working set */
size_t maxObjects = 1000;
size_t objectSize = 1024;
size_t overhead = 2; // Account for fragmentation
membound *sized = memboundCreate(maxObjects * objectSize * overhead);
```

### 2. Free Memory Promptly

```c
/* BAD - leaks memory */
for (int i = 0; i < 1000; i++) {
    void *data = memboundAlloc(m, 1024);
    doSomething(data);
    // Never freed!
}

/* GOOD - free immediately after use */
for (int i = 0; i < 1000; i++) {
    void *data = memboundAlloc(m, 1024);
    doSomething(data);
    memboundFree(m, data);
}
```

### 3. Use Power-of-2 Sizes

```c
/* BAD - wastes space */
void *data = memboundAlloc(m, 300); // Allocates 512 bytes, wastes 212!

/* GOOD - request power-of-2 */
void *data = memboundAlloc(m, 256); // Allocates exactly 256 bytes
```

### 4. Check Allocation Failures

```c
/* BAD - assumes success */
void *data = memboundAlloc(m, size);
memset(data, 0, size); // CRASH if allocation failed!

/* GOOD - check return value */
void *data = memboundAlloc(m, size);
if (!data) {
    fprintf(stderr, "Out of memory\n");
    return ERROR;
}
memset(data, 0, size);
```

## Common Pitfalls

### 1. Forgetting Pool is Shared After Fork

```c
/* WRONG ASSUMPTION */
membound *m = memboundCreate(1024);
void *data = memboundAlloc(m, 256);

if (fork() == 0) {
    memboundFree(m, data); // Frees in BOTH processes!
    exit(0);
}

wait(NULL);
// 'data' is now invalid in parent too!
```

### 2. Growing Pool with Outstanding Allocations

```c
/* DANGEROUS - pointers may move! */
void *ptr1 = memboundAlloc(m, 1024);
memboundIncreaseSize(m, newSize); // Copies pool - ptr1 may be invalid!
// ptr1 now points to OLD (unmapped) memory!

/* SAFE - grow before allocating */
memboundIncreaseSize(m, newSize);
void *ptr1 = memboundAlloc(m, 1024);
```

### 3. Exceeding 1 GiB Limit

```c
/* FAILS - exceeds max */
void *huge = memboundAlloc(m, 2ULL * 1024 * 1024 * 1024); // 2 GB
assert(huge == NULL); // Always fails

/* Use multiple allocations instead */
void *chunks[4];
for (int i = 0; i < 4; i++) {
    chunks[i] = memboundAlloc(m, 512 * 1024 * 1024); // 512 MB each
}
```

### 4. Not Checking Shutdown Status

```c
/* BAD - may fail silently */
memboundShutdown(m); // Returns false if allocations exist

/* GOOD - check return */
if (!memboundShutdownSafe(m)) {
    fprintf(stderr, "Memory leak: %zu allocations remaining\n",
            memboundCurrentAllocationCount(m));
}
```

## Debugging

### Memory Dump (Debug Builds Only)

```c
#ifndef NDEBUG
/* Dump allocator state to file for debugging */
void memboundMemBoundDump(membound *m, const char *filename);

/* Usage */
memboundMemBoundDump(m, "membound-state.txt");
```

Output includes:

- Free list statistics per size class
- Total allocations, current usage
- Maximum usage statistics
- Fragmentation information

## See Also

- [fibbuf](FIBBUF.md) - Fibonacci growth strategy for dynamic buffers
- [jebuf](JEBUF.md) - Jemalloc size class calculator
- [Architecture Overview](../../ARCHITECTURE.md) - Memory management design

## Testing

Run the membound test suite:

```bash
./src/datakit-test test membound
```

The test suite validates:

- Allocation and freeing patterns
- Out-of-memory conditions
- Statistics tracking
- Thread safety (when enabled)
