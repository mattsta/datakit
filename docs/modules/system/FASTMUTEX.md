# FastMutex - High-Performance Mutex Implementation

## Overview

`FastMutex` provides a **high-performance mutex** implementation optimized for low-contention scenarios. It uses atomic operations for fast-path locking and falls back to pthread primitives only when necessary, resulting in significantly better performance than standard `pthread_mutex_t` in common cases.

**Key Features:**

- Fast-path locking using atomic compare-and-swap (lock-free when uncontended)
- Automatic adaptive behavior (spins before sleeping)
- Condition variable support compatible with fast mutexes
- FIFO wait queue for fairness under contention
- Zero overhead when uncontended
- Drop-in replacement for pthread mutexes in most cases

**Header**: `fastmutex.h`

**Source**: `fastmutex.c`

**Platforms**: All platforms with C11 atomics and pthreads

**Origin**: Adapted from jemalloc's fast mutex implementation

## Data Types

### fastMutex

```c
typedef struct fastMutex {
    _Atomic uint64_t word;
} fastMutex;
```

The primary mutex type. Internally uses a 64-bit atomic word to store:

- Lock state (bit 0)
- Queue lock state (bit 1)
- Waiter queue pointer (remaining bits)

### fastCond

```c
typedef struct fastCond {
    pthread_mutex_t m;    /* Internal mutex for condition variable */
    fastWaiter *w;        /* Wait queue */
} fastCond;
```

Condition variable compatible with `fastMutex`.

## API Reference

### Mutex Operations

#### Initialization

```c
/* Initialize a fast mutex
 * m: pointer to fastMutex
 *
 * Sets mutex to unlocked state
 * Must be called before first use
 */
static inline void fastMutexInit(fastMutex *m);
```

#### Locking

```c
/* Lock the mutex
 * m: pointer to fastMutex
 *
 * Blocks until lock is acquired
 * Fast path: single atomic CAS when uncontended
 * Slow path: adaptive spin then sleep
 */
static inline void fast_mutex_lock(fastMutex *m);

/* Try to lock the mutex without blocking
 * m: pointer to fastMutex
 * Returns: true if lock acquired, false if already locked
 *
 * Never blocks
 */
static inline bool fastMutexTryLock(fastMutex *m);
```

#### Unlocking

```c
/* Unlock the mutex
 * m: pointer to fastMutex
 *
 * Fast path: single atomic CAS when no waiters
 * Slow path: wake one waiting thread
 * Must be called by the thread that locked the mutex
 */
static inline void fastMutexUnlock(fastMutex *m);
```

#### Status Check

```c
/* Check if mutex is currently locked
 * m: pointer to fastMutex
 * Returns: true if locked, false if unlocked
 *
 * Non-blocking, no synchronization guarantees
 * Useful for debugging/assertions only
 */
static inline bool fastMutexIsLocked(fastMutex *m);
```

### Condition Variable Operations

#### Initialization

```c
/* Initialize a condition variable
 * c: pointer to fastCond
 * unused: reserved parameter (pass NULL)
 * Returns: 0 on success, error code on failure
 */
static inline int fastMutexCondInit(fastCond *c, void *unused);
```

#### Wait Operations

```c
/* Wait on condition variable
 * c: pointer to fastCond
 * m: pointer to fastMutex (must be locked by caller)
 * Returns: 0 on success, error code on failure
 *
 * Atomically unlocks mutex and waits
 * Re-locks mutex before returning
 */
int fastMutexCondWait(fastCond *c, fastMutex *m);

/* Wait on condition variable with timeout
 * c: pointer to fastCond
 * m: pointer to fastMutex (must be locked by caller)
 * ts: absolute timeout (CLOCK_REALTIME)
 * Returns: 0 on success, ETIMEDOUT on timeout, other error on failure
 *
 * Atomically unlocks mutex and waits
 * Re-locks mutex before returning
 */
int fastMutexCondTimedWait(fastCond *c, fastMutex *m, struct timespec *ts);
```

#### Signal Operation

```c
/* Wake one thread waiting on condition variable
 * c: pointer to fastCond
 *
 * If no threads waiting, has no effect
 * Does not require holding the associated mutex
 */
void fastMutexCondSignal(fastCond *c);
```

## Usage Examples

### Example 1: Basic Mutex Usage

```c
#include "fastmutex.h"

typedef struct Counter {
    fastMutex lock;
    uint64_t value;
} Counter;

void counterInit(Counter *c) {
    fastMutexInit(&c->lock);
    c->value = 0;
}

void counterIncrement(Counter *c) {
    fast_mutex_lock(&c->lock);
    c->value++;
    fastMutexUnlock(&c->lock);
}

uint64_t counterGet(Counter *c) {
    fast_mutex_lock(&c->lock);
    uint64_t val = c->value;
    fastMutexUnlock(&c->lock);
    return val;
}

bool counterTryIncrement(Counter *c) {
    if (fastMutexTryLock(&c->lock)) {
        c->value++;
        fastMutexUnlock(&c->lock);
        return true;
    }
    return false;
}
```

### Example 2: Condition Variable Producer-Consumer

```c
#include "fastmutex.h"
#include <stdlib.h>

typedef struct Queue {
    fastMutex lock;
    fastCond notEmpty;
    int *items;
    size_t head, tail, size, capacity;
} Queue;

void queueInit(Queue *q, size_t capacity) {
    fastMutexInit(&q->lock);
    fastMutexCondInit(&q->notEmpty, NULL);
    q->items = malloc(capacity * sizeof(int));
    q->head = q->tail = q->size = 0;
    q->capacity = capacity;
}

void queuePush(Queue *q, int item) {
    fast_mutex_lock(&q->lock);

    /* Wait if queue is full */
    while (q->size >= q->capacity) {
        fastMutexUnlock(&q->lock);
        /* In real code, you'd have a notFull condition variable */
        usleep(1000);
        fast_mutex_lock(&q->lock);
    }

    q->items[q->tail] = item;
    q->tail = (q->tail + 1) % q->capacity;
    q->size++;

    /* Signal that queue is not empty */
    fastMutexCondSignal(&q->notEmpty);

    fastMutexUnlock(&q->lock);
}

int queuePop(Queue *q) {
    fast_mutex_lock(&q->lock);

    /* Wait while queue is empty */
    while (q->size == 0) {
        fastMutexCondWait(&q->notEmpty, &q->lock);
    }

    int item = q->items[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->size--;

    fastMutexUnlock(&q->lock);
    return item;
}

int queuePopTimeout(Queue *q, int timeoutMs) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeoutMs / 1000;
    ts.tv_nsec += (timeoutMs % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000;
    }

    fast_mutex_lock(&q->lock);

    while (q->size == 0) {
        int ret = fastMutexCondTimedWait(&q->notEmpty, &q->lock, &ts);
        if (ret == ETIMEDOUT) {
            fastMutexUnlock(&q->lock);
            return -1;  /* Timeout */
        }
    }

    int item = q->items[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->size--;

    fastMutexUnlock(&q->lock);
    return item;
}
```

### Example 3: Shared Resource with Try-Lock

```c
#include "fastmutex.h"

typedef struct Cache {
    fastMutex lock;
    void *data;
    size_t size;
    uint64_t hits, misses;
} Cache;

void cacheInit(Cache *c) {
    fastMutexInit(&c->lock);
    c->data = NULL;
    c->size = 0;
    c->hits = c->misses = 0;
}

/* Fast lookup - try to get value without blocking */
bool cacheTryGet(Cache *c, void **dataOut) {
    if (!fastMutexTryLock(&c->lock)) {
        /* Someone else has the lock, count as miss */
        __atomic_add_fetch(&c->misses, 1, __ATOMIC_RELAXED);
        return false;
    }

    if (c->data) {
        *dataOut = c->data;
        c->hits++;
        fastMutexUnlock(&c->lock);
        return true;
    }

    c->misses++;
    fastMutexUnlock(&c->lock);
    return false;
}

/* Blocking lookup */
void *cacheGet(Cache *c) {
    fast_mutex_lock(&c->lock);
    void *data = c->data;
    if (data) {
        c->hits++;
    } else {
        c->misses++;
    }
    fastMutexUnlock(&c->lock);
    return data;
}

void cacheSet(Cache *c, void *data, size_t size) {
    fast_mutex_lock(&c->lock);
    c->data = data;
    c->size = size;
    fastMutexUnlock(&c->lock);
}
```

### Example 4: Critical Section Guards

```c
#include "fastmutex.h"

typedef struct Stats {
    fastMutex lock;
    uint64_t requests;
    uint64_t errors;
    uint64_t totalLatency;
} Stats;

void statsInit(Stats *s) {
    fastMutexInit(&s->lock);
    s->requests = 0;
    s->errors = 0;
    s->totalLatency = 0;
}

void statsRecordRequest(Stats *s, uint64_t latencyUs, bool error) {
    fast_mutex_lock(&s->lock);

    s->requests++;
    s->totalLatency += latencyUs;
    if (error) {
        s->errors++;
    }

    fastMutexUnlock(&s->lock);
}

void statsGetSnapshot(Stats *s, uint64_t *requests, uint64_t *errors,
                      double *avgLatency) {
    fast_mutex_lock(&s->lock);

    *requests = s->requests;
    *errors = s->errors;
    *avgLatency = s->requests > 0 ?
                  (double)s->totalLatency / s->requests : 0.0;

    fastMutexUnlock(&s->lock);
}

/* Check if lock is held (for debugging) */
void statsAssertLocked(Stats *s) {
    assert(fastMutexIsLocked(&s->lock));
}
```

### Example 5: Thread Pool Work Queue

```c
#include "fastmutex.h"
#include <pthread.h>

typedef struct Task {
    void (*func)(void *);
    void *arg;
    struct Task *next;
} Task;

typedef struct WorkQueue {
    fastMutex lock;
    fastCond hasWork;
    Task *head;
    Task *tail;
    bool shutdown;
} WorkQueue;

void workQueueInit(WorkQueue *wq) {
    fastMutexInit(&wq->lock);
    fastMutexCondInit(&wq->hasWork, NULL);
    wq->head = wq->tail = NULL;
    wq->shutdown = false;
}

void workQueuePush(WorkQueue *wq, void (*func)(void *), void *arg) {
    Task *task = malloc(sizeof(Task));
    task->func = func;
    task->arg = arg;
    task->next = NULL;

    fast_mutex_lock(&wq->lock);

    if (wq->tail) {
        wq->tail->next = task;
    } else {
        wq->head = task;
    }
    wq->tail = task;

    /* Wake one worker */
    fastMutexCondSignal(&wq->hasWork);

    fastMutexUnlock(&wq->lock);
}

void *workerThread(void *arg) {
    WorkQueue *wq = arg;

    while (1) {
        fast_mutex_lock(&wq->lock);

        /* Wait for work or shutdown */
        while (!wq->head && !wq->shutdown) {
            fastMutexCondWait(&wq->hasWork, &wq->lock);
        }

        if (wq->shutdown && !wq->head) {
            fastMutexUnlock(&wq->lock);
            break;
        }

        /* Dequeue task */
        Task *task = wq->head;
        wq->head = task->next;
        if (!wq->head) {
            wq->tail = NULL;
        }

        fastMutexUnlock(&wq->lock);

        /* Execute task outside lock */
        task->func(task->arg);
        free(task);
    }

    return NULL;
}

void workQueueShutdown(WorkQueue *wq) {
    fast_mutex_lock(&wq->lock);
    wq->shutdown = true;
    /* Wake all workers (would need condBroadcast for this) */
    fastMutexCondSignal(&wq->hasWork);
    fastMutexUnlock(&wq->lock);
}
```

## Implementation Details

### Locking Algorithm

**Fast Path (Uncontended):**

1. Attempt atomic CAS from 0 → 1
2. If successful, lock acquired (1-2 CPU cycles)

**Slow Path (Contended):**

1. **Adaptive Spin**: Spin up to 40-100 iterations checking lock state
2. **Sleep Preparation**: Set "has waiters" bit (bit 1)
3. **Queue Management**: Add thread to FIFO waiter queue
4. **Sleep**: Use pthread condition variable to sleep
5. **Wake**: When unlocked, first waiter is woken

### Memory Layout

The 64-bit `word` field encodes:

- **Bit 0**: Lock state (0 = unlocked, 1 = locked)
- **Bit 1**: Queue lock (for modifying waiter queue)
- **Bits 2-63**: Pointer to waiter queue (or 0 if no waiters)

### Fairness

FastMutex provides **FIFO fairness** under contention:

- Waiters form a queue
- Unlock wakes the first waiter
- Prevents starvation

### Performance Characteristics

| Operation | Uncontended | Low Contention | High Contention |
| --------- | ----------- | -------------- | --------------- |
| Lock      | ~5ns        | ~50ns          | ~1μs            |
| Unlock    | ~5ns        | ~50ns          | ~1μs            |
| TryLock   | ~5ns        | ~5ns           | ~5ns            |
| IsLocked  | ~1ns        | ~1ns           | ~1ns            |

**Comparison to pthread_mutex_t:**

- Uncontended: **10-100x faster**
- Low contention: **2-5x faster**
- High contention: **Similar performance**

### Adaptive Spinning

The implementation uses adaptive spinning to avoid syscalls:

```c
uint64_t sleeptime = 1;
while (true) {
    /* Try to acquire lock */
    if (can_acquire_lock()) return;

    /* Spin while locked */
    while (fastMutexIsLocked(m)) {
        if (sleeptime++ > 40) {
            usleep(100);  /* Short sleep */
        }
        if (sleeptime > 100) {
            break;  /* Give up spinning */
        }
    }

    /* Proceed to queue-based waiting */
    /* ... */
}
```

**Strategy:**

- Iterations 1-40: Busy-wait (expect quick unlock)
- Iterations 41-100: Sleep 100μs between checks
- After 100: Give up and use pthread wait queue

## Condition Variable Implementation

Condition variables use a separate waiter queue from the mutex:

```c
struct fastCond {
    pthread_mutex_t m;    /* Protects waiter queue */
    fastWaiter *w;        /* FIFO waiter queue */
};
```

**Wait Process:**

1. Add thread to condition's wait queue
2. Unlock the fastMutex
3. Sleep on pthread condition variable
4. On wake, re-lock the fastMutex

**Signal Process:**

1. Remove first waiter from queue
2. Wake that waiter's condition variable

## Platform Support

| Platform | Support | Atomics | Notes                             |
| -------- | ------- | ------- | --------------------------------- |
| Linux    | ✓       | C11     | Full support                      |
| macOS    | ✓       | C11     | Full support                      |
| FreeBSD  | ✓       | C11     | Full support                      |
| Windows  | Partial | C11     | Needs pthread-win32               |
| Others   | ✓       | C11     | Requires C11 atomics and pthreads |

**Requirements:**

- C11 atomics (`<stdatomic.h>`)
- POSIX threads (`pthread.h`)
- 64-bit atomic operations

## Best Practices

### 1. Use for Low-Contention Scenarios

```c
/* GOOD - Protecting fast operations */
fast_mutex_lock(&cache->lock);
value = cache->data[key];
fastMutexUnlock(&cache->lock);

/* BAD - Long critical sections */
fast_mutex_lock(&lock);
processLargeDataset();  /* Holds lock too long */
fastMutexUnlock(&lock);
```

### 2. Keep Critical Sections Short

```c
/* GOOD - Minimal lock holding */
fast_mutex_lock(&stats->lock);
uint64_t count = stats->count++;
fastMutexUnlock(&stats->lock);

processItem(count);  /* Outside lock */

/* BAD - Lock held during I/O */
fast_mutex_lock(&stats->lock);
stats->count++;
writeToFile(stats);  /* Slow I/O! */
fastMutexUnlock(&stats->lock);
```

### 3. Use TryLock for Optional Operations

```c
/* GOOD - Non-critical stats update */
if (fastMutexTryLock(&stats->lock)) {
    stats->optionalMetric++;
    fastMutexUnlock(&stats->lock);
}
/* Continue even if locked */

/* BAD - Blocking for optional work */
fast_mutex_lock(&stats->lock);
stats->optionalMetric++;  /* Blocks unnecessarily */
fastMutexUnlock(&stats->lock);
```

### 4. Don't Use IsLocked for Synchronization

```c
/* BAD - Race condition! */
if (!fastMutexIsLocked(&m)) {
    /* Lock could be acquired by another thread here! */
    accessSharedData();  /* UNSAFE! */
}

/* GOOD - Proper synchronization */
fast_mutex_lock(&m);
accessSharedData();
fastMutexUnlock(&m);
```

### 5. Always Initialize Before Use

```c
/* GOOD */
fastMutex lock;
fastMutexInit(&lock);
fast_mutex_lock(&lock);

/* BAD - Undefined behavior! */
fastMutex lock;  /* Not initialized */
fast_mutex_lock(&lock);  /* May crash or deadlock */
```

## Debugging

### Common Issues

**Deadlock:**

```c
/* BAD - Self-deadlock */
fast_mutex_lock(&m);
fast_mutex_lock(&m);  /* Deadlock! */
```

**Lock/Unlock Mismatch:**

```c
/* BAD - Unlock without lock */
fastMutexUnlock(&m);  /* Undefined behavior! */
```

**Forgot Unlock:**

```c
/* BAD - Leaked lock */
fast_mutex_lock(&m);
if (error) return;  /* Forgot to unlock! */
fastMutexUnlock(&m);
```

### Debug Assertions

```c
/* Add assertions in debug builds */
#ifndef NDEBUG
    #define ASSERT_LOCKED(m) assert(fastMutexIsLocked(m))
    #define ASSERT_UNLOCKED(m) assert(!fastMutexIsLocked(m))
#else
    #define ASSERT_LOCKED(m)
    #define ASSERT_UNLOCKED(m)
#endif

void processData(Data *d) {
    ASSERT_LOCKED(&d->lock);
    /* ... */
}
```

## Use Cases

1. **Low-Latency Applications**: Reduce locking overhead in hot paths
2. **Shared Counters**: Fast increment/decrement operations
3. **Cache Structures**: Protect cached data with minimal overhead
4. **Producer-Consumer Queues**: Fast enqueue/dequeue with condition variables
5. **Statistics Tracking**: Update metrics with minimal contention
6. **Reference Counting**: Protect reference count updates
7. **Resource Pools**: Manage shared resource allocation

## See Also

- `pthread_mutex_t` - Standard POSIX mutex
- Atomic operations (`<stdatomic.h>`)
- jemalloc fast mutex implementation
- Futex-based locking on Linux

## Testing

FastMutex can be tested with standard multithreading tests:

```c
#include "fastmutex.h"
#include <pthread.h>

/* Test basic locking */
void testBasicLock(void) {
    fastMutex m;
    fastMutexInit(&m);

    assert(!fastMutexIsLocked(&m));

    fast_mutex_lock(&m);
    assert(fastMutexIsLocked(&m));

    fastMutexUnlock(&m);
    assert(!fastMutexIsLocked(&m));
}

/* Test contention */
void *threadFunc(void *arg) {
    Counter *c = arg;
    for (int i = 0; i < 100000; i++) {
        counterIncrement(c);
    }
    return NULL;
}

void testContention(void) {
    Counter c;
    counterInit(&c);

    pthread_t threads[8];
    for (int i = 0; i < 8; i++) {
        pthread_create(&threads[i], NULL, threadFunc, &c);
    }

    for (int i = 0; i < 8; i++) {
        pthread_join(threads[i], NULL);
    }

    assert(counterGet(&c) == 800000);
}
```

Performance benchmarks should compare against `pthread_mutex_t` for various contention levels.
