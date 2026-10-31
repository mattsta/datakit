# MultiTimer - Multi-Timer Event Management

## Overview

`multiTimer` is a **compact sorted-map timer scheduler**. Timers live in a multimap sorted by expiry, so the next-event query is a first-entry read and storage stays maximally compact (~50-60 bytes per timer, with no preallocated wheel structure). Registration and cancellation are O(log n); firing walks the due prefix in order.

**Key Features:**

- Timer storage in a sorted multimap - no preallocated slot arrays
- One-shot and repeating timer support
- Microsecond precision timing
- Registration/unregistration from inside callbacks
- Due timers fire in expiry order
- Zero overhead for inactive timers
- On-demand statistics snapshot (`multiTimerGetStats`)

**Header**: `multiTimer.h`

**Source**: `multiTimer.c`, `multiTimerInternal.h`

**Dependencies**: `multimap`, `timeUtil`

**Platforms**: Linux, macOS, and the BSD family (anything `multimap` and `timeUtil` build on)

### Processing is NOT automatic

Nothing fires on its own. Call `multiTimerProcessTimerEvents()` periodically, or
sleep until `multiTimerNextTimerEventOffsetFromNowUs()` and then call it. Timers
that expire between calls fire on the next call; none are lost.

### Thread safety

`multiTimer` is **not** thread-safe. All calls must come from one thread, or the
caller must serialize them externally. Typical usage is a single-threaded event
loop.

### Choosing between multiTimer and timerWheel

Use `multiTimer` when memory per timer matters most: storage is a sorted map with
nothing preallocated. Use [timerWheel](TIMER_WHEEL.md) for large, churn-heavy
timer populations where per-operation latency matters more than memory - it
trades a fixed 448-slot structure for O(1) amortized insert and tick.

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
typedef uint64_t multiTimerDurationUs;           /* Delay/interval in microseconds */
typedef int64_t multiTimerUs;                    /* Signed microsecond offset */
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
- `false`: Don't reschedule (cancel the repeat)

For one-shot timers (`repeatEveryMicroseconds == 0`) the return value is ignored.

**Safe operations inside a callback:**

- Register new timers - deferred until the current processing pass completes
- Unregister any timer, including the one currently firing
- `multiTimerStopAll()` - timers registered afterward (even later in the same
  callback) survive
- Call `multiTimerProcessTimerEvents()` - a safe no-op; the in-progress outer
  call picks up the deferred work
- Read `multiTimerCount()` / `multiTimerGetStats()` - but see the note under
  `multiTimerCount` about transient over-reporting of `stopPending` mid-batch

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
 * Must be called before using timer manager. The struct is fully zeroed
 * first, so a stack-allocated multiTimer is safe - no field inherits
 * stack garbage. On failure the partially built state is torn down.
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
 * Returns: timer ID, always non-zero
 *
 * IDs are handed out monotonically and the call cannot fail (allocation
 * failure is fatal in datakit), so the returned ID is always usable.
 * Can be called from within timer callbacks; registrations made from a
 * callback are held aside and merged in when processing completes.
 */
multiTimerId multiTimerRegister(
    multiTimer *t,
    multiTimerDurationUs startAfterMicroseconds,
    multiTimerDurationUs repeatEveryMicroseconds,
    multiTimerCallback *cb,
    void *clientData
);

/* Stop a timer by id
 * t: timer manager
 * id: timer ID to cancel
 * Returns: true if the id was ever issued by this timer manager,
 *          false otherwise (id 0, or an id never handed out)
 *
 * Cancellation is a tombstone: the id is recorded in O(log n) and the
 * timer's storage is reclaimed when processing next encounters it (or
 * when the timer set drains empty). The timer will not fire again.
 * Safe to call during a callback, including on the firing timer itself.
 * Unregistering an already-fired id is harmless and count-exact.
 */
bool multiTimerUnregister(multiTimer *t, multiTimerId id);

/* Stop all currently registered timers
 * t: timer manager
 * Returns: true
 *
 * Safe from within a callback; timers registered after this call
 * (including later in the same callback) survive.
 */
bool multiTimerStopAll(multiTimer *t);
```

### Timer Processing

```c
/* Process all expired timers
 * t: timer manager
 *
 * Executes callbacks for all timers that have reached their scheduled time,
 * reschedules repeating timers whose callback returned true, discards
 * stop-marked entries it encounters, merges timers registered from inside
 * callbacks, and reconciles stop accounting at the end of the batch.
 * Should be called regularly from the event loop.
 * Calling this from inside a callback is a safe no-op.
 */
void multiTimerProcessTimerEvents(multiTimer *t);
```

### Timer Queries

```c
/* Count of live timers (registered, not yet fired or stopped)
 * t: timer manager
 * Returns: number of live timers
 *
 * Exact. When an already-fired id has been unregistered, the stop
 * accounting is stale, so this lazily reconciles it against stored
 * entries first - an O(n) walk that runs at most once per cancellation
 * batch, keeping the call amortized O(1).
 *
 * Called from inside a callback the reconcile is skipped (the batch's
 * bulk delete has not run yet), so mid-callback reads may transiently
 * over-report stopPending until the batch ends.
 */
size_t multiTimerCount(multiTimer *t);

/* Get absolute time of next timer
 * t: timer manager
 * Returns: monotonic time (microseconds) of the earliest timer that will
 * fire (a stopped timer still stored is skipped), or 0 if none will
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

### Statistics

```c
/* Live-state snapshot, computed on demand (nothing is stored) */
typedef struct multiTimerStats {
    size_t activeCount;    /* live timers (excludes stop-marked) */
    size_t scheduledCount; /* entries currently in the scheduled map */
    size_t pendingCount;   /* entries deferred from inside callbacks */
    size_t stopPending;    /* stop-marked entries awaiting lazy discard */
    size_t memoryBytes;    /* bytes across all three maps + struct */
} multiTimerStats;

/* Populate a stats snapshot.
 * Like multiTimerCount, this reconciles stale stop accounting first when
 * called outside a callback, so activeCount and stopPending are exact.
 */
void multiTimerGetStats(multiTimer *t, multiTimerStats *stats);
```

```c
/* Example */
multiTimerStats stats;
multiTimerGetStats(timer, &stats);
printf("live=%zu scheduled=%zu pending=%zu stopPending=%zu bytes=%zu\n",
       stats.activeCount, stats.scheduledCount, stats.pendingCount,
       stats.stopPending, stats.memoryBytes);
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
    while (multiTimerCount(timer) > 0) {
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

    while (multiTimerCount(timer) > 0) {
        multiTimerProcessTimerEvents(timer);
        usleep(10000);
    }

    multiTimerFree(timer);
}
```

### Example 3: Multiple Timers

```c
#include "multiTimer.h"
#include <inttypes.h>

typedef struct TimerContext {
    const char *name;
    int fireCount;
} TimerContext;

bool namedTimerCallback(multiTimer *t, multiTimerId id, void *clientData) {
    TimerContext *ctx = clientData;
    ctx->fireCount++;

    printf("[%s] Timer %" PRIu64 " fired (count: %d)\n",
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

    while (multiTimerCount(timer) > 0) {
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
#include <inttypes.h>

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

    printf("Request %" PRIu64 " timed out\n", req->id);
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

bool stage2Callback(multiTimer *t, multiTimerId id, void *clientData);
bool stage3Callback(multiTimer *t, multiTimerId id, void *clientData);

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

    while (multiTimerCount(ctx.timer) > 0) {
        multiTimerProcessTimerEvents(ctx.timer);
        usleep(10000);
    }

    multiTimerFree(ctx.timer);
}
```

## Implementation Details

### Internal Structure

Declared in `multiTimerInternal.h` (not part of the public API):

```c
struct multiTimer {
    multimap *scheduled;         /* active timers, sorted by fire time */
    multimap *pendingScheduling; /* to-be-active timers (deferred from callbacks) */
    multimap *stopEvents;        /* timer IDs marked to not run later */
    multiTimerId nextTimerId;
    multiTimerSystemMonotonicUs initialStartTime;

    /* Ids at or below this are stopped wholesale (StopAll) without
     * materializing stopEvents entries; ids are monotonic so the
     * watermark never has to be rolled back. */
    multiTimerId stoppedUpTo;

    multiTimerId stopLowest;  /* shortcut endpoints for stopped-id checks */
    multiTimerId stopHighest;

    size_t stopPending;       /* stored entries believed stopped, awaiting
                               * lazy discard */
    bool stopsMaybeStale;     /* a planted stop may name an id whose entry is
                               * already gone; readers reconcile lazily */

    multiTimerContext context;          /* USER or TIMER */
    bool timersInclusiveOfTimerRuntime; /* timing mode */
};
```

### Timer Event Storage

Each timer is one 5-element multimap entry, sorted by the first element
(`runAtMicroseconds`), so the next-due timer is always the map's first entry.
The decoded form used internally is:

```c
typedef struct multiTimerEvent {
    multiTimerAdjustedUs runAtMicroseconds;   /* When to fire (adjusted time) */
    multiTimerCallback *cb;                   /* Callback function */
    void *clientData;                         /* User data */
    multiTimerId id;                          /* Timer ID */
    multiTimerDurationUs repeatIntervalMicroseconds; /* 0 = one-shot */
} multiTimerEvent;
```

Expiry math saturates rather than wraps: an absurd delay becomes "effectively
never" (`UINT64_MAX`) instead of silently firing immediately.

### Time Adjustment

MultiTimer uses **adjusted time** internally to avoid overflow:

- **Initial Start Time**: Captured when timer manager is initialized
- **Adjusted Time**: Current monotonic time - initial start time
- **Native Time**: Actual monotonic time from system

This allows timers to use smaller numbers and delays overflow.

### Processing Algorithm

1. **Get Current Time**: Calculate adjusted time
2. **Find Expired Timers**: Walk the due prefix of the sorted map (entries <= now)
3. **Skip Stopped Timers**: Entries whose id is tombstoned (or below the StopAll
   watermark) are discarded instead of run, consuming their tombstone
4. **Execute Callbacks**: Run each remaining timer's callback in expiry order
5. **Reschedule Repeating**: If interval > 0 and the callback returned true, reschedule
6. **Bulk Delete**: Remove the entries processed in this batch
7. **Merge Pending**: Move any timers registered from inside callbacks into the
   scheduled map (a pointer swap when the scheduled map drained empty)
8. **Reconcile Stops**: If any tombstone might name an id that no longer has a
   stored entry, walk stored entries once to restore exact accounting

### Context Switching

During callback execution:

- Context switches from `MULTI_TIMER_CONTEXT_USER` to `MULTI_TIMER_CONTEXT_TIMER`
- New timer registrations go to `pendingScheduling` map
- After all callbacks complete, context returns to `USER`
- Pending timers are merged into `scheduled` map

This prevents modifying the timer map while iterating it.

### Cancellation Handling (Tombstones)

`multiTimerUnregister()` never searches for the timer's storage. It:

1. Rejects id 0 and ids never issued (returns false)
2. Returns true immediately if the id is at or below the `stoppedUpTo`
   watermark (already stopped wholesale by `multiTimerStopAll`)
3. Returns true immediately if the id already has a tombstone (double
   unregister must not plant a duplicate)
4. Otherwise inserts the id into `stopEvents`, extends
   `stopLowest`/`stopHighest`, bumps `stopPending`, and sets `stopsMaybeStale`

Processing then discards the timer's storage the next time it encounters it.

`multiTimerStopAll()` is a watermark rather than n tombstones: `stoppedUpTo` is
set to `nextTimerId` and the whole stop map is dropped. Outside a callback the
scheduled and pending maps are reset immediately; inside a callback the entries
are left in place (the map is being iterated) and discarded lazily on encounter.

**Unregistering an already-fired id** plants a tombstone with no matching stored
entry, which would deflate counts forever. `stopsMaybeStale` marks that
possibility; readers (`multiTimerCount`, `multiTimerGetStats`, and the end of
each processing batch) run an O(n) reconcile walk that drops tombstones with no
live entry. The walk runs at most once per cancellation batch, so counts stay
exact at amortized O(1). Mid-callback the reconcile is skipped, because the
batch's already-run entries have not been bulk-deleted yet and would be
misclassified.

## Performance Characteristics

| Operation  | Complexity     | Notes                                              |
| ---------- | -------------- | -------------------------------------------------- |
| Register   | O(log n)       | Insert into sorted multimap                        |
| Unregister | O(log n)       | Insert into stop map; no search for the timer      |
| StopAll    | O(1) amortized | Watermark + map resets                             |
| Process    | O(k log n)     | k = expired timers, n = total timers               |
| Next timer | O(1)           | First entry of the sorted map                      |
| Count      | O(1) amortized | O(n) reconcile at most once per cancellation batch |
| GetStats   | O(1) amortized | Same reconcile rule as Count                       |

**Memory Usage:**

- Per timer: ~50-60 bytes (one 5-element multimap entry)
- Nothing is preallocated - storage is exactly the live timer set
- `multiTimerGetStats().memoryBytes` reports the exact live total
  (struct + all three maps)

**Timing Precision:**

- Resolution: 1 microsecond (software)
- Accuracy: Depends on `multiTimerProcessTimerEvents()` call frequency
- Jitter: Minimal if processing is regular

## Timer Modes

### Inclusive vs. Exclusive Timing

The internal `timersInclusiveOfTimerRuntime` flag controls repeat behavior:

**Inclusive (true):**

- Next fire time = previous fire time + interval
- Maintains exact intervals even if callback is slow
- May fire multiple times rapidly if far behind

**Exclusive (false):**

- Next fire time = current time + interval
- Interval represents gap between callback completions
- More forgiving of slow callbacks

The flag lives in `multiTimerInternal.h` and there is **no public setter**:
`multiTimerInit()` sets it to false, so repeating timers currently always use
exclusive timing. Repeat scheduling saturates on overflow, matching registration.

## Platform Support

`multiTimer` works wherever `multimap` and `timeUtil` build - POSIX systems with
a monotonic clock. There is no Windows support in datakit.

| Platform   | Support | Notes                      |
| ---------- | ------- | -------------------------- |
| Linux      | ✓       | Full support               |
| macOS      | ✓       | Full support               |
| BSD family | ✓       | FreeBSD / NetBSD / OpenBSD |

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

### 5. Keep the Timer ID If You Might Cancel

`multiTimerRegister()` always succeeds and always returns a non-zero id, so
there is no error branch to write. What matters is keeping the id: it is the
only way to cancel the timer later.

```c
/* GOOD - id retained for cancellation */
ctx->timerId = multiTimerRegister(timer, 1000000, 0, cb, ctx);
/* ... */
multiTimerUnregister(timer, ctx->timerId);

/* FINE - fire-and-forget one-shot that never needs cancelling */
multiTimerRegister(timer, 1000000, 0, cb, data);
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

### 2. Freeing Client Data Without Unregistering

Once `multiTimerUnregister()` returns true, the timer will never fire again -
the tombstone is checked before every callback dispatch - so freeing its client
data immediately is safe. What is _not_ safe is freeing client data for a timer
that is still registered.

```c
/* WRONG - timer still live, callback will read freed memory */
Context *ctx = createContext();
multiTimerId id = multiTimerRegister(timer, 1000000, 0, cb, ctx);
free(ctx);

/* CORRECT - cancel first, then free; no processing pass needed */
multiTimerUnregister(timer, id);
free(ctx);
```

The timer's _storage_ is reclaimed lazily (when processing next encounters the
entry, or when the set drains empty), but the callback is already dead at that
point. Reclaiming storage is not something the caller has to wait for.

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

- [timerWheel](TIMER_WHEEL.md) - hierarchical timing wheel; the latency-oriented
  alternative to this memory-oriented scheduler
- [multimap](../multimap/MULTIMAP.md) - underlying sorted storage
- [timeUtil](TIME_UTIL.md) - monotonic clock behind `multiTimerGetUs()`
- POSIX timers (`timer_create`, `timer_settime`)

## Testing

MultiTimer can be tested for correctness and performance:

```c
#include "multiTimer.h"
#include <assert.h>
#include <unistd.h>

static bool countOnce(multiTimer *t, multiTimerId id, void *data) {
    (void)t;
    (void)id;
    (*(int *)data)++;
    return false;
}

static bool countUpToFive(multiTimer *t, multiTimerId id, void *data) {
    (void)t;
    (void)id;
    (*(int *)data)++;
    return *(int *)data < 5; /* Fire 5 times */
}

void testBasicTimer(void) {
    multiTimer *t = multiTimerNew();
    assert(t != NULL);

    int fired = 0;
    multiTimerId id = multiTimerRegister(t, 1000, 0, countOnce, &fired);

    assert(id != 0);
    assert(multiTimerCount(t) == 1);

    /* Process immediately - shouldn't fire */
    multiTimerProcessTimerEvents(t);
    assert(fired == 0);

    /* Wait and process */
    usleep(2000);
    multiTimerProcessTimerEvents(t);
    assert(fired == 1);
    assert(multiTimerCount(t) == 0);

    multiTimerFree(t);
}

void testRepeatingTimer(void) {
    multiTimer *t = multiTimerNew();
    int count = 0;

    multiTimerRegister(t, 1000, 1000, countUpToFive, &count);

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

    multiTimerId id = multiTimerRegister(t, 1000, 0, countOnce, &fired);

    /* Cancel before firing */
    assert(multiTimerUnregister(t, id) == true);
    assert(multiTimerCount(t) == 0);

    usleep(2000);
    multiTimerProcessTimerEvents(t);

    assert(fired == 0);
    assert(multiTimerCount(t) == 0);

    multiTimerFree(t);
}

void testUnregisterAfterFiring(void) {
    multiTimer *t = multiTimerNew();
    int fired = 0;

    multiTimerId id = multiTimerRegister(t, 1000, 0, countOnce, &fired);

    usleep(2000);
    multiTimerProcessTimerEvents(t);
    assert(fired == 1);

    /* Cancelling an already-fired id still reports "id was issued",
     * and the count stays exact thanks to the lazy reconcile. */
    assert(multiTimerUnregister(t, id) == true);
    assert(multiTimerCount(t) == 0);

    /* Never-issued ids are rejected */
    assert(multiTimerUnregister(t, id + 1000) == false);
    assert(multiTimerUnregister(t, 0) == false);

    multiTimerFree(t);
}
```

Performance benchmarks should test:

- Scalability with many timers (1K, 10K, 100K)
- Processing overhead for expired timers
- Registration/unregistration cost
- Cancellation-heavy workloads (the reconcile walk amortizes to once per batch)
