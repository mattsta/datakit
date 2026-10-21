#include "ptrPrevNext.h"
#include "../deps/varint/src/varintSplitFull16.h"

/* Entries start at byte offsets that are multiples of SPATIAL_OFFSET and
 * never reach past the next such offset after their last byte (their
 * aligned span).  Free space is exact, never inferred from zero bytes (an
 * entry of three zeros is itself all zero bytes):
 *   - the tail [writeFromEnd, len), all zero bytes;
 *   - at most one hole [bestFree, bestFree + bestFreeLen) below it, all
 *     zero bytes, aligned at both ends (bestFreeLen 0: no hole). */
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
    /* @own: caller; free=ptrPrevNextFree */
    ptrPrevNext *ppn = zcalloc(1, sizeof(*ppn));
    ppn->ms.len = 4096;
    /* @own: container; free=zfree */
    ppn->ms.mem = zcalloc(1, ppn->ms.len);

    ppn->ms.bestFree = 0;
    ppn->ms.bestFreeLen = 0; /* no hole: everything is tail */
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

/* The largest entry: three fields of at most 9 bytes, rounded up to the
 * next slot boundary */
#define PTR_PREV_NEXT_MAX_SPAN 30

/* 'off' rounded up to a slot boundary */
static size_t ptrPrevNextAlignUp_(size_t off) {
    alignAlignedOffset(off);
    return off;
}

/* Byte length of the entry at byte offset 'at'; '*lenAtom' gets the
 * length of its first field. */
static size_t ptrPrevNextEntryLen_(const ptrPrevNext *ppn, const size_t at,
                                   uint8_t *lenAtom) {
    const uint8_t a = varintSplitFull16GetLenQuick_(MOFF(ppn->ms, at));
    const uint8_t p = varintSplitFull16GetLenQuick_(MOFF(ppn->ms, at + a));
    const uint8_t n = varintSplitFull16GetLenQuick_(MOFF(ppn->ms, at + a + p));
    *lenAtom = a;
    return (size_t)a + p + n;
}

/* Grow until the tail holds the largest entry; new bytes are zero, as the
 * tail must be.  OOM is fail-fast. */
static void ptrPrevNextEnsureTail_(ptrPrevNext *ppn) {
    while (!MEMSPACE_FITS(ppn->ms, PTR_PREV_NEXT_MAX_SPAN)) {
        const size_t oldLen = ppn->ms.len;
        ppn->ms.len = jebufSizeAllocation(ppn->ms.len * 1.5);
        /* @own: container; free=zfree */
        ppn->ms.mem = zrealloc(ppn->ms.mem, ppn->ms.len);
        memset(MOFF(ppn->ms, oldLen), 0, ppn->ms.len - oldLen);
    }
}

/* Return the zeroed aligned span [at, end) to free space: it shrinks the
 * tail when it ends there, else joins or replaces the hole (the larger
 * hole is kept, the lower on a tie). */
static void ptrPrevNextFreeSpan_(ptrPrevNext *ppn, const size_t at,
                                 const size_t end) {
    memspace *ms = &ppn->ms;
    const int64_t len = (int64_t)(end - at);
    const bool haveHole = ms->bestFreeLen > 0;
    if (end == ms->writeFromEnd) {
        ms->writeFromEnd = at;
        if (haveHole && ms->bestFree + (size_t)ms->bestFreeLen == at) {
            ms->writeFromEnd = ms->bestFree;
            ms->bestFree = 0;
            ms->bestFreeLen = 0;
        }
    } else if (haveHole && ms->bestFree + (size_t)ms->bestFreeLen == at) {
        ms->bestFreeLen += len;
    } else if (haveHole && end == ms->bestFree) {
        ms->bestFree = at;
        ms->bestFreeLen += len;
    } else if (len > ms->bestFreeLen ||
               (len == ms->bestFreeLen && at < ms->bestFree)) {
        ms->bestFree = at;
        ms->bestFreeLen = len;
    }
}

ptrPrevNextPtr ptrPrevNextAdd(ptrPrevNext *ppn, const size_t atomIndex,
                              const size_t prevOffset,
                              const size_t nextOffset) {
    /* Step 1: the hole, when the entry fits there */
    if (ppn->ms.bestFreeLen > 0) {
        const size_t at = ppn->ms.bestFree;
        const size_t written = ptrPrevNextPopulateAll(
            atomIndex, prevOffset, nextOffset, MOFF(ppn->ms, at),
            (size_t)ppn->ms.bestFreeLen);
        if (written) {
            /* The hole is aligned at both ends, so this stays inside it */
            const size_t used = ptrPrevNextAlignUp_(written);
            ppn->ms.bestFree += used;
            ppn->ms.bestFreeLen -= (int64_t)used;
            if (ppn->ms.bestFreeLen == 0) {
                ppn->ms.bestFree = 0;
            }

            return at / SPATIAL_OFFSET;
        }
    }

    /* Step 2: the tail */
    ptrPrevNextEnsureTail_(ppn);
    const size_t at = ppn->ms.writeFromEnd;
    assert(at % SPATIAL_OFFSET == 0);
    const size_t written = ptrPrevNextPopulateAll(
        atomIndex, prevOffset, nextOffset, MOFF(ppn->ms, at), ppn->ms.len - at);
    assert(written);
    ppn->ms.writeFromEnd = ptrPrevNextAlignUp_(at + written);
    return at / SPATIAL_OFFSET;
}

ptrPrevNextPtr ptrPrevNextUpdate(ptrPrevNext *ppn,
                                 const ptrPrevNextPtr memOffset,
                                 const size_t prevOffset,
                                 const size_t nextOffset) {
    const size_t at = memOffset * SPATIAL_OFFSET;
    uint8_t lenAtom;
    const size_t oldEnd = at + ptrPrevNextEntryLen_(ppn, at, &lenAtom);
    const size_t spanEnd = ptrPrevNextAlignUp_(oldEnd);
    const size_t fieldsAt = at + lenAtom;

    /* In place: within the entry's aligned span, or, for the last entry,
     * anywhere up to the last slot boundary of the buffer (so the tail
     * still starts on a boundary inside it). */
    const bool isLast = spanEnd == ppn->ms.writeFromEnd;
    const size_t limit =
        isLast ? ppn->ms.len - ppn->ms.len % SPATIAL_OFFSET : spanEnd;
    const size_t written = ptrPrevNextPopulatePrevNext(
        prevOffset, nextOffset, MOFF(ppn->ms, fieldsAt), limit - fieldsAt);
    if (written) {
        const size_t newEnd = fieldsAt + written;
        if (newEnd < oldEnd) {
            /* Free space and padding stay zero */
            memset(MOFF(ppn->ms, newEnd), 0, oldEnd - newEnd);
        }

        if (isLast) {
            ppn->ms.writeFromEnd = ptrPrevNextAlignUp_(newEnd);
        }

        return memOffset;
    }

    /* Too big for its place: free it and add it again */
    uint8_t atomLen;
    size_t atomRef;
    varintSplitFull16Get_(MOFF(ppn->ms, at), atomLen, atomRef);
    (void)atomLen;
    memset(MOFF(ppn->ms, at), 0, spanEnd - at);
    ptrPrevNextFreeSpan_(ppn, at, spanEnd);
    return ptrPrevNextAdd(ppn, atomRef, prevOffset, nextOffset);
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
    const size_t at = memOffset * SPATIAL_OFFSET;
    uint8_t lenAtom;
    const size_t end =
        ptrPrevNextAlignUp_(at + ptrPrevNextEntryLen_(ppn, at, &lenAtom));
    memset(MOFF(ppn->ms, at), 0, end - at);
    ptrPrevNextFreeSpan_(ppn, at, end);
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

/* splitmix64 */
static uint64_t ppnSoakRand_(uint64_t *state) {
    uint64_t z = (*state += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

/* A value from every encoding class: zero, 14-bit, 22-bit, 30-bit and
 * the variable-width class up to SIZE_MAX. */
static size_t ppnSoakValue_(uint64_t *s) {
    const uint64_t r = ppnSoakRand_(s);
    switch (ppnSoakRand_(s) % 6) {
    case 0:
        return 0;
    case 1:
        return (size_t)(r % 0x4000);
    case 2:
        return (size_t)(r % 0x400000);
    case 3:
        return (size_t)(r % 0x40000000);
    case 4:
        return SIZE_MAX - (size_t)(r % 4);
    default:
        return (size_t)r;
    }
}

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

    TEST("soak: every live entry reads back after adds, updates, releases") {
        enum { SLOTS = 96, OPS = 3000 };
        typedef struct ppnSoakEntry {
            bool live;
            ptrPrevNextPtr ptr;
            size_t vals[3];
        } ppnSoakEntry;

        for (uint64_t seed = 1; seed <= 6 && !err; seed++) {
            uint64_t s = seed;
            ptrPrevNext *ppn = ptrPrevNextNew();
            ppnSoakEntry e[SLOTS] = {{0}};
            for (size_t op = 0; op < OPS && !err; op++) {
                ppnSoakEntry *const pick = &e[ppnSoakRand_(&s) % SLOTS];
                const size_t b = ppnSoakValue_(&s);
                const size_t c = ppnSoakValue_(&s);
                if (!pick->live) {
                    const size_t a = ppnSoakValue_(&s);
                    pick->live = true;
                    pick->ptr = ptrPrevNextAdd(ppn, a, b, c);
                    pick->vals[0] = a;
                    pick->vals[1] = b;
                    pick->vals[2] = c;
                } else if (ppnSoakRand_(&s) % 3 != 0) {
                    pick->ptr = ptrPrevNextUpdate(ppn, pick->ptr, b, c);
                    pick->vals[1] = b;
                    pick->vals[2] = c;
                } else {
                    ptrPrevNextRelease(ppn, pick->ptr);
                    pick->live = false;
                }

                for (size_t j = 0; j < SLOTS; j++) {
                    if (!e[j].live) {
                        continue;
                    }

                    size_t got[3];
                    ptrPrevNextRead(ppn, e[j].ptr, &got[0], &got[1], &got[2]);
                    if (memcmp(got, e[j].vals, sizeof(got)) != 0) {
                        ERR("seed %" PRIu64 " op %zu: entry %zu at ptr %zu "
                            "reads {%zu %zu %zu}, want {%zu %zu %zu}",
                            seed, op, j, e[j].ptr, got[0], got[1], got[2],
                            e[j].vals[0], e[j].vals[1], e[j].vals[2]);
                        break;
                    }

                    for (size_t k = j + 1; op % 64 == 0 && k < SLOTS; k++) {
                        if (e[k].live && e[k].ptr == e[j].ptr) {
                            ERR("seed %" PRIu64 " op %zu: entries %zu and %zu "
                                "share ptr %zu",
                                seed, op, j, k, e[j].ptr);
                        }
                    }
                }
            }

            ptrPrevNextFree(ppn);
        }
    }

    TEST_FINAL_RESULT;
}
#endif
