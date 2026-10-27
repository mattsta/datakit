#include "ptrPrevNext.h"
#include "../deps/varint/src/varintSplitFull16.h"

static constexpr int SPATIAL_OFFSET = 6;

/* The largest entry: three fields of at most 9 bytes, rounded up to the
 * next slot boundary */
static constexpr int PTR_PREV_NEXT_MAX_SPAN = 30;

/* Free runs are listed by size in slots: class k (k < 5) holds runs of
 * exactly k + 1 slots, the last class every run of 6 slots or more */
static constexpr int PTR_PREV_NEXT_CLASSES = 6;

typedef struct ptrPrevNextFreeList {
    size_t *slots; /* first slot of each listed run (may be stale) */
    size_t count;
    size_t cap;
} ptrPrevNextFreeList;

/* Entries start at byte offsets that are multiples of SPATIAL_OFFSET (a
 * slot) and never reach past the next slot boundary after their last byte
 * (their aligned span, 1 to 5 slots).  Free space is exact, never inferred
 * from zero bytes (an entry of three zeros is itself all zero bytes):
 *   - the tail [writeFromEnd, len), all zero bytes;
 *   - below it, a bitmap with one bit per slot marks every free (zeroed)
 *     slot.  Freeing merges a span with its free neighbours into one run,
 *     and a run that reaches the tail gives itself back to the tail, so no
 *     freed byte is lost.  Runs are listed by size for allocation; a list
 *     entry whose run has since merged or been used is stale and is
 *     dropped when met (the bitmap decides). */
typedef struct memspace {
    void *mem;
    size_t writeFromEnd;
    size_t len;
    uint8_t *freeBits; /* one bit per slot of 'len' */
    ptrPrevNextFreeList free[PTR_PREV_NEXT_CLASSES];
} memspace;

struct ptrPrevNext {
    memspace ms;
};

size_t ptrPrevNextReprSingle(ptrPrevNext *ppn, size_t restoredOffset);

#define MEMSPACE_FITS(ms, size) (((ms).writeFromEnd + (size)) <= (ms).len)
#define MOFF(ms, off) ((uint8_t *)((ms).mem) + (off))

ptrPrevNext *ptrPrevNextNew(void) {
    /* @own: caller; free=ptrPrevNextFree */
    ptrPrevNext *ppn = zcalloc(1, sizeof(*ppn));
    ppn->ms.len = 4096;
    /* @own: container; free=zfree */
    ppn->ms.mem = zcalloc(1, ppn->ms.len);

    /* @own: container; free=zfree */
    ppn->ms.freeBits = zcalloc(1, ppn->ms.len / SPATIAL_OFFSET / 8 + 1);
    ppn->ms.writeFromEnd = 0; /* no entries: everything is tail */
    return ppn;
}

void ptrPrevNextFree(ptrPrevNext *ppn) {
    if (ppn) {
        for (size_t k = 0; k < PTR_PREV_NEXT_CLASSES; k++) {
            zfree(ppn->ms.free[k].slots);
        }

        zfree(ppn->ms.freeBits);
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

        const size_t oldBits = oldLen / SPATIAL_OFFSET / 8 + 1;
        const size_t newBits = ppn->ms.len / SPATIAL_OFFSET / 8 + 1;
        /* @own: container; free=zfree */
        ppn->ms.freeBits = zrealloc(ppn->ms.freeBits, newBits);
        memset(ppn->ms.freeBits + oldBits, 0, newBits - oldBits);
    }
}

static bool ptrPrevNextSlotFree_(const memspace *ms, const size_t slot) {
    return (ms->freeBits[slot >> 3] >> (slot & 7)) & 1;
}

static void ptrPrevNextSlotsMark_(memspace *ms, const size_t first,
                                  const size_t n, const bool free) {
    for (size_t s = first; s < first + n; s++) {
        if (free) {
            ms->freeBits[s >> 3] |= (uint8_t)(1u << (s & 7));
        } else {
            ms->freeBits[s >> 3] &= (uint8_t)~(1u << (s & 7));
        }
    }
}

/* Free slots from 'slot' up to the next used slot or the tail */
static size_t ptrPrevNextRunSlots_(const memspace *ms, const size_t slot) {
    const size_t tailSlot = ms->writeFromEnd / SPATIAL_OFFSET;
    size_t end = slot;
    while (end < tailSlot && ptrPrevNextSlotFree_(ms, end)) {
        end++;
    }

    return end - slot;
}

static size_t ptrPrevNextClass_(const size_t slots) {
    return (slots < PTR_PREV_NEXT_CLASSES ? slots : PTR_PREV_NEXT_CLASSES) - 1;
}

/* List the free run starting at 'slot' (its slots already marked).  OOM
 * is fail-fast. */
static void ptrPrevNextListRun_(memspace *ms, const size_t slot,
                                const size_t slots) {
    ptrPrevNextFreeList *list = &ms->free[ptrPrevNextClass_(slots)];
    if (list->count == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 8;
        /* @own: container; free=zfree */
        list->slots = zrealloc(list->slots, list->cap * sizeof(*list->slots));
    }

    list->slots[list->count++] = slot;
}

/* Return the zeroed aligned span [at, end) to free space: merge it with
 * its free neighbours into one run, which goes back to the tail when it
 * reaches it, and onto its size's list otherwise. */
static void ptrPrevNextFreeSpan_(ptrPrevNext *ppn, const size_t at,
                                 const size_t end) {
    memspace *ms = &ppn->ms;
    size_t first = at / SPATIAL_OFFSET;
    const size_t last = end / SPATIAL_OFFSET;
    ptrPrevNextSlotsMark_(ms, first, last - first, true);
    while (first > 0 && ptrPrevNextSlotFree_(ms, first - 1)) {
        first--;
    }

    const size_t slots = (last - first) + ptrPrevNextRunSlots_(ms, last);
    if ((first + slots) * SPATIAL_OFFSET == ms->writeFromEnd) {
        ptrPrevNextSlotsMark_(ms, first, slots, false);
        ms->writeFromEnd = first * SPATIAL_OFFSET;
        return;
    }

    ptrPrevNextListRun_(ms, first, slots);
}

/* A free run of at least 'size' bytes (smallest class first), taken and
 * marked used; what is left of a longer run is listed again.  SIZE_MAX
 * when no free run is long enough. */
static size_t ptrPrevNextFreeTake_(ptrPrevNext *ppn, const size_t size) {
    memspace *ms = &ppn->ms;
    const size_t need = size / SPATIAL_OFFSET;
    for (size_t k = ptrPrevNextClass_(need); k < PTR_PREV_NEXT_CLASSES; k++) {
        ptrPrevNextFreeList *list = &ms->free[k];
        for (size_t e = list->count; e > 0; e--) {
            const size_t slot = list->slots[e - 1];
            const bool starts =
                ptrPrevNextSlotFree_(ms, slot) &&
                (slot == 0 || !ptrPrevNextSlotFree_(ms, slot - 1));
            const size_t slots = starts ? ptrPrevNextRunSlots_(ms, slot) : 0;
            if (slots == 0 || ptrPrevNextClass_(slots) != k) {
                /* Stale: merged, absorbed by the tail, or used */
                list->slots[e - 1] = list->slots[--list->count];
                continue;
            }

            if (slots < need) {
                continue; /* a large-class run too short for this entry */
            }

            list->slots[e - 1] = list->slots[--list->count];
            ptrPrevNextSlotsMark_(ms, slot, need, false);
            if (slots > need) {
                /* The rest stays one maximal run: its right neighbour was
                 * already used */
                ptrPrevNextListRun_(ms, slot + need, slots - need);
            }

            return slot * SPATIAL_OFFSET;
        }
    }

    return SIZE_MAX;
}

/* Bytes an entry of these values takes */
static size_t ptrPrevNextEncodedLen_(const size_t atomIndex,
                                     const size_t prevOffset,
                                     const size_t nextOffset) {
    uint8_t a;
    uint8_t p;
    uint8_t n;
    varintSplitFull16Length_(a, atomIndex);
    varintSplitFull16Length_(p, prevOffset);
    varintSplitFull16Length_(n, nextOffset);
    return (size_t)a + p + n;
}

ptrPrevNextPtr ptrPrevNextAdd(ptrPrevNext *ppn, const size_t atomIndex,
                              const size_t prevOffset,
                              const size_t nextOffset) {
    /* Step 1: a freed span that fits */
    const size_t span = ptrPrevNextAlignUp_(
        ptrPrevNextEncodedLen_(atomIndex, prevOffset, nextOffset));
    const size_t freeAt = ptrPrevNextFreeTake_(ppn, span);
    if (freeAt != SIZE_MAX) {
        [[maybe_unused]] const size_t written = ptrPrevNextPopulateAll(
            atomIndex, prevOffset, nextOffset, MOFF(ppn->ms, freeAt), span);
        assert(written);
        return freeAt / SPATIAL_OFFSET;
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

        const size_t newSpanEnd = ptrPrevNextAlignUp_(newEnd);
        if (isLast) {
            ppn->ms.writeFromEnd = newSpanEnd;
        } else if (newSpanEnd < spanEnd) {
            /* The slots the entry no longer reaches are free (zero) */
            ptrPrevNextFreeSpan_(ppn, newSpanEnd, spanEnd);
        }

        return memOffset;
    }

    /* Too big for its place: free it and add it again */
    [[maybe_unused]] uint8_t atomLen;
    size_t atomRef;
    varintSplitFull16Get_(MOFF(ppn->ms, at), atomLen, atomRef);
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
    printf("{meta {writeFromEnd %zu} {len %zu} {bytesFree %zu}}\n",
           ppn->ms.writeFromEnd, ppn->ms.len,
           ppn->ms.len - ppn->ms.writeFromEnd);
    size_t currentOffset = 0;
    for (size_t i = 0; currentOffset < ppn->ms.writeFromEnd; i++) {
        /* Free slots are known exactly; an entry may be all zero bytes */
        const size_t gap =
            ptrPrevNextRunSlots_(&ppn->ms, currentOffset / SPATIAL_OFFSET) *
            SPATIAL_OFFSET;
        currentOffset += gap;
        if (gap) {
            printf("[FREE %zu]\n", gap);
        }

        if (currentOffset >= ppn->ms.writeFromEnd) {
            break;
        }

        printf("{offset {%2zu %4zu %4zu} ", i, currentOffset,
               currentOffset / SPATIAL_OFFSET);
        currentOffset += ptrPrevNextReprSingle(ppn, currentOffset);
        alignAlignedOffset(currentOffset);
    }
}

#include "ctest.h"

/* A value from every encoding class: zero, 14-bit, 22-bit, 30-bit and
 * the variable-width class up to SIZE_MAX. */
static size_t ppnSoakValue_(uint64_t *s) {
    const uint64_t r = dkSplitmix64Next(s);
    switch (dkSplitmix64Next(s) % 6) {
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

int ptrPrevNextTest(int, char *[]) {
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
        srand(time(nullptr) ^ getpid());
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
            ppnSoakEntry e[SLOTS] = {};
            for (size_t op = 0; op < OPS && !err; op++) {
                ppnSoakEntry *const pick = &e[dkSplitmix64Next(&s) % SLOTS];
                const size_t b = ppnSoakValue_(&s);
                const size_t c = ppnSoakValue_(&s);
                if (!pick->live) {
                    const size_t a = ppnSoakValue_(&s);
                    pick->live = true;
                    pick->ptr = ptrPrevNextAdd(ppn, a, b, c);
                    pick->vals[0] = a;
                    pick->vals[1] = b;
                    pick->vals[2] = c;
                } else if (dkSplitmix64Next(&s) % 3 != 0) {
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

    TEST("churn: freed spans are all reused; the arena stops growing") {
        enum { ENTRIES = 600, ROUNDS = 8 };
        uint64_t s = 99;
        ptrPrevNext *ppn = ptrPrevNextNew();
        ptrPrevNextPtr ptrs[ENTRIES];
        size_t vals[ENTRIES][3];
        size_t lenAfterFirst = 0;
        for (size_t round = 0; round < ROUNDS && !err; round++) {
            /* Fill with entries of every size, update some (moving or
             * shrinking them), then release all in random order */
            for (size_t e = 0; e < ENTRIES; e++) {
                for (size_t f = 0; f < 3; f++) {
                    vals[e][f] = ppnSoakValue_(&s);
                }

                ptrs[e] =
                    ptrPrevNextAdd(ppn, vals[e][0], vals[e][1], vals[e][2]);
            }

            for (size_t e = 0; e < ENTRIES; e += 3) {
                vals[e][1] = ppnSoakValue_(&s);
                vals[e][2] = ppnSoakValue_(&s);
                ptrs[e] =
                    ptrPrevNextUpdate(ppn, ptrs[e], vals[e][1], vals[e][2]);
            }

            for (size_t e = 0; e < ENTRIES; e++) {
                size_t got[3];
                ptrPrevNextRead(ppn, ptrs[e], &got[0], &got[1], &got[2]);
                if (memcmp(got, vals[e], sizeof(got)) != 0) {
                    ERR("round %zu: entry %zu reads back wrong", round, e);
                    break;
                }
            }

            for (size_t e = ENTRIES; e > 1; e--) {
                const size_t k = (size_t)(dkSplitmix64Next(&s) % e);
                ptrPrevNextRelease(ppn, ptrs[k]);
                ptrs[k] = ptrs[e - 1];
                memcpy(vals[k], vals[e - 1], sizeof(vals[k]));
            }

            ptrPrevNextRelease(ppn, ptrs[0]);
            if (round == 0) {
                lenAfterFirst = ppn->ms.len;
            } else if (ppn->ms.len > lenAfterFirst) {
                ERR("round %zu: arena grew from %zu to %zu bytes although "
                    "everything was freed",
                    round, lenAfterFirst, ppn->ms.len);
            }
        }

        ptrPrevNextFree(ppn);
    }

    TEST_FINAL_RESULT;
}
#endif
