#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h> /* for ssize_t */

/* ====================================================================
 * Multi-Level LRU Cache (S4LRU-style with N configurable levels)
 * ====================================================================
 *
 * OVERVIEW
 * --------
 * A memory-efficient segmented LRU implementation featuring O(1) operations,
 * adaptive entry width, and full S4LRU demotion semantics for scan-resistant
 * caching.
 *
 * ARCHITECTURE
 * ------------
 * The cache maintains N levels (default 7, max 64). New entries start at
 * level 0. Each access ("hit") promotes an entry to the next higher level.
 * On eviction, entries demote from level N to N-1 (second chance) until
 * reaching level 0, where true eviction occurs.
 *
 *   Level 6 (hottest)  ------>  [MRU] <- entries <- [LRU]
 *   Level 5            ------>  [MRU] <- entries <- [LRU]
 *   ...
 *   Level 0 (coldest)  ------>  [MRU] <- entries <- [LRU] --> EVICT
 *
 * This S4LRU approach protects frequently-accessed items from being evicted
 * by a scan of new items (scan resistance), while still allowing cold items
 * to age out naturally.
 *
 * ADAPTIVE ENTRY WIDTH
 * --------------------
 * Entry metadata automatically scales based on cache size using finer-grained
 * tiers to minimize migration costs:
 *
 *   Width  Address Bits  Max Entries  Migration Cost (from prev tier)
 *   -----  ------------  -----------  --------------------------------
 *    5       16 bits       64K        -
 *    6       20 bits        1M        64K × 1B =  64KB
 *    7       24 bits       16M         1M × 1B =   1MB
 *    8       28 bits      256M        16M × 1B =  16MB
 *    9       32 bits        4B       256M × 1B = 256MB
 *   10       36 bits       64B         4B × 1B =   4GB
 *   11       40 bits        1T        64B × 1B =  64GB
 *   12       44 bits       16T         1T × 1B =   1TB
 *   16       60 bits        1E        16T × 4B =  64TB
 *
 * Width automatically upgrades during growth; entries are migrated seamlessly.
 * Each tier transition adds only 1 byte per entry (except 12→16), minimizing
 * the cost of growing large caches.
 *
 * MEMORY BUDGET
 * -------------
 * Per-entry overhead (LRU metadata only, excludes your cached data):
 *   - Without weights: 5-16 bytes/entry (adaptive based on scale)
 *   - With weights:    13-24 bytes/entry (8 bytes added for weight tracking)
 *
 * Fixed overhead: ~180 bytes + 24 bytes per level
 *
 * PERFORMANCE
 * -----------
 * All core operations are O(1):
 *   - Insert:   ~1 billion ops/sec
 *   - Promote:  ~1 billion ops/sec
 *   - Remove:   ~1.5 billion ops/sec
 *   - Query:    ~1 billion ops/sec
 *
 * The levelMask bitmap enables O(1) lowest-entry lookup regardless of
 * how many levels are configured.
 *
 * THREAD SAFETY
 * -------------
 * NOT thread-safe. External synchronization required for concurrent access.
 * For read-heavy workloads, consider read-write locks.
 *
 * USAGE EXAMPLE - Basic Cache
 * ---------------------------
 *   // Create cache with default settings (7 levels)
 *   multilru *cache = multilruNew();
 *
 *   // Insert entry (returns handle for future reference)
 *   multilruPtr handle = multilruInsert(cache);
 *   myHashMap[key] = handle;  // Store handle in your data structure
 *
 *   // On cache hit - promote entry
 *   multilruIncrease(cache, myHashMap[key]);
 *
 *   // Evict LRU entry when full
 *   multilruPtr evicted;
 *   if (multilruRemoveMinimum(cache, &evicted)) {
 *       // Entry was demoted or evicted
 *       // If evicted from level 0, handle is now invalid
 *   }
 *
 *   multilruFree(cache);
 *
 * USAGE EXAMPLE - Size-Limited Cache
 * ----------------------------------
 *   multilruConfig config = {
 *       .maxLevels = 7,
 *       .startCapacity = 10000,
 *       .policy = MLRU_POLICY_COUNT,
 *       .maxCount = 10000,  // Auto-evict when > 10000 entries
 *   };
 *   multilru *cache = multilruNewWithConfig(&config);
 *
 *   // Inserts automatically trigger eviction when over limit
 *   for (int i = 0; i < 100000; i++) {
 *       multilruInsert(cache);  // Cache stays at ~10000 entries
 *   }
 *
 * USAGE EXAMPLE - Weight-Based Cache (e.g., Video Cache)
 * ------------------------------------------------------
 *   multilruConfig config = {
 *       .maxLevels = 7,
 *       .policy = MLRU_POLICY_SIZE,
 *       .maxWeight = 15ULL * 1024 * 1024 * 1024,  // 15GB
 *       .enableWeights = true,
 *   };
 *   multilru *cache = multilruNewWithConfig(&config);
 *
 *   // Insert 12GB video
 *   multilruPtr bigVideo = multilruInsertWeighted(cache, 12ULL * 1024 * 1024 *
 * 1024);
 *
 *   // Insert 100MB video
 *   multilruPtr smallVideo = multilruInsertWeighted(cache, 100 * 1024 * 1024);
 *
 *   // Eviction prefers removing cold large items
 *   // Frequently-accessed small items survive
 *
 * ERROR HANDLING
 * --------------
 * - multilruInsert() returns 0 on allocation failure
 * - Functions accepting multilruPtr silently ignore invalid/out-of-bounds
 * pointers
 * - multilruRemoveMinimum() returns false when cache is empty
 *
 * CONFIGURATION GUIDE
 * -------------------
 * Levels: More levels = better scan resistance, slight memory overhead
 *   - 4 levels: Light protection, minimal overhead
 *   - 7 levels: Good balance (default)
 *   - 16+ levels: Strong protection for adversarial workloads
 *
 * Policy:
 *   - MLRU_POLICY_COUNT: Simple entry count limit
 *   - MLRU_POLICY_SIZE: Total weight/size limit (requires enableWeights)
 *   - MLRU_POLICY_HYBRID: Both limits enforced
 *
 * Evict Strategy:
 *   - MLRU_EVICT_LRU: Pure LRU (coldest first)
 *   - MLRU_EVICT_SIZE_WEIGHTED: Prefer evicting large cold items
 *   - MLRU_EVICT_SIZE_LRU: Balance of recency and size
 *
 * FUTURE WORK (TODO)
 * ------------------
 * The following optimizations are planned for future implementation:
 *
 * 1. SHRINK COMPACTION
 *    When many entries are deleted, the free list accumulates "holes" (recycled
 *    slot indices) scattered throughout the entry array. This is fine for
 * memory usage since the slots are reused, but it means:
 *    - nextFresh stays high even with few active entries
 *    - The entry array may be oversized relative to actual usage
 *
 *    Future feature: multilruCompact() would:
 *    - Relocate entries to fill holes, creating a dense array
 *    - Update all prev/next pointers during relocation
 *    - Reset nextFresh to (maxLevels + 1 + count)
 *    - Allow shrinking the entry array via realloc
 *    - Return a mapping of old->new indices for caller's hash table updates
 *
 *    Architecture considerations:
 *    - Must pause operations during compaction (or use copy-on-write)
 *    - Could track fragmentation ratio: freeCount / (nextFresh - maxLevels - 1)
 *    - Trigger compaction when fragmentation > threshold (e.g., 50%)
 *    - Alternative: background incremental compaction
 *
 * 2. WIDTH DOWNGRADE
 *    Currently width only upgrades (5→6→7→...→16). After mass deletion, entries
 *    might fit in a smaller width. Downgrade would:
 *    - Save memory per entry (e.g., 8 bytes → 6 bytes per entry)
 *    - Require compaction first to ensure all indices fit in smaller width
 *    - Only practical after significant deletions (e.g., 90% reduction)
 *
 * 3. TIERED STORAGE (alternative to migration)
 *    Instead of uniform width with migration, use separate arrays per tier:
 *    - Tier 0: indices 1-64K use 5-byte entries
 *    - Tier 1: indices 64K-1M use 6-byte entries
 *    - etc.
 *    Eliminates migration entirely but adds tier lookup overhead per access.
 *    See tiered-multilru-analysis.md for detailed design.
 */

typedef struct multilru multilru;

/* Entry pointer type - used to reference entries in the LRU.
 * The actual width varies based on cache scale, but we use size_t
 * for the external API to ensure compatibility.
 * Value 0 indicates invalid/null pointer.
 *
 * ID LIFECYCLE
 * ------------
 * - IDs are stable: once assigned, an ID refers to that entry until
 *   explicitly deleted or evicted
 * - IDs are recycled: after delete/eviction, the ID may be reused for
 *   a future insert (LIFO order - most recently freed ID reused first)
 * - IDs are dense-ish: allocation prefers recycled IDs, then sequential
 *   fresh IDs, so ID values stay relatively compact
 *
 * IMPORTANT: After eviction/delete, the old ID becomes invalid. If you
 * store IDs externally (e.g., in a hash map), you MUST remove stale
 * mappings when entries are evicted. Using a stale ID may reference
 * a different entry if the ID was recycled. */
typedef size_t multilruPtr;

/* ====================================================================
 * Policy Configuration
 * ==================================================================== */

/* Eviction trigger policy - determines WHEN to evict */
typedef enum multilruPolicy {
    MLRU_POLICY_COUNT = 0, /* Evict when count > maxCount */
    MLRU_POLICY_SIZE,      /* Evict when totalWeight > maxWeight */
    MLRU_POLICY_HYBRID,    /* Evict when either limit exceeded */
} multilruPolicy;

/* Victim selection strategy - determines WHAT to evict */
typedef enum multilruEvictStrategy {
    MLRU_EVICT_LRU = 0,       /* Pure LRU: lowest level, oldest entry */
    MLRU_EVICT_SIZE_WEIGHTED, /* Prefer evicting large cold items */
    MLRU_EVICT_SIZE_LRU,      /* LRU but account for size freed */
} multilruEvictStrategy;

/* Entry width modes (selected automatically based on capacity) */
typedef enum multilruEntryWidth {
    MLRU_WIDTH_5 = 5,   /* 16-bit indices, max 64K entries */
    MLRU_WIDTH_6 = 6,   /* 20-bit indices, max 1M entries */
    MLRU_WIDTH_7 = 7,   /* 24-bit indices, max 16M entries */
    MLRU_WIDTH_8 = 8,   /* 28-bit indices, max 256M entries */
    MLRU_WIDTH_9 = 9,   /* 32-bit indices, max 4B entries */
    MLRU_WIDTH_10 = 10, /* 36-bit indices, max 64B entries */
    MLRU_WIDTH_11 = 11, /* 40-bit indices, max 1T entries */
    MLRU_WIDTH_12 = 12, /* 44-bit indices, max 16T entries */
    MLRU_WIDTH_16 = 16, /* 60-bit indices, max ~1 quintillion entries */
} multilruEntryWidth;

/* Full configuration for creating a new multilru */
typedef struct multilruConfig {
    size_t maxLevels;      /* Number of LRU levels (default: 7, max: 64) */
    size_t startCapacity;  /* Initial entry capacity (0 = auto) */
    uint64_t maxWeight;    /* Max total weight in bytes (0 = unlimited) */
    uint64_t maxCount;     /* Max entries (0 = unlimited) */
    multilruPolicy policy; /* Eviction trigger policy */
    multilruEvictStrategy evictStrategy; /* Victim selection strategy */
    bool enableWeights; /* Allocate weight array for size tracking */
} multilruConfig;

/* ====================================================================
 * Creation and Destruction
 * ==================================================================== */

/* Create with default settings (7 levels, count-only policy, no limits) */
multilru *multilruNew(void);

/* Create with specified number of levels (1-64) */
multilru *multilruNewWithLevels(size_t maxLevels);

/* Create with levels and pre-allocated capacity */
multilru *multilruNewWithLevelsCapacity(size_t maxLevels, size_t startCapacity);

/* Create with full configuration control */
multilru *multilruNewWithConfig(const multilruConfig *config);

/* Free all resources. Safe to call with NULL. */
void multilruFree(multilru *mlru);

/* ====================================================================
 * Core Operations
 * ==================================================================== */

/* Insert new entry at level 0.
 * Returns: Entry handle (non-zero), or 0 on allocation failure.
 * Note: If policy limits are set, may trigger automatic eviction. */
multilruPtr multilruInsert(multilru *mlru);

/* Insert with associated weight for size-based eviction.
 * Requires: enableWeights=true or MLRU_POLICY_SIZE/HYBRID. */
multilruPtr multilruInsertWeighted(multilru *mlru, uint64_t weight);

/* Promote entry to next level (call on cache hit).
 * Entry moves from level N to level N+1 (capped at maxLevels-1).
 * Safe to call with invalid pointer (no-op). */
void multilruIncrease(multilru *mlru, multilruPtr ptr);

/* Update weight of existing entry.
 * Requires weight tracking enabled. Safe to call with invalid pointer. */
void multilruUpdateWeight(multilru *mlru, multilruPtr ptr, uint64_t newWeight);

/* Remove and return the LRU entry with S4LRU demotion.
 * If entry is at level > 0: demotes to level-1 (second chance)
 * If entry is at level 0: true eviction, handle becomes invalid
 * Returns: true if entry was found and processed, false if cache empty.
 * The 'out' pointer receives the entry handle (useful for cleanup). */
bool multilruRemoveMinimum(multilru *mlru, multilruPtr *out);

/* Delete specific entry immediately (bypasses demotion).
 * Safe to call with invalid pointer (no-op). */
void multilruDelete(multilru *mlru, multilruPtr ptr);

/* ====================================================================
 * Bulk Eviction Operations
 * ==================================================================== */

/* Evict up to n entries, storing handles in out[].
 * Each call applies full S4LRU demotion semantics.
 * Returns: Number of entries actually evicted from level 0. */
size_t multilruEvictN(multilru *mlru, multilruPtr out[], size_t n);

/* Evict entries until totalWeight <= targetWeight.
 * Returns: Number of entries evicted. */
size_t multilruEvictToSize(multilru *mlru, uint64_t targetWeight,
                           multilruPtr out[], size_t maxN);

/* ====================================================================
 * Query Operations
 * ==================================================================== */

/* Total active entry count */
size_t multilruCount(const multilru *mlru);

/* Total memory used by LRU structure (bytes) */
size_t multilruBytes(const multilru *mlru);

/* Total weight of all entries (requires weight tracking) */
uint64_t multilruTotalWeight(const multilru *mlru);

/* Entry count at specific level (0 to maxLevels-1) */
size_t multilruLevelCount(const multilru *mlru, size_t level);

/* Total weight at specific level */
uint64_t multilruLevelWeight(const multilru *mlru, size_t level);

/* Get weight of specific entry (0 if invalid or no weight tracking) */
uint64_t multilruGetWeight(const multilru *mlru, multilruPtr ptr);

/* Get current level of entry (0 if invalid) */
size_t multilruGetLevel(const multilru *mlru, multilruPtr ptr);

/* Check if entry handle is valid and populated */
bool multilruIsPopulated(const multilru *mlru, multilruPtr ptr);

/* ====================================================================
 * Statistics / Metrics
 * ====================================================================
 * For production monitoring and observability. All counters are
 * lifetime totals since cache creation. The struct snapshot is O(1).
 *
 * SLOT ALLOCATION METRICS
 * -----------------------
 * The cache uses a hybrid allocation strategy:
 *   1. Recycled slots (free list) - IDs from deleted/evicted entries
 *   2. Fresh slots (nextFresh) - Never-used sequential IDs
 *
 * On insert: prefer recycled slots, then allocate fresh.
 * On delete/evict: slot goes to free list for recycling.
 *
 * Key metrics for monitoring:
 *   - nextFresh: High water mark of slot allocation
 *   - freeCount: Recycled slots available (holes in the array)
 *   - Fragmentation ratio: freeCount / (nextFresh - maxLevels - 1)
 *
 * Example interpretation:
 *   nextFresh=1000, freeCount=0   -> Dense: 1000 slots, all in use
 *   nextFresh=1000, freeCount=500 -> Fragmented: 500 active, 500 holes
 *   nextFresh=1000, freeCount=900 -> Very sparse: only 100 active entries
 *
 * High fragmentation is normal after many deletes. Memory is still
 * efficiently reused (holes get recycled). Future compaction feature
 * will allow shrinking the underlying array if needed.
 */

typedef struct multilruStats {
    /* Current state */
    uint64_t totalWeight; /* Sum of all entry weights */
    size_t count;         /* Active entries */
    size_t capacity;      /* Allocated capacity */
    size_t bytesUsed;     /* Total memory footprint */

    /* Slot allocation state */
    uint64_t nextFresh; /* Next never-used slot index */
    uint64_t freeCount; /* Recycled slots available in free list */

    /* Lifetime operation counters */
    uint64_t inserts;    /* Total insert operations */
    uint64_t evictions;  /* True evictions from level 0 */
    uint64_t demotions;  /* Demotions (level N -> N-1) */
    uint64_t promotions; /* Promotions via multilruIncrease() */
    uint64_t deletes;    /* Direct delete operations */

    /* Configuration snapshot */
    size_t maxLevels;   /* Number of configured levels */
    uint64_t maxWeight; /* Current weight limit (0=unlimited) */
    uint64_t maxCount;  /* Current count limit (0=unlimited) */
    uint8_t entryWidth; /* Current entry width (5-12 or 16) */
    bool autoEvict;     /* Auto-eviction enabled */
} multilruStats;

/* Fill stats struct with current metrics snapshot (O(1) operation) */
void multilruGetStats(const multilru *mlru, multilruStats *stats);

/* ====================================================================
 * Bulk Query Operations
 * ==================================================================== */

/* Get up to n entries from coldest (level 0, oldest first) */
void multilruGetNLowest(multilru *mlru, multilruPtr out[], size_t n);

/* Get up to n entries from hottest (highest level, newest first) */
void multilruGetNHighest(multilru *mlru, multilruPtr out[], size_t n);

/* ====================================================================
 * Runtime Configuration (can be changed after creation)
 * ====================================================================
 *
 * DYNAMIC CACHE RESIZING
 * ----------------------
 * Limits can be changed at runtime. Behavior depends on direction:
 *
 * EXPANDING (increasing limits):
 *   Simply increase the limit - takes effect immediately.
 *   multilruSetMaxCount(cache, 20000);  // Was 10000, now 20000
 *
 * SHRINKING (decreasing limits):
 *   Decreasing limits does NOT trigger immediate eviction (to avoid
 *   blocking). Instead, use gradual eviction to avoid system stalls:
 *
 *   // Shrink limit
 *   multilruSetMaxCount(cache, 5000);  // Was 10000, now 5000
 *
 *   // Gradually evict in batches (non-blocking pattern)
 *   while (multilruNeedsEviction(cache)) {
 *       multilruPtr evicted[100];
 *       size_t n = multilruEvictN(cache, evicted, 100);
 *       for (size_t i = 0; i < n; i++) {
 *           cleanupExternalData(evicted[i]);
 *       }
 *       // Yield to event loop / other work between batches
 *       maybeYield();
 *   }
 *
 *   // Progress tracking (optional)
 *   size_t current = multilruCount(cache);
 *   uint64_t limit = multilruGetMaxCount(cache);
 *   size_t remaining = current > limit ? current - limit : 0;
 */

/* Change eviction trigger policy */
void multilruSetPolicy(multilru *mlru, multilruPolicy policy);

/* Change victim selection strategy */
void multilruSetEvictStrategy(multilru *mlru, multilruEvictStrategy strategy);

/* Set/get max entry count (0 = unlimited) */
void multilruSetMaxCount(multilru *mlru, uint64_t maxCount);
uint64_t multilruGetMaxCount(const multilru *mlru);

/* Set/get max total weight (0 = unlimited) */
void multilruSetMaxWeight(multilru *mlru, uint64_t maxWeight);
uint64_t multilruGetMaxWeight(const multilru *mlru);

/* ====================================================================
 * Eviction Control
 * ====================================================================
 *
 * Two production workflows for managing cache limits:
 *
 * WORKFLOW 1: Automatic Eviction with Callback (default)
 * -------------------------------------------------------
 * The cache automatically evicts entries when limits are exceeded.
 * Register a callback to be notified of evictions for cleanup.
 *
 *   void onEvict(size_t ptr, void *data) {
 *       MyHashMap *map = data;
 *       myHashMapDeleteByLruPtr(map, ptr);  // Clean up external data
 *   }
 *
 *   multilru *cache = multilruNewWithConfig(&config);
 *   multilruSetEvictCallback(cache, onEvict, myHashMap);
 *   // Inserts now auto-evict and notify via callback
 *
 * WORKFLOW 2: Manual Eviction (polling-based)
 * -------------------------------------------
 * Disable auto-eviction and manage eviction externally. Useful when
 * you need full control over eviction timing (e.g., batch processing).
 *
 *   multilru *cache = multilruNewWithConfig(&config);
 *   multilruSetAutoEvict(cache, false);  // Disable auto-eviction
 *
 *   // Insert freely, cache grows without limit enforcement
 *   for (...) {
 *       multilruInsert(cache);
 *   }
 *
 *   // Periodically check and evict manually
 *   while (multilruNeedsEviction(cache)) {
 *       multilruPtr evicted;
 *       if (multilruRemoveMinimum(cache, &evicted)) {
 *           // Clean up external data for evicted entry
 *           myHashMapDeleteByLruPtr(map, evicted);
 *       }
 *   }
 */

/* Enable/disable automatic eviction on insert (default: true).
 * When disabled, cache grows past limits until manual eviction. */
void multilruSetAutoEvict(multilru *mlru, bool autoEvict);

/* Get current auto-eviction setting */
bool multilruGetAutoEvict(const multilru *mlru);

/* Register callback for eviction notification.
 * Called with entry pointer BEFORE the entry is freed.
 * Set callback to NULL to disable notifications. */
void multilruSetEvictCallback(multilru *mlru,
                              void (*callback)(size_t evictedPtr,
                                               void *userData),
                              void *userData);

/* Check if cache exceeds configured limits (count or weight).
 * Use with manual eviction workflow to determine when to evict. */
bool multilruNeedsEviction(const multilru *mlru);

/* ====================================================================
 * Introspection
 * ==================================================================== */

/* Get number of configured levels */
size_t multilruMaxLevels(const multilru *mlru);

/* Get current entry width (5, 6, 7, 8, 9, 10, 11, 12, or 16 bytes) */
multilruEntryWidth multilruGetEntryWidth(const multilru *mlru);

/* Get current allocated capacity (may be > count) */
size_t multilruCapacity(const multilru *mlru);

/* Check if weight tracking is enabled (weights array allocated) */
bool multilruHasWeights(const multilru *mlru);

/* ====================================================================
 * Testing (only available with DATAKIT_TEST defined)
 * ==================================================================== */

#ifdef DATAKIT_TEST
/* Print human-readable representation of LRU state */
void multilruRepr(const multilru *mlru);

/* Traverse and count entries (for validation) */
ssize_t multilruTraverseSize(const multilru *mlru);

/* Run test suite */
int multilruTest(int argc, char *argv[]);
#endif
