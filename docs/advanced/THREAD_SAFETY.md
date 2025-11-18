# Thread Safety and Concurrent Usage Patterns

## Overview

Most datakit containers are **not thread-safe by default**. This is an intentional design decision to maximize single-threaded performance. However, datakit provides several patterns and modules for safe concurrent access when needed.

## General Thread Safety Model

### Default: Not Thread-Safe

```c
/* DEFAULT: Most containers are NOT thread-safe */
multimap *m = multimapNew(2);

/* UNSAFE: Concurrent access from multiple threads */
void thread1(void *arg) {
    multimapInsert(&m, key1, klen1, val1, vlen1);  // DANGER
}

void thread2(void *arg) {
    multimapInsert(&m, key2, klen2, val2, vlen2);  // DANGER
}

/* Result: Data corruption, crashes, undefined behavior */
```

### Thread-Safe Modules

Only these modules have internal synchronization:

| Module | Thread-Safe | Notes |
|--------|------------|-------|
| **membound** | Yes | Mutex-protected allocations |
| **fastmutex** | Yes | Mutex implementation itself |
| **OSRegulate** | Yes | Process resource limiting |
| **All others** | No | Require external synchronization |

## Synchronization Patterns

### Pattern 1: External Locking (Simplest)

Use mutex to protect all operations:

```c
#include <pthread.h>

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
multimap *shared_map = NULL;

void init_map(void) {
    multimapNew(&shared_map);
}

void thread_safe_insert(const void *key, size_t klen,
                       const void *val, size_t vlen) {
    pthread_mutex_lock(&lock);
    multimapInsert(&shared_map, key, klen, val, vlen);
    pthread_mutex_unlock(&lock);
}

bool thread_safe_lookup(const void *key, size_t klen,
                       void **val, size_t *vlen) {
    pthread_mutex_lock(&lock);
    bool found = multimapLookup(shared_map, key, klen, val, vlen);
    pthread_mutex_unlock(&lock);
    return found;
}

void cleanup_map(void) {
    pthread_mutex_lock(&lock);
    multimapFree(shared_map);
    pthread_mutex_unlock(&lock);
}
```

**Advantages:**
- Simple to implement
- Guarantees safety
- No data races possible

**Disadvantages:**
- Serializes all access
- High contention under load
- No parallelism

### Pattern 2: Read-Write Locks

Allow concurrent reads, exclusive writes:

```c
#include <pthread.h>

pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;
multimap *shared_map = NULL;

/* Multiple readers can run concurrently */
bool concurrent_lookup(const void *key, size_t klen,
                      void **val, size_t *vlen) {
    pthread_rwlock_rdlock(&rwlock);
    bool found = multimapLookup(shared_map, key, klen, val, vlen);
    pthread_rwlock_unlock(&rwlock);
    return found;
}

/* Writers get exclusive access */
void exclusive_insert(const void *key, size_t klen,
                     const void *val, size_t vlen) {
    pthread_rwlock_wrlock(&rwlock);
    multimapInsert(&shared_map, key, klen, val, vlen);
    pthread_rwlock_unlock(&rwlock);
}
```

**Advantages:**
- Concurrent reads (no contention)
- Good for read-heavy workloads
- Better scalability than mutex

**Disadvantages:**
- Still serializes writes
- More complex than simple mutex
- Higher overhead per operation

### Pattern 3: Per-Thread Containers

No locking needed - each thread has its own container:

```c
#define MAX_THREADS 32

typedef struct ThreadData {
    pthread_t thread;
    multimap *local_map;
    int thread_id;
} ThreadData;

ThreadData threads[MAX_THREADS];

void worker_thread(void *arg) {
    ThreadData *data = (ThreadData *)arg;

    /* Each thread has its own map - no locking needed */
    for (int i = 0; i < 10000; i++) {
        multimapInsert(&data->local_map, ...);  // Thread-safe!
    }
}

void create_workers(int num_threads) {
    for (int i = 0; i < num_threads; i++) {
        threads[i].thread_id = i;
        multimapNew(&threads[i].local_map);

        pthread_create(&threads[i].thread, NULL,
                      worker_thread, &threads[i]);
    }
}

/* Merge results after threads complete */
void merge_results(void) {
    multimap *combined = NULL;
    multimapNew(&combined);

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i].thread, NULL);

        /* Copy data from local map to combined */
        multimapIterator *iter = multimapIterator(threads[i].local_map);
        /* ... iterate and insert into combined ... */

        multimapFree(threads[i].local_map);
    }
}
```

**Advantages:**
- Zero contention
- Maximum parallelism
- No locking overhead

**Disadvantages:**
- Higher memory usage (N copies)
- Must merge results
- Not suitable for shared state

### Pattern 4: Copy-on-Write

Readers use stale copy, writers create new version:

```c
#include <stdatomic.h>

typedef struct COWContainer {
    atomic_uintptr_t current;  /* Atomic pointer to multimap */
    pthread_mutex_t write_lock;
} COWContainer;

COWContainer *cowNew(void) {
    COWContainer *cow = malloc(sizeof(*cow));
    multimap *initial = NULL;
    multimapNew(&initial);
    atomic_init(&cow->current, (uintptr_t)initial);
    pthread_mutex_init(&cow->write_lock, NULL);
    return cow;
}

/* Readers don't need locks */
bool cowRead(COWContainer *cow, const void *key, size_t klen,
            void **val, size_t *vlen) {
    multimap *snapshot = (multimap *)atomic_load(&cow->current);
    return multimapLookup(snapshot, key, klen, val, vlen);
}

/* Writers copy-on-write */
void cowWrite(COWContainer *cow, const void *key, size_t klen,
             const void *val, size_t vlen) {
    pthread_mutex_lock(&cow->write_lock);

    /* Get current version */
    multimap *old = (multimap *)atomic_load(&cow->current);

    /* Create new version (copy + modify) */
    multimap *new = multimapDuplicate(old);
    multimapInsert(&new, key, klen, val, vlen);

    /* Atomically publish new version */
    atomic_store(&cow->current, (uintptr_t)new);

    pthread_mutex_unlock(&cow->write_lock);

    /* Old version still valid for existing readers */
    /* Need garbage collection mechanism */
}
```

**Advantages:**
- Lock-free reads
- Excellent for read-heavy workloads
- Consistent snapshots

**Disadvantages:**
- Expensive writes (copy entire structure)
- Memory overhead (multiple versions)
- Complex garbage collection

### Pattern 5: Sharding

Partition data across multiple containers:

```c
#define NUM_SHARDS 16

typedef struct ShardedMap {
    multimap *shards[NUM_SHARDS];
    pthread_mutex_t locks[NUM_SHARDS];
} ShardedMap;

ShardedMap *shardedMapNew(void) {
    ShardedMap *sm = malloc(sizeof(*sm));
    for (int i = 0; i < NUM_SHARDS; i++) {
        multimapNew(&sm->shards[i]);
        pthread_mutex_init(&sm->locks[i], NULL);
    }
    return sm;
}

/* Hash key to determine shard */
int getShard(const void *key, size_t klen) {
    uint32_t hash = xxHash32(key, klen, 0);
    return hash % NUM_SHARDS;
}

void shardedInsert(ShardedMap *sm, const void *key, size_t klen,
                  const void *val, size_t vlen) {
    int shard = getShard(key, klen);

    /* Lock only one shard */
    pthread_mutex_lock(&sm->locks[shard]);
    multimapInsert(&sm->shards[shard], key, klen, val, vlen);
    pthread_mutex_unlock(&sm->locks[shard]);
}

bool shardedLookup(ShardedMap *sm, const void *key, size_t klen,
                  void **val, size_t *vlen) {
    int shard = getShard(key, klen);

    pthread_mutex_lock(&sm->locks[shard]);
    bool found = multimapLookup(sm->shards[shard], key, klen, val, vlen);
    pthread_mutex_unlock(&sm->locks[shard]);

    return found;
}
```

**Advantages:**
- Reduces contention (N-way parallelism)
- Scales well with cores
- Balanced approach

**Disadvantages:**
- More complex than single lock
- May have hot shards
- Iteration requires locking all shards

## Thread-Safe Modules

### membound - Thread-Safe Allocator

```c
/* membound uses internal mutex for all operations */
membound *pool = memboundCreate(100 * 1024 * 1024);

/* Safe from multiple threads */
void worker_thread(void *arg) {
    membound *shared_pool = (membound *)arg;

    void *data1 = memboundAlloc(shared_pool, 1024);  // Thread-safe
    void *data2 = memboundAlloc(shared_pool, 2048);  // Thread-safe

    /* ... use data ... */

    memboundFree(shared_pool, data1);  // Thread-safe
    memboundFree(shared_pool, data2);  // Thread-safe
}

/* Launch multiple threads sharing the same pool */
pthread_t threads[10];
for (int i = 0; i < 10; i++) {
    pthread_create(&threads[i], NULL, worker_thread, pool);
}

for (int i = 0; i < 10; i++) {
    pthread_join(threads[i], NULL);
}

memboundShutdown(pool);
```

**Process-Shared Memory:**

membound uses `PTHREAD_PROCESS_SHARED` mutex, allowing use across fork():

```c
membound *pool = memboundCreate(10 * 1024 * 1024);
void *shared_data = memboundAlloc(pool, 1024);

pid_t pid = fork();
if (pid == 0) {
    /* Child process */
    void *child_data = memboundAlloc(pool, 512);  // Safe!
    memboundFree(pool, child_data);
    exit(0);
} else {
    /* Parent process */
    wait(NULL);
    memboundFree(pool, shared_data);
    memboundShutdown(pool);
}
```

### OSRegulate - Thread-Safe Resource Limits

```c
/* OSRegulate provides thread-safe resource limiting */
OSRegulate *reg = OSRegulateNew();

/* Safe from multiple threads */
void limited_operation(void *arg) {
    OSRegulate *regulator = (OSRegulate *)arg;

    if (OSRegulateAcquire(regulator)) {
        /* Perform rate-limited operation */
        do_work();

        OSRegulateRelease(regulator);
    } else {
        /* Rate limit exceeded */
        printf("Too many requests\n");
    }
}
```

## Common Concurrency Patterns

### Producer-Consumer Queue

```c
typedef struct ThreadSafeQueue {
    multilist *queue;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    size_t max_size;
    bool shutdown;
} ThreadSafeQueue;

ThreadSafeQueue *queueNew(size_t max_size) {
    ThreadSafeQueue *q = malloc(sizeof(*q));
    q->queue = multilistNew(FLEX_CAP_LEVEL_2048, 0);
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    q->max_size = max_size;
    q->shutdown = false;
    return q;
}

void queueProduce(ThreadSafeQueue *q, databox *item) {
    pthread_mutex_lock(&q->lock);

    /* Wait if queue is full */
    while (multilistCount(q->queue) >= q->max_size && !q->shutdown) {
        pthread_cond_wait(&q->not_full, &q->lock);
    }

    if (!q->shutdown) {
        mflexState *state = mflexStateCreate();
        multilistPushByTypeTail(&q->queue, state, item);
        mflexStateFree(state);

        pthread_cond_signal(&q->not_empty);
    }

    pthread_mutex_unlock(&q->lock);
}

bool queueConsume(ThreadSafeQueue *q, databox *item) {
    pthread_mutex_lock(&q->lock);

    /* Wait if queue is empty */
    while (multilistCount(q->queue) == 0 && !q->shutdown) {
        pthread_cond_wait(&q->not_empty, &q->lock);
    }

    bool success = false;
    if (multilistCount(q->queue) > 0) {
        mflexState *state = mflexStateCreate();
        multilistPopByTypeHead(&q->queue, state, item);
        mflexStateFree(state);
        success = true;

        pthread_cond_signal(&q->not_full);
    }

    pthread_mutex_unlock(&q->lock);
    return success;
}

void queueShutdown(ThreadSafeQueue *q) {
    pthread_mutex_lock(&q->lock);
    q->shutdown = true;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->lock);
}
```

### Concurrent Cache with Expiration

```c
typedef struct CacheEntry {
    databox key;
    databox value;
    time_t expires_at;
} CacheEntry;

typedef struct ConcurrentCache {
    multimap *data;
    pthread_rwlock_t lock;
    time_t default_ttl;
} ConcurrentCache;

ConcurrentCache *cacheNew(time_t ttl) {
    ConcurrentCache *cache = malloc(sizeof(*cache));
    multimapNew(&cache->data);
    pthread_rwlock_init(&cache->lock, NULL);
    cache->default_ttl = ttl;
    return cache;
}

void cacheSet(ConcurrentCache *cache, databox *key, databox *value) {
    pthread_rwlock_wrlock(&cache->lock);

    CacheEntry entry = {
        .key = *key,
        .value = *value,
        .expires_at = time(NULL) + cache->default_ttl
    };

    multimapInsert(&cache->data,
                  databoxBytes(key), key->len,
                  &entry, sizeof(entry));

    pthread_rwlock_unlock(&cache->lock);
}

bool cacheGet(ConcurrentCache *cache, databox *key, databox *value) {
    pthread_rwlock_rdlock(&cache->lock);

    void *data;
    size_t len;
    bool found = multimapLookup(cache->data,
                               databoxBytes(key), key->len,
                               &data, &len);

    bool valid = false;
    if (found) {
        CacheEntry *entry = (CacheEntry *)data;
        if (time(NULL) < entry->expires_at) {
            *value = entry->value;
            valid = true;
        }
    }

    pthread_rwlock_unlock(&cache->lock);

    /* Clean up expired entry if found but invalid */
    if (found && !valid) {
        pthread_rwlock_wrlock(&cache->lock);
        multimapDelete(&cache->data, databoxBytes(key), key->len);
        pthread_rwlock_unlock(&cache->lock);
    }

    return valid;
}
```

### Parallel Processing with Result Aggregation

```c
typedef struct WorkItem {
    int id;
    void *input_data;
    size_t input_size;
} WorkItem;

typedef struct WorkerThread {
    pthread_t thread;
    int thread_id;
    multilist *work_queue;
    multiarray *results;
    pthread_mutex_t *queue_lock;
    pthread_cond_t *work_available;
    bool *shutdown;
} WorkerThread;

void *worker_function(void *arg) {
    WorkerThread *worker = (WorkerThread *)arg;
    mflexState *state = mflexStateCreate();

    while (true) {
        pthread_mutex_lock(worker->queue_lock);

        /* Wait for work */
        while (multilistCount(worker->work_queue) == 0 && !(*worker->shutdown)) {
            pthread_cond_wait(worker->work_available, worker->queue_lock);
        }

        if (*worker->shutdown && multilistCount(worker->work_queue) == 0) {
            pthread_mutex_unlock(worker->queue_lock);
            break;
        }

        /* Get work item */
        databox work_box;
        multilistPopByTypeHead(&worker->work_queue, state, &work_box);
        pthread_mutex_unlock(worker->queue_lock);

        /* Process work (no lock needed) */
        WorkItem *work = (WorkItem *)databoxBytes(&work_box);
        void *result = process_work(work);

        /* Store result in thread-local array (no lock needed) */
        multiarrayInsert(worker->results, multilistCount(worker->results),
                        &result, sizeof(result));
    }

    mflexStateFree(state);
    return NULL;
}

/* Main coordinator */
void parallel_process(WorkItem *items, int count, int num_threads) {
    multilist *work_queue = multilistNew(FLEX_CAP_LEVEL_4096, 0);
    pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t work_available = PTHREAD_COND_INITIALIZER;
    bool shutdown = false;

    /* Create worker threads */
    WorkerThread workers[num_threads];
    for (int i = 0; i < num_threads; i++) {
        workers[i].thread_id = i;
        workers[i].work_queue = work_queue;
        workers[i].results = multiarrayMediumNew(sizeof(void *), 1024);
        workers[i].queue_lock = &queue_lock;
        workers[i].work_available = &work_available;
        workers[i].shutdown = &shutdown;

        pthread_create(&workers[i].thread, NULL, worker_function, &workers[i]);
    }

    /* Distribute work */
    mflexState *state = mflexStateCreate();
    for (int i = 0; i < count; i++) {
        pthread_mutex_lock(&queue_lock);

        databox work_box = databoxNewBytes(&items[i], sizeof(WorkItem));
        multilistPushByTypeTail(&work_queue, state, &work_box);

        pthread_cond_signal(&work_available);
        pthread_mutex_unlock(&queue_lock);
    }

    /* Signal shutdown */
    pthread_mutex_lock(&queue_lock);
    shutdown = true;
    pthread_cond_broadcast(&work_available);
    pthread_mutex_unlock(&queue_lock);

    /* Collect results */
    for (int i = 0; i < num_threads; i++) {
        pthread_join(workers[i].thread, NULL);

        /* Aggregate results from each worker */
        for (size_t j = 0; j < multiarrayCount(workers[i].results); j++) {
            void **result_ptr = multiarrayGet(workers[i].results, j);
            process_result(*result_ptr);
        }

        multiarrayFree(workers[i].results);
    }

    mflexStateFree(state);
    multilistFree(work_queue);
}
```

## Best Practices

### 1. Choose Appropriate Locking Granularity

```c
/* COARSE: One lock for entire structure (simple, high contention) */
pthread_mutex_t global_lock;
multimap *map;

/* MEDIUM: Sharded locks (balanced) */
#define SHARDS 16
pthread_mutex_t locks[SHARDS];
multimap *shards[SHARDS];

/* FINE: Lock-free per-thread (complex, zero contention) */
__thread multimap *thread_local_map;
```

### 2. Hold Locks for Minimum Time

```c
/* BAD: Lock held during expensive operation */
pthread_mutex_lock(&lock);
databox result;
multimapLookup(map, key, klen, &result, &rlen);
expensive_processing(&result);  // Lock still held!
pthread_mutex_unlock(&lock);

/* GOOD: Release lock before expensive operation */
pthread_mutex_lock(&lock);
databox result;
multimapLookup(map, key, klen, &result, &rlen);
databox copy = databoxCopy(&result);
pthread_mutex_unlock(&lock);

expensive_processing(&copy);  // Lock released
```

### 3. Avoid Deadlocks

```c
/* BAD: Different lock order in different functions */
void functionA() {
    pthread_mutex_lock(&lock1);
    pthread_mutex_lock(&lock2);  // Order: 1, 2
    /* ... */
    pthread_mutex_unlock(&lock2);
    pthread_mutex_unlock(&lock1);
}

void functionB() {
    pthread_mutex_lock(&lock2);
    pthread_mutex_lock(&lock1);  // Order: 2, 1 - DEADLOCK!
    /* ... */
    pthread_mutex_unlock(&lock1);
    pthread_mutex_unlock(&lock2);
}

/* GOOD: Consistent lock ordering */
void functionA() {
    pthread_mutex_lock(&lock1);
    pthread_mutex_lock(&lock2);  // Order: 1, 2
    /* ... */
    pthread_mutex_unlock(&lock2);
    pthread_mutex_unlock(&lock1);
}

void functionB() {
    pthread_mutex_lock(&lock1);  // Order: 1, 2
    pthread_mutex_lock(&lock2);
    /* ... */
    pthread_mutex_unlock(&lock2);
    pthread_mutex_unlock(&lock1);
}
```

### 4. Use Condition Variables for Signaling

```c
/* BAD: Busy-wait polling */
while (multilistCount(queue) == 0) {
    usleep(1000);  // Wastes CPU
}

/* GOOD: Condition variable */
pthread_mutex_lock(&lock);
while (multilistCount(queue) == 0) {
    pthread_cond_wait(&not_empty, &lock);  // Efficient wait
}
pthread_mutex_unlock(&lock);
```

### 5. Consider Read-Heavy vs Write-Heavy

```c
/* Read-heavy: Use rwlock */
pthread_rwlock_t rwlock;
// Multiple concurrent readers, exclusive writers

/* Write-heavy: Use mutex */
pthread_mutex_t mutex;
// Simpler, less overhead

/* Balanced: Use sharding */
// Reduces contention for both
```

## Performance Considerations

### Lock Contention Impact

```c
/* Single mutex (high contention) */
// Throughput: ~100K ops/sec across 8 threads

/* Sharded locks (medium contention) */
// Throughput: ~600K ops/sec across 8 threads

/* Per-thread (no contention) */
// Throughput: ~2M ops/sec across 8 threads
```

### Read-Write Lock Performance

```c
/* 90% reads, 10% writes */
Mutex:   ~200K ops/sec
RWLock:  ~800K ops/sec (4x improvement)

/* 50% reads, 50% writes */
Mutex:   ~150K ops/sec
RWLock:  ~180K ops/sec (marginal improvement)

/* 10% reads, 90% writes */
Mutex:   ~120K ops/sec
RWLock:  ~100K ops/sec (slower due to overhead)
```

## See Also

- [PERFORMANCE.md](PERFORMANCE.md) - Performance optimization
- [membound](../modules/memory/MEMBOUND.md) - Thread-safe allocator
- [OSRegulate](../modules/system/OS_REGULATE.md) - Thread-safe resource limiting
- [fastmutex](../modules/system/FASTMUTEX.md) - High-performance mutex
