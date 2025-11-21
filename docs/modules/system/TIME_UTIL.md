# TimeUtil - High-Resolution Time Utilities

## Overview

`TimeUtil` provides **cross-platform high-resolution time measurement** utilities. It offers both wall-clock time and monotonic time functions with microsecond and millisecond precision, abstracting platform differences between Linux, macOS, FreeBSD, and other Unix systems.

**Key Features:**

- Wall-clock time (can go backwards due to NTP, timezone changes)
- Monotonic time (guaranteed never to go backwards)
- Microsecond, millisecond, and second precision
- Cross-platform abstraction (Linux, macOS, FreeBSD, others)
- Zero-overhead inline functions
- Thread-safe operations

**Header**: `timeUtil.h`

**Source**: `timeUtil.c`

**Platforms**: All POSIX-compliant systems

## API Reference

### Wall-Clock Time Functions

Wall-clock time represents the actual time of day. It can jump forwards or backwards due to NTP adjustments, timezone changes, or manual clock settings.

```c
/* Get current time in microseconds since Unix epoch
 * Returns: microseconds since 1970-01-01 00:00:00 UTC
 * Source: gettimeofday()
 */
uint64_t timeUtilUs(void);

/* Get current time in milliseconds since Unix epoch
 * Returns: milliseconds since 1970-01-01 00:00:00 UTC
 * Implementation: timeUtilUs() / 1000
 */
uint64_t timeUtilMs(void);

/* Get current time in seconds since Unix epoch
 * Returns: seconds since 1970-01-01 00:00:00 UTC
 * Source: gettimeofday()
 */
uint64_t timeUtilS(void);
```

**When to Use:**

- Recording actual timestamps (log entries, database records)
- Comparing with external time sources
- Displaying time to users
- Serialization/deserialization of time values

**When NOT to Use:**

- Measuring elapsed time (use monotonic time instead)
- Timeouts or intervals
- Performance measurements

### Monotonic Time Functions

Monotonic time is guaranteed to never decrease. It's not affected by NTP or manual clock adjustments. Perfect for measuring elapsed time.

```c
/* Get monotonic time in nanoseconds
 * Returns: nanoseconds since arbitrary start point
 * Source: Platform-dependent (see Platform Support)
 */
uint64_t timeUtilMonotonicNs(void);

/* Get monotonic time in microseconds
 * Returns: microseconds since arbitrary start point
 * Implementation: timeUtilMonotonicNs() / 1000
 */
uint64_t timeUtilMonotonicUs(void);

/* Get monotonic time in milliseconds
 * Returns: milliseconds since arbitrary start point
 * Implementation: timeUtilMonotonicNs() / 1000000
 */
uint64_t timeUtilMonotonicMs(void);
```

**When to Use:**

- Measuring elapsed time
- Timeouts and deadlines
- Performance benchmarking
- Rate limiting
- Retry intervals

**When NOT to Use:**

- Absolute timestamps (use wall-clock time)
- Comparing across reboots (monotonic time resets)
- Comparing across different machines

## Platform-Specific Implementation

### macOS (Darwin)

```c
/* macOS uses mach_absolute_time() */
uint64_t timeUtilMonotonicNs(void) {
#if DK_OS_APPLE_MAC
    /* On macOS, mach_absolute_time() returns nanoseconds directly */
    return mach_absolute_time();
#elif DK_OS_APPLE
    /* Other Apple platforms need timebase conversion */
    uint64_t machTime = mach_absolute_time();
    static mach_timebase_info_data_t timebaseInfo = {0};
    static uint64_t unitOffset = 0;

    if (!unitOffset) {
        mach_timebase_info(&timebaseInfo);
        unitOffset = timebaseInfo.numer / timebaseInfo.denom;
    }

    return machTime * unitOffset;
#endif
}
```

### FreeBSD

```c
/* FreeBSD uses CLOCK_MONOTONIC_FAST for better performance */
#if DK_OS_FREEBSD
    const clockid_t useClock = CLOCK_MONOTONIC_FAST;
#else
    const clockid_t useClock = CLOCK_MONOTONIC;
#endif

struct timespec ts = {0};
clock_gettime(useClock, &ts);
return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
```

### Linux and Others

```c
/* Standard POSIX CLOCK_MONOTONIC */
struct timespec ts = {0};
clock_gettime(CLOCK_MONOTONIC, &ts);
return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
```

## Usage Examples

### Example 1: Measuring Elapsed Time

```c
#include "timeUtil.h"
#include <stdio.h>

void measureOperation(void) {
    uint64_t start = timeUtilMonotonicUs();

    /* Perform operation */
    expensiveComputation();

    uint64_t end = timeUtilMonotonicUs();
    uint64_t elapsedUs = end - start;

    printf("Operation took %lu microseconds (%.2f ms)\n",
           elapsedUs, elapsedUs / 1000.0);
}

void measureMultipleOperations(void) {
    uint64_t start = timeUtilMonotonicMs();

    for (int i = 0; i < 1000; i++) {
        processItem(i);
    }

    uint64_t end = timeUtilMonotonicMs();
    uint64_t elapsedMs = end - start;

    printf("Processed 1000 items in %lu ms (%.2f items/sec)\n",
           elapsedMs, 1000.0 / (elapsedMs / 1000.0));
}
```

### Example 2: Timeout Implementation

```c
#include "timeUtil.h"
#include <stdbool.h>

bool waitForConditionWithTimeout(int timeoutMs) {
    uint64_t deadline = timeUtilMonotonicMs() + timeoutMs;

    while (timeUtilMonotonicMs() < deadline) {
        if (checkCondition()) {
            return true;  /* Success */
        }

        usleep(1000);  /* Sleep 1ms between checks */
    }

    return false;  /* Timeout */
}

/* More precise timeout with microseconds */
bool preciseTimeout(int timeoutUs) {
    uint64_t deadline = timeUtilMonotonicUs() + timeoutUs;

    while (timeUtilMonotonicUs() < deadline) {
        if (checkCondition()) {
            return true;
        }

        /* Busy-wait for very short timeouts */
        if (timeoutUs < 1000) {
            /* Spin */
        } else {
            usleep(100);
        }
    }

    return false;
}
```

### Example 3: Rate Limiting

```c
#include "timeUtil.h"

typedef struct RateLimiter {
    uint64_t lastActionUs;
    uint64_t minIntervalUs;
} RateLimiter;

void rateLimiterInit(RateLimiter *rl, double actionsPerSecond) {
    rl->lastActionUs = 0;
    rl->minIntervalUs = (uint64_t)(1000000.0 / actionsPerSecond);
}

bool rateLimiterTryAction(RateLimiter *rl) {
    uint64_t now = timeUtilMonotonicUs();

    if (rl->lastActionUs == 0 ||
        now - rl->lastActionUs >= rl->minIntervalUs) {
        rl->lastActionUs = now;
        return true;  /* Action allowed */
    }

    return false;  /* Rate limited */
}

uint64_t rateLimiterTimeUntilNextAction(RateLimiter *rl) {
    uint64_t now = timeUtilMonotonicUs();

    if (rl->lastActionUs == 0) {
        return 0;  /* Can act immediately */
    }

    uint64_t elapsed = now - rl->lastActionUs;
    if (elapsed >= rl->minIntervalUs) {
        return 0;  /* Can act now */
    }

    return rl->minIntervalUs - elapsed;
}
```

### Example 4: Performance Benchmarking

```c
#include "timeUtil.h"
#include <stdio.h>

typedef struct Benchmark {
    const char *name;
    uint64_t totalUs;
    uint64_t count;
    uint64_t minUs;
    uint64_t maxUs;
} Benchmark;

void benchmarkInit(Benchmark *b, const char *name) {
    b->name = name;
    b->totalUs = 0;
    b->count = 0;
    b->minUs = UINT64_MAX;
    b->maxUs = 0;
}

void benchmarkStart(Benchmark *b, uint64_t *start) {
    *start = timeUtilMonotonicNs();
}

void benchmarkEnd(Benchmark *b, uint64_t start) {
    uint64_t end = timeUtilMonotonicNs();
    uint64_t elapsedNs = end - start;
    uint64_t elapsedUs = elapsedNs / 1000;

    b->totalUs += elapsedUs;
    b->count++;

    if (elapsedUs < b->minUs) b->minUs = elapsedUs;
    if (elapsedUs > b->maxUs) b->maxUs = elapsedUs;
}

void benchmarkReport(Benchmark *b) {
    if (b->count == 0) {
        printf("%s: No samples\n", b->name);
        return;
    }

    double avgUs = (double)b->totalUs / b->count;

    printf("%s: %lu samples\n", b->name, b->count);
    printf("  Min:  %lu μs\n", b->minUs);
    printf("  Max:  %lu μs\n", b->maxUs);
    printf("  Avg:  %.2f μs\n", avgUs);
    printf("  Total: %.2f ms\n", b->totalUs / 1000.0);
}

/* Example usage */
void runBenchmark(void) {
    Benchmark b;
    benchmarkInit(&b, "Database Query");

    for (int i = 0; i < 1000; i++) {
        uint64_t start;
        benchmarkStart(&b, &start);

        executeQuery();

        benchmarkEnd(&b, start);
    }

    benchmarkReport(&b);
}
```

### Example 5: Timestamping Log Entries

```c
#include "timeUtil.h"
#include <stdio.h>
#include <time.h>

typedef struct LogEntry {
    uint64_t timestampUs;
    char message[256];
} LogEntry;

void logMessage(const char *format, ...) {
    uint64_t nowUs = timeUtilUs();
    uint64_t nowS = timeUtilS();

    /* Convert to local time for display */
    time_t t = (time_t)nowS;
    struct tm tm;
    localtime_r(&t, &tm);

    /* Microsecond component */
    uint64_t usComponent = nowUs % 1000000;

    /* Format: YYYY-MM-DD HH:MM:SS.uuuuuu */
    char timestamp[32];
    snprintf(timestamp, sizeof(timestamp),
             "%04d-%02d-%02d %02d:%02d:%02d.%06lu",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             usComponent);

    va_list args;
    va_start(args, format);
    printf("[%s] ", timestamp);
    vprintf(format, args);
    printf("\n");
    va_end(args);
}

/* Example output:
 * [2025-11-18 10:23:45.123456] Server started
 * [2025-11-18 10:23:45.234567] Connection accepted
 */
```

### Example 6: Retry with Exponential Backoff

```c
#include "timeUtil.h"
#include <unistd.h>

bool retryWithBackoff(bool (*operation)(void),
                      int maxRetries,
                      int initialDelayMs) {
    int delayMs = initialDelayMs;

    for (int attempt = 0; attempt < maxRetries; attempt++) {
        uint64_t start = timeUtilMonotonicMs();

        if (operation()) {
            return true;  /* Success */
        }

        /* Exponential backoff */
        printf("Attempt %d failed, retrying in %d ms...\n",
               attempt + 1, delayMs);

        usleep(delayMs * 1000);

        uint64_t elapsed = timeUtilMonotonicMs() - start;
        printf("Actual delay: %lu ms\n", elapsed);

        delayMs *= 2;  /* Double delay for next attempt */
        if (delayMs > 30000) {
            delayMs = 30000;  /* Cap at 30 seconds */
        }
    }

    return false;  /* All retries exhausted */
}
```

### Example 7: Simple Profiler

```c
#include "timeUtil.h"
#include <stdio.h>

typedef struct Profiler {
    const char *sections[16];
    uint64_t times[16];
    int count;
    uint64_t totalStart;
} Profiler;

void profilerStart(Profiler *p) {
    p->count = 0;
    p->totalStart = timeUtilMonotonicUs();
}

void profilerMark(Profiler *p, const char *section) {
    if (p->count < 16) {
        p->sections[p->count] = section;
        p->times[p->count] = timeUtilMonotonicUs();
        p->count++;
    }
}

void profilerEnd(Profiler *p) {
    uint64_t totalEnd = timeUtilMonotonicUs();
    uint64_t totalUs = totalEnd - p->totalStart;

    printf("=== Profile Report ===\n");
    printf("Total: %.2f ms\n\n", totalUs / 1000.0);

    for (int i = 0; i < p->count; i++) {
        uint64_t startUs = (i == 0) ? p->totalStart : p->times[i - 1];
        uint64_t endUs = p->times[i];
        uint64_t elapsedUs = endUs - startUs;
        double percent = (double)elapsedUs / totalUs * 100.0;

        printf("%s: %.2f ms (%.1f%%)\n",
               p->sections[i],
               elapsedUs / 1000.0,
               percent);
    }
}

/* Example usage */
void processRequest(void) {
    Profiler p;
    profilerStart(&p);

    authenticateUser();
    profilerMark(&p, "Authentication");

    loadData();
    profilerMark(&p, "Data Loading");

    processData();
    profilerMark(&p, "Processing");

    sendResponse();
    profilerMark(&p, "Response");

    profilerEnd(&p);
}

/* Output:
 * === Profile Report ===
 * Total: 45.23 ms
 *
 * Authentication: 5.12 ms (11.3%)
 * Data Loading: 15.67 ms (34.6%)
 * Processing: 20.34 ms (45.0%)
 * Response: 4.10 ms (9.1%)
 */
```

## Implementation Details

### Time Resolution

| Platform | Wall Clock       | Monotonic Clock        | Resolution |
| -------- | ---------------- | ---------------------- | ---------- |
| Linux    | `gettimeofday()` | `CLOCK_MONOTONIC`      | ~1μs       |
| macOS    | `gettimeofday()` | `mach_absolute_time()` | ~1ns       |
| FreeBSD  | `gettimeofday()` | `CLOCK_MONOTONIC_FAST` | ~1μs       |
| Others   | `gettimeofday()` | `CLOCK_MONOTONIC`      | ~1μs       |

### Constructor Attribute (macOS Only)

On macOS, the `timeUtilMonotonicNs()` function uses a constructor attribute to pre-initialize the timebase conversion:

```c
#if DK_OS_APPLE
#define TIME_INIT __attribute__((constructor))
#else
#define TIME_INIT
#endif

uint64_t TIME_INIT timeUtilMonotonicNs(void) {
    /* ... implementation ... */
}
```

This eliminates a branch on every call after initialization.

### Thread Safety

All TimeUtil functions are **thread-safe** and reentrant:

- Wall-clock functions use `gettimeofday()` (thread-safe system call)
- Monotonic functions use `clock_gettime()` or `mach_absolute_time()` (thread-safe)
- macOS timebase initialization uses static variables (safe after first call)

### Performance Characteristics

| Function                | Typical Latency |
| ----------------------- | --------------- |
| `timeUtilUs()`          | ~50-100ns       |
| `timeUtilMs()`          | ~50-100ns       |
| `timeUtilS()`           | ~50-100ns       |
| `timeUtilMonotonicNs()` | ~20-50ns        |
| `timeUtilMonotonicUs()` | ~20-50ns        |
| `timeUtilMonotonicMs()` | ~20-50ns        |

Monotonic time is typically faster because:

- No timezone conversion
- No leap second handling
- Direct hardware counter access (on some platforms)

## Wall Clock vs. Monotonic Time

### Wall Clock Time Characteristics

**Advantages:**

- Absolute timestamps
- Can be compared across systems (if synchronized)
- Meaningful to humans
- Persistent across reboots

**Disadvantages:**

- Can jump backwards (NTP adjustments)
- Can jump forwards (DST, timezone changes)
- Not suitable for measuring intervals
- Subject to system clock changes

### Monotonic Time Characteristics

**Advantages:**

- Never goes backwards
- Perfect for measuring elapsed time
- Unaffected by clock adjustments
- Usually faster than wall clock

**Disadvantages:**

- Arbitrary zero point
- Not comparable across systems
- Not comparable across reboots
- Not meaningful to humans

### Decision Matrix

| Use Case                     | Use Wall Clock | Use Monotonic |
| ---------------------------- | -------------- | ------------- |
| Log timestamp                | ✓              | ✗             |
| Database timestamp           | ✓              | ✗             |
| Measure duration             | ✗              | ✓             |
| Timeout/deadline             | ✗              | ✓             |
| Performance benchmark        | ✗              | ✓             |
| Rate limiting                | ✗              | ✓             |
| Display to user              | ✓              | ✗             |
| Compare with external system | ✓              | ✗             |

## Platform Support

| Platform | Wall Clock | Monotonic | CLOCK_MONOTONIC_FAST | mach_absolute_time |
| -------- | ---------- | --------- | -------------------- | ------------------ |
| Linux    | ✓          | ✓         | ✗                    | ✗                  |
| macOS    | ✓          | ✓         | ✗                    | ✓                  |
| iOS      | ✓          | ✓         | ✗                    | ✓                  |
| FreeBSD  | ✓          | ✓         | ✓                    | ✗                  |
| OpenBSD  | ✓          | ✓         | ✗                    | ✗                  |
| Solaris  | ✓          | ✓         | ✗                    | ✗                  |

## Best Practices

### 1. Use Monotonic Time for Intervals

```c
/* GOOD - Monotonic time for elapsed time */
uint64_t start = timeUtilMonotonicUs();
doWork();
uint64_t elapsed = timeUtilMonotonicUs() - start;

/* BAD - Wall clock can go backwards! */
uint64_t start = timeUtilUs();
doWork();
uint64_t elapsed = timeUtilUs() - start;  /* May be negative! */
```

### 2. Use Wall Clock for Absolute Timestamps

```c
/* GOOD - Wall clock for timestamps */
uint64_t timestamp = timeUtilUs();
saveToDatabase(timestamp, data);

/* BAD - Monotonic time is meaningless */
uint64_t timestamp = timeUtilMonotonicUs();
saveToDatabase(timestamp, data);  /* Can't interpret later! */
```

### 3. Choose Appropriate Resolution

```c
/* GOOD - Microsecond resolution for precise timing */
uint64_t start = timeUtilMonotonicUs();
fastOperation();
uint64_t elapsedUs = timeUtilMonotonicUs() - start;

/* GOOD - Millisecond resolution for coarse timing */
uint64_t start = timeUtilMonotonicMs();
slowOperation();
uint64_t elapsedMs = timeUtilMonotonicMs() - start;

/* OVERKILL - Nanosecond for ms-level timing */
uint64_t start = timeUtilMonotonicNs();
slowOperation();  /* Precision wasted */
```

### 4. Handle Overflow (for Long-Running Processes)

```c
/* GOOD - Use 64-bit values */
uint64_t start = timeUtilMonotonicUs();
/* uint64_t overflows after ~584,000 years */

/* BAD - 32-bit overflow after ~71 minutes */
uint32_t start = (uint32_t)timeUtilMonotonicUs();
/* Wraps around! */
```

### 5. Cache Time for Batched Operations

```c
/* GOOD - Cache time for batch */
uint64_t batchTime = timeUtilMonotonicMs();
for (int i = 0; i < 1000; i++) {
    item[i].processedAt = batchTime;
}

/* BAD - Syscall per item */
for (int i = 0; i < 1000; i++) {
    item[i].processedAt = timeUtilMonotonicMs();  /* Slow! */
}
```

## Common Pitfalls

### 1. Subtracting Wall Clock Times

```c
/* DANGER - Can be negative! */
uint64_t elapsed = timeUtilUs() - startUs;
if (elapsed > timeout) {  /* May never be true if clock went backwards */
    /* ... */
}
```

### 2. Comparing Monotonic Times Across Processes

```c
/* WRONG - Monotonic time is process-specific */
/* Process A */
uint64_t timeA = timeUtilMonotonicUs();
sendToProcessB(timeA);

/* Process B */
uint64_t timeB = timeUtilMonotonicUs();
uint64_t diff = timeB - timeA;  /* Meaningless! */
```

### 3. Persisting Monotonic Time

```c
/* WRONG - Monotonic time resets on reboot */
saveToFile(timeUtilMonotonicUs());
/* After reboot */
uint64_t old = loadFromFile();
uint64_t now = timeUtilMonotonicUs();
/* old and now not comparable! */
```

## Use Cases

1. **Performance Measurement**: Benchmark code execution time
2. **Timeouts**: Implement connection, operation, or lock timeouts
3. **Rate Limiting**: Throttle API requests or operations
4. **Deadlines**: Enforce time limits on operations
5. **Profiling**: Track time spent in different code sections
6. **Retry Logic**: Implement exponential backoff
7. **Logging**: Timestamp log entries with high precision
8. **Statistics**: Track operation latencies
9. **Event Scheduling**: Determine when events should fire
10. **Cache Expiration**: Determine when cached data is stale

## See Also

- `gettimeofday(2)` - get wall clock time
- `clock_gettime(2)` - get monotonic time
- `mach_absolute_time()` - macOS monotonic time
- `time(2)` - get seconds since epoch

## Testing

TimeUtil functions can be tested for correctness and performance:

```c
#include "timeUtil.h"
#include <stdio.h>
#include <unistd.h>

void testWallClock(void) {
    uint64_t s = timeUtilS();
    uint64_t ms = timeUtilMs();
    uint64_t us = timeUtilUs();

    printf("Wall clock:\n");
    printf("  Seconds: %lu\n", s);
    printf("  Milliseconds: %lu\n", ms);
    printf("  Microseconds: %lu\n", us);

    /* Check conversions */
    assert(ms / 1000 == s);
    assert(us / 1000000 == s);
}

void testMonotonic(void) {
    uint64_t ns1 = timeUtilMonotonicNs();
    uint64_t us1 = timeUtilMonotonicUs();
    uint64_t ms1 = timeUtilMonotonicMs();

    usleep(10000);  /* Sleep 10ms */

    uint64_t ns2 = timeUtilMonotonicNs();
    uint64_t us2 = timeUtilMonotonicUs();
    uint64_t ms2 = timeUtilMonotonicMs();

    printf("Monotonic time advances:\n");
    printf("  Nanoseconds: %lu\n", ns2 - ns1);
    printf("  Microseconds: %lu\n", us2 - us1);
    printf("  Milliseconds: %lu\n", ms2 - ms1);

    /* Check time advanced */
    assert(ns2 > ns1);
    assert(us2 > us1);
    assert(ms2 >= ms1);

    /* Check conversions (approximate) */
    assert((ns2 - ns1) / 1000 > (us2 - us1) * 0.9);
    assert((ns2 - ns1) / 1000 < (us2 - us1) * 1.1);
}

void testPerformance(void) {
    const int iterations = 1000000;
    uint64_t start, end;

    /* Benchmark timeUtilUs() */
    start = timeUtilMonotonicNs();
    for (int i = 0; i < iterations; i++) {
        timeUtilUs();
    }
    end = timeUtilMonotonicNs();
    printf("timeUtilUs: %lu ns/call\n",
           (end - start) / iterations);

    /* Benchmark timeUtilMonotonicNs() */
    start = timeUtilMonotonicNs();
    for (int i = 0; i < iterations; i++) {
        timeUtilMonotonicNs();
    }
    end = timeUtilMonotonicNs();
    printf("timeUtilMonotonicNs: %lu ns/call\n",
           (end - start) / iterations);
}
```
