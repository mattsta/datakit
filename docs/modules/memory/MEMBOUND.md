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
- **O(1) freelist lookup** via bitmap + CTZ instruction
- **SIMD-accelerated zeroing** for calloc (AVX-512/AVX2/SSE2/NEON)
- **Fast size class computation** via CLZ/BSR intrinsics

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
    uint32_t atomShift;     /* log2(szAtom) for fast division via bit shift */
    uint64_t size;          /* Total byte extent of 'zPool' */
    int64_t nBlock;         /* Number of szAtom sized blocks in zPool */

    /* Thread safety */
    pthread_mutex_t mutex;  /* Mutex for thread-safe access */

    /* Performance statistics (64-bit for large pools) */
    uint64_t nAlloc;        /* Total number of malloc calls */
    uint64_t totalAlloc;    /* Total of all malloc calls (including internal frag) */
    uint64_t totalExcess;   /* Total internal fragmentation */
    uint64_t currentOut;    /* Current bytes checked out (including frag) */
    uint64_t currentCount;  /* Current number of distinct allocations */
    uint64_t maxOut;        /* Maximum instantaneous currentOut */
    uint64_t maxCount;      /* Maximum instantaneous currentCount */

    /* Free lists - one per power-of-two size class */
    int32_t aiFreelist[31]; /* Free block lists by size class */

    /* Bitmap for O(1) freelist lookup - bit i set if aiFreelist[i] has blocks */
    uint64_t freelistBitmap;

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
/* Increase pool size (only works when pool is empty!)
 * m: membound allocator
 * size: new total size (must be larger than current)
 * Returns: true on success, false if allocations exist or growth fails
 *
 * IMPORTANT: Can only be called when there are NO outstanding allocations.
 * All existing pointers would become invalid, so this is enforced.
 */
bool memboundIncreaseSize(membound *m, size_t size);

/* Example: Grow from 10 MB to 20 MB */
/* First, ensure pool is empty */
if (memboundCurrentAllocationCount(m) > 0) {
    fprintf(stderr, "Cannot grow - allocations exist\n");
}
if (!memboundIncreaseSize(m, 20 * 1024 * 1024)) {
    fprintf(stderr, "Failed to grow allocator\n");
}
```

### Memory Allocation

```c
/* Allocate memory from pool
 * size: bytes to allocate (max 1 GiB)
 * Returns: pointer to memory, or NULL if insufficient space
 */
void *memboundAlloc(membound *m, size_t size);

/* Allocate zero-initialized memory (like calloc)
 * count: number of elements
 * size: size of each element
 * Returns: pointer to zeroed memory, or NULL on failure/overflow
 */
void *memboundCalloc(membound *m, size_t count, size_t size);

/* Free previously allocated memory
 * p: pointer returned by memboundAlloc (NULL is safely ignored)
 */
void memboundFree(membound *m, void *p);

/* Reallocate memory (resize existing allocation)
 * p: existing allocation (NULL acts like alloc)
 * newlen: new size in bytes (any positive value, 0 acts like free)
 * Returns: pointer to resized memory, or NULL on failure
 * Note: On failure, original allocation is preserved
 */
void *memboundRealloc(membound *m, void *p, size_t newlen);

/* Example: Basic allocation */
void *data = memboundAlloc(m, 1024);
if (!data) {
    fprintf(stderr, "Out of memory\n");
    return;
}

/* Use the memory */
memset(data, 0, 1024);

/* Free when done (NULL-safe) */
memboundFree(m, data);

/* Example: Calloc for arrays */
int *numbers = memboundCalloc(m, 100, sizeof(int));
/* All 100 integers are zero-initialized */
```

### Statistics and Monitoring

```c
/* Get current number of outstanding allocations
 * Returns: count of allocations not yet freed
 */
size_t memboundCurrentAllocationCount(const membound *m);

/* Get total bytes currently allocated (including internal fragmentation)
 * Returns: bytes in use
 */
size_t memboundBytesUsed(const membound *m);

/* Get approximate bytes available for new allocations
 * Returns: bytes free (actual may vary due to fragmentation)
 */
size_t memboundBytesAvailable(const membound *m);

/* Get total pool capacity in usable bytes
 * Returns: total capacity
 */
size_t memboundCapacity(const membound *m);

/* Check if a pointer belongs to this pool's memory region
 * Returns: true if pointer is within pool bounds
 */
bool memboundOwns(const membound *m, const void *p);

/* Reset allocator to initial state (frees all memory)
 * WARNING: Invalidates all outstanding pointers!
 * Useful for arena-style "bulk free" patterns.
 */
void memboundReset(membound *m);

/* Example: Check for leaks */
size_t outstanding = memboundCurrentAllocationCount(m);
if (outstanding > 0) {
    fprintf(stderr, "Memory leak: %zu allocations not freed\n", outstanding);
}

/* Example: Monitor memory pressure */
size_t used = memboundBytesUsed(m);
size_t capacity = memboundCapacity(m);
double utilization = (double)used / capacity * 100.0;
printf("Pool utilization: %.1f%% (%zu / %zu bytes)\n",
       utilization, used, capacity);

/* Example: Validate pointer ownership */
void *ptr = memboundAlloc(m, 256);
assert(memboundOwns(m, ptr));      /* true - from this pool */
int stackVar;
assert(!memboundOwns(m, &stackVar)); /* false - stack variable */
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

### Example 5: Per-Task Memory Pools (Arena Allocation)

```c
/* Async task with dedicated memory pool - perfect for arena allocation pattern.
 * When task completes, just destroy the pool - no individual frees needed! */

typedef struct asyncTask {
    membound *pool;        /* Task-private memory pool */
    void (*work)(struct asyncTask *);
    void *userData;
} asyncTask;

asyncTask *asyncTaskCreate(size_t memoryBudget) {
    asyncTask *task = malloc(sizeof(*task));
    if (!task) return NULL;

    /* Each task gets its own bounded memory pool */
    task->pool = memboundCreate(memoryBudget);
    if (!task->pool) {
        free(task);
        return NULL;
    }

    return task;
}

/* Allocate from task's private pool */
void *asyncTaskAlloc(asyncTask *task, size_t size) {
    return memboundAlloc(task->pool, size);
}

void *asyncTaskCalloc(asyncTask *task, size_t count, size_t size) {
    return memboundCalloc(task->pool, count, size);
}

/* Check task memory usage */
void asyncTaskPrintStats(asyncTask *task) {
    printf("Task memory: %zu / %zu bytes (%.1f%% used)\n",
           memboundBytesUsed(task->pool),
           memboundCapacity(task->pool),
           100.0 * memboundBytesUsed(task->pool) / memboundCapacity(task->pool));
}

/* Destroy task - all allocations freed automatically! */
void asyncTaskDestroy(asyncTask *task) {
    /* Option 1: Just destroy - all memory released at once */
    memboundShutdown(task->pool);

    /* Option 2: Verify no unexpected leaks first (debugging)
    if (!memboundShutdownSafe(task->pool)) {
        fprintf(stderr, "Task leaked %zu allocations!\n",
                memboundCurrentAllocationCount(task->pool));
        memboundShutdown(task->pool);  // Force cleanup anyway
    }
    */

    free(task);
}

/* Usage example */
void processRequest(void) {
    /* Create task with 1 MB budget */
    asyncTask *task = asyncTaskCreate(1 * 1024 * 1024);

    /* All allocations come from task's private pool */
    char *buffer = asyncTaskAlloc(task, 4096);
    int *results = asyncTaskCalloc(task, 1000, sizeof(int));
    /* ... do work ... */

    /* Task complete - entire pool freed at once! */
    /* No need to track or free individual allocations */
    asyncTaskDestroy(task);
}
```

**Benefits for async tasks:**

1. **Isolation** - Each task's memory is completely separate
2. **Bounded** - Task cannot exceed its memory budget
3. **Fast cleanup** - Destroy pool = instant cleanup of all allocations
4. **No leaks** - Pool destruction guarantees all memory is freed
5. **Monitoring** - Easy to track per-task memory usage

## Performance Characteristics

| Operation        | Complexity | Notes                                        |
| ---------------- | ---------- | -------------------------------------------- |
| Create           | O(n)       | n = pool size, must initialize control array |
| Alloc            | O(1)       | Bitmap + CTZ finds free block instantly      |
| Alloc + split    | O(log k)   | k = size classes, when splitting needed      |
| Free (best)      | O(1)       | No coalescing possible                       |
| Free (worst)     | O(log k)   | Full coalescing up to largest block          |
| Calloc           | O(n)       | Alloc + SIMD zeroing (n = allocation size)   |
| Realloc (shrink) | O(1)       | Returns same pointer                         |
| Realloc (grow)   | O(n)       | Allocates new, copies n bytes, frees old     |
| Shutdown         | O(1)       | Just unmaps memory                           |

### Optimization Details

**O(1) Freelist Lookup**: Instead of scanning 31 freelist slots to find a free block, the allocator maintains a 64-bit bitmap where bit *i* indicates if `aiFreelist[i]` has free blocks. Finding the first available size class is a single CTZ (count trailing zeros) instruction.

**Fast Size Class Computation**: Size class calculation uses CLZ (count leading zeros) on ARM64 or BSR (bit scan reverse) on x86-64, replacing the original loop with a single instruction.

**Shift-Based Division**: All divisions by `szAtom` use bit shifts via the precomputed `atomShift` field (since szAtom is always a power of 2). This replaces ~20-80 cycle divisions with 1-cycle shifts.

**SIMD Calloc Zeroing**: For allocations ≥256 bytes, `memboundCalloc()` uses explicit SIMD instructions (AVX-512, AVX2, SSE2, or NEON depending on platform) for faster zeroing than `memset()`.

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

## Optimal Usage Patterns

### Arena Allocation (Recommended)

The most efficient way to use membound is the **arena pattern**: allocate freely during a task, then bulk-free everything with `memboundReset()` when done.

```c
/* Per-request/per-task memory pool */
void handleRequest(Request *req) {
    membound *arena = memboundCreate(1 << 20);  /* 1 MB arena */

    /* Allocate freely - no need to track individual allocations */
    char *buffer = memboundAlloc(arena, 4096);
    Node *nodes = memboundCalloc(arena, 100, sizeof(Node));
    Result *result = memboundAlloc(arena, sizeof(Result));

    /* ... process request using arena memory ... */

    /* Single cleanup - all memory released instantly */
    memboundShutdown(arena);  /* Or memboundReset() to reuse */
}
```

**Why this is optimal:**
1. No individual free() calls needed during work
2. No fragmentation accumulates (reset clears everything)
3. Leak detection is trivial (check count before shutdown)
4. O(n) bulk free is faster than n individual frees

### Why Compaction Isn't Needed

The buddy allocator handles fragmentation through **coalescing**, not compaction:

1. **Automatic merging**: When you free a block, it automatically merges with its buddy if both are free, recursively up to the largest possible block

2. **Bounded fragmentation**: Robson's theorem mathematically guarantees fragmentation cannot exceed a known bound

3. **Arena reset eliminates fragmentation**: `memboundReset()` returns the pool to pristine state - this IS the "compaction" for task-based workloads

4. **Compaction would break pointers**: Moving allocated blocks invalidates all application pointers, requiring handle indirection - a fundamental API change

**If you're seeing fragmentation issues**, the solution is usually:
- Use `memboundReset()` between logical work units
- Size your pool larger (2x working set is a good rule)
- Use power-of-2 allocation sizes to minimize internal fragmentation

### Minimizing Internal Fragmentation

All allocations round up to the next power of 2. Choose sizes wisely:

```c
/* BAD: 50% wasted */
void *p = memboundAlloc(m, 300);   /* Rounds to 512, wastes 212 bytes */

/* GOOD: 0% wasted */
void *p = memboundAlloc(m, 256);   /* Exactly 256 bytes */

/* ACCEPTABLE: Design structs to power-of-2 sizes */
typedef struct {
    uint64_t id;
    uint64_t timestamp;
    char data[240];  /* Pad to 256 bytes total */
} __attribute__((packed)) Record;  /* sizeof = 256 */
```

### Pool Sizing Guidelines

```c
/* Rule of thumb: 2x expected peak working set */
size_t expectedPeak = maxObjects * avgObjectSize;
size_t poolSize = expectedPeak * 2;  /* Headroom for fragmentation */

/* For arena pattern: size for single task's lifetime */
size_t taskPoolSize = estimatedTaskMemory * 1.5;
```

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

### 2. Growing Pool Requires Empty Pool

```c
/* memboundIncreaseSize() now enforces safety - returns false if allocations exist */
void *ptr1 = memboundAlloc(m, 1024);
bool grew = memboundIncreaseSize(m, newSize);
// grew == false! Function refuses to grow with outstanding allocations.

/* CORRECT - free allocations first, then grow */
memboundFree(m, ptr1);
// Or use memboundReset(m) to free all at once
bool grew = memboundIncreaseSize(m, newSize); // Now succeeds
void *ptr2 = memboundAlloc(m, 1024);
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

The test suite (22 tests) validates:

- Basic allocation and freeing patterns
- Pool exhaustion and OOM handling
- Calloc zero-initialization and overflow protection
- Realloc grow/shrink semantics
- Reset (arena-style bulk free)
- Pool growth (memboundIncreaseSize)
- Safe shutdown with allocation detection
- Statistics tracking (bytes used/available/capacity)
- Pointer ownership checking
- NULL safety for all APIs
- Various allocation sizes (including non-power-of-2)
- Maximum allocation size (1 GiB limit)
- Fragmentation and coalescing
- **Stress test**: 100,000 rapid alloc/free cycles
- **SIMD calloc stress test**: Verifies zeroing at various sizes
