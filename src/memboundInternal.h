#pragma once

/* memboundInternal.h - Internal structures for membound extent system
 *
 * This header defines the internal data structures used by membound.
 * Not part of the public API - only for membound.c implementation.
 *
 * Copyright (c) 2024-2025, Matt Stancliff <matt@genges.com>
 * All rights reserved.
 *
 * ============================================================================
 * IMPLEMENTATION ARCHITECTURE OVERVIEW
 * ============================================================================
 *
 * This file contains the internal implementation of membound's tiered
 * memory allocator. Key design decisions documented here:
 *
 * BUDDY ALLOCATOR IMPLEMENTATION:
 *
 *   Each extent uses the Robson buddy system with these components:
 *
 *   zPool - The actual memory buffer for user allocations
 *   aCtrl - Control byte array, one byte per minimum allocation unit
 *           Bits 0-4: Log2 of block size (0-30)
 *           Bit 5: FREE flag (1 = block is available)
 *
 *   aiFreelist - Array of head indices for each size class
 *                aiFreelist[n] = -1 means no free blocks of size 2^n
 *                aiFreelist[n] >= 0 is index into zPool of first free block
 *
 *   freelistBitmap - Bitmap where bit n is set if aiFreelist[n] != -1
 *                    Allows O(1) "find any free block >= size" via CTZ
 *
 *   Allocation algorithm:
 *   1. Calculate log2(requested_size) to get size class
 *   2. Use CTZ on bitmap to find smallest available class >= requested
 *   3. If larger block found, split recursively until right size
 *   4. Mark block as allocated, update free lists
 *
 *   Free algorithm:
 *   1. Look up block size from aCtrl
 *   2. Check if buddy block is free and same size
 *   3. If yes, coalesce into parent and repeat
 *   4. Add final block to free list
 *
 * EXTENT LOOKUP OPTIMIZATION:
 *
 *   When freeing, we need to find which extent owns a pointer.
 *   With many extents, linear search is too slow. We use:
 *
 *   - Inline array for <=4 extents (no heap allocation)
 *   - Sorted array for 5-7 extents (binary search, O(log n))
 *   - SIMD parallel comparison for >=8 extents (O(1) with vector width)
 *
 *   The lookup table stores (start, end, extent*) tuples sorted by start.
 *   Binary search finds the candidate, then we verify end bound.
 *
 *   SIMD implementation uses 256-bit or 512-bit vectors to compare
 *   the pointer against all extent bounds simultaneously. This provides
 *   essentially O(1) lookup for up to 32 extents with AVX-512.
 *
 * TIER DETECTION:
 *
 *   All tier structs (membound, memboundMicro, memboundCompactPool) have
 *   the tier value as their FIRST byte. This allows O(1) tier detection:
 *
 *     memboundTier tier = *(uint8_t*)ptr;
 *
 *   On little-endian systems (all modern x86/ARM), the enum's low byte
 *   is at the struct's first byte. The tier values are arranged so:
 *
 *     STANDARD   = 0  (safe default for uninitialized memory)
 *     MICRO      = 1
 *     COMPACT    = 2
 *     ENTERPRISE = 3
 *
 * MEMORY LAYOUT CONSIDERATIONS:
 *
 *   Extent struct is designed to fit in 8 cache lines (512 bytes):
 *   - Hot fields (zPool, aCtrl, freelistBitmap) at the start
 *   - Statistics grouped together for cache efficiency
 *   - Per-extent mutex at the end (only used by ENTERPRISE)
 *
 *   MICRO struct fits in 1 cache line (64 bytes):
 *   - Uses 16-bit indices (max 64KB pool)
 *   - 8 size classes instead of 31
 *   - No mutex, no callbacks, no lifetime stats
 *
 *   COMPACT struct fits in 4 cache lines (256 bytes):
 *   - Uses 32-bit indices (supports multi-GB pools)
 *   - 16 size classes
 *   - Optional mutex
 *
 */

#include "membound.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* ====================================================================
 * SIMD Platform Detection
 * ====================================================================
 * SIMD is used for two operations:
 *
 * 1. Extent lookup (memboundIndexLookupSIMD):
 *    When finding which extent owns a pointer, we can compare against
 *    multiple extent ranges simultaneously using vector instructions.
 *    - AVX-512: 8 extents per comparison (512-bit / 64-bit pointers)
 *    - AVX2:    4 extents per comparison
 *    - NEON:    2 extents per comparison
 *
 * 2. Memory zeroing (memboundZeroFast):
 *    For calloc and extent initialization, SIMD provides ~4x speedup
 *    over memset for buffers > 256 bytes.
 *
 * Platform detection is compile-time for zero runtime overhead.
 */

#if defined(__AVX512F__)
#define MEMBOUND_USE_AVX512 1
#include <immintrin.h>
#elif defined(__AVX2__)
#define MEMBOUND_USE_AVX2 1
#include <immintrin.h>
#elif defined(__SSE2__)
#define MEMBOUND_USE_SSE2 1
#include <immintrin.h>
#endif

#if defined(__aarch64__) || defined(__ARM_NEON) || defined(__ARM_NEON__)
#define MEMBOUND_USE_NEON 1
#include <arm_neon.h>
#endif

/* Branch prediction hints
 *
 * Used to guide CPU branch predictor for uncommon paths:
 * - LIKELY: Expected to be true (allocation succeeds, lock uncontended)
 * - UNLIKELY: Expected to be false (error paths, growth needed)
 *
 * PREFETCH hints tell the CPU to start loading data before we need it.
 * Used when walking extent lists to prefetch next extent while processing
 * current one. */
#if defined(__GNUC__) || defined(__clang__)
#define MEMBOUND_LIKELY(x) __builtin_expect(!!(x), 1)
#define MEMBOUND_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define MEMBOUND_PREFETCH(addr) __builtin_prefetch(addr, 0, 0)
#else
#define MEMBOUND_LIKELY(x) (x)
#define MEMBOUND_UNLIKELY(x) (x)
#define MEMBOUND_PREFETCH(addr) ((void)0)
#endif

/* ====================================================================
 * Constants
 * ====================================================================
 * These constants control the fundamental behavior of the allocator.
 * They are chosen based on performance analysis and memory trade-offs.
 */

/* Maximum size of any individual allocation request is ((1<<LOGMAX)*szAtom).
 * Since szAtom is always at least 8 and 32-bit integers are used,
 * it is not actually possible to reach this limit.
 * Value 30 allows allocations up to 1 billion atoms * atom_size. */
#define MEMBOUND_LOGMAX 30

/* Minimum allocation atom size (power of 2).
 *
 * Why 256 bytes minimum?
 * 1. Each free block needs a MemboundLink (16 bytes for next/prev indices)
 * 2. Smaller atoms mean larger aCtrl array (one byte per atom)
 * 3. 256B provides good balance: 4KB pool = 16 atoms = 16 byte aCtrl
 * 4. Most applications don't benefit from finer granularity
 *
 * For tiny allocations, consider using a slab allocator instead. */
#define MEMBOUND_MIN_ATOM 256

/* MICRO tier constants:
 * - Reduced freelist for ultra-compact metadata
 * - Max pool size limited to 64KB to use 16-bit indices
 * - 8 size classes instead of 31 (covers 256B to 32KB in powers of 2)
 *
 * Why 64KB limit?
 * - 16-bit indices save 12 bytes per freelist entry vs 64-bit
 * - 64KB is sufficient for coroutine/fiber scratch space
 * - Larger pools should use COMPACT or STANDARD tier */
#define MEMBOUND_MICRO_LOGMAX 8       /* 8 size classes */
#define MEMBOUND_MICRO_MAX_POOL 65536 /* 64KB max pool size */

/* COMPACT tier constants:
 * - Uses 16 size classes (covers 256B to 8MB)
 * - No extent management, just embedded pool
 * - 32-bit indices (4B vs 8B per entry) */
#define MEMBOUND_COMPACT_LOGMAX 16

/* Extent lookup optimization thresholds
 *
 * INLINE_MAX: Below this, we use a simple inline array in the struct.
 *             No heap allocation, cache-friendly for common cases.
 *
 * SIMD_THRESHOLD: Above this, SIMD parallel lookup becomes worthwhile.
 *                 Below this, binary search on sorted array is faster
 *                 due to lower setup overhead. */
#define MEMBOUND_EXTENT_INLINE_MAX 4     /* Inline array for <=4 extents */
#define MEMBOUND_EXTENT_SIMD_THRESHOLD 8 /* Use SIMD for >=8 extents */

/* Growth strategy defaults
 *
 * DEFAULT_GROWTH_SIZE: When growing, add 1MB by default.
 *                      Large enough for batch efficiency, small enough
 *                      to not over-allocate.
 *
 * DEFAULT_MAX_EXTENTS: Limit to 256 extents. At 1MB each, that's 256MB
 *                      maximum with default growth. Override for larger.
 *
 * DEFAULT_GROWTH_FACTOR: For geometric growth, multiply by 1.5x.
 *                        Balances rapid growth against over-allocation. */
#define MEMBOUND_DEFAULT_GROWTH_SIZE (1ULL << 20) /* 1 MB */
#define MEMBOUND_DEFAULT_MAX_EXTENTS 256
#define MEMBOUND_DEFAULT_GROWTH_FACTOR 1.5

/* ====================================================================
 * Extent Structure
 * ====================================================================
 * Each extent is a complete buddy allocator region with its own:
 *   - Memory pool (zPool)
 *   - Control array (aCtrl)
 *   - Free lists per size class
 *   - Statistics
 *
 * Extents are linked in a list and also indexed in a sorted array
 * for O(log n) ownership lookup.
 */

typedef struct memboundExtent {
    /* Memory region */
    uint8_t *zPool; /* Memory available to be allocated */
    uint8_t *aCtrl; /* Block control/status array */
    size_t size;    /* Byte extent of zPool allocation */

    /* Pool configuration */
    int64_t szAtom;     /* Smallest allocation unit (power of 2) */
    uint32_t atomShift; /* log2(szAtom) for fast division */
    int64_t nBlock;     /* Number of szAtom blocks in pool */

    /* Free lists for this extent */
    int64_t aiFreelist[MEMBOUND_LOGMAX + 1];
    uint64_t freelistBitmap; /* O(1) free block lookup via CTZ */

    /* Per-extent statistics */
    uint64_t currentOut;   /* Bytes currently allocated */
    uint64_t currentCount; /* Outstanding allocation count */
    uint64_t nAlloc;       /* Total allocations from this extent */
    uint64_t totalAlloc;   /* Total bytes allocated (with frag) */
    uint64_t totalExcess;  /* Internal fragmentation total */

    /* Extent age and strategy metadata */
    uint64_t creationSeq;  /* Sequence number at creation (for aging) */
    uint64_t lastAllocSeq; /* Sequence number of last allocation */
    int8_t sizeClass;      /* Assigned size class (-1 = any) */
    uint8_t flags;         /* Extent flags (see MEMBOUND_EXTENT_FLAG_*) */

    /* Extent list linkage */
    struct memboundExtent *next;
    struct memboundExtent *prev;

    /* Extent index in sorted lookup array */
    size_t sortedIndex;

    /* Per-extent mutex (ENTERPRISE tier only)
     * When enabled, allows parallel alloc/free across different extents.
     * Note: This increases extent size to ~8 cache lines (512 bytes). */
    pthread_mutex_t extentMutex;
    bool extentMutexInitialized;
} memboundExtent;

/* Extent flags */
#define MEMBOUND_EXTENT_FLAG_PRIMARY 0x01   /* Is primary extent */
#define MEMBOUND_EXTENT_FLAG_DEDICATED 0x02 /* Dedicated to size class */
#define MEMBOUND_EXTENT_FLAG_DRAINING 0x04  /* Being drained for shrink */

/* Verify extent fits nicely in cache lines (8 cache lines with per-extent
 * mutex) */
_Static_assert(sizeof(memboundExtent) <= 512,
               "memboundExtent should fit in 8 cache lines");

/* ====================================================================
 * Extent Lookup Index
 * ====================================================================
 * For efficient pointer-to-extent lookup (O(log n) or O(1) with SIMD),
 * we maintain a sorted array of extent base addresses. Each entry stores
 * the start and end addresses for binary search.
 */

typedef struct memboundExtentRange {
    uintptr_t start;        /* Pool start address */
    uintptr_t end;          /* Pool end address (exclusive) */
    memboundExtent *extent; /* Pointer to extent */
} memboundExtentRange;

typedef struct memboundExtentIndex {
    /* For small extent counts, use inline array */
    memboundExtentRange inlineRanges[MEMBOUND_EXTENT_INLINE_MAX];

    /* For larger counts, heap-allocated sorted array */
    memboundExtentRange *ranges;
    size_t count;
    size_t capacity;

    /* Statistics for lookup optimization */
    uint64_t lookupCount;
    uint64_t primaryHits; /* Times primary extent was the answer */
} memboundExtentIndex;

/* ====================================================================
 * Growth Strategy Configuration
 * ==================================================================== */

typedef enum memboundGrowthType {
    MEMBOUND_GROWTH_FIXED,     /* Always grow by fixed size */
    MEMBOUND_GROWTH_GEOMETRIC, /* Grow by factor of current capacity */
    MEMBOUND_GROWTH_ADAPTIVE,  /* Adjust based on allocation patterns */
} memboundGrowthType;

typedef struct memboundGrowthConfig {
    memboundGrowthType type;
    size_t fixedGrowthSize; /* For FIXED: bytes to add per extent */
    double growthFactor;    /* For GEOMETRIC: multiplier (e.g., 1.5) */
    size_t minExtentSize;   /* Minimum extent size */
    size_t maxExtentSize;   /* Maximum single extent size */
    size_t maxTotalSize;    /* Hard limit on total capacity (0=unlimited) */
    size_t maxExtentCount;  /* Maximum number of extents (0=unlimited) */
} memboundGrowthConfig;

/* ====================================================================
 * Extent Selection Strategy (Internal)
 * ====================================================================
 * memboundExtentStrategy enum is defined in membound.h (public API).
 * This section contains internal implementation details.
 */

/* Size class thresholds for SIZE_CLASS strategy */
#define MEMBOUND_SIZE_CLASS_SMALL 1024   /* <= 1KB = small */
#define MEMBOUND_SIZE_CLASS_MEDIUM 65536 /* <= 64KB = medium, else large */

/* Size class indices */
typedef enum memboundSizeClass {
    MEMBOUND_CLASS_SMALL = 0,  /* <= 1KB */
    MEMBOUND_CLASS_MEDIUM = 1, /* 1KB - 64KB */
    MEMBOUND_CLASS_LARGE = 2,  /* > 64KB */
    MEMBOUND_CLASS_COUNT = 3
} memboundSizeClass;

/* Extent selection configuration */
typedef struct memboundStrategyConfig {
    memboundExtentStrategy strategy;

    /* Occupancy thresholds (0.0 - 1.0) */
    float minOccupancyForAlloc; /* Don't allocate from extents below this (0 =
                                   disabled) */
    float shrinkThreshold;      /* Auto-shrink extents below this occupancy */

    /* Size-class thresholds (for SIZE_CLASS strategy)
     * Uses defaults if set to 0 */
    size_t sizeClassSmall;  /* Max bytes for small class */
    size_t sizeClassMedium; /* Max bytes for medium class */

    /* Size-class dedicated extents (for SIZE_CLASS strategy) */
    memboundExtent *classExtents[MEMBOUND_CLASS_COUNT];

    /* Adaptive strategy state */
    uint64_t adaptiveSwitchCount; /* Times strategy was switched */
    float fragmentationThreshold; /* Switch to best-fit above this */
} memboundStrategyConfig;

/* ====================================================================
 * MICRO Tier Structure (~64 bytes)
 * ====================================================================
 * Ultra-compact for millions of pools. Uses 16-bit indices for 64KB max.
 * NO mutex, NO lifetime stats, NO callbacks.
 *
 * IMPORTANT: 'tier' MUST be the first field so tier detection works.
 */
typedef struct memboundMicro {
    /* Tier marker - MUST BE FIRST for reliable tier detection */
    uint8_t tier;      /* 1 byte: Must be MEMBOUND_TIER_MICRO */
    uint8_t atomShift; /* 1 byte: log2(szAtom) */
    uint16_t szAtom;   /* 2 bytes: Atom size (power of 2) */
    uint16_t nBlock;   /* 2 bytes: Number of blocks (max 256 for 64KB/256) */
    uint16_t poolSize; /* 2 bytes: Total pool size (max 64KB) */

    /* Pool memory (allocated separately, pointed to) */
    uint8_t *zPool; /* 8 bytes: Memory pool */
    uint8_t *aCtrl; /* 8 bytes: Control array */

    /* Basic stats only */
    uint16_t currentOut;   /* 2 bytes: Bytes currently allocated */
    uint16_t currentCount; /* 2 bytes: Outstanding allocations */

    /* Compact freelists (8 size classes using 16-bit indices) */
    int16_t aiFreelist[MEMBOUND_MICRO_LOGMAX + 1]; /* 18 bytes: -1 = empty */
    uint16_t freelistBitmap; /* 2 bytes: O(1) free block lookup */

    /* Padding to cache line */
    uint8_t reserved[8]; /* 8 bytes: Reserved for future use */
} memboundMicro;

/* Verify MICRO struct is compact (fits in 1 cache line) */
_Static_assert(sizeof(memboundMicro) <= 64,
               "memboundMicro should be <= 64 bytes (1 cache line)");

/* ====================================================================
 * COMPACT Tier Structure (~192 bytes)
 * ====================================================================
 * Lightweight single-extent pool with optional mutex.
 * Basic stats, no dynamic growth.
 *
 * Note: Named 'memboundCompactPool' to avoid collision with the
 * public memboundCompact() function (which compacts memory pools).
 *
 * IMPORTANT: 'tier' MUST be the first field so tier detection works.
 */
typedef struct memboundCompactPool {
    /* Tier marker - MUST BE FIRST for reliable tier detection */
    uint8_t tier;       /* Must be MEMBOUND_TIER_COMPACT */
    uint8_t atomShift;  /* log2(szAtom) */
    uint8_t threadSafe; /* Whether mutex is used */
    uint8_t pad;

    /* Pool configuration */
    int32_t szAtom; /* Atom size (power of 2) */
    int32_t nBlock; /* Number of blocks */

    /* Pool memory */
    uint8_t *zPool;  /* Memory pool */
    uint8_t *aCtrl;  /* Control array */
    size_t poolSize; /* Total pool size */

    /* Basic stats */
    uint32_t currentOut;   /* Bytes currently allocated */
    uint32_t currentCount; /* Outstanding allocations */

    /* Freelists (16 size classes) */
    int32_t aiFreelist[MEMBOUND_COMPACT_LOGMAX + 1]; /* 68 bytes */
    uint32_t freelistBitmap; /* O(1) free block lookup */

    /* Optional mutex (only used if threadSafe = true) */
    pthread_mutex_t mutex;
    bool mutexInitialized;

    /* Safety violation tracking */
    uint32_t safetyViolations;
} memboundCompactPool;

/* Verify COMPACT struct size is reasonable */
_Static_assert(sizeof(memboundCompactPool) <= 256,
               "memboundCompactPool should be <= 256 bytes");

/* ====================================================================
 * ENTERPRISE Tier Extensions
 * ====================================================================
 * Additional fields for per-extent locking and caching.
 */
typedef struct memboundEnterpriseExt {
    /* Extent selection cache */
    memboundExtent *lastAllocExtent; /* Last extent used for allocation */
    uint64_t lastAllocSeq;           /* Sequence at last cache use */

    /* Per-extent lock support (extents have their own mutex) */
    bool perExtentLocking; /* Enable per-extent locks */

    /* Extended profiling */
    uint64_t extentSwitchCount; /* Times allocation switched extents */
    uint64_t cacheHits;         /* Times cache was used */
    uint64_t cacheMisses;       /* Times cache was invalid */
} memboundEnterpriseExt;

/* ====================================================================
 * Main membound Structure (STANDARD/ENTERPRISE)
 * ==================================================================== */

struct membound {
    /* Tier and mode selection - MUST BE FIRST for tier detection */
    memboundTier tier;
    memboundMode mode;

    /* Extent management */
    memboundExtent *extents; /* Linked list of all extents */
    memboundExtent *primary; /* Hot path: last extent with free space */
    size_t extentCount;      /* Number of extents */

    /* Extent lookup index (for pointer ownership) */
    memboundExtentIndex index;

    /* Growth configuration (dynamic mode only) */
    memboundGrowthConfig growth;

    /* Extent selection strategy (dynamic mode only) */
    memboundStrategyConfig strategy;

    /* Aggregate statistics across all extents */
    uint64_t totalCapacity; /* Sum of all extent capacities */
    uint64_t currentOut;    /* Total bytes in use across extents */
    uint64_t currentCount;  /* Total allocations across extents */
    uint64_t maxOut;        /* Peak memory usage */
    uint64_t maxCount;      /* Peak allocation count */
    uint64_t nAlloc;        /* Total lifetime allocations */
    uint64_t totalAlloc;    /* Total lifetime bytes allocated */
    uint64_t totalExcess;   /* Total lifetime fragmentation */
#if MEMBOUND_DEBUG
    uint64_t maxRequest; /* Largest single allocation request */
#endif

    /* Thread safety
     *
     * All STANDARD and ENTERPRISE tier pools use a global mutex.
     * ENTERPRISE additionally has per-extent mutexes for parallelism. */
    pthread_mutex_t mutex;
    bool mutexInitialized;

    /* Lock contention profiling
     *
     * These atomics track locking performance without introducing
     * additional synchronization overhead. All use relaxed memory
     * ordering since precise ordering isn't needed for profiling.
     *
     * lockAcquisitions: Incremented every time we acquire the lock.
     *                   Includes both fast-path (trylock success) and
     *                   slow-path (had to spin or block).
     *
     * lockContentions:  Incremented only when we had to block on the
     *                   mutex (spinning didn't acquire it). High values
     *                   indicate the lock is heavily contended.
     *
     * lockWaitTimeNs:   Total nanoseconds spent blocked on mutex.
     *                   Does NOT include spin time (spinning is cheap).
     *                   High values mean threads are waiting a lot.
     *
     * Derived metric (calculated in memboundGetStats):
     *   contentionPercent = (lockContentions / lockAcquisitions) * 100
     *
     * Interpretation:
     *   < 1%:  Lock is essentially uncontended, performance is optimal
     *   1-5%:  Some contention, acceptable for most workloads
     *   > 5%:  Significant contention, consider ENTERPRISE tier
     *   > 10%: Severe contention, definitely use ENTERPRISE tier */
    _Atomic uint64_t lockAcquisitions; /* Total lock acquisition attempts */
    _Atomic uint64_t lockContentions;  /* Times lock was already held */
    _Atomic uint64_t lockWaitTimeNs;   /* Total nanoseconds spent waiting */

    /* Safety violation tracking
     *
     * Tracks runtime safety check failures. Non-zero indicates bugs:
     * - Double-free attempts
     * - Freeing pointers not from this pool
     * - Freeing misaligned pointers
     * - Out-of-bounds pointer operations
     *
     * Violations are silently ignored (no crash) but counted here.
     * Check this value periodically in debug builds. */
    _Atomic uint64_t safetyViolations; /* Runtime safety check failures */

    /* Memory pressure callback (optional) */
    memboundPressureCallback pressureCallback;
    void *pressureUserData;
    memboundPressure lastPressureLevel;

    /* Enterprise tier extensions (NULL for STANDARD tier) */
    memboundEnterpriseExt *enterprise;
};

/* ====================================================================
 * Extent Internal Functions
 * ==================================================================== */

/* Extent lifecycle */
memboundExtent *memboundExtentCreate(size_t size);
void memboundExtentDestroy(memboundExtent *extent);
bool memboundExtentInit(memboundExtent *extent, void *space, size_t len);
void memboundExtentReset(memboundExtent *extent);

/* Extent allocation */
void *memboundExtentAlloc(memboundExtent *extent, size_t nBytes);
void memboundExtentFree(memboundExtent *extent, void *ptr);
void *memboundExtentRealloc(memboundExtent *extent, void *ptr, size_t nBytes);

/* Extent queries */
bool memboundExtentOwns(const memboundExtent *extent, const void *ptr);
size_t memboundExtentSize(const memboundExtent *extent, const void *ptr);
size_t memboundExtentBytesUsed(const memboundExtent *extent);
size_t memboundExtentBytesAvailable(const memboundExtent *extent);
size_t memboundExtentCapacity(const memboundExtent *extent);

/* ====================================================================
 * Index Internal Functions
 * ==================================================================== */

/* Index lifecycle */
void memboundIndexInit(memboundExtentIndex *index);
void memboundIndexFree(memboundExtentIndex *index);

/* Index operations */
bool memboundIndexAdd(memboundExtentIndex *index, memboundExtent *extent);
bool memboundIndexRemove(memboundExtentIndex *index, memboundExtent *extent);
memboundExtent *memboundIndexLookup(const memboundExtentIndex *index,
                                    const void *ptr);

/* SIMD-optimized lookup for many extents */
memboundExtent *memboundIndexLookupSIMD(const memboundExtentIndex *index,
                                        const void *ptr);

/* ====================================================================
 * Growth Strategy Functions
 * ==================================================================== */

/* Calculate next extent size based on strategy */
size_t memboundCalculateGrowthSize(const membound *m, size_t requestedBytes);

/* Check if growth is allowed */
bool memboundCanGrowBy(const membound *m, size_t additionalBytes);

/* Memory pressure calculation */
memboundPressure memboundCalculatePressure(const membound *m);
void memboundNotifyPressure(membound *m, memboundPressure level);

/* ====================================================================
 * Extent Selection Strategy Functions
 * ==================================================================== */

/* Initialize strategy config with defaults */
void memboundStrategyInit(memboundStrategyConfig *config);

/* Get the size class for an allocation size using configured thresholds */
static inline memboundSizeClass
memboundGetSizeClassWithThresholds(size_t nBytes, size_t smallThreshold,
                                   size_t mediumThreshold) {
    /* Use defaults if thresholds are 0 */
    const size_t small =
        smallThreshold ? smallThreshold : MEMBOUND_SIZE_CLASS_SMALL;
    const size_t medium =
        mediumThreshold ? mediumThreshold : MEMBOUND_SIZE_CLASS_MEDIUM;

    if (nBytes <= small) {
        return MEMBOUND_CLASS_SMALL;
    } else if (nBytes <= medium) {
        return MEMBOUND_CLASS_MEDIUM;
    }
    return MEMBOUND_CLASS_LARGE;
}

/* Get the size class for an allocation size using default thresholds */
static inline memboundSizeClass memboundGetSizeClass(size_t nBytes) {
    return memboundGetSizeClassWithThresholds(nBytes, 0, 0);
}

/* Calculate extent pool size (nBlock * szAtom) with overflow protection.
 * Returns the pool size in bytes, or 0 if overflow would occur.
 * This function should be used everywhere nBlock * szAtom is calculated. */
static inline size_t memboundExtentPoolSize(const memboundExtent *e) {
    if (!e || e->nBlock <= 0 || e->szAtom <= 0) {
        return 0;
    }
    /* Check for overflow: nBlock * szAtom > SIZE_MAX */
    if ((uint64_t)e->nBlock > SIZE_MAX / (uint64_t)e->szAtom) {
        return SIZE_MAX; /* Saturate rather than overflow */
    }
    return (size_t)((uint64_t)e->nBlock * (uint64_t)e->szAtom);
}

/* Calculate extent occupancy (0.0 - 1.0) with overflow protection */
static inline float memboundExtentOccupancy(const memboundExtent *e) {
    if (!e || e->size == 0) {
        return 0.0f;
    }
    const size_t poolSize = memboundExtentPoolSize(e);
    if (poolSize == 0) {
        return 0.0f;
    }
    /* Use double for better precision with large values */
    return (float)((double)e->currentOut / (double)poolSize);
}

/* Select extent for allocation using configured strategy */
memboundExtent *memboundSelectExtent(membound *m, size_t nBytes);

/* Select extent using best-fit strategy (fullest with space) */
memboundExtent *memboundSelectBestFit(membound *m, size_t nBytes);

/* Select extent using worst-fit strategy (emptiest with space) */
memboundExtent *memboundSelectWorstFit(membound *m, size_t nBytes);

/* Select extent using size-class segregation */
memboundExtent *memboundSelectSizeClass(membound *m, size_t nBytes);

/* Adaptive strategy: determine effective strategy based on state */
memboundExtentStrategy memboundAdaptiveGetStrategy(const membound *m);

/* Calculate overall fragmentation ratio (0.0 - 1.0) */
float memboundFragmentationRatio(const membound *m);

/* Scan and release all empty extents below threshold.
 * Note: This is an O(n) scan. For targeted release after a single
 * free operation, use memboundReleaseExtent directly. */
/* (Internal: memboundScanAndReleaseEmpty - called by public memboundShrink) */

/* ====================================================================
 * MemboundLink Structure (for free list)
 * ==================================================================== */

/* A minimum allocation is an instance of the following structure.
 * Larger allocations are an array of these structures where the
 * size of the array is a power of 2.
 *
 * The size of this object must be a power of two.
 * Using int64_t allows addressing extents up to exabytes in size. */
typedef struct MemboundLink {
    int64_t next; /* Index of next free chunk */
    int64_t prev; /* Index of previous free chunk */
} MemboundLink;

_Static_assert(sizeof(MemboundLink) == 16, "MemboundLink must be 16 bytes");

/* Masks used for aCtrl[] elements */
enum memboundCtrl {
    MEMBOUND_CTRL_LOGSIZE = 0x1f, /* Log2 size of this block */
    MEMBOUND_CTRL_FREE = 0x20     /* True if not checked out */
};

/* Convert block index to MemboundLink pointer */
#define MEMBOUND_LINK(extent, idx)                                             \
    ((MemboundLink *)(&(extent)->zPool[(idx) * (extent)->szAtom]))

/* ====================================================================
 * Fast Math Utilities
 * ==================================================================== */

/* Fast ceiling log2 using platform intrinsics - 64-bit version */
static inline int memboundLog64(size_t value) {
    if (value <= 1) {
        return 0;
    }

#if defined(__GNUC__) || defined(__clang__)
    /* Use 64-bit count leading zeros */
    return 64 - __builtin_clzll((unsigned long long)(value - 1));
#else
    int log = 0;
    value--;
    while (value >>= 1) {
        log++;
    }
    return log + 1;
#endif
}

/* Fast ceiling log2 using platform intrinsics - for block indices */
static inline int memboundLog(int64_t iValue) {
    if (iValue <= 1) {
        return 0;
    }

#if defined(__GNUC__) || defined(__clang__)
    return 64 - __builtin_clzll((unsigned long long)(iValue - 1));
#elif defined(__aarch64__)
    const int leadingZeros = __builtin_clzll((unsigned long long)(iValue - 1));
    return 64 - leadingZeros;
#elif defined(__amd64__) || defined(__x86_64__)
    unsigned long long result;
    asm("bsrq %1, %0" : "=r"(result) : "r"((unsigned long long)(iValue - 1)));
    return (int)(result + 1);
#else
    int iLog = 0;
    iValue--;
    while (iValue >>= 1) {
        iLog++;
    }
    return iLog + 1;
#endif
}

/* Next power of 2 >= x */
static inline size_t memboundNextPow2(size_t x) {
    if (x == 0) {
        return 1;
    }
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
#if SIZE_MAX > 0xFFFFFFFF
    x |= x >> 32;
#endif
    return x + 1;
}

/* ====================================================================
 * SIMD Memory Operations
 * ==================================================================== */

/* SIMD-accelerated memory zeroing */
void memboundZeroFast(void *dst, size_t bytes);

/* Threshold for using SIMD zeroing vs memset */
#define MEMBOUND_SIMD_ZERO_THRESHOLD 256

/* ====================================================================
 * Tier Detection and Helpers
 * ==================================================================== */

/* All tier structs have tier as the FIRST byte, so detection is simple.
 * Note: STANDARD/ENTERPRISE have memboundTier (4 bytes enum), but the
 * first byte will still be the tier value on little-endian systems.
 *
 * Tier enum values (after reordering for safe defaults):
 *   MEMBOUND_TIER_STANDARD = 0 (default for uninitialized configs)
 *   MEMBOUND_TIER_MICRO = 1
 *   MEMBOUND_TIER_COMPACT = 2
 *   MEMBOUND_TIER_ENTERPRISE = 3
 */

/* Check if a membound pointer is a MICRO tier pool. */
static inline bool memboundIsMicro(const void *m) {
    if (!m) {
        return false;
    }
    return *(const uint8_t *)m == MEMBOUND_TIER_MICRO;
}

/* Check if a membound pointer is a COMPACT tier pool. */
static inline bool memboundIsCompact(const void *m) {
    if (!m) {
        return false;
    }
    return *(const uint8_t *)m == MEMBOUND_TIER_COMPACT;
}

/* Check if a membound pointer is STANDARD or ENTERPRISE tier.
 * These use the full membound struct. */
static inline bool memboundIsStandardOrEnterprise(const void *m) {
    if (!m) {
        return false;
    }
    uint8_t t = *(const uint8_t *)m;
    return t == MEMBOUND_TIER_STANDARD || t == MEMBOUND_TIER_ENTERPRISE;
}

/* Get tier from any membound pointer.
 * Works because all structs have tier as first byte. */
static inline memboundTier memboundGetTierInternal(const void *m) {
    if (!m) {
        return MEMBOUND_TIER_STANDARD;
    }
    uint8_t t = *(const uint8_t *)m;
    /* Validate it's a known tier */
    if (t <= MEMBOUND_TIER_ENTERPRISE) {
        return (memboundTier)t;
    }
    return MEMBOUND_TIER_STANDARD; /* Default for unknown */
}

/* ====================================================================
 * MICRO Tier Internal Functions
 * ==================================================================== */

/* MICRO tier lifecycle */
memboundMicro *memboundMicroCreate(size_t size);
void memboundMicroDestroy(memboundMicro *m);
void memboundMicroReset(memboundMicro *m);

/* MICRO tier allocation */
void *memboundMicroAlloc(memboundMicro *m, size_t nBytes);
void memboundMicroFree(memboundMicro *m, void *ptr);

/* MICRO tier queries */
bool memboundMicroOwns(const memboundMicro *m, const void *ptr);
size_t memboundMicroSize(const memboundMicro *m, const void *ptr);
size_t memboundMicroBytesUsed(const memboundMicro *m);
size_t memboundMicroBytesAvailable(const memboundMicro *m);
size_t memboundMicroCapacity(const memboundMicro *m);
size_t memboundMicroAllocationCount(const memboundMicro *m);

/* ====================================================================
 * COMPACT Tier Internal Functions
 * ====================================================================
 * Note: Use 'memboundCompactPool' struct name to avoid collision with
 * the public memboundCompact() function.
 */

/* COMPACT tier lifecycle */
memboundCompactPool *memboundCompactPoolCreate(size_t size, bool threadSafe);
void memboundCompactPoolDestroy(memboundCompactPool *m);
void memboundCompactPoolReset(memboundCompactPool *m);

/* COMPACT tier allocation */
void *memboundCompactPoolAlloc(memboundCompactPool *m, size_t nBytes);
void memboundCompactPoolFree(memboundCompactPool *m, void *ptr);

/* COMPACT tier queries */
bool memboundCompactPoolOwns(const memboundCompactPool *m, const void *ptr);
size_t memboundCompactPoolSize(const memboundCompactPool *m, const void *ptr);
size_t memboundCompactPoolBytesUsed(const memboundCompactPool *m);
size_t memboundCompactPoolBytesAvailable(const memboundCompactPool *m);
size_t memboundCompactPoolCapacity(const memboundCompactPool *m);
size_t memboundCompactPoolAllocationCount(const memboundCompactPool *m);
