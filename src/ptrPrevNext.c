#include "ptrPrevNext.h"
#include "../deps/varint/src/varintSplitFull16.h"

typedef struct memspace {
    void *mem;
    size_t bestFree;
    int64_t bestFreeLen;
    size_t writeFromEnd;
    size_t len;
} memspace;

struct ptrPrevNext {
    memspace ms;
};

size_t ptrPrevNextReprSingle(ptrPrevNext *ppn, size_t restoredOffset);

#define SPATIAL_OFFSET 6

#define MEMSPACE_FITS(ms, size) (((ms).writeFromEnd + (size)) <= (ms).len)
#define MOFF(ms, off) ((uint8_t *)((ms).mem) + (off))

/* We check 'off' AND 'off+1' because our allocations are two byte minimum, and
 * the first byte of a valid allocation can be 0, but two zeroes never appear
 * next to each other without traversing a non-zero type byte. */
#define MOFF_ZERO(ms, off)                                                     \
    (((off) < ((ms).len - 1)) && (*((uint8_t *)((ms).mem) + (off)) == 0) &&    \
     (*((uint8_t *)((ms).mem) + (off + 1)) == 0))

ptrPrevNext *ptrPrevNextNew(void) {
    ptrPrevNext *ppn = zcalloc(1, sizeof(*ppn));
    ppn->ms.len = 4096;
    ppn->ms.mem = zcalloc(1, ppn->ms.len);

    ppn->ms.bestFree = 0;
    ppn->ms.bestFreeLen = ppn->ms.len;
    ppn->ms.writeFromEnd = 0;

    return ppn;
}

void ptrPrevNextFree(ptrPrevNext *ppn) {
    if (ppn) {
        zfree(ppn->ms.mem);
        zfree(ppn);
    }
}

DK_STATIC size_t ptrPrevNextPopulatePrevNext(const size_t prevOffset,
                                             const size_t nextOffset,
                                             void *_dst, const size_t len) {
    uint8_t *const dst = _dst;
    uint8_t prevLen;
    uint8_t nextLen;

    uint8_t totalSize = 0;
    varintSplitFull16Length_(prevLen, prevOffset);
    totalSize += prevLen;

    varintSplitFull16Length_(nextLen, nextOffset);
    totalSize += nextLen;

    if (totalSize > len) {
        /* Size too big for destination space. Don't write anything. */
        return 0;
    }

    varintSplitFull16Put_(dst, prevLen, prevOffset);
    varintSplitFull16Put_(dst + prevLen, nextLen, nextOffset);

    return totalSize;
}

DK_STATIC size_t ptrPrevNextPopulateAll(const size_t atomIndex,
                                        const size_t prevOffset,
                                        const size_t nextOffset, void *_dst,
                                        const size_t len) {
    uint8_t *dst = _dst;
    uint8_t atomLen;
    uint8_t prevLen;
    uint8_t nextLen;

    uint8_t totalSize = 0;
    varintSplitFull16Length_(atomLen, atomIndex);
    totalSize += atomLen;

    varintSplitFull16Length_(prevLen, prevOffset);
    totalSize += prevLen;

    varintSplitFull16Length_(nextLen, nextOffset);
    totalSize += nextLen;

    if (totalSize > len) {
        /* Size too big for destination space. Don't write anything. */
        return 0;
    }

    varintSplitFull16Put_(dst, atomLen, atomIndex);
    varintSplitFull16Put_(dst + atomLen, prevLen, prevOffset);
    varintSplitFull16Put_(dst + atomLen + prevLen, nextLen, nextOffset);

    return totalSize;
}

#define alignAlignedOffset(target)                                             \
    do {                                                                       \
        /* All our write slots must be divisible by SPATIAL_OFFSET */          \
        const uint32_t divRemainder = (target) % SPATIAL_OFFSET;               \
        if (divRemainder) {                                                    \
            /* If writeFromEnd not divisible by 3, boost it by three           \
             * then subtract the difference to get a proper rem 0 offset. */   \
            (target) += SPATIAL_OFFSET - divRemainder;                         \
        }                                                                      \
    } while (0)

ptrPrevNextPtr ptrPrevNextAdd(ptrPrevNext *ppn, const size_t atomIndex,
                              const size_t prevOffset,
                              const size_t nextOffset) {
    /* Step 1: try to use lowest available space first.
     *         if not enough low space, use end for writing. */
    size_t memOffset = ppn->ms.bestFree;
    int64_t availableLen = ppn->ms.bestFreeLen;
    bool useLowest = true;

    if (availableLen < 6) {
        while (!MEMSPACE_FITS(ppn->ms, 9 * 3)) {
            /* grow! */
            ppn->ms.len = jebufSizeAllocation(ppn->ms.len * 1.5);
            ppn->ms.mem = zrealloc(ppn->ms.mem, ppn->ms.len);
        }

        availableLen = 9 * 3;
        memOffset = ppn->ms.writeFromEnd;
        useLowest = false;
    }

    assert(memOffset % SPATIAL_OFFSET == 0);

    /* Step 2: write {index, prev, next} into memory block */
    uint8_t encodedLen =
        ptrPrevNextPopulateAll(atomIndex, prevOffset, nextOffset,
                               MOFF(ppn->ms, memOffset), availableLen);

    /* Step 3: update metadata based on written size */
    if (useLowest) {
        bool setHighestToo = false;
        if (ppn->ms.bestFree == ppn->ms.writeFromEnd) {
            setHighestToo = true;
        }

        ppn->ms.bestFree += encodedLen;
        ppn->ms.bestFreeLen -= encodedLen;

        /* All our write slots must be divisible by 3 */
        const uint32_t divRemainder = ppn->ms.bestFree % SPATIAL_OFFSET;
        if (divRemainder) {
            /* If writeFromEnd not divisible by 3, boost it by three
             * then subtract the difference to get a proper rem 0 offset. */
            ppn->ms.bestFree += SPATIAL_OFFSET - divRemainder;
            ppn->ms.bestFreeLen -= SPATIAL_OFFSET - divRemainder;
        }

        if (setHighestToo) {
            ppn->ms.writeFromEnd = ppn->ms.bestFree;
        }
    } else {
        ppn->ms.writeFromEnd += encodedLen;
        alignAlignedOffset(ppn->ms.writeFromEnd);
    }

    /* Step 4: return offset into memory block for this pointer */
    return memOffset / SPATIAL_OFFSET;
}

ptrPrevNextPtr ptrPrevNextUpdate(ptrPrevNext *ppn,
                                 const ptrPrevNextPtr memOffset,
                                 const size_t prevOffset,
                                 const size_t nextOffset) {
    const size_t restoredOffset = memOffset * SPATIAL_OFFSET;

    /* Verify size of prevoffset + nextoffset <= size of current offsets */
    size_t currentOffset = restoredOffset;

    const uint8_t lenAtom =
        varintSplitFull16GetLenQuick_(MOFF(ppn->ms, currentOffset));
    currentOffset += lenAtom;

    const uint8_t lenOldPrev =
        varintSplitFull16GetLenQuick_(MOFF(ppn->ms, currentOffset));
    currentOffset += lenOldPrev;

    const uint8_t lenOldNext =
        varintSplitFull16GetLenQuick_(MOFF(ppn->ms, currentOffset));
    currentOffset += lenOldNext;

    const bool isEndOffset = currentOffset == ppn->ms.writeFromEnd;

    /* Write into current space */
    const uint8_t encodedLen = ptrPrevNextPopulatePrevNext(
        prevOffset, nextOffset, MOFF(ppn->ms, restoredOffset + lenAtom),
        isEndOffset ? ppn->ms.len - ppn->ms.writeFromEnd
                    : lenOldPrev + lenOldNext);

    if (encodedLen == 0) {
        /* NEED TO GROW, INVALIDATE CURRENT OFFSET */
        /* INSERT AS NEW, RETURN NEW. */
        /* MEMSET ZERO CURRENT, UPDATE MIN OFFSET IF REQUIRED */

        uint8_t _atomLen;
        size_t atomRef;
        varintSplitFull16Get_(MOFF(ppn->ms, restoredOffset), _atomLen, atomRef);

        memset(MOFF(ppn->ms, restoredOffset), 0,
               currentOffset - restoredOffset);

        if (memOffset < ppn->ms.bestFree) {
            ppn->ms.bestFree = memOffset;
            ppn->ms.bestFreeLen = currentOffset - restoredOffset;
            for (size_t i = 0; MOFF_ZERO(ppn->ms, ppn->ms.bestFree + i); i++) {
                ppn->ms.bestFreeLen++;
            }

            /* TODO: iterate from mem+bestFree+bestFreeLen and increase
             * bestFreeLen to account for all empty entries abutting
             * after this memOffset.  (i.e. if there's one or more empty
             * entries immediately after this new minimum, consume them
             * into 'bestFreeLen' as well.) */
            /* Also, when using 'bestFree,' if not used entire bestFreeLen,
             * set new bestFree to the remainder of the length not used by
             * a new setting. */
        }

        return ptrPrevNextAdd(ppn, atomRef, prevOffset, nextOffset);
    }

    /* else, it all fit in place on the first try */

    /* update accounting if necessary! */
    if (isEndOffset) {
        ppn->ms.writeFromEnd = restoredOffset + lenAtom + encodedLen;
        alignAlignedOffset(ppn->ms.writeFromEnd);
    }

    return memOffset;
}

void ptrPrevNextRead(const ptrPrevNext *ppn, const ptrPrevNextPtr memOffset,
                     size_t *atomRef, size_t *prev, size_t *next) {
    const size_t restoredOffset = memOffset * SPATIAL_OFFSET;

    size_t *const values[3] = {atomRef, prev, next};
    size_t totalOffsetLen = restoredOffset;
    for (int i = 0; i < 3; i++) {
        uint8_t len;
        varintSplitFull16Get_(MOFF(ppn->ms, totalOffsetLen), len, *values[i]);
        totalOffsetLen += len;
    }
}

void ptrPrevNextRelease(ptrPrevNext *ppn, const ptrPrevNextPtr memOffset) {
    const uint8_t atomLen =
        varintSplitFull16GetLenQuick_(MOFF(ppn->ms, memOffset));
    const uint8_t prevLen =
        varintSplitFull16GetLenQuick_(MOFF(ppn->ms, memOffset + atomLen));
    const uint8_t nextLen = varintSplitFull16GetLenQuick_(
        MOFF(ppn->ms, memOffset + atomLen + prevLen));

    uint32_t totalLen = atomLen + prevLen + nextLen;
    memset(MOFF(ppn->ms, memOffset), 0, totalLen);

    while (MOFF_ZERO(ppn->ms, memOffset + totalLen)) {
        totalLen++;
    }

    if (totalLen > ppn->ms.bestFreeLen) {
        ppn->ms.bestFree = memOffset;
        ppn->ms.bestFreeLen = totalLen;
    }
}

#ifdef DATAKIT_TEST
size_t ptrPrevNextReprSingle(ptrPrevNext *ppn, size_t restoredOffset) {
    size_t currentOffset = restoredOffset;

    size_t atomIndex;
    size_t prevOffset;
    size_t nextOffset;

    uint8_t atomLen;
    uint8_t prevLen;
    uint8_t nextLen;

    const size_t presentOffset = currentOffset;

    varintSplitFull16Get_(MOFF(ppn->ms, currentOffset), atomLen, atomIndex);
    currentOffset += atomLen;

    varintSplitFull16Get_(MOFF(ppn->ms, currentOffset), prevLen, prevOffset);
    currentOffset += prevLen;

    varintSplitFull16Get_(MOFF(ppn->ms, currentOffset), nextLen, nextOffset);
    currentOffset += nextLen;

    if (presentOffset % SPATIAL_OFFSET != 0) {
        printf("Failed offset: %zu\n", presentOffset);
    }

    printf("{atomIndex {bytes %d} %12zu } ", atomLen, atomIndex);
    printf("{prevOffset {bytes %d} %12zu } ", prevLen, prevOffset);
    printf("{nextOffset {bytes %d} %12zu }", nextLen, nextOffset);
    printf("\n");

    /* Return number of bytes walked over */
    return currentOffset - presentOffset;
}

void ptrPrevNextRepr(ptrPrevNext *ppn) {
    size_t currentOffset = 0;
    printf("{meta {bestFree %zu} {bestFreeLen %" PRId64 "}\n"
           "      {writeFromEnd %zu} {len %zu}\n"
           "      {bytesFree %zd}}\n",
           ppn->ms.bestFree, ppn->ms.bestFreeLen, ppn->ms.writeFromEnd,
           ppn->ms.len, ppn->ms.len - ppn->ms.writeFromEnd);
    for (size_t i = 0; currentOffset < ppn->ms.writeFromEnd; i++) {
        size_t gap = 0;
#if 1
        while (MOFF_ZERO(ppn->ms, currentOffset)) {
            currentOffset++;
            gap++;
        }
#endif

        if (gap) {
            printf("[GAP LEN %zu]\n", gap);
        }

        assert(currentOffset % SPATIAL_OFFSET == 0);

        printf("{offset {%2zu %4zu %4zu} ", i, currentOffset,
               currentOffset / SPATIAL_OFFSET);
        currentOffset += ptrPrevNextReprSingle(ppn, currentOffset);

        alignAlignedOffset(currentOffset);
    }
}

#include "ctest.h"

int ptrPrevNextTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    TEST("create") {
        ptrPrevNext *ppn = ptrPrevNextNew();
        ptrPrevNextFree(ppn);
    }

    TEST("create fill 1") {
        ptrPrevNext *ppn = ptrPrevNextNew();
        ptrPrevNextPtr ptr = ptrPrevNextAdd(ppn, 1, 1, 1);
        assert(ptr == 0);
        ptrPrevNextFree(ppn);
    }

    TEST("create fill 20 same") {
        ptrPrevNext *ppn = ptrPrevNextNew();
        for (uint32_t i = 0; i < 20; i++) {
            ptrPrevNextPtr ptr = ptrPrevNextAdd(ppn, 1, 1, 1);
            assert(ptr == i);
        }
        ptrPrevNextRepr(ppn);
        ptrPrevNextFree(ppn);
    }

    TEST_DESC("create fill %" PRId32 " sequential values", (int32_t)(1 << 22)) {
        ptrPrevNext *ppn = ptrPrevNextNew();
        for (uint32_t i = 1; i <= 1 << 22; i++) {
            const ptrPrevNextPtr ptr = ptrPrevNextAdd(ppn, i, i, i);
            size_t gotA;
            size_t gotB;
            size_t gotC;
            ptrPrevNextRead(ppn, ptr, &gotA, &gotB, &gotC);
            if (gotA != i) {
                ERR("Expected %u but got %zu instead!\n", i, gotA);
            }

            if (gotB != i) {
                ERR("Expected %u but got %zu instead!\n", i, gotB);
            }

            if (gotC != i) {
                ERR("Expected %u but got %zu instead!\n", i, gotC);
            }
        }
        ptrPrevNextFree(ppn);
    }

    TEST("create fill 20 random values") {
        ptrPrevNext *ppn = ptrPrevNextNew();
        srand(time(NULL) ^ getpid());
        for (uint32_t i = 1; i <= 20; i++) {
            const uint32_t a = rand();
            const uint32_t b = rand() >> 7;
            const uint32_t c = rand() >> 12;
            const ptrPrevNextPtr ptr = ptrPrevNextAdd(ppn, a, b, c);

            size_t gotA;
            size_t gotB;
            size_t gotC;
            ptrPrevNextRead(ppn, ptr, &gotA, &gotB, &gotC);
            if (gotA != a) {
                ERR("Expected %u but got %zu instead!\n", a, gotA);
            }

            if (gotB != b) {
                ERR("Expected %u but got %zu instead!\n", b, gotB);
            }

            if (gotC != c) {
                ERR("Expected %u but got %zu instead!\n", c, gotC);
            }
        }
        ptrPrevNextRepr(ppn);
        ptrPrevNextFree(ppn);
    }

    TEST("create fill 20 same update same") {
        ptrPrevNext *ppn = ptrPrevNextNew();
        for (uint32_t i = 1; i <= 20; i++) {
            const ptrPrevNextPtr ptr = ptrPrevNextAdd(ppn, i, i, i);
            const ptrPrevNextPtr ptrUpdated = ptrPrevNextUpdate(ppn, ptr, i, i);
            if (ptr != ptrUpdated) {
                ERR("Expected memory offset to remain the same, but got %zu "
                    "instead of %zu!\n",
                    ptrUpdated, ptr);
            }

            size_t gotA;
            size_t gotB;
            size_t gotC;
            ptrPrevNextRead(ppn, ptr, &gotA, &gotB, &gotC);
            if (gotA != i) {
                ERR("Expected %u but got %zu instead!\n", i, gotA);
            }

            if (gotB != i) {
                ERR("Expected %u but got %zu instead!\n", i, gotB);
            }

            if (gotC != i) {
                ERR("Expected %u but got %zu instead!\n", i, gotC);
            }
        }

        ptrPrevNextFree(ppn);
    }

    TEST("create fill 20 same update grow") {
        ptrPrevNext *ppn = ptrPrevNextNew();
        for (uint32_t i = 1; i <= 20; i++) {
            const ptrPrevNextPtr ptr = ptrPrevNextAdd(ppn, i, i, i);
            const ptrPrevNextPtr ptrUpdated =
                ptrPrevNextUpdate(ppn, ptr, i * i, i * i);

            size_t gotA;
            size_t gotB;
            size_t gotC;
            ptrPrevNextRead(ppn, ptrUpdated, &gotA, &gotB, &gotC);

            if (ptrUpdated != ptr) {
                /* Verify original 'ptr' space is now all 0 */
            }

            if (gotA != i) {
                ERR("Expected %u but got %zu instead!\n", i, gotA);
            }

            if (gotB != i * i) {
                ERR("Expected %u but got %zu instead!\n", i * i, gotB);
            }

            if (gotC != i * i) {
                ERR("Expected %u but got %zu instead!\n", i * i, gotC);
            }
        }

        ptrPrevNextRepr(ppn);
        ptrPrevNextFree(ppn);
    }

    TEST("create fill 20 random update random sequential") {
        ptrPrevNext *ppn = ptrPrevNextNew();
        for (uint32_t i = 1; i <= 20; i++) {
            const ptrPrevNextPtr ptr = ptrPrevNextAdd(ppn, i, i, i);
            const size_t b = rand() * i;
            const size_t c = rand() * i;
            const ptrPrevNextPtr ptrUpdated = ptrPrevNextUpdate(ppn, ptr, b, c);

            size_t gotA;
            size_t gotB;
            size_t gotC;
            ptrPrevNextRead(ppn, ptrUpdated, &gotA, &gotB, &gotC);

            if (ptrUpdated != ptr) {
                /* Verify original 'ptr' space is now all 0 */
            }

            if (gotA != i) {
                ERR("Expected %u but got %zu instead!\n", i, gotA);
            }

            if (gotB != b) {
                ERR("Expected %zu but got %zu instead!\n", b, gotB);
            }

            if (gotC != c) {
                ERR("Expected %zu but got %zu instead!\n", c, gotC);
            }
        }

        ptrPrevNextRepr(ppn);
        ptrPrevNextFree(ppn);
    }

    TEST_FINAL_RESULT;
}
#endif
