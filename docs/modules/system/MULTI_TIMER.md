# MultiTimer - Multi-Timer Event Management

## Overview

`MultiTimer` provides a **scalable multi-timer event management system** for scheduling and executing timer callbacks. It efficiently manages thousands of concurrent timers with microsecond precision, supporting both one-shot and repeating timers.

**Key Features:**

- Efficient timer scheduling using sorted multimap structure
- One-shot and repeating timer support
- Microsecond precision timing
- Dynamic timer registration/unregistration during callbacks
- FIFO processing of expired timers
- Zero overhead for inactive timers
- Thread-safe with proper external synchronization

**Header**: `multiTimer.h`

**Source**: `multiTimer.c`, `multiTimerInternal.h`

**Dependencies**: `multimap`, `timeUtil`

**Platforms**: All platforms supported by multimap and timeUtil

## Core Concepts

### Timer Event

A timer event consists of:

- **Start Time**: When the timer should first fire (microseconds from now)
- **Repeat Interval**: How often to repeat (0 for one-shot)
- **Callback**: Function to call when timer fires
- **Client Data**: User data passed to callback
- **Timer ID**: Unique identifier for this timer

### Timer States

- **Scheduled**: Active timers waiting to fire
- **Pending**: Timers created during callback execution (will be scheduled after processing)
- **Stopped**: Timers marked for deletion (won't fire)

### Execution Context

- **USER**: Normal API calls from user code
- **TIMER**: Inside timer callback execution (restricts some operations)

## Data Types

### multiTimer

```c
typedef struct multiTimer multiTimer;
```

Opaque timer manager structure. Created with `multiTimerNew()` or initialized with `multiTimerInit()`.

### multiTimerId

```c
typedef uint64_t multiTimerId;
```

Unique identifier for each timer. Returned by `multiTimerRegister()` and used to unregister timers.

### Time Types

```c
typedef int64_t multiTimerUs;                    /* Microsecond duration */
typedef int64_t multiTimerSystemMonotonicUs;     /* Monotonic time in microseconds */
```

### Timer Callback

```c
typedef bool multiTimerCallback(multiTimer *t, multiTimerId id, void *clientData);
```

**Parameters:**

- `t`: Timer manager that triggered this callback
- `id`: Timer ID of the firing timer
- `clientData`: User data registered with timer

**Returns:**

- `true`: Reschedule repeating timer (if repeat interval > 0)
- `false`: Don't reschedule (one-shot timer or cancel repeat)

## API Reference

### Initialization and Cleanup

```c
/* Create new timer manager
 * Returns: allocated multiTimer or NULL on error
 *
 * Allocates and initializes timer manager
 */
multiTimer *multiTimerNew(void);

/* Initialize existing timer manager
 * t: pointer to multiTimer structure
 * Returns: true on success, false on error
 *
 * Must be called before using timer manager
 */
bool multiTimerInit(multiTimer *t);

/* Clean up timer manager resources
 * t: pointer to multiTimer
 *
 * Stops all timers and frees internal structures
 * Does not free the multiTimer structure itself
 */
void multiTimerDeinit(multiTimer *t);

/* Free timer manager
 * t: pointer to multiTimer
 *
 * Calls multiTimerDeinit() then frees structure
 */
void multiTimerFree(multiTimer *t);
```

### Timer Registration

```c
/* Register a new timer
 * t: timer manager
 * startAfterMicroseconds: delay before first execution
 * repeatEveryMicroseconds: interval for repeating (0 for one-shot)
 * cb: callback function to invoke
 * clientData: user data passed to callback
 * Returns: timer ID (non-zero) or 0 on error
 *
 * Can be called from within timer callbacks
 */
multiTimerId multiTimerRegister(
    multiTimer *t,
    uint64_t startAfterMicroseconds,
    uint64_t repeatEveryMicroseconds,
    multiTimerCallback *cb,
    void *clientData
);

/* Unregister a timer
 * t: timer manager
 * id: timer ID to cancel
 * Returns: true on success, false if ID not found
 *
 * Timer won't fire again (safe to call during callback)
 * May not take effect until after current processing completes
 */
bool multiTimerUnregister(multiTimer *t, multiTimerId id);

/* Stop all timers
 * t: timer manager
 * Returns: true on success
 *
 * Marks all active timers for deletion
 */
bool multiTimerStopAll(multiTimer *t);
```

### Timer Processing

```c
/* Process all expired timers
 * t: timer manager
 *
 * Executes callbacks for all timers that have reached their scheduled time
 * Should be called regularly from event loop
 */
void multiTimerProcessTimerEvents(multiTimer *t);
```

### Timer Queries

```c
/* Get count of active timers
 * t: timer manager
 * Returns: number of scheduled timers
 */
size_t multitimerCount(multiTimer *t);

/* Get absolute time of next timer
 * t: timer manager
 * Returns: monotonic time (microseconds) of next timer, or 0 if no timers
 */
multiTimerSystemMonotonicUs multiTimerNextTimerEventStartUs(multiTimer *t);

/* Get time until next timer
 * t: timer manager
 * Returns: microseconds until next timer, or 0 if no timers
 *
 * May be negative if timer is overdue
 */
multiTimerUs multiTimerNextTimerEventOffsetFromNowUs(multiTimer *t);
```

### Time Utilities

```c
/* Get current monotonic time in nanoseconds
 * Returns: monotonic time in nanoseconds
 */
multiTimerSystemMonotonicUs multiTimerGetNs(void);

/* Get current monotonic time in microseconds
 * Returns: monotonic time in microseconds
 */
multiTimerSystemMonotonicUs multiTimerGetUs(void);
```

## Usage Examples

### Example 1: Basic One-Shot Timer

```c
#include "multiTimer.h"
#include <stdio.h>

bool oneShotCallback(multiTimer *t, multiTimerId id, void *clientData) {
    const char *message = clientData;
    printf("Timer fired: %s\n", message);
    return false;  /* Don't reschedule */
}

int main(void) {
    multiTimer *timer = multiTimerNew();

    /* Fire after 5 seconds */
    multiTimerRegister(timer, 5000000, 0, oneShotCallback, "5 second timer");

    /* Event loop */
    while (multitimerCount(timer) > 0) {
        multiTimerProcessTimerEvents(timer);
        usleep(10000);  /* Check every 10ms */
    }

    multiTimerFree(timer);
    return 0;
}
```

### Example 2: Repeating Timer

```c
#include "multiTimer.h"

typedef struct Counter {
    int count;
    int maxCount;
} Counter;

bool repeatingCallback(multiTimer *t, multiTimerId id, void *clientData) {
    Counter *counter = clientData;
    counter->count++;

    printf("Timer tick: %d/%d\n", counter->count, counter->maxCount);

    if (counter->count >= counter->maxCount) {
        printf("Counter limit reached, stopping timer\n");
        return false;  /* Stop repeating */
    }

    return true;  /* Continue repeating */
}

void runRepeatingTimer(void) {
    multiTimer *timer = multiTimerNew();
    Counter counter = {0, 10};

    /* Start after 1 second, repeat every 500ms */
    multiTimerRegister(timer, 1000000, 500000, repeatingCallback, &counter);

    while (multitimerCount(timer) > 0) {
        multiTimerProcessTimerEvents(timer);
        usleep(10000);
    }

    multiTimerFree(timer);
}
```

### Example 3: Multiple Timers

```c
#include "multiTimer.h"

typedef struct TimerContext {
    const char *name;
    int fireCount;
} TimerContext;

bool namedTimerCallback(multiTimer *t, multiTimerId id, void *clientData) {
    TimerContext *ctx = clientData;
    ctx->fireCount++;

    printf("[%s] Timer %lu fired (count: %d)\n",
           ctx->name, id, ctx->fireCount);

    return ctx->fireCount < 5;  /* Fire 5 times total */
}

void runMultipleTimers(void) {
    multiTimer *timer = multiTimerNew();

    /* Create contexts */
    TimerContext fast = {"FAST", 0};
    TimerContext medium = {"MEDIUM", 0};
    TimerContext slow = {"SLOW", 0};

    /* Register timers with different intervals */
    multiTimerRegister(timer, 100000, 100000, namedTimerCallback, &fast);
    multiTimerRegister(timer, 500000, 500000, namedTimerCallback, &medium);
    multiTimerRegister(timer, 1000000, 1000000, namedTimerCallback, &slow);

    printf("Started 3 timers: fast (100ms), medium (500ms), slow (1s)\n");

    while (multitimerCount(timer) > 0) {
        multiTimerProcessTimerEvents(timer);
        usleep(10000);
    }

    multiTimerFree(timer);
}
```

### Example 4: Dynamic Timer Management

```c
#include "multiTimer.h"

typedef struct Worker {
    multiTimer *timer;
    multiTimerId healthCheckId;
    multiTimerId cleanupId;
    bool running;
} Worker;

bool healthCheckCallback(multiTimer *t, multiTimerId id, void *clientData) {
    Worker *worker = clientData;

    if (!performHealthCheck()) {
        printf("Health check failed, stopping worker\n");
        worker->running = false;
        multiTimerUnregister(t, worker->cleanupId);  /* Cancel cleanup */
        return false;
    }

    printf("Health check passed\n");
    return true;  /* Continue repeating */
}

bool cleanupCallback(multiTimer *t, multiTimerId id, void *clientData) {
    Worker *worker = clientData;

    printf("Running cleanup\n");
    performCleanup();

    return worker->running;  /* Continue if still running */
}

void workerStart(Worker *worker) {
    worker->timer = multiTimerNew();
    worker->running = true;

    /* Health check every 30 seconds */
    worker->healthCheckId = multiTimerRegister(
        worker->timer, 30000000, 30000000,
        healthCheckCallback, worker
    );

    /* Cleanup every 5 minutes */
    worker->cleanupId = multiTimerRegister(
        worker->timer, 300000000, 300000000,
        cleanupCallback, worker
    );

    printf("Worker started with 2 timers\n");
}

void workerStop(Worker *worker) {
    worker->running = false;
    multiTimerStopAll(worker->timer);
    multiTimerFree(worker->timer);
    printf("Worker stopped\n");
}
```

### Example 5: Timer in Event Loop

```c
#include "multiTimer.h"
#include <sys/select.h>

typedef struct Server {
    int listenFd;
    multiTimer *timer;
} Server;

bool periodicTaskCallback(multiTimer *t, multiTimerId id, void *clientData) {
    Server *server = clientData;

    printf("Running periodic maintenance\n");
    performMaintenance(server);

    return true;  /* Keep repeating */
}

void serverEventLoop(Server *server) {
    /* Register periodic task (every 60 seconds) */
    multiTimerRegister(server->timer, 60000000, 60000000,
                      periodicTaskCallback, server);

    while (server->running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server->listenFd, &readfds);

        /* Calculate timeout until next timer */
        struct timeval tv;
        int64_t nextUs = multiTimerNextTimerEventOffsetFromNowUs(server->timer);

        if (nextUs > 0) {
            tv.tv_sec = nextUs / 1000000;
            tv.tv_usec = nextUs % 1000000;
        } else {
            /* Timer overdue or no timers, check immediately */
            tv.tv_sec = 0;
            tv.tv_usec = 0;
        }

        int ready = select(server->listenFd + 1, &readfds, NULL, NULL, &tv);

        if (ready > 0) {
            /* Handle incoming connection */
            handleConnection(server);
        }

        /* Process any expired timers */
        multiTimerProcessTimerEvents(server->timer);
    }
}
```

### Example 6: Timeout Manager

```c
#include "multiTimer.h"

typedef struct TimeoutManager {
    multiTimer *timer;
} TimeoutManager;

typedef struct Request {
    uint64_t id;
    multiTimerId timerId;
    void *data;
} Request;

bool requestTimeoutCallback(multiTimer *t, multiTimerId id, void *clientData) {
    Request *req = clientData;

    printf("Request %lu timed out\n", req->id);
    cleanupRequest(req);

    return false;  /* One-shot timeout */
}

void timeoutManagerInit(TimeoutManager *tm) {
    tm->timer = multiTimerNew();
}

multiTimerId timeoutManagerAddRequest(TimeoutManager *tm, Request *req,
                                     uint64_t timeoutUs) {
    req->timerId = multiTimerRegister(
        tm->timer, timeoutUs, 0,
        requestTimeoutCallback, req
    );

    return req->timerId;
}

void timeoutManagerCancelRequest(TimeoutManager *tm, Request *req) {
    if (req->timerId) {
        multiTimerUnregister(tm->timer, req->timerId);
        req->timerId = 0;
    }
}

void timeoutManagerProcess(TimeoutManager *tm) {
    multiTimerProcessTimerEvents(tm->timer);
}

/* Example usage */
void handleRequest(TimeoutManager *tm) {
    Request *req = createRequest();

    /* Set 30-second timeout */
    timeoutManagerAddRequest(tm, req, 30000000);

    /* ... process request ... */

    if (requestCompleted) {
        /* Cancel timeout on success */
        timeoutManagerCancelRequest(tm, req);
    }
}
```

### Example 7: Rate Limiter with Timer

```c
#include "multiTimer.h"

typedef struct RateLimiter {
    multiTimer *timer;
    int tokensRemaining;
    int maxTokens;
    int refillRate;  /* Tokens per second */
} RateLimiter;

bool refillCallback(multiTimer *t, multiTimerId id, void *clientData) {
    RateLimiter *rl = clientData;

    /* Refill one token */
    if (rl->tokensRemaining < rl->maxTokens) {
        rl->tokensRemaining++;
        printf("Token refilled: %d/%d\n",
               rl->tokensRemaining, rl->maxTokens);
    }

    return true;  /* Continue refilling */
}

void rateLimiterInit(RateLimiter *rl, int maxTokens, int tokensPerSecond) {
    rl->timer = multiTimerNew();
    rl->tokensRemaining = maxTokens;
    rl->maxTokens = maxTokens;
    rl->refillRate = tokensPerSecond;

    /* Calculate refill interval */
    uint64_t refillIntervalUs = 1000000 / tokensPerSecond;

    /* Start refill timer */
    multiTimerRegister(rl->timer, refillIntervalUs, refillIntervalUs,
                      refillCallback, rl);
}

bool rateLimiterTryConsume(RateLimiter *rl) {
    /* Process any pending refills */
    multiTimerProcessTimerEvents(rl->timer);

    if (rl->tokensRemaining > 0) {
        rl->tokensRemaining--;
        return true;
    }

    return false;  /* Rate limited */
}
```

### Example 8: Cascading Timers

```c
#include "multiTimer.h"

typedef struct CascadeContext {
    multiTimer *timer;
    int stage;
} CascadeContext;

bool stage1Callback(multiTimer *t, multiTimerId id, void *clientData) {
    CascadeContext *ctx = clientData;
    printf("Stage 1 complete\n");

    /* Schedule stage 2 */
    ctx->stage = 2;
    multiTimerRegister(t, 1000000, 0, stage2Callback, ctx);

    return false;
}

bool stage2Callback(multiTimer *t, multiTimerId id, void *clientData) {
    CascadeContext *ctx = clientData;
    printf("Stage 2 complete\n");

    /* Schedule stage 3 */
    ctx->stage = 3;
    multiTimerRegister(t, 1000000, 0, stage3Callback, ctx);

    return false;
}

bool stage3Callback(multiTimer *t, multiTimerId id, void *clientData) {
    CascadeContext *ctx = clientData;
    printf("All stages complete\n");
    ctx->stage = 0;

    return false;
}

void runCascade(void) {
    CascadeContext ctx;
    ctx.timer = multiTimerNew();
    ctx.stage = 1;

    /* Start stage 1 */
    multiTimerRegister(ctx.timer, 1000000, 0, stage1Callback, &ctx);

    while (multitimerCount(ctx.timer) > 0) {
        multiTimerProcessTimerEvents(ctx.timer);
        usleep(10000);
    }

    multiTimerFree(ctx.timer);
}
```

## Implementation Details

### Internal Structure

```c
struct multiTimer {
    multimap *scheduled;         /* Active timers sorted by fire time */
    multimap *pendingScheduling; /* Timers to be scheduled after processing */
    multimap *stopEvents;        /* Timer IDs marked for deletion */
    multiTimerId nextTimerId;    /* Next ID to assign */
    multiTimerSystemMonotonicUs initialStartTime; /* Reference time */
    multiTimerId stopLowest;     /* Range of IDs to check for deletion */
    multiTimerId stopHighest;
    multiTimerContext context;   /* USER or TIMER */
    bool timersInclusiveOfTimerRuntime; /* Timing mode */
};
```

### Timer Event Storage

Timer events are stored as 5-element multimap entries:

```c
/* Conceptual structure (not actually used) */
typedef struct multiTimerEvent {
    uint64_t runAtMicroseconds;              /* When to fire */
    multiTimerCallback *cb;                   /* Callback function */
    void *clientData;                         /* User data */
    multiTimerId id;                          /* Timer ID */
    uint64_t repeatIntervalMicroseconds;      /* Repeat interval (0 = one-shot) */
} multiTimerEvent;
```

### Time Adjustment

MultiTimer uses **adjusted time** internally to avoid overflow:

- **Initial Start Time**: Captured when timer manager is initialized
- **Adjusted Time**: Current monotonic time - initial start time
- **Native Time**: Actual monotonic time from system

This allows timers to use smaller numbers and delays overflow.

### Processing Algorithm

1. **Get Current Time**: Calculate adjusted time
2. **Find Expired Timers**: Query multimap for timers <= current time
3. **Execute Callbacks**: Run each timer's callback in order
4. **Reschedule Repeating**: If callback returns true and interval > 0, reschedule
5. **Handle Deletions**: Remove timers marked for deletion
6. **Merge Pending**: Add any timers created during callbacks to scheduled map

### Context Switching

During callback execution:

- Context switches from `MULTI_TIMER_CONTEXT_USER` to `MULTI_TIMER_CONTEXT_TIMER`
- New timer registrations go to `pendingScheduling` map
- After all callbacks complete, context returns to `USER`
- Pending timers are merged into `scheduled` map

This prevents modifying the timer map while iterating it.

### Deletion Handling

When `multiTimerUnregister()` is called:

1. Timer ID is added to `stopEvents` map
2. `stopLowest` and `stopHighest` are updated
3. During processing, timers are checked against this range
4. Matching timers skip execution and are removed

## Performance Characteristics

| Operation  | Complexity | Notes                                |
| ---------- | ---------- | ------------------------------------ |
| Register   | O(log n)   | Insert into sorted multimap          |
| Unregister | O(log n)   | Insert into stop map                 |
| Process    | O(k log n) | k = expired timers, n = total timers |
| Next timer | O(1)       | Query first element of map           |
| Count      | O(1)       | Multimap maintains count             |

**Memory Usage:**

- Base overhead: ~200 bytes per multiTimer
- Per timer: ~80 bytes (multimap entry + overhead)
- Scales linearly with number of active timers

**Timing Precision:**

- Resolution: 1 microsecond (software)
- Accuracy: Depends on `multiTimerProcessTimerEvents()` call frequency
- Jitter: Minimal if processing is regular

## Timer Modes

### Inclusive vs. Exclusive Timing

The `timersInclusiveOfTimerRuntime` flag controls repeat behavior:

**Inclusive (true):**

- Next fire time = previous fire time + interval
- Maintains exact intervals even if callback is slow
- May fire multiple times rapidly if far behind

**Exclusive (false - default):**

- Next fire time = current time + interval
- Interval represents gap between callback completions
- More forgiving of slow callbacks

## Platform Support

MultiTimer works on all platforms supported by:

- `multimap` (all platforms)
- `timeUtil` (all POSIX platforms)

| Platform | Support | Notes                              |
| -------- | ------- | ---------------------------------- |
| Linux    | ✓       | Full support                       |
| macOS    | ✓       | Full support                       |
| FreeBSD  | ✓       | Full support                       |
| Windows  | Partial | Requires POSIX compatibility layer |
| Others   | ✓       | Any POSIX system                   |

## Best Practices

### 1. Call ProcessTimerEvents Regularly

```c
/* GOOD - Regular processing */
while (running) {
    multiTimerProcessTimerEvents(timer);
    handleOtherEvents();
    usleep(1000);  /* Check every 1ms */
}

/* BAD - Infrequent processing */
while (running) {
    multiTimerProcessTimerEvents(timer);
    sleep(1);  /* Timers can be seconds late! */
}
```

### 2. Keep Callbacks Short

```c
/* GOOD - Quick callback */
bool fastCallback(multiTimer *t, multiTimerId id, void *clientData) {
    triggerAsyncWork(clientData);
    return false;
}

/* BAD - Slow callback */
bool slowCallback(multiTimer *t, multiTimerId id, void *clientData) {
    performExpensiveOperation();  /* Blocks other timers! */
    return false;
}
```

### 3. Use Appropriate Intervals

```c
/* GOOD - Reasonable intervals */
multiTimerRegister(timer, 1000000, 1000000, cb, data);  /* 1 second */

/* BAD - Too frequent */
multiTimerRegister(timer, 10, 10, cb, data);  /* 10μs - overhead too high */
```

### 4. Clean Up Properly

```c
/* GOOD - Stop timers before freeing data */
typedef struct Context {
    multiTimerId id;
    void *data;
} Context;

void cleanup(multiTimer *timer, Context *ctx) {
    multiTimerUnregister(timer, ctx->id);
    free(ctx->data);
    free(ctx);
}

/* BAD - Free data while timer active */
free(ctx);  /* Timer callback may use freed memory! */
```

### 5. Check Return Values

```c
/* GOOD */
multiTimerId id = multiTimerRegister(timer, 1000000, 0, cb, data);
if (id == 0) {
    fprintf(stderr, "Failed to register timer\n");
    return false;
}

/* BAD */
multiTimerRegister(timer, 1000000, 0, cb, data);
/* No error checking! */
```

## Common Pitfalls

### 1. Memory Leaks in Callbacks

```c
/* WRONG - Leaked memory if timer repeats */
bool leakyCallback(multiTimer *t, multiTimerId id, void *clientData) {
    void *data = malloc(1024);
    /* ... use data ... */
    return true;  /* Repeats without freeing data! */
}

/* CORRECT */
bool properCallback(multiTimer *t, multiTimerId id, void *clientData) {
    void *data = malloc(1024);
    /* ... use data ... */
    free(data);
    return true;
}
```

### 2. Stale Pointers After Unregister

```c
/* WRONG - Timer might still fire */
Context *ctx = createContext();
multiTimerId id = multiTimerRegister(timer, 1000000, 0, cb, ctx);
multiTimerUnregister(timer, id);
free(ctx);  /* Timer might be in pending queue! */

/* CORRECT - Process before freeing */
multiTimerUnregister(timer, id);
multiTimerProcessTimerEvents(timer);
free(ctx);  /* Now safe */
```

### 3. Long-Running Callbacks

```c
/* WRONG - Blocks event loop */
bool blockingCallback(multiTimer *t, multiTimerId id, void *clientData) {
    sleep(10);  /* All other timers blocked! */
    return false;
}

/* CORRECT - Queue work for later */
bool asyncCallback(multiTimer *t, multiTimerId id, void *clientData) {
    queueWork(clientData);
    return false;
}
```

## Use Cases

1. **Periodic Maintenance**: Database cleanup, cache expiration, log rotation
2. **Timeouts**: Connection timeouts, request timeouts, lock timeouts
3. **Rate Limiting**: Token bucket refill, request throttling
4. **Scheduled Tasks**: Cron-like job scheduling
5. **Retry Logic**: Exponential backoff, periodic retries
6. **Health Checks**: Service monitoring, heartbeat detection
7. **Animation**: Frame timing, transition scheduling
8. **Delayed Actions**: Defer execution, debouncing
9. **Watchdog Timers**: Deadlock detection, hang detection
10. **Event Simulation**: Game tick, simulation step

## See Also

- `multimap` - Underlying data structure
- `timeUtil` - Time measurement functions
- POSIX timers (`timer_create`, `timer_settime`)
- libevent - Alternative event library

## Testing

MultiTimer can be tested for correctness and performance:

```c
#include "multiTimer.h"
#include <assert.h>

void testBasicTimer(void) {
    multiTimer *t = multiTimerNew();
    assert(t != NULL);

    /* Register timer */
    int fired = 0;
    multiTimerId id = multiTimerRegister(t, 1000, 0,
        ^(multiTimer *t, multiTimerId id, void *data) {
            (*(int *)data)++;
            return false;
        }, &fired);

    assert(id != 0);
    assert(multitimerCount(t) == 1);

    /* Process immediately - shouldn't fire */
    multiTimerProcessTimerEvents(t);
    assert(fired == 0);

    /* Wait and process */
    usleep(2000);
    multiTimerProcessTimerEvents(t);
    assert(fired == 1);
    assert(multitimerCount(t) == 0);

    multiTimerFree(t);
}

void testRepeatingTimer(void) {
    multiTimer *t = multiTimerNew();
    int count = 0;

    multiTimerRegister(t, 1000, 1000,
        ^(multiTimer *t, multiTimerId id, void *data) {
            (*(int *)data)++;
            return *(int *)data < 5;  /* Fire 5 times */
        }, &count);

    /* Process over time */
    for (int i = 0; i < 10; i++) {
        usleep(1000);
        multiTimerProcessTimerEvents(t);
    }

    assert(count == 5);
    multiTimerFree(t);
}

void testUnregister(void) {
    multiTimer *t = multiTimerNew();
    int fired = 0;

    multiTimerId id = multiTimerRegister(t, 1000, 0,
        ^(multiTimer *t, multiTimerId id, void *data) {
            (*(int *)data)++;
            return false;
        }, &fired);

    /* Cancel before firing */
    multiTimerUnregister(t, id);

    usleep(2000);
    multiTimerProcessTimerEvents(t);

    assert(fired == 0);
    assert(multitimerCount(t) == 0);

    multiTimerFree(t);
}
```

Performance benchmarks should test:

- Scalability with many timers (1K, 10K, 100K)
- Processing overhead for expired timers
- Registration/unregistration cost
