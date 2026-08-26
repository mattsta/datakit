# FastMutex - High-Performance Mutex Implementation

## Overview

`FastMutex` provides a **high-performance mutex** implementation optimized for low-contention scenarios. It uses atomic operations for fast-path locking and falls back to pthread primitives only when necessary, resulting in significantly better performance than standard `pthread_mutex_t` in common cases.

**Key Features:**

- Fast-path locking using atomic compare-and-swap (one CAS when uncontended)
- Adaptive behavior: spins, then short-sleeps, then queues
- Condition variable support compatible with fast mutexes
- FIFO wait queue built from stack-allocated waiter nodes
- No allocation: a contended waiter's queue node lives on its own stack

**Header**: `fastmutex.h`

**Source**: `fastmutex.c`, `fastmutexTest.c`

**Platforms**: Linux, macOS, and the BSD family - anything with C11 atomics and
POSIX threads

**Origin**: Adapted from a jemalloc pull request
(<https://github.com/jemalloc/jemalloc/pull/1516>)

### Not a drop-in pthread replacement

`fastMutex` deliberately implements a subset of `pthread_mutex_t`:

- **No destroy function.** There is no `fastMutexDestroy` / `fastMutexCondDestroy`.
  A `fastMutex` is a plain atomic word with nothing to release; a `fastCond`
  owns a `pthread_mutex_t` that is never destroyed, so a long-lived program
  that creates and abandons condition variables leaks that pthread mutex.
- **No condition broadcast.** Only `fastMutexCondSignal` (wake one) exists.
  Wake-all patterns must signal in a loop, one wake per known waiter.
- **No attributes, no recursion, no error checking.** Re-locking from the
  owning thread deadlocks; unlocking an unheld mutex is undefined.
- **No static initializer.** There is no `FASTMUTEX_INITIALIZER`; call
  `fastMutexInit()`.

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
- Waiter queue head pointer (remaining bits; waiter nodes are 4-byte aligned so
  the low two bits are free - the implementation asserts this)

The struct is one atomic word, so it is safe to embed directly in your own
structs and to zero-init via `fastMutexInit()`.

### fastCond

```c
typedef struct fastCond {
    pthread_mutex_t m;    /* Internal mutex for condition variable */
    fastWaiter *w;        /* Wait queue */
} fastCond;
```

Condition variable compatible with `fastMutex`. `fastWaiter` is opaque - its
definition lives in `fastmutex.c`.

## API Reference

> **Naming note:** the lock entry point is `fast_mutex_lock()` (snake_case),
> while everything else is camelCase (`fastMutexUnlock`, `fastMutexTryLock`, ...).
> This is the actual spelling in `fastmutex.h`, inherited from the upstream
> patch; `fastMutexLock` does not exist.

The header also declares `fastMutexLockSlow()` and `fastMutexUnlockSlow()`.
These are the out-of-line slow paths called by the inline fast paths. They are
public only because the inline functions need them - do not call them directly.

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
 * Returns: true if lock acquired, false if already held
 *
 * Never blocks. Uses a STRONG compare-exchange, so a free lock is
 * reliably acquired with no spurious false negatives. Only acquires
 * when the word is fully zero (unlocked with no waiters), matching the
 * fast path of fast_mutex_lock().
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
 * A relaxed atomic load of bit 0. No ordering, no synchronization
 * guarantees, and the answer may be stale the instant it returns.
 * Useful for debugging/assertions only.
 */
static inline bool fastMutexIsLocked(fastMutex *m);
```

### Condition Variable Operations

#### Initialization

```c
/* Initialize a condition variable
 * c: pointer to fastCond
 * unused: ignored; exists for pthread_cond_init() signature compatibility
 *         (pass NULL)
 * Returns: 0 on success, the pthread_mutex_init() error code on failure
 *
 * There is no matching destroy function - see "Not a drop-in pthread
 * replacement" above.
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
 * ts: absolute timeout (CLOCK_REALTIME); NULL means wait forever, which
 *     is exactly what fastMutexCondWait() passes
 * Returns: 0 on success, ETIMEDOUT on timeout, other error on failure
 *
 * Enqueues the caller, unlocks the mutex, sleeps, and re-locks the mutex
 * before returning - on the timeout path too.
 *
 * Timeout race: if the timeout fires at the same moment a signal removes
 * this waiter from the queue, the timed-out thread cannot find its own
 * node, so it waits for the pending wake and returns 0 rather than
 * ETIMEDOUT. A signal is never lost.
 */
int fastMutexCondTimedWait(fastCond *c, fastMutex *m, struct timespec *ts);
```

#### Signal Operation

```c
/* Wake one thread waiting on condition variable
 * c: pointer to fastCond
 *
 * Wakes the queue head (FIFO). If no threads are waiting, has no effect -
 * the signal is NOT remembered for a future waiter.
 * Does not require holding the associated mutex.
 *
 * There is no broadcast. To wake N waiters, call this N times.
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

void workQueueShutdown(WorkQueue *wq, size_t workerCount) {
    fast_mutex_lock(&wq->lock);
    wq->shutdown = true;
    fastMutexUnlock(&wq->lock);

    /* There is no broadcast: signal once per worker so every one wakes.
     * Extra signals are harmless - a signal with no waiter is a no-op. */
    for (size_t i = 0; i < workerCount; i++) {
        fastMutexCondSignal(&wq->hasWork);
    }
}
```

## Implementation Details

### Locking Algorithm

**Fast Path (Uncontended):**

1. Attempt a weak atomic CAS from 0 → 1 with acquire ordering
2. If it succeeds, the lock is held and no function call was made

**Slow Path (Contended, `fastMutexLockSlow`):**

1. **Retry the acquire**: CAS the lock bit on, preserving any waiter pointer
2. **Adaptive Spin**: While the lock reads locked, spin; after 40 iterations
   start `usleep(100)` between checks; after 100 iterations give up spinning
3. **Take the queue lock**: CAS bit 1 on (retry the whole loop if the lock has
   since been released, or if someone else holds the queue lock)
4. **Enqueue**: Append a `fastWaiter` allocated _on this thread's stack_ to the
   tail of the FIFO queue, then publish the new head and release the queue lock
5. **Sleep**: Block on that waiter's own pthread condition variable
6. **Wake**: The unlocking thread pops the head waiter and signals it

**Unlock (`fastMutexUnlockSlow`):**

1. Spin until the queue lock is free, then CAS it on
2. Pop the head waiter, splice the tail pointer onto its successor
3. Store the successor as the new word - this releases both the lock bit and
   the queue lock in one store
4. Signal the popped waiter's condition variable

### Memory Layout

The 64-bit `word` field encodes:

- **Bit 0**: Lock state (0 = unlocked, 1 = locked)
- **Bit 1**: Queue lock (for modifying waiter queue)
- **Bits 2-63**: Pointer to the waiter queue head (or 0 if no waiters)

Waiter nodes are stack objects, and the code asserts their 4-byte alignment so
the two tag bits are always available.

### Fairness

The wait queue is **FIFO**, but this is not a strict handoff:

- Waiters are appended at the tail and woken from the head
- Unlock **releases the lock before** signalling the woken waiter, so a thread
  arriving on the fast path can barge in and take the lock first
- The woken waiter then re-enters the slow path and re-queues

So wake _order_ is fair; lock _acquisition_ order is not guaranteed. Under
sustained heavy contention a queued waiter can be overtaken repeatedly.

### Performance Characteristics

No benchmark numbers are published for this module. Qualitatively:

| Operation | Uncontended                   | Contended                                  |
| --------- | ----------------------------- | ------------------------------------------ |
| Lock      | one atomic CAS, fully inlined | spin, then `usleep(100)` polls, then queue |
| Unlock    | one atomic CAS, fully inlined | queue-lock CAS + one `pthread_cond_signal` |
| TryLock   | one atomic CAS, never blocks  | one failed CAS, never blocks               |
| IsLocked  | one relaxed load              | one relaxed load                           |

The design targets the uncontended case: the fast paths are `static inline` and
make no function call at all. Note the contended path uses a 100μs sleep rather
than a futex, so a lock held longer than the spin budget costs at least that
much latency. Measure your own workload with the cmake `perf` preset before
choosing this over `pthread_mutex_t`.

### Adaptive Spinning

The implementation spins before queueing, to avoid the queue cost when the
holder is about to release:

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
- Iterations 41-100: `usleep(100)` between checks
- After 100: Give up and enqueue on the pthread wait queue

`sleeptime` is not reset between outer-loop passes, so a thread that loops back
around (because it lost the queue-lock race) resumes at its previous count and
goes straight to the queue.

## Condition Variable Implementation

Condition variables use a separate waiter queue from the mutex:

```c
struct fastCond {
    pthread_mutex_t m;    /* Protects waiter queue */
    fastWaiter *w;        /* FIFO waiter queue */
};
```

The `pthread_mutex_t` inside `fastCond` protects only that waiter queue; it is
never held while the caller's `fastMutex` is held.

**Wait Process:**

1. Append a stack-allocated waiter to the condition's FIFO queue (under `c->m`)
2. Unlock the `fastMutex`
3. Sleep on that waiter's own pthread condition variable
4. On timeout, unlink the waiter from the queue
5. Re-lock the `fastMutex` before returning

**Signal Process:**

1. Pop the head waiter from the queue (under `c->m`)
2. Clear its `waiting` flag and signal its condition variable

## Platform Support

| Platform   | Support | Atomics | Notes                                  |
| ---------- | ------- | ------- | -------------------------------------- |
| Linux      | ✓       | C11     | Full support                           |
| macOS      | ✓       | C11     | Full support                           |
| BSD family | ✓       | C11     | FreeBSD / NetBSD / OpenBSD / DragonFly |

There is no Windows support in datakit.

**Requirements:**

- C11 atomics (`<stdatomic.h>`)
- POSIX threads (`pthread.h`)
- 64-bit atomic operations
- Pointers that fit in 62 bits with 4-byte alignment (asserted at runtime)

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
- jemalloc pull request 1516 - the origin of this implementation
- Futex-based locking on Linux

## Testing

The module has a dedicated test registered as `fastmutex` (source:
`fastmutexTest.c`, built under `DATAKIT_TEST`, entry point
`int fastmutexTest(int argc, char *argv[])`). It covers trylock semantics,
plain lock/unlock, and mutual exclusion under contention across 8 threads.

The shape of those tests:

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
