#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* membound - Bounded Memory Pool Allocator with Dynamic Extent Support
 *
 * A buddy allocation system with tiered metadata for different deployment
 * scales:
 *
 * DEPLOYMENT TIERS:
 *
 *   MICRO tier (~56 bytes overhead per pool):
 *     - For millions of tiny pools (coroutines, fibers, per-request allocators)
 *     - Inline buddy allocator, no separate extent struct
 *     - No mutex (caller responsible for thread safety)
 *     - No lifetime stats or pressure callbacks
 *     - Fixed mode only, max 64KB pool size
 *     - Use case: 1M coroutines in MMORPG, microservice request handlers
 *
 *   COMPACT tier (~192 bytes overhead per pool):
 *     - For thousands of lightweight pools
 *     - Single embedded extent, optional mutex
 *     - Basic stats only (current usage, allocation count)
 *     - Fixed mode only, no dynamic growth
 *     - Use case: Per-connection pools, task-local allocators
 *
 *   STANDARD tier (~450 bytes overhead per pool, default):
 *     - Full-featured implementation
 *     - Dynamic extent support with configurable growth
 *     - Full mutex protection and statistics
 *     - All strategies: first-fit, best-fit, size-class, adaptive
 *     - Use case: General purpose, server applications
 *
 *   ENTERPRISE tier (~650+ bytes overhead per pool):
 *     - For large-scale multi-threaded deployments
 *     - Per-extent locks for parallel alloc/free
 *     - Extent selection caching for O(1) repeated allocations
 *     - Extended profiling and diagnostics
 *     - Use case: Database buffer pools, high-throughput servers
 *
 * ALLOCATION MODES:
 *
 * FIXED MODE (default, original behavior):
 *   - Pre-allocated memory pool of specified size
 *   - Returns NULL when pool is exhausted
 *   - Useful for bounded memory usage, per-task isolation
 *
 * DYNAMIC MODE (STANDARD and ENTERPRISE tiers only):
 *   - Starts with initial extent, grows by adding more extents on demand
 *   - Multiple allocation strategies: fixed growth, geometric, adaptive
 *   - Optional maximum size limit
 *   - Supports shrinking by releasing empty extents
 *
 * Common features:
 *   - Buddy allocation with O(log n) alloc/free
 *   - Fast bulk deallocation via memboundReset()
 *   - Process-shared memory (works across fork) - STANDARD/ENTERPRISE only
 *   - Thread-safe via internal mutex (optional for COMPACT, N/A for MICRO)
 *   - SIMD-optimized operations where available
 *
 * ============================================================================
 * DESIGN PHILOSOPHY AND ARCHITECTURE DECISIONS
 * ============================================================================
 *
 * WHY TIERED ARCHITECTURE?
 *
 *   Modern applications have vastly different memory allocation patterns.
 *   A single allocator design forces painful trade-offs:
 *
 *   - Game engines need millions of tiny per-frame allocators (coroutines)
 *   - Web servers need thousands of per-connection pools
 *   - Databases need a few large, highly concurrent buffer pools
 *
 *   Rather than compromise on all fronts, membound provides purpose-built
 *   tiers. Each tier is optimized for its use case:
 *
 *   MICRO:      56 bytes overhead - for when you have 1M+ pools
 *               Trade-off: No mutex, no stats, max 64KB pool
 *               Why: At 1M pools, even 200 bytes overhead = 200MB wasted
 *
 *   COMPACT:   192 bytes overhead - for thousands of pools
 *               Trade-off: No dynamic growth, basic stats only
 *               Why: Connection pools don't need complex features
 *
 *   STANDARD:  450 bytes overhead - full features, reasonable default
 *               Trade-off: Single lock, no per-extent parallelism
 *               Why: Covers 90% of use cases without complexity
 *
 *   ENTERPRISE: 650+ bytes overhead - maximum performance at scale
 *               Trade-off: Higher memory overhead, more complexity
 *               Why: Database buffer pools need every optimization
 *
 * WHY BUDDY ALLOCATION?
 *
 *   The Robson buddy system (1977) provides excellent properties:
 *
 *   - O(log n) allocation and free operations
 *   - No external fragmentation (memory is always contiguous)
 *   - Predictable worst-case internal fragmentation (< 50%)
 *   - Fast coalescing without traversing free lists
 *   - Simple bitmap-based free block lookup via CTZ instruction
 *
 *   Trade-offs accepted:
 *   - Power-of-2 sizing causes some internal waste
 *   - Minimum allocation size is constrained by link structure (16 bytes)
 *   - Not optimal for tiny uniform allocations (use slab allocator)
 *
 *   The buddy system is particularly well-suited for bounded pools because:
 *   - Pool boundaries are known at creation (natural buddy tree root)
 *   - No need for complex coalescing across memory regions
 *   - Bitmap acceleration provides near-constant-time free block finding
 *
 * WHY EXTENT-BASED DYNAMIC GROWTH?
 *
 *   Traditional allocators use mremap() or realloc() to grow, but this has
 *   severe problems:
 *
 *   - Existing pointers may be invalidated
 *   - Requires contiguous virtual memory (may not be available)
 *   - Growing is all-or-nothing (can't partially fail)
 *   - Cannot easily shrink when memory is freed
 *
 *   Extent-based growth solves these:
 *
 *   - New extents are independent memory regions
 *   - Existing pointers remain valid forever
 *   - Growth can partially succeed (some extents may fail)
 *   - Empty extents can be released back to the OS
 *   - Different extents can have different characteristics
 *
 *   Each extent is a complete buddy allocator. The membound structure
 *   coordinates allocation across extents using configurable strategies.
 *
 *
 * THREAD SAFETY:
 *   All public functions are thread-safe via internal mutex with the following
 *   guarantees and exceptions:
 *
 *   FULLY THREAD-SAFE (can be called concurrently from any thread):
 *     - memboundAlloc, memboundCalloc, memboundRealloc, memboundFree
 *     - memboundBytesUsed, memboundBytesAvailable, memboundCapacity
 *     - memboundCurrentAllocationCount, memboundOwns, memboundGetStats
 *     - memboundGetExtentInfo, memboundExtentCount, memboundGetMode
 *     - memboundSetStrategy, memboundGetStrategy, memboundSetMinOccupancy
 *     - memboundSetShrinkThreshold, memboundGetFragmentation
 *     - memboundGrowBy, memboundShrink, memboundCompact, memboundIncreaseSize
 *
 *   REQUIRES EXCLUSIVE ACCESS (no concurrent calls allowed):
 *     - memboundShutdown, memboundShutdownSafe: Caller must ensure no other
 *       threads are using the pool. Pointers from the pool become invalid.
 *     - memboundReset: Caller must ensure no other threads hold pointers from
 *       this pool. All allocations are invalidated.
 *
 *   CALLBACK SAFETY:
 *     - Pressure callbacks are invoked OUTSIDE the internal mutex to prevent
 *       deadlock. Callbacks MAY safely call membound functions on the same
 * pool.
 *     - Callbacks are invoked from the thread that triggered the pressure
 * change.
 *
 *   LOCK CONTENTION PROFILING:
 *     - memboundGetStats returns lock contention metrics (lockAcquisitions,
 *       lockContentions, lockWaitTimeNs, contentionPercent) for performance
 *       analysis in multi-threaded workloads.
 *
 *   MMAP OPTIMIZATION:
 *     - When growing the pool (dynamic mode), the mmap syscall is executed
 *       OUTSIDE the mutex to allow other threads to continue working with
 *       existing extents during extent creation.
 *
 * ============================================================================
 * EXTENT RELEASE STRATEGY (NOT "SHRINKING")
 * ============================================================================
 *
 * IMPORTANT CLARIFICATION: The API uses "shrink" for familiarity, but this
 * is technically a misnomer. We cannot actually shrink an extent - buddy
 * allocators do not support partial extent release. We can only:
 *
 *   1. Release an ENTIRE extent when it becomes completely empty
 *   2. Keep an extent as-is if any allocation remains
 *
 * Why we can't truly shrink:
 *   - Buddy allocation requires power-of-2 sized pools
 *   - Coalescing depends on buddy relationships within fixed bounds
 *   - Partial release would invalidate the buddy tree structure
 *   - Moving allocations would invalidate user pointers
 *
 * What "shrink" actually does:
 *   - Scans all extents for empty ones (currentOut == 0)
 *   - Releases empty extents back to the OS via munmap
 *   - Never releases the primary extent (always kept)
 *
 * Extent release happens in two ways:
 *
 *   AUTOMATIC (on free, O(1)):
 *     After each free, we check ONLY the extent we just freed from.
 *     If it's now empty AND below shrinkThreshold, release it.
 *     This provides responsive memory release without scanning.
 *
 *   EXPLICIT (memboundShrink, O(n)):
 *     Scans all extents and releases all empty ones.
 *     Ignores shrinkThreshold - releases everything empty.
 *     Use when you want to aggressively reclaim memory.
 *
 * The shrinkThreshold setting controls automatic release:
 *   - 0.0: Never auto-release (must call memboundShrink explicitly)
 *   - 0.1: Release when extent drops to 10% usage (aggressive)
 *   - 1.0: Release only when completely empty (conservative)
 *
 * ============================================================================
 * LOCK OPTIMIZATION: SPIN-THEN-BLOCK PATTERN
 * ============================================================================
 *
 * Memory allocators are typically called in performance-critical paths.
 * Every allocation is in the "hot path" of application logic. Traditional
 * mutex locking has significant overhead:
 *
 *   - Uncontended pthread_mutex_lock: ~25-50ns (still a syscall on some OS)
 *   - Contended pthread_mutex_lock: ~1-10μs (context switch)
 *   - Buddy allocation itself: ~50-200ns
 *
 * The lock can dominate allocation time! We use three optimizations:
 *
 * 1. TRYLOCK FAST PATH:
 *    First attempt is pthread_mutex_trylock() which never blocks.
 *    If successful, we skip all contention tracking overhead.
 *    This handles the common uncontended case in ~10-20ns.
 *
 * 2. BRIEF SPIN BEFORE BLOCK:
 *    If trylock fails, we spin briefly (32 iterations) retrying.
 *    Buddy operations are fast enough that the holder likely finishes
 *    before we would complete a context switch anyway.
 *    Spinning uses CPU pause hints to reduce power and SMT interference.
 *
 * 3. BLOCK WITH PROFILING:
 *    Only if spinning fails do we actually block on the mutex.
 *    We record timing to detect systematic contention issues.
 *    High contentionPercent (>5%) suggests need for ENTERPRISE tier.
 *
 * Platform-specific spin hints:
 *   x86/x64: PAUSE instruction (reduces power, improves SMT scheduling)
 *   ARM64:   YIELD instruction (similar purpose)
 *   Other:   No-op (spinning still helps, just less efficiently)
 *
 * ENTERPRISE tier extends this with per-extent locking:
 *   - Each extent has its own mutex
 *   - Allocations to different extents can proceed in parallel
 *   - Free operations only lock the owning extent
 *   - Global lock only needed for extent list operations
 *
 * ============================================================================
 * SAFETY VIOLATION TRACKING
 * ============================================================================
 *
 * Memory corruption bugs are notoriously hard to debug. By the time a crash
 * occurs, the original error may be thousands of operations in the past.
 * membound tracks safety violations to catch bugs early:
 *
 * What we detect (all tiers):
 *   - Double-free: Freeing a pointer that's already free
 *   - Out-of-bounds: Pointer outside any extent's memory region
 *   - Misaligned: Pointer not aligned to atom boundary
 *   - Invalid extent: Pointer doesn't belong to any known extent
 *
 * How it works:
 *   - Every free validates the pointer BEFORE modifying any state
 *   - Violations increment an atomic counter (thread-safe)
 *   - Invalid frees are silently ignored (no crash, no corruption)
 *   - Counter accessible via memboundStats.safetyViolations
 *
 * Why silent handling:
 *   - Crashing on invalid free could mask the real bug
 *   - Ignoring allows program to continue (for debugging)
 *   - Counter provides clear signal something is wrong
 *   - Non-zero safetyViolations should trigger investigation
 *
 * Recommended usage:
 *   - In debug builds: Assert safetyViolations == 0 at checkpoints
 *   - In production: Log warning if safetyViolations > 0
 *   - In tests: Verify invalid operations increment the counter
 *
 * Performance impact:
 *   - Validation adds ~5-10ns per free operation
 *   - Atomic increment is relaxed memory order (minimal overhead)
 *   - No impact on allocation path
 *
 */

typedef struct membound membound;

/* ====================================================================
 * Allocation Modes
 * ==================================================================== */

typedef enum memboundMode {
    MEMBOUND_MODE_FIXED = 0, /* Single extent, fail on exhaustion */
    MEMBOUND_MODE_DYNAMIC    /* Multiple extents, grow on demand */
} memboundMode;

/* ====================================================================
 * Deployment Tiers
 * ====================================================================
 * Tiers trade off metadata overhead vs features. Choose based on:
 *   - Number of pools: 1M+ → MICRO, 1K+ → COMPACT, <100 → STANDARD/ENTERPRISE
 *   - Thread safety: Need mutex → COMPACT+, caller handles → MICRO
 *   - Features: Dynamic growth → STANDARD+, per-extent locks → ENTERPRISE
 *
 * Performance characteristics (approximate):
 *   MICRO:      56 bytes/pool,  alloc ~50ns,   no mutex overhead
 *   COMPACT:   192 bytes/pool,  alloc ~80ns,   optional mutex
 *   STANDARD:  450 bytes/pool,  alloc ~100ns,  full mutex
 *   ENTERPRISE: 650+ bytes/pool, alloc ~120ns, per-extent parallel
 *
 * Note: STANDARD is 0 so that uninitialized/default configs get STANDARD tier.
 */
typedef enum memboundTier {
    MEMBOUND_TIER_STANDARD =
        0, /* Full-featured: default behavior (0 for safe default) */
    MEMBOUND_TIER_MICRO,     /* Ultra-compact: no mutex, inline extent */
    MEMBOUND_TIER_COMPACT,   /* Lightweight: optional mutex, embedded extent */
    MEMBOUND_TIER_ENTERPRISE /* Extended: per-extent locks, profiling */
} memboundTier;

/* ====================================================================
 * Memory Pressure Levels (for callbacks)
 * ==================================================================== */

typedef enum memboundPressure {
    MEMBOUND_PRESSURE_LOW = 0, /* < 50% used */
    MEMBOUND_PRESSURE_MEDIUM,  /* 50-80% used */
    MEMBOUND_PRESSURE_HIGH,    /* 80-95% used */
    MEMBOUND_PRESSURE_CRITICAL /* > 95% used or allocation failed */
} memboundPressure;

/* Memory pressure callback type */
typedef void (*memboundPressureCallback)(membound *m, memboundPressure level,
                                         void *userData);

/* ====================================================================
 * Extent Selection Strategy (for dynamic mode)
 * ====================================================================
 *
 * When a membound pool has multiple extents (dynamic mode), the strategy
 * determines which extent receives new allocations:
 *
 * FIRST_FIT (default):
 *   - Check primary extent first, then scan all extents in order
 *   - Best for: Simple workloads, low extent counts
 *   - Trade-off: O(n) worst case, but fast for common case
 *
 * BEST_FIT:
 *   - Use the fullest extent that still has enough space
 *   - Best for: Minimizing fragmentation, long-lived allocations
 *   - Trade-off: May require more extent scans
 *
 * WORST_FIT:
 *   - Use the emptiest extent with sufficient space
 *   - Best for: Spreading allocations, reducing contention
 *   - Trade-off: May increase overall memory usage
 *
 * SIZE_CLASS:
 *   - Route allocations by size to dedicated extents:
 *     * SMALL:  size <= sizeClassSmall (default 1KB)
 *     * MEDIUM: up to sizeClassMedium (default 64KB)
 *     * LARGE:  > sizeClassMedium
 *   - Best for: Mixed workloads, database-style access
 *   - Trade-off: Requires more extents
 *
 * ADAPTIVE:
 *   - Dynamically switches between BEST_FIT and WORST_FIT
 *   - Uses BEST_FIT when fragmentation < threshold
 *   - Switches to WORST_FIT when fragmentation increases
 *   - Best for: Unpredictable workloads, self-tuning systems
 *
 * Additional controls:
 *   - minOccupancyForAlloc: Skip extents below this threshold
 *   - shrinkThreshold: Auto-release extents below this threshold
 *   - fragmentationThreshold: Trigger point for ADAPTIVE strategy
 */

typedef enum memboundExtentStrategy {
    MEMBOUND_STRATEGY_FIRST_FIT = 0, /* Use primary, then scan all (default) */
    MEMBOUND_STRATEGY_BEST_FIT,      /* Use fullest extent with space */
    MEMBOUND_STRATEGY_WORST_FIT,     /* Use emptiest extent with space */
    MEMBOUND_STRATEGY_SIZE_CLASS,    /* Route by allocation size category */
    MEMBOUND_STRATEGY_ADAPTIVE       /* Dynamic strategy based on metrics */
} memboundExtentStrategy;

/* ====================================================================
 * Configuration Structures
 * ==================================================================== */

/* Configuration for creating membound pools with full control */
typedef struct memboundConfig {
    /* Required */
    memboundMode mode;
    size_t initialSize; /* Initial pool/extent size */

    /* Tier selection (0 = STANDARD for backwards compatibility) */
    memboundTier tier;

    /* Thread safety options (for COMPACT tier) */
    bool threadSafe; /* MICRO: ignored (always false)
                      * COMPACT: true = use mutex, false = no mutex
                      * STANDARD/ENTERPRISE: ignored (always true) */

    /* Dynamic mode settings (STANDARD and ENTERPRISE tiers only) */
    size_t maxTotalSize;   /* Hard limit (0 = unlimited) */
    size_t growthSize;     /* Fixed growth: bytes per new extent */
    double growthFactor;   /* Geometric growth: multiplier (e.g., 1.5) */
    size_t maxExtentCount; /* Max extents (0 = unlimited) */

    /* Extent selection strategy (dynamic mode only) */
    memboundExtentStrategy strategy; /* Default: FIRST_FIT */
    float minOccupancyForAlloc;   /* Skip extents below this (0 = disabled) */
    float shrinkThreshold;        /* Auto-shrink below this (0 = disabled) */
    float fragmentationThreshold; /* For adaptive: switch at this level */

    /* Size-class thresholds for MEMBOUND_STRATEGY_SIZE_CLASS
     * Allocations are routed to dedicated extents based on size:
     *   - SMALL:  size <= sizeClassSmall (default: 1024 bytes / 1KB)
     *   - MEDIUM: sizeClassSmall < size <= sizeClassMedium (default: 65536 /
     * 64KB)
     *   - LARGE:  size > sizeClassMedium
     * Set to 0 to use defaults. */
    size_t sizeClassSmall;  /* Max bytes for small class (0 = 1024) */
    size_t sizeClassMedium; /* Max bytes for medium class (0 = 65536) */

    /* ENTERPRISE tier options (only used when tier = MEMBOUND_TIER_ENTERPRISE)
     */
    bool perExtentLocking; /* Enable per-extent locks for parallel alloc/free */

    /* Optional callbacks (STANDARD and ENTERPRISE tiers only) */
    memboundPressureCallback pressureCallback;
    void *pressureUserData;
} memboundConfig;

/* Extent information (for diagnostics) */
typedef struct memboundExtentInfo {
    size_t capacity;         /* Total bytes in this extent */
    size_t bytesUsed;        /* Currently allocated bytes */
    size_t allocationCount;  /* Outstanding allocations */
    const void *baseAddress; /* Pool start address (for ownership) */
    size_t index;            /* Extent index (0 = primary) */
} memboundExtentInfo;

/* ====================================================================
 * Creation and Destruction
 * ==================================================================== */

/* Create a fixed-size memory pool (original behavior, STANDARD tier).
 * Returns NULL on failure (e.g., mmap failed). */
membound *memboundCreate(size_t size);

/* Create a fixed-size pool (explicit, STANDARD tier) */
membound *memboundCreateFixed(size_t size);

/* Create a dynamic pool that grows on demand (STANDARD tier).
 * Parameters:
 *   initialSize  - Size of first extent
 *   maxSize      - Maximum total capacity (0 = unlimited)
 *   growthSize   - Size of new extents when growing (0 = use initialSize) */
membound *memboundCreateDynamic(size_t initialSize, size_t maxSize,
                                size_t growthSize);

/* Create with full configuration control */
membound *memboundCreateWithConfig(const memboundConfig *config);

/* ====================================================================
 * Tier-Specific Creation Functions
 * ==================================================================== */

/* Create a MICRO tier pool - ultra-compact, no mutex, for millions of pools.
 * Parameters:
 *   size - Pool size (max 64KB for MICRO tier)
 * Notes:
 *   - NOT thread-safe: caller must provide external synchronization
 *   - No lifetime stats, no pressure callbacks
 *   - Fixed mode only (no dynamic growth)
 *   - ~56 bytes overhead per pool */
membound *memboundCreateMicro(size_t size);

/* Create a COMPACT tier pool - lightweight with optional mutex.
 * Parameters:
 *   size       - Pool size
 *   threadSafe - true = include mutex, false = no mutex (caller synchronizes)
 * Notes:
 *   - Basic stats only (current usage, allocation count)
 *   - Fixed mode only (no dynamic growth)
 *   - ~192 bytes overhead per pool */
membound *memboundCreateCompact(size_t size, bool threadSafe);

/* Create an ENTERPRISE tier pool - full features plus per-extent parallelism.
 * Parameters:
 *   initialSize - Initial extent size
 *   maxSize     - Maximum total capacity (0 = unlimited)
 * Notes:
 *   - Per-extent locks for parallel alloc/free across extents
 *   - Extent selection caching for O(1) repeated allocations
 *   - Extended profiling and diagnostics
 *   - ~650+ bytes overhead per pool */
membound *memboundCreateEnterprise(size_t initialSize, size_t maxSize);

/* ====================================================================
 * Preset Configurations
 * ====================================================================
 * Helper functions returning pre-configured memboundConfig for common use
 * cases. Modify the returned config as needed before passing to
 * memboundCreateWithConfig.
 */

/* Preset: Coroutine/fiber pools (MICRO tier)
 * For millions of short-lived micro-allocators.
 * Returns config for 4KB pool, no thread safety. */
memboundConfig memboundPresetCoroutine(void);

/* Preset: Per-connection pools (COMPACT tier)
 * For thousands of connection-specific allocators.
 * Returns config for 64KB pool with optional thread safety. */
memboundConfig memboundPresetConnection(bool threadSafe);

/* Preset: General purpose (STANDARD tier)
 * Default full-featured configuration.
 * Returns config for 1MB initial, unlimited growth. */
memboundConfig memboundPresetGeneral(void);

/* Preset: Database/cache (ENTERPRISE tier)
 * For large shared buffer pools with high concurrency.
 * Returns config for 64MB initial, size-class strategy. */
memboundConfig memboundPresetDatabase(void);

/* Preset: Embedded/constrained (COMPACT tier, no growth)
 * For memory-constrained environments.
 * Returns config for specified size, no thread safety. */
memboundConfig memboundPresetEmbedded(size_t maxMemory);

/* Destroy the pool, releasing all memory.
 * WARNING: Invalidates all pointers from this pool! */
bool memboundShutdown(membound *m);

/* Destroy the pool only if no allocations are outstanding.
 * Returns false if allocations exist (pool not destroyed). */
bool memboundShutdownSafe(membound *m);

/* ====================================================================
 * Allocation Functions
 * ==================================================================== */

/* Allocate memory from the pool. Returns NULL if exhausted. */
void *memboundAlloc(membound *m, size_t nBytes);

/* Allocate zero-initialized memory (like calloc). */
void *memboundCalloc(membound *m, size_t count, size_t size);

/* Resize an allocation. Returns NULL on failure (original preserved). */
void *memboundRealloc(membound *m, void *pPrior, size_t nBytes);

/* Free memory back to the pool. NULL pointers are safely ignored. */
void memboundFree(membound *m, void *pPrior);

/* ====================================================================
 * Pool Management
 * ==================================================================== */

/* Reset the pool, freeing all allocations at once.
 * WARNING: Invalidates all pointers from this pool! */
void memboundReset(membound *m);

/* Grow the pool to a new size. Only works when pool is empty.
 * Returns false if allocations exist or growth fails.
 * Note: In dynamic mode, prefer memboundGrowBy() instead. */
bool memboundIncreaseSize(membound *m, size_t size);

/* ====================================================================
 * Dynamic Mode Operations
 * ==================================================================== */

/* Manually add a new extent (useful for pre-warming).
 * Returns true if extent was added, false on failure or if not allowed. */
bool memboundGrowBy(membound *m, size_t additionalBytes);

/* Release empty extents to free memory.
 * Never releases the primary extent.
 * Returns number of extents released. */
size_t memboundShrink(membound *m);

/* Release empty extents to reduce memory footprint.
 * Currently equivalent to memboundShrink() - true compaction (moving
 * allocations between extents) is not yet implemented as it would
 * invalidate pointers and require application cooperation.
 * Returns number of extents that were released. */
size_t memboundCompact(membound *m);

/* ====================================================================
 * Extent Selection Strategy (dynamic mode)
 * ==================================================================== */

/* Set extent selection strategy. Changes take effect on next allocation.
 * Returns previous strategy. */
memboundExtentStrategy memboundSetStrategy(membound *m,
                                           memboundExtentStrategy strategy);

/* Get current extent selection strategy. */
memboundExtentStrategy memboundGetStrategy(const membound *m);

/* Set occupancy threshold for allocation (0.0 - 1.0).
 * Extents below this threshold are skipped during allocation.
 * Set to 0 to disable (allocate from any extent). */
void memboundSetMinOccupancy(membound *m, float threshold);

/* Set shrink threshold (0.0 - 1.0).
 * Extents below this occupancy may be auto-released after free operations.
 * Set to 0 to disable proactive shrinking. */
void memboundSetShrinkThreshold(membound *m, float threshold);

/* Get current fragmentation ratio (0.0 = no fragmentation, 1.0 = fully
 * fragmented). Useful for adaptive strategy or diagnostics. */
float memboundGetFragmentation(const membound *m);

/* ====================================================================
 * Statistics and Queries
 * ==================================================================== */

/* Get the current allocation mode */
memboundMode memboundGetMode(const membound *m);

/* Get the current deployment tier */
memboundTier memboundGetTier(const membound *m);

/* Get the number of extents (always 1 for fixed mode) */
size_t memboundExtentCount(const membound *m);

/* Get the number of outstanding allocations. */
size_t memboundCurrentAllocationCount(const membound *m);

/* Get total bytes currently allocated (includes internal fragmentation). */
size_t memboundBytesUsed(const membound *m);

/* Get approximate bytes available for new allocations. */
size_t memboundBytesAvailable(const membound *m);

/* Get total pool capacity in usable bytes. */
size_t memboundCapacity(const membound *m);

/* Get maximum allowed size (0 = unlimited, dynamic mode only) */
size_t memboundMaxSize(const membound *m);

/* Check if pool can grow (always false for fixed mode) */
bool memboundCanGrow(const membound *m);

/* Check if a pointer belongs to this pool's memory region. */
bool memboundOwns(const membound *m, const void *p);

/* ====================================================================
 * Extent Information (for diagnostics)
 * ==================================================================== */

/* Get information about a specific extent (0-indexed).
 * Returns true if index is valid, false otherwise. */
bool memboundGetExtentInfo(const membound *m, size_t index,
                           memboundExtentInfo *info);

/* ====================================================================
 * Callbacks
 * ==================================================================== */

/* Set memory pressure callback (called when pressure level changes) */
void memboundSetPressureCallback(membound *m, memboundPressureCallback cb,
                                 void *userData);

/* ====================================================================
 * Detailed Statistics
 * ==================================================================== */

typedef struct memboundStats {
    /* Current state */
    memboundMode mode;
    memboundTier tier; /* Deployment tier (MICRO/COMPACT/STANDARD/ENTERPRISE) */
    size_t extentCount;
    size_t capacity;
    size_t bytesUsed;
    size_t bytesAvailable;
    size_t allocationCount;

    /* Limits */
    size_t maxTotalSize;
    size_t maxExtentCount;

    /* Lifetime statistics (STANDARD and ENTERPRISE tiers only)
     * For MICRO/COMPACT tiers, these are always 0. */
    uint64_t totalAllocations;    /* Lifetime allocation count */
    uint64_t totalBytesAllocated; /* Lifetime bytes (with fragmentation) */
    uint64_t totalFragmentation;  /* Lifetime internal fragmentation */

    /* Peak usage (STANDARD and ENTERPRISE tiers only)
     * For MICRO/COMPACT tiers, these are always 0. */
    uint64_t peakBytesUsed;
    uint64_t peakAllocationCount;

    /* Memory pressure (STANDARD and ENTERPRISE tiers only)
     * For MICRO/COMPACT tiers, always LOW and 0.0. */
    memboundPressure currentPressure;
    float usagePercent; /* 0.0 - 100.0 */

    /* Lock contention profiling (STANDARD and ENTERPRISE tiers only)
     * For MICRO tier (no mutex) or COMPACT tier without threadSafe, always 0.
     */
    uint64_t lockAcquisitions; /* Total lock acquisition attempts */
    uint64_t lockContentions;  /* Times lock was already held */
    uint64_t lockWaitTimeNs;   /* Total nanoseconds spent waiting */
    float contentionPercent;   /* lockContentions / lockAcquisitions * 100 */

    /* Tier-specific flags */
    bool threadSafe; /* Whether this pool has mutex protection */

    /* Safety violations (always tracked, all tiers).
     * Incremented when runtime safety checks fail (e.g., double-free,
     * out-of-bounds, invalid pointer). Non-zero indicates a bug. */
    uint64_t safetyViolations;
} memboundStats;

/* Get detailed statistics */
void memboundGetStats(const membound *m, memboundStats *stats);

#ifdef DATAKIT_TEST
int memboundTest(int argc, char *argv[]);
#endif
