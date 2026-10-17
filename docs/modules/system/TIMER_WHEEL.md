# TimerWheel - Hierarchical Timing Wheel

## Overview

`timerWheel` is a **four-level hierarchical timing wheel**. Timers are bucketed
into fixed-size slot rings by how far away they expire, so registration is an
append into one slot and a tick is a walk of one slot - both O(1) amortized,
independent of how many timers exist. Slots hold their entries in a `flex`,
giving compact storage (~27 bytes per timer) and good cache locality for the
near-term timers that are touched most.

**Key Features:**

- Four wheel levels plus a sorted overflow map for very long delays
- O(1) amortized registration and O(1) amortized tick processing
- One-shot and repeating timers, microsecond input resolution
- Registration, cancellation, and stop-all from inside callbacks
- Empty spans and idle gaps are skipped, not ticked through
- Tombstone cancellation with an explicit reconcile for exact accounting
- Deterministic simulation mode (`timerWheelAdvanceTime`)
- On-demand statistics snapshot (`timerWheelGetStats`)

**Header**: `timerWheel.h`

**Source**: `timerWheel.c`, `timerWheelInternal.h`

**Dependencies**: `flex`, `intset`, `multimap`, `databox`, `timeUtil`

**Platforms**: Linux, macOS, and the BSD family (anything `flex`, `multimap`,
and `timeUtil` build on)

### Processing is NOT automatic

Nothing fires on its own. Call `timerWheelProcessTimerEvents()` periodically, or
sleep until `timerWheelNextTimerEventOffsetFromNowUs()` and then call it. Timers
that expire between calls fire on the next call; none are lost.

### Thread safety

`timerWheel` is **not** thread-safe. All calls must come from one thread, or the
caller must serialize them externally. Typical usage is a single-threaded event
loop.

### Choosing between timerWheel and multiTimer

Use `timerWheel` for large, churn-heavy timer populations where per-operation
latency matters most: it trades a fixed 448-slot structure for O(1) amortized
insert and tick. Use [multiTimer](MULTI_TIMER.md) when memory per timer matters
most - its storage is a sorted map with nothing preallocated, at O(log n) per
operation.

## Architecture

### Wheel levels

| Level    | Slots | Slot resolution | Span covered  |
| -------- | ----- | --------------- | ------------- |
| Wheel 0  | 256   | 1ms             | 256ms         |
| Wheel 1  | 64    | 256ms           | ~16.4s        |
| Wheel 2  | 64    | ~16.4s          | ~17.5min      |
| Wheel 3  | 64    | ~17.5min        | ~18.6h        |
| Overflow | -     | -               | beyond ~18.6h |

Total: 448 slots. Slot storage is allocated lazily - an untouched slot is a NULL
pointer, not an empty container. Overflow is a `multimap` sorted by expiry, so
its first entry is always the earliest long-delay timer.

### Placement

An entry is placed by its delay, measured from the wheel clock
(`currentTimeUs`, the time the wheel has processed up to) rather than from the
current time:

- delay < 1ms (below wheel 0's resolution): the **pending** queue, which fires on
  the next processing pass
- delay < the span of some wheel level: that level, at slot
  `(currentSlot + delay / levelResolution) mod levelSize`
- delay >= the total wheel span: **overflow**

Every placement (registration, repeat, re-insert of a not-yet-due entry,
pending drain, overflow pull, cascade) uses this one base. The wheel clock lags
the current time between processing passes, and the slot offset is measured
from it, so choosing the level from the same base keeps a level-L delay in
`[levelResolution, levelSize * levelResolution)` and its slot offset in
`[1, levelSize)`: an entry never wraps past the ring and never lands in the
current, already-cascaded slot.

Overflow entries move into the wheel once they are within range of the wheel
clock. A processing pass pulls them before the catch-up, so they cascade in
expiry order with the wheel's own entries, and once more after it, so an entry
that comes due during a lag longer than the wheels' coverage is placed into
pending and fires in the same pass.

Expiry arithmetic saturates rather than wraps: an absurd delay becomes
"effectively never" (`UINT64_MAX`) and parks in overflow instead of silently
firing immediately.

### Tick math

Time is tracked in microseconds relative to the wheel's creation
(`initialStartTime`); the public API converts to and from the system monotonic
clock at its boundary. The four slot indices form a base-(256, 64, 64, 64)
odometer over the wheel-0 tick count `currentTimeUs / 1000`, so any slot index
is a pure function of elapsed ticks.

Each processing pass advances wheel 0 one tick at a time until it reaches the
current time, firing the entered slot's due entries. Two shortcuts keep that
loop from being proportional to elapsed real time:

- **Occupancy bitmap**: one bit per wheel-0 slot, set iff that slot has storage
  allocated. When the current slot is empty the loop hops directly to the
  nearest occupied slot, the ring wrap point, or "now" - whichever is closest -
  in a handful of word probes instead of ticking through the empty span.
- **Empty-wheel fast-forward**: when no timers are stored at all, the loop is
  skipped entirely and the clock plus all four slot indices jump straight to the
  current tick count.

### Cascading

When wheel 0 wraps, the upper wheels advance and each **slot being entered** is
cascaded, top-down (wheel 3 before wheel 2 before wheel 1). Entry-cascade rather
than exit-cascade is what lets a timer parked in a coarse wheel still fire
within one wheel-0 slot (1ms) of its scheduled time instead of up to a full
coarse-slot span late. Top-down order lets an entry dropped by wheel N land in
wheel N-1's future slots before wheel N-1 itself cascades.

A cascaded entry is re-placed by its remaining delay: into a strictly lower
wheel level, into pending when the remainder is sub-resolution, or - if it
somehow maps back to the same or a higher level - into pending, where the same
processing pass re-places it. Cascades therefore always make downward progress.

### Callback context and deferral

The wheel tracks whether it is inside a callback. While a callback runs:

- new registrations go to the pending queue regardless of delay, because the
  slot storage may be mid-iteration
- `timerWheelProcessTimerEvents()` and the processing half of
  `timerWheelAdvanceTime()` are no-ops; the in-progress outer call picks up the
  deferred work
- `timerWheelReconcileCancellations()` refuses to run and returns false

The slot being processed is detached from the wheel before its entries are
walked, so re-inserts that target the same slot index (a repeat interval landing
exactly one revolution away, for example) cannot invalidate the walk.

## Data Types

### timerWheel

```c
typedef struct timerWheel timerWheel;
```

Opaque wheel instance. Created with `timerWheelNew()` or initialized in place
with `timerWheelInit()`.

### timerWheelId

```c
typedef uint64_t timerWheelId;
```

Unique identifier for each timer. Returned by `timerWheelRegister()` and used to
unregister. Ids are handed out monotonically starting at 1; id 0 is never valid.

### Time types

```c
typedef uint64_t timerWheelDurationUs;        /* Delay/interval in microseconds */
typedef int64_t timerWheelUs;                 /* Signed microsecond offset */
typedef int64_t timerWheelSystemMonotonicUs;  /* Monotonic time in microseconds */
```

### Timer callback

```c
typedef bool timerWheelCallback(timerWheel *tw, timerWheelId id,
                                void *clientData);
```

**Parameters:**

- `tw`: the wheel that fired this timer
- `id`: id of the firing timer
- `clientData`: user data registered with the timer

**Returns:**

- `true`: reschedule for another interval (repeating timers only)
- `false`: do not reschedule

For one-shot timers (`repeatEveryMicroseconds == 0`) the return value is
ignored.

**Safe operations inside a callback:**

- Register new timers - deferred to pending until the current pass completes
- Unregister any timer, including the one currently firing
- `timerWheelStopAll()` - timers registered afterward (even later in the same
  callback) survive
- Call `timerWheelProcessTimerEvents()` - a safe no-op
- Read `timerWheelCount()` and `timerWheelGetStats()` - the stats reconcile step
  is skipped mid-callback, so raw counters are reported

## API Reference

### Lifecycle

```c
/**
 * Create a new timer wheel instance.
 * Returns NULL on allocation failure.
 */
timerWheel *timerWheelNew(void);

/**
 * Free a timer wheel and all associated resources.
 * Safe to call with NULL.
 */
void timerWheelFree(timerWheel *tw);

/**
 * Initialize an existing timer wheel structure.
 * Returns true on success, false on allocation failure.
 */
bool timerWheelInit(timerWheel *tw);

/**
 * Deinitialize a timer wheel, freeing internal resources.
 */
void timerWheelDeinit(timerWheel *tw);
```

`timerWheelInit()` zeroes the whole structure first, so a stack- or
caller-allocated `timerWheel` inherits no garbage. It captures the monotonic
start time, allocates the overflow map, the cancellation set, and the pending
queue, and tears down whatever it already built if a later allocation fails.
`timerWheelDeinit()` frees every allocated slot, the overflow map, the
cancellation set, and the pending queue, then zeroes the structure without
freeing it.

### Timer management

```c
/**
 * Register a new timer.
 *
 * @param tw                    Timer wheel instance
 * @param startAfterMicroseconds Delay before first firing (0 = immediate)
 * @param repeatEveryMicroseconds Interval for repeating (0 = one-shot)
 * @param cb                    Callback function
 * @param clientData            User data passed to callback
 * @return Timer ID (never 0 on success)
 */
timerWheelId timerWheelRegister(timerWheel *tw,
                                timerWheelDurationUs startAfterMicroseconds,
                                timerWheelDurationUs repeatEveryMicroseconds,
                                timerWheelCallback *cb, void *clientData);

/**
 * Unregister a timer by ID.
 * ...
 * @return true if the id was ever issued by this wheel, false otherwise
 */
bool timerWheelUnregister(timerWheel *tw, timerWheelId id);

/**
 * Stop all currently registered timers.
 * Safe to call from within a timer callback; timers registered after
 * this call (including later in the same callback) are unaffected.
 */
bool timerWheelStopAll(timerWheel *tw);

/**
 * Get the count of scheduled timers, excluding cancelled ones. O(1).
 */
size_t timerWheelCount(const timerWheel *tw);

/**
 * Restore exact cancellation accounting ...
 *
 * @return true on success; false when called from within a timer
 *         callback (storage cannot be walked safely mid-processing)
 */
bool timerWheelReconcileCancellations(timerWheel *tw);
```

### Timer processing

```c
/**
 * Process all expired timers - MUST BE CALLED PERIODICALLY.
 */
void timerWheelProcessTimerEvents(timerWheel *tw);

/**
 * Advance time by specified microseconds without wall-clock dependency.
 */
void timerWheelAdvanceTime(timerWheel *tw, timerWheelDurationUs microseconds);
```

`timerWheelAdvanceTime()` shifts the wheel's start time backward by the given
amount - making "now" appear that much later - and then runs a processing pass.
Called from inside a callback the shift still applies, but the processing half
is the usual no-op, so the shift takes effect on the next pass.

### Timer queries

```c
/**
 * Get absolute monotonic time (us) of the next timer event.
 *
 * @return Absolute time in microseconds, or 0 if no timers scheduled
 */
timerWheelSystemMonotonicUs timerWheelNextTimerEventStartUs(timerWheel *tw);

/**
 * Get microseconds until the next timer event fires.
 *
 * @return Microseconds until next timer (negative if overdue, 0 if none)
 */
timerWheelUs timerWheelNextTimerEventOffsetFromNowUs(timerWheel *tw);
```

The returned absolute time shares the time base of `timeUtilMonotonicUs()`.
The result is cached: a processing pass invalidates the cache, and a new
registration folds into it (a new timer can only move the next expiry earlier),
so register-heavy workloads keep O(1) next-event queries. On a cache miss the
scan covers:

- the pending queue;
- every occupied wheel-0 slot (found through the occupancy bitmap), because a
  slot holding only cancelled entries has no live expiry and must not mask a
  later live timer;
- for each upper wheel, the first slot in ring order holding a live entry, plus
  its successor slot. An entry's slot is `floor(E / R)` or one less, so slot
  order matches expiry order to within one slot and the wheel's minimum lies in
  one of those two slots;
- the first overflow entry.

The result is 0 only when no live timer is stored. A query made from inside a
callback is answered but not cached, because the slot being processed is
detached and repeats are not yet re-inserted while the callback runs.

### Statistics

```c
typedef struct timerWheelStats {
    size_t
        totalRegistrations; /* Total timers registered since creation/reset */
    size_t totalCancellations; /* Total timers cancelled via unregister */
    size_t totalExpirations;   /* Total timer callbacks executed */
    size_t totalCascades;      /* Timer migrations between wheel levels */
    size_t overflowCount;      /* Current timers in overflow (>18.6h) */
    size_t pendingCount;       /* Current timers in the pending queue (deferred
                                * from callbacks or sub-resolution delays) */
    size_t cancelledPending;   /* Current cancelled timers awaiting lazy
                                * discard (tombstone backlog) */
    size_t memoryBytes;        /* Current memory usage in bytes */
} timerWheelStats;

/**
 * Get current statistics snapshot.
 */
void timerWheelGetStats(timerWheel *tw, timerWheelStats *stats);

/**
 * Reset statistics counters to zero.
 */
void timerWheelResetStats(timerWheel *tw);
```

`memoryBytes` is computed live and totals the wheel structure itself plus every
allocated slot, the overflow map, the cancellation set, and the pending queue.
`overflowCount`, `pendingCount`, and `cancelledPending` are current state, also
computed at call time. `timerWheelResetStats()` zeroes only the four cumulative
counters (`totalRegistrations`, `totalCancellations`, `totalExpirations`,
`totalCascades`); the current-state fields are unaffected because they are not
stored counters.

```c
/* Example */
timerWheelStats stats;
timerWheelGetStats(tw, &stats);
printf("registered=%zu fired=%zu cascades=%zu overflow=%zu "
       "pending=%zu cancelPending=%zu bytes=%zu\n",
       stats.totalRegistrations, stats.totalExpirations, stats.totalCascades,
       stats.overflowCount, stats.pendingCount, stats.cancelledPending,
       stats.memoryBytes);
```

## Contracts

### Registration

- The returned id is monotonic and never 0.
- `startAfterMicroseconds == 0` means "fire on the next processing pass": the
  delay is below wheel-0 resolution, so the entry lands in pending.
- `repeatEveryMicroseconds == 0` makes the timer one-shot and the callback's
  return value irrelevant.
- Registering from inside a callback always defers to pending, whatever the
  delay; the entry is placed properly at the end of the current pass.
- Repeat scheduling is exclusive of callback runtime: a repeat placed during
  slot processing is scheduled relative to the wheel's position just past the
  slot it fired from, and a repeat placed during pending processing is scheduled
  relative to the pass's "now". Repeating timers therefore fire once per
  interval as the wheel advances instead of stacking up missed fires.
- Repeat expiry saturates on overflow, exactly as registration does.

### Cancellation

`timerWheelUnregister()` never searches for the timer's storage. It:

1. Rejects id 0 and any id above the highest id ever issued (returns false)
2. Returns true immediately when the id is at or below the stop-all watermark
   (already cancelled wholesale)
3. Otherwise records the id in the tombstone set, widens the
   lowest/highest bounds used for fast rejection, increments the pending
   cancellation count, and marks the accounting as possibly stale

The tombstone is consulted before every dispatch, so once `timerWheelUnregister()`
returns the timer will not fire again. Its _storage_ is reclaimed lazily - the
next time processing encounters the entry (slot walk, cascade, overflow pull, or
pending walk), or when the wheel drains empty. Callers never have to wait for
that reclamation before freeing client data.

`timerWheelStopAll()` is a watermark, not n tombstones: the watermark is set to
the highest id issued so far and the tombstone set is dropped wholesale, since
every individual tombstone is at or below the watermark. Ids are monotonic, so
the watermark never needs rolling back and any timer registered afterward is
untouched. Outside a callback the slots, overflow map, occupancy bitmap, and
pending queue are cleared immediately and the accounting is exact again. Inside
a callback the entries are left in place - the wheel is being iterated - and the
watermark makes processing discard each one on encounter.

### Count exactness and reconcile

`timerWheelCount()` is `stored entries - pending cancellations`, an O(1) read
that never walks storage. It is exact except in one case: **unregistering an id
whose timer already fired**. The wheel keeps no id index, so `timerWheelUnregister()`
cannot tell that case apart inline; it plants a tombstone with no matching
stored entry, which makes the count under-report. Cancelling an already-fired id
is otherwise harmless - it never affects any other timer.

Exactness is restored in three ways:

- **Drain**: when a processing pass finds no stored entries at all, every
  remaining tombstone is by definition stale, so the set and counter are purged.
- **Explicit**: `timerWheelReconcileCancellations()` walks all stored entries
  (all four wheels, pending, overflow) in O(n), rebuilds the tombstone set from
  the ids that actually still have storage, and resets the counter and bounds.
  It returns false and does nothing when called from inside a callback, because
  the slot currently being processed is detached from the wheel and its live
  cancelled entries would be misread as orphans - dropping their tombstones
  would let them fire.
- **Automatic in stats**: `timerWheelGetStats()` runs that same reconcile first
  whenever a cancellation since the last reconcile might be stale, so
  `cancelledPending` and `timerWheelCount()` are exact afterward. A flag gates
  the walk to at most once per cancellation batch, keeping stats amortized
  cheap. Mid-callback the reconcile is skipped and raw counters are reported;
  the next out-of-callback stats call heals the accounting.

### Firing precision

Timers fire within one wheel-0 slot (1ms) of their scheduled time at every wheel
level, provided processing is called at least that often. Less frequent
processing degrades precision to the processing interval; it never loses timers.
Timers that expire while processing is not being called fire on the next call.

## Usage Example

```c
#include "timerWheel.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

typedef struct heartbeat {
    uint32_t beats;
    uint32_t maxBeats;
} heartbeat;

static bool onHeartbeat(timerWheel *tw, timerWheelId id, void *clientData) {
    (void)tw;
    heartbeat *hb = clientData;
    hb->beats++;

    printf("timer %" PRIu64 " beat %" PRIu32 "/%" PRIu32 "\n", id, hb->beats,
           hb->maxBeats);

    return hb->beats < hb->maxBeats; /* false stops the repeat */
}

static bool onTimeout(timerWheel *tw, timerWheelId id, void *clientData) {
    (void)tw;
    (void)clientData;
    printf("one-shot timer %" PRIu64 " expired\n", id);
    return false; /* ignored for one-shot timers */
}

int main(void) {
    timerWheel *tw = timerWheelNew();
    if (!tw) {
        return 1;
    }

    heartbeat hb = {.beats = 0, .maxBeats = 5};

    /* Repeat every 100ms, starting 100ms from now. */
    timerWheelRegister(tw, 100000, 100000, onHeartbeat, &hb);

    /* One-shot 2s deadline; keep the id so it can be cancelled. */
    const timerWheelId deadline = timerWheelRegister(tw, 2000000, 0, onTimeout,
                                                     NULL);

    while (timerWheelCount(tw) > 0) {
        timerWheelProcessTimerEvents(tw);

        const timerWheelUs waitUs = timerWheelNextTimerEventOffsetFromNowUs(tw);
        if (waitUs > 0) {
            usleep((useconds_t)(waitUs < 10000 ? waitUs : 10000));
        }

        if (hb.beats == hb.maxBeats) {
            /* Cancel the deadline; it will never fire now. */
            timerWheelUnregister(tw, deadline);
        }
    }

    timerWheelStats stats;
    timerWheelGetStats(tw, &stats);
    printf("registered=%zu fired=%zu cancelled=%zu bytes=%zu\n",
           stats.totalRegistrations, stats.totalExpirations,
           stats.totalCancellations, stats.memoryBytes);

    timerWheelFree(tw);
    return 0;
}
```

### Deterministic simulation

```c
#include "timerWheel.h"

#include <assert.h>
#include <stdint.h>

static bool bump(timerWheel *tw, timerWheelId id, void *data) {
    (void)tw;
    (void)id;
    (*(uint32_t *)data)++;
    return false;
}

void simulate(void) {
    timerWheel *tw = timerWheelNew();
    uint32_t fired = 0;

    timerWheelRegister(tw, 5000, 0, bump, &fired); /* fire in 5ms */

    timerWheelAdvanceTime(tw, 5000); /* jump 5ms, timer fires */
    assert(fired == 1);
    assert(timerWheelCount(tw) == 0);

    timerWheelFree(tw);
}
```

### Event loop integration

```c
/* Pattern A: fixed interval polling */
while (running) {
    timerWheelProcessTimerEvents(tw);
    usleep(1000);
}

/* Pattern B: sleep until the next timer (efficient for sparse timers) */
while (running) {
    timerWheelProcessTimerEvents(tw);
    const timerWheelUs waitUs = timerWheelNextTimerEventOffsetFromNowUs(tw);
    if (waitUs > 0) {
        usleep((useconds_t)(waitUs < 10000 ? waitUs : 10000));
    }
}

/* Pattern C: with select/poll/epoll */
while (running) {
    const timerWheelUs timeoutUs = timerWheelNextTimerEventOffsetFromNowUs(tw);
    struct timeval tv = {timeoutUs / 1000000, timeoutUs % 1000000};
    select(nfds, &readfds, NULL, NULL, timeoutUs > 0 ? &tv : NULL);
    timerWheelProcessTimerEvents(tw);
    /* ... handle I/O ... */
}
```

## Complexity

| Operation      | Complexity               | Notes                                                             |
| -------------- | ------------------------ | ----------------------------------------------------------------- |
| Register       | O(1) amortized           | Append into one slot flex                                         |
| Unregister     | O(log c)                 | c = outstanding cancellations; O(1) when none                     |
| StopAll        | O(allocated slots)       | O(1) inside a callback (watermark only)                           |
| Process (tick) | O(1) amortized           | Plus O(k) for k fired timers; occasional cascades                 |
| Cascade        | O(entries in slot)       | Once per upper-wheel slot entry                                   |
| Count          | O(1)                     | Never walks storage                                               |
| Reconcile      | O(n)                     | n = all stored entries                                            |
| Next event     | O(1) cached              | Miss: pending, wheel-0 slots, 2 slots per level, overflow head    |
| GetStats       | O(slots) + optional O(n) | Memory walk; reconcile at most once per cancellation batch        |

**Memory:**

- Fixed: 448 slot pointers plus the wheel-0 occupancy bitmap, allocated with the
  structure; slot storage itself is lazy
- Per timer: ~27 bytes (one 5-element flex entry: expiry, callback, client data,
  id, repeat interval)
- `timerWheelGetStats().memoryBytes` reports the exact live total

**Timing:**

- Input resolution: 1 microsecond; scheduling resolution: 1ms (wheel 0's slot)
- Sub-millisecond delays fire on the next processing pass via the pending queue
- Accuracy depends on `timerWheelProcessTimerEvents()` call frequency

## Relationship to multiTimer

`timerWheel` and [multiTimer](MULTI_TIMER.md) solve the same problem with
opposite trade-offs and present near-identical APIs (`Register`, `Unregister`,
`StopAll`, `Count`, `ProcessTimerEvents`, `NextTimerEventStartUs`,
`NextTimerEventOffsetFromNowUs`, `GetStats`), so switching between them is
mostly mechanical.

| Aspect                | timerWheel                                                  | multiTimer                              |
| --------------------- | ----------------------------------------------------------- | --------------------------------------- |
| Storage               | 448 fixed slots of `flex` + overflow map                    | One sorted `multimap`                   |
| Register / Unregister | O(1) amortized / O(log c)                                   | O(log n) / O(log n)                     |
| Fire order            | Slot order; unsorted within a slot                          | Strict expiry order                     |
| Per-timer memory      | ~27 bytes                                                   | ~50-60 bytes                            |
| Baseline memory       | Fixed slot arrays always present                            | Nothing preallocated                    |
| Count exactness       | Exact; needs reconcile after cancelling an already-fired id | Exact; reconciles lazily inside `Count` |

Notable behavioral differences:

- `multiTimerCount()` reconciles stale cancellation accounting itself;
  `timerWheelCount()` is a pure O(1) read, and the reconcile is a separate call
  (`timerWheelReconcileCancellations()`) that `timerWheelGetStats()` also
  triggers.
- `timerWheel` fires the timers within a slot in insertion order, not expiry
  order; `multiTimer` fires in strict expiry order. Code that depends on
  ordering among timers due in the same millisecond should use `multiTimer`.
- `timerWheel` adds `timerWheelAdvanceTime()` for deterministic simulation with
  no wall-clock dependency.

## See Also

- [multiTimer](MULTI_TIMER.md) - the memory-oriented alternative scheduler
- [timeUtil](TIME_UTIL.md) - monotonic clock behind the wheel's time base
- [flex](../flex/FLEX.md) - slot storage
- [multimap](../multimap/MULTIMAP.md) - overflow storage
