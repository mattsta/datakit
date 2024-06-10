#include "membound.h"

/* This was originally 'mem5.c' from sqlite3. */

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>

#include <string.h>

/* Maximum size of any individual allocation request is ((1<<LOGMAX)*szAtom).
 * Since szAtom is always at least 8 and 32-bit integers are used,
 * it is not actually possible to reach this limit. */
#define MEMBOUND_LOGMAX 30

struct membound {
    /* Memory available for allocation */
    uint8_t *zPool; /* Memory available to be allocated */
    int64_t szAtom; /* Smallest possible allocation in bytes */
    uint64_t size;  /* Byte extent of 'zPool' */
    int64_t nBlock; /* Number of szAtom sized blocks in zPool */

    /* Mutex to control access to the memory allocation subsystem. */
    pthread_mutex_t mutex;

    /* Performance statistics */
    uint64_t nAlloc;     /* Total number of calls to malloc */
    uint64_t totalAlloc; /* Total of all malloc calls including internal frag */
    uint64_t totalExcess;  /* Total internal fragmentation */
    uint32_t currentOut;   /* Current checkout, including internal frag */
    uint32_t currentCount; /* Current number of distinct checkouts */
    uint32_t maxOut;       /* Maximum instantaneous currentOut */
    uint32_t maxCount;     /* Maximum instantaneous currentCount */
#if MEMBOUND_DEBUG
    uint32_t maxRequest; /* Largest allocation (exclusive of internal frag) */
#endif

    /* Lists of free blocks.  aiFreelist[0] is a list of free blocks of
     * size szAtom.  aiFreelist[1] holds blocks of size szAtom*2.
     * aiFreelist[2] holds free blocks of size szAtom*4.  And so forth. */
    int32_t aiFreelist[MEMBOUND_LOGMAX + 1];

    /* Space for tracking which blocks are checked out and the size
     * of each block. One byte per block. */
    uint8_t *aCtrl;
};

#ifndef NDEBUG
static void memboundMemBoundDumpLock(membound *m, const char *zFilename,
                                     bool useLock);
#endif

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

    const int32_t i = (int)(((uint8_t *)p - m->zPool) / m->szAtom);
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

    /* Round nByte up to the next valid power of two */
    for (iFullSz = m->szAtom, iLogsize = 0; iFullSz < nByte;
         iFullSz *= 2, iLogsize++) {
    }

    /* Make sure m->aiFreelist[iLogsize] contains at least one free
     * block.  If not, then split a block of the next larger power of
     * two in order to create a new free block of size iLogsize.  */
    for (iBin = iLogsize; iBin <= MEMBOUND_LOGMAX && m->aiFreelist[iBin] < 0;
         iBin++) {
    }

    if (iBin > MEMBOUND_LOGMAX) {
#if 0
        printf("failed to allocate %u bytes", nByte);
#endif
        return NULL;
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
     */
    int32_t iBlock = (int)(((uint8_t *)pOld - m->zPool) / m->szAtom);

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
    assert(m->currentOut >= (size * m->szAtom));
    m->currentCount--;
    m->currentOut -= size * m->szAtom;
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

/* Allocate nBytes of memory. */
void *memboundAlloc(membound *m, size_t nBytes) {
    void *p = NULL;
    if (nBytes > 0) {
        memboundEnter(m);
        p = memboundMallocUnsafe(m, nBytes);
        memboundLeave(m);
    }

#if MEMBOUND_DEBUG
    /* During testing we should have no null values... */
    assert(nBytes && p);
#endif

    return p;
}

/* Free memory.
 *
 * The outer layer memory allocator prevents this routine from
 * being called with a NULL pPrior. */
void memboundFree(membound *m, void *pPrior) {
    assert(pPrior);
    memboundEnter(m);
    memboundFreeUnsafe(m, pPrior);
    memboundLeave(m);
}

/* Change the size of an existing memory allocation.
 *
 * The outer layer memory allocator prevents this routine from
 * being called with pPrior==0.
 *
 * nBytes is always a value obtained from a prior call to
 * memboundRound().  Hence nBytes is always a non-negative power
 * of two.  If nBytes==0 that means that an oversize allocation
 * (an allocation larger than 0x40000000) was requested and this
 * routine should return 0 without freeing pPrior.  */
void *memboundRealloc(membound *m, void *pPrior, int32_t nBytes) {
    assert(pPrior != 0);
    assert((nBytes & (nBytes - 1)) == 0); /* EV: R-46199-30249 */
    assert(nBytes >= 0);
    if (nBytes == 0) {
        return 0;
    }

    int32_t nOld = memboundSize(m, pPrior);
    if (nBytes <= nOld) {
        return pPrior;
    }

    void *p = memboundAlloc(m, nBytes);
    if (p) {
        memcpy(p, pPrior, nOld);
        memboundFree(m, pPrior);
    }

    return p;
}

/* Return the ceiling of the logarithm base 2 of iValue.
 *
 * Examples:   memboundLog(1) -> 0
 *             memboundLog(2) -> 1
 *             memboundLog(4) -> 2
 *             memboundLog(5) -> 3
 *             memboundLog(8) -> 3
 *             memboundLog(9) -> 4 */
static int32_t memboundLog(int iValue) {
    int32_t iLog;
    for (iLog = 0;
         (iLog < (int32_t)((sizeof(int32_t) * 8) - 1)) && (1 << iLog) < iValue;
         iLog++) {
    }

    return iLog;
}

/* Mark everything free again */
void memboundReset(membound *m) {
    for (int32_t ii = 0; ii <= MEMBOUND_LOGMAX; ii++) {
        m->aiFreelist[ii] = -1;
    }

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
    }

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

static bool memboundGrow(membound *m, const size_t newLen) {
    /* Create new pool */
    void *newPool = memboundMAP(newLen);
    if (!newPool) {
        return false;
    }

    /* Copy existing pool to new pool */
    memcpy(newPool, m->zPool, m->size);

    /* Release old pool */
    if (munmap(m->zPool, m->size) != 0) {
        /* If failure, don't continue */
        munmap(newPool, newLen);
        return false;
    }

    /* Replace old pool with new pool */
    m->zPool = newPool;
    m->size = newLen;

    /* Done! */
    return true;
}

bool memboundIncreaseSize(membound *m, size_t size) {
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

bool memboundShutdownSafe(membound *m) {
    /* If users haven't called memboundFree() to cancel
     * out every memboundAlloc(), then the users stil have
     * outstanding pointers to this allocator's memory and
     * we shouldn't destroy it yet. */
    if (m->currentCount != 0) {
        return false;
    }

    return memboundShutdown(m);
}

bool memboundShutdown(membound *m) {
    memboundEnter(m);
    pthread_mutex_destroy(&m->mutex);
#ifdef __linux__
    m->mutex = (pthread_mutex_t){{0}};
#else
    m->mutex = (pthread_mutex_t){0};
#endif

    const int32_t zpStat = munmap(m->zPool, m->size);
    const int32_t mStat = munmap(m, sizeof(*m));
    return !zpStat && !mStat;
}

size_t memboundCurrentAllocationCount(const membound *m) {
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
    fprintf(out, "m->currentOut   = %u\n", m->currentOut);
    fprintf(out, "m->currentCount = %u\n", m->currentCount);
    fprintf(out, "m->maxOut       = %u\n", m->maxOut);
    fprintf(out, "m->maxCount     = %u\n", m->maxCount);
#if MEMBOUND_DEBUG
    fprintf(out, "m->maxRequest   = %u\n", m->maxRequest);
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
int memboundTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("Allocating and freeing a lot...");

    const int createMax = 1 << 20;
    const int allocateDuration = (createMax / 8192) * 2;
    membound *m = memboundCreate(createMax);

    for (int i = 0; i < allocateDuration; i++) {
        void *got = memboundAlloc(m, 8192);

        got ? printf(".") : printf("!");
        fflush(stdout);

        assert(got);
        memboundFree(m, got);
    }

    printf("\n");
    memboundMemBoundDump(m, NULL);

    printf("\nAllocating beyond entire memory space...");
    for (int i = 0; i < allocateDuration; i++) {
        const void *got = memboundAlloc(m, 8192);

        got ? printf(".") : printf("!");
        fflush(stdout);

        assert(((i + 1) * 8192) < createMax ? !!got : !!!got);
    }

    printf("\n");
    memboundMemBoundDump(m, NULL);

    printf("\nShutting down allocator...\n");
    const bool shut = memboundShutdown(m);
    assert(shut);

    printf("Done!\n");

    return 0;
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
