# membound — Bounded Buddy Memory Allocator

## Overview

`membound` is a **bounded memory allocator**: it serves
`malloc`/`calloc`/`realloc`/`free` semantics from memory it obtains directly via
`mmap`, using a power-of-two **buddy allocation** algorithm. After creation it
makes no per-call trips to the system allocator.

It is offered in four **deployment tiers** that trade metadata overhead for
features, from ~56-byte single-extent pools for millions of coroutines up to a
multi-extent, per-extent-locked allocator for large multithreaded servers.

**Header**: `membound.h` **Source**: `membound.c`, `memboundInternal.h`
**Origin**: buddy core derived from SQLite's `mem5.c` / J. M. Robson (JACM 1974)

For internal design, structures, and the invariants that govern safe extension,
see **[MEMBOUND_ARCHITECTURE.md](MEMBOUND_ARCHITECTURE.md)**. This document is the
user-facing API and usage guide.

**Key properties**

- Power-of-two buddy allocation with automatic coalescing of free blocks
- O(1) free-class lookup (bitmap + CTZ); O(log n) split/coalesce
- Minimum allocation atom of 256 bytes; single-allocation cap of 1 GiB
- Dynamic growth via additional "extents" (STANDARD/ENTERPRISE tiers)
- Tiered thread-safety: none / optional / global mutex / global + per-extent
- Process-shared capable (`mmap`, fork-safe pools)
- SIMD-accelerated `calloc` zeroing and multi-extent ownership lookup
- Optional pressure callbacks and opt-in relocating compaction

## Tiers at a glance

| Tier       | Create with                                                        | Mutex               | Growth        | Best for                                            |
| ---------- | ------------------------------------------------------------------ | ------------------- | ------------- | --------------------------------------------------- |
| MICRO      | `memboundCreateMicro(size)`                                        | none                | no            | millions of tiny pools (≤ 64 KB), coroutines/fibers |
| COMPACT    | `memboundCreateCompact(size, threadSafe)`                          | optional            | no            | thousands of per-connection / task-local pools      |
| STANDARD   | `memboundCreate` / `memboundCreateFixed` / `memboundCreateDynamic` | global              | yes (dynamic) | general-purpose, servers                            |
| ENTERPRISE | `memboundCreateEnterprise(initial, max)`                           | global + per-extent | yes           | large multithreaded workloads                       |

Or build a `memboundConfig` with a preset and call `memboundCreateWithConfig`:
`memboundPresetCoroutine` (MICRO/4 KB), `memboundPresetConnection(threadSafe)`
(COMPACT/64 KB), `memboundPresetEmbedded(max)` (COMPACT/fixed),
`memboundPresetGeneral` (STANDARD/1 MB), `memboundPresetDatabase`
(ENTERPRISE/64 MB, size-class strategy, per-extent locking).

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
```

`nBytes == 0` returns NULL (malloc-style); single allocations are capped at
1 GiB. `memboundRealloc` copies under the pool lock, so the source is safe (and
now correctly tier-dispatches MICRO/COMPACT). `memboundAllocAligned` exploits
buddy blocks' natural self-alignment and verify-and-fails rather than ever
under-aligning. `memboundGoodSize(m,n) == memboundUsableSize(m, memboundAlloc(m,n))`.

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

ENTERPRISE uses the same single global lock as STANDARD — there is no per-extent
locking (it was removed: it taxed every allocation ~12 ns for the lock dance and
_regressed_ under contention; the thread cache is the correct scaling mechanism).
See [MEMBOUND_MT_STRATEGY_DECISION.md](../../MEMBOUND_MT_STRATEGY_DECISION.md).

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
active size class out of the (bounded) pool. Measured at 4 threads / 64 B, a shared
pool drops from ~56 ns/op at batch 16 to ~37 at 64 to ~30 at 256 (diminishing
returns — it asymptotes to the buddy core's inherent per-op cost). Tune it **down**
for many threads under strict memory bounds; **up** for throughput when reservation
is cheap.

Each thread's cache is drained automatically at thread exit; call
`memboundFlushThreadCache(m)` to drain the calling thread's cache early (e.g.
before `memboundShutdownSafe`, since cached/buffered blocks count as outstanding).
`memboundReset` invalidates every thread's cache safely via an epoch counter — a
stale cache discards its (already-freed) blocks instead of double-freeing.

The thread cache is membound's recommended multithread mechanism — measured
~**5× faster** than plain ENTERPRISE and ~1.6× faster than STANDARD at 4 threads.
It does **not** turn membound into a lock-free allocator — it reduces lock
_traffic_, not the serialized buddy work, and stays well behind `memshard`. **For
hot multithreaded paths prefer `memshard`** (the lock-free per-thread-heap
allocator); reach for `threadCache` when you want a bounded/arena/process-shared
membound pool that _also_ behaves better under threads.

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

The callback runs with the pool lock held and **must not** re-enter the same
pool; the application must ensure no other thread touches a block while it is
being moved. See [MEMBOUND_ARCHITECTURE.md](MEMBOUND_ARCHITECTURE.md) §8 for the
full contract.

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
(ASAN/UBSAN); the thread cache and the ENTERPRISE per-extent path are additionally
validated under a TSAN build (`-DEnableTSAN=ON`, 0 races); benchmarks use the
`perf` preset. Single-thread per-op alloc/free cost for the STANDARD / ENTERPRISE /
threadCache tiers is a deterministic row in the CI perf gate
(`datakit-test test perfRegression`, module `membound`); the multithreaded
comparison lives in the separate `memboundBench` target.

## See also

- [MEMBOUND_ARCHITECTURE.md](MEMBOUND_ARCHITECTURE.md) — internals & invariants
- [Architecture Overview](../../ARCHITECTURE.md) — memory-management design
