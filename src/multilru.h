#pragma once

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
 * Fixed overhead: ~200 bytes + 32 bytes per level
 *
 * PERFORMANCE
 * -----------
 * All core operations are O(1).  perfRegression's multilru rows measure
 * them (100K entries, clang 22 perf build, development machine): insert
 * ~10 ns, promote
 * ~14 ns, delete ~9 ns, an eviction step ~25 ns (~29 ns under
 * MLRU_EVICT_SIZE_WEIGHTED, which weighs 8 candidates).
 *
 * The levelMask bitmap finds the lowest populated level in O(1)
 * regardless of how many levels are configured.
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
 * - multilruInsert() returns 0 when the cache cannot grow or its own
 *   automatic eviction took the new entry
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
 *   - MLRU_POLICY_SIZE: Total weight/size limit
 *   - MLRU_POLICY_HYBRID: Both limits enforced
 *
 * Evict Strategy:
 *   - MLRU_EVICT_LRU: Pure LRU (coldest first)
 *   - MLRU_EVICT_SIZE_WEIGHTED: The heaviest of the 8 coldest
 *   - MLRU_EVICT_SIZE_LRU: The coldest of those that covers the excess
 *     weight (multilruEvictStrategy)
 *
 * The size policies and strategies read entry weights: choosing one, at
 * creation or later through the setters, turns weight tracking on (8
 * bytes per slot; entries held before then weigh 0).
 *
 * Recycled slots stay allocated: after many deletes the entry array keeps
 * its high-water size (multilruStats nextFresh / freeCount show it) and
 * new inserts reuse the holes; multilruRepack gives the memory back.
 */

/* @handle: fixed: its address holds from creation to free */
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

/* Victim selection strategy - determines WHAT the next eviction step
 * takes (multilruPeekMinimum names it; a level > 0 victim is demoted, a
 * level 0 one evicted).  The size-aware strategies choose among the 8
 * oldest entries of the lowest populated level, so a step stays O(1); they
 * need weights (a size policy or enableWeights turns them on) and act as
 * MLRU_EVICT_LRU without them. */
typedef enum multilruEvictStrategy {
    MLRU_EVICT_LRU = 0,       /* The oldest entry of the lowest level */
    MLRU_EVICT_SIZE_WEIGHTED, /* The heaviest of those 8 (oldest of equals):
                                 frees the most weight per step */
    MLRU_EVICT_SIZE_LRU,      /* Over maxWeight: the oldest of those 8 whose
                                 weight alone covers the excess, else the
                                 heaviest; otherwise the oldest entry */
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

/* The most levels a multilru keeps (one bit each in its 64-bit level
 * mask); a config asking for more gets this many */
static constexpr size_t MULTILRU_LEVELS_MAX = 64;

/* Full configuration for creating a new multilru */
typedef struct multilruConfig {
    size_t maxLevels; /* LRU levels (default: 7, max: MULTILRU_LEVELS_MAX) */
    size_t startCapacity;  /* Initial entry capacity (0 = auto) */
    uint64_t maxWeight;    /* Max total weight in bytes (0 = unlimited) */
    uint64_t maxCount;     /* Max entries (0 = unlimited) */
    multilruPolicy policy; /* Eviction trigger policy */
    multilruEvictStrategy evictStrategy; /* Victim selection strategy */
    bool enableWeights; /* Allocate weight array for size tracking */
    /* Levels without a share (S4LRU segments off): an entry stays at the
     * level its uses reached until every level below empties, so levels
     * count lifetime uses instead of recent ones (default false) */
    bool unboundedLevels;
} multilruConfig;

/* ====================================================================
 * Creation and Destruction
 * ==================================================================== */

/* Create with default settings (7 levels, count-only policy, no limits) */
[[nodiscard]] multilru *multilruNew(void);

/* Create with specified number of levels (1 .. MULTILRU_LEVELS_MAX) */
[[nodiscard]] multilru *multilruNewWithLevels(size_t maxLevels);

/* Create with levels and pre-allocated capacity */
[[nodiscard]] multilru *multilruNewWithLevelsCapacity(size_t maxLevels,
                                                      size_t startCapacity);

/* Create with full configuration control (NULL: the defaults, as
 * multilruNew) */
[[nodiscard]] multilru *multilruNewWithConfig(const multilruConfig *config);

/* Free all resources. Safe to call with NULL. */
void multilruFree(multilru *mlru);

/* ====================================================================
 * Core Operations
 * ==================================================================== */

/* Insert new entry at level 0.
 * Returns: Entry handle (non-zero), or 0 when the cache cannot grow or
 * the automatic eviction an over-limit insert triggers took the new
 * entry itself. */
multilruPtr multilruInsert(multilru *mlru);

/* Insert with associated weight for size-based eviction (the weight is
 * dropped without weight tracking: multilruHasWeights). */
multilruPtr multilruInsertWeighted(multilru *mlru, uint64_t weight);

/* Promote entry to next level (call on cache hit).
 * Entry moves from level N to level N+1 (capped at maxLevels-1).
 * Safe to call with invalid pointer (no-op). */
void multilruIncrease(multilru *mlru, multilruPtr ptr);

/* Update weight of existing entry.
 * Requires weight tracking enabled. Safe to call with invalid pointer. */
void multilruUpdateWeight(multilru *mlru, multilruPtr ptr, uint64_t newWeight);

/* Take one eviction step on the entry the strategy picks (the LRU entry
 * under MLRU_EVICT_LRU), with S4LRU demotion.
 * If entry is at level > 0: demotes to level-1 (second chance)
 * If entry is at level 0: true eviction, handle becomes invalid
 * Returns: true if entry was found and processed, false if cache empty.
 * The 'out' pointer receives the entry handle (useful for cleanup).
 * Distinguish demotion from eviction with multilruIsPopulated(out):
 * still populated = demoted (do NOT free the caller's resource);
 * unpopulated = truly evicted. */
bool multilruRemoveMinimum(multilru *mlru, multilruPtr *out);

/* Delete specific entry immediately (bypasses demotion).
 * Safe to call with invalid pointer (no-op). */
void multilruDelete(multilru *mlru, multilruPtr ptr);

/* Demote entry from level N to the head of level N-1, exactly as
 * multilruRemoveMinimum demotes its victim.  No-op at level 0 and for an
 * invalid pointer.  For replaying a logged demotion. */
void multilruDecrease(multilru *mlru, multilruPtr ptr);

/* Place an entry as the newest of level 'level' (below maxLevels), moving
 * nothing else: no level share is applied and no promotion or demotion is
 * counted.  For restoring a recorded order: placing entries oldest first
 * rebuilds each level's order.  False, nothing moved, for an invalid
 * pointer or level. */
bool multilruPlace(multilru *mlru, multilruPtr ptr, size_t level);

/* The entry multilruRemoveMinimum would take next under the current
 * strategy and limits (0 when empty): it demotes it when multilruGetLevel
 * > 0 and evicts it at level 0.  Lets a writer log an eviction step before
 * taking it. */
multilruPtr multilruPeekMinimum(const multilru *mlru);

/* The handle the next insert will return: the most recently freed slot,
 * else the next never-used one.  Lets a writer log an insert before
 * making it. */
multilruPtr multilruPeekNextHandle(const multilru *mlru);

/* The recycled slots in the order inserts will reuse them (up to 'n'
 * written to 'out'); multilruGetStats reports how many there are.  A
 * snapshot that keeps this order restores to a multilru that hands out
 * the same handles. */
size_t multilruFreeHandles(const multilru *mlru, multilruPtr out[], size_t n);

/* ====================================================================
 * Repacking
 * ====================================================================
 * Deletes and evictions leave holes the next inserts reuse; the arrays
 * keep their high-water size.  Repacking moves every live entry into the
 * lowest slots, then shrinks the arrays to fit them, at the narrowest
 * entry width their handles allow. */

/* Told once per entry a repack moves: its handle was 'from' and is 'to'
 * from now on.  It must not call into the multilru being repacked. */
typedef void multilruMovedFn(multilruPtr from, multilruPtr to, void *ctx);

/* Repack: recency order, levels, weights, limits and counters are kept;
 * handles of moved entries change (reported through 'moved', which may be
 * NULL when the caller keeps no handles); no recycled slots remain, so the
 * next insert takes the slot after the last entry.  O(slots used).
 * Returns how many entries moved. */
size_t multilruRepack(multilru *mlru, multilruMovedFn *moved, void *ctx);

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
 * High fragmentation is normal after many deletes: inserts reuse the
 * holes before taking fresh slots, and multilruRepack removes them.
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

/* Write up to n entries from coldest (level 0, oldest first) into out[] and
 * return how many were written. Slots past that count are not touched. */
size_t multilruCollectLowest(const multilru *mlru, multilruPtr out[], size_t n);

/* Write up to n entries from hottest (highest level, newest first) into
 * out[] and return how many were written. Slots past that count are not
 * touched. */
size_t multilruCollectHighest(const multilru *mlru, multilruPtr out[],
                              size_t n);

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

/* Change eviction trigger policy (a size policy turns weight tracking on;
 * entries held until then weigh 0) */
void multilruSetPolicy(multilru *mlru, multilruPolicy policy);

/* Change victim selection strategy (a size-aware one turns weight
 * tracking on, as multilruSetPolicy does) */
void multilruSetEvictStrategy(multilru *mlru, multilruEvictStrategy strategy);
multilruPolicy multilruGetPolicy(const multilru *mlru);
multilruEvictStrategy multilruGetEvictStrategy(const multilru *mlru);

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
 *   void onEvict(multilruPtr ptr, void *data) {
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

/* Levels with or without their S4LRU share (multilruConfig.unboundedLevels).
 * Turning shares on moves nothing at once: a level over its share gives up
 * its oldest entries at its next promotion or demotion. */
void multilruSetUnboundedLevels(multilru *mlru, bool unbounded);
bool multilruGetUnboundedLevels(const multilru *mlru);

/* Told of each true eviction (level 0), BEFORE the entry's slot is freed */
typedef void multilruEvictFn(multilruPtr evicted, void *userData);

/* Register callback for eviction notification (NULL disables it) */
void multilruSetEvictCallback(multilru *mlru, multilruEvictFn *callback,
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
