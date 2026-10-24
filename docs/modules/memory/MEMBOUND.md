# membound — Bounded Buddy Memory Allocator

## Overview

`membound` is a **bounded memory allocator**: it serves
`malloc`/`calloc`/`realloc`/`free` semantics from memory it obtains directly via
`mmap`, using a power-of-two **buddy allocation** algorithm. After creation it
makes no per-call trips to the system allocator.

It is offered in four **deployment tiers** that trade metadata overhead for
features, from ~56-byte single-extent pools for millions of coroutines up to a
multi-extent pool with an opt-in thread cache for large multithreaded servers.

membound is **external-only**: it is used through the allocator configuration,
never embedded inside datakit's own data structures.

**Header**: `membound.h` **Source**: `membound.c`, `memboundInternal.h`
**Origin**: buddy core originally derived from SQLite's `mem5.c` (2007, public
domain); algorithm from J. M. Robson, "Bounds for Some Functions Concerning
Dynamic Storage Allocation", JACM 21(8), July 1974

**Key properties**

- Power-of-two buddy allocation with automatic coalescing of free blocks
- O(1) free-class lookup (bitmap + CTZ); O(log n) split/coalesce
- Minimum allocation atom of 256 bytes (`MEMBOUND_MIN_ATOM`)
- Largest single block is `szAtom << LOGMAX` for the tier - LOGMAX is 8 for
  MICRO, 16 for COMPACT, 30 for STANDARD/ENTERPRISE - and is further bounded by
  the extent's capacity
- Dynamic growth via additional "extents" (STANDARD/ENTERPRISE tiers)
- Tiered thread-safety: none / optional / global mutex (see [Thread safety](#thread-safety))
- Process-shared capable (`mmap`, fork-safe pools)
- SIMD-accelerated `calloc` zeroing and multi-extent ownership lookup
- Optional pressure callbacks and opt-in relocating compaction
- Exact free validation on every tier: a free is honoured only for the start
  of a block currently handed out (a double free, an interior, misaligned,
  unowned or pre-reset pointer is refused), counted in
  `memboundStats.safetyViolations` on STANDARD, ENTERPRISE and COMPACT

## Tiers at a glance

| Tier       | Create with                                                        | Mutex    | Growth        | Overhead | Best for                                            |
| ---------- | ------------------------------------------------------------------ | -------- | ------------- | -------- | --------------------------------------------------- |
| MICRO      | `memboundCreateMicro(size)`                                        | none     | no            | ~56 B    | millions of tiny pools (≤ 64 KB), coroutines/fibers |
| COMPACT    | `memboundCreateCompact(size, threadSafe)`                          | optional | no            | ~192 B   | thousands of per-connection / task-local pools      |
| STANDARD   | `memboundCreate` / `memboundCreateFixed` / `memboundCreateDynamic` | global   | yes (dynamic) | ~450 B   | general-purpose, servers                            |
| ENTERPRISE | `memboundCreateEnterprise(initial, max)`                           | global   | yes           | ~650+ B  | large multithreaded workloads                       |

ENTERPRISE is STANDARD plus an extent-selection cache hint, extended profiling,
and access to the opt-in thread cache. It uses the **same single global lock**
as STANDARD.

Or build a `memboundConfig` with a preset and call `memboundCreateWithConfig`:
`memboundPresetCoroutine` (MICRO/4 KB), `memboundPresetConnection(threadSafe)`
(COMPACT/64 KB), `memboundPresetEmbedded(max)` (COMPACT/fixed),
`memboundPresetGeneral` (STANDARD/1 MB, unlimited growth), `memboundPresetDatabase`
(ENTERPRISE/64 MB, size-class strategy).

`MEMBOUND_TIER_STANDARD` is 0, so a zero-initialized `memboundConfig` gets the
STANDARD tier, `MEMBOUND_MODE_FIXED`, and `MEMBOUND_STRATEGY_FIRST_FIT`.
`memboundCreateEnterprise()` leaves the thread cache **off**; enable it through
`config.threadCache` with `memboundCreateWithConfig()`.

The tier is fixed for the pool's lifetime. MICRO and COMPACT are always
fixed-mode single-extent pools; STANDARD and ENTERPRISE support dynamic growth.

### When to use membound

Use it when you need strict memory bounds, predictable behaviour, an isolated
heap, process-shared memory, or per-task arena allocation. Avoid it when you need
sub-256-byte granularity, single allocations above 1 GiB, or when the system
allocator is already adequate.

## API

### Creation and shutdown

```c
membound *memboundCreate(size_t size);                 /* fixed, STANDARD */
membound *memboundCreateFixed(size_t size);            /* fixed, STANDARD */
membound *memboundCreateDynamic(size_t initialSize,    /* dynamic, STANDARD */
                                size_t maxSize,         /*   0 = unlimited  */
                                size_t growthSize);     /*   0 = use initial */
membound *memboundCreateMicro(size_t size);            /* MICRO (≤ 64 KB)  */
membound *memboundCreateCompact(size_t size, bool threadSafe);
membound *memboundCreateEnterprise(size_t initialSize, size_t maxSize);
membound *memboundCreateWithConfig(const memboundConfig *config);

bool memboundShutdown(membound *m);       /* destroy; invalidates all pointers */
bool memboundShutdownSafe(membound *m);   /* fails if allocations outstanding  */
```

### Allocation

```c
void *memboundAlloc(membound *m, size_t nBytes);   /* NULL if 0 or exhausted */
void *memboundCalloc(membound *m, size_t count, size_t size); /* zeroed; NULL on overflow */
void *memboundRealloc(membound *m, void *p, size_t nBytes);   /* original preserved on failure */
void  memboundFree(membound *m, void *p);          /* NULL safely ignored */

/* Capability helpers (all tiers): */
size_t memboundUsableSize(const membound *m, const void *p);  /* actual block size, 0 if unowned */
size_t memboundGoodSize(const membound *m, size_t size);      /* size a request rounds up to */
void  *memboundExpand(membound *m, void *p, size_t newSize);  /* in-place grow or NULL */
void  *memboundRecalloc(membound *m, void *p, size_t count, size_t size); /* realloc + zero tail */
void  *memboundAllocAligned(membound *m, size_t nBytes, size_t alignment); /* pow2 align */

/* ENTERPRISE thread cache (no-op when the cache is disabled): */
void memboundFlushThreadCache(membound *m);
```

`nBytes == 0` returns NULL (malloc-style). A request larger than the tier's
largest block fails; `memboundGoodSize` returns 0 for such a size without
allocating. `memboundCalloc` returns NULL on `count * size` overflow.
`memboundRealloc` copies under the pool lock, so the source is safe, and it
dispatches per tier including MICRO/COMPACT. `memboundAllocAligned` exploits
buddy blocks' natural self-alignment and verify-and-fails rather than ever
under-aligning; the result is freed with plain `memboundFree`.
`memboundExpand` grows strictly in place - it returns `ptr` when the existing
block already satisfies `newSize`, else NULL with the original untouched and
**not** freed. `memboundRecalloc` zeroes only the newly grown tail; a zero
product frees `ptr` and returns NULL.

Invariant: `memboundGoodSize(m,n) == memboundUsableSize(m, memboundAlloc(m,n))`
for any `n` that allocates successfully.

### Pool management and dynamic mode

```c
void   memboundReset(membound *m);                  /* bulk free all; invalidates pointers */
bool   memboundIncreaseSize(membound *m, size_t size); /* grow only when empty */
bool   memboundGrowBy(membound *m, size_t additionalBytes); /* pre-warm an extent */
size_t memboundShrink(membound *m);                 /* release empty non-primary extents */
size_t memboundCompact(membound *m);                /* consolidate (see Compaction) */
```

### Strategy, queries, and callbacks

```c
memboundExtentStrategy memboundSetStrategy(membound *m, memboundExtentStrategy s);
memboundExtentStrategy memboundGetStrategy(const membound *m);
void  memboundSetMinOccupancy(membound *m, float threshold);
void  memboundSetShrinkThreshold(membound *m, float threshold);
float memboundGetFragmentation(const membound *m);

memboundMode memboundGetMode(const membound *m);
memboundTier memboundGetTier(const membound *m);
size_t memboundExtentCount(const membound *m);
size_t memboundCurrentAllocationCount(const membound *m);
size_t memboundBytesUsed(const membound *m);
size_t memboundBytesAvailable(const membound *m);
size_t memboundCapacity(const membound *m);
size_t memboundMaxSize(const membound *m);
bool   memboundCanGrow(const membound *m);
bool   memboundOwns(const membound *m, const void *p);
bool   memboundGetExtentInfo(const membound *m, size_t index, memboundExtentInfo *info);
void   memboundGetStats(const membound *m, memboundStats *stats);

void memboundSetPressureCallback(membound *m, memboundPressureCallback cb, void *userData);
void memboundSetRelocationCallback(membound *m, memboundRelocationCallback cb, void *userData);
```

Selection strategies (dynamic mode): `FIRST_FIT` (default), `BEST_FIT`,
`WORST_FIT`, `SIZE_CLASS`, `ADAPTIVE`. Pressure levels: `LOW` (< 50%),
`MEDIUM` (50–80%), `HIGH` (80–95%), `CRITICAL` (≥ 95% or alloc failed). Pressure
callbacks are invoked **outside** the lock, so they may safely call back into the
pool.

## Thread safety

| Tier       | Guarantee                                                                   |
| ---------- | --------------------------------------------------------------------------- |
| MICRO      | none — the **caller** must serialize all access                             |
| COMPACT    | mutex only if created with `threadSafe = true`                              |
| STANDARD   | fully thread-safe via a global mutex                                        |
| ENTERPRISE | same global mutex; multithread _scaling_ comes from the opt-in thread cache |

Where a mutex is present it is `PTHREAD_PROCESS_SHARED`, so pools placed in shared
memory work across `fork()`. (After fork, a pointer freed in one process is freed
for both — the pool is shared, not copied.)

ENTERPRISE uses the same single global lock as STANDARD — there is **no**
per-extent locking. See
[MEMBOUND_MT_STRATEGY_DECISION.md](../../MEMBOUND_MT_STRATEGY_DECISION.md) for why
the thread cache is the scaling mechanism instead.

**Fully thread-safe** (concurrent from any thread): alloc/calloc/realloc/free,
every query and stats call, strategy and threshold setters, and
`memboundGrowBy` / `memboundShrink` / `memboundCompact` / `memboundIncreaseSize`.

**Requires exclusive access** (no concurrent calls, and no other thread may hold
pool pointers): `memboundShutdown`, `memboundShutdownSafe`, `memboundReset`.

The lock itself uses a spin-then-block pattern: `pthread_mutex_trylock` first,
then a brief bounded spin with a CPU pause hint (x86 `PAUSE`, ARM64 `YIELD`),
and only then a blocking lock with wait-time profiling. `memboundGetStats`
reports `lockAcquisitions`, `lockContentions`, `lockWaitTimeNs`, and
`contentionPercent`; a `contentionPercent` above ~5% is the signal to enable the
thread cache. In dynamic mode the `mmap` for a new extent runs **outside** the
mutex so other threads keep working during extent creation.

Pressure callbacks are invoked outside the mutex, from the thread that triggered
the level change, and may safely call membound functions on the same pool. The
relocation callback is the opposite: it runs **with the lock held** and must not
re-enter the pool.

### Thread-local cache (ENTERPRISE, opt-in)

Set `config.threadCache = true` on an ENTERPRISE pool to add a per-thread cache on
both sides of the allocator, cutting global-lock traffic for hot multithreaded
shared-pool workloads:

- **Alloc** pops a block from a per-thread, per-size-class free list with no lock;
  on a miss it refills a batch (`config.threadCacheBatch`, default 64) under a
  single lock.
- **Free** buffers the pointer in a per-thread array and returns the whole batch
  to the buddy core under a single lock when full.

**Tuning `threadCacheBatch`** (0 = default 64, clamped to 1..256): this is the
throughput-vs-memory knob. A larger batch means fewer locked refills/flushes —
more multithreaded throughput — but each thread reserves up to `batch` blocks per
active size class out of the (bounded) pool, and returns diminish as the cost
asymptotes to the buddy core's inherent per-op work. Tune it **down** for many
threads under strict memory bounds; **up** for throughput when reservation is
cheap. Measure with the `memboundBench` target under the cmake `perf` preset —
sanitizer builds invalidate these numbers entirely.

Each thread's cache is drained automatically at thread exit; call
`memboundFlushThreadCache(m)` to drain the calling thread's cache early (e.g.
before `memboundShutdownSafe`, since cached/buffered blocks count as outstanding).
`memboundReset` invalidates every thread's cache safely via an epoch counter — a
stale cache discards its (already-freed) blocks instead of double-freeing.

The thread cache is membound's multithread mechanism, but it does **not** turn
membound into a lock-free allocator — it reduces lock _traffic_, not the
serialized buddy work underneath. Reach for it when you want a bounded, arena, or
process-shared membound pool that also behaves better under threads. If none of
those properties matter and raw multithreaded throughput does, membound is the
wrong tool: see the allocator configuration for the general-purpose backend.

## Compaction

`memboundCompact` reduces extent count:

- **Without** a relocation callback it behaves like `memboundShrink` — it only
  releases fully empty non-primary extents; nothing is moved.
- **With** a callback registered via `memboundSetRelocationCallback`
  (STANDARD/ENTERPRISE), it relocates live allocations out of sparse non-primary
  extents into free space in other existing extents, invokes the callback for
  each moved block so you can fix up your pointers, and releases the emptied
  extents. It never grows the pool and never moves out of the primary extent.

```c
/* Example: a slot table the callback keeps consistent across moves. */
static void relocate(void *oldPtr, void *newPtr, size_t size, void *ud) {
    SlotTable *t = ud;
    for (size_t i = 0; i < t->n; i++) {
        if (t->slot[i] == oldPtr) { t->slot[i] = newPtr; break; }
    }
    /* MUST NOT call back into the same membound here. */
}

memboundSetRelocationCallback(m, relocate, &table);
size_t released = memboundCompact(m);   /* extents freed after consolidation */
```

Callback contract (from `membound.h`):

- Called once per moved block, **with the pool lock held**. The callback must
  not call back into this same membound instance — that deadlocks.
- Compaction is exclusive: the application must ensure no other thread touches a
  moved block for the duration of the call.
- Both `oldPtr` and `newPtr` hold valid copies of the data while the callback
  runs. `oldPtr` is freed only after the callback returns and must not be used
  afterward.
- `size` is the block's _allocated_ size (a power-of-two multiple of the atom
  size, >= the original request), so copying the whole block is safe.
- The relocation callback is STANDARD/ENTERPRISE only. It is ignored for
  MICRO/COMPACT, which have a single inline extent and nothing to compact.

`memboundShrink` is the non-moving sibling: it scans all extents and releases
every fully empty non-primary one, ignoring `shrinkThreshold`. Automatic release
is different again — after each free, only the extent just freed from is checked,
and it is released if empty and below `shrinkThreshold` (0.0 disables automatic
release entirely). "Shrink" never returns part of an extent: buddy allocators
release whole extents or nothing.

## Usage patterns

### Arena allocation (recommended)

Allocate freely during a unit of work, then bulk-free with `memboundReset` (reuse
the pool) or `memboundShutdown` (release it). No per-object frees, no accumulated
fragmentation, trivial leak detection.

```c
void handleRequest(Request *req) {
    membound *arena = memboundCreate(1 << 20);     /* 1 MB */
    char   *buf  = memboundAlloc(arena, 4096);
    Node   *node = memboundCalloc(arena, 100, sizeof(Node));
    /* ... use arena memory ... */
    memboundShutdown(arena);                        /* all freed at once */
}
```

For per-coroutine or per-connection pools, prefer the MICRO/COMPACT tiers or the
matching presets — they have a fraction of the metadata overhead.

### Sizing and fragmentation

Round requests to powers of two to avoid internal waste (a 300-byte request uses
a 512-byte block). Size pools at roughly 2× peak working set. External
fragmentation is structurally absent; internal fragmentation is Robson-bounded.
When the number of extents (not bytes) is the concern, register a relocation
callback and call `memboundCompact`.

## Common pitfalls

- **MICRO/COMPACT-without-`threadSafe` are not synchronized** — you must lock.
- **`memboundIncreaseSize` requires an empty pool**; for dynamic growth use
  `memboundCreateDynamic` / `memboundGrowBy`.
- **Shared-after-fork**: a pool in shared memory is shared, not duplicated.
- **Always check allocation results** and `memboundShutdownSafe`'s return value
  to catch leaks.
- **Invalid frees are swallowed, not fatal.** A double-free, a pointer into a
  block's interior, an out-of-bounds or misaligned pointer, a pointer from
  another pool, or one handed out before `memboundReset` is validated before
  any state changes, silently ignored, and counted (MICRO refuses without
  counting). Check `memboundStats.safetyViolations` — a non-zero value is a bug
  in your code, and nothing else will tell you about it. Assert it is zero at
  checkpoints in debug builds; log it in production.
- **Lifetime/peak/pressure/lock stats are STANDARD and ENTERPRISE only.** On
  MICRO and COMPACT `totalAllocations`, `peakBytesUsed`, `usagePercent`, and the
  lock-contention fields read as 0 (and `currentPressure` as `LOW`) — that is
  "not tracked", not "nothing happened".
- **`memboundGoodSize` returning 0** means either a zero size or a size beyond
  the tier's largest block. It is not an error code you can distinguish.
- **`memboundExpand` never falls back to moving.** NULL means "did not grow";
  the original pointer is still valid and still yours to free.

## Testing

```bash
./src/datakit-test test membound
```

The `TEST()` suite covers every tier, dynamic growth, all selection strategies,
presets, index/ownership (including the SIMD path), compaction (both shrink
fallback and relocation), the capability helpers, the opt-in thread cache
(alloc + free side, thread-exit drain, reset-during-cache, and a 4-thread
contention stress), safety (double-free / bounds / violation counters), and
multi-threaded stress tests. The correctness gate runs under the `dev` preset
(ASAN/UBSAN); the thread cache is additionally validated under a TSAN build
(`-DEnableTSAN=ON`); benchmarks use the `perf` preset. Single-thread per-op
alloc/free cost for the STANDARD / ENTERPRISE / threadCache tiers is a
deterministic row in the CI perf gate (`datakit-test test perfRegression`,
module `membound`); the multithreaded comparison lives in the separate
`memboundBench` target.

## See also

- [jebuf](JEBUF.md) — jemalloc size classes (**not** applicable to membound;
  use `memboundGoodSize` / `memboundUsableSize` instead)
- [Architecture Overview](../../ARCHITECTURE.md) — memory-management design
- [MEMBOUND_MT_STRATEGY_DECISION.md](../../MEMBOUND_MT_STRATEGY_DECISION.md) —
  why the thread cache, not lock sharding
