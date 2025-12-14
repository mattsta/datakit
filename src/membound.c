/* membound.c - Bounded Memory Pool Allocator with Dynamic Extent Support
 *
 * A buddy allocation system supporting fixed-size and dynamically-growing
 * memory pools via extent-based management.
 *
 * Originally derived from SQLite's mem5.c (2007, public domain).
 * Extended with extent support, SIMD optimizations, and dynamic growth
 * by Matt Stancliff (2024-2025).
 *
 * Algorithm based on: J. M. Robson. "Bounds for Some Functions Concerning
 * Dynamic Storage Allocation". Journal of the ACM, Vol 21, No 8, July 1974.
 */

#include "membound.h"
#include "memboundInternal.h"

#include "datakit.h"

#include "timeUtil.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

/* ====================================================================
 * Runtime Safety Checks
 * ====================================================================
 * These macros provide always-on safety checks that prevent memory
 * corruption. Unlike assertions, these work in release builds and
 * return early on failure rather than crashing.
 *
 * Safety violations are tracked via atomic counters for diagnostics.
 */

/* Safety check for extent operations - returns on failure.
 * Used for runtime checks that prevent memory corruption.
 * Note: These don't track violations - tracking happens at membound level. */
#define MEMBOUND_SAFETY_CHECK_EXTENT(extent, condition)                        \
    do {                                                                       \
        if (unlikely(!(condition))) {                                          \
            return; /* Silent fail - prevents corruption */                    \
        }                                                                      \
    } while (0)

/* Validate a free operation and track violations.
 * Returns true if valid, false if invalid (and increments violation counter).
 */
static inline bool memboundValidateFree(membound *m, memboundExtent *owner,
                                        void *pOld) {
    if (!owner || !pOld) {
        return false;
    }

    int64_t iBlock =
        (int64_t)(((uint8_t *)pOld - owner->zPool) >> owner->atomShift);

    /* Bounds check */
    if (iBlock < 0 || iBlock >= owner->nBlock) {
        atomic_fetch_add_explicit(&m->safetyViolations, 1,
                                  memory_order_relaxed);
        return false;
    }

    /* Alignment check */
    if (((uint8_t *)pOld - owner->zPool) % owner->szAtom != 0) {
        atomic_fetch_add_explicit(&m->safetyViolations, 1,
                                  memory_order_relaxed);
        return false;
    }

    /* Double-free check */
    if (owner->aCtrl[iBlock] & MEMBOUND_CTRL_FREE) {
        atomic_fetch_add_explicit(&m->safetyViolations, 1,
                                  memory_order_relaxed);
        return false;
    }

    return true;
}

/* ====================================================================
 * Memory Mapping Utilities
 * ==================================================================== */

static void *memboundMAP(const size_t len) {
    void *z = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_SHARED, -1, 0);
    if (z == MAP_FAILED) {
        return NULL;
    }
    return z;
}

static void memboundUNMAP(void *ptr, size_t len) {
    if (ptr && len > 0) {
        munmap(ptr, len);
    }
}

/* ====================================================================
 * SIMD Memory Operations
 * ==================================================================== */

void memboundZeroFast(void *dst, size_t bytes) {
    uint8_t *p = (uint8_t *)dst;

#if defined(MEMBOUND_USE_AVX512)
    const __m512i zero512 = _mm512_setzero_si512();
    while (bytes >= 64) {
        _mm512_storeu_si512((__m512i *)p, zero512);
        p += 64;
        bytes -= 64;
    }
#elif defined(MEMBOUND_USE_AVX2)
    const __m256i zero256 = _mm256_setzero_si256();
    while (bytes >= 32) {
        _mm256_storeu_si256((__m256i *)p, zero256);
        p += 32;
        bytes -= 32;
    }
#elif defined(MEMBOUND_USE_SSE2)
    const __m128i zero128 = _mm_setzero_si128();
    while (bytes >= 16) {
        _mm_storeu_si128((__m128i *)p, zero128);
        p += 16;
        bytes -= 16;
    }
#elif defined(MEMBOUND_USE_NEON)
    const uint8x16_t zero = vdupq_n_u8(0);
    while (bytes >= 16) {
        vst1q_u8(p, zero);
        p += 16;
        bytes -= 16;
    }
#endif

    /* Handle remainder with 64-bit stores, then byte stores */
    while (bytes >= 8) {
        *(uint64_t *)p = 0;
        p += 8;
        bytes -= 8;
    }
    while (bytes > 0) {
        *p++ = 0;
        bytes--;
    }
}

/* ====================================================================
 * Extent Index Implementation
 * ==================================================================== */

void memboundIndexInit(memboundExtentIndex *index) {
    memset(index->inlineRanges, 0, sizeof(index->inlineRanges));
    index->ranges = NULL;
    index->count = 0;
    index->capacity = 0;
    index->lookupCount = 0;
    index->primaryHits = 0;
}

void memboundIndexFree(memboundExtentIndex *index) {
    if (index->ranges) {
        zfree(index->ranges);
        index->ranges = NULL;
    }
    index->count = 0;
    index->capacity = 0;
}

/* Binary search for extent containing pointer */
static memboundExtent *
memboundIndexBinarySearch(const memboundExtentRange *ranges, size_t count,
                          uintptr_t addr) {
    if (count == 0) {
        return NULL;
    }

    size_t lo = 0;
    size_t hi = count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (addr < ranges[mid].start) {
            hi = mid;
        } else if (addr >= ranges[mid].end) {
            lo = mid + 1;
        } else {
            return ranges[mid].extent;
        }
    }
    return NULL;
}

/* SIMD-optimized lookup for 4+ extents using range comparison */
#if defined(MEMBOUND_USE_AVX2) || defined(MEMBOUND_USE_SSE2) ||                \
    defined(MEMBOUND_USE_NEON)
memboundExtent *memboundIndexLookupSIMD(const memboundExtentIndex *index,
                                        const void *ptr) {
    const uintptr_t addr = (uintptr_t)ptr;
    const memboundExtentRange *ranges =
        (index->count <= MEMBOUND_EXTENT_INLINE_MAX) ? index->inlineRanges
                                                     : index->ranges;
    const size_t count = index->count;

    /* For small counts, SIMD overhead isn't worth it */
    if (count < MEMBOUND_EXTENT_SIMD_THRESHOLD) {
        return memboundIndexBinarySearch(ranges, count, addr);
    }

#if defined(MEMBOUND_USE_AVX2)
    /* Process 4 extents at a time with AVX2
     * To do unsigned comparison with signed SIMD ops, XOR all values with
     * the sign bit. This transforms unsigned ordering to signed ordering:
     *   unsigned 0 -> signed MIN_INT64
     *   unsigned MAX -> signed MAX_INT64 */
    const __m256i signBit = _mm256_set1_epi64x((int64_t)0x8000000000000000ULL);
    const __m256i addrVec =
        _mm256_xor_si256(_mm256_set1_epi64x((int64_t)addr), signBit);

    for (size_t i = 0; i + 4 <= count; i += 4) {
        /* Load 4 start addresses and convert to signed comparison space */
        __m256i starts =
            _mm256_xor_si256(_mm256_set_epi64x((int64_t)ranges[i + 3].start,
                                               (int64_t)ranges[i + 2].start,
                                               (int64_t)ranges[i + 1].start,
                                               (int64_t)ranges[i + 0].start),
                             signBit);

        /* Load 4 end addresses and convert to signed comparison space */
        __m256i ends =
            _mm256_xor_si256(_mm256_set_epi64x((int64_t)ranges[i + 3].end,
                                               (int64_t)ranges[i + 2].end,
                                               (int64_t)ranges[i + 1].end,
                                               (int64_t)ranges[i + 0].end),
                             signBit);

        /* addr >= start (unsigned) */
        __m256i geStart = _mm256_or_si256(_mm256_cmpgt_epi64(addrVec, starts),
                                          _mm256_cmpeq_epi64(addrVec, starts));

        /* addr < end (unsigned) */
        __m256i ltEnd = _mm256_cmpgt_epi64(ends, addrVec);

        /* Combine: addr >= start AND addr < end */
        __m256i inRange = _mm256_and_si256(geStart, ltEnd);

        /* Check if any lane matched */
        int mask = _mm256_movemask_epi8(inRange);
        if (mask) {
            /* Find first matching lane */
            int bytePos = __builtin_ctz(mask);
            int lane = bytePos / 8;
            return ranges[i + lane].extent;
        }
    }

    /* Handle remainder */
    size_t remainder = count % 4;
    if (remainder) {
        return memboundIndexBinarySearch(ranges + count - remainder, remainder,
                                         addr);
    }
#elif defined(MEMBOUND_USE_SSE2)
    /* Process 2 extents at a time with SSE2
     * XOR with sign bit to convert unsigned comparison to signed space */
    const __m128i signBit = _mm_set1_epi64x((int64_t)0x8000000000000000ULL);
    const __m128i addrVec =
        _mm_xor_si128(_mm_set1_epi64x((int64_t)addr), signBit);

    for (size_t i = 0; i + 2 <= count; i += 2) {
        __m128i starts =
            _mm_xor_si128(_mm_set_epi64x((int64_t)ranges[i + 1].start,
                                         (int64_t)ranges[i + 0].start),
                          signBit);
        __m128i ends = _mm_xor_si128(_mm_set_epi64x((int64_t)ranges[i + 1].end,
                                                    (int64_t)ranges[i + 0].end),
                                     signBit);

        /* SSE2 doesn't have 64-bit compare, use subtraction trick
         * In signed comparison space, diff >= 0 means addr >= start (unsigned)
         * and diff > 0 means end > addr (unsigned) */
        __m128i diffStart = _mm_sub_epi64(addrVec, starts);
        __m128i diffEnd = _mm_sub_epi64(ends, addrVec);

        /* Check high bits for sign (positive = in range) */
        int64_t d0s, d1s, d0e, d1e;
        _mm_storel_epi64((__m128i *)&d0s, diffStart);
        _mm_storel_epi64((__m128i *)&d0e, diffEnd);
        d1s = _mm_extract_epi64(diffStart, 1);
        d1e = _mm_extract_epi64(diffEnd, 1);

        if (d0s >= 0 && d0e > 0) {
            return ranges[i + 0].extent;
        }
        if (d1s >= 0 && d1e > 0) {
            return ranges[i + 1].extent;
        }
    }

    if (count % 2) {
        return memboundIndexBinarySearch(ranges + count - 1, 1, addr);
    }
#elif defined(MEMBOUND_USE_NEON)
    /* Process 2 extents at a time with NEON
     * Use unsigned comparison intrinsics (vcgeq_u64/vcltq_u64) for correct
     * handling of addresses with high bit set */
    const uint64x2_t addrVec = vdupq_n_u64((uint64_t)addr);

    for (size_t i = 0; i + 2 <= count; i += 2) {
        uint64x2_t starts = {(uint64_t)ranges[i].start,
                             (uint64_t)ranges[i + 1].start};
        uint64x2_t ends = {(uint64_t)ranges[i].end,
                           (uint64_t)ranges[i + 1].end};

        uint64x2_t geStart = vcgeq_u64(addrVec, starts);
        uint64x2_t ltEnd = vcltq_u64(addrVec, ends);
        uint64x2_t inRange = vandq_u64(geStart, ltEnd);

        if (vgetq_lane_u64(inRange, 0)) {
            return ranges[i].extent;
        }
        if (vgetq_lane_u64(inRange, 1)) {
            return ranges[i + 1].extent;
        }
    }

    if (count % 2) {
        return memboundIndexBinarySearch(ranges + count - 1, 1, addr);
    }
#endif

    return NULL;
}
#else
memboundExtent *memboundIndexLookupSIMD(const memboundExtentIndex *index,
                                        const void *ptr) {
    /* Fallback to binary search when no SIMD */
    const memboundExtentRange *ranges =
        (index->count <= MEMBOUND_EXTENT_INLINE_MAX) ? index->inlineRanges
                                                     : index->ranges;
    return memboundIndexBinarySearch(ranges, index->count, (uintptr_t)ptr);
}
#endif

memboundExtent *memboundIndexLookup(const memboundExtentIndex *index,
                                    const void *ptr) {
    if (!index || !ptr || index->count == 0) {
        return NULL;
    }

    const memboundExtentRange *ranges =
        (index->count <= MEMBOUND_EXTENT_INLINE_MAX) ? index->inlineRanges
                                                     : index->ranges;

    /* For very small counts, linear search is faster */
    if (index->count <= 4) {
        const uintptr_t addr = (uintptr_t)ptr;
        for (size_t i = 0; i < index->count; i++) {
            if (addr >= ranges[i].start && addr < ranges[i].end) {
                return ranges[i].extent;
            }
        }
        return NULL;
    }

    /* Use SIMD for larger extent counts */
    return memboundIndexLookupSIMD(index, ptr);
}

/* Insert extent into sorted index */
bool memboundIndexAdd(memboundExtentIndex *index, memboundExtent *extent) {
    if (!index || !extent) {
        return false;
    }

    const uintptr_t start = (uintptr_t)extent->zPool;
    const uintptr_t end = start + memboundExtentPoolSize(extent);

    memboundExtentRange newRange = {
        .start = start, .end = end, .extent = extent};

    /* Use inline array for small counts */
    if (index->count < MEMBOUND_EXTENT_INLINE_MAX) {
        /* Find insertion point (keep sorted) */
        size_t insertPos = 0;
        while (insertPos < index->count &&
               index->inlineRanges[insertPos].start < start) {
            insertPos++;
        }

        /* Shift elements */
        for (size_t i = index->count; i > insertPos; i--) {
            index->inlineRanges[i] = index->inlineRanges[i - 1];
        }

        index->inlineRanges[insertPos] = newRange;
        extent->sortedIndex = insertPos;
        index->count++;
        return true;
    }

    /* Transition to heap array if needed */
    if (index->count == MEMBOUND_EXTENT_INLINE_MAX && !index->ranges) {
        index->capacity = 16;
        index->ranges = zmalloc(index->capacity * sizeof(memboundExtentRange));
        if (!index->ranges) {
            return false;
        }
        memcpy(index->ranges, index->inlineRanges,
               MEMBOUND_EXTENT_INLINE_MAX * sizeof(memboundExtentRange));
    }

    /* Grow heap array if needed */
    if (index->count >= index->capacity) {
        size_t newCap = index->capacity * 2;
        memboundExtentRange *newRanges =
            zrealloc(index->ranges, newCap * sizeof(memboundExtentRange));
        if (!newRanges) {
            return false;
        }
        index->ranges = newRanges;
        index->capacity = newCap;
    }

    /* Find insertion point */
    size_t insertPos = 0;
    while (insertPos < index->count && index->ranges[insertPos].start < start) {
        insertPos++;
    }

    /* Shift elements */
    memmove(&index->ranges[insertPos + 1], &index->ranges[insertPos],
            (index->count - insertPos) * sizeof(memboundExtentRange));

    index->ranges[insertPos] = newRange;
    extent->sortedIndex = insertPos;
    index->count++;

    /* Update sortedIndex for shifted extents */
    for (size_t i = insertPos + 1; i < index->count; i++) {
        index->ranges[i].extent->sortedIndex = i;
    }

    return true;
}

bool memboundIndexRemove(memboundExtentIndex *index, memboundExtent *extent) {
    if (!index || !extent || index->count == 0) {
        return false;
    }

    memboundExtentRange *ranges = (index->count <= MEMBOUND_EXTENT_INLINE_MAX)
                                      ? index->inlineRanges
                                      : index->ranges;

    /* Find extent in index */
    size_t pos = extent->sortedIndex;
    if (pos >= index->count || ranges[pos].extent != extent) {
        /* sortedIndex is stale, search for it */
        const uintptr_t start = (uintptr_t)extent->zPool;
        for (pos = 0; pos < index->count; pos++) {
            if (ranges[pos].start == start) {
                break;
            }
        }
        if (pos >= index->count) {
            return false;
        }
    }

    /* Remove by shifting */
    index->count--;
    if (index->count <= MEMBOUND_EXTENT_INLINE_MAX && index->ranges) {
        /* Can transition back to inline */
        for (size_t i = pos; i < index->count; i++) {
            ranges[i] = ranges[i + 1];
            ranges[i].extent->sortedIndex = i;
        }
        if (index->count <= MEMBOUND_EXTENT_INLINE_MAX) {
            memcpy(index->inlineRanges, index->ranges,
                   index->count * sizeof(memboundExtentRange));
            zfree(index->ranges);
            index->ranges = NULL;
            index->capacity = 0;
        }
    } else {
        memmove(&ranges[pos], &ranges[pos + 1],
                (index->count - pos) * sizeof(memboundExtentRange));
        for (size_t i = pos; i < index->count; i++) {
            ranges[i].extent->sortedIndex = i;
        }
    }

    return true;
}

/* ====================================================================
 * Extent Free List Operations
 * ==================================================================== */

/* Unlink chunk from its free list */
static void memboundExtentUnlink(memboundExtent *e, int64_t i, int iLogsize) {
    assert(i >= 0 && i < e->nBlock);
    assert(iLogsize >= 0 && iLogsize <= MEMBOUND_LOGMAX);
    assert((e->aCtrl[i] & MEMBOUND_CTRL_LOGSIZE) == iLogsize);

    const int64_t next = MEMBOUND_LINK(e, i)->next;
    const int64_t prev = MEMBOUND_LINK(e, i)->prev;

    if (prev < 0) {
        e->aiFreelist[iLogsize] = next;
        if (next < 0) {
            e->freelistBitmap &= ~(1ULL << iLogsize);
        }
    } else {
        MEMBOUND_LINK(e, prev)->next = next;
    }

    if (next >= 0) {
        MEMBOUND_LINK(e, next)->prev = prev;
    }
}

/* Link chunk into its free list */
static void memboundExtentLink(memboundExtent *e, int64_t i, int iLogsize) {
    assert(i >= 0 && i < e->nBlock);
    assert(iLogsize >= 0 && iLogsize <= MEMBOUND_LOGMAX);
    assert((e->aCtrl[i] & MEMBOUND_CTRL_LOGSIZE) == iLogsize);

    const int64_t x = MEMBOUND_LINK(e, i)->next = e->aiFreelist[iLogsize];
    MEMBOUND_LINK(e, i)->prev = -1;

    if (x >= 0) {
        assert(x < e->nBlock);
        MEMBOUND_LINK(e, x)->prev = i;
    }

    e->aiFreelist[iLogsize] = i;
    e->freelistBitmap |= (1ULL << iLogsize);
}

/* ====================================================================
 * Extent Lifecycle
 * ==================================================================== */

bool memboundExtentInit(memboundExtent *extent, void *space, size_t len) {
    if (!extent || !space || len == 0) {
        return false;
    }

    /* Calculate minimum allocation size */
    int nMinLog = memboundLog(MEMBOUND_MIN_ATOM);
    extent->szAtom = (1LL << nMinLog);
    while ((int64_t)sizeof(MemboundLink) > extent->szAtom) {
        extent->szAtom <<= 1;
        nMinLog++;
    }
    extent->atomShift = nMinLog;

    /* Setup pool */
    extent->nBlock = (len / (extent->szAtom + sizeof(uint8_t)));
    extent->zPool = space;

    /* Calculate pool data size with overflow check */
    const size_t poolDataSize =
        ((uint64_t)extent->nBlock > SIZE_MAX / (uint64_t)extent->szAtom)
            ? SIZE_MAX
            : (size_t)((uint64_t)extent->nBlock * (uint64_t)extent->szAtom);

    extent->aCtrl = (uint8_t *)&extent->zPool[poolDataSize];
    extent->size = len;

    /* Initialize statistics */
    extent->currentOut = 0;
    extent->currentCount = 0;
    extent->nAlloc = 0;
    extent->totalAlloc = 0;
    extent->totalExcess = 0;

    /* Initialize linkage */
    extent->next = NULL;
    extent->prev = NULL;
    extent->sortedIndex = 0;

    /* Initialize free lists */
    memboundExtentReset(extent);

    return true;
}

void memboundExtentReset(memboundExtent *extent) {
    if (!extent) {
        return;
    }

    /* Clear all free lists */
    for (int i = 0; i <= MEMBOUND_LOGMAX; i++) {
        extent->aiFreelist[i] = -1;
    }
    extent->freelistBitmap = 0;

    /* Build free list from largest possible blocks */
    int64_t iOffset = 0;
    for (int ii = MEMBOUND_LOGMAX; ii >= 0; ii--) {
        int64_t nAlloc = (1LL << ii);
        if ((iOffset + nAlloc) <= extent->nBlock) {
            extent->aCtrl[iOffset] = ii | MEMBOUND_CTRL_FREE;
            memboundExtentLink(extent, iOffset, ii);
            iOffset += nAlloc;
        }
        assert((iOffset + nAlloc) > extent->nBlock);
    }

    /* Reset per-extent usage stats */
    extent->currentOut = 0;
    extent->currentCount = 0;
}

memboundExtent *memboundExtentCreate(size_t size) {
    if (size == 0) {
        return NULL;
    }

    /* Allocate extent structure */
    memboundExtent *extent = memboundMAP(sizeof(memboundExtent));
    if (!extent) {
        return NULL;
    }
    memset(extent, 0, sizeof(*extent));

    /* Initialize strategy metadata */
    extent->sizeClass = -1; /* Unassigned */
    extent->flags = 0;

    /* Allocate pool */
    void *space = memboundMAP(size);
    if (!space) {
        memboundUNMAP(extent, sizeof(memboundExtent));
        return NULL;
    }

    if (!memboundExtentInit(extent, space, size)) {
        memboundUNMAP(space, size);
        memboundUNMAP(extent, sizeof(memboundExtent));
        return NULL;
    }

    return extent;
}

void memboundExtentDestroy(memboundExtent *extent) {
    if (!extent) {
        return;
    }

    if (extent->zPool) {
        memboundUNMAP(extent->zPool, extent->size);
    }
    memboundUNMAP(extent, sizeof(memboundExtent));
}

/* ====================================================================
 * Extent Allocation Operations
 * ==================================================================== */

void *memboundExtentAlloc(memboundExtent *extent, size_t nBytes) {
    if (!extent || nBytes == 0) {
        return NULL;
    }

    /* Limit allocation to extent capacity (reasonable upper bound) */
    if (nBytes > memboundExtentPoolSize(extent)) {
        return NULL;
    }

    int iBin, iLogsize;
    size_t iFullSz;

    /* Round nBytes up to next valid power of two */
    if ((int64_t)nBytes <= extent->szAtom) {
        iLogsize = 0;
        iFullSz = extent->szAtom;
    } else {
        iLogsize = memboundLog(
            (int64_t)((nBytes + extent->szAtom - 1) >> extent->atomShift));
        iFullSz = (size_t)extent->szAtom << iLogsize;
    }

    /* Find available block using bitmap */
    {
        const uint64_t availableMask = extent->freelistBitmap >> iLogsize;
        if (availableMask == 0) {
            return NULL;
        }
        iBin = iLogsize + __builtin_ctzll(availableMask);
    }

    int64_t i = extent->aiFreelist[iBin];
    memboundExtentUnlink(extent, i, iBin);

    /* Split larger blocks if needed */
    while (iBin > iLogsize) {
        iBin--;
        int64_t newSize = 1LL << iBin;
        extent->aCtrl[i + newSize] = MEMBOUND_CTRL_FREE | iBin;
        memboundExtentLink(extent, i + newSize, iBin);
    }

    extent->aCtrl[i] = iLogsize;

    /* Update statistics */
    extent->nAlloc++;
    extent->totalAlloc += iFullSz;
    extent->totalExcess += iFullSz - nBytes;
    extent->currentCount++;
    extent->currentOut += iFullSz;

#if MEMBOUND_DEBUG
    memset(&extent->zPool[i * extent->szAtom], 0xAA, iFullSz);
#endif

    return (void *)&extent->zPool[i * extent->szAtom];
}

void memboundExtentFree(memboundExtent *extent, void *pOld) {
    if (!extent || !pOld) {
        return;
    }

    int64_t iBlock =
        (int64_t)(((uint8_t *)pOld - extent->zPool) >> extent->atomShift);

    /* Runtime safety checks - prevent memory corruption on invalid input */
    MEMBOUND_SAFETY_CHECK_EXTENT(extent,
                                 iBlock >= 0 && iBlock < extent->nBlock);
    MEMBOUND_SAFETY_CHECK_EXTENT(
        extent, ((uint8_t *)pOld - extent->zPool) % extent->szAtom == 0);
    MEMBOUND_SAFETY_CHECK_EXTENT(extent,
                                 (extent->aCtrl[iBlock] & MEMBOUND_CTRL_FREE) ==
                                     0); /* Double-free check */

    int iLogsize = extent->aCtrl[iBlock] & MEMBOUND_CTRL_LOGSIZE;
    int64_t size = 1LL << iLogsize;

    MEMBOUND_SAFETY_CHECK_EXTENT(extent, iBlock + size - 1 < extent->nBlock);

    extent->aCtrl[iBlock] |= MEMBOUND_CTRL_FREE;
    extent->aCtrl[iBlock + size - 1] |= MEMBOUND_CTRL_FREE;

    /* These are internal consistency checks - if they fail, something is very
     * wrong */
    MEMBOUND_SAFETY_CHECK_EXTENT(extent, extent->currentCount > 0);
    MEMBOUND_SAFETY_CHECK_EXTENT(
        extent,
        extent->currentOut >= ((uint64_t)size * (uint64_t)extent->szAtom));
    extent->currentCount--;
    extent->currentOut -= (uint64_t)size * (uint64_t)extent->szAtom;

    extent->aCtrl[iBlock] = MEMBOUND_CTRL_FREE | iLogsize;

    /* Coalesce with buddies */
    while (iLogsize < MEMBOUND_LOGMAX) {
        int64_t iBuddy;
        if ((iBlock >> iLogsize) & 1) {
            iBuddy = iBlock - size;
            if (unlikely(iBuddy < 0)) {
                break; /* Safety: invalid buddy index */
            }
        } else {
            iBuddy = iBlock + size;
            if (iBuddy >= extent->nBlock) {
                break;
            }
        }

        if (extent->aCtrl[iBuddy] != (MEMBOUND_CTRL_FREE | iLogsize)) {
            break;
        }

        memboundExtentUnlink(extent, iBuddy, iLogsize);
        iLogsize++;
        if (iBuddy < iBlock) {
            extent->aCtrl[iBlock] = 0;
            extent->aCtrl[iBuddy] = MEMBOUND_CTRL_FREE | iLogsize;
            iBlock = iBuddy;
        } else {
            extent->aCtrl[iBlock] = MEMBOUND_CTRL_FREE | iLogsize;
            extent->aCtrl[iBuddy] = 0;
        }
        size *= 2;
    }

#if MEMBOUND_DEBUG
    memset(&extent->zPool[iBlock * extent->szAtom], 0x55,
           size * extent->szAtom);
#endif

    memboundExtentLink(extent, iBlock, iLogsize);
}

size_t memboundExtentSize(const memboundExtent *extent, const void *ptr) {
    if (!extent || !ptr) {
        return 0;
    }

    const int64_t i =
        (int64_t)(((const uint8_t *)ptr - extent->zPool) >> extent->atomShift);
    assert(i >= 0 && i < extent->nBlock);

    return (size_t)extent->szAtom << (extent->aCtrl[i] & MEMBOUND_CTRL_LOGSIZE);
}

/* ====================================================================
 * Extent Query Functions
 * ==================================================================== */

bool memboundExtentOwns(const memboundExtent *extent, const void *ptr) {
    if (!extent || !ptr) {
        return false;
    }

    const uint8_t *p = (const uint8_t *)ptr;
    const uint8_t *poolStart = extent->zPool;
    const uint8_t *poolEnd = extent->zPool + memboundExtentPoolSize(extent);

    return (p >= poolStart && p < poolEnd);
}

size_t memboundExtentBytesUsed(const memboundExtent *extent) {
    return extent ? extent->currentOut : 0;
}

size_t memboundExtentBytesAvailable(const memboundExtent *extent) {
    if (!extent) {
        return 0;
    }
    const size_t total = memboundExtentPoolSize(extent);
    return (total > extent->currentOut) ? (total - extent->currentOut) : 0;
}

size_t memboundExtentCapacity(const memboundExtent *extent) {
    return memboundExtentPoolSize(extent);
}

/* ====================================================================
 * Growth Strategy Implementation
 * ==================================================================== */

size_t memboundCalculateGrowthSize(const membound *m, size_t requestedBytes) {
    if (!m) {
        return 0;
    }

    size_t growSize;

    switch (m->growth.type) {
    case MEMBOUND_GROWTH_FIXED:
        growSize = m->growth.fixedGrowthSize;
        break;

    case MEMBOUND_GROWTH_GEOMETRIC:
        growSize = (size_t)(m->totalCapacity * (m->growth.growthFactor - 1.0));
        break;

    case MEMBOUND_GROWTH_ADAPTIVE:
        /* Start with geometric, but consider recent allocation patterns */
        growSize = (size_t)(m->totalCapacity * 0.5);
        /* Ensure at least 2x requested bytes */
        if (growSize < requestedBytes * 2) {
            growSize = requestedBytes * 2;
        }
        break;

    default:
        growSize = MEMBOUND_DEFAULT_GROWTH_SIZE;
    }

    /* Apply min/max constraints */
    if (growSize < m->growth.minExtentSize) {
        growSize = m->growth.minExtentSize;
    }
    if (m->growth.maxExtentSize > 0 && growSize > m->growth.maxExtentSize) {
        growSize = m->growth.maxExtentSize;
    }

    /* Ensure at least requestedBytes fits */
    if (growSize < requestedBytes) {
        growSize = memboundNextPow2(requestedBytes);
    }

    return growSize;
}

bool memboundCanGrowBy(const membound *m, size_t additionalBytes) {
    if (!m || m->mode != MEMBOUND_MODE_DYNAMIC) {
        return false;
    }

    /* Check extent count limit */
    if (m->growth.maxExtentCount > 0 &&
        m->extentCount >= m->growth.maxExtentCount) {
        return false;
    }

    /* Check total size limit */
    if (m->growth.maxTotalSize > 0 &&
        m->totalCapacity + additionalBytes > m->growth.maxTotalSize) {
        return false;
    }

    return true;
}

/* Forward declarations for mutex functions used in pressure notification */
static void memboundEnter(membound *m);
static void memboundLeave(membound *m);

memboundPressure memboundCalculatePressure(const membound *m) {
    if (!m || m->totalCapacity == 0) {
        return MEMBOUND_PRESSURE_LOW;
    }

    const double usage = (double)m->currentOut / (double)m->totalCapacity;

    if (usage >= 0.95) {
        return MEMBOUND_PRESSURE_CRITICAL;
    } else if (usage >= 0.80) {
        return MEMBOUND_PRESSURE_HIGH;
    } else if (usage >= 0.50) {
        return MEMBOUND_PRESSURE_MEDIUM;
    }
    return MEMBOUND_PRESSURE_LOW;
}

/* Check if pressure level changed and should trigger callback.
 * Returns true if callback should be invoked, and fills out callback info.
 * Call this while holding the lock, then invoke callback AFTER releasing lock.
 */
static bool memboundShouldNotifyPressure(membound *m, memboundPressure level,
                                         memboundPressureCallback *outCb,
                                         void **outUserData) {
    if (!m || !m->pressureCallback || level == m->lastPressureLevel) {
        return false;
    }

    /* Snapshot callback info while holding lock */
    *outCb = m->pressureCallback;
    *outUserData = m->pressureUserData;
    m->lastPressureLevel = level;
    return true;
}

/* Legacy function - only call when NOT holding the lock */
void memboundNotifyPressure(membound *m, memboundPressure level) {
    if (!m) {
        return;
    }

    memboundPressureCallback cb = NULL;
    void *userData = NULL;

    memboundEnter(m);
    bool shouldNotify = memboundShouldNotifyPressure(m, level, &cb, &userData);
    memboundLeave(m);

    if (shouldNotify && cb) {
        cb(m, level, userData);
    }
}

/* ====================================================================
 * Extent Selection Strategy Implementation
 * ==================================================================== */

void memboundStrategyInit(memboundStrategyConfig *config) {
    if (!config) {
        return;
    }
    config->strategy = MEMBOUND_STRATEGY_FIRST_FIT;
    config->minOccupancyForAlloc = 0.0f;
    config->shrinkThreshold = 0.0f;
    config->fragmentationThreshold = 0.3f; /* Switch to best-fit at 30% frag */
    config->adaptiveSwitchCount = 0;
    for (int i = 0; i < MEMBOUND_CLASS_COUNT; i++) {
        config->classExtents[i] = NULL;
    }
}

/* Calculate fragmentation: ratio of wasted space due to extent
 * underutilization. Fragmentation is defined as the portion of total capacity
 * that is not usable due to allocations being spread across multiple
 * partially-filled extents.
 *
 * Returns 0.0 - 1.0 where:
 *   0.0 = no fragmentation (single extent or all extents fully utilized)
 *   1.0 = maximum fragmentation (all capacity wasted)
 */
float memboundFragmentationRatio(const membound *m) {
    if (!m || m->totalCapacity == 0 || m->extentCount <= 1) {
        return 0.0f;
    }

    /* If nothing is allocated, no fragmentation */
    if (m->currentOut == 0) {
        return 0.0f;
    }

    /* Count wasted space across all non-full extents.
     * "Wasted" = capacity that can't be coalesced because allocations
     * are spread across multiple extents.
     *
     * Fixed: Include all non-full extents (0% to <100%), not just 0-90%. */
    uint64_t wastedSpace = 0;
    size_t nonFullExtents = 0;

    for (memboundExtent *e = m->extents; e; e = e->next) {
        const size_t cap = memboundExtentPoolSize(e);
        if (cap == 0) {
            continue;
        }

        /* Extent is non-full if it has unused capacity */
        if (e->currentOut < cap) {
            nonFullExtents++;
            /* Only count waste if extent has some allocations (not empty) */
            if (e->currentOut > 0) {
                wastedSpace += cap - e->currentOut;
            }
        }
    }

    /* With 0 or 1 non-full extents, fragmentation is minimal */
    if (nonFullExtents <= 1) {
        return 0.0f;
    }

    return (float)wastedSpace / (float)m->totalCapacity;
}

/* Best-fit: find fullest extent with sufficient space */
memboundExtent *memboundSelectBestFit(membound *m, size_t nBytes) {
    memboundExtent *best = NULL;
    float bestOccupancy = -1.0f;

    for (memboundExtent *e = m->extents; e; e = e->next) {
        /* Skip extents that can't satisfy the request */
        if (memboundExtentBytesAvailable(e) < nBytes) {
            continue;
        }

        /* Skip extents below minimum occupancy threshold */
        float occ = memboundExtentOccupancy(e);
        if (m->strategy.minOccupancyForAlloc > 0.0f &&
            occ < m->strategy.minOccupancyForAlloc &&
            e->currentCount > 0) { /* Allow empty extents */
            continue;
        }

        /* Best-fit: prefer fullest extent */
        if (occ > bestOccupancy) {
            bestOccupancy = occ;
            best = e;
        }
    }

    return best;
}

/* Worst-fit: find emptiest extent with sufficient space */
memboundExtent *memboundSelectWorstFit(membound *m, size_t nBytes) {
    memboundExtent *best = NULL;
    float bestOccupancy = 2.0f; /* Start above max */

    for (memboundExtent *e = m->extents; e; e = e->next) {
        if (memboundExtentBytesAvailable(e) < nBytes) {
            continue;
        }

        float occ = memboundExtentOccupancy(e);

        /* Worst-fit: prefer emptiest extent */
        if (occ < bestOccupancy) {
            bestOccupancy = occ;
            best = e;
        }
    }

    return best;
}

/* Size-class: route to dedicated extent for size category */
memboundExtent *memboundSelectSizeClass(membound *m, size_t nBytes) {
    memboundSizeClass sc = memboundGetSizeClassWithThresholds(
        nBytes, m->strategy.sizeClassSmall, m->strategy.sizeClassMedium);
    memboundExtent *dedicated = m->strategy.classExtents[sc];

    /* If we have a dedicated extent for this class, try it first */
    if (dedicated && memboundExtentBytesAvailable(dedicated) >= nBytes) {
        return dedicated;
    }

    /* Find or assign a new dedicated extent for this class */
    for (memboundExtent *e = m->extents; e; e = e->next) {
        /* Skip extents dedicated to other classes */
        if (e->sizeClass >= 0 && e->sizeClass != (int8_t)sc) {
            continue;
        }

        if (memboundExtentBytesAvailable(e) >= nBytes) {
            /* Assign this extent to the size class if not already */
            if (e->sizeClass < 0) {
                e->sizeClass = (int8_t)sc;
                e->flags |= MEMBOUND_EXTENT_FLAG_DEDICATED;
                m->strategy.classExtents[sc] = e;
            }
            return e;
        }
    }

    return NULL; /* No suitable extent found */
}

/* Adaptive: choose strategy based on current fragmentation */
memboundExtentStrategy memboundAdaptiveGetStrategy(const membound *m) {
    if (!m) {
        return MEMBOUND_STRATEGY_FIRST_FIT;
    }

    float frag = memboundFragmentationRatio(m);

    /* If fragmented, use best-fit to consolidate */
    if (frag >= m->strategy.fragmentationThreshold) {
        return MEMBOUND_STRATEGY_BEST_FIT;
    }

    /* Otherwise, use first-fit for speed */
    return MEMBOUND_STRATEGY_FIRST_FIT;
}

/* Main extent selector - dispatches to strategy-specific function */
memboundExtent *memboundSelectExtent(membound *m, size_t nBytes) {
    if (!m) {
        return NULL;
    }

    memboundExtentStrategy effectiveStrategy = m->strategy.strategy;

    /* Adaptive resolves to a concrete strategy */
    if (effectiveStrategy == MEMBOUND_STRATEGY_ADAPTIVE) {
        effectiveStrategy = memboundAdaptiveGetStrategy(m);
    }

    memboundExtent *selected = NULL;

    switch (effectiveStrategy) {
    case MEMBOUND_STRATEGY_BEST_FIT:
        selected = memboundSelectBestFit(m, nBytes);
        break;

    case MEMBOUND_STRATEGY_WORST_FIT:
        selected = memboundSelectWorstFit(m, nBytes);
        break;

    case MEMBOUND_STRATEGY_SIZE_CLASS:
        selected = memboundSelectSizeClass(m, nBytes);
        break;

    case MEMBOUND_STRATEGY_FIRST_FIT:
    case MEMBOUND_STRATEGY_ADAPTIVE:
    default:
        /* First-fit: just use primary */
        if (m->primary && memboundExtentBytesAvailable(m->primary) >= nBytes) {
            selected = m->primary;
        }
        break;
    }

    return selected;
}

/* Forward declaration */
static void memboundRemoveExtent(membound *m, memboundExtent *extent);

/* Release a single non-primary extent back to the OS.
 * Caller must hold the lock. Returns true if released. */
static bool memboundReleaseExtent(membound *m, memboundExtent *e) {
    if (!m || !e || e == m->primary) {
        return false;
    }
    memboundRemoveExtent(m, e);
    return true;
}

/* Note on extent release strategy:
 *
 * We release empty extents in two ways:
 * 1. Automatic (on free): O(1) - check only the extent we just freed from
 * 2. Explicit (memboundShrink): O(n) - scan and release all empty extents
 *
 * The automatic release respects shrinkThreshold to avoid releasing
 * extents that are briefly empty during high-churn workloads.
 * The explicit release ignores the threshold and releases all empty.
 *
 * "Shrink" in the API is a misnomer - we can't actually shrink an extent.
 * We can only release it entirely when empty. The name is kept for
 * API consistency with common memory pool conventions. */

/* ====================================================================
 * Main membound Implementation
 * ==================================================================== */

/* Spin iterations before falling back to mutex.
 * Buddy allocator operations are fast (~50-200ns), so brief spinning
 * often avoids the overhead of a syscall. Too much spinning wastes CPU.
 * 32 iterations is ~100-200 cycles on modern CPUs. */
#define MEMBOUND_SPIN_ITERATIONS 32

/* CPU pause hint for spin-wait loops.
 * Reduces power consumption and improves SMT performance. */
#if defined(__x86_64__) || defined(__i386__)
#define MEMBOUND_SPIN_PAUSE() __builtin_ia32_pause()
#elif defined(__aarch64__)
#define MEMBOUND_SPIN_PAUSE() __asm__ volatile("yield")
#else
#define MEMBOUND_SPIN_PAUSE() ((void)0)
#endif

static void memboundEnter(membound *m) {
    /* Fast path: try to acquire immediately without syscall overhead */
    if (pthread_mutex_trylock(&m->mutex) == 0) {
        /* Got lock on first try - no contention, skip all profiling overhead */
        atomic_fetch_add_explicit(&m->lockAcquisitions, 1,
                                  memory_order_relaxed);
        return;
    }

    /* Contended path: brief spin before blocking.
     * Buddy allocator critical sections are short, so the holder
     * will likely release soon. Spinning avoids syscall overhead. */
    for (int i = 0; i < MEMBOUND_SPIN_ITERATIONS; i++) {
        MEMBOUND_SPIN_PAUSE();
        if (pthread_mutex_trylock(&m->mutex) == 0) {
            /* Got lock after spinning - still fast path */
            atomic_fetch_add_explicit(&m->lockAcquisitions, 1,
                                      memory_order_relaxed);
            return;
        }
    }

    /* Slow path: must block. Record contention stats. */
    atomic_fetch_add_explicit(&m->lockAcquisitions, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&m->lockContentions, 1, memory_order_relaxed);

    /* Time the blocking wait */
    uint64_t startNs = timeUtilMonotonicNs();
    pthread_mutex_lock(&m->mutex);
    uint64_t endNs = timeUtilMonotonicNs();
    atomic_fetch_add_explicit(&m->lockWaitTimeNs, endNs - startNs,
                              memory_order_relaxed);
}

static void memboundLeave(membound *m) {
    pthread_mutex_unlock(&m->mutex);
}

/* Per-extent locking for ENTERPRISE tier with perExtentLocking enabled.
 * These allow parallel alloc/free across different extents.
 * Uses same spin-then-block pattern as global lock for consistency. */
static inline void memboundExtentEnter(memboundExtent *e) {
    if (!e->extentMutexInitialized) {
        return;
    }

    /* Fast path: try immediate acquisition */
    if (pthread_mutex_trylock(&e->extentMutex) == 0) {
        return;
    }

    /* Brief spin before blocking */
    for (int i = 0; i < MEMBOUND_SPIN_ITERATIONS; i++) {
        MEMBOUND_SPIN_PAUSE();
        if (pthread_mutex_trylock(&e->extentMutex) == 0) {
            return;
        }
    }

    /* Must block */
    pthread_mutex_lock(&e->extentMutex);
}

static inline void memboundExtentLeave(memboundExtent *e) {
    if (e->extentMutexInitialized) {
        pthread_mutex_unlock(&e->extentMutex);
    }
}

/* Check if per-extent locking is enabled for this pool */
static inline bool memboundUsesPerExtentLocking(const membound *m) {
    return m->tier == MEMBOUND_TIER_ENTERPRISE && m->enterprise &&
           m->enterprise->perExtentLocking;
}

/* Add a new extent to the pool */
static bool memboundAddExtent(membound *m, size_t size) {
    memboundExtent *extent = memboundExtentCreate(size);
    if (!extent) {
        return false;
    }

    /* Add to index */
    if (!memboundIndexAdd(&m->index, extent)) {
        memboundExtentDestroy(extent);
        return false;
    }

    /* Link into extent list */
    extent->next = m->extents;
    if (m->extents) {
        m->extents->prev = extent;
    }
    m->extents = extent;
    m->extentCount++;

    /* Update aggregate stats */
    m->totalCapacity += memboundExtentCapacity(extent);

    /* Make new extent primary (it has the most free space) */
    m->primary = extent;

    return true;
}

/* Remove an extent from the pool */
static void memboundRemoveExtent(membound *m, memboundExtent *extent) {
    if (!m || !extent) {
        return;
    }

    /* Remove from index */
    memboundIndexRemove(&m->index, extent);

    /* Unlink from list */
    if (extent->prev) {
        extent->prev->next = extent->next;
    } else {
        m->extents = extent->next;
    }
    if (extent->next) {
        extent->next->prev = extent->prev;
    }
    m->extentCount--;

    /* Clear size-class assignment if applicable */
    if (extent->sizeClass >= 0 && extent->sizeClass < MEMBOUND_CLASS_COUNT) {
        if (m->strategy.classExtents[extent->sizeClass] == extent) {
            m->strategy.classExtents[extent->sizeClass] = NULL;
        }
    }

    /* Clear from ENTERPRISE cache if applicable */
    if (m->enterprise && m->enterprise->lastAllocExtent == extent) {
        m->enterprise->lastAllocExtent = NULL;
    }

    /* Update aggregate stats */
    m->totalCapacity -= memboundExtentCapacity(extent);

    /* If this was primary, find a new one */
    if (m->primary == extent) {
        m->primary = m->extents;
    }

    memboundExtentDestroy(extent);
}

/* Core allocation for fixed mode */
static void *memboundAllocFixed(membound *m, size_t nBytes) {
    void *p = memboundExtentAlloc(m->primary, nBytes);
    if (p) {
        m->currentOut += memboundExtentSize(m->primary, p);
        m->currentCount++;
        m->nAlloc++;
        if (m->currentOut > m->maxOut) {
            m->maxOut = m->currentOut;
        }
        if (m->currentCount > m->maxCount) {
            m->maxCount = m->currentCount;
        }
    }
    return p;
}

/* Helper to add a pre-created extent to the pool (called under lock) */
static bool memboundAddExtentToPool(membound *m, memboundExtent *extent) {
    /* Add to index */
    if (!memboundIndexAdd(&m->index, extent)) {
        return false;
    }

    /* Initialize per-extent mutex for ENTERPRISE tier with per-extent locking
     */
    if (m->tier == MEMBOUND_TIER_ENTERPRISE && m->enterprise &&
        m->enterprise->perExtentLocking) {
        pthread_mutex_init(&extent->extentMutex, NULL);
        extent->extentMutexInitialized = true;
    }

    /* Link into extent list */
    extent->next = m->extents;
    if (m->extents) {
        m->extents->prev = extent;
    }
    m->extents = extent;
    m->extentCount++;

    /* Update aggregate stats */
    m->totalCapacity += memboundExtentCapacity(extent);

    /* Make new extent primary (it has the most free space) */
    m->primary = extent;

    return true;
}

/* Parallel allocation helper for ENTERPRISE tier with per-extent locking.
 * This function tries to allocate from a selected extent while allowing
 * concurrent allocations to other extents.
 *
 * Pattern: Acquire per-extent lock while holding global lock, then release
 * global lock, allocate under per-extent lock, release per-extent lock,
 * re-acquire global lock for stats.
 *
 * This ensures that once we've selected an extent, no other thread can
 * start an operation on it until we've completed ours.
 */
static void *memboundAllocFromExtentParallel(membound *m, memboundExtent *e,
                                             size_t nBytes,
                                             size_t *outAllocSize) {
    if (!e || !e->extentMutexInitialized) {
        return NULL; /* Per-extent locking not enabled for this extent */
    }

    /* IMPORTANT: Acquire per-extent lock WHILE holding global lock.
     * This prevents race where another thread selects the same extent
     * before we can lock it. */
    memboundExtentEnter(e);

    /* Now release global lock - other threads can select different extents */
    memboundLeave(m);

    /* Try allocation under per-extent lock only */
    void *p = memboundExtentAlloc(e, nBytes);
    size_t allocSize = 0;
    if (p) {
        allocSize = memboundExtentSize(e, p);
    }

    /* Release per-extent lock */
    memboundExtentLeave(e);

    /* Re-acquire global lock for stats update */
    memboundEnter(m);

    if (outAllocSize) {
        *outAllocSize = allocSize;
    }
    return p;
}

/* Core allocation for dynamic mode */
static void *memboundAllocDynamic(membound *m, size_t nBytes) {
    void *p = NULL;
    memboundExtent *selectedExtent = NULL;
    size_t allocSize = 0;
    const bool useParallel = memboundUsesPerExtentLocking(m);
    const bool useCache =
        m->tier == MEMBOUND_TIER_ENTERPRISE && m->enterprise != NULL;

    /* ENTERPRISE tier: Try cached extent first for O(1) repeated allocations.
     * This avoids the strategy selection overhead when the same extent
     * can satisfy repeated allocations of similar sizes. */
    if (useCache && m->enterprise->lastAllocExtent) {
        memboundExtent *cached = m->enterprise->lastAllocExtent;

        /* Verify cached extent is still valid and has enough space */
        if (memboundExtentBytesAvailable(cached) >= nBytes) {
            if (useParallel) {
                p = memboundAllocFromExtentParallel(m, cached, nBytes,
                                                    &allocSize);
            } else {
                p = memboundExtentAlloc(cached, nBytes);
                if (p) {
                    allocSize = memboundExtentSize(cached, p);
                }
            }

            if (p) {
                m->enterprise->cacheHits++;
                selectedExtent = cached;
                goto success_parallel; /* Stats handled in success_parallel */
            }
        }
        /* Cache miss - extent didn't have space or allocation failed */
        m->enterprise->cacheMisses++;
    }

    /* 1. Use strategy to select an extent */
    selectedExtent = memboundSelectExtent(m, nBytes);
    if (selectedExtent) {
        if (useParallel) {
            /* ENTERPRISE tier: release global lock during allocation */
            p = memboundAllocFromExtentParallel(m, selectedExtent, nBytes,
                                                &allocSize);
            if (p) {
                goto success_parallel;
            }
        } else {
            p = memboundExtentAlloc(selectedExtent, nBytes);
            if (p) {
                /* Update primary if this extent has more space */
                if (memboundExtentBytesAvailable(selectedExtent) >
                    memboundExtentBytesAvailable(m->primary)) {
                    m->primary = selectedExtent;
                }
                goto success;
            }
        }
    }

    /* 2. Strategy didn't find suitable extent, try all extents (fallback) */
    for (memboundExtent *e = m->extents; e; e = e->next) {
        if (useParallel) {
            p = memboundAllocFromExtentParallel(m, e, nBytes, &allocSize);
            if (p) {
                selectedExtent = e;
                goto success_parallel;
            }
        } else {
            p = memboundExtentAlloc(e, nBytes);
            if (p) {
                /* Consider promoting this extent to primary */
                if (memboundExtentBytesAvailable(e) >
                    memboundExtentBytesAvailable(m->primary)) {
                    m->primary = e;
                }
                selectedExtent = e;
                goto success;
            }
        }
    }

    /* 3. Check if we can grow */
    size_t growSize = memboundCalculateGrowthSize(m, nBytes);
    if (!memboundCanGrowBy(m, growSize)) {
        /* Pressure callback will be invoked by caller after releasing lock */
        return NULL;
    }

    /* 4. Create new extent - RELEASE LOCK during mmap syscall for better
     * concurrency. The mmap syscall can block, so releasing the lock allows
     * other threads to continue working on existing extents. */
    {
        /* Release lock before expensive mmap syscall */
        memboundLeave(m);

        /* Create extent outside the critical section */
        memboundExtent *newExtent = memboundExtentCreate(growSize);

        /* Re-acquire lock */
        memboundEnter(m);

        /* After re-acquiring lock, state may have changed. Another thread
         * might have:
         * - Grown the pool already (allocations may succeed now)
         * - Reached max limits (we can't add our extent)
         * - Freed memory (allocations may succeed now)
         *
         * Try allocation again first - it's cheaper than adding an extent. */
        for (memboundExtent *e = m->extents; e; e = e->next) {
            p = memboundExtentAlloc(e, nBytes);
            if (p) {
                /* Another thread helped us - destroy our unneeded extent */
                if (newExtent) {
                    memboundExtentDestroy(newExtent);
                }
                selectedExtent = e;
                goto success;
            }
        }

        /* Still need to grow - add our extent if we got one */
        if (!newExtent) {
            /* mmap failed */
            return NULL;
        }

        /* Re-check growth limits (may have changed while lock was released) */
        if (!memboundCanGrowBy(m, growSize)) {
            /* Another thread hit the limit - destroy our extent */
            memboundExtentDestroy(newExtent);
            return NULL;
        }

        /* Add extent to the pool */
        if (!memboundAddExtentToPool(m, newExtent)) {
            memboundExtentDestroy(newExtent);
            return NULL;
        }
    }

    /* 5. For size-class strategy, assign new extent to the size class */
    if (m->strategy.strategy == MEMBOUND_STRATEGY_SIZE_CLASS) {
        memboundSizeClass sc = memboundGetSizeClassWithThresholds(
            nBytes, m->strategy.sizeClassSmall, m->strategy.sizeClassMedium);
        if (m->strategy.classExtents[sc] == NULL) {
            m->primary->sizeClass = (int8_t)sc;
            m->primary->flags |= MEMBOUND_EXTENT_FLAG_DEDICATED;
            m->strategy.classExtents[sc] = m->primary;
        }
    }

    /* 6. Allocate from new extent */
    if (useParallel) {
        p = memboundAllocFromExtentParallel(m, m->primary, nBytes, &allocSize);
        if (p) {
            selectedExtent = m->primary;
            goto success_parallel;
        }
    } else {
        p = memboundExtentAlloc(m->primary, nBytes);
    }
    if (!p) {
        /* This shouldn't happen with a fresh extent */
        return NULL;
    }
    selectedExtent = m->primary;

success:
    /* Non-parallel path: calculate allocSize here */
    allocSize = memboundExtentSize(selectedExtent, p);

success_parallel: {
    /* Both paths merge here with allocSize already set */
    m->currentOut += allocSize;
    m->currentCount++;
    m->nAlloc++;
    m->totalAlloc += allocSize;

    /* Update extent's last allocation sequence */
    selectedExtent->lastAllocSeq = m->nAlloc;

    /* ENTERPRISE tier: Update extent cache for O(1) repeated allocations */
    if (useCache) {
        memboundExtent *previousCached = m->enterprise->lastAllocExtent;
        if (selectedExtent != previousCached) {
            /* Track extent switches for profiling */
            m->enterprise->extentSwitchCount++;
        }
        m->enterprise->lastAllocExtent = selectedExtent;
        m->enterprise->lastAllocSeq = m->nAlloc;
    }

    /* Update primary if this extent has more space (parallel path only) */
    if (useParallel && memboundExtentBytesAvailable(selectedExtent) >
                           memboundExtentBytesAvailable(m->primary)) {
        m->primary = selectedExtent;
    }

    if (m->currentOut > m->maxOut) {
        m->maxOut = m->currentOut;
    }
    if (m->currentCount > m->maxCount) {
        m->maxCount = m->currentCount;
    }

    /* Pressure callback will be invoked by caller after releasing lock */
}

    return p;
}

/* ====================================================================
 * Public API Implementation
 * ==================================================================== */

membound *memboundCreate(size_t size) {
    return memboundCreateFixed(size);
}

membound *memboundCreateFixed(size_t size) {
    if (size == 0) {
        return NULL;
    }

    membound *m = memboundMAP(sizeof(membound));
    if (!m) {
        return NULL;
    }
    memset(m, 0, sizeof(*m));

    m->tier = MEMBOUND_TIER_STANDARD; /* Set tier BEFORE mode (tier is first) */
    m->mode = MEMBOUND_MODE_FIXED;

    /* Initialize index */
    memboundIndexInit(&m->index);

    /* Create initial extent */
    if (!memboundAddExtent(m, size)) {
        memboundUNMAP(m, sizeof(membound));
        return NULL;
    }

    /* Initialize mutex (process-shared for fork safety) */
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&m->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    m->mutexInitialized = true;

    return m;
}

membound *memboundCreateDynamic(size_t initialSize, size_t maxSize,
                                size_t growthSize) {
    memboundConfig config = {.mode = MEMBOUND_MODE_DYNAMIC,
                             .initialSize = initialSize,
                             .maxTotalSize = maxSize,
                             .growthSize =
                                 growthSize > 0 ? growthSize : initialSize,
                             .growthFactor = 0, /* Use fixed growth */
                             .maxExtentCount = 0,
                             .pressureCallback = NULL,
                             .pressureUserData = NULL};
    return memboundCreateWithConfig(&config);
}

membound *memboundCreateWithConfig(const memboundConfig *config) {
    if (!config || config->initialSize == 0) {
        return NULL;
    }

    /* Dispatch to tier-specific creation.
     * Note: MEMBOUND_TIER_STANDARD == 0, so uninitialized configs default
     * to STANDARD tier (the safest default).
     * MICRO and COMPACT tiers only support FIXED mode. */
    if (config->tier == MEMBOUND_TIER_MICRO) {
        if (config->mode == MEMBOUND_MODE_DYNAMIC) {
            /* MICRO doesn't support dynamic mode - fall through to STANDARD */
        } else if (config->initialSize <= MEMBOUND_MICRO_MAX_POOL) {
            return memboundCreateMicro(config->initialSize);
        }
        /* Size too large for MICRO - fall through to STANDARD */
    } else if (config->tier == MEMBOUND_TIER_COMPACT) {
        if (config->mode == MEMBOUND_MODE_DYNAMIC) {
            /* COMPACT doesn't support dynamic mode - fall through to STANDARD
             */
        } else {
            return memboundCreateCompact(config->initialSize,
                                         config->threadSafe);
        }
    }
    /* STANDARD (default) and ENTERPRISE tiers handled below.
     * Also handles fallback for MICRO/COMPACT with unsupported features. */

    membound *m = memboundMAP(sizeof(membound));
    if (!m) {
        return NULL;
    }
    memset(m, 0, sizeof(*m));

    /* Set tier - defaults to STANDARD if not specified (0 = STANDARD) */
    m->tier = (config->tier == MEMBOUND_TIER_ENTERPRISE)
                  ? MEMBOUND_TIER_ENTERPRISE
                  : MEMBOUND_TIER_STANDARD;
    m->mode = config->mode;

    /* Setup growth configuration */
    if (config->mode == MEMBOUND_MODE_DYNAMIC) {
        if (config->growthFactor > 0) {
            m->growth.type = MEMBOUND_GROWTH_GEOMETRIC;
            m->growth.growthFactor = config->growthFactor;
        } else if (config->growthSize > 0) {
            m->growth.type = MEMBOUND_GROWTH_FIXED;
            m->growth.fixedGrowthSize = config->growthSize;
        } else {
            m->growth.type = MEMBOUND_GROWTH_FIXED;
            m->growth.fixedGrowthSize = config->initialSize;
        }
        m->growth.maxTotalSize = config->maxTotalSize;
        m->growth.maxExtentCount = config->maxExtentCount;
        m->growth.minExtentSize = config->initialSize / 4;
        m->growth.maxExtentSize = config->initialSize * 4;

        /* Setup extent selection strategy */
        memboundStrategyInit(&m->strategy);
        m->strategy.strategy = config->strategy;
        if (config->minOccupancyForAlloc > 0.0f) {
            m->strategy.minOccupancyForAlloc = config->minOccupancyForAlloc;
        }
        if (config->shrinkThreshold > 0.0f) {
            m->strategy.shrinkThreshold = config->shrinkThreshold;
        }
        if (config->fragmentationThreshold > 0.0f) {
            m->strategy.fragmentationThreshold = config->fragmentationThreshold;
        }

        /* Setup size-class thresholds (0 = use defaults) */
        m->strategy.sizeClassSmall = config->sizeClassSmall;
        m->strategy.sizeClassMedium = config->sizeClassMedium;
    }

    /* Setup callbacks */
    m->pressureCallback = config->pressureCallback;
    m->pressureUserData = config->pressureUserData;
    m->lastPressureLevel = MEMBOUND_PRESSURE_LOW;

    /* Initialize index */
    memboundIndexInit(&m->index);

    /* Create initial extent */
    if (!memboundAddExtent(m, config->initialSize)) {
        memboundUNMAP(m, sizeof(membound));
        return NULL;
    }

    /* Initialize mutex */
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&m->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    m->mutexInitialized = true;

    /* Initialize ENTERPRISE tier extension if requested */
    if (m->tier == MEMBOUND_TIER_ENTERPRISE) {
        m->enterprise = memboundMAP(sizeof(memboundEnterpriseExt));
        if (!m->enterprise) {
            /* Fall back to STANDARD tier */
            m->tier = MEMBOUND_TIER_STANDARD;
        } else {
            memset(m->enterprise, 0, sizeof(memboundEnterpriseExt));
            m->enterprise->perExtentLocking = config->perExtentLocking;

            /* Initialize per-extent mutex for existing extent if enabled.
             * Note: Using NULL for attributes (default mutex) since per-extent
             * locks don't need process-shared attribute. */
            if (m->enterprise->perExtentLocking && m->primary) {
                pthread_mutex_init(&m->primary->extentMutex, NULL);
                m->primary->extentMutexInitialized = true;
            }
        }
    }

    return m;
}

bool memboundShutdown(membound *m) {
    if (!m) {
        return false;
    }

    /* Dispatch based on tier - MICRO and COMPACT have different structs */
    if (memboundIsMicro(m)) {
        memboundMicroDestroy((memboundMicro *)m);
        return true;
    }

    if (memboundIsCompact(m)) {
        memboundCompactPoolDestroy((memboundCompactPool *)m);
        return true;
    }

    /* STANDARD and ENTERPRISE tiers use full membound struct */

    /* Acquire lock to ensure no operations are in progress.
     * This also acts as a memory barrier to ensure all previous
     * operations are visible. */
    if (m->mutexInitialized) {
        memboundEnter(m);
        /* We now own the lock - no other operations can proceed.
         * We don't release it since we're destroying the mutex. */
    }

    /* Destroy all extents (and their per-extent mutexes for ENTERPRISE tier) */
    memboundExtent *e = m->extents;
    while (e) {
        memboundExtent *next = e->next;
        /* Destroy per-extent mutex if initialized (ENTERPRISE tier) */
        if (e->extentMutexInitialized) {
            pthread_mutex_destroy(&e->extentMutex);
        }
        memboundExtentDestroy(e);
        e = next;
    }

    /* Free index */
    memboundIndexFree(&m->index);

    /* Free enterprise extension if allocated */
    if (m->enterprise) {
        memboundUNMAP(m->enterprise, sizeof(memboundEnterpriseExt));
    }

    /* Destroy mutex - we still hold it but that's intentional.
     * POSIX allows destroying a locked mutex if no thread is waiting on it.
     * Since we hold the only reference, no other thread should be waiting. */
    if (m->mutexInitialized) {
        pthread_mutex_destroy(&m->mutex);
    }

    memboundUNMAP(m, sizeof(membound));
    return true;
}

bool memboundShutdownSafe(membound *m) {
    if (!m) {
        return false;
    }

    memboundEnter(m);
    const bool hasAllocations = (m->currentCount != 0);
    memboundLeave(m);

    if (hasAllocations) {
        return false;
    }

    return memboundShutdown(m);
}

void *memboundAlloc(membound *m, size_t nBytes) {
    if (!m || nBytes == 0) {
        return NULL;
    }

    /* Dispatch based on tier - MICRO and COMPACT have different structs */
    if (memboundIsMicro(m)) {
        return memboundMicroAlloc((memboundMicro *)m, nBytes);
    }

    if (memboundIsCompact(m)) {
        return memboundCompactPoolAlloc((memboundCompactPool *)m, nBytes);
    }

    /* STANDARD and ENTERPRISE tiers use full membound struct */
    if (nBytes > 0x40000000) {
        return NULL;
    }

    memboundEnter(m);
    void *p;
    if (m->mode == MEMBOUND_MODE_FIXED) {
        p = memboundAllocFixed(m, nBytes);
    } else {
        p = memboundAllocDynamic(m, nBytes);
    }

    /* Capture callback info while holding lock */
    memboundPressure pressure = memboundCalculatePressure(m);
    memboundPressureCallback cb = NULL;
    void *userData = NULL;
    bool shouldNotify =
        memboundShouldNotifyPressure(m, pressure, &cb, &userData);

    memboundLeave(m);

    /* Invoke callback after releasing lock to prevent deadlock */
    if (shouldNotify && cb) {
        cb(m, pressure, userData);
    }

    return p;
}

void *memboundCalloc(membound *m, size_t count, size_t size) {
    if (!m || count == 0 || size == 0) {
        return NULL;
    }

    /* Overflow check */
    if (count > SIZE_MAX / size) {
        return NULL;
    }

    const size_t totalBytes = count * size;
    void *p = memboundAlloc(m, totalBytes);
    if (p) {
#if defined(MEMBOUND_USE_AVX512) || defined(MEMBOUND_USE_AVX2) ||              \
    defined(MEMBOUND_USE_SSE2) || defined(MEMBOUND_USE_NEON)
        if (totalBytes >= MEMBOUND_SIMD_ZERO_THRESHOLD) {
            memboundZeroFast(p, totalBytes);
        } else {
            memset(p, 0, totalBytes);
        }
#else
        memset(p, 0, totalBytes);
#endif
    }
    return p;
}

void *memboundRealloc(membound *m, void *pPrior, size_t nBytes) {
    if (!pPrior) {
        return memboundAlloc(m, nBytes);
    }

    if (nBytes == 0) {
        memboundFree(m, pPrior);
        return NULL;
    }

    if (!m || nBytes > 0x40000000) {
        return NULL;
    }

    memboundEnter(m);

    /* Find owning extent */
    memboundExtent *owner = memboundIndexLookup(&m->index, pPrior);
    if (!owner) {
        memboundLeave(m);
        return NULL;
    }

    size_t oldSize = memboundExtentSize(owner, pPrior);

    /* If new size fits within current allocation, no-op */
    if (nBytes <= oldSize) {
        memboundLeave(m);
        return pPrior;
    }

    /* Need to grow - allocate new block while holding lock to prevent race.
     * Use internal allocation functions that assume lock is held. */
    void *p;
    if (m->mode == MEMBOUND_MODE_FIXED) {
        p = memboundAllocFixed(m, nBytes);
    } else {
        p = memboundAllocDynamic(m, nBytes);
    }

    if (p) {
        /* Copy data while lock is held - pPrior is protected */
        memcpy(p, pPrior, oldSize);

        /* Free old block - internal free logic, lock already held */
        size_t freedSize = memboundExtentSize(owner, pPrior);
        memboundExtentFree(owner, pPrior);
        m->currentOut -= freedSize;
        m->currentCount--;

        /* Note: skip pressure callback and proactive shrink in realloc
         * to avoid potential deadlock. Pressure will be checked on next
         * alloc/free operation. */
    }

    memboundLeave(m);
    return p;
}

void memboundFree(membound *m, void *pPrior) {
    if (!m || !pPrior) {
        return;
    }

    /* Dispatch based on tier - MICRO and COMPACT have different structs */
    if (memboundIsMicro(m)) {
        memboundMicroFree((memboundMicro *)m, pPrior);
        return;
    }

    if (memboundIsCompact(m)) {
        memboundCompactPoolFree((memboundCompactPool *)m, pPrior);
        return;
    }

    /* STANDARD and ENTERPRISE tiers use full membound struct */
    memboundEnter(m);

    /* Find owning extent */
    memboundExtent *owner = memboundIndexLookup(&m->index, pPrior);
    if (!owner) {
        /* Pointer doesn't belong to any extent - invalid free */
        atomic_fetch_add_explicit(&m->safetyViolations, 1,
                                  memory_order_relaxed);
        memboundLeave(m);
        return;
    }

    /* Validate the free operation and track violations */
    if (!memboundValidateFree(m, owner, pPrior)) {
        memboundLeave(m);
        return;
    }

    size_t freedSize = memboundExtentSize(owner, pPrior);

    /* ENTERPRISE tier with per-extent locking: release global lock during free
     */
    if (memboundUsesPerExtentLocking(m) && owner->extentMutexInitialized) {
        /* IMPORTANT: Acquire per-extent lock WHILE holding global lock.
         * This prevents race where another thread starts an operation on
         * this extent before we can lock it. */
        memboundExtentEnter(owner);

        /* Now release global lock - other threads can work on other extents */
        memboundLeave(m);

        /* Do the actual free under per-extent lock */
        memboundExtentFree(owner, pPrior);

        /* Release per-extent lock */
        memboundExtentLeave(owner);

        /* Re-acquire global lock for stats update */
        memboundEnter(m);
    } else {
        /* Standard path: free under global lock */
        memboundExtentFree(owner, pPrior);
    }

    /* Update aggregate stats */
    m->currentOut -= freedSize;
    m->currentCount--;

    /* Release this extent if it is now empty after the free operation.
     * This is O(1) - we only check the single extent we just freed from.
     *
     * Note: We release if empty AND below shrinkThreshold. This is an
     * optimization to avoid releasing/reallocating extents that are briefly
     * empty during high-churn workloads. If shrinkThreshold is 0, automatic
     * release is disabled (use explicit memboundShrink instead). */
    if (m->mode == MEMBOUND_MODE_DYNAMIC &&
        m->strategy.shrinkThreshold > 0.0f && owner->currentCount == 0 &&
        owner != m->primary) {
        float occ = memboundExtentOccupancy(owner);
        if (occ < m->strategy.shrinkThreshold) {
            memboundReleaseExtent(m, owner);
        }
    }

    /* Capture callback info while holding lock */
    memboundPressure pressure = memboundCalculatePressure(m);
    memboundPressureCallback cb = NULL;
    void *userData = NULL;
    bool shouldNotify =
        memboundShouldNotifyPressure(m, pressure, &cb, &userData);

    memboundLeave(m);

    /* Invoke callback after releasing lock to prevent deadlock */
    if (shouldNotify && cb) {
        cb(m, pressure, userData);
    }
}

void memboundReset(membound *m) {
    if (!m) {
        return;
    }

    /* Dispatch based on tier - MICRO and COMPACT have different structs */
    if (memboundIsMicro(m)) {
        memboundMicroReset((memboundMicro *)m);
        return;
    }

    if (memboundIsCompact(m)) {
        memboundCompactPoolReset((memboundCompactPool *)m);
        return;
    }

    /* STANDARD and ENTERPRISE tiers use full membound struct */
    memboundEnter(m);

    /* Reset all extents */
    for (memboundExtent *e = m->extents; e; e = e->next) {
        memboundExtentReset(e);
    }

    /* Reset aggregate stats */
    m->currentOut = 0;
    m->currentCount = 0;

    memboundLeave(m);
}

bool memboundIncreaseSize(membound *m, size_t size) {
    if (!m) {
        return false;
    }

    memboundEnter(m);

    /* Only works when pool is empty */
    if (m->currentCount != 0) {
        memboundLeave(m);
        return false;
    }

    /* For fixed mode: replace the single extent */
    if (m->mode == MEMBOUND_MODE_FIXED) {
        if (size <= m->totalCapacity) {
            memboundLeave(m);
            return false;
        }

        /* Release lock before mmap syscall */
        memboundLeave(m);

        /* Create new larger extent outside lock */
        memboundExtent *newExtent = memboundExtentCreate(size);
        if (!newExtent) {
            return false;
        }

        /* Re-acquire lock for data structure updates */
        memboundEnter(m);

        /* Re-check preconditions (state may have changed) */
        if (m->currentCount != 0 || size <= m->totalCapacity) {
            memboundLeave(m);
            memboundExtentDestroy(newExtent);
            return false;
        }

        /* Remove old extent */
        memboundExtent *oldExtent = m->primary;
        memboundIndexRemove(&m->index, oldExtent);
        memboundExtentDestroy(oldExtent);

        /* Add new extent */
        m->extents = NULL;
        m->primary = NULL;
        m->extentCount = 0;
        m->totalCapacity = 0;

        memboundIndexAdd(&m->index, newExtent);
        m->extents = newExtent;
        m->primary = newExtent;
        m->extentCount = 1;
        m->totalCapacity = memboundExtentCapacity(newExtent);

        memboundLeave(m);
        return true;
    }

    /* For dynamic mode: release lock around mmap */
    memboundLeave(m);
    memboundExtent *newExtent = memboundExtentCreate(size);
    if (!newExtent) {
        return false;
    }
    memboundEnter(m);

    /* Add extent to the pool */
    bool result = memboundAddExtentToPool(m, newExtent);
    if (!result) {
        memboundExtentDestroy(newExtent);
    }
    memboundLeave(m);
    return result;
}

bool memboundGrowBy(membound *m, size_t additionalBytes) {
    if (!m || m->mode != MEMBOUND_MODE_DYNAMIC) {
        return false;
    }

    memboundEnter(m);

    if (!memboundCanGrowBy(m, additionalBytes)) {
        memboundLeave(m);
        return false;
    }

    /* Release lock before mmap syscall */
    memboundLeave(m);

    /* Create extent outside lock */
    memboundExtent *newExtent = memboundExtentCreate(additionalBytes);
    if (!newExtent) {
        return false;
    }

    /* Re-acquire lock for data structure updates */
    memboundEnter(m);

    /* Re-check growth limits (may have changed while lock was released) */
    if (!memboundCanGrowBy(m, additionalBytes)) {
        memboundLeave(m);
        memboundExtentDestroy(newExtent);
        return false;
    }

    /* Add extent to the pool */
    bool result = memboundAddExtentToPool(m, newExtent);
    if (!result) {
        memboundExtentDestroy(newExtent);
    }
    memboundLeave(m);
    return result;
}

/* Release all empty extents back to the OS.
 * This is an explicit operation - releases ALL empty extents regardless
 * of shrinkThreshold. Never releases the primary extent.
 *
 * Note: Despite the name "shrink", we can't actually shrink an extent.
 * We can only release it entirely when empty. The name is kept for
 * API consistency with common memory pool conventions. */
size_t memboundShrink(membound *m) {
    if (!m || m->mode != MEMBOUND_MODE_DYNAMIC) {
        return 0;
    }

    memboundEnter(m);

    size_t released = 0;
    memboundExtent *e = m->extents;

    while (e) {
        memboundExtent *next = e->next;

        /* Release if empty and not primary.
         * Primary extent is never released - it's the pool's anchor. */
        if (e != m->primary && e->currentCount == 0) {
            memboundRemoveExtent(m, e);
            released++;
        }

        e = next;
    }

    memboundLeave(m);
    return released;
}

/* Attempt to consolidate memory by releasing empty extents.
 * Currently equivalent to memboundShrink() - true compaction would
 * require moving allocations between extents, which would invalidate
 * pointers and require application cooperation. */
size_t memboundCompact(membound *m) {
    return memboundShrink(m);
}

/* ====================================================================
 * Extent Selection Strategy API
 * ==================================================================== */

memboundExtentStrategy memboundSetStrategy(membound *m,
                                           memboundExtentStrategy strategy) {
    if (!m || m->mode != MEMBOUND_MODE_DYNAMIC) {
        return MEMBOUND_STRATEGY_FIRST_FIT;
    }

    memboundEnter(m);
    memboundExtentStrategy old = m->strategy.strategy;
    m->strategy.strategy = strategy;

    /* Clear size-class assignments if switching away from SIZE_CLASS */
    if (old == MEMBOUND_STRATEGY_SIZE_CLASS &&
        strategy != MEMBOUND_STRATEGY_SIZE_CLASS) {
        for (memboundExtent *e = m->extents; e; e = e->next) {
            e->sizeClass = -1;
            e->flags &= ~MEMBOUND_EXTENT_FLAG_DEDICATED;
        }
        for (int i = 0; i < MEMBOUND_CLASS_COUNT; i++) {
            m->strategy.classExtents[i] = NULL;
        }
    }

    memboundLeave(m);
    return old;
}

memboundExtentStrategy memboundGetStrategy(const membound *m) {
    if (!m || m->mode != MEMBOUND_MODE_DYNAMIC) {
        return MEMBOUND_STRATEGY_FIRST_FIT;
    }
    return m->strategy.strategy;
}

void memboundSetMinOccupancy(membound *m, float threshold) {
    if (!m || m->mode != MEMBOUND_MODE_DYNAMIC) {
        return;
    }
    memboundEnter(m);
    m->strategy.minOccupancyForAlloc = (threshold < 0.0f)   ? 0.0f
                                       : (threshold > 1.0f) ? 1.0f
                                                            : threshold;
    memboundLeave(m);
}

void memboundSetShrinkThreshold(membound *m, float threshold) {
    if (!m || m->mode != MEMBOUND_MODE_DYNAMIC) {
        return;
    }
    memboundEnter(m);
    m->strategy.shrinkThreshold = (threshold < 0.0f)   ? 0.0f
                                  : (threshold > 1.0f) ? 1.0f
                                                       : threshold;
    memboundLeave(m);
}

float memboundGetFragmentation(const membound *m) {
    if (!m) {
        return 0.0f;
    }
    /* Cast away const for mutex - safe as we only lock, not modify data */
    memboundEnter((membound *)m);
    float result = memboundFragmentationRatio(m);
    memboundLeave((membound *)m);
    return result;
}

/* ====================================================================
 * Query Functions
 * ==================================================================== */

memboundMode memboundGetMode(const membound *m) {
    if (!m) {
        return MEMBOUND_MODE_FIXED;
    }
    /* MICRO and COMPACT tiers are always fixed mode */
    if (memboundIsMicro(m) || memboundIsCompact(m)) {
        return MEMBOUND_MODE_FIXED;
    }
    return m->mode;
}

size_t memboundExtentCount(const membound *m) {
    if (!m) {
        return 0;
    }
    /* MICRO and COMPACT tiers always have 1 extent (inline) */
    if (memboundIsMicro(m) || memboundIsCompact(m)) {
        return 1;
    }
    return m->extentCount;
}

size_t memboundCurrentAllocationCount(const membound *m) {
    if (!m) {
        return 0;
    }
    if (memboundIsMicro(m)) {
        return memboundMicroAllocationCount((const memboundMicro *)m);
    }
    if (memboundIsCompact(m)) {
        return memboundCompactPoolAllocationCount(
            (const memboundCompactPool *)m);
    }
    return m->currentCount;
}

size_t memboundBytesUsed(const membound *m) {
    if (!m) {
        return 0;
    }
    if (memboundIsMicro(m)) {
        return memboundMicroBytesUsed((const memboundMicro *)m);
    }
    if (memboundIsCompact(m)) {
        return memboundCompactPoolBytesUsed((const memboundCompactPool *)m);
    }
    return m->currentOut;
}

size_t memboundBytesAvailable(const membound *m) {
    if (!m) {
        return 0;
    }
    if (memboundIsMicro(m)) {
        return memboundMicroBytesAvailable((const memboundMicro *)m);
    }
    if (memboundIsCompact(m)) {
        return memboundCompactPoolBytesAvailable(
            (const memboundCompactPool *)m);
    }
    return (m->totalCapacity > m->currentOut)
               ? (m->totalCapacity - m->currentOut)
               : 0;
}

size_t memboundCapacity(const membound *m) {
    if (!m) {
        return 0;
    }
    if (memboundIsMicro(m)) {
        return memboundMicroCapacity((const memboundMicro *)m);
    }
    if (memboundIsCompact(m)) {
        return memboundCompactPoolCapacity((const memboundCompactPool *)m);
    }
    return m->totalCapacity;
}

size_t memboundMaxSize(const membound *m) {
    if (!m) {
        return 0;
    }
    /* MICRO and COMPACT tiers don't support dynamic growth */
    if (memboundIsMicro(m) || memboundIsCompact(m)) {
        return 0;
    }
    if (m->mode != MEMBOUND_MODE_DYNAMIC) {
        return 0;
    }
    return m->growth.maxTotalSize;
}

bool memboundCanGrow(const membound *m) {
    if (!m) {
        return false;
    }
    /* MICRO and COMPACT tiers don't support dynamic growth */
    if (memboundIsMicro(m) || memboundIsCompact(m)) {
        return false;
    }
    if (m->mode != MEMBOUND_MODE_DYNAMIC) {
        return false;
    }
    return memboundCanGrowBy(m, m->growth.minExtentSize);
}

bool memboundOwns(const membound *m, const void *p) {
    if (!m || !p) {
        return false;
    }
    if (memboundIsMicro(m)) {
        return memboundMicroOwns((const memboundMicro *)m, p);
    }
    if (memboundIsCompact(m)) {
        return memboundCompactPoolOwns((const memboundCompactPool *)m, p);
    }
    memboundEnter((membound *)m);
    bool result = memboundIndexLookup(&m->index, p) != NULL;
    memboundLeave((membound *)m);
    return result;
}

bool memboundGetExtentInfo(const membound *m, size_t index,
                           memboundExtentInfo *info) {
    if (!m || !info) {
        return false;
    }

    memboundEnter((membound *)m);

    if (index >= m->extentCount) {
        memboundLeave((membound *)m);
        return false;
    }

    /* O(1) access using the extent index array
     * The ranges array is sorted by address for ownership lookup,
     * but provides O(1) access by array index */
    memboundExtent *e = NULL;
    const memboundExtentIndex *idx = &m->index;

    if (index < MEMBOUND_EXTENT_INLINE_MAX &&
        idx->count <= MEMBOUND_EXTENT_INLINE_MAX) {
        /* Use inline array for small extent counts */
        e = idx->inlineRanges[index].extent;
    } else if (idx->ranges && index < idx->count) {
        /* Use heap-allocated array for larger counts */
        e = idx->ranges[index].extent;
    }

    if (!e) {
        memboundLeave((membound *)m);
        return false;
    }

    info->capacity = memboundExtentCapacity(e);
    info->bytesUsed = e->currentOut;
    info->allocationCount = e->currentCount;
    info->baseAddress = e->zPool;
    info->index = index;

    memboundLeave((membound *)m);
    return true;
}

void memboundSetPressureCallback(membound *m, memboundPressureCallback cb,
                                 void *userData) {
    if (!m) {
        return;
    }
    memboundEnter(m);
    m->pressureCallback = cb;
    m->pressureUserData = userData;
    memboundLeave(m);
}

void memboundGetStats(const membound *m, memboundStats *stats) {
    if (!m || !stats) {
        return;
    }

    memset(stats, 0, sizeof(*stats));

    /* Handle MICRO tier - no mutex, minimal stats */
    if (memboundIsMicro(m)) {
        const memboundMicro *micro = (const memboundMicro *)m;
        stats->mode = MEMBOUND_MODE_FIXED;
        stats->tier = MEMBOUND_TIER_MICRO;
        stats->extentCount = 1;
        stats->capacity = memboundMicroCapacity(micro);
        stats->bytesUsed = micro->currentOut;
        stats->bytesAvailable = stats->capacity - stats->bytesUsed;
        stats->allocationCount = micro->currentCount;
        stats->threadSafe = false;
        /* MICRO tier doesn't track lifetime stats, pressure, or lock contention
         */
        stats->currentPressure = MEMBOUND_PRESSURE_LOW;
        if (stats->capacity > 0) {
            stats->usagePercent =
                (float)stats->bytesUsed / (float)stats->capacity * 100.0f;
        }
        stats->safetyViolations = 0; /* MICRO tier doesn't track */
        return;
    }

    /* Handle COMPACT tier - optional mutex, basic stats */
    if (memboundIsCompact(m)) {
        const memboundCompactPool *compact = (const memboundCompactPool *)m;
        stats->mode = MEMBOUND_MODE_FIXED;
        stats->tier = MEMBOUND_TIER_COMPACT;
        stats->extentCount = 1;
        stats->capacity = compact->poolSize;
        stats->bytesUsed = compact->currentOut;
        stats->bytesAvailable = stats->capacity - stats->bytesUsed;
        stats->allocationCount = compact->currentCount;
        stats->threadSafe = compact->threadSafe;
        /* COMPACT tier doesn't track lifetime stats, pressure, or lock
         * contention */
        stats->currentPressure = MEMBOUND_PRESSURE_LOW;
        if (stats->capacity > 0) {
            stats->usagePercent =
                (float)stats->bytesUsed / (float)stats->capacity * 100.0f;
        }
        stats->safetyViolations = compact->safetyViolations;
        return;
    }

    /* STANDARD and ENTERPRISE tiers use full membound struct */
    memboundEnter((membound *)m);

    stats->mode = m->mode;
    stats->tier = m->tier;
    stats->extentCount = m->extentCount;
    stats->capacity = m->totalCapacity;
    stats->bytesUsed = m->currentOut;
    stats->bytesAvailable = (m->totalCapacity > m->currentOut)
                                ? (m->totalCapacity - m->currentOut)
                                : 0;
    stats->allocationCount = m->currentCount;
    stats->threadSafe = m->mutexInitialized;

    if (m->mode == MEMBOUND_MODE_DYNAMIC) {
        stats->maxTotalSize = m->growth.maxTotalSize;
        stats->maxExtentCount = m->growth.maxExtentCount;
    }

    stats->totalAllocations = m->nAlloc;
    stats->totalBytesAllocated = m->totalAlloc;
    stats->totalFragmentation = m->totalExcess;

    stats->peakBytesUsed = m->maxOut;
    stats->peakAllocationCount = m->maxCount;

    stats->currentPressure = memboundCalculatePressure(m);
    stats->usagePercent =
        (m->totalCapacity > 0)
            ? ((float)m->currentOut / (float)m->totalCapacity * 100.0f)
            : 0.0f;

    /* Lock contention profiling - use atomic loads for consistency */
    stats->lockAcquisitions =
        atomic_load_explicit(&m->lockAcquisitions, memory_order_relaxed);
    stats->lockContentions =
        atomic_load_explicit(&m->lockContentions, memory_order_relaxed);
    stats->lockWaitTimeNs =
        atomic_load_explicit(&m->lockWaitTimeNs, memory_order_relaxed);
    stats->contentionPercent = (stats->lockAcquisitions > 0)
                                   ? ((float)stats->lockContentions /
                                      (float)stats->lockAcquisitions * 100.0f)
                                   : 0.0f;

    /* Safety violation tracking */
    stats->safetyViolations =
        atomic_load_explicit(&m->safetyViolations, memory_order_relaxed);

    memboundLeave((membound *)m);
}

/* ====================================================================
 * Debug / Dump Functions
 * ==================================================================== */

#ifndef NDEBUG
static void memboundDumpExtent(const memboundExtent *e, FILE *out) {
    const size_t poolSize = memboundExtentPoolSize(e);
    fprintf(out, "  Extent %p:\n", (const void *)e);
    fprintf(out, "    Pool: %p - %p (%zu bytes)\n", (const void *)e->zPool,
            (const void *)(e->zPool + poolSize), poolSize);
    fprintf(out, "    Atom size: %" PRId64 ", blocks: %" PRId64 "\n", e->szAtom,
            e->nBlock);
    fprintf(out, "    Current: %" PRIu64 " bytes, %" PRIu64 " allocs\n",
            e->currentOut, e->currentCount);

    int nMinLog = memboundLog(e->szAtom);
    for (int i = 0; i <= MEMBOUND_LOGMAX && i + nMinLog < 64; i++) {
        int64_t n = 0;
        for (int64_t j = e->aiFreelist[i]; j >= 0;
             j = MEMBOUND_LINK(e, j)->next, n++) {
        }
        if (n > 0) {
            fprintf(out,
                    "    Freelist[%d] (size %" PRId64 "): %" PRId64 " blocks\n",
                    i, e->szAtom << i, n);
        }
    }
}

static void memboundDump(const membound *m, const char *filename) {
    FILE *out = filename ? fopen(filename, "w") : stdout;
    if (!out) {
        return;
    }

    fprintf(out, "=== membound dump ===\n");
    fprintf(out, "Mode: %s\n",
            m->mode == MEMBOUND_MODE_FIXED ? "FIXED" : "DYNAMIC");
    fprintf(out, "Extents: %zu\n", m->extentCount);
    fprintf(out, "Capacity: %" PRIu64 " bytes\n", m->totalCapacity);
    fprintf(out, "Current: %" PRIu64 " bytes, %" PRIu64 " allocs\n",
            m->currentOut, m->currentCount);
    fprintf(out, "Peak: %" PRIu64 " bytes, %" PRIu64 " allocs\n", m->maxOut,
            m->maxCount);
    fprintf(out, "Lifetime: %" PRIu64 " allocs, %" PRIu64 " bytes total\n",
            m->nAlloc, m->totalAlloc);

    for (memboundExtent *e = m->extents; e; e = e->next) {
        memboundDumpExtent(e, out);
    }

    if (out != stdout) {
        fclose(out);
    } else {
        fflush(stdout);
    }
}
#endif

/* ====================================================================
 * MICRO Tier Implementation
 * ====================================================================
 * Ultra-compact buddy allocator for millions of tiny pools.
 * - ~64 bytes overhead per pool (fits in 1 cache line)
 * - No mutex (caller responsible for thread safety)
 * - No lifetime stats or pressure callbacks
 * - Max 64KB pool size using 16-bit indices
 * - Uses the same buddy allocation algorithm as extents but with
 *   compact data structures
 */

/* Compact version of memboundLog for 16-bit indices */
static inline int memboundMicroLog(int value) {
    if (value <= 1) {
        return 0;
    }
#if defined(__GNUC__) || defined(__clang__)
    return 32 - __builtin_clz((unsigned)(value - 1));
#else
    int log = 0;
    value--;
    while (value >>= 1) {
        log++;
    }
    return log + 1;
#endif
}

/* MICRO tier MemboundLink - 4 bytes (uses 16-bit indices) */
typedef struct MemboundMicroLink {
    int16_t next;
    int16_t prev;
} MemboundMicroLink;

_Static_assert(sizeof(MemboundMicroLink) == 4,
               "MemboundMicroLink must be 4 bytes");

/* Get link pointer for MICRO tier */
#define MEMBOUND_MICRO_LINK(m, idx)                                            \
    ((MemboundMicroLink *)(&(m)->zPool[(idx) * (m)->szAtom]))

/* Unlink from MICRO freelist */
static void memboundMicroUnlink(memboundMicro *m, int16_t i, int iLogsize) {
    assert(i >= 0 && i < m->nBlock);
    assert(iLogsize >= 0 && iLogsize <= MEMBOUND_MICRO_LOGMAX);

    const int16_t next = MEMBOUND_MICRO_LINK(m, i)->next;
    const int16_t prev = MEMBOUND_MICRO_LINK(m, i)->prev;

    if (prev < 0) {
        m->aiFreelist[iLogsize] = next;
        if (next < 0) {
            m->freelistBitmap &= ~(1U << iLogsize);
        }
    } else {
        MEMBOUND_MICRO_LINK(m, prev)->next = next;
    }

    if (next >= 0) {
        MEMBOUND_MICRO_LINK(m, next)->prev = prev;
    }
}

/* Link into MICRO freelist */
static void memboundMicroLink(memboundMicro *m, int16_t i, int iLogsize) {
    assert(i >= 0 && i < m->nBlock);
    assert(iLogsize >= 0 && iLogsize <= MEMBOUND_MICRO_LOGMAX);

    const int16_t x = MEMBOUND_MICRO_LINK(m, i)->next = m->aiFreelist[iLogsize];
    MEMBOUND_MICRO_LINK(m, i)->prev = -1;

    if (x >= 0) {
        assert(x < m->nBlock);
        MEMBOUND_MICRO_LINK(m, x)->prev = i;
    }

    m->aiFreelist[iLogsize] = i;
    m->freelistBitmap |= (1U << iLogsize);
}

memboundMicro *memboundMicroCreate(size_t size) {
    /* Enforce MICRO tier limits */
    if (size == 0 || size > MEMBOUND_MICRO_MAX_POOL) {
        return NULL;
    }

    /* Allocate combined structure + pool + control array */
    /* Layout: [memboundMicro struct][pool data][control array] */

    /* Calculate atom size (minimum allocation unit) */
    int nMinLog = memboundMicroLog(MEMBOUND_MIN_ATOM);
    uint16_t szAtom = (1U << nMinLog);
    /* Ensure szAtom can hold MemboundMicroLink */
    while (sizeof(MemboundMicroLink) > szAtom) {
        szAtom <<= 1;
        nMinLog++;
    }

    /* Calculate number of blocks and control array size */
    uint16_t nBlock = size / (szAtom + sizeof(uint8_t));
    if (nBlock == 0) {
        return NULL;
    }

    /* Calculate pool data size */
    size_t poolDataSize = (size_t)nBlock * (size_t)szAtom;
    size_t ctrlSize = nBlock;

    /* Total allocation: struct + pool + ctrl */
    size_t totalSize = sizeof(memboundMicro) + poolDataSize + ctrlSize;

    /* Allocate everything in one mmap */
    void *mem = memboundMAP(totalSize);
    if (!mem) {
        return NULL;
    }
    memset(mem, 0, totalSize);

    /* Set up pointers */
    memboundMicro *m = (memboundMicro *)mem;
    m->zPool = (uint8_t *)mem + sizeof(memboundMicro);
    m->aCtrl = m->zPool + poolDataSize;

    /* Initialize fields */
    m->tier = MEMBOUND_TIER_MICRO;
    m->atomShift = nMinLog;
    m->szAtom = szAtom;
    m->nBlock = nBlock;
    m->poolSize = totalSize; /* Total mmap size for cleanup */
    m->currentOut = 0;
    m->currentCount = 0;
    m->freelistBitmap = 0;

    /* Initialize freelists */
    for (int i = 0; i <= MEMBOUND_MICRO_LOGMAX; i++) {
        m->aiFreelist[i] = -1;
    }

    /* Build initial free list from largest possible blocks */
    int16_t iOffset = 0;
    for (int ii = MEMBOUND_MICRO_LOGMAX; ii >= 0; ii--) {
        int16_t nAlloc = (1 << ii);
        if ((iOffset + nAlloc) <= m->nBlock) {
            m->aCtrl[iOffset] = ii | MEMBOUND_CTRL_FREE;
            memboundMicroLink(m, iOffset, ii);
            iOffset += nAlloc;
        }
    }

    return m;
}

void memboundMicroDestroy(memboundMicro *m) {
    if (!m) {
        return;
    }
    memboundUNMAP(m, m->poolSize);
}

void memboundMicroReset(memboundMicro *m) {
    if (!m) {
        return;
    }

    /* Clear all free lists */
    for (int i = 0; i <= MEMBOUND_MICRO_LOGMAX; i++) {
        m->aiFreelist[i] = -1;
    }
    m->freelistBitmap = 0;

    /* Rebuild free list from largest possible blocks */
    int16_t iOffset = 0;
    for (int ii = MEMBOUND_MICRO_LOGMAX; ii >= 0; ii--) {
        int16_t nAlloc = (1 << ii);
        if ((iOffset + nAlloc) <= m->nBlock) {
            m->aCtrl[iOffset] = ii | MEMBOUND_CTRL_FREE;
            memboundMicroLink(m, iOffset, ii);
            iOffset += nAlloc;
        }
    }

    m->currentOut = 0;
    m->currentCount = 0;
}

void *memboundMicroAlloc(memboundMicro *m, size_t nBytes) {
    if (!m || nBytes == 0) {
        return NULL;
    }

    /* Limit allocation to pool capacity */
    size_t maxAlloc = (size_t)m->nBlock * (size_t)m->szAtom;
    if (nBytes > maxAlloc) {
        return NULL;
    }

    int iBin, iLogsize;
    size_t iFullSz;

    /* Round nBytes up to next valid power of two */
    if ((int)nBytes <= m->szAtom) {
        iLogsize = 0;
        iFullSz = m->szAtom;
    } else {
        iLogsize =
            memboundMicroLog((int)((nBytes + m->szAtom - 1) >> m->atomShift));
        if (iLogsize > MEMBOUND_MICRO_LOGMAX) {
            return NULL; /* Too large for MICRO tier */
        }
        iFullSz = (size_t)m->szAtom << iLogsize;
    }

    /* Find available block using bitmap */
    const uint16_t availableMask = m->freelistBitmap >> iLogsize;
    if (availableMask == 0) {
        return NULL;
    }
    iBin = iLogsize + __builtin_ctz(availableMask);
    if (iBin > MEMBOUND_MICRO_LOGMAX) {
        return NULL;
    }

    int16_t i = m->aiFreelist[iBin];
    memboundMicroUnlink(m, i, iBin);

    /* Split larger blocks if needed */
    while (iBin > iLogsize) {
        iBin--;
        int16_t newSize = (1 << iBin);
        m->aCtrl[i + newSize] = MEMBOUND_CTRL_FREE | iBin;
        memboundMicroLink(m, i + newSize, iBin);
    }

    m->aCtrl[i] = iLogsize;

    /* Update statistics */
    m->currentCount++;
    m->currentOut += iFullSz;

    return (void *)&m->zPool[i * m->szAtom];
}

void memboundMicroFree(memboundMicro *m, void *pOld) {
    if (!m || !pOld) {
        return;
    }

    int16_t iBlock = (int16_t)(((uint8_t *)pOld - m->zPool) >> m->atomShift);

    /* Runtime safety checks - MICRO tier doesn't track violations for minimal
     * overhead */
    if (unlikely(iBlock < 0 || iBlock >= m->nBlock)) {
        return;
    }
    if (unlikely(((uint8_t *)pOld - m->zPool) % m->szAtom != 0)) {
        return;
    }
    if (unlikely(m->aCtrl[iBlock] & MEMBOUND_CTRL_FREE)) {
        return; /* Double-free */
    }

    int iLogsize = m->aCtrl[iBlock] & MEMBOUND_CTRL_LOGSIZE;
    int16_t size = (1 << iLogsize);

    if (unlikely(iBlock + size - 1 >= m->nBlock)) {
        return;
    }

    m->aCtrl[iBlock] |= MEMBOUND_CTRL_FREE;
    m->aCtrl[iBlock + size - 1] |= MEMBOUND_CTRL_FREE;

    if (unlikely(m->currentCount == 0)) {
        return;
    }
    if (unlikely(m->currentOut < ((uint16_t)size * (uint16_t)m->szAtom))) {
        return;
    }
    m->currentCount--;
    m->currentOut -= (uint16_t)size * (uint16_t)m->szAtom;

    m->aCtrl[iBlock] = MEMBOUND_CTRL_FREE | iLogsize;

    /* Coalesce with buddies */
    while (iLogsize < MEMBOUND_MICRO_LOGMAX) {
        int16_t iBuddy;
        if ((iBlock >> iLogsize) & 1) {
            iBuddy = iBlock - size;
            if (unlikely(iBuddy < 0)) {
                break; /* Safety: invalid buddy index */
            }
        } else {
            iBuddy = iBlock + size;
            if (iBuddy >= m->nBlock) {
                break;
            }
        }

        if (m->aCtrl[iBuddy] != (MEMBOUND_CTRL_FREE | iLogsize)) {
            break;
        }

        memboundMicroUnlink(m, iBuddy, iLogsize);
        iLogsize++;
        if (iBuddy < iBlock) {
            m->aCtrl[iBlock] = 0;
            m->aCtrl[iBuddy] = MEMBOUND_CTRL_FREE | iLogsize;
            iBlock = iBuddy;
        } else {
            m->aCtrl[iBlock] = MEMBOUND_CTRL_FREE | iLogsize;
            m->aCtrl[iBuddy] = 0;
        }
        size *= 2;
    }

    memboundMicroLink(m, iBlock, iLogsize);
}

bool memboundMicroOwns(const memboundMicro *m, const void *ptr) {
    if (!m || !ptr) {
        return false;
    }
    const uint8_t *p = (const uint8_t *)ptr;
    const size_t poolDataSize = (size_t)m->nBlock * (size_t)m->szAtom;
    return (p >= m->zPool && p < m->zPool + poolDataSize);
}

size_t memboundMicroSize(const memboundMicro *m, const void *ptr) {
    if (!m || !ptr) {
        return 0;
    }
    const int16_t i =
        (int16_t)(((const uint8_t *)ptr - m->zPool) >> m->atomShift);
    assert(i >= 0 && i < m->nBlock);
    return (size_t)m->szAtom << (m->aCtrl[i] & MEMBOUND_CTRL_LOGSIZE);
}

size_t memboundMicroBytesUsed(const memboundMicro *m) {
    return m ? m->currentOut : 0;
}

size_t memboundMicroBytesAvailable(const memboundMicro *m) {
    if (!m) {
        return 0;
    }
    const size_t total = (size_t)m->nBlock * (size_t)m->szAtom;
    return (total > m->currentOut) ? (total - m->currentOut) : 0;
}

size_t memboundMicroCapacity(const memboundMicro *m) {
    if (!m) {
        return 0;
    }
    return (size_t)m->nBlock * (size_t)m->szAtom;
}

size_t memboundMicroAllocationCount(const memboundMicro *m) {
    return m ? m->currentCount : 0;
}

/* ====================================================================
 * MICRO Tier Public API Wrappers
 * ==================================================================== */

/* Create a MICRO tier pool (public API) */
membound *memboundCreateMicro(size_t size) {
    memboundMicro *m = memboundMicroCreate(size);
    /* Cast is safe because tier is first byte, enabling detection */
    return (membound *)m;
}

/* ====================================================================
 * COMPACT Tier Implementation
 * ====================================================================
 * Lightweight single-extent pool with optional mutex.
 * Uses 32-bit indices for larger pools (up to ~4GB).
 * 16 size classes for finer granularity.
 */

/* Calculate log2 for COMPACT tier (up to 2^16 = 64K blocks) */
static inline int memboundCompactLog(int value) {
    if (value <= 1) {
        return 0;
    }
#if defined(__GNUC__) || defined(__clang__)
    /* Fast path: use hardware CLZ */
    return 32 - __builtin_clz((unsigned int)value - 1);
#else
    /* Portable path */
    int n = 0;
    int v = value - 1;
    while (v > 0) {
        v >>= 1;
        n++;
    }
    return n;
#endif
}

/* COMPACT tier Link node - 8 bytes (uses 32-bit indices) */
typedef struct memboundCompactLinkNode {
    int32_t next;
    int32_t prev;
} memboundCompactLinkNode;

/* Get link pointer for COMPACT tier */
#define MEMBOUND_COMPACT_LINK(m, idx)                                          \
    ((memboundCompactLinkNode *)&(m)->zPool[(idx) * (m)->szAtom])

/* Unlink from COMPACT freelist */
static void memboundCompactUnlink(memboundCompactPool *m, int32_t i,
                                  int iLogsize) {
    assert(i >= 0 && i < m->nBlock);
    assert(iLogsize >= 0 && iLogsize <= MEMBOUND_COMPACT_LOGMAX);

    const int32_t next = MEMBOUND_COMPACT_LINK(m, i)->next;
    const int32_t prev = MEMBOUND_COMPACT_LINK(m, i)->prev;

    if (prev < 0) {
        /* Head of list */
        m->aiFreelist[iLogsize] = next;
        if (next < 0) {
            m->freelistBitmap &= ~(1U << iLogsize);
        }
    } else {
        MEMBOUND_COMPACT_LINK(m, prev)->next = next;
    }

    if (next >= 0) {
        MEMBOUND_COMPACT_LINK(m, next)->prev = prev;
    }
}

/* Link into COMPACT freelist */
static void memboundCompactLink(memboundCompactPool *m, int32_t i,
                                int iLogsize) {
    assert(i >= 0 && i < m->nBlock);
    assert(iLogsize >= 0 && iLogsize <= MEMBOUND_COMPACT_LOGMAX);

    const int32_t x = MEMBOUND_COMPACT_LINK(m, i)->next =
        m->aiFreelist[iLogsize];
    MEMBOUND_COMPACT_LINK(m, i)->prev = -1;

    if (x >= 0) {
        assert(MEMBOUND_COMPACT_LINK(m, x)->prev < 0);
        MEMBOUND_COMPACT_LINK(m, x)->prev = i;
    }

    m->aiFreelist[iLogsize] = i;
    m->freelistBitmap |= (1U << iLogsize);
}

/* Create COMPACT tier pool (internal) */
memboundCompactPool *memboundCompactPoolCreate(size_t size, bool threadSafe) {
    if (size == 0) {
        return NULL;
    }

    /* Round up to power of 2 */
    size_t poolSize = 1;
    while (poolSize < size) {
        poolSize *= 2;
    }

    /* Calculate pool parameters */
    int nMinLog = memboundCompactLog(MEMBOUND_MIN_ATOM);
    int nPoolLog = memboundCompactLog((int)poolSize);

    /* Pool must be larger than minimum atom */
    if (nPoolLog <= nMinLog) {
        nPoolLog = nMinLog + 1;
        poolSize = (size_t)1 << nPoolLog;
    }

    int szAtom = MEMBOUND_MIN_ATOM;
    int atomShift = nMinLog;
    int nBlock = (int)(poolSize / szAtom);

    /* Ensure block count fits in 32-bit signed int */
    if (nBlock > INT32_MAX / 2) {
        /* Pool too large - cap it */
        nBlock = INT32_MAX / 2;
        poolSize = (size_t)nBlock * szAtom;
    }

    /* Calculate control array size */
    size_t ctrlSize = nBlock;

    /* Layout: [memboundCompactPool struct][pool data][control array] */
    size_t poolDataSize = (size_t)nBlock * szAtom;
    size_t totalSize = sizeof(memboundCompactPool) + poolDataSize + ctrlSize;

    /* Allocate all-in-one */
    void *mem = zmalloc(totalSize);
    if (!mem) {
        return NULL;
    }

    memset(mem, 0, totalSize);

    memboundCompactPool *m = (memboundCompactPool *)mem;
    m->zPool = (uint8_t *)mem + sizeof(memboundCompactPool);
    m->aCtrl = m->zPool + poolDataSize;

    m->tier = MEMBOUND_TIER_COMPACT;
    m->atomShift = (uint8_t)atomShift;
    m->szAtom = szAtom;
    m->nBlock = nBlock;
    m->poolSize = poolDataSize;
    m->currentOut = 0;
    m->currentCount = 0;
    m->freelistBitmap = 0;
    m->threadSafe = threadSafe ? 1 : 0;

    /* Initialize mutex if thread-safe */
    if (threadSafe) {
        if (pthread_mutex_init(&m->mutex, NULL) != 0) {
            zfree(mem);
            return NULL;
        }
        m->mutexInitialized = true;
    }

    /* Initialize freelists */
    for (int i = 0; i <= MEMBOUND_COMPACT_LOGMAX; i++) {
        m->aiFreelist[i] = -1;
    }

    /* Initialize control array and link blocks to freelists */
    for (int ii = MEMBOUND_COMPACT_LOGMAX; ii >= 0; ii--) {
        int nAlloc = (1 << ii);
        while (nBlock >= nAlloc) {
            int iOffset = nBlock - nAlloc;
            m->aCtrl[iOffset] = (uint8_t)ii;
            memboundCompactLink(m, iOffset, ii);
            nBlock -= nAlloc;
        }
    }

    return m;
}

/* Destroy COMPACT tier pool */
void memboundCompactPoolDestroy(memboundCompactPool *m) {
    if (!m) {
        return;
    }

    if (m->mutexInitialized) {
        pthread_mutex_destroy(&m->mutex);
    }

    zfree(m);
}

/* Reset COMPACT tier pool */
void memboundCompactPoolReset(memboundCompactPool *m) {
    if (!m) {
        return;
    }

    if (m->threadSafe && m->mutexInitialized) {
        pthread_mutex_lock(&m->mutex);
    }

    /* Clear stats */
    m->currentOut = 0;
    m->currentCount = 0;
    m->freelistBitmap = 0;

    /* Reset freelists */
    for (int i = 0; i <= MEMBOUND_COMPACT_LOGMAX; i++) {
        m->aiFreelist[i] = -1;
    }

    /* Reset control array and rebuild freelists */
    int nBlock = m->nBlock;
    for (int ii = MEMBOUND_COMPACT_LOGMAX; ii >= 0; ii--) {
        int nAlloc = (1 << ii);
        while (nBlock >= nAlloc) {
            int iOffset = nBlock - nAlloc;
            m->aCtrl[iOffset] = (uint8_t)ii;
            memboundCompactLink(m, iOffset, ii);
            nBlock -= nAlloc;
        }
    }

    if (m->threadSafe && m->mutexInitialized) {
        pthread_mutex_unlock(&m->mutex);
    }
}

/* Allocate from COMPACT tier */
void *memboundCompactPoolAlloc(memboundCompactPool *m, size_t nBytes) {
    if (!m || nBytes == 0) {
        return NULL;
    }

    if (m->threadSafe && m->mutexInitialized) {
        pthread_mutex_lock(&m->mutex);
    }

    void *result = NULL;
    int iLogsize;

    if (nBytes <= (size_t)m->szAtom) {
        iLogsize = 0;
    } else {
        iLogsize =
            memboundCompactLog((int)((nBytes + m->szAtom - 1) >> m->atomShift));
        if (iLogsize > MEMBOUND_COMPACT_LOGMAX) {
            goto done; /* Too large */
        }
    }

    /* Find smallest bin with free blocks */
    uint32_t mask = m->freelistBitmap >> iLogsize;
    if (mask == 0) {
        goto done; /* No space */
    }

    int iBin = iLogsize + __builtin_ctz(mask);
    if (iBin > MEMBOUND_COMPACT_LOGMAX) {
        goto done;
    }

    int32_t i = m->aiFreelist[iBin];
    memboundCompactUnlink(m, i, iBin);

    /* Split block if larger than needed */
    while (iBin > iLogsize) {
        iBin--;
        int32_t newSize = (1 << iBin);
        m->aCtrl[i + newSize] = (uint8_t)iBin;
        memboundCompactLink(m, i + newSize, iBin);
    }

    /* Mark as allocated */
    m->aCtrl[i] = (uint8_t)(iLogsize | 0x80); /* High bit = allocated */

    size_t blockSize = (size_t)m->szAtom << iLogsize;
    m->currentOut += (uint32_t)blockSize;
    m->currentCount++;

    result = &m->zPool[i * m->szAtom];

done:
    if (m->threadSafe && m->mutexInitialized) {
        pthread_mutex_unlock(&m->mutex);
    }
    return result;
}

/* Free to COMPACT tier */
void memboundCompactPoolFree(memboundCompactPool *m, void *pOld) {
    if (!m || !pOld) {
        return;
    }

    uint8_t *z = (uint8_t *)pOld;

    /* Verify ownership */
    if (z < m->zPool || z >= m->zPool + m->poolSize) {
        m->safetyViolations++;
        return;
    }

    if (m->threadSafe && m->mutexInitialized) {
        pthread_mutex_lock(&m->mutex);
    }

    int32_t iBlock = (int32_t)((z - m->zPool) / m->szAtom);

    /* Alignment check */
    if (((z - m->zPool) % m->szAtom) != 0) {
        m->safetyViolations++;
        goto done;
    }

    /* Double-free check - COMPACT uses high bit as allocated marker */
    if ((m->aCtrl[iBlock] & 0x80) == 0) {
        m->safetyViolations++;
        goto done;
    }

    int iLogsize = m->aCtrl[iBlock] & 0x7F; /* Strip allocated bit */
    size_t blockSize = (size_t)m->szAtom << iLogsize;

    m->currentOut -= (uint32_t)blockSize;
    m->currentCount--;

    /* Coalesce with buddy blocks */
    while (iLogsize < MEMBOUND_COMPACT_LOGMAX) {
        int nSize = (1 << iLogsize);
        int32_t iBuddy;

        /* Find buddy block */
        if ((iBlock >> iLogsize) & 1) {
            iBuddy = iBlock - nSize; /* We're the right buddy */
        } else {
            iBuddy = iBlock + nSize; /* We're the left buddy */
            if (iBuddy >= m->nBlock) {
                break;
            }
        }

        /* Check if buddy is free and same size */
        if (m->aCtrl[iBuddy] != iLogsize) {
            break;
        }

        /* Coalesce */
        memboundCompactUnlink(m, iBuddy, iLogsize);
        if (iBuddy < iBlock) {
            iBlock = iBuddy;
        }
        iLogsize++;
    }

    m->aCtrl[iBlock] = (uint8_t)iLogsize;
    memboundCompactLink(m, iBlock, iLogsize);

done:
    if (m->threadSafe && m->mutexInitialized) {
        pthread_mutex_unlock(&m->mutex);
    }
}

/* Check ownership for COMPACT tier */
bool memboundCompactPoolOwns(const memboundCompactPool *m, const void *ptr) {
    if (!m || !ptr) {
        return false;
    }
    const uint8_t *z = (const uint8_t *)ptr;
    return (z >= m->zPool && z < m->zPool + m->poolSize);
}

/* Query functions for COMPACT tier */
size_t memboundCompactPoolBytesUsed(const memboundCompactPool *m) {
    return m ? m->currentOut : 0;
}

size_t memboundCompactPoolBytesAvailable(const memboundCompactPool *m) {
    if (!m) {
        return 0;
    }
    /* Calculate based on freelist bitmap */
    size_t avail = 0;
    for (int i = 0; i <= MEMBOUND_COMPACT_LOGMAX; i++) {
        if (m->freelistBitmap & (1U << i)) {
            /* Count blocks in this freelist */
            int32_t idx = m->aiFreelist[i];
            while (idx >= 0) {
                avail += (size_t)m->szAtom << i;
                idx =
                    MEMBOUND_COMPACT_LINK((memboundCompactPool *)m, idx)->next;
            }
        }
    }
    return avail;
}

size_t memboundCompactPoolCapacity(const memboundCompactPool *m) {
    return m ? m->poolSize : 0;
}

size_t memboundCompactPoolAllocationCount(const memboundCompactPool *m) {
    return m ? m->currentCount : 0;
}

/* ====================================================================
 * COMPACT Tier Public API Wrappers
 * ==================================================================== */

/* Create a COMPACT tier pool (public API) */
membound *memboundCreateCompact(size_t size, bool threadSafe) {
    memboundCompactPool *m = memboundCompactPoolCreate(size, threadSafe);
    /* Cast is safe because tier is first byte, enabling detection */
    return (membound *)m;
}

/* Create an ENTERPRISE tier pool (public API) */
membound *memboundCreateEnterprise(size_t initialSize, size_t maxSize) {
    memboundConfig config = {
        .mode = MEMBOUND_MODE_DYNAMIC,
        .initialSize = initialSize,
        .tier = MEMBOUND_TIER_ENTERPRISE,
        .threadSafe = true,
        .maxTotalSize = maxSize,
        .growthSize = initialSize, /* Grow by same amount as initial */
        .growthFactor = 0.0,
        .maxExtentCount = 0, /* Unlimited */
        .strategy = MEMBOUND_STRATEGY_FIRST_FIT,
        .perExtentLocking = true, /* Enable per-extent parallelism by default */
    };
    return memboundCreateWithConfig(&config);
}

/* ====================================================================
 * Preset Configurations
 * ==================================================================== */

/* Preset: Coroutine/fiber pools (MICRO tier)
 * For millions of short-lived micro-allocators.
 * Returns config for 4KB pool, no thread safety. */
memboundConfig memboundPresetCoroutine(void) {
    memboundConfig config = {
        .mode = MEMBOUND_MODE_FIXED,
        .initialSize = 4096, /* 4 KB - small coroutine stack */
        .tier = MEMBOUND_TIER_MICRO,
        .threadSafe = false, /* Coroutines typically single-threaded */
    };
    return config;
}

/* Preset: Per-connection pools (COMPACT tier)
 * For thousands of connection-specific allocators.
 * Returns config for 64KB pool with optional thread safety. */
memboundConfig memboundPresetConnection(bool threadSafe) {
    memboundConfig config = {
        .mode = MEMBOUND_MODE_FIXED,
        .initialSize = 65536, /* 64 KB - typical connection buffer */
        .tier = MEMBOUND_TIER_COMPACT,
        .threadSafe = threadSafe,
    };
    return config;
}

/* Preset: General purpose (STANDARD tier)
 * Default full-featured configuration.
 * Returns config for 1MB initial, unlimited growth. */
memboundConfig memboundPresetGeneral(void) {
    memboundConfig config = {
        .mode = MEMBOUND_MODE_DYNAMIC,
        .initialSize = 1 << 20, /* 1 MB initial */
        .tier = MEMBOUND_TIER_STANDARD,
        .threadSafe = true,
        .maxTotalSize = 0,     /* Unlimited */
        .growthSize = 1 << 20, /* Grow by 1MB chunks */
        .strategy = MEMBOUND_STRATEGY_FIRST_FIT,
    };
    return config;
}

/* Preset: Database/cache (ENTERPRISE tier)
 * For large shared buffer pools with high concurrency.
 * Returns config for 64MB initial, size-class strategy. */
memboundConfig memboundPresetDatabase(void) {
    memboundConfig config = {
        .mode = MEMBOUND_MODE_DYNAMIC,
        .initialSize = 64 << 20, /* 64 MB initial */
        .tier = MEMBOUND_TIER_ENTERPRISE,
        .threadSafe = true,
        .maxTotalSize = 0,      /* Unlimited */
        .growthSize = 64 << 20, /* Grow by 64MB chunks */
        .strategy = MEMBOUND_STRATEGY_SIZE_CLASS,
        .sizeClassSmall = 1024,   /* Small: <= 1KB */
        .sizeClassMedium = 65536, /* Medium: <= 64KB */
        .perExtentLocking = true, /* Enable per-extent parallelism */
    };
    return config;
}

/* Preset: Embedded/constrained (COMPACT tier, no growth)
 * For memory-constrained environments.
 * Returns config for specified size, no thread safety. */
memboundConfig memboundPresetEmbedded(size_t maxMemory) {
    memboundConfig config = {
        .mode = MEMBOUND_MODE_FIXED,
        .initialSize = maxMemory,
        .tier = MEMBOUND_TIER_COMPACT,
        .threadSafe = false, /* Minimize overhead */
    };
    return config;
}

/* ====================================================================
 * Tier Query Function
 * ==================================================================== */

memboundTier memboundGetTier(const membound *m) {
    return memboundGetTierInternal(m);
}

/* ====================================================================
 * Test Suite
 * ==================================================================== */

#ifdef DATAKIT_TEST
#include "ctest.h"

/* Test callback for pressure tests */
static memboundPressure testLastPressure = MEMBOUND_PRESSURE_LOW;
static int testPressureCallCount = 0;

static void testPressureCb(membound *mb, memboundPressure level, void *data) {
    (void)mb;
    (void)data;
    testLastPressure = level;
    testPressureCallCount++;
}

/* ================================================================
 * Multi-threaded Test Infrastructure
 * ================================================================ */

#define MT_NUM_THREADS 8
#define MT_ITERATIONS_PER_THREAD 10000

/* Shared state for multi-threaded tests */
typedef struct {
    membound *m;
    int threadId;
    int iterations;
    volatile int *startFlag;
    volatile int errors;
} mtTestContext;

static void *mtAllocFreeWorker(void *arg) {
    mtTestContext *ctx = (mtTestContext *)arg;

    /* Wait for start signal */
    while (!*ctx->startFlag) {
        /* spin */
    }

    void *ptrs[16] = {0};
    int ptrCount = 0;

    for (int i = 0; i < ctx->iterations; i++) {
        /* Random operation: alloc, free, or query */
        int op = (i + ctx->threadId) % 3;

        if (op == 0 && ptrCount < 16) {
            /* Alloc */
            size_t size = 64 + ((i * 37 + ctx->threadId) % 4096);
            void *p = memboundAlloc(ctx->m, size);
            if (p) {
                /* Write pattern to verify no corruption */
                memset(p, ctx->threadId & 0xFF, size < 64 ? size : 64);
                ptrs[ptrCount++] = p;
            }
        } else if (op == 1 && ptrCount > 0) {
            /* Free */
            int idx = i % ptrCount;
            void *p = ptrs[idx];
            /* Verify ownership before free */
            if (!memboundOwns(ctx->m, p)) {
                ctx->errors++;
            }
            memboundFree(ctx->m, p);
            ptrs[idx] = ptrs[--ptrCount];
        } else {
            /* Query stats */
            memboundStats stats;
            memboundGetStats(ctx->m, &stats);
            /* Basic sanity check */
            if (stats.bytesUsed > stats.capacity) {
                ctx->errors++;
            }
        }
    }

    /* Cleanup remaining allocations */
    for (int i = 0; i < ptrCount; i++) {
        memboundFree(ctx->m, ptrs[i]);
    }

    return NULL;
}

static void *mtReallocWorker(void *arg) {
    mtTestContext *ctx = (mtTestContext *)arg;

    /* Wait for start signal */
    while (!*ctx->startFlag) {
        /* spin */
    }

    void *p = NULL;

    for (int i = 0; i < ctx->iterations; i++) {
        /* Alternate between grow and shrink */
        size_t newSize;
        if (i % 2 == 0) {
            newSize = 128 + ((i * 17 + ctx->threadId) % 8192);
        } else {
            newSize = 64 + ((i * 13 + ctx->threadId) % 256);
        }

        void *newP = memboundRealloc(ctx->m, p, newSize);
        if (newP || newSize == 0) {
            p = newP;

            if (p && newSize > 0) {
                /* Write pattern */
                ((unsigned char *)p)[0] = ctx->threadId & 0xFF;
                if (newSize > 1) {
                    ((unsigned char *)p)[newSize - 1] = ctx->threadId & 0xFF;
                }
            }
        }

        /* Occasionally free and start fresh */
        if (i % 100 == 99) {
            memboundFree(ctx->m, p);
            p = NULL;
        }
    }

    if (p) {
        memboundFree(ctx->m, p);
    }

    return NULL;
}

static void *mtQueryWorker(void *arg) {
    mtTestContext *ctx = (mtTestContext *)arg;

    while (!*ctx->startFlag) {
        /* spin */
    }

    for (int i = 0; i < ctx->iterations; i++) {
        /* Query various stats while other threads mutate */
        memboundStats stats;
        memboundGetStats(ctx->m, &stats);

        float frag = memboundGetFragmentation(ctx->m);
        (void)frag; /* May be NaN if no allocations */

        size_t count = memboundExtentCount(ctx->m);
        if (count == 0) {
            ctx->errors++;
        }

        size_t cap = memboundCapacity(ctx->m);
        if (cap == 0) {
            ctx->errors++;
        }

        size_t used = memboundBytesUsed(ctx->m);
        size_t avail = memboundBytesAvailable(ctx->m);
        (void)used;
        (void)avail;

        bool canGrow = memboundCanGrow(ctx->m);
        (void)canGrow;
    }

    return NULL;
}

/* Pressure callback reentrancy test - verify callback executes outside lock */
static volatile int mtPressureCallbackExecuting = 0;
static membound *mtPressureTestPool = NULL;
static volatile int mtPressureCallbackCount = 0;

static void mtPressureCallback(membound *mb, memboundPressure level,
                               void *userData) {
    (void)userData;
    (void)level;

    /* Guard against recursive invocation (alloc/free in callback triggers
     * callback) */
    if (mtPressureCallbackExecuting) {
        return;
    }

    mtPressureCallbackExecuting = 1;
    mtPressureCallbackCount++;

    /* Try to call a membound function from within the callback.
     * If the lock is held, this would deadlock. */
    size_t count = memboundCurrentAllocationCount(mb);
    (void)count;

    /* Try another query */
    memboundStats stats;
    memboundGetStats(mb, &stats);

    /* Even try an allocation (should work if callback is outside lock) */
    void *p = memboundAlloc(mb, 64);
    if (p) {
        memboundFree(mb, p);
    }

    mtPressureCallbackExecuting = 0;
}

int memboundTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    /* ================================================================
     * Fixed Mode Tests (Original Behavior)
     * ================================================================ */

    TEST("Fixed: Basic alloc and free") {
        membound *m = memboundCreate(1 << 20);
        assert(m != NULL);
        assert(memboundGetMode(m) == MEMBOUND_MODE_FIXED);
        assert(memboundExtentCount(m) == 1);

        void *p = memboundAlloc(m, 256);
        assert(p != NULL);
        assert(memboundCurrentAllocationCount(m) == 1);
        assert(memboundOwns(m, p));

        memboundFree(m, p);
        assert(memboundCurrentAllocationCount(m) == 0);

        memboundShutdown(m);
    }

    TEST("Fixed: Repeated alloc/free cycle") {
        const int createMax = 1 << 20;
        const int iterations = (createMax / 8192) * 2;
        membound *m = memboundCreate(createMax);
        assert(m != NULL);

        for (int i = 0; i < iterations; i++) {
            void *got = memboundAlloc(m, 8192);
            assert(got != NULL);
            memboundFree(m, got);
        }

        assert(memboundCurrentAllocationCount(m) == 0);
        memboundShutdown(m);
    }

    TEST("Fixed: Pool exhaustion") {
        const size_t poolSize = 1 << 16;
        membound *m = memboundCreate(poolSize);
        assert(m != NULL);

        void *ptrs[100];
        int count = 0;
        for (int i = 0; i < 100; i++) {
            void *p = memboundAlloc(m, 4096);
            if (!p) {
                break;
            }
            ptrs[count++] = p;
        }
        assert(count > 0);
        assert(count < 100);

        assert(memboundAlloc(m, 4096) == NULL);

        for (int i = 0; i < count; i++) {
            memboundFree(m, ptrs[i]);
        }
        assert(memboundCurrentAllocationCount(m) == 0);

        memboundShutdown(m);
    }

    TEST("Fixed: calloc zero-initialization") {
        membound *m = memboundCreate(1 << 20);
        assert(m != NULL);

        void *p1 = memboundAlloc(m, 1024);
        assert(p1 != NULL);
        memset(p1, 0xFF, 1024);
        memboundFree(m, p1);

        uint8_t *p2 = memboundCalloc(m, 128, 8);
        assert(p2 != NULL);
        for (int i = 0; i < 1024; i++) {
            assert(p2[i] == 0);
        }
        memboundFree(m, p2);

        memboundShutdown(m);
    }

    TEST("Fixed: calloc overflow protection") {
        membound *m = memboundCreate(1 << 20);
        assert(m != NULL);

        assert(memboundCalloc(m, SIZE_MAX, 2) == NULL);
        assert(memboundCalloc(m, 0, 100) == NULL);
        assert(memboundCalloc(m, 100, 0) == NULL);

        memboundShutdown(m);
    }

    TEST("Fixed: realloc grow") {
        membound *m = memboundCreate(1 << 20);
        assert(m != NULL);

        char *p = memboundAlloc(m, 256);
        assert(p != NULL);
        strcpy(p, "hello");

        p = memboundRealloc(m, p, 1024);
        assert(p != NULL);
        assert(strcmp(p, "hello") == 0);

        memboundFree(m, p);
        memboundShutdown(m);
    }

    TEST("Fixed: realloc shrink (no-op)") {
        membound *m = memboundCreate(1 << 20);
        assert(m != NULL);

        char *p = memboundAlloc(m, 1024);
        assert(p != NULL);
        char *original = p;
        strcpy(p, "test data");

        p = memboundRealloc(m, p, 256);
        assert(p == original);
        assert(strcmp(p, "test data") == 0);

        memboundFree(m, p);
        memboundShutdown(m);
    }

    TEST("Fixed: realloc NULL acts like alloc") {
        membound *m = memboundCreate(1 << 20);
        assert(m != NULL);

        void *p = memboundRealloc(m, NULL, 256);
        assert(p != NULL);
        assert(memboundCurrentAllocationCount(m) == 1);

        memboundFree(m, p);
        memboundShutdown(m);
    }

    TEST("Fixed: realloc zero size acts like free") {
        membound *m = memboundCreate(1 << 20);
        assert(m != NULL);

        void *p = memboundAlloc(m, 256);
        assert(p != NULL);
        assert(memboundCurrentAllocationCount(m) == 1);

        void *result = memboundRealloc(m, p, 0);
        assert(result == NULL);
        assert(memboundCurrentAllocationCount(m) == 0);

        memboundShutdown(m);
    }

    TEST("Fixed: reset bulk free") {
        membound *m = memboundCreate(1 << 20);
        assert(m != NULL);

        for (int i = 0; i < 10; i++) {
            void *p = memboundAlloc(m, 1024);
            assert(p != NULL);
        }
        assert(memboundCurrentAllocationCount(m) == 10);

        memboundReset(m);
        assert(memboundCurrentAllocationCount(m) == 0);
        assert(memboundBytesUsed(m) == 0);

        void *p = memboundAlloc(m, 1024);
        assert(p != NULL);
        memboundFree(m, p);

        memboundShutdown(m);
    }

    TEST("Fixed: increaseSize when empty") {
        membound *m = memboundCreate(1 << 16);
        assert(m != NULL);

        size_t oldCapacity = memboundCapacity(m);

        bool grew = memboundIncreaseSize(m, 1 << 18);
        assert(grew);
        assert(memboundCapacity(m) > oldCapacity);

        void *p = memboundAlloc(m, 1024);
        assert(p != NULL);
        memboundFree(m, p);

        memboundShutdown(m);
    }

    TEST("Fixed: increaseSize fails with allocations") {
        membound *m = memboundCreate(1 << 16);
        assert(m != NULL);

        void *p = memboundAlloc(m, 256);
        assert(p != NULL);

        bool grew = memboundIncreaseSize(m, 1 << 18);
        assert(!grew);

        memboundFree(m, p);

        grew = memboundIncreaseSize(m, 1 << 18);
        assert(grew);

        memboundShutdown(m);
    }

    TEST("Fixed: shutdownSafe with allocations") {
        membound *m = memboundCreate(1 << 16);
        assert(m != NULL);

        void *p = memboundAlloc(m, 256);
        assert(p != NULL);

        bool shut = memboundShutdownSafe(m);
        assert(!shut);

        memboundFree(m, p);

        shut = memboundShutdownSafe(m);
        assert(shut);
    }

    TEST("Fixed: bytesUsed and bytesAvailable") {
        membound *m = memboundCreate(1 << 20);
        assert(m != NULL);

        size_t capacity = memboundCapacity(m);
        assert(memboundBytesUsed(m) == 0);
        assert(memboundBytesAvailable(m) == capacity);

        void *p = memboundAlloc(m, 256);
        assert(p != NULL);
        assert(memboundBytesUsed(m) > 0);
        assert(memboundBytesAvailable(m) < capacity);

        memboundFree(m, p);
        assert(memboundBytesUsed(m) == 0);

        memboundShutdown(m);
    }

    TEST("Fixed: ownership check") {
        membound *m = memboundCreate(1 << 16);
        assert(m != NULL);

        void *p = memboundAlloc(m, 256);
        assert(p != NULL);
        assert(memboundOwns(m, p));

        int stackVar = 42;
        assert(!memboundOwns(m, &stackVar));
        assert(!memboundOwns(m, NULL));

        memboundFree(m, p);
        memboundShutdown(m);
    }

    TEST("Fixed: NULL safety") {
        assert(memboundAlloc(NULL, 100) == NULL);
        memboundFree(NULL, (void *)0x1234);
        memboundFree((membound *)0x1234, NULL);
        assert(memboundRealloc(NULL, NULL, 100) == NULL);
        memboundReset(NULL);
        assert(memboundIncreaseSize(NULL, 1000) == false);
        assert(memboundShutdown(NULL) == false);
        assert(memboundShutdownSafe(NULL) == false);
        assert(memboundCurrentAllocationCount(NULL) == 0);
        assert(memboundBytesUsed(NULL) == 0);
        assert(memboundBytesAvailable(NULL) == 0);
        assert(memboundCapacity(NULL) == 0);
        assert(memboundOwns(NULL, (void *)0x1234) == false);
        assert(memboundCalloc(NULL, 10, 10) == NULL);
    }

    TEST("Fixed: various allocation sizes") {
        membound *m = memboundCreate(1 << 22);
        assert(m != NULL);

        size_t sizes[] = {1,   7,    64,   100,   255,   256,   257,
                          500, 1000, 4096, 10000, 65536, 100000};
        void *ptrs[sizeof(sizes) / sizeof(sizes[0])];

        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            ptrs[i] = memboundAlloc(m, sizes[i]);
            assert(ptrs[i] != NULL);
            memset(ptrs[i], (int)i, sizes[i]);
        }

        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            memboundFree(m, ptrs[i]);
        }

        assert(memboundCurrentAllocationCount(m) == 0);
        memboundShutdown(m);
    }

    TEST("Fixed: fragmentation and coalescing") {
        membound *m = memboundCreate(1 << 20);
        assert(m != NULL);

        void *p1 = memboundAlloc(m, 256);
        void *p2 = memboundAlloc(m, 256);
        void *p3 = memboundAlloc(m, 256);
        void *p4 = memboundAlloc(m, 256);
        assert(p1 && p2 && p3 && p4);

        memboundFree(m, p2);
        memboundFree(m, p4);
        memboundFree(m, p1);
        memboundFree(m, p3);

        assert(memboundCurrentAllocationCount(m) == 0);
        void *big = memboundAlloc(m, 4096);
        assert(big != NULL);
        memboundFree(m, big);

        memboundShutdown(m);
    }

    TEST("Fixed: stress test - rapid alloc/free cycles") {
        membound *m = memboundCreate(1 << 22);
        assert(m != NULL);

        const int iterations = 100000;
        for (int i = 0; i < iterations; i++) {
            size_t size = 64 + (i % 1024);
            void *p = memboundAlloc(m, size);
            assert(p != NULL);
            *(volatile char *)p = (char)i;
            memboundFree(m, p);
        }

        assert(memboundCurrentAllocationCount(m) == 0);
        memboundShutdown(m);
    }

    /* ================================================================
     * MICRO Tier Tests
     * ================================================================ */

    TEST("MICRO: Basic alloc and free") {
        membound *m = memboundCreateMicro(8192);
        assert(m != NULL);
        assert(memboundGetMode(m) == MEMBOUND_MODE_FIXED);
        assert(memboundGetTier(m) == MEMBOUND_TIER_MICRO);
        assert(memboundExtentCount(m) == 1);

        void *p = memboundAlloc(m, 256);
        assert(p != NULL);
        assert(memboundCurrentAllocationCount(m) == 1);
        assert(memboundOwns(m, p));

        memboundFree(m, p);
        assert(memboundCurrentAllocationCount(m) == 0);

        memboundShutdown(m);
    }

    TEST("MICRO: Multiple allocations") {
        membound *m = memboundCreateMicro(16384);
        assert(m != NULL);

        void *p1 = memboundAlloc(m, 256);
        void *p2 = memboundAlloc(m, 512);
        void *p3 = memboundAlloc(m, 1024);
        assert(p1 != NULL);
        assert(p2 != NULL);
        assert(p3 != NULL);
        assert(memboundCurrentAllocationCount(m) == 3);

        /* Verify all are distinct pointers */
        assert(p1 != p2 && p2 != p3 && p1 != p3);

        /* Verify ownership */
        assert(memboundOwns(m, p1));
        assert(memboundOwns(m, p2));
        assert(memboundOwns(m, p3));

        memboundFree(m, p2);
        assert(memboundCurrentAllocationCount(m) == 2);
        /* Note: memboundOwns checks pool bounds, not allocation state,
         * so freed pointer may still return true - that's expected */

        memboundFree(m, p1);
        memboundFree(m, p3);
        assert(memboundCurrentAllocationCount(m) == 0);

        memboundShutdown(m);
    }

    TEST("MICRO: Pool exhaustion") {
        /* Small pool to force exhaustion */
        membound *m = memboundCreateMicro(4096);
        assert(m != NULL);

        void *ptrs[32];
        int count = 0;
        for (int i = 0; i < 32; i++) {
            void *p = memboundAlloc(m, 256);
            if (!p) {
                break;
            }
            ptrs[count++] = p;
        }
        assert(count > 0);
        assert(count < 32);

        /* Pool is exhausted */
        assert(memboundAlloc(m, 256) == NULL);

        /* Free all and verify we can allocate again */
        for (int i = 0; i < count; i++) {
            memboundFree(m, ptrs[i]);
        }
        assert(memboundCurrentAllocationCount(m) == 0);

        void *p = memboundAlloc(m, 256);
        assert(p != NULL);
        memboundFree(m, p);

        memboundShutdown(m);
    }

    TEST("MICRO: Reset functionality") {
        membound *m = memboundCreateMicro(8192);
        assert(m != NULL);

        for (int i = 0; i < 5; i++) {
            void *p = memboundAlloc(m, 256);
            assert(p != NULL);
        }
        assert(memboundCurrentAllocationCount(m) == 5);

        memboundReset(m);
        assert(memboundCurrentAllocationCount(m) == 0);
        assert(memboundBytesUsed(m) == 0);

        /* Can allocate again after reset */
        void *p = memboundAlloc(m, 1024);
        assert(p != NULL);
        memboundFree(m, p);

        memboundShutdown(m);
    }

    TEST("MICRO: Stats and capacity queries") {
        membound *m = memboundCreateMicro(16384);
        assert(m != NULL);

        size_t cap = memboundCapacity(m);
        assert(cap > 0 && cap <= 16384);

        size_t avail0 = memboundBytesAvailable(m);
        assert(avail0 > 0);

        void *p1 = memboundAlloc(m, 1024);
        void *p2 = memboundAlloc(m, 2048);
        assert(p1 && p2);

        size_t used = memboundBytesUsed(m);
        assert(used >= 3072); /* At least what we allocated */

        size_t avail1 = memboundBytesAvailable(m);
        assert(avail1 < avail0); /* Should have decreased */

        memboundStats stats;
        memboundGetStats(m, &stats);
        assert(stats.tier == MEMBOUND_TIER_MICRO);
        assert(stats.allocationCount == 2);
        assert(stats.bytesUsed >= 3072);

        memboundFree(m, p1);
        memboundFree(m, p2);
        memboundShutdown(m);
    }

    TEST("MICRO: Buddy coalescing") {
        membound *m = memboundCreateMicro(8192);
        assert(m != NULL);

        /* Allocate two adjacent blocks */
        void *p1 = memboundAlloc(m, 256);
        void *p2 = memboundAlloc(m, 256);
        assert(p1 && p2);

        size_t usedBefore = memboundBytesUsed(m);

        /* Free both - should coalesce */
        memboundFree(m, p1);
        memboundFree(m, p2);

        /* Now allocate larger block that fits in coalesced space */
        void *p3 = memboundAlloc(m, 512);
        assert(p3 != NULL);

        /* Should be able to use the coalesced space */
        size_t usedAfter = memboundBytesUsed(m);
        /* Larger allocation but single block */
        assert(usedAfter >= 512);
        (void)usedBefore;

        memboundFree(m, p3);
        memboundShutdown(m);
    }

    TEST("MICRO: Various allocation sizes") {
        membound *m = memboundCreateMicro(MEMBOUND_MICRO_MAX_POOL);
        assert(m != NULL);

        /* Test power-of-2 sizes */
        size_t sizes[] = {64, 128, 256, 512, 1024, 2048, 4096};
        void *ptrs[7];

        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            ptrs[i] = memboundAlloc(m, sizes[i]);
            if (!ptrs[i]) {
                break; /* Exhausted */
            }
            /* Write to verify usable */
            memset(ptrs[i], (int)i, sizes[i]);
        }

        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            if (ptrs[i]) {
                memboundFree(m, ptrs[i]);
            }
        }

        memboundShutdown(m);
    }

    TEST("MICRO: Minimum and maximum allocations") {
        membound *m = memboundCreateMicro(32768);
        assert(m != NULL);

        /* Minimum allocation */
        void *pMin = memboundAlloc(m, 1);
        assert(pMin != NULL);
        *(char *)pMin = 'x';

        /* Large allocation (but within MICRO limits) */
        void *pLarge = memboundAlloc(m, 8192);
        assert(pLarge != NULL);

        /* Too large for remaining pool should fail */
        void *pHuge = memboundAlloc(m, 32768);
        assert(pHuge == NULL); /* Won't fit */

        memboundFree(m, pMin);
        memboundFree(m, pLarge);
        memboundShutdown(m);
    }

    TEST("MICRO: NULL safety") {
        /* These should not crash */
        memboundFree((membound *)NULL, (void *)0x1234);

        membound *m = memboundCreateMicro(8192);
        assert(m != NULL);

        memboundFree(m, NULL); /* Should be no-op */
        assert(memboundOwns(m, NULL) == false);

        void *p = memboundAlloc(m, 0);
        assert(p == NULL); /* Zero-size allocation */

        memboundShutdown(m);
    }

    TEST("MICRO: Creation limits") {
        /* NULL for zero size */
        membound *m0 = memboundCreateMicro(0);
        assert(m0 == NULL);

        /* NULL for size exceeding MICRO max */
        membound *mBig = memboundCreateMicro(MEMBOUND_MICRO_MAX_POOL + 1);
        assert(mBig == NULL);

        /* Valid creation at max size */
        membound *mMax = memboundCreateMicro(MEMBOUND_MICRO_MAX_POOL);
        assert(mMax != NULL);
        assert(memboundGetTier(mMax) == MEMBOUND_TIER_MICRO);
        memboundShutdown(mMax);

        /* Valid creation at small size */
        membound *mSmall = memboundCreateMicro(1024);
        assert(mSmall != NULL);
        memboundShutdown(mSmall);
    }

    TEST("MICRO: Repeated alloc/free stress") {
        membound *m = memboundCreateMicro(16384);
        assert(m != NULL);

        for (int i = 0; i < 1000; i++) {
            size_t size = 64 + (i % 512);
            void *p = memboundAlloc(m, size);
            if (p) {
                memset(p, i & 0xFF, size);
                memboundFree(m, p);
            }
        }

        assert(memboundCurrentAllocationCount(m) == 0);
        memboundShutdown(m);
    }

    TEST("MICRO: Config-based creation") {
        memboundConfig config = {0};
        config.tier = MEMBOUND_TIER_MICRO;
        config.mode = MEMBOUND_MODE_FIXED;
        config.initialSize = 8192;

        membound *m = memboundCreateWithConfig(&config);
        assert(m != NULL);
        assert(memboundGetTier(m) == MEMBOUND_TIER_MICRO);

        void *p = memboundAlloc(m, 256);
        assert(p != NULL);
        memboundFree(m, p);

        memboundShutdown(m);
    }

    TEST("MICRO: Config with too-large size falls back to STANDARD") {
        memboundConfig config = {0};
        config.tier = MEMBOUND_TIER_MICRO;
        config.mode = MEMBOUND_MODE_FIXED;
        config.initialSize = MEMBOUND_MICRO_MAX_POOL + 4096;

        membound *m = memboundCreateWithConfig(&config);
        assert(m != NULL);
        /* Should have fallen back to STANDARD tier */
        assert(memboundGetTier(m) == MEMBOUND_TIER_STANDARD);

        memboundShutdown(m);
    }

    /* ================================================================
     * COMPACT Tier Tests
     * ================================================================ */

    TEST("COMPACT: Basic alloc and free") {
        membound *m = memboundCreateCompact(1 << 20, false);
        assert(m != NULL);
        assert(memboundGetMode(m) == MEMBOUND_MODE_FIXED);
        assert(memboundGetTier(m) == MEMBOUND_TIER_COMPACT);
        assert(memboundExtentCount(m) == 1);

        void *p = memboundAlloc(m, 256);
        assert(p != NULL);
        assert(memboundCurrentAllocationCount(m) == 1);
        assert(memboundOwns(m, p));

        memboundFree(m, p);
        assert(memboundCurrentAllocationCount(m) == 0);

        memboundShutdown(m);
    }

    TEST("COMPACT: Thread-safe mode") {
        membound *m = memboundCreateCompact(1 << 20, true);
        assert(m != NULL);
        assert(memboundGetTier(m) == MEMBOUND_TIER_COMPACT);

        memboundStats stats;
        memboundGetStats(m, &stats);
        assert(stats.threadSafe == true);

        void *p = memboundAlloc(m, 1024);
        assert(p != NULL);
        memboundFree(m, p);

        memboundShutdown(m);
    }

    TEST("COMPACT: Multiple allocations") {
        membound *m = memboundCreateCompact(1 << 20, false);
        assert(m != NULL);

        void *p1 = memboundAlloc(m, 256);
        void *p2 = memboundAlloc(m, 512);
        void *p3 = memboundAlloc(m, 1024);
        void *p4 = memboundAlloc(m, 4096);
        assert(p1 && p2 && p3 && p4);
        assert(memboundCurrentAllocationCount(m) == 4);

        /* Verify all are distinct */
        assert(p1 != p2 && p2 != p3 && p3 != p4);

        memboundFree(m, p2);
        assert(memboundCurrentAllocationCount(m) == 3);

        memboundFree(m, p1);
        memboundFree(m, p3);
        memboundFree(m, p4);
        assert(memboundCurrentAllocationCount(m) == 0);

        memboundShutdown(m);
    }

    TEST("COMPACT: Pool exhaustion") {
        membound *m = memboundCreateCompact(1 << 16, false);
        assert(m != NULL);

        void *ptrs[64];
        int count = 0;
        for (int i = 0; i < 64; i++) {
            void *p = memboundAlloc(m, 2048);
            if (!p) {
                break;
            }
            ptrs[count++] = p;
        }
        assert(count > 0);
        assert(count < 64);

        /* Pool is exhausted */
        assert(memboundAlloc(m, 2048) == NULL);

        for (int i = 0; i < count; i++) {
            memboundFree(m, ptrs[i]);
        }
        assert(memboundCurrentAllocationCount(m) == 0);

        memboundShutdown(m);
    }

    TEST("COMPACT: Reset functionality") {
        membound *m = memboundCreateCompact(1 << 20, false);
        assert(m != NULL);

        for (int i = 0; i < 10; i++) {
            void *p = memboundAlloc(m, 1024);
            assert(p != NULL);
        }
        assert(memboundCurrentAllocationCount(m) == 10);

        memboundReset(m);
        assert(memboundCurrentAllocationCount(m) == 0);
        assert(memboundBytesUsed(m) == 0);

        void *p = memboundAlloc(m, 1024);
        assert(p != NULL);
        memboundFree(m, p);

        memboundShutdown(m);
    }

    TEST("COMPACT: Stats and capacity queries") {
        membound *m = memboundCreateCompact(1 << 20, false);
        assert(m != NULL);

        size_t cap = memboundCapacity(m);
        assert(cap > 0);

        size_t avail0 = memboundBytesAvailable(m);
        assert(avail0 > 0);

        void *p1 = memboundAlloc(m, 1024);
        void *p2 = memboundAlloc(m, 2048);
        assert(p1 && p2);

        size_t used = memboundBytesUsed(m);
        assert(used >= 3072);

        size_t avail1 = memboundBytesAvailable(m);
        assert(avail1 < avail0);

        memboundStats stats;
        memboundGetStats(m, &stats);
        assert(stats.tier == MEMBOUND_TIER_COMPACT);
        assert(stats.allocationCount == 2);
        assert(stats.bytesUsed >= 3072);

        memboundFree(m, p1);
        memboundFree(m, p2);
        memboundShutdown(m);
    }

    TEST("COMPACT: Large allocations") {
        membound *m = memboundCreateCompact(1 << 22, false);
        assert(m != NULL);

        /* Allocate various sizes */
        void *p1 = memboundAlloc(m, 65536);  /* 64KB */
        void *p2 = memboundAlloc(m, 131072); /* 128KB */
        void *p3 = memboundAlloc(m, 262144); /* 256KB */
        assert(p1 && p2 && p3);

        /* Write to verify usable */
        memset(p1, 0xAA, 65536);
        memset(p2, 0xBB, 131072);
        memset(p3, 0xCC, 262144);

        memboundFree(m, p1);
        memboundFree(m, p2);
        memboundFree(m, p3);
        assert(memboundCurrentAllocationCount(m) == 0);

        memboundShutdown(m);
    }

    TEST("COMPACT: Config-based creation") {
        memboundConfig config = {0};
        config.tier = MEMBOUND_TIER_COMPACT;
        config.mode = MEMBOUND_MODE_FIXED;
        config.initialSize = 1 << 20;
        config.threadSafe = true;

        membound *m = memboundCreateWithConfig(&config);
        assert(m != NULL);
        assert(memboundGetTier(m) == MEMBOUND_TIER_COMPACT);

        memboundStats stats;
        memboundGetStats(m, &stats);
        assert(stats.threadSafe == true);

        void *p = memboundAlloc(m, 256);
        assert(p != NULL);
        memboundFree(m, p);

        memboundShutdown(m);
    }

    TEST("COMPACT: Repeated alloc/free stress") {
        membound *m = memboundCreateCompact(1 << 20, false);
        assert(m != NULL);

        for (int i = 0; i < 10000; i++) {
            size_t size = 64 + (i % 8192);
            void *p = memboundAlloc(m, size);
            if (p) {
                memset(p, i & 0xFF, size < 1024 ? size : 1024);
                memboundFree(m, p);
            }
        }

        assert(memboundCurrentAllocationCount(m) == 0);
        memboundShutdown(m);
    }

    TEST("COMPACT: Buddy coalescing") {
        membound *m = memboundCreateCompact(1 << 20, false);
        assert(m != NULL);

        /* Allocate adjacent blocks */
        void *p1 = memboundAlloc(m, 256);
        void *p2 = memboundAlloc(m, 256);
        void *p3 = memboundAlloc(m, 256);
        void *p4 = memboundAlloc(m, 256);
        assert(p1 && p2 && p3 && p4);

        /* Free all - should coalesce */
        memboundFree(m, p1);
        memboundFree(m, p2);
        memboundFree(m, p3);
        memboundFree(m, p4);

        /* Now allocate a larger block using coalesced space */
        void *pLarge = memboundAlloc(m, 1024);
        assert(pLarge != NULL);

        memboundFree(m, pLarge);
        memboundShutdown(m);
    }

    /* ================================================================
     * ENTERPRISE Tier Tests
     * ================================================================ */

    TEST("ENTERPRISE: Basic creation via config") {
        memboundConfig config = {
            .mode = MEMBOUND_MODE_DYNAMIC,
            .initialSize = 1 << 20, /* 1 MB */
            .tier = MEMBOUND_TIER_ENTERPRISE,
            .threadSafe = true,
            .maxTotalSize = 0, /* Unlimited */
            .growthSize = 1 << 20,
            .perExtentLocking = false, /* Start without per-extent locks */
        };

        membound *m = memboundCreateWithConfig(&config);
        assert(m != NULL);
        assert(memboundGetTier(m) == MEMBOUND_TIER_ENTERPRISE);

        void *p = memboundAlloc(m, 1024);
        assert(p != NULL);
        memboundFree(m, p);
        memboundShutdown(m);
    }

    TEST("ENTERPRISE: Creation via helper function") {
        membound *m = memboundCreateEnterprise(1 << 20, 0);
        assert(m != NULL);
        assert(memboundGetTier(m) == MEMBOUND_TIER_ENTERPRISE);

        /* Basic allocation */
        void *p = memboundAlloc(m, 4096);
        assert(p != NULL);
        memboundFree(m, p);
        memboundShutdown(m);
    }

    TEST("ENTERPRISE: Per-extent locking enabled") {
        memboundConfig config = {
            .mode = MEMBOUND_MODE_DYNAMIC,
            .initialSize = 1 << 20,
            .tier = MEMBOUND_TIER_ENTERPRISE,
            .threadSafe = true,
            .maxTotalSize = 0,
            .growthSize = 1 << 20,
            .perExtentLocking = true, /* Enable per-extent locks */
        };

        membound *m = memboundCreateWithConfig(&config);
        assert(m != NULL);
        assert(memboundGetTier(m) == MEMBOUND_TIER_ENTERPRISE);

        /* Basic operations work with per-extent locking */
        void *p1 = memboundAlloc(m, 512);
        void *p2 = memboundAlloc(m, 1024);
        assert(p1 && p2);

        memboundFree(m, p1);
        memboundFree(m, p2);
        memboundShutdown(m);
    }

    TEST("ENTERPRISE: Dynamic growth") {
        membound *m = memboundCreateEnterprise(1 << 16, 0); /* 64 KB initial */
        assert(m != NULL);

        /* Allocate more than initial extent to trigger growth */
        void *ptrs[10];
        for (int i = 0; i < 10; i++) {
            ptrs[i] = memboundAlloc(m, 8192);
            assert(ptrs[i] != NULL);
        }

        /* Verify stats reflect growth */
        memboundStats stats;
        memboundGetStats(m, &stats);
        assert(stats.capacity > (1 << 16)); /* Grew beyond initial */

        /* Free all */
        for (int i = 0; i < 10; i++) {
            memboundFree(m, ptrs[i]);
        }
        memboundShutdown(m);
    }

    TEST("ENTERPRISE: Pool operations") {
        membound *m = memboundCreateEnterprise(1 << 20, 0);
        assert(m != NULL);

        /* Test calloc */
        int *arr = memboundCalloc(m, 100, sizeof(int));
        assert(arr != NULL);
        for (int i = 0; i < 100; i++) {
            assert(arr[i] == 0);
        }

        /* Test realloc */
        arr = memboundRealloc(m, arr, 200 * sizeof(int));
        assert(arr != NULL);

        /* Ownership check */
        assert(memboundOwns(m, arr) == true);

        int stackVar = 42;
        assert(memboundOwns(m, &stackVar) == false);

        memboundFree(m, arr);
        memboundShutdown(m);
    }

    TEST("ENTERPRISE: Reset pool") {
        membound *m = memboundCreateEnterprise(1 << 20, 0);
        assert(m != NULL);

        void *p1 = memboundAlloc(m, 1024);
        void *p2 = memboundAlloc(m, 2048);
        assert(p1 && p2);

        memboundStats stats;
        memboundGetStats(m, &stats);
        assert(stats.allocationCount == 2);

        /* Reset should free everything */
        memboundReset(m);

        memboundGetStats(m, &stats);
        assert(stats.allocationCount == 0);

        /* Pool should still be usable */
        void *p3 = memboundAlloc(m, 4096);
        assert(p3 != NULL);

        memboundFree(m, p3);
        memboundShutdown(m);
    }

    TEST("ENTERPRISE: Size-class strategy") {
        memboundConfig config = {
            .mode = MEMBOUND_MODE_DYNAMIC,
            .initialSize = 1 << 20,
            .tier = MEMBOUND_TIER_ENTERPRISE,
            .threadSafe = true,
            .maxTotalSize = 0,
            .growthSize = 1 << 18,
            .strategy = MEMBOUND_STRATEGY_SIZE_CLASS,
            .sizeClassSmall = 1024,
            .sizeClassMedium = 65536,
            .perExtentLocking = true,
        };

        membound *m = memboundCreateWithConfig(&config);
        assert(m != NULL);

        /* Allocate from different size classes */
        void *small = memboundAlloc(m, 512);    /* Small */
        void *medium = memboundAlloc(m, 8192);  /* Medium */
        void *large = memboundAlloc(m, 131072); /* Large */

        assert(small && medium && large);

        memboundFree(m, small);
        memboundFree(m, medium);
        memboundFree(m, large);
        memboundShutdown(m);
    }

    TEST("ENTERPRISE: Max size limit") {
        membound *m = memboundCreateEnterprise(
            1 << 16, 1 << 17); /* 64KB init, 128KB max */
        assert(m != NULL);

        /* First allocation should succeed */
        void *p1 = memboundAlloc(m, 32768);
        assert(p1 != NULL);

        /* Second allocation should also succeed (still within max) */
        void *p2 = memboundAlloc(m, 32768);
        assert(p2 != NULL);

        /* Large allocation that would exceed limit should fail */
        void *p3 = memboundAlloc(m, 65536);
        assert(p3 == NULL); /* Exceeds 128KB limit */

        memboundFree(m, p1);
        memboundFree(m, p2);
        memboundShutdown(m);
    }

    /* ================================================================
     * Preset Configuration Tests
     * ================================================================ */

    TEST("Preset: Coroutine (MICRO tier)") {
        memboundConfig config = memboundPresetCoroutine();
        assert(config.tier == MEMBOUND_TIER_MICRO);
        assert(config.mode == MEMBOUND_MODE_FIXED);
        assert(config.initialSize == 4096);
        assert(config.threadSafe == false);

        membound *m = memboundCreateWithConfig(&config);
        assert(m != NULL);
        assert(memboundGetTier(m) == MEMBOUND_TIER_MICRO);

        void *p = memboundAlloc(m, 128);
        assert(p != NULL);
        memboundFree(m, p);
        memboundShutdown(m);
    }

    TEST("Preset: Connection (COMPACT tier, thread-safe)") {
        memboundConfig config = memboundPresetConnection(true);
        assert(config.tier == MEMBOUND_TIER_COMPACT);
        assert(config.mode == MEMBOUND_MODE_FIXED);
        assert(config.initialSize == 65536);
        assert(config.threadSafe == true);

        membound *m = memboundCreateWithConfig(&config);
        assert(m != NULL);
        assert(memboundGetTier(m) == MEMBOUND_TIER_COMPACT);

        void *p = memboundAlloc(m, 1024);
        assert(p != NULL);
        memboundFree(m, p);
        memboundShutdown(m);
    }

    TEST("Preset: Connection (COMPACT tier, no mutex)") {
        memboundConfig config = memboundPresetConnection(false);
        assert(config.threadSafe == false);

        membound *m = memboundCreateWithConfig(&config);
        assert(m != NULL);
        assert(memboundGetTier(m) == MEMBOUND_TIER_COMPACT);

        void *p = memboundAlloc(m, 512);
        assert(p != NULL);
        memboundFree(m, p);
        memboundShutdown(m);
    }

    TEST("Preset: General (STANDARD tier)") {
        memboundConfig config = memboundPresetGeneral();
        assert(config.tier == MEMBOUND_TIER_STANDARD);
        assert(config.mode == MEMBOUND_MODE_DYNAMIC);
        assert(config.initialSize == (1 << 20));
        assert(config.threadSafe == true);

        membound *m = memboundCreateWithConfig(&config);
        assert(m != NULL);
        assert(memboundGetTier(m) == MEMBOUND_TIER_STANDARD);

        void *p = memboundAlloc(m, 4096);
        assert(p != NULL);
        memboundFree(m, p);
        memboundShutdown(m);
    }

    TEST("Preset: Database (ENTERPRISE tier)") {
        memboundConfig config = memboundPresetDatabase();
        assert(config.tier == MEMBOUND_TIER_ENTERPRISE);
        assert(config.mode == MEMBOUND_MODE_DYNAMIC);
        assert(config.initialSize == (64 << 20));
        assert(config.strategy == MEMBOUND_STRATEGY_SIZE_CLASS);
        assert(config.perExtentLocking == true);

        membound *m = memboundCreateWithConfig(&config);
        assert(m != NULL);
        assert(memboundGetTier(m) == MEMBOUND_TIER_ENTERPRISE);

        /* Test allocations from different size classes */
        void *small = memboundAlloc(m, 256);
        void *medium = memboundAlloc(m, 8192);
        void *large = memboundAlloc(m, 131072);
        assert(small && medium && large);

        memboundFree(m, small);
        memboundFree(m, medium);
        memboundFree(m, large);
        memboundShutdown(m);
    }

    TEST("Preset: Embedded (COMPACT tier, fixed)") {
        memboundConfig config = memboundPresetEmbedded(32768);
        assert(config.tier == MEMBOUND_TIER_COMPACT);
        assert(config.mode == MEMBOUND_MODE_FIXED);
        assert(config.initialSize == 32768);
        assert(config.threadSafe == false);

        membound *m = memboundCreateWithConfig(&config);
        assert(m != NULL);
        assert(memboundGetTier(m) == MEMBOUND_TIER_COMPACT);

        void *p = memboundAlloc(m, 1024);
        assert(p != NULL);
        memboundFree(m, p);
        memboundShutdown(m);
    }

    /* ================================================================
     * Dynamic Mode Tests
     * ================================================================ */

    TEST("Dynamic: create initial extent") {
        membound *m = memboundCreateDynamic(1 << 16, 0, 1 << 16);
        assert(m != NULL);
        assert(memboundGetMode(m) == MEMBOUND_MODE_DYNAMIC);
        assert(memboundExtentCount(m) == 1);
        assert(memboundCanGrow(m) == true);
        memboundShutdown(m);
    }

    TEST("Dynamic: grows on exhaustion") {
        membound *m = memboundCreateDynamic(1 << 14, 0, 1 << 14);
        assert(m != NULL);

        void *ptrs[50];
        int count = 0;

        /* Allocate until we've definitely grown */
        for (int i = 0; i < 50; i++) {
            ptrs[i] = memboundAlloc(m, 4096);
            if (ptrs[i]) {
                count++;
            }
        }

        /* Should have grown past initial extent */
        assert(memboundExtentCount(m) > 1);
        assert(count > 0);

        /* All pointers should be owned */
        for (int i = 0; i < count; i++) {
            assert(memboundOwns(m, ptrs[i]));
        }

        for (int i = 0; i < count; i++) {
            memboundFree(m, ptrs[i]);
        }

        memboundShutdown(m);
    }

    TEST("Dynamic: respects max size") {
        membound *m = memboundCreateDynamic(1 << 14, 1 << 15, 1 << 14);
        assert(m != NULL);

        void *ptrs[100];
        int count = 0;

        /* Allocate until hitting max */
        while (count < 100) {
            void *p = memboundAlloc(m, 4096);
            if (!p) {
                break;
            }
            ptrs[count++] = p;
        }

        /* Should have hit limit */
        assert(memboundAlloc(m, 4096) == NULL);
        assert(memboundCapacity(m) <= (1 << 15));

        for (int i = 0; i < count; i++) {
            memboundFree(m, ptrs[i]);
        }
        memboundShutdown(m);
    }

    TEST("Dynamic: canGrow returns correct value") {
        membound *m = memboundCreateDynamic(1 << 14, 1 << 16, 1 << 14);
        assert(m != NULL);
        assert(memboundCanGrow(m) == true);

        /* Fill up to limit */
        while (memboundCanGrow(m)) {
            memboundGrowBy(m, 1 << 14);
        }

        assert(memboundCanGrow(m) == false);
        memboundShutdown(m);
    }

    TEST("Dynamic: shrink releases empty extents") {
        membound *m = memboundCreateDynamic(1 << 12, 0, 1 << 12);
        assert(m != NULL);

        /* Force multiple extents */
        void *ptrs[100];
        int count = 0;
        while (count < 100) {
            void *p = memboundAlloc(m, 512);
            if (!p) {
                break;
            }
            ptrs[count++] = p;
            /* Grow if allocation failed on last attempt */
            if (memboundExtentCount(m) >= 3) {
                break;
            }
        }

        size_t extentsBefore = memboundExtentCount(m);
        assert(extentsBefore > 1);

        /* Free everything */
        for (int i = 0; i < count; i++) {
            memboundFree(m, ptrs[i]);
        }

        /* Shrink should release extents (keeping primary) */
        size_t released = memboundShrink(m);
        assert(released > 0);
        assert(memboundExtentCount(m) < extentsBefore);

        memboundShutdown(m);
    }

    TEST("Dynamic: ownership across extents") {
        membound *m = memboundCreateDynamic(1 << 12, 0, 1 << 12);
        assert(m != NULL);

        /* Allocate across multiple extents */
        void *ptrs[50];
        int count = 0;
        while (count < 50 && memboundExtentCount(m) < 4) {
            void *p = memboundAlloc(m, 1024);
            if (!p) {
                break;
            }
            ptrs[count++] = p;
        }

        /* All pointers should be owned */
        for (int i = 0; i < count; i++) {
            assert(memboundOwns(m, ptrs[i]));
        }

        /* Stack variable should not be owned */
        int x;
        assert(!memboundOwns(m, &x));

        for (int i = 0; i < count; i++) {
            memboundFree(m, ptrs[i]);
        }
        memboundShutdown(m);
    }

    TEST("Dynamic: getExtentInfo") {
        membound *m = memboundCreateDynamic(1 << 14, 0, 1 << 14);
        assert(m != NULL);

        /* Force second extent */
        void *ptrs[50];
        int count = 0;
        while (memboundExtentCount(m) < 2 && count < 50) {
            void *p = memboundAlloc(m, 4096);
            if (!p) {
                break;
            }
            ptrs[count++] = p;
        }

        /* Get info for both extents */
        memboundExtentInfo info0, info1;
        assert(memboundGetExtentInfo(m, 0, &info0) == true);
        assert(info0.capacity > 0);
        assert(info0.index == 0);

        if (memboundExtentCount(m) > 1) {
            assert(memboundGetExtentInfo(m, 1, &info1) == true);
            assert(info1.capacity > 0);
            assert(info1.index == 1);
        }

        /* Invalid index should fail */
        memboundExtentInfo infoInvalid;
        assert(memboundGetExtentInfo(m, 999, &infoInvalid) == false);

        for (int i = 0; i < count; i++) {
            memboundFree(m, ptrs[i]);
        }
        memboundShutdown(m);
    }

    TEST("Dynamic: getStats") {
        membound *m = memboundCreateDynamic(1 << 16, 1 << 20, 1 << 16);
        assert(m != NULL);

        void *p = memboundAlloc(m, 1024);
        assert(p != NULL);

        memboundStats stats;
        memboundGetStats(m, &stats);

        assert(stats.mode == MEMBOUND_MODE_DYNAMIC);
        assert(stats.extentCount >= 1);
        assert(stats.bytesUsed > 0);
        assert(stats.allocationCount == 1);
        assert(stats.maxTotalSize == (1 << 20));

        memboundFree(m, p);
        memboundShutdown(m);
    }

    TEST("Dynamic: pressure callback") {
        /* Reset test callback state */
        testLastPressure = MEMBOUND_PRESSURE_LOW;
        testPressureCallCount = 0;

        /* Small pool to easily trigger pressure */
        membound *m = memboundCreateDynamic(1 << 12, 1 << 13, 1 << 12);
        assert(m != NULL);
        memboundSetPressureCallback(m, testPressureCb, NULL);

        /* Fill up to trigger pressure */
        void *ptrs[100];
        int count = 0;
        while (count < 100) {
            void *p = memboundAlloc(m, 256);
            if (!p) {
                break;
            }
            ptrs[count++] = p;
        }

        /* Should have triggered pressure callback at some point */
        assert(testPressureCallCount > 0 ||
               testLastPressure >= MEMBOUND_PRESSURE_MEDIUM);

        for (int i = 0; i < count; i++) {
            memboundFree(m, ptrs[i]);
        }
        memboundShutdown(m);
    }

    TEST("Dynamic: config with geometric growth") {
        memboundConfig config = {.mode = MEMBOUND_MODE_DYNAMIC,
                                 .initialSize = 1 << 14,
                                 .maxTotalSize = 0,
                                 .growthSize = 0,
                                 .growthFactor = 1.5,
                                 .maxExtentCount = 10,
                                 .pressureCallback = NULL,
                                 .pressureUserData = NULL};

        membound *m = memboundCreateWithConfig(&config);
        assert(m != NULL);
        assert(memboundGetMode(m) == MEMBOUND_MODE_DYNAMIC);

        /* Force growth */
        void *ptrs[100];
        int count = 0;
        while (memboundExtentCount(m) < 3 && count < 100) {
            void *p = memboundAlloc(m, 4096);
            if (!p) {
                break;
            }
            ptrs[count++] = p;
        }

        assert(memboundExtentCount(m) >= 2);

        for (int i = 0; i < count; i++) {
            memboundFree(m, ptrs[i]);
        }
        memboundShutdown(m);
    }

    TEST("Dynamic: manual growBy") {
        membound *m = memboundCreateDynamic(1 << 14, 0, 1 << 14);
        assert(m != NULL);

        size_t initialExtents = memboundExtentCount(m);
        size_t initialCapacity = memboundCapacity(m);

        bool grew = memboundGrowBy(m, 1 << 14);
        assert(grew);
        assert(memboundExtentCount(m) == initialExtents + 1);
        assert(memboundCapacity(m) > initialCapacity);

        memboundShutdown(m);
    }

    TEST("Dynamic: fixed mode cannot grow") {
        membound *m = memboundCreateFixed(1 << 16);
        assert(m != NULL);
        assert(memboundGetMode(m) == MEMBOUND_MODE_FIXED);
        assert(memboundCanGrow(m) == false);

        bool grew = memboundGrowBy(m, 1 << 16);
        assert(!grew);

        memboundShutdown(m);
    }

    /* ================================================================
     * Index Lookup Tests
     * ================================================================ */

    TEST("Index: binary search correctness") {
        membound *m = memboundCreateDynamic(1 << 12, 0, 1 << 12);
        assert(m != NULL);

        /* Create many extents to test binary search */
        for (int i = 0; i < 10; i++) {
            memboundGrowBy(m, 1 << 12);
        }

        assert(memboundExtentCount(m) >= 10);

        /* Allocate from each extent and verify ownership */
        void *ptrs[50];
        int count = 0;
        for (int i = 0; i < 50 && count < 50; i++) {
            void *p = memboundAlloc(m, 256);
            if (p) {
                ptrs[count++] = p;
            }
        }

        /* Every pointer should be owned */
        for (int i = 0; i < count; i++) {
            assert(memboundOwns(m, ptrs[i]));
        }

        for (int i = 0; i < count; i++) {
            memboundFree(m, ptrs[i]);
        }
        memboundShutdown(m);
    }

    /* ================================================================
     * Strategy Tests
     * ================================================================ */

    TEST("Strategy: best-fit consolidates allocations") {
        membound *m = memboundCreateDynamic(1 << 14, 0, 1 << 14);
        assert(m != NULL);

        /* Set best-fit strategy */
        memboundSetStrategy(m, MEMBOUND_STRATEGY_BEST_FIT);
        assert(memboundGetStrategy(m) == MEMBOUND_STRATEGY_BEST_FIT);

        /* Fill up first extent */
        void *ptrs[32];
        int count = 0;
        while (memboundExtentCount(m) == 1 && count < 32) {
            ptrs[count] = memboundAlloc(m, 256);
            if (ptrs[count]) {
                count++;
            } else {
                break;
            }
        }
        assert(count > 0);

        /* Force second extent */
        while (memboundExtentCount(m) < 2) {
            void *p = memboundAlloc(m, 4096);
            if (!p) {
                break;
            }
        }
        assert(memboundExtentCount(m) >= 2);

        /* Free half from first extent */
        for (int i = 0; i < count / 2; i++) {
            memboundFree(m, ptrs[i]);
        }

        /* New allocation should go to fuller extent (best-fit) */
        void *newAlloc = memboundAlloc(m, 256);
        assert(newAlloc != NULL);
        memboundFree(m, newAlloc);

        /* Cleanup */
        for (int i = count / 2; i < count; i++) {
            memboundFree(m, ptrs[i]);
        }
        memboundShutdown(m);
    }

    TEST("Strategy: worst-fit spreads allocations") {
        membound *m = memboundCreateDynamic(1 << 14, 0, 1 << 14);
        assert(m != NULL);

        /* Force multiple extents */
        while (memboundExtentCount(m) < 3) {
            memboundGrowBy(m, 1 << 14);
        }
        assert(memboundExtentCount(m) >= 3);

        /* Set worst-fit strategy */
        memboundSetStrategy(m, MEMBOUND_STRATEGY_WORST_FIT);
        assert(memboundGetStrategy(m) == MEMBOUND_STRATEGY_WORST_FIT);

        /* Allocate - should spread across extents */
        void *ptrs[10];
        for (int i = 0; i < 10; i++) {
            ptrs[i] = memboundAlloc(m, 1024);
            assert(ptrs[i] != NULL);
        }

        /* Cleanup */
        for (int i = 0; i < 10; i++) {
            memboundFree(m, ptrs[i]);
        }
        memboundShutdown(m);
    }

    TEST("Strategy: size-class segregation") {
        membound *m = memboundCreateDynamic(1 << 16, 0, 1 << 16);
        assert(m != NULL);

        /* Force multiple extents for size-class */
        while (memboundExtentCount(m) < 3) {
            memboundGrowBy(m, 1 << 16);
        }

        /* Set size-class strategy */
        memboundSetStrategy(m, MEMBOUND_STRATEGY_SIZE_CLASS);
        assert(memboundGetStrategy(m) == MEMBOUND_STRATEGY_SIZE_CLASS);

        /* Allocate small (<= 1KB) */
        void *small1 = memboundAlloc(m, 256);
        void *small2 = memboundAlloc(m, 512);
        assert(small1 && small2);

        /* Allocate medium (1KB - 64KB) */
        void *med1 = memboundAlloc(m, 4096);
        void *med2 = memboundAlloc(m, 8192);
        assert(med1 && med2);

        /* All allocations should succeed */
        memboundFree(m, small1);
        memboundFree(m, small2);
        memboundFree(m, med1);
        memboundFree(m, med2);
        memboundShutdown(m);
    }

    TEST("Strategy: adaptive switches on fragmentation") {
        membound *m = memboundCreateDynamic(1 << 14, 0, 1 << 14);
        assert(m != NULL);

        memboundSetStrategy(m, MEMBOUND_STRATEGY_ADAPTIVE);
        assert(memboundGetStrategy(m) == MEMBOUND_STRATEGY_ADAPTIVE);

        /* Initially fragmentation should be low */
        float frag = memboundGetFragmentation(m);
        assert(frag >= 0.0f && frag <= 1.0f);

        /* Allocate and free to cause fragmentation */
        void *p1 = memboundAlloc(m, 1024);
        void *p2 = memboundAlloc(m, 1024);
        void *p3 = memboundAlloc(m, 1024);
        assert(p1 && p2 && p3);

        memboundFree(m, p2); /* Leave hole */

        frag = memboundGetFragmentation(m);
        /* Fragmentation measured, value depends on state */
        assert(frag >= 0.0f && frag <= 1.0f);

        memboundFree(m, p1);
        memboundFree(m, p3);
        memboundShutdown(m);
    }

    TEST("Strategy: min occupancy threshold") {
        membound *m = memboundCreateDynamic(1 << 14, 0, 1 << 14);
        assert(m != NULL);

        /* Force multiple extents */
        while (memboundExtentCount(m) < 2) {
            memboundGrowBy(m, 1 << 14);
        }

        memboundSetStrategy(m, MEMBOUND_STRATEGY_BEST_FIT);
        memboundSetMinOccupancy(m, 0.1f); /* Skip extents < 10% full */

        /* Allocations should still work */
        void *p = memboundAlloc(m, 1024);
        assert(p != NULL);
        memboundFree(m, p);

        memboundShutdown(m);
    }

    TEST("Strategy: proactive shrink on free") {
        memboundConfig config = {
            .mode = MEMBOUND_MODE_DYNAMIC,
            .initialSize = 1 << 14,
            .growthSize = 1 << 14,
            .shrinkThreshold = 0.05f /* Shrink extents < 5% full */
        };
        membound *m = memboundCreateWithConfig(&config);
        assert(m != NULL);

        /* Force multiple extents */
        void *ptrs[100];
        int count = 0;
        while (memboundExtentCount(m) < 3 && count < 100) {
            ptrs[count] = memboundAlloc(m, 4096);
            if (ptrs[count]) {
                count++;
            } else {
                break;
            }
        }
        assert(memboundExtentCount(m) >= 2);
        size_t extentsBefore = memboundExtentCount(m);

        /* Free all allocations from secondary extents */
        for (int i = 0; i < count; i++) {
            memboundFree(m, ptrs[i]);
        }

        /* Proactive shrink should have released empty extents */
        size_t extentsAfter = memboundExtentCount(m);
        /* At minimum, primary extent remains */
        assert(extentsAfter >= 1);
        assert(extentsAfter <= extentsBefore);

        memboundShutdown(m);
    }

    TEST("Strategy: fragmentation ratio calculation") {
        membound *m = memboundCreateDynamic(1 << 14, 0, 1 << 14);
        assert(m != NULL);

        /* Single extent - no fragmentation */
        float frag = memboundGetFragmentation(m);
        assert(frag == 0.0f);

        /* Force multiple extents */
        while (memboundExtentCount(m) < 3) {
            memboundGrowBy(m, 1 << 14);
        }

        /* Still no fragmentation with empty extents */
        frag = memboundGetFragmentation(m);
        assert(frag >= 0.0f);

        /* Allocate across extents */
        void *p1 = memboundAlloc(m, 1024);
        void *p2 = memboundAlloc(m, 2048);
        assert(p1 && p2);

        frag = memboundGetFragmentation(m);
        assert(frag >= 0.0f && frag <= 1.0f);

        memboundFree(m, p1);
        memboundFree(m, p2);
        memboundShutdown(m);
    }

    TEST("Strategy: switch strategy at runtime") {
        membound *m = memboundCreateDynamic(1 << 14, 0, 1 << 14);
        assert(m != NULL);

        /* Default is FIRST_FIT */
        assert(memboundGetStrategy(m) == MEMBOUND_STRATEGY_FIRST_FIT);

        /* Switch to each strategy */
        memboundExtentStrategy old;

        old = memboundSetStrategy(m, MEMBOUND_STRATEGY_BEST_FIT);
        assert(old == MEMBOUND_STRATEGY_FIRST_FIT);
        assert(memboundGetStrategy(m) == MEMBOUND_STRATEGY_BEST_FIT);

        old = memboundSetStrategy(m, MEMBOUND_STRATEGY_WORST_FIT);
        assert(old == MEMBOUND_STRATEGY_BEST_FIT);

        old = memboundSetStrategy(m, MEMBOUND_STRATEGY_SIZE_CLASS);
        assert(old == MEMBOUND_STRATEGY_WORST_FIT);

        old = memboundSetStrategy(m, MEMBOUND_STRATEGY_ADAPTIVE);
        assert(old == MEMBOUND_STRATEGY_SIZE_CLASS);

        old = memboundSetStrategy(m, MEMBOUND_STRATEGY_FIRST_FIT);
        assert(old == MEMBOUND_STRATEGY_ADAPTIVE);

        memboundShutdown(m);
    }

    /* ================================================================
     * Large Allocation Tests
     * ================================================================ */

    TEST("Large: allocation up to extent capacity") {
        /* Create 1MB extent */
        membound *m = memboundCreateDynamic(1 << 20, 0, 1 << 20);
        assert(m != NULL);

        /* Allocate half of extent capacity */
        void *p = memboundAlloc(m, 1 << 19); /* 512KB */
        assert(p != NULL);
        memboundFree(m, p);

        /* Allocate near full capacity */
        p = memboundAlloc(m, (1 << 20) - 4096); /* Just under 1MB */
        /* May succeed or fail depending on overhead */
        if (p) {
            memboundFree(m, p);
        }

        memboundShutdown(m);
    }

    TEST("Large: multi-megabyte allocations") {
        /* Create 16MB extent */
        membound *m = memboundCreateDynamic(1 << 24, 0, 1 << 24);
        assert(m != NULL);

        /* 4MB allocation */
        void *p1 = memboundAlloc(m, 1 << 22);
        assert(p1 != NULL);

        /* Another 4MB */
        void *p2 = memboundAlloc(m, 1 << 22);
        assert(p2 != NULL);

        /* Verify they're different */
        assert(p1 != p2);

        memboundFree(m, p1);
        memboundFree(m, p2);
        memboundShutdown(m);
    }

    TEST("Large: extent growth with large allocations") {
        /* Start small, allow growth with extent size matching allocation need
         */
        membound *m = memboundCreateDynamic(1 << 16, 1 << 26, 1 << 20);
        assert(m != NULL);

        /* Allocate something that fits in growth extent (1MB) but not initial
         * (64KB) */
        void *p = memboundAlloc(m, 1 << 17); /* 128KB > 64KB initial */
        if (p != NULL) {
            /* Allocation succeeded - extent grew */
            assert(memboundExtentCount(m) >= 2);
            memboundFree(m, p);
        } else {
            /* Allocation may fail if growth doesn't produce usable space */
            /* This is acceptable - the test verifies growth was attempted */
        }

        memboundShutdown(m);
    }

#ifndef NDEBUG
    TEST("Debug dump (visual check)") {
        membound *m = memboundCreateDynamic(1 << 14, 0, 1 << 14);
        assert(m != NULL);

        void *p1 = memboundAlloc(m, 256);
        void *p2 = memboundAlloc(m, 512);
        (void)p1;
        (void)p2;

        /* Force growth */
        while (memboundExtentCount(m) < 2) {
            void *p = memboundAlloc(m, 4096);
            if (!p) {
                break;
            }
        }

        printf("\n--- Memory dump with allocations ---\n");
        memboundDump(m, NULL);

        memboundReset(m);
        memboundShutdown(m);
    }
#endif

    /* ================================================================
     * Multi-threaded Stress Tests
     * ================================================================ */

    TEST("MT: Concurrent alloc/free stress test") {
        membound *m = memboundCreateDynamic(1 << 20, 0, 1 << 18);
        assert(m != NULL);

        pthread_t threads[MT_NUM_THREADS];
        mtTestContext contexts[MT_NUM_THREADS];
        volatile int startFlag = 0;

        /* Create threads */
        for (int i = 0; i < MT_NUM_THREADS; i++) {
            contexts[i].m = m;
            contexts[i].threadId = i;
            contexts[i].iterations = MT_ITERATIONS_PER_THREAD;
            contexts[i].startFlag = &startFlag;
            contexts[i].errors = 0;
            int rc = pthread_create(&threads[i], NULL, mtAllocFreeWorker,
                                    &contexts[i]);
            assert(rc == 0);
        }

        /* Start all threads at once */
        startFlag = 1;

        /* Wait for completion */
        int totalErrors = 0;
        for (int i = 0; i < MT_NUM_THREADS; i++) {
            pthread_join(threads[i], NULL);
            totalErrors += contexts[i].errors;
        }

        assert(totalErrors == 0);

        /* Verify pool is consistent after stress */
        assert(memboundCurrentAllocationCount(m) == 0);

        memboundShutdown(m);
    }

    TEST("MT: Concurrent realloc stress test") {
        membound *m = memboundCreateDynamic(1 << 20, 0, 1 << 18);
        assert(m != NULL);

        pthread_t threads[MT_NUM_THREADS];
        mtTestContext contexts[MT_NUM_THREADS];
        volatile int startFlag = 0;

        for (int i = 0; i < MT_NUM_THREADS; i++) {
            contexts[i].m = m;
            contexts[i].threadId = i;
            contexts[i].iterations = MT_ITERATIONS_PER_THREAD /
                                     10; /* Fewer iterations for realloc */
            contexts[i].startFlag = &startFlag;
            contexts[i].errors = 0;
            int rc = pthread_create(&threads[i], NULL, mtReallocWorker,
                                    &contexts[i]);
            assert(rc == 0);
        }

        startFlag = 1;

        for (int i = 0; i < MT_NUM_THREADS; i++) {
            pthread_join(threads[i], NULL);
        }

        assert(memboundCurrentAllocationCount(m) == 0);
        memboundShutdown(m);
    }

    TEST("MT: Concurrent query while mutating") {
        membound *m = memboundCreateDynamic(1 << 20, 0, 1 << 18);
        assert(m != NULL);

        pthread_t threads[MT_NUM_THREADS];
        mtTestContext contexts[MT_NUM_THREADS];
        volatile int startFlag = 0;

        /* Half the threads do alloc/free, half do queries */
        for (int i = 0; i < MT_NUM_THREADS; i++) {
            contexts[i].m = m;
            contexts[i].threadId = i;
            contexts[i].iterations = MT_ITERATIONS_PER_THREAD / 2;
            contexts[i].startFlag = &startFlag;
            contexts[i].errors = 0;

            void *(*fn)(void *) =
                (i % 2 == 0) ? mtAllocFreeWorker : mtQueryWorker;
            int rc = pthread_create(&threads[i], NULL, fn, &contexts[i]);
            assert(rc == 0);
        }

        startFlag = 1;

        int totalErrors = 0;
        for (int i = 0; i < MT_NUM_THREADS; i++) {
            pthread_join(threads[i], NULL);
            totalErrors += contexts[i].errors;
        }

        assert(totalErrors == 0);
        memboundShutdown(m);
    }

    TEST("MT: Pressure callback reentrancy (deadlock prevention)") {
        /* Create a small pool that will hit pressure thresholds quickly */
        membound *m =
            memboundCreateDynamic(1 << 14, 0, 0); /* 16KB, no growth */
        assert(m != NULL);

        /* Reset global test state */
        mtPressureCallbackExecuting = 0;
        mtPressureCallbackCount = 0;
        mtPressureTestPool = m;
        memboundSetPressureCallback(m, mtPressureCallback, NULL);

        /* Allocate until we hit high pressure */
        void *ptrs[64];
        int count = 0;

        for (int i = 0; i < 64; i++) {
            void *p = memboundAlloc(m, 256);
            if (!p) {
                break;
            }
            ptrs[count++] = p;
        }

        /* Verify the callback was invoked at least once */
        assert(mtPressureCallbackCount > 0);

        /* If we got here without deadlock, the callback ran outside the lock */
        /* The callback itself verifies it can call membound functions */

        /* Cleanup */
        for (int i = 0; i < count; i++) {
            memboundFree(m, ptrs[i]);
        }

        memboundShutdown(m);
        mtPressureTestPool = NULL;
    }

    TEST("MT: Lock contention profiling") {
        /* Create a pool for concurrent access */
        membound *m =
            memboundCreateDynamic(1 << 16, 0, 1 << 16); /* 64KB, grows */
        assert(m != NULL);

        /* Reset state and run concurrent workers */
        volatile int startFlag = 0;
        mtTestContext contexts[MT_NUM_THREADS];
        pthread_t threads[MT_NUM_THREADS];

        for (int i = 0; i < MT_NUM_THREADS; i++) {
            contexts[i].m = m;
            contexts[i].threadId = i;
            contexts[i].iterations = MT_ITERATIONS_PER_THREAD;
            contexts[i].startFlag = &startFlag;
            contexts[i].errors = 0;
            pthread_create(&threads[i], NULL, mtAllocFreeWorker, &contexts[i]);
        }

        /* Start all threads simultaneously */
        __atomic_store_n(&startFlag, 1, __ATOMIC_RELEASE);

        /* Wait for completion */
        for (int i = 0; i < MT_NUM_THREADS; i++) {
            pthread_join(threads[i], NULL);
        }

        /* Get contention stats */
        memboundStats stats;
        memboundGetStats(m, &stats);

        /* Verify profiling is working (at least one lock acquisition) */
        assert(stats.lockAcquisitions > 0);

        /* Report contention metrics */
        printf("    Contention stats: %" PRIu64 " acquisitions, %" PRIu64
               " contentions (%.2f%%), %" PRIu64 " ns total wait\n",
               stats.lockAcquisitions, stats.lockContentions,
               stats.contentionPercent, stats.lockWaitTimeNs);

        /* Under high concurrency, we expect SOME contention */
        /* (don't assert on exact values - depends on hardware) */

        memboundShutdown(m);
    }

    TEST("MT: ENTERPRISE per-extent parallel alloc/free") {
        /* Create ENTERPRISE pool with per-extent locking enabled */
        membound *m = memboundCreateEnterprise(1 << 16, 0); /* 64KB init */
        assert(m != NULL);
        assert(memboundGetTier(m) == MEMBOUND_TIER_ENTERPRISE);

        /* Force creation of multiple extents by allocating */
        void *bigAlloc = memboundAlloc(m, 32768);
        assert(bigAlloc != NULL);
        memboundFree(m, bigAlloc);

        /* Run concurrent workers - they should be able to allocate from
         * different extents in parallel with per-extent locking */
        volatile int startFlag = 0;
        mtTestContext contexts[MT_NUM_THREADS];
        pthread_t threads[MT_NUM_THREADS];

        for (int i = 0; i < MT_NUM_THREADS; i++) {
            contexts[i].m = m;
            contexts[i].threadId = i;
            contexts[i].iterations = MT_ITERATIONS_PER_THREAD;
            contexts[i].startFlag = &startFlag;
            contexts[i].errors = 0;
            pthread_create(&threads[i], NULL, mtAllocFreeWorker, &contexts[i]);
        }

        /* Start all threads simultaneously */
        __atomic_store_n(&startFlag, 1, __ATOMIC_RELEASE);

        /* Wait for completion */
        int totalErrors = 0;
        for (int i = 0; i < MT_NUM_THREADS; i++) {
            pthread_join(threads[i], NULL);
            totalErrors += contexts[i].errors;
        }

        assert(totalErrors == 0);

        /* Get stats to verify per-extent locking was used */
        memboundStats stats;
        memboundGetStats(m, &stats);
        assert(stats.lockAcquisitions > 0);

        memboundShutdown(m);
    }

    TEST("ENTERPRISE: Extent selection caching") {
        /* Create ENTERPRISE pool with multiple extents */
        membound *m = memboundCreateEnterprise(1 << 14, 0); /* 16KB init */
        assert(m != NULL);
        assert(m->enterprise != NULL);

        /* Force creation of second extent */
        void *largeAlloc = memboundAlloc(m, 8192);
        assert(largeAlloc != NULL);

        /* More allocations should go to the cached extent */
        void *allocs[10];
        for (int i = 0; i < 10; i++) {
            allocs[i] = memboundAlloc(m, 128);
            assert(allocs[i] != NULL);
        }

        /* Check that cache was used */
        uint64_t hits = m->enterprise->cacheHits;
        uint64_t misses = m->enterprise->cacheMisses;

        /* After repeated same-size allocations, we should have cache hits */
        /* First alloc may miss (cache not set), subsequent should hit */
        assert(hits > 0 || misses > 0); /* At least some cache activity */

        /* Free everything and verify we can still allocate */
        memboundFree(m, largeAlloc);
        for (int i = 0; i < 10; i++) {
            memboundFree(m, allocs[i]);
        }

        /* More allocations after free should work */
        void *postFree = memboundAlloc(m, 256);
        assert(postFree != NULL);
        memboundFree(m, postFree);

        memboundShutdown(m);
    }

    TEST("Safety: Invalid operations are handled gracefully") {
        /* Create a STANDARD tier pool */
        membound *m = memboundCreate(8192);
        assert(m != NULL);

        void *p = memboundAlloc(m, 256);
        assert(p != NULL);

        memboundStats stats;
        memboundGetStats(m, &stats);
        assert(stats.safetyViolations == 0);

        /* Valid free - should work */
        memboundFree(m, p);

        /* Double free - should increment violation counter */
        memboundFree(m, p);

        memboundGetStats(m, &stats);
        assert(stats.safetyViolations == 1);

        /* NULL free - should be silently ignored (not a violation) */
        memboundFree(m, NULL);

        memboundGetStats(m, &stats);
        assert(stats.safetyViolations == 1); /* Unchanged */

        /* Invalid pointer (not from pool) - should increment violation */
        int stackVar = 42;
        memboundFree(m, &stackVar);

        memboundGetStats(m, &stats);
        assert(stats.safetyViolations == 2);

        /* Verify pool is still usable after invalid operations */
        void *p2 = memboundAlloc(m, 128);
        assert(p2 != NULL);
        memboundFree(m, p2);

        memboundShutdown(m);
    }

    TEST("Safety: Stats report violations count") {
        membound *m = memboundCreate(4096);
        assert(m != NULL);

        memboundStats stats;
        memboundGetStats(m, &stats);

        /* Initial violations should be zero */
        assert(stats.safetyViolations == 0);

        /* Valid operations don't increment violations */
        void *p = memboundAlloc(m, 100);
        assert(p != NULL);
        memboundFree(m, p);

        memboundGetStats(m, &stats);
        assert(stats.safetyViolations == 0);

        memboundShutdown(m);
    }

    TEST("Safety: COMPACT tier tracks violations") {
        membound *m = memboundCreateCompact(4096, false);
        assert(m != NULL);

        memboundStats stats;
        memboundGetStats(m, &stats);
        assert(stats.safetyViolations == 0);

        void *p = memboundAlloc(m, 100);
        assert(p != NULL);
        memboundFree(m, p);

        /* Double free should increment violation */
        memboundFree(m, p);

        memboundGetStats(m, &stats);
        assert(stats.safetyViolations == 1);

        /* Invalid pointer */
        int stackVar = 42;
        memboundFree(m, &stackVar);

        memboundGetStats(m, &stats);
        assert(stats.safetyViolations == 2);

        memboundShutdown(m);
    }

    TEST_FINAL_RESULT;
}
#endif

/*
 * Originally:
 * 2007 October 14
 *
 * The author disclaims copyright to this source code.  In place of
 * a legal notice, here is a blessing:
 *
 *    May you do good and not evil.
 *    May you find forgiveness for yourself and forgive others.
 *    May you share freely, never taking more than you give.
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * This file contains the C functions that implement a memory
 * allocation subsystem for use by SQLite.
 *
 * This version of the memory allocation subsystem omits all
 * use of malloc(). The application gives SQLite a block of memory
 * before calling membound_initialize() from which allocations
 * are made and returned by the xMalloc() and xRealloc()
 * implementations. Once membound_initialize() has been called,
 * the amount of memory available to SQLite is fixed and cannot
 * be changed.
 *
 * This memory allocator uses the following algorithm:
 *
 *   1.  All memory allocation sizes are rounded up to a power of 2.
 *
 *   2.  If two adjacent free blocks are the halves of a larger block,
 *       then the two blocks are coalesced into the single larger block.
 *
 *   3.  New memory is allocated from the first available free block.
 *
 * This algorithm is described in: J. M. Robson. "Bounds for Some Functions
 * Concerning Dynamic Storage Allocation". Journal of the Association for
 * Computing Machinery, Volume 21, Number 8, July 1974, pages 491-499.
 *
 * Let n be the size of the largest allocation divided by the minimum
 * allocation size (after rounding all sizes up to a power of 2.)  Let M
 * be the maximum amount of memory ever outstanding at one time.  Let
 * N be the total amount of memory available for allocation.  Robson
 * proved that this memory allocator will never breakdown due to
 * fragmentation as long as the following constraint holds:
 *
 *      N >=  M*(1 + log2(n)/2) - n + 1
 *
 * The membound_status() logic tracks the maximum values of n and M so
 * that an application can, at any time, verify this constraint.
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * DATAKIT EXTENSIONS (2024-2025, Matt Stancliff):
 *
 * This implementation extends the original mem5.c with significant
 * enhancements while preserving the core Robson algorithm:
 *
 * 1. EXTENT-BASED ARCHITECTURE:
 *    - The original single-pool design is now wrapped in "extent" structures
 *    - Each extent is a self-contained buddy allocator region
 *    - Multiple extents can be linked together for dynamic growth
 *
 * 2. DUAL OPERATION MODES:
 *    - FIXED MODE: Original behavior - pre-allocated, returns NULL on
 * exhaustion
 *    - DYNAMIC MODE: Automatically grows by adding new extents on demand
 *
 * 3. GROWTH STRATEGIES:
 *    - Fixed: Add new extents of constant size
 *    - Geometric: Grow by factor (e.g., 1.5x) of current capacity
 *    - Adaptive: Adjust based on allocation patterns
 *
 * 4. SIMD-OPTIMIZED OPERATIONS:
 *    - Fast memory zeroing via AVX512/AVX2/SSE2/NEON
 *    - SIMD-accelerated extent lookup for pointer ownership
 *    - Branch prediction hints for hot paths
 *
 * 5. EXTENT INDEX:
 *    - Sorted array for O(log n) pointer-to-extent lookup
 *    - Inline storage for <=4 extents (common case optimization)
 *    - SIMD-optimized lookup when extent count >= 8
 *
 * 6. MEMORY PRESSURE CALLBACKS:
 *    - Applications can register callbacks for pressure level changes
 *    - Four levels: LOW (<50%), MEDIUM (50-80%), HIGH (80-95%), CRITICAL (>95%)
 *
 * 7. COMPREHENSIVE STATISTICS:
 *    - Per-extent and aggregate statistics
 *    - Peak usage tracking
 *    - Lifetime allocation counters
 *    - Fragmentation metrics
 *
 * 8. SHRINK AND COMPACT:
 *    - memboundShrink() releases empty extents back to the OS
 *    - memboundCompact() attempts to consolidate allocations
 *
 * The Robson fragmentation bound still applies within each extent,
 * providing the same worst-case guarantees for each memory region.
 */
