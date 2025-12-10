#include "membound.h"

/* This was originally 'mem5.c' from sqlite3. */

#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* SIMD support for optimized memory operations */
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

/* Maximum size of any individual allocation request is ((1<<LOGMAX)*szAtom).
 * Since szAtom is always at least 8 and 32-bit integers are used,
 * it is not actually possible to reach this limit. */
#define MEMBOUND_LOGMAX 30

struct membound {
    /* Memory available for allocation */
    uint8_t *zPool;     /* Memory available to be allocated */
    int64_t szAtom;     /* Smallest possible allocation in bytes */
    uint32_t atomShift; /* log2(szAtom) for fast division via shift */
    uint64_t size;      /* Byte extent of 'zPool' */
    int64_t nBlock;     /* Number of szAtom sized blocks in zPool */

    /* Mutex to control access to the memory allocation subsystem. */
    pthread_mutex_t mutex;

    /* Performance statistics */
    uint64_t nAlloc;     /* Total number of calls to malloc */
    uint64_t totalAlloc; /* Total of all malloc calls including internal frag */
    uint64_t totalExcess;  /* Total internal fragmentation */
    uint64_t currentOut;   /* Current checkout, including internal frag */
    uint64_t currentCount; /* Current number of distinct checkouts */
    uint64_t maxOut;       /* Maximum instantaneous currentOut */
    uint64_t maxCount;     /* Maximum instantaneous currentCount */
#if MEMBOUND_DEBUG
    uint64_t maxRequest; /* Largest allocation (exclusive of internal frag) */
#endif

    /* Lists of free blocks.  aiFreelist[0] is a list of free blocks of
     * size szAtom.  aiFreelist[1] holds blocks of size szAtom*2.
     * aiFreelist[2] holds free blocks of size szAtom*4.  And so forth. */
    int32_t aiFreelist[MEMBOUND_LOGMAX + 1];

    /* Bitmap indicating which size classes have free blocks.
     * Bit i is set if aiFreelist[i] >= 0 (has at least one free block).
     * Enables O(1) search for available blocks using CTZ. */
    uint64_t freelistBitmap;

    /* Space for tracking which blocks are checked out and the size
     * of each block. One byte per block. */
    uint8_t *aCtrl;
};

#ifndef NDEBUG
static void memboundMemBoundDumpLock(membound *m, const char *zFilename,
                                     bool useLock);
#endif

/* Forward declaration for fast log2 */
static inline int32_t memboundLog(int iValue);

/* A minimum allocation is an instance of the following structure.
 * Larger allocations are an array of these structures where the
 * size of the array is a power of 2.
 *
 * The size of this object must be a power of two.  That fact is
 * verified in memboundInit().  */
typedef struct MemboundLink {
    int32_t next; /* Index of next free chunk */
    int32_t prev; /* Index of previous free chunk */
} MemboundLink;

/* The size of a MemboundLink object must be a power of two.  Verify that
 * this is case.  */
_Static_assert(sizeof(MemboundLink) == 8, "MemboundLink changed size class?");

/* Masks used for m->aCtrl[] elements. */
enum memboundCtrl {
    MEMBOUND_CTRL_LOGSIZE = 0x1f, /* Log2 Size of this block */
    MEMBOUND_CTRL_FREE = 0x20     /* True if not checked out */
} memboundCtrl;

/* Assuming m->zPool is divided up into an array of MemboundLink
 * structures, return a pointer to the idx-th such link. */
#define MEMBOUND_LINK(m, idx)                                                  \
    ((MemboundLink *)(&(m)->zPool[(idx) * (m)->szAtom]))

/* Unlink the chunk at m->aPool[i] from list it is currently
 * on.  It should be found on m->aiFreelist[iLogsize]. */
static void memboundUnlink(membound *m, int i, int32_t iLogsize) {
    assert(i >= 0 && i < m->nBlock);
    assert(iLogsize >= 0 && iLogsize <= MEMBOUND_LOGMAX);
    assert((m->aCtrl[i] & MEMBOUND_CTRL_LOGSIZE) == iLogsize);

    const int32_t next = MEMBOUND_LINK(m, i)->next;
    const int32_t prev = MEMBOUND_LINK(m, i)->prev;
    if (prev < 0) {
        m->aiFreelist[iLogsize] = next;
        /* If freelist is now empty, clear bitmap bit */
        if (next < 0) {
            m->freelistBitmap &= ~(1ULL << iLogsize);
        }
    } else {
        MEMBOUND_LINK(m, prev)->next = next;
    }

    if (next >= 0) {
        MEMBOUND_LINK(m, next)->prev = prev;
    }
}

/* Link the chunk at m->aPool[i] so that is on the iLogsize
 * free list. */
static void memboundLink(membound *m, int i, int32_t iLogsize) {
#if 0
    assert(membound_mutex_held(&m->mutex));
#endif
    assert(i >= 0 && i < m->nBlock);
    assert(iLogsize >= 0 && iLogsize <= MEMBOUND_LOGMAX);
    assert((m->aCtrl[i] & MEMBOUND_CTRL_LOGSIZE) == iLogsize);

    const int32_t x = MEMBOUND_LINK(m, i)->next = m->aiFreelist[iLogsize];
    MEMBOUND_LINK(m, i)->prev = -1;
    if (x >= 0) {
        assert(x < m->nBlock);
        MEMBOUND_LINK(m, x)->prev = i;
    }

    m->aiFreelist[iLogsize] = i;

    /* Update bitmap: mark this size class as having free blocks */
    m->freelistBitmap |= (1ULL << iLogsize);
}

/* Obtain or release the mutex needed to access global data structures. */
#if 1
static void memboundEnter(membound *m) {
    pthread_mutex_lock(&m->mutex);
}
#else
#define memboundEnter(m)                                                       \
    do {                                                                       \
        printf("%s:%d Entering with %p\n", __func__, __LINE__, m);             \
        pthread_mutex_lock(&m->mutex);                                         \
    } while (0)
#endif

static void memboundLeave(membound *m) {
    pthread_mutex_unlock(&m->mutex);
}

/* Return the size of an outstanding allocation, in bytes.
 * This only works for chunks that are currently checked out. */
static int32_t memboundSize(membound *m, void *p) {
    assert(p);

    /* Use shift instead of division for performance (szAtom is power of 2) */
    const int32_t i = (int)(((uint8_t *)p - m->zPool) >> m->atomShift);
    assert(i >= 0 && i < m->nBlock);

    const int32_t iSize =
        m->szAtom * (1 << (m->aCtrl[i] & MEMBOUND_CTRL_LOGSIZE));

    return iSize;
}

/* Return a block of memory of at least nBytes in size.
 * Return NULL if unable.  Return NULL if nBytes==0.
 *
 * The caller guarantees that nByte is positive.
 *
 * The caller has obtained a mutex prior to invoking this
 * routine so there is never any chance that two or more
 * threads can be in this routine at the same time.  */
static void *memboundMallocUnsafe(membound *m, int nByte) {
    int32_t i;        /* Index of a m->aPool[] slot */
    int32_t iBin;     /* Index into m->aiFreelist[] */
    int32_t iFullSz;  /* Size of allocation rounded up to power of 2 */
    int32_t iLogsize; /* Log2 of iFullSz/POW2_MIN */

    /* nByte must be a positive */
    assert(nByte > 0);

    /* No more than 1GiB per allocation */
    if (nByte > 0x40000000) {
        return 0;
    }

#if MEMBOUND_DEBUG
    /* Keep track of the maximum allocation request.  Even unfulfilled
     * requests are counted */
    if ((uint32_t)nByte > m->maxRequest) {
        m->maxRequest = nByte;
    }
#endif

    /* Round nByte up to the next valid power of two.
     * Use fast log2 to compute the size class in O(1). */
    if (nByte <= m->szAtom) {
        iLogsize = 0;
        iFullSz = m->szAtom;
    } else {
        /* Compute ceiling log2 of (nByte / szAtom).
         * Use shift instead of division (szAtom is power of 2). */
        iLogsize = memboundLog((nByte + m->szAtom - 1) >> m->atomShift);
        iFullSz = m->szAtom << iLogsize;
    }

    /* Make sure m->aiFreelist[iLogsize] contains at least one free
     * block.  If not, then split a block of the next larger power of
     * two in order to create a new free block of size iLogsize.
     *
     * Use bitmap for O(1) lookup: mask off size classes smaller than
     * iLogsize, then use CTZ to find first available. */
    {
        /* Create mask for size classes >= iLogsize */
        const uint64_t availableMask = m->freelistBitmap >> iLogsize;
        if (availableMask == 0) {
#if 0
            printf("failed to allocate %u bytes", nByte);
#endif
            return NULL;
        }
        /* CTZ gives offset from iLogsize to first available */
        iBin = iLogsize + __builtin_ctzll(availableMask);
    }

    i = m->aiFreelist[iBin];
    memboundUnlink(m, i, iBin);
    while (iBin > iLogsize) {
        int32_t newSize;

        iBin--;
        newSize = 1 << iBin;
        m->aCtrl[i + newSize] = MEMBOUND_CTRL_FREE | iBin;
        memboundLink(m, i + newSize, iBin);
    }

    m->aCtrl[i] = iLogsize;

    /* Update allocator performance statistics. */
    m->nAlloc++;
    m->totalAlloc += iFullSz;
    m->totalExcess += iFullSz - nByte;
    m->currentCount++;
    m->currentOut += iFullSz;
    if (m->maxCount < m->currentCount) {
        m->maxCount = m->currentCount;
    }

    if (m->maxOut < m->currentOut) {
        m->maxOut = m->currentOut;
    }

#if MEMBOUND_DEBUG
    /* Make sure the allocated memory does not assume that it is set to zero
     * or retains a value from a previous allocation */
    memset(&m->zPool[i * m->szAtom], 0xAA, iFullSz);
#endif

    /* Return a pointer to the allocated memory. */
    return (void *)&m->zPool[i * m->szAtom];
}

/* Free an outstanding memory allocation. */
static void memboundFreeUnsafe(membound *m, void *pOld) {
    /* Set iBlock to the index of the block pointed to by pOld in
     * the array of m->szAtom byte blocks pointed to by m->zPool.
     * Use shift instead of division for performance (szAtom is power of 2).
     */
    int32_t iBlock = (int)(((uint8_t *)pOld - m->zPool) >> m->atomShift);

    /* Check that the pointer pOld points to a valid, non-free block. */
    assert(iBlock >= 0 && iBlock < m->nBlock);
    assert(((uint8_t *)pOld - m->zPool) % m->szAtom == 0);
    assert((m->aCtrl[iBlock] & MEMBOUND_CTRL_FREE) == 0);

    uint32_t iLogsize = m->aCtrl[iBlock] & MEMBOUND_CTRL_LOGSIZE;
    uint32_t size = 1 << iLogsize;
    assert(iBlock + size - 1 < (uint32_t)m->nBlock);

    m->aCtrl[iBlock] |= MEMBOUND_CTRL_FREE;
    m->aCtrl[iBlock + size - 1] |= MEMBOUND_CTRL_FREE;

    assert(m->currentCount > 0);
    assert(m->currentOut >= ((uint64_t)size * (uint64_t)m->szAtom));
    m->currentCount--;
    m->currentOut -= (uint64_t)size * (uint64_t)m->szAtom;
    assert(m->currentOut > 0 || m->currentCount == 0);
    assert(m->currentCount > 0 || m->currentOut == 0);

    m->aCtrl[iBlock] = MEMBOUND_CTRL_FREE | iLogsize;
    while (iLogsize < MEMBOUND_LOGMAX) {
        int32_t iBuddy;
        if ((iBlock >> iLogsize) & 1) {
            iBuddy = iBlock - size;
            assert(iBuddy >= 0);
        } else {
            iBuddy = iBlock + size;
            if (iBuddy >= m->nBlock) {
                break;
            }
        }

        if (m->aCtrl[iBuddy] != (MEMBOUND_CTRL_FREE | iLogsize)) {
            break;
        }

        memboundUnlink(m, iBuddy, iLogsize);
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

#if MEMBOUND_DEBUG
    /* Overwrite freed memory with the 0x55 bit pattern to verify that it is
     * not used after being freed */
    memset(&m->zPool[iBlock * m->szAtom], 0x55, size);
#endif

    memboundLink(m, iBlock, iLogsize);
}

/* Allocate nBytes of memory from the pool.
 *
 * Parameters:
 *   m      - The membound allocator (must not be NULL)
 *   nBytes - Number of bytes to allocate (0 returns NULL)
 *
 * Returns:
 *   Pointer to allocated memory on success
 *   NULL if m is NULL, nBytes is 0, nBytes > 1 GiB, or pool is exhausted */
void *memboundAlloc(membound *m, size_t nBytes) {
    if (!m || nBytes == 0) {
        return NULL;
    }

    /* Check size limit before calling unsafe function (which uses int) */
    if (nBytes > 0x40000000) { /* 1 GiB limit */
        return NULL;
    }

    memboundEnter(m);
    void *p = memboundMallocUnsafe(m, (int)nBytes);
    memboundLeave(m);

#if MEMBOUND_DEBUG
    /* During testing we should have no null values... */
    assert(p);
#endif

    return p;
}

/* Free memory previously allocated from this pool.
 *
 * Parameters:
 *   m      - The membound allocator (must not be NULL)
 *   pPrior - Pointer to free (NULL is safely ignored)
 *
 * Note: Freeing a pointer not from this pool is undefined behavior. */
void memboundFree(membound *m, void *pPrior) {
    if (!m || !pPrior) {
        return;
    }

    memboundEnter(m);
    memboundFreeUnsafe(m, pPrior);
    memboundLeave(m);
}

/* Change the size of an existing memory allocation.
 *
 * Parameters:
 *   m      - The membound allocator
 *   pPrior - Existing allocation (must not be NULL)
 *   nBytes - New desired size in bytes (any positive value)
 *
 * Returns:
 *   Pointer to reallocated memory on success (may be same as pPrior)
 *   NULL on failure (pPrior remains valid and unchanged)
 *
 * Behavior:
 *   - If nBytes fits within current allocation, returns pPrior unchanged
 *   - If nBytes requires larger allocation, allocates new block and copies data
 *   - On failure, original allocation is preserved
 *   - nBytes > 1 GiB will fail (allocation limit) */
void *memboundRealloc(membound *m, void *pPrior, size_t nBytes) {
    if (!pPrior) {
        /* NULL pPrior acts like alloc */
        return memboundAlloc(m, nBytes);
    }

    if (nBytes == 0) {
        /* Zero size acts like free, return NULL */
        memboundFree(m, pPrior);
        return NULL;
    }

    /* Get the current allocated size (power-of-2 rounded) */
    int32_t nOldAllocated = memboundSize(m, pPrior);

    /* If the new size fits within current allocation, no-op */
    if (nBytes <= (size_t)nOldAllocated) {
        return pPrior;
    }

    /* Need to grow - allocate new block */
    void *p = memboundAlloc(m, nBytes);
    if (p) {
        /* Copy old data to new location */
        memcpy(p, pPrior, nOldAllocated);
        memboundFree(m, pPrior);
    }
    /* On failure, pPrior remains valid */

    return p;
}

/* SIMD-accelerated memory zeroing.
 * Uses widest available SIMD registers for bulk zeroing. */
static inline void memboundZeroFast(void *dst, size_t bytes) {
    uint8_t *p = (uint8_t *)dst;

#if defined(MEMBOUND_USE_AVX512)
    /* AVX-512: 64 bytes per iteration */
    const __m512i zero512 = _mm512_setzero_si512();
    while (bytes >= 64) {
        _mm512_storeu_si512((__m512i *)p, zero512);
        p += 64;
        bytes -= 64;
    }
#elif defined(MEMBOUND_USE_AVX2)
    /* AVX2: 32 bytes per iteration */
    const __m256i zero256 = _mm256_setzero_si256();
    while (bytes >= 32) {
        _mm256_storeu_si256((__m256i *)p, zero256);
        p += 32;
        bytes -= 32;
    }
#elif defined(MEMBOUND_USE_SSE2)
    /* SSE2: 16 bytes per iteration */
    const __m128i zero128 = _mm_setzero_si128();
    while (bytes >= 16) {
        _mm_storeu_si128((__m128i *)p, zero128);
        p += 16;
        bytes -= 16;
    }
#elif defined(MEMBOUND_USE_NEON)
    /* ARM NEON: 16 bytes per iteration */
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

/* Threshold for using SIMD zeroing vs memset.
 * SIMD overhead is only worthwhile for larger allocations. */
#define MEMBOUND_SIMD_ZERO_THRESHOLD 256

/* Allocate zero-initialized memory from the pool.
 *
 * Parameters:
 *   m     - The membound allocator (must not be NULL)
 *   count - Number of elements to allocate
 *   size  - Size of each element in bytes
 *
 * Returns:
 *   Pointer to zero-initialized memory on success
 *   NULL if m is NULL, count*size overflows, or pool is exhausted
 *
 * Note: Like standard calloc, the memory is guaranteed to be zeroed.
 *       Uses SIMD-accelerated zeroing for larger allocations. */
void *memboundCalloc(membound *m, size_t count, size_t size) {
    if (!m || count == 0 || size == 0) {
        return NULL;
    }

    /* Check for overflow */
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

/* Get the total bytes currently allocated (checked out) from the pool.
 *
 * Parameters:
 *   m - The membound allocator (NULL returns 0)
 *
 * Returns: Total bytes currently in use (including internal fragmentation).
 *
 * Note: This reflects the actual memory consumed in the pool, which may be
 * larger than the sum of requested sizes due to power-of-2 rounding. */
size_t memboundBytesUsed(const membound *m) {
    if (!m) {
        return 0;
    }
    return m->currentOut;
}

/* Get the total bytes available for allocation in the pool.
 *
 * Parameters:
 *   m - The membound allocator (NULL returns 0)
 *
 * Returns: Approximate bytes available. Due to fragmentation, actual
 *          allocatable bytes may vary.
 *
 * Note: This is the pool capacity minus current usage. Individual allocations
 * may fail even with available space due to fragmentation or size rounding. */
size_t memboundBytesAvailable(const membound *m) {
    if (!m) {
        return 0;
    }
    /* Total usable pool size minus currently allocated */
    const size_t totalUsable = m->nBlock * m->szAtom;
    return (totalUsable > m->currentOut) ? (totalUsable - m->currentOut) : 0;
}

/* Get the total pool capacity in bytes.
 *
 * Parameters:
 *   m - The membound allocator (NULL returns 0)
 *
 * Returns: Total pool capacity (usable bytes, not including control array). */
size_t memboundCapacity(const membound *m) {
    if (!m) {
        return 0;
    }
    return m->nBlock * m->szAtom;
}

/* Check if a pointer was allocated from this pool.
 *
 * Parameters:
 *   m - The membound allocator (must not be NULL)
 *   p - Pointer to check (NULL returns false)
 *
 * Returns:
 *   true if p points within this pool's memory region
 *   false if p is NULL, m is NULL, or p is outside the pool
 *
 * Note: This only checks if p is within the pool's address range.
 * It does NOT verify that p is a valid allocation start address. */
bool memboundOwns(const membound *m, const void *p) {
    if (!m || !p) {
        return false;
    }

    const uint8_t *ptr = (const uint8_t *)p;
    const uint8_t *poolStart = m->zPool;
    const uint8_t *poolEnd = m->zPool + (m->nBlock * m->szAtom);

    return (ptr >= poolStart && ptr < poolEnd);
}

/* Return the ceiling of the logarithm base 2 of iValue.
 *
 * Examples:   memboundLog(1) -> 0
 *             memboundLog(2) -> 1
 *             memboundLog(4) -> 2
 *             memboundLog(5) -> 3
 *             memboundLog(8) -> 3
 *             memboundLog(9) -> 4
 *
 * Optimized with CLZ/BSR intrinsics for O(1) performance. */
static inline int32_t memboundLog(int iValue) {
    if (iValue <= 1) {
        return 0;
    }

#if defined(__aarch64__)
    /* ARM64: use CLZ (count leading zeros) instruction.
     * For ceiling log2, we compute floor(log2(iValue-1)) + 1
     * which equals 32 - CLZ(iValue - 1) for a 32-bit value. */
    const int leadingZeros = __builtin_clz((unsigned int)(iValue - 1));
    return 32 - leadingZeros;
#elif defined(__amd64__) || defined(__x86_64__)
    /* x86-64: use BSR (bit scan reverse) instruction.
     * BSR returns index of highest set bit (0-based).
     * For ceiling log2 of iValue, we need floor(log2(iValue-1)) + 1. */
    unsigned int result;
    asm("bsrl %1, %0" : "=r"(result) : "r"((unsigned int)(iValue - 1)));
    return (int32_t)(result + 1);
#else
    /* Fallback: original loop-based implementation */
    int32_t iLog;
    for (iLog = 0;
         (iLog < (int32_t)((sizeof(int32_t) * 8) - 1)) && (1 << iLog) < iValue;
         iLog++) {
    }
    return iLog;
#endif
}

/* Reset the allocator to initial state, freeing all allocations.
 *
 * WARNING: This invalidates ALL pointers allocated from this pool!
 * Only call this when you are certain no code holds references to
 * allocated memory.
 *
 * Parameters:
 *   m - The membound allocator (NULL is safely ignored)
 *
 * This is useful for "arena" style allocation where you want to
 * bulk-free all allocations at once (e.g., per-task memory pools). */
void memboundReset(membound *m) {
    if (!m) {
        return;
    }

    for (int32_t ii = 0; ii <= MEMBOUND_LOGMAX; ii++) {
        m->aiFreelist[ii] = -1;
    }

    /* Reset freelist bitmap (will be set by memboundLink calls below) */
    m->freelistBitmap = 0;

    int32_t iOffset = 0; /* An offset into m->aCtrl[] */
    for (int32_t ii = MEMBOUND_LOGMAX; ii >= 0; ii--) {
        int32_t nAlloc = (1 << ii);
        if ((iOffset + nAlloc) <= m->nBlock) {
            m->aCtrl[iOffset] = ii | MEMBOUND_CTRL_FREE;
            memboundLink(m, iOffset, ii);
            iOffset += nAlloc;
        }

        assert((iOffset + nAlloc) > m->nBlock);
    }

    /* Reset statistics */
    m->currentOut = 0;
    m->currentCount = 0;
}

/* Initialize the memory allocator.
 *
 * This routine is not threadsafe.  The caller must be holding a mutex
 * to prevent multiple threads from entering at the same time.  */
static bool memboundInit(membound *m, void *space, const size_t len) {
    /* boundaries on memboundGlobalConfig.mnReq are enforced in
     * membound_config() */

    /* Calculate log base 2 of minimum allocation size in bytes */
    int32_t nMinLog = memboundLog(256);

    m->szAtom = (1 << nMinLog);
    while ((int)sizeof(MemboundLink) > m->szAtom) {
        m->szAtom = m->szAtom << 1;
        nMinLog++;
    }
    m->atomShift = nMinLog; /* Store log2(szAtom) for fast division */

    m->nBlock = (len / (m->szAtom + sizeof(uint8_t)));
    m->zPool = space;
    m->aCtrl = (uint8_t *)&m->zPool[m->nBlock * m->szAtom];

    memboundReset(m);

    pthread_mutexattr_t attr;

    /* Make sure the mutex is valid ACROSS PROCESSES (FORK!) */
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);

    pthread_mutex_init(&m->mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    return true;
}

#include <sys/mman.h>
static void *memboundMAP(const size_t len) {
    void *z;
    if ((z = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED,
                  -1, 0)) == MAP_FAILED) {
        return NULL;
    }

    return z;
}

/* Grow the memory pool to a new size.
 *
 * IMPORTANT: This can only be called when there are NO outstanding allocations.
 * All existing pointers become invalid after growth because the pool is
 * remapped to a new memory location.
 *
 * Returns true on success, false on failure (including if allocations exist).
 */
static bool memboundGrow(membound *m, const size_t newLen) {
    /* Safety check: refuse to grow if there are outstanding allocations.
     * Growing would invalidate all existing pointers. */
    if (m->currentCount != 0) {
        return false;
    }

    /* Validate new size is larger than current */
    if (newLen <= m->size) {
        return false;
    }

    /* Create new pool */
    void *newPool = memboundMAP(newLen);
    if (!newPool) {
        return false;
    }

    /* Release old pool - safe because no allocations exist */
    if (munmap(m->zPool, m->size) != 0) {
        munmap(newPool, newLen);
        return false;
    }

    /* Calculate new pool parameters */
    int32_t nMinLog = memboundLog(256);
    int64_t szAtom = (1 << nMinLog);
    while ((int)sizeof(MemboundLink) > szAtom) {
        szAtom = szAtom << 1;
        nMinLog++;
    }

    /* Update pool metadata */
    m->zPool = newPool;
    m->size = newLen;
    m->szAtom = szAtom;
    m->atomShift = nMinLog;
    m->nBlock = (newLen / (szAtom + sizeof(uint8_t)));
    m->aCtrl = (uint8_t *)&m->zPool[m->nBlock * m->szAtom];

    /* Reset statistics for the new pool */
    m->nAlloc = 0;
    m->totalAlloc = 0;
    m->totalExcess = 0;
    m->currentOut = 0;
    m->currentCount = 0;
    m->maxOut = 0;
    m->maxCount = 0;

    /* Initialize free lists for the new pool */
    memboundReset(m);

    return true;
}

/* Increase the pool size to accommodate more allocations.
 *
 * Parameters:
 *   m    - The membound allocator (must not be NULL)
 *   size - New pool size in bytes (must be larger than current)
 *
 * Returns:
 *   true on success
 *   false if m is NULL, size is not larger, or allocations exist
 *
 * IMPORTANT: Can only be called when there are NO outstanding allocations.
 * All statistics are reset after growth. */
bool memboundIncreaseSize(membound *m, size_t size) {
    if (!m) {
        return false;
    }

    memboundEnter(m);
    const bool status = memboundGrow(m, size);
    memboundLeave(m);
    return status;
}

membound *memboundCreate(size_t size) {
    membound *m = memboundMAP(sizeof(*m));
    if (!m) {
        return NULL;
    }

    m->size = size;

    void *space = memboundMAP(size);
    if (!space) {
        munmap(m, sizeof(*m));
        return NULL;
    }

    memboundInit(m, space, size);

    return m;
}

/* Shutdown allocator only if no outstanding allocations exist.
 *
 * Returns: true on success, false if allocations still exist.
 *
 * Thread safety: Caller must ensure no other threads are using this allocator
 * when calling shutdown functions. */
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

/* Shutdown and destroy the allocator, releasing all memory.
 *
 * WARNING: This will invalidate ALL pointers allocated from this pool!
 *
 * Thread safety: Caller must ensure no other threads are using this allocator.
 * Calling this while other threads are allocating/freeing is undefined
 * behavior.
 *
 * Returns: true on success, false on munmap failure. */
bool memboundShutdown(membound *m) {
    if (!m) {
        return false;
    }

    /* Capture pool info before destroying anything */
    void *zPool = m->zPool;
    size_t poolSize = m->size;

    /* Lock, then unlock before destroying mutex (POSIX requires mutex
     * to be unlocked before destruction) */
    memboundEnter(m);
    memboundLeave(m);

    /* Now safe to destroy the mutex */
    pthread_mutex_destroy(&m->mutex);

    /* Release memory mappings */
    const int32_t zpStat = munmap(zPool, poolSize);
    const int32_t mStat = munmap(m, sizeof(*m));

    return (zpStat == 0) && (mStat == 0);
}

/* Get the number of outstanding allocations from this pool.
 *
 * Parameters:
 *   m - The membound allocator (NULL returns 0)
 *
 * Returns: Number of allocations that have not been freed. */
size_t memboundCurrentAllocationCount(const membound *m) {
    if (!m) {
        return 0;
    }
    return m->currentCount;
}

#ifndef NDEBUG
/* Open the file indicated and write a log of all unfreed memory
 * allocations into that log. */
static void memboundMemBoundDumpLock(membound *m, const char *zFilename,
                                     bool useLock) {
    FILE *out = NULL;

    if (zFilename) {
        out = fopen(zFilename, "w");
        if (out == 0) {
            fprintf(stderr,
                    " * Unable to output memory debug output log: %s  *\n",
                    zFilename);
            return;
        }
    } else {
        out = stdout;
    }

    if (useLock) {
        memboundEnter(m);
    }

    int32_t j, n;
    for (int32_t nMinLog = memboundLog(m->szAtom), i = 0;
         i <= MEMBOUND_LOGMAX && i + nMinLog < 32; i++) {
        for (n = 0, j = m->aiFreelist[i]; j >= 0;
             j = MEMBOUND_LINK(m, j)->next, n++) {
        }

        fprintf(out, "[%d] freelist items of size %" PRId64 ": %d\n", i,
                m->szAtom << i, n);
    }

    fprintf(out, "m->nAlloc       = %" PRIu64 "\n", m->nAlloc);
    fprintf(out, "m->totalAlloc   = %" PRIu64 "\n", m->totalAlloc);
    fprintf(out, "m->totalExcess  = %" PRIu64 "\n", m->totalExcess);
    fprintf(out, "m->currentOut   = %" PRIu64 "\n", m->currentOut);
    fprintf(out, "m->currentCount = %" PRIu64 "\n", m->currentCount);
    fprintf(out, "m->maxOut       = %" PRIu64 "\n", m->maxOut);
    fprintf(out, "m->maxCount     = %" PRIu64 "\n", m->maxCount);
#if MEMBOUND_DEBUG
    fprintf(out, "m->maxRequest   = %" PRIu64 "\n", m->maxRequest);
#endif

    if (useLock) {
        memboundLeave(m);
    }

    if (out == stdout) {
        fflush(stdout);
    } else {
        fclose(out);
    }
}

static void memboundMemBoundDump(membound *m, const char *zFilename) {
    memboundMemBoundDumpLock(m, zFilename, true);
}
#endif

#ifdef DATAKIT_TEST
#include "ctest.h"

int memboundTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    TEST("Basic alloc and free") {
        membound *m = memboundCreate(1 << 20); /* 1 MB pool */
        assert(m != NULL);

        void *p = memboundAlloc(m, 256);
        assert(p != NULL);
        assert(memboundCurrentAllocationCount(m) == 1);

        memboundFree(m, p);
        assert(memboundCurrentAllocationCount(m) == 0);

        memboundShutdown(m);
    }

    TEST("Repeated alloc/free cycle") {
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

    TEST("Pool exhaustion") {
        const size_t poolSize = 1 << 16; /* 64 KB */
        membound *m = memboundCreate(poolSize);
        assert(m != NULL);

        /* Allocate until exhaustion */
        void *ptrs[100];
        int count = 0;
        for (int i = 0; i < 100; i++) {
            void *p = memboundAlloc(m, 4096);
            if (!p) {
                break;
            }
            ptrs[count++] = p;
        }
        assert(count > 0);   /* Should get at least some allocations */
        assert(count < 100); /* Should exhaust before 100 */

        /* Next allocation should fail */
        assert(memboundAlloc(m, 4096) == NULL);

        /* Free all and verify */
        for (int i = 0; i < count; i++) {
            memboundFree(m, ptrs[i]);
        }
        assert(memboundCurrentAllocationCount(m) == 0);

        memboundShutdown(m);
    }

    TEST("memboundCalloc zero-initialization") {
        membound *m = memboundCreate(1 << 20);
        assert(m != NULL);

        /* Allocate some memory and fill with non-zero */
        void *p1 = memboundAlloc(m, 1024);
        assert(p1 != NULL);
        memset(p1, 0xFF, 1024);
        memboundFree(m, p1);

        /* Calloc should return zeroed memory */
        uint8_t *p2 = memboundCalloc(m, 128, 8); /* 1024 bytes */
        assert(p2 != NULL);
        for (int i = 0; i < 1024; i++) {
            assert(p2[i] == 0);
        }
        memboundFree(m, p2);

        memboundShutdown(m);
    }

    TEST("memboundCalloc overflow protection") {
        membound *m = memboundCreate(1 << 20);
        assert(m != NULL);

        /* This should fail due to overflow */
        void *p = memboundCalloc(m, SIZE_MAX, 2);
        assert(p == NULL);

        /* Zero count should return NULL */
        p = memboundCalloc(m, 0, 100);
        assert(p == NULL);

        /* Zero size should return NULL */
        p = memboundCalloc(m, 100, 0);
        assert(p == NULL);

        memboundShutdown(m);
    }

    TEST("memboundRealloc grow") {
        membound *m = memboundCreate(1 << 20);
        assert(m != NULL);

        /* Allocate small */
        char *p = memboundAlloc(m, 256);
        assert(p != NULL);
        strcpy(p, "hello");

        /* Grow */
        p = memboundRealloc(m, p, 1024);
        assert(p != NULL);
        assert(strcmp(p, "hello") == 0); /* Data preserved */

        memboundFree(m, p);
        memboundShutdown(m);
    }

    TEST("memboundRealloc shrink (no-op)") {
        membound *m = memboundCreate(1 << 20);
        assert(m != NULL);

        char *p = memboundAlloc(m, 1024);
        assert(p != NULL);
        char *original = p;
        strcpy(p, "test data");

        /* Shrink should return same pointer */
        p = memboundRealloc(m, p, 256);
        assert(p == original);
        assert(strcmp(p, "test data") == 0);

        memboundFree(m, p);
        memboundShutdown(m);
    }

    TEST("memboundRealloc NULL acts like alloc") {
        membound *m = memboundCreate(1 << 20);
        assert(m != NULL);

        void *p = memboundRealloc(m, NULL, 256);
        assert(p != NULL);
        assert(memboundCurrentAllocationCount(m) == 1);

        memboundFree(m, p);
        memboundShutdown(m);
    }

    TEST("memboundRealloc zero size acts like free") {
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

    TEST("memboundReset bulk free") {
        membound *m = memboundCreate(1 << 20);
        assert(m != NULL);

        /* Make several allocations */
        for (int i = 0; i < 10; i++) {
            void *p = memboundAlloc(m, 1024);
            assert(p != NULL);
        }
        assert(memboundCurrentAllocationCount(m) == 10);

        /* Reset should free all */
        memboundReset(m);
        assert(memboundCurrentAllocationCount(m) == 0);
        assert(memboundBytesUsed(m) == 0);

        /* Should be able to allocate again */
        void *p = memboundAlloc(m, 1024);
        assert(p != NULL);
        memboundFree(m, p);

        memboundShutdown(m);
    }

    TEST("memboundIncreaseSize when empty") {
        membound *m = memboundCreate(1 << 16); /* 64 KB */
        assert(m != NULL);

        size_t oldCapacity = memboundCapacity(m);

        /* Grow to 256 KB */
        bool grew = memboundIncreaseSize(m, 1 << 18);
        assert(grew);
        assert(memboundCapacity(m) > oldCapacity);

        /* Verify usable */
        void *p = memboundAlloc(m, 1024);
        assert(p != NULL);
        memboundFree(m, p);

        memboundShutdown(m);
    }

    TEST("memboundIncreaseSize fails with allocations") {
        membound *m = memboundCreate(1 << 16);
        assert(m != NULL);

        void *p = memboundAlloc(m, 256);
        assert(p != NULL);

        /* Should fail because allocation exists */
        bool grew = memboundIncreaseSize(m, 1 << 18);
        assert(!grew);

        memboundFree(m, p);

        /* Now should succeed */
        grew = memboundIncreaseSize(m, 1 << 18);
        assert(grew);

        memboundShutdown(m);
    }

    TEST("memboundShutdownSafe with allocations") {
        membound *m = memboundCreate(1 << 16);
        assert(m != NULL);

        void *p = memboundAlloc(m, 256);
        assert(p != NULL);

        /* Should fail - allocation exists */
        bool shut = memboundShutdownSafe(m);
        assert(!shut);

        memboundFree(m, p);

        /* Now should succeed */
        shut = memboundShutdownSafe(m);
        assert(shut);
    }

    TEST("memboundBytesUsed and memboundBytesAvailable") {
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

    TEST("memboundOwns pointer check") {
        membound *m = memboundCreate(1 << 16);
        assert(m != NULL);

        void *p = memboundAlloc(m, 256);
        assert(p != NULL);

        /* Should own the allocated pointer */
        assert(memboundOwns(m, p));

        /* Should not own stack variable */
        int stackVar = 42;
        assert(!memboundOwns(m, &stackVar));

        /* Should not own NULL */
        assert(!memboundOwns(m, NULL));

        memboundFree(m, p);
        memboundShutdown(m);
    }

    TEST("NULL safety") {
        /* All functions should handle NULL gracefully */
        assert(memboundAlloc(NULL, 100) == NULL);
        memboundFree(NULL, (void *)0x1234);     /* Should not crash */
        memboundFree((membound *)0x1234, NULL); /* Should not crash */
        assert(memboundRealloc(NULL, NULL, 100) == NULL);
        memboundReset(NULL); /* Should not crash */
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

    TEST("Various allocation sizes") {
        membound *m = memboundCreate(1 << 22); /* 4 MB */
        assert(m != NULL);

        /* Test various sizes including non-power-of-2 */
        size_t sizes[] = {1,   7,    64,   100,   255,   256,   257,
                          500, 1000, 4096, 10000, 65536, 100000};
        void *ptrs[sizeof(sizes) / sizeof(sizes[0])];

        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            ptrs[i] = memboundAlloc(m, sizes[i]);
            assert(ptrs[i] != NULL);
            /* Write to verify it's usable */
            memset(ptrs[i], (int)i, sizes[i]);
        }

        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            memboundFree(m, ptrs[i]);
        }

        assert(memboundCurrentAllocationCount(m) == 0);
        memboundShutdown(m);
    }

    TEST("Max allocation size (1 GiB limit)") {
        membound *m = memboundCreate((size_t)2 << 30); /* 2 GB pool */
        if (m == NULL) {
            /* Skip if can't create large pool */
            printf("(skipped - can't create 2GB pool) ");
        } else {
            /* Should fail - exceeds 1 GiB limit */
            void *p = memboundAlloc(m, (size_t)1 << 31); /* 2 GB */
            assert(p == NULL);

            /* 1 GiB should work */
            p = memboundAlloc(m, (size_t)1 << 30);
            if (p != NULL) {
                memboundFree(m, p);
            }

            memboundShutdown(m);
        }
    }

    TEST("Fragmentation and coalescing") {
        membound *m = memboundCreate(1 << 20);
        assert(m != NULL);

        /* Allocate several blocks */
        void *p1 = memboundAlloc(m, 256);
        void *p2 = memboundAlloc(m, 256);
        void *p3 = memboundAlloc(m, 256);
        void *p4 = memboundAlloc(m, 256);
        assert(p1 && p2 && p3 && p4);

        /* Free in scattered order to test coalescing */
        memboundFree(m, p2);
        memboundFree(m, p4);
        memboundFree(m, p1);
        memboundFree(m, p3);

        /* All should be coalesced - verify with fresh allocation */
        assert(memboundCurrentAllocationCount(m) == 0);
        void *big = memboundAlloc(m, 4096);
        assert(big != NULL);
        memboundFree(m, big);

        memboundShutdown(m);
    }

#ifndef NDEBUG
    TEST("Debug dump (visual check)") {
        membound *m = memboundCreate(1 << 16);
        assert(m != NULL);

        void *p1 = memboundAlloc(m, 256);
        void *p2 = memboundAlloc(m, 512);
        (void)p1;
        (void)p2;

        printf("\n--- Memory dump with allocations ---\n");
        memboundMemBoundDump(m, NULL);

        memboundFree(m, p1);
        memboundFree(m, p2);
        memboundShutdown(m);
    }
#endif

    TEST("Stress test - rapid alloc/free cycles") {
        membound *m = memboundCreate(1 << 22); /* 4 MB */
        assert(m != NULL);

        const int iterations = 100000;
        for (int i = 0; i < iterations; i++) {
            /* Vary allocation size to exercise different size classes */
            size_t size = 64 + (i % 1024);
            void *p = memboundAlloc(m, size);
            assert(p != NULL);
            /* Touch memory to ensure it's valid */
            *(volatile char *)p = (char)i;
            memboundFree(m, p);
        }

        assert(memboundCurrentAllocationCount(m) == 0);
        memboundShutdown(m);
    }

    TEST("SIMD calloc stress test") {
        membound *m = memboundCreate(1 << 24); /* 16 MB */
        assert(m != NULL);

        /* Test various sizes around the SIMD threshold */
        size_t sizes[] = {128, 256, 512, 1024, 4096, 8192, 16384};
        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            for (int i = 0; i < 100; i++) {
                uint8_t *p = memboundCalloc(m, 1, sizes[s]);
                assert(p != NULL);
                /* Verify zeroed */
                for (size_t j = 0; j < sizes[s]; j++) {
                    assert(p[j] == 0);
                }
                memboundFree(m, p);
            }
        }

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
 */
